#pragma once
#include "../../src/d3d11/flat_hdr_route.h"
#include "../../src/d3d11/flat_trace.h"
#include <cstdio>
#include <cstring>
#include <memory>

namespace laser_scene_tone_test {
using namespace edvr;
inline const void* token(uintptr_t n) { return reinterpret_cast<const void*>(n); }
enum class Scenario {
    Qualified, MissingSource, FirstPersonSource, StaleSource, WrongDsv,
    WrongLayout, AmbiguousSources, ChangedCamera, SameHashDifferentBytes,
    DepthMutation, BadHdr, LateSource, UnsupportedAlternate, NoImageSource
};
struct Result {
    bool tone = false, imageAccepted = false, imageRefused = false, hdrBad = false;
    FlatMonoReason route = FlatMonoReason::InvalidInput;
    uint32_t sourceCount = 0, sourceLast = 0, laserQ = 0, toneFirst = 0, toneWriteSeq = 0;
    uint64_t toneWriteEpoch = 0;
};
inline void camera(FlatRuntimeDraw& d, uint64_t frame, uint32_t q, float nearPlane = .025f) {
    float rows[6][4]{};
    rows[0][0] = rows[1][1] = rows[2][3] = 1.f;
    rows[4][2] = rows[2][3];
    rows[3][2] = nearPlane;
    rows[5][0] = 1.75f; rows[5][1] = 3.f; rows[5][2] = -13.f;
    std::memcpy(d.camera, rows, sizeof(rows));
    d.key.camera = d.camera;
    d.key.cameraHash = flatCameraHash(d.camera);
    d.key.b1 = token(0x3000);
    d.key.writeEpoch = frame;
    d.key.writeSeq = q;
}
inline FlatRuntimeDraw draw(const void* colour, uint32_t format, const void* depth,
                           const void* dsv, uint64_t vs, uint64_t ps) {
    FlatRuntimeDraw d{};
    d.key.color = colour; d.key.rtv = token(reinterpret_cast<uintptr_t>(colour) + 1);
    d.key.format = format; d.key.width = d.key.depthWidth = 1920;
    d.key.height = d.key.depthHeight = 1200;
    d.key.depth = depth; d.key.dsv = dsv; d.key.depthFormat = depth ? 19 : 0;
    d.key.viewportCount = 1; d.key.viewport[2] = 1920; d.key.viewport[3] = 1200;
    d.key.viewport[5] = 1;
    d.key.vs = vs; d.key.ps = ps; d.instances = 1;
    return d;
}
inline Result replay(Scenario scenario) {
    constexpr uint64_t frame = 14847;
    const void *depth = token(0x2000), *dsv = token(0x2001),
               *pool = token(0x1000), *hdr = token(0x4000),
               *image = token(0x5000), *output = token(0x9000);
    auto p = std::make_unique<FlatRuntimePrefix>();
    p->frame = frame; p->output = output; p->width = 1920; p->height = 1200; p->format = 26;
    FlatHdrFrame route{}; flatHdrBeginFrame(route,1920,1200);
    auto apply = [&](FlatRuntimeDraw& d) {
        flatRuntimeObserve(*p,d);
        flatHdrObserveDraw(route,d.key,p->sequence,nullptr,false);
    };
    if (scenario != Scenario::MissingSource && scenario != Scenario::LateSource) {
        auto source = draw(pool,23,depth,scenario == Scenario::WrongDsv ? token(0x2BAD) : dsv,
                           0xBFE51414CC3024B4ull,0xDB79AE788E049DFDull);
        source.key.kind = kFlatContractPool; source.supported = true;
        if (scenario == Scenario::WrongLayout) source.key.width = source.key.depthWidth = 1919;
        source.firstPersonCohort = scenario == Scenario::FirstPersonSource;
        camera(source,scenario == Scenario::StaleSource ? frame-1 : frame,1);
        apply(source);
    }
    if (scenario == Scenario::AmbiguousSources) {
        auto other = draw(token(0x1100),23,depth,dsv,0xA1,0xB1);
        other.key.kind = kFlatContractPool; other.supported = true;
        camera(other,frame,p->sequence+1,.0675f);
        apply(other);
    }
    if (scenario == Scenario::UnsupportedAlternate) {
        auto other = draw(token(0x1100),60,depth,dsv,0x53211E8C072CD02Eull,0x7EAC71963E66C5FEull);
        other.key.kind = kFlatContractPool; other.supported = false;
        camera(other,frame,p->sequence+1,.0675f);
        apply(other);
    }
    auto h = draw(hdr,26,depth,dsv,0x7E38A6AA1269C901ull,0x7CECABDE34FFBE9Eull);
    h.key.kind = kFlatContractScreen;
    if (scenario == Scenario::BadHdr) h.key.viewport[0] = 1;
    apply(h);
    if (scenario == Scenario::LateSource) {
        // A valid donor observed after H began may not certify H's earlier writers.
        auto late = draw(pool,23,depth,dsv,0xBFE51414CC3024B4ull,0xDB79AE788E049DFDull);
        late.key.kind = kFlatContractPool; late.supported = true;
        camera(late,frame,p->sequence+1);
        apply(late);
    }
    auto imageSource = draw(image,9,depth,dsv,0xCFA91824129ECBBCull,0xFCFAD73924BF45B9ull);
    imageSource.imageSourceCameraIndependentVerified = true;
    if (scenario != Scenario::NoImageSource) apply(imageSource);
    if (scenario == Scenario::DepthMutation) flatRuntimeWritten(*p,depth);
    auto laser = draw(hdr,26,depth,dsv,0x88DCF1164C640EC3ull,0x494506A63091DF8Cull);
    laser.key.kind = kFlatContractScreen;
    camera(laser,frame,p->sequence+1,scenario == Scenario::ChangedCamera ? .0675f : .025f);
    if (scenario == Scenario::SameHashDifferentBytes) {
        laser.camera[80] ^= 1;
        laser.key.cameraHash = p->sources[0].key.cameraHash;
        laser.cameraHashTrusted = true; // isolate exact-byte check from the hash check
    }
    apply(laser);
    const uint32_t laserQ = p->sequence;
    const FlatRuntimeTarget* t = nullptr;
    for(uint32_t i=0;i<p->targetsUsed;++i) if(p->targets[i].resource==hdr)t=&p->targets[i];
    const bool tone = t && t->hdrCamera && flat_mono_detail::cameraCurrent(t->tone,frame);
    auto copy = draw(hdr,26,nullptr,nullptr,0xCFA91824129ECBBCull,0xDFCBA0EC70B03C9Bull);
    copy.key.depthWidth = copy.key.depthHeight = 0;
    copy.key.srvResource[0] = image; copy.key.srvView[0] = token(0x5002);
    copy.hdrCopyVerified = true;
    apply(copy);
    auto consumer = draw(token(0x6000),26,nullptr,nullptr,0xDFED8E1C9E191BECull,0x14C60393D1FC9766ull);
    consumer.key.width = 960; consumer.key.height = 600;
    consumer.key.depthWidth = consumer.key.depthHeight = 0;
    consumer.key.viewport[2] = 960; consumer.key.viewport[3] = 600;
    flatRuntimeObserve(*p,consumer);
    const void* srv[4] = {hdr,nullptr,nullptr,nullptr};
    const bool triggered = flatHdrObserveDraw(route,consumer.key,p->sequence,srv,true);
    Result result{}; result.tone = tone; result.imageAccepted = p->imageCopiesAccepted == 1;
    result.imageRefused = p->imageCopiesRefused == 1;
    result.hdrBad = t && t->hdrBad;
    result.sourceCount = p->sourcesUsed;
    result.sourceLast = p->sourcesUsed ? p->sources[0].last : 0;
    result.laserQ = laserQ;
    if(t){result.toneFirst=t->tone.first;result.toneWriteEpoch=t->tone.firstWriteEpoch;
      result.toneWriteSeq=t->tone.firstWriteSeq;}
    if (triggered) result.route = flatSelectHdrRoute(*p,route,[](uint64_t,uint64_t){return true;}).reason;
    return result;
}
inline unsigned run() {
    unsigned failures = 0;
    auto expect = [&](bool ok,const char* label){if(!ok){std::printf("FAIL: laser scene tone %s\n",label);++failures;}};
    const Result good = replay(Scenario::Qualified);
    expect(good.sourceCount==1 && good.tone && good.imageAccepted && !good.imageRefused &&
           !good.hdrBad && good.route==FlatMonoReason::Selected,
           "FTR5-order source/H/image/laser/copy/consumer qualifies the HDR route");
    expect(good.toneFirst==good.laserQ && good.sourceLast<good.toneFirst &&
           good.toneWriteEpoch==14847 && good.toneWriteSeq==good.laserQ,
           "H tone provenance belongs to the actual laser H draw, not a backdated pool or copy");
    const Result unsupported = replay(Scenario::UnsupportedAlternate);
    expect(unsupported.sourceCount==1 && unsupported.tone && unsupported.imageAccepted &&
           unsupported.route==FlatMonoReason::Selected,
           "unsupported alternate-camera format-60 draw is not a donor or ambiguity");
    const Scenario bad[] = {Scenario::MissingSource,Scenario::FirstPersonSource,
      Scenario::StaleSource,Scenario::WrongDsv,Scenario::WrongLayout,
      Scenario::AmbiguousSources,Scenario::ChangedCamera,Scenario::SameHashDifferentBytes,
      Scenario::DepthMutation,Scenario::BadHdr,Scenario::LateSource,Scenario::NoImageSource};
    for(const auto scenario:bad){const Result r=replay(scenario);
      expect(!r.tone && !r.imageAccepted && r.imageRefused && r.route!=FlatMonoReason::Selected,
             "missing/stale/alternate/ambiguous/mutated source cannot establish scene H camera");
    }
    std::printf("flat laser scene tone: %u failure(s)\n",failures);
    return failures;
}
} // namespace laser_scene_tone_test
