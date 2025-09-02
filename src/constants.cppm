/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

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
    constexpr auto fallback_font = "./shell/Play-Regular.ttf";
#else
    constexpr auto asset_directory = "../share/shell/";
    constexpr auto locale_directory = "../share/locale/";
    constexpr auto fallback_font = "../share/shell/Play-Regular.ttf";
#endif

    constexpr auto fallback_datetime_format = "%m/%d %H:%M";
    constexpr auto pipeline_cache_file = "pipeline_cache.bin";
}
