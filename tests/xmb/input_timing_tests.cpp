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
    std::cout<<"input_timing_tests: release boundaries, bounce gap and direction cadence passed\n";
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
