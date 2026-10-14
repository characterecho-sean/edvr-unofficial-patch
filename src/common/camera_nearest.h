#pragma once
// The nearest write, as a veto (fix.camera_nearest, temporary A/B; docs\camera-rows-carry-2026-09-25.md, the 2026-10-11 nearest entry).
//
// Ported 2026-10-13 from branch claude/hangar-test-4 / claude/camera-nearest (commits d3ed2787 and e40d6674, the veto form) as the first arm of the
// sky-flicker A/B (docs\sky-flicker-2026-10-13.md). The pose helpers it used came from camera_ipd_pair.h there; here they are in camera_pose_math.h.
//
// The legacy chooser (chooseCameraRows, src\d3d11\temporal_pass.cpp) takes the frame's camera rows from its own preference. Flight 6
// (edvr_gfx_20261011_141016) showed the track is what matters: the drawn camera at frame f+1 equals the write picked at f exactly, and the rare
// errors were writes 90 to 125 m off the track. So the legacy pick stands unless it is beyond the track's own step, and then the write nearest the
// last applied pick takes its place.
//
// Distance: translation in metres plus kMetresPerDeg for each degree of rotation. The weight: one degree of view rotation moves a scene point 2.9 m
// away by 5 cm, the depth of the cockpit and the hangar, so a degree counts as the translation it looks like at that depth.
//
// The veto: the legacy pick is applied unless its distance from the last applied pick exceeds T = kVetoBaseM + kVetoStepMul x the previous applied
// step. That is about 5 cm in a still hangar and about 1.7 m at 0.55 m a frame in flight. A normal pick never reaches T, so the veto never overrides
// it (an eye pair six centimetres apart does not alternate).
//
// The re-anchor: when the legacy pick is the same pose as last frame's (samePose: 1 mm, 0.1 deg), it has held two frames, and it is beyond T, it is
// a real cut (the galaxy map, a camera switch, a jump) and is taken even if an old-view write is still nearer.
//
// Never hold: the rule returns the legacy pick or one of this frame's continuous writes, never the previous rows. The identical-to-previous twin is
// not offered (the chooser excludes it), since it is last frame's rows again.
//
// With the key off the rule still runs and counts: its state follows the applied (legacy) rows, so its counts say what it would have done on the
// track the flight actually flew.
//
// Pure: no Windows, no globals. The chooser's use is in chooseCameraRows; the rig is tools\temporal_test.

#include "camera_pose_math.h"

namespace edvr::camnearest {

constexpr float kMetresPerDeg = 0.05f;      // the rotation term's weight: a degree counts as 5 cm of translation
constexpr float kVetoBaseM = 0.05f;         // the veto's threshold is this plus kVetoStepMul steps
constexpr float kVetoStepMul = 3.0f;

// The state: the last applied pick and the step that brought it there, and the legacy pick of the last frame.
struct State {
    bool havePrev = false;
    float prev[12] = {};
    bool haveStep = false;      // two applied picks exist, so step is measured; with none, the legacy pick stands (no basis for a veto)
    float step = 0.0f;          // the translation step, metres, from the pick before to prev
    bool haveLegacy = false;
    float legacy[12] = {};
};

// The distance between two poses: translation in metres plus kMetresPerDeg per degree of rotation.
inline float distance(const float a[12], const float b[12]) {
    const float dx = a[3] - b[3], dy = a[7] - b[7], dz = a[11] - b[11];
    float ra[9], rb[9];
    campose::rot3Of34(a, ra);
    campose::rot3Of34(b, rb);
    return std::sqrt(dx * dx + dy * dy + dz * dz) + kMetresPerDeg * campose::angle3Deg(ra, rb);
}

// The threshold T: the veto's distance from the last applied pick, metres.
inline float vetoLimit(const State& s) { return kVetoBaseM + kVetoStepMul * s.step; }

// The legacy pick is the same pose as last frame's legacy pick (the backstop's samePose: 1 mm, 0.1 deg).
inline bool legacyAgrees(const State& s, const float legacy[12]) {
    return s.haveLegacy && campose::samePose(legacy, s.legacy);
}

inline void noteLegacy(State& s, const float legacy[12]) {
    std::memcpy(s.legacy, legacy, sizeof(s.legacy));
    s.haveLegacy = true;
}

// The verdict for one frame. index: the candidate applied in place of the legacy pick (vetoed frames only), or -1 for the legacy pick. dist: the
// legacy pick's distance from the last applied pick (0 with no previous pick). vetoed: the legacy pick was beyond T and the nearest candidate
// took its place. reanchor: the legacy pick was beyond T but held two frames: a cut, taken.
struct Verdict {
    int index = -1;
    float dist = 0.0f;
    bool vetoed = false;
    bool reanchor = false;
};

// legacy: the legacy pick's rows. cands: the continuous writes in the chooser's preference order (ties go to the earlier), n of them.
// legacyAgreed: legacyAgrees for this frame's legacy pick. Without a previous pick the frame takes the legacy pick.
inline Verdict decide(const State& s, const float legacy[12], const float* const* cands, int n, bool legacyAgreed) {
    Verdict v;
    if (!s.havePrev) return v;
    v.dist = distance(legacy, s.prev);
    if (!s.haveStep || v.dist <= vetoLimit(s)) return v;   // a normal pick, or no step measured yet: the legacy pick stands
    if (legacyAgreed) {                     // beyond T and held two frames: a cut
        v.reanchor = true;
        return v;
    }
    int best = -1;
    for (int k = 0; k < n; ++k) {
        if (!cands[k]) continue;
        if (best < 0 || distance(cands[k], s.prev) < distance(cands[best], s.prev)) best = k;
    }
    if (best < 0) return v;                 // nothing to take in its place: the legacy pick
    v.index = best;
    v.vetoed = true;
    return v;
}

// The candidate applied this frame: the verdict's with the key on, the legacy pick (-1) with it off.
inline int applied(bool keyOn, const Verdict& v) { return keyOn ? v.index : -1; }

// Records the rows applied this frame: the state the next frame measures from.
inline void commit(State& s, const float rows[12]) {
    if (s.havePrev) {
        const float dx = rows[3] - s.prev[3], dy = rows[7] - s.prev[7], dz = rows[11] - s.prev[11];
        s.step = std::sqrt(dx * dx + dy * dy + dz * dz);
        s.haveStep = true;
    }
    std::memcpy(s.prev, rows, sizeof(s.prev));
    s.havePrev = true;
}

// ---- the 30 s line -------------------------------------------------------------------------------------------------------------------------------------------

struct Stats {
    uint32_t frames = 0;        // the legacy picks the rule judged in the window
    uint32_t differs = 0;       // ...that the rule replaced with another write (a veto that found one)
    uint32_t reanchors = 0;     // ...taken as a cut
    float maxDistM = 0.0f;      // the largest distance of a legacy pick from the last applied pick, metres
};

// differs: the verdict replaces the legacy pick (the caller compares the rows' ring indices). Counted with the key off too.
inline void note(Stats& s, const Verdict& v, bool differs) {
    ++s.frames;
    if (v.reanchor) ++s.reanchors;
    if (differs) ++s.differs;
    if (v.dist > s.maxDistM) s.maxDistM = v.dist;
}

inline void resetWindow(Stats& s) {
    s.frames = 0;
    s.differs = 0;
    s.reanchors = 0;
    s.maxDistM = 0.0f;
}

// "temporal camera nearest: frames=.. differs_from_legacy=.. reanchors=.. max_dist_m=.. key=on|off". With the key off, differs_from_legacy is what the
// rule WOULD have replaced. Returns the length written, or 0 when the buffer is too small.
inline int formatReport(char* buf, size_t n, const Stats& s, bool keyOn) {
    const int len = std::snprintf(buf, n,
        "temporal camera nearest: frames=%u differs_from_legacy=%u reanchors=%u max_dist_m=%.3f key=%s",
        s.frames, s.differs, s.reanchors, static_cast<double>(s.maxDistM), keyOn ? "on" : "off");
    return (len <= 0 || static_cast<size_t>(len) >= n) ? 0 : len;
}

}  // namespace edvr::camnearest
