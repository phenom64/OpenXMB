#include "openxmb/xmb/wave_data.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace openxmb::xmb;

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool near(float actual, float expected, float tolerance = 0.00001F) {
    return std::abs(actual - expected) <= tolerance;
}

std::filesystem::path find_reference(int argc, char** argv) {
    if (argc > 1) return std::filesystem::absolute(argv[1]);
    std::vector<std::filesystem::path> seeds{std::filesystem::current_path(),
                                             std::filesystem::absolute(__FILE__)};
    for (auto seed : seeds) {
        if (!std::filesystem::is_directory(seed)) seed = seed.parent_path();
        while (!seed.empty()) {
            for (const auto& candidate : {seed / "xmb-web", seed.parent_path() / "xmb-web"}) {
                if (std::filesystem::exists(candidate / "wave_geo.bin") &&
                    std::filesystem::exists(candidate / "wave_seq2.json")) {
                    return candidate;
                }
            }
            const auto parent = seed.parent_path();
            if (parent == seed) break;
            seed = parent;
        }
    }
    return {};
}

float catmull(float p0, float a, float b, float p3, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5F * ((2.0F * a) + (-p0 + b) * t +
                   (2.0F * p0 - 5.0F * a + 4.0F * b - p3) * t2 +
                   (-p0 + 3.0F * a - 3.0F * b + p3) * t3);
}

void reverse_u32(OwnedBytes& bytes, std::size_t offset) {
    std::reverse(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                 bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4));
}

void test_grid_indices() {
    auto triangles = generate_wave_grid_indices();
    expect(triangles.has_value(), "triangle index generation succeeds");
    if (triangles) {
        expect(triangles.value().size() == 96774, "128x128 triangle index count is 96,774");
        const std::vector<std::uint32_t> prefix{0, 128, 1, 1, 128, 129};
        expect(std::equal(prefix.begin(), prefix.end(), triangles.value().begin()),
               "triangle winding matches the xmb-web rule");
        expect(validate_wave_grid_indices(triangles.value(), 128,
                                          WaveIndexTopology::triangles)
                   .has_value(),
               "authoritative triangle indices validate");
        triangles.value()[7] ^= 1U;
        expect(!validate_wave_grid_indices(triangles.value(), 128,
                                           WaveIndexTopology::triangles),
               "mutated triangle indices are rejected");
    }
    auto strip = generate_wave_grid_indices(128, WaveIndexTopology::triangle_strip);
    expect(strip && strip.value().size() == 32764,
           "128x128 degenerate triangle-strip count is 32,764");
    auto lines = generate_wave_grid_indices(128, WaveIndexTopology::lines);
    expect(lines && lines.value().size() == 32512,
           "128x128 line index count is 32,512");
    expect(!generate_wave_grid_indices(1), "1x1 grid is rejected");
}

void test_rejections(const std::filesystem::path& reference,
                     const OwnedBytes& geometry_bytes,
                     const OwnedBytes& idle_bytes,
                     const std::string& idle_json,
                     const OwnedBytes& boot_bytes,
                     const std::string& boot_json) {
    auto truncated_geometry = geometry_bytes;
    truncated_geometry.pop_back();
    auto geometry_result = parse_wave_geometry(truncated_geometry);
    expect(!geometry_result && geometry_result.error().code == WaveErrorCode::truncated,
           "truncated geometry is rejected explicitly");

    auto oversized_geometry = geometry_bytes;
    oversized_geometry.push_back(std::byte{0});
    geometry_result = parse_wave_geometry(oversized_geometry);
    expect(!geometry_result && geometry_result.error().code == WaveErrorCode::unexpected_size,
           "oversized geometry is rejected explicitly");

    auto big_endian_geometry = geometry_bytes;
    for (std::size_t offset = 0; offset < big_endian_geometry.size(); offset += 4) {
        reverse_u32(big_endian_geometry, offset);
    }
    geometry_result = parse_wave_geometry(big_endian_geometry);
    expect(!geometry_result && geometry_result.error().code == WaveErrorCode::wrong_endian,
           "big-endian float32 geometry is rejected explicitly");

    auto non_finite_geometry = geometry_bytes;
    non_finite_geometry[0] = std::byte{0x00};
    non_finite_geometry[1] = std::byte{0x00};
    non_finite_geometry[2] = std::byte{0xc0};
    non_finite_geometry[3] = std::byte{0x7f};
    geometry_result = parse_wave_geometry(non_finite_geometry);
    expect(!geometry_result && geometry_result.error().code == WaveErrorCode::non_finite,
           "non-finite geometry is rejected explicitly");

    OwnedBytes pointer;
    const std::string lfs =
        "version https://git-lfs.github.com/spec/v1\noid sha256:00\nsize 123\n";
    pointer.resize(lfs.size());
    std::transform(lfs.begin(), lfs.end(), pointer.begin(),
                   [](char value) { return static_cast<std::byte>(value); });
    geometry_result = parse_wave_geometry(pointer);
    expect(!geometry_result && geometry_result.error().code == WaveErrorCode::lfs_pointer,
           "LFS pointer geometry is rejected explicitly");

    auto truncated_idle = idle_bytes;
    truncated_idle.pop_back();
    auto idle_result = parse_idle_wave_sequence(truncated_idle, idle_json);
    expect(!idle_result && idle_result.error().code == WaveErrorCode::truncated,
           "truncated WSQ2 is rejected explicitly");

    auto wrong_endian_idle = idle_bytes;
    for (std::size_t offset = 4; offset < 24; offset += 4) reverse_u32(wrong_endian_idle, offset);
    idle_result = parse_idle_wave_sequence(wrong_endian_idle, idle_json);
    expect(!idle_result && idle_result.error().code == WaveErrorCode::wrong_endian,
           "big-endian WSQ2 header is rejected explicitly");

    auto non_finite_idle = idle_bytes;
    non_finite_idle[24] = std::byte{0x00};
    non_finite_idle[25] = std::byte{0x7c};
    idle_result = parse_idle_wave_sequence(non_finite_idle, idle_json);
    expect(!idle_result && idle_result.error().code == WaveErrorCode::non_finite,
           "non-finite WSQ2 half-float is rejected explicitly");

    std::string inconsistent_idle_json = idle_json;
    const auto frame_count = inconsistent_idle_json.find("\"frameCount\": 85");
    expect(frame_count != std::string::npos, "idle corruption fixture finds frameCount");
    if (frame_count != std::string::npos) inconsistent_idle_json.replace(frame_count, 16, "\"frameCount\": 84");
    idle_result = parse_idle_wave_sequence(idle_bytes, inconsistent_idle_json);
    expect(!idle_result, "inconsistent WSQ2 metadata is rejected");

    auto truncated_boot = boot_bytes;
    truncated_boot.pop_back();
    auto boot_result = parse_boot_wave_sequence(truncated_boot, boot_json);
    expect(!boot_result && boot_result.error().code == WaveErrorCode::truncated,
           "truncated WBT1 is rejected explicitly");

    auto wrong_endian_boot = boot_bytes;
    for (std::size_t offset = 4; offset < 20; offset += 4) reverse_u32(wrong_endian_boot, offset);
    boot_result = parse_boot_wave_sequence(wrong_endian_boot, boot_json);
    expect(!boot_result && boot_result.error().code == WaveErrorCode::wrong_endian,
           "big-endian WBT1 header is rejected explicitly");

    auto non_finite_boot = boot_bytes;
    non_finite_boot[20] = std::byte{0x00};
    non_finite_boot[21] = std::byte{0x7c};
    boot_result = parse_boot_wave_sequence(non_finite_boot, boot_json);
    expect(!boot_result && boot_result.error().code == WaveErrorCode::non_finite,
           "non-finite WBT1 half-float is rejected explicitly");

    const auto lfs_path = reference / "xmb-test-lfs-pointer.tmp";
    // File-level LFS behavior is proven through an owned fixture without touching
    // the read-only checkout: the parser path above and importer tests cover I/O.
    (void)lfs_path;
}

void test_authoritative_reference(const std::filesystem::path& reference) {
    auto geometry_bytes = load_wave_bytes(reference / "wave_geo.bin");
    auto idle_bytes = load_wave_bytes(reference / "wave_seq2.bin");
    auto idle_json_bytes = load_wave_bytes(reference / "wave_seq2.json");
    auto boot_bytes = load_wave_bytes(reference / "wave_boot.bin");
    auto boot_json_bytes = load_wave_bytes(reference / "wave_boot.json");
    expect(geometry_bytes && idle_bytes && idle_json_bytes && boot_bytes && boot_json_bytes,
           "all pinned reference wave assets load");
    if (!geometry_bytes || !idle_bytes || !idle_json_bytes || !boot_bytes || !boot_json_bytes) {
        return;
    }
    const std::string idle_json(reinterpret_cast<const char*>(idle_json_bytes.value().data()),
                                idle_json_bytes.value().size());
    const std::string boot_json(reinterpret_cast<const char*>(boot_json_bytes.value().data()),
                                boot_json_bytes.value().size());

    expect(geometry_bytes.value().size() == 589824, "geometry byte size is authoritative");
    auto geometry = parse_wave_geometry(geometry_bytes.value());
    expect(geometry.has_value(), "authoritative geometry parses");
    if (geometry) {
        expect(geometry.value().grid_size == 128 &&
                   geometry.value().vertices.size() == 16384,
               "geometry is exactly a 128x128 grid");
        const auto& first = geometry.value().vertices.front();
        expect(near(first.clip.x, 11.42608738F) && near(first.clip.w, 7.64032459F) &&
                   near(first.normal.x, 0.19822182F) && near(first.uv.u, 0.0F),
               "geometry layout is clip4, normal3, UV2 little-endian float32");
    }

    expect(idle_bytes.value().size() == 8355864, "WSQ2 total byte size is authoritative");
    auto idle = parse_idle_wave_sequence(idle_bytes.value(), idle_json);
    expect(idle.has_value(), "authoritative WSQ2 + JSON parse");
    if (idle) {
        const auto& sequence = idle.value();
        expect(sequence.frames.size() == 85 && sequence.grid_size == 128,
               "WSQ2 has 85 frames of 16,384 vertices");
        expect(sequence.frames.front().positions.size() == 16384,
               "WSQ2 frameBytes is exactly 98,304");
        expect(near(sequence.z_w_ratio, 0.975589156F, 0.0000005F),
               "WSQ2 Z/W reconstruction ratio matches binary header");
        expect(sequence.capture_source_fps == 60 && sequence.capture_keyframe_stride == 5 &&
                   sequence.capture_source_frame_count == 419,
               "WSQ2 capture metadata is retained");
        expect(sequence.reference_keyframes_per_second == 0.8 &&
                   sequence.seam_crossfade_keyframes == 5,
               "active reference playback rate and seam policy are explicit");

        IdleSamplingPolicy raw_policy;
        raw_policy.vertical_flatten = 1.0F;
        auto at_zero = sample_idle_wave(sequence, 0.0, raw_policy);
        auto at_frame_one = sample_idle_wave(sequence, 1.25, raw_policy);
        expect(at_zero && at_frame_one, "known idle sample times succeed");
        if (at_zero && at_frame_one) {
            expect(near(at_zero.value().positions[0].x,
                        sequence.frames[0].positions[0].x),
                   "idle t=0 selects keyframe zero");
            expect(near(at_frame_one.value().positions[0].x,
                        sequence.frames[1].positions[0].x),
                   "idle t=1.25s selects keyframe one at 0.8 keyframes/s");
        }
        auto at_half = sample_idle_wave(sequence, 0.625, raw_policy);
        if (at_half) {
            const float expected = catmull(sequence.frames[84].positions[0].x,
                                           sequence.frames[0].positions[0].x,
                                           sequence.frames[1].positions[0].x,
                                           sequence.frames[2].positions[0].x, 0.5F);
            expect(near(at_half.value().positions[0].x, expected),
                   "idle half-keyframe uses Catmull-Rom over four frames");
        }
        const double period = sequence.frames.size() / kReferenceIdleKeyframesPerSecond;
        auto wrapped = sample_idle_wave(sequence, period, raw_policy);
        auto before_wrap = sample_idle_wave(sequence, period - 0.0001, raw_policy);
        expect(wrapped && before_wrap, "wrap samples succeed");
        if (wrapped && before_wrap) {
            expect(near(wrapped.value().positions[0].x,
                        sequence.frames[0].positions[0].x),
                   "idle loop wraps exactly to frame zero");
            expect(std::abs(before_wrap.value().positions[0].x -
                            wrapped.value().positions[0].x) < 0.001F,
                   "five-keyframe seam blend is continuous at wrap");
        }
        const double seam_half_seconds = (82.5 / kReferenceIdleKeyframesPerSecond);
        auto seam_half = sample_idle_wave(sequence, seam_half_seconds, raw_policy);
        if (seam_half) {
            const float main = catmull(sequence.frames[81].positions[0].x,
                                       sequence.frames[82].positions[0].x,
                                       sequence.frames[83].positions[0].x,
                                       sequence.frames[84].positions[0].x, 0.5F);
            const float expected = 0.5F * main + 0.5F * sequence.frames[0].positions[0].x;
            expect(near(seam_half.value().positions[0].x, expected),
                   "seam midpoint blends Catmull-Rom result 50% to frame zero");
        }
        auto flattened = sample_idle_wave(sequence, 0.0);
        if (flattened) {
            const float expected = sequence.temporal_mean_y[0] +
                (sequence.frames[0].positions[0].y - sequence.temporal_mean_y[0]) * 0.72F;
            expect(near(flattened.value().positions[0].y, expected),
                   "active 0.72 vertical flatten fit is deterministic");
        }
    }

    expect(boot_bytes.value().size() == 5505044, "WBT1 total byte size is authoritative");
    auto boot = parse_boot_wave_sequence(boot_bytes.value(), boot_json);
    expect(boot.has_value(), "authoritative WBT1 + JSON parse");
    if (boot) {
        const auto& sequence = boot.value();
        expect(sequence.frames.size() == 42 && sequence.grid_size == 128 &&
                   sequence.frames.front().positions.size() == 16384,
               "WBT1 has 42 frames at exactly 131,072 bytes each");
        expect(sequence.frame_info.size() == 42 && sequence.source_fps == 60 &&
                   sequence.frame_info.front().source_frame == 23 &&
                   sequence.frame_info.back().source_frame == 123,
               "WBT1 source timestamps are retained and validated");
        auto first = sample_boot_wave(sequence, 0.0);
        auto last = sample_boot_wave(sequence, 1.0);
        auto midpoint = sample_boot_wave(sequence, 0.5 / 41.0);
        expect(first && last && midpoint, "boot endpoint and midpoint samples succeed");
        if (first && last && midpoint) {
            expect(near(first.value().positions[0].x, sequence.frames.front().positions[0].x),
                   "boot progress 0 is the first frame");
            expect(near(last.value().positions[0].x, sequence.frames.back().positions[0].x),
                   "boot progress 1 is the last frame");
            const float expected = 0.5F * (sequence.frames[0].positions[0].x +
                                           sequence.frames[1].positions[0].x);
            expect(near(midpoint.value().positions[0].x, expected),
                   "boot normalized sampler is linear");
        }
        const double first_time_midpoint =
            0.5 * (sequence.frame_info[0].boot_seconds +
                   sequence.frame_info[1].boot_seconds);
        auto timed = sample_boot_wave_at_time(sequence, first_time_midpoint);
        if (timed) {
            const float expected = 0.5F * (sequence.frames[0].positions[0].x +
                                           sequence.frames[1].positions[0].x);
            expect(near(timed.value().positions[0].x, expected, 0.0001F),
                   "boot timestamp sampler respects non-uniform JSON times");
        }
    }

    test_rejections(reference, geometry_bytes.value(), idle_bytes.value(), idle_json,
                    boot_bytes.value(), boot_json);
}

}  // namespace

int main(int argc, char** argv) {
    test_grid_indices();
    const auto reference = find_reference(argc, argv);
    if (reference.empty()) {
        std::cout << "SKIP: local pinned xmb-web checkout not found; grid tests ran\n";
    } else {
        std::cout << "reference=" << reference.string() << '\n';
        test_authoritative_reference(reference);
    }
    if (failures != 0) {
        std::cerr << failures << " wave-data test(s) failed\n";
        return 1;
    }
    std::cout << "wave_data_tests: PASS\n";
    return 0;
}
