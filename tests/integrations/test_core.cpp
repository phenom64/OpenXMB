#include "openxmb/integrations/libretro_abi.hpp"
#include <array>
#include <cstring>

#ifdef _WIN32
#define CORE_EXPORT extern "C" __declspec(dllexport)
#else
#define CORE_EXPORT extern "C" __attribute__((visibility("default")))
#endif
namespace {
using namespace openxmb::integrations::retro;
environment_t environment{};
video_refresh_t video{};
audio_sample_t sample{};
audio_sample_batch_t audio{};
input_poll_t poll{};
input_state_t input{};
std::uint32_t counter{};
std::array<std::uint8_t, 16> ram{};
std::array<std::uint32_t, 64 * 48> pixels{};
std::array<std::int16_t, 128 * 2> samples{};
}
CORE_EXPORT unsigned retro_api_version() { return 1; }
CORE_EXPORT void retro_set_environment(environment_t callback) {
  environment = callback;
  bool no_game = true; environment(18, &no_game);
}
CORE_EXPORT void retro_set_video_refresh(video_refresh_t callback) { video = callback; }
CORE_EXPORT void retro_set_audio_sample(audio_sample_t callback) { sample = callback; }
CORE_EXPORT void retro_set_audio_sample_batch(audio_sample_batch_t callback) { audio = callback; }
CORE_EXPORT void retro_set_input_poll(input_poll_t callback) { poll = callback; }
CORE_EXPORT void retro_set_input_state(input_state_t callback) { input = callback; }
CORE_EXPORT void retro_get_system_info(system_info* info) { *info = {"OpenXMB Test Core", "1.0", "bin", false, false}; }
CORE_EXPORT void retro_get_system_av_info(system_av_info* info) { *info = {{64, 48, 64, 48, 4.0F / 3.0F}, {60.0, 48000.0}}; }
CORE_EXPORT void retro_init() { counter = 0; }
CORE_EXPORT void retro_deinit() {}
CORE_EXPORT bool retro_load_game(const game_info*) { unsigned pixel = 1; return environment(10, &pixel); }
CORE_EXPORT void retro_unload_game() {}
CORE_EXPORT void retro_reset() { counter = 0; }
CORE_EXPORT void retro_set_controller_port_device(unsigned, unsigned) {}
CORE_EXPORT void retro_run() {
  ++counter; poll();
  static_cast<void>(input(0, device_joypad, 0, joypad_mask));
  for (std::size_t index = 0; index < pixels.size(); ++index) pixels[index] = 0xff000000U | (static_cast<std::uint32_t>(index % 256) << 8) | (counter & 0xffU);
  video(pixels.data(), 64, 48, 64 * 4);
  samples.fill(static_cast<std::int16_t>(counter * 100));
  audio(samples.data(), 128);
  if (sample) sample(0, 0);
  ram[0] = static_cast<std::uint8_t>(counter);
}
CORE_EXPORT std::size_t retro_serialize_size() { return sizeof(counter); }
CORE_EXPORT bool retro_serialize(void* data, std::size_t size) { if (size != sizeof(counter)) return false; std::memcpy(data, &counter, size); return true; }
CORE_EXPORT bool retro_unserialize(const void* data, std::size_t size) { if (size != sizeof(counter)) return false; std::memcpy(&counter, data, size); return true; }
CORE_EXPORT void* retro_get_memory_data(unsigned id) { return id == 0 ? ram.data() : nullptr; }
CORE_EXPORT std::size_t retro_get_memory_size(unsigned id) { return id == 0 ? ram.size() : 0; }
