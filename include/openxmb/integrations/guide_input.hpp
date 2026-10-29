#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace openxmb::integrations {

struct GuideButtonEvent {
  bool pressed{};
  // GameInput's monotonic microsecond clock, not wall-clock or SDL ticks.
  std::uint64_t timestamp_us{};
  // The same event normalized to steady_clock's microsecond epoch for timing
  // and source deduplication. Values outside that epoch saturate at 0/max.
  std::uint64_t steady_timestamp_us{};
};

// Optional Windows SDK v0 GameInput Guide listener. Missing runtime/header/API
// leaves the existing input fallback usable. All public methods, including
// destruction, belong to the UI thread. GameInput's worker only queues edges.
// Start only while a session needs Guide input, then stop when it ends.
//
// Background delivery is best effort: another foreground owner's exclusive
// policy, the Windows runtime or unsupported hardware can suppress callbacks.
class GuideInput {
public:
  GuideInput() noexcept;
  ~GuideInput();
  GuideInput(const GuideInput&) = delete;
  GuideInput& operator=(const GuideInput&) = delete;

  [[nodiscard]] bool start() noexcept;
  // Disables delivery immediately. COM/DLL release runs on a cleanup worker;
  // start can return false while another adapter owns the process runtime or
  // its previous teardown is pending. At most one runtime is active per process.
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept;
  // Consumes up to 64 queued edges. On overflow, the oldest edges are discarded
  // while retaining the newest state (including a release). stop discards all
  // pending edges, so a later session cannot replay them.
  [[nodiscard]] std::vector<GuideButtonEvent> drain();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace openxmb::integrations
