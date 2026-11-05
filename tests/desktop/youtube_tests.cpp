#include "openxmb/desktop/youtube.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <archive.h>
#include <archive_entry.h>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif
namespace fs=std::filesystem;
using namespace openxmb::desktop;
void require(bool x,const char* what){if(!x)throw std::runtime_error(what);}
#ifdef _WIN32
std::string zip(std::string name,std::string data) {
  std::string bytes(10240,'\0');std::size_t used{};auto a=archive_write_new();archive_write_set_format_zip(a);require(archive_write_open_memory(a,bytes.data(),bytes.size(),&used)==ARCHIVE_OK,"zip open");
  auto e=archive_entry_new();archive_entry_set_pathname(e,name.c_str());archive_entry_set_filetype(e,AE_IFREG);archive_entry_set_perm(e,0644);archive_entry_set_size(e,data.size());require(archive_write_header(a,e)==ARCHIVE_OK,"zip header");require(archive_write_data(a,data.data(),data.size())==static_cast<la_ssize_t>(data.size()),"zip body");archive_entry_free(e);archive_write_close(a);archive_write_free(a);bytes.resize(used);return bytes;
}
std::string zip_pair(std::string first,std::string second) {
  std::string bytes(32768,'\0');std::size_t used{};auto a=archive_write_new();archive_write_set_format_zip(a);require(archive_write_open_memory(a,bytes.data(),bytes.size(),&used)==ARCHIVE_OK,"pair zip open");
  for(const auto& name:{first,second}) {auto e=archive_entry_new();archive_entry_set_pathname(e,name.c_str());archive_entry_set_filetype(e,AE_IFREG);archive_entry_set_perm(e,0644);archive_entry_set_size(e,1);require(archive_write_header(a,e)==ARCHIVE_OK,"pair zip header");require(archive_write_data(a,"x",1)==1,"pair zip data");archive_entry_free(e);}
  archive_write_close(a);archive_write_free(a);bytes.resize(used);return bytes;
}
std::string zip_link() {
  std::string bytes(10240,'\0');std::size_t used{};auto a=archive_write_new();archive_write_set_format_zip(a);require(archive_write_open_memory(a,bytes.data(),bytes.size(),&used)==ARCHIVE_OK,"link zip open");
  auto e=archive_entry_new();archive_entry_set_pathname(e,"link");archive_entry_set_filetype(e,AE_IFLNK);archive_entry_set_perm(e,0777);archive_entry_set_symlink(e,"../outside");archive_entry_set_size(e,0);require(archive_write_header(a,e)==ARCHIVE_OK,"link zip header");archive_entry_free(e);archive_write_close(a);archive_write_free(a);bytes.resize(used);return bytes;
}
std::string hash(const std::string& bytes) {
  BCRYPT_ALG_HANDLE a{};BCRYPT_HASH_HANDLE h{};DWORD n{},object_size{};require(BCryptOpenAlgorithmProvider(&a,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"sha provider");require(BCryptGetProperty(a,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&object_size),sizeof(object_size),&n,0)>=0,"sha size");std::string object(object_size,'\0'),out(32,'\0');
  require(BCryptCreateHash(a,&h,reinterpret_cast<PUCHAR>(object.data()),object_size,nullptr,0,0)>=0,"sha init");require(BCryptHashData(h,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),static_cast<ULONG>(bytes.size()),0)>=0&&BCryptFinishHash(h,reinterpret_cast<PUCHAR>(out.data()),32,0)>=0,"sha finish");BCryptDestroyHash(h);BCryptCloseAlgorithmProvider(a,0);const char* hex="0123456789abcdef";std::string result;for(unsigned char c:out){result+=hex[c>>4];result+=hex[c&15];}return result;
}
#endif
struct Entry {std::string name;unsigned type{AE_IFREG};unsigned perm{0644};std::string data,link;};
// Builds a ZIP or gzip-compressed ustar archive entirely in memory.
std::string build_archive(bool tarball,const std::vector<Entry>& entries) {
  std::string bytes(1024*1024,'\0');std::size_t used{};auto a=archive_write_new();
  if(tarball){archive_write_set_format_pax_restricted(a);archive_write_add_filter_gzip(a);}else archive_write_set_format_zip(a);
  require(archive_write_open_memory(a,bytes.data(),bytes.size(),&used)==ARCHIVE_OK,"archive open");
  for(const auto& item:entries){auto e=archive_entry_new();archive_entry_set_pathname(e,item.name.c_str());archive_entry_set_filetype(e,item.type);archive_entry_set_perm(e,item.perm);
    if(item.type==AE_IFLNK)archive_entry_set_symlink(e,item.link.c_str());archive_entry_set_size(e,item.type==AE_IFREG?static_cast<la_int64_t>(item.data.size()):0);
    require(archive_write_header(a,e)==ARCHIVE_OK,"archive header");if(item.type==AE_IFREG&&!item.data.empty())require(archive_write_data(a,item.data.data(),item.data.size())==static_cast<la_ssize_t>(item.data.size()),"archive body");archive_entry_free(e);}
  require(archive_write_close(a)==ARCHIVE_OK,"archive close");archive_write_free(a);bytes.resize(used);return bytes;
}
bool extracts(const std::string& bytes,YoutubePlatform platform,YoutubeArch arch,const fs::path& stage,std::string* error=nullptr) {
  fs::remove_all(stage);fs::create_directories(stage);
  try{detail::extract_youtube_archive(bytes,platform,arch,stage);return true;}catch(const std::exception& e){if(error)*error=e.what();return false;}
}
void cross_platform_layouts(const fs::path& root) {
  require(detail::youtube_sha256("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA-256 empty vector");
  require(detail::youtube_sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA-256 abc vector");
  require(detail::youtube_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")=="248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1","SHA-256 two-block vector");
  require(detail::youtube_sha256(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","SHA-256 million-a vector");
  const std::string digest(64,'b');
  for(const auto& [platform,name]:{std::pair{YoutubePlatform::linux,std::string("VacuumTube-x64.tar.gz")},std::pair{YoutubePlatform::macos,std::string("VacuumTube-x64.zip")}}) {
    const auto metadata=std::string{"{\"tag_name\":\"v2.0.1\",\"assets\":[{\"name\":\""}+name+"\",\"size\":121295523,\"digest\":\"sha256:"+digest+"\",\"browser_download_url\":\"https://github.com/shy1132/VacuumTube/releases/download/v2.0.1/"+name+"\"}]}";
    const auto chosen=select_youtube_release(metadata,platform,YoutubeArch::x64);require(bool(chosen)&&chosen.value->install_supported&&chosen.value->asset_name==name,"Linux/macOS official asset selection");
  }
  const auto stage=root/"layout";std::string error;
  const auto linux_archive=build_archive(true,{{"VacuumTube-x64/",AE_IFDIR,0755},{"VacuumTube-x64/vacuumtube",AE_IFREG,0755,"elf"},{"VacuumTube-x64/locales/",AE_IFDIR,0755},{"VacuumTube-x64/locales/en-GB.pak",AE_IFREG,0644,"pak"}});
  if(!extracts(linux_archive,YoutubePlatform::linux,YoutubeArch::x64,stage,&error))throw std::runtime_error("Linux tarball: "+error);
  require(fs::is_regular_file(stage/"VacuumTube-x64"/"vacuumtube")&&fs::is_regular_file(stage/"VacuumTube-x64"/"locales"/"en-GB.pak"),"Linux tarball layout");
#ifndef _WIN32
  require((fs::status(stage/"VacuumTube-x64"/"vacuumtube").permissions()&fs::perms::owner_exec)!=fs::perms::none,"Linux executable bit preserved");
  require((fs::status(stage/"VacuumTube-x64"/"locales"/"en-GB.pak").permissions()&fs::perms::owner_exec)==fs::perms::none,"Linux data file made executable");
#endif
  require(!extracts(linux_archive,YoutubePlatform::linux,YoutubeArch::arm64,stage),"Linux arm64 accepted an x64 tarball");
  require(!extracts(build_archive(true,{{"VacuumTube-x64/vacuumtube",AE_IFREG,0755,"elf"},{"VacuumTube-x64/lib",AE_IFLNK,0777,{},"vacuumtube"}}),YoutubePlatform::linux,YoutubeArch::x64,stage),"Linux tarball symbolic link accepted");
  require(!extracts(build_archive(true,{{"../VacuumTube-x64/vacuumtube",AE_IFREG,0755,"elf"}}),YoutubePlatform::linux,YoutubeArch::x64,stage)&&!fs::exists(root/"VacuumTube-x64"),"Linux tarball traversal accepted");
  const std::vector<Entry> bundle{{"VacuumTube.app/Contents/MacOS/VacuumTube",AE_IFREG,0755,"macho"},{"VacuumTube.app/Contents/Info.plist",AE_IFREG,0644,"<key>CFBundleShortVersionString</key><string>2.0.1</string>"},
    {"VacuumTube.app/Contents/Frameworks/E.framework/Versions/A/E",AE_IFREG,0755,"dylib"},{"VacuumTube.app/Contents/Frameworks/E.framework/Versions/Current",AE_IFLNK,0755,{},"A"},
    {"VacuumTube.app/Contents/Frameworks/E.framework/E",AE_IFLNK,0755,{},"Versions/Current/E"}};
  const auto mac_archive=build_archive(false,bundle);
  bool links=true;
#ifdef _WIN32
  // Creating links needs Developer Mode or a privilege on Windows; the macOS
  // positive case still runs whenever this host can create them.
  {std::error_code ec;fs::create_directories(root/"probe");std::ofstream(root/"probe"/"t")<<"t";fs::create_symlink("t",root/"probe"/"l",ec);links=!ec;}
#endif
  if(links){if(!extracts(mac_archive,YoutubePlatform::macos,YoutubeArch::x64,stage,&error))throw std::runtime_error("macOS bundle: "+error);
    require(fs::is_symlink(stage/"VacuumTube.app/Contents/Frameworks/E.framework/E")&&fs::read_symlink(stage/"VacuumTube.app/Contents/Frameworks/E.framework/Versions/Current")=="A","macOS framework links");}
  for(const auto* hostile:{"../../../outside","/etc/passwd","Versions/../../escape","./A","A//B"})
    require(!extracts(build_archive(false,{bundle[0],{"VacuumTube.app/Contents/x",AE_IFLNK,0755,{},hostile}}),YoutubePlatform::macos,YoutubeArch::x64,stage),"hostile macOS link target accepted");
  require(!extracts(build_archive(false,{{"VacuumTube.app/Contents/MacOS/Other",AE_IFREG,0755,"x"}}),YoutubePlatform::macos,YoutubeArch::x64,stage),"macOS bundle without the executable accepted");
  fs::remove_all(stage);
}
int live_install(const fs::path& root) {
  try {
    require(!fs::exists(root),"Use a new isolated directory for live installation acceptance");
    const HttpTransport transport=[](const HttpRequest& request){return http_get(request);};
    const auto release=fetch_youtube_release(youtube_platform(),youtube_arch(),transport);
    if(!release)throw std::runtime_error(release.error);
    std::cout<<"Official release "<<release.value->tag<<", "<<release.value->size<<" bytes\n"<<std::flush;
    std::string previous;std::uint64_t last_bucket=std::uint64_t(-1);
    const auto installed=install_youtube(root,*release.value,transport,[&](std::string_view stage,std::uint64_t done,std::uint64_t total){
      const auto bucket=done/(16*1024*1024);
      if(stage!=previous||bucket!=last_bucket){previous=stage;last_bucket=bucket;std::cout<<stage<<" "<<done<<"/"<<total<<"\n"<<std::flush;}
      return true;
    });
    if(!installed)throw std::runtime_error(installed.error);
    YoutubeDetectionOptions options;options.data_root=root;options.search_defaults=false;
    const auto detected=detect_youtube(options);
    require(bool(detected)&&detected.value->managed&&detected.value->executable==installed.executable,"Managed application detection failed after real installation");
    require(!detected.value->version.empty(),"Installed version is unavailable");
    require(detected.value->arguments.size()>=2,"Fullscreen launch arguments are missing");
    std::cout<<"PASS real verified download, extraction, managed detection and fullscreen launch plan; version "<<detected.value->version<<" (application not launched)\n";
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
int main(int argc,char** argv){
 if(argc==3&&std::string_view(argv[1])=="--live-install")return live_install(fs::absolute(fs::u8path(argv[2])));
 // Opt-in: extract a real downloaded release archive for another platform.
 if(argc==6&&std::string_view(argv[1])=="--extract") try {
   const std::string_view os=argv[2],cpu=argv[3];const auto platform=os=="linux"?YoutubePlatform::linux:os=="macos"?YoutubePlatform::macos:YoutubePlatform::windows;
   std::ifstream in(fs::u8path(argv[4]),std::ios::binary);const std::string bytes{std::istreambuf_iterator<char>(in),{}};const auto stage=fs::absolute(fs::u8path(argv[5]));require(!fs::exists(stage),"Use a new extraction directory");fs::create_directories(stage);
   detail::extract_youtube_archive(bytes,platform,cpu=="arm64"?YoutubeArch::arm64:YoutubeArch::x64,stage);std::cout<<"PASS "<<bytes.size()<<" bytes, sha256 "<<detail::youtube_sha256(bytes)<<"\n";return 0;
 } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
 if(argc!=1){std::cerr<<"Use --live-install <new isolated directory> for opt-in network acceptance\n";return 2;}
 const auto root=fs::temp_directory_path()/"openxmb-youtube-fixture";try{fs::remove_all(root);
  const std::string digest(64,'a');const auto json=std::string{"{\"tag_name\":\"v2.0.1\",\"assets\":[{\"name\":\"VacuumTube-x64-Portable.zip\",\"size\":155272382,\"digest\":\"sha256:"}+digest+"\",\"browser_download_url\":\"https://github.com/shy1132/VacuumTube/releases/download/v2.0.1/VacuumTube-x64-Portable.zip\"}]}";
  auto plan=select_youtube_release(json,YoutubePlatform::windows,YoutubeArch::x64);require(bool(plan)&&plan.value->size==155272382,"official asset selection");
  require(!select_youtube_release(json,YoutubePlatform::windows,YoutubeArch::arm64),"wrong architecture accepted");
  auto missing=json;const auto digest_at=missing.find("sha256:");missing.replace(digest_at,7+64,"none");require(!select_youtube_release(missing,YoutubePlatform::windows,YoutubeArch::x64),"missing digest accepted");
  auto repeated=json;repeated.insert(1,"\"tag_name\":\"v9\",");require(!select_youtube_release(repeated,YoutubePlatform::windows,YoutubeArch::x64),"duplicate metadata accepted");
  require(!select_youtube_release("<html>bad</html>",YoutubePlatform::windows,YoutubeArch::x64),"HTML accepted as release metadata");
  HttpTransport metadata=[&](const HttpRequest& r){require(r.url=="https://api.github.com/repos/shy1132/VacuumTube/releases/latest"&&r.max_redirects==0,"unexpected metadata endpoint");return HttpResponse{true,200,json,{}};};
  require(bool(fetch_youtube_release(YoutubePlatform::windows,YoutubeArch::x64,metadata)),"release lookup failed");
  auto bad=json;bad.replace(bad.find("github.com"),10,"evil.testX");require(!select_youtube_release(bad,YoutubePlatform::windows,YoutubeArch::x64),"untrusted URL accepted");
  cross_platform_layouts(root);
  YoutubeDetectionOptions options;options.data_root=root;options.search_defaults=false;require(!detect_youtube(options),"missing app detected");
  fs::create_directories(root/"custom");const auto exe=root/"custom"/"vacuumtube.exe";std::ofstream(exe)<<"fixture";options.executable_override=exe;
  auto app=detect_youtube(options);require(bool(app)&&app.value->arguments.back()=="--fullscreen"&&app.value->tracking==TrackingQuality::launcher_only&&app.value->version.empty(),"override launch plan and unknown version");
#ifdef _WIN32
  wchar_t system_dir[MAX_PATH]{};require(GetSystemDirectoryW(system_dir,MAX_PATH)>0,"system directory unavailable");const auto versioned=fs::path(system_dir)/"kernel32.dll";
  DWORD version_handle{};if(GetFileVersionInfoSizeW(versioned.c_str(),&version_handle)>0){options.executable_override=versioned;const auto system_version=detect_youtube(options);require(bool(system_version)&&!system_version.value->version.empty(),"Windows fixed version metadata unavailable");}
  options.executable_override.clear();const auto archive=zip("VacuumTube.exe","fixture executable");YoutubeRelease release=*plan.value;release.size=archive.size();release.sha256=hash(archive);
  HttpTransport fake=[&](const HttpRequest&){return HttpResponse{true,200,archive,{}};};
  fs::create_directories(root/"applications"/"youtube");const auto unrelated=root/"applications"/"youtube"/"keep.txt";std::ofstream(unrelated)<<"do not touch";
  auto installed=install_youtube(root,release,fake,{});require(bool(installed),installed.error.c_str());auto detected=detect_youtube(options);require(fs::exists(unrelated)&&bool(detected)&&detected.value->version=="v2.0.1","install, version, or unrelated file preservation");
  auto invalid=release;invalid.sha256=std::string(64,'0');auto failure=install_youtube(root,invalid,fake,{});require(!failure&&fs::exists(unrelated)&&bool(detect_youtube(options)),"checksum rollback");
  const auto hostile=zip("../escape.txt","bad");invalid=release;invalid.tag="v2.0.3";invalid.url="https://github.com/shy1132/VacuumTube/releases/download/v2.0.3/"+invalid.asset_name;invalid.size=hostile.size();invalid.sha256=hash(hostile);HttpTransport attack=[&](const HttpRequest&){return HttpResponse{true,200,hostile,{}};};
  failure=install_youtube(root,invalid,attack,{});require(!failure&&!fs::exists(root/"escape.txt")&&bool(detect_youtube(options)),"zip traversal or rollback");
  auto try_archive=[&](const std::string& value,const std::string& tag){auto r=release;r.tag=tag;r.url="https://github.com/shy1132/VacuumTube/releases/download/"+tag+"/"+r.asset_name;r.size=value.size();r.sha256=hash(value);HttpTransport tr=[&](const HttpRequest&){return HttpResponse{true,200,value,{}};};return install_youtube(root,r,tr,{});};
  require(!try_archive(zip_pair("VacuumTube.exe","VACUUMTUBE.EXE"),"v2.0.4"),"case collision accepted");
  require(!try_archive(zip_pair("Folder/a","folder/b"),"v2.0.5"),"parent case collision accepted");
  require(!try_archive(zip_link(),"v2.0.6"),"symbolic link accepted");
  require(!try_archive(zip("CON.txt","bad"),"v2.0.7"),"Windows reserved filename accepted");
  require(bool(detect_youtube(options))&&fs::exists(unrelated),"unsafe archives damaged current installation");
  auto cancelled=install_youtube(root,release,fake,[](std::string_view,std::uint64_t,std::uint64_t){return false;});require(!cancelled&&bool(detect_youtube(options)),"cancelled setup damaged current install");
  const auto pointer=root/"applications"/"youtube"/"openxmb-managed"/"current.txt";require(SetFileAttributesW(pointer.c_str(),FILE_ATTRIBUTE_READONLY)!=0,"cannot protect test pointer");
  auto replacement=release;replacement.tag="v2.0.8";replacement.url="https://github.com/shy1132/VacuumTube/releases/download/v2.0.8/"+replacement.asset_name;bool promoted=false;
  failure=install_youtube(root,replacement,fake,[&](std::string_view stage,std::uint64_t,std::uint64_t){if(stage=="Installing")promoted=true;return true;});
  require(SetFileAttributesW(pointer.c_str(),FILE_ATTRIBUTE_NORMAL)!=0,"cannot restore test pointer attributes");
  require(!failure&&promoted&&bool(detect_youtube(options))&&!fs::exists(pointer.parent_path()/"versions"/"v2.0.8"),"failed pointer promotion did not preserve and roll back");
#endif
  fs::remove_all(root);std::cout<<"YouTube backend fixtures passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';fs::remove_all(root);return 1;}}
