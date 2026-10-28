#pragma once

#include "shell_locale.hpp"
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace app::detail {
// Only immutable character arrays opt into implicit UI localization. Dynamic
// strings, string_views, pointers and mutable buffers remain display content.
template<class Character, std::size_t N>
    requires std::is_same_v<Character, const char>
inline std::string localize_ui_literal(Character (&literal)[N]) {
    return openxmb::localization::shell_locale::translate(std::string_view(literal,N-1));
}
inline std::vector<std::string> localize_ui_choices(std::vector<std::string> labels) {
    for(auto& label:labels)label=openxmb::localization::shell_locale::translate(label);
    return labels;
}
}
