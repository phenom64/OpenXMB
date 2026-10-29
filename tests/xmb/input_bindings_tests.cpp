#include "../../src/app/input_bindings.hpp"
#include "../../src/app/keyboard_glyphs.hpp"
#include <iostream>
#include <stdexcept>
void require(bool v,const char* text) {if(!v)throw std::runtime_error(text);}
int main() try {
    using namespace app::input_bindings;std::map<std::string,std::string> saved;
    require(read(saved,false,6)==43,"Tab invokes Options by default");
    require(read(saved,true,6)==3,"Y invokes Options by default");
    assign(saved,"pad.ok",1);require(read(saved,true,4)==1,"confirm can be remapped");
    require(read(saved,true,5)==0,"conflicting default cancel button swapped");
    require(glyph_index(saved,true,4)==5,"confirm prompt follows the effective B/Circle binding");
    assign(saved,"key.options",40);require(read(saved,false,6)==40,"Options binding changes");
    require(read(saved,false,4)==43,"keyboard conflict swapped");
    require(glyph_index(saved,false,4)==6,"keyboard confirm prompt shows Tab after swap");
    saved["key.options"]="not a code";require(read(saved,false,6)==43,"malformed saved binding falls back");
    saved["pad.options"]="100";require(read(saved,true,6)==3,"out of range controller binding rejected");
    assign(saved,"pad.ok",21);require(read(saved,true,4)==21,"L2 Confirm survives assignment and reload validation");
    assign(saved,"pad.extra",22);require(read(saved,true,7)==22,"R2 deletion survives assignment and reload validation");
    require(xbox_artwork(6)=="xbox_start","Start uses authored glossy Start artwork");
    require(xbox_artwork(21)=="xbox_lt"&&xbox_artwork(22)=="xbox_rt","trigger remaps use their authored trigger artwork");
    require(xbox_artwork(4)=="xbox_back"&&xbox_artwork(9)=="xbox_lb"&&xbox_artwork(10)=="xbox_rb","Back and bumper remaps keep the correct physical artwork");
    require(xbox_artwork(7)=="xbox_left_stick_button"&&xbox_artwork(8)=="xbox_right_stick_button","stick clicks use click artwork rather than movement prompts");
    require(xbox_artwork(11)=="xbox_dpad_up"&&xbox_artwork(14)=="xbox_dpad_right","directional hints use matching glossy D-pad art");
    require(xbox_artwork(0).empty()&&xbox_artwork(5).empty(),"face art stays in the existing family and no Guide art is invented");
    using app::keyboard_glyphs::stem;
    require(stem(82)=="uparrow"&&stem(80)=="leftarrow"&&stem(40)=="return"&&stem(41)=="escape", "keyboard controls resolve authored vector keys");
    assign(saved,"key.up",26);require(stem(read(saved,false,2))=="w","keyboard vector follows remapped physical key");
    require(stem(227,true)=="command"&&stem(227,false)=="windows"&&stem(226,true)=="option","platform modifiers use their actual legends");
    require(stem(58).empty()&&stem(44).empty(),"missing keys request a labeled outline fallback, never unrelated artwork");
    std::cout<<"input_bindings_tests: Options defaults, remapping, swaps and validation passed\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
