#pragma once

#include <openxmb/video.hpp>

#include <chrono>
#include <memory>

namespace openxmb::video {

struct playback_snapshot {
    bool loading = false;
    bool ready = false;
    state status = state::paused;
    information info;
    std::string error;
    double position = 0;
    double rate = 1;
    float volume = 1;
    bool repeat = false;
    int audio_track = -1;
    int subtitle_track = -1;
    std::string subtitle;
    std::chrono::steady_clock::time_point observed = std::chrono::steady_clock::now();
};

// One persistent decoding thread, a bounded native decoder queue, and one
// replaceable presentation frame. No filesystem or decoder work runs in the UI.
class worker {
public:
    worker();
    explicit worker(const std::filesystem::path& path, options settings = {});
    ~worker();
    worker(const worker&) = delete;
    worker& operator=(const worker&) = delete;

    void set_path(const std::filesystem::path& path, options settings = {});
    std::shared_ptr<const playback_snapshot> snapshot() const;
    std::optional<frame> take_frame();
    information info() const { return snapshot()->info; }
    state playback_state() const { return snapshot()->status; }
    std::string error() const { return snapshot()->error; }
    double position() const { return snapshot()->position; }
    double rate() const { return snapshot()->rate; }
    int selected_audio() const { return snapshot()->audio_track; }
    int selected_subtitle() const { return snapshot()->subtitle_track; }
    std::string subtitle_text() const { return snapshot()->subtitle; }

    void pause(bool paused);
    void seek(double seconds);
    void step(int direction = 1);
    void replay(double seconds = 10);
    void set_repeat(bool repeat);
    void set_volume(float volume);
    void set_rate(double rate);
    void select_audio(int stream);
    void select_subtitle(int stream);

private:
    struct implementation;
    std::unique_ptr<implementation> impl;
};

}
