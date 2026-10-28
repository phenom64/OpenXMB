#include "openxmb/integrations/desktop.hpp"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
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
  ~State() { if (handle) CloseHandle(handle); }
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
#endif
}
OwnedProcess::OwnedProcess(std::shared_ptr<State> state) : state_(std::move(state)) {}
std::int64_t OwnedProcess::id() const noexcept { return state_->pid; }
ProcessObservation OwnedProcess::observe() const {
  std::lock_guard lock(state_->mutex); state_->refresh(); return state_->observation;
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
OwnedProcessLaunch launch_owned_process(const std::vector<std::string>& arguments, const std::filesystem::path& working_directory) {
  if (arguments.empty() || arguments.size() > 256 || arguments.front().empty()) return {{}, "Select a program and at most 256 arguments"};
  std::size_t bytes = 0;
  for (const auto& argument : arguments) {
    if (argument.find('\0') != std::string::npos || argument.size() > 32768) return {{}, "An argument contains NUL or exceeds 32768 bytes"};
    bytes += argument.size();
  }
  if (bytes > 256 * 1024 || working_directory.native().find(typename std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
    return {{}, "Launch arguments or working directory are invalid"};
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
  STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION native_process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP, nullptr,
      working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &native_process))
    return {{}, "Unable to launch program (Windows error " + std::to_string(GetLastError()) + ")"};
  CloseHandle(native_process.hThread); state->handle = native_process.hProcess; state->pid = native_process.dwProcessId;
#else
  std::vector<char*> argv; argv.reserve(arguments.size() + 1);
  for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
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
  if (!error) error = posix_spawnp(&pid, argv.front(), &actions, &attributes, argv.data(), environ);
  posix_spawnattr_destroy(&attributes); posix_spawn_file_actions_destroy(&actions);
  if (error) return {{}, std::strerror(error)};
  state->pid = pid;
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
