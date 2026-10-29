#include "../../src/app/components/desktop_settings_catalog.hpp"
#include "openxmb/xmb/settings_controller.hpp"
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>

void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
int main(int argc,char** argv) try {
    require(argc==2,"catalog fixture path required");
    auto loaded=openxmb::xmb::load_catalog_file(argv[1]);require(static_cast<bool>(loaded),"catalog loads");
    auto catalog=std::move(*loaded.value);
    const auto system_icon=catalog.nodes.at("category.settings.system.settings").icon_ref;
    const auto update_icon=catalog.nodes.at("category.settings.system.update").icon_ref;
    app::desktop_settings::populate(catalog);
    const auto& trophy_sound=catalog.nodes.at("category.settings.desktop.audio.trophy.mode");
    require(trophy_sound.choices.size()==2&&trophy_sound.choices[0].value=="Off"&&trophy_sound.choices[1].value=="User-supplied","trophy sound exposes only Off and explicit user-supplied audio");
    const auto& trophy_file=catalog.nodes.at("category.settings.desktop.audio.trophy.file");
    require(trophy_file.action_id=="desktop.audio.trophy.file","trophy audio file has a real settings action");
    require(app::desktop_settings::trophy_sound_file_label("C:\\Users\\Example\\Audio\\Trophy.wav")=="Trophy.wav","sound file row shows the name instead of an overlapping full path");
    require(app::desktop_settings::trophy_sound_file_label("")=="Not selected","empty sound selection has a readable value");
    require(app::desktop_settings::trophy_sound_file_label("/audio/abcdefghijklmnopqrstuvwxyz-long-file.wav")=="abcdefghijklmnopqrstuvwx...","long sound filename is bounded");
    const auto& root=catalog.nodes.at("category.settings");
    require(root.children.front()=="category.settings.system.update","System Update remains visible");
    require(catalog.nodes.at(root.children.front()).icon_ref==update_icon,"update artwork retained");
    require(catalog.nodes.at("category.settings.desktop.System").icon_ref==system_icon,"system artwork retained");
    require(catalog.nodes.contains("category.settings.printer.settings"),"future pane assets and source definitions preserved");
    auto made=openxmb::xmb::SettingsCatalogController::create(catalog);require(static_cast<bool>(made),"desktop tree creates controller");
    auto& controller=*made.controller;
    const auto group=std::find(root.children.begin(),root.children.end(),"category.settings.desktop.Accessory");
    require(group!=root.children.end(),"accessory group exposed");
    const auto selection=static_cast<std::size_t>(group-root.children.begin());
    require(static_cast<bool>(controller.set_selection(selection,1)),"select group");
    require(static_cast<bool>(controller.activate(2)),"activate group");
    require(controller.route().id==*group,"nested settings open selected group");
    require(static_cast<bool>(controller.set_selection(3,3)),"select child");
    require(static_cast<bool>(controller.back(4)),"return to parent");
    require(controller.route().selection==selection,"Back restores parent choice");
    require(controller.route().id=="category.settings","Back returns to category");
    std::set<std::string> actions;
    for(const auto& entry:app::desktop_settings::entries()) {
        const auto& node=catalog.nodes.at("category.settings.desktop."+entry.key);
        require(actions.insert(node.action_id).second,"desktop actions unique");
        require(!catalog.text(node.label_key).empty(),"every row has readable text");
        require(!node.action_id.starts_with("simulate."),"desktop row never simulates successful config writes");
    }
    std::cout<<"desktop_settings_tests: retained assets, live tree and parent navigation passed\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
