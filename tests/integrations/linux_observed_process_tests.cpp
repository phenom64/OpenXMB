// Isolated-Xvfb tests for the Linux native X11 provider observer. The fixture
// is this same binary copied into a fixture installation root and re-executed
// with --observed-fixture. Nothing in this file signals a process: fixtures end
// on a control file, on WM_DELETE_WINDOW or on their own 25 second timeout.
#include "openxmb/integrations/observed_process.hpp"
#include "openxmb/integrations/session_window.hpp"
#include "linux_observed_process.hpp"
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace openxmb::integrations;
using namespace std::chrono_literals;
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;

void require(bool okay,const std::string& message){if(!okay)throw std::runtime_error(message);}

// ---------------------------------------------------------------- fixture child
int fixture_main(const fs::path& control,const std::string& mode) {
  const auto touch=[&](const char* name,const std::string& text=""){std::ofstream(control/name)<<text;};
  const auto exists=[&](const char* name){std::error_code error;return fs::exists(control/name,error);};
  if(mode=="protected")prctl(PR_SET_DUMPABLE,0);
  Display* d{};Window window{};Atom protocols{},del{};
  if(mode!="nowindow") {
    d=XOpenDisplay(nullptr);if(!d)return 2;
    const auto root=DefaultRootWindow(d);
    protocols=XInternAtom(d,"WM_PROTOCOLS",False);del=XInternAtom(d,"WM_DELETE_WINDOW",False);
    window=XCreateSimpleWindow(d,root,10,10,64,48,0,0,0x336699);
    unsigned long pid=static_cast<unsigned long>(getpid());
    XChangeProperty(d,window,XInternAtom(d,"_NET_WM_PID",False),XA_CARDINAL,32,PropModeReplace,reinterpret_cast<unsigned char*>(&pid),1);
    if(mode!="plain")XChangeProperty(d,window,protocols,XA_ATOM,32,PropModeReplace,reinterpret_cast<unsigned char*>(&del),1);
    XSelectInput(d,window,StructureNotifyMask);XMapWindow(d,window);XFlush(d);
    bool mapped=false;const auto limit=Clock::now()+10s;
    while(!mapped) {
      if(Clock::now()>limit)return 3;
      if(!XPending(d)){std::this_thread::sleep_for(5ms);continue;}
      XEvent event{};XNextEvent(d,&event);if(event.type==MapNotify&&event.xmap.window==window)mapped=true;
    }
  }
  touch("ready",std::to_string(window));
  const auto deadline=Clock::now()+25s;
  while(Clock::now()<deadline&&!exists("stop")) {
    if(d) {
      if(window&&exists("drop")){XDestroyWindow(d,window);XSync(d,False);window=0;touch("dropped");}
      while(XPending(d)) {
        XEvent event{};XNextEvent(d,&event);
        if(event.type!=ClientMessage)continue;
        if(event.xclient.message_type==protocols&&static_cast<Atom>(event.xclient.data.l[0])==del){touch("closed");XCloseDisplay(d);return 0;}
        touch("unexpected");
      }
    }
    std::this_thread::sleep_for(10ms);
  }
  if(d)XCloseDisplay(d);
  return 0;
}

// ------------------------------------------------------------------- test world
struct Child {
  pid_t pid{};fs::path control;bool reaped{};
  [[nodiscard]] std::optional<unsigned long> ready() const {
    std::ifstream input(control/"ready");unsigned long value{};
    if(!(input>>value))return {};return value;
  }
  [[nodiscard]] bool marker(const char* name) const {std::error_code error;return fs::exists(control/name,error);}
  void command(const char* name) const {std::ofstream(control/name)<<"1";}
  bool alive() {
    if(reaped)return false;
    int status{};const auto result=waitpid(pid,&status,WNOHANG);
    if(result==pid){reaped=true;return false;}
    return result==0;
  }
  bool wait_exit(std::chrono::milliseconds limit) {
    const auto deadline=Clock::now()+limit;
    while(alive()){if(Clock::now()>deadline)return false;std::this_thread::sleep_for(10ms);}
    return true;
  }
};
struct Spawner {
  fs::path base;std::vector<std::unique_ptr<Child>> children;unsigned serial{};
  ~Spawner(){for(auto& child:children){child->command("stop");(void)child->wait_exit(5s);}}
  // Copies the fixture binary into an installation root as an executable image.
  static fs::path install(const fs::path& directory,const std::string& name) {
    fs::create_directories(directory);
    fs::copy_file(fs::canonical("/proc/self/exe"),directory/name,fs::copy_options::overwrite_existing);
    fs::permissions(directory/name,fs::perms::owner_all|fs::perms::group_read|fs::perms::group_exec|fs::perms::others_read|fs::perms::others_exec);
    return fs::canonical(directory/name);
  }
  Child& start(const fs::path& image,const std::string& mode="delete") {
    auto child=std::make_unique<Child>();
    child->control=base/("control-"+std::to_string(++serial));fs::create_directories(child->control);
    const auto control=child->control.string(),program=image.string();
    const pid_t pid=fork();require(pid>=0,"fork failed");
    if(!pid) {
      execl(program.c_str(),program.c_str(),"--observed-fixture",control.c_str(),mode.c_str(),static_cast<char*>(nullptr));
      _exit(127);
    }
    child->pid=pid;children.push_back(std::move(child));return *children.back();
  }
  Child& start_ready(const fs::path& image,const std::string& mode="delete") {
    auto& child=start(image,mode);const auto deadline=Clock::now()+10s;
    while(!child.ready()){require(Clock::now()<deadline&&child.alive(),"fixture child never became ready");std::this_thread::sleep_for(10ms);}
    return child;
  }
};
// A minimal window manager: maps clients, publishes the client list and counts
// (but never grants) activation requests so focus changes are observable.
struct Desktop {
  Display* d{};Display* foreign{};Window root{},shell{};std::atomic<bool> stop{};std::atomic<int> activations{};std::thread wm;
  Atom atom(const char* name){return XInternAtom(d,name,False);}
  explicit Desktop() {
    require(std::getenv("OPENXMB_X11_TEST_ISOLATED"),"Run only in an explicitly isolated Xvfb server");
    XInitThreads();d=XOpenDisplay(nullptr);foreign=XOpenDisplay(nullptr);require(d&&foreign,"Xvfb display missing");root=DefaultRootWindow(d);
    XSelectInput(d,root,SubstructureRedirectMask|SubstructureNotifyMask);
    shell=XCreateSimpleWindow(d,root,300,300,50,50,0,0,0);XMapWindow(d,shell);
    XSetInputFocus(d,shell,RevertToParent,CurrentTime);set_active(shell);publish({});XSync(d,False);
    wm=std::thread([this]{
      std::vector<Window> clients;
      const auto remove=[&](Window window){std::erase(clients,window);publish(clients);};
      while(!stop) {
        while(XPending(d)) {
          XEvent e{};XNextEvent(d,&e);
          if(e.type==MapRequest){XMapWindow(d,e.xmaprequest.window);clients.push_back(e.xmaprequest.window);publish(clients);}
          else if(e.type==UnmapNotify)remove(e.xunmap.window);
          else if(e.type==DestroyNotify)remove(e.xdestroywindow.window);
          else if(e.type==ClientMessage&&e.xclient.message_type==atom("_NET_ACTIVE_WINDOW"))++activations;
        }
        XFlush(d);std::this_thread::sleep_for(2ms);
      }
    });
  }
  ~Desktop(){stop=true;if(wm.joinable())wm.join();XCloseDisplay(foreign);XCloseDisplay(d);}
  void set_active(Window window){XChangeProperty(d,root,atom("_NET_ACTIVE_WINDOW"),XA_WINDOW,32,PropModeReplace,reinterpret_cast<unsigned char*>(&window),1);}
  void publish(const std::vector<Window>& clients) {
    for(const auto* name:{"_NET_CLIENT_LIST_STACKING","_NET_CLIENT_LIST"})
      XChangeProperty(d,root,atom(name),XA_WINDOW,32,PropModeReplace,reinterpret_cast<const unsigned char*>(clients.data()),static_cast<int>(clients.size()));
  }
  [[nodiscard]] std::pair<Window,Window> focus_state() {
    Window focus{};int revert{};XGetInputFocus(d,&focus,&revert);
    Atom type{};int format{};unsigned long count{},after{};unsigned char* data{};Window active{};
    if(XGetWindowProperty(d,root,atom("_NET_ACTIVE_WINDOW"),0,1,False,XA_WINDOW,&type,&format,&count,&after,&data)==Success&&data){if(count==1)active=*reinterpret_cast<Window*>(data);XFree(data);}
    return {focus,active};
  }
};

ExecutableMatch settle(ExecutableObservation& watch,std::stop_token stop={},std::chrono::milliseconds limit=10s) {
  const auto deadline=Clock::now()+limit;
  for(;;) {
    auto match=watch.poll(stop);if(match.state!=ExecutableMatchState::pending)return match;
    require(Clock::now()<deadline,"observation stayed pending");std::this_thread::sleep_for(25ms);
  }
}
void stays_pending(ExecutableObservation& watch,std::chrono::milliseconds duration,const char* message) {
  const auto deadline=Clock::now()+duration;
  do {
    const auto match=watch.poll();
    require(match.state==ExecutableMatchState::pending&&!match.process,std::string(message)+" ["+match.message+"]");
    std::this_thread::sleep_for(25ms);
  } while(Clock::now()<deadline);
}
template<class F>void expect_throw(F&& call,const std::string& message){
  try{call();}catch(const std::runtime_error&){return;}
  throw std::runtime_error(message);
}
bool contains(const std::string& text,std::string_view needle){return text.find(needle)!=text.npos;}
bool wait_for(auto&& condition,std::chrono::milliseconds limit=5s){
  const auto deadline=Clock::now()+limit;
  while(!condition()){if(Clock::now()>deadline)return false;std::this_thread::sleep_for(10ms);}
  return true;
}

// ---------------------------------------------------------------------- cases
void private_unit_checks() {
  namespace lo=linux_observer;
  require(lo::within("/g/Game","/g/Game/bin/x"),"A contained image must be within the root");
  require(!lo::within("/g/Game","/g/Game"),"The root itself is not an image inside the root");
  require(!lo::within("/g/Game","/g/Game2/x"),"A string-prefix sibling must be outside the root");
  require(!lo::within("/g/Game","/g/other/Game/x"),"A foreign path containing the name must be outside");
  for(const auto* helper:{"launcher","UnityCrashHandler64","crashpad_handler","vc_redist.x64","MyGameLauncher","setup","steamwebhelper","eac_launcher","EasyAntiCheat_EOS"})
    require(lo::helper_image("/g/Game",std::string("/g/Game/")+helper),std::string(helper)+" must be a helper image");
  require(lo::helper_image("/g/Game","/g/Game/_CommonRedist/dx/game"),"Redistributable folders hold helpers");
  require(!lo::helper_image("/g/Game","/g/Game/bin/CrashBandicoot.x86_64"),"A game merely named Crash... is not a crash helper");
  require(!lo::helper_image("/g/Game","/g/Game/game"),"The game is not a helper");
  require(!lo::helper_image("/g/redist/Game","/g/redist/Game/game"),"Folders above the root are not inspected");
  // Bounded, promptly cancellable enumeration.
  std::stop_source stopped;stopped.request_stop();
  require(lo::enumerate_processes(stopped.get_token(),Clock::now()+10s,[](std::int64_t){return true;})==lo::Enumeration::stopped,"A stop request must end enumeration");
  require(lo::enumerate_processes({},Clock::now()-1s,[](std::int64_t){return true;})==lo::Enumeration::limit,"The deadline must bound enumeration");
  require(lo::enumerate_processes({},Clock::now()+10s,[](std::int64_t){return true;},1)==lo::Enumeration::limit,"The entry limit must bound enumeration");
  std::size_t visited=0;
  require(lo::enumerate_processes({},Clock::now()+10s,[&](std::int64_t){++visited;return false;})==lo::Enumeration::complete&&visited==1,"A visitor can end enumeration early");
  require(lo::same_user(::getpid())&&lo::same_filesystem_view(::getpid()),"The current process must satisfy its own identity predicates");
  int error{};auto pin=lo::open_pidfd(::getpid(),error);require(pin&&lo::pidfd_state(pin.get())==lo::PidfdState::running,"pidfd_open must pin a live process");
}

void unsafe_paths(Spawner& spawn) {
  const auto base=spawn.base;
  fs::create_directories(base/"unsafe/library/steamapps");fs::create_directories(base/"unsafe/world");
  fs::permissions(base/"unsafe/world",fs::perms::all);
  std::error_code error;fs::create_directory_symlink("/usr",base/"unsafe/usr-link",error);require(!error,"symlink fixture");
  std::ofstream(base/"unsafe/data.txt")<<"data";
  const auto* home=std::getenv("HOME");require(home&&*home,"HOME required");
  const std::vector<fs::path> roots{"/","/usr/share",home,fs::path(home).parent_path(),base/"unsafe/library",base/"unsafe/world",base/"unsafe/usr-link",
    base/"unsafe/missing","relative/dir"};
  for(const auto& root:roots)expect_throw([&]{ExecutableObservation watch(root,true);},"Unsafe installation root accepted: "+root.string());
  expect_throw([&]{ExecutableObservation watch(base/"unsafe/library",false);},"A directory is not an executable image");
  expect_throw([&]{ExecutableObservation watch(base/"unsafe/data.txt",false);},"A non-executable file is not a game image");
}

void exact_lifecycle(Spawner& spawn,Desktop& desktop) {
  const auto image=Spawner::install(spawn.base/"exact/Game","game");
  { // A preexisting matching game, even with a window, is never adopted.
    auto& old=spawn.start_ready(image);
    ExecutableObservation watch(image);
    const auto match=watch.poll();
    require(match.state==ExecutableMatchState::rejected&&!match.process&&contains(match.message,"already running"),"A preexisting game must be rejected: "+match.message);
    require(watch.poll().state==ExecutableMatchState::rejected,"Rejection is terminal");
    old.command("stop");require(old.wait_exit(5s),"old fixture ended");
  }
  ExecutableObservation watch(image);
  stays_pending(watch,150ms,"Nothing is running yet");
  { // The forwarded launcher PID is never a candidate.
    ExecutableObservation excluded(image);
    auto& other=spawn.start_ready(image);excluded.exclude_process(other.pid);
    stays_pending(excluded,700ms,"An excluded launcher PID must not be adopted");
    other.command("stop");require(other.wait_exit(5s),"excluded fixture ended");
  }
  ExecutableObservation fresh(image);
  auto& game=spawn.start_ready(image);
  const auto before=desktop.focus_state();
  const auto match=settle(fresh);
  require(match.state==ExecutableMatchState::matched&&match.process,"A new unique game must associate: "+match.message);
  const auto process=match.process;
  require(process->id()==game.pid,"The associated PID must be the fixture child");
  require(process->creation_time()==linux_detail::process_creation(game.pid).value_or(0)&&process->creation_time(),"The capability must carry the process start tick");
  const auto running=process->observe();
  require(running.state==ProcessState::running&&running.process_creation==process->creation_time(),"A live observed game is running");
  const auto window=process->window();
  require(window&&window->pid==game.pid&&window->handle==*game.ready()&&window->process_creation==process->creation_time(),"The XRes-pinned window must be exposed");
  require(fresh.poll().process==process,"A second poll must return the same pinned capability");
  std::stop_source stopped;stopped.request_stop();
  require(fresh.poll(stopped.get_token()).process==process,"Cancellation applies only to discovery");
  // Close: the advertised WM_DELETE message only; no focus request or change.
  const auto closed=process->request_close();
  require(closed.success,"Close request failed: "+closed.message);
  require(wait_for([&]{return game.marker("closed");}),"The fixture never received WM_DELETE_WINDOW");
  require(wait_for([&]{return process->observe().state==ProcessState::exited;}),"A closed fixture must be observed as exited");
  const auto after=desktop.focus_state();
  require(before==after&&desktop.activations==0,"request_close must not move or request focus");
  const auto ended=process->observe();
  require(ended.state==ProcessState::exited&&!ended.exit_code&&ended.process_creation==process->creation_time(),"A non-child exit must not invent an exit code");
  require(game.wait_exit(5s),"fixture reaped");
  require(!process->window()&&!process->request_close().success,"An exited game has no window or close capability");
  // No rebinding: a later instance of the same image is never rediscovered.
  auto& later=spawn.start_ready(image);
  for(int i=0;i<12;++i){
    const auto again=fresh.poll();
    require(again.state==ExecutableMatchState::matched&&again.process==process&&again.process->id()==game.pid,"The pinned capability must not rebind");
    std::this_thread::sleep_for(25ms);
  }
  require(process->observe().state==ProcessState::exited,"An exited capability stays exited");
  later.command("stop");require(later.wait_exit(5s),"later fixture ended");
}

void exact_ambiguous(Spawner& spawn) {
  const auto image=Spawner::install(spawn.base/"ambiguous/Game","game");
  ExecutableObservation watch(image);
  spawn.start_ready(image);spawn.start_ready(image);
  const auto match=settle(watch);
  require(match.state==ExecutableMatchState::rejected&&!match.process&&contains(match.message,"ambiguous"),"Two matching games must be ambiguous: "+match.message);
}

void exact_without_window_or_close(Spawner& spawn,Desktop& desktop) {
  const auto image=Spawner::install(spawn.base/"nowindow/Game","game");
  { // A process without a validated XRes window has no display identity.
    ExecutableObservation watch(image);
    auto& hidden=spawn.start_ready(image,"nowindow");
    stays_pending(watch,700ms,"A windowless process must not associate");
    hidden.command("stop");require(hidden.wait_exit(5s),"windowless fixture ended");
  }
  { // A forged _NET_WM_PID on another client's window is not an identity.
    ExecutableObservation watch(image);
    auto& liar=spawn.start_ready(image,"nowindow");
    const auto window=XCreateSimpleWindow(desktop.foreign,DefaultRootWindow(desktop.foreign),70,70,40,40,0,0,0);
    unsigned long forged=static_cast<unsigned long>(liar.pid);
    XChangeProperty(desktop.foreign,window,XInternAtom(desktop.foreign,"_NET_WM_PID",False),XA_CARDINAL,32,PropModeReplace,reinterpret_cast<unsigned char*>(&forged),1);
    XMapWindow(desktop.foreign,window);XSync(desktop.foreign,False);
    stays_pending(watch,700ms,"A forged window PID must not associate");
    XDestroyWindow(desktop.foreign,window);XSync(desktop.foreign,False);
    liar.command("stop");require(liar.wait_exit(5s),"forging fixture ended");
  }
  { // No advertised WM_DELETE_WINDOW: explicit failure and no message at all.
    ExecutableObservation watch(image);
    auto& plain=spawn.start_ready(image,"plain");
    const auto match=settle(watch);require(match.state==ExecutableMatchState::matched,"plain window associates: "+match.message);
    const auto closed=match.process->request_close();
    require(!closed.success&&contains(closed.message,"WM_DELETE_WINDOW"),"An unadvertised close protocol must fail explicitly: "+closed.message);
    std::this_thread::sleep_for(150ms);
    require(!plain.marker("unexpected")&&match.process->observe().state==ProcessState::running,"No close message may be sent without the advertised protocol");
    plain.command("stop");require(plain.wait_exit(5s),"plain fixture ended");
  }
  { // Stale target: the pinned window is gone and a foreign client forges its PID.
    ExecutableObservation watch(image);
    auto& game=spawn.start_ready(image,"dropwindow");
    const auto match=settle(watch);require(match.state==ExecutableMatchState::matched,"dropwindow associates: "+match.message);
    auto* d=desktop.foreign;
    const auto window=XCreateSimpleWindow(d,DefaultRootWindow(d),90,90,40,40,0,0,0);
    unsigned long forged=static_cast<unsigned long>(game.pid);Atom del=XInternAtom(d,"WM_DELETE_WINDOW",False);
    XChangeProperty(d,window,XInternAtom(d,"_NET_WM_PID",False),XA_CARDINAL,32,PropModeReplace,reinterpret_cast<unsigned char*>(&forged),1);
    XChangeProperty(d,window,XInternAtom(d,"WM_PROTOCOLS",False),XA_ATOM,32,PropModeReplace,reinterpret_cast<unsigned char*>(&del),1);
    XMapWindow(d,window);XSync(d,False);
    require(match.process->window().has_value(),"The genuine window is still unique before it is dropped");
    game.command("drop");require(wait_for([&]{return game.marker("dropped");}),"fixture dropped its window");
    require(wait_for([&]{return !match.process->window();}),"A dropped window must not be rediscovered through a forged PID");
    const auto closed=match.process->request_close();
    require(!closed.success,"A stale or foreign target must not be closed");
    XSync(d,False);int messages=0;
    while(XPending(d)){XEvent e{};XNextEvent(d,&e);if(e.type==ClientMessage)++messages;}
    require(!messages&&!game.marker("unexpected")&&!game.marker("closed"),"No close message may reach a foreign or stale target");
    XDestroyWindow(d,window);XSync(d,False);
    game.command("stop");require(game.wait_exit(5s),"dropwindow fixture ended");
    require(wait_for([&]{return match.process->observe().state==ProcessState::exited;}),"dropwindow exit observed");
  }
}

void protected_game(Spawner& spawn) {
  if(geteuid()==0){std::cout<<"SKIP protected-process case: root can read nondumpable images\n";return;}
  const auto image=Spawner::install(spawn.base/"protected/Game","game");
  auto& hidden=spawn.start_ready(image,"protected");
  int error{};require(!linux_observer::link_target("/proc/"+std::to_string(hidden.pid)+"/exe",error)&&error==EACCES,"Precondition: nondumpable image is hidden");
  ExecutableObservation watch(image);
  const auto match=watch.poll();
  require(match.state==ExecutableMatchState::unavailable&&contains(match.message,"protected"),"A protected same-name game must be explicitly unsupported: "+match.message);
  hidden.command("stop");require(hidden.wait_exit(5s),"protected fixture ended");
}

void directory_mode(Spawner& spawn) {
  const auto root=spawn.base/"dir/Game";
  const auto game=Spawner::install(root,"game");
  const auto crash=Spawner::install(root,"UnityCrashHandler64");
  const auto sibling=Spawner::install(spawn.base/"dir/Game2","game");
  const auto outside=Spawner::install(spawn.base/"dir/Outside","game-outside");
  fs::create_symlink(outside,root/"linked");
  { // An old game inside the root is never adopted.
    auto& old=spawn.start_ready(game);
    ExecutableObservation watch(root,true);
    const auto match=watch.poll();
    require(match.state==ExecutableMatchState::rejected&&contains(match.message,"already running"),"A preexisting directory game must be rejected: "+match.message);
    old.command("stop");require(old.wait_exit(5s),"old directory fixture ended");
  }
  ExecutableObservation watch(root,true);
  auto& helper=spawn.start_ready(crash);
  auto& neighbour=spawn.start_ready(sibling);
  auto& escaped=spawn.start_ready(root/"linked"); // a symlink in the root that resolves outside it
  stays_pending(watch,800ms,"Helpers, prefix siblings and symlink escapes must not associate");
  auto& real=spawn.start_ready(game);
  const auto match=settle(watch);
  require(match.state==ExecutableMatchState::matched&&match.process&&match.process->id()==real.pid,"The contained non-helper game must associate: "+match.message);
  require(match.process->window()&&match.process->window()->pid==real.pid,"Directory mode requires the XRes-pinned window");
  require(helper.alive()&&neighbour.alive()&&escaped.alive(),"Excluded fixtures were not touched");
  require(match.process->request_close().success&&real.wait_exit(5s)&&real.marker("closed"),"Close reaches only the validated game window");
  require(!helper.marker("closed")&&!neighbour.marker("closed")&&!escaped.marker("closed"),"No close for unrelated windows");
  for(auto* other:{&helper,&neighbour,&escaped}){other->command("stop");require(other->wait_exit(5s),"directory neighbour ended");}
  { // Two valid new games are ambiguous.
    const auto second=Spawner::install(root,"game-b");
    ExecutableObservation ambiguous(root,true);
    spawn.start_ready(game);spawn.start_ready(second);
    const auto result=settle(ambiguous);
    require(result.state==ExecutableMatchState::rejected&&contains(result.message,"ambiguous"),"Two directory games are ambiguous: "+result.message);
  }
}

void cancellation(Spawner& spawn) {
  const auto image=Spawner::install(spawn.base/"cancel/Game","game");
  { // A pre-requested stop is prompt and terminal.
    ExecutableObservation watch(image);
    std::stop_source source;source.request_stop();
    const auto started=Clock::now();const auto match=watch.poll(source.get_token());
    require(match.state==ExecutableMatchState::unavailable&&contains(match.message,"stopped")&&Clock::now()-started<200ms,"A stopped poll must return promptly: "+match.message);
    require(watch.poll().state==ExecutableMatchState::unavailable,"Cancelled discovery stays unavailable");
  }
  { // A stop issued while a worker is polling.
    ExecutableObservation watch(image);
    std::stop_source source;std::atomic<int> result{-1};std::atomic<bool> finished{};
    std::thread worker([&]{for(;;){const auto match=watch.poll(source.get_token());if(match.state!=ExecutableMatchState::pending){result=static_cast<int>(match.state);break;}std::this_thread::sleep_for(5ms);}finished=true;});
    std::this_thread::sleep_for(100ms);const auto asked=Clock::now();source.request_stop();
    const bool prompt=wait_for([&]{return finished.load();},1s);worker.join();
    require(prompt&&Clock::now()-asked<1s&&result==static_cast<int>(ExecutableMatchState::unavailable),"A running discovery must stop promptly");
  }
}

void wayland_unsupported(Spawner& spawn) {
  const auto image=Spawner::install(spawn.base/"wayland/Game","game");
  const auto* old_type=std::getenv("XDG_SESSION_TYPE");const std::string saved=old_type?old_type:"";
  setenv("XDG_SESSION_TYPE","wayland",1);
  { ExecutableObservation watch(image);const auto match=watch.poll();
    require(match.state==ExecutableMatchState::unavailable&&contains(match.message,"Wayland"),"Wayland must be explicitly unsupported: "+match.message); }
  if(old_type)setenv("XDG_SESSION_TYPE",saved.c_str(),1);else unsetenv("XDG_SESSION_TYPE");
  setenv("WAYLAND_DISPLAY","wayland-fixture",1);
  { ExecutableObservation watch(image);require(watch.poll().state==ExecutableMatchState::unavailable,"WAYLAND_DISPLAY must be unsupported"); }
  unsetenv("WAYLAND_DISPLAY");
}

int main(int argc,char** argv)try {
  if(argc==4&&std::string(argv[1])=="--observed-fixture")return fixture_main(argv[2],argv[3]);
  const auto* out=std::getenv("OPENXMB_X11_TEST_OUTPUT");const auto* home=std::getenv("HOME");
  require(home&&*home,"HOME required");
  const fs::path parent=out&&*out?fs::path(out):fs::path(home)/".cache/openxmb-x11-tools";fs::create_directories(parent);
  std::string pattern=(parent/"observed-XXXXXX").string();require(mkdtemp(pattern.data()),"Cannot create fixture directory");
  struct Cleanup{fs::path path;~Cleanup(){std::error_code error;fs::remove_all(path,error);}} cleanup{pattern};
  {
    Desktop desktop;Spawner spawn;spawn.base=fs::canonical(pattern);
    const std::vector<std::pair<const char*,std::function<void()>>> cases{
      {"private identity predicates",[&]{private_unit_checks();}},
      {"unsafe paths",[&]{unsafe_paths(spawn);}},
      {"exact lifecycle, pinning, close and no rebinding",[&]{exact_lifecycle(spawn,desktop);}},
      {"exact ambiguity",[&]{exact_ambiguous(spawn);}},
      {"windowless, forged, unadvertised and stale targets",[&]{exact_without_window_or_close(spawn,desktop);}},
      {"protected process",[&]{protected_game(spawn);}},
      {"directory mode",[&]{directory_mode(spawn);}},
      {"cancellation",[&]{cancellation(spawn);}},
      {"wayland fallback",[&]{wayland_unsupported(spawn);}}};
    for(const auto& [name,run]:cases){run();std::cout<<"PASS "<<name<<'\n';}
  }
  std::cout<<"PASS Linux native X11 observed-process identity\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
