// F166 (native on foot VR): the upper body held at its still crouched-ADS pose while walking crouched in the sights. Header only, no game or D3D11 state, so native_frame_test runs the
// very functions the Animate post-hook and the frame thread use. Journal: the arc doc, F166.
//
// The pilot, after F165: the left arm still bends awkwardly while crouching in the sights and moving, and looks right sitting still. The flights edvr_gfx_20261028_135901 and
// edvr_gfx_20261028_172556 (build 60ea7dc3, F165 active) put the cause in the game's walk animation of the upper body, not in the arm: crouched in the sights and still, the animated
// chest leans +47 to +49 deg and the neck stands 0.85 m; crouch-walking, the chest leans +70 to +75 deg and the neck 1.04 to 1.07 m, and the eye sits 40.0 cm ahead of the chest still
// against 30.7 cm moving. The left shoulder travels (F76 event 13, the walk's onset) 14.8 cm to the right, 3.4 cm up and 9.3 cm forward of the model in 16 frames with the camera
// still. The gun is locked to the eye, so the arm has to bend to follow the moving shoulder: the left elbow plane goes 30 to 35 deg off the stance's plane still to 40 to 48 deg
// moving (max 51 to 59), the solved elbow from 7 to 11.7 cm under the shoulder to 22 to 25 cm, the upper arm from 22.4 cm to 16.5 to 19 cm of the eye.
//
// The hold: while crouched (F163's weight), in the sights (the ADS detach weight) and moving, the spine from above the pelvis to the chest (16, 21, 24), both clavicles and both upper
// arms take their local transforms (against their parents) from the pose the body had crouched in the sights and still. Everything below a held joint comes with it unchanged against it
// (the neck and head, the forearms and hands, the fingers), so the arm solve, the pin and the elbow steps work on the held torso; the neck and the head are not held (the pin owns them),
// nor the arms below the shoulder (the arm solve owns them), nor the hips and the legs (they keep walking).
//
// The weight is the crouched weight x the detach weight x a moving ramp (0.15 s in, 0.2 s out). At weight 0 (standing, at the hip, unarmed, still) nothing is touched, bit for bit.
// The pose is captured while still (the speed under 0.1 m/s for 0.25 s, then averaged over about 0.12 s), crouched and in the sights (both weights 0.9 or more), and cleared by a new
// press, a weapon change, a skeleton change or leaving the crouch. A pilot who starts moving straight away has no still pose: the press's first crouched in-the-sights frame stands in
// until a still one replaces it. Rotations are slerped, translations lerped.
#pragma once

#include "native_on_foot_core.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f166 {

constexpr double kCaptureWeight = 0.9;         // the crouched and the detach weights from which a pose is captured
constexpr double kLeaveCrouchWeight = 0.5;     // under this crouched weight the crouch is left and the held pose cleared (F161's crouched threshold)
constexpr double kHipDetachBelow = 0.05;       // under this detach weight the call is at the hip: the press is over, the held pose cleared
constexpr double kStillMps = 0.10;             // the AMC's low-passed horizontal speed under which the body is still for the capture (moving is kPinWalkMps and over, as F95a)
constexpr double kStillSec = 0.25;             // still for this long before the still pose is taken
constexpr double kAverageSec = 0.12;           // the still pose is a running average of the animated one over about this time
constexpr double kRampInSec = 0.15, kRampOutSec = 0.20;   // the moving ramp
constexpr double kMaxStepSec = 0.10;           // a call after a longer gap steps by this much
constexpr double kHeldHalf = 0.5;              // a frame is held from this weight
constexpr double kLeanTolDeg = 10.0;           // the written lean is "held" within this of the held pose's, the animated one "moved" from this far
constexpr uint32_t kMinSecondFrames = 10;      // fewer crouched-in-the-sights moving frames than this are not a second of it
constexpr uint32_t kMaxHold = 8;

// ---- the plan: which joints are held, and which come with them ---------------------------------------------------------------------------------------------------------------

enum Why : uint32_t { kOk = 0, kBadInput = 1, kNoSpineChain = 2, kBadSpineParent = 3, kBadTree = 4 };
inline const char* whyText(uint32_t why) noexcept {
    switch (why) {
        case kOk: return "ok";
        case kBadInput: return "no joints, or a joint out of range";
        case kNoSpineChain: return "the spine 16, 21, 24 is not a chain of direct parents";
        case kBadSpineParent: return "the spine root 16 has no parent in range";
        case kBadTree: return "a parent out of range or a cycle";
        default: return "?";
    }
}

// The held joints, parent first (the spine 16, 21, 24, then each side's clavicle and upper arm), each with its parent and the slot of that parent among the held joints (-1: outside,
// the parent's model matrix is the animated one), and for every other joint under a held one the slot of its nearest held ancestor (it comes with that joint unchanged against it).
struct Plan {
    uint32_t why = kBadInput;
    uint16_t joints = 0;
    uint8_t n = 0;
    uint16_t joint[kMaxHold] = {};
    uint16_t parent[kMaxHold] = {};
    int8_t parentSlot[kMaxHold] = {};
    uint8_t arms = 0;           // bit 0 the left clavicle and upper arm, bit 1 the right
    uint16_t carriedN = 0;
    uint16_t carried[kPfzMaxJoints] = {};
    uint8_t owner[kPfzMaxJoints] = {};   // per carried entry: the held slot it comes with
    bool ok() const noexcept { return why == kOk; }
};

// parents: the pose's parent array (0xFFFF the root); clav and upper: the arm plan's clavicle and upper-arm joints per side (0xFFFF when the side's arm plan was refused). A side whose
// clavicle is not a direct child of the chest 24, or whose upper arm is not a direct child of the clavicle, is left out; the spine alone is required.
inline Plan buildPlan(const uint16_t* parents, uint32_t joints, const uint32_t clav[2], const uint32_t upper[2]) noexcept {
    Plan p;
    if (!parents || joints == 0 || joints > kPfzMaxJoints || kIkSpine[2] >= joints) return p;
    p.joints = static_cast<uint16_t>(joints);
    if (parents[kIkSpine[1]] != kIkSpine[0] || parents[kIkSpine[2]] != kIkSpine[1]) {
        p.why = kNoSpineChain;
        return p;
    }
    if (parents[kIkSpine[0]] >= joints) {
        p.why = kBadSpineParent;
        return p;
    }
    auto add = [&](uint32_t j) noexcept {
        p.joint[p.n] = static_cast<uint16_t>(j);
        p.parent[p.n] = parents[j];
        p.parentSlot[p.n] = -1;
        for (uint32_t k = 0; k < p.n; ++k)
            if (p.joint[k] == parents[j]) p.parentSlot[p.n] = static_cast<int8_t>(k);
        ++p.n;
    };
    for (uint32_t k = 0; k < 3; ++k) add(kIkSpine[k]);
    for (int side = 0; side < 2; ++side) {
        const uint32_t c = clav[side], u = upper[side];
        if (c >= joints || u >= joints || parents[c] != kIkSpine[2] || parents[u] != c) continue;
        add(c);
        add(u);
        p.arms |= static_cast<uint8_t>(1u << side);
    }
    // Every other joint under a held one comes with its nearest held ancestor.
    for (uint32_t j = 0; j < joints; ++j) {
        bool isHeld = false;
        for (uint32_t k = 0; k < p.n; ++k) isHeld = isHeld || p.joint[k] == j;
        if (isHeld) continue;
        int slot = -1;
        uint32_t a = parents[j], steps = 0;
        while (a != 0xFFFFu) {
            if (a >= joints || ++steps > joints) {
                p.why = kBadTree;
                return p;
            }
            for (uint32_t k = 0; k < p.n && slot < 0; ++k)
                if (p.joint[k] == a) slot = static_cast<int>(k);
            if (slot >= 0) break;
            a = parents[a];
        }
        if (slot < 0) continue;
        p.carried[p.carriedN] = static_cast<uint16_t>(j);
        p.owner[p.carriedN] = static_cast<uint8_t>(slot);
        ++p.carriedN;
    }
    p.why = kOk;
    return p;
}

// ---- a local transform as a position and a rotation --------------------------------------------------------------------------------------------------------------------------

struct Rec {
    double p[3] = {};
    double q[4] = {0.0, 0.0, 0.0, 1.0};   // x, y, z, w
};

// The record of a local matrix (a row-vector 4x4: the rotation in the upper 3x3, the position in row 3). False when the 3x3 is not a rotation (a reflection, or a scale far from 1),
// which a pose of position-and-rotation records never gives.
inline bool recOf(const float m[16], Rec* r) noexcept {
    if (!pfzAffineElementsFinite(m)) return false;
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(det > 0.97 && det < 1.03)) return false;
    float q[4];
    pfzQuatFromMatrix(m, q);
    for (int k = 0; k < 4; ++k) r->q[k] = q[k];
    for (int k = 0; k < 3; ++k) r->p[k] = m[12 + k];
    return std::isfinite(r->q[0]) && std::isfinite(r->q[1]) && std::isfinite(r->q[2]) && std::isfinite(r->q[3]);
}
// The local matrix of a record.
inline void matOf(const Rec& r, float out[16]) noexcept {
    const float rec[8] = {static_cast<float>(r.p[0]), static_cast<float>(r.p[1]), static_cast<float>(r.p[2]), 1.0f,
                          static_cast<float>(r.q[0]), static_cast<float>(r.q[1]), static_cast<float>(r.q[2]), static_cast<float>(r.q[3])};
    pfzLocalMatrix(rec, out);
}
// Spherical interpolation on the short arc, a to b by t (0..1); nearly parallel quaternions take the normalised lerp.
inline void slerpQuat(const double a[4], const double b[4], double t, double out[4]) noexcept {
    double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const double sgn = d < 0.0 ? -1.0 : 1.0;
    d = std::fabs(d);
    double ka = 1.0 - t, kb = t;
    if (d < 0.9995) {
        const double th = std::acos(d < 1.0 ? d : 1.0), sn = std::sin(th);
        ka = std::sin((1.0 - t) * th) / sn;
        kb = std::sin(t * th) / sn;
    }
    double n = 0.0;
    for (int i = 0; i < 4; ++i) {
        out[i] = ka * a[i] + kb * sgn * b[i];
        n += out[i] * out[i];
    }
    n = n > 1e-18 ? 1.0 / std::sqrt(n) : 1.0;
    for (int i = 0; i < 4; ++i) out[i] *= n;
}
// a to b by t: the rotation slerped, the position lerped. t <= 0 (or not a number) is a, t >= 1 is b.
inline Rec blendRec(const Rec& a, const Rec& b, double t) noexcept {
    if (!(t > 0.0)) return a;
    if (t >= 1.0) return b;
    Rec o;
    for (int i = 0; i < 3; ++i) o.p[i] = a.p[i] + (b.p[i] - a.p[i]) * t;
    slerpQuat(a.q, b.q, t, o.q);
    return o;
}
// The angle between two rotations (deg), 0..180.
inline double angleDeg(const Rec& a, const Rec& b) noexcept {
    const double d = std::fabs(a.q[0] * b.q[0] + a.q[1] * b.q[1] + a.q[2] * b.q[2] + a.q[3] * b.q[3]);
    return 2.0 * std::acos(d < 1.0 ? d : 1.0) * 180.0 / 3.14159265358979323846;
}

// ---- the held pose and the state around it -----------------------------------------------------------------------------------------------------------------------------

enum Source : uint8_t { kNone = 0, kStill = 1, kFirstFrame = 2 };
inline const char* sourceName(int s) noexcept { return s == kStill ? "a still frame" : s == kFirstFrame ? "the press's first frame" : "none"; }

struct Held {
    Source source = kNone;
    Rec rec[kMaxHold] = {};
    double leanDeg = std::nan("");   // the chest's lean of the pose the record was taken from (set by the caller, who has the heading)
    bool have() const noexcept { return source != kNone; }
    void clear() noexcept { *this = Held{}; }
    // The lean of the pose: the first capture's, then the running average's (alpha 1: replaces).
    void noteLean(double lean, double alpha) noexcept {
        if (!std::isfinite(lean)) return;
        if (!std::isfinite(leanDeg) || alpha >= 1.0) leanDeg = lean;
        else leanDeg += (lean - leanDeg) * (alpha > 0.0 ? alpha : 0.0);
    }
};

// The held joints' local records from a pose (the model matrices): joint against its parent. Alpha 1 replaces the held pose; below 1 it moves toward this pose by alpha (the running
// average). False (the held pose untouched) when a parent is singular or a local is not a rotation.
inline bool capture(const Plan& pl, const float (*anim)[16], uint32_t count, Source source, double alpha, Held* held) noexcept {
    if (!pl.ok() || !held || count < pl.joints || source == kNone) return false;
    Rec now[kMaxHold];
    for (uint32_t k = 0; k < pl.n; ++k) {
        const uint32_t j = pl.joint[k], p = pl.parent[k];
        if (j >= count || p >= count) return false;
        float inv[16], local[16];
        if (!pfzAffineInverse(anim[p], inv)) return false;
        pfzMul(anim[j], inv, local);
        if (!recOf(local, &now[k])) return false;
    }
    const bool fresh = !held->have() || alpha >= 1.0 || held->source != source;
    for (uint32_t k = 0; k < pl.n; ++k) held->rec[k] = fresh ? now[k] : blendRec(held->rec[k], now[k], alpha);
    held->source = source;
    return true;
}

// The pose with the hold at weight w: out holds the new model matrices of the held joints and of every joint that comes with them (touched, in the order written: the held ones parent
// first, then the carried ones), the others not touched. A held joint's new local is the animated local blended toward the held one by w; the matrix is that local against its parent's
// new matrix. A joint under it keeps its animated local against it: its new matrix is its animated one carried by its owner's change. False (nothing valid in out) at w <= 0, without a
// held pose, or on a singular matrix.
inline bool apply(const Plan& pl, const Held& held, double w, const float (*anim)[16], uint32_t count, float (*out)[16], uint16_t* touched, uint32_t* touchedN) noexcept {
    if (touchedN) *touchedN = 0;
    if (!pl.ok() || !held.have() || !(w > 0.0) || count < pl.joints) return false;
    const double wc = w > 1.0 ? 1.0 : w;
    float delta[kMaxHold][16];   // each held joint's change: inv(animated) x new, the carry of everything under it
    uint32_t nt = 0;
    for (uint32_t k = 0; k < pl.n; ++k) {
        const uint32_t j = pl.joint[k], p = pl.parent[k];
        if (j >= count || p >= count) return false;
        float invP[16], local[16], mixed[16], inv[16];
        if (!pfzAffineInverse(anim[p], invP)) return false;
        pfzMul(anim[j], invP, local);
        Rec ar;
        if (!recOf(local, &ar)) return false;
        matOf(blendRec(ar, held.rec[k], wc), mixed);
        const float* parentNew = pl.parentSlot[k] >= 0 ? out[pl.joint[pl.parentSlot[k]]] : anim[p];
        pfzMul(mixed, parentNew, out[j]);
        if (!pfzAffineInverse(anim[j], inv)) return false;
        pfzMul(inv, out[j], delta[k]);
        for (int c = 0; c < 16; ++c)
            if (!std::isfinite(out[j][c])) return false;
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

// ---- the weight, the moving ramp and the capture rules ------------------------------------------------------------------------------------------------------------------

// The hold's weight: the crouched weight x the detach weight x the moving ramp, each held to 0..1; a figure that is not a number reads 0.
inline double holdWeight(double postureW, double detachW, double ramp) noexcept { return unit01(postureW) * unit01(detachW) * unit01(ramp); }

// Moving as F95a files it (the AMC's speed from the walk threshold up, or its vertical speed past the air threshold); a speed that is not a number is neither moving nor still.
inline bool movingNow(double speed, double vy) noexcept { return std::isfinite(speed) && std::isfinite(vy) && !(speed < kPinWalkMps && std::fabs(vy) <= kPinAirMps); }
inline bool stillNow(double speed, double vy) noexcept { return std::isfinite(speed) && std::isfinite(vy) && speed < kStillMps && std::fabs(vy) <= kPinAirMps; }

struct In {
    double postureW = 0.0, detachW = 0.0, speed = 0.0, vy = 0.0, dtSec = 0.0;
    uint32_t item = 0, planKey = 0;
};
enum Action : uint8_t { kNothing = 0, kCaptureFirst, kCaptureStill, kAverageStill };
struct Step {
    Action action = kNothing;
    double alpha = 1.0;        // the capture's weight of this call's pose
    bool cleared = false;      // a held pose was dropped this call
    bool moving = false, still = false, inSights = false;
    double ramp = 0.0, weight = 0.0;
};

struct State {
    Held held;
    double ramp = 0.0, stillSec = 0.0;
    uint32_t item = 0, planKey = 0;
    bool seen = false;

    // Back to nothing held and no ramp (the arms stood down, the tree refused).
    void reset() noexcept {
        held.clear();
        ramp = stillSec = 0.0;
        seen = false;
    }
    // One call. The caller captures (capture(), with the action's source and alpha) when the action says so, before it asks for the weight's pose; the weight is returned.
    Step step(const In& in) noexcept {
        Step s;
        const double dt = !(in.dtSec > 0.0) ? 0.0 : in.dtSec > kMaxStepSec ? kMaxStepSec : in.dtSec;
        if (!seen || in.item != item || in.planKey != planKey) {
            s.cleared = held.have();
            held.clear();
            ramp = stillSec = 0.0;
            item = in.item;
            planKey = in.planKey;
            seen = true;
        }
        const double posture = unit01(in.postureW), detach = unit01(in.detachW);
        if ((detach < kHipDetachBelow || posture < kLeaveCrouchWeight) && held.have()) {
            held.clear();   // a new press, or the crouch left
            s.cleared = true;
        }
        s.moving = movingNow(in.speed, in.vy);
        s.still = stillNow(in.speed, in.vy);
        s.inSights = posture >= kCaptureWeight && detach >= kCaptureWeight;
        // the moving ramp: in over kRampInSec while moving in the sights, out over kRampOutSec otherwise
        const bool want = s.moving && detach >= kHipDetachBelow;
        const double rate = want ? (dt / kRampInSec) : -(dt / kRampOutSec);
        ramp += rate;
        ramp = ramp < 0.0 ? 0.0 : ramp > 1.0 ? 1.0 : ramp;
        stillSec = (s.still && s.inSights) ? stillSec + dt : 0.0;
        if (s.inSights) {
            if (!held.have()) {
                s.action = kCaptureFirst;
                s.alpha = 1.0;
            } else if (stillSec >= kStillSec) {
                if (held.source != kStill) {
                    s.action = kCaptureStill;   // the first still pose of the press replaces the first frame's
                    s.alpha = 1.0;
                } else {
                    s.action = kAverageStill;
                    s.alpha = dt > 0.0 ? 1.0 - std::exp(-dt / kAverageSec) : 0.0;
                }
            }
        }
        s.ramp = ramp;
        s.weight = held.have() || s.action != kNothing ? holdWeight(in.postureW, in.detachW, ramp) : 0.0;
        return s;
    }
};

// ---- the figures ----------------------------------------------------------------------------------------------------------------------------------------------------------

// The chest's lean, forward positive (deg): the chest-to-neck vector's part along the heading (hx, hz; any length over 0.5) against its height.
inline double leanDeg(const double chest[3], const double neck[3], double hx, double hz) noexcept {
    const double hn = std::sqrt(hx * hx + hz * hz);
    if (!(hn > 0.5)) return std::nan("");
    const double v[3] = {neck[0] - chest[0], neck[1] - chest[1], neck[2] - chest[2]};
    const double r = std::atan2((v[0] * hx + v[2] * hz) / hn, v[1]) * 180.0 / 3.14159265358979323846;
    return std::isfinite(r) ? r : std::nan("");
}
// A point's offset from the eye in the heading's axes (right, up, forward; m), the point less the eye; not numbers without a heading.
inline void offsetFromEye(const double point[3], const double eye[3], double hx, double hz, double out[3]) noexcept {
    const double hn = std::sqrt(hx * hx + hz * hz);
    if (!(hn > 0.5)) {
        out[0] = out[1] = out[2] = std::nan("");
        return;
    }
    const double h[3] = {hx / hn, 0.0, hz / hn}, d[3] = {point[0] - eye[0], point[1] - eye[1], point[2] - eye[2]};
    headingAxes(h, d, out);
}

// What the hold stage tells the rest of the call (PinCall's part).
struct CallInfo {
    bool preValid = false;     // the chest 24, neck 34 and head 44 as the game animated them were kept (preM)
    bool ran = false;          // the hold wrote this call
    bool captured = false;     // a pose was captured this call
    bool failed = false;       // the hold had a pose and a weight and could not write
    uint8_t source = kNone;    // the held pose's source after this call's capture
    double alpha = 1.0;        // the capture's weight of this call's pose
    double weight = 0.0;       // the hold's weight this call
    double preNeckY = std::nan("");
    float preM[3][16] = {};
};

// One call's figures, taken once the body is written.
struct Sample {
    bool crouchedAds = false, moving = false, failed = false;
    double weight = 0.0;
    uint8_t source = kNone;
    double leanWrittenDeg = std::nan(""), leanAnimDeg = std::nan(""), leanHeldDeg = std::nan("");
    double shoulderM[3] = {std::nan(""), std::nan(""), std::nan("")};   // the left upper arm joint less the eye, in the heading's axes
};
struct Counters {
    uint32_t ads = 0, still = 0, moving = 0, held = 0, failed = 0;
    uint32_t src[3] = {};                 // the moving frames by the held pose's source: none, a still frame, the first frame
    double weightSum = 0.0;               // over the moving frames
    uint32_t leanN = 0, leanMoved = 0, leanHeld = 0;   // over the held frames with a lean: with one, the animation far from the held pose, of those the written lean near it
    double leanWrittenSum = 0.0, leanAnimSum = 0.0, leanHeldSum = 0.0;
    uint32_t shN[2] = {};                 // the frames with the shoulder's offset: still, moving
    double shSum[2][3] = {};
    void add(const Sample& s) noexcept {
        if (!s.crouchedAds) return;
        ++ads;
        if (s.failed) ++failed;
        if (!s.moving) {
            ++still;
        } else {
            ++moving;
            weightSum += std::isfinite(s.weight) ? s.weight : 0.0;
            ++src[s.source <= kFirstFrame ? s.source : kNone];
            const bool heldFrame = std::isfinite(s.weight) && s.weight >= kHeldHalf;
            if (heldFrame) {
                ++held;
                if (std::isfinite(s.leanWrittenDeg) && std::isfinite(s.leanAnimDeg) && std::isfinite(s.leanHeldDeg)) {
                    ++leanN;
                    leanWrittenSum += s.leanWrittenDeg;
                    leanAnimSum += s.leanAnimDeg;
                    leanHeldSum += s.leanHeldDeg;
                    if (std::fabs(s.leanAnimDeg - s.leanHeldDeg) >= kLeanTolDeg) {
                        ++leanMoved;
                        if (std::fabs(s.leanWrittenDeg - s.leanHeldDeg) <= kLeanTolDeg) ++leanHeld;
                    }
                }
            }
        }
        if (std::isfinite(s.shoulderM[0]) && std::isfinite(s.shoulderM[1]) && std::isfinite(s.shoulderM[2])) {
            const int i = s.moving ? 1 : 0;
            ++shN[i];
            for (int c = 0; c < 3; ++c) shSum[i][c] += s.shoulderM[c];
        }
    }
    void merge(const Counters& o) noexcept {
        ads += o.ads; still += o.still; moving += o.moving; held += o.held; failed += o.failed;
        for (int i = 0; i < 3; ++i) src[i] += o.src[i];
        weightSum += o.weightSum;
        leanN += o.leanN; leanMoved += o.leanMoved; leanHeld += o.leanHeld;
        leanWrittenSum += o.leanWrittenSum; leanAnimSum += o.leanAnimSum; leanHeldSum += o.leanHeldSum;
        for (int i = 0; i < 2; ++i) {
            shN[i] += o.shN[i];
            for (int c = 0; c < 3; ++c) shSum[i][c] += o.shSum[i][c];
        }
    }
};

// The seconds of a window: those with crouched-in-the-sights moving frames (kMinSecondFrames or more), those held in half or more of them, and of the ones whose held frames had the
// animated lean far from the held pose, those where the written lean stayed near it (9 in 10 of those frames).
struct Seconds {
    uint32_t moving = 0, held = 0, leanMoved = 0, leanHeld = 0;
    void fold(const Counters& second) noexcept {
        if (second.moving < kMinSecondFrames) return;
        ++moving;
        if (second.held * 2u >= second.moving) ++held;
        if (second.leanMoved) {
            ++leanMoved;
            if (second.leanHeld * 10u >= second.leanMoved * 9u) ++leanHeld;
        }
    }
};

// ---- the line -------------------------------------------------------------------------------------------------------------------------------------------------------------

// The window's line: the item; the frames crouched in the sights, still and moving; on the moving ones the hold's weight mean, the frames held (weight 0.5 or more) and the held pose's
// source; on the held ones the chest's lean as written, animated and as the held pose has it, how many frames had the animation far from the held pose and in how many of them the
// written lean stayed near it; the left shoulder's offset from the eye still and moving; then the seconds. The log's lines end at 1200 bytes; this one runs to about 800.
inline int formatSummary(char* buf, size_t n, uint32_t seconds, const char* item, const Counters& c, const Seconds& s) noexcept {
    const double mn = c.moving ? static_cast<double>(c.moving) : 1.0, ln = c.leanN ? static_cast<double>(c.leanN) : 1.0;
    const double sn0 = c.shN[0] ? static_cast<double>(c.shN[0]) : 1.0, sn1 = c.shN[1] ? static_cast<double>(c.shN[1]) : 1.0;
    return std::snprintf(buf, n,
                         "native on foot upper body hold (F166) %u s: item %s; crouched in the sights %u frames (still %u, moving %u); on the moving frames the hold weight mean %.2f, held "
                         "(weight 0.5 or more) in %u, the held pose from a still frame in %u, from the press's first frame in %u, none in %u; on the held frames the chest's lean as "
                         "written %+.1f deg, animated %+.1f, the held pose's %+.1f (the animation %.0f deg or more from the held pose in %u frames, the written lean within that of "
                         "it in %u); the left shoulder from the eye (right/up/forward, cm): still %u frames %+.1f/%+.1f/%+.1f, moving %u frames %+.1f/%+.1f/%+.1f; seconds crouched "
                         "in the sights and moving %u, held in half or more of them %u, with the animated lean far from the held pose %u, the written lean near it in %u; "
                         "writes refused %u",
                         seconds, item ? item : "none", c.ads, c.still, c.moving, c.weightSum / mn, c.held, c.src[kStill], c.src[kFirstFrame], c.src[kNone],
                         c.leanWrittenSum / ln, c.leanAnimSum / ln, c.leanHeldSum / ln, kLeanTolDeg, c.leanMoved, c.leanHeld, c.shN[0], c.shSum[0][0] * 100.0 / sn0,
                         c.shSum[0][1] * 100.0 / sn0, c.shSum[0][2] * 100.0 / sn0, c.shN[1], c.shSum[1][0] * 100.0 / sn1, c.shSum[1][1] * 100.0 / sn1, c.shSum[1][2] * 100.0 / sn1,
                         s.moving, s.held, s.leanMoved, s.leanHeld, c.failed);
}

}  // namespace f166
}  // namespace nofoot
}  // namespace edvr
