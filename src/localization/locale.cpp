#include "openxmb/localization/locale.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace openxmb::localization {
namespace {

[[nodiscard]] bool ascii_alpha(char value) noexcept {
  const auto byte = static_cast<unsigned char>(value);
  return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

[[nodiscard]] bool ascii_digit(char value) noexcept {
  const auto byte = static_cast<unsigned char>(value);
  return byte >= '0' && byte <= '9';
}

[[nodiscard]] bool ascii_alnum(char value) noexcept {
  return ascii_alpha(value) || ascii_digit(value);
}

[[nodiscard]] bool all_alpha(std::string_view value) noexcept {
  return std::ranges::all_of(value, ascii_alpha);
}

[[nodiscard]] bool all_digits(std::string_view value) noexcept {
  return std::ranges::all_of(value, ascii_digit);
}

[[nodiscard]] std::string lowercase(std::string value) {
  std::ranges::transform(value, value.begin(), [](char character) {
    if (character >= 'A' && character <= 'Z')
      return static_cast<char>(character + ('a' - 'A'));
    return character;
  });
  return value;
}

[[nodiscard]] char uppercase_ascii(char character) noexcept {
  if (character >= 'a' && character <= 'z')
    return static_cast<char>(character - ('a' - 'A'));
  return character;
}

[[nodiscard]] LocalizationError locale_error(std::string_view tag,
                                             std::string detail,
                                             std::size_t byte_offset = 0) {
  LocalizationError error;
  error.code = LocalizationErrorCode::invalid_locale;
  error.operation = "normalize_locale_tag";
  error.detail = std::move(detail);
  error.locale = std::string(tag);
  error.byte_offset = byte_offset;
  return error;
}

[[nodiscard]] std::vector<std::string> split(std::string_view normalized) {
  std::vector<std::string> parts;
  std::size_t begin = 0;
  while (begin < normalized.size()) {
    const auto end = normalized.find('-', begin);
    parts.emplace_back(normalized.substr(begin, end == std::string_view::npos
                                                    ? normalized.size() - begin
                                                    : end - begin));
    if (end == std::string_view::npos)
      break;
    begin = end + 1;
  }
  return parts;
}

[[nodiscard]] std::string join(const std::vector<std::string> &parts,
                               std::size_t count) {
  std::string result;
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0)
      result.push_back('-');
    result += parts[i];
  }
  return result;
}

void append_unique(std::vector<std::string> &chain, std::string value) {
  if (std::ranges::find(chain, value) == chain.end())
    chain.push_back(std::move(value));
}

void append_locale_and_parents(std::vector<std::string> &chain,
                               std::string_view normalized) {
  auto parts = split(normalized);
  append_unique(chain, std::string(normalized));
  if (parts.front() == "x")
    return;

  // Extensions and private-use sections are atomic for fallback. For example,
  // de-DE-u-co-phonebk falls directly back to de-DE, never de-DE-u-co.
  auto structural_count = parts.size();
  for (std::size_t i = 1; i < parts.size(); ++i) {
    if (parts[i].size() == 1) {
      structural_count = i;
      break;
    }
  }
  if (structural_count < parts.size())
    append_unique(chain, join(parts, structural_count));

  while (structural_count > 1) {
    --structural_count;
    append_unique(chain, join(parts, structural_count));
  }
}

} // namespace

LocalizationResult<std::string> normalize_locale_tag(std::string_view tag) {
  if (tag.empty()) {
    return LocalizationResult<std::string>::failure(
        locale_error(tag, "locale tag is empty"));
  }
  if (tag.size() > 255) {
    return LocalizationResult<std::string>::failure(
        locale_error(tag, "locale tag exceeds 255 bytes"));
  }

  std::vector<std::string> parts;
  std::size_t start = 0;
  for (std::size_t index = 0; index <= tag.size(); ++index) {
    if (index != tag.size() && tag[index] != '-' && tag[index] != '_') {
      if (!ascii_alnum(tag[index])) {
        return LocalizationResult<std::string>::failure(locale_error(
            tag,
            "locale tags may contain only ASCII letters, digits and separators",
            index));
      }
      continue;
    }
    if (index == start) {
      return LocalizationResult<std::string>::failure(
          locale_error(tag, "locale tag contains an empty subtag", index));
    }
    if (index - start > 8) {
      return LocalizationResult<std::string>::failure(
          locale_error(tag, "locale subtag exceeds eight characters", start));
    }
    parts.emplace_back(tag.substr(start, index - start));
    start = index + 1;
  }

  if (lowercase(parts.front()) == "x") {
    if (parts.size() == 1) {
      return LocalizationResult<std::string>::failure(
          locale_error(tag, "private-use locale requires at least one value"));
    }
    for (auto &part : parts)
      part = lowercase(std::move(part));
    return LocalizationResult<std::string>::success(join(parts, parts.size()));
  }

  if (parts.front().size() < 2 || parts.front().size() > 8 ||
      !all_alpha(parts.front())) {
    return LocalizationResult<std::string>::failure(locale_error(
        tag, "primary language must contain two to eight ASCII letters"));
  }
  parts.front() = lowercase(std::move(parts.front()));

  std::set<std::string, std::less<>> variants;
  std::set<std::string, std::less<>> singletons;
  std::size_t extlang_count = 0;
  bool script_seen = false;
  bool region_seen = false;
  bool variant_seen = false;
  bool in_extension = false;
  bool in_private_use = false;
  bool extension_needs_value = false;

  for (std::size_t index = 1; index < parts.size(); ++index) {
    auto &part = parts[index];
    if (part.size() == 1) {
      if (in_private_use) {
        part = lowercase(std::move(part));
        extension_needs_value = false;
        continue;
      }
      if (extension_needs_value) {
        return LocalizationResult<std::string>::failure(
            locale_error(tag, "extension singleton is missing its value"));
      }
      const auto singleton = lowercase(part);
      if (!singletons.insert(singleton).second) {
        return LocalizationResult<std::string>::failure(
            locale_error(tag, "locale tag repeats an extension singleton"));
      }
      part = singleton;
      in_private_use = singleton == "x";
      in_extension = !in_private_use;
      extension_needs_value = true;
      continue;
    }

    if (in_extension || in_private_use) {
      if (in_extension && part.size() < 2) {
        return LocalizationResult<std::string>::failure(locale_error(
            tag, "extension values must contain two to eight characters"));
      }
      part = lowercase(std::move(part));
      extension_needs_value = false;
      continue;
    }

    if (part.size() == 3 && all_alpha(part) && !script_seen && !region_seen &&
        !variant_seen && extlang_count < 3) {
      part = lowercase(std::move(part));
      ++extlang_count;
    } else if (part.size() == 4 && all_alpha(part) && !script_seen &&
               !region_seen && !variant_seen) {
      part = lowercase(std::move(part));
      part.front() = uppercase_ascii(part.front());
      script_seen = true;
    } else if (part.size() == 2 && all_alpha(part) && !region_seen &&
               !variant_seen) {
      std::ranges::transform(part, part.begin(), [](char character) {
        return uppercase_ascii(character);
      });
      region_seen = true;
    } else if (part.size() == 3 && all_digits(part) && !region_seen &&
               !variant_seen) {
      // Numeric regions retain their spelling.
      region_seen = true;
    } else {
      const bool valid_variant =
          (part.size() >= 5 && part.size() <= 8) ||
          (part.size() == 4 && ascii_digit(part.front()));
      if (!valid_variant) {
        return LocalizationResult<std::string>::failure(locale_error(
            tag, "locale subtags are out of order or have an invalid shape"));
      }
      part = lowercase(std::move(part));
      if (!variants.insert(part).second) {
        return LocalizationResult<std::string>::failure(
            locale_error(tag, "locale tag repeats a variant"));
      }
      variant_seen = true;
    }
  }

  if (extension_needs_value) {
    return LocalizationResult<std::string>::failure(
        locale_error(tag, "extension singleton is missing its value"));
  }

  return LocalizationResult<std::string>::success(join(parts, parts.size()));
}

LocalizationResult<std::vector<std::string>>
locale_fallback_chain(std::string_view requested_locale,
                      std::string_view default_locale) {
  auto requested = normalize_locale_tag(requested_locale);
  if (!requested) {
    return LocalizationResult<std::vector<std::string>>::failure(
        std::move(requested).error());
  }
  auto fallback = normalize_locale_tag(default_locale);
  if (!fallback) {
    auto error = std::move(fallback).error();
    error.operation = "locale_fallback_chain";
    return LocalizationResult<std::vector<std::string>>::failure(
        std::move(error));
  }

  std::vector<std::string> chain;
  append_locale_and_parents(chain, requested.value());
  append_locale_and_parents(chain, fallback.value());
  return LocalizationResult<std::vector<std::string>>::success(
      std::move(chain));
}

bool is_pseudo_locale(std::string_view normalized_locale) noexcept {
  return normalized_locale == expansion_pseudo_locale ||
         normalized_locale == rtl_pseudo_locale;
}

} // namespace openxmb::localization
