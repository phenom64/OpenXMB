#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <webkit2/webkit2.h>
#include <X11/Xlib.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>

// Web content runs in WebKit's sandboxed processes. Its native surface is a
// child of the shell window; Vulkan draws the surrounding XMB presentation.
namespace {
struct Browser {
    GtkWidget* window{};
    WebKitWebView* view{};
    Display* display{};
    Window parent{},child{};
    std::filesystem::path state_path;
    std::string hover;
    int width{},height{};
    std::int64_t command_sequence{};
    bool address_requested{};
    void reveal() {
        gtk_widget_show(window);
        gtk_widget_grab_focus(GTK_WIDGET(view));
        gdk_window_focus(gtk_widget_get_window(window),GDK_CURRENT_TIME);
    }
    void publish() const {
        const char* uri=webkit_web_view_get_uri(view);const char* title=webkit_web_view_get_title(view);
        const nlohmann::json state{{"uri",uri?uri:""},{"title",title?title:""},{"hover",hover},
            {"address_requested",address_requested},{"loading",webkit_web_view_is_loading(view)!=FALSE},{"back",webkit_web_view_can_go_back(view)!=FALSE}};
        auto pending=state_path;pending+=".tmp";
        std::ofstream file(pending,std::ios::binary|std::ios::trunc);file<<state.dump();file.close();
        std::error_code error;std::filesystem::rename(pending,state_path,error);
    }
};
int ignore_closed_parent(Display*,XErrorEvent*) {return 0;}
GdkFilterReturn parent_lifecycle(GdkXEvent* native,GdkEvent*,gpointer data) {
    const auto& event=*static_cast<XEvent*>(native);
    const auto& b=*static_cast<Browser*>(data);
    if(event.type==DestroyNotify&&
       (event.xdestroywindow.window==b.parent||event.xdestroywindow.window==b.child)) {
        // X11 has already destroyed the child surface. Exit before GTK queues
        // another paint against its invalid drawable; closing IPC releases the
        // sandboxed web processes and the OS releases native resources.
        std::_Exit(EXIT_SUCCESS);
    }
    return GDK_FILTER_CONTINUE;
}
gboolean update(gpointer data) {
    auto& b=*static_cast<Browser*>(data);
    XWindowAttributes a{};
    if(!XGetWindowAttributes(b.display,b.parent,&a)||a.map_state==IsUnmapped) {gtk_main_quit();return G_SOURCE_REMOVE;}
    if(a.width!=b.width||a.height!=b.height) {
        b.width=a.width;b.height=a.height;
        const double scale=std::min(a.width/1920.0,a.height/1080.0);
        const int ox=(a.width-1920*scale)/2,oy=(a.height-1080*scale)/2;
        const int width=std::max(1,static_cast<int>(1094*scale)),height=std::max(1,static_cast<int>(928*scale));
        gtk_window_resize(GTK_WINDOW(b.window),width,height);
        XMoveResizeWindow(b.display,b.child,ox+static_cast<int>(414*scale),oy+static_cast<int>(76*scale),width,height);
        XFlush(b.display);
    }
    try {
        std::ifstream input(b.state_path.parent_path()/"page-command.json");
        if(input) {
            const auto message=nlohmann::json::parse(input);
            const auto sequence=message.value("sequence",std::int64_t{});
            if(sequence!=b.command_sequence) {
                b.command_sequence=sequence;b.address_requested=false;const auto command=message.value("command",std::string{});
                if(command=="hide") gtk_widget_hide(b.window);
                else if(command=="show") b.reveal();
                else if(command=="navigate") {const auto uri=message.value("value",std::string{});if(uri.starts_with("http://")||uri.starts_with("https://")) webkit_web_view_load_uri(b.view,uri.c_str());b.reveal();}
                else if(command=="back") webkit_web_view_go_back(b.view);
                else if(command=="forward") webkit_web_view_go_forward(b.view);
                else if(command=="text") {b.reveal();const auto text=message.value("value",std::string{});webkit_web_view_execute_editing_command_with_argument(b.view,"InsertText",text.c_str());}
                else if(command=="reload") webkit_web_view_reload(b.view);
            }
        }
    }catch(...) {}
    b.publish();return G_SOURCE_CONTINUE;
}
void mouse_target(WebKitWebView*,WebKitHitTestResult* hit,guint,gpointer data) {
    auto& b=*static_cast<Browser*>(data);
    const char* uri=webkit_hit_test_result_context_is_link(hit)?webkit_hit_test_result_get_link_uri(hit):nullptr;
    b.hover=uri?uri:"";
}
gboolean key(GtkWidget*,GdkEventKey* key,gpointer data) {
    auto& b=*static_cast<Browser*>(data);
    if(key->keyval==GDK_KEY_Escape&&!(key->state&GDK_CONTROL_MASK)) {gtk_main_quit();return TRUE;}
    if((key->state&GDK_CONTROL_MASK)&&(key->keyval==GDK_KEY_l||key->keyval==GDK_KEY_L)) {b.address_requested=true;b.publish();return TRUE;}
    if(key->keyval==GDK_KEY_F5) {webkit_web_view_reload(b.view);return TRUE;}
    if((key->state&GDK_MOD1_MASK)&&key->keyval==GDK_KEY_Left) {webkit_web_view_go_back(b.view);return TRUE;}
    if((key->state&GDK_MOD1_MASK)&&key->keyval==GDK_KEY_Right) {webkit_web_view_go_forward(b.view);return TRUE;}
    return FALSE;
}
}
int main(int argc,char** argv) {
    if(argc!=5) return 2;
    const std::string uri=argv[1];
    if(uri.rfind("https://",0)!=0&&uri.rfind("http://",0)!=0) return 2;
    Browser b;
    try {b.parent=std::stoull(argv[2]);b.state_path=argv[4];}catch(...) {return 2;}
    if(!gtk_init_check(nullptr,nullptr)) return 3;
    b.display=GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    XSetErrorHandler(ignore_closed_parent);
    XWindowAttributes parent{};if(!b.parent||!XGetWindowAttributes(b.display,b.parent,&parent)) return 3;
    const auto profile=std::filesystem::path(argv[3]);
    std::filesystem::create_directories(profile/"cache");std::filesystem::create_directories(profile/"data");
    auto* manager=webkit_website_data_manager_new("base-data-directory",(profile/"data").c_str(),"base-cache-directory",(profile/"cache").c_str(),nullptr);
    auto* context=webkit_web_context_new_with_website_data_manager(manager);
    webkit_web_context_set_sandbox_enabled(context,TRUE);
    b.view=WEBKIT_WEB_VIEW(webkit_web_view_new_with_context(context));
    g_object_unref(context);g_object_unref(manager);
    b.window=gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(b.window),"OpenXMB Web Content");
    gtk_window_set_decorated(GTK_WINDOW(b.window),FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(b.window),TRUE);
    gtk_container_add(GTK_CONTAINER(b.window),GTK_WIDGET(b.view));
    g_signal_connect(b.window,"destroy",G_CALLBACK(gtk_main_quit),nullptr);
    g_signal_connect(b.window,"key-press-event",G_CALLBACK(key),&b);
    g_signal_connect(b.view,"mouse-target-changed",G_CALLBACK(mouse_target),&b);
    gtk_widget_show_all(b.window);
    b.child=GDK_WINDOW_XID(gtk_widget_get_window(b.window));
    XReparentWindow(b.display,b.child,b.parent,0,0);
    XSelectInput(b.display,b.parent,StructureNotifyMask);
    gdk_window_add_filter(nullptr,parent_lifecycle,&b);
    update(&b);
    webkit_web_view_load_uri(b.view,uri.c_str());
    g_timeout_add(200,update,&b);
    b.reveal();
    gtk_main();
    return 0;
}
