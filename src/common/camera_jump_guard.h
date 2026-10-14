#pragma once
// The camera jump guard (fix.camera_jump_guard, temporary A/B; docs\camera-rows-carry-2026-09-25.md, the 2026-10-11 jump guard entry).
//
// The legacy chooser (chooseCameraRows, src\d3d11\temporal_pass.cpp) takes the frame's camera rows by orientation continuity: a write within 3 deg of
// last frame's applied rows. In the hangar flight edvr_gfx_20261011_091619 two one-frame excursions passed that gate (f11797: 0.19 m and 2.99 deg;
// f14857: 2.94 deg), and each matched a DLSS history reset on the static panels.
//
// Ported 2026-10-13 from branch claude/hangar-test-4 (commit 998921da, present in builds -53 and -58, where no camera-row events appeared) as the
// second arm of the sky-flicker A/B (docs\sky-flicker-2026-10-13.md). The tests it uses came from camera_ipd_pair.h there; here they are in
// camera_pose_math.h, since main has no IPD rule.
//
// The guard tests the legacy pick against the previous applied pose with the same two tests (campose::departs and continuesStep): a translation more than 3 cm
// from it that does not continue the previous step (constant velocity, within 1 cm plus 5% of the step, and 0.5 deg) is rejected. A rejected pick takes
// the chooser's next-best candidate that passes the same test, in the chooser's own preference order. The candidates are the frame's continuous
// writes: the bound object's first, then the latest. The identical-to-previous twin is not one of them, since it is last frame's rows again.
//
// Never hold. The guard only ever returns one of the frame's own writes, never the previous rows. A jump that persists for kReplaceLimit frames in a
// row is a real camera change (the galaxy map, an external camera, on foot), so the legacy pick is taken; a pick with no passing candidate is taken
// at once. After either, the applied pose is the new camera, and the next frame measures from it.
//
// With the key off the guard still runs and counts: its state follows the applied (legacy) rows, so the counts say what the guard would have done on
// the track the flight actually flew.
//
// Pure: no Windows, no globals. The chooser's use is in chooseCameraRows; the rig is tools\temporal_test.

#include "camera_pose_math.h"

namespace edvr::jumpguard {

constexpr uint32_t kReplaceLimit = 3;   // consecutive replaced frames before a persisting jump is taken as a real change

// The guard's state: the applied poses (the last two, for the constant-velocity prediction) and the run of replaced frames.
struct Guard {
    campose::Chain chain;
    uint32_t replaceRun = 0;
};

// The guard's verdict for one frame. alt is an index into the alternates the chooser offered, or -1 for the legacy pick. jumpM is the legacy pick's
// translation from the previous applied pose, set when it was rejected.
struct Decision {
    int alt = -1;
    bool rejected = false;
    float jumpM = 0.0f;
};

// The test: a pose passes when it is not a jump, or when it continues the previous step.
inline bool passes(const float cand[12], const campose::Chain& c) {
    return !campose::departs(cand, c.pose) ||
           campose::continuesStep(cand, c.pose, c.havePrev ? c.before : nullptr);
}

// One frame. legacy: the legacy pick's rows. alts: the alternates in preference order (this frame's writes, never the previous rows), altN of them.
// Without a previous pose there is nothing to jump from, so the legacy pick passes.
inline Decision decide(const Guard& g, const float* legacy, const float* const* alts, int altN) {
    Decision d;
    if (!g.chain.havePose || passes(legacy, g.chain)) return d;
    d.rejected = true;
    d.jumpM = campose::translationM(legacy, g.chain.pose);
    if (g.replaceRun >= kReplaceLimit) return d;   // a jump that persists is a real change: taken
    for (int k = 0; k < altN; ++k) {
        if (alts[k] && passes(alts[k], g.chain)) {
            d.alt = k;
            return d;
        }
    }
    return d;
}

// The alternate applied this frame: the verdict's with the key on, the legacy pick (-1) with it off.
inline int applied(bool keyOn, const Decision& d) { return keyOn ? d.alt : -1; }

// Records the rows applied this frame: the state the next frame measures from. replaced: an alternate was applied this frame.
inline void commit(Guard& g, const float* rows, bool replaced) {
    campose::commitPose(g.chain, rows);
    g.replaceRun = replaced ? g.replaceRun + 1 : 0;
}

// ---- the 30 s line -------------------------------------------------------------------------------------------------------------------------------------------

struct Stats {
    uint32_t frames = 0;        // the legacy picks the guard judged in the window
    uint32_t rejected = 0;      // ...whose translation jump failed the test (a replacement, or a real change taken)
    uint32_t replaced = 0;      // ...that the guard would replace with the next-best passing candidate
    float maxJumpM = 0.0f;      // the largest rejected jump in the window, metres
};

inline void note(Stats& s, const Decision& d) {
    ++s.frames;
    if (!d.rejected) return;
    ++s.rejected;
    if (d.alt >= 0) ++s.replaced;
    if (d.jumpM > s.maxJumpM) s.maxJumpM = d.jumpM;
}

// The window's counts start over.
inline void resetWindow(Stats& s) {
    s.frames = 0;
    s.rejected = 0;
    s.replaced = 0;
    s.maxJumpM = 0.0f;
}

// "temporal camera jump guard: frames=.. rejected=.. replaced=.. passed_through=.. key=on|off|superseded max_jump_m=..". passed_through is the legacy
// picks applied unchanged: frames minus replaced, real camera changes among them. key is the state the pick was actually made under: "on", "off", or
// "superseded" (the key is on but fix.camera_nearest is on too and wins, so the guard only counted). With the key not "on", replaced is what the guard
// WOULD have replaced. Returns the length written, or 0 when the buffer is too small.
inline int formatReport(char* buf, size_t n, const Stats& s, const char* key) {
    const uint32_t passedThrough = s.frames >= s.replaced ? s.frames - s.replaced : 0u;
    const int len = std::snprintf(buf, n,
        "temporal camera jump guard: frames=%u rejected=%u replaced=%u passed_through=%u key=%s max_jump_m=%.3f",
        s.frames, s.rejected, s.replaced, passedThrough, key ? key : "off", static_cast<double>(s.maxJumpM));
    return (len <= 0 || static_cast<size_t>(len) >= n) ? 0 : len;
}

}  // namespace edvr::jumpguard
