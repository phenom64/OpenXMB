#include "openxmb/integrations/desktop.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
using namespace openxmb::integrations;
namespace fs=std::filesystem;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string utf8(const fs::path& path){auto value=path.u8string();return {value.begin(),value.end()};}
std::optional<std::string> environment(const char* name){
#ifdef _WIN32
  std::wstring key(name,name+std::char_traits<char>::length(name));
  const auto count=GetEnvironmentVariableW(key.c_str(),nullptr,0);
  if(!count){if(GetLastError()==ERROR_ENVVAR_NOT_FOUND)return {};return std::string{};}
  std::wstring value(count,L'\0');GetEnvironmentVariableW(key.c_str(),value.data(),count);value.pop_back();
  const int size=WideCharToMultiByte(CP_UTF8,0,value.data(),value.size(),nullptr,0,nullptr,nullptr);
  std::string output(size,'\0');WideCharToMultiByte(CP_UTF8,0,value.data(),value.size(),output.data(),size,nullptr,nullptr);return output;
#else
  const auto* value=std::getenv(name);return value?std::optional<std::string>(value):std::nullopt;
#endif
}
void set(const char* name,const std::optional<std::string>& value){
#ifdef _WIN32
  std::wstring key(name,name+std::char_traits<char>::length(name)),wide;
  if(value){int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value->data(),value->size(),nullptr,0);wide.resize(count);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value->data(),value->size(),wide.data(),count);}
  require(SetEnvironmentVariableW(key.c_str(),value?wide.c_str():nullptr)!=0,"test environment initialization failed");
#else
  require((value?setenv(name,value->c_str(),1):unsetenv(name))==0,"test environment initialization failed");
#endif
}
struct ParentEnvironment {
  std::map<std::string,std::optional<std::string>> previous;
  ParentEnvironment(){for(auto key:{"OPENXMB_ENV_TEST_VALUE","OPENXMB_ENV_TEST_INHERIT","OPENXMB_ENV_TEST_UNICODE","OPENXMB_ENV_TEST_EMPTY","OPENXMB_ENV_TEST_ADDED","OpenXmb_Env_Test_Case"})previous[key]=environment(key);set("OPENXMB_ENV_TEST_VALUE","parent");set("OPENXMB_ENV_TEST_INHERIT","inherited");set("OPENXMB_ENV_TEST_UNICODE",{});set("OPENXMB_ENV_TEST_EMPTY","nonempty");set("OPENXMB_ENV_TEST_ADDED",{});set("OpenXmb_Env_Test_Case","parent-case");}
  ~ParentEnvironment(){for(const auto& [key,value]:previous)set(key.c_str(),value);}
};
void wait(const std::shared_ptr<OwnedProcess>& process){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);while(process->observe().state==ProcessState::running){require(std::chrono::steady_clock::now()<deadline,"child timeout");std::this_thread::sleep_for(std::chrono::milliseconds(2));}require(process->observe().exit_code==0,"child failed");}
std::vector<std::string> read(const fs::path& path){std::ifstream input(path,std::ios::binary);require(bool(input),"child report missing");std::vector<std::string> values;std::string line;while(std::getline(input,line))values.push_back(line);return values;}
int run(const std::vector<std::string>& args){
  if(args.size()==4&&args[1]=="--hold-lease") {
    const auto signal=fs::path(std::u8string(args[2].begin(),args[2].end()));
    const auto release=fs::path(std::u8string(args[3].begin(),args[3].end()));
    {std::ofstream ready(signal);ready<<"ready";}
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    while(!fs::exists(release)&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return fs::exists(release)?0:3;
  }
  if(args.size()>2&&args[1]=="--child"){
    std::ofstream out(fs::path(std::u8string(args[2].begin(),args[2].end())),std::ios::binary);
    for(auto key:{"OPENXMB_ENV_TEST_VALUE","OPENXMB_ENV_TEST_INHERIT","OPENXMB_ENV_TEST_UNICODE","OPENXMB_ENV_TEST_EMPTY","OPENXMB_ENV_TEST_ADDED","OpenXmb_Env_Test_Case"})out<<environment(key).value_or("<absent>")<<'\n';return out?0:2;
  }
  const auto root=fs::temp_directory_path()/("openxmb-env-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
  try{
    ParentEnvironment parent;const auto self=fs::absolute(fs::path(std::u8string(args[0].begin(),args[0].end())));
    const std::string unicode(reinterpret_cast<const char*>(u8"Tokyo \u6771\u4eac \U0001f3ae"));
    std::map<std::string,std::string> overrides{{"OPENXMB_ENV_TEST_VALUE","child"},{"OPENXMB_ENV_TEST_UNICODE",unicode},{"OPENXMB_ENV_TEST_EMPTY",""},{"OPENXMB_ENV_TEST_ADDED","new"}};
#ifdef _WIN32
    overrides["OPENXMB_ENV_TEST_CASE"]="child-case";
#endif
    auto child=launch_owned_process({utf8(self),"--child",utf8(root/"overridden")},{},overrides);require(bool(child),child.message.c_str());wait(child.process);
    const auto values=read(root/"overridden");require(values.size()==6&&values[0]=="child","child did not receive override");require(values[1]=="inherited"&&values[2]==unicode&&values[3].empty()&&values[4]=="new","inherited/Unicode/empty/added environment changed");
#ifdef _WIN32
    require(values[5]=="child-case","Windows case-insensitive override failed");
#endif
    require(environment("OPENXMB_ENV_TEST_VALUE")=="parent"&&environment("OPENXMB_ENV_TEST_EMPTY")=="nonempty"&&!environment("OPENXMB_ENV_TEST_ADDED"),"launcher mutated parent environment");
    std::vector<std::future<OwnedProcessLaunch>> concurrent;
    for(unsigned i=0;i<8;++i)concurrent.push_back(std::async(std::launch::async,[&,i]{return launch_owned_process({utf8(self),"--child",utf8(root/std::to_string(i))},{},i%2?std::map<std::string,std::string>{}:std::map<std::string,std::string>{{"OPENXMB_ENV_TEST_VALUE",std::to_string(i)}});}));
    for(unsigned i=0;i<concurrent.size();++i){auto launched=concurrent[i].get();require(bool(launched),launched.message.c_str());wait(launched.process);auto report=read(root/std::to_string(i));require(report[0]==(i%2?"parent":std::to_string(i)),"concurrent child environments contaminated each other");}
    const std::vector<std::map<std::string,std::string>> invalid{{{"","x"}},{{"A=B","x"}},{{"1ABC","x"}},{{"BAD-NAME","x"}},{{std::string(129,'A'),"x"}},{{"VALID",std::string("bad\0value",9)}},{{std::string("BAD\0NAME",8),"x"}},{{"VALID",std::string("\xff",1)}},{{"VALID",std::string("\xc0\xaf",2)}},{{"VALID",std::string("\xed\xa0\x80",3)}},{{"VALID",std::string("\xf4\x90\x80\x80",4)}},{{"VALID",std::string(32769,'x')}}};
    for(const auto& value:invalid)require(!launch_owned_process({utf8(self),"--child",utf8(root/"invalid")},{},value),"invalid child environment accepted");
    std::map<std::string,std::string> excessive;for(unsigned i=0;i<65;++i)excessive["VAR_"+std::to_string(i)]="x";require(!launch_owned_process({utf8(self)},{},excessive),"too many overrides accepted");
    excessive.clear();for(unsigned i=0;i<9;++i)excessive["VAR_"+std::to_string(i)]=std::string(32768,'x');require(!launch_owned_process({utf8(self)},{},excessive),"aggregate environment byte limit was ignored");
#ifdef _WIN32
    require(!launch_owned_process({utf8(self)},{},{{"MixedCase","one"},{"MIXEDCASE","two"}}),"Windows ambiguous case variants accepted");
    const auto selected_path=root/"selected-lock",unrelated_path=root/"unrelated-lock";
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    auto selected=CreateFileW(selected_path.c_str(),GENERIC_READ,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    auto unrelated=CreateFileW(unrelated_path.c_str(),GENERIC_READ,0,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(selected!=INVALID_HANDLE_VALUE&&unrelated!=INVALID_HANDLE_VALUE,"isolated lock handles unavailable");
    auto leased=launch_owned_process({utf8(self),"--hold-lease",utf8(root/"ready"),utf8(root/"release")},{},{},{reinterpret_cast<std::uintptr_t>(selected)});
    CloseHandle(selected);CloseHandle(unrelated);
    require(bool(leased),leased.message.c_str());
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!fs::exists(root/"ready")&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(2));
    require(fs::exists(root/"ready"),"lease child never became ready");
    auto attempt=CreateFileW(selected_path.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(attempt!=INVALID_HANDLE_VALUE)CloseHandle(attempt);
    require(attempt==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION,"child lost selected lock after parent closed its handle");
    attempt=CreateFileW(unrelated_path.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(attempt!=INVALID_HANDLE_VALUE,"unrelated inheritable handle leaked into child");CloseHandle(attempt);
    {std::ofstream release(root/"release");release<<"done";}wait(leased.process);
    attempt=CreateFileW(selected_path.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(attempt!=INVALID_HANDLE_VALUE,"child exit retained original lock");CloseHandle(attempt);
    require(!launch_owned_process({utf8(self)},{},{},{0}),"invalid inherited handle accepted");
    require(!launch_owned_process({utf8(self)},{},{},std::vector<std::uintptr_t>(17,1)),"unbounded inherited handles accepted");
#else
    require(!launch_owned_process({utf8(self)},{},{},{1}),"unsupported native handles accepted");
#endif
    fs::remove_all(root);std::cout<<"Child-only environment inheritance, UTF-8, empty values, concurrent isolation and validation passed\n";return 0;
  }catch(...){fs::remove_all(root);throw;}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){try{std::vector<std::string> args;for(int i=0;i<argc;++i){int count=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);std::string value(count,'\0');WideCharToMultiByte(CP_UTF8,0,argv[i],-1,value.data(),count,nullptr,nullptr);value.pop_back();args.push_back(std::move(value));}return run(args);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
#else
int main(int argc,char** argv){try{return run({argv,argv+argc});}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
#endif
