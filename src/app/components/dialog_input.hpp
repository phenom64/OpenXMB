#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace app::dialog_input {
enum class Command { none,left,right,up,down,ok,cancel,options,extra };
enum class Outcome { handled,accept,cancel };
template<class Action> constexpr Command mapped(Action action) {
    switch(action) {
    case Action::left:return Command::left; case Action::right:return Command::right;
    case Action::up:return Command::up; case Action::down:return Command::down;
    case Action::ok:return Command::ok; case Action::cancel:return Command::cancel;
    case Action::options:return Command::options; case Action::extra:return Command::extra;
    default:return Command::none;
    }
}
template<class Action,class Semantic,class Raw>
decltype(auto) dispatch(Action action,bool button,Semantic&& semantic,Raw&& raw) {
    if(button && action!=Action::none) return std::forward<Semantic>(semantic)(action);
    return std::forward<Raw>(raw)();
}
struct TextEntry {
    explicit TextEntry(std::string initial):value(std::move(initial)){}
    std::string value;
    int row=0,col=0;
    bool caps=false,left_shift=false,right_shift=false;
    static constexpr std::array<std::string_view,5> rows{"1234567890","qwertyuiop","asdfghjkl:","zxcvbnm./-","_@+?=\\()[]"};
    bool upper() const {return caps != (left_shift||right_shift);}
    void erase() {
        if(value.empty())return;
        auto start=value.size()-1;
        while(start && (static_cast<unsigned char>(value[start])&0xc0)==0x80)--start;
        value.resize(start);
    }
    void append(char ch) {if(value.size()<4096)value+=ch;}
    Outcome event(Command command,std::optional<unsigned> down={},std::optional<unsigned> up={}) {
        if(up==225)left_shift=false;
        if(up==229)right_shift=false;
        switch(command) {
        case Command::cancel:return Outcome::cancel;
        case Command::options:return Outcome::accept;
        case Command::extra:erase();return Outcome::handled;
        case Command::up:row=(row+5)%6;return Outcome::handled;
        case Command::down:row=(row+1)%6;return Outcome::handled;
        case Command::left:col=(col+9)%10;return Outcome::handled;
        case Command::right:col=(col+1)%10;return Outcome::handled;
        case Command::ok:
            if(down)return Outcome::accept;
            if(row==5) {
                if(col<2)erase();else if(col<5)append(' ');else if(col<7)caps=!caps;else return Outcome::accept;
            } else {char ch=rows[row][col];if(upper()&&ch>='a'&&ch<='z')ch-='a'-'A';append(ch);}
            return Outcome::handled;
        default:break;
        }
        if(!down)return Outcome::handled;
        const auto code=*down;
        if(code==40||code==88)return Outcome::accept;
        if(code==42){erase();return Outcome::handled;}
        if(code==225){left_shift=true;return Outcome::handled;}
        if(code==229){right_shift=true;return Outcome::handled;}
        char ch=0;
        if(code>=4&&code<=29)ch=static_cast<char>((upper()?'A':'a')+code-4);
        else if(code>=30&&code<=38)ch=upper()?std::string_view("!@#$%^&*(")[code-30]:static_cast<char>('1'+code-30);
        else if(code==39)ch=upper()?')':'0';
        else if(code==44)ch=' ';
        else if(code>=45&&code<=56) {
            constexpr std::array<char,12> punctuation{'-','=','[',']','\\','\\',';','\'','`',',','.','/'};
            ch=upper()?std::string_view("_+{}||:\"~<>?")[code-45]:punctuation[code-45];
        }
        if(ch)append(ch);
        return Outcome::handled;
    }
};
}
