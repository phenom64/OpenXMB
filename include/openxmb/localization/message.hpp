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

#include "openxmb/localization/error.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace openxmb::localization {

struct PlaceholderUse {
  std::string name;
  std::size_t count{};

  [[nodiscard]] bool operator==(const PlaceholderUse &) const = default;
};

struct PlaceholderSignature {
  std::vector<PlaceholderUse> placeholders;

  [[nodiscard]] bool operator==(const PlaceholderSignature &) const = default;
};

enum class PseudoLocaleKind { expansion, right_to_left };

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

// Recognises named placeholders such as {name} and {count:03}. Literal braces
// must be escaped as {{ and }}. Placeholder order may change in translations,
// but names and repetition counts must remain identical.
[[nodiscard]] LocalizationResult<PlaceholderSignature>
analyze_placeholders(std::string_view message);

[[nodiscard]] LocalizationResult<void>
validate_placeholder_compatibility(std::string_view source,
                                   std::string_view translation);

// Pseudo-localization preserves placeholders exactly and keeps brace escapes
// syntactically intact while transforming their human-visible literal text.
[[nodiscard]] LocalizationResult<std::string>
pseudo_localize(std::string_view message, PseudoLocaleKind kind);

} // namespace openxmb::localization
