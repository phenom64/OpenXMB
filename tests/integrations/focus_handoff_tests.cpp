// Isolated overlay focus-helper fixtures. Every window and helper runs on a
// private, never-input desktop, so nothing here can take the user's focus.
// argv[1]: path of openxmb_focus_handoff_test_helper.exe (default: beside this
// executable; it must share this executable's directory).
#ifndef _WIN32
#include <iostream>
int main() {std::cout<<"Focus helper fixtures are Windows-only\n";return 0;}
#else
#include "focus_handoff.hpp"
#include "openxmb/integrations/xmb_overlay.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
using namespace openxmb::integrations;
using namespace openxmb::integrations::focus_handoff;
using namespace std::chrono_literals;
namespace {
void require(bool ok,const std::string& message) {if(!ok)throw std::runtime_error(message);}
bool contains(const std::string& text,std::string_view part) {return text.find(part)!=std::string::npos;}
std::uint64_t value(HWND window) {return reinterpret_cast<std::uintptr_t>(window);}
std::vector<std::string> split(const std::wstring& line) {
  std::vector<std::string> out;std::string current;
  for(const wchar_t c:line){if(c==L' '){out.push_back(current);current.clear();}else current.push_back(static_cast<char>(c));}
  out.push_back(current);return out;
}
std::optional<Request> parse(std::initializer_list<std::string_view> args) {return parse_request(std::vector<std::string_view>(args));}

// Runs `body` on a fresh thread attached to a private desktop.
std::wstring desktop_name;
template<class Body> void on_private_desktop(Body body) {
  static unsigned serial{};
  desktop_name=L"OpenXMBFocusHandoff"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(++serial);
  const HDESK desktop=CreateDesktopW(desktop_name.c_str(),nullptr,nullptr,0,
    DESKTOP_CREATEWINDOW|DESKTOP_READOBJECTS|DESKTOP_WRITEOBJECTS|DESKTOP_ENUMERATE|DESKTOP_CREATEMENU,nullptr);
  require(desktop!=nullptr,"CreateDesktopW failed with "+std::to_string(GetLastError()));
  std::string failure;
  std::thread([&]{
    if(!SetThreadDesktop(desktop)){failure="SetThreadDesktop failed with "+std::to_string(GetLastError());return;}
    try{body(desktop);}catch(const std::exception& error){failure=error.what();}
  }).join();
  CloseDesktop(desktop);
  require(failure.empty(),failure);
}
// A test-helper child in window mode on `desktop`; prints its HWND.
struct WindowChild {
  PROCESS_INFORMATION process{};HWND window{};
  ~WindowChild() {if(process.hProcess){TerminateProcess(process.hProcess,0);WaitForSingleObject(process.hProcess,5000);CloseHandle(process.hProcess);}}
};
void spawn_window(const std::filesystem::path& helper,bool hung,WindowChild& child) {
  SetEnvironmentVariableA("OPENXMB_FOCUS_TEST_HELPER_MODE",hung?"window-hung":"window");
  SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};HANDLE read{},write{};
  require(CreatePipe(&read,&write,&inherit,0)&&SetHandleInformation(read,HANDLE_FLAG_INHERIT,0),"fixture pipe");
  std::wstring desk=L"WinSta0\\"+desktop_name;
  STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdOutput=write;startup.lpDesktop=desk.data();
  std::wstring line=L"\""+helper.native()+L"\"";
  const BOOL created=CreateProcessW(helper.c_str(),line.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child.process);
  CloseHandle(write);
  if(!created){CloseHandle(read);require(false,"fixture window process failed with "+std::to_string(GetLastError()));}
  CloseHandle(child.process.hThread);child.process.hThread=nullptr;
  std::string text;char buffer[64];DWORD count{};
  while(text.find('\n')==std::string::npos&&ReadFile(read,buffer,sizeof(buffer),&count,nullptr)&&count)text.append(buffer,count);
  CloseHandle(read);
  child.window=reinterpret_cast<HWND>(static_cast<std::uintptr_t>(std::stoull(text)));
}
void parser_fixtures() {
  const Request request{4242,133000000000000001ull,0x1A2B3C,77,9001,133000000000000002ull,0x4D5E};
  const auto words=split(format_arguments(request));
  require(words.size()==8&&words[0]==command,"format_arguments shape");
  std::vector<std::string_view> args(words.begin()+1,words.end());
  require(parse_request(args)==request,"argument round trip");
  require(!parse({"1","2","3","4","5","6"}),"six arguments accepted");
  require(!parse({"1","2","3","4","5","6","7","8"}),"eight arguments accepted");
  for(const std::string_view bad:{"0","01","+1","-1"," 1","1 ","0x10","1e3","","\xEF\xBC\x91","4294967296"})
    require(!parse({bad,"2","3","4","5","6","7"}),"pid accepted '"+std::string(bad)+"'");
  require(!parse({"1","2","3","4","5","6","18446744073709551616"}),"64-bit overflow accepted");
  require(bool(parse({"4294967295","18446744073709551615","3","4","5","6","7"})),"maximum values rejected");
  Report full{Outcome::admission_granted,5,2,0x10,0x20};
  full.strategy=Strategy::restore_grant;full.relay=1;full.grant=1;full.rights_before=0;
  Report control;control.strategy=static_cast<Strategy>(6);
  require(parse_report(format_report(control))==control,"unattached negative control report round trip");
  full.source_flags=0x14;full.source_capture=~0ull;full.source_menu_owner=0x30;full.overlay_flags=unavailable;full.lock_timeout=200000;full.idle_ms=0;
  require(parse_report(format_report(full))==full,"report round trip");
  for(const Report each:{Report{Outcome::handed_off,0,1,1,2},Report{Outcome::malformed,0,2,0,0},Report{Outcome::internal,4294967295u,0,0,0},
      Report{Outcome::grant_refused,5,2,0,0}})
    require(parse_report(format_report(each))==each,"report round trip for outcome "+std::to_string(static_cast<unsigned>(each.outcome)));
  const std::string tail=" 0 0 0 2 2 2 4294967295 0 0 4294967295 4294967295 4294967295\n";
  require(bool(parse_report("OXFH2 0 0 1"+tail)),"valid report rejected");
  for(const std::string& bad:std::initializer_list<std::string>{"OXFH2 0 0 1"+tail.substr(0,tail.size()-1),"OXFH1 0 0 1"+tail,"OXFH2 0 0 1 0"+tail,"OXFH2 0 0 1 0 0 0 2 2 2 4294967295 0 0 4294967295 4294967295\n",
      "OXFH2 3 0 2"+tail,"OXFH2 9 0 2"+tail,"OXFH2 26 0 2"+tail,"OXFH2 0 0 3"+tail,"OXFH2 00 0 1"+tail,"OXFH2  0 0 1"+tail,"OXFH2 0 0 1"+tail.substr(0,tail.size()-1)+" \n",
      "OXFH2 0 0 1 0 0 7 2 2 2 0 0 0 0 0 0\n","OXFH2 0 0 1 0 0 0 3 2 2 0 0 0 0 0 0\n","OXFH2 0 0 1 0 0 0 2 2 2 0 18446744073709551616 0 0 0 0\n",
      "OXFH2 0 0 1 0 0 0 2 2 2 4294967296 0 0 0 0 0\n","OXFH2 0 0 1 0 -1 0 2 2 2 0 0 0 0 0 0\n"})
    require(!parse_report(bad),"malformed report accepted: "+bad);
  // The production entry rejects malformed arguments before any identity work.
  const std::vector<std::string_view> malformed{"1","2"};
  require(run_helper(malformed)==static_cast<int>(Outcome::malformed),"run_helper accepted malformed arguments");
}
// An interactive host, visible off-screen without activation on the private desktop.
void show_host(XmbOverlay& overlay,OverlayInteraction interaction=OverlayInteraction::interactive) {
  std::vector<std::uint8_t> pixels(16*16*4);
  require(bool(overlay.prepare({-32000,-32000,16,16},interaction==OverlayInteraction::interactive?OverlayCallback{[](const OverlayInput&){}}:OverlayCallback{},interaction)),"host prepare");
  require(bool(overlay.present(pixels,16,16,16*4))&&bool(overlay.show(false)),"host present/show");
}
void identity_fixtures(const std::filesystem::path& helper) {
  on_private_desktop([&](HDESK) {
    XmbOverlay overlay;show_host(overlay);
    const HWND host=reinterpret_cast<HWND>(overlay.native_handle());
    const Request base{GetCurrentProcessId(),process_facts(GetCurrentProcessId()).creation,value(host),GetCurrentThreadId(),0,0,0};
    require(!check_overlay(base),"genuine interactive host rejected");
    auto wrong=base;wrong.overlay_thread+=1;require(check_overlay(wrong)->outcome==Outcome::overlay_mismatch,"wrong overlay thread accepted");
    wrong=base;wrong.parent_pid+=4;require(check_overlay(wrong)->outcome==Outcome::overlay_mismatch,"overlay of another parent accepted");
    // Foreign HWNDs: the desktop window (another process) and a non-overlay window of ours.
    wrong=base;wrong.overlay=value(GetDesktopWindow());require(check_overlay(wrong)->outcome==Outcome::overlay_mismatch,"foreign HWND accepted as overlay");
    const HWND plain=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"",WS_POPUP|WS_VISIBLE,-32000,-32000,8,8,nullptr,nullptr,nullptr,nullptr);
    require(plain!=nullptr,"plain fixture window");
    wrong=base;wrong.overlay=value(plain);require(check_overlay(wrong)->outcome==Outcome::overlay_mismatch,"non-overlay class accepted");
    DestroyWindow(plain);
    XmbOverlay notice;show_host(notice,OverlayInteraction::passive);
    wrong=base;wrong.overlay=notice.native_handle();require(check_overlay(wrong)->outcome==Outcome::overlay_mismatch,"passive notice host accepted for activation");
    overlay.hide();require(check_overlay(base)->outcome==Outcome::overlay_mismatch,"hidden host accepted");

    WindowChild game;spawn_window(helper,false,game);
    const auto game_creation=process_facts(game.process.dwProcessId).creation;
    const Request session{GetCurrentProcessId(),0,0,0,game.process.dwProcessId,game_creation,value(game.window)};
    require(!check_session(session),"genuine session window rejected");
    auto stale=session;stale.session_creation+=1;require(check_session(stale)->outcome==Outcome::session_mismatch,"reused session pid accepted");
    stale=session;stale.session_window=value(host);require(check_session(stale)->outcome==Outcome::session_mismatch,"window of another process accepted as session");
    stale=session;stale.parent_pid=stale.session_pid;require(check_session(stale)->outcome==Outcome::session_mismatch,"parent accepted as its own session");
    require(session_window_or_owned(game.window,game.window)&&!session_window_or_owned(host,game.window),"session ownership");

    require(!preflight(game.window),"responsive session failed the preflight");
    WindowChild wedged;spawn_window(helper,true,wedged);
    const auto start=std::chrono::steady_clock::now();
    const auto probe=preflight(wedged.window,100);
    const auto took=std::chrono::steady_clock::now()-start;
    require(probe&&probe->outcome==Outcome::preflight_failed&&probe->error==ERROR_TIMEOUT,"wedged session passed the preflight or lost ERROR_TIMEOUT");
    require(took>=90ms&&took<1s,"preflight was not bounded by its timeout");
  });
}
// The relay must be invisible: no taskbar button, no hit testing, alpha 0,
// 1x1 off-screen, activatable (never NOACTIVATE), and gone with its owner.
void relay_fixtures() {
  on_private_desktop([](HDESK) {
    HWND seen{};
    for(const bool minimized:{false,true}) {
      {
        const Relay relay(minimized);
        seen=relay.window();
        require(seen&&!relay.error(),"relay creation failed (error "+std::to_string(relay.error())+")");
        const auto ex=GetWindowLongPtrW(seen,GWL_EXSTYLE);
        require((ex&WS_EX_LAYERED)&&(ex&WS_EX_TRANSPARENT)&&(ex&WS_EX_TOOLWINDOW)&&!(ex&WS_EX_APPWINDOW)&&!(ex&WS_EX_NOACTIVATE),"relay extended style");
        require(GetWindow(seen,GW_OWNER)==nullptr&&(GetWindowLongPtrW(seen,GWL_STYLE)&WS_POPUP),"relay style/owner");
        BYTE alpha{255};DWORD flags{};
        require(GetLayeredWindowAttributes(seen,nullptr,&alpha,&flags)&&(flags&LWA_ALPHA)&&alpha==0,"relay is not fully transparent");
        require(IsWindowVisible(seen)&&bool(IsIconic(seen))==minimized,"relay show state");
        if(!minimized) {
          RECT rect{};require(GetWindowRect(seen,&rect)&&rect.left==-32000&&rect.top==-32000&&rect.right-rect.left==1&&rect.bottom-rect.top==1,"relay bounds");
        }
        require(GetForegroundWindow()!=seen,"relay activated on creation");
      }
      require(!IsWindow(seen),"relay outlived its owner");
    }
  });
}
// The helper runs with a real parent identity; launch() pumps this thread.
struct Launch {HelperRun run;Result result;std::chrono::milliseconds elapsed{};int failed_probes{},answered_probes{};};
Launch run_mode(const std::filesystem::path& helper,const char* mode,std::chrono::milliseconds budget,bool corrupt_parent=false,bool full=false) {
  Launch out;
  on_private_desktop([&](HDESK desktop) {
    SetEnvironmentVariableA("OPENXMB_FOCUS_TEST_HELPER_MODE",mode);
    XmbOverlay overlay;show_host(overlay);
    const HWND host=reinterpret_cast<HWND>(overlay.native_handle());
    Request request{GetCurrentProcessId(),process_facts(GetCurrentProcessId()).creation,value(host),GetCurrentThreadId(),
      GetCurrentProcessId()+4,1,value(host)};
    if(corrupt_parent)request.parent_creation+=1;
    // Another thread keeps asking the waiting UI thread to answer messages.
    std::atomic<bool> done{false};
    std::thread prober([&]{
      if(!SetThreadDesktop(desktop))return;
      while(!done){DWORD_PTR ignored{};if(SendMessageTimeoutW(host,WM_NULL,0,0,SMTO_ABORTIFHUNG,100,&ignored))++out.answered_probes;else ++out.failed_probes;}
    });
    const auto start=std::chrono::steady_clock::now();
    if(full)out.result=hand_off(host,request,helper,start+budget);
    else out.run=launch(helper,request,start+budget,host);
    out.elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start);
    // Keep answering the prober's in-flight message while it stops.
    done=true;const HANDLE stopping=prober.native_handle();
    while(MsgWaitForMultipleObjects(1,&stopping,FALSE,INFINITE,QS_SENDMESSAGE)==WAIT_OBJECT_0+1){MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);}
    prober.join();
    require(GetForegroundWindow()!=host,"private-desktop host became foreground");
  });
  SetEnvironmentVariableA("OPENXMB_FOCUS_TEST_HELPER_MODE",nullptr);
  return out;
}
void process_fixtures(const std::filesystem::path& helper) {
  // Production entry, real identity: parent accepted, refused at the desktop check.
  auto real=run_mode(helper,"real",1500ms);
  require(real.run.launched&&real.run.exited&&real.run.report&&real.run.report->outcome==Outcome::desktop_mismatch,
    "real helper on a private desktop did not refuse at the desktop check (exit "+std::to_string(real.run.exit_code)+")");
  require(real.run.report->activation==2,"real helper called SetForegroundWindow");
  auto foreign=run_mode(helper,"real",1500ms,true);
  require(foreign.run.report&&foreign.run.report->outcome==Outcome::parent_mismatch,"forged parent creation accepted");
  auto refused=run_mode(helper,"real",1500ms,false,true);
  require(!refused.result&&contains(refused.result.message,describe(Outcome::desktop_mismatch)),"hand_off hid the helper refusal: "+refused.result.message);
  // Every probe validates first: no relay, grant or activation before the checks pass.
  const std::pair<const char*,Strategy> probes[]{{"probe-attached-activate",Strategy::attached_activate},{"probe-attached-grant",Strategy::attached_grant},
    {"probe-relay-activate",Strategy::relay_activate},{"probe-relay-grant",Strategy::relay_grant},
    {"probe-restore-activate",Strategy::restore_activate},{"probe-restore-grant",Strategy::restore_grant}};
  for(const auto& [mode,strategy]:probes) {
    auto probe=run_mode(helper,mode,1500ms);
    require(probe.run.report&&probe.run.report->outcome==Outcome::desktop_mismatch,std::string(mode)+" did not refuse at the desktop check");
    const auto& r=*probe.run.report;
    require(r.strategy==strategy,std::string(mode)+" reported another strategy");
    require(r.activation==2&&r.relay==2&&r.grant==2&&r.rights_before==2,std::string(mode)+" acted before validation");
  }

  // Containment: a helper that never returns is terminated and reaped at the deadline.
  auto hung=run_mode(helper,"hang",400ms);
  require(hung.run.launched&&hung.run.timed_out&&hung.run.terminated&&hung.run.reaped,"wedged helper was not terminated and reaped");
  require(hung.run.exit_code==ERROR_TIMEOUT&&!hung.run.report,"terminated helper exit code/report");
  require(hung.elapsed>=380ms&&hung.elapsed<400ms+reap_bound,"deadline not honoured: "+std::to_string(hung.elapsed.count())+" ms");
  require(hung.answered_probes>0&&hung.failed_probes==0,"UI thread stopped answering messages while waiting ("+
    std::to_string(hung.answered_probes)+" answered, "+std::to_string(hung.failed_probes)+" timed out)");
  auto hung_result=run_mode(helper,"hang",300ms,false,true);
  require(!hung_result.result&&contains(hung_result.result.message,"terminated and reaped (exit code 1460)"),"timeout message: "+hung_result.result.message);

  // Success needs the foreground observed here, never the helper's word alone.
  auto lie=run_mode(helper,"lie",300ms,false,true);
  require(!lie.result&&contains(lie.result.message,"but the foreground is"),"unverified helper success accepted: "+lie.result.message);
  // A grant is admission only: this thread must still activate and observe it.
  auto grant_lie=run_mode(helper,"grant-lie",300ms,false,true);
  require(!grant_lie.result&&contains(grant_lie.result.message,"SetForegroundWindow here returned FALSE")&&contains(grant_lie.result.message,"but the foreground is"),
    "unverified grant accepted: "+grant_lie.result.message);
  for(const char* mode:{"silent","mismatch"}) {
    auto bad=run_mode(helper,mode,1500ms,false,true);
    require(!bad.result&&contains(bad.result.message,"without a valid report"),std::string(mode)+" helper accepted: "+bad.result.message);
  }
  Launch missing;
  on_private_desktop([&](HDESK){
    XmbOverlay overlay;show_host(overlay);const HWND host=reinterpret_cast<HWND>(overlay.native_handle());
    missing.result=hand_off(host,Request{GetCurrentProcessId(),1,value(host),GetCurrentThreadId(),1,1,1},helper.parent_path()/"missing-helper.exe",
      std::chrono::steady_clock::now()+300ms);
  });
  require(!missing.result&&contains(missing.result.message,"could not start (Windows error 2"),"missing helper: "+missing.result.message);
  std::cout<<"timeout containment: "<<hung.elapsed.count()<<" ms, "<<hung.answered_probes<<" UI-thread probes answered\n";
}
} // namespace
int main(int argc,char** argv) {
  try {
    std::wstring self(32768,L'\0');self.resize(GetModuleFileNameW(nullptr,self.data(),static_cast<DWORD>(self.size())));
    const std::filesystem::path helper=argc>1?std::filesystem::path{argv[1]}:std::filesystem::path{self}.parent_path()/"openxmb_focus_handoff_test_helper.exe";
    require(std::filesystem::is_regular_file(helper),"test helper not found: "+helper.string());
    parser_fixtures();
    identity_fixtures(helper);
    relay_fixtures();
    process_fixtures(helper);
    std::cout<<"Focus helper fixtures passed\n";return 0;
  } catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
#endif
