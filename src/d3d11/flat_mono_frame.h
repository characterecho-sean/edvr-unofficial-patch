// Passive mono input selection from one completed, bounded discovery frame.
// Resource pointers are identities only, with no COM ownership or GPU work.
// A selection is metadata, never a certificate to jitter or run temporal AA.
#pragma once
#include "flat_temporal_model.h"
#include <cmath>

namespace edvr {

enum class FlatMonoReason {
    Selected, InvalidInput, UnknownOutput, Truncated, ForeignWork,
    NoOutputCopy, AmbiguousOutputCopy, InvalidOutputCopy,
    NoTonePass, AmbiguousTonePass, InvalidTonePass, BrokenLineage,
    WrongOrder, MissingCamera, InvalidCamera, NoHdr,
    ConflictingHdr, NoHdrCamera, NoSupportedSource, AmbiguousSource,
    InvalidSource,
    // The HDR route's own (flat_hdr_route.h, section 81). Appended, never reordered: a reason's
    // value is part of the frame-contract hash and the stand-down's published word.
    NoHdrConsumer, HdrExtent,
    // The final copy's admission by structure (flat_copy_structure.h, section 83), appended the same way. RenderSize: the
    // game renders the scene at a size that is not a uniform scale of the output between half and twice (Elite's resolution
    // is not the screen's shape, or the supersampling takes it out of the band): the measured sizes ride the refusal.
    // NoScene: a final copy was found and the frame has no scene at all (startup, a loading screen, a 2D menu).
    RenderSize, NoScene
};
inline const char* flatMonoReasonName(FlatMonoReason reason) {
    switch (reason) {
    case FlatMonoReason::Selected: return "observed-mono-input-candidate";
    case FlatMonoReason::InvalidInput: return "invalid-frame-input";
    case FlatMonoReason::UnknownOutput: return "output-unknown-or-unsupported";
    case FlatMonoReason::Truncated: return "relevant-observations-truncated";
    case FlatMonoReason::ForeignWork: return "unknown-or-foreign-work";
    case FlatMonoReason::NoOutputCopy: return "no-known-output-copy";
    case FlatMonoReason::AmbiguousOutputCopy: return "output-copy-not-unique";
    case FlatMonoReason::InvalidOutputCopy: return "invalid-output-copy";
    case FlatMonoReason::NoTonePass: return "no-known-tone-pass";
    case FlatMonoReason::AmbiguousTonePass: return "tone-pass-not-unique";
    case FlatMonoReason::InvalidTonePass: return "invalid-tone-pass";
    case FlatMonoReason::BrokenLineage: return "broken-resource-lineage";
    case FlatMonoReason::WrongOrder: return "inconsistent-draw-order";
    case FlatMonoReason::MissingCamera: return "handoff-camera-unavailable-or-stale";
    case FlatMonoReason::InvalidCamera: return "invalid-camera-rows";
    case FlatMonoReason::NoHdr: return "no-observed-hdr-writes";
    case FlatMonoReason::ConflictingHdr: return "conflicting-hdr-target-or-camera";
    case FlatMonoReason::NoHdrCamera: return "hdr-camera-not-observed";
    case FlatMonoReason::NoSupportedSource: return "no-supported-motion-source-pair";
    case FlatMonoReason::AmbiguousSource: return "source-camera-or-depth-not-unique";
    case FlatMonoReason::InvalidSource: return "invalid-source-provenance-or-viewport";
    case FlatMonoReason::NoHdrConsumer: return "no-hdr-consumer";
    case FlatMonoReason::HdrExtent: return "hdr-route-needs-render-at-least-output";
    case FlatMonoReason::RenderSize: return "render-size-does-not-fit-output";
    case FlatMonoReason::NoScene: return "no-3d-scene";
    }
    return "unknown-selector-result";
}

struct FlatMonoFrameInput {
    const FlatContractRecord* world = nullptr;
    const FlatContractRecord* handoff = nullptr;
    uint32_t worldCount = 0, handoffCount = 0;
    const void* output = nullptr;
    uint32_t outputWidth = 0, outputHeight = 0, outputFormat = 0;
    uint64_t frame = 0, epoch = 0;
    uint32_t droppedViews = 0, droppedTargets = 0;
    uint32_t droppedWorld = 0, droppedHandoff = 0, droppedLargeCb = 0;
    // Small CB loss alone cannot invalidate complete captured scene rows.
    uint32_t droppedSmallCb = 0, unknownLists = 0;
    uint64_t foreignCalls = 0;
    bool (*supportedPair)(uint64_t, uint64_t) = nullptr;
    // HDR only: a caller may certify that every draw of an alternate-camera
    // same-depth Pool record wrote its original passing fragments to a
    // conservative untrusted-camera coverage map.
    bool (*qualifiedAlternate)(const FlatContractRecord&, const unsigned char*, void*) = nullptr;
    void* qualifiedAlternateUser = nullptr;
};

// WHY A SCENE HAS NO MOTION SOURCE (design doc section 104, the training mission's turned-left view). The selector needs a draw whose
// shader pair the motion producer can substitute (a pool family: engine_velocity_families.h) to name the scene's depth, camera and motion
// slots; a view with none (open ground and sky) refuses every frame with no-supported-motion-source-pair until one returns. This is what
// the refused frame DID hold on the HDR's depth, so a log can say what the view was made of: the records and draws that write or bind
// that depth, how many used the HDR's camera, and the four pairs with the most draws. Filled by that one refusal and no other.
struct FlatMonoSourceless {
    struct Pair {
        uint64_t vs = 0, ps = 0;
        uint32_t draws = 0, records = 0;
        bool sameCamera = false;   // drawn with the HDR's camera
        bool pool = false;         // contract kind pool (a source candidate by kind)
    };
    uint32_t records = 0, draws = 0, sameCameraDraws = 0, poolRecords = 0, distinctPairs = 0;
    Pair top[4];
    uint32_t topCount = 0;
};

struct FlatMonoFrame {
    FlatMonoReason reason = FlatMonoReason::InvalidInput;
    uint64_t frame = 0, epoch = 0;
    const void* color = nullptr;  // post-tone format 27, before output copy/UI
    const void* hdr = nullptr;    // format 26, observed tone input at the variant's HDR slot
    const void* depth = nullptr, *dsv = nullptr, *sceneConstants = nullptr;
    const void* output = nullptr;
    uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    uint32_t depthFormat = 0;
    float camera[6][4] = {};
    float nearPlane = 0;
    uint64_t cameraHash = 0;
    uint32_t sourceFirst = 0, sourceLast = 0, hdrFirst = 0, hdrLast = 0;
    uint32_t toneSequence = 0, copySequence = 0, firstLaterOutput = 0;
    uint32_t supportedDraws = 0, unsupportedDraws = 0;
    bool mixedCamera = false;
    FlatMonoSourceless sourceless;
    bool selected() const { return reason == FlatMonoReason::Selected; }
};

namespace flat_mono_detail {
constexpr uint64_t kToneVs = 0xF9CFC798F21E9AEAull;
// Epic 20260926_124418 (EDHM chained): the same session's frames swap the
// tone slot's VS between the stock one, this no-constant passthrough
// (position + UV only -- vs_CFA91824129ECBBC review in build/flat-audit-menu)
// and this one, which adds a cb2[2].y varying z (vs_43CA9F1C0AD2ACFE review).
// None consumes a camera or projects; all three are interchangeable for the
// tone role, and the observed frames mix them against the PS variants below.
constexpr uint64_t kToneVsNoConst = 0xCFA91824129ECBBCull;
constexpr uint64_t kToneVsCbZ = 0x43CA9F1C0AD2ACFEull;
constexpr uint64_t kTonePs = 0xFEE777E92850B390ull;
// Epic 20260926_073622, all graphics settings maxed: the same tone VS with
// the DoF composite folded in. This PS blends the HDR -- bound at PS0 here,
// not PS1 -- with the quarter-res DoF blur at PS1, by a VS-varying factor,
// all at unchanged UV, with no depth texture, SV_Position or projection
// consumption of its own (ps_DE65BFFF2F12ECC6 review in
// build/flat-audit-menu). Same tone role, same jitter contract; its HDR
// lineage lives in slot 0.
constexpr uint64_t kToneDofCompositePs = 0xDE65BFFF2F12ECC6ull;
// Epic 20260926_124418 frame 33504 (EDHM chained): the settings-tier tone
// with the bloom composite folded in -- t0 is the HDR sampled at unchanged
// UV, t1 the soft-clamped bloom blended by cb2[1].w, no depth texture,
// SV_Position or matrix consumption (ps_9270C355389DA302 review). Its HDR
// lineage lives in slot 0, like the DoF composite.
constexpr uint64_t kToneBloomCompositePs = 0x9270C355389DA302ull;
// Same session, frame 33939: EDHM's recolor grade folded into the tone --
// t1 is the HDR at unchanged UV, t2 the bloom, t0 a color-grade LUT applied
// after tonemapping, t120 the mod's own config table; no depth texture,
// SV_Position or matrix consumption (ps_EAA5F18F10533BD1 review). Same tone
// role and jitter contract; its HDR lineage lives in slot 1, like the stock.
constexpr uint64_t kToneEdhmGradePs = 0xEAA5F18F10533BD1ull;
constexpr uint64_t kCopyVs = 0x20F383BBAC05C031ull;
constexpr uint64_t kCopyPs = 0xDED8796049C7BB4Aull;

inline bool knownToneVs(uint64_t vs) {
    return vs == kToneVs || vs == kToneVsNoConst || vs == kToneVsCbZ;
}
// The slot a known tone PS binds its HDR lineage to: stock and the EDHM
// grade read it at PS1, the DoF and bloom composites at PS0 (their PS1 is
// the blur/bloom). ~0u when the PS is no known tone variant.
inline uint32_t tonePsHdrSlot(uint64_t ps) {
    if (ps == kTonePs || ps == kToneEdhmGradePs) return 1;
    if (ps == kToneDofCompositePs || ps == kToneBloomCompositePs) return 0;
    return ~0u;
}
// ~0u unless (vs,ps) is a known tone pass; otherwise its HDR lineage slot.
inline uint32_t toneHdrSlot(uint64_t vs, uint64_t ps) {
    return knownToneVs(vs) ? tonePsHdrSlot(ps) : ~0u;
}
// The resource a recorded tone pass reads its HDR lineage from, or nullptr
// when (vs,ps) is no known tone variant. Every consumer of the tone's HDR
// input goes through this; hardcoded slots aggregated the DoF blur as HDR.
inline const void* toneHdrInput(const FlatContractObservation& k) {
    const uint32_t slot = toneHdrSlot(k.vs, k.ps);
    return slot < 2 ? k.srvResource[slot] : nullptr;
}

inline const FlatContractRecord& record(const FlatMonoFrameInput& in, uint32_t i) {
    return i < in.worldCount ? in.world[i] : in.handoff[i - in.worldCount];
}
inline bool ordered(const FlatContractRecord& r) {
    return r.draws && r.first && r.last >= r.first &&
        (r.draws == 1 ? r.first == r.last : r.last > r.first);
}
inline bool oneDraw(const FlatContractRecord& r) {
    return r.draws == 1 && ordered(r) && r.firstInstances == 1 && r.lastInstances == 1;
}
inline bool fullViewportExtent(uint32_t count, const float* viewport, uint32_t w, uint32_t h) {
    return w && h && count == 1 &&
        viewport[0] == 0 && viewport[1] == 0 &&
        viewport[2] == static_cast<float>(w) && viewport[3] == static_cast<float>(h);
}
inline bool fullViewportExtent(const FlatContractObservation& k, uint32_t w, uint32_t h) {
    return fullViewportExtent(k.viewportCount, k.viewport, w, h);
}
inline bool fullViewport(uint32_t count, const float* viewport, uint32_t w, uint32_t h) {
    return fullViewportExtent(count, viewport, w, h) && viewport[4] == 0 && viewport[5] == 1;
}
inline bool fullViewport(const FlatContractObservation& k, uint32_t w, uint32_t h) {
    return fullViewport(k.viewportCount, k.viewport, w, h);
}
inline bool hdrViewport(uint32_t count, const float* viewport, uint32_t w, uint32_t h) {
    // Complete Epic frames 36865/41961 contain three HDR writes at full XY
    // extent with depth clamped to zero. They do not name the motion source.
    return fullViewportExtent(count, viewport, w, h) && viewport[4] == 0 &&
        (viewport[5] == 0 || viewport[5] == 1);
}
inline bool hdrViewport(const FlatContractObservation& k, uint32_t w, uint32_t h) {
    return hdrViewport(k.viewportCount, k.viewport, w, h);
}
inline bool cameraCurrent(const FlatContractRecord& r, uint64_t epoch) {
    return ordered(r) && r.key.b1 && r.key.camera &&
        r.firstWriteEpoch == epoch && r.lastWriteEpoch == epoch &&
        r.firstWriteSeq && r.firstWriteSeq <= r.first &&
        r.lastWriteSeq >= r.firstWriteSeq && r.lastWriteSeq <= r.last &&
        r.key.cameraHash == flatCameraHash(r.camera);
}
inline bool sameCamera(const FlatContractRecord& a, const FlatContractRecord& b) {
    return a.key.b1 == b.key.b1 && a.key.camera && b.key.camera &&
        a.key.cameraHash == b.key.cameraHash &&
        std::memcmp(a.camera, b.camera, kFlatCameraBytes) == 0;
}
// What the frame holds on the scene's depth when no draw names a supported source (FlatMonoSourceless). Counts every record that binds
// the HDR's depth, grouped by shader pair, and keeps the four pairs with the most draws; pairs beyond sixteen are counted, not named.
inline void summarizeSourceless(const FlatMonoFrameInput& in, uint32_t count, const void* depth,
                                const FlatContractRecord& hdrCamera, FlatMonoSourceless& out) {
    out = FlatMonoSourceless{};
    FlatMonoSourceless::Pair acc[16];
    uint32_t used = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = record(in, i);
        const auto& k = r.key;
        if (!k.depth || k.depth != depth) continue;
        const bool same = sameCamera(r, hdrCamera);
        ++out.records; out.draws += r.draws;
        if (same) out.sameCameraDraws += r.draws;
        if (k.kind == kFlatContractPool) ++out.poolRecords;
        uint32_t at = 0;
        while (at < used && !(acc[at].vs == k.vs && acc[at].ps == k.ps)) ++at;
        if (at == used) {
            ++out.distinctPairs;
            if (used == 16) continue;
            acc[used] = {}; acc[used].vs = k.vs; acc[used].ps = k.ps; acc[used].sameCamera = same;
            at = used++;
        }
        acc[at].draws += r.draws; ++acc[at].records;
        acc[at].sameCamera = acc[at].sameCamera && same;
        acc[at].pool = acc[at].pool || k.kind == kFlatContractPool;
    }
    for (uint32_t n = 0; n < 4 && n < used; ++n) {
        uint32_t best = n;
        for (uint32_t j = n + 1; j < used; ++j) if (acc[j].draws > acc[best].draws) best = j;
        const auto swap = acc[n]; acc[n] = acc[best]; acc[best] = swap;
        out.top[out.topCount++] = acc[n];
    }
}
inline bool cameraShape(const unsigned char* bytes, float (&rows)[6][4]) {
    std::memcpy(rows, bytes, sizeof(rows));
    for (const auto& row : rows)
        for (float value : row) if (!std::isfinite(value)) return false;
    // The measured source encoding: clip x/y/w columns span the view,
    // clip-z coefficients are zero and row 273 supplies positive near depth.
    if (rows[0][2] != 0 || rows[1][2] != 0 || rows[2][2] != 0 || rows[3][2] <= 0)
        return false;
    const double a = rows[0][0], b = rows[0][1], c = rows[0][3];
    const double d = rows[1][0], e = rows[1][1], f = rows[1][3];
    const double g = rows[2][0], h = rows[2][1], i = rows[2][3];
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(determinant) || determinant == 0) return false;
    return rows[4][0] == rows[0][3] && rows[4][1] == rows[1][3] && rows[4][2] == rows[2][3];
}
}  // namespace flat_mono_detail

inline FlatMonoFrame flatSelectMonoFrame(const FlatMonoFrameInput& in) {
    using namespace flat_mono_detail;
    FlatMonoFrame out{};
    out.frame = in.frame; out.epoch = in.epoch;
    auto refuse = [&](FlatMonoReason reason) { out.reason = reason; return out; };
    if (!in.frame || !in.epoch || !in.supportedPair ||
        in.worldCount > 224 || in.handoffCount > 32 ||
        (in.worldCount && !in.world) || (in.handoffCount && !in.handoff))
        return refuse(FlatMonoReason::InvalidInput);
    if (!in.output || !in.outputWidth || !in.outputHeight || in.outputFormat != 28)
        return refuse(FlatMonoReason::UnknownOutput);
    if (in.droppedViews || in.droppedTargets || in.droppedWorld ||
        in.droppedHandoff || in.droppedLargeCb)
        return refuse(FlatMonoReason::Truncated);
    if (in.unknownLists || in.foreignCalls) return refuse(FlatMonoReason::ForeignWork);
    const uint32_t count = in.worldCount + in.handoffCount;

    const FlatContractRecord* copy = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = record(in, i);
        if (r.key.color != in.output || r.key.vs != kCopyVs || r.key.ps != kCopyPs) continue;
        if (copy || r.draws != 1) return refuse(FlatMonoReason::AmbiguousOutputCopy);
        copy = &r;
    }
    if (!copy) return refuse(FlatMonoReason::NoOutputCopy);
    const auto& ck = copy->key;
    if (!oneDraw(*copy) || ck.kind != kFlatContractOutput || !ck.rtv || ck.depth || ck.dsv ||
        ck.format != in.outputFormat || ck.width != in.outputWidth || ck.height != in.outputHeight ||
        !fullViewport(ck, in.outputWidth, in.outputHeight))
        return refuse(FlatMonoReason::InvalidOutputCopy);
    if (!ck.srvView[0] || !ck.srvResource[0] || ck.srvResource[0] == in.output)
        return refuse(FlatMonoReason::BrokenLineage);

    const FlatContractRecord* tone = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = record(in, i);
        if (r.key.color != ck.srvResource[0] || toneHdrSlot(r.key.vs, r.key.ps) == ~0u) continue;
        if (tone || r.draws != 1) return refuse(FlatMonoReason::AmbiguousTonePass);
        tone = &r;
    }
    if (!tone) return refuse(FlatMonoReason::NoTonePass);
    const auto& tk = tone->key;
    // Each admitted variant names the slot its HDR lineage binds; the rest of
    // the role checks is unchanged across variants.
    const uint32_t hdrSlot = toneHdrSlot(tk.vs, tk.ps);
    if (!oneDraw(*tone) || !tk.rtv || tk.format != 27 || tk.depth || tk.dsv ||
        !fullViewport(tk, tk.width, tk.height) ||
        // The tone maps to the output by a uniform scale within rounding
        // (flatUniformScale, not exact aspect equality): a rounded render
        // size maps; a shadow-like or wild-aspect target does not. Per-axis
        // bounds mirror the scene band, excluding sub-half chains and
        // unbounded targets.
        !flatUniformScale(tk.width, tk.height, in.outputWidth, in.outputHeight) ||
        tk.width * 2 < in.outputWidth || tk.height * 2 < in.outputHeight ||
        tk.width > in.outputWidth * 2 || tk.height > in.outputHeight * 2)
        return refuse(FlatMonoReason::InvalidTonePass);
    if (!tk.srvView[hdrSlot] || !tk.srvResource[hdrSlot] || tk.srvResource[hdrSlot] == tk.color ||
        tk.srvResource[hdrSlot] == in.output) return refuse(FlatMonoReason::BrokenLineage);
    if (tone->last >= copy->first) return refuse(FlatMonoReason::WrongOrder);
    // Tone and copy are texture operations. Their bound VS b1 can have been
    // reused by unrelated work; the current scene camera belongs to HDR.
    const FlatContractRecord* hdr = nullptr;
    const FlatContractRecord* hdrCamera = nullptr;
    uint32_t hdrFirst = 0, hdrLast = 0, hdrCameraDraws = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = record(in, i);
        const auto& k = r.key;
        if (k.color != tk.srvResource[hdrSlot]) continue;
        if (!ordered(r) || k.format != 26 || k.width != tk.width || k.height != tk.height ||
            !k.rtv || !k.depth || !k.dsv || !k.depthFormat ||
            k.depthWidth != tk.width || k.depthHeight != tk.height ||
            !hdrViewport(k, tk.width, tk.height)) return refuse(FlatMonoReason::ConflictingHdr);
        if (r.last >= tone->first) return refuse(FlatMonoReason::WrongOrder);
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
    if (!hdrCameraDraws) return refuse(FlatMonoReason::NoHdrCamera);
    float camera[6][4];
    if (!cameraShape(hdrCamera->camera, camera)) return refuse(FlatMonoReason::InvalidCamera);

    uint32_t sourceFirst = 0, sourceLast = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = record(in, i);
        const auto& k = r.key;
        if (k.kind != kFlatContractPool || k.width != tk.width || k.height != tk.height ||
            k.depth != hdr->key.depth) continue;
        if (!in.supportedPair(k.vs, k.ps)) { out.unsupportedDraws += r.draws; continue; }
        if (!k.color || !k.rtv || k.dsv != hdr->key.dsv || k.depthFormat != hdr->key.depthFormat ||
            k.depthWidth != tk.width || k.depthHeight != tk.height)
            return refuse(FlatMonoReason::AmbiguousSource);
        if (!cameraCurrent(r, in.epoch) || !fullViewport(k, tk.width, tk.height))
            return refuse(FlatMonoReason::InvalidSource);
        if (!sameCamera(r, *hdrCamera)) return refuse(FlatMonoReason::AmbiguousSource);
        if (r.last >= tone->first || r.first > hdrLast) return refuse(FlatMonoReason::WrongOrder);
        out.supportedDraws += r.draws;
        if (!sourceFirst || r.first < sourceFirst) sourceFirst = r.first;
        if (r.last > sourceLast) sourceLast = r.last;
    }
    if (!out.supportedDraws) {
        // Section 104: what the view held instead, for the line that names it. Nothing else reads it.
        summarizeSourceless(in, count, hdr->key.depth, *hdrCamera, out.sourceless);
        return refuse(FlatMonoReason::NoSupportedSource);
    }

    // Detect another supported naming at this extent under the same camera
    // but another depth; a lucky matching chain is not proof of uniqueness.
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = record(in, i);
        const auto& k = r.key;
        if (k.kind == kFlatContractPool && k.width == tk.width && k.height == tk.height &&
            k.depth && k.depth != hdr->key.depth && in.supportedPair(k.vs, k.ps) &&
            cameraCurrent(r, in.epoch) && sameCamera(r, *hdrCamera))
            return refuse(FlatMonoReason::AmbiguousSource);
        // Another write into the tone target between tone and copy breaks
        // this handoff, including a coalesced record spanning the interval.
        if (&r != tone && k.color == tk.color && r.last >= tone->first && r.first <= copy->last)
            return refuse(FlatMonoReason::BrokenLineage);
        if (k.color == in.output && r.first > copy->last &&
            (!out.firstLaterOutput || r.first < out.firstLaterOutput)) out.firstLaterOutput = r.first;
    }
    out.color = tk.color; out.hdr = tk.srvResource[hdrSlot];
    out.depth = hdr->key.depth; out.dsv = hdr->key.dsv; out.depthFormat = hdr->key.depthFormat;
    out.sceneConstants = hdrCamera->key.b1; out.output = in.output;
    out.renderWidth = tk.width; out.renderHeight = tk.height;
    out.outputWidth = in.outputWidth; out.outputHeight = in.outputHeight;
    std::memcpy(out.camera, camera, sizeof(camera));
    out.nearPlane = camera[3][2]; out.cameraHash = hdrCamera->key.cameraHash;
    out.sourceFirst = sourceFirst; out.sourceLast = sourceLast;
    out.hdrFirst = hdrFirst; out.hdrLast = hdrLast;
    out.toneSequence = tone->first; out.copySequence = copy->first;
    out.reason = FlatMonoReason::Selected;
    return out;
}
}  // namespace edvr
