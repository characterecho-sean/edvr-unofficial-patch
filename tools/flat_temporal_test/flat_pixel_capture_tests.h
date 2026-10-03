#pragma once
#include "../../src/d3d11/flat_pixel_capture_policy.h"
#include <cstdio>
inline int flatPixelCaptureTests() {
    using edvr::FlatPixelCapturePolicy;
    int failures=0;
    auto check=[&](bool ok,const char* what) {if(!ok){std::printf("FAIL: flat pixel capture %s\n",what);++failures;}};
    FlatPixelCapturePolicy p;
    check(!p.due(1),"inactive capture cannot reserve copies");
    p.arm(100,1000);
    check(p.due(100) && p.reserve(100,1000,32),"manual arm admits initial copy");
    check(!p.reserve(100,1000,32) && p.reserve(101,1001,32) && p.pendingCount==2,
          "second live frame can queue while first readback is pending, without duplicating one frame");
    check(!p.reserve(102,1002,32) && p.copied==2 && p.bytes==64,"a third readback waits for the pair");
    check(!p.pendingExpired(220,5999) && p.pendingExpired(221,5999) && p.pendingExpired(102,6001),"pending set bounded by either frames or wallclock");
    p.finish(false);
    p.finish(true);
    check(p.failed==1 && !p.pending && !p.due(115) && p.due(116),"failed readback counts and keeps later-pair spacing");
    check(p.reserve(116,1100,FlatPixelCapturePolicy::maxBytes-64),"exact total byte cap admitted");
    p.finish(true);
    check(!p.fits(1) && !p.reserve(130,1200,1),"cumulative byte cap includes failed first set");
    p.arm(200,2000);
    check(p.bytes==0 && p.failed==0 && p.copied==0 && !p.pending,"rearm discards prior budget and pending state");
    for(unsigned i=0;i<4;++i) {
        const unsigned frame=200+(i/2)*16+(i%2);
        check(p.reserve(frame,2000+i,16),"two adjacent pairs admitted");p.finish(true);
    }
    check(p.completed==4 && !p.due(300) && !p.reserve(300,2300,16),"burst cannot exceed four sets");
    check(!p.expired(1099,31999) && p.expired(1100,31999) && p.expired(201,32000),"arm expires by either frames or wallclock");
    check(p.expired(199,2000) && p.expired(200,1999),"frame or clock rewind ends burst");
    check(!p.fits(0) && !p.fits(UINT64_MAX),"zero and overflow-size requests refused");
    // F3 (2026-09-29): a reset frame is never a sample; the second copy can queue
    // before the first readback completes so the evidence really is consecutive.
    p.arm(500,5000);
    check(!p.due(500,false) && !p.reserve(500,5000,32,false) && p.copied==0 && p.bytes==0,
          "a reset frame is not sample 1 and changes nothing");
    check(p.due(501) && p.reserve(501,5001,32),"the next live frame is sample 1");
    check(!p.due(501) && p.due(502) && !p.due(502,false),
          "sample 2 is due the next frame while sample 1 is pending -- and not on a reset frame");
    check(p.reserve(502,5002,32),"sample 2 admitted on the next live frame");
    check(p.pendingCount==2,"both readbacks can coexist");
    p.finish(true);
    p.finish(true);
    check(!p.due(516) && p.due(517),"sample 3 waits out the old spacing after sample 2");
    p.arm(600,6000);
    check(p.reserve(600,6000,32),"rearm: sample 1");
    p.finish(false);
    check(!p.due(601) && !p.due(614) && p.due(615),
          "a FAILED first sample keeps the old spacing for its retry (a retry is not a second sample)");
    // The live-frame rule both captures share.
    using edvr::flatCaptureFrameLive;
    check(!flatCaptureFrameLive(true,true) && !flatCaptureFrameLive(true,false) &&
          !flatCaptureFrameLive(false,false) && flatCaptureFrameLive(false,true),
          "a live frame did not reset and ran at a nonzero phase");
    check(flatCaptureFrameLive(false,false,false) && !flatCaptureFrameLive(true,false,false),
          "with the jitter off on purpose phase 0 is what a live frame is, but a reset frame never is");
    return failures;
}
