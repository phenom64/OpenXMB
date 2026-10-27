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
