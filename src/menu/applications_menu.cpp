module;

#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

module shell.app;

import spdlog;
import glibmm;
import giomm;
import i18n;
import dreamrender;
import shell.config;

import :applications_menu;
import :choice_overlay;
import :message_overlay;

namespace menu {

using namespace mfk::i18n::literals;

applications_menu::applications_menu(std::string name, dreamrender::texture&& icon, app::shell* xmb, dreamrender::resource_loader& loader, AppFilter filter)
    : simple_menu(std::move(name), std::move(icon)), xmb(xmb), loader(loader), filter(filter)
{
    auto appInfos = Gio::AppInfo::get_all();
    for (const auto& app : appInfos) {
        if(!filter(app.get()))
            continue;
        bool is_hidden = config::CONFIG.excludedApplications.contains(app->get_id());
        if(!show_hidden && is_hidden)
            continue;

        spdlog::trace("Found application: {} ({})", app->get_display_name(), app->get_id());
        auto entry = create_action_menu_entry(app, is_hidden);
        apps.push_back(app);
        entries.push_back(std::move(entry));
    }
}

std::unique_ptr<action_menu_entry> applications_menu::create_action_menu_entry(Glib::RefPtr<Gio::AppInfo> app, bool hidden) {
    std::string icon_path;
    if(auto r = utils::resolve_icon(app->get_icon().get())) {
        icon_path = r->string();
    } else {
        spdlog::warn("Could not resolve icon for application: {}", app->get_display_name());
    }

    dreamrender::texture icon_texture(loader.getDevice(), loader.getAllocator());
    std::string name = app->get_display_name();
    if(hidden) {
        name += " (hidden)"_();
    }
    auto entry = std::make_unique<action_menu_entry>(name, std::move(icon_texture),
        std::function<result()>{}, [this, app](auto && PH1) { return activate_app(app, std::forward<decltype(PH1)>(PH1)); });
    if(!icon_path.empty()) {
        loader.loadTexture(&entry->get_icon(), icon_path);
    }
    return entry;
}

void applications_menu::reload() {
    auto appInfos = Gio::AppInfo::get_all();
    for (const auto& app : appInfos) {
        bool is_hidden = config::CONFIG.excludedApplications.contains(app->get_id());
        bool should_include = filter(app.get()) && (show_hidden || !is_hidden);
        spdlog::trace("Found application: {} ({})", app->get_display_name(), app->get_id());
        auto it = std::ranges::find_if(apps, [&app](const Glib::RefPtr<Gio::AppInfo>& e){
            return app->get_id() == e->get_id();
        });
        if(it == apps.end()) {
            if(should_include) {
                spdlog::trace("Found new application: {} ({})", app->get_display_name(), app->get_id());
                auto entry = create_action_menu_entry(app, is_hidden);
                apps.push_back(app);
                entries.push_back(std::move(entry));
            }
        } else {
            if(!should_include) {
                spdlog::trace("Removing application: {} ({})", app->get_display_name(), app->get_id());
                auto index = std::distance(apps.begin(), it);
                apps.erase(it);
                entries.erase(entries.begin() + index);
            }
        }
    }
}

result applications_menu::activate_app(Glib::RefPtr<Gio::AppInfo> app, action action) {
    if(action == action::ok) {
        return app->launch(std::vector<Glib::RefPtr<Gio::File>>()) ? result::success : result::failure;
    } else if(action == action::options) {
        bool hidden = config::CONFIG.excludedApplications.contains(app->get_id());
        xmb->emplace_overlay<app::choice_overlay>(std::vector{
            "Launch Application"_(), "View information"_(), hidden ? "Show in XMB"_() : "Hide from XMB"_()
        }, 0, [this, app, hidden](unsigned int index){
            switch(index) {
                case 0:
                    app->launch(std::vector<Glib::RefPtr<Gio::File>>());
                    return;
                case 1: {
                    std::string info{};
                    info += "appinfo|ID: "_() + app->get_id() + "\n";
                    info += "appinfo|Name: "_() + app->get_name() + "\n";
                    info += "appinfo|Display Name: "_() + app->get_display_name() + "\n";
                    info += "appinfo|Description: "_() + app->get_description() + "\n";
                    info += "appinfo|Executable: "_() + app->get_executable() + "\n";
                    info += "appinfo|Command Line: "_() + app->get_commandline() + "\n";
                    info += "appinfo|Icon: "_() + app->get_icon()->to_string() + "\n";
#if __linux__
                    if(auto desktop_app = dynamic_cast<Gio::DesktopAppInfo*>(app.get())) {
                        info += "appinfo|Categories: "_() + desktop_app->get_categories() + "\n";
                    }
#endif
                    xmb->emplace_overlay<app::message_overlay>("Application Information"_(), info, std::vector<std::string>{"OK"_()}, [](unsigned int){}, false);
                    return;
                    }
                case 2:
                    if(!hidden) {
                        xmb->emplace_overlay<app::message_overlay>("Hide Application"_(),
                            "Are you sure you want to hide this application from XMB Shell?"_(),
                            std::vector<std::string>{"Yes"_(), "No"_()}, [this, app](unsigned int index){
                            if(index == 0) {
                                config::CONFIG.excludeApplication(app->get_id());
                                reload();
                            }
                        }, true);
                    } else {
                        config::CONFIG.excludeApplication(app->get_id(), false);
                        reload();
                    }
                    return;
            }
        });
        return result::success;
    }
    return result::unsupported;
}

result applications_menu::activate(action action) {
    if(action == action::extra) {
        show_hidden = !show_hidden;
        reload();
        return result::success;
    }
    return simple_menu::activate(action);
}

void applications_menu::get_button_actions(std::vector<std::pair<action, std::string>>& v) {
    v.emplace_back(action::none, "");
    v.emplace_back(action::none, "");
    v.emplace_back(action::options, "Options"_());
    v.emplace_back(action::extra, show_hidden ? "Hide excluded apps"_() : "Show excluded apps"_());
}

} // namespace menu