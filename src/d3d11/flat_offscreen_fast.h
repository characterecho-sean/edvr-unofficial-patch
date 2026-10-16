// The offscreen fast path: a draw into a shadow map, a depth-only target or a small intermediate (class 4 of the draw class census,
// docs\flat-cz-fps-2026-10-11.md, 2026-10-16) that repeats the previous verified draw of its kind skips the draw scope's constructor and
// destructor and does only what such a draw needs: the pair-facts store, engine motion's restore, the prefix model's reducer (the count, q and
// the target's record), the HDR detector's observation, the trace record and the UI layer's scene-draw note.
//
// WHY IT IS SOUND. The constructor of a draw that is not a motion source, not a tone or copy, not a UI family, not a screen-extent or HDR
// target and not a fullscreen triangle spends its time in blocks that each do nothing for such a draw while a short list of live conditions
// holds. This file names the draw conditions (flatOffscreenShapeVerdict: a pure function of the draw's own key, its shader pair's facts and
// the frame's extent) and the live conditions (flatOffscreenLiveVerdict: each is one field of FlatOffscreenLive, filled by the runtime from
// the state it names, and each comment says which skipped block reads it). The reducer, the HDR detector and the trace are NOT skipped: they
// are called for real, with a draw record that is a pure function of the key (FlatOffscreenMemo keeps it, and the rig replays the 46-frame
// corpus with the memo's record in place of each repeat's own, requiring identical contracts).
//
// THE KEY (FlatOffscreenKey), every input named: the shader pair (the pair class), the colour, depth, RTV and DSV and their extents and
// formats, the viewport, the constant buffer at b1, the camera rows' hash and write provenance (the camera-row epoch), and the generations of
// the RTV and DSV bindings (a rebind is not evidence of the same object). The frame and the draw args (a fullscreen triangle never qualifies)
// ride beside it. The rig drops each input in turn and requires a failure.
//
// Pure and header-only: tools\flat_temporal_test drives every function.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "flat_offscreen_counts.h"
#include "flat_pair_facts.h"
#include "flat_runtime_model.h"

namespace edvr {

// The draw's own arguments (the hook's, not the context's).
struct FlatOffscreenArgs {
    char kind = '?';
    uint32_t count = 0, instances = 1, startInstance = 0;
};
// A fullscreen triangle: the UI layer's tone candidates and the back-off trail read it (flat_runtime.cpp: trailDraw, the count == 3 branch).
inline bool flatOffscreenFullscreenTriangle(const FlatOffscreenArgs& a) noexcept {
    return a.count == 3 && a.instances == 1 && (a.kind == 'D' || a.kind == 'N');
}

// ---- the draw conditions: pure, value-based --------------------------------------------------------------------------------
// The formats the skipped blocks act on: 9 (image intermediates, the untrusted/unclassified pool, the HDR image copy), 23 (the motion
// source's colour), 26 (HDR: overlay planning, the deferred camera, the menu and image copies), 27 and 60 (screen formats).
inline bool flatOffscreenActedOnFormat(uint32_t format) noexcept {
    return format == 9 || format == 23 || format == 26 || format == 27 || format == 60;
}
// None when the draw is a class-4 draw the fast path may serve; else the reason it may not.
inline FlatOffscreenNo flatOffscreenShapeVerdict(const FlatContractObservation& k, const FlatPairFacts& pair, uint32_t prefixWidth,
                                                 uint32_t prefixHeight, const void* output, const FlatOffscreenArgs& a) noexcept {
    // The pair: nothing the pair table names (a motion source's substitution, a tone, the final copy, a UI family). kCopyPs on any
    // vertex shader is the UI layer's "plain copy" branch.
    if (pair.supported || pair.toneSlot != ~0u || pair.mapToneSlot != ~0u || pair.sourceTone || pair.copyPair || pair.uiMayAsk ||
        k.ps == flat_mono_detail::kCopyPs)
        return FlatOffscreenNo::Pair;
    if (k.color && (k.color == output || flatOffscreenActedOnFormat(k.format))) return FlatOffscreenNo::Format;
    // A depth of the scene's size: the foreground domain's candidate block reads it (`k.depth && (sceneExtent || nativeDepthExtent)`). The other
    // half of that condition, sceneExtent (flatContractKind's Screen), needs a colour target of format 23, 26, 27 or 60 (9 is read as 26), and
    // every one of those is refused above, so it cannot be true here and is not asked.
    const bool nativeDepth = k.depth && k.depthWidth == prefixWidth && k.depthHeight == prefixHeight;
    if (nativeDepth) return FlatOffscreenNo::Extent;
    if (flatOffscreenFullscreenTriangle(a)) return FlatOffscreenNo::Shape;
    return FlatOffscreenNo::None;
}

// ---- the live conditions ---------------------------------------------------------------------------------------------------------
// Each field is one condition the skipped blocks read, filled by the runtime (flatOffscreenLiveOf in flat_runtime.cpp) from the state
// named in its comment. A live condition is read at every draw: there is no epoch word to bump and so no writer to miss.
struct FlatOffscreenLive {
    bool workFull = false;              // s.work == FlatWork::Full: a Probe frame returns after the reducer, Paused never gets here
    bool gpuFrameOpened = false;        // s.gpuFrameTried: the first game draw of the frame opened the GPU span (gpuFrameOpen)
    bool foregroundFrameReady = false;  // s.engine == Taa, or s.foregroundDomainFrame == s.prefix.frame: the once-a-frame reset ran
    bool auditing = false;              // drawIngressAudit.active: the audit counts and samples every draw
    bool diagnostics = false;           // flatTemporalCapturing() || s.drawCapture.active() || s.drawPackets.armed()
    bool foreignWork = false;           // foreignWork: the constructor marks the prefix uncertain and fails an open overlay
    bool overlayOpen = false;           // overlayOpen(s) || overlaySuffixActive: the reducer's suffix rule and the effective-depth query
    bool projectionAudit = false;       // s.projectionFrames != 0: projectionDraws counts, captures read the context
    bool weaponFootprint = false;       // s.weaponFootprint.active(): the footprint probe watches its named pair
    bool foregroundTouchesDepth = false;// s.foreground.active() and the draw has a depth: needsPreWorldState / noteAfterWorldDraw / noteSameDepthDraw
    bool hdrCouldConsume = false;       // flatHdrCouldConsume(s.hdr, k): the detector might fire on this draw
};
inline FlatOffscreenNo flatOffscreenLiveVerdict(const FlatOffscreenLive& l) noexcept {
    if (!l.workFull) return FlatOffscreenNo::Work;
    if (!l.gpuFrameOpened || !l.foregroundFrameReady) return FlatOffscreenNo::FrameState;
    if (l.auditing) return FlatOffscreenNo::Audit;
    if (l.diagnostics) return FlatOffscreenNo::Diagnostics;
    if (l.foreignWork) return FlatOffscreenNo::Foreign;
    if (l.overlayOpen) return FlatOffscreenNo::Overlay;
    if (l.projectionAudit) return FlatOffscreenNo::ProjectionAudit;
    if (l.weaponFootprint) return FlatOffscreenNo::WeaponFootprint;
    if (l.foregroundTouchesDepth) return FlatOffscreenNo::Foreground;
    if (l.hdrCouldConsume) return FlatOffscreenNo::HdrConsumer;
    return FlatOffscreenNo::None;
}

// ---- the key ----------------------------------------------------------------------------------------------------------------------
struct FlatOffscreenKey {
    const void* color = nullptr; const void* depth = nullptr; const void* rtv = nullptr; const void* dsv = nullptr; const void* b1 = nullptr;
    uint64_t vs = 0, ps = 0;               // the pair class
    uint64_t cameraHash = 0, writeEpoch = 0;
    uint32_t writeSeq = 0;                 // with writeEpoch: the camera-row epoch
    uint32_t width = 0, height = 0, format = 0, depthWidth = 0, depthHeight = 0, depthFormat = 0;
    uint32_t viewportCount = 0;
    float viewport[6] = {};
    bool hasCamera = false;
    uint32_t rtvGen = 0, dsvGen = 0;       // binding generations: a rebind is not evidence of the same object
    // Which inputs a comparison reads: the rig drops one at a time (1 shift per input) and requires the comparison to fail.
    // Inputs, in order: 0 vs, 1 ps, 2 color, 3 depth, 4 rtv, 5 dsv, 6 b1, 7 cameraHash, 8 writeEpoch, 9 writeSeq, 10 hasCamera,
    // 11 extents/formats, 12 viewport, 13 rtvGen, 14 dsvGen.
    static constexpr unsigned kInputs = 15;
    bool same(const FlatOffscreenKey& o, unsigned dropped = ~0u) const noexcept {
        const auto in = [&](unsigned bit) { return dropped != bit; };
        if (in(0) && vs != o.vs) return false;
        if (in(1) && ps != o.ps) return false;
        if (in(2) && color != o.color) return false;
        if (in(3) && depth != o.depth) return false;
        if (in(4) && rtv != o.rtv) return false;
        if (in(5) && dsv != o.dsv) return false;
        if (in(6) && b1 != o.b1) return false;
        if (in(7) && cameraHash != o.cameraHash) return false;
        if (in(8) && writeEpoch != o.writeEpoch) return false;
        if (in(9) && writeSeq != o.writeSeq) return false;
        if (in(10) && hasCamera != o.hasCamera) return false;
        if (in(11) && (width != o.width || height != o.height || format != o.format || depthWidth != o.depthWidth ||
                       depthHeight != o.depthHeight || depthFormat != o.depthFormat))
            return false;
        if (in(12) && (viewportCount != o.viewportCount || std::memcmp(viewport, o.viewport, sizeof(viewport)) != 0)) return false;
        if (in(13) && rtvGen != o.rtvGen) return false;
        if (in(14) && dsvGen != o.dsvGen) return false;
        return true;
    }
};
// The key of a draw's observation (the runtime fills the observation from the same fields before the reducer sees it).
inline FlatOffscreenKey flatOffscreenKeyOf(const FlatContractObservation& k, uint32_t rtvGen, uint32_t dsvGen) noexcept {
    FlatOffscreenKey key;
    key.color = k.color; key.depth = k.depth; key.rtv = k.rtv; key.dsv = k.dsv; key.b1 = k.b1;
    key.vs = k.vs; key.ps = k.ps; key.cameraHash = k.cameraHash; key.writeEpoch = k.writeEpoch; key.writeSeq = k.writeSeq;
    key.width = k.width; key.height = k.height; key.format = k.format;
    key.depthWidth = k.depthWidth; key.depthHeight = k.depthHeight; key.depthFormat = k.depthFormat;
    key.viewportCount = k.viewportCount; std::memcpy(key.viewport, k.viewport, sizeof(key.viewport));
    key.hasCamera = k.camera != nullptr; key.rtvGen = rtvGen; key.dsvGen = dsvGen;
    return key;
}

// ---- the memo ---------------------------------------------------------------------------------------------------------------------
// The previous verified draw of its kind: its key and its draw record. The record is a pure function of the key (nothing in it is set by a
// block the fast path skips), so a draw with the same key reuses it with only its instance count changed.
class FlatOffscreenMemo {
public:
    bool valid() const noexcept { return valid_; }
    uint64_t frame() const noexcept { return frame_; }
    const FlatOffscreenKey& key() const noexcept { return key_; }
    void forget() noexcept { valid_ = false; }
    // The reason the draw (key `now`, frame `frame`) cannot reuse the verified draw, or None.
    FlatOffscreenNo lookup(uint64_t frame, const FlatOffscreenKey& now) const noexcept {
        if (!valid_) return FlatOffscreenNo::NoMemo;
        if (frame != frame_) return FlatOffscreenNo::OtherFrame;
        return key_.same(now) ? FlatOffscreenNo::None : FlatOffscreenNo::KeyDiffers;
    }
    // A verified draw of this frame: the draw conditions and the live conditions held, and its slow path did nothing beyond what the fast
    // path does. The record's camera pointer is re-pointed at the memo's own copy of the rows.
    void record(uint64_t frame, const FlatOffscreenKey& key, const FlatRuntimeDraw& d) {
        if (valid_ && frame_ == frame && key_.same(key)) return;   // already held: nothing to copy
        valid_ = true; frame_ = frame; key_ = key; draw_ = d;
        draw_.key.camera = d.key.camera ? draw_.camera : nullptr;
    }
    // The draw record for a hit, its instance count set. Owner thread; valid until the next record().
    FlatRuntimeDraw& use(uint32_t instances) noexcept {
        draw_.instances = instances;
        if (draw_.key.camera) draw_.key.camera = draw_.camera;
        return draw_;
    }
    const FlatRuntimeDraw& held() const noexcept { return draw_; }

private:
    bool valid_ = false;
    uint64_t frame_ = 0;
    FlatOffscreenKey key_{};
    FlatRuntimeDraw draw_{};
};

}  // namespace edvr
