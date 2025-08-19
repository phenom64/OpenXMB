module;

#if __linux__
#include <sdbus-c++/sdbus-c++.h>
#endif

export module xmbshell.dbus;
import dreamrender;
import xmbshell.app;

export namespace dbus
{
    class dbus_server
    {
        public:
            dbus_server(dreamrender::window* w, app::xmbshell* xmb);
            ~dbus_server();
            void run();
        private:
#if __linux__
            std::unique_ptr<sdbus::IConnection> connection;
            std::unique_ptr<sdbus::IObject> object;
#endif
            dreamrender::window* win;
            app::xmbshell* xmb;
    };
}
