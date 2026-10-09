#pragma once
// Explorer Cam's comfort fade, runtime side: the version ladder the native frame client climbs down against a d3d11.dll of any age, and what a mismatched pair reads
// (docs\design-explorer-cam-free-camera-2026-10-07.md, "Comfort fade"). A fade level that cannot be trusted reads 0: no fade, never black. The same ladder carries
// the canted-display arc's two temporary test keys (version 6, docs\canted-projection.md), the terrain-culling arc's cull probe (version 7) and its pose-time
// switch (version 8, advanced.cull_pose; docs\terrain-culling.md): a provider too old to carry them reads as all off.
#include "../../src/openxr/native_frame_client.h"
#include "../../src/common/comfort_fade.h"
#include <cmath>
#include <limits>

namespace edvr::openxr::test {
namespace comfort_fixture {
// A provider of a given age: it knows the shapes up to maxVersion, refuses any other with E_INVALIDARG and consumes nothing when it does, and writes the fade
// level only into a struct that has the slot (the test-key, probe and pose-time slots likewise). `scribble` makes it write the slots anyway (a provider that claims less than it writes).
struct Provider {
  uint32_t maxVersion=8;float fade=1.0f;uint32_t cant=1;float simulate=10.0f;uint32_t probe=2;uint32_t pose=3;unsigned calls=0;bool scribble=false;HRESULT fail=S_OK;
  HRESULT answer(EdvrNativeFrameOutput& o) {
    ++calls;
    if(fail!=S_OK)return fail;
    if(o.version>maxVersion||o.version<EDVR_NATIVE_FRAME_VERSION_1)return E_INVALIDARG;
    const uint32_t want=o.version==8?uint32_t(sizeof(o)):o.version==7?EDVR_NATIVE_FRAME_OUTPUT_SIZE_7:o.version==6?EDVR_NATIVE_FRAME_OUTPUT_SIZE_6:o.version==5?EDVR_NATIVE_FRAME_OUTPUT_SIZE_5:
      o.version==4?EDVR_NATIVE_FRAME_OUTPUT_SIZE_4:o.version==3?EDVR_NATIVE_FRAME_OUTPUT_SIZE_3:o.version==2?EDVR_NATIVE_FRAME_OUTPUT_SIZE_2:EDVR_NATIVE_FRAME_OUTPUT_SIZE_1;
    if(o.size!=want)return E_INVALIDARG;
    o.sceneReady=1;
    if(o.version>=5||scribble)o.fadeAlpha=fade;
    if(o.version>=6||scribble){o.cantedEyeFix=cant;o.simulateCantDeg=simulate;}
    if(o.version>=7||scribble)o.cullProbe=probe;
    if(o.version>=8||scribble)o.cullPose=pose;
    return S_OK;
  }
};
}
template<class Check> void runComfortFadeCases(Check&& check) {
  using namespace comfort_fixture;
  using Client=edvr::openxr::NativeFrameClient;
  auto ladder=[&](Provider& p,uint32_t& version,EdvrNativeFrameOutput& out){
    return Client::beginLadder([&](EdvrNativeFrameOutput& o){return p.answer(o);},version,out);
  };
  {
    Provider p;uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==8&&out.fadeAlpha==1.0f&&v==8&&p.calls==1&&out.sceneReady==1,"a current d3d11.dll: one call, version 8, the fade level arrives");
    check(out.cantedEyeFix==1&&out.simulateCantDeg==10.0f&&out.cullProbe==2&&out.cullPose==3,"...and so do the canted-display test keys, the cull probe and the pose-time switch");
  }
  {
    Provider p;p.maxVersion=7;uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==7&&v==7&&p.calls==2&&out.cantedEyeFix==1&&out.simulateCantDeg==10.0f&&out.cullProbe==2,
      "a d3d11.dll one version old (7): the version 8 ask is refused, the client steps down to 7, remembers it, and the cull probe still arrives");
    check(out.cullPose==0,"...and reads the pose-time switch OFF (Elite's game-thread head pose located as always), though that provider has a setting of its own it could not carry");
    check(ladder(p,v,out)==S_OK&&p.calls==3&&out.version==7,"...the next frame asks 7 first: one call, not two");
  }
  {
    Provider p;p.maxVersion=6;uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==6&&v==6&&p.calls==3&&out.cantedEyeFix==1&&out.simulateCantDeg==10.0f,
      "a d3d11.dll two versions old (6): the asks for 8 and 7 are refused, the client steps down to 6, remembers it, and the canted-display keys still arrive");
    check(out.cullProbe==0&&out.cullPose==0,"...and reads the cull probe and the pose-time switch OFF, though that provider has settings of its own it could not carry");
    check(ladder(p,v,out)==S_OK&&p.calls==4&&out.version==6,"...the next frame asks 6 first: one call, not three");
  }
  {
    Provider p;p.maxVersion=5;uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==5&&v==5&&p.calls==4&&out.sceneReady==1&&out.fadeAlpha==1.0f,
      "a d3d11.dll three versions old (5): the client steps down 8, 7, 6, 5, remembers it, and the fade level still arrives");
    check(out.cantedEyeFix==0&&out.simulateCantDeg==0.0f&&out.cullProbe==0&&out.cullPose==0,"...and reads the test keys, the probe and the pose-time switch OFF (no correction, no cant, no probe, no pose mode)");
    check(ladder(p,v,out)==S_OK&&p.calls==5&&out.version==5,"...the next frame asks 5 first: one call, not four");
  }
  {
    Provider p;p.maxVersion=4;uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==4&&v==4&&p.calls==5&&out.sceneReady==1,"an OLDER d3d11.dll (version 4): the asks for 8, 7, 6 and 5 are refused, the client steps down to 4, and remembers it");
    check(out.fadeAlpha==0.0f,"...and reads NO FADE (0), though that provider has a level of 1 it could not carry");
    check(out.cantedEyeFix==0&&out.simulateCantDeg==0.0f&&out.cullProbe==0&&out.cullPose==0,"...and the test keys, the probe and the pose-time switch off");
    check(ladder(p,v,out)==S_OK&&p.calls==6&&out.version==4,"...the next frame asks 4 first: one call, not two");
  }
  {
    Provider p;p.maxVersion=4;p.scribble=true;uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==4&&out.fadeAlpha==0.0f,"A PROVIDER THAT WRITES THE FADE SLOT OF A SHAPE THAT HAS NONE is not believed: version 4 reads 0");
    check(out.cantedEyeFix==0&&out.simulateCantDeg==0.0f&&out.cullProbe==0&&out.cullPose==0,"...nor the test-key, probe and pose-time slots of a shape that has none");
  }
  {
    Provider p;p.maxVersion=5;p.scribble=true;uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==5&&out.fadeAlpha==1.0f&&out.cantedEyeFix==0&&out.simulateCantDeg==0.0f&&out.cullProbe==0&&out.cullPose==0,
      "A VERSION 5 PROVIDER THAT WRITES THE TEST-KEY, PROBE AND POSE-TIME SLOTS OF A SHAPE THAT HAS NONE is not believed: version 5 reads all off");
  }
  {
    Provider p;p.maxVersion=6;p.scribble=true;uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==6&&out.cantedEyeFix==1&&out.simulateCantDeg==10.0f&&out.cullProbe==0&&out.cullPose==0,
      "A VERSION 6 PROVIDER THAT WRITES THE PROBE AND POSE-TIME SLOTS OF A SHAPE THAT HAS NONE is not believed: both read off, the keys it does carry are kept");
  }
  {
    Provider p;p.maxVersion=7;p.scribble=true;uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==7&&out.cullProbe==2&&out.cantedEyeFix==1&&out.cullPose==0,
      "A VERSION 7 PROVIDER THAT WRITES THE POSE-TIME SLOT OF A SHAPE THAT HAS NONE is not believed: the switch reads off, the probe it does carry is kept");
  }
  for(uint32_t version:{3u,2u,1u}) {
    Provider p;p.maxVersion=version;uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==version&&v==version&&out.fadeAlpha==0.0f&&out.cantedEyeFix==0&&out.cullProbe==0&&out.cullPose==0&&p.calls==8-version+1,
      "an older still (3, 2, 1): the client steps down one version at a time and reads no fade, the test keys off, no probe and no pose-time switch");
  }
  {
    Provider p;p.fade=std::numeric_limits<float>::quiet_NaN();uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==8&&out.fadeAlpha==0.0f,"a version 8 answer carrying NaN as the fade reads 0");
  }
  {
    Provider p;p.simulate=std::numeric_limits<float>::quiet_NaN();uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==8&&out.simulateCantDeg==0.0f&&out.cantedEyeFix==1,"a version 8 answer carrying NaN as the simulated cant reads 0 (off), and leaves the correction as told");
  }
  {
    Provider p;p.simulate=40.0f;p.cant=7;uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.simulateCantDeg==0.0f&&out.cantedEyeFix==0,"a version 8 answer outside the contract (40 degrees, a flag of 7) reads both off");
  }
  {
    Provider p;p.probe=9;uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==8&&out.cullProbe==0&&out.cantedEyeFix==1&&out.cullPose==3,"a version 8 answer with a probe code outside 0..6 reads the probe off, and the pose-time switch it carries is kept");
  }
  {
    Provider p;uint32_t v=8;EdvrNativeFrameOutput out{};
    bool codes=true;
    for(uint32_t code=0;code<=4;++code){p.pose=code;codes=codes&&ladder(p,v,out)==S_OK&&out.cullPose==code;}
    check(codes,"every pose-time code 0..4 arrives as itself");
    p.pose=5;
    check(ladder(p,v,out)==S_OK&&out.version==8&&out.cullPose==0&&out.cullProbe==2,"a version 8 answer with a pose-time code outside 0..4 reads the switch off, and keeps the probe");
    p.pose=0xFFFFFFFFu;
    check(ladder(p,v,out)==S_OK&&out.cullPose==0,"...a wild code too");
  }
  {
    Provider p;p.fail=E_FAIL;uint32_t v=8;EdvrNativeFrameOutput out{};out.fadeAlpha=1.0f;out.cantedEyeFix=1;out.simulateCantDeg=10.0f;out.cullProbe=2;out.cullPose=3;
    check(ladder(p,v,out)==E_FAIL&&out.fadeAlpha==0.0f&&out.cantedEyeFix==0&&out.simulateCantDeg==0.0f&&out.cullProbe==0&&out.cullPose==0&&v==8&&p.calls==1,
      "a provider that fails the frame outright leaves no fade, test key, probe or pose-time switch behind (and is not mistaken for an old one)");
  }
  {
    Provider p;p.fail=E_INVALIDARG;uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==E_INVALIDARG&&v==1&&out.fadeAlpha==0.0f&&out.cantedEyeFix==0&&out.cullProbe==0&&out.cullPose==0&&p.calls==8,
      "a provider that refuses every shape is tried down to version 1 and reads no fade, the test keys off, no probe and no pose-time switch");
  }
  check(Client::ask(8).size==sizeof(EdvrNativeFrameOutput)&&Client::ask(8).version==8&&Client::ask(7).size==EDVR_NATIVE_FRAME_OUTPUT_SIZE_7&&Client::ask(7).version==7&&
        Client::ask(6).size==EDVR_NATIVE_FRAME_OUTPUT_SIZE_6&&Client::ask(6).version==6&&
        Client::ask(5).size==EDVR_NATIVE_FRAME_OUTPUT_SIZE_5&&Client::ask(5).version==5&&Client::ask(4).size==EDVR_NATIVE_FRAME_OUTPUT_SIZE_4&&Client::ask(4).version==4&&
        Client::ask(9).version==8&&Client::ask(0).version==1,
        "the shapes the client asks for: 8 is the whole struct, 7 stops before the pose-time slot, 6 before the probe slot, 5 before the test-key slots, 4 before the fade slot, anything above is 8, 0 is 1");
  check(Client::ask(8).fadeAlpha==0.0f&&Client::ask(8).cantedEyeFix==0&&Client::ask(8).simulateCantDeg==0.0f&&Client::ask(8).cullProbe==0&&Client::ask(8).cullPose==0,
        "a fresh ask carries fade 0 (no fade), both test keys, the probe and the pose-time switch off until a provider says otherwise");
}
} // namespace edvr::openxr::test
