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
    check(!p.reserve(120,1020,32) && p.copied==1 && p.bytes==32,"one outstanding set prevents duplicate copies");
    check(!p.pendingExpired(219,5999) && p.pendingExpired(220,5999) && p.pendingExpired(101,6000),"pending set bounded by either frames or wallclock");
    p.finish(false);
    check(p.failed==1 && !p.due(114) && p.due(115),"failed readback counts and keeps sample spacing");
    check(p.reserve(115,1100,FlatPixelCapturePolicy::maxBytes-32),"exact total byte cap admitted");
    p.finish(true);
    check(!p.fits(1) && !p.reserve(130,1200,1),"cumulative byte cap includes failed first set");
    p.arm(200,2000);
    check(p.bytes==0 && p.failed==0 && p.copied==0 && !p.pending,"rearm discards prior budget and pending state");
    for(unsigned i=0;i<4;++i) {check(p.reserve(200+15*i,2000+i,16),"four scheduled samples admitted");p.finish(true);}
    check(p.completed==4 && !p.due(300) && !p.reserve(300,2300,16),"burst cannot exceed four sets");
    check(!p.expired(1099,31999) && p.expired(1100,31999) && p.expired(201,32000),"arm expires by either frames or wallclock");
    check(p.expired(199,2000) && p.expired(200,1999),"frame or clock rewind ends burst");
    check(!p.fits(0) && !p.fits(UINT64_MAX),"zero and overflow-size requests refused");
    return failures;
}
