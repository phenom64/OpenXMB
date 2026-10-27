#pragma once
#include <array>
#include <charconv>
#include <map>
#include <string>
#include <string_view>

namespace app::input_bindings {
inline constexpr std::array<std::string_view,8> actions{"left","right","up","down","ok","cancel","options","extra"};
inline constexpr std::array<int,8> keyboard{80,79,82,81,40,41,43,57};
inline constexpr std::array<int,8> controller{13,14,11,12,0,1,3,2};
inline int read(const std::map<std::string,std::string>& preferences,bool pad,std::size_t index) {
    const auto fallback=pad?controller[index]:keyboard[index];
    const auto it=preferences.find(std::string(pad?"pad.":"key.")+std::string(actions[index]));
    if(it==preferences.end())return fallback;
    int value{};const auto& text=it->second;const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
    if(error!=std::errc{}||end!=text.data()+text.size()||value<0||value>(pad?20:512))return fallback;
    return value;
}
inline void assign(std::map<std::string,std::string>& preferences,const std::string& key,int value) {
    const bool pad=key.starts_with("pad.");const auto prefix=std::string(pad?"pad.":"key.");
    std::size_t selected=actions.size();for(std::size_t i=0;i<actions.size();++i)if(prefix+std::string(actions[i])==key)selected=i;
    if(selected==actions.size())return;
    const auto old=read(preferences,pad,selected);
    for(std::size_t i=0;i<actions.size();++i)if(i!=selected&&read(preferences,pad,i)==value)preferences[prefix+std::string(actions[i])]=std::to_string(old);
    preferences[key]=std::to_string(value);
}
inline int glyph_index(const std::map<std::string,std::string>& preferences,bool pad,std::size_t action) {
    const auto code=read(preferences,pad,action);
    for(std::size_t i=0;i<actions.size();++i)if(code==(pad?controller[i]:keyboard[i]))return static_cast<int>(i);
    return -1;
}
inline std::string_view xbox_artwork(int button) {
    switch(button) {
    case 4:return "xbox_back";case 6:return "xbox_start";
    case 7:return "xbox_left_stick_button";case 8:return "xbox_right_stick_button";
    case 9:return "xbox_lb";case 10:return "xbox_rb";
    case 11:return "xbox_dpad_up";case 12:return "xbox_dpad_down";
    case 13:return "xbox_dpad_left";case 14:return "xbox_dpad_right";
    default:return {};
    }
}

}
