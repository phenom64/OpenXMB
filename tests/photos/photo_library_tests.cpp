/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <openxmb/photos/photo_library.hpp>
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace photos=openxmb::photos;
void require(bool test,const char* message) { if(!test) throw std::runtime_error(message); }
void fixture(const std::filesystem::path& path) {
    SDL_Surface* image=SDL_CreateRGBSurfaceWithFormat(0,4,2,32,SDL_PIXELFORMAT_RGBA32);
    require(image!=nullptr,"create fixture");
    const std::uint32_t red=SDL_MapRGBA(image->format,255,0,0,255);
    const std::uint32_t blue=SDL_MapRGBA(image->format,0,0,255,255);
    SDL_FillRect(image,nullptr,red); SDL_Rect right{2,0,2,2}; SDL_FillRect(image,&right,blue);
    const int status=IMG_SavePNG(image,path.string().c_str()); SDL_FreeSurface(image);
    require(status==0,"save PNG fixture");
}
void decode_regressions(const std::filesystem::path& root) {
    const auto fixtures=std::filesystem::path(__FILE__).parent_path()/"fixtures";
    const auto png=root/"decode.png";
    fixture(png);
    int failures=0;
    auto check=[&](const char* name, auto test) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch(const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    };
    check("Unicode PNG path",[&] {
        const auto folder=root/std::filesystem::path(u8"\u65c5\u884c-\u00e9-\U0001f4f7");
        std::filesystem::create_directory(folder);
        const auto path=folder/std::filesystem::path(u8"\u5199\u771f.png");
        std::filesystem::copy_file(png,path);
        require(photos::load_bitmap(path).rgba==photos::load_bitmap(png).rgba,"Unicode PNG pixels");
    });
    check("baseline JPEG and EXIF orientation",[&] {
        const auto loaded=photos::load_photo(fixtures/"baseline.jpg");
        require(loaded.image.width==2 && loaded.image.height==4,"EXIF rotated JPEG dimensions");
        require(loaded.metadata.width==4 && loaded.metadata.height==2 && loaded.metadata.orientation==6,"EXIF metadata preserved");
        require(loaded.image.rgba[0]>240 && loaded.image.rgba[2]<15 && loaded.image.rgba[3]==255,"JPEG RGBA red pixel");
        const auto small=photos::load_bitmap(fixtures/"baseline.jpg",2);
        require(small.width==1 && small.height==2,"JPEG bounded oriented preview");
    });
    check("progressive JPEG",[&] {
        const auto image=photos::load_bitmap(fixtures/"progressive.jpeg");
        require(image.width==4 && image.height==2 && image.rgba[0]>240,"progressive JPEG pixels");
    });
    check("JPEG signature overrides extension",[&] {
        for(const auto* name : {"jpeg.png","jpeg.bin","jpeg"}) {
            const auto path=root/name;
            std::filesystem::copy_file(fixtures/"baseline.jpg",path);
            require(photos::load_bitmap(path).height==4,"JPEG recognized by content");
        }
    });
    check("PNG signature overrides JPEG extension",[&] {
        const auto path=root/"png.jpeg";
        std::filesystem::copy_file(png,path);
        require(photos::load_bitmap(path).rgba==photos::load_bitmap(png).rgba,"PNG recognized by content");
    });
    check("Unicode JPEG path",[&] {
        const auto path=root/std::filesystem::path(u8"\u65c5\u884c-\U0001f4f7.jpeg");
        std::filesystem::copy_file(fixtures/"progressive.jpeg",path);
        require(photos::load_bitmap(path).width==4,"Unicode JPEG dimensions");
    });
    check("JPEG pixel limit preserved",[&] {
        std::ifstream input(fixtures/"baseline.jpg",std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(input)),{});
        const auto sof=bytes.find(std::string("\xff\xc0",2));
        require(sof!=std::string::npos && sof+9<bytes.size(),"baseline SOF marker");
        // A valid JPEG header claiming 8192 x 8192 must fail before allocation.
        bytes[sof+5]=bytes[sof+7]=0x20;
        bytes[sof+6]=bytes[sof+8]=0;
        const auto path=root/"oversized.jpg";
        std::ofstream(path,std::ios::binary).write(bytes.data(),bytes.size());
        bool rejected=false;
        try { photos::load_bitmap(path); }
        catch(const std::runtime_error& error) { rejected=std::string(error.what()).find("32 megapixel")!=std::string::npos; }
        require(rejected,"JPEG pixel limit rejected with limit diagnostic");
    });
    check("file byte limit preserved",[&] {
        const auto path=root/"oversized-file.jpg";
        std::ofstream(path,std::ios::binary).put('\0');
        std::filesystem::resize_file(path,128ULL*1024*1024+1);
        bool rejected=false;
        try { photos::load_bitmap(path); }
        catch(const std::runtime_error& error) { rejected=std::string(error.what()).find("128 MiB")!=std::string::npos; }
        require(rejected,"file byte limit rejected with limit diagnostic");
    });
    check("invalid JPEG rejected",[&] {
        const auto path=root/"broken.jpg";
        std::ofstream(path,std::ios::binary).write("\xff\xd8\xff\xe0\x00\x10",6);
        bool rejected=false;
        try { photos::load_bitmap(path); } catch(const std::runtime_error&) { rejected=true; }
        require(rejected,"invalid JPEG rejected");
    });
    require(failures==0,"photo decode regressions failed");
}
int main(int argc,char** argv) {
    const auto root=std::filesystem::temp_directory_path()/("openxmb-photo-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path,ec); } } cleanup{root};
    try {
        if(argc>1 && std::string(argv[1])=="--decode-focused") { decode_regressions(root); return 0; }
        const auto original=root/"A photograph ; $(not-a-command).PNG";
        fixture(original); fixture(root/"b.png"); fixture(root/".hidden.png");
        std::ofstream(root/"notes.txt") << "not an image";
        auto images=photos::gallery(original);
        require(images.size()==2,"gallery excludes hidden and non-images");
        require(images.front()==original,"case insensitive gallery order");
        auto info=photos::metadata(original); require(info.width==4 && info.height==2 && info.bytes>0,"metadata dimensions and size");
        auto image=photos::load_bitmap(original);
        require(image.rgba[0]==255 && image.rgba[2]==0,"original pixel red");
        auto turned=photos::rotate(image,1);
        require(turned.width==2 && turned.height==4,"quarter turn dimensions");
        require(turned.rgba[0]==255 && turned.rgba[(3*2)*4+2]==255,"rotation preserves pixel positions");
        auto returned=photos::rotate(turned,-1); require(returned.rgba==image.rgba,"rotation round trip");
        auto region=photos::centered_crop(4,2,1.0); auto square=photos::crop(image,region);
        require(square.width==2 && square.height==2,"centered square crop");
        require(square.rgba[0]==255 && square.rgba[4+2]==255,"crop includes center pixels");
        bool rejected=false; try { photos::crop(image,{-0.1,0,1,1}); } catch(const std::invalid_argument&) { rejected=true; }
        require(rejected,"out of bounds crop rejected");
        auto reduced=photos::load_bitmap(original,2); require(reduced.width==2 && reduced.height==1,"bounded preview dimensions");
        const auto before=std::filesystem::file_size(original);
        auto copy=photos::save_copy(original,1); auto another=photos::save_copy(original,1);
        require(copy!=another && std::filesystem::exists(copy),"unique saved copies");
        require(std::filesystem::file_size(original)==before && photos::load_bitmap(original).rgba==image.rgba,"original unchanged");
        auto copy_info=photos::metadata(copy); require(copy_info.width==2 && copy_info.height==4,"saved copy rotated at full resolution");
        auto crop_copy=photos::save_copy(original,0,region); require(photos::metadata(crop_copy).width==2,"crop saved copy");
        auto recoverable=photos::move_to_trash(original,root/"trash");
        require(!std::filesystem::exists(original) && std::filesystem::exists(recoverable),"photo moved to recoverable trash");
        require(photos::load_bitmap(recoverable).rgba==image.rgba,"trash content preserved");
        require(std::filesystem::exists(recoverable.parent_path().parent_path()/"original-path.txt"),"trash origin recorded");
        bool invalid=false; try { photos::load_bitmap(root/"notes.txt"); } catch(const std::exception&) { invalid=true; }
        require(invalid,"invalid image reports failure");
        decode_regressions(root);
        std::cout << "Photo tests passed: gallery, metadata, rotation, crop, bounded previews, save-copy, trash and invalid input\n";
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
