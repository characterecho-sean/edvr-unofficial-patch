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
       !owned(provider,candidate.setCullState)||!owned(provider,candidate.invalidate)||!owned(provider,candidate.close))return E_NOINTERFACE;
    table_=candidate;generation_=generation;legacyProvider_=false;return S_OK;
  }
  bool acquired()const{return table_.context!=nullptr;}
  HRESULT begin(const GeometryInput& geometry,uint64_t reference,EdvrNativeFrameOutput& out) {
    out=ask(legacyProvider_);
    if(!acquired())return S_FALSE;
    EdvrNativeFrameInput in{sizeof(in),EDVR_NATIVE_FRAME_VERSION_1};
    in.generation=generation_;in.referenceGeneration=reference;in.sequence=geometry.sequence;
    GeometrySnapshot snapshot{};in.valid=makeGeometrySnapshot(geometry,snapshot)?1u:0u;
    if(in.valid)std::memcpy(in.physicalHead,snapshot.headToLocal.m,sizeof(in.physicalHead));
    auto r=table_.beginFrame(table_.context,&in,&out);
    // A d3d11.dll from before the trim existed refuses the newer struct and
    // consumes nothing when it does, so the same frame can be asked again
    // the only way that one knows. It then carries no trim, ever.
    if(r==E_INVALIDARG&&!legacyProvider_) {
      out=ask(true);
      r=table_.beginFrame(table_.context,&in,&out);
      if(r==S_OK)legacyProvider_=true;
    }
    if(r!=S_OK||out.version<EDVR_NATIVE_FRAME_VERSION_2)
      out.trimOuterDeg=out.trimNasalDeg=out.trimVerticalDeg=0;
    return r;
  }
  bool legacyProvider() const{return legacyProvider_;}
  HRESULT latch(uint64_t sequence,EdvrNativeFrameDecision& out) {
    out={sizeof(out),EDVR_NATIVE_FRAME_VERSION_1};
    return acquired()?table_.latchSubmit(table_.context,sequence,&out):S_FALSE;
  }
  HRESULT cull(uint32_t stage,float width,float height) {
    return acquired()?table_.setCullState(table_.context,stage,width,height):S_FALSE;
  }
  HRESULT invalidate(){return acquired()?table_.invalidate(table_.context):S_FALSE;}
  HRESULT close(){if(!acquired())return S_FALSE;const auto r=table_.close(table_.context);if(SUCCEEDED(r)){table_={};legacyProvider_=false;}return r;}
 private:
  template<class T>static bool owned(HMODULE provider,T address) {
    if(!address)return false;HMODULE actual=nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(address),&actual)&&actual==provider;
  }
  static EdvrNativeFrameOutput ask(bool legacy) {
    return legacy?EdvrNativeFrameOutput{EDVR_NATIVE_FRAME_OUTPUT_SIZE_1,EDVR_NATIVE_FRAME_VERSION_1}
                 :EdvrNativeFrameOutput{sizeof(EdvrNativeFrameOutput),EDVR_NATIVE_FRAME_VERSION_2};
  }
  EdvrNativeFrameTable table_{};uint64_t generation_=0;bool legacyProvider_=false;
};
}
