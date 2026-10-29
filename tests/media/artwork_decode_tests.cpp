#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#undef main

int main(int argc, char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("Artwork fixture directory required");
        if(SDL_Init(0)!=0)throw std::runtime_error(SDL_GetError());
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
