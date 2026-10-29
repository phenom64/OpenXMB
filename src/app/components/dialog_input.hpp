#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "keyboard_completion.hpp"

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

    explicit TextEntry(std::string initial,std::vector<std::string> corpus={},bool local_completion=true)
        :value(std::move(initial)),completion_enabled_(local_completion),vocabulary_(local_completion?completion::vocabulary(corpus):std::vector<std::string>{}){}
    std::string value;
    std::size_t tail=0; // bytes after the caret; 0 keeps the caret at the end
    int row=0,col=0,page=0;
    bool caps=false,left_shift=false,right_shift=false;
    bool prediction_focused=false;
    std::size_t prediction_index=0;

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
        value.insert(caret(),text);prediction_focused=false;
    }
    void append(char ch) {insert(std::string_view(&ch,1));}
    void erase() {
        prediction_focused=false;
        auto end=caret();
        if(!end) return;
        auto start=end-1;
        while(start && continuation(value[start]))--start;
        value.erase(start,end-start);
    }
    void erase_forward() {
        prediction_focused=false;
        const auto start=caret();if(start==value.size())return;
        auto end=start+1;while(end<value.size()&&continuation(value[end]))++end;
        value.erase(start,end-start);tail=value.size()-start;
    }
    void move_caret(int direction) {
        prediction_focused=false;
        auto position=caret();
        if(direction<0&&position) {--position;while(position&&continuation(value[position]))--position;}
        else if(direction>0&&position<value.size()) {++position;while(position<value.size()&&continuation(value[position]))++position;}
        tail=value.size()-position;
    }
    void cycle_page() {page=(page+1)%3;}
    void move(Command command) {
        if(prediction_focused) {
            const auto choices=candidates();
            if(choices.empty()||command==Command::left||command==Command::right)prediction_focused=false;
            else if(command==Command::up)prediction_index=(prediction_index+choices.size()-1)%choices.size();
            else if(command==Command::down)prediction_index=(prediction_index+1)%choices.size();
            return;
        }
        if(command==Command::right&&row<character_rows&&col==columns-1&&!candidates().empty()) {
            prediction_focused=true;prediction_index=0;return;
        }
        if(delete_from_confirm_)stop_delete();
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
        if(prediction_focused) {accept_prediction();return Outcome::handled;}
        switch(focused().key) {
        case Key::character:insert(character(row,col));break;
        case Key::symbols:page=page==1?0:1;break;
        case Key::accents:page=page==2?0:2;break;
        case Key::caps:caps=!caps;break;
        case Key::backspace:erase();break;
        case Key::space:insert(" ");break;
        case Key::caret_left:move_caret(-1);break;
        case Key::caret_right:move_caret(1);break;
        case Key::up:case Key::down:
            if(!candidates().empty()) {prediction_focused=true;prediction_index=0;}break;
        case Key::input_mode:cycle_page();break;
        case Key::enter:return Outcome::accept;
        case Key::copy:return Outcome::copy;
        case Key::paste:return Outcome::paste;
        default:break;
        }
        return Outcome::handled;
    }
    Outcome event(Command command,std::optional<unsigned> down={},std::optional<unsigned> up={},
                  std::optional<unsigned> button_down={},std::optional<unsigned> button_up={}) {
        if(up&&*up<physical_slots)release(*up);
        if(button_up&&*button_up<controller_slots)release(physical_slots+*button_up);
        const auto source=down&&*down<physical_slots?down:
            button_down&&*button_down<controller_slots?std::optional<unsigned>(physical_slots+*button_down):std::nullopt;
        const bool first=source&&*source<feedback_.size()?!feedback_[*source].held:true;
        if(source&&*source<feedback_.size())feedback_[*source].held=true;
        switch(command) {
        case Command::cancel:cancel_holds();return Outcome::cancel;
        case Command::options:
            if(button_down) {if(first){insert(" ");if(source)feedback(*source,4,6);}return Outcome::handled;}
            cancel_holds();return Outcome::accept;
        case Command::extra:delete_press(source,false,first);return Outcome::handled;
        case Command::up:case Command::down:case Command::left:case Command::right:move(command);return Outcome::handled;
        case Command::ok:
            if(!first)return Outcome::handled;
            if(prediction_focused) {if(first)accept_prediction();return Outcome::handled;}
            if(down) {cancel_holds();return Outcome::accept;}
            if(focused().key==Key::backspace) {delete_press(source,false,first);delete_from_confirm_=true;return Outcome::handled;}
            if(source)feedback(*source,row,col);
            return activate();
        default:break;
        }
        if(button_down&&source&&first) {
            switch(*button_down) {
            case 4:cycle_page();feedback(*source,5,6);break;
            case 6:cancel_holds();return Outcome::accept;
            case 9:move_caret(-1);feedback(*source,5,2);break;
            case 10:move_caret(1);feedback(*source,5,3);break;
            case 21:page=page==2?0:2;feedback(*source,4,2);break;
            case 22:caps=!caps;feedback(*source,4,3);break;
            default:break;
            }
        }
        if(!down)return Outcome::handled;
        const auto code=*down;
        if(code==40||code==88) {
            if(!first)return Outcome::handled;
            if(prediction_focused) {if(first)accept_prediction();return Outcome::handled;}
            cancel_holds();return Outcome::accept;
        }
        if(code==42||code==76){delete_press(source,code==76,first);return Outcome::handled;}
        if(code==225){left_shift=true;feedback(code,4,3);return Outcome::handled;}
        if(code==229){right_shift=true;feedback(code,4,3);return Outcome::handled;}
        if(code==57){if(first)caps=!caps;feedback(code,4,3);return Outcome::handled;}
        if(code==43) {if(first&&!candidates().empty()){prediction_focused=!prediction_focused;prediction_index=0;}return Outcome::handled;}
        if(shortcut_modifier())return Outcome::handled;
        const auto ch=physical_character(code);
        if(ch) {
            append(ch);prediction_focused=false;
            if(ch==' ')feedback(code,4,6);
            else for(int r=0;r<character_rows;++r)for(int c=0;c<columns;++c)
                if(character(r,c)==std::string_view(&ch,1))feedback(code,r,c);
        }
        return Outcome::handled;
    }
    bool pressed(int r,int c) const {
        return std::any_of(feedback_.begin(),feedback_.end(),[&](const auto& item){
            return item.row==r&&cell(r,c).first<=item.col&&item.col<=cell(r,c).last&&(item.held||item.flash>0);
        });
    }
    void cancel_holds() {
        feedback_={};left_shift=false;right_shift=false;stop_delete();
    }
    // Schedule against elapsed hold time, consuming at most eight repeats per
    // tick. OS key-repeat events never add a second deletion to this schedule.
    void advance(double seconds,double base_interval=0.180) {
        if(!std::isfinite(seconds)||seconds<0)return;
        for(auto& item:feedback_)item.flash=std::max(0.0,item.flash-seconds);
        if(!delete_source_)return;
        delete_elapsed_+=seconds;const auto base=std::isfinite(base_interval)?std::clamp(base_interval,0.04,1.0):0.180;
        const auto interval=[&](double at) {
            const auto t=std::clamp((at-1.2)/1.8,0.0,1.0);
            return base/(1+2*t*t*(3-2*t));
        };
        unsigned count=0;
        while(delete_elapsed_+1e-9>=delete_next_&&count<8) {
            if(delete_forward_)erase_forward();else erase();
            delete_next_+=interval(delete_next_);++count;
        }
        if(count==8&&delete_elapsed_>=delete_next_)delete_next_=delete_elapsed_+interval(delete_elapsed_);
    }
    const std::vector<std::string>& candidates() const {
        if(cached_caret_!=caret()||cached_value_!=value) {
            cached_value_=value;cached_caret_=caret();
            cached_candidates_=completion_enabled_?completion::candidates(vocabulary_,value,caret()):std::vector<std::string>{};
        }
        return cached_candidates_;
    }
    bool accept_prediction() {
        const auto choices=candidates();const auto token=completion::token(value,caret());
        if(choices.empty()||!token) {prediction_focused=false;prediction_index=0;return false;}
        const auto selected=completion::match_case(choices[std::min(prediction_index,choices.size()-1)],token->prefix);
        if(value.size()-(token->end-token->begin)+selected.size()>4096)return false;
        value.replace(token->begin,token->end-token->begin,selected);
        tail=value.size()-token->begin-selected.size();prediction_focused=false;prediction_index=0;
        return true;
    }
private:
    static constexpr unsigned physical_slots=512,controller_slots=32;
    struct Feedback {bool held=false;int row=-1,col=-1;double flash=0;};
    std::array<Feedback,physical_slots+controller_slots> feedback_{};
    bool completion_enabled_=true,delete_forward_=false,delete_from_confirm_=false;
    std::vector<std::string> vocabulary_;
    mutable std::string cached_value_;
    mutable std::size_t cached_caret_=std::string::npos;
    mutable std::vector<std::string> cached_candidates_;
    std::optional<unsigned> delete_source_;
    double delete_elapsed_=0,delete_next_=0.450;
    void feedback(unsigned source,int r,int c) {
        if(source>=feedback_.size())return;
        auto& item=feedback_[source];item.row=r;item.col=c;item.flash=0.090;
    }
    void release(unsigned source) {
        if(source<feedback_.size())feedback_[source].held=false;
        if(source==225)left_shift=false;
        if(source==229)right_shift=false;
        if(delete_source_==source)stop_delete();
    }
    void stop_delete() {delete_source_.reset();delete_elapsed_=0;delete_next_=0.450;delete_from_confirm_=false;}
    void delete_press(std::optional<unsigned> source,bool forward,bool first) {
        if(!first)return;
        if(forward)erase_forward();else erase();
        if(source) {
            feedback(*source,4,4);delete_source_=source;delete_elapsed_=0;delete_next_=0.450;
            delete_forward_=forward;delete_from_confirm_=false;
        }
    }
    bool shortcut_modifier() const {
        for(const auto code:{224u,226u,227u,228u,230u,231u})if(feedback_[code].held)return true;
        return false;
    }
    char physical_character(unsigned code) const {
        if(code>=4&&code<=29)return static_cast<char>((upper()?'A':'a')+code-4);
        const bool shifted=left_shift||right_shift;
        if(code>=30&&code<=38)return shifted?std::string_view("!@#$%^&*(")[code-30]:static_cast<char>('1'+code-30);
        if(code==39)return shifted?')':'0';
        if(code==44)return ' ';
        if(code>=45&&code<=56) {
            constexpr std::array<char,12> punctuation{'-','=','[',']','\\','\\',';','\'','`',',','.','/'};
            return shifted?std::string_view("_+{}||:\"~<>?")[code-45]:punctuation[code-45];
        }
        return 0;
    }
};
}
