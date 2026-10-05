#pragma once
#include "../../src/d3d11/flat_domain_depth_route.h"
#include <cstdio>

inline int flatDomainDepthRouteTests() {
    using namespace edvr;
    int failures=0;
    auto check=[&](bool pass,const char* message){if(!pass){std::printf("FAIL: flat depth route %s\n",message);++failures;}};
    FlatDomainDepthRoute<4> route;
    int unrelated=0,scene=0,another=0,fourth=0,overflow=0;
    route.noteUnknownMutation(50914);
    check(route.unknownMutation(50914) && route.count(50914)==0,
          "unknown mutation before any candidate is retained for H refusal");
    check(!route.unknownMutation(50915),"unknown mutation cannot bleed into another frame");
    FlatDomainDepthRoute<2> unknownBefore;
    unknownBefore.noteUnknownMutation(50915);unknownBefore.observe(&scene,50915);
    check(unknownBefore.selected(&scene,50915)>=0 && unknownBefore.unknownMutation(50915),
          "later scene selection still sees the pre-candidate unknown mutation");
    const auto first=route.observe(&unrelated,50915);
    check(first.slot>=0 && first.created,"q1 native-sized pass is retained provisionally");
    const auto actual=route.observe(&scene,50915);
    check(actual.slot>=0 && actual.slot!=first.slot && route.selected(&scene,50915)==actual.slot,
          "later scene DSV selects its own slot without inheriting first pass");
    const char* failuresBySlot[4]{};
    failuresBySlot[first.slot]="foreground-original-camera-unavailable";
    check(!failuresBySlot[route.selected(&scene,50915)],
          "unrelated q1 camera failure cannot poison selected H");
    failuresBySlot[actual.slot]="foreground-original-shader-unavailable";
    check(failuresBySlot[route.selected(&scene,50915)]==failuresBySlot[actual.slot],
          "unknown writer on selected scene remains a refusal");
    check(route.count(50915)==2 && route.selected(&unrelated,50916)<0,
          "H selection requires an observation in its own frame");
    route.observe(&another,50915);route.observe(&fourth,50915);
    route.pin(&scene);
    check(route.observe(&overflow,50915).slot<0 && route.overflowed(50915) &&
          route.selected(&overflow,50915)<0,"bounded overflow cannot fabricate a selected candidate");
    route.beginFrame(50916);
    const auto kept=route.observe(&scene,50916);
    check(kept.slot==actual.slot && !kept.created && !route.overflowed(50916),
          "consecutive selected DSV retains its history slot");
    const auto recycled=route.observe(&overflow,50916);
    check(recycled.slot>=0 && recycled.created && recycled.slot!=kept.slot,
          "an inactive candidate can be recycled without losing selected history");
    FlatDomainDepthRoute<2> pinned;
    pinned.observe(&unrelated,600);const auto oldScene=pinned.observe(&scene,600);
    pinned.pin(&scene);pinned.beginFrame(601);
    pinned.observe(&another,601);
    check(pinned.observe(&scene,601).slot==oldScene.slot,
          "an early unrelated DSV cannot evict the prior selected depth before it appears");
    if(!failures)std::puts("flat depth route: premature latch, selected unknown writer, cap and history PASS");
    return failures;
}
