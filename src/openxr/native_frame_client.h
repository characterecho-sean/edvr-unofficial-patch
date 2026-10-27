#pragma once
#include "../common/native_frame.h"
#include "../common/world_scale.h"
#include "geometry_snapshot.h"
#include "now_head_publication.h"   // F31: the "now" head the version 2 input carries
#include "native_trace.h"
#include "head_pose_time.h"   // F40: the live now-pose flag
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
    table_=candidate;generation_=generation;providerVersion_=EDVR_NATIVE_FRAME_VERSION_11;return S_OK;
  }
  bool acquired()const{return table_.context!=nullptr;}
  // F62b: the version the client asks first (11 until a provider refuses it): the host locates the LOCAL head only for an ask of 10 or more.
  uint32_t askedVersion()const{return providerVersion_;}
  // F62b: localHead (optional, 12 floats, row-major 3x4 rigid in natural LOCAL) rides the version 3 input of a version 10 ask.
  // F89: eyeApplied is the applied-alignment word (native_frame.h, EdvrNativeFrameInput::eyeAlignApplied): the serial of the eye alignment the pose carries, plus 1.
  HRESULT begin(const GeometryInput& geometry,uint64_t reference,EdvrNativeFrameOutput& out,const float* localHead=nullptr,uint32_t eyeApplied=0) {
    out=ask(providerVersion_);
    if(!acquired())return S_FALSE;
    EdvrNativeFrameInput in{sizeof(in),EDVR_NATIVE_FRAME_INPUT_VERSION_3};
    if(localHead){std::memcpy(in.localHead,localHead,sizeof(in.localHead));in.localValid=1;}
    in.eyeAlignApplied=eyeApplied;
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
      // F62b: a provider that refuses version 10 is sent the version 2 words (size = the version 2 prefix), exactly as before.
      // F101: the shapes are native_frame.h's (nativeFrameInputShape); version 1's size is the released struct's sizeof (88), which a released provider requires.
      EdvrNativeFrameInput sent=in;
      edvr::nativeFrameInputShape(version,sent);
      return table_.beginFrame(table_.context,&sent,&o);
    },providerVersion_,out);
    // F40: the live now-pose flag from the provider's answer (version 9 only; a version 8 answer carries none, so the default holds).
    if(result==S_OK) applyNowPose(out.nowPoseRender<=2u?out.nowPoseRender:0u);
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
    // F40: the now-pose flag is off unless a version 9 answer carried it.
    if(r!=S_OK||out.version<EDVR_NATIVE_FRAME_VERSION_9) out.nowPoseRender=0;
    // F62b: the ADS head-aim's pitch (reservedYaw) is zero unless a version 10 answer carried it.
    if(r!=S_OK||out.version<EDVR_NATIVE_FRAME_VERSION_10||!(out.reservedYaw==out.reservedYaw)) out.reservedYaw=0;
    // F106: the left eye's share (leftEyeShare, a float's bits; F89's darkening before) is zero unless a version 10 answer carried it.
    if(r!=S_OK||out.version<EDVR_NATIVE_FRAME_VERSION_10) out.leftEyeShare=0;
    // F123: the position-only recentre ask is none unless a version 10 answer carried it.
    if(r!=S_OK||out.version<EDVR_NATIVE_FRAME_VERSION_10) out.positionRecentre=0;
    // World scale: Elite's own scale (100) unless a version 11 answer carried a percent; a zero or an out-of-range word is held to the range (common/world_scale.h).
    out.worldScalePercent=(r==S_OK&&out.version>=EDVR_NATIVE_FRAME_VERSION_11)?edvr::worldScaleClamp(out.worldScalePercent):edvr::kWorldScaleDefault;
    return r;
  }
  HRESULT latch(uint64_t sequence,EdvrNativeFrameDecision& out) {
    out={sizeof(out),EDVR_NATIVE_FRAME_VERSION_1};
    return acquired()?table_.latchSubmit(table_.context,sequence,&out):S_FALSE;
  }
  HRESULT invalidate(){return acquired()?table_.invalidate(table_.context):S_FALSE;}
  HRESULT close(){if(!acquired())return S_FALSE;const auto r=table_.close(table_.context);if(SUCCEEDED(r)){table_={};providerVersion_=EDVR_NATIVE_FRAME_VERSION_11;}return r;}
  // F40/F42: the runtime's now-pose mode (0 next, 1 render, 2 render_locked), from the provider's answer; logged at the first value and at each change.
  static void applyNowPose(uint32_t mode) {
    static bool logged = false;
    static const char* const kNames[3] = {"next", "render", "render_locked"};
    const uint32_t was = g_nowPoseMode.exchange(mode, std::memory_order_acq_rel);
    if (!logged || was != mode) {
      logged = true;
      nativeTracePrintf("now pose: %s (Elite's now request is answered %s)\n", kNames[mode],
                        mode == 2 ? "with the latched render pose of the frame (one head pose a frame)"
                        : mode == 1 ? "at the instant the frame draws at (the WaitGetPoses latch)" : "one display period ahead (today's behaviour)");
    }
  }
  static EdvrNativeFrameOutput ask(uint32_t version) {return edvr::nativeFrameAsk(version);}   // F101: native_frame.h's, shared with the provider's rig
 private:
  template<class T>static bool owned(HMODULE provider,T address) {
    if(!address)return false;HMODULE actual=nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(address),&actual)&&actual==provider;
  }
  EdvrNativeFrameTable table_{};uint64_t generation_=0;
  // The shape (5/4/3/2/1) the provider last accepted; begin() asks this one
  // first, so a provider once found to be older is not re-probed every frame.
  uint32_t providerVersion_=EDVR_NATIVE_FRAME_VERSION_11;   // starts at 11 (world scale; F62b's input version 3, the LOCAL head, from 10; F31's input version 2 below it); a refusal steps down
  uint32_t loggedVersion_=0;   // F32: the version last logged as negotiated
  float ring_[8][12]{};uint64_t ringSeq_[8]{},ringQpc_[8]{};uint32_t ringNewest_=0,ringCount_=0;   // F37: the render head ring
};
}
