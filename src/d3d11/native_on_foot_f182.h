// F182c (native on foot VR): the left hand stays on a two-handed gun aimed steeply up. Header only, no game or D3D11 state, so native_frame_test runs the very functions the hook uses.
// Journal: the arc doc, F182.
//
// The flight edvr_gfx_20261030_115059 (build c7e4c7c3, the plasma shotgun in the sights, aim pitch +74.6; the eye dump 115659467002): the left hand open and off the gun beside the barrel, the
// arm large across the lower view. F67f counted the left hand "not solved ... the off hand is free (F69h): the FP left hand is not on the item" in 4 to 9 calls of every window at that pitch
// (11:56:54 to 11:57:05), and in 1 to 4 calls of three windows near draws at a level aim.
//
// What F69h's "not on the item" reads (OffHand::step, native_on_foot_core.h): not the geometry. Every F69h line of those windows reads the FP pair 31.1 cm apart (min 31.1, max 31.1) and 52's
// orientation against 51 changed 0.0 deg: the hand IS on the item. The decision is dropped by its INPUT: offInputValid needs the FP snapshot no older than kIkPoseDeviationMaxGapMs, and the F67f
// lines of the same windows read the snapshot's age 125, 2203 and 2531 ms, each beside a frame of 110, 2197, 1175, 2526 and 1172 ms between Presents (LONG FRAME and FREEZE lines of the same
// seconds). Three of those (110, 2197 and 2526 ms) were outside EDVR's hooks (the game's own, with 885 to 1136 buffers created); two (1175 and 1172 ms) were EDVR's own: 1154 ms of the Present hook
// in the object_probe tick, the pilot's eye dump (run 115659467002) arming at 11:56:59.45 and again at 11:57:04.7. One unreadable call clears F69h's history and releases the hand, and taking it
// back needs 11 fresh unique P1 frames, so at a few frames a second the hand stays free for the 4 to 9 calls. EDVR does not move the item or the FP hand (it writes only the third-person
// pose), so its shipped behaviour does not cause the release; its dump instrument did, twice, for the seconds it was armed.
//
// The hold: in the sights (detach weight 0.5 or more) with a two-handed gun (F69h had the pair on this item, and the left grip target was stored) AND the game's aim pitch at 45 deg or more up,
// while F69h does not hold the hand fully, the left grip target is the stored point: the last grip point it had on the item, kept in the item's frame (the right grip target's, which the right
// hand solves to and which moves with the item): left = relative x right, the relative being made on every call F69h holds the pair at weight 1. The hold's weight is 1 while it holds and 0.15 s
// falling after; offW is the larger of F69h's and the hold's. It ends when F69h holds the pair again, the sights or the pitch end, the item or a rig changes, or after kHoldMaxSec (counted).
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

constexpr double kSteepDeg = 45.0;     // the game's aim pitch up from which the hold applies
constexpr double kDetachMin = 0.5;     // the sights: the ADS detach weight from which the hold applies
constexpr double kHoldMaxSec = 3.0;    // a hold ends after this much hook time (counted); the call's step is held to kStepMaxSec, so a freeze does not spend it
constexpr double kStepMaxSec = 0.10;
constexpr double kReleaseSec = 0.15;   // the hold's weight falls to 0 over this long once it ends (F69h's own ramp)
constexpr double kHeldTolCm = 1.0;     // the health rule: the held calls' hand-to-grip error under this is a hand on the point
constexpr uint64_t kMinSteepCalls = 30;   // the health rule: fewer steep two-handed ADS calls than this are too few to judge

// The sights and a steep look up: the detach weight at kDetachMin or more and the aim pitch at kSteepDeg or more (a value that is not a number is neither).
inline bool steepSights(double detachW, double pitchDeg) noexcept {
    return std::isfinite(detachW) && std::isfinite(pitchDeg) && detachW >= kDetachMin && pitchDeg >= kSteepDeg;
}

// The aim's pitch (deg, up positive) from G's forward row (the FP root's forward in the model's axes: G.g[8], G.g[9], G.g[10]): pinStage0's f86AimPitch. Not a number for a zero row.
inline double aimPitchDeg(const float g[16]) noexcept {
    const double x = g[8], y = g[9], z = g[10];
    const double h = std::sqrt(x * x + z * z);
    if (!(h + std::fabs(y) > 1e-9)) return std::nan("");
    const double p = std::atan2(y, h) * 180.0 / kIkPi;
    return std::isfinite(p) ? p : std::nan("");
}

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
    bool two = false;              // F69h's decision: two-handed (attached) with readable inputs
    double offW = 0.0;             // F69h's weight now, 0..1
    double detachW = 0.0, pitchDeg = std::nan("");
    double dtSec = 0.0;
    uint64_t nowMs = 0;
    uint32_t item = 0, planGen = 0, fpGen = 0;
    uint64_t epoch = 0;
    uint8_t modeLeft = 0, modeRight = 0;   // how each live target was made (the anatomical palm frame, the attach fallback): the point is read against a right target made the way it was
    const float* left = nullptr;   // the live left and right grip targets, 16 floats each (the model's axes)
    const float* right = nullptr;
};
struct Out {
    bool steep = false;     // the sights, the pitch up, a stored point of this item
    bool freed = false;     // steep and F69h does not hold the pair fully
    bool held = false;      // the left target below is the stored point's (the hold is on, or falling)
    bool limit = false;     // the limit ended the hold this call
    bool stored = false;    // the point was made from this call's live pair
    double w = 0.0;         // the hold's weight, 0..1
    double ageMs = 0.0;     // the stored point's age
    float left[16] = {};
};

struct State {
    float rel[16] = {};
    bool have = false;
    uint32_t item = 0, planGen = 0, fpGen = 0;
    uint64_t epoch = 0, atMs = 0;
    uint8_t modeLeft = 0, modeRight = 0;
    bool active = false, spent = false;   // holding; the limit ended this stretch (held off until F69h or the condition ends it)
    double heldSec = 0.0, w = 0.0;

    void reset() noexcept { *this = State{}; }
    Out step(const In& in) noexcept {
        Out o;
        if (!in.armed || !in.gun) {
            reset();
            return o;
        }
        if (have && (item != in.item || planGen != in.planGen || fpGen != in.fpGen || epoch != in.epoch)) reset();
        const double dt = !(in.dtSec > 0.0) || !std::isfinite(in.dtSec) ? 0.0 : in.dtSec > kStepMaxSec ? kStepMaxSec : in.dtSec;
        const bool full = in.two && in.offW >= 1.0 - 1e-9;   // F69h holds the pair at weight 1: its live pair is the item's
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
        o.steep = have && steepSights(in.detachW, in.pitchDeg);
        o.freed = o.steep && !full;
        const bool pointOk = have && in.liveOk && in.right && modeRight == in.modeRight;
        if (full) {
            active = false;
            spent = false;
            heldSec = 0.0;
            w = 0.0;
        } else {
            const bool want = o.freed && pointOk && !spent;
            if (want) {
                if (!active) {
                    active = true;
                    heldSec = 0.0;
                }
                heldSec += dt;
                if (heldSec > kHoldMaxSec) {
                    active = false;
                    spent = true;
                    o.limit = true;
                }
            } else {
                active = false;
                if (!o.freed) spent = false;
            }
            if (active) w = 1.0;
            else if (!pointOk) w = 0.0;
            else w = std::max(0.0, w - dt / kReleaseSec);
        }
        o.w = w;
        if (w > 0.0 && pointOk) {
            o.held = true;
            o.ageMs = in.nowMs >= atMs ? static_cast<double>(in.nowMs - atMs) : 0.0;
            leftFrom(rel, in.right, o.left);
        }
        return o;
    }
};

// What the F67f window counts of it (one writer, the hook): the steep two-handed ADS calls, those F69h freed the hand in, those held, the stored point's age on the held ones, the limit's
// endings, and the hand-to-grip error (cm) of the held calls the left arm was solved on.
struct Counters {
    uint64_t steep = 0, freed = 0, held = 0, limit = 0, errN = 0;
    double ageSum = 0.0, ageMax = 0.0, errSum = 0.0, errMax = 0.0;
    void add(const Out& o) noexcept {
        if (o.steep) ++steep;
        if (o.freed) ++freed;
        if (o.limit) ++limit;
        if (o.held) {
            ++held;
            ageSum += o.ageMs;
            ageMax = std::max(ageMax, o.ageMs);
        }
    }
    void addError(double cm) noexcept {
        if (!std::isfinite(cm)) return;
        ++errN;
        errSum += cm;
        errMax = std::max(errMax, cm);
    }
};

// The F67f line's ending: empty with no steep call in the window, else the counts. The log's lines end at 1200 bytes (the F67f line is about 600).
inline int formatClause(char* buf, size_t n, const Counters& c) noexcept {
    if (!buf || n == 0) return 0;
    buf[0] = '\0';
    if (!c.steep) return 0;
    const double hn = c.held ? static_cast<double>(c.held) : 1.0, en = c.errN ? static_cast<double>(c.errN) : 1.0;
    return std::snprintf(buf, n,
                         "; steep two-handed ADS (F182c): %llu calls (the sights, the aim pitch 45 deg or more up), F69h freed the off hand in %llu, held on the item by F182c %llu (the pitch over 45 deg), "
                         "its grip point age mean %.0f max %.0f ms, hand-to-grip error of the held calls mean %.2f max %.2f cm, ended by the %.0f s limit %llu",
                         static_cast<unsigned long long>(c.steep), static_cast<unsigned long long>(c.freed), static_cast<unsigned long long>(c.held), c.ageSum / hn, c.ageMax, c.errSum / en, c.errMax,
                         kHoldMaxSec, static_cast<unsigned long long>(c.limit));
}

}  // namespace f182
}  // namespace nofoot
}  // namespace edvr
