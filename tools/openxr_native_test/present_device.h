#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace edvr::openxr {
// Standalone test device created before VR Init, through the actual graphics
// proxy and its real owned-swapchain hook. The HWND remains hidden. Creating,
// presenting, message pumping and destruction all stay on this caller.
class PresentDevice final {
 public:
  PresentDevice()=default;
  PresentDevice(const PresentDevice&)=delete;
  PresentDevice& operator=(const PresentDevice&)=delete;
  ~PresentDevice(){swapchain_.Reset();context_.Reset();device_.Reset();if(window_)DestroyWindow(window_);}
  HRESULT initialize(HMODULE proxy,D3D_DRIVER_TYPE driver) {
    if(window_||!proxy)return E_INVALIDARG;
    const auto create=reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(
      GetProcAddress(proxy,"D3D11CreateDeviceAndSwapChain"));
    if(!create)return E_NOINTERFACE;
    // Built-in window class avoids process-global class registration cleanup.
    window_=CreateWindowExW(0,L"STATIC",L"EDVR hidden Present fixture",WS_POPUP,
      0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!window_)return HRESULT_FROM_WIN32(GetLastError());
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
  HRESULT present(UINT flags=0) {
    MSG message{};
    while(PeekMessageW(&message,window_,0,0,PM_REMOVE)) {
      TranslateMessage(&message);DispatchMessageW(&message);
    }
    return swapchain_?swapchain_->Present(0,flags):E_UNEXPECTED;
  }
  ID3D11Device* device()const{return device_.Get();}
  ID3D11DeviceContext* context()const{return context_.Get();}
  IDXGISwapChain* swapchain()const{return swapchain_.Get();}
 private:
  HWND window_=nullptr;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IDXGISwapChain> swapchain_;
};
}
