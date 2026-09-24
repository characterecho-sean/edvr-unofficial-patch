#pragma once
#include "flat_mono_frame.h"

namespace edvr {
inline uint32_t flatRuntimeDepthReadFormat(uint32_t format) {
    return format == 19 ? 21u : format == 39 ? 41u : format == 44 ? 46u : 0u;
}
// Online prefix contract. No resource ownership and no previous-frame admission.
struct FlatRuntimeDraw {
    FlatContractObservation key{};
    unsigned char camera[kFlatCameraBytes]{};
    bool supported = false;
    uint32_t instances = 1;
};
enum class FlatRuntimeConflict : uint32_t {
    None, Viewport, MissingDepth, DepthMismatch, CameraChange,
    CameraProvenance, ExplicitWrite, SelectorLayout,
    SelectorCamera, SelectorCameraProvenance, Count
};
inline const char* flatRuntimeConflictName(FlatRuntimeConflict c) {
    switch (c) {
    case FlatRuntimeConflict::Viewport: return "viewport";
    case FlatRuntimeConflict::MissingDepth: return "missing-depth-or-dsv";
    case FlatRuntimeConflict::DepthMismatch: return "depth-or-dsv-changed";
    case FlatRuntimeConflict::CameraChange: return "hdr-camera-changed";
    case FlatRuntimeConflict::CameraProvenance: return "hdr-camera-provenance";
    case FlatRuntimeConflict::ExplicitWrite: return "explicit-resource-write";
    case FlatRuntimeConflict::SelectorLayout: return "selector-hdr-layout";
    case FlatRuntimeConflict::SelectorCamera: return "selector-hdr-vs-tone-camera";
    case FlatRuntimeConflict::SelectorCameraProvenance: return "selector-hdr-camera-provenance";
    default: return "none";
    }
}
struct FlatRuntimeWitnessDraw {
    const void* rtv = nullptr, *depth = nullptr, *dsv = nullptr, *b1 = nullptr;
    uint64_t vs = 0, ps = 0, cameraHash = 0, writeEpoch = 0;
    uint32_t first = 0, writeSeq = 0, width = 0, height = 0, format = 0;
    uint32_t depthWidth = 0, depthHeight = 0, depthFormat = 0, viewportCount = 0;
    float viewport[6]{};
    bool hasCamera = false;
    unsigned char camera[kFlatCameraBytes]{};
};
inline FlatRuntimeWitnessDraw flatRuntimeWitnessDraw(const FlatContractRecord& r) {
    FlatRuntimeWitnessDraw d{}; const auto& k = r.key;
    d.rtv = k.rtv; d.depth = k.depth; d.dsv = k.dsv; d.b1 = k.b1;
    d.vs = k.vs; d.ps = k.ps; d.cameraHash = k.cameraHash;
    d.writeEpoch = r.firstWriteEpoch; d.first = r.first; d.writeSeq = r.firstWriteSeq;
    d.width = k.width; d.height = k.height; d.format = k.format;
    d.depthWidth = k.depthWidth; d.depthHeight = k.depthHeight; d.depthFormat = k.depthFormat;
    d.viewportCount = k.viewportCount;
    std::memcpy(d.viewport, k.viewport, sizeof(d.viewport));
    d.hasCamera = k.camera != nullptr;
    if (d.hasCamera) std::memcpy(d.camera, r.camera, sizeof(d.camera));
    return d;
}
struct FlatRuntimeWitness {
    FlatRuntimeConflict cause = FlatRuntimeConflict::None;
    const void* hdr = nullptr;
    uint32_t sequence = 0;
    FlatRuntimeWitnessDraw reference{}, current{};
};
static_assert(sizeof(FlatRuntimeWitness) <= 512, "keep per-target refusal witnesses bounded");
struct FlatRuntimeTarget {
    const void* resource = nullptr;
    FlatContractRecord writes{}, tone{};
    bool hdrBad = false, hdrCamera = false;
    uint32_t tones = 0;
    FlatRuntimeWitness firstBad{};
};
struct FlatRuntimePrefix {
    FlatRuntimeTarget targets[128]{};
    FlatContractRecord sources[32]{};
    uint32_t targetsUsed = 0, sourcesUsed = 0, sequence = 0, copies = 0;
    uint64_t frame = 0;
    const void* output = nullptr;
    uint32_t width = 0, height = 0, format = 0;
    bool uncertain = false;
    FlatRuntimeWitness selectedConflict{};
};
inline void flatRuntimeBad(FlatRuntimeTarget& t, FlatRuntimeConflict cause,
                          uint32_t sequence, const FlatContractRecord& reference,
                          const FlatContractRecord& current) {
    if (t.firstBad.cause == FlatRuntimeConflict::None) {
        t.firstBad.cause = cause; t.firstBad.hdr = t.resource;
        t.firstBad.sequence = sequence; t.firstBad.reference = flatRuntimeWitnessDraw(reference);
        t.firstBad.current = flatRuntimeWitnessDraw(current);
    }
    t.hdrBad = true;
}
inline FlatContractRecord flatRuntimeRecord(const FlatRuntimeDraw& d, uint32_t q, uint64_t frame) {
    FlatContractRecord r{}; r.key = d.key; r.draws = 1; r.first = r.last = q;
    r.firstInstances = r.lastInstances = d.instances;
    (void)frame;
    r.firstWriteEpoch = r.lastWriteEpoch = d.key.writeEpoch;
    r.firstWriteSeq = r.lastWriteSeq = d.key.writeSeq;
    std::memcpy(r.camera, d.camera, sizeof(r.camera)); return r;
}
inline FlatRuntimeTarget* flatRuntimeTarget(FlatRuntimePrefix& p, const void* resource) {
    for (uint32_t i = 0; i < p.targetsUsed; ++i) if (p.targets[i].resource == resource) return &p.targets[i];
    if (!resource || p.targetsUsed == 128) { p.uncertain = true; return nullptr; }
    auto& t = p.targets[p.targetsUsed++]; t.resource = resource; return &t;
}
inline void flatRuntimeWritten(FlatRuntimePrefix& p, const void* resource) {
    for (uint32_t i = 0; i < p.targetsUsed; ++i) if (p.targets[i].resource == resource && p.targets[i].writes.draws) {
        auto& t = p.targets[i];
        flatRuntimeBad(t, FlatRuntimeConflict::ExplicitWrite, p.sequence,
                       t.writes, FlatContractRecord{});
        t.tones = 0;
    }
    for (uint32_t i = 0; i < p.sourcesUsed; ++i) if (p.sources[i].key.depth == resource) p.sources[i].key.camera = nullptr;
}
inline FlatMonoFrame flatRuntimeObserve(FlatRuntimePrefix& p, const FlatRuntimeDraw& d) {
    using namespace flat_mono_detail;
    FlatMonoFrame out{}; out.frame = out.epoch = p.frame;
    const uint32_t q = ++p.sequence;
    const auto current = flatRuntimeRecord(d, q, p.frame);
    const auto& k = d.key;
    const bool copy = k.vs == kCopyVs && k.ps == kCopyPs && k.color == p.output;
    if (copy) {
        p.selectedConflict = FlatRuntimeWitness{};
        ++p.copies;
        if (p.uncertain || p.copies != 1) { out.reason = FlatMonoReason::Truncated; return out; }
        FlatRuntimeTarget* tone = nullptr;
        for (uint32_t i = 0; i < p.targetsUsed; ++i) if (p.targets[i].resource == k.srvResource[0]) tone = &p.targets[i];
        if (!tone || tone->tones != 1 || tone->tone.last != tone->writes.last) { out.reason = FlatMonoReason::NoTonePass; return out; }
        FlatContractRecord records[36]{}; uint32_t n = 0;
        // Reuse the proven completed-frame selector on this exact prefix. Only
        // HDR aggregates, supported sources and tone/copy enter the fixture.
        for (uint32_t i = 0; i < p.targetsUsed; ++i) {
            const auto& t = p.targets[i];
            if (t.resource == tone->tone.key.srvResource[1] && t.writes.key.format == 26 && t.writes.draws) {
                if (t.hdrBad) { p.selectedConflict = t.firstBad; out.reason = FlatMonoReason::ConflictingHdr; return out; }
                records[n] = t.writes; records[n].key.camera = nullptr; records[n++].key.kind = kFlatContractScreen;
                if (t.hdrCamera) { records[n] = t.tone; records[n++].key.kind = kFlatContractScreen; }
            }
        }
        records[n++] = tone->tone;
        for (uint32_t i = 0; i < p.sourcesUsed; ++i) records[n++] = p.sources[i];
        records[n++] = current;
        FlatMonoFrameInput in{}; in.world = records; in.worldCount = n;
        in.output = p.output; in.outputWidth = p.width; in.outputHeight = p.height; in.outputFormat = p.format;
        in.frame = in.epoch = p.frame; in.supportedPair = [](uint64_t, uint64_t) { return true; };
        out = flatSelectMonoFrame(in);
        if (out.reason == FlatMonoReason::ConflictingHdr) {
            for (uint32_t i = 0; i < p.targetsUsed; ++i) {
                const auto& t = p.targets[i];
                if (t.resource != tone->tone.key.srvResource[1] || !t.writes.draws) continue;
                auto& w = p.selectedConflict;
                w.hdr = t.resource; w.sequence = t.hdrCamera ? t.tone.first : t.writes.first;
                w.reference = flatRuntimeWitnessDraw(tone->tone);
                w.current = flatRuntimeWitnessDraw(t.hdrCamera ? t.tone : t.writes);
                w.cause = t.hdrCamera && !cameraCurrent(t.tone, p.frame)
                    ? FlatRuntimeConflict::SelectorCameraProvenance
                    : t.hdrCamera && !sameCamera(t.tone, tone->tone)
                    ? FlatRuntimeConflict::SelectorCamera : FlatRuntimeConflict::SelectorLayout;
                break;
            }
        }
        return out;
    }
    if (!k.color) return out;
    auto* t = flatRuntimeTarget(p, k.color); if (!t) return out;
    if (k.format == 26) {
        if (!hdrViewport(k, k.width, k.height)) flatRuntimeBad(*t, FlatRuntimeConflict::Viewport, q, t->writes, current);
        if (!k.depth || !k.dsv || k.depthWidth != k.width || k.depthHeight != k.height)
            flatRuntimeBad(*t, FlatRuntimeConflict::MissingDepth, q, t->writes, current);
        if (t->writes.draws && (t->writes.key.depth != k.depth || t->writes.key.dsv != k.dsv))
            flatRuntimeBad(*t, FlatRuntimeConflict::DepthMismatch, q, t->writes, current);
        if (k.camera) {
            if (t->hdrCamera && !sameCamera(t->tone, current))
                flatRuntimeBad(*t, FlatRuntimeConflict::CameraChange, q, t->tone, current);
            if (!cameraCurrent(current, p.frame))
                flatRuntimeBad(*t, FlatRuntimeConflict::CameraProvenance, q, t->tone, current);
            // Keep the first known camera draw separate from earlier HDR writes
            // without b1. Assigning its later write to those draws invents provenance.
            if (!t->hdrCamera) { t->tone = current; t->hdrCamera = true; }
        }
    }
    if (!t->writes.draws) t->writes = current;
    else { ++t->writes.draws; t->writes.last = q; t->writes.lastInstances = d.instances;
        if (k.camera) { t->writes.lastWriteEpoch = k.writeEpoch; t->writes.lastWriteSeq = k.writeSeq; } }
    if (k.vs == kToneVs && k.ps == kTonePs) { ++t->tones; t->tone = current; }
    if (d.supported && k.depth && k.kind == kFlatContractPool) {
        FlatContractRecord* source = nullptr;
        for (uint32_t i = 0; i < p.sourcesUsed; ++i) if (p.sources[i].key.depth == k.depth && sameCamera(p.sources[i], current)) { source = &p.sources[i]; break; }
        if (!source) {
            if (p.sourcesUsed == 32) p.uncertain = true;
            else p.sources[p.sourcesUsed++] = current;
        } else {
            if (!fullViewport(k, k.width, k.height) || source->key.dsv != k.dsv ||
                source->key.width != k.width || source->key.height != k.height ||
                source->key.depthWidth != k.depthWidth || source->key.depthHeight != k.depthHeight)
                source->key.camera = nullptr;
            ++source->draws; source->last = q; source->lastWriteEpoch = k.writeEpoch; source->lastWriteSeq = k.writeSeq; source->lastInstances = d.instances;
        }
    }
    return out;
}
} // namespace edvr
