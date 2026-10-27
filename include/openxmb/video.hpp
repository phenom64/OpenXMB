#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace openxmb::video {

enum class pixel_format { rgba, yuv420p, nv12 };
enum class color_matrix { bt601, bt709, bt2020 };

// Tightly packed owned planes. For NV12 plane 1 has ceil(width/2) texels
// containing interleaved U,V bytes (upload as R8G8); planar YUV uses R8.
struct plane {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<std::uint8_t> bytes;
};

struct frame {
    int width = 0;
    int height = 0;
    double timestamp = 0.0;
    double aspect_ratio = 1.0;
    std::vector<std::uint8_t> rgba;
    pixel_format format = pixel_format::rgba;
    color_matrix matrix = color_matrix::bt601;
    bool full_range = false;
    std::array<plane, 3> planes;
};

// Cumulative session measurements. Preparation includes plane allocation/copy
// or swscale; hardware download is measured separately, not claimed zero-copy.
struct pipeline_statistics {
    std::uint64_t frames = 0;
    std::uint64_t native_frames = 0;
    std::uint64_t rgba_frames = 0;
    std::uint64_t hardware_downloads = 0;
    std::uint64_t upload_bytes = 0;
    double hardware_download_ms = 0;
    double preparation_ms = 0;
};

struct track {
    int index = -1;
    std::string label;
};

struct subtitle_cue {
    double start = 0.0;
    double end = 0.0;
    std::string text;
};

struct information {
    int width = 0;
    int height = 0;
    double duration = 0.0;
    double frame_rate = 25.0;
    double aspect_ratio = 1.0;
    std::string decoder;
    std::string audio_status;
    std::vector<track> audio_tracks;
    std::vector<track> subtitle_tracks;
};

struct options {
    std::string hardware_acceleration = "auto";
    bool audio_output = true;
    bool native_yuv = false;
};

enum class state { playing, paused, ended, failed };

// Local-file decoding and playback clock. Call control/pump methods from one
// thread; SDL's audio callback consumes buffered PCM independently.
class session {
public:
    explicit session(const std::filesystem::path& path, options settings = {});
    ~session();
    session(const session&) = delete;
    session& operator=(const session&) = delete;

    const information& info() const;
    state playback_state() const;
    const std::string& error() const;
    double position() const;
    double buffered_audio_seconds() const;
    std::size_t played_audio_frames() const;
    const pipeline_statistics& statistics() const;

    // Decodes a bounded amount of work and publishes only frames whose
    // presentation timestamp is due. Audio is the master clock when available.
    void pump();
    std::optional<frame> take_frame();
    bool seek(double seconds);
    void pause(bool paused);
    void step(int direction = 1);
    void replay(double seconds = 10.0);
    void set_repeat(bool enabled);
    bool repeat() const;
    void set_volume(float volume);
    float volume() const;
    void set_rate(double rate);
    double rate() const;
    bool select_audio(int stream_index);
    bool select_subtitle(int stream_index);
    int selected_audio() const;
    int selected_subtitle() const;
    std::string subtitle_text() const;

private:
    struct implementation;
    std::unique_ptr<implementation> impl;
};

// Sidecar SubRip parsing is shared by playback and fixture-based validation.
std::vector<subtitle_cue> read_subrip(const std::filesystem::path& path);
std::string clean_subtitle_text(const std::string& text, bool ass = false);

}
