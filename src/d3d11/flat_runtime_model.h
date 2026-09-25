#pragma once
#include "flat_mono_frame.h"

namespace edvr {
inline uint32_t flatRuntimeDepthReadFormat(uint32_t format) {
    return format == 19 ? 21u : format == 39 ? 41u : format == 44 ? 46u : 0u;
}
// Known, actual-shader-verified projection recipes may use the selector's HDR
// viewport contract only on a scene HDR target with its owned scene depth.
// This does not admit depth-clamped draws as motion sources or image intermediates.
inline bool flatRuntimeProjectionHdr(const FlatContractObservation& k, const void* output,
                                     const void* ownedDepth) {
    return k.format == 26 && k.color && k.color != output && k.rtv &&
        ownedDepth && k.depth == ownedDepth && k.dsv && k.width && k.height &&
        k.depthWidth == k.width && k.depthHeight == k.height;
}
inline bool flatRuntimeProjectionViewport(uint32_t count, const float* viewport,
                                          uint32_t width, uint32_t height, bool sceneHdr) {
    return sceneHdr ? flat_mono_detail::hdrViewport(count, viewport, width, height)
                    : flat_mono_detail::fullViewport(count, viewport, width, height);
}
// Online prefix contract. No resource ownership and no previous-frame admission.
struct FlatRuntimeDraw {
    FlatContractObservation key{};
    unsigned char camera[kFlatCameraBytes]{};
    bool supported = false;
    // Set only after the bridge verifies the actual HDR image-copy shaders,
    // RT0/DSV/SRV0 views, dimensions, sample count and viewport.
    bool hdrCopyVerified = false;
    // Exact menu HDR-to-HDR copy, with live views and typed texture layout verified.
    bool menuHdrCopyVerified = false;
    // The bridge verified the actual camera-independent format 9 image shaders
    // and their RT/DSV views. The model also checks the exact shader pair.
    bool imageSourceCameraIndependentVerified = false;
    uint32_t instances = 1;
};
enum class FlatRuntimeConflict : uint32_t {
    None, Viewport, MissingDepth, DepthMismatch, CameraChange,
    CameraProvenance, ExplicitWrite, ImageCopySource, MenuCopySource, SelectorLayout,
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
    case FlatRuntimeConflict::ImageCopySource: return "image-copy-source";
    case FlatRuntimeConflict::MenuCopySource: return "menu-copy-source";
    case FlatRuntimeConflict::SelectorLayout: return "selector-hdr-layout";
    case FlatRuntimeConflict::SelectorCamera: return "selector-hdr-camera-conflict";
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
    // tone stores the first known camera for HDR, or for format 9 image geometry.
    FlatContractRecord writes{}, tone{};
    bool hdrBad = false, hdrCamera = false, hdrLayoutChanged = false, imageSourceBad = false;
    bool menuInherited = false;
    bool imageHasCamera = false;
    uint32_t tones = 0;
    FlatRuntimeWitness firstBad{};
};
struct FlatRuntimePrefix {
    FlatRuntimeTarget targets[128]{};
    FlatContractRecord sources[32]{};
    uint32_t targetsUsed = 0, sourcesUsed = 0, sequence = 0, copies = 0;
    uint32_t imageCopiesAccepted = 0, imageCopiesRefused = 0;
    uint32_t menuCopiesAccepted = 0, menuCopiesRefused = 0;
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
        if (t.writes.key.format == 9) t.imageSourceBad = true;
    }
    for (uint32_t i = 0; i < p.sourcesUsed; ++i) if (p.sources[i].key.depth == resource) p.sources[i].key.camera = nullptr;
}
inline void flatRuntimeComputeWritten(FlatRuntimePrefix& p, const void* resource) {
    // Lighting may write the original scene HDR before the menu copy. An
    // inherited destination has no such exemption: its contents are a copy.
    if (!p.menuCopiesAccepted) return;
    for (uint32_t i = 0; i < p.targetsUsed; ++i)
        if (p.targets[i].resource == resource && p.targets[i].menuInherited) {
            flatRuntimeWritten(p, resource);
            return;
        }
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
                w.reference = flatRuntimeWitnessDraw(t.writes);
                w.current = flatRuntimeWitnessDraw(t.hdrCamera ? t.tone : t.writes);
                w.cause = t.hdrCamera && !cameraCurrent(t.tone, p.frame)
                    ? FlatRuntimeConflict::SelectorCameraProvenance
                    : t.hdrCamera && t.writes.key.camera && !sameCamera(t.tone, t.writes)
                    ? FlatRuntimeConflict::SelectorCamera : FlatRuntimeConflict::SelectorLayout;
                break;
            }
        }
        return out;
    }
    if (!k.color) return out;
    auto* t = flatRuntimeTarget(p, k.color); if (!t) return out;
    constexpr uint64_t kHdrImageCopyVs = 0xCFA91824129ECBBCull;
    constexpr uint64_t kHdrImageCopyPs = 0xDFCBA0EC70B03C9Bull;
    constexpr uint64_t kImageSourcePs = 0xFCFAD73924BF45B9ull;
    constexpr uint64_t kMenuCopyVs = 0xDEF19B035D5EDEDCull;
    constexpr uint64_t kMenuCopyPs = 0xDED8796049C7BB4Aull;
    if (k.format == 26 && k.vs == kMenuCopyVs && k.ps == kMenuCopyPs) {
        FlatRuntimeTarget* source = nullptr;
        for (uint32_t i = 0; i < p.targetsUsed; ++i)
            if (p.targets[i].resource == k.srvResource[0]) source = &p.targets[i];
        const bool valid = d.menuHdrCopyVerified && k.srvView[0] && k.srvResource[0] &&
            k.srvResource[0] != k.color && source && source != t &&
            source->writes.draws && !source->hdrBad && !source->hdrLayoutChanged && source->hdrCamera &&
            source->writes.key.format == 26 && source->writes.key.rtv &&
            source->writes.key.width == k.width && source->writes.key.height == k.height &&
            source->writes.key.depth && source->writes.key.dsv && source->writes.key.depthFormat &&
            source->writes.key.depthWidth == k.width && source->writes.key.depthHeight == k.height &&
            source->writes.last < q && cameraCurrent(source->tone, p.frame) &&
            (!source->writes.key.camera || sameCamera(source->writes, source->tone)) &&
            !t->writes.draws && !t->hdrBad && !k.depth && !k.dsv &&
            fullViewport(k, k.width, k.height);
        if (!valid) {
            ++p.menuCopiesRefused;
            const bool firstConflict = t->firstBad.cause == FlatRuntimeConflict::None;
            flatRuntimeBad(*t, FlatRuntimeConflict::MenuCopySource, q,
                           source ? source->writes : t->writes, current);
            if (firstConflict && source && source->firstBad.cause != FlatRuntimeConflict::None)
                t->firstBad = source->firstBad;
            // A rejected first copy is still a real write into this HDR target;
            // retain it so the final selector reports the original conflict.
            if (!t->writes.draws) t->writes = current;
            return out;
        }
        ++p.menuCopiesAccepted;
        // The destination comes into existence at this copy. Its scene and camera
        // provenance is inherited from the verified source, never from copy b1.
        t->writes = current;
        t->writes.key.depth = source->writes.key.depth;
        t->writes.key.dsv = source->writes.key.dsv;
        t->writes.key.depthWidth = source->writes.key.depthWidth;
        t->writes.key.depthHeight = source->writes.key.depthHeight;
        t->writes.key.depthFormat = source->writes.key.depthFormat;
        t->writes.key.camera = nullptr;
        t->tone = current;
        t->tone.key.depth = t->writes.key.depth;
        t->tone.key.dsv = t->writes.key.dsv;
        t->tone.key.depthWidth = t->writes.key.depthWidth;
        t->tone.key.depthHeight = t->writes.key.depthHeight;
        t->tone.key.depthFormat = t->writes.key.depthFormat;
        t->tone.key.b1 = source->tone.key.b1;
        t->tone.key.cameraHash = source->tone.key.cameraHash;
        t->tone.firstWriteEpoch = t->tone.lastWriteEpoch = source->tone.firstWriteEpoch;
        t->tone.firstWriteSeq = t->tone.lastWriteSeq = source->tone.firstWriteSeq;
        std::memcpy(t->tone.camera, source->tone.camera, sizeof(t->tone.camera));
        t->tone.key.camera = t->tone.camera;
        t->hdrCamera = true;
        t->menuInherited = true;
        return out;
    }
    const bool imageCameraIndependent = k.format == 9 &&
        k.vs == kHdrImageCopyVs && k.ps == kImageSourcePs &&
        d.imageSourceCameraIndependentVerified;
    const bool imageCopy = k.format == 26 && k.vs == kHdrImageCopyVs && k.ps == kHdrImageCopyPs;
    if (imageCopy) {
        FlatRuntimeTarget* source = nullptr;
        for (uint32_t i = 0; i < p.targetsUsed; ++i)
            if (p.targets[i].resource == k.srvResource[0]) source = &p.targets[i];
        const bool valid = d.hdrCopyVerified && k.srvView[0] && k.srvResource[0] &&
            k.srvResource[0] != k.color && source && source->writes.draws &&
            !source->imageSourceBad && !source->hdrBad &&
            source->writes.key.format == 9 && source->writes.key.rtv &&
            source->writes.key.width == k.width && source->writes.key.height == k.height &&
            source->writes.key.depth && source->writes.key.dsv &&
            source->writes.key.depthWidth == k.width && source->writes.key.depthHeight == k.height &&
            source->writes.last < q &&
            (!source->imageHasCamera ||
             (cameraCurrent(source->tone, p.frame) && sameCamera(t->tone, source->tone))) &&
            t->writes.draws && !t->hdrBad && t->hdrCamera &&
            t->writes.key.format == 26 && t->writes.key.width == k.width &&
            t->writes.key.height == k.height &&
            t->writes.key.depth == source->writes.key.depth &&
            t->writes.key.dsv == source->writes.key.dsv &&
            t->writes.key.depthFormat == source->writes.key.depthFormat &&
            cameraCurrent(t->tone, p.frame) &&
            fullViewport(k, k.width, k.height) && !k.depth && !k.dsv;
        if (!valid) {
            ++p.imageCopiesRefused;
            const bool firstConflict = t->firstBad.cause == FlatRuntimeConflict::None;
            flatRuntimeBad(*t, FlatRuntimeConflict::ImageCopySource, q,
                           source ? source->writes : t->writes, current);
            if (firstConflict &&
                source && source->firstBad.cause == FlatRuntimeConflict::ImageCopySource) {
                t->firstBad.reference = source->firstBad.reference;
                t->firstBad.current = source->firstBad.current;
            }
            return out;
        }
        ++p.imageCopiesAccepted;
        ++t->writes.draws; t->writes.last = q; t->writes.lastInstances = d.instances;
        return out;
    }
    if (k.format == 26) {
        if (!hdrViewport(k, k.width, k.height)) flatRuntimeBad(*t, FlatRuntimeConflict::Viewport, q, t->writes, current);
        if (!k.depth || !k.dsv || k.depthWidth != k.width || k.depthHeight != k.height)
            flatRuntimeBad(*t, FlatRuntimeConflict::MissingDepth, q, t->writes, current);
        if (t->writes.draws && (t->writes.key.depth != k.depth || t->writes.key.dsv != k.dsv))
            flatRuntimeBad(*t, FlatRuntimeConflict::DepthMismatch, q, t->writes, current);
        if (k.camera) {
            // Epic 20260925_122208: the first-person weapon pass (laser rifle
            // family) draws into the scene HDR/depth with its own qualified
            // second projection -- same position and orientation, narrower
            // FOV, near 0.0675 versus 0.025; its b0[4..7] transposes its own
            // b1[270..273]. It neither sets nor vetoes the scene camera; its
            // pixels fail strict depth ownership at resolve and fall back to
            // current colour.
            const bool secondCamera =
                k.vs == 0x88DCF1164C640EC3ull && k.ps == 0x494506A63091DF8Cull;
            if (t->hdrCamera && !sameCamera(t->tone, current) && !secondCamera)
                flatRuntimeBad(*t, FlatRuntimeConflict::CameraChange, q, t->tone, current);
            if (!cameraCurrent(current, p.frame))
                flatRuntimeBad(*t, FlatRuntimeConflict::CameraProvenance, q, t->tone, current);
            // Keep the first known camera draw separate from earlier HDR writes
            // without b1. Assigning its later write to those draws invents provenance.
            if (!t->hdrCamera && !secondCamera) { t->tone = current; t->hdrCamera = true; }
        }
    }
    if (t->writes.draws && t->writes.key.format == 26 &&
        (k.format != 26 || k.width != t->writes.key.width ||
         k.height != t->writes.key.height ||
         k.depth != t->writes.key.depth || k.dsv != t->writes.key.dsv ||
         k.depthWidth != t->writes.key.depthWidth ||
         k.depthHeight != t->writes.key.depthHeight ||
         k.depthFormat != t->writes.key.depthFormat))
        t->hdrLayoutChanged = true;
    if (t->writes.draws && t->writes.key.format == 9 && k.format != 9) {
        t->imageSourceBad = true;
        flatRuntimeBad(*t, FlatRuntimeConflict::ImageCopySource, q, t->writes, current);
    }
    if (k.format == 9) {
        const auto& first = t->writes;
        if (!k.depth || !k.dsv || !k.depthFormat ||
            k.depthWidth != k.width || k.depthHeight != k.height ||
            !fullViewport(k, k.width, k.height) ||
            (!imageCameraIndependent && !cameraCurrent(current, p.frame)) ||
            (first.draws && (first.key.format != k.format || first.key.width != k.width ||
                             first.key.height != k.height || first.key.depth != k.depth ||
                             first.key.dsv != k.dsv || first.key.depthFormat != k.depthFormat ||
                             first.key.depthWidth != k.depthWidth ||
                             first.key.depthHeight != k.depthHeight ||
                             (!imageCameraIndependent && t->imageHasCamera &&
                              !sameCamera(t->tone, current))))) {
            t->imageSourceBad = true;
            flatRuntimeBad(*t, FlatRuntimeConflict::ImageCopySource, q,
                           t->imageHasCamera ? t->tone : first, current);
        }
        if (!imageCameraIndependent && k.camera && !t->imageHasCamera) {
            t->tone = current; t->imageHasCamera = true;
        }
    }
    if (!t->writes.draws) t->writes = current;
    else { ++t->writes.draws; t->writes.last = q; t->writes.lastInstances = d.instances;
        if (k.camera) { t->writes.lastWriteEpoch = k.writeEpoch; t->writes.lastWriteSeq = k.writeSeq; } }
    if (k.format != 9 && k.vs == kToneVs && k.ps == kTonePs) { ++t->tones; t->tone = current; }
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
