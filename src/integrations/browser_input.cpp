#include "openxmb/integrations/desktop.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <nlohmann/json.hpp>
#endif

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#include <unistd.h>
#else
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace openxmb::integrations {
namespace {
#ifndef _WIN32
#ifndef __APPLE__
struct XInput {
  Display* display{XOpenDisplay(nullptr)};
  void* xtest{dlopen("libXtst.so.6", RTLD_LAZY | RTLD_LOCAL)};
  using Key = Bool (*)(Display*, unsigned, Bool, unsigned long);
  using Button = Bool (*)(Display*, unsigned, Bool, unsigned long);
  using Motion = Bool (*)(Display*, int, int, int, unsigned long);
  Key key{};
  Button button{};
  Motion motion{};
  Window window{};
  XInput() {
    if (xtest) {
      key = reinterpret_cast<Key>(dlsym(xtest, "XTestFakeKeyEvent"));
      button = reinterpret_cast<Button>(dlsym(xtest, "XTestFakeButtonEvent"));
      motion = reinterpret_cast<Motion>(dlsym(xtest, "XTestFakeMotionEvent"));
    }
  }
  ~XInput() { if (display) XCloseDisplay(display); if (xtest) dlclose(xtest); }
};
// Xlib's process-global error callback must keep errors on other SDL displays
// with their original handler while treating a closing browser as a lost target.
std::mutex x_error_mutex;
std::atomic<Display*> guarded_display{};
std::atomic<int> last_x_error{};
std::atomic<XErrorHandler> previous_error_handler{};
int x_error(Display* display, XErrorEvent* event) {
  if (display == guarded_display) { last_x_error = event->error_code; return 0; }
  const auto previous = previous_error_handler.load();
  return previous ? previous(display, event) : 0;
}
struct XErrors {
  std::unique_lock<std::mutex> lock{x_error_mutex};
  Display* display;
  explicit XErrors(Display* value) : display(value) {
    guarded_display = value; last_x_error = 0;
    previous_error_handler = XSetErrorHandler(x_error);
  }
  bool okay() { XSync(display, False); return last_x_error == 0; }
  ~XErrors() { XSync(display, False); XSetErrorHandler(previous_error_handler.load()); guarded_display = nullptr; }
};
bool browser_window(Display* display, Window window, std::int64_t process_id) {
  Atom actual{}; int format{}; unsigned long items{}, remaining{}; unsigned char* data{};
  const auto atom = XInternAtom(display, "_NET_WM_PID", False);
  if (XGetWindowProperty(display, window, atom, 0, 1, False, XA_CARDINAL, &actual, &format, &items, &remaining, &data) != Success) return false;
  bool owns = false;
  if (data && format == 32 && items == 1) {
    const auto pid = *reinterpret_cast<unsigned long*>(data);
    owns = static_cast<std::int64_t>(pid) == process_id || static_cast<std::int64_t>(getpgid(static_cast<pid_t>(pid))) == process_id;
  }
  if (data) XFree(data);
  return owns;
}
Window locate_window(Display* display, Window parent, std::int64_t process_id, unsigned depth = 0) {
  if (depth > 5) return 0;
  if (browser_window(display, parent, process_id)) return parent;
  Window root{}, actual_parent{}, *children{}; unsigned count{};
  if (!XQueryTree(display, parent, &root, &actual_parent, &children, &count)) return 0;
  Window found{};
  for (unsigned index = 0; index < std::min(count, 1024U) && !found; ++index) found = locate_window(display, children[index], process_id, depth + 1);
  if (children) XFree(children);
  return found;
}
bool x_key(XInput& context, KeySym symbol, KeySym modifier = NoSymbol) {
  const auto key = XKeysymToKeycode(context.display, symbol);
  const auto mod = modifier == NoSymbol ? 0 : XKeysymToKeycode(context.display, modifier);
  if (!key || (modifier != NoSymbol && !mod)) return false;
  if (mod) context.key(context.display, mod, True, 0);
  context.key(context.display, key, True, 0); context.key(context.display, key, False, 0);
  if (mod) context.key(context.display, mod, False, 0);
  return true;
}
#endif
#endif
#ifdef _WIN32
HWND locate_window(std::int64_t pid) {
  struct State { DWORD pid; HWND window; } state{static_cast<DWORD>(pid), nullptr};
  EnumWindows([](HWND window, LPARAM argument) -> BOOL {
    auto& value = *reinterpret_cast<State*>(argument);
    DWORD owner{}; GetWindowThreadProcessId(window, &owner);
    if (owner == value.pid && IsWindowVisible(window)) { value.window = window; return FALSE; }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&state));
  return state.window;
}
void win_key(std::vector<INPUT>& events, WORD key, bool down, bool unicode = false) {
  INPUT event{}; event.type = INPUT_KEYBOARD;
  event.ki.wVk = unicode ? 0 : key; event.ki.wScan = unicode ? key : 0;
  event.ki.dwFlags = (unicode ? KEYEVENTF_UNICODE : 0) | (down ? 0 : KEYEVENTF_KEYUP);
  events.push_back(event);
}
#endif
}
namespace detail {
std::uintptr_t capture_return_window() {
#ifdef _WIN32
  // Foreground may belong to another app during asynchronous startup. Never
  // embed into that app: prefer this thread's active root, then our own window.
  auto window=GetActiveWindow();
  DWORD owner{};GetWindowThreadProcessId(window,&owner);
  if(window&&owner==GetCurrentProcessId()) return reinterpret_cast<std::uintptr_t>(GetAncestor(window,GA_ROOT));
  struct Search {HWND window{};} search;
  EnumWindows([](HWND candidate,LPARAM argument)->BOOL {
    DWORD pid{};GetWindowThreadProcessId(candidate,&pid);
    if(pid==GetCurrentProcessId()&&IsWindowVisible(candidate)&&!GetWindow(candidate,GW_OWNER)) {
      reinterpret_cast<Search*>(argument)->window=candidate;return FALSE;
    }
    return TRUE;
  },reinterpret_cast<LPARAM>(&search));
  return reinterpret_cast<std::uintptr_t>(search.window);
#elif defined(__APPLE__)
  return static_cast<std::uintptr_t>(getpid());
#else
  auto* display = XOpenDisplay(nullptr);
  if (!display) return 0;
  Window window{}; int revert{}; XGetInputFocus(display, &window, &revert); XCloseDisplay(display);
  return window == None || window == PointerRoot ? 0 : static_cast<std::uintptr_t>(window);
#endif
}
}
Result browser_input(BrowserSession& session, const BrowserInput& input) {
  if (!std::isfinite(input.x) || !std::isfinite(input.y)) return {false, "Invalid browser pointer coordinates"};
  if (input.text.size() > 4096) return {false, "Browser text is too long"};
  if (input.control != BrowserControl::return_to_shell && (!session.dedicated_window || session.process_id <= 0))
    return {false, "Controller navigation requires a dedicated browser window"};
#ifdef _WIN32
  if(session.embedded_view) {
    switch(input.control) {
    case BrowserControl::back: return browser_view_command(session,"back");
    case BrowserControl::forward: return browser_view_command(session,"forward");
    case BrowserControl::text: return browser_view_command(session,"text",input.text);
    case BrowserControl::address_bar: return browser_view_command(session,"address");
    case BrowserControl::return_to_shell: return browser_view_command(session,"return");
    default: return browser_view_command(session,"input",nlohmann::json{{"control",static_cast<int>(input.control)},{"x",input.x},{"y",input.y}}.dump());
    }
  }
  const auto window = input.control == BrowserControl::return_to_shell ? reinterpret_cast<HWND>(session.return_window) : locate_window(session.process_id);
  if (!window || !IsWindow(window)) return {false, "The browser window is not ready"};
  ShowWindow(window, SW_RESTORE); SetForegroundWindow(window);
  if (input.control == BrowserControl::return_to_shell) return {true, "Returned to OpenXMB"};
  if (GetForegroundWindow() != window) return {false, "Select the browser window to permit controller input"};
  std::vector<INPUT> events;
  const auto chord = [&](WORD key, WORD modifier = 0) {
    if (modifier) win_key(events, modifier, true);
    win_key(events, key, true); win_key(events, key, false);
    if (modifier) win_key(events, modifier, false);
  };
  INPUT mouse{}; mouse.type = INPUT_MOUSE;
  switch (input.control) {
  case BrowserControl::pointer: {
    RECT client{}; GetClientRect(window, &client); POINT point{0, 0}; ClientToScreen(window, &point);
    const auto x = point.x + std::clamp(input.x, 0.0, 1.0) * client.right;
    const auto y = point.y + std::clamp(input.y, 0.0, 1.0) * client.bottom;
    mouse.mi.dx = static_cast<LONG>((x - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535.0 / std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1));
    mouse.mi.dy = static_cast<LONG>((y - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535.0 / std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1));
    mouse.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK; events.push_back(mouse); break;
  }
  case BrowserControl::click: case BrowserControl::context_click:
    mouse.mi.dwFlags = input.control == BrowserControl::click ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN; events.push_back(mouse);
    mouse.mi.dwFlags = input.control == BrowserControl::click ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP; events.push_back(mouse); break;
  case BrowserControl::scroll: mouse.mi.dwFlags = MOUSEEVENTF_WHEEL; mouse.mi.mouseData = static_cast<DWORD>(input.y < 0 ? WHEEL_DELTA : -WHEEL_DELTA); events.push_back(mouse); break;
  case BrowserControl::tab: chord(VK_TAB); break;
  case BrowserControl::reverse_tab: chord(VK_TAB, VK_SHIFT); break;
  case BrowserControl::back: chord(VK_LEFT, VK_MENU); break;
  case BrowserControl::forward: chord(VK_RIGHT, VK_MENU); break;
  case BrowserControl::confirm: chord(VK_RETURN); break;
  case BrowserControl::escape: chord(VK_ESCAPE); break;
  case BrowserControl::address_bar: chord('L', VK_CONTROL); break;
  case BrowserControl::text: {
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.text.data(), static_cast<int>(input.text.size()), nullptr, 0);
    if (!length && !input.text.empty()) return {false, "Invalid UTF-8 browser text"};
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.text.data(), static_cast<int>(input.text.size()), text.data(), length);
    for (const auto character : text) { win_key(events, character, true, true); win_key(events, character, false, true); }
    break;
  }
  default: break;
  }
  if (!events.empty() && SendInput(static_cast<UINT>(events.size()), events.data(), sizeof(INPUT)) != events.size()) return {false, "Windows rejected browser input"};
  return {true, "Input sent"};
#elif defined(__APPLE__)
  if (!AXIsProcessTrusted()) return {false, "Enable OpenXMB in macOS Accessibility settings to use browser controller input"};
  const auto pid = static_cast<pid_t>(input.control == BrowserControl::return_to_shell ? session.return_window : static_cast<std::uintptr_t>(session.process_id));
  if (pid <= 0) return {false, "The browser window is not ready"};
  const auto application = AXUIElementCreateApplication(pid);
  const auto activated = AXUIElementSetAttributeValue(application, kAXFrontmostAttribute, kCFBooleanTrue);
  CFRelease(application);
  if (activated != kAXErrorSuccess) return {false, "Unable to focus the browser window"};
  if (input.control == BrowserControl::return_to_shell) return {true, "Returned to OpenXMB"};
  const auto post_key = [&](CGKeyCode key, CGEventFlags flags = 0) {
    const auto down = CGEventCreateKeyboardEvent(nullptr, key, true), up = CGEventCreateKeyboardEvent(nullptr, key, false);
    CGEventSetFlags(down, flags); CGEventSetFlags(up, flags);
    CGEventPostToPid(pid, down); CGEventPostToPid(pid, up); CFRelease(down); CFRelease(up);
  };
  const auto bounds = CGDisplayBounds(CGMainDisplayID());
  const auto point = CGPointMake(bounds.origin.x + std::clamp(input.x, 0.0, 1.0) * bounds.size.width, bounds.origin.y + std::clamp(input.y, 0.0, 1.0) * bounds.size.height);
  switch (input.control) {
  case BrowserControl::pointer: {
    const auto event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, point, kCGMouseButtonLeft);
    CGEventPostToPid(pid, event); CFRelease(event); break;
  }
  case BrowserControl::click: case BrowserControl::context_click: {
    const auto current = CGEventCreate(nullptr); const auto position = CGEventGetLocation(current); CFRelease(current);
    const bool left = input.control == BrowserControl::click;
    const auto down = CGEventCreateMouseEvent(nullptr, left ? kCGEventLeftMouseDown : kCGEventRightMouseDown, position, left ? kCGMouseButtonLeft : kCGMouseButtonRight);
    const auto up = CGEventCreateMouseEvent(nullptr, left ? kCGEventLeftMouseUp : kCGEventRightMouseUp, position, left ? kCGMouseButtonLeft : kCGMouseButtonRight);
    CGEventPostToPid(pid, down); CGEventPostToPid(pid, up); CFRelease(down); CFRelease(up); break;
  }
  case BrowserControl::scroll: { const auto event = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 1, input.y < 0 ? 3 : -3); CGEventPostToPid(pid, event); CFRelease(event); break; }
  case BrowserControl::tab: post_key(48); break;
  case BrowserControl::reverse_tab: post_key(48, kCGEventFlagMaskShift); break;
  case BrowserControl::confirm: post_key(36); break;
  case BrowserControl::escape: post_key(53); break;
  case BrowserControl::back: post_key(33, kCGEventFlagMaskCommand); break;
  case BrowserControl::forward: post_key(30, kCGEventFlagMaskCommand); break;
  case BrowserControl::address_bar: post_key(37, kCGEventFlagMaskCommand); break;
  case BrowserControl::text: {
    const auto text = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(input.text.data()), static_cast<CFIndex>(input.text.size()), kCFStringEncodingUTF8, false);
    if (!text) return {false, "Invalid UTF-8 browser text"};
    std::vector<UniChar> characters(static_cast<std::size_t>(CFStringGetLength(text)));
    CFStringGetCharacters(text, CFRangeMake(0, CFStringGetLength(text)), characters.data()); CFRelease(text);
    const auto down = CGEventCreateKeyboardEvent(nullptr, 0, true), up = CGEventCreateKeyboardEvent(nullptr, 0, false);
    CGEventKeyboardSetUnicodeString(down, characters.size(), characters.data()); CGEventKeyboardSetUnicodeString(up, characters.size(), characters.data());
    CGEventPostToPid(pid, down); CGEventPostToPid(pid, up); CFRelease(down); CFRelease(up); break;
  }
  default: break;
  }
  return {true, "Input sent"};
#else
  if (!session.native_input) session.native_input = std::make_shared<XInput>();
  auto context = std::static_pointer_cast<XInput>(session.native_input);
  if (!context->display) return {false, "Browser controller input requires an X11 display"};
  if (!context->key || !context->button || !context->motion) return {false, "Install the XTest runtime library for browser controller input"};
  XErrors errors{context->display};
  if (input.control == BrowserControl::return_to_shell) {
    if (!session.return_window) return {false, "The OpenXMB window could not be identified"};
    const auto window = static_cast<Window>(session.return_window);
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(context->display, window, &attributes) || attributes.map_state != IsViewable) return {false, "The OpenXMB window is unavailable"};
    XRaiseWindow(context->display, window); XSetInputFocus(context->display, window, RevertToParent, CurrentTime);
    return {errors.okay(), "Returned to OpenXMB"};
  }
  if (!context->window || !browser_window(context->display, context->window, session.process_id))
    context->window = locate_window(context->display, DefaultRootWindow(context->display), session.process_id);
  if (!context->window) return {false, "The browser window is not ready"};
  XWindowAttributes attributes{};
  if (!XGetWindowAttributes(context->display, context->window, &attributes) || attributes.map_state != IsViewable) { context->window = 0; return {false, "The browser window is unavailable"}; }
  XRaiseWindow(context->display, context->window); XSetInputFocus(context->display, context->window, RevertToParent, CurrentTime);
  const auto click = [&](unsigned button) { context->button(context->display, button, True, 0); context->button(context->display, button, False, 0); };
  bool sent = true;
  switch (input.control) {
  case BrowserControl::pointer: {
    int x{}, y{}; Window child{};
    XTranslateCoordinates(context->display, context->window, DefaultRootWindow(context->display), 0, 0, &x, &y, &child);
    context->motion(context->display, -1, x + static_cast<int>(std::clamp(input.x, 0.0, 1.0) * std::max(1, attributes.width - 1)), y + static_cast<int>(std::clamp(input.y, 0.0, 1.0) * std::max(1, attributes.height - 1)), 0); break;
  }
  case BrowserControl::click: click(1); break;
  case BrowserControl::context_click: click(3); break;
  case BrowserControl::scroll: click(input.y < 0 ? 4 : 5); break;
  case BrowserControl::tab: sent = x_key(*context, XK_Tab); break;
  case BrowserControl::reverse_tab: sent = x_key(*context, XK_Tab, XK_Shift_L); break;
  case BrowserControl::back: sent = x_key(*context, XK_Left, XK_Alt_L); break;
  case BrowserControl::forward: sent = x_key(*context, XK_Right, XK_Alt_L); break;
  case BrowserControl::confirm: sent = x_key(*context, XK_Return); break;
  case BrowserControl::escape: sent = x_key(*context, XK_Escape); break;
  case BrowserControl::address_bar: sent = x_key(*context, XK_l, XK_Control_L); break;
  case BrowserControl::text:
    for (const unsigned char character : input.text) if (character >= 128 || character < 32) return {false, "Use the physical keyboard for non-Latin browser text on X11"};
    for (const unsigned char character : input.text) {
      const auto symbol = static_cast<KeySym>(character);
      const auto key = XKeysymToKeycode(context->display, symbol);
      const bool shifted = key && XkbKeycodeToKeysym(context->display, key, 0, 0) != symbol && XkbKeycodeToKeysym(context->display, key, 0, 1) == symbol;
      if (!x_key(*context, symbol, shifted ? XK_Shift_L : NoSymbol)) { sent = false; break; }
    }
    break;
  default: break;
  }
  const bool okay = errors.okay();
  if (!okay) context->window = 0;
  return {sent && okay, sent && okay ? "Input sent" : "The browser did not accept controller input"};
#endif
}
} // namespace openxmb::integrations
