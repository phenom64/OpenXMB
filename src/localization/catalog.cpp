#include "openxmb/localization/catalog.hpp"

#include "openxmb/localization/locale.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace openxmb::localization {
namespace {

[[nodiscard]] bool valid_key(std::string_view key) noexcept {
  if (key.empty() || key.front() == '.' || key.back() == '.')
    return false;
  bool previous_dot = false;
  for (const auto character : key) {
    const bool alpha = (character >= 'A' && character <= 'Z') ||
                       (character >= 'a' && character <= 'z');
    const bool digit = character >= '0' && character <= '9';
    if (!alpha && !digit && character != '_' && character != '-' &&
        character != '.')
      return false;
    if (character == '.' && previous_dot)
      return false;
    previous_dot = character == '.';
  }
  return true;
}

[[nodiscard]] LocalizationError catalog_error(LocalizationErrorCode code,
                                              std::string operation,
                                              std::string detail,
                                              std::string locale = {},
                                              std::string key = {}) {
  LocalizationError error;
  error.code = code;
  error.operation = std::move(operation);
  error.detail = std::move(detail);
  error.locale = std::move(locale);
  error.key = std::move(key);
  return error;
}

[[nodiscard]] LocalizationResult<PlaceholderSignature>
validate_message(std::string_view key, std::string_view text,
                 std::string_view locale, std::string_view operation) {
  if (!valid_key(key)) {
    return LocalizationResult<PlaceholderSignature>::failure(catalog_error(
        LocalizationErrorCode::invalid_key, std::string(operation),
        "message key must be a non-empty portable ASCII identifier",
        std::string(locale), std::string(key)));
  }
  if (!is_valid_utf8(text)) {
    return LocalizationResult<PlaceholderSignature>::failure(catalog_error(
        LocalizationErrorCode::invalid_utf8, std::string(operation),
        "localized message is not well-formed UTF-8", std::string(locale),
        std::string(key)));
  }
  auto signature = analyze_placeholders(text);
  if (!signature) {
    auto error = signature.error();
    error.operation = std::string(operation);
    error.locale = std::string(locale);
    error.key = std::string(key);
    return LocalizationResult<PlaceholderSignature>::failure(std::move(error));
  }
  return signature;
}

} // namespace

LocalizationResult<LocalizationCatalog>
LocalizationCatalog::create(CatalogLayer base_layer) {
  auto locale = normalize_locale_tag(base_layer.locale);
  if (!locale)
    return LocalizationResult<LocalizationCatalog>::failure(locale.error());
  if (is_pseudo_locale(locale.value())) {
    return LocalizationResult<LocalizationCatalog>::failure(catalog_error(
        LocalizationErrorCode::invalid_locale, "LocalizationCatalog::create",
        "a generated pseudo-locale cannot be the source catalog locale",
        locale.value()));
  }
  if (base_layer.messages.empty()) {
    return LocalizationResult<LocalizationCatalog>::failure(catalog_error(
        LocalizationErrorCode::invalid_key, "LocalizationCatalog::create",
        "source catalog must contain at least one message", locale.value()));
  }

  LocalizationCatalog catalog;
  catalog.default_locale_ = locale.value();
  MessageMap normalized_messages;
  for (auto &[key, text] : base_layer.messages) {
    auto signature = validate_message(key, text, locale.value(),
                                      "LocalizationCatalog::create");
    if (!signature)
      return LocalizationResult<LocalizationCatalog>::failure(
          signature.error());
    catalog.base_signatures_.emplace(key, std::move(signature).value());
    normalized_messages.emplace(std::move(key), std::move(text));
  }
  catalog.layers_.emplace(catalog.default_locale_,
                          std::move(normalized_messages));
  return LocalizationResult<LocalizationCatalog>::success(std::move(catalog));
}

LocalizationResult<void> LocalizationCatalog::overlay(CatalogLayer layer,
                                                      OverlayPolicy policy) {
  auto locale = normalize_locale_tag(layer.locale);
  if (!locale)
    return LocalizationResult<void>::failure(locale.error());
  if (is_pseudo_locale(locale.value())) {
    return LocalizationResult<void>::failure(catalog_error(
        LocalizationErrorCode::invalid_locale, "LocalizationCatalog::overlay",
        "generated pseudo-locales cannot be replaced by catalog layers",
        locale.value()));
  }

  // Validate into a temporary map so a rejected layer never partially mutates
  // a live catalog.
  MessageMap validated;
  const auto existing_layer = layers_.find(locale.value());
  for (auto &[key, text] : layer.messages) {
    const auto base = base_signatures_.find(key);
    if (base == base_signatures_.end()) {
      return LocalizationResult<void>::failure(
          catalog_error(LocalizationErrorCode::unknown_translation_key,
                        "LocalizationCatalog::overlay",
                        "translation key does not exist in the source catalog",
                        locale.value(), key));
    }
    auto signature = validate_message(key, text, locale.value(),
                                      "LocalizationCatalog::overlay");
    if (!signature)
      return LocalizationResult<void>::failure(signature.error());
    if (signature.value() != base->second) {
      auto error = catalog_error(
          LocalizationErrorCode::placeholder_mismatch,
          "LocalizationCatalog::overlay",
          "translation placeholders do not match the source message",
          locale.value(), key);
      for (const auto &placeholder : base->second.placeholders) {
        for (std::size_t count = 0; count < placeholder.count; ++count)
          error.expected_placeholders.push_back(placeholder.name);
      }
      for (const auto &placeholder : signature.value().placeholders) {
        for (std::size_t count = 0; count < placeholder.count; ++count)
          error.actual_placeholders.push_back(placeholder.name);
      }
      return LocalizationResult<void>::failure(std::move(error));
    }
    if (policy == OverlayPolicy::reject_existing &&
        existing_layer != layers_.end() &&
        existing_layer->second.contains(key)) {
      return LocalizationResult<void>::failure(
          catalog_error(LocalizationErrorCode::duplicate_translation,
                        "LocalizationCatalog::overlay",
                        "overlay would replace an existing localized message",
                        locale.value(), key));
    }
    validated.emplace(std::move(key), std::move(text));
  }

  auto &destination = layers_[locale.value()];
  for (auto &[key, text] : validated)
    destination.insert_or_assign(std::move(key), std::move(text));
  return LocalizationResult<void>::success();
}

LocalizationResult<LocalizedMessage>
LocalizationCatalog::lookup(std::string_view key,
                            std::string_view requested_locale) const {
  if (!valid_key(key)) {
    return LocalizationResult<LocalizedMessage>::failure(catalog_error(
        LocalizationErrorCode::invalid_key, "LocalizationCatalog::lookup",
        "lookup key must be a non-empty portable ASCII identifier", {},
        std::string(key)));
  }
  auto normalized = normalize_locale_tag(requested_locale);
  if (!normalized)
    return LocalizationResult<LocalizedMessage>::failure(normalized.error());

  if (is_pseudo_locale(normalized.value())) {
    const auto base_layer = layers_.find(default_locale_);
    const auto source = base_layer->second.find(key);
    if (source == base_layer->second.end()) {
      auto error = catalog_error(
          LocalizationErrorCode::missing_key, "LocalizationCatalog::lookup",
          "message key is absent from the source catalog", normalized.value(),
          std::string(key));
      error.attempted_locales = {normalized.value(), default_locale_};
      return LocalizationResult<LocalizedMessage>::failure(std::move(error));
    }
    const auto kind = normalized.value() == expansion_pseudo_locale
                          ? PseudoLocaleKind::expansion
                          : PseudoLocaleKind::right_to_left;
    auto generated = pseudo_localize(source->second, kind);
    if (!generated)
      return LocalizationResult<LocalizedMessage>::failure(generated.error());
    return LocalizationResult<LocalizedMessage>::success(
        {.key = std::string(key),
         .text = std::move(generated).value(),
         .trace = {.requested_locale = normalized.value(),
                   .resolved_locale = default_locale_,
                   .attempted_locales = {normalized.value(), default_locale_},
                   .used_fallback = false,
                   .pseudo_localized = true}});
  }

  auto chain = locale_fallback_chain(normalized.value(), default_locale_);
  if (!chain)
    return LocalizationResult<LocalizedMessage>::failure(chain.error());
  for (std::size_t depth = 0; depth < chain.value().size(); ++depth) {
    const auto layer = layers_.find(chain.value()[depth]);
    if (layer == layers_.end())
      continue;
    const auto message = layer->second.find(key);
    if (message == layer->second.end())
      continue;
    return LocalizationResult<LocalizedMessage>::success(
        {.key = std::string(key),
         .text = message->second,
         .trace = {.requested_locale = normalized.value(),
                   .resolved_locale = layer->first,
                   .attempted_locales = chain.value(),
                   .used_fallback = depth != 0,
                   .pseudo_localized = false}});
  }

  auto error = catalog_error(LocalizationErrorCode::missing_key,
                             "LocalizationCatalog::lookup",
                             "message key was not found in any fallback locale",
                             normalized.value(), std::string(key));
  error.attempted_locales = chain.value();
  return LocalizationResult<LocalizedMessage>::failure(std::move(error));
}

std::vector<std::string> LocalizationCatalog::available_locales() const {
  std::vector<std::string> locales;
  locales.reserve(layers_.size() + 2);
  for (const auto &[locale, messages] : layers_) {
    static_cast<void>(messages);
    locales.push_back(locale);
  }
  locales.emplace_back(expansion_pseudo_locale);
  locales.emplace_back(rtl_pseudo_locale);
  std::ranges::sort(locales);
  return locales;
}

} // namespace openxmb::localization
