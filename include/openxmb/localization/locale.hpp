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
