#include <openxmb/video_worker.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace openxmb::video {

struct worker::implementation {
    struct load_request {
        std::filesystem::path path;
        options settings;
        std::uint64_t generation = 0;
    };
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::optional<load_request> load;
    std::vector<std::function<void(session&)>> commands;
    std::optional<frame> latest_frame;
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::shared_ptr<const playback_snapshot>> latest{std::make_shared<const playback_snapshot>()};
    std::jthread thread;

    implementation() : thread([this](std::stop_token stop) { run(stop); }) {}
    ~implementation() {
        thread.request_stop();
        wake.notify_all();
        thread.join();
    }

    void command(std::function<void(session&)> operation) {
        { std::lock_guard lock(mutex); commands.push_back(std::move(operation)); }
        wake.notify_one();
    }

    void run(std::stop_token stop) {
        std::unique_ptr<session> playback;
        std::uint64_t active_generation = 0;
        while(!stop.stop_requested()) {
            std::optional<load_request> request;
            std::vector<std::function<void(session&)>> operations;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(5), [&] { return stop.stop_requested() || load || !commands.empty(); });
                if(stop.stop_requested()) break;
                request = std::move(load);
                load.reset();
                operations.swap(commands);
            }
            if(request) {
                playback.reset();
                active_generation = request->generation;
                try {
                    if(!request->path.empty()) playback = std::make_unique<session>(request->path, request->settings);
                } catch(const std::exception& exception) {
                    auto value = std::make_shared<playback_snapshot>();
                    value->status = state::failed;
                    value->error = exception.what();
                    std::lock_guard lock(mutex);
                    if(active_generation == generation.load()) latest.store(std::move(value));
                }
            }
            if(!playback || active_generation != generation.load()) continue;
            for(auto& operation : operations) operation(*playback);
            playback->pump();
            auto picture = playback->take_frame();
            auto value = std::make_shared<playback_snapshot>();
            value->ready = true;
            value->status = playback->playback_state();
            value->info = playback->info();
            value->error = playback->error();
            value->position = playback->position();
            value->rate = playback->rate();
            value->volume = playback->volume();
            value->repeat = playback->repeat();
            value->audio_track = playback->selected_audio();
            value->subtitle_track = playback->selected_subtitle();
            value->subtitle = playback->subtitle_text();
            value->observed = std::chrono::steady_clock::now();
            {
                std::lock_guard lock(mutex);
                if(active_generation != generation.load()) continue;
                if(picture) latest_frame = std::move(picture);
                latest.store(std::move(value));
            }
        }
    }
};

worker::worker() : impl(std::make_unique<implementation>()) {}
worker::worker(const std::filesystem::path& path, options settings) : worker() { set_path(path, std::move(settings)); }
worker::~worker() = default;

void worker::set_path(const std::filesystem::path& path, options settings) {
    auto next = impl->generation.fetch_add(1) + 1;
    auto value = std::make_shared<playback_snapshot>();
    value->loading = !path.empty();
    {
        std::lock_guard lock(impl->mutex);
        impl->load = implementation::load_request{path, std::move(settings), next};
        impl->commands.clear();
        impl->latest_frame.reset();
        impl->latest.store(std::move(value));
    }
    impl->wake.notify_one();
}
std::shared_ptr<const playback_snapshot> worker::snapshot() const { return impl->latest.load(); }
std::optional<frame> worker::take_frame() {
    std::lock_guard lock(impl->mutex);
    return std::exchange(impl->latest_frame, std::nullopt);
}
void worker::pause(bool paused) { impl->command([=](auto& player) { player.pause(paused); }); }
void worker::seek(double seconds) { impl->command([=](auto& player) { player.seek(seconds); }); }
void worker::step(int direction) { impl->command([=](auto& player) { player.step(direction); }); }
void worker::replay(double seconds) { impl->command([=](auto& player) { player.replay(seconds); }); }
void worker::set_repeat(bool enabled) { impl->command([=](auto& player) { player.set_repeat(enabled); }); }
void worker::set_volume(float volume) { impl->command([=](auto& player) { player.set_volume(volume); }); }
void worker::set_rate(double rate) { impl->command([=](auto& player) { player.set_rate(rate); }); }
void worker::select_audio(int stream) { impl->command([=](auto& player) { player.select_audio(stream); }); }
void worker::select_subtitle(int stream) { impl->command([=](auto& player) { player.select_subtitle(stream); }); }

}
