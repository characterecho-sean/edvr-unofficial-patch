#include "../../src/openxr/device_gpu_timing.h"
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>
using Microsoft::WRL::ComPtr;
static unsigned checks=0,failures=0;
static void check(bool ok,const char* s){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",s);}}
#include "collector_cases.h"
static ComPtr<ID3D11Texture2D> makeTexture(ID3D11Device* d){D3D11_TEXTURE2D_DESC x{};x.Width=x.Height=32;x.ArraySize=x.MipLevels=x.SampleDesc.Count=1;x.Format=DXGI_FORMAT_R8G8B8A8_UNORM;x.Usage=D3D11_USAGE_DEFAULT;x.BindFlags=D3D11_BIND_SHADER_RESOURCE;ComPtr<ID3D11Texture2D> t;check(SUCCEEDED(d->CreateTexture2D(&x,nullptr,&t)),"texture");return t;}
int wmain(int argc,wchar_t** argv){
 if(argc!=2)return 2;if(!std::wcscmp(argv[1],L"--dry-run")){std::puts("native_device_gpu_test: dry-run (no WARP device)");return 0;}if(std::wcscmp(argv[1],L"--self-test"))return 2;
 wchar_t sys[MAX_PATH]{};check(GetSystemDirectoryW(sys,MAX_PATH)!=0,"system directory");HMODULE m=LoadLibraryW((std::wstring(sys)+L"\\d3d11.dll").c_str());check(m!=nullptr,"system D3D11");if(!m)return 1;
 auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(m,"D3D11CreateDevice"));ComPtr<ID3D11Device>d;ComPtr<ID3D11DeviceContext>ctx;D3D_FEATURE_LEVEL fl{};check(create&&SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&fl,&ctx)),"WARP device");if(!d)return 1;
 collectorCases(d.Get(),ctx.Get());
 edvr::openxr::DeviceGpuTiming timing;check(timing.initialize(d.Get(),ctx.Get()),"initialize separate timing device");auto a=makeTexture(d.Get()),b=makeTexture(d.Get());
 ComPtr<ID3D11DeviceContext> deferred;check(SUCCEEDED(d->CreateDeferredContext(0,&deferred)),"composition command context");
 check(timing.beginFrame(1,true),"begin frame CPU");
 // Consumer callbacks may arrive in reverse eye order; all four phases must
 // still share one disjoint scope.
 const unsigned order[4]={1,0,3,2};for(unsigned j=0;j<4;++j){const unsigned phase=order[j];
   ComPtr<ID3D11CommandList> list;
   if(phase>=2){deferred->CopyResource(b.Get(),a.Get());check(SUCCEEDED(deferred->FinishCommandList(FALSE,&list)),"composition list");}
   timing.beginGpuWork(phase,ctx.Get());
   if(list)ctx->ExecuteCommandList(list.Get(),TRUE);else ctx->CopyResource(phase&1?b.Get():a.Get(),phase&1?a.Get():b.Get());
   timing.endGpuWork(phase,ctx.Get());}
 check(timing.acceptFrame(1),"accept submitted frame");EdvrNativeDeviceGpuSample samples[8]{};check(timing.poll(samples,0)==0,"zero capacity does not consume");ctx->Flush();unsigned n=0;for(unsigned i=0;i<30&&!n;++i){Sleep(1);n=timing.poll(samples,8);}check(n==1,"poll completed sample");if(n)check(samples[0].status==EdvrNativeGpuValid,"valid WARP sample");
 check(timing.beginFrame(2,true),"second frame");timing.beginGpuWork(0,ctx.Get());timing.endGpuWork(0,ctx.Get());timing.invalidate();ctx->Flush();check(timing.poll(samples,8)==0,"invalidated partial frame suppressed");
 check(timing.beginFrame(3,false),"disable CPU toggle");timing.beginGpuWork(0,ctx.Get());check(timing.poll(samples,8)==0,"unaccepted disabled frame has no sample");check(timing.acceptFrame(99)==false,"wrong sequence rejected");
 check(timing.acceptFrame(3)&&timing.poll(samples,8)==1&&samples[0].status==EdvrNativeGpuDisabled,"disabled frame reports status");timing.abandon();timing.invalidate();
 // A partial final frame is abandoned with the device without context calls.
 { Ops o;DeviceTiming finalTiming;finalTiming.initialize(d.Get(),ctx.Get(),o.callbacks());finalTiming.beginFrame(1,true);finalTiming.beginGpuWork(0,ctx.Get());const auto before=o.commands;finalTiming.abandon();check(o.commands==before&&o.releases==o.queries.size(),"partial final abandon is Release only"); }
 std::printf("native_device_gpu_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
