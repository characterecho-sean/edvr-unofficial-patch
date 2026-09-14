#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <string>
#include "../../src/common/native_sharpen.h"
#include "../../src/common/config.h"
#include "../../src/openxr/native_sharpen_client.h"
#pragma comment(linker, "/EXPORT:edvrAcquireNativeSharpen")
using Microsoft::WRL::ComPtr;
unsigned checks=0, failures=0, calls=0, lastEye=0;
float strength=0, lastBounds[4]{};
bool passSucceeds=true;
void check(bool ok,const char* message) {
  ++checks; if(!ok) { ++failures; std::printf("FAIL: %s\n",message); }
}
void require(bool ok,const char* message) { check(ok,message); if(!ok)throw std::runtime_error(message); }
extern "C" void* edvrSharpen(void* source,int eye,const float* bounds,float value) {
  ++calls; strength=value; lastEye=unsigned(eye);
  const float full[4]={0,0,1,1}; std::memcpy(lastBounds,bounds?bounds:full,sizeof(lastBounds));
  return passSucceeds?source:nullptr;
}
struct Device {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  Device() {
    wchar_t system[MAX_PATH]{};
    require(GetSystemDirectoryW(system,MAX_PATH)!=0,"system directory");
    auto module=LoadLibraryW((std::wstring(system)+L"\\d3d11.dll").c_str());
    require(module!=nullptr,"system D3D11 loads");
    auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice"));
    D3D_FEATURE_LEVEL level{};
    require(create&&SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&device,&level,&context)),"WARP device");
  }
  ComPtr<ID3D11Texture2D> texture(bool staging=false) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width=32; d.Height=24; d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    d.Usage=staging?D3D11_USAGE_STAGING:D3D11_USAGE_DEFAULT;
    d.CPUAccessFlags=staging?D3D11_CPU_ACCESS_READ:0;
    d.BindFlags=staging?0:D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> t;
    require(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&t)),"texture"); return t;
  }
};
EdvrNativeSharpenTable acquire(Device& d,uint64_t generation) {
  EdvrNativeSharpenRequest request{sizeof(request),1,d.device.Get(),generation};
  EdvrNativeSharpenTable table{sizeof(table),1};
  require(edvrAcquireNativeSharpen(&request,&table)==S_OK,"acquire"); return table;
}
ULONG refs(ID3D11Texture2D* texture) { const auto n=texture->AddRef(); texture->Release(); return n-1; }
HRESULT treat(EdvrNativeSharpenTable& t,uint64_t sequence,unsigned eye,ID3D11Texture2D* source,const float* bounds=nullptr) {
  ID3D11Texture2D* output=source; float box[4]={9,9,9,9};
  const auto before=source?refs(source):0;
  const auto hr=t.treatEye(t.context,sequence,eye,source,bounds,&output,box);
  if(hr==S_OK) {
    check(output==source,"stub output returned");
    check(refs(source)==before+1,"provider returns exactly one owned reference");
    check(box[0]==(bounds&&bounds[0]>bounds[2]?1.f:0.f)&&box[2]==1-box[0]&&
        box[1]==(bounds&&bounds[1]>bounds[3]?1.f:0.f)&&box[3]==1-box[1],"full output bounds preserve flips");
    output->Release(); check(refs(source)==before,"output reference released");
  } else check(!output&&box[0]==0&&box[1]==0&&box[2]==0&&box[3]==0,"no output on passthrough or failure");
  return hr;
}
void run() {
  Device device,foreignDevice;
  auto source=device.texture(), foreign=foreignDevice.texture(), staging=device.texture(true);
  auto& cfg=edvr::Config::get(); cfg.set("fix.render_sharpness","0.7");
  auto t=acquire(device,9); check(edvr::nativeSharpenActive(),"native availability after acquire");
  check(treat(t,1,1,source.Get())==S_OK&&lastEye==1&&std::fabs(strength-.7f)<1e-5f,"right first uses setting");
  cfg.set("fix.render_sharpness","0.1");
  check(treat(t,1,0,source.Get())==S_OK&&std::fabs(strength-.7f)<1e-5f,"setting frozen across stereo pair");
  check(treat(t,1,1,source.Get())==E_INVALIDARG,"duplicate eye rejected");
  check(treat(t,2,0,source.Get())==S_OK&&std::fabs(strength-.1f)<1e-5f,"next sequence starts with previously consumed eye");
  check(treat(t,2,1,source.Get())==S_OK,"complete consecutive pair");
  const auto beforeInvalid=calls;
  const float invalid[][4]={{0,0,1,NAN},{0,0,INFINITY,1},{-.1f,0,1,1},{0,0,1.1f,1},{0,0,0,1}};
  for(const auto& b:invalid)check(treat(t,3,0,source.Get(),b)==E_INVALIDARG,"invalid bounds rejected");
  check(treat(t,3,0,foreign.Get())==E_INVALIDARG,"wrong device rejected");
  check(treat(t,3,0,staging.Get())==E_INVALIDARG,"staging input rejected");
  check(treat(t,3,2,source.Get())==E_INVALIDARG,"invalid eye rejected");
  check(treat(t,0,0,source.Get())==E_INVALIDARG,"zero sequence rejected");
  check(treat(t,1,0,source.Get())==E_INVALIDARG,"old sequence rejected");
  check(treat(t,3,0,nullptr)==E_INVALIDARG,"null source rejected");
  HRESULT wrong=S_OK;
  std::thread worker([&]{wrong=treat(t,3,0,source.Get());}); worker.join();
  check(wrong==E_INVALIDARG&&calls==beforeInvalid,"invalid inputs never invoke pass");
  cfg.set("fix.render_sharpness","2");
  const float flipped[4]={.75f,1,.25f,0};
  check(treat(t,3,0,source.Get(),flipped)==S_OK&&strength==1&&
      !std::memcmp(lastBounds,flipped,sizeof(flipped)),"rejected inputs do not latch; flipped region reaches shader");
  check(t.invalidate(t.context)==S_OK,"invalidate");
  check(treat(t,3,1,source.Get())==E_INVALIDARG,"invalidated sequence stays retired");
  check(treat(t,4,1,source.Get())==S_OK,"newer sequence recovers");
  uint64_t sequence=5;
  for(const char* setting:{"0","-1","nan","inf"}) {
    cfg.set("fix.render_sharpness",setting); const auto before=calls;
    check(treat(t,sequence,0,source.Get())==S_FALSE&&treat(t,sequence,1,source.Get())==S_FALSE&&calls==before,
        "zero negative and nonfinite strengths skip shader for both eyes"); ++sequence;
  }
  cfg.set("fix.render_sharpness","0.5");
  check(treat(t,sequence++,0,source.Get())==S_OK&&strength==.5f,"re-enable after disabled frames");
  passSucceeds=false; const auto beforeRefusal=calls;
  check(treat(t,sequence,0,source.Get())==S_FALSE&&calls==beforeRefusal+1,"shader refusal safely passes through");
  passSucceeds=true;
  check(treat(t,sequence,1,source.Get())==S_FALSE&&treat(t,sequence+1,0,source.Get())==S_FALSE&&calls==beforeRefusal+1,
      "shader refusal stands down subsequent eyes and frames");
  HRESULT closed=E_FAIL;
  std::thread cpu([&]{closed=t.close(t.context);});cpu.join();
  check(closed==S_OK&&!edvr::nativeSharpenActive(),"CPU close clears availability");
  check(t.close(t.context)==S_FALSE,"idempotent close");
  auto fresh=acquire(device,10);
  check(treat(t,100,0,source.Get())==E_INVALIDARG,"old table cannot address fresh session");
  check(t.close(t.context)==S_FALSE&&edvr::nativeSharpenActive(),"stale close cannot retire fresh session");
  check(treat(fresh,1,0,source.Get())==S_OK,"new session resets stand-down and sequence");
  check(fresh.close(fresh.context)==S_OK,"fresh close");
  edvr::openxr::NativeSharpenClient client;
  require(client.acquire(GetModuleHandleW(nullptr),device.device.Get(),11)==S_OK,"client validates same-module export table");
  ComPtr<ID3D11Texture2D> result; vr::VRTextureBounds_t box{};
  check(client.treat(1,0,source.Get(),nullptr,result,box)==S_OK&&result.Get()==source.Get(),"client retains output");
  result.Reset(); check(client.invalidate()==S_OK&&client.close()==S_OK,"client lifecycle");
  check(client.treat(2,0,source.Get(),nullptr,result,box)==S_FALSE&&!result&&box.uMax==0,"closed client passthrough");
}
int wmain(int argc,wchar_t** argv) {
  SetErrorMode(3);
  if(argc==2&&!wcscmp(argv[1],L"--dry-run")){std::puts("native_sharpen_test: dry-run (no device or files)");return 0;}
  if(argc!=2||wcscmp(argv[1],L"--self-test"))return 2;
  try{run();}catch(const std::exception& e){std::printf("FAIL: setup aborted: %s\n",e.what());return 1;}
  std::printf("native_sharpen_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
