#include "openxmb/desktop/session.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace openxmb::desktop {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
std::atomic<std::uint64_t> serial{};
std::uint64_t wall_time() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}
std::string path_text(const fs::path& path) { const auto value = path.u8string(); return {value.begin(), value.end()}; }
const std::string& owner_token() {
  static const auto token = std::to_string(wall_time()) + "-" + std::to_string(Clock::now().time_since_epoch().count()) + "-" +
#ifdef _WIN32
    std::to_string(GetCurrentProcessId());
#else
    std::to_string(getpid());
#endif
  return token;
}
void bounded_string(const std::string& value, std::size_t maximum, bool required = false) {
  if ((required && value.empty()) || value.size() > maximum || value.find('\0') != std::string::npos) throw std::runtime_error("Invalid or oversized session text");
  (void)json(value).dump(); // Reject invalid UTF-8 before native launch.
}
void validate(const SessionDescriptor& value) {
  bounded_string(value.identity, 512, true); bounded_string(value.title, 512, true);
  bounded_string(value.provider, 128, true); bounded_string(value.runner, 128, true); bounded_string(path_text(value.profile_directory), 32768, true);
  if (value.kind != SessionKind::game && value.kind != SessionKind::application) throw std::runtime_error("Invalid session kind");
  if (value.tracking != TrackingQuality::direct_child && value.tracking != TrackingQuality::launcher_only && value.tracking != TrackingQuality::unknown) throw std::runtime_error("Invalid tracking quality");
}
std::string tracking_message(TrackingQuality quality) {
  if (quality == TrackingQuality::direct_child) return "Observed direct child lifetime; detached descendants are not tracked";
  if (quality == TrackingQuality::launcher_only) return "Only the forwarding launcher is observed; application lifetime and playtime are unknown";
  return "Application lifetime is unknown; no playtime is credited";
}
std::uint64_t unsigned_value(const json& value) {
  if (!value.is_number_integer() || value < 0) throw std::runtime_error("Session counters must be unsigned integers");
  return value.get<std::uint64_t>();
}
int enum_value(const json& value, int maximum) {
  if (!value.is_number_integer() || value < 0 || value > maximum) throw std::runtime_error("Invalid session enumeration");
  return value.get<int>();
}
json record_json(const SessionSnapshot& value) {
  const auto& d = value.descriptor;
  json result{{"id", value.id}, {"identity", d.identity}, {"title", d.title}, {"provider", d.provider}, {"runner", d.runner}, {"kind", static_cast<int>(d.kind)},
    {"tracking", static_cast<int>(d.tracking)}, {"state", static_cast<int>(value.state)}, {"owner", owner_token()}, {"process_id", value.process_id},
    {"started_at", value.started_at}, {"ended_at", value.ended_at}, {"duration_ms", value.duration_ms}};
  result["exit_code"] = value.exit_code ? json(*value.exit_code) : json(nullptr); return result;
}
SessionSnapshot read_record(const json& record, const fs::path& profile) {
  SessionSnapshot value;
  value.id = record.at("id").get<std::string>(); bounded_string(value.id, 256, true);
  auto& d = value.descriptor;
  d.identity = record.at("identity"); d.title = record.at("title"); d.provider = record.at("provider"); d.runner = record.at("runner"); d.profile_directory = profile;
  d.kind = static_cast<SessionKind>(enum_value(record.at("kind"), 1)); d.tracking = static_cast<TrackingQuality>(enum_value(record.at("tracking"), 2)); validate(d);
  const auto state = enum_value(record.at("state"), 3);
  value.state = static_cast<SessionState>(state); value.process_id = record.at("process_id").get<std::int64_t>();
  value.started_at = unsigned_value(record.at("started_at")); value.ended_at = unsigned_value(record.at("ended_at")); value.duration_ms = unsigned_value(record.at("duration_ms"));
  if (!record.at("exit_code").is_null()) value.exit_code = record.at("exit_code").get<int>();
  const auto owner = record.at("owner").get<std::string>(); bounded_string(owner, 256, true);
  if (value.state == SessionState::running && owner != owner_token()) { value.state = SessionState::interrupted; value.ended_at = 0; value.exit_code.reset(); }
  value.tracking_message = tracking_message(d.tracking);
  if (value.state == SessionState::interrupted) value.tracking_message += "; observation was interrupted and actual exit is unknown";
  if (value.state == SessionState::detached) value.tracking_message += "; observation stopped while the process was running";
  return value;
}
fs::path journal_path(const fs::path& profile) { return profile / "sessions/history.json"; }
struct JournalLock {
#ifdef _WIN32
  HANDLE handle{INVALID_HANDLE_VALUE}; OVERLAPPED overlap{};
  explicit JournalLock(const fs::path& directory) {
    handle = CreateFileW((directory / "history.lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open session journal lock");
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap)) { CloseHandle(handle); handle = INVALID_HANDLE_VALUE; throw std::runtime_error("Session journal is busy"); }
  }
  ~JournalLock() { if (handle != INVALID_HANDLE_VALUE) { UnlockFileEx(handle, 0, 1, 0, &overlap); CloseHandle(handle); } }
#else
  int handle{-1};
  explicit JournalLock(const fs::path& directory) {
    handle = open((directory / "history.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (handle < 0) throw std::runtime_error("Cannot open session journal lock");
    if (flock(handle, LOCK_EX | LOCK_NB) != 0) { close(handle); handle = -1; throw std::runtime_error("Session journal is busy"); }
  }
  ~JournalLock() { if (handle >= 0) { flock(handle, LOCK_UN); close(handle); } }
#endif
  JournalLock(const JournalLock&) = delete;
  JournalLock& operator=(const JournalLock&) = delete;
};
json load_journal(const fs::path& profile) {
  const auto path = journal_path(profile);
  if (!fs::exists(path)) return {{"version", 1}, {"recent", json::array()}, {"totals", json::array()}};
  if (!fs::is_regular_file(path) || fs::file_size(path) > 4 * 1024 * 1024) throw std::runtime_error("Session journal exceeds 4 MiB or is not a regular file");
  std::ifstream input(path, std::ios::binary);
  std::string bytes(4 * 1024 * 1024 + 1, '\0'); input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (input.bad() || static_cast<std::size_t>(input.gcount()) > 4 * 1024 * 1024) throw std::runtime_error("Cannot read bounded session journal");
  bytes.resize(static_cast<std::size_t>(input.gcount())); auto journal = json::parse(bytes);
  if (!journal.is_object() || journal.value("version", 0) != 1 || !journal.at("recent").is_array() || journal.at("recent").size() > 512 ||
      !journal.at("totals").is_array() || journal.at("totals").size() > 4096) throw std::runtime_error("Invalid bounded session journal");
  std::set<std::string> ids, identities;
  for (auto& record : journal.at("recent")) {
    const auto snapshot = read_record(record, profile);
    if (!ids.insert(snapshot.id).second) throw std::runtime_error("Duplicate session journal identity");
    if (snapshot.state == SessionState::interrupted && record.at("state") == static_cast<int>(SessionState::running)) {
      record["state"] = static_cast<int>(SessionState::interrupted); record["ended_at"] = 0; record["exit_code"] = nullptr;
    }
  }
  for (const auto& total : journal.at("totals")) {
    bounded_string(total.at("identity").get<std::string>(), 512, true); bounded_string(total.at("provider").get<std::string>(), 128, true); bounded_string(total.at("runner").get<std::string>(), 128, true);
    (void)enum_value(total.at("kind"), 1);
    const auto identity = json::array({total.at("identity"), total.at("provider"), total.at("runner"), total.at("kind")}).dump();
    if (!identities.insert(identity).second) throw std::runtime_error("Duplicate session total identity");
    (void)unsigned_value(total.at("launch_count")); (void)unsigned_value(total.at("total_playtime_ms"));
  }
  return journal;
}
void write_journal(const fs::path& profile, const json& journal) {
  const auto path = journal_path(profile); const auto bytes = journal.dump();
  if (bytes.size() > 4 * 1024 * 1024) throw std::runtime_error("Session journal exceeds 4 MiB");
  auto temporary = path; temporary += ".tmp-" + owner_token() + "-" + std::to_string(++serial);
  try {
    { std::ofstream output(temporary, std::ios::binary | std::ios::trunc); output.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); output.flush(); if (!output) throw std::runtime_error("Cannot write session journal"); }
#ifdef _WIN32
    const auto handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot flush session journal");
    const bool flushed = FlushFileBuffers(handle) != FALSE; CloseHandle(handle);
    if (!flushed || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace session journal");
#else
    const int handle = open(temporary.c_str(), O_RDONLY | O_CLOEXEC);
    if (handle < 0) throw std::runtime_error("Cannot flush session journal");
    const auto flushed = fsync(handle); close(handle);
    if (flushed != 0) throw std::runtime_error("Cannot flush session journal");
    fs::rename(temporary, path);
    const int directory = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) { const auto status = fsync(directory); close(directory); if (status != 0) throw std::runtime_error("Cannot flush session journal directory"); }
#endif
  } catch (...) { std::error_code ignored; fs::remove(temporary, ignored); throw; }
}
bool matches(const json& total, const SessionDescriptor& value) {
  return total.at("identity") == value.identity && total.at("provider") == value.provider && total.at("runner") == value.runner && total.at("kind") == static_cast<int>(value.kind);
}
void preflight(const SessionDescriptor& value) {
  fs::create_directories(journal_path(value.profile_directory).parent_path());
  JournalLock lock(journal_path(value.profile_directory).parent_path()); auto journal = load_journal(value.profile_directory);
  if (journal.at("totals").size() >= 4096 && std::ranges::none_of(journal.at("totals"), [&](const json& total) { return matches(total, value); })) throw std::runtime_error("Session history supports at most 4096 identities per profile");
  write_journal(value.profile_directory, journal);
}
void add(std::uint64_t& destination, std::uint64_t value) {
  if (value > std::numeric_limits<std::uint64_t>::max() - destination) throw std::runtime_error("Session history counter overflow");
  destination += value;
}
void persist(const SessionSnapshot& value) {
  const auto& profile = value.descriptor.profile_directory;
  JournalLock lock(journal_path(profile).parent_path()); auto journal = load_journal(profile);
  auto& recent = journal.at("recent"); auto& totals = journal.at("totals");
  auto found = std::ranges::find_if(recent, [&](const json& record) { return record.at("id") == value.id; });
  const bool inserted = found == recent.end();
  const bool first_terminal = value.state != SessionState::running && (inserted || found->at("state") == static_cast<int>(SessionState::running));
  auto total = std::ranges::find_if(totals, [&](const json& row) { return matches(row, value.descriptor); });
  if (total == totals.end()) {
    if (totals.size() >= 4096) throw std::runtime_error("Session identity history limit reached");
    totals.push_back({{"identity", value.descriptor.identity}, {"provider", value.descriptor.provider}, {"runner", value.descriptor.runner}, {"kind", static_cast<int>(value.descriptor.kind)}, {"launch_count", 0}, {"total_playtime_ms", 0}});
    total = std::prev(totals.end());
  }
  if (inserted) { auto count = total->at("launch_count").get<std::uint64_t>(); add(count, 1); (*total)["launch_count"] = count; }
  if (first_terminal && value.state == SessionState::exited && value.descriptor.tracking == TrackingQuality::direct_child) {
    auto duration = total->at("total_playtime_ms").get<std::uint64_t>(); add(duration, value.duration_ms); (*total)["total_playtime_ms"] = duration;
  }
  if (inserted) recent.push_back(record_json(value)); else *found = record_json(value);
  while (recent.size() > 512) {
    const auto removable = std::ranges::find_if(recent, [](const json& record) { return record.at("state") != static_cast<int>(SessionState::running); });
    if (removable == recent.end()) throw std::runtime_error("Too many active session records");
    recent.erase(removable);
  }
  write_journal(profile, journal);
}
}

struct SessionManager::Impl {
  struct Entry { SessionSnapshot snapshot; std::shared_ptr<integrations::OwnedProcess> process; Clock::time_point started; bool dirty{true}; };
  mutable std::mutex mutex;
  std::mutex shutdown_mutex;
  std::condition_variable condition;
  std::vector<Entry> entries;
  bool stopping{};
  std::thread observer;
  Impl() : observer([this] { run(); }) {}
  static void update(Entry& entry) {
    if (entry.snapshot.state != SessionState::running) return;
    entry.snapshot.duration_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - entry.started).count());
    const auto observation = entry.process->observe();
    if (observation.state == integrations::ProcessState::running) return;
    entry.snapshot.state = observation.state == integrations::ProcessState::exited ? SessionState::exited : SessionState::interrupted;
    entry.snapshot.ended_at = entry.snapshot.state == SessionState::exited ? wall_time() : 0; entry.snapshot.exit_code = observation.exit_code;
    if (!observation.message.empty()) entry.snapshot.tracking_message += "; " + observation.message;
    entry.dirty = true;
  }
  static void save(Entry& entry) {
    if (!entry.dirty) return;
    try { persist(entry.snapshot); entry.snapshot.persistence_error.clear(); entry.dirty = false; }
    catch (const std::exception& error) { entry.snapshot.persistence_error = error.what(); }
  }
  void run() {
    std::unique_lock lock(mutex);
    while (!stopping) {
      condition.wait_for(lock, std::chrono::milliseconds(25), [&] { return stopping; });
      if (stopping) break;
      for (auto& entry : entries) { update(entry); save(entry); }
    }
  }
};
SessionManager::SessionManager() : impl_(std::make_unique<Impl>()) {}
SessionManager::~SessionManager() { (void)shutdown(); }
SessionLaunchResult SessionManager::launch(SessionDescriptor descriptor, const std::vector<std::string>& arguments, const fs::path& working_directory) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->stopping) return {false, {}, "Session observation has shut down"};
  if (impl_->entries.size() >= 512) {
    const auto removable = std::ranges::find_if(impl_->entries, [](const auto& value) { return value.snapshot.state != SessionState::running && !value.dirty; });
    if (removable == impl_->entries.end()) return {false, {}, "Session observation backlog is full; resolve history save failures before launching more"};
    impl_->entries.erase(removable);
  }
  if (std::ranges::count_if(impl_->entries, [](const auto& entry) { return entry.snapshot.state == SessionState::running; }) >= 256) return {false, {}, "Too many active sessions"};
  try { validate(descriptor); descriptor.profile_directory = fs::weakly_canonical(fs::absolute(descriptor.profile_directory)); preflight(descriptor); }
  catch (const std::exception& error) { return {false, {}, "Cannot prepare session history: " + std::string(error.what())}; }
  const auto started = Clock::now(); auto launched = integrations::launch_owned_process(arguments, working_directory);
  if (!launched) return {false, {}, launched.message};
  SessionSnapshot snapshot;
  snapshot.id = "session-" + owner_token() + "-" + std::to_string(++serial); snapshot.descriptor = std::move(descriptor);
  snapshot.process_id = launched.process->id(); snapshot.started_at = wall_time(); snapshot.tracking_message = tracking_message(snapshot.descriptor.tracking);
  impl_->entries.push_back({std::move(snapshot), std::move(launched.process), started, true});
  auto& entry = impl_->entries.back(); Impl::save(entry);
  const auto result = SessionLaunchResult{true, entry.snapshot.id, entry.snapshot.persistence_error.empty() ? "Session started" : "Session started; history save will retry: " + entry.snapshot.persistence_error};
  if (impl_->entries.size() > 512) {
    const auto removable = std::ranges::find_if(impl_->entries, [](const auto& value) { return value.snapshot.state != SessionState::running && !value.dirty; });
    if (removable != impl_->entries.end()) impl_->entries.erase(removable);
  }
  impl_->condition.notify_one(); return result;
}
std::vector<SessionSnapshot> SessionManager::snapshot() const {
  std::lock_guard lock(impl_->mutex); std::vector<SessionSnapshot> result; result.reserve(impl_->entries.size());
  for (const auto& entry : impl_->entries) {
    auto snapshot = entry.snapshot;
    if (snapshot.state == SessionState::running) snapshot.duration_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - entry.started).count());
    result.push_back(std::move(snapshot));
  }
  return result;
}
std::optional<SessionSnapshot> SessionManager::active_session() const {
  std::lock_guard lock(impl_->mutex);
  for (auto it = impl_->entries.rbegin(); it != impl_->entries.rend(); ++it) if (it->snapshot.state == SessionState::running) {
    auto value=it->snapshot;
    value.duration_ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-it->started).count());
    return value;
  }
  return {};
}
integrations::Result SessionManager::quit(const std::string& session_id, integrations::ProcessStopMode mode) {
  std::lock_guard lock(impl_->mutex);
  for (auto& entry : impl_->entries) if (entry.snapshot.id == session_id) {
    Impl::update(entry); Impl::save(entry);
    if (entry.snapshot.state != SessionState::running || !entry.process) return {false, "This session is no longer observed as running"};
    return entry.process->stop(mode);
  }
  return {false, "This session is not owned by this manager"};
}
SessionHistory SessionManager::history(const fs::path& profile_directory, const std::string& identity) const {
  SessionHistory result;
  try {
    const auto profile = fs::weakly_canonical(fs::absolute(profile_directory));
    if (!fs::exists(journal_path(profile))) return result;
    JournalLock lock(journal_path(profile).parent_path()); const auto journal = load_journal(profile);
    for (auto it = journal.at("recent").rbegin(); it != journal.at("recent").rend(); ++it) {
      auto record = read_record(*it, profile); if (identity.empty() || record.descriptor.identity == identity) result.recent.push_back(std::move(record));
    }
    for (const auto& total : journal.at("totals")) if (identity.empty() || total.at("identity") == identity) {
      add(result.launch_count, total.at("launch_count").get<std::uint64_t>()); add(result.total_playtime_ms, total.at("total_playtime_ms").get<std::uint64_t>());
    }
  } catch (const std::exception& error) { result = {}; result.error = error.what(); }
  return result;
}
integrations::Result SessionManager::shutdown() {
  std::lock_guard shutdown_lock(impl_->shutdown_mutex);
  { std::lock_guard lock(impl_->mutex); impl_->stopping = true; impl_->condition.notify_all(); }
  if (impl_->observer.joinable()) impl_->observer.join();
  std::lock_guard lock(impl_->mutex); std::string error;
  for (auto& entry : impl_->entries) {
    Impl::update(entry);
    if (entry.snapshot.state == SessionState::running) {
      entry.snapshot.state = SessionState::detached; entry.snapshot.ended_at = 0; entry.dirty = true;
      entry.snapshot.tracking_message += "; observation shut down while the process was running";
    }
    Impl::save(entry); entry.process.reset(); if (!entry.snapshot.persistence_error.empty()) error = entry.snapshot.persistence_error;
  }
  return {error.empty(), error.empty() ? "Session observation stopped; running applications were left open" : "Session observation stopped; history save failed: " + error};
}
} // namespace openxmb::desktop
