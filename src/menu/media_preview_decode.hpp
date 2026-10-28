#pragma once

#include <openxmb/photos/photo_library.hpp>
#include <filesystem>

namespace openxmb::menu_preview {
enum class Kind { photo, music, video };

// CPU-only RGBA thumbnail; no playback or renderer state is touched.
// maximum_edge is clamped to [1, 256]. Missing/unusable media returns {};
// shared photo decoding may throw, so callers should catch decode errors.
openxmb::photos::Bitmap decode(const std::filesystem::path& path, Kind kind,
                              int maximum_edge = 256);
} // namespace openxmb::menu_preview
