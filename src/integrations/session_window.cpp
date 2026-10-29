#include "openxmb/integrations/session_window.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <limits>
#include <vector>

namespace openxmb::integrations {
namespace {
std::optional<std::uint64_t> creation_time(std::int64_t pid) {
  if (pid <= 0 || pid > std::numeric_limits<DWORD>::max()) return {};
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (!process) return {};
  FILETIME create{}, exit{}, kernel{}, user{};
  const bool valid = WaitForSingleObject(process, 0) == WAIT_TIMEOUT && GetProcessTimes(process, &create, &exit, &kernel, &user);
  CloseHandle(process);
  if (!valid) return {};
  return (std::uint64_t(create.dwHighDateTime) << 32) | create.dwLowDateTime;
}
bool eligible(HWND hwnd, DWORD pid) {
  DWORD owner{}; GetWindowThreadProcessId(hwnd, &owner);
  if (owner != pid || !IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd || GetWindow(hwnd, GW_OWNER)) return false;
  if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return false;
  wchar_t name[128]{}; if (!GetClassNameW(hwnd, name, 128) || lstrcmpiW(name, L"ConsoleWindowClass") == 0) return false;
  DWORD cloaked{}; if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked) return false;
  RECT rect{}; return GetClientRect(hwnd, &rect) && rect.right > rect.left && rect.bottom > rect.top;
}
bool valid_target(const SessionWindowTarget& target) {
  if (!target.handle || target.pid <= 0 || target.pid > std::numeric_limits<DWORD>::max()) return false;
  HWND hwnd = reinterpret_cast<HWND>(target.handle);
  if (!IsWindow(hwnd) || !eligible(hwnd, static_cast<DWORD>(target.pid))) return false;
  const auto token = creation_time(target.pid);
  return token && *token == target.process_creation;
}
struct FindContext { DWORD pid; std::vector<HWND> windows; };
BOOL CALLBACK collect(HWND hwnd, LPARAM value) {
  auto& context = *reinterpret_cast<FindContext*>(value);
  if (eligible(hwnd, context.pid)) context.windows.push_back(hwnd);
  return TRUE;
}
struct Occlusion { HWND target; RECT client; bool blocked{}, found{}; };
BOOL CALLBACK inspect_above(HWND hwnd, LPARAM value) {
  auto& state = *reinterpret_cast<Occlusion*>(value);
  if (hwnd == state.target) { state.found = true; return FALSE; }
  if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
  DWORD cloaked{};
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
  RECT bounds{}, overlap{};
  if (!GetWindowRect(hwnd, &bounds)) { state.blocked = true; return FALSE; }
  if (IntersectRect(&overlap, &bounds, &state.client)) { state.blocked = true; return FALSE; }
  return TRUE;
}
bool client_geometry(HWND hwnd, RECT& screen, int& width, int& height) {
  RECT client{}; POINT top{}, bottom{};
  if (!GetClientRect(hwnd, &client)) return false;
  width = client.right - client.left; height = client.bottom - client.top;
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return false;
  top = {client.left, client.top}; bottom = {client.right, client.bottom};
  if (!ClientToScreen(hwnd, &top) || !ClientToScreen(hwnd, &bottom)) return false;
  screen = {top.x, top.y, bottom.x, bottom.y};
  if (screen.right - screen.left != width || screen.bottom - screen.top != height) return false;
  HMONITOR monitor = MonitorFromRect(&screen, MONITOR_DEFAULTTONULL); MONITORINFO info{}; info.cbSize = sizeof(info);
  return monitor && GetMonitorInfoW(monitor, &info) && screen.left >= info.rcMonitor.left && screen.top >= info.rcMonitor.top && screen.right <= info.rcMonitor.right && screen.bottom <= info.rcMonitor.bottom;
}
bool unobstructed(HWND hwnd, const RECT& rect) {
  Occlusion state{hwnd, rect}; EnumWindows(inspect_above, reinterpret_cast<LPARAM>(&state));
  return state.found && !state.blocked;
}
bool shell_hidden(std::uintptr_t value) {
  HWND shell = reinterpret_cast<HWND>(value); DWORD pid{};
  if (!shell || !IsWindow(shell)) return false;
  GetWindowThreadProcessId(shell, &pid);
  return pid == GetCurrentProcessId() && (!IsWindowVisible(shell) || IsIconic(shell));
}
bool png_encoder(CLSID& result) {
  UINT count{}, bytes{}; if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || !bytes) return false;
  std::vector<BYTE> buffer(bytes); auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
  if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) return false;
  for (UINT i = 0; i < count; ++i) if (encoders[i].MimeType && wcscmp(encoders[i].MimeType, L"image/png") == 0) { result = encoders[i].Clsid; return true; }
  return false;
}
} // namespace
std::optional<SessionWindowTarget> find_session_window(std::int64_t pid) {
  const auto token = creation_time(pid); if (!token) return {};
  FindContext context{static_cast<DWORD>(pid), {}};
  if (!EnumWindows(collect, reinterpret_cast<LPARAM>(&context)) || context.windows.size() != 1) return {};
  SessionWindowTarget target{pid, reinterpret_cast<std::uintptr_t>(context.windows.front()), *token};
  return valid_target(target) ? std::optional<SessionWindowTarget>{target} : std::nullopt;
}
Result focus_session_window(const SessionWindowTarget& target) {
  if (!valid_target(target)) return {false, "The session window is no longer valid."};
  HWND hwnd = reinterpret_cast<HWND>(target.handle);
  if (IsIconic(hwnd)) ShowWindowAsync(hwnd, SW_RESTORE);
  if (!SetForegroundWindow(hwnd) || GetForegroundWindow() != hwnd || !valid_target(target))
    return {false, "Windows did not grant focus to the session window. Select it from the taskbar."};
  return {true, "Session window focused."};
}
Result capture_session_window(const SessionWindowTarget& target, const std::filesystem::path& destination, std::uintptr_t excluded_shell_handle) {
  if (!valid_target(target)) return {false, "The session window is no longer valid."};
  if (!shell_hidden(excluded_shell_handle)) return {false, "Hide or minimize the OpenXMB window before capturing."};
  HWND hwnd = reinterpret_cast<HWND>(target.handle);
  if (IsIconic(hwnd) || GetForegroundWindow() != hwnd) return {false, "Bring the session window to the foreground before capturing."};
  DWORD affinity{};
  if (!GetWindowDisplayAffinity(hwnd, &affinity) || affinity != WDA_NONE) return {false, "The session window is protected or its capture policy is unavailable."};
  RECT rect{}; int width{}, height{};
  if (!client_geometry(hwnd, rect, width, height)) return {false, "The session client area is offscreen or unavailable."};
  if (FAILED(DwmFlush())) return {false, "Windows could not synchronize the desktop before capture."};
  if (!unobstructed(hwnd, rect)) return {false, "The session window is obscured. Move other windows away and try again."};
  HDC screen = GetDC(nullptr); if (!screen) return {false, "Windows could not access the visible screen."};
  HDC memory = CreateCompatibleDC(screen);
  BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
  void* pixels{}; HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  HGDIOBJ old = memory && bitmap ? SelectObject(memory, bitmap) : nullptr;
  bool copied = old && old != HGDI_ERROR && BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
  if (copied) copied = GdiFlush() != FALSE;
  if (old && old != HGDI_ERROR) SelectObject(memory, old);
  if (memory) DeleteDC(memory); ReleaseDC(nullptr, screen);
  RECT after{}; int after_width{}, after_height{};
  const bool stable = copied && valid_target(target) && shell_hidden(excluded_shell_handle) && GetForegroundWindow() == hwnd && !IsIconic(hwnd) && client_geometry(hwnd, after, after_width, after_height) && EqualRect(&rect, &after) && unobstructed(hwnd, after);
  if (!stable) { if (bitmap) DeleteObject(bitmap); return {false, "The session window changed, lost focus, or became obscured during capture."}; }
  const auto* data = static_cast<const unsigned char*>(pixels); bool nonblack = false;
  for (std::size_t i = 0, n = static_cast<std::size_t>(width) * height * 4; i < n; i += 4) if (data[i] || data[i+1] || data[i+2]) { nonblack = true; break; }
  if (!nonblack) { DeleteObject(bitmap); return {false, "The captured surface was black or unavailable. A black game frame cannot be distinguished from an unavailable surface."}; }
  Gdiplus::GdiplusStartupInput startup; ULONG_PTR token{};
  if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) { DeleteObject(bitmap); return {false, "Windows image encoding could not start."}; }
  CLSID encoder{}; bool saved = false;
  if (png_encoder(encoder)) { Gdiplus::Bitmap output(bitmap, nullptr); saved = output.GetLastStatus() == Gdiplus::Ok && output.Save(destination.c_str(), &encoder, nullptr) == Gdiplus::Ok; }
  Gdiplus::GdiplusShutdown(token); DeleteObject(bitmap);
  return saved ? Result{true, "Session window captured."} : Result{false, "Could not save the PNG screenshot at the requested location."};
}
} // namespace openxmb::integrations
#else
namespace openxmb::integrations {
std::optional<SessionWindowTarget> find_session_window(std::int64_t) { return {}; }
Result focus_session_window(const SessionWindowTarget&) { return {false, "Session window switching is unavailable on this platform."}; }
Result capture_session_window(const SessionWindowTarget&, const std::filesystem::path&, std::uintptr_t) { return {false, "Session window capture is unavailable on this platform."}; }
} // namespace openxmb::integrations
#endif


