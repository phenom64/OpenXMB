#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openxmb::photos {

struct Bitmap {
    int width{};
    int height{};
    std::vector<std::uint8_t> rgba;
};
struct Crop {
    double x{};
    double y{};
    double width{1.0};
    double height{1.0};
};
struct Metadata {
    std::filesystem::path path;
    std::uintmax_t bytes{};
    int width{};
    int height{};
    std::string modified;
    std::string camera;
    std::string captured;
    int orientation{1};
};

struct LoadedPhoto { Bitmap image; Metadata metadata; };
LoadedPhoto load_photo(const std::filesystem::path& path, int maximum_edge = 2048);

bool supported_image(const std::filesystem::path& path);
std::vector<std::filesystem::path> gallery(const std::filesystem::path& selected);
Metadata metadata(const std::filesystem::path& path);
Bitmap load_bitmap(const std::filesystem::path& path, int maximum_edge = 2048);
Bitmap rotate(Bitmap image, int quarter_turns);
Bitmap crop(const Bitmap& image, Crop region);
Crop centered_crop(int width, int height, double aspect_ratio);
std::filesystem::path save_copy(const std::filesystem::path& original,
                                int quarter_turns = 0,
                                std::optional<Crop> region = std::nullopt);
// Moves into a uniquely reserved recovery directory; never permanently deletes.
std::filesystem::path move_to_trash(const std::filesystem::path& original,
                                   const std::filesystem::path& trash_root = {});
std::string describe(const Metadata& metadata);

}
