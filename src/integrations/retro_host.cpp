#include "openxmb/integrations/desktop.hpp"
#include "openxmb/integrations/libretro_abi.hpp"

#include <SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <csignal>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using namespace openxmb::integrations;
namespace retro = openxmb::integrations::retro;
volatile std::sig_atomic_t termination_requested = 0;
void request_termination(int) { termination_requested = 1; }
struct DynamicCore {
#ifdef _WIN32
  HMODULE handle{};
#else
  void* handle{};
#endif
  explicit DynamicCore(const std::filesystem::path& path) {
#ifdef _WIN32
    handle = LoadLibraryW(path.wstring().c_str());
    if (!handle) throw std::runtime_error("Cannot load core library");
#else
    handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) throw std::runtime_error(std::string{"Cannot load core: "} + dlerror());
#endif
  }
  ~DynamicCore() {
#ifdef _WIN32
    if (handle) FreeLibrary(handle);
#else
    if (handle) dlclose(handle);
#endif
  }
  template<class T> T symbol(const char* name) const {
#ifdef _WIN32
    const auto address = GetProcAddress(handle, name);
#else
    const auto address = dlsym(handle, name);
#endif
    if (!address) throw std::runtime_error(std::string{"Core does not implement "} + name);
    T function{};
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
  }
};
struct CoreApi {
  void (*set_environment)(retro::environment_t);
  void (*set_video_refresh)(retro::video_refresh_t);
  void (*set_audio_sample)(retro::audio_sample_t);
  void (*set_audio_sample_batch)(retro::audio_sample_batch_t);
  void (*set_input_poll)(retro::input_poll_t);
  void (*set_input_state)(retro::input_state_t);
  unsigned (*api_version)();
  void (*get_system_info)(retro::system_info*);
  void (*get_system_av_info)(retro::system_av_info*);
  void (*init)();
  void (*deinit)();
  bool (*load_game)(const retro::game_info*);
  void (*unload_game)();
  void (*run)();
  void (*reset)();
  void (*set_controller_port_device)(unsigned, unsigned);
  std::size_t (*serialize_size)();
  bool (*serialize)(void*, std::size_t);
  bool (*unserialize)(const void*, std::size_t);
  void* (*get_memory_data)(unsigned);
  std::size_t (*get_memory_size)(unsigned);
  explicit CoreApi(const DynamicCore& core) :
#define CORE_FUNCTION(name) name(core.symbol<decltype(name)>("retro_" #name))
    CORE_FUNCTION(set_environment), CORE_FUNCTION(set_video_refresh),
    CORE_FUNCTION(set_audio_sample), CORE_FUNCTION(set_audio_sample_batch),
    CORE_FUNCTION(set_input_poll), CORE_FUNCTION(set_input_state), CORE_FUNCTION(api_version),
    CORE_FUNCTION(get_system_info), CORE_FUNCTION(get_system_av_info), CORE_FUNCTION(init), CORE_FUNCTION(deinit),
    CORE_FUNCTION(load_game), CORE_FUNCTION(unload_game), CORE_FUNCTION(run), CORE_FUNCTION(reset),
    CORE_FUNCTION(set_controller_port_device), CORE_FUNCTION(serialize_size), CORE_FUNCTION(serialize),
    CORE_FUNCTION(unserialize), CORE_FUNCTION(get_memory_data), CORE_FUNCTION(get_memory_size)
#undef CORE_FUNCTION
  {}
};
struct Host {
  SDL_Window* window{};
  SDL_Renderer* renderer{};
  SDL_Texture* texture{};
  SDL_AudioDeviceID audio{};
  std::array<SDL_GameController*, 4> controllers{};
  unsigned pixel_format{};
  unsigned texture_width{}, texture_height{}, texture_format{std::numeric_limits<unsigned>::max()};
  bool support_no_game{};
  bool hardware_requested{};
  bool requested_shutdown{};
  std::string fatal_error;
  std::string system_directory;
  std::string save_directory;
  std::map<std::string, std::string> variables;
  float aspect{};
  unsigned sample_rate{};
  std::uint64_t video_callbacks{}, audio_frames{};
  static Host* active;
  ~Host() {
    for (auto* controller : controllers) if (controller) SDL_GameControllerClose(controller);
    if (audio) SDL_CloseAudioDevice(audio);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    active = nullptr;
  }
  static void log(int, const char* format, ...) {
    va_list args; va_start(args, format); std::vfprintf(stderr, format, args); va_end(args);
  }
  static bool environment(unsigned command, void* data) {
    auto& self = *active;
    switch (command & 0xffffU) {
    case 2: *static_cast<bool*>(data) = false; return true; // overscan
    case 3: *static_cast<bool*>(data) = true; return true; // duplicate frames
    case 6: if (data) std::cerr << static_cast<retro::message*>(data)->msg << '\n'; return true;
    case 7: self.requested_shutdown = true; return true;
    case 9: *static_cast<const char**>(data) = self.system_directory.c_str(); return true;
    case 10: {
      const auto format = *static_cast<unsigned*>(data);
      if (format > 2) return false;
      self.pixel_format = format; return true;
    }
    case 11: return true; // input descriptions
    case 14: self.hardware_requested = true; return false;
    case 15: {
      auto& variable = *static_cast<retro::variable*>(data);
      const auto found = self.variables.find(variable.key ? variable.key : "");
      variable.value = found == self.variables.end() ? nullptr : found->second.c_str();
      return variable.value != nullptr;
    }
    case 16: {
      for (auto* variable = static_cast<retro::variable*>(data); variable && variable->key; ++variable) {
        std::string value = variable->value ? variable->value : "";
        const auto delimiter = value.find(';');
        if (delimiter != std::string::npos) value.erase(0, delimiter + 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        if (const auto end = value.find('|'); end != std::string::npos) value.resize(end);
        self.variables[variable->key] = std::move(value);
      }
      return true;
    }
    case 17: *static_cast<bool*>(data) = false; return true;
    case 18: self.support_no_game = *static_cast<bool*>(data); return true;
    case 27: static_cast<retro::log_callback*>(data)->log = &log; return true;
    case 30: *static_cast<const char**>(data) = self.system_directory.c_str(); return true;
    case 31: *static_cast<const char**>(data) = self.save_directory.c_str(); return true;
    case 35: return true; // controller information
    case 37: {
      const auto& geometry = *static_cast<retro::game_geometry*>(data);
      self.aspect = geometry.aspect_ratio; return true;
    }
    case 39: *static_cast<unsigned*>(data) = 0; return true; // English
    case 47: *static_cast<int*>(data) = 3; return true; // audio+video enabled
    case 51: return true; // joypad bitmasks
    case 52: *static_cast<unsigned*>(data) = 0; return true; // legacy core options
    default: return false;
    }
  }
  static void video(const void* pixels, unsigned width, unsigned height, std::size_t pitch) {
    auto& self = *active;
    ++self.video_callbacks;
    if (pixels == reinterpret_cast<const void*>(static_cast<std::uintptr_t>(-1))) {
      self.fatal_error = "Hardware-framebuffer cores are not supported by this software host";
      return;
    }
    if (!pixels) return;
    if (width == 0 || height == 0 || width > 8192 || height > 8192 || pitch > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      self.fatal_error = "The core supplied an invalid video frame"; return;
    }
    const auto minimum_pitch = static_cast<std::size_t>(width) * (self.pixel_format == 1 ? 4U : 2U);
    if (pitch < minimum_pitch) { self.fatal_error = "The core supplied a truncated video row"; return; }
    const auto format = self.pixel_format == 1 ? SDL_PIXELFORMAT_ARGB8888 : self.pixel_format == 2 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_ARGB1555;
    if (!self.texture || width != self.texture_width || height != self.texture_height || self.texture_format != self.pixel_format) {
      if (self.texture) SDL_DestroyTexture(self.texture);
      self.texture = SDL_CreateTexture(self.renderer, format, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(width), static_cast<int>(height));
      self.texture_width = width; self.texture_height = height; self.texture_format = self.pixel_format;
      if (!self.texture) { self.fatal_error = SDL_GetError(); return; }
      SDL_SetTextureBlendMode(self.texture, SDL_BLENDMODE_NONE);
    }
    if (SDL_UpdateTexture(self.texture, nullptr, pixels, static_cast<int>(pitch)) != 0) self.fatal_error = SDL_GetError();
  }
  static std::size_t audio_batch(const std::int16_t* samples, std::size_t frames) {
    auto& self = *active;
    self.audio_frames += frames;
    if (!self.audio || !samples || frames > std::numeric_limits<Uint32>::max() / 4) return frames;
    // A suspended window/device must not retain seconds of stale audio.
    if (SDL_GetQueuedAudioSize(self.audio) > self.sample_rate * 4U / 5U) SDL_ClearQueuedAudio(self.audio);
    if (SDL_QueueAudio(self.audio, samples, static_cast<Uint32>(frames * 4)) != 0) self.fatal_error = SDL_GetError();
    return frames;
  }
  static void audio_sample(std::int16_t left, std::int16_t right) { const std::array samples{left, right}; audio_batch(samples.data(), 1); }
  static void input_poll() { SDL_PumpEvents(); }
  static std::int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    auto& self = *active;
    if (port >= self.controllers.size()) return 0;
    const auto* keyboard = SDL_GetKeyboardState(nullptr);
    auto* controller = self.controllers[port];
    device &= 0xffU;
    if (device == retro::device_keyboard) {
      const auto scan = SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(id));
      return scan != SDL_SCANCODE_UNKNOWN && keyboard[scan] ? 1 : 0;
    }
    if (device == retro::device_analog) {
      if (!controller || index > 1 || id > 1) return 0;
      const auto axis = static_cast<SDL_GameControllerAxis>(index * 2 + id);
      return SDL_GameControllerGetAxis(controller, axis);
    }
    if (device != retro::device_joypad) return 0;
    static constexpr std::array keys{SDL_SCANCODE_Z, SDL_SCANCODE_A, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN,
      SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_X, SDL_SCANCODE_S,
      SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_1, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_6};
    static constexpr std::array buttons{SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_BACK,
      SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
      SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_Y,
      SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_INVALID,
      SDL_CONTROLLER_BUTTON_INVALID, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK};
    const auto pressed = [&](unsigned button) {
      if (button >= keys.size()) return false;
      bool down = port == 0 && keyboard[keys[button]];
      if (controller) {
        if (button == 12 || button == 13) down = down || SDL_GameControllerGetAxis(controller,
          button == 12 ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384;
        else if (buttons[button] != SDL_CONTROLLER_BUTTON_INVALID) down = down || SDL_GameControllerGetButton(controller, buttons[button]);
      }
      return down;
    };
    if (id == retro::joypad_mask) {
      unsigned mask{}; for (unsigned button = 0; button < 16; ++button) if (pressed(button)) mask |= 1U << button;
      return static_cast<std::int16_t>(mask);
    }
    return pressed(id) ? 1 : 0;
  }
  void refresh_controllers() {
    for (auto*& controller : controllers) { if (controller) SDL_GameControllerClose(controller); controller = nullptr; }
    unsigned port{};
    for (int index = 0; index < SDL_NumJoysticks() && port < controllers.size(); ++index)
      if (SDL_IsGameController(index)) controllers[port++] = SDL_GameControllerOpen(index);
  }
  void screenshot(const std::filesystem::path& path) {
    int width{}, height{}; SDL_GetRendererOutputSize(renderer, &width, &height);
    auto* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) throw std::runtime_error(SDL_GetError());
    const auto read = SDL_RenderReadPixels(renderer, nullptr, surface->format->format, surface->pixels, surface->pitch);
    const auto saved = read == 0 ? SDL_SaveBMP(surface, path.string().c_str()) : -1;
    SDL_FreeSurface(surface);
    if (saved != 0) throw std::runtime_error(std::string{"Cannot save screenshot: "} + SDL_GetError());
    std::cout << "screenshot_saved=" << path.string() << '\n';
  }
  void present() {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderClear(renderer);
    if (texture) {
      int width{}, height{}; SDL_GetRendererOutputSize(renderer, &width, &height);
      const auto ratio = aspect > 0 ? aspect : static_cast<float>(texture_width) / static_cast<float>(texture_height);
      SDL_Rect target{0, 0, width, static_cast<int>(static_cast<float>(width) / ratio)};
      if (target.h > height) { target.h = height; target.w = static_cast<int>(static_cast<float>(height) * ratio); }
      target.x = (width - target.w) / 2; target.y = (height - target.h) / 2;
      SDL_RenderCopy(renderer, texture, nullptr, &target);
    }
    SDL_RenderPresent(renderer);
  }
};
Host* Host::active{};
std::vector<char> read_file(const std::filesystem::path& path, std::size_t limit = 256U * 1024U * 1024U) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("Cannot open " + path.string());
  const auto end = input.tellg();
  if (end < 0 || static_cast<std::uintmax_t>(end) > limit) throw std::runtime_error("File exceeds the supported size: " + path.string());
  std::vector<char> bytes(static_cast<std::size_t>(end)); input.seekg(0);
  if (!bytes.empty() && !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("Cannot read " + path.string());
  return bytes;
}
void write_file(const std::filesystem::path& path, const void* data, std::size_t bytes) {
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (bytes) output.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
  output.close();
  if (!output) throw std::runtime_error("Cannot save " + path.string());
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
#ifdef _WIN32
  if (error) { std::filesystem::remove(path, error); std::filesystem::rename(temporary, path, error); }
#endif
  if (error) throw std::runtime_error("Cannot replace save file: " + error.message());
}
std::string save_stem(const std::filesystem::path& core, const std::filesystem::path& content) {
  const auto source = std::filesystem::absolute(core).string() + ":" + (content.empty() ? "no-content" : std::filesystem::absolute(content).string());
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char ch : source) { hash ^= ch; hash *= 1099511628211ULL; }
  auto name = (content.empty() ? core.stem() : content.stem()).string();
  for (auto& ch : name) if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_') ch = '_';
  return name + "-" + std::to_string(hash);
}
void save_state(CoreApi& core, const std::filesystem::path& path) {
  const auto size = core.serialize_size();
  if (size == 0) throw std::runtime_error("This core does not support save states");
  if (size > 64U * 1024U * 1024U) throw std::runtime_error("The core requested an excessive save-state size");
  std::vector<char> bytes(size);
  if (!core.serialize(bytes.data(), bytes.size())) throw std::runtime_error("Core failed to serialize state");
  write_file(path, bytes.data(), bytes.size());
  std::cout << "state_saved=" << path.string() << '\n';
}
void load_state(CoreApi& core, const std::filesystem::path& path) {
  auto bytes = read_file(path, 64U * 1024U * 1024U);
  if (!core.unserialize(bytes.data(), bytes.size())) throw std::runtime_error("Save state is incompatible with this core/content");
  std::cout << "state_loaded=" << path.string() << '\n';
}
}
int main(int argc, char** argv) {
  try {
    std::filesystem::path core_path, content_path, state_directory, load_path, screenshot_path;
    std::uint64_t frame_limit{};
    bool headless{}, save_on_exit{};
    for (int index = 1; index < argc; ++index) {
      const std::string_view option = argv[index];
      if (option == "--headless") headless = true;
      else if (option == "--save-on-exit") save_on_exit = true;
      else if (option == "--help") { std::cout << "openxmb-retro --core LIBRARY [--content FILE] [--state-directory DIR] [--load-state FILE] [--frames N] [--headless] [--save-on-exit]\nKeyboard: arrows, Z/X/A/S, Shift/Enter, Q/E. Escape quits; Space pauses; F2 resets; F5 saves; F8 loads; F12 screenshots. Controller: normal pad inputs; Guide quits. Hold Back with X pause, Y reset, L save, R load, A screenshot, Start quit.\n"; return 0; }
      else {
        if (index + 1 >= argc) throw std::runtime_error("Missing value for " + std::string{option});
        const std::string value = argv[++index];
        if (option == "--core") core_path = value;
        else if (option == "--content") content_path = value;
        else if (option == "--state-directory") state_directory = value;
        else if (option == "--load-state") load_path = value;
        else if (option == "--screenshot") screenshot_path = value;
        else if (option == "--frames") frame_limit = std::stoull(value);
        else throw std::runtime_error("Unknown option: " + std::string{option});
      }
    }
    std::signal(SIGTERM, request_termination);
    std::signal(SIGINT, request_termination);
    if (core_path.empty()) throw std::runtime_error("Select a libretro core with --core");
    if (state_directory.empty()) state_directory = user_data_directory() / "retro";
    std::filesystem::create_directories(state_directory / "system");
    if (headless) {
      SDL_setenv("SDL_VIDEODRIVER", "dummy", 0); SDL_setenv("SDL_AUDIODRIVER", "dummy", 0);
    }
    Host host;
    Host::active = &host;
    host.system_directory = (state_directory / "system").string();
    host.save_directory = state_directory.string();
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) throw std::runtime_error(SDL_GetError());
    host.window = SDL_CreateWindow("OpenXMB — Emulation", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720, SDL_WINDOW_RESIZABLE | (headless ? SDL_WINDOW_HIDDEN : 0));
    if (!host.window) throw std::runtime_error(SDL_GetError());
    host.renderer = SDL_CreateRenderer(host.window, -1, headless ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED);
    if (!host.renderer) host.renderer = SDL_CreateRenderer(host.window, -1, SDL_RENDERER_SOFTWARE);
    if (!host.renderer) throw std::runtime_error(SDL_GetError());
    host.refresh_controllers();
    DynamicCore library{core_path}; CoreApi core{library};
    if (core.api_version() != retro::api_version) throw std::runtime_error("Unsupported libretro API version");
    core.set_environment(&Host::environment); core.set_video_refresh(&Host::video);
    core.set_audio_sample(&Host::audio_sample); core.set_audio_sample_batch(&Host::audio_batch);
    core.set_input_poll(&Host::input_poll); core.set_input_state(&Host::input_state);
    core.init();
    struct Cleanup { CoreApi& core; bool loaded{}; ~Cleanup() { if (loaded) core.unload_game(); core.deinit(); } } cleanup{core};
    retro::system_info info{}; core.get_system_info(&info);
    std::cout << "core=" << (info.library_name ? info.library_name : "unknown") << " version=" << (info.library_version ? info.library_version : "unknown") << '\n';
    std::vector<char> content;
    const std::string content_name = content_path.string();
    retro::game_info game{};
    if (!content_path.empty()) {
      if (!std::filesystem::is_regular_file(content_path)) throw std::runtime_error("The selected content file does not exist");
      game.path = content_name.c_str();
      if (!info.need_fullpath) { content = read_file(content_path); game.data = content.data(); game.size = content.size(); }
    } else if (!host.support_no_game) throw std::runtime_error("This core requires a content/ROM file");
    if (!core.load_game(content_path.empty() ? nullptr : &game)) throw std::runtime_error(host.hardware_requested ? "This core requires a hardware-rendering context. Select a software-rendering core." : "The core rejected the selected content; check file format and required BIOS files");
    cleanup.loaded = true;
    core.set_controller_port_device(0, retro::device_joypad);
    retro::system_av_info av{}; core.get_system_av_info(&av);
    if (!std::isfinite(av.timing.fps) || av.timing.fps <= 0 || av.timing.fps > 1000 || !std::isfinite(av.timing.sample_rate) || av.timing.sample_rate < 1000 || av.timing.sample_rate > 384000)
      throw std::runtime_error("The core returned invalid timing information");
    host.aspect = av.geometry.aspect_ratio;
    host.sample_rate = static_cast<unsigned>(std::lround(av.timing.sample_rate));
    SDL_AudioSpec desired{}, obtained{};
    desired.freq = static_cast<int>(host.sample_rate); desired.format = AUDIO_S16SYS; desired.channels = 2; desired.samples = 1024;
    host.audio = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (host.audio) SDL_PauseAudioDevice(host.audio, 0);
    else std::cerr << "Audio device unavailable: " << SDL_GetError() << '\n';
    const auto stem = save_stem(core_path, content_path);
    const auto state_path = state_directory / (stem + ".state");
    const auto ram_path = state_directory / (stem + ".srm");
    const auto ram_size = core.get_memory_size(0);
    if (ram_size > 64U * 1024U * 1024U) throw std::runtime_error("Core save RAM exceeds the supported size");
    if (ram_size && core.get_memory_data(0) && std::filesystem::is_regular_file(ram_path)) {
      auto bytes = read_file(ram_path, 64U * 1024U * 1024U);
      if (bytes.size() == ram_size) std::memcpy(core.get_memory_data(0), bytes.data(), ram_size);
      else std::cerr << "Ignoring save RAM with an incompatible size\n";
    }
    if (!load_path.empty()) load_state(core, load_path);
    auto next_frame = std::chrono::steady_clock::now();
    bool paused{}, quit{};
    std::uint64_t frames{};
    while (!quit && !termination_requested && !host.requested_shutdown && (!frame_limit || frames < frame_limit)) {
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) quit = true;
        if (event.type == SDL_CONTROLLERDEVICEADDED || event.type == SDL_CONTROLLERDEVICEREMOVED) host.refresh_controllers();
        if (event.type == SDL_CONTROLLERBUTTONDOWN) {
          if (event.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) quit = true;
          SDL_GameController* controller = nullptr;
          for (auto* candidate : host.controllers) if (candidate && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(candidate)) == event.cbutton.which) { controller = candidate; break; }
          if (controller && SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK)) {
            try {
              switch (event.cbutton.button) {
              case SDL_CONTROLLER_BUTTON_START: quit = true; break;
              case SDL_CONTROLLER_BUTTON_X: paused = !paused; if (host.audio) SDL_PauseAudioDevice(host.audio, paused ? 1 : 0); break;
              case SDL_CONTROLLER_BUTTON_Y: core.reset(); break;
              case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: save_state(core, state_path); break;
              case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: load_state(core, state_path); if (host.audio) SDL_ClearQueuedAudio(host.audio); break;
              case SDL_CONTROLLER_BUTTON_A: host.screenshot(state_directory / (stem + "-screenshot.bmp")); break;
              default: break;
              }
            } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
          }
        }
        if (event.type == SDL_KEYDOWN && !event.key.repeat) {
          try {
            switch (event.key.keysym.sym) {
            case SDLK_ESCAPE: quit = true; break;
            case SDLK_SPACE: paused = !paused; if (host.audio) SDL_PauseAudioDevice(host.audio, paused ? 1 : 0); break;
            case SDLK_F2: core.reset(); break;
            case SDLK_F5: save_state(core, state_path); break;
            case SDLK_F8: load_state(core, state_path); if (host.audio) SDL_ClearQueuedAudio(host.audio); break;
            case SDLK_F12: {
              host.screenshot(state_directory / (stem + "-screenshot.bmp"));
              break;
            }
            default: break;
            }
          } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
        }
      }
      if (quit) break;
      if (!paused) { core.run(); ++frames; }
      if (!host.fatal_error.empty()) throw std::runtime_error(host.fatal_error);
      host.present();
      if (!headless) {
        next_frame += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>{1.0 / av.timing.fps});
        const auto now = std::chrono::steady_clock::now();
        if (next_frame > now) std::this_thread::sleep_until(next_frame);
        else if (now - next_frame > std::chrono::milliseconds(250)) next_frame = now;
      }
    }
    if (!screenshot_path.empty()) host.screenshot(screenshot_path);
    if (save_on_exit) save_state(core, state_path);
    if (ram_size && core.get_memory_data(0)) write_file(ram_path, core.get_memory_data(0), ram_size);
    std::cout << "frames=" << frames << " video_callbacks=" << host.video_callbacks << " audio_frames=" << host.audio_frames << '\n';
    return 0;
  } catch (const std::exception& error) { std::cerr << "openxmb-retro: " << error.what() << '\n'; return 1; }
}
