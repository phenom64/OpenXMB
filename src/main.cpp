#include <iostream>
#include <thread>
#include <cstdlib>
#include <string_view>

#include <libintl.h>

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

#undef main
int main(int argc, char *argv[])
{
#ifndef NDEBUG
    spdlog::set_level(spdlog::level::trace);
#endif
    spdlog::cfg::load_env_levels();

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

    auto* shell = new app::shell(&window);
    if(program.get<bool>("--background-only")) {
        shell->set_background_only(true);
    }
    window.set_phase(shell, shell, shell);

    window.loop();

    return 0;
}
