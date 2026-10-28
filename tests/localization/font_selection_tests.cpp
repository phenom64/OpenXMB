#include "../../src/localization/font_selection.hpp"
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv)try {
    if(argc!=3)return 2;
    const std::filesystem::path assets=argv[1],configured=assets/"Play-Regular.ttf";
    using namespace openxmb::localization;
    if(!font_covers(configured,u8"British English"))throw std::runtime_error("baseline typeface unavailable");
    if(select_font(configured,assets,"en")!=configured)throw std::runtime_error("English must retain selected font");
    for(const auto& [locale,text]:{std::pair{"hi",u8"सेटिंग्स"},{"ja",u8"設定"},{"zh_CN",u8"设置"},{"zh_TW",u8"設定"},{"ko",u8"설정"},{"ru",u8"Настройки"}}) {
        const auto chosen=select_font(configured,assets,locale);
        if(!font_covers(chosen,text))throw std::runtime_error(std::string("missing script font for ")+locale);
        if(select_font(chosen,assets,locale)!=chosen)throw std::runtime_error("custom capable font must remain selected");
    }
    for(const auto* locale:{"en","de","fr","hi","pl","id","es","ja","zh_CN","zh_TW","ko","ru"}) {
        const auto texts=font_catalog_texts(argv[2],locale);
        if(texts.size()<1000)throw std::runtime_error(std::string("incomplete font preload catalog: ")+locale);
        for(const auto& text:texts)if(text.find('\0')!=std::string::npos)throw std::runtime_error("plural catalog entry was not split");
    }
    if(!font_catalog_texts(std::filesystem::path(argv[2])/"missing","en").empty())throw std::runtime_error("missing catalog must be safe");
    std::cout<<"Script font selection passed; configured preference remains unchanged\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
