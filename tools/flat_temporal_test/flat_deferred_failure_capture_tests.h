#pragma once
#include <memory>
#include <vector>
#include <fstream>
#include <string>
#include "../../src/d3d11/flat_trace.h"
inline unsigned flatDeferredFailureCaptureTests() {
    using namespace edvr;
    unsigned failures=0;
    auto expect=[&](bool ok,const char* what){if(!ok){std::printf("FAIL: deferred failure snapshot %s\n",what);++failures;}};
    FlatTraceFailureCapture capture;
    expect(capture.reportNeverRan() && !capture.reportNeverRan() && capture.available,
           "resize never-ran report one-shot without consuming capture capability");
    capture.arm();expect(capture.reportNeverRan() && !capture.reportNeverRan(),"manual arm reopens only one never-ran report");
    expect(capture.request(42) && !capture.request(43),"one pending actual failure");
    expect(!capture.ready(41) && capture.ready(42),"completion waits for requested frame");
    capture.finish(false);
    expect(capture.fullWindow(43) && capture.request(44),"unavailable first frame retries next actual failure with full capacity");
    capture.finish(false);
    expect(!capture.request(45) && !capture.fullWindow(45),"two failed attempts exhaust budget");
    capture.arm();expect(capture.request(50),"manual rearm opens new bounded budget");
    capture.arm();expect(capture.pendingFrame==50,"manual rearm preserves pending failure");
    capture.finish(true);expect(!capture.request(51),"emitted failure ends automatic capture");
    capture.arm();capture.request(60);capture.finish(false);
    expect(capture.expired(961) && !capture.request(961),"retry without failure expires at finite deadline");
    capture.cancel();expect(!capture.pendingFrame && !capture.available,"reset reports and cancels pending lifetime");
    auto ring=std::make_unique<FlatTraceRing>();
    flatTraceBeginFrame(*ring,42,reinterpret_cast<void*>(1),1248,780,28);
    flatTraceMark(*ring,kFlatTraceEventMarkUncertain,nullptr);
    auto dump=[&](bool current){std::vector<unsigned char> bytes;flatTraceDump(*ring,[&](const void* p,uint32_t n){const auto* b=static_cast<const unsigned char*>(p);bytes.insert(bytes.end(),b,b+n);return n;},current);return bytes;};
    auto frames=[&](const std::vector<unsigned char>& bytes){unsigned count=0;flatTraceParse(bytes.data(),bytes.size(),[&](const FlatTraceFrameHeader& h){expect(h.frame==42 && h.eventCount==1 && !h.truncated,"serialized exact completed failure frame");++count;},[](const FlatTraceEvent&){});return count;};
    expect(frames(dump(true))==0,"unsealed current never emitted even with opt-in");
    flatTraceSeal(*ring,false,0);
    expect(frames(dump(false))==0,"manual default still excludes sealed current");
    expect(frames(dump(true))==1,"completed-current opt-in includes the actual failed frame");
    flatTraceMark(*ring,kFlatTraceEventMarkUncertain,nullptr);
    expect(frames(dump(true))==0,"post-seal append revokes completion");
    flatTraceSeal(*ring,false,0);ring->headers[ring->slot].truncated=1;
    expect(frames(dump(true))==0,"overflowed failure never claimed captured");
    flatTraceBeginFrame(*ring,43,reinterpret_cast<void*>(1),1248,780,28);
    flatTraceMark(*ring,kFlatTraceEventMarkUncertain,nullptr);
    flatTraceBeginFrame(*ring,44,reinterpret_cast<void*>(1),1248,780,28);
    expect(frames(dump(true))==0,"unsealed noncurrent slot excluded in diagnostic mode");
    // Pin the actual integration lifetime as well as the pure serializer:
    // inspect only the bounded frame-boundary region, never retain the full TU.
    std::ifstream source("src/d3d11/flat_runtime.cpp");std::string line,boundary;
    bool reading=false;
    while(std::getline(source,line)) {
        if(line.find("// Trace ring: seal the frame that just ended")!=std::string::npos)reading=true;
        if(!reading)continue;
        boundary+=line+'\n';
        if(line.find("flatTraceBeginFrame(s.traceRing")!=std::string::npos)break;
        if(boundary.size()>8192)break;
    }
    const auto seal=boundary.find("flatTraceSeal(s.traceRing");
    const auto ready=boundary.find("s.deferredFailureCapture.ready");
    const auto emit=boundary.find("flatTraceDumpToLogDir(s,failed,true)");
    const auto finish=boundary.find("s.deferredFailureCapture.finish(written)");
    const auto begin=boundary.find("flatTraceBeginFrame(s.traceRing");
    expect(seal!=std::string::npos && ready!=std::string::npos && emit!=std::string::npos &&
           finish!=std::string::npos && begin!=std::string::npos && seal<ready && ready<emit && emit<finish && finish<begin,
           "runtime seals and emits actual failure before completion and next BeginFrame");
    std::printf("flat deferred failure snapshot: %u failure(s)\n",failures);
    return failures;
}
