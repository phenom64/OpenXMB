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
void keyboard_interaction_regressions() {
    // Logical commands from the harness/accessibility path have no physical
    // button. The invalid sentinel must not alias a held physical key slot.
    input::TextEntry logical("mu",{"music"});logical.row=2;logical.col=9;
    constexpr unsigned invalid_button=static_cast<unsigned>(-1);
    logical.event(input::Command::right,{},{},invalid_button);
    logical.event(input::Command::ok,{},{},invalid_button);
    check(logical.value=="music" && !logical.prediction_focused,"source-less logical Confirm accepts a suggestion after navigation");
    check(logical.event(input::Command::ok,40)==input::Outcome::accept,"physical Enter saves after logical suggestion acceptance");

    input::TextEntry shortcuts("ab");shortcuts.row=2;shortcuts.col=4;
    shortcuts.event(input::Command::none,{},{},21);
    check(shortcuts.page==2 && shortcuts.pressed(4,2),"L2 opens accents and presses its displayed key");
    shortcuts.event(input::Command::none,{},{},21);
    check(shortcuts.page==2,"holding L2 does not toggle accents repeatedly");
    shortcuts.event(input::Command::none,{},{},{},21);
    shortcuts.event(input::Command::none,{},{},21);
    check(shortcuts.page==0,"a second L2 press returns to letters");
    shortcuts.event(input::Command::none,{},{},22);
    shortcuts.event(input::Command::none,{},{},22);
    check(shortcuts.caps && shortcuts.pressed(4,3),"R2 changes case once per press and presses its displayed key");
    shortcuts.event(input::Command::none,{},{},9);
    check(shortcuts.caret()==1 && shortcuts.pressed(5,2),"L1 moves the caret left with key feedback");
    shortcuts.event(input::Command::none,{},{},10);
    check(shortcuts.caret()==2 && shortcuts.pressed(5,3),"R1 moves the caret right with key feedback");
    shortcuts.event(input::Command::none,{},{},4);
    check(shortcuts.page==1 && shortcuts.pressed(5,6),"Select changes input mode with key feedback");
    shortcuts.event(input::Command::options,{},{},3);
    check(shortcuts.value=="ab " && shortcuts.pressed(4,6),"controller Triangle inserts space and presses its displayed key");
    check(shortcuts.row==2 && shortcuts.col==4,"controller shortcuts preserve board navigation focus");
    check(shortcuts.event(input::Command::none,{},{},6)==input::Outcome::accept,"controller Start submits the keyboard");
    input::TextEntry remapped("ab");
    remapped.event(input::Command::extra,{},{},9);
    check(remapped.value=="a" && remapped.caret()==1,"remapped shoulder obeys its semantic binding before raw caret shortcut");
    remapped.event(input::Command::extra,{},{},21);
    check(remapped.value.empty() && remapped.page==0,"remapped L2 deletes instead of changing the accent page");

    input::TextEntry typing("");typing.row=2;typing.col=4;
    typing.event(input::Command::none,4);
    check(typing.value=="a" && typing.pressed(2,0),"physical typing presses the matching OSK cell once");
    check(typing.row==2 && typing.col==4,"physical typing keeps controller focus");
    typing.event(input::Command::none,{},4);typing.advance(0.12);
    check(!typing.pressed(2,0),"physical release clears key feedback after the tap flash");
    typing.event(input::Command::none,225);typing.event(input::Command::none,5);
    check(typing.value=="aB" && typing.pressed(3,4),"Shift typing finds the uppercase matching key");
    typing.event(input::Command::none,{},225);typing.event(input::Command::none,{},5);
    typing.event(input::Command::none,57);typing.event(input::Command::none,57);
    check(typing.caps,"physical Caps Lock toggles only on the first keydown");
    typing.event(input::Command::none,{},57);typing.event(input::Command::none,6);
    check(typing.value=="aBC","physical Caps Lock applies to letter input");
    typing.event(input::Command::none,224);typing.event(input::Command::none,7);
    check(typing.value=="aBC","Ctrl shortcuts do not insert printable characters");
    typing.cancel_holds();
    check(!typing.left_shift && !typing.right_shift && !typing.pressed(2,0),"blur cancels modifiers and feedback");

    input::TextEntry held(std::string(200,'x'));
    held.event(input::Command::none,42);
    check(held.value.size()==199 && held.pressed(4,4),"Backspace deletes once and presses the wide key");
    held.event(input::Command::none,42);
    check(held.value.size()==199,"OS repeat keydown cannot duplicate the owned delete repeat");
    held.advance(0.44);check(held.value.size()==199,"held delete respects the initial delay");
    held.advance(0.01);check(held.value.size()==198,"held delete begins at the existing repeat delay");
    held.advance(0.18);check(held.value.size()==197,"early held delete uses the base repeat pace");
    for(int i=0;i<110;++i)held.advance(0.02);
    const auto fast_start=held.value.size();
    for(int i=0;i<50;++i)held.advance(0.02);
    check(fast_start-held.value.size()>8,"held delete accelerates after a sustained hold");
    held.event(input::Command::none,{},42);const auto released=held.value.size();
    held.advance(2);check(held.value.size()==released,"release stops delete repeats");
    held.event(input::Command::none,42);const auto repressed=held.value.size();
    held.advance(0.44);check(held.value.size()==repressed,"a fresh press resets acceleration and delay");
    held.cancel_holds();held.advance(20);check(held.value.size()==repressed,"blur or close resets held deletion");
    held.event(input::Command::none,42);const auto before_stall=held.value.size();
    held.advance(300);check(before_stall-held.value.size()<=8,"a stalled frame has a bounded delete budget");

    input::TextEntry hz30(std::string(200,'x')),hz120(std::string(200,'x'));
    hz30.event(input::Command::extra,22);hz120.event(input::Command::extra,22);
    for(int i=0;i<120;++i)hz30.advance(1.0/30.0);
    for(int i=0;i<480;++i)hz120.advance(1.0/120.0);
    check(hz30.value==hz120.value,"held delete scheduling is frame-rate independent");
    hz30.event(input::Command::none,{},22);const auto mapped_release=hz30.value;
    hz30.advance(2);check(hz30.value==mapped_release,"mapped keyboard deletion stops on its raw key release");
    input::TextEntry controller(std::string(20,'x'));
    controller.event(input::Command::extra,{},{},2);
    controller.event(input::Command::extra,{},{},2);
    check(controller.value.size()==19,"mapped controller delete owns one initial deletion");
    controller.advance(0.63);check(controller.value.size()==17,"mapped controller delete repeats locally");
    controller.event(input::Command::none,{},{},{},2);controller.advance(2);
    check(controller.value.size()==17,"controller release cancels delete hold");
    input::TextEntry confirm_delete("abc");confirm_delete.row=4;confirm_delete.col=4;
    confirm_delete.event(input::Command::ok,{},{},0);confirm_delete.advance(0.45);
    check(confirm_delete.value=="a","holding controller Confirm on Backspace repeats deletion");
    confirm_delete.event(input::Command::right);confirm_delete.advance(2);
    check(confirm_delete.value=="a","leaving the selected Backspace stops its Confirm hold");

    input::TextEntry utf8("A\xE6\x9B\xB2\xF0\x9F\x8E\xB5" "B");
    utf8.move_caret(-1);utf8.event(input::Command::none,42);
    check(utf8.value=="A\xE6\x9B\xB2" "B" && utf8.caret()==4,"held Backspace preserves UTF-8 codepoint boundaries and suffix");
    utf8.event(input::Command::none,{},42);utf8.move_caret(-1);utf8.event(input::Command::none,76);
    check(utf8.value=="AB" && utf8.caret()==1,"Delete removes the whole UTF-8 codepoint after the caret");

    input::TextEntry completion("play mu next",{"Music musical multiplayer playlist Music C:/private/path {secret} %value user@example.com"});
    for(int i=0;i<5;++i)completion.move_caret(-1);
    const auto candidates=completion.candidates();
    check(candidates.size()==3 && candidates[0]=="multiplayer" && candidates[1]=="Music" && candidates[2]=="musical","local completion filters and deduplicates real matching words");
    completion.event(input::Command::none,43);
    check(completion.prediction_focused,"Tab enters candidates without submitting the field");
    completion.event(input::Command::down);completion.event(input::Command::ok,40);
    check(completion.value=="play music next" && completion.caret()==10,"candidate acceptance replaces only the prefix and preserves suffix and case");
    check(!completion.prediction_focused,"accepting a candidate restores OSK focus");
    completion.event(input::Command::none,{},43);
    input::TextEntry inside("musical tail",{"music musicbox"});
    for(int i=0;i<9;++i)inside.move_caret(-1);
    inside.event(input::Command::none,43);inside.event(input::Command::ok);
    check(inside.value=="music tail","completion replaces the token at the caret without duplicating its existing suffix");
    input::TextEntry uppercase("MU",{"music musical"});uppercase.event(input::Command::none,43);uppercase.event(input::Command::ok);
    check(uppercase.value=="MUSIC","completion preserves all-uppercase prefixes");
    input::TextEntry raw_enter("mu",{"music"});raw_enter.event(input::Command::none,43);
    check(raw_enter.event(input::Command::none,40)==input::Outcome::handled && raw_enter.value=="music","unbound physical Enter accepts a focused candidate before submitting");
    check(raw_enter.event(input::Command::none,40)==input::Outcome::handled,"holding Enter after candidate acceptance cannot submit the field");
    raw_enter.event(input::Command::none,{},40);
    check(raw_enter.event(input::Command::none,40)==input::Outcome::accept,"a deliberate second Enter submits after candidate acceptance");
    input::TextEntry save_tab("mu",{"music"});
    check(save_tab.event(input::Command::options,43)==input::Outcome::accept && save_tab.value=="mu","default Tab Save binding wins over raw candidate navigation");
    std::vector<std::string> large_corpus;
    for(unsigned index=0;index<10000;++index) {
        std::string word="mu";auto number=index;
        for(unsigned place=0;place<3;++place){word+=static_cast<char>('a'+number%26);number/=26;}
        large_corpus.push_back(std::move(word));
    }
    input::TextEntry large("mu",large_corpus);
    const auto large_matches=large.candidates();
    check(large_matches.size()==6 && large_matches.front()=="muaaa" && large_matches.back()=="muaaf","large local corpora return a bounded deterministic prefix range");
    large.insert(" music mu");
    check(large.candidates().size()==6 && large.candidates().front()=="muaaa","buffer matches merge with the immutable corpus without changing ranking");
    input::TextEntry empty("",{"Music"});
    check(empty.candidates().empty(),"empty prefixes do not fabricate predictions");
    input::TextEntry private_entry("music mu",{"musical"},false);
    check(private_entry.candidates().empty(),"private contexts disable local completion");
    input::TextEntry unavailable("mu");check(unavailable.candidates().empty(),"missing vocabulary does not advertise pretend candidates");
    input::TextEntry fresh("mu");check(fresh.candidates().empty(),"one editor cannot learn from another editor's typed text");
    input::TextEntry reused("music mu");check(reused.candidates()==std::vector<std::string>{"music"},"words already in the current buffer are usable only in that editor");
    input::TextEntry unsafe("pr",{"C:/private/path /private/place {private} %private PRIVATE_KEY user@private.com"});
    check(unsafe.candidates().empty(),"paths placeholders email and environment names are excluded from local vocabulary");
    input::TextEntry unicode("\xE6\x9B\xB2",{"\xE6\x9B\xB2\xE7\x9B\xAE"});unicode.event(input::Command::none,43);unicode.event(input::Command::ok);
    check(unicode.value=="\xE6\x9B\xB2\xE7\x9B\xAE","non-ASCII completion matches exact UTF-8 prefixes");
}
int main() {
  try {
    keyboard_interaction_regressions();
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
    input::TextEntry forward_delete("ab");
    forward_delete.move_caret(-1);
    forward_delete.event(input::Command::none,76);
    check(forward_delete.value=="a","physical Delete removes the codepoint after the caret");
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
