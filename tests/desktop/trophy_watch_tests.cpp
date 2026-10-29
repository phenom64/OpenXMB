#include "openxmb/desktop/trophy_watch.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs=std::filesystem;
using namespace openxmb::desktop;
using namespace std::chrono_literals;
namespace {
void require(bool condition,const char* why){if(!condition)throw std::runtime_error(why);}
void write(const fs::path& path,std::string_view bytes){fs::create_directories(path.parent_path());std::ofstream file(path,std::ios::binary);file.write(bytes.data(),bytes.size());if(!file)throw std::runtime_error("isolated trophy fixture write failed");}
std::string read(const fs::path& path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
void put32(std::string& bytes,std::size_t at,std::uint32_t value){for(int i=3;i>=0;--i){bytes[at+i]=static_cast<char>(value&255);value>>=8;}}
void put64(std::string& bytes,std::size_t at,std::uint64_t value){for(int i=7;i>=0;--i){bytes[at+i]=static_cast<char>(value&255);value>>=8;}}
std::string state(bool first=true,bool second=false,bool third=false){
  constexpr unsigned count=3;constexpr std::size_t table6=0x70+count*0x60;
  std::string bytes(table6+count*0x70,'\0');put32(bytes,0,0x818f54ad);put32(bytes,8,2);
  put32(bytes,0x30,4);put32(bytes,0x34,0x50);put32(bytes,0x3c,count);put64(bytes,0x40,0x70);
  put32(bytes,0x50,6);put32(bytes,0x54,0x60);put32(bytes,0x5c,count);put64(bytes,0x60,table6);
  const bool unlocked[]{first,second,third};const unsigned grades[]{4,2,3};
  for(unsigned i=0;i<count;++i){const auto four=0x70+i*0x60;const auto six=table6+i*0x70;
    put32(bytes,four,4);put32(bytes,four+4,0x50);put32(bytes,four+8,i);put32(bytes,four+16,i);put32(bytes,four+20,grades[i]);
    put32(bytes,six,6);put32(bytes,six+4,0x60);put32(bytes,six+8,i);put32(bytes,six+16,i);put32(bytes,six+20,unlocked[i]);
    if(unlocked[i])put64(bytes,six+40,62135596800000000ull+1700000000ull*1000000+i*1000000);
  }return bytes;
}
const std::string xml="<trophyconf><title-name>Isolated watch fixture</title-name><trophy id=\"0\" ttype=\"B\" hidden=\"no\"><name>Historical</name><detail>Must stay silent</detail></trophy><trophy id=\"1\" ttype=\"G\" hidden=\"no\"><name>New gold</name><detail>Known edge</detail></trophy><trophy id=\"2\" ttype=\"S\" hidden=\"yes\"><name>New silver</name><detail>Stable retry</detail></trophy></trophyconf>";
template<class Function>void wait(Function condition,const char* why){const auto end=std::chrono::steady_clock::now()+5s;while(!condition()){if(std::chrono::steady_clock::now()>=end)throw std::runtime_error(why);std::this_thread::sleep_for(5ms);}}
void polls(Rpcs3TrophyWatch& watch,unsigned count=5){const auto target=watch.status().completed_polls+count;wait([&]{return watch.status().completed_polls>=target;},"trophy worker stopped polling its isolated fixture");}
struct Fixture {
  fs::path root,set,game_set;
  TrophyWatchSession session;
  explicit Fixture(fs::path path):root(std::move(path)),set(root/"hdd0/home/00000001/trophy/NPWR00001_00"),game_set(root/"game/TROPDIR/NPWR00001_00") {
    fs::create_directories(root/"profile");write(game_set/"TROPHY.TRP","Isolated association fixture, not a real trophy archive");
    write(set/"TROPCONF.SFM",xml);write(set/"TROPUSR.DAT",state());
    session={1,"controlled-session",root/"profile",root/"game",root/"hdd0","00000001"};
  }
};
void edges(const fs::path& root){
  Fixture f(root);Rpcs3TrophyWatch watch(25ms);watch.set_session(f.session);
  wait([&]{return watch.status().baseline_ready;},"initial known trophy state did not become a stable silent baseline");
  require(watch.take_events().empty(),"historical initial unlock generated a live notification");
  write(f.set/"TROPUSR.DAT",state(true,true));std::vector<TrophyUnlockEvent> events;
  wait([&]{events=watch.take_events();return !events.empty();},"known locked-to-unlocked edge was not observed");
  require(events.size()==1&&events[0].provider=="rpcs3"&&events[0].user_id=="00000001"&&events[0].set_id=="NPWR00001_00"&&events[0].trophy_id=="1"&&
    events[0].session==f.session&&events[0].achievement.grade==TrophyGrade::gold&&events[0].achievement.unlock_state_known&&events[0].achievement.unlocked&&
    !events[0].source_identity.empty(),"live edge lost full provider/user/set/trophy/session/source identity");
  polls(watch);require(watch.take_events().empty(),"unchanged unlock was delivered twice");
  write(f.set/"TROPUSR.DAT",state());polls(watch);write(f.set/"TROPUSR.DAT",state(true,true));polls(watch);
  require(watch.take_events().empty(),"state rollback and replay duplicated an already observed unlock");
  write(f.set/"TROPUSR.DAT","partial RPCS3 write");polls(watch);require(watch.take_events().empty(),"malformed state produced a trophy event");
  write(f.set/"TROPUSR.DAT",state(true,true,true));
  wait([&]{events=watch.take_events();return !events.empty();},"stable state after a non-atomic write lost its real unlock edge");
  require(events.size()==1&&events[0].trophy_id=="2","partial write fabricated or duplicated a trophy edge");
  write(f.set/"TROPUSR.DAT",state(false,false,false));polls(watch);
  write(f.set/"TROPUSR.DAT",state(true,true,true));polls(watch);watch.set_session(std::nullopt);
  require(watch.take_events().empty()&&!watch.status().watching,"session cancellation retained queued notifications");
  require(read(f.set/"TROPCONF.SFM")==xml&&read(f.set/"TROPUSR.DAT")==state(true,true,true)&&read(f.game_set/"TROPHY.TRP")=="Isolated association fixture, not a real trophy archive",
    "watcher modified trophy state metadata or game source files");
}
void isolation(const fs::path& root){
  Fixture f(root);Rpcs3TrophyWatch watch(25ms);
  write(f.set/"TROPUSR.DAT","initial unknown state");watch.set_session(f.session);polls(watch);
  write(f.set/"TROPUSR.DAT",state(true,true,true));wait([&]{return watch.status().baseline_ready;},"first valid state after unknown baseline was not accepted");
  require(watch.take_events().empty(),"unknown-to-unlocked state was mistaken for an earned trophy");
  f.session.generation=2;write(f.set/"TROPUSR.DAT",state(false,false,false));watch.set_session(f.session);
  wait([&]{return watch.status().baseline_ready;},"new generation did not establish its own baseline");
  const auto other=f.root/"hdd0/home/00000002/trophy/NPWR00001_00";write(other/"TROPCONF.SFM",xml);write(other/"TROPUSR.DAT",state(true,true,true));polls(watch);
  require(watch.take_events().empty(),"another RPCS3 user's unlock crossed the selected profile boundary");
  const auto unrelated=f.root/"hdd0/home/00000001/trophy/NPWR99999_00";write(unrelated/"TROPCONF.SFM",xml);write(unrelated/"TROPUSR.DAT",state(true,true,true));polls(watch);
  require(watch.take_events().empty(),"an unrelated trophy set was associated by title or directory guessing");
  auto changed_xml=xml;changed_xml.replace(changed_xml.find("Isolated watch fixture"),std::string_view("Isolated watch fixture").size(),"Replaced source fixture");
  write(f.set/"TROPCONF.SFM",changed_xml);write(f.set/"TROPUSR.DAT",state(true,true,true));polls(watch);
  require(watch.take_events().empty(),"changed metadata/source produced historical unlock notifications");
  watch.set_session(std::nullopt);f.session.generation=3;f.session.profile_directory=f.root/"other-profile";fs::create_directories(f.session.profile_directory);
  watch.set_session(f.session);wait([&]{return watch.status().baseline_ready;},"changed OpenXMB profile did not get a fresh baseline");
  require(watch.take_events().empty(),"historical unlocks leaked across session/profile generations");
}
void source_replacement_and_cancel(const fs::path& root){
  Fixture f(root);Rpcs3TrophyWatch watch(25ms);watch.set_session(f.session);
  wait([&]{return watch.status().baseline_ready;},"replacement fixture baseline failed");
  write(f.set/"TROPUSR.DAT",state(true,true));wait([&]{return watch.status().queued==1;},"replacement fixture edge was not queued");
  fs::rename(f.set,f.set.parent_path()/"old-source");write(f.set/"TROPCONF.SFM",xml);write(f.set/"TROPUSR.DAT",state(true,true,true));
  polls(watch);require(watch.take_events().empty()&&watch.status().baseline_ready,"reused trophy path retained old-source events or notified replacement history");
  f.session.generation=4;write(f.set/"TROPUSR.DAT",state());watch.set_session(f.session);
  wait([&]{return watch.status().baseline_ready;},"cancellation fixture baseline failed");
  write(f.set/"TROPUSR.DAT",state(true,true));wait([&]{return watch.status().queued==1;},"cancellation fixture edge was not queued");
  watch.set_session(std::nullopt);require(watch.take_events().empty(),"canceled session delivered a queued unlock from an old generation");
  f.session.generation=5;write(f.set/"TROPUSR.DAT",state());watch.set_session(f.session);
  wait([&]{return watch.status().baseline_ready;},"source-loss fixture baseline failed");
  write(f.set/"TROPUSR.DAT",state(true,true));wait([&]{return watch.status().queued==1;},"source-loss fixture edge was not queued");
  fs::rename(f.root/"game/TROPDIR",f.root/"game/removed-TROPDIR");polls(watch);
  require(watch.take_events().empty()&&!watch.status().baseline_ready,"lost game association retained notifications from an unknown source");
}
void stable_retry(const fs::path& root){
  Fixture f(root);Rpcs3TrophyWatch watch(100ms);watch.set_session(f.session);
  wait([&]{return watch.status().baseline_ready;},"stability fixture baseline failed");
  const auto count=watch.status().completed_polls;write(f.set/"TROPUSR.DAT",state(true,true));
  wait([&]{return watch.status().completed_polls>count;},"stability fixture did not sample its transient write");
  write(f.set/"TROPUSR.DAT",state());polls(watch,3);
  require(watch.take_events().empty(),"one transient valid read was treated as a stable earned trophy");
}
void bounded_queue(const fs::path& root){
  Fixture f(root);fs::remove_all(f.game_set);fs::remove_all(f.set);
  const auto many_state=[](bool unlocked){
    constexpr unsigned n=64;constexpr std::size_t sixth=0x70+n*0x60;std::string bytes(sixth+n*0x70,'\0');
    put32(bytes,0,0x818f54ad);put32(bytes,8,2);put32(bytes,0x30,4);put32(bytes,0x34,0x50);put32(bytes,0x3c,n);put64(bytes,0x40,0x70);
    put32(bytes,0x50,6);put32(bytes,0x54,0x60);put32(bytes,0x5c,n);put64(bytes,0x60,sixth);
    for(unsigned i=0;i<n;++i){const auto a=0x70+i*0x60;const auto b=sixth+i*0x70;put32(bytes,a,4);put32(bytes,a+4,0x50);put32(bytes,a+8,i);put32(bytes,a+16,i);put32(bytes,a+20,4);
      put32(bytes,b,6);put32(bytes,b+4,0x60);put32(bytes,b+8,i);put32(bytes,b+16,i);put32(bytes,b+20,unlocked);if(unlocked)put64(bytes,b+40,62135596800000000ull+1700000000ull*1000000);}
    return bytes;
  };
  std::string metadata="<trophyconf><title-name>Isolated overflow fixture</title-name>";
  for(unsigned i=0;i<64;++i)metadata+="<trophy id=\""+std::to_string(i)+"\" ttype=\"B\" hidden=\"no\"><name>Fixture</name><detail>Bounded queue</detail></trophy>";
  metadata+="</trophyconf>";const auto locked=many_state(false),unlocked=many_state(true);
  std::vector<fs::path> sets;
  for(const auto* id:{"NPWR00001_00","NPWR00002_00","NPWR00003_00"}){
    write(f.root/"game/TROPDIR"/id/"TROPHY.TRP","isolated association fixture");const auto set=f.root/"hdd0/home/00000001/trophy"/id;
    write(set/"TROPCONF.SFM",metadata);write(set/"TROPUSR.DAT",locked);sets.push_back(set);
  }
  Rpcs3TrophyWatch watch(25ms);watch.set_session(f.session);wait([&]{return watch.status().baseline_ready;},"bounded multi-set baseline failed");
  for(const auto& set:sets)write(set/"TROPUSR.DAT",unlocked);
  wait([&]{return watch.status().dropped==64;},"bounded event queue did not report overflow");
  require(watch.take_events().size()==128,"trophy event queue exceeded its hard bound");polls(watch,7);
  require(watch.take_events().empty(),"overflow replayed old trophy edges after draining");
  watch.set_session(std::nullopt);for(const auto& set:sets)write(set/"TROPUSR.DAT",locked);f.session.generation=7;watch.set_session(f.session);
  wait([&]{return watch.status().baseline_ready;},"whole-user replacement baseline failed");for(const auto& set:sets)write(set/"TROPUSR.DAT",unlocked);
  wait([&]{return watch.status().dropped==64;},"whole-user replacement edges were not queued");
  const auto user=f.root/"hdd0/home/00000001";fs::rename(user,f.root/"hdd0/home/replaced-user");
  for(const auto& set:sets){write(set/"TROPCONF.SFM",metadata);write(set/"TROPUSR.DAT",unlocked);}
  polls(watch,1);require(watch.take_events().empty(),"changed RPCS3 user source retained other sets' queued unlocks");
}
}
int main(){const auto root=fs::weakly_canonical(fs::temp_directory_path())/("openxmb-trophy-watch-fixtures-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try{edges(root/"edges");isolation(root/"isolation");source_replacement_and_cancel(root/"source");stable_retry(root/"stable");bounded_queue(root/"bounds");
    fs::remove_all(root);std::cout<<"Isolated RPCS3 trophy-watch fixture checks passed; not real unlock evidence\n";return 0;}
  catch(const std::exception& error){std::cerr<<error.what()<<'\n';fs::remove_all(root);return 1;}
}
