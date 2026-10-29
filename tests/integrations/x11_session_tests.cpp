#include "openxmb/integrations/xmb_overlay.hpp"
#include "openxmb/integrations/session_window.hpp"
#include "openxmb/integrations/desktop.hpp"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
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
  Display* d{};void* xtst{}; Window root{},target{},shell{}; std::atomic<bool> stop{},deny{};std::thread wm;
  void* xtst_runtime(){if(!xtst)xtst=dlopen("libXtst.so.6",RTLD_NOW|RTLD_LOCAL);return xtst;}
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
  ~Fixture(){stop=true;if(wm.joinable())wm.join();if(d){XDestroyWindow(d,target);XDestroyWindow(d,shell);XCloseDisplay(d);}if(xtst)dlclose(xtst);}
};
void blocker_bounds(Fixture& fixture,const SessionWindowTarget& target,const std::filesystem::path& output) {
  const auto check=[&](int x,int y,unsigned width,unsigned height,bool blocked,unsigned border=0){
    XSetWindowAttributes attributes{};attributes.override_redirect=True;attributes.background_pixel=0xff0000;attributes.border_pixel=0xff0000;
    const auto window=XCreateWindow(fixture.d,fixture.root,x,y,width,height,border,CopyFromParent,InputOutput,CopyFromParent,CWOverrideRedirect|CWBackPixel|CWBorderPixel,&attributes);
    XMapRaised(fixture.d,window);XSync(fixture.d,False);const auto result=capture_session_window(target,output,fixture.shell);
    XDestroyWindow(fixture.d,window);XSync(fixture.d,False);
    if(blocked)require(result.state==CaptureState::pending&&!std::filesystem::exists(output),"A partly offscreen overlapping window must still block capture");
    else {require(bool(result),"A nonoverlapping partly offscreen or oversized window incorrectly blocked capture");std::filesystem::remove(output);}
  };
  check(-40,180,80,30,false);
  check(DisplayWidth(fixture.d,DefaultScreen(fixture.d))-20,10,60,20,false);
  check(240,-15,50,30,false);
  check(20,DisplayHeight(fixture.d,DefaultScreen(fixture.d))-10,40,30,false);
  check(0,180,20000,10,false);
  check(-10,60,60,30,true);
  check(10,60,5,30,true,15);
  XMoveWindow(fixture.d,fixture.target,-5,40);XSync(fixture.d,False);
  const auto offscreen=capture_session_window(target,output,fixture.shell);
  XMoveWindow(fixture.d,fixture.target,30,40);XSync(fixture.d,False);
  require(offscreen.state==CaptureState::pending&&!std::filesystem::exists(output),"The capture target must remain wholly onscreen");
  fixture.property(fixture.target,"_NET_WM_BYPASS_COMPOSITOR",XA_CARDINAL,1);XSync(fixture.d,False);
  const auto protected_frame=capture_session_window(target,output,fixture.shell);
  XDeleteProperty(fixture.d,fixture.target,fixture.atom("_NET_WM_BYPASS_COMPOSITOR"));XSync(fixture.d,False);
  require(protected_frame.state==CaptureState::unavailable&&!std::filesystem::exists(output),"Compositor-bypass content must remain unavailable");
}
void overlay_repeat(Fixture& fixture,XmbOverlay& overlay,std::vector<OverlayInput>& events) {
  struct Keyboard {
    Display* display;void* module;using Fake=Bool(*)(Display*,unsigned int,Bool,unsigned long);Fake fake{};unsigned key{},delay{},interval{};bool held{};
    explicit Keyboard(Fixture& fixture):display(fixture.d),module(fixture.xtst_runtime()) {
      require(module,"Isolated XTest runtime unavailable");fake=reinterpret_cast<Fake>(dlsym(module,"XTestFakeKeyEvent"));require(fake,"Isolated XTest key injection unavailable");
      require(XkbGetAutoRepeatRate(display,XkbUseCoreKbd,&delay,&interval),"Cannot read the isolated Xvfb repeat rate");
      require(XkbSetAutoRepeatRate(display,XkbUseCoreKbd,80,20),"Cannot configure the isolated Xvfb repeat rate");XAutoRepeatOn(display);key=XKeysymToKeycode(display,XK_Return);require(key,"Enter keycode unavailable");XSync(display,False);
    }
    void send(bool down){require(fake(display,key,down?True:False,CurrentTime),"XTest key event failed");held=down;XSync(display,False);}
    ~Keyboard(){if(fake&&held)(void)fake(display,key,False,CurrentTime);if(display&&delay&&interval)(void)XkbSetAutoRepeatRate(display,XkbUseCoreKbd,delay,interval);if(display)XSync(display,False);}
  } keyboard(fixture);
  const auto pump=[&](auto duration){const auto deadline=std::chrono::steady_clock::now()+duration;do{(void)overlay.focused();std::this_thread::sleep_for(2ms);}while(std::chrono::steady_clock::now()<deadline);};
  const auto count=[&](OverlayInputKind kind,bool repeat){return std::ranges::count_if(events,[&](const auto& event){return event.virtual_key==XK_Return&&event.kind==kind&&(kind!=OverlayInputKind::key_down||event.repeat==repeat);});};
  events.clear();keyboard.send(true);pump(400ms);
  require(count(OverlayInputKind::key_down,false)==1&&count(OverlayInputKind::key_down,true)>0,"Held Enter fabricated fresh confirmation presses instead of repeat downs");
  require(count(OverlayInputKind::key_up,false)==0,"Autorepeat fabricated logical releases before physical Enter release");
  std::cout<<"Enter held: fresh="<<count(OverlayInputKind::key_down,false)<<" repeats="<<count(OverlayInputKind::key_down,true)<<" releases="<<count(OverlayInputKind::key_up,false)<<'\n';
  keyboard.send(false);pump(30ms);require(count(OverlayInputKind::key_up,false)==1,"Physical Enter release was lost or duplicated");
  events.clear();keyboard.send(true);keyboard.send(false);keyboard.send(true);keyboard.send(false);pump(30ms);
  require(count(OverlayInputKind::key_down,false)==2&&count(OverlayInputKind::key_down,true)==0&&count(OverlayInputKind::key_up,false)==2,"Distinct rapid Enter taps were coalesced or duplicated");
  std::cout<<"Rapid Enter taps: fresh="<<count(OverlayInputKind::key_down,false)<<" releases="<<count(OverlayInputKind::key_up,false)<<'\n';
}
void passive_notice(Fixture& fixture) {
  using Motion=int(*)(Display*,int,int,int,unsigned long);using Button=int(*)(Display*,unsigned,Bool,unsigned long);using Key=int(*)(Display*,unsigned,Bool,unsigned long);
  auto* module=fixture.xtst_runtime();require(module,"Isolated XTest runtime unavailable");
  const auto motion=reinterpret_cast<Motion>(dlsym(module,"XTestFakeMotionEvent"));const auto button=reinterpret_cast<Button>(dlsym(module,"XTestFakeButtonEvent"));const auto key=reinterpret_cast<Key>(dlsym(module,"XTestFakeKeyEvent"));
  require(motion&&button&&key,"Isolated XTest pointer/key injection unavailable");
  const auto focus=[&]{Window window{};int revert{};XGetInputFocus(fixture.d,&window,&revert);return window;};
  const auto active=[&]{Atom type{};int format{};unsigned long count{},after{};unsigned char* data{};Window value{};
    if(XGetWindowProperty(fixture.d,fixture.root,fixture.atom("_NET_ACTIVE_WINDOW"),0,1,False,XA_WINDOW,&type,&format,&count,&after,&data)==Success&&data&&count==1)value=*reinterpret_cast<unsigned long*>(data);
    if(data)XFree(data);return value;};
  require(focus()==fixture.target&&active()==fixture.target,"The game fixture must own focus before the notice appears");
  std::vector<OverlayInput> events;XmbOverlay notice;
  const auto prepared=notice.prepare({50,60,40,30},[&](auto input){events.push_back(input);},OverlayInteraction::passive);
  require(bool(prepared),prepared.message.c_str());
  XWindowAttributes attributes{};require(XGetWindowAttributes(fixture.d,notice.native_handle(),&attributes)&&attributes.override_redirect,"The passive notice must be override-redirect");
  require(!(attributes.all_event_masks&(KeyPressMask|KeyReleaseMask|ButtonPressMask|ButtonReleaseMask|PointerMotionMask|FocusChangeMask)),"The passive notice selected input events");
  std::vector<std::uint8_t> pixels(40*30*4);
  for(std::size_t i=0;i<pixels.size();i+=4){pixels[i+2]=188;pixels[i+3]=128;}
  require(bool(notice.present(pixels,40,30,40*4)),"Passive ARGB present failed");
  require(!notice.show(true)&&!notice.visible(),"A passive notice accepted activation");
  const auto shown=notice.show(false);require(bool(shown),shown.message.c_str());
  std::this_thread::sleep_for(80ms);
  require(notice.visible()&&!notice.focused(),"Passive notice must be visible without focus");
  require(focus()==fixture.target&&active()==fixture.target,"Showing the passive notice changed game focus");
  auto* blended=XGetImage(fixture.d,fixture.root,55,65,1,1,AllPlanes,ZPixmap);require(blended,"Composited notice pixel unavailable");
  const auto pixel=XGetPixel(blended,0,0);XDestroyImage(blended);
  require(((pixel>>16)&255)>=135&&((pixel>>16)&255)<=155&&(pixel&255)>=85&&(pixel&255)<=105,"The passive notice was not composited above the game");
  require(motion(fixture.d,DefaultScreen(fixture.d),60,70,CurrentTime),"XTest pointer motion failed");XSync(fixture.d,False);
  Window root{},child{};int root_x{},root_y{},window_x{},window_y{};unsigned mask{};
  require(XQueryPointer(fixture.d,fixture.root,&root,&child,&root_x,&root_y,&window_x,&window_y,&mask)&&root_x==60&&root_y==70,"Pointer fixture position unavailable");
  require(child==fixture.target,"The pointer hit the notice instead of the game: input region is not empty");
  require(button(fixture.d,1,True,CurrentTime)&&button(fixture.d,1,False,CurrentTime),"XTest click failed");
  const auto left=XKeysymToKeycode(fixture.d,XK_Left);require(left&&key(fixture.d,left,True,CurrentTime)&&key(fixture.d,left,False,CurrentTime),"XTest key failed");
  XEvent direct{};direct.xkey.type=KeyPress;direct.xkey.display=fixture.d;direct.xkey.window=notice.native_handle();direct.xkey.root=fixture.root;direct.xkey.keycode=left;direct.xkey.same_screen=True;
  XSendEvent(fixture.d,direct.xkey.window,False,KeyPressMask|FocusChangeMask,&direct);XSync(fixture.d,False);
  require(bool(notice.present(pixels,40,30,40*4)),"Passive restack/present failed");
  std::this_thread::sleep_for(30ms);(void)notice.visible();
  require(events.empty(),"The passive notice routed pointer, keyboard or focus input");
  require(focus()==fixture.target&&active()==fixture.target,"Input while the notice was shown moved focus");
  notice.hide();require(!notice.visible()&&focus()==fixture.target,"Hiding the passive notice changed focus");
  std::cout<<"Passive notice: composited, override-redirect, empty input region, focus unchanged\n";
}
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
  if(!std::getenv("OPENXMB_X11_TEST_REPEAT_ONLY"))blocker_bounds(fixture,*target,output);
  passive_notice(fixture);
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
  overlay_repeat(fixture,overlay,events);
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
  std::cout<<"PASS isolated X11 identity, focus/denial, ARGB, PNG, offscreen blockers, held/released/rapid Enter and process token\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
