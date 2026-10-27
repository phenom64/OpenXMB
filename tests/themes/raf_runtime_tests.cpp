#include "openxmb/themes/raf_runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string_view>
#include <zlib.h>

namespace {
using namespace openxmb::themes;
using json=nlohmann::json;
using Bytes=std::vector<std::uint8_t>;
void require(bool value,std::string_view message){if(!value){std::cerr<<"RAF runtime: "<<message<<'\n';std::exit(EXIT_FAILURE);}}
void word(Bytes& bytes,std::size_t at,std::uint32_t n,bool big=false){if(bytes.size()<at+4)bytes.resize(at+4);for(unsigned k=0;k<4;++k)bytes[at+k]=static_cast<std::uint8_t>(n>>(big?24-8*k:8*k));}
void half(Bytes& bytes,std::size_t at,std::uint16_t n){if(bytes.size()<at+2)bytes.resize(at+2);bytes[at]=n>>8;bytes[at+1]=n;}
void floating(Bytes& bytes,std::size_t at,float n){word(bytes,at,std::bit_cast<std::uint32_t>(n),true);}
struct Script {
    std::vector<std::array<std::uint32_t,2>> code;
    std::vector<std::string> names,properties,text;
    std::size_t emit(unsigned op,unsigned arg=0){code.push_back({op,arg});return code.size()-1;}
    void number(float value){emit(0x27,std::bit_cast<std::uint32_t>(value));}
    unsigned name(std::string value){auto i=std::find(names.begin(),names.end(),value);if(i==names.end()){names.push_back(value);return names.size()-1;}return i-names.begin();}
    unsigned property(std::string value){auto i=std::find(properties.begin(),properties.end(),value);if(i==properties.end()){properties.push_back(value);return properties.size()-1;}return i-properties.begin();}
    unsigned string(std::string value){text.push_back(value);return text.size()-1;}
    Bytes bytes(){Bytes out(48);out[0]='V';out[1]='S';out[2]='M';out[3]='X';word(out,8,out.size());word(out,12,code.size()*8);for(auto c:code){const auto at=out.size();word(out,at,c[0]);word(out,at+4,c[1]);}
        auto table=[&](unsigned header,const auto& strings){const auto begin=out.size();for(const auto& string:strings){out.insert(out.end(),string.begin(),string.end());out.push_back(0);}word(out,header,begin);word(out,header+4,out.size()-begin);};table(16,text);table(28,properties);table(40,names);return out;}
    void actor(){emit(0x2e,name("a"));emit(0x2e,name("Actor"));emit(0x28,string("sun"));emit(0x3e,1);emit(1);emit(0x22);}
};
json scene(){
    json out;
    out["camera"]={{"position",{0,0,10}},{"direction",{0,0,-1}},{"up",{0,1,0}},{"yfov",.6},{"near",.1},{"far",1000}};
    out["textures"]=json::array({json{{"file","texture.png"}}});
    json mesh={{"positions",{-1,-1,0,1,-1,0,0,1,0}},{"uvs",{0,0,1,0,.5,1}},{"indexes",{0,1,2}}};
    out["models"]=json::array({json{{"geometry",mesh}}});
    out["actors"]=json::array();
    out["actors"].push_back({{"id","sun"},{"model",0},{"texture",0},{"position",{0,0,0}},{"rotation",{0,0,0}},{"scale",{1,1,1}},{"color",{1,1,1,1}}});
    out["lights"]=json::array();return out;
}
std::array<unsigned char,4> first_png_pixel(const std::filesystem::path& path){
    std::ifstream file(path,std::ios::binary);Bytes data{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()},compressed;
    const auto big=[&](std::size_t at){return std::uint32_t(data.at(at))<<24|std::uint32_t(data.at(at+1))<<16|std::uint32_t(data.at(at+2))<<8|data.at(at+3);};
    for(std::size_t at=8;at+12<=data.size();){const auto size=big(at);require(size<=data.size()-at-12,"PNG fixture chunks bounded");
        if(std::string_view(reinterpret_cast<const char*>(data.data()+at+4),4)=="IDAT")compressed.insert(compressed.end(),data.begin()+at+8,data.begin()+at+8+size);at+=size+12;}
    Bytes scanlines(68);uLongf count=scanlines.size();require(uncompress(scanlines.data(),&count,compressed.data(),compressed.size())==Z_OK,"GTF conversion produces decodable PNG pixels");
    return {scanlines[1],scanlines[2],scanlines[3],scanlines[4]};
}
}

int main(){
    const auto directory=std::filesystem::temp_directory_path()/("openxmb-raf-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(directory);std::ofstream(directory/"texture.png")<<"fixture";
    Script glide;glide.actor();glide.emit(0x2e,glide.name("a"));glide.emit(0x30,glide.property("setPosition"));glide.number(2);glide.number(0);glide.number(0);glide.emit(0x49,3);glide.number(1);glide.emit(0x3d,2);glide.emit(0x22);glide.emit(0x45);
    RafRuntime moving(scene(),glide.bytes());for(const auto& warning:moving.warnings())std::cerr<<warning<<'\n';require(moving.warnings().empty(),"ordinary actor script initializes");
    const auto first=moving.sample(0,directory),halfFrame=moving.sample(.5,directory);require(first.draws.size()==1&&first.draws[0].vertices.size()==3,"scene produces textured triangle geometry");
    require(std::abs(moving.actor_state("sun")["position"][0].get<float>()-1)<.04F,"VSMX setter drives native actor interpolation");
    require(halfFrame.draws[0].vertices[0].x>first.draws[0].vertices[0].x,"scripted motion changes projected geometry");
    moving.sample(0,directory);require(std::abs(moving.actor_state("sun")["position"][0].get<float>())<.001F,"seeking back resets theme script state");

    Script timer;timer.actor();timer.emit(0x2e,timer.name("a"));timer.emit(0x2f,timer.property("timer"));timer.emit(0x26,0);timer.emit(0x2e,timer.name("IntervalTimer"));timer.number(1.0F/30);const auto function=timer.emit(0x2a);timer.emit(0x3e,2);timer.emit(0x36);timer.emit(0x22);timer.emit(0x45);
    timer.code[function][1]=timer.code.size();timer.emit(0x2c);timer.emit(0x2f,timer.property("position"));timer.emit(0x4b);timer.number(.1F);timer.emit(2);timer.emit(0x4c);timer.emit(0x22);timer.emit(0x45);
    RafRuntime ticking(scene(),timer.bytes());ticking.sample(.1,directory);require(ticking.warnings().empty(),"per-frame timer runs without script errors");
    require(std::abs(ticking.actor_state("sun")["position"][0].get<float>()-.3F)<.02F,"timer callback receives its owning actor as this");
    json cached={{"kind","raf"},{"scene",scene()},{"script",timer.bytes()}};
    const auto start=sample_raf_animation(cached,directory,100),advanced=sample_raf_animation(cached,directory,100.1);
    require(advanced.draws[0].vertices[0].x>start.draws[0].vertices[0].x,"cached scene advances between frames");
    auto other=cached;other["overlay_enabled"]=false;
    require(sample_raf_animation(other,directory,101).draws.empty(),"RAF overlay toggle filters effect actors");
    const auto restarted=sample_raf_animation(cached,directory,102);
    require(std::abs(restarted.draws[0].vertices[0].x-start.draws[0].vertices[0].x)<.001F,"returning to a theme resets its playback instance");
    auto backdrop=cached;backdrop["scene"]["textures"][0]["width"]=1920;backdrop["scene"]["textures"][0]["height"]=1080;backdrop["scene"]["textures"][0]["solid"]=1;
    backdrop["background_enabled"]=false;
    require(sample_raf_animation(backdrop,directory,103).draws.empty(),"RAF backdrop toggle filters solid widescreen backgrounds");

    auto skinned=scene();skinned["models"][0]["skeleton"]={{"parents",{-1}},{"pose",json::array({json::array({0,0,0,1,0,0,0,0,1,1,1,0})})}};
    json animationData={{"id","travel"},{"duration",1},{"fps",30}};
    animationData["translation"]["0"]=json::array({json::array({0,0,0,0}),json::array({30,2,0,0})});
    skinned["models"][0]["animations"]=json::array({animationData});
    RafRuntime animation(skinned);const auto rest=animation.sample(0,directory),pose=animation.sample(.5,directory);require(pose.draws[0].vertices[0].x>rest.draws[0].vertices[0].x,"skeletal keyframes skin actual native vertices");

    Script infinite;infinite.emit(0x39,0);RafRuntime bounded(scene(),infinite.bytes());require(!bounded.warnings().empty(),"infinite theme scripts hit the instruction budget");require(!bounded.sample(0,directory).draws.empty(),"script failure preserves last valid scene");
    bool rejected=false;try{RafRuntime invalid(scene(),Bytes{'V','S','M','X'});}catch(const ThemeError&){rejected=true;}require(rejected,"truncated scripts rejected before execution");
    auto outside=json{{"kind","raf"},{"scene_file","../outside.json"}};rejected=false;try{validate_raf_animation(outside,directory);}catch(const ThemeError&){rejected=true;}require(rejected,"external scene resource paths rejected");

    Bytes gtf(52+8);word(gtf,16,52,true);word(gtf,20,8,true);gtf[24]=0x86;half(gtf,32,4);half(gtf,34,4);gtf[52]=0;gtf[53]=0xf8;gtf[54]=0xe0;gtf[55]=7;
    RafDocument document;document.files.push_back(gtf);document.elements=json::array({{{"name","file"},{"attributes",{{"id","texture-file"}}}},{{"name","texture"},{"attributes",{{"id","red"},{"type",0},{"fileindex",0}}}}});
    std::vector<std::string> warnings;const auto descriptor=compile_raf_animation(document,directory,{},warnings);require(warnings.empty(),"common DXT1 RAF texture imports without fallback");validate_raf_animation(descriptor,directory);
    std::ifstream textureFile(directory/"animation/texture-0.png",std::ios::binary);std::array<unsigned char,8> signature;textureFile.read(reinterpret_cast<char*>(signature.data()),signature.size());textureFile.close();require(signature==std::array<unsigned char,8>{137,80,78,71,13,10,26,10},"GTF decoder writes a native PNG resource");
    require(first_png_pixel(directory/"animation/texture-0.png")==std::array<unsigned char,4>{255,0,0,255},"DXT1 endpoint decodes red opaque pixels");
    for(unsigned format:{0x87u,0x88u}){
        Bytes block(52+16);word(block,16,52,true);word(block,20,16,true);block[24]=format;half(block,32,4);half(block,34,4);
        if(format==0x87)std::fill(block.begin()+52,block.begin()+60,0x88);else{block[52]=128;block[53]=64;}
        block[60]=0;block[61]=0xf8;block[62]=0xe0;block[63]=7;document.files[0]=block;warnings.clear();
        compile_raf_animation(document,directory,{},warnings);require(warnings.empty(),"DXT3/5 imports without fallback");
        const auto pixel=first_png_pixel(directory/"animation/texture-0.png");require(pixel[0]==255&&pixel[1]==0&&pixel[2]==0&&pixel[3]==(format==0x87?136:128),"DXT3/5 alpha data survives conversion");
    }
    std::filesystem::remove_all(directory);std::cout<<"RAF script, timer, geometry, skinning and resource contracts passed\n";
}
