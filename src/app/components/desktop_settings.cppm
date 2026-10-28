module;
#include "../../localization/ui_literal_text.hpp"
#include "openxmb/xmb/catalog.hpp"
#include "../../localization/shell_locale.hpp"
#include "../input_bindings.hpp"
#include "openxmb/music/player.hpp"
#include "openxmb/desktop/library.hpp"
#include "openxmb/themes/library.hpp"
#include "../../desktop/log_path.hpp"
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_image.h>
#include <cctype>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
#include <nlohmann/json.hpp>
#include "openxmb/integrations/desktop.hpp"
#include <SDL2/SDL.h>
#undef main
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cstdlib>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif
export module openxmb.app:desktop_settings;
import i18n;
import auroreengine;
import vulkan_hpp;
import openxmb.config;
import :component;
import :main;
import :choice_overlay;
import :message_overlay;
import :text_entry;
import :xmb_presentation;

namespace app {
export class binding_capture final : public component,public event_receiver {
public:
    explicit binding_capture(std::string key) : key_(std::move(key)) {}
    float typography_scale() const override {return 2.5F;}
    result on_event(const event& input) override {
        if(input.action==action::cancel) return result::close|result::back_sound;

        if(!armed_) return result::success;
        int code=-1;
        if(key_.starts_with("pad.")) {if(const auto* button=input.get<events::controller_button_down>())code=static_cast<int>(button->button);}
        else if(const auto* key=input.get<events::key_down>())code=static_cast<int>(key->keycode);
        if(code<0) return result::success;
        input_bindings::assign(config::CONFIG.preferences,key_,code);config::CONFIG.save_config();if(host_)host_->reload_button_icons();
        return result::close|result::confirm_sound;
    }
    result tick(shell* host) override {
        host_=host;
        if(!armed_) {
            int count{};const auto* keys=SDL_GetKeyboardState(&count);bool pressed=false;
            for(int i=0;i<count;++i)pressed=pressed||keys[i]!=0;
            for(const auto& [id,controller]:host->get_window()->controllers)for(int i=0;i<SDL_CONTROLLER_BUTTON_MAX;++i)pressed=pressed||SDL_GameControllerGetButton(controller.get(),static_cast<SDL_GameControllerButton>(i))!=0;
            armed_=!pressed;
        }
        return result::success;
    }
    void render(auroreengine::gui_renderer& r,shell* shell) override {
        xmb_canvas c(r);c.rect(0,0,1920,1080,{0,0,0,0.28F});
        c.rect(0,199,1920,1,{1,1,1,0.75F});c.rect(0,879,1920,1,{1,1,1,0.75F});
        c.text("Change Binding",960,350,36,{1,1,1,1},0,true);
        c.text("Release the controls, then press a "+std::string(key_.starts_with("pad.")?"controller button":"keyboard key")+" for "+key_.substr(4),960,500,28,{1,1,1,1},0,true);
        shell->render_controller_buttons(r,0.5F,0.81F,std::array{std::pair{action::cancel,"Cancel"}});
    }
private:
    std::string key_;bool armed_{};shell* host_{};
};
}

namespace app::desktop_settings {
using namespace mfk::i18n::literals;
export std::string value(const std::string& key,shell* host=nullptr) {
    const auto& c=config::CONFIG;
    const auto hex=[](const auto& color) {std::string value="#000000";constexpr char digits[]="0123456789abcdef";for(int i=0;i<3;++i){const auto byte=std::clamp(static_cast<int>(color[i]*255+0.5F),0,255);value[1+i*2]=digits[byte>>4];value[2+i*2]=digits[byte&15];}return value;};
    if(key=="theme.icon.color")return c.iconColorAuto?"auto":hex(c.iconColor);
    if(key=="theme.text.color")return hex(c.textColor);
    if(key=="theme.particle.color")return hex(c.particleColor);
    if(host&&!host->get_window()->config.headless) {
        if(key=="display.mode") {const auto flags=SDL_GetWindowFlags(host->get_window()->win.get());return (flags&SDL_WINDOW_FULLSCREEN_DESKTOP)==SDL_WINDOW_FULLSCREEN_DESKTOP?"Borderless":(flags&SDL_WINDOW_FULLSCREEN)?"Fullscreen":"Windowed";}
        if(key=="display.monitor")return std::to_string(SDL_GetWindowDisplayIndex(host->get_window()->win.get()));
        if(key=="display.resolution") {int w{},h{};SDL_GetWindowSize(host->get_window()->win.get(),&w,&h);return std::to_string(w)+"x"+std::to_string(h);}
        if(key=="audio.device") {const auto name=host->audio_device_name();return name.empty()?"System Default":name;}
    }
    if(key=="language") return c.language;
    if(key=="text.scale") return std::to_string(c.textScale).substr(0,3);
    if(key=="vsync") return c.preferredPresentMode==vk::PresentModeKHR::eFifoRelaxed?"On":"Off";
    if(key=="fps") return c.maxFPS>10000?"Unlimited":std::to_string(static_cast<int>(c.maxFPS));
    if(key=="samples") return std::to_string(static_cast<int>(c.sampleCount));
    if(key=="glass") return c.iconGlassRefraction?"On":"Off";
    if(key=="theme.waves") return c.wavesEnabled?"On":"Off";
    if(key=="theme.particles") return c.particlesEnabled?"On":"Off";
    if(key=="theme.icon.style") return c.iconStyle;
    if(key=="theme.font") return c.fontPath.string();
    if(key=="controller.type") return c.controllerType;
    if(key=="controller.rumble") return c.controllerRumble?"On":"Off";
    if(key=="controller.analog") return c.controllerAnalogStick?"On":"Off";
    if(key=="pictures.folder") return c.pictures_path().string();
    if(key=="music.folder") return c.music_path().string();
    if(key=="videos.folder") return c.videos_path().string();
    if(key=="debug.fps") return c.showFPS?"On":"Off";
    if(key=="debug.memory") return c.showMemory?"On":"Off";
    if(key=="about") return "OpenXMB " OPENXMB_VERSION_STRING;
    if(key=="system.name") return c.preference(key,"OpenXMB system");
    if(key=="autostart") return c.autostart?"On":"Off";
    const auto saved=c.preference(key);if(!saved.empty()) return saved;
    if(key=="audio.device") return "System Default";
    if(key=="browser.homepage") return "https://duckduckgo.com";
    if(key=="browser.downloads") return (openxmb::integrations::user_data_directory()/"downloads").string();
    if(key=="games.folder") return (openxmb::integrations::user_data_directory()/"games").string();
    if(key.starts_with("audio.") && key!="audio.startup" && key!="audio.background" && key!="audio.duck") return "100";
    if(key=="music.visualizer") return "Wave";
    if(key=="music.group") return "Folders";
    if(key=="music.sort") return "Name";
    if(key=="video.hardware") return "auto";
    if(key=="photo.duration") return "5";
    if(key=="photo.transition") return "Fade";
    if(key=="photo.fit"||key=="video.fit") return "Fit";
    if(key=="video.subtitle.size"||key=="display.brightness"||key=="browser.zoom") return "1.0";
    if(key=="video.audio.language") return "Default";
    if(key=="video.subtitle.language") return "Off";
    if(key=="time.offset") return "0";
    if(key=="time.format") return "24-hour";
    if(key=="date.format") return "DD/MM/YYYY";
    if(key=="input.repeat") return "180";
    if(key=="browser.search") return "DuckDuckGo";
    if(key=="display.mode") return "Windowed";
    if(key=="games.launch") return "Keep Running";
    if(key=="power.background.fps") return "30";
    if(key=="power.dim"||key=="music.shuffle"||key=="music.repeat"||key=="photo.shuffle"||key=="time.seconds"||key=="power.reduced.motion") return "Off";
    if(key=="startup.intro"||key=="startup.warning"||key=="audio.startup"||key=="audio.background"||key=="audio.duck"||key=="music.background"||key=="music.artwork"||key=="video.resume"||key=="photo.repeat"||key=="input.osk"||key=="power.keep.awake") return "On";
    return {};
}
export std::string display_value(const std::string& key,shell* host=nullptr) {
    const auto raw=value(key,host);
    return key=="language" ? openxmb::localization::shell_locale::language_label(raw) :
        openxmb::localization::shell_locale::translate(raw);
}
void show_error(shell* shell,const std::string& message) {
    shell->emplace_overlay<message_overlay>("Settings",message);
}
void apply(shell* shell,const std::string& key,const std::string& selected) {
    auto& c=config::CONFIG;auto* window=shell->get_window();
    if(key=="theme.icon.color"||key=="theme.text.color"||key=="theme.particle.color") {
        const bool automatic=key=="theme.icon.color"&&(selected=="auto"||selected=="original");
        const bool hex=selected.size()==7&&selected.front()=='#'&&std::all_of(selected.begin()+1,selected.end(),[](unsigned char c){return std::isxdigit(c)!=0;});
        if(!automatic&&!hex){show_error(shell,"Use a six-digit hexadecimal colour such as #ffeefc.");return;}
    }
    if(key=="browser.homepage") {const auto checked=openxmb::integrations::validate_browser_url(selected);if(!checked){show_error(shell,checked.message);return;}}
    if(key=="system.name") SDL_SetWindowTitle(window->win.get(),selected.c_str());
    if(key=="language") {c.setLanguage(selected);c.save_config();return;}
    if(key=="text.scale") {c.textScale=std::stod(selected);shell->reload_fonts();}
    else if(key=="vsync") c.setVSync(selected=="On");
    else if(key=="fps") {c.setMaxFPS(selected=="Unlimited"?0:std::stod(selected));window->config.fpsLimit=selected=="Unlimited"?0:std::stod(selected);}
    else if(key=="samples") {
        const auto samples=static_cast<vk::SampleCountFlagBits>(std::stoi(selected));
        const auto supported=window->physicalDevice.getProperties().limits.framebufferColorSampleCounts;
        if(!(supported&samples)) {show_error(shell,"This sample count is unavailable on the active GPU.");return;}
        window->waitIdle();c.setSampleCount(samples);window->config.sampleCount=samples;window->recreateSwapchain();
    }
    else if(key=="theme.waves") c.setWavesEnabled(selected=="On");
    else if(key=="theme.particles") c.setParticlesEnabled(selected=="On");
    else if(key=="theme.icon.style") c.setIconStyle(selected);
    else if(key=="theme.icon.color") c.setIconColor(selected);
    else if(key=="theme.particle.color") c.setParticleColor(selected);
    else if(key=="theme.text.color") c.setTextColor(selected);
    else if(key=="theme.font") {if(!std::filesystem::is_regular_file(std::filesystem::u8path(selected))) {show_error(shell,"Font file not found.");return;}shell->set_font_file(selected);}
    else if(key=="theme.wallpaper") {if(!std::filesystem::is_regular_file(std::filesystem::u8path(selected))) {show_error(shell,"Background file not found.");return;}auto ext=std::filesystem::u8path(selected).extension().string();std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        const std::array<std::string_view,13> videos{".mp4",".mkv",".webm",".mov",".m4v",".avi",".wmv",".mpg",".mpeg",".ts",".m2ts",".vob",".ogv"};
        const bool video=std::find(videos.begin(),videos.end(),ext)!=videos.end();
        if(video) {AVFormatContext* context{};const bool opened=avformat_open_input(&context,selected.c_str(),nullptr,nullptr)>=0;const bool valid=opened&&avformat_find_stream_info(context,nullptr)>=0&&av_find_best_stream(context,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0)>=0;avformat_close_input(&context);if(!valid){show_error(shell,"This video cannot be opened.");return;}}
        else {auto* image=IMG_Load(selected.c_str());if(!image){show_error(shell,"This file is not a supported image or video.");return;}SDL_FreeSurface(image);}
        c.setBackgroundImage(selected);c.setBackgroundType(video?config::config::background_type::video:config::config::background_type::image);}
    else if(key=="theme.sounds") {const auto folder=std::filesystem::u8path(selected);if(!std::filesystem::is_directory(folder)) {show_error(shell,"Sound folder not found.");return;}for(const auto* name:{"ok.wav","confirm.wav","cancel.wav","back.wav"}) {auto* sound=Mix_LoadWAV((folder/name).string().c_str());if(!sound){show_error(shell,std::string("Sound set needs a playable ")+name);return;}Mix_FreeChunk(sound);}}
    else if(key=="glass") c.setIconGlassRefraction(selected=="On");
    else if(key=="controller.type") c.setControllerType(selected);
    else if(key=="controller.rumble") c.setControllerRumble(selected=="On");
    else if(key=="controller.analog") c.setControllerAnalogStick(selected=="On");
    else if(key=="debug.fps") c.setShowFPS(selected=="On");
    else if(key=="debug.memory") c.setShowMemory(selected=="On");
    else if(key=="pictures.folder"||key=="music.folder"||key=="videos.folder"||key=="games.folder"||key=="browser.downloads") {
        if(!std::filesystem::is_directory(std::filesystem::u8path(selected))) {show_error(shell,"Choose an existing folder.");return;}
        if(key=="pictures.folder") c.picturesPath=std::filesystem::u8path(selected);
        if(key=="music.folder") c.musicPath=std::filesystem::u8path(selected);
        if(key=="videos.folder") c.videosPath=std::filesystem::u8path(selected);
        if(key=="games.folder") {openxmb::desktop::Library library;library.add_game_root(std::filesystem::u8path(selected));}
    }
    else if(key=="display.mode") {
        const auto flags=selected=="Windowed"?0:selected=="Borderless"?SDL_WINDOW_FULLSCREEN_DESKTOP:SDL_WINDOW_FULLSCREEN;
        if(SDL_SetWindowFullscreen(window->win.get(),flags)!=0) {show_error(shell,SDL_GetError());return;}
        window->config.fullscreen=selected!="Windowed";
    }
    else if(key=="display.monitor") {const int index=std::stoi(selected);SDL_Rect bounds{};if(SDL_GetDisplayBounds(index,&bounds)==0) SDL_SetWindowPosition(window->win.get(),bounds.x,bounds.y);}
    else if(key=="display.resolution") {int w=0,h=0;const auto x=selected.find('x');if(x!=std::string::npos) {w=std::stoi(selected.substr(0,x));h=std::stoi(selected.substr(x+1));}if(w>0&&h>0) {SDL_DisplayMode mode{};mode.w=w;mode.h=h;SDL_SetWindowDisplayMode(window->win.get(),&mode);SDL_SetWindowSize(window->win.get(),w,h);}}
    else if(key=="autostart") {
#ifdef _WIN32
        HKEY handle{};if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&handle,nullptr)!=ERROR_SUCCESS) {show_error(shell,"Unable to update launch at login.");return;}
        const auto command=L"\""+(c.exe_directory/L"OpenXMB.exe").wstring()+L"\"";
        const auto result=selected=="On"?RegSetValueExW(handle,L"OpenXMB",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t))):RegDeleteValueW(handle,L"OpenXMB");RegCloseKey(handle);
        if(result!=ERROR_SUCCESS&&result!=ERROR_FILE_NOT_FOUND) {show_error(shell,"Unable to update launch at login.");return;}
#else
        const auto home=std::getenv("HOME");if(!home) {show_error(shell,"Home directory unavailable.");return;}
        const auto file=std::filesystem::path(home)/".config/autostart/openxmb.desktop";
        std::filesystem::create_directories(file.parent_path());
        if(selected=="On") {std::ofstream out(file);out<<"[Desktop Entry]\nType=Application\nName=OpenXMB\nExec=\""<<(c.exe_directory/"XMS.bin").string()<<"\"\n";}
        else std::filesystem::remove(file);
#endif
        c.autostart=selected=="On";
    }
    if(key=="audio.device"&&!shell->set_audio_device(selected=="System Default"?std::string{}:selected)) {show_error(shell,openxmb::music::service().snapshot().error);return;}
    c.preferences[key]=selected;c.save_config();
    if(key=="theme.sounds")shell->reload_theme_sounds();
    if(key=="music.group"||key=="music.sort"||key.ends_with(".folder")) shell->reload_menu();
}
export void activate(shell* shell,const openxmb::xmb::CatalogNode& node) {
    const auto key=node.action_id.substr(8);const auto title=key;
    if(key=="system.update") {shell->emplace_overlay<message_overlay>("System Update"_(),"This feature is not available yet. This will be implemented in a future beta. Sorry!"_());return;}
    if(key=="theme.install") {shell->emplace_overlay<text_entry_overlay>("Theme archive or folder","",[shell](std::string path){try {shell->theme_library()->import_theme(std::filesystem::u8path(path));shell->reload_menu();}catch(const std::exception& e){show_error(shell,e.what());}});return;}
    if(key=="theme.preset.save") {shell->emplace_overlay<text_entry_overlay>("Preset name","",[shell](std::string name){try {auto library=shell->theme_library();const auto theme=library->create_custom_theme(name,nlohmann::json::parse(config::CONFIG.shell_snapshot()));library->save_preset(name,theme.id,{});}catch(const std::exception& e){show_error(shell,e.what());}});return;}
    if(key=="theme.preset.load") {const auto names=shell->theme_library()->presets();if(names.empty()){show_error(shell,"No saved theme presets.");return;}shell->emplace_overlay<choice_overlay>(names,0,[shell,names](unsigned i){try{shell->apply_theme(shell->theme_library()->load_preset(names.at(i)));}catch(const std::exception& e){show_error(shell,e.what());}});return;}
    if(key=="theme.reset") {shell->emplace_overlay<message_overlay>("Restore Original Theme?","Restore the original theme appearance?",std::vector<std::string>{"No","Yes"},[shell](unsigned i){if(i)shell->apply_theme(shell->theme_library()->revert());});return;}
    if(key=="about") {shell->open_panel("about");return;}
    if(key=="licenses") {shell->open_panel("licenses");return;}
    if(key=="browser.clear") {shell->emplace_overlay<message_overlay>("Clear Browsing Data?","Remove browser cookies, site storage and cached pages?",std::vector<std::string>{"No","Yes"},[](unsigned i){if(i) {config::CONFIG.preferences["browser.clear_site_data"]=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());config::CONFIG.save_config();}});return;}
    if(key=="reset") {shell->emplace_overlay<message_overlay>("Restore Default Settings?","Restore this profile's preferences? Your media files will be preserved.",std::vector<std::string>{"No","Yes"},[shell](unsigned i){if(i) {apply(shell,"autostart","Off");shell->reset_settings();}});return;}
    if(key=="music.rescan") {shell->reload_menu();return;}
    if(key=="games.rescan") {openxmb::desktop::Library library;const auto count=library.scan_games();show_error(shell,"Added "+std::to_string(count)+" games to the library.");return;}
    if(key=="games.emulation") {shell->open_panel("emulation");return;}
    if(key=="cache.clear") {shell->emplace_overlay<message_overlay>("Clear Thumbnail Cache?","Generated thumbnails will be recreated when needed. Original media is preserved.",std::vector<std::string>{"No","Yes"},[shell](unsigned i){if(i) {const auto root=openxmb::integrations::user_data_directory()/"thumbnails";std::error_code error;std::filesystem::remove_all(root,error);if(error)show_error(shell,error.message());}});return;}
    if(key=="debug.logs") {
#ifdef _WIN32
        const auto directory=openxmb::desktop::log_directory();std::filesystem::create_directories(directory);ShellExecuteW(nullptr,L"open",directory.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
#else
        show_error(shell,"File logging is unavailable on this platform.");
#endif
        return;
    }
    if(key=="debug.decoder") {const auto& m=openxmb::music::service().snapshot().metadata;show_error(shell,"Music codec: "+m.codec+"\nSample rate: "+std::to_string(m.sample_rate)+" Hz\nChannels: "+std::to_string(m.channels)+"\nVideo decoder details are available in the video player's information display.");return;}
    if(key=="input.keyboard"||key=="input.controller") {
        const bool controller=key=="input.controller";
        shell->emplace_overlay<choice_overlay>(::app::detail::localize_ui_choices(std::vector<std::string>{"Left","Right","Up","Down","Confirm","Back","Options","Extra","Restore Defaults"}),0,[shell,controller](unsigned i){
            static const std::array<std::string,8> actions{"left","right","up","down","ok","cancel","options","extra"};
            if(i==8) {for(const auto& action:actions) config::CONFIG.preferences.erase((controller?"pad.":"key.")+action);config::CONFIG.save_config();shell->reload_button_icons();return;}
            shell->queue_panel(std::string(controller?"bind.pad.":"bind.key.")+actions[i]);
        });return;
    }
    std::vector<std::string> values;for(const auto& choice:node.choices) values.push_back(choice.value);
    if(key=="display.monitor") for(int i=0;i<SDL_GetNumVideoDisplays();++i) values.push_back(std::to_string(i));
    if(key=="display.resolution") {for(int i=0;i<SDL_GetNumDisplayModes(0);++i) {SDL_DisplayMode mode{};if(SDL_GetDisplayMode(0,i,&mode)==0) {const auto item=std::to_string(mode.w)+"x"+std::to_string(mode.h);if(std::find(values.begin(),values.end(),item)==values.end())values.push_back(item);}}}
    if(key=="audio.device") {values.push_back("System Default");for(int i=0;i<SDL_GetNumAudioDevices(0);++i) if(auto* name=SDL_GetAudioDeviceName(i,0))values.emplace_back(name);}
    if(!values.empty()) {const auto current=value(key,shell);const auto it=std::find(values.begin(),values.end(),current);const auto selected=it==values.end()?0:static_cast<unsigned>(it-values.begin());auto labels=values;for(auto& label:labels)label=key=="audio.device"&&label!="System Default"?label:key=="language"?openxmb::localization::shell_locale::language_label(label):openxmb::localization::shell_locale::translate(label);shell->emplace_overlay<choice_overlay>(labels,selected,[shell,key,values](unsigned i){if(i<values.size())try{apply(shell,key,values[i]);}catch(const std::exception& e){show_error(shell,e.what());}});return;}
    shell->emplace_overlay<text_entry_overlay>(node.id.substr(node.id.find_last_of('.')+1),value(key,shell),[shell,key](std::string text){try{apply(shell,key,text);}catch(const std::exception& e){show_error(shell,e.what());}});
}
}
