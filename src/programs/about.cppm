module;
#include "../localization/shell_locale.hpp"
#include <SDL2/SDL.h>
#undef main
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
export module openxmb.app:about;
import auroreengine;
import glm;
import vulkan_hpp;
import openxmb.config;
import :component;
import :main;
import :xmb_presentation;
import :message_overlay;

namespace programs {
using namespace app;
namespace {
std::string decimal(double value,int precision=1) {std::ostringstream out;out<<std::fixed<<std::setprecision(precision)<<value;return out.str();}
std::vector<std::pair<std::string,std::string>> hardware(shell* shell) {
    std::vector<std::pair<std::string,std::string>> rows;
#ifdef _WIN32
    HKEY processor{};std::array<char,512> name{};DWORD size=name.size();DWORD mhz=0,number=sizeof(mhz);
    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",0,KEY_READ,&processor)==ERROR_SUCCESS) {
        RegQueryValueExA(processor,"ProcessorNameString",nullptr,nullptr,reinterpret_cast<BYTE*>(name.data()),&size);
        RegQueryValueExA(processor,"~MHz",nullptr,nullptr,reinterpret_cast<BYTE*>(&mhz),&number);RegCloseKey(processor);
    }
    if(name[0]) rows.emplace_back("Processor",name.data());
    DWORD bytes=0;GetLogicalProcessorInformationEx(RelationProcessorCore,nullptr,&bytes);
    std::vector<std::uint8_t> information(bytes);unsigned cores=0;
    if(bytes&&GetLogicalProcessorInformationEx(RelationProcessorCore,reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(information.data()),&bytes))
        for(std::size_t offset=0;offset<bytes;) {auto* entry=reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(information.data()+offset);if(!entry->Size)break;++cores;offset+=entry->Size;}
    rows.emplace_back("CPU Cores / Threads",std::to_string(cores)+" cores / "+std::to_string(SDL_GetCPUCount())+" threads");
    if(mhz) rows.emplace_back("Reported CPU Clock",decimal(mhz/1000.0)+" GHz");
    MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);if(GlobalMemoryStatusEx(&memory)) rows.emplace_back("Installed Memory",decimal(memory.ullTotalPhys/1073741824.0)+" GiB");
    // SMBIOS Type 17 contains the configured DIMM transfer rate, when firmware exposes it.
    const DWORD table_id=0x52534d42;const auto table_bytes=GetSystemFirmwareTable(table_id,0,nullptr,0);
    if(table_bytes>8&&table_bytes<16*1024*1024) {
        std::vector<std::uint8_t> table(table_bytes);GetSystemFirmwareTable(table_id,0,table.data(),table_bytes);
        unsigned speed=0;for(std::size_t p=8;p+4<table.size();) {const auto type=table[p],length=table[p+1];if(length<4||p+length>table.size())break;if(type==17&&length>=0x22) {const auto rate=static_cast<unsigned>(table[p+0x20])|(static_cast<unsigned>(table[p+0x21])<<8);if(rate&&rate!=65535)speed=std::max(speed,rate);}p+=length;while(p+1<table.size()&&(table[p]||table[p+1]))++p;p+=2;if(type==127)break;}
        if(speed) rows.emplace_back("Memory Transfer Rate",std::to_string(speed)+" MT/s");
    }
    rows.emplace_back("Operating System","Windows ("+std::string(SDL_GetPlatform())+")");
#else
    rows.emplace_back("CPU Threads",std::to_string(SDL_GetCPUCount()));
    rows.emplace_back("Installed Memory",decimal(SDL_GetSystemRAM()/1024.0)+" GiB");
    rows.emplace_back("Operating System",SDL_GetPlatform());
    std::ifstream cpu("/proc/cpuinfo");std::string line;while(std::getline(cpu,line))if(line.starts_with("model name")) {rows.emplace_back("Processor",line.substr(line.find(':')+2));break;}
#endif
    const auto gpu=shell->get_window()->physicalDevice.getProperties();
    rows.emplace_back("Graphics Processor",gpu.deviceName.data());
    const auto heaps=shell->get_window()->physicalDevice.getMemoryProperties();std::uint64_t local=0;
    for(unsigned i=0;i<heaps.memoryHeapCount;++i) if(heaps.memoryHeaps[i].flags&vk::MemoryHeapFlagBits::eDeviceLocal) local+=heaps.memoryHeaps[i].size;
    rows.emplace_back(gpu.deviceType==vk::PhysicalDeviceType::eDiscreteGpu?"Dedicated Graphics Memory":"GPU Local Memory",decimal(local/1073741824.0)+" GiB");
    rows.emplace_back("Graphics Driver",std::to_string(gpu.driverVersion));
    rows.emplace_back("Vulkan",std::to_string((gpu.apiVersion>>22))+"."+std::to_string(((gpu.apiVersion>>12)&1023))+"."+std::to_string((gpu.apiVersion&4095)));
    return rows;
}
std::string license_text() {
    std::string result;const auto root=config::CONFIG.asset_directory/"licenses";std::error_code error;
    if(std::filesystem::exists(root,error)) for(const auto& entry:std::filesystem::recursive_directory_iterator(root,error)) {
        if(!entry.is_regular_file()||entry.file_size()>2*1024*1024)continue;
        std::ifstream file(entry.path());result+="\n\n"+entry.path().filename().string()+"\n\n";result.append(std::istreambuf_iterator<char>(file),{});
    }
    return result.empty()?"The GNU General Public License version 3 or later applies to OpenXMB. License files could not be found in this installation.":result;
}
}
export class licenses_screen : public message_overlay {
public:
    explicit licenses_screen(auroreengine::resource_loader&) : message_overlay("Licenses",license_text()) {}
};
export class about_screen final : public component,public action_receiver {
public:
    explicit about_screen(auroreengine::resource_loader& loader) {icons_.load(loader,"branding/syndromatic");icons_.load(loader,"branding/openxmb");}
    float typography_scale() const override {return 2.5F;}
    bool music_background() const override {return true;}
    result tick(shell* shell) override {shell_=shell;if(rows_.empty()) rows_=hardware(shell);return result::success;}
    result on_action(action a) override {
        if(a==action::cancel)return result::close|result::back_sound;
        if(a==action::options&&shell_) {shell_->open_panel("licenses");return result::success;}
        if(a==action::down||a==action::right||a==action::ok) page_=1;
        if(a==action::up||a==action::left) page_=0;
        return result::success;
    }
    void render(auroreengine::gui_renderer& renderer,shell* shell) override {
        xmb_canvas c(renderer);
        if(!page_) {
            icons_.box(c,"branding/openxmb",610,210,700,190);
            c.text("version " OPENXMB_VERSION_STRING,960,410,29,{1,1,1,1},0,true);
            c.text("The Syndromatic Open Xross Media Bar shell experience.",960,481,28,{1,1,1,1},0,true);
            c.text("Designed by Kavish Krishnakumar in Manchester.",960,524,27,{1,1,1,1},0,true);
            c.text("This program is licensed to you under the GNU General Public License version 3.0.",960,606,25,{1,1,1,1},0,true);
            c.text("For more information, see the Licenses section.",960,646,25,{1,1,1,1},0,true);
            icons_.draw(c,"branding/syndromatic",960,817,104);
            const auto footer=::openxmb::localization::shell_locale::translate("& © 2025-2026. Syndromatic Limited Britannia. All rights reserved.");
            const auto footer_width=renderer.text_width_pixels(footer,24*c.scale)/c.scale;
            c.text("TM",960-footer_width*0.5F-22,919,11,{1,1,1,1});
            c.text(footer,960,929,24,{1,1,1,1},0,true);
        } else {
            c.rect(220,180,1480,680,{0,0,0,0.38F});
            icons_.box(c,"branding/openxmb",740,45,440,120);
            c.text("System Information",960,240,32,{1,1,1,1},0,true);
            float y=315;for(const auto& [label,value]:rows_) {c.text(::openxmb::localization::shell_locale::translate(label),290,y,24,{1,1,1,0.75F});c.text(value,760,y,24);y+=46;}
        }
        const auto previous_scale=renderer.text_scale;renderer.text_scale=1.0F;
        shell->render_controller_buttons(renderer,0.5F,0.94F,std::array{std::pair{page_?action::up:action::down,page_?"About":"System Information"},std::pair{action::options,"Licenses"},std::pair{action::cancel,"Back"}});
        renderer.text_scale=previous_scale;
    }
private:
    media_icons icons_;shell* shell_{};unsigned page_{};
    std::vector<std::pair<std::string,std::string>> rows_;
};
}
