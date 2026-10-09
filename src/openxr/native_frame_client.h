#pragma once
#include "../common/native_frame.h"
#include "geometry_snapshot.h"
#include <cstring>

namespace edvr::openxr {
class NativeFrameClient final {
 public:
  HRESULT acquire(HMODULE provider,ID3D11Device* device,uint64_t generation) {
    if(table_.context)return E_PENDING;
    if(!provider||!device||!generation)return E_INVALIDARG;
    auto entry=GetProcAddress(provider,"edvrAcquireNativeFrame");
    if(!owned(provider,entry))return E_NOINTERFACE;
    EdvrNativeFrameRequest request{sizeof(request),EDVR_NATIVE_FRAME_VERSION_1,device,generation};
    EdvrNativeFrameTable candidate{sizeof(candidate),EDVR_NATIVE_FRAME_VERSION_1};
    const auto r=reinterpret_cast<decltype(&edvrAcquireNativeFrame)>(entry)(&request,&candidate);
    if(r!=S_OK)return FAILED(r)?r:E_NOINTERFACE;
    if(candidate.size!=sizeof(candidate)||candidate.version!=EDVR_NATIVE_FRAME_VERSION_1||!candidate.context||
       !owned(provider,candidate.beginFrame)||!owned(provider,candidate.latchSubmit)||
       !owned(provider,candidate.invalidate)||!owned(provider,candidate.close))return E_NOINTERFACE;
    table_=candidate;generation_=generation;providerVersion_=EDVR_NATIVE_FRAME_VERSION_6;return S_OK;
  }
  bool acquired()const{return table_.context!=nullptr;}
  HRESULT begin(const GeometryInput& geometry,uint64_t reference,EdvrNativeFrameOutput& out) {
    out=ask(providerVersion_);
    if(!acquired())return S_FALSE;
    EdvrNativeFrameInput in{sizeof(in),EDVR_NATIVE_FRAME_VERSION_1};
    in.generation=generation_;in.referenceGeneration=reference;in.sequence=geometry.sequence;
    GeometrySnapshot snapshot{};in.valid=makeGeometrySnapshot(geometry,snapshot)?1u:0u;
    if(in.valid)std::memcpy(in.physicalHead,snapshot.headToLocal.m,sizeof(in.physicalHead));
    return beginLadder([&](EdvrNativeFrameOutput& o){return table_.beginFrame(table_.context,&in,&o);},providerVersion_,out);
  }
  // The version ladder, on its own so a rig can drive it with a provider of any age. A d3d11.dll that does not yet know a newer shape refuses it and
  // consumes nothing when it does, so the same frame can be asked again the only way that one knows: one step down at a time, until version 1 (which every
  // provider answers) or an acceptance settles what shape to ask for first next time. It then carries no trim/pacing/fade field, ever; whatever
  // the answer did not carry reads as its off value (fadeAlpha 0: no fade, never black; cantedEyeFix 0 and simulateCantDeg 0: the canted-display test keys off).
  template<class Begin>
  static HRESULT beginLadder(Begin&& call,uint32_t& providerVersion,EdvrNativeFrameOutput& out) {
    out=ask(providerVersion);
    auto r=call(out);
    while(r==E_INVALIDARG&&providerVersion>EDVR_NATIVE_FRAME_VERSION_1) {
      --providerVersion;
      out=ask(providerVersion);
      r=call(out);
    }
    if(r!=S_OK||out.version<EDVR_NATIVE_FRAME_VERSION_2)
      out.trimOuterDeg=out.trimNasalDeg=out.trimVerticalDeg=0;
    if(r!=S_OK||out.version<EDVR_NATIVE_FRAME_VERSION_3)
      out.deferredPacing=0;
    if(r!=S_OK||out.version<EDVR_NATIVE_FRAME_VERSION_5||!(out.fadeAlpha==out.fadeAlpha))
      out.fadeAlpha=0;
    if(r!=S_OK||out.version<EDVR_NATIVE_FRAME_VERSION_6||out.cantedEyeFix>1)
      out.cantedEyeFix=0;
    if(r!=S_OK||out.version<EDVR_NATIVE_FRAME_VERSION_6||!(out.simulateCantDeg>=0.0f&&out.simulateCantDeg<=15.0f))
      out.simulateCantDeg=0;
    return r;
  }
  HRESULT latch(uint64_t sequence,EdvrNativeFrameDecision& out) {
    out={sizeof(out),EDVR_NATIVE_FRAME_VERSION_1};
    return acquired()?table_.latchSubmit(table_.context,sequence,&out):S_FALSE;
  }
  HRESULT invalidate(){return acquired()?table_.invalidate(table_.context):S_FALSE;}
  HRESULT close(){if(!acquired())return S_FALSE;const auto r=table_.close(table_.context);if(SUCCEEDED(r)){table_={};providerVersion_=EDVR_NATIVE_FRAME_VERSION_6;}return r;}
  static EdvrNativeFrameOutput ask(uint32_t version) {
    if(version>=EDVR_NATIVE_FRAME_VERSION_6)
      return EdvrNativeFrameOutput{sizeof(EdvrNativeFrameOutput),EDVR_NATIVE_FRAME_VERSION_6};
    if(version==EDVR_NATIVE_FRAME_VERSION_5)
      return EdvrNativeFrameOutput{EDVR_NATIVE_FRAME_OUTPUT_SIZE_5,EDVR_NATIVE_FRAME_VERSION_5};
    if(version==EDVR_NATIVE_FRAME_VERSION_4)
      return EdvrNativeFrameOutput{EDVR_NATIVE_FRAME_OUTPUT_SIZE_4,EDVR_NATIVE_FRAME_VERSION_4};
    if(version==EDVR_NATIVE_FRAME_VERSION_3)
      return EdvrNativeFrameOutput{EDVR_NATIVE_FRAME_OUTPUT_SIZE_3,EDVR_NATIVE_FRAME_VERSION_3};
    if(version==EDVR_NATIVE_FRAME_VERSION_2)
      return EdvrNativeFrameOutput{EDVR_NATIVE_FRAME_OUTPUT_SIZE_2,EDVR_NATIVE_FRAME_VERSION_2};
    return EdvrNativeFrameOutput{EDVR_NATIVE_FRAME_OUTPUT_SIZE_1,EDVR_NATIVE_FRAME_VERSION_1};
  }
 private:
  template<class T>static bool owned(HMODULE provider,T address) {
    if(!address)return false;HMODULE actual=nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(address),&actual)&&actual==provider;
  }
  EdvrNativeFrameTable table_{};uint64_t generation_=0;
  // The shape (6/5/4/3/2/1) the provider last accepted; begin() asks this one
  // first, so a provider once found to be older is not re-probed every frame.
  uint32_t providerVersion_=EDVR_NATIVE_FRAME_VERSION_6;
};
}
