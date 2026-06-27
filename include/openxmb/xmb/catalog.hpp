#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openxmb::xmb {

struct TransparentStringHash {
  using is_transparent = void;
  [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }
};

struct TransparentStringEqual {
  using is_transparent = void;
  [[nodiscard]] bool operator()(std::string_view left,
                                std::string_view right) const noexcept {
    return left == right;
  }
};

template <class Value>
using CatalogMap = std::unordered_map<std::string, Value, TransparentStringHash,
                                      TransparentStringEqual>;

enum class CatalogErrorCode {
  io,
  malformed_json,
  unsupported_schema,
  invalid_shape,
  duplicate_id,
  unresolved_reference,
  missing_translation,
};

struct CatalogError {
  CatalogErrorCode code{CatalogErrorCode::invalid_shape};
  std::string path;
  std::string message;
  std::size_t byte_offset{};
};

enum class NodeKind { category, menu, action, setting, media, landing };
enum class SafetyClass { safe, simulated, dangerous };

struct CatalogChoice {
  std::string id;
  std::string label_key;
  std::string value;
};

struct CatalogNode {
  std::string id;
  NodeKind kind{NodeKind::action};
  std::string label_key;
  std::string description_key;
  std::string value_key;
  std::string icon_ref;
  std::string action_id;
  std::string persistence_key;
  std::string capability;
  std::string optional_local_media_ref;
  SafetyClass safety{SafetyClass::safe};
  bool restricted_media{};
  std::size_t default_selection{};
  std::vector<CatalogChoice> choices;
  std::vector<std::string> children;
};

struct DialogTemplate {
  std::string id;
  std::string type;
  std::string title_key;
  std::string body_key;
  std::string illustration_ref;
  std::string capability;
  SafetyClass safety{SafetyClass::safe};
  bool side_panel{};
  std::vector<CatalogChoice> choices;
};

struct WizardScreen {
  std::string id;
  std::string kind;
  std::string title_key;
  std::string body_key;
  std::string label_key;
  std::string persistence_key;
  std::string capability;
  SafetyClass safety{SafetyClass::safe};
  bool transient{};
  bool optional{};
  std::vector<CatalogChoice> choices;
  std::vector<std::string> next_ids;
};

struct ControlEntry {
  std::string id;
  std::string surface;
  std::string label_key;
  std::string action_id;
  std::string capability;
  SafetyClass safety{SafetyClass::safe};
  std::vector<CatalogChoice> choices;
};

struct LandingTemplate {
  std::string id;
  std::string title_key;
  std::string body_key;
  std::string hero_ref;
  std::string optional_local_media_ref;
  std::string capability;
};

struct Catalog {
  int schema_version{};
  std::string locale;
  std::string source_commit;
  std::vector<std::string> root_order;
  CatalogMap<CatalogNode> nodes;
  CatalogMap<DialogTemplate> dialogs;
  CatalogMap<WizardScreen> wizards;
  CatalogMap<ControlEntry> controls;
  CatalogMap<LandingTemplate> landings;
  CatalogMap<std::string> strings;

  [[nodiscard]] const CatalogNode *
  find_node(std::string_view id) const noexcept;
  [[nodiscard]] const WizardScreen *
  find_wizard(std::string_view id) const noexcept;
  [[nodiscard]] std::string_view text(std::string_view key) const noexcept;
};

struct CatalogLoadResult {
  std::optional<Catalog> value;
  std::optional<CatalogError> error;

  [[nodiscard]] explicit operator bool() const noexcept {
    return value.has_value();
  }
};

[[nodiscard]] CatalogLoadResult
load_catalog_json(std::string_view json,
                  std::string_view source_name = "<memory>") noexcept;
[[nodiscard]] CatalogLoadResult
load_catalog_file(const std::filesystem::path &path) noexcept;

} // namespace openxmb::xmb
