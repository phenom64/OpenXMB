#pragma once
// Only included when compiling the focused test target with
// OPENXMB_MUSIC_CALLBACK_TESTS. No diagnostics/timers/hooks ship in production.
#include <atomic>
#include <cstdint>

namespace openxmb::music::callback_test {
struct Audit {
  std::atomic<bool> hold_callback{}, callback_held{};
  std::atomic<bool> hold_producer{}, producer_held{};
  std::atomic<std::uint64_t> calls{}, callbacks{}, completed{}, timed{}, max_ns{};
  std::atomic<std::uint64_t> quiesces{};
  std::atomic<std::uint64_t> allocations{}, invariant_failures{};
  std::atomic<std::uint64_t> epoch{}, last_output_epoch{};
  std::atomic<std::uint64_t> last_frames{};
  std::atomic<std::uint64_t> capacity{}, max_queued{};
  std::atomic<std::uint64_t> max_read{}, max_write{}, analysis_drops{};
  std::atomic<std::uint64_t> max_buffer_frames{};
};
extern Audit audit;
extern thread_local bool in_callback;
}
