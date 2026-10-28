#include "openxmb/integrations/desktop.hpp"
#include <nlohmann/json.hpp>

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
#include <atomic>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <gdiplus.h>
#include "browser_ipc_win.hpp"
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
  for (;;) {
    if (const auto code = process_exit_status(process.process_id))
      return {*code == 0, *code == 0 ? "Completed" : "Program exited with status " + std::to_string(*code)};
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
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
std::string browser_address_url(std::string_view text,std::string_view search_engine) {
  while(!text.empty()&&text.front()==' ') text.remove_prefix(1);
  while(!text.empty()&&text.back()==' ') text.remove_suffix(1);
  if(text.empty()||text.size()>8192) return {};
  for(const unsigned char ch:text) if(ch<32||ch==127) return {};
  if(text.starts_with("http://")||text.starts_with("https://")) return std::string(text);
  if(text.find(' ')==std::string_view::npos&&text.find(':')==std::string_view::npos&&
     text.find('.')!=std::string_view::npos) return "https://"+std::string(text);
  std::string encoded;
  constexpr char hex[]="0123456789ABCDEF";
  for(const unsigned char ch:text) {
    if((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='-'||ch=='_'||ch=='.'||ch=='~') encoded+=ch;
    else {encoded+='%';encoded+=hex[ch>>4];encoded+=hex[ch&15];}
  }
  std::string engine(search_engine);
  std::ranges::transform(engine,engine.begin(),[](unsigned char ch) {return static_cast<char>(std::tolower(ch));});
  const std::string base=engine=="google"?"https://www.google.com/search?q=":
    engine=="bing"?"https://www.bing.com/search?q=":"https://duckduckgo.com/?q=";
  return base+encoded;
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
  const auto return_window = detail::capture_return_window();
  bool embedded=false;
  const auto state_file=profile/"page-state.json";
#ifdef _WIN32
  if (!std::getenv("OPENXMB_BROWSER")) {
    const auto parent = reinterpret_cast<HWND>(return_window);
    DWORD owner{}; GetWindowThreadProcessId(parent, &owner);
    if (!IsWindow(parent) || owner != GetCurrentProcessId())
      return {false, "Select the OpenXMB window before opening its embedded browser"};
    const auto helper = executable_path().parent_path()/"openxmb-web.exe";
    if (!std::filesystem::is_regular_file(helper, error))
      return {false, "The embedded browser helper openxmb-web.exe is missing. Reinstall OpenXMB with Windows browser support."};
    std::filesystem::remove(state_file,error);
    std::filesystem::remove(profile/"page-command.json",error);
    std::wstring command = windows_quote(helper.wstring()) + L" " + windows_quote(wide(url)) + L" " +
      std::to_wstring(return_window) + L" " + windows_quote(profile.wstring()) + L" " + windows_quote(state_file.wstring());
    STARTUPINFOW startup{}; startup.cb=sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))
      return {false,"Cannot start the embedded browser (Windows error " + std::to_string(GetLastError()) + ")"};
    CloseHandle(process.hThread);
    // Permit the child to transfer keyboard focus into its WebView when ready.
    AllowSetForegroundWindow(process.dwProcessId);
    session = {static_cast<std::int64_t>(process.dwProcessId),true,std::string(url),return_window,{},true,state_file,
      std::shared_ptr<void>(process.hProcess,[](void* handle) { CloseHandle(handle); })};
    return {true,"Opening embedded browser"};
  }
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
  if(return_window&&!std::getenv("OPENXMB_BROWSER")) {
    if(auto helper=find_executable((executable_path().parent_path()/"openxmb-web").string())) {
      args={helper->string(),std::string(url),std::to_string(return_window),profile.string(),state_file.string()};
      embedded=true;
      std::error_code remove_error;std::filesystem::remove(state_file,remove_error);std::filesystem::remove(profile/"page-command.json",remove_error);
    }
  }
#endif
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
  if(args.empty()) for (const auto& name : browsers) {
    if (auto path = find_executable(name)) {
      if (path->filename().string().find("firefox") != std::string::npos) args = {path->string(), "--no-remote", "--profile", profile.string(), "--new-window", std::string{url}};
      else args = {path->string(), "--no-first-run", "--user-data-dir=" + profile.string(), "--app=" + std::string{url}};
      break;
    }
  }
  if (args.empty() && std::getenv("OPENXMB_BROWSER")) return {false, "The configured browser executable was not found"};
  if (args.empty()) { args = {"xdg-open", std::string{url}}; dedicated = false; }
#endif
  const auto launched = launch_process(args, profile.parent_path());
  if (!launched) return {false, "Cannot open browser: " + launched.message};
  session = {launched.process_id, dedicated, std::string{url}, return_window, {},embedded,state_file};
  if(embedded) return {true,"Browser opened"};
  return {true, dedicated ? "Browser opened in a separate window; return to OpenXMB to close the session" : "Opened in your system browser; use its own navigation and close controls"};
}
BrowserPage browser_page(const BrowserSession& session) {
  BrowserPage page{.url=session.url};
  if(!session.embedded_view||session.state_file.empty()) return page;
  std::error_code error;const auto bytes=std::filesystem::file_size(session.state_file,error);
  if(error||bytes>65536) return page;
  try {
#ifdef _WIN32
    const auto state=detail::read_browser_json(session.state_file,65536);
#else
    std::ifstream file(session.state_file);const auto state=nlohmann::json::parse(file);
#endif
    page.title=state.value("title",std::string{});page.url=state.value("uri",session.url);
    page.hover=state.value("hover",std::string{});page.loading=state.value("loading",false);page.address_requested=state.value("address_requested",false);
    page.back=state.value("back",false);page.forward=state.value("forward",false);
    page.ready=state.value("ready",false);page.error=state.value("error",std::string{});
    page.options_requested=state.value("options_requested",false);
    page.status=state.value("status",std::string{});
    page.edit_token=state.value("edit_token",std::uint64_t{});page.edit_requested=state.value("edit_requested",false);
    page.edit_password=state.value("edit_password",false);page.edit_multiline=state.value("edit_multiline",false);
    page.edit_label=state.value("edit_label",std::string{});
    if(!page.edit_password) page.edit_value=state.value("edit_value",std::string{});
  }catch(...) {}
  return page;
}
Result browser_view_command(BrowserSession& session,std::string_view command,std::string_view value) {
  if(!session.embedded_view) return {false,"An embedded browser view is not available"};
  if(command=="navigate") {const auto result=validate_browser_url(value);if(!result) return result;}
  else if(command!="hide"&&command!="show"&&command!="back"&&command!="forward"&&command!="reload"&&command!="text"
#ifdef _WIN32
    &&command!="close"&&command!="input"&&command!="address"&&command!="return"&&command!="configure"&&command!="clear_site_data"&&command!="menu"&&command!="edit"
#endif
    ) return {false,"Unknown browser command"};
  if(value.size()>16384) return {false,"Browser command is too long"};
  static std::atomic<std::int64_t> sequence{};
  nlohmann::json message{{"sequence",++sequence},{"command",command},{"value",value}};
  const auto destination=session.state_file.parent_path()/"page-command.json";auto temporary=destination;temporary+=".tmp";
#ifdef _WIN32
  // Preserve commands issued between helper timer ticks (e.g. hide then return,
  // or pointer then click). The latest envelope remains Linux-protocol compatible.
  nlohmann::json commands=nlohmann::json::array();
  try {
    std::int64_t acknowledged{};
    const auto state=detail::read_browser_json(session.state_file);
    if(state.is_object()) acknowledged=state.value("command_sequence",std::int64_t{});
    const auto previous=detail::read_browser_json(destination);
    if(previous.is_object()) {
      for(const auto& pending:previous.value("commands",nlohmann::json::array()))
        if(pending.value("sequence",std::int64_t{})>acknowledged) commands.push_back(pending);
    }
  }catch(...) {}
  if(commands.size()>=256) return {false,"The browser is not responding to commands"};
  commands.push_back(message);message["commands"]=std::move(commands);
#endif
  std::string encoded;
  try {encoded=message.dump();}catch(const nlohmann::json::exception&) {return {false,"Browser command contains invalid UTF-8 text"};}
  if(encoded.size()>262144) return {false,"The browser command queue is full"};
  std::ofstream file(temporary,std::ios::binary|std::ios::trunc);file<<encoded;file.close();
  if(!file) return {false,"Unable to update the browser view"};
  std::error_code error;
#ifdef _WIN32
  if(!detail::replace_browser_file(temporary,destination))
    error=std::error_code(GetLastError(),std::system_category());
#else
  std::filesystem::rename(temporary,destination,error);
#endif
  if(!error&&command=="navigate") session.url=value;
  return {!error,error?error.message():std::string{}};
}
Result close_browser(BrowserSession& session) {
  if (!session.dedicated_window) return {false, "Close your system browser with its own window controls"};
#ifdef _WIN32
  if(session.embedded_view&&session.native_process) {
    const auto handle=static_cast<HANDLE>(session.native_process.get());
    auto result=browser_view_command(session,"close");
    // Closing the controller first allows the runtime to flush cookies/profile
    // and release its processes. Force only an unresponsive helper to exit.
    // Closing WebView2/focusing the shell can send synchronous native messages
    // across UI threads. Service sent messages while waiting, without dispatching
    // queued user input that could re-enter this closing browser component.
    const auto deadline=GetTickCount64()+3000;
    DWORD waited=WAIT_TIMEOUT;
    while((waited=WaitForSingleObject(handle,0))==WAIT_TIMEOUT&&GetTickCount64()<deadline) {
      MsgWaitForMultipleObjects(1,&handle,FALSE,20,QS_SENDMESSAGE);
      MSG sent{};PeekMessageW(&sent,nullptr,WM_NULL,WM_NULL,PM_NOREMOVE);
    }
    if(waited==WAIT_TIMEOUT) {
      const auto forced=TerminateProcess(handle,1)!=FALSE;
      if(forced) WaitForSingleObject(handle,1000);
      result={forced,forced?"Unresponsive browser session stopped":"Unable to stop the browser session"};
    } else result={true,"Browser session closed"};
    const auto parent=reinterpret_cast<HWND>(session.return_window);
    if(IsWindow(parent)) {SetForegroundWindow(parent);SetFocus(parent);}
    session.process_id=-1;session.embedded_view=false;session.native_input.reset();session.native_process.reset();
    return result;
  }
#endif
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
