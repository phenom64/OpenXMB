#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace openxmb::desktop {
// Opaque owner of the private storage created by one successful or partially
// failed preparation. Defined in rpcs3_session.cpp.
class Rpcs3SessionStorage;
enum class Rpcs3SessionError {
  none,
  invalid_input,
  unsafe_path,
  portable_config,
  unsupported_vfs,
  bounds_exceeded,
  io_error,
  config_in_use,
  instance_lock_unavailable,
  unsupported_platform
};
struct Rpcs3SessionRequest {
  // Empty resolves RPCS3's native effective root without changing the host
  // environment. Explicit roots must already be absolute directories.
  std::filesystem::path effective_config_root, private_directory, executable;
  std::optional<std::string> user_id;
};
struct Rpcs3SessionConfig {
  std::filesystem::path private_directory, dev_hdd0;
  std::string user_id, config_dir_environment;
  std::vector<std::string> arguments;
  std::map<std::string, std::string> environment_overrides;
  // Windows: read-only exclusive original RPCS3.buf lease. The raw HANDLE is
  // noninheritable; the launcher must duplicate it into a restricted child
  // HANDLE_LIST so protection survives parent termination. Keep the shared
  // owner alive through child creation. Linux uses RPCS3's original cache lock.
  std::shared_ptr<void> lease;
  std::vector<std::uintptr_t> inherited_handles;
  // Windows only. RPCS3's cache lives under its config root and has no
  // independent override, so the private <root>/cache is a verified NTFS
  // directory junction to the existing canonical original cache; this is the
  // exact original cache directory the session reuses. Empty means the
  // session safely fell back to a cold private cache and the category text
  // explains why. Linux reuses its native cache through XDG_CACHE_HOME and
  // leaves both fields empty.
  std::filesystem::path warm_cache_directory;
  std::string cache_fallback_reason;
  // Shared by every copy of this config. Final destruction while the storage is
  // still unlaunched (never passed to retain_rpcs3_session) removes exactly the
  // newly created private base after verifying its identity. Null only for a
  // default-constructed config.
  std::shared_ptr<Rpcs3SessionStorage> storage;
};
struct Rpcs3SessionResult {
  std::optional<Rpcs3SessionConfig> config;
  Rpcs3SessionError error{Rpcs3SessionError::none};
  // Actionable category text only: configuration values and secrets are never
  // included. A failed preparation must never be used to launch RPCS3.
  std::string message;
  explicit operator bool() const noexcept { return config.has_value(); }
};
// Creates a new owner-only RPCS3 configuration snapshot. Never launches
// RPCS3, modifies source bytes/modification times, or mutates the host environment.
// Rejects portable/ precedence, links/reparse points, overlapping destinations,
// unrecognized VFS schemas and relative VFS mounts. Bounds: 4096 entries, depth
// 12, 8 MiB/file and 64 MiB total across config, GuiConfigs, patches, data and Icons.
// Windows requires an existing regular RPCS3.buf and never creates or writes it.
// The returned UTF-8 RPCS3_CONFIG_DIR value always has a trailing slash.
// Linux uses private_directory/rpcs3 and an XDG_CONFIG_HOME override, retaining
// its original absolute native cache through XDG_CACHE_HOME. Relative cache
// fallback without an absolute HOME/XDG basis fails explicitly.
// Windows then links the new private cache directory to the existing original
// cache with a junction after validating that the whole source chain is local,
// regular and non-reparse and that the link resolves to the same directory
// identity. The original cache is never created, enumerated, copied or
// modified; any doubt keeps the cold private cache. Deleting the private
// directory with std::filesystem::remove_all removes only the junction.
// macOS is explicitly unsupported; redirecting HOME requires a broader contract.
Rpcs3SessionResult prepare_rpcs3_session(const Rpcs3SessionRequest &request);
// Private-storage lifetime. prepare_rpcs3_session creates one brand-new private
// base (Windows: private_directory; Linux: its parent holding rpcs3/) and
// records its exact identity. Every failure after that creation, and the final
// destruction of an unlaunched config, roll back only that base. A pre-existing
// destination is never touched. The original configuration, its cache and
// every other directory are never inspected for deletion.
//
// Call after the managed child process was created successfully. From then on
// destructors (including shutdown while the game keeps running) preserve the
// storage. Returns false when the config owns no active storage (default
// config or already cleaned up).
bool retain_rpcs3_session(const Rpcs3SessionConfig &config) noexcept;
struct Rpcs3SessionCleanupResult {
  Rpcs3SessionError error{Rpcs3SessionError::none};
  // True only when this call deleted the private base; false for a repeated
  // cleanup, or when the base had already vanished.
  bool removed{false};
  std::string message;
  explicit operator bool() const noexcept { return error == Rpcs3SessionError::none; }
};
// Retires the private base after the caller confirmed the managed RPCS3 process
// has exited. Never call it at shutdown, for detached sessions or merely on a
// profile change. The base is deleted only if its canonical parent, root
// identity (no link/reparse point, same file id), canonical location and
// private owner marker all still match what preparation created; otherwise
// nothing is deleted and unsafe_path is returned. A windows cache junction is
// removed without walking its target. io_error (for example a still-open
// file) leaves the storage armed so cleanup can be retried. Repeated calls and
// calls through other copies succeed with removed == false. A config that
// never owned storage reports invalid_input.
// Limit: a same-user process that swaps the verified root or parent between
// verification and deletion is not detected; every earlier swap is refused.
Rpcs3SessionCleanupResult cleanup_rpcs3_session(const Rpcs3SessionConfig &config);
// Whether a failed preparation may fall back to a native (unmanaged) launch.
// True: portable_config, instance_lock_unavailable, unsupported_platform,
// unsupported_vfs, bounds_exceeded and io_error (the private snapshot
// optimization could not be prepared and its partial storage was rolled back).
// False: none, unsafe_path, invalid_input and config_in_use stay fail-closed.
bool rpcs3_session_native_fallback(Rpcs3SessionError error) noexcept;
} // namespace openxmb::desktop
