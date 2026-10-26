#include "openxmb/music/queue.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <numeric>
#include <random>
#include <stdexcept>

namespace openxmb::music {
namespace {
std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}
std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
} // namespace

void Queue::reorder(std::size_t selected, std::uint32_t seed) {
  order_.resize(entries_.size());
  std::iota(order_.begin(), order_.end(), 0);
  cursor_ = 0;
  if (order_.empty()) return;
  selected = std::min(selected, entries_.size() - 1);
  if (shuffled_) {
    std::swap(order_[0], order_[selected]);
    std::mt19937 engine(seed);
    std::shuffle(order_.begin() + 1, order_.end(), engine);
  } else {
    cursor_ = selected;
  }
}
void Queue::assign(std::vector<QueueEntry> entries, std::size_t selected) {
  entries_ = std::move(entries);
  reorder(selected);
}
std::optional<std::size_t> Queue::current_index() const noexcept {
  if (cursor_ >= order_.size()) return {};
  return order_[cursor_];
}
const QueueEntry *Queue::current() const noexcept {
  const auto index = current_index();
  return index ? &entries_[*index] : nullptr;
}
bool Queue::select(std::size_t index) {
  const auto found = std::find(order_.begin(), order_.end(), index);
  if (found == order_.end()) return false;
  cursor_ = static_cast<std::size_t>(found - order_.begin());
  return true;
}
bool Queue::next(bool natural_end) {
  if (order_.empty()) return false;
  if (natural_end && repeat_ == Repeat::one) return true;
  if (cursor_ + 1 < order_.size()) { ++cursor_; return true; }
  if (repeat_ == Repeat::all) { cursor_ = 0; return true; }
  return false;
}
bool Queue::previous() {
  if (order_.empty()) return false;
  if (cursor_ > 0) { --cursor_; return true; }
  if (repeat_ == Repeat::all) { cursor_ = order_.size() - 1; return true; }
  return false;
}
void Queue::set_shuffle(bool enabled, std::uint32_t seed) {
  if (enabled == shuffled_) return;
  const auto selected = current_index().value_or(0);
  shuffled_ = enabled;
  reorder(selected, seed);
}
void Queue::append(QueueEntry entry) {
  entries_.push_back(std::move(entry));
  order_.push_back(entries_.size() - 1);
}
bool Queue::erase(std::size_t index) {
  if (index >= entries_.size()) return false;
  auto selected = current_index().value_or(0);
  entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
  if (index < selected) --selected;
  reorder(selected);
  return true;
}
bool Queue::move(std::size_t from, std::size_t to) {
  if (from >= entries_.size() || to >= entries_.size()) return false;
  auto selected = current_index().value_or(0);
  auto entry = std::move(entries_[from]);
  entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(from));
  entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(to), std::move(entry));
  if (selected == from) selected = to;
  else if (from < selected && selected <= to) --selected;
  else if (to <= selected && selected < from) ++selected;
  reorder(selected);
  return true;
}
bool supported_audio(const std::filesystem::path &path) {
  const auto ext = lower(path.extension().string());
  constexpr const char *extensions[]{".mp3", ".wav", ".ogg", ".oga", ".opus", ".flac", ".m4a", ".aac", ".wma", ".aiff", ".aif", ".mod", ".xm", ".it", ".s3m"};
  return std::ranges::any_of(extensions, [&](const char *value) { return ext == value; });
}
bool supported_playlist(const std::filesystem::path &path) {
  const auto ext = lower(path.extension().string());
  return ext == ".m3u" || ext == ".m3u8" || ext == ".pls";
}
std::vector<QueueEntry> read_playlist(const std::filesystem::path &path) {
  std::ifstream stream(path);
  if (!stream) throw std::runtime_error("Cannot read playlist: " + path.string());
  std::vector<QueueEntry> entries;
  std::string line, title;
  const bool pls = lower(path.extension().string()) == ".pls";
  while (std::getline(stream, line)) {
    if (entries.size() >= 10000) throw std::runtime_error("Playlist exceeds 10000 tracks");
    line = trim(std::move(line));
    if (line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
    if (line.empty()) continue;
    if (line.starts_with("#EXTINF:")) {
      const auto comma = line.find(',');
      title = comma == std::string::npos ? std::string{} : line.substr(comma + 1);
      continue;
    }
    if (line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
    if (pls) {
      const auto equal = line.find('=');
      if (equal == std::string::npos || !lower(line.substr(0, equal)).starts_with("file")) continue;
      line = trim(line.substr(equal + 1));
    }
    if (line.find("://") != std::string::npos) throw std::runtime_error("Network playlist entries are unsupported");
    std::filesystem::path item(line);
    if (item.is_relative()) item = path.parent_path() / item;
    if (!supported_audio(item)) continue;
    entries.push_back({item.lexically_normal(), title.empty() ? item.stem().string() : title});
    title.clear();
  }
  if (entries.empty()) throw std::runtime_error("Playlist contains no supported local music");
  return entries;
}
void write_playlist(const std::filesystem::path &path, const std::vector<QueueEntry> &entries) {
  if (!supported_playlist(path)) throw std::runtime_error("Choose an M3U, M3U8, or PLS playlist filename");
  for (const auto &entry : entries) {
    if (entry.path.string().find_first_of("\r\n") != std::string::npos || entry.title.find_first_of("\r\n") != std::string::npos)
      throw std::runtime_error("Playlist paths and titles cannot contain line breaks");
  }
  std::ofstream stream(path, std::ios::trunc);
  if (!stream) throw std::runtime_error("Cannot write playlist: " + path.string());
  const bool pls = lower(path.extension().string()) == ".pls";
  stream << (pls ? "[playlist]\n" : "#EXTM3U\n");
  std::size_t index = 0;
  for (const auto &entry : entries) {
    if (pls) stream << "File" << ++index << '=' << std::filesystem::absolute(entry.path).string() << '\n';
    else stream << "#EXTINF:-1," << entry.title << '\n' << std::filesystem::absolute(entry.path).string() << '\n';
  }
  if (pls) stream << "NumberOfEntries=" << entries.size() << "\nVersion=2\n";
  stream.flush();
  if (!stream) throw std::runtime_error("Failed to save complete playlist");
}
} // namespace openxmb::music
