#include "openxmb/integrations/guide_input.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <thread>

#if defined(_WIN32) && __has_include(<GameInput.h>)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GameInput.h>
// The in-box SDK 10.0.26100.0 has the original global v0 ABI. Newer NuGet
// headers use versioned namespaces/factories and must not be mixed with it.
#if !defined(GAMEINPUT_API_VERSION) || GAMEINPUT_API_VERSION == 0
#define OPENXMB_GAMEINPUT_V0 1
#endif
#endif

namespace openxmb::integrations {

#ifdef OPENXMB_GAMEINPUT_V0
namespace {
// The in-box v0 wrapper redirects to a separately serviced runtime. Keep its
// initialization/teardown exclusive across adapter instances: overlapping
// final Release calls crashed that redirected runtime during live validation.
std::atomic<bool> guide_runtime_owned{};

constexpr std::uint64_t normalize_guide_timestamp(std::uint64_t timestamp,
                                                 std::uint64_t input_anchor,
                                                 std::uint64_t steady_anchor) noexcept {
  if (timestamp >= input_anchor) {
    const auto delta = timestamp - input_anchor;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return delta > maximum - steady_anchor ? maximum : steady_anchor + delta;
  }
  const auto delta = input_anchor - timestamp;
  return delta > steady_anchor ? 0 : steady_anchor - delta;
}
}
#endif

struct GuideInput::Impl {
  static constexpr std::size_t capacity = 64;
  std::mutex mutex;
  std::array<GuideButtonEvent, capacity> events{};
  std::size_t first{}, size{};
  bool accepting{};

#ifdef OPENXMB_GAMEINPUT_V0
  HMODULE module{};
  IGameInput* input{};
  GameInputCallbackToken token{};
  bool owns_runtime{};
  bool configured{};
  // Written before registration, then immutable until unregister completes.
  std::uint64_t input_timestamp_anchor{}, steady_timestamp_anchor{};
  std::shared_ptr<std::atomic<bool>> releasing{std::make_shared<std::atomic<bool>>(false)};

  static void CALLBACK on_button(GameInputCallbackToken, void* context,
                                 IGameInputDevice*, std::uint64_t timestamp,
                                 GameInputSystemButtons current,
                                 GameInputSystemButtons previous) noexcept {
    const bool pressed = (current & GameInputSystemButtonGuide) != 0;
    const bool was_pressed = (previous & GameInputSystemButtonGuide) != 0;
    if (pressed == was_pressed) return;
    auto& self = *static_cast<Impl*>(context);
    std::lock_guard lock(self.mutex);
    if (!self.accepting) return;
    if (self.size == capacity) {
      self.first = (self.first + 1) % capacity;
      --self.size;
    }
    self.events[(self.first + self.size) % capacity] = {
      pressed, timestamp, normalize_guide_timestamp(timestamp, self.input_timestamp_anchor,
                                                   self.steady_timestamp_anchor)};
    ++self.size;
  }

  bool start() noexcept {
    if (token) return accepting;
    if (releasing->load(std::memory_order_acquire)) return false;
    // A failed cleanup-thread allocation may have retained resources. Retry
    // their cleanup before permitting a new runtime instance.
    if (input || module) { stop(); return false; }
    bool expected = false;
    if (!guide_runtime_owned.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
    owns_runtime = true;
    module = ::LoadLibraryExW(L"GameInput.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) { stop(); return false; }
    using Create = HRESULT (WINAPI*)(IGameInput**);
    const auto create = reinterpret_cast<Create>(::GetProcAddress(module, "GameInputCreate"));
    if (!create || FAILED(create(&input)) || !input) {
      stop();
      return false;
    }

    // GameInput is COM-lite: reference-counted agile objects, with no COM
    // apartment initialization or CoCreateInstance. Verify the v0 interface
    // before using slots beyond IUnknown when loading the optional factory.
    IGameInput* verified{};
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#endif
    const auto result = input->QueryInterface(IID_PPV_ARGS(&verified));
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    if (FAILED(result) || !verified) {
      if (verified) verified->Release();
      stop();
      return false;
    }
    input->Release(); // QueryInterface holds a second reference, so not final.
    input = verified;

    // Installed v0 defines DisableBackgroundGuideButton=0x4 and
    // ExclusiveForegroundGuideButton=0x8. Leave background disabling unset;
    // v0 has no EnableBackgroundGuideButton (the modern 0x80 flag). Runtime
    // versions have different background defaults, so delivery is best effort.
    // Exclusivity applies only while our own process is foreground, and only
    // to Guide. It never requests foreground ownership over another app.
    input->SetFocusPolicy(GameInputExclusiveForegroundGuideButton);
    configured = true;
    const auto input_now = input->GetCurrentTimestamp();
    const auto steady_now = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
    {
      std::lock_guard lock(mutex);
      first = size = 0;
      input_timestamp_anchor = input_now;
      steady_timestamp_anchor = steady_now > 0 ? static_cast<std::uint64_t>(steady_now) : 0;
      accepting = true;
    }
    const auto registered = input->RegisterSystemButtonCallback(
      nullptr, GameInputSystemButtonGuide, this, &on_button, &token);
    if (FAILED(registered) || !token) {
      stop();
      return false;
    }
    return true;
  }

  void stop() noexcept {
    {
      std::lock_guard lock(mutex);
      accepting = false;
      first = size = 0;
    }
    if (input && configured) {
      input->SetFocusPolicy(GameInputDefaultFocusPolicy);
      configured = false;
    }
    if (input && token) {
      // Never hold the queue mutex across cancellation: an in-flight callback
      // may still need it. Stop prevents new dispatch, then a bounded v0 wait
      // confirms callbacks finished before context/COM/DLL resources are freed.
      input->StopCallback(token);
      if (!input->UnregisterCallback(token, 10000)) {
        // A failed unregister cannot prove that context/DLL are safe to free.
        // Retain this disabled registration for a later stop retry; destruction
        // deliberately retains its resources if the runtime never confirms it.
        return;
      }
      token = 0;
    }
    if (input || module) {
      // The installed v0 runtime can take eight seconds to release its final
      // interface. Callback cancellation is already confirmed above; retain
      // the COM reference and module together off the UI thread until done.
      releasing->store(true, std::memory_order_release);
      try {
        std::thread cleanup([owned_input = input, owned_module = module, state = releasing] {
          if (owned_input) owned_input->Release();
          if (owned_module) ::FreeLibrary(owned_module);
          guide_runtime_owned.store(false, std::memory_order_release);
          state->store(false, std::memory_order_release);
        });
        cleanup.detach();
        input = nullptr;
        module = nullptr;
        owns_runtime = false;
      } catch (...) {
        releasing->store(false, std::memory_order_release);
        // Retain resources for a later retry if a worker cannot be created.
      }
    } else if (owns_runtime) {
      owns_runtime = false;
      guide_runtime_owned.store(false, std::memory_order_release);
    }
  }
#else
  bool start() noexcept { return false; }
  void stop() noexcept {}
#endif
};

GuideInput::GuideInput() noexcept = default;

GuideInput::~GuideInput() {
  stop();
#ifdef OPENXMB_GAMEINPUT_V0
  // Fail safe against a broken runtime: a live callback must never retain a
  // dangling context or an unloaded DLL. This exceptional path leaks the one
  // disabled registration until process exit instead of risking use-after-free.
  if (impl_ && (impl_->token || impl_->input || impl_->module)) static_cast<void>(impl_.release());
#endif
}

bool GuideInput::start() noexcept {
  try {
    if (!impl_) impl_ = std::make_unique<Impl>();
    return impl_->start();
  } catch (...) {
    stop();
    return false;
  }
}

void GuideInput::stop() noexcept {
  if (impl_) impl_->stop();
}

bool GuideInput::running() const noexcept {
#ifdef OPENXMB_GAMEINPUT_V0
  return impl_ && impl_->token && impl_->accepting;
#else
  return false;
#endif
}

std::vector<GuideButtonEvent> GuideInput::drain() {
  std::vector<GuideButtonEvent> result;
  if (!impl_) return result;
  std::lock_guard lock(impl_->mutex);
  result.reserve(impl_->size);
  for (std::size_t offset = 0; offset < impl_->size; ++offset)
    result.push_back(impl_->events[(impl_->first + offset) % Impl::capacity]);
  impl_->first = impl_->size = 0;
  return result;
}

} // namespace openxmb::integrations
