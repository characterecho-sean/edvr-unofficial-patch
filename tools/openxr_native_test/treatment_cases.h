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
  // healPair models mode 1 (target eye 0) only: which eyes treatEye has
  // snapshotted this sequence, and whether the heal was already delivered.
  uint64_t fssSequence=0; unsigned fssEyes=0; bool fssDelivered=false;
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
inline HRESULT WINAPI fss(void*,uint64_t seq,uint32_t eye,ID3D11Texture2D* src,const float*,ID3D11Texture2D** out) {
  // treatEye only snapshots now: always S_FALSE with a null output, unless
  // the scenario fails this stage.
  state.wrongThread|=GetCurrentThreadId()!=state.thread;
  state.calls+='F';state.inputs.push_back(src);if(out)*out=nullptr;
  if(state.fail=='F')return E_FAIL;
  if(state.fssSequence!=seq){state.fssSequence=seq;state.fssEyes=0;state.fssDelivered=false;}
  state.fssEyes|=1u<<eye;
  return S_FALSE;
}
inline HRESULT WINAPI healPair(void*,uint64_t,uint32_t* eye,ID3D11Texture2D** out) {
  // Not recorded in calls. Models mode 1 (target eye 0) only: S_FALSE until
  // eye 0 is snapshotted, E_PENDING until eye 1 also is, then S_OK once.
  if(out)*out=nullptr;if(eye)*eye=0;
  if(!state.heal)return S_FALSE;
  if(!(state.fssEyes&1u))return S_FALSE;
  if(!(state.fssEyes&2u))return E_PENDING;
  if(state.fssDelivered)return S_FALSE;
  state.fssDelivered=true;
  *out=state.outputs[0].Get();(*out)->AddRef();
  return S_OK;
}
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
  *table={sizeof(*table),EDVR_NATIVE_FSS_VERSION_2,&state,beginFss,fss,healPair,invalidate,close};return S_OK;
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
  // System32's d3d11, not an import: this exe sits in build\ beside EDVR's proxy.
  const auto createDevice=systemD3D11CreateDevice();
  check(createDevice&&SUCCEEDED(createDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"treatment WARP device");
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
  // The host seeds both at wait-frame; a fixture that hand-builds the pair
  // has to as well, or the menu is published half a stereo geometry.
  h.frameContentViews[0]=h.frameViews[0];h.frameContentViews[1]=h.frameViews[1];
  check(f.initialized&&SUCCEEDED(h.captured.initialize(device.Get())),"host treatment capture initialized");
  const auto provider=GetModuleHandleW(nullptr);
  check(h.fss.acquire(provider,device.Get(),1)==S_OK&&h.temporal.acquire(provider,device.Get(),1)==S_OK&&
    h.sharpen.acquire(provider,device.Get(),1)==S_OK&&h.menu.acquire(provider,device.Get(),1)==S_OK,"host uses validated provider tables");
  check(f.route.bind()&&f.owner.start(),"host treatment rendezvous started");
  const vr::Texture_t texture{source.Get(),vr::API_DirectX,vr::ColorSpace_Gamma};
  const vr::VRTextureBounds_t bounds{.75f,.875f,.125f,.25f};
  const auto capturedPixel=[&](vr::EVREye eye) {
    auto* image=h.captured.texture(eye);if(!image)return uint32_t(0);
    auto td=desc;td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;td.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;
    ComPtr<ID3D11Texture2D> read;if(FAILED(device->CreateTexture2D(&td,nullptr,&read)))return uint32_t(0);
    context->CopyResource(read.Get(),image);D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped)))return uint32_t(0);
    const auto value=*static_cast<const uint32_t*>(mapped.pData);context->Unmap(read.Get(),0);return value;
  };
  for(unsigned scenario=0;scenario<8;++scenario) {
    state.calls.clear();state.inputs.clear();state.passthrough=scenario==1;state.heal=scenario==2;
    state.fail=scenario==3?'F':scenario==4?'T':scenario==5?'S':scenario==6?'P':scenario==7?'M':0;
    const auto before=h.graphicsCalls.calls;const auto oldPixel=capturedPixel(vr::Eye_Left);
    vr::EVRCompositorError result=vr::VRCompositorError_InvalidTexture;
    check(f.route.invoke([&]{
      h.activeFrameSequence=100+scenario;
      h.frameTangentShift[0][0]=.02f;h.frameTangentShift[0][1]=-.01f;
      result=h.capture(vr::Eye_Left,&texture,&bounds,vr::Submit_Default,true);
    }),"host capture completed through owner/render rendezvous");
    if(scenario==2) {
      // The two-eye heal: the left eye's fss heal comes back E_PENDING (the
      // right has not been treated this frame), so its sharpen/menu/capture
      // defer to the right eye's own submit.
      check(result==vr::VRCompositorError_None&&state.calls=="TF"&&h.graphicsCalls.calls-before==2,
        "left treatment runs temporal and the fss snapshot, then waits: no copy callback yet");
      check(capturedPixel(vr::Eye_Left)==oldPixel,"a deferred left eye copies nothing this call");
      check(h.deferredEye.active,"the left eye is held for the right eye's submit");
      const auto beforeRight=h.graphicsCalls.calls;
      check(f.route.invoke([&]{
        result=h.capture(vr::Eye_Right,&texture,&bounds,vr::Submit_Default,true);
      }),"host right capture completes the pending heal");
      check(result==vr::VRCompositorError_None&&state.calls=="TFTFSPMSPM",
        "right treatment runs temporal, fss, then finishes the deferred left before its own");
      check(h.graphicsCalls.calls-beforeRight==3,
        "right capture uses validation, one batched treatment, one copy callback carrying both eyes");
      check(capturedPixel(vr::Eye_Left)==0xff000013u&&capturedPixel(vr::Eye_Right)==0xff000013u,
        "both eyes land the sharpen/menu chain's final output");
      check(!h.deferredEye.active&&h.fssDeferredEyes==1,"the deferral is consumed exactly once");
      check(state.inputs.size()>7&&
        state.inputs[0]==source.Get()&&state.inputs[1]==state.outputs[1].Get()&&
        state.inputs[2]==source.Get()&&state.inputs[3]==state.outputs[1].Get()&&
        state.inputs[4]==state.outputs[0].Get()&&state.inputs[5]==state.outputs[2].Get()&&
        state.inputs[6]==state.outputs[1].Get()&&state.inputs[7]==state.outputs[2].Get(),
        "left reads the heal, right reads its own temporal output, both sharpen before menu");
      continue;
    }
    // Temporal first, then the FSS snapshot (never a skip 'H': a healed eye
    // keeps its temporal frame since 2026-09-16), then sharpen, publish,
    // menu; a failure stops the chain where it happens.
    const char* expected[]={"TFSPM","TFSPM","-","TF","T","TFS","TFSP","TFSPM"};
    check(state.calls==expected[scenario]&&!state.wrongThread,"all treatments ordered on the bound producer, stopping at failure");
    if(scenario<2) {
      check(result==vr::VRCompositorError_None&&h.graphicsCalls.calls-before==3,
        "borrowed capture uses validation, one batched treatment, one copy callback");
      check(capturedPixel(vr::Eye_Left)==(scenario==1?0xff000088u:0xff000013u),"capture snapshots the selected final writer");
      if(scenario==1) {
        const auto outputBounds=h.captured.bounds(vr::Eye_Left);
        check(std::memcmp(&bounds,&outputBounds,sizeof(bounds))==0,"passthrough preserves cropped/flipped input bounds");
        check(std::fabs(std::tan(h.frameViews[0].fov.angleLeft)-std::tan(h.frameGeometry.views[0].fov.angleLeft)-.02f)<.00001f,
          "passthrough retains rendered jitter despite spatial passthrough callbacks");
        check(state.inputs.size()>1&&state.inputs[1]==source.Get(),"with no temporal output the heal reads the raw source");
      } else {
        check(state.inputs.size()>1&&state.inputs[1]==state.outputs[1].Get(),"the heal reads the temporal output, not the raw source");
        check(state.inputs.size()>2&&state.inputs[2]==state.outputs[1].Get(),
          "sharpen follows the temporal output and menu follows sharpen");
      }
    } else {
      check(result==vr::VRCompositorError_InvalidTexture&&h.graphicsCalls.calls-before==2&&capturedPixel(vr::Eye_Left)==oldPixel,
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

template<class Check> void runDeferredTreatmentCases(Check check) {
  using namespace treatment_fixture;
  state={};state.thread=GetCurrentThreadId();
  ComPtr<ID3D11Device> producer,consumer;ComPtr<ID3D11DeviceContext> producerContext,consumerContext;
  const auto createDevice=systemD3D11CreateDevice();
  check(createDevice&&SUCCEEDED(createDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&producer,nullptr,&producerContext))&&
    SUCCEEDED(createDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&consumer,nullptr,&consumerContext)),"host handoff devices");
  if(!producer||!consumer)return;
  ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapterDesc{};
  check(SUCCEEDED(consumer.As(&dxgi))&&SUCCEEDED(dxgi->GetAdapter(&adapter))&&SUCCEEDED(adapter->GetDesc(&adapterDesc)),"host handoff adapter");
  D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=4;desc.MipLevels=desc.ArraySize=1;
  desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  for(auto& output:state.outputs) {
    check(SUCCEEDED(producer->CreateTexture2D(&desc,nullptr,&output)),"host handoff treatment allocation");
    if(!output)return;
  }
  uint32_t pixels[16];std::fill_n(pixels,16,0xff123456u);
  producerContext->UpdateSubresource(state.outputs[3].Get(),0,nullptr,pixels,16,0);
  launch_fixture::Fixture f;auto& h=f.host;
  h.startupOptions.separateDevice=true;h.externalDevice=producer.Get();
  h.frameViews[0]=h.frameGeometry.views[0];h.frameViews[1]=h.frameGeometry.views[1];
  // The host seeds both at wait-frame; a fixture that hand-builds the pair
  // has to as well, or the menu is published half a stereo geometry.
  h.frameContentViews[0]=h.frameViews[0];h.frameContentViews[1]=h.frameViews[1];
  const auto provider=GetModuleHandleW(nullptr);
  check(h.fss.acquire(provider,producer.Get(),1)==S_OK&&h.temporal.acquire(provider,producer.Get(),1)==S_OK&&
    h.sharpen.acquire(provider,producer.Get(),1)==S_OK&&h.menu.acquire(provider,producer.Get(),1)==S_OK,"host handoff providers");
  check(f.route.bind()&&f.owner.start(),"host handoff route");
  bool initialized=false;
  check(f.route.invoke([&]{initialized=h.graphics.initializeExisting(consumer.Get(),adapterDesc.AdapterLuid,D3D_FEATURE_LEVEL_11_0)==S_OK&&
    h.captured.initializeShared(producer.Get(),consumer.Get(),&h.graphicsCalls)==S_OK;} )&&initialized,"host capture owns separate XR device");
  if(!initialized){f.owner.stop();return;}
  const vr::Texture_t texture{state.outputs[0].Get(),vr::API_DirectX,vr::ColorSpace_Gamma};
  check(f.route.invoke([&]{h.activeFrameSequence=1;check(h.capture(vr::Eye_Left,&texture,nullptr,vr::Submit_Default,true)==vr::VRCompositorError_None&&
    h.captured.hasPending()&&!h.captured.texture(vr::Eye_Left),"actual host first eye does not wait for consumer");}),"host first eye returns");
  std::fill_n(pixels,16,0xffabcdefu);producerContext->UpdateSubresource(state.outputs[3].Get(),0,nullptr,pixels,16,0);
  check(f.route.invoke([&]{check(h.capture(vr::Eye_Right,&texture,nullptr,vr::Submit_Default,true)==vr::VRCompositorError_None&&
    h.captured.hasPending()&&!h.captured.texture(vr::Eye_Right),"actual host second eye also publishes producer-owned pixels");}),"host second eye returns");
  const auto calls=h.graphicsCalls.calls;f.dispatcher.close();
  check(f.owner.invoke([&]{
    check(h.captured.completePending()==S_OK,"host pair can consume after render admission closes");
    auto stagingDesc=desc;stagingDesc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;stagingDesc.BindFlags=0;
    stagingDesc.Usage=D3D11_USAGE_STAGING;stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;check(SUCCEEDED(consumer->CreateTexture2D(&stagingDesc,nullptr,&staging)),"host snapshot readback");
    if(staging)for(unsigned e=0;e<2;++e) {
      auto* image=h.captured.texture(vr::EVREye(e));
      check(image!=nullptr,"host snapshot published");if(!image)continue;
      consumerContext->CopyResource(staging.Get(),image);
      D3D11_MAPPED_SUBRESOURCE mapped{};const bool mappedOk=SUCCEEDED(consumerContext->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
      check(mappedOk,"host snapshot mapped");
      if(mappedOk){check(*static_cast<uint32_t*>(mapped.pData)==(e?0xffabcdefu:0xff123456u),"host preserves per-eye output before producer reuse");consumerContext->Unmap(staging.Get(),0);}
    }
    check(h.captured.shutdownShared()==S_OK,"host pending transfers retire without callbacks");h.graphics.reset();
  })&&h.graphicsCalls.calls==calls,"host consumption and shutdown never reenter stopped producer");
  check(f.owner.stop(),"host handoff owner stopped");
  h.fss.close();h.temporal.close();h.sharpen.close();h.menu.close();state={};
}

template<class Check> void runSubmissionStatsCases(Check check) {
  SubmissionStats stats;SubmissionStats::Sample sample{};
  check(stats.advance(1000)&&stats.window()==1,"initial periodic window armed");
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
  check(!stats.advance(30999)&&stats.full(),"periodic window remains bounded before its deadline");
  check(stats.advance(31000)&&!stats.full()&&stats.count()==0&&stats.window()==2,"periodic window rearms after 30 seconds");
  for(unsigned n=0;n<SubmissionStats::warmup+1;++n){++sample.sequence;stats.add(sample);}
  check(stats.count()==1,"rearmed window warms up before sampling");
  ++sample.sequence;++sample.outputWidth[0];stats.add(sample);
  check(stats.count()==0,"postprocess output resize restarts warmup");
  for(unsigned n=0;n<SubmissionStats::warmup;++n){++sample.sequence;stats.add(sample);}
  check(stats.count()==1,"stable output can collect again");
  sample.sequence+=2;stats.add(sample);check(stats.count()==0,"missing/withheld frame restarts warmup");
  for(unsigned n=0;n<SubmissionStats::warmup;++n){++sample.sequence;stats.add(sample);}
  ++sample.sequence;++sample.treatments[1];stats.add(sample);check(stats.count()==0,"changed treatment mask restarts warmup");
  SubmissionStats changed;
  for(unsigned n=1;n<=SubmissionStats::warmup+1;++n){sample.sequence=n;changed.add(sample);}
  check(changed.count()==1,"second fixture has a sample");
  ++sample.sequence;sample.width[0]=101;check(!changed.add(sample)&&changed.count()==0,"size change restarts warmup instead of mixing resolutions");
  sample.sequence=0;check(!changed.add(sample)&&changed.count()==0,"invalid sequence excluded");
}
} // namespace edvr::openxr::test
