#include "openxmb/integrations/desktop.hpp"
#include "openxmb/integrations/disc.hpp"
#include <iostream>
#ifdef _WIN32
#include "focus_handoff.hpp"
#include <string_view>
#include <vector>
#endif

int main(int argc, char** argv) {
  using namespace openxmb::integrations;
#ifdef _WIN32
  // Private overlay focus helper started by OpenXMB itself; see focus_handoff.hpp.
  if (argc >= 2 && std::string_view{argv[1]} == focus_handoff::command) {
    std::vector<std::string_view> args;
    try { args.assign(argv + 2, argv + argc); } catch (...) { return static_cast<int>(focus_handoff::Outcome::internal); }
    return focus_handoff::run_helper(args);
  }
#endif
  try {
    if (argc < 2) { std::cout << "openxmb-integrations probe [PATH] | browser URL | screenshot FILE | autostart on|off [EXECUTABLE]\n"; return 0; }
    const std::string_view command{argv[1]};
    if (command == "probe") {
      const auto discs = argc > 2 ? std::vector{probe_disc(argv[2])} : discover_discs();
      for (const auto& disc : discs) std::cout << disc.path.string() << '\t' << disc_kind_name(disc.kind) << '\t' << disc.representative_stream.string() << '\n';
      return 0;
    }
    if (command == "browser" && argc > 2) { BrowserSession session; const auto result = open_browser(argv[2], session); std::cout << result.message << '\n'; return result ? 0 : 1; }
    if (command == "screenshot" && argc > 2) { const auto result = capture_screenshot(argv[2]); std::cout << result.message << '\n'; return result ? 0 : 1; }
    if (command == "autostart" && argc > 2) {
      const std::string_view mode{argv[2]};
      if (mode != "on" && mode != "off") throw std::runtime_error("Use autostart on or off");
      const auto result = set_autostart(mode == "on", argc > 3 ? std::filesystem::path{argv[3]} : executable_path().parent_path() / "XMS.bin");
      std::cout << result.message << '\n'; return result ? 0 : 1;
    }
    throw std::runtime_error("Unknown command or missing argument");
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
