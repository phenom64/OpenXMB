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

#include <string>
#include <string_view>
#include <vector>

namespace openxmb::localization {

inline constexpr std::string_view expansion_pseudo_locale = "en-XA";
inline constexpr std::string_view rtl_pseudo_locale = "ar-XB";

[[nodiscard]] LocalizationResult<std::string>
normalize_locale_tag(std::string_view tag);

// Ordered from the most specific requested locale to the default locale.
// Duplicate parents are removed while preserving first occurrence.
[[nodiscard]] LocalizationResult<std::vector<std::string>>
locale_fallback_chain(std::string_view requested_locale,
                      std::string_view default_locale);

[[nodiscard]] bool
is_pseudo_locale(std::string_view normalized_locale) noexcept;

} // namespace openxmb::localization
