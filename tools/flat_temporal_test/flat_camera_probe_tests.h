#pragma once
#include "../../src/d3d11/flat_camera_probe.h"
#include <cstdio>
#include <cstring>

inline int flatCameraProbeTests() {
    using namespace edvr;
    int failures=0;
    auto check=[&](bool ok,const char* why) {
        if(!ok){std::printf("FAIL: flat camera probe %s\n",why);++failures;}
    };
    FlatCameraProbe probe{};
    const auto& old=kFlatCameraProbePairs[0];
    const auto& material=kFlatCameraProbePairs[1];
    const auto& effect=kFlatCameraProbePairs[2];
    check(!probe.begin(false,material.vs,material.ps,10,true),"no arm leaves state untouched");
    check(probe.pairs[1].observed==0,"no arm has no observation");
    check(!probe.begin(true,material.vs,effect.ps,10,true),"cross PS refused");
    check(!probe.begin(true,material.vs,old.ps,10,true),"cross old PS refused");
    check(!probe.begin(true,0,0,10,true),"unknown refused");
    check(!probe.begin(true,old.vs,old.ps,10,false),"nonconflicting old pair observed only");
    check(probe.pairs[0].observed==1 && probe.pairs[0].attempts==0 &&
          !std::strcmp(probe.pairs[0].result(),"observed-without-HDR-camera-conflict"),"old result intact");
    const uint32_t first=probe.begin(true,material.vs,material.ps,20,true);
    check(first==3 && FlatCameraProbe::pairForToken(first)==1,"material first token");
    probe.finish(first,true,true);
    check(!probe.begin(true,material.vs,material.ps,20,true),"same frame cannot take second sample");
    const uint32_t effectFirst=probe.begin(true,effect.vs,effect.ps,20,true);
    check(effectFirst==5,"effect budget independent on same frame");
    probe.finish(effectFirst,false,true);
    const uint32_t second=probe.begin(true,material.vs,material.ps,21,true);
    check(second==4,"material second later frame");
    probe.finish(second,true,false);
    check(!probe.begin(true,material.vs,material.ps,22,true),"material cap is two");
    const uint32_t effectSecond=probe.begin(true,effect.vs,effect.ps,21,true);
    check(effectSecond==6,"effect second unaffected by material cap");
    probe.finish(effectSecond,true,true);
    check(probe.pairs[1].attempts==2 && probe.pairs[1].complete==1 &&
          probe.pairs[1].actualMismatch==1 &&
          !std::strcmp(probe.pairs[1].result(),"actual-shader-mismatch"),"material mismatch reported");
    check(probe.pairs[2].attempts==2 && probe.pairs[2].missing==1 &&
          probe.pairs[2].complete==1 &&
          !std::strcmp(probe.pairs[2].result(),"partial-missing-evidence"),"effect partial reported");
    check(probe.pairs[0].attempts==0,"old pair remains unconsumed");
    probe={};
    check(probe.pairs[1].observed==0 && probe.pairs[2].attempts==0 &&
          !std::strcmp(probe.pairs[2].result(),"exact-pair-never-observed"),"rearm clears every pair");
    check(probe.begin(true,old.vs,old.ps,23,true)==1,"old pair still captures after rearm");
    return failures;
}
