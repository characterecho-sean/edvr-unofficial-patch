// F182c (native on foot VR): F69h keeps the left hand on the item through a stale first-person snapshot, and the hand's grip target is the last grip point it had on the item. Header only, no
// game or D3D11 state, so native_frame_test runs the very functions the hook uses (OffHand::step, in native_on_foot_core.h, holds the decision; this holds the point). Journal: the arc doc, F182.
//
// The flight edvr_gfx_20261030_115059 (build c7e4c7c3, the plasma shotgun in the sights, aim pitch +74.6; the eye dump 115659467002): the left hand open and off the gun beside the barrel, the
// arm large across the lower view. F67f counted the left hand "not solved ... the off hand is free (F69h): the FP left hand is not on the item" in 4 to 9 calls of every window at that pitch
// (11:56:54 to 11:57:05), and in 1 to 4 calls of three windows near draws at a level aim.
//
// What F69h's "not on the item" reads (OffHand::step, native_on_foot_core.h): not the geometry. Every F69h line of those windows reads the FP pair 31.1 cm apart (min 31.1, max 31.1) and 52's
// orientation against 51 changed 0.0 deg: the hand IS on the item. The decision is dropped by its INPUT: offInputValid needs the FP snapshot no older than kIkPoseDeviationMaxGapMs, and the F67f
// lines of the same windows read the snapshot's age 125, 2203 and 2531 ms, each beside a frame of 110, 2197, 1175, 2526 and 1172 ms between Presents (LONG FRAME and FREEZE lines of the same
// seconds). Three of those (110, 2197 and 2526 ms) were outside EDVR's hooks (the game's own, with 885 to 1136 buffers created); two (1175 and 1172 ms) were EDVR's own: 1154 ms of the Present hook
// in the object_probe tick, the pilot's eye dump (run 115659467002) arming at 11:56:59.45 and again at 11:57:04.7. One unreadable call cleared F69h's history and released the hand, and taking it
// back needed 11 fresh unique P1 frames, so at a few frames a second the hand stayed free for the 4 to 9 calls. It is not about the pitch: a level aim near a draw hits it too (1 to 4 calls).
// EDVR does not move the item or the FP hand (it writes only the third-person pose), so its shipped behaviour does not cause the release; its dump instrument did, twice.
//
// The fix, in two parts:
//  1. F69h (OffHand::step): an input that is invalid ONLY because the snapshot is stale keeps the decision and the history, for up to kOffHandStaleKeepSec of hook time (a call counts at most
//     0.1 s; counted: "released for staleness after 3 s"). A fresh call resumes the normal rule from the kept history: a fresh unchanged pair stays on with no 11 frames to wait for, and a pair that
//     really changed (past 40 cm, the explicit one-hand policy, a rig or item change) releases as it always did once fresh frames show it.
//  2. While that kept decision holds and the input is stale, the left grip target is the last grip point the hand had on the item, kept in the item's frame (the right grip target's, which the
//     right hand solves to and which moves with the item): left = relative x right, the relative being made on every fresh call F69h holds the pair at weight 1. The stale snapshot's left
//     target, which the game's stalled frame has left behind the item, does not drag the hand. No pitch or detach gate: hip or sights, any two-handed gun. Pistols and tools are never attached by
//     F69h, so nothing is kept for them.
#pragma once

#include "native_on_foot_core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f182 {

// The left target in the right target's frame, and back (row vectors: left = rel x right). False on a singular right target.
inline bool relativeOf(const float left[16], const float right[16], float rel[16]) noexcept {
    float inv[16];
    if (!pfzAffineInverse(right, inv)) return false;
    pfzMul(left, inv, rel);
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(rel[i])) return false;
    return true;
}
inline void leftFrom(const float rel[16], const float right[16], float left[16]) noexcept { pfzMul(rel, right, left); }

struct In {
    bool armed = false;            // an armed call with the snapshot and G usable
    bool gun = false;              // an ordinary two-handed-gun call: no pistol, tool, authored left arm, support cup, primary draw, no suspended selection
    bool liveOk = false;           // both live grip targets are sane
    bool fresh = false;            // F69h's input was readable this call (OffHand::Out::ok)
    bool two = false;              // F69h's decision: two-handed (attached)
    bool kept = false;             // F69h kept the decision through a stale input this call (OffHand::Out::kept)
    bool staleLimit = false;       // F69h released the hand because the stale stretch passed kOffHandStaleKeepSec this call
    double offW = 0.0;             // F69h's weight now, 0..1
    double staleAgeMs = 0.0;       // the FP snapshot's age this call
    uint64_t nowMs = 0;
    uint32_t item = 0, planGen = 0, fpGen = 0;
    uint64_t epoch = 0;
    uint8_t modeLeft = 0, modeRight = 0;   // how each live target was made (the anatomical palm frame, the attach fallback): the point is read against a right target made the way it was
    const float* left = nullptr;   // the live left and right grip targets, 16 floats each (the model's axes)
    const float* right = nullptr;
};
struct Out {
    bool kept = false;      // F69h kept the decision through a stale input this call
    bool limit = false;     // the stale stretch's limit released the hand this call
    bool held = false;      // kept and the left target below is the stored point's
    bool stored = false;    // the point was made from this call's live pair
    double staleAgeMs = 0.0;   // the snapshot's age on a kept call
    double pointAgeMs = 0.0;   // the stored point's age on a held call
    float left[16] = {};
};

struct State {
    float rel[16] = {};
    bool have = false;
    uint32_t item = 0, planGen = 0, fpGen = 0;
    uint64_t epoch = 0, atMs = 0;
    uint8_t modeLeft = 0, modeRight = 0;

    void reset() noexcept { *this = State{}; }
    Out step(const In& in) noexcept {
        Out o;
        if (!in.armed || !in.gun) {
            reset();
            return o;
        }
        if (have && (item != in.item || planGen != in.planGen || fpGen != in.fpGen || epoch != in.epoch)) reset();
        const bool full = in.fresh && in.two && in.offW >= 1.0 - 1e-9;   // F69h holds the pair at weight 1 on a readable input: its live pair is the item's
        if (full && in.liveOk && in.left && in.right) {
            float r[16];
            if (relativeOf(in.left, in.right, r)) {
                std::memcpy(rel, r, sizeof(rel));
                have = true;
                item = in.item; planGen = in.planGen; fpGen = in.fpGen; epoch = in.epoch;
                modeLeft = in.modeLeft; modeRight = in.modeRight;
                atMs = in.nowMs;
                o.stored = true;
            }
        }
        o.kept = in.kept;
        o.limit = in.staleLimit;
        if (in.kept) o.staleAgeMs = in.staleAgeMs;
        if (in.kept && have && in.liveOk && in.right && modeRight == in.modeRight) {
            o.held = true;
            o.pointAgeMs = in.nowMs >= atMs ? static_cast<double>(in.nowMs - atMs) : 0.0;
            leftFrom(rel, in.right, o.left);
        }
        return o;
    }
};

// What the F67f window counts of it (one writer, the hook): the calls F69h kept the decision through a stale input (and the snapshot's age on them), the stale stretches the limit released, the calls
// the left target was the held point, and the hand-to-grip error (cm) of the held calls the left arm was solved on.
struct Counters {
    uint64_t kept = 0, limit = 0, held = 0, errN = 0;
    double ageSum = 0.0, ageMax = 0.0, errSum = 0.0, errMax = 0.0;
    void add(const Out& o) noexcept {
        if (o.kept) {
            ++kept;
            ageSum += o.staleAgeMs;
            ageMax = std::max(ageMax, o.staleAgeMs);
        }
        if (o.limit) ++limit;
        if (o.held) ++held;
    }
    void addError(double cm) noexcept {
        if (!std::isfinite(cm)) return;
        ++errN;
        errSum += cm;
        errMax = std::max(errMax, cm);
    }
};

// The F67f line's ending: empty with no kept call and no release in the window, else the counts, and the window's other (unheld) solved left calls' hand-to-grip error to judge the held ones
// against (unheldMeanCm over unheldN calls). The log's lines end at 1200 bytes (the F67f line is about 600).
inline int formatClause(char* buf, size_t n, const Counters& c, double unheldMeanCm, uint64_t unheldN) noexcept {
    if (!buf || n == 0) return 0;
    buf[0] = '\0';
    if (!c.kept && !c.limit) return 0;
    const double kn = c.kept ? static_cast<double>(c.kept) : 1.0, en = c.errN ? static_cast<double>(c.errN) : 1.0;
    return std::snprintf(buf, n,
                         "; stale grip hold (F182c): kept through a stale FP input %llu calls (stale age mean %.0f max %.0f ms), released for staleness after %.0f s %llu; the left hand on the held grip "
                         "point in %llu calls, their hand-to-grip error mean %.2f max %.2f cm against %.2f cm on the %llu unheld calls",
                         static_cast<unsigned long long>(c.kept), c.ageSum / kn, c.ageMax, kOffHandStaleKeepSec, static_cast<unsigned long long>(c.limit), static_cast<unsigned long long>(c.held),
                         c.errSum / en, c.errMax, std::isfinite(unheldMeanCm) ? unheldMeanCm : 0.0, static_cast<unsigned long long>(unheldN));
}

}  // namespace f182
}  // namespace nofoot
}  // namespace edvr
