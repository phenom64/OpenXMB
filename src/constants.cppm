module;

#include <cstdint>

export module openxmb.constants;

export namespace constants
{
    constexpr auto displayname = "XMB Shell";
    constexpr auto name = "shell";

    constexpr uint32_t version = 1;

#if defined(_WIN32)
    constexpr auto asset_directory = "./shell/";
    constexpr auto locale_directory = "./locale/";
    constexpr auto fallback_font = "./shell/Ubuntu-R.ttf";
#else
    constexpr auto asset_directory = "../share/shell/";
    constexpr auto locale_directory = "../share/locale/";
    constexpr auto fallback_font = "../share/shell/Ubuntu-R.ttf";
#endif

    constexpr auto fallback_datetime_format = "%m/%d %H:%M";
    constexpr auto pipeline_cache_file = "pipeline_cache.bin";
}