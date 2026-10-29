#include "openxmb/desktop/rpcs3_session.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <aclapi.h>
#include <winioctl.h>
#else
#include <sys/resource.h>
#include <unistd.h>

#include <csignal>
#endif

namespace fs = std::filesystem;
using namespace openxmb::desktop;
using namespace std::string_view_literals;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::string utf8(const fs::path &path) {
  const auto value = path.generic_u8string();
  return {value.begin(), value.end()};
}
void write(const fs::path &path, std::string_view bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(output), "fixture write failed");
}
std::string read(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}
std::string quoted(std::string value) {
  std::string result = "'";
  for (char c : value) {
    result += c;
    if (c == '\'')
      result += c;
  }
  return result + "'";
}
fs::path cfg_relative() {
#ifdef _WIN32
  return "config";
#else
  return {};
#endif
}
std::optional<std::string> environment_value() {
#ifdef _WIN32
  const auto size = GetEnvironmentVariableA("RPCS3_CONFIG_DIR", nullptr, 0);
  if (!size)
    return {};
  std::string value(size, '\0');
  require(GetEnvironmentVariableA("RPCS3_CONFIG_DIR", value.data(), size) == size - 1,
          "fixture environment read failed");
  value.resize(size - 1);
  return value;
#else
  const auto *value = std::getenv("RPCS3_CONFIG_DIR");
  return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}
bool directory_link(const fs::path &target, const fs::path &link) {
#ifdef _WIN32
  if (!CreateDirectoryW(link.c_str(), nullptr))
    return false;
  const auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    RemoveDirectoryW(link.c_str());
    return false;
  }
  const std::wstring substitute = L"\\??\\" + target.native(), display = target.native();
  struct Junction {
    DWORD tag;
    WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
    wchar_t path[1];
  };
  const auto path_bytes = (substitute.size() + display.size() + 2) * sizeof(wchar_t);
  std::vector<unsigned char> bytes(16 + path_bytes, 0);
  auto *data = reinterpret_cast<Junction *>(bytes.data());
  data->tag = IO_REPARSE_TAG_MOUNT_POINT;
  data->length = static_cast<WORD>(8 + path_bytes);
  data->substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
  data->print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
  data->print_length = static_cast<WORD>(display.size() * sizeof(wchar_t));
  std::memcpy(data->path, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
  std::memcpy(reinterpret_cast<unsigned char *>(data->path) + data->print_offset, display.c_str(),
              (display.size() + 1) * sizeof(wchar_t));
  DWORD returned = 0;
  const bool success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data,
                                       static_cast<DWORD>(bytes.size()), nullptr, 0, &returned, nullptr);
  CloseHandle(handle);
  if (!success)
    RemoveDirectoryW(link.c_str());
  return success;
#else
  std::error_code error;
  fs::create_directory_symlink(target, link, error);
  return !error;
#endif
}
void remove_link(const fs::path &link) {
#ifdef _WIN32
  require(RemoveDirectoryW(link.c_str()), "junction fixture cleanup failed");
#else
  require(fs::remove(link), "symlink fixture cleanup failed");
#endif
}
struct Fixture {
  fs::path root, original, exe, destination, source_config;
  explicit Fixture(fs::path directory)
      : root(std::move(directory)),
        original(root / fs::path(std::u8string(u8"Original \u6771\u4eac \U0001f3ae"))),
        exe(root / "bin/rpcs3.exe"), destination(root / "managed"), source_config(original / cfg_relative()) {
    write(exe, "isolated executable fixture");
    write(original / "RPCS3.buf", "");
    write(source_config / "config.yml", "Core: preserve exactly\r\n");
    write(source_config / "custom_configs/config_TEST00001.yml", "SPU Decoder: Recompiler\n");
    write(source_config / "input_configs/TEST00001/Default.yml", "Handler: DualSense\n");
    write(original / "GuiConfigs/CurrentSettings.ini",
          "; keep this "
          "comment\r\n[main_window]\r\nconfirmationBoxExitGame=true\r\nunrelated=preserve\r\n[GSFrame]"
          "\r\ndisableKbHotkeys=true\r\n");
    write(original / "GuiConfigs/persistent_settings.dat",
          "[Users]\nactive_user=00000007\n[Other]\nvalue=unchanged\n");
    write(original / "patches/patch.yml", "private patch preference\n");
    write(original / "data/settings.dat", "opaque preference bytes\0tail"sv);
    write(original / "Icons/ui/cross.png", "isolated UI image bytes");
    write(original / "Icons/game_icons/TEST00001/ICON0.PNG", "explicit custom game icon bytes");
    write(original / "dev_hdd0/home/00000007/savedata/save", "real save fixture must not be copied");
  }
  Rpcs3SessionRequest request() const { return {original, destination, exe, {}}; }
  fs::path managed() const {
#ifdef _WIN32
    return destination;
#else
    return destination / "rpcs3";
#endif
  }
};
void owner_only(const fs::path &path) {
#ifdef _WIN32
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  PACL acl = nullptr;
  PSID owner = nullptr;
  const auto status = GetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT,
                                            DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION, &owner,
                                            nullptr, &acl, nullptr, &descriptor);
  require(status == ERROR_SUCCESS && acl, "private destination has no readable DACL");
  ACL_SIZE_INFORMATION info{};
  void *ace = nullptr;
  const bool ok = GetAclInformation(acl, &info, sizeof(info), AclSizeInformation) && info.AceCount == 1 &&
                  GetAce(acl, 0, &ace) &&
                  static_cast<ACE_HEADER *>(ace)->AceType == ACCESS_ALLOWED_ACE_TYPE && owner &&
                  EqualSid(owner, &static_cast<ACCESS_ALLOWED_ACE *>(ace)->SidStart);
  if (descriptor)
    LocalFree(descriptor);
  require(ok, "private destination grants access beyond its owner");
#else
  const auto permissions = fs::status(path).permissions();
  require((permissions & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none,
          "private destination permissions are not owner-only");
#endif
}
void defaults(const fs::path &root) {
  Fixture f(root);
  std::map<fs::path, std::pair<std::string, fs::file_time_type>> originals;
  for (const auto &entry : fs::recursive_directory_iterator(f.original))
    if (entry.is_regular_file())
      originals[entry.path()] = {read(entry.path()), entry.last_write_time()};
  const auto before = environment_value();
  const auto result = prepare_rpcs3_session(f.request());
  require(static_cast<bool>(result), "default managed configuration preparation failed");
  const auto &config = *result.config;
  const auto managed = f.managed();
  require(config.private_directory == managed && config.dev_hdd0 == f.original / "dev_hdd0",
          "default VFS did not retain original Unicode data root");
  require(config.user_id == "00000007" &&
              config.arguments == std::vector<std::string>({"--user-id", "00000007"}),
          "persistent selected user was not preserved");
  require(config.config_dir_environment == utf8(managed) + "/",
          "RPCS3_CONFIG_DIR lost Unicode or its mandatory trailing slash");
#ifdef _WIN32
  require(config.environment_overrides ==
              std::map<std::string, std::string>{{"RPCS3_CONFIG_DIR", utf8(managed) + "/"}},
          "Windows child environment override is incomplete");
  require(config.lease &&
              config.inherited_handles ==
                  std::vector<std::uintptr_t>{reinterpret_cast<std::uintptr_t>(config.lease.get())},
          "managed session did not retain its original-root instance lease");
  require(config.warm_cache_directory.empty() && !config.cache_fallback_reason.empty() &&
              !fs::exists(managed / "cache") && !fs::exists(f.original / "cache"),
          "missing original cache was created, linked or not reported as a cold fallback");
#else
  const auto cache_parent = [] {
    if (const auto *value = std::getenv("XDG_CACHE_HOME"))
      return std::string(value);
    if (const auto *value = std::getenv("XDG_CONFIG_HOME"))
      return std::string(value);
    if (const auto *value = std::getenv("HOME"))
      return std::string(value) + "/.cache";
    return std::string{};
  }();
  require(config.environment_overrides ==
              std::map<std::string, std::string>{{"XDG_CONFIG_HOME", utf8(f.destination)},
                                                 {"XDG_CACHE_HOME", cache_parent}},
          "Linux child environment does not select the private XDG layout");
  require(config.warm_cache_directory.empty() && config.cache_fallback_reason.empty(),
          "Linux native cache reuse unexpectedly reported Windows junction fields");
#endif
  require(read(managed / "GuiConfigs/CurrentSettings.ini") ==
              "; keep this "
              "comment\r\n[main_window]\r\nconfirmationBoxExitGame=false\r\nunrelated=preserve\r\n[GSFrame]"
              "\r\ndisableKbHotkeys=true\r\n",
          "private INI changed unrelated preferences or did not disable confirmation");
  for (const auto &[path, source] : originals)
    require(read(path) == source.first && fs::last_write_time(path) == source.second,
            "source bytes or modification timestamp changed");
  for (const auto &relative : std::vector<fs::path>{
           cfg_relative() / "config.yml", cfg_relative() / "custom_configs/config_TEST00001.yml",
           cfg_relative() / "input_configs/TEST00001/Default.yml", "GuiConfigs/persistent_settings.dat",
           "patches/patch.yml", "data/settings.dat", "Icons/ui/cross.png",
           "Icons/game_icons/TEST00001/ICON0.PNG"})
    require(read(f.original / relative) == read(managed / relative),
            "configuration/input/patch/data snapshot lost unrelated bytes");
  require(!fs::exists(managed / "dev_hdd0"),
          "managed helper copied game saves instead of retaining the original VFS mount");
  const auto vfs = read(managed / cfg_relative() / "vfs.yml");
  require(vfs.find("'/dev_hdd0/': " + quoted(utf8(f.original / "dev_hdd0") + "/")) != std::string::npos,
          "default VFS hdd0 mount is not explicit");
  require(vfs.find("$(EmulatorDir)dev_") == std::string::npos,
          "default VFS retained relocatable device placeholders");
  owner_only(f.destination);
  owner_only(managed / cfg_relative() / "config.yml");
  require(environment_value() == before, "helper changed the host process environment");
}
void custom_vfs(const fs::path &root) {
  Fixture f(root);
  const auto emu = root / "User storage";
  const auto hdd = root / fs::path(std::u8string(u8"Saves \u00e9"));
  const auto usb = root / "USB #1";
  const auto source = "'$(EmulatorDir)': " + quoted(utf8(emu) + "/") + "\n/dev_hdd0/: " + quoted(utf8(hdd)) +
                      "\n/dev_hdd1/: ''\n/dev_flash/: '$(EmulatorDir)firmware/'\n/app_home/: "
                      "''\n/dev_usb***/:\n  /dev_usb003:\n    Path: " +
                      quoted(utf8(usb)) +
                      "\n    Serial: 'serial: preserved'\n    VID: '1234'\n    PID: 'abcd'\n";
  write(f.source_config / "vfs.yml", source);
  auto request = f.request();
  request.user_id = "00000009";
  const auto result = prepare_rpcs3_session(request);
  require(static_cast<bool>(result), "standard custom VFS schema was rejected");
  require(result.config->dev_hdd0 == hdd && result.config->user_id == "00000009",
          "explicit VFS root or selected-user override changed");
  const auto output = read(f.managed() / cfg_relative() / "vfs.yml");
  for (const auto &mount :
       std::vector<std::string>{utf8(hdd) + "/", utf8(emu / "dev_hdd1") + "/", utf8(emu / "firmware") + "/",
                                utf8(usb) + "/", "serial: preserved"})
    require(output.find(quoted(mount)) != std::string::npos,
            "custom VFS path or USB device metadata was lost");
  require(output.find("/dev_usb000") == std::string::npos,
          "helper fabricated a default USB mount beside an explicit custom USB map");
  require(read(f.source_config / "vfs.yml") == source, "custom original VFS source was changed");
}
void rejection(const fs::path &root) {
  Fixture f(root);
  auto request = f.request();
  request.user_id = "00000000";
  require(prepare_rpcs3_session(request).error == Rpcs3SessionError::invalid_input,
          "zero RPCS3 user ID was accepted");
  request = f.request();
  fs::create_directory(f.destination);
  write(f.destination / "keep", "existing destination sentinel");
  require(prepare_rpcs3_session(request).error == Rpcs3SessionError::unsafe_path &&
              read(f.destination / "keep") == "existing destination sentinel",
          "existing destination was accepted or changed");
  request.private_directory = f.original / "nested";
  require(prepare_rpcs3_session(request).error == Rpcs3SessionError::unsafe_path,
          "destination nested under original configuration was accepted");
  request.private_directory = root / "relative" / ".." / "invalid";
  require(prepare_rpcs3_session(request).error == Rpcs3SessionError::unsafe_path,
          "dot-segment destination was accepted");
  request = f.request();
  request.private_directory = root / "portable-test";
  fs::create_directory(f.exe.parent_path() / "portable");
  require(prepare_rpcs3_session(request).error == Rpcs3SessionError::portable_config &&
              !fs::exists(request.private_directory),
          "portable precedence did not block ineffective environment override");
}
void unsupported_vfs(const fs::path &root) {
  Fixture f(root);
  int attempt = 0;
  for (const auto *content :
       {"/unknown/: /user/data\n", "/dev_hdd0/: relative/path\n", "/dev_hdd0/: &anchor /user/data\n",
        "/dev_hdd0/: /one\n/dev_hdd0/: /two\n", "/dev_usb***/:\n  /dev_usb000:\n    Unknown: value\n",
        "/dev_hdd0/: '$(Unknown)dev_hdd0/'\n"}) {
    write(f.source_config / "vfs.yml", content);
    auto request = f.request();
    request.private_directory = root / ("unsupported-" + std::to_string(attempt++));
    const auto result = prepare_rpcs3_session(request);
    require(result.error == Rpcs3SessionError::unsupported_vfs && !result.message.empty() &&
                !fs::exists(request.private_directory),
            "unsupported VFS schema was guessed or left a destination");
  }
}
void limits_and_links(const fs::path &root) {
  Fixture f(root);
  const auto oversized = f.source_config / "oversized.yml";
  {
    std::ofstream file(oversized, std::ios::binary);
    file.seekp(8 * 1024 * 1024);
    file.put('x');
  }
  require(prepare_rpcs3_session(f.request()).error == Rpcs3SessionError::bounds_exceeded &&
              !fs::exists(f.destination),
          "oversized preferences were copied without a bound");
  fs::remove(oversized);
  const auto source_link = f.original / "data/link";
  require(directory_link(f.source_config, source_link), "cannot create the isolated link/reparse fixture");
  const auto source_result = prepare_rpcs3_session(f.request());
  remove_link(source_link);
  require(source_result.error == Rpcs3SessionError::unsafe_path && !fs::exists(f.destination),
          "linked source subtree was followed");
  require(directory_link(f.original, f.destination), "cannot create the isolated destination link fixture");
  const auto destination_result = prepare_rpcs3_session(f.request());
  remove_link(f.destination);
  require(destination_result.error == Rpcs3SessionError::unsafe_path, "linked destination was accepted");
  const auto parent_link = f.root / "linked-parent";
  require(directory_link(f.original, parent_link), "cannot create the isolated parent link fixture");
  auto request = f.request();
  request.private_directory = parent_link / "new-private";
  const auto parent_result = prepare_rpcs3_session(request);
  remove_link(parent_link);
  require(parent_result.error == Rpcs3SessionError::unsafe_path && !fs::exists(f.original / "new-private"),
          "linked destination parent was followed");
}
void other_bounds(const fs::path &root) {
  Fixture deep(root / "deep");
  auto nested = deep.original / "data/deep";
  for (unsigned i = 0; i < 13; ++i)
    nested /= "level";
  write(nested / "value", "deep fixture");
  require(prepare_rpcs3_session(deep.request()).error == Rpcs3SessionError::bounds_exceeded &&
              !fs::exists(deep.destination),
          "directory-depth bound was not enforced before destination creation");
  Fixture entries(root / "entries");
  for (unsigned i = 0; i < 4097; ++i)
    write(entries.original / "data/entries" / (std::to_string(i) + ".dat"), {});
  require(prepare_rpcs3_session(entries.request()).error == Rpcs3SessionError::bounds_exceeded &&
              !fs::exists(entries.destination),
          "snapshot entry-count bound was not enforced");
  Fixture total(root / "total");
  for (unsigned i = 0; i < 9; ++i) {
    std::ofstream output(total.original / "data" / (std::to_string(i) + ".dat"), std::ios::binary);
    output.seekp(8 * 1024 * 1024 - 1);
    output.put('x');
  }
  require(prepare_rpcs3_session(total.request()).error == Rpcs3SessionError::bounds_exceeded &&
              !fs::exists(total.destination),
          "snapshot total-byte bound was not enforced");
}
void ini_and_user_edges(const fs::path &root) {
  const std::vector<std::pair<std::string, std::string>> cases{
      {"", "[main_window]\nconfirmationBoxExitGame=false\n"},
      {"[Other]\r\npreserve=1",
       "[Other]\r\npreserve=1\r\n[main_window]\r\nconfirmationBoxExitGame=false\r\n"},
      {"[main_window]\nunrelated=1\n[Other]\nkeep=2\n",
       "[main_window]\nunrelated=1\nconfirmationBoxExitGame=false\n[Other]\nkeep=2\n"},
      {"[main_window]\nconfirmationBoxExitGame=false", "[main_window]\nconfirmationBoxExitGame=false"}};
  unsigned i = 0;
  for (const auto &[source, expected] : cases) {
    Fixture f(root / std::to_string(i++));
    write(f.original / "GuiConfigs/CurrentSettings.ini", source);
    fs::remove(f.original / "GuiConfigs/persistent_settings.dat");
    const auto result = prepare_rpcs3_session(f.request());
    require(static_cast<bool>(result) && result.config->user_id == "00000001",
            "RPCS3's absent-user fallback was not preserved");
    require(read(f.managed() / "GuiConfigs/CurrentSettings.ini") == expected &&
                read(f.original / "GuiConfigs/CurrentSettings.ini") == source,
            "INI insertion changed unrelated preferences or the source");
  }
  Fixture duplicate(root / "duplicate");
  write(duplicate.original / "GuiConfigs/CurrentSettings.ini",
        "[main_window]\nvalue=1\n[main_window]\nvalue=2\n");
  require(prepare_rpcs3_session(duplicate.request()).error == Rpcs3SessionError::invalid_input &&
              !fs::exists(duplicate.destination),
          "ambiguous duplicate INI sections were accepted");
  Fixture invalid(root / "invalid-user");
  write(invalid.original / "GuiConfigs/persistent_settings.dat", "[Users]\nactive_user=invalid\n");
  require(prepare_rpcs3_session(invalid.request()).error == Rpcs3SessionError::invalid_input &&
              !fs::exists(invalid.destination),
          "invalid persistent RPCS3 user was guessed");
}
void instance_lease(const fs::path &root) {
#ifdef _WIN32
  Fixture f(root);
  const auto lock_path = f.original / "RPCS3.buf";
  const auto native_open = [&] {
    return CreateFileW(lock_path.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  };
  HANDLE native = native_open();
  require(native != INVALID_HANDLE_VALUE, "cannot create native-style instance lock fixture");
  const auto blocked = prepare_rpcs3_session(f.request());
  CloseHandle(native);
  require(blocked.error == Rpcs3SessionError::config_in_use && !fs::exists(f.destination),
          "native-style competing lock did not block preparation before copying");
  write(lock_path, "retain existing instance bytes");
  const auto lock_time = fs::last_write_time(lock_path);
  auto prepared = prepare_rpcs3_session(f.request());
  require(static_cast<bool>(prepared) && prepared.config->lease,
          "managed instance lease could not be acquired");
  DWORD flags = 0;
  require(GetHandleInformation(prepared.config->lease.get(), &flags) && !(flags & HANDLE_FLAG_INHERIT),
          "original instance lease is unexpectedly inheritable");
  native = native_open();
  const auto error = GetLastError();
  if (native != INVALID_HANDLE_VALUE)
    CloseHandle(native);
  require(native == INVALID_HANDLE_VALUE && error == ERROR_SHARING_VIOLATION,
          "managed lease did not block a native-style writer");
  auto second = f.request();
  second.private_directory = f.root / "second-managed";
  require(prepare_rpcs3_session(second).error == Rpcs3SessionError::config_in_use &&
              !fs::exists(second.private_directory),
          "two managed sessions acquired the same original-root lease");
  auto retained = prepared.config->lease;
  prepared.config.reset();
  native = native_open();
  if (native != INVALID_HANDLE_VALUE)
    CloseHandle(native);
  require(native == INVALID_HANDLE_VALUE, "lease was released before its final shared owner");
  retained.reset();
  require(read(lock_path) == "retain existing instance bytes" && fs::last_write_time(lock_path) == lock_time,
          "read-only instance lease modified source lock bytes or modification time");
  native = native_open();
  require(native != INVALID_HANDLE_VALUE, "native lock remained blocked after lease release");
  CloseHandle(native);
  Fixture missing(root / "missing");
  fs::remove(missing.original / "RPCS3.buf");
  require(prepare_rpcs3_session(missing.request()).error == Rpcs3SessionError::instance_lock_unavailable &&
              !fs::exists(missing.destination) && !fs::exists(missing.original / "RPCS3.buf"),
          "missing original instance lock was created or bypassed");
#else
  (void)root;
#endif
}
void icons(const fs::path &root) {
  Fixture f(root);
  const auto result = prepare_rpcs3_session(f.request());
  require(static_cast<bool>(result), "icons fixture preparation failed");
  require(read(f.managed() / "Icons/ui/cross.png") == "isolated UI image bytes" &&
              read(f.managed() / "Icons/game_icons/TEST00001/ICON0.PNG") == "explicit custom game icon bytes",
          "managed snapshot omitted original UI or explicit custom game icons");
}
// Fixed old timestamp so an accidental touch is visible.
fs::file_time_type old_time() {
  static const auto value = fs::file_time_type::clock::now() - std::chrono::hours(24 * 30);
  return value;
}
fs::path make_cache(const fs::path &original) {
  const auto cache = original / "cache";
  write(cache / "ppu-hash-A/sentinel.obj", "warm PPU cache bytes\0end"sv);
  write(cache / fs::path(std::u8string(u8"TEST00001 \u6771\u4eac/spu-\U0001f3ae.bin")), "warm SPU cache bytes");
  write(cache / "shaders_cache/TEST00001/program.bin", "warm shader cache bytes");
  for (const auto &entry : fs::recursive_directory_iterator(cache))
    if (entry.is_regular_file())
      fs::last_write_time(entry.path(), old_time());
  std::vector<fs::path> directories;
  for (const auto &entry : fs::recursive_directory_iterator(cache))
    if (entry.is_directory())
      directories.push_back(entry.path());
  for (const auto &directory : directories)
    fs::last_write_time(directory, old_time());
  fs::last_write_time(cache, old_time());
  return cache;
}
// Bytes, modification time and shape of every entry below root (fixtures only).
std::map<fs::path, std::string> tree(const fs::path &root) {
  std::map<fs::path, std::string> result;
  result["."] = "D" + std::to_string(fs::last_write_time(root).time_since_epoch().count());
  for (const auto &entry : fs::recursive_directory_iterator(root)) {
    const auto relative = entry.path().lexically_relative(root);
    // Enumeration timestamps of directories are lazily refreshed on NTFS; read the real ones.
    const auto time = std::to_string(fs::last_write_time(entry.path()).time_since_epoch().count());
    result[relative] = entry.is_directory() ? "D" + time : "F" + time + read(entry.path());
  }
  return result;
}
#ifdef _WIN32
bool is_junction(const fs::path &path) {
  const auto attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY);
}
Rpcs3SessionConfig warm_session(const Fixture &f, Rpcs3SessionRequest request, const fs::path &cache) {
  const auto before = tree(f.original);
  auto result = prepare_rpcs3_session(request);
  require(static_cast<bool>(result) && result.config->cache_fallback_reason.empty(),
          "managed session did not retain a warm original cache");
  const auto link = result.config->private_directory / "cache";
  require(is_junction(link) && fs::equivalent(link, cache) &&
              result.config->warm_cache_directory == fs::canonical(cache),
          "private cache does not resolve to the exact original Unicode cache directory");
  require(read(link / "ppu-hash-A/sentinel.obj") == "warm PPU cache bytes\0end"sv &&
              read(link / fs::path(std::u8string(u8"TEST00001 \u6771\u4eac/spu-\U0001f3ae.bin"))) ==
                  "warm SPU cache bytes",
          "warm cache sentinel bytes are not visible through the private cache");
  require(tree(f.original) == before, "cache link preparation changed original bytes or modification times");
  return std::move(*result.config);
}
void warm_cache(const fs::path &root) {
  Fixture f(root);
  const auto cache = make_cache(f.original);
  const auto before = tree(f.original);
  auto config = warm_session(f, f.request(), cache);
  // Normal and managed launches stay mutually exclusive after cache linking.
  auto second = f.request();
  second.private_directory = f.root / "second-managed";
  require(prepare_rpcs3_session(second).error == Rpcs3SessionError::config_in_use &&
              !fs::exists(second.private_directory),
          "second managed session acquired the warm-cache original lease");
  const auto lock_path = f.original / "RPCS3.buf";
  const HANDLE native = CreateFileW(lock_path.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  const auto native_error = GetLastError();
  if (native != INVALID_HANDLE_VALUE)
    CloseHandle(native);
  require(native == INVALID_HANDLE_VALUE && native_error == ERROR_SHARING_VIOLATION,
          "warm-cache session did not retain native instance-lock exclusion");
  config.lease.reset();
  // Same native operation a profile/private-session removal would use.
  std::error_code error;
  const auto removed = fs::remove_all(f.destination, error);
  require(!error && removed > 1 && !fs::exists(f.destination),
          "private session removal failed with its cache junction");
  require(tree(f.original) == before, "removing the private snapshot touched the original cache");
  // Removing only the link is also safe.
  Fixture link_only(root / "link-only");
  const auto link_cache = make_cache(link_only.original);
  const auto link_before = tree(link_only.original);
  auto linked = warm_session(link_only, link_only.request(), link_cache);
  linked.lease.reset();
  fs::remove_all(link_only.destination / "cache", error);
  require(!error && !fs::exists(link_only.destination / "cache") && fs::exists(link_only.destination) &&
              tree(link_only.original) == link_before,
          "removing the junction alone followed it into the original cache");
  fs::remove_all(link_only.destination, error);
  require(!error && tree(link_only.original) == link_before, "later private removal changed the cache");
}
void cold_cache(const fs::path &root) {
  const auto cold = [](const Fixture &f, const char *name) {
    auto result = prepare_rpcs3_session(f.request());
    require(static_cast<bool>(result) && result.config->warm_cache_directory.empty() &&
                !result.config->cache_fallback_reason.empty() &&
                result.config->cache_fallback_reason.find(utf8(f.original)) == std::string::npos &&
                !fs::exists(f.managed() / "cache"),
            name);
  };
  Fixture missing(root / "missing");
  const auto missing_before = tree(missing.original);
  cold(missing, "missing original cache was linked or not reported");
  require(!fs::exists(missing.original / "cache") && tree(missing.original) == missing_before,
          "missing original cache was created or the original changed");
  Fixture file(root / "file");
  write(file.original / "cache", "not a directory");
  fs::last_write_time(file.original / "cache", old_time());
  cold(file, "file-valued cache was linked");
  require(read(file.original / "cache") == "not a directory" &&
              fs::last_write_time(file.original / "cache") == old_time(),
          "file-valued cache was changed");
  Fixture linked(root / "linked");
  write(root / "outside-cache/outside.sentinel", "outside bytes");
  const auto outside_before = tree(root / "outside-cache");
  require(directory_link(root / "outside-cache", linked.original / "cache"),
          "cannot create the linked-cache fixture");
  cold(linked, "linked original cache was followed");
  remove_link(linked.original / "cache");
  require(tree(root / "outside-cache") == outside_before, "linked cache target changed");
  // Other guards stay strict: a linked effective root is still rejected outright.
  Fixture guard(root / "guard");
  make_cache(guard.original);
  require(directory_link(guard.original, root / "linked-config"), "cannot create the linked-root fixture");
  auto request = guard.request();
  request.effective_config_root = root / "linked-config";
  const auto rejected = prepare_rpcs3_session(request);
  remove_link(root / "linked-config");
  require(rejected.error == Rpcs3SessionError::unsafe_path && !fs::exists(guard.destination),
          "linked effective config root was accepted by the warm-cache route");
}
// Fresh roots on other local volumes: cross-volume junctions and volumes
// without reparse-point support (for example exFAT).
struct VolumeRoot {
  fs::path path;
  explicit VolumeRoot(const fs::path &parent, const std::string &name) : path(parent / name) {
    require(fs::create_directory(path), "cannot create a fresh other-volume fixture root");
  }
  ~VolumeRoot() {
    std::error_code error;
    fs::remove_all(path, error);
  }
};
void other_volumes(const fs::path &temp_root) {
  const auto here = fs::canonical(fs::temp_directory_path()).root_name();
  const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto drives = GetLogicalDrives();
  unsigned warm = 0, cold = 0;
  for (unsigned letter = 0; letter < 26; ++letter) {
    if (!(drives & (1u << letter)))
      continue;
    const std::wstring volume = std::wstring(1, static_cast<wchar_t>(L'A' + letter)) + L":\\";
    if (GetDriveTypeW(volume.c_str()) != DRIVE_FIXED || fs::path(volume).root_name() == here)
      continue;
    DWORD flags = 0;
    if (!GetVolumeInformationW(volume.c_str(), nullptr, 0, nullptr, nullptr, &flags, nullptr, 0))
      continue;
    std::error_code error;
    const auto probe = fs::path(volume) / ("openxmb_rpcs3_session_fixture_" + stamp);
    if (!fs::create_directory(probe, error))
      continue;
    fs::remove(probe);
    const std::string name = "openxmb_rpcs3_session_fixture_" + stamp;
    VolumeRoot other(fs::path(volume), name);
    Fixture f(temp_root / ("cross-" + std::to_string(letter)));
    const auto cache = make_cache(f.original);
    auto request = f.request();
    request.private_directory = other.path / "managed";
    if (flags & FILE_SUPPORTS_REPARSE_POINTS) {
      auto config = warm_session(f, request, cache);
      config.lease.reset();
      const auto before = tree(f.original);
      fs::remove_all(request.private_directory, error);
      require(!error && tree(f.original) == before, "cross-volume private removal changed the cache");
      // Checked cleanup across volumes removes the junction without walking its target.
      request.private_directory = other.path / "managed-cleanup";
      auto cleaned = warm_session(f, request, cache);
      cleaned.lease.reset();
      const auto cleanup = cleanup_rpcs3_session(cleaned);
      require(cleanup && cleanup.removed && !fs::exists(request.private_directory) && tree(f.original) == before,
              "cross-volume checked cleanup failed or changed the cache");
      // And the reverse direction: original cache on the other volume.
      Fixture remote(other.path / "original-root");
      const auto remote_cache = make_cache(remote.original);
      auto reverse = remote.request();
      reverse.private_directory = temp_root / ("reverse-" + std::to_string(letter));
      auto reverse_config = warm_session(remote, reverse, remote_cache);
      reverse_config.lease.reset();
      fs::remove_all(reverse.private_directory, error);
      require(!error, "reverse cross-volume private removal failed");
      ++warm;
    } else {
      const auto before = tree(f.original);
      auto result = prepare_rpcs3_session(request);
      require(static_cast<bool>(result) && result.config->warm_cache_directory.empty() &&
                  !result.config->cache_fallback_reason.empty() && !fs::exists(request.private_directory / "cache") &&
                  tree(f.original) == before,
              "unsupported-volume session did not fall back to a cold private cache");
      result.config.reset();
      require(!fs::exists(request.private_directory), "unsupported-volume rollback left its private base behind");
      ++cold;
    }
  }
  std::cout << "Other-volume checks: " << warm << " junction volume(s), " << cold << " no-reparse volume(s)\n";
}
#endif
// ---- Private storage lifetime --------------------------------------------
constexpr const char *owner_marker_file = ".openxmb-session-owner";
Rpcs3SessionConfig prepared(const Rpcs3SessionRequest &request) {
  auto result = prepare_rpcs3_session(request);
  if (!result || !result.config->storage)
    throw std::runtime_error("storage fixture preparation failed: " + result.message);
  return std::move(*result.config);
}
Rpcs3SessionRequest request_for(const Fixture &f, const fs::path &destination) {
  auto request = f.request();
  request.private_directory = destination;
  return request;
}
fs::path unicode_private(const Fixture &f) {
  return f.root / fs::path(std::u8string(u8"Private \u6771\u4eac \U0001f3ae"));
}
// Linux only: the native cache the session reuses, but only inside the
// temporary directory so a developer's real cache is never touched.
std::optional<fs::path> native_cache_dir() {
#ifdef _WIN32
  return {};
#else
  const auto *value = std::getenv("XDG_CACHE_HOME");
  if (!value)
    return {};
  std::error_code error;
  const auto temp = fs::weakly_canonical(fs::temp_directory_path(), error);
  const auto cache = fs::weakly_canonical(fs::path(value) / "rpcs3", error);
  if (error)
    return {};
  auto at = cache.begin();
  for (auto part = temp.begin(); part != temp.end(); ++part, ++at)
    if (at == cache.end() || *at != *part)
      return {};
  return fs::path(value) / "rpcs3";
#endif
}
// Warm original cache sentinels (and, on Linux, the temp-only native cache):
// bytes and modification times must survive every storage operation.
struct OriginalGuard {
  fs::path original;
  std::optional<fs::path> native;
  std::map<fs::path, std::string> before, native_before;
  explicit OriginalGuard(const Fixture &f) : original(f.original), native(native_cache_dir()) {
    make_cache(original);
    if (native) {
      const auto sentinel = *native / "fixture-warm-sentinel.bin";
      write(sentinel, "native warm cache bytes");
      fs::last_write_time(sentinel, old_time());
    }
    before = tree(original);
    if (native)
      native_before = tree(*native);
  }
  void check(const char *message) const {
    require(tree(original) == before && (!native || tree(*native) == native_before), message);
  }
};
void fallback_policy() {
  const std::map<Rpcs3SessionError, bool> expected{{Rpcs3SessionError::none, false},
                                                   {Rpcs3SessionError::invalid_input, false},
                                                   {Rpcs3SessionError::unsafe_path, false},
                                                   {Rpcs3SessionError::portable_config, true},
                                                   {Rpcs3SessionError::unsupported_vfs, true},
                                                   {Rpcs3SessionError::bounds_exceeded, true},
                                                   {Rpcs3SessionError::io_error, true},
                                                   {Rpcs3SessionError::config_in_use, false},
                                                   {Rpcs3SessionError::instance_lock_unavailable, true},
                                                   {Rpcs3SessionError::unsupported_platform, true}};
  require(expected.size() == 10, "fallback policy fixture omits an error category");
  for (const auto &[error, allowed] : expected)
    require(rpcs3_session_native_fallback(error) == allowed, "native fallback policy changed");
}
void storage_unlaunched(const fs::path &root) {
  Fixture f(root);
  const OriginalGuard guard(f);
  const auto base = unicode_private(f);
  write(f.root / "neighbor.sentinel", "neighbor bytes");
  const auto request = request_for(f, base);
  auto first = prepared(request);
  auto second = first;
  auto third = second;
  require(first.storage == second.storage && second.storage == third.storage,
          "config copies do not share one storage owner");
#ifdef _WIN32
  require(first.private_directory == base && is_junction(base / "cache"),
          "Windows private layout did not link the warm cache");
#else
  require(first.private_directory == base / "rpcs3" && fs::is_directory(base / "rpcs3"),
          "Linux private layout is not <base>/rpcs3");
#endif
  first = Rpcs3SessionConfig{};
  require(fs::is_directory(base), "private storage was removed while config copies remain");
  second = Rpcs3SessionConfig{};
  require(fs::is_directory(base), "private storage was removed while a config copy remains");
  third = Rpcs3SessionConfig{};
  require(!fs::exists(base), "unlaunched final owner left its private base behind");
  require(read(f.root / "neighbor.sentinel") == "neighbor bytes" && fs::is_directory(f.root),
          "rollback touched something besides the private base");
  guard.check("unlaunched rollback changed the original cache or configuration");
  prepared(request); // the destination is reusable and the temporary config rolls back again
  require(!fs::exists(base), "second unlaunched rollback left its private base behind");
  guard.check("second rollback changed the original cache or configuration");
  const Rpcs3SessionConfig none;
  require(!retain_rpcs3_session(none) && cleanup_rpcs3_session(none).error == Rpcs3SessionError::invalid_input,
          "a config without storage was accepted by retain/cleanup");
}
void storage_retained(const fs::path &root) {
  {
    Fixture f(root / "shutdown");
    const OriginalGuard guard(f);
    {
      auto config = prepared(f.request());
      auto copy = config;
      require(retain_rpcs3_session(copy) && retain_rpcs3_session(config), "retain rejected live storage");
    } // OpenXMB shutdown while the game keeps running
    require(fs::is_directory(f.destination) &&
                read(f.managed() / cfg_relative() / "config.yml") == "Core: preserve exactly\r\n",
            "retained storage was removed when its configs were destroyed");
#ifdef _WIN32
    require(is_junction(f.destination / "cache"), "retained storage lost its cache junction");
#endif
    guard.check("retained scope exit changed the original cache or configuration");
    // Documented residue: only explicit later tooling or the fixture removes it.
    fs::remove_all(f.destination);
    guard.check("fixture removal of retained storage followed the cache link");
  }
  Fixture f(root / "exited");
  const OriginalGuard guard(f);
  write(f.root / "neighbor.sentinel", "neighbor bytes");
  write(f.root / "managed-sibling/keep", "sibling bytes");
  auto config = prepared(f.request());
  auto copy = config;
  auto other = config;
  require(retain_rpcs3_session(config), "retain rejected live storage");
  config = Rpcs3SessionConfig{};
  require(fs::is_directory(f.destination), "retained storage was removed by an unrelated copy");
  const auto removed = cleanup_rpcs3_session(copy);
  require(removed && removed.removed && removed.message.empty() && !fs::exists(f.destination),
          "cleanup after confirmed exit did not remove the whole private base");
  require(read(f.root / "neighbor.sentinel") == "neighbor bytes" &&
              read(f.root / "managed-sibling/keep") == "sibling bytes" && fs::is_directory(f.root),
          "cleanup removed more than the private base");
  guard.check("cleanup changed the warm original cache, Unicode root or native cache");
  const auto again = cleanup_rpcs3_session(copy), via_copy = cleanup_rpcs3_session(other);
  require(again && !again.removed && via_copy && !via_copy.removed, "repeated cleanup was not a safe no-op");
  require(!retain_rpcs3_session(copy), "retain accepted retired storage");
  // A directory created later at the same path is never ours.
  fs::create_directory(f.destination);
  write(f.destination / "later.sentinel", "later bytes");
  copy = Rpcs3SessionConfig{};
  other = Rpcs3SessionConfig{};
  require(read(f.destination / "later.sentinel") == "later bytes", "retired storage deleted a later directory");
  // Storage that vanished on its own (or whose whole parent did) is a quiet success.
  Fixture gone(root / "gone");
  auto parents = gone.root / "parents";
  fs::create_directory(parents);
  auto vanished = prepared(request_for(gone, parents / "managed"));
  fs::remove_all(parents / "managed");
  const auto no_root = cleanup_rpcs3_session(vanished);
  require(no_root && !no_root.removed, "externally removed storage was reported as an error");
  vanished = Rpcs3SessionConfig{}; // the original-root lease is exclusive
  vanished = prepared(request_for(gone, parents / "managed2"));
  fs::remove_all(parents);
  const auto no_parent = cleanup_rpcs3_session(vanished);
  require(no_parent && !no_parent.removed, "externally removed parent was reported as an error");
}
void storage_concurrent_cleanup(const fs::path &root) {
  Fixture f(root);
  const auto config = prepared(f.request());
  std::vector<Rpcs3SessionConfig> copies(8, config);
  std::atomic<int> removed{0}, failed{0};
  std::vector<std::thread> threads;
  for (const auto &copy : copies)
    threads.emplace_back([&] {
      const auto result = cleanup_rpcs3_session(copy);
      if (!result)
        ++failed;
      else if (result.removed)
        ++removed;
    });
  for (auto &thread : threads)
    thread.join();
  require(removed == 1 && failed == 0 && !fs::exists(f.destination),
          "concurrent cleanup through shared copies did not remove exactly once");
}
void storage_failed_prepare(const fs::path &root) {
  Fixture f(root);
  write(f.root / "neighbor.sentinel", "neighbor bytes");
#ifdef _WIN32
  // NTFS is case-insensitive while snapshot keys are not: this spelling makes the generated
  // config/vfs.yml collide with the copied file after the private base already exists.
  write(f.source_config / "VFS.YML", "");
  require(fs::exists(f.source_config / "vfs.yml"), "fixture volume is case-sensitive");
  const auto expected = Rpcs3SessionError::unsafe_path;
#else
  // The private copy of this file exceeds RLIMIT_FSIZE; reading it does not.
  write(f.original / "data/big.bin", std::string(6000, 'x'));
  const auto expected = Rpcs3SessionError::io_error;
#endif
  const OriginalGuard guard(f);
#ifdef _WIN32
  const auto result = prepare_rpcs3_session(f.request());
#else
  struct rlimit previous {};
  require(getrlimit(RLIMIT_FSIZE, &previous) == 0, "cannot read RLIMIT_FSIZE");
  auto limited = previous;
  limited.rlim_cur = 4096;
  const auto handler = std::signal(SIGXFSZ, SIG_IGN);
  require(setrlimit(RLIMIT_FSIZE, &limited) == 0, "cannot lower RLIMIT_FSIZE");
  const auto result = prepare_rpcs3_session(f.request());
  setrlimit(RLIMIT_FSIZE, &previous);
  std::signal(SIGXFSZ, handler);
#endif
  require(!result && !result.config && result.error == expected &&
              result.message.find("left in place") == std::string::npos,
          "injected post-creation failure did not fail closed with a clean rollback");
  require(!fs::exists(f.destination), "partial preparation left its private base behind");
  require(read(f.root / "neighbor.sentinel") == "neighbor bytes", "partial rollback touched a neighbor");
  guard.check("partial rollback changed the original cache or configuration");
}
void storage_preexisting(const fs::path &root) {
  Fixture f(root);
  const OriginalGuard guard(f);
  fs::create_directory(f.destination);
  write(f.destination / "keep", "existing destination sentinel");
  {
    const auto result = prepare_rpcs3_session(f.request());
    require(!result && result.error == Rpcs3SessionError::unsafe_path, "existing destination was accepted");
  }
  std::size_t entries = 0;
  for (const auto &entry : fs::directory_iterator(f.destination)) {
    (void)entry;
    ++entries;
  }
  require(read(f.destination / "keep") == "existing destination sentinel" && entries == 1,
          "failed preparation or its destruction changed a pre-existing destination");
  fs::remove_all(f.destination);
  fs::create_directory(f.destination);
  {
    const auto result = prepare_rpcs3_session(f.request());
    require(!result && result.error == Rpcs3SessionError::unsafe_path, "empty existing destination was accepted");
  }
  require(fs::is_directory(f.destination) && fs::is_empty(f.destination),
          "an empty pre-existing destination was removed or populated");
  fs::remove(f.destination);
  write(f.destination, "file destination");
  require(prepare_rpcs3_session(f.request()).error == Rpcs3SessionError::unsafe_path &&
              read(f.destination) == "file destination",
          "a pre-existing file destination was changed");
  guard.check("pre-existing destination handling changed the original");
}
void expect_refused(const Rpcs3SessionConfig &config, const char *message) {
  const auto result = cleanup_rpcs3_session(config);
  require(!result && result.error == Rpcs3SessionError::unsafe_path && !result.removed && !result.message.empty(),
          message);
}
void expect_removed(const Rpcs3SessionConfig &config, const fs::path &base, const char *message) {
  const auto result = cleanup_rpcs3_session(config);
  require(result && result.removed && !fs::exists(base), message);
}
void storage_replaced_root(const fs::path &root) {
  {
    Fixture f(root / "moved");
    const OriginalGuard guard(f);
    auto config = prepared(f.request());
    const auto moved = f.root / "moved-session";
    fs::rename(f.destination, moved);
    fs::create_directory(f.destination);
    write(f.destination / "replacement.sentinel", "replacement");
    const auto moved_before = tree(moved);
    expect_refused(config, "cleanup deleted a replacement directory");
    config = Rpcs3SessionConfig{}; // the unlaunched destructor refuses as well
    require(read(f.destination / "replacement.sentinel") == "replacement" && tree(moved) == moved_before,
            "a replaced root was deleted or its original moved contents changed");
    guard.check("replaced-root refusal changed the original");
  }
  {
    Fixture f(root / "restored");
    const OriginalGuard guard(f);
    auto config = prepared(f.request());
    const auto moved = f.root / "moved-session";
    fs::rename(f.destination, moved);
    fs::create_directory(f.destination);
    write(f.destination / "replacement.sentinel", "replacement");
    expect_refused(config, "cleanup deleted a replacement directory");
    fs::remove_all(f.destination);
    fs::rename(moved, f.destination);
    expect_removed(config, f.destination, "cleanup did not recover after the owned root returned");
    guard.check("recovered cleanup changed the original");
  }
  {
    Fixture f(root / "linked");
    const OriginalGuard guard(f);
    auto config = prepared(f.request());
    const auto moved = f.root / "moved-session";
    fs::rename(f.destination, moved);
    require(directory_link(moved, f.destination), "cannot create the replacement link fixture");
    const auto moved_before = tree(moved);
    expect_refused(config, "cleanup followed a replacement link or reparse point");
    config = Rpcs3SessionConfig{};
    require(tree(moved) == moved_before && fs::exists(moved / owner_marker_file),
            "a replacement link was followed and its target deleted");
    remove_link(f.destination);
    guard.check("link refusal changed the original");
  }
  {
    Fixture f(root / "recreated");
    const OriginalGuard guard(f);
    auto config = prepared(f.request());
    fs::remove_all(f.destination);
    fs::create_directory(f.destination); // a recreated directory may even reuse the file id
    write(f.destination / "replacement.sentinel", "replacement");
    expect_refused(config, "cleanup deleted a recreated directory");
    config = Rpcs3SessionConfig{};
    require(read(f.destination / "replacement.sentinel") == "replacement",
            "an unlaunched destructor deleted a recreated directory");
    guard.check("recreated-root refusal changed the original");
  }
  {
    Fixture f(root / "marker");
    const OriginalGuard guard(f);
    auto config = prepared(f.request());
    const auto marker = f.destination / owner_marker_file;
    const auto token = read(marker);
    require(!token.empty(), "private base has no owner marker");
    write(marker, "someone else");
    expect_refused(config, "cleanup accepted a different owner marker");
    fs::remove(marker);
    expect_refused(config, "cleanup accepted a missing owner marker");
    require(fs::exists(f.managed() / cfg_relative() / "config.yml"), "refusal deleted private contents");
    write(marker, token);
    expect_removed(config, f.destination, "restored owner marker was not accepted");
    guard.check("marker refusal changed the original");
  }
}
void storage_replaced_parent(const fs::path &root) {
  {
    Fixture f(root / "link");
    const OriginalGuard guard(f);
    const auto parents = f.root / "parents";
    fs::create_directory(parents);
    auto config = prepared(request_for(f, parents / "managed"));
    const auto moved = f.root / "parents-moved";
    fs::rename(parents, moved);
    require(directory_link(moved, parents), "cannot create the parent link fixture");
    expect_refused(config, "cleanup followed a replaced parent link");
    require(fs::exists(moved / "managed" / owner_marker_file), "parent-link refusal deleted the target");
    remove_link(parents);
    fs::rename(moved, parents);
    expect_removed(config, parents / "managed", "cleanup did not recover after the parent returned");
    guard.check("parent-link refusal changed the original");
  }
  Fixture f(root / "lookalike");
  const OriginalGuard guard(f);
  const auto parents = f.root / "parents";
  fs::create_directory(parents);
  auto config = prepared(request_for(f, parents / "managed"));
  const auto moved = f.root / "parents-moved";
  fs::rename(parents, moved);
  fs::create_directories(parents / "managed");
  fs::copy_file(moved / "managed" / owner_marker_file, parents / "managed" / owner_marker_file);
  write(parents / "managed/lookalike.sentinel", "lookalike");
  expect_refused(config, "cleanup trusted a lookalike under a replaced parent");
  require(read(parents / "managed/lookalike.sentinel") == "lookalike" &&
              fs::exists(moved / "managed" / owner_marker_file),
          "parent replacement refusal deleted something");
  fs::remove_all(parents);
  fs::rename(moved, parents);
  expect_removed(config, parents / "managed", "cleanup did not recover after the real parent returned");
  guard.check("parent replacement refusal changed the original");
}
void storage_cleanup_retry(const fs::path &root) {
  Fixture f(root);
  const OriginalGuard guard(f);
  auto config = prepared(f.request());
  const auto hold = f.managed() / "GuiConfigs";
#ifdef _WIN32
  const HANDLE locked = CreateFileW((hold / "CurrentSettings.ini").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(locked != INVALID_HANDLE_VALUE, "cannot open the locked-file fixture");
  const auto blocked = cleanup_rpcs3_session(config);
  CloseHandle(locked);
#else
  if (geteuid() == 0) {
    std::cout << "Cleanup retry fixture skipped: permissions do not constrain root\n";
    return;
  }
  fs::permissions(hold, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
  const auto blocked = cleanup_rpcs3_session(config);
  fs::permissions(hold, fs::perms::owner_all, fs::perm_options::replace);
#endif
  require(!blocked && blocked.error == Rpcs3SessionError::io_error && !blocked.removed && fs::exists(hold),
          "a partially failed cleanup was not reported as io_error");
  expect_removed(config, f.destination, "cleanup could not be retried after a partial failure");
  guard.check("cleanup retry changed the original");
}
void cleanup_fixture(const fs::path &root) {
  require(fs::equivalent(root.parent_path(), fs::temp_directory_path()) &&
              root.filename().string().starts_with("openxmb_rpcs3_session_fixture_"),
          "unsafe fixture cleanup target");
  fs::remove_all(root);
}
#ifdef __APPLE__
void unsupported_platform_contract(const fs::path& root) {
  Fixture f(root);std::map<fs::path,std::pair<std::string,fs::file_time_type>> source;
  for(const auto& entry:fs::recursive_directory_iterator(f.original))if(entry.is_regular_file())source[entry.path()]={read(entry.path()),entry.last_write_time()};
  std::map<std::string,std::optional<std::string>> environment;
  for(const auto* name:{"RPCS3_CONFIG_DIR","XDG_CONFIG_HOME","XDG_CACHE_HOME","HOME"}){const auto* value=std::getenv(name);environment[name]=value?std::optional<std::string>(value):std::nullopt;}
  const auto executable_time=fs::last_write_time(f.exe);const auto result=prepare_rpcs3_session(f.request());
  require(!result&&result.error==Rpcs3SessionError::unsupported_platform&&!result.config&&!fs::exists(f.destination),"macOS must reject managed isolation without creating a private destination");
  require(result.message.find("normal direct launch")!=std::string::npos,"macOS unsupported result did not describe the ordinary direct-launch fallback");
  for(const auto& [path,value]:source)require(read(path)==value.first&&fs::last_write_time(path)==value.second,"unsupported macOS preparation modified source preferences or data");
  require(read(f.exe)=="isolated executable fixture"&&fs::last_write_time(f.exe)==executable_time,"unsupported macOS preparation modified the selected executable");
  for(const auto& [name,before]:environment){const auto* value=std::getenv(name.c_str());require((value?std::optional<std::string>(value):std::nullopt)==before,"unsupported macOS preparation changed the host environment");}
}
#endif
void native_root_probe(const fs::path &root, std::string_view mode) {
  require(root.is_absolute() && fs::equivalent(root.parent_path(), fs::temp_directory_path()) &&
              root.filename().string().starts_with("openxmb_rpcs3_session_fixture_") && !fs::exists(root),
          "unsafe native-root fixture target");
  Fixture f(root);
  fs::path expected;
#ifndef _WIN32
  if (mode == "cache-unavailable") {
    const auto result = prepare_rpcs3_session(f.request());
    require(result.error == Rpcs3SessionError::instance_lock_unavailable && !fs::exists(f.destination),
            "unresolvable original Linux cache/instance-lock directory was silently relocated");
    cleanup_fixture(root);
    std::cout << "Unavailable native cache fixture passed\n";
    return;
  }
#endif
  if (mode == "environment")
    expected = f.original;
  else if (mode == "executable")
    expected = f.exe.parent_path();
  else if (mode == "xdg")
    expected = root / "native/rpcs3";
  else if (mode == "home")
    expected = root / "home/.config/rpcs3";
  else
    throw std::runtime_error("unknown native-root fixture mode");
  write(expected / cfg_relative() / "config.yml", "native-root fixture\n");
  write(expected / "RPCS3.buf", "");
  write(expected / "GuiConfigs/persistent_settings.dat", "[Users]\nactive_user=00000004\n");
  auto request = f.request();
  request.effective_config_root.clear();
  auto result = prepare_rpcs3_session(request);
  require(static_cast<bool>(result) && result.config->dev_hdd0 == expected / "dev_hdd0" &&
              result.config->user_id == "00000004",
          "empty-root request did not preserve native RPCS3 config-root/user semantics");
#ifndef _WIN32
  const auto cache_parent = [] {
    if (const auto *value = std::getenv("XDG_CACHE_HOME"))
      return std::string(value);
    if (const auto *value = std::getenv("XDG_CONFIG_HOME"))
      return std::string(value);
    if (const auto *value = std::getenv("HOME"))
      return std::string(value) + "/.cache";
    return std::string{};
  }();
  require(result.config->environment_overrides.at("XDG_CACHE_HOME") == cache_parent &&
              !result.config->lease && result.config->inherited_handles.empty(),
          "Linux managed layout lost the original native cache/instance-lock path");
#endif
  result.config.reset();
  cleanup_fixture(root);
  std::cout << "Native RPCS3 config-root fixture passed\n";
}
} // namespace
int main(int argc, char **argv) {
#ifdef __APPLE__
  // macOS commonly exposes its temporary directory through /var or /tmp
  // symlinks; use the physical path so the fixture reaches platform rejection.
  const auto temporary = fs::canonical(fs::temp_directory_path());
#else
  const auto temporary = fs::temp_directory_path();
#endif
  const auto root = temporary / ("openxmb_rpcs3_session_fixture_" +
                                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
#ifdef __APPLE__
    unsupported_platform_contract(root/"unsupported-macos");
    cleanup_fixture(root);
    std::cout<<"Managed RPCS3 macOS unsupported contract passed\n";
    return 0;
#endif
    if (argc == 2 && std::string_view(argv[1]) == "--lease-only") {
      instance_lease(root / "lease");
      cleanup_fixture(root);
      std::cout << "Instance lease fixtures passed\n";
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--icons-only") {
      icons(root / "icons");
      cleanup_fixture(root);
      std::cout << "Icon snapshot fixtures passed\n";
      return 0;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--native-root") {
      native_root_probe(fs::path(argv[2]), argv[3]);
      return 0;
    }
    defaults(root / "defaults");
    custom_vfs(root / "custom");
    rejection(root / "rejections");
    unsupported_vfs(root / "unsupported");
    limits_and_links(root / "limits");
    other_bounds(root / "other-bounds");
    ini_and_user_edges(root / "ini-user");
    instance_lease(root / "lease");
    icons(root / "icons");
    fallback_policy();
    storage_unlaunched(root / "storage-unlaunched");
    storage_retained(root / "storage-retained");
    storage_concurrent_cleanup(root / "storage-concurrent");
    storage_failed_prepare(root / "storage-failed");
    storage_preexisting(root / "storage-preexisting");
    storage_replaced_root(root / "storage-root");
    storage_replaced_parent(root / "storage-parent");
    storage_cleanup_retry(root / "storage-retry");
#ifdef _WIN32
    warm_cache(root / "warm");
    cold_cache(root / "cold");
    other_volumes(root / "volumes");
#endif
    cleanup_fixture(root);
    std::cout << "Managed RPCS3 session fixtures passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
