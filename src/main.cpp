/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
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

#include <iostream>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include <libintl.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <spdlog/sinks/msvc_sink.h>
#endif

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

import sdl2;
import spdlog;
import dreamrender;
import argparse;
import openxmb.app;
import openxmb.debug;
import openxmb.config;
import openxmb.constants;

#ifdef _WIN32
namespace {
std::filesystem::path openxmb_log_path()
{
    const char* base = std::getenv("LOCALAPPDATA");
    if(!base || !*base) {
        base = std::getenv("APPDATA");
    }
    auto dir = base && *base
        ? std::filesystem::path(base) / "OpenXMB" / "logs"
        : std::filesystem::temp_directory_path() / "OpenXMB" / "logs";
    std::filesystem::create_directories(dir);
    return dir / "openxmb.log";
}

void initialize_windows_logging()
{
    try {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(openxmb_log_path().string(), true));
        sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());

        auto logger = std::make_shared<spdlog::logger>("OpenXMB", sinks.begin(), sinks.end());
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        spdlog::set_default_logger(logger);
    } catch(const std::exception& e) {
        OutputDebugStringA(("OpenXMB failed to initialize file logging: " + std::string(e.what()) + "\n").c_str());
    }
}

LONG WINAPI log_windows_exception(EXCEPTION_POINTERS* exception_info)
{
    if(exception_info && exception_info->ExceptionRecord) {
        char buffer[256]{};
        std::snprintf(buffer, sizeof(buffer),
            "Unhandled Windows exception 0x%08lX at %p\n",
            static_cast<unsigned long>(exception_info->ExceptionRecord->ExceptionCode),
            exception_info->ExceptionRecord->ExceptionAddress);
        OutputDebugStringA(buffer);
        spdlog::critical(
            "Unhandled Windows exception 0x{:08X} at {}",
            exception_info->ExceptionRecord->ExceptionCode,
            exception_info->ExceptionRecord->ExceptionAddress
        );
    } else {
        OutputDebugStringA("Unhandled Windows exception\n");
        spdlog::critical("Unhandled Windows exception");
    }
    spdlog::default_logger()->flush();
    return EXCEPTION_CONTINUE_SEARCH;
}
}
#endif

#undef main
int main(int argc, char *argv[])
{
#ifndef NDEBUG
    spdlog::set_level(spdlog::level::trace);
#endif
    spdlog::cfg::load_env_levels();
#ifdef _WIN32
    initialize_windows_logging();
    SetUnhandledExceptionFilter(log_windows_exception);
#endif
    spdlog::flush_on(spdlog::level::err);
    spdlog::cfg::load_env_levels();
    spdlog::cfg::load_argv_levels(argc, argv);

    argparse::ArgumentParser program("OpenXMB");
    program.add_argument("--width")
        .help("Width of the window")
        .metavar("WIDTH")
        .scan<'i', int>()
        .default_value(1280);
    program.add_argument("--height")
        .help("Height of the window")
        .metavar("HEIGHT")
        .scan<'i', int>()
        .default_value(800);
    program.add_argument("--no-fullscreen").flag()
        .help("Do not start in fullscreen mode");
    program.add_argument("--background-only").flag()
        .help("Only render the background");
    program.add_argument("--interfacefx-debug").flag()
        .help("Enable interface/UI graphics debug overlays (e.g., font atlas)");

    try {
        program.parse_args(argc, argv);
    }
    catch (const std::exception& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        std::exit(1);
    }

    spdlog::info("Welcome to OpenXMB!");
    // Initialize interface/FX debug mode from compile-time, env, or runtime flag
#ifdef IFXDEBUG
    openxmb::debug::interfacefx_debug = true;
#endif
    if(const char* e = std::getenv("OPENXMB_IFXDEBUG")) {
        std::string_view sv{e};
        if(sv == "1" || sv == "true" || sv == "on") {
            openxmb::debug::interfacefx_debug = true;
        }
    }
    if(program.get<bool>("--interfacefx-debug")) {
        openxmb::debug::interfacefx_debug = true;
    }
    std::set_terminate([]() {
        spdlog::critical("Uncaught exception");

        try {
            std::rethrow_exception(std::current_exception());
        } catch(const std::exception& e) {
            spdlog::critical("Exception: {}", e.what());
        } catch(...) {
            spdlog::critical("Unknown exception");
        }

        spdlog::default_logger()->flush();
        std::abort();
    });

    setlocale(LC_ALL, "");

    config::CONFIG.load();
    // Initialize gettext/i18n: bind domain to our locale directory and use UTF-8
    // Note: even without compiled translations, gettext will fall back to the
    // original strings, so text should still render in English.
    bindtextdomain(constants::name, config::CONFIG.locale_directory.string().c_str());
    bind_textdomain_codeset(constants::name, "UTF-8");
    textdomain(constants::name);
    spdlog::debug("Config loaded");

    SDL_SetMainReady();

    dreamrender::window_config window_config;
    window_config.name = "OpenXMB";
    window_config.title = "OpenXMB";
    window_config.preferredPresentMode = config::CONFIG.preferredPresentMode;
    window_config.sampleCount = config::CONFIG.sampleCount;
    window_config.fpsLimit = config::CONFIG.maxFPS;
    window_config.width = program.get<int>("--width");
    window_config.height = program.get<int>("--height");
    window_config.fullscreen = !program.get<bool>("--no-fullscreen");

    dreamrender::window window{window_config};
    window.init();

    spdlog::debug("Creating OpenXMB shell phase");
    auto* shell = new app::shell(&window);
    if(program.get<bool>("--background-only")) {
        shell->set_background_only(true);
    }
    spdlog::debug("Installing OpenXMB shell phase");
    window.set_phase(shell, shell, shell);

    spdlog::debug("Entering OpenXMB main loop");
    window.loop();
    spdlog::debug("OpenXMB main loop exited");

    return 0;
}
