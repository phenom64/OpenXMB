#include "openxmb/integrations/session_window.hpp"
#include "openxmb/desktop/session.hpp"
#include <filesystem>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <chrono>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>
#include <chrono>
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
    const auto invalid_capture=capture_session_window(invalid,fs::temp_directory_path()/"invalid-session.png");
    require(!invalid_capture && invalid_capture.state==CaptureState::unavailable,"invalid target must fail permanently without retrying");
    std::stop_source cancelled_request;cancelled_request.request_stop();
    const auto cancelled_path=fs::temp_directory_path()/("openxmb-cancelled-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".png");
    const auto cancelled_capture=capture_session_window(invalid,cancelled_path,0,cancelled_request.get_token());
    require(!cancelled_capture&&cancelled_capture.message.find("cancelled")!=std::string::npos&&!fs::exists(cancelled_path),"cancelled capture must stop before native work or publication");
#ifdef _WIN32
    require(argc == 2, "fixture executable argument is required");
    const auto root = fs::temp_directory_path() / ("openxmb-window-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(root); const auto ready = root / "ready"; const auto image = root / "capture.png"; fs::remove(ready);
    openxmb::desktop::SessionManager manager;
    openxmb::desktop::SessionDescriptor descriptor;
    descriptor.identity="window-fixture";descriptor.title="Window Fixture";descriptor.provider="application";descriptor.runner="fixture";
    descriptor.profile_directory=root;descriptor.kind=openxmb::desktop::SessionKind::application;descriptor.tracking=openxmb::desktop::TrackingQuality::launcher_only;
    const auto helper=fs::path(argv[1]).u8string(),destination=ready.u8string();
    const auto launched=manager.launch(descriptor,{std::string(helper.begin(),helper.end()),std::string(destination.begin(),destination.end()),"15000"});
    require(bool(launched),"fixture launch failed");
    struct Guard {openxmb::desktop::SessionManager& manager;std::string id;~Guard(){static_cast<void>(manager.quit(id,ProcessStopMode::force_stop));}} guard{manager,launched.session_id};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fs::exists(ready) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(fs::exists(ready), "fixture did not become ready");
    const auto launched_entries=manager.snapshot();
    const auto launched_entry=std::find_if(launched_entries.begin(),launched_entries.end(),[&](const auto& entry){return entry.id==launched.session_id;});
    require(launched_entry!=launched_entries.end(),"fixture process record missing");
    auto target = find_session_window(launched_entry->process_id); require(bool(target), "fixture target not found");
    auto stale = *target; ++stale.process_creation;
    require(!focus_session_window(stale), "stale creation token must not focus");
    require(!capture_session_window(stale, image), "stale creation token must not capture");
    auto wrong = *target; wrong.pid = GetCurrentProcessId();
    require(!focus_session_window(wrong), "wrong PID must not focus");
    require(!capture_session_window(wrong, image), "wrong PID must not capture");
    require(!manager.confirm_application_window(launched.session_id,wrong),"A forwarding session must not adopt another process");
    require(!manager.confirm_application_window(launched.session_id,stale),"A forwarding session must not accept a stale window");
    require(bool(manager.confirm_application_window(launched.session_id,*target)),"A live child window did not confirm the application session");
    require(manager.active_session()->descriptor.tracking==openxmb::desktop::TrackingQuality::direct_child,"Confirmed application did not become directly observed");
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
      const auto read_bytes=[&]{std::ifstream file(image,std::ios::binary);return std::string(std::istreambuf_iterator<char>(file),{});};
      const auto original_png=read_bytes();
      const auto collision=capture_session_window(*target,image,reinterpret_cast<std::uintptr_t>(shell));
      require(!collision && collision.state==CaptureState::unavailable,"capture must never overwrite an existing screenshot");
      require(read_bytes()==original_png,"screenshot collision modified an existing file");
      RECT target_rect{};GetWindowRect(reinterpret_cast<HWND>(target->handle),&target_rect);
      HWND obstruction=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE,L"STATIC",L"Capture fixture occluder",WS_POPUP|SS_BLACKRECT,
        target_rect.left+20,target_rect.top+40,80,80,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
      require(obstruction!=nullptr,"capture occluder fixture failed");
      struct ObstructionGuard{HWND window;~ObstructionGuard(){DestroyWindow(window);}} obstruction_guard{obstruction};
      SetWindowPos(obstruction,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
      require(GetForegroundWindow()==reinterpret_cast<HWND>(target->handle),"passive capture fixture stole focus");
      const auto unobscured=capture_session_window(*target,root/"under-overlay.png",reinterpret_cast<std::uintptr_t>(shell));
      require(bool(unobscured),"native window capture must exclude unrelated topmost overlays");
      require(Gdiplus::GdiplusStartup(&token,&input,nullptr)==Gdiplus::Ok,"capture verification encoder unavailable");
      {
        Gdiplus::Bitmap bitmap((root/"under-overlay.png").c_str());RECT client{};POINT origin{};
        GetClientRect(reinterpret_cast<HWND>(target->handle),&client);ClientToScreen(reinterpret_cast<HWND>(target->handle),&origin);
        require(bitmap.GetWidth()==static_cast<unsigned>(client.right)&&bitmap.GetHeight()==static_cast<unsigned>(client.bottom),"native capture changed client dimensions");
        Gdiplus::Color behind;bitmap.GetPixel(target_rect.left+40-origin.x,target_rect.top+60-origin.y,&behind);
        require(behind.GetRed()>180&&behind.GetGreen()<70&&behind.GetAlpha()==255,"native capture included the black occluder or lost alpha");
      }
      Gdiplus::GdiplusShutdown(token);
      std::cout << "Windows session window identity, focus and screenshot pixel checks passed\n";
    } else std::cout << "Foreground denied; visual capture checks skipped\n";
    DestroyWindow(shell); PostMessageW(reinterpret_cast<HWND>(target->handle), WM_CLOSE, 0, 0);
    const auto exit_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(manager.active_session()&&std::chrono::steady_clock::now()<exit_deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(!manager.active_session(),"Window fixture did not exit");
    static_cast<void>(manager.shutdown());fs::remove_all(root);
#else
    (void)argc; (void)argv;
    require(!find_session_window(1), "unsupported platform must not resolve a target");
#endif
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}


