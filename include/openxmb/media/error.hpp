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
