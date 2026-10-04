// The HDR route: resolve the game's HDR scene image once a frame, before its post chain
// (docs/design-flat-temporal-aa-2026-09-23.md, section 81).
//
// WHY. Today's flat route resolves the tone-mapped image at the game's output copy, so it has to
// recognise the whole chain between the scene and the copy (bloom, depth of field, tone variants,
// the game's own AA): a treadmill of whitelisted shader pairs (sections 63-79). The HDR route
// resolves H itself, the R11G11B10F scene target the game draws its lighting, particles and glass
// into, at the first full-scale pass that reads it after its last write, and writes the result back
// into H. Bloom, depth of field, tone and grades then act on an anti-aliased image and the chain
// shape stops mattering.
//
// WHAT IS HERE, all pure (tools\flat_temporal_test drives every function on the trace corpus and on
// mutated streams, and the runtime calls the very same code):
//   - the trigger detector: the first draw after H's first write with no depth bound, a target that is
//     not H, H bound as a pixel-shader resource at t0..t3 and a target at least half of H per axis;
//   - the late-write accounting after it (a draw, a dispatch or an explicit write into H) and the latch
//     that turns the route off after three treated frames with such writes;
//   - the selector, a sibling of flatSelectMonoFrame over the prefix model's records so far, without the
//     tone and copy requirements, and the extent gate R >= D;
//   - the per-window census token and the log lines.
// It never touches D3D: the runtime hands it resource identities and sizes.
#pragma once
#include "flat_runtime_model.h"
#include "flat_mono_resolve.h"
#include "flat_standdown.h"
#include "hdr_backend_flags.h"
#include <cstdio>
#include <cstring>

namespace edvr {

// ---- the key --------------------------------------------------------------------------
// experimental.temporal_aa_before_post: auto (the HDR route where it applies) or off (the copy route only; the
// trigger still runs, observing). Sean's decision (a): off until it had flown, then the default becomes auto; it flew
// on 2026-09-30 and the default is auto since (design section 81). A key that is absent reads as the default; a value
// that is present and is not "auto" reads as off, so a typo leaves the copy route every frame had before the route
// existed and never switches the route on by accident.
enum class FlatHdrKey : uint8_t { Off, Auto };
inline FlatHdrKey flatHdrKeyFromText(const char* text) {
    if (!text) return FlatHdrKey::Off;
    const char* a = "auto";
    for (; *a; ++a, ++text) {
        char c = *text;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != *a) return FlatHdrKey::Off;
    }
    return *text == 0 ? FlatHdrKey::Auto : FlatHdrKey::Off;
}
inline const char* flatHdrKeyName(FlatHdrKey key) { return key == FlatHdrKey::Auto ? "auto" : "off"; }

// ---- the trigger detector -------------------------------------------------------------------
constexpr uint32_t kFlatHdrCandidates = 4;
// A draw into a target is the route's H candidate when it is an R11G11B10F target at the scene's extent
// with a depth target of its own size bound: what the game's lighting, particles and glass draw into.
// The extent is the prefix model's own screen band (flatContractKind), so a 1024x1024 probe with a
// depth does not qualify.
inline bool flatHdrCandidateDraw(const FlatContractObservation& k, uint32_t outW, uint32_t outH) {
    return k.format == 26 && k.color && k.depth && k.dsv && k.width && k.height &&
        k.depthWidth == k.width && k.depthHeight == k.height &&
        flatContractKind(false, k.color, k.depth, k.width, k.height, 26, outW, outH, false) == kFlatContractScreen;
}
// Rule (iv): a target at least half of H per axis. A bloom chain's first level is exactly half and an
// odd extent halves down, so one pixel of slack; the exposure reduction at a quarter per axis is far
// outside it.
inline bool flatHdrHalfOrMore(uint32_t w, uint32_t h, uint32_t hdrW, uint32_t hdrH) {
    return w && h && uint64_t(w) * 2 + 1 >= hdrW && uint64_t(h) * 2 + 1 >= hdrH;
}

struct FlatHdrCandidate {
    const void* resource = nullptr;
    uint32_t width = 0, height = 0;
    uint32_t firstSeq = 0, lastSeq = 0, draws = 0;
};
struct FlatHdrTrigger {
    const void* hdr = nullptr;
    uint32_t hdrWidth = 0, hdrHeight = 0;
    uint32_t sequence = 0;            // the consumer draw's place in the prefix (the model's q)
    uint32_t srvSlot = ~0u;           // the pixel-shader slot that binds H; ~0u when the SRVs were not known
    uint64_t vs = 0, ps = 0;
    uint32_t targetWidth = 0, targetHeight = 0, targetFormat = 0;
    bool srvKnown = false;
    bool ambiguous = false;           // more than one H candidate matched the consumer
};
struct FlatHdrFrame {
    uint32_t outputWidth = 0, outputHeight = 0;
    FlatHdrCandidate candidates[kFlatHdrCandidates]{};
    uint32_t candidateCount = 0;
    bool candidateOverflow = false;   // a fifth candidate: nothing can be named
    bool triggered = false;
    FlatHdrTrigger trigger{};
    // After the trigger: writes into any H candidate. The resolve's result is in H for the draws that read
    // it next; a write after it lands on top of the un-resolved, jittered image.
    uint32_t lateDraws = 0, lateDispatches = 0, lateExplicit = 0;
    bool lateNewCandidate = false;    // a candidate that did not exist at the trigger (H changed identity)
    bool lateNamed = false;
    uint32_t lateSequence = 0;
    uint64_t lateVs = 0, latePs = 0;
    uint32_t lateTargetWidth = 0, lateTargetHeight = 0;
};
inline uint32_t flatHdrLateWrites(const FlatHdrFrame& f) { return f.lateDraws + f.lateDispatches + f.lateExplicit; }
inline void flatHdrBeginFrame(FlatHdrFrame& f, uint32_t outputWidth, uint32_t outputHeight) {
    f = FlatHdrFrame{};
    f.outputWidth = outputWidth; f.outputHeight = outputHeight;
}
inline const FlatHdrCandidate* flatHdrFindCandidate(const FlatHdrFrame& f, const void* resource) {
    if (!resource) return nullptr;
    for (uint32_t i = 0; i < f.candidateCount; ++i) if (f.candidates[i].resource == resource) return &f.candidates[i];
    return nullptr;
}
// The runtime's cheap gate before it resolves the four shader-resource slots: rules (i), (ii) and (iv)
// against some candidate. Most draws of a frame fail it on the first comparison.
inline bool flatHdrCouldConsume(const FlatHdrFrame& f, const FlatContractObservation& k) {
    if (f.triggered || !f.candidateCount || !k.color || k.depth || k.dsv) return false;
    for (uint32_t i = 0; i < f.candidateCount; ++i) {
        const auto& c = f.candidates[i];
        if (c.draws && c.resource != k.color && flatHdrHalfOrMore(k.width, k.height, c.width, c.height)) return true;
    }
    return false;
}
// Every draw, in order. `srv` holds the resources bound at pixel-shader t0..t3 (null entries for none),
// `srvKnown` says the runtime resolved them: a trace recorded before the format carried them has no SRVs
// and the rule then applies (i), (ii) and (iv) only, which can fire early but never late. True at the
// trigger draw, once a frame.
inline bool flatHdrObserveDraw(FlatHdrFrame& f, const FlatContractObservation& k, uint32_t sequence,
                               const void* const* srv, bool srvKnown) {
    if (flatHdrCandidateDraw(k, f.outputWidth, f.outputHeight)) {
        if (f.triggered) {
            ++f.lateDraws;
            if (!flatHdrFindCandidate(f, k.color)) f.lateNewCandidate = true;
            if (!f.lateNamed) {
                f.lateNamed = true; f.lateSequence = sequence; f.lateVs = k.vs; f.latePs = k.ps;
                f.lateTargetWidth = k.width; f.lateTargetHeight = k.height;
            }
            return false;
        }
        auto* c = const_cast<FlatHdrCandidate*>(flatHdrFindCandidate(f, k.color));
        if (!c) {
            if (f.candidateCount == kFlatHdrCandidates) { f.candidateOverflow = true; return false; }
            c = &f.candidates[f.candidateCount++];
            c->resource = k.color; c->width = k.width; c->height = k.height; c->firstSeq = sequence;
        }
        ++c->draws; c->lastSeq = sequence;
        return false;
    }
    if (f.triggered) {
        // A draw into H that is not shaped like the scene draws (no depth, say) is a late write all the same.
        if (k.color && flatHdrFindCandidate(f, k.color)) {
            ++f.lateDraws;
            if (!f.lateNamed) {
                f.lateNamed = true; f.lateSequence = sequence; f.lateVs = k.vs; f.latePs = k.ps;
                f.lateTargetWidth = k.width; f.lateTargetHeight = k.height;
            }
        }
        return false;
    }
    if (!flatHdrCouldConsume(f, k)) return false;
    uint32_t matches = 0, slot = ~0u;
    const FlatHdrCandidate* chosen = nullptr;
    for (uint32_t i = 0; i < f.candidateCount; ++i) {
        const auto& c = f.candidates[i];
        if (!c.draws || c.resource == k.color || !flatHdrHalfOrMore(k.width, k.height, c.width, c.height)) continue;
        if (srvKnown) {
            uint32_t bound = ~0u;
            for (uint32_t s = 0; s < 4 && bound == ~0u; ++s) if (srv && srv[s] == c.resource) bound = s;
            if (bound == ~0u) continue;
            if (!chosen) slot = bound;
        }
        ++matches; if (!chosen) chosen = &c;
    }
    if (!matches) return false;
    f.triggered = true;
    auto& t = f.trigger;
    t.hdr = chosen->resource; t.hdrWidth = chosen->width; t.hdrHeight = chosen->height;
    t.sequence = sequence; t.srvSlot = slot; t.srvKnown = srvKnown;
    t.vs = k.vs; t.ps = k.ps; t.targetWidth = k.width; t.targetHeight = k.height; t.targetFormat = k.format;
    // Without SRVs a second candidate in the frame is enough to doubt which one this consumer reads.
    t.ambiguous = srvKnown ? matches > 1 : f.candidateCount > 1;
    if (f.candidateOverflow) t.ambiguous = true;
    return true;
}
// A write the prefix model sees between draws: Map, Update, Copy/Resolve destination, Clear. After the trigger
// into an H candidate it is a late write.
inline void flatHdrObserveExplicitWrite(FlatHdrFrame& f, const void* resource) {
    if (f.triggered && flatHdrFindCandidate(f, resource)) ++f.lateExplicit;
}
// A dispatch whose UAV is an H candidate.
inline void flatHdrObserveDispatchWrite(FlatHdrFrame& f, const void* resource) {
    if (f.triggered && flatHdrFindCandidate(f, resource)) ++f.lateDispatches;
}

enum class FlatHdrFrameVerdict : uint8_t { NoHdr, Trigger, NoTrigger };
// What the frame showed, for the census: no H at all (a 2D menu, a loading screen), an H with its consumer,
// an H and no consumer.
inline FlatHdrFrameVerdict flatHdrFrameVerdict(const FlatHdrFrame& f) {
    if (f.triggered) return FlatHdrFrameVerdict::Trigger;
    return f.candidateCount ? FlatHdrFrameVerdict::NoTrigger : FlatHdrFrameVerdict::NoHdr;
}

// ---- the selector ----------------------------------------------------------------------------
// How the selector bounds the scene's render size R against the output D. The HDR route resolves at R, so it needs R >= D
// (HdrExtent otherwise: the copy route and its whitelist serve a smaller R, decision (c) of section 81). The copy's
// admission by structure (flat_copy_structure.h, section 83) is the one that serves it: the game's own final copy scales R
// to D, so any uniform R from half to twice D works there (RenderSize otherwise, with the measured sizes).
enum class FlatHdrExtentGate : uint8_t { RenderAtLeastOutput, UniformHalfToDouble };
enum class FlatHdrSourceIssue : uint8_t {
    None, SameDepthLayout, InvalidProvenance, SameDepthCamera,
    WrongOrder, SecondDepthSameCamera
};
inline const char* flatHdrSourceIssueName(FlatHdrSourceIssue issue) {
    switch (issue) {
    case FlatHdrSourceIssue::SameDepthLayout: return "same-depth-layout";
    case FlatHdrSourceIssue::InvalidProvenance: return "invalid-provenance";
    case FlatHdrSourceIssue::SameDepthCamera: return "same-depth-camera";
    case FlatHdrSourceIssue::WrongOrder: return "wrong-order";
    case FlatHdrSourceIssue::SecondDepthSameCamera: return "second-depth-same-camera";
    default: return "none";
    }
}
struct FlatHdrSourceFacts {
    bool eligible = false, sameDepth = false, layoutValid = false;
    bool current = false, fullViewport = false, sameCamera = false, orderValid = false;
    FlatHdrSourceIssue issue = FlatHdrSourceIssue::None;
};
// Diagnose the selector's exact Pool candidates, using the same 34-record
// input and branch order. This reports evidence only; it never admits a source.
inline FlatHdrSourceFacts flatHdrSourceFacts(const FlatMonoFrameInput& in, uint32_t index,
    const FlatContractRecord& hdr, const FlatContractRecord& hdrCamera,
    uint32_t width, uint32_t height, uint32_t consumerSeq, uint32_t hdrLast) {
    using namespace flat_mono_detail;
    FlatHdrSourceFacts f{};
    if (index >= in.worldCount + in.handoffCount || !in.supportedPair) return f;
    const auto& r=record(in,index); const auto& k=r.key;
    if (k.kind != kFlatContractPool || k.width != width || k.height != height ||
        !in.supportedPair(k.vs,k.ps)) return f;
    f.eligible=true;
    f.sameDepth=k.depth==hdr.key.depth;
    f.layoutValid=k.color && k.rtv && k.dsv==hdr.key.dsv &&
        k.depthFormat==hdr.key.depthFormat && k.depthWidth==width && k.depthHeight==height;
    f.current=cameraCurrent(r,in.epoch);
    f.fullViewport=fullViewport(k,width,height);
    f.sameCamera=sameCamera(r,hdrCamera);
    f.orderValid=r.last<consumerSeq && r.first<=hdrLast;
    if(f.sameDepth) {
        if(!f.layoutValid) f.issue=FlatHdrSourceIssue::SameDepthLayout;
        else if(!f.current || !f.fullViewport) f.issue=FlatHdrSourceIssue::InvalidProvenance;
        else if(!f.sameCamera) f.issue=FlatHdrSourceIssue::SameDepthCamera;
        else if(!f.orderValid) f.issue=FlatHdrSourceIssue::WrongOrder;
    } else if(k.depth && f.current && f.sameCamera) {
        f.issue=FlatHdrSourceIssue::SecondDepthSameCamera;
    }
    return f;
}
inline bool flatHdrShouldSampleAmbiguousSource(uint64_t frame, uint64_t firstFrame,
                                               uint32_t captured) {
    return frame && (captured == 0 ||
        (captured == 1 && frame > firstFrame && frame - firstFrame >= 60));
}
using FlatHdrAmbiguousSourceSink = void(*)(const FlatMonoFrameInput&, const void*, uint32_t, void*);
// flatSelectMonoFrame's sibling, run at the trigger over the records so far: H and its camera, the
// supported pool sources, one depth, one camera hash, in order, without the tone and copy requirements
// and with the extent gate R >= D. A refusal keeps the selector's own reason; the copy route is then
// the frame's only route, as before. `consumerSeq` is the place every H draw and every source must precede: the trigger
// draw's, for the route; the first draw into the copy's source, for the copy structure.
inline FlatMonoFrame flatSelectHdrFrame(const FlatMonoFrameInput& in, const void* hdrResource, uint32_t consumerSeq,
                                        FlatHdrExtentGate gate = FlatHdrExtentGate::RenderAtLeastOutput) {
    using namespace flat_mono_detail;
    FlatMonoFrame out{};
    out.frame = in.frame; out.epoch = in.epoch;
    auto refuse = [&](FlatMonoReason reason) { out.reason = reason; return out; };
    if (!in.frame || !in.epoch || !in.supportedPair || !hdrResource || !consumerSeq ||
        in.worldCount > 224 || in.handoffCount > 32 ||
        (in.worldCount && !in.world) || (in.handoffCount && !in.handoff))
        return refuse(FlatMonoReason::InvalidInput);
    if (!in.outputWidth || !in.outputHeight) return refuse(FlatMonoReason::UnknownOutput);
    if (in.droppedViews || in.droppedTargets || in.droppedWorld ||
        in.droppedHandoff || in.droppedLargeCb)
        return refuse(FlatMonoReason::Truncated);
    if (in.unknownLists || in.foreignCalls) return refuse(FlatMonoReason::ForeignWork);
    const uint32_t count = in.worldCount + in.handoffCount;

    const FlatContractRecord* hdr = nullptr;
    const FlatContractRecord* hdrCamera = nullptr;
    uint32_t hdrFirst = 0, hdrLast = 0, hdrCameraDraws = 0, w = 0, h = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = record(in, i);
        const auto& k = r.key;
        if (k.color != hdrResource) continue;
        if (!w) { w = k.width; h = k.height; }
        if (!ordered(r) || k.format != 26 || k.width != w || k.height != h ||
            !k.rtv || !k.depth || !k.dsv || !k.depthFormat ||
            k.depthWidth != w || k.depthHeight != h ||
            !hdrViewport(k, w, h)) return refuse(FlatMonoReason::ConflictingHdr);
        if (r.last >= consumerSeq) return refuse(FlatMonoReason::WrongOrder);
        if (hdr && (k.depth != hdr->key.depth || k.dsv != hdr->key.dsv ||
                    k.depthFormat != hdr->key.depthFormat)) return refuse(FlatMonoReason::ConflictingHdr);
        if (k.camera) {
            if (!cameraCurrent(r, in.epoch) || (hdrCamera && !sameCamera(r, *hdrCamera)))
                return refuse(FlatMonoReason::ConflictingHdr);
            if (!hdrCamera || r.first < hdrCamera->first) hdrCamera = &r;
            hdrCameraDraws += r.draws;
        }
        if (!hdrFirst || r.first < hdrFirst) hdrFirst = r.first;
        if (r.last > hdrLast) hdrLast = r.last;
        hdr = &r;
    }
    if (!hdr) return refuse(FlatMonoReason::NoHdr);
    // The extent gate, before anything that could make the frame look broken for another reason: the route
    // resolves at E = R and needs R >= D (upscaling keeps the copy route and its whitelist, decision (c)).
    // A target past twice the output or off the output's aspect is no scene extent either. The refusal carries the
    // measured sizes (H's and the output's): the F8 warning names supersampling below 1.0 from them, not from
    // Elite's settings file.
    if (gate == FlatHdrExtentGate::RenderAtLeastOutput) {
        if (w < in.outputWidth || h < in.outputHeight || w > in.outputWidth * 2 || h > in.outputHeight * 2 ||
            !flatUniformScale(w, h, in.outputWidth, in.outputHeight)) {
            out.renderWidth = w; out.renderHeight = h;
            out.outputWidth = in.outputWidth; out.outputHeight = in.outputHeight;
            return refuse(FlatMonoReason::HdrExtent);
        }
    } else if (!flatUniformScale(w, h, in.outputWidth, in.outputHeight) || w * 2 < in.outputWidth ||
               h * 2 < in.outputHeight || w > in.outputWidth * 2 || h > in.outputHeight * 2) {
        // The whitelist's own bound on the tone target (flatSelectMonoFrame): a uniform scale of the output between half
        // and twice, per axis.
        out.renderWidth = w; out.renderHeight = h;
        out.outputWidth = in.outputWidth; out.outputHeight = in.outputHeight;
        return refuse(FlatMonoReason::RenderSize);
    }
    if (!hdrCameraDraws) return refuse(FlatMonoReason::NoHdrCamera);
    float camera[6][4];
    if (!cameraShape(hdrCamera->camera, camera)) return refuse(FlatMonoReason::InvalidCamera);

    uint32_t sourceFirst = 0, sourceLast = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = record(in, i);
        const auto& k = r.key;
        if (k.kind != kFlatContractPool || k.width != w || k.height != h || k.depth != hdr->key.depth) continue;
        if (!in.supportedPair(k.vs, k.ps)) {
            // An unsupported same-depth writer with a different camera has
            // no qualified coverage export; it cannot silently disappear
            // from the source census when another writer made the frame mixed.
            if (in.qualifiedAlternate && k.camera && cameraCurrent(r,in.epoch) &&
                !sameCamera(r,*hdrCamera))
                return refuse(FlatMonoReason::AmbiguousSource);
            out.unsupportedDraws += r.draws; continue;
        }
        if (!k.color || !k.rtv || k.dsv != hdr->key.dsv || k.depthFormat != hdr->key.depthFormat ||
            k.depthWidth != w || k.depthHeight != h)
            return refuse(FlatMonoReason::AmbiguousSource);
        if (!cameraCurrent(r, in.epoch) || !fullViewport(k, w, h))
            return refuse(FlatMonoReason::InvalidSource);
        if (!sameCamera(r, *hdrCamera)) {
            if (!in.qualifiedAlternate ||
                !in.qualifiedAlternate(r,hdrCamera->camera,in.qualifiedAlternateUser))
                return refuse(FlatMonoReason::AmbiguousSource);
            out.mixedCamera = true;
        }
        if (r.last >= consumerSeq || r.first > hdrLast) return refuse(FlatMonoReason::WrongOrder);
        out.supportedDraws += r.draws;
        if (!sourceFirst || r.first < sourceFirst) sourceFirst = r.first;
        if (r.last > sourceLast) sourceLast = r.last;
    }
    if (!out.supportedDraws) return refuse(FlatMonoReason::NoSupportedSource);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = record(in, i);
        const auto& k = r.key;
        if (k.kind == kFlatContractPool && k.width == w && k.height == h &&
            k.depth && k.depth != hdr->key.depth && in.supportedPair(k.vs, k.ps) &&
            cameraCurrent(r, in.epoch) && sameCamera(r, *hdrCamera))
            return refuse(FlatMonoReason::AmbiguousSource);
    }
    // `color` is H itself: the resolver's history identity (a change of it is a reset) and the trace's
    // name for what the route resolved. There is no tone output and no output copy on this route.
    out.color = hdrResource; out.hdr = hdrResource;
    out.depth = hdr->key.depth; out.dsv = hdr->key.dsv; out.depthFormat = hdr->key.depthFormat;
    out.sceneConstants = hdrCamera->key.b1; out.output = in.output;
    out.renderWidth = w; out.renderHeight = h;
    out.outputWidth = in.outputWidth; out.outputHeight = in.outputHeight;
    std::memcpy(out.camera, camera, sizeof(camera));
    out.nearPlane = camera[3][2]; out.cameraHash = hdrCamera->key.cameraHash;
    out.sourceFirst = sourceFirst; out.sourceLast = sourceLast;
    out.hdrFirst = hdrFirst; out.hdrLast = hdrLast;
    out.toneSequence = consumerSeq; out.copySequence = 0;
    out.reason = FlatMonoReason::Selected;
    return out;
}

// The prefix model's records at a draw, assembled the way the copy branch assembles them for the copy
// route (flatRuntimeObserve) and run through flatSelectHdrFrame. `consumerSeq` is the place every H draw and every
// source must precede and `gate` the extent bound (flatSelectHdrRoute below is the trigger's, the route's own).
// A conflict the model recorded on H is kept as the witness, as the copy route keeps it.
inline FlatMonoFrame flatSelectHdrRouteAt(FlatRuntimePrefix& p, const FlatHdrFrame& f,
                                          bool (*supportedPair)(uint64_t, uint64_t), uint32_t consumerSeq,
                                          FlatHdrExtentGate gate,
                                          FlatHdrAmbiguousSourceSink sourceSink = nullptr, void* sourceSinkUser = nullptr,
                                          bool (*qualifiedAlternate)(const FlatContractRecord&,const unsigned char*,void*) = nullptr,
                                          void* qualifiedAlternateUser = nullptr) {
    using namespace flat_mono_detail;
    FlatMonoFrame out{}; out.frame = out.epoch = p.frame;
    if (!f.triggered) { out.reason = FlatMonoReason::NoHdrConsumer; return out; }
    if (f.trigger.ambiguous) { out.reason = FlatMonoReason::ConflictingHdr; return out; }
    if (p.uncertain) { out.reason = FlatMonoReason::Truncated; return out; }
    const FlatRuntimeTarget* target = nullptr;
    for (uint32_t i = 0; i < p.targetsUsed; ++i) if (p.targets[i].resource == f.trigger.hdr) { target = &p.targets[i]; break; }
    if (!target || !target->writes.draws || target->writes.key.format != 26) { out.reason = FlatMonoReason::NoHdr; return out; }
    if (target->hdrBad) {
        p.selectedConflict = target->firstBad;
        out.reason = FlatMonoReason::ConflictingHdr; return out;
    }
    FlatContractRecord records[34]{}; uint32_t n = 0;
    records[n] = target->writes; records[n].key.camera = nullptr; records[n++].key.kind = kFlatContractScreen;
    if (target->hdrCamera) { records[n] = target->tone; records[n++].key.kind = kFlatContractScreen; }
    for (uint32_t i = 0; i < p.sourcesUsed && n < 34; ++i) records[n++] = p.sources[i];
    FlatMonoFrameInput in{}; in.world = records; in.worldCount = n;
    in.output = p.output; in.outputWidth = p.width; in.outputHeight = p.height; in.outputFormat = p.format;
    in.frame = in.epoch = p.frame; in.supportedPair = supportedPair;
    in.qualifiedAlternate = qualifiedAlternate;
    in.qualifiedAlternateUser = qualifiedAlternateUser;
    out = flatSelectHdrFrame(in, f.trigger.hdr, consumerSeq, gate);
    if (out.reason == FlatMonoReason::AmbiguousSource && sourceSink)
        sourceSink(in, f.trigger.hdr, consumerSeq, sourceSinkUser);
    if (out.reason == FlatMonoReason::ConflictingHdr) {
        auto& wit = p.selectedConflict;
        wit.hdr = target->resource; wit.sequence = target->hdrCamera ? target->tone.first : target->writes.first;
        wit.reference = flatRuntimeWitnessDraw(target->writes);
        wit.current = flatRuntimeWitnessDraw(target->hdrCamera ? target->tone : target->writes);
        wit.cause = target->hdrCamera && !cameraCurrent(target->tone, p.frame)
            ? FlatRuntimeConflict::SelectorCameraProvenance
            : target->hdrCamera && target->writes.key.camera && !sameCamera(target->tone, target->writes)
            ? FlatRuntimeConflict::SelectorCamera : FlatRuntimeConflict::SelectorLayout;
    }
    return out;
}
// The route's own selection, at its trigger: every H draw and source before the trigger draw, R >= D.
inline FlatMonoFrame flatSelectHdrRoute(FlatRuntimePrefix& p, const FlatHdrFrame& f,
                                        bool (*supportedPair)(uint64_t, uint64_t),
                                        FlatHdrAmbiguousSourceSink sourceSink = nullptr, void* sourceSinkUser = nullptr,
                                        bool (*qualifiedAlternate)(const FlatContractRecord&,const unsigned char*,void*) = nullptr,
                                        void* qualifiedAlternateUser = nullptr) {
    return flatSelectHdrRouteAt(p, f, supportedPair, f.trigger.sequence,
                                FlatHdrExtentGate::RenderAtLeastOutput, sourceSink, sourceSinkUser,
                                qualifiedAlternate, qualifiedAlternateUser);
}

// ---- the route's gate ----------------------------------------------------------------------------
// The effective route's evaluation size must be the render size: DLSS and DLAA at R = D and above, FSR the
// same, EDVR's TAA only at R = D (above D it evaluates on the display grid, which stays on the copy route).
inline bool flatHdrRouteEvaluatesAtRender(FlatMonoResolveMode mode, uint32_t rW, uint32_t rH, uint32_t dW, uint32_t dH) {
    const auto route = flatResolveRoute(mode, rW, rH, dW, dH);
    return !route.refused && route.evalWidth == rW && route.evalHeight == rH && rW >= dW && rH >= dH;
}

// What the route adds to a frame's stand-down verdict at its trigger (key auto; flat_standdown.h). That verdict is
// about the SHAPE of the game's chain, so the route speaks only where it recognised the frame and will resolve it:
// Treatable, whatever the copy stage makes of the bloom or tone variant. Every other answer adds nothing. A refusal
// merged as itself would outrank the copy stage's structural one (the order is None < Structural < Transient <
// Treatable), and the stand-down that stops an unrecognised chain from costing CPU, and the F8 warning that says why,
// would never start: render below the output (R < D), where the copy route and its whitelist serve the frame and every
// trigger says hdr-route-needs-render-at-least-output, and EDVR's TAA above D, which stays on the copy route, are
// both that case. The key off merges nothing, so none of this reaches it.
inline FlatFrameSeen flatHdrTriggerSeen(const FlatMonoFrame& selection, FlatMonoResolveMode mode) {
    return selection.selected() && flatHdrRouteEvaluatesAtRender(mode, selection.renderWidth, selection.renderHeight,
                                                                 selection.outputWidth, selection.outputHeight)
        ? FlatFrameSeen::Treatable : FlatFrameSeen::None;
}

// ---- the scene's and the output's sizes, for the panel ------------------------------------------------------
// (The F8 warning once advised raising Elite's supersampling to 1.0 when the game rendered below the output. Below 1.0
// is served now, by the copy's admission by structure, flat_copy_structure.h; the advice and its predicate are gone.)
// The four sizes in one 64-bit word, 16 bits each (a size past 65535 cannot be a screen), 0 meaning "not the case":
// what the runtime publishes for the panel thread to read without a lock.
inline uint64_t flatHdrPackSizes(uint32_t renderW, uint32_t renderH, uint32_t outputW, uint32_t outputH) {
    if (!renderW || !renderH || !outputW || !outputH || renderW > 0xFFFFu || renderH > 0xFFFFu ||
        outputW > 0xFFFFu || outputH > 0xFFFFu)
        return 0;
    return uint64_t(renderW) | uint64_t(renderH) << 16 | uint64_t(outputW) << 32 | uint64_t(outputH) << 48;
}
inline bool flatHdrUnpackSizes(uint64_t packed, uint32_t* renderW, uint32_t* renderH, uint32_t* outputW,
                               uint32_t* outputH) {
    if (!packed) return false;
    if (renderW) *renderW = uint32_t(packed & 0xFFFFu);
    if (renderH) *renderH = uint32_t(packed >> 16 & 0xFFFFu);
    if (outputW) *outputW = uint32_t(packed >> 32 & 0xFFFFu);
    if (outputH) *outputH = uint32_t(packed >> 48 & 0xFFFFu);
    return true;
}

// ---- the latch -----------------------------------------------------------------------------------
// Three treated frames with writes into H after the resolve turn the route off until the key is flipped:
// the frames showed jittered, un-resolved pixels on top of the resolved ones, and a fourth would too.
constexpr uint32_t kFlatHdrLatchFrames = 3;
struct FlatHdrLatch {
    uint32_t frames = 0;
    bool tripped = false;
    // A treated frame closes: true on the frame that trips the latch.
    bool treatedFrame(bool hadLateWrites) {
        if (tripped || !hadLateWrites) return false;
        if (++frames >= kFlatHdrLatchFrames) { tripped = true; return true; }
        return false;
    }
    void reset() { frames = 0; tripped = false; }
};

// ---- the census ----------------------------------------------------------------------------------
// Where the route's frames got to in a window: how many reached each step of the treatment. The runtime counts the first
// two, the resolver the rest (FlatMonoResolveStats::hdrCaptured and its neighbours, handed over by difference), so a window
// whose counts stop at one step names the step that frames stop at in a session that goes on. The crash-safe trail of the
// first frames, for a session that does not, is flat_hdr_crumbs.h; this is the census of every frame, with no budget.
struct FlatHdrSteps {
    uint64_t admitted = 0;   // the route took the frame: treatHdr ran
    uint64_t reached = 0;    // ... and the frame got to the resolver (the resolve, or the spatial recovery)
    uint64_t captured = 0;   // the game's pipeline state was swapped out
    uint64_t copied = 0;     // H was copied into the private input
    uint64_t prepped = 0;    // the prep dispatch ran (the resolve only)
    uint64_t backend = 0;    // the backend returned success (the resolve only)
    uint64_t finished = 0;   // the pixel-shader draw into H ran
    uint64_t restored = 0;   // the game's pipeline state was put back
};
// One 5 s window, reset when it prints. The per-window token is the trigger decision: frames with an H and
// a trigger against frames with an H and none.
struct FlatHdrWindow {
    uint64_t frames = 0;          // every frame the runtime watched
    uint64_t hdrFrames = 0;       // ... with an H candidate
    uint64_t triggerFrames = 0, noTriggerFrames = 0, ambiguousFrames = 0;
    uint64_t lateWriteFrames = 0, lateWrites = 0;
    uint64_t treated = 0;         // frames the route resolved
    uint64_t declined = 0;        // the selector chose the frame and the treatment declined it (each is logged, to a dozen)
    FlatHdrSteps steps;           // how far the route's frames got, step by step
    const char* lastVerdict = "none";
    uint64_t lastTriggerVs = 0, lastTriggerPs = 0;
    uint32_t lastHdrWidth = 0, lastHdrHeight = 0, lastTargetWidth = 0, lastTargetHeight = 0;
    // What the selector said at each trigger, by name: "selected" or the reason it refused. The key off's whole
    // question ("would the route take these frames, and what stops the ones it would not?") is this tally, so it is
    // counted whatever the key says. More reasons than the table holds fall into `selectionOther`.
    static constexpr uint32_t kSelections = 6;
    struct Selection { const char* name = nullptr; uint64_t count = 0; };
    Selection selections[kSelections]{};
    uint64_t selectionOther = 0;
    void noteSelection(const char* name) {
        if (!name) return;
        for (auto& s : selections) {
            if (!s.name) { s.name = name; s.count = 1; return; }
            if (s.name == name || std::strcmp(s.name, name) == 0) { ++s.count; return; }
        }
        ++selectionOther;
    }
    void noteFrame(const FlatHdrFrame& f) {
        ++frames;
        const auto v = flatHdrFrameVerdict(f);
        if (v == FlatHdrFrameVerdict::NoHdr) return;
        ++hdrFrames;
        if (v == FlatHdrFrameVerdict::NoTrigger) { ++noTriggerFrames; return; }
        ++triggerFrames;
        if (f.trigger.ambiguous) ++ambiguousFrames;
        const uint32_t late = flatHdrLateWrites(f);
        if (late) { ++lateWriteFrames; lateWrites += late; }
        lastTriggerVs = f.trigger.vs; lastTriggerPs = f.trigger.ps;
        lastHdrWidth = f.trigger.hdrWidth; lastHdrHeight = f.trigger.hdrHeight;
        lastTargetWidth = f.trigger.targetWidth; lastTargetHeight = f.trigger.targetHeight;
    }
    void reset() { *this = FlatHdrWindow{}; }
};
enum class FlatHdrState : uint8_t { Observing, Active, Latched };
inline const char* flatHdrStateName(FlatHdrState s) {
    return s == FlatHdrState::Active ? "active" : s == FlatHdrState::Latched ? "latched" : "observing";
}
// The 5 s line, printed every window while a temporal mode is selected, zeros included: an absent line is
// what "the detector never ran" looks like.
inline int flatHdrFormatWindow(char* out, size_t size, FlatHdrKey key, FlatHdrState state, const FlatHdrWindow& w) {
    int n = std::snprintf(out, size,
        "flat hdr route 5s: key=%s state=%s frames=%llu hdr-frames=%llu trigger=%llu none=%llu ambiguous=%llu "
        "treated=%llu declined=%llu late-hdr-writes=%llu (in %llu frames) last=%s "
        "steps: admitted=%llu reached=%llu captured=%llu copied=%llu prepped=%llu backend=%llu finished=%llu restored=%llu "
        "last-trigger=VS=%016llX PS=%016llX target=%ux%u hdr=%ux%u selection=",
        flatHdrKeyName(key), flatHdrStateName(state),
        static_cast<unsigned long long>(w.frames), static_cast<unsigned long long>(w.hdrFrames),
        static_cast<unsigned long long>(w.triggerFrames), static_cast<unsigned long long>(w.noTriggerFrames),
        static_cast<unsigned long long>(w.ambiguousFrames), static_cast<unsigned long long>(w.treated),
        static_cast<unsigned long long>(w.declined), static_cast<unsigned long long>(w.lateWrites),
        static_cast<unsigned long long>(w.lateWriteFrames), w.lastVerdict,
        static_cast<unsigned long long>(w.steps.admitted), static_cast<unsigned long long>(w.steps.reached),
        static_cast<unsigned long long>(w.steps.captured), static_cast<unsigned long long>(w.steps.copied),
        static_cast<unsigned long long>(w.steps.prepped), static_cast<unsigned long long>(w.steps.backend),
        static_cast<unsigned long long>(w.steps.finished), static_cast<unsigned long long>(w.steps.restored),
        static_cast<unsigned long long>(w.lastTriggerVs), static_cast<unsigned long long>(w.lastTriggerPs),
        w.lastTargetWidth, w.lastTargetHeight, w.lastHdrWidth, w.lastHdrHeight);
    // The selector's verdicts at this window's triggers, "name:count" each, or "none" when no frame triggered.
    const auto append = [&](const char* first, const char* name, unsigned long long count) {
        if (n < 0 || static_cast<size_t>(n) >= size) return;
        const int more = std::snprintf(out + n, size - static_cast<size_t>(n), "%s%s:%llu", first, name, count);
        if (more > 0) n += more;
    };
    bool any = false;
    for (const auto& s : w.selections) {
        if (!s.name) break;
        append(any ? "," : "", s.name, static_cast<unsigned long long>(s.count));
        any = true;
    }
    if (w.selectionOther) { append(any ? "," : "", "other", static_cast<unsigned long long>(w.selectionOther)); any = true; }
    if (!any && n >= 0 && static_cast<size_t>(n) < size) { std::snprintf(out + n, size - static_cast<size_t>(n), "none"); n += 4; }
    return n;
}
// The first trigger of a session, once.
inline int flatHdrFormatFirstTrigger(char* out, size_t size, uint64_t frame, FlatHdrKey key, const FlatHdrFrame& f) {
    const auto& t = f.trigger;
    const FlatHdrCandidate* h = flatHdrFindCandidate(f, t.hdr);
    return std::snprintf(out, size,
        "flat hdr route: first trigger at frame=%llu seq=%u (key=%s): VS=%016llX PS=%016llX target=%ux%u fmt=%u "
        "reads the scene HDR %ux%u at t%d; its %u draw(s) ran seq %u..%u%s",
        static_cast<unsigned long long>(frame), t.sequence, flatHdrKeyName(key),
        static_cast<unsigned long long>(t.vs), static_cast<unsigned long long>(t.ps),
        t.targetWidth, t.targetHeight, t.targetFormat, t.hdrWidth, t.hdrHeight,
        t.srvKnown ? static_cast<int>(t.srvSlot) : -1,
        h ? h->draws : 0u, h ? h->firstSeq : 0u, h ? h->lastSeq : 0u,
        t.ambiguous ? "; MORE THAN ONE HDR candidate matched, so the route declines" : "");
}
// A frame whose H was written after the trigger: the pair that did it, named once per session.
inline int flatHdrFormatLateWrite(char* out, size_t size, uint64_t frame, FlatHdrKey key, const FlatHdrFrame& f) {
    return std::snprintf(out, size,
        "flat hdr route: frame=%llu wrote the scene HDR after the trigger (key=%s): %u draw(s), %u dispatch(es), %u "
        "explicit write(s); the first is seq %u VS=%016llX PS=%016llX target=%ux%u%s; a treated frame with such "
        "writes counts toward the latch (%u turns the route off)",
        static_cast<unsigned long long>(frame), flatHdrKeyName(key), f.lateDraws, f.lateDispatches, f.lateExplicit,
        f.lateSequence, static_cast<unsigned long long>(f.lateVs), static_cast<unsigned long long>(f.latePs),
        f.lateTargetWidth, f.lateTargetHeight, f.lateNewCandidate ? " (into an HDR target that did not exist at the trigger)" : "",
        kFlatHdrLatchFrames);
}
inline int flatHdrFormatLatched(char* out, size_t size, uint64_t frame, const FlatHdrFrame& f, uint32_t frames) {
    return std::snprintf(out, size,
        "flat hdr route: turned off at frame=%llu after %u treated frame(s) wrote the scene HDR after the resolve "
        "(the latest: %u draw(s), %u dispatch(es), %u explicit write(s), first VS=%016llX PS=%016llX); the copy "
        "route treats from here until experimental.temporal_aa_before_post is set off and auto again",
        static_cast<unsigned long long>(frame), frames, f.lateDraws, f.lateDispatches, f.lateExplicit,
        static_cast<unsigned long long>(f.lateVs), static_cast<unsigned long long>(f.latePs));
}

}  // namespace edvr
