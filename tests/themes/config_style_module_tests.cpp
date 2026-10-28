#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
import openxmb.config;
int main() {
  namespace fs=std::filesystem;
  using json=nlohmann::json;
  const auto root=fs::temp_directory_path()/("openxmb-style-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  unsigned checks=0;
  const auto require=[&](bool b,const char* text){++checks;if(!b)throw std::runtime_error(text);};
  try {
    config::config a;a.profileConfigPath=root/"profile.json";
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
