#pragma once
// Windows-only isolated foreground handoff for the interactive XMB overlay.
//
// SetForegroundWindow refuses a background process whose user request came
// from a controller (Guide is not window input). The scoped handoff shares the
// foreground thread's input queue to activate a helper-owned invisible relay,
// then hands foreground to the overlay from that foreground process. An attached
// SetForegroundWindow can block until the previous foreground thread processes
// its deactivation, which a wedged game never does. The attachment therefore
// lives in a short-lived child process (`openxmb-integrations.exe
// --focus-handoff-v2 ...`): the UI thread never attaches its own queue, waits
// only to a strict deadline while pumping its own messages, and on expiry
// terminates exactly that child. Process exit releases the child's attachment
// and relay window. Which admission step the child uses is `Strategy`.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "openxmb/integrations/desktop.hpp"
#include <windows.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace openxmb::integrations::focus_handoff {
// Private command of openxmb-integrations.exe; argv[1]. Not user-facing.
inline constexpr std::string_view command="--focus-handoff-v2";
inline constexpr wchar_t overlay_class[]=L"OpenXMB.LayeredOverlay.v1";
inline constexpr wchar_t helper_name[]=L"openxmb-integrations.exe";
// Whole handoff budget on the UI thread: helper start, validation, the
// helper's activation and the overlay's own activation messages.
inline constexpr std::chrono::milliseconds deadline{1500};
// Bound for reaping a terminated helper.
inline constexpr std::chrono::milliseconds reap_bound{1000};
// Responsiveness preflight the helper sends to the foreground window. It
// bounds only that probe, not the admission step that follows.
inline constexpr UINT preflight_ms=200;
// The helper terminates itself when its parent exits or after this lifetime,
// whichever comes first, even if its main thread is blocked.
inline constexpr DWORD helper_lifetime_ms=5000;

// Seven ASCII decimal arguments after `command`, in this order. All nonzero;
// no sign, whitespace or leading zero; pids/threads are 32-bit.
struct Request {
  DWORD parent_pid{};std::uint64_t parent_creation{};
  std::uint64_t overlay{};DWORD overlay_thread{};
  DWORD session_pid{};std::uint64_t session_creation{};std::uint64_t session_window{};
  bool operator==(const Request&) const=default;
};
[[nodiscard]] std::optional<Request> parse_request(std::span<const std::string_view> args);
// `command` followed by the seven arguments, space separated.
[[nodiscard]] std::wstring format_arguments(const Request& request);

// The helper's exit code equals its reported outcome.
enum class Outcome : std::uint32_t {
  handed_off=0,          // the helper's SetForegroundWindow(overlay) returned TRUE; parent verifies
  already_foreground=1,  // the overlay already owned the foreground
  admission_granted=2,   // the helper granted the parent; the parent activates and verifies
  malformed=10,parent_mismatch,desktop_mismatch,overlay_mismatch,session_mismatch,
  foreground_mismatch,hung,preflight_failed,changed,attach_failed,refused,detach_failed,internal,
  relay_failed,          // the helper's relay window could not be created
  relay_refused,         // the relay did not become the foreground window
  grant_refused,         // AllowSetForegroundWindow(parent) returned FALSE
};
[[nodiscard]] std::string_view describe(Outcome outcome);
// How the helper obtains foreground admission. Production uses
// production_strategy; the others exist for the test-only probe helper and
// adoption is a change of that one constant.
enum class Strategy : std::uint32_t {
  // Attached to the foreground thread: SetForegroundWindow(overlay).
  attached_activate=0,
  // Attached: AllowSetForegroundWindow(parent); the parent activates.
  attached_grant=1,
  // Attached: SetForegroundWindow(relay) on the shared foreground queue, then
  // as the foreground process SetForegroundWindow(overlay).
  relay_activate=2,
  // As relay_activate, but AllowSetForegroundWindow(parent); the parent activates.
  relay_grant=3,
  // No attachment: ShowWindow(SW_RESTORE) then SetForegroundWindow on a
  // minimized relay (the call pair SDL_RestoreWindow+SDL_RaiseWindow makes,
  // which regained the foreground for the shell after RPCS3 refusals); if the
  // relay became the foreground, SetForegroundWindow(overlay). Undocumented.
  restore_activate=4,
  // As restore_activate, but AllowSetForegroundWindow(parent); the parent activates.
  restore_grant=5,
  // Test-only negative control: plain activation, no attach, relay or grant.
  unattached_activate=6,
};
inline constexpr Strategy production_strategy=Strategy::relay_activate;
// Tri-state BOOL fields: 0 FALSE, 1 TRUE, 2 not called.
// Diagnostic fields hold `unavailable` when the query failed or did not run.
inline constexpr std::uint32_t unavailable=0xFFFFFFFFu;
struct Report {
  Outcome outcome{Outcome::internal};DWORD error{};std::uint32_t activation{2};
  std::uint64_t foreground_before{},foreground_after{};
  Strategy strategy{production_strategy};
  std::uint32_t relay{2},grant{2},rights_before{2};
  // Read-only state when the admission step ran: the foreground thread's
  // GUITHREADINFO (flags, capture and menu-owner windows), the overlay
  // thread's flags, SPI_GETFOREGROUNDLOCKTIMEOUT and ms since the last input.
  std::uint32_t source_flags{unavailable};std::uint64_t source_capture{},source_menu_owner{};
  std::uint32_t overlay_flags{unavailable},lock_timeout{unavailable},idle_ms{unavailable};
  bool operator==(const Report&) const=default;
};
// One line: "OXFH2 <outcome> <error> <activation> <before> <after> <strategy>
// <relay> <grant> <rights_before> <source_flags> <source_capture>
// <source_menu_owner> <overlay_flags> <lock_timeout> <idle_ms>\n".
[[nodiscard]] std::string format_report(const Report& report);
[[nodiscard]] std::optional<Report> parse_report(std::string_view text);
// Report fields after the outcome, for logs: "[outcome N, ...]".
[[nodiscard]] std::string report_text(const Report& report);
struct ProcessFacts {std::uint64_t creation{};std::wstring image;};
[[nodiscard]] ProcessFacts process_facts(DWORD pid);
// `window` is `session` or a window it owns (dialogs, fullscreen popups).
[[nodiscard]] bool session_window_or_owned(HWND window,HWND session);
[[nodiscard]] std::string hwnd_text(std::uint64_t window);
[[nodiscard]] std::string narrow(std::wstring_view text);
// "action (Windows error N: text)."; `code` captured right after the failure.
[[nodiscard]] std::string windows_error(std::string_view action,DWORD code);

// Helper-side checks, individually testable in-process. Empty means passed.
[[nodiscard]] std::optional<Report> check_overlay(const Request& request);
[[nodiscard]] std::optional<Report> check_session(const Request& request);
// SendMessageTimeoutW(WM_NULL) to `window`; error is the failing GetLastError.
[[nodiscard]] std::optional<Report> preflight(HWND window,UINT timeout_ms=preflight_ms);

// The helper's admission relay: a 1x1, alpha-0, click-through layered tool
// window off-screen on this thread (no taskbar button, never NOACTIVATE),
// shown without activation, or minimized when `minimized`. Destroyed by
// ~Relay; error holds GetLastError of a failed creation.
class Relay {
  HWND window_{};DWORD error_{};
public:
  explicit Relay(bool minimized) noexcept;
  ~Relay();
  Relay(const Relay&)=delete;
  Relay& operator=(const Relay&)=delete;
  [[nodiscard]] HWND window() const noexcept {return window_;}
  [[nodiscard]] DWORD error() const noexcept {return error_;}
};

// Test-only seam: production (cli.cpp) passes no hooks.
struct Hooks {
  void (*after_attach)()=nullptr;
  void (*after_relay)()=nullptr;
  // Receives the exact report line run_helper writes to stdout.
  void (*on_report)(std::string_view line)=nullptr;
  Strategy strategy=production_strategy;
  // Records rights_before=AllowSetForegroundWindow(own pid) before admission.
  // Never in production: any ASFW call can cancel another process's grant.
  bool measure_rights=false;
};
// Validates parent, desktop, overlay, session and foreground identity, probes
// the foreground, revalidates, then obtains admission per hooks.strategy
// (attachments always end with a checked detach). Activates nothing but
// request.overlay and its own relay; grants only request.parent_pid.
[[nodiscard]] Report perform(const Request& request,const Hooks& hooks={});
// Entry for argv[2..]: writes the report to stdout and returns the outcome code.
int run_helper(std::span<const std::string_view> args,const Hooks& hooks={}) noexcept;

// Parent side.
struct HelperRun {
  bool launched{},exited{},timed_out{},terminated{},reaped{};
  DWORD launch_error{},terminate_error{},pid{},exit_code{STILL_ACTIVE};
  std::optional<Report> report;  // present only when well-formed and equal to exit_code
  std::chrono::milliseconds elapsed{};
};
// Directory of this process's executable joined with helper_name.
[[nodiscard]] std::filesystem::path sibling_helper();
// Starts exactly `helper` with `request` on this thread's desktop and waits
// until it exits or `until`, dispatching `pump_window`'s messages and other
// threads' sent messages meanwhile. On expiry terminates and reaps only this
// helper. Never throws; every handle is closed on return.
[[nodiscard]] HelperRun launch(const std::filesystem::path& helper,const Request& request,
  std::chrono::steady_clock::time_point until,HWND pump_window) noexcept;
// Pumps like launch() until GetForegroundWindow()==window or `until`.
bool await_foreground(HWND window,std::chrono::steady_clock::time_point until) noexcept;
// Runs the helper and succeeds only when it exited after reporting a handoff
// (or a grant, after which this thread calls SetForegroundWindow(window)) and
// this thread then observes `window` (request.overlay) in the foreground.
// Failure messages describe the helper's facts without a leading capital.
[[nodiscard]] Result hand_off(HWND window,const Request& request,const std::filesystem::path& helper,
  std::chrono::steady_clock::time_point until);
} // namespace openxmb::integrations::focus_handoff
#endif
