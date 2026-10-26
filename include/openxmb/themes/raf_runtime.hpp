#pragma once

#include "openxmb/themes/library.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace openxmb::themes {

// Bounded, already-unpacked RAFO tree. Binary file attributes are kept in the
// ordered file table; attributes in elements are ordinary JSON scalar/arrays.
struct RafDocument {
    nlohmann::json elements = nlohmann::json::array();
    std::vector<std::vector<std::uint8_t>> files;
};

nlohmann::json compile_raf_animation(const RafDocument& document,
                                     const std::filesystem::path& output_directory,
                                     const ImportLimits& limits,
                                     std::vector<std::string>& warnings);
void validate_raf_animation(const nlohmann::json& animation,
                            const std::filesystem::path& directory,
                            const ImportLimits& limits = {});

class RafRuntime {
public:
    RafRuntime(nlohmann::json scene, std::vector<std::uint8_t> script = {},
               ImportLimits limits = {});
    ~RafRuntime();
    RafRuntime(RafRuntime&&) noexcept;
    RafRuntime& operator=(RafRuntime&&) noexcept;
    RafRuntime(const RafRuntime&) = delete;
    RafRuntime& operator=(const RafRuntime&) = delete;

    AnimationFrame sample(double seconds, const std::filesystem::path& directory = {});
    [[nodiscard]] nlohmann::json actor_state(std::string_view id) const;
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

AnimationFrame sample_raf_animation(const nlohmann::json& animation,
                                     const std::filesystem::path& directory,
                                     double seconds);

} // namespace openxmb::themes
