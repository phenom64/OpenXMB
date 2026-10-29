#include "openxmb/desktop/retroachievements.hpp"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs=std::filesystem;
using namespace openxmb::desktop;
void require(bool v,const char* why) {if(!v)throw std::runtime_error(why);}
namespace {
constexpr const char* secret="secret-key";
constexpr const char* hash_a="f1944737edc510b22ae2910f7b190843"; // 70000-byte pattern ROM
constexpr const char* hash_million_a="7707d6ae4e027c70eea2a935c2296f21"; // MD5 of 1,000,000 x 'a'
constexpr const char* hash_zero_gb="6923685781779ac0b69c77ec08ce0479"; // 0x150 zero bytes
std::string pattern_rom(){std::string s(70000,'\0');for(std::size_t i=0;i<s.size();++i)s[i]=static_cast<char>((i*31+7)%251);return s;}
void put(const fs::path& path,const std::string& bytes){fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));}
std::string slurp(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
std::string game(unsigned id,unsigned console,const std::string& hashes){return "{\"Title\":\"Fixture "+std::to_string(id)+"\",\"ID\":"+std::to_string(id)+",\"ConsoleID\":"+std::to_string(console)+",\"ConsoleName\":\"Fixture\",\"ImageIcon\":\"/Images/000001.png\",\"NumAchievements\":3,\"NumLeaderboards\":0,\"Points\":10,\"DateModified\":\"2022-11-20 03:44:12\",\"ForumTopicID\":null,\"Hashes\":["+hashes+"]}";}
std::string list(std::initializer_list<std::string> games){std::string out="[";for(const auto& g:games){if(out.size()>1)out+=',';out+=g;}return out+"]";}
unsigned console_of(const std::string& url){const auto at=url.find("&i=");return at==url.npos?0:static_cast<unsigned>(std::stoul(url.substr(at+3)));}
void no_secret_on_disk(const fs::path& root){
  if(!fs::exists(root))return;
  for(const auto& f:fs::recursive_directory_iterator(root))if(f.is_regular_file())require(slurp(f.path()).find(secret)==std::string::npos&&f.path().string().find(secret)==std::string::npos,"credential leaked to disk");
}
void set_fetched(const fs::path& file,long long seconds){auto data=slurp(file);const auto start=data.find("\"fetched_at\":");require(start!=std::string::npos,"fetch time missing");const auto begin=start+13,end=data.find(',',begin);data.replace(begin,end-begin,std::to_string(seconds));put(file,data);}

void hashing_tests(const fs::path& dir) {
  const auto million=dir/"million.GBA";put(million,std::string(1000000,'a'));
  auto h=retroachievements_hash_rom(million);require(bool(h)&&h.value->md5==hash_million_a&&h.value->format==RetroRomFormat::game_boy_advance&&h.value->size==1000000,"million-a vector (multi-chunk, upper-case extension)");
  put(dir/"pattern.gb",pattern_rom());h=retroachievements_hash_rom(dir/"pattern.gb");require(bool(h)&&h.value->md5==hash_a&&h.value->format==RetroRomFormat::game_boy,"pattern ROM vector");
  put(dir/"zero.gbc",std::string(0x150,'\0'));h=retroachievements_hash_rom(dir/"zero.gbc");require(bool(h)&&h.value->md5==hash_zero_gb&&h.value->format==RetroRomFormat::game_boy_color,"minimal GBC vector");
  put(dir/"tiny.gba",std::string(0xC0,'\0'));h=retroachievements_hash_rom(dir/"tiny.gba");require(bool(h)&&h.value->md5=="b7dd5e0194ee0ac08a4b802cb73d867f","minimal GBA vector");
  // Unsupported formats are rejected by extension before any file access.
  for(const char* name:{"a.nes","a.sfc","a.smc","a.iso","a.chd","a.cue","a.zip","a.7z","a.rar","a.bin","a.gbax","a.sgb","a.gba.zip","a","a.pkg","a.exe"}){
    h=retroachievements_hash_rom(dir/name);require(!h&&h.status==RetroRomHashStatus::unsupported,name);
  }
  // Size bounds per format.
  put(dir/"short.gb",std::string(0x14F,'\0'));require(retroachievements_hash_rom(dir/"short.gb").status==RetroRomHashStatus::invalid,"short GB accepted");
  put(dir/"short.gba",std::string(0xBF,'\0'));require(retroachievements_hash_rom(dir/"short.gba").status==RetroRomHashStatus::invalid,"short GBA accepted");
  put(dir/"empty.gba","");require(retroachievements_hash_rom(dir/"empty.gba").status==RetroRomHashStatus::invalid,"empty ROM accepted");
  put(dir/"max.gb",std::string(8*1024*1024,'\0'));require(bool(retroachievements_hash_rom(dir/"max.gb")),"8 MiB GB rejected");
  put(dir/"big.gb",std::string(8*1024*1024+1,'\0'));require(retroachievements_hash_rom(dir/"big.gb").status==RetroRomHashStatus::invalid,"oversized GB accepted");
  put(dir/"big.gba",std::string(32*1024*1024+1,'\0'));require(retroachievements_hash_rom(dir/"big.gba").status==RetroRomHashStatus::invalid,"oversized GBA accepted");
  fs::create_directories(dir/"folder.gba");require(retroachievements_hash_rom(dir/"folder.gba").status==RetroRomHashStatus::unreadable,"directory hashed");
  require(retroachievements_hash_rom(dir/"missing.gba").status==RetroRomHashStatus::unreadable,"missing file hashed");
  // Cancellation, before and between chunks.
  require(retroachievements_hash_rom(million,[]{return true;}).status==RetroRomHashStatus::cancelled,"pre-cancel ignored");
  int polls=0;require(retroachievements_hash_rom(million,[&]{return ++polls>=3;}).status==RetroRomHashStatus::cancelled&&polls==3,"mid-hash cancellation ignored");
  // Source change during hashing: append, in-place timestamp change and replacement all fail.
  put(dir/"live.gba",std::string(300000,'x'));polls=0;
  auto result=retroachievements_hash_rom(dir/"live.gba",[&]{if(++polls==2){std::ofstream out(dir/"live.gba",std::ios::binary|std::ios::app);out<<"more";}return false;});
  require(!result&&result.status==RetroRomHashStatus::changed,"appended source accepted");
  put(dir/"live.gba",std::string(300000,'x'));polls=0;
  result=retroachievements_hash_rom(dir/"live.gba",[&]{if(++polls==2)fs::last_write_time(dir/"live.gba",fs::last_write_time(dir/"live.gba")+std::chrono::hours(1));return false;});
  require(!result&&result.status==RetroRomHashStatus::changed,"touched source accepted");
  put(dir/"live.gba",std::string(300000,'x'));polls=0;
  result=retroachievements_hash_rom(dir/"live.gba",[&]{if(++polls==2)put(dir/"live.gba",std::string(100000,'y'));return false;});
  require(!result&&result.status==RetroRomHashStatus::changed,"truncated source accepted");
  put(dir/"live.gba",std::string(300000,'x'));require(bool(retroachievements_hash_rom(dir/"live.gba",[]{return false;})),"stable source rejected");
}

void matching_tests(const fs::path& base) {
  const auto cache=base/"cache",roms=base/"roms";
  const auto rom_a=roms/"a.gba",rom_a_gb=roms/"same-bytes.gb",rom_b=roms/"b.gba",rom_c=roms/"c.gba";
  const auto rom_gbc=roms/"d.gbc";put(rom_a,pattern_rom());put(rom_a_gb,pattern_rom());put(rom_b,std::string(1000,'b'));put(rom_c,std::string(1000,'c'));put(rom_gbc,std::string(0x150,'\0'));
  const std::string hash_b="6b0d01bb52f9ab0a1d1a6a6c4eb1d6e5"; // listed below for a different file
  std::vector<HttpRequest> requests;std::map<unsigned,std::string> lists;long status=200;bool success=true;
  lists[5]=list({game(1001,5,std::string("\"")+hash_b+"\""),game(14402,5,std::string("\"")+"F1944737EDC510B22AE2910F7B190843"+"\",\""+"00000000000000000000000000000001\"")});
  lists[6]=list({game(2001,6,"\"00000000000000000000000000000002\"")});lists[4]=list({game(3001,4,"\"00000000000000000000000000000003\"")});
  HttpTransport transport=[&](const HttpRequest& req){requests.push_back(req);const auto found=lists.find(console_of(req.url));return HttpResponse{success,status,found==lists.end()?std::string{}:found->second,secret};};
  const RetroAccount alice{"Alice","00003EMFWR7XB8SDPEHB3K56ZQ"},bob{"Bob","00003EMFWR7XB8SDPEHB3K56ZR"},bob_name{"Bob",""};
  const std::vector<fs::path> paths{rom_a,rom_b,roms/"game.nes",roms/"missing.gba",rom_a_gb};
  auto r=retroachievements_match(cache,alice,secret,paths,transport);
  require(r.matched==1&&r.unmatched==2&&r.unsupported==1&&r.unavailable==1&&r.skipped==0&&r.items.size()==5,"mixed result counts");
  require(r.items[0].status==RetroMatchStatus::matched&&r.items[0].game_id==14402,"exact hash match (upper-case provider hash normalised)");
  require(r.items[1].status==RetroMatchStatus::unmatched&&r.items[1].game_id==0,"unlisted hash must not match");
  require(r.items[2].status==RetroMatchStatus::unsupported&&r.items[3].status==RetroMatchStatus::unavailable,"unsupported and missing statuses");
  require(r.items[4].status==RetroMatchStatus::unmatched&&r.items[4].game_id==0,"a .gb file must not match another console's list (same bytes, console 4)");
  require(requests.size()==2,"one request per console, none per ROM");
  for(const auto& q:requests){require(q.sensitive&&q.max_redirects==0&&q.max_bytes<=8u*1024u*1024u,"transport policy");
    require(q.url.rfind("https://retroachievements.org/API/API_GetGameList.php?",0)==0&&q.url.find("&h=1&f=1")!=std::string::npos&&q.url.find("y=secret-key")!=std::string::npos,"official list request shape");
    require(q.url.find("roms")==std::string::npos&&q.url.find("a.gba")==std::string::npos&&q.url.find(hash_a)==std::string::npos,"ROM names or hashes sent to the provider");}
  require(requests[0].url.find("i=5")!=std::string::npos&&requests[1].url.find("i=4")!=std::string::npos,"console 5 then 4 queried");
  require(r.error.empty()&&r.warning.empty(),"unexpected provider error");
  no_secret_on_disk(base);
  require(fs::exists(cache/"retroachievements"/("ulid-"+alice.ulid)/"hashes-4.json")&&fs::exists(cache/"retroachievements"/("ulid-"+alice.ulid)/"hashes-5.json")&&!fs::exists(cache/"retroachievements"/("ulid-"+alice.ulid)/"hashes-6.json"),"one cache file per queried console");

  // Positive and negative results are reused without requests, online or offline.
  requests.clear();r=retroachievements_match(cache,alice,secret,{rom_a,rom_b},transport);
  require(requests.empty()&&r.matched==1&&r.unmatched==1&&r.from_cache&&!r.stale,"fresh cache must not call the provider");
  r=retroachievements_match(cache,alice,"",{rom_a,rom_b},transport,{},false);require(requests.empty()&&r.matched==1&&r.unmatched==1,"offline matching");

  // Stale list: still used, flagged, refreshed only when a key is available.
  const auto file=cache/"retroachievements"/("ulid-"+alice.ulid)/"hashes-5.json";set_fetched(file,1);
  r=retroachievements_match(cache,alice,"",{rom_a},transport,{},true,60);require(requests.empty()&&r.matched==1&&r.stale&&r.items[0].stale&&!r.warning.empty(),"stale offline match not identified");
  r=retroachievements_match(cache,alice,secret,{rom_a},transport,{},false,60);require(requests.empty()&&r.items[0].stale,"refresh=false must not contact the provider");
  status=429;r=retroachievements_match(cache,alice,secret,{rom_a,rom_gbc},transport,{},true,60);
  require(requests.size()==1&&r.rate_limited&&r.matched==1&&r.stale&&r.items[0].stale&&r.items[1].status==RetroMatchStatus::unavailable,"429: stale fallback, no request for the next console");
  require(r.warning.find(secret)==std::string::npos&&r.error.find(secret)==std::string::npos,"429 text leaked key");
  status=200;requests.clear();r=retroachievements_match(cache,alice,secret,{rom_a},transport,{},true,60);require(requests.size()==1&&r.matched==1&&!r.stale&&!r.from_cache,"stale list was not refreshed");
  success=false;status=500;set_fetched(file,1);requests.clear();r=retroachievements_match(cache,alice,secret,{rom_a},transport,{},true,60);
  require(requests.size()==1&&r.matched==1&&r.stale&&!r.warning.empty(),"server failure should use the stale list honestly");
  success=true;status=200;

  // Account and profile-cache separation.
  requests.clear();r=retroachievements_match(cache,bob,"",{rom_a},transport,{},false);require(requests.empty()&&r.unavailable==1&&r.matched==0,"another ULID reused a hash list");
  r=retroachievements_match(cache,bob_name,"",{rom_a},transport,{},false);require(requests.empty()&&r.unavailable==1,"a name-only account reused a ULID list");
  fs::create_directories(cache/"retroachievements"/("ulid-"+bob.ulid));fs::copy_file(file,cache/"retroachievements"/("ulid-"+bob.ulid)/"hashes-5.json");
  r=retroachievements_match(cache,bob,"",{rom_a},transport,{},false);require(r.unavailable==1&&r.matched==0,"a copied cache file for a different account was accepted");
  const auto other_profile=base/"other-profile-cache";r=retroachievements_match(other_profile,alice,"",{rom_a},transport,{},false);require(r.unavailable==1&&!fs::exists(other_profile),"another profile cache root was read or created");
  r=retroachievements_match(cache,alice,"",{rom_a},transport,{},false);require(r.matched==1,"original account cache lost");

  // Provider payload validation: every malformed response is rejected and never cached.
  const auto h5="\"00000000000000000000000000000005\"";
  const std::vector<std::pair<const char*,std::string>> bad{
    {"not JSON","{not-json"},{"failure object","{\"success\":false}"},{"empty list","[]"},
    {"wrong console",list({game(7,6,h5)})},{"zero game ID",list({game(0,5,h5)})},{"duplicate game ID",list({game(7,5,h5),game(7,5,"")})},
    {"short hash",list({game(7,5,"\"abcd\"")})},{"non-hex hash",list({game(7,5,"\"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\"")})},{"non-string hash",list({game(7,5,"1234")})},
    {"duplicate JSON keys","[{\"ID\":7,\"ID\":7,\"ConsoleID\":5,\"Hashes\":[]}]"},{"negative ID","[{\"ID\":-7,\"ConsoleID\":5,\"Hashes\":[]}]"},
    {"string ID","[{\"ID\":\"7\",\"ConsoleID\":5,\"Hashes\":[]}]"},{"missing hashes","[{\"ID\":7,\"ConsoleID\":5}]"},{"hashes not array","[{\"ID\":7,\"ConsoleID\":5,\"Hashes\":\"x\"}]"},
    {"entries not objects","[1,2]"},{"object root","{\"7\":{\"ID\":7}}"}};
  for(const auto& [why,body]:bad){
    const auto fresh=base/"bad-cache";fs::remove_all(fresh);lists[5]=body;requests.clear();
    r=retroachievements_match(fresh,alice,secret,{rom_a},transport);require(r.unavailable==1&&r.matched==0&&r.items[0].game_id==0&&requests.size()==1,why);
    require(!fs::exists(fresh/"retroachievements"/("ulid-"+alice.ulid)/"hashes-5.json"),why);no_secret_on_disk(fresh);
  }
  // Same hash under two games is ambiguous: never resolved by guessing.
  lists[5]=list({game(11,5,std::string("\"")+hash_a+"\""),game(12,5,std::string("\"")+hash_a+"\"")});
  fs::remove_all(base/"amb");r=retroachievements_match(base/"amb",alice,secret,{rom_a},transport);require(r.unmatched==1&&r.matched==0&&r.items[0].game_id==0&&!r.items[0].message.empty(),"ambiguous hash was resolved");
  r=retroachievements_match(base/"amb",alice,"",{rom_a},transport,{},false);require(r.unmatched==1&&r.items[0].game_id==0,"ambiguity lost in the cache");
  // A hash repeated within a single game is not ambiguous.
  lists[5]=list({game(13,5,std::string("\"")+hash_a+"\",\""+hash_a+"\"")});fs::remove_all(base/"dup");
  r=retroachievements_match(base/"dup",alice,secret,{rom_a},transport);require(r.matched==1&&r.items[0].game_id==13,"repeated hash within one game rejected");

  // Corrupt or tampered cache files are ignored rather than trusted.
  const auto tamper=base/"dup"/"retroachievements"/("ulid-"+alice.ulid)/"hashes-5.json";
  put(tamper,"{\"version\":1,\"account\":\"ulid-"+alice.ulid+"\",\"console\":5,\"fetched_at\":4102444800,\"hashes\":{\"nothex\":1}}");
  r=retroachievements_match(base/"dup",alice,"",{rom_a},transport,{},false);require(r.unavailable==1,"invalid cached hash accepted");
  put(tamper,"{\"version\":1,\"account\":\"ulid-"+alice.ulid+"\",\"console\":6,\"fetched_at\":1,\"hashes\":{}}");
  r=retroachievements_match(base/"dup",alice,"",{rom_a},transport,{},false);require(r.unavailable==1,"cache for another console accepted");

  // 429 and hard failure without a cache: explicit, key-free errors; nothing is cached.
  status=429;fs::remove_all(base/"none");r=retroachievements_match(base/"none",alice,secret,{rom_a},transport);require(r.rate_limited&&r.unavailable==1&&r.error.find(secret)==std::string::npos&&!r.items[0].message.empty(),"429 without cache");
  status=200;success=false;fs::remove_all(base/"none");r=retroachievements_match(base/"none",alice,secret,{rom_a},transport);require(r.unavailable==1&&r.error.find(secret)==std::string::npos,"transport failure without cache");
  success=true;lists[5]=list({game(14402,5,std::string("\"")+hash_a+"\"")});
  r=retroachievements_match(base/"none",alice,secret,{rom_a},transport);require(r.matched==1,"a failed request must not poison later matching");
  // No key and no cache: the provider is never contacted.
  requests.clear();fs::remove_all(base/"none");r=retroachievements_match(base/"none",alice,"",{rom_a},transport);require(requests.empty()&&r.unavailable==1,"requested without a key");
  r=retroachievements_match(base/"none",alice,secret,{rom_a},HttpTransport{});require(r.unavailable==1,"missing transport");
  r=retroachievements_match({},alice,secret,{rom_a},transport);require(!r.error.empty()&&r.items.empty(),"empty cache location accepted");
  r=retroachievements_match(cache,RetroAccount{"bad name",""},secret,{rom_a},transport);require(!r.error.empty()&&r.items.empty(),"invalid account accepted");
  // Local files that cannot be hashed never reach the provider.
  requests.clear();r=retroachievements_match(base/"none",alice,secret,{roms/"game.nes",roms/"missing.gba"},transport);require(requests.empty()&&r.unsupported==1&&r.unavailable==1,"requested with nothing to look up");

  // Cancellation is reported and stops lookups.
  requests.clear();r=retroachievements_match(base/"none",alice,secret,{rom_a},transport,[]{return true;});require(r.cancelled&&requests.empty()&&r.matched==0,"cancelled match continued");
  int polls=0;fs::remove_all(base/"none");r=retroachievements_match(base/"none",alice,secret,{rom_a},transport,[&]{return ++polls>=4;});require(r.cancelled&&r.matched==0,"cancel before lookup ignored");

  // The per-run limit bounds local hashing and the extras are reported.
  std::vector<fs::path> many;for(unsigned i=0;i<retroachievements_match_limit+2;++i){many.push_back(roms/("many"+std::to_string(i)+".gba"));put(many.back(),std::string(0xC0,static_cast<char>('A'+(i%26)))+std::to_string(i));}
  requests.clear();fs::remove_all(base/"limit");r=retroachievements_match(base/"limit",alice,secret,many,transport);
  require(r.items.size()==many.size()&&r.skipped==2&&r.unmatched==retroachievements_match_limit&&requests.size()==1&&r.items[64].status==RetroMatchStatus::skipped,"per-run limit");
  no_secret_on_disk(base);
}
}
int main() {
  const auto root=fs::temp_directory_path()/"openxmb-retro-provider-test";
  try {
    fs::remove_all(root); std::vector<HttpRequest> requests;
    std::string profile=R"({"User":"Alice","ULID":"00003EMFWR7XB8SDPEHB3K56ZQ"})";
    std::string body=R"({"ID":14402,"Title":"Fixture","ImageIcon":"/Images/026368.png","NumAchievements":2,"Achievements":{"9":{"ID":9,"Title":"First","Description":"Do it","Points":3,"BadgeName":"250336","DateEarnedHardcore":"2024-01-02 03:04:05"},"10":{"ID":10,"Title":"Second","Description":"Wait","Points":5,"BadgeName":"12345"}}})";
    HttpTransport transport=[&](const HttpRequest& req){requests.push_back(req);return HttpResponse{true,200,req.url.find("UserProfile")!=std::string::npos?profile:body,{}};};
    auto account=retroachievements_account("Alice","secret-key",transport); require(bool(account)&&account.value->ulid=="00003EMFWR7XB8SDPEHB3K56ZQ","profile or ULID");
    require(requests[0].sensitive&&requests[0].max_redirects==0,"credential request policy");
    auto progress=retroachievements_progress(root,*account.value,14402,"secret-key",transport); require(bool(progress),progress.error.c_str());
    require(progress.game->entries.size()==2&&progress.game->entries[0].grade==TrophyGrade::none&&progress.game->entries[0].points==3,"achievement metadata");
    require(progress.game->entries[0].unlocked&&progress.game->entries[0].hardcore==true&&progress.details[0].hardcore_at==1704164645&&!progress.details[0].softcore_at,"hardcore-only unlock");
    require(!progress.game->entries[1].unlocked&&progress.game->entries[1].unlock_state_known,"locked state");
    require(progress.game_icon_url=="https://retroachievements.org/Images/026368.png"&&progress.details[0].badge_url=="https://retroachievements.org/Badge/250336.png","official artwork URLs");
    require(requests.size()==2&&requests[1].sensitive&&requests[1].max_redirects==0,"game request policy");
    for(const auto& f:fs::recursive_directory_iterator(root)) if(f.is_regular_file()) {std::ifstream in(f.path(),std::ios::binary);std::string contents{std::istreambuf_iterator<char>(in),{}};require(contents.find("secret-key")==std::string::npos&&f.path().string().find("secret-key")==std::string::npos,"credential leaked to cache");}
    const auto count=requests.size();auto cached=retroachievements_progress(root,*account.value,14402,"",transport);require(bool(cached)&&cached.from_cache&&requests.size()==count,"offline cache must not call transport");
    RetroAccount other{"Alice","00003EMFWR7XB8SDPEHB3K56ZR"};require(!retroachievements_progress(root,other,14402,"",transport),"different ULID reused cache");
    HttpTransport rate=[](const HttpRequest&){return HttpResponse{false,429,{},"secret-key"};};
    cached=retroachievements_progress(root,*account.value,14402,"secret-key",rate);require(bool(cached)&&cached.from_cache&&cached.rate_limited&&cached.warning.find("secret-key")==std::string::npos,"429 cache fallback");
    fs::remove_all(root); body=R"({"ID":14402,"Title":"Bad","NumAchievements":1,"Achievements":{"9":{"ID":9,"Title":"Bad","Description":"Bad","Points":-3,"BadgeName":"12345"}}})";
    require(!retroachievements_progress(root,*account.value,14402,"secret-key",transport),"negative points accepted");
    body=R"({"ID":14402,"Title":"Bad date","NumAchievements":1,"Achievements":{"9":{"ID":9,"Title":"Bad","Description":"Bad","Points":3,"BadgeName":"12345","DateEarned":"2024-02-30 00:00:00"}}})";
    progress=retroachievements_progress(root,*account.value,14402,"secret-key",transport);require(bool(progress)&&!progress.game->entries[0].unlock_state_known&&!progress.game->entries[0].unlocked_at&&!progress.warning.empty(),"invalid date must be unknown");
    fs::remove_all(root);body="{not-json";require(!retroachievements_progress(root,*account.value,14402,"secret-key",transport),"malformed payload accepted");
    body=R"({"ID":14402,"ID":14402,"Title":"Bad","NumAchievements":0,"Achievements":{}})";require(!retroachievements_progress(root,*account.value,14402,"secret-key",transport),"duplicate JSON keys accepted");
    body=R"({"ID":14403,"Title":"Bad","NumAchievements":0,"Achievements":{}})";require(!retroachievements_progress(root,*account.value,14402,"secret-key",transport),"wrong game ID accepted");
    body=R"({"ID":14402,"Title":"Bad","ImageIcon":"https://evil.test/image.png","NumAchievements":0,"Achievements":{}})";require(!retroachievements_progress(root,*account.value,14402,"secret-key",transport),"untrusted artwork URL accepted");
    body=R"({"ID":14402,"Title":"Fixture","NumAchievements":1,"Achievements":{"9":{"ID":9,"Title":"First","Description":"Do it","Points":3,"BadgeName":"250336","DateEarned":"2023-01-01 00:00:00","DateEarnedHardcore":"2024-01-02 03:04:05"}}})";
    progress=retroachievements_progress(root,*account.value,14402,"secret-key",transport);require(bool(progress)&&progress.details[0].softcore_at==1672531200&&progress.details[0].hardcore_at==1704164645,"separate UTC dates");
    const auto cachefile=root/"retroachievements"/("ulid-"+account.value->ulid)/"14402.json";{std::ifstream in(cachefile);std::string data{std::istreambuf_iterator<char>(in),{}};const auto start=data.find("\"fetched_at\":");require(start!=std::string::npos,"cache time missing");const auto begin=start+13,end=data.find(',',begin);data.replace(begin,end-begin,"1");std::ofstream out(cachefile,std::ios::trunc);out<<data;}
    cached=retroachievements_progress(root,*account.value,14402,"",transport,true,60);require(bool(cached)&&cached.stale,"stale cache not identified");
    fs::remove_all(root); auto limited=retroachievements_progress(root,*account.value,14402,"secret-key",rate);require(!limited&&limited.rate_limited&&limited.error.find("secret-key")==std::string::npos,"429 without cache");
    profile=R"({"User":"Bob","ULID":"00003EMFWR7XB8SDPEHB3K56ZQ"})";require(!retroachievements_account("Alice","secret-key",transport),"mismatched account accepted");
    fs::create_directories(root);std::error_code link_error;fs::create_directory_symlink(fs::temp_directory_path(),root/"retroachievements",link_error);
    if(!link_error){progress=retroachievements_progress(root,*account.value,14402,"secret-key",transport);require(bool(progress)&&!progress.warning.empty(),"linked cache directory must not be written");}
    require(!retroachievements_account("Alice","",transport),"empty key accepted"); require(!retroachievements_progress(root,*account.value,0,"secret-key",transport),"invalid game ID accepted");
    hashing_tests(root/"hash");matching_tests(root/"match");
    fs::remove_all(root);std::cout<<"RetroAchievements fixtures passed\n";return 0;
  } catch(const std::exception& e){std::cerr<<e.what()<<'\n';fs::remove_all(root);return 1;}
}

