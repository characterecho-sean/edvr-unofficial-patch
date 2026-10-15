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
  uint32_t maxVersion=8;float fade=1.0f;unsigned calls=0;bool scribble=false;HRESULT fail=S_OK;
  HRESULT answer(EdvrNativeFrameOutput& o) {
    ++calls;
    if(fail!=S_OK)return fail;
    if(o.version>maxVersion||o.version<EDVR_NATIVE_FRAME_VERSION_1)return E_INVALIDARG;
    const uint32_t want=o.version==8?uint32_t(sizeof(o)):o.version==6?EDVR_NATIVE_FRAME_OUTPUT_SIZE_6:o.version==5?EDVR_NATIVE_FRAME_OUTPUT_SIZE_5:o.version==4?EDVR_NATIVE_FRAME_OUTPUT_SIZE_4:o.version==3?EDVR_NATIVE_FRAME_OUTPUT_SIZE_3:
      o.version==2?EDVR_NATIVE_FRAME_OUTPUT_SIZE_2:EDVR_NATIVE_FRAME_OUTPUT_SIZE_1;
    if(o.size!=want)return E_INVALIDARG;
    o.sceneReady=1;
    if(o.version>=5||scribble)o.fadeAlpha=fade;
    if(o.version>=6) {o.lookYawTotal=0.25f;o.lookYawGameFrame=7;}   // F22: the look's total rides the version 6 shape only
    if(o.version>=8) {o.eyeAlign[0]=0.1f;o.eyeAlign[1]=0.2f;o.eyeAlign[2]=-0.3f;o.eyeAlignSerial=5;}   // F33: the body-align translation rides the version 8 shape only
    return S_OK;
  }
};
}
template<class Check> void runComfortFadeCases(Check&& check) {
  using namespace comfort_fixture;
  using Client=edvr::openxr::NativeFrameClient;
  auto ladder=[&](Provider& p,uint32_t& version,EdvrNativeFrameOutput& out){
    return Client::beginLadder([&](EdvrNativeFrameOutput& o,uint32_t){return p.answer(o);},version,out);
  };
  {
    Provider p;uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==6&&out.fadeAlpha==1.0f&&v==6&&p.calls==1&&out.sceneReady==1,"a current d3d11.dll: one call, version 6, the fade level arrives");
    check(out.lookYawTotal==0.25f&&out.lookYawGameFrame==7,"...and the look's total arrives with it (F22)");
  }
  {
    Provider p;p.maxVersion=4;uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==4&&v==4&&p.calls==3&&out.sceneReady==1,"an OLDER d3d11.dll (version 4): the version 6 and 5 asks are refused, the client steps down to 4, and remembers it");
    check(out.fadeAlpha==0.0f&&out.lookYawTotal==0.0f,"...and reads NO FADE (0) and NO LOOK TOTAL (0), though that provider has a level of 1 it could not carry");
    check(ladder(p,v,out)==S_OK&&p.calls==4&&out.version==4,"...the next frame asks 4 first: one call, not three");
  }
  {
    Provider p;p.maxVersion=4;p.scribble=true;uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==4&&out.fadeAlpha==0.0f,"A PROVIDER THAT WRITES THE FADE SLOT OF A SHAPE THAT HAS NONE is not believed: version 4 reads 0");
  }
  for(uint32_t version:{3u,2u,1u}) {
    Provider p;p.maxVersion=version;uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==version&&v==version&&out.fadeAlpha==0.0f&&p.calls==7-version,
      "an older still (3, 2, 1): the client steps down one version at a time and reads no fade");
  }
  {
    Provider p;p.fade=std::numeric_limits<float>::quiet_NaN();uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==6&&out.fadeAlpha==0.0f,"a version 6 answer carrying NaN reads 0");
  }
  {
    Provider p;p.fail=E_FAIL;uint32_t v=6;EdvrNativeFrameOutput out{};out.fadeAlpha=1.0f;
    check(ladder(p,v,out)==E_FAIL&&out.fadeAlpha==0.0f&&v==6&&p.calls==1,"a provider that fails the frame outright leaves no fade behind (and is not mistaken for an old one)");
  }
  {
    Provider p;p.fail=E_INVALIDARG;uint32_t v=6;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==E_INVALIDARG&&v==1&&out.fadeAlpha==0.0f&&p.calls==6,"a provider that refuses every shape is tried down to version 1 and reads no fade");
  }
  {   // F33: version 8 carries the eye alignment; a provider or client older than 8 reads zero
    Provider p;uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==8&&v==8&&p.calls==1&&out.eyeAlign[0]==0.1f&&out.eyeAlign[1]==0.2f&&out.eyeAlign[2]==-0.3f&&out.eyeAlignSerial==5&&out.lookYawTotal==0.25f,
      "a version 8 d3d11.dll: one call, the eye alignment and its serial arrive with the look's total (F33)");
  }
  {
    Provider p;p.maxVersion=6;uint32_t v=8;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==6&&v==7&&p.calls==2&&out.eyeAlign[0]==0.0f&&out.eyeAlign[1]==0.0f&&out.eyeAlign[2]==0.0f&&out.eyeAlignSerial==0&&out.lookYawTotal==0.25f,
      "a version 6 d3d11.dll: the version 8 ask is refused, version 7 asks the version 6 shape, the eye alignment reads zero and the look still arrives (F33)");
  }
  {
    Provider p;p.fail=E_FAIL;uint32_t v=8;EdvrNativeFrameOutput out{};out.eyeAlign[1]=1.0f;out.eyeAlignSerial=9;
    check(ladder(p,v,out)==E_FAIL&&out.eyeAlign[1]==0.0f&&out.eyeAlignSerial==0&&v==8,"a provider that fails the frame leaves no eye alignment behind (F33)");
  }
  check(Client::ask(8).size==sizeof(EdvrNativeFrameOutput)&&Client::ask(8).version==8&&Client::ask(7).size==EDVR_NATIVE_FRAME_OUTPUT_SIZE_6&&Client::ask(7).version==6&&
        Client::ask(8).eyeAlign[0]==0.0f&&Client::ask(8).eyeAlignSerial==0,"F33: 8 is the whole struct, 7 asks the version 6 shape (the input version 2 step), a fresh ask has no alignment");
  check(Client::ask(6).size==EDVR_NATIVE_FRAME_OUTPUT_SIZE_6&&Client::ask(6).version==6&&Client::ask(5).size==EDVR_NATIVE_FRAME_OUTPUT_SIZE_5&&Client::ask(5).version==5&&
        Client::ask(4).size==EDVR_NATIVE_FRAME_OUTPUT_SIZE_4&&Client::ask(4).version==4&&Client::ask(9).version==8&&Client::ask(0).version==1,
        "the shapes the client asks for: 6 stops before the eye alignment, 5 stops before the look's words, 4 stops before the fade slot, anything above is 8, 0 is 1");
  check(Client::ask(6).fadeAlpha==0.0f&&Client::ask(6).lookYawTotal==0.0f,"a fresh ask carries fade 0 and look total 0 (no fade, no compensation) until a provider says otherwise");
}
} // namespace edvr::openxr::test
