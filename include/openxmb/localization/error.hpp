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
