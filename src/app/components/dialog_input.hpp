#pragma once
#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace app::dialog_input {
enum class Command { none,left,right,up,down,ok,cancel,options,extra };
enum class Outcome { handled,accept,cancel,copy,paste };
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

// On-screen keyboard model. Rows 0-3 are the character grid of the observed PS3
// layout; rows 4 and 5 are its function keys.
struct TextEntry {
    enum class Key { character,symbols,accents,caps,backspace,space,blank,up,down,caret_left,caret_right,keypad,input_mode,enter,copy,paste };
    struct Cell { Key key; int first,last; };
    using Row=std::array<std::string_view,10>;
    static constexpr int character_rows=4,total_rows=6,columns=10,wide_columns=12;
    // Letter and accent grids (with their capital variants) follow photos of
    // the console keyboard; the symbol page is a compact grid standing in
    // for the separate "@#:" panel, which was not captured.
    static constexpr std::array<std::array<Row,4>,3> pages{{
        {{{"1","2","3","4","5","6","7","8","9","0"},{"q","w","e","r","t","y","u","i","o","p"},{"a","s","d","f","g","h","j","k","l","'"},{"z","x","c","v","b","n","m",",",".","?"}}},
        {{{"!","\"","#","$","%","&","'","(",")","*"},{"+",",","-",".","/",":",";","<","=",">"},{"?","@","[","\\","]","^","_","`","{","|"},{"}","~","\xC2\xA1","\xC2\xBF","\xC2\xB0","\xC2\xAB","\xC2\xBB","\xE2\x82\xAC","\xC2\xA3","\xC2\xA5"}}},
        {{{"\xC3\xA0","\xC3\xA1","\xC3\xA2","\xC3\xA3","\xC3\xA4","\xC3\xA5","\xC3\xA6","\xC3\xA7","[","]"},{"\xC3\xA8","\xC3\xA9","\xC3\xAA","\xC3\xAB","\xC3\xAC","\xC3\xAD","\xC3\xAE","\xC3\xAF",";",":"},{"\xC3\xB1","\xC3\xB2","\xC3\xB3","\xC3\xB4","\xC3\xB5","\xC3\xB6","\xC3\xB8","\xC5\x93","`","\xC2\xA1"},{"\xC3\x9F","\xC3\xB9","\xC3\xBA","\xC3\xBB","\xC3\xBC","\xC3\xBD","\xC3\xBF",",",".","\xC2\xBF"}}}}};
    static constexpr std::array<std::array<Row,4>,3> pages_upper{{
        {{{"!","@","#","$","%","^","&","*","(",")"},{"Q","W","E","R","T","Y","U","I","O","P"},{"A","S","D","F","G","H","J","K","L","\""},{"Z","X","C","V","B","N","M","-","_","/"}}},
        {{{"!","\"","#","$","%","&","'","(",")","*"},{"+",",","-",".","/",":",";","<","=",">"},{"?","@","[","\\","]","^","_","`","{","|"},{"}","~","\xC2\xA1","\xC2\xBF","\xC2\xB0","\xC2\xAB","\xC2\xBB","\xE2\x82\xAC","\xC2\xA3","\xC2\xA5"}}},
        {{{"\xC3\x80","\xC3\x81","\xC3\x82","\xC3\x83","\xC3\x84","\xC3\x85","\xC3\x86","\xC3\x87","<",">"},{"\xC3\x88","\xC3\x89","\xC3\x8A","\xC3\x8B","\xC3\x8C","\xC3\x8D","\xC3\x8E","\xC3\x8F","=","+"},{"\xC3\x91","\xC3\x92","\xC3\x93","\xC3\x94","\xC3\x95","\xC3\x96","\xC3\x98","\xC5\x92","~","\""},{"\xC3\x9F","\xC3\x99","\xC3\x9A","\xC3\x9B","\xC3\x9C","\xC3\x9D","\xC5\xB8","-","_","/"}}}}};
    static constexpr std::array<Cell,7> function_row{{
        {Key::symbols,0,0},{Key::blank,1,1},{Key::accents,2,2},{Key::caps,3,3},
        {Key::backspace,4,5},{Key::space,6,7},{Key::blank,8,9}}};
    static constexpr std::array<Cell,10> control_row{{
        {Key::up,0,0},{Key::down,1,1},{Key::caret_left,2,2},{Key::caret_right,3,3},{Key::keypad,4,4},
        {Key::blank,5,5},{Key::input_mode,6,7},{Key::enter,8,9},{Key::copy,10,10},{Key::paste,11,11}}};

    explicit TextEntry(std::string initial):value(std::move(initial)){}
    std::string value;
    std::size_t tail=0; // bytes after the caret; 0 keeps the caret at the end
    int row=0,col=0,page=0;
    bool caps=false,left_shift=false,right_shift=false;

    bool upper() const {return caps != (left_shift||right_shift);}
    static int width(int r) {return r==5?wide_columns:columns;}
    static Cell cell(int r,int c) {
        if(r<character_rows) return {Key::character,c,c};
        if(r==4) {for(const auto& item:function_row) if(c>=item.first&&c<=item.last) return item;}
        else {for(const auto& item:control_row) if(c>=item.first&&c<=item.last) return item;}
        return {Key::blank,c,c};
    }
    Cell focused() const {return cell(row,col);}
    std::string character(int r,int c) const {
        if(r<0||r>=character_rows||c<0||c>=columns) return {};
        return std::string((upper()?pages_upper:pages)[page][r][c]);
    }
    std::size_t caret() const {return value.size()-std::min(tail,value.size());}
    static bool continuation(char ch) {return (static_cast<unsigned char>(ch)&0xc0)==0x80;}
    void insert(std::string_view text) {
        if(value.size()+text.size()>4096) return;
        value.insert(caret(),text);
    }
    void append(char ch) {insert(std::string_view(&ch,1));}
    void erase() {
        auto end=caret();
        if(!end) return;
        auto start=end-1;
        while(start && continuation(value[start]))--start;
        value.erase(start,end-start);
    }
    void move_caret(int direction) {
        auto position=caret();
        if(direction<0&&position) {--position;while(position&&continuation(value[position]))--position;}
        else if(direction>0&&position<value.size()) {++position;while(position<value.size()&&continuation(value[position]))++position;}
        tail=value.size()-position;
    }
    void cycle_page() {page=(page+1)%3;}
    void move(Command command) {
        const auto current=focused();
        switch(command) {
        case Command::up:row=(row+total_rows-1)%total_rows;break;
        case Command::down:row=(row+1)%total_rows;break;
        case Command::left:col=current.first?current.first-1:width(row)-1;break;
        case Command::right:col=current.last+1<width(row)?current.last+1:0;break;
        default:break;
        }
        col=std::min(col,width(row)-1);
    }
    Outcome activate() {
        switch(focused().key) {
        case Key::character:insert(character(row,col));break;
        case Key::symbols:page=page==1?0:1;break;
        case Key::accents:page=page==2?0:2;break;
        case Key::caps:caps=!caps;break;
        case Key::backspace:erase();break;
        case Key::space:insert(" ");break;
        case Key::caret_left:move_caret(-1);break;
        case Key::caret_right:move_caret(1);break;
        case Key::input_mode:cycle_page();break;
        case Key::enter:return Outcome::accept;
        case Key::copy:return Outcome::copy;
        case Key::paste:return Outcome::paste;
        default:break;
        }
        return Outcome::handled;
    }
    Outcome event(Command command,std::optional<unsigned> down={},std::optional<unsigned> up={}) {
        if(up==225)left_shift=false;
        if(up==229)right_shift=false;
        switch(command) {
        case Command::cancel:return Outcome::cancel;
        case Command::options:return Outcome::accept;
        case Command::extra:erase();return Outcome::handled;
        case Command::up:case Command::down:case Command::left:case Command::right:move(command);return Outcome::handled;
        case Command::ok:
            if(down)return Outcome::accept;
            return activate();
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
