#include "../../src/app/input_timing.hpp"
#include <iostream>
#include <stdexcept>
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main() try {
    app::ButtonTiming input;
    check(input.press(1,false,0),"initial Back press accepted");
    check(!input.press(1,false,0.2),"held Back cannot pop another menu");
    check(!input.repeat(2,0.18),"face buttons never repeat");
    input.release(1);check(!input.press(1,false,0.22),"Back bounce gap respected");
    input.release(1);check(input.press(1,false,0.30),"deliberate later Back accepted");
    input.release(1);check(input.press(0,false,0.31),"independent Confirm press accepted");
    check(input.press(12,true,1),"direction press accepted");
    check(!input.repeat(1.44,0.18),"direction initial delay respected");
    check(input.repeat(1.46,0.18)==12,"direction repeats after initial delay");
    check(!input.repeat(1.55,0.18),"direction repeat cadence respected");
    input.release(12);check(!input.repeat(3,0.18),"release stops direction repeat");
    input.clear();check(input.press(0,false,4),"disconnect clears held state");
    check(input.press(21,false,4),"virtual left trigger uses the normal button timing path");
    check(!input.press(21,false,5),"held virtual trigger cannot generate repeated presses");
    input.release(21);check(input.press(21,false,6),"released virtual trigger can be pressed again");
    check(!input.trigger(0,16000),"trigger stays released below its press threshold");
    check(input.trigger(0,20000)==true,"trigger crosses the press threshold once");
    check(!input.trigger(0,17000)&&!input.trigger(0,12000),"trigger hold and threshold jitter do not repeat");
    check(input.trigger(0,8000)==false,"trigger releases below the separate release threshold");
    check(input.trigger(1,32767)==true,"right trigger tracks independently");
    input.clear();check(input.trigger(1,32767)==true,"disconnect or blur resets trigger state");
    check(!input.trigger(2,32767),"invalid trigger index is ignored");
    std::cout<<"input_timing_tests: release boundaries, bounce gap and direction cadence passed\n";
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
