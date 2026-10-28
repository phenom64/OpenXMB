#pragma once
#include <atomic>
#include <thread>

// Deterministic scheduling barriers, compiled only into the video test target.
namespace openxmb::video::pause_test {
enum class point : unsigned { none, before_lock, inside_lock };
inline std::atomic<unsigned> armed{0}, completed{0};
inline std::atomic<bool> entered{false}, released{true};
inline std::atomic<bool> probe_pause_lock{false}, pause_attempted{false}, pause_contended{false};
inline void arm(point where) {
    released.store(false);
    entered.store(false);
    armed.store(static_cast<unsigned>(where));
}
inline void arrive(point where) {
    auto expected=static_cast<unsigned>(where);
    if(!armed.compare_exchange_strong(expected,0)) return;
    entered.store(true,std::memory_order_release);
    while(!released.load(std::memory_order_acquire)) std::this_thread::yield();
}
struct callback_scope {
    ~callback_scope() { completed.fetch_add(1,std::memory_order_release); }
};
}
