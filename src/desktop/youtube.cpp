#include "openxmb/desktop/youtube.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
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
#include <bcrypt.h>
#include <archive.h>
#include <archive_entry.h>
#endif
namespace openxmb::desktop {
namespace {
namespace fs=std::filesystem; using json=nlohmann::json;
constexpr std::uint64_t max_download=256ull*1024*1024;
constexpr std::string_view marker="OpenXMB managed YouTube installation v1\n";
bool tag_valid(std::string_view s){return !s.empty()&&s.size()<=64&&s!="."&&s!=".."&&std::all_of(s.begin(),s.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c=='_';});}
bool digest_valid(std::string_view s){return s.size()==64&&std::all_of(s.begin(),s.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
bool file(const fs::path& p){std::error_code e;auto s=fs::symlink_status(p,e);return !e&&fs::is_regular_file(s);}
bool directory(const fs::path& p){std::error_code e;auto s=fs::symlink_status(p,e);return !e&&fs::is_directory(s);}
std::string utf8(const fs::path& p){auto value=p.u8string();return {value.begin(),value.end()};}
std::string read_small(const fs::path& path,std::size_t max=128){if(!file(path))return {};std::error_code e;auto size=fs::file_size(path,e);if(e||size>max)return {};std::ifstream input(path,std::ios::binary);std::string data(static_cast<std::size_t>(size),'\0');if(!data.empty()&&!input.read(data.data(),static_cast<std::streamsize>(data.size())))return {};return data;}
[[maybe_unused]] std::optional<std::pair<fs::path,std::string>> managed_exe(const fs::path& root) {
  if(root.empty())return {};
  const auto a=root/"applications",b=a/"youtube",m=b/"openxmb-managed",versions=m/"versions";
  if(!directory(a)||!directory(b)||!directory(m)||!directory(versions)||read_small(m/"owner.txt")!=marker)return {};
  const auto current=read_small(m/"current.txt");if(!tag_valid(current))return {};
  auto folder=versions/fs::path(current);if(!directory(folder))return {};const auto exe=folder/"VacuumTube.exe";return file(exe)?std::optional<std::pair<fs::path,std::string>>{{exe,current}}:std::nullopt;
}
std::string executable_version(const fs::path& exe) {
#ifdef _WIN32
  DWORD ignored{};const auto size=GetFileVersionInfoSizeW(exe.c_str(),&ignored);if(size==0||size>4u*1024*1024)return {};
  std::vector<unsigned char> data(size);if(!GetFileVersionInfoW(exe.c_str(),0,size,data.data()))return {};
  VS_FIXEDFILEINFO* info{};UINT length{};if(!VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&info),&length)||!info||length<sizeof(VS_FIXEDFILEINFO)||info->dwSignature!=0xfeef04bd)return {};
  if(info->dwFileVersionMS==0&&info->dwFileVersionLS==0)return {};
  return std::to_string(HIWORD(info->dwFileVersionMS))+"."+std::to_string(LOWORD(info->dwFileVersionMS))+"."+std::to_string(HIWORD(info->dwFileVersionLS))+"."+std::to_string(LOWORD(info->dwFileVersionLS));
#else
  (void)exe;return {};
#endif
}
YoutubeApplication application(const fs::path& exe,bool managed=false,std::string version={}) {
  if(!managed)version=executable_version(exe);
  return {exe,{utf8(exe),"--fullscreen"},TrackingQuality::launcher_only,managed,"A running instance may receive this launch; its lifetime cannot be owned reliably.",std::move(version)};
}
std::string asset_for(YoutubePlatform p,YoutubeArch a){
  if(a==YoutubeArch::unknown)return {};
  if(p==YoutubePlatform::windows)return a==YoutubeArch::x64?"VacuumTube-x64-Portable.zip":"VacuumTube-arm64-Portable.zip";
  if(p==YoutubePlatform::linux)return a==YoutubeArch::x64?"VacuumTube-x86_64.AppImage":"VacuumTube-arm64.AppImage";
  return a==YoutubeArch::x64?"VacuumTube-x64.dmg":"VacuumTube-arm64.dmg";
}
bool release_valid(const YoutubeRelease& r){return tag_valid(r.tag)&&digest_valid(r.sha256)&&r.asset_name==asset_for(r.platform,r.arch)&&r.size>0&&r.size<=max_download&&r.url=="https://github.com/shy1132/VacuumTube/releases/download/"+r.tag+"/"+r.asset_name;}
#ifdef _WIN32
void notify(const YoutubeProgress& cb,std::string_view stage){if(cb&&!cb(stage,0,0))throw std::runtime_error("Setup was cancelled.");}
struct Algorithm {BCRYPT_ALG_HANDLE h{};~Algorithm(){if(h)BCryptCloseAlgorithmProvider(h,0);}};
struct Hash {BCRYPT_HASH_HANDLE h{};~Hash(){if(h)BCryptDestroyHash(h);}};
std::string sha256(const std::string& data) {
  Algorithm a;Hash h;DWORD size{},used{};if(BCryptOpenAlgorithmProvider(&a.h,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0||BCryptGetProperty(a.h,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&used,0)<0||size>1024*1024)throw std::runtime_error("SHA-256 is unavailable.");
  std::vector<unsigned char> object(size),result(32);if(BCryptCreateHash(a.h,&h.h,object.data(),size,nullptr,0,0)<0||BCryptHashData(h.h,reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),static_cast<ULONG>(data.size()),0)<0||BCryptFinishHash(h.h,result.data(),32,0)<0)throw std::runtime_error("SHA-256 failed.");
  const char* hex="0123456789abcdef";std::string out;out.reserve(64);for(auto c:result){out+=hex[c>>4];out+=hex[c&15];}return out;
}
bool valid_component(std::string_view c) {
  if(c.empty()||c=="."||c==".."||c.size()>200||c.back()=='.'||c.back()==' ')return false;
  for(unsigned char x:c)if(x<32||x>=127||std::string_view{"<>:\"\\|?*"}.find(static_cast<char>(x))!=std::string_view::npos)return false;
  auto stem=std::string(c.substr(0,c.find('.')));for(auto& x:stem)if(x>='a'&&x<='z')x=static_cast<char>(x-'a'+'A');
  if(stem=="CON"||stem=="PRN"||stem=="AUX"||stem=="NUL"||stem=="CONIN$"||stem=="CONOUT$")return false;
  if(stem.size()==4&&(stem.starts_with("COM")||stem.starts_with("LPT"))&&stem[3]>='1'&&stem[3]<='9')return false;
  return true;
}
std::vector<std::string> components(std::string_view name) {
  if(name.empty()||name.size()>1024||name.front()=='/'||name.front()=='\\')throw std::runtime_error("The ZIP contains an invalid path.");
  if(name.back()=='/')name.remove_suffix(1);std::vector<std::string> out;
  while(!name.empty()){auto slash=name.find('/');auto part=name.substr(0,slash);if(!valid_component(part))throw std::runtime_error("The ZIP contains an unsafe path.");out.emplace_back(part);if(out.size()>20)throw std::runtime_error("The ZIP path is too deep.");if(slash==name.npos)break;name.remove_prefix(slash+1);}
  if(out.empty())throw std::runtime_error("The ZIP contains an empty path.");return out;
}
void make_directory(const fs::path& p){std::error_code e;if(!fs::create_directory(p,e)&&e)throw std::runtime_error("Cannot create an installation directory.");if(!directory(p))throw std::runtime_error("Installation directory is not a safe directory.");}
void discard_stage(const fs::path& stage,const fs::path& managed) noexcept {
  std::error_code ec;const auto parent=fs::weakly_canonical(managed,ec);if(ec)return;
  const auto target=fs::weakly_canonical(stage,ec);if(ec||target.parent_path()!=parent||!stage.filename().string().starts_with("stage-")||!directory(stage))return;
  fs::remove_all(stage,ec);
}
void discard_promoted(const fs::path& path,const fs::path& versions,const std::string& tag) noexcept {
  std::error_code ec;const auto parent=fs::weakly_canonical(versions,ec);if(ec)return;
  const auto target=fs::weakly_canonical(path,ec);if(ec||target.parent_path()!=parent||path.filename().string()!=tag||!tag_valid(tag)||!directory(path))return;
  fs::remove_all(path,ec);
}
void extract(const std::string& bytes,const fs::path& stage) {
  auto* ar=archive_read_new();if(!ar)throw std::runtime_error("Cannot initialize ZIP reader.");
  struct Guard {archive* a;~Guard(){archive_read_free(a);}} guard{ar};archive_read_support_format_zip(ar);if(archive_read_open_memory(ar,bytes.data(),bytes.size())!=ARCHIVE_OK)throw std::runtime_error("Invalid ZIP archive.");
  std::set<std::string> seen;std::map<std::string,std::string> spelling;std::uint64_t total{};std::size_t entries{};archive_entry* entry{};int status{};
  while((status=archive_read_next_header(ar,&entry))==ARCHIVE_OK) {
    if(++entries>40000||archive_entry_symlink(entry)||archive_entry_hardlink(entry)||archive_entry_is_encrypted(entry)>0)throw std::runtime_error("ZIP contains unsupported links, encryption, or too many entries.");
    const auto kind=archive_entry_filetype(entry);if(kind!=AE_IFREG&&kind!=AE_IFDIR)throw std::runtime_error("ZIP contains a special file.");
    const char* raw=archive_entry_pathname_utf8(entry);if(!raw)throw std::runtime_error("ZIP contains an invalid filename.");auto parts=components(raw);std::string key;
    std::string original;
    for(const auto& p:parts){if(!key.empty()){key+='/';original+='/';}original+=p;for(char c:p)key+=(c>='A'&&c<='Z')?static_cast<char>(c-'A'+'a'):c;
      auto [found,inserted]=spelling.emplace(key,original);if(!inserted&&found->second!=original)throw std::runtime_error("ZIP contains case-colliding paths.");}
    if(!seen.insert(key).second)throw std::runtime_error("ZIP contains duplicate paths.");
    if(!archive_entry_size_is_set(entry)||archive_entry_size(entry)<0||static_cast<std::uint64_t>(archive_entry_size(entry))>512ull*1024*1024)throw std::runtime_error("ZIP entry has an invalid size.");
    const auto size=static_cast<std::uint64_t>(archive_entry_size(entry));if(size>1024ull*1024*1024-total)throw std::runtime_error("ZIP expands beyond the limit.");total+=size;
    fs::path path=stage;for(std::size_t i=0;i<parts.size();++i){path/=parts[i];if(i+1<parts.size()||kind==AE_IFDIR)make_directory(path);}
    if(kind==AE_IFDIR){if(size!=0)throw std::runtime_error("ZIP directory has unexpected data.");continue;}
    std::error_code ec;if(fs::exists(path,ec)||ec)throw std::runtime_error("ZIP path collides with another entry.");
    std::ofstream out(path,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("Cannot write an extracted file.");
    char buffer[32768];std::uint64_t written{};la_ssize_t n{};while((n=archive_read_data(ar,buffer,sizeof(buffer)))>0){if(static_cast<std::uint64_t>(n)>size-written)throw std::runtime_error("ZIP entry exceeded its declared size.");out.write(buffer,n);if(!out)throw std::runtime_error("Cannot write an extracted file.");written+=static_cast<std::uint64_t>(n);}
    if(n<0||written!=size)throw std::runtime_error("ZIP entry is incomplete or corrupt.");
  }
  if(status!=ARCHIVE_EOF||entries==0||!file(stage/"VacuumTube.exe"))throw std::runtime_error("ZIP is incomplete or does not contain the application.");
}
#endif
} // namespace
YoutubePlatform youtube_platform(){
#ifdef _WIN32
  return YoutubePlatform::windows;
#elif defined(__APPLE__)
  return YoutubePlatform::macos;
#else
  return YoutubePlatform::linux;
#endif
}
YoutubeArch youtube_arch(){
#if defined(_M_X64) || defined(__x86_64__)
  return YoutubeArch::x64;
#elif defined(_M_ARM64) || defined(__aarch64__)
  return YoutubeArch::arm64;
#else
  return YoutubeArch::unknown;
#endif
}
YoutubeDetectionResult detect_youtube(const YoutubeDetectionOptions& options) {
  if(!options.executable_override.empty()) {auto path=options.executable_override;if(directory(path))path/="Contents/MacOS/VacuumTube";if(file(path))return {application(path),{}};return {{},"The configured YouTube application was not found."};}
#ifdef _WIN32
  if(auto path=managed_exe(options.data_root))return {application(path->first,true,path->second),{}};
#endif
  std::vector<fs::path> roots=options.search_roots;
  if(options.search_defaults) {
#ifdef _WIN32
    const auto environment=[](const wchar_t* name)->fs::path {const auto size=GetEnvironmentVariableW(name,nullptr,0);if(!size||size>32768)return {};std::wstring value(size,L'\0');const auto n=GetEnvironmentVariableW(name,value.data(),size);if(!n||n>=size)return {};value.resize(n);return fs::path(value);};
    if(const auto local=environment(L"LOCALAPPDATA");!local.empty())roots.emplace_back(local/"Programs"/"VacuumTube");
    if(const auto program=environment(L"ProgramFiles");!program.empty())roots.emplace_back(program/"VacuumTube");
#elif defined(__APPLE__)
    roots.emplace_back("/Applications/VacuumTube.app");if(const char* p=std::getenv("HOME"))roots.emplace_back(fs::path(p)/"Applications/VacuumTube.app");
#else
    roots.emplace_back("/usr/bin");roots.emplace_back("/usr/local/bin");if(const char* p=std::getenv("HOME"))roots.emplace_back(fs::path(p)/".local/bin");
#endif
  }
  if(roots.size()>64)return {{},"Too many application search locations."};
  for(const auto& root:roots){
#ifdef _WIN32
    const auto path=root/"VacuumTube.exe";
#elif defined(__APPLE__)
    const auto path=root/"Contents/MacOS/VacuumTube";
#else
    for(const auto* name:{"vacuumtube","VacuumTube-x86_64.AppImage","VacuumTube-arm64.AppImage"})if(file(root/name))return {application(root/name),{}};
    continue;
#endif
#if defined(_WIN32) || defined(__APPLE__)
    if(file(path))return {application(path),{}};
#endif
  }
#ifndef _WIN32
#ifndef __APPLE__
  if(options.search_defaults){std::vector<fs::path> flatpaks{"/var/lib/flatpak/app/rocks.shy.VacuumTube/current/active/metadata"};if(const char* p=std::getenv("HOME"))flatpaks.push_back(fs::path(p)/".local/share/flatpak/app/rocks.shy.VacuumTube/current/active/metadata");
    if(std::any_of(flatpaks.begin(),flatpaks.end(),[](const auto& p){std::error_code e;return fs::is_regular_file(p,e)&&!e;})&&file("/usr/bin/flatpak")) {YoutubeApplication app{fs::path("/usr/bin/flatpak"),{"/usr/bin/flatpak","run","rocks.shy.VacuumTube","--fullscreen"},TrackingQuality::launcher_only,false,"A running instance may receive this launch; its lifetime cannot be owned reliably.",{}};return {app,{}};}}
#endif
#endif
  return {{},"YouTube is not installed. Choose an executable or set it up."};
}
YoutubeReleaseResult select_youtube_release(std::string_view metadata,YoutubePlatform platform,YoutubeArch arch) {
  try {if(metadata.size()>2*1024*1024)throw std::runtime_error("Release metadata is too large.");std::map<int,std::set<std::string>> keys;bool duplicate=false;auto callback=[&](int depth,json::parse_event_t event,json& value){if(depth>64)throw std::runtime_error("Metadata nesting is too deep.");if(event==json::parse_event_t::object_start)keys[depth+1].clear();else if(event==json::parse_event_t::key&&!keys[depth].insert(value.get<std::string>()).second)duplicate=true;else if(event==json::parse_event_t::object_end)keys.erase(depth+1);return true;};const auto document=json::parse(metadata,callback);if(duplicate||!document.is_object()||!document.contains("tag_name")||!document["tag_name"].is_string()||!document.contains("assets")||!document["assets"].is_array()||document["assets"].size()>256)throw std::runtime_error("Invalid release metadata.");
    YoutubeRelease release;release.tag=document["tag_name"].get<std::string>();release.platform=platform;release.arch=arch;release.asset_name=asset_for(platform,arch);release.install_supported=platform==YoutubePlatform::windows;if(release.asset_name.empty()||!tag_valid(release.tag))throw std::runtime_error("This platform or architecture has no supported release.");
    unsigned found{};for(const auto& asset:document["assets"]) {if(!asset.is_object()||!asset.contains("name")||!asset["name"].is_string()||asset["name"]!=release.asset_name)continue;++found;if(!asset.contains("size")||!asset["size"].is_number_unsigned()||!asset.contains("digest")||!asset["digest"].is_string()||!asset.contains("browser_download_url")||!asset["browser_download_url"].is_string())throw std::runtime_error("The release has no verified SHA-256 digest or size.");
      release.size=asset["size"].get<std::uint64_t>();const auto digest=asset["digest"].get<std::string>();if(!digest.starts_with("sha256:"))throw std::runtime_error("The release has no SHA-256 digest.");release.sha256=digest.substr(7);release.url=asset["browser_download_url"].get<std::string>();}
    if(found!=1||!release_valid(release))throw std::runtime_error("The official release asset or its digest is unavailable.");
    return {release,{}};
  }catch(...){return {{},"Cannot find a valid official YouTube release with a verified SHA-256 digest for this platform."};}
}
YoutubeReleaseResult fetch_youtube_release(YoutubePlatform platform,YoutubeArch arch,const HttpTransport& transport){
  if(!transport)return {{},"Network access is unavailable."};
  try {HttpRequest req;req.url="https://api.github.com/repos/shy1132/VacuumTube/releases/latest";req.headers={"Accept: application/vnd.github+json"};req.max_bytes=2*1024*1024;req.max_redirects=0;const auto response=transport(req);if(!response.success||response.status!=200||response.body.size()>req.max_bytes)return {{},"Cannot retrieve the official release information."};return select_youtube_release(response.body,platform,arch);}catch(...){return {{},"Cannot retrieve the official release information."};}
}
YoutubeInstallResult install_youtube(const fs::path& root,const YoutubeRelease& release,const HttpTransport& transport,const YoutubeProgress& progress) {
#ifndef _WIN32
  (void)root;(void)release;(void)transport;(void)progress;return {{},"Portable YouTube setup is currently available only on Windows."};
#else
  if(root.empty()||release.platform!=YoutubePlatform::windows||release.arch!=youtube_arch()||!release.install_supported||!release_valid(release)||!transport)return {{},"The selected release or installation location is invalid or unsupported."};
  fs::path stage, managed_root, promoted, versions_root;bool own_stage=false, own_promoted=false;
  try {
    notify(progress,"Downloading");HttpRequest request;request.url=release.url;request.max_bytes=static_cast<std::size_t>(release.size);request.timeout_seconds=120;request.max_redirects=3;request.progress=[&](std::uint64_t done,std::uint64_t total){return !progress||progress("Downloading",done,total);};const auto response=transport(request);
    if(!response.success||response.status!=200||response.body.size()!=release.size)throw std::runtime_error("The download failed or was incomplete.");
    notify(progress,"Verifying");if(sha256(response.body)!=release.sha256)throw std::runtime_error("The download did not match the official SHA-256 digest.");
    std::error_code ec;fs::create_directories(root,ec);if(ec)throw std::runtime_error("Cannot create the application data directory.");
    const auto applications=root/"applications",youtube=applications/"youtube",managed=youtube/"openxmb-managed",versions=managed/"versions";
    managed_root=managed;versions_root=versions;
    make_directory(applications);make_directory(youtube);bool created=fs::create_directory(managed,ec);if(ec)throw std::runtime_error("Cannot create the managed application directory.");
    if(!directory(managed))throw std::runtime_error("The managed application path is not a safe directory.");
    if(created){std::ofstream owner(managed/"owner.txt",std::ios::binary);owner<<marker;owner.close();if(!owner)throw std::runtime_error("Cannot initialize the managed application directory.");}
    if(read_small(managed/"owner.txt")!=marker)throw std::runtime_error("The managed application directory is not owned by OpenXMB.");
    make_directory(versions);const auto destination=versions/fs::path(release.tag);if(fs::exists(destination,ec)||ec)throw std::runtime_error("This version is already present; the existing installation was preserved.");
    static std::atomic<std::uint64_t> serial{};for(unsigned i=0;i<100;++i){stage=managed/("stage-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));if(fs::create_directory(stage,ec)){own_stage=true;break;}if(ec)throw std::runtime_error("Cannot create an installation staging directory.");}
    if(!own_stage)throw std::runtime_error("Cannot create an installation staging directory.");
    notify(progress,"Extracting");extract(response.body,stage);notify(progress,"Installing");fs::rename(stage,destination,ec);if(ec)throw std::runtime_error("Cannot promote the staged application.");own_stage=false;promoted=destination;own_promoted=true;
    const auto pointer=managed/"current.txt";if(fs::exists(pointer,ec)&&!file(pointer))throw std::runtime_error("The current installation pointer is not a regular file.");if(ec)throw std::runtime_error("Cannot inspect the current installation pointer.");
    auto temp=managed/("current-"+std::to_string(++serial)+".tmp");HANDLE handle=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot update the current installation pointer.");
    DWORD count{};bool okay=WriteFile(handle,release.tag.data(),static_cast<DWORD>(release.tag.size()),&count,nullptr)&&count==release.tag.size()&&FlushFileBuffers(handle);CloseHandle(handle);
    if(!okay||!MoveFileExW(temp.c_str(),pointer.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){fs::remove(temp,ec);throw std::runtime_error("Cannot update the current installation pointer; the previous version is preserved.");}
    own_promoted=false;
    try { if(progress) (void)progress("Ready",0,0); } catch(...) {}
    return {destination/"VacuumTube.exe",{}};
  } catch(const std::exception& error){if(own_stage)discard_stage(stage,managed_root);if(own_promoted)discard_promoted(promoted,versions_root,release.tag);return {{},error.what()};}catch(...){if(own_stage)discard_stage(stage,managed_root);if(own_promoted)discard_promoted(promoted,versions_root,release.tag);return {{},"YouTube setup failed."};}
#endif
}
} // namespace openxmb::desktop








