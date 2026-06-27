#include "openxmb/localization/catalog.hpp"
#include "openxmb/localization/locale.hpp"
#include "openxmb/localization/message.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace openxmb::localization;

void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

template <typename T>
void require(const LocalizationResult<T> &result, std::string_view message) {
  require(result.has_value(), message);
}

template <typename Actual, typename Expected>
void require_equal(const Actual &actual, const Expected &expected,
                   std::string_view message) {
  if (!(actual == expected))
    throw std::runtime_error(std::string(message));
}

std::filesystem::path fixture_path(std::string_view name) {
  auto candidate = std::filesystem::current_path();
  while (!candidate.empty()) {
    const auto fixture =
        candidate / "tests" / "fixtures" / "localization" / name;
    if (std::filesystem::exists(fixture))
      return fixture;
    const auto parent = candidate.parent_path();
    if (parent == candidate)
      break;
    candidate = parent;
  }
  const auto source = std::filesystem::absolute(__FILE__);
  return source.parent_path().parent_path() / "fixtures" / "localization" /
         name;
}

CatalogLayer load_fixture(std::string locale, std::string_view name) {
  const auto path = fixture_path(name);
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "localization test fixture opens");

  CatalogLayer layer{.locale = std::move(locale),
                     .messages = {},
                     .source_name = path.string()};
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line.front() == '#')
      continue;
    const auto tab = line.find('\t');
    require(tab != std::string::npos, "fixture line has a tab separator");
    require(layer.messages.emplace(line.substr(0, tab), line.substr(tab + 1))
                .second,
            "fixture keys are unique");
  }
  return layer;
}

LocalizationCatalog representative_catalog() {
  auto result =
      LocalizationCatalog::create(load_fixture("en_gb", "base.en-GB.tsv"));
  require(result, "source localization fixture creates a catalog");
  auto catalog = std::move(result).value();
  require(catalog.overlay(load_fixture("fr", "fr.tsv")),
          "French partial overlay is valid");
  require(catalog.overlay(load_fixture("fr_ca", "fr-CA.tsv")),
          "Canadian French partial overlay is valid");
  return catalog;
}

void test_locale_normalization() {
  require_equal(normalize_locale_tag("EN_latn_us").value(),
                std::string("en-Latn-US"),
                "language, script, and region casing is canonical");
  require_equal(normalize_locale_tag("sr_cYRL_rs").value(),
                std::string("sr-Cyrl-RS"), "mixed separators normalize");
  require_equal(normalize_locale_tag("de-DE-u-CO-phonebk").value(),
                std::string("de-DE-u-co-phonebk"),
                "Unicode extension values normalize to lowercase");
  require_equal(normalize_locale_tag("zh-Hant-TW-x-OpenXMB").value(),
                std::string("zh-Hant-TW-x-openxmb"),
                "private-use values normalize to lowercase");
  require_equal(normalize_locale_tag("es-419").value(), std::string("es-419"),
                "numeric regions are retained");
  require_equal(normalize_locale_tag("EN-x-a").value(), std::string("en-x-a"),
                "one-character private-use values are accepted");
  require_equal(normalize_locale_tag("X-Internal-Test").value(),
                std::string("x-internal-test"),
                "private-use-only locale tags normalize");

  const std::vector<std::string_view> invalid{"",
                                              "e",
                                              "-en",
                                              "en-",
                                              "en--US",
                                              "en GB",
                                              "日本語",
                                              "englishish",
                                              "en-u",
                                              "en-u-ca-u-nu",
                                              "en-variant-variant",
                                              "en-US-GB",
                                              "en-Latn-Cyrl",
                                              "en-US-abc",
                                              "en-12",
                                              "x"};
  for (const auto tag : invalid) {
    const auto result = normalize_locale_tag(tag);
    require(!result, "malformed locale tag is rejected");
    require_equal(result.error().code, LocalizationErrorCode::invalid_locale,
                  "malformed tag has a typed locale error");
  }

  require(is_pseudo_locale("en-XA"), "expansion pseudo-locale is recognized");
  require(is_pseudo_locale("ar-XB"), "RTL pseudo-locale is recognized");
  require(!is_pseudo_locale("en-US"), "ordinary locale is not pseudo");
}

void test_fallback_chains() {
  auto chain = locale_fallback_chain("zh-Hant-TW", "en-GB");
  require(chain, "specific locale creates a fallback chain");
  require_equal(
      chain.value(),
      std::vector<std::string>{"zh-Hant-TW", "zh-Hant", "zh", "en-GB", "en"},
      "script and region parents precede default locale parents");

  chain = locale_fallback_chain("de-DE-u-co-phonebk", "en");
  require_equal(
      chain.value(),
      std::vector<std::string>{"de-DE-u-co-phonebk", "de-DE", "de", "en"},
      "extension fallback is atomic");

  chain = locale_fallback_chain("en-US", "en");
  require_equal(chain.value(), std::vector<std::string>{"en-US", "en"},
                "duplicate default parent is removed deterministically");
  chain = locale_fallback_chain("x-internal-test", "en");
  require_equal(chain.value(),
                std::vector<std::string>{"x-internal-test", "en"},
                "private-use-only tags fall back atomically");
  require(!locale_fallback_chain("bad tag", "en"),
          "invalid requested locale does not silently use the default");
  require(!locale_fallback_chain("fr", "bad tag"),
          "invalid configured default locale is rejected");
}

void test_utf8_validation() {
  require(is_valid_utf8("Music • 日本語 • Ελληνικά"),
          "multilingual fixture is valid UTF-8");
  require(is_valid_utf8("emoji: \xf0\x9f\x8e\xae"),
          "four-byte scalar is valid UTF-8");
  require(!is_valid_utf8(std::string("\xc0\xaf", 2)),
          "overlong UTF-8 encoding is rejected");
  require(!is_valid_utf8(std::string("\xed\xa0\x80", 3)),
          "UTF-8 surrogate scalar is rejected");
  require(!is_valid_utf8(std::string("\xf4\x90\x80\x80", 4)),
          "scalar beyond U+10FFFF is rejected");
  require(!is_valid_utf8(std::string("\xe2\x82", 2)),
          "truncated UTF-8 sequence is rejected");
  require(!is_valid_utf8(std::string("\xe2\x28\xa1", 3)),
          "bad continuation byte is rejected");
}

void test_placeholders() {
  auto signature = analyze_placeholders(
      "{name} copied {count:03} files for {name}; {{literal}}");
  require(signature, "named placeholders and escaped braces parse");
  require_equal(signature.value().placeholders,
                std::vector<PlaceholderUse>{{"count", 1}, {"name", 2}},
                "placeholder signature is sorted and counts repetitions");
  require(validate_placeholder_compatibility(
              "Copied {count} of {total}",
              "Sur {total}, {count} fichiers ont été copiés"),
          "translations may reorder placeholders");
  require(validate_placeholder_compatibility("Value {count:03}",
                                             "Valeur {count:d}"),
          "translation may use a locale-appropriate format specifier");

  auto mismatch = validate_placeholder_compatibility(
      "Copied {count} of {total}", "{count} fichiers copiés");
  require(!mismatch, "missing translation placeholder is rejected");
  require_equal(mismatch.error().code,
                LocalizationErrorCode::placeholder_mismatch,
                "placeholder mismatch has a typed error");
  require_equal(mismatch.error().expected_placeholders,
                std::vector<std::string>{"count", "total"},
                "mismatch diagnostic includes expected placeholder names");
  require_equal(mismatch.error().actual_placeholders,
                std::vector<std::string>{"count"},
                "mismatch diagnostic includes actual placeholder names");

  const std::vector<std::string_view> malformed{
      "Missing {name", "Unexpected name}",      "Empty {}",
      "Bad {0name}",   "Nested {outer{inner}}", "Empty format {count:}"};
  for (const auto message : malformed) {
    const auto result = analyze_placeholders(message);
    require(!result, "malformed placeholder syntax is rejected");
    require_equal(result.error().code,
                  LocalizationErrorCode::malformed_placeholder,
                  "malformed placeholder has a typed error");
  }
}

void test_pseudo_locales() {
  const std::string source = "Hello, {name}! Use {{confirm}}.";
  auto expansion = pseudo_localize(source, PseudoLocaleKind::expansion);
  require(expansion, "expansion pseudo-localization succeeds");
  require(is_valid_utf8(expansion.value()),
          "expansion pseudo-localization remains valid UTF-8");
  require(expansion.value().starts_with("\xe2\x9f\xa6") &&
              expansion.value().ends_with("\xe2\x9f\xa7"),
          "expansion result has visible boundary markers");
  require(expansion.value().contains("{name}") &&
              expansion.value().contains("{{") &&
              expansion.value().contains("}}"),
          "expansion preserves placeholder spelling and brace escapes");
  require(expansion.value().size() > source.size(),
          "expansion pseudo-locale stresses available width");

  auto rtl = pseudo_localize(source, PseudoLocaleKind::right_to_left);
  require(rtl, "RTL pseudo-localization succeeds");
  require(is_valid_utf8(rtl.value()),
          "RTL pseudo-localization remains valid UTF-8");
  require(rtl.value().starts_with("\xe2\x81\xa7") &&
              rtl.value().ends_with("\xe2\x81\xa9"),
          "RTL result uses isolate markers rather than global bidi overrides");
  require(rtl.value().contains("{name}") && rtl.value().contains("{{") &&
              rtl.value().contains("}}"),
          "RTL pseudo-locale preserves formatting syntax");
  require(rtl.value() != source, "RTL pseudo-locale transforms visible text");

  require(!pseudo_localize("Broken {name", PseudoLocaleKind::expansion),
          "pseudo-localization rejects malformed formatting syntax");
}

void test_catalog_lookup_and_fallback() {
  auto catalog = representative_catalog();
  require_equal(catalog.default_locale(), std::string("en-GB"),
                "catalog stores canonical default locale");
  require_equal(catalog.base_message_count(), std::size_t(6),
                "all clean-room source messages are indexed");

  auto message = catalog.lookup("menu.greeting", "FR_ca");
  require(message, "specific translated message resolves");
  require_equal(message.value().text, std::string("Allô, {name} !"),
                "most-specific overlay wins");
  require_equal(message.value().trace.resolved_locale, std::string("fr-CA"),
                "lookup reports canonical resolved locale");
  require(!message.value().trace.used_fallback,
          "direct locale hit is not marked as fallback");

  message = catalog.lookup("menu.progress", "fr-CA");
  require_equal(message.value().text,
                std::string("{count} fichiers copiés sur {total}"),
                "missing regional string falls back to language overlay");
  require_equal(message.value().trace.resolved_locale, std::string("fr"),
                "language fallback is observable");
  require(message.value().trace.used_fallback,
          "parent-locale resolution is marked as fallback");

  message = catalog.lookup("menu.unicode", "fr-CA");
  require_equal(
      message.value().text, std::string("Music • 日本語 • Ελληνικά"),
      "partial translations reach the source locale deterministically");
  require_equal(message.value().trace.attempted_locales,
                std::vector<std::string>{"fr-CA", "fr", "en-GB", "en"},
                "lookup exposes its complete deterministic search chain");

  const auto missing = catalog.lookup("menu.does.not.exist", "fr-CA");
  require(!missing, "unknown key produces a diagnostic result");
  require_equal(missing.error().code, LocalizationErrorCode::missing_key,
                "unknown key has typed missing-key status");
  require_equal(missing.error().key, std::string("menu.does.not.exist"),
                "missing-key diagnostic retains the requested key");
  require_equal(missing.error().attempted_locales,
                std::vector<std::string>{"fr-CA", "fr", "en-GB", "en"},
                "missing-key diagnostic retains every attempted locale");
  require(!catalog.lookup("bad key", "en-GB"),
          "invalid lookup key is rejected before traversal");
  require(!catalog.lookup("menu..plain", "en-GB"),
          "lookup key cannot contain empty path segments");
  require(!catalog.lookup("menu.plain", "bad locale"),
          "invalid requested locale never silently resolves source text");

  require_equal(
      catalog.available_locales(),
      std::vector<std::string>{"ar-XB", "en-GB", "en-XA", "fr", "fr-CA"},
      "available locales are sorted and include built-in stress modes");
}

void test_catalog_overlay_validation_and_atomicity() {
  auto catalog = representative_catalog();

  CatalogLayer malformed{.locale = "de",
                         .messages = {{"menu.plain", "Einstellungen"},
                                      {"menu.unknown", "Unbekannt"}},
                         .source_name = "atomicity-test"};
  auto result = catalog.overlay(std::move(malformed));
  require(!result, "overlay containing an unknown key is rejected");
  require_equal(result.error().code,
                LocalizationErrorCode::unknown_translation_key,
                "unknown overlay key has typed status");
  auto lookup = catalog.lookup("menu.plain", "de");
  require_equal(lookup.value().trace.resolved_locale, std::string("en-GB"),
                "rejected layer does not leak earlier valid entries");

  result = catalog.overlay({.locale = "de",
                            .messages = {{"menu.greeting", "Hallo!"}},
                            .source_name = "placeholder-test"});
  require(!result, "overlay that drops a placeholder is rejected");
  require_equal(result.error().code,
                LocalizationErrorCode::placeholder_mismatch,
                "catalog propagates typed placeholder mismatch");
  require_equal(result.error().locale, std::string("de"),
                "placeholder diagnostic names overlay locale");
  require_equal(result.error().key, std::string("menu.greeting"),
                "placeholder diagnostic names overlay key");

  const std::string invalid_utf8("\xc0\xaf", 2);
  result = catalog.overlay({.locale = "de",
                            .messages = {{"menu.plain", invalid_utf8}},
                            .source_name = "utf8-test"});
  require(!result, "invalid UTF-8 overlay is rejected");
  require_equal(result.error().code, LocalizationErrorCode::invalid_utf8,
                "invalid translation encoding has typed status");

  require(catalog.overlay({.locale = "de",
                           .messages = {{"menu.plain", "Systemeinstellungen"}},
                           .source_name = "first"}),
          "valid German translation overlays");
  result = catalog.overlay({.locale = "de",
                            .messages = {{"menu.plain", "Einstellungen"}},
                            .source_name = "duplicate"},
                           OverlayPolicy::reject_existing);
  require(!result, "reject-existing policy protects loaded translations");
  require_equal(result.error().code,
                LocalizationErrorCode::duplicate_translation,
                "duplicate overlay has typed status");
  require(catalog.overlay({.locale = "de",
                           .messages = {{"menu.plain", "Einstellungen"}},
                           .source_name = "replacement"}),
          "default overlay policy permits intentional replacement");
  require_equal(catalog.lookup("menu.plain", "de").value().text,
                std::string("Einstellungen"),
                "replacement overlay becomes observable");

  result = catalog.overlay({.locale = "en-XA",
                            .messages = {{"menu.plain", "Forbidden"}},
                            .source_name = "reserved"});
  require(!result, "generated pseudo-locale cannot be shadowed by data");
}

void test_catalog_pseudo_lookup() {
  auto catalog = representative_catalog();
  auto expansion = catalog.lookup("menu.greeting", "en-xa");
  require(expansion, "catalog produces expansion stress locale on demand");
  require(expansion.value().trace.pseudo_localized,
          "lookup trace identifies generated text");
  require_equal(expansion.value().trace.requested_locale, std::string("en-XA"),
                "pseudo request is canonicalized");
  require_equal(expansion.value().trace.resolved_locale, std::string("en-GB"),
                "trace identifies source used to generate pseudo text");
  require(expansion.value().text.contains("{name}"),
          "catalog pseudo lookup preserves placeholder");

  auto rtl = catalog.lookup("menu.greeting", "AR_xb");
  require(rtl && rtl.value().trace.pseudo_localized,
          "catalog produces RTL stress locale on demand");
  require(is_valid_utf8(rtl.value().text),
          "generated catalog RTL text remains valid UTF-8");
}

void test_invalid_source_catalogs() {
  require(!LocalizationCatalog::create(
              {.locale = "en", .messages = {}, .source_name = "empty"}),
          "empty source catalog is rejected");
  require(!LocalizationCatalog::create({.locale = "en-XA",
                                        .messages = {{"key", "Value"}},
                                        .source_name = "pseudo-base"}),
          "pseudo-locale cannot be configured as source catalog");
  require(!LocalizationCatalog::create({.locale = "bad locale",
                                        .messages = {{"key", "Value"}},
                                        .source_name = "bad-locale"}),
          "invalid source locale is rejected");
  require(!LocalizationCatalog::create({.locale = "en",
                                        .messages = {{"bad key", "Value"}},
                                        .source_name = "bad-key"}),
          "invalid source key is rejected");
  require(!LocalizationCatalog::create({.locale = "en",
                                        .messages = {{"key", "Broken {name"}},
                                        .source_name = "bad-message"}),
          "malformed source formatting syntax is rejected");
}

} // namespace

int main() {
  try {
    test_locale_normalization();
    test_fallback_chains();
    test_utf8_validation();
    test_placeholders();
    test_pseudo_locales();
    test_catalog_lookup_and_fallback();
    test_catalog_overlay_validation_and_atomicity();
    test_catalog_pseudo_lookup();
    test_invalid_source_catalogs();
    std::cout << "OpenXMB localization core tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "OpenXMB localization core test failure: " << error.what()
              << '\n';
    return 1;
  }
}
