module;
#include "openxmb/desktop/library.hpp"
#include "openxmb/desktop/preview_audio.hpp"
#include "openxmb/music/player.hpp"
#if OPENXMB_ENABLE_VIDEO_PLAYER
#include "openxmb/video_worker.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

export module openxmb.app:game_presentation;
import auroreengine;
import glm;
import vulkan_hpp;
import vma;

namespace app {
namespace desktop = openxmb::desktop;
namespace fs = std::filesystem;
export class game_presentation {
private:
  auroreengine::resource_loader &loader;
  vk::Device device;
  vma::Allocator allocator;
  auroreengine::window *window{};
  std::shared_ptr<const desktop::Game> selected;
  std::string selection_key;
  std::chrono::steady_clock::time_point selected_at{};
  std::shared_ptr<auroreengine::texture> background;
  std::future<void> background_ready;
  std::shared_ptr<auroreengine::texture> loading_background, next_background;
  fs::path background_path, loading_path, next_path, attempted_path;
  float background_opacity{};
  std::chrono::steady_clock::time_point faded_at{std::chrono::steady_clock::now()};
  desktop::PreviewAudio audio;
  bool started{}, owns_duck{};
#if OPENXMB_ENABLE_VIDEO_PLAYER
  openxmb::video::worker movie;
  std::optional<openxmb::video::frame> pending;
  std::unique_ptr<auroreengine::texture> movie_texture;
  std::vector<vma::UniqueAllocation> allocations;
  std::vector<vma::UniqueBuffer> buffers;
  bool movie_initialized{}, movie_current{};
  double movie_aspect{16.0 / 9.0};
#endif
public:
  explicit game_presentation(auroreengine::resource_loader &value)
      : loader(value), device(value.getDevice()), allocator(value.getAllocator()) {}
  ~game_presentation() {
    stop();
    if (background_ready.valid()) background_ready.wait();
    try { wait_gpu(); } catch (...) {}
  }
  void wait_gpu() {
    if (window) window->waitIdle(); else device.waitIdle();
  }
  void duck(bool enabled) {
    if (enabled) { openxmb::music::service().set_external_duck(true); owns_duck = true; }
    else if (owns_duck) { openxmb::music::service().set_external_duck(false); owns_duck = false; }
  }
  void stop() {
    audio.stop(); duck(false);
#if OPENXMB_ENABLE_VIDEO_PLAYER
    movie.set_path({}); pending.reset(); movie_current = false;
#endif
    selected.reset(); selection_key.clear(); started = false;
  }
  void choose(std::shared_ptr<const desktop::Game> game) {
    const std::string key = game ? game->id + '\n' + game->background.string() + '\n' + game->preview.string() + '\n' + game->music.string() : "";
    if (key == selection_key) return;
    stop(); selected = std::move(game); selection_key = key;
    selected_at = std::chrono::steady_clock::now();
    attempted_path.clear();
    if (next_background && (!selected || next_path != selected->background)) {
      next_background.reset(); next_path.clear();
    }
  }
  void tick(auroreengine::window *value) {
    window = value;
    const auto now=std::chrono::steady_clock::now();
    const float dt=std::max(0.0f,std::chrono::duration<float>(now-faded_at).count());
    faded_at=now;
    if (selected && !started && now - selected_at >= std::chrono::milliseconds(350)) {
      started = true;
      if (!selected->music.empty()) audio.set_path(selected->music, 0.45f);
#if OPENXMB_ENABLE_VIDEO_PLAYER
      if (!selected->preview.empty()) {
        openxmb::video::options options;
        options.hardware_acceleration = "off";
        options.audio_output = selected->music.empty();
        movie.set_path(selected->preview, options);
        movie.set_repeat(true);
        movie.set_volume(0.45f);
      }
#endif
    }
    // A loader request owns its destination until its future completes. A
    // changed selection never blocks on it and never displays a stale result.
    if (background_ready.valid() && background_ready.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
      bool loaded=false;
      try {background_ready.get();loaded=loading_background && loading_background->loaded;}
      catch(...) {loaded=false;}
      if (loaded && selected && started && selected->background==loading_path) {
        next_background=std::move(loading_background);next_path=loading_path;
      } else loading_background.reset();
      loading_path.clear();
    }
    if (selected && started && !selected->background.empty() && selected->background!=background_path &&
        selected->background!=next_path && selected->background!=attempted_path && !background_ready.valid()) {
      attempted_path=selected->background;
      try {
        loading_background=std::make_shared<auroreengine::texture>(device,allocator);
        loading_path=selected->background;
        background_ready=loader.loadTexture(loading_background.get(),loading_path);
      } catch(...) {loading_background.reset();loading_path.clear();}
    }
    const bool show=selected && started && background && background->loaded &&
                    selected->background==background_path;
    constexpr float fade_seconds=0.35f;
    background_opacity=std::clamp(background_opacity+(show?dt:-dt)/fade_seconds,0.0f,1.0f);
    if (background_opacity<=0.0f && background && !show) {
      // Submitted frames can still sample this texture after it disappears
      // from the current scene; retire only after those GPU reads complete.
      wait_gpu();background.reset();background_path.clear();
    }
    if (!background && next_background) {
      if (selected && started && selected->background==next_path) {
        background=std::move(next_background);background_path=next_path;
      } else next_background.reset();
      next_path.clear();
    }
    if (!selected) return;
    audio.tick();
    bool audible = audio.active();
#if OPENXMB_ENABLE_VIDEO_PLAYER
    if (started && !selected->preview.empty()) {
      const auto snapshot = movie.snapshot();
      if (snapshot->ready && snapshot->status != openxmb::video::state::failed) {
        if (auto frame = movie.take_frame()) pending = std::move(frame);
        audible = audible || (selected->music.empty() && !snapshot->info.audio_tracks.empty());
      }
    }
#endif
    duck(audible);
  }
  void upload(vk::CommandBuffer command, int frame, auroreengine::window *value) {
    window = value;
#if OPENXMB_ENABLE_VIDEO_PLAYER
    if (!pending) return;
    const auto &image = *pending;
    if (!movie_texture || movie_texture->width != image.width || movie_texture->height != image.height || buffers.size() != window->swapchainImageCount) {
      wait_gpu(); movie_texture.reset(); buffers.clear(); allocations.clear();
      movie_texture = std::make_unique<auroreengine::texture>(device, allocator, image.width, image.height);
      buffers.resize(window->swapchainImageCount); allocations.resize(window->swapchainImageCount);
      const vk::BufferCreateInfo info({}, image.rgba.size(), vk::BufferUsageFlagBits::eTransferSrc, vk::SharingMode::eExclusive);
      const vma::AllocationCreateInfo memory({}, vma::MemoryUsage::eCpuToGpu);
      for (std::size_t i = 0; i < buffers.size(); ++i) std::tie(buffers[i], allocations[i]) = allocator.createBufferUnique(info, memory);
      movie_initialized = false;
    }
    if (frame < 0 || static_cast<std::size_t>(frame) >= buffers.size()) return;
    allocator.copyMemoryToAllocation(image.rgba.data(), allocations[frame].get(), 0, image.rgba.size());
    const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
    command.pipelineBarrier(movie_initialized ? vk::PipelineStageFlagBits::eFragmentShader : vk::PipelineStageFlagBits::eTopOfPipe,
      vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, vk::ImageMemoryBarrier(movie_initialized ? vk::AccessFlags{vk::AccessFlagBits::eShaderRead} : vk::AccessFlags{},
        vk::AccessFlagBits::eTransferWrite, movie_initialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
        vk::ImageLayout::eTransferDstOptimal, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, movie_texture->image, range));
    command.copyBufferToImage(buffers[frame].get(), movie_texture->image, vk::ImageLayout::eTransferDstOptimal,
      vk::BufferImageCopy(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), vk::Offset3D(0, 0, 0),
        vk::Extent3D(static_cast<std::uint32_t>(image.width), static_cast<std::uint32_t>(image.height), 1)));
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {},
      vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead, vk::ImageLayout::eTransferDstOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, movie_texture->image, range));
    movie_initialized = movie_current = true; movie_texture->loaded.store(true, std::memory_order_release);
    movie_aspect = image.aspect_ratio; pending.reset();
#else
    (void)command; (void)frame;
#endif
  }
  bool has_movie() const {
#if OPENXMB_ENABLE_VIDEO_PLAYER
    return selected && movie_texture && movie_current;
#else
    return false;
#endif
  }
  [[nodiscard]] float opacity() const { return background_opacity; }
  [[nodiscard]] bool background_ready_for_draw() const {return background && background->loaded && background_opacity>0.0f;}
  void draw_background(auroreengine::gui_renderer &r,float max_opacity=1.0f) {
    if (!background_ready_for_draw() || background->width<=0 || background->height<=0) return;
    const float screen=static_cast<float>(r.aspect_ratio);
    if (screen<=0) return;
    const float image=static_cast<float>(background->width)/static_cast<float>(background->height);
    const float height=std::max(1.0f,screen/image),width=height*image;
    const float x=(1.0f-width/screen)*0.5f,y=(1.0f-height)*0.5f;
    r.draw_image(background->imageView.get(),x,y,width,height,{1,1,1,background_opacity*std::clamp(max_opacity,0.0f,1.0f)});
  }
  [[nodiscard]] const auroreengine::texture* animated_icon(std::string_view identity) const {
#if OPENXMB_ENABLE_VIDEO_PLAYER
    return has_movie() && selected->id==identity ? movie_texture.get() : nullptr;
#else
    (void)identity; return nullptr;
#endif
  }
  void draw_movie(auroreengine::gui_renderer &r) {
#if OPENXMB_ENABLE_VIDEO_PLAYER
    if (!has_movie()) return;
    const float width = 0.25f * static_cast<float>(r.aspect_ratio);
    const float height = std::min(0.33f, width / static_cast<float>(std::max(0.1, movie_aspect)));
    r.draw_rect({0.665f, 0.275f}, {0.26f, height + 0.02f}, {0.0f, 0.0f, 0.0f, 0.65f});
    r.draw_image(movie_texture->imageView.get(), 0.67f, 0.285f, width, height);
#else
    (void)r;
#endif
  }
};

}
