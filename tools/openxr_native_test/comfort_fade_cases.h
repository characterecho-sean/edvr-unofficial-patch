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
// level only into a struct that has the slot. `scribble` makes it write the slot anyway (a provider that claims less than it writes).
struct Provider {
  uint32_t maxVersion=5;float fade=1.0f;unsigned calls=0;bool scribble=false;HRESULT fail=S_OK;
  HRESULT answer(EdvrNativeFrameOutput& o) {
    ++calls;
    if(fail!=S_OK)return fail;
    if(o.version>maxVersion||o.version<EDVR_NATIVE_FRAME_VERSION_1)return E_INVALIDARG;
    const uint32_t want=o.version==5?uint32_t(sizeof(o)):o.version==4?EDVR_NATIVE_FRAME_OUTPUT_SIZE_4:o.version==3?EDVR_NATIVE_FRAME_OUTPUT_SIZE_3:
      o.version==2?EDVR_NATIVE_FRAME_OUTPUT_SIZE_2:EDVR_NATIVE_FRAME_OUTPUT_SIZE_1;
    if(o.size!=want)return E_INVALIDARG;
    o.sceneReady=1;
    if(o.version>=5||scribble)o.fadeAlpha=fade;
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
    Provider p;uint32_t v=5;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==5&&out.fadeAlpha==1.0f&&v==5&&p.calls==1&&out.sceneReady==1,"a current d3d11.dll: one call, version 5, the fade level arrives");
  }
  {
    Provider p;p.maxVersion=4;uint32_t v=5;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==4&&v==4&&p.calls==2&&out.sceneReady==1,"an OLDER d3d11.dll (version 4): the version 5 ask is refused, the client steps down to 4, and remembers it");
    check(out.fadeAlpha==0.0f,"...and reads NO FADE (0), though that provider has a level of 1 it could not carry");
    check(ladder(p,v,out)==S_OK&&p.calls==3&&out.version==4,"...the next frame asks 4 first: one call, not two");
  }
  {
    Provider p;p.maxVersion=4;p.scribble=true;uint32_t v=5;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==4&&out.fadeAlpha==0.0f,"A PROVIDER THAT WRITES THE FADE SLOT OF A SHAPE THAT HAS NONE is not believed: version 4 reads 0");
  }
  for(uint32_t version:{3u,2u,1u}) {
    Provider p;p.maxVersion=version;uint32_t v=5;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==version&&v==version&&out.fadeAlpha==0.0f&&p.calls==5-version+1,
      "an older still (3, 2, 1): the client steps down one version at a time and reads no fade");
  }
  {
    Provider p;p.fade=std::numeric_limits<float>::quiet_NaN();uint32_t v=5;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==5&&out.fadeAlpha==0.0f,"a version 5 answer carrying NaN reads 0");
  }
  {
    Provider p;p.fail=E_FAIL;uint32_t v=5;EdvrNativeFrameOutput out{};out.fadeAlpha=1.0f;
    check(ladder(p,v,out)==E_FAIL&&out.fadeAlpha==0.0f&&v==5&&p.calls==1,"a provider that fails the frame outright leaves no fade behind (and is not mistaken for an old one)");
  }
  {
    Provider p;p.fail=E_INVALIDARG;uint32_t v=5;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==E_INVALIDARG&&v==1&&out.fadeAlpha==0.0f&&p.calls==5,"a provider that refuses every shape is tried down to version 1 and reads no fade");
  }
  check(Client::ask(5).size==sizeof(EdvrNativeFrameOutput)&&Client::ask(5).version==5&&Client::ask(4).size==EDVR_NATIVE_FRAME_OUTPUT_SIZE_4&&Client::ask(4).version==4&&
        Client::ask(9).version==5&&Client::ask(0).version==1,"the shapes the client asks for: 5 is the whole struct, 4 stops before the fade slot, anything above is 5, 0 is 1");
  check(Client::ask(5).fadeAlpha==0.0f,"a fresh ask carries fade 0 (no fade) until a provider says otherwise");
}
} // namespace edvr::openxr::test
