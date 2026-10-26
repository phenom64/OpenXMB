#include "openxmb/integrations/desktop.hpp"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <unistd.h>
#include <iostream>
#include <stdexcept>

int main() {
  using namespace openxmb::integrations;
  auto* display = XOpenDisplay(nullptr);
  if (!display) { std::cerr << "An X11 display is required\n"; return 1; }
  const auto window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 640, 480, 0, 0, 0);
  const auto pid = static_cast<unsigned long>(getpid());
  const auto property = XInternAtom(display, "_NET_WM_PID", False);
  XChangeProperty(display, window, property, XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&pid), 1);
  XSelectInput(display, window, KeyPressMask | ButtonPressMask | PointerMotionMask);
  XMapWindow(display, window); XSetInputFocus(display, window, RevertToParent, CurrentTime); XSync(display, False);
  BrowserSession session{static_cast<std::int64_t>(pid), true, "http://fixture.invalid", static_cast<std::uintptr_t>(window), {}};
  try {
    const auto check = [](const Result& result) { if (!result) throw std::runtime_error(result.message); };
    check(browser_input(session, {BrowserControl::pointer, 0.5, 0.5, {}}));
    Window root{}, child{}; int root_x{}, root_y{}, x{}, y{}; unsigned mask{};
    XQueryPointer(display, window, &root, &child, &root_x, &root_y, &x, &y, &mask);
    if (x < 315 || x > 325 || y < 235 || y > 245) throw std::runtime_error("Virtual pointer did not reach the browser center");
    while (XPending(display)) { XEvent event; XNextEvent(display, &event); }
    check(browser_input(session, {BrowserControl::click}));
    check(browser_input(session, {BrowserControl::text, 0, 0, "Hi:"}));
    XSync(display, False);
    bool clicked{}; std::string typed;
    while (XPending(display)) {
      XEvent event; XNextEvent(display, &event);
      if (event.type == ButtonPress && event.xbutton.button == Button1) clicked = true;
      if (event.type == KeyPress) {
        char text[16]{}; KeySym symbol{};
        const auto bytes = XLookupString(&event.xkey, text, sizeof(text), &symbol, nullptr);
        typed.append(text, static_cast<std::size_t>(bytes));
      }
    }
    if (!clicked || typed != "Hi:") throw std::runtime_error("Browser click/text injection did not reach its owned target: " + typed);
    check(browser_input(session, {BrowserControl::tab}));
    check(browser_input(session, {BrowserControl::back}));
    check(browser_input(session, {BrowserControl::return_to_shell}));
    BrowserSession unrelated{static_cast<std::int64_t>(pid + 100000), true, "http://fixture.invalid", 0, {}};
    if (browser_input(unrelated, {BrowserControl::text, 0, 0, "must not type"})) throw std::runtime_error("Input was accepted without an owned browser window");
    XDestroyWindow(display, window); XSync(display, False);
    if (browser_input(session, {BrowserControl::click})) throw std::runtime_error("A closed browser window accepted input");
    session.native_input.reset(); unrelated.native_input.reset(); XCloseDisplay(display);
    std::cout << "browser_input_tests: owned-window pointer, click, text, tab, history, focus and closed-window checks passed\n";
    return 0;
  } catch (const std::exception& error) { session.native_input.reset(); XCloseDisplay(display); std::cerr << error.what() << '\n'; return 1; }
}
