#include "openxmb/xmb/wave_data.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <system_error>

namespace openxmb::xmb {
namespace {

constexpr std::size_t kGeometryStrideBytes = 9U * sizeof(float);
constexpr std::size_t kIdleHeaderBytes = 24;
constexpr std::size_t kBootHeaderBytes = 20;
constexpr std::uint32_t kFloat16Format = 1;
constexpr std::size_t kMaxWaveAssetBytes = 128U * 1024U * 1024U;
constexpr std::string_view kLfsPrefix =
    "version https://git-lfs.github.com/spec/v1";

WaveError make_error(WaveErrorCode code, std::string_view source,
                     std::string detail, std::size_t byte_offset = 0) {
    return WaveError{code, std::string(source), std::move(detail), byte_offset};
}

bool begins_with_lfs_pointer(std::span<const std::byte> bytes) {
    if (bytes.size() < kLfsPrefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kLfsPrefix.size(); ++i) {
        if (std::to_integer<unsigned char>(bytes[i]) !=
            static_cast<unsigned char>(kLfsPrefix[i])) {
            return false;
        }
    }
    return true;
}

std::uint16_t read_u16(std::span<const std::byte> bytes, std::size_t offset,
                       bool big_endian = false) {
    const auto a = std::to_integer<std::uint16_t>(bytes[offset]);
    const auto b = std::to_integer<std::uint16_t>(bytes[offset + 1]);
    return big_endian ? static_cast<std::uint16_t>((a << 8U) | b)
                      : static_cast<std::uint16_t>(a | (b << 8U));
}

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset,
                       bool big_endian = false) {
    std::uint32_t result = 0;
    if (big_endian) {
        for (std::size_t i = 0; i < 4; ++i) {
            result = (result << 8U) |
                     std::to_integer<std::uint32_t>(bytes[offset + i]);
        }
    } else {
        for (std::size_t i = 0; i < 4; ++i) {
            result |= std::to_integer<std::uint32_t>(bytes[offset + i])
                      << (8U * i);
        }
    }
    return result;
}

float read_f32(std::span<const std::byte> bytes, std::size_t offset,
               bool big_endian = false) {
    return std::bit_cast<float>(read_u32(bytes, offset, big_endian));
}

float half_to_float(std::uint16_t value) {
    const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000U) << 16U;
    std::uint32_t exponent = (value >> 10U) & 0x1fU;
    std::uint32_t mantissa = value & 0x03ffU;
    std::uint32_t bits = 0;

    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 127U - 15U + 1U;
            while ((mantissa & 0x0400U) == 0) {
                mantissa <<= 1U;
                --exponent;
            }
            mantissa &= 0x03ffU;
            bits = sign | (exponent << 23U) | (mantissa << 13U);
        }
    } else if (exponent == 0x1fU) {
        bits = sign | 0x7f800000U | (mantissa << 13U);
    } else {
        exponent += 127U - 15U;
        bits = sign | (exponent << 23U) | (mantissa << 13U);
    }
    return std::bit_cast<float>(bits);
}

bool matches_magic(std::span<const std::byte> bytes, std::string_view magic) {
    if (bytes.size() < magic.size()) {
        return false;
    }
    for (std::size_t i = 0; i < magic.size(); ++i) {
        if (std::to_integer<unsigned char>(bytes[i]) !=
            static_cast<unsigned char>(magic[i])) {
            return false;
        }
    }
    return true;
}

bool multiply_would_overflow(std::size_t a, std::size_t b) {
    return a != 0 && b > std::numeric_limits<std::size_t>::max() / a;
}

// A deliberately small, dependency-free JSON reader. The public wave model is
// a portable C++20 library; it must not pull renderer or application JSON code
// into tools/tests just to validate the two pinned metadata documents.
struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue, std::less<>>;

struct JsonValue {
    using Storage =
        std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject>;
    Storage storage{nullptr};
};

class JsonParser {
public:
    JsonParser(std::string_view input, std::string_view source)
        : input_(input), source_(source) {}

    WaveResult<JsonValue> parse() {
        skip_space();
        auto value = parse_value();
        if (!value) {
            return value;
        }
        skip_space();
        if (position_ != input_.size()) {
            return fail("trailing bytes after JSON value");
        }
        return value;
    }

private:
    WaveResult<JsonValue> fail(std::string detail) const {
        return WaveResult<JsonValue>::failure(make_error(
            WaveErrorCode::invalid_metadata, source_, std::move(detail), position_));
    }

    void skip_space() {
        while (position_ < input_.size()) {
            const char c = input_[position_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                break;
            }
            ++position_;
        }
    }

    WaveResult<JsonValue> parse_value() {
        skip_space();
        if (position_ >= input_.size()) {
            return fail("unexpected end of JSON");
        }
        switch (input_[position_]) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': {
                auto text = parse_string();
                if (!text) {
                    return WaveResult<JsonValue>::failure(text.error());
                }
                return WaveResult<JsonValue>::success(
                    JsonValue{std::move(text).value()});
            }
            case 't': return parse_literal("true", JsonValue{true});
            case 'f': return parse_literal("false", JsonValue{false});
            case 'n': return parse_literal("null", JsonValue{nullptr});
            default: return parse_number();
        }
    }

    WaveResult<JsonValue> parse_literal(std::string_view literal, JsonValue value) {
        if (input_.substr(position_, literal.size()) != literal) {
            return fail("invalid JSON literal");
        }
        position_ += literal.size();
        return WaveResult<JsonValue>::success(std::move(value));
    }

    static void append_utf8(std::string& output, std::uint32_t cp) {
        if (cp <= 0x7fU) {
            output.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (cp >> 6U)));
            output.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xe0U | (cp >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
        }
    }

    std::optional<std::uint32_t> parse_hex4() {
        if (position_ + 4 > input_.size()) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[position_++];
            value <<= 4U;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return std::nullopt;
        }
        return value;
    }

    WaveResult<std::string> parse_string() {
        if (input_[position_] != '"') {
            return WaveResult<std::string>::failure(make_error(
                WaveErrorCode::invalid_metadata, source_, "expected JSON string",
                position_));
        }
        ++position_;
        std::string output;
        while (position_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[position_++]);
            if (c == '"') {
                return WaveResult<std::string>::success(std::move(output));
            }
            if (c < 0x20U) {
                return WaveResult<std::string>::failure(make_error(
                    WaveErrorCode::invalid_metadata, source_,
                    "unescaped control byte in JSON string", position_ - 1));
            }
            if (c != '\\') {
                output.push_back(static_cast<char>(c));
                continue;
            }
            if (position_ >= input_.size()) {
                return WaveResult<std::string>::failure(make_error(
                    WaveErrorCode::invalid_metadata, source_,
                    "unterminated JSON escape", position_));
            }
            const char escaped = input_[position_++];
            switch (escaped) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    auto cp = parse_hex4();
                    if (!cp || (*cp >= 0xd800U && *cp <= 0xdfffU)) {
                        return WaveResult<std::string>::failure(make_error(
                            WaveErrorCode::invalid_metadata, source_,
                            "invalid or unsupported JSON Unicode escape", position_));
                    }
                    append_utf8(output, *cp);
                    break;
                }
                default:
                    return WaveResult<std::string>::failure(make_error(
                        WaveErrorCode::invalid_metadata, source_,
                        "invalid JSON escape", position_ - 1));
            }
        }
        return WaveResult<std::string>::failure(make_error(
            WaveErrorCode::invalid_metadata, source_, "unterminated JSON string",
            position_));
    }

    WaveResult<JsonValue> parse_number() {
        const std::size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) return fail("invalid JSON number");
        if (input_[position_] == '0') {
            ++position_;
        } else if (input_[position_] >= '1' && input_[position_] <= '9') {
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
        } else {
            return fail("invalid JSON number");
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t digits = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
            if (digits == position_) return fail("missing fractional digits");
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            const std::size_t digits = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
            if (digits == position_) return fail("missing exponent digits");
        }
        double value = 0.0;
        const char* first = input_.data() + start;
        const char* last = input_.data() + position_;
        const auto parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc{} || parsed.ptr != last || !std::isfinite(value)) {
            return fail("invalid or non-finite JSON number");
        }
        return WaveResult<JsonValue>::success(JsonValue{value});
    }

    WaveResult<JsonValue> parse_array() {
        ++position_;
        JsonArray values;
        skip_space();
        if (position_ < input_.size() && input_[position_] == ']') {
            ++position_;
            return WaveResult<JsonValue>::success(JsonValue{std::move(values)});
        }
        while (true) {
            auto value = parse_value();
            if (!value) return value;
            values.push_back(std::move(value).value());
            skip_space();
            if (position_ >= input_.size()) return fail("unterminated JSON array");
            const char delimiter = input_[position_++];
            if (delimiter == ']') break;
            if (delimiter != ',') return fail("expected ',' or ']' in JSON array");
        }
        return WaveResult<JsonValue>::success(JsonValue{std::move(values)});
    }

    WaveResult<JsonValue> parse_object() {
        ++position_;
        JsonObject values;
        skip_space();
        if (position_ < input_.size() && input_[position_] == '}') {
            ++position_;
            return WaveResult<JsonValue>::success(JsonValue{std::move(values)});
        }
        while (true) {
            skip_space();
            auto key = parse_string();
            if (!key) return WaveResult<JsonValue>::failure(key.error());
            skip_space();
            if (position_ >= input_.size() || input_[position_] != ':') {
                return fail("expected ':' after JSON object key");
            }
            ++position_;
            auto value = parse_value();
            if (!value) return value;
            if (!values.emplace(std::move(key).value(), std::move(value).value()).second) {
                return fail("duplicate JSON object key");
            }
            skip_space();
            if (position_ >= input_.size()) return fail("unterminated JSON object");
            const char delimiter = input_[position_++];
            if (delimiter == '}') break;
            if (delimiter != ',') return fail("expected ',' or '}' in JSON object");
        }
        return WaveResult<JsonValue>::success(JsonValue{std::move(values)});
    }

    std::string_view input_;
    std::string source_;
    std::size_t position_{0};
};

class MetadataReader {
public:
    MetadataReader(const JsonObject* object, std::string_view source,
                   std::string prefix = {})
        : object_(object), source_(source), prefix_(std::move(prefix)) {
        if (object_ == nullptr) set_error("expected JSON object");
    }

    std::string text(std::string_view key) {
        const JsonValue* value = find(key);
        if (value == nullptr) return {};
        const auto* result = std::get_if<std::string>(&value->storage);
        if (result == nullptr) {
            set_error("field '" + field_name(key) + "' must be a string");
            return {};
        }
        return *result;
    }

    double number(std::string_view key) {
        const JsonValue* value = find(key);
        if (value == nullptr) return 0.0;
        const auto* result = std::get_if<double>(&value->storage);
        if (result == nullptr || !std::isfinite(*result)) {
            set_error("field '" + field_name(key) + "' must be a finite number");
            return 0.0;
        }
        return *result;
    }

    std::uint32_t unsigned_integer(std::string_view key) {
        const double value = number(key);
        if (error_) return 0;
        if (value < 0.0 || value > std::numeric_limits<std::uint32_t>::max() ||
            std::floor(value) != value) {
            set_error("field '" + field_name(key) + "' must be a uint32");
            return 0;
        }
        return static_cast<std::uint32_t>(value);
    }

    const JsonArray* array(std::string_view key) {
        const JsonValue* value = find(key);
        if (value == nullptr) return nullptr;
        const auto* result = std::get_if<JsonArray>(&value->storage);
        if (result == nullptr) {
            set_error("field '" + field_name(key) + "' must be an array");
        }
        return result;
    }

    [[nodiscard]] const std::optional<WaveError>& error() const { return error_; }

private:
    std::string field_name(std::string_view key) const {
        return prefix_.empty() ? std::string(key) : prefix_ + std::string(key);
    }

    const JsonValue* find(std::string_view key) {
        if (error_ || object_ == nullptr) return nullptr;
        const auto found = object_->find(key);
        if (found == object_->end()) {
            set_error("missing required field '" + field_name(key) + "'");
            return nullptr;
        }
        return &found->second;
    }

    void set_error(std::string detail) {
        if (!error_) {
            error_ = make_error(WaveErrorCode::invalid_metadata, source_,
                                std::move(detail));
        }
    }

    const JsonObject* object_{nullptr};
    std::string source_;
    std::string prefix_;
    std::optional<WaveError> error_;
};

struct IdleMetadata {
    std::uint32_t frame_count{0};
    std::uint32_t grid_size{0};
    std::uint32_t vertex_stride{0};
    std::uint32_t header_bytes{0};
    std::uint32_t frame_bytes{0};
    float z_w_ratio{0.0F};
    std::uint32_t source_fps{0};
    std::uint32_t keyframe_stride{0};
    std::uint32_t source_frame_count{0};
    std::uint32_t seam_crossfade{0};
};

WaveResult<IdleMetadata> parse_idle_metadata(std::string_view json,
                                             std::string_view source) {
    auto parsed = JsonParser(json, source).parse();
    if (!parsed) return WaveResult<IdleMetadata>::failure(parsed.error());
    const auto* object = std::get_if<JsonObject>(&parsed.value().storage);
    MetadataReader reader(object, source);
    const std::string magic = reader.text("magic");
    IdleMetadata result;
    result.frame_count = reader.unsigned_integer("frameCount");
    const std::uint32_t keyframe_count = reader.unsigned_integer("keyframeCount");
    result.grid_size = reader.unsigned_integer("gridN");
    result.vertex_stride = reader.unsigned_integer("vertStride");
    const std::string format = reader.text("format");
    result.header_bytes = reader.unsigned_integer("headerBytes");
    result.frame_bytes = reader.unsigned_integer("frameBytes");
    result.z_w_ratio = static_cast<float>(reader.number("zwRatio"));
    result.source_fps = reader.unsigned_integer("sourceFps");
    result.keyframe_stride = reader.unsigned_integer("keyframeStride");
    result.source_frame_count = reader.unsigned_integer("sourceFrameCount");
    result.seam_crossfade =
        reader.unsigned_integer("recommendedCrossfadeKeyframes");
    if (reader.error()) return WaveResult<IdleMetadata>::failure(*reader.error());
    if (magic != "WSQ2" || format != "float16_LE") {
        return WaveResult<IdleMetadata>::failure(make_error(
            WaveErrorCode::invalid_metadata, source,
            "metadata must describe WSQ2 float16_LE"));
    }
    if (result.frame_count != keyframe_count) {
        return WaveResult<IdleMetadata>::failure(make_error(
            WaveErrorCode::inconsistent_metadata, source,
            "frameCount and keyframeCount disagree"));
    }
    return WaveResult<IdleMetadata>::success(result);
}

struct BootMetadata {
    std::uint32_t frame_count{0};
    std::uint32_t grid_size{0};
    std::uint32_t vertex_stride{0};
    std::uint32_t header_bytes{0};
    std::uint32_t frame_bytes{0};
    std::uint32_t source_fps{0};
    std::vector<BootWaveFrameInfo> frames;
};

WaveResult<BootMetadata> parse_boot_metadata(std::string_view json,
                                             std::string_view source) {
    auto parsed = JsonParser(json, source).parse();
    if (!parsed) return WaveResult<BootMetadata>::failure(parsed.error());
    const auto* object = std::get_if<JsonObject>(&parsed.value().storage);
    MetadataReader reader(object, source);
    const std::string magic = reader.text("magic");
    BootMetadata result;
    result.frame_count = reader.unsigned_integer("frameCount");
    result.grid_size = reader.unsigned_integer("gridN");
    result.vertex_stride = reader.unsigned_integer("vertStride");
    const std::string format = reader.text("format");
    result.header_bytes = reader.unsigned_integer("headerBytes");
    result.frame_bytes = reader.unsigned_integer("frameBytes");
    result.source_fps = reader.unsigned_integer("sourceFps");
    const JsonArray* keyframes = reader.array("keyframes");
    if (reader.error()) return WaveResult<BootMetadata>::failure(*reader.error());
    if (magic != "WBT1" || format != "float16_LE") {
        return WaveResult<BootMetadata>::failure(make_error(
            WaveErrorCode::invalid_metadata, source,
            "metadata must describe WBT1 float16_LE"));
    }
    if (keyframes == nullptr || keyframes->size() != result.frame_count) {
        return WaveResult<BootMetadata>::failure(make_error(
            WaveErrorCode::inconsistent_metadata, source,
            "keyframes length does not match frameCount"));
    }
    result.frames.reserve(keyframes->size());
    for (std::size_t i = 0; i < keyframes->size(); ++i) {
        const auto* frame_object = std::get_if<JsonObject>(&(*keyframes)[i].storage);
        MetadataReader frame_reader(frame_object, source,
                                    "keyframes[" + std::to_string(i) + "].");
        BootWaveFrameInfo info;
        info.source_frame = frame_reader.unsigned_integer("frame");
        info.boot_seconds = frame_reader.number("boot_t");
        info.brightness = static_cast<float>(frame_reader.number("brightness01"));
        if (frame_reader.error()) {
            return WaveResult<BootMetadata>::failure(*frame_reader.error());
        }
        if (!result.frames.empty() &&
            (info.source_frame <= result.frames.back().source_frame ||
             info.boot_seconds <= result.frames.back().boot_seconds)) {
            return WaveResult<BootMetadata>::failure(make_error(
                WaveErrorCode::inconsistent_metadata, source,
                "boot keyframe timestamps must be strictly increasing"));
        }
        if (info.brightness < 0.0F || info.brightness > 1.0F) {
            return WaveResult<BootMetadata>::failure(make_error(
                WaveErrorCode::out_of_range, source,
                "boot keyframe brightness must be in [0, 1]"));
        }
        const double expected_time =
            static_cast<double>(info.source_frame) / result.source_fps;
        if (std::abs(expected_time - info.boot_seconds) > 0.00011) {
            return WaveResult<BootMetadata>::failure(make_error(
                WaveErrorCode::inconsistent_metadata, source,
                "boot_t does not match source frame/sourceFps"));
        }
        result.frames.push_back(info);
    }
    return WaveResult<BootMetadata>::success(std::move(result));
}

bool geometry_plausible(std::span<const std::byte> bytes, bool big_endian) {
    if (bytes.size() != kWaveVertexCount * kGeometryStrideBytes) return false;
    for (std::size_t i = 0; i < kWaveVertexCount; ++i) {
        const std::size_t base = i * kGeometryStrideBytes;
        std::array<float, 9> values{};
        for (std::size_t c = 0; c < values.size(); ++c) {
            values[c] = read_f32(bytes, base + c * sizeof(float), big_endian);
            if (!std::isfinite(values[c])) return false;
        }
        if (values[3] < 0.001F || values[3] > 100.0F ||
            std::abs(values[0]) > 100.0F || std::abs(values[1]) > 100.0F ||
            std::abs(values[2]) > 100.0F) return false;
        const float normal_length_sq =
            values[4] * values[4] + values[5] * values[5] + values[6] * values[6];
        if (normal_length_sq < 0.000001F || normal_length_sq > 100.0F) return false;
        if (values[7] < -0.001F || values[7] > 1.001F ||
            values[8] < -0.001F || values[8] > 1.001F) return false;
    }
    return true;
}

WaveResult<void> validate_sequence_shape(std::uint32_t grid_size,
                                         const std::vector<WaveFrame>& frames,
                                         std::string_view source) {
    if (grid_size < 2 || frames.size() < 2) {
        return WaveResult<void>::failure(make_error(
            WaveErrorCode::invalid_grid, source,
            "sequence requires a grid of at least 2 and at least two frames"));
    }
    const std::size_t expected = static_cast<std::size_t>(grid_size) * grid_size;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].positions.size() != expected) {
            return WaveResult<void>::failure(make_error(
                WaveErrorCode::inconsistent_metadata, source,
                "frame " + std::to_string(i) + " has an inconsistent vertex count"));
        }
    }
    return WaveResult<void>::success();
}

WavePosition interpolate_linear(const WavePosition& a, const WavePosition& b,
                                float t) {
    const auto mix = [t](float x, float y) { return x + (y - x) * t; };
    return {mix(a.x, b.x), mix(a.y, b.y), mix(a.z, b.z), mix(a.w, b.w)};
}

float catmull(float p0, float a, float b, float p3, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5F * ((2.0F * a) + (-p0 + b) * t +
                   (2.0F * p0 - 5.0F * a + 4.0F * b - p3) * t2 +
                   (-p0 + 3.0F * a - 3.0F * b + p3) * t3);
}

WavePosition interpolate_catmull(const WavePosition& p0, const WavePosition& a,
                                 const WavePosition& b, const WavePosition& p3,
                                 float t) {
    return {catmull(p0.x, a.x, b.x, p3.x, t),
            catmull(p0.y, a.y, b.y, p3.y, t),
            catmull(p0.z, a.z, b.z, p3.z, t),
            catmull(p0.w, a.w, b.w, p3.w, t)};
}

}  // namespace

WaveResult<OwnedBytes> load_wave_bytes(const std::filesystem::path& path) {
    const std::string source = path.string();
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return WaveResult<OwnedBytes>::failure(make_error(
            WaveErrorCode::io_open_failed, source, "could not open asset"));
    }
    const std::streamoff end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > kMaxWaveAssetBytes) {
        return WaveResult<OwnedBytes>::failure(make_error(
            WaveErrorCode::unexpected_size, source,
            "asset size is invalid or exceeds the 128 MiB parser limit"));
    }
    OwnedBytes bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty() &&
        !input.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()))) {
        return WaveResult<OwnedBytes>::failure(make_error(
            WaveErrorCode::io_read_failed, source, "could not read the complete asset"));
    }
    if (begins_with_lfs_pointer(bytes)) {
        return WaveResult<OwnedBytes>::failure(make_error(
            WaveErrorCode::lfs_pointer, source,
            "asset is a Git LFS pointer rather than binary content"));
    }
    return WaveResult<OwnedBytes>::success(std::move(bytes));
}

WaveResult<WaveGeometry> parse_wave_geometry(const OwnedBytes& bytes,
                                             std::string_view source) {
    if (begins_with_lfs_pointer(bytes)) {
        return WaveResult<WaveGeometry>::failure(make_error(
            WaveErrorCode::lfs_pointer, source,
            "asset is a Git LFS pointer rather than wave geometry"));
    }
    const std::size_t expected = kWaveVertexCount * kGeometryStrideBytes;
    if (bytes.size() < expected) {
        return WaveResult<WaveGeometry>::failure(make_error(
            WaveErrorCode::truncated, source,
            "wave_geo.bin must contain exactly 16384 vertices at 36 bytes each",
            bytes.size()));
    }
    if (bytes.size() > expected) {
        return WaveResult<WaveGeometry>::failure(make_error(
            WaveErrorCode::unexpected_size, source,
            "wave_geo.bin contains trailing bytes", expected));
    }
    if (!geometry_plausible(bytes, false)) {
        const bool is_big_endian = geometry_plausible(bytes, true);
        return WaveResult<WaveGeometry>::failure(make_error(
            is_big_endian ? WaveErrorCode::wrong_endian : WaveErrorCode::non_finite,
            source,
            is_big_endian
                ? "geometry is valid only as big-endian float32; little-endian is required"
                : "geometry contains non-finite or out-of-contract values"));
    }

    WaveGeometry result;
    result.vertices.resize(kWaveVertexCount);
    for (std::size_t i = 0; i < kWaveVertexCount; ++i) {
        const std::size_t base = i * kGeometryStrideBytes;
        auto value = [&](std::size_t column) {
            return read_f32(bytes, base + column * sizeof(float));
        };
        result.vertices[i] = {{value(0), value(1), value(2), value(3)},
                              {value(4), value(5), value(6)},
                              {value(7), value(8)}};
    }
    return WaveResult<WaveGeometry>::success(std::move(result));
}

WaveResult<WaveGeometry> load_wave_geometry(const std::filesystem::path& path) {
    auto bytes = load_wave_bytes(path);
    if (!bytes) return WaveResult<WaveGeometry>::failure(bytes.error());
    return parse_wave_geometry(bytes.value(), path.string());
}

WaveResult<IdleWaveSequence> parse_idle_wave_sequence(
    const OwnedBytes& binary, std::string_view metadata_json,
    std::string_view binary_source, std::string_view metadata_source) {
    if (begins_with_lfs_pointer(binary)) {
        return WaveResult<IdleWaveSequence>::failure(make_error(
            WaveErrorCode::lfs_pointer, binary_source,
            "asset is a Git LFS pointer rather than an idle sequence"));
    }
    auto metadata_result = parse_idle_metadata(metadata_json, metadata_source);
    if (!metadata_result) return WaveResult<IdleWaveSequence>::failure(metadata_result.error());
    const IdleMetadata& metadata = metadata_result.value();
    if (binary.size() < kIdleHeaderBytes) {
        return WaveResult<IdleWaveSequence>::failure(make_error(
            WaveErrorCode::truncated, binary_source, "WSQ2 header is truncated",
            binary.size()));
    }
    if (!matches_magic(binary, "WSQ2")) {
        const WaveErrorCode code = matches_magic(binary, "2QSW")
                                       ? WaveErrorCode::wrong_endian
                                       : WaveErrorCode::invalid_magic;
        return WaveResult<IdleWaveSequence>::failure(make_error(
            code, binary_source, "expected WSQ2 magic"));
    }
    const std::uint32_t frame_count = read_u32(binary, 4);
    const std::uint32_t grid_size = read_u32(binary, 8);
    const std::uint32_t vertex_stride = read_u32(binary, 12);
    const std::uint32_t format = read_u32(binary, 16);
    const float z_w_ratio = read_f32(binary, 20);
    const bool big_endian_matches =
        read_u32(binary, 4, true) == metadata.frame_count &&
        read_u32(binary, 8, true) == metadata.grid_size &&
        read_u32(binary, 12, true) == metadata.vertex_stride &&
        read_u32(binary, 16, true) == kFloat16Format;
    if (big_endian_matches) {
        return WaveResult<IdleWaveSequence>::failure(make_error(
            WaveErrorCode::wrong_endian, binary_source,
            "WSQ2 header is big-endian; little-endian is required"));
    }
    if (frame_count != metadata.frame_count || grid_size != metadata.grid_size ||
        vertex_stride != metadata.vertex_stride || format != kFloat16Format ||
        metadata.header_bytes != kIdleHeaderBytes ||
        std::abs(z_w_ratio - metadata.z_w_ratio) > 0.000001F) {
        return WaveResult<IdleWaveSequence>::failure(make_error(
            WaveErrorCode::inconsistent_metadata, binary_source,
            "WSQ2 header and metadata disagree"));
    }
    if (grid_size != kWaveGridSize || vertex_stride != 3 ||
        !std::isfinite(z_w_ratio) || z_w_ratio <= 0.0F) {
        return WaveResult<IdleWaveSequence>::failure(make_error(
            WaveErrorCode::invalid_header, binary_source,
            "unsupported WSQ2 grid, stride, or Z/W ratio"));
    }
    const std::size_t vertices = static_cast<std::size_t>(grid_size) * grid_size;
    const std::size_t frame_bytes = vertices * vertex_stride * sizeof(std::uint16_t);
    if (frame_bytes != metadata.frame_bytes ||
        multiply_would_overflow(frame_bytes, frame_count)) {
        return WaveResult<IdleWaveSequence>::failure(make_error(
            WaveErrorCode::inconsistent_metadata, metadata_source,
            "WSQ2 frame byte count is inconsistent"));
    }
    const std::size_t expected = kIdleHeaderBytes + frame_bytes * frame_count;
    if (binary.size() < expected) {
        return WaveResult<IdleWaveSequence>::failure(make_error(
            WaveErrorCode::truncated, binary_source, "WSQ2 frame data is truncated",
            binary.size()));
    }
    if (binary.size() > expected) {
        return WaveResult<IdleWaveSequence>::failure(make_error(
            WaveErrorCode::unexpected_size, binary_source,
            "WSQ2 contains trailing bytes", expected));
    }

    IdleWaveSequence result;
    result.grid_size = grid_size;
    result.z_w_ratio = z_w_ratio;
    result.capture_source_fps = metadata.source_fps;
    result.capture_keyframe_stride = metadata.keyframe_stride;
    result.capture_source_frame_count = metadata.source_frame_count;
    result.seam_crossfade_keyframes = metadata.seam_crossfade;
    result.frames.resize(frame_count);
    result.temporal_mean_y.assign(vertices, 0.0F);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        auto& positions = result.frames[frame].positions;
        positions.resize(vertices);
        const std::size_t frame_offset = kIdleHeaderBytes + frame * frame_bytes;
        for (std::size_t i = 0; i < vertices; ++i) {
            const std::size_t offset = frame_offset + i * 3U * sizeof(std::uint16_t);
            const float x = half_to_float(read_u16(binary, offset));
            const float y = half_to_float(read_u16(binary, offset + 2));
            const float w = half_to_float(read_u16(binary, offset + 4));
            const float z = w * z_w_ratio;
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) ||
                !std::isfinite(z)) {
                return WaveResult<IdleWaveSequence>::failure(make_error(
                    WaveErrorCode::non_finite, binary_source,
                    "WSQ2 contains a non-finite half-float", offset));
            }
            if (w <= 0.001F || w > 100.0F || std::abs(x) > 100.0F ||
                std::abs(y) > 100.0F) {
                return WaveResult<IdleWaveSequence>::failure(make_error(
                    WaveErrorCode::out_of_range, binary_source,
                    "WSQ2 clip position is outside the validated capture range", offset));
            }
            positions[i] = {x, y, z, w};
            result.temporal_mean_y[i] += y / static_cast<float>(frame_count);
        }
    }
    return WaveResult<IdleWaveSequence>::success(std::move(result));
}

WaveResult<IdleWaveSequence> load_idle_wave_sequence(
    const std::filesystem::path& binary_path,
    const std::filesystem::path& metadata_path) {
    auto binary = load_wave_bytes(binary_path);
    if (!binary) return WaveResult<IdleWaveSequence>::failure(binary.error());
    auto metadata = load_wave_bytes(metadata_path);
    if (!metadata) return WaveResult<IdleWaveSequence>::failure(metadata.error());
    const std::string json(reinterpret_cast<const char*>(metadata.value().data()),
                           metadata.value().size());
    return parse_idle_wave_sequence(binary.value(), json, binary_path.string(),
                                    metadata_path.string());
}

WaveResult<BootWaveSequence> parse_boot_wave_sequence(
    const OwnedBytes& binary, std::string_view metadata_json,
    std::string_view binary_source, std::string_view metadata_source) {
    if (begins_with_lfs_pointer(binary)) {
        return WaveResult<BootWaveSequence>::failure(make_error(
            WaveErrorCode::lfs_pointer, binary_source,
            "asset is a Git LFS pointer rather than a boot sequence"));
    }
    auto metadata_result = parse_boot_metadata(metadata_json, metadata_source);
    if (!metadata_result) return WaveResult<BootWaveSequence>::failure(metadata_result.error());
    BootMetadata metadata = std::move(metadata_result).value();
    if (binary.size() < kBootHeaderBytes) {
        return WaveResult<BootWaveSequence>::failure(make_error(
            WaveErrorCode::truncated, binary_source, "WBT1 header is truncated",
            binary.size()));
    }
    if (!matches_magic(binary, "WBT1")) {
        const WaveErrorCode code = matches_magic(binary, "1TBW")
                                       ? WaveErrorCode::wrong_endian
                                       : WaveErrorCode::invalid_magic;
        return WaveResult<BootWaveSequence>::failure(make_error(
            code, binary_source, "expected WBT1 magic"));
    }
    const std::uint32_t frame_count = read_u32(binary, 4);
    const std::uint32_t grid_size = read_u32(binary, 8);
    const std::uint32_t vertex_stride = read_u32(binary, 12);
    const std::uint32_t format = read_u32(binary, 16);
    const bool big_endian_matches =
        read_u32(binary, 4, true) == metadata.frame_count &&
        read_u32(binary, 8, true) == metadata.grid_size &&
        read_u32(binary, 12, true) == metadata.vertex_stride &&
        read_u32(binary, 16, true) == kFloat16Format;
    if (big_endian_matches) {
        return WaveResult<BootWaveSequence>::failure(make_error(
            WaveErrorCode::wrong_endian, binary_source,
            "WBT1 header is big-endian; little-endian is required"));
    }
    if (frame_count != metadata.frame_count || grid_size != metadata.grid_size ||
        vertex_stride != metadata.vertex_stride || format != kFloat16Format ||
        metadata.header_bytes != kBootHeaderBytes) {
        return WaveResult<BootWaveSequence>::failure(make_error(
            WaveErrorCode::inconsistent_metadata, binary_source,
            "WBT1 header and metadata disagree"));
    }
    if (grid_size != kWaveGridSize || vertex_stride != 4) {
        return WaveResult<BootWaveSequence>::failure(make_error(
            WaveErrorCode::invalid_header, binary_source,
            "unsupported WBT1 grid or stride"));
    }
    const std::size_t vertices = static_cast<std::size_t>(grid_size) * grid_size;
    const std::size_t frame_bytes = vertices * vertex_stride * sizeof(std::uint16_t);
    if (frame_bytes != metadata.frame_bytes ||
        multiply_would_overflow(frame_bytes, frame_count)) {
        return WaveResult<BootWaveSequence>::failure(make_error(
            WaveErrorCode::inconsistent_metadata, metadata_source,
            "WBT1 frame byte count is inconsistent"));
    }
    const std::size_t expected = kBootHeaderBytes + frame_bytes * frame_count;
    if (binary.size() < expected) {
        return WaveResult<BootWaveSequence>::failure(make_error(
            WaveErrorCode::truncated, binary_source, "WBT1 frame data is truncated",
            binary.size()));
    }
    if (binary.size() > expected) {
        return WaveResult<BootWaveSequence>::failure(make_error(
            WaveErrorCode::unexpected_size, binary_source,
            "WBT1 contains trailing bytes", expected));
    }

    BootWaveSequence result;
    result.grid_size = grid_size;
    result.source_fps = metadata.source_fps;
    result.frame_info = std::move(metadata.frames);
    result.frames.resize(frame_count);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        auto& positions = result.frames[frame].positions;
        positions.resize(vertices);
        const std::size_t frame_offset = kBootHeaderBytes + frame * frame_bytes;
        for (std::size_t i = 0; i < vertices; ++i) {
            const std::size_t offset = frame_offset + i * 4U * sizeof(std::uint16_t);
            WavePosition position{half_to_float(read_u16(binary, offset)),
                                  half_to_float(read_u16(binary, offset + 2)),
                                  half_to_float(read_u16(binary, offset + 4)),
                                  half_to_float(read_u16(binary, offset + 6))};
            if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
                !std::isfinite(position.z) || !std::isfinite(position.w)) {
                return WaveResult<BootWaveSequence>::failure(make_error(
                    WaveErrorCode::non_finite, binary_source,
                    "WBT1 contains a non-finite half-float", offset));
            }
            if (position.w <= 0.001F || position.w > 100.0F ||
                std::abs(position.x) > 100.0F || std::abs(position.y) > 100.0F ||
                std::abs(position.z) > 100.0F) {
                return WaveResult<BootWaveSequence>::failure(make_error(
                    WaveErrorCode::out_of_range, binary_source,
                    "WBT1 clip position is outside the validated capture range", offset));
            }
            positions[i] = position;
        }
    }
    return WaveResult<BootWaveSequence>::success(std::move(result));
}

WaveResult<BootWaveSequence> load_boot_wave_sequence(
    const std::filesystem::path& binary_path,
    const std::filesystem::path& metadata_path) {
    auto binary = load_wave_bytes(binary_path);
    if (!binary) return WaveResult<BootWaveSequence>::failure(binary.error());
    auto metadata = load_wave_bytes(metadata_path);
    if (!metadata) return WaveResult<BootWaveSequence>::failure(metadata.error());
    const std::string json(reinterpret_cast<const char*>(metadata.value().data()),
                           metadata.value().size());
    return parse_boot_wave_sequence(binary.value(), json, binary_path.string(),
                                    metadata_path.string());
}

WaveResult<void> sample_idle_wave_into(const IdleWaveSequence& sequence,
                                       double elapsed_seconds,
                                       std::span<WavePosition> output,
                                       const IdleSamplingPolicy& policy) {
    auto shape = validate_sequence_shape(sequence.grid_size, sequence.frames,
                                         "idle-wave-sequence");
    if (!shape) return shape;
    const std::size_t vertices =
        static_cast<std::size_t>(sequence.grid_size) * sequence.grid_size;
    if (output.size() != vertices || sequence.temporal_mean_y.size() != vertices) {
        return WaveResult<void>::failure(make_error(
            WaveErrorCode::invalid_argument, "idle-wave-sampler",
            "output or temporal mean size does not match the sequence grid"));
    }
    if (!std::isfinite(elapsed_seconds) || !std::isfinite(policy.keyframes_per_second) ||
        !std::isfinite(policy.vertical_flatten) || policy.keyframes_per_second <= 0.0 ||
        policy.vertical_flatten < 0.0F || policy.vertical_flatten > 1.0F ||
        policy.seam_crossfade_keyframes >= sequence.frames.size()) {
        return WaveResult<void>::failure(make_error(
            WaveErrorCode::invalid_argument, "idle-wave-sampler",
            "invalid time, playback rate, flatten factor, or seam length"));
    }
    const std::size_t count = sequence.frames.size();
    double phase = std::fmod(elapsed_seconds * policy.keyframes_per_second,
                             static_cast<double>(count));
    if (phase < 0.0) phase += static_cast<double>(count);
    const std::size_t i0 = static_cast<std::size_t>(std::floor(phase));
    const std::size_t i1 = (i0 + 1U) % count;
    const std::size_t im1 = (i0 + count - 1U) % count;
    const std::size_t i2 = (i1 + 1U) % count;
    const float fraction = static_cast<float>(phase - std::floor(phase));
    float seam_weight = 0.0F;
    if (policy.seam_crossfade_keyframes > 0) {
        const double seam_start =
            static_cast<double>(count - policy.seam_crossfade_keyframes);
        if (phase > seam_start) {
            seam_weight = static_cast<float>(
                (phase - seam_start) / policy.seam_crossfade_keyframes);
        }
    }
    for (std::size_t i = 0; i < vertices; ++i) {
        WavePosition value = interpolate_catmull(
            sequence.frames[im1].positions[i], sequence.frames[i0].positions[i],
            sequence.frames[i1].positions[i], sequence.frames[i2].positions[i],
            fraction);
        if (seam_weight > 0.0001F) {
            value = interpolate_linear(value, sequence.frames[0].positions[i],
                                       seam_weight);
        }
        const float mean_y = sequence.temporal_mean_y[i];
        value.y = mean_y + (value.y - mean_y) * policy.vertical_flatten;
        output[i] = value;
    }
    return WaveResult<void>::success();
}

WaveResult<WaveFrame> sample_idle_wave(const IdleWaveSequence& sequence,
                                       double elapsed_seconds,
                                       const IdleSamplingPolicy& policy) {
    WaveFrame result;
    result.positions.resize(static_cast<std::size_t>(sequence.grid_size) *
                            sequence.grid_size);
    auto sampled = sample_idle_wave_into(sequence, elapsed_seconds,
                                         result.positions, policy);
    if (!sampled) return WaveResult<WaveFrame>::failure(sampled.error());
    return WaveResult<WaveFrame>::success(std::move(result));
}

WaveResult<void> sample_boot_wave_into(const BootWaveSequence& sequence,
                                       double normalized_progress,
                                       std::span<WavePosition> output) {
    auto shape = validate_sequence_shape(sequence.grid_size, sequence.frames,
                                         "boot-wave-sequence");
    if (!shape) return shape;
    const std::size_t vertices =
        static_cast<std::size_t>(sequence.grid_size) * sequence.grid_size;
    if (output.size() != vertices || !std::isfinite(normalized_progress)) {
        return WaveResult<void>::failure(make_error(
            WaveErrorCode::invalid_argument, "boot-wave-sampler",
            "output size or normalized progress is invalid"));
    }
    const double progress = std::clamp(normalized_progress, 0.0, 1.0);
    const double phase = progress * static_cast<double>(sequence.frames.size() - 1U);
    const std::size_t i0 = static_cast<std::size_t>(std::floor(phase));
    const std::size_t i1 = std::min(i0 + 1U, sequence.frames.size() - 1U);
    const float fraction = static_cast<float>(phase - std::floor(phase));
    for (std::size_t i = 0; i < vertices; ++i) {
        output[i] = interpolate_linear(sequence.frames[i0].positions[i],
                                       sequence.frames[i1].positions[i], fraction);
    }
    return WaveResult<void>::success();
}

WaveResult<WaveFrame> sample_boot_wave(const BootWaveSequence& sequence,
                                       double normalized_progress) {
    WaveFrame result;
    result.positions.resize(static_cast<std::size_t>(sequence.grid_size) *
                            sequence.grid_size);
    auto sampled = sample_boot_wave_into(sequence, normalized_progress,
                                         result.positions);
    if (!sampled) return WaveResult<WaveFrame>::failure(sampled.error());
    return WaveResult<WaveFrame>::success(std::move(result));
}

WaveResult<WaveFrame> sample_boot_wave_at_time(const BootWaveSequence& sequence,
                                               double boot_seconds) {
    if (!std::isfinite(boot_seconds) ||
        sequence.frame_info.size() != sequence.frames.size() ||
        sequence.frame_info.empty()) {
        return WaveResult<WaveFrame>::failure(make_error(
            WaveErrorCode::invalid_argument, "boot-wave-time-sampler",
            "invalid time or missing boot frame metadata"));
    }
    if (boot_seconds <= sequence.frame_info.front().boot_seconds) {
        return WaveResult<WaveFrame>::success(sequence.frames.front());
    }
    if (boot_seconds >= sequence.frame_info.back().boot_seconds) {
        return WaveResult<WaveFrame>::success(sequence.frames.back());
    }
    const auto upper = std::upper_bound(
        sequence.frame_info.begin(), sequence.frame_info.end(), boot_seconds,
        [](double value, const BootWaveFrameInfo& info) {
            return value < info.boot_seconds;
        });
    const std::size_t i1 = static_cast<std::size_t>(
        std::distance(sequence.frame_info.begin(), upper));
    const std::size_t i0 = i1 - 1U;
    const double span = sequence.frame_info[i1].boot_seconds -
                        sequence.frame_info[i0].boot_seconds;
    const float fraction = static_cast<float>(
        (boot_seconds - sequence.frame_info[i0].boot_seconds) / span);
    WaveFrame result;
    result.positions.resize(sequence.frames[i0].positions.size());
    for (std::size_t i = 0; i < result.positions.size(); ++i) {
        result.positions[i] = interpolate_linear(sequence.frames[i0].positions[i],
                                                 sequence.frames[i1].positions[i],
                                                 fraction);
    }
    return WaveResult<WaveFrame>::success(std::move(result));
}

WaveResult<std::vector<std::uint32_t>> generate_wave_grid_indices(
    std::uint32_t grid_size, WaveIndexTopology topology) {
    if (grid_size < 2 ||
        static_cast<std::uint64_t>(grid_size) * grid_size >
            std::numeric_limits<std::uint32_t>::max()) {
        return WaveResult<std::vector<std::uint32_t>>::failure(make_error(
            WaveErrorCode::invalid_grid, "wave-grid",
            "grid size cannot be represented with uint32 indices"));
    }
    const std::size_t n = grid_size;
    std::vector<std::uint32_t> indices;
    if (topology == WaveIndexTopology::triangles) {
        indices.reserve((n - 1U) * (n - 1U) * 6U);
        for (std::uint32_t row = 0; row < grid_size - 1U; ++row) {
            for (std::uint32_t column = 0; column < grid_size - 1U; ++column) {
                const std::uint32_t a = row * grid_size + column;
                const std::uint32_t b = a + 1U;
                const std::uint32_t d = (row + 1U) * grid_size + column;
                const std::uint32_t e = d + 1U;
                indices.insert(indices.end(), {a, d, b, b, d, e});
            }
        }
    } else if (topology == WaveIndexTopology::triangle_strip) {
        indices.reserve((n - 1U) * n * 2U + (n - 2U) * 2U);
        for (std::uint32_t column = 0; column < grid_size - 1U; ++column) {
            if (column > 0) {
                indices.push_back(column + (grid_size - 1U) * grid_size);
                indices.push_back(column + 1U);
            }
            for (std::uint32_t row = 0; row < grid_size; ++row) {
                indices.push_back(row * grid_size + column + 1U);
                indices.push_back(row * grid_size + column);
            }
        }
    } else {
        indices.reserve(n * (n - 1U) * 2U);
        for (std::uint32_t row = 0; row < grid_size; ++row) {
            for (std::uint32_t column = 0; column < grid_size - 1U; ++column) {
                indices.push_back(row * grid_size + column);
                indices.push_back(row * grid_size + column + 1U);
            }
        }
    }
    return WaveResult<std::vector<std::uint32_t>>::success(std::move(indices));
}

WaveResult<void> validate_wave_grid_indices(
    std::span<const std::uint32_t> indices, std::uint32_t grid_size,
    WaveIndexTopology topology) {
    auto authoritative = generate_wave_grid_indices(grid_size, topology);
    if (!authoritative) return WaveResult<void>::failure(authoritative.error());
    if (indices.size() != authoritative.value().size()) {
        return WaveResult<void>::failure(make_error(
            WaveErrorCode::invalid_grid, "wave-grid",
            "index count does not match the authoritative topology rule"));
    }
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] != authoritative.value()[i]) {
            return WaveResult<void>::failure(make_error(
                WaveErrorCode::invalid_grid, "wave-grid",
                "index differs from the authoritative topology rule", i));
        }
    }
    return WaveResult<void>::success();
}

}  // namespace openxmb::xmb
