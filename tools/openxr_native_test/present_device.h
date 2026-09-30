#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace edvr::openxr {
// Standalone test device created before VR Init, through the actual graphics
// proxy and its real owned-swapchain hook. The HWND remains hidden. Creating,
// presenting, message pumping and destruction all stay on this caller.
//
// The HWND is a child of a message-only window, and that is load-bearing. The
// proxy force-foregrounds the window a swap chain is created on
// (d3d11.focus_on_launch, on by default: it is for the game's own window), and
// a hidden top-level window is taken just the same. Every rig that made one
// took the keyboard from whoever was typing during a full build, at each of a
// dozen exe launches. The proxy leaves any window that is not a top-level
// window on the desktop alone (src\d3d11\focus_target.h), and one under a
// message-only parent is on no desktop at all. openxr_present_test checks
// this window against that rule; tools\run_jobs.py watches every rig with
// tools\focus_watch.py and fails the build if one puts a window on the desktop
// or takes the focus.
class PresentDevice final {
 public:
  PresentDevice()=default;
  PresentDevice(const PresentDevice&)=delete;
  PresentDevice& operator=(const PresentDevice&)=delete;
  ~PresentDevice(){
    swapchain_.Reset();context_.Reset();device_.Reset();
    if(window_)DestroyWindow(window_);
    if(anchor_)DestroyWindow(anchor_);
  }
  HRESULT initialize(HMODULE proxy,D3D_DRIVER_TYPE driver) {
    if(window_||!proxy)return E_INVALIDARG;
    const auto create=reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(
      GetProcAddress(proxy,"D3D11CreateDeviceAndSwapChain"));
    if(!create)return E_NOINTERFACE;
    // Built-in window class avoids process-global class registration cleanup.
    anchor_=CreateWindowExW(0,L"STATIC",L"EDVR hidden Present fixture parent",WS_POPUP,
      0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!anchor_)return HRESULT_FROM_WIN32(GetLastError());
    window_=CreateWindowExW(0,L"STATIC",L"EDVR hidden Present fixture",WS_CHILD,
      0,0,64,64,anchor_,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!window_){
      const DWORD error=GetLastError();
      DestroyWindow(anchor_);anchor_=nullptr;
      return HRESULT_FROM_WIN32(error);
    }
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width=desc.BufferDesc.Height=64;
    desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=2;desc.OutputWindow=window_;desc.Windowed=TRUE;
    desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    HRESULT r=create(nullptr,driver,nullptr,0,levels,2,D3D11_SDK_VERSION,&desc,
      &swapchain_,&device_,nullptr,&context_);
    if(r==E_INVALIDARG) {
      swapchain_.Reset();context_.Reset();device_.Reset();
      r=create(nullptr,driver,nullptr,0,levels+1,1,D3D11_SDK_VERSION,&desc,
        &swapchain_,&device_,nullptr,&context_);
    }
    return r;
  }
  void pumpMessages() {
    MSG message{};
    while(PeekMessageW(&message,window_,0,0,PM_REMOVE)) {
      TranslateMessage(&message);DispatchMessageW(&message);
    }
  }
  HRESULT present(UINT flags=0) {
    pumpMessages();
    return swapchain_?swapchain_->Present(0,flags):E_UNEXPECTED;
  }
  HWND window()const{return window_;}
  ID3D11Device* device()const{return device_.Get();}
  ID3D11DeviceContext* context()const{return context_.Get();}
  IDXGISwapChain* swapchain()const{return swapchain_.Get();}
 private:
  HWND anchor_=nullptr;
  HWND window_=nullptr;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IDXGISwapChain> swapchain_;
};
}
