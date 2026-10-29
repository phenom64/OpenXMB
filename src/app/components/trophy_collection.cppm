module;
#include "openxmb/desktop/achievements.hpp"
#include "openxmb/desktop/retroachievements.hpp"
#include "openxmb/desktop/http.hpp"
#include "openxmb/desktop/library.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <future>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

export module openxmb.app:trophy_collection;
import :component;
import :main;
import :xmb_presentation;
import :setup_controls;
import :dialog_chrome;
import :choice_overlay;
import :message_overlay;
import :text_entry;
import auroreengine;
import openxmb.config;
import openxmb.utils;
import glm;

export namespace app {
namespace trophy_detail {
namespace fs=std::filesystem;
namespace desktop=openxmb::desktop;
struct Profile {fs::path root;std::string user;};
struct Record {desktop::AchievementGame game;std::vector<desktop::RetroProgressDetail> details;bool remote{},loaded{};};
enum class WorkKind {profiles,sets,account,progress};
struct WorkResult {
  WorkKind kind{};std::vector<Profile> profiles;std::vector<Record> records;
  desktop::RetroAccountResult account;desktop::RetroProgressResult progress;
  unsigned game_id{};std::string message;
};
std::string path_text(const fs::path& path){const auto value=path.u8string();return {value.begin(),value.end()};}
fs::path text_path(const std::string& text){return fs::path(std::u8string(text.begin(),text.end()));}
std::string elide(std::string text,std::size_t length){if(text.size()<=length)return text;while(length&&(static_cast<unsigned char>(text[length])&0xc0)==0x80)--length;return text.substr(0,length)+"...";}
std::vector<unsigned> game_ids(std::string_view text){
  std::vector<unsigned> values;std::size_t start=0;
  while(start<text.size()) {auto end=text.find(',',start);auto part=text.substr(start,end==text.npos?text.size()-start:end-start);unsigned id{};auto parsed=std::from_chars(part.data(),part.data()+part.size(),id);
    if(part.empty()||parsed.ec!=std::errc{}||parsed.ptr!=part.data()+part.size()||!id||values.size()>=64)throw std::runtime_error("Use up to 64 comma-separated numeric game IDs.");
    if(std::ranges::find(values,id)==values.end())values.push_back(id);if(end==text.npos)break;start=end+1;
  }return values;
}
bool env_name(std::string_view name){if(name.empty()||name.size()>128||!((name[0]>='A'&&name[0]<='Z')||(name[0]>='a'&&name[0]<='z')||name[0]=='_'))return false;return std::all_of(name.begin(),name.end(),[](char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';});}
std::string date(std::optional<std::int64_t> value){if(!value)return "Unavailable";const auto raw=static_cast<std::time_t>(*value);if(static_cast<std::int64_t>(raw)!=*value)return "Unavailable";std::tm tm{};
#ifdef _WIN32
  if(gmtime_s(&tm,&raw)!=0)return "Unavailable";
#else
  if(!gmtime_r(&raw,&tm))return "Unavailable";
#endif
  char output[64]{};return std::strftime(output,sizeof(output),"%Y-%m-%d %H:%M UTC",&tm)?output:"Unavailable";
}
std::string grade(desktop::TrophyGrade value){switch(value){case desktop::TrophyGrade::platinum:return "Platinum";case desktop::TrophyGrade::gold:return "Gold";case desktop::TrophyGrade::silver:return "Silver";case desktop::TrophyGrade::bronze:return "Bronze";default:return {};}}
bool concealed(const desktop::Achievement& a){return a.hidden&&(!a.unlock_state_known||!a.unlocked);}
std::string state(const desktop::Achievement& a){if(!a.unlock_state_known)return "Unlock state unavailable";if(!a.unlocked)return "Locked";return a.hardcore==true?"Unlocked · Hardcore":"Unlocked";}
desktop::HttpTransport transport(std::shared_ptr<std::atomic_bool> cancelled) {
  return [cancelled](const desktop::HttpRequest& original){if(cancelled->load())return desktop::HttpResponse{false,0,{},"Cancelled"};auto req=original;auto previous=req.progress;req.progress=[cancelled,previous](std::uint64_t n,std::uint64_t total){return !cancelled->load()&&(!previous||previous(n,total));};return desktop::http_get(req);};
}
}
class trophy_collection final : public component, public action_receiver {
  using Clock=std::chrono::steady_clock;
  using WorkResult=trophy_detail::WorkResult;
  using Record=trophy_detail::Record;
  using Profile=trophy_detail::Profile;
  enum class Page {games,trophies,settings};
public:
  explicit trophy_collection(auroreengine::resource_loader& loader):loader_(loader),alive_(std::make_shared<std::atomic_bool>(true)) {
    try {
      openxmb::desktop::Library library;origin_root_=library.root();origin_profile_=library.active_profile().id;origin_config_=config::CONFIG.profileConfigPath;
      cache_root_=origin_root_/"cache"/"achievements"/origin_profile_;
      saved_root_=config::CONFIG.preference("trophies.rpcs3.root");saved_user_=config::CONFIG.preference("trophies.rpcs3.user");
      ra_.username=config::CONFIG.preference("achievements.ra.username");ra_.ulid=config::CONFIG.preference("achievements.ra.ulid");
      key_env_=config::CONFIG.preference("achievements.ra.key_env","RETROACHIEVEMENTS_API_KEY");ra_games_=trophy_detail::game_ids(config::CONFIG.preference("achievements.ra.games"));
      begin_profiles();
    }catch(const std::exception& e){message_=e.what();}
  }
  ~trophy_collection() override {
    alive_->store(false);if(cancel_)cancel_->store(true);
    if(artwork_loading_.valid())artwork_loading_.wait();
    try{loader_.getDevice().waitIdle();}catch(...){}
  }
  [[nodiscard]] bool is_opaque() const override{return true;}
  [[nodiscard]] bool do_fade_out() const override{return true;}
  result tick(shell* host) override {
    host_=host;
    if(Clock::now()-last_origin_check_>=std::chrono::seconds(1)){last_origin_check_=Clock::now();if(!same_origin()){if(cancel_)cancel_->store(true);message_="The active profile changed. Close and reopen Trophy Collection.";origin_valid_=false;}}
    if(work_.valid()&&work_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
      auto value=work_.get();busy_=false;if(origin_valid_&&same_origin())finish(std::move(value));else {origin_valid_=false;message_="The active profile changed. Close and reopen Trophy Collection.";}
    }
    update_artwork();return closing_?result::close|result::back_sound:result::success;
  }
  result on_action(action input) override {
    if(input==action::cancel){if(page_==Page::games){closing_=true;if(cancel_)cancel_->store(true);}else if(page_==Page::trophies){page_=Page::games;selected_=selected_game_;}else {page_=Page::games;selected_=std::min(selected_game_,records_.size());}return result::success|result::back_sound;}
    const auto count=row_count();if(input==action::up&&count){selected_=(selected_+count-1)%count;return result::success;}
    if(input==action::down&&count){selected_=(selected_+1)%count;return result::success;}
    if(!origin_valid_||!host_)return result::success;
    if(input==action::options){show_information();return result::success;}
    if(input==action::extra){refresh();return result::success;}
    if(input==action::ok){if(busy_)message_="Please wait for the current request.";else activate();return result::success|result::ok_sound;}
    return result::unsupported;
  }
  void render(auroreengine::gui_renderer& renderer,shell* host) override {
    xmb_canvas c(renderer);c.rect(0,0,1920,1080,{0,0,0,0.58F});dialog_chrome::divider_frame(c,0.8F);
    c.text("Trophy Collection",180,171,35,{1,1,1,0.95F});
    if(page_==Page::settings)c.text("Settings",180,258,26,{1,1,1,0.75F});
    else if(page_==Page::trophies&&selected_game_<records_.size())c.text(trophy_detail::elide(records_[selected_game_].game.title,70),180,258,26,{1,1,1,0.75F});
    else c.text(profile_label_.empty()?"Choose a trophy source":trophy_detail::elide(profile_label_,86),180,258,24,{1,1,1,0.72F});
    const auto count=row_count(),first=selected_>5?selected_-5:0;const auto last=std::min(count,first+7);
    for(std::size_t i=first;i<last;++i){const float y=315+static_cast<float>(i-first)*72;const bool focus=i==selected_;if(focus){c.glow(155,y-31,1150,58,{1,1,1,0.20F},3,13,0);c.rect(155,y-31,1150,58,{1,1,1,0.11F},3);}
      auto [title,detail]=row_text(i);c.text(trophy_detail::elide(title,70),180,y,27,{1,1,1,focus?1.0F:0.78F},focus?0.35F*xmb_canvas::pulse():0);if(!detail.empty())c.text(trophy_detail::elide(detail,92),182,y+23,17,{1,1,1,0.59F});}
    if(count==0)c.text(busy_?"Loading trophies...":"No trophies found for the selected source.",180,337,25,{1,1,1,0.7F});
    if(count>7)setup_controls::scrollbar(c,1310,290,490,count,7,first,0.75F);
    if(artwork_&&artwork_->loaded&&artwork_->width>0&&artwork_->height>0){const float scale=std::min(260.0F/artwork_->width,260.0F/artwork_->height);const float w=artwork_->width*scale,h=artwork_->height*scale;c.image(*artwork_,1500+(260-w)/2,340+(260-h)/2,w,h);}
    if(!message_.empty())c.text(trophy_detail::elide(message_,125),180,841,19,{1,1,1,0.78F});
    host->render_message_footer(renderer,std::array{std::pair{action::ok,"Select"},std::pair{action::options,"Information"},std::pair{action::extra,"Refresh"},std::pair{action::cancel,"Back"}},1);
  }
private:
  template<class F>void start(trophy_detail::WorkKind kind,F task){
    if(busy_){message_="Please wait for the current request.";return;}busy_=true;cancel_=std::make_shared<std::atomic_bool>(false);auto cancelled=cancel_;
    std::promise<WorkResult> promise;work_=promise.get_future();std::thread([kind,task=std::move(task),cancelled,promise=std::move(promise)]() mutable {WorkResult result;result.kind=kind;try{result=task(cancelled);result.kind=kind;}catch(const std::exception& e){result.message=e.what();}catch(...){result.message="Could not load trophy data.";}try{promise.set_value(std::move(result));}catch(...) {}}).detach();
  }
  bool same_origin() const {
    if(origin_root_.empty()||origin_profile_.empty()||config::CONFIG.profileConfigPath!=origin_config_)return false;
    try{std::error_code e;if(!std::filesystem::is_directory(origin_root_,e)||e||!std::filesystem::exists(origin_root_/"profiles.json",e)||e||!std::filesystem::is_directory(origin_root_/"profiles"/origin_profile_,e)||e)return false;return openxmb::desktop::Library(origin_root_).active_profile().id==origin_profile_;}catch(...){return false;}
  }
  template<class F>bool save(F change){if(!same_origin()){message_="The active profile changed. Reopen Trophy Collection.";return false;}const auto previous=config::CONFIG.preferences;change(config::CONFIG.preferences);if(!config::CONFIG.save_config_checked()){config::CONFIG.preferences=previous;message_="Could not save profile settings.";return false;}return true;}
  void begin_profiles(){message_="Finding RPCS3 trophy profiles...";const auto configured=saved_root_;
    start(trophy_detail::WorkKind::profiles,[configured](auto cancelled){WorkResult out;std::vector<std::filesystem::path> roots;if(!configured.empty())roots.push_back(trophy_detail::text_path(configured));else for(const auto& game_root:openxmb::desktop::rpcs3_game_roots())if(game_root.filename()=="game"&&game_root.parent_path().filename()=="dev_hdd0"&&std::ranges::find(roots,game_root.parent_path())==roots.end())roots.push_back(game_root.parent_path());
      if(roots.size()>32)throw std::runtime_error("Too many RPCS3 data roots. Choose one in Settings.");for(const auto& root:roots){if(cancelled->load())break;auto users=openxmb::desktop::rpcs3_trophy_users(root);if(!users){out.message=users.error;continue;}for(const auto& user:users.values){if(out.profiles.size()>=64)throw std::runtime_error("Too many RPCS3 users. Choose one in Settings.");out.profiles.push_back({root,user});}}return out;});
  }
  void choose_profiles(std::vector<Profile> profiles){profiles_=std::move(profiles);if(profiles_.empty()){records_.clear();profile_={};profile_label_="No RPCS3 trophy profile selected";append_remote();if(message_.empty())message_="RPCS3 profiles were not found. Choose a data root in Settings.";return;}
    const auto saved=std::ranges::find_if(profiles_,[&](const auto& p){return trophy_detail::path_text(p.root)==saved_root_&&p.user==saved_user_;});
    if(saved!=profiles_.end()){select_profile(*saved,false);return;}if(profiles_.size()==1){select_profile(profiles_.front(),false);return;}prompt_profile();
  }
  void prompt_profile(){if(profiles_.empty()){message_="No RPCS3 users found in the selected data root.";return;}std::vector<std::string> labels;for(const auto& p:profiles_)labels.push_back(p.user+" — "+trophy_detail::path_text(p.root));auto choices=profiles_;auto weak=std::weak_ptr<std::atomic_bool>(alive_);
    host_->emplace_overlay<choice_overlay>(std::move(labels),0,[this,weak,choices](unsigned index){auto live=weak.lock();if(!live||!live->load()||index>=choices.size())return;select_profile(choices[index],true);});
  }
  void select_profile(const Profile& profile,bool explicit_choice){if(explicit_choice&&!save([&](auto& prefs){prefs["trophies.rpcs3.root"]=trophy_detail::path_text(profile.root);prefs["trophies.rpcs3.user"]=profile.user;}))return;
    profile_=profile;profile_label_="RPCS3 user "+profile.user+" · "+trophy_detail::path_text(profile.root);if(explicit_choice){saved_root_=trophy_detail::path_text(profile.root);saved_user_=profile.user;}load_sets();}
  void load_sets(){if(page_==Page::trophies&&selected_game_<records_.size())reload_game_id_=records_[selected_game_].game.game_id;else reload_game_id_.clear();const auto profile=profile_;message_="Loading local trophies...";start(trophy_detail::WorkKind::sets,[profile](auto cancelled){WorkResult out;auto sets=openxmb::desktop::rpcs3_trophy_sets(profile.root,profile.user);if(!sets){out.message=sets.error;return out;}if(sets.values.size()>256)throw std::runtime_error("Too many trophy sets in this profile.");for(const auto& id:sets.values){if(cancelled->load())break;auto read=openxmb::desktop::read_rpcs3_trophy_set(profile.root,profile.user,id);if(!read){if(out.message.empty())out.message="Some RPCS3 trophy sets could not be read: "+read.error;continue;}out.records.push_back({std::move(*read.value),{},false,true});}return out;});}
  void append_remote(){for(const auto id:ra_games_){Record value;value.remote=true;value.game.provider="retroachievements";value.game.user_id=ra_.ulid.empty()?ra_.username:ra_.ulid;value.game.game_id=std::to_string(id);value.game.title="RetroAchievements game #"+std::to_string(id);records_.push_back(std::move(value));}}
  void finish(WorkResult value){if(!value.message.empty())message_=value.message;
    switch(value.kind){case trophy_detail::WorkKind::profiles:choose_profiles(std::move(value.profiles));break;
    case trophy_detail::WorkKind::sets:records_=std::move(value.records);append_remote();if(page_==Page::trophies&&!reload_game_id_.empty()){auto found=std::ranges::find_if(records_,[&](const auto& r){return !r.remote&&r.game.game_id==reload_game_id_;});if(found==records_.end()){page_=Page::games;selected_=0;}else {selected_game_=static_cast<std::size_t>(found-records_.begin());selected_=std::min(selected_,found->game.entries.empty()?std::size_t{}:found->game.entries.size()-1);}}else selected_=std::min(selected_,records_.size());if(message_=="Loading local trophies...")message_=records_.empty()?"No trophy sets were found.":"Local trophy data loaded.";break;
    case trophy_detail::WorkKind::account:if(value.account.rate_limited)retry_after_=Clock::now()+std::chrono::seconds(60);if(!value.account){message_=value.account.error;break;}if(save([&](auto& prefs){prefs["achievements.ra.username"]=value.account.value->username;prefs["achievements.ra.ulid"]=value.account.value->ulid;})){ra_=*value.account.value;std::erase_if(records_,[](const auto& r){return r.remote;});append_remote();message_="RetroAchievements account linked.";}break;
    case trophy_detail::WorkKind::progress:{if(value.progress.rate_limited)retry_after_=Clock::now()+std::chrono::seconds(60);auto found=std::ranges::find_if(records_,[&](const auto& r){return r.remote&&r.game.game_id==std::to_string(value.game_id);});if(found==records_.end())break;if(!value.progress){message_=value.progress.error;break;}found->game=std::move(*value.progress.game);found->details=std::move(value.progress.details);found->loaded=true;message_=value.progress.warning.empty()?(value.progress.from_cache?(value.progress.stale?"Showing stale offline data.":"Showing cached data."):"Progress refreshed."):value.progress.warning;break;}}
  }
  std::optional<std::string> key(){if(!trophy_detail::env_name(key_env_)){message_="Set a valid API key environment variable name in Settings.";return {};}const char* p=std::getenv(key_env_.c_str());if(!p||!*p){message_="Set the "+key_env_+" environment variable to your RetroAchievements web API key.";return {};}return std::string(p);}
  void load_remote(unsigned id,bool online){if(ra_.username.empty()){message_="Link a RetroAchievements account in Settings.";return;}if(online&&Clock::now()<retry_after_){message_="Rate limited. Wait before trying again.";return;}std::string secret;if(online){auto value=key();if(!value)return;secret=std::move(*value);}const auto account=ra_;const auto cache=cache_root_;message_=online?"Refreshing RetroAchievements...":"Loading cached progress...";
    start(trophy_detail::WorkKind::progress,[account,cache,id,secret=std::move(secret),online](auto cancelled){WorkResult out;out.game_id=id;out.progress=openxmb::desktop::retroachievements_progress(cache,account,id,secret,trophy_detail::transport(cancelled),online);return out;});
  }
  void link(std::string username){if(Clock::now()<retry_after_){message_="Rate limited. Wait before trying again.";return;}auto secret=key();if(!secret)return;message_="Checking RetroAchievements account...";start(trophy_detail::WorkKind::account,[username=std::move(username),secret=std::move(*secret)](auto cancelled){WorkResult out;out.account=openxmb::desktop::retroachievements_account(username,secret,trophy_detail::transport(cancelled));return out;});}
  std::size_t row_count()const{if(page_==Page::settings)return 5;if(page_==Page::games)return records_.size()+1;if(selected_game_>=records_.size())return 0;return records_[selected_game_].game.entries.size();}
  std::pair<std::string,std::string> row_text(std::size_t i)const {
    if(page_==Page::settings){const std::array<std::pair<std::string,std::string>,5> rows{{{"RPCS3 data root",saved_root_.empty()?"Automatic discovery":saved_root_},{"RPCS3 user",profile_.user.empty()?"Select a profile":profile_.user},{"Link RetroAchievements",ra_.username.empty()?"No account linked":ra_.username},{"API key environment variable",key_env_},{"Add RetroAchievements game ID","Explicit numeric ID"}}};return rows.at(i);}
    if(page_==Page::games){if(i==records_.size())return {"Settings","Choose trophy sources and account"};const auto& r=records_.at(i);return {r.game.title,r.remote?"RetroAchievements":"RPCS3 · "+r.game.user_id};}
    const auto& a=records_.at(selected_game_).game.entries.at(i);if(trophy_detail::concealed(a))return {"Hidden trophy",trophy_detail::state(a)};auto detail=trophy_detail::grade(a.grade);if(a.points)detail=std::to_string(*a.points)+" points";if(!detail.empty())detail+=" · ";return {a.title,detail+trophy_detail::state(a)};
  }
  void activate(){if(page_==Page::games){if(selected_>=records_.size()){page_=Page::settings;selected_game_=selected_;selected_=0;return;}selected_game_=selected_;page_=Page::trophies;selected_=0;if(records_[selected_game_].remote&&!records_[selected_game_].loaded){unsigned id{};const auto& s=records_[selected_game_].game.game_id;auto parsed=std::from_chars(s.data(),s.data()+s.size(),id);if(parsed.ec==std::errc{})load_remote(id,false);}return;}
    if(page_==Page::trophies){show_information();return;}settings_action();}
  void settings_action(){auto weak=std::weak_ptr<std::atomic_bool>(alive_);
    if(selected_==0){host_->emplace_overlay<text_entry_overlay>("RPCS3 dev_hdd0 folder",saved_root_,[this,weak](std::string text){auto live=weak.lock();if(!live||!live->load())return;if(!text.empty()){std::error_code e;if(!std::filesystem::is_directory(trophy_detail::text_path(text),e)||e){message_="Choose an existing RPCS3 dev_hdd0 folder.";return;}}if(save([&](auto& p){p["trophies.rpcs3.root"]=text;p.erase("trophies.rpcs3.user");})){saved_root_=text;saved_user_.clear();records_.clear();profile_={};begin_profiles();}});}
    else if(selected_==1)prompt_profile();
    else if(selected_==2)host_->emplace_overlay<text_entry_overlay>("RetroAchievements user name",ra_.username,[this,weak](std::string value){auto live=weak.lock();if(live&&live->load())link(std::move(value));});
    else if(selected_==3)host_->emplace_overlay<text_entry_overlay>("API key environment variable",key_env_,[this,weak](std::string value){auto live=weak.lock();if(!live||!live->load())return;if(!trophy_detail::env_name(value)){message_="Enter an environment variable name, not the API key.";return;}if(save([&](auto& p){p["achievements.ra.key_env"]=value;})){key_env_=value;message_="Environment variable name saved.";}});
    else host_->emplace_overlay<text_entry_overlay>("RetroAchievements game ID","",[this,weak](std::string value){auto live=weak.lock();if(!live||!live->load())return;try{const auto ids=trophy_detail::game_ids(value);if(ids.size()!=1)throw std::runtime_error("Enter one numeric game ID.");auto updated=ra_games_;if(std::ranges::find(updated,ids.front())==updated.end())updated.push_back(ids.front());if(updated.size()>64)throw std::runtime_error("At most 64 game IDs are supported.");std::string serial;for(auto id:updated){if(!serial.empty())serial+=',';serial+=std::to_string(id);}if(save([&](auto& p){p["achievements.ra.games"]=serial;})){ra_games_=std::move(updated);std::erase_if(records_,[](const auto& r){return r.remote;});append_remote();message_="Game ID added.";}}catch(const std::exception& e){message_=e.what();}});
  }
  void refresh(){if(busy_){message_="Please wait for the current request.";return;}if(page_==Page::settings){begin_profiles();return;}const auto index=page_==Page::trophies?selected_game_:selected_;if(index<records_.size()&&records_[index].remote){unsigned id{};const auto& text=records_[index].game.game_id;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),id);if(parsed.ec==std::errc{})load_remote(id,true);}else if(!profile_.user.empty())load_sets();else begin_profiles();}
  void show_information(){if(page_==Page::settings)return;const auto index=page_==Page::trophies?selected_game_:selected_;if(index>=records_.size())return;const auto& record=records_[index];std::string title=record.game.title,text="Source: "+record.game.provider+"\nUser: "+record.game.user_id+"\nGame ID: "+record.game.game_id;
    if(page_==Page::trophies&&selected_<record.game.entries.size()){const auto& entry=record.game.entries[selected_];if(trophy_detail::concealed(entry)){title="Hidden trophy";text=trophy_detail::state(entry);}else {title=entry.title;text=entry.description+"\n"+trophy_detail::state(entry);const auto g=trophy_detail::grade(entry.grade);if(!g.empty())text+="\nGrade: "+g;if(entry.points)text+="\nPoints: "+std::to_string(*entry.points);if(record.remote){auto found=std::ranges::find_if(record.details,[&](const auto& d){return d.id==entry.id;});text+="\nSoftcore: "+trophy_detail::date(found==record.details.end()?std::nullopt:found->softcore_at);text+="\nHardcore: "+trophy_detail::date(found==record.details.end()?std::nullopt:found->hardcore_at);}else text+="\nUnlocked: "+trophy_detail::date(entry.unlocked_at);}}
    else for(const auto& warning:record.game.warnings)text+="\n"+warning;host_->emplace_overlay<message_overlay>(title,text,std::vector<std::string>{"OK"});}
  std::filesystem::path desired_artwork()const{if(page_==Page::games&&selected_<records_.size())return records_[selected_].game.icon;if(page_==Page::trophies&&selected_game_<records_.size()&&selected_<records_[selected_game_].game.entries.size()){const auto& a=records_[selected_game_].game.entries[selected_];if(!trophy_detail::concealed(a))return a.icon;}return {};}
  void update_artwork(){const auto desired=desired_artwork();if(artwork_loading_.valid()&&artwork_loading_.wait_for(std::chrono::seconds(0))==std::future_status::ready){try{artwork_loading_.get();}catch(...){}if(loading_&&loading_->loaded&&loading_path_==desired){if(host_)host_->get_window()->waitIdle();artwork_=std::move(loading_);artwork_path_=loading_path_;}else loading_.reset();loading_path_.clear();}
    if(artwork_&&artwork_path_!=desired){if(host_)host_->get_window()->waitIdle();artwork_.reset();artwork_path_.clear();}
    if(desired!=attempted_)attempted_.clear();if(desired.empty()||artwork_||artwork_loading_.valid()||desired==attempted_)return;attempted_=desired;std::error_code e;if(!std::filesystem::is_regular_file(desired,e)||e)return;try{loading_=std::make_shared<auroreengine::texture>(loader_.getDevice(),loader_.getAllocator());loading_path_=desired;artwork_loading_=loader_.loadTexture(loading_.get(),desired);}catch(...){loading_.reset();loading_path_.clear();}}
  auroreengine::resource_loader& loader_;shell* host_{};std::shared_ptr<std::atomic_bool> alive_,cancel_;std::future<WorkResult> work_;bool busy_{},closing_{},origin_valid_{true};
  std::filesystem::path origin_root_,origin_config_,cache_root_;std::string origin_profile_,saved_root_,saved_user_,key_env_,profile_label_,message_,reload_game_id_;std::vector<unsigned> ra_games_;openxmb::desktop::RetroAccount ra_;Profile profile_;std::vector<Profile> profiles_;std::vector<Record> records_;Page page_{Page::games};std::size_t selected_{},selected_game_{};Clock::time_point retry_after_{},last_origin_check_{};
  std::shared_ptr<auroreengine::texture> artwork_,loading_;std::future<void> artwork_loading_;std::filesystem::path artwork_path_,loading_path_,attempted_;
};
} // namespace app
