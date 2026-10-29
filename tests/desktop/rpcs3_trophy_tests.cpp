#include "openxmb/desktop/achievements.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs = std::filesystem;
using namespace openxmb::desktop;
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
void write(const fs::path& p, const std::string& data) { std::ofstream o(p, std::ios::binary); o.write(data.data(), static_cast<std::streamsize>(data.size())); if (!o) throw std::runtime_error("fixture write failed"); }
std::string read(const fs::path& p) { std::ifstream i(p, std::ios::binary); return {std::istreambuf_iterator<char>(i), {}}; }
void put32(std::string& v, std::size_t at, std::uint32_t n) { for (int i = 3; i >= 0; --i) { v[at+i] = static_cast<char>(n & 255); n >>= 8; } }
void put64(std::string& v, std::size_t at, std::uint64_t n) { for (int i = 7; i >= 0; --i) { v[at+i] = static_cast<char>(n & 255); n >>= 8; } }
std::string binary() {
  std::string v(0x30 + 0x40 + 0x60 * 2 + 0x70 * 2, '\0'); put32(v,0,0x818f54ad); put32(v,8,2);
  put32(v,0x30,4); put32(v,0x34,0x50); put32(v,0x3c,2); put64(v,0x40,0x70);
  put32(v,0x50,6); put32(v,0x54,0x60); put32(v,0x5c,2); put64(v,0x60,0x130);
  for (int i=0;i<2;++i) { auto a=0x70+i*0x60; put32(v,a,4); put32(v,a+4,0x50); put32(v,a+8,i); put32(v,a+16,i); put32(v,a+20,i==0?4:2);
    auto b=0x130+i*0x70; put32(v,b,6); put32(v,b+4,0x60); put32(v,b+8,i); put32(v,b+16,i); put32(v,b+20,i==0?1:0); if(i==0) put64(v,b+40,62135596800000000ull+1700000000ull*1000000); }
  return v;
}
int main() {
  const auto root=fs::temp_directory_path()/"openxmb-trophy-parser-test";
  try {
    fs::remove_all(root); const auto set=root/"home"/"00000001"/"trophy"/"NPWR00001_00"; fs::create_directories(set);
    const std::string xml="<?xml version=\"1.0\"?><trophyconf><title-name>Fixture &amp; Game</title-name><trophy id=\"0\" ttype=\"B\" hidden=\"no\"><name>First</name><detail>Earned</detail></trophy><trophy id=\"1\" ttype=\"G\" hidden=\"yes\"><name>Secret</name><detail>Hidden</detail></trophy></trophyconf>";
    const auto data=binary(); write(set/"TROPCONF.SFM",xml); write(set/"TROPUSR.DAT",data);
    auto users=rpcs3_trophy_users(root); require(bool(users)&&users.values.size()==1&&users.values[0]=="00000001","user discovery");
    auto sets=rpcs3_trophy_sets(root,"00000001"); require(bool(sets)&&sets.values.size()==1&&sets.values[0]=="NPWR00001_00","set discovery");
    require(!rpcs3_trophy_sets(root,"../bad"),"path traversal accepted");
    auto result=read_rpcs3_trophy_set(root,"00000001","NPWR00001_00"); require(bool(result),result.error.c_str());
    const auto& game=*result.value; require(game.title=="Fixture & Game"&&game.entries.size()==2,"metadata parsing");
    require(game.entries[0].grade==TrophyGrade::bronze&&game.entries[0].unlock_state_known&&game.entries[0].unlocked&&game.entries[0].unlocked_at==1700000000,"unlocked state or RTC timestamp");
    require(game.entries[1].hidden&&game.entries[1].unlock_state_known&&!game.entries[1].unlocked&&!game.entries[1].unlocked_at,"locked state");
    require(!game.entries[0].points&&!game.entries[0].hardcore,"unknown provider fields fabricated");
    require(read(set/"TROPCONF.SFM")==xml&&read(set/"TROPUSR.DAT")==data,"source bytes modified");
    fs::remove(set/"TROPUSR.DAT"); result=read_rpcs3_trophy_set(root,"00000001","NPWR00001_00"); require(bool(result)&&!result.value->entries[0].unlock_state_known&&!result.value->warnings.empty(),"missing state must remain unknown");
    auto malformed=data; put64(malformed,0x60,~std::uint64_t{}); write(set/"TROPUSR.DAT",malformed);
    result=read_rpcs3_trophy_set(root,"00000001","NPWR00001_00"); require(bool(result)&&!result.value->entries[0].unlock_state_known&&!result.value->warnings.empty(),"offset overflow must remain unknown");
    malformed=data; put32(malformed,0x130+0x70+16,0); write(set/"TROPUSR.DAT",malformed);
    result=read_rpcs3_trophy_set(root,"00000001","NPWR00001_00"); require(bool(result)&&!result.value->entries[0].unlock_state_known,"duplicate state id accepted");
    malformed=data; put32(malformed,0x70+20,1); write(set/"TROPUSR.DAT",malformed);
    result=read_rpcs3_trophy_set(root,"00000001","NPWR00001_00"); require(bool(result)&&!result.value->entries[0].unlock_state_known,"grade mismatch accepted");
    write(set/"TROPCONF.SFM","<!DOCTYPE trophyconf [<!ENTITY x SYSTEM 'file:///secret'>]><trophyconf><title-name>&x;</title-name></trophyconf>");
    require(!read_rpcs3_trophy_set(root,"00000001","NPWR00001_00"),"DTD accepted");
    write(set/"TROPCONF.SFM",xml+"\xff"); require(!read_rpcs3_trophy_set(root,"00000001","NPWR00001_00"),"invalid UTF8 accepted");
    auto badxml=xml; badxml.replace(badxml.find("&amp;"),5,"&unknown;"); write(set/"TROPCONF.SFM",badxml); require(!read_rpcs3_trophy_set(root,"00000001","NPWR00001_00"),"unknown entity accepted");
    badxml=xml; badxml.replace(badxml.find("id=\"1\""),6,"id=\"0\""); write(set/"TROPCONF.SFM",badxml); require(!read_rpcs3_trophy_set(root,"00000001","NPWR00001_00"),"duplicate metadata ID accepted");
    badxml=xml; badxml.replace(badxml.find("ttype=\"G\""),9,"ttype=\"X\""); write(set/"TROPCONF.SFM",badxml); require(!read_rpcs3_trophy_set(root,"00000001","NPWR00001_00"),"unknown metadata grade accepted");
    badxml=xml.substr(0,xml.size()-10); write(set/"TROPCONF.SFM",badxml); require(!read_rpcs3_trophy_set(root,"00000001","NPWR00001_00"),"truncated XML accepted");
    write(set/"TROPCONF.SFM",xml); malformed=data; put64(malformed,0x130+40,~std::uint64_t{}); write(set/"TROPUSR.DAT",malformed);
    result=read_rpcs3_trophy_set(root,"00000001","NPWR00001_00"); require(bool(result)&&result.value->entries[0].unlocked&&result.value->entries[0].unlock_state_known&&!result.value->entries[0].unlocked_at&&!result.value->warnings.empty(),"invalid timestamp must not be fabricated");
    malformed=data; put32(malformed,0x5c,1); write(set/"TROPUSR.DAT",malformed); result=read_rpcs3_trophy_set(root,"00000001","NPWR00001_00"); require(bool(result)&&!result.value->entries[1].unlock_state_known,"partial state must remain unknown");
    require(!read_rpcs3_trophy_set(root,"00000002","NPWR00001_00"),"other user must not resolve the first user's data");
    require(!read_rpcs3_trophy_set(root,"00000001","../NPWR00001_00"),"set path traversal accepted");
    malformed=data; put32(malformed,0x130+16,1); put32(malformed,0x130+0x70+16,0); write(set/"TROPUSR.DAT",malformed);
    result=read_rpcs3_trophy_set(root,"00000001","NPWR00001_00"); require(bool(result)&&!result.value->entries[0].unlock_state_known,"reordered state IDs accepted");
    badxml=xml; badxml.replace(badxml.find("id=\"0\""),6,"id=\"0\" id=\"0\""); write(set/"TROPCONF.SFM",badxml); require(!read_rpcs3_trophy_set(root,"00000001","NPWR00001_00"),"duplicate attribute accepted");
    write(set/"TROPCONF.SFM",xml); std::error_code link_error; const auto other=root/"home"/"00000002"; fs::create_directory_symlink(set.parent_path().parent_path(),other,link_error);
    if(!link_error) require(!read_rpcs3_trophy_set(root,"00000002","NPWR00001_00"),"linked user directory accepted");
    fs::remove_all(root); std::cout << "RPCS3 trophy fixtures passed\n"; return 0;
  } catch(const std::exception& e) { std::cerr << e.what() << '\n'; fs::remove_all(root); return 1; }
}


