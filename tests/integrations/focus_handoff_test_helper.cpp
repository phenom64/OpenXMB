// TEST-ONLY stand-in for `openxmb-integrations.exe --focus-handoff-v2`. Never
// built into or shipped with OpenXMB; the product helper has no such modes.
// OPENXMB_FOCUS_TEST_HELPER_MODE selects the behaviour:
//   real         the production helper entry (default)
//   attach-hang  production validation and attach, then blocks while attached
//                (real-desktop containment fixture; the parent must kill it)
//   relay-hang   blocks after the relay becomes foreground (same containment)
//   input-recipient  argv[1] is the game HWND; a separate nonactivating input
//                fixture receives its own clicks, preserving game foreground
//   control-unattached  negative control, plain helper activation only
//   attached-activate / relay-activate  admission probes without ASFW measurement
//   hang         blocks right after the arguments parse
//   lie          reports a handoff without doing anything
//   silent       exits 0 without a report
//   mismatch     reports a handoff but exits with a different code
//   grant-lie    reports admission_granted without granting anything
//   window       (no arguments) shows an inert window, prints its HWND, pumps
//   window-hung  same window, then never pumps (unresponsive session fixture)
// Admission probes for actual-app runs: the full production validation with
// the named Strategy, plus rights_before=AllowSetForegroundWindow(own pid):
//   probe-attached-activate probe-attached-grant probe-relay-activate
//   probe-relay-grant probe-restore-activate probe-restore-grant
// Every mode running the helper entry also appends its exact report line to
// the file named by OPENXMB_FOCUS_TEST_REPORT_LOG when that is set, because
// the product logs helper reports only on failure.
#ifndef _WIN32
int main() {return 0;}
#else
#include "focus_handoff.hpp"
#include <cstdint>
#include <vector>
#include <string>
#include <string_view>
#include <chrono>
using namespace openxmb::integrations::focus_handoff;
namespace {
void block_forever() {WaitForSingleObject(GetCurrentProcess(),INFINITE);}
void block_with_marker(std::string_view marker) {
  wchar_t path[32768]{};
  const DWORD size=GetEnvironmentVariableW(L"OPENXMB_FOCUS_TEST_ATTACH_MARKER",path,32768);
  if(size&&size<32768) {
    const HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE) {DWORD written{};WriteFile(file,marker.data(),static_cast<DWORD>(marker.size()),&written,nullptr);CloseHandle(file);}
  }
  block_forever();
}
void block_after_attach() {block_with_marker("attached\n");}
void block_after_relay() {block_with_marker("relay "+std::to_string(GetCurrentProcessId())+" "+
  std::to_string(reinterpret_cast<std::uintptr_t>(GetForegroundWindow()))+"\n");}
void write(std::string_view text) {DWORD written{};WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text.data(),static_cast<DWORD>(text.size()),&written,nullptr);}
void log_report(std::string_view line) noexcept {
  wchar_t path[32768]{};
  const DWORD size=GetEnvironmentVariableW(L"OPENXMB_FOCUS_TEST_REPORT_LOG",path,32768);
  if(!size||size>=32768)return;
  const HANDLE file=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)return;
  DWORD written{};WriteFile(file,line.data(),static_cast<DWORD>(line.size()),&written,nullptr);CloseHandle(file);
}
struct Probe {std::string_view mode;Strategy strategy;};
constexpr Probe probes[]{
  {"probe-attached-activate",Strategy::attached_activate},{"probe-attached-grant",Strategy::attached_grant},
  {"probe-relay-activate",Strategy::relay_activate},{"probe-relay-grant",Strategy::relay_grant},
  {"probe-restore-activate",Strategy::restore_activate},{"probe-restore-grant",Strategy::restore_grant},
};
bool input_received{};
LRESULT CALLBACK input_recipient_proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
  if(message==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
  if(message==WM_LBUTTONUP)input_received=true;
  return DefWindowProcW(window,message,wp,lp);
}
// A sibling of the shell, never an ancestor of the activation helper. It
// injects and receives its own click marker so neither the harness nor any
// shell ancestor acquires SendInput/last-input eligibility from the fixture.
int input_recipient(HWND game) {
  if(!game||GetForegroundWindow()!=game)return 40;
  WNDCLASSW cls{};cls.lpfnWndProc=input_recipient_proc;cls.lpszClassName=L"OpenXMB.AdmissionInputFixture.v1";
  cls.hInstance=GetModuleHandleW(nullptr);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
  if(!RegisterClassW(&cls))return 41;
  RECT bounds{};if(!GetWindowRect(game,&bounds))return 42;
  const int x=bounds.left+48,y=bounds.top+48;
  const HWND own=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,cls.lpszClassName,L"Input fixture",
    WS_POPUP,x,y,48,32,nullptr,nullptr,cls.hInstance,nullptr);
  if(!own)return 43;
  ShowWindow(own,SW_SHOWNOACTIVATE);
  POINT cursor{},point{x+16,y+16};GetCursorPos(&cursor);
  if(cursor.x==point.x&&cursor.y==point.y)++point.x;
  GUITHREADINFO game_input{};game_input.cbSize=sizeof(game_input);
  if(WindowFromPoint(point)!=own||GetForegroundWindow()!=game||
     !GetGUIThreadInfo(GetWindowThreadProcessId(game,nullptr),&game_input)||game_input.hwndCapture){DestroyWindow(own);return 44;}
  INPUT event{};event.type=INPUT_MOUSE;
  event.mi.dx=MulDiv(point.x-GetSystemMetrics(SM_XVIRTUALSCREEN),65535,GetSystemMetrics(SM_CXVIRTUALSCREEN)-1);
  event.mi.dy=MulDiv(point.y-GetSystemMetrics(SM_YVIRTUALSCREEN),65535,GetSystemMetrics(SM_CYVIRTUALSCREEN)-1);
  event.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
  INPUT marker[]{event,{}, {}};marker[1].type=marker[2].type=INPUT_MOUSE;
  marker[1].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;marker[2].mi.dwFlags=MOUSEEVENTF_LEFTUP;
  if((GetAsyncKeyState(VK_LBUTTON)|GetAsyncKeyState(VK_RBUTTON)|GetAsyncKeyState(VK_MBUTTON))&0x8000){DestroyWindow(own);return 50;}
  const auto send_marker=[&] {
    // The absolute move/down/up batch cannot interleave with user input. If
    // Windows inserts only move/down, release our button before failing.
    const UINT sent=SendInput(3,marker,sizeof(INPUT));
    if(sent==2)SendInput(1,marker+2,sizeof(INPUT));
    return sent==3;
  };
  if(!send_marker()){DestroyWindow(own);return 45;}
  POINT marker_position{};if(!GetCursorPos(&marker_position)){DestroyWindow(own);return 51;}
  const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  bool announced{};
  while(std::chrono::steady_clock::now()<end) {
    MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE))DispatchMessageW(&message);
    if(input_received&&!announced) {
      if(GetForegroundWindow()!=game){DestroyWindow(own);return 46;}
      write("recipient-ready\n");announced=true;
    }
    DWORD available{};
    if(!PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE),nullptr,0,nullptr,&available,nullptr)||available)break;
    if(GetForegroundWindow()==game&&WindowFromPoint(point)==own) {
      POINT current{};game_input.cbSize=sizeof(game_input);
      if(!GetCursorPos(&current)||current.x!=marker_position.x||current.y!=marker_position.y||
         !GetGUIThreadInfo(GetWindowThreadProcessId(game,nullptr),&game_input)||game_input.hwndCapture){DestroyWindow(own);return 52;}
      if(!send_marker()){DestroyWindow(own);return 45;}
    }
    Sleep(10);
  }
  DestroyWindow(own);
  return announced?0:47;
}
}
int main(int argc,char** argv) {
  char mode_buffer[32]{};
  const DWORD length=GetEnvironmentVariableA("OPENXMB_FOCUS_TEST_HELPER_MODE",mode_buffer,sizeof(mode_buffer));
  const std::string_view mode=length&&length<sizeof(mode_buffer)?std::string_view{mode_buffer,length}:"real";
  if(mode=="input-recipient") {
    if(argc!=2)return 48;
    try{return input_recipient(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(std::stoull(argv[1]))));}
    catch(...){return 49;}
  }
  if(mode=="window"||mode=="window-hung") {
    // Off-screen, never activated: a stand-in session window on the caller's desktop.
    const HWND window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"focus fixture",WS_POPUP,-32000,-32000,8,8,nullptr,nullptr,nullptr,nullptr);
    if(!window)return 1;
    ShowWindow(window,SW_SHOWNOACTIVATE);
    write(std::to_string(reinterpret_cast<std::uintptr_t>(window))+"\n");
    if(mode=="window-hung")block_forever();
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0)DispatchMessageW(&message);
    return 0;
  }
  if(argc<2||std::string_view{argv[1]}!=command)return static_cast<int>(Outcome::malformed);
  const std::vector<std::string_view> args(argv+2,argv+argc);
  Hooks hooks;hooks.on_report=log_report;
  if(mode=="control-unattached"){hooks.strategy=Strategy::unattached_activate;return run_helper(args,hooks);}
  if(mode=="relay-activate"){hooks.strategy=Strategy::relay_activate;return run_helper(args,hooks);}
  if(mode=="attached-activate"){hooks.strategy=Strategy::attached_activate;return run_helper(args,hooks);}
  if(mode=="attach-hang"){hooks.after_attach=block_after_attach;return run_helper(args,hooks);}
  if(mode=="relay-hang"){hooks.after_relay=block_after_relay;return run_helper(args,hooks);}
  for(const auto& probe:probes)if(mode==probe.mode){hooks.strategy=probe.strategy;hooks.measure_rights=true;return run_helper(args,hooks);}
  if(mode=="hang"){if(!parse_request(args))return static_cast<int>(Outcome::malformed);block_forever();}
  if(mode=="lie"){write(format_report(Report{Outcome::handed_off,0,1,0,0}));return 0;}
  if(mode=="grant-lie"){write(format_report(Report{Outcome::admission_granted,0,2,0,0}));return static_cast<int>(Outcome::admission_granted);}
  if(mode=="silent")return 0;
  if(mode=="mismatch"){write(format_report(Report{Outcome::handed_off,0,1,0,0}));return static_cast<int>(Outcome::refused);}
  return run_helper(args,hooks);
}
#endif
