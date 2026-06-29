#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void require_contains(const std::string &haystack, std::string_view needle,
                      std::string_view message) {
  require(haystack.find(needle) != std::string::npos, message);
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot read " + path.string());
  }
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

void test_scene_presets_match_thresholds(const std::filesystem::path &root) {
  const auto presets =
      read_file(root / "tools/xmb/scene_presets.json");
  const auto thresholds =
      read_file(root / "assets/xmb/manifests/visual-thresholds.json");

  for (const std::string_view scene :
       {"root", "settings", "theme", "theme-panel"}) {
    require_contains(presets, scene, "scene_presets.json registers scene");
    require_contains(thresholds, scene, "visual-thresholds.json registers scene");
  }

  require_contains(presets, "\"OPENXMB_INITIAL_CATEGORY\"",
                   "scene presets expose category harness");
  require_contains(presets, "\"OPENXMB_INITIAL_SETTINGS_MENU\"",
                   "scene presets expose settings menu harness");
  require_contains(presets, "\"OPENXMB_OPEN_INITIAL_SETTINGS_CHOICE\"",
                   "scene presets expose choice panel harness");
}

void test_runtime_harness_contract(const std::filesystem::path &root) {
  const auto main_menu =
      read_file(root / "src/app/components/main_menu.cpp");
  const auto shell = read_file(root / "src/app/shell.cpp");

  require_contains(main_menu, "OPENXMB_INITIAL_CATEGORY",
                   "main_menu reads category harness env");
  require_contains(main_menu, "OPENXMB_INITIAL_SETTINGS_MENU",
                   "main_menu reads settings menu harness env");
  require_contains(main_menu, "OPENXMB_INITIAL_SETTINGS_SELECTION",
                   "main_menu reads settings selection harness env");
  require_contains(main_menu, "OPENXMB_OPEN_INITIAL_SETTINGS_CHOICE",
                   "main_menu reads choice panel harness env");
  require_contains(shell, "OPENXMB_HEADLESS_STARTUP",
                   "shell supports optional headless startup overlay");
  require_contains(shell, "OPENXMB_FIXED_BOOT_SECONDS",
                   "shell supports fixed boot seconds");
}

void test_tooling_present(const std::filesystem::path &root) {
  require(std::filesystem::is_regular_file(root / "tools/xmb/capture_frame.ps1"),
          "capture_frame.ps1 exists");
  require(std::filesystem::is_regular_file(
              root / "tools/xmb/run_visual_regression.ps1"),
          "run_visual_regression.ps1 exists");
  require(std::filesystem::is_regular_file(
              root / "tools/xmb/compare_frames.py"),
          "compare_frames.py exists");
  require(std::filesystem::is_regular_file(
              root / "tools/xmb/verify_reference_frames.py"),
          "verify_reference_frames.py exists");
  require(std::filesystem::is_regular_file(
              root / "tools/xmb/install_reference_frames.ps1"),
          "install_reference_frames.ps1 exists");
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: openxmb_visual_harness_tests <repo-root>\n";
    return 2;
  }

  try {
    const std::filesystem::path root{argv[1]};
    test_scene_presets_match_thresholds(root);
    test_runtime_harness_contract(root);
    test_tooling_present(root);
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }

  if (failures != 0) {
    std::cerr << failures << " visual harness test(s) failed\n";
    return 1;
  }

  std::cout << "openxmb_visual_harness_tests: all checks passed\n";
  return 0;
}