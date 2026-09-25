#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include "../common/system_d3d11.h"
#include "native_trace.h"

namespace edvr::openxr {
class NativeDevice {
 public:
  NativeDevice()=default;
  ~NativeDevice(){reset();}
  NativeDevice(const NativeDevice&)=delete;
  NativeDevice& operator=(const NativeDevice&)=delete;
  static bool matchesLuid(const LUID& a,const LUID& b) {return a.LowPart==b.LowPart&&a.HighPart==b.HighPart;}
  static std::vector<D3D_FEATURE_LEVEL> featureLevels(D3D_FEATURE_LEVEL minimum) {
    const D3D_FEATURE_LEVEL all[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};
    std::vector<D3D_FEATURE_LEVEL> levels;for(auto level:all)if(level>=minimum)levels.push_back(level);return levels;
  }
  static HRESULT validate(ID3D11Device* device,const LUID& required,D3D_FEATURE_LEVEL minimum) {
    if(!device)return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    HRESULT hr=device->QueryInterface(IID_PPV_ARGS(&dxgi));if(FAILED(hr))return hr;
    hr=dxgi->GetAdapter(&adapter);if(FAILED(hr))return hr;
    DXGI_ADAPTER_DESC desc{};hr=adapter->GetDesc(&desc);if(FAILED(hr))return hr;
    if(!matchesLuid(desc.AdapterLuid,required)||device->GetFeatureLevel()<minimum)return E_FAIL;
    return device->GetDeviceRemovedReason();
  }
  // The default create function is System32's, never an import: naming
  // D3D11CreateDevice here would bind every host of this header to whichever
  // d3d11.dll sits beside it, and beside the game that is EDVR's own proxy.
  HRESULT initialize(const LUID& required,D3D_FEATURE_LEVEL minimum,
                     decltype(&D3D11CreateDevice) createDevice=systemD3D11CreateDevice()) {
    reset();if(!createDevice)return E_INVALIDARG;
    auto levels=featureLevels(minimum);if(levels.empty())return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;HRESULT hr=CreateDXGIFactory1(IID_PPV_ARGS(&factory));if(FAILED(hr))return hr;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> selected;
    for(UINT n=0;;++n) {
      Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;hr=factory->EnumAdapters1(n,&adapter);
      if(hr==DXGI_ERROR_NOT_FOUND)break;if(FAILED(hr))return hr;
      DXGI_ADAPTER_DESC1 desc{};hr=adapter->GetDesc1(&desc);if(FAILED(hr))return hr;
      if(matchesLuid(desc.AdapterLuid,required)){selected=adapter;break;}
    }
    if(!selected)return DXGI_ERROR_NOT_FOUND;
    Microsoft::WRL::ComPtr<ID3D11Device> device;Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL got{};
    hr=createDevice(selected.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels.data(),UINT(levels.size()),D3D11_SDK_VERSION,&device,&got,&context);
    // Older D3D11 implementations reject the 11.1 enumerator. Retry the same
    // adapter only, without weakening the runtime's minimum feature level.
    if(hr==E_INVALIDARG&&levels.front()==D3D_FEATURE_LEVEL_11_1&&levels.size()>1) {
      device.Reset();context.Reset();
      hr=createDevice(selected.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels.data()+1,UINT(levels.size()-1),D3D11_SDK_VERSION,&device,&got,&context);
    }
    if(FAILED(hr))return hr;
    hr=validate(device.Get(),required,minimum);if(FAILED(hr))return hr;
    device_=device;context_=context;feature_=got;return S_OK;
  }
  HRESULT initializeExisting(ID3D11Device* device,const LUID& required,D3D_FEATURE_LEVEL minimum) {
    // Validate the pre-existing game's device; never replace it on mismatch.
    reset();const auto r=validate(device,required,minimum);if(FAILED(r))return r;
    device->GetImmediateContext(&context_);
    if(!context_)return E_NOINTERFACE;
    device_=device;feature_=device->GetFeatureLevel();return S_OK;
  }
  // Bypass the application's proxy export when creating the XR-owned device.
  // openSystemD3D11 (common/system_d3d11.h) takes the System32 module that is
  // already mapped, found by full path with GetModuleHandleExW rather than
  // LoadLibraryExW: another d3d11 mod chained behind EDVR (3Dmigoto/EDHM with
  // load_library_redirect=2) hooks LoadLibraryExW and answers the System32
  // path with its own game-directory proxy, which the identity check then
  // rightly rejects and no XR device is ever created. The game's d3d11 proxy
  // keeps the genuine module mapped for the life of the process, so the load
  // fallback only serves bare test rigs. Keep this explicit System32 module
  // reference through device/context release.
  HRESULT initializeSeparate(const LUID& required,D3D_FEATURE_LEVEL minimum) {
    separateRoute_="none";separateModule_.clear();
    struct ModuleReference {
      HMODULE value=nullptr;
      ~ModuleReference(){if(value)FreeLibrary(value);}
    } module;
    const SystemD3D11 system=openSystemD3D11();
    module.value=system.module;separateRoute_=system.route;
    if(!system.path.empty())separateModule_=utf8(system.path.c_str(),system.path.size());
    if(FAILED(system.result))return system.result;
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module.value,"D3D11CreateDevice"));
    const HRESULT result=initialize(required,minimum,create);
    if(FAILED(result))return result;
    systemModule_=module.value;module.value=nullptr;return S_OK;
  }
  // Diagnostics for the last initializeSeparate: how the d3d11 module was
  // obtained ("mapped", "loaded" or "none") and the path it actually has.
  const char* separateModuleRoute()const{return separateRoute_;}
  const std::string& separateModulePath()const{return separateModule_;}
  void reset() {
    context_.Reset();
    device_.Reset();
    feature_ = D3D_FEATURE_LEVEL_1_0_CORE;
    if (systemModule_) {
      nativeTracePrintf("device_module_reset,pinned=%u,path=%s\n",
          unsigned(edvr::isSystemD3D11Pinned()), separateModule_.c_str());
      systemModule_ = nullptr;
    }
  }
  ID3D11Device* device()const{return device_.Get();}
  ID3D11DeviceContext* context()const{return context_.Get();}
  D3D_FEATURE_LEVEL featureLevel()const{return feature_;}
 private:
  static std::string utf8(const wchar_t* text,size_t length) {
    const int needed=WideCharToMultiByte(CP_UTF8,0,text,int(length),nullptr,0,nullptr,nullptr);
    if(needed<=0)return "?";
    std::string out(size_t(needed),'\0');
    if(WideCharToMultiByte(CP_UTF8,0,text,int(length),out.data(),needed,nullptr,nullptr)!=needed)return "?";
    return out;
  }
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  D3D_FEATURE_LEVEL feature_=D3D_FEATURE_LEVEL_1_0_CORE;
  HMODULE systemModule_=nullptr;
  const char* separateRoute_="none";
  std::string separateModule_;
};
}
