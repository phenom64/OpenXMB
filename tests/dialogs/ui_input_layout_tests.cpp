#include "../../src/app/components/dialog_input.hpp"
#include "../../src/app/components/dialog_text_layout.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace input = app::dialog_input;
namespace text = app::dialog_text;
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
// Variable-width UTF-8 measurement makes byte/count-based wrapping fail.
float measure(std::string_view value,float pixels=24) {
  float width=0;
  for(std::size_t i=0;i<value.size();) {
    const auto next=text::next_boundary(value,i);
    width+=(next-i>1?1.1F:value[i]=='W'?0.95F:value[i]=='i'?0.22F:0.5F)*pixels;
    i=next;
  }
  return width;
}
enum class FakeAction { none=91,left=44,right=18,up=6,down=33,ok=21,cancel=58,options=70,extra=81 };
int main() {
  try {
    input::TextEntry editor("Unchanged");
    check(editor.event(input::mapped(FakeAction::options),20)==input::Outcome::accept && editor.value=="Unchanged","mapped printable Options saves before typing");
    check(editor.event(input::mapped(FakeAction::cancel),21)==input::Outcome::cancel && editor.value=="Unchanged","mapped printable Cancel cancels before typing");
    check(editor.event(input::Command::none,4)==input::Outcome::handled && editor.value=="Unchangeda","unbound printable key still edits text");
    check(editor.event(input::Command::ok,44)==input::Outcome::accept,"mapped keyboard Confirm submits rather than inserting a space");
    editor.value.clear();
    check(editor.event(input::Command::ok)==input::Outcome::handled && editor.value=="1","controller Confirm selects an OSK key");
    check(editor.event(input::Command::extra,22)==input::Outcome::handled && editor.value.empty(),"mapped Extra deletes before raw S shortcut");
    editor.value="\xE6\x9B\xB2\xF0\x9F\x8E\xB5";
    editor.erase();check(editor.value=="\xE6\x9B\xB2","UTF-8 delete removes a complete codepoint");
    editor.value.clear();editor.event(input::Command::none,225);editor.event(input::Command::none,229);
    editor.event(input::Command::none,{},225);editor.event(input::Command::none,4);
    check(editor.value=="A","releasing one Shift does not clear the other held Shift");

    // PS3 keyboard layout: four character rows, then function and control rows.
    input::TextEntry osk("ab");
    check(osk.character(1,0)=="q" && osk.character(2,9)=="'" && osk.character(3,9)=="?","character rows follow the PS3 layout");
    osk.move_caret(-1);osk.insert("X");
    check(osk.value=="aXb","insertion happens at the caret, not only at the end");
    osk.erase();check(osk.value=="ab","backspace removes the codepoint before the caret");
    osk.move_caret(1);osk.append('c');check(osk.value=="abc","caret returns to the end");
    osk.row=4;osk.col=4;check(osk.focused().key==input::TextEntry::Key::backspace,"Backspace spans two columns");
    osk.event(input::Command::right);check(osk.col==6 && osk.focused().key==input::TextEntry::Key::space,"right skips across a wide key");
    osk.event(input::Command::left);check(osk.focused().key==input::TextEntry::Key::backspace,"left returns to the wide key");
    osk.row=5;osk.col=11;check(osk.focused().key==input::TextEntry::Key::paste,"control row reaches copy and paste");
    osk.event(input::Command::up);check(osk.row==4 && osk.col==9,"moving up from a wide row clamps the column");
    osk.row=5;osk.col=8;check(osk.event(input::Command::ok)==input::Outcome::accept,"Enter key accepts");
    osk.row=4;osk.col=0;osk.event(input::Command::ok);check(osk.page==1 && osk.character(2,1)=="@","@#: opens the symbol page");
    osk.event(input::Command::ok);check(osk.page==0,"@#: toggles back to letters");
    osk.row=4;osk.col=3;osk.event(input::Command::ok);check(osk.character(1,0)=="Q" && osk.character(0,1)=="@","a/A switches case, including the number row");
    osk.value="\xE6\x9B\xB2";osk.tail=0;osk.move_caret(-1);check(osk.caret()==0,"caret moves by whole UTF-8 codepoints");
    int confirms=0,raw_previous=0;
    check(input::dispatch(FakeAction::ok,true,[&](auto){++confirms;return 7;},[&]{++raw_previous;return 8;})==7 && confirms==1 && raw_previous==0,"mapped Confirm wins over LB previous-photo shortcut");
    check(input::dispatch(FakeAction::none,true,[&](auto){return 7;},[&]{++raw_previous;return 8;})==8 && raw_previous==1,"unbound shoulder retains previous-photo shortcut");
    check(input::dispatch(FakeAction::left,false,[&](auto){return 7;},[&]{return 8;})==8,"mouse/axis events retain their explicit raw receiver");

    const std::string token="C:/very/long/"+std::string(60,'W')+"/\xE6\x9B\xB2\xF0\x9F\x8E\xB5";
    const auto lines=text::wrap(token,150,[](auto value){return measure(value);});
    std::string joined;for(const auto& line:lines){check(measure(line)<=150.001F,"long token exceeds measured width");joined+=line;}
    check(joined==token,"long-token wrapping must preserve every UTF-8 byte");
    check(text::wrap("a\r\n\r\nb\n",1000,[](auto value){return measure(value);})==std::vector<std::string>{"a","","b",""},"hard line breaks and blank paragraphs are preserved");
    const auto words=text::wrap("Wi Wi Wi Wi",65,[](auto value){return measure(value);});
    check(words.size()>1 && std::ranges::all_of(words,[](const auto& line){return measure(line)<=65;}),"word wrapping must use actual widths");
    const auto tail=text::tail(token,120,[](auto value){return measure(value);});
    check(measure(tail)<=120.001F && tail.ends_with("\xE6\x9B\xB2\xF0\x9F\x8E\xB5"),"input tail is measured and does not split UTF-8");
    check(text::wrap("W",1,[](auto value){return measure(value);})==std::vector<std::string>{"W"},"oversized single codepoint must make progress");
    for(float ui:{0.8F,1.0F,1.4F,2.0F}) {
      const text::Metrics title{23*ui,7*ui},body{20*ui,6*ui},choice{22*ui,7*ui};
      const auto page=text::message_layout(2,100,2,title,body,choice,ui);
      check(page.capacity>=1,"scaled message must retain a body page");
      check(page.title_last_baseline+title.descent<page.body_top,"header overlaps body");
      check(page.body_first_baseline-body.ascent>=page.body_top-0.001F,"body ascender crosses its upper bound");
      check(page.body_first_baseline+(page.capacity-1)*page.body_pitch+body.descent<=page.body_bottom+0.001F,"body page descender crosses its lower bound");
      check(page.body_bottom<page.choice_first_baseline-choice.ascent,"body overlaps horizontal choices");
      const auto scaled=text::wrap(token,350,[&](auto value){return measure(value,28*ui);});
      for(const auto& line:scaled)check(measure(line,28*ui)<=350.001F,"choice wrapping ignored final UI text scale");
      check(text::line_capacity(480,38*ui,28*ui)>=1,"chooser line capacity underflow");
    }
    const std::vector<float> heights{40,80,40,120,40};
    const auto window=text::choice_window(heights,3,160,0);
    check(window.first<=3 && window.last>3,"selected variable-height choice must remain visible");
    check(text::choice_window({800},0,200,0).last==1,"oversized selected choice must have a page");
    check(text::choice_window({},0,200,0).last==0,"empty chooser must not underflow");
    std::cout<<"Mapped input, UTF-8 width wrapping and scaled dialog layout passed\n";
  } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
