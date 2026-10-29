#include "openxmb/desktop/youtube.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <archive.h>
#include <archive_entry.h>
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
int main(){const auto root=fs::temp_directory_path()/"openxmb-youtube-fixture";try{fs::remove_all(root);
  const std::string digest(64,'a');const auto json=std::string{"{\"tag_name\":\"v2.0.1\",\"assets\":[{\"name\":\"VacuumTube-x64-Portable.zip\",\"size\":155272382,\"digest\":\"sha256:"}+digest+"\",\"browser_download_url\":\"https://github.com/shy1132/VacuumTube/releases/download/v2.0.1/VacuumTube-x64-Portable.zip\"}]}";
  auto plan=select_youtube_release(json,YoutubePlatform::windows,YoutubeArch::x64);require(bool(plan)&&plan.value->size==155272382,"official asset selection");
  require(!select_youtube_release(json,YoutubePlatform::windows,YoutubeArch::arm64),"wrong architecture accepted");
  auto missing=json;const auto digest_at=missing.find("sha256:");missing.replace(digest_at,7+64,"none");require(!select_youtube_release(missing,YoutubePlatform::windows,YoutubeArch::x64),"missing digest accepted");
  auto repeated=json;repeated.insert(1,"\"tag_name\":\"v9\",");require(!select_youtube_release(repeated,YoutubePlatform::windows,YoutubeArch::x64),"duplicate metadata accepted");
  require(!select_youtube_release("<html>bad</html>",YoutubePlatform::windows,YoutubeArch::x64),"HTML accepted as release metadata");
  HttpTransport metadata=[&](const HttpRequest& r){require(r.url=="https://api.github.com/repos/shy1132/VacuumTube/releases/latest"&&r.max_redirects==0,"unexpected metadata endpoint");return HttpResponse{true,200,json,{}};};
  require(bool(fetch_youtube_release(YoutubePlatform::windows,YoutubeArch::x64,metadata)),"release lookup failed");
  auto bad=json;bad.replace(bad.find("github.com"),10,"evil.testX");require(!select_youtube_release(bad,YoutubePlatform::windows,YoutubeArch::x64),"untrusted URL accepted");
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






