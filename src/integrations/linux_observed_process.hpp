#pragma once
#ifdef __linux__
// Private, read-only Linux process identity primitives for ObservedProcess.
// Nothing here can signal, reparent or otherwise manage another process.
#include "linux_process_identity.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <iterator>
#include <poll.h>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace openxmb::integrations::linux_observer {
namespace fs=std::filesystem;

struct FileId {
  dev_t device{};ino_t inode{};
  bool operator==(const FileId&) const=default;
};
inline std::optional<FileId> file_id(const std::string& path,bool regular=false) {
  struct stat info{};
  if(::stat(path.c_str(),&info)||(regular&&!S_ISREG(info.st_mode)))return {};
  return FileId{info.st_dev,info.st_ino};
}

// Owns one descriptor. Used for the retained pidfd.
class Fd {
public:
  Fd()=default;
  explicit Fd(int value) noexcept:value_(value){}
  Fd(Fd&& other) noexcept:value_(std::exchange(other.value_,-1)){}
  Fd& operator=(Fd&& other) noexcept {if(this!=&other){reset();value_=std::exchange(other.value_,-1);}return *this;}
  Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;
  ~Fd(){reset();}
  void reset() noexcept {if(value_>=0)::close(value_);value_=-1;}
  [[nodiscard]] int get() const noexcept {return value_;}
  explicit operator bool() const noexcept {return value_>=0;}
private:
  int value_{-1};
};

// readlink without truncation. A path above PATH_MAX is rejected.
inline std::optional<std::string> link_target(const std::string& path,int& error) {
  std::string buffer(PATH_MAX+1,'\0');
  const auto count=::readlink(path.c_str(),buffer.data(),buffer.size());
  if(count<=0||static_cast<std::size_t>(count)>PATH_MAX){error=count<0?errno:ENAMETOOLONG;return {};}
  buffer.resize(static_cast<std::size_t>(count));return buffer;
}

// Bounded read of a small procfs text file.
inline std::optional<std::string> read_small(const std::string& path,std::size_t limit=8192) {
  const Fd fd(::open(path.c_str(),O_RDONLY|O_CLOEXEC));
  if(!fd)return {};
  std::string text(limit+1,'\0');std::size_t used=0;
  while(used<text.size()) {
    const auto count=::read(fd.get(),text.data()+used,text.size()-used);
    if(count<0){if(errno==EINTR)continue;return {};}
    if(!count)break;used+=static_cast<std::size_t>(count);
  }
  if(used>limit)return {};
  text.resize(used);return text;
}

// Real, effective, saved and filesystem uid must all be this effective user.
// /proc/<pid>/status stays readable for a nondumpable process whose /proc inode
// is owned by root, so this also lets the caller identify "protected" games.
inline bool same_user(std::int64_t pid) {
  const auto text=read_small("/proc/"+std::to_string(pid)+"/status");
  if(!text)return false;
  const auto at=text->find("\nUid:");
  if(at==text->npos)return false;
  const char* cursor=text->c_str()+at+5;
  for(int field=0;field<4;++field) {
    char* end{};errno=0;const auto value=std::strtoul(cursor,&end,10);
    if(end==cursor||errno||value!=static_cast<unsigned long>(::geteuid()))return false;
    cursor=end;
  }
  return true;
}
// A truncated (15 byte) comm is never an identity; it only reveals that a
// readable-by-us but protected process may be the requested executable.
inline bool comm_matches(std::int64_t pid,std::string_view executable_name) {
  auto text=read_small("/proc/"+std::to_string(pid)+"/comm",256);
  if(!text||text->empty())return false;
  if(text->back()=='\n')text->pop_back();
  return !text->empty()&&*text==executable_name.substr(0,15);
}
// A different root or mount namespace gives /proc/<pid>/exe a path that is not
// comparable with this process' filesystem view.
inline bool same_filesystem_view(std::int64_t pid) {
  const auto base="/proc/"+std::to_string(pid);
  const auto mine=file_id("/proc/self/ns/mnt"),theirs=file_id(base+"/ns/mnt");
  const auto my_root=file_id("/proc/self/root"),their_root=file_id(base+"/root");
  return mine&&theirs&&my_root&&their_root&&*mine==*theirs&&*my_root==*their_root;
}

inline std::optional<std::uint64_t> boot_ticks() {
  timespec now{};const auto hz=::sysconf(_SC_CLK_TCK);
  if(hz<=0||::clock_gettime(CLOCK_BOOTTIME,&now))return {};
  return static_cast<std::uint64_t>(now.tv_sec)*static_cast<std::uint64_t>(hz)+static_cast<std::uint64_t>(now.tv_nsec)*static_cast<std::uint64_t>(hz)/1000000000ULL;
}

// pidfd_open is the only supported process pin. No PID-only fallback exists.
inline Fd open_pidfd(std::int64_t pid,int& error) {
#ifdef SYS_pidfd_open
  if(pid<=0||pid>INT_MAX){error=ESRCH;return {};}
  const long fd=::syscall(SYS_pidfd_open,static_cast<pid_t>(pid),0U);
  if(fd<0){error=errno;return {};}
  return Fd(static_cast<int>(fd));
#else
  (void)pid;error=ENOSYS;return {};
#endif
}
enum class PidfdState { running, exited, failed };
// A pidfd becomes readable once the process it references terminates.
inline PidfdState pidfd_state(int fd) {
  pollfd entry{fd,POLLIN,0};
  int ready;do ready=::poll(&entry,1,0);while(ready<0&&errno==EINTR);
  if(ready<0||(entry.revents&POLLNVAL))return PidfdState::failed;
  return ready>0?PidfdState::exited:PidfdState::running;
}

enum class Enumeration { complete, stopped, limit, failed };
// Walk numeric /proc entries with bounded work. `visit` returns false to end
// early. Stop requests are honored between entries.
template<class Visit>
Enumeration enumerate_processes(std::stop_token stop,std::chrono::steady_clock::time_point deadline,Visit&& visit,std::size_t entry_limit=65536) {
  const std::unique_ptr<DIR,int(*)(DIR*)> directory(::opendir("/proc"),::closedir);
  if(!directory)return Enumeration::failed;
  std::size_t entries=0;
  for(;;) {
    errno=0;const auto* entry=::readdir(directory.get());
    if(!entry)return errno?Enumeration::failed:Enumeration::complete;
    if(stop.stop_requested())return Enumeration::stopped;
    if(++entries>entry_limit||std::chrono::steady_clock::now()>deadline)return Enumeration::limit;
    const std::string_view name(entry->d_name);
    if(name.empty()||name.size()>10||!std::ranges::all_of(name,[](unsigned char c){return std::isdigit(c)!=0;}))continue;
    std::int64_t pid{};for(const char c:name)pid=pid*10+(c-'0');
    if(pid<=0||pid>INT_MAX)continue;
    if(!visit(pid))return Enumeration::complete;
  }
}

inline std::string lower(std::string value) {
  for(auto& c:value)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return value;
}
// Component-wise strict containment; "/games/Game2/x" is not within "/games/Game".
inline bool within(const fs::path& root,const fs::path& path) {
  auto part=path.begin();
  for(const auto& component:root) {
    if(part==path.end()||component!=*part)return false;
    ++part;
  }
  return part!=path.end();
}
// Bootstrap, launcher, crash, installer and redistributable images are not the
// game process even when they live in its installation root.
inline bool helper_image(const fs::path& root,const fs::path& image) {
  auto name=lower(image.filename().string());
  name=name.substr(0,name.find('.'));
  for(const auto* exact:{"steam","steamwebhelper","gameoverlayui","launcher","setup","install","installer","bootstrap","bootstrapper","updater","uninstall","reaper","start_protected_game","eac_launcher","eosbootstrapper","dxsetup"})
    if(name==exact)return true;
  for(const auto* prefix:{"unitycrashhandler","crashpad","crashreport","crashsender","crashhandler","vc_redist","vcredist","easyanticheat","unins","steam-runtime","pressure-vessel","gameoverlay","steamerrorreporter"})
    if(name.starts_with(prefix))return true;
  for(const auto* suffix:{"launcher","bootstrap","bootstrapper","crashhandler","crashreporter","installer","updater"})
    if(name.ends_with(suffix))return true;
  auto part=image.begin();
  for(const auto& ignored:root){(void)ignored;if(part!=image.end())++part;}
  for(;part!=image.end();++part) {
    const auto folder=lower(part->string());
    for(const auto* helper:{"_commonredist","redist","redistributables","installers","crashpad","vcredist","directx"})
      if(folder==helper)return true;
  }
  return false;
}

// Directory mode never accepts a broad location. `root` MUST be canonical.
inline bool safe_root(const fs::path& root) {
  if(!root.is_absolute()||root.native().size()>PATH_MAX||root.native().find('\0')!=std::string::npos)return false;
  const auto relative=root.relative_path();
  if(std::distance(relative.begin(),relative.end())<2)return false;
  const auto first=lower(relative.begin()->string());
  for(const auto* system:{"usr","bin","sbin","lib","lib32","lib64","libx32","etc","proc","sys","dev","run","boot"})
    if(first==system)return false;
  const auto name=lower(root.filename().string());
  for(const auto* broad:{"steam","steamlibrary","steamapps","common","games","home","mnt","media","opt","var","tmp","root","desktop","documents","downloads",".steam",".local",".config",".cache","share"})
    if(name==broad)return false;
  std::error_code error;
  if(fs::exists(root/"steamapps",error)||error)return false;
  for(const char* variable:{"HOME","XDG_DATA_HOME","XDG_CONFIG_HOME","XDG_CACHE_HOME","XDG_RUNTIME_DIR"}) {
    const auto* value=std::getenv(variable);if(!value||!*value)continue;
    const auto location=fs::canonical(fs::path(value),error);
    if(error){error.clear();continue;}
    if(root==location||within(root,location))return false;
  }
  struct stat info{},parent{};
  if(::stat(root.c_str(),&info)||!S_ISDIR(info.st_mode)||::stat(root.parent_path().c_str(),&parent))return false;
  if(info.st_dev!=parent.st_dev)return false;                       // a mount point is a drive/share root
  if(info.st_uid!=::geteuid()&&info.st_uid!=0)return false;       // another user's tree
  if(info.st_mode&S_IWOTH)return false;                             // anyone could plant an image
  return true;
}
}
#endif
