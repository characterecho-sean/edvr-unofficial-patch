// F192 (native on foot VR): the left elbow held down through a weapon draw. Header only, no game or D3D11 state, so native_frame_test runs the very functions the arm solve uses.
// Journal: the arc doc, F192. Behaviour, no key.
//
// The pilot: "when I switch between rifles, the left elbow starts at the awkward position and then moves down". The flight edvr_gfx_20261031_080600 (v0.22.0-rc.4-8-g58d4f8c8): at a
// switch the native epoch changes (the pole seed's and F69h's identity), and the two-handed decision takes over in the following 11 frames. From the first stable-pole frame the left
// hip elbow's IK has no feasible swivel while the game's draw animation carries the hand across the body, so ikStableSwivel takes its minimum-violation candidate: the elbow at its
// outward bound, or parked on the E.y <= S.y ceiling. F181's hip cap is measured against the game's ANIMATED arm, which is itself out of place during the draw, so the cap barely bites
// (capped 1 of 36, the cut mean 6.8 deg) until the animation settles; in the steady state it cuts 27 to 41 deg (capped 90 of 90). The swap of the shotgun for the plasma assault rifle at
// 08:11:16.726 began about 0.9 s before Status.json named the item (17.616); the shotgun swap at 08:11:07.46 had the elbow at the ceiling 22 cm out, back to -22.6 cm by 09.5.
//
// The draw-hold weight dh, per native epoch (the identity the pole is seeded by):
//   1.0 from the epoch's first call with a two-handed gun armed;
//   held until the Status.json item flip plus kAfterFlipS, or kTimeoutS from the epoch's start, whichever comes first;
//   then eased to 0 over kRampS.
// While dh > 0 the left elbow's pole is blended toward the body-down pole (the weight is the larger of F180's elbow weight and dh) so the body-down pole holds from the first stable
// frame, and F181's cap share is scaled by (1 - dh): a cap measured against the animation that is out of place would drag the elbow toward it.
// Scope: two-handed guns only. A pistol and a tool have no native epoch (it needs the primary slot), so they are not held; at the arm solve a pistol's authored support (its own body-down pole
// every frame) and a support-cup tool take no weight whatever the state says; the holster (nothing armed, no stable pole) is unchanged; a draw from the fists gets the hold. A second switch inside the ramp restarts the hold at 1.
#pragma once

#include "native_on_foot_core.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f192 {

constexpr double kAfterFlipS = 0.5;   // the hold lasts this long past the Status.json item flip
constexpr double kTimeoutS = 2.0;     // or this long from the epoch's start, whichever comes first
constexpr double kRampS = 0.3;        // then falls to 0 over this

// The hold is made for a two-handed gun on a native epoch: armed, and a native epoch identity, which only a selection in the primary weapon slot has (NativePrimarySelectionState: the rifles, the
// shotguns, the launchers; never a pistol or a tool). The item's kind is deliberately not asked here: it comes from Status.json's name for the item, which lags the draw by 0.3 to 0.9 s, so
// at a switch it is still the PREVIOUS item's (a pistol or a tool before a rifle) and would hold the hold back through the early draw it is made for.
inline bool eligible(bool armed, bool nativeEpoch) noexcept { return armed && nativeEpoch; }

// At the arm solve the weight is used only for the left hand's stable pole of a two-handed gun: a pistol's authored support and a support-cup tool take none, whatever the state says (the
// support-cup tool takes the stable-pole block, as F165's blend does).
inline double usedWeight(double dh, bool supportCup, bool authoredPistol) noexcept {
    return (supportCup || authoredPistol || !std::isfinite(dh)) ? 0.0 : dh < 0.0 ? 0.0 : dh > 1.0 ? 1.0 : dh;
}

// The pole blend's weight: F180's elbow weight (the sights) or the draw hold, whichever is larger.
inline double blendWeight(double elbowW, double dh) noexcept {
    const double a = std::isfinite(elbowW) ? (elbowW < 0.0 ? 0.0 : elbowW > 1.0 ? 1.0 : elbowW) : 0.0;
    const double b = std::isfinite(dh) ? (dh < 0.0 ? 0.0 : dh > 1.0 ? 1.0 : dh) : 0.0;
    return a > b ? a : b;
}

// F181's hip share, scaled by (1 - dh).
inline double capShare(double hipShare, double dh) noexcept {
    const double s = std::isfinite(hipShare) ? (hipShare < 0.0 ? 0.0 : hipShare > 1.0 ? 1.0 : hipShare) : 0.0;
    const double d = std::isfinite(dh) ? (dh < 0.0 ? 0.0 : dh > 1.0 ? 1.0 : dh) : 0.0;
    return s * (1.0 - d);
}

enum Phase : int { kIdle = 0, kHeld = 1, kRamp = 2, kDone = 3 };
enum Why : int { kWhyNone = 0, kWhyFlip = 1, kWhyTimeout = 2 };
enum Cut : int { kCutNone = 0, kCutGone = 1, kCutReplaced = 2 };   // gone: nothing armed (or no longer eligible); replaced: another epoch began

inline const char* whyName(int why) noexcept {
    return why == kWhyFlip ? "the item flip in Status.json and 0.5 s" : why == kWhyTimeout ? "the 2 s timeout" : "not ended";
}
inline const char* cutName(int cut) noexcept { return cut == kCutGone ? "cut short: the gun was put away" : cut == kCutReplaced ? "cut short: another draw began" : "ran its course"; }

// One draw's account, for the line: the epoch, how long the hold stood (from the epoch's start to the end of the hold, or to the cut), why it ended, and when the ramp was done (ms from the
// epoch's start; not a number when the draw was cut before it was).
struct Line {
    uint64_t epoch = 0;
    double heldMs = 0.0, rampDoneMs = std::nan("");
    int why = kWhyNone, cut = kCutNone;
};

struct Out {
    double weight = 0.0;   // dh
    bool begun = false;    // an epoch's hold began in this call
    bool lineDue = false;  // the draw's account is complete: `line` is to be logged
    Line line;
};

struct DrawHold {
    int phase = kIdle;
    uint64_t epoch = 0;
    double startS = 0.0, flipS = std::nan(""), endS = std::nan("");
    int why = kWhyNone;

    void reset() noexcept { *this = DrawHold{}; }

    // epochOk: the call is eligible (above) and `epoch` names its native epoch; flipped: Status.json has named the item since the epoch began (the held item's serial moved); tS: seconds, any
    // monotonic clock. Eligible false: the hold is over (an unfinished draw gets its line, cut), nothing is held.
    Out step(bool epochOk, uint64_t epochId, bool flipped, double tS) noexcept {
        Out o;
        if (!epochOk || !std::isfinite(tS)) {
            if (phase == kHeld || phase == kRamp) {
                o.lineDue = true;
                o.line = account(tS, kCutGone);
            }
            reset();
            return o;
        }
        if (phase == kIdle || epochId != epoch) {
            if (phase == kHeld || phase == kRamp) {
                o.lineDue = true;
                o.line = account(tS, kCutReplaced);
            }
            phase = kHeld;
            epoch = epochId;
            startS = tS;
            flipS = std::nan("");
            endS = std::nan("");
            why = kWhyNone;
            o.begun = true;
            o.weight = 1.0;
            return o;
        }
        if (phase == kHeld) {
            if (flipped && !std::isfinite(flipS)) flipS = tS;
            double end = startS + kTimeoutS;
            int w = kWhyTimeout;
            if (std::isfinite(flipS) && flipS + kAfterFlipS <= end) {
                end = flipS + kAfterFlipS;
                w = kWhyFlip;
            }
            if (!(tS >= end)) {
                o.weight = 1.0;
                return o;
            }
            phase = kRamp;
            endS = end;
            why = w;
        }
        if (phase == kRamp) {
            const double t = tS - endS;
            if (t >= kRampS) {
                phase = kDone;
                o.weight = 0.0;
                o.lineDue = true;
                o.line = account(tS, kCutNone);
                return o;
            }
            o.weight = t > 0.0 ? 1.0 - t / kRampS : 1.0;
            return o;
        }
        return o;   // done: 0 until another epoch
    }

private:
    Line account(double tS, int cut) const noexcept {
        Line l;
        l.epoch = epoch;
        l.cut = cut;
        l.why = why;
        if (std::isfinite(endS)) {
            l.heldMs = (endS - startS) * 1e3;
            if (cut == kCutNone) l.rampDoneMs = (endS + kRampS - startS) * 1e3;
        } else {
            l.heldMs = std::isfinite(tS) ? (tS - startS) * 1e3 : 0.0;
        }
        return l;
    }
};

// ---- the figures ----------------------------------------------------------------------------------------------------------------------------------------------------------------------

// The F98a window's counters (F98Left's): the left hand's stable-pole solves with a two-handed gun, those held (dh above 0), those the blend was applied to, the weight's sum, and the
// draws begun in the window.
struct Counters {
    uint32_t solves = 0, held = 0, applied = 0, begun = 0;
    double wSum = 0.0;
    void add(double dh, bool wasApplied) noexcept {
        ++solves;
        if (dh > 0.0) {
            ++held;
            wSum += dh;
            if (wasApplied) ++applied;
        }
    }
    void merge(const Counters& o) noexcept {
        solves += o.solves; held += o.held; applied += o.applied; begun += o.begun;
        wSum += o.wSum;
    }
};

// The F159 line's ending: "; draw hold (F192): held H of N solves (applied K, mean weight W, draws begun B)". Appended to the line the caller built (cap - 1 bytes at most, nothing when the
// buffer has no room).
inline int appendSummary(char* buf, size_t cap, const Counters& c) noexcept {
    if (!buf || cap == 0 || !c.solves) return 0;
    const size_t used = std::strlen(buf);
    if (used + 1 >= cap) return 0;
    return std::snprintf(buf + used, cap - used, "; draw hold (F192): held %u of %u solves (applied %u, mean weight %.2f, draws begun %u)", c.held, c.solves, c.applied,
                         c.held ? c.wSum / static_cast<double>(c.held) : 0.0, c.begun);
}

// One draw's line: "native on foot left elbow draw hold (F192): epoch N, held X ms (why), ramp done at Y ms after the epoch began" (a cut draw: "ramp not done").
inline int formatLine(char* buf, size_t n, const Line& l) noexcept {
    char done[48];
    if (std::isfinite(l.rampDoneMs)) std::snprintf(done, sizeof(done), "ramp done at %.0f ms after the epoch began", l.rampDoneMs);
    else std::snprintf(done, sizeof(done), "ramp not done");
    return std::snprintf(buf, n, "native on foot left elbow draw hold (F192): epoch %llu, held %.0f ms (%s; %s), %s",
                         static_cast<unsigned long long>(l.epoch), l.heldMs, l.why != kWhyNone ? whyName(l.why) : "not ended", cutName(l.cut), done);
}

}  // namespace f192
}  // namespace nofoot
}  // namespace edvr
