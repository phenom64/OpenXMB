#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>
#include "openxmb/integrations/desktop.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <cstdlib>

namespace {
std::vector<std::string> checks,captures;
void require(bool value,const std::string& message) {if(!value) throw std::runtime_error(message);}
void pump() {MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&message);DispatchMessageW(&message);}}
void focus_fixture(HWND parent) {
  // Background CI/agent launches may not receive SetForegroundWindow rights.
  // Click only our own verified native client area to make it the active app.
  SetWindowPos(parent,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
  POINT point{10,10};ClientToScreen(parent,&point);
  const auto target=WindowFromPoint(point);
  require(target==parent||IsChild(parent,target),"test parent is obscured; cannot acquire safe input focus");
  SetCursorPos(point.x,point.y);
  INPUT events[2]{};events[0].type=events[1].type=INPUT_MOUSE;
  events[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;events[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
  SendInput(2,events,sizeof(INPUT));
  pump();
  SetWindowPos(parent,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}
template<class Predicate> void wait(Predicate predicate,const char* label) {
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(25);
  do {pump();if(predicate()) {checks.push_back(label);std::cout<<"PASS "<<label<<std::endl;return;}std::this_thread::sleep_for(std::chrono::milliseconds(20));}while(std::chrono::steady_clock::now()<deadline);
  throw std::runtime_error(std::string("Timed out: ")+label);
}
void settle() {const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(400);while(std::chrono::steady_clock::now()<until){pump();std::this_thread::sleep_for(std::chrono::milliseconds(20));}}
void capture(HWND child,const char* name) {
  const auto* output=std::getenv("OPENXMB_BROWSER_SMOKE_OUTPUT");if(!output||!*output)return;
  const auto directory=std::filesystem::path(output);std::filesystem::create_directories(directory);
  RECT rect{};GetWindowRect(child,&rect);const int width=rect.right-rect.left,height=rect.bottom-rect.top;
  const auto screen=GetDC(nullptr),memory=CreateCompatibleDC(screen);
  const auto bitmap=CreateCompatibleBitmap(screen,width,height);const auto previous=SelectObject(memory,bitmap);
  require(BitBlt(memory,0,0,width,height,screen,rect.left,rect.top,SRCCOPY|CAPTUREBLT)!=FALSE,"Cannot capture native fixture");
  Gdiplus::GdiplusStartupInput startup;ULONG_PTR token{};require(Gdiplus::GdiplusStartup(&token,&startup,nullptr)==Gdiplus::Ok,"Cannot initialize screenshot encoder");
  bool saved{};
  {const CLSID png{0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0,0,0xf8,0x1e,0xf3,0x2e}};Gdiplus::Bitmap image(bitmap,nullptr);saved=image.Save((directory/name).c_str(),&png,nullptr)==Gdiplus::Ok;}
  Gdiplus::GdiplusShutdown(token);SelectObject(memory,previous);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(nullptr,screen);
  require(saved,"Cannot save native fixture screenshot");captures.push_back(name);
}
}
int main(int argc,char** argv) {
  using namespace openxmb::integrations;
  if(argc<2||argc>3) return 2;
  const std::string origin=argv[1];
  const bool interactive=argc==3&&std::string_view(argv[2])=="--interactive-input";
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const auto root=std::filesystem::temp_directory_path()/("openxmb-webview2-test-"+std::to_string(GetCurrentProcessId()));
  std::filesystem::create_directories(root);
  WNDCLASSW klass{};klass.lpfnWndProc=DefWindowProcW;klass.hInstance=GetModuleHandleW(nullptr);klass.lpszClassName=L"OpenXMB.Browser.TestParent";
  RegisterClassW(&klass);
  RECT outer{0,0,1920,1080};AdjustWindowRect(&outer,WS_OVERLAPPEDWINDOW,FALSE);
  HWND parent=CreateWindowW(klass.lpszClassName,L"OpenXMB browser runtime test",WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,0,outer.right-outer.left,outer.bottom-outer.top,nullptr,nullptr,klass.hInstance,nullptr);
  BrowserSession session;
  try {
    require(parent!=nullptr,"Could not create test parent");
    SetForegroundWindow(parent);SetFocus(parent);pump();
    if(interactive) focus_fixture(parent);
    require(!open_browser("javascript:alert(1)",session,root),"invalid launch URL accepted");
    require(session.process_id==-1,"invalid launch spawned a process");
    const auto opened=open_browser(origin+"/first",session,root/"profile");require(bool(opened),opened.message);
    require(session.embedded_view&&session.dedicated_window&&session.native_process,"launch did not retain embedded process ownership");
    const auto pid=session.process_id;
    require(!open_browser(origin+"/second",session,root/"profile"),"duplicate live browser launch accepted");
    wait([&] {const auto page=browser_page(session);if(!page.error.empty()) throw std::runtime_error(page.error);return page.ready&&page.title=="First page"&&!page.loading;},"real page loaded through open_browser");
    std::ifstream state_file(session.state_file);const auto state=nlohmann::json::parse(state_file);
    state_file.close();
    const auto child=reinterpret_cast<HWND>(state.at("window").get<std::uintptr_t>());
    require(IsWindow(child)&&GetParent(child)==parent&&(GetWindowLongPtrW(child,GWL_STYLE)&WS_CHILD),"WebView host is not a native child of the shell");
    auto geometry=[&](int x,int y,int width,int height) {
      RECT bounds{};GetWindowRect(child,&bounds);POINT point{bounds.left,bounds.top};ScreenToClient(parent,&point);
      return point.x==x&&point.y==y&&bounds.right-bounds.left==width&&bounds.bottom-bounds.top==height;
    };
    RECT actual_parent{},actual_child{};GetClientRect(parent,&actual_parent);GetWindowRect(child,&actual_child);
    std::cout<<"Parent client: "<<actual_parent.right<<"x"<<actual_parent.bottom<<"; child: "<<actual_child.right-actual_child.left<<"x"<<actual_child.bottom-actual_child.top<<std::endl;
    wait([&] {return geometry(414,76,1094,928);},"shell viewport geometry");
    wait([&] {
      POINT point{300,300};ClientToScreen(child,&point);const auto dc=GetDC(nullptr);const auto color=GetPixel(dc,point.x,point.y);ReleaseDC(nullptr,dc);
      return abs(GetRValue(color)-42)<10&&abs(GetGValue(color)-199)<10&&abs(GetBValue(color)-143)<10;
    },"actual WebView painted page pixels");
    auto command=[&](const char* name,const std::string& value=std::string{}) {const auto result=browser_view_command(session,name,value);require(bool(result),result.message);};
    const auto editable_page=[&](const char* kind) {
      if(interactive) focus_fixture(parent);
      command("navigate",origin+"/osk?kind="+kind);
      if(interactive) command("show");
    };
    command("hide");command("return");
    wait([&] {return !IsWindowVisible(child);},"hide and queued shell return");
    command("show");wait([&] {return IsWindowVisible(child);},"show child again");
    command("text","Unicode \xe2\x9c\x93 \"quote\"");
    wait([&] {return browser_page(session).title=="Typed:Unicode \xe2\x9c\x93 \"quote\"";},"UTF-8 text reaches DOM");
    command("navigate",origin+"/second");wait([&] {return browser_page(session).title=="Second page"&&browser_page(session).back;},"navigation/title/history state");
    command("back");wait([&] {return browser_page(session).url.ends_with("/first")&&!browser_page(session).loading;},"back history");
    command("forward");wait([&] {return browser_page(session).url.ends_with("/second")&&!browser_page(session).loading;},"forward history");
    command("navigate",origin+"/first");wait([&] {return browser_page(session).title=="First page"&&!browser_page(session).loading;},"automatic popup stays suppressed");
    if(interactive) {
    focus_fixture(parent);command("show");
    wait([&] {return GetAncestor(GetForegroundWindow(),GA_ROOT)==parent;},"owned test parent has keyboard focus");
    const double dpi_scale=GetDpiForWindow(child)/96.0;
    std::cout<<"Pointer fixture DPI scale: "<<dpi_scale<<"; foreground root matches: "<<(GetAncestor(GetForegroundWindow(),GA_ROOT)==parent)<<std::endl;
    require(bool(browser_input(session,{BrowserControl::pointer,30.0*dpi_scale/1094,78.0*dpi_scale/928,{}})),"embedded pointer command failed");
    wait([&] {POINT point{};GetCursorPos(&point);ScreenToClient(child,&point);return abs(point.x-30*dpi_scale)<3&&abs(point.y-78*dpi_scale)<3;},"controller pointer reaches link coordinates");
    wait([&] {return browser_page(session).hover.ends_with("/popup");},"native hover state and controller pointer");
    require(bool(browser_input(session,{BrowserControl::click})),"embedded click command failed");
    wait([&] {return browser_page(session).title=="Popup page"&&!browser_page(session).loading;},"user popup navigates inside same child");
    require(GetParent(child)==parent,"popup detached from shell parent");
    }
    command("configure",nlohmann::json{{"zoom",125},{"downloads",(root/"downloads").string()}}.dump());
    wait([&] {std::ifstream file(session.state_file);return nlohmann::json::parse(file).value("zoom",0.0)==125;},"configured zoom");
    command("navigate",origin+"/download");
    wait([&] {std::ifstream file(root/"downloads"/"fixture.txt");const std::string content((std::istreambuf_iterator<char>(file)),{});return content=="OpenXMB WebView2 download fixture\n";},"download completes in configured folder");
    command("clear_site_data","runtime-test-token");
    wait([&] {return browser_page(session).status=="Browser site data cleared.";},"site-data clearing completed");
    require(std::filesystem::exists(root/"profile"/"cleared-site-data-token"),"site-data request token not persisted");
    command("address");wait([&] {return browser_page(session).address_requested;},"address request protocol");
    command("show");wait([&] {return !browser_page(session).address_requested;},"address request acknowledged");
    command("configure",R"({"zoom":100,"downloads":"","options_scancode":4})");
    command("navigate",origin+"/options?code=KeyA");
    wait([&] {return browser_page(session).options_requested;},"remapped Options key requests shell menu");
    command("menu");wait([&] {return !IsWindowEnabled(child)&&!browser_page(session).options_requested;},"shell menu suspends native page input and acknowledges request");
    command("show");wait([&] {return IsWindowEnabled(child);},"closing menu resumes native page input");
    command("configure",R"({"zoom":100,"osk":true})");
    editable_page("input");
    wait([&] {return browser_page(session).edit_requested&&browser_page(session).edit_value=="Existing value";},"focused textbox requests shell OSK");
    const auto input_token=browser_page(session).edit_token;
    settle();require(browser_page(session).edit_token==input_token,"one focus produced multiple OSK requests");
    capture(child,"01-editable-focused.png");
    command("hide");command("return");
    command("edit",nlohmann::json{{"token",input_token},{"accepted",true},{"text","Accepted Unicode \xe2\x9c\x93"}}.dump());command("show");
    wait([&] {return browser_page(session).title=="Value:Accepted Unicode \xe2\x9c\x93 |inputs:1"&&!browser_page(session).edit_requested;},"OSK acceptance replaces original target and dispatches input");
    settle();command("show");settle();require(browser_page(session).edit_token==input_token&&!browser_page(session).edit_requested,"restoring focus reopened OSK");
    capture(child,"02-edit-accepted.png");
    editable_page("textarea");
    wait([&] {return browser_page(session).edit_requested&&browser_page(session).edit_token>input_token&&browser_page(session).edit_multiline;},"textarea requests multiline OSK");
    const auto cancel_token=browser_page(session).edit_token;
    command("hide");command("edit",nlohmann::json{{"token",cancel_token},{"accepted",false}}.dump());command("show");settle();
    require(browser_page(session).title=="Value:Existing value |inputs:0"&&!browser_page(session).edit_requested&&browser_page(session).edit_token==cancel_token,"OSK cancellation changed value or reopened editor");
    capture(child,"03-edit-cancelled.png");
    editable_page("contenteditable");
    wait([&] {return browser_page(session).edit_requested&&browser_page(session).edit_token>cancel_token;},"contenteditable requests OSK");
    const auto content_token=browser_page(session).edit_token;
    command("edit",nlohmann::json{{"token",content_token},{"accepted",true},{"text","Content updated"}}.dump());command("show");
    wait([&] {return browser_page(session).title=="Value:Content updated |inputs:1";},"contenteditable target receives replacement");
    editable_page("password");
    wait([&] {return browser_page(session).edit_requested&&browser_page(session).edit_password;},"password requests empty OSK field");
    const auto password_token=browser_page(session).edit_token;
    require(browser_page(session).edit_value.empty(),"existing password serialized in page API");
    {std::ifstream file(session.state_file);const std::string raw((std::istreambuf_iterator<char>(file)),{});require(raw.find("existing-password-must-stay-in-dom")==std::string::npos,"existing password leaked into state file");}
    capture(child,"04-password-focused.png");
    command("hide");command("edit",nlohmann::json{{"token",password_token},{"accepted",false}}.dump());command("show");settle();
    require(browser_page(session).title=="Password length:34 |inputs:0"&&!browser_page(session).edit_requested,"password cancel changed existing value");
    command("configure",R"({"zoom":100,"osk":false})");editable_page("input");
    wait([&] {return browser_page(session).url.ends_with("/osk?kind=input")&&!browser_page(session).loading;},"OSK-disabled page loaded");settle();
    require(!browser_page(session).edit_requested,"input.osk Off still requested editor");
    command("configure",R"({"zoom":100,"osk":true})");editable_page("readonly");
    wait([&] {return browser_page(session).url.ends_with("/osk?kind=readonly")&&!browser_page(session).loading;},"readonly text fixture loaded");settle();
    require(!browser_page(session).edit_requested,"readonly textbox requested OSK");
    editable_page("frame");
    wait([&] {return browser_page(session).edit_requested&&browser_page(session).edit_value=="Existing value";},"cross-origin iframe requests OSK through native frame channel");
    const auto frame_token=browser_page(session).edit_token;
    command("hide");command("edit",nlohmann::json{{"token",frame_token},{"accepted",true},{"text","Frame replacement"}}.dump());command("show");
    wait([&] {return browser_page(session).title=="Frame edited |leaks:0"&&!browser_page(session).edit_requested;},"iframe writeback targets original frame without parent-page payload leaks");
    if(const auto* output=std::getenv("OPENXMB_BROWSER_SMOKE_OUTPUT");output&&*output) {
      std::ofstream(std::filesystem::path(output)/"metadata.json")<<nlohmann::json{{"checks",checks},{"captures",captures},{"parent_pid",GetCurrentProcessId()},{"helper_pid",pid},{"viewport",{{"width",1094},{"height",928}}},{"edit_request_tokens",{input_token,cancel_token,content_token,password_token}},
        {"assertions",{{"one_request_per_focus",true},{"accept_reaches_target_and_fires_input",true},{"cancel_preserves_value",true},{"show_focus_does_not_reopen",true},{"password_value_not_serialized",true},{"osk_off_respected",true},{"readonly_excluded",true}}}}.dump(2);
    }
    outer={0,0,960,540};AdjustWindowRect(&outer,WS_OVERLAPPEDWINDOW,FALSE);
    SetWindowPos(parent,nullptr,0,0,outer.right-outer.left,outer.bottom-outer.top,SWP_NOMOVE|SWP_NOZORDER);
    wait([&] {return geometry(207,38,547,464);},"parent resize follows shell geometry");
    const auto retained=session.native_process;
    const auto closed=close_browser(session);
    require(bool(closed)&&closed.message=="Browser session closed","graceful browser close failed: "+closed.message);
    DWORD exit_code{};GetExitCodeProcess(static_cast<HANDLE>(retained.get()),&exit_code);
    require(exit_code==0,"browser helper was forcibly terminated");
    require(session.process_id==-1&&!session.native_process&&!session.embedded_view,"close did not release session ownership");
    require(!IsWindow(child),"child survived close");
    require(process_exit_status(pid).has_value(),"closed helper is still running");
    SetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER",L"C:\\OpenXMB-no-such-runtime");
    SetForegroundWindow(parent);pump();
    const auto missing=open_browser(origin+"/first",session,root/"missing-runtime");
    SetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER",nullptr);
    require(bool(missing),missing.message);
    wait([&] {return !browser_page(session).error.empty();},"missing runtime exposes meaningful error");
    require(browser_page(session).error.find("Runtime")!=std::string::npos,"missing runtime error is not meaningful");
    require(bool(close_browser(session)),"error helper could not close");
    SetForegroundWindow(parent);pump();
    require(bool(open_browser(origin+"/first",session,root/"parent-close")),"restart failed");
    wait([&] {return browser_page(session).ready;},"helper restart");
    DestroyWindow(parent);parent=nullptr;
    wait([&] {const auto code=process_exit_status(session.process_id);if(code&&*code!=0) throw std::runtime_error("parent-close helper exit status "+std::to_string(*code));return code.has_value();},"parent destruction closes helper cleanly");
    session.native_process.reset();session.process_id=-1;
    std::filesystem::remove_all(root);
    std::cout<<"browser_windows_tests: all runtime checks passed\n";return 0;
  }catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    if(session.process_id>0) static_cast<void>(close_browser(session));
    if(parent) DestroyWindow(parent);
    return 1;
  }
}
