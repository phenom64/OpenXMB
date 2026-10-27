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
int main() {
    const auto root=std::filesystem::temp_directory_path()/("openxmb-photo-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path,ec); } } cleanup{root};
    try {
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
        std::cout << "Photo tests passed: gallery, metadata, rotation, crop, bounded previews, save-copy, trash and invalid input\n";
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
