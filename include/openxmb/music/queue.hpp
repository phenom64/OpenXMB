/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

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
