#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <locale>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
import openxmb.config;
namespace {
struct comma_decimal : std::numpunct<char> {
  char do_decimal_point() const override { return ','; }
};
struct scoped_environment {
  std::vector<std::pair<std::string,std::optional<std::string>>> previous;
  static void assign(const std::string& key,const std::optional<std::string>& value) {
#ifdef _WIN32
    if(_putenv_s(key.c_str(),value?value->c_str():"")!=0) throw std::runtime_error("Cannot set test environment");
#else
    if(value?setenv(key.c_str(),value->c_str(),1)!=0:unsetenv(key.c_str())!=0)
      throw std::runtime_error("Cannot set test environment");
#endif
  }
  void set(const std::string& key,std::optional<std::string> value) {
    const auto* old=std::getenv(key.c_str());
    previous.emplace_back(key,old?std::optional<std::string>{old}:std::nullopt);
    assign(key,value);
  }
  ~scoped_environment() {
    for(auto it=previous.rbegin();it!=previous.rend();++it) {
      try {assign(it->first,it->second);} catch(...) {std::terminate();}
    }
  }
};
std::string bytes(const std::filesystem::path& path) {
  std::ifstream stream(path,std::ios::binary);
  if(!stream)throw std::runtime_error("Cannot read test fixture");
  return {std::istreambuf_iterator<char>{stream},std::istreambuf_iterator<char>{}};
}
}
int main() {
  namespace fs=std::filesystem;
  using json=nlohmann::json;
  const auto root=fs::temp_directory_path()/("openxmb-style-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  unsigned checks=0;
  const auto require=[&](bool b,const char* text){++checks;if(!b)throw std::runtime_error(text);};
  try {
    scoped_environment environment;
    for(const auto* name:{"HOME","USERPROFILE","APPDATA","LOCALAPPDATA","XDG_CONFIG_HOME"})
      environment.set(name,(root/"isolated home").string());
    environment.set("OPENXMB_CONFIG",(root/"environment-profile.json").string());
    environment.set("OPENXMB_CONFIG_READ_ONLY",std::nullopt);
    config::config fresh;fresh.profileConfigPath=root/"first run"/"config.json";fresh.load();
    require(fresh.initialSetupRequired,"missing profile requires initial setup");
    require(!fs::exists(fresh.profileConfigPath),"loading a missing profile does not prematurely create it");
    require(fresh.save_config_checked(),"incomplete profile can be saved without completing setup");
    config::config restarted;restarted.profileConfigPath=fresh.profileConfigPath;restarted.load();
    require(restarted.initialSetupRequired,"saving preferences before completion still requires setup after restart");
    fresh.initialSetupRequired=false;
    require(fresh.save_config_checked(),"completed setup saves successfully");
    const auto completed=json::parse(std::ifstream(fresh.profileConfigPath));
    require(completed.at("preferences").at("setup.completed")=="true","completed marker is serialized");
    restarted.load();require(!restarted.initialSetupRequired,"completion marker survives restart");
    fresh.reset_to_defaults();
    const auto reset=json::parse(std::ifstream(fresh.profileConfigPath));
    require(fresh.initialSetupRequired&&reset.at("preferences").at("setup.completed")=="false","reset writes an incomplete setup marker");
    restarted.load();require(restarted.initialSetupRequired,"reset uses the same setup entry condition after restart");

    const auto beta3dir=root/"Beta 3 profile";fs::create_directories(beta3dir);
    const auto library=beta3dir/"library.json";
    const auto library_contents=std::string{R"({"collections":[{"title":"Keep this album","items":["track.flac"]}],"revision":19})"};
    std::ofstream(library,std::ios::binary)<<library_contents;
    // A real pre-setup user profile uses Beta 3's version-5 schema with no setup marker.
    const json beta3={
      {"_meta",{{"project","OpenXMB"},{"app_version","0.3.0-beta.3"},{"config_version",5}}},
      {"shell",{{"language","fr"},{"music-path",(root/"My Music").string()},
        {"pictures-path",(root/"My Pictures").string()},{"videos-path",(root/"My Videos").string()},
        {"text-scale",1.25},{"theme-custom-colour","#123456"},{"date-time-x-offset",17.0}}},
      {"preferences",{{"profile.name","Beta Three User"},{"media.shuffle","On"},{"library.index",library.string()}}},
      {"controller",{{"rumble",false},{"cursor-speed",1.75}}},
      {"render",{{"max-fps",75},{"show-fps",true}}}
    };
    const auto beta3path=beta3dir/"config.json";std::ofstream(beta3path)<<beta3.dump(2);
    config::config existing;existing.profileConfigPath=beta3path;existing.load();
    require(!existing.initialSetupRequired,"existing Beta 3 user profile skips initial setup without a marker");
    require(existing.language=="fr"&&existing.textScale==1.25&&existing.dateTimeOffset==17.0,"Beta 3 language, style and clock preferences survive loading");
    require(existing.music_path()==root/"My Music"&&existing.pictures_path()==root/"My Pictures"&&existing.videos_path()==root/"My Videos","Beta 3 media directories survive loading");
    require(existing.preference("profile.name")=="Beta Three User"&&existing.preference("media.shuffle")=="On"&&existing.preference("library.index")==library.string(),"Beta 3 profile and media preferences survive loading");
    require(!existing.controllerRumble&&existing.controllerCursorSpeed==1.75&&existing.maxFPS==75&&existing.showFPS,"Beta 3 controller and render settings survive loading");
    require(existing.save_config_checked(),"Beta 3 profile saves after adoption");
    existing.load();require(!existing.initialSetupRequired,"adopted Beta 3 profile remains completed after restart");
    require(existing.preference("profile.name")=="Beta Three User"&&existing.musicPath==root/"My Music","adopting setup marker preserves profile and media settings on disk");
    require(bytes(library)==library_contents,"setup adoption never changes the separate library sentinel");
    existing.reset_to_defaults();existing.load();
    require(existing.initialSetupRequired,"resetting an adopted Beta 3 profile enters setup after reload");
    require(bytes(library)==library_contents,"resetting an adopted Beta 3 profile preserves its separate library");

    const auto before_read_only=bytes(fresh.profileConfigPath);
    {
      scoped_environment read_only;read_only.set("OPENXMB_CONFIG_READ_ONLY","1");
      fresh.initialSetupRequired=false;
      require(!fresh.save_config_checked(),"read-only mode never reports a successful setup save");
      require(bytes(fresh.profileConfigPath)==before_read_only,"read-only setup save leaves existing profile bytes unchanged");
      restarted.load();require(restarted.initialSetupRequired,"read-only completion does not survive restart");
    }
    require(std::getenv("OPENXMB_CONFIG_READ_ONLY")==nullptr,"nested read-only environment override is restored");
    config::config failed;failed.initialSetupRequired=false;
    const auto blocked=root/"parent is a file";std::ofstream(blocked)<<"keep";
    failed.profileConfigPath=blocked/"config.json";
    require(!failed.save_config_checked(),"failure creating profile directory is reported");
    require(bytes(blocked)=="keep","failed directory creation preserves blocking file");
    failed.profileConfigPath=fresh.profileConfigPath;
    auto blocked_temp=failed.profileConfigPath;blocked_temp+=".tmp";
    fs::create_directories(blocked_temp);std::ofstream(blocked_temp/"keep")<<"keep";
    require(!failed.save_config_checked(),"failure opening temporary config is reported");
    require(bytes(fresh.profileConfigPath)==before_read_only,"failed temporary write preserves original profile");
    restarted.load();require(restarted.initialSetupRequired,"failed temporary completion write does not complete setup after restart");
    fs::remove_all(blocked_temp);
    failed.profileConfigPath=root/"destination is a directory";fs::create_directories(failed.profileConfigPath);
    std::ofstream(failed.profileConfigPath/"keep")<<"keep";
    require(!failed.save_config_checked(),"failure replacing destination is reported");
    require(bytes(failed.profileConfigPath/"keep")=="keep","failed replacement preserves destination contents");
    require(bytes(library)==library_contents,"reset and failed saves leave separate library untouched");
    config::config a;a.profileConfigPath=root/"profile.json";
    const auto check_number=[&](const char* input,double expected) {
      a.preferences["numeric-test"]=input;
      require(a.number("numeric-test",-42)==expected,input);
    };
    check_number("0",0);check_number("-0.5",-.5);check_number(".5",.5);
    check_number("1.",1);check_number("1.25e+2",125);check_number("2E-2",.02);
    check_number("4.9406564584124654e-324",std::numeric_limits<double>::denorm_min());
    a.preferences["numeric-test"]="-0";
    require(std::signbit(a.number("numeric-test",-42)),"numeric parser preserves negative zero");
    require(a.number("missing-numeric-test",-42)==-42,"missing numeric preference uses fallback");
    a.preferences["numeric-test"]=std::string("1\0x",3);
    require(a.number("numeric-test",-42)==-42,"numeric parser rejects embedded trailing NUL");
    for(const auto* input:{"", " 1", "1 ", "+1", "1x", "1e", "0x1p2", "1,5", "nan", "inf", "-inf", "1e9999", "1e-9999"})
      check_number(input,-42);
    const auto previous_locale=std::locale();
    try {
      std::locale::global(std::locale(previous_locale,new comma_decimal));
      check_number("1.5",1.5);check_number("1,5",-42);
    } catch (...) {std::locale::global(previous_locale);throw;}
    std::locale::global(previous_locale);
    a.setTextScale(3);require(a.textScale==2,"text scale upper bound");
    a.setTextScale(.1);require(a.textScale==.5,"text scale lower bound");a.setTextScale(.85);
    a.setTextColor("#123456");a.setIconColor("#abcdef");a.setClockColor("#fedcba");a.setParticleColor("#204060");
    a.setIconStyle("flat");require(!a.iconGlassRefraction,"flat style disables glass");
    a.setIconStyle("glossy");require(a.iconGlassRefraction&&a.iconStyle=="glossy","glossy style enables glass");
    a.setWavesEnabled(false);a.setParticlesEnabled(false);
    a.setWaveParameters(R"({"spline:flowSpeed":0.5,"particle":{"count":5000,"opacity":2,"unknown":10}})");
    require(a.waveParameters.at("particle").at("count")==4096,"particle count normalized");
    require(a.waveParameters.at("particle").at("opacity")==1,"particle opacity normalized");
    require(!a.waveParameters.at("particle").contains("unknown"),"unknown wave fields rejected");
    require(a.waveParameters.at("spline").at("flowSpeed")==.5F,"flat wave parameters normalized");
    const auto previous=a.waveParameters;a.setWaveParameters("{broken");require(a.waveParameters==previous,"invalid wave data preserves prior settings");
    a.save_config();config::config b;b.profileConfigPath=a.profileConfigPath;b.load();
    const auto snapshot=json::parse(b.shell_snapshot());
    require(snapshot.at("font-size")==85,"text scale persists");
    require(snapshot.at("text-color")=="#123456"&&snapshot.at("icon-color")=="#abcdef"&&snapshot.at("clock-color")=="#fedcba","independent UI colors persist");
    require(snapshot.at("particle-color")=="#204060","particle color persists");
    require(!snapshot.at("theme-waves-enabled").get<bool>()&&!snapshot.at("theme-particles-enabled").get<bool>(),"independent effect gates persist");
    require(snapshot.at("icon-style")=="glossy"&&snapshot.at("icon-glass-refraction").get<bool>(),"icon style persists");
    require(snapshot.at("wave-parameters").at("spline").at("flowSpeed")==.5,"normalized parameters persist and snapshot");
    const auto priorText=b.textColor;b.setTextColor("invalid");require(b.textColor.r==priorText.r&&b.textColor.g==priorText.g&&b.textColor.b==priorText.b,"invalid colors preserve current setting");
    std::ofstream(root/"legacy.json")<<R"({"_meta":{"config_version":4},"shell":{},"render":{}})";
    b.profileConfigPath=root/"legacy.json";b.load();
    const auto migrated=json::parse(std::ifstream(root/"legacy.json"));
    require(migrated.at("_meta").at("config_version")==5,"legacy migration is persisted while loading");
    require(b.textScale==1&&b.wavesEnabled&&b.particlesEnabled&&b.waveParameters.empty(),"legacy profile does not inherit previous style/effects");
    require(b.textColor.r==1&&b.textColor.g==1&&b.textColor.b==1&&b.iconColor.r==1&&b.iconColor.g==1&&b.iconColor.b==1,"legacy profile uses neutral color defaults");
    const auto demo=root/"demo home";
    for(const auto* name:{"Music","Pictures","Videos"})fs::create_directories(demo/name);
    b.forcedMediaDirectory=demo;
    require(b.music_path()==demo/"Music","forced home routes Music to its own contents");
    require(b.pictures_path()==demo/"Pictures","forced home routes Pictures to its own contents");
    require(b.videos_path()==demo/"Videos","forced home routes Videos to its own contents");
    const auto aliases=root/"alternate home";
    for(const auto* name:{"music","Photos","Movies"})fs::create_directories(aliases/name);
    b.forcedMediaDirectory=aliases;
    require(b.music_path()==aliases/"music","lowercase media names work on case-sensitive systems");
    require(b.pictures_path()==aliases/"Photos","Photos alias supported");
    require(b.videos_path()==aliases/"Movies","macOS Movies alias supported");
    const auto partial=root/"partial home";fs::create_directories(partial/"Music");b.forcedMediaDirectory=partial;
    require(b.pictures_path()==partial/"Pictures"&&b.videos_path()==partial/"Videos","missing categories never show sibling folders or host libraries");
    const auto flat=root/"flat library";fs::create_directories(flat/"Album");b.forcedMediaDirectory=flat;
    require(b.music_path()==flat&&b.pictures_path()==flat&&b.videos_path()==flat,"direct flat media directory still supported");
    b.forcedMediaDirectory=demo;b.save_config();
    const auto saved=json::parse(std::ifstream(b.profileConfigPath));
    require(saved.at("shell").at("music-path")!=demo.string()&&saved.at("shell").at("music-path")!=(demo/"Music").string(),"runtime media override never saved as library preference");
    b.reset_to_defaults();require(b.music_path()==demo/"Music","forced home survives settings reset");
    b.forcedMediaDirectory.clear();
    require(b.music_path()==b.musicPath&&b.pictures_path()==b.picturesPath&&b.videos_path()==b.videosPath,"normal configured folders restored without override");
    fs::remove_all(root);std::cout<<"Config styles: "<<checks<<" checks passed\n";return 0;
  } catch(const std::exception& error){fs::remove_all(root);std::cerr<<error.what()<<'\n';return 1;}
}
