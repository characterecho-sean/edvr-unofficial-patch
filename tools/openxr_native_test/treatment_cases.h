#pragma once
#include "launch_centre_cases.h"
#include <vector>
#include <string>

// Private provider fixture exported by the test EXE, never the game DLLs.
// Exercise the actual host dispatcher and client-table validation without an
// OpenXR runtime, using distinct outputs to detect reordered/skipped stages.
namespace edvr::openxr::test::treatment_fixture {
using Microsoft::WRL::ComPtr;
struct State {
  DWORD thread=0;
  bool wrongThread=false, passthrough=false, heal=false;
  char fail=0;
  unsigned invalidations=0;
  std::string calls;
  std::vector<ID3D11Texture2D*> inputs;
  ComPtr<ID3D11Texture2D> outputs[4];
} inline state;
inline HRESULT WINAPI beginFss(void*,const EdvrNativeFssFrame*){return S_OK;}
inline HRESULT WINAPI beginTemporal(void*,const EdvrNativeTemporalFrame*,EdvrNativeTemporalProjection*){return S_OK;}
inline HRESULT WINAPI projection(void*,uint64_t,uint32_t,float,float){return S_OK;}
inline HRESULT WINAPI invalidate(void*){++state.invalidations;return S_OK;}
inline HRESULT WINAPI close(void*){return S_OK;}
inline HRESULT WINAPI skip(void*,uint64_t,uint32_t,uint32_t,uint32_t){state.calls+='H';return S_OK;}
inline HRESULT output(char stage,unsigned index,ID3D11Texture2D* input,ID3D11Texture2D** out,float* box) {
  state.wrongThread|=GetCurrentThreadId()!=state.thread;
  state.calls+=stage;state.inputs.push_back(input);*out=nullptr;
  if(stage==state.fail)return E_FAIL;
  if(state.passthrough||(stage=='F'&&!state.heal))return S_FALSE;
  *out=state.outputs[index].Get();(*out)->AddRef();
  if(box){box[0]=box[1]=0;box[2]=box[3]=1;}
  return S_OK;
}
inline HRESULT WINAPI fss(void*,uint64_t,uint32_t,ID3D11Texture2D* src,const float*,ID3D11Texture2D** out){return output('F',0,src,out,nullptr);}
inline HRESULT WINAPI temporal(void*,uint64_t,uint32_t,ID3D11Texture2D* src,const float*,ID3D11Texture2D** out,float* box){return output('T',1,src,out,box);}
inline HRESULT WINAPI sharpen(void*,uint64_t,uint32_t,ID3D11Texture2D* src,const float*,ID3D11Texture2D** out,float* box){return output('S',2,src,out,box);}
inline HRESULT WINAPI menu(void*,uint32_t,ID3D11Texture2D* src,const float*,ID3D11Texture2D** out,float* box){return output('M',3,src,out,box);}
inline HRESULT WINAPI publish(void*,const float* head,const float (*)[12],const float (*)[4],uint64_t,uint64_t) {
  if(!head){++state.invalidations;return S_OK;}
  state.wrongThread|=GetCurrentThreadId()!=state.thread;state.calls+='P';return state.fail=='P'?E_FAIL:S_OK;
}
} // namespace edvr::openxr::test::treatment_fixture

#pragma comment(linker, "/export:edvrAcquireNativeFss")
#pragma comment(linker, "/export:edvrAcquireNativeTemporal")
#pragma comment(linker, "/export:edvrAcquireNativeSharpen")
#pragma comment(linker, "/export:edvrAcquireNativeMenu")
extern "C" HRESULT WINAPI edvrAcquireNativeFss(const EdvrNativeFssRequest*,EdvrNativeFssTable* table) {
  using namespace edvr::openxr::test::treatment_fixture;
  *table={sizeof(*table),EDVR_NATIVE_FSS_VERSION_1,&state,beginFss,fss,invalidate,close};return S_OK;
}
extern "C" HRESULT WINAPI edvrAcquireNativeTemporal(const EdvrNativeTemporalRequest*,EdvrNativeTemporalTable* table) {
  using namespace edvr::openxr::test::treatment_fixture;
  *table={sizeof(*table),EDVR_NATIVE_TEMPORAL_VERSION_1,&state,beginTemporal,projection,temporal,invalidate,close,skip};return S_OK;
}
extern "C" HRESULT WINAPI edvrAcquireNativeSharpen(const EdvrNativeSharpenRequest*,EdvrNativeSharpenTable* table) {
  using namespace edvr::openxr::test::treatment_fixture;
  *table={sizeof(*table),EDVR_NATIVE_SHARPEN_VERSION_1,&state,sharpen,invalidate,close};return S_OK;
}
extern "C" HRESULT WINAPI edvrAcquireNativeMenu(const EdvrNativeMenuRequest*,EdvrNativeMenuTable* table) {
  using namespace edvr::openxr::test::treatment_fixture;
  *table={sizeof(*table),EDVR_NATIVE_MENU_VERSION_1,&state,publish,menu,close};return S_OK;
}

namespace edvr::openxr::test {
template<class Check> void runTreatmentCases(Check check) {
  using namespace treatment_fixture;
  state={};state.thread=GetCurrentThreadId();
  ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
  check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"treatment WARP device");
  if(!device)return;
  D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=4;desc.MipLevels=desc.ArraySize=1;
  desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  for(unsigned i=0;i<4;++i) {
    const uint32_t color=0xff000010u+i;uint32_t pixels[16];std::fill_n(pixels,16,color);
    D3D11_SUBRESOURCE_DATA data{pixels,16,0};
    check(SUCCEEDED(device->CreateTexture2D(&desc,&data,&state.outputs[i])),"distinct treatment output");
    if(!state.outputs[i])return;
  }
  ComPtr<ID3D11Texture2D> source;
  uint32_t pixels[16];std::fill_n(pixels,16,0xff000088u);D3D11_SUBRESOURCE_DATA data{pixels,16,0};
  check(SUCCEEDED(device->CreateTexture2D(&desc,&data,&source)),"treatment source");if(!source)return;
  launch_fixture::Fixture f;auto& h=f.host;
  h.frameViews[0]=h.frameGeometry.views[0];h.frameViews[1]=h.frameGeometry.views[1];
  check(f.initialized&&SUCCEEDED(h.captured.initialize(device.Get())),"host treatment capture initialized");
  const auto provider=GetModuleHandleW(nullptr);
  check(h.fss.acquire(provider,device.Get(),1)==S_OK&&h.temporal.acquire(provider,device.Get(),1)==S_OK&&
    h.sharpen.acquire(provider,device.Get(),1)==S_OK&&h.menu.acquire(provider,device.Get(),1)==S_OK,"host uses validated provider tables");
  check(f.route.bind()&&f.owner.start(),"host treatment rendezvous started");
  const vr::Texture_t texture{source.Get(),vr::API_DirectX,vr::ColorSpace_Gamma};
  const vr::VRTextureBounds_t bounds{.75f,.875f,.125f,.25f};
  const auto capturedPixel=[&] {
    auto* image=h.captured.texture(vr::Eye_Left);if(!image)return uint32_t(0);
    auto td=desc;td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;td.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;
    ComPtr<ID3D11Texture2D> read;if(FAILED(device->CreateTexture2D(&td,nullptr,&read)))return uint32_t(0);
    context->CopyResource(read.Get(),image);D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped)))return uint32_t(0);
    const auto value=*static_cast<const uint32_t*>(mapped.pData);context->Unmap(read.Get(),0);return value;
  };
  for(unsigned scenario=0;scenario<8;++scenario) {
    state.calls.clear();state.inputs.clear();state.passthrough=scenario==1;state.heal=scenario==2;
    state.fail=scenario==3?'F':scenario==4?'T':scenario==5?'S':scenario==6?'P':scenario==7?'M':0;
    const auto before=h.graphicsCalls.calls;const auto oldPixel=capturedPixel();
    vr::EVRCompositorError result=vr::VRCompositorError_InvalidTexture;
    check(f.route.invoke([&]{
      h.activeFrameSequence=100+scenario;
      h.frameTangentShift[0][0]=.02f;h.frameTangentShift[0][1]=-.01f;
      result=h.capture(vr::Eye_Left,&texture,&bounds,vr::Submit_Default,true);
    }),"host capture completed through owner/render rendezvous");
    const char* expected[]={"FTSPM","FTSPM","FHSPM","F","FT","FTS","FTSP","FTSPM"};
    check(state.calls==expected[scenario]&&!state.wrongThread,"all treatments ordered on the bound producer, stopping at failure");
    if(scenario<3) {
      check(result==vr::VRCompositorError_None&&h.graphicsCalls.calls-before==3,
        "borrowed capture uses validation, one batched treatment, one copy callback");
      check(capturedPixel()==(scenario==1?0xff000088u:0xff000013u),"capture snapshots the selected final writer");
      if(scenario==1) {
        const auto outputBounds=h.captured.bounds(vr::Eye_Left);
        check(std::memcmp(&bounds,&outputBounds,sizeof(bounds))==0,"passthrough preserves cropped/flipped input bounds");
        check(std::fabs(std::tan(h.frameViews[0].fov.angleLeft)-std::tan(h.frameGeometry.views[0].fov.angleLeft)-.02f)<.00001f,
          "passthrough retains rendered jitter despite spatial passthrough callbacks");
      } else {
        const unsigned sharpenIndex=scenario==2?1:2;
        check(state.inputs.size()>sharpenIndex+1&&state.inputs[sharpenIndex]==state.outputs[scenario==2?0:1].Get()&&state.inputs[sharpenIndex+1]==state.outputs[2].Get(),
          "sharpen follows temporal/healed output and menu follows sharpen");
      }
    } else {
      check(result==vr::VRCompositorError_InvalidTexture&&h.graphicsCalls.calls-before==2&&capturedPixel()==oldPixel,
        "failed treatment never copies partial or stale output");
    }
  }
  state.fail=0;state.calls.clear();const auto before=h.graphicsCalls.calls;
  check(f.route.invoke([&]{check(h.capture(vr::Eye_Left,&texture,&bounds,vr::Submit_Default,false)==vr::VRCompositorError_None,
    "nonrendering submit validates");}),"nonrendering rendezvous");
  check(state.calls.empty()&&h.graphicsCalls.calls-before==1,"nonrendering submit does not run treatments or copy");
  check(f.owner.stop(),"treatment owner stops without deferred callbacks");
  f.dispatcher.close();h.fss.close();h.temporal.close();h.sharpen.close();h.menu.close();h.captured.shutdown();state={};
}

template<class Check> void runSubmissionStatsCases(Check check) {
  SubmissionStats stats;SubmissionStats::Sample sample{};
  sample.width[0]=sample.width[1]=100;sample.height[0]=sample.height[1]=120;
  sample.callbacks=6;sample.dispatchMs=2;sample.treatmentMs=1;
  for(unsigned n=1;n<=SubmissionStats::warmup+SubmissionStats::capacity;++n) {
    sample.sequence=n;sample.submitMs=double(n-SubmissionStats::warmup);
    if(n<=SubmissionStats::warmup)sample.submitMs=0;
    const bool ready=stats.add(sample);
    check(ready==(n==SubmissionStats::warmup+SubmissionStats::capacity),"bounded report only fires when full");
  }
  const auto d=stats.distribution(&SubmissionStats::Sample::submitMs);
  check(stats.count()==256&&d.p50==128&&d.p95==244&&d.p99==254&&stats.meanCallbacks()==6,
    "window percentiles exclude warmup and count callbacks per stereo pair");
  ++sample.sequence;check(!stats.add(sample)&&stats.count()==256,"full window never samples or reports again");
  SubmissionStats changed;
  for(unsigned n=1;n<=SubmissionStats::warmup+1;++n){sample.sequence=n;changed.add(sample);}
  check(changed.count()==1,"second fixture has a sample");
  ++sample.sequence;sample.width[0]=101;check(!changed.add(sample)&&changed.count()==0,"size change restarts warmup instead of mixing resolutions");
  sample.sequence=0;check(!changed.add(sample)&&changed.count()==0,"invalid sequence excluded");
}
} // namespace edvr::openxr::test
