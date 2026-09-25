#include "native_device.h"
#include <cstdio>
#include <cstring>
#include <string>
using edvr::openxr::NativeDevice;
using Microsoft::WRL::ComPtr;

static bool identity(IUnknown* first,IUnknown* second) {
  ComPtr<IUnknown> a,b;
  return first&&second&&first->QueryInterface(IID_PPV_ARGS(&a))==S_OK&&
    second->QueryInterface(IID_PPV_ARGS(&b))==S_OK&&a.Get()==b.Get();
}

// The System32 d3d11.dll path as UTF-8, and whether that module is already
// mapped (without touching its reference count). A mapped module must be taken
// over LoadLibraryExW, which chained d3d11 mods hook and redirect.
static std::string systemD3D11Path() {
  wchar_t directory[MAX_PATH]{};const UINT length=GetSystemDirectoryW(directory,MAX_PATH);
  const std::wstring wide=std::wstring(directory,length)+L"\\d3d11.dll";
  const int needed=WideCharToMultiByte(CP_UTF8,0,wide.c_str(),-1,nullptr,0,nullptr,nullptr);
  std::string out(size_t(needed>0?needed:1),'\0');
  if(needed>0)WideCharToMultiByte(CP_UTF8,0,wide.c_str(),-1,out.data(),needed,nullptr,nullptr);
  out.resize(needed>0?size_t(needed-1):0);return out;
}
static const char* expectedSeparateRoute() {
  wchar_t directory[MAX_PATH]{};const UINT length=GetSystemDirectoryW(directory,MAX_PATH);
  const std::wstring wide=std::wstring(directory,length)+L"\\d3d11.dll";
  HMODULE mapped=nullptr;
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,wide.c_str(),&mapped)&&mapped?"mapped":"loaded";
}

int hardwareTest() {
  unsigned fails=0,checks=0;auto check=[&](bool value,const char* name){++checks;if(!value){++fails;std::printf("FAIL: %s\n",name);}};
  ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory))==S_OK,"DXGI factory");
  ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 description{};
  if(factory)for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(i,&candidate)!=S_OK)break;
    DXGI_ADAPTER_DESC1 d{};if(candidate->GetDesc1(&d)==S_OK&&!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){adapter=candidate;description=d;break;}}
  check(bool(adapter),"hardware adapter available");if(!adapter)return 1;
  D3D_FEATURE_LEVEL level{};ComPtr<ID3D11Device> initial;ComPtr<ID3D11DeviceContext> initialContext;
  check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
      &initial,&level,&initialContext)==S_OK&&initial&&initialContext,"initial hardware device");
  if(!initial)return 1;
  DXGI_ADAPTER_DESC actual{};ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> actualAdapter;
  check(initial.As(&dxgi)==S_OK&&dxgi->GetAdapter(&actualAdapter)==S_OK&&actualAdapter->GetDesc(&actual)==S_OK,"initial hardware identity");
  const char* expectedRoute=expectedSeparateRoute();
  check(edvr::systemD3D11CreateDevice()!=nullptr,"systemD3D11CreateDevice returns valid function pointer");
  check(edvr::isSystemD3D11Pinned(),"isSystemD3D11Pinned evaluates to true once systemD3D11CreateDevice invoked");
  NativeDevice separate;check(separate.initializeSeparate(actual.AdapterLuid,level)==S_OK,"separate hardware device initialization");
  check(!std::strcmp(separate.separateModuleRoute(),expectedRoute)&&!_stricmp(separate.separateModulePath().c_str(),systemD3D11Path().c_str()),
        "separate hardware device came from the mapped System32 d3d11.dll");
  check(separate.device()&&separate.context()&&!identity(separate.device(),initial.Get())&&
        !identity(separate.context(),initialContext.Get()),"separate device and context are distinct COM identities");
  ComPtr<IDXGIDevice> separateDxgi;ComPtr<IDXGIAdapter> separateAdapter;DXGI_ADAPTER_DESC separateDesc{};
  const bool separateIdentity=separate.device()&&separate.device()->QueryInterface(IID_PPV_ARGS(&separateDxgi))==S_OK&&
    separateDxgi->GetAdapter(&separateAdapter)==S_OK&&separateAdapter->GetDesc(&separateDesc)==S_OK&&
    NativeDevice::matchesLuid(separateDesc.AdapterLuid,actual.AdapterLuid)&&separate.featureLevel()>=level&&
    !(separate.device()->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED);
  check(separateIdentity,"separate device matches adapter and feature level without SINGLETHREADED");
  check(separate.initializeSeparate(actual.AdapterLuid,level)==S_OK&&separate.device()&&separate.context(),"repeated separate initialization succeeds");
  NativeDevice invalidMinimum;check(invalidMinimum.initializeSeparate(actual.AdapterLuid,D3D_FEATURE_LEVEL_12_1)!=S_OK&&
      !invalidMinimum.device()&&!invalidMinimum.context(),"unsupported separate feature minimum leaves empty ownership");
  LUID missing{0xffffffffu,-1};NativeDevice missingDevice;
  check(missingDevice.initializeSeparate(missing,level)!=S_OK&&!missingDevice.device()&&!missingDevice.context(),"missing adapter LUID leaves empty ownership");
  separate.reset();separate.reset();check(!separate.device()&&!separate.context(),"separate reset is idempotent");
  check(edvr::isSystemD3D11Pinned(),"separate reset does not disturb process-wide pinned state");
  std::printf("native_device_hardware_test: %u checks, %u failures adapter=%ls luid=%08lx:%08lx\n",checks,fails,
      description.Description,static_cast<unsigned long>(actual.AdapterLuid.HighPart),static_cast<unsigned long>(actual.AdapterLuid.LowPart));
  return fails?1:0;
}

int wmain(int argc,wchar_t** argv) {
  if(argc!=2)return 2;std::wstring arg=argv[1];
  if(arg==L"--dry-run"){std::puts("native_device_test: dry-run (no WARP device)");return 0;}
  if(arg==L"--hardware")return hardwareTest();
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
      NativeDevice supplied;
      check(supplied.initializeExisting(device.Get(),desc.AdapterLuid,level)==S_OK&&
        supplied.device()==device.Get()&&supplied.context()==context.Get(),"existing device and immediate context retained exactly");
      check(FAILED(supplied.initializeExisting(device.Get(),foreign,level))&&!supplied.device()&&!supplied.context(),"existing mismatched device rejected without replacement");
      check(FAILED(supplied.initializeExisting(nullptr,desc.AdapterLuid,level))&&!supplied.device(),"null existing device rejected");
      NativeDevice separate;
      check(separate.initializeSeparate(desc.AdapterLuid,level)==S_OK,"separate WARP device initialization");
      check(edvr::systemD3D11CreateDevice()!=nullptr,"systemD3D11CreateDevice returns valid function pointer");
      check(edvr::isSystemD3D11Pinned(),"isSystemD3D11Pinned evaluates to true once systemD3D11CreateDevice invoked");
      separate.reset();
      check(!separate.device()&&!separate.context(),"separate reset clears device and context");
      check(edvr::isSystemD3D11Pinned(),"separate reset does not disturb process-wide pinned state");
    }
  }
  check(FAILED(NativeDevice::validate(nullptr,LUID{},D3D_FEATURE_LEVEL_10_0)),"null device rejected");
  NativeDevice native;
  check(native.initialize(LUID{},D3D_FEATURE_LEVEL_10_0,nullptr)==E_INVALIDARG&&!native.device()&&!native.context(),"null device factory rejected");
  check(FAILED(native.initialize(LUID{},D3D_FEATURE_LEVEL_12_1))&&!native.device()&&!native.context(),"failed initialize leaves empty ownership");
  check(!std::strcmp(native.separateModuleRoute(),"none")&&native.separateModulePath().empty(),"separate diagnostics start empty");
  const char* expectedRoute=expectedSeparateRoute();
  check(native.initializeSeparate(LUID{},D3D_FEATURE_LEVEL_12_1)==E_INVALIDARG&&
      !native.device()&&!native.context(),"separate unsupported minimum releases module reference");
  check(!std::strcmp(native.separateModuleRoute(),expectedRoute),"separate device takes the mapped System32 module before loading one");
  check(!_stricmp(native.separateModulePath().c_str(),systemD3D11Path().c_str()),"separate device names the System32 d3d11.dll it obtained");
  check(native.initializeSeparate(LUID{0xffffffffu,-1},D3D_FEATURE_LEVEL_10_0)==DXGI_ERROR_NOT_FOUND&&
      !native.device()&&!native.context(),"separate missing adapter fails without fallback");
  check(!std::strcmp(native.separateModuleRoute(),expectedRoute)&&!native.separateModulePath().empty(),"separate diagnostics survive a failed adapter lookup");
  std::printf("native_device_test: %u checks, %u failures\n",checks,fails);return fails?1:0;
}
