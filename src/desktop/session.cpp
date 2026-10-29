#include "openxmb/desktop/session.hpp"
#include "openxmb/integrations/session_window.hpp"
#include "openxmb/integrations/observed_process.hpp"
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
  if (value.tracking != TrackingQuality::direct_child && value.tracking != TrackingQuality::launcher_only && value.tracking != TrackingQuality::unknown && value.tracking != TrackingQuality::provider_observed) throw std::runtime_error("Invalid tracking quality");
  if(value.observation) {
    const auto& spec=*value.observation;
    bounded_string(path_text(spec.executable),32768,spec.installation_directory.empty());
    bounded_string(path_text(spec.installation_directory),32768,spec.executable.empty());
    if(value.runner!="steam"||value.kind!=SessionKind::game||value.tracking!=TrackingQuality::launcher_only)
      throw std::runtime_error("Process observation requires a launcher-only Steam game session");
    if(spec.executable.empty()&&!spec.steam_app_id)throw std::runtime_error("Automatic Steam observation requires its installed app ID");
    if(spec.timeout_ms<250||spec.timeout_ms>300000)throw std::runtime_error("Steam game observation timeout must be between 250 ms and five minutes");
  }
}
std::string tracking_message(TrackingQuality quality) {
  if (quality == TrackingQuality::direct_child) return "Observed direct child lifetime; detached descendants are not tracked";
  if (quality == TrackingQuality::launcher_only) return "Only the forwarding launcher is observed; application lifetime and playtime are unknown";
  if (quality == TrackingQuality::provider_observed) return "Observed matching Steam game process; the process is not owned by OpenXMB";
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
    {"started_at", value.started_at}, {"ended_at", value.ended_at}, {"duration_ms", value.duration_ms}, {"awaiting_observation",value.awaiting_observation}};
  result["exit_code"] = value.exit_code ? json(*value.exit_code) : json(nullptr); return result;
}
SessionSnapshot read_record(const json& record, const fs::path& profile) {
  SessionSnapshot value;
  value.id = record.at("id").get<std::string>(); bounded_string(value.id, 256, true);
  auto& d = value.descriptor;
  d.identity = record.at("identity"); d.title = record.at("title"); d.provider = record.at("provider"); d.runner = record.at("runner"); d.profile_directory = profile;
  d.kind = static_cast<SessionKind>(enum_value(record.at("kind"), 1)); d.tracking = static_cast<TrackingQuality>(enum_value(record.at("tracking"), 3)); validate(d);
  const auto state = enum_value(record.at("state"), 3);
  value.state = static_cast<SessionState>(state); value.process_id = record.at("process_id").get<std::int64_t>();
  value.started_at = unsigned_value(record.at("started_at")); value.ended_at = unsigned_value(record.at("ended_at")); value.duration_ms = unsigned_value(record.at("duration_ms"));
  value.awaiting_observation=record.value("awaiting_observation",false);
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
    if(total.contains("provider_observed_playtime_ms"))(void)unsigned_value(total.at("provider_observed_playtime_ms"));
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
  if(first_terminal&&value.state==SessionState::exited&&value.descriptor.tracking==TrackingQuality::provider_observed) {
    auto duration=total->value("provider_observed_playtime_ms",std::uint64_t{});add(duration,value.duration_ms);(*total)["provider_observed_playtime_ms"]=duration;
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
  struct Entry {
    SessionSnapshot snapshot;std::shared_ptr<integrations::OwnedProcess> process;
    std::shared_ptr<integrations::ExecutableObservation> watch;
    std::shared_ptr<integrations::ObservedProcess> observed;
    Clock::time_point started,deadline,next_poll{};
    bool dirty{true};std::uint64_t revision{};std::mutex save_mutex;
  };
  mutable std::mutex mutex;
  mutable std::mutex journal_mutex;
  std::mutex shutdown_mutex;
  std::condition_variable condition;
  std::vector<std::shared_ptr<Entry>> entries;
  bool stopping{};
  std::stop_source stop;
  std::thread observer;
  Impl() : observer([this] { run(); }) {}
  static void changed(Entry& entry){entry.dirty=true;++entry.revision;}
  void update(const std::shared_ptr<Entry>& entry,bool discover=true) {
    std::shared_ptr<integrations::OwnedProcess> process;std::shared_ptr<integrations::ExecutableObservation> watch;
    std::shared_ptr<integrations::ObservedProcess> observed;
    {
      std::lock_guard lock(mutex);if(entry->snapshot.state!=SessionState::running)return;
      if(discover&&stopping)return;
      if(discover&&entry->watch&&Clock::now()<entry->next_poll)return;
      process=entry->process;watch=entry->watch;observed=entry->observed;
      if(!discover&&watch&&!observed)return;
      entry->next_poll=Clock::now()+std::chrono::milliseconds(100);
    }
    // Process enumeration, identity queries and window discovery never hold
    // the snapshot mutex. The worker is the sole watch/poll consumer.
    integrations::ProcessObservation observation;
    integrations::ExecutableMatch match;
    std::optional<integrations::SessionWindowTarget> window;
    try {
      if(watch) {
        match=observed?integrations::ExecutableMatch{integrations::ExecutableMatchState::matched,observed,{}}:watch->poll(stop.get_token());
        if(match.process){observation=match.process->observe();if(discover&&observation.state==integrations::ProcessState::running)window=match.process->window();}
      }
      else {
        observation=process->observe();
        if(discover&&observation.state==integrations::ProcessState::running&&observation.process_creation) {
          window=integrations::find_session_window(process->id());
          if(window&&window->process_creation!=observation.process_creation)window.reset();
        }
      }
    }catch(const std::exception& error){match.state=integrations::ExecutableMatchState::unavailable;match.message=error.what();observation={integrations::ProcessState::unknown,{},error.what()};}
    std::lock_guard lock(mutex);auto& e=*entry;auto& snapshot=e.snapshot;
    if(snapshot.state!=SessionState::running)return;
    if(discover&&stopping)return;
    if(watch) {
      if(snapshot.awaiting_observation&&Clock::now()>=e.deadline) {
        match.state=integrations::ExecutableMatchState::unavailable;match.message="No unique new matching Steam game was observed before the launch timeout.";
      }
      if(match.state==integrations::ExecutableMatchState::pending) {
        if(snapshot.awaiting_observation)return;
        match.state=integrations::ExecutableMatchState::unavailable;match.message="The associated game process is no longer observable.";
      }
      if(match.state!=integrations::ExecutableMatchState::matched||!match.process) {
        snapshot.state=SessionState::interrupted;
        snapshot.duration_ms=snapshot.awaiting_observation?0:static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-e.started).count());
        snapshot.awaiting_observation=false;
        snapshot.capabilities={};snapshot.window.reset();snapshot.tracking_message=match.message;changed(e);return;
      }
      if(!e.observed) {
        e.observed=match.process;e.started=Clock::now();snapshot.process_id=match.process->id();snapshot.started_at=wall_time();
        snapshot.descriptor.tracking=TrackingQuality::provider_observed;snapshot.awaiting_observation=false;
        snapshot.tracking_message=tracking_message(TrackingQuality::provider_observed);changed(e);
      }
    }
    snapshot.duration_ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-e.started).count());
    snapshot.window=window;
    if(!watch&&window&&observation.state==integrations::ProcessState::running&&
       snapshot.descriptor.kind==SessionKind::application&&snapshot.descriptor.runner=="youtube"&&
       snapshot.descriptor.tracking!=TrackingQuality::direct_child) {
      snapshot.descriptor.tracking=TrackingQuality::direct_child;
      snapshot.tracking_message="Observed application window and direct child lifetime; detached descendants are not tracked";
      changed(e);
    }
    const bool controlled=snapshot.descriptor.tracking==TrackingQuality::direct_child||snapshot.descriptor.tracking==TrackingQuality::provider_observed;
    snapshot.capabilities={controlled&&window.has_value(),controlled&&window.has_value(),controlled&&window.has_value()};
#ifndef _WIN32
    // Preserve the existing owned-child SIGTERM request on POSIX. Provider
    // observation remains unavailable; no external PID is signalled.
    snapshot.capabilities.graceful_close=snapshot.descriptor.tracking==TrackingQuality::direct_child&&observation.state==integrations::ProcessState::running;
#endif
    if(observation.state==integrations::ProcessState::running)return;
    snapshot.state=observation.state==integrations::ProcessState::exited?SessionState::exited:SessionState::interrupted;
    snapshot.ended_at=snapshot.state==SessionState::exited?wall_time():0;snapshot.exit_code=observation.exit_code;
    snapshot.capabilities={};snapshot.window.reset();
    if(!observation.message.empty())snapshot.tracking_message+="; "+observation.message;
    changed(e);
  }
  void save(const std::shared_ptr<Entry>& entry) {
    std::lock_guard saving(entry->save_mutex);SessionSnapshot snapshot;std::uint64_t revision;
    {std::lock_guard lock(mutex);if(!entry->dirty)return;snapshot=entry->snapshot;revision=entry->revision;}
    std::string error;try{std::lock_guard journal(journal_mutex);persist(snapshot);}catch(const std::exception& failure){error=failure.what();}
    std::lock_guard lock(mutex);entry->snapshot.persistence_error=std::move(error);
    if(entry->revision==revision&&entry->snapshot.persistence_error.empty())entry->dirty=false;
  }
  void run() {
    std::unique_lock lock(mutex);
    while (!stopping) {
      condition.wait_for(lock, std::chrono::milliseconds(25), [&] { return stopping; });
      if (stopping) break;
      const auto work=entries;lock.unlock();
      for (const auto& entry : work) {
        if(stop.stop_requested())break;
        update(entry);
        if(stop.stop_requested())break;
        save(entry);
      }
      lock.lock();
    }
  }
};
SessionManager::SessionManager() : impl_(std::make_unique<Impl>()) {}
SessionManager::~SessionManager() { (void)shutdown(); }
SessionLaunchResult SessionManager::launch(SessionDescriptor descriptor, const std::vector<std::string>& arguments, const fs::path& working_directory, const std::map<std::string,std::string>& environment_overrides, const std::vector<std::uintptr_t>& inherited_handles) {
  std::lock_guard launching(impl_->shutdown_mutex);
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping) return {false, {}, "Session observation has shut down"};
    if (impl_->entries.size() >= 512) {
      const auto removable = std::ranges::find_if(impl_->entries, [](const auto& value) { return value->snapshot.state != SessionState::running && !value->dirty; });
      if (removable == impl_->entries.end()) return {false, {}, "Session observation backlog is full; resolve history save failures before launching more"};
      impl_->entries.erase(removable);
    }
    if (std::ranges::count_if(impl_->entries, [](const auto& entry) { return entry->snapshot.state == SessionState::running; }) >= 256) return {false, {}, "Too many active sessions"};
  }
  if(descriptor.tracking==TrackingQuality::provider_observed)return {false,{},"Provider observation can only be established by matching a new process"};
  try { validate(descriptor); descriptor.profile_directory = fs::weakly_canonical(fs::absolute(descriptor.profile_directory));std::lock_guard journal(impl_->journal_mutex);preflight(descriptor); }
  catch (const std::exception& error) { return {false, {}, "Cannot prepare session history: " + std::string(error.what())}; }
  auto entry=std::make_shared<Impl::Entry>();
  if(descriptor.observation)try {
    const auto& spec=*descriptor.observation;const bool directory=spec.executable.empty();
    entry->watch=std::make_shared<integrations::ExecutableObservation>(directory?spec.installation_directory:spec.executable,directory);
  }
  catch(const std::exception& error){return {false,{},std::string("Cannot prepare Steam game observation: ")+error.what()};}
  const auto started = Clock::now(); auto launched = integrations::launch_owned_process(arguments, working_directory, environment_overrides, inherited_handles);
  if (!launched) return {false, {}, launched.message};
  SessionSnapshot snapshot;
  snapshot.id = "session-" + owner_token() + "-" + std::to_string(++serial); snapshot.descriptor = std::move(descriptor);
  snapshot.process_id = entry->watch?-1:launched.process->id(); snapshot.started_at = entry->watch?0:wall_time(); snapshot.tracking_message = tracking_message(snapshot.descriptor.tracking);
  snapshot.awaiting_observation=bool(entry->watch);
  if(entry->watch){entry->watch->exclude_process(launched.process->id());snapshot.tracking_message="Waiting for a unique new Steam game process; no game lifetime is credited yet.";}
  entry->snapshot=std::move(snapshot);entry->process=std::move(launched.process);entry->started=started;
  entry->deadline=started+std::chrono::milliseconds(entry->snapshot.descriptor.observation?entry->snapshot.descriptor.observation->timeout_ms:0);
  {std::lock_guard lock(impl_->mutex);impl_->entries.push_back(entry);}
  impl_->save(entry);
  SessionLaunchResult result;
  {std::lock_guard lock(impl_->mutex);result={true,entry->snapshot.id,entry->snapshot.persistence_error.empty()?"Session started":"Session started; history save will retry: "+entry->snapshot.persistence_error};}
  impl_->condition.notify_one(); return result;
}
std::vector<SessionSnapshot> SessionManager::snapshot() const {
  std::lock_guard lock(impl_->mutex); std::vector<SessionSnapshot> result; result.reserve(impl_->entries.size());
  for (const auto& entry : impl_->entries) {
    auto snapshot = entry->snapshot;
    if (snapshot.state == SessionState::running&&!snapshot.awaiting_observation) snapshot.duration_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - entry->started).count());
    result.push_back(std::move(snapshot));
  }
  return result;
}
std::optional<SessionSnapshot> SessionManager::active_session() const {
  std::lock_guard lock(impl_->mutex);
  for (auto it = impl_->entries.rbegin(); it != impl_->entries.rend(); ++it) if ((*it)->snapshot.state == SessionState::running &&
      ((*it)->snapshot.awaiting_observation||(*it)->snapshot.descriptor.tracking==TrackingQuality::direct_child||
       (*it)->snapshot.descriptor.tracking==TrackingQuality::provider_observed)) {
    auto value=(*it)->snapshot;
    if(!value.awaiting_observation)value.duration_ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-(*it)->started).count());
    return value;
  }
  return {};
}
integrations::Result SessionManager::confirm_application_window(const std::string& id,const integrations::SessionWindowTarget& target) {
  std::shared_ptr<Impl::Entry> selected;std::shared_ptr<integrations::OwnedProcess> process;
  {std::lock_guard lock(impl_->mutex);for(const auto& entry:impl_->entries)if(entry->snapshot.id==id){selected=entry;process=entry->process;break;}}
  if(selected) {
    if(!process)return {false,"The application's owned child is unavailable."};
    const auto observation=process->observe();
    if(observation.state!=integrations::ProcessState::running||!observation.process_creation||target.process_creation!=observation.process_creation)
      return {false,"The application window does not match its live owned process identity."};
    const auto valid=integrations::validate_session_window(target);if(!valid)return valid;
    {std::lock_guard lock(impl_->mutex);auto& entry=*selected;
    if(entry.snapshot.state!=SessionState::running || entry.watch || entry.snapshot.descriptor.kind!=SessionKind::application || target.pid!=entry.process->id())
      return {false,"A live window must belong to the application's owned child."};
    entry.snapshot.descriptor.tracking=TrackingQuality::direct_child;
    entry.snapshot.tracking_message="Observed application window and direct child lifetime; detached descendants are not tracked";
    entry.snapshot.window=target;entry.snapshot.capabilities={true,true,true};Impl::changed(entry);}
    impl_->save(selected);
    return {true,"Application window confirmed."};
  }
  return {false,"The application session is no longer available."};
}
integrations::Result SessionManager::quit(const std::string& session_id, integrations::ProcessStopMode mode) {
  std::shared_ptr<integrations::OwnedProcess> owned;std::shared_ptr<integrations::ObservedProcess> observed;
  std::shared_ptr<Impl::Entry> canceled;
  {
    std::lock_guard lock(impl_->mutex);
    for (const auto& entry : impl_->entries) if (entry->snapshot.id == session_id) {
      const auto& snapshot=entry->snapshot;
      if(snapshot.state!=SessionState::running)return {false,"This game has no current process capability."};
      if(snapshot.awaiting_observation) {
        if(mode!=integrations::ProcessStopMode::request_close)return {false,"Pending observation cannot force stop a forwarding launcher."};
        auto& value=entry->snapshot;value.state=SessionState::detached;value.awaiting_observation=false;
        value.duration_ms=0;value.ended_at=0;value.exit_code.reset();value.capabilities={};value.window.reset();
        value.tracking_message="Game observation canceled; the launcher and any game were left running.";
        Impl::changed(*entry);canceled=entry;break;
      }
      if(snapshot.descriptor.tracking==TrackingQuality::provider_observed) {
        if(mode!=integrations::ProcessStopMode::request_close)return {false,"An observed Steam game can only receive a graceful window close request; force stop is unavailable."};
        if(!snapshot.capabilities.graceful_close)return {false,"The observed game has no unique current window to close."};
        observed=entry->observed;
      }else if(snapshot.descriptor.tracking==TrackingQuality::direct_child) {
        if(mode==integrations::ProcessStopMode::request_close&&!snapshot.capabilities.graceful_close)return {false,"The owned game has no unique current window to close."};
        owned=entry->process;
      }
      else return {false,"A forwarding launcher is not a game process; quit from the game itself."};
      break;
    }
  }
  if(canceled){impl_->save(canceled);return {true,"Game observation canceled; running processes were left open."};}
  if(observed)return observed->request_close();
  if(owned)return owned->stop(mode);
  return {false, "This session is not owned by this manager"};
}
SessionHistory SessionManager::history(const fs::path& profile_directory, const std::string& identity) const {
  std::lock_guard journal(impl_->journal_mutex);
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
      add(result.provider_observed_playtime_ms,total.value("provider_observed_playtime_ms",std::uint64_t{}));
    }
  } catch (const std::exception& error) { result = {}; result.error = error.what(); }
  return result;
}
integrations::Result SessionManager::shutdown() {
  std::lock_guard shutdown_lock(impl_->shutdown_mutex);
  { std::lock_guard lock(impl_->mutex); impl_->stopping = true;impl_->stop.request_stop(); impl_->condition.notify_all(); }
  if (impl_->observer.joinable()) impl_->observer.join();
  std::vector<std::shared_ptr<Impl::Entry>> entries;{std::lock_guard lock(impl_->mutex);entries=impl_->entries;}
  std::string error;
  for (const auto& entry : entries) {
    // Refresh retained identities only. Pending discoveries remain unknown;
    // shutdown must not begin another executable or window enumeration.
    impl_->update(entry,false);
    {std::lock_guard lock(impl_->mutex);
      if(entry->snapshot.state==SessionState::running) {
        entry->snapshot.state=SessionState::detached;entry->snapshot.ended_at=0;entry->snapshot.capabilities={};entry->snapshot.window.reset();
        entry->snapshot.tracking_message+="; observation shut down while the process was running";Impl::changed(*entry);
      }
    }
    impl_->save(entry);
    {std::lock_guard lock(impl_->mutex);entry->process.reset();entry->watch.reset();entry->observed.reset();if(!entry->snapshot.persistence_error.empty())error=entry->snapshot.persistence_error;}
  }
  return {error.empty(), error.empty() ? "Session observation stopped; running applications were left open" : "Session observation stopped; history save failed: " + error};
}
} // namespace openxmb::desktop
