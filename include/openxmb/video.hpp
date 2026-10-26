#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace openxmb::video {

struct frame {
    int width = 0;
    int height = 0;
    double timestamp = 0.0;
    double aspect_ratio = 1.0;
    std::vector<std::uint8_t> rgba;
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
