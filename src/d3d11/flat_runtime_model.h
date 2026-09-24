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
struct FlatRuntimeTarget {
    const void* resource = nullptr;
    FlatContractRecord writes{}, tone{};
    bool hdrBad = false, hdrCamera = false;
    uint32_t tones = 0;
};
struct FlatRuntimePrefix {
    FlatRuntimeTarget targets[128]{};
    FlatContractRecord sources[32]{};
    uint32_t targetsUsed = 0, sourcesUsed = 0, sequence = 0, copies = 0;
    uint64_t frame = 0;
    const void* output = nullptr;
    uint32_t width = 0, height = 0, format = 0;
    bool uncertain = false;
};
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
        p.targets[i].hdrBad = true; p.targets[i].tones = 0;
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
                if (t.hdrBad) { out.reason = FlatMonoReason::ConflictingHdr; return out; }
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
        return flatSelectMonoFrame(in);
    }
    if (!k.color) return out;
    auto* t = flatRuntimeTarget(p, k.color); if (!t) return out;
    if (k.format == 26) {
        if (!hdrViewport(k, k.width, k.height) || !k.depth || !k.dsv || k.depthWidth != k.width || k.depthHeight != k.height) t->hdrBad = true;
        if (t->writes.draws && (t->writes.key.depth != k.depth || t->writes.key.dsv != k.dsv)) t->hdrBad = true;
        if (k.camera) {
            if (t->hdrCamera && !sameCamera(t->tone, current)) t->hdrBad = true;
            if (!cameraCurrent(current, p.frame)) t->hdrBad = true;
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
