// Deterministic v0 runtime boundary tests. No controller input or focus changes.
#include <windows.h>
#include <GameInput.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
template<class Predicate>
void wait_for(Predicate done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!done() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(done(), "asynchronous cleanup did not finish");
}
struct Runtime;
Runtime* runtime{};
HMODULE WINAPI test_load_library(LPCWSTR, HANDLE, DWORD);
FARPROC WINAPI test_get_proc(HMODULE, LPCSTR);
BOOL WINAPI test_free_library(HMODULE);
}

// Substitute only the DLL boundary, keeping the real queue and callback code.
#define LoadLibraryExW test_load_library
#define GetProcAddress test_get_proc
#define FreeLibrary test_free_library
#include "../../src/integrations/guide_input.cpp"
#undef LoadLibraryExW
#undef GetProcAddress
#undef FreeLibrary

namespace {
struct Runtime : IGameInput {
  bool available{true}, symbol{true}, creation{true}, registration{true};
  bool interface_supported{true}, unregister_succeeds{true};
  bool invalid_interface_slot{};
  bool stopped{}, unregistered{}, released{};
  std::atomic<bool> unloaded{}, release_permitted{true};
  unsigned loads{}, references{};
  uint64_t current_timestamp{1000};
  bool timestamp_was_read{};
  GameInputFocusPolicy policy{GameInputDefaultFocusPolicy};
  GameInputSystemButtonCallback callback{};
  void* context{};
  std::thread pending;
  std::atomic<bool> invoke_pending{};
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** value) noexcept override {
    if (!interface_supported) { *value = nullptr; return E_NOINTERFACE; }
    *value = this; AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    require(!callback || unregistered, "Release before callback unregistered");
    const auto left = --references;
    if (!left) while (!release_permitted) std::this_thread::yield();
    released = true; return left;
  }
  uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() noexcept override {
    timestamp_was_read = true;
    return current_timestamp;
  }
  HRESULT STDMETHODCALLTYPE GetCurrentReading(GameInputKind, IGameInputDevice*, IGameInputReading**) noexcept override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetNextReading(IGameInputReading*, GameInputKind, IGameInputDevice*, IGameInputReading**) noexcept override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPreviousReading(IGameInputReading*, GameInputKind, IGameInputDevice*, IGameInputReading**) noexcept override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetTemporalReading(uint64_t, IGameInputDevice*, IGameInputReading**) noexcept override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE RegisterReadingCallback(IGameInputDevice*, GameInputKind, float, void*, GameInputReadingCallback, GameInputCallbackToken*) noexcept override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE RegisterDeviceCallback(IGameInputDevice*, GameInputKind, GameInputDeviceStatus, GameInputEnumerationKind, void*, GameInputDeviceCallback, GameInputCallbackToken*) noexcept override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE RegisterSystemButtonCallback(IGameInputDevice* device, GameInputSystemButtons filter, void* ctx, GameInputSystemButtonCallback fn, GameInputCallbackToken* token) noexcept override {
    require(device == nullptr && filter == GameInputSystemButtonGuide, "registration must filter only Guide on all devices");
    require(timestamp_was_read, "clock anchor must be captured before callback registration");
    timestamp_was_read = false;
    if (!registration) { *token = 0; return E_FAIL; }
    stopped = unregistered = false; callback = fn; context = ctx; *token = 42; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE RegisterKeyboardLayoutCallback(IGameInputDevice*, void*, GameInputKeyboardLayoutCallback, GameInputCallbackToken*) noexcept override { return E_NOTIMPL; }
  void STDMETHODCALLTYPE StopCallback(GameInputCallbackToken token) noexcept override {
    require(token == 42, "stop token mismatch"); stopped = true;
    invoke_pending = true;
  }
  bool STDMETHODCALLTYPE UnregisterCallback(GameInputCallbackToken token, uint64_t timeout) noexcept override {
    require(stopped && token == 42 && timeout == 10000, "stop/unregister ordering or bounded v0 timeout mismatch");
    if (pending.joinable()) pending.join();
    if (!unregister_succeeds) return false;
    unregistered = true; return true;
  }
  HRESULT STDMETHODCALLTYPE CreateDispatcher(IGameInputDispatcher**) noexcept override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateAggregateDevice(GameInputKind, IGameInputDevice**) noexcept override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE FindDeviceFromId(APP_LOCAL_DEVICE_ID const*, IGameInputDevice**) noexcept override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE FindDeviceFromObject(IUnknown*, IGameInputDevice**) noexcept override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformHandle(HANDLE, IGameInputDevice**) noexcept override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformString(LPCWSTR, IGameInputDevice**) noexcept override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnableOemDeviceSupport(uint16_t, uint16_t, uint8_t, uint8_t) noexcept override { return E_NOTIMPL; }
  void STDMETHODCALLTYPE SetFocusPolicy(GameInputFocusPolicy value) noexcept override {
    if (!interface_supported) invalid_interface_slot = true;
    policy = value;
  }
  void edge(bool down, uint64_t timestamp) {
    callback(42, context, nullptr, timestamp,
      down ? GameInputSystemButtonGuide : GameInputSystemButtonNone,
      down ? GameInputSystemButtonNone : GameInputSystemButtonGuide);
  }
};
HRESULT WINAPI test_create(IGameInput** value) {
  *value = nullptr;
  if (!runtime->creation) return E_FAIL;
  *value = runtime; runtime->AddRef(); return S_OK;
}
HMODULE WINAPI test_load_library(LPCWSTR name, HANDLE handle, DWORD flags) {
  require(std::wstring(name) == L"GameInput.dll" && handle == nullptr && flags == LOAD_LIBRARY_SEARCH_SYSTEM32,
          "optional runtime must load only from System32");
  ++runtime->loads;
  runtime->unloaded = false;
  return runtime->available ? reinterpret_cast<HMODULE>(runtime) : nullptr;
}
FARPROC WINAPI test_get_proc(HMODULE module, LPCSTR name) {
  require(module == reinterpret_cast<HMODULE>(runtime) && std::string(name) == "GameInputCreate", "factory lookup mismatch");
  return runtime->symbol ? std::bit_cast<FARPROC>(&test_create) : nullptr;
}
BOOL WINAPI test_free_library(HMODULE module) {
  require(module == reinterpret_cast<HMODULE>(runtime), "unload module mismatch");
  require(runtime->references == 0, "DLL unloaded before COM references released");
  require(!runtime->callback || runtime->unregistered, "DLL unloaded before callback unregistered");
  runtime->unloaded = true; return TRUE;
}
}

int main() {
  using openxmb::integrations::GuideInput;
  try {
    using openxmb::integrations::normalize_guide_timestamp;
    constexpr auto maximum = std::numeric_limits<uint64_t>::max();
    require(normalize_guide_timestamp(125, 100, 1000) == 1025, "forward normalization delta changed");
    require(normalize_guide_timestamp(75, 100, 1000) == 975, "backward normalization delta changed");
    require(normalize_guide_timestamp(0, maximum, 42) == 0, "backward normalization must clamp at zero");
    require(normalize_guide_timestamp(maximum, 0, 42) == maximum, "forward normalization must saturate at UINT64_MAX");
    require(normalize_guide_timestamp(maximum, maximum, maximum) == maximum, "anchor identity overflowed");
    require(normalize_guide_timestamp(0, maximum, maximum) == 0, "full-range backward delta overflowed");
    for (int failure = 0; failure < 5; ++failure) {
      Runtime fake; runtime = &fake;
      fake.available = failure != 0; fake.symbol = failure != 1;
      fake.creation = failure != 2; fake.registration = failure != 3;
      fake.interface_supported = failure != 4;
      GuideInput input;
      require(!input.running() && input.drain().empty(), "initial state must be inert");
      require(!input.start(), "missing runtime/API must return false");
      require(!input.running() && input.drain().empty(), "failure must leave adapter inert");
      input.stop(); input.stop();
      if (failure != 0) wait_for([&] { return fake.unloaded.load(); });
      require(!fake.invalid_interface_slot, "unsupported interface must not access v0-specific slots");
    }
    Runtime fake; runtime = &fake;
    GuideInput input;
    const auto steady_before = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    require(input.start() && input.running(), "start failed");
    const auto steady_after = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    fake.edge(true, fake.current_timestamp);
    const auto anchored = input.drain();
    require(anchored.size() == 1 && anchored[0].timestamp_us == fake.current_timestamp &&
            anchored[0].steady_timestamp_us >= static_cast<uint64_t>(steady_before) &&
            anchored[0].steady_timestamp_us <= static_cast<uint64_t>(steady_after),
            "callback timestamp must normalize to the captured steady clock anchor");
    fake.current_timestamp = 999999;
    fake.edge(false, 1000);
    const auto unchanged_anchor = input.drain();
    require(unchanged_anchor.size() == 1 && unchanged_anchor[0].timestamp_us == 1000 &&
            unchanged_anchor[0].steady_timestamp_us == anchored[0].steady_timestamp_us &&
            !fake.timestamp_was_read,
            "callback must use the immutable registration anchor without querying runtime time");
    require(fake.policy == GameInputExclusiveForegroundGuideButton, "only supported v0 foreground Guide policy is allowed");
    require(input.start() && fake.loads == 1, "start must be idempotent");
    fake.edge(true, 10); fake.edge(false, 20);
    auto events = input.drain();
    require(events.size() == 2 && events[0].pressed && !events[1].pressed && events[0].timestamp_us == 10 && events[1].timestamp_us == 20, "edges lost order or timestamps");
    require(events[1].steady_timestamp_us - events[0].steady_timestamp_us == 10, "callback normalization must preserve timestamp deltas");
    require(input.drain().empty(), "drain must consume events");
    fake.callback(42, fake.context, nullptr, 25, GameInputSystemButtonGuide, GameInputSystemButtonGuide);
    require(input.drain().empty(), "unchanged state must not emit an edge");
    for (uint64_t index = 0; index < 10000; ++index) fake.edge((index % 2) == 0, index);
    events = input.drain();
    require(!events.empty() && events.size() <= 64 && events.back().timestamp_us == 9999 && !events.back().pressed, "overflow must bound memory and preserve latest release");
    fake.pending = std::thread([&] { while (!fake.invoke_pending) std::this_thread::yield(); fake.edge(true, 10001); });
    input.stop();
    require(!input.running() && input.drain().empty(), "stop must discard pending input including in-flight callbacks");
    wait_for([&] { return fake.unloaded.load(); });
    require(fake.stopped && fake.unregistered && fake.unloaded && fake.references == 0, "stop did not release runtime");
    input.stop();
    wait_for([&] { return fake.unloaded.load(); });
    fake.callback = nullptr; fake.invoke_pending = false;
    wait_for([&] { return input.start(); });
    require(input.drain().empty(), "restart must not replay prior session input");
    input.stop();
    fake.callback = nullptr;
    wait_for([&] { return input.start(); });
    fake.unloaded = false; fake.released = false;
    fake.unregister_succeeds = false;
    input.stop();
    require(!input.running() && input.drain().empty(), "failed unregister must disable delivery");
    require(!fake.unloaded && !fake.released && !input.start(), "unconfirmed callback must retain DLL/context and prevent restart");
    fake.unregister_succeeds = true;
    input.stop();
    wait_for([&] { return fake.unloaded.load(); });
    require(fake.unloaded && fake.references == 0, "stop retry failed to release registration");
    fake.callback = nullptr;
    wait_for([&] { return input.start(); });
    fake.release_permitted = false;
    const auto stop_begin = std::chrono::steady_clock::now();
    input.stop();
    require(std::chrono::steady_clock::now() - stop_begin < std::chrono::milliseconds(100), "slow Release must not block UI stop");
    require(!fake.unloaded && !input.start(), "pending cleanup must retain DLL and prevent a second runtime instance");
    GuideInput another;
    require(!another.start(), "another adapter must not initialize during pending runtime cleanup");
    fake.release_permitted = true;
    wait_for([&] { return fake.unloaded.load(); });
    std::cout << "Guide adapter absence, v0 policy, bounded queue, callback teardown and restart passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
