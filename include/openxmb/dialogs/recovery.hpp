#pragma once

#include "openxmb/dialogs/state_machine.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace openxmb::dialogs {

enum class RecoveryErrorCode {
  none,
  malformed,
  unsupported_version,
  invalid_state,
  missing_definition,
  limit_exceeded,
  allocation_failure,
};

struct RecoveryError {
  RecoveryErrorCode code{RecoveryErrorCode::none};
  std::size_t byte_offset{};
  std::string_view message;
};

struct RecoveryResult {
  std::optional<SessionState> state;
  std::optional<RecoveryError> error;

  [[nodiscard]] explicit operator bool() const noexcept {
    return state.has_value();
  }
};

// Canonical, length-prefixed, versioned recovery encoding. Maps are serialized
// in key order, so identical session states always produce identical bytes.
[[nodiscard]] std::string serialize_recovery(const SessionState &state);

[[nodiscard]] RecoveryResult
restore_recovery(const DefinitionCatalog &catalog,
                 std::string_view encoded) noexcept;

} // namespace openxmb::dialogs
