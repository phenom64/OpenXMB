#pragma once

#include "openxmb/music/queue.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace openxmb::music {
enum class Status { stopped, playing, paused, ended, error };
enum class Visualizer { waves, spectrum, globe };
struct Metadata {
  std::string title, artist, album, genre, date, track, codec;
  int sample_rate{}, channels{};
  std::int64_t bit_rate{};
  double duration{};
  std::filesystem::path artwork_path;
  std::shared_ptr<const std::vector<std::uint8_t>> artwork;
  std::string artwork_type;
};
struct Snapshot {
  Metadata metadata;
  Status status{Status::stopped};
  double position{};
  float volume{0.75f}, effective_volume{0.75f};
  bool muted{}, background{true}, ducking{true}, ducked{};
  Visualizer visualizer{Visualizer::waves};
  std::array<float, 24> spectrum{};
  std::string error;
  std::uint64_t generation{};
};

// Main-thread transport. The SDL post-mix callback publishes samples through
// atomics and never accesses queue, metadata, UI, or filesystem state.
class Player {
public:
  Player();
  ~Player();
  Player(const Player &) = delete;
  Player &operator=(const Player &) = delete;
  bool open(const std::filesystem::path &path);
  bool set_queue(std::vector<QueueEntry> entries, std::size_t selected = 0);
  bool select(std::size_t index);
  bool play();
  void pause();
  void stop();
  bool seek(double seconds);
  bool next();
  bool previous();
  void tick();
  void shutdown();
  void set_volume(float volume);
  void set_muted(bool enabled);
  void set_background_enabled(bool enabled);
  void set_ducking_enabled(bool enabled);
  void set_external_duck(bool active);
  void set_visualizer(Visualizer visualizer);
  void set_shuffle(bool enabled);
  void set_repeat(Repeat repeat);
  bool remove(std::size_t index);
  bool move(std::size_t from, std::size_t to);
  bool append(const std::filesystem::path &path);
  bool save_playlist(const std::filesystem::path &path);
  const Snapshot &snapshot() const noexcept;
  const Queue &queue() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
Player &service();
} // namespace openxmb::music
