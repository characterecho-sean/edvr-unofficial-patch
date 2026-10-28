// F143 checks (native on-foot VR), run143 below. Header only, run by native_frame_test.
//
// F143 (native_on_foot_f143.h): (1) the ADS eye's shift taken from the game's own sight line E -> S (read by F141's hook, expressed relative to P14w in the FP root's axes,
// filtered per activation, published with its time, item and scope): Q is the point on the line at the lock's eye depth k along the root's forward from the origin, the game's
// shift is Q less (origin + k f) less its part along f, the two live trims are added, and F139's 8 cm bound stands; a line that cannot be used falls back to F139's table with the
// reason counted. (2) the coat's clip modifiers: the parts the unclip A/B adds (everything but the head parts and F19's list), and when the measure reads.
#pragma once

#include "../../src/d3d11/avatar_hooks_core.h"
#include "../../src/d3d11/native_on_foot_f143.h"
#include "f141_checks.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace f143test {

namespace f143 = edvr::nofoot::f143;
namespace f139 = edvr::nofoot::f139;

inline bool near143(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
inline double dot143(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// A pair for the held item: root coordinates in cm for readability.
inline f143::Pub pair143(const double e[3], const double s[3], uint64_t ms, uint32_t held = 7, int scope = 1, uint32_t activation = 3) {
    f143::Pub p;
    p.valid = true;
    p.ms = ms;
    p.key.held = held;
    p.key.coherent = true;
    p.key.scope = scope;
    p.key.activation = activation;
    for (int i = 0; i < 3; ++i) {
        p.e[i] = 0.01 * e[i];
        p.s[i] = 0.01 * s[i];
    }
    return p;
}

template <class Check>
inline void run143(Check&& check) {
    // ---- the flight's laser SMG: E (-0.50, -4.61, +14.93), S (-0.46, -4.63, +35.9) cm from P14 in the root's axes, k 21 cm: up -4.62, right -0.49
    const double eCm[3] = {-0.50, -4.61, 14.93}, sCm[3] = {-0.46, -4.63, 35.9};
    {
        const double p14w[3] = {10.0, 1.6, -5.0}, up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
        const f143::Pub pub = pair143(eCm, sCm, 1000);
        const f143::Choice c = f143::choose(true, true, true, 7, 1, pub, 1100, p14w, up, right, fwd, p14w, 0.21, 0.0, 0.0);
        check(c.game && c.why == f143::kGame && near143(c.gameUpCm, -4.62, 0.005) && near143(c.gameRightCm, -0.49, 0.005),
              "F143: the flight's laser SMG: E (-0.50, -4.61, +14.93) and S (-0.46, -4.63, +35.9) cm from P14, k 21 cm: the game's shift is up -4.62, right -0.49 cm");
        check(near143(c.shift[0], -0.0049, 5e-5) && near143(c.shift[1], -0.0462, 5e-5) && near143(c.shift[2], 0.0, 1e-12) && near143(c.shiftM, std::sqrt(0.0049 * 0.0049 + 0.0462 * 0.0462), 1e-4),
              "F143: ... as a world shift on a level root (metres), nothing along the forward");
        const double qRel[3] = {c.q[0] - p14w[0], c.q[1] - p14w[1], c.q[2] - p14w[2]};
        check(near143(dot143(qRel, fwd), 0.21, 1e-9), "F143: Q lies at the lock's eye depth along the forward (dot(Q - origin, f) = k)");
        const double lineDeg = std::atan2(std::sqrt(0.0004 * 0.0004 + 0.0002 * 0.0002), 0.2097) * 180.0 / 3.14159265358979323846;
        check(near143(c.ageMs, 100.0, 1e-9) && near143(c.lineDeg, lineDeg, 1e-6) && c.lineDeg > 0.1 && c.lineDeg < 0.2,
              "F143: the pair's age (100 ms) and the line's angle off the forward (0.12 deg) are the figures the line prints");
    }
    {   // a turned, pitched root and P14 elsewhere: the same root coordinates give the same shift in the root's axes
        float froot[16];
        const double rootPos[3] = {-18.4, 48.8, 45.2};
        f141test::rigid(37.0, 22.0, rootPos, froot);
        double r[3], u[3], f[3];
        check(f143::rootAxes(froot, r, u, f), "F143: a rigid root has unit axes");
        const double p14w[3] = {-18.48, 48.9, 45.3};
        const f143::Pub pub = pair143(eCm, sCm, 5000);
        const f143::Choice c = f143::choose(true, true, true, 7, 1, pub, 5050, p14w, u, r, f, p14w, 0.21, 0.0, 0.0);
        check(c.game && near143(c.gameUpCm, -4.62, 0.005) && near143(c.gameRightCm, -0.49, 0.005) && near143(dot143(c.shift, f), 0.0, 1e-12) && near143(dot143(c.gameShift, f), 0.0, 1e-12),
              "F143: on a root turned 37 deg and pitched 22 deg the shift is the same in the root's axes and perpendicular to the forward");
        // a toRoot round trip: a world point expressed in the root's axes and rebuilt by choose() agree
        const float pos[3] = {static_cast<float>(p14w[0] + 0.0004 * r[0] - 0.0462 * u[0] + 0.21 * f[0]), static_cast<float>(p14w[1] + 0.0004 * r[1] - 0.0462 * u[1] + 0.21 * f[1]),
                              static_cast<float>(p14w[2] + 0.0004 * r[2] - 0.0462 * u[2] + 0.21 * f[2])};
        double back[3];
        check(f143::toRoot(froot, p14w, pos, back) && near143(back[0], 0.0004, 2e-6) && near143(back[1], -0.0462, 2e-6) && near143(back[2], 0.21, 2e-6),
              "F143: a world point expressed in the root's right, up and forward from P14w");
        float zero[16] = {};
        const float nanPos[3] = {std::nanf(""), 0.0f, 0.0f};
        check(!f143::toRoot(zero, p14w, pos, back) && !f143::toRoot(froot, p14w, nanPos, back), "F143: a root with no axes, or a point that is not a number: no coordinates");
    }
    {   // the origin is the lock's (F79's measured origin may replace P14w): the target is Q whatever the origin
        const double p14w[3] = {0.0, 0.0, 0.0}, origin[3] = {0.004, 0.002, 0.03}, up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
        const f143::Pub pub = pair143(eCm, sCm, 100);
        const f143::Choice c = f143::choose(true, true, true, 7, 1, pub, 100, p14w, up, right, fwd, origin, 0.21, 0.0, 0.0);
        const double target[3] = {origin[0] + 0.21 * fwd[0] + c.shift[0], origin[1] + 0.21 * fwd[1] + c.shift[1], origin[2] + 0.21 * fwd[2] + c.shift[2]};
        const double qOnPlane[3] = {c.q[0], c.q[1], origin[2] + 0.21};
        check(c.game && near143(target[0], qOnPlane[0], 1e-9) && near143(target[1], qOnPlane[1], 1e-9) && near143(target[2], qOnPlane[2], 1e-12),
              "F143: with another origin the eye target (origin + k f + shift) is the point of the line at that depth");
    }
    {   // the trims are added along u and r, as F139 applies them
        const double p14w[3] = {0, 0, 0}, up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
        const f143::Pub pub = pair143(eCm, sCm, 100);
        const f143::Choice base = f143::choose(true, true, true, 7, 1, pub, 100, p14w, up, right, fwd, p14w, 0.21, 0.0, 0.0);
        const f143::Choice t = f143::choose(true, true, true, 7, 1, pub, 100, p14w, up, right, fwd, p14w, 0.21, 0.5, -0.3);
        check(t.game && near143(t.shift[0], base.shift[0] - 0.003, 1e-12) && near143(t.shift[1], base.shift[1] + 0.005, 1e-12) && near143(t.gameUpCm, base.gameUpCm, 1e-12) &&
                  near143(t.gameRightCm, base.gameRightCm, 1e-12),
              "F143: the live trims (0.5 cm up, 0.3 cm left) are added to the game's shift; the game's own figures are unchanged");
        const f143::Choice big = f143::choose(true, true, true, 7, 1, pub, 100, p14w, up, right, fwd, p14w, 0.21, 20.0, 0.0);
        check(big.game && near143(big.shift[1], base.shift[1] + 0.08, 1e-12), "F143: a trim of 20 cm is clamped to 8 cm before it is added");
        const f143::Choice over = f143::choose(true, true, true, 7, 1, pub, 100, p14w, up, right, fwd, p14w, 0.21, 4.0, 0.0);   // -4.62 + 4.0 = -0.62 cm up
        check(over.game && near143(over.gameUpCm, base.gameUpCm, 1e-9) && near143(over.shift[1], base.shift[1] + 0.04, 1e-12),
              "F143: a trim that moves the eye back toward the line's other side leaves the shift smaller, inside the bound");
        const f143::Choice past = f143::choose(true, true, true, 7, 1, pub, 100, p14w, up, right, fwd, p14w, 0.21, -4.0, 0.0);   // -4.62 - 4.0 = -8.62 cm
        check(!past.game && past.why == f143::kBound && past.shiftM == 0.0, "F143: the game's -4.62 cm up with a trim of -4.0 is 8.6 cm: past the bound, the table stands in");
        // the bound is the whole shift (a lateral trim counts): sqrt(4.62^2 + 7.01^2) = 8.4
        const f143::Choice lat = f143::choose(true, true, true, 7, 1, pub, 100, p14w, up, right, fwd, p14w, 0.21, 0.0, 7.5);
        check(!lat.game && lat.why == f143::kBound, "F143: the bound counts the lateral trim: -4.62 up and +7.0 right is 8.4 cm, past it");
        // a far line: S 40 cm over the sight at 36 cm depth is an 11 cm shift at k 21: refused
        const double e2[3] = {0.0, 20.0, 5.0}, s2[3] = {0.0, 20.0, 45.0};
        check(f143::choose(true, true, true, 7, 1, pair143(e2, s2, 100), 100, p14w, up, right, fwd, p14w, 0.21, 0.0, 0.0).why == f143::kBound,
              "F143: a game line 20 cm off the sight at the eye depth is past the bound");
    }
    {   // refusals and their order
        const double p14w[3] = {0, 0, 0}, up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
        const f143::Pub pub = pair143(eCm, sCm, 1000);
        const auto run = [&](bool key, bool itemOk, bool weapon, uint32_t held, int scope, const f143::Pub& p, uint64_t now) {
            return f143::choose(key, itemOk, weapon, held, scope, p, now, p14w, up, right, fwd, p14w, 0.21, 0.0, 0.0);
        };
        check(!run(false, true, true, 7, 1, pub, 1000).game && run(false, true, true, 7, 1, pub, 1000).why == f143::kKeyOff, "F143: the key off: the table (counted as key off)");
        check(run(false, false, true, 7, 1, pub, 1000).why == f143::kKeyOff, "F143: ... the key off is counted before anything else");
        check(run(true, false, true, 7, 1, pub, 1000).why == f143::kNoItem, "F143: an item snapshot that was not read: the table (item not read)");
        f143::Pub none;
        check(run(true, true, true, 7, 1, none, 1000).why == f143::kNoPair, "F143: no pair published: the table (no pair)");
        check(run(true, true, true, 8, 1, pub, 1000).why == f143::kOtherItem, "F143: a pair for another item (held serial 7, now 8): the table (another item)");
        check(run(true, true, true, 7, 0, pub, 1000).why == f143::kOtherItem, "F143: a pair for another scope state: the table (another item)");
        check(run(true, true, false, 7, 1, pub, 1000).why == f143::kOtherItem, "F143: a held item that is not a weapon takes no game line");
        f143::Pub incoherent = pub;
        incoherent.key.coherent = false;
        check(run(true, true, true, 7, 1, incoherent, 1000).why == f143::kOtherItem, "F143: a pair made while the item was not coherent is not for any item");
        check(run(true, true, true, 7, 1, pub, 1250).game && run(true, true, true, 7, 1, pub, 1251).why == f143::kStale, "F143: 250 ms old is usable, 251 ms is stale");
        check(run(true, true, true, 7, 1, pub, 900).game && near143(run(true, true, true, 7, 1, pub, 900).ageMs, 0.0, 1e-12),
              "F143: a pair stamped after now (the clock went back) is age 0, not stale");
        const double flatE[3] = {0.0, -4.0, 20.0}, flatS[3] = {0.5, -4.0, 20.9};
        check(run(true, true, true, 7, 1, pair143(flatE, flatS, 1000), 1000).why == f143::kDegenerate, "F143: S 0.9 cm further along than E: under 1 cm of depth, refused");
        const double okE[3] = {0.0, -4.0, 20.0}, okS[3] = {0.5, -4.0, 21.2};
        check(run(true, true, true, 7, 1, pair143(okE, okS, 1000), 1000).game, "F143: 1.2 cm of depth is enough");
        const double backE[3] = {0.0, -4.0, 30.0}, backS[3] = {0.0, -4.0, 15.0};
        check(run(true, true, true, 7, 1, pair143(backE, backS, 1000), 1000).game, "F143: a line pointing back (S nearer than E) has depth too: Q is still on it at k");
        const double zeroAxis[3] = {0, 0, 0};
        check(f143::choose(true, true, true, 7, 1, pub, 1000, p14w, zeroAxis, right, fwd, p14w, 0.21, 0.0, 0.0).why == f143::kDegenerate &&
                  f143::choose(true, true, true, 7, 1, pub, 1000, p14w, up, right, zeroAxis, p14w, 0.21, 0.0, 0.0).why == f143::kDegenerate &&
                  f143::choose(true, true, true, 7, 1, pub, 1000, p14w, up, right, fwd, p14w, std::nan(""), 0.0, 0.0).why == f143::kDegenerate,
              "F143: an axis with no length, or an eye depth that is not a number: refused as degenerate");
        f143::Pub nanPub = pub;
        nanPub.s[1] = std::nan("");
        check(run(true, true, true, 7, 1, nanPub, 1000).why == f143::kDegenerate, "F143: a pair that is not a number is refused as degenerate");
        check(std::strcmp(f143::whyText(f143::kGame), "the game's line") == 0 && std::strcmp(f143::whyText(f143::kKeyOff), "key off") == 0 && std::strcmp(f143::whyText(99), "?") == 0 &&
                  f143::kWhyCount == 8,
              "F143: the reasons' text");
    }
    {   // the filter: first sample, the time constant, a tick's mean, the restart on a key change
        f143::LineFilter fl;
        f143::LineKey k;
        k.held = 7; k.coherent = true; k.scope = 1; k.activation = 3;
        const double e0[3] = {0.0, 0.0, 0.15}, s0[3] = {0.0, 0.0, 0.35}, e1[3] = {0.01, -0.02, 0.15}, s1[3] = {0.01, -0.02, 0.35};
        check(!fl.flush(100) && !fl.have, "F143: nothing noted, nothing to flush");
        fl.note(k, e0, s0);
        check(fl.flush(1000) && fl.have && near143(fl.e[2], 0.15, 1e-12) && fl.ms == 1000 && fl.samples == 1, "F143: the first sample is the state");
        fl.note(k, e1, s1);
        check(fl.flush(1150) && near143(fl.e[0], 0.01 * (1.0 - std::exp(-1.0)), 1e-12) && near143(fl.e[1], -0.02 * (1.0 - std::exp(-1.0)), 1e-12) && near143(fl.s[0], fl.e[0], 1e-12),
              "F143: 0.15 s later the state has moved 1 - 1/e of the way to the new sample (tau 0.15 s)");
        fl.note(k, e1, s1);
        check(fl.flush(1150) && near143(fl.e[0], 0.01 * (1.0 - std::exp(-1.0)), 1e-12), "F143: a second tick at the same time moves nothing (dt 0)");
        // a tick's mean: two samples drained together are one step
        f143::LineFilter m;
        const double ea[3] = {0.02, 0.0, 0.1}, eb[3] = {0.04, 0.0, 0.2};
        m.note(k, e0, s0);
        m.flush(0);
        m.note(k, ea, s0);
        m.note(k, eb, s0);
        m.flush(1000000);   // a long time: the state is the mean
        check(near143(m.e[0], 0.03, 1e-9) && near143(m.e[2], 0.15, 1e-9) && m.samples == 3, "F143: samples drained together are averaged into one step");
        // restart on another item, scope or activation
        f143::LineKey k2 = k;
        k2.activation = 4;
        fl.note(k2, e0, s0);
        check(fl.flush(1200) && near143(fl.e[0], 0.0, 1e-12) && fl.samples == 1 && fl.key == k2, "F143: another activation restarts the filter from the new sample");
        k2 = k; k2.held = 8;
        fl.note(k2, e1, s1);
        check(fl.flush(1300) && near143(fl.e[0], 0.01, 1e-12) && fl.samples == 1, "F143: another item restarts it");
        k = k2; k.scope = 0;
        fl.note(k, e0, s0);
        check(fl.flush(1400) && near143(fl.e[0], 0.0, 1e-12) && fl.samples == 1, "F143: another scope restarts it");
        f143::Pub pub;
        f143::publish(pub, fl);
        check(pub.valid && pub.ms == 1400 && pub.key == k && near143(pub.e[2], 0.15, 1e-12), "F143: the published pair carries the time, the key and the filtered pair");
        check(near143(f143::stepM(e0, e1), std::sqrt(0.01 * 0.01 + 0.02 * 0.02), 1e-12), "F143: the step between two shifts is their distance");
    }

    // ---- 2. the coat's clip modifiers --------------------------------------------------------------------------------------------------------------------------------
    {
        check(f143::kPartCount == edvr::avh::kPartCount, "F143: the clip snapshot holds every part of the avatar's table");
        uint64_t list = 0, head = 0;
        static const char* const names[] = {"EVASuit_Torso", "EVASuit_Arms", "EVASuit_Hands", "Shoulder_Left", "Shoulder_Right", "EVASuit_Legs", "EVASuit_Feet", "FirstPersonSkeleton",
                                            "EVASuit_Gear_LowerArms", "EVASuit_Gear_LowerArmL", "EVASuit_Gear_LowerArmR"};
        for (const char* n : names) {
            const int i = edvr::avh::partIndexByName(n);
            if (i >= 0) list |= 1ull << i;
        }
        for (uint32_t h : edvr::avh::kHeadParts) head |= 1ull << h;
        const uint64_t extra = f143::extraUnclipMask(list, head);
        int count = 0;
        for (int i = 0; i < 54; ++i) count += (extra >> i) & 1;
        const auto has = [&](const char* n) { return ((extra >> edvr::avh::partIndexByName(n)) & 1ull) != 0; };
        check(count == 54 - 12 - 11, "F143: the A/B adds 31 parts: 54 less the 12 head parts and the 11 the unclip list names");
        check(has("EVASuit_Gear_Back") && has("EVASuit_Gear_UpperArms") && has("EVASuit_Gear_Chest") && has("EVASuit_Gear_Waist") && has("OuterClothing_Torso") && has("EVASuit"),
              "F143: ... among them the coat's Gear_Back and the other gear parts");
        check(!has("EVASuit_Torso") && !has("EVASuit_Arms") && !has("EVASuit_Hands") && !has("Shoulder_Left") && !has("EVASuit_Gear_LowerArmL") && !has("EVASuit_Legs") &&
                  !has("EVASuit_Feet") && !has("FirstPersonSkeleton"),
              "F143: ... not the parts already covered, nor the comparison parts the list leaves clipped, nor the first-person skeleton");
        check(!has("Head") && !has("Hair") && !has("EVASuit_Helmet") && !has("EVASuit_Eyewear") && !has("EVASuit_Gear_Head") && !has("Beard") && !has("Teeth"), "F143: ... and no head part");
        check((extra >> 54) == 0 && f143::extraUnclipMask(0, 0) == (1ull << 54) - 1ull && f143::extraUnclipMask((1ull << 54) - 1ull, 0) == 0, "F143: the mask is the avatar's 54 bits and no more");
        // armed state
        check(f143::armedState(false, "$wpn_m_assaultrifle_laser_fauto_name;") == f143::kUnarmed && f143::armedState(true, "$wpn_m_assaultrifle_laser_fauto_name;") == f143::kWeapon &&
                  f143::armedState(true, "$humanoid_companalyser_name;") == f143::kTool && f143::armedState(true, nullptr) == f143::kTool && f143::armedState(true, "") == f143::kTool,
              "F143: unarmed, a weapon ($wpn_ item) and a tool");
        check(std::strcmp(f143::armedText(0), "unarmed") == 0 && std::strcmp(f143::armedText(1), "weapon") == 0 && std::strcmp(f143::armedText(2), "tool") == 0 && std::strcmp(f143::armedText(9), "?") == 0,
              "F143: the armed state as the log says it");
        // when the measure reads
        check(f143::readWhy(-1, 0, 1000, 0, 0, 300) == f143::kReadState, "F143: the first call reads");
        check(f143::readWhy(0, 0, 1000, 6000, 0, 300) == f143::kReadNo && f143::readWhy(0, 0, 5999, 6000, 0, 300) == f143::kReadNo, "F143: nothing to read before the 5 s clock");
        check(f143::readWhy(0, 0, 6000, 6000, 0, 300) == f143::kReadClock, "F143: the 5 s clock reads");
        check(f143::readWhy(0, 1, 1000, 6000, 0, 300) == f143::kReadState && f143::readWhy(1, 2, 1000, 6000, 0, 300) == f143::kReadState, "F143: a change of the armed state reads at once");
        check(f143::readWhy(0, 0, 9000, 6000, 300, 300) == f143::kReadNo && f143::readWhy(0, 2, 9000, 6000, 300, 300) == f143::kReadState,
              "F143: past the cap the clock stops reading, a change of the armed state does not");
        f143::ClipSnap snap;
        check(snap.n == 0 && snap.why == f143::kReadClock && !snap.flagsOk, "F143: an empty clip snapshot");
    }
}

}  // namespace f143test
