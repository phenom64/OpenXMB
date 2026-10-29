#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace openxmb::desktop {
// Optional trophy-notice alert played from ONE user-supplied PCM WAV file.
//
// There is no built-in sound, download, firmware extraction or reuse of another
// application's audio. The default (Settings{}) is Off and reads no file.
//
// The class borrows the application's already-open SDL_mixer device. It never
// opens/closes/reconfigures audio, installs hooks, changes channel/group
// reservations, channel volumes or music volume. Gain is applied only to its own
// Mix_Chunk. Every mixer call must come from the thread that already drives
// SDL_mixer (no internal locking, threads, sleeps, network or logging).
//
// Dedup: play_once() consumes a notice serial the first time it is seen,
// whatever the outcome (Off, muted, unavailable, device closed...). The consumed
// serial survives configure(), set_volume(), stop(), device_will_close() and
// prepare(), so enabling or unmuting while an old popup is visible never
// replays it. Only a strictly larger serial can play again.
class TrophySound {
public:
  static constexpr std::size_t max_file_bytes = 2u * 1024u * 1024u;
  static constexpr std::uint32_t max_duration_ms = 5000;
  static constexpr std::uint32_t min_sample_rate = 8000;
  static constexpr std::uint32_t max_sample_rate = 96000;

  enum class CueStatus {
    off,                // disabled: no file was read, nothing is cached
    ready,              // validated and cached (see CueResult::decoded)
    path_empty,         // enabled but no file chosen
    path_invalid,       // not absolute local UTF-8 (relative, UNC/device, bad UTF-8, NUL, stream)
    not_found,
    not_regular_file,   // directory, device, ...
    unreadable,         // open/read failed, or the file changed while being read
    too_large,          // larger than max_file_bytes
    not_wav,            // no RIFF/WAVE container
    unsupported_format, // not uncompressed PCM, 8/16-bit, mono/stereo, 8-96 kHz
    malformed,          // truncated, bad chunk sizes, missing/duplicate fmt/data
    too_long,           // longer than max_duration_ms
    decode_failed       // SDL_mixer refused the validated data
  };

  struct Settings {
    bool enabled = false;       // audio.trophy.mode == User-supplied
    std::string path;           // absolute UTF-8 local path (audio.trophy.file)
    int master_percent = 100;   // clamped 0..100
    int effects_percent = 100;  // clamped 0..100
  };

  // Result of validating/caching a file. Fields other than status are zero unless
  // the file reached the PCM format check.
  struct CueResult {
    CueStatus status = CueStatus::off;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::uint32_t bits_per_sample = 0;
    std::uint32_t duration_ms = 0;
    std::size_t file_bytes = 0;
    bool decoded = false; // chunk exists for the CURRENT mixer device (false while it is closed)
    [[nodiscard]] bool ok() const noexcept { return status == CueStatus::ready; }
  };

  enum class PlayStatus {
    played,         // a channel of this instance started
    repeated,       // serial already consumed (e.g. passive->interactive promotion)
    stale,          // serial older than one already consumed; ignored
    invalid_serial, // zero; ignored, nothing consumed
    off,            // feature disabled; serial consumed
    unavailable,    // enabled but no valid cue configured; serial consumed
    muted,          // master*effects gain is 0; serial consumed
    device_closed,  // SDL_mixer not open; serial consumed
    no_channel,     // every unreserved mixer channel busy; serial consumed
    play_failed     // lazy re-decode or Mix_PlayChannel failed; serial consumed
  };

  TrophySound();
  ~TrophySound(); // halts/frees only this instance's chunk
  TrophySound(const TrophySound &) = delete;
  TrophySound &operator=(const TrophySound &) = delete;

  // Pure check; touches neither mixer nor any instance. Reads the file once, with
  // bounded memory. Use it to validate a new Settings file BEFORE saving it.
  [[nodiscard]] static CueResult inspect(const std::string &utf8_path);

  // Stops and drops the previous cue first (an invalid file never leaves the old
  // sound active), then reads/validates/caches the file (blocking disk I/O: call
  // off the playback path). If the mixer is open the cue is decoded now,
  // otherwise it is held as validated PCM until prepare()/play_once().
  // Enabled == false reads nothing and returns CueStatus::off. Keeps the consumed
  // serial. Never reads the source again after returning.
  CueResult configure(const Settings &settings);

  // Cheap per-frame/mute/volume update, no file access. Applies to this chunk
  // only. Zero gain halts this instance's active cue (it is never restarted).
  void set_volume(int master_percent, int effects_percent) noexcept;

  // Call once per genuine notice when it first becomes visible. Memory only.
  PlayStatus play_once(std::uint64_t notice_serial);

  // Halts this instance's active cue only (cancel/profile/session change). Does
  // not forget the consumed serial.
  void stop() noexcept;

  // Call BEFORE the shared mixer is closed or reopened (output-device change):
  // halts and frees this instance's chunk, keeps validated bytes and the serial.
  void device_will_close() noexcept;

  // Call after the mixer reopened (optional; play_once also does it lazily):
  // decodes cached bytes for the current device. No disk read. Returns decoded().
  bool prepare() noexcept;

  [[nodiscard]] bool active() const noexcept;   // this instance's cue is playing right now
  [[nodiscard]] int channel() const noexcept;   // mixer channel of the last cue, -1 if none
  [[nodiscard]] std::uint64_t last_serial() const noexcept;
  [[nodiscard]] int volume() const noexcept;    // chunk volume 0..128 derived from the percents
  [[nodiscard]] const CueResult &cue() const noexcept;

  [[nodiscard]] static const char *describe(CueStatus status) noexcept;
  [[nodiscard]] static const char *describe(PlayStatus status) noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace openxmb::desktop
