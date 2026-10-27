#pragma once
#include <cstdlib>
#include <filesystem>
namespace openxmb::desktop {
inline std::filesystem::path log_directory() {
    const char* base=std::getenv("LOCALAPPDATA");
    if(!base||!*base)base=std::getenv("APPDATA");
    return base&&*base?std::filesystem::path(base)/"OpenXMB"/"logs":std::filesystem::temp_directory_path()/"OpenXMB"/"logs";
}
}
