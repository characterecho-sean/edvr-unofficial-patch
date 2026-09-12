#include "native_device.h"
#include <cstdio>
#include <string>
using edvr::openxr::NativeDevice;
int wmain(int argc,wchar_t** argv) {
  if(argc!=2)return 2;std::wstring arg=argv[1];
  if(arg==L"--dry-run"){std::puts("native_device_test: dry-run (no WARP device)");return 0;}
  if(arg!=L"--self-test")return 2;
  unsigned fails=0,checks=0;auto check=[&](bool value,const char* name){++checks;if(!value){++fails;std::printf("FAIL: %s\n",name);}};
  const auto a=NativeDevice::featureLevels(D3D_FEATURE_LEVEL_11_0);
  check(a==std::vector<D3D_FEATURE_LEVEL>({D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0}),"filtered 11.0 levels");
  check(NativeDevice::featureLevels(D3D_FEATURE_LEVEL_11_1)==std::vector<D3D_FEATURE_LEVEL>({D3D_FEATURE_LEVEL_11_1}),"11.1 minimum");
  check(NativeDevice::featureLevels(D3D_FEATURE_LEVEL_12_1).empty(),"unsupported minimum");
  D3D_FEATURE_LEVEL level{};Microsoft::WRL::ComPtr<ID3D11Device> device;Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  const HRESULT hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context);
  check(SUCCEEDED(hr)&&device&&context,"WARP creation");
  if(device){
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    const bool found=SUCCEEDED(device.As(&dxgi))&&SUCCEEDED(dxgi->GetAdapter(&adapter));check(found,"actual WARP adapter");
    if(found){
      DXGI_ADAPTER_DESC desc{};check(SUCCEEDED(adapter->GetDesc(&desc)),"adapter description");
      check(NativeDevice::validate(device.Get(),desc.AdapterLuid,level)==S_OK,"matching live device");
      LUID foreign=desc.AdapterLuid;foreign.LowPart^=1;
      check(FAILED(NativeDevice::validate(device.Get(),foreign,level)),"foreign adapter rejected");
      check(FAILED(NativeDevice::validate(device.Get(),desc.AdapterLuid,D3D_FEATURE_LEVEL_12_1)),"feature minimum rejected");
    }
  }
  check(FAILED(NativeDevice::validate(nullptr,LUID{},D3D_FEATURE_LEVEL_10_0)),"null device rejected");
  NativeDevice native;
  check(FAILED(native.initialize(LUID{},D3D_FEATURE_LEVEL_12_1))&&!native.device()&&!native.context(),"failed initialize leaves empty ownership");
  std::printf("native_device_test: %u checks, %u failures\n",checks,fails);return fails?1:0;
}
