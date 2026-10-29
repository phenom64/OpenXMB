#include "focus_handoff.hpp"
#ifdef _WIN32
#include <tlhelp32.h>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <limits>
#include <vector>

namespace openxmb::integrations::focus_handoff {
namespace {
using clock=std::chrono::steady_clock;
constexpr std::string_view report_tag="OXFH2";
constexpr std::size_t report_limit=256;

// Canonical ASCII decimal: digits only, no leading zero; zero only if allowed.
bool parse_decimal(std::string_view text,std::uint64_t maximum,bool allow_zero,std::uint64_t& out) {
  if(text.empty()||text.size()>20)return false;
  if(text[0]=='0'&&(text.size()>1||!allow_zero))return false;
  if(!std::all_of(text.begin(),text.end(),[](char c){return c>='0'&&c<='9';}))return false;
  std::uint64_t value{};const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
  if(error!=std::errc{}||end!=text.data()+text.size()||value>maximum)return false;
  out=value;return true;
}
bool known(std::uint64_t value) {
  return value<=static_cast<std::uint32_t>(Outcome::admission_granted)||
    (value>=static_cast<std::uint32_t>(Outcome::malformed)&&value<=static_cast<std::uint32_t>(Outcome::grant_refused));
}
constexpr std::size_t report_fields=15;  // after the tag
// Writes the report line without allocating; returns its length or 0.
std::size_t write_report(const Report& r,char* out,std::size_t size) noexcept {
  const int length=std::snprintf(out,size,"%s %u %lu %u %llu %llu %u %u %u %u %u %llu %llu %u %u %u\n",report_tag.data(),
    static_cast<unsigned>(r.outcome),static_cast<unsigned long>(r.error),static_cast<unsigned>(r.activation),
    static_cast<unsigned long long>(r.foreground_before),static_cast<unsigned long long>(r.foreground_after),
    static_cast<unsigned>(r.strategy),static_cast<unsigned>(r.relay),static_cast<unsigned>(r.grant),static_cast<unsigned>(r.rights_before),
    static_cast<unsigned>(r.source_flags),static_cast<unsigned long long>(r.source_capture),static_cast<unsigned long long>(r.source_menu_owner),
    static_cast<unsigned>(r.overlay_flags),static_cast<unsigned>(r.lock_timeout),static_cast<unsigned>(r.idle_ms));
  return length>0&&static_cast<std::size_t>(length)<size?static_cast<std::size_t>(length):0;
}
HWND as_hwnd(std::uint64_t value) {return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value));}
std::uint64_t as_value(HWND window) {return reinterpret_cast<std::uintptr_t>(window);}
Report failure(Outcome outcome,DWORD error=0) {Report report;report.outcome=outcome;report.error=error;return report;}
std::wstring module_path() {
  std::wstring buffer(32768,L'\0');
  const DWORD size=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
  buffer.resize(size<buffer.size()?size:0);return buffer;
}
bool same_text(std::wstring_view a,std::wstring_view b) {
  return CompareStringOrdinal(a.data(),static_cast<int>(a.size()),b.data(),static_cast<int>(b.size()),TRUE)==CSTR_EQUAL;
}
std::wstring object_name(HANDLE object) {
  DWORD needed{};GetUserObjectInformationW(object,UOI_NAME,nullptr,0,&needed);
  if(!needed)return {};
  std::wstring name(needed/sizeof(wchar_t),L'\0');
  if(!GetUserObjectInformationW(object,UOI_NAME,name.data(),needed,&needed))return {};
  while(!name.empty()&&name.back()==L'\0')name.pop_back();
  return name;
}
struct Search {HWND window;bool found;};
BOOL CALLBACK find_window(HWND window,LPARAM context) {
  auto& search=*reinterpret_cast<Search*>(context);
  if(window==search.window){search.found=true;return FALSE;}
  return TRUE;
}
// Top-level `window` lives on this thread's desktop.
bool on_this_desktop(HWND window) {
  Search search{window,false};
  EnumDesktopWindows(GetThreadDesktop(GetCurrentThreadId()),find_window,reinterpret_cast<LPARAM>(&search));
  return search.found;
}
std::uint64_t own_creation() {
  FILETIME create{},exit{},kernel{},user{};
  if(!GetProcessTimes(GetCurrentProcess(),&create,&exit,&kernel,&user))return 0;
  return (std::uint64_t(create.dwHighDateTime)<<32)|create.dwLowDateTime;
}
std::optional<Report> check_parent(const Request& request) {
  const DWORD self=GetCurrentProcessId();
  DWORD actual{};
  const HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
  if(snapshot==INVALID_HANDLE_VALUE)return failure(Outcome::parent_mismatch,GetLastError());
  PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
  for(BOOL more=Process32FirstW(snapshot,&entry);more;more=Process32NextW(snapshot,&entry))
    if(entry.th32ProcessID==self){actual=entry.th32ParentProcessID;break;}
  CloseHandle(snapshot);
  if(actual!=request.parent_pid)return failure(Outcome::parent_mismatch);
  // Creation identity excludes a reused pid; an exact sibling image excludes a
  // foreign process that merely spawned this executable.
  const auto parent=process_facts(request.parent_pid);
  if(!parent.creation||parent.creation!=request.parent_creation||parent.creation>own_creation())return failure(Outcome::parent_mismatch);
  const std::filesystem::path parent_image{parent.image},own_image{module_path()};
  // QueryFullProcessImageName resolves junctions while GetModuleFileName can
  // retain the caller's spelling (including the CMake checkout alias). Require
  // the same directory identity instead of equal path strings.
  std::error_code directory_error;
  if(parent.image.empty()||own_image.empty()||
     !std::filesystem::equivalent(parent_image.parent_path(),own_image.parent_path(),directory_error)||directory_error)
    return failure(Outcome::parent_mismatch);
  return std::nullopt;
}
std::optional<Report> check_desktop() {
  const HDESK input=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
  if(!input)return failure(Outcome::desktop_mismatch,GetLastError());
  const auto input_name=object_name(input);CloseDesktop(input);
  const auto own_name=object_name(GetThreadDesktop(GetCurrentThreadId()));
  if(input_name.empty()||!same_text(input_name,own_name))return failure(Outcome::desktop_mismatch);
  return std::nullopt;
}
// Foreground facts for the handoff source. Empty when it is acceptable.
std::optional<Report> check_foreground(const Request& request,HWND& foreground,DWORD& thread) {
  foreground=GetForegroundWindow();
  if(foreground==as_hwnd(request.overlay)){auto report=failure(Outcome::already_foreground);report.foreground_before=report.foreground_after=as_value(foreground);return report;}
  DWORD pid{};thread=foreground?GetWindowThreadProcessId(foreground,&pid):0;
  if(!thread||pid!=request.session_pid||thread==GetCurrentThreadId()||!session_window_or_owned(foreground,as_hwnd(request.session_window))) {
    auto report=failure(Outcome::foreground_mismatch);report.foreground_before=as_value(foreground);return report;
  }
  if(IsHungAppWindow(foreground)){auto report=failure(Outcome::hung);report.foreground_before=as_value(foreground);return report;}
  return std::nullopt;
}
// Shares this helper thread's input queue with the foreground thread for one
// activation. A detach is recorded only after AttachThreadInput(FALSE)
// succeeds; the destructor retries, and process exit is the final release.
// Nothing here allocates or throws.
class InputAttachment {
  DWORD self_,other_,attach_error_{},detach_error_{};bool attached_{};
public:
  InputAttachment(DWORD self,DWORD other) noexcept:self_(self),other_(other) {
    SetLastError(ERROR_SUCCESS);attached_=AttachThreadInput(self,other,TRUE)!=FALSE;if(!attached_)attach_error_=GetLastError();
  }
  ~InputAttachment() {(void)detach();}
  InputAttachment(const InputAttachment&)=delete;
  InputAttachment& operator=(const InputAttachment&)=delete;
  explicit operator bool() const noexcept {return attached_;}
  bool detach() noexcept {
    for(unsigned attempt=0;attached_&&attempt<3;++attempt) {
      SetLastError(ERROR_SUCCESS);
      if(AttachThreadInput(self_,other_,FALSE)){attached_=false;break;}
      detach_error_=GetLastError();
      // The system removes attachments of a thread that has exited.
      if(const HANDLE other=OpenThread(SYNCHRONIZE,FALSE,other_)) {
        const bool gone=WaitForSingleObject(other,0)==WAIT_OBJECT_0;CloseHandle(other);
        if(gone){attached_=false;break;}
      }
    }
    return !attached_;
  }
  [[nodiscard]] DWORD attach_error() const noexcept {return attach_error_;}
  [[nodiscard]] DWORD detach_error() const noexcept {return detach_error_;}
};
DWORD WINAPI watchdog(void* parent) {
  // Contains a helper whose main thread is blocked in an attached activation
  // after its parent died or stopped waiting.
  WaitForSingleObject(static_cast<HANDLE>(parent),helper_lifetime_ms);
  TerminateProcess(GetCurrentProcess(),static_cast<UINT>(Outcome::internal));
  return 0;
}
struct Handle {
  HANDLE value{};
  Handle()=default;
  explicit Handle(HANDLE handle):value(handle) {}
  ~Handle() {close();}
  Handle(const Handle&)=delete;
  Handle& operator=(const Handle&)=delete;
  void close() noexcept {if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value={};}
};
// Dispatches `window`'s messages and delivers other threads' sent messages
// and activation events without removing anything posted to other windows.
void pump(HWND window) noexcept {
  MSG message{};
  for(unsigned count=0;window&&count<64&&PeekMessageW(&message,window,0,0,PM_REMOVE);++count){TranslateMessage(&message);DispatchMessageW(&message);}
  PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
}
// True when `object` is signalled or `done()` before `until`. `slice` caps a
// single wait for conditions that raise no wake bit of their own.
template<class Done>
bool pump_until(HANDLE object,HWND window,clock::time_point until,DWORD slice,Done done) noexcept {
  for(;;) {
    pump(window);
    if(object&&WaitForSingleObject(object,0)==WAIT_OBJECT_0)return true;
    if(done())return true;
    const auto now=clock::now();if(now>=until)return false;
    const auto remaining=std::chrono::ceil<std::chrono::milliseconds>(until-now).count();
    const DWORD wait=static_cast<DWORD>(std::min<long long>(remaining,slice));
    MsgWaitForMultipleObjectsEx(object?1:0,object?&object:nullptr,wait,QS_ALLINPUT,0);
  }
}
std::string elapsed_text(clock::time_point since) {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now()-since).count())+" ms";
}
} // namespace

std::optional<Request> parse_request(std::span<const std::string_view> args) {
  if(args.size()!=7)return std::nullopt;
  constexpr auto u32=std::numeric_limits<DWORD>::max();constexpr auto u64=std::numeric_limits<std::uint64_t>::max();
  std::uint64_t value[7]{};
  constexpr std::uint64_t limits[7]{u32,u64,u64,u32,u32,u64,u64};
  for(std::size_t i=0;i<7;++i)if(!parse_decimal(args[i],limits[i],false,value[i]))return std::nullopt;
  if(value[2]>std::numeric_limits<std::uintptr_t>::max()||value[6]>std::numeric_limits<std::uintptr_t>::max())return std::nullopt;
  return Request{static_cast<DWORD>(value[0]),value[1],value[2],static_cast<DWORD>(value[3]),static_cast<DWORD>(value[4]),value[5],value[6]};
}
std::wstring format_arguments(const Request& request) {
  std::wstring out(command.begin(),command.end());
  for(const std::uint64_t value:{std::uint64_t{request.parent_pid},request.parent_creation,request.overlay,std::uint64_t{request.overlay_thread},
      std::uint64_t{request.session_pid},request.session_creation,request.session_window})out+=L" "+std::to_wstring(value);
  return out;
}
std::string_view describe(Outcome outcome) {
  switch(outcome) {
  case Outcome::handed_off:return "the helper's activation was admitted";
  case Outcome::already_foreground:return "the overlay already owned the foreground";
  case Outcome::malformed:return "the request arguments were malformed";
  case Outcome::parent_mismatch:return "the requesting process is not the helper's OpenXMB parent";
  case Outcome::desktop_mismatch:return "the helper is not on the active input desktop";
  case Outcome::overlay_mismatch:return "the target is not a visible interactive OpenXMB overlay of the parent UI thread on this desktop";
  case Outcome::session_mismatch:return "the session process or window no longer matches the validated session";
  case Outcome::foreground_mismatch:return "the foreground window is not the validated session window or a window it owns";
  case Outcome::hung:return "the session window is not responding";
  case Outcome::preflight_failed:return "the session window did not answer the responsiveness preflight";
  case Outcome::changed:return "the foreground changed during the preflight";
  case Outcome::attach_failed:return "AttachThreadInput to the session thread failed";
  case Outcome::refused:return "Windows refused the helper's activation";
  case Outcome::detach_failed:return "AttachThreadInput detach failed";
  case Outcome::internal:return "the helper failed internally";
  case Outcome::admission_granted:return "the helper granted this process foreground admission";
  case Outcome::relay_failed:return "the helper could not create its relay window";
  case Outcome::relay_refused:return "the helper's relay window did not become the foreground";
  case Outcome::grant_refused:return "Windows refused the helper's AllowSetForegroundWindow";
  }
  return "the helper reported an unknown outcome";
}
std::string report_text(const Report& report) {
  constexpr std::string_view strategies[]{"attached_activate","attached_grant","relay_activate","relay_grant","restore_activate","restore_grant","unattached_activate"};
  const auto index=static_cast<std::uint32_t>(report.strategy);
  const auto flag=[](std::uint32_t value){return std::string(value==2?"not called":value?"TRUE":"FALSE");};
  const auto hex=[](std::uint32_t value){
    if(value==unavailable)return std::string("unavailable");
    char text[2+8+1]{};std::snprintf(text,sizeof(text),"0x%X",value);return std::string(text);};
  const auto ms=[](std::uint32_t value){return value==unavailable?std::string("unavailable"):std::to_string(value)+" ms";};
  std::string text="[outcome "+std::to_string(static_cast<std::uint32_t>(report.outcome))+", strategy "+
    (index<std::size(strategies)?std::string(strategies[index]):std::to_string(index));
  if(report.rights_before!=2)text+=", own AllowSetForegroundWindow before admission="+flag(report.rights_before);
  if(report.relay!=2)text+=", relay foreground="+flag(report.relay);
  if(report.grant!=2)text+=", AllowSetForegroundWindow(parent)="+flag(report.grant);
  if(report.activation!=2)text+=", helper SetForegroundWindow(overlay)="+flag(report.activation);
  text+=", foreground "+hwnd_text(report.foreground_before)+" -> "+hwnd_text(report.foreground_after);
  text+=", foreground thread GUI flags "+hex(report.source_flags)+" capture "+hwnd_text(report.source_capture)+
    " menu owner "+hwnd_text(report.source_menu_owner)+", overlay thread GUI flags "+hex(report.overlay_flags)+
    ", foreground lock timeout "+ms(report.lock_timeout)+", input idle "+ms(report.idle_ms)+"]";
  return text;
}
std::string format_report(const Report& report) {
  char line[report_limit]{};
  return std::string(line,write_report(report,line,sizeof(line)));
}
std::optional<Report> parse_report(std::string_view text) {
  if(text.empty()||text.size()>=report_limit||text.back()!='\n')return std::nullopt;
  text.remove_suffix(1);
  std::string_view fields[report_fields+1];std::size_t count{};
  for(;;) {
    if(count==std::size(fields))return std::nullopt;
    const auto space=text.find(' ');fields[count++]=text.substr(0,space);
    if(space==std::string_view::npos)break;
    text.remove_prefix(space+1);
  }
  if(count!=std::size(fields)||fields[0]!=report_tag)return std::nullopt;
  constexpr auto u32=std::uint64_t{std::numeric_limits<std::uint32_t>::max()};constexpr auto u64=std::numeric_limits<std::uint64_t>::max();
  constexpr std::uint64_t limits[report_fields]{u32,u32,2,u64,u64,static_cast<std::uint32_t>(Strategy::unattached_activate),2,2,2,u32,u64,u64,u32,u32,u32};
  std::uint64_t value[report_fields]{};
  for(std::size_t i=0;i<report_fields;++i)if(!parse_decimal(fields[i+1],limits[i],true,value[i]))return std::nullopt;
  if(!known(value[0]))return std::nullopt;
  const auto u=[&](std::size_t i){return static_cast<std::uint32_t>(value[i]);};
  Report report;
  report.outcome=static_cast<Outcome>(value[0]);report.error=u(1);report.activation=u(2);
  report.foreground_before=value[3];report.foreground_after=value[4];report.strategy=static_cast<Strategy>(value[5]);
  report.relay=u(6);report.grant=u(7);report.rights_before=u(8);
  report.source_flags=u(9);report.source_capture=value[10];report.source_menu_owner=value[11];
  report.overlay_flags=u(12);report.lock_timeout=u(13);report.idle_ms=u(14);
  return report;
}
ProcessFacts process_facts(DWORD pid) {
  ProcessFacts facts;
  const HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!process)return facts;
  FILETIME create{},exit{},kernel{},user{};
  if(GetProcessTimes(process,&create,&exit,&kernel,&user))facts.creation=(std::uint64_t(create.dwHighDateTime)<<32)|create.dwLowDateTime;
  std::wstring path(32768,L'\0');DWORD size=static_cast<DWORD>(path.size());
  if(QueryFullProcessImageNameW(process,0,path.data(),&size)){path.resize(size);facts.image=std::move(path);}
  CloseHandle(process);return facts;
}
bool session_window_or_owned(HWND window,HWND session) {
  for(unsigned depth=0;window&&depth<32;++depth,window=GetWindow(window,GW_OWNER))if(window==session)return true;
  return false;
}
std::string hwnd_text(std::uint64_t window) {
  char text[2+16+1]{};std::snprintf(text,sizeof(text),"0x%llX",static_cast<unsigned long long>(window));return text;
}
std::string narrow(std::wstring_view text) {
  if(text.empty())return {};
  const int size=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
  std::string out(size>0?static_cast<std::size_t>(size):0,'\0');
  if(size>0)WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),size,nullptr,nullptr);
  return out;
}
std::string windows_error(std::string_view action,DWORD code) {
  std::string out(action);
  if(!code)return out+" (Windows reported no error code).";
  wchar_t* text{};
  const auto length=FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
    nullptr,code,0,reinterpret_cast<wchar_t*>(&text),0,nullptr);
  std::wstring_view view(text?text:L"",text?length:0);
  while(!view.empty()&&(view.back()==L'\r'||view.back()==L'\n'||view.back()==L' '||view.back()==L'.'))view.remove_suffix(1);
  out+=" (Windows error "+std::to_string(code);
  if(!view.empty())out+=": "+narrow(view);
  if(text)LocalFree(text);
  return out+").";
}
std::optional<Report> check_overlay(const Request& request) {
  const HWND overlay=as_hwnd(request.overlay);DWORD pid{};
  if(!overlay||!IsWindow(overlay)||GetWindowThreadProcessId(overlay,&pid)!=request.overlay_thread||pid!=request.parent_pid)return failure(Outcome::overlay_mismatch);
  wchar_t name[64]{};const int length=GetClassNameW(overlay,name,64);
  if(GetAncestor(overlay,GA_ROOT)!=overlay||length<=0||std::wstring_view(name,static_cast<std::size_t>(length))!=overlay_class)return failure(Outcome::overlay_mismatch);
  // Passive notice hosts are WS_EX_NOACTIVATE and must never be activated.
  if((GetWindowLongPtrW(overlay,GWL_EXSTYLE)&WS_EX_NOACTIVATE)||!IsWindowVisible(overlay)||!on_this_desktop(overlay))return failure(Outcome::overlay_mismatch);
  return std::nullopt;
}
std::optional<Report> check_session(const Request& request) {
  if(request.session_pid==request.parent_pid||request.session_pid==GetCurrentProcessId())return failure(Outcome::session_mismatch);
  const auto facts=process_facts(request.session_pid);
  if(!facts.creation||facts.creation!=request.session_creation)return failure(Outcome::session_mismatch);
  const HWND session=as_hwnd(request.session_window);DWORD pid{};
  if(!session||!IsWindow(session)||!GetWindowThreadProcessId(session,&pid)||pid!=request.session_pid||!on_this_desktop(session))return failure(Outcome::session_mismatch);
  return std::nullopt;
}
std::optional<Report> preflight(HWND window,UINT timeout_ms) {
  DWORD_PTR ignored{};SetLastError(ERROR_SUCCESS);
  if(SendMessageTimeoutW(window,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_ERRORONEXIT,timeout_ms,&ignored))return std::nullopt;
  auto report=failure(Outcome::preflight_failed,GetLastError());report.foreground_before=as_value(window);return report;
}
Relay::Relay(bool minimized) noexcept {
  const HINSTANCE instance=GetModuleHandleW(nullptr);
  WNDCLASSEXW type{};type.cbSize=sizeof(type);type.lpfnWndProc=DefWindowProcW;type.hInstance=instance;type.lpszClassName=L"OpenXMB.FocusRelay.v1";
  if(!RegisterClassExW(&type)) {
    const DWORD error=GetLastError();
    if(error!=ERROR_CLASS_ALREADY_EXISTS){error_=error;return;}
  }
  window_=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW,type.lpszClassName,L"",WS_POPUP,
    -32000,-32000,1,1,nullptr,nullptr,instance,nullptr);
  if(!window_){error_=GetLastError();return;}
  if(!SetLayeredWindowAttributes(window_,0,0,LWA_ALPHA)){error_=GetLastError();DestroyWindow(window_);window_={};return;}
  ShowWindow(window_,minimized?SW_SHOWMINNOACTIVE:SW_SHOWNA);
}
Relay::~Relay() {if(window_)DestroyWindow(window_);}
namespace {
// Read-only admission state; fields stay `unavailable` when a query fails.
void observe(Report& report,DWORD source_thread,DWORD overlay_thread) noexcept {
  GUITHREADINFO info{};info.cbSize=sizeof(info);
  if(GetGUIThreadInfo(source_thread,&info)) {
    report.source_flags=info.flags;report.source_capture=as_value(info.hwndCapture);report.source_menu_owner=as_value(info.hwndMenuOwner);
  }
  GUITHREADINFO overlay{};overlay.cbSize=sizeof(overlay);
  if(GetGUIThreadInfo(overlay_thread,&overlay))report.overlay_flags=overlay.flags;
  DWORD timeout{};
  if(SystemParametersInfoW(SPI_GETFOREGROUNDLOCKTIMEOUT,0,&timeout,0))report.lock_timeout=timeout;
  LASTINPUTINFO input{};input.cbSize=sizeof(input);
  if(GetLastInputInfo(&input))report.idle_ms=GetTickCount()-input.dwTime;
}
bool uses_relay(Strategy strategy) {return strategy>=Strategy::relay_activate&&strategy<=Strategy::restore_grant;}
bool grants(Strategy strategy) {
  return strategy==Strategy::attached_grant||strategy==Strategy::relay_grant||strategy==Strategy::restore_grant;
}
// The admission step once the relay (if any) is foreground or the helper is
// attached: activate the overlay, or grant only the parent.
void admit(Report& report,const Request& request,Strategy strategy) noexcept {
  if(grants(strategy)) {
    SetLastError(ERROR_SUCCESS);
    const BOOL granted=AllowSetForegroundWindow(request.parent_pid);
    report.grant=granted?1:0;
    if(granted)report.outcome=Outcome::admission_granted;else{report.outcome=Outcome::grant_refused;report.error=GetLastError();}
    return;
  }
  const BOOL activated=SetForegroundWindow(as_hwnd(request.overlay));
  report.activation=activated?1:0;
  report.outcome=activated?Outcome::handed_off:Outcome::refused;
}
Report validate_and_admit(const Request& request,const Hooks& hooks) {
  if(auto failed=check_parent(request))return *failed;
  if(auto failed=check_desktop())return *failed;
  if(auto failed=check_overlay(request))return *failed;
  if(auto failed=check_session(request))return *failed;
  HWND foreground{};DWORD thread{};
  if(auto failed=check_foreground(request,foreground,thread))return *failed;
  if(auto failed=preflight(foreground))return *failed;
  // A relay exists before revalidation so nothing runs between the final
  // checks and the admission step but the admission itself.
  std::optional<Relay> relay;
  if(uses_relay(hooks.strategy)) {
    relay.emplace(hooks.strategy==Strategy::restore_activate||hooks.strategy==Strategy::restore_grant);
    if(!relay->window())return failure(Outcome::relay_failed,relay->error());
  }
  // The preflight let other threads run: revalidate immediately before admission.
  HWND current{};DWORD current_thread{};
  if(check_overlay(request)||check_session(request)||check_foreground(request,current,current_thread)||current!=foreground||current_thread!=thread) {
    auto report=failure(Outcome::changed);report.foreground_before=as_value(foreground);report.foreground_after=as_value(current);return report;
  }
  Report report;report.foreground_before=as_value(foreground);
  observe(report,thread,request.overlay_thread);
  if(hooks.measure_rights)report.rights_before=AllowSetForegroundWindow(GetCurrentProcessId())?1:0;
  if(hooks.strategy==Strategy::unattached_activate) {
    admit(report,request,hooks.strategy);
  } else if(hooks.strategy==Strategy::restore_activate||hooks.strategy==Strategy::restore_grant) {
    // SDL_RestoreWindow then SDL_RaiseWindow, without any attachment.
    ShowWindow(relay->window(),SW_RESTORE);
    if(GetForegroundWindow()!=relay->window())SetForegroundWindow(relay->window());
    report.relay=GetForegroundWindow()==relay->window()?1:0;
    if(report.relay)admit(report,request,hooks.strategy);else report.outcome=Outcome::relay_refused;
  } else {
    // AttachThreadInput needs this thread to own a message queue.
    MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
    InputAttachment shared(GetCurrentThreadId(),thread);
    if(!shared)return failure(Outcome::attach_failed,shared.attach_error());
    if(hooks.after_attach)hooks.after_attach();
    if(relay) {
      // The relay is on the shared, foreground queue.
      report.relay=SetForegroundWindow(relay->window())&&GetForegroundWindow()==relay->window()?1:0;
      if(report.relay) {
        if(hooks.after_relay)hooks.after_relay();
        admit(report,request,hooks.strategy);
      } else report.outcome=Outcome::relay_refused;
    } else admit(report,request,hooks.strategy);
    if(!shared.detach()){report.outcome=Outcome::detach_failed;report.error=shared.detach_error();}
  }
  report.foreground_after=as_value(GetForegroundWindow());
  return report;
}
} // namespace
Report perform(const Request& request,const Hooks& hooks) {
  Report report=validate_and_admit(request,hooks);
  report.strategy=hooks.strategy;
  return report;
}
int run_helper(std::span<const std::string_view> args,const Hooks& hooks) noexcept {
  Report report=failure(Outcome::internal);report.strategy=hooks.strategy;
  try {
    if(const auto request=parse_request(args)) {
      // The watchdog owns this handle until process exit.
      const HANDLE parent=OpenProcess(SYNCHRONIZE,FALSE,request->parent_pid);
      if(!parent)report=failure(Outcome::parent_mismatch,GetLastError());
      else if(const HANDLE guard=CreateThread(nullptr,64*1024,watchdog,parent,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);!guard) {
        report=failure(Outcome::internal,GetLastError());CloseHandle(parent);
      } else {CloseHandle(guard);report=perform(*request,hooks);}
    } else report=failure(Outcome::malformed);
  } catch(...) {report=failure(Outcome::internal);}
  report.strategy=hooks.strategy;
  char line[report_limit]{};
  DWORD written{};
  if(const auto length=write_report(report,line,sizeof(line))) {
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,static_cast<DWORD>(length),&written,nullptr);
    if(hooks.on_report)hooks.on_report(std::string_view(line,length));
  }
  return static_cast<int>(report.outcome);
}
std::filesystem::path sibling_helper() {
  return std::filesystem::path{module_path()}.parent_path()/helper_name;
}
HelperRun launch(const std::filesystem::path& helper,const Request& request,clock::time_point until,HWND pump_window) noexcept {
  HelperRun run;const auto start=clock::now();
  const auto finish=[&]{run.elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(clock::now()-start);return run;};
  try {
    std::wstring line=L"\""+helper.native()+L"\" "+format_arguments(request);
    std::wstring desktop=object_name(GetProcessWindowStation())+L"\\"+object_name(GetThreadDesktop(GetCurrentThreadId()));
    const std::wstring directory=helper.parent_path().native();
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    Handle report_read,report_write;
    if(!CreatePipe(&report_read.value,&report_write.value,&inheritable,4096)||!SetHandleInformation(report_read.value,HANDLE_FLAG_INHERIT,0)){run.launch_error=GetLastError();return finish();}
    // Only the report pipe is inherited, never other inheritable handles.
    SIZE_T size{};InitializeProcThreadAttributeList(nullptr,1,0,&size);
    std::vector<std::byte> storage(size);auto* attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if(!InitializeProcThreadAttributeList(attributes,1,0,&size)){run.launch_error=GetLastError();return finish();}
    struct Attributes {LPPROC_THREAD_ATTRIBUTE_LIST list;~Attributes(){DeleteProcThreadAttributeList(list);}} attributes_guard{attributes};
    HANDLE inherited[]{report_write.value};
    if(!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)){run.launch_error=GetLastError();return finish();}
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput=report_write.value;startup.StartupInfo.lpDesktop=desktop.data();startup.lpAttributeList=attributes;
    PROCESS_INFORMATION created{};
    if(!CreateProcessW(helper.c_str(),line.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,
        directory.empty()?nullptr:directory.c_str(),&startup.StartupInfo,&created)){run.launch_error=GetLastError();return finish();}
    Handle process{created.hProcess};CloseHandle(created.hThread);report_write.close();
    run.launched=true;run.pid=created.dwProcessId;
    run.exited=pump_until(process.value,pump_window,until,INFINITE,[]{return false;});
    if(!run.exited) {
      run.timed_out=true;
      // Fails with ERROR_ACCESS_DENIED if the helper exited at the deadline.
      if(TerminateProcess(process.value,ERROR_TIMEOUT))run.terminated=true;else run.terminate_error=GetLastError();
    }
    // Reaping is bounded and keeps answering this thread's messages.
    run.reaped=run.exited||pump_until(process.value,pump_window,clock::now()+reap_bound,INFINITE,[]{return false;});
    GetExitCodeProcess(process.value,&run.exit_code);
    if(run.exited) {
      // Never wait for pipe EOF: only consume bytes already present, even if
      // an unexpected descendant retained the helper's inherited write end.
      DWORD available{},count{};
      if(PeekNamedPipe(report_read.value,nullptr,0,nullptr,&available,nullptr)&&available&&available<=report_limit) {
        char buffer[report_limit];
        if(ReadFile(report_read.value,buffer,available,&count,nullptr)&&count==available) {
          if(auto report=parse_report(std::string_view(buffer,count));report&&static_cast<DWORD>(report->outcome)==run.exit_code)run.report=report;
        }
      }
    }
  } catch(...) {if(!run.launched)run.launch_error=ERROR_OUTOFMEMORY;}
  return finish();
}
bool await_foreground(HWND window,clock::time_point until) noexcept {
  // Cross-thread activation completes when this thread processes it; the
  // slice covers a foreground change that raises no wake bit here.
  return pump_until(nullptr,window,until,16,[window]{return GetForegroundWindow()==window;});
}
Result hand_off(HWND window,const Request& request,const std::filesystem::path& helper,clock::time_point until) {
  const auto start=clock::now();
  const auto run=launch(helper,request,until,window);
  const auto name="the focus helper "+narrow(helper.filename().native())+(run.pid?" (process "+std::to_string(run.pid)+")":"");
  if(!run.launched)return {false,windows_error(name+" could not start",run.launch_error)};
  if(run.timed_out) {
    std::string text=name+" did not finish within "+elapsed_text(start)+"; ";
    if(!run.terminated&&!run.reaped)return {false,windows_error(text+"terminating it failed",run.terminate_error)};
    if(!run.terminated)return {false,text+"it exited on its own at the deadline (exit code "+std::to_string(run.exit_code)+")."};
    text+="it was terminated";
    text+=run.reaped?" and reaped (exit code "+std::to_string(run.exit_code)+").":" but had not exited within "+std::to_string(reap_bound.count())+" ms.";
    return {false,text};
  }
  if(!run.report)return {false,name+" exited with code "+std::to_string(run.exit_code)+" without a valid report."};
  const auto& report=*run.report;
  if(report.outcome!=Outcome::handed_off&&report.outcome!=Outcome::already_foreground&&report.outcome!=Outcome::admission_granted) {
    const std::string text=name+" refused: "+std::string(describe(report.outcome))+" "+report_text(report);
    return {false,report.error?windows_error(text,report.error):text+"."};
  }
  // The helper's process has exited, so its attachment and relay are gone.
  // A grant is only admission: this thread activates its own overlay.
  std::string own;
  if(report.outcome==Outcome::admission_granted)own=std::string(" and SetForegroundWindow here returned ")+(SetForegroundWindow(window)?"TRUE":"FALSE");
  // Success needs the overlay to be the foreground as observed here.
  if(await_foreground(window,until))return {true,"Overlay focus: "+name+" verified "+report_text(report)+own+"."};
  return {false,name+" reported that "+std::string(describe(report.outcome))+" "+report_text(report)+own+", but the foreground is "+
    hwnd_text(as_value(GetForegroundWindow()))+" after "+elapsed_text(start)+"."};
}
} // namespace openxmb::integrations::focus_handoff
#endif
