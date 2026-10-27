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
#include <vector>

namespace openxmb::localization {

enum class LocalizationErrorCode {
  invalid_locale,
  invalid_utf8,
  invalid_key,
  malformed_placeholder,
  placeholder_mismatch,
  unknown_translation_key,
  duplicate_translation,
  missing_key,
};

struct LocalizationError {
  LocalizationErrorCode code{LocalizationErrorCode::invalid_key};
  std::string operation;
  std::string detail;
  std::string locale;
  std::string key;
  std::size_t byte_offset{};
  std::vector<std::string> attempted_locales;
  std::vector<std::string> expected_placeholders;
  std::vector<std::string> actual_placeholders;

  [[nodiscard]] bool operator==(const LocalizationError &) const = default;
};

template <typename T> class [[nodiscard]] LocalizationResult {
public:
  static LocalizationResult success(T value) {
    return LocalizationResult(std::in_place_index<0>, std::move(value));
  }

  static LocalizationResult failure(LocalizationError error) {
    return LocalizationResult(std::in_place_index<1>, std::move(error));
  }

  [[nodiscard]] bool has_value() const noexcept {
    return storage_.index() == 0;
  }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T &value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T &value() const & { return std::get<0>(storage_); }
  [[nodiscard]] T &&value() && { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] LocalizationError &error() & { return std::get<1>(storage_); }
  [[nodiscard]] const LocalizationError &error() const & {
    return std::get<1>(storage_);
  }

private:
  template <std::size_t I, typename U>
  explicit LocalizationResult(std::in_place_index_t<I> tag, U &&value)
      : storage_(tag, std::forward<U>(value)) {}

  std::variant<T, LocalizationError> storage_;
};

template <> class [[nodiscard]] LocalizationResult<void> {
public:
  static LocalizationResult success() { return LocalizationResult(true, {}); }
  static LocalizationResult failure(LocalizationError error) {
    return LocalizationResult(false, std::move(error));
  }

  [[nodiscard]] bool has_value() const noexcept { return has_value_; }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] LocalizationError &error() & { return error_; }
  [[nodiscard]] const LocalizationError &error() const & { return error_; }

private:
  LocalizationResult(bool has_value, LocalizationError error)
      : has_value_(has_value), error_(std::move(error)) {}

  bool has_value_{};
  LocalizationError error_{};
};

} // namespace openxmb::localization
