#pragma once

// Private bridge between the shell's gettext messages and its JSON catalog.
#include <libintl.h>
#include "language_options.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace openxmb::localization::shell_locale {
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

inline void apply(std::string_view requested, const std::filesystem::path& directory) {
    const auto language=normalize(requested);
    // LANGUAGE is a gettext override, not an OS locale name. In particular,
    // changing LC_ALL alone does not select a catalog on Windows.
#ifdef _WIN32
    _putenv_s("LANGUAGE",language=="auto" ? "" : language.c_str());
#else
    if(language=="auto") unsetenv("LANGUAGE");
    else setenv("LANGUAGE",language.c_str(),1);
#endif
    // Toggle LC_MESSAGES to invalidate gettext's translation cache. Do not
    // change numeric/time locales or overwrite the user's LC_ALL/LANG values.
    setlocale(LC_MESSAGES,"C");
    setlocale(LC_MESSAGES,"");
    const auto* messages=setlocale(LC_MESSAGES,nullptr);
    if(language!="auto" && (!messages || std::string_view(messages)=="C" ||
                           std::string_view(messages)=="POSIX")) {
#ifdef _WIN32
        setlocale(LC_MESSAGES,"en_US.UTF-8");
#else
        if(!setlocale(LC_MESSAGES,"C.UTF-8")) setlocale(LC_MESSAGES,"en_US.UTF-8");
#endif
    }
    bindtextdomain("shell",std::filesystem::absolute(directory).string().c_str());
    bind_textdomain_codeset("shell","UTF-8");
    textdomain("shell");
}

inline std::string translate(std::string_view source) {
    // gettext reserves the empty key for catalog metadata, never visible UI.
    if(source.empty()) return {};
    const auto exact_key=std::string(source);
    const auto* exact=dgettext("shell",exact_key.c_str());
    // gettext returns the argument itself on a miss. A catalog entry with the
    // same spelling is still authoritative (for names and British English).
    if(exact && exact!=exact_key.c_str()) return std::string(exact);
    // Reuse reviewed translations when the desktop catalog calls the same
    // control by a newer name. Untranslated messages retain their real source.
    constexpr std::pair<std::string_view,std::string_view> aliases[]{
        {"Theme Settings","Personalization Settings"},
        {"Preferred interface language.","Preferred language for the shell"},
        {"Date and Time Settings","Date/Time Settings"},
        {"Accessory Settings","Input Settings"},
        {"Frame Limit","Max FPS"}, {"Anti-aliasing","Sample Count"},
        {"Controller Prompts","Controller Type"},
        {"Controller Vibration","Controller Rumble"},
        {"Analog Navigation","Navigate Menus with Analog Stick"},
        {"Show GPU Memory","Show Memory Usage"},
    };
    auto msgid=source;
    for(const auto& [label,existing]:aliases) if(source==label) {msgid=existing;break;}
    const auto key=std::string(msgid);
    const auto* translated=dgettext("shell",key.c_str());
    return translated && std::string_view(translated)!=msgid ? std::string(translated) : std::string(source);
}
inline std::string language_label(std::string_view value) {
    const auto language=normalize(value);
    for(const auto& [code,name]:languages) if(code==language) return translate(name);
    return translate("British English");
}
}
