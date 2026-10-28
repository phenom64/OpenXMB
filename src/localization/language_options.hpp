#pragma once
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openxmb::localization::shell_locale {
inline constexpr std::array<std::pair<std::string_view,std::string_view>,13> languages{{
    {"auto","Use system language"},{"en","British English"},
    {"de","German"},{"fr","French"},{"hi","Hindi"},{"pl","Polish"},
    {"id","Indonesian"},{"es","Spanish"},{"ja","Japanese"},
    {"zh_CN","Simplified Chinese"},{"zh_TW","Traditional Chinese"},
    {"ko","Korean"},{"ru","Russian"}
}};
inline std::vector<std::string> language_values() {
    std::vector<std::string> result;
    for(const auto& [value,label]:languages) result.emplace_back(value);
    return result;
}
}
