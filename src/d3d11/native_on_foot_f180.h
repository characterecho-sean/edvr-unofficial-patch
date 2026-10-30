// F180 (native on foot VR): the left elbow hangs standing in ADS (F180a), the spine straighten dial (F180b, TEMPORARY), and the neck guard's distance (F180c, see native_on_foot_f179.h). Header
// only, no game or D3D11 state, so native_frame_test runs the very functions the pin and the frame thread use. Journal: the arc doc, F180.
//
// The flight edvr_gfx_20261030_10xxxx (build 94f25520, the plasma shotgun, standing in the sights; the eye dump 104137228001): F179a worked (the neck raised +13.1 cm, the eye over the clavicles
// +19.2, ahead of the chest +10.9), but the pilot saw the spine still curved, the neck that cannot be brought under the headset, and the left elbow sitting upright. The dump: the left shoulder
// pad close beside the face, the left arm reaching nearly level to the foregrip with the elbow up. F98a: the elbow's swivel 40.8 deg off the stance's plane, the pole from the stable pole's
// seed in 60 of 60 solves, body-down 0 (F165's body-down blend was crouched only); the chest's lean as written +23.4 deg standing in the sights (unarmed standing about +22): the overall chest
// lean is not the curve, the hunch is in the upper spine and the neck.
//
// ---- F180a: the left elbow hangs standing in ADS
// F165 blends the left elbow toward the body-down pole by the crouched weight x the detach weight. The weight is now the crouched share PLUS F178's standing share, which is the detach weight
// whatever the stance: standing in the sights the elbow hangs as crouched. Crouched (weight 1) it is the earlier value bit for bit; at the hip it is 0; the blend sits in the left hand's
// stable-pole branch, so a gun the left hand does not hold (a pistol) has no solve to blend.
//
// ---- F180b: the spine straighten dial (TEMPORARY)
// experimental.native_on_foot_stand_ads_spine_straighten, 0..1, default 0 (F181: 0.9, the pilot's flown value). The share of the game's ADS hunch of the upper spine and the neck taken back out, standing in the sights: the written
// local rotation of each level (the spine 16, 21, 24 and the neck 34, which is a child of the chest 24) was slerp(animated, reference, dial x the standing share x the detach weight) -- since F182b
// it is one-sided (below): only the forward bend beyond the reference is taken back. The
// reference is the same item's levels at the hip: the local rotations on the last standing hip frames (the detach weight under 0.05, the crouched weight under 0.5), a running average over
// about 0.12 s, kept per item and plan; none yet (the item came out in the sights), the press's first frame in the sights stands in until a hip frame replaces it, as F166's capture does.
// None at all: nothing is straightened, and the log counts it. The levels are written with F166's upper-body machinery (its capture and apply: the same records, slerp and carried joints; the
// plan is its planAdd / planCarry on 16, 21, 24, 34), before the pin, the reach rule, the bend and the arm solve, so they all work on the straightened shoulders (the clavicles and arms are
// carried unchanged against the chest, and the reach rule and the arm IK read the pose from g_pfzAnim after it). F179a's raise and the neck guard are made after it, from the straightened neck
// and chest: a spine that rises asks the legs for less.
#pragma once

#include "native_on_foot_core.h"
#include "native_on_foot_f159.h"
#include "native_on_foot_f165.h"
#include "native_on_foot_f166.h"
#include "native_on_foot_f178.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f180 {

// ---- F180a ----------------------------------------------------------------------------------------------------------------------------------------------------------------------

// The weight of the left elbow's blend toward the body-down pole: F165's crouched share (the crouched weight x the detach weight) plus F178's standing share ((1 - the crouched weight) x
// the detach weight), held to 0..1. The detach weight in either stance; exactly F165's at the crouched weight 1 (the standing share is then 0), and 0 at the hip.
inline double elbowBlendWeight(double postureW, double detachW) noexcept { return unit01(f165::blendWeight(postureW, detachW) + f178::weight(postureW, detachW)); }

// ---- F180b ----------------------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr double kDialMin = 0.0, kDialMax = 1.0;   // the key's range
constexpr int kLevels = 4;                          // the spine 16, 21, 24 and the neck 34
constexpr uint32_t kNeck = 34;                      // the neck joint: a direct child of the chest 24
constexpr uint32_t kLevelJoint[kLevels] = {16, 21, 24, kNeck};
constexpr uint32_t kLevelNext[kLevels] = {21, 24, kNeck, 44};   // the joint each level's bone leads to, for the bone's lean (the head 44 after the neck)
constexpr double kMinWrittenShare = 0.001;          // the health rule: a straighten share under this writes nothing worth counting

// The key's value held to its range; a value that is not a number reads 0 (nothing straightened).
inline double clampDial(double v) noexcept { return !std::isfinite(v) ? 0.0 : v < kDialMin ? kDialMin : v > kDialMax ? kDialMax : v; }

// The share of the hunch taken back: the dial x F178's standing share x the detach weight, 0..1. Exactly 0 at the dial 0, crouched (the standing share 0) and at the hip.
inline double share(double dial, double postureW, double detachW) noexcept { return clampDial(dial) * f178::weight(postureW, detachW); }

// The plan: the spine 16, 21, 24 and the neck 34 as held joints (F166's Plan: parent first, each with its parent's slot), every joint under them carried. The neck is left out (the plan is then the
// spine alone) when it is not a direct child of the chest. why != kOk refuses the dial alone.
inline f166::Plan buildPlan(const uint16_t* parents, uint32_t joints) noexcept {
    f166::Plan p;
    if (!parents || joints == 0 || joints > kPfzMaxJoints || kNeck >= joints || kIkSpine[2] >= joints) return p;
    p.joints = static_cast<uint16_t>(joints);
    if (parents[kIkSpine[1]] != kIkSpine[0] || parents[kIkSpine[2]] != kIkSpine[1]) {
        p.why = f166::kNoSpineChain;
        return p;
    }
    if (parents[kIkSpine[0]] >= joints) {
        p.why = f166::kBadSpineParent;
        return p;
    }
    for (uint32_t k = 0; k < 3; ++k) f166::planAdd(p, parents, kIkSpine[k]);
    if (parents[kNeck] == kIkSpine[2]) f166::planAdd(p, parents, kNeck);
    f166::planCarry(p, parents, joints);
    return p;
}
// True when the plan holds the neck as its last joint.
inline bool planHasNeck(const f166::Plan& p) noexcept { return p.ok() && p.n == kLevels && p.joint[kLevels - 1] == kNeck; }

// ---- F182b: the straighten never adds forward lean --------------------------------------------------------------------------------------------------------------------------------
// The flight edvr_gfx_20261030_115059 (the plasma shotgun, aim pitch +74.6): looking up the game leans the torso BACK (the four bones' lean animated -4.3/-14.7/-26.3/-6.9 deg) while the reference
// (the hip pose before the press, at a level aim) leans forward (+29.7/+28.7/+38.4/+55.6); the slerp toward the reference pulled the torso FORWARD (written +9.2/+6.9/+5.4/+25.7) and the left
// shoulder 22 cm into the view. The rule is now one-sided, per bone, about the body's right axis (the lean the line reports): written = animated - share x max(0, animated - reference). Only
// the forward bend the animation has BEYOND the reference is taken back; a bone already at or behind the reference is written as animated, and the level's other components stay as animated.
// The correction is made as model-space turns about the right axis, level by level: level k is turned by -(c(k) - c(k-1)) degrees, c(k) being what is taken back from bone k (c(-1) = 0), so
// the turns add up along the chain to exactly c(k) at bone k, the joints under a level are carried unchanged against it (F166's carry), and a level whose bone is left as animated while the one
// above was turned undoes that turn there (the lean of bone k is then exactly the animated one).
struct OneSided {
    bool any = false;                  // some bone's forward bend is taken back (a turn is made)
    double cum[kLevels] = {};          // per bone: the lean taken back, deg (0 when left as animated)
    bool left[kLevels] = {};           // per bone: left as animated because it leans back of (or level with) the reference
};
// The take-back of each bone from its animated lean and the reference's, at the straighten share (0..1). A bone whose animated or reference lean is not a number is left alone, uncounted.
inline OneSided oneSided(const double animLean[kLevels], const double refLean[kLevels], double shareV) noexcept {
    OneSided o;
    const double s = std::isfinite(shareV) ? (shareV < 0.0 ? 0.0 : shareV > 1.0 ? 1.0 : shareV) : 0.0;
    for (int k = 0; k < kLevels; ++k) {
        if (!std::isfinite(animLean[k]) || !std::isfinite(refLean[k])) continue;
        const double over = animLean[k] - refLean[k];
        if (over > 0.0 && s > 0.0) {
            o.cum[k] = s * over;
            o.any = true;
        } else if (!(over > 0.0)) {
            o.left[k] = true;
        }
    }
    return o;
}
// The pose with the turns made: out holds the new model matrices of the four levels and of every joint that comes with them (touched, in the order written: the levels parent first, then the
// carried ones), the others not touched. Level k's new matrix is its animated local carried by its parent's new matrix (the animated one for the first level), then turned about the model's
// right axis, in place (its position kept), by -(cum[k] - cum[k-1]) degrees; a level above the first turned level is carried even at a zero turn. False (nothing valid in out) without the
// plan, or on a singular or non-finite matrix.
inline bool applyOneSided(const f166::Plan& pl, const double cum[kLevels], const float (*anim)[16], uint32_t count, float (*out)[16], uint16_t* touched, uint32_t* touchedN) noexcept {
    if (touchedN) *touchedN = 0;
    if (!planHasNeck(pl) || count < pl.joints) return false;
    float delta[f166::kMaxHold][16];   // each level's change: inv(animated) x new, the carry of everything under it
    uint32_t nt = 0;
    double prev = 0.0;
    bool changed = false;              // a level above has been turned: this one is carried by its parent's new matrix
    for (int k = 0; k < kLevels; ++k) {
        const uint32_t j = pl.joint[k], p = pl.parent[k];
        if (j >= count || p >= count || !std::isfinite(cum[k])) return false;
        float b[16];
        if (!changed) {
            std::memcpy(b, anim[j], sizeof(b));
        } else {
            float invP[16], local[16];
            if (!pfzAffineInverse(anim[p], invP)) return false;
            pfzMul(anim[j], invP, local);
            const float* parentNew = pl.parentSlot[k] >= 0 ? out[pl.joint[pl.parentSlot[k]]] : anim[p];
            pfzMul(local, parentNew, b);
        }
        const double turnDeg = -(cum[k] - prev);   // the lean this level adds (negative: back)
        prev = cum[k];
        if (turnDeg != 0.0) {
            // a row-vector frame turned in the model's axes: each axis row v becomes v x Rx(phi), Rx = [[1,0,0],[0,c,s],[0,-s,c]]: (y, z) -> (y c - z s, y s + z c), which adds phi to atan2(z, y)
            const double rad = turnDeg * kIkPi / 180.0, cs = std::cos(rad), sn = std::sin(rad);
            for (int r = 0; r < 3; ++r) {
                const double y = b[r * 4 + 1], z = b[r * 4 + 2];
                b[r * 4 + 1] = static_cast<float>(y * cs - z * sn);
                b[r * 4 + 2] = static_cast<float>(y * sn + z * cs);
            }
            changed = true;
        }
        for (int c = 0; c < 16; ++c)
            if (!std::isfinite(b[c])) return false;
        std::memcpy(out[j], b, sizeof(b));
        float inv[16];
        if (!pfzAffineInverse(anim[j], inv)) return false;
        pfzMul(inv, out[j], delta[k]);
        if (touched) touched[nt] = static_cast<uint16_t>(j);
        ++nt;
    }
    for (uint32_t i = 0; i < pl.carriedN; ++i) {
        const uint32_t j = pl.carried[i];
        if (j >= count) return false;
        pfzMul(anim[j], delta[pl.owner[i]], out[j]);
        if (touched) touched[nt] = static_cast<uint16_t>(j);
        ++nt;
    }
    if (touchedN) *touchedN = nt;
    return true;
}

// The reference's source, for the line: the last standing hip frames, the press's first frame, none.
inline const char* sourceName(int s) noexcept { return s == f166::kStill ? "hip frames" : s == f166::kFirstFrame ? "the press's first frame" : "none"; }

// A bone's lean about the body's right axis, deg, forward (the model's +z) positive: the bone from `from` to `to` (model positions), as F166 reads the chest's.
inline double boneLeanDeg(const double from[3], const double to[3]) noexcept { return f166::leanDeg(from, to, 0.0, 1.0); }

enum Action : uint8_t { kNothing = 0, kCaptureHip, kAverageHip, kCaptureFirst };
struct In {
    double dial = 0.0, postureW = 0.0, detachW = 0.0, dtSec = 0.0;
    uint32_t item = 0, planKey = 0;
};
struct Step {
    Action action = kNothing;
    double alpha = 1.0;      // the capture's weight of this call's pose
    bool cleared = false;    // a reference was dropped this call (another item or plan)
    double share = 0.0;      // the dial x the standing share x the detach weight
    bool atHip = false, standing = false;
};

// The reference and the rules around it (the hook's, one writer). The caller captures (f166::capture with the action's source and alpha, then clears the held pose's parent flag so the root is
// held against the hips as a local) when the action says so, and notes the levels' bones' leans of the pose it captured from.
struct State {
    f166::Held held;
    double refLean[kLevels] = {std::nan(""), std::nan(""), std::nan(""), std::nan("")};
    uint32_t item = 0, planKey = 0;
    bool seen = false;

    void reset() noexcept {
        held.clear();
        for (double& v : refLean) v = std::nan("");
        seen = false;
    }
    // The leans of the captured pose: alpha 1 replaces, below 1 moves toward them (the running average, as the records are).
    void noteLeans(const double lean[kLevels], double alpha) noexcept {
        for (int k = 0; k < kLevels; ++k) {
            if (!std::isfinite(lean[k])) continue;
            if (!std::isfinite(refLean[k]) || alpha >= 1.0) refLean[k] = lean[k];
            else refLean[k] += (lean[k] - refLean[k]) * (alpha > 0.0 ? alpha : 0.0);
        }
    }
    Step step(const In& in) noexcept {
        Step s;
        const double dt = !(in.dtSec > 0.0) ? 0.0 : in.dtSec > f166::kMaxStepSec ? f166::kMaxStepSec : in.dtSec;
        if (!seen || in.item != item || in.planKey != planKey) {
            s.cleared = held.have();
            reset();
            item = in.item;
            planKey = in.planKey;
            seen = true;
        }
        const double posture = unit01(in.postureW), detach = unit01(in.detachW);
        s.atHip = detach < f166::kHipDetachBelow;
        s.standing = posture < f166::kLeaveCrouchWeight;
        if (s.atHip && s.standing) {
            if (!held.have() || held.source != f166::kStill) {
                s.action = kCaptureHip;   // the first hip frame, or the first after the press's first frame stood in: replaces it
                s.alpha = 1.0;
            } else {
                s.action = kAverageHip;
                s.alpha = dt > 0.0 ? 1.0 - std::exp(-dt / f166::kAverageSec) : 0.0;
            }
        } else if (!s.atHip && s.standing && !held.have()) {
            s.action = kCaptureFirst;     // the item came out in the sights: the press's first frame stands in
            s.alpha = 1.0;
        }
        s.share = share(in.dial, in.postureW, in.detachW);
        return s;
    }
};

// ---- the figures ----------------------------------------------------------------------------------------------------------------------------------------------------------------

// What the stage tells the rest of the call (PinCall's part): it ran (wrote), the share, the reference's source, how far the straighten moved the neck 34 and the left shoulder (model axes, m).
struct CallInfo {
    bool valid = false;          // the stage had its plan, pose and gate this call (a standing-in-the-sights call counts)
    bool ran = false;            // the one-sided rule was made (F182b): the share and a reference were there and the turns written; every bone may have been left as animated
    bool failed = false;         // a share and a reference, and the write could not be made
    bool captured = false;
    uint8_t source = f166::kNone;
    double dial = 0.0, share = 0.0, weight = 0.0;   // weight: the share when a turn was made, 0 when every bone was left as animated (the bend yields to a straighten that moved something)
    double animLean[kLevels] = {std::nan(""), std::nan(""), std::nan(""), std::nan("")};
    double neckMove[3] = {}, shoulderMove[3] = {};
    bool movesOk = false;
    bool leftAsIs[kLevels] = {};   // F182b: per bone, left as animated because it leaned back of (or level with) the reference
};

// One standing-in-the-sights call's figures, taken once the body is written: the leans of the four bones as animated, as the reference has them and as finally written (deg; not numbers when
// unmeasured), the neck 34 and the left shoulder from the eye after (right, up, forward, m) and the same before the straighten (after less what it moved), the dial, the share, whether the
// levels were written, the reference's source, and F179a's raise asked (m).
struct Sample {
    double dial = 0.0, share = 0.0, raiseAskedM = 0.0;
    bool wrote = false;
    bool leftAsIs[kLevels] = {};   // F182b: per bone, left as animated (leaned back of the reference)
    uint8_t source = f166::kNone;
    double animLean[kLevels] = {std::nan(""), std::nan(""), std::nan(""), std::nan("")};
    double refLean[kLevels] = {std::nan(""), std::nan(""), std::nan(""), std::nan("")};
    double writtenLean[kLevels] = {std::nan(""), std::nan(""), std::nan(""), std::nan("")};
    double neckAfter[3] = {std::nan(""), std::nan(""), std::nan("")}, neckBefore[3] = {std::nan(""), std::nan(""), std::nan("")};
    double shoulderAfter[3] = {std::nan(""), std::nan(""), std::nan("")}, shoulderBefore[3] = {std::nan(""), std::nan(""), std::nan("")};
};
struct Counters {
    uint32_t frames = 0, wrote = 0, src[3] = {};   // the reference's source: none (0), hip frames (1), the press's first frame (2) -- the f166::Source values
    double dialSum = 0.0, dialMax = 0.0, shareSum = 0.0, raiseSum = 0.0;
    uint32_t leanN[3][kLevels] = {};                // per kind (animated 0, reference 1, written 2) and level
    double leanSum[3][kLevels] = {};
    uint32_t neckN[2] = {}, shoulderN[2] = {};      // per kind: after (0), before (1)
    double neckSum[2][3] = {}, shoulderSum[2][3] = {};
    uint32_t leftN[kLevels] = {};                   // F182b: per bone, the frames left as animated because it leaned back of the reference
    static void addVec(uint32_t& n, double (&sum)[3], const double v[3]) noexcept {
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return;
        ++n;
        for (int c = 0; c < 3; ++c) sum[c] += v[c];
    }
    void add(const Sample& s) noexcept {
        ++frames;
        if (s.wrote) ++wrote;
        ++src[s.source < 3 ? s.source : 0];
        if (std::isfinite(s.dial)) {
            dialSum += s.dial;
            dialMax = s.dial > dialMax ? s.dial : dialMax;
        }
        if (std::isfinite(s.share)) shareSum += s.share;
        if (std::isfinite(s.raiseAskedM)) raiseSum += s.raiseAskedM;
        for (int k = 0; k < kLevels; ++k)
            if (s.leftAsIs[k]) ++leftN[k];
        for (int k = 0; k < kLevels; ++k) {
            const double* v[3] = {&s.animLean[k], &s.refLean[k], &s.writtenLean[k]};
            for (int t = 0; t < 3; ++t)
                if (std::isfinite(*v[t])) {
                    ++leanN[t][k];
                    leanSum[t][k] += *v[t];
                }
        }
        addVec(neckN[0], neckSum[0], s.neckAfter);
        addVec(neckN[1], neckSum[1], s.neckBefore);
        addVec(shoulderN[0], shoulderSum[0], s.shoulderAfter);
        addVec(shoulderN[1], shoulderSum[1], s.shoulderBefore);
    }
    void merge(const Counters& o) noexcept {
        frames += o.frames; wrote += o.wrote;
        for (int i = 0; i < 3; ++i) src[i] += o.src[i];
        dialSum += o.dialSum;
        dialMax = o.dialMax > dialMax ? o.dialMax : dialMax;
        shareSum += o.shareSum; raiseSum += o.raiseSum;
        for (int k = 0; k < kLevels; ++k) leftN[k] += o.leftN[k];
        for (int t = 0; t < 3; ++t)
            for (int k = 0; k < kLevels; ++k) {
                leanN[t][k] += o.leanN[t][k];
                leanSum[t][k] += o.leanSum[t][k];
            }
        for (int i = 0; i < 2; ++i) {
            neckN[i] += o.neckN[i]; shoulderN[i] += o.shoulderN[i];
            for (int c = 0; c < 3; ++c) {
                neckSum[i][c] += o.neckSum[i][c];
                shoulderSum[i][c] += o.shoulderSum[i][c];
            }
        }
    }
};

// ---- the line -------------------------------------------------------------------------------------------------------------------------------------------------------------------

// The window's line, printed whenever a frame stood in the sights with a two-handed gun (the dial at 0 included: the before picture): the item; those frames; the dial's mean and largest value;
// the share's mean; the frames the levels were written in; the reference's source (hip frames / the press's first frame / none, in frames); the four bones' lean about the body's right axis
// (forward positive; spine 16, 21, chest 24, neck 34), animated / reference / written (deg, means over the frames each was measured on); the neck 34 and the left shoulder from the eye after
// the writes and before the straighten (right/up/forward, cm); F179a's leg raise asked (cm); and (F182b) per bone the frames it was left as animated because it leaned back of the reference
// ("written in" counts the frames the one-sided rule was made in, every bone left as animated included). The log's lines end at 1200 bytes.
inline int formatSummary(char* buf, size_t n, uint32_t seconds, const char* item, const Counters& c) noexcept {
    const double an = c.frames ? static_cast<double>(c.frames) : 1.0;
    const auto mean = [](double sum, uint32_t k) noexcept { return k ? sum / static_cast<double>(k) : 0.0; };
    const auto lean = [&](int t, int k) noexcept { return mean(c.leanSum[t][k], c.leanN[t][k]); };
    const auto v = [&](const double (*sum)[3], const uint32_t* cnt, int i, int axis) noexcept { return mean(sum[i][axis], cnt[i]) * 100.0; };
    return std::snprintf(buf, n,
                         "native on foot standing ADS spine (F180b, TEMPORARY) %u s: item %s; standing in the sights %u frames; the dial experimental.native_on_foot_stand_ads_spine_straighten mean %.2f "
                         "(largest %.2f); the straighten share mean %.2f; written in %u frames; the reference: hip frames %u, the press's first frame %u, none %u; lean about the body's right axis "
                         "(deg, forward positive) animated/reference/written: spine 16 %+.1f/%+.1f/%+.1f, spine 21 %+.1f/%+.1f/%+.1f, chest 24 %+.1f/%+.1f/%+.1f, neck 34 %+.1f/%+.1f/%+.1f; "
                         "the neck from the eye (right/up/forward, cm) after %+.1f/%+.1f/%+.1f, before %+.1f/%+.1f/%+.1f; the left shoulder from the eye after %+.1f/%+.1f/%+.1f, before "
                         "%+.1f/%+.1f/%+.1f; F179a's leg raise asked mean %+.1f cm; left as animated, leaning back of the reference (frames): spine 16 %u, spine 21 %u, chest 24 %u, neck 34 %u",
                         seconds, item ? item : "none", c.frames, c.dialSum / an, c.dialMax, c.shareSum / an, c.wrote, c.src[f166::kStill], c.src[f166::kFirstFrame], c.src[f166::kNone],
                         lean(0, 0), lean(1, 0), lean(2, 0), lean(0, 1), lean(1, 1), lean(2, 1), lean(0, 2), lean(1, 2), lean(2, 2), lean(0, 3), lean(1, 3), lean(2, 3),
                         v(c.neckSum, c.neckN, 0, 0), v(c.neckSum, c.neckN, 0, 1), v(c.neckSum, c.neckN, 0, 2), v(c.neckSum, c.neckN, 1, 0), v(c.neckSum, c.neckN, 1, 1), v(c.neckSum, c.neckN, 1, 2),
                         v(c.shoulderSum, c.shoulderN, 0, 0), v(c.shoulderSum, c.shoulderN, 0, 1), v(c.shoulderSum, c.shoulderN, 0, 2), v(c.shoulderSum, c.shoulderN, 1, 0),
                         v(c.shoulderSum, c.shoulderN, 1, 1), v(c.shoulderSum, c.shoulderN, 1, 2), c.raiseSum / an * 100.0, c.leftN[0], c.leftN[1], c.leftN[2], c.leftN[3]);
}

}  // namespace f180
}  // namespace nofoot
}  // namespace edvr
