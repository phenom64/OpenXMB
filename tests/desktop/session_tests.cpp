#include "openxmb/desktop/session.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
using namespace openxmb::desktop;
using namespace openxmb::integrations;
using namespace std::chrono_literals;
namespace {
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
std::string text(const fs::path& path) { const auto value = path.u8string(); return {value.begin(), value.end()}; }
template<class Predicate> void wait(Predicate predicate, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!predicate()) { if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message); std::this_thread::sleep_for(5ms); }
}
SessionDescriptor descriptor(const fs::path& profile, std::string id = "fixture-game") {
  return {std::move(id), "Fixture title", "native", "fixture", profile, SessionKind::game, TrackingQuality::direct_child};
}
void process_checks(const fs::path& root, const fs::path& helper) {
  require(!launch_owned_process({text(root / "does-not-exist")}), "failed exec must be reported promptly");
  require(!launch_owned_process({text(helper), "record", "0", "0"}, root / "missing-cwd"), "failed chdir must be reported before launch succeeds");
  require(!launch_owned_process({text(helper), std::string("bad\0argument", 12)}), "NUL argument must be rejected");
  fs::create_directories(root);
  const auto process = launch_owned_process({text(helper), "wait", "ready", "release"}, root);
  require(bool(process), "native owned process did not launch");
  wait([&] { return fs::exists(root / "ready"); }, "native process never became ready");
  require(process.process->observe().state == ProcessState::running, "live process was reported exited");
  require(!stop_process(process.process->id()), "a bare PID must not adopt or stop a tracked session capability");
  require(process.process->observe().state == ProcessState::running, "unowned PID stop request affected child");
  std::ofstream(root / "release") << "release";
  wait([&] { return process.process->observe().state != ProcessState::running; }, "child exit was not observed");
  for (unsigned i = 0; i < 10; ++i) require(process.process->observe().exit_code == 0, "owned exit status must stay cached across repeated observations");
  const auto legacy = launch_process({text(helper), "record", "17", "0"}, root);
  require(bool(legacy), "legacy launcher failed");
  wait([&] { return process_exit_status(legacy.process_id).has_value(); }, "legacy exit never observed");
  for (unsigned i = 0; i < 10; ++i) require(process_exit_status(legacy.process_id) == 17, "legacy repeated polling lost actual exit code or double-reaped");
  require(!stop_process(legacy.process_id), "finished legacy process must not be signalled");
  const auto special = launch_owned_process({text(helper), "record", "259", "0"}, root);
  require(bool(special), "special exit fixture failed to launch");
  wait([&] { return special.process->observe().state != ProcessState::running; }, "special exit code was confused with a live process");
#ifdef _WIN32
  require(special.process->observe().exit_code == 259, "Windows STILL_ACTIVE-valued exit code was lost");
#else
  require(special.process->observe().exit_code == 3, "POSIX exit status did not preserve native eight-bit semantics");
#endif
}
void manager_checks(const fs::path& root, const fs::path& helper) {
  SessionManager manager;
  const auto profile = root / fs::path(std::u8string(u8"profile \u6771\u4eac"));
  const auto cwd = root / fs::path(std::u8string(u8"working \u6771\u4eac")); fs::create_directories(cwd);
  const auto failed = manager.launch(descriptor(profile), {text(root / "absent")}, cwd);
  require(!failed && manager.snapshot().empty() && manager.history(profile).launch_count == 0, "failed launch created a session/history count");
  const std::vector<std::string> argv{text(helper), "record", "9", "180", "", "spaces and 'quotes'", "ends\\", "a\\\"b", "$(touch injected); &", std::string(reinterpret_cast<const char*>(u8"\u6771\u4eac \U0001f3ae"))};
  const auto launched = manager.launch(descriptor(profile), argv, cwd);
  require(bool(launched) && manager.active_session().has_value(), "session launch did not expose an active snapshot");
  require(!manager.quit("not-a-session"), "unknown session quit was accepted");
  // history() only reads the journal. No UI tick/snapshot call drives exits.
  wait([&] { const auto h = manager.history(profile); return h.error.empty() && !h.recent.empty() && h.recent.front().state == SessionState::exited; }, "independent observer did not finalize child while UI was idle");
  auto history = manager.history(profile, "fixture-game");
  require(history.launch_count == 1 && history.recent.size() == 1 && history.recent.front().exit_code == 9, "session completion did not preserve launch count/exit status");
  require(history.total_playtime_ms >= 100 && history.recent.front().duration_ms >= 100, "steady-clock direct-child playtime was not measured");
  require(history.recent.front().started_at > 0 && history.recent.front().ended_at >= history.recent.front().started_at, "wall timestamps are invalid");
  require(manager.history(root / "other-profile").launch_count == 0, "history leaked into another originating profile");
  nlohmann::json recorded; { std::ifstream input(cwd / "arguments.json"); input >> recorded; }
  require(recorded.at("arguments") == argv && recorded.at("cwd") == text(fs::weakly_canonical(cwd)), "native literal argv/Unicode/cwd changed");
  require(!fs::exists(cwd / "injected"), "argument text was interpreted by a shell");
  const auto expected_duration = history.total_playtime_ms;
  for (unsigned i = 0; i < 20; ++i) { (void)manager.snapshot(); history = manager.history(profile); }
  require(history.launch_count == 1 && history.total_playtime_ms == expected_duration, "completion was persisted more than once");
  auto launcher = descriptor(profile, "forwarder"); launcher.kind = SessionKind::application; launcher.tracking = TrackingQuality::launcher_only;
  const auto forwarding = manager.launch(launcher, {text(helper), "record", "0", "20"}, cwd);
  require(bool(forwarding), "launcher-only session did not start");
  wait([&] { const auto h = manager.history(profile, "forwarder"); return !h.recent.empty() && h.recent.front().state == SessionState::exited; }, "launcher-only session did not finish");
  const auto forwarding_history = manager.history(profile, "forwarder");
  require(forwarding_history.total_playtime_ms == 0 && !forwarding_history.recent.front().tracking_message.empty(), "forwarding launcher lifetime was misrepresented as application playtime");
  require(bool(manager.shutdown()), "clean manager shutdown failed");
  SessionManager reopened;
  require(reopened.history(profile, "fixture-game").total_playtime_ms == expected_duration, "durable history did not survive manager recreation");
  require(!fs::exists(profile / "library.json"), "session backend wrote library.json");
}
void journal_checks(const fs::path& root, const fs::path& helper) {
  const auto profile = root / "profile"; const auto cwd = root / "cwd"; fs::create_directories(cwd);
  SessionManager manager;
  const auto launched = manager.launch(descriptor(profile), {text(helper), "wait", "ready", "release"}, cwd);
  require(bool(launched), "journal retry fixture did not launch");
  wait([&] { return fs::exists(cwd / "ready"); }, "journal retry fixture not ready");
  const auto journal = profile / "sessions/history.json";
  nlohmann::json original; { std::ifstream input(journal); input >> original; }
  fs::remove(journal); fs::create_directory(journal);
  std::ofstream(cwd / "release") << "release";
  wait([&] { const auto snapshots = manager.snapshot(); return !snapshots.empty() && snapshots.front().state == SessionState::exited && !snapshots.front().persistence_error.empty(); }, "background persistence failure was silently hidden");
  fs::remove(journal); { std::ofstream output(journal); output << original; }
  wait([&] { const auto h = manager.history(profile); return h.error.empty() && !h.recent.empty() && h.recent.front().state == SessionState::exited; }, "failed completion journal update did not retry");
  require(manager.history(profile).launch_count == 1, "completion persistence retry counted the launch twice");
  nlohmann::json completed; { std::ifstream input(journal); input >> completed; }
  auto stale = original; stale["recent"][0]["owner"] = "previous-process-instance";
  { std::ofstream output(journal); output << stale; }
  require(manager.history(profile).recent.front().state == SessionState::interrupted, "stale running record was represented as a live/adopted process");
  const auto next = manager.launch(descriptor(profile, "next"), {text(helper), "record", "0", "0"}, cwd);
  require(bool(next), "new session could not normalize old interrupted records");
  nlohmann::json normalized; { std::ifstream input(journal); input >> normalized; }
  require(normalized["recent"][0]["state"] == static_cast<int>(SessionState::interrupted), "stale running records must be durably normalized so they remain pruneable");
  wait([&] { return !manager.active_session(); }, "normalization fixture did not exit");
  const auto invalid_profile = root / "invalid-profile"; fs::create_directories(invalid_profile / "sessions");
  const auto invalid_journal = invalid_profile / "sessions/history.json";
  auto corrupt = completed; corrupt["totals"][0]["launch_count"] = -1;
  { std::ofstream output(invalid_journal); output << corrupt; }
  require(!manager.history(invalid_profile).error.empty(), "negative journal counters were accepted as unsigned history");
  corrupt = completed; corrupt["recent"].push_back(corrupt["recent"][0]);
  { std::ofstream output(invalid_journal); output << corrupt; }
  require(!manager.history(invalid_profile).error.empty(), "duplicate durable session identities were accepted");
  { std::ofstream output(invalid_journal, std::ios::binary); output.seekp(4 * 1024 * 1024); output.put('x'); }
  require(!manager.history(invalid_profile).error.empty() && !manager.launch(descriptor(invalid_profile), {text(helper), "record", "0", "0"}, cwd), "oversized journal was accepted or silently overwritten before launch");
}
void quit_and_shutdown(const fs::path& root, const fs::path& helper) {
  const auto profile = root / "profile"; const auto cwd = root / "cwd"; fs::create_directories(cwd);
  SessionManager manager;
  const auto session = manager.launch(descriptor(profile), {text(helper), "wait", "ready", "release"}, cwd);
  require(bool(session), "quit fixture did not start");
  wait([&] { return fs::exists(cwd / "ready"); }, "quit fixture not ready");
#ifdef _WIN32
  require(!manager.quit(session.session_id, ProcessStopMode::request_close), "console child without a window must not be silently forcibly terminated");
  require(manager.active_session().has_value(), "graceful request silently killed console child");
#endif
  require(bool(manager.quit(session.session_id, ProcessStopMode::force_stop)), "explicit owned-process stop failed");
  wait([&] { return !manager.active_session(); }, "owned process stop was not observed");
  require(!manager.quit(session.session_id, ProcessStopMode::force_stop), "ended session accepted another stop");
  fs::remove(cwd / "ready");
  const auto detached = manager.launch(descriptor(profile, "shutdown-survivor"), {text(helper), "wait", "ready", "release"}, cwd);
  require(bool(detached), "shutdown fixture failed to launch");
  wait([&] { return fs::exists(cwd / "ready"); }, "shutdown fixture not ready");
#ifdef _WIN32
  const auto detached_pid = manager.active_session()->process_id;
#endif
  const auto started = std::chrono::steady_clock::now();
  require(bool(manager.shutdown()), "shutdown could not persist detached observation");
  require(std::chrono::steady_clock::now() - started < 1s, "manager shutdown waited for running child");
  std::ofstream(cwd / "release") << "release";
  wait([&] { return fs::exists(cwd / "completed"); }, "manager shutdown killed the user's running process");
#ifdef _WIN32
  // The completion marker proves survival; process exit additionally releases
  // Windows' current-directory handle before the fixture tree is removed.
  wait([&] { return process_exit_status(detached_pid).has_value(); }, "shutdown survivor did not finish releasing its working directory");
#endif
  SessionManager reader;
  const auto history = reader.history(profile, "shutdown-survivor");
  require(history.recent.size() == 1 && history.recent.front().state == SessionState::detached && history.recent.front().ended_at == 0 && history.total_playtime_ms == 0, "shutdown claimed an unobserved game exit or duration");
  const auto blocked = root / "blocked"; fs::create_directories(blocked); std::ofstream(blocked / "sessions") << "blocked directory";
  require(!reader.launch(descriptor(blocked), {text(helper), "record", "0", "0"}, cwd), "unwritable session journal launched an unjournalled child");
}
}
int main(int argc, char** argv) {
  if (argc != 2) { std::cerr << "expected native session child path\n"; return 2; }
  const auto root = fs::weakly_canonical(fs::temp_directory_path()) / ("openxmb-session-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    const auto helper = fs::absolute(argv[1]);
    process_checks(root / "process", helper);
    manager_checks(root / "manager", helper);
    journal_checks(root / "journal", helper);
    quit_and_shutdown(root / "quit", helper);
    fs::remove_all(root); std::cout << "Owned process and session journal checks passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
