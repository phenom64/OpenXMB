#include "openxmb/integrations/session_window.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>
#include <chrono>
#include <fstream>
#include <thread>
#endif
using namespace openxmb::integrations;
namespace fs = std::filesystem;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char** argv) {
  try {
    require(!find_session_window(-1), "invalid PID must not resolve");
    const SessionWindowTarget invalid{1, 1, 1};
    require(!focus_session_window(invalid), "invalid target must not focus");
    require(!capture_session_window(invalid, fs::temp_directory_path() / "invalid-session.png"), "invalid target must not capture");
#ifdef _WIN32
    require(argc == 2, "fixture executable argument is required");
    const auto root = fs::temp_directory_path() / ("openxmb-window-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(root); const auto ready = root / "ready"; const auto image = root / "capture.png"; fs::remove(ready);
    std::wstring command = L"\"" + fs::path(argv[1]).wstring() + L"\" \"" + ready.wstring() + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != FALSE, "fixture launch failed");
    struct Guard { PROCESS_INFORMATION p; ~Guard() { TerminateProcess(p.hProcess, 0); CloseHandle(p.hThread); CloseHandle(p.hProcess); } } guard{pi};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fs::exists(ready) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(fs::exists(ready), "fixture did not become ready");
    auto target = find_session_window(pi.dwProcessId); require(bool(target), "fixture target not found");
    auto stale = *target; ++stale.process_creation;
    require(!focus_session_window(stale), "stale creation token must not focus");
    require(!capture_session_window(stale, image), "stale creation token must not capture");
    auto wrong = *target; wrong.pid = GetCurrentProcessId();
    require(!focus_session_window(wrong), "wrong PID must not focus");
    require(!capture_session_window(wrong, image), "wrong PID must not capture");
    HWND shell = CreateWindowW(L"STATIC", L"Hidden fixture shell", WS_OVERLAPPED, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(shell != nullptr, "hidden fixture shell creation failed");
    require(!capture_session_window(*target, image), "missing shell handle must fail closed");
    if (focus_session_window(*target)) {
      const auto captured = capture_session_window(*target, image, reinterpret_cast<std::uintptr_t>(shell));
      require(bool(captured), captured.message.c_str());
      Gdiplus::GdiplusStartupInput input; ULONG_PTR token{}; require(Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok, "GDI+ startup failed");
      { Gdiplus::Bitmap bitmap(image.c_str()); require(bitmap.GetLastStatus() == Gdiplus::Ok, "captured image unreadable");
        RECT client{}; GetClientRect(reinterpret_cast<HWND>(target->handle), &client);
        require(bitmap.GetWidth() == static_cast<UINT>(client.right) && bitmap.GetHeight() == static_cast<UINT>(client.bottom), "capture must use client dimensions");
        Gdiplus::Color left, right; bitmap.GetPixel(bitmap.GetWidth()/4, bitmap.GetHeight()/2, &left); bitmap.GetPixel(bitmap.GetWidth()*3/4, bitmap.GetHeight()/2, &right);
        require(left.GetRed() > left.GetGreen() && right.GetGreen() > right.GetRed(), "capture did not contain fixture pixels"); }
      Gdiplus::GdiplusShutdown(token);
      std::cout << "Windows session window identity, focus and screenshot pixel checks passed\n";
    } else std::cout << "Foreground denied; visual capture checks skipped\n";
    DestroyWindow(shell); PostMessageW(reinterpret_cast<HWND>(target->handle), WM_CLOSE, 0, 0); fs::remove_all(root);
#else
    (void)argc; (void)argv;
    require(!find_session_window(1), "unsupported platform must not resolve a target");
#endif
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}


