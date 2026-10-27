#include "openxmb/themes/raf_runtime.hpp"
#include "vsmx_vm.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <span>
#include <unordered_map>
#include <zlib.h>

namespace openxmb::themes {
namespace {
using json=nlohmann::json;
using Bytes=std::vector<std::uint8_t>;
using View=std::span<const std::uint8_t>;
namespace fs=std::filesystem;
constexpr std::size_t maxVertices=200000,maxIndexes=600000,maxActors=512,maxJoints=512;

View bytes(View data,std::size_t at,std::size_t count) {
    if(at>data.size()||count>data.size()-at)throw ThemeError("Truncated RAF binary resource");return data.subspan(at,count);
}
unsigned u8(View data,std::size_t at) {return bytes(data,at,1)[0];}
std::uint16_t u16(View data,std::size_t at,bool big=true) {
    auto b=bytes(data,at,2);return big?std::uint16_t((b[0]<<8)|b[1]):std::uint16_t(b[0]|(b[1]<<8));
}
std::uint32_t u32(View data,std::size_t at,bool big=true) {
    auto b=bytes(data,at,4);return big?(std::uint32_t(b[0])<<24)|(std::uint32_t(b[1])<<16)|(std::uint32_t(b[2])<<8)|b[3]
        :std::uint32_t(b[0])|(std::uint32_t(b[1])<<8)|(std::uint32_t(b[2])<<16)|(std::uint32_t(b[3])<<24);
}
float f32(View data,std::size_t at) {
    const float value=std::bit_cast<float>(u32(data,at));if(!std::isfinite(value))throw ThemeError("Nonfinite RAF float");return value;
}
float half(std::uint16_t bits) {
    const unsigned sign=bits>>15,exponent=(bits>>10)&31,mantissa=bits&1023;
    if(exponent==31)throw ThemeError("Nonfinite RAF half float");
    const float value=exponent?std::ldexp(1.0F+mantissa/1024.0F,int(exponent)-15):std::ldexp(float(mantissa),-24);
    return sign?-value:value;
}
struct Bits {
    View data;std::size_t position{};
    std::uint64_t read(unsigned count) {
        if(count>63||position>data.size()*8||count>data.size()*8-position)throw ThemeError("Truncated RAF bit stream");
        std::uint64_t out=0;for(unsigned i=0;i<count;++i,++position)out=(out<<1)|((data[position/8]>>(7-position%8))&1);return out;
    }
};
void save(const fs::path& file,View data) {
    fs::create_directories(file.parent_path());std::ofstream stream(file,std::ios::binary);
    stream.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));if(!stream)throw ThemeError("Could not save RAF resource");
}
Bytes load(const fs::path& file,std::uint64_t maximum) {
    if(!fs::is_regular_file(file)||fs::file_size(file)>maximum)throw ThemeError("RAF resource is missing or exceeds its budget");
    std::ifstream stream(file,std::ios::binary);Bytes out(static_cast<std::size_t>(fs::file_size(file)));
    stream.read(reinterpret_cast<char*>(out.data()),static_cast<std::streamsize>(out.size()));if(!stream)throw ThemeError("Could not read RAF resource");return out;
}
fs::path resource(const fs::path& directory,const json& name) {
    if(!name.is_string())throw ThemeError("RAF resource path must be a string");
    const fs::path path=name.get<std::string>();if(path.empty()||path.is_absolute())throw ThemeError("RAF resource path must be relative");
    const auto root=fs::weakly_canonical(directory),result=fs::weakly_canonical(root/path);
    const auto relative=result.lexically_relative(root);
    if(relative.empty()||relative.is_absolute()||*relative.begin()=="..")throw ThemeError("RAF resource escapes its theme directory");return result;
}
void be32(Bytes& out,std::uint32_t n) {for(int i=3;i>=0;--i)out.push_back(static_cast<std::uint8_t>(n>>(i*8)));}
Bytes png(unsigned width,unsigned height,View rgba) {
    Bytes out{137,80,78,71,13,10,26,10};
    auto chunk=[&](std::string_view type,View payload) {
        be32(out,static_cast<std::uint32_t>(payload.size()));auto crc=crc32(0,reinterpret_cast<const Bytef*>(type.data()),4);
        crc=crc32(crc,payload.data(),static_cast<uInt>(payload.size()));out.insert(out.end(),type.begin(),type.end());out.insert(out.end(),payload.begin(),payload.end());be32(out,crc);
    };
    Bytes header;be32(header,width);be32(header,height);header.insert(header.end(),{8,6,0,0,0});chunk("IHDR",header);
    Bytes filtered;filtered.reserve(rgba.size()+height);
    for(unsigned y=0;y<height;++y){filtered.push_back(0);auto row=bytes(rgba,std::size_t(y)*width*4,width*4);filtered.insert(filtered.end(),row.begin(),row.end());}
    uLongf count=compressBound(filtered.size());Bytes compressed(count);
    if(compress2(compressed.data(),&count,filtered.data(),filtered.size(),6)!=Z_OK)throw ThemeError("Could not encode RAF texture");
    compressed.resize(count);chunk("IDAT",compressed);chunk("IEND",{});return out;
}
struct Texture {Bytes rgba;unsigned width,height;};
Texture texture(View gtf,const ImportLimits& limits) {
    const auto offset=u32(gtf,16),size=u32(gtf,20),format=u8(gtf,24),base=format&~0x60u;
    const unsigned w=u16(gtf,32),h=u16(gtf,34),pitch=u32(gtf,40);
    if(!w||!h||w>limits.image_dimension||h>limits.image_dimension||u8(gtf,27)!=0)throw ThemeError("Invalid RAF texture dimensions or cube map");
    auto data=bytes(gtf,offset,size);Texture out{Bytes(std::size_t(w)*h*4),w,h};
    if(base==0x85) {
        auto morton=[](unsigned x,unsigned y,unsigned width,unsigned height){std::size_t v=0;unsigned shift=0;
            for(unsigned bit=1;bit<std::max(width,height);bit<<=1){if(bit<width)v|=std::size_t((x&bit)!=0)<<shift++;if(bit<height)v|=std::size_t((y&bit)!=0)<<shift++;}return v;};
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const auto at=format&0x20?std::size_t(y)*(pitch?pitch:w*4)+x*4:morton(x,y,w,h)*4;
            auto c=bytes(data,at,4);auto target=out.rgba.data()+(std::size_t(y)*w+x)*4;target[0]=c[1];target[1]=c[2];target[2]=c[3];target[3]=c[0];}
        return out;
    }
    if(base<0x86||base>0x88)throw ThemeError("Unsupported RAF GTF texture format "+std::to_string(base));
    const unsigned block=base==0x86?8:16,bw=(w+3)/4,bh=(h+3)/4,row=(format&0x20)&&pitch?pitch:bw*block;
    auto color565=[](unsigned v){return std::array<unsigned,4>{((v>>11)&31)*255/31,((v>>5)&63)*255/63,(v&31)*255/31,255};};
    for(unsigned by=0;by<bh;++by)for(unsigned bx=0;bx<bw;++bx) {
        auto b=bytes(data,std::size_t(by)*row+bx*block,block),c=b.subspan(block==16?8:0);
        const auto c0=u16(c,0,false),c1=u16(c,2,false);std::array<std::array<unsigned,4>,4> palette{};
        palette[0]=color565(c0);palette[1]=color565(c1);
        for(unsigned k=0;k<3;++k){palette[2][k]=c0>c1||block==16?(2*palette[0][k]+palette[1][k])/3:(palette[0][k]+palette[1][k])/2;palette[3][k]=c0>c1||block==16?(palette[0][k]+2*palette[1][k])/3:0;}
        palette[2][3]=255;palette[3][3]=c0>c1||block==16?255:0;
        std::array<unsigned,16> alpha{};alpha.fill(255);
        if(base==0x87)for(unsigned i=0;i<16;++i)alpha[i]=((b[i/2]>>(4*(i%2)))&15)*17;
        if(base==0x88) {
            std::array<unsigned,8> a{b[0],b[1],0,0,0,0,0,0};
            if(a[0]>a[1])for(unsigned i=1;i<7;++i)a[i+1]=((7-i)*a[0]+i*a[1])/7;
            else{for(unsigned i=1;i<5;++i)a[i+1]=((5-i)*a[0]+i*a[1])/5;a[6]=0;a[7]=255;}
            std::uint64_t bits=0;for(unsigned i=0;i<6;++i)bits|=std::uint64_t(b[2+i])<<(i*8);
            for(unsigned i=0;i<16;++i)alpha[i]=a[(bits>>(3*i))&7];
        }
        const auto bits=u32(c,4,false);
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)if(bx*4+x<w&&by*4+y<h){const unsigned i=y*4+x;auto color=palette[(bits>>(2*i))&3];
            auto dst=out.rgba.data()+(std::size_t(by*4+y)*w+bx*4+x)*4;for(unsigned k=0;k<3;++k)dst[k]=color[k];dst[3]=block==16?alpha[i]:color[3];}
    }
    return out;
}

std::vector<std::uint32_t> indexes(View data,std::size_t count) {
    if(count>maxIndexes||count%3)throw ThemeError("Invalid RAF triangle count");
    const auto bias=u16(data,2),seqBytes=u16(data,4);
    const auto deltaBits=u8(data,6);const auto triBytes=(count/3*2+7)/8;
    Bits seq{bytes(data,8,seqBytes)},tri{bytes(data,8+seqBytes,triBytes)},deltas{bytes(data,8+seqBytes+triBytes,data.size()-std::min(data.size(),8+seqBytes+triBytes))};
    std::vector<std::int64_t> references;std::vector<std::uint32_t> out;out.reserve(count);std::int64_t next=0;
    auto get=[&](){if(!seq.read(1))return next++;const auto v=static_cast<std::int64_t>(deltas.read(deltaBits))+(references.size()>=8?references[references.size()-8]:0)-bias;references.push_back(v);if(v<0||v>maxVertices)throw ThemeError("RAF index outside vertex budget");return v;};
    std::array<std::uint32_t,3> previous{};
    for(std::size_t t=0;t<count/3;++t){const auto code=tri.read(2);std::array<std::uint32_t,3> row;
        if(t&&code<3){constexpr unsigned edges[3][2]={{0,2},{2,1},{1,0}};row={previous[edges[code][0]],previous[edges[code][1]],static_cast<std::uint32_t>(get())};}
        else row={static_cast<std::uint32_t>(get()),static_cast<std::uint32_t>(get()),static_cast<std::uint32_t>(get())};
        out.insert(out.end(),row.begin(),row.end());previous=row;
    }return out;
}
json attribute(View data,std::size_t base,unsigned stride,unsigned offset,unsigned format,unsigned components,std::size_t count) {
    if(components>4||!components)throw ThemeError("Invalid RAF vertex attribute width");json out=json::array();
    for(std::size_t v=0;v<count;++v){const auto at=base+v*stride+offset;
        if(format==6){const auto word=u32(data,at);auto signedPart=[](unsigned x,unsigned n){const int v=x&(1u<<(n-1))?int(x)-int(1u<<n):int(x);return float(v)/float((1u<<(n-1))-1);};out.push_back(signedPart(word&2047,11));out.push_back(signedPart((word>>11)&2047,11));out.push_back(signedPart(word>>22,10));}
        else for(unsigned k=0;k<components;++k){float value;if(format==2)value=f32(data,at+4*k);else if(format==3)value=half(u16(data,at+2*k));else if(format==4)value=u8(data,at+k)/255.0F;else throw ThemeError("Unsupported RAF vertex attribute format");out.push_back(value);}
    }return out;
}
json geometry(View data) {
    const auto count=u32(data,0),offset=u32(data,4);if(count>1024)throw ThemeError("RAF segment count exceeds budget");
    json out={{"positions",json::array()},{"normals",json::array()},{"uvs",json::array()},{"colors",json::array()},
              {"joints",json::array()},{"weights",json::array()},{"indexes",json::array()}};
    bool hasNormals=true;
    for(unsigned segment=0;segment<count;++segment){auto s=bytes(data,offset+segment*0x80,0x80);
        if(u8(s,2)!=0)throw ThemeError("Unsupported RAF compressed vertex positions");
        const auto vertices=u16(s,8),indexCount=u16(s,10);const auto base=out["positions"].size()/3;
        if(base+vertices>maxVertices||out["indexes"].size()+indexCount>maxIndexes)throw ThemeError("RAF mesh exceeds budget");
        const auto positionOffset=u32(s,0x18),normalOffset=u32(s,0x1c),rsxOffset=u32(s,0x2c),spuDesc=u32(s,0x6c),rsxDesc=u32(s,0x74);
        json uv=json::array(),normals=json::array(),colors=json::array();
        auto attributes=[&](unsigned desc,unsigned buffer,bool normalStream){if(!desc||!buffer)return;const auto n=u8(data,desc),stride=u8(data,desc+1);if(n>32)throw ThemeError("RAF vertex declaration exceeds budget");
            for(unsigned i=0;i<n;++i){const auto at=desc+8+i*8;const auto id=u8(data,at+3),components=u8(data,at+2),format=u8(data,at+1),start=u8(data,at);
                if(normalStream&&id==2)normals=attribute(data,buffer,stride,start,format,components,vertices);
                else if(!normalStream&&id==5&&uv.empty())uv=attribute(data,buffer,stride,start,format,components,vertices);
                else if(!normalStream&&id==9&&components==4)colors=attribute(data,buffer,stride,start,format,components,vertices);}};
        attributes(spuDesc,normalOffset,true);attributes(rsxDesc,rsxOffset,false);
        if(normals.size()!=std::size_t(vertices)*3)hasNormals=false;
        const auto skinSize=u16(s,0x3c);const auto skinOffset=u32(s,0x40),per=std::max(2u,(std::min<unsigned>(skinSize/std::max(1u,unsigned(vertices)),8)&~1u));
        const unsigned first0=u16(s,0x34)/48,first1=u16(s,0x36)/48,count0=u16(s,0x38)/48;
        for(unsigned i=0;i<vertices;++i){for(unsigned k=0;k<3;++k){out["positions"].push_back(f32(data,positionOffset+(std::size_t(i)*3+k)*4));out["normals"].push_back(normals.size()>i*3+k?normals[i*3+k]:json(k==2?1:0));}
            for(unsigned k=0;k<2;++k)out["uvs"].push_back(uv.size()>i*2+k?uv[i*2+k]:json(0));
            for(unsigned k=0;k<4;++k){out["colors"].push_back(colors.size()>i*4+k?colors[i*4+k]:json(1));unsigned joint=0;float weight=k==0?1:0;
                if((u8(s,6)&15)&&skinOffset&&skinSize){weight=k<per/2?u8(data,skinOffset+i*per+2*k)/255.0F:0;const auto local=k<per/2?u8(data,skinOffset+i*per+2*k+1):0;joint=local<count0?first0+local:first1+local-count0;}
                if(joint>=maxJoints)throw ThemeError("RAF skin joint exceeds budget");out["joints"].push_back(joint);out["weights"].push_back(weight);}}
        auto decoded=indexes(bytes(data,u32(s,0x10),u16(s,0x14)),indexCount);
        for(auto index:decoded){if(index>=vertices)throw ThemeError("RAF triangle references missing vertex");out["indexes"].push_back(base+index);}
    }if(!hasNormals)out["normals"]=json::array();return out;
}
json skeleton(View data) {
    if(std::memcmp(bytes(data,0,4).data(),"ES01",4))throw ThemeError("Unsupported RAF skeleton");
    const unsigned count=u16(data,12),base=0x14+u32(data,0x14);if(count>maxJoints)throw ThemeError("RAF skeleton exceeds budget");
    json parents=json::array(),pose=json::array();for(unsigned j=0;j<count;++j){parents.push_back(-1);json p=json::array();for(unsigned k=0;k<12;++k)p.push_back(f32(data,base+48*j+4*k));pose.push_back(std::move(p));}
    for(unsigned at=0x30;at+4<=base;at+=4){auto j=u16(data,at),p=u16(data,at+2);if(p!=0xffff&&j<count&&(p&0x7fff)<count)parents[j]=p&0x7fff;}
    return {{"parents",parents},{"pose",pose}};
}

struct Packing {unsigned sign,exponent,mantissa;};
std::array<Packing,3> packing(std::uint32_t word) {
    std::array<Packing,3> out;for(unsigned c=0;c<3;++c){const auto v=(word>>(32-10*(c+1)))&1023;out[c]={(v>>9)&1,(v>>5)&15,v&31};}return out;
}
unsigned packed_bits(std::uint32_t spec){unsigned out=0;for(auto p:packing(spec))out+=p.sign+p.exponent+p.mantissa;return out;}
json packed_key(Bits& bits,std::uint32_t spec,bool rotation) {
    json out=json::array();float length=0;
    for(auto p:packing(spec)){double value;
        if(!p.exponent){auto raw=static_cast<std::int64_t>(bits.read(p.sign+p.mantissa));if(p.sign&&raw>=std::int64_t(1)<<(p.sign+p.mantissa-1))raw-=std::int64_t(1)<<(p.sign+p.mantissa);
            value=p.sign?std::ldexp(double(raw),-int(p.mantissa)):double(raw)/std::max(1.0,std::ldexp(1.0,p.mantissa)-1);}
        else{const auto sign=bits.read(p.sign),exponent=bits.read(p.exponent),mantissa=bits.read(p.mantissa);value=std::ldexp(1.0+double(mantissa)/std::ldexp(1.0,p.mantissa),int(exponent)-int((1u<<(p.exponent-1))-1));if(sign)value=-value;}
        if(!std::isfinite(value))throw ThemeError("Nonfinite RAF animation key");out.push_back(value);length+=float(value*value);}
    if(rotation)out.insert(out.begin()+(spec&3),std::sqrt(std::max(0.0F,1-length)));return out;
}
json animation(View data) {
    if(std::memcmp(bytes(data,0,4).data(),"EA03",4))throw ThemeError("Unsupported RAF animation");
    const auto duration=f32(data,4),fps=f32(data,8);const auto sets=u16(data,18);std::array<unsigned,8> counts;
    for(unsigned i=0;i<8;++i)counts[i]=u16(data,0x16+2*i);
    if(counts[7]||sets>4096)throw ThemeError("Unsupported RAF user channels or animation size");
    auto relative=[&](unsigned at){return at+u32(data,at);};auto pad=[](unsigned n){return (n+3)/4*4;};
    const auto dma=relative(0x34),info=relative(0x38),spec=relative(0x4c);const std::array<unsigned,3> constants{relative(0x3c),relative(0x40),relative(0x44)};
    std::array<std::vector<unsigned>,7> lists;unsigned at=0x60;
    for(unsigned i=0;i<4;++i){if(counts[i]>maxJoints)throw ThemeError("RAF animation joint list exceeds budget");for(unsigned j=0;j<counts[i];++j)lists[i].push_back(u16(data,at+j*2));at+=2*pad(counts[i]);}
    const auto animatedPad=pad(counts[4])+pad(counts[5])+pad(counts[6]);at=std::max(at,constants[0]>=2*animatedPad?constants[0]-2*animatedPad:0);
    for(unsigned i=4;i<7;++i){if(counts[i]>maxJoints)throw ThemeError("RAF animation joint list exceeds budget");for(unsigned j=0;j<counts[i];++j)lists[i].push_back(u16(data,at+j*2));at+=2*pad(counts[i]);}
    constexpr const char* names[3]={"rotation","translation","scale"};json out={{"duration",duration},{"fps",fps},{"frames",u16(data,16)}};
    std::array<std::vector<std::uint32_t>,3> specs;at=spec;
    for(unsigned g=0;g<3;++g){out[names[g]]=json::object();const auto constantSpec=u32(data,at);at+=4;
        for(unsigned i=0;i<counts[4+g];++i){specs[g].push_back(u32(data,at));at+=4;out[names[g]][std::to_string(lists[4+g][i])]=json::array();}
        Bits bits{bytes(data,constants[g],(lists[g].size()*packed_bits(constantSpec)+7)/8)};
        for(auto joint:lists[g]){auto key=packed_key(bits,constantSpec,g==0);key.insert(key.begin(),0);out[names[g]][std::to_string(joint)]=json::array({key});}}
    for(unsigned set=0;set<sets;++set){const unsigned size=u32(data,dma+8*set),offset=u32(data,dma+8*set+4),start=u16(data,info+4*set),intraCount=u16(data,info+4*set+2);
        if(!size && offset+16>data.size())continue;std::array<unsigned,8> h;for(unsigned k=0;k<8;++k)h[k]=u16(data,offset+2*k);unsigned p=offset+16;
        for(unsigned g=0;g<3;++g){unsigned nbits=0;for(auto s:specs[g])nbits+=packed_bits(s);const auto count=g<2?h[g]:(nbits+7)/8;Bits bits{bytes(data,p,count)};
            for(unsigned i=0;i<specs[g].size();++i){auto key=packed_key(bits,specs[g][i],g==0);key.insert(key.begin(),start);out[names[g]][std::to_string(lists[4+g][i])].push_back(key);}p+=count;}
        if(!size)continue;
        const auto maskAt=offset+16+h[0]+h[1]+h[2];std::size_t channels=specs[0].size()+specs[1].size()+specs[2].size();if(channels*intraCount>8*1024*1024)throw ThemeError("RAF key mask exceeds budget");
        Bits masks{bytes(data,maskAt,(channels*intraCount+7)/8)};std::array<std::vector<std::vector<unsigned>>,3> active;std::array<unsigned,3> need{};
        for(unsigned g=0;g<3;++g)for(auto spec:specs[g]){std::vector<unsigned> frames;for(unsigned f=0;f<intraCount;++f)if(masks.read(1))frames.push_back(f);need[g]+=frames.size()*packed_bits(spec);active[g].push_back(std::move(frames));}
        if((need[0]+7)/8!=h[4]||(need[1]+7)/8!=h[5]||(need[2]+7)/8>h[6])throw ThemeError("Invalid RAF animation masks");
        p=maskAt+(channels*intraCount+7)/8;
        for(unsigned g=0;g<3;++g){const auto count=g<2?h[4+g]:(need[g]+7)/8;Bits bits{bytes(data,p,count)};p+=count;
            for(unsigned i=0;i<specs[g].size();++i)for(auto frame:active[g][i]){auto key=packed_key(bits,specs[g][i],g==0);key.insert(key.begin(),start+1+frame);out[names[g]][std::to_string(lists[4+g][i])].push_back(key);}}}
    for(const auto* name:names)for(auto& keys:out[name])std::sort(keys.begin(),keys.end(),[](const json& a,const json& b){return a[0].get<float>()<b[0].get<float>();});
    return out;
}

} // namespace

json compile_raf_animation(const RafDocument& document,const fs::path& output,const ImportLimits& limits,std::vector<std::string>& warnings) {
    if(!document.elements.is_array()||document.elements.size()>limits.entries||document.files.size()>limits.entries)throw ThemeError("RAF tree exceeds budget");
    std::map<std::string,std::size_t> fileIds;std::size_t fileIndex=0;
    for(const auto& e:document.elements)if(e.value("name","")=="file"){fileIds[e.value("attributes",json::object()).value("id","")]=fileIndex++;}
    auto attrs=[](const json& e){return e.value("attributes",json::object());};
    auto file=[&](const json& a)->View{std::size_t index;
        if(a.contains("fileindex")){const auto n=a["fileindex"].get<std::int64_t>();if(n<0)throw ThemeError("Negative RAF file index");index=static_cast<std::size_t>(n);}
        else{auto i=fileIds.find(a.value("fileref",""));if(i==fileIds.end())throw ThemeError("Missing RAF file reference");index=i->second;}
        if(index>=document.files.size()||document.files[index].size()>limits.file_bytes)throw ThemeError("RAF binary file exceeds budget");return document.files[index];};
    json scene={{"textures",json::array()},{"models",json::array()},{"actors",json::array()},{"lights",json::array()},
        {"camera",{{"position",{0,0,10}},{"direction",{0,0,-1}},{"up",{0,1,0}},{"yfov",0.6},{"near",0.1},{"far",1000}}}};
    std::map<std::string,std::size_t> textureIds,modelIds;std::map<std::string,json> materials;std::string material;Bytes script;
    std::uint64_t expanded=0;
    for(const auto& e:document.elements)if(e.value("name","")=="texture"){
        const auto a=attrs(e);const auto type=a.value("type",0);if(type<0||type>3)continue;
        try{auto decoded=texture(file(a),limits);expanded+=decoded.rgba.size();if(expanded>limits.expanded_bytes)throw ThemeError("RAF textures exceed expanded budget");
            const auto id=a.value("id",""),name="animation/texture-"+std::to_string(scene["textures"].size())+".png";save(output/name,png(decoded.width,decoded.height,decoded.rgba));
            std::size_t solid=0;for(std::size_t p=3;p<decoded.rgba.size();p+=4)if(decoded.rgba[p]>=250)++solid;
            json metadata={{"id",id},{"file",name},{"width",decoded.width},{"height",decoded.height},{"type",type},
                {"solid",double(solid)/std::max<std::size_t>(1,decoded.rgba.size()/4)}};
            if(type!=0){const auto raw="animation/map-"+std::to_string(scene["textures"].size())+".rgba";save(output/raw,decoded.rgba);metadata["rgba_file"]=raw;}
            textureIds[id]=scene["textures"].size();scene["textures"].push_back(std::move(metadata));
        }catch(const ThemeError& error){warnings.push_back(error.what());}}
    for(const auto& e:document.elements){const auto name=e.value("name","");const auto a=attrs(e);
        if(name=="material"){material=a.value("id","");materials[material]=a;}
        else if(name=="_texture"&&!material.empty()){const auto id=a.value("texref","");if(textureIds.contains(id)){const auto i=textureIds[id];const auto kind=scene["textures"][i].value("type",0);const auto key=kind==0?"texture":kind==2?"specularMap":"normalMap";
            if(!materials[material].contains(key))materials[material][key]=i;if(kind==1||kind==3)materials[material]["normalSpace"]=kind==1?"object":"tangent";}}}
    json* model=nullptr;
    for(const auto& e:document.elements){const auto name=e.value("name","");const auto a=attrs(e);
        if(name=="model"){if(scene["models"].size()>=256)throw ThemeError("RAF model count exceeds budget");modelIds[a.value("id","")]=scene["models"].size();scene["models"].push_back({{"id",a.value("id","")}});model=&scene["models"].back();}
        else if(model&&(name=="geometry"||name=="skeleton"||name=="animation"||name=="inv-bind")){
            const auto data=file(a);
            if(name=="geometry")(*model)[name]=geometry(data);
            else if(name=="skeleton")(*model)[name]=skeleton(data);
            else if(name=="inv-bind"){if(data.size()%48||data.size()/48>maxJoints)throw ThemeError("Invalid RAF inverse bind matrices");auto v=json::array();for(std::size_t i=0;i<data.size();i+=4)v.push_back(f32(data,i));(*model)["invBind"]=v;}
            else{auto value=animation(data);value["id"]=a.value("id","");if(!model->contains("animations"))(*model)["animations"]=json::array();(*model)["animations"].push_back(value);}}
        else if(name=="camera"){for(auto it=a.begin();it!=a.end();++it)scene["camera"][it.key()]=it.value();if(a.value("type",0)==1)scene["camera"]["ymag"]=a.value("ymag",1.0);}
        else if(name=="script"){auto b=file(a);script.assign(b.begin(),b.end());}
        else if(name=="light")scene["lights"].push_back(a);
        else if(name=="actor") {if(scene["actors"].size()>=maxActors)throw ThemeError("RAF actor count exceeds budget");auto actor=a;const auto id=a.value("model","");actor["model"]=modelIds.contains(id)?json(modelIds[id]):json{};
            const auto mat=materials.find(a.value("material",""));if(mat!=materials.end()){for(const auto* key:{"texture","normalMap","specularMap","normalSpace"})if(mat->second.contains(key))actor[key]=mat->second[key];const auto effect=mat->second.value("effect","pure_texture");actor["lit"]=!effect.starts_with("pure_texture");actor["blend"]=effect.ends_with("_add")?"add":"alpha";actor["depth"]=effect.find("depth_0")==std::string::npos;actor["edgeLit"]=effect.find("edge_lit")!=std::string::npos;}
            scene["actors"].push_back(actor);}}
    const auto metadata=scene.dump();if(metadata.size()>limits.file_bytes)throw ThemeError("RAF decoded scene exceeds metadata budget");
    save(output/"animation/scene.json",View(reinterpret_cast<const std::uint8_t*>(metadata.data()),metadata.size()));
    if(!script.empty())save(output/"animation/script.vsmx",script);
    json result={{"kind","raf"},{"scene_file","animation/scene.json"}};if(!script.empty())result["script_file"]="animation/script.vsmx";
    return result;
}

namespace {
using Vec2=std::array<float,2>;
using Vec3=std::array<float,3>;
using Vec4=std::array<float,4>;
using Mat=std::array<float,12>;
constexpr Mat identity{1,0,0,0,0,1,0,0,0,0,1,0};
Vec3 add(Vec3 a,Vec3 b){for(unsigned k=0;k<3;++k)a[k]+=b[k];return a;}
Vec3 subtract(Vec3 a,Vec3 b){for(unsigned k=0;k<3;++k)a[k]-=b[k];return a;}
Vec3 scale(Vec3 a,float s){for(auto& x:a)x*=s;return a;}
float dot(Vec3 a,Vec3 b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
Vec3 cross(Vec3 a,Vec3 b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
Vec3 normalize(Vec3 a){return scale(a,1/std::max(0.000001F,std::sqrt(dot(a,a))));}
template<std::size_t N>std::array<float,N> vector(const json& a,std::array<float,N> fallback={}) {
    if(!a.is_array())return fallback;for(std::size_t k=0;k<N&&k<a.size();++k)if(a[k].is_number()){const auto v=a[k].get<float>();if(std::isfinite(v))fallback[k]=v;}return fallback;
}
template<std::size_t N>std::array<float,N> vector(vsmx::Value a,std::array<float,N> fallback={}) {
    a=vsmx::deref(a);if(!a.obj||!a.obj->array)return fallback;for(std::size_t k=0;k<N&&k<a.obj->items.size();++k){const auto v=vsmx::number(a.obj->items[k]);if(std::isfinite(v))fallback[k]=v;}return fallback;
}
Mat mul(const Mat& a,const Mat& b){Mat m{};for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)m[r*4+c]=a[r*4]*b[c]+a[r*4+1]*b[4+c]+a[r*4+2]*b[8+c]+(c==3?a[r*4+3]:0);return m;}
Vec3 transform(const Mat& m,Vec3 v,bool point=true){return {m[0]*v[0]+m[1]*v[1]+m[2]*v[2]+(point?m[3]:0),m[4]*v[0]+m[5]*v[1]+m[6]*v[2]+(point?m[7]:0),m[8]*v[0]+m[9]*v[1]+m[10]*v[2]+(point?m[11]:0)};}
Mat compose(Vec4 q,Vec3 t,Vec3 s){const auto [x,y,z,w]=q;return {(1-2*(y*y+z*z))*s[0],2*(x*y-z*w)*s[1],2*(x*z+y*w)*s[2],t[0],2*(x*y+z*w)*s[0],(1-2*(x*x+z*z))*s[1],2*(y*z-x*w)*s[2],t[1],2*(x*z-y*w)*s[0],2*(y*z+x*w)*s[1],(1-2*(x*x+y*y))*s[2],t[2]};}
Mat actor_matrix(const vsmx::Value& a){
    const auto p=vector<3>(vsmx::get(a,"position")),s=vector<3>(vsmx::get(a,"scale"),{1,1,1}),r=vector<3>(vsmx::get(a,"rotation"));
    Mat m=identity;const auto direction=vsmx::deref(vsmx::get(a,"direction"));
    if(direction.obj&&direction.obj->array){const auto z=normalize(vector<3>(direction,{0,0,1})),x=normalize(cross(vector<3>(vsmx::get(a,"up"),{0,1,0}),z)),y=cross(z,x);m={x[0],y[0],z[0],0,x[1],y[1],z[1],0,x[2],y[2],z[2],0};}
    else{const float cx=std::cos(r[0]),sx=std::sin(r[0]),cy=std::cos(r[1]),sy=std::sin(r[1]),cz=std::cos(r[2]),sz=std::sin(r[2]);m={cz*cy,cz*sy*sx-sz*cx,cz*sy*cx+sz*sx,0,sz*cy,sz*sy*sx+cz*cx,sz*sy*cx-cz*sx,0,-sy,cy*sx,cy*cx,0};}
    for(unsigned row=0;row<3;++row){for(unsigned col=0;col<3;++col)m[row*4+col]*=s[col];m[row*4+3]=p[row];}return m;
}
float ease(float t,const Vec4& curve,bool bezier){if(!bezier)return t;float lo=0,hi=1;const auto f=[](float a,float b,float u){return 3*a*u*(1-u)*(1-u)+3*b*u*u*(1-u)+u*u*u;};for(unsigned i=0;i<16;++i){const float m=(lo+hi)*.5F;if(f(curve[0],curve[2],m)<t)lo=m;else hi=m;}return f(curve[1],curve[3],(lo+hi)*.5F);}

struct Key {float time;Vec4 value;};
using Track=std::vector<Key>;
struct Animation {float duration{},fps{30};std::string id;std::array<std::map<unsigned,Track>,3> channels;};
struct Model {
    bool hasNormals{};
    std::vector<Vec3> positions,normals,tangents,bitangents;std::vector<Vec2> uv;std::vector<Vec4> colors,weights;
    std::vector<std::array<unsigned,4>> joints;std::vector<std::uint32_t> indexes;
    std::vector<Vec4> poseQ;std::vector<Vec3> poseT,poseS;std::vector<int> parents;
    std::vector<Mat> inverse;std::vector<Animation> animations;
};
Vec4 sample_track(const Track& keys,float time,bool rotation){
    if(keys.empty())return {};if(time<=keys.front().time)return keys.front().value;if(time>=keys.back().time)return keys.back().value;
    const auto upper=std::upper_bound(keys.begin(),keys.end(),time,[](float t,const Key& k){return t<k.time;});const auto& b=*upper;const auto& a=*(upper-1);
    const float f=(time-a.time)/std::max(0.00001F,b.time-a.time);float sign=1;if(rotation&&std::inner_product(a.value.begin(),a.value.end(),b.value.begin(),0.0F)<0)sign=-1;
    Vec4 value;for(unsigned k=0;k<4;++k)value[k]=a.value[k]+(b.value[k]*sign-a.value[k])*f;
    if(rotation){const float n=std::sqrt(std::inner_product(value.begin(),value.end(),value.begin(),0.0F));for(auto& v:value)v/=std::max(n,.00001F);}return value;
}
std::vector<Model> load_models(const json& scene){
    std::vector<Model> out;const auto models=scene.value("models",json::array());if(!models.is_array()||models.size()>256)throw ThemeError("RAF model count exceeds budget");
    for(const auto& data:models){Model m;const auto g=data.value("geometry",json::object());const auto positions=g.value("positions",json::array());if(positions.size()%3||positions.size()/3>maxVertices)throw ThemeError("Invalid RAF position array");
        const auto n=positions.size()/3,normals=g.value("normals",json::array()).size();m.hasNormals=n>0&&normals==n*3;
        auto floats=[&](const char* key,std::size_t vertex,unsigned count,float fallback){Vec4 v{};const auto it=g.find(key);for(unsigned k=0;k<count;++k)v[k]=it!=g.end()&&it->is_array()&&vertex*count+k<it->size()&&(*it)[vertex*count+k].is_number()?(*it)[vertex*count+k].get<float>():fallback;for(auto x:v)if(!std::isfinite(x))throw ThemeError("Nonfinite RAF vertex data");return v;};
        for(std::size_t i=0;i<n;++i){const auto p=floats("positions",i,3,0),normal=floats("normals",i,3,0),uv=floats("uvs",i,2,0);m.positions.push_back({p[0],p[1],p[2]});m.normals.push_back({normal[0],normal[1],normal[2]});m.uv.push_back({uv[0],uv[1]});m.colors.push_back(floats("colors",i,4,1));auto w=floats("weights",i,4,0);if(w==Vec4{})w[0]=1;m.weights.push_back(w);
            const auto j=floats("joints",i,4,0);std::array<unsigned,4> joints;for(unsigned k=0;k<4;++k){if(j[k]<0||j[k]>=maxJoints)throw ThemeError("Invalid RAF joint index");joints[k]=static_cast<unsigned>(j[k]);}m.joints.push_back(joints);}
        const auto indices=g.value("indexes",json::array());if(indices.size()>maxIndexes||indices.size()%3)throw ThemeError("Invalid RAF index array");for(const auto& index:indices){if(!index.is_number_unsigned()&&!index.is_number_integer())throw ThemeError("Invalid RAF mesh index");const auto i=index.get<std::int64_t>();if(i<0||static_cast<std::size_t>(i)>=n)throw ThemeError("RAF mesh index out of range");m.indexes.push_back(static_cast<std::uint32_t>(i));}
        m.tangents.resize(n);m.bitangents.resize(n);
        for(std::size_t triangle=0;triangle<m.indexes.size();triangle+=3){const auto a=m.indexes[triangle],b=m.indexes[triangle+1],c=m.indexes[triangle+2];const auto e1=subtract(m.positions[b],m.positions[a]),e2=subtract(m.positions[c],m.positions[a]);
            const float u1=m.uv[b][0]-m.uv[a][0],v1=m.uv[b][1]-m.uv[a][1],u2=m.uv[c][0]-m.uv[a][0],v2=m.uv[c][1]-m.uv[a][1],det=u1*v2-u2*v1;
            if(std::abs(det)>.000001F){const auto tangent=scale(subtract(scale(e1,v2),scale(e2,v1)),1/det),bitangent=scale(subtract(scale(e2,u1),scale(e1,u2)),1/det);for(auto i:{a,b,c}){m.tangents[i]=add(m.tangents[i],tangent);m.bitangents[i]=add(m.bitangents[i],bitangent);}}}
        const auto sk=data.value("skeleton",json::object()),pose=sk.value("pose",json::array()),parents=sk.value("parents",json::array());if(pose.size()>maxJoints)throw ThemeError("RAF skeleton exceeds budget");
        for(std::size_t j=0;j<pose.size();++j){const auto p=vector<12>(pose[j]);m.poseQ.push_back({p[0],p[1],p[2],p[3]});m.poseT.push_back({p[4],p[5],p[6]});m.poseS.push_back({p[8],p[9],p[10]});m.parents.push_back(j<parents.size()&&parents[j].is_number_integer()?parents[j].get<int>():-1);}
        const auto inv=data.value("invBind",json::array());if(inv.size()>maxJoints*12||inv.size()%12)throw ThemeError("Invalid RAF inverse bind array");for(std::size_t i=0;i<inv.size()/12;++i){Mat mat;for(unsigned k=0;k<12;++k)mat[k]=inv[i*12+k].get<float>();m.inverse.push_back(mat);}
        auto anims=data.value("animations",json::array());if(data.contains("animation"))anims.push_back(data["animation"]);if(anims.size()>64)throw ThemeError("RAF animation slot count exceeds budget");
        for(const auto& animation:anims){Animation a;a.id=animation.value("id","");a.duration=animation.value("duration",0.0F);a.fps=animation.value("fps",30.0F);if(!std::isfinite(a.duration)||!std::isfinite(a.fps)||a.duration<0||a.fps<=0||a.fps>240)throw ThemeError("Invalid RAF animation timing");
            constexpr const char* kinds[3]={"rotation","translation","scale"};for(unsigned kind=0;kind<3;++kind){const auto channels=animation.value(kinds[kind],json::object());if(!channels.is_object()||channels.size()>maxJoints)throw ThemeError("Invalid RAF animation channels");
                for(auto it=channels.begin();it!=channels.end();++it){const auto joint=std::stoul(it.key());if(joint>=maxJoints||!it->is_array()||it->size()>65536)throw ThemeError("RAF animation track exceeds budget");auto& track=a.channels[kind][joint];for(const auto& key:*it){if(!key.is_array()||key.size()<(kind==0?5:4))throw ThemeError("Invalid RAF animation key");Vec4 v{};for(unsigned k=0;k<(kind==0?4:3);++k)v[k]=key[k+1].get<float>();track.push_back({key[0].get<float>(),v});}std::sort(track.begin(),track.end(),[](const Key& x,const Key& y){return x.time<y.time;});}}
            m.animations.push_back(std::move(a));}out.push_back(std::move(m));}
    return out;
}
} // namespace

struct RafRuntime::Impl {
    using Value=vsmx::Value;
    json initial;
    Bytes script;
    ImportLimits limits;
    vsmx::VM vm;
    std::vector<Model> models;
    std::vector<Value> actors,lights;
    Value camera,system;
    struct Glide {Value target;std::string property;std::vector<float> from,to;float age{},duration{};Vec4 curve{};bool bezier{},direction{};};
    struct Timer {float interval{},due{};Value fn,self;std::weak_ptr<vsmx::Object> object;bool once{},active{};};
    std::vector<Glide> glides;
    std::vector<Timer> timers;
    std::vector<std::string> diagnostics;
    std::vector<Bytes> mapPixels;
    std::vector<bool> largeBackdrop;
    std::string mapsDirectory;
    float clock{},frameTime{1.0F/30};double requested{},remainder{};
    float fittedZoom{};
    bool broken{};
    std::uint32_t randomState{0x9e3779b9};

    Impl(json scene,Bytes code,ImportLimits budget):initial(std::move(scene)),script(std::move(code)),limits(budget),vm(script),models(load_models(initial)) {
        const auto data=initial.value("actors",json::array());if(!data.is_array()||data.size()>maxActors)throw ThemeError("RAF actor count exceeds budget");largeBackdrop.resize(data.size());
        for(const auto& actor:data){auto object=vm.make_object();
            for(const auto* key:{"id","position","rotation","scale","color","up","direction"})if(actor.contains(key))vsmx::set(object,key,vm.from_json(actor[key]));
            for(const auto& [key,value]:std::map<std::string,json>{{"position",{0,0,0}},{"rotation",{0,0,0}},{"scale",{1,1,1}},{"color",{1,1,1,1}},{"uv_offset",actor.value("uvOffset",actor.value("uv_offset",json::array({0,0})))}})if(vsmx::get(object,key).kind==Value::undefined)vsmx::set(object,key,vm.from_json(value));
            vsmx::set(object,"enable",true);
            for(const auto& [name,source,fallback]:std::array<std::tuple<const char*,const char*,json>,3>{{{"animTimes","anim_time",json::array({0})},{"animSpeeds","anim_speed",json::array({1})},{"animWeights","anim_weight",json::array({1})}}})vsmx::set(object,name,vm.from_json(actor.value(name,actor.value(source,fallback))));
            methods(object,true);actors.push_back(object);}
        for(const auto& light:initial.value("lights",json::array())){if(lights.size()>=128)throw ThemeError("RAF light count exceeds budget");auto object=vm.from_json(light);methods(object,false);lights.push_back(object);}
        camera=vm.from_json(initial.value("camera",json::object()));for(const auto& [key,value]:std::map<std::string,json>{{"position",{0,0,10}},{"direction",{0,0,-1}},{"up",{0,1,0}},{"yfov",.6},{"near",.1},{"far",1000},{"aspect",16.0/9}})if(vsmx::get(camera,key).kind==Value::undefined)vsmx::set(camera,key,vm.from_json(value));methods(camera,false);
        const unsigned width=std::clamp(initial.value("_screen_width",1920u),1u,8192u),height=std::clamp(initial.value("_screen_height",1080u),1u,8192u);
        const bool wide=float(width)/height>=(16.0F/9+4.0F/3)*.5F;vsmx::set(camera,"aspect",wide?16.0F/9:4.0F/3);
        system=vm.make_object();vsmx::set(system,"timer",vm.make_object(true));vsmx::set(system,"interval",frameTime);vsmx::set(system,"resolution",vm.from_json(wide?json::array({1920,1080}):json::array({width,height})));vsmx::set(system,"printPerf",Value(vsmx::Native([](const Value&,std::span<const Value>){return Value{};})));
        host();try{vm.run();}catch(const std::exception& error){broken=true;note(error.what());}collect_warnings();
    }
    void note(std::string message){if(diagnostics.size()<128&&std::find(diagnostics.begin(),diagnostics.end(),message)==diagnostics.end())diagnostics.push_back(std::move(message));}
    void collect_warnings(){for(const auto& warning:vm.warnings)note("Unsupported RAF script host method: "+warning);}
    static Value arg(std::span<const Value> args,std::size_t i){return i<args.size()?args[i]:Value{};}
    static unsigned slot(Value value){const auto number=vsmx::number(value);return std::isfinite(number)?static_cast<unsigned>(std::clamp(number,0.0F,63.0F)):0;}
    std::vector<float> values(Value value){value=vsmx::deref(value);std::vector<float> out;if(value.obj&&value.obj->array){if(value.obj->items.size()>16)throw ThemeError("RAF animation vector exceeds budget");for(const auto& v:value.obj->items)out.push_back(vsmx::number(v));}else out.push_back(vsmx::number(value));return out;}
    void glide(const Value& target,std::string property,std::span<const Value> args){
        auto to=values(arg(args,0));float duration=std::max(0.0F,vsmx::number(arg(args,1)));auto from=values(vsmx::get(target,property));
        if(property=="color"&&to.size()==3)to.push_back(from.size()>3?from[3]:1);
        if(property=="direction"&&from.size()!=3)from={0,0,1};
        std::erase_if(glides,[&](const Glide& g){return g.target.obj==target.obj&&g.property==property;});
        if(duration<=0){vsmx::set(target,property,vm.array(to));return;}
        if(glides.size()>=2048)throw ThemeError("RAF active glide count exceeds budget");
        const auto curve=vsmx::deref(arg(args,3));glides.push_back({target,property,std::move(from),std::move(to),0,duration,vector<4>(curve),bool(curve.obj&&curve.obj->array&&curve.obj->items.size()>=4),property=="direction"});
    }
    void methods(Value object,bool actor){
        for(const auto& [method,property]:std::map<std::string,std::string>{{"setColor","color"},{"setPosition","position"},{"setRotation","rotation"},{"setScale","scale"},{"setDirection","direction"},{"setUVOffset","uv_offset"}})
            vsmx::set(object,method,Value(vsmx::Native([this,property](const Value& self,std::span<const Value> args){glide(self,property,args);return Value{};})));
        auto timer=vm.make_object(true);std::weak_ptr<vsmx::Object> owner=object.obj;
        // Timer callbacks receive the actor that owns its timer slot as `this`.
        timer.obj->properties["__owner"]=Value(object.obj);vsmx::set(object,"timer",timer);
        if(!actor)return;
        vsmx::set(object,"setAnimSpeed",Value(vsmx::Native([](const Value& self,std::span<const Value> args){auto values=vsmx::get(self,"animSpeeds");vsmx::set(values,std::to_string(slot(arg(args,0))),vsmx::number(arg(args,1)));return Value{};})));
        vsmx::set(object,"setAnimTime",Value(vsmx::Native([](const Value& self,std::span<const Value> args){auto values=vsmx::get(self,"animTimes");vsmx::set(values,std::to_string(slot(arg(args,0))),vsmx::number(arg(args,1)));return Value{};})));
        vsmx::set(object,"getAnimTime",Value(vsmx::Native([](const Value& self,std::span<const Value> args){return vsmx::get(vsmx::get(self,"animTimes"),std::to_string(slot(arg(args,0))));})));
        vsmx::set(object,"getAnimIndex",Value(vsmx::Native([this](const Value& self,std::span<const Value> args){const auto name=vsmx::string(arg(args,0)),id=vsmx::string(vsmx::get(self,"id"));for(std::size_t i=0;i<actors.size();++i)if(vsmx::string(vsmx::get(actors[i],"id"))==id){const auto m=initial["actors"][i].value("model",json{});if(m.is_number_unsigned()||m.is_number_integer()){const auto index=m.get<unsigned>();if(index<models.size())for(std::size_t j=0;j<models[index].animations.size();++j)if(models[index].animations[j].id==name)return Value(static_cast<int>(j));}}return Value(0);})));
        vsmx::set(object,"setAnimWeight",Value(vsmx::Native([this](const Value& self,std::span<const Value> args){const auto index=slot(arg(args,0));auto weights=vsmx::get(self,"animWeights");const auto target=std::max(0.0F,vsmx::number(arg(args,1)));const auto duration=vsmx::number(arg(args,2));if(duration<=0)vsmx::set(weights,std::to_string(index),target);else{std::array<Value,4> values{Value(target),Value(duration),arg(args,3),arg(args,4)};glide(weights,std::to_string(index),values);}return Value{};})));
    }
    void host(){
        auto bind=[&](std::string name,vsmx::Native callback){vsmx::set(vm.globals,name,Value(std::move(callback)));};
        bind("Actor",[this](const Value&,std::span<const Value> args){const auto id=vsmx::string(arg(args,0));for(const auto& actor:actors)if(vsmx::string(vsmx::get(actor,"id"))==id)return actor;note("Script references missing actor "+id);auto a=vm.make_object();methods(a,true);return a;});
        bind("Light",[this](const Value&,std::span<const Value> args){const auto id=vsmx::string(arg(args,0));for(const auto& light:lights)if(vsmx::string(vsmx::get(light,"id"))==id)return light;return vm.make_object();});
        bind("Camera",[this](const Value&,std::span<const Value>){return camera;});vsmx::set(vm.globals,"System",system);
        bind("Object",[this](const Value&,std::span<const Value>){return vm.make_object();});
        bind("Array",[this](const Value&,std::span<const Value> args){auto a=vm.make_object(true);if(args.size()==1&&args[0].kind==Value::number){const auto n=vsmx::number(args[0]);if(!std::isfinite(n)||n<0||n>4096)throw ThemeError("RAF script array exceeds budget");a.obj->items.resize(static_cast<std::size_t>(n));}else a.obj->items.assign(args.begin(),args.end());return a;});
        bind("String",[](const Value&,std::span<const Value> args){return Value(vsmx::string(arg(args,0)));});
        bind("Error",[this](const Value& self,std::span<const Value> args){vsmx::set(self,"name","Error");vsmx::set(self,"message",vsmx::string(arg(args,0)));return self;});
        bind("Date",[](const Value& self,std::span<const Value>){const auto now=std::time(nullptr);std::tm time{};
#ifdef _WIN32
            localtime_s(&time,&now);
#else
            localtime_r(&now,&time);
#endif
            for(const auto& [key,value]:std::map<std::string,int>{{"hours",time.tm_hour},{"minutes",time.tm_min},{"seconds",time.tm_sec},{"day",time.tm_mday},{"month",time.tm_mon+1},{"year",time.tm_year+1900}})vsmx::set(self,key,value);return self;});
        bind("writeln",[](const Value&,std::span<const Value>){return Value{};});
        for(const auto& [key,value]:std::map<std::string,int>{{"INTERPOLATION_LINEAR",0},{"INTERPOLATION_STEP",1},{"INTERPOLATION_BEZIER",2}})vsmx::set(vm.globals,key,value);
        for(const auto& [name,once]:std::map<std::string,bool>{{"IntervalTimer",false},{"OneShotTimer",true}})bind(name,[this,once](const Value& self,std::span<const Value> args){
            float interval=vsmx::number(arg(args,0));if(!std::isfinite(interval))interval=1;interval=std::max(.001F,interval);
            std::size_t slot=0;for(;slot<timers.size();++slot)if(!timers[slot].active&&timers[slot].object.expired())break;
            if(slot>=1024)throw ThemeError("RAF timer count exceeds budget");if(slot==timers.size())timers.emplace_back();timers[slot]={interval,clock+interval,arg(args,1),{},self.obj,once,true};vsmx::set(self,"__timer",static_cast<int>(slot));return self;});
        auto math=vm.make_object();vsmx::set(vm.globals,"Math",math);vsmx::set(math,"PI",3.141592653589793);vsmx::set(math,"E",2.718281828459045);
        const std::map<std::string,std::function<float(float)>> unary{{"sin",[](float x){return std::sin(x);}},{"cos",[](float x){return std::cos(x);}},{"tan",[](float x){return std::tan(x);}},{"asin",[](float x){return std::asin(x);}},{"acos",[](float x){return std::acos(x);}},{"atan",[](float x){return std::atan(x);}},{"sqrt",[](float x){return std::sqrt(x);}},{"abs",[](float x){return std::abs(x);}},{"exp",[](float x){return std::exp(x);}},{"log",[](float x){return std::log(x);}},{"floor",[](float x){return std::floor(x);}},{"ceil",[](float x){return std::ceil(x);}},{"round",[](float x){return std::floor(x+.5F);}}};
        for(const auto& [name,fn]:unary)vsmx::set(math,name,Value(vsmx::Native([fn](const Value&,std::span<const Value> args){return Value(fn(vsmx::number(arg(args,0))));})));
        for(const auto& name:{"pow","atan2","min","max"})vsmx::set(math,name,Value(vsmx::Native([name=std::string(name)](const Value&,std::span<const Value> args){const auto a=vsmx::number(arg(args,0)),b=vsmx::number(arg(args,1));if(name=="pow")return Value(std::pow(a,b));if(name=="atan2")return Value(std::atan2(a,b));float out=a;for(std::size_t i=1;i<args.size();++i)out=name=="min"?std::min(out,vsmx::number(args[i])):std::max(out,vsmx::number(args[i]));return Value(out);})));
        vsmx::set(math,"random",Value(vsmx::Native([this](const Value&,std::span<const Value>){randomState^=randomState<<13;randomState^=randomState>>17;randomState^=randomState<<5;return Value(float(randomState&0xffffff)/16777216.0F);})));
        vsmx::set(math,"dot",Value(vsmx::Native([](const Value&,std::span<const Value> args){return Value(dot(vector<3>(arg(args,0)),vector<3>(arg(args,1))));})));
        vsmx::set(math,"cross",Value(vsmx::Native([this](const Value&,std::span<const Value> args){return vm.array(cross(vector<3>(arg(args,0)),vector<3>(arg(args,1))));})));
        vsmx::set(math,"normalize",Value(vsmx::Native([this](const Value&,std::span<const Value> args){return vm.array(normalize(vector<3>(arg(args,0))));})));
        vsmx::set(math,"len",Value(vsmx::Native([](const Value&,std::span<const Value> args){const auto a=vsmx::deref(arg(args,0));return Value(a.obj&&a.obj->array?std::sqrt(dot(vector<3>(a),vector<3>(a))):std::abs(vsmx::number(a)));})));
    }
    void tick(float dt){
        clock+=dt;frameTime=dt;vsmx::set(system,"interval",dt);vm.begin_tick();
        if(!broken){const auto count=timers.size();for(std::size_t i=0;i<count;++i){if(!timers[i].active||(timers[i].interval>.05F&&clock<timers[i].due))continue;auto callback=timers[i].fn,self=timers[i].self;const auto object=timers[i].object.lock();
                if(!self.obj&&object){self=vsmx::get(Value(object),"__owner");if(self.obj)timers[i].self=self;}
                timers[i].due=std::max(timers[i].due+timers[i].interval,clock);if(timers[i].once)timers[i].active=false;
                try{vm.invoke(callback,self,{});}catch(const std::exception& error){broken=true;note(error.what());break;}}}collect_warnings();
        for(auto& glide:glides){glide.age=std::min(glide.duration,glide.age+dt);const float t=ease(glide.age/glide.duration,glide.curve,glide.bezier);std::vector<float> value(glide.to.size());
            for(std::size_t k=0;k<value.size();++k)value[k]=(k<glide.from.size()?glide.from[k]:0)*(1-t)+glide.to[k]*t;
            if(glide.direction&&value.size()==3){auto direction=normalize(Vec3{value[0],value[1],value[2]});value.assign(direction.begin(),direction.end());}
            vsmx::set(glide.target,glide.property,glide.target.obj&&glide.target.obj->array&&value.size()==1?Value(value[0]):vm.array(value));}
        std::erase_if(glides,[](const Glide& glide){return glide.age>=glide.duration;});
        for(std::size_t i=0;i<actors.size();++i){const auto model=initial["actors"][i].value("model",json{});if(!model.is_number_integer()&&!model.is_number_unsigned())continue;const auto index=model.get<unsigned>();if(index>=models.size())continue;
            auto times=vsmx::get(actors[i],"animTimes"),speeds=vsmx::get(actors[i],"animSpeeds");for(std::size_t slot=0;slot<models[index].animations.size();++slot){const auto key=std::to_string(slot);auto speed=vsmx::get(speeds,key);if(speed.kind==Value::undefined)speed=vsmx::get(speeds,"0");float time=vsmx::number(vsmx::get(times,key))+dt*vsmx::number(speed);const auto duration=models[index].animations[slot].duration;if(duration>0&&time>duration+.001F)time=std::fmod(time,duration);vsmx::set(times,key,time);}}
    }
    std::vector<Mat> skin(const Model& model,const Value& actor){
        if(model.poseQ.empty())return {identity};std::vector<Mat> world(model.poseQ.size(),identity);
        const auto times=vsmx::get(actor,"animTimes"),weights=vsmx::get(actor,"animWeights");
        for(std::size_t joint=0;joint<world.size();++joint){Vec4 q{};Vec3 t{},s{};float total=0;
            for(std::size_t slot=0;slot<model.animations.size();++slot){const float weight=vsmx::number(vsmx::get(weights,std::to_string(slot)));if(weight<=.001F)continue;const auto& anim=model.animations[slot];const float frame=vsmx::number(vsmx::get(times,std::to_string(slot)))*anim.fps;
                Vec4 rotation=model.poseQ[joint],translation{model.poseT[joint][0],model.poseT[joint][1],model.poseT[joint][2],0},scaling{model.poseS[joint][0],model.poseS[joint][1],model.poseS[joint][2],0};
                if(auto it=anim.channels[0].find(joint);it!=anim.channels[0].end()&&!it->second.empty())rotation=sample_track(it->second,frame,true);
                if(auto it=anim.channels[1].find(joint);it!=anim.channels[1].end()&&!it->second.empty())translation=sample_track(it->second,frame,false);
                if(auto it=anim.channels[2].find(joint);it!=anim.channels[2].end()&&!it->second.empty())scaling=sample_track(it->second,frame,false);
                const float sign=std::inner_product(q.begin(),q.end(),rotation.begin(),0.0F)<0?-1:1;for(unsigned k=0;k<4;++k)q[k]+=rotation[k]*weight*sign;for(unsigned k=0;k<3;++k){t[k]+=translation[k]*weight;s[k]+=scaling[k]*weight;}total+=weight;}
            Mat local;if(total<=.001F)local=compose(model.poseQ[joint],model.poseT[joint],model.poseS[joint]);else{const float norm=std::sqrt(std::inner_product(q.begin(),q.end(),q.begin(),0.0F));for(auto& v:q)v/=std::max(.00001F,norm);for(unsigned k=0;k<3;++k){t[k]/=total;s[k]/=total;}local=compose(q,t,s);}
            const auto parent=model.parents[joint];world[joint]=parent>=0&&static_cast<std::size_t>(parent)<joint?mul(world[parent],local):local;}
        for(std::size_t j=0;j<world.size()&&j<model.inverse.size();++j)world[j]=mul(world[j],model.inverse[j]);return world;
    }
    AnimationFrame draw(const fs::path& directory){
        const auto textures=initial.value("textures",json::array());
        if(mapsDirectory!=directory.string()){mapsDirectory=directory.string();mapPixels.clear();mapPixels.resize(textures.size());
            for(std::size_t i=0;i<textures.size();++i)if(textures[i].contains("rgba_file")){const auto width=textures[i].value("width",0u),height=textures[i].value("height",0u);if(!width||!height||width>limits.image_dimension||height>limits.image_dimension||std::uint64_t(width)*height*4>limits.file_bytes)throw ThemeError("RAF material map exceeds its budget");
                mapPixels[i]=load(resource(directory,textures[i]["rgba_file"]),limits.file_bytes);if(mapPixels[i].size()!=std::size_t(width)*height*4)throw ThemeError("Truncated RAF material map");}}
        auto texel=[&](std::size_t index,float u,float v)->Vec4{if(index>=mapPixels.size()||mapPixels[index].empty())return {1,1,1,1};const unsigned w=textures[index].value("width",1u),h=textures[index].value("height",1u);u-=std::floor(u);v-=std::floor(v);const auto at=(std::size_t(std::min(h-1,static_cast<unsigned>(v*h)))*w+std::min(w-1,static_cast<unsigned>(u*w)))*4;const auto& bytes=mapPixels[index];return {bytes[at]/255.0F,bytes[at+1]/255.0F,bytes[at+2]/255.0F,bytes[at+3]/255.0F};};
        auto backdrop=[&](std::size_t index){if(index>=textures.size())return false;const float w=textures[index].value("width",0.0F),h=textures[index].value("height",0.0F);return h>0&&std::abs(w/h-16.0F/9)<.03F&&textures[index].value("solid",0.0F)>=.9F;};
        bool hasBackdrop=false;for(const auto& actor:initial["actors"])if(actor.value("blend","")!="add"&&actor.contains("texture")&&actor["texture"].is_number_integer()&&backdrop(actor["texture"].get<std::size_t>()))hasBackdrop=true;
        AnimationFrame frame;const auto cameraPosition=vector<3>(vsmx::get(camera,"position"),{0,0,10}),forward=normalize(vector<3>(vsmx::get(camera,"direction"),{0,0,-1}));
        const auto right=normalize(cross(forward,vector<3>(vsmx::get(camera,"up"),{0,1,0}))),up=cross(right,forward);
        const float aspect=std::max(.1F,float(initial.value("_screen_width",1920u))/std::max(1u,initial.value("_screen_height",1080u))),near=std::max(.0001F,vsmx::number(vsmx::get(camera,"near"))),far=std::max(near+.001F,vsmx::number(vsmx::get(camera,"far")));
        const auto ymag=vsmx::get(camera,"ymag");const bool ortho=ymag.kind!=Value::undefined&&vsmx::number(ymag)>0;
        const float f=ortho?2/vsmx::number(ymag):1/std::tan(std::clamp(vsmx::number(vsmx::get(camera,"yfov")),.01F,3.0F)*.5F);
        struct Ordered {float distance;AnimationDraw draw;};std::vector<Ordered> draws;float bestZoom=std::numeric_limits<float>::infinity();
        for(std::size_t a=0;a<actors.size();++a){const auto& source=initial["actors"][a];const auto actor=actors[a];if(!vsmx::truth(vsmx::get(actor,"enable")))continue;const auto color=vector<4>(vsmx::get(actor,"color"),{1,1,1,1});if(color[3]<=.001F)continue;
            const auto modelIndex=source.value("model",json{}),textureIndex=source.value("texture",json{});if((!modelIndex.is_number_unsigned()&&!modelIndex.is_number_integer())||(!textureIndex.is_number_unsigned()&&!textureIndex.is_number_integer()))continue;
            const auto mi=modelIndex.get<std::size_t>(),ti=textureIndex.get<std::size_t>();if(mi>=models.size()||ti>=initial["textures"].size())continue;const auto& model=models[mi];const auto& texture=initial["textures"][ti];if(!texture.contains("file"))continue;
            const auto matrix=actor_matrix(actor);const auto joints=skin(model,actor);const auto uvOffset=vector<2>(vsmx::get(actor,"uv_offset")),baseOffset=vector<2>(source.value("uvOffset",source.value("uv_offset",json::array({0,0})))),uvScale=vector<2>(source.value("uvScale",source.value("uv_scale",json::array({1,1}))),{1,1});
            AnimationDraw batch;batch.image=resource(directory,texture["file"]);batch.additive=source.value("blend","")=="add";batch.depth=source.value("depth",true);batch.vertices.reserve(model.indexes.size());
            std::vector<AnimationVertex> vertices(model.positions.size());float distanceSum=0;
            for(std::size_t i=0;i<vertices.size();++i){Vec3 point{},normal{};for(unsigned k=0;k<4;++k){const float weight=model.weights[i][k];const auto joint=model.joints[i][k];if(weight<=0||joint>=joints.size())continue;point=add(point,scale(transform(joints[joint],model.positions[i]),weight));normal=add(normal,scale(transform(joints[joint],model.normals[i],false),weight));}
                point=transform(matrix,point);normal=normalize(transform(matrix,normal,false));const auto eye=subtract(point,cameraPosition);const float distance=dot(eye,forward);distanceSum+=distance;auto& v=vertices[i];const float w=ortho?1:distance;
                v.x=.5F+.5F*dot(eye,right)*f/(aspect*w);v.y=.5F-.5F*dot(eye,up)*f/w;v.z=ortho?(distance-near)/(far-near):far/(far-near)*(1-near/distance);v.w=w;
                v.u=model.uv[i][0]*uvScale[0]+uvOffset[0]-baseOffset[0];v.v=model.uv[i][1]*uvScale[1]+uvOffset[1]-baseOffset[1];v.color=color;for(unsigned k=0;k<4;++k)v.color[k]*=model.colors[i][k];
                if(source.value("lit",false)&&model.hasNormals){
                    if(source.contains("normalMap")&&source["normalMap"].is_number_integer()){const auto sample=texel(source["normalMap"].get<std::size_t>(),v.u,v.v);Vec3 mapped{sample[0]*2-1,sample[1]*2-1,sample[2]*2-1};
                        if(source.value("normalSpace","")=="object")normal=normalize(transform(matrix,mapped,false));
                        else{const auto tangent=normalize(transform(matrix,model.tangents[i],false)),bitangent=normalize(transform(matrix,model.bitangents[i],false));normal=normalize(add(add(scale(tangent,mapped[0]),scale(bitangent,mapped[1])),scale(normal,mapped[2])));}}
                    const float shine=source.contains("specularMap")&&source["specularMap"].is_number_integer()?texel(source["specularMap"].get<std::size_t>(),v.u,v.v)[0]:0;Vec3 light{},specular{},primary{1,1,1};const auto view=normalize(subtract(cameraPosition,point));
                    for(std::size_t l=0;l<lights.size();++l){const auto kind=initial["lights"][l].value("type",0);const auto tint=vector<3>(vsmx::get(lights[l],"color"),{1,1,1});if(kind==2)light=add(light,tint);else{const auto delta=subtract(vector<3>(vsmx::get(lights[l],"position")),point);const float d=std::sqrt(dot(delta,delta));const auto att=vector<3>(initial["lights"][l].value("attenuation",json::array({1,0,0})),{1,0,0});const float attenuation=1/std::max(.001F,att[0]+att[1]*d+att[2]*d*d);const auto direction=normalize(delta);light=add(light,scale(tint,std::max(0.0F,dot(normal,direction))*attenuation));specular=add(specular,scale(tint,std::pow(std::max(0.0F,dot(normal,normalize(add(direction,view)))),24)*shine*attenuation));primary=tint;}}
                    if(source.value("edgeLit",false))light=add(light,scale(primary,std::pow(1-std::max(0.0F,dot(normal,view)),2.5F)*.9F));
                    if(lights.empty())light={1,1,1};for(unsigned k=0;k<3;++k)v.color[k]=v.color[k]*light[k]+specular[k];}}
            for(std::size_t triangle=0;triangle<model.indexes.size();triangle+=3){bool visible=true;for(unsigned k=0;k<3;++k){const auto& v=vertices[model.indexes[triangle+k]];if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.w)||v.w<=0||v.z<0||v.z>1)visible=false;}
                if(visible)for(unsigned k=0;k<3;++k)batch.vertices.push_back(vertices[model.indexes[triangle+k]]);}
            if(!batch.vertices.empty()){
                float left=1,right=0,top=1,bottom=0;for(const auto& v:batch.vertices){left=std::min(left,v.x);right=std::max(right,v.x);top=std::min(top,v.y);bottom=std::max(bottom,v.y);}
                const float share=std::max(0.0F,std::min(1.0F,right)-std::max(0.0F,left))*std::max(0.0F,std::min(1.0F,bottom)-std::max(0.0F,top));
                if(fittedZoom==0&&!batch.additive&&color[3]>=1&&left<.5F&&right>.5F&&top<.5F&&bottom>.5F)
                    bestZoom=std::min(bestZoom,std::max({1.0F,.5F/(.5F-left),.5F/(right-.5F),.5F/(.5F-top),.5F/(bottom-.5F)}));
                if(share>=.2F)largeBackdrop[a]=true;
                const bool background=!batch.additive&&(hasBackdrop?backdrop(ti):largeBackdrop[a]&&texture.value("solid",1.0F)>=.1F);
                if((background&&!initial.value("_background_enabled",true))||(!background&&!initial.value("_overlay_enabled",true)))continue;
                draws.push_back({distanceSum/std::max<std::size_t>(1,vertices.size()),std::move(batch)});}}
        if(fittedZoom==0)fittedZoom=bestZoom>1.001F&&bestZoom<=1.1F?bestZoom:1;
        std::stable_sort(draws.begin(),draws.end(),[](const Ordered& a,const Ordered& b){return a.distance>b.distance;});
        for(auto& draw:draws){if(fittedZoom!=1)for(auto& vertex:draw.draw.vertices){vertex.x=.5F+(vertex.x-.5F)*fittedZoom;vertex.y=.5F+(vertex.y-.5F)*fittedZoom;}frame.draws.push_back(std::move(draw.draw));}return frame;
    }
};

RafRuntime::RafRuntime(json scene,Bytes script,ImportLimits limits):impl_(std::make_unique<Impl>(std::move(scene),std::move(script),limits)){}
RafRuntime::~RafRuntime()=default;
RafRuntime::RafRuntime(RafRuntime&&) noexcept=default;
RafRuntime& RafRuntime::operator=(RafRuntime&&) noexcept=default;
AnimationFrame RafRuntime::sample(double seconds,const fs::path& directory){
    if(!std::isfinite(seconds)||seconds<0)throw ThemeError("Invalid RAF sample time");
    if(seconds<impl_->requested)impl_=std::make_unique<Impl>(impl_->initial,impl_->script,impl_->limits);
    double delta=seconds-impl_->requested;impl_->requested=seconds;
    if(delta>120) {impl_->note("Long RAF timeline gap held instead of replaying unbounded callbacks");delta=0;}
    impl_->remainder+=delta;const bool sixty=std::any_of(impl_->timers.begin(),impl_->timers.end(),[](const auto& t){return t.active&&!t.once&&t.interval<.025F;});const double step=sixty?1.0/60:1.0/30;
    unsigned guard=0;while(impl_->remainder+1e-8>=step){if(++guard>7201)throw ThemeError("RAF catch-up budget exhausted");impl_->tick(static_cast<float>(step));impl_->remainder-=step;}
    return impl_->draw(directory);
}
json RafRuntime::actor_state(std::string_view id) const {for(const auto& a:impl_->actors)if(vsmx::string(vsmx::get(a,"id"))==id)return impl_->vm.to_json(a);return {};}
const std::vector<std::string>& RafRuntime::warnings() const noexcept {return impl_->diagnostics;}

void validate_raf_animation(const json& descriptor,const fs::path& directory,const ImportLimits& limits){
    if(descriptor.value("kind","")!="raf")throw ThemeError("Invalid RAF animation descriptor");
    json scene;if(descriptor.contains("scene"))scene=descriptor["scene"];else{auto source=load(resource(directory,descriptor.at("scene_file")),limits.file_bytes);scene=json::parse(source.begin(),source.end());}
    if(!scene.is_object()||!scene.value("textures",json::array()).is_array())throw ThemeError("Invalid RAF scene");
    for(const auto& texture:scene.value("textures",json::array())){const auto path=resource(directory,texture.at("file"));if(!fs::is_regular_file(path)||fs::file_size(path)>limits.file_bytes)throw ThemeError("RAF texture is missing or exceeds its budget");if(texture.contains("rgba_file")){const auto raw=resource(directory,texture["rgba_file"]);if(!fs::is_regular_file(raw)||fs::file_size(raw)>limits.file_bytes)throw ThemeError("RAF material map is missing or exceeds its budget");}}
    Bytes script;if(descriptor.contains("script_file"))script=load(resource(directory,descriptor["script_file"]),limits.file_bytes);else if(descriptor.contains("script"))script=descriptor["script"].get<Bytes>();
    // Validate geometry and bytecode without executing an untrusted theme at
    // discovery time. Running its bounded script happens only when displayed.
    (void)load_models(scene);vsmx::VM vm(script);
}
AnimationFrame sample_raf_animation(const json& descriptor,const fs::path& directory,double seconds){
    struct Cache {std::string key;std::unique_ptr<RafRuntime> runtime;double origin;};static thread_local std::vector<Cache> cache;
    static thread_local std::string activeKey;
    auto key=fs::weakly_canonical(directory).string()+"|"+descriptor.dump();
    for(const auto* field:{"scene_file","script_file"})if(descriptor.contains(field)){
        const auto path=resource(directory,descriptor[field]);
        const auto elapsed=fs::last_write_time(path).time_since_epoch();
        const auto seconds=std::chrono::duration_cast<std::chrono::seconds>(elapsed);
        const auto nanoseconds=std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed-seconds);
        key+='|'+std::to_string(seconds.count())+'.'+std::to_string(nanoseconds.count())+':'+std::to_string(fs::file_size(path));
    }
    auto it=std::find_if(cache.begin(),cache.end(),[&](const Cache& c){return c.key==key;});
    if(it==cache.end()){if(cache.size()>=2)cache.erase(cache.begin());const ImportLimits limits;json scene;
        if(descriptor.contains("scene"))scene=descriptor["scene"];else{auto source=load(resource(directory,descriptor.at("scene_file")),limits.file_bytes);scene=json::parse(source.begin(),source.end());}
        Bytes script;if(descriptor.contains("script_file"))script=load(resource(directory,descriptor["script_file"]),limits.file_bytes);else if(descriptor.contains("script"))script=descriptor["script"].get<Bytes>();
        scene["_background_enabled"]=descriptor.value("background_enabled",true);scene["_overlay_enabled"]=descriptor.value("overlay_enabled",true);
        scene["_screen_width"]=descriptor.value("screen_width",1920u);scene["_screen_height"]=descriptor.value("screen_height",1080u);
        cache.push_back({key,std::make_unique<RafRuntime>(std::move(scene),std::move(script),limits),seconds});it=cache.end()-1;}
    if(activeKey!=key){activeKey=key;it->origin=seconds;}
    if(seconds<it->origin)it->origin=seconds;
    auto frame=it->runtime->sample(std::max(0.0,seconds-it->origin),directory);
    frame.warnings=it->runtime->warnings();return frame;
}

} // namespace openxmb::themes
