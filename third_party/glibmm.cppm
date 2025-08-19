module;

#include <glibmm.h>

export module glibmm;

export namespace Glib {
    using Glib::Error;
    using Glib::ustring;
    using Glib::RefPtr;
    using Glib::setenv;
    using Glib::get_host_name;
    using Glib::get_user_name;
    using Glib::get_real_name;
    using Glib::get_home_dir;
    using Glib::get_user_cache_dir;
    using Glib::get_user_special_dir;
    using Glib::UserDirectory;
    using Glib::MainLoop;
    using Glib::Variant;
    using Glib::VariantBase;
    using Glib::VariantType;
    using Glib::VARIANT_TYPE_BOOL;
    using Glib::VariantContainerBase;
    using Glib::DBusObjectPathString;

    using Glib::operator==;
};

export namespace sigc {
    using sigc::mem_fun;
}
