/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "openxmb/localization/error.hpp"
#include "openxmb/localization/message.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace openxmb::localization {

using MessageMap = std::map<std::string, std::string, std::less<>>;

struct CatalogLayer {
  std::string locale;
  MessageMap messages;
  std::string source_name;
};

enum class OverlayPolicy { replace_existing, reject_existing };

struct LookupTrace {
  std::string requested_locale;
  std::string resolved_locale;
  std::vector<std::string> attempted_locales;
  bool used_fallback{};
  bool pseudo_localized{};

  [[nodiscard]] bool operator==(const LookupTrace &) const = default;
};

struct LocalizedMessage {
  std::string key;
  std::string text;
  LookupTrace trace;

  [[nodiscard]] bool operator==(const LocalizedMessage &) const = default;
};

class LocalizationCatalog {
public:
  [[nodiscard]] static LocalizationResult<LocalizationCatalog>
  create(CatalogLayer base_layer);

  // Layers may be partial. Every translated key must exist in the base layer
  // and retain its placeholder signature.
  [[nodiscard]] LocalizationResult<void>
  overlay(CatalogLayer layer,
          OverlayPolicy policy = OverlayPolicy::replace_existing);

  [[nodiscard]] LocalizationResult<LocalizedMessage>
  lookup(std::string_view key, std::string_view requested_locale) const;

  [[nodiscard]] const std::string &default_locale() const noexcept {
    return default_locale_;
  }
  [[nodiscard]] std::vector<std::string> available_locales() const;
  [[nodiscard]] std::size_t base_message_count() const noexcept {
    return base_signatures_.size();
  }

private:
  std::string default_locale_;
  std::map<std::string, MessageMap, std::less<>> layers_;
  std::map<std::string, PlaceholderSignature, std::less<>> base_signatures_;
};

} // namespace openxmb::localization
