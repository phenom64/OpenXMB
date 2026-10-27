#pragma once
#include <openxmb/video.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace programs::video_preferences {
enum class focus_action {none,pause,resume};
struct focus_policy {
  bool paused_by_focus{};
  void cancel() {paused_by_focus=false;}
  focus_action update(bool focused,bool allow_background,openxmb::video::state state) {
    if(state==openxmb::video::state::ended||state==openxmb::video::state::failed) {cancel();return focus_action::none;}
    if(!focused&&!allow_background) {
      if(!paused_by_focus&&state==openxmb::video::state::playing) {paused_by_focus=true;return focus_action::pause;}
      return focus_action::none;
    }
    if(paused_by_focus) {paused_by_focus=false;return focus_action::resume;}
    return focus_action::none;
  }
};
inline float output_gain(float local,double master,double video) {
  if(!std::isfinite(local)||!std::isfinite(master)||!std::isfinite(video)) return 0;
  return std::clamp(local,0.0F,1.0F)*static_cast<float>(std::clamp(master,0.0,100.0)*std::clamp(video,0.0,100.0)/10000.0);
}
inline std::optional<int> language_track(const std::vector<openxmb::video::track>& tracks,std::string_view language) {
  const auto marker="("+std::string(language)+")";
  for(const auto& track:tracks) if(track.label.find(marker)!=std::string::npos) return track.index;
  return std::nullopt;
}
inline std::pair<std::vector<std::filesystem::path>,std::size_t> discover_playlist(const std::filesystem::path& current) {
  std::vector<std::filesystem::path> files;
  std::error_code error;
  const auto directory=current.has_parent_path()?current.parent_path():std::filesystem::current_path(error);
  constexpr std::array<std::string_view,10> extensions{".mp4",".mkv",".avi",".mov",".webm",".mpg",".mpeg",".m4v",".ts",".ogv"};
  for(std::filesystem::directory_iterator item(directory,std::filesystem::directory_options::skip_permission_denied,error),end;item!=end&&!error;item.increment(error)) {
    if(!item->is_regular_file(error)) continue;
    auto extension=item->path().extension().string();
    for(auto& ch:extension) if(ch>='A'&&ch<='Z') ch=static_cast<char>(ch-'A'+'a');
    if(std::find(extensions.begin(),extensions.end(),extension)!=extensions.end()) files.push_back(item->path());
  }
  if(std::find(files.begin(),files.end(),current)==files.end()) files.push_back(current);
  std::sort(files.begin(),files.end());files.erase(std::unique(files.begin(),files.end()),files.end());
  const auto selected=static_cast<std::size_t>(std::find(files.begin(),files.end(),current)-files.begin());
  return {std::move(files),selected};
}
}
