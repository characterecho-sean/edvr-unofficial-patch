// F194 checks (native on-foot VR), run194 below. Header only, run by native_frame_test.
//
// F194 (native_on_foot_f194.h, native_on_foot_f109.h, native_on_foot.cpp, src/openxr/view_fov_note.h; TEMPORARY A/B keys experimental.native_on_foot_look_to_aim_aims_at_picture_centre and
// experimental.native_on_foot_look_to_aim_eye_height_as_mouse_look, both default on, live). The tester (Quest 3, rc.4): look to aim's aim sits about 12.9 deg above where they look. The pilot, on his
// Quest 3: with the mouse aim settled he turned look to aim on and the view dropped; on his Pimax he sees neither. The headset's vertical frustum: Quest 3 (Virtual Desktop) up 0.9657, down 1.4281, so the
// middle of the picture is atan((0.9657 - 1.4281) / 2) = -13.0 deg from the pose's forward axis; Pimax Crystal Super 1.2648 both, 0. Look to aim took the forward axis. Part 1: the centring takes the ray
// through the middle of the picture. Part 2: look to aim's own 10 deg settle gate (on the CARRIED pitch) is skipped, so F139's gate covers it as it covers mouse look.
// The rig: the pitch helper (Quest, Pimax, bad inputs), the channel's swap (the API names are reversed physically), the ray (a level head on the Quest centres to -13.0, a head pitched +13 to about 0, the Pimax
// to the forward axis bit for bit, a rolled head turned about its own right axis and not world X, a yawed and pitched head, the refusals near vertical), centreOffset with the key on and off, the gate's
// predicate (aim -13 / -15 / +12, armed and unarmed, against F139's rule; off: today's 10 deg), the lines, the height probe and the source sites.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f109.h"
#include "../../src/d3d11/native_on_foot_f138.h"
#include "../../src/d3d11/native_on_foot_f171c.h"
#include "../../src/d3d11/native_on_foot_f194.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f194test {

namespace nf = edvr::nofoot;
namespace f194 = edvr::nofoot::f194;
namespace f109 = edvr::nofoot::f109;
namespace f138 = edvr::nofoot::f138;

constexpr double kPi = 3.14159265358979323846;
constexpr double kQuestUp = 0.9657, kQuestDown = 1.4281, kPimax = 1.2648;
inline double rad(double d) { return d * kPi / 180.0; }
inline double deg(double r) { return r * 180.0 / kPi; }

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
inline size_t countOf(const std::string& hay, const std::string& needle) {
    size_t n = 0, at = 0;
    while ((at = hay.find(needle, at)) != std::string::npos) {
        ++n;
        at += needle.size();
    }
    return n;
}

// 3x3 row-major products, for the independent expectation: the ray's forward is the third column (negated) of R times Rx(d), the head's rotation turned about its own right axis.
struct M3 {
    double a[3][3];
};
inline M3 mul(const M3& x, const M3& y) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r.a[i][j] += x.a[i][k] * y.a[k][j];
    return r;
}
inline M3 rx(double d) {   // about the right axis: positive turns the nose up
    const double c = std::cos(d), s = std::sin(d);
    return M3{{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
}
inline M3 ry(double d) {   // about the up axis: positive turns the nose left (the seat's yaw sign)
    const double c = std::cos(d), s = std::sin(d);
    return M3{{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
}
inline M3 rz(double d) {   // about the back axis: a roll
    const double c = std::cos(d), s = std::sin(d);
    return M3{{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}};
}
// The head's rotation: yaw, then pitch, then roll in the head's own frame (R = Ry Rx Rz), as a 3x4 row-major rigid transform with a translation.
inline M3 headR(double yawDeg, double pitchDeg, double rollDeg) { return mul(mul(ry(rad(yawDeg)), rx(rad(pitchDeg))), rz(rad(rollDeg))); }
inline void toMat(const M3& r, float m[12]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[4 * i + j] = static_cast<float>(r.a[i][j]);
        m[4 * i + 3] = 0.1f * static_cast<float>(i + 1);
    }
}
inline void head(double yawDeg, double pitchDeg, double rollDeg, float m[12]) { toMat(headR(yawDeg, pitchDeg, rollDeg), m); }
// The yaw and pitch (degrees, the seat's sign) of the third column of R, negated: what adsHeadAngles reads of a rotation.
inline void anglesOf(const M3& r, double* yawDeg, double* pitchDeg) {
    const double fx = -r.a[0][2], fy = -r.a[1][2], fz = -r.a[2][2];
    *yawDeg = deg(std::atan2(-fx, -fz));
    *pitchDeg = deg(std::asin(fy));
}
inline bool eq(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

template <class Check>
void run194(Check&& check) {
    // ---- 1. the pitch of the picture's middle -------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double quest = deg(f194::pictureCentrePitchRad(kQuestUp, kQuestDown)), pimax = deg(f194::pictureCentrePitchRad(kPimax, kPimax));
        check(eq(quest, -13.0, 0.05) && eq(quest, deg(std::atan((kQuestUp - kQuestDown) / 2.0)), 1e-9),
              "F194: the Quest 3's picture centre is atan((0.9657 - 1.4281) / 2) = -13.0 deg from the forward axis (below)");
        check(pimax == 0.0, "F194: the Pimax Crystal Super's (1.2648 both) is exactly 0");
        check(f194::pictureCentrePitchRad(std::nan(""), 1.0) == 0.0 && f194::pictureCentrePitchRad(1.0, std::nan("")) == 0.0 && f194::pictureCentrePitchRad(0.0, 1.0) == 0.0 &&
                  f194::pictureCentrePitchRad(1.0, 0.0) == 0.0 && f194::pictureCentrePitchRad(-1.0, 1.0) == 0.0 && f194::pictureCentrePitchRad(1.0, -1.0) == 0.0 &&
                  f194::pictureCentrePitchRad(INFINITY, 1.0) == 0.0 && f194::pictureCentrePitchRad(1.0, INFINITY) == 0.0,
              "F194: the pitch is 0 for a frustum that is not numbers, not finite, or not positive");
        check(f194::pictureCentrePitchRad(10.0, 0.1) == 0.0 && f194::pictureCentrePitchRad(0.1, 10.0) == 0.0 && f194::pictureCentrePitchRad(1.0, 1.7) != 0.0,
              "F194: a centre further than 30 deg from the forward axis is no headset's: 0 (1.0 up, 1.7 down, 19.3 deg, is read)");
        check(f194::pictureCentrePitchRad(1.0, 1.2) > f194::pictureCentrePitchRad(1.0, 1.6) && f194::pictureCentrePitchRad(1.6, 1.0) > 0.0, "F194: more reach down than up is below the axis (negative); more up than down, above");
    }
    // ---- 2. the channel: its names are reversed ----------------------------------------------------------------------------------------------------------------------------------------------------
    {
        // The channel carries OpenVR's pfTop and pfBottom as magnitudes: for the Quest top 1.4281, bottom 0.9657 (frame_flag.h: "t=-1.4281 b=+0.9657"); the physical up is the bottom's, the down the top's.
        const f194::Frustum q = f194::frustumFromChannel(true, kQuestDown, kQuestUp);
        check(q.have && q.usable && q.upTan == kQuestUp && q.downTan == kQuestDown && eq(deg(q.pitchRad), -13.0, 0.05),
              "F194: the channel's top (1.4281) is the physical DOWN and its bottom (0.9657) the physical UP: the Quest's picture centre reads -13.0 deg, below (the swap is in frustumFromChannel)");
        const f194::Frustum wrong = f194::frustumFromChannel(true, kQuestUp, kQuestDown);
        check(deg(wrong.pitchRad) > 12.9, "F194: and the unswapped order would read +13.0 (the sign the rig pins)");
        const f194::Frustum p = f194::frustumFromChannel(true, kPimax, kPimax);
        check(p.usable && p.pitchRad == 0.0, "F194: the Pimax reads 0 whichever order");
        const f194::Frustum none = f194::frustumFromChannel(false, 0.0, 0.0), zero = f194::frustumFromChannel(true, 0.0, 0.0), nan = f194::frustumFromChannel(true, std::nan(""), 1.0),
                            wide = f194::frustumFromChannel(true, 0.1, 10.0);
        check(!none.have && !none.usable && none.pitchRad == 0.0 && zero.have && !zero.usable && zero.pitchRad == 0.0 && nan.have && !nan.usable && wide.have && !wide.usable && wide.pitchRad == 0.0,
              "F194: no answer, a zero, a NaN and an implausible frustum are not usable and carry no pitch");
        check(f194::pitchInForce(true, q) == q.pitchRad && f194::pitchInForce(false, q) == 0.0 && f194::pitchInForce(true, none) == 0.0 && f194::pitchInForce(true, zero) == 0.0 &&
                  f194::pitchInForce(true, wide) == 0.0,
              "F194: the pitch in force is the picture's centre with the key on and a usable frustum, else 0 (the forward axis)");
        check(f194::sameFrustum(q, f194::frustumFromChannel(true, 1.4284, 0.9654)) && !f194::sameFrustum(q, p) && !f194::sameFrustum(q, none) && f194::sameFrustum(none, none),
              "F194: two frusta are the same to the channel's resolution (a thousandth of a tangent)");
    }
    // ---- 3. the ray ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double quest = f194::pictureCentrePitchRad(kQuestUp, kQuestDown);
        float m[12];
        double y = 0, p = 0;
        head(0, 0, 0, m);
        check(f194::rayAngles(m, quest, &y, &p) && eq(deg(p), -13.0, 0.05) && eq(deg(y), 0.0, 1e-6), "F194: a level head with the Quest's tangents has its picture-centre ray at pitch -13.0 deg, yaw 0");
        head(0, 13, 0, m);
        check(f194::rayAngles(m, quest, &y, &p) && eq(deg(p), 0.0, 0.1) && eq(deg(y), 0.0, 1e-6), "F194: a head pitched +13 deg has it at about 0 (it looks at the middle of its picture level)");
        head(0, 0, 0, m);
        double fy = 0, fp = 0;
        check(f194::rayAngles(m, 0.0, &y, &p) && nf::adsHeadAngles(m, &fy, &fp) && y == fy && p == fp, "F194: the Pimax (0 rad) is adsHeadAngles itself, bit for bit, on a level head");
        for (int i = 0; i < 40; ++i) {
            head(-170.0 + 8.7 * i, -60.0 + 2.9 * i, -40.0 + 1.9 * i, m);
            const bool a = f194::rayAngles(m, 0.0, &y, &p), b = nf::adsHeadAngles(m, &fy, &fp);
            if (a != b || (a && (y != fy || p != fp))) {
                check(false, "F194: the Pimax (0 rad) is adsHeadAngles itself, bit for bit, on rotated and rolled heads");
                break;
            }
            if (i == 39) check(true, "F194: the Pimax (0 rad) is adsHeadAngles itself, bit for bit, on rotated and rolled heads (40 poses)");
        }
        // a yawed and pitched head, no roll: the right axis is horizontal, so the ray is the head's pitch + d at the head's yaw
        head(40, 20, 0, m);
        check(f194::rayAngles(m, quest, &y, &p) && eq(deg(p), 20.0 - 13.0, 0.06) && eq(deg(y), 40.0, 0.01), "F194: a head yawed 40 and pitched 20 has its ray at yaw 40, pitch 7 (the pitch less 13)");
        // a rolled head: turned about the head's own right axis, which is not horizontal, so the ray is off to the side as well; the expectation is the rotation product R * Rx(d)
        for (double roll : {30.0, -30.0, 60.0}) {
            head(0, 0, roll, m);
            const M3 turned = mul(headR(0, 0, roll), rx(quest));
            double ey = 0, ep = 0;
            anglesOf(turned, &ey, &ep);
            check(f194::rayAngles(m, quest, &y, &p) && eq(deg(y), ey, 0.05) && eq(deg(p), ep, 0.05) && std::fabs(deg(y)) > 5.0 && std::fabs(deg(p) + 13.0) > 0.5,
                  "F194: a head rolled 30 / -30 / 60 deg is turned about its own right axis (the rotation product R Rx(d)), not about world X: its ray is off to the side, not at yaw 0 and pitch -13");
        }
        head(25, -10, 20, m);
        {
            const M3 turned = mul(headR(25, -10, 20), rx(quest));
            double ey = 0, ep = 0;
            anglesOf(turned, &ey, &ep);
            check(f194::rayAngles(m, quest, &y, &p) && eq(deg(y), ey, 0.05) && eq(deg(p), ep, 0.05), "F194: a head yawed, pitched and rolled at once: the ray is the rotation product's (independent of the vector form)");
        }
        // the refusals near vertical, by the ray as adsHeadAngles does by the axis
        head(0, 85, 0, m);
        check(!nf::adsHeadAngles(m, &fy, &fp) && f194::rayAngles(m, quest, &y, &p) && eq(deg(p), 72.0, 0.1),
              "F194: the forward axis at 85 deg up is refused by adsHeadAngles, the ray (72 deg) is read: the refusal is the ray's own");
        head(0, 85, 0, m);
        check(!f194::rayAngles(m, rad(13.0), &y, &p), "F194: a ray at 98 deg (over the top) is refused");
        head(0, 80, 0, m);
        check(!f194::rayAngles(m, rad(5.0), &y, &p), "F194: and one within 11 deg of vertical (85 deg) is");
        float zero[12] = {};
        check(!f194::rayAngles(zero, quest, &y, &p) && !f194::rayAngles(m, std::nan(""), &y, &p) && !f194::rayAngles(m, INFINITY, &y, &p), "F194: a degenerate matrix and a pitch that is not a number are refused");
        float skew[12] = {};
        skew[2] = -1.0f;   // forward = +x
        skew[1] = 1.0f;    // up = +x: parallel to the forward axis
        skew[10] = 0.0f;
        check(!f194::rayAngles(skew, quest, &y, &p), "F194: an up axis parallel to the forward axis (no right axis to turn about) is refused");
    }
    // ---- 4. the centring (centreOffset) with the key on and off ----------------------------------------------------------------------------------------------------------------------------------------
    {
        const f194::Frustum quest = f194::frustumFromChannel(true, kQuestDown, kQuestUp), pimax = f194::frustumFromChannel(true, kPimax, kPimax), none = f194::frustumFromChannel(false, 0, 0);
        float m[12];
        head(0, 0, 0, m);
        double y = 0, p = 0;
        check(f109::centreOffset(m, &y, &p, f194::pitchInForce(true, quest)) && eq(deg(p), -13.0, 0.05), "F194: with the key on, a level head on the Quest centres to -13.0 deg pitch (the aim goes 13 deg down onto the middle of the picture)");
        check(f109::centreOffset(m, &y, &p, f194::pitchInForce(false, quest)) && p == 0.0 && y == 0.0, "F194: with the key off, the same head centres to the forward axis: 0");
        check(f109::centreOffset(m, &y, &p) && p == 0.0 && y == 0.0, "F194: the old call (no pitch given) is the forward axis");
        check(f109::centreOffset(m, &y, &p, f194::pitchInForce(true, none)) && p == 0.0, "F194: with the key on and no frustum known, the forward axis, as before");
        check(f109::centreOffset(m, &y, &p, f194::pitchInForce(true, pimax)) && p == 0.0 && y == 0.0, "F194: with the key on a Pimax centres to the forward axis (nothing to correct)");
        // a head at +13 on the Quest: the forward axis is 13 deg above the aim, the ray is on it
        head(0, 13, 0, m);
        double fy = 0, fp = 0;
        check(f109::centreOffset(m, &y, &p, f194::pitchInForce(true, quest)) && eq(deg(p), 0.0, 0.1) && nf::adsHeadAngles(m, &fy, &fp) && eq(deg(fp), 13.0, 1e-3),
              "F194: a head pitched +13 on the Quest has the ray on the aim (offset 0) while the forward axis stands 13 deg above it");
        // the mean of the two eyes (the head) is on the axis horizontally: a yaw offset is untouched by the vertical correction for an unrolled head
        head(35, 0, 0, m);
        check(f109::centreOffset(m, &y, &p, f194::pitchInForce(true, quest)) && eq(deg(y), 35.0, 1e-3) && eq(deg(p), -13.0, 0.05), "F194: the correction is vertical only: a head turned 35 deg has its ray at yaw 35 and pitch -13");
        // the centring threshold and the follower: the offset goes into the target as it did, and the aim follows the ray from then on
        f109::Follower fl;
        head(0, 0, 0, m);
        check(f109::centreOffset(m, &y, &p, f194::pitchInForce(true, quest)), "F194: (centring read)");
        fl.addHead(y, p);
        double ay = 0, ap = 0;
        fl.ask(1.0 / 90.0, 0.0, 0.0, 0.0, &ay, &ap);
        check(ap < 0.0 && eq(fl.target[1], rad(-13.0), 1e-3), "F194: the follower's target after the centring is the ray's offset, -13 deg pitch: the aim is asked down, not up");
        // the follower's head steps are the forward axis's and do not see the picture's centre (a constant offset drops out of a step)
        float a[12], b[12];
        head(10, 0, 0, a);
        head(10, 4, 0, b);
        double dy = 0, dp = 0;
        check(nf::adsHeadDelta(a, b, &dy, &dp) && eq(deg(dp), 4.0, 1e-4), "F194: the follower's head steps (adsHeadDelta) are the forward axis's: a 4 deg nod is 4 deg, whatever the picture's centre");
        double ay0 = 0, ap0 = 0, ay1 = 0, ap1 = 0;
        check(f194::rayAngles(a, f194::pitchInForce(true, quest), &ay0, &ap0) && f194::rayAngles(b, f194::pitchInForce(true, quest), &ay1, &ap1) && eq(deg(ap1 - ap0), 4.0, 0.01),
              "F194: and the ray's step for an unrolled head is the same 4 deg (a constant offset drops out of a step)");
    }
    // ---- 5. part 2: look to aim's settle gate ----------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(!f194::ltaGateWouldRefuse(9.99) && f194::ltaGateWouldRefuse(10.0) && f194::ltaGateWouldRefuse(45.0) && !f194::ltaGateWouldRefuse(std::nan("")) && !f194::ltaGateWouldRefuse(0.0),
              "F194: look to aim's own gate would refuse from 10 deg of carried pitch (not under it, not for a number that is not one)");
        check(f194::ltaGateRefuses(false, 13.0) && !f194::ltaGateRefuses(true, 13.0) && !f194::ltaGateRefuses(false, 9.9) && !f194::ltaGateRefuses(true, 9.9),
              "F194: with the key off the gate refuses from 10 deg (today's); with it on it refuses nothing");
        // on: the carried pitch is the engage's account (13 with the aim on the Quest's picture centre); F139 reads the FP root's pitch (the game's aim), 20 deg, nothing drawn
        const struct {
            double carried, root;
            bool item;
            bool refusedOn, refusedOff;
        } cases[] = {
            {13.0, -13.0, false, false, true},    // the Quest's level head: aim -13: settles with the key on, refused by the 10 deg gate with it off
            {13.0, -13.0, true, false, true},
            {15.0, -15.0, false, false, true},
            {15.0, -15.0, true, false, true},
            {12.0, 12.0, false, false, true},     // the earlier Pimax engage at +11.9: refused for 80+90+37 calls, the view 10 cm low until the head levelled
            {12.0, 12.0, true, false, true},
            {5.0, -25.0, false, true, true},      // the game's aim 25 deg down, nothing drawn: F139 refuses in both
            {5.0, -25.0, true, false, false},     // ... with an item drawn it does not
            {5.0, 21.0, false, true, true},
            {9.9, -13.0, false, false, false},    // under the 10 deg carried and under F139's 20: both settle
            {0.0, 0.0, false, false, false},
        };
        bool all = true, tens = true, matchesF139 = true;
        for (const auto& c : cases) {
            const f194::SettleVerdict on = f194::settleVerdict(true, c.carried, c.root, true, c.item), off = f194::settleVerdict(false, c.carried, c.root, true, c.item);
            all = all && on.refused() == c.refusedOn && off.refused() == c.refusedOff;
            tens = tens && !on.tenRefused && on.tenWould == (c.carried >= 10.0) && off.tenWould == (c.carried >= 10.0) && off.tenRefused == (c.carried >= 10.0);
            matchesF139 = matchesF139 && on.f139Refused == f138::settleRefused(c.root, f138::kSettleGateDeg, c.item);
        }
        check(all, "F194: the settle's verdict: aim -13 / -15 / +12 (carried 13 / 15 / 12) settles with the key on, armed and unarmed; the 10 deg gate refused all of them with it off; F139 refuses 25 deg down unarmed in both");
        check(tens && matchesF139, "F194: with the key on the only refusal is F139's rule on the game's aim (f138::settleRefused, 20 deg, nothing drawn), exactly as mouse look; the 10 deg gate is counted as would-have in both");
        const f194::SettleVerdict unknown = f194::settleVerdict(true, 30.0, 50.0, false, false);
        check(unknown.tenWould && !unknown.tenRefused && !unknown.f139Refused && !unknown.refused(), "F194: a game aim that was not read is not F139's to refuse; the 10 deg gate is only counted");
        const f194::SettleVerdict offFirst = f194::settleVerdict(false, 13.0, -25.0, true, false);
        check(offFirst.tenRefused && !offFirst.f139Refused, "F194: off, the 10 deg gate refuses first and F139's rule is not asked, as the hook does (it counts the calls it refuses itself)");
    }
    // ---- 6. the lines -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        char buf[1400];
        const f194::Frustum quest = f194::frustumFromChannel(true, kQuestDown, kQuestUp), pimax = f194::frustumFromChannel(true, kPimax, kPimax), none = f194::frustumFromChannel(false, 0, 0),
                            wide = f194::frustumFromChannel(true, 0.1, 10.0), above = f194::frustumFromChannel(true, 0.9, 1.4);
        f194::formatFrustum(buf, sizeof(buf), quest, true);
        check(std::strcmp(buf, "native on foot look to aim (F194): the picture's centre is 13.0 deg below the headset's forward axis (up 0.966, down 1.428 tangents); look to aim aims at it: on") == 0,
              "F194: the frustum line: the Quest's picture centre 13.0 deg below the headset's forward axis (up 0.966, down 1.428 tangents), the key");
        f194::formatFrustum(buf, sizeof(buf), quest, false);
        check(std::strstr(buf, "aims at it: off") != nullptr && std::strstr(buf, "13.0 deg below") != nullptr, "F194: ... and the key off says so, with the same centre");
        f194::formatFrustum(buf, sizeof(buf), none, true);
        check(std::strstr(buf, "(F194): no frustum from the headset yet: the forward axis; look to aim aims at the picture's centre: on") != nullptr, "F194: before an answer the line says the forward axis");
        f194::formatFrustum(buf, sizeof(buf), wide, true);
        check(std::strstr(buf, "is not usable: the forward axis") != nullptr && std::strstr(buf, "up 10.000, down 0.100") != nullptr, "F194: a frustum that cannot be used says so, with its numbers");
        f194::formatFrustum(buf, sizeof(buf), pimax, true);
        check(std::strstr(buf, "the picture's centre is on the headset's forward axis (up 1.265, down 1.265 tangents)") != nullptr, "F194: a Pimax's centre is on the forward axis");
        f194::formatFrustum(buf, sizeof(buf), above, true);
        check(std::strstr(buf, "deg above the headset's forward axis") != nullptr, "F194: a frustum that reaches further up than down is above");

        // the 1 s line
        f194::Second s;
        f194::Window w;
        for (int i = 0; i < 90; ++i) {
            s.add(true, 2.0, -3.0, true, -16.0, true, 13.0, true, 0.1);
            f194::Call c;
            c.eyeOverNeckM = 0.19;
            c.raiseM = -0.05;
            c.tenWould = true;
            c.taken = i == 30;
            if (i < 3) c.f139Refused = true;
            w.add(c);
        }
        f194::formatSecond(buf, sizeof(buf), s, w, -13.0, true, true);
        check(std::strlen(buf) < 1100 && std::strstr(buf, "look to aim height (F194) 1 s: the picture-centre pitch in force -13.0 deg (look to aim aims at the picture's centre: on);") != nullptr &&
                  std::strstr(buf, "the headset's pitch (LOCAL) mean +2.0 deg;") != nullptr && std::strstr(buf, "carried aim pitch mean -3.0 deg (the game's aim -16.0 deg);") != nullptr &&
                  std::strstr(buf, "the forward axis +13.0 deg, the ray in force +0.1 deg;") != nullptr &&
                  std::strstr(buf, "look to aim's 10 deg gate would have refused (skipped) 90 of 90 pin calls, F138/F139's 20 deg gate refused 3, taken 1;") != nullptr &&
                  std::strstr(buf, "the published raise mean -5.0 cm;") != nullptr && std::strstr(buf, "the drawn eye over the animated neck mean +19.0 cm") != nullptr,
              "F194: the 1 s line says the pitch in force, the headset's, the carried and the game's aim pitch, the forward axis's and the ray's pitch from the aim, the settle (the 10 deg gate would have refused, skipped; F139's; taken), the raise and the drawn eye over the neck");
        f194::Window off;
        for (int i = 0; i < 12; ++i) {
            f194::Call c;
            c.tenWould = c.tenRefused = true;
            off.add(c);
        }
        f194::formatSecond(buf, sizeof(buf), s, off, 0.0, false, false);
        check(std::strstr(buf, "(look to aim aims at the picture's centre: off);") != nullptr && std::strstr(buf, "look to aim's 10 deg gate refused 12 of 12 pin calls,") != nullptr &&
                  std::strstr(buf, "the picture-centre pitch in force +0.0 deg") != nullptr,
              "F194: with the key off the 10 deg gate line says it refused (not would have), and the pitch in force is 0");
        f194::formatSecond(buf, sizeof(buf), s, f194::Window{}, 0.0, true, true);
        check(std::strstr(buf, "of 0 pin calls") != nullptr && std::strstr(buf, "taken 0;") != nullptr,
              "F194: a second in which the hook wrote nothing says 0 pin calls: the line cannot read as success when the hook never ran");
        f194::Window w2;
        f194::Call missing;   // eye over the neck and the raise not numbers: not counted, not zero
        w2.add(missing);
        check(w2.any() && w2.eyeN == 0 && w2.raiseN == 0 && w2.calls == 1, "F194: a call with no eye reading or no raise yet counts as a call and adds nothing to the means");
        f194::Second ss;
        ss.add(false, 0.0, 5.0, false, 0.0, false, 0.0, false, 0.0);
        check(ss.any() && ss.localN == 0 && ss.mean(ss.localSum, ss.localN) == 0.0 && ss.mean(ss.carriedSum, ss.ticks) == 5.0, "F194: a tick whose angles were not read adds nothing to their means");
        // the window goes through the mailbox the other 1 s lines use
        nf::f171c::Mailbox<f194::Window> box;
        f194::Call c1;
        c1.taken = true;
        box.write([&](f194::Window& win) noexcept { win.add(c1); });
        f194::Window got;
        check(box.take(&got) && got.calls == 1 && got.taken == 1, "F194: the hook's window goes through the mailbox the other 1 s lines use");
        f194::Window again;
        check(box.take(&again) && !again.any(), "F194: ... and a second take with no write in between reads an empty window");
    }
    // ---- 7. the view's height at an edge --------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        char buf[900];
        // an engage that moves nothing prints a small change, not nothing
        f194::HeightProbe flat;
        flat.begin(true, 10000, true, 1.610, 0.0);
        bool done = false;
        for (uint64_t t = 10000; t <= 13600 && !done; t += 11) done = flat.tick(t, true, 1.6101, -2.0);
        flat.format(buf, sizeof(buf));
        check(done && std::strlen(buf) < 760 && std::strstr(buf, "look to aim height (F194) engage: the view's height") != nullptr && std::strstr(buf, "before 1.610 m, carried aim pitch +0.0 deg;") != nullptr &&
                  std::strstr(buf, "after 0.5 s +0.0 cm (1.610 m), carried aim pitch -2.0 deg;") != nullptr && std::strstr(buf, "after 1 s +0.0 cm") != nullptr && std::strstr(buf, "after 3 s +0.0 cm") != nullptr &&
                  std::strstr(buf, "not read") == nullptr,
              "F194: an engage that moves nothing prints +0.0 cm at 0.5 s, 1 s and 3 s, with the heights and the carried aim pitch (a small change, not nothing)");
        // the pilot's drop: 10 cm in the first half second, then more at 3 s
        f194::HeightProbe drop;
        drop.begin(true, 20000, true, 1.610, 0.0);
        done = false;
        for (uint64_t t = 20000; t <= 23600 && !done; t += 11) done = drop.tick(t, true, t < 20500 ? 1.610 : t < 21000 ? 1.510 : t < 23000 ? 1.505 : 1.500, 13.0);
        drop.format(buf, sizeof(buf));
        check(done && std::strstr(buf, "after 0.5 s -10.0 cm (1.510 m)") != nullptr && std::strstr(buf, "after 1 s -10.5 cm (1.505 m)") != nullptr && std::strstr(buf, "after 3 s -11.0 cm (1.500 m), carried aim pitch +13.0 deg") != nullptr,
              "F194: the pilot's drop reads as -10.0 cm at 0.5 s, -10.5 at 1 s, -11.0 at 3 s with the carried aim pitch at each");
        // the disengage is named so
        f194::HeightProbe dis;
        dis.begin(false, 1000, true, 1.5, 3.0);
        dis.format(buf, sizeof(buf));
        check(std::strstr(buf, "look to aim height (F194) disengage: the view's height") != nullptr, "F194: a disengage's probe says disengage");
        // no frame before: the afters are heights with no change
        f194::HeightProbe noBefore;
        noBefore.begin(true, 30000, false, 1.6, 0.0);
        done = false;
        for (uint64_t t = 30000; t <= 33600 && !done; t += 11) done = noBefore.tick(t, true, 1.55, 1.0);
        noBefore.format(buf, sizeof(buf));
        check(done && std::strstr(buf, "before not read;") != nullptr && std::strstr(buf, "after 0.5 s 1.550 m (no frame before to change from)") != nullptr && std::strstr(buf, " cm (") == nullptr,
              "F194: with no frame before, the line says so and gives the heights, no change");
        // no fresh frame at all: the three moments read not read, and the probe still ends
        f194::HeightProbe stale;
        stale.begin(true, 40000, true, 1.6, 0.0);
        done = false;
        for (uint64_t t = 40000; t <= 44000 && !done; t += 11) done = stale.tick(t, false, 0.0, 0.0);
        stale.format(buf, sizeof(buf));
        check(done && std::strstr(buf, "after 0.5 s not read; after 1 s not read; after 3 s not read") != nullptr, "F194: with no fresh frame the moments read not read (a probe that read nothing is not a probe that read 0 cm)");
        // a moment is served by a frame read at most 400 ms after it: a stall past that is not read, the later ones still are
        f194::HeightProbe gap;
        gap.begin(true, 50000, true, 1.6, 0.0);
        done = false;
        for (uint64_t t = 50000; t <= 54000 && !done; t += 11) {
            const bool fresh = !(t >= 50400 && t < 51000);   // no frame from 0.4 s to 1.0 s
            done = gap.tick(t, fresh, 1.58, 0.0);
        }
        gap.format(buf, sizeof(buf));
        check(done && std::strstr(buf, "after 0.5 s not read; after 1 s -2.0 cm") != nullptr && std::strstr(buf, "after 3 s -2.0 cm") != nullptr,
              "F194: a stall of frames over the 0.5 s moment (and 400 ms of grace) reads it as not read; the 1 s and 3 s moments are served afterwards");
        // a frame inside the grace serves the moment
        f194::HeightProbe late;
        late.begin(true, 60000, true, 1.6, 0.0);
        done = false;
        for (uint64_t t = 60000; t <= 64000 && !done; t += 11) done = late.tick(t, t >= 60700, 1.59, 0.0);
        late.format(buf, sizeof(buf));
        check(done && std::strstr(buf, "after 0.5 s -1.0 cm") != nullptr, "F194: a frame 200 ms after the 0.5 s moment (inside the grace) serves it");
        // a next edge inside the 3 s cuts it short and says so
        f194::HeightProbe cut;
        cut.begin(true, 70000, true, 1.6, 0.0);
        cut.tick(70500, true, 1.59, 0.0);
        cut.cutShort();
        cut.format(buf, sizeof(buf));
        check(std::strstr(buf, "after 0.5 s -1.0 cm") != nullptr && std::strstr(buf, "after 1 s not read (cut short by the next edge)") != nullptr && std::strstr(buf, "after 3 s not read (cut short by the next edge)") != nullptr,
              "F194: an edge inside the 3 s cuts the probe short: what it read stays, the rest reads not read (cut short by the next edge)");
        f194::HeightProbe idle;
        check(!idle.tick(1000, true, 1.0, 0.0) && !idle.active, "F194: a probe that was never begun does nothing");
        f194::HeightProbe nan;
        nan.begin(true, 80000, true, std::nan(""), 0.0);
        check(!nan.haveBefore, "F194: a frame before that is not a number is no frame before");
    }
    // ---- 8. the source sites ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini"), f109h = slurp("src/d3d11/native_on_foot_f109.h"), f194h = slurp("src/d3d11/native_on_foot_f194.h");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const auto at = [&](const std::string& needle) { return cpp.find(needle); };
        check(!cpp.empty() && !ini.empty() && !f109h.empty() && !f194h.empty(), "F194: the sources the pins read are readable from the repo root");
        // part 1: every caller of centreOffset passes the picture-centre pitch in force; the head steps stay the forward axis's
        check(countOf(cpp, "centreOffset(np.render, &oy, &op, g_ltaCentreRad)") == 3 && countOf(cpp, "centreOffset(") == 3,
              "F194: the three callers of centreOffset (the engage's centring, F133's yaw re-centre, the 1 s line's offset) pass g_ltaCentreRad, the pitch in force");
        check(f109h.find("inline bool centreOffset(const float render[12], double* yawRad, double* pitchRad, double pictureCentreRad = 0.0) noexcept {") != std::string::npos &&
                  f109h.find("if (!f194::rayAngles(render, pictureCentreRad, &y, &p)) return false;") != std::string::npos,
              "F194: centreOffset takes the ray through the middle of the picture (0 is adsHeadAngles itself)");
        check(has("const nofoot::f194::Frustum fr = nofoot::f194::frustumFromChannel(answered, static_cast<double>(topMag), static_cast<double>(botMag));") &&
                  has("const bool answered = eyeTangentsVertical(&topMag, &botMag);") && has("g_f194CentreWantedRad = nofoot::f194::pitchInForce(aimsOn, fr);") &&
                  has("g_ltaCentreRad = g_f194CentreWantedRad;   // F194: the picture-centre pitch this engagement centres on"),
              "F194: the channel is read where look to aim runs (topMag first, the swap is frustumFromChannel's), the wanted pitch is the key's and the frustum's, and an engage (a fresh follower) takes it");
        const size_t keyBlock = at("const bool ltaKey = g_lookKeyOn.load(std::memory_order_relaxed);"), frustumAt = at("const bool answered = eyeTangentsVertical(&topMag, &botMag);"),
                     wantAt = at("const bool want = ltaKey && !off && !g_lta.tripped;");
        check(keyBlock != std::string::npos && frustumAt != std::string::npos && wantAt != std::string::npos && keyBlock < frustumAt && frustumAt < wantAt,
              "F194: the frustum is read before the engage decision of the same tick, so an engage takes this tick's");
        // the sites that take the head's forward axis and were left alone, with the reason: a heading difference of one axis (F134's seat yaw), a lead measured beside the aim (F175, measure only)
        check(has("nofoot::adsHeadAngles(np.local, &yawLocal, &pitchLocal) && nofoot::adsHeadAngles(np.render, &yawSeated, &pitchSeated)") &&
                  has("nofoot::adsHeadAngles(in.render, &leadYaw, &leadPitch)"),
              "F194: F134's seat yaw (the difference of one axis's headings in two spaces) and F175's lead (measure only) still read the forward axis: neither is an aim");
        // the keys: spelled out in their own calls, default on, live; edvr.ini has both with a menu row; neither is in a shipped retired list
        check(has("cfg.getString(\"experimental.native_on_foot_look_to_aim_aims_at_picture_centre\", \"off\") == \"on\"") &&
                  has("cfg.getString(\"experimental.native_on_foot_look_to_aim_eye_height_as_mouse_look\", \"on\") != \"off\"") && has("std::atomic<bool> g_ltaAimsAtPictureCentre{false};") &&
                  has("std::atomic<bool> g_ltaEyeHeightAsMouseLook{true};"),
              "F194: both keys are read with their own literal; the picture-centre aim defaults off since F195 (the 2026-10-31 Quest flight refuted it for the pilot), the eye height key on");
        check(ini.find("\nnative_on_foot_look_to_aim_aims_at_picture_centre = off\n") != std::string::npos && ini.find("\nnative_on_foot_look_to_aim_eye_height_as_mouse_look = on\n") != std::string::npos &&
                  ini.find("# ui: Look to aim: aim at the middle of the picture (test) | choices on, off | recommended off | live\n") != std::string::npos &&
                  ini.find("# ui: Look to aim: eye height as mouse look (test) | choices on, off | recommended on | live\n") != std::string::npos &&
                  ini.find("# TEMPORARY (F194, A/B): Look to aim: aim at the middle of the picture") != std::string::npos && ini.find("# TEMPORARY (F194, A/B): Look to aim: eye height as mouse look") != std::string::npos &&
                  ini.find("\nnative_on_foot_pistol_sprint_swing_phase = 0.5\n\n# TEMPORARY (F194, A/B): Look to aim: aim at the middle") != std::string::npos,
              "F194: edvr.ini has both keys (the picture-centre aim off since F195, the eye height on), marked TEMPORARY, each with a menu row, after the pistol sprint swing row");
        check(has("a change while look to aim is engaged is taken "),
              "F194: the key's log line says a live change is taken at the next engage");
        // part 2: the 10 deg gate is the predicate's, skipped by the key, counted either way; F139's gate after it is unchanged; the old constant is gone
        const size_t wouldAt = at("if (si.valid && nofoot::f194::ltaGateWouldRefuse(f194CarriedAbs)) {"), countAt = at("g_f109SettleGated.fetch_add(1, std::memory_order_relaxed);"),
                     refuseAt = at("if (nofoot::f194::ltaGateRefuses(f194AsMouse, f194CarriedAbs)) {"),
                     f139At = at("if (si.valid && f138AimKnown && nofoot::f138::settleRefused(pc.f86AimPitch, nofoot::f138::kSettleGateDeg, f138Item)) {");
        check(wouldAt != std::string::npos && countAt != std::string::npos && refuseAt != std::string::npos && f139At != std::string::npos && wouldAt < countAt && countAt < refuseAt && refuseAt < f139At &&
                  has("const bool f194AsMouse = g_ltaEyeHeightAsMouseLook.load(std::memory_order_relaxed);") && has("const double f194CarriedAbs = static_cast<double>(g_f109AimPitchAbsDeg.load(std::memory_order_relaxed));") &&
                  !has("kF109SettlePitchDeg"),
              "F194: the 10 deg gate counts the call it would refuse, then refuses only with the key off, and runs before F139's gate, which is the same line as before; the old constant is gone");
        check(has("g_f109AimPitchAbsDeg.store(g_lta.engaged ? static_cast<float>(std::fabs(g_adsMap.pitchSeat) / nofoot::kAdsDeg) : 0.0f, std::memory_order_relaxed);"),
              "F194: the gate's input is the carried pitch (AdsAimMap::pitchSeat), unchanged");
        // the instruments: the hook's window, the view's height, the probe's edges, the 1 s line
        check(has("g_f194Box.write([&](nofoot::f194::Window& win) noexcept { win.add(c194); });") && has("c194.tenWould = f194TenWould;") && has("c194.tenRefused = f194TenRefused;") &&
                  has("c194.f139Refused = f138SettleRefused;") && has("c194.taken = so.settled;") && has("if (pc.f97EyeDrawnOk) c194.eyeOverNeckM = pc.f97EyeDrawn[1] - N0[1];") &&
                  has("g_f194ViewY.store(pc.f97EyeDrawn[1], std::memory_order_relaxed);") && has("if (freshIn && pc.f97EyeDrawnOk && refusal != kPinRefCameraAway && !(pc.detachW > 0.0)) {"),
              "F194: the pin hook writes the settle's fate, the drawn eye over the neck and the raise to the window, and the view's height (a fresh hip frame, the commander's camera) for the probe");
        check(has("f194Edge(now, true);   // F194: the view's height before the engage") && has("f194Edge(now, false);   // F194: the view's height before the disengage") &&
                  has("if (g_lta.engaged) f194Edge(GetTickCount64(), false);") && has("f194Tick(now);") && has("nofoot::f194::formatSecond(heightLine, sizeof(heightLine), g_f194Sec, hw,") &&
                  has("if (g_f194Box.take(&hw) && g_f194Sec.any()) {"),
              "F194: the probe is begun at the engage, the disengage and a stop, ticked every frame, and the 1 s line is taken from the hook's window while engaged");
        // the centred line gains the pitch in force and the forward axis's own offset
        check(has("the picture-centre pitch in force ") && has("deg (F194), the forward axis itself yaw %+.2f pitch %+.2f deg from the aim; pitch kept %+.2f deg, centred %+.2f deg (F195: %s); %s"),
              "F194: the F109 centred line adds the picture-centre pitch in force and the forward axis's own offset beside the ray's");
    }
}

}  // namespace f194test
