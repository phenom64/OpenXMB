#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace openxmb::themes {

class ThemeError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct ImportLimits {
  std::uint64_t archive_bytes = 128ULL * 1024 * 1024;
  std::uint64_t expanded_bytes = 512ULL * 1024 * 1024;
  std::uint64_t file_bytes = 64ULL * 1024 * 1024;
  std::size_t entries = 4096;
  unsigned image_dimension = 4096;
};

struct ComponentOptions {
  bool background = true;
  bool icons = true;
  bool sounds = true;
  bool colors = true;
  bool font = true;
  bool overlay = true;
};

struct ThemeInfo {
  std::string id;
  std::string name;
  std::string author;
  std::string description;
  std::filesystem::path directory;
  std::filesystem::path background;
  std::filesystem::path preview;
  std::filesystem::path font;
  std::map<std::string, std::filesystem::path> icons;
  std::map<std::string, std::filesystem::path> sounds;
  nlohmann::json settings = nlohmann::json::object();
  nlohmann::json animation = nlohmann::json::object();
  std::vector<std::string> warnings;
  bool dynamic = false;
};

struct AnimationLayer {
  std::filesystem::path image;
  float x = 0, y = 0, width = 1, height = 1;
  float opacity = 1;
  bool additive = false;
};

struct AnimationVertex {
  float x = 0, y = 0, z = 0, w = 1, u = 0, v = 0;
  std::array<float, 4> color{1, 1, 1, 1};
};

struct AnimationDraw {
  std::filesystem::path image;
  std::vector<AnimationVertex> vertices;
  bool additive = false;
  bool depth = false;
};

struct AnimationFrame {
  std::filesystem::path background;
  std::vector<AnimationLayer> layers;
  std::vector<AnimationDraw> draws;
  std::vector<std::string> warnings;
};

struct AppliedTheme {
  std::string id;
  std::string name;
  std::filesystem::path directory;
  nlohmann::json shell = nlohmann::json::object();
  nlohmann::json settings = nlohmann::json::object();
  std::map<std::string, std::filesystem::path> icons;
  std::map<std::string, std::filesystem::path> sounds;
  nlohmann::json animation = nlohmann::json::object();
  std::vector<std::string> warnings;
  ComponentOptions components;
  bool reverted = false;
  [[nodiscard]] std::string serialize() const;
};

// A library owns only files beneath its supplied data directory. Source media
// and imported archives remain untouched. Instances may be scoped per profile.
class ThemeLibrary {
public:
  explicit ThemeLibrary(std::filesystem::path root, ImportLimits limits = {});
  [[nodiscard]] const std::filesystem::path& root() const noexcept;
  [[nodiscard]] std::filesystem::path inbox() const;
  [[nodiscard]] std::vector<ThemeInfo> discover() const;
  [[nodiscard]] ThemeInfo preview(const std::string& id) const;
  ThemeInfo import_theme(const std::filesystem::path& source);
  ThemeInfo create_custom_theme(const std::string& name, nlohmann::json shell_settings);
  void export_theme(const std::string& id, const std::filesystem::path& destination) const;
  void remove_theme(const std::string& id);
  AppliedTheme apply(const std::string& id, ComponentOptions options = {});
  AppliedTheme revert();
  [[nodiscard]] std::optional<AppliedTheme> active() const;
  void set_baseline(nlohmann::json shell);
  void save_preset(const std::string& name, const std::string& id, ComponentOptions options);
  [[nodiscard]] std::vector<std::string> presets() const;
  AppliedTheme load_preset(const std::string& name);
  void delete_preset(const std::string& name);
  [[nodiscard]] static AnimationFrame sample_animation(const ThemeInfo& theme, double seconds);
  [[nodiscard]] static AnimationFrame sample_animation(const AppliedTheme& theme, double seconds);
private:
  std::filesystem::path root_;
  ImportLimits limits_;
  [[nodiscard]] nlohmann::json read_state() const;
  void write_state(const nlohmann::json& value) const;
  [[nodiscard]] AppliedTheme resolve(const std::string& id, ComponentOptions options) const;
  [[nodiscard]] ThemeInfo load(const std::filesystem::path& directory) const;
};

// Converts a bounded P3TF resource tree into a portable native theme manifest.
// Unsupported scene features are retained as diagnostics with a still fallback.
nlohmann::json import_p3t(const std::filesystem::path& source,
                          const std::filesystem::path& output_directory,
                          const ImportLimits& limits = {});

} // namespace openxmb::themes
