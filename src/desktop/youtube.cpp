#include "openxmb/desktop/youtube.hpp"
#include <nlohmann/json.hpp>
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
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
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
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
// Any entry (including a dangling link) occupies a path; unreadable is occupied.
bool occupied(const fs::path& p){std::error_code e;return fs::symlink_status(p,e).type()!=fs::file_type::not_found;}
std::string utf8(const fs::path& p){auto value=p.u8string();return {value.begin(),value.end()};}
std::string read_small(const fs::path& path,std::size_t max=128){if(!file(path))return {};std::error_code e;auto size=fs::file_size(path,e);if(e||size>max)return {};std::ifstream input(path,std::ios::binary);std::string data(static_cast<std::size_t>(size),'\0');if(!data.empty()&&!input.read(data.data(),static_cast<std::streamsize>(data.size())))return {};return data;}
const char* arch_name(YoutubeArch a){return a==YoutubeArch::x64?"x64":"arm64";}
// Upstream archive layouts (VacuumTube v2.0.1, verified 5 November 2026):
// Windows portable ZIPs keep VacuumTube.exe at the root; Linux tarballs keep
// vacuumtube inside VacuumTube-<arch>/; macOS ZIPs contain VacuumTube.app.
fs::path managed_relative(YoutubePlatform p,YoutubeArch a){
  if(p==YoutubePlatform::windows)return "VacuumTube.exe";
  if(p==YoutubePlatform::linux)return fs::path(std::string("VacuumTube-")+arch_name(a))/"vacuumtube";
  return fs::path("VacuumTube.app")/"Contents"/"MacOS"/"VacuumTube";
}
std::optional<std::pair<fs::path,std::string>> managed_exe(const fs::path& root,YoutubePlatform platform,YoutubeArch arch) {
  if(root.empty()||arch==YoutubeArch::unknown)return {};
  const auto a=root/"applications",b=a/"youtube",m=b/"openxmb-managed",versions=m/"versions";
  if(!directory(a)||!directory(b)||!directory(m)||!directory(versions)||read_small(m/"owner.txt")!=marker)return {};
  const auto current=read_small(m/"current.txt");if(!tag_valid(current))return {};
  auto folder=versions/fs::path(current);if(!directory(folder))return {};const auto exe=folder/managed_relative(platform,arch);return file(exe)?std::optional<std::pair<fs::path,std::string>>{{exe,current}}:std::nullopt;
}
std::string executable_version(const fs::path& exe) {
#ifdef _WIN32
  DWORD ignored{};const auto size=GetFileVersionInfoSizeW(exe.c_str(),&ignored);if(size==0||size>4u*1024*1024)return {};
  std::vector<unsigned char> data(size);if(!GetFileVersionInfoW(exe.c_str(),0,size,data.data()))return {};
  VS_FIXEDFILEINFO* info{};UINT length{};if(!VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&info),&length)||!info||length<sizeof(VS_FIXEDFILEINFO)||info->dwSignature!=0xfeef04bd)return {};
  if(info->dwFileVersionMS==0&&info->dwFileVersionLS==0)return {};
  return std::to_string(HIWORD(info->dwFileVersionMS))+"."+std::to_string(LOWORD(info->dwFileVersionMS))+"."+std::to_string(HIWORD(info->dwFileVersionLS))+"."+std::to_string(LOWORD(info->dwFileVersionLS));
#elif defined(__APPLE__)
  // <bundle>/Contents/MacOS/<exe> -> <bundle>/Contents/Info.plist. A bounded
  // literal scan avoids linking Foundation into the desktop core.
  const auto plist=read_small(exe.parent_path().parent_path()/"Info.plist",256*1024);
  const auto key=plist.find("<key>CFBundleShortVersionString</key>");if(key==std::string::npos)return {};
  const auto open=plist.find("<string>",key);const auto close=open==std::string::npos?open:plist.find("</string>",open);
  if(open==std::string::npos||close==std::string::npos||close-open-8>32)return {};
  auto value=plist.substr(open+8,close-open-8);return tag_valid(value)?value:std::string{};
#else
  (void)exe;return {};
#endif
}
#if defined(__linux__)
// Electron needs either a setuid-root chrome-sandbox or unprivileged user
// namespaces. Ubuntu 24.04+ restricts the latter through AppArmor for
// unconfined applications, so a user-owned extraction cannot start sandboxed.
bool linux_sandbox_unavailable(const fs::path& exe) {
  struct stat helper{};
  if(::stat((exe.parent_path()/"chrome-sandbox").c_str(),&helper)==0&&helper.st_uid==0&&(helper.st_mode&S_ISUID))return false;
  const auto restricted=read_small("/proc/sys/kernel/apparmor_restrict_unprivileged_userns",16);
  const auto clone=read_small("/proc/sys/kernel/unprivileged_userns_clone",16);
  return restricted.starts_with("1")||clone.starts_with("0");
}
#endif
YoutubeApplication application(const fs::path& exe,bool managed=false,std::string version={}) {
  if(!managed||version.empty())version=executable_version(exe);
  YoutubeApplication app{exe,{utf8(exe),"--fullscreen"},TrackingQuality::launcher_only,managed,"A running instance may receive this launch; its lifetime cannot be owned reliably.",std::move(version)};
#if defined(__linux__)
  if(managed&&linux_sandbox_unavailable(exe)) {
    app.arguments.emplace_back("--no-sandbox");
    app.message+=" This system restricts unprivileged user namespaces, so the managed client runs without Chromium's sandbox.";
  }
#endif
  return app;
}
std::string asset_for(YoutubePlatform p,YoutubeArch a){
  if(a==YoutubeArch::unknown)return {};
  if(p==YoutubePlatform::windows)return std::string("VacuumTube-")+arch_name(a)+"-Portable.zip";
  if(p==YoutubePlatform::linux)return std::string("VacuumTube-")+arch_name(a)+".tar.gz";
  return std::string("VacuumTube-")+arch_name(a)+".zip";
}
bool release_valid(const YoutubeRelease& r){return tag_valid(r.tag)&&digest_valid(r.sha256)&&r.asset_name==asset_for(r.platform,r.arch)&&r.size>0&&r.size<=max_download&&r.url=="https://github.com/shy1132/VacuumTube/releases/download/"+r.tag+"/"+r.asset_name;}
void notify(const YoutubeProgress& cb,std::string_view stage){if(cb&&!cb(stage,0,0))throw std::runtime_error("Setup was cancelled.");}
// FIPS 180-4 SHA-256. Portable so every platform verifies the same digest.
class Sha256 {
  std::array<std::uint32_t,8> h{0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
  std::array<unsigned char,64> block{};std::size_t used{};std::uint64_t bits{};
  static std::uint32_t rotr(std::uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
  void compress(const unsigned char* p) {
    static constexpr std::uint32_t k[64]{
      0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
      0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
      0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
      0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
    std::uint32_t w[64];
    for(unsigned i=0;i<16;++i)w[i]=(std::uint32_t(p[i*4])<<24)|(std::uint32_t(p[i*4+1])<<16)|(std::uint32_t(p[i*4+2])<<8)|p[i*4+3];
    for(unsigned i=16;i<64;++i){const auto s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3),s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
    auto [a,b,c,d,e,f,g,hh]=h;
    for(unsigned i=0;i<64;++i){
      const auto t1=hh+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
      const auto t2=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));
      hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
  }
public:
  void update(std::string_view data) {
    bits+=static_cast<std::uint64_t>(data.size())*8;
    for(const unsigned char c:data){block[used++]=c;if(used==64){compress(block.data());used=0;}}
  }
  std::string hex() {
    const auto length=bits;block[used++]=0x80;
    if(used>56){while(used<64)block[used++]=0;compress(block.data());used=0;}
    while(used<56)block[used++]=0;
    for(int i=7;i>=0;--i)block[used++]=static_cast<unsigned char>(length>>(i*8));
    compress(block.data());
    const char* digits="0123456789abcdef";std::string out;out.reserve(64);
    for(const auto word:h)for(int i=3;i>=0;--i){const auto v=static_cast<unsigned char>(word>>(i*8));out+=digits[v>>4];out+=digits[v&15];}
    return out;
  }
};
// Conservative for every host: the strictest (Windows) component rules also
// keep macOS bundles and Linux tarballs portable between machines.
bool valid_component(std::string_view c) {
  if(c.empty()||c=="."||c==".."||c.size()>200||c.back()=='.'||c.back()==' ')return false;
  for(unsigned char x:c)if(x<32||x>=127||std::string_view{"<>:\"\\|?*"}.find(static_cast<char>(x))!=std::string_view::npos)return false;
  auto stem=std::string(c.substr(0,c.find('.')));for(auto& x:stem)if(x>='a'&&x<='z')x=static_cast<char>(x-'a'+'A');
  if(stem=="CON"||stem=="PRN"||stem=="AUX"||stem=="NUL"||stem=="CONIN$"||stem=="CONOUT$")return false;
  if(stem.size()==4&&(stem.starts_with("COM")||stem.starts_with("LPT"))&&stem[3]>='1'&&stem[3]<='9')return false;
  return true;
}
std::vector<std::string> components(std::string_view name,const char* noun="archive path") {
  if(name.starts_with("./"))name.remove_prefix(2);
  if(name.empty()||name.size()>1024||name.front()=='/'||name.front()=='\\')throw std::runtime_error(std::string("The archive contains an invalid ")+noun+".");
  if(name.back()=='/')name.remove_suffix(1);std::vector<std::string> out;
  while(!name.empty()){auto slash=name.find('/');auto part=name.substr(0,slash);if(!valid_component(part))throw std::runtime_error(std::string("The archive contains an unsafe ")+noun+".");out.emplace_back(part);if(out.size()>20)throw std::runtime_error("The archive path is too deep.");if(slash==name.npos)break;name.remove_prefix(slash+1);}
  if(out.empty())throw std::runtime_error("The archive contains an empty path.");return out;
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
void set_mode([[maybe_unused]] const fs::path& path,[[maybe_unused]] bool executable) {
#ifndef _WIN32
  std::error_code ec;fs::permissions(path,executable?fs::perms(0755):fs::perms(0644),fs::perm_options::replace|fs::perm_options::nofollow,ec);
  if(ec)throw std::runtime_error("Cannot set extracted file permissions.");
#endif
}
} // namespace
namespace detail {
std::string youtube_sha256(std::string_view bytes){Sha256 hash;hash.update(bytes);return hash.hex();}
void extract_youtube_archive(std::string_view bytes,YoutubePlatform platform,YoutubeArch arch,const fs::path& stage) {
  auto* ar=archive_read_new();if(!ar)throw std::runtime_error("Cannot initialize the archive reader.");
  struct Guard {archive* a;~Guard(){archive_read_free(a);}} guard{ar};
  if(platform==YoutubePlatform::linux){archive_read_support_filter_gzip(ar);archive_read_support_format_tar(ar);archive_read_support_format_gnutar(ar);}
  // Seekable ZIP reading (enabled by this call for memory input) uses the
  // central directory, which carries the Unix modes and symbolic links.
  else archive_read_support_format_zip(ar);
  if(archive_read_open_memory(ar,bytes.data(),bytes.size())!=ARCHIVE_OK)throw std::runtime_error("Invalid application archive.");
  // Links only appear inside macOS framework bundles. Each target must be a
  // relative, strictly descending path: no '.', '..' or absolute components,
  // so resolution can never leave the directory containing the link. Links are
  // created after every regular file so no entry is written through one.
  std::vector<std::pair<fs::path,fs::path>> links;
  std::set<std::string> seen;std::map<std::string,std::string> spelling;std::uint64_t total{};std::size_t entries{};archive_entry* entry{};int status{};
  while((status=archive_read_next_header(ar,&entry))==ARCHIVE_OK) {
    if(++entries>40000||archive_entry_hardlink(entry)||archive_entry_is_encrypted(entry)>0)throw std::runtime_error("The archive contains hard links, encryption, or too many entries.");
    const auto kind=archive_entry_filetype(entry);
    if(kind==AE_IFLNK&&platform!=YoutubePlatform::macos)throw std::runtime_error("The archive contains an unexpected symbolic link.");
    if(kind!=AE_IFREG&&kind!=AE_IFDIR&&kind!=AE_IFLNK)throw std::runtime_error("The archive contains a special file.");
    const char* raw=archive_entry_pathname_utf8(entry);if(!raw)raw=archive_entry_pathname(entry);if(!raw)throw std::runtime_error("The archive contains an invalid filename.");
    auto parts=components(raw);std::string key;std::string original;
    for(const auto& p:parts){if(!key.empty()){key+='/';original+='/';}original+=p;for(char c:p)key+=(c>='A'&&c<='Z')?static_cast<char>(c-'A'+'a'):c;
      auto [found,inserted]=spelling.emplace(key,original);if(!inserted&&found->second!=original)throw std::runtime_error("The archive contains case-colliding paths.");}
    // Tarballs may list a directory both implicitly and explicitly.
    if(!seen.insert(key).second&&kind!=AE_IFDIR)throw std::runtime_error("The archive contains duplicate paths.");
    fs::path path=stage;for(std::size_t i=0;i<parts.size();++i){path/=parts[i];if(i+1<parts.size()||kind==AE_IFDIR){if(!directory(path))make_directory(path);}}
    if(kind==AE_IFDIR){set_mode(path,true);continue;}
    if(kind==AE_IFLNK) {
      const char* target=archive_entry_symlink_utf8(entry);if(!target)target=archive_entry_symlink(entry);
      if(!target||!*target||std::strlen(target)>256)throw std::runtime_error("The archive contains an invalid symbolic link.");
      const auto steps=components(target,"symbolic link");fs::path relative;for(const auto& step:steps)relative/=step;
      links.emplace_back(path,relative);continue;
    }
    if(!archive_entry_size_is_set(entry)||archive_entry_size(entry)<0||static_cast<std::uint64_t>(archive_entry_size(entry))>512ull*1024*1024)throw std::runtime_error("An archive entry has an invalid size.");
    const auto size=static_cast<std::uint64_t>(archive_entry_size(entry));if(size>1024ull*1024*1024-total)throw std::runtime_error("The archive expands beyond the limit.");total+=size;
    if(occupied(path))throw std::runtime_error("An archive path collides with another entry.");
    {
      std::ofstream out(path,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("Cannot write an extracted file.");
      char buffer[65536];std::uint64_t written{};la_ssize_t n{};while((n=archive_read_data(ar,buffer,sizeof(buffer)))>0){if(static_cast<std::uint64_t>(n)>size-written)throw std::runtime_error("An archive entry exceeded its declared size.");out.write(buffer,n);if(!out)throw std::runtime_error("Cannot write an extracted file.");written+=static_cast<std::uint64_t>(n);}
      if(n<0||written!=size)throw std::runtime_error("An archive entry is incomplete or corrupt.");
    }
    set_mode(path,(archive_entry_perm(entry)&0111)!=0);
  }
  if(status!=ARCHIVE_EOF||entries==0)throw std::runtime_error("The archive is incomplete.");
  for(const auto& [path,target]:links) {
    std::error_code ec;if(occupied(path))throw std::runtime_error("A symbolic link collides with another entry.");
    fs::create_symlink(target,path,ec);if(ec)throw std::runtime_error("Cannot create a symbolic link from the archive.");
  }
  if(!file(stage/managed_relative(platform,arch)))throw std::runtime_error("The archive does not contain the application.");
}
} // namespace detail
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
  if(auto path=managed_exe(options.data_root,youtube_platform(),youtube_arch()))return {application(path->first,true,path->second),{}};
  std::vector<fs::path> roots=options.search_roots;
  if(options.search_defaults) {
#ifdef _WIN32
    const auto environment=[](const wchar_t* name)->fs::path {const auto size=GetEnvironmentVariableW(name,nullptr,0);if(!size||size>32768)return {};std::wstring value(size,L'\0');const auto n=GetEnvironmentVariableW(name,value.data(),size);if(!n||n>=size)return {};value.resize(n);return fs::path(value);};
    if(const auto local=environment(L"LOCALAPPDATA");!local.empty())roots.emplace_back(local/"Programs"/"VacuumTube");
    if(const auto program=environment(L"ProgramFiles");!program.empty())roots.emplace_back(program/"VacuumTube");
#elif defined(__APPLE__)
    roots.emplace_back("/Applications/VacuumTube.app");if(const char* p=std::getenv("HOME"))roots.emplace_back(fs::path(p)/"Applications/VacuumTube.app");
#else
    roots.emplace_back("/usr/bin");roots.emplace_back("/usr/local/bin");roots.emplace_back("/opt/VacuumTube");if(const char* p=std::getenv("HOME"))roots.emplace_back(fs::path(p)/".local/bin");
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
    YoutubeRelease release;release.tag=document["tag_name"].get<std::string>();release.platform=platform;release.arch=arch;release.asset_name=asset_for(platform,arch);release.install_supported=true;if(release.asset_name.empty()||!tag_valid(release.tag))throw std::runtime_error("This platform or architecture has no supported release.");
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
namespace {
// Replaces current.txt atomically; the previous pointer survives any failure.
void write_pointer(const fs::path& managed,const std::string& tag) {
  static std::atomic<std::uint64_t> serial{};std::error_code ec;
  const auto pointer=managed/"current.txt";if(occupied(pointer)&&!file(pointer))throw std::runtime_error("The current installation pointer is not a regular file.");
  auto temp=managed/("current-"+std::to_string(++serial)+".tmp");
#ifdef _WIN32
  HANDLE handle=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot update the current installation pointer.");
  DWORD count{};bool okay=WriteFile(handle,tag.data(),static_cast<DWORD>(tag.size()),&count,nullptr)&&count==tag.size()&&FlushFileBuffers(handle);CloseHandle(handle);
  if(!okay||!MoveFileExW(temp.c_str(),pointer.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){fs::remove(temp,ec);throw std::runtime_error("Cannot update the current installation pointer; the previous version is preserved.");}
#else
  const int handle=::open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0644);if(handle<0)throw std::runtime_error("Cannot update the current installation pointer.");
  const bool okay=::write(handle,tag.data(),tag.size())==static_cast<ssize_t>(tag.size())&&::fsync(handle)==0;::close(handle);
  // POSIX rename refuses to replace a read-only *directory* entry only through
  // directory permissions; honour a read-only pointer like Windows does.
  struct stat current{};const bool locked=::stat(pointer.c_str(),&current)==0&&!(current.st_mode&S_IWUSR);
  if(!okay||locked||::rename(temp.c_str(),pointer.c_str())!=0){fs::remove(temp,ec);throw std::runtime_error("Cannot update the current installation pointer; the previous version is preserved.");}
#endif
}
} // namespace
YoutubeInstallResult install_youtube(const fs::path& root,const YoutubeRelease& release,const HttpTransport& transport,const YoutubeProgress& progress) {
  if(root.empty()||release.platform!=youtube_platform()||release.arch!=youtube_arch()||!release.install_supported||!release_valid(release)||!transport)return {{},"The selected release or installation location is invalid or unsupported."};
  fs::path stage, managed_root, promoted, versions_root;bool own_stage=false, own_promoted=false;
  try {
    notify(progress,"Downloading");HttpRequest request;request.url=release.url;request.max_bytes=static_cast<std::size_t>(release.size);request.timeout_seconds=120;request.max_redirects=3;request.progress=[&](std::uint64_t done,std::uint64_t total){return !progress||progress("Downloading",done,total);};const auto response=transport(request);
    if(!response.success||response.status!=200||response.body.size()!=release.size)throw std::runtime_error("The download failed or was incomplete.");
    notify(progress,"Verifying");if(detail::youtube_sha256(response.body)!=release.sha256)throw std::runtime_error("The download did not match the official SHA-256 digest.");
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
    notify(progress,"Extracting");detail::extract_youtube_archive(response.body,release.platform,release.arch,stage);
    notify(progress,"Installing");fs::rename(stage,destination,ec);if(ec)throw std::runtime_error("Cannot promote the staged application.");own_stage=false;promoted=destination;own_promoted=true;
    write_pointer(managed,release.tag);
    own_promoted=false;
    try { if(progress) (void)progress("Ready",0,0); } catch(...) {}
    return {destination/managed_relative(release.platform,release.arch),{}};
  } catch(const std::exception& error){if(own_stage)discard_stage(stage,managed_root);if(own_promoted)discard_promoted(promoted,versions_root,release.tag);return {{},error.what()};}catch(...){if(own_stage)discard_stage(stage,managed_root);if(own_promoted)discard_promoted(promoted,versions_root,release.tag);return {{},"YouTube setup failed."};}
}
} // namespace openxmb::desktop
