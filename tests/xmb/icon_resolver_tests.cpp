#include <openxmb/xmb/icon_resolver.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void test_filename_number_extraction() {
  expect(openxmb::xmb::normal_map_number_from_icon_filename("xmb_icon_042.png") ==
             std::optional<std::string>{"042"},
         "xmb_icon_042 maps to 042");
  expect(openxmb::xmb::normal_map_number_from_icon_filename("icontex_003_rendered.png") ==
             std::optional<std::string>{"003"},
         "icontex_003_rendered maps to 003");
  expect(!openxmb::xmb::normal_map_number_from_icon_filename("xmb_icon_psn.png"),
         "psn icon has no numbered normal map");
  expect(!openxmb::xmb::normal_map_number_from_icon_filename("playstation_store.png"),
         "store icon has no numbered normal map");
}

void test_compat_pack_normal_maps(const std::filesystem::path &compat_root) {
  const auto icons = compat_root / "icons";
  const auto normalmaps = compat_root / "normalmaps";
  expect(std::filesystem::is_directory(icons), "compat icons directory exists");
  expect(std::filesystem::is_directory(normalmaps),
         "compat normalmaps directory exists");
  if (!std::filesystem::is_directory(icons) ||
      !std::filesystem::is_directory(normalmaps)) {
    return;
  }

  std::vector<std::string> missing;
  std::size_t mapped = 0;
  for (const auto &entry : std::filesystem::directory_iterator(icons)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".png") {
      continue;
    }
    const auto resolved = openxmb::xmb::xmb_web_normal_map_for_icon(entry.path());
    if (!resolved) {
      continue;
    }
    ++mapped;
    if (!std::filesystem::exists(*resolved)) {
      missing.push_back(entry.path().filename().string() + " -> " +
                        resolved->filename().string());
    }
  }

  expect(mapped >= 70, "at least 70 numbered compat icons resolve normal maps");
  if (!missing.empty()) {
    for (const auto &item : missing) {
      std::cerr << "FAIL: missing normal map for " << item << '\n';
    }
    failures += static_cast<int>(missing.size());
  }
}

} // namespace

int main(int argc, char **argv) {
  test_filename_number_extraction();

  if (argc >= 2) {
    test_compat_pack_normal_maps(std::filesystem::path{argv[1]});
  } else {
    std::cout
        << "icon_resolver_tests: filename checks passed; skipping compat audit "
           "(pass compat/xmb-ui-compat path as argv[1])\n";
    if (failures != 0) {
      return 1;
    }
    return 0;
  }

  if (failures != 0) {
    std::cerr << failures << " icon resolver test(s) failed\n";
    return 1;
  }

  std::cout << "icon_resolver_tests: all checks passed\n";
  return 0;
}