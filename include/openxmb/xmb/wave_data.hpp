#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace openxmb::xmb {

inline constexpr std::uint32_t kWaveGridSize = 128;
inline constexpr std::size_t kWaveVertexCount =
    static_cast<std::size_t>(kWaveGridSize) * kWaveGridSize;
inline constexpr double kReferenceIdleKeyframesPerSecond = 0.8;
inline constexpr std::uint32_t kIdleSeamCrossfadeKeyframes = 5;
inline constexpr float kReferenceIdleVerticalFlatten = 0.72F;

using OwnedBytes = std::vector<std::byte>;

enum class WaveErrorCode {
    io_open_failed,
    io_read_failed,
    lfs_pointer,
    truncated,
    unexpected_size,
    wrong_endian,
    invalid_magic,
    unsupported_format,
    invalid_header,
    invalid_metadata,
    inconsistent_metadata,
    non_finite,
    out_of_range,
    invalid_grid,
    invalid_argument,
};

struct WaveError {
    WaveErrorCode code{WaveErrorCode::invalid_argument};
    std::string source;
    std::string detail;
    std::size_t byte_offset{0};
};

template <typename T>
class [[nodiscard]] WaveResult {
public:
    static WaveResult success(T value) {
        return WaveResult(std::in_place_index<0>, std::move(value));
    }

    static WaveResult failure(WaveError error) {
        return WaveResult(std::in_place_index<1>, std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }
    [[nodiscard]] WaveError& error() & { return std::get<1>(storage_); }
    [[nodiscard]] const WaveError& error() const& { return std::get<1>(storage_); }

private:
    template <std::size_t I, typename U>
    explicit WaveResult(std::in_place_index_t<I> tag, U&& value)
        : storage_(tag, std::forward<U>(value)) {}

    std::variant<T, WaveError> storage_;
};

template <>
class [[nodiscard]] WaveResult<void> {
public:
    static WaveResult success() { return WaveResult(true, {}); }
    static WaveResult failure(WaveError error) {
        return WaveResult(false, std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] WaveError& error() & { return error_; }
    [[nodiscard]] const WaveError& error() const& { return error_; }

private:
    WaveResult(bool has_value, WaveError error)
        : has_value_(has_value), error_(std::move(error)) {}

    bool has_value_{false};
    WaveError error_{};
};

struct WavePosition {
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
    float w{1.0F};
};

struct WaveNormal {
    float x{0.0F};
    float y{0.0F};
    float z{1.0F};
};

struct WaveUv {
    float u{0.0F};
    float v{0.0F};
};

struct WaveVertex {
    WavePosition clip;
    WaveNormal normal;
    WaveUv uv;
};

struct WaveFrame {
    std::vector<WavePosition> positions;
};

struct WaveGeometry {
    std::uint32_t grid_size{kWaveGridSize};
    std::vector<WaveVertex> vertices;
};

struct IdleWaveSequence {
    std::uint32_t grid_size{kWaveGridSize};
    float z_w_ratio{1.0F};
    std::uint32_t capture_source_fps{0};
    std::uint32_t capture_keyframe_stride{0};
    std::uint32_t capture_source_frame_count{0};
    double reference_keyframes_per_second{kReferenceIdleKeyframesPerSecond};
    std::uint32_t seam_crossfade_keyframes{kIdleSeamCrossfadeKeyframes};
    std::vector<WaveFrame> frames;
    // Cached from the raw sequence. The reference sampler uses it for the active
    // xmb-web 0.72 vertical-amplitude fit without mutating authoritative frames.
    std::vector<float> temporal_mean_y;
};

struct BootWaveFrameInfo {
    std::uint32_t source_frame{0};
    double boot_seconds{0.0};
    float brightness{0.0F};
};

struct BootWaveSequence {
    std::uint32_t grid_size{kWaveGridSize};
    std::uint32_t source_fps{0};
    std::vector<WaveFrame> frames;
    std::vector<BootWaveFrameInfo> frame_info;
};

struct IdleSamplingPolicy {
    double keyframes_per_second{kReferenceIdleKeyframesPerSecond};
    std::uint32_t seam_crossfade_keyframes{kIdleSeamCrossfadeKeyframes};
    float vertical_flatten{kReferenceIdleVerticalFlatten};
};

enum class WaveIndexTopology {
    triangles,
    triangle_strip,
    lines,
};

[[nodiscard]] WaveResult<OwnedBytes> load_wave_bytes(
    const std::filesystem::path& path);

[[nodiscard]] WaveResult<WaveGeometry> parse_wave_geometry(
    const OwnedBytes& bytes, std::string_view source = "wave_geo.bin");
[[nodiscard]] WaveResult<WaveGeometry> load_wave_geometry(
    const std::filesystem::path& path);

[[nodiscard]] WaveResult<IdleWaveSequence> parse_idle_wave_sequence(
    const OwnedBytes& binary, std::string_view metadata_json,
    std::string_view binary_source = "wave_seq2.bin",
    std::string_view metadata_source = "wave_seq2.json");
[[nodiscard]] WaveResult<IdleWaveSequence> load_idle_wave_sequence(
    const std::filesystem::path& binary_path,
    const std::filesystem::path& metadata_path);

[[nodiscard]] WaveResult<BootWaveSequence> parse_boot_wave_sequence(
    const OwnedBytes& binary, std::string_view metadata_json,
    std::string_view binary_source = "wave_boot.bin",
    std::string_view metadata_source = "wave_boot.json");
[[nodiscard]] WaveResult<BootWaveSequence> load_boot_wave_sequence(
    const std::filesystem::path& binary_path,
    const std::filesystem::path& metadata_path);

// Matches the active xmb-web path: forward phase, Catmull-Rom interpolation,
// then a linear five-keyframe blend to frame zero at the wrap seam.
[[nodiscard]] WaveResult<void> sample_idle_wave_into(
    const IdleWaveSequence& sequence, double elapsed_seconds,
    std::span<WavePosition> output,
    const IdleSamplingPolicy& policy = {});
[[nodiscard]] WaveResult<WaveFrame> sample_idle_wave(
    const IdleWaveSequence& sequence, double elapsed_seconds,
    const IdleSamplingPolicy& policy = {});

// normalized_progress follows the active web boot path and clamps to [0, 1].
[[nodiscard]] WaveResult<void> sample_boot_wave_into(
    const BootWaveSequence& sequence, double normalized_progress,
    std::span<WavePosition> output);
[[nodiscard]] WaveResult<WaveFrame> sample_boot_wave(
    const BootWaveSequence& sequence, double normalized_progress);

// Uses the non-uniform source capture timestamps from wave_boot.json.
[[nodiscard]] WaveResult<WaveFrame> sample_boot_wave_at_time(
    const BootWaveSequence& sequence, double boot_seconds);

[[nodiscard]] WaveResult<std::vector<std::uint32_t>> generate_wave_grid_indices(
    std::uint32_t grid_size = kWaveGridSize,
    WaveIndexTopology topology = WaveIndexTopology::triangles);
[[nodiscard]] WaveResult<void> validate_wave_grid_indices(
    std::span<const std::uint32_t> indices, std::uint32_t grid_size,
    WaveIndexTopology topology);

}  // namespace openxmb::xmb
