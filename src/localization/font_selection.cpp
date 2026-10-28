#include "font_selection.hpp"
#include "shell_locale.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <array>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
namespace openxmb::localization {
namespace {
std::string environment(const char* key) {
#ifdef _WIN32
    char* raw{};std::size_t size{};
    if(_dupenv_s(&raw,&size,key)!=0)return {};
    std::string value=raw?raw:"";std::free(raw);return value;
#else
    const auto* value=std::getenv(key);return value?value:"";
#endif
}
std::u8string_view sample(std::string_view language) {
    if(language=="hi")return u8"सेटिंग्स भाषा";
    if(language=="ja")return u8"設定日本語";
    if(language=="zh_CN")return u8"设置简体中文";
    if(language=="zh_TW")return u8"設定繁體中文";
    if(language=="ko")return u8"설정한국어";
    if(language=="ru")return u8"Настройки";
    if(language=="pl")return u8"Żółć ustawień";
    if(language=="fr")return u8"Paramètres français";
    if(language=="es")return u8"Configuración español";
    if(language=="de")return u8"Änderungen";
    return u8"British English";
}
std::string effective_language(std::string_view language) {
    if(language!="auto"&&!language.empty())return shell_locale::normalize(language);
#ifdef _WIN32
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if(GetUserDefaultLocaleName(name,LOCALE_NAME_MAX_LENGTH)) {
        std::string ascii;for(const auto c:std::wstring_view(name))ascii.push_back(static_cast<char>(c));
        return shell_locale::normalize(ascii);
    }
#endif
    for(const auto* key:{"LANGUAGE","LC_MESSAGES","LANG"})if(const auto value=environment(key);!value.empty())return shell_locale::normalize(value);
    return "en";
}
}
bool font_covers(const std::filesystem::path& path,std::u8string_view text) {
    std::error_code error;const auto size=std::filesystem::file_size(path,error);
    if(error||size==0||size>128ULL*1024*1024)return false;
    std::ifstream input(path,std::ios::binary);std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    if(!input.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))return false;
    FT_Library library{};if(FT_Init_FreeType(&library))return false;
    FT_Face face{};
    if(FT_New_Memory_Face(library,bytes.data(),static_cast<FT_Long>(bytes.size()),0,&face)) {FT_Done_FreeType(library);return false;}
    bool covered=true;
    for(std::size_t i=0;i<text.size();) {
        auto first=static_cast<unsigned char>(text[i++]);std::uint32_t code=first;
        const unsigned continuation=first<0x80?0:first<0xE0?1:first<0xF0?2:3;
        if(continuation)code&=(1u<<(6-continuation))-1;
        for(unsigned n=0;n<continuation&&i<text.size();++n)code=(code<<6)|(static_cast<unsigned char>(text[i++])&63);
        if(code>=32&&FT_Get_Char_Index(face,code)==0){covered=false;break;}
    }
    FT_Done_Face(face);FT_Done_FreeType(library);return covered;
}
std::filesystem::path select_font(const std::filesystem::path& configured,const std::filesystem::path& assets,std::string_view requested) {
    const auto language=effective_language(requested);const auto required=sample(language);
    if(font_covers(configured,required))return configured;
    std::vector<std::filesystem::path> candidates{assets/"fonts/NotoSans-Regular.ttf",assets/"Play-Regular.ttf"};
#ifdef _WIN32
    const auto windows=environment("WINDIR");const auto dir=std::filesystem::path(windows.empty()?"C:/Windows":windows)/"Fonts";
    if(language=="hi")candidates.insert(candidates.begin(),dir/"Nirmala.ttc");
    else if(language=="ja")candidates.insert(candidates.begin(),dir/"msgothic.ttc");
    else if(language=="ko")candidates.insert(candidates.begin(),dir/"malgun.ttf");
    else if(language=="zh_CN")candidates.insert(candidates.begin(),dir/"msyh.ttc");
    else if(language=="zh_TW")candidates.insert(candidates.begin(),dir/"msjh.ttc");
    candidates.push_back(dir/"segoeui.ttf");candidates.push_back(dir/"arial.ttf");
#elif defined(__APPLE__)
    for(const auto* name:{"ヒラギノ角ゴシック W3.ttc","PingFang.ttc","AppleSDGothicNeo.ttc","Supplemental/Devanagari Sangam MN.ttc","Supplemental/Arial Unicode.ttf"})candidates.emplace_back(std::filesystem::path("/System/Library/Fonts")/name);
#else
    for(const auto* name:{"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc","/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf","/usr/share/fonts/truetype/noto/NotoSansDevanagari-Regular.ttf","/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"})candidates.emplace_back(name);
#endif
    for(const auto& candidate:candidates)if(font_covers(candidate,required))return candidate;
    return configured;
}

std::vector<std::string> font_catalog_texts(const std::filesystem::path& locales,std::string_view requested) {
    auto path=locales/effective_language(requested)/"LC_MESSAGES/shell.mo";
    std::error_code error;const auto size=std::filesystem::file_size(path,error);
    if(error||size<28||size>16*1024*1024)return {};
    std::ifstream input(path,std::ios::binary);std::vector<unsigned char> data(static_cast<std::size_t>(size));
    if(!input.read(reinterpret_cast<char*>(data.data()),data.size()))return {};
    const auto word=[&](std::size_t offset,bool swap) {
        if(offset+4>data.size())throw std::runtime_error("Invalid message catalog");
        std::uint32_t result=0;for(unsigned i=0;i<4;++i)result|=std::uint32_t(data[offset+i])<<(swap?(3-i)*8:i*8);
        return result;
    };
    const auto magic=word(0,false);if(magic!=0x950412de&&magic!=0xde120495)return {};
    const bool swap=magic==0xde120495;const auto count=word(8,swap),table=word(16,swap);
    if(count>100000||std::uint64_t(table)+std::uint64_t(count)*8>data.size())return {};
    std::vector<std::string> result;
    for(std::uint32_t i=1;i<count;++i) {
        const auto length=word(table+i*8,swap),offset=word(table+i*8+4,swap);
        if(length>65536||std::uint64_t(offset)+length>data.size())continue;
        std::string_view forms(reinterpret_cast<const char*>(data.data()+offset),length);
        while(!forms.empty()) {
            const auto end=forms.find('\0');
            result.emplace_back(forms.substr(0,end));
            if(end==std::string_view::npos)break;
            forms.remove_prefix(end+1);
        }
    }
    return result;
}
}
