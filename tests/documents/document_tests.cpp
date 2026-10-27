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

#include <openxmb/documents/document.hpp>
#include <openxmb/photos/photo_library.hpp>
#include <archive.h>
#include <archive_entry.h>
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <chrono>
#include <array>
#include <future>
#include <thread>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace docs=openxmb::documents;
void require(bool test,const char* text) { if(!test) throw std::runtime_error(text); }
std::vector<char> bytes(const std::filesystem::path& path) { std::ifstream file(path,std::ios::binary); return {std::istreambuf_iterator<char>(file),{}}; }
void image(const std::filesystem::path& path,bool blue) {
    auto* surface=SDL_CreateRGBSurfaceWithFormat(0,8,6,32,SDL_PIXELFORMAT_RGBA32);
    require(surface,"create image fixture");
    SDL_FillRect(surface,nullptr,SDL_MapRGBA(surface->format,blue?0:255,0,blue?255:0,255));
    const auto result=IMG_SavePNG(surface,path.string().c_str()); SDL_FreeSurface(surface); require(result==0,"write image fixture");
}
void comic(const std::filesystem::path& path,const std::vector<std::pair<std::string,std::vector<char>>>& pages,bool tar=false,bool oversized=false) {
    auto* archive=archive_write_new();
    if(tar) archive_write_set_format_pax_restricted(archive); else archive_write_set_format_zip(archive);
    require(archive_write_open_filename(archive,path.string().c_str())==ARCHIVE_OK,"create comic archive");
    for(const auto& [name,contents]:pages) {
        auto* entry=archive_entry_new(); archive_entry_set_pathname(entry,name.c_str()); archive_entry_set_filetype(entry,AE_IFREG); archive_entry_set_perm(entry,0644);
        archive_entry_set_size(entry,oversized?129LL*1024*1024:static_cast<la_int64_t>(contents.size()));
        require(archive_write_header(archive,entry)==ARCHIVE_OK,"archive page header");
        if(!oversized) require(archive_write_data(archive,contents.data(),contents.size())==static_cast<la_ssize_t>(contents.size()),"archive page data");
        archive_entry_free(entry);
    }
    archive_write_close(archive); archive_write_free(archive);
}
void pdf(const std::filesystem::path& path) {
    std::ofstream output(path,std::ios::binary); output<<"%PDF-1.4\n";
    const std::vector<std::string> streams{"1 0 0 rg 0 0 200 100 re f\n","0 0 1 rg 0 0 200 100 re f\n"};
    const std::vector<std::string> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Resources << >> /Contents 4 0 R >>",
        "<< /Length "+std::to_string(streams[0].size())+" >>\nstream\n"+streams[0]+"endstream",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Resources << >> /Contents 6 0 R >>",
        "<< /Length "+std::to_string(streams[1].size())+" >>\nstream\n"+streams[1]+"endstream"};
    std::vector<std::streamoff> offsets{0};
    for(std::size_t i=0;i<objects.size();++i) { offsets.push_back(output.tellp());output<<i+1<<" 0 obj\n"<<objects[i]<<"\nendobj\n"; }
    const auto xref=output.tellp(); output<<"xref\n0 7\n0000000000 65535 f \n";
    for(std::size_t i=1;i<offsets.size();++i) output<<std::setfill('0')<<std::setw(10)<<offsets[i]<<" 00000 n \n";
    output<<"trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n"<<xref<<"\n%%EOF\n";
}
void set_environment(const char* name,const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name,value.c_str());
#else
    setenv(name,value.c_str(),1);
#endif
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/("openxmb-document-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup {std::filesystem::path p;~Cleanup(){std::error_code ec;std::filesystem::remove_all(p,ec);}} cleanup{root};
    try {
        image(root/"red.png",false);image(root/"blue.png",true);
        auto red=bytes(root/"red.png"),blue=bytes(root/"blue.png");
        for(bool tar:{false,true}) {
            auto path=root/(tar?"pages.cbt":"pages.cbz");
            comic(path,{{"page10.png",blue},{"page2.png",red},{"Readme.txt",{'h','i'}}},tar);
            auto document=docs::open(path);require(document->pages==2,"CBZ/CBT page count");
            require(docs::page_label(*document,0)=="page2.png","natural numeric comic page order");
            auto first=openxmb::photos::load_bitmap(docs::render_page(*document,0));require(first.rgba[0]==255 && first.rgba[2]==0,"first comic page raster");
            auto second=openxmb::photos::load_bitmap(docs::render_page(*document,1));require(second.rgba[0]==0 && second.rgba[2]==255,"second comic page raster");
            bool rejected=false;try{docs::render_page(*document,2);}catch(const std::out_of_range&){rejected=true;}require(rejected,"page range checked");
        }
        comic(root/"traversal.cbz",{{"../../escape.png",red}});
        auto traversal=docs::open(root/"traversal.cbz");auto raster=docs::render_page(*traversal,0);
        require(raster.filename()=="current-page.png" && !std::filesystem::exists(root.parent_path()/"escape.png"),"archive paths never extracted onto filesystem");
        {
            std::array<char,512> header{};
            std::memcpy(header.data(),"page.png",8);
            std::snprintf(header.data()+100,8,"%07o",0644);
            std::snprintf(header.data()+124,12,"%011llo",129ULL*1024*1024);
            std::fill(header.begin()+148,header.begin()+156,' '); header[156]='0';
            std::memcpy(header.data()+257,"ustar",5); std::memcpy(header.data()+263,"00",2);
            unsigned checksum=0;for(unsigned char c:header)checksum+=c;
            std::snprintf(header.data()+148,8,"%06o",checksum);header[155]=' ';
            std::ofstream large(root/"too-large.cbt",std::ios::binary);large.write(header.data(),header.size());
        }
        bool oversized=false;try{docs::open(root/"too-large.cbt");}catch(const std::exception&){oversized=true;}require(oversized,"oversized archive pages rejected");
        std::ofstream(root/"bad.pdf")<<"invalid PDF";
        bool invalid=false;try{docs::open(root/"bad.pdf");}catch(const std::exception&){invalid=true;}require(invalid,"invalid PDF failure surfaced");
        const auto pdf_path=root/"book ; $(touch unwanted).pdf";pdf(pdf_path);
        auto document=docs::open(pdf_path);require(document->pages==2,"real PDF page count through pdfinfo");
        auto first=openxmb::photos::load_bitmap(docs::render_page(*document,0));require(first.width==1600 && first.rgba[0]>240 && first.rgba[2]<10,"PDF first page rendered red");
        auto second=openxmb::photos::load_bitmap(docs::render_page(*document,1));require(second.rgba[0]<10 && second.rgba[2]>240,"PDF second page rendered blue");
        std::stop_source stopped;stopped.request_stop();bool cancelled=false;try{docs::render_page(*document,0,stopped.get_token());}catch(const std::exception&){cancelled=true;}require(cancelled,"cancellation stops page work");
#if !defined(_WIN32)
        const auto tools=root/"tools";std::filesystem::create_directory(tools);
        const std::string previous_path=std::getenv("PATH")?std::getenv("PATH"):"";
        std::ofstream(tools/"lp")<<"#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$OPENXMB_DOCUMENT_PRINT_ARGS\"\n";
        std::filesystem::permissions(tools/"lp",std::filesystem::perms::owner_all);
        set_environment("PATH",tools.string()+":"+previous_path);set_environment("OPENXMB_DOCUMENT_PRINT_ARGS",(root/"print-arguments.txt").string());
        std::ofstream(tools/"pdfinfo")<<"#!/bin/sh\nexec sleep 5\n";
        std::filesystem::permissions(tools/"pdfinfo",std::filesystem::perms::owner_all);
        std::stop_source active_cancel;
        const auto cancel_started=std::chrono::steady_clock::now();
        auto running=std::async(std::launch::async,[&](){
            try { docs::open(pdf_path,active_cancel.get_token()); return false; }
            catch(const std::exception& error) { return std::string(error.what()).find("cancelled")!=std::string::npos; }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));active_cancel.request_stop();
        require(running.get() && std::chrono::steady_clock::now()-cancel_started<std::chrono::seconds(2),"running child process cancellation");
        std::filesystem::remove(tools/"pdfinfo");
        require(docs::printing_available(),"print tool discovery");docs::print(*document,1);
        auto args=bytes(root/"print-arguments.txt");std::string arguments(args.begin(),args.end());
        require(arguments.find("page-ranges=2")!=std::string::npos && arguments.find(pdf_path.string())!=std::string::npos,"printing forwards page and literal pathname as argv");
        std::ofstream(tools/"lp",std::ios::trunc)<<"#!/bin/sh\necho 'printer unavailable' >&2\nexit 2\n";
        bool failed=false;try{docs::print(*document);}catch(const std::exception& error){failed=std::string(error.what()).find("printer unavailable")!=std::string::npos;}require(failed,"printer errors surfaced");
        set_environment("PATH",previous_path);
#endif
        require(!std::filesystem::exists(root/"unwanted"),"document names never interpreted by shell");
        std::cout<<"Document tests passed: PDF raster/pages, CBZ/CBT, ordering, bounded extraction, cancellation, print argv/errors\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
