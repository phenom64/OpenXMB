#include "openxmb/dialogs/recovery.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace openxmb::dialogs {
namespace {

constexpr std::string_view magic = "OXMBDLG1";
constexpr std::size_t maximum_encoded_size = 16U * 1024U * 1024U;
constexpr std::size_t maximum_string_size = 1024U * 1024U;
constexpr std::uint32_t maximum_frames = 64;
constexpr std::uint32_t maximum_entries = 4096;

template <class Integer> void append_integer(std::string &out, Integer value) {
  using Unsigned = std::make_unsigned_t<Integer>;
  auto bits = static_cast<Unsigned>(value);
  for (std::size_t i = 0; i < sizeof(Integer); ++i) {
    out.push_back(static_cast<char>((bits >> (i * 8U)) & 0xffU));
  }
}

void append_string(std::string &out, const std::string_view value) {
  if (value.size() > maximum_string_size ||
      value.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("recovery string is too large");
  append_integer(out, static_cast<std::uint32_t>(value.size()));
  out.append(value);
}

class Reader {
public:
  explicit Reader(const std::string_view bytes) : bytes_(bytes) {}

  template <class Integer> std::optional<Integer> integer() noexcept {
    if (remaining() < sizeof(Integer))
      return std::nullopt;
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned value{};
    for (std::size_t i = 0; i < sizeof(Integer); ++i) {
      value |=
          static_cast<Unsigned>(static_cast<unsigned char>(bytes_[offset_ + i]))
          << (i * 8U);
    }
    offset_ += sizeof(Integer);
    return static_cast<Integer>(value);
  }

  std::optional<std::string> string() {
    const auto size = integer<std::uint32_t>();
    if (!size || *size > maximum_string_size || remaining() < *size)
      return std::nullopt;
    std::string value(bytes_.substr(offset_, *size));
    offset_ += *size;
    return value;
  }

  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes_.size() - offset_;
  }

private:
  std::string_view bytes_;
  std::size_t offset_{};
};

RecoveryResult failure(const RecoveryErrorCode code, const std::size_t offset,
                       const std::string_view message) noexcept {
  return {std::nullopt, RecoveryError{code, offset, message}};
}

std::vector<FocusTarget> focus_targets(const ScreenDefinition &definition) {
  std::vector<FocusTarget> targets;
  if (definition.kind == ScreenKind::text_entry) {
    for (const auto &field : definition.fields)
      targets.push_back({FocusKind::field, field.id});
  }
  for (const auto &choice : definition.choices)
    targets.push_back({FocusKind::choice, choice.id});
  for (const auto &button : definition.buttons) {
    if (button.enabled)
      targets.push_back({FocusKind::button, button.id});
  }
  return targets;
}

bool recovered_state_valid(const DefinitionCatalog &catalog,
                           const SessionState &state) {
  if (state.outcome.kind == OutcomeKind::none) {
    if (state.stack.empty())
      return false;
  } else if (!state.stack.empty()) {
    return false;
  }
  for (const auto &frame : state.stack) {
    const auto *definition = catalog.find(frame.definition_id);
    if (!definition || !std::isfinite(frame.progress) || frame.progress < 0.0 ||
        frame.progress > 1.0)
      return false;
    if (definition->choices.empty()) {
      if (frame.selected_choice != 0)
        return false;
    } else if (frame.selected_choice >= definition->choices.size()) {
      return false;
    }
    if (frame.fields.size() != definition->fields.size())
      return false;
    for (const auto &field : definition->fields) {
      const auto value = frame.fields.find(field.id);
      if (value == frame.fields.end() ||
          value->second.size() > maximum_string_size)
        return false;
    }
    const auto targets = focus_targets(*definition);
    if (targets.empty()) {
      if (frame.focus.kind != FocusKind::none || !frame.focus.id.empty() ||
          frame.focus_position != 0)
        return false;
    } else if (frame.focus_position >= targets.size() ||
               frame.focus != targets[frame.focus_position]) {
      return false;
    }
    if (frame.focus.kind == FocusKind::choice &&
        definition->choices[frame.selected_choice].id != frame.focus.id) {
      return false;
    }
  }
  return true;
}

} // namespace

std::string serialize_recovery(const SessionState &state) {
  std::string out;
  out.reserve(256);
  out.append(magic);
  append_integer(out, static_cast<std::uint8_t>(state.outcome.kind));
  append_integer(out, state.revision);
  append_string(out, state.outcome.definition_id);
  append_string(out, state.outcome.action_id);
  append_string(out, state.outcome.result_code);
  append_string(out, state.outcome.message_key);

  if (state.variables.size() > maximum_entries ||
      state.stack.size() > maximum_frames) {
    throw std::length_error("recovery state exceeds structural limits");
  }
  append_integer(out, static_cast<std::uint32_t>(state.variables.size()));
  for (const auto &[key, value] : state.variables) {
    append_string(out, key);
    append_string(out, value);
  }

  append_integer(out, static_cast<std::uint32_t>(state.stack.size()));
  for (const auto &frame : state.stack) {
    append_string(out, frame.definition_id);
    append_integer(out, static_cast<std::uint8_t>(frame.focus.kind));
    append_string(out, frame.focus.id);
    if (frame.focus_position > std::numeric_limits<std::uint32_t>::max() ||
        frame.selected_choice > std::numeric_limits<std::uint32_t>::max() ||
        frame.fields.size() > maximum_entries) {
      throw std::length_error("recovery frame exceeds structural limits");
    }
    append_integer(out, static_cast<std::uint32_t>(frame.focus_position));
    append_integer(out, static_cast<std::uint32_t>(frame.selected_choice));
    append_integer(out, std::bit_cast<std::uint64_t>(frame.progress));
    append_integer(out, frame.entered_at_ms);
    append_integer(out, static_cast<std::uint32_t>(frame.fields.size()));
    for (const auto &[key, value] : frame.fields) {
      append_string(out, key);
      append_string(out, value);
    }
  }
  if (out.size() > maximum_encoded_size)
    throw std::length_error("recovery encoding exceeds size limit");
  return out;
}

RecoveryResult restore_recovery(const DefinitionCatalog &catalog,
                                const std::string_view encoded) noexcept {
  if (encoded.size() > maximum_encoded_size)
    return failure(RecoveryErrorCode::limit_exceeded, 0,
                   "recovery encoding exceeds size limit");
  if (!encoded.starts_with(magic))
    return failure(RecoveryErrorCode::unsupported_version, 0,
                   "unsupported recovery format");
  try {
    Reader reader(encoded.substr(magic.size()));
    const auto outcome = reader.integer<std::uint8_t>();
    const auto revision = reader.integer<std::uint64_t>();
    if (!outcome || !revision ||
        *outcome > static_cast<std::uint8_t>(OutcomeKind::failed)) {
      return failure(RecoveryErrorCode::malformed,
                     magic.size() + reader.offset(), "invalid recovery header");
    }

    SessionState state;
    state.outcome.kind = static_cast<OutcomeKind>(*outcome);
    state.revision = *revision;
    auto read_outcome_string = [&](std::string &target) {
      auto value = reader.string();
      if (!value)
        return false;
      target = std::move(*value);
      return true;
    };
    if (!read_outcome_string(state.outcome.definition_id) ||
        !read_outcome_string(state.outcome.action_id) ||
        !read_outcome_string(state.outcome.result_code) ||
        !read_outcome_string(state.outcome.message_key)) {
      return failure(RecoveryErrorCode::malformed,
                     magic.size() + reader.offset(),
                     "truncated outcome record");
    }

    const auto variable_count = reader.integer<std::uint32_t>();
    if (!variable_count || *variable_count > maximum_entries)
      return failure(RecoveryErrorCode::limit_exceeded,
                     magic.size() + reader.offset(), "invalid variable count");
    for (std::uint32_t index = 0; index < *variable_count; ++index) {
      auto key = reader.string();
      auto value = reader.string();
      if (!key || !value || key->empty() ||
          !state.variables.emplace(std::move(*key), std::move(*value)).second) {
        return failure(RecoveryErrorCode::malformed,
                       magic.size() + reader.offset(),
                       "invalid or duplicate recovery variable");
      }
    }

    const auto frame_count = reader.integer<std::uint32_t>();
    if (!frame_count || *frame_count > maximum_frames)
      return failure(RecoveryErrorCode::limit_exceeded,
                     magic.size() + reader.offset(), "invalid frame count");
    state.stack.reserve(*frame_count);
    for (std::uint32_t index = 0; index < *frame_count; ++index) {
      FrameState frame;
      auto definition_id = reader.string();
      const auto focus_kind = reader.integer<std::uint8_t>();
      auto focus_id = reader.string();
      const auto focus_position = reader.integer<std::uint32_t>();
      const auto selected_choice = reader.integer<std::uint32_t>();
      const auto progress_bits = reader.integer<std::uint64_t>();
      const auto entered_at = reader.integer<std::uint64_t>();
      const auto field_count = reader.integer<std::uint32_t>();
      if (!definition_id || definition_id->empty() || !focus_kind ||
          *focus_kind > static_cast<std::uint8_t>(FocusKind::field) ||
          !focus_id || !focus_position || !selected_choice || !progress_bits ||
          !entered_at || !field_count || *field_count > maximum_entries) {
        return failure(RecoveryErrorCode::malformed,
                       magic.size() + reader.offset(),
                       "truncated or invalid frame record");
      }
      frame.definition_id = std::move(*definition_id);
      frame.focus = {static_cast<FocusKind>(*focus_kind), std::move(*focus_id)};
      frame.focus_position = *focus_position;
      frame.selected_choice = *selected_choice;
      frame.progress = std::bit_cast<double>(*progress_bits);
      frame.entered_at_ms = *entered_at;
      for (std::uint32_t field_index = 0; field_index < *field_count;
           ++field_index) {
        auto key = reader.string();
        auto value = reader.string();
        if (!key || !value || key->empty() ||
            !frame.fields.emplace(std::move(*key), std::move(*value)).second) {
          return failure(RecoveryErrorCode::malformed,
                         magic.size() + reader.offset(),
                         "invalid or duplicate recovery field");
        }
      }
      state.stack.push_back(std::move(frame));
    }
    if (reader.remaining() != 0)
      return failure(RecoveryErrorCode::malformed,
                     magic.size() + reader.offset(),
                     "trailing bytes after recovery record");
    if (!catalog.validate().empty())
      return failure(RecoveryErrorCode::invalid_state, 0,
                     "definition catalog is invalid");
    for (const auto &frame : state.stack) {
      if (!catalog.find(frame.definition_id))
        return failure(RecoveryErrorCode::missing_definition, 0,
                       "recovery frame references a missing definition");
    }
    if (!state.outcome.definition_id.empty() &&
        !catalog.find(state.outcome.definition_id)) {
      return failure(RecoveryErrorCode::missing_definition, 0,
                     "recovery outcome references a missing definition");
    }
    if (!recovered_state_valid(catalog, state))
      return failure(RecoveryErrorCode::invalid_state, 0,
                     "recovery state violates definition invariants");
    return {std::move(state), std::nullopt};
  } catch (const std::bad_alloc &) {
    return failure(RecoveryErrorCode::allocation_failure, 0,
                   "recovery allocation failed");
  } catch (...) {
    return failure(RecoveryErrorCode::malformed, 0, "recovery decoding failed");
  }
}

} // namespace openxmb::dialogs
