// Process-isolated Windows child surface for OpenXMB's browser protocol.
// WebView2 and all its callbacks live on this STA thread; no ATL is required.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <wrl.h>
#include <WebView2.h>
#include "openxmb/integrations/desktop.hpp"
#include <nlohmann/json.hpp>
#include "browser_ipc_win.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using openxmb::integrations::BrowserControl;
namespace {
constexpr UINT close_message=WM_APP+1, popup_message=WM_APP+2, action_message=WM_APP+3, finish_message=WM_APP+4;
std::string utf8(const wchar_t* text) {
  if(!text||!*text) return {};
  const int count=WideCharToMultiByte(CP_UTF8,0,text,-1,nullptr,0,nullptr,nullptr);
  std::string result(count,'\0');
  WideCharToMultiByte(CP_UTF8,0,text,-1,result.data(),count,nullptr,nullptr);
  result.pop_back();return result;
}
std::wstring wide(const std::string& text) {
  const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
  std::wstring result(count,L'\0');
  MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),count);
  return result;
}
bool web_uri(const std::string& uri) {
  return bool(openxmb::integrations::validate_browser_url(uri));
}
std::string take(LPWSTR value) {const auto result=utf8(value);CoTaskMemFree(value);return result;}

struct Browser {
  HWND parent{},host{},dispatcher{};
  HANDLE parent_process{};
  DWORD parent_pid{};
  std::filesystem::path state_path,profile;
  ComPtr<ICoreWebView2Environment> environment;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> view;
  std::string uri,title{"Opening Internet Browser"},hover,error,popup,status,clear_token;
  std::filesystem::path downloads;
  double zoom{1.0};
  bool clearing{},pending_clear{};
  std::int64_t sequence{};
  bool loading{true},address_requested{},back{},forward{},visible{true},closing{},ready{};
  bool waiting_for_runtime{},runtime_exited{},finished{};
  EventRegistrationToken runtime_exit{};
  RECT last_bounds{-1,-1,-1,-1};
  POINT last_origin{-1,-1};
  EventRegistrationToken accelerator{};

  void publish() const {
    const nlohmann::json state{{"uri",uri},{"title",title},{"hover",hover},{"loading",loading},
      {"address_requested",address_requested},{"back",back},{"forward",forward},{"error",error},
      {"ready",ready},{"command_sequence",sequence},{"window",reinterpret_cast<std::uintptr_t>(host)},
      {"status",status},{"clearing_site_data",clearing},{"site_data_token",clear_token},{"zoom",zoom*100}};
    auto pending=state_path;pending+=L".tmp";
    std::ofstream file(pending,std::ios::binary|std::ios::trunc);
    file<<state.dump(-1,' ',false,nlohmann::json::error_handler_t::replace);file.close();
    if(file) openxmb::integrations::detail::replace_browser_file(pending,state_path);
  }
  void fail(const std::string& operation,HRESULT code) {
    char hex[20]{};snprintf(hex,sizeof(hex),"0x%08lX",static_cast<unsigned long>(code));
    error=operation+" ("+hex+"). ";
    if(code==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
      error+="Install Microsoft Edge WebView2 Evergreen Runtime, then reopen Internet Browser.";
    else if(code==E_ACCESSDENIED) error+="Check that the browser profile directory is writable.";
    else error+="Close Internet Browser and reopen it. Check the WebView2 Runtime and browser profile if this continues.";
    title="Internet Browser: "+error;loading=false;ready=false;
    if(controller) controller->put_IsVisible(FALSE);
    InvalidateRect(host,nullptr,TRUE);publish();
  }
  void return_focus() {
    if(!IsWindow(parent)) return;
    // Focus belongs to a different UI thread. Attach only for this transfer.
    const auto thread=GetWindowThreadProcessId(parent,nullptr);
    const auto current=GetCurrentThreadId();
    const bool attached=thread!=current&&AttachThreadInput(current,thread,TRUE);
    SetForegroundWindow(parent);SetFocus(parent);
    if(attached) AttachThreadInput(current,thread,FALSE);
  }
  void reveal() {
    visible=true;ShowWindow(host,SW_SHOWNA);
    if(controller&&error.empty()) {
      controller->put_IsVisible(TRUE);
      // Do not steal focus while another application is foreground.
      if(GetAncestor(GetForegroundWindow(),GA_ROOT)==GetAncestor(parent,GA_ROOT)) {
        SetFocus(host);controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
      }
    }
  }
  void resize(bool force=false) {
    RECT bounds{};GetClientRect(parent,&bounds);
    const double scale=std::min(bounds.right/1920.0,bounds.bottom/1080.0);
    const int x=static_cast<int>((bounds.right-1920*scale)/2)+static_cast<int>(414*scale);
    const int y=static_cast<int>((bounds.bottom-1080*scale)/2)+static_cast<int>(76*scale);
    const int width=std::max(1,static_cast<int>(1094*scale)),height=std::max(1,static_cast<int>(928*scale));
    const RECT next{x,y,x+width,y+height};
    if(force||!EqualRect(&next,&last_bounds)) {
      SetWindowPos(host,HWND_TOP,x,y,width,height,SWP_NOACTIVATE);
      if(controller) {RECT client{0,0,width,height};controller->put_Bounds(client);}
      last_bounds=next;
    }
    POINT origin{};ClientToScreen(parent,&origin);
    if(controller&&(force||origin.x!=last_origin.x||origin.y!=last_origin.y)) controller->NotifyParentWindowPositionChanged();
    last_origin=origin;
  }
  void navigate(const std::string& value) {
    if(!web_uri(value)) return;
    uri=value;
    if(!view) return;
    error.clear();hover.clear();uri=value;
    const auto result=view->Navigate(wide(value).c_str());
    if(FAILED(result)) fail("Unable to navigate",result);
  }
  void configure(const std::string& encoded) {
    const auto settings=nlohmann::json::parse(encoded);
    const double percent=settings.value("zoom",100.0);
    if(!std::isfinite(percent)||percent<25||percent>500) {status="Browser zoom must be between 25% and 500%.";return;}
    zoom=percent/100.0;
    if(controller) controller->put_ZoomFactor(zoom);
    const auto directory=settings.value("downloads",std::string{});
    downloads=directory.empty()?std::filesystem::path{}:std::filesystem::path(wide(directory));
    if(!downloads.empty()) {
      std::error_code ec;
      if(!downloads.is_absolute()) {status="Choose an absolute browser download folder.";downloads.clear();return;}
      std::filesystem::create_directories(downloads,ec);
      if(ec) {status="Cannot use the browser download folder: "+ec.message();downloads.clear();return;}
    }
    ComPtr<ICoreWebView2_13> latest;
    if(view&&SUCCEEDED(view.As(&latest))) {
      ComPtr<ICoreWebView2Profile> browser_profile;latest->get_Profile(&browser_profile);
      if(browser_profile&&!downloads.empty()) {
        const auto result=browser_profile->put_DefaultDownloadFolderPath(downloads.c_str());
        if(FAILED(result)) status="WebView2 could not use the selected download folder.";
      }
    }
  }
  void clear_data() {
    if(!view||clearing||!pending_clear) return;
    pending_clear=false;
    ComPtr<ICoreWebView2_13> latest;ComPtr<ICoreWebView2Profile> browser_profile;ComPtr<ICoreWebView2Profile2> data;
    if(FAILED(view.As(&latest))||FAILED(latest->get_Profile(&browser_profile))||FAILED(browser_profile.As(&data))) {
      status="Update Microsoft Edge WebView2 Runtime to clear site data.";publish();return;
    }
    clearing=true;status="Clearing browser cookies, site storage and cache…";publish();
    // The request token is persisted only after successful completion. A clear
    // requested in Settings while no browser is open runs on the next launch.
    const auto requested_token=clear_token;
    const auto result=data->ClearBrowsingData(
      static_cast<COREWEBVIEW2_BROWSING_DATA_KINDS>(COREWEBVIEW2_BROWSING_DATA_KINDS_ALL_SITE|COREWEBVIEW2_BROWSING_DATA_KINDS_DISK_CACHE),
      Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>([this,requested_token](HRESULT result)->HRESULT {
        clearing=false;
        if(SUCCEEDED(result)) {
          status="Browser site data cleared.";
          std::ofstream(profile/"cleared-site-data-token",std::ios::binary|std::ios::trunc)<<requested_token;
        } else status="WebView2 could not clear site data. Close and reopen Internet Browser to retry.";
        publish();if(pending_clear) clear_data();return S_OK;
      }).Get());
    if(FAILED(result)) {clearing=false;status="WebView2 could not start clearing site data.";publish();}
  }
  void state_changed() {
    if(!view) return;
    LPWSTR value{};
    if(SUCCEEDED(view->get_Source(&value))) uri=take(value);
    value=nullptr;if(SUCCEEDED(view->get_DocumentTitle(&value))) title=take(value);
    BOOL flag{};view->get_CanGoBack(&flag);back=flag!=FALSE;
    flag=FALSE;view->get_CanGoForward(&flag);forward=flag!=FALSE;
    publish();
  }
  void key(WORD code,WORD modifier=0) {
    if(!visible||!controller||GetAncestor(GetForegroundWindow(),GA_ROOT)!=GetAncestor(parent,GA_ROOT)) return;
    controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    std::vector<INPUT> events;
    const auto append=[&](WORD key,bool up) {INPUT event{};event.type=INPUT_KEYBOARD;event.ki.wVk=key;event.ki.dwFlags=up?KEYEVENTF_KEYUP:0;events.push_back(event);};
    if(modifier) append(modifier,false);
    append(code,false);append(code,true);
    if(modifier) append(modifier,true);
    SendInput(static_cast<UINT>(events.size()),events.data(),sizeof(INPUT));
  }
  void input(const std::string& encoded) {
    if(!visible||!controller||!error.empty()||GetAncestor(GetForegroundWindow(),GA_ROOT)!=GetAncestor(parent,GA_ROOT)) return;
    const auto data=nlohmann::json::parse(encoded);
    const auto control=static_cast<BrowserControl>(data.at("control").get<int>());
    const double x=data.value("x",0.5),y=data.value("y",0.5);
    if(!std::isfinite(x)||!std::isfinite(y)) return;
    RECT bounds{};GetClientRect(host,&bounds);
    POINT point{static_cast<LONG>(std::clamp(x,0.0,1.0)*std::max(0L,bounds.right-1)),
      static_cast<LONG>(std::clamp(y,0.0,1.0)*std::max(0L,bounds.bottom-1))};
    if(control==BrowserControl::pointer) {ClientToScreen(host,&point);SetCursorPos(point.x,point.y);return;}
    if(control==BrowserControl::tab) {key(VK_TAB);return;}
    if(control==BrowserControl::reverse_tab) {key(VK_TAB,VK_SHIFT);return;}
    if(control==BrowserControl::confirm) {key(VK_RETURN);return;}
    if(control==BrowserControl::escape) {PostMessageW(dispatcher,close_message,0,0);return;}
    // A controller click must stay inside this visible child; never click an
    // unrelated foreground window or the shell's surrounding controls.
    POINT cursor{};GetCursorPos(&cursor);ScreenToClient(host,&cursor);
    if(!PtInRect(&bounds,cursor)) return;
    controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    INPUT events[2]{};events[0].type=events[1].type=INPUT_MOUSE;
    if(control==BrowserControl::scroll) {
      events[0].mi.dwFlags=MOUSEEVENTF_WHEEL;events[0].mi.mouseData=static_cast<DWORD>(y<0?WHEEL_DELTA:-WHEEL_DELTA);
      SendInput(1,events,sizeof(INPUT));
    } else if(control==BrowserControl::click||control==BrowserControl::context_click) {
      const bool left=control==BrowserControl::click;
      events[0].mi.dwFlags=left?MOUSEEVENTF_LEFTDOWN:MOUSEEVENTF_RIGHTDOWN;
      events[1].mi.dwFlags=left?MOUSEEVENTF_LEFTUP:MOUSEEVENTF_RIGHTUP;
      SendInput(2,events,sizeof(INPUT));
    }
  }
  void command(const nlohmann::json& message) {
    const auto next=message.value("sequence",std::int64_t{});
    if(next<=sequence) return;
    sequence=next;
    const auto name=message.value("command",std::string{}),value=message.value("value",std::string{});
    if(name=="hide"||name=="show"||name=="navigate") address_requested=false;
    if(name=="close") {PostMessageW(dispatcher,close_message,0,0);return;}
    if(name=="hide") {visible=false;if(controller) controller->put_IsVisible(FALSE);ShowWindow(host,SW_HIDE);}
    else if(name=="show") reveal();
    else if(name=="return") return_focus();
    else if(name=="address") {address_requested=true;return_focus();}
    else if(name=="configure") configure(value);
    else if(name=="clear_site_data") {
      std::ifstream saved(profile/"cleared-site-data-token",std::ios::binary);
      const std::string previous((std::istreambuf_iterator<char>(saved)),{});
      if(value.empty()||value!=previous) {clear_token=value;pending_clear=true;clear_data();}
    }
    else if(name=="navigate") {navigate(value);reveal();}
    else if(view) {
      if(name=="back") view->GoBack();
      else if(name=="forward") view->GoForward();
      else if(name=="reload") {error.clear();view->Reload();reveal();}
      else if(name=="input") input(value);
      else if(name=="text") {
        // JSON encoding prevents typed text from becoming executable script.
        const auto literal=nlohmann::json(value).dump(-1,' ',true);
        const auto script="document.execCommand('insertText',false,"+literal+");";
        reveal();view->ExecuteScript(wide(script).c_str(),nullptr);
      }
    }
  }
  void tick() {
    DWORD owner{};GetWindowThreadProcessId(parent,&owner);
    if(!IsWindow(parent)||owner!=parent_pid||WaitForSingleObject(parent_process,0)!=WAIT_TIMEOUT) {
      PostMessageW(dispatcher,close_message,0,0);return;
    }
    resize();
    try {
      const auto path=state_path.parent_path()/"page-command.json";
      const auto message=openxmb::integrations::detail::read_browser_json(path);
      if(message.is_object()) {
        if(message.contains("commands")&&message.at("commands").is_array()) {
          for(const auto& item:message.at("commands")) command(item);
        } else command(message);
      }
    }catch(const nlohmann::json::exception&) {} // The next atomic write can recover malformed IPC.
    publish();
  }
  HRESULT install_events() {
    // Hold callbacks in the COM event sources; remove them before closing.
    HRESULT result=S_OK;EventRegistrationToken token{};
#define REGISTER_EVENT(kind, Interface, body) \
    result=view->add_##kind(Callback<Interface>(body).Get(),&token); \
    if(FAILED(result)) return result
    REGISTER_EVENT(NavigationStarting,ICoreWebView2NavigationStartingEventHandler,
      [this](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* args)->HRESULT {
        LPWSTR target{};args->get_Uri(&target);const auto requested=take(target);
        if(!web_uri(requested)&&requested!="about:blank") {args->put_Cancel(TRUE);error="This address cannot be opened in Internet Browser.";publish();return S_OK;}
        loading=true;error.clear();hover.clear();publish();return S_OK;
      });
    REGISTER_EVENT(NavigationCompleted,ICoreWebView2NavigationCompletedEventHandler,
      [this](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs* args)->HRESULT {
        loading=false;BOOL success{};args->get_IsSuccess(&success);
        if(!success) {COREWEBVIEW2_WEB_ERROR_STATUS status{};args->get_WebErrorStatus(&status);
          if(status!=COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) error="The page could not load (WebView2 network error "+std::to_string(status)+"). Check the address and connection, then reload.";
        }
        state_changed();return S_OK;
      });
    REGISTER_EVENT(SourceChanged,ICoreWebView2SourceChangedEventHandler,
      [this](ICoreWebView2*,ICoreWebView2SourceChangedEventArgs*)->HRESULT {hover.clear();state_changed();return S_OK;});
    REGISTER_EVENT(DocumentTitleChanged,ICoreWebView2DocumentTitleChangedEventHandler,
      [this](ICoreWebView2*,IUnknown*)->HRESULT {state_changed();return S_OK;});
    REGISTER_EVENT(HistoryChanged,ICoreWebView2HistoryChangedEventHandler,
      [this](ICoreWebView2*,IUnknown*)->HRESULT {state_changed();return S_OK;});
    REGISTER_EVENT(NewWindowRequested,ICoreWebView2NewWindowRequestedEventHandler,
      [this](ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs* args)->HRESULT {
        args->put_Handled(TRUE);BOOL user{};args->get_IsUserInitiated(&user);
        LPWSTR target{};args->get_Uri(&target);auto requested=take(target);
        // User links stay in this surface. Script-driven popups are suppressed.
        if(user&&web_uri(requested)) {popup=std::move(requested);PostMessageW(dispatcher,popup_message,0,0);}
        return S_OK;
      });
    REGISTER_EVENT(WindowCloseRequested,ICoreWebView2WindowCloseRequestedEventHandler,
      [this](ICoreWebView2*,IUnknown*)->HRESULT {PostMessageW(dispatcher,close_message,0,0);return S_OK;});
    REGISTER_EVENT(ProcessFailed,ICoreWebView2ProcessFailedEventHandler,
      [this](ICoreWebView2*,ICoreWebView2ProcessFailedEventArgs* args)->HRESULT {
        COREWEBVIEW2_PROCESS_FAILED_KIND kind{};args->get_ProcessFailedKind(&kind);
        if(kind==COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED||kind==COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED||kind==COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE)
          fail("The WebView2 browser process stopped",E_FAIL);
        return S_OK;
      });
#undef REGISTER_EVENT
    ComPtr<ICoreWebView2_12> status_view;
    if(SUCCEEDED(view.As(&status_view))) status_view->add_StatusBarTextChanged(
      Callback<ICoreWebView2StatusBarTextChangedEventHandler>([this](ICoreWebView2* sender,IUnknown*)->HRESULT {
        ComPtr<ICoreWebView2_12> source;
        if(SUCCEEDED(sender->QueryInterface(IID_PPV_ARGS(&source)))) {
          LPWSTR text{};if(SUCCEEDED(source->get_StatusBarText(&text))) hover=take(text);publish();
        }
        return S_OK;
      }).Get(),&token);
    ComPtr<ICoreWebView2_4> download_view;
    if(SUCCEEDED(view.As(&download_view))) download_view->add_DownloadStarting(
      Callback<ICoreWebView2DownloadStartingEventHandler>([this](ICoreWebView2*,ICoreWebView2DownloadStartingEventArgs* args)->HRESULT {
        LPWSTR raw{};args->get_ResultFilePath(&raw);auto path=std::filesystem::path(wide(take(raw)));
        if(!downloads.empty()) {
          path=downloads/path.filename();
          // Do not overwrite a previous download with the same server filename.
          const auto original=path;std::error_code ec;
          for(int i=1;std::filesystem::exists(path,ec)&&i<10000;++i)
            path=original.parent_path()/(original.stem().wstring()+L" ("+std::to_wstring(i)+L")"+original.extension().wstring());
          args->put_ResultFilePath(path.c_str());
        }
        args->put_Handled(TRUE);status="Download started: "+utf8(path.c_str());publish();return S_OK;
      }).Get(),&token);
    // Accelerator callbacks are synchronous; post WebView operations so they
    // run after the callback has returned, without a nested message loop.
    return controller->add_AcceleratorKeyPressed(Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
      [this](ICoreWebView2Controller*,ICoreWebView2AcceleratorKeyPressedEventArgs* args)->HRESULT {
        COREWEBVIEW2_KEY_EVENT_KIND kind{};args->get_KeyEventKind(&kind);
        if(kind!=COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN&&kind!=COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) return S_OK;
        UINT key{};args->get_VirtualKey(&key);const bool ctrl=GetKeyState(VK_CONTROL)<0,alt=GetKeyState(VK_MENU)<0;
        int action{};
        if(key==VK_ESCAPE&&!ctrl) action=1;
        else if(ctrl&&key=='L') action=2;
        else if(key==VK_F5) action=3;
        else if(alt&&key==VK_LEFT) action=4;
        else if(alt&&key==VK_RIGHT) action=5;
        if(action) {args->put_Handled(TRUE);PostMessageW(dispatcher,action_message,action,0);}
        return S_OK;
      }).Get(),&accelerator);
  }
  void start() {
    const auto result=CreateCoreWebView2EnvironmentWithOptions(nullptr,profile.c_str(),nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [this](HRESULT result,ICoreWebView2Environment* created)->HRESULT {
          if(closing) return S_OK;
          if(FAILED(result)||!created) {fail("Unable to initialize WebView2 Runtime",FAILED(result)?result:E_FAIL);return S_OK;}
          environment=created;
          ComPtr<ICoreWebView2Environment5> lifecycle;
          if(SUCCEEDED(environment.As(&lifecycle))) {
            const auto registered=lifecycle->add_BrowserProcessExited(
              Callback<ICoreWebView2BrowserProcessExitedEventHandler>([this](ICoreWebView2Environment*,ICoreWebView2BrowserProcessExitedEventArgs*)->HRESULT {
                runtime_exited=true;
                if(closing) PostMessageW(dispatcher,finish_message,0,0);
                return S_OK;
              }).Get(),&runtime_exit);
            waiting_for_runtime=SUCCEEDED(registered);
          }
          const auto started=environment->CreateCoreWebView2Controller(host,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
              [this](HRESULT result,ICoreWebView2Controller* created)->HRESULT {
                if(closing) {if(created) created->Close();return S_OK;}
                if(FAILED(result)||!created) {fail("Unable to create embedded WebView2",FAILED(result)?result:E_FAIL);return S_OK;}
                controller=created;const auto obtained=controller->get_CoreWebView2(&view);
                if(FAILED(obtained)||!view) {fail("Unable to obtain WebView2 page",FAILED(obtained)?obtained:E_FAIL);return S_OK;}
                const auto events=install_events();if(FAILED(events)) {fail("Unable to register browser events",events);return S_OK;}
                resize(true);controller->put_ZoomFactor(zoom);ready=true;publish();navigate(uri);clear_data();
                if(visible) reveal();else controller->put_IsVisible(FALSE);
                return S_OK;
              }).Get());
          if(FAILED(started)) fail("Unable to create WebView2 controller",started);
          return S_OK;
        }).Get());
    if(FAILED(result)) fail("Unable to initialize WebView2 Runtime",result);
  }
  void shutdown() {
    if(closing) return;
    closing=true;KillTimer(dispatcher,1);
    // Close disconnects event handlers, child windows and WebView processes.
    if(controller) {controller->remove_AcceleratorKeyPressed(accelerator);controller->Close();}
    const bool had_view=bool(view);
    view.Reset();controller.Reset();
    loading=false;ready=false;publish();return_focus();
    if(IsWindow(host)) ShowWindow(host,SW_HIDE);
    if(had_view&&waiting_for_runtime&&!runtime_exited) SetTimer(dispatcher,2,1500,nullptr);
    else finish();
  }
  void finish() {
    if(finished) return;
    finished=true;KillTimer(dispatcher,2);
    ComPtr<ICoreWebView2Environment5> lifecycle;
    if(environment&&waiting_for_runtime&&SUCCEEDED(environment.As(&lifecycle))) lifecycle->remove_BrowserProcessExited(runtime_exit);
    environment.Reset();
    if(IsWindow(host)) DestroyWindow(host);
    DestroyWindow(dispatcher);PostQuitMessage(0);
  }
};
LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
  auto* b=reinterpret_cast<Browser*>(GetWindowLongPtrW(window,GWLP_USERDATA));
  if(message==WM_NCCREATE) {
    b=static_cast<Browser*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(b));
    if(GetParent(window)!=HWND_MESSAGE) b->host=window;
  }
  if(!b) return DefWindowProcW(window,message,wparam,lparam);
  switch(message) {
  case WM_TIMER: if(wparam==2) b->finish();else b->tick();return 0;
  case WM_CLOSE: case close_message: b->shutdown();return 0;
  case finish_message: b->finish();return 0;
  case WM_DESTROY:
    // The parent can destroy this native child while WebView2 is unwinding a
    // window callback. Defer controller closure until that native stack returns.
    if(window==b->host&&!b->closing) PostMessageW(b->dispatcher,close_message,0,0);
    else if(window==b->dispatcher) PostQuitMessage(0);
    return 0;
  case popup_message: b->navigate(b->popup);b->popup.clear();return 0;
  case action_message:
    if(wparam==1) b->shutdown();
    else if(wparam==2) {b->address_requested=true;b->publish();}
    else if(b->view) {
      if(wparam==3) b->view->Reload();
      else if(wparam==4) b->view->GoBack();
      else if(wparam==5) b->view->GoForward();
    }
    return 0;
  case WM_SETFOCUS: if(b->controller&&b->visible&&b->error.empty()) b->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);return 0;
  case WM_KEYDOWN: if(wparam==VK_ESCAPE) {PostMessageW(b->dispatcher,close_message,0,0);return 0;}break;
  case WM_PAINT: {
    PAINTSTRUCT paint{};const auto dc=BeginPaint(window,&paint);
    RECT bounds{};GetClientRect(window,&bounds);FillRect(dc,&bounds,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SetTextColor(dc,RGB(240,240,240));SetBkMode(dc,TRANSPARENT);InflateRect(&bounds,-32,-32);
    const auto text=wide(b->error.empty()?"Opening Internet Browser…":b->error+"\n\nPress Escape to return to OpenXMB.");
    DrawTextW(dc,text.c_str(),-1,&bounds,DT_LEFT|DT_TOP|DT_WORDBREAK|DT_NOPREFIX);EndPaint(window,&paint);return 0;
  }
  }
  return DefWindowProcW(window,message,wparam,lparam);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
  int count{};auto** arguments=CommandLineToArgvW(GetCommandLineW(),&count);
  if(!arguments) return 2;
  if(count!=5) {LocalFree(arguments);return 2;}
  Browser browser;
  try {
    browser.uri=utf8(arguments[1]);browser.parent=reinterpret_cast<HWND>(std::stoull(arguments[2]));
    browser.profile=arguments[3];browser.state_path=arguments[4];
  }catch(...) {LocalFree(arguments);return 2;}
  LocalFree(arguments);
  if(!web_uri(browser.uri)||!IsWindow(browser.parent)) return 2;
  GetWindowThreadProcessId(browser.parent,&browser.parent_pid);
  browser.parent_process=OpenProcess(SYNCHRONIZE,FALSE,browser.parent_pid);
  if(!browser.parent_process) return 3;
  // Match the shell's DPI awareness before creating a cross-process child.
  SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(browser.parent));
  const auto apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  if(FAILED(apartment)) {CloseHandle(browser.parent_process);return 3;}
  std::error_code error;std::filesystem::create_directories(browser.profile,error);
  WNDCLASSW klass{};klass.lpfnWndProc=window_proc;klass.hInstance=instance;
  klass.lpszClassName=L"OpenXMB.WebView2.Child";klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
  RegisterClassW(&klass);
  // A message-only dispatcher survives automatic child destruction when the
  // shell disappears, allowing WebView2's asynchronous cleanup to complete.
  browser.dispatcher=CreateWindowExW(0,klass.lpszClassName,L"OpenXMB Browser Lifecycle",0,
    0,0,0,0,HWND_MESSAGE,nullptr,instance,&browser);
  browser.host=CreateWindowExW(WS_EX_CONTROLPARENT,klass.lpszClassName,L"OpenXMB Web Content",
    WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,1,1,browser.parent,nullptr,instance,&browser);
  if(browser.host) {
    browser.resize();browser.publish();SetTimer(browser.dispatcher,1,20,nullptr);
    if(error) browser.fail("Cannot create browser profile",HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    else browser.start();
    MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0) {TranslateMessage(&message);DispatchMessageW(&message);}
    if(!browser.closing) browser.shutdown();
  }
  CloseHandle(browser.parent_process);CoUninitialize();
  return browser.host?0:3;
}
