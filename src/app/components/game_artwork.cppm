module;
#include "openxmb/desktop/async_job.hpp"
#include "openxmb/desktop/library.hpp"
#include "openxmb/desktop/steamgriddb.hpp"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#undef main
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module openxmb.app:game_artwork;
import :component;
import :main;
import :xmb_presentation;
import :setup_controls;
import :dialog_chrome;
import :message_overlay;
import :text_entry;
import auroreengine;
import glm;
import vulkan_hpp;
import vma;
import openxmb.config;
import openxmb.utils;

namespace app {
namespace artwork_detail {
namespace fs=std::filesystem;
namespace desktop=openxmb::desktop;
using Role=desktop::SteamGridDBRole;
using Clock=std::chrono::steady_clock;
constexpr std::array roles{Role::icon,Role::horizontal_grid,Role::logo,Role::hero};
std::string path_text(const fs::path& path) {const auto value=path.u8string();return {value.begin(),value.end()};}
fs::path text_path(const std::string& text) {return fs::path(std::u8string(text.begin(),text.end()));}
std::string elide(std::string text,std::size_t length) {
  if(text.size()<=length)return text;
  while(length&&(static_cast<unsigned char>(text[length])&0xc0)==0x80)--length;
  return text.substr(0,length)+"...";
}
const char* role_name(Role role) {
  switch(role) {case Role::icon:return "Icon";case Role::horizontal_grid:return "Horizontal grid";case Role::logo:return "Logo";case Role::hero:return "Hero / background";}
  return "Artwork";
}
const char* role_key(Role role) {
  switch(role) {case Role::icon:return "icon";case Role::horizontal_grid:return "cover";case Role::logo:return "logo";case Role::hero:return "background";}
  return "icon";
}
fs::path& selected_path(desktop::Game& game,Role role) {
  switch(role) {case Role::icon:return game.icon;case Role::horizontal_grid:return game.cover;case Role::logo:return game.logo;case Role::hero:return game.background;}
  return game.icon;
}
fs::path selected_path(const desktop::Game& game,Role role) {
  switch(role) {case Role::icon:return game.icon;case Role::horizontal_grid:return game.cover;case Role::logo:return game.logo;case Role::hero:return game.background;}
  return game.icon;
}
bool existing(const fs::path& path) {std::error_code error;return !path.empty()&&fs::is_regular_file(path,error)&&!error;}
bool same_path(const fs::path& a,const fs::path& b) {
  std::error_code error;
  if(!a.empty()&&!b.empty()&&fs::equivalent(a,b,error)&&!error)return true;
  return a.lexically_normal()==b.lexically_normal();
}
fs::path native_path(const desktop::Game& game,Role role) {
  if(game.platform!="PlayStation 3 (RPCS3)"||game.game_directory.empty())return {};
  if(role==Role::icon&&existing(game.game_directory/"ICON0.PNG"))return game.game_directory/"ICON0.PNG";
  if(role==Role::hero)for(const auto* name:{"PIC1.PNG","PIC0.PNG"})if(existing(game.game_directory/name))return game.game_directory/name;
  return {};
}
bool env_name(std::string_view value) {
  const auto letter=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';};
  return !value.empty()&&value.size()<=128&&letter(value.front())&&std::all_of(value.begin(),value.end(),[&](char c){return letter(c)||(c>='0'&&c<='9');});
}
struct Transfer {std::mutex mutex;std::uint64_t done{},total{};};
desktop::HttpTransport transport(std::stop_token stop,std::shared_ptr<Transfer> transfer) {
  return [stop,transfer](desktop::HttpRequest request) {
    if(stop.stop_requested())return desktop::HttpResponse{false,0,{},"Canceled"};
    request.progress=[stop,transfer,previous=request.progress](std::uint64_t done,std::uint64_t total) {
      {std::lock_guard lock(transfer->mutex);transfer->done=done;transfer->total=total;}
      return !stop.stop_requested()&&(!previous||previous(done,total));
    };
    return desktop::http_get(request);
  };
}
enum class WorkKind {search,assets,download};
struct Work {
  WorkKind kind{};std::uint64_t generation{},game_id{};Role role{};unsigned page{};
  desktop::SteamGridDBSearchResult search;
  desktop::SteamGridDBAssetsResult assets;
  desktop::SteamGridDBDownloadResult download;
};
struct Preview {
  fs::path path;Role role{};std::string source;bool assignable{},native{};
  unsigned width{},height{};std::uintmax_t file_size{};fs::file_time_type modified{};
};
struct Image {Preview preview;std::vector<std::uint8_t> rgba;int width{},height{};std::uint64_t generation{};};
Image decode(Preview preview,std::uint64_t generation,std::stop_token stop) {
  if(stop.stop_requested())throw std::runtime_error("Canceled");
  std::error_code error;
  if(!existing(preview.path))throw std::runtime_error("Choose an existing image file.");
  preview.file_size=fs::file_size(preview.path,error);
  if(error||preview.file_size==0||preview.file_size>32ULL*1024*1024)throw std::runtime_error("Artwork must be an image of at most 32 MB.");
  preview.modified=fs::last_write_time(preview.path,error);
  if(error)throw std::runtime_error("Could not read the artwork file.");
  std::ifstream input(preview.path,std::ios::binary);
  std::vector<char> encoded(static_cast<std::size_t>(preview.file_size));
  if(!input.read(encoded.data(),static_cast<std::streamsize>(encoded.size())))throw std::runtime_error("Could not read the artwork file.");
  if(stop.stop_requested())throw std::runtime_error("Canceled");
  using Surface=std::unique_ptr<SDL_Surface,decltype(&SDL_FreeSurface)>;
  auto* stream=SDL_RWFromConstMem(encoded.data(),static_cast<int>(encoded.size()));
  if(!stream)throw std::runtime_error("Could not open the image decoder.");
  Surface surface{IMG_Load_RW(stream,1),SDL_FreeSurface};
  if(!surface)throw std::runtime_error("The file could not be decoded as an image.");
  if(surface->w<=0||surface->h<=0||static_cast<std::uint64_t>(surface->w)*surface->h>32ULL*1024*1024)throw std::runtime_error("Artwork exceeds the 32 megapixel decoding limit.");
  preview.width=static_cast<unsigned>(surface->w);preview.height=static_cast<unsigned>(surface->h);
  // Only the disposable preview is resized. The selected file and displayed
  // source dimensions remain unchanged, including wide hero/logo artwork.
  const double scale=std::min({1.0,1920.0/surface->w,1080.0/surface->h});
  const int width=std::max(1,static_cast<int>(std::lround(surface->w*scale)));
  const int height=std::max(1,static_cast<int>(std::lround(surface->h*scale)));
  Surface rgba{SDL_ConvertSurfaceFormat(surface.get(),SDL_PIXELFORMAT_RGBA32,0),SDL_FreeSurface};
  if(!rgba)throw std::runtime_error("Could not convert the artwork image.");
  if(width!=rgba->w||height!=rgba->h) {
    Surface reduced{SDL_CreateRGBSurfaceWithFormat(0,width,height,32,SDL_PIXELFORMAT_RGBA32),SDL_FreeSurface};
    if(!reduced||SDL_BlitScaled(rgba.get(),nullptr,reduced.get(),nullptr)!=0)throw std::runtime_error("Could not prepare the artwork preview.");
    rgba=std::move(reduced);
  }
  if(stop.stop_requested())throw std::runtime_error("Canceled");
  Image result;result.preview=std::move(preview);result.width=width;result.height=height;result.generation=generation;
  result.rgba.resize(static_cast<std::size_t>(width)*height*4);
  if(SDL_LockSurface(rgba.get())!=0)throw std::runtime_error("Could not read decoded artwork pixels.");
  for(int y=0;y<height;++y)std::memcpy(result.rgba.data()+static_cast<std::size_t>(y)*width*4,static_cast<const std::uint8_t*>(rgba->pixels)+static_cast<std::size_t>(y)*rgba->pitch,static_cast<std::size_t>(width)*4);
  SDL_UnlockSurface(rgba.get());
  if(stop.stop_requested())throw std::runtime_error("Canceled");
  return result;
}
}

export class game_artwork final : public component,public action_receiver {
  using Role=artwork_detail::Role;
  using Clock=artwork_detail::Clock;
  using Preview=artwork_detail::Preview;
  using Image=artwork_detail::Image;
  enum class Page {home,role,games,assets,preview};
public:
  game_artwork(auroreengine::resource_loader& loader,openxmb::desktop::Game game):loader_(loader),game_(std::move(game)) {
    try {
      openxmb::desktop::Library library;
      origin_root_=library.root();origin_profile_=library.active_profile().id;origin_config_=config::CONFIG.profileConfigPath;
      cache_root_=origin_root_/"cache"/"artwork"/origin_profile_;
      key_env_=config::CONFIG.preference("artwork.steamgriddb.key_env","STEAMGRIDDB_API_KEY");
      search_text_=game_.title;
      origin_valid_=same_origin();
      message_=origin_valid_?"Choose a role. SteamGridDB is queried only when you request it.":"The active profile could not be verified. Reopen Artwork.";
    }catch(const std::exception& error){origin_valid_=false;message_=error.what();}
  }
  ~game_artwork() override {
    alive_->store(false);work_.cancel();decode_.cancel();
    // Network and decoder futures never join. An already submitted local GPU
    // upload must finish before its destination is destroyed; never transfer
    // Vulkan objects to detached cleanup that could outlive the engine.
    if(upload_.valid())upload_.wait();
    if(texture_)try{wait_gpu();}catch(...){}
  }
  [[nodiscard]] bool is_opaque() const override{return true;}
  [[nodiscard]] bool do_fade_in() const override{return true;}
  [[nodiscard]] bool do_fade_out() const override{return true;}
  result tick(shell* host) override {
    host_=host;
    if(origin_valid_&&Clock::now()-last_origin_check_>=std::chrono::seconds(1)) {
      last_origin_check_=Clock::now();if(!same_origin())invalidate_origin();
    }
    if(work_.ready()) {
      try {auto value=work_.get();if(value.generation==work_generation_&&guard()){work_busy_=false;finish(std::move(value));}}
      catch(const std::exception& error){if(work_busy_){work_busy_=false;message_=error.what();}}
    }
    if(upload_.valid()&&upload_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
      try {
        upload_.get();
        if(upload_generation_==decode_generation_&&guard()&&incoming_&&incoming_->loaded.load(std::memory_order_acquire)) {
          if(texture_)wait_gpu();texture_=std::move(incoming_);preview_=upload_preview_;preview_ready_=true;
          message_="Preview ready. "+preview_.source+" · "+dimensions(preview_.width,preview_.height);
        }else incoming_.reset();
      }catch(...){incoming_.reset();if(upload_generation_==decode_generation_)message_="The artwork preview could not be uploaded.";}
    }
    if(decode_.ready()) {
      try {auto image=decode_.get();if(image.generation==decode_generation_&&guard()){decode_busy_=false;decoded_=std::move(image);}}
      catch(const std::exception& error){if(decode_busy_){decode_busy_=false;message_=error.what();}}
    }
    if(decoded_&&!upload_.valid()&&origin_valid_) {
      auto image=std::make_shared<Image>(std::move(*decoded_));decoded_.reset();
      if(image->generation==decode_generation_)try {
        incoming_=std::make_shared<auroreengine::texture>(loader_.getDevice(),loader_.getAllocator(),image->width,image->height);
        upload_generation_=image->generation;upload_preview_=image->preview;
        upload_=loader_.loadTexture(incoming_.get(),auroreengine::LoaderFunction{[image](std::uint8_t* target,std::size_t capacity) {
          if(image->rgba.size()>capacity)throw std::runtime_error("Artwork preview exceeds the texture upload limit.");
          std::memcpy(target,image->rgba.data(),image->rgba.size());
        }});
      }catch(...){incoming_.reset();message_="The artwork preview could not be created.";}
    }
    return closing_?result::close|result::back_sound:result::success;
  }
  result on_action(action input) override {
    if(input==action::cancel) {
      cancel_jobs();
      if(page_==Page::home)closing_=true;
      else if(page_==Page::preview) {page_=preview_return_;selected_=preview_return_selection_;}
      else if(page_==Page::assets){page_=Page::role;selected_=3;}
      else {page_=Page::home;selected_=role_index();}
      return result::success|result::back_sound;
    }
    const auto count=row_count();
    if(input==action::up&&count){selected_=(selected_+count-1)%count;return result::success;}
    if(input==action::down&&count){selected_=(selected_+1)%count;return result::success;}
    if(!host_||!guard())return result::success;
    if(input==action::options){show_information();return result::success;}
    if((input==action::left||input==action::right)&&page_==Page::assets&&!busy()) {
      if(input==action::left&&asset_page_>0)load_assets(asset_page_-1);
      if(input==action::right&&has_more_)load_assets(asset_page_+1);
      return result::success;
    }
    if(input==action::ok){if(busy())message_="Please wait, or press Back to cancel.";else activate();return result::success|result::ok_sound;}
    return result::unsupported;
  }
  void render(auroreengine::gui_renderer& renderer,shell* host) override {
    const xmb_canvas c(renderer);c.rect(0,0,1920,1080,{0,0,0,0.64F});dialog_chrome::divider_frame(c,0.9F);
    c.text("Artwork",180,171,35,{1,1,1,0.95F});
    c.text(artwork_detail::elide(game_.title,86),180,249,26,{1,1,1,0.86F});
    std::string subtitle=page_==Page::home?"Choose artwork to change":artwork_detail::role_name(role_);
    if(page_==Page::games)subtitle="SteamGridDB search · select a game explicitly";
    if(page_==Page::assets)subtitle="SteamGridDB · "+std::string(artwork_detail::role_name(role_))+" · page "+std::to_string(asset_page_+1);
    c.text(artwork_detail::elide(subtitle,90),180,287,20,{1,1,1,0.63F});
    const auto count=row_count();const auto first=selected_>5?selected_-5:0;const auto last=std::min(count,first+7);
    for(std::size_t i=first;i<last;++i) {
      const float y=352+static_cast<float>(i-first)*66;const bool focused=i==selected_;
      if(focused){c.glow(155,y-29,1015,56,{1,1,1,0.18F},3,12,0);c.rect(155,y-29,1015,56,{1,1,1,0.10F},3);}
      const auto [title,detail]=row_text(i);
      setup_controls::text(c,artwork_detail::elide(title,59),180,y,25,focused,focused?1.0F:0.78F,false,false);
      if(!detail.empty())setup_controls::text(c,artwork_detail::elide(detail,83),182,y+22,16,false,0.60F,false,false);
    }
    if(count==0)c.text(busy()?"Loading...":"No results. Try another search or game ID.",180,352,24,{1,1,1,0.70F});
    if(count>7)setup_controls::scrollbar(c,1190,321,455,count,7,first,0.8F);
    if(page_==Page::preview) {
      c.text("Preview",1280,340,23,{1,1,1,0.82F});
      if(preview_ready_&&texture_&&texture_->loaded.load(std::memory_order_acquire)&&texture_->width>0&&texture_->height>0) {
        const float scale=std::min(475.0F/texture_->width,340.0F/texture_->height);
        const float width=texture_->width*scale,height=texture_->height*scale;
        c.image(*texture_,1280+(475-width)/2,375+(340-height)/2,width,height);
      }else c.text(decode_busy_||upload_.valid()?"Preparing preview...":"Preview unavailable",1280,475,21,{1,1,1,0.60F});
      c.text(artwork_detail::elide(preview_.source,45),1280,756,18,{1,1,1,0.72F});
      if(preview_.width)c.text(dimensions(preview_.width,preview_.height)+" · original",1280,785,18,{1,1,1,0.64F});
    }else {
      c.text("SteamGridDB",1280,357,24,{1,1,1,0.78F});
      c.text(provider_id_?"Game #"+std::to_string(provider_id_):"No game selected",1280,402,21,{1,1,1,0.66F});
      if(!provider_name_.empty())c.text(artwork_detail::elide(provider_name_,39),1280,438,19,{1,1,1,0.66F});
      c.text("Select a game, then browse a role.",1280,508,19,{1,1,1,0.60F});
      c.text("Artwork changes require confirmation.",1280,543,19,{1,1,1,0.60F});
    }
    std::string status=message_;
    if(work_busy_){std::lock_guard lock(transfer_->mutex);if(transfer_->total)status+=" · "+std::to_string(transfer_->done/1024)+" / "+std::to_string((transfer_->total+1023)/1024)+" KB";}
    if(!status.empty())c.text(artwork_detail::elide(status,143),180,843,18,{1,1,1,0.78F});
    if(page_==Page::assets)host->render_message_footer(renderer,std::array{std::pair{action::ok,"Preview"},std::pair{action::left,"Previous page"},std::pair{action::right,"Next page"},std::pair{action::options,"Information"},std::pair{action::cancel,"Back"}},1);
    else host->render_message_footer(renderer,std::array{std::pair{action::ok,"Select"},std::pair{action::options,"Information"},std::pair{action::cancel,"Back"}},1);
  }
private:
  static std::string dimensions(unsigned width,unsigned height){return std::to_string(width)+" × "+std::to_string(height);}
  bool same_origin() const {
    if(origin_root_.empty()||origin_profile_.empty()||config::CONFIG.profileConfigPath!=origin_config_)return false;
    try {
      std::error_code error;
      if(!std::filesystem::is_directory(origin_root_,error)||error||!std::filesystem::exists(origin_root_/"profiles.json",error)||error||!std::filesystem::is_directory(origin_root_/"profiles"/origin_profile_,error)||error)return false;
      return openxmb::desktop::Library(origin_root_).active_profile().id==origin_profile_;
    }catch(...){return false;}
  }
  void invalidate_origin(){origin_valid_=false;cancel_jobs();preview_ready_=false;message_="The active profile changed. Close and reopen Artwork.";}
  bool guard(){if(!origin_valid_)return false;if(!same_origin()){invalidate_origin();return false;}return true;}
  void cancel_jobs(){work_.cancel();decode_.cancel();++work_generation_;++decode_generation_;work_busy_=decode_busy_=false;decoded_.reset();}
  bool busy()const{return work_busy_||decode_busy_||(page_==Page::preview&&upload_.valid());}
  void wait_gpu(){if(host_)host_->get_window()->waitIdle();else loader_.getDevice().waitIdle();}
  std::size_t role_index()const{return static_cast<std::size_t>(std::find(artwork_detail::roles.begin(),artwork_detail::roles.end(),role_)-artwork_detail::roles.begin());}
  std::size_t row_count()const {
    if(page_==Page::home)return 7;
    if(page_==Page::role)return 7;
    if(page_==Page::games)return games_.size();
    if(page_==Page::assets)return assets_.size();
    return preview_.assignable?(preview_.role==Role::horizontal_grid?2:1):1;
  }
  std::pair<std::string,std::string> row_text(std::size_t index)const {
    if(page_==Page::home) {
      if(index<4){const auto role=artwork_detail::roles[index];const auto path=artwork_detail::selected_path(game_,role);return {artwork_detail::role_name(role),path.empty()?"No artwork selected":artwork_detail::path_text(path)};}
      if(index==4)return {"Search SteamGridDB",search_text_};
      if(index==5)return {"Enter SteamGridDB game ID",provider_id_?std::to_string(provider_id_):"Explicit numeric game ID"};
      return {"API key environment variable",key_env_};
    }
    if(page_==Page::role) {
      if(index==0){const auto path=artwork_detail::selected_path(game_,role_);return {"Preview current artwork",path.empty()?"No selected image":artwork_detail::path_text(path)};}
      if(index==1){const auto path=artwork_detail::native_path(game_,role_);return {"Use native artwork",path.empty()?"No native PS3 image exists for this role":artwork_detail::path_text(path)};}
      if(index==2)return {"Choose custom image file","Existing file; decoded before it can be applied"};
      if(index==3)return {"Browse SteamGridDB artwork",provider_id_?"Game #"+std::to_string(provider_id_)+" · "+artwork_detail::role_name(role_):"First search or enter a game ID"};
      if(index==4)return {"Search SteamGridDB",search_text_};
      if(index==5)return {"Enter SteamGridDB game ID",provider_id_?std::to_string(provider_id_):"Explicit numeric game ID"};
      return {"API key environment variable",key_env_};
    }
    if(page_==Page::games){const auto& game=games_.at(index);return {game.name,"Game #"+std::to_string(game.id)+(game.exact_match?" · exact title match":" · title differs; check before selecting")};}
    if(page_==Page::assets){const auto& asset=assets_.at(index);return {std::string(artwork_detail::role_name(asset.role))+" #"+std::to_string(asset.id),dimensions(asset.width,asset.height)+" · "+asset.mime+" · SteamGridDB"};}
    if(!preview_.assignable)return {"Back to artwork role","Current selection is unchanged"};
    if(index==1)return {"Use for horizontal grid and icon","Explicitly changes both the cover and icon"};
    return {"Use for "+std::string(artwork_detail::role_name(preview_.role)),preview_.native?"Restore native image and automatic discovery":"Change only this artwork role"};
  }
  void activate() {
    if(page_==Page::home){if(selected_<4){role_=artwork_detail::roles[selected_];page_=Page::role;selected_=0;}else if(selected_==4)prompt_search();else if(selected_==5)prompt_id();else prompt_env();return;}
    if(page_==Page::role) {
      if(selected_==0){const auto path=artwork_detail::selected_path(game_,role_);if(!artwork_detail::existing(path)){message_="The selected artwork file does not exist.";return;}begin_preview(path,"Current library selection",false,false);}
      else if(selected_==1){const auto path=artwork_detail::native_path(game_,role_);if(path.empty()){message_="No native PS3 artwork exists for this role.";return;}begin_preview(path,"Native PS3 artwork",true,true);}
      else if(selected_==2)prompt_file();
      else if(selected_==3){if(!provider_id_){message_="Search SteamGridDB or enter a numeric game ID first.";return;}load_assets(0);}
      else if(selected_==4)prompt_search();else if(selected_==5)prompt_id();else prompt_env();return;
    }
    if(page_==Page::games){if(selected_>=games_.size())return;const auto& game=games_[selected_];provider_id_=game.id;provider_name_=game.name;page_=Page::role;selected_=3;message_="Game selected explicitly. Browse a role to request artwork.";return;}
    if(page_==Page::assets){if(selected_<assets_.size())download(assets_[selected_]);return;}
    if(!preview_.assignable){page_=Page::role;selected_=0;return;}
    if(!preview_ready_){message_="A successfully decoded preview is required before applying artwork.";return;}
    request_apply(selected_==1);
  }
  std::string transient_key()const {
    if(!artwork_detail::env_name(key_env_))return {};
    const auto* value=std::getenv(key_env_.c_str());return value?std::string(value):std::string{};
  }
  bool can_request(){if(!guard())return false;if(Clock::now()<retry_after_){message_="SteamGridDB rate limited this request. Wait a minute before retrying.";return false;}return true;}
  template<class Function>void start(artwork_detail::WorkKind kind,Function function) {
    transfer_=std::make_shared<artwork_detail::Transfer>();const auto transfer=transfer_;const auto generation=++work_generation_;work_busy_=true;
    work_.start([function=std::move(function),kind,generation,transfer](std::stop_token stop)mutable {
      auto result=function(stop,transfer);result.kind=kind;result.generation=generation;return result;
    });
  }
  void prompt_search() {
    const auto weak=std::weak_ptr<std::atomic_bool>(alive_);
    host_->emplace_overlay<text_entry_overlay>("Search SteamGridDB",search_text_,[this,weak](std::string text) {
      const auto alive=weak.lock();if(!alive||!alive->load()||!can_request())return;
      if(text.empty()||text.size()>256){message_="Enter a game title of at most 256 bytes.";return;}
      search_text_=text;const auto cache=cache_root_;auto key=transient_key();message_="Searching SteamGridDB...";
      start(artwork_detail::WorkKind::search,[cache,title=std::move(text),key=std::move(key)](auto stop,auto transfer) {
        artwork_detail::Work out;openxmb::desktop::SteamGridDB provider(cache,artwork_detail::transport(stop,transfer));out.search=provider.search(title,key);return out;
      });
    });
  }
  void prompt_id() {
    const auto weak=std::weak_ptr<std::atomic_bool>(alive_);
    host_->emplace_overlay<text_entry_overlay>("SteamGridDB numeric game ID",provider_id_?std::to_string(provider_id_):"",[this,weak](std::string text) {
      const auto alive=weak.lock();if(!alive||!alive->load()||!guard())return;
      std::uint64_t id{};const auto parsed=std::from_chars(text.data(),text.data()+text.size(),id);
      if(text.empty()||parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()||id==0){message_="Enter one positive numeric SteamGridDB game ID.";return;}
      provider_id_=id;provider_name_.clear();page_=Page::role;selected_=3;message_="Game ID selected. Browse a role to request artwork.";
    });
  }
  void prompt_env() {
    const auto weak=std::weak_ptr<std::atomic_bool>(alive_);
    host_->emplace_overlay<text_entry_overlay>("API key environment variable name",key_env_,[this,weak](std::string text) {
      const auto alive=weak.lock();if(!alive||!alive->load()||!guard())return;
      if(!artwork_detail::env_name(text)){message_="Enter an environment variable name, not an API key.";return;}
      const auto previous=config::CONFIG.preferences;config::CONFIG.preferences["artwork.steamgriddb.key_env"]=text;
      if(!config::CONFIG.save_config_checked()){config::CONFIG.preferences=previous;message_="Could not save the environment variable name.";return;}
      key_env_=std::move(text);message_="Variable name saved. Set that process variable to your SteamGridDB API key.";
    });
  }
  void prompt_file() {
    const auto weak=std::weak_ptr<std::atomic_bool>(alive_);
    host_->emplace_overlay<text_entry_overlay>("Artwork image file",artwork_detail::path_text(artwork_detail::selected_path(game_,role_)),[this,weak](std::string text) {
      const auto alive=weak.lock();if(!alive||!alive->load()||!guard())return;
      try {auto path=artwork_detail::text_path(text);if(!path.is_absolute()){message_="Enter the full path to an existing artwork image.";return;}begin_preview(std::move(path),"Custom image file",true,false);}
      catch(...){message_="The artwork file path is invalid.";}
    });
  }
  void load_assets(unsigned page) {
    if(!can_request())return;const auto cache=cache_root_;const auto id=provider_id_;const auto role=role_;auto key=transient_key();message_="Loading SteamGridDB artwork...";
    start(artwork_detail::WorkKind::assets,[cache,id,role,page,key=std::move(key)](auto stop,auto transfer) {
      artwork_detail::Work out;out.game_id=id;out.role=role;out.page=page;openxmb::desktop::SteamGridDB provider(cache,artwork_detail::transport(stop,transfer));out.assets=provider.assets(id,role,key,page);return out;
    });
  }
  void download(openxmb::desktop::SteamGridDBAsset asset) {
    if(!can_request())return;const auto cache=cache_root_;const auto page=asset_page_;auto key=transient_key();message_="Downloading selected SteamGridDB artwork...";
    start(artwork_detail::WorkKind::download,[cache,asset=std::move(asset),page,key=std::move(key)](auto stop,auto transfer) {
      artwork_detail::Work out;out.game_id=asset.game_id;out.role=asset.role;out.page=page;openxmb::desktop::SteamGridDB provider(cache,artwork_detail::transport(stop,transfer));
      // Download is preview-only. Library/native override confirmation occurs
      // after actual decoding, immediately before a role-specific save.
      out.download=provider.select(asset.game_id,asset.id,asset.role,{},openxmb::desktop::SteamGridDBOverride::override_native,key,page);return out;
    });
  }
  void finish(artwork_detail::Work value) {
    const auto note=[&](const std::string& text){message_=text;if(text.find("Set a SteamGridDB API key")!=std::string::npos)message_+=" · process variable: "+key_env_;if(text.find("429")!=std::string::npos)retry_after_=Clock::now()+std::chrono::seconds(60);};
    if(value.kind==artwork_detail::WorkKind::search) {
      note(value.search.message);if(!value.search)return;games_=std::move(value.search.games);page_=Page::games;selected_=0;
      if(message_.empty())message_=value.search.stale?"Showing stale cached search results.":value.search.from_cache?"Showing cached search results.":"Select a game; title matches are marked explicitly.";
    }else if(value.kind==artwork_detail::WorkKind::assets) {
      note(value.assets.message);if(!value.assets)return;assets_=std::move(value.assets.assets);asset_page_=value.assets.page;has_more_=value.assets.has_more;role_=value.role;page_=Page::assets;selected_=0;
      if(message_.empty())message_=value.assets.stale?"Showing stale cached artwork metadata.":value.assets.from_cache?"Showing cached artwork metadata.":"Select an asset to download and preview it.";
    }else {
      note(value.download.message);if(!value.download)return;role_=value.role;
      begin_preview(std::move(value.download.path),value.download.from_cache?"SteamGridDB · cached image":"SteamGridDB · downloaded image",true,false);
    }
  }
  void begin_preview(std::filesystem::path path,std::string source,bool assignable,bool native) {
    if(!guard())return;
    preview_return_=page_;preview_return_selection_=selected_;page_=Page::preview;selected_=0;preview_ready_=false;decoded_.reset();
    preview_=Preview{};preview_.path=std::move(path);preview_.role=role_;preview_.source=std::move(source);preview_.assignable=assignable;preview_.native=native;
    const auto preview=preview_;const auto generation=++decode_generation_;decode_busy_=true;message_="Decoding artwork preview...";
    decode_.start([preview,generation](std::stop_token stop){return artwork_detail::decode(preview,generation,stop);});
  }
  std::optional<openxmb::desktop::Game> fresh_game() {
    if(!guard())return {};
    try {openxmb::desktop::Library library(origin_root_);if(library.active_profile().id!=origin_profile_){invalidate_origin();return {};}
      const auto games=library.games();const auto found=std::find_if(games.begin(),games.end(),[&](const auto& game){return game.id==game_.id;});
      if(found==games.end()){message_="This game was removed. Close and reopen Artwork.";return {};}return *found;
    }catch(const std::exception& error){message_=error.what();return {};}
  }
  bool native_override_needed(const openxmb::desktop::Game& game,const Preview& preview,bool both)const {
    const auto native=artwork_detail::native_path(game,preview.role);
    if(!native.empty()&&!artwork_detail::same_path(native,preview.path))return true;
    if(both){const auto icon=artwork_detail::native_path(game,Role::icon);if(!icon.empty()&&!artwork_detail::same_path(icon,preview.path))return true;}
    return false;
  }
  void request_apply(bool both) {
    if(!preview_ready_)return;const auto current=fresh_game();if(!current)return;
    const auto preview=preview_;const bool overrides=native_override_needed(*current,preview,both);
    std::string text=preview.source+"\n"+dimensions(preview.width,preview.height)+"\n"+artwork_detail::path_text(preview.path)+"\n\n";
    text+=both?"Use this image for the horizontal grid and icon?":"Use this image for "+std::string(artwork_detail::role_name(preview.role))+"?";
    if(overrides)text+="\nNative PS3 artwork exists. Applying this image explicitly overrides its priority.";
    const auto weak=std::weak_ptr<std::atomic_bool>(alive_);
    host_->emplace_overlay<message_overlay>(overrides?"Override native artwork?":"Apply selected artwork?",std::move(text),std::vector<std::string>{"Cancel",overrides?"Override native":"Apply"},[this,weak,preview,both,overrides](unsigned choice) {
      const auto alive=weak.lock();if(!alive||!alive->load()||choice!=1)return;persist(preview,both,overrides);
    });
  }
  void persist(const Preview& preview,bool both,bool allow_override) {
    const auto current=fresh_game();if(!current)return;
    try {
      std::error_code error;
      if(!artwork_detail::existing(preview.path)||std::filesystem::file_size(preview.path,error)!=preview.file_size||error||std::filesystem::last_write_time(preview.path,error)!=preview.modified||error){message_="The artwork file changed or disappeared. Preview it again before applying.";return;}
      if(preview.native&&!artwork_detail::same_path(artwork_detail::native_path(*current,preview.role),preview.path)){message_="Native artwork changed. Preview it again before applying.";return;}
      if(native_override_needed(*current,preview,both)&&!allow_override){message_="Native artwork now exists. Confirm its override before applying.";return;}
      // Reopen at the captured root and read again immediately before writing.
      // No stale Game snapshot may replace concurrent metadata or other roles.
      openxmb::desktop::Library library(origin_root_);
      if(!guard()||library.active_profile().id!=origin_profile_)return;
      auto games=library.games();auto found=std::find_if(games.begin(),games.end(),[&](const auto& game){return game.id==game_.id;});
      if(found==games.end()){message_="This game was removed. No artwork was saved.";return;}
      if(native_override_needed(*found,preview,both)&&!allow_override){message_="Native artwork changed. Preview and confirm again.";return;}
      artwork_detail::selected_path(*found,preview.role)=preview.path;
      if(both)found->icon=preview.path;
      if(preview.native)found->automatic_artwork[artwork_detail::role_key(preview.role)]=preview.path;
      game_=library.save_game(std::move(*found));host_->queue_menu_reload();
      page_=Page::role;selected_=0;message_=both?"Horizontal grid and icon saved.":std::string(artwork_detail::role_name(preview.role))+" saved.";
    }catch(const std::exception& error){message_=std::string("Could not save artwork: ")+error.what();}
  }
  void show_information() {
    std::string text="Library game: "+game_.title+"\nRole: "+artwork_detail::role_name(role_);
    if(page_==Page::preview)text+="\nSource: "+preview_.source+"\nFile: "+artwork_detail::path_text(preview_.path)+(preview_.width?"\nDimensions: "+dimensions(preview_.width,preview_.height):"");
    else if(page_==Page::assets&&selected_<assets_.size()){const auto& asset=assets_[selected_];text+="\nSteamGridDB game: "+std::to_string(asset.game_id)+"\nAsset: "+std::to_string(asset.id)+"\nDimensions: "+dimensions(asset.width,asset.height)+"\nFormat: "+asset.mime+"\nSource: "+asset.url;}
    else if(page_==Page::games&&selected_<games_.size()){const auto& game=games_[selected_];text+="\nSteamGridDB game: "+std::to_string(game.id)+"\nTitle: "+game.name+(game.exact_match?"\nExact title match":"\nTitle differs. Selection is always manual.");}
    else text+="\nCurrent: "+artwork_detail::path_text(artwork_detail::selected_path(game_,role_))+"\nNative PS3: "+artwork_detail::path_text(artwork_detail::native_path(game_,role_))+"\nAPI key variable: "+key_env_+"\nOnly the variable name is saved; the key is read from the process environment when requested.";
    host_->emplace_overlay<message_overlay>("Artwork information",std::move(text),std::vector<std::string>{"OK"});
  }
  auroreengine::resource_loader& loader_;shell* host_{};openxmb::desktop::Game game_;
  std::filesystem::path origin_root_,origin_config_,cache_root_;std::string origin_profile_,key_env_,search_text_,provider_name_,message_;
  std::shared_ptr<std::atomic_bool> alive_=std::make_shared<std::atomic_bool>(true);
  std::shared_ptr<artwork_detail::Transfer> transfer_=std::make_shared<artwork_detail::Transfer>();
  openxmb::desktop::AsyncJob<artwork_detail::Work> work_;openxmb::desktop::AsyncJob<Image> decode_;
  std::uint64_t provider_id_{},work_generation_{},decode_generation_{},upload_generation_{};
  Page page_{Page::home},preview_return_{Page::role};Role role_{Role::icon};std::size_t selected_{},preview_return_selection_{};
  unsigned asset_page_{};bool has_more_{},origin_valid_{true},work_busy_{},decode_busy_{},closing_{},preview_ready_{};
  Clock::time_point last_origin_check_{},retry_after_{};
  std::vector<openxmb::desktop::SteamGridDBGame> games_;std::vector<openxmb::desktop::SteamGridDBAsset> assets_;
  Preview preview_,upload_preview_;std::optional<Image> decoded_;std::future<void> upload_;
  std::shared_ptr<auroreengine::texture> texture_,incoming_;
};
} // namespace app
