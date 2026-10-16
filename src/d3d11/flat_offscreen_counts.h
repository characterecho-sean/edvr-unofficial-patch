// What the offscreen fast path did, per frame and per window, and why not (flat_offscreen_fast.h). Dependency-free, so the census
// (flat_cpu.h) can carry it without the reducer's headers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

// Why a draw took the slow path, in the order the checks run. None = it did not.
enum class FlatOffscreenNo : unsigned {
    None = 0,
    NoMemo,           // no verified draw yet this frame
    OtherFrame,       // the verified draw is from an earlier frame
    Work,             // the frame is not a Full one (a Probe frame returns after the reducer; Paused never gets here)
    FrameState,       // the frame's once-a-frame work (the GPU span's opening, the foreground domain's reset) has not run yet
    Audit,            // a draw ingress audit (F10) is counting
    Diagnostics,      // a capture is armed (flatTemporalCapturing, a draw capture, draw packets): the game's state goes back at once
    Foreign,          // work from another thread or context was seen this frame
    Overlay,          // a protected HDR overlay suffix is open or planned
    ProjectionAudit,  // the F10 projection audit counts every draw (s.projectionFrames)
    WeaponFootprint,  // the weapon footprint probe is armed
    Foreground,       // the foreground ownership probe is active and this draw has a depth
    HdrConsumer,      // the draw could be the HDR route's consumer (the detector would act on it)
    Pair,             // the shader pair is a pool family, tone, copy, map tone, source tone or UI family
    Format,           // the target's format is one the skipped blocks act on (9, 23, 26, 27, 60), or it is the output
    Extent,           // the draw is of the scene's extent, or into a depth of it
    Shape,            // a fullscreen triangle: the UI layer's tone probes read it
    KeyDiffers,       // the draw is not the verified draw's state
    Count
};
inline constexpr unsigned kFlatOffscreenReasons = static_cast<unsigned>(FlatOffscreenNo::Count);
inline constexpr const char* kFlatOffscreenNames[kFlatOffscreenReasons] = {
    "none", "no verified draw", "other frame", "work tier", "frame state", "audit", "diagnostics", "foreign work", "overlay",
    "projection audit", "weapon footprint", "foreground", "hdr consumer", "pair", "format", "extent", "shape", "state changed"};

// What the runtime counts: the draws that took the fast path, the first draws of their kind (verified on the slow path and remembered), and
// the draws that matched the verified draw but were sent the slow way by a live condition, by reason. A draw that is not a repeat of the
// verified draw is none of these: most of a frame's draws are not.
struct FlatOffscreenCounts {
    uint64_t fast = 0, recorded = 0;
    uint64_t declined[kFlatOffscreenReasons] = {};
    uint64_t declinedTotal() const noexcept { uint64_t n = 0; for (uint64_t v : declined) n += v; return n; }
    void add(const FlatOffscreenCounts& o) noexcept {
        fast += o.fast; recorded += o.recorded;
        for (unsigned i = 0; i < kFlatOffscreenReasons; ++i) declined[i] += o.declined[i];
    }
};

// The line's token: "offscreen fast path: 3770.2 draws a frame skipped the draw scope's constructor and destructor (41.0 a frame were verified on the
// slow path first; 12.3 a frame matched but had to ask why not: foreign work 12.3)". Zeros are printed: an absent token means the block never ran.
inline void flatOffscreenFormat(char* out, size_t cap, const FlatOffscreenCounts& c, double frames) noexcept {
    if (!cap) return;
    const double f = frames > 0 ? frames : 1.0;
    size_t used = 0;
    auto put = [&](const char* format, auto... args) {
        if (used + 1 >= cap) return;
        const int n = std::snprintf(out + used, cap - used, format, args...);
        if (n > 0) used = used + static_cast<size_t>(n) < cap ? used + static_cast<size_t>(n) : cap - 1;
    };
    put("offscreen fast path: %.1f draws a frame skipped the draw scope's constructor and destructor (%.1f a frame were verified on the slow path "
        "first; %.1f a frame matched but had to ask why not",
        static_cast<double>(c.fast) / f, static_cast<double>(c.recorded) / f, static_cast<double>(c.declinedTotal()) / f);
    // The three most frequent reasons, largest first; "and N more" for the rest, so the token fits its 320 characters at any count.
    unsigned order[kFlatOffscreenReasons], n = 0;
    for (unsigned i = 1; i < kFlatOffscreenReasons; ++i) if (c.declined[i]) order[n++] = i;
    for (unsigned i = 1; i < n; ++i)
        for (unsigned j = i; j > 0 && c.declined[order[j]] > c.declined[order[j - 1]]; --j) { const unsigned t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    for (unsigned i = 0; i < n && i < 3; ++i) put("%s %s %.1f", i ? "," : ":", kFlatOffscreenNames[order[i]], static_cast<double>(c.declined[order[i]]) / f);
    if (n > 3) put(", and %u more", n - 3);
    put(")");
    out[used < cap ? used : cap - 1] = 0;
}

}  // namespace edvr
