#pragma once
#include <array>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace openxmb::input {
enum class PromptControl {up,down,left,right,ok,cancel,extra,options};
struct PromptPart {std::string_view text;std::optional<PromptControl> control;};
inline std::vector<PromptPart> prompt_parts(std::string_view text) {
    constexpr std::array names{
        std::pair{"up",PromptControl::up},std::pair{"down",PromptControl::down},
        std::pair{"left",PromptControl::left},std::pair{"right",PromptControl::right},
        std::pair{"ok",PromptControl::ok},std::pair{"cancel",PromptControl::cancel},
        std::pair{"extra",PromptControl::extra},std::pair{"options",PromptControl::options}};
    std::vector<PromptPart> result;std::size_t literal=0,cursor=0;
    while((cursor=text.find('{',cursor))!=text.npos) {
        const auto end=text.find('}',cursor+1);if(end==text.npos)break;
        const auto name=text.substr(cursor+1,end-cursor-1);std::optional<PromptControl> control;
        for(const auto& [key,value]:names)if(name==key){control=value;break;}
        if(control) {
            if(cursor>literal)result.push_back({text.substr(literal,cursor-literal),{}});
            result.push_back({{},control});literal=end+1;
        }
        cursor=end+1;
    }
    if(literal<text.size())result.push_back({text.substr(literal),{}});
    return result;
}
}
