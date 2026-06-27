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
