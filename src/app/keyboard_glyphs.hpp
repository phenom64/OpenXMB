#pragma once
#include <array>
#include <string_view>

namespace app::keyboard_glyphs {
inline std::string_view stem(int scancode,bool mac=false) {
    constexpr std::array<std::string_view,26> letters{"a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u","v","w","x","y","z"};
    constexpr std::array<std::string_view,10> digits{"1","2","3","4","5","6","7","8","9","0"};
    if(scancode>=4&&scancode<=29)return letters[scancode-4];
    if(scancode>=30&&scancode<=39)return digits[scancode-30];
    switch(scancode) {
    case 40:return "return";case 41:return "escape";case 42:return "backspace";case 43:return "tab";
    case 45:return "dash";case 47:return "bracketleft";case 48:return "bracketright";case 49:return "backslash";
    case 52:return "quote";case 53:return "tilde";case 54:return "comma";case 55:return "period";
    case 57:return "caps";case 76:return "delete";
    case 79:return "rightarrow";case 80:return "leftarrow";case 81:return "downarrow";case 82:return "uparrow";
    case 224:case 228:return "control";case 225:case 229:return "shift";
    case 226:case 230:return mac?"option":"alt";case 227:case 231:return mac?"command":"windows";
    default:return {};
    }
}
}
