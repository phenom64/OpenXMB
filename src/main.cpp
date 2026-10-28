#include "desktop/log_path.hpp"
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
#include <clocale>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#ifdef __APPLE__
#include <dlfcn.h>
#endif

#include <libintl.h>
#include "localization/shell_locale.hpp"
#include "openxmb/desktop/library.hpp"
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

static_assert(FMT_VERSION == 110200, "OpenXMB requires its pinned fmt headers");

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
import auroreengine;
import argparse;
import openxmb.app;
import openxmb.debug;
import openxmb.config;
import openxmb.constants;

namespace {
void startup_trace(const char* phase)
{
    if(std::getenv("OPENXMB_STARTUP_TRACE")) {
        std::fprintf(stderr, "OpenXMB startup: %s\n", phase);
        std::fflush(stderr);
    }
}
}

#ifdef _WIN32
namespace {
std::filesystem::path openxmb_log_path()
{
    auto dir=openxmb::desktop::log_directory();
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
int main(int argc, char *argv[]) try {
    startup_trace("entered main");
    std::set_terminate([]() {
        std::fputs("OpenXMB terminated with an uncaught exception\n", stderr);
        if(const auto exception = std::current_exception()) {
            try {
                std::rethrow_exception(exception);
            } catch(const std::exception& error) {
                std::fprintf(stderr, "%s\n", error.what());
            } catch(...) {
                std::fputs("Unknown exception\n", stderr);
            }
        }
        std::fflush(stderr);
        std::abort();
    });
#if defined(__APPLE__) || defined(_WIN32)
    const auto binary_directory=config::CONFIG.exe_directory;
    const char* previous_path=std::getenv("PATH");
#ifdef _WIN32
    const auto tools=(binary_directory/"tools/poppler/bin").string()+";"+binary_directory.string()+";"+(previous_path?previous_path:"");
    _putenv_s("PATH",tools.c_str());
#else
    const auto tools=binary_directory.string()+":"+(previous_path?previous_path:"");
    setenv("PATH",tools.c_str(),1);
    const auto contents=binary_directory.parent_path();
    const auto icd=contents/"Resources/vulkan/icd.d/MoltenVK_icd.json";
    if(!std::getenv("VK_ICD_FILENAMES")&&std::filesystem::is_regular_file(icd))
        setenv("VK_ICD_FILENAMES",icd.c_str(),1);
    const auto loader=contents/"Frameworks/libvulkan.1.dylib";
    if(std::filesystem::is_regular_file(loader)) {
        if(!std::getenv("SDL_VULKAN_LIBRARY")) setenv("SDL_VULKAN_LIBRARY",loader.c_str(),1);
        if(!std::getenv("AUROREENGINE_VULKAN_LIBRARY")) setenv("AUROREENGINE_VULKAN_LIBRARY",loader.c_str(),1);
        static_cast<void>(dlopen(loader.c_str(),RTLD_NOW|RTLD_GLOBAL));
    }
    const auto fontconfig=contents/"Resources/fonts/fonts.conf";
    if(!std::getenv("FONTCONFIG_FILE")&&std::filesystem::is_regular_file(fontconfig))
        setenv("FONTCONFIG_FILE",fontconfig.c_str(),1);
    const auto certificates=contents/"Resources/certs/cert.pem";
    if(std::filesystem::is_regular_file(certificates)) {
        if(!std::getenv("CURL_CA_BUNDLE")) setenv("CURL_CA_BUNDLE",certificates.c_str(),1);
        if(!std::getenv("SSL_CERT_FILE")) setenv("SSL_CERT_FILE",certificates.c_str(),1);
    }
#endif
#endif
    startup_trace("runtime paths configured");
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
    startup_trace("logging initialized");

    argparse::ArgumentParser program("OpenXMB", OPENXMB_VERSION_STRING);
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

    program.add_argument("--open-media").default_value(std::string{}).help("Open a media file in its native viewer after startup");
    program.add_argument("--force-dir").default_value(std::string{}).metavar("DIRECTORY")
        .help("Use this directory for Photo, Music and Video libraries for this run only");
    program.add_argument("--open-panel").default_value(std::string{}).help("Open themes, browser, emulation, discs or system");
    try {
        startup_trace("parsing arguments");
        program.parse_args(argc, argv);
    }
    catch (const std::exception& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        std::exit(1);
    }

    std::filesystem::path forced_media_directory;
    if(const auto directory=program.get<std::string>("--force-dir");!directory.empty()) {
        std::error_code error;
        forced_media_directory=std::filesystem::absolute(std::filesystem::u8path(directory),error).lexically_normal();
        if(error||!std::filesystem::is_directory(forced_media_directory,error)||error) {
            std::cerr<<"--force-dir must name an existing directory: "<<directory<<'\n';
            return 1;
        }
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
    setlocale(LC_ALL, "");

    config::CONFIG.load();
    if(std::getenv("OPENXMB_CONFIG") == nullptr) {
        try {
            openxmb::desktop::Library library;
            const auto path=library.config_path(library.active_profile().id);
            if(!std::filesystem::exists(path) || std::filesystem::file_size(path)<32) {
                config::CONFIG.profileConfigPath=path;
                config::CONFIG.save_config();
            } else config::CONFIG.switch_profile(path);
        } catch(const std::exception& error) { spdlog::warn("Could not restore local profile: {}",error.what()); }
    }
    // Initialize gettext/i18n: bind domain to our locale directory and use UTF-8
    config::CONFIG.forcedMediaDirectory=std::move(forced_media_directory);
    // Note: even without compiled translations, gettext will fall back to the
    // original strings, so text should still render in English.
    openxmb::localization::shell_locale::apply(config::CONFIG.language, config::CONFIG.locale_directory);
    spdlog::debug("Config loaded");

    SDL_SetMainReady();

    auroreengine::window_config window_config;
    window_config.name = "OpenXMB";
    window_config.title = "OpenXMB";
    window_config.preferredPresentMode = config::CONFIG.preferredPresentMode;
    window_config.sampleCount = config::CONFIG.sampleCount;
    window_config.fpsLimit = config::CONFIG.maxFPS;
    window_config.width = program.get<int>("--width");
    window_config.height = program.get<int>("--height");
    window_config.fullscreen = !program.get<bool>("--no-fullscreen");

    auroreengine::window window{window_config};
    window.init();

    spdlog::debug("Creating OpenXMB shell phase");
    auto* shell = new app::shell(&window);
    if(program.get<bool>("--background-only")) {
        shell->set_background_only(true);
    }
    spdlog::debug("Installing OpenXMB shell phase");
    window.set_phase(shell, shell, shell);
    shell->queue_panel(program.get<std::string>("--open-panel"));
    shell->queue_media(program.get<std::string>("--open-media"));

    spdlog::debug("Entering OpenXMB main loop");
    window.loop();
    spdlog::debug("OpenXMB main loop exited");

    return 0;
} catch(const std::exception& error) {
    std::fprintf(stderr, "OpenXMB startup or rendering failed: %s\n", error.what());
    return 1;
} catch(...) {
    std::fputs("OpenXMB startup or rendering failed with an unknown exception\n", stderr);
    return 1;
}
