#include "openxmb/xmb/catalog.hpp"
#include "../../src/app/components/desktop_settings_catalog.hpp"
#include <libintl.h>
#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#if __has_include("../../src/localization/shell_locale.hpp")
#include "../../src/localization/shell_locale.hpp"
#define HAS_LANGUAGE_FIX 1
#endif

namespace {
std::string environment(const char* key) {
#ifdef _WIN32
    char* raw{}; std::size_t size{};
    if(_dupenv_s(&raw,&size,key)!=0)return {};
    std::string value=raw?raw:""; std::free(raw);return value;
#else
    const auto* raw=std::getenv(key);return raw?raw:"";
#endif
}
}
int main(int argc, char** argv) {
    if(argc < 3) return 2;
    int failures=0;
    const auto check=[&](bool ok, const std::string& message) {
        if(!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };
    const auto apply=[&](const std::string& language) {
#ifdef HAS_LANGUAGE_FIX
        openxmb::localization::shell_locale::apply(language, argv[2]);
#else
        if(!setlocale(LC_ALL, language.c_str())) setlocale(LC_ALL, "");
        bindtextdomain("shell", argv[2]);
        bind_textdomain_codeset("shell", "UTF-8"); textdomain("shell");
#endif
    };
    const auto original_numeric=std::string(setlocale(LC_NUMERIC,nullptr));
#if defined(__linux__)
    // Prevent a developer machine's locale archive from masking the minimal-
    // distro case. The shell must find its packaged non-C message locale.
    const auto original_locpath=environment("LOCPATH");
    const auto unavailable=std::filesystem::path(argv[2])/"unavailable-native-locales";
    setenv("LOCPATH",unavailable.string().c_str(),1);
#endif
    for(const auto& [language, expected] : {
        std::pair{"de", "Einstellungen"}, {"fr", "Paramètres"},
        {"pl", "Ustawienia"}, {"hi", "सेटिंग्स"}, {"en", "Settings"},
        {"de", "Einstellungen"}, {"en", "Settings"}}) {
        apply(language);
        check(std::string(dgettext("shell","Settings"))==expected,
              std::string("live gettext switch to ")+language);
        check(std::string(dgettext(nullptr,"Settings"))==expected,
              std::string("i18n default domain switch to ")+language);
#ifdef HAS_LANGUAGE_FIX
        check(openxmb::localization::shell_locale::translate("").empty(),"empty labels must not render gettext metadata");
        check(openxmb::localization::shell_locale::translate("Theme Settings")==dgettext("shell","Theme Settings"),"exact catalog translation takes priority over legacy alias");
#endif
        auto loaded=openxmb::xmb::load_catalog_file(std::filesystem::path(argv[1])/"assets/xmb/catalog/en.json");
        if(!loaded) return 3;
        auto& catalog=*loaded.value;
        for(const auto& [key,text]:catalog.strings) if(!text.empty())
            check(dgettext("shell",text.c_str())!=text.c_str(),std::string("compiled catalog entry missing for ")+language+": "+key);
        check(catalog.text("category.settings.system.settings.system.name.value.default")=="OpenXMB system","default system name belongs to OpenXMB");
        for(const auto& [key,text]:catalog.strings)
            check(text.find("PS3™ system")==std::string::npos && text.find("PS3™ System")==std::string::npos,"product ownership wording: "+key);
        check(catalog.text("category.game.saved.data.utility.ps3tm.label")=="Saved Data Utility (PS3™)","legitimate console format name preserved");
#ifdef HAS_LANGUAGE_FIX
        app::desktop_settings::populate(catalog, openxmb::localization::shell_locale::translate);
#else
        app::desktop_settings::populate(catalog);
#endif
        check(catalog.text("category.settings.label")==expected, std::string("visible catalog switch to ")+language);
        const auto& node=catalog.nodes.at("category.settings.desktop.language");
        for(const auto& [code,label]:openxmb::localization::shell_locale::languages) {
            const auto choice=std::ranges::find(node.choices,code,&openxmb::xmb::CatalogChoice::value);
            check(choice!=node.choices.end(),"language choice is present: "+std::string(code));
            if(choice!=node.choices.end())check(catalog.text(choice->label_key)==
                (code=="auto"?std::string(dgettext("shell",std::string(label).c_str())):std::string(label)),
                "language choice preserves its native name: "+std::string(code));
        }
        check(node.choices.size()==13,"auto and all twelve authorized locale packs offered");
        for(const auto& [id,legacy]:catalog.nodes) if(id=="category.settings.system.settings.system.language") {
            check(legacy.action_id=="desktop.language","legacy language selector uses live settings path");
            check(legacy.choices.size()==13,"legacy selector only advertises shipped packs");
        }
        if(std::string(language)=="de") {
            check(catalog.text(node.label_key)=="Sprache","desktop setting label translated");
            check(std::string(dgettext("shell","Yes"))=="Ja","dialog translation uses same locale");
        }
    }
    apply("auto");
    check(environment("LANGUAGE").empty(),"auto removes explicit language override");
    check(std::string(setlocale(LC_NUMERIC,nullptr))==original_numeric,"language switching preserves numeric locale");
#if defined(__linux__)
    check(environment("LOCPATH")==unavailable.string(),"message locale loading preserves LOCPATH");
    if(original_locpath.empty()) unsetenv("LOCPATH");
    else setenv("LOCPATH",original_locpath.c_str(),1);
#endif
#ifdef _WIN32
    const auto old_all=environment("LC_ALL");
    const auto old_messages=environment("LC_MESSAGES");
    _putenv_s("LC_ALL","");
    _putenv_s("LC_MESSAGES","fr_FR.UTF-8");
    apply("de"); apply("auto");
    check(std::string(dgettext("shell","Settings"))=="Paramètres","auto restores system/environment message locale after explicit switch");
    check(environment("LC_MESSAGES")=="fr_FR.UTF-8","auto preserves user's locale environment");
    _putenv_s("LC_MESSAGES",old_messages.c_str());
    _putenv_s("LC_ALL",old_all.c_str());
#endif
#ifdef HAS_LANGUAGE_FIX
    check(openxmb::localization::shell_locale::normalize("en_GB")=="en","old English region value migrates");
    check(openxmb::localization::shell_locale::normalize("xx")=="en","unsupported saved language has deterministic fallback");
    check(openxmb::localization::shell_locale::normalize("zh-Hant-HK")=="zh_TW","traditional Chinese script selects correct catalog");
    check(openxmb::localization::shell_locale::normalize("zh-Hans-CN")=="zh_CN","simplified Chinese script selects correct catalog");
    apply("en");
    check(openxmb::localization::shell_locale::language_label("en")=="British English","English option is exactly British English");
    for(const auto* current:{"fr","ja","en"}) {
        apply(current);
        for(const auto& [code,native]:std::array<std::pair<const char*,const char*>,12>{{
            {"en","British English"},{"de","Deutsch"},{"fr","Français"},{"hi","हिन्दी"},
            {"pl","Polski"},{"id","Bahasa Indonesia"},{"es","Español"},{"ja","日本語"},
            {"zh_CN","简体中文"},{"zh_TW","繁體中文"},{"ko","한국어"},{"ru","Русский"}}})
            check(openxmb::localization::shell_locale::language_label(code)==native,
                std::string("language autonym changed with interface locale: ")+current+" / "+code);
    }
    if(argc>3) for(const auto* language:{"id","es","ja","zh_CN","zh_TW","ko","ru"}) {
        apply(language);
        auto loaded=openxmb::xmb::load_catalog_file(std::filesystem::path(argv[1])/"assets/xmb/catalog/en.json");
        if(!loaded) return 3;
        for(const auto& [key,text]:loaded.value->strings) if(!text.empty())
            check(dgettext("shell",text.c_str())!=text.c_str(),std::string("new pack catalog entry missing for ")+language+": "+key);
        check(std::string(dgettext("shell","Settings"))!="Settings",std::string("new pack changes actual Settings text: ")+language);
        check(std::string(dgettext("shell","Flow visualiser"))!="Flow visualiser",std::string("new pack translates Flow notice: ")+language);
        check(std::string(dgettext("shell","Wave visualiser"))!="Wave visualiser",std::string("new pack translates Wave notice: ")+language);
    }
#endif
    std::cout << "language switching failures: " << failures << '\n';
    return failures ? 1 : 0;
}
