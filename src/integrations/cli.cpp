#include "openxmb/integrations/desktop.hpp"
#include "openxmb/integrations/disc.hpp"
#include <iostream>

int main(int argc, char** argv) {
  using namespace openxmb::integrations;
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
