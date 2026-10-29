#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#undef main

void keyboard_glyphs(const std::filesystem::path& directory) {
    std::size_t count=0;
    for(const auto& item:std::filesystem::directory_iterator(directory)) {
        if(item.path().extension()!=".svg")continue;
        const auto utf8=item.path().u8string();
        std::unique_ptr<SDL_Surface,decltype(&SDL_FreeSurface)> decoded(IMG_Load(reinterpret_cast<const char*>(utf8.c_str())),SDL_FreeSurface);
        if(!decoded)throw std::runtime_error("Keyboard SVG decode failed: "+item.path().filename().string()+": "+IMG_GetError());
        if(decoded->h!=288||decoded->w<288)throw std::runtime_error("Keyboard vector viewport/aspect was changed");
        std::unique_ptr<SDL_Surface,decltype(&SDL_FreeSurface)> rgba(SDL_ConvertSurfaceFormat(decoded.get(),SDL_PIXELFORMAT_RGBA32,0),SDL_FreeSurface);
        if(!rgba)throw std::runtime_error(SDL_GetError());
        std::size_t ink=0,clear=0;
        for(int y=0;y<rgba->h;++y)for(int x=0;x<rgba->w;++x) {
            const auto* pixel=static_cast<const unsigned char*>(rgba->pixels)+y*rgba->pitch+x*4;
            if(pixel[3]) {++ink;if(pixel[0]<250||pixel[1]<250||pixel[2]<250)throw std::runtime_error("Keyboard vector contains nonwhite ink");}
            else ++clear;
        }
        if(!ink||clear<ink)throw std::runtime_error("Keyboard vector lost its transparent outline presentation");
        if(item.path().filename()=="icon-return.svg"&&decoded->w!=533)throw std::runtime_error("Return key was stretched to a square");
        ++count;
    }
    if(count!=65)throw std::runtime_error("Incomplete keyboard vector asset pack");
    std::cout<<"65 native SVG keyboard keys: white ink, transparency, high-DPI viewport and wide-key aspect passed\n";
}
int main(int argc, char** argv) {
    try {
        if(argc!=2&&argc!=3)throw std::runtime_error("Artwork fixture directory or --keyboard <SVG directory> required");
        if(SDL_Init(0)!=0)throw std::runtime_error(SDL_GetError());
        if(argc==3&&std::string_view(argv[1])=="--keyboard") {keyboard_glyphs(argv[2]);IMG_Quit();SDL_Quit();return 0;}
        if(argc!=2)throw std::runtime_error("Unknown decode test mode");
        for(const auto* extension:{"png","jpg","webp","tiff"}) {
            const auto path=std::filesystem::path(argv[1])/(std::string("landscape.")+extension);
            const auto utf8=path.u8string();
            std::unique_ptr<SDL_Surface,decltype(&SDL_FreeSurface)> decoded(
                IMG_Load(reinterpret_cast<const char*>(utf8.c_str())),SDL_FreeSurface);
            if(!decoded)throw std::runtime_error(std::string(extension)+" runtime decoding failed: "+IMG_GetError());
            if(decoded->w!=14 || decoded->h!=8)throw std::runtime_error("Landscape dimensions changed");
            std::unique_ptr<SDL_Surface,decltype(&SDL_FreeSurface)> pixels(
                SDL_ConvertSurfaceFormat(decoded.get(),SDL_PIXELFORMAT_RGBA32,0),SDL_FreeSurface);
            if(!pixels)throw std::runtime_error(SDL_GetError());
            const auto* rgba=static_cast<const unsigned char*>(pixels->pixels);
            if(std::abs(rgba[0]-42)>3 || std::abs(rgba[1]-180)>3 || std::abs(rgba[2]-95)>3 || rgba[3]!=255)
                throw std::runtime_error(std::string(extension)+" decoded incorrect pixel channels");
        }
        IMG_Quit();SDL_Quit();
        std::cout<<"PNG/JPEG/WebP/TIFF runtime artwork decoding passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
