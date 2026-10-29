// F166 checks (native on-foot VR), run166 below. Header only, run by native_frame_test.
//
// F166 (native_on_foot_f166.h): the upper body held at its still crouched-ADS pose while walking crouched in the sights. The spine from above the pelvis to the chest (16, 21, 24), both
// clavicles and both upper arms take their local transforms from the pose captured crouched, in the sights and still; every joint under them comes with them unchanged against its held
// ancestor; the pelvis and the legs are not touched. The weight is the crouched weight x the detach weight x a moving ramp, so standing, at the hip and still are bit for bit the earlier
// code. The figures are the flights edvr_gfx_20261028_135901 and edvr_gfx_20261028_172556 (build 60ea7dc3): crouched in the sights and still the animated chest leans +47 to +49 deg and
// the neck stands 0.85 m, crouch-walking +70 to +75 deg and 1.04 to 1.07 m, and the left shoulder rises 23 cm and goes 12 cm forward at the walk's onset (F76 event 33 of the first
// flight: (y 0.850, z 0.235) to (1.079, 0.357) in 3 frames). The skeleton is built here with the real joint numbers (hips 7, spine 16/21/24, neck 34, head 44, clavicles 35/36, upper arms
// 25/26) and the real chain shape (the spine root 16 under the hips, 1/1/N joints exclusive to the spine levels); its lengths are a commander's, not the game's.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f163.h"
#include "../../src/d3d11/native_on_foot_f166.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace f166test {

namespace nf = edvr::nofoot;
namespace f166 = edvr::nofoot::f166;
namespace f163 = edvr::nofoot::f163;

constexpr uint32_t kN = 110;
// The joints the rig uses (the real numbers where the game's are known).
constexpr uint32_t kHips = 7, kPelvis = 10, kThighR = 17, kThighL = 18, kSpine0 = 16, kSpine1 = 21, kChest = 24, kUpperL = 25, kUpperR = 26, kForeL = 31, kForeR = 32, kNeck = 34,
                   kClavL = 35, kClavR = 36, kHead = 44, kHandL = 58, kHandR = 70, kFingerL = 60, kHeadExtra = 100;

struct Pose {
    double leanDeg = 0.0;       // the spine's forward lean, split in thirds over 16, 21, 24 (positive leans the neck forward: the rig finds the sign)
    double hipRise = 0.0;       // m, the hips raised
    double spineRise = 0.0;     // m, the spine root raised against the hips
    bool target = false;        // put the left upper arm at `shoulder` (model axes) whatever the chain gives
    double shoulder[3] = {};
};
struct Rig {
    uint16_t parent[kN];
    float local[kN][16];
    float model[kN][16];
};

inline void setLocal(Rig& r, uint32_t j, uint32_t parent, double x, double y, double z, double rotXDeg = 0.0) {
    r.parent[j] = static_cast<uint16_t>(parent);
    f166::Rec rec;
    rec.p[0] = x;
    rec.p[1] = y;
    rec.p[2] = z;
    const double h = rotXDeg * 3.14159265358979323846 / 360.0;
    rec.q[0] = std::sin(h);
    rec.q[3] = std::cos(h);
    f166::matOf(rec, r.local[j]);
}
inline void solveModel(Rig& r, uint32_t j, bool* done, const Pose& p) {
    if (done[j]) return;
    if (r.parent[j] == 0xFFFFu) {
        std::memcpy(r.model[j], r.local[j], sizeof(r.model[j]));
    } else {
        solveModel(r, r.parent[j], done, p);
        if (j == kUpperL && p.target) {   // the left upper arm where the flight had it: the local translation that puts it there under this chain
            float inv[16];
            nf::pfzAffineInverse(r.model[r.parent[j]], inv);
            double t[3];
            nf::ikXform(p.shoulder, inv, t);
            r.local[j][12] = static_cast<float>(t[0]);
            r.local[j][13] = static_cast<float>(t[1]);
            r.local[j][14] = static_cast<float>(t[2]);
        }
        nf::pfzMul(r.local[j], r.model[r.parent[j]], r.model[j]);
    }
    done[j] = true;
}
// A skeleton like the game's third-person one where it matters, in a pose. lean is signed by leanSign (the rig's own forward).
inline Rig makeRig(const Pose& p, double leanSign) {
    Rig r;
    for (uint32_t j = 0; j < kN; ++j) setLocal(r, j, j == 0 ? 0xFFFFu : 0u, 0.0, 0.0, 0.0);
    const double a = leanSign * p.leanDeg / 3.0;
    setLocal(r, kHips, 0, 0.0, 0.55 + p.hipRise, 0.0);
    setLocal(r, kPelvis, kHips, 0.0, -0.05, 0.0);
    setLocal(r, kThighR, kPelvis, 0.09, -0.08, 0.0);
    setLocal(r, kThighL, kPelvis, -0.09, -0.08, 0.0);
    setLocal(r, kSpine0, kHips, 0.0, 0.10 + p.spineRise, 0.0, a);
    setLocal(r, kSpine1, kSpine0, 0.0, 0.12, 0.0, a);
    setLocal(r, kChest, kSpine1, 0.0, 0.14, 0.0, a);
    setLocal(r, kNeck, kChest, 0.0, 0.20, 0.0);   // straight up the chest: the chest-to-neck lean is the spine's own
    setLocal(r, kHead, kNeck, 0.0, 0.12, 0.0);
    setLocal(r, kClavL, kChest, -0.04, 0.12, 0.03);
    setLocal(r, kClavR, kChest, 0.04, 0.12, 0.03);
    setLocal(r, kUpperL, kClavL, -0.14, 0.0, 0.0);
    setLocal(r, kUpperR, kClavR, 0.14, 0.0, 0.0);
    setLocal(r, kForeL, kUpperL, 0.0, -0.28, 0.0, 12.0);
    setLocal(r, kForeR, kUpperR, 0.0, -0.28, 0.0, -9.0);
    setLocal(r, kHandL, kForeL, 0.0, -0.27, 0.0);
    setLocal(r, kHandR, kForeR, 0.0, -0.27, 0.0);
    setLocal(r, kFingerL, kHandL, 0.02, -0.04, 0.0, 20.0);
    setLocal(r, kHeadExtra, kHead, 0.01, 0.05, 0.0);
    setLocal(r, 101, kHead, -0.01, 0.05, 0.0);
    bool done[kN] = {};
    for (uint32_t j = 0; j < kN; ++j) solveModel(r, j, done, p);
    return r;
}
inline double leanOf(const float (*model)[16]) {
    const double c[3] = {model[kChest][12], model[kChest][13], model[kChest][14]}, n[3] = {model[kNeck][12], model[kNeck][13], model[kNeck][14]};
    return f166::leanDeg(c, n, 0.0, 1.0);
}
inline void posOf(const float m[16], double out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = m[12 + i];
}
inline double dist3(const double a[3], const double b[3]) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }
// A joint's local record against its parent in a model array.
inline bool localRec(const float (*model)[16], uint32_t j, uint32_t parent, f166::Rec* out) {
    float inv[16], local[16];
    if (!nf::pfzAffineInverse(model[parent], inv)) return false;
    nf::pfzMul(model[j], inv, local);
    return f166::recOf(local, out);
}

template <class Check>
inline void run166(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    // the rig's forward: which sign of the spine's rotation leans the neck toward +z (the heading the checks use)
    const double leanSign = leanOf(makeRig(Pose{10.0}, 1.0).model) > 0.0 ? 1.0 : -1.0;
    // the poses: crouched in the sights still (the chest leans 48 deg, the shoulder at (y 0.850, z 0.235) over the model), and crouch-walking (74 deg, the shoulder 23 cm up and 12 cm
    // forward of that, the spine root 15 cm up against the hips: F76 event 33 of edvr_gfx_20261028_135901, the lean of F98a in edvr_gfx_20261028_172556)
    Pose stillPose;
    stillPose.leanDeg = 48.0;
    stillPose.target = true;
    stillPose.shoulder[0] = -0.112;
    stillPose.shoulder[1] = 0.850;
    stillPose.shoulder[2] = 0.235;
    Pose walkPose;
    walkPose.leanDeg = 74.0;
    walkPose.spineRise = 0.15;
    walkPose.target = true;
    walkPose.shoulder[0] = -0.112 + 0.0;
    walkPose.shoulder[1] = 1.079;
    walkPose.shoulder[2] = 0.357;
    const Rig still = makeRig(stillPose, leanSign), walk = makeRig(walkPose, leanSign);
    uint16_t parents[kN];
    for (uint32_t j = 0; j < kN; ++j) parents[j] = still.parent[j];
    const uint32_t clav[2] = {kClavL, kClavR}, upper[2] = {kUpperL, kUpperR};
    const f166::Plan plan = f166::buildPlan(parents, kN, clav, upper);

    // ---- 1. the plan ----------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const uint16_t expJoint[7] = {kSpine0, kSpine1, kChest, kClavL, kUpperL, kClavR, kUpperR}, expParent[7] = {kHips, kSpine0, kSpine1, kChest, kClavL, kChest, kClavR};
        const int8_t expSlot[7] = {-1, 0, 1, 2, 3, 2, 5};
        bool order = plan.ok() && plan.n == 7 && plan.arms == 3;
        for (int k = 0; k < 7 && order; ++k) order = plan.joint[k] == expJoint[k] && plan.parent[k] == expParent[k] && plan.parentSlot[k] == expSlot[k];
        check(order, "F166: the held joints are the spine 16, 21, 24 and each side's clavicle and upper arm, parent first, each with its parent (16's the hips) and its parent's slot among them");
        // the joints that come with them: every joint under a held one, by brute force, each with its nearest held ancestor; the legs, the pelvis, the hips and the root are not among them
        uint32_t expected = 0;
        bool owners = true, notLegs = true;
        for (uint32_t j = 0; j < kN; ++j) {
            bool held = false;
            for (uint32_t k = 0; k < plan.n; ++k) held = held || plan.joint[k] == j;
            if (held) continue;
            int nearest = -1;
            for (uint32_t a = parents[j]; a != 0xFFFFu && nearest < 0; a = parents[a])
                for (uint32_t k = 0; k < plan.n; ++k)
                    if (plan.joint[k] == a) nearest = static_cast<int>(k);
            if (nearest < 0) continue;
            ++expected;
            bool found = false;
            for (uint32_t i = 0; i < plan.carriedN; ++i)
                if (plan.carried[i] == j) found = plan.owner[i] == nearest;
            owners = owners && found;
        }
        for (uint32_t i = 0; i < plan.carriedN; ++i)
            notLegs = notLegs && plan.carried[i] != kHips && plan.carried[i] != kPelvis && plan.carried[i] != kThighL && plan.carried[i] != kThighR && plan.carried[i] != 0;
        check(plan.carriedN == expected && owners && notLegs && expected == 9,
              "F166: every joint under a held one is carried by its nearest held ancestor (the neck and head by the chest, a forearm and the hand by its upper arm), and the hips, pelvis, legs and root are not touched");
        auto ownerOf = [&](uint32_t j) { for (uint32_t i = 0; i < plan.carriedN; ++i) if (plan.carried[i] == j) return static_cast<int>(plan.owner[i]); return -1; };
        check(ownerOf(kNeck) == 2 && ownerOf(kHead) == 2 && ownerOf(kHeadExtra) == 2 && ownerOf(kForeL) == 4 && ownerOf(kHandL) == 4 && ownerOf(kFingerL) == 4 && ownerOf(kForeR) == 6 &&
                  ownerOf(kHandR) == 6 && ownerOf(kThighL) == -1,
              "F166: the neck, the head and what is under it come with the chest, the left forearm, hand and fingers with the left upper arm, the right with the right, a thigh with nothing");
        // refusals
        uint16_t bad[kN];
        std::memcpy(bad, parents, sizeof(bad));
        bad[kSpine1] = kHips;
        const f166::Plan notChain = f166::buildPlan(bad, kN, clav, upper);
        std::memcpy(bad, parents, sizeof(bad));
        bad[kSpine0] = 0xFFFFu;
        const f166::Plan noParent = f166::buildPlan(bad, kN, clav, upper);
        std::memcpy(bad, parents, sizeof(bad));
        bad[kClavL] = kSpine1;
        const f166::Plan leftOut = f166::buildPlan(bad, kN, clav, upper);
        std::memcpy(bad, parents, sizeof(bad));
        bad[kUpperR] = kChest;
        const f166::Plan rightOut = f166::buildPlan(bad, kN, clav, upper);
        std::memcpy(bad, parents, sizeof(bad));
        bad[kHead] = kHeadExtra;
        bad[kHeadExtra] = kHead;
        const f166::Plan cycle = f166::buildPlan(bad, kN, clav, upper);
        const uint32_t noArms[2] = {0xFFFFu, 0xFFFFu};
        const f166::Plan spineOnly = f166::buildPlan(parents, kN, noArms, noArms);
        check(notChain.why == f166::kNoSpineChain && !notChain.ok() && noParent.why == f166::kBadSpineParent && cycle.why == f166::kBadTree && !cycle.ok() &&
                  f166::buildPlan(nullptr, kN, clav, upper).why == f166::kBadInput && f166::buildPlan(parents, 0, clav, upper).why == f166::kBadInput,
              "F166: a spine that is not a chain of direct parents, a spine root with no parent, a cycle, no joints or no parents refuse the plan");
        check(leftOut.ok() && leftOut.arms == 2 && leftOut.n == 5 && rightOut.ok() && rightOut.arms == 1 && rightOut.n == 5 && spineOnly.ok() && spineOnly.arms == 0 && spineOnly.n == 3,
              "F166: a side whose clavicle is not the chest's child or whose upper arm is not the clavicle's child is left out alone; with no arm plan the spine is held alone");
    }

    // ---- 2. the pose math: slerp, short arc, exact ends ------------------------------------------------------------------------------------------------------------------------------
    {
        f166::Rec a, b;
        a.q[0] = 0.0; a.q[1] = 0.0; a.q[2] = 0.0; a.q[3] = 1.0;
        const double h = 60.0 * 3.14159265358979323846 / 360.0;
        b.q[0] = std::sin(h); b.q[1] = 0.0; b.q[2] = 0.0; b.q[3] = std::cos(h);   // 60 deg about x
        b.p[0] = 1.0; b.p[1] = 2.0; b.p[2] = 3.0;
        const f166::Rec w0 = f166::blendRec(a, b, 0.0), w1 = f166::blendRec(a, b, 1.0), wn = f166::blendRec(a, b, std::nan(""));
        const f166::Rec mid = f166::blendRec(a, b, 0.5);
        f166::Rec nb = b;
        for (int i = 0; i < 4; ++i) nb.q[i] = -b.q[i];   // the same rotation, the other sign
        const f166::Rec midNeg = f166::blendRec(a, nb, 0.5);
        check(std::memcmp(&w0, &a, sizeof(a)) == 0 && std::memcmp(&w1, &b, sizeof(b)) == 0 && std::memcmp(&wn, &a, sizeof(a)) == 0,
              "F166: a blend at weight 0 (or not a number) is the animated record bit for bit and at 1 the held record bit for bit");
        check(eq(f166::angleDeg(a, mid), 30.0, 1e-9) && eq(f166::angleDeg(mid, b), 30.0, 1e-9) && eq(mid.p[1], 1.0, 1e-15) && eq(f166::angleDeg(mid, midNeg), 0.0, 1e-4) &&
                  eq(f166::angleDeg(a, f166::blendRec(a, b, 0.25)), 15.0, 1e-9),
              "F166: the rotation is slerped on the short arc (half of 60 deg is 30 deg, a quarter 15, the other sign of the same rotation the same result) and the position lerped");
        // the matrix round trip
        f166::Rec r;
        r.p[0] = 0.25; r.p[1] = -0.5; r.p[2] = 0.125;
        const double ax[3] = {0.3, -0.5, 0.8}, an = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]), hh = 0.9;
        for (int i = 0; i < 3; ++i) r.q[i] = std::sin(hh) * ax[i] / an;
        r.q[3] = std::cos(hh);
        float m[16];
        f166::matOf(r, m);
        f166::Rec back;
        const bool okBack = f166::recOf(m, &back);
        float refl[16], scaled[16];
        std::memcpy(refl, m, sizeof(refl));
        std::memcpy(scaled, m, sizeof(scaled));
        for (int c = 0; c < 3; ++c) { refl[c] = -refl[c]; scaled[c] *= 2.0f; scaled[4 + c] *= 2.0f; scaled[8 + c] *= 2.0f; }
        f166::Rec junk;
        check(okBack && f166::angleDeg(r, back) < 1e-3 && eq(back.p[0], 0.25, 1e-7) && eq(back.p[1], -0.5, 1e-7) && eq(back.p[2], 0.125, 1e-7) && !f166::recOf(refl, &junk) && !f166::recOf(scaled, &junk),
              "F166: a local matrix and its record go back and forth, and a reflection or a doubled scale is not a rotation to hold");
    }

    // ---- 3. the hold on the poses ----------------------------------------------------------------------------------------------------------------------------------------------------
    f166::Held held;
    const bool captured = f166::capture(plan, still.model, kN, f166::kStill, 1.0, &held);
    alignas(16) float out[kN][16];
    uint16_t touched[kN];
    uint32_t touchedN = 0;
    {
        check(captured && held.source == f166::kStill && held.have(), "F166: a still pose is captured from the animated one");
        // the same pose held at weight 1 is that pose: nothing moves when the animation is already the held pose
        std::memcpy(out, still.model, sizeof(out));   // the joints the hold does not touch keep the animated pose
        const bool same = f166::apply(plan, held, 1.0, still.model, kN, out, touched, &touchedN);
        double worst = 0.0;
        for (uint32_t i = 0; same && i < touchedN; ++i)
            for (int c = 0; c < 16; ++c) worst = std::fmax(worst, std::fabs(static_cast<double>(out[touched[i]][c]) - still.model[touched[i]][c]));
        check(same && touchedN == static_cast<uint32_t>(plan.n) + plan.carriedN && worst < 2e-5,
              "F166: the pose held on the very pose it was taken from is that pose (every touched joint within 2e-5), and the touched joints are the held ones and the carried ones");
        // weight 0 or no pose: refused and nothing written
        std::memset(out, 0x7F, sizeof(out));
        float sentinel[kN][16];
        std::memcpy(sentinel, out, sizeof(sentinel));
        uint32_t n0 = 99;
        const bool w0 = f166::apply(plan, held, 0.0, walk.model, kN, out, touched, &n0);
        const bool wn = f166::apply(plan, held, std::nan(""), walk.model, kN, out, touched, &n0);
        f166::Held none;
        const bool np = f166::apply(plan, none, 1.0, walk.model, kN, out, touched, &n0);
        check(!w0 && !wn && !np && n0 == 0 && std::memcmp(out, sentinel, sizeof(out)) == 0,
              "F166: at weight 0 (standing, at the hip, still: the ramp at 0) or with no held pose nothing is computed and nothing is written, bit for bit");
        // the onset: the animated shoulder is 23 cm up and 12 cm forward of the still value; held at weight 1 it is the still value
        std::memcpy(out, walk.model, sizeof(out));
        const bool ok = f166::apply(plan, held, 1.0, walk.model, kN, out, touched, &touchedN);
        double sStill[3], sWalk[3], sHeld[3];
        posOf(still.model[kUpperL], sStill);
        posOf(walk.model[kUpperL], sWalk);
        posOf(out[kUpperL], sHeld);
        check(ok && eq(sWalk[1] - sStill[1], 0.229, 1e-4) && eq(sWalk[2] - sStill[2], 0.122, 1e-4) && dist3(sWalk, sStill) > 0.25 && dist3(sHeld, sStill) < 3e-5,
              "F166: the replayed onset: the animated left shoulder rises 22.9 cm and goes 12.2 cm forward of the still value (F76 event 33), 26 cm off it; held at weight 1 it stays on the still value "
              "(within 0.03 mm) with the hips where they were");
        // every held joint's local against its parent is the held one, and the others keep their animated local against their parent
        bool heldLocals = true, carriedLocals = true;
        for (uint32_t k = 0; k < plan.n; ++k) {
            f166::Rec lr;
            heldLocals = heldLocals && localRec(out, plan.joint[k], plan.parent[k], &lr) && f166::angleDeg(lr, held.rec[k]) < 0.1 && dist3(lr.p, held.rec[k].p) < 3e-5;
        }
        for (uint32_t i = 0; i < plan.carriedN; ++i) {
            const uint32_t j = plan.carried[i];
            f166::Rec la, ln;
            carriedLocals = carriedLocals && localRec(walk.model, j, parents[j], &la) && localRec(out, j, parents[j], &ln) && f166::angleDeg(la, ln) < 0.1 && dist3(la.p, ln.p) < 3e-5;
        }
        check(heldLocals && carriedLocals,
              "F166: each held joint's local against its parent is the held pose's (rotation within 0.1 deg: the float quaternions' own noise is 0.03 deg, position within 0.03 mm) and each joint under them keeps its animated local against its parent: "
              "the neck, head, forearms, hands and fingers move rigidly with the held torso");
        // the lean: held, 48 deg, against the animated 74
        check(eq(leanOf(still.model), 48.0, 1e-3) && eq(leanOf(walk.model), 74.0, 1e-3) && eq(leanOf(out), 48.0, 0.02),
              "F166: the chest's lean as written is the still pose's 48 deg while the animation walks at 74 deg (F98a's +48.8 still against +74.3 moving in the flight)");
        // the hips, the pelvis and the legs are not touched
        bool legsUntouched = true;
        for (uint32_t i = 0; i < touchedN; ++i) legsUntouched = legsUntouched && touched[i] != kHips && touched[i] != kPelvis && touched[i] != kThighL && touched[i] != kThighR;
        check(legsUntouched, "F166: the hips, the pelvis and the legs are never among the joints the hold writes");
        // the weight between: the left shoulder is nearer the still value than the animated one, by about the weight
        std::memcpy(out, walk.model, sizeof(out));
        f166::apply(plan, held, 0.5, walk.model, kN, out, touched, &touchedN);
        double sMid[3];
        posOf(out[kUpperL], sMid);
        const double dFull = dist3(sWalk, sHeld), dMid = dist3(sMid, sHeld);
        check(dMid > 0.2 * dFull && dMid < 0.8 * dFull && eq(leanOf(out), 61.0, 0.5),
              "F166: at weight 0.5 the left shoulder is between the animated and the held one (neither end within a fifth of the span) and the lean half way (61 deg)");
        // a hips rise carries the held torso with it (the pelvis keeps walking)
        Pose bob = walkPose;
        bob.hipRise = 0.11;
        const Rig bobbed = makeRig(bob, leanSign);
        std::memcpy(out, bobbed.model, sizeof(out));
        const bool bobOk = f166::apply(plan, held, 1.0, bobbed.model, kN, out, touched, &touchedN);
        double sBob[3];
        posOf(out[kUpperL], sBob);
        check(bobOk && eq(sBob[1] - sHeld[1], 0.11, 3e-5), "F166: the hold is against the hips: with the hips 11 cm higher the held shoulder is 11 cm higher too");
        // a singular parent or a non-finite pose refuses
        Rig broken = walk;
        for (int c = 0; c < 16; ++c) broken.model[kHips][c] = 0.0f;
        const bool singular = f166::apply(plan, held, 1.0, broken.model, kN, out, touched, &touchedN);
        Rig nonFinite = walk;
        nonFinite.model[kChest][13] = std::nanf("");
        const bool notFinite = f166::apply(plan, held, 1.0, nonFinite.model, kN, out, touched, &touchedN);
        check(!singular && !notFinite && touchedN == 0, "F166: a singular parent matrix or a pose that is not finite refuses the hold (nothing written)");
        // a capture from a broken pose leaves the held pose as it was
        f166::Held keep = held;
        const bool badCapture = f166::capture(plan, broken.model, kN, f166::kStill, 1.0, &keep);
        check(!badCapture && std::memcmp(&keep, &held, sizeof(keep)) == 0, "F166: a capture from a pose with a singular parent fails and leaves the held pose as it was");
    }

    // ---- 4. the capture rules and the weight ----------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double dt = 1.0 / 90.0;
        const auto call = [&](f166::State& s, double posture, double detach, double speed = 0.0, double vy = 0.0, uint32_t item = 7, uint32_t key = 3) {
            f166::In in;
            in.postureW = posture; in.detachW = detach; in.speed = speed; in.vy = vy; in.dtSec = dt; in.item = item; in.planKey = key;
            return s.step(in);
        };
        // a press that starts still: the first frame, then the still pose once the body has been still 0.25 s, then the running average
        {
            f166::State s;
            int firsts = 0, stills = 0, averages = 0, frames = 0;
            double alphaFirst = -1.0, alphaAvg = -1.0;
            double firstStillAt = -1.0;
            for (int i = 0; i < 90; ++i) {
                const f166::Step st = call(s, 1.0, 1.0, 0.0);
                if (st.action == f166::kCaptureFirst) { ++firsts; alphaFirst = st.alpha; s.held.source = f166::kFirstFrame; }
                if (st.action == f166::kCaptureStill) { ++stills; firstStillAt = i * dt; s.held.source = f166::kStill; }
                if (st.action == f166::kAverageStill) { ++averages; alphaAvg = st.alpha; }
                ++frames;
            }
            check(firsts == 1 && alphaFirst == 1.0 && stills == 1 && eq(firstStillAt, 0.25, 2.0 * dt) && frames == 90 && averages > 40 && eq(alphaAvg, 1.0 - std::exp(-dt / f166::kAverageSec), 1e-12),
                  "F166: a press that starts still takes the first frame, then at 0.25 s still the still pose (once), then averages it each call by 1 - exp(-dt / 0.12)");
            check(s.held.source == f166::kStill && s.ramp == 0.0, "F166: still, the moving ramp stays at 0");
        }
        // a press that starts moving: the first frame, never replaced while moving; the ramp is in over 0.15 s
        {
            f166::State s;
            int firsts = 0, others = 0;
            double rampAt015 = -1.0, w015 = -1.0, firstWeight = -1.0;
            for (int i = 0; i < 60; ++i) {
                const f166::Step st = call(s, 1.0, 1.0, 1.5);
                if (st.action == f166::kCaptureFirst) { ++firsts; s.held.source = f166::kFirstFrame; firstWeight = st.weight; }
                else if (st.action != f166::kNothing) ++others;
                if (i == static_cast<int>(std::ceil(0.15 / dt)) - 1) { rampAt015 = st.ramp; w015 = st.weight; }
            }
            check(firsts == 1 && others == 0 && s.held.source == f166::kFirstFrame && eq(firstWeight, dt / f166::kRampInSec, 1e-12),
                  "F166: a press that starts moving has no still pose: its first crouched in-the-sights frame is the held pose (once, never replaced while moving), and the weight begins at the ramp's first step");
            check(eq(rampAt015, 1.0, 1e-9) && eq(w015, 1.0, 1e-9) && s.ramp == 1.0, "F166: the moving ramp is full 0.15 s after the body moves, and the weight with it (the crouched and detach weights 1)");
            // out over 0.2 s when it stops (the speed under the walk threshold)
            double rampEnd = -1.0;
            for (int i = 0; i < static_cast<int>(std::round(0.2 / dt)); ++i) rampEnd = call(s, 1.0, 1.0, 0.0).ramp;
            check(eq(rampEnd, 0.0, 1e-9), "F166: the ramp is out again 0.2 s after the body stops");
            // the first frame is replaced by a still pose after 0.25 s still
            int replaced = 0;
            for (int i = 0; i < 60; ++i) {
                const f166::Step st = call(s, 1.0, 1.0, 0.0);
                if (st.action == f166::kCaptureStill) { ++replaced; s.held.source = f166::kStill; }
            }
            check(replaced == 1 && s.held.source == f166::kStill, "F166: the first frame's pose is replaced by a still one once the body has been still 0.25 s in the sights");
        }
        // thresholds: below 0.9 nothing is captured
        {
            f166::State s;
            bool any = false;
            for (int i = 0; i < 30; ++i) {
                any = any || call(s, 0.89, 1.0, 0.0).action != f166::kNothing || call(s, 1.0, 0.89, 0.0).action != f166::kNothing;
            }
            check(!any && !s.held.have(), "F166: a crouched weight or a detach weight under 0.9 captures nothing");
        }
        // clearing: a new press (detach under 0.05), leaving the crouch (under 0.5), another weapon, another skeleton
        {
            const auto armed = [&](f166::State& s) { for (int i = 0; i < 4; ++i) { const f166::Step st = call(s, 1.0, 1.0, 1.5); if (st.action == f166::kCaptureFirst) s.held.source = f166::kFirstFrame; } };
            f166::State s;
            armed(s);
            const bool had = s.held.have();
            const f166::Step press = call(s, 1.0, 0.04);
            const bool pressCleared = press.cleared && !s.held.have();
            armed(s);
            call(s, 0.49, 1.0, 1.5);
            const bool crouchCleared = !s.held.have();
            armed(s);
            const f166::Step itemStep = call(s, 1.0, 1.0, 1.5, 0.0, 8, 3);
            const bool itemCleared = itemStep.cleared;
            if (itemStep.action == f166::kCaptureFirst) s.held.source = f166::kFirstFrame;
            const f166::Step planStep = call(s, 1.0, 1.0, 1.5, 0.0, 8, 4);
            check(had && pressCleared && crouchCleared && itemCleared && planStep.cleared,
                  "F166: a new press (the detach weight under 0.05), leaving the crouch (under 0.5), another weapon and another skeleton each clear the held pose");
            // after a clear the next in-the-sights frame takes the first frame again
            f166::State t;
            armed(t);
            call(t, 1.0, 0.0);
            const f166::Step again = call(t, 1.0, 1.0, 1.5);
            check(again.action == f166::kCaptureFirst, "F166: after a new press the first in-the-sights frame is captured again");
        }
        // the weight: the crouched weight x the detach weight x the ramp; standing, at the hip and still are 0
        {
            check(f166::holdWeight(1.0, 1.0, 1.0) == 1.0 && f166::holdWeight(0.0, 1.0, 1.0) == 0.0 && f166::holdWeight(1.0, 0.0, 1.0) == 0.0 && f166::holdWeight(1.0, 1.0, 0.0) == 0.0 &&
                      eq(f166::holdWeight(0.5, 0.5, 0.5), 0.125, 1e-15) && f166::holdWeight(std::nan(""), 1.0, 1.0) == 0.0 && f166::holdWeight(1.0, 1.0, 1.5) == 1.0,
                  "F166: the weight is the crouched weight x the detach weight x the ramp, each held to 0..1; standing, at the hip and still read 0; a figure that is not a number reads 0");
            f166::State s;
            double standingW = 0.0, hipW = 0.0, stillW = 0.0;
            for (int i = 0; i < 60; ++i) standingW = std::fmax(standingW, call(s, 0.0, 1.0, 1.5).weight);
            f166::State h;
            for (int i = 0; i < 60; ++i) hipW = std::fmax(hipW, call(h, 1.0, 0.0, 1.5).weight);
            f166::State t;
            for (int i = 0; i < 60; ++i) stillW = std::fmax(stillW, call(t, 1.0, 1.0, 0.0).weight);
            check(standingW == 0.0 && hipW == 0.0 && stillW == 0.0, "F166: walking standing, walking at the hip and crouched in the sights and still never give a weight over 0 (the hold writes nothing)");
            f166::State p;
            double half = 0.0;
            const f166::Step first = call(p, 1.0, 1.0, 1.5);   // the pose is captured crouched (1.0); the stance then eases to 0.7
            if (first.action == f166::kCaptureFirst) p.held.source = f166::kFirstFrame;
            for (int i = 0; i < 60; ++i) half = call(p, 0.7, 1.0, 1.5).weight;
            check(eq(half, 0.7, 1e-12), "F166: a crouched weight of 0.7 in the sights, moving, with the ramp full, holds at 0.7");
        }
        // moving as F95a files it, still under 0.1 m/s; a speed that is not a number is neither
        check(f166::movingNow(0.35, 0.0) && f166::movingNow(0.0, 0.9) && !f166::movingNow(0.34, 0.8) && !f166::movingNow(std::nan(""), 0.0) && f166::stillNow(0.09, 0.0) && !f166::stillNow(0.1, 0.0) &&
                  !f166::stillNow(0.0, 0.9) && !f166::stillNow(std::nan(""), 0.0),
              "F166: moving is F95a's (the walk threshold 0.35 m/s, or the vertical speed past 0.8), still is under 0.1 m/s on the level, a speed that is not a number is neither");
    }

    // ---- 5. the onset replayed through the state and the apply -------------------------------------------------------------------------------------------------------------------------
    {
        const double dt = 1.0 / 90.0;
        f166::State s;
        f166::Held& h = s.held;
        double speed = 0.0;
        // 0.6 s crouched in the sights and still: the still pose is captured and averaged
        for (int i = 0; i < 54; ++i) {
            f166::In in;
            in.postureW = 1.0; in.detachW = 1.0; in.speed = 0.0; in.dtSec = dt; in.item = 7; in.planKey = 3;
            const f166::Step st = s.step(in);
            if (st.action != f166::kNothing)
                f166::capture(plan, still.model, kN, st.action == f166::kCaptureFirst ? f166::kFirstFrame : f166::kStill, st.alpha, &h);
        }
        // the walk's onset: the animation goes from the still pose to the walking one over 3 frames, the AMC's speed (low-passed, 0.15 s) goes from 0 to 1.5 m/s
        double stillPos[3], hipsStill[3];
        posOf(still.model[kUpperL], stillPos);
        posOf(still.model[kHips], hipsStill);
        double worstAnim = 0.0, devAtFull = -1.0, tFull = -1.0, devEarly = -1.0, devMid = -1.0;
        for (int i = 0; i < 40; ++i) {
            const double u = i < 3 ? (i + 1) / 3.0 : 1.0;   // the animation's travel from still to walking
            speed += (1.0 - std::exp(-dt / 0.15)) * (1.5 - speed);
            Pose mix;
            mix.leanDeg = 48.0 + (74.0 - 48.0) * u;
            mix.spineRise = 0.15 * u;
            mix.target = true;
            for (int c = 0; c < 3; ++c) mix.shoulder[c] = stillPose.shoulder[c] + (walkPose.shoulder[c] - stillPose.shoulder[c]) * u;
            const Rig anim = makeRig(mix, leanSign);
            f166::In in;
            in.postureW = 1.0; in.detachW = 1.0; in.speed = speed; in.dtSec = dt; in.item = 7; in.planKey = 3;
            const f166::Step st = s.step(in);
            double sAnim[3], sOut[3], hipsNow[3];
            posOf(anim.model[kUpperL], sAnim);
            posOf(anim.model[kHips], hipsNow);
            double outShoulder[3] = {sAnim[0], sAnim[1], sAnim[2]};
            if (st.weight > 0.0 && f166::apply(plan, h, st.weight, anim.model, kN, out, touched, &touchedN)) posOf(out[kUpperL], outShoulder);
            for (int c = 0; c < 3; ++c) sOut[c] = outShoulder[c] - hipsNow[c] - (stillPos[c] - hipsStill[c]);
            const double dev = std::sqrt(sOut[0] * sOut[0] + sOut[1] * sOut[1] + sOut[2] * sOut[2]);
            double animRel[3] = {sAnim[0] - hipsNow[0] - (stillPos[0] - hipsStill[0]), sAnim[1] - hipsNow[1] - (stillPos[1] - hipsStill[1]), sAnim[2] - hipsNow[2] - (stillPos[2] - hipsStill[2])};
            worstAnim = std::fmax(worstAnim, std::sqrt(animRel[0] * animRel[0] + animRel[1] * animRel[1] + animRel[2] * animRel[2]));
            if (i == 4) devEarly = dev;
            if (st.ramp >= 0.5 && devMid < 0.0) devMid = dev;
            if (st.ramp >= 1.0 && tFull < 0.0) { tFull = i * dt; devAtFull = dev; }
        }
        check(worstAnim > 0.25 && devAtFull >= 0.0 && devAtFull < 3e-5 && devEarly > 0.2 && devMid > 0.05 && devMid < devEarly && devAtFull < devMid && tFull > 0.12 && tFull < 0.25,
              "F166: through the onset (the animation travelling still to walking in 3 frames, the speed rising over 0.15 s) the animated left shoulder is more than 25 cm from the still value against the "
              "hips; the held one is still 20 cm out in the first frames, comes in as the ramp rises, and once the ramp is full (about 0.19 s) it is on the still value within 0.03 mm");
    }

    // ---- 6. the stance's weight reads the game's neck, so the hold cannot keep itself on -----------------------------------------------------------------------------------------------
    {
        // standing up in the sights while walking: the animated neck climbs 1.05 to 1.50 m over a second; the weight from the game's neck falls to 0 and clears the held pose. The torso the
        // hold writes puts the neck low (about 0.85 to 0.96 m): read from there the weight would stay 1 for ever.
        const double dt = 1.0 / 90.0;
        f166::State s;
        double minAnimWeight = 1.0, lastHoldWeight = -1.0;
        bool clearedAt = false;
        for (int i = 0; i < 135; ++i) {
            const double neck = 1.05 + (1.50 - 1.05) * (i / 134.0);
            const double posture = f163::postureWeight(f163::neckDrop(false, 0.0, neck));
            f166::In in;
            in.postureW = posture; in.detachW = 1.0; in.speed = 1.5; in.dtSec = dt; in.item = 7; in.planKey = 3;
            const f166::Step st = s.step(in);
            if (st.action == f166::kCaptureFirst) s.held.source = f166::kFirstFrame;
            if (st.cleared) clearedAt = true;
            minAnimWeight = std::fmin(minAnimWeight, posture);
            lastHoldWeight = st.weight;
        }
        // the same climb read from the held torso's neck (0.96 m throughout): the weight would never fall
        f166::State bad;
        double badW = 0.0;
        for (int i = 0; i < 135; ++i) {
            const double posture = f163::postureWeight(f163::neckDrop(false, 0.0, 0.96));
            f166::In in;
            in.postureW = posture; in.detachW = 1.0; in.speed = 1.5; in.dtSec = dt; in.item = 7; in.planKey = 3;
            const f166::Step st = bad.step(in);
            if (st.action == f166::kCaptureFirst) bad.held.source = f166::kFirstFrame;
            badW = st.weight;
        }
        check(clearedAt && minAnimWeight == 0.0 && lastHoldWeight == 0.0 && badW == 1.0,
              "F166: standing up in the sights while walking, the weight read from the game's animated neck falls to 0 and clears the held pose; read from the held torso's low neck it would stay 1 "
              "(which is why the stance's weight is taken from the neck before the hold writes)");
    }

    // ---- 7. the counters, the seconds and the line ---------------------------------------------------------------------------------------------------------------------------------------
    {
        f166::Counters c;
        const auto frame = [&](bool ads, bool moving, double w, uint8_t src, double written, double anim, double heldLean, double right) {
            f166::Sample s;
            s.crouchedAds = ads; s.moving = moving; s.weight = w; s.source = src;
            s.leanWrittenDeg = written; s.leanAnimDeg = anim; s.leanHeldDeg = heldLean;
            s.shoulderM[0] = right; s.shoulderM[1] = -0.10; s.shoulderM[2] = -0.30;
            c.add(s);
        };
        for (int i = 0; i < 40; ++i) frame(true, false, 0.0, f166::kStill, 48.0, 48.0, 48.0, -0.11);                            // still
        for (int i = 0; i < 60; ++i) frame(true, true, 1.0, f166::kStill, 49.0, 74.0, 48.0, -0.11);                              // moving, held, lean held
        for (int i = 0; i < 10; ++i) frame(true, true, 0.2, f166::kFirstFrame, 70.0, 74.0, 48.0, -0.05);                         // moving, ramping in
        for (int i = 0; i < 5; ++i) frame(true, true, 0.0, f166::kNone, 74.0, 74.0, std::nan(""), -0.05);                        // moving with no pose
        for (int i = 0; i < 7; ++i) frame(false, true, 0.0, f166::kNone, 74.0, 74.0, std::nan(""), -0.05);                       // not crouched in the sights: not counted
        check(c.ads == 115 && c.still == 40 && c.moving == 75 && c.held == 60 && c.src[f166::kStill] == 60 && c.src[f166::kFirstFrame] == 10 && c.src[f166::kNone] == 5 &&
                  eq(c.weightSum, 60.0 + 2.0, 1e-9) && c.leanN == 60 && c.leanMoved == 60 && c.leanHeld == 60 && eq(c.leanWrittenSum / c.leanN, 49.0, 1e-9) && eq(c.leanAnimSum / c.leanN, 74.0, 1e-9) &&
                  eq(c.leanHeldSum / c.leanN, 48.0, 1e-9) && c.shN[0] == 40 && c.shN[1] == 75 && eq(c.shSum[0][0] / c.shN[0], -0.11, 1e-9),
              "F166: the counters keep the crouched-in-the-sights frames only, still against moving, the moving ones by weight (held from 0.5) and by the held pose's source, the lean as written, animated "
              "and held on the held frames, and the shoulder's offset still against moving");
        // a lean the hold did not keep is counted as moved and not held
        f166::Counters d;
        f166::Sample bad;
        bad.crouchedAds = true; bad.moving = true; bad.weight = 1.0; bad.source = f166::kStill;
        bad.leanWrittenDeg = 74.0; bad.leanAnimDeg = 74.0; bad.leanHeldDeg = 48.0;
        d.add(bad);
        bad.leanAnimDeg = 50.0; bad.leanWrittenDeg = 50.0;   // the animation near the held pose: held trivially, not a lean the hold had to keep
        d.add(bad);
        check(d.held == 2 && d.leanN == 2 && d.leanMoved == 1 && d.leanHeld == 0,
              "F166: a written lean 26 deg off the held pose's where the animation was too is counted as moved and not held; one with the animation already near the held pose is not counted as moved");
        f166::Counters m;
        m.merge(c);
        m.merge(c);
        check(m.ads == 230 && m.held == 120 && m.src[f166::kStill] == 120 && m.shN[1] == 150 && eq(m.weightSum, 2.0 * c.weightSum, 1e-9) && eq(m.shSum[1][2], 2.0 * c.shSum[1][2], 1e-9),
              "F166: two windows add up");
        // the seconds
        f166::Seconds sec;
        sec.fold(c);
        f166::Counters few;
        for (int i = 0; i < 5; ++i) { f166::Sample s; s.crouchedAds = true; s.moving = true; s.weight = 1.0; few.add(s); }
        sec.fold(few);
        sec.fold(d);   // 2 moving frames: under the 10 that make a second of it
        f166::Counters u;
        for (int i = 0; i < 20; ++i) { f166::Sample s; s.crouchedAds = true; s.moving = true; s.weight = 0.0; s.source = f166::kStill; u.add(s); }
        sec.fold(u);
        f166::Counters lost;
        for (int i = 0; i < 20; ++i) { f166::Sample s; s.crouchedAds = true; s.moving = true; s.weight = 1.0; s.source = f166::kStill; s.leanWrittenDeg = 74.0; s.leanAnimDeg = 74.0; s.leanHeldDeg = 48.0; lost.add(s); }
        sec.fold(lost);
        check(sec.moving == 3 && sec.held == 2 && sec.leanMoved == 2 && sec.leanHeld == 1,
              "F166: a second counts from 10 crouched-in-the-sights moving frames; it is held with half or more of them held; its lean is moved when a held frame's animation was 10 deg off the held pose, and held when 9 in 10 of "
              "those frames' written lean stayed within 10 deg of it");
        // the line
        char line[1400];
        const int len = f166::formatSummary(line, sizeof(line), 10, "$wpn_m_shotgun_plasma_doublebarrel_name;", c, sec);
        f166::Counters big;
        big.ads = 4000000000u; big.still = 4000000000u; big.moving = 4000000000u; big.held = 4000000000u; big.failed = 4000000000u;
        for (int i = 0; i < 3; ++i) big.src[i] = 4000000000u;
        big.leanN = big.leanMoved = big.leanHeld = 4000000000u;
        big.shN[0] = big.shN[1] = 4000000000u;
        big.leanWrittenSum = -1.0e12; big.leanAnimSum = 1.0e12; big.leanHeldSum = -1.0e12;
        for (int i = 0; i < 2; ++i) for (int k = 0; k < 3; ++k) big.shSum[i][k] = -1.0e13;
        f166::Seconds bigSec;
        bigSec.moving = bigSec.held = bigSec.leanMoved = bigSec.leanHeld = 4000000000u;
        char worst[1400];
        const int worstLen = f166::formatSummary(worst, sizeof(worst), 4000000000u, "$wpn_m_shotgun_plasma_doublebarrel_name_with_a_long_tail;", big, bigSec);
        check(len > 0 && len < 1100 && worstLen > 0 && worstLen < 1190 && std::strstr(line, "(F166) 10 s: item $wpn_m_shotgun_plasma_doublebarrel_name;") != nullptr &&
                  std::strstr(line, "crouched in the sights 115 frames (still 40, moving 75)") != nullptr && std::strstr(line, "held (weight 0.5 or more) in 60, the held pose from a still frame in 60, from the press's first frame in 10, none in 5") != nullptr &&
                  std::strstr(line, "seconds crouched in the sights and moving 3, held in half or more of them 2, with the animated lean far from the held pose 2, the written lean near it in 1") != nullptr,
              "F166: the window's line names the tag, the frames still against moving, the hold's weight, the held pose's source, the lean as written against animated, the shoulder's offset from the eye, and "
              "the seconds, within the log's 1200 bytes even with every figure at its worst");
        check(std::strstr(line, "the left shoulder from the eye (right/up/forward, cm): still 40 frames -11.0/-10.0/-30.0, moving 75 frames") != nullptr,
              "F166: the left shoulder's offset from the eye is printed in cm, still against moving");
    }
}

}  // namespace f166test
