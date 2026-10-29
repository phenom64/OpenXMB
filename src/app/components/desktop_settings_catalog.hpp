#pragma once
#include "openxmb/xmb/catalog.hpp"
#include "../../localization/language_options.hpp"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <string_view>
#include <functional>
#include <string>
#include <vector>

namespace app::desktop_settings {
struct Entry {
    std::string group, key, label, description;
    std::vector<std::string> values{};
};
inline std::string trophy_sound_file_label(std::string_view path) {
    if(path.empty())return "Not selected";
    const auto slash=path.find_last_of("/\\");
    const auto name=path.substr(slash==std::string_view::npos?0:slash+1);
    if(name.empty())return "Not selected";
    std::size_t end=0;unsigned points=0;
    while(end<name.size()&&points<24) {
        ++end;while(end<name.size()&&(static_cast<unsigned char>(name[end])&0xc0)==0x80)++end;
        ++points;
    }
    std::string label(name.substr(0,end));
    for(auto& c:label)if(static_cast<unsigned char>(c)<32||c==127)c='?';
    if(end<name.size())label+="...";
    return label;
}
inline const std::vector<Entry>& entries() {
    static const std::vector<Entry> data{
        {"Theme","theme.install","Install Theme","Import a theme archive or theme folder."},
        {"Theme","theme.wallpaper","Background File","Choose a photo or video background."},
        {"Theme","theme.waves","Wave","Show the animated background ribbon.",{"On","Off"}},
        {"Theme","theme.particles","Sparkles","Show background particles.",{"On","Off"}},
        {"Theme","theme.particle.color","Sparkle Colour","Use a hexadecimal colour such as #ffeefc."},
        {"Theme","theme.icon.style","Icon Style","Choose the icon material.",{"default","flat"}},
        {"Theme","theme.icon.color","Icon Colour","Use auto or a hexadecimal colour."},
        {"Theme","theme.text.color","Text Colour","Use a hexadecimal colour."},
        {"Theme","theme.font","Font File","Choose a TrueType or OpenType font file."},
        {"Theme","theme.sounds","Sound Set Folder","Use a folder containing ok.wav, confirm.wav, cancel.wav and back.wav."},
        {"Theme","theme.preset.save","Save Theme Preset","Save the current appearance with a name."},
        {"Theme","theme.preset.load","Saved Theme Presets","Apply a saved appearance preset."},
        {"Theme","theme.reset","Restore Original Theme","Restore the original OpenXMB appearance."},
        {"System","system.name","System Name","The name displayed by OpenXMB."},
        {"System","language","Language","Preferred interface language.",openxmb::localization::shell_locale::language_values()},
        {"System","startup.intro","Startup Intro","Play the startup sequence.",{"On","Off"}},
        {"System","startup.warning","Photosensitivity Warning","Show the dismissible startup warning.",{"On","Off"}},
        {"System","autostart","Launch at Login","Start OpenXMB when you sign in.",{"Off","On"}},
        {"System","pictures.folder","Pictures Folder","Choose the root folder for photos."},
        {"System","music.folder","Music Folder","Choose the root folder for music and playlists."},
        {"System","videos.folder","Videos Folder","Choose the root folder for videos."},
        {"System","about","System Information","OpenXMB version and information about this computer."},
        {"System","licenses","Licenses","Read the program and third-party license notices."},
        {"System","reset","Restore Default Settings","Restore this profile's shell settings; preserve media."},
        {"Display","display.mode","Window Mode","Choose how OpenXMB uses the display.",{"Windowed","Borderless","Fullscreen"}},
        {"Display","display.monitor","Display","Move OpenXMB to a connected display."},
        {"Display","display.resolution","Resolution","Choose the fullscreen display resolution."},
        {"Display","text.scale","Interface Text Size","Scale text throughout OpenXMB.",{"0.8","1.0","1.2","1.4"}},
        {"Display","display.brightness","Brightness","Adjust the OpenXMB scene brightness.",{"0.6","0.8","1.0","1.2"}},
        {"Display","vsync","VSync","Synchronise presentation to the display.",{"On","Off"}},
        {"Display","fps","Frame Limit","Maximum shell rendering rate.",{"30","60","120","144","240","Unlimited"}},
        {"Display","samples","Anti-aliasing","Multisample anti-aliasing for supported graphics hardware.",{"1","2","4","8"}},
        {"Display","glass","Glass Icons","Use refractive icon materials.",{"On","Off"}},
        {"Sound","audio.device","Audio Output Device","Select an available playback device."},
        {"Sound","audio.master","Master Volume","Volume for all OpenXMB audio.",{"0","25","50","75","100"}},
        {"Sound","audio.effects","Interface Volume","Volume of navigation and confirmation sounds.",{"0","25","50","75","100"}},
        {"Sound","audio.trophy.mode","Trophy Notification Sound","Play your own WAV with OpenXMB trophy notices. Leave Off if RPCS3 already plays a trophy sound.",{"Off","User-supplied"}},
        {"Sound","audio.trophy.file","Trophy Sound File","Choose your own PCM WAV (up to 5 seconds and 2 MiB). No original PS3 sound is included."},
        {"Sound","audio.music","Music Volume","Default volume for music playback.",{"0","25","50","75","100"}},
        {"Sound","audio.video","Video Volume","Default volume for video playback.",{"0","25","50","75","100"}},
        {"Sound","audio.startup","Startup Sound","Play the startup cue in full.",{"On","Off"}},
        {"Sound","audio.background","Background Audio","Continue audio when OpenXMB loses focus.",{"On","Off"}},
        {"Sound","audio.duck","Quiet Interface During Music","Reduce navigation sound volume while music is playing.",{"On","Off"}},
        {"Music","music.rescan","Rescan Music Library","Refresh folders and metadata from your music location."},
        {"Music","music.group","Library View","Browse folders, songs, albums, artists or saved playlists.",{"Folders","Songs","Albums","Artists","Playlists"}},
        {"Music","music.sort","Default Sort","Sort tracks in each folder.",{"Name","Date","Size"}},
        {"Music","music.background","Background Music","Continue music after closing the player.",{"On","Off"}},
        {"Music","music.repeat","Default Repeat","Repeat setting when opening the player.",{"Off","All","One"}},
        {"Music","music.shuffle","Default Shuffle","Shuffle tracks in the play queue.",{"Off","On"}},
        {"Music","music.visualizer","Music Visualiser","Choose the full-screen music presentation.",{"Wave","Flow"}},
        {"Music","music.artwork","Album Artwork","Display available cover artwork.",{"On","Off"}},
        {"Video","video.hardware","Video Decoding","Automatically use hardware decoding when available.",{"auto","off","d3d11va","vaapi","vulkan"}},
        {"Video","video.audio.language","Audio Language","Preferred audio track language.",{"Default","eng","fra","deu","spa","jpn"}},
        {"Video","video.subtitle.language","Subtitle Language","Preferred subtitle track language.",{"Off","Default","eng","fra","deu","spa","jpn"}},
        {"Video","video.subtitle.size","Subtitle Size","Scale subtitle text.",{"0.8","1.0","1.25","1.5"}},
        {"Video","video.resume","Resume Playback","Remember the last playback position.",{"On","Off"}},
        {"Video","video.fit","Default Screen Fit","How video fits on the screen.",{"Fit","Fill","Original"}},
        {"Photo","photo.duration","Slideshow Duration","Seconds each photograph remains visible.",{"3","5","10","20","30"}},
        {"Photo","photo.transition","Slideshow Transition","Transition between photographs.",{"Fade","None"}},
        {"Photo","photo.repeat","Repeat Slideshow","Continue after the last photograph.",{"On","Off"}},
        {"Photo","photo.shuffle","Shuffle Slideshow","Randomise slideshow order.",{"Off","On"}},
        {"Photo","photo.fit","Default Screen Fit","How photographs fit on screen.",{"Fit","Fill"}},
        {"Accessory","controller.type","Controller Prompts","Automatic detection or a preferred button style.",{"auto","playstation","xbox","keyboard","steam"}},
        {"Accessory","controller.rumble","Controller Vibration","Enable controller feedback.",{"On","Off"}},
        {"Accessory","controller.analog","Analog Navigation","Enable analog controller navigation.",{"On","Off"}},
        {"Accessory","input.repeat","Navigation Repeat","Delay between held directional inputs in milliseconds.",{"150","180","220","300"}},
        {"Accessory","input.keyboard","Keyboard Bindings","Choose an action, then press its keyboard key."},
        {"Accessory","input.controller","Controller Bindings","Choose an action, then press its controller button."},
        {"Accessory","input.osk","On-Screen Keyboard","Show the controller keyboard during text entry.",{"On","Off"}},
        {"Date and Time","date.format","Date Format","Order of year, month and day.",{"DD/MM/YYYY","MM/DD/YYYY","YYYY-MM-DD"}},
        {"Date and Time","time.format","Time Format","Choose a 12-hour or 24-hour clock.",{"24-hour","12-hour"}},
        {"Date and Time","time.seconds","Show Seconds","Show seconds in the clock.",{"Off","On"}},
        {"Date and Time","time.offset","Time Zone Offset","Hours added to local system time for display.",{"-12","-8","-5","0","1","3","5.5","8","9","12"}},
        {"Power Save","power.dim","Idle Dimming","Dim the scene after inactivity.",{"Off","1 minute","5 minutes","10 minutes"}},
        {"Power Save","power.background.fps","Background Frame Limit","Reduce rendering while unfocused.",{"15","30","60"}},
        {"Power Save","power.reduced.motion","Reduced Motion","Reduce background movement and sparkles.",{"Off","On"}},
        {"Power Save","power.keep.awake","Prevent Sleep During Playback","Keep this computer awake while media plays.",{"On","Off"}},
        {"Browser","browser.homepage","Homepage","Page opened when launching the browser."},
        {"Browser","browser.search","Search Engine","Search provider used for non-address text.",{"DuckDuckGo","Google","Bing"}},
        {"Browser","browser.zoom","Page Zoom","Default browser page scale.",{"0.8","1.0","1.25","1.5","2.0"}},
        {"Browser","browser.downloads","Downloads Folder","Directory for browser downloads."},
        {"Browser","browser.clear","Clear Browsing Data","Remove browser cookies and cached site data."},
        {"Game","games.folder","Game Library Folder","Choose the folder scanned for games."},
        {"Game","games.rescan","Rescan Game Library","Refresh the configured game library."},
        {"Game","games.launch","After Launching a Game","Choose how the shell behaves while a game runs.",{"Keep Running","Minimise"}},
        {"Game","games.emulation","Emulation Configuration","Select an installed core and game content."},
        {"Video","youtube.executable","YouTube Application","Choose an installed application, or leave empty for automatic discovery."},
        {"Game","games.runners","Game Runners","Detect installed emulators and configure executable paths."},
        {"Game","artwork.steamgriddb.automatic","Automatic Game Artwork","Fill missing icons and backgrounds after an exact title match.",{"On","Off"}},
        {"Game","artwork.steamgriddb.key_env","Artwork API Key Variable","Environment variable containing your SteamGridDB API key."},
        {"Game","session.hotkey","In-Game XMB Shortcut","Switch to OpenXMB during an owned session. Guide also works when the system allows it.",{"Ctrl+Shift+F1","Ctrl+Shift+F2","Ctrl+Shift+F3","Ctrl+Shift+F4"}},
        {"Advanced","debug.fps","Show FPS","Display the shell frame rate.",{"Off","On"}},
        {"Advanced","debug.memory","Show GPU Memory","Display GPU memory use.",{"Off","On"}},
        {"Advanced","debug.logs","Open Log Folder","Open the location of diagnostic logs."},
        {"Advanced","debug.decoder","Decoder Information","View active audio and video playback diagnostics."},
    };
    return data;
}
inline void populate(openxmb::xmb::Catalog& catalog,
    const std::function<std::string(std::string_view)>& translate =
        [](std::string_view text){return std::string(text);}) {
    using namespace openxmb::xmb;
    auto& root=catalog.nodes.at("category.settings");
    root.children={"category.settings.system.update","category.settings.theme.settings"};root.default_selection=1;
    catalog.nodes.at("category.settings.system.update").action_id="desktop.system.update";
    for(const auto& entry:entries()) {
        const auto group=entry.group=="Theme"?std::string{"category.settings.theme.settings"}:"category.settings.desktop."+entry.group;
        if(!catalog.nodes.contains(group)) {
            CatalogNode node;node.id=group;node.kind=NodeKind::menu;node.label_key=group+".label";
            const std::vector<std::pair<std::string,std::string>> icons{
                {"System","category.settings.system.settings"},{"Display","category.settings.display.settings"},
                {"Sound","category.settings.sound.settings"},{"Music","category.settings.music.settings"},
                {"Video","category.settings.video.settings"},{"Accessory","category.settings.accessory.settings"},
                {"Date and Time","category.settings.date.and.time.settings"},{"Power Save","category.settings.power.save.settings"},
                {"Game","category.settings.game.settings"},{"Browser","category.settings.network.settings"}};
            node.icon_ref="compat.icon.xmb.icon.001";
            for(const auto& [name,id]:icons)if(name==entry.group&&catalog.nodes.contains(id))node.icon_ref=catalog.nodes.at(id).icon_ref;
            node.action_id="navigate.children";
            catalog.strings[node.label_key]=entry.group+" Settings";catalog.nodes.emplace(group,std::move(node));
            root.children.push_back(group);
        }
        CatalogNode node;node.id="category.settings.desktop."+entry.key;node.kind=NodeKind::setting;
        node.label_key=node.id+".label";node.description_key=node.id+".description";
        node.icon_ref="compat.icon.xmb.icon.022";node.action_id="desktop."+entry.key;
        catalog.strings[node.label_key]=entry.label;catalog.strings[node.description_key]=entry.description;
        for(std::size_t i=0;i<entry.values.size();++i) {
            const auto key=node.id+".choice."+std::to_string(i);catalog.strings[key]=entry.values[i];
            if(entry.key=="language") {
                catalog.strings[key]=openxmb::localization::shell_locale::languages[i].second;
            }
            node.choices.push_back({key,key,entry.values[i]});
        }
        catalog.nodes.at(group).children.push_back(node.id);catalog.nodes.emplace(node.id,std::move(node));
    }
    // Root order observed on the PS3 (RPCS3 category inventory): System Update,
    // Game, Video, Music, [Chat], System, Theme, Date and Time, Power Save,
    // Accessory, [Printer], Display, Sound. Bracketed rows have no OpenXMB
    // equivalent; OpenXMB-only groups keep their order after the PS3 rows.
    constexpr std::array<std::string_view,11> ps3_order{
        "category.settings.system.update","category.settings.desktop.Game","category.settings.desktop.Video",
        "category.settings.desktop.Music","category.settings.desktop.System","category.settings.theme.settings",
        "category.settings.desktop.Date and Time","category.settings.desktop.Power Save",
        "category.settings.desktop.Accessory","category.settings.desktop.Display","category.settings.desktop.Sound"};
    std::ranges::stable_sort(root.children,{},[&](const std::string& id) {
        return static_cast<std::size_t>(std::ranges::find(ps3_order,id)-ps3_order.begin());
    });
    catalog.strings["category.settings.system.settings.description"]="Adjust settings for the OpenXMB system.";
    // Replace the legacy console language selector with actual shipped locales.
    for(auto& [id,node]:catalog.nodes) if(node.action_id=="simulate.system.language") {
        node.action_id="desktop.language"; node.choices.clear(); node.default_selection=0;
        for(const auto& [value,name]:openxmb::localization::shell_locale::languages) {
            const auto key=id+".choice."+std::string(value);catalog.strings[key]=name;
            node.choices.push_back({key,key,std::string(value)});
        }
    }
    for(auto& [key,text]:catalog.strings) text=translate(text);
    for(const auto& [id,node]:catalog.nodes) if(node.action_id=="desktop.language") {
        for(const auto& choice:node.choices) {
            const auto native=std::ranges::find(openxmb::localization::shell_locale::languages,
                choice.value,[](const auto& language){return language.first;});
            if(native!=openxmb::localization::shell_locale::languages.end()&&native->first!="auto")
                catalog.strings[choice.label_key]=native->second;
        }
    }
}
}
