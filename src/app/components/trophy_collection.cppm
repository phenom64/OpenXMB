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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <ranges>
#include <span>
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
import openxmb.xmb.root_scene;
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
// Last two folder names, e.g. "rpcs3/dev_hdd0"; the full path stays in Information.
std::string short_path(const std::string& text){auto path=text_path(text);if(path.filename().empty())path=path.parent_path();const auto name=path_text(path.filename()),parent=path_text(path.parent_path().filename());return parent.empty()?name:parent+"/"+name;}
std::vector<unsigned> game_ids(std::string_view text){
  std::vector<unsigned> values;std::size_t start=0;
  while(start<text.size()) {auto end=text.find(',',start);auto part=text.substr(start,end==text.npos?text.size()-start:end-start);unsigned id{};auto parsed=std::from_chars(part.data(),part.data()+part.size(),id);
    if(part.empty()||parsed.ec!=std::errc{}||parsed.ptr!=part.data()+part.size()||!id||values.size()>=64)throw std::runtime_error("Use up to 64 comma-separated numeric game IDs.");
    if(std::ranges::find(values,id)==values.end())values.push_back(id);if(end==text.npos)break;start=end+1;
  }return values;
}
bool env_name(std::string_view name){if(name.empty()||name.size()>128||!((name[0]>='A'&&name[0]<='Z')||(name[0]>='a'&&name[0]<='z')||name[0]=='_'))return false;return std::all_of(name.begin(),name.end(),[](char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';});}
// The PS3 printed earned dates as "2008/6/11 8:42". Provider times are UTC and
// the shell's configured time zone is not exposed here, so the zone is stated.
std::string date(std::optional<std::int64_t> value){if(!value)return "Unavailable";const auto raw=static_cast<std::time_t>(*value);if(static_cast<std::int64_t>(raw)!=*value)return "Unavailable";std::tm tm{};
#ifdef _WIN32
  if(gmtime_s(&tm,&raw)!=0)return "Unavailable";
#else
  if(!gmtime_r(&raw,&tm))return "Unavailable";
#endif
  return std::to_string(tm.tm_year+1900)+"/"+std::to_string(tm.tm_mon+1)+"/"+std::to_string(tm.tm_mday)+" "+std::to_string(tm.tm_hour)+":"+(tm.tm_min<10?"0":"")+std::to_string(tm.tm_min)+" UTC";
}
std::string grade(desktop::TrophyGrade value){switch(value){case desktop::TrophyGrade::platinum:return "Platinum";case desktop::TrophyGrade::gold:return "Gold";case desktop::TrophyGrade::silver:return "Silver";case desktop::TrophyGrade::bronze:return "Bronze";default:return {};}}
bool concealed(const desktop::Achievement& a){return a.hidden&&(!a.unlock_state_known||!a.unlocked);}
bool earned(const desktop::Achievement& a){return a.unlock_state_known&&a.unlocked;}
std::string state(const desktop::Achievement& a){if(!a.unlock_state_known)return "Unlock state unavailable";if(!a.unlocked)return "Locked";return a.hardcore==true?"Unlocked · Hardcore":"Unlocked";}
desktop::HttpTransport transport(std::shared_ptr<std::atomic_bool> cancelled) {
  return [cancelled](const desktop::HttpRequest& original){if(cancelled->load())return desktop::HttpResponse{false,0,{},"Cancelled"};auto req=original;auto previous=req.progress;req.progress=[cancelled,previous](std::uint64_t n,std::uint64_t total){return !cancelled->load()&&(!previous||previous(n,total));};return desktop::http_get(req);};
}
// Highest grade first, the order of the PS3 manual's grade chart.
inline constexpr std::array<desktop::TrophyGrade,4> grade_order{desktop::TrophyGrade::platinum,desktop::TrophyGrade::gold,desktop::TrophyGrade::silver,desktop::TrophyGrade::bronze};
int grade_slot(desktop::TrophyGrade value){switch(value){case desktop::TrophyGrade::platinum:return 0;case desktop::TrophyGrade::gold:return 1;case desktop::TrophyGrade::silver:return 2;case desktop::TrophyGrade::bronze:return 3;default:return -1;}}
// Grade metals sampled from the PS3 manual's grade chart (trophy001.jpg) as
// sRGB; grey stands for a concealed or absent grade.
glm::vec4 grade_colour(desktop::TrophyGrade value,float alpha){
  glm::vec4 srgb{0.55F,0.55F,0.55F,alpha};
  switch(value){case desktop::TrophyGrade::platinum:srgb={0.66F,0.78F,0.88F,alpha};break;case desktop::TrophyGrade::gold:srgb={0.90F,0.72F,0.28F,alpha};break;
    case desktop::TrophyGrade::silver:srgb={0.80F,0.80F,0.82F,alpha};break;case desktop::TrophyGrade::bronze:srgb={0.80F,0.50F,0.30F,alpha};break;default:break;}
  return dialog_chrome::linear(srgb);
}
// Counts over real provider records only. Unknown unlock states are counted
// separately and never as earned.
struct Tally {std::array<unsigned,4> earned{},total{};unsigned entries{},known{},unlocked{},hardcore{};std::uint64_t points{},points_total{};};
Tally tally(const desktop::AchievementGame& game){
  Tally t;for(const auto& a:game.entries){++t.entries;const int slot=grade_slot(a.grade);if(slot>=0)++t.total[slot];if(a.points)t.points_total+=*a.points;
    if(!a.unlock_state_known)continue;++t.known;if(!a.unlocked)continue;++t.unlocked;if(slot>=0)++t.earned[slot];if(a.points)t.points+=*a.points;if(a.hardcore==true)++t.hardcore;}
  return t;
}
void add(Tally& sum,const Tally& t){for(std::size_t i=0;i<4;++i){sum.earned[i]+=t.earned[i];sum.total[i]+=t.total[i];}sum.entries+=t.entries;sum.known+=t.known;sum.unlocked+=t.unlocked;sum.hardcore+=t.hardcore;sum.points+=t.points;sum.points_total+=t.points_total;}
// Earned share by count, as RPCS3's own trophy manager reports it. Withheld
// while any unlock state is unknown rather than guessed.
std::optional<unsigned> percent(const Tally& t){if(!t.entries||t.known!=t.entries)return std::nullopt;return static_cast<unsigned>(std::uint64_t{t.unlocked}*100/t.entries);}
// Icons for the visible rows. A texture is destroyed only after the GPU is
// idle, and a load owns its destination until its future completes.
class icon_cache {
  struct entry {std::shared_ptr<auroreengine::texture> image;std::future<void> ready;std::uint64_t used{};bool failed{};};
  std::map<fs::path,entry> items_;std::uint64_t frame_{};
  static constexpr std::size_t capacity=48,retain=36,in_flight=4;
public:
  icon_cache()=default;icon_cache(const icon_cache&)=delete;icon_cache& operator=(const icon_cache&)=delete;
  ~icon_cache(){wait();}
  void wait(){for(auto& [path,item]:items_)if(item.ready.valid())item.ready.wait();}
  void next_frame(){++frame_;}
  void poll(){
    for(auto& [path,item]:items_){
      if(!item.ready.valid()||item.ready.wait_for(std::chrono::seconds(0))!=std::future_status::ready)continue;
      try{item.ready.get();}catch(...){item.failed=true;}
      if(item.failed||!item.image||!item.image->loaded||item.image->width<=0||item.image->height<=0){item.failed=true;item.image.reset();}
    }
  }
  void request(auroreengine::resource_loader& loader,const fs::path& path){
    if(path.empty())return;
    if(const auto found=items_.find(path);found!=items_.end()){found->second.used=frame_;return;}
    if(static_cast<std::size_t>(std::ranges::count_if(items_,[](const auto& item){return item.second.ready.valid();}))>=in_flight)return;
    entry item;item.used=frame_;std::error_code error;
    if(!fs::is_regular_file(path,error)||error)item.failed=true;
    else try{item.image=std::make_shared<auroreengine::texture>(loader.getDevice(),loader.getAllocator());item.ready=loader.loadTexture(item.image.get(),path);}catch(...){item.image.reset();item.failed=true;}
    items_.emplace(path,std::move(item));
  }
  [[nodiscard]] const auroreengine::texture* find(const fs::path& path) const {
    if(path.empty())return nullptr;const auto found=items_.find(path);
    if(found==items_.end()||found->second.failed||found->second.ready.valid()||!found->second.image)return nullptr;
    return found->second.image.get();
  }
  // Failed lookups are retried after Refresh, when files may have appeared.
  void forget_failed(){std::erase_if(items_,[](const auto& item){return item.second.failed;});}
  void trim(auroreengine::resource_loader& loader,auroreengine::window* window){
    if(items_.size()<=capacity)return;
    std::vector<std::map<fs::path,entry>::iterator> stale;
    for(auto it=items_.begin();it!=items_.end();++it)if(!it->second.ready.valid()&&it->second.used<frame_)stale.push_back(it);
    std::ranges::sort(stale,{},[](const auto& it){return it->second.used;});
    std::size_t excess=items_.size()-retain;bool idle=false;
    for(const auto& it:stale){if(!excess)break;if(it->second.image&&!idle){if(window)window->waitIdle();else loader.getDevice().waitIdle();idle=true;}items_.erase(it);--excess;}
  }
};
}
namespace trophy_view {
// 1920x1080 design-space geometry measured from the PS3 User's Guide Trophy
// Collection screenshots (manuals.playstation.net imgs/trophy003.jpg and
// trophy004.jpg). trophy004 is a 304 px wide full-screen capture, so one
// source pixel spans ~6.3 design pixels: treat values as +/-6 px. The focus
// centre and row pitch reuse the XMB constants they matched.
inline constexpr float focus_y=static_cast<float>(openxmb::xmb::kItemFocusY); // measured ~505
inline constexpr float pitch=static_cast<float>(openxmb::xmb::kItemPitch);    // measured 79-82
inline constexpr float focus_gap=175.0F;          // focused centre to either neighbour (174-176)
inline constexpr float parent_x=266.0F;           // cup on the game list, the game's ICON0 on its trophies
inline constexpr float parent_icon_extent=150.0F; // the theme cup drawn ~88 px wide
inline constexpr float games_arrow_x=426.0F,trophies_arrow_x=476.0F;
inline constexpr float game_icon_x=628.0F,game_label_x=808.0F;
inline constexpr float game_icon_focus_h=176.0F,game_icon_h=80.0F,icon0_aspect=320.0F/176.0F; // ICON0 is 1:1 when focused
inline constexpr float trophy_icon_x=620.0F,trophy_icon_focus=127.0F,trophy_icon=76.0F;
inline constexpr float glyph_x=737.0F,trophy_label_x=759.0F;
inline constexpr float percent_x=1684.0F,percent_left=1580.0F,text_right=1840.0F;
inline constexpr float bar_x=1604.0F,bar_w=158.0F,bar_h=7.0F,bar_drop=32.0F;
inline constexpr float title_drop=10.0F,tag_drop=35.0F,row_tag_drop=30.0F,percent_drop=9.0F;
inline constexpr float detail_title_drop=-17.0F,detail_pitch=24.0F,detail_line=27.0F;
inline constexpr float focus_title_size=28.0F,row_title_size=26.0F,detail_size=20.0F,percent_size=26.0F,points_size=22.0F;
inline constexpr float glyph_scale=0.86F,percent_glyph_h=24.0F,percent_glyph_gap=14.0F;
inline constexpr float idle_alpha=0.85F;
inline constexpr float tag_height=20.0F,tag_size=15.0F,tag_pad=6.0F,tag_gap=12.0F;
// Summary panel: where trophy004 shows its profile box (x 1560-1806, y 95-230),
// widened leftwards so four grade counts fit. The material follows the profile
// screen's Level and Trophies boxes (profile002.jpg): a glossy grey header
// strip over a dark body.
inline constexpr float panel_x=1506.0F,panel_y=95.0F,panel_w=300.0F,panel_h=136.0F,panel_header=30.0F,panel_radius=6.0F;
inline constexpr float panel_slot=(panel_w-24.0F)/4.0F;
// Rows beyond the second neighbour above fade beneath the panel; rows below
// fade before the status line and the controller hints.
inline constexpr float top_fade=140.0F,top_clear=240.0F,bottom_clear=840.0F,bottom_fade=905.0F;
inline constexpr float status_y=930.0F,status_size=20.0F,status_width=1500.0F;
inline constexpr float page_slide=120.0F;
inline constexpr float focus_seconds=static_cast<float>(openxmb::xmb::kItemTransitionSeconds);
inline constexpr float page_seconds=static_cast<float>(openxmb::xmb::kCategoryTransitionSeconds);
// The OpenXMB settings page follows the XMB nested settings list.
inline constexpr float settings_icon_x=static_cast<float>(openxmb::xmb::kItemIconX),settings_label_x=static_cast<float>(openxmb::xmb::kItemLabelX);
inline constexpr float settings_icon=static_cast<float>(openxmb::xmb::kInactiveItemIconExtent),settings_icon_focus=static_cast<float>(openxmb::xmb::kActiveItemIconExtent);
inline constexpr float settings_label_size=static_cast<float>(openxmb::xmb::kInactiveItemLabelSize),settings_label_focus=static_cast<float>(openxmb::xmb::kActiveItemLabelSize);
inline constexpr float settings_idle_alpha=static_cast<float>(openxmb::xmb::kInactiveItemAlpha);
inline constexpr float settings_above_y=120.0F,settings_next_padding=static_cast<float>(openxmb::xmb::kSelectedToNextPadding);
inline constexpr float settings_value_right=1840.0F,settings_value_size=26.0F,settings_value_width=600.0F,settings_description_size=22.0F;
inline constexpr std::size_t settings_rows=5;

inline float text_width(const xmb_canvas& c,std::string_view text,float size){return c.renderer.text_width_pixels(text,size*c.scale*config::CONFIG.textScale)/c.scale;}
inline std::size_t utf8_floor(std::string_view text,std::size_t length){while(length>0&&length<text.size()&&(static_cast<unsigned char>(text[length])&0xC0U)==0x80U)--length;return length;}
inline std::string fit(const xmb_canvas& c,std::string_view text,float size,float max_width){
  if(text_width(c,text,size)<=max_width)return std::string(text);
  std::size_t low=0,high=text.size();
  while(low<high){const std::size_t mid=(low+high+1)/2;std::string candidate(text.substr(0,utf8_floor(text,mid)));candidate+="...";if(text_width(c,candidate,size)<=max_width)low=mid;else high=mid-1;}
  std::string out(text.substr(0,utf8_floor(text,low)));while(!out.empty()&&out.back()==' ')out.pop_back();return out+"...";
}
inline std::vector<std::string> wrap(const xmb_canvas& c,std::string_view text,float size,float max_width,std::size_t lines){
  std::vector<std::string> words;std::size_t start=0;
  while(start<text.size()){const auto end=text.find_first_of(" \t\r\n",start);const auto word=text.substr(start,end==text.npos?text.npos:end-start);if(!word.empty())words.emplace_back(word);if(end==text.npos)break;start=end+1;}
  std::vector<std::string> out;std::string line;
  for(std::size_t i=0;i<words.size();++i){
    std::string candidate=line.empty()?words[i]:line+' '+words[i];
    if(line.empty()||text_width(c,candidate,size)<=max_width){line=std::move(candidate);continue;}
    if(out.size()+1>=lines){for(;i<words.size();++i)line+=' '+words[i];break;}
    out.push_back(fit(c,line,size,max_width));line=words[i];
  }
  if(!line.empty())out.push_back(fit(c,line,size,max_width));
  return out;
}
inline glm::vec2 point(const xmb_canvas& c,glm::vec2 p){return {(c.ox+p.x*c.scale)/c.renderer.frame_size.width,(c.oy+p.y*c.scale)/c.renderer.frame_size.height};}
// Small grade cup drawn as geometry (no Sony artwork): ring handles, a
// half-ellipse bowl fanned from its rim, a stem, collar and plinth.
inline void cup(const xmb_canvas& c,float cx,float top,float h,glm::vec4 colour){
  using V=auroreengine::simple_renderer::vertex_data;
  const float w=h*0.66F,bowl=h*0.46F;
  c.shape(cx-w*0.66F,top+h*0.03F,w*0.40F,h*0.30F,colour,h*0.15F,h*0.05F,0.0F);
  c.shape(cx+w*0.26F,top+h*0.03F,w*0.40F,h*0.30F,colour,h*0.15F,h*0.05F,0.0F);
  constexpr int steps=12;std::vector<V> vertices;vertices.reserve(steps*3);
  const auto rim=[&](int i){const float t=std::numbers::pi_v<float>*static_cast<float>(i)/static_cast<float>(steps);return point(c,{cx+w*0.5F*std::cos(t),top+bowl*std::sin(t)});};
  const auto centre=point(c,{cx,top});
  for(int i=0;i<steps;++i){vertices.push_back(V{centre,colour,{0,0}});vertices.push_back(V{rim(i),colour,{1,0}});vertices.push_back(V{rim(i+1),colour,{0,1}});}
  c.renderer.draw_generic(vertices);
  c.rect(cx-h*0.05F,top+bowl,h*0.10F,h*0.22F,colour);
  c.shape(cx-w*0.28F,top+h*0.68F,w*0.56F,h*0.09F,colour,h*0.02F);
  c.shape(cx-w*0.38F,top+h*0.80F,w*0.76F,h*0.20F,colour,h*0.03F);
}
// Unearned trophies show a light grey placeholder instead of their image
// (trophy003.jpg; the manual: "Trophy images are displayed for the trophies
// that you have earned"). An earned trophy without a local image (all
// RetroAchievements badges, which stay remote) gets a dark variant.
inline void placeholder(const xmb_canvas& c,float cx,float cy,float size,bool earned,float alpha){
  const auto grey=[&](float s,float a){return dialog_chrome::linear(glm::vec4{s,s,s,a*alpha});};
  const float x=cx-size*0.5F,y=cy-size*0.5F;
  const std::array<std::pair<float,glm::vec4>,2> face{{{0.0F,grey(earned?0.40F:0.80F,1.0F)},{1.0F,grey(earned?0.22F:0.56F,1.0F)}}};
  c.gradient_shape(x,y,size,size,face,size*0.05F);
  cup(c,cx,y+size*0.22F,size*0.56F,earned?grey(0.95F,0.9F):grey(0.38F,0.85F));
}
// Glossy track with an olive fill, sampled from the 42% bar in trophy002.jpg.
inline void bar(const xmb_canvas& c,float x,float y,float w,float h,float value,float alpha){
  const auto grey=[&](float s){return dialog_chrome::linear(glm::vec4{s,s,s,alpha});};
  const auto tint=[&](float r,float g,float b){return dialog_chrome::linear(glm::vec4{r,g,b,alpha});};
  const std::array<std::pair<float,glm::vec4>,4> track{{{0.0F,grey(0.45F)},{0.3F,grey(0.31F)},{0.7F,grey(0.41F)},{1.0F,grey(0.36F)}}};
  c.gradient_shape(x,y,w,h,track,h*0.5F);
  value=std::clamp(value,0.0F,1.0F);if(value<=0.0F)return;
  const std::array<std::pair<float,glm::vec4>,4> fill{{{0.0F,tint(0.59F,0.62F,0.45F)},{0.35F,tint(0.55F,0.67F,0.16F)},{0.7F,tint(0.55F,0.64F,0.27F)},{1.0F,tint(0.34F,0.35F,0.36F)}}};
  c.gradient_shape(x,y,std::max(h,w*value),h,fill,h*0.5F);
}
// Small framed label under a game title, after the tags in trophy004.jpg.
inline float tag(const xmb_canvas& c,float x,float cy,std::string_view label,float alpha){
  const float w=text_width(c,label,tag_size)+2.0F*tag_pad,top=cy-tag_height*0.5F;
  c.rect(x,top,w,tag_height,{0,0,0,dialog_chrome::shade_alpha(0.30F)*alpha},2.0F);
  dialog_chrome::edge_glow(c,x,top,w,tag_height,0.55F,0.12F,1.0F,1.0F,alpha,2.0F);
  c.text(label,x+tag_pad,cy+tag_size*0.36F,tag_size,{1,1,1,0.92F*alpha});
  return w;
}
inline void back_arrow(const xmb_canvas& c,float x,float y,float alpha){setup_controls::triangle(c,{x-6.0F,y},{x+6.0F,y-8.0F},{x+6.0F,y+8.0F},{1,1,1,0.75F*alpha});}
inline void notice(const xmb_canvas& c,float x,float y,std::string_view title,std::string_view hint,float alpha){
  c.text(title,x,y+title_drop,focus_title_size,{1,1,1,0.92F*alpha});
  if(!hint.empty())c.text(hint,x,y+title_drop+32.0F,detail_size,{1,1,1,0.72F*alpha});
}
inline float list_fade(float y){return dialog_chrome::smooth01((y-top_fade)/(top_clear-top_fade))*dialog_chrome::smooth01((bottom_fade-y)/(bottom_fade-bottom_clear));}
inline float bottom_only_fade(float y){return dialog_chrome::smooth01((bottom_fade-y)/(bottom_fade-bottom_clear));}
inline float ease(float t){const float u=1.0F-std::clamp(t,0.0F,1.0F);return 1.0F-u*u*u;}
}
class trophy_collection final : public component, public action_receiver {
  using Clock=std::chrono::steady_clock;
  using WorkResult=trophy_detail::WorkResult;
  using Record=trophy_detail::Record;
  using Profile=trophy_detail::Profile;
  using Tally=trophy_detail::Tally;
  using Hint=std::pair<::action,std::string_view>;
  enum class Page {games,trophies,settings};
  struct SettingRow {std::string label,value,description;};
public:
  explicit trophy_collection(auroreengine::resource_loader& loader):loader_(loader),alive_(std::make_shared<std::atomic_bool>(true)) {
    try{for(const auto* name:{"theme/icon_trophy","theme/icon_game","theme/icon_setting","theme/icon_setting_item"})icons_.load(loader_,name);}catch(...){}
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
    textures_.wait();
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
    if(tallies_dirty_||tallies_.size()!=records_.size())retally();
    update_icons();return closing_?result::close|result::back_sound:result::success;
  }
  result on_action(action input) override {
    if(input==action::cancel){if(page_==Page::games){closing_=true;if(cancel_)cancel_->store(true);}else if(page_==Page::trophies){page_=Page::games;selected_=selected_game_;paged(-1);}else {page_=Page::games;selected_=std::min(selected_game_,records_.size());paged(-1);}return result::success|result::back_sound;}
    const auto count=row_count();if(input==action::up&&count){moved(selected_);selected_=(selected_+count-1)%count;return result::success;}
    if(input==action::down&&count){moved(selected_);selected_=(selected_+1)%count;return result::success;}
    if(!origin_valid_||!host_)return result::success;
    if(input==action::options){show_information();return result::success;}
    if(input==action::extra){refresh();return result::success;}
    if(input==action::ok){if(busy_)message_="Please wait for the current request.";else activate();return result::success|result::ok_sound;}
    return result::unsupported;
  }
  // The PS3 draws Trophy Collection inside the XMB (manual trophy002-004): the
  // parent icon steps left behind a small arrow, the list hangs off the focus
  // row and focus is the enlarged icon and a breathing label, not a box.
  void render(auroreengine::gui_renderer& renderer,shell* host) override {
    const xmb_canvas c(renderer);
    const float entrance=page_amount();
    const float dx=(1.0F-entrance)*trophy_view::page_slide*static_cast<float>(page_dir_);
    draw_parent(c,dx*0.5F,entrance);
    if(page_==Page::games)draw_games(c,dx,entrance);else if(page_==Page::trophies)draw_trophies(c,dx,entrance);else draw_settings(c,dx,entrance);
    if(page_!=Page::settings)draw_panel(c);
    if(!message_.empty())c.text(trophy_view::fit(c,message_,trophy_view::status_size,trophy_view::status_width),960,trophy_view::status_y,trophy_view::status_size,{1,1,1,0.8F},0,true);
    if(page_==Page::trophies)render_controller_buttons(host,renderer,0.5F,0.9F,std::array{Hint{action::ok,"Information"},Hint{action::extra,"Refresh"},Hint{action::cancel,"Back"}});
    else render_controller_buttons(host,renderer,0.5F,0.9F,std::array{Hint{action::ok,"Select"},Hint{action::options,"Information"},Hint{action::extra,"Refresh"},Hint{action::cancel,"Back"}});
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
    const auto selected_executable=trophy_detail::text_path(config::CONFIG.preference("runner.rpcs3.path"));
    start(trophy_detail::WorkKind::profiles,[configured,selected_executable](auto cancelled){WorkResult out;std::vector<std::filesystem::path> roots;if(!configured.empty())roots.push_back(trophy_detail::text_path(configured));else for(const auto& game_root:openxmb::desktop::rpcs3_game_roots(selected_executable))if(game_root.filename()=="game"&&game_root.parent_path().filename()=="dev_hdd0"&&std::ranges::find(roots,game_root.parent_path())==roots.end())roots.push_back(game_root.parent_path());
      if(roots.size()>32)throw std::runtime_error("Too many RPCS3 data roots. Choose one in Settings.");for(const auto& root:roots){if(cancelled->load())break;auto users=openxmb::desktop::rpcs3_trophy_users(root);if(!users){out.message=users.error;continue;}for(const auto& user:users.values){if(out.profiles.size()>=64)throw std::runtime_error("Too many RPCS3 users. Choose one in Settings.");out.profiles.push_back({root,user});}}return out;});
  }
  void choose_profiles(std::vector<Profile> profiles){profiles_=std::move(profiles);if(profiles_.empty()){records_.clear();profile_={};append_remote();if(message_.empty())message_="RPCS3 profiles were not found. Choose a data root in Settings.";return;}
    const auto saved=std::ranges::find_if(profiles_,[&](const auto& p){return trophy_detail::path_text(p.root)==saved_root_&&p.user==saved_user_;});
    if(saved!=profiles_.end()){select_profile(*saved,false);return;}if(profiles_.size()==1){select_profile(profiles_.front(),false);return;}prompt_profile();
  }
  void prompt_profile(){if(profiles_.empty()){message_="No RPCS3 users found in the selected data root.";return;}std::vector<std::string> labels;for(const auto& p:profiles_)labels.push_back(p.user+" — "+trophy_detail::path_text(p.root));auto choices=profiles_;auto weak=std::weak_ptr<std::atomic_bool>(alive_);
    host_->emplace_overlay<choice_overlay>(std::move(labels),0,[this,weak,choices](unsigned index){auto live=weak.lock();if(!live||!live->load()||index>=choices.size())return;select_profile(choices[index],true);});
  }
  void select_profile(const Profile& profile,bool explicit_choice){if(explicit_choice&&!save([&](auto& prefs){prefs["trophies.rpcs3.root"]=trophy_detail::path_text(profile.root);prefs["trophies.rpcs3.user"]=profile.user;}))return;
    profile_=profile;if(explicit_choice){saved_root_=trophy_detail::path_text(profile.root);saved_user_=profile.user;}load_sets();}
  void load_sets(){if(page_==Page::trophies&&selected_game_<records_.size())reload_game_id_=records_[selected_game_].game.game_id;else reload_game_id_.clear();const auto profile=profile_;message_="Loading local trophies...";start(trophy_detail::WorkKind::sets,[profile](auto cancelled){WorkResult out;auto sets=openxmb::desktop::rpcs3_trophy_sets(profile.root,profile.user);if(!sets){out.message=sets.error;return out;}if(sets.values.size()>256)throw std::runtime_error("Too many trophy sets in this profile.");for(const auto& id:sets.values){if(cancelled->load())break;auto read=openxmb::desktop::read_rpcs3_trophy_set(profile.root,profile.user,id);if(!read){if(out.message.empty())out.message="Some RPCS3 trophy sets could not be read: "+read.error;continue;}out.records.push_back({std::move(*read.value),{},false,true});}return out;});}
  void append_remote(){for(const auto id:ra_games_){Record value;value.remote=true;value.game.provider="retroachievements";value.game.user_id=ra_.ulid.empty()?ra_.username:ra_.ulid;value.game.game_id=std::to_string(id);value.game.title="RetroAchievements game #"+std::to_string(id);records_.push_back(std::move(value));}tallies_dirty_=true;}
  void finish(WorkResult value){if(!value.message.empty())message_=value.message;
    switch(value.kind){case trophy_detail::WorkKind::profiles:choose_profiles(std::move(value.profiles));break;
    case trophy_detail::WorkKind::sets:records_=std::move(value.records);append_remote();if(page_==Page::trophies&&!reload_game_id_.empty()){auto found=std::ranges::find_if(records_,[&](const auto& r){return !r.remote&&r.game.game_id==reload_game_id_;});if(found==records_.end()){page_=Page::games;selected_=0;}else {selected_game_=static_cast<std::size_t>(found-records_.begin());selected_=std::min(selected_,found->game.entries.empty()?std::size_t{}:found->game.entries.size()-1);}}else selected_=std::min(selected_,records_.size());if(message_=="Loading local trophies...")message_=records_.empty()?"No trophy sets were found.":"Local trophy data loaded.";break;
    case trophy_detail::WorkKind::account:if(value.account.rate_limited)retry_after_=Clock::now()+std::chrono::seconds(60);if(!value.account){message_=value.account.error;break;}if(save([&](auto& prefs){prefs["achievements.ra.username"]=value.account.value->username;prefs["achievements.ra.ulid"]=value.account.value->ulid;})){ra_=*value.account.value;std::erase_if(records_,[](const auto& r){return r.remote;});append_remote();message_="RetroAchievements account linked.";}break;
    case trophy_detail::WorkKind::progress:{if(value.progress.rate_limited)retry_after_=Clock::now()+std::chrono::seconds(60);auto found=std::ranges::find_if(records_,[&](const auto& r){return r.remote&&r.game.game_id==std::to_string(value.game_id);});if(found==records_.end())break;if(!value.progress){message_=value.progress.error;break;}found->game=std::move(*value.progress.game);found->details=std::move(value.progress.details);found->loaded=true;tallies_dirty_=true;message_=value.progress.warning.empty()?(value.progress.from_cache?(value.progress.stale?"Showing stale offline data.":"Showing cached data."):"Progress refreshed."):value.progress.warning;break;}}
  }
  std::optional<std::string> key(){if(!trophy_detail::env_name(key_env_)){message_="Set a valid API key environment variable name in Settings.";return {};}const char* p=std::getenv(key_env_.c_str());if(!p||!*p){message_="Set the "+key_env_+" environment variable to your RetroAchievements web API key.";return {};}return std::string(p);}
  void load_remote(unsigned id,bool online){if(ra_.username.empty()){message_="Link a RetroAchievements account in Settings.";return;}if(online&&Clock::now()<retry_after_){message_="Rate limited. Wait before trying again.";return;}std::string secret;if(online){auto value=key();if(!value)return;secret=std::move(*value);}const auto account=ra_;const auto cache=cache_root_;message_=online?"Refreshing RetroAchievements...":"Loading cached progress...";
    start(trophy_detail::WorkKind::progress,[account,cache,id,secret=std::move(secret),online](auto cancelled){WorkResult out;out.game_id=id;out.progress=openxmb::desktop::retroachievements_progress(cache,account,id,secret,trophy_detail::transport(cancelled),online);return out;});
  }
  void link(std::string username){if(Clock::now()<retry_after_){message_="Rate limited. Wait before trying again.";return;}auto secret=key();if(!secret)return;message_="Checking RetroAchievements account...";start(trophy_detail::WorkKind::account,[username=std::move(username),secret=std::move(*secret)](auto cancelled){WorkResult out;out.account=openxmb::desktop::retroachievements_account(username,secret,trophy_detail::transport(cancelled));return out;});}
  std::size_t row_count()const{if(page_==Page::settings)return trophy_view::settings_rows;if(page_==Page::games)return records_.size()+1;if(selected_game_>=records_.size())return 0;return records_[selected_game_].game.entries.size();}
  void activate(){if(page_==Page::games){if(selected_>=records_.size()){page_=Page::settings;selected_game_=selected_;selected_=0;paged(1);return;}selected_game_=selected_;page_=Page::trophies;selected_=0;paged(1);if(records_[selected_game_].remote&&!records_[selected_game_].loaded){unsigned id{};const auto& s=records_[selected_game_].game.game_id;auto parsed=std::from_chars(s.data(),s.data()+s.size(),id);if(parsed.ec==std::errc{})load_remote(id,false);}return;}
    if(page_==Page::trophies){show_information();return;}settings_action();}
  void settings_action(){auto weak=std::weak_ptr<std::atomic_bool>(alive_);
    if(selected_==0){host_->emplace_overlay<text_entry_overlay>("RPCS3 dev_hdd0 folder",saved_root_,[this,weak](std::string text){auto live=weak.lock();if(!live||!live->load())return;if(!text.empty()){std::error_code e;if(!std::filesystem::is_directory(trophy_detail::text_path(text),e)||e){message_="Choose an existing RPCS3 dev_hdd0 folder.";return;}}if(save([&](auto& p){p["trophies.rpcs3.root"]=text;p.erase("trophies.rpcs3.user");})){saved_root_=text;saved_user_.clear();records_.clear();profile_={};begin_profiles();}});}
    else if(selected_==1)prompt_profile();
    else if(selected_==2)host_->emplace_overlay<text_entry_overlay>("RetroAchievements user name",ra_.username,[this,weak](std::string value){auto live=weak.lock();if(live&&live->load())link(std::move(value));});
    else if(selected_==3)host_->emplace_overlay<text_entry_overlay>("API key environment variable",key_env_,[this,weak](std::string value){auto live=weak.lock();if(!live||!live->load())return;if(!trophy_detail::env_name(value)){message_="Enter an environment variable name, not the API key.";return;}if(save([&](auto& p){p["achievements.ra.key_env"]=value;})){key_env_=value;message_="Environment variable name saved.";}});
    else host_->emplace_overlay<text_entry_overlay>("RetroAchievements game ID","",[this,weak](std::string value){auto live=weak.lock();if(!live||!live->load())return;try{const auto ids=trophy_detail::game_ids(value);if(ids.size()!=1)throw std::runtime_error("Enter one numeric game ID.");auto updated=ra_games_;if(std::ranges::find(updated,ids.front())==updated.end())updated.push_back(ids.front());if(updated.size()>64)throw std::runtime_error("At most 64 game IDs are supported.");std::string serial;for(auto id:updated){if(!serial.empty())serial+=',';serial+=std::to_string(id);}if(save([&](auto& p){p["achievements.ra.games"]=serial;})){ra_games_=std::move(updated);std::erase_if(records_,[](const auto& r){return r.remote;});append_remote();message_="Game ID added.";}}catch(const std::exception& e){message_=e.what();}});
  }
  void refresh(){if(busy_){message_="Please wait for the current request.";return;}textures_.forget_failed();if(page_==Page::settings){begin_profiles();return;}const auto index=page_==Page::trophies?selected_game_:selected_;if(index<records_.size()&&records_[index].remote){unsigned id{};const auto& text=records_[index].game.game_id;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),id);if(parsed.ec==std::errc{})load_remote(id,true);}else if(!profile_.user.empty())load_sets();else begin_profiles();}
  // Information carries what normal browsing labels leave out: provider
  // semantics, per-grade totals, raw folders and provider warnings.
  void show_information(){
    if(page_==Page::settings){show_setting_information();return;}
    const auto index=page_==Page::trophies?selected_game_:selected_;
    if(page_==Page::games&&index==records_.size()){host_->emplace_overlay<message_overlay>("Settings",sources_information(),std::vector<std::string>{"OK"});return;}
    if(index>=records_.size())return;const auto& record=records_[index];std::string title=record.game.title,text;
    if(page_==Page::trophies&&selected_<record.game.entries.size()){const auto& entry=record.game.entries[selected_];
      if(trophy_detail::concealed(entry)){title="Hidden trophy";text="Its details appear once it is earned.\n"+trophy_detail::state(entry);}
      else {title=entry.title;text=entry.description+"\n"+trophy_detail::state(entry);const auto g=trophy_detail::grade(entry.grade);if(!g.empty())text+="\nGrade: "+g;if(entry.points)text+="\nPoints: "+std::to_string(*entry.points);
        if(record.remote){const auto found=std::ranges::find_if(record.details,[&](const auto& d){return d.id==entry.id;});const auto when=[&](std::optional<std::int64_t> at){return !entry.unlock_state_known?std::string("Unavailable"):at?trophy_detail::date(at):std::string("Not earned");};
          text+="\nSoftcore: "+when(found==record.details.end()?std::nullopt:found->softcore_at);text+="\nHardcore: "+when(found==record.details.end()?std::nullopt:found->hardcore_at);}
        else if(trophy_detail::earned(entry))text+="\nUnlocked: "+trophy_detail::date(entry.unlocked_at);}}
    else text=record_information(record,index);
    host_->emplace_overlay<message_overlay>(title,text,std::vector<std::string>{"OK"});}
  std::string record_information(const Record& record,std::size_t index)const{
    const auto& game=record.game;std::string text;
    if(record.remote)text="Provider: RetroAchievements\nRetroAchievements awards points; it has no PlayStation trophy grades.\nAccount: "+(ra_.username.empty()?game.user_id:ra_.username)+"\nGame ID: "+game.game_id;
    else text="Provider: RPCS3 (local emulator data, not synchronised with PlayStation Network)\nRPCS3 user: "+game.user_id+"\nTrophy set: "+game.game_id;
    const Tally* t=index<tallies_.size()?&tallies_[index]:nullptr;
    if(record.remote&&!record.loaded)text+="\nProgress is not loaded. Refresh downloads it with your web API key.";
    else if(t){
      text+="\nEarned: "+std::to_string(t->unlocked)+" of "+std::to_string(t->entries);if(const auto p=trophy_detail::percent(*t))text+=" ("+std::to_string(*p)+"%)";
      if(t->known!=t->entries)text+="\nUnlock state unavailable for "+std::to_string(t->entries-t->known)+" of "+std::to_string(t->entries);
      if(record.remote)text+="\nHardcore: "+std::to_string(t->hardcore)+"\nPoints: "+std::to_string(t->points)+" of "+std::to_string(t->points_total);
      else {text+="\n";for(std::size_t s=0;s<trophy_detail::grade_order.size();++s)text+=std::string(s?" · ":"")+trophy_detail::grade(trophy_detail::grade_order[s])+" "+std::to_string(t->earned[s])+"/"+std::to_string(t->total[s]);}
    }
    if(!record.remote&&!game.source_directory.empty())text+="\nData folder: "+trophy_detail::path_text(game.source_directory);
    for(const auto& warning:game.warnings)text+="\n"+warning;
    return text;
  }
  std::string sources_information()const{
    std::string text="RPCS3: "+(profile_.user.empty()?std::string("no user selected"):"user "+profile_.user);
    if(!profile_.root.empty())text+="\nRPCS3 data root: "+trophy_detail::path_text(profile_.root);
    text+="\nRetroAchievements: "+(ra_.username.empty()?std::string("not linked"):ra_.username)+"\nRetroAchievements game IDs: "+std::to_string(ra_games_.size());
    return text;
  }
  void show_setting_information(){
    if(selected_>=trophy_view::settings_rows)return;
    std::string text;
    switch(selected_){
    case 0:text=saved_root_.empty()?"Automatic discovery uses the RPCS3 game folders known to OpenXMB.":"Folder: "+saved_root_;if(!profile_.root.empty())text+="\nIn use: "+trophy_detail::path_text(profile_.root);text+="\nRPCS3 users found: "+std::to_string(profiles_.size());break;
    case 1:text=profile_.user.empty()?"No RPCS3 user is selected.":"User: "+profile_.user+"\nData root: "+trophy_detail::path_text(profile_.root);text+="\nTrophies are local RPCS3 records for this user only; they are not synchronised with PlayStation Network.";break;
    case 2:text=ra_.username.empty()?"No account is linked.":"Account: "+ra_.username+(ra_.ulid.empty()?std::string():"\nULID: "+ra_.ulid);text+="\nRetroAchievements awards points, not PlayStation trophy grades.";break;
    case 3:text="Variable: "+key_env_+"\nYour web API key is read from this variable only when OpenXMB contacts RetroAchievements. The key is never saved.";break;
    default:{std::string ids;for(const auto id:ra_games_){if(!ids.empty())ids+=", ";ids+=std::to_string(id);}text=ids.empty()?"No game IDs added.":"Game IDs: "+ids;break;}
    }
    host_->emplace_overlay<message_overlay>(setting_row(selected_).label,text,std::vector<std::string>{"OK"});
  }
  SettingRow setting_row(std::size_t i)const{
    switch(i){
    case 0:return {"RPCS3 data root",saved_root_.empty()?"Automatic":trophy_detail::short_path(saved_root_),"The RPCS3 dev_hdd0 folder. Information shows the full path."};
    case 1:return {"RPCS3 user",profile_.user.empty()?"Not selected":profile_.user,"Trophies are read from one RPCS3 user at a time."};
    case 2:return {"Link RetroAchievements",ra_.username.empty()?"Not linked":ra_.username,"Checks a RetroAchievements account with your web API key."};
    case 3:return {"API key environment variable",key_env_,"Name of the variable that holds your web API key. The key is never saved."};
    default:return {"Add RetroAchievements game ID",ra_games_.empty()?std::string("None"):std::to_string(ra_games_.size())+(ra_games_.size()==1?" game":" games"),"Adds a game by its numeric RetroAchievements ID."};
    }
  }
  void retally(){tallies_.clear();tallies_.reserve(records_.size());aggregate_={};rpcs3_sets_=0;for(const auto& r:records_){tallies_.push_back(trophy_detail::tally(r.game));if(!r.remote){trophy_detail::add(aggregate_,tallies_.back());++rpcs3_sets_;}}tallies_dirty_=false;}
  // Focused row first, then outwards; hidden and unearned trophies keep their
  // images unloaded.
  void update_icons(){
    textures_.poll();textures_.next_frame();
    constexpr std::array<std::ptrdiff_t,9> order{0,-1,1,-2,2,-3,3,4,5};
    const auto at=[&](std::ptrdiff_t d,std::size_t count)->std::optional<std::size_t>{const auto i=static_cast<std::ptrdiff_t>(selected_)+d;if(i<0||static_cast<std::size_t>(i)>=count)return std::nullopt;return static_cast<std::size_t>(i);};
    if(page_==Page::games){for(const auto d:order)if(const auto i=at(d,records_.size()))textures_.request(loader_,records_[*i].game.icon);}
    else if(page_==Page::trophies&&selected_game_<records_.size()){const auto& game=records_[selected_game_].game;textures_.request(loader_,game.icon);
      for(const auto d:order)if(const auto i=at(d,game.entries.size());i&&trophy_detail::earned(game.entries[*i]))textures_.request(loader_,game.entries[*i].icon);}
    textures_.trim(loader_,host_?host_->get_window():nullptr);
  }
  void moved(std::size_t from){focus_from_=from;focus_at_=Clock::now();}
  void paged(int direction){page_dir_=direction;page_at_=Clock::now();focus_from_=selected_;focus_at_={};}
  float progress(Clock::time_point since,float seconds)const{if(config::CONFIG.enabled("power.reduced.motion",false))return 1.0F;return trophy_view::ease(std::chrono::duration<float>(Clock::now()-since).count()/seconds);}
  float page_amount()const{return progress(page_at_,trophy_view::page_seconds);}
  float focus_progress()const{return progress(focus_at_,trophy_view::focus_seconds);}
  // Rows sit 175 px either side of the focus and 80 px apart (trophy004);
  // settings keep the XMB rule, where rows above jump over the focus gap.
  float slot(std::size_t i,std::size_t focus)const{
    namespace v=trophy_view;
    if(page_==Page::settings){if(i==focus)return v::focus_y;if(i<focus)return v::settings_above_y-static_cast<float>(focus-i-1)*v::pitch;return v::focus_y+v::settings_next_padding+static_cast<float>(i-focus)*v::pitch;}
    if(i==focus)return v::focus_y;if(i<focus)return v::focus_y-v::focus_gap-static_cast<float>(focus-i-1)*v::pitch;return v::focus_y+v::focus_gap+static_cast<float>(i-focus-1)*v::pitch;
  }
  bool animating(std::size_t count)const{return focus_from_<count&&(focus_from_+1==selected_||selected_+1==focus_from_);}
  float row_y(std::size_t i,std::size_t count,float e)const{if(e>=1.0F||!animating(count))return slot(i,selected_);return std::lerp(slot(i,focus_from_),slot(i,selected_),e);}
  float focus_amount(std::size_t i,std::size_t count,float e)const{const float now=i==selected_?1.0F:0.0F;if(e>=1.0F||!animating(count))return now;return std::lerp(i==focus_from_?1.0F:0.0F,now,e);}
  void game_icon(const xmb_canvas& c,const std::filesystem::path& path,float cx,float cy,float h,float alpha)const{
    if(const auto* t=textures_.find(path)){const float tw=static_cast<float>(t->width),th=static_cast<float>(t->height);const float s=std::min(h*trophy_view::icon0_aspect/tw,h/th);c.image(*t,cx-tw*s*0.5F,cy-th*s*0.5F,tw*s,th*s,{1,1,1,alpha});return;}
    icons_.draw(c,"theme/icon_game",cx,cy,h,alpha);
  }
  void draw_parent(const xmb_canvas& c,float dx,float alpha)const{
    namespace v=trophy_view;
    if(page_==Page::trophies&&selected_game_<records_.size()){game_icon(c,records_[selected_game_].game.icon,v::parent_x+dx,v::focus_y,v::game_icon_focus_h,alpha);v::back_arrow(c,v::trophies_arrow_x+dx,v::focus_y,alpha);return;}
    icons_.draw(c,page_==Page::settings?"theme/icon_setting":"theme/icon_trophy",v::parent_x+dx,v::focus_y,v::parent_icon_extent,0.9F*alpha);
    v::back_arrow(c,v::games_arrow_x+dx,v::focus_y,alpha);
  }
  std::pair<std::string,std::string> games_notice()const{
    if(busy_)return {"Loading...",""};
    if(profile_.user.empty())return {"No RPCS3 user selected","Choose an RPCS3 data root or user in Settings."};
    return {"No trophy sets found","RPCS3 user "+profile_.user+" has no trophy data yet."};
  }
  std::pair<std::string,std::string> trophies_notice(const Record& record)const{
    if(busy_)return {"Loading...",""};
    if(record.remote&&!record.loaded)return {"Progress not loaded","Refresh downloads it from RetroAchievements with your web API key."};
    return {"No trophies in this set",""};
  }
  void draw_games(const xmb_canvas& c,float dx,float alpha)const{
    namespace v=trophy_view;
    const auto count=row_count();const float e=focus_progress();
    if(records_.empty()){const auto [title,hint]=games_notice();v::notice(c,v::game_label_x+dx,v::focus_y-v::focus_gap,title,hint,alpha);}
    for(std::size_t i=0;i<count;++i){
      const float y=row_y(i,count,e),f=focus_amount(i,count,e);
      const float a=alpha*v::list_fade(y)*std::lerp(v::idle_alpha,1.0F,f);
      if(a<=0.004F)continue;
      const float h=std::lerp(v::game_icon_h,v::game_icon_focus_h,f),size=std::lerp(v::row_title_size,v::focus_title_size,f),glow=f*xmb_canvas::pulse();
      if(i==records_.size()){
        icons_.draw(c,"theme/icon_setting",v::game_icon_x+dx,y,h,a);
        c.text("Settings",v::game_label_x+dx,y+v::title_drop,size,{1,1,1,a},glow);
        if(f>0.0F)c.text("Choose trophy sources and account",v::game_label_x+dx,y+v::tag_drop+6.0F,v::detail_size,{1,1,1,0.8F*a*f});
        continue;}
      const auto& record=records_[i];const Tally* t=i<tallies_.size()?&tallies_[i]:nullptr;
      game_icon(c,record.game.icon,v::game_icon_x+dx,y,h,a);
      c.text(v::fit(c,record.game.title,size,v::percent_left-v::game_label_x),v::game_label_x+dx,y+v::title_drop,size,{1,1,1,a},glow);
      const float tag_y=y+std::lerp(v::row_tag_drop,v::tag_drop,f);float tx=v::game_label_x+dx;
      tx+=v::tag(c,tx,tag_y,record.remote?"RetroAchievements":"RPCS3",a)+v::tag_gap;
      if(record.remote&&!record.loaded)v::tag(c,tx,tag_y,"Not loaded",a);
      else if(t&&t->known!=t->entries)v::tag(c,tx,tag_y,"Unlock state unavailable",a);
      if(!t)continue;const auto p=trophy_detail::percent(*t);if(!p)continue;
      const auto label=std::to_string(*p)+"%";
      c.text(label,v::percent_x+dx,y+v::percent_drop,v::percent_size,{1,1,1,a},0,true);
      if(t->earned[0]>0){const float w=v::text_width(c,label,v::percent_size);v::cup(c,v::percent_x+dx-w*0.5F-v::percent_glyph_gap,y-v::percent_glyph_h*0.6F,v::percent_glyph_h,trophy_detail::grade_colour(openxmb::desktop::TrophyGrade::platinum,a));}
      if(f>0.0F)v::bar(c,v::bar_x+dx,y+v::bar_drop,v::bar_w,v::bar_h,static_cast<float>(*p)/100.0F,a*f);
    }
  }
  std::string unlock_line(const Record& record,const openxmb::desktop::Achievement& entry)const{
    if(!entry.unlock_state_known)return "Unlock state unavailable";
    if(!entry.unlocked)return "Not yet earned";
    if(!record.remote)return entry.unlocked_at?trophy_detail::date(entry.unlocked_at):std::string("Earned · date unavailable");
    const auto found=std::ranges::find_if(record.details,[&](const auto& d){return d.id==entry.id;});std::string text;
    if(found!=record.details.end()){if(found->hardcore_at)text="Hardcore "+trophy_detail::date(found->hardcore_at);if(found->softcore_at)text+=(text.empty()?std::string():std::string(" · "))+"Softcore "+trophy_detail::date(found->softcore_at);}
    return text.empty()?std::string("Earned · date unavailable"):text;
  }
  void draw_trophies(const xmb_canvas& c,float dx,float alpha)const{
    namespace v=trophy_view;
    if(selected_game_>=records_.size())return;
    const auto& record=records_[selected_game_];const auto& entries=record.game.entries;
    if(entries.empty()){const auto [title,hint]=trophies_notice(record);v::notice(c,v::trophy_label_x+dx,v::focus_y,title,hint,alpha);return;}
    const float e=focus_progress(),right=record.remote?v::percent_left:v::text_right;const auto count=entries.size();
    for(std::size_t i=0;i<count;++i){
      const float y=row_y(i,count,e),f=focus_amount(i,count,e);
      const float a=alpha*v::list_fade(y)*std::lerp(v::idle_alpha,1.0F,f);
      if(a<=0.004F)continue;
      const auto& entry=entries[i];const bool hidden=trophy_detail::concealed(entry),earned=trophy_detail::earned(entry);
      const float s=std::lerp(v::trophy_icon,v::trophy_icon_focus,f);
      if(const auto* t=earned?textures_.find(entry.icon):nullptr)c.image(*t,v::trophy_icon_x+dx-s*0.5F,y-s*0.5F,s,s,{1,1,1,a});
      else v::placeholder(c,v::trophy_icon_x+dx,y,s,earned,a);
      const float size=std::lerp(v::row_title_size,v::focus_title_size,f),base=y+std::lerp(v::title_drop,v::detail_title_drop,f);
      if(entry.grade!=openxmb::desktop::TrophyGrade::none){const float g=size*v::glyph_scale;v::cup(c,v::glyph_x+dx,base-g*0.82F,g,trophy_detail::grade_colour(hidden?openxmb::desktop::TrophyGrade::none:entry.grade,a));}
      c.text(hidden?std::string("???"):v::fit(c,entry.title,size,right-v::trophy_label_x),v::trophy_label_x+dx,base,size,{1,1,1,a},f*xmb_canvas::pulse());
      if(entry.points)c.text(std::to_string(*entry.points)+(*entry.points==1?" point":" points"),v::percent_x+dx,y+v::percent_drop,v::points_size,{1,1,1,0.85F*a},0,true);
      if(f<=0.0F)continue;
      const glm::vec4 detail{1,1,1,0.9F*a*f};float line=base+v::detail_pitch;
      for(const auto& text:v::wrap(c,hidden?std::string_view("Hidden trophy"):std::string_view(entry.description),v::detail_size,right-v::trophy_label_x,2)){c.text(text,v::trophy_label_x+dx,line,v::detail_size,detail);line+=v::detail_line;}
      c.text(unlock_line(record,entry),v::trophy_label_x+dx,line,v::detail_size,detail);
    }
  }
  void draw_settings(const xmb_canvas& c,float dx,float alpha)const{
    namespace v=trophy_view;const float e=focus_progress();
    for(std::size_t i=0;i<v::settings_rows;++i){
      const float y=row_y(i,v::settings_rows,e),f=focus_amount(i,v::settings_rows,e);
      const float a=alpha*v::bottom_only_fade(y)*std::lerp(v::settings_idle_alpha,1.0F,f);
      if(a<=0.004F)continue;
      const auto row=setting_row(i);
      icons_.draw(c,"theme/icon_setting_item",v::settings_icon_x+dx,y,std::lerp(v::settings_icon,v::settings_icon_focus,f),a);
      const auto value=v::fit(c,row.value,v::settings_value_size,v::settings_value_width);const float value_w=v::text_width(c,value,v::settings_value_size);
      const float size=std::lerp(v::settings_label_size,v::settings_label_focus,f),base=y+std::lerp(11.0F,3.0F,f);
      c.text(v::fit(c,row.label,size,v::settings_value_right-value_w-48.0F-v::settings_label_x),v::settings_label_x+dx,base,size,{1,1,1,a},f*xmb_canvas::pulse());
      c.text(value,v::settings_value_right+dx-value_w,base,v::settings_value_size,{1,1,1,0.9F*a});
      if(f>0.0F)c.text(v::fit(c,row.description,v::settings_description_size,v::settings_value_right-v::settings_label_x),v::settings_label_x+dx,y+32.0F,v::settings_description_size,{1,1,1,0.82F*a*f});
    }
  }
  // Summary of the focused provider: RPCS3 grade counts (all local sets on the
  // game list, one set on its trophies) or RetroAchievements counts and points.
  void draw_panel(const xmb_canvas& c)const{
    namespace v=trophy_view;
    const Record* record=nullptr;
    if(page_==Page::trophies&&selected_game_<records_.size())record=&records_[selected_game_];
    else if(page_==Page::games&&selected_<records_.size()&&records_[selected_].remote)record=&records_[selected_];
    const Tally* t=nullptr;bool graded=true,loaded=true;std::string header="Trophies",provider="RPCS3",context;
    if(record){const auto index=static_cast<std::size_t>(record-records_.data());t=index<tallies_.size()?&tallies_[index]:nullptr;graded=!record->remote;loaded=!record->remote||record->loaded;
      if(record->remote){header="Achievements";provider="RetroAchievements";}
      context=page_==Page::trophies?record->game.title:(ra_.username.empty()?std::string("No account linked"):"Account "+ra_.username);}
    else {if(profile_.user.empty()&&rpcs3_sets_==0)return;t=&aggregate_;context=profile_.user.empty()?std::string("No RPCS3 user selected"):"User "+profile_.user+" · Local data";}
    const float x=v::panel_x,y=v::panel_y,w=v::panel_w,h=v::panel_h,k=v::panel_header/v::panel_h,body=y+v::panel_header;
    const auto grey=[](float s,float a){return dialog_chrome::linear(glm::vec4{s,s,s,a});};
    const std::array<std::pair<float,glm::vec4>,5> stops{{{0.0F,grey(0.80F,0.95F)},{0.5F*k,grey(0.62F,0.95F)},{k,grey(0.45F,0.95F)},{k,grey(0.16F,0.90F)},{1.0F,grey(0.12F,0.90F)}}};
    c.gradient_shape(x,y,w,h,stops,v::panel_radius);
    dialog_chrome::edge_glow(c,x,y,w,h,0.30F,0.0F,1.2F,1.0F,1.0F,v::panel_radius);
    c.text(header,x+12.0F,y+22.0F,20,{1,1,1,1});
    const float pw=v::text_width(c,provider,16);c.text(provider,x+w-12.0F-pw,y+21.0F,16,{1,1,1,0.9F});
    if(t&&graded){for(std::size_t s=0;s<trophy_detail::grade_order.size();++s){const float sx=x+12.0F+static_cast<float>(s)*v::panel_slot;v::cup(c,sx+9.0F,body+16.0F,22.0F,trophy_detail::grade_colour(trophy_detail::grade_order[s],1));const auto count=std::to_string(t->earned[s]);c.text(count,sx+24.0F,body+36.0F,count.size()>3?19.0F:24.0F,{1,1,1,1});}}
    else if(t&&loaded){const auto earned=std::to_string(t->unlocked)+" / "+std::to_string(t->entries);c.text(earned,x+12.0F,body+36.0F,24,{1,1,1,1});c.text("earned",x+18.0F+v::text_width(c,earned,24),body+36.0F,16,{1,1,1,0.8F});
      if(t->hardcore){const auto hc="Hardcore "+std::to_string(t->hardcore);c.text(hc,x+w-12.0F-v::text_width(c,hc,16),body+36.0F,16,{1,1,1,0.85F});}}
    else if(!loaded)c.text("Progress not loaded",x+12.0F,body+36.0F,20,{1,1,1,0.85F});
    if(t&&loaded){
      if(const auto p=trophy_detail::percent(*t)){const auto label=std::to_string(*p)+"%";const float lw=v::text_width(c,label,20);v::bar(c,x+12.0F,body+52.0F,w-36.0F-lw,8.0F,static_cast<float>(*p)/100.0F,1);c.text(label,x+w-12.0F-lw,body+61.0F,20,{1,1,1,1});}
      else if(t->entries)c.text("Unlock state unavailable",x+12.0F,body+61.0F,17,{1,1,1,0.8F});
    }
    std::string line=context;if(t&&!graded&&loaded&&t->points_total)line="Points "+std::to_string(t->points)+" / "+std::to_string(t->points_total)+" · "+context;
    c.text(v::fit(c,line,16,w-24.0F),x+12.0F,body+92.0F,16,{1,1,1,0.78F});
  }
  auroreengine::resource_loader& loader_;shell* host_{};std::shared_ptr<std::atomic_bool> alive_,cancel_;std::future<WorkResult> work_;bool busy_{},closing_{},origin_valid_{true};
  std::filesystem::path origin_root_,origin_config_,cache_root_;std::string origin_profile_,saved_root_,saved_user_,key_env_,message_,reload_game_id_;std::vector<unsigned> ra_games_;openxmb::desktop::RetroAccount ra_;Profile profile_;std::vector<Profile> profiles_;std::vector<Record> records_;Page page_{Page::games};std::size_t selected_{},selected_game_{};Clock::time_point retry_after_{},last_origin_check_{};
  std::vector<Tally> tallies_;Tally aggregate_;std::size_t rpcs3_sets_{};bool tallies_dirty_{true};
  std::size_t focus_from_{};Clock::time_point focus_at_{},page_at_{};int page_dir_{1};
  media_icons icons_;trophy_detail::icon_cache textures_;
};
} // namespace app
