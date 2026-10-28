#pragma once

// Private bridge between the shell's gettext messages and its JSON catalog.
#include <libintl.h>
#include "language_options.hpp"
#include <algorithm>
#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace openxmb::localization::shell_locale {

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
                           std::string_view(messages).starts_with("C.") ||
                           std::string_view(messages)=="POSIX")) {
#ifdef _WIN32
        setlocale(LC_MESSAGES,"en_US.UTF-8");
#else
        // glibc intentionally ignores LANGUAGE for C.<encoding> as well as C.
        // Select a real message locale; keep all other locale categories intact.
        if(!setlocale(LC_MESSAGES,"en_US.UTF-8") && !setlocale(LC_MESSAGES,"en_US.utf8")) {
#if defined(__linux__)
            // Minimal distributions can contain only C locales. The Linux
            // package carries a message locale compiled at its glibc baseline.
            // LOCPATH is needed only while loading it, and remains user-owned.
            const auto bundled=std::filesystem::absolute(directory).parent_path()/".native-locales";
            if(std::filesystem::is_directory(bundled)) {
                const auto* previous=std::getenv("LOCPATH");
                const bool had_previous=previous!=nullptr;
                const std::string saved=previous?previous:"";
                const auto lookup=bundled.string()+(saved.empty()?"":":"+saved);
                setenv("LOCPATH",lookup.c_str(),1);
                setlocale(LC_MESSAGES,"en_US.UTF-8");
                if(had_previous) setenv("LOCPATH",saved.c_str(),1);
                else unsetenv("LOCPATH");
            }
#endif
        }
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
