#pragma once

// The stable libretro C ABI used by the standalone software-rendering host.
// These declarations describe the public interface; cores remain separate libraries.
#include <cstddef>
#include <cstdint>

namespace openxmb::integrations::retro {
using environment_t = bool (*)(unsigned, void*);
using video_refresh_t = void (*)(const void*, unsigned, unsigned, std::size_t);
using audio_sample_t = void (*)(std::int16_t, std::int16_t);
using audio_sample_batch_t = std::size_t (*)(const std::int16_t*, std::size_t);
using input_poll_t = void (*)();
using input_state_t = std::int16_t (*)(unsigned, unsigned, unsigned, unsigned);
struct game_info { const char* path; const void* data; std::size_t size; const char* meta; };
struct system_info { const char* library_name; const char* library_version; const char* valid_extensions; bool need_fullpath; bool block_extract; };
struct game_geometry { unsigned base_width, base_height, max_width, max_height; float aspect_ratio; };
struct system_timing { double fps, sample_rate; };
struct system_av_info { game_geometry geometry; system_timing timing; };
struct variable { const char* key; const char* value; };
struct message { const char* msg; unsigned frames; };
struct log_callback { void (*log)(int, const char*, ...); };
inline constexpr unsigned device_joypad = 1;
inline constexpr unsigned device_keyboard = 3;
inline constexpr unsigned device_analog = 5;
inline constexpr unsigned joypad_mask = 256;
inline constexpr unsigned api_version = 1;
} // namespace openxmb::integrations::retro
