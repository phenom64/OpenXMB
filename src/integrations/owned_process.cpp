#include "openxmb/integrations/desktop.hpp"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#ifdef __linux__
#include "linux_process_identity.hpp"
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace openxmb::integrations {
struct OwnedProcess::State {
  std::mutex mutex;
  std::int64_t pid{-1};
  ProcessObservation observation{ProcessState::running, {}, {}};
#ifdef _WIN32
  HANDLE handle{};
  std::uint64_t creation{};
  ~State() { if (handle) CloseHandle(handle); }
#endif
#ifdef __linux__
  std::uint64_t creation{};
#endif
  void refresh() {
    if (observation.state != ProcessState::running) return;
#ifdef _WIN32
    const auto waited = WaitForSingleObject(handle, 0);
    if (waited == WAIT_TIMEOUT) return;
    DWORD code{};
    if (waited == WAIT_OBJECT_0 && GetExitCodeProcess(handle, &code)) observation = {ProcessState::exited, static_cast<int>(code), {}};
    else observation = {ProcessState::unknown, {}, "Unable to observe owned process exit"};
#else
    int status{};
    pid_t result;
    do { result = waitpid(static_cast<pid_t>(pid), &status, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result == 0) return;
    if (result == static_cast<pid_t>(pid)) {
      const auto code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
      observation = {ProcessState::exited, code, {}};
    } else observation = {ProcessState::unknown, {}, "Owned child exit status is unavailable; another observer may have reaped it"};
#endif
  }
};
namespace {
// Process lifetime ownership outlives any UI/SessionManager. A single bounded
// polling service reaps abandoned children; releasing a capability never kills
// the child or waits for it. Heap lifetime avoids shutdown-order races between
// application services and this detached OS-resource observer.
struct ProcessTracker {
  std::mutex mutex;
  std::vector<std::shared_ptr<OwnedProcess::State>> running;
  std::map<std::int64_t, std::shared_ptr<OwnedProcess>> legacy;
  ProcessTracker() {
    std::thread([this] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        std::lock_guard lock(mutex);
        std::erase_if(running, [](const auto& state) {
          std::lock_guard process_lock(state->mutex); state->refresh();
          return state->observation.state != ProcessState::running;
        });
        if (legacy.size() > 1024) {
          for (auto it = legacy.begin(); it != legacy.end() && legacy.size() > 1024;) {
            if (it->second->observe().state != ProcessState::running) it = legacy.erase(it); else ++it;
          }
        }
      }
    }).detach();
  }
};
ProcessTracker& tracker() { static auto* value = new ProcessTracker; return *value; }
bool utf8_value(std::string_view value) {
  for(std::size_t at=0;at<value.size();) {
    const auto lead=static_cast<unsigned char>(value[at++]);if(lead==0)return false;if(lead<0x80)continue;
    const unsigned count=lead>=0xc2&&lead<=0xdf?2:lead>=0xe0&&lead<=0xef?3:lead>=0xf0&&lead<=0xf4?4:0;
    if(!count||value.size()-at<count-1)return false;
    std::uint32_t point=lead&(0x7f>>count);
    for(unsigned i=1;i<count;++i){const auto next=static_cast<unsigned char>(value[at++]);if((next&0xc0)!=0x80)return false;point=(point<<6)|(next&63);}
    if((count==2&&point<0x80)||(count==3&&point<0x800)||(count==4&&point<0x10000)||point>0x10ffff||(point>=0xd800&&point<=0xdfff))return false;
  }
  return true;
}
std::string validate_environment(const std::map<std::string,std::string>& overrides) {
  if(overrides.size()>64)return "At most 64 child environment overrides are supported";
  std::size_t bytes{};
#ifdef _WIN32
  std::set<std::string> names;
#endif
  for(const auto& [name,value]:overrides) {
    const auto letter=[](unsigned char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_';};
    if(name.empty()||name.size()>128||!letter(name.front())||!std::ranges::all_of(name,[&](unsigned char c){return letter(c)||(c>='0'&&c<='9');}))return "Child environment names must be ASCII identifiers of at most 128 bytes";
    if(value.size()>32768||!utf8_value(value))return "Child environment values must be valid UTF-8 without NUL and at most 32768 bytes";
    bytes+=name.size()+value.size()+2;if(bytes>256*1024)return "Child environment overrides exceed 256 KiB";
#ifdef _WIN32
    auto folded=name;for(auto& c:folded)if(c>='A'&&c<='Z')c+=('a'-'A');
    if(!names.insert(std::move(folded)).second)return "Windows child environment names must not differ only by case";
#endif
  }
  return {};
}
#ifdef _WIN32
bool wide_argument(const std::string& value, std::wstring& result) {
  if (value.empty()) { result.clear(); return true; }
  const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (!size) return false;
  result.resize(static_cast<std::size_t>(size));
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size) != 0;
}
std::wstring quote(const std::wstring& value) {
  std::wstring result{L"\""}; std::size_t slashes = 0;
  for (const auto character : value) {
    if (character == L'\\') { ++slashes; continue; }
    result.append(slashes * (character == L'"' ? 2 : 1) + (character == L'"' ? 1 : 0), L'\\');
    slashes = 0; result += character;
  }
  result.append(slashes * 2, L'\\'); result += L'"'; return result;
}
struct EnvironmentNameLess {
  bool operator()(const std::wstring& a,const std::wstring& b)const{return CompareStringOrdinal(a.data(),static_cast<int>(a.size()),b.data(),static_cast<int>(b.size()),TRUE)==CSTR_LESS_THAN;}
};
bool child_environment(const std::map<std::string,std::string>& overrides,std::vector<wchar_t>& block) {
  auto* inherited=GetEnvironmentStringsW();if(!inherited)return false;
  std::unique_ptr<wchar_t,decltype(&FreeEnvironmentStringsW)> release(inherited,&FreeEnvironmentStringsW);
  std::map<std::wstring,std::wstring,EnvironmentNameLess> values;
  constexpr std::size_t limit=4*1024*1024;std::size_t bytes{};
  for(auto* entry=inherited;*entry;) {
    const auto length=std::char_traits<wchar_t>::length(entry);bytes+=length+1;if(bytes>limit)return false;
    const std::wstring_view line(entry,length);const auto equal=line.find(L'=',line.starts_with(L'=')?1:0);
    // Retain Windows' hidden per-drive current directories (=C:=...), whose
    // leading '=' is allowed only in inherited entries, never an override.
    if(equal==line.npos)return false;
    values[std::wstring(line.substr(0,equal))]=line.substr(equal+1);entry+=length+1;
  }
  for(const auto& [name,value]:overrides){std::wstring key,converted;if(!wide_argument(name,key)||!wide_argument(value,converted)||key.size()+converted.size()+2>32767)return false;values[std::move(key)]=std::move(converted);}
  for(const auto& [name,value]:values){if(block.size()+name.size()+value.size()+2>limit)return false;block.insert(block.end(),name.begin(),name.end());block.push_back(L'=');block.insert(block.end(),value.begin(),value.end());block.push_back(L'\0');}
  block.push_back(L'\0');if(block.size()==1)block.push_back(L'\0');return true;
}
#else
bool child_environment(const std::map<std::string,std::string>& overrides,std::vector<std::string>& values,std::vector<char*>& pointers) {
  std::size_t bytes{},count{};
  for(auto** entry=environ;entry&&*entry;++entry){const auto length=std::strlen(*entry);bytes+=length+1;if(++count>65536||bytes>4*1024*1024)return false;
    const std::string_view line(*entry,length);const auto equal=line.find('=');
    if(equal==line.npos||!overrides.contains(std::string(line.substr(0,equal))))values.emplace_back(line);
  }
  for(const auto& [name,value]:overrides)values.push_back(name+'='+value);
  pointers.reserve(values.size()+1);for(auto& value:values)pointers.push_back(value.data());pointers.push_back(nullptr);return true;
}
#endif
}
OwnedProcess::OwnedProcess(std::shared_ptr<State> state) : state_(std::move(state)) {}
std::int64_t OwnedProcess::id() const noexcept { return state_->pid; }
ProcessObservation OwnedProcess::observe() const {
  std::lock_guard lock(state_->mutex); state_->refresh();
  auto observation=state_->observation;
#if defined(_WIN32) || defined(__linux__)
  observation.process_creation=state_->creation;
#endif
  return observation;
}
Result OwnedProcess::stop(ProcessStopMode mode) const {
  std::lock_guard lock(state_->mutex); state_->refresh();
  if (state_->observation.state != ProcessState::running) return {false, "The owned process has already ended or its identity is unavailable"};
#ifdef _WIN32
  if (mode == ProcessStopMode::force_stop) {
    const bool okay = TerminateProcess(state_->handle, 1) != FALSE;
    return {okay, okay ? "Force stop requested for owned process" : "Unable to force stop owned process"};
  }
  struct Windows { DWORD pid; bool requested{}; } windows{static_cast<DWORD>(state_->pid)};
  EnumWindows([](HWND window, LPARAM value) -> BOOL {
    auto& windows = *reinterpret_cast<Windows*>(value);
    DWORD pid{}; GetWindowThreadProcessId(window, &pid);
    if (pid == windows.pid && PostMessageW(window, WM_CLOSE, 0, 0)) windows.requested = true;
    return TRUE;
  }, reinterpret_cast<LPARAM>(&windows));
  return {windows.requested, windows.requested ? "Window close requested; the application may ask to save work" : "The owned process has no closeable window; force stop is a separate action"};
#else
  // refresh/reap and signalling share this mutex. The live unreaped child pins
  // its PID, and its launch-created group is never addressed after child exit.
  const bool okay = kill(-static_cast<pid_t>(state_->pid), mode == ProcessStopMode::force_stop ? SIGKILL : SIGTERM) == 0;
  return {okay, okay ? (mode == ProcessStopMode::force_stop ? "Force stop requested for owned process group" : "Termination requested for owned process group") : "Unable to signal owned process group"};
#endif
}
OwnedProcessLaunch launch_owned_process(const std::vector<std::string>& arguments, const std::filesystem::path& working_directory,
                                        const std::map<std::string,std::string>& environment_overrides,
                                        const std::vector<std::uintptr_t>& inherited_handles) {
  if(inherited_handles.size()>16)return {{},"At most 16 selected file handles may be inherited"};
#ifndef _WIN32
  if(!inherited_handles.empty())return {{},"Selected child handle inheritance is unavailable on this platform"};
#endif
  if (arguments.empty() || arguments.size() > 256 || arguments.front().empty()) return {{}, "Select a program and at most 256 arguments"};
  std::size_t bytes = 0;
  for (const auto& argument : arguments) {
    if (argument.find('\0') != std::string::npos || argument.size() > 32768) return {{}, "An argument contains NUL or exceeds 32768 bytes"};
    bytes += argument.size();
  }
  if (bytes > 256 * 1024 || working_directory.native().find(typename std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
    return {{}, "Launch arguments or working directory are invalid"};
  if(const auto error=validate_environment(environment_overrides);!error.empty())return {{},error};
  auto& service = tracker();
  { std::lock_guard lock(service.mutex); if (service.running.size() >= 256) return {{}, "Too many tracked processes are active"}; }
  auto state = std::make_shared<OwnedProcess::State>();
#ifdef _WIN32
  std::wstring command, converted;
  for (const auto& argument : arguments) {
    if (!wide_argument(argument, converted)) return {{}, "A launch argument is not valid UTF-8"};
    if (!command.empty()) command += L' ';
    command += quote(converted);
  }
  if (command.size() >= 32767) return {{}, "Windows launch command exceeds 32766 characters"};
  std::vector<wchar_t> environment;
  if(!environment_overrides.empty()&&!child_environment(environment_overrides,environment))return {{},"Unable to merge the bounded Windows child environment"};
  struct ChildHandles {
    std::vector<HANDLE> files;
    std::vector<unsigned char> attributes;
    LPPROC_THREAD_ATTRIBUTE_LIST list{};
    ~ChildHandles(){if(list)DeleteProcThreadAttributeList(list);for(auto file:files)CloseHandle(file);}
  } selected;
  for(const auto raw:inherited_handles) {
    const auto file=reinterpret_cast<HANDLE>(raw);DWORD flags{};
    if(!raw||file==INVALID_HANDLE_VALUE||!GetHandleInformation(file,&flags)||GetFileType(file)!=FILE_TYPE_DISK)
      return {{},"Only valid selected disk-file handles may be inherited"};
    HANDLE duplicate{};
    if(!DuplicateHandle(GetCurrentProcess(),file,GetCurrentProcess(),&duplicate,0,TRUE,DUPLICATE_SAME_ACCESS))
      return {{},"Windows could not duplicate a selected child file handle"};
    selected.files.push_back(duplicate);
  }
  if(!selected.files.empty()) {
    SIZE_T size{};InitializeProcThreadAttributeList(nullptr,1,0,&size);
    if(!size)return {{},"Windows could not size the restricted child handle list"};
    selected.attributes.resize(size);
    const auto list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(selected.attributes.data());
    if(!InitializeProcThreadAttributeList(list,1,0,&size))return {{},"Windows could not prepare the restricted child handle list"};
    selected.list=list;
    if(!UpdateProcThreadAttribute(selected.list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,selected.files.data(),selected.files.size()*sizeof(HANDLE),nullptr,nullptr))
      return {{},"Windows could not restrict the inherited child handles"};
  }
  STARTUPINFOEXW startup{};startup.StartupInfo.cb=selected.list?sizeof(startup):sizeof(STARTUPINFOW);
  startup.lpAttributeList=selected.list;PROCESS_INFORMATION native_process{};
  const DWORD creation_flags=CREATE_NEW_PROCESS_GROUP|(environment.empty()?0:CREATE_UNICODE_ENVIRONMENT)|(selected.list?EXTENDED_STARTUPINFO_PRESENT:0);
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, selected.list!=nullptr, creation_flags, environment.empty()?nullptr:environment.data(),
      working_directory.empty() ? nullptr : working_directory.c_str(), &startup.StartupInfo, &native_process))
    return {{}, "Unable to launch program (Windows error " + std::to_string(GetLastError()) + ")"};
  CloseHandle(native_process.hThread); state->handle = native_process.hProcess; state->pid = native_process.dwProcessId;
  FILETIME created{},exited{},kernel{},user{};
  if(GetProcessTimes(state->handle,&created,&exited,&kernel,&user))state->creation=(std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
#else
  std::vector<char*> argv; argv.reserve(arguments.size() + 1);
  for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
  std::vector<std::string> environment_values;std::vector<char*> environment;
  if(!environment_overrides.empty()&&!child_environment(environment_overrides,environment_values,environment))return {{},"Unable to merge the bounded POSIX child environment"};
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  if (error) return {{}, std::strerror(error)};
  posix_spawnattr_t attributes;
  error = posix_spawnattr_init(&attributes);
  if (error) { posix_spawn_file_actions_destroy(&actions); return {{}, std::strerror(error)}; }
  if (!working_directory.empty()) error = posix_spawn_file_actions_addchdir_np(&actions, working_directory.c_str());
  if (!error) error = posix_spawnattr_setpgroup(&attributes, 0);
  if (!error) error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  pid_t pid{};
  if (!error) error = posix_spawnp(&pid, argv.front(), &actions, &attributes, argv.data(), environment.empty()?environ:environment.data());
  posix_spawnattr_destroy(&attributes); posix_spawn_file_actions_destroy(&actions);
  if (error) return {{}, std::strerror(error)};
  state->pid = pid;
#ifdef __linux__
  state->creation=linux_detail::process_creation(pid).value_or(0);
#endif
#endif
  auto process = std::shared_ptr<OwnedProcess>(new OwnedProcess(state));
  { std::lock_guard lock(service.mutex); service.running.push_back(std::move(state)); }
  return {std::move(process), "Program started"};
}
ProcessResult launch_process(const std::vector<std::string>& arguments, const std::filesystem::path& working_directory) {
  auto launched = launch_owned_process(arguments, working_directory);
  if (!launched) return {false, -1, launched.message};
  const auto id = launched.process->id();
  { auto& service = tracker(); std::lock_guard lock(service.mutex); service.legacy[id] = std::move(launched.process); }
  return {true, id, launched.message};
}
Result stop_process(std::int64_t process_id) {
  std::shared_ptr<OwnedProcess> process;
  { auto& service = tracker(); std::lock_guard lock(service.mutex); const auto found = service.legacy.find(process_id); if (found != service.legacy.end()) process = found->second; }
  if (!process) return {false, "This PID is not an owned legacy process; use its session capability"};
  return process->stop();
}
std::optional<int> process_exit_status(std::int64_t process_id) {
  if (process_id <= 0) return -1;
  std::shared_ptr<OwnedProcess> process;
  { auto& service = tracker(); std::lock_guard lock(service.mutex); const auto found = service.legacy.find(process_id); if (found != service.legacy.end()) process = found->second; }
  if (process) {
    const auto observation = process->observe();
    if (observation.state == ProcessState::running) return {};
    return observation.exit_code.value_or(-1);
  }
#ifdef _WIN32
  // Embedded browser helpers retain their own native handle and predate this
  // launcher. Keep their read-only PID observation contract; never adopt them
  // for stop_process, and never use this fallback for new session ownership.
  const auto handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(process_id));
  if (!handle) return -1;
  const auto waited = WaitForSingleObject(handle, 0); DWORD code{};
  const bool okay = waited == WAIT_OBJECT_0 && GetExitCodeProcess(handle, &code);
  CloseHandle(handle);
  if (waited == WAIT_TIMEOUT) return {};
  return okay ? static_cast<int>(code) : -1;
#else
  return -1; // ECHILD/unknown ownership is never fabricated as successful exit.
#endif
}
} // namespace openxmb::integrations
