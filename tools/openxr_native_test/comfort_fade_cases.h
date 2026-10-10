#pragma once
// Explorer Cam's comfort fade, runtime side: the version ladder the native frame client climbs down against a d3d11.dll of any age, and what a mismatched pair reads
// (docs\design-explorer-cam-free-camera-2026-10-07.md, "Comfort fade"). A fade level that cannot be trusted reads 0: no fade, never black.
#include "../../src/openxr/native_frame_client.h"
#include "../../src/common/comfort_fade.h"
#include <cmath>
#include <limits>

namespace edvr::openxr::test {
namespace comfort_fixture {
// A provider of a given age: it knows the shapes up to maxVersion, refuses any other with E_INVALIDARG and consumes nothing when it does, and writes the fade
// level (version 5) and the pose-time switch (version 6) only into a struct that has the slot. `scribble` makes it write the slots anyway (a provider that claims less than it writes).
struct Provider {
  uint32_t maxVersion=6;float fade=1.0f;uint32_t pose=2;unsigned calls=0;bool scribble=false;HRESULT fail=S_OK;
  HRESULT answer(EdvrNativeFrameOutput& o) {
    ++calls;
    if(fail!=S_OK){if(scribble){o.fadeAlpha=fade;o.cullPose=pose;}return fail;}   // (a provider that wrote its slots and then failed)
    if(o.version>maxVersion||o.version<EDVR_NATIVE_FRAME_VERSION_1)return E_INVALIDARG;
    const uint32_t want=o.version==6?uint32_t(sizeof(o)):o.version==5?EDVR_NATIVE_FRAME_OUTPUT_SIZE_5:o.version==4?EDVR_NATIVE_FRAME_OUTPUT_SIZE_4:o.version==3?EDVR_NATIVE_FRAME_OUTPUT_SIZE_3:
      o.version==2?EDVR_NATIVE_FRAME_OUTPUT_SIZE_2:EDVR_NATIVE_FRAME_OUTPUT_SIZE_1;
    if(o.size!=want)return E_INVALIDARG;
    o.sceneReady=1;
    if(o.version>=5||scribble)o.fadeAlpha=fade;
    if(o.version>=6||scribble)o.cullPose=pose;
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
    Provider p;uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==6&&out.fadeAlpha==1.0f&&out.cullPose==2u&&v==6&&p.calls==1&&out.sceneReady==1,"a current d3d11.dll: one call, version 6, the fade level and the pose-time mode arrive");
  }
  {
    Provider p;p.maxVersion=5;uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==5&&v==5&&p.calls==2&&out.sceneReady==1&&out.fadeAlpha==1.0f,"a d3d11.dll of the last release (version 5): the version 6 ask is refused, the client steps down to 5, and remembers it; the fade still arrives");
    check(out.cullPose==0u,"...and reads the pose-time mode 0, the default (display), though that provider has a mode of its own it could not carry");
    check(ladder(p,v,out)==S_OK&&p.calls==3&&out.version==5,"...the next frame asks 5 first: one call, not two");
  }
  {
    Provider p;p.maxVersion=5;p.scribble=true;uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==5&&out.cullPose==0u,"A PROVIDER THAT WRITES THE POSE SLOT OF A SHAPE THAT HAS NONE is not believed: version 5 reads the default");
  }
  {
    Provider p;p.maxVersion=4;uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==4&&v==4&&p.calls==3&&out.sceneReady==1,"an OLDER d3d11.dll (version 4): 6 and 5 are refused, the client steps down to 4, and remembers it");
    check(out.fadeAlpha==0.0f&&out.cullPose==0u,"...and reads NO FADE (0) and the default pose-time mode, though that provider has a level of 1 it could not carry");
  }
  {
    Provider p;p.maxVersion=4;p.scribble=true;uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==4&&out.fadeAlpha==0.0f&&out.cullPose==0u,"A PROVIDER THAT WRITES THE FADE AND POSE SLOTS OF A SHAPE THAT HAS NEITHER is not believed: version 4 reads 0 and the default");
  }
  for(uint32_t version:{3u,2u,1u}) {
    Provider p;p.maxVersion=version;uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==version&&v==version&&out.fadeAlpha==0.0f&&out.cullPose==0u&&p.calls==6-version+1,
      "an older still (3, 2, 1): the client steps down one version at a time and reads no fade and the default mode");
  }
  {
    Provider p;p.fade=std::numeric_limits<float>::quiet_NaN();uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==6&&out.fadeAlpha==0.0f&&out.cullPose==2u,"a version 6 answer carrying NaN reads a fade of 0 and keeps its mode");
  }
  for(uint32_t code:{0u,1u,2u,3u,4u}) {
    Provider p;p.pose=code;uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.cullPose==code,"every mode code 0..4 arrives as sent");
  }
  for(uint32_t code:{5u,6u,0x80000000u,0xFFFFFFFFu}) {
    Provider p;p.pose=code;uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==6&&out.cullPose==0u&&out.fadeAlpha==1.0f,"a version 6 answer with a mode code outside 0..4 reads the default, and the rest of the answer is kept");
  }
  {
    Provider p;p.fail=E_FAIL;p.scribble=true;uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==E_FAIL&&out.fadeAlpha==0.0f&&out.cullPose==0u&&v==6,"a provider that wrote its fade and mode and THEN failed the frame leaves neither behind");
  }
  {
    Provider p;p.fail=E_FAIL;uint32_t v=6;EdvrNativeFrameOutput out{};out.fadeAlpha=1.0f;out.cullPose=2u;
    check(ladder(p,v,out)==E_FAIL&&out.fadeAlpha==0.0f&&out.cullPose==0u&&v==6&&p.calls==1,"a provider that fails the frame outright leaves no fade or mode behind (and is not mistaken for an old one)");
  }
  {
    Provider p;p.fail=E_INVALIDARG;uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==E_INVALIDARG&&v==1&&out.fadeAlpha==0.0f&&out.cullPose==0u&&p.calls==6,"a provider that refuses every shape is tried down to version 1 and reads no fade and the default mode");
  }
  check(Client::ask(6).size==sizeof(EdvrNativeFrameOutput)&&Client::ask(6).version==6&&Client::ask(5).size==EDVR_NATIVE_FRAME_OUTPUT_SIZE_5&&Client::ask(5).version==5&&
        Client::ask(4).size==EDVR_NATIVE_FRAME_OUTPUT_SIZE_4&&Client::ask(4).version==4&&Client::ask(9).version==6&&Client::ask(0).version==1,
        "the shapes the client asks for: 6 is the whole struct, 5 stops before the pose slot, 4 before the fade slot, anything above is 6, 0 is 1");
  check(Client::ask(6).fadeAlpha==0.0f&&Client::ask(6).cullPose==0u,"a fresh ask carries fade 0 (no fade) and the default pose-time mode until a provider says otherwise");
}
} // namespace edvr::openxr::test
