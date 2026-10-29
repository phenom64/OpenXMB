#include "windows_capture.hpp"
#if defined(_WIN32) && defined(OPENXMB_HAVE_WINDOWS_CAPTURE)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <combaseapi.h>
#include <dwmapi.h>
#include <d3d11_4.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <thread>

namespace openxmb::integrations::windows_capture {
namespace {
using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Foundation::Metadata::ApiInformation;

// GraphicsCapture.dll runs an internal worker thread that calls CoInitializeEx(MTA), waits, then
// calls CoUninitialize and returns into its own code. If that CoUninitialize releases the process's
// last MTA reference, COM tears the MTA down and unloads the DLLs it loaded, GraphicsCapture.dll
// included, while the worker is still running in it (execute AV at GraphicsCapture.dll_unloaded).
// A per-call init/uninit on the caller makes the worker the last MTA member after every capture.
// One process-lifetime MTA usage reference makes that teardown impossible. It is never released.
HRESULT hold_process_mta() noexcept {
  static const HRESULT status=[]() noexcept {CO_MTA_USAGE_COOKIE cookie{};return CoIncrementMTAUsage(&cookie);}();
  return status;
}

// Explicit or implicit MTA. STA callers would load the capture DLLs into an apartment whose
// owner controls their unload, so they are rejected rather than serviced.
bool in_multithreaded_apartment() noexcept {
  APTTYPE type{};APTTYPEQUALIFIER qualifier{};
  return SUCCEEDED(CoGetApartmentType(&type,&qualifier))&&type==APTTYPE_MTA;
}

// Screen rectangle whose top-left is the captured surface origin. Accepted only when its size
// equals the size Windows reports for the capture item, so the client crop is never guessed.
bool captured_window_rect(HWND hwnd,winrt::Windows::Graphics::SizeInt32 size,RECT& rect) {
  const auto matches=[&]{return rect.right-rect.left==size.Width&&rect.bottom-rect.top==size.Height;};
  if(SUCCEEDED(DwmGetWindowAttribute(hwnd,DWMWA_EXTENDED_FRAME_BOUNDS,&rect,sizeof(rect)))&&matches())return true;
  return GetWindowRect(hwnd,&rect)&&matches();
}

SessionCaptureResult failure(HRESULT code) {
  if(code==E_ACCESSDENIED)return {CaptureState::unavailable,"Windows denied native capture of this game window."};
  if(code==E_INVALIDARG||code==HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE))
    return {CaptureState::unavailable,"Windows cannot capture this game window. It may have closed or is not a capturable top-level window."};
  if(code==DXGI_ERROR_DEVICE_REMOVED||code==DXGI_ERROR_DEVICE_RESET||code==DXGI_ERROR_DEVICE_HUNG||code==DXGI_ERROR_DRIVER_INTERNAL_ERROR)
    return {CaptureState::pending,"The graphics device reset during native game capture."};
  if(code==DXGI_ERROR_UNSUPPORTED||code==DXGI_ERROR_NOT_CURRENTLY_AVAILABLE||code==E_NOTIMPL)
    return {CaptureState::unavailable,"This graphics device or Windows session does not support native game capture."};
  if(code==E_OUTOFMEMORY)return {CaptureState::unavailable,"Not enough memory for native game capture."};
  char message[96];
  std::snprintf(message,sizeof(message),"Windows could not capture this game window (HRESULT 0x%08lX).",static_cast<unsigned long>(code));
  return {CaptureState::unavailable,message};
}

Frame capture(HWND hwnd,SessionWindowBounds client) {
  Frame output;
  if(!ApiInformation::IsApiContractPresent(L"Windows.Foundation.UniversalApiContract",8)||!GraphicsCaptureSession::IsSupported())return output;
  output.supported=true;
  GraphicsCaptureItem item{nullptr};
  const auto interop=winrt::get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
  winrt::check_hresult(interop->CreateForWindow(hwnd,winrt::guid_of<GraphicsCaptureItem>(),winrt::put_abi(item)));
  const auto size=item.Size();
  if(size.Width<=0||size.Height<=0||size.Width>16384||size.Height>16384||
     static_cast<std::uint64_t>(size.Width)*size.Height*4>256u*1024u*1024u) {
    output.result={CaptureState::unavailable,"The game window exceeds the native capture size limit."};return output;
  }
  RECT source{};
  if(!captured_window_rect(hwnd,size,source)) {
    output.result={CaptureState::pending,"The game window is still changing its capture bounds."};return output;
  }
  const auto x=client.x-source.left,y=client.y-source.top;
  if(x<0||y<0||!client.width||!client.height||static_cast<std::uint64_t>(x)+client.width>static_cast<unsigned>(size.Width)||
     static_cast<std::uint64_t>(y)+client.height>static_cast<unsigned>(size.Height)) {
    output.result={CaptureState::pending,"The game client area is still changing its capture bounds."};return output;
  }
  winrt::com_ptr<ID3D11Device> device;winrt::com_ptr<ID3D11DeviceContext> context;
  winrt::check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    nullptr,0,D3D11_SDK_VERSION,device.put(),nullptr,context.put()));
  // The free-threaded pool touches this device from the capture worker thread.
  if(const auto threading=context.try_as<ID3D11Multithread>())threading->SetMultithreadProtected(TRUE);
  winrt::com_ptr<IInspectable> inspectable;
  winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(device.as<IDXGIDevice>().get(),inspectable.put()));
  const auto projected=inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
  struct Capture {
    Direct3D11CaptureFramePool pool{nullptr};GraphicsCaptureSession session{nullptr};
    ~Capture(){try{if(session)session.Close();}catch(...) {}try{if(pool)pool.Close();}catch(...) {}}
  } capture;
  capture.pool=Direct3D11CaptureFramePool::CreateFreeThreaded(projected,
    winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size);
  capture.session=capture.pool.CreateCaptureSession(item);
  if(ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",L"IsCursorCaptureEnabled"))capture.session.IsCursorCaptureEnabled(false);
  capture.session.StartCapture();
  Direct3D11CaptureFrame frame{nullptr};
  struct FrameGuard {Direct3D11CaptureFrame& frame;~FrameGuard(){try{if(frame)frame.Close();}catch(...) {}}} frame_guard{frame};
  const auto frame_deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1200);
  while(!(frame=capture.pool.TryGetNextFrame())&&std::chrono::steady_clock::now()<frame_deadline)std::this_thread::sleep_for(std::chrono::milliseconds(5));
  if(!frame) {output.result={CaptureState::pending,"Windows has not delivered a game capture frame yet."};return output;}
  const auto content=frame.ContentSize();
  RECT confirmed{};
  if(content.Width!=size.Width||content.Height!=size.Height||!captured_window_rect(hwnd,size,confirmed)||!EqualRect(&source,&confirmed)) {
    output.result={CaptureState::pending,"The game window moved or resized during native capture."};return output;
  }
  winrt::com_ptr<ID3D11Texture2D> texture;
  winrt::check_hresult(frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>()->GetInterface(__uuidof(ID3D11Texture2D),texture.put_void()));
  D3D11_TEXTURE2D_DESC description{};texture->GetDesc(&description);
  if(description.Format!=DXGI_FORMAT_B8G8R8A8_UNORM||description.SampleDesc.Count!=1||
     description.Width<static_cast<unsigned>(size.Width)||description.Height<static_cast<unsigned>(size.Height)) {
    output.result={CaptureState::unavailable,"Windows returned an unsupported game capture format."};return output;
  }
  description.Width=client.width;description.Height=client.height;description.MipLevels=1;description.ArraySize=1;
  description.Usage=D3D11_USAGE_STAGING;description.BindFlags=0;description.CPUAccessFlags=D3D11_CPU_ACCESS_READ;description.MiscFlags=0;
  winrt::com_ptr<ID3D11Texture2D> staging;winrt::check_hresult(device->CreateTexture2D(&description,nullptr,staging.put()));
  const D3D11_BOX box{static_cast<UINT>(x),static_cast<UINT>(y),0,static_cast<UINT>(x)+client.width,static_cast<UINT>(y)+client.height,1};
  context->CopySubresourceRegion(staging.get(),0,0,0,0,texture.get(),0,&box);
  context->Flush();
  // Bounded GPU readback: never block indefinitely on a stalled copy.
  D3D11_MAPPED_SUBRESOURCE mapped{};HRESULT status{};
  const auto copy_deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1000);
  while((status=context->Map(staging.get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped))==DXGI_ERROR_WAS_STILL_DRAWING&&
        std::chrono::steady_clock::now()<copy_deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if(status==DXGI_ERROR_WAS_STILL_DRAWING) {output.result={CaptureState::pending,"The graphics device did not finish reading the game frame in time."};return output;}
  winrt::check_hresult(status);
  struct Mapping{ID3D11DeviceContext* context;ID3D11Texture2D* texture;~Mapping(){context->Unmap(texture,0);}} mapping{context.get(),staging.get()};
  const auto row=static_cast<std::size_t>(client.width)*4;
  if(mapped.RowPitch<row) {output.result={CaptureState::unavailable,"Windows returned an invalid capture row stride."};return output;}
  output.bgra.resize(row*client.height);
  for(unsigned line=0;line<client.height;++line)std::memcpy(output.bgra.data()+line*row,static_cast<const unsigned char*>(mapped.pData)+static_cast<std::size_t>(line)*mapped.RowPitch,row);
  for(std::size_t at=3;at<output.bgra.size();at+=4)output.bgra[at]=255;
  output.result={CaptureState::saved,"Native game window frame captured."};
  return output;
}
}

Frame acquire(std::uintptr_t handle,SessionWindowBounds client) {
  Frame output;
  output.supported=true;
  if(FAILED(hold_process_mta())) {output.result={CaptureState::unavailable,"Windows COM could not start for native game capture."};return output;}
  if(!in_multithreaded_apartment()) {output.result={CaptureState::unavailable,"Native game capture must run on a worker thread outside a single-threaded apartment."};return output;}
  // capture() has destroyed its WinRT, D3D and frame objects before a handler runs; the
  // process-lifetime MTA keeps any captured restricted error info valid while it is released.
  try {
    return capture(reinterpret_cast<HWND>(handle),client);
  }catch(const winrt::hresult_error& error) {
    output.result=failure(error.code());
  }catch(const std::bad_alloc&) {
    output.result={CaptureState::unavailable,"Not enough memory for native game capture."};
  }catch(const std::exception&) {
    output.result={CaptureState::unavailable,"The native game capture could not be completed."};
  }
  return output;
}
}
#else
namespace openxmb::integrations::windows_capture {
Frame acquire(std::uintptr_t,SessionWindowBounds){return {};}
}
#endif
