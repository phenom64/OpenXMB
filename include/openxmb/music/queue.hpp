#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openxmb::music {
enum class Repeat { off, all, one };
struct QueueEntry {
  std::filesystem::path path;
  std::string title;
};

class Queue {
public:
  void assign(std::vector<QueueEntry> entries, std::size_t selected = 0);
  bool select(std::size_t index);
  bool next(bool natural_end = false);
  bool previous();
  void set_shuffle(bool enabled, std::uint32_t seed = 0x584d42);
  bool shuffle() const noexcept { return shuffled_; }
  void set_repeat(Repeat mode) noexcept { repeat_ = mode; }
  Repeat repeat() const noexcept { return repeat_; }
  const std::vector<QueueEntry> &entries() const noexcept { return entries_; }
  std::optional<std::size_t> current_index() const noexcept;
  const QueueEntry *current() const noexcept;
  void append(QueueEntry entry);
  bool erase(std::size_t index);
  bool move(std::size_t from, std::size_t to);

private:
  void reorder(std::size_t selected, std::uint32_t seed = 0x584d42);
  std::vector<QueueEntry> entries_;
  std::vector<std::size_t> order_;
  std::size_t cursor_{};
  bool shuffled_{};
  Repeat repeat_{Repeat::off};
};

bool supported_audio(const std::filesystem::path &path);
bool supported_playlist(const std::filesystem::path &path);
std::vector<QueueEntry> read_playlist(const std::filesystem::path &path);
void write_playlist(const std::filesystem::path &path,
                    const std::vector<QueueEntry> &entries);
} // namespace openxmb::music
