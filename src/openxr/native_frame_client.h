#pragma once
#include "../common/native_frame.h"
#include "geometry_snapshot.h"
#include "now_head_publication.h"   // F31: the "now" head the version 2 input carries
#include "native_trace.h"
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
    table_=candidate;generation_=generation;providerVersion_=EDVR_NATIVE_FRAME_VERSION_8;return S_OK;
  }
  bool acquired()const{return table_.context!=nullptr;}
  HRESULT begin(const GeometryInput& geometry,uint64_t reference,EdvrNativeFrameOutput& out) {
    out=ask(providerVersion_);
    if(!acquired())return S_FALSE;
    EdvrNativeFrameInput in{sizeof(in),EDVR_NATIVE_FRAME_INPUT_VERSION_2};
    in.generation=generation_;in.referenceGeneration=reference;in.sequence=geometry.sequence;
    GeometrySnapshot snapshot{};in.valid=makeGeometrySnapshot(geometry,snapshot)?1u:0u;
    if(in.valid)std::memcpy(in.physicalHead,snapshot.headToLocal.m,sizeof(in.physicalHead));
    // F31: the head the runtime last answered to Elite's "now" call (the version 2 input only; a version 1 send carries none of it).
    uint32_t nowSequence=0;
    in.nowValid=readNowHead(in.nowHead,nowSequence)?1u:0u;
    in.nowSequence=nowSequence;
    // F37: the render head ring (the last eight frames' heads, this frame's newest), so the d3d11 half can match a marker to the pose the game drew with.
    if(in.valid) {
      ringNewest_=(ringNewest_+1)%8;
      std::memcpy(ring_[ringNewest_],snapshot.headToLocal.m,sizeof(ring_[0]));
      ringSeq_[ringNewest_]=geometry.sequence;
      LARGE_INTEGER q{};QueryPerformanceCounter(&q);ringQpc_[ringNewest_]=uint64_t(q.QuadPart);
      if(ringCount_<8)++ringCount_;
    }
    std::memcpy(in.ringHead,ring_,sizeof(in.ringHead));
    std::memcpy(in.ringSequence,ringSeq_,sizeof(in.ringSequence));
    std::memcpy(in.ringQpc,ringQpc_,sizeof(in.ringQpc));
    in.ringNewest=ringNewest_;in.ringCount=ringCount_;
    const uint32_t askedBefore=providerVersion_;
    const HRESULT result=beginLadder([&](EdvrNativeFrameOutput& o,uint32_t version){
      // A provider that refuses version 2 is sent the version 1 words, exactly as before (size = the version 1 prefix).
      EdvrNativeFrameInput sent=in;
      if(version<EDVR_NATIVE_FRAME_VERSION_7){sent.size=EDVR_NATIVE_FRAME_INPUT_SIZE_1;sent.version=EDVR_NATIVE_FRAME_INPUT_VERSION_1;}
      return table_.beginFrame(table_.context,&sent,&o);
    },providerVersion_,out);
    // F32: the negotiation is visible: each step down, and the version the provider accepted.
    if(providerVersion_!=askedBefore)nativeTracePrintf("frame input: the provider refused version %u; asking version %u from now on\n",askedBefore,providerVersion_);
    if(result==S_OK&&providerVersion_!=loggedVersion_){loggedVersion_=providerVersion_;
      nativeTracePrintf("frame input: negotiated version %u (output version %u), now head %s\n",providerVersion_,out.version,in.nowValid?"sent":"none yet");}
    return result;
  }
  // The version ladder, on its own so a rig can drive it with a provider of any age. A d3d11.dll that does not yet know a newer shape refuses it and
  // consumes nothing when it does, so the same frame can be asked again the only way that one knows: one step down at a time, until version 1 (which every
  // provider answers) or an acceptance settles what shape to ask for first next time. It then carries no trim/pacing/fade field, ever; whatever
  // the answer did not carry reads as its off value (fadeAlpha 0: no fade, never black).
  // (F31: the call is given the shape it should send, so the version 7 step can send its input version 2.)
  template<class Begin>
  static HRESULT beginLadder(Begin&& call,uint32_t& providerVersion,EdvrNativeFrameOutput& out) {
    out=ask(providerVersion);
    auto r=call(out,providerVersion);
    while(r==E_INVALIDARG&&providerVersion>EDVR_NATIVE_FRAME_VERSION_1) {
      --providerVersion;
      out=ask(providerVersion);
      r=call(out,providerVersion);
    }
    if(r!=S_OK||out.version<EDVR_NATIVE_FRAME_VERSION_2)
      out.trimOuterDeg=out.trimNasalDeg=out.trimVerticalDeg=0;
    if(r!=S_OK||out.version<EDVR_NATIVE_FRAME_VERSION_3)
      out.deferredPacing=0;
    if(r!=S_OK||out.version<EDVR_NATIVE_FRAME_VERSION_5||!(out.fadeAlpha==out.fadeAlpha))
      out.fadeAlpha=0;
    // F22: the look's total is zero unless a version 6 answer carried it (no compensation, never a stale one).
    if(r!=S_OK||out.version<EDVR_NATIVE_FRAME_VERSION_6||!(out.lookYawTotal==out.lookYawTotal)) {
      out.lookYawTotal=0;out.lookYawGameFrame=0;out.lookYawReset=0;
    }
    // F33: the eye alignment is zero unless a version 8 answer carried it (no alignment, never a stale one).
    if(r!=S_OK||out.version<EDVR_NATIVE_FRAME_VERSION_8||!(out.eyeAlign[0]==out.eyeAlign[0])||!(out.eyeAlign[1]==out.eyeAlign[1])||!(out.eyeAlign[2]==out.eyeAlign[2])) {
      out.eyeAlign[0]=out.eyeAlign[1]=out.eyeAlign[2]=0;out.eyeAlignSerial=0;
    }
    return r;
  }
  HRESULT latch(uint64_t sequence,EdvrNativeFrameDecision& out) {
    out={sizeof(out),EDVR_NATIVE_FRAME_VERSION_1};
    return acquired()?table_.latchSubmit(table_.context,sequence,&out):S_FALSE;
  }
  HRESULT invalidate(){return acquired()?table_.invalidate(table_.context):S_FALSE;}
  HRESULT close(){if(!acquired())return S_FALSE;const auto r=table_.close(table_.context);if(SUCCEEDED(r)){table_={};providerVersion_=EDVR_NATIVE_FRAME_VERSION_8;}return r;}
  static EdvrNativeFrameOutput ask(uint32_t version) {
    if(version>=EDVR_NATIVE_FRAME_VERSION_8)
      return EdvrNativeFrameOutput{sizeof(EdvrNativeFrameOutput),EDVR_NATIVE_FRAME_VERSION_8};
    if(version>=EDVR_NATIVE_FRAME_VERSION_6)
      return EdvrNativeFrameOutput{EDVR_NATIVE_FRAME_OUTPUT_SIZE_6,EDVR_NATIVE_FRAME_VERSION_6};
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
  // The shape (5/4/3/2/1) the provider last accepted; begin() asks this one
  // first, so a provider once found to be older is not re-probed every frame.
  uint32_t providerVersion_=EDVR_NATIVE_FRAME_VERSION_8;   // F33: starts at 8 (the eye alignment; F31's input version 2 rides with it); a refusal steps down
  uint32_t loggedVersion_=0;   // F32: the version last logged as negotiated
  float ring_[8][12]{};uint64_t ringSeq_[8]{},ringQpc_[8]{};uint32_t ringNewest_=0,ringCount_=0;   // F37: the render head ring
};
}
