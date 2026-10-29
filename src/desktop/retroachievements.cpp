#include "openxmb/desktop/retroachievements.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace openxmb::desktop {
namespace {
namespace fs=std::filesystem; using json=nlohmann::json;
constexpr std::size_t limit=8*1024*1024, max_achievements=10000;
constexpr std::string_view host="https://retroachievements.org";
bool valid_username(std::string_view s) {return !s.empty()&&s.size()<=32&&std::all_of(s.begin(),s.end(),[](unsigned char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-';});}
bool valid_ulid(std::string_view s) {return s.size()==26&&s[0]>='0'&&s[0]<='7'&&std::all_of(s.begin(),s.end(),[](char c){return std::string_view{"0123456789ABCDEFGHJKMNPQRSTVWXYZ"}.find(c)!=std::string_view::npos;});}
bool valid_account(const RetroAccount& a) {return valid_username(a.username)&&(a.ulid.empty()||valid_ulid(a.ulid));}
std::string identity(const RetroAccount& a) {return a.ulid.empty()?"user-"+a.username:"ulid-"+a.ulid;}
bool safe_key(std::string_view k) {return !k.empty()&&k.size()<=512&&k.find('\0')==k.npos;}
std::string encode(std::string_view value) {
  constexpr char hex[]="0123456789ABCDEF";std::string out;
  for(unsigned char c:value) if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')out+=static_cast<char>(c);else {out+='%';out+=hex[c>>4];out+=hex[c&15];}
  return out;
}
std::string url(const char* endpoint,const RetroAccount& account,const std::string& key) {
  return std::string(host)+"/API/"+endpoint+"?u="+encode(account.ulid.empty()?account.username:account.ulid)+"&y="+encode(key);
}
std::string bounded_string(const json& j,const char* key,std::size_t max=4096) {
  auto it=j.find(key);if(it==j.end()||!it->is_string())throw std::runtime_error("Invalid response field.");auto value=it->get<std::string>();if(value.size()>max||value.find('\0')!=std::string::npos)throw std::runtime_error("Invalid response text.");return value;
}
unsigned number(const json& j,const char* key,unsigned max=std::numeric_limits<unsigned>::max()) {
  auto it=j.find(key);if(it==j.end()||!it->is_number_unsigned())throw std::runtime_error("Invalid numeric response field.");auto n=it->get<std::uint64_t>();if(n>max)throw std::runtime_error("Numeric response field out of bounds.");return static_cast<unsigned>(n);
}
json parse(std::string_view text) {
  if(text.size()>limit)throw std::runtime_error("Response is too large.");
  std::map<int,std::set<std::string>> keys;bool duplicate=false;
  auto cb=[&](int depth,json::parse_event_t event,json& value) {
    if(depth>64)throw std::runtime_error("Response nesting exceeds the limit.");
    if(event==json::parse_event_t::object_start)keys[depth+1].clear();
    else if(event==json::parse_event_t::key && !keys[depth].insert(value.get<std::string>()).second)duplicate=true;
    else if(event==json::parse_event_t::object_end)keys.erase(depth+1);
    return true;
  };
  auto j=json::parse(text,cb);if(duplicate)throw std::runtime_error("Duplicate response keys.");return j;
}
std::string image_url(std::string_view path) {
  if(!path.starts_with("/Images/")||!path.ends_with(".png"))return {};
  const auto name=path.substr(8,path.size()-12);if(name.empty()||name.size()>16||!std::all_of(name.begin(),name.end(),[](char c){return c>='0'&&c<='9';}))return {};
  return std::string(host)+std::string(path);
}
std::string badge_url(std::string_view badge) {
  if(badge.empty()||badge.size()>16||!std::all_of(badge.begin(),badge.end(),[](char c){return c>='0'&&c<='9';}))throw std::runtime_error("Invalid badge ID.");
  return std::string(host)+"/Badge/"+std::string(badge)+".png";
}
std::optional<std::int64_t> timestamp(std::string_view s) {
  if(s.size()!=19||s[4]!='-'||s[7]!='-'||s[10]!=' '||s[13]!=':'||s[16]!=':')return {};
  auto part=[&](std::size_t pos,std::size_t len)->int{int x{};auto a=s.data()+pos,b=a+len;auto p=std::from_chars(a,b,x);return p.ec==std::errc{}&&p.ptr==b?x:-1;};
  const int year=part(0,4),month=part(5,2),day=part(8,2),hour=part(11,2),minute=part(14,2),second=part(17,2);
  if(year<1||hour<0||hour>23||minute<0||minute>59||second<0||second>59)return {};
  const std::chrono::year_month_day date{std::chrono::year{year},std::chrono::month{static_cast<unsigned>(month)},std::chrono::day{static_cast<unsigned>(day)}};
  if(!date.ok())return {};
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::sys_days{date}.time_since_epoch()).count()+hour*3600+minute*60+second;
}
struct DateField {bool present{},valid{true};std::optional<std::int64_t> value;};
DateField date_field(const json& j,const char* name) {
  const auto it=j.find(name);if(it==j.end())return {};
  DateField d;d.present=true;if(!it->is_string()){d.valid=false;return d;}d.value=timestamp(it->get<std::string>());d.valid=d.value.has_value();return d;
}
RetroProgressResult decode(const json& payload,const RetroAccount& account,unsigned game_id) {
  if(!payload.is_object()||number(payload,"ID")!=game_id)throw std::runtime_error("Wrong game response.");
  const auto count=number(payload,"NumAchievements",static_cast<unsigned>(max_achievements));auto it=payload.find("Achievements");
  if(it==payload.end()||(!it->is_object()&&!(it->is_array()&&it->empty()))||it->size()!=count)throw std::runtime_error("Incomplete achievement response.");
  RetroProgressResult result;AchievementGame game;game.provider="retroachievements";game.user_id=account.ulid.empty()?account.username:account.ulid;game.game_id=std::to_string(game_id);game.title=bounded_string(payload,"Title");
  if(auto icon=payload.find("ImageIcon");icon!=payload.end()&&!icon->is_null()) {if(!icon->is_string())throw std::runtime_error("Invalid artwork.");result.game_icon_url=image_url(icon->get<std::string>());if(result.game_icon_url.empty())throw std::runtime_error("Invalid artwork path.");}
  if(it->is_object())for(auto a=it->begin();a!=it->end();++a) {
    if(!a.value().is_object())throw std::runtime_error("Invalid achievement.");
    unsigned id{};const auto& k=a.key();auto p=std::from_chars(k.data(),k.data()+k.size(),id);if(k.empty()||p.ec!=std::errc{}||p.ptr!=k.data()+k.size()||!id||std::to_string(id)!=k||number(a.value(),"ID")!=id)throw std::runtime_error("Invalid achievement identity.");
    Achievement entry;entry.id=k;entry.title=bounded_string(a.value(),"Title");entry.description=bounded_string(a.value(),"Description");entry.points=number(a.value(),"Points",1000000);
    RetroProgressDetail detail;detail.id=k;detail.badge_url=badge_url(bounded_string(a.value(),"BadgeName",16));
    const auto soft=date_field(a.value(),"DateEarned"),hard=date_field(a.value(),"DateEarnedHardcore");
    if(!soft.valid||!hard.valid) {entry.unlock_state_known=false;result.warning="Some unlock dates are invalid; those states are unknown.";}
    else {entry.unlock_state_known=true;entry.unlocked=soft.present||hard.present;if(entry.unlocked){entry.hardcore=hard.present;entry.unlocked_at=soft.value?soft.value:hard.value;}detail.softcore_at=soft.value;detail.hardcore_at=hard.value;}
    game.entries.push_back(std::move(entry));result.details.push_back(std::move(detail));
  }
  const auto id_order=[](const auto& left,const auto& right) {return left.id.size()!=right.id.size()?left.id.size()<right.id.size():left.id<right.id;};
  std::sort(game.entries.begin(),game.entries.end(),id_order);
  std::sort(result.details.begin(),result.details.end(),id_order);
  result.game=std::move(game);return result;
}
std::int64_t now() {return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
json clean_payload(const json& payload) {
  json out={{"ID",payload.at("ID")},{"Title",payload.at("Title")},{"NumAchievements",payload.at("NumAchievements")},{"Achievements",json::object()}};
  if(payload.contains("ImageIcon"))out["ImageIcon"]=payload.at("ImageIcon");
  if(payload.at("Achievements").is_object())for(auto it=payload.at("Achievements").begin();it!=payload.at("Achievements").end();++it) {
    json item;for(const auto* field:{"ID","Title","Description","Points","BadgeName","DateEarned","DateEarnedHardcore"})if(it.value().contains(field))item[field]=it.value().at(field);out["Achievements"][it.key()]=std::move(item);
  }
  return out;
}
std::optional<std::string> read_cache(const fs::path& path) {
  std::error_code ec;auto st=fs::symlink_status(path,ec);if(ec||!fs::is_regular_file(st))return {};
  auto size=fs::file_size(path,ec);if(ec||size>limit)return {};std::ifstream in(path,std::ios::binary);if(!in)return {};std::string s(static_cast<std::size_t>(size),'\0');if(!s.empty()&&!in.read(s.data(),static_cast<std::streamsize>(s.size())))return {};if(in.peek()!=std::char_traits<char>::eof())return {};return s;
}
bool cache_directory(const fs::path& root,const std::string& account,bool create) {
  std::error_code ec;
  if(create){fs::create_directories(root,ec);if(ec)return false;}
  for(const auto& path:{root/"retroachievements",root/"retroachievements"/account}) {
    if(create) {fs::create_directory(path,ec);if(ec)return false;}
    const auto state=fs::symlink_status(path,ec);if(ec||!fs::is_directory(state))return false;
  }
  return true;
}
bool write_cache(const fs::path& path,std::string_view bytes) {
  static std::atomic<std::uint64_t> serial{};std::error_code ec;
  auto temp=path;temp+=".tmp-"+std::to_string(now())+"-"+std::to_string(++serial);
#ifdef _WIN32
  HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)return false;
  DWORD written{};const bool good=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file);
  CloseHandle(file);if(!good){fs::remove(temp,ec);return false;}
  if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){fs::remove(temp,ec);return false;}
#else
  const int file=open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(file<0)return false;
  std::size_t offset{};bool good=true;while(offset<bytes.size()){const auto n=write(file,bytes.data()+offset,bytes.size()-offset);if(n<=0){good=false;break;}offset+=static_cast<std::size_t>(n);}
  if(good)good=fsync(file)==0;
  if(close(file)!=0)good=false;
  if(!good){fs::remove(temp,ec);return false;}
  fs::rename(temp,path,ec);if(ec){fs::remove(temp,ec);return false;}
#endif
  return true;
}
RetroProgressResult cached(const fs::path& path,const RetroAccount& account,unsigned game,std::uint64_t max_age) {
  try {const auto bytes=read_cache(path);if(!bytes)return {};auto doc=parse(*bytes);if(!doc.is_object()||doc.value("version",0)!=1||bounded_string(doc,"account",64)!=identity(account)||number(doc,"game")!=game) return {};
    const auto fetched=doc.at("fetched_at");if(!fetched.is_number_integer())return {};const auto seconds=fetched.get<std::int64_t>();if(seconds<0||seconds>now()+300)return {};
    auto result=decode(doc.at("payload"),account,game);result.from_cache=true;result.fetched_at=seconds;result.stale=now()>seconds&&static_cast<std::uint64_t>(now()-seconds)>max_age;return result;
  } catch(...) {return {};}
}
} // namespace
RetroAccountResult retroachievements_account(const std::string& username,const std::string& key,const HttpTransport& transport) {
  if(!valid_username(username)||!safe_key(key)||!transport)return {{},"Enter a valid account name and web API key.",false};
  try {RetroAccount request{username,{}};HttpRequest query;query.url=url("API_GetUserProfile.php",request,key);query.sensitive=true;query.max_redirects=0;query.max_bytes=limit;
    const auto response=transport(query);if(response.status==429)return {{},"RetroAchievements rate limit reached. Try again later.",true};
    if(!response.success||response.status!=200)return {{},"Could not load the RetroAchievements profile.",false};
    const auto body=parse(response.body);if(!body.is_object())throw std::runtime_error("Bad profile");RetroAccount account;account.username=bounded_string(body,"User",32);
    if(body.contains("ULID")&&!body["ULID"].is_null())account.ulid=bounded_string(body,"ULID",26);
    if(!valid_account(account))throw std::runtime_error("Bad account");
    const auto lower=[](std::string value) { for(auto& c:value) if(c >= 'A' && c <= 'Z') c=static_cast<char>(c-'A'+'a'); return value; };
    if(valid_ulid(username) ? account.ulid!=username : lower(account.username)!=lower(username))throw std::runtime_error("Mismatched account");
    return {account,{},false};
  } catch(...) {return {{},"RetroAchievements returned an invalid profile.",false};}
}
RetroProgressResult retroachievements_progress(const fs::path& root,const RetroAccount& account,unsigned game_id,const std::string& key,const HttpTransport& transport,bool refresh,std::uint64_t max_age) {
  RetroProgressResult error;if(!valid_account(account)||!game_id||root.empty()){error.error="Invalid account, game ID, or cache location.";return error;}
  const auto path=root/"retroachievements"/identity(account)/(std::to_string(game_id)+".json");
  auto fallback=cache_directory(root,identity(account),false)?cached(path,account,game_id,max_age):RetroProgressResult{};
  if(!refresh||!safe_key(key)||!transport){if(fallback){fallback.warning=refresh?"Showing cached RetroAchievements data; refresh requires a web API key.":"Showing cached RetroAchievements data.";return fallback;}error.error="No cached data is available; enter a web API key to refresh.";return error;}
  try {
    HttpRequest query;query.url=url("API_GetGameInfoAndUserProgress.php",account,key)+"&g="+std::to_string(game_id);query.sensitive=true;query.max_redirects=0;query.max_bytes=limit;
    const auto response=transport(query);if(response.status==429){if(fallback){fallback.rate_limited=true;fallback.warning="Rate limit reached; showing cached data. Try again later.";return fallback;}error.error="RetroAchievements rate limit reached. Try again later.";error.rate_limited=true;return error;}
    if(!response.success||response.status!=200){if(fallback){fallback.warning="Refresh failed; showing cached data.";return fallback;}error.error="Could not load RetroAchievements progress.";return error;}
    const auto doc=parse(response.body);auto result=decode(doc,account,game_id);const auto seconds=now();result.fetched_at=seconds;
    const auto cache=json{{"version",1},{"account",identity(account)},{"game",game_id},{"fetched_at",seconds},{"payload",clean_payload(doc)}}.dump();
    if(cache.find(key)!=std::string::npos)result.warning="Progress loaded, but could not safely cache this response.";
    else if(cache.size()>limit||!cache_directory(root,identity(account),true)||!write_cache(path,cache))result.warning="Progress loaded, but could not save its offline cache.";
    return result;
  } catch(...) {if(fallback){fallback.warning="Invalid or unavailable response; showing cached data.";return fallback;}error.error="RetroAchievements returned invalid or unavailable progress.";return error;}
}
} // namespace openxmb::desktop





