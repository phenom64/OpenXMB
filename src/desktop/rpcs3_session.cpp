#include "openxmb/desktop/rpcs3_session.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <sddl.h>
#include <winioctl.h>

#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
namespace openxmb::desktop {
namespace {
constexpr std::size_t max_entries = 4096, max_depth = 12, max_file = 8 * 1024 * 1024,
                      max_total = 64 * 1024 * 1024;
struct Failure {
  Rpcs3SessionError code;
  std::string message;
};
[[noreturn]] void fail(Rpcs3SessionError code, const char *message) { throw Failure{code, message}; }
std::string utf8(const fs::path &path) {
  const auto value = path.generic_u8string();
  return {value.begin(), value.end()};
}
fs::path text_path(std::string_view text) { return fs::path(std::u8string(text.begin(), text.end())); }
std::string slash(std::string value) {
#ifdef _WIN32
  std::replace(value.begin(), value.end(), '\\', '/');
#endif
  return value;
}
std::string trailing(std::string value) {
  if (value.empty() || value.back() != '/')
    value += '/';
  return value;
}
fs::path no_trailing(fs::path path) {
  while (path.has_relative_path() && path.filename().empty())
    path = path.parent_path();
  return path;
}
std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r'))
    value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
    value.remove_suffix(1);
  return value;
}
void valid_text(std::string_view value, Rpcs3SessionError error) {
  for (std::size_t at = 0; at < value.size();) {
    const auto first = static_cast<unsigned char>(value[at++]);
    if (first < 0x20 || first == 0x7f)
      fail(error, "Control characters in configuration scalars are unsupported");
    if (first < 0x80)
      continue;
    const unsigned length = first >= 0xc2 && first <= 0xdf   ? 2
                            : first >= 0xe0 && first <= 0xef ? 3
                            : first >= 0xf0 && first <= 0xf4 ? 4
                                                             : 0;
    if (!length || value.size() - at < length - 1)
      fail(error, "Configuration text must contain valid UTF-8");
    std::uint32_t codepoint = first & (0x7f >> length);
    for (unsigned i = 1; i < length; ++i) {
      const auto next = static_cast<unsigned char>(value[at++]);
      if ((next & 0xc0) != 0x80)
        fail(error, "Configuration text must contain valid UTF-8");
      codepoint = (codepoint << 6) | (next & 63);
    }
    if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
        (length == 4 && codepoint < 0x10000) || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff))
      fail(error, "Configuration text must contain valid Unicode");
  }
}
void append_utf8(std::string &text, std::uint32_t value) {
  if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
    fail(Rpcs3SessionError::unsupported_vfs, "Unsupported VFS Unicode escape");
  if (value < 0x80)
    text += static_cast<char>(value);
  else if (value < 0x800) {
    text += static_cast<char>(0xc0 | (value >> 6));
    text += static_cast<char>(0x80 | (value & 63));
  } else if (value < 0x10000) {
    text += static_cast<char>(0xe0 | (value >> 12));
    text += static_cast<char>(0x80 | ((value >> 6) & 63));
    text += static_cast<char>(0x80 | (value & 63));
  } else {
    text += static_cast<char>(0xf0 | (value >> 18));
    text += static_cast<char>(0x80 | ((value >> 12) & 63));
    text += static_cast<char>(0x80 | ((value >> 6) & 63));
    text += static_cast<char>(0x80 | (value & 63));
  }
}
bool path_exists(const fs::path &path) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory)
    fail(Rpcs3SessionError::io_error, "Cannot inspect a configuration path");
  return status.type() != fs::file_type::not_found;
}
void no_link(const fs::path &path) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error)
    fail(Rpcs3SessionError::io_error, "Cannot inspect a configuration source entry");
  if (fs::is_symlink(status))
    fail(Rpcs3SessionError::unsafe_path, "Links in managed configuration paths are unsupported");
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES)
    fail(Rpcs3SessionError::io_error, "Cannot inspect Windows configuration attributes");
  if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
    fail(Rpcs3SessionError::unsafe_path, "Reparse points in managed configuration paths are unsupported");
#endif
}
void existing_chain(const fs::path &path) {
  fs::path current;
  for (const auto &part : path) {
    current /= part;
    if (current == path.root_name())
      continue;
    no_link(current);
  }
}
void absolute_input(const fs::path &path) {
  if (path.empty() || !path.is_absolute())
    fail(Rpcs3SessionError::invalid_input,
         "RPCS3 configuration, executable and destination paths must be absolute");
  valid_text(utf8(path), Rpcs3SessionError::invalid_input);
  for (const auto &part : path)
    if (part == "." || part == "..")
      fail(Rpcs3SessionError::unsafe_path, "Dot segments in managed configuration paths are unsupported");
}
bool path_part_equal(const fs::path &left, const fs::path &right) {
#ifdef _WIN32
  auto a = left.native(), b = right.native();
  std::transform(a.begin(), a.end(), a.begin(), [](wchar_t c) { return std::towlower(c); });
  std::transform(b.begin(), b.end(), b.begin(), [](wchar_t c) { return std::towlower(c); });
  return a == b;
#else
  return left == right;
#endif
}
bool contains(const fs::path &parent, const fs::path &child) {
  auto a = parent.begin(), b = child.begin();
  for (; a != parent.end(); ++a, ++b)
    if (b == child.end() || !path_part_equal(*a, *b))
      return false;
  return true;
}
std::optional<fs::path> environment_path(const char *name) {
#ifdef _WIN32
  const std::wstring key(name, name + std::char_traits<char>::length(name));
  SetLastError(ERROR_SUCCESS);
  const auto size = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
  if (!size) {
    if (GetLastError() == ERROR_ENVVAR_NOT_FOUND || GetLastError() == ERROR_SUCCESS)
      return {};
    fail(Rpcs3SessionError::io_error, "Cannot read the RPCS3 config-root environment");
  }
  std::wstring value(size, L'\0');
  if (GetEnvironmentVariableW(key.c_str(), value.data(), size) != size - 1)
    fail(Rpcs3SessionError::io_error, "RPCS3 config-root environment changed during preparation");
  value.resize(size - 1);
  return fs::path(value);
#else
  if (const auto *value = std::getenv(name))
    return text_path(value);
  return {};
#endif
}
fs::path native_root(const fs::path &executable) {
  (void)executable;
#ifdef _WIN32
  if (const auto value = environment_path("RPCS3_CONFIG_DIR")) {
    auto text = slash(utf8(*value));
    const auto last = text.rfind('/');
    if (last == std::string::npos)
      fail(Rpcs3SessionError::invalid_input, "RPCS3_CONFIG_DIR does not resolve to an absolute directory");
    return no_trailing(text_path(text.substr(0, last + 1)));
  }
  return executable.parent_path();
#elif defined(__APPLE__)
  if (const auto home = environment_path("HOME"))
    return text_path(utf8(*home) + "/Library/Application Support/rpcs3");
  fail(Rpcs3SessionError::invalid_input,
       "Set an explicit effective RPCS3 configuration root when HOME is unavailable");
#else
  if (const auto xdg = environment_path("XDG_CONFIG_HOME"))
    return text_path(utf8(*xdg) + "/rpcs3");
  if (const auto home = environment_path("HOME"))
    return text_path(utf8(*home) + "/.config/rpcs3");
  fail(Rpcs3SessionError::invalid_input,
       "Set an explicit effective RPCS3 configuration root when HOME and XDG_CONFIG_HOME are unavailable");
#endif
}
fs::path config_relative() {
#ifdef _WIN32
  return "config";
#else
  return {};
#endif
}
#ifdef _WIN32
std::shared_ptr<void> acquire_instance_lease(const fs::path &original) {
  const auto lock_path = original / "RPCS3.buf";
  const HANDLE handle = CreateFileW(lock_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
      fail(Rpcs3SessionError::config_in_use, "The original RPCS3 configuration is in use; close its active "
                                             "instance before starting a managed session");
    fail(Rpcs3SessionError::instance_lock_unavailable,
         "The original RPCS3 instance lock is missing or inaccessible; use an ordinary direct launch to "
         "initialize the installation");
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle, &info) || GetFileType(handle) != FILE_TYPE_DISK ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
    CloseHandle(handle);
    fail(Rpcs3SessionError::instance_lock_unavailable,
         "The original RPCS3 instance lock is not a regular file without a reparse point");
  }
  return std::shared_ptr<void>(handle, [](void *value) { CloseHandle(value); });
}
void owner_directory(const fs::path &path);
// Warm-cache route. Upstream Windows RPCS3 keeps its PPU/SPU/shader caches in
// <config root>/cache and offers no independent cache override, so the private
// config root would otherwise start cold on every launch. The private cache is
// an NTFS directory junction to the canonical original cache. Every failure
// degrades to the previous cold private cache; nothing below creates, writes,
// deletes or enumerates the original cache. Only the brand-new private link is
// created (and, on failure, removed with RemoveDirectoryW, which never
// follows a junction).
struct CacheLink {
  fs::path directory;
  std::string reason;
};
struct Handle {
  HANDLE value;
  explicit Handle(HANDLE handle) : value(handle) {}
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  ~Handle() {
    if (value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
  explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};
Handle open_directory(const std::wstring &path, DWORD access, bool follow_reparse) {
  return Handle(CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | (follow_reparse ? 0 : FILE_FLAG_OPEN_REPARSE_POINT),
                            nullptr));
}
// Identity of an opened directory; empty unless it is a directory handle.
std::optional<FILE_ID_INFO> directory_identity(HANDLE handle, bool require_plain) {
  BY_HANDLE_FILE_INFORMATION info{};
  FILE_ID_INFO identity{};
  if (!GetFileInformationByHandle(handle, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (require_plain && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) ||
      !GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity)))
    return {};
  return identity;
}
bool same_identity(const FILE_ID_INFO &left, const FILE_ID_INFO &right) {
  return left.VolumeSerialNumber == right.VolumeSerialNumber &&
         std::memcmp(&left.FileId, &right.FileId, sizeof(left.FileId)) == 0;
}
std::wstring final_path(HANDLE handle) {
  std::wstring value(1024, L'\0');
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto size = GetFinalPathNameByHandleW(handle, value.data(), static_cast<DWORD>(value.size()),
                                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!size)
      return {};
    if (size < value.size()) {
      value.resize(size);
      return value;
    }
    value.assign(size + 1, L'\0');
  }
  return {};
}
std::wstring lower_text(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
  return value;
}
// A local drive-letter path without any Win32 namespace prefix.
bool local_drive_path(const std::wstring &value) {
  return value.size() > 3 && ((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z')) &&
         value[1] == L':' && value[2] == L'\\' && value.back() != L'\\';
}
std::wstring drop_namespace_prefix(std::wstring value) {
  if (value.starts_with(L"\\\\?\\") && !value.starts_with(L"\\\\?\\UNC\\"))
    value.erase(0, 4);
  return value;
}
bool volume_of(const std::wstring &path, std::wstring &volume) {
  volume.assign(32768, L'\0');
  if (!GetVolumePathNameW(path.c_str(), volume.data(), static_cast<DWORD>(volume.size())))
    return false;
  volume.resize(std::wcslen(volume.c_str()));
  return !volume.empty();
}
bool set_junction(const fs::path &link, const std::wstring &target) {
  const std::wstring substitute = L"\\??\\" + target;
  const auto path_bytes = (substitute.size() + target.size() + 2) * sizeof(wchar_t);
  if (path_bytes > 0xffff - 16)
    return false;
  std::vector<unsigned char> bytes(16 + path_bytes, 0);
  const auto put16 = [&](std::size_t at, std::size_t value) {
    bytes[at] = static_cast<unsigned char>(value & 0xff);
    bytes[at + 1] = static_cast<unsigned char>(value >> 8);
  };
  const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
  std::memcpy(bytes.data(), &tag, sizeof(tag));
  put16(4, 8 + path_bytes);
  put16(8, 0);
  put16(10, substitute.size() * sizeof(wchar_t));
  put16(12, (substitute.size() + 1) * sizeof(wchar_t));
  put16(14, target.size() * sizeof(wchar_t));
  std::memcpy(bytes.data() + 16, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
  std::memcpy(bytes.data() + 16 + (substitute.size() + 1) * sizeof(wchar_t), target.c_str(),
              (target.size() + 1) * sizeof(wchar_t));
  Handle handle(CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  DWORD returned = 0;
  return handle && DeviceIoControl(handle.value, FSCTL_SET_REPARSE_POINT, bytes.data(),
                                   static_cast<DWORD>(bytes.size()), nullptr, 0, &returned, nullptr);
}
CacheLink link_original_cache(const fs::path &physical_original, const fs::path &base) {
  const fs::path link = base / "cache";
  bool created = false;
  const auto cold = [&](const char *reason) {
    if (created)
      RemoveDirectoryW(link.c_str());
    return CacheLink{{}, reason};
  };
  try {
    const auto source = physical_original / "cache";
    if (!path_exists(source))
      return cold("The original RPCS3 cache does not exist yet; this session starts with a cold private "
                  "cache.");
    existing_chain(source);
    if (!fs::is_directory(source))
      return cold("The original RPCS3 cache is not a directory; this session starts with a cold private "
                  "cache.");
    const auto target = fs::canonical(source);
    const auto target_text = drop_namespace_prefix(target.native());
    if (!local_drive_path(target_text) || target.filename().empty() || !path_part_equal(target.filename(), "cache") ||
        !contains(physical_original, target.parent_path()) || !contains(target.parent_path(), physical_original))
      return cold("The original RPCS3 cache does not resolve to a local <config root>/cache directory; this "
                  "session starts with a cold private cache.");
    std::wstring target_volume, link_volume;
    wchar_t flags_name[MAX_PATH + 1]{};
    DWORD volume_flags = 0;
    if (!volume_of(target_text, target_volume) || GetDriveTypeW(target_volume.c_str()) != DRIVE_FIXED)
      return cold("The original RPCS3 cache is not on a fixed local volume; this session starts with a cold "
                  "private cache.");
    if (!volume_of(base.parent_path().native(), link_volume) ||
        !GetVolumeInformationW(link_volume.c_str(), nullptr, 0, nullptr, nullptr, &volume_flags, flags_name,
                               MAX_PATH) ||
        !(volume_flags & FILE_SUPPORTS_REPARSE_POINTS))
      return cold("The private session volume does not support directory junctions; this session starts with "
                  "a cold private cache.");
    // Pin the intended cache directory by identity without following links.
    Handle original(open_directory(target_text, FILE_READ_ATTRIBUTES, false));
    const auto intended = original ? directory_identity(original.value, true) : std::nullopt;
    if (!intended || lower_text(drop_namespace_prefix(final_path(original.value))) != lower_text(target_text))
      return cold("The original RPCS3 cache changed or is not a plain directory; this session starts with a "
                  "cold private cache.");
    owner_directory(link);
    created = true;
    if (!set_junction(link, target_text))
      return cold("The private cache junction could not be created; this session starts with a cold private "
                  "cache.");
    {
      Handle own(open_directory(link.native(), FILE_READ_ATTRIBUTES, false));
      BY_HANDLE_FILE_INFORMATION info{};
      if (!own || !GetFileInformationByHandle(own.value, &info) ||
          !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return cold("The private cache junction could not be verified; this session starts with a cold "
                    "private cache.");
    }
    Handle resolved(open_directory(link.native(), FILE_READ_ATTRIBUTES, true));
    const auto actual = resolved ? directory_identity(resolved.value, true) : std::nullopt;
    if (!actual || !same_identity(*actual, *intended))
      return cold("The private cache junction does not resolve to the original cache; this session starts "
                  "with a cold private cache.");
    return {target, {}};
  } catch (const Failure &) {
    return cold("The original RPCS3 cache contains a link or cannot be inspected safely; this session starts "
                "with a cold private cache.");
  } catch (const std::exception &) {
    return cold("The original RPCS3 cache could not be inspected safely; this session starts with a cold "
                "private cache.");
  }
}
#else
std::string original_cache_parent() {
  std::optional<fs::path> parent;
#ifdef __APPLE__
  if (const auto home = environment_path("HOME"))
    parent = text_path(utf8(*home) + "/Library/Caches");
#else
  if (const auto cache = environment_path("XDG_CACHE_HOME"))
    parent = *cache;
  else if (const auto config = environment_path("XDG_CONFIG_HOME"))
    parent = *config;
  else if (const auto home = environment_path("HOME"))
    parent = text_path(utf8(*home) + "/.cache");
#endif
  if (!parent || !text_path(utf8(*parent) + "/rpcs3").is_absolute())
    fail(Rpcs3SessionError::instance_lock_unavailable,
         "The original RPCS3 cache/instance-lock directory has no absolute HOME or XDG basis; use an "
         "ordinary direct launch or provide an absolute XDG_CACHE_HOME");
  valid_text(utf8(*parent), Rpcs3SessionError::instance_lock_unavailable);
  return utf8(*parent);
}
#endif
struct SnapshotFile {
  std::string bytes;
  fs::file_time_type time;
  bool retain_time{true};
};
struct Snapshot {
  std::map<fs::path, SnapshotFile> files;
  std::set<fs::path> directories;
  std::size_t entries{}, bytes{};
  void count() {
    if (++entries > max_entries)
      fail(Rpcs3SessionError::bounds_exceeded, "RPCS3 preference snapshot exceeds 4096 entries");
  }
  void capture(const fs::path &source, const fs::path &relative, std::size_t depth = 0) {
    if (depth > max_depth)
      fail(Rpcs3SessionError::bounds_exceeded, "RPCS3 preference snapshot exceeds 12 directory levels");
    no_link(source);
    count();
    if (fs::is_directory(source)) {
      directories.insert(relative);
      for (const auto &entry : fs::directory_iterator(source))
        capture(entry.path(), relative / entry.path().filename(), depth + 1);
      return;
    }
    if (!fs::is_regular_file(source))
      fail(Rpcs3SessionError::unsafe_path,
           "Only regular preference files and directories can be snapshotted");
    const auto size = fs::file_size(source);
    if (size > max_file || size > max_total - bytes)
      fail(Rpcs3SessionError::bounds_exceeded,
           "RPCS3 preference snapshot exceeds its 8 MiB/file or 64 MiB total limit");
    const auto time = fs::last_write_time(source);
    std::ifstream input(source, std::ios::binary);
    if (!input)
      fail(Rpcs3SessionError::io_error, "Cannot read an RPCS3 preference file");
    std::string content(static_cast<std::size_t>(size) + 1, '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    const auto read_size = static_cast<std::size_t>(input.gcount());
    if (input.bad() || read_size != size)
      fail(Rpcs3SessionError::io_error, "RPCS3 preferences changed or could not be read completely");
    no_link(source);
    if (fs::file_size(source) != size || fs::last_write_time(source) != time)
      fail(Rpcs3SessionError::io_error,
           "RPCS3 preferences changed during the snapshot; retry after closing its settings dialog");
    content.resize(read_size);
    bytes += read_size;
    files.emplace(relative, SnapshotFile{std::move(content), time, true});
  }
  std::string get(const fs::path &relative) const {
    const auto found = files.find(relative);
    return found == files.end() ? std::string{} : found->second.bytes;
  }
  void replace(const fs::path &relative, std::string content) {
    const auto found = files.find(relative);
    if (found != files.end())
      bytes -= found->second.bytes.size();
    else
      count();
    if (content.size() > max_file || content.size() > max_total - bytes)
      fail(Rpcs3SessionError::bounds_exceeded,
           "Generated RPCS3 preferences exceed the bounded snapshot limit");
    bytes += content.size();
    files[relative] = {std::move(content), {}, false};
  }
};
Snapshot snapshot(const fs::path &original) {
  Snapshot result;
  for (const auto *name : {"GuiConfigs", "patches", "data", "Icons/ui", "Icons/game_icons"})
    if (path_exists(original / name)) {
      if (!fs::is_directory(original / name))
        fail(Rpcs3SessionError::unsafe_path, "RPCS3 preference trees must be directories");
      result.capture(original / name, name);
    }
#ifdef _WIN32
  if (path_exists(original / "config")) {
    if (!fs::is_directory(original / "config"))
      fail(Rpcs3SessionError::unsafe_path, "RPCS3 config must be a directory");
    result.capture(original / "config", "config");
  }
  if (!result.files.contains("config/config.yml") && path_exists(original / "config.yml"))
    result.capture(original / "config.yml", "config/config.yml");
#else
  for (const auto *name : {"custom_configs", "input_configs"})
    if (path_exists(original / name))
      result.capture(original / name, name);
  for (const auto &entry : fs::directory_iterator(original)) {
    const auto name = entry.path().filename();
    const auto extension = name.extension();
    if (extension == ".yml" || extension == ".yaml" || name == "uuid")
      result.capture(entry.path(), name);
  }
#endif
  return result;
}
struct YamlNode {
  bool scalar{};
  std::string value;
  std::map<std::string, YamlNode> children;
  std::optional<std::size_t> child_indent;
};
std::string scalar(std::string_view line, std::size_t &at, bool key) {
  using E = Rpcs3SessionError;
  if (at == line.size())
    fail(E::unsupported_vfs, "VFS scalar is missing; use RPCS3's standard block-map format");
  const char quote = line[at] == '\'' || line[at] == '"' ? line[at++] : '\0';
  std::string value;
  if (quote) {
    bool closed = false;
    while (at < line.size()) {
      char c = line[at++];
      if (c == quote) {
        if (quote == '\'' && at < line.size() && line[at] == '\'') {
          value += '\'';
          ++at;
          continue;
        }
        closed = true;
        break;
      }
      if (quote != '"' || c != '\\') {
        value += c;
        continue;
      }
      if (at == line.size())
        fail(E::unsupported_vfs, "Truncated VFS YAML escape");
      c = line[at++];
      if (c == '"' || c == '\\' || c == '/' || c == ' ') {
        value += c;
        continue;
      }
      if (c != 'x' && c != 'u' && c != 'U')
        fail(E::unsupported_vfs, "Unsupported VFS YAML escape; use literal path strings");
      const unsigned count = c == 'x' ? 2 : c == 'u' ? 4 : 8;
      if (line.size() - at < count)
        fail(E::unsupported_vfs, "Truncated VFS Unicode escape");
      std::uint32_t codepoint = 0;
      for (unsigned i = 0; i < count; ++i) {
        const char digit = line[at++];
        const int number = digit >= '0' && digit <= '9'   ? digit - '0'
                           : digit >= 'a' && digit <= 'f' ? digit - 'a' + 10
                           : digit >= 'A' && digit <= 'F' ? digit - 'A' + 10
                                                          : -1;
        if (number < 0)
          fail(E::unsupported_vfs, "Invalid VFS Unicode escape");
        codepoint = (codepoint << 4) | static_cast<unsigned>(number);
      }
      append_utf8(value, codepoint);
    }
    if (!closed)
      fail(E::unsupported_vfs, "Unterminated VFS YAML scalar");
  } else {
    const auto first = line[at];
    if (std::string_view("!&*{}[],#|>\"'%@`").find(first) != std::string_view::npos ||
        ((first == '-' || first == '?' || first == ':') && (at + 1 == line.size() || line[at + 1] == ' ')))
      fail(E::unsupported_vfs, "VFS aliases, tags, collections and multiline values are unsupported");
    const auto start = at;
    while (at < line.size()) {
      if (line[at] == ':' && (at + 1 == line.size() || line[at + 1] == ' ')) {
        if (key)
          break;
        fail(E::unsupported_vfs, "Unexpected nested VFS scalar mapping");
      }
      if (line[at] == '#' && at > start && line[at - 1] == ' ')
        break;
      ++at;
    }
    value = trim(line.substr(start, at - start));
  }
  if (value.size() > 32768 || (key && value.empty()))
    fail(E::unsupported_vfs, "Unsupported VFS scalar length");
  valid_text(value, E::unsupported_vfs);
  return value;
}
YamlNode parse_vfs(std::string_view bytes) {
  using E = Rpcs3SessionError;
  if (bytes.size() > 256 * 1024)
    fail(E::bounds_exceeded, "VFS configuration exceeds 256 KiB");
  if (bytes.starts_with("\xef\xbb\xbf"))
    bytes.remove_prefix(3);
  YamlNode root;
  struct Level {
    int indent;
    YamlNode *node;
  };
  std::vector<Level> stack{{-1, &root}};
  std::size_t entries = 0;
  while (!bytes.empty()) {
    const auto newline = bytes.find('\n');
    auto line = bytes.substr(0, newline);
    bytes = newline == std::string_view::npos ? std::string_view{} : bytes.substr(newline + 1);
    if (line.ends_with('\r'))
      line.remove_suffix(1);
    if (line.size() > 65536)
      fail(E::bounds_exceeded, "VFS configuration line exceeds its bound");
    std::size_t at = 0;
    while (at < line.size() && line[at] == ' ')
      ++at;
    if (at == line.size() || line[at] == '#')
      continue;
    if (line[at] == '\t')
      fail(E::unsupported_vfs, "Tab-indented VFS YAML is unsupported");
    if (trim(line) == "{}" && stack.size() == 1 && root.children.empty() && trim(bytes).empty())
      return root;
    const auto indent = at;
    while (stack.back().indent >= static_cast<int>(indent))
      stack.pop_back();
    auto &parent = *stack.back().node;
    if ((stack.size() == 1 && indent != 0) || (parent.child_indent && *parent.child_indent != indent))
      fail(E::unsupported_vfs, "Inconsistent VFS YAML mapping indentation");
    parent.child_indent = indent;
    const auto key = scalar(line, at, true);
    while (at < line.size() && line[at] == ' ')
      ++at;
    if (at == line.size() || line[at++] != ':' || (at < line.size() && line[at] != ' '))
      fail(E::unsupported_vfs, "VFS must use RPCS3's standard block-map format");
    while (at < line.size() && line[at] == ' ')
      ++at;
    YamlNode node;
    if (at < line.size() && line[at] != '#') {
      if (trim(line.substr(at)) == "{}")
        at = line.size();
      else {
        node.scalar = true;
        node.value = scalar(line, at, false);
        while (at < line.size() && line[at] == ' ')
          ++at;
        if (at < line.size() && line[at] != '#')
          fail(E::unsupported_vfs, "Unexpected VFS scalar trailing content");
      }
    }
    if (++entries > 256 || stack.size() > 3)
      fail(E::bounds_exceeded, "VFS mapping exceeds its entry or nesting bound");
    const auto [found, inserted] = parent.children.emplace(key, std::move(node));
    if (!inserted)
      fail(E::unsupported_vfs, "Duplicate VFS keys are unsupported");
    if (!found->second.scalar)
      stack.push_back({static_cast<int>(indent), &found->second});
  }
  return root;
}
std::string quote(std::string_view value) {
  std::string result = "'";
  for (char c : value) {
    result += c;
    if (c == '\'')
      result += c;
  }
  return result + "'";
}
std::string yaml_value(const YamlNode &node) {
  if (!node.scalar)
    fail(Rpcs3SessionError::unsupported_vfs, "VFS path fields must be scalar strings");
  return node.value;
}
std::string resolve_mount(std::string value, std::string_view fallback, std::string_view emu) {
  if (value.empty())
    value = fallback;
  if (value.empty())
    return {};
  for (std::size_t at = 0; (at = value.find("$(EmulatorDir)", at)) != std::string::npos; at += emu.size())
    value.replace(at, 14, emu);
  if (value.find("$(") != std::string::npos)
    fail(Rpcs3SessionError::unsupported_vfs,
         "Unknown VFS placeholders are unsupported; use absolute mount paths");
  value = slash(std::move(value));
  if (!text_path(value).is_absolute())
    fail(Rpcs3SessionError::unsupported_vfs,
         "Relative VFS mounts cannot be preserved; configure absolute paths in RPCS3");
  return trailing(std::move(value));
}
struct Vfs {
  std::string bytes;
  fs::path dev_hdd0;
};
Vfs preserve_vfs(const std::string &input, const fs::path &original) {
  using E = Rpcs3SessionError;
  const auto root = parse_vfs(input);
  const std::map<std::string, std::string> defaults{
      {"/dev_hdd0/", "$(EmulatorDir)dev_hdd0/"},     {"/dev_hdd1/", "$(EmulatorDir)dev_hdd1/"},
      {"/dev_flash/", "$(EmulatorDir)dev_flash/"},   {"/dev_flash2/", "$(EmulatorDir)dev_flash2/"},
      {"/dev_flash3/", "$(EmulatorDir)dev_flash3/"}, {"/dev_bdvd/", "$(EmulatorDir)dev_bdvd/"},
      {"/games/", "$(EmulatorDir)games/"},           {"/app_home/", ""}};
  for (const auto &[key, node] : root.children) {
    (void)node;
    if (key != "$(EmulatorDir)" && key != "/dev_usb***/" && !defaults.contains(key))
      fail(E::unsupported_vfs,
           "Unrecognized VFS fields cannot be safely preserved; use the supported RPCS3 VFS schema");
  }
  auto emu = trailing(slash(utf8(original)));
  if (const auto found = root.children.find("$(EmulatorDir)"); found != root.children.end()) {
    const auto value = yaml_value(found->second);
    if (!value.empty()) {
      emu = slash(value);
      if (!text_path(emu).is_absolute() || emu.back() != '/')
        fail(E::unsupported_vfs, "Custom EmulatorDir must be absolute and end with a directory separator");
      if (emu.find("$(") != std::string::npos)
        fail(E::unsupported_vfs, "Nested EmulatorDir placeholders are unsupported");
    }
  }
  Vfs result;
  result.bytes = quote("$(EmulatorDir)") + ": " + quote(emu) + "\n";
  for (const auto &[key, fallback] : defaults) {
    const auto found = root.children.find(key);
    const auto resolved = resolve_mount(
        found == root.children.end() ? std::string{} : yaml_value(found->second), fallback, emu);
    result.bytes += quote(key) + ": " + quote(resolved) + "\n";
    if (key == "/dev_hdd0/")
      result.dev_hdd0 = no_trailing(text_path(resolved));
  }
  std::map<std::string, std::map<std::string, std::string>> devices;
  const auto usb = root.children.find("/dev_usb***/");
  if (usb == root.children.end())
    devices["/dev_usb000"] = {{"Path", resolve_mount({}, "$(EmulatorDir)dev_usb000/", emu)},
                              {"Serial", ""},
                              {"VID", ""},
                              {"PID", ""}};
  else {
    if (usb->second.scalar)
      fail(E::unsupported_vfs, "VFS USB devices must use RPCS3's block-map schema");
    for (const auto &[key, device] : usb->second.children) {
      if (key.size() != 11 || !key.starts_with("/dev_usb00") || key.back() < '0' || key.back() > '7' ||
          device.scalar)
        fail(E::unsupported_vfs, "Unsupported VFS USB device identifier or schema");
      auto &values = devices[key];
      for (const auto *field : {"Path", "Serial", "VID", "PID"})
        values[field] = "";
      for (const auto &[field, node] : device.children) {
        if (!values.contains(field))
          fail(E::unsupported_vfs, "Unrecognized VFS USB metadata cannot be safely preserved");
        values[field] = yaml_value(node);
      }
      values["Path"] =
          resolve_mount(values["Path"], key == "/dev_usb000" ? "$(EmulatorDir)dev_usb000/" : "", emu);
    }
  }
  result.bytes += quote("/dev_usb***/") + ":" + (devices.empty() ? " {}\n" : "\n");
  for (const auto &[key, fields] : devices) {
    result.bytes += "  " + quote(key) + ":\n";
    for (const auto &[field, value] : fields)
      result.bytes += "    " + quote(field) + ": " + quote(value) + "\n";
  }
  return result;
}
std::string patch_confirmation(std::string_view input) {
  std::string output;
  const std::string newline = input.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
  bool target = false, section = false, key = false;
  const auto insert = [&] {
    if (!output.empty() && !output.ends_with('\n'))
      output += newline;
    output += "confirmationBoxExitGame=false" + newline;
    key = true;
  };
  if (input.starts_with("\xef\xbb\xbf")) {
    output = "\xef\xbb\xbf";
    input.remove_prefix(3);
  }
  while (!input.empty()) {
    const auto end = input.find('\n');
    const auto raw = input.substr(0, end == std::string_view::npos ? input.size() : end + 1);
    auto line = raw;
    if (line.ends_with('\n'))
      line.remove_suffix(1);
    const std::string ending = line.ends_with('\r') ? "\r\n" : raw.ends_with('\n') ? "\n" : "";
    if (line.ends_with('\r'))
      line.remove_suffix(1);
    input = end == std::string_view::npos ? std::string_view{} : input.substr(end + 1);
    const auto clean = trim(line);
    if (clean.starts_with('[')) {
      if (target && !key)
        insert();
      target = clean == "[main_window]";
      if (target) {
        if (section)
          fail(Rpcs3SessionError::invalid_input, "Duplicate main_window INI sections are unsupported");
        section = true;
      }
    }
    const auto equal = clean.find('=');
    if (target && equal != std::string_view::npos &&
        trim(clean.substr(0, equal)) == "confirmationBoxExitGame") {
      const auto original_equal = line.find('=');
      output += line.substr(0, original_equal + 1);
      output += "false" + ending;
      key = true;
    } else
      output += raw;
  }
  if (!section) {
    if (!output.empty() && !output.ends_with('\n'))
      output += newline;
    output += "[main_window]" + newline;
    insert();
  } else if (target && !key)
    insert();
  return output;
}
bool valid_user(std::string_view value) {
  return value.size() == 8 && value != "00000000" &&
         std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
}
std::string selected_user(const Rpcs3SessionRequest &request, std::string_view persistent) {
  if (request.user_id) {
    if (!valid_user(*request.user_id))
      fail(Rpcs3SessionError::invalid_input,
           "Selected RPCS3 user must be eight digits and cannot be 00000000");
    return *request.user_id;
  }
  std::string user;
  bool users = false, seen = false;
  if (persistent.starts_with("\xef\xbb\xbf"))
    persistent.remove_prefix(3);
  while (!persistent.empty()) {
    const auto end = persistent.find('\n');
    const auto line = trim(persistent.substr(0, end));
    persistent = end == std::string_view::npos ? std::string_view{} : persistent.substr(end + 1);
    if (line.starts_with('[')) {
      users = line == "[Users]";
      continue;
    }
    const auto equal = line.find('=');
    if (users && equal != std::string_view::npos && trim(line.substr(0, equal)) == "active_user") {
      if (seen)
        fail(Rpcs3SessionError::invalid_input,
             "Duplicate persistent RPCS3 active-user settings are unsupported");
      seen = true;
      auto value = trim(line.substr(equal + 1));
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
      }
      user = value;
    }
  }
  if (user.empty())
    user = "00000001";
  if (!valid_user(user))
    fail(Rpcs3SessionError::invalid_input,
         "Persistent RPCS3 active user is invalid; select an eight-digit user explicitly");
  return user;
}
void owner_directory(const fs::path &path) {
#ifdef _WIN32
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    fail(Rpcs3SessionError::io_error, "Cannot obtain the current user's security token");
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> buffer(size);
  const bool token_ok = size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
  CloseHandle(token);
  if (!token_ok)
    fail(Rpcs3SessionError::io_error, "Cannot obtain the current user's security identity");
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &sid))
    fail(Rpcs3SessionError::io_error, "Cannot encode the current user's security identity");
  const std::wstring descriptor = L"O:" + std::wstring(sid) + L"D:P(A;OICI;FA;;;" + sid + L")";
  LocalFree(sid);
  PSECURITY_DESCRIPTOR security = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &security,
                                                            nullptr))
    fail(Rpcs3SessionError::io_error, "Cannot construct owner-only configuration permissions");
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security, FALSE};
  const bool created = CreateDirectoryW(path.c_str(), &attributes);
  const auto error = GetLastError();
  LocalFree(security);
  if (!created)
    fail(error == ERROR_ALREADY_EXISTS ? Rpcs3SessionError::unsafe_path : Rpcs3SessionError::io_error,
         "Cannot create a new owner-only private configuration directory");
#else
  if (::mkdir(path.c_str(), 0700) != 0)
    fail(errno == EEXIST ? Rpcs3SessionError::unsafe_path : Rpcs3SessionError::io_error,
         "Cannot create a new owner-only private configuration directory");
#endif
}
void subdirectories(const fs::path &base, const fs::path &relative) {
  fs::path current = base;
  for (const auto &part : relative) {
    current /= part;
    if (!path_exists(current))
      owner_directory(current);
    else {
      no_link(current);
      if (!fs::is_directory(current))
        fail(Rpcs3SessionError::unsafe_path, "Private configuration path was replaced by a non-directory");
    }
  }
}
void materialize(const Snapshot &data, const fs::path &base) {
  for (const auto &relative : data.directories)
    subdirectories(base, relative);
  for (const auto &[relative, file] : data.files) {
    subdirectories(base, relative.parent_path());
    const auto target = base / relative;
    if (path_exists(target))
      fail(Rpcs3SessionError::unsafe_path, "Private preference destination already exists");
    std::ofstream output(target, std::ios::binary);
    if (!output)
      fail(Rpcs3SessionError::io_error, "Cannot create a private preference file");
    output.write(file.bytes.data(), static_cast<std::streamsize>(file.bytes.size()));
    output.close();
    if (!output)
      fail(Rpcs3SessionError::io_error, "Cannot finish writing private preferences");
#ifndef _WIN32
    fs::permissions(target, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
    if (file.retain_time)
      fs::last_write_time(target, file.time);
  }
}
// ---- Private storage ownership -------------------------------------------
// Owner marker written into every newly created private base. It binds the
// directory to this preparation so a replaced root that merely reuses a file id
// (common for freshly recreated Linux directories) is still refused.
constexpr const char *owner_marker = ".openxmb-session-owner";
struct Identity {
  std::uint64_t volume{}, high{}, low{};
  bool operator==(const Identity &) const = default;
};
// Identity of a directory; empty when it is missing, not a directory, or (for
// follow == false) a link/reparse point. Windows prefers the 128-bit file id and
// falls back to the legacy 64-bit index on volumes without FileIdInfo (exFAT);
// the owner marker backs up the weaker identity.
std::optional<Identity> directory_id(const fs::path &path, bool follow) {
#ifdef _WIN32
  static_assert(sizeof(FILE_ID_128) == 2 * sizeof(std::uint64_t));
  Handle handle(open_directory(path.native(), FILE_READ_ATTRIBUTES, follow));
  BY_HANDLE_FILE_INFORMATION info{};
  if (!handle || !GetFileInformationByHandle(handle.value, &info) ||
      !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    return {};
  Identity result;
  FILE_ID_INFO id{};
  if (GetFileInformationByHandleEx(handle.value, FileIdInfo, &id, sizeof(id))) {
    result.volume = id.VolumeSerialNumber;
    std::memcpy(&result.high, &id.FileId, sizeof(result.high));
    std::memcpy(&result.low, reinterpret_cast<const unsigned char *>(&id.FileId) + sizeof(result.high),
                sizeof(result.low));
  } else {
    result.volume = info.dwVolumeSerialNumber;
    result.high = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    result.low = 1; // legacy index marker
  }
  return result;
#else
  struct stat info{};
  if ((follow ? ::stat(path.c_str(), &info) : ::lstat(path.c_str(), &info)) != 0 || !S_ISDIR(info.st_mode))
    return {};
  Identity result;
  result.volume = static_cast<std::uint64_t>(info.st_dev);
  result.high = static_cast<std::uint64_t>(info.st_ino);
  return result;
#endif
}
bool same_path(const fs::path &left, const fs::path &right) { return contains(left, right) && contains(right, left); }
std::string new_token() {
  std::random_device device;
  std::string token;
  for (int word = 0; word < 4; ++word) {
    const auto value = static_cast<std::uint32_t>(device());
    for (int nibble = 0; nibble < 8; ++nibble)
      token += "0123456789abcdef"[(value >> (4 * nibble)) & 15];
  }
  return token;
}
// Everything recorded about the one directory this preparation created.
struct Owned {
  fs::path base, canonical_parent;
  Identity root_id, parent_id;
  std::string token;
};
// True when the owned base still exists and is provably the directory this
// preparation created; false when nothing owned remains; throws Failure when a
// replacement or escape is detected. allow_unmarked is only for resuming a
// deletion that already started (the marker may be gone by then).
bool verify_owned(const Owned &owned, bool allow_unmarked) {
  const auto parent = owned.base.parent_path();
  if (!path_exists(parent))
    return false;
  existing_chain(parent);
  std::error_code error;
  const auto canonical_parent = fs::canonical(parent, error);
  if (error)
    fail(Rpcs3SessionError::io_error, "Cannot resolve the private session parent directory");
  const auto parent_id = directory_id(canonical_parent, true);
  if (!same_path(canonical_parent, owned.canonical_parent) || !parent_id || *parent_id != owned.parent_id)
    fail(Rpcs3SessionError::unsafe_path,
         "The private session parent directory changed; its storage was not removed");
  if (!path_exists(owned.base))
    return false;
  no_link(owned.base);
  const auto root_id = directory_id(owned.base, false);
  if (!root_id || *root_id != owned.root_id)
    fail(Rpcs3SessionError::unsafe_path, "The private session directory was replaced; it was not removed");
  const auto canonical_base = fs::canonical(owned.base, error);
  if (error)
    fail(Rpcs3SessionError::io_error, "Cannot resolve the private session directory");
  if (!same_path(canonical_base, canonical_parent / owned.base.filename()))
    fail(Rpcs3SessionError::unsafe_path, "The private session directory moved; it was not removed");
  const auto marker = owned.base / owner_marker;
  if (!path_exists(marker)) {
    if (allow_unmarked)
      return true;
    fail(Rpcs3SessionError::unsafe_path, "The private session directory lost its owner marker; it was not removed");
  }
  no_link(marker);
  std::ifstream input(marker, std::ios::binary);
  std::string text(owned.token.size() + 1, '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<std::size_t>(input.gcount()));
  if (!fs::is_regular_file(marker, error) || error || text != owned.token)
    fail(Rpcs3SessionError::unsafe_path, "The private session directory owner marker differs; it was not removed");
  return true;
}
// remove_all removes directory links/junctions themselves (the Windows cache
// junction) and never walks their targets.
void remove_owned(const Owned &owned) {
  std::error_code error;
  fs::remove_all(owned.base, error);
  if (error || path_exists(owned.base))
    fail(Rpcs3SessionError::io_error, "The private session directory could not be removed completely");
}
void write_marker(const fs::path &marker, const std::string &token) {
  std::ofstream output(marker, std::ios::binary);
  if (!output)
    fail(Rpcs3SessionError::io_error, "Cannot create the private session owner marker");
  output.write(token.data(), static_cast<std::streamsize>(token.size()));
  output.close();
  if (!output)
    fail(Rpcs3SessionError::io_error, "Cannot finish the private session owner marker");
#ifndef _WIN32
  fs::permissions(marker, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
}
} // namespace
// Shared by every copy of a config. Armed from creation until explicit cleanup
// succeeds. While armed and not retained, the final owner's destruction rolls
// the base back; retained storage is only ever removed by explicit cleanup.
class Rpcs3SessionStorage {
public:
  explicit Rpcs3SessionStorage(Owned owned) : owned_(std::move(owned)) {}
  Rpcs3SessionStorage(const Rpcs3SessionStorage &) = delete;
  Rpcs3SessionStorage &operator=(const Rpcs3SessionStorage &) = delete;
  ~Rpcs3SessionStorage() {
    try {
      const std::lock_guard lock(mutex_);
      if (armed_ && !retained_ && verify_owned(owned_, partial_))
        remove_owned(owned_);
    } catch (...) {
      // Refusals and I/O failures leave residue for a later verified cleanup.
    }
  }
  bool retain() noexcept {
    try {
      const std::lock_guard lock(mutex_);
      if (!armed_)
        return false;
      retained_ = true;
      return true;
    } catch (...) {
      return false;
    }
  }
  Rpcs3SessionCleanupResult cleanup() {
    try {
      const std::lock_guard lock(mutex_);
      if (!armed_)
        return {};
      Rpcs3SessionCleanupResult result;
      if (verify_owned(owned_, partial_)) {
        partial_ = true;
        remove_owned(owned_);
        result.removed = true;
      }
      armed_ = false;
      return result;
    } catch (const Failure &failure) {
      return {failure.code, false, failure.message};
    } catch (...) {
      return {Rpcs3SessionError::io_error, false, "The private session directory could not be cleaned up"};
    }
  }

private:
  const Owned owned_;
  std::mutex mutex_;
  bool armed_{true}, retained_{false}, partial_{false};
};
namespace {
// Called right after this process created `base`. Records the exact identity and
// owner marker; on any failure removes only the still-empty directory it just
// created.
std::shared_ptr<Rpcs3SessionStorage> adopt_created(const fs::path &base, const fs::path &canonical_parent) {
  const auto discard = [&] {
    std::error_code error;
    fs::remove(base / owner_marker, error);
    fs::remove(base, error); // removes an empty directory only
  };
  try {
    Owned owned{base, canonical_parent, {}, {}, new_token()};
    const auto root = directory_id(base, false), parent = directory_id(canonical_parent, true);
    if (!root || !parent)
      fail(Rpcs3SessionError::io_error, "Cannot record the identity of the new private session directory");
    owned.root_id = *root;
    owned.parent_id = *parent;
    write_marker(base / owner_marker, owned.token);
    return std::make_shared<Rpcs3SessionStorage>(std::move(owned));
  } catch (...) {
    discard();
    throw;
  }
}
// Rolls a failed preparation back and describes any residue.
std::string rollback_note(const std::shared_ptr<Rpcs3SessionStorage> &storage) {
  if (!storage || storage->cleanup())
    return {};
  return " Its partial private destination could not be removed and was left in place.";
}
} // namespace
bool retain_rpcs3_session(const Rpcs3SessionConfig &config) noexcept {
  return config.storage && config.storage->retain();
}
Rpcs3SessionCleanupResult cleanup_rpcs3_session(const Rpcs3SessionConfig &config) {
  if (!config.storage)
    return {Rpcs3SessionError::invalid_input, false, "The RPCS3 session owns no private storage"};
  return config.storage->cleanup();
}
bool rpcs3_session_native_fallback(Rpcs3SessionError error) noexcept {
  switch (error) {
  case Rpcs3SessionError::portable_config:
  case Rpcs3SessionError::instance_lock_unavailable:
  case Rpcs3SessionError::unsupported_platform:
  case Rpcs3SessionError::unsupported_vfs:
  case Rpcs3SessionError::bounds_exceeded:
  case Rpcs3SessionError::io_error:
    return true;
  case Rpcs3SessionError::none:
  case Rpcs3SessionError::invalid_input:
  case Rpcs3SessionError::unsafe_path:
  case Rpcs3SessionError::config_in_use:
    return false;
  }
  return false;
}
Rpcs3SessionResult prepare_rpcs3_session(const Rpcs3SessionRequest &request) {
  std::shared_ptr<Rpcs3SessionStorage> storage;
  try {
    absolute_input(request.executable);
    existing_chain(request.executable);
    if (!fs::is_regular_file(request.executable))
      fail(Rpcs3SessionError::invalid_input, "Selected RPCS3 executable must be a regular file");
    const auto portable = request.executable.parent_path() / "portable";
    if (path_exists(portable) && (fs::is_directory(portable) || fs::is_symlink(fs::symlink_status(portable))))
      fail(Rpcs3SessionError::portable_config, "RPCS3 portable/ takes precedence over environment overrides; "
                                               "select a non-portable executable for managed sessions");
    auto original = request.effective_config_root.empty() ? native_root(request.executable)
                                                          : request.effective_config_root;
    original = no_trailing(original);
    absolute_input(original);
    existing_chain(original);
    if (!fs::is_directory(original))
      fail(Rpcs3SessionError::invalid_input,
           "Effective RPCS3 configuration root must be an existing directory");
    const auto base = no_trailing(request.private_directory);
    absolute_input(base);
    if (base == base.root_path() || path_exists(base))
      fail(Rpcs3SessionError::unsafe_path,
           "Private configuration destination must be a new non-root directory");
    existing_chain(base.parent_path());
    if (!fs::is_directory(base.parent_path()))
      fail(Rpcs3SessionError::unsafe_path, "Private configuration destination parent must already exist");
    const auto physical_original = fs::canonical(original),
               physical_base = fs::canonical(base.parent_path()) / base.filename(),
               physical_exe = fs::canonical(request.executable);
    if (contains(physical_original, physical_base) || contains(physical_base, physical_original) ||
        contains(physical_base, physical_exe) || contains(physical_exe.parent_path(), physical_base))
      fail(Rpcs3SessionError::unsafe_path,
           "Private configuration destination must not overlap the source installation or its configuration");
#ifdef __APPLE__
    fail(Rpcs3SessionError::unsupported_platform,
         "Managed RPCS3 sessions on macOS require broader HOME isolation; use a normal direct launch");
#endif
    // Keep the original native lock protected before reading any settings.
#ifdef _WIN32
    auto instance_lease = acquire_instance_lease(original);
#else
    const auto cache_parent = original_cache_parent();
#endif
    auto data = snapshot(original);
    const auto vfs_relative = config_relative() / "vfs.yml";
    const auto vfs = preserve_vfs(data.get(vfs_relative), original);
    data.replace(vfs_relative, vfs.bytes);
    data.replace("GuiConfigs/CurrentSettings.ini",
                 patch_confirmation(data.get("GuiConfigs/CurrentSettings.ini")));
    const auto user = selected_user(request, data.get("GuiConfigs/persistent_settings.dat"));
    Rpcs3SessionConfig config;
    config.user_id = user;
    config.dev_hdd0 = vfs.dev_hdd0;
    config.arguments = {"--user-id", user};
#ifdef _WIN32
    config.lease = std::move(instance_lease);
    config.inherited_handles.push_back(reinterpret_cast<std::uintptr_t>(config.lease.get()));
#endif
    owner_directory(base);
    storage = adopt_created(base, physical_base.parent_path());
#ifdef _WIN32
    config.private_directory = base;
    config.config_dir_environment = trailing(utf8(base));
    config.environment_overrides["RPCS3_CONFIG_DIR"] = config.config_dir_environment;
#else
    config.private_directory = base / "rpcs3";
    owner_directory(config.private_directory);
    config.config_dir_environment = trailing(utf8(config.private_directory));
    config.environment_overrides["XDG_CONFIG_HOME"] = utf8(base);
    config.environment_overrides["XDG_CACHE_HOME"] = cache_parent;
#endif
    materialize(data, config.private_directory);
#ifdef _WIN32
    auto cache = link_original_cache(physical_original, base);
    config.warm_cache_directory = std::move(cache.directory);
    config.cache_fallback_reason = std::move(cache.reason);
#endif
    config.storage = std::move(storage);
    return {std::move(config), Rpcs3SessionError::none, {}};
  } catch (const Failure &failure) {
    return {{}, failure.code, failure.message + rollback_note(storage)};
  } catch (const std::exception &) {
    return {{},
            Rpcs3SessionError::io_error,
            storage ? "Private RPCS3 snapshot could not be completed." + rollback_note(storage)
                    : "RPCS3 preferences could not be inspected or copied safely"};
  }
}
} // namespace openxmb::desktop
