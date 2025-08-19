#include <iostream>
#include <thread>

#include <libintl.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

import sdl2;
import spdlog;
import glibmm;
import giomm;
import dreamrender;
import argparse;
import xmbshell.app;
import xmbshell.dbus;
import xmbshell.config;

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

    try {
        program.parse_args(argc, argv);
    }
    catch (const std::exception& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        std::exit(1);
    }

    spdlog::info("Welcome to OpenXMB!");
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

    Gio::init();
    setlocale(LC_ALL, "");
    // bindtextdomain("xmbshell", config::CONFIG.locale_directory.string().c_str());
    // textdomain("xmbshell");
    Glib::RefPtr<Glib::MainLoop> loop;
    std::thread main_loop_thread([&loop]() {
        loop = Glib::MainLoop::create();
        loop->run();
    });

    config::CONFIG.load();
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

    auto* shell = new app::xmbshell(&window);
    if(program.get<bool>("--background-only")) {
        shell->set_background_only(true);
    }
    window.set_phase(shell, shell, shell);

    window.loop();

    loop->quit();
    main_loop_thread.join();

    return 0;
}