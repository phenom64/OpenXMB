#include "openxmb/integrations/session_window.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <atomic>
#include <windows.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <limits>
#include <vector>

namespace openxmb::integrations {
namespace {
struct PhysicalCoordinates {
  DPI_AWARENESS_CONTEXT previous=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  ~PhysicalCoordinates(){if(previous)SetThreadDpiAwarenessContext(previous);}
};
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
  RECT client{};
  if (!GetClientRect(hwnd, &client)) return false;
  width = client.right - client.left; height = client.bottom - client.top;
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return false;
  POINT corners[2]{{client.left,client.top},{client.right,client.bottom}};
  SetLastError(ERROR_SUCCESS);
  if(!MapWindowPoints(hwnd,nullptr,corners,2) && GetLastError()!=ERROR_SUCCESS)return false;
  screen = {corners[0].x,corners[0].y,corners[1].x,corners[1].y};
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
Result validate_session_window(const SessionWindowTarget& target) {
  return valid_target(target)?Result{true,"Session window identity is current."}:Result{false,"The session window identity is no longer valid."};
}
std::optional<SessionWindowBounds> session_window_bounds(const SessionWindowTarget& target) {
  PhysicalCoordinates coordinates;
  if(!valid_target(target))return {};
  RECT rect{};int width{},height{};
  if(!client_geometry(reinterpret_cast<HWND>(target.handle),rect,width,height))return {};
  return SessionWindowBounds{rect.left,rect.top,static_cast<unsigned>(width),static_cast<unsigned>(height)};
}
SessionCaptureResult capture_session_window(const SessionWindowTarget& target, const std::filesystem::path& destination, std::uintptr_t excluded_shell_handle) {
  PhysicalCoordinates coordinates;
  if (!valid_target(target)) return {CaptureState::unavailable, "The session window is no longer valid."};
  std::error_code destination_error;
  if(std::filesystem::exists(destination,destination_error)||destination_error)
    return {CaptureState::unavailable,"The screenshot destination already exists or cannot be inspected."};
  if (!shell_hidden(excluded_shell_handle)) return {CaptureState::unavailable, "Hide or minimize the OpenXMB window before capturing."};
  HWND hwnd = reinterpret_cast<HWND>(target.handle);
  if(IsIconic(hwnd))return {CaptureState::pending,"The game window is still restoring."};
  if(GetForegroundWindow()!=hwnd)return {CaptureState::unavailable,"Bring the session window to the foreground before capturing."};
  DWORD affinity{};
  if (!GetWindowDisplayAffinity(hwnd, &affinity) || affinity != WDA_NONE) return {CaptureState::unavailable, "The session window is protected or its capture policy is unavailable."};
  RECT rect{}; int width{}, height{};
  if (!client_geometry(hwnd, rect, width, height)) return {CaptureState::pending, "The session client area is offscreen or unavailable."};
  if (FAILED(DwmFlush())) return {CaptureState::unavailable, "Windows could not synchronize the desktop before capture."};
  if (!unobstructed(hwnd, rect)) return {CaptureState::pending, "The session window is obscured. Move other windows away and try again."};
  HDC screen = GetDC(nullptr); if (!screen) return {CaptureState::unavailable, "Windows could not access the visible screen."};
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
  if (!stable) { if (bitmap) DeleteObject(bitmap); return {CaptureState::pending, "The session window changed, lost focus, or became obscured during capture."}; }
  const auto* data = static_cast<const unsigned char*>(pixels); bool nonblack = false;
  for (std::size_t i = 0, n = static_cast<std::size_t>(width) * height * 4; i < n; i += 4) if (data[i] || data[i+1] || data[i+2]) { nonblack = true; break; }
  if (!nonblack) { DeleteObject(bitmap); return {CaptureState::pending, "The captured surface was black or unavailable. A black game frame cannot be distinguished from an unavailable surface."}; }
  Gdiplus::GdiplusStartupInput startup; ULONG_PTR token{};
  if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) { DeleteObject(bitmap); return {CaptureState::unavailable, "Windows image encoding could not start."}; }
  static std::atomic<std::uint64_t> capture_serial{};
  auto temporary=destination;
  temporary+=".part-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+"-"+std::to_string(++capture_serial);
  CLSID encoder{}; bool saved = false;
  if (png_encoder(encoder)) { Gdiplus::Bitmap output(bitmap, nullptr); saved = output.GetLastStatus() == Gdiplus::Ok && output.Save(temporary.c_str(), &encoder, nullptr) == Gdiplus::Ok; }
  Gdiplus::GdiplusShutdown(token); DeleteObject(bitmap);
  // Publish a complete PNG atomically; never replace a user's existing file.
  if(saved)saved=MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
  if(!saved)DeleteFileW(temporary.c_str());
  return saved ? SessionCaptureResult{CaptureState::saved, "Session window captured."} : SessionCaptureResult{CaptureState::unavailable, "Could not save the PNG screenshot at the requested location."};
}
} // namespace openxmb::integrations
#elif defined(__linux__)
#include "x11_support.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <dlfcn.h>
#include <fcntl.h>
#include <thread>
#if __has_include(<png.h>)
#include <png.h>
#define OPENXMB_X11_PNG 1
#endif
namespace openxmb::integrations {
namespace {
using x11_detail::Connection;using x11_detail::Reply;
bool eligible(const Connection& x,xcb_window_t window,std::int64_t pid) {
  Reply<xcb_get_window_attributes_reply_t> attributes{xcb_get_window_attributes_reply(x.c,xcb_get_window_attributes(x.c,window),nullptr)};
  if(!attributes||attributes->_class!=XCB_WINDOW_CLASS_INPUT_OUTPUT||attributes->override_redirect)return false;
  const auto hidden=x.property(window,"_NET_WM_STATE",XCB_ATOM_ATOM);
  if(attributes->map_state!=XCB_MAP_STATE_VIEWABLE&&std::ranges::find(hidden,x.atom("_NET_WM_STATE_HIDDEN"))==hidden.end())return false;
  if(!x.property(window,"WM_TRANSIENT_FOR",XCB_ATOM_WINDOW,1).empty())return false;
  const auto types=x.property(window,"_NET_WM_WINDOW_TYPE",XCB_ATOM_ATOM);
  if(!types.empty()&&std::ranges::find(types,x.atom("_NET_WM_WINDOW_TYPE_NORMAL"))==types.end())return false;
  const auto declared=x.property(window,"_NET_WM_PID",XCB_ATOM_CARDINAL,1);
  const auto actual=x.client_pid(window);
  return declared.size()==1&&declared[0]==pid&&actual&&*actual==pid;
}
bool valid_target(const Connection& x,const SessionWindowTarget& target) {
  if(!target.handle||target.handle>std::numeric_limits<xcb_window_t>::max())return false;
  const auto token=linux_detail::process_creation(target.pid);
  return token&&*token==target.process_creation&&eligible(x,static_cast<xcb_window_t>(target.handle),target.pid);
}
std::optional<SessionWindowBounds> geometry(const Connection& x,xcb_window_t window) {
  Reply<xcb_get_window_attributes_reply_t> attributes{xcb_get_window_attributes_reply(x.c,xcb_get_window_attributes(x.c,window),nullptr)};
  Reply<xcb_get_geometry_reply_t> size{xcb_get_geometry_reply(x.c,xcb_get_geometry(x.c,window),nullptr)};
  Reply<xcb_translate_coordinates_reply_t> point{xcb_translate_coordinates_reply(x.c,xcb_translate_coordinates(x.c,window,x.screen->root,0,0),nullptr)};
  if(!attributes||attributes->map_state!=XCB_MAP_STATE_VIEWABLE||!size||!point||!size->width||!size->height||size->width>16384||size->height>16384||static_cast<std::size_t>(size->width)*size->height>64*1024*1024)return {};
  if(point->dst_x<0||point->dst_y<0||point->dst_x+size->width>x.screen->width_in_pixels||point->dst_y+size->height>x.screen->height_in_pixels)return {};
  return SessionWindowBounds{point->dst_x,point->dst_y,size->width,size->height};
}
bool shell_hidden(const Connection& x,std::uintptr_t handle) {
  if(!handle||handle>std::numeric_limits<xcb_window_t>::max())return false;
  const auto pid=x.client_pid(handle);if(!pid||*pid!=getpid())return false;
  Reply<xcb_get_window_attributes_reply_t> attributes{xcb_get_window_attributes_reply(x.c,xcb_get_window_attributes(x.c,handle),nullptr)};
  return attributes&&attributes->map_state!=XCB_MAP_STATE_VIEWABLE;
}
bool unobstructed(const Connection& x,xcb_window_t target,const SessionWindowBounds& rect) {
  // WM client order alone misses override-redirect menus. Inspect root stacking
  // as well, resolving a reparented client's outermost frame first.
  auto frame=target;
  for(unsigned depth=0;depth<32;++depth){Reply<xcb_query_tree_reply_t> tree{xcb_query_tree_reply(x.c,xcb_query_tree(x.c,frame),nullptr)};if(!tree)return false;if(tree->parent==x.screen->root)break;if(!tree->parent||tree->parent==frame)return false;frame=tree->parent;if(depth==31)return false;}
  Reply<xcb_query_tree_reply_t> tree{xcb_query_tree_reply(x.c,xcb_query_tree(x.c,x.screen->root),nullptr)};
  if(!tree||tree->children_len>4096)return false;
  const auto* children=xcb_query_tree_children(tree.get());bool found=false;
  for(unsigned i=0;i<tree->children_len;++i){if(children[i]==frame){found=true;continue;}if(!found)continue;
    Reply<xcb_get_window_attributes_reply_t> attributes{xcb_get_window_attributes_reply(x.c,xcb_get_window_attributes(x.c,children[i]),nullptr)};
    if(!attributes)return false;if(attributes->_class==XCB_WINDOW_CLASS_INPUT_ONLY||attributes->map_state!=XCB_MAP_STATE_VIEWABLE)continue;
    const auto other=geometry(x,children[i]);if(!other)return false;
    if(other->x<rect.x+static_cast<int>(rect.width)&&rect.x<other->x+static_cast<int>(other->width)&&other->y<rect.y+static_cast<int>(rect.height)&&rect.y<other->y+static_cast<int>(other->height))return false;
  }
  return found;
}
bool same_geometry(const SessionWindowBounds& a,const SessionWindowBounds& b){return a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height;}
bool write_png(const std::filesystem::path& destination,std::span<const unsigned char> rgba,unsigned width,unsigned height) {
#ifdef OPENXMB_X11_PNG
  void* module=dlopen("libpng16.so.16",RTLD_NOW|RTLD_LOCAL);if(!module)return false;
  using Write=decltype(&png_image_write_to_stdio);const auto write=reinterpret_cast<Write>(dlsym(module,"png_image_write_to_stdio"));
  if(!write){dlclose(module);return false;}
  static std::atomic<std::uint64_t> serial{};auto temporary=destination;temporary+=".part-"+std::to_string(getpid())+"-"+std::to_string(++serial);
  const int fd=::open(temporary.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);if(fd<0){dlclose(module);return false;}
  FILE* stream=fdopen(fd,"wb");if(!stream){::close(fd);::unlink(temporary.c_str());dlclose(module);return false;}
  png_image image{};image.version=PNG_IMAGE_VERSION;image.width=width;image.height=height;image.format=PNG_FORMAT_RGBA;
  bool saved=write(&image,stream,0,rgba.data(),0,nullptr)&&fflush(stream)==0&&fsync(fd)==0;
  if(fclose(stream)!=0)saved=false;
  // link publishes without replacing an existing destination, even if another
  // process creates it after our initial inspection. Temporary is same FS.
  if(saved)saved=::link(temporary.c_str(),destination.c_str())==0;
  ::unlink(temporary.c_str());dlclose(module);return saved;
#else
  (void)destination;(void)rgba;(void)width;(void)height;return false;
#endif
}
}
std::optional<SessionWindowTarget> find_session_window(std::int64_t pid) {
  const auto token=linux_detail::process_creation(pid);if(!token)return {};
  Connection x;if(!x)return {};
  auto windows=x.property(x.screen->root,"_NET_CLIENT_LIST_STACKING",XCB_ATOM_WINDOW);if(windows.empty())windows=x.property(x.screen->root,"_NET_CLIENT_LIST",XCB_ATOM_WINDOW);
  std::optional<SessionWindowTarget> found;
  for(const auto window:windows)if(eligible(x,window,pid)){if(found)return {};found=SessionWindowTarget{pid,window,*token};}
  return found&&valid_target(x,*found)?found:std::nullopt;
}
Result validate_session_window(const SessionWindowTarget& target){Connection x;return x&&valid_target(x,target)?Result{true,"X11 window identity is current."}:Result{false,"The local X11 session window identity is unavailable or no longer valid."};}
std::optional<SessionWindowBounds> session_window_bounds(const SessionWindowTarget& target){Connection x;return x&&valid_target(x,target)?geometry(x,target.handle):std::nullopt;}
Result focus_session_window(const SessionWindowTarget& target) {
  Connection x;if(!x||!valid_target(x,target))return {false,"The local X11 session window is no longer valid."};
  if(!x.check(xcb_map_window_checked(x.c,target.handle))||!x.activate(target.handle))return {false,"The X11 window manager could not request application focus."};
  for(unsigned attempt=0;attempt<20;++attempt){if(x.active(target.handle)&&x.focused(target.handle)&&valid_target(x,target))return {true,"Session window focused."};std::this_thread::sleep_for(std::chrono::milliseconds(5));}
  return {false,"The X11 window manager did not grant application focus. Select it using the window switcher."};
}
SessionCaptureResult capture_session_window(const SessionWindowTarget& target,const std::filesystem::path& destination,std::uintptr_t excluded_shell_handle) {
  Connection x;if(!x||!valid_target(x,target))return {CaptureState::unavailable,"A native local X11 session window is required for capture."};
  std::error_code error;if(std::filesystem::exists(destination,error)||error)return {CaptureState::unavailable,"The screenshot destination already exists or cannot be inspected."};
  if(!shell_hidden(x,excluded_shell_handle))return {CaptureState::unavailable,"Hide or minimize the OpenXMB window before capturing."};
  if(!x.active(target.handle)||!x.focused(target.handle))return {CaptureState::unavailable,"Bring the session window to the foreground before capturing."};
  const auto bypass=x.property(target.handle,"_NET_WM_BYPASS_COMPOSITOR",XCB_ATOM_CARDINAL,1);
  if(!bypass.empty()&&bypass[0]==1)return {CaptureState::unavailable,"The application requests compositor bypass; its live GPU surface cannot be verified."};
  const auto before=geometry(x,target.handle);if(!before||!unobstructed(x,target.handle,*before))return {CaptureState::pending,"The session client is offscreen, obscured or still restoring."};
  const auto* setup=xcb_get_setup(x.c);bool pixels32=false;
  for(auto formats=xcb_setup_pixmap_formats_iterator(setup);formats.rem;xcb_format_next(&formats))if(formats.data->depth==x.screen->root_depth&&formats.data->bits_per_pixel==32&&formats.data->scanline_pad==32)pixels32=true;
  if(!pixels32||setup->image_byte_order!=XCB_IMAGE_ORDER_LSB_FIRST)return {CaptureState::unavailable,"This X11 capture pixel format is unsupported."};
  bool rgb=false;for(auto depths=xcb_screen_allowed_depths_iterator(x.screen);depths.rem;xcb_depth_next(&depths))for(auto visuals=xcb_depth_visuals_iterator(depths.data);visuals.rem;xcb_visualtype_next(&visuals))if(visuals.data->visual_id==x.screen->root_visual&&visuals.data->red_mask==0xff0000&&visuals.data->green_mask==0xff00&&visuals.data->blue_mask==0xff)rgb=true;
  if(!rgb)return {CaptureState::unavailable,"This X11 capture visual is unsupported."};
  Reply<xcb_get_image_reply_t> image{xcb_get_image_reply(x.c,xcb_get_image(x.c,XCB_IMAGE_FORMAT_Z_PIXMAP,x.screen->root,before->x,before->y,before->width,before->height,0xffffffff),nullptr)};
  const auto size=static_cast<std::size_t>(before->width)*before->height*4;
  if(!image||static_cast<std::size_t>(xcb_get_image_data_length(image.get()))!=size)return {CaptureState::pending,"The visible X11 frame is unavailable."};
  const auto after=geometry(x,target.handle);
  if(!after||!same_geometry(*before,*after)||!valid_target(x,target)||!x.active(target.handle)||!x.focused(target.handle)||!shell_hidden(x,excluded_shell_handle)||!unobstructed(x,target.handle,*after))return {CaptureState::pending,"The session window changed or became obscured during capture."};
  const auto* pixels=xcb_get_image_data(image.get());std::vector<unsigned char> rgba(size);bool nonblack=false;
  for(std::size_t i=0;i<size;i+=4){rgba[i]=pixels[i+2];rgba[i+1]=pixels[i+1];rgba[i+2]=pixels[i];rgba[i+3]=255;nonblack=nonblack||pixels[i]||pixels[i+1]||pixels[i+2];}
  if(!nonblack)return {CaptureState::pending,"A black frame cannot be distinguished from an unavailable GPU surface."};
  return write_png(destination,rgba,before->width,before->height)?SessionCaptureResult{CaptureState::saved,"Visible X11 session window captured."}:SessionCaptureResult{CaptureState::unavailable,"Could not encode or publish the PNG screenshot; libpng16 and a writable destination are required."};
}
}
#else
namespace openxmb::integrations {
std::optional<SessionWindowTarget> find_session_window(std::int64_t) { return {}; }
std::optional<SessionWindowBounds> session_window_bounds(const SessionWindowTarget&) {return {};}
Result validate_session_window(const SessionWindowTarget&) { return {false,"Session window identity is unavailable on this platform."}; }
Result focus_session_window(const SessionWindowTarget&) { return {false, "Session window switching is unavailable on this platform."}; }
SessionCaptureResult capture_session_window(const SessionWindowTarget&, const std::filesystem::path&, std::uintptr_t) { return {CaptureState::unavailable, "Session window capture is unavailable on this platform."}; }
} // namespace openxmb::integrations
#endif


