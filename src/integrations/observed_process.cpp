#include "openxmb/integrations/observed_process.hpp"
#include <chrono>
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
};
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
#else
  state_->observation={ProcessState::unknown,{},"Provider process observation is unavailable on this platform."};
#endif
  auto observation=state_->observation;observation.process_creation=state_->creation;return observation;
}
std::optional<SessionWindowTarget> ObservedProcess::window()const {
  if(observe().state!=ProcessState::running)return {};
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
#else
  return {false,"Provider game closure is unavailable on this platform."};
#endif
}

struct ExecutableObservation::Impl {
  fs::path executable;std::string error;
  ExecutableMatchState failed{ExecutableMatchState::pending};
  std::int64_t excluded{-1};std::shared_ptr<ObservedProcess> matched;
#ifdef _WIN32
  std::uint64_t since{},candidate_creation{};std::int64_t candidate_pid{-1};Clock::time_point candidate_at{};
  DWORD session{};std::vector<unsigned char> user;
  static std::uint64_t stamp(FILETIME time){return(std::uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;}
  static bool equal(const std::wstring& a,const std::wstring& b){return CompareStringOrdinal(a.data(),static_cast<int>(a.size()),b.data(),static_cast<int>(b.size()),TRUE)==CSTR_EQUAL;}
  static fs::path canonical_image(const fs::path& path) {
    HANDLE file=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return {};
    std::wstring native(32768,L'\0');const auto length=GetFinalPathNameByHandleW(file,native.data(),static_cast<DWORD>(native.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);CloseHandle(file);
    if(!length||length>=native.size())return {};native.resize(length);return fs::path(native);
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
      if(entry.th32ProcessID&&entry.th32ProcessID!=excluded&&equal(entry.szExeFile,executable.filename().native())) {
        DWORD candidate_session{};
        if(!ProcessIdToSessionId(entry.th32ProcessID,&candidate_session)){error="A matching process session is inaccessible.";break;}
        if(candidate_session==session) {
          const auto handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,entry.th32ProcessID);
          if(!handle){if(GetLastError()!=ERROR_INVALID_PARAMETER)error="A matching process is inaccessible.";}
          else {
            auto state=std::make_shared<ObservedProcess::State>();state->handle=handle;state->pid=entry.th32ProcessID;
            wchar_t image[32768]{};DWORD length=32768;FILETIME create{},exit{},kernel{},cpu{};
            if(WaitForSingleObject(handle,0)==WAIT_TIMEOUT) {
              if(!QueryFullProcessImageNameW(handle,0,image,&length)||!GetProcessTimes(handle,&create,&exit,&kernel,&cpu))error="A matching process identity is inaccessible.";
              else {
                const auto path=canonical_image(fs::path(std::wstring(image,length)));
                if(path.empty())error="A matching process image cannot be verified.";
                else if(equal(path.native(),executable.native())) {
                  const auto owner=token_user(handle);
                  if(owner.empty())error="The matching process user is inaccessible.";
                  else if(EqualSid(reinterpret_cast<const TOKEN_USER*>(owner.data())->User.Sid,reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid)) {
                    state->creation=stamp(create);result.push_back(std::move(state));
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
#endif
};
ExecutableObservation::ExecutableObservation(const fs::path& executable):impl_(std::make_unique<Impl>()) {
  auto& p=*impl_;
#ifdef _WIN32
  if(!executable.is_absolute()||executable.native().find(wchar_t{})!=std::wstring::npos||executable.native().size()>32768)throw std::runtime_error("Choose a full executable path for Steam game observation.");
  std::error_code error;p.executable=fs::canonical(executable,error);
  if(error||!fs::is_regular_file(p.executable,error)||error||!Impl::equal(p.executable.extension().native(),L".exe"))throw std::runtime_error("Choose an existing Windows game executable for observation.");
  p.executable=Impl::canonical_image(p.executable);if(p.executable.empty())throw std::runtime_error("The game executable's canonical identity is unavailable.");
  if(!ProcessIdToSessionId(GetCurrentProcessId(),&p.session)||(p.user=Impl::token_user(GetCurrentProcess())).empty())throw std::runtime_error("The current user/session identity is unavailable.");
  const auto baseline=p.scan();
  if(!p.error.empty())p.failed=ExecutableMatchState::unavailable;
  else if(!baseline.empty()){p.failed=ExecutableMatchState::rejected;p.error="A matching game was already running before this launch; it was not adopted.";}
  FILETIME now{};GetSystemTimeAsFileTime(&now);p.since=Impl::stamp(now);
#else
  (void)executable;p.failed=ExecutableMatchState::unavailable;p.error="Steam game process observation is unavailable on this platform.";
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
#else
  return {ExecutableMatchState::unavailable,{},p.error};
#endif
}
} // namespace openxmb::integrations
