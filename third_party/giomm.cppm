module;

#include <giomm.h>

export module giomm;

export namespace Gio {
    using Gio::init;
#if __linux__
    using Gio::DesktopAppInfo;
#endif
    using Gio::AppInfo;
    using Gio::Icon;
    using Gio::FileIcon;
    using Gio::ThemedIcon;
    using Gio::File;
    using Gio::FileInfo;
    using Gio::FileType;
    using Gio::Settings;
    using Gio::SettingsSchema;
    using Gio::SettingsSchemaSource;
    using Gio::SettingsSchemaKey;
    using Gio::Error;
    using Gio::FileQueryInfoFlags;

    namespace DBus {
        using Gio::DBus::Proxy;
        using Gio::DBus::Connection;
        using Gio::DBus::BusType;
        using Gio::DBus::Error;
    }
};
