module;

#include "openxmb/integrations/desktop.hpp"
#include "openxmb/integrations/disc.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>
#include <vector>

export module openxmb.app:integration_center;
import :component;
import :main;
import :text_entry;
import :xmb_presentation;
import auroreengine;
import sdl2;
import glm;
import openxmb.utils;
import openxmb.config;

export namespace programs {

// Native shell presentation for desktop integrations and the optional web surface.
class integration_center : public app::component, public action_receiver, public app::event_receiver {
public:
    float typography_scale() const override { return 2.5F; }

    integration_center(std::string section, auroreengine::resource_loader& loader)
        : section_(std::move(section)), preferences_(openxmb::integrations::load_preferences()) {
        if (section_ == "disc") section_ = "discs";
        if (section_ == "libretro" || section_ == "games") section_ = "emulation";
        if (section_ != "browser" && section_ != "emulation" && section_ != "discs" && section_ != "system") section_ = "browser";
        cores_ = openxmb::integrations::discover_libretro_cores();
        if (preferences_.core.empty() && !cores_.empty()) preferences_.core = cores_.front();
        if (section_ == "discs") discs_ = openxmb::integrations::discover_discs();
        if (section_ == "browser") {
            browser_icons_.load(loader,"theme/icon_browser");
            const auto homepage=browser_preference("browser.homepage","https://duckduckgo.com");
            if(openxmb::integrations::validate_browser_url(homepage)) preferences_.browser_url=homepage;
        }
        rebuild_rows();
    }
    ~integration_center() override {
        if(shell_&&section_=="browser") shell_->set_blur_background(previous_blur_);
        if (background_input_) sdl::SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "0");
        if (browser_.dedicated_window && browser_.process_id > 0)
            static_cast<void>(openxmb::integrations::close_browser(browser_));
        if (core_process_ > 0 && !openxmb::integrations::process_exit_status(core_process_))
            static_cast<void>(openxmb::integrations::stop_process(core_process_));
        for (const auto pid : media_processes_) if (!openxmb::integrations::process_exit_status(pid))
            static_cast<void>(openxmb::integrations::stop_process(pid));
    }
    result tick(app::shell* shell) override {
        shell_ = shell;
        const auto now = std::chrono::steady_clock::now();
        if(section_=="browser"&&!browser_started_) {
            browser_started_=true;
            if(!browser_blur_saved_) {previous_blur_=shell->get_blur_background();browser_blur_saved_=true;}
            shell->set_blur_background(true);
            const auto opened=openxmb::integrations::open_browser(preferences_.browser_url,browser_);message_=opened.message;
            browser_settings_signature_.clear();browser_clear_token_.clear();
            browser_edit_token_=0;
            if(opened&&browser_.dedicated_window) {sdl::SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS","1");background_input_=true;browser_controlling_=true;}
        }
        if(browser_.embedded_view&&now-page_updated_>std::chrono::milliseconds(200)) {
            page_=openxmb::integrations::browser_page(browser_);page_updated_=now;
            sync_browser_settings();
            if(!page_.error.empty()) message_=page_.error;
            else if(!page_.status.empty()) message_=page_.status;
            if(page_.address_requested&&!browser_editing_) browser_text(true);
            if(page_.options_requested&&!browser_editing_&&!browser_options_open_) toggle_browser_options();
            if(page_.edit_requested&&page_.edit_token!=browser_edit_token_&&!browser_editing_&&!browser_options_open_) browser_editable();
        }
        const auto elapsed = std::clamp(std::chrono::duration<double>(now - input_updated_).count(), 0.0, 0.1);
        input_updated_ = now;
        if (browser_controlling_) {
            using namespace openxmb::integrations;
            if (std::abs(pointer_axis_x_) > 0.2 || std::abs(pointer_axis_y_) > 0.2) {
                pointer_x_ = std::clamp(pointer_x_ + pointer_axis_x_ * elapsed * 0.8, 0.0, 1.0);
                pointer_y_ = std::clamp(pointer_y_ + pointer_axis_y_ * elapsed * 0.8, 0.0, 1.0);
                const auto moved = browser_input(browser_, {BrowserControl::pointer, pointer_x_, pointer_y_, {}});
                if (!moved) message_ = moved.message;
            }
            if (std::abs(scroll_axis_) > 0.3 && now - scrolled_ > std::chrono::milliseconds(100)) {
                const auto scrolled = browser_input(browser_, {BrowserControl::scroll, 0, scroll_axis_, {}});
                if (!scrolled) message_ = scrolled.message;
                scrolled_ = now;
            }
        }
        if (core_process_ > 0) {
            if (const auto status = openxmb::integrations::process_exit_status(core_process_)) {
                message_ = *status == 0 ? "Emulation session finished. Save RAM is stored automatically." : "The core session could not continue. Check the selected core/content and its BIOS requirements.";
                core_process_ = -1;
            }
        }
        if (browser_.process_id > 0) {
            if (const auto status = openxmb::integrations::process_exit_status(browser_.process_id); status) {
                if(browser_.embedded_view&&*status==0) return result::close;
                browser_.embedded_view=false;browser_controlling_=false;
                message_ = page_.error.empty()?"The browser exited unexpectedly. Reopen Internet Browser; check the WebView2 Runtime if this continues.":page_.error;
                browser_.process_id = -1;
                browser_.native_process.reset();
            }
        }
        return result::success;
    }
    result on_event(const app::event& event) override {
        // A remapped Options action owns the event before any raw key/button
        // handling. Tab must open the shell menu rather than advance page focus.
        if(section_=="browser"&&event.action==action::options&&!browser_editing_) return on_action(action::options);
        if(browser_options_open_) return browser_options_action(event.action);
        if (browser_controlling_) {
            if (const auto* axis = event.get<app::events::joystick_axis>()) {
                if (axis->index == app::events::logical_joystick_index::left) { pointer_axis_x_ = axis->x; pointer_axis_y_ = axis->y; }
                else scroll_axis_ = axis->y;
                return result::success;
            }
            if (const auto* button = event.get<app::events::controller_button_down>()) {
                using Button = app::events::logical_controller_button;
                using Control = openxmb::integrations::BrowserControl;
                if (button->button == Button::leftshoulder) return send_control(Control::reverse_tab);
                if (button->button == Button::rightshoulder) return send_control(Control::confirm);
                if (button->button == Button::start) { browser_text(true); return result::success; }
                if (button->button == Button::back) return send_control(Control::context_click);
                if (button->button == Button::a) { if (primary_down_) return result::success; primary_down_ = true; }
            }
            if (const auto* button = event.get<app::events::controller_button_up>(); button && button->button == app::events::logical_controller_button::a) {
                primary_down_ = false; return result::success;
            }
        }
        return on_action(event.action);
    }
    result on_action(action selected_action) override {
        if(section_=="browser"&&selected_action==action::options&&!browser_editing_) {toggle_browser_options();return result::success;}
        if(browser_options_open_) return browser_options_action(selected_action);
        if(section_=="browser"&&!browser_controlling_&&(selected_action==action::options||selected_action==action::ok)) {
            if(selected_action==action::options) {
                if(browser_.process_id>0) browser_text(true);
                else edit("Address",preferences_.browser_url,[this](std::string value) {
                    value=openxmb::integrations::browser_address_url(value,browser_preference("browser.search","duckduckgo"));
                    if(const auto valid=openxmb::integrations::validate_browser_url(value);!valid) {message_=valid.message;return;}
                    preferences_.browser_url=std::move(value);browser_started_=false;
                });
            } else {
                if(browser_.process_id>0) {browser_controlling_=true;return send_control(openxmb::integrations::BrowserControl::tab);}
                browser_started_=false;
            }
            return result::success;
        }
        if (browser_controlling_) {
            using Control = openxmb::integrations::BrowserControl;
            switch (selected_action) {
            case action::cancel: {
                if(browser_.embedded_view) {static_cast<void>(openxmb::integrations::close_browser(browser_));return result::close|result::back_sound;}
                const auto returned = openxmb::integrations::browser_input(browser_, {Control::return_to_shell});
                message_ = returned.message; browser_controlling_ = false; pointer_axis_x_ = pointer_axis_y_ = scroll_axis_ = 0;
                return result::success;
            }
            case action::ok: return send_control(Control::click);
            case action::up: return send_control(Control::scroll, -1);
            case action::down: return send_control(Control::scroll, 1);
            case action::left: return send_control(Control::back);
            case action::right: return send_control(Control::forward);
            case action::extra: return send_control(Control::tab);
            default: return result::success;
            }
        }
        if (selected_action == action::cancel) return result::close | result::back_sound;
        if (selected_action == action::up) { selected_ = selected_ == 0 ? rows_.size() - 1 : selected_ - 1; return result::success; }
        if (selected_action == action::down) { selected_ = (selected_ + 1) % rows_.size(); return result::success; }
        if (selected_action == action::left || selected_action == action::right) {
            if (section_ == "emulation" && selected_ == 0 && !cores_.empty()) {
                const auto found = std::ranges::find(cores_, preferences_.core);
                auto index = found == cores_.end() ? std::size_t{} : static_cast<std::size_t>(found - cores_.begin());
                index = selected_action == action::right ? (index + 1) % cores_.size() : (index + cores_.size() - 1) % cores_.size();
                preferences_.core = cores_[index]; persist(); rebuild_rows();
            }
            return result::success;
        }
        if (selected_action != action::ok || !shell_) return result::unsupported;
        using namespace openxmb::integrations;
        if (section_ == "browser") {
            if (selected_ == 0) edit("Web address", preferences_.browser_url, [this](std::string value) {
                if (const auto valid = validate_browser_url(value); !valid) { message_ = valid.message; return; }
                preferences_.browser_url = std::move(value); persist();
            });
            else if (selected_ == 1) {
                const auto opened = open_browser(preferences_.browser_url, browser_); message_ = opened.message;
                if (opened && browser_.dedicated_window) {
                    sdl::SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1"); background_input_ = true; browser_controlling_ = true;
                }
            } else if (selected_ == 2) {
                const auto focused = browser_input(browser_, {BrowserControl::pointer, pointer_x_, pointer_y_, {}});
                message_ = focused.message;
                if (focused) { sdl::SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1"); background_input_ = true; browser_controlling_ = true; }
            } else if (selected_ == 3) browser_text(false);
            else { message_ = close_browser(browser_).message; browser_controlling_ = false; }
        } else if (section_ == "emulation") {
            if (selected_ == 0) edit("Libretro core library", preferences_.core.string(), [this](std::string value) { preferences_.core = std::move(value); persist(); });
            else if (selected_ == 1) edit("Game/content file (empty for no-content cores)", preferences_.content.string(), [this](std::string value) { preferences_.content = std::move(value); persist(); });
            else if (selected_ == 2) {
                if (core_process_ > 0) message_ = "A core is already running. Return to its window or stop the session.";
                else {
                    const auto launched = launch_libretro(preferences_.core, preferences_.content);
                    message_ = launched ? "Core started in its own window. Escape returns here; F5 saves and F8 loads." : launched.message;
                    if (launched) core_process_ = launched.process_id;
                }
            } else if (selected_ == 3) {
                const auto stopped = stop_process(core_process_); message_ = stopped.message; core_process_ = -1;
            } else {
                cores_ = discover_libretro_cores(); message_ = std::to_string(cores_.size()) + " cores found. Left/Right cycles the core field.";
            }
        } else if (section_ == "discs") {
            if (selected_ == 0) { discs_ = discover_discs(); message_ = discs_.empty() ? "No audio CD, mounted DVD, or mounted Blu-ray was found." : std::to_string(discs_.size()) + " playable discs found."; }
            else if (selected_ == 1) edit("Mounted optical-disc directory or device", disc_path_, [this](std::string value) {
                disc_path_ = std::move(value);
                auto disc = probe_disc(disc_path_);
                if (disc.kind == DiscKind::absent || disc.kind == DiscKind::data) message_ = "No playable disc structure was found at that path.";
                else { discs_ = {std::move(disc)}; message_ = "Disc ready. Select it to play."; }
            });
            else if (selected_ - 2 < discs_.size()) {
                const auto launched = play_disc(discs_[selected_ - 2]); message_ = launched.message;
                if (launched) media_processes_.push_back(launched.process_id);
            }
        } else {
            if (selected_ == 0) {
                const auto now = std::chrono::system_clock::now().time_since_epoch();
                const auto file = user_data_directory() / "screenshots" / ("OpenXMB-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()) + ".png");
                message_ = capture_screenshot(file).message;
            } else {
                message_ = set_autostart(!autostart_enabled(), executable_path()).message;
            }
        }
        rebuild_rows();
        return result::success;
    }
    void render(auroreengine::gui_renderer& renderer, app::shell* shell) override {
        shell_ = shell;
        if(section_=="browser") {
            app::xmb_canvas c(renderer);
            c.rect(0,0,1920,74,{0,0,0,0.22F});c.rect(0,74,1920,2,{1,1,1,0.42F});
            c.rect(0,1004,1920,76,{1,1,1,0.12F});c.rect(0,1004,1920,2,{1,1,1,0.5F});
            browser_icons_.draw(c,"theme/icon_browser",53,38,42,0.85F);
            const auto title=page_.title.empty()?std::string("Internet Browser"):page_.title.substr(0,55);
            c.text(title,92,33,22,{1,1,1,0.92F});
            c.text(page_.url.empty()?preferences_.browser_url:page_.url.substr(0,125),92,63,20,{1,1,1,0.72F});
            if(page_.loading) c.text("◌",1815,48,32,{1,1,1,0.85F},app::xmb_canvas::pulse());
            const auto footer=!page_.error.empty()?page_.error:!page_.status.empty()?page_.status:page_.hover;
            c.text(footer.substr(0,140),70,1030,22,{1,1,1,0.8F});
            render_controller_buttons(shell,renderer,0.72F,0.965F,std::array{
                std::pair{action::options,std::string_view{"Options"}},std::pair{action::cancel,std::string_view{browser_options_open_?"Close menu":"Return"}}});
            if(browser_options_open_) render_browser_options(c);
            if(browser_info_) {
                c.text("Page Information",44,180,26,{1,1,1,0.9F});c.rect(44,200,320,1,{1,1,1,0.4F});
                c.text("Title",44,245,20,{1,1,1,0.55F});c.text(page_.title.substr(0,26),44,280,22);
                c.text("Address",44,335,20,{1,1,1,0.55F});
                for(std::size_t line=0;line<7&&line*27<page_.url.size();++line) c.text(page_.url.substr(line*27,27),44,370+line*30,19);
                c.text(page_.loading?"Loading page…":"Page loaded",44,650,22,{1,1,1,0.75F});
            }
            if(!browser_.embedded_view) {
                c.text(message_.empty()?"Opening Internet Browser…":message_,960,450,26,{1,1,1,1},0,true);
                render_controller_buttons(shell,renderer,0.5F,0.9F,std::array{std::pair{action::ok,std::string_view{"Open / Resume"}},std::pair{action::options,std::string_view{"Address"}},std::pair{action::cancel,std::string_view{"Back"}}});
            }
            return;
        }
        const std::string title = section_ == "browser" ? "Internet Browser" : section_ == "emulation" ? "Emulation" : section_ == "discs" ? "Optical Media" : "Desktop Tools";
        renderer.draw_rect({0.08F, 0.13F}, {0.84F, 0.74F}, {0.025F, 0.035F, 0.055F, 0.94F});
        renderer.draw_text(title, 0.12F, 0.20F, 0.055F);
        const auto description = section_ == "browser" ? "Browse in a separate system-browser window with your own keyboard and mouse."
            : section_ == "emulation" ? "Native libretro software cores. Provide your own core, content and BIOS files."
            : section_ == "discs" ? "Audio CDs, DVD and Blu-ray folders/devices. Playback uses an installed desktop player."
            : "Capture your desktop or start OpenXMB automatically when you log in.";
        renderer.draw_text(description, 0.12F, 0.27F, 0.026F, {0.75F, 0.81F, 0.89F, 1.0F});
        const auto first = selected_ > 5 ? selected_ - 5 : 0;
        for (std::size_t index = first; index < rows_.size() && index < first + 6; ++index) {
            const float y = 0.37F + static_cast<float>(index - first) * 0.061F;
            if (index == selected_) renderer.draw_rect({0.11F, y - 0.022F}, {0.77F, 0.055F}, {0.20F, 0.36F, 0.54F, 0.8F});
            const auto shown = rows_[index].size() > 88 ? rows_[index].substr(0, 40) + "..." + rows_[index].substr(rows_[index].size() - 45) : rows_[index];
            renderer.draw_text(shown, 0.13F, y, 0.032F);
        }
        if (section_ == "browser") renderer.draw_text("Stick: pointer  A: click  X: Tab  Y: text  Start: address  Shoulder: Enter  B: return", 0.12F, 0.75F, 0.021F);
        if (section_ == "emulation") renderer.draw_text("Arrows + Z/X/A/S: play   Space: pause   F5/F8: save/load   F12: screenshot", 0.12F, 0.75F, 0.024F);
        if (!message_.empty()) {
            const auto shown = message_.size() > 125 ? message_.substr(0, 122) + "..." : message_;
            renderer.draw_text(shown, 0.12F, 0.80F, 0.024F, {0.70F, 0.88F, 1.0F, 1.0F});
        }
        render_controller_buttons(shell, renderer, 0.5F, 0.93F, std::array{
            std::pair{action::ok, std::string_view{"Select / Edit"}},
            std::pair{action::cancel, std::string_view{"Back"}}});
    }
    bool is_opaque() const override { return section_=="browser"; }
private:
    void toggle_browser_options() {
        if(browser_options_open_) {close_browser_options();return;}
        browser_options_open_=true;browser_option_=0;browser_controlling_=false;
        pointer_axis_x_=pointer_axis_y_=scroll_axis_=0;
        if(browser_.embedded_view) {
#ifdef _WIN32
            dismiss_browser_edit_request();
            static_cast<void>(openxmb::integrations::browser_view_command(browser_,"menu"));
#else
            static_cast<void>(openxmb::integrations::browser_view_command(browser_,"hide"));
            static_cast<void>(openxmb::integrations::browser_input(browser_,{openxmb::integrations::BrowserControl::return_to_shell}));
#endif
        }
    }
    void close_browser_options(bool resume=true) {
        browser_options_open_=false;browser_controlling_=browser_.process_id>0;
        if(resume&&browser_.embedded_view) static_cast<void>(openxmb::integrations::browser_view_command(browser_,"show"));
    }
    result browser_options_action(action selected) {
        if(selected==action::cancel||selected==action::options||selected==action::left) {close_browser_options();return result::success;}
        if(selected==action::up) browser_option_=(browser_option_+8)%9;
        if(selected==action::down) browser_option_=(browser_option_+1)%9;
        if(selected!=action::ok) return result::success;
        using namespace openxmb::integrations;
        const auto option=browser_option_;
#ifdef _WIN32
        if((option==0&&!page_.back)||(option==1&&!page_.forward)) return result::success;
#endif
        close_browser_options(option!=4&&option!=5&&option!=6);
        if(option==0||option==1||option==2) {
            const auto sent=browser_view_command(browser_,option==0?"back":option==1?"forward":"reload");message_=sent.message;
        } else if(option==3) {
            const auto sent=browser_view_command(browser_,"navigate",browser_preference("browser.homepage","https://duckduckgo.com"));message_=sent.message;
        } else if(option==4||option==6) browser_text(option==4);
        else if(option==5) {
            browser_editing_=true;browser_controlling_=false;
            static_cast<void>(browser_view_command(browser_,"hide"));
            shell_->emplace_overlay<app::text_entry_overlay>("Internet Search",std::string{},[this](std::string query) {
                const auto url=browser_address_url(query,browser_preference("browser.search","DuckDuckGo"));
                const auto sent=browser_view_command(browser_,"navigate",url);message_=sent.message;
                if(!sent) static_cast<void>(browser_view_command(browser_,"show"));
                browser_editing_=false;browser_controlling_=true;
            },[this] {static_cast<void>(browser_view_command(browser_,"show"));browser_editing_=false;browser_controlling_=true;});
        } else if(option==7) {browser_info_=!browser_info_;}
        else if(option==8) {static_cast<void>(close_browser(browser_));return result::close;}
        return result::success;
    }
    void render_browser_options(const app::xmb_canvas& c) {
        // The native viewport ends at x=1508. Keep the XMB side menu in the
        // shell's right margin; suspend child input while retaining its page.
        c.gradient(1514,76,406,928,{0.08F,0.08F,0.08F,0.85F},{0.02F,0.02F,0.02F,0.94F});
        c.rect(1514,76,1,928,{1,1,1,0.3F});
        c.text("Internet Browser",1550,151,25,{1,1,1,0.72F});c.rect(1544,178,330,1,{1,1,1,0.3F});
        constexpr std::array<std::string_view,9> labels{"Back","Forward","Refresh","Home","Enter Address","Internet Search","Enter Text","Page Information","Exit"};
        for(std::size_t i=0;i<labels.size();++i) {
            const float y=245+static_cast<float>(i)*68;
            const bool focused=i==browser_option_;
            const bool unavailable=(i==0&&!page_.back)||(i==1&&!page_.forward);
            c.text(labels[i],1570,y,focused?29:26,{1,1,1,unavailable?0.3F:focused?1.0F:0.65F},focused&&!unavailable?app::xmb_canvas::pulse():0);
            if(focused) {c.text("›",1536,y,30,{1,1,1,0.95F},app::xmb_canvas::pulse());c.gradient(1550,y+12,305,2,{1,1,1,0.35F},{1,1,1,0});}
        }
    }
    static std::string browser_preference(const std::string& key,const std::string& fallback={}) {
        const auto& values=config::CONFIG.preferences;
        const auto found=values.find(key);
        return found==values.end()?fallback:found->second;
    }
    void sync_browser_settings() {
#ifdef _WIN32
        const auto zoom=browser_preference("browser.zoom","1.0");
        const auto downloads=browser_preference("browser.downloads");
        const auto options_key=browser_preference("key.options","43");
        const auto osk=browser_preference("input.osk","On");
        const auto signature=zoom+"\n"+downloads+"\n"+options_key+"\n"+osk;
        if(signature!=browser_settings_signature_) {
            try {
                std::size_t used{};const double factor=std::stod(zoom,&used);
                if(used!=zoom.size()||!std::isfinite(factor)||factor<0.25||factor>5) throw std::runtime_error("Zoom must be between 0.25 and 5.0");
                const auto sent=openxmb::integrations::browser_view_command(browser_,"configure",nlohmann::json{{"zoom",factor*100},{"downloads",downloads},{"options_scancode",std::stoi(options_key)},{"osk",osk=="On"}}.dump());
                if(sent) browser_settings_signature_=signature;else message_=sent.message;
            } catch(const std::exception& error) {message_=error.what();}
        }
        const auto clear=browser_preference("browser.clear_site_data");
        if(!clear.empty()&&clear!=browser_clear_token_) {
            const auto sent=openxmb::integrations::browser_view_command(browser_,"clear_site_data",clear);
            if(sent) browser_clear_token_=clear;else message_=sent.message;
        }
#endif
    }
    result send_control(openxmb::integrations::BrowserControl control, double scroll = 0) {
        const auto sent = openxmb::integrations::browser_input(browser_, {control, pointer_x_, scroll, {}});
        if (!sent) message_ = sent.message;
        return result::success;
    }
    void browser_editable() {
        using namespace openxmb::integrations;
        const auto token=page_.edit_token;browser_edit_token_=token;
        if(!config::CONFIG.enabled("input.osk")) {
            static_cast<void>(browser_view_command(browser_,"edit",nlohmann::json{{"token",token},{"accepted",false}}.dump()));return;
        }
        if(!shell_||browser_.process_id<=0) return;
        browser_editing_=true;browser_controlling_=false;primary_down_=false;pointer_axis_x_=pointer_axis_y_=scroll_axis_=0;
        static_cast<void>(browser_view_command(browser_,"hide"));
        static_cast<void>(browser_input(browser_,{BrowserControl::return_to_shell}));
        const auto initial=page_.edit_password?std::string{}:page_.edit_value;
        const auto title=page_.edit_password?std::string("Password"):page_.edit_label.empty()?std::string("Text"):page_.edit_label;
        shell_->emplace_overlay<app::text_entry_overlay>(title,initial,
            [this,token](std::string text) {finish_browser_edit(token,true,std::move(text));},
            [this,token] {finish_browser_edit(token,false,{});});
    }
    void finish_browser_edit(std::uint64_t token,bool accepted,std::string text) {
        using namespace openxmb::integrations;
        const auto result=browser_view_command(browser_,"edit",nlohmann::json{{"token",token},{"accepted",accepted},{"text",std::move(text)}}.dump());
        message_=result.message;
        static_cast<void>(browser_view_command(browser_,"show"));
        browser_editing_=false;browser_controlling_=true;primary_down_=false;
    }
    void dismiss_browser_edit_request() {
        if(!page_.edit_requested) return;
        browser_edit_token_=page_.edit_token;
        static_cast<void>(openxmb::integrations::browser_view_command(browser_,"edit",nlohmann::json{{"token",page_.edit_token},{"accepted",false}}.dump()));
    }
    void browser_text(bool address) {
        using namespace openxmb::integrations;
        if (!shell_ || browser_.process_id <= 0 || !browser_.dedicated_window) { message_ = "Open a dedicated browser window first."; return; }
        if(browser_.embedded_view) {
            if(!address&&page_.edit_requested) {browser_editable();return;}
            if(address) dismiss_browser_edit_request();
            browser_editing_=true;browser_controlling_=false;
            static_cast<void>(browser_view_command(browser_,"hide"));
            static_cast<void>(browser_input(browser_,{BrowserControl::return_to_shell}));
            shell_->emplace_overlay<app::text_entry_overlay>(address?"Address":"Text",address?(page_.url.empty()?preferences_.browser_url:page_.url):std::string{},
                [this,address](std::string value) {
                    if(address) value=browser_address_url(value,browser_preference("browser.search","duckduckgo"));
                    const auto result=openxmb::integrations::browser_view_command(browser_,address?"navigate":"text",value);message_=result.message;
                    if(address&&result) preferences_.browser_url=value;
                    if(!result) static_cast<void>(openxmb::integrations::browser_view_command(browser_,"show"));browser_editing_=false;browser_controlling_=true;
                },[this] {static_cast<void>(openxmb::integrations::browser_view_command(browser_,"show"));browser_editing_=false;browser_controlling_=true;});
            return;
        }
        const auto returned = browser_input(browser_, {BrowserControl::return_to_shell});
        if (!returned) { message_ = returned.message; return; }
        browser_controlling_ = false; pointer_axis_x_ = pointer_axis_y_ = scroll_axis_ = 0;
        edit(address ? "Browser address" : "Text for the focused browser field", address ? preferences_.browser_url : std::string{}, [this, address](std::string value) {
            if (address) {
                const auto valid = validate_browser_url(value);
                if (!valid) { message_ = valid.message; return; }
                const auto selected = browser_input(browser_, {BrowserControl::address_bar});
                if (!selected) { message_ = selected.message; return; }
                preferences_.browser_url = value; persist();
            }
            const auto typed = browser_input(browser_, {BrowserControl::text, 0, 0, std::move(value)});
            message_ = typed.message;
            if (typed) {
                if (address) message_ = browser_input(browser_, {BrowserControl::confirm}).message;
                browser_controlling_ = true;
            }
        });
    }
    void persist() {
        const auto saved = openxmb::integrations::save_preferences(preferences_);
        message_ = saved.message; rebuild_rows();
    }
    void edit(std::string title, std::string value, std::function<void(std::string)> accept) {
        shell_->push_overlay(std::make_unique<app::text_entry_overlay>(std::move(title), std::move(value), [this, accept = std::move(accept)](std::string changed) {
            accept(std::move(changed)); rebuild_rows();
        }));
    }
    void rebuild_rows() {
        using namespace openxmb::integrations;
        rows_.clear();
        if (section_ == "browser") rows_ = {"Address: " + preferences_.browser_url, "Open browser window", "Control browser with gamepad", "Type into browser field...", "Close dedicated browser session"};
        else if (section_ == "emulation") rows_ = {"Core: " + (preferences_.core.empty() ? std::string{"Select a core library"} : preferences_.core.string()), "Content: " + (preferences_.content.empty() ? std::string{"None (no-content core)"} : preferences_.content.string()), "Start emulation", "Stop emulation", "Refresh installed cores (Left/Right cycles core)"};
        else if (section_ == "discs") {
            rows_ = {"Refresh optical media", "Open disc directory/device..."};
            for (const auto& disc : discs_) rows_.push_back(std::string{disc_kind_name(disc.kind)} + ": " + disc.label);
        } else rows_ = {"Take screenshot", std::string{"Start OpenXMB at login: "} + (autostart_enabled() ? "On" : "Off")};
        selected_ = std::min(selected_, rows_.size() - 1);
    }
    std::string section_, message_, disc_path_;
    openxmb::integrations::Preferences preferences_;
    openxmb::integrations::BrowserSession browser_;
    std::vector<std::filesystem::path> cores_;
    std::vector<openxmb::integrations::DiscInfo> discs_;
    std::vector<std::int64_t> media_processes_;
    std::vector<std::string> rows_;
    std::size_t selected_{};
    std::int64_t core_process_{-1};
    app::shell* shell_{};
    bool browser_controlling_{},background_input_{},primary_down_{},browser_started_{},browser_editing_{},previous_blur_{},browser_blur_saved_{},browser_options_open_{},browser_info_{};
    std::size_t browser_option_{};
    std::uint64_t browser_edit_token_{};
    app::media_icons browser_icons_;
    std::string browser_settings_signature_,browser_clear_token_;
    openxmb::integrations::BrowserPage page_;
    std::chrono::steady_clock::time_point page_updated_{};
    double pointer_x_{0.5}, pointer_y_{0.5}, pointer_axis_x_{}, pointer_axis_y_{}, scroll_axis_{};
    std::chrono::steady_clock::time_point input_updated_{std::chrono::steady_clock::now()}, scrolled_{};
};
} // namespace programs
