#pragma once

#include <string_view>

namespace openxmb::xmb {

// This is the only product identity permitted in the OpenXMB boot scene.
// It intentionally remains data owned by OpenXMB, never baked into a shader.
inline constexpr std::string_view kStartupIdentity =
    "Syndromatic Limited Bharat Britannia";
inline constexpr std::string_view kStartupIdentitySemanticId =
    "openxmb.identity.syndromatic-limited-bharat-britannia";

} // namespace openxmb::xmb
