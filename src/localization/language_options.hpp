#pragma once
#include <array>
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openxmb::localization::shell_locale {
inline constexpr std::array<std::pair<std::string_view,std::string_view>,13> languages{{
    {"auto","Use system language"},{"en","British English"},
    {"de","Deutsch"},{"fr","Français"},{"hi","हिन्दी"},{"pl","Polski"},
    {"id","Bahasa Indonesia"},{"es","Español"},{"ja","日本語"},
    {"zh_CN","简体中文"},{"zh_TW","繁體中文"},
    {"ko","한국어"},{"ru","Русский"}
}};
inline std::string normalize(std::string_view requested) {
    if(requested.empty() || requested=="auto") return "auto";
    auto tag=std::string(requested);
    std::replace(tag.begin(),tag.end(),'-','_');
    std::transform(tag.begin(),tag.end(),tag.begin(),[](unsigned char c) {
        return c>='A' && c<='Z' ? static_cast<char>(c-'A'+'a') : static_cast<char>(c);
    });
    if(tag.starts_with("zh")) {
        if(tag.find("hant")!=std::string::npos || tag.starts_with("zh_tw") || tag.starts_with("zh_hk") || tag.starts_with("zh_mo")) return "zh_TW";
        return "zh_CN";
    }
    auto language=std::string(tag.substr(0,tag.find_first_of("_.:@")));
    std::transform(language.begin(),language.end(),language.begin(),[](unsigned char c) {
        return c>='A' && c<='Z' ? static_cast<char>(c-'A'+'a') : static_cast<char>(c);
    });
    for(const auto& [supported,label]:languages)
        if(language==supported) return language;
    return "en";
}

inline std::vector<std::string> language_values() {
    std::vector<std::string> result;
    for(const auto& [value,label]:languages) result.emplace_back(value);
    return result;
}
}
