#include "openxmb/desktop/retroachievements.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs=std::filesystem;
using namespace openxmb::desktop;
void require(bool v,const char* why) {if(!v)throw std::runtime_error(why);}
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
    fs::remove_all(root);std::cout<<"RetroAchievements fixtures passed\n";return 0;
  } catch(const std::exception& e){std::cerr<<e.what()<<'\n';fs::remove_all(root);return 1;}
}

