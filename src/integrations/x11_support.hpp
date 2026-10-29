#pragma once
#ifdef __linux__
#include "linux_process_identity.hpp"
#include <xcb/xcb.h>
#include <xcb/res.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openxmb::integrations::x11_detail {
struct Free {void operator()(void* pointer)const{std::free(pointer);}};
template<class T>using Reply=std::unique_ptr<T,Free>;
inline bool x11_session() {
  const auto* type=std::getenv("XDG_SESSION_TYPE");
  if(type&&std::string_view(type)=="wayland")return false;
  const auto* wayland=std::getenv("WAYLAND_DISPLAY");
  return !(wayland&&*wayland);
}
struct Connection {
  xcb_connection_t* c{};xcb_screen_t* screen{};int screen_index{};
  Connection(){if(!x11_session())return;c=xcb_connect(nullptr,&screen_index);if(!c||xcb_connection_has_error(c))return;Reply<xcb_query_extension_reply_t> wayland{xcb_query_extension_reply(c,xcb_query_extension(c,8,"XWAYLAND"),nullptr)};if(wayland&&wayland->present)return;auto it=xcb_setup_roots_iterator(xcb_get_setup(c));auto index=screen_index;while(index--&&it.rem)xcb_screen_next(&it);if(it.rem)screen=it.data;}
  ~Connection(){if(c)xcb_disconnect(c);}
  explicit operator bool()const{return c&&screen&&!xcb_connection_has_error(c);}
  Connection(const Connection&)=delete;Connection& operator=(const Connection&)=delete;
  xcb_atom_t atom(const char* name)const {Reply<xcb_intern_atom_reply_t> reply{xcb_intern_atom_reply(c,xcb_intern_atom(c,0,std::strlen(name),name),nullptr)};return reply?reply->atom:XCB_ATOM_NONE;}
  bool check(xcb_void_cookie_t cookie)const {Reply<xcb_generic_error_t> error{xcb_request_check(c,cookie)};return !error&&!xcb_connection_has_error(c);}
  std::vector<std::uint32_t> property(xcb_window_t window,const char* name,xcb_atom_t type,std::uint32_t limit=4096)const {
    Reply<xcb_get_property_reply_t> reply{xcb_get_property_reply(c,xcb_get_property(c,0,window,atom(name),type,0,limit),nullptr)};
    if(!reply||reply->format!=32||reply->type!=type||reply->bytes_after||reply->value_len>limit)return {};
    const auto* data=static_cast<const std::uint32_t*>(xcb_get_property_value(reply.get()));return {data,data+reply->value_len};
  }
  bool property(xcb_window_t window,const char* name,xcb_atom_t type,std::span<const std::uint32_t> values)const {
    return check(xcb_change_property_checked(c,XCB_PROP_MODE_REPLACE,window,atom(name),type,32,values.size(),values.data()));
  }
  std::optional<std::int64_t> client_pid(xcb_window_t window)const {
    // XRes identifies the connection which owns the XID. A client-supplied
    // _NET_WM_PID by itself must never establish a process capability.
    Reply<xcb_res_query_version_reply_t> version{xcb_res_query_version_reply(c,xcb_res_query_version(c,1,2),nullptr)};
    if(!version||version->server_major<1||(version->server_major==1&&version->server_minor<2))return {};
    const xcb_res_client_id_spec_t spec{window,XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID};
    Reply<xcb_res_query_client_ids_reply_t> ids{xcb_res_query_client_ids_reply(c,xcb_res_query_client_ids(c,1,&spec),nullptr)};
    if(!ids||ids->num_ids>16)return {};
    for(auto it=xcb_res_query_client_ids_ids_iterator(ids.get());it.rem;xcb_res_client_id_value_next(&it))
      if(it.data->spec.mask==XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID&&xcb_res_client_id_value_value_length(it.data)==1){const auto pid=*xcb_res_client_id_value_value(it.data);if(pid&&pid!=std::numeric_limits<std::uint32_t>::max())return pid;}
    return {};
  }
  bool compositor()const {const auto name="_NET_WM_CM_S"+std::to_string(screen_index);Reply<xcb_get_selection_owner_reply_t> owner{xcb_get_selection_owner_reply(c,xcb_get_selection_owner(c,atom(name.c_str())),nullptr)};return owner&&owner->owner;}
  bool active(xcb_window_t window)const {const auto value=property(screen->root,"_NET_ACTIVE_WINDOW",XCB_ATOM_WINDOW,1);return value.size()==1&&value[0]==window;}
  bool focused(xcb_window_t window)const {
    Reply<xcb_get_input_focus_reply_t> focus{xcb_get_input_focus_reply(c,xcb_get_input_focus(c),nullptr)};if(!focus)return false;
    auto child=focus->focus;
    for(unsigned depth=0;depth<32&&child&&child!=XCB_INPUT_FOCUS_POINTER_ROOT;++depth){if(child==window)return true;if(child==screen->root)return false;Reply<xcb_query_tree_reply_t> tree{xcb_query_tree_reply(c,xcb_query_tree(c,child),nullptr)};if(!tree||tree->parent==child)return false;child=tree->parent;}
    return false;
  }
  bool activate(xcb_window_t window)const {
    xcb_client_message_event_t event{};event.response_type=XCB_CLIENT_MESSAGE;event.window=window;event.type=atom("_NET_ACTIVE_WINDOW");event.format=32;event.data.data32[0]=1;event.data.data32[1]=XCB_CURRENT_TIME;
    return check(xcb_send_event_checked(c,0,screen->root,XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,reinterpret_cast<const char*>(&event)));
  }
};
}
#endif
