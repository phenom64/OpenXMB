#include "openxmb/xmb/catalog.hpp"

#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace openxmb::xmb {
namespace {

using Json = nlohmann::json;

struct ShapeError final : std::runtime_error {
  CatalogErrorCode code;
  std::string path;
  ShapeError(CatalogErrorCode c, std::string p, std::string message)
      : std::runtime_error(std::move(message)), code(c), path(std::move(p)) {}
};

const Json &require_member(const Json &object, std::string_view key,
                           Json::value_t type, std::string_view path) {
  const auto it = object.find(key);
  const std::string member_path = std::string(path) + "." + std::string(key);
  if (it == object.end()) {
    throw ShapeError(CatalogErrorCode::invalid_shape, member_path,
                     "required member is missing");
  }
  if (it->type() != type) {
    throw ShapeError(CatalogErrorCode::invalid_shape, member_path,
                     "member has the wrong JSON type");
  }
  return *it;
}

std::string optional_string(const Json &object, std::string_view key,
                            std::string_view path) {
  const auto it = object.find(key);
  if (it == object.end())
    return {};
  if (!it->is_string()) {
    throw ShapeError(CatalogErrorCode::invalid_shape,
                     std::string(path) + "." + std::string(key),
                     "optional member must be a string");
  }
  return it->get<std::string>();
}

bool optional_bool(const Json &object, std::string_view key, bool fallback,
                   std::string_view path) {
  const auto it = object.find(key);
  if (it == object.end())
    return fallback;
  if (!it->is_boolean()) {
    throw ShapeError(CatalogErrorCode::invalid_shape,
                     std::string(path) + "." + std::string(key),
                     "optional member must be a boolean");
  }
  return it->get<bool>();
}

std::size_t optional_size(const Json &object, std::string_view key,
                          std::size_t fallback, std::string_view path) {
  const auto it = object.find(key);
  if (it == object.end())
    return fallback;
  if (!it->is_number_unsigned() && !it->is_number_integer()) {
    throw ShapeError(CatalogErrorCode::invalid_shape,
                     std::string(path) + "." + std::string(key),
                     "optional member must be a non-negative integer");
  }
  const auto value = it->get<long long>();
  if (value < 0) {
    throw ShapeError(CatalogErrorCode::invalid_shape,
                     std::string(path) + "." + std::string(key),
                     "optional member must be non-negative");
  }
  return static_cast<std::size_t>(value);
}

std::string required_string(const Json &object, std::string_view key,
                            std::string_view path) {
  auto value = require_member(object, key, Json::value_t::string, path)
                   .get<std::string>();
  if (value.empty()) {
    throw ShapeError(CatalogErrorCode::invalid_shape,
                     std::string(path) + "." + std::string(key),
                     "string must not be empty");
  }
  return value;
}

SafetyClass parse_safety(const Json &object, std::string_view path) {
  const auto text = optional_string(object, "safety", path);
  if (text.empty() || text == "safe")
    return SafetyClass::safe;
  if (text == "simulated")
    return SafetyClass::simulated;
  if (text == "dangerous")
    return SafetyClass::dangerous;
  throw ShapeError(CatalogErrorCode::invalid_shape,
                   std::string(path) + ".safety",
                   "unknown safety classification");
}

NodeKind parse_node_kind(std::string_view text, std::string_view path) {
  if (text == "category")
    return NodeKind::category;
  if (text == "menu")
    return NodeKind::menu;
  if (text == "action")
    return NodeKind::action;
  if (text == "setting")
    return NodeKind::setting;
  if (text == "media")
    return NodeKind::media;
  if (text == "landing")
    return NodeKind::landing;
  throw ShapeError(CatalogErrorCode::invalid_shape, std::string(path) + ".kind",
                   "unknown node kind");
}

std::vector<CatalogChoice> parse_choices(const Json &object,
                                         std::string_view path) {
  const auto it = object.find("choices");
  if (it == object.end())
    return {};
  if (!it->is_array()) {
    throw ShapeError(CatalogErrorCode::invalid_shape,
                     std::string(path) + ".choices",
                     "choices must be an array");
  }
  std::vector<CatalogChoice> choices;
  choices.reserve(it->size());
  std::unordered_map<std::string, bool> ids;
  for (std::size_t index = 0; index < it->size(); ++index) {
    const auto &value = (*it)[index];
    const auto choice_path =
        std::string(path) + ".choices[" + std::to_string(index) + "]";
    if (!value.is_object()) {
      throw ShapeError(CatalogErrorCode::invalid_shape, choice_path,
                       "choice must be an object");
    }
    CatalogChoice choice{
        required_string(value, "id", choice_path),
        required_string(value, "label_key", choice_path),
        required_string(value, "value", choice_path),
    };
    if (!ids.emplace(choice.id, true).second) {
      throw ShapeError(CatalogErrorCode::duplicate_id, choice_path + ".id",
                       "duplicate choice id");
    }
    choices.push_back(std::move(choice));
  }
  return choices;
}

std::vector<std::string> parse_string_array(const Json &object,
                                            std::string_view key,
                                            std::string_view path) {
  const auto it = object.find(key);
  if (it == object.end())
    return {};
  if (!it->is_array()) {
    throw ShapeError(CatalogErrorCode::invalid_shape,
                     std::string(path) + "." + std::string(key),
                     "member must be an array");
  }
  std::vector<std::string> result;
  result.reserve(it->size());
  for (std::size_t index = 0; index < it->size(); ++index) {
    if (!(*it)[index].is_string() ||
        (*it)[index].get_ref<const std::string &>().empty()) {
      throw ShapeError(CatalogErrorCode::invalid_shape,
                       std::string(path) + "." + std::string(key) + "[" +
                           std::to_string(index) + "]",
                       "array entry must be a non-empty string");
    }
    result.push_back((*it)[index].get<std::string>());
  }
  return result;
}

template <class Map, class Value>
void insert_unique(Map &map, Value value, std::string_view path) {
  const auto id = value.id;
  if (!map.emplace(id, std::move(value)).second) {
    throw ShapeError(CatalogErrorCode::duplicate_id, std::string(path) + ".id",
                     "duplicate stable semantic id");
  }
}

void require_translation(const Catalog &catalog, std::string_view key,
                         std::string_view path) {
  if (!key.empty() &&
      catalog.strings.find(std::string(key)) == catalog.strings.end()) {
    throw ShapeError(CatalogErrorCode::missing_translation, std::string(path),
                     "translation key is not present in the locale catalog: " +
                         std::string(key));
  }
}

void validate_choice_translations(const Catalog &catalog,
                                  const std::vector<CatalogChoice> &choices,
                                  std::string_view path) {
  for (std::size_t index = 0; index < choices.size(); ++index) {
    require_translation(catalog, choices[index].label_key,
                        std::string(path) + ".choices[" +
                            std::to_string(index) + "]");
  }
}

void validate_catalog(const Catalog &catalog) {
  if (catalog.root_order.size() != 9) {
    throw ShapeError(
        CatalogErrorCode::invalid_shape, "$.root_order",
        "the reference catalogue must have exactly nine root categories");
  }
  std::unordered_map<std::string, bool> roots;
  for (std::size_t index = 0; index < catalog.root_order.size(); ++index) {
    const auto &id = catalog.root_order[index];
    const auto it = catalog.nodes.find(id);
    if (it == catalog.nodes.end()) {
      throw ShapeError(CatalogErrorCode::unresolved_reference,
                       "$.root_order[" + std::to_string(index) + "]",
                       "root node does not exist");
    }
    if (it->second.kind != NodeKind::category ||
        !roots.emplace(id, true).second) {
      throw ShapeError(CatalogErrorCode::invalid_shape,
                       "$.root_order[" + std::to_string(index) + "]",
                       "root entries must be unique category nodes");
    }
  }
  for (const auto &[id, node] : catalog.nodes) {
    const auto path = "$.nodes." + id;
    require_translation(catalog, node.label_key, path + ".label_key");
    require_translation(catalog, node.description_key,
                        path + ".description_key");
    require_translation(catalog, node.value_key, path + ".value_key");
    validate_choice_translations(catalog, node.choices, path);
    for (const auto &child : node.children) {
      if (catalog.nodes.find(child) == catalog.nodes.end()) {
        throw ShapeError(CatalogErrorCode::unresolved_reference,
                         path + ".children",
                         "child node does not exist: " + child);
      }
    }
  }
  for (const auto &[id, dialog] : catalog.dialogs) {
    const auto path = "$.dialogs." + id;
    require_translation(catalog, dialog.title_key, path + ".title_key");
    require_translation(catalog, dialog.body_key, path + ".body_key");
    validate_choice_translations(catalog, dialog.choices, path);
  }
  for (const auto &[id, wizard] : catalog.wizards) {
    const auto path = "$.wizards." + id;
    require_translation(catalog, wizard.title_key, path + ".title_key");
    require_translation(catalog, wizard.body_key, path + ".body_key");
    require_translation(catalog, wizard.label_key, path + ".label_key");
    validate_choice_translations(catalog, wizard.choices, path);
    for (const auto &next : wizard.next_ids) {
      if (next != "__close__" &&
          catalog.wizards.find(next) == catalog.wizards.end()) {
        throw ShapeError(CatalogErrorCode::unresolved_reference,
                         path + ".next_ids",
                         "wizard destination does not exist: " + next);
      }
    }
  }
  for (const auto &[id, control] : catalog.controls) {
    const auto path = "$.controls." + id;
    require_translation(catalog, control.label_key, path + ".label_key");
    validate_choice_translations(catalog, control.choices, path);
  }
  for (const auto &[id, landing] : catalog.landings) {
    const auto path = "$.landings." + id;
    require_translation(catalog, landing.title_key, path + ".title_key");
    require_translation(catalog, landing.body_key, path + ".body_key");
  }
}

} // namespace

const CatalogNode *Catalog::find_node(std::string_view id) const noexcept {
  const auto it = nodes.find(id);
  return it == nodes.end() ? nullptr : &it->second;
}

const WizardScreen *Catalog::find_wizard(std::string_view id) const noexcept {
  const auto it = wizards.find(id);
  return it == wizards.end() ? nullptr : &it->second;
}

std::string_view Catalog::text(std::string_view key) const noexcept {
  const auto it = strings.find(key);
  return it == strings.end() ? std::string_view{}
                             : std::string_view(it->second);
}

CatalogLoadResult load_catalog_json(std::string_view json,
                                    std::string_view source_name) noexcept {
  try {
    const Json root = Json::parse(json.begin(), json.end());
    if (!root.is_object()) {
      throw ShapeError(CatalogErrorCode::invalid_shape, "$",
                       "catalog root must be an object");
    }

    Catalog catalog;
    const auto schema = root.find("schema_version");
    if (schema == root.end() || !schema->is_number_integer()) {
      throw ShapeError(CatalogErrorCode::invalid_shape, "$.schema_version",
                       "schema_version must be an integer");
    }
    catalog.schema_version = schema->get<int>();
    if (catalog.schema_version != 1) {
      throw ShapeError(CatalogErrorCode::unsupported_schema, "$.schema_version",
                       "only native XMB catalog schema version 1 is supported");
    }
    catalog.locale = required_string(root, "locale", "$");
    catalog.source_commit = required_string(root, "source_commit", "$");
    catalog.root_order = parse_string_array(root, "root_order", "$");

    const auto &strings =
        require_member(root, "strings", Json::value_t::object, "$");
    for (const auto &[key, value] : strings.items()) {
      if (key.empty() || !value.is_string()) {
        throw ShapeError(CatalogErrorCode::invalid_shape, "$.strings." + key,
                         "translation entries must be non-empty string pairs");
      }
      catalog.strings.emplace(key, value.get<std::string>());
    }

    const auto &nodes =
        require_member(root, "nodes", Json::value_t::array, "$");
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      const auto &value = nodes[index];
      const auto path = "$.nodes[" + std::to_string(index) + "]";
      if (!value.is_object()) {
        throw ShapeError(CatalogErrorCode::invalid_shape, path,
                         "node must be an object");
      }
      CatalogNode node;
      node.id = required_string(value, "id", path);
      node.kind = parse_node_kind(required_string(value, "kind", path), path);
      node.label_key = required_string(value, "label_key", path);
      node.description_key = optional_string(value, "description_key", path);
      node.value_key = optional_string(value, "value_key", path);
      node.icon_ref = optional_string(value, "icon_ref", path);
      node.action_id = optional_string(value, "action_id", path);
      node.persistence_key = optional_string(value, "persistence_key", path);
      node.capability = optional_string(value, "capability", path);
      node.optional_local_media_ref =
          optional_string(value, "optional_local_media_ref", path);
      node.safety = parse_safety(value, path);
      node.restricted_media =
          optional_bool(value, "restricted_media", false, path);
      node.default_selection =
          optional_size(value, "default_selection", 0, path);
      node.choices = parse_choices(value, path);
      node.children = parse_string_array(value, "children", path);
      insert_unique(catalog.nodes, std::move(node), path);
    }

    const auto &dialogs =
        require_member(root, "dialogs", Json::value_t::array, "$");
    for (std::size_t index = 0; index < dialogs.size(); ++index) {
      const auto &value = dialogs[index];
      const auto path = "$.dialogs[" + std::to_string(index) + "]";
      if (!value.is_object())
        throw ShapeError(CatalogErrorCode::invalid_shape, path,
                         "dialog must be an object");
      DialogTemplate dialog;
      dialog.id = required_string(value, "id", path);
      dialog.type = required_string(value, "type", path);
      dialog.title_key = required_string(value, "title_key", path);
      dialog.body_key = optional_string(value, "body_key", path);
      dialog.illustration_ref =
          optional_string(value, "illustration_ref", path);
      dialog.capability = optional_string(value, "capability", path);
      dialog.safety = parse_safety(value, path);
      dialog.side_panel = optional_bool(value, "side_panel", false, path);
      dialog.choices = parse_choices(value, path);
      insert_unique(catalog.dialogs, std::move(dialog), path);
    }

    const auto &wizards =
        require_member(root, "wizards", Json::value_t::array, "$");
    for (std::size_t index = 0; index < wizards.size(); ++index) {
      const auto &value = wizards[index];
      const auto path = "$.wizards[" + std::to_string(index) + "]";
      if (!value.is_object())
        throw ShapeError(CatalogErrorCode::invalid_shape, path,
                         "wizard screen must be an object");
      WizardScreen screen;
      screen.id = required_string(value, "id", path);
      screen.kind = required_string(value, "kind", path);
      screen.title_key = required_string(value, "title_key", path);
      screen.body_key = optional_string(value, "body_key", path);
      screen.label_key = optional_string(value, "label_key", path);
      screen.persistence_key = optional_string(value, "persistence_key", path);
      screen.capability = optional_string(value, "capability", path);
      screen.safety = parse_safety(value, path);
      screen.transient = optional_bool(value, "transient", false, path);
      screen.optional = optional_bool(value, "optional", false, path);
      screen.choices = parse_choices(value, path);
      screen.next_ids = parse_string_array(value, "next_ids", path);
      insert_unique(catalog.wizards, std::move(screen), path);
    }

    const auto &controls =
        require_member(root, "controls", Json::value_t::array, "$");
    for (std::size_t index = 0; index < controls.size(); ++index) {
      const auto &value = controls[index];
      const auto path = "$.controls[" + std::to_string(index) + "]";
      if (!value.is_object())
        throw ShapeError(CatalogErrorCode::invalid_shape, path,
                         "control must be an object");
      ControlEntry control;
      control.id = required_string(value, "id", path);
      control.surface = required_string(value, "surface", path);
      control.label_key = required_string(value, "label_key", path);
      control.action_id = required_string(value, "action_id", path);
      control.capability = optional_string(value, "capability", path);
      control.safety = parse_safety(value, path);
      control.choices = parse_choices(value, path);
      insert_unique(catalog.controls, std::move(control), path);
    }

    const auto &landings =
        require_member(root, "landings", Json::value_t::array, "$");
    for (std::size_t index = 0; index < landings.size(); ++index) {
      const auto &value = landings[index];
      const auto path = "$.landings[" + std::to_string(index) + "]";
      if (!value.is_object())
        throw ShapeError(CatalogErrorCode::invalid_shape, path,
                         "landing must be an object");
      LandingTemplate landing;
      landing.id = required_string(value, "id", path);
      landing.title_key = required_string(value, "title_key", path);
      landing.body_key = required_string(value, "body_key", path);
      landing.hero_ref = optional_string(value, "hero_ref", path);
      landing.optional_local_media_ref =
          optional_string(value, "optional_local_media_ref", path);
      landing.capability = optional_string(value, "capability", path);
      insert_unique(catalog.landings, std::move(landing), path);
    }

    validate_catalog(catalog);
    return {std::move(catalog), std::nullopt};
  } catch (const Json::parse_error &error) {
    return {std::nullopt,
            CatalogError{CatalogErrorCode::malformed_json,
                         std::string(source_name), error.what(), error.byte}};
  } catch (const ShapeError &error) {
    return {std::nullopt,
            CatalogError{error.code, error.path, error.what(), 0}};
  } catch (const std::exception &error) {
    return {std::nullopt,
            CatalogError{CatalogErrorCode::invalid_shape,
                         std::string(source_name), error.what(), 0}};
  } catch (...) {
    return {std::nullopt, CatalogError{CatalogErrorCode::invalid_shape,
                                       std::string(source_name),
                                       "unknown catalogue load failure", 0}};
  }
}

CatalogLoadResult
load_catalog_file(const std::filesystem::path &path) noexcept {
  try {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
      return {std::nullopt,
              CatalogError{CatalogErrorCode::io, path.string(),
                           "catalogue file could not be opened", 0}};
    }
    std::string contents((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
    if (!stream.good() && !stream.eof()) {
      return {std::nullopt,
              CatalogError{CatalogErrorCode::io, path.string(),
                           "catalogue file could not be read completely", 0}};
    }
    return load_catalog_json(contents, path.string());
  } catch (const std::exception &error) {
    return {std::nullopt,
            CatalogError{CatalogErrorCode::io, path.string(), error.what(), 0}};
  } catch (...) {
    return {std::nullopt, CatalogError{CatalogErrorCode::io, path.string(),
                                       "unknown catalogue file failure", 0}};
  }
}

} // namespace openxmb::xmb
