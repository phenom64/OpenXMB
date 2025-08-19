module;

#include <cstdint>

export module xmbshell.constants;

export namespace constants
{
    constexpr auto displayname = "XMB Shell";
    constexpr auto name = "xmbshell";

    constexpr uint32_t version = 1;

#if defined(_WIN32)
    constexpr auto asset_directory = "./xmbshell/";
    constexpr auto locale_directory = "./locale/";
    constexpr auto fallback_font = "./xmbshell/Ubuntu-R.ttf";
#else
    constexpr auto asset_directory = "../share/xmbshell/";
    constexpr auto locale_directory = "../share/locale/";
    constexpr auto fallback_font = "../share/xmbshell/Ubuntu-R.ttf";
#endif

    constexpr auto fallback_datetime_format = "%m/%d %H:%M";
    constexpr auto pipeline_cache_file = "pipeline_cache.bin";
}
