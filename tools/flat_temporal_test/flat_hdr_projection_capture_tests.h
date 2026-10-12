#pragma once
#include <cstdio>
#include <initializer_list>
#include "../../src/d3d11/flat_hdr_projection_capture.h"
inline int flatHdrProjectionCaptureTests() {
    using namespace edvr;
    int failures=0;
    const auto expect=[&](bool ok,const char* name){if(!ok){std::printf("FAIL: HDR payload capture %s\n",name);++failures;}};
    const void* h=reinterpret_cast<void*>(1);const void* depth=reinterpret_cast<void*>(2);
    FlatHdrProjectionCapture c;
    expect(!c.begin(12,h,depth,0) && !c.writer(12,h),"disabled leaves draws untouched");
    c.arm();expect(!c.complete() && !c.target,"armed but never ran");
    expect(c.begin(12,h,depth,0),"anchor begins forward frame");c.donors=1;
    expect(!c.writer(11,h) && !c.writer(12,depth),"wrong frame and unrelated target excluded");
    expect(c.writer(12,h),"first actual H writer");++c.payloads;
    expect(!c.complete(),"payload alone cannot close capture");
    c.closed=true;expect(c.complete() && !c.writer(12,h),"closed payload bounded by handoff");
    auto bad=c;bad.donors=2;expect(!bad.complete(),"competing donors explicit");
    bad=c;bad.payloads=0;expect(!bad.complete(),"missing original shadow explicit");
    bad=c;bad.partial=true;expect(!bad.complete(),"earlier uncaptured writer explicit");
    bad=c;bad.uncertain=true;expect(!bad.complete(),"foreign work explicit");
    bad=c;bad.closed=false;bad.written(depth);bad.closed=true;expect(!bad.complete(),"intervening depth mutation explicit");
    c.finishFrame();expect(!c.begin(12,h,depth,0),"two frames must be distinct");
    expect(!c.begin(102,h,depth,0),"90-frame spacing is too short");
    expect(c.begin(103,h,depth,0),"91-frame spacing is eligible");
    for(uint32_t i=0;i<FlatHdrProjectionCapture::kWriterLimit;++i)expect(c.writer(103,h),"within bounded writer budget");
    expect(!c.writer(103,h) && c.overflow,"overflow distinguishable from complete");
    c.finishFrame();expect(c.begin(194,h,depth,0),"incomplete attempt retries after 91-frame spacing");
    c.finishFrame();expect(c.begin(285,h,depth,0),"fourth bounded attempt");
    c.finishFrame();expect(!c.begin(376,h,depth,0),"attempt budget exhausted");
    c.arm();for(uint64_t frame:{12ull,103ull}) {
        expect(c.begin(frame,h,depth,0),"complete-frame attempt");c.donors=1;
        c.writer(frame,h);c.payloads=1;c.closed=true;c.finishFrame();
    }
    expect(c.completeFrames==2 && !c.begin(194,h,depth,0),"stop after two complete payloads");
    c.arm();uint32_t residues=0;uint64_t frame=0;
    for(unsigned attempt=0;attempt<FlatHdrProjectionCapture::kAttemptLimit;++attempt) {
        expect(c.begin(frame,h,depth,0),"stable three-frame cycle attempt eligible");
        residues|=1u<<static_cast<unsigned>(frame%3u);
        c.finishFrame();frame+=FlatHdrProjectionCapture::kMinimumFrameSpacing;
    }
    expect(residues==0x7u,"four spaced attempts visit all three stable-cycle residues");
    expect(flatHdrCaptureRange(7,17,10,7),"shader relative nonzero first range");
    expect(!flatHdrCaptureRange(0,16,10,7) && !flatHdrCaptureRange(UINT32_MAX,17,10,7) &&
        !flatHdrCaptureRange(0,UINT32_MAX,UINT32_MAX,1),"short and overflowing binding ranges");
    return failures;
}
