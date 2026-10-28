#pragma once
#include <filesystem>
#include <string_view>
#include <string>
#include <vector>
namespace openxmb::localization {
bool font_covers(const std::filesystem::path& font,std::u8string_view text);
std::filesystem::path select_font(const std::filesystem::path& configured,
    const std::filesystem::path& assets,std::string_view language);
std::vector<std::string> font_catalog_texts(const std::filesystem::path& locales,std::string_view language);
}
