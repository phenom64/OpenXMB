#include "openxmb/integrations/xmb_overlay.hpp"
#include "openxmb/integrations/session_window.hpp"
#include "openxmb/integrations/desktop.hpp"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace openxmb::integrations;
using namespace std::chrono_literals;
void require(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
struct Fixture {
  Display* d{}; Window root{},target{},shell{}; std::atomic<bool> stop{},deny{};std::thread wm;
  Atom atom(const char* name){return XInternAtom(d,name,False);}
  void property(Window w,const char* name,Atom type,unsigned long value){XChangeProperty(d,w,atom(name),type,32,PropModeReplace,reinterpret_cast<unsigned char*>(&value),1);}
  Fixture() {
    require(std::getenv("OPENXMB_X11_TEST_ISOLATED"),"Run only in an explicitly isolated Xvfb server");
    XInitThreads();d=XOpenDisplay(nullptr);require(d,"Xvfb display missing");root=DefaultRootWindow(d);
    XSelectInput(d,root,SubstructureRedirectMask|SubstructureNotifyMask);
    property(root,"_NET_SUPPORTING_WM_CHECK",XA_WINDOW,root);
    target=XCreateSimpleWindow(d,root,30,40,160,100,0,0,0x2040c0);
    shell=XCreateSimpleWindow(d,root,240,200,100,60,0,0,0);
    for(auto w:{target,shell})property(w,"_NET_WM_PID",XA_CARDINAL,getpid());
    property(root,"_NET_CLIENT_LIST_STACKING",XA_WINDOW,target);
    XMapWindow(d,target);XSetInputFocus(d,target,RevertToParent,CurrentTime);property(root,"_NET_ACTIVE_WINDOW",XA_WINDOW,target);XSync(d,False);
    wm=std::thread([this]{while(!stop){while(XPending(d)){XEvent e{};XNextEvent(d,&e);
      if(e.type==MapRequest){XMapWindow(d,e.xmaprequest.window);XRaiseWindow(d,e.xmaprequest.window);}
      if(e.type==ClientMessage&&e.xclient.message_type==atom("_NET_ACTIVE_WINDOW")&&!deny){XSetInputFocus(d,e.xclient.window,RevertToParent,CurrentTime);property(root,"_NET_ACTIVE_WINDOW",XA_WINDOW,e.xclient.window);}
    }XFlush(d);std::this_thread::sleep_for(2ms);}});
  }
  ~Fixture(){stop=true;if(wm.joinable())wm.join();if(d){XDestroyWindow(d,target);XDestroyWindow(d,shell);XCloseDisplay(d);}}
};
int main(int argc,char** argv)try {
  if(argc==2&&std::string(argv[1])=="--no-compositor"){
    XmbOverlay overlay;require(!overlay.prepare({0,0,32,32},{}),"Missing compositor must fail closed");
    std::cout<<"PASS missing compositor fallback\n";return 0;
  }
  Fixture fixture;
  auto target=find_session_window(getpid());require(target.has_value(),"X11 must discover the unique local process window");
  require(target->handle==fixture.target&&target->process_creation,"Window identity must pin its real client PID and process creation");
  const auto bounds=session_window_bounds(*target);require(bounds&&bounds->x==30&&bounds->y==40&&bounds->width==160&&bounds->height==100,"Validated client bounds changed");
  auto wrong=*target;++wrong.process_creation;require(!validate_session_window(wrong),"Stale process creation must be rejected");
  wrong=*target;wrong.pid=1;require(!validate_session_window(wrong),"A forged window PID must be rejected");
  require(bool(focus_session_window(*target)),"WM-approved EWMH focus must succeed");
  auto output=std::filesystem::temp_directory_path()/("openxmb-x11-"+std::to_string(getpid())+".png");
  std::filesystem::remove(output);
  auto captured=capture_session_window(*target,output,fixture.shell);
  require(bool(captured),captured.message.c_str());
  std::ifstream png(output,std::ios::binary);char signature[8]{};png.read(signature,8);
  require(std::string_view(signature,8)==std::string_view("\x89PNG\r\n\x1a\n",8),"Capture must publish a PNG");
  require(!capture_session_window(*target,output,fixture.shell),"Capture must not replace an existing user file");
  std::filesystem::remove(output);
  XSetWindowAttributes cover_attributes{};cover_attributes.override_redirect=True;cover_attributes.background_pixel=0xff0000;
  auto cover=XCreateWindow(fixture.d,fixture.root,50,60,30,30,0,CopyFromParent,InputOutput,CopyFromParent,CWOverrideRedirect|CWBackPixel,&cover_attributes);
  XMapRaised(fixture.d,cover);XSync(fixture.d,False);
  require(capture_session_window(*target,output,fixture.shell).state==CaptureState::pending,"Obscuring menus must block capture");
  XDestroyWindow(fixture.d,cover);XSync(fixture.d,False);
  std::vector<OverlayInput> events;XmbOverlay overlay;
  const auto prepared=overlay.prepare({0,0,320,240},[&](auto input){events.push_back(input);});
  require(bool(prepared),prepared.message.c_str());
  require(!overlay.visible()&&!overlay.focused()&&!overlay.show(),"Preparation must remain hidden until first frame");
  std::vector<std::uint8_t> pixels(320*240*4);
  for(std::size_t i=0;i<pixels.size();i+=4){pixels[i+2]=188;pixels[i+3]=128;}
  require(bool(overlay.present(pixels,320,240,320*4)),"ARGB compositor present failed");
  require(!overlay.visible(),"Present must not map the overlay");
  bool rejected{};std::thread wrong_thread([&]{rejected=!overlay.present(pixels,320,240,320*4);});wrong_thread.join();
  require(rejected,"Cross-thread presentation must be rejected");
  auto shown=overlay.show();require(bool(shown),shown.message.c_str());require(overlay.visible()&&overlay.focused(),"WM-approved overlay must become visible and focused");
  std::this_thread::sleep_for(80ms);
  auto* blended=XGetImage(fixture.d,fixture.root,50,60,1,1,AllPlanes,ZPixmap);require(blended,"Composited screen pixel unavailable");
  const auto pixel=XGetPixel(blended,0,0);XDestroyImage(blended);
  const auto red=(pixel>>16)&255,green=(pixel>>8)&255,blue=pixel&255;
  std::cout<<"Composited application sample RGB: "<<red<<','<<green<<','<<blue<<'\n';
  require(red>=135&&red<=155&&green>=25&&green<=40&&blue>=85&&blue<=105,"Real compositor did not blend half-red ARGB over the blue application");
  XEvent key{};key.xkey.type=KeyPress;key.xkey.display=fixture.d;key.xkey.window=overlay.native_handle();key.xkey.root=fixture.root;key.xkey.keycode=XKeysymToKeycode(fixture.d,XK_Left);key.xkey.same_screen=True;
  XSendEvent(fixture.d,key.xkey.window,False,KeyPressMask,&key);XSync(fixture.d,False);(void)overlay.focused();
  require(std::ranges::any_of(events,[](const auto& event){return event.kind==OverlayInputKind::key_down&&event.virtual_key==XK_Left;}),"Private X11 event pump lost the native keysym");
  overlay.hide();require(!overlay.visible(),"Hide must unmap overlay");
  require(bool(focus_session_window(*target)),"Game focus must be recoverable");
  fixture.deny=true;shown=overlay.show();require(!shown&&!overlay.visible(),"Denied activation must hide overlay without forcing focus");
  fixture.deny=false;
  const auto owned=launch_owned_process({"/bin/sleep","10"});require(bool(owned),"Owned child fixture launch failed");
  require(owned.process->observe().process_creation!=0,"Linux owned children must expose the same process creation token");
  fixture.property(fixture.target,"_NET_WM_PID",XA_CARDINAL,owned.process->id());XSync(fixture.d,False);
  require(!find_session_window(owned.process->id()),"A forged _NET_WM_PID must not override XRes client identity");
  fixture.property(fixture.target,"_NET_WM_PID",XA_CARDINAL,getpid());XSync(fixture.d,False);
  require(bool(owned.process->stop()),"Owned child cleanup failed");
  setenv("XDG_SESSION_TYPE","wayland",1);require(!overlay.prepare({0,0,32,32},{}),"Wayland must not silently use Xwayland overlay");unsetenv("XDG_SESSION_TYPE");
  std::cout<<"PASS isolated X11 identity, focus/denial, ARGB lifecycle, PNG and process token\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
