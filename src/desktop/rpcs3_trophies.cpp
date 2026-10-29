#include "openxmb/desktop/achievements.hpp"
#include <pugixml.hpp>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace openxmb::desktop {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t max_file = 4 * 1024 * 1024, max_entries = 128, max_dirs = 4096;
bool user_name(std::string_view s) { return s.size()==8 && std::all_of(s.begin(),s.end(),[](unsigned char c){return c>='0'&&c<='9';}); }
bool game_name(std::string_view s) { return s.size()==12 && s.substr(0,4)=="NPWR" && s[9]=='_' && std::all_of(s.begin()+4,s.begin()+9,[](unsigned char c){return c>='0'&&c<='9';}) && std::all_of(s.begin()+10,s.end(),[](unsigned char c){return c>='0'&&c<='9';}); }
bool directory(const fs::path& p) { std::error_code e; auto st=fs::symlink_status(p,e); return !e && fs::is_directory(st); }
bool regular(const fs::path& p) { std::error_code e; auto st=fs::symlink_status(p,e); return !e && fs::is_regular_file(st); }
fs::path user_dir(const fs::path& root, std::string_view user) {
  if (!user_name(user)) throw std::runtime_error("Invalid RPCS3 user ID.");
  const auto home=root/"home", path=home/fs::path(user);
  if (!directory(home) || !directory(path)) throw std::runtime_error("RPCS3 user directory is unavailable or is a link.");
  return path;
}
std::string bounded_read(const fs::path& path) {
  if (!regular(path)) throw std::runtime_error("File is unavailable or is a link.");
  std::error_code error; const auto size=fs::file_size(path,error);
  if (error || size>max_file) throw std::runtime_error("File is unreadable or exceeds the size limit.");
  std::ifstream file(path,std::ios::binary); if(!file) throw std::runtime_error("File cannot be opened.");
  std::string result(static_cast<std::size_t>(size),'\0');
  if (!result.empty() && !file.read(result.data(),static_cast<std::streamsize>(result.size()))) throw std::runtime_error("File changed or could not be read completely.");
  if (file.peek()!=std::char_traits<char>::eof()) throw std::runtime_error("File changed while reading.");
  return result;
}
bool valid_xml_utf8(std::string_view s) {
  for(std::size_t i=0;i<s.size();) {
    const auto c=static_cast<unsigned char>(s[i]); std::uint32_t cp{}; std::size_t n{};
    if(c<0x80){cp=c;n=1;} else if(c>=0xc2&&c<=0xdf){cp=c&31;n=2;} else if(c>=0xe0&&c<=0xef){cp=c&15;n=3;} else if(c>=0xf0&&c<=0xf4){cp=c&7;n=4;} else return false;
    if(i+n>s.size()) return false;
    for(std::size_t j=1;j<n;++j){const auto d=static_cast<unsigned char>(s[i+j]);if((d&0xc0)!=0x80)return false;cp=(cp<<6)|(d&63);}
    if((n==2&&cp<0x80)||(n==3&&cp<0x800)||(n==4&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)||cp==0xfffe||cp==0xffff||(cp<0x20&&cp!=9&&cp!=10&&cp!=13))return false;
    i+=n;
  }
  return true;
}
bool valid_entities(std::string_view s) {
  for(std::size_t i=0;i<s.size();++i) if(s[i]=='&') {
    const auto end=s.find(';',i+1); if(end==s.npos || end-i>16) return false;
    const auto e=s.substr(i+1,end-i-1);
    if(e!="amp"&&e!="lt"&&e!="gt"&&e!="apos"&&e!="quot") {
      if(e.size()<2||e[0]!='#')return false;
      auto digits=e.substr(1); int base=10; if(!digits.empty()&&digits[0]=='x'){digits.remove_prefix(1);base=16;}
      if(digits.empty())return false;
      unsigned value{}; const auto parsed=std::from_chars(digits.data(),digits.data()+digits.size(),value,base);
      if(parsed.ec!=std::errc{}||parsed.ptr!=digits.data()+digits.size()||value>0x10ffff||(value>=0xd800&&value<=0xdfff)||value==0xfffe||value==0xffff||(value<32&&value!=9&&value!=10&&value!=13))return false;
    }
    i=end;
  }
  return true;
}
std::uint32_t be32(std::string_view s,std::size_t n) { return (std::uint32_t(static_cast<unsigned char>(s[n]))<<24)|(std::uint32_t(static_cast<unsigned char>(s[n+1]))<<16)|(std::uint32_t(static_cast<unsigned char>(s[n+2]))<<8)|static_cast<unsigned char>(s[n+3]); }
std::uint64_t be64(std::string_view s,std::size_t n) { return (std::uint64_t(be32(s,n))<<32)|be32(s,n+4); }
unsigned decimal(std::string_view s) {
  unsigned n{}; if(s.empty())throw std::runtime_error("Missing trophy ID.");
  auto p=std::from_chars(s.data(),s.data()+s.size(),n);
  if(p.ec!=std::errc{}||p.ptr!=s.data()+s.size()||n>=max_entries) throw std::runtime_error("Invalid trophy ID.");
  return n;
}
std::string child_text(pugi::xml_node node,const char* name,bool required) {
  auto child=node.child(name); if(!child) { if(required) throw std::runtime_error(std::string("Missing ")+name+"."); return {}; }
  if(child.next_sibling(name))throw std::runtime_error(std::string("Duplicate ")+name+".");
  for(auto c:child.children()) if(c.type()!=pugi::node_pcdata&&c.type()!=pugi::node_cdata)throw std::runtime_error("Invalid nested trophy text.");
  auto value=std::string(child.text().as_string()); if(value.size()>4096)throw std::runtime_error("Trophy text exceeds the size limit.");
  return value;
}
TrophyGrade grade(std::string_view s) { if(s=="P")return TrophyGrade::platinum;if(s=="G")return TrophyGrade::gold;if(s=="S")return TrophyGrade::silver;if(s=="B")return TrophyGrade::bronze;throw std::runtime_error("Invalid trophy grade."); }
unsigned grade_number(TrophyGrade g) { switch(g){case TrophyGrade::platinum:return 1;case TrophyGrade::gold:return 2;case TrophyGrade::silver:return 3;case TrophyGrade::bronze:return 4;default:return 0;} }
fs::path optional_icon(const fs::path& directory,const std::string& name) {auto p=directory/name; return regular(p)?p:fs::path{};}
struct State { unsigned grade{}, state{}; std::uint64_t timestamp{}; };
std::map<unsigned,State> parse_state(std::string_view bytes, const std::map<unsigned,TrophyGrade>& expected) {
  if(bytes.size()<0x30||be32(bytes,0)!=0x818f54ad)throw std::runtime_error("Invalid trophy state header.");
  const auto count=be32(bytes,8); if(count==0||count>16||0x30ull+std::uint64_t(count)*0x20>bytes.size())throw std::runtime_error("Invalid trophy table count.");
  std::map<unsigned,State> states; std::set<unsigned> types; std::vector<std::pair<std::size_t,std::size_t>> ranges;
  bool got4=false,got6=false;
  for(unsigned k=0;k<count;++k) {
    const std::size_t h=0x30+k*0x20; const auto type=be32(bytes,h),size=be32(bytes,h+4),n=be32(bytes,h+12); const auto offset=be64(bytes,h+16);
    if(!types.insert(type).second || n>max_entries || size>4096 || offset<0x30ull+std::uint64_t(count)*0x20)throw std::runtime_error("Invalid or duplicate trophy table.");
    const std::uint64_t stride=std::uint64_t(size)+16, length=stride*n;
    if(offset>bytes.size()||length>bytes.size()-offset)throw std::runtime_error("Trophy table exceeds file bounds.");
    const auto start=static_cast<std::size_t>(offset),end=start+static_cast<std::size_t>(length);
    for(const auto& r:ranges) if(start<r.second&&r.first<end)throw std::runtime_error("Overlapping trophy tables.");
    ranges.emplace_back(start,end);
    if(type!=4&&type!=6)continue;
    if((type==4&&size!=0x50)||(type==6&&size!=0x60)||n!=expected.size())throw std::runtime_error("Incomplete or invalid trophy state table.");
    if(type==4)got4=true;else got6=true; std::set<unsigned> ids;
    for(unsigned i=0;i<n;++i) {
      const auto at=start+static_cast<std::size_t>(stride)*i;
      if(be32(bytes,at)!=type||be32(bytes,at+4)!=size||be32(bytes,at+8)!=i)throw std::runtime_error("Invalid trophy entry header.");
      const auto id=be32(bytes,at+16); auto found=expected.find(id);
      if(id!=i||found==expected.end()||!ids.insert(id).second)throw std::runtime_error("Out-of-order, unknown, or duplicate trophy state ID.");
      auto& entry=states[id]; if(type==4) {entry.grade=be32(bytes,at+20);if(entry.grade!=grade_number(found->second))throw std::runtime_error("Trophy grade does not match metadata.");}
      else {entry.state=be32(bytes,at+20);entry.timestamp=be64(bytes,at+40);if(entry.state>1)throw std::runtime_error("Invalid trophy unlock state.");}
    }
  }
  if(!got4||!got6||states.size()!=expected.size())throw std::runtime_error("Trophy state is incomplete.");
  return states;
}
} // namespace
TrophyListResult rpcs3_trophy_users(const fs::path& root) {
  try { const auto home=root/"home"; if(!directory(home)) throw std::runtime_error("RPCS3 home directory is unavailable or is a link.");
    TrophyListResult out; std::size_t seen{};
    for(const auto& entry:fs::directory_iterator(home)){if(++seen>max_dirs)throw std::runtime_error("Too many RPCS3 user entries.");auto name=entry.path().filename().string();if(user_name(name)&&directory(entry.path()))out.values.push_back(name);}
    std::sort(out.values.begin(),out.values.end());return out;
  } catch(const std::exception& e){return {{},e.what()};}
}
TrophyListResult rpcs3_trophy_sets(const fs::path& root,const std::string& user) {
  try {const auto trophy=user_dir(root,user)/"trophy"; if(!directory(trophy))throw std::runtime_error("RPCS3 trophy directory is unavailable or is a link.");
    TrophyListResult out;std::size_t seen{};for(const auto& entry:fs::directory_iterator(trophy)){if(++seen>max_dirs)throw std::runtime_error("Too many RPCS3 trophy sets.");auto name=entry.path().filename().string();if(game_name(name)&&directory(entry.path()))out.values.push_back(name);}std::sort(out.values.begin(),out.values.end());return out;
  } catch(const std::exception& e){return {{},e.what()};}
}
TrophyReadResult read_rpcs3_trophy_set(const fs::path& root,const std::string& user,const std::string& game) {
  try {
    if(!game_name(game))throw std::runtime_error("Invalid RPCS3 trophy set ID.");
    const auto trophy=user_dir(root,user)/"trophy",dir=trophy/game; if(!directory(trophy)||!directory(dir))throw std::runtime_error("RPCS3 trophy set is unavailable or is a link.");
    const auto xml=bounded_read(dir/"TROPCONF.SFM");
    if(!valid_xml_utf8(xml)||!valid_entities(xml)||xml.find("<!DOCTYPE")!=std::string::npos||xml.find("<!ENTITY")!=std::string::npos)throw std::runtime_error("Unsafe or invalid trophy XML.");
    pugi::xml_document doc;const auto parsed=doc.load_buffer(xml.data(),xml.size(),pugi::parse_default|pugi::parse_doctype,pugi::encoding_utf8);
    if(!parsed)throw std::runtime_error("Malformed trophy XML.");
    unsigned roots{};for(auto node:doc.children()){if(node.type()==pugi::node_doctype)throw std::runtime_error("DTD is not permitted.");if(node.type()==pugi::node_element)++roots;}
    auto base=doc.child("trophyconf");if(roots!=1||!base)throw std::runtime_error("Missing trophyconf root.");
    AchievementGame result;result.provider="rpcs3";result.user_id=user;result.game_id=game;result.source_directory=dir;result.title=child_text(base,"title-name",true);result.icon=optional_icon(dir,"ICON0.PNG");
    std::map<unsigned,TrophyGrade> ids;
    for(auto node:base.children("trophy")) {
      if(ids.size()>=max_entries)throw std::runtime_error("Too many trophies.");
      std::set<std::string> attributes; for(auto attr:node.attributes()) if(!attributes.insert(attr.name()).second) throw std::runtime_error("Duplicate trophy attribute.");
      const auto id=decimal(node.attribute("id").value());const auto g=grade(node.attribute("ttype").value());if(!ids.emplace(id,g).second)throw std::runtime_error("Duplicate trophy ID.");
      const auto hidden=std::string_view(node.attribute("hidden").value());if(hidden!="yes"&&hidden!="no")throw std::runtime_error("Invalid hidden flag.");
      Achievement item;item.id=std::to_string(id);item.grade=g;item.hidden=hidden=="yes";item.title=child_text(node,"name",true);item.description=child_text(node,"detail",true);
      std::ostringstream filename;filename<<"TROP"<<std::setw(3)<<std::setfill('0')<<id<<".PNG";item.icon=optional_icon(dir,filename.str());result.entries.push_back(std::move(item));
    }
    if(ids.empty())throw std::runtime_error("No trophies in metadata.");
    try {
      const auto data=bounded_read(dir/"TROPUSR.DAT");const auto states=parse_state(data,ids);
      constexpr std::uint64_t epoch=62135596800000000ull,max_tick=315537897599999999ull;
      for(auto& entry:result.entries) {const auto& state=states.at(decimal(entry.id));entry.unlock_state_known=true;entry.unlocked=state.state==1;
        if(entry.unlocked && state.timestamp>=epoch && state.timestamp<=max_tick) entry.unlocked_at=static_cast<std::int64_t>((state.timestamp-epoch)/1000000);
        else if(entry.unlocked)result.warnings.push_back("Trophy "+entry.id+" has an unavailable or invalid unlock time.");
      }
    } catch(const std::exception& e){result.warnings.push_back(std::string("Unlock state unavailable: ")+e.what());}
    return {std::move(result),{}};
  } catch(const std::exception& e){return {{},e.what()};}
}
} // namespace openxmb::desktop


