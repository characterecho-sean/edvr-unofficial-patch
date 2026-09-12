#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <vector>

namespace edvr::openxr {
class NativeDevice {
 public:
  NativeDevice()=default;
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
  HRESULT initialize(const LUID& required,D3D_FEATURE_LEVEL minimum) {
    reset();auto levels=featureLevels(minimum);if(levels.empty())return E_INVALIDARG;
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
    hr=D3D11CreateDevice(selected.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels.data(),UINT(levels.size()),D3D11_SDK_VERSION,&device,&got,&context);
    // Older D3D11 implementations reject the 11.1 enumerator. Retry the same
    // adapter only, without weakening the runtime's minimum feature level.
    if(hr==E_INVALIDARG&&levels.front()==D3D_FEATURE_LEVEL_11_1&&levels.size()>1) {
      device.Reset();context.Reset();
      hr=D3D11CreateDevice(selected.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels.data()+1,UINT(levels.size()-1),D3D11_SDK_VERSION,&device,&got,&context);
    }
    if(FAILED(hr))return hr;
    hr=validate(device.Get(),required,minimum);if(FAILED(hr))return hr;
    device_=device;context_=context;feature_=got;return S_OK;
  }
  void reset(){context_.Reset();device_.Reset();feature_=D3D_FEATURE_LEVEL_1_0_CORE;}
  ID3D11Device* device()const{return device_.Get();}
  ID3D11DeviceContext* context()const{return context_.Get();}
  D3D_FEATURE_LEVEL featureLevel()const{return feature_;}
 private:
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  D3D_FEATURE_LEVEL feature_=D3D_FEATURE_LEVEL_1_0_CORE;
};
}
