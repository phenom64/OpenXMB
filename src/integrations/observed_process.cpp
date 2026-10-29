#include "openxmb/integrations/observed_process.hpp"
#include <algorithm>
#include <chrono>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif
#ifdef __linux__
#include "linux_observed_process.hpp"
#include "x11_support.hpp"
#include <cstdlib>
#include <limits>
#include <thread>
#endif

namespace openxmb::integrations {
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
struct ObservedProcess::State {
  std::int64_t pid{-1};std::uint64_t creation{};
  mutable std::mutex mutex;
  mutable ProcessObservation observation{ProcessState::running,{},{}};
#ifdef _WIN32
  HANDLE handle{};
  ~State(){if(handle)CloseHandle(handle);}
#endif
#ifdef __linux__
  // The retained pidfd, not the PID number, is the process identity.
  linux_observer::Fd pidfd;std::string display;
#endif
};
#ifdef __linux__
namespace {
// Sends only the advertised WM_DELETE_WINDOW protocol message to the XRes-pinned
// window of the pinned process. It never requests focus and never signals.
Result close_x11_window(const ObservedProcess& process,const SessionWindowTarget& target) {
  if(!target.handle||target.handle>std::numeric_limits<xcb_window_t>::max())return {false,"The observed game window identity is invalid."};
  x11_detail::Connection x;
  if(!x)return {false,"A native local X11 display is required to close the observed game."};
  const auto window=static_cast<xcb_window_t>(target.handle);
  const auto owner=x.client_pid(window);
  const auto token=linux_detail::process_creation(target.pid);
  if(!owner||*owner!=target.pid||!token||*token!=target.process_creation||process.observe().state!=ProcessState::running)
    return {false,"The observed game window changed."};
  const auto protocols=x.property(window,"WM_PROTOCOLS",XCB_ATOM_ATOM);
  const auto close=x.atom("WM_DELETE_WINDOW");
  if(close==XCB_ATOM_NONE||std::ranges::find(protocols,close)==protocols.end())return {false,"The game window does not advertise WM_DELETE_WINDOW; no close request was sent."};
  xcb_client_message_event_t event{};
  event.response_type=XCB_CLIENT_MESSAGE;event.format=32;event.window=window;event.type=x.atom("WM_PROTOCOLS");
  event.data.data32[0]=close;event.data.data32[1]=XCB_CURRENT_TIME;
  const bool sent=x.check(xcb_send_event_checked(x.c,0,window,XCB_EVENT_MASK_NO_EVENT,reinterpret_cast<const char*>(&event)));
  return {sent,sent?"Window close requested; the game may prompt or remain running.":"X11 could not deliver the game close request."};
}
}
#endif
ObservedProcess::ObservedProcess(std::shared_ptr<State> state):state_(std::move(state)){}
std::int64_t ObservedProcess::id()const noexcept{return state_->pid;}
std::uint64_t ObservedProcess::creation_time()const noexcept{return state_->creation;}
ProcessObservation ObservedProcess::observe()const {
  std::lock_guard lock(state_->mutex);
#ifdef _WIN32
  if(state_->observation.state==ProcessState::running) {
    const auto waited=WaitForSingleObject(state_->handle,0);
    if(waited==WAIT_OBJECT_0) {
      DWORD code{};
      if(GetExitCodeProcess(state_->handle,&code))state_->observation={ProcessState::exited,static_cast<int>(code),{}};
      else state_->observation={ProcessState::unknown,{},"The observed game exit code is unavailable."};
    }else if(waited!=WAIT_TIMEOUT)state_->observation={ProcessState::unknown,{},"The observed game identity is unavailable."};
  }
#elif defined(__linux__)
  if(state_->observation.state==ProcessState::running) {
    switch(linux_observer::pidfd_state(state_->pidfd.get())) {
    case linux_observer::PidfdState::exited:
      // A non-child exit status belongs to the parent; never invent one.
      state_->observation={ProcessState::exited,std::nullopt,"The observed game exited; the exit status of a non-child process is unavailable."};break;
    case linux_observer::PidfdState::failed:
      state_->observation={ProcessState::unknown,{},"The observed game identity is unavailable."};break;
    case linux_observer::PidfdState::running:
      if(const auto token=linux_detail::process_creation(state_->pid);token&&*token!=state_->creation)
        state_->observation={ProcessState::unknown,{},"The observed game identity changed."};
      break;
    }
  }
#else
  state_->observation={ProcessState::unknown,{},"Provider process observation is unavailable on this platform."};
#endif
  auto observation=state_->observation;observation.process_creation=state_->creation;return observation;
}
std::optional<SessionWindowTarget> ObservedProcess::window()const {
  if(observe().state!=ProcessState::running)return {};
#ifdef __linux__
  const auto* display=std::getenv("DISPLAY");
  if(!display||state_->display!=display)return {};
#endif
  const auto target=find_session_window(id());
  if(!target||target->process_creation!=creation_time()||!validate_session_window(*target))return {};
  return target;
}
Result ObservedProcess::request_close()const {
  const auto target=window();
  if(!target)return {false,"The observed game has no unique current window to close."};
#ifdef _WIN32
  if(observe().state!=ProcessState::running||!validate_session_window(*target))return {false,"The observed game window changed."};
  const bool sent=PostMessageW(reinterpret_cast<HWND>(target->handle),WM_CLOSE,0,0)!=FALSE;
  return {sent,sent?"Window close requested; the game may prompt or remain running.":"Windows could not request game closure."};
#elif defined(__linux__)
  return close_x11_window(*this,*target);
#else
  return {false,"Provider game closure is unavailable on this platform."};
#endif
}

struct ExecutableObservation::Impl {
  fs::path executable;std::string error;bool directory{};
  ExecutableMatchState failed{ExecutableMatchState::pending};
  std::int64_t excluded{-1};std::shared_ptr<ObservedProcess> matched;
#ifdef _WIN32
  std::uint64_t since{},candidate_creation{};std::int64_t candidate_pid{-1};Clock::time_point candidate_at{};
  DWORD session{};std::vector<unsigned char> user;
  static std::uint64_t stamp(FILETIME time){return(std::uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;}
  static bool equal(const std::wstring& a,const std::wstring& b){return CompareStringOrdinal(a.data(),static_cast<int>(a.size()),b.data(),static_cast<int>(b.size()),TRUE)==CSTR_EQUAL;}
  static fs::path canonical_image(const fs::path& path,bool directory=false) {
    HANDLE file=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
      directory?FILE_FLAG_BACKUP_SEMANTICS:FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return {};
    std::wstring native(32768,L'\0');const auto length=GetFinalPathNameByHandleW(file,native.data(),static_cast<DWORD>(native.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);CloseHandle(file);
    if(!length||length>=native.size())return {};native.resize(length);return fs::path(native);
  }
  static bool within(const fs::path& root,const fs::path& path) {
    auto part=path.begin();
    for(const auto& component:root) {
      if(part==path.end()||!equal(component.native(),part->native()))return false;
      ++part;
    }
    return part!=path.end(); // A sibling with a matching string prefix is outside.
  }
  static std::wstring lower(std::wstring value) {
    for(auto& c:value)if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';return value;
  }
  static bool helper_image(const fs::path& root,const fs::path& image) {
    const auto name=lower(image.stem().native());
    for(const auto* exact:{L"steam",L"steamwebhelper",L"gameoverlayui",L"launcher",L"setup",L"install",L"dxsetup",L"start_protected_game",L"eac_launcher",L"eosbootstrapper"})
      if(name==exact)return true;
    for(const auto* prefix:{L"unitycrashhandler",L"crashpad",L"crashreport",L"crashsender",L"vc_redist",L"vcredist",L"easyanticheat",L"unins"})
      if(name.starts_with(prefix))return true;
    if(name.ends_with(L"launcher"))return true;
    auto part=image.begin();for(const auto& ignored:root){(void)ignored;++part;}
    for(;part!=image.end();++part) {
      const auto folder=lower(part->native());
      for(const auto* helper:{L"_commonredist",L"redist",L"redistributables",L"directx",L"installers",L"crashpad"})
        if(folder==helper)return true;
    }
    return false;
  }
  static bool safe_root(const fs::path& root) {
    const auto relative=root.relative_path();
    if(!root.is_absolute()||root==root.root_path()||std::distance(relative.begin(),relative.end())<2)return false;
    const auto name=lower(root.filename().native());
    for(const auto* broad:{L"steam",L"steamlibrary",L"steamapps",L"common",L"games",L"windows",L"system32",L"syswow64",L"users",L"program files",L"program files (x86)",L"programdata",L"appdata",L"local",L"roaming",L"temp",L"tmp",L"desktop",L"documents",L"downloads"})
      if(name==broad)return false;
    std::error_code error;if(fs::is_directory(root/"steamapps",error))return false;
    if(lower(root.parent_path().filename().native())==L"users")return false;
    for(const auto* variable:{L"USERPROFILE",L"LOCALAPPDATA",L"APPDATA",L"PROGRAMDATA",L"ProgramFiles",L"ProgramFiles(x86)",L"TEMP",L"TMP",L"SystemRoot"}) {
      wchar_t value[32768]{};const auto size=GetEnvironmentVariableW(variable,value,32768);
      if(!size||size>=32768)continue;
      const auto location=fs::canonical(fs::path(value),error);if(error){error.clear();continue;}
      if(equal(root.native(),location.native())||(equal(variable,L"SystemRoot")&&within(location,root)))return false;
    }
    // Directory mode only accepts a local installation, not a UNC/remote tree.
    const auto path=root.native();return !path.starts_with(L"\\\\");
  }
  static std::vector<unsigned char> token_user(HANDLE process) {
    HANDLE token{};if(!OpenProcessToken(process,TOKEN_QUERY,&token))return {};
    DWORD bytes{};GetTokenInformation(token,TokenUser,nullptr,0,&bytes);
    if(!bytes||bytes>65536){CloseHandle(token);return {};}
    std::vector<unsigned char> data(bytes);
    const bool okay=GetTokenInformation(token,TokenUser,data.data(),bytes,&bytes)!=FALSE;CloseHandle(token);
    if(!okay)return {};
    return data; // Move the buffer: TOKEN_USER contains a SID pointer into it.
  }
  std::vector<std::shared_ptr<ObservedProcess::State>> scan(std::stop_token stop = {}) {
    std::vector<std::shared_ptr<ObservedProcess::State>> result;
    if(stop.stop_requested()){error="Game process discovery was stopped.";return result;}
    const auto scan_started=Clock::now();
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE){error="Windows could not enumerate game processes.";return result;}
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);bool more=Process32FirstW(snapshot,&entry)!=FALSE;std::size_t count=0;
    if(!more&&GetLastError()!=ERROR_NO_MORE_FILES)error="Windows process enumeration is unavailable.";
    while(more) {
      if(stop.stop_requested()){error="Game process discovery was stopped.";break;}
      if(++count>32768||Clock::now()-scan_started>std::chrono::milliseconds(250)){error="The process enumeration exceeds its time or entry limit.";break;}
      if(entry.th32ProcessID&&entry.th32ProcessID!=excluded&&(directory||equal(entry.szExeFile,executable.filename().native()))) {
        DWORD candidate_session{};
        if(!ProcessIdToSessionId(entry.th32ProcessID,&candidate_session)) {
          if(!directory){error="A matching process session is inaccessible.";break;}
        } else if(candidate_session==session) {
          const auto handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,entry.th32ProcessID);
          if(!handle){if(!directory&&GetLastError()!=ERROR_INVALID_PARAMETER)error="A matching process is inaccessible.";}
          else {
            auto state=std::make_shared<ObservedProcess::State>();state->handle=handle;state->pid=entry.th32ProcessID;
            wchar_t image[32768]{};DWORD length=32768;FILETIME create{},exit{},kernel{},cpu{};
            if(WaitForSingleObject(handle,0)==WAIT_TIMEOUT) {
              if(!QueryFullProcessImageNameW(handle,0,image,&length)||!GetProcessTimes(handle,&create,&exit,&kernel,&cpu)) {
                if(!directory)error="A matching process identity is inaccessible.";
              }
              else {
                const auto path=canonical_image(fs::path(std::wstring(image,length)));
                if(path.empty()){if(!directory)error="A matching process image cannot be verified.";}
                else if(directory?(within(executable,path)&&equal(path.extension().native(),L".exe")&&!helper_image(executable,path)):
                                  equal(path.native(),executable.native())) {
                  const auto owner=token_user(handle);
                  if(owner.empty()){if(!directory)error="The matching process user is inaccessible.";}
                  else if(EqualSid(reinterpret_cast<const TOKEN_USER*>(owner.data())->User.Sid,reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid)) {
                    state->creation=stamp(create);
                    const auto window=directory?find_session_window(state->pid):std::optional<SessionWindowTarget>{};
                    if((!directory||(window&&window->process_creation==state->creation&&validate_session_window(*window)))&&
                       WaitForSingleObject(handle,0)==WAIT_TIMEOUT)result.push_back(std::move(state));
                  }
                }
              }
            }
          }
        }
      }
      if(!error.empty())break;
      more=Process32NextW(snapshot,&entry)!=FALSE;
      if(!more&&GetLastError()!=ERROR_NO_MORE_FILES)error="Windows process enumeration was interrupted.";
    }
    CloseHandle(snapshot);return result;
  }
#elif defined(__linux__)
  // Native X11 only. The observer retains a pidfd for the candidate it returns;
  // a PID number is never rebound after association.
  struct Candidate {std::shared_ptr<ObservedProcess::State> state;bool windowed{};};
  std::string display;
  std::uint64_t since{},candidate_creation{};std::int64_t candidate_pid{-1};Clock::time_point candidate_at{};
  std::shared_ptr<ObservedProcess::State> inspect(std::int64_t pid,bool probe_window,bool& windowed) {
    using namespace linux_observer;
    windowed=false;
    const auto proc="/proc/"+std::to_string(pid);int link_error{};
    const auto image=link_target(proc+"/exe",link_error);
    if(!image) {
      // A same-user nondumpable (protected) process hides its image. In exact
      // mode a same-name one is explicitly unsupported rather than invisible.
      if(!directory&&link_error==EACCES&&comm_matches(pid,executable.filename().string())&&same_user(pid))
        error="A matching process is protected or inaccessible; protected games are unsupported.";
      return {};
    }
    const fs::path path(*image);
    if(directory?!(within(executable,path)&&!helper_image(executable,path)):path!=executable)return {};
    if(!same_user(pid))return {};
    if(!same_filesystem_view(pid)){error="A matching game runs in another mount namespace or root; only host-native X11 games are supported.";return {};}
    const auto start=linux_detail::process_creation(pid);
    const auto named=file_id(*image,true),running=file_id(proc+"/exe",true);
    if(!start||!named||!running||*named!=*running)return {};
    int pin_error{};auto pidfd=open_pidfd(pid,pin_error);
    if(!pidfd){if(pin_error!=ESRCH)error="Linux pidfd process pinning is unavailable; game observation fails closed.";return {};}
    // Both reads bracket the pidfd, so it refers to the process that was inspected.
    const auto again=link_target(proc+"/exe",link_error);const auto after=linux_detail::process_creation(pid);
    if(!again||*again!=*image||!after||*after!=*start||pidfd_state(pidfd.get())!=PidfdState::running)return {};
    auto state=std::make_shared<ObservedProcess::State>();
    state->pid=pid;state->creation=*start;state->pidfd=std::move(pidfd);state->display=display;
    if(probe_window) {
      const auto window=find_session_window(pid);
      windowed=window&&window->process_creation==*start&&validate_session_window(*window);
    }
    if(pidfd_state(state->pidfd.get())!=PidfdState::running)return {};
    return state;
  }
  std::vector<Candidate> scan(std::stop_token stop,bool baseline) {
    std::vector<Candidate> result;
    if(stop.stop_requested()){error="Game process discovery was stopped.";return result;}
    const auto self=static_cast<std::int64_t>(::getpid());
    // Exact mode counts every matching process; directory mode only windowed
    // ones, so a windowless background process of the install cannot block.
    const bool probe=directory||!baseline;const std::size_t wanted=baseline?1:2;std::size_t matches=0;
    const auto status=linux_observer::enumerate_processes(stop,Clock::now()+std::chrono::milliseconds(500),[&](std::int64_t pid) {
      if(pid==self||pid==excluded)return true;
      bool windowed{};auto state=inspect(pid,probe,windowed);
      if(!error.empty())return false;
      if(!state)return true;
      if(++matches>16){error="Too many processes match this game; observation is ambiguous.";return false;}
      if(directory&&!windowed)return true;
      result.push_back({std::move(state),windowed});
      return result.size()<wanted;
    });
    if(error.empty()) switch(status) {
    case linux_observer::Enumeration::stopped:error="Game process discovery was stopped.";break;
    case linux_observer::Enumeration::limit:error="The process enumeration exceeds its time or entry limit.";break;
    case linux_observer::Enumeration::failed:error="Linux process enumeration is unavailable.";break;
    case linux_observer::Enumeration::complete:break;
    }
    return result;
  }
#endif
};
ExecutableObservation::ExecutableObservation(const fs::path& target,bool installation_directory):impl_(std::make_unique<Impl>()) {
  auto& p=*impl_;p.directory=installation_directory;
#ifdef _WIN32
  if(!target.is_absolute()||target.native().find(wchar_t{})!=std::wstring::npos||target.native().size()>32768)throw std::runtime_error("Choose a full executable or installation path for Steam game observation.");
  std::error_code error;p.executable=fs::canonical(target,error);
  if(error)throw std::runtime_error("The selected Steam observation path is unavailable.");
  if(p.directory) {
    if(!fs::is_directory(p.executable,error)||error||!Impl::safe_root(p.executable))throw std::runtime_error("Choose a specific local game installation directory, not a drive, shared folder or Steam library root.");
  } else if(!fs::is_regular_file(p.executable,error)||error||!Impl::equal(p.executable.extension().native(),L".exe"))throw std::runtime_error("Choose an existing Windows game executable for observation.");
  p.executable=Impl::canonical_image(p.executable,p.directory);if(p.executable.empty())throw std::runtime_error("The game's canonical executable or installation identity is unavailable.");
  if(!ProcessIdToSessionId(GetCurrentProcessId(),&p.session)||(p.user=Impl::token_user(GetCurrentProcess())).empty())throw std::runtime_error("The current user/session identity is unavailable.");
  const auto baseline=p.scan();
  if(!p.error.empty())p.failed=ExecutableMatchState::unavailable;
  else if(!baseline.empty()){p.failed=ExecutableMatchState::rejected;p.error="A matching game was already running before this launch; it was not adopted.";}
  FILETIME now{};GetSystemTimeAsFileTime(&now);p.since=Impl::stamp(now);
#elif defined(__linux__)
  if(!target.is_absolute()||target.native().size()>PATH_MAX||target.native().find('\0')!=std::string::npos)throw std::runtime_error("Choose a full executable or installation path for Steam game observation.");
  std::error_code error;p.executable=fs::canonical(target,error);
  if(error)throw std::runtime_error("The selected Steam observation path is unavailable.");
  if(p.directory) {
    if(!fs::is_directory(p.executable,error)||error||!linux_observer::safe_root(p.executable))throw std::runtime_error("Choose a specific local game installation directory, not a home, system, mount, shared or Steam library root.");
  } else {
    struct stat info{};
    if(::stat(p.executable.c_str(),&info)||!S_ISREG(info.st_mode)||::access(p.executable.c_str(),X_OK))throw std::runtime_error("Choose an existing native Linux game executable for observation.");
  }
  if(const auto* display=std::getenv("DISPLAY"))p.display=display;
  if(!x11_detail::x11_session()) {p.failed=ExecutableMatchState::unavailable;p.error="Game observation does not support Wayland sessions.";return;}
  {
    const x11_detail::Connection x;
    if(!x){p.failed=ExecutableMatchState::unavailable;p.error="Game observation requires a native local X11 display; Wayland, Xwayland and remote displays are unsupported.";return;}
  }
  int pin_error{};
  if(!linux_observer::open_pidfd(::getpid(),pin_error)||!linux_detail::process_creation(::getpid())||!linux_observer::boot_ticks()) {
    p.failed=ExecutableMatchState::unavailable;p.error="Game observation requires pidfd and readable /proc process identity.";return;
  }
  // Start times are whole kernel ticks. Anything that starts at or before this
  // tick predates the launch; the launch is only released after the tick ends
  // so a genuinely new game always starts in a later tick.
  p.since=linux_observer::boot_ticks().value_or(std::numeric_limits<std::uint64_t>::max());
  const auto baseline=p.scan({},true);
  if(!p.error.empty())p.failed=ExecutableMatchState::unavailable;
  else if(!baseline.empty()){p.failed=ExecutableMatchState::rejected;p.error="A matching game was already running before this launch; it was not adopted.";}
  for(const auto limit=Clock::now()+std::chrono::milliseconds(100);p.since!=std::numeric_limits<std::uint64_t>::max()&&linux_observer::boot_ticks().value_or(0)<=p.since;std::this_thread::sleep_for(std::chrono::milliseconds(1)))
    if(Clock::now()>limit){p.since=std::numeric_limits<std::uint64_t>::max();break;}
#else
  (void)target;p.failed=ExecutableMatchState::unavailable;p.error="Steam game process observation is unavailable on this platform.";
#endif
}
ExecutableObservation::~ExecutableObservation()=default;
void ExecutableObservation::exclude_process(std::int64_t pid){impl_->excluded=pid;}
ExecutableMatch ExecutableObservation::poll(std::stop_token stop) {
  auto& p=*impl_;
  if(p.failed!=ExecutableMatchState::pending)return {p.failed,{},p.error};
#ifdef _WIN32
  // Discovery ends at association. Keep this exact retained process handle
  // through exit; a new instance or a PID reuse never triggers rediscovery.
  if(p.matched)return {ExecutableMatchState::matched,p.matched,{}};
  const auto candidates=p.scan(stop);
  if(!p.error.empty()){p.failed=ExecutableMatchState::unavailable;return {p.failed,{},p.error};}
  if(candidates.size()>1){p.failed=ExecutableMatchState::rejected;return {p.failed,{},"Several matching game processes appeared; observation is ambiguous."};}
  if(candidates.empty()){p.candidate_pid=-1;return {};}
  const auto& candidate=candidates.front();
  if(candidate->creation<=p.since){p.failed=ExecutableMatchState::rejected;return {p.failed,{},"The matching process predates this launch; it was not adopted."};}
  if(candidate->pid!=p.candidate_pid||candidate->creation!=p.candidate_creation){p.candidate_pid=candidate->pid;p.candidate_creation=candidate->creation;p.candidate_at=Clock::now();return {};}
  if(Clock::now()-p.candidate_at<std::chrono::milliseconds(200))return {};
  p.matched=std::shared_ptr<ObservedProcess>(new ObservedProcess(candidate));return {ExecutableMatchState::matched,p.matched,{}};
#elif defined(__linux__)
  // Discovery ends at association. The retained pidfd stays through exit; a
  // later instance or a reused PID never triggers rediscovery.
  if(p.matched)return {ExecutableMatchState::matched,p.matched,{}};
  const auto found=p.scan(stop,false);
  if(!p.error.empty()){p.failed=ExecutableMatchState::unavailable;return {p.failed,{},p.error};}
  if(found.size()>1){p.failed=ExecutableMatchState::rejected;return {p.failed,{},"Several matching game processes appeared; observation is ambiguous."};}
  if(found.empty()||(!p.directory&&!found.front().windowed)){p.candidate_pid=-1;return {};}
  const auto& candidate=found.front().state;
  if(candidate->creation<=p.since){p.failed=ExecutableMatchState::rejected;return {p.failed,{},"The matching process predates this launch; it was not adopted."};}
  if(candidate->pid!=p.candidate_pid||candidate->creation!=p.candidate_creation){p.candidate_pid=candidate->pid;p.candidate_creation=candidate->creation;p.candidate_at=Clock::now();return {};}
  if(Clock::now()-p.candidate_at<std::chrono::milliseconds(200))return {};
  p.matched=std::shared_ptr<ObservedProcess>(new ObservedProcess(candidate));return {ExecutableMatchState::matched,p.matched,{}};
#else
  (void)stop;
  return {ExecutableMatchState::unavailable,{},p.error};
#endif
}
} // namespace openxmb::integrations
