#include "openxmb/input/prompt.hpp"
#include <iostream>
#include <stdexcept>
using namespace openxmb::input;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main()try{
    const auto english=prompt_parts("Use {up} and {down}. Press {ok}.");
    require(english.size()==7&&english[1].control==PromptControl::up&&english[3].control==PromptControl::down&&
        english[5].control==PromptControl::ok,"logical controls were not separated from prose");
    const auto reordered=prompt_parts("{ok} を押してください。{left}{right}");
    require(reordered.size()==4&&reordered[0].control==PromptControl::ok&&reordered[1].text==" を押してください。"&&
        reordered[2].control==PromptControl::left&&reordered[3].control==PromptControl::right,
        "localized word order or adjacent glyphs were damaged");
    const auto literal=prompt_parts("Unknown {host} and unfinished {up");
    require(literal.size()==1&&literal[0].text=="Unknown {host} and unfinished {up","unknown tokens must remain readable literal text");
    require(prompt_parts("").empty(),"empty instructions must stay empty");
    std::cout<<"Localized inline prompt controls and literal text passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
