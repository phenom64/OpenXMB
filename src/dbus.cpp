module;

#if __linux__
#include <sdbus-c++/sdbus-c++.h>
#endif

module xmbshell.dbus;

import dreamrender;
import spdlog;
import sdl2;
import xmbshell.app;
import xmbshell.config;

namespace dbus
{
    dbus_server::dbus_server(dreamrender::window* w, app::xmbshell* x) : win(w), xmb(x)
    {
#if __linux__
        const char* serviceName = "re.jcm.xmbos.xmbshell";
        connection = sdbus::createSessionBusConnection(serviceName);

        const char* objectPath = "/re/jcm/xmbos/xmbshell";
        object = sdbus::createObject(*connection, objectPath);

        auto close = [this](){
            spdlog::info("Exit request from D-Bus");
            sdl::Event event = {
                .quit = {
                    .type = sdl::EventType::SDL_QUIT,
                    .timestamp = sdl::GetTicks()
                }
            };
            sdl::PushEvent(&event);
        };
        auto ingame = [this](){
            spdlog::info("Ingame XMB request from D-Bus");
            sdl::RaiseWindow(win->win.get());
            xmb->set_ingame_mode(true);
        };

        object->registerMethod("close").onInterface("re.jcm.xmbos.Window").implementedAs(std::move(close));
        object->registerMethod("ingame").onInterface("re.jcm.xmbos.Window").implementedAs(std::move(ingame));
        object->registerProperty("fps").onInterface("re.jcm.xmbos.Render").withGetter([this](){return win->currentFPS;});
        object->registerProperty("re.jcm.xmbos.Render", "maxFps", "i",
            [this](sdbus::PropertyGetReply& reply){reply << static_cast<int>(config::CONFIG.maxFPS);},
            [this](sdbus::PropertySetCall& call){
                int maxFPS{};
                call >> maxFPS;
                config::CONFIG.setMaxFPS(maxFPS);
            });

        object->finishRegistration();
#endif
    }

    dbus_server::~dbus_server()
    {
#if __linux__
        connection->leaveEventLoop();
#endif
    }

    void dbus_server::run()
    {
#if __linux__
        connection->enterEventLoopAsync();
#endif
    }
}
