#include "openxmb/localization/message.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace openxmb::localization {
namespace {

struct MessageToken {
  std::string text;
  bool protected_syntax{};
};

struct ParsedMessage {
  std::vector<MessageToken> tokens;
  PlaceholderSignature signature;
};

[[nodiscard]] LocalizationError message_error(LocalizationErrorCode code,
                                              std::string operation,
                                              std::string detail,
                                              std::size_t offset = 0) {
  LocalizationError error;
  error.code = code;
  error.operation = std::move(operation);
  error.detail = std::move(detail);
  error.byte_offset = offset;
  return error;
}

[[nodiscard]] bool placeholder_start(char character) noexcept {
  return (character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z') || character == '_';
}

[[nodiscard]] bool placeholder_continue(char character) noexcept {
  return placeholder_start(character) ||
         (character >= '0' && character <= '9') || character == '.' ||
         character == '-';
}

[[nodiscard]] LocalizationResult<ParsedMessage>
parse_message(std::string_view message) {
  if (!is_valid_utf8(message)) {
    return LocalizationResult<ParsedMessage>::failure(message_error(
        LocalizationErrorCode::invalid_utf8, "analyze_placeholders",
        "message is not well-formed UTF-8"));
  }

  ParsedMessage parsed;
  std::map<std::string, std::size_t, std::less<>> counts;
  std::string literal;
  auto flush_literal = [&]() {
    if (!literal.empty()) {
      parsed.tokens.push_back({std::move(literal), false});
      literal.clear();
    }
  };

  std::size_t offset = 0;
  while (offset < message.size()) {
    if (message[offset] == '{') {
      if (offset + 1 < message.size() && message[offset + 1] == '{') {
        flush_literal();
        parsed.tokens.push_back({"{{", true});
        offset += 2;
        continue;
      }

      flush_literal();
      const auto close = message.find('}', offset + 1);
      if (close == std::string_view::npos) {
        return LocalizationResult<ParsedMessage>::failure(
            message_error(LocalizationErrorCode::malformed_placeholder,
                          "analyze_placeholders",
                          "placeholder is missing a closing brace", offset));
      }
      const auto nested = message.find('{', offset + 1);
      if (nested != std::string_view::npos && nested < close) {
        return LocalizationResult<ParsedMessage>::failure(message_error(
            LocalizationErrorCode::malformed_placeholder,
            "analyze_placeholders",
            "placeholder contains a nested opening brace", nested));
      }

      const auto body = message.substr(offset + 1, close - offset - 1);
      const auto colon = body.find(':');
      const auto name = body.substr(0, colon);
      if (name.empty() || !placeholder_start(name.front()) ||
          !std::ranges::all_of(name.substr(1), placeholder_continue)) {
        return LocalizationResult<ParsedMessage>::failure(message_error(
            LocalizationErrorCode::malformed_placeholder,
            "analyze_placeholders",
            "placeholder names must be portable ASCII identifiers", offset));
      }
      if (colon != std::string_view::npos && colon + 1 == body.size()) {
        return LocalizationResult<ParsedMessage>::failure(message_error(
            LocalizationErrorCode::malformed_placeholder,
            "analyze_placeholders", "placeholder format specifier is empty",
            offset + 1 + colon));
      }
      if (body.find('}', 0) != std::string_view::npos) {
        return LocalizationResult<ParsedMessage>::failure(
            message_error(LocalizationErrorCode::malformed_placeholder,
                          "analyze_placeholders",
                          "placeholder contains an unexpected brace", offset));
      }

      ++counts[std::string(name)];
      parsed.tokens.push_back(
          {std::string(message.substr(offset, close - offset + 1)), true});
      offset = close + 1;
      continue;
    }

    if (message[offset] == '}') {
      if (offset + 1 < message.size() && message[offset + 1] == '}') {
        flush_literal();
        parsed.tokens.push_back({"}}", true});
        offset += 2;
        continue;
      }
      return LocalizationResult<ParsedMessage>::failure(message_error(
          LocalizationErrorCode::malformed_placeholder, "analyze_placeholders",
          "message contains an unescaped closing brace", offset));
    }

    literal.push_back(message[offset]);
    ++offset;
  }
  flush_literal();

  for (auto &[name, count] : counts)
    parsed.signature.placeholders.push_back({std::move(name), count});
  return LocalizationResult<ParsedMessage>::success(std::move(parsed));
}

[[nodiscard]] bool decode_utf8(std::string_view text,
                               std::vector<std::uint32_t> *output) noexcept {
  if (output)
    output->clear();
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto lead = static_cast<unsigned char>(text[offset]);
    std::uint32_t codepoint{};
    std::size_t length{};
    std::uint32_t minimum{};
    if (lead <= 0x7f) {
      codepoint = lead;
      length = 1;
      minimum = 0;
    } else if ((lead & 0xe0) == 0xc0) {
      codepoint = lead & 0x1f;
      length = 2;
      minimum = 0x80;
    } else if ((lead & 0xf0) == 0xe0) {
      codepoint = lead & 0x0f;
      length = 3;
      minimum = 0x800;
    } else if ((lead & 0xf8) == 0xf0) {
      codepoint = lead & 0x07;
      length = 4;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (offset + length > text.size())
      return false;
    for (std::size_t index = 1; index < length; ++index) {
      const auto continuation =
          static_cast<unsigned char>(text[offset + index]);
      if ((continuation & 0xc0) != 0x80)
        return false;
      codepoint = (codepoint << 6) | (continuation & 0x3f);
    }
    if (codepoint < minimum || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff))
      return false;
    if (output)
      output->push_back(codepoint);
    offset += length;
  }
  return true;
}

void append_utf8(std::string &output, std::uint32_t codepoint) {
  if (codepoint <= 0x7f) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
}

[[nodiscard]] std::uint32_t accented(std::uint32_t codepoint) noexcept {
  constexpr std::uint32_t lower[]{
      0x00e0, 0x0180, 0x00e7, 0x010f, 0x00e9, 0x0192, 0x011d, 0x0125, 0x00ee,
      0x0135, 0x0137, 0x013c, 0x1e3f, 0x00f1, 0x00f6, 0x00fe, 0x01eb, 0x0155,
      0x0161, 0x0163, 0x00fc, 0x1e7d, 0x0175, 0x1e8b, 0x00fd, 0x017e};
  constexpr std::uint32_t upper[]{
      0x00c0, 0x0181, 0x00c7, 0x010e, 0x00c9, 0x0191, 0x011c, 0x0124, 0x00ce,
      0x0134, 0x0136, 0x013b, 0x1e3e, 0x00d1, 0x00d6, 0x00de, 0x01ea, 0x0154,
      0x0160, 0x0162, 0x00dc, 0x1e7c, 0x0174, 0x1e8a, 0x00dd, 0x017d};
  if (codepoint >= 'a' && codepoint <= 'z')
    return lower[codepoint - 'a'];
  if (codepoint >= 'A' && codepoint <= 'Z')
    return upper[codepoint - 'A'];
  return codepoint;
}

[[nodiscard]] std::vector<std::string>
expanded_placeholder_names(const PlaceholderSignature &signature) {
  std::vector<std::string> names;
  for (const auto &placeholder : signature.placeholders) {
    for (std::size_t count = 0; count < placeholder.count; ++count)
      names.push_back(placeholder.name);
  }
  return names;
}

} // namespace

bool is_valid_utf8(std::string_view text) noexcept {
  return decode_utf8(text, nullptr);
}

LocalizationResult<PlaceholderSignature>
analyze_placeholders(std::string_view message) {
  auto parsed = parse_message(message);
  if (!parsed)
    return LocalizationResult<PlaceholderSignature>::failure(parsed.error());
  return LocalizationResult<PlaceholderSignature>::success(
      std::move(parsed).value().signature);
}

LocalizationResult<void>
validate_placeholder_compatibility(std::string_view source,
                                   std::string_view translation) {
  auto expected = analyze_placeholders(source);
  if (!expected)
    return LocalizationResult<void>::failure(expected.error());
  auto actual = analyze_placeholders(translation);
  if (!actual)
    return LocalizationResult<void>::failure(actual.error());
  if (expected.value() == actual.value())
    return LocalizationResult<void>::success();

  auto error =
      message_error(LocalizationErrorCode::placeholder_mismatch,
                    "validate_placeholder_compatibility",
                    "translation placeholders do not match the source message");
  error.expected_placeholders = expanded_placeholder_names(expected.value());
  error.actual_placeholders = expanded_placeholder_names(actual.value());
  return LocalizationResult<void>::failure(std::move(error));
}

LocalizationResult<std::string> pseudo_localize(std::string_view message,
                                                PseudoLocaleKind kind) {
  auto parsed = parse_message(message);
  if (!parsed)
    return LocalizationResult<std::string>::failure(parsed.error());

  std::string output;
  std::size_t visible_scalars = 0;
  if (kind == PseudoLocaleKind::expansion)
    output += "\xe2\x9f\xa6"; // U+27E6 MATHEMATICAL LEFT WHITE SQUARE BRACKET
  else
    output += "\xe2\x81\xa7"; // U+2067 RIGHT-TO-LEFT ISOLATE

  for (const auto &token : parsed.value().tokens) {
    if (token.protected_syntax) {
      output += token.text;
      continue;
    }
    std::vector<std::uint32_t> codepoints;
    static_cast<void>(decode_utf8(token.text, &codepoints));
    visible_scalars += codepoints.size();
    if (kind == PseudoLocaleKind::right_to_left)
      std::ranges::reverse(codepoints);
    for (const auto codepoint : codepoints)
      append_utf8(output, accented(codepoint));
  }

  if (kind == PseudoLocaleKind::expansion) {
    const auto padding = static_cast<std::size_t>(
        std::ceil(static_cast<double>(visible_scalars) * 0.35));
    for (std::size_t index = 0; index < padding; ++index)
      output += "\xc2\xb7";   // U+00B7 MIDDLE DOT
    output += "\xe2\x9f\xa7"; // U+27E7
  } else {
    output += "\xe2\x81\xa9"; // U+2069 POP DIRECTIONAL ISOLATE
  }
  return LocalizationResult<std::string>::success(std::move(output));
}

} // namespace openxmb::localization
