#include "openxmb/desktop/trophy_watch.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace openxmb::desktop {
namespace {
namespace fs=std::filesystem;
constexpr std::uintmax_t max_file=4*1024*1024;
constexpr std::size_t max_sets=32,max_directory_entries=256,max_events=128;
std::string text(const fs::path& path){const auto value=path.u8string();return {value.begin(),value.end()};}
bool set_name(std::string_view name){return name.size()==12&&name.starts_with("NPWR")&&name[9]=='_'&&
  std::all_of(name.begin()+4,name.begin()+9,[](char c){return c>='0'&&c<='9';})&&
  std::all_of(name.begin()+10,name.end(),[](char c){return c>='0'&&c<='9';});}
void valid_path(const fs::path& path){
  if(path.empty()||!path.is_absolute()||path.native().size()>32768||path.native().find(fs::path::value_type{})!=fs::path::string_type::npos)
    throw std::runtime_error("Choose an absolute local trophy profile and game source.");
  const auto value=text(path);if(value.starts_with("//")||value.starts_with("\\\\"))throw std::runtime_error("Live trophy observation requires local source directories.");
#ifdef _WIN32
  if(GetDriveTypeW(path.root_path().c_str())==DRIVE_REMOTE)throw std::runtime_error("Live trophy observation requires local source directories.");
#endif
}
void validate(const TrophyWatchSession& session){
  if(!session.generation||session.session_id.empty()||session.session_id.size()>512||session.session_id.find('\0')!=std::string::npos)
    throw std::runtime_error("Live trophy observation requires a current session generation.");
  if(session.rpcs3_user_id.size()!=8||!std::all_of(session.rpcs3_user_id.begin(),session.rpcs3_user_id.end(),[](char c){return c>='0'&&c<='9';}))
    throw std::runtime_error("Select the exact RPCS3 user before observing live trophies.");
  valid_path(session.profile_directory);valid_path(session.game_directory);valid_path(session.rpcs3_hdd0);
}
std::string identity(const fs::path& path,bool directory){
  std::error_code error;const auto status=fs::symlink_status(path,error);
  if(error||(directory?!fs::is_directory(status):!fs::is_regular_file(status)))throw std::runtime_error("Trophy source is unavailable or is a link.");
#ifdef _WIN32
  const auto handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
    FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr);
  if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Trophy source identity is unavailable.");
  BY_HANDLE_FILE_INFORMATION info{};const bool okay=GetFileInformationByHandle(handle,&info)!=FALSE&&(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)==0&&
    (info.nFileIndexHigh!=0||info.nFileIndexLow!=0);CloseHandle(handle);
  if(!okay)throw std::runtime_error("Trophy source identity changed or is a link.");
  return std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+std::to_string(info.nFileIndexLow);
#else
  struct stat info{};if(lstat(path.c_str(),&info)!=0||S_ISLNK(info.st_mode)||info.st_ino==0)throw std::runtime_error("Trophy source identity is unavailable.");
  return std::to_string(info.st_dev)+":"+std::to_string(info.st_ino);
#endif
}
struct Resolved {fs::path profile,game,hdd0;};
Resolved resolve(const TrophyWatchSession& session){
  validate(session);
  Resolved result{fs::canonical(session.profile_directory),fs::canonical(session.game_directory),fs::canonical(session.rpcs3_hdd0)};
  valid_path(result.profile);valid_path(result.game);valid_path(result.hdd0);return result;
}
std::vector<std::string> associated_sets(const Resolved& source){
  const auto directory=source.game/"TROPDIR";(void)identity(directory,true);
  std::vector<std::string> result;std::size_t entries=0;
  for(const auto& item:fs::directory_iterator(directory)){
    if(++entries>max_directory_entries)throw std::runtime_error("The game's TROPDIR exceeds its observation limit.");
    const auto name=text(item.path().filename());if(!set_name(name))continue;
    try{(void)identity(item.path(),true);(void)identity(item.path()/"TROPHY.TRP",false);}catch(const std::exception&){continue;}
    if(result.size()>=max_sets)throw std::runtime_error("The game has too many trophy sets for live observation.");
    result.push_back(name);
  }
  std::sort(result.begin(),result.end());return result;
}
std::string root_identity(const Resolved& source,const TrophyWatchSession& session){
  const auto trophy=source.hdd0/"home"/session.rpcs3_user_id/"trophy";
  std::string result;
  for(const auto& path:{source.profile,source.game,source.game/"TROPDIR",source.hdd0,source.hdd0/"home",source.hdd0/"home"/session.rpcs3_user_id,trophy})
    result+=identity(path,true)+"/";
  return result;
}
std::string source_identity(const Resolved& source,const TrophyWatchSession& session,const std::string& id){
  return root_identity(source,session)+identity(source.game/"TROPDIR"/id,true)+"/"+
    identity(source.hdd0/"home"/session.rpcs3_user_id/"trophy"/id,true)+"/"+identity(source.game/"TROPDIR"/id/"TROPHY.TRP",false);
}
struct FileMark {
  std::string identity;
  std::uintmax_t size{};
  fs::file_time_type changed{};
  std::uint64_t hash{};
  bool operator==(const FileMark&)const=default;
};
FileMark mark(const fs::path& path){
  FileMark value;value.identity=identity(path,false);value.size=fs::file_size(path);value.changed=fs::last_write_time(path);
  if(value.size>max_file)throw std::runtime_error("Trophy source exceeds the bounded file size.");return value;
}
template<class Canceled>FileMark fingerprint(const fs::path& path,Canceled&& canceled){
  auto value=mark(path);std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("Trophy source could not be opened.");
  std::array<char,16384> buffer{};std::uintmax_t total=0;value.hash=14695981039346656037ull;
  while(input){
    if(canceled())throw std::runtime_error("Trophy observation changed session.");
    input.read(buffer.data(),buffer.size());const auto count=input.gcount();total+=static_cast<std::uintmax_t>(count);
    if(total>max_file||total>value.size)throw std::runtime_error("Trophy source changed while reading.");
    for(std::streamsize i=0;i<count;++i){value.hash^=static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);value.hash*=1099511628211ull;}
  }
  auto after=mark(path);after.hash=value.hash;
  if(input.bad()||total!=value.size||after!=value)throw std::runtime_error("Trophy source changed while reading.");return value;
}
struct Sample {std::string source;FileMark metadata,state;AchievementGame game;};
bool same_states(const AchievementGame& first,const AchievementGame& second){
  if(first.entries.size()!=second.entries.size())return false;
  for(std::size_t i=0;i<first.entries.size();++i){const auto& a=first.entries[i];const auto& b=second.entries[i];
    if(a.id!=b.id||a.unlock_state_known!=b.unlock_state_known||a.unlocked!=b.unlocked||a.unlocked_at!=b.unlocked_at)return false;
  }return true;
}
bool same_sample(const Sample& a,const Sample& b){return a.source==b.source&&a.metadata==b.metadata&&a.state==b.state&&same_states(a.game,b.game);}
template<class Canceled>Sample read_sample(const Resolved& source,const TrophyWatchSession& session,const std::string& id,Canceled&& canceled){
  Sample sample;sample.source=source_identity(source,session,id);
  const auto directory=source.hdd0/"home"/session.rpcs3_user_id/"trophy"/id;
  sample.metadata=fingerprint(directory/"TROPCONF.SFM",canceled);sample.state=fingerprint(directory/"TROPUSR.DAT",canceled);
  if(canceled())throw std::runtime_error("Trophy observation changed session.");
  auto read=read_rpcs3_trophy_set(source.hdd0,session.rpcs3_user_id,id);
  if(!read)throw std::runtime_error(read.error);
  if(read.value->entries.empty()||!std::all_of(read.value->entries.begin(),read.value->entries.end(),[](const Achievement& value){return value.unlock_state_known;}))
    throw std::runtime_error("Trophy unlock state is incomplete; waiting for a stable RPCS3 write.");
  auto xml_after=mark(directory/"TROPCONF.SFM"),state_after=mark(directory/"TROPUSR.DAT");
  xml_after.hash=sample.metadata.hash;state_after.hash=sample.state.hash;
  if(xml_after!=sample.metadata||state_after!=sample.state||source_identity(source,session,id)!=sample.source)
    throw std::runtime_error("Trophy source changed during genuine state parsing.");
  sample.game=std::move(*read.value);return sample;
}
} // namespace

struct Rpcs3TrophyWatch::Impl {
  struct Model {std::optional<Sample> candidate,accepted;std::set<std::string> seen_unlocked;};
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::optional<TrophyWatchSession> session;
  TrophyWatchStatus current;
  std::deque<TrophyUnlockEvent> events;
  std::atomic<std::uint64_t> epoch{};
  std::chrono::milliseconds interval;
  std::jthread worker;
  explicit Impl(std::chrono::milliseconds poll):interval(std::clamp(poll,std::chrono::milliseconds(25),std::chrono::milliseconds(5000))),worker([this](std::stop_token stop){run(stop);}){}
  ~Impl(){worker.request_stop();wake.notify_all();worker.join();}
  void discard_set(std::uint64_t generation,const std::string& id){std::lock_guard lock(mutex);if(epoch.load()!=generation)return;
    std::erase_if(events,[&](const TrophyUnlockEvent& value){return value.set_id==id;});current.queued=events.size();}
  void run(std::stop_token stop){
    std::uint64_t generation=0;std::map<std::string,Model> models;std::size_t cursor=0;std::string common_source;
    while(!stop.stop_requested()){
      TrophyWatchSession bound;std::uint64_t requested;
      {std::unique_lock lock(mutex);wake.wait(lock,[&]{return stop.stop_requested()||session.has_value();});if(stop.stop_requested())return;bound=*session;requested=epoch.load();}
      if(generation!=requested){generation=requested;models.clear();cursor=0;common_source.clear();}
      const auto canceled=[&]{return stop.stop_requested()||epoch.load()!=generation;};
      std::string error;std::vector<TrophyUnlockEvent> produced;bool mapped=false;
      try{
        const auto source=resolve(bound);const auto common=root_identity(source,bound);
        if(common!=common_source){
          models.clear();cursor=0;common_source=common;
          std::lock_guard lock(mutex);if(epoch.load()==generation){events.clear();current.queued=0;}
        }
        const auto sets=associated_sets(source);
        for(auto it=models.begin();it!=models.end();)if(!std::binary_search(sets.begin(),sets.end(),it->first)){discard_set(generation,it->first);it=models.erase(it);}else ++it;
        for(const auto& id:sets)models.try_emplace(id);
        if(sets.empty())throw std::runtime_error("This game has no exact readable TROPDIR trophy-set association.");
        mapped=true;
        const auto& id=sets[cursor++%sets.size()];auto& model=models.at(id);
        try{
          auto sample=read_sample(source,bound,id,canceled);
          if(model.accepted&&(model.accepted->source!=sample.source||model.accepted->metadata.identity!=sample.metadata.identity||model.accepted->metadata.hash!=sample.metadata.hash)){
            model.accepted.reset();model.candidate.reset();model.seen_unlocked.clear();discard_set(generation,id);
          }
          if(model.candidate&&same_sample(*model.candidate,sample)){
            if(model.accepted){
              std::map<std::string,bool> previous;for(const auto& item:model.accepted->game.entries)previous.emplace(item.id,item.unlock_state_known&&!item.unlocked);
              for(const auto& item:sample.game.entries)if(item.unlocked&&item.unlock_state_known){
                const bool fresh=model.seen_unlocked.insert(item.id).second;
                if(fresh&&previous.contains(item.id)&&previous.at(item.id))produced.push_back({bound,"rpcs3",bound.rpcs3_user_id,id,item.id,sample.game.title,sample.game.source_directory,
                  sample.source+":"+sample.metadata.identity+":"+std::to_string(sample.metadata.hash),item});
              }
            }else for(const auto& item:sample.game.entries)if(item.unlocked)model.seen_unlocked.insert(item.id);
            model.accepted=sample;
          }
          model.candidate=std::move(sample);
        }catch(const std::exception& failure){model.candidate.reset();discard_set(generation,id);throw;}
      }catch(const std::exception& failure){
        error=failure.what();
        if(!mapped){models.clear();std::lock_guard lock(mutex);if(epoch.load()==generation){events.clear();current.queued=0;}}
      }
      if(!canceled()){
        std::lock_guard lock(mutex);if(epoch.load()==generation){
          ++current.completed_polls;current.associated_sets=models.size();current.error=std::move(error);
          current.baseline_ready=!models.empty()&&std::all_of(models.begin(),models.end(),[](const auto& item){return item.second.accepted.has_value();});
          for(auto& event:produced){if(events.size()==max_events){events.pop_front();++current.dropped;}events.push_back(std::move(event));}
          current.queued=events.size();
        }
      }
      std::unique_lock lock(mutex);wake.wait_for(lock,interval,[&]{return stop.stop_requested()||epoch.load()!=generation;});
    }
  }
};
Rpcs3TrophyWatch::Rpcs3TrophyWatch(std::chrono::milliseconds interval):impl_(std::make_unique<Impl>(interval)){}
Rpcs3TrophyWatch::~Rpcs3TrophyWatch()=default;
void Rpcs3TrophyWatch::set_session(std::optional<TrophyWatchSession> session){
  std::lock_guard lock(impl_->mutex);if(impl_->session==session)return;
  impl_->session=std::move(session);++impl_->epoch;impl_->events.clear();impl_->current={};impl_->current.watching=impl_->session.has_value();impl_->wake.notify_all();
}
std::vector<TrophyUnlockEvent> Rpcs3TrophyWatch::take_events(){
  std::lock_guard lock(impl_->mutex);std::vector<TrophyUnlockEvent> result;result.reserve(impl_->events.size());
  while(!impl_->events.empty()){result.push_back(std::move(impl_->events.front()));impl_->events.pop_front();}impl_->current.queued=0;return result;
}
TrophyWatchStatus Rpcs3TrophyWatch::status()const{std::lock_guard lock(impl_->mutex);return impl_->current;}
} // namespace openxmb::desktop
