// Native Windows Graphics Capture checks against a child-process fixture window. Never takes focus.
// The fixture and occluder are non-activating; the regression of interest is a process crash after
// capture teardown (GraphicsCapture.dll unloaded under its own worker thread), so the test keeps
// running after the last capture long enough for that worker to exit.
#include "../../src/integrations/windows_capture.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
using namespace openxmb::integrations;
namespace {
void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
constexpr wchar_t fixture_class[]=L"OpenXMBWindowCaptureFixture";
constexpr COLORREF red=RGB(220,20,20),green=RGB(20,220,20);
LRESULT CALLBACK fixture_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
  if(message==WM_PAINT) {
    PAINTSTRUCT paint;HDC dc=BeginPaint(window,&paint);RECT client;GetClientRect(window,&client);
    RECT left=client,right=client;left.right=right.left=client.right/2;
    HBRUSH left_brush=CreateSolidBrush(red),right_brush=CreateSolidBrush(green);
    FillRect(dc,&left,left_brush);FillRect(dc,&right,right_brush);DeleteObject(left_brush);DeleteObject(right_brush);
    EndPaint(window,&paint);return 0;
  }
  if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
  return DefWindowProcW(window,message,wparam,lparam);
}
RECT fixture_rect() {RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);return {work.right-360,work.bottom-240,work.right-40,work.bottom-40};}
int run_fixture() {
  WNDCLASSW type{};type.lpfnWndProc=fixture_proc;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=fixture_class;RegisterClassW(&type);
  const auto rect=fixture_rect();
  // WS_BORDER makes the client area differ from the captured window bounds, so a wrong crop shows.
  HWND window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,fixture_class,L"Window capture fixture",WS_POPUP|WS_BORDER,
    rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,type.hInstance,nullptr);
  if(!window)return 2;
  ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);
  const auto deadline=GetTickCount64()+30000;MSG message;
  while(GetTickCount64()<deadline) {
    if(MsgWaitForMultipleObjects(0,nullptr,FALSE,100,QS_ALLINPUT)!=WAIT_OBJECT_0)continue;
    while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message==WM_QUIT)return 0;DispatchMessageW(&message);}
  }
  return 0;
}
void pump_for(std::chrono::milliseconds duration) {
  const auto end=std::chrono::steady_clock::now()+duration;MSG message;
  while(std::chrono::steady_clock::now()<end){while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE))DispatchMessageW(&message);Sleep(10);}
}
windows_capture::Frame on_thread(HWND window,SessionWindowBounds bounds,DWORD apartment=~DWORD{}) {
  windows_capture::Frame frame;std::atomic<bool> done{};
  std::thread worker([&]{
    const bool com=apartment!=~DWORD{}&&SUCCEEDED(CoInitializeEx(nullptr,apartment));
    frame=windows_capture::acquire(reinterpret_cast<std::uintptr_t>(window),bounds);
    if(com)CoUninitialize();
    done=true;
  });
  while(!done)pump_for(std::chrono::milliseconds(5));
  worker.join();return frame;
}
void require_fixture_pixels(const windows_capture::Frame& frame,SessionWindowBounds bounds,const char* label) {
  require(frame.supported&&frame.result.state==CaptureState::saved,std::string(label)+": "+frame.result.message);
  require(frame.bgra.size()==static_cast<std::size_t>(bounds.width)*bounds.height*4,std::string(label)+": frame is not the client size");
  const auto pixel=[&](unsigned x,unsigned y){const auto* p=frame.bgra.data()+(static_cast<std::size_t>(y)*bounds.width+x)*4;return RGB(p[2],p[1],p[0])|(static_cast<DWORD>(p[3])<<24);};
  const auto opaque=[](DWORD color,COLORREF expected){return color==(expected|0xff000000u);};
  const unsigned w=bounds.width,h=bounds.height;
  // Exact client crop: every corner is fixture paint, never the window border or desktop.
  require(opaque(pixel(0,0),red)&&opaque(pixel(0,h-1),red),std::string(label)+": left client edge is not fixture red");
  require(opaque(pixel(w-1,0),green)&&opaque(pixel(w-1,h-1),green),std::string(label)+": right client edge is not fixture green");
  require(opaque(pixel(w/4,h/2),red)&&opaque(pixel(w*3/4,h/2),green),std::string(label)+": fixture interior pixels differ");
}
}

int main(int argc,char** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if(argc>1&&std::strcmp(argv[1],"fixture")==0)return run_fixture();
  PROCESS_INFORMATION child{};
  try {
    const auto unsupported=windows_capture::acquire(0,{0,0,1,1});
    if(!unsupported.supported){std::cout<<"Windows Graphics Capture unavailable; native capture checks skipped\n";return 0;}
    require(unsupported.result.state==CaptureState::unavailable&&unsupported.bgra.empty(),"a null window must fail without pixels");
    wchar_t self[MAX_PATH];require(GetModuleFileNameW(nullptr,self,MAX_PATH)>0,"own path unavailable");
    std::wstring command=L"\""+std::wstring(self)+L"\" fixture";STARTUPINFOW startup{};startup.cb=sizeof(startup);
    require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child),"fixture process failed to start");
    HWND window{};
    for(int attempt=0;attempt<500&&!window;++attempt) {
      Sleep(10);
      for(HWND candidate=nullptr;(candidate=FindWindowExW(nullptr,candidate,fixture_class,nullptr));) {
        DWORD pid{};GetWindowThreadProcessId(candidate,&pid);if(pid==child.dwProcessId){window=candidate;break;}
      }
    }
    require(window!=nullptr,"fixture window did not appear");
    pump_for(std::chrono::milliseconds(300));
    RECT client{};POINT origin{};require(GetClientRect(window,&client)&&ClientToScreen(window,&origin),"fixture geometry unavailable");
    const SessionWindowBounds bounds{origin.x,origin.y,static_cast<unsigned>(client.right),static_cast<unsigned>(client.bottom)};
    // COM-uninitialized worker (the shell's AsyncJob shape), repeated, then an explicit MTA worker.
    for(int round=0;round<3;++round)require_fixture_pixels(on_thread(window,bounds),bounds,"uninitialized worker");
    require_fixture_pixels(on_thread(window,bounds,COINIT_MULTITHREADED),bounds,"MTA worker");
    // A topmost window over the fixture must not appear in the native capture.
    const auto fixture=fixture_rect();
    HWND occluder=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,L"STATIC",L"Capture occluder",WS_POPUP|SS_BLACKRECT,
      fixture.left+20,fixture.top+20,fixture.right-fixture.left-40,fixture.bottom-fixture.top-40,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    require(occluder!=nullptr,"occluder fixture failed");
    ShowWindow(occluder,SW_SHOWNOACTIVATE);pump_for(std::chrono::milliseconds(200));
    const auto covered=on_thread(window,bounds);
    DestroyWindow(occluder);
    require_fixture_pixels(covered,bounds,"occluded fixture");
    // Bounds that do not match the window's client area must not produce a guessed crop.
    const auto shifted=on_thread(window,{bounds.x-4000,bounds.y,bounds.width,bounds.height});
    require(shifted.supported&&shifted.result.state==CaptureState::pending&&shifted.bgra.empty(),"misaligned client bounds must not be cropped");
    // STA callers are rejected explicitly instead of loading capture DLLs into a caller-owned apartment.
    const auto sta=on_thread(window,bounds,COINIT_APARTMENTTHREADED);
    require(sta.supported&&sta.result.state==CaptureState::unavailable&&sta.bgra.empty(),"STA capture must fail explicitly");
    // GraphicsCapture's worker thread exits after its session closes; the process must survive it.
    pump_for(std::chrono::milliseconds(2500));
    PostMessageW(window,WM_CLOSE,0,0);
    require(WaitForSingleObject(child.hProcess,5000)==WAIT_OBJECT_0,"fixture process did not exit");
    CloseHandle(child.hProcess);CloseHandle(child.hThread);
    std::cout<<"Windows native window capture checks passed\n";
    return 0;
  } catch(const std::exception& error) {
    if(child.hProcess){TerminateProcess(child.hProcess,1);CloseHandle(child.hProcess);CloseHandle(child.hThread);}
    std::cerr<<error.what()<<'\n';return 1;
  }
}
