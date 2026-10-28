#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace openxmb::xmb {

// The spline is evaluated by the GPU. Keeping control points rather than a
// displacement image reduces each frame's upload from 64 KiB to 7 KiB.
inline constexpr std::size_t kProceduralWaveRows = 64;
inline constexpr std::size_t kProceduralWaveControlPoints = 28;
inline constexpr std::uint32_t kProceduralWaveGrid = 100;
// Ribbon mesh resolution (columns along the screen, rows through the sheet's
// depth). Fold outlines follow row lines, so a dense row count keeps them
// smooth; the total stays within 16-bit indices. procedural_wave.vert
// hard-codes the same numbers.
inline constexpr std::uint32_t kProceduralWaveMeshColumns = 128;
inline constexpr std::uint32_t kProceduralWaveMeshRows = 448;

struct ProceduralWaveSettings {
    float flowSpeed{0.18F}, tension{0.12F}, damping{0.0001F};
    float length{0.306001F}, spacing{407.658F}, timeStep{1.0F};
    float bandAmplitude{0.200F}, bandSecondaryFreq{7.0F}, bandSecondaryAmp{0.025F};
    float travelSpeed1{0.25F}, travelAmp1{0.014F}, travelSpeed2{0.15F}, travelAmp2{0.008F};
    float perturbation{0.0998587F}, perturbationScale{0.07F};
    float waveCosAmp{0.09F}, waveBias{-0.1F}, waveHeightScale{0.5F}, waveSoftClip{0.22F};
    float rePipelineBlend{0.45F}, reDescriptorStrength{0.7F}, reSyntheticDescriptorSeed{1337.0F};
    float reSyntheticDescriptorMotion{0.65F}, reKernelGain{0.04F}, reNormalizeGain{0.08F};
    float reKernelPhaseStep{0.45F}, reIndexJitter{0.006F}, reTemporalSmooth{0.84F};
    float fresnelPower{4.0F}, fresnelScale{1.25F}, opacity{0.65F}, brightness{0.98F}, zDetailScale{0.08F};
    float ffdScale1X{1.5F}, ffdScale1Y{1.00077F}, ffdScale1Z{1.0F};
    float ffdScale2X{2.82755F}, ffdScale2Y{1.27579F}, ffdScale2Z{2.88782F};
    float ffdOffsetX{}, ffdOffsetY{-0.469999F}, ffdOffsetZ{}, ffdYAmp{0.06F}, ffdZAmp{0.01F};
    float posY{-0.2F}, tilt{0.1F}, thickness{1.9F};
    float bandFrequency{2.0F}, rowSpread{1.7F}, cosFrequency{1.0F}, fill{0.03F}, edgeSoft{0.6F};

    // Keys match theme parameter names. Invalid/unknown values leave state intact.
    bool set(std::string_view key, float value, bool* changed = nullptr) noexcept;
};

struct ProceduralParticleSettings {
    std::uint32_t count{2400};
    float opacity{1.0F}, sizeBase{3.6F}, sizeVar{6.0F}, flowSpeed{0.18F};
    float posY{0.1F}, spreadY{0.6F};
};

struct alignas(16) ProceduralWaveFrame {
    std::array<std::array<float, 4>, 16> settings{};
    std::array<float, kProceduralWaveRows * kProceduralWaveControlPoints> control{};
};

// All scratch storage is owned up front. update() performs no allocations, file
// reads, or random-device calls. Fresh samplers and explicit seeks are
// deterministic; continuous playback smooths at a refresh-rate-independent rate.
class ProceduralWaveSampler {
public:
    void update(double seconds, const ProceduralWaveSettings& settings,
                const ProceduralParticleSettings& particles = {}) noexcept;
    [[nodiscard]] const ProceduralWaveFrame& frame() const noexcept { return frame_; }
    [[nodiscard]] float displacement(float u, float v) const noexcept;
    void reset_history() noexcept { previous_seconds_ = -1.0; }

private:
    ProceduralWaveFrame frame_{};
    std::array<std::uint8_t, 0x2200> descriptor_{};
    std::array<float, 361 * 4> table_{};
    std::array<float, 64 * 4> kernel_{};
    std::array<float, 64 * 4> previous_kernel_{};
    double previous_seconds_{-1.0};
    float previous_seed_{-1.0F};
    float previous_strength_{-1.0F};
    float previous_length_{-1.0F};
    float previous_tension_{-1.0F};
    float previous_scale_{-1.0F};
};

} // namespace openxmb::xmb
