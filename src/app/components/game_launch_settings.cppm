module;
#include "openxmb/desktop/library.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

export module openxmb.app:game_launch_settings;
import :component;
import :main;
import :choice_overlay;
import :message_overlay;
import :text_entry;
import :xmb_presentation;
import :setup_controls;
import :dialog_chrome;
import auroreengine;
import openxmb.config;
import openxmb.utils;

namespace app {
export class game_launch_settings final : public component, public action_receiver {
  using Game=openxmb::desktop::Game;
  using Mode=openxmb::desktop::GameLaunchMode;
  using Path=std::filesystem::path;
  struct Entry {Path path;bool folder;};
public:
  explicit game_launch_settings(Game game):game_(std::move(game)),draft_(game_.launch_override) {
    try {
      openxmb::desktop::Library library;
      origin_root_=library.root();origin_profile_=library.active_profile().id;origin_config_=config::CONFIG.profileConfigPath;
      valid_=game_.runner=="steam";
      message_=valid_?"Changes take effect when you choose Save settings.":"Launch settings are available for Steam games.";
    }catch(const std::exception& error){valid_=false;message_=error.what();}
  }
  ~game_launch_settings() override {alive_->store(false);}
  bool is_opaque() const override{return true;}
  bool do_fade_in() const override{return true;}
  bool do_fade_out() const override{return true;}
  result tick(shell* host) override {
    host_=host;const auto now=std::chrono::steady_clock::now();
    if(valid_&&now-last_origin_check_>=std::chrono::seconds(1)){last_origin_check_=now;guard();}
    return closing_?result::close|result::back_sound:result::success;
  }
  result on_action(action input) override {
    if(input==action::cancel) {
      if(browsing_){browsing_=false;selected_=folder_browser_?3:1;}
      else closing_=true;
      return result::success|result::back_sound;
    }
    const auto count=row_count();
    if(input==action::up&&count){selected_=(selected_+count-1)%count;return result::success;}
    if(input==action::down&&count){selected_=(selected_+1)%count;return result::success;}
    if(!host_||!guard())return result::success;
    if(input==action::options){information();return result::success;}
    if(input==action::ok){activate();return result::success|result::ok_sound;}
    return result::unsupported;
  }
  void render(auroreengine::gui_renderer& renderer,shell* host) override {
    const xmb_canvas c(renderer);c.rect(0,0,1920,1080,{0,0,0,0.64F});dialog_chrome::divider_frame(c,0.9F);
    c.text("Launch Settings",180,171,35,{1,1,1,0.95F});
    c.text(elide(game_.title,105),180,249,26,{1,1,1,0.86F});
    c.text(browsing_?elide(path_text(folder_),124):"Choose Steam or a direct game executable",180,287,20,{1,1,1,0.63F});
    const auto count=row_count(),first=selected_>5?selected_-5:0,last=std::min(count,first+7);
    for(std::size_t i=first;i<last;++i) {
      const float y=352+static_cast<float>(i-first)*66;const bool focused=i==selected_;
      if(focused){c.glow(155,y-29,1590,56,{1,1,1,0.18F},3,12,0);c.rect(155,y-29,1590,56,{1,1,1,0.10F},3);}
      const auto [title,detail]=row_text(i);
      setup_controls::text(c,elide(title,104),180,y,25,focused,focused?1.0F:0.78F,false,false);
      if(!detail.empty())setup_controls::text(c,elide(detail,137),182,y+22,16,false,0.60F,false,false);
    }
    if(count>7)setup_controls::scrollbar(c,1770,321,455,count,7,first,0.8F);
    c.text(elide(message_,142),180,843,18,{1,1,1,0.78F});
    host->render_message_footer(renderer,std::array{std::pair{action::ok,"Select"},std::pair{action::options,"Information"},std::pair{action::cancel,"Back"}},1);
  }
private:
  static std::string path_text(const Path& path){const auto text=path.u8string();return {text.begin(),text.end()};}
  static Path text_path(const std::string& text){return Path(std::u8string(text.begin(),text.end()));}
  static std::string elide(std::string text,std::size_t limit) {
    if(text.size()<=limit)return text;
    while(limit&&(static_cast<unsigned char>(text[limit])&0xc0)==0x80)--limit;
    return text.substr(0,limit)+"...";
  }
  bool guard() {
    if(!valid_)return false;
    try {
      std::error_code error;
      if(config::CONFIG.profileConfigPath==origin_config_&&std::filesystem::exists(origin_root_/"profiles.json",error)&&!error&&
         openxmb::desktop::Library(origin_root_).active_profile().id==origin_profile_)return true;
    }catch(...){}
    valid_=false;message_="The active profile changed. Close and reopen Launch Settings.";return false;
  }
  std::size_t row_count()const{return browsing_?3+entries_.size():6;}
  std::pair<std::string,std::string> row_text(std::size_t index)const {
    if(browsing_) {
      if(index==0)return folder_browser_?std::pair<std::string,std::string>{"Use this folder",path_text(folder_)}:
        std::pair<std::string,std::string>{"Enter executable path","Paste or type the full path to the game executable"};
      if(index==1)return {"Parent folder",path_text(folder_.parent_path())};
      if(index==2)return {"Enter folder path","Browse another installation folder or drive"};
      const auto& entry=entries_[index-3];return {path_text(entry.path.filename()),entry.folder?"Folder":"Game executable"};
    }
    switch(index) {
      case 0:return {"Launch mode",draft_.mode==Mode::direct?"Direct executable":"Steam (default)"};
      case 1:return {"Game executable",draft_.executable.empty()?"Optional for Steam session tracking; required for direct launch":path_text(draft_.executable)};
      case 2:return {"Direct launch arguments",draft_.arguments.empty()?"None · each argument is passed literally":openxmb::desktop::format_arguments(draft_.arguments)};
      case 3:return {"Direct launch folder",draft_.working_directory.empty()?"Default: the executable's folder":path_text(draft_.working_directory)};
      case 4:return {"Save settings","Applies to this game in the current local profile"};
      default:return {"About direct launch","Some Steam games require Steam, even when launched directly"};
    }
  }
  void begin_browser(bool folder) {
    folder_browser_=folder;browsing_=true;
    Path start=folder?draft_.working_directory:draft_.executable.parent_path();
    std::error_code error;
    if(start.empty()||!std::filesystem::is_directory(start,error))start=game_.working_directory;
    if(start.empty()||!std::filesystem::is_directory(start,error))start=origin_root_;
    browse(start);
  }
  void browse(Path folder) {
    std::error_code error;
    if(!std::filesystem::is_directory(folder,error)||error){message_="Choose an existing folder you can read.";return;}
    folder_=std::filesystem::absolute(folder,error).lexically_normal();
    if(error){message_="The selected folder could not be opened.";return;}
    entries_.clear();selected_=0;std::size_t inspected=0;
    auto it=std::filesystem::directory_iterator(folder_,std::filesystem::directory_options::skip_permission_denied,error);
    const auto end=std::filesystem::directory_iterator{};
    while(!error&&it!=end&&inspected++<4096) {
      const auto& entry=*it;std::error_code status;
      const bool directory=entry.is_directory(status);
      bool include=directory;
      if(!folder_browser_&&!directory&&entry.is_regular_file(status)) {
#ifdef _WIN32
        auto ext=path_text(entry.path().extension());for(auto& c:ext)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        include=ext==".exe";
#else
        const auto permissions=entry.status(status).permissions();
        include=!status&&(permissions&(std::filesystem::perms::owner_exec|std::filesystem::perms::group_exec|std::filesystem::perms::others_exec))!=std::filesystem::perms::none;
#endif
      }
      if(include&&!status)entries_.push_back({entry.path(),directory});
      if(entries_.size()>=512)break;
      it.increment(error);
    }
    std::sort(entries_.begin(),entries_.end(),[](const Entry& a,const Entry& b){if(a.folder!=b.folder)return a.folder;return a.path.filename()<b.path.filename();});
    message_=error?"Some entries could not be read. You can enter an exact path.":
      (entries_.size()>=512||inspected>=4096)?"Large folder: showing a limited list. Enter an exact path if needed.":
      folder_browser_?"Choose Use this folder, or open a subfolder.":"Choose the game's executable. Setup programs and uninstallers are not games.";
  }
  void edit_executable() {
    const std::weak_ptr<std::atomic_bool> weak=alive_;
    host_->emplace_overlay<text_entry_overlay>("Game executable (full file path)",path_text(draft_.executable),[this,weak](std::string text){
      const auto alive=weak.lock();if(!alive||!alive->load()||!guard())return;
      try {auto candidate=game_;candidate.launch_override=draft_;candidate.launch_override.mode=Mode::direct;candidate.launch_override.executable=text_path(text);
        // Validate the executable independently of an unfinished working-folder edit.
        candidate.launch_override.working_directory.clear();(void)openxmb::desktop::launch_game_target(candidate);
        draft_.executable=candidate.launch_override.executable;browsing_=false;selected_=1;message_="Executable selected. Choose Save settings to apply.";
      }catch(const std::exception& error){message_=error.what();}
    });
  }
  void activate() {
    if(browsing_) {
      if(selected_==0) {
        if(folder_browser_){draft_.working_directory=folder_;browsing_=false;selected_=3;message_="Working folder selected. Choose Save settings to apply.";}
        else edit_executable();
      }else if(selected_==1)browse(folder_.parent_path().empty()?folder_:folder_.parent_path());
      else if(selected_==2) {
        const std::weak_ptr<std::atomic_bool> weak=alive_;
        host_->emplace_overlay<text_entry_overlay>("Browse folder (full path)",path_text(folder_),[this,weak](std::string text){const auto alive=weak.lock();if(alive&&alive->load()&&guard())browse(text_path(text));});
      }else {
        const auto entry=entries_[selected_-3];
        if(entry.folder)browse(entry.path);
        else {draft_.executable=entry.path;browsing_=false;selected_=1;message_="Executable selected. Choose Save settings to apply.";}
      }
      return;
    }
    const std::weak_ptr<std::atomic_bool> weak=alive_;
    if(selected_==0)host_->emplace_overlay<choice_overlay>(std::vector<std::string>{"Steam (default)","Direct executable"},draft_.mode==Mode::direct?1U:0U,[this,weak](unsigned index){const auto alive=weak.lock();if(!alive||!alive->load()||!guard()||index>1)return;draft_.mode=index?Mode::direct:Mode::provider;message_="Launch mode changed. Choose Save settings to apply.";});
    else if(selected_==1)begin_browser(false);
    else if(selected_==2)host_->emplace_overlay<text_entry_overlay>("Launch arguments (quote values containing spaces)",openxmb::desktop::format_arguments(draft_.arguments),[this,weak](std::string text){const auto alive=weak.lock();if(!alive||!alive->load()||!guard())return;try{auto arguments=openxmb::desktop::parse_arguments(text);if(arguments.size()>255)throw std::runtime_error("Use at most 255 arguments.");draft_.arguments=std::move(arguments);message_="Arguments changed. Choose Save settings to apply.";}catch(const std::exception& error){message_=error.what();}});
    else if(selected_==3)host_->emplace_overlay<choice_overlay>(std::vector<std::string>{"Use executable's folder (default)","Choose working folder"},draft_.working_directory.empty()?0U:1U,[this,weak](unsigned index){const auto alive=weak.lock();if(!alive||!alive->load()||!guard())return;if(index==0){draft_.working_directory.clear();message_="Default working folder selected. Choose Save settings to apply.";}else if(index==1)begin_browser(true);});
    else if(selected_==4)save();
    else information();
  }
  void save() {
    if(!guard())return;
    try {
      openxmb::desktop::Library library(origin_root_);auto games=library.games();
      const auto found=std::find_if(games.begin(),games.end(),[&](const Game& game){return game.id==game_.id;});
      if(found==games.end())throw std::runtime_error("This game was removed. Close and reopen Launch Settings.");
      if(found->runner!="steam"||found->content!=game_.content)throw std::runtime_error("This game's provider changed. Close and reopen Launch Settings.");
      found->launch_override=draft_;(void)openxmb::desktop::launch_game_target(*found);
      if(!guard())return;
      game_=library.save_game(std::move(*found));host_->queue_menu_reload();
      message_=draft_.mode==Mode::direct?"Direct launch settings saved. Steam may still be required by this game.":"Steam launch saved. The selected executable can identify this game for session tracking.";
    }catch(const std::exception& error){message_=std::string("Could not save: ")+error.what();}
  }
  void information() {
    host_->emplace_overlay<message_overlay>("Steam launch settings",
      "Steam (default) asks Steam to launch this game. Selecting its game executable also lets OpenXMB identify a new local game window for the in-game XMB.\n\nDirect executable starts the installed game file with your chosen arguments and working folder. Select the actual game executable, not Steam or an installer.\n\nSome DRM-free Steam games support direct launch. Other games require Steam, authentication or their own launcher. This setting does not bypass those requirements.\n\nArguments are passed literally; quote values containing spaces. Leave the working folder at its default unless the game requires another folder.\n\nChoose Save settings to apply. Your Steam library entry, artwork and play history stay together after discovery refresh.",std::vector<std::string>{"OK"});
  }
  shell* host_{};Game game_;openxmb::desktop::GameLaunchOverride draft_;
  Path origin_root_,origin_config_,folder_;std::string origin_profile_,message_;
  std::chrono::steady_clock::time_point last_origin_check_{};
  std::vector<Entry> entries_;std::size_t selected_{};bool browsing_{},folder_browser_{},closing_{},valid_{true};
  std::shared_ptr<std::atomic_bool> alive_=std::make_shared<std::atomic_bool>(true);
};
} // namespace app
