#include "openxmb/integrations/guide_input.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

int main() {
  using openxmb::integrations::GuideInput;
  try {
    for (int iteration = 0; iteration < 3; ++iteration) {
      GuideInput input;
      if (input.running() || !input.drain().empty()) throw std::runtime_error("initial adapter not inert");
      const bool started = input.start();
      if (started != input.running()) throw std::runtime_error("start/running mismatch");
      if (started && !input.start()) throw std::runtime_error("repeated start failed");
      if (started) {
        GuideInput competing;
        if (competing.start()) throw std::runtime_error("two adapters acquired the process runtime");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      const auto stop_begin = std::chrono::steady_clock::now();
      input.stop(); input.stop();
      if (std::chrono::steady_clock::now() - stop_begin > std::chrono::milliseconds(250))
        throw std::runtime_error("stop blocked the UI thread for more than 250ms");
      if (input.running() || !input.drain().empty()) throw std::runtime_error("stop retained state");
      std::cout << "iteration " << iteration << ": " << (started ? "runtime registered and stopped" : "runtime unavailable, fallback retained") << std::endl;
#ifdef _WIN32
      const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      while (GetModuleHandleW(L"GameInput.dll") && std::chrono::steady_clock::now() < cleanup_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      if (GetModuleHandleW(L"GameInput.dll")) throw std::runtime_error("runtime DLL remained loaded after cleanup deadline");
      std::cout << "Deferred COM release and runtime DLL unload completed" << std::endl;
#endif
    }
    std::cout << "No focus changes or controller input generated; physical Guide delivery untested\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
