#include "openxmb/integrations/desktop.hpp"
#include "openxmb/integrations/disc.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace openxmb::integrations;
namespace {
int checks{};
void require(bool okay, const char* message) { ++checks; if (!okay) throw std::runtime_error(message); }
void write(const std::filesystem::path& path, std::string_view value) { std::filesystem::create_directories(path.parent_path()); std::ofstream file(path, std::ios::binary); file << value; }
}
int main(int argc, char** argv) {
  const auto root = std::filesystem::temp_directory_path() / ("openxmb-integrations-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    std::filesystem::create_directories(root);
    require(static_cast<bool>(validate_browser_url("https://example.org/path?a=1&b=2")), "valid URL rejected");
    require(!validate_browser_url("javascript:alert(1)"), "unsafe browser scheme accepted");
    require(!validate_browser_url("https:///missing-host"), "missing host accepted");
    require(!validate_browser_url("https://example.org/\n--flag"), "control characters accepted");
    Preferences preferences;
    preferences.browser_url = "https://example.org/?q=a%20b";
    preferences.core = root / "core with spaces.so";
    preferences.content = root / "game's file.bin";
    require(static_cast<bool>(save_preferences(preferences, root / "preferences")), "preferences save failed");
    const auto loaded = load_preferences(root / "preferences");
    require(loaded.browser_url == preferences.browser_url && loaded.core == preferences.core && loaded.content == preferences.content, "preferences did not round-trip");
    write(root / "dvd/VIDEO_TS/VTS_01_1.VOB", "video");
    write(root / "dvd/VIDEO_TS/VTS_02_1.VOB", "larger video stream");
    write(root / "bluray/BDMV/STREAM/00001.m2ts", "bluray video");
    const auto dvd = probe_disc(root / "dvd");
    require(dvd.kind == DiscKind::dvd_video, "DVD structure was not detected");
    require(dvd.representative_stream.filename() == "VTS_02_1.VOB", "DVD representative stream selection failed");
    require(probe_disc(root / "bluray").kind == DiscKind::blu_ray, "Blu-ray structure was not detected");
    require(probe_disc(root / "absent").kind == DiscKind::absent, "missing disc reported as present");
    require(discover_discs({root}).size() == 2, "disc discovery missed fixtures");
#ifndef _WIN32
    const auto executable = root / "OpenXMB 100% \"quoted\"";
    write(executable, "executable fixture");
    require(static_cast<bool>(set_autostart(true, executable, root / "config")), "autostart creation failed");
    require(autostart_enabled(root / "config"), "autostart status was not persisted");
    require(static_cast<bool>(set_autostart(false, executable, root / "config")), "autostart removal failed");
    require(!autostart_enabled(root / "config"), "autostart status remains enabled");
    require(!set_autostart(true, root / "bad\npath", root / "config"), "autostart accepted a newline path");
#endif
    require(!launch_process({"/this-program-does-not-exist-openxmb"}), "missing executable launch was accepted");
    if (argc > 1) {
      const auto helper = launch_process({argv[1], "probe", (root / "dvd").string()});
      require(static_cast<bool>(helper), "companion command did not launch");
      std::optional<int> status;
      for (int attempt = 0; attempt < 100 && !status; ++attempt) { status = process_exit_status(helper.process_id); if (!status) std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
      require(status && *status == 0, "companion probe command failed");
    }
    std::filesystem::remove_all(root);
    std::cout << "integrations_tests: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::filesystem::remove_all(root);
    std::cerr << "integrations_tests: " << error.what() << '\n'; return 1;
  }
}
