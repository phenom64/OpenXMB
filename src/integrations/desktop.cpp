#include "openxmb/integrations/desktop.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <set>
#include <system_error>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <gdiplus.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace openxmb::integrations {
namespace detail { std::uintptr_t capture_return_window(); }
namespace {
std::filesystem::path home_directory() {
#ifdef _WIN32
  if (const char* value = std::getenv("USERPROFILE")) return value;
#else
  if (const char* value = std::getenv("HOME")) return value;
#endif
  return std::filesystem::current_path();
}
std::filesystem::path config_directory() {
  if (const char* value = std::getenv("XDG_CONFIG_HOME")) return value;
  return home_directory() / ".config";
}
std::string desktop_quote(std::string_view value) {
  std::string out{"\""};
  for (const char ch : value) {
    if (ch == '\\' || ch == '"' || ch == '`' || ch == '$') out += '\\';
    // Desktop Entry Exec field expands percent codes even inside quotes.
    if (ch == '%') out += '%';
    out += ch;
  }
  out += '"';
  return out;
}
[[maybe_unused]] std::string xml_quote(std::string_view value) {
  std::string out;
  for (const char ch : value) {
    switch (ch) {
    case '&': out += "&amp;"; break;
    case '<': out += "&lt;"; break;
    case '>': out += "&gt;"; break;
    case '"': out += "&quot;"; break;
    default: out += ch; break;
    }
  }
  return out;
}
#ifdef _WIN32
std::wstring wide(std::string_view value) {
  if (value.empty()) return {};
  const auto size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
  return result;
}
std::wstring windows_quote(const std::wstring& value) {
  std::wstring result{L"\""};
  std::size_t slashes = 0;
  for (const auto ch : value) {
    if (ch == L'\\') { ++slashes; continue; }
    if (ch == L'"') { result.append(slashes * 2 + 1, L'\\'); result += ch; }
    else { result.append(slashes, L'\\'); result += ch; }
    slashes = 0;
  }
  result.append(slashes * 2, L'\\'); result += L'"';
  return result;
}
#endif
Result wait_for_process(const ProcessResult& process) {
  if (!process) return {false, process.message};
#ifdef _WIN32
  const auto handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(process.process_id));
  if (!handle) return {false, "Unable to inspect process"};
  WaitForSingleObject(handle, INFINITE);
  DWORD code{};
  GetExitCodeProcess(handle, &code);
  CloseHandle(handle);
  return {code == 0, code == 0 ? "Completed" : "Program exited with status " + std::to_string(code)};
#else
  int status{};
  pid_t waited{};
  do { waited = waitpid(static_cast<pid_t>(process.process_id), &status, 0); } while (waited == -1 && errno == EINTR);
  if (waited == -1) return {false, std::strerror(errno)};
  return {WIFEXITED(status) && WEXITSTATUS(status) == 0,
          WIFEXITED(status) ? "Program exited with status " + std::to_string(WEXITSTATUS(status)) : "Program was interrupted"};
#endif
}
}

ProcessResult launch_process(const std::vector<std::string>& arguments, const std::filesystem::path& working_directory) {
  if (arguments.empty() || arguments.front().empty()) return {false, -1, "No program was selected"};
  for (const auto& argument : arguments) {
    if (argument.find('\0') != std::string::npos) return {false, -1, "An argument contains an invalid NUL character"};
  }
#ifdef _WIN32
  std::wstring command;
  for (const auto& argument : arguments) {
    if (!command.empty()) command += L' ';
    command += windows_quote(wide(argument));
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const auto cwd = working_directory.wstring();
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &startup, &process))
    return {false, -1, "Unable to launch program (Windows error " + std::to_string(GetLastError()) + ")"};
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
  return {true, static_cast<std::int64_t>(process.dwProcessId), "Program started"};
#else
  int errors[2];
  if (pipe(errors) != 0) return {false, -1, std::strerror(errno)};
  fcntl(errors[1], F_SETFD, FD_CLOEXEC);
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
  const auto pid = fork();
  if (pid < 0) { close(errors[0]); close(errors[1]); return {false, -1, std::strerror(errno)}; }
  if (pid == 0) {
    close(errors[0]);
    setpgid(0, 0);
    if (!working_directory.empty() && chdir(working_directory.c_str()) != 0) {
      const int error = errno; static_cast<void>(write(errors[1], &error, sizeof(error))); _exit(127);
    }
    execvp(argv.front(), argv.data());
    const int error = errno;
    static_cast<void>(write(errors[1], &error, sizeof(error)));
    _exit(127);
  }
  close(errors[1]);
  int error{};
  ssize_t bytes;
  do { bytes = read(errors[0], &error, sizeof(error)); } while (bytes < 0 && errno == EINTR);
  close(errors[0]);
  if (bytes > 0) { int status{}; waitpid(pid, &status, 0); return {false, -1, std::strerror(error)}; }
  return {true, static_cast<std::int64_t>(pid), "Program started"};
#endif
}

Result stop_process(std::int64_t process_id) {
  if (process_id <= 0) return {false, "There is no active session"};
#ifdef _WIN32
  const auto handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(process_id));
  if (!handle) return {false, "The session has already ended"};
  struct WindowSearch { DWORD pid; bool closed; } search{static_cast<DWORD>(process_id), false};
  EnumWindows([](HWND window, LPARAM argument) -> BOOL {
    auto& state = *reinterpret_cast<WindowSearch*>(argument);
    DWORD pid{}; GetWindowThreadProcessId(window, &pid);
    if (pid == state.pid && IsWindowVisible(window)) state.closed = PostMessageW(window, WM_CLOSE, 0, 0) != 0;
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  const bool success = search.closed || TerminateProcess(handle, 0) != 0;
  CloseHandle(handle);
  return {success, success ? "Session closed" : "Unable to close session"};
#else
  const auto pid = static_cast<pid_t>(process_id);
  const bool success = kill(-pid, SIGTERM) == 0 || kill(pid, SIGTERM) == 0;
  if (success) { int status{}; static_cast<void>(waitpid(pid, &status, WNOHANG)); }
  return {success, success ? "Session closed" : "The session has already ended"};
#endif
}
std::optional<int> process_exit_status(std::int64_t process_id) {
  if (process_id <= 0) return -1;
#ifdef _WIN32
  const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(process_id));
  if (!handle) return -1;
  DWORD status{};
  const bool okay = GetExitCodeProcess(handle, &status) != 0;
  CloseHandle(handle);
  if (!okay) return -1;
  if (status == STILL_ACTIVE) return std::nullopt;
  return static_cast<int>(status);
#else
  int status{};
  const auto waited = waitpid(static_cast<pid_t>(process_id), &status, WNOHANG);
  if (waited == 0) return std::nullopt;
  if (waited == -1) return errno == ECHILD ? std::optional<int>{0} : std::optional<int>{-1};
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}
std::optional<std::filesystem::path> find_executable(std::string_view name) {
  const auto executable = [](const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return false;
#ifdef _WIN32
    return true;
#else
    return access(path.c_str(), X_OK) == 0;
#endif
  };
  const std::filesystem::path requested{name};
  if (requested.has_parent_path()) return executable(requested) ? std::optional{requested} : std::nullopt;
  const char* raw = std::getenv("PATH");
  if (!raw) return std::nullopt;
  std::string paths{raw};
#ifdef _WIN32
  constexpr char separator = ';';
#else
  constexpr char separator = ':';
#endif
  std::size_t start{};
  while (start <= paths.size()) {
    const auto end = paths.find(separator, start);
    const auto directory = paths.substr(start, end == std::string::npos ? paths.size() - start : end - start);
    const auto path = std::filesystem::path{directory.empty() ? "." : directory} / requested;
    if (executable(path)) return path;
#ifdef _WIN32
    if (executable(path.string() + ".exe")) return path.string() + ".exe";
#endif
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return std::nullopt;
}
std::filesystem::path executable_path() {
#ifdef _WIN32
  std::wstring buffer(32768, L'\0');
  const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(size); return buffer;
#elif defined(__APPLE__)
  std::uint32_t size{}; _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) == 0) return std::filesystem::weakly_canonical(buffer.c_str());
  return {};
#else
  std::error_code error;
  return std::filesystem::read_symlink("/proc/self/exe", error);
#endif
}
std::filesystem::path user_data_directory() {
  if (const char* value = std::getenv("OPENXMB_DATA_DIR"); value && *value) return value;
#ifdef _WIN32
  if (const char* value = std::getenv("LOCALAPPDATA")) return std::filesystem::path{value} / "OpenXMB";
#elif defined(__APPLE__)
  return home_directory() / "Library/Application Support/OpenXMB";
#endif
  if (const char* value = std::getenv("XDG_DATA_HOME")) return std::filesystem::path{value} / "OpenXMB";
  return home_directory() / ".local/share/OpenXMB";
}
Result validate_browser_url(std::string_view url) {
  if (url.size() > 8192 || url.empty()) return {false, "Enter a web address"};
  if (!url.starts_with("https://") && !url.starts_with("http://")) return {false, "Web addresses must start with https:// or http://"};
  const auto host = url.substr(url.find("://") + 3);
  if (host.empty() || host.front() == '/' || host.front() == '?' || host.front() == '#') return {false, "The web address has no host"};
  for (const unsigned char ch : url) if (ch <= 32 || ch == 127) return {false, "Web addresses cannot contain whitespace or control characters"};
  return {true, "Web address accepted"};
}
Result open_browser(std::string_view url, BrowserSession& session, const std::filesystem::path& profile_directory) {
  if (const auto valid = validate_browser_url(url); !valid) return valid;
  if (session.process_id > 0 && session.dedicated_window && !process_exit_status(session.process_id))
    return {false, "Close the current browser session before opening another"};
  const auto profile = profile_directory.empty() ? user_data_directory() / "browser-profile" : profile_directory;
  std::error_code error; std::filesystem::create_directories(profile, error);
  if (error) return {false, "Cannot create browser profile: " + error.message()};
  std::vector<std::string> args;
  bool dedicated = true;
#ifdef _WIN32
  std::vector<std::filesystem::path> browsers;
  if (const char* configured = std::getenv("OPENXMB_BROWSER"); configured && *configured) browsers.emplace_back(configured);
  else for (const char* variable : {"PROGRAMFILES", "PROGRAMFILES(X86)", "LOCALAPPDATA"}) {
    if (const char* base = std::getenv(variable)) {
      browsers.push_back(std::filesystem::path{base} / "Google/Chrome/Application/chrome.exe");
      browsers.push_back(std::filesystem::path{base} / "Microsoft/Edge/Application/msedge.exe");
      browsers.push_back(std::filesystem::path{base} / "Mozilla Firefox/firefox.exe");
    }
  }
  for (const auto& browser : browsers) if (auto path = find_executable(browser.string())) {
    if (path->filename().string().find("firefox") != std::string::npos) args = {path->string(), "--no-remote", "--profile", profile.string(), "--new-window", std::string{url}};
    else args = {path->string(), "--no-first-run", "--user-data-dir=" + profile.string(), "--app=" + std::string{url}};
    break;
  }
  if (args.empty()) { args = {"rundll32.exe", "url.dll,FileProtocolHandler", std::string{url}}; dedicated = false; }
#elif defined(__APPLE__)
  std::vector<std::filesystem::path> browsers;
  if (const char* configured = std::getenv("OPENXMB_BROWSER"); configured && *configured) browsers.emplace_back(configured);
  else browsers = {"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", "/Applications/Firefox.app/Contents/MacOS/firefox"};
  for (const auto& browser : browsers) if (auto path = find_executable(browser.string())) {
    if (path->filename().string().find("firefox") != std::string::npos) args = {path->string(), "--no-remote", "--profile", profile.string(), "--new-window", std::string{url}};
    else args = {path->string(), "--no-first-run", "--user-data-dir=" + profile.string(), "--app=" + std::string{url}};
    break;
  }
  if (args.empty()) { args = {"open", std::string{url}}; dedicated = false; }
#else
  std::vector<std::string> browsers;
  if (const char* configured = std::getenv("OPENXMB_BROWSER"); configured && *configured) browsers.emplace_back(configured);
  else browsers = {"chromium", "chromium-browser", "google-chrome", "firefox", "firefox-esr"};
  for (const auto& name : browsers) {
    if (auto path = find_executable(name)) {
      if (path->filename().string().find("firefox") != std::string::npos) args = {path->string(), "--no-remote", "--profile", profile.string(), "--new-window", std::string{url}};
      else args = {path->string(), "--no-first-run", "--user-data-dir=" + profile.string(), "--app=" + std::string{url}};
      break;
    }
  }
  if (args.empty() && std::getenv("OPENXMB_BROWSER")) return {false, "The configured browser executable was not found"};
  if (args.empty()) { args = {"xdg-open", std::string{url}}; dedicated = false; }
#endif
  const auto return_window = detail::capture_return_window();
  const auto launched = launch_process(args, profile.parent_path());
  if (!launched) return {false, "Cannot open browser: " + launched.message};
  session = {launched.process_id, dedicated, std::string{url}, return_window, {}};
  return {true, dedicated ? "Browser opened in a separate window; return to OpenXMB to close the session" : "Opened in your system browser; use its own navigation and close controls"};
}
Result close_browser(BrowserSession& session) {
  if (!session.dedicated_window) return {false, "Close your system browser with its own window controls"};
  auto result = stop_process(session.process_id);
  session.process_id = -1;
  session.native_input.reset();
  return result;
}
Result capture_screenshot(const std::filesystem::path& destination) {
  if (destination.empty()) return {false, "Choose a screenshot destination"};
  std::error_code error;
  if (destination.has_parent_path()) std::filesystem::create_directories(destination.parent_path(), error);
  if (error) return {false, error.message()};
#ifdef __APPLE__
  auto result = wait_for_process(launch_process({"screencapture", "-x", destination.string()}));
#elif defined(_WIN32)
  Gdiplus::GdiplusStartupInput startup;
  ULONG_PTR token{};
  if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) return {false, "Cannot initialize the screenshot encoder"};
  const auto screen = GetDC(nullptr);
  const auto memory = CreateCompatibleDC(screen);
  const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  const auto bitmap = CreateCompatibleBitmap(screen, width, height);
  const auto previous = SelectObject(memory, bitmap);
  const bool captured = BitBlt(memory, 0, 0, width, height, screen, GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), SRCCOPY | CAPTUREBLT) != 0;
  bool saved = false;
  if (captured) {
    const CLSID png_encoder{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0, 0, 0xf8, 0x1e, 0xf3, 0x2e}};
    Gdiplus::Bitmap image(bitmap, nullptr);
    saved = image.Save(destination.wstring().c_str(), &png_encoder, nullptr) == Gdiplus::Ok;
  }
  SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, screen);
  Gdiplus::GdiplusShutdown(token);
  return {saved, saved ? "Screenshot saved to " + destination.string() : "Unable to capture the desktop"};
#else
  auto tool = find_executable("import");
  if (!tool) return {false, "Install ImageMagick to enable desktop screenshots"};
  auto result = wait_for_process(launch_process({tool->string(), "-silent", "-window", "root", "PNG24:" + destination.string()}));
#endif
#ifndef _WIN32
  if (!result) return result;
  if (!std::filesystem::is_regular_file(destination, error) || std::filesystem::file_size(destination, error) == 0)
    return {false, "The screenshot tool did not create an image"};
  return {true, "Screenshot saved to " + destination.string()};
#endif
}
Result set_autostart(bool enabled, const std::filesystem::path& executable, const std::filesystem::path& configuration_root) {
  if (executable.empty()) return {false, "The OpenXMB executable path is unavailable"};
  if (executable.string().find_first_of("\r\n") != std::string::npos) return {false, "Executable path contains a line break"};
#ifdef _WIN32
  if (!configuration_root.empty()) return {false, "Windows autostart uses the current-user Run registry key"};
  HKEY key{};
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    return {false, "Unable to open the current-user startup registry key"};
  auto command = windows_quote(executable.wstring());
  const auto result = enabled ? RegSetValueExW(key, L"OpenXMB", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) : RegDeleteValueW(key, L"OpenXMB");
  RegCloseKey(key);
  return {result == ERROR_SUCCESS || (!enabled && result == ERROR_FILE_NOT_FOUND), enabled ? "OpenXMB starts at login" : "Startup at login disabled"};
#else
#ifdef __APPLE__
  const auto root = configuration_root.empty() ? home_directory() / "Library" : configuration_root;
  const auto file = root / "LaunchAgents/com.openxmb.shell.plist";
#else
  const auto root = configuration_root.empty() ? config_directory() : configuration_root;
  const auto file = root / "autostart/openxmb.desktop";
#endif
  std::error_code error;
  if (!enabled) { std::filesystem::remove(file, error); return {!error, error ? error.message() : "Startup at login disabled"}; }
  if (!std::filesystem::is_regular_file(executable, error)) return {false, "The selected executable does not exist"};
  std::filesystem::create_directories(file.parent_path(), error);
  if (error) return {false, error.message()};
  std::ofstream stream(file, std::ios::trunc);
#ifdef __APPLE__
  stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?><!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\"><plist version=\"1.0\"><dict><key>Label</key><string>com.openxmb.shell</string><key>ProgramArguments</key><array><string>" << xml_quote(executable.string()) << "</string></array><key>RunAtLoad</key><true/></dict></plist>\n";
#else
  stream << "[Desktop Entry]\nType=Application\nVersion=1.0\nName=OpenXMB\nComment=OpenXMB desktop shell\nExec=" << desktop_quote(std::filesystem::absolute(executable).string()) << "\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
#endif
  stream.close();
  return {!stream.fail(), stream.fail() ? "Unable to save autostart configuration" : "OpenXMB starts at login"};
#endif
}
bool autostart_enabled(const std::filesystem::path& configuration_root) {
#ifdef _WIN32
  HKEY key{}; DWORD bytes{};
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
  const auto result = RegQueryValueExW(key, L"OpenXMB", nullptr, nullptr, nullptr, &bytes);
  RegCloseKey(key); return result == ERROR_SUCCESS && bytes > 0;
#elif defined(__APPLE__)
  const auto root = configuration_root.empty() ? home_directory() / "Library" : configuration_root;
  return std::filesystem::is_regular_file(root / "LaunchAgents/com.openxmb.shell.plist");
#else
  const auto root = configuration_root.empty() ? config_directory() : configuration_root;
  return std::filesystem::is_regular_file(root / "autostart/openxmb.desktop");
#endif
}
Preferences load_preferences(const std::filesystem::path& directory) {
  Preferences value;
  const auto root = directory.empty() ? user_data_directory() : directory;
  std::ifstream file(root / "integrations.conf");
  std::string core, content;
  if (file >> std::quoted(value.browser_url) >> std::quoted(core) >> std::quoted(content)) { value.core = core; value.content = content; }
  return value;
}
Result save_preferences(const Preferences& preferences, const std::filesystem::path& directory) {
  const auto root = directory.empty() ? user_data_directory() : directory;
  std::error_code error; std::filesystem::create_directories(root, error);
  if (error) return {false, error.message()};
  std::ofstream file(root / "integrations.conf", std::ios::trunc);
  file << std::quoted(preferences.browser_url) << '\n' << std::quoted(preferences.core.string()) << '\n' << std::quoted(preferences.content.string()) << '\n';
  file.close();
  return {!file.fail(), file.fail() ? "Unable to save preferences" : "Preferences saved"};
}
std::vector<std::filesystem::path> discover_libretro_cores() {
  std::vector<std::filesystem::path> roots{user_data_directory() / "cores", "/usr/lib/libretro", "/usr/lib/x86_64-linux-gnu/libretro", "/usr/local/lib/libretro", executable_path().parent_path() / "cores"};
  if (const char* value = std::getenv("OPENXMB_CORE_DIR")) roots.insert(roots.begin(), value);
  std::set<std::filesystem::path> found;
  for (const auto& root : roots) {
    std::error_code error;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
      if (!it->is_regular_file(error)) continue;
      const auto extension = it->path().extension().string();
      if (extension == ".so" || extension == ".dylib" || extension == ".dll") found.insert(it->path());
    }
  }
  return {found.begin(), found.end()};
}
ProcessResult launch_libretro(const std::filesystem::path& core, const std::filesystem::path& content) {
  if (!std::filesystem::is_regular_file(core)) return {false, -1, "Select an installed libretro core library"};
  auto executable = executable_path().parent_path() /
#ifdef _WIN32
      "openxmb-retro.exe";
#else
      "openxmb-retro";
#endif
  if (const char* value = std::getenv("OPENXMB_RETRO_HOST")) executable = value;
  if (!std::filesystem::is_regular_file(executable)) return {false, -1, "The native libretro host is not installed beside OpenXMB"};
  const auto absolute_core = std::filesystem::absolute(core);
  const auto absolute_content = content.empty() ? std::filesystem::path{} : std::filesystem::absolute(content);
  std::vector<std::string> args{executable.string(), "--core", absolute_core.string()};
  if (!absolute_content.empty()) args.insert(args.end(), {"--content", absolute_content.string()});
  return launch_process(args, absolute_content.empty() ? absolute_core.parent_path() : absolute_content.parent_path());
}
} // namespace openxmb::integrations
