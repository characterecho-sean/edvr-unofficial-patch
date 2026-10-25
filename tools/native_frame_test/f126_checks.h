// F126 checks (native on-foot VR): three review findings (native_on_foot_f126.h, native_on_foot_core.h). Header only, run by native_frame_test (run126).
// 1. The body pin's eye pair: the two eyes' scene rows are taken as one pair only when they carry the same frame. The reviewer's case is a head at 4 m/s on a 90 Hz
//    frame clock whose Animate call falls between the two publications: the rows of two frames, one apart, show 78 mm between the eyes (inside the 50 to 80 mm
//    window) and put the midpoint 22 mm behind where it is.
// 2. The arm IK's FP snapshot: after a skeleton change the snapshot of the earlier rig is not used by either target path (the anatomical one was already
//    refused; the attach fallback kept pulling the hands to the earlier rig's grips) until the next FP call replaces it.
// 3. A nearly straight elbow with no learned hinge reaches a reachable grip exactly on both sides of the hinge window (the sine of the bend over 0.05), where the
//    shortest-arc stand-in missed it by up to 14 mm just under the window.
#pragma once

#include "../../src/d3d11/native_on_foot_f126.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace f126test {

namespace nf = edvr::nofoot;
namespace h = edvr::nofoot::f126;

inline double distMm(const double a[3], const double b[3]) {
    const double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return nf::ikLen(d) * 1000.0;
}

// ---- 1. the eye pair ------------------------------------------------------------------------------------------------------------------------------
// One eye's scene rows (worldClip's layout: clip_j = (p - o) . rows[4 i + j] + rows[12 + j], j = x, y, z, w): x and y scaled, w the depth along the forward axis.
inline void eyeRows(const double eye[3], const double o[3], const double r[3], const double u[3], const double f[3], float rows[16]) {
    const double sx = 0.9, sy = 1.1, sz = 1.0;
    for (int i = 0; i < 3; ++i) {
        rows[4 * i + 0] = static_cast<float>(sx * r[i]);
        rows[4 * i + 1] = static_cast<float>(sy * u[i]);
        rows[4 * i + 2] = static_cast<float>(sz * f[i]);
        rows[4 * i + 3] = static_cast<float>(f[i]);
    }
    const double rel[3] = {eye[0] - o[0], eye[1] - o[1], eye[2] - o[2]};
    rows[12] = static_cast<float>(-sx * nf::ikDot(rel, r));
    rows[13] = static_cast<float>(-sy * nf::ikDot(rel, u));
    rows[14] = static_cast<float>(-sz * nf::ikDot(rel, f));
    rows[15] = static_cast<float>(-nf::ikDot(rel, f));
}

// A head moving along its forward axis, yawed 25 degrees, eyes 64 mm apart: the eyes' positions at a frame (frame k is k steps from frame 0).
struct Head {
    double yaw = 25.0 * nf::kIkPi / 180.0, speed = 4.0, hz = 90.0, ipd = 0.064;
    double r[3] = {}, u[3] = {0.0, 1.0, 0.0}, f[3] = {};
    Head() {
        f[0] = std::sin(yaw);
        f[1] = 0.0;
        f[2] = std::cos(yaw);
        r[0] = std::cos(yaw);
        r[1] = 0.0;
        r[2] = -std::sin(yaw);
    }
    double step() const { return speed / hz; }
    void eye(int e, int frame, double out[3]) const {
        const double base[3] = {12.0, 1.7, -30.0};
        for (int i = 0; i < 3; ++i) out[i] = base[i] + f[i] * step() * frame + r[i] * (e == 0 ? -0.5 : 0.5) * ipd;
    }
};
// The pair the Animate hook reads: eye 0 from frame f0, eye 1 from frame f1, the frame counter at cur.
inline h::Pair readPair(const Head& head, int f0, int f1, uint32_t cur, uint32_t tag0, uint32_t tag1, h::Cyclo* out) {
    const double o[3] = {10.0, 2.0, -25.0};
    float rows[2][16], oo[2][4] = {};
    for (int e = 0; e < 2; ++e) {
        double eye[3];
        head.eye(e, e == 0 ? f0 : f1, eye);
        eyeRows(eye, o, head.r, head.u, head.f, rows[e]);
        for (int i = 0; i < 3; ++i) oo[e][i] = static_cast<float>(o[i]);
    }
    const bool read[2] = {true, true};
    const uint32_t fr[2] = {tag0, tag1};
    return h::cycloFromRows(read, fr, rows, oo, cur, out);
}

// ---- 3. the elbow ---------------------------------------------------------------------------------------------------------------------------------
struct Lcg {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    double next() {   // 0..1
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(s >> 11) / 9007199254740992.0;
    }
    void unit(double v[3]) {
        for (;;) {
            v[0] = 2.0 * next() - 1.0;
            v[1] = 2.0 * next() - 1.0;
            v[2] = 2.0 * next() - 1.0;
            const double l = nf::ikLen(v);
            if (l > 0.1 && l < 1.0) {
                for (int i = 0; i < 3; ++i) v[i] /= l;
                return;
            }
        }
    }
};
// An animated arm bent by `bendDeg` about a hinge perpendicular to the upper arm, a reachable target and a swivel, solved with the given hints.
struct ElbowCase {
    bool ok = false;
    int source = -1;
    double missMm = 0.0;
};
inline ElbowCase solveElbow(Lcg& rng, double bendDeg, bool withLast) {
    ElbowCase c;
    const double S[3] = {0.1, 1.2, 0.2}, l1 = 0.30, l2 = 0.27;
    double u0[3], t[3], n0[3];
    rng.unit(u0);
    rng.unit(t);
    const double d0 = nf::ikDot(t, u0);
    const double pr[3] = {t[0] - d0 * u0[0], t[1] - d0 * u0[1], t[2] - d0 * u0[2]};
    if (!nf::ikUnit(pr, n0)) return c;
    double Rf[9], h0[3], E0[3], H0[3];
    nf::ikRotAxis(n0, bendDeg * nf::kIkPi / 180.0, Rf);
    nf::ikRowRot(u0, Rf, h0);
    for (int i = 0; i < 3; ++i) {
        E0[i] = S[i] + l1 * u0[i];
        H0[i] = E0[i] + l2 * h0[i];
    }
    double dir[3], T[3];
    rng.unit(dir);
    const double dist = (0.45 + 0.50 * rng.next()) * (l1 + l2);
    for (int i = 0; i < 3; ++i) T[i] = S[i] + dist * dir[i];
    const double ref[3] = {E0[0] - S[0], E0[1] - S[1], E0[2] - S[2]};
    const nf::IkTwoBone tb = nf::ikTwoBoneFrame(S, T, l1, l2, ref);
    if (!tb.ok || tb.limited) return c;
    double E[3];
    tb.elbowAt(tb.angleOf(ref) + (2.0 * rng.next() - 1.0) * 2.5, E);
    double last[3];
    rng.unit(last);
    const nf::HingeSwings hs = nf::ikHingeSwings(S, E0, H0, E, tb.hand, nullptr, withLast ? last : nullptr);
    if (!hs.ok) return c;
    double hv[3] = {H0[0] - S[0], H0[1] - S[1], H0[2] - S[2]}, hr[3], rel[3], out[3];
    nf::ikRowRot(hv, hs.R1, hr);
    for (int i = 0; i < 3; ++i) rel[i] = hr[i] + S[i] - E[i];
    nf::ikRowRot(rel, hs.R2, out);
    const double miss[3] = {E[0] + out[0] - tb.hand[0], E[1] + out[1] - tb.hand[1], E[2] + out[2] - tb.hand[2]};
    c.ok = true;
    c.source = hs.source;
    c.missMm = nf::ikLen(miss) * 1000.0;
    return c;
}
struct ElbowSweep {
    int cases = 0, wrongSource = 0, bySource[4] = {};
    double worstMm = 0.0, meanMm = 0.0;
};
inline ElbowSweep sweepElbow(double bendDeg, bool withLast, int wantSource, int n = 600) {
    ElbowSweep s;
    Lcg rng;
    for (int i = 0; i < n; ++i) {
        const ElbowCase c = solveElbow(rng, bendDeg, withLast);
        if (!c.ok) continue;
        ++s.cases;
        s.worstMm = std::max(s.worstMm, c.missMm);
        s.meanMm += c.missMm;
        if (c.source != wantSource) ++s.wrongSource;
        if (c.source >= 0 && c.source < 4) ++s.bySource[c.source];
    }
    if (s.cases) s.meanMm /= s.cases;
    return s;
}

template <class Check>
inline void run126(Check&& check) {
    // ---- 1. the eye pair ----
    {
        const Head head;
        const uint32_t N = 100;
        h::Cyclo same, mixed, mixedOld, forged;
        const h::Pair a = readPair(head, 1, 1, N + 1, N + 1, N + 1, &same);
        const double truth[3] = {0.5 * head.ipd * head.r[0], 0.5 * head.ipd * head.r[1], 0.5 * head.ipd * head.r[2]};
        const double errSame = distMm(same.off, truth);
        check(a == h::Pair::kUse && errSame < 0.01,
              "F126: both eyes' rows of one frame are one pair: the offset to the midpoint of the eyes is half the IPD along the eyes' right, within 0.01 mm");
        // the reviewer's case: the left eye's rows of frame N + 1 beside the right eye's of frame N, 4 m/s at 90 Hz
        for (int i = 0; i < 3; ++i) mixed.off[i] = 7.0;
        const h::Pair b = readPair(head, 1, 0, N + 1, N + 1, N, &mixed);
        check(b == h::Pair::kFrames && mixed.off[0] == 7.0 && mixed.off[1] == 7.0 && mixed.off[2] == 7.0,
              "F126: the left eye's rows of frame N + 1 beside the right eye's of frame N are refused as rows of two frames, and the last good pair is left as it was");
        const h::Pair c = readPair(head, 0, 1, N + 1, N, N + 1, &mixedOld);
        check(c == h::Pair::kFrames, "F126: the same with the right eye's rows the newer ones");
        // what the refused pair would have done: the frames forged equal, the two eyes' rows as they are
        const h::Pair d = readPair(head, 1, 0, N + 1, N + 1, N + 1, &forged);
        const double errMixed = distMm(forged.off, truth);
        const double along = (forged.off[0] - truth[0]) * head.f[0] + (forged.off[1] - truth[1]) * head.f[1] + (forged.off[2] - truth[2]) * head.f[2];
        check(d == h::Pair::kUse && std::fabs(errMixed - 22.2) < 0.2 && std::fabs(along * 1000.0 + 22.2) < 0.2,
              "F126: the reviewer's number: a pair taken from rows one frame apart at 4 m/s and 90 Hz passes the IPD window (78 mm) and puts the midpoint 22 mm behind "
              "where it is, along the head's motion");
        h::Cyclo staleOut;
        const h::Pair e = readPair(head, 1, 1, N + 20, N + 1, N + 1, &staleOut);
        check(e == h::Pair::kStale, "F126: rows more than 8 frames behind the counter are stale, as before");
        const bool unread[2] = {false, true};
        const uint32_t fr[2] = {N, N};
        float rows[2][16] = {}, oo[2][4] = {};
        h::Cyclo unreadOut;
        check(h::cycloFromRows(unread, fr, rows, oo, N, &unreadOut) == h::Pair::kUnread, "F126: an eye whose read was torn is no pair");
        // equal frames over a long run: every pair of a steady stream is taken; a stream whose eyes interleave is refused whenever they differ
        int taken = 0, refused = 0;
        for (int k = 2; k < 200; ++k) {
            h::Cyclo o2;
            const int left = k, right = (k % 3 == 0) ? k - 1 : k;   // one frame in three the right eye's rows are still the last frame's
            const h::Pair p = readPair(head, left, right, static_cast<uint32_t>(left), static_cast<uint32_t>(left), static_cast<uint32_t>(right), &o2);
            if (p == h::Pair::kUse) ++taken;
            if (p == h::Pair::kFrames) ++refused;
        }
        check(taken == 132 && refused == 66,
              "F126: a stream in which the right eye's rows lag a frame on every third call: the two thirds that match are taken, the third that does not is refused");
    }
    // ---- 2. the FP snapshot's rig ----
    {
        check(h::snapshotRigCurrent(4, 0x1000, 0x1000, 4) && !h::snapshotRigCurrent(5, 0x1000, 0x1000, 5) && !h::snapshotRigCurrent(6, 0x2000, 0x1000, 4) &&
                  !h::snapshotRigCurrent(6, 0x1000, 0x1000, 4) && !h::snapshotRigCurrent(6, 0x2000, 0x1000, 6) && h::snapshotRigCurrent(6, 0x2000, 0x2000, 6) &&
                  !h::snapshotRigCurrent(6, 0x2000, 0x2000, 4) && !h::snapshotRigCurrent(6, 0, 0x2000, 6),
              "F126: a snapshot is of the current rig only under an even plan counter, from the plan's X, under the plan's generation (not while the plan is written, not "
              "after a new X, not after a new generation of the same X, not when the plan's X is cleared)");
        // A skeleton change as the 3P update sees it: the FP plan moves from rig A (X 0x1000, generation 4) to rig B (X 0x2000, generation 6) through a write (5); the
        // snapshot is of rig A until the FP call after the change replaces it. Each update uses the snapshot's grips if it may. The grip of rig A and of rig B differ.
        struct Step { uint32_t planSeq; uint64_t planX; uint64_t snapX; uint32_t snapGen; bool ageUsable; };
        const Step steps[] = {
            {4, 0x1000, 0x1000, 4, true},   // before the change
            {4, 0x1000, 0x1000, 4, true},
            {5, 0x0000, 0x1000, 4, true},   // the plan is being written
            {6, 0x2000, 0x1000, 4, true},   // rig B's plan is out; the 3P update comes before the FP call: the snapshot is of rig A
            {6, 0x2000, 0x1000, 4, true},
            {6, 0x2000, 0x2000, 6, true},   // the FP call has replaced the snapshot
            {6, 0x2000, 0x2000, 6, true},
            {6, 0x2000, 0x2000, 6, false},  // and the snapshot is old: refused for its age, whatever its rig
        };
        const double gripA[3] = {0.12, -0.31, 0.44}, gripB[3] = {0.19, -0.27, 0.40};   // the attach's place in the 3P model for each rig, metres
        int used = 0, refused = 0;
        double worstPullMm = 0.0;
        for (const Step& s : steps) {
            const bool rigCurrent = h::snapshotRigCurrent(s.planSeq, s.planX, s.snapX, s.snapGen);
            if (!h::armsMayUseSnapshot(s.ageUsable, rigCurrent)) {
                ++refused;
                continue;
            }
            ++used;
            // the snapshot's grip is rig A's when the snapshot is of X 0x1000; the rig that is current is B once the plan says so
            const double* snap = s.snapX == 0x1000 ? gripA : gripB;
            const double* want = s.planSeq >= 6 ? gripB : gripA;
            const double miss[3] = {snap[0] - want[0], snap[1] - want[1], snap[2] - want[2]};
            worstPullMm = std::max(worstPullMm, nf::ikLen(miss) * 1000.0);
        }
        check(used == 4 && refused == 4 && worstPullMm < 1e-9,
              "F126: through a skeleton change the arm IK uses the snapshot only while it is of the rig the plan names: the update with the plan being written and the two "
              "before the FP call refuse, and none pulls a hand to the earlier rig's grip (without the rig test the attach fallback used seven of the eight, two of them 90 mm "
              "from the rig's grip); an old snapshot refuses whatever its rig");
        check(h::armsMayUseSnapshot(true, true) && !h::armsMayUseSnapshot(false, true) && !h::armsMayUseSnapshot(true, false) && !h::armsMayUseSnapshot(false, false),
              "F126: the arm IK reads the snapshot's matrices (the anatomical retarget and the attach fallback alike) only when it is fresh and of the current rig");
    }
    // ---- 3. the elbow ----
    {
        // the window's edge: asin(0.05) = 2.866 degrees
        const double edge = std::asin(nf::kHingeMinSin) * 180.0 / nf::kIkPi;
        check(std::fabs(edge - 2.866) < 0.001, "F126: the hinge window closes at a bend whose sine is 0.05, 2.866 degrees");
        const ElbowSweep below = sweepElbow(2.859, false, 2), above = sweepElbow(2.871, false, 0);
        check(below.cases > 400 && below.wrongSource == 0 && above.cases > 400 && above.wrongSource == 0,
              "F126: a bend of 2.859 degrees with no hint takes the shortest-arc stand-in (source 2) and 2.871 degrees the animated hinge (source 0): the two sides of the window");
        check(below.worstMm < 1e-6 && above.worstMm < 1e-6,
              "F126: the hand reaches a reachable grip exactly on both sides of the hinge window (600 random arms, targets and swivels each): 2.859 and 2.871 degrees "
              "within a nanometre");
        const ElbowSweep last = sweepElbow(2.859, true, 1);
        check(last.cases > 400 && last.bySource[1] * 10 > last.cases * 9 && last.bySource[0] == 0 && last.bySource[3] == 0 && last.worstMm < 1e-6,
              "F126: with only the last solved hinge to go on (source 1; the shortest arc where the hint lies along the upper arm) a 2.859 degree bend reaches the grip "
              "exactly too");
        double worst = 0.0;
        int steps = 0, wrong = 0;
        for (double bend = 0.0; bend <= 6.0001; bend += 0.25) {
            const ElbowSweep s = sweepElbow(bend, false, std::sin(bend * nf::kIkPi / 180.0) > nf::kHingeMinSin ? 0 : 2, 80);
            worst = std::max(worst, s.worstMm);
            wrong += s.wrongSource;
            ++steps;
        }
        check(steps == 25 && wrong == 0 && worst < 1e-6,
              "F126: from a straight arm to a 6 degree bend in quarter degrees, the stand-in below the window and the animated hinge above it all reach the grip exactly");
        // the edge itself in steps of two thousandths of a degree, 2.84 to 2.89: the stand-in on the near side, the animated hinge on the far side, no step in the miss
        double edgeWorst = 0.0;
        int edgeSteps = 0, edgeNear = 0, edgeFar = 0, edgeWrong = 0;
        for (int k = 0; k <= 25; ++k) {
            const double bend = 2.84 + 0.002 * k;
            const int want = std::sin(bend * nf::kIkPi / 180.0) > nf::kHingeMinSin ? 0 : 2;
            const ElbowSweep s = sweepElbow(bend, false, want, 60);
            edgeWorst = std::max(edgeWorst, s.worstMm);
            edgeWrong += s.wrongSource;
            (want == 0 ? edgeFar : edgeNear)++;
            ++edgeSteps;
        }
        check(edgeSteps == 26 && edgeNear > 0 && edgeFar > 0 && edgeWrong == 0 && edgeWorst < 1e-6,
              "F126: across the window's edge in steps of 0.002 degree (2.84 to 2.89, both branches met) the hand misses the grip by under a nanometre at every step");
        // an arm exactly straight, with a row to go on or none: as before, and the hand lands
        Lcg rng;
        double straightWorst = 0.0;
        for (int i = 0; i < 100; ++i) {
            const ElbowCase c = solveElbow(rng, 0.0, i % 2 == 1);
            if (c.ok) straightWorst = std::max(straightWorst, c.missMm);
        }
        check(straightWorst < 1e-6, "F126: an exactly straight arm still reaches the grip exactly, with the last hinge to go on or without");
    }
}

}  // namespace f126test
