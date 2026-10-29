#include "openxmb/desktop/session.hpp"
#include "openxmb/integrations/observed_process.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace openxmb::desktop;
using namespace openxmb::integrations;
using namespace std::chrono_literals;
namespace {
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
std::string text(const fs::path& path) { const auto value = path.u8string(); return {value.begin(), value.end()}; }
template<class Predicate> void wait(Predicate predicate, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!predicate()) { if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message); std::this_thread::sleep_for(5ms); }
}
SessionDescriptor descriptor(const fs::path& profile, std::string id = "fixture-game") {
  return {std::move(id), "Fixture title", "native", "fixture", profile, SessionKind::game, TrackingQuality::direct_child};
}
void process_checks(const fs::path& root, const fs::path& helper) {
  require(!launch_owned_process({text(root / "does-not-exist")}), "failed exec must be reported promptly");
  require(!launch_owned_process({text(helper), "record", "0", "0"}, root / "missing-cwd"), "failed chdir must be reported before launch succeeds");
  require(!launch_owned_process({text(helper), std::string("bad\0argument", 12)}), "NUL argument must be rejected");
  fs::create_directories(root);
  const auto process = launch_owned_process({text(helper), "wait", "ready", "release"}, root);
  require(bool(process), "native owned process did not launch");
  wait([&] { return fs::exists(root / "ready"); }, "native process never became ready");
  require(process.process->observe().state == ProcessState::running, "live process was reported exited");
#ifdef _WIN32
  require(process.process->observe().process_creation!=0,"owned process observation must expose identity from its retained handle");
#endif
  require(!stop_process(process.process->id()), "a bare PID must not adopt or stop a tracked session capability");
  require(process.process->observe().state == ProcessState::running, "unowned PID stop request affected child");
  std::ofstream(root / "release") << "release";
  wait([&] { return process.process->observe().state != ProcessState::running; }, "child exit was not observed");
  for (unsigned i = 0; i < 10; ++i) require(process.process->observe().exit_code == 0, "owned exit status must stay cached across repeated observations");
  const auto legacy = launch_process({text(helper), "record", "17", "0"}, root);
  require(bool(legacy), "legacy launcher failed");
  wait([&] { return process_exit_status(legacy.process_id).has_value(); }, "legacy exit never observed");
  for (unsigned i = 0; i < 10; ++i) require(process_exit_status(legacy.process_id) == 17, "legacy repeated polling lost actual exit code or double-reaped");
  require(!stop_process(legacy.process_id), "finished legacy process must not be signalled");
  const auto special = launch_owned_process({text(helper), "record", "259", "0"}, root);
  require(bool(special), "special exit fixture failed to launch");
  wait([&] { return special.process->observe().state != ProcessState::running; }, "special exit code was confused with a live process");
#ifdef _WIN32
  require(special.process->observe().exit_code == 259, "Windows STILL_ACTIVE-valued exit code was lost");
#else
  require(special.process->observe().exit_code == 3, "POSIX exit status did not preserve native eight-bit semantics");
#endif
}
void manager_checks(const fs::path& root, const fs::path& helper) {
  SessionManager manager;
  const auto profile = root / fs::path(std::u8string(u8"profile \u6771\u4eac"));
  const auto cwd = root / fs::path(std::u8string(u8"working \u6771\u4eac")); fs::create_directories(cwd);
  const auto failed = manager.launch(descriptor(profile), {text(root / "absent")}, cwd);
  require(!failed && manager.snapshot().empty() && manager.history(profile).launch_count == 0, "failed launch created a session/history count");
  const std::vector<std::string> argv{text(helper), "record", "9", "180", "", "spaces and 'quotes'", "ends\\", "a\\\"b", "$(touch injected); &", std::string(reinterpret_cast<const char*>(u8"\u6771\u4eac \U0001f3ae"))};
  const auto launched = manager.launch(descriptor(profile), argv, cwd);
  require(bool(launched) && manager.active_session().has_value(), "session launch did not expose an active snapshot");
  require(!manager.quit("not-a-session"), "unknown session quit was accepted");
  // history() only reads the journal. No UI tick/snapshot call drives exits.
  wait([&] { const auto h = manager.history(profile); return h.error.empty() && !h.recent.empty() && h.recent.front().state == SessionState::exited; }, "independent observer did not finalize child while UI was idle");
  auto history = manager.history(profile, "fixture-game");
  require(history.launch_count == 1 && history.recent.size() == 1 && history.recent.front().exit_code == 9, "session completion did not preserve launch count/exit status");
  require(history.total_playtime_ms >= 100 && history.recent.front().duration_ms >= 100, "steady-clock direct-child playtime was not measured");
  require(history.recent.front().started_at > 0 && history.recent.front().ended_at >= history.recent.front().started_at, "wall timestamps are invalid");
  require(manager.history(root / "other-profile").launch_count == 0, "history leaked into another originating profile");
  nlohmann::json recorded; { std::ifstream input(cwd / "arguments.json"); input >> recorded; }
  require(recorded.at("arguments") == argv && recorded.at("cwd") == text(fs::weakly_canonical(cwd)), "native literal argv/Unicode/cwd changed");
  require(!fs::exists(cwd / "injected"), "argument text was interpreted by a shell");
  const auto expected_duration = history.total_playtime_ms;
  for (unsigned i = 0; i < 20; ++i) { (void)manager.snapshot(); history = manager.history(profile); }
  require(history.launch_count == 1 && history.total_playtime_ms == expected_duration, "completion was persisted more than once");
  auto launcher = descriptor(profile, "forwarder"); launcher.kind = SessionKind::application; launcher.tracking = TrackingQuality::launcher_only;
  const auto forwarding = manager.launch(launcher, {text(helper), "record", "0", "20"}, cwd);
  require(bool(forwarding), "launcher-only session did not start");
  wait([&] { const auto h = manager.history(profile, "forwarder"); return !h.recent.empty() && h.recent.front().state == SessionState::exited; }, "launcher-only session did not finish");
  const auto forwarding_history = manager.history(profile, "forwarder");
  require(forwarding_history.total_playtime_ms == 0 && !forwarding_history.recent.front().tracking_message.empty(), "forwarding launcher lifetime was misrepresented as application playtime");
  require(bool(manager.shutdown()), "clean manager shutdown failed");
  SessionManager reopened;
  require(reopened.history(profile, "fixture-game").total_playtime_ms == expected_duration, "durable history did not survive manager recreation");
  // Append-only tracking enum migration: observed-provider history is separate
  // from playtime of children OpenXMB actually owns.
  const auto journal=profile/"sessions/history.json";
  nlohmann::json provider_history;{std::ifstream input(journal);input>>provider_history;}
  provider_history["recent"][0]["tracking"]=3;
  provider_history["totals"][0]["provider_observed_playtime_ms"]=321;
  {std::ofstream output(journal);output<<provider_history;}
  require(reopened.history(profile).error.empty(),"provider-observed tracking value 3 remains readable beside existing session enum values");
  require(!fs::exists(profile / "library.json"), "session backend wrote library.json");
}
void journal_checks(const fs::path& root, const fs::path& helper) {
  const auto profile = root / "profile"; const auto cwd = root / "cwd"; fs::create_directories(cwd);
  SessionManager manager;
  const auto launched = manager.launch(descriptor(profile), {text(helper), "wait", "ready", "release"}, cwd);
  require(bool(launched), "journal retry fixture did not launch");
  wait([&] { return fs::exists(cwd / "ready"); }, "journal retry fixture not ready");
  const auto journal = profile / "sessions/history.json";
  nlohmann::json original; { std::ifstream input(journal); input >> original; }
  fs::remove(journal); fs::create_directory(journal);
  std::ofstream(cwd / "release") << "release";
  wait([&] { const auto snapshots = manager.snapshot(); return !snapshots.empty() && snapshots.front().state == SessionState::exited && !snapshots.front().persistence_error.empty(); }, "background persistence failure was silently hidden");
  fs::remove(journal); { std::ofstream output(journal); output << original; }
  wait([&] { const auto h = manager.history(profile); return h.error.empty() && !h.recent.empty() && h.recent.front().state == SessionState::exited; }, "failed completion journal update did not retry");
  require(manager.history(profile).launch_count == 1, "completion persistence retry counted the launch twice");
  nlohmann::json completed; { std::ifstream input(journal); input >> completed; }
  auto stale = original; stale["recent"][0]["owner"] = "previous-process-instance";
  { std::ofstream output(journal); output << stale; }
  require(manager.history(profile).recent.front().state == SessionState::interrupted, "stale running record was represented as a live/adopted process");
  const auto next = manager.launch(descriptor(profile, "next"), {text(helper), "record", "0", "0"}, cwd);
  require(bool(next), "new session could not normalize old interrupted records");
  nlohmann::json normalized; { std::ifstream input(journal); input >> normalized; }
  require(normalized["recent"][0]["state"] == static_cast<int>(SessionState::interrupted), "stale running records must be durably normalized so they remain pruneable");
  wait([&] { return !manager.active_session(); }, "normalization fixture did not exit");
  const auto invalid_profile = root / "invalid-profile"; fs::create_directories(invalid_profile / "sessions");
  const auto invalid_journal = invalid_profile / "sessions/history.json";
  auto corrupt = completed; corrupt["totals"][0]["launch_count"] = -1;
  { std::ofstream output(invalid_journal); output << corrupt; }
  require(!manager.history(invalid_profile).error.empty(), "negative journal counters were accepted as unsigned history");
  corrupt = completed; corrupt["recent"].push_back(corrupt["recent"][0]);
  { std::ofstream output(invalid_journal); output << corrupt; }
  require(!manager.history(invalid_profile).error.empty(), "duplicate durable session identities were accepted");
  { std::ofstream output(invalid_journal, std::ios::binary); output.seekp(4 * 1024 * 1024); output.put('x'); }
  require(!manager.history(invalid_profile).error.empty() && !manager.launch(descriptor(invalid_profile), {text(helper), "record", "0", "0"}, cwd), "oversized journal was accepted or silently overwritten before launch");
}
void quit_and_shutdown(const fs::path& root, const fs::path& helper) {
  const auto profile = root / "profile"; const auto cwd = root / "cwd"; fs::create_directories(cwd);
  SessionManager manager;
  const auto session = manager.launch(descriptor(profile), {text(helper), "wait", "ready", "release"}, cwd);
  require(bool(session), "quit fixture did not start");
  wait([&] { return fs::exists(cwd / "ready"); }, "quit fixture not ready");
#ifdef _WIN32
  require(!manager.quit(session.session_id, ProcessStopMode::request_close), "console child without a window must not be silently forcibly terminated");
  require(manager.active_session().has_value(), "graceful request silently killed console child");
#endif
  require(bool(manager.quit(session.session_id, ProcessStopMode::force_stop)), "explicit owned-process stop failed");
  wait([&] { return !manager.active_session(); }, "owned process stop was not observed");
  require(!manager.quit(session.session_id, ProcessStopMode::force_stop), "ended session accepted another stop");
  fs::remove(cwd / "ready");
  const auto detached = manager.launch(descriptor(profile, "shutdown-survivor"), {text(helper), "wait", "ready", "release"}, cwd);
  require(bool(detached), "shutdown fixture failed to launch");
  wait([&] { return fs::exists(cwd / "ready"); }, "shutdown fixture not ready");
#ifdef _WIN32
  const auto detached_pid = manager.active_session()->process_id;
#endif
  const auto started = std::chrono::steady_clock::now();
  require(bool(manager.shutdown()), "shutdown could not persist detached observation");
  require(std::chrono::steady_clock::now() - started < 1s, "manager shutdown waited for running child");
  std::ofstream(cwd / "release") << "release";
  wait([&] { return fs::exists(cwd / "completed"); }, "manager shutdown killed the user's running process");
#ifdef _WIN32
  // The completion marker proves survival; process exit additionally releases
  // Windows' current-directory handle before the fixture tree is removed.
  wait([&] { return process_exit_status(detached_pid).has_value(); }, "shutdown survivor did not finish releasing its working directory");
#endif
  SessionManager reader;
  const auto history = reader.history(profile, "shutdown-survivor");
  require(history.recent.size() == 1 && history.recent.front().state == SessionState::detached && history.recent.front().ended_at == 0 && history.total_playtime_ms == 0, "shutdown claimed an unobserved game exit or duration");
  const auto blocked = root / "blocked"; fs::create_directories(blocked); std::ofstream(blocked / "sessions") << "blocked directory";
  require(!reader.launch(descriptor(blocked), {text(helper), "record", "0", "0"}, cwd), "unwritable session journal launched an unjournalled child");
}
#ifdef _WIN32
struct FixtureWindow {bool close_requested{};};
LRESULT CALLBACK fixture_window(HWND hwnd,UINT message,WPARAM parameter,LPARAM value) {
  if(message==WM_NCCREATE)SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(value)->lpCreateParams));
  if(message==WM_CLOSE) {auto* state=reinterpret_cast<FixtureWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));if(state)state->close_requested=true;return 0;}
  return DefWindowProcW(hwnd,message,parameter,value);
}
int observation_fixture(const std::string& mode) {
  FixtureWindow state;HWND hwnd{};
  if(mode=="window") {
    WNDCLASSW cls{};cls.lpfnWndProc=fixture_window;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"OpenXMB.ObservedFixture";
    if(!RegisterClassW(&cls))return 81;
    hwnd=CreateWindowExW(WS_EX_NOACTIVATE,cls.lpszClassName,L"Observed game fixture",WS_POPUP|WS_VISIBLE,-32000,-32000,120,80,nullptr,nullptr,cls.hInstance,&state);
    if(!hwnd)return 82;
  }
  if(mode=="inaccessible") {
    SECURITY_DESCRIPTOR descriptor{};alignas(ACL) unsigned char bytes[64]{};auto* acl=reinterpret_cast<ACL*>(bytes);
    if(!InitializeSecurityDescriptor(&descriptor,SECURITY_DESCRIPTOR_REVISION)||!InitializeAcl(acl,sizeof(bytes),ACL_REVISION)||
       !SetSecurityDescriptorDacl(&descriptor,TRUE,acl,FALSE)||!SetKernelObjectSecurity(GetCurrentProcess(),DACL_SECURITY_INFORMATION,&descriptor))return 83;
  }
  std::ofstream("ready")<<GetCurrentProcessId();
  const auto deadline=std::chrono::steady_clock::now()+15s;
  while(!fs::exists("release")&&!state.close_requested&&std::chrono::steady_clock::now()<deadline) {
    MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
    std::this_thread::sleep_for(5ms);
  }
  if(state.close_requested)std::ofstream("closed")<<"closed";
  if(hwnd)DestroyWindow(hwnd);
  return 0;
}
fs::path fixture_image(const fs::path& root,const fs::path& self,const char* name) {
  fs::create_directories(root);const auto image=root/name;fs::copy_file(self,image);return fs::canonical(image);
}
OwnedProcessLaunch fixture_start(const fs::path& image,const fs::path& folder,const char* mode="window") {
  fs::create_directories(folder);
  auto process=launch_owned_process({text(image),"--observation-fixture",mode},folder);
  require(bool(process),"observed fixture did not start");
  wait([&]{return fs::exists(folder/"ready");},"observed fixture did not become ready");return process;
}
void fixture_release(const OwnedProcessLaunch& process,const fs::path& folder) {
  std::ofstream(folder/"release")<<"release";
  wait([&]{return process.process->observe().state!=ProcessState::running;},"observed fixture did not exit");
}
SessionDescriptor steam_descriptor(const fs::path& profile,const fs::path& image,std::uint32_t timeout=4000) {
  auto value=descriptor(profile,"observed-steam");value.runner="steam";value.provider="Steam";value.tracking=TrackingQuality::launcher_only;
  value.observation=ProviderObservationSpec{image,timeout};return value;
}
void observed_process_checks(const fs::path& root,const fs::path& self) {
  const auto image=fixture_image(root,self,"observed-identity-game.exe");
  const auto before=GetForegroundWindow();
  ExecutableObservation watch(image);
  const auto process=fixture_start(image,root/"success");
  ExecutableMatch match;
  wait([&]{match=watch.poll();if(match.state==ExecutableMatchState::rejected||match.state==ExecutableMatchState::unavailable)throw std::runtime_error(match.message);return match.state==ExecutableMatchState::matched;},"new exact executable was not observed");
  require(match.process&&match.process->id()==process.process->id()&&match.process->creation_time()!=0,"observed identity did not pin the correct PID and creation time");
  const auto window=match.process->window();require(window.has_value(),"unique observed fixture window was not available");
  const auto later=fixture_start(image,root/"later-instance");
  const auto retained=watch.poll();
  require(retained.state==ExecutableMatchState::matched&&retained.process==match.process,
    "once associated, later matching processes must not replace or invalidate the pinned game capability");
  fixture_release(later,root/"later-instance");
  auto stale=*window;++stale.process_creation;
  require(!validate_session_window(stale),"a changed process creation token was accepted");
  require(GetForegroundWindow()==before,"observed fixture or observation unexpectedly stole focus");
  require(bool(match.process->request_close()),"observed fixture did not accept graceful close");
  wait([&]{return match.process->observe().state==ProcessState::exited;},"read-only observation missed fixture exit");
  for(unsigned i=0;i<10;++i)require(match.process->observe().exit_code==0,"observed exit code was not cached");
  require(!match.process->window()&&!match.process->request_close(),"ended process retained a window or close capability");
  wait([&]{return process.process->observe().state==ProcessState::exited;},"fixture owner did not observe exit");
  const auto existing=fixture_start(image,root/"preexisting");
  ExecutableObservation baseline(image);
  require(baseline.poll().state==ExecutableMatchState::rejected,"preexisting matching process was adopted");
  fixture_release(existing,root/"preexisting");
  ExecutableObservation ambiguous(image);
  const auto first=fixture_start(image,root/"first"),second=fixture_start(image,root/"second");
  require(ambiguous.poll().state==ExecutableMatchState::rejected,"two new matching processes were silently disambiguated");
  fixture_release(first,root/"first");fixture_release(second,root/"second");
  ExecutableObservation inaccessible(image);
  const auto protected_process=fixture_start(image,root/"inaccessible","inaccessible");
  require(inaccessible.poll().state==ExecutableMatchState::unavailable,"inaccessible matching process was accepted or ignored as absent");
  fixture_release(protected_process,root/"inaccessible");
  ExecutableObservation canceled(image);std::stop_source stop;stop.request_stop();
  const auto cancel_started=std::chrono::steady_clock::now();
  require(canceled.poll(stop.get_token()).state==ExecutableMatchState::unavailable&&std::chrono::steady_clock::now()-cancel_started<250ms,
    "canceled process discovery did not stop promptly before enumerating candidates");
}
void observed_manager_checks(const fs::path& root,const fs::path& helper,const fs::path& self) {
  const auto image=fixture_image(root,self,"observed-session-game.exe");const auto cwd=root/"cwd";fs::create_directories(cwd);
  SessionManager manager;const auto profile=root/"profile";
  const auto launched=manager.launch(steam_descriptor(profile,image,1200),{text(helper),"record","0","0"},cwd);
  require(bool(launched),"provider observation launch failed");
  std::this_thread::sleep_for(150ms);
  auto pending=manager.active_session();
  require(pending&&pending->awaiting_observation&&pending->duration_ms==0&&pending->started_at==0&&pending->process_id==-1,
    "forwarder exit ended pending observation or credited launcher lifetime");
  require(!manager.quit(launched.session_id,ProcessStopMode::force_stop),"pending provider session granted force-stop capability");
  const auto process=fixture_start(image,root/"game");
  wait([&]{const auto value=manager.active_session();return value&&value->descriptor.tracking==TrackingQuality::provider_observed&&value->capabilities.graceful_close;},"session did not associate the new exact game process");
  const auto active=*manager.active_session();
  require(active.process_id==process.process->id()&&!active.awaiting_observation&&active.capabilities.focus&&active.capabilities.capture&&active.window,
    "observed session capabilities did not correspond to its unique current window");
  require(!manager.quit(launched.session_id,ProcessStopMode::force_stop)&&process.process->observe().state==ProcessState::running,
    "provider-observed process was force terminated");
  std::this_thread::sleep_for(1300ms);
  require(bool(manager.quit(launched.session_id)),"provider-observed graceful close failed");
  wait([&]{const auto history=manager.history(profile);return !history.recent.empty()&&history.recent.front().state==SessionState::exited;},"observed session was not finalized independently of the UI");
  const auto history=manager.history(profile);
  require(history.launch_count==1&&history.total_playtime_ms==0&&history.provider_observed_playtime_ms>=100,
    "observed-provider accounting was conflated with owned-child playtime");
  require(history.recent.front().state==SessionState::exited&&history.recent.front().duration_ms>=1200&&history.recent.front().exit_code==0,
    "a matched game exiting after the discovery timeout lost its observed lifetime or terminal exit");
  for(unsigned i=0;i<20;++i)require(manager.history(profile).provider_observed_playtime_ms==history.provider_observed_playtime_ms,"provider-observed exit was counted more than once");
  wait([&]{return process.process->observe().state==ProcessState::exited;},"closed observed fixture did not exit");
  require(fs::exists(root/"game/closed"),"graceful request did not pass through the fixture WM_CLOSE handler");
  const auto absent_image=fixture_image(root,self,"never-started-game.exe");
  const auto timeout=manager.launch(steam_descriptor(profile,absent_image,400),{text(helper),"record","0","0"},cwd);
  require(bool(timeout),"timeout fixture failed to launch");
  wait([&]{return !manager.active_session();},"pending game observation did not time out");
  const auto timed=manager.history(profile).recent.front();
  require(timed.state==SessionState::interrupted&&timed.duration_ms==0&&timed.ended_at==0&&!timed.capabilities.focus,"timeout fabricated a game exit or capability");
  const auto survivor=manager.launch(steam_descriptor(profile,image),{text(helper),"record","0","0"},cwd);
  require(bool(survivor),"observed shutdown fixture failed to launch");
  const auto running=fixture_start(image,root/"survivor");
  wait([&]{const auto value=manager.active_session();return value&&value->descriptor.tracking==TrackingQuality::provider_observed;},"shutdown fixture was not associated");
  const auto before=std::chrono::steady_clock::now();require(bool(manager.shutdown()),"observed shutdown failed");
  require(std::chrono::steady_clock::now()-before<1s&&running.process->observe().state==ProcessState::running,"observed shutdown waited for or killed the game");
  fixture_release(running,root/"survivor");
  require(manager.history(profile).recent.front().state==SessionState::detached&&manager.history(profile).provider_observed_playtime_ms==history.provider_observed_playtime_ms,
    "detached observed game was credited with an unobserved exit");
  SessionManager direct;
  const auto direct_folder=root/"direct-window";fs::create_directories(direct_folder);
  auto direct_descriptor=descriptor(root/"direct-profile");direct_descriptor.kind=SessionKind::application;
  const auto own=direct.launch(direct_descriptor,{text(image),"--observation-fixture","window"},direct_folder);
  require(bool(own),"direct window identity fixture did not launch");
  wait([&]{const auto active=direct.active_session();return active&&active->window&&active->capabilities.focus;},"owned direct window did not gain validated capabilities");
  const auto own_snapshot=*direct.active_session();
  require(own_snapshot.window->process_creation!=0&&own_snapshot.window->pid==own_snapshot.process_id,"direct window capability has no pinned process identity");
  auto stale=*own_snapshot.window;++stale.process_creation;
  require(!direct.confirm_application_window(own.session_id,stale),"direct application confirmation accepted a mismatched creation identity");
  require(bool(direct.quit(own.session_id)),"owned window graceful close failed after capability validation");
  wait([&]{return !direct.active_session();},"owned window close was not finalized");
}
void pending_shutdown_checks(const fs::path& root,const fs::path& self) {
  const auto never_started=fixture_image(root,self,"pending-never-started-game.exe");
  const auto forwarder=fixture_image(root,self,"pending-owned-forwarder.exe");
  const auto profile=root/"profile";SessionManager manager;
  struct Child {fs::path folder;HANDLE handle;};std::vector<Child> children;
  for(unsigned index=0;index<16;++index) {
    const auto folder=root/("forwarder-"+std::to_string(index));fs::create_directories(folder);
    auto desc=steam_descriptor(profile,never_started,60000);desc.identity="pending-"+std::to_string(index);
    require(bool(manager.launch(desc,{text(forwarder),"--observation-fixture","wait"},folder)),"pending shutdown forwarder failed to launch");
    DWORD pid{};
    wait([&]{std::ifstream input(folder/"ready");return bool(input>>pid)&&pid!=0;},"pending shutdown forwarder did not publish its ready identity");
    HANDLE handle=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    require(handle&&WaitForSingleObject(handle,0)==WAIT_TIMEOUT,"pending shutdown fixture was not live before shutdown");
    children.push_back({folder,handle});
  }
  for(const auto& snapshot:manager.snapshot())require(snapshot.awaiting_observation&&snapshot.duration_ms==0,"pending launch unexpectedly gained a game lifetime");
  const auto started=std::chrono::steady_clock::now();require(bool(manager.shutdown()),"multiple pending observations did not shut down cleanly");
  require(std::chrono::steady_clock::now()-started<1s,"shutdown finished pending discovery batches instead of stopping promptly");
  const auto history=manager.history(profile);
  require(history.launch_count==16&&history.total_playtime_ms==0&&history.provider_observed_playtime_ms==0,"pending shutdown fabricated playtime or changed launch accounting");
  for(const auto& record:history.recent)require(record.state==SessionState::detached&&record.awaiting_observation&&record.duration_ms==0&&record.ended_at==0&&!record.window,
    "pending shutdown fabricated association or a terminal game exit");
  for(const auto& child:children) {
    require(WaitForSingleObject(child.handle,0)==WAIT_TIMEOUT,"pending observation shutdown killed an owned forwarder");
    std::ofstream(child.folder/"release")<<"release";
    require(WaitForSingleObject(child.handle,5000)==WAIT_OBJECT_0,"pending forwarder failed to finish after explicit fixture release");CloseHandle(child.handle);
  }
}
#endif
}
int main(int argc, char** argv) {
#ifdef _WIN32
  if(argc==3&&std::string(argv[1])=="--observation-fixture")return observation_fixture(argv[2]);
#endif
  if (argc != 2) { std::cerr << "expected native session child path\n"; return 2; }
  const auto root = fs::weakly_canonical(fs::temp_directory_path()) / ("openxmb-session-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    const auto helper = fs::absolute(argv[1]);
    process_checks(root / "process", helper);
    manager_checks(root / "manager", helper);
    journal_checks(root / "journal", helper);
    quit_and_shutdown(root / "quit", helper);
#ifdef _WIN32
    observed_process_checks(root/"observed-process",fs::absolute(argv[0]));
    observed_manager_checks(root/"observed-manager",helper,fs::absolute(argv[0]));
    pending_shutdown_checks(root/"pending-shutdown",fs::absolute(argv[0]));
#endif
    fs::remove_all(root); std::cout << "Owned process and session journal checks passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
