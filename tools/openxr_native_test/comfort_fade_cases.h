#pragma once
// Explorer Cam's comfort fade, runtime side: the version ladder the native frame client climbs down against a d3d11.dll of any age, and what a mismatched pair reads
// (docs\design-explorer-cam-free-camera-2026-10-07.md, "Comfort fade"). A fade level that cannot be trusted reads 0: no fade, never black.
#include "../../src/openxr/native_frame_client.h"
#include "../../src/common/comfort_fade.h"
#include "../../src/common/left_eye_share.h"
#include <cmath>
#include <limits>
#include <utility>

namespace edvr::openxr::test {
namespace comfort_fixture {
// A provider of a given age: it knows the shapes up to maxVersion, refuses any other with E_INVALIDARG and consumes nothing when it does, and writes the fade
// level only into a struct that has the slot. `scribble` makes it write the slot anyway (a provider that claims less than it writes).
struct Provider {
  uint32_t maxVersion=8;float fade=1.0f;unsigned calls=0;bool scribble=false;HRESULT fail=S_OK;uint32_t worldScale=130;
  HRESULT answer(EdvrNativeFrameOutput& o) {
    ++calls;
    if(fail!=S_OK)return fail;
    if(o.version>maxVersion||o.version<EDVR_NATIVE_FRAME_VERSION_1)return E_INVALIDARG;
    const uint32_t want=o.version>=8?uint32_t(sizeof(o)):o.version==6?EDVR_NATIVE_FRAME_OUTPUT_SIZE_6:o.version==5?EDVR_NATIVE_FRAME_OUTPUT_SIZE_5:o.version==4?EDVR_NATIVE_FRAME_OUTPUT_SIZE_4:o.version==3?EDVR_NATIVE_FRAME_OUTPUT_SIZE_3:
      o.version==2?EDVR_NATIVE_FRAME_OUTPUT_SIZE_2:EDVR_NATIVE_FRAME_OUTPUT_SIZE_1;
    if(o.size!=want)return E_INVALIDARG;
    o.sceneReady=1;
    if(o.version>=5||scribble)o.fadeAlpha=fade;
    if(o.version>=6) {o.lookYawTotal=0.25f;o.lookYawGameFrame=7;}   // F22: the look's total rides the version 6 shape only
    if(o.version>=8) {o.eyeAlign[0]=0.1f;o.eyeAlign[1]=0.2f;o.eyeAlign[2]=-0.3f;o.eyeAlignSerial=5;}   // F33: the body-align translation rides the version 8 shape only
    if(o.version>=10||scribble)o.reservedYaw=0.4f;   // F62b: the ADS head-aim's pitch rides a version 10 answer only
    if(o.version>=10||scribble)o.leftEyeShare=0x3F19999Au;   // F106: the left eye's share (0.6f's bits; F89's darkening before) rides a version 10 answer only
    if(o.version>=11||scribble)o.worldScalePercent=worldScale;   // world scale: rides a version 11 answer only
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
        Client::ask(4).size==EDVR_NATIVE_FRAME_OUTPUT_SIZE_4&&Client::ask(4).version==4&&Client::ask(9).version==9&&Client::ask(9).size==sizeof(EdvrNativeFrameOutput)&&Client::ask(0).version==1,
        "the shapes the client asks for: 6 stops before the eye alignment, 5 stops before the look's words, 4 stops before the fade slot, 8 is 8, 9 and above is 9 (F40), 0 is 1");
  check(Client::ask(6).fadeAlpha==0.0f&&Client::ask(6).lookYawTotal==0.0f,"a fresh ask carries fade 0 and look total 0 (no fade, no compensation) until a provider says otherwise");
  {   // F62b: version 10 (input version 3) carries the ADS head-aim's pitch; a version 9 d3d11.dll refuses 10 and the client reads no pitch
    Provider p;p.maxVersion=10;uint32_t v=10;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==10&&v==10&&p.calls==1&&out.reservedYaw==0.4f&&Client::ask(10).version==10&&Client::ask(10).size==sizeof(EdvrNativeFrameOutput)&&
          Client::ask(10).version==10&&Client::ask(11).version==11&&Client::ask(11).size==sizeof(EdvrNativeFrameOutput)&&Client::ask(12).version==11,
          "F62b: a version 10 d3d11.dll: one call, the ADS pitch arrives; 10 asks 10, 11 and above asks 11, both the whole struct");
    Provider old;old.maxVersion=9;old.scribble=true;uint32_t v9=10;EdvrNativeFrameOutput out9{};
    check(ladder(old,v9,out9)==S_OK&&out9.version==9&&v9==9&&old.calls==2&&out9.reservedYaw==0.0f&&out9.eyeAlign[0]==0.1f,
          "F62b: a version 9 d3d11.dll: the 10 ask is refused, 9 is answered, and a pitch it scribbles into reservedYaw is not believed");
    // F106: the left eye's share (leftEyeShare, a float's bits; F89's darkening in the same word) arrives with a version 10 answer and reads 0 from any other shape or a failed frame
    check(out.leftEyeShare==0x3F19999Au&&edvr::eyeshare::fromBits(out.leftEyeShare)==0.6f&&out9.leftEyeShare==0u&&edvr::eyeshare::fromBits(out9.leftEyeShare)==0.0f,
          "F106: a version 10 d3d11.dll's left eye share (0.6) arrives, as an F89 d3d11.dll's darkening in the same word does; a version 9 d3d11.dll that scribbles the slot reads 0 (no share from a shape that has none)");
    Provider failing;failing.maxVersion=10;failing.fail=E_FAIL;uint32_t vf=10;EdvrNativeFrameOutput outf{};outf.leftEyeShare=0x3F800000u;
    check(ladder(failing,vf,outf)==E_FAIL&&outf.leftEyeShare==0u,"F106: a provider that fails the frame leaves no left eye share behind");
    check(edvr::eyeshare::fromBits(0x7FC00000u)==0.0f&&edvr::eyeshare::fromBits(0xBF800000u)==0.0f&&edvr::eyeshare::fromBits(0x40000000u)==1.0f,
          "F106: a slot that is NaN or negative reads 0 and one above 1 reads 1: never shared by accident");
  }
  {   // World scale: a version 11 answer carries the percent; every older answer, a failed frame and a word out of range read Elite's own scale or the nearest end of the range
    Provider p;p.maxVersion=11;uint32_t v=11;EdvrNativeFrameOutput out{};
    check(ladder(p,v,out)==S_OK&&out.version==11&&v==11&&p.calls==1&&out.worldScalePercent==130&&out.reservedYaw==0.4f&&out.leftEyeShare==0x3F19999Au,
          "world scale: a version 11 d3d11.dll: one call, the percent arrives beside the version 10 words");
    Provider old;old.maxVersion=10;old.scribble=true;uint32_t v10=11;EdvrNativeFrameOutput out10{};
    check(ladder(old,v10,out10)==S_OK&&out10.version==10&&v10==10&&old.calls==2&&out10.worldScalePercent==100&&out10.reservedYaw==0.4f,
          "world scale: a version 10 d3d11.dll refuses the 11 ask, is answered at 10, and a percent it scribbles into the slot is not believed: 100 (Elite's own scale)");
    Provider older;older.maxVersion=8;older.scribble=true;uint32_t v8=11;EdvrNativeFrameOutput out8{};
    check(ladder(older,v8,out8)==S_OK&&out8.version==8&&out8.worldScalePercent==100,"...an older d3d11.dll likewise reads 100");
    Provider failing;failing.fail=E_FAIL;uint32_t vf=11;EdvrNativeFrameOutput outf{};outf.worldScalePercent=130;
    check(ladder(failing,vf,outf)==E_FAIL&&outf.worldScalePercent==100&&vf==11,"world scale: a provider that fails the frame leaves no percent behind, and is not mistaken for an old one");
    bool held=true;
    for(const auto& c:{std::pair<uint32_t,uint32_t>{0,100},{49,50},{50,50},{200,200},{201,200},{4000000000u,200}}) {
      Provider q;q.maxVersion=11;q.worldScale=c.first;uint32_t vq=11;EdvrNativeFrameOutput outq{};
      held=held&&ladder(q,vq,outq)==S_OK&&outq.worldScalePercent==c.second;
    }
    check(held,"world scale: a version 11 word of 0 reads 100 and one outside 50..200 is held to the nearest end");
    check(Client::ask(11).worldScalePercent==0,"world scale: a fresh ask carries no percent");
  }
}
} // namespace edvr::openxr::test
