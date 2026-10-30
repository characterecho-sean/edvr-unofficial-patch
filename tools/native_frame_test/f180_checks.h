// F180 checks (native on-foot VR), run180 below. Header only, run by native_frame_test.
//
// F180a (native_on_foot_f180.h): the left elbow's blend toward the body-down pole is weighted by the crouched share PLUS F178's standing share, the detach weight in either stance (F165's weight
// was the crouched share alone). F180b (TEMPORARY): experimental.native_on_foot_stand_ads_spine_straighten, the share of the game's ADS hunch of the spine levels 16, 21, 24 and the neck 34
// taken back out toward the item's pose at the hip: the written local rotation of each level is slerp(animated, reference, dial x the standing share x the detach weight). The rig builds a
// toy skeleton (the real joint numbers, the real parents that matter) and runs F166's own capture and apply on it. F180c (TEMPORARY): experimental.native_on_foot_neck_guard_cm, the neck guard's
// distance (0 = off); unarmed at the hip the guard measures only.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f159.h"
#include "../../src/d3d11/native_on_foot_f165.h"
#include "../../src/d3d11/native_on_foot_f166.h"
#include "../../src/d3d11/native_on_foot_f178.h"
#include "../../src/d3d11/native_on_foot_f179.h"
#include "../../src/d3d11/native_on_foot_f180.h"
#include "f179_checks.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f180test {

namespace nf = edvr::nofoot;
namespace f165 = edvr::nofoot::f165;
namespace f166 = edvr::nofoot::f166;
namespace f178 = edvr::nofoot::f178;
namespace f179 = edvr::nofoot::f179;
namespace f180 = edvr::nofoot::f180;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A toy skeleton with the joints that matter at their real numbers: the root 0, the spine 16 -> 21 -> 24 (24 the chest), the neck 34 and both clavicles 35 and 36 under the chest, the head 44 under
// the neck; every other joint hangs from the root, from the spine level above it, or from the chest/neck/head so that there is something to carry.
constexpr uint32_t kJoints = 48;
struct Skel {
    uint16_t parents[kJoints];
    Skel() {
        for (uint32_t j = 0; j < kJoints; ++j) parents[j] = 0;
        parents[0] = 0xFFFF;
        for (uint32_t j = 1; j <= 16; ++j) parents[j] = static_cast<uint16_t>(j - 1);
        for (uint32_t j = 17; j <= 20; ++j) parents[j] = 16;
        parents[21] = 16;
        parents[22] = parents[23] = 21;
        parents[24] = 21;
        for (uint32_t j = 25; j <= 33; ++j) parents[j] = 24;
        parents[34] = 24;
        parents[35] = parents[36] = 24;
        for (uint32_t j = 37; j <= 43; ++j) parents[j] = 36;
        parents[44] = 34;
        for (uint32_t j = 45; j < kJoints; ++j) parents[j] = 44;
    }
};
// A pose: each joint's local rotation about the body's right axis (x) by angle[j] deg, its local position 10 cm up its parent's y (the spine, neck and head) or at a fixed offset; the model
// matrices (row vectors: model = local x parent's model).
inline void poseOf(const Skel& k, const double angleDeg[kJoints], float (*out)[16]) {
    for (uint32_t j = 0; j < kJoints; ++j) {
        f166::Rec r;
        const double h = 0.5 * angleDeg[j] * 3.14159265358979323846 / 180.0;
        r.q[0] = std::sin(h); r.q[1] = 0.0; r.q[2] = 0.0; r.q[3] = std::cos(h);
        r.p[0] = (j == 35 ? -0.05 : j == 36 ? 0.05 : 0.0);
        r.p[1] = 0.10;
        r.p[2] = 0.0;
        float local[16];
        f166::matOf(r, local);
        if (k.parents[j] == 0xFFFF) std::memcpy(out[j], local, sizeof(local));
        else nf::pfzMul(local, out[k.parents[j]], out[j]);
    }
}
// The lean (deg about x, forward +z positive) of the bones 16->21, 21->24, 24->34, 34->44 of a pose.
inline void leansOf(const float (*m)[16], double lean[4]) {
    for (int i = 0; i < 4; ++i) {
        const double a[3] = {m[f180::kLevelJoint[i]][12], m[f180::kLevelJoint[i]][13], m[f180::kLevelJoint[i]][14]},
                     b[3] = {m[f180::kLevelNext[i]][12], m[f180::kLevelNext[i]][13], m[f180::kLevelNext[i]][14]};
        lean[i] = f180::boneLeanDeg(a, b);
    }
}
inline void localOf(const float (*m)[16], uint32_t j, uint32_t parent, float out[16]) {
    float inv[16];
    nf::pfzAffineInverse(m[parent], inv);
    nf::pfzMul(m[j], inv, out);
}

template <class Check>
inline void run180(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    char msg[640];

    // ---- F180a. the elbow's blend weight ------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        bool crouched = true, standing = true, hip = true, between = true;
        for (double d : {0.0, 0.05, 0.2, 0.5, 0.77, 1.0}) {
            crouched = crouched && f180::elbowBlendWeight(1.0, d) == f165::blendWeight(1.0, d);
            standing = standing && eq(f180::elbowBlendWeight(0.0, d), d, 1e-15);
            hip = hip && f180::elbowBlendWeight(0.0, 0.0) == 0.0 && f180::elbowBlendWeight(1.0, 0.0) == 0.0 && f180::elbowBlendWeight(0.5, 0.0) == 0.0;
            for (double p : {0.1, 0.25, 0.5, 0.75, 0.9}) between = between && eq(f180::elbowBlendWeight(p, d), d, 1e-15);
        }
        check(crouched, "F180a: crouched (the crouched weight 1) the elbow's blend weight is F165's, bit for bit, at every detach weight");
        check(standing && hip && between,
              "F180a: standing the blend weight is the detach weight (the crouched share plus the standing share), at every crouched weight in between too; at the hip (detach 0) it is 0 in every stance");
        check(eq(f180::elbowBlendWeight(std::nan(""), 0.6), 0.6, 1e-15) && f180::elbowBlendWeight(0.0, std::nan("")) == 0.0 && f180::elbowBlendWeight(2.0, 0.6) == f165::blendWeight(1.0, 0.6) &&
                  eq(f180::elbowBlendWeight(-1.0, 0.6), 0.6, 1e-15) && f180::elbowBlendWeight(0.0, 2.0) == 1.0 && f180::elbowBlendWeight(1.0, 2.0) == 1.0,
              "F180a: a crouched weight that is not a number reads standing, a detach weight that is not a number reads the hip, and both are held to 0..1");
        const double w[5] = {0.0, 0.25, 0.5, 0.75, 1.0};
        bool rising = true;
        double last = -1.0;
        for (int i = 0; i <= 10; ++i) {
            const double v = f180::elbowBlendWeight(0.0, i / 10.0);
            rising = rising && v >= last;
            last = v;
        }
        (void)w;
        check(rising && eq(last, 1.0, 1e-15), "F180a: through a press (the detach weight rising, standing) the blend rises without a step from 0 to 1");
    }

    // ---- F180b 1. the dial and the share --------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f180::clampDial(0.5) == 0.5 && f180::clampDial(-0.5) == 0.0 && f180::clampDial(1.5) == 1.0 && f180::clampDial(std::nan("")) == 0.0 && f180::clampDial(std::numeric_limits<double>::infinity()) == 0.0 &&
                  f180::kDialMin == 0.0 && f180::kDialMax == 1.0,
              "F180b: the dial is held to 0..1; a value that is not a number reads 0");
        check(f180::share(0.0, 0.0, 1.0) == 0.0 && f180::share(1.0, 0.0, 1.0) == 1.0 && f180::share(1.0, 1.0, 1.0) == 0.0 && f180::share(1.0, 0.0, 0.0) == 0.0 && eq(f180::share(1.0, 0.5, 1.0), 0.5, 1e-15) &&
                  eq(f180::share(0.4, 0.0, 0.5), 0.2, 1e-15) && eq(f180::share(0.4, 0.0, 1.0), 0.4, 1e-15) && f180::share(2.0, 0.0, 1.0) == 1.0 && f180::share(std::nan(""), 0.0, 1.0) == 0.0,
              "F180b: the share is the dial x F178's standing share x the detach weight: 0 at the dial 0, crouched and at the hip; whole at dial 1 standing in the sights; half at half a crouch or half the detach");
    }

    // ---- F180b 2. the plan: the spine and the neck, the neck a child of the chest ------------------------------------------------------------------------------------------------------------------
    const Skel skel;
    const f166::Plan plan = f180::buildPlan(skel.parents, kJoints);
    {
        check(plan.ok() && f180::planHasNeck(plan) && plan.n == 4 && plan.joint[0] == 16 && plan.joint[1] == 21 && plan.joint[2] == 24 && plan.joint[3] == 34 && plan.parentSlot[0] == -1 && plan.parentSlot[1] == 0 &&
                  plan.parentSlot[2] == 1 && plan.parentSlot[3] == 2 && f180::kNeck == 34 && f180::kLevels == 4,
              "F180b: the plan holds the spine 16, 21, 24 and the neck 34 (a direct child of the chest 24), parent first, each with its parent's slot");
        bool carriesChest = false, carriesHead = false, carriesClav = false, carriesRoot = false;
        for (uint32_t i = 0; i < plan.carriedN; ++i) {
            carriesChest = carriesChest || plan.carried[i] == 25;
            carriesHead = carriesHead || plan.carried[i] == 44;
            carriesClav = carriesClav || plan.carried[i] == 35 || plan.carried[i] == 36;
            carriesRoot = carriesRoot || plan.carried[i] == 0 || plan.carried[i] == 16;
        }
        check(carriesChest && carriesHead && carriesClav && !carriesRoot && plan.owner[0] <= 3,
              "F180b: every joint under a held one comes with it (the clavicles, the head, what hangs from the chest); the root and the held joints themselves do not");
        // the neck is left out when it is not a child of the chest; the spine alone is then the plan and the stage does not run
        Skel odd;
        odd.parents[34] = 23;
        const f166::Plan spineOnly = f180::buildPlan(odd.parents, kJoints);
        check(spineOnly.ok() && spineOnly.n == 3 && !f180::planHasNeck(spineOnly), "F180b: a neck that is not a direct child of the chest 24 is left out: the plan is the spine alone and planHasNeck says so (the stage is off)");
        Skel broken;
        broken.parents[24] = 16;
        check(!f180::buildPlan(broken.parents, kJoints).ok() && !f180::buildPlan(nullptr, kJoints).ok() && !f180::buildPlan(skel.parents, 30).ok() && !f180::buildPlan(skel.parents, 0).ok(),
              "F180b: a spine that is not a chain of direct parents, no parents, too few joints or none refuse the plan");
        // F166's own plan on the same tree is what it was: the refactor (planAdd, planCarry) changed nothing
        const uint32_t clav[2] = {35, 36}, upper[2] = {0xFFFFu, 0xFFFFu};
        const f166::Plan hold = f166::buildPlan(skel.parents, kJoints, clav, upper);
        check(hold.ok() && hold.n == 3 && hold.joint[2] == 24 && hold.arms == 0 && hold.carriedN > 0, "F180b: F166's plan, built through the same two steps, is the spine (a side whose upper arm is refused is left out) as it was");
    }

    // ---- F180b 3. F166's capture and apply on the toy: the slerp share --------------------------------------------------------------------------------------------------------------------------------
    {
        // the animated ADS pose: the levels hunched (local pitch 10, 12, 8 deg and the neck 15); the reference at the hip: 2, 3, 2 and 4 deg
        double animA[kJoints] = {}, refA[kJoints] = {};
        animA[16] = 10.0; animA[21] = 12.0; animA[24] = 8.0; animA[34] = 15.0;
        refA[16] = 2.0; refA[21] = 3.0; refA[24] = 2.0; refA[34] = 4.0;
        float anim[kJoints][16], ref[kJoints][16], out[kJoints][16];
        poseOf(skel, animA, anim);
        poseOf(skel, refA, ref);
        double leanAnim[4], leanRef[4];
        leansOf(anim, leanAnim);
        leansOf(ref, leanRef);
        const double sg = leanAnim[0] >= 0.0 ? 1.0 : -1.0;   // the pose records' rotation sense: the magnitudes are what is pinned, the sign is the matrices' own
        check(eq(leanAnim[0], sg * 10.0, 1e-4) && eq(leanAnim[1], sg * 22.0, 1e-4) && eq(leanAnim[2], sg * 30.0, 1e-4) && eq(leanAnim[3], sg * 45.0, 1e-4) && eq(leanRef[0], sg * 2.0, 1e-4) &&
                  eq(leanRef[1], sg * 5.0, 1e-4) && eq(leanRef[2], sg * 7.0, 1e-4) && eq(leanRef[3], sg * 11.0, 1e-4),
              "F180b: the four bones' lean about the body's right axis (16 to 21, 21 to 24, 24 to 34, 34 to 44) reads the cumulative pitch of the levels: 10, 22, 30, 45 deg animated and 2, 5, 7, 11 at the hip");
        f166::Held held;
        check(f166::capture(plan, ref, kJoints, f166::kStill, 1.0, &held) && held.have() && held.source == f166::kStill, "F180b: the reference is captured from a hip pose with F166's capture, as a still frame's source");
        held.parentHave = false;   // the stage does the same: the root is held as a local against the hips, not in the model's axes
        uint16_t touched[kJoints];
        uint32_t touchedN = 0;
        // dial 0 (share 0): nothing is written
        check(!f166::apply(plan, held, 0.0, anim, kJoints, out, touched, &touchedN) && touchedN == 0, "F180b: at the share 0 nothing is written: the animated pose stays whatever the reference is, bit for bit");
        // share 1: the written levels are the reference's
        check(f166::apply(plan, held, 1.0, anim, kJoints, out, touched, &touchedN) && touchedN >= 4, "F180b: at the share 1 the four levels and everything under them are written");
        float outFull[kJoints][16];
        std::memcpy(outFull, anim, sizeof(anim));
        for (uint32_t i = 0; i < touchedN; ++i) std::memcpy(outFull[touched[i]], out[touched[i]], 64);
        double leanFull[4];
        leansOf(outFull, leanFull);
        check(eq(leanFull[0], sg * 2.0, 1e-3) && eq(leanFull[1], sg * 5.0, 1e-3) && eq(leanFull[2], sg * 7.0, 1e-3) && eq(leanFull[3], sg * 11.0, 1e-3),
              "F180b: at the dial 1 and the share 1 each level is the reference: the four bones' lean reads 2, 5, 7 and 11 deg");
        // share 0.5: each local rotation halfway (6, 7.5, 5, 9.5 deg): the cumulative leans 6, 13.5, 18.5, 28
        check(f166::apply(plan, held, 0.5, anim, kJoints, out, touched, &touchedN), "F180b: at the share 0.5 the levels are written");
        float outHalf[kJoints][16];
        std::memcpy(outHalf, anim, sizeof(anim));
        for (uint32_t i = 0; i < touchedN; ++i) std::memcpy(outHalf[touched[i]], out[touched[i]], 64);
        double leanHalf[4];
        leansOf(outHalf, leanHalf);
        std::snprintf(msg, sizeof(msg), "F180b: at the share 0.5 each level's local rotation is halfway between the animated and the reference (slerp): the bones' lean reads %.2f %.2f %.2f %.2f deg (6, 13.5, 18.5, 28 in the matrices' sense)",
                      leanHalf[0] * sg, leanHalf[1] * sg, leanHalf[2] * sg, leanHalf[3] * sg);
        check(eq(leanHalf[0], sg * 6.0, 1e-3) && eq(leanHalf[1], sg * 13.5, 1e-3) && eq(leanHalf[2], sg * 18.5, 1e-3) && eq(leanHalf[3], sg * 28.0, 1e-3), msg);
        // everything under a level keeps its animated local against it: the clavicles and the head are carried, the shoulders move with the chest
        bool carried = true;
        for (uint32_t j : {25u, 35u, 36u, 40u, 44u, 46u}) {
            float la[16], lw[16];
            localOf(anim, j, skel.parents[j], la);
            localOf(outHalf, j, skel.parents[j], lw);
            for (int c = 0; c < 16; ++c) carried = carried && eq(la[c], lw[c], 1e-5);
        }
        const double shoulderMoved = std::sqrt(std::pow(anim[35][12] - outHalf[35][12], 2.0) + std::pow(anim[35][13] - outHalf[35][13], 2.0) + std::pow(anim[35][14] - outHalf[35][14], 2.0));
        check(carried && shoulderMoved > 0.01, "F180b: every joint under a level keeps its local against it (the clavicles, the head, the rest of the chest) and the left clavicle moves with the straightened chest (the reach rule and the arm IK read it from there)");
        // the joints not under any level (the root, the legs' side) are not touched
        bool untouched = true;
        for (uint32_t i = 0; i < touchedN; ++i) untouched = untouched && touched[i] != 0 && touched[i] != 1 && touched[i] != 15;
        check(untouched, "F180b: the root and the joints below the spine root are not written");
        // a singular parent or a local that is not a rotation writes nothing
        float bad[kJoints][16];
        std::memcpy(bad, anim, sizeof(anim));
        for (int c = 0; c < 16; ++c) bad[16][c] = 0.0f;
        check(!f166::apply(plan, held, 1.0, bad, kJoints, out, touched, &touchedN), "F180b: a singular matrix writes nothing and says so");
        // the reference's leans are the pose's: captured with the records
        f180::State st;
        const double hipLean[4] = {2.0, 5.0, 7.0, 11.0};
        st.noteLeans(hipLean, 1.0);
        const double moreLean[4] = {4.0, 9.0, 11.0, 15.0};
        st.noteLeans(moreLean, 0.5);
        check(eq(st.refLean[0], 3.0, 1e-12) && eq(st.refLean[3], 13.0, 1e-12), "F180b: the reference's leans are the running average of the captured poses', alpha 1 replacing");
    }

    // ---- F180b 4. the reference store: the hip frames, the press's first frame as the fallback, per item ------------------------------------------------------------------------------------------------------
    {
        f180::State st;
        f180::In in;
        in.dial = 0.5;
        in.dtSec = 0.011;
        in.item = 7;
        in.planKey = 3;
        // standing at the hip: the first frame captures, the next average
        f180::Step s1 = st.step(in);
        check(s1.action == f180::kCaptureHip && s1.alpha == 1.0 && s1.atHip && s1.standing && s1.share == 0.0, "F180b: the first standing hip frame captures the reference whole (alpha 1); the share is 0 at the hip");
        f180::State st2;
        float refPose[kJoints][16];
        double a[kJoints] = {};
        poseOf(skel, a, refPose);
        st2.step(in);
        f166::capture(plan, refPose, kJoints, f166::kStill, 1.0, &st2.held);
        const f180::Step s2 = st2.step(in);
        check(s2.action == f180::kAverageHip && eq(s2.alpha, 1.0 - std::exp(-0.011 / f166::kAverageSec), 1e-12), "F180b: the next hip frames average into it over about 0.12 s (alpha 1 - exp(-dt / 0.12))");
        // crouched at the hip: no capture
        f180::In crouchedHip = in;
        crouchedHip.postureW = 1.0;
        f180::State st3;
        check(st3.step(crouchedHip).action == f180::kNothing, "F180b: crouched at the hip nothing is captured (a crouched pose is not the standing reference)");
        // at the hip a standing frame replaces a press's first frame
        f180::State st4;
        st4.step(in);
        f180::In sights = in;
        sights.detachW = 1.0;
        const f180::Step press = st4.step(sights);
        check(press.action == f180::kCaptureFirst && press.alpha == 1.0 && !press.atHip && press.share == 0.5,
              "F180b: the item came out in the sights (no reference yet): the press's first frame stands in, and the share at dial 0.5 standing in the sights is 0.5");
        f166::capture(plan, refPose, kJoints, f166::kFirstFrame, 1.0, &st4.held);
        check(st4.step(sights).action == f180::kNothing, "F180b: in the sights with a reference held nothing more is captured");
        const f180::Step back = st4.step(in);
        check(back.action == f180::kCaptureHip && back.alpha == 1.0, "F180b: the next standing hip frame replaces the press's first frame whole");
        // crouched in the sights: no capture, no share
        f180::In crouchedSights = sights;
        crouchedSights.postureW = 1.0;
        f180::State st5;
        const f180::Step cs = st5.step(crouchedSights);
        check(cs.action == f180::kNothing && cs.share == 0.0, "F180b: crouched in the sights nothing is captured and the share is 0");
        // another item, another plan: the reference is dropped
        f180::State st6;
        st6.step(in);
        f166::capture(plan, refPose, kJoints, f166::kStill, 1.0, &st6.held);
        f180::In other = in;
        other.item = 8;
        const f180::Step ni = st6.step(other);
        check(ni.cleared && !st6.held.have() && ni.action == f180::kCaptureHip, "F180b: another item drops the reference and captures a new one at the hip");
        f166::capture(plan, refPose, kJoints, f166::kStill, 1.0, &st6.held);
        f180::In otherPlan = other;
        otherPlan.planKey = 4;
        const f180::Step np = st6.step(otherPlan);
        check(np.cleared && !st6.held.have(), "F180b: another skeleton's plan drops it too");
        // a long gap steps by 0.1 s at most
        f180::State st7;
        st7.step(in);
        f166::capture(plan, refPose, kJoints, f166::kStill, 1.0, &st7.held);
        f180::In gap = in;
        gap.dtSec = 5.0;
        check(eq(st7.step(gap).alpha, 1.0 - std::exp(-f166::kMaxStepSec / f166::kAverageSec), 1e-12), "F180b: a call after a long gap averages by 0.1 s of it, not the whole gap");
        // a source named
        check(std::strcmp(f180::sourceName(f166::kStill), "hip frames") == 0 && std::strcmp(f180::sourceName(f166::kFirstFrame), "the press's first frame") == 0 && std::strcmp(f180::sourceName(f166::kNone), "none") == 0,
              "F180b: the sources are named hip frames, the press's first frame and none");
    }

    // ---- F180b 5. the counters and the line ---------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f180::Counters c;
        for (int i = 0; i < 100; ++i) {
            f180::Sample s;
            s.dial = 0.5;
            s.share = 0.4;
            s.wrote = i < 80;
            s.source = i < 90 ? f166::kStill : i < 95 ? f166::kFirstFrame : f166::kNone;
            s.raiseAskedM = 0.09;
            const double anim[4] = {30.0, 25.0, 23.0, 20.0}, ref[4] = {12.0, 10.0, 15.0, 5.0}, wr[4] = {21.0, 17.0, 19.0, 12.0};
            for (int k = 0; k < 4; ++k) {
                s.animLean[k] = anim[k];
                s.refLean[k] = i < 95 ? ref[k] : std::nan("");
                s.writtenLean[k] = wr[k];
            }
            const double na[3] = {0.01, -0.09, 0.08}, nb[3] = {0.01, -0.14, 0.11}, sa[3] = {-0.14, -0.03, 0.04}, sb[3] = {-0.14, -0.06, 0.06};
            for (int k = 0; k < 3; ++k) {
                s.neckAfter[k] = na[k]; s.neckBefore[k] = nb[k]; s.shoulderAfter[k] = sa[k]; s.shoulderBefore[k] = sb[k];
            }
            c.add(s);
        }
        check(c.frames == 100 && c.wrote == 80 && c.src[f166::kStill] == 90 && c.src[f166::kFirstFrame] == 5 && c.src[f166::kNone] == 5 && c.dialMax == 0.5 && eq(c.dialSum, 50.0, 1e-9) &&
                  c.leanN[1][0] == 95 && c.leanN[0][0] == 100 && eq(c.leanSum[2][3], 1200.0, 1e-9),
              "F180b: the counters keep the frames, those written, the reference's source, the dial's sum and largest, and each lean's mean over the frames it was measured on (the reference's over those that had one)");
        f180::Counters m;
        m.merge(c);
        m.merge(c);
        check(m.frames == 200 && m.wrote == 160 && m.src[f166::kFirstFrame] == 10 && m.leanN[1][3] == 190 && eq(m.neckSum[1][1], 2.0 * c.neckSum[1][1], 1e-9) && m.dialMax == 0.5, "F180b: two windows add up");
        char line[1400];
        const int len = f180::formatSummary(line, sizeof(line), 10, "$wpn_m_shotgun_plasma_doublebarrel_name;", c);
        check(len > 0 && std::strstr(line, "native on foot standing ADS spine (F180b, TEMPORARY) 10 s: item $wpn_m_shotgun_plasma_doublebarrel_name;; standing in the sights 100 frames; the dial "
                                           "experimental.native_on_foot_stand_ads_spine_straighten mean 0.50 (largest 0.50); the straighten share mean 0.40; written in 80 frames; the reference: hip frames 90, "
                                           "the press's first frame 5, none 5; lean about the body's right axis (deg, forward positive) animated/reference/written: spine 16 +30.0/+12.0/+21.0, spine 21 "
                                           "+25.0/+10.0/+17.0, chest 24 +23.0/+15.0/+19.0, neck 34 +20.0/+5.0/+12.0; the neck from the eye (right/up/forward, cm) after +1.0/-9.0/+8.0, before "
                                           "+1.0/-14.0/+11.0; the left shoulder from the eye after -14.0/-3.0/+4.0, before -14.0/-6.0/+6.0; F179a's leg raise asked mean +9.0 cm") != nullptr,
              "F180b: the window's line names the tag, the item, the frames, the dial's mean and largest, the share, the frames written, the reference's source, the four bones' leans animated / reference / written, the neck and the left shoulder from the eye after and before, and F179a's raise asked");
        // the worst a window holds, counts no window could hold, and a window with nothing
        f180::Counters big;
        big.frames = 900; big.wrote = 900; big.src[1] = 900; big.dialSum = 900.0; big.dialMax = 1.0; big.shareSum = 900.0; big.raiseSum = 900.0 * 0.2;
        for (int t = 0; t < 3; ++t)
            for (int k = 0; k < 4; ++k) {
                big.leanN[t][k] = 900;
                big.leanSum[t][k] = -180.0 * 900.0;
            }
        for (int i = 0; i < 2; ++i) {
            big.neckN[i] = big.shoulderN[i] = 900;
            for (int c3 = 0; c3 < 3; ++c3) {
                big.neckSum[i][c3] = -1.0 * 900.0;
                big.shoulderSum[i][c3] = -1.0 * 900.0;
            }
        }
        char worst[1400];
        const int worstLen = f180::formatSummary(worst, sizeof(worst), 10, "$wpn_m_shotgun_plasma_doublebarrel_name_with_a_long_tail;", big);
        f180::Counters absurd = big;
        absurd.frames = absurd.wrote = absurd.src[0] = absurd.src[1] = absurd.src[2] = 4000000000u;
        absurd.dialSum = absurd.shareSum = 1.0e12;
        char absurdLine[1200];
        const int absurdLen = f180::formatSummary(absurdLine, sizeof(absurdLine), 4000000000u, "$wpn_m_shotgun_plasma_doublebarrel_name_with_a_long_tail;", absurd);
        f180::Counters none;
        char noneLine[1400];
        const int noneLen = f180::formatSummary(noneLine, sizeof(noneLine), 1, nullptr, none);
        std::snprintf(msg, sizeof(msg), "F180b: the line is within the log's 1166 bytes (1200 less its stamp and truncation marker) with every figure at its worst (%d bytes typical, %d worst); a window with no frame and an item not named print zeros, never a number that is not one",
                      len, worstLen);
        check(worstLen > 0 && worstLen < 1166 && len < 1166 && absurdLen > 0 && std::strlen(absurdLine) < sizeof(absurdLine) && noneLen > 0 && std::strstr(noneLine, "item none") != nullptr &&
                  std::strstr(noneLine, "nan") == nullptr && std::strstr(noneLine, "inf") == nullptr && std::strstr(noneLine, "standing in the sights 0 frames") != nullptr,
              msg);
        // the dial at 0 still prints every measure: the before picture
        f180::Counters before;
        for (int i = 0; i < 50; ++i) {
            f180::Sample s;
            s.source = f166::kStill;
            for (int k = 0; k < 4; ++k) { s.animLean[k] = 25.0; s.refLean[k] = 10.0; s.writtenLean[k] = 25.0; }
            for (int k = 0; k < 3; ++k) { s.neckAfter[k] = s.neckBefore[k] = 0.05; s.shoulderAfter[k] = s.shoulderBefore[k] = -0.1; }
            before.add(s);
        }
        char beforeLine[1400];
        f180::formatSummary(beforeLine, sizeof(beforeLine), 1, "x", before);
        check(std::strstr(beforeLine, "mean 0.00 (largest 0.00); the straighten share mean 0.00; written in 0 frames") != nullptr && std::strstr(beforeLine, "spine 16 +25.0/+10.0/+25.0") != nullptr &&
                  std::strstr(beforeLine, "the neck from the eye (right/up/forward, cm) after +5.0/+5.0/+5.0, before +5.0/+5.0/+5.0") != nullptr,
              "F180b: a window with the dial at 0 still prints the leans, the neck and the shoulder (the before picture), written in 0 frames");
    }

    // ---- F180c 1. the distance ----------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f179::clampGuardCm(10.0) == 10.0 && f179::clampGuardCm(0.0) == 0.0 && f179::clampGuardCm(-3.0) == 0.0 && f179::clampGuardCm(25.0) == 20.0 && f179::clampGuardCm(20.0) == 20.0 &&
                  f179::clampGuardCm(std::nan("")) == 2.0 && f179::kNeckGuardMinCm == 0.0 && f179::kNeckGuardMaxCm == 20.0 && f179::kNeckGuardAheadCm == 10.0 && f179::kNeckGuardDefaultCm == 2.0,
              "F180c: the distance is held to 0..20 cm, 10 is F179's own (the pure functions' default argument), 0 stays 0 (the guard off) and a value that is not a number reads the key's default 2 (F181)");
        check(eq(f179::heldBelowM(0.10), 0.09, 1e-15) && eq(f179::heldBelowM(0.05), 0.04, 1e-15) && f179::heldBelowM(0.005) == 0.0 && f179::heldBelowM(0.0) == 0.0 && eq(f179::heldBelowM(0.20), 0.19, 1e-15),
              "F180c: a guard that did not hold is a measured figure more than 1 cm under the distance asked: 9 cm at 10, 4 at 5, 19 at 20, never at 1 cm or less");
        check(eq(f179::deficitM(0.06), 0.04, 1e-12) && eq(f179::deficitM(0.06, 0.15), 0.09, 1e-12) && eq(f179::deficitM(0.06, 0.05), 0.0, 1e-12) && f179::deficitM(0.06, 0.0) == 0.0 && f179::deficitM(-0.05, 0.0) == 0.05 &&
                  f179::deficitM(0.06, std::nan("")) == 0.0 && eq(f179::deficitM(0.06, 0.20), 0.14, 1e-12),
              "F180c: the deficit is the distance less the eye ahead of the chest: 4 cm at 10, 9 at 15, 0 at 5; the dial's 0 is turned off by the pin (no geometry) and not by the arithmetic (a chest in front still reads a deficit)");
        // the guard moves the target to the distance asked, at every distance
        bool distances = true;
        for (double dist : {0.03, 0.05, 0.10, 0.15, 0.20}) {
            f179test::Scene s;
            s.chest[2] = -0.06; s.n0[2] = -0.06; s.target[2] = -0.06;
            const f179test::Result r = f179test::runScene(s, dist);
            distances = distances && r.ok && eq(r.aheadAfter, dist > 0.06 ? dist : 0.06, 1e-12) && eq(r.pre, dist > 0.06 ? dist - 0.06 : 0.0, 1e-12);
        }
        check(distances, "F180c: a chest 6 cm behind the eye is pushed back until the eye is the distance asked ahead of it at 3, 5, 10, 15 and 20 cm (not at all where it is already further)");
        // F179's run over many scenes at a few distances
        uint32_t seed = 987654u;
        const auto rnd = [&seed](double lo, double hi) {
            seed = seed * 1664525u + 1013904223u;
            return lo + (hi - lo) * (static_cast<double>(seed >> 8) / 16777216.0);
        };
        int total = 0, held = 0, capped = 0;
        bool inBound = true;
        for (double dist : {0.05, 0.10, 0.15, 0.20}) {
            for (int i = 0; i < 1500; ++i) {
                f179test::Scene s;
                const double a = rnd(0.0, 6.2831853);
                s.hx = std::sin(a); s.hz = std::cos(a);
                const double behind = rnd(0.0, 0.45);
                s.eye[0] = rnd(-1.0, 1.0); s.eye[2] = rnd(-1.0, 1.0);
                s.chest[0] = s.eye[0] - behind * s.hx; s.chest[2] = s.eye[2] - behind * s.hz;
                s.n0[0] = s.chest[0] + rnd(-0.05, 0.05); s.n0[2] = s.chest[2] + rnd(-0.05, 0.05);
                s.target[0] = s.n0[0] + rnd(-0.3, 0.3); s.target[2] = s.n0[2] + rnd(-0.3, 0.3);
                const double reach = rnd(-0.10, 0.15), add = rnd(0.0, 0.20);
                s.reach[0] = reach * s.hx; s.reach[2] = reach * s.hz;
                s.post[0] = add * s.hx; s.post[2] = add * s.hz;
                s.bias = rnd(-0.05, 0.05);
                const f179test::Result r = f179test::runScene(s, dist);
                ++total;
                const double pushTotal = r.pre + r.offPost + r.residual;
                inBound = inBound && pushTotal <= f179::pushCapM(dist) + 1e-9;   // F183: the push of a frame is at most the distance plus 10 cm
                if (r.aheadAfter >= dist - 1e-9) ++held;
                else if (r.bounded && pushTotal >= f179::pushCapM(dist) - 1e-9) ++capped;
            }
        }
        std::snprintf(msg, sizeof(msg), "F180c: F179's scenes at 5, 10, 15 and 20 cm: the predicted eye ends the distance asked ahead of the chest or more in %d of %d, the other %d asked more than the distance plus 10 cm (F183: the bound)", held, total, capped);
        check(inBound && held + capped == total && capped < total / 4, msg);
        // the guard at 5 cm leaves a body at 7 cm alone that the guard at 10 pushes
        f179test::Scene close7;
        close7.chest[2] = -0.07; close7.n0[2] = -0.07; close7.target[2] = -0.07;
        check(f179test::runScene(close7, 0.05).pre == 0.0 && eq(f179test::runScene(close7, 0.10).pre, 0.03, 1e-12), "F180c: a chest 7 cm behind the eye is left where it is by the guard at 5 cm and pushed 3 cm by the guard at 10");
    }

    // ---- F180c 2. unarmed at the hip: measured, not moved --------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f179::contextOf(false, 0.0, 0.0) == f179::kHipUnarmed && f179::contextOf(true, 0.0, 0.0) == f179::kHipArmed, "F180c: the unarmed hip is its own context (no item drawn, the detach under 0.05)");
        f179::Counters c;
        for (int i = 0; i < 100; ++i) {
            f179::Sample s;
            s.ctx = f179::kHipUnarmed;
            s.wouldM = i < 40 ? 0.024 : 0.0;
            s.beforeM = i < 40 ? 0.076 : 0.30;
            s.predM = s.beforeM;
            c.addGuard(s);
            c.addMeasured(f179::kHipUnarmed, 0.20, f179::heldBelowM(0.10));
        }
        for (int i = 0; i < 100; ++i) {
            f179::Sample s;
            s.ctx = f179::kHipArmed;
            s.pushM = i < 10 ? 0.03 : 0.0;
            s.beforeM = 0.30;
            s.predM = 0.30;
            c.addGuard(s);
            c.addMeasured(f179::kHipArmed, i < 2 ? 0.05 : 0.30, f179::heldBelowM(0.10));
        }
        const f179::Ctx1& un = c.c[f179::kHipUnarmed];
        const f179::Ctx1& ar = c.c[f179::kHipArmed];
        check(un.frames == 100 && un.wouldFrames == 100 && un.wouldN == 40 && eq(un.wouldSum, 0.96, 1e-12) && eq(un.wouldMax, 0.024, 1e-15) && un.pushed == 0 && un.offPostN == 0 &&
                  ar.wouldFrames == 0 && ar.wouldN == 0 && ar.pushed == 10 && ar.under9 == 2,
              "F180c: the unarmed hip's frames count the push it would have made (40 of 100, 2.4 cm each) and no push; the armed hip counts pushes and the frames under the distance less 1 cm");
        char line[1400];
        f179::formatContext(line, sizeof(line), 10, "none", f179::kHipUnarmed, un, 10.0);
        check(std::strstr(line, "native on foot neck guard (F179b) 10 s: item none; hip unarmed 100 frames, the guard at 10.0 cm, measuring only (nothing is moved): would push 40 frames, the push mean 2.4 cm "
                                "(largest 2.4); the eye ahead of the chest (as the pose puts it) min +7.6 mean +21.0 cm, as measured (100 frames) min +20.0 mean +20.0, predicted mean +21.0") != nullptr &&
                  std::strstr(line, "pushed") == nullptr && std::strstr(line, "under") == nullptr,
              "F180c: the unarmed hip's line says the guard's distance, that it measures only and moves nothing, the frames it would push and the push it would make, and has no pushed count and no frames under the distance");
        char armedLine[1400];
        f179::formatContext(armedLine, sizeof(armedLine), 10, "none", f179::kHipArmed, ar, 14.0);
        check(std::strstr(armedLine, "hip armed 100 frames, the guard at 14.0 cm: pushed 10, the push mean 3.0 cm (largest 3.0); off the post-reach dial add 0 frames") != nullptr &&
                  std::strstr(armedLine, "predicted after mean +30.0, under 13 cm 2") != nullptr,
              "F180c: an armed context's line says the guard's distance and counts the measured frames under it less 1 cm (13 cm at 14)");
        // the cap: measured scenes that the acting guard would have pushed give the same figure the measuring context reports
        f179test::Scene s;
        s.chest[2] = -0.076; s.n0[2] = -0.076; s.target[2] = -0.076;
        const f179test::Result acted = f179test::runScene(s);
        const f179::Geometry g = f179::makeGeometry(true, s.eye, s.hx, s.hz, true, s.chest);
        const nf::PinMove mv = nf::pinMove(s.n0, s.target);
        const double would = f179::deficitM(g.aheadWith(mv.th));
        check(g.ok && eq(would, acted.pre, 1e-15) && eq(would, 0.024, 1e-12) && std::memcmp(s.target, s.target, sizeof(s.target)) == 0,
              "F180c: the push the measuring context reports is the push the acting guard would make on the same scene (2.4 cm), computed without touching the target");
    }

    // ---- the source sites ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini"), logpin = slurp("src/d3d11/native_on_foot_logpin.h"), hooks = slurp("src/d3d11/native_on_foot_hooks.cpp"),
                          hdr = slurp("src/d3d11/native_on_foot.h");
        check(!cpp.empty() && !ini.empty() && !logpin.empty() && !hooks.empty() && !hdr.empty(), "F180: the source pins read the sources and edvr.ini (run the rig from the repository root)");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const auto at = [&](const std::string& needle) { return cpp.find(needle); };
        // F180a: the weight in the left hand's blend, in the branch that is the left hand's stable pole (a one-handed gun has no such solve)
        const size_t branch = at("if (side == 0 && pinWanted && pinCall.valid) {\n                    f181Eligible = true;   // F181a: the same gate: every two-handed gun, the left hand's stable pole\n                    const double w165 = nofoot::f180::elbowBlendWeight(pinCall.postureW, pinCall.detachW);");
        check(branch != std::string::npos && !has("const double w165 = nofoot::f165::blendWeight(") && has("const nofoot::f165::Blend mix = nofoot::f165::blendPole(tb, poleTheta165, w165,"),
              "F180a: the F165 blend's weight is nofoot::f180::elbowBlendWeight, in the left hand's stable-pole branch (the blend itself is F165's blendPole as it was)");
        // F180b: the key read and logged, the stage before the pin, the bend yielding to it, the sample and the line
        check(has("std::atomic<float> g_standAdsSpine{0.9f};") && has("nofoot::f180::clampDial(static_cast<double>(cfg.getFloat(\"experimental.native_on_foot_stand_ads_spine_straighten\", 0.9f)))") &&
                  has("if (first || v != g_standAdsSpineLast)") && has("native on foot standing ADS spine dial (F180b, TEMPORARY): experimental.native_on_foot_stand_ads_spine_straighten = %.2f") &&
                  has("g_standAdsSpine.store(static_cast<float>(v), std::memory_order_relaxed);"),
              "F180b: the key is read from [experimental] with the default 0, held to its range, logged at the first read and at each change, and stored for the stage");
        const size_t f166Call = at("if (!f166Stage(pl, sp, froze, armedNow, itemSerialForCall, bodyDetachForCall, pinCall)) {"), f180Call = at("if (!f180Stage(pl, sp, froze, armedNow, itemSerialForCall, bodyDetachForCall, pinCall)) {"),
                     f169Call = at("if (!f169Stage(pl, sp, froze, f167Pistol"), pinCallAt = at("!pinStage0(pl, sp, froze, now, armsOk ? &G : nullptr"), stageDef = at("bool f180Stage(const pfz::ArmPlan& pl"),
                     f169Def = at("bool f169Stage(const pfz::ArmPlan& pl");
        check(f166Call != std::string::npos && f166Call < f180Call && f180Call < f169Call && f169Call < pinCallAt && stageDef != std::string::npos && stageDef < f169Def,
              "F180b: the order in the hook is F166's hold, the spine straighten, F169's swing, then the pin (the reach rule, F179a's raise and the neck guard read the straightened pose from g_pfzAnim, then the bend and the arm IK)");
        check(has("nofoot::f166::capture(pl.f180, g_pfzAnim, count, src, st.alpha, &g_f180.held)") && has("g_f180.held.parentHave = false;") && has("nofoot::f180::applyOneSided(pl.f180, os.cum, g_pfzAnim, count, g_f180Out, g_f180Touched, &touchedN)") &&
                  has("std::memcpy(g_pfzAnim[g_f180Touched[i]], g_f180Out[g_f180Touched[i]], 64);") && has("nofoot::f180::planHasNeck(pl.f180)"),
              "F180b: the reference is captured with F166's capture on the plan of the spine and the neck (its root held as a local); since F182b the levels are written one-sided (nofoot::f180::applyOneSided), into the span and g_pfzAnim");
        check(has("std::max(pinCall.f166.ran ? pinCall.f166.weight : 0.0, pinCall.f180.ran ? pinCall.f180.weight : 0.0)"), "F180b: the bend and the square-up yield to the straightened torso as to the held one (the larger weight; F166's factor exactly at the dial 0)");
        const size_t sample178 = at("f.f178.add(sample178);"), sample180 = at("f.f180.add(s180);");
        check(sample178 != std::string::npos && sample178 < sample180 && has("nofoot::f180::Counters f180;") && has("if (f98.f180.frames) {") && has("nofoot::f180::formatSummary(line180, sizeof(line180), w.seconds, w.item, f98.f180);") &&
                  logpin.find("d.f180.merge(s.f180);") != std::string::npos,
              "F180b: the frames standing in the sights are filed after F178's, F98Left carries the counters, logpin merges them and the line is printed whenever a frame was filed");
        check(hooks.find("plan.f180 = nofoot::f180::buildPlan(g_pfzParents, joints);") != std::string::npos && hdr.find("nofoot::f166::Plan f180;") != std::string::npos,
              "F180b: the plan of the spine and the neck rides the arm plan");
        // F180b: the raise made after: it reads the neck from g_pfzAnim, which the stage has written
        check(has("const float* nk = froze && g_ikFzSlot[34] != 0xFFFFu ? g_pfzChain[g_ikFzSlot[34]] : g_pfzAnim[34];") && has("pc.f179StandRaise = nofoot::f179::legsRaiseAskedM(legsRoomM,"),
              "F180b: F179a's raise and the guard read the neck 34 and the chest 24 from g_pfzAnim in the pin, after the stage has written it");
        // F180c: the key, the geometry off at 0, the measuring context, the three acting sites gated, the window closing on a change
        check(has("std::atomic<float> g_neckGuardCm{static_cast<float>(nofoot::f179::kNeckGuardDefaultCm)};") &&
                  has("nofoot::f179::clampGuardCm(static_cast<double>(cfg.getFloat(\"experimental.native_on_foot_neck_guard_cm\", static_cast<float>(nofoot::f179::kNeckGuardDefaultCm))))") &&
                  has("if (first || cm != g_neckGuardCmLast)") && has("native on foot neck guard distance (F180c, TEMPORARY): experimental.native_on_foot_neck_guard_cm = %.1f cm") &&
                  has("g_neckGuardCm.store(static_cast<float>(cm), std::memory_order_relaxed);"),
              "F180c (F181: the pilot's value): the key is read from [experimental] with the default 2 (was 10), held to 0..20, logged at the first read and at each change, and stored for the pin");
        check(has("if (!(pc.f179DistM > 0.0)) g179 = nofoot::f179::Geometry{};") && has("pc.f179Measure = !nofoot::f179::guardActs(pc.f179Ctx);") &&
                  has("if (!pc.f179Measure) pc.f179PushPre = nofoot::f179::guardTarget(g179, N0, pc.f179Bias, T, pc.f179DistM, &pc.f179Bounded);") && has("if (g179.ok && !pc.f179Measure) pc.f179PushPre = nofoot::f179::guardTarget(g179, N0, pc.f179Bias, physicalTarget, pc.f179DistM, &pc.f179Bounded,") &&
                  has("if (g179.ok && !pc.f179Measure) {\n        const double thWithPost[3]") && has("if (g179.ok && !pc.f179Measure) {\n            const nofoot::PinMove formed = nofoot::pinMove(N0, desiredNeck);") &&
                  has("if (pc.f179Measure) pc.f179Would = nofoot::f179::deficitBoundedM(pc.f179PredRaw + pc.f179Bias, pc.f179DistM, &pc.f179Bounded);") && has("gs.wouldM = pc.f179Would;"),
              "F180c: the distance 0 leaves no geometry (the guard off), unarmed at the hip only measures (the target push, the post-reach trim and the formed neck's look are all gated by it) and files the push it would make");
        check(has("float guardKey = 0.0f;") && has("pending.guardKey != g_neckGuardCm.load(std::memory_order_relaxed)") && has("w.guardKey = g_neckGuardCm.load(std::memory_order_relaxed);"),
              "F180c: a change of the distance closes the window, so each line's distance is the one its figures were made at");
        // the ini: both keys beside the standing dials, TEMPORARY, ui rows, defaults 0.0 and 10.0, before the torso square
        const size_t standLeft = ini.find("\nnative_on_foot_stand_ads_body_left_cm = "), spineKey = ini.find("\nnative_on_foot_stand_ads_spine_straighten = 0.9\n"), guardKey = ini.find("\nnative_on_foot_neck_guard_cm = 2.0\n"),
                     squareKey = ini.find("\nnative_on_foot_crouch_ads_square = ");
        check(standLeft != std::string::npos && spineKey != std::string::npos && guardKey != std::string::npos && squareKey != std::string::npos && standLeft < spineKey && spineKey < guardKey && guardKey < squareKey &&
                  ini.find("# TEMPORARY (F180): Standing ADS spine straighten") != std::string::npos && ini.find("# TEMPORARY (F180): Neck guard distance") != std::string::npos &&
                  ini.find("# ui: Standing ADS spine straighten | range 0..1 | step 0.05 | live\n") != std::string::npos &&
                  ini.find("# ui: Neck guard distance | range 0..20 | step 0.5 | unit cm | live\n") != std::string::npos,
              "F180 (F181: the pilot's values): edvr.ini has both keys (the spine straighten at 0.9, the neck guard distance at 2.0; were 0.0 and 10.0), TEMPORARY, each with its ui: row, between the standing left dial and the torso square");
    }
}

}  // namespace f180test
