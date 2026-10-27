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

#include <cstddef>
#include <string>
#include <utility>
#include <variant>

namespace openxmb::media {

enum class MediaErrorCode {
  invalid_argument,
  invalid_state,
  not_loaded,
  already_loaded,
  out_of_range,
  malformed_catalog,
  duplicate_id,
  missing_reference,
  unsupported_operation,
  backend_unavailable,
  backend_failure,
  clock_discontinuity,
};

struct MediaError {
  MediaErrorCode code{MediaErrorCode::invalid_argument};
  std::string operation;
  std::string detail;
  std::string subject_id;

  [[nodiscard]] bool operator==(const MediaError &) const = default;
};

[[nodiscard]] inline MediaError make_media_error(MediaErrorCode code,
                                                 std::string operation,
                                                 std::string detail,
                                                 std::string subject_id = {}) {
  return {code, std::move(operation), std::move(detail), std::move(subject_id)};
}

template <typename T> class [[nodiscard]] MediaResult {
public:
  static MediaResult success(T value) {
    return MediaResult(std::in_place_index<0>, std::move(value));
  }

  static MediaResult failure(MediaError error) {
    return MediaResult(std::in_place_index<1>, std::move(error));
  }

  [[nodiscard]] bool has_value() const noexcept {
    return storage_.index() == 0;
  }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T &value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T &value() const & { return std::get<0>(storage_); }
  [[nodiscard]] T &&value() && { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] MediaError &error() & { return std::get<1>(storage_); }
  [[nodiscard]] const MediaError &error() const & {
    return std::get<1>(storage_);
  }

private:
  template <std::size_t I, typename U>
  explicit MediaResult(std::in_place_index_t<I> tag, U &&value)
      : storage_(tag, std::forward<U>(value)) {}

  std::variant<T, MediaError> storage_;
};

template <> class [[nodiscard]] MediaResult<void> {
public:
  static MediaResult success() { return MediaResult(true, {}); }
  static MediaResult failure(MediaError error) {
    return MediaResult(false, std::move(error));
  }

  [[nodiscard]] bool has_value() const noexcept { return has_value_; }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] MediaError &error() & { return error_; }
  [[nodiscard]] const MediaError &error() const & { return error_; }

private:
  MediaResult(bool has_value, MediaError error)
      : has_value_(has_value), error_(std::move(error)) {}

  bool has_value_{false};
  MediaError error_{};
};

} // namespace openxmb::media
