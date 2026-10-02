// The intro composite's constants read as a world-space panel (src\d3d11\intro_curve_math.h): the pure half of the intro movie and the
// splash following fix.panel_curvature. No D3D, no files: 20 floats in, a reading out.
//
// WHAT IS PINNED, by case (every check carries a label "C<n>.<what>"; tools\intro_curve_math_test\mutants.py compiles this rig against a
// copy of the header with ONE rule flipped, runs that rule's case alone, and requires the label of the first FAIL to start with the
// case's id):
//   C1   the two real splash captures (the panel in front, and behind after a 180-degree yaw) read ok, half-width 4.44444, toward +1
//   C2   the movie's stock constants (either eye) read as screen space and are refused in those words
//   C3   every refusal in its own words, each alone (all zeros included), and a refusal leaves no half-width or direction behind
//   C4   NaN and infinity in each of the 20 slots, on both captures: refused as not finite, before anything else is asked
//   C5   cb2[3]'s length: too short, too long, both edges inclusive, every component counts, either sign
//   C6   cb2[3].w and cb2[4].w clear of zero: exactly 0, 1e-4, the strict edge 1e-3, either sign, in either place
//   C7   cb2[0].x and cb2[0].y plausible half-sizes: each outside value in each component, the edges inclusive; the half-width is x
//   C8   the depth direction: all four sign pairs, by the signs of cb2[3].w and cb2[4].w and of nothing else
//   C9   when several conditions fail at once, the first in the stated order is the one named
//   C10  today's screen-space rule on a table of vectors with hand-derived answers (the boundary values, the quirks)
//   C11  that rule against a VERBATIM copy of intro_panel.cpp's looksScreenSpace, over every boundary and its neighbours, one slot,
//        two slots and a deterministic sample of whole vectors at a time
//   C12  which way the placement's +x runs (introPlacementXDir), on the real readings: both captures, both eyes of each, and the movie's stock
//        constants, either eye, all run LEFT; the same constants with the x column negated run RIGHT; introReadWorldCb carries the answer
//   C13  the same rule's margin (just above and just below 0.05 of the two terms' magnitudes, in the four signs, at any scale), the exact edge,
//        float rounding that only double arithmetic gets right, and the degenerate inputs (every term zero, NaN, infinity); only the four floats
//        it reads are read
//   C14  introReadWorldCb with a direction that cannot be told: not ok, the new reason, nothing left behind; every refusal leaves xDir unknown
//   C15  the answer against geometry computed independently in double (a head turned through every yaw, pitch and roll, both eyes, a panel of
//        either frame), and a panel turned toward edge-on: a known answer is always the right one, an unknown one always has a ratio under 0.05
//
// Usage: --self-test [--only C1,C4,...]   |   --dry-run (no checks run)
#include "intro_curve_math.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace edvr;

// ---- the harness ---------------------------------------------------------------------------------------------------------
unsigned g_checks = 0;
void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) throw std::runtime_error(label);
}
void check(bool ok, const std::string& label) { check(ok, label.c_str()); }

const float kNan = std::numeric_limits<float>::quiet_NaN();
const float kInf = std::numeric_limits<float>::infinity();
float up(float v) { return std::nextafter(v, kInf); }    // the next float above v
float dn(float v) { return std::nextafter(v, -kInf); }   // the next float below v

struct Cb {
    float f[20];
};
Cb make(std::initializer_list<float> v) {
    Cb c = {};
    size_t i = 0;
    for (float x : v) c.f[i++] = x;
    return c;
}

// The two real splash captures (Frontier log edvr_gfx_20260828_182818.log, the `DCW read` lines) and the movie's stock constants: the
// numbers of the header's comment.
Cb capture2() {   // the panel in front of the viewer, cb2[4].w = +3.76
    return make({4.44444f, 2.5f, 0.0f, 0.0f, -0.780684f, -0.0197438f, 9.48621e-08f, -0.000948464f, -0.0047429f, 0.788105f, -7.60756e-06f, 0.076063f,
                 -0.194148f, 0.0601386f, 9.97268e-05f, -0.997103f, -0.0725725f, -0.247872f, 0.0996338f, 3.76108f});
}
Cb capture1() {   // yaw about 180 out, the panel behind the viewer, cb2[4].w = -1.59
    return make({4.44444f, 2.5f, 0.0f, 0.0f, 0.795297f, 0.0141897f, -9.32133e-06f, 0.0931979f, -0.0047877f, 0.788078f, -7.64753e-06f, 0.0764627f,
                 0.121095f, -0.0620333f, -9.92872e-05f, 0.992707f, -0.908756f, 18.4664f, 0.100169f, -1.5911f});
}
Cb stockLeft() {   // docs\frontier-intro-video-report.md, left eye
    return make({512.0f, 288.0f, 0.0f, 0.0f, -0.000368732f, 0.0f, 0.0f, 0.0f, 0.0f, 0.000373413f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.193907f, 0.0f, 0.0f,
                 1.0f});
}
Cb stockRight() {   // the other eye: the frustum centre cb2[4].x flips sign (measured -0.1940)
    Cb c = stockLeft();
    c.f[16] = -0.1940f;
    return c;
}
// The other eye of each capture (the same log, the second `DCW read` line of each pair: q=41 and q=42 against q=37).
Cb capture2Eye2() {   // the panel in front of the viewer, the other eye: cb2[1].x -0.780317, cb2[4].x -1.57885
    return make({4.44444f, 2.5f, 0.0f, 0.0f, -0.780317f, -0.0197438f, 9.48621e-08f, -0.000948464f, -0.0342501f, 0.788105f, -7.60756e-06f, 0.076063f,
                 0.192659f, 0.0601386f, 9.97268e-05f, -0.997103f, -1.57885f, -0.247872f, 0.0996338f, 3.76108f});
}
Cb capture1Eye2() {   // the panel behind the viewer, the other eye: cb2[1].x +0.759143, cb2[4].x -0.338754
    return make({4.44444f, 2.5f, 0.0f, 0.0f, 0.759143f, 0.0141897f, -9.32133e-06f, 0.0931979f, -0.0344499f, 0.788078f, -7.64753e-06f, 0.0764627f,
                 -0.264007f, -0.0620333f, -9.92872e-05f, 0.992707f, -0.338754f, 18.4664f, 0.100169f, -1.5911f});
}
// capture 2 with cb2[3] replaced (the z column: slots 12..15)
Cb withCol3(float a, float b, float c, float d) {
    Cb v = capture2();
    v.f[12] = a;
    v.f[13] = b;
    v.f[14] = c;
    v.f[15] = d;
    return v;
}
// a unit-length cb2[3] = (0.8, 0.6, 0, w3) and cb2[4].w = w4 on capture 2: the two w's can be set without touching the length rule
Cb withW(float w3, float w4) {
    Cb v = withCol3(0.8f, 0.6f, 0.0f, w3);
    v.f[19] = w4;
    return v;
}
// The two terms of the x direction, written out as the header's rule forms them: a = cb2[1].x * cb2[4].w, b = cb2[4].x * cb2[1].w, each a
// product of two floats (exact in double), and the ratio |a - b| / (|a| + |b|) it holds against its margin.
double termA(const Cb& c) { return static_cast<double>(c.f[4]) * static_cast<double>(c.f[19]); }
double termB(const Cb& c) { return static_cast<double>(c.f[16]) * static_cast<double>(c.f[7]); }
double ratioOf(const Cb& c) {
    const double a = termA(c), b = termB(c);
    const double scale = std::fabs(a) + std::fabs(b);
    return scale > 0.0 ? std::fabs(a - b) / scale : 0.0;
}
// capture 2 with the four floats the x direction reads replaced (cb2[1].x, cb2[4].w, cb2[4].x, cb2[1].w): every other rule still passes it
Cb withTerms(float f4, float f19, float f16, float f7) {
    Cb v = capture2();
    v.f[4] = f4;
    v.f[19] = f19;
    v.f[16] = f16;
    v.f[7] = f7;
    return v;
}
// capture 2 with its centre's x moved until the two terms cancel: a panel seen edge-on. Every rule before the direction passes it.
Cb edgeOn() {
    Cb c = capture2();
    c.f[16] = static_cast<float>(termA(c) / static_cast<double>(c.f[7]));
    return c;
}
Cb negatedX(Cb c) {   // cb2[1] = -cb2[1]: the x column run the other way
    for (int i = 4; i < 8; ++i) c.f[i] = -c.f[i];
    return c;
}

// The reasons, as the header words them (the later log line prints one of these).
const char* const kWhyFinite = "a constant is not a finite number";
const char* const kWhyScreen = "the constants read as a screen-space placement (cb2[3] is zero and w is a constant 1), not a world-space panel";
const char* const kWhyLength = "cb2[3] is not a unit-scale column (its length is outside 0.5 to 2)";
const char* const kWhyW3 = "cb2[3].w is too close to zero to tell which way the panel's depth runs";
const char* const kWhyW4 = "cb2[4].w is too close to zero to tell which way the panel's depth runs";
const char* const kWhyX = "cb2[0].x is not a plausible half-width in metres (outside 0.5 to 50)";
const char* const kWhyY = "cb2[0].y is not a plausible half-height in metres (outside 0.5 to 50)";
const char* const kWhyDir = "the placement's +x cannot be told to run left or right (the panel is seen edge-on, or its x column and its centre cancel)";

// Refused for exactly this reason, and nothing left behind (half-width 0, depth direction 0, x direction unknown).
bool refused(const Cb& c, const char* why) {
    const IntroWorldCb r = introReadWorldCb(c.f);
    return !r.ok && r.halfWidth == 0.0f && r.toward == 0 && r.xDir == IntroXDir::kUnknown && r.why != nullptr && std::strcmp(r.why, why) == 0;
}
// An ok reading, which always says which way +x runs (left or right, never unknown).
bool readsOk(const Cb& c) {
    const IntroWorldCb r = introReadWorldCb(c.f);
    return r.ok && (r.xDir == IntroXDir::kLeft || r.xDir == IntroXDir::kRight);
}

// ---- C1: the two real captures ---------------------------------------------------------------------------------------------
void caseC1() {
    const Cb a = capture2(), b = capture1();
    const IntroWorldCb ra = introReadWorldCb(a.f), rb = introReadWorldCb(b.f);
    check(ra.ok && ra.halfWidth == 4.44444f && ra.toward == 1, "C1.capture 2 (the panel in front, cb2[4].w +3.76) reads ok: half-width 4.44444, toward +1");
    check(rb.ok && rb.halfWidth == 4.44444f && rb.toward == 1,
          "C1.capture 1 (yaw about 180 out, the panel behind, cb2[4].w -1.59) reads ok: half-width 4.44444, toward +1");
    check(a.f[15] < 0.0f && a.f[19] > 0.0f && b.f[15] > 0.0f && b.f[19] < 0.0f,
          "C1.the convention holds in both captures, read off the data: cb2[3].w and cb2[4].w have opposite signs (-0.997 / +3.761, +0.993 / -1.591)");
    check(ra.why != nullptr && rb.why != nullptr && ra.why[0] == 0 && rb.why[0] == 0, "C1.an ok reading carries an empty static reason, never a null one");
    check(!introCbLooksScreenSpace(a.f) && !introCbLooksScreenSpace(b.f), "C1.neither capture reads as a screen-space placement");
}

// ---- C2: the movie's stock constants ---------------------------------------------------------------------------------------
void caseC2() {
    const Cb l = stockLeft(), r = stockRight();
    check(introCbLooksScreenSpace(l.f) && introCbLooksScreenSpace(r.f), "C2.today's rule reads the movie's stock constants, either eye, as screen space");
    check(refused(l, kWhyScreen), "C2.the movie's stock constants (left eye) do not read ok, and the reason is the screen-space placement");
    check(refused(r, kWhyScreen), "C2.nor do the right eye's (cb2[4].x flips sign)");
    // (The stock constants fail the length and cb2[3].w rules as well, cb2[3] being all zero: the screen-space reason is the one named,
    // because it is asked first. C9 pins the order.)
}

// ---- C3: every refusal in its own words ------------------------------------------------------------------------------------
void caseC3() {
    {
        Cb c = capture2();
        c.f[2] = kNan;   // a slot no other rule reads
        check(refused(c, kWhyFinite), "C3.a NaN in a slot no other rule reads: the finite reason");
    }
    check(refused(stockLeft(), kWhyScreen), "C3.the stock constants: the screen-space reason");
    check(refused(withCol3(0.1f, 0.0f, 0.0f, -0.4f), kWhyLength), "C3.cb2[3] of length 0.41: the length reason");
    check(refused(withW(0.0005f, 3.76108f), kWhyW3), "C3.|cb2[3].w| 0.0005 on a unit column: the cb2[3].w reason");
    check(refused(withW(-0.5f, 0.0005f), kWhyW4), "C3.|cb2[4].w| 0.0005: the cb2[4].w reason");
    {
        Cb c = capture2();
        c.f[0] = 0.4f;
        check(refused(c, kWhyX), "C3.cb2[0].x 0.4: the half-width reason");
    }
    {
        Cb c = capture2();
        c.f[1] = 60.0f;
        check(refused(c, kWhyY), "C3.cb2[0].y 60: the half-height reason");
    }
    check(refused(make({}), kWhyLength), "C3.all zeros: finite, not screen space (the half-size is under 16), and cb2[3] has no length: the length reason");
    check(refused(edgeOn(), kWhyDir), "C3.a placement whose x column and centre cancel (seen edge-on): the direction reason");
    // the eight reasons are eight different texts
    const char* all[] = {kWhyFinite, kWhyScreen, kWhyLength, kWhyW3, kWhyW4, kWhyX, kWhyY, kWhyDir};
    bool distinct = true;
    for (size_t i = 0; i < 8; ++i)
        for (size_t j = i + 1; j < 8; ++j) distinct = distinct && std::strcmp(all[i], all[j]) != 0;
    check(distinct, "C3.the eight reasons are eight different texts");
}

// ---- C4: NaN and infinity in every slot ------------------------------------------------------------------------------------
void caseC4() {
    const float bad[3] = {kNan, kInf, -kInf};
    const char* const badName[3] = {"NaN", "+infinity", "-infinity"};
    for (int base = 0; base < 2; ++base) {
        for (int slot = 0; slot < 20; ++slot) {
            for (int k = 0; k < 3; ++k) {
                Cb c = base == 0 ? capture2() : capture1();
                c.f[slot] = bad[k];
                char label[160];
                std::snprintf(label, sizeof(label), "C4.%s in slot %d of capture %d is refused as not finite, before anything else is asked", badName[k], slot,
                              base == 0 ? 2 : 1);
                check(refused(c, kWhyFinite), label);
            }
        }
    }
    {   // a huge but finite value in a slot nothing reads is still finite
        Cb c = capture2();
        c.f[2] = 1e30f;
        c.f[3] = -1e30f;
        check(readsOk(c), "C4.a huge but finite value (1e30) in the slots no rule reads is not refused");
    }
}

// ---- C5: cb2[3]'s length ---------------------------------------------------------------------------------------------------
void caseC5() {
    check(refused(withCol3(0.0f, 0.0f, 0.0f, 0.4999f), kWhyLength), "C5.length 0.4999 is too short");
    check(readsOk(withCol3(0.0f, 0.0f, 0.0f, 0.5f)), "C5.length 0.5 is the inclusive lower edge");
    check(readsOk(withCol3(0.0f, 0.0f, 0.0f, 1.0f)), "C5.length 1 reads ok");
    check(readsOk(withCol3(0.0f, 0.0f, 0.0f, 2.0f)), "C5.length 2 is the inclusive upper edge");
    check(refused(withCol3(0.0f, 0.0f, 0.0f, 2.0001f), kWhyLength), "C5.length 2.0001 is too long");
    check(readsOk(withCol3(0.0f, 0.0f, 0.0f, -0.5f)) && readsOk(withCol3(0.0f, 0.0f, 0.0f, -2.0f)), "C5.the edges are the same with the other sign");
    check(refused(withCol3(0.0f, 0.0f, 0.0f, -0.4999f), kWhyLength) && refused(withCol3(0.0f, 0.0f, 0.0f, -2.0001f), kWhyLength),
          "C5.and just outside them with the other sign");
    // Every component counts: (0.25, 0.25, 0.25, 0.25) has length exactly 0.5; take any one away and it is too short.
    check(readsOk(withCol3(0.25f, 0.25f, 0.25f, 0.25f)), "C5.four components of 0.25 make length exactly 0.5: ok");
    check(refused(withCol3(0.0f, 0.25f, 0.25f, 0.25f), kWhyLength), "C5.without cb2[3].x the same column is too short");
    check(refused(withCol3(0.25f, 0.0f, 0.25f, 0.25f), kWhyLength), "C5.without cb2[3].y the same column is too short");
    check(refused(withCol3(0.25f, 0.25f, 0.0f, 0.25f), kWhyLength), "C5.without cb2[3].z the same column is too short");
    check(refused(withCol3(0.25f, 0.25f, 0.25f, 0.0f), kWhyLength), "C5.without cb2[3].w the same column is too short");
    // (1, 1, 1, 1) has length exactly 2; any one component a little larger and it is too long, any one dropped it would read ok.
    check(readsOk(withCol3(1.0f, 1.0f, 1.0f, 1.0f)), "C5.four components of 1 make length exactly 2: ok");
    check(refused(withCol3(1.0001f, 1.0f, 1.0f, 1.0f), kWhyLength), "C5.cb2[3].x a little over 1 makes it too long");
    check(refused(withCol3(1.0f, 1.0001f, 1.0f, 1.0f), kWhyLength), "C5.cb2[3].y a little over 1 makes it too long");
    check(refused(withCol3(1.0f, 1.0f, 1.0001f, 1.0f), kWhyLength), "C5.cb2[3].z a little over 1 makes it too long");
    check(refused(withCol3(1.0f, 1.0f, 1.0f, 1.0001f), kWhyLength), "C5.cb2[3].w a little over 1 makes it too long");
    // the length is the Euclidean one: (1.2, 1.2, 0, 0.5) has squares summing to 3.13 (length 1.77): ok; (1.5, 1.5, 0, 0.5) sums to 4.75 (2.18): too long
    check(readsOk(withCol3(1.2f, 1.2f, 0.0f, 0.5f)), "C5.a column of length 1.77 is ok (the sum of the four squares is under 4)");
    check(refused(withCol3(1.5f, 1.5f, 0.0f, 0.5f), kWhyLength), "C5.a column of length 2.18 is too long");
}

// ---- C6: cb2[3].w and cb2[4].w clear of zero --------------------------------------------------------------------------------
void caseC6() {
    const float clear[4] = {0.0011f, -0.0011f, 0.002f, -0.002f};
    const float notClear[7] = {0.0f, 1e-4f, -1e-4f, 1e-3f, -1e-3f, 0.0009f, -0.0009f};
    for (float w : notClear) {
        char label[120];
        std::snprintf(label, sizeof(label), "C6.cb2[3].w %g is not clear of zero (strictly more than 1e-3 in magnitude is needed)", static_cast<double>(w));
        check(refused(withW(w, 3.76108f), kWhyW3), label);
        std::snprintf(label, sizeof(label), "C6.cb2[4].w %g is not clear of zero (strictly more than 1e-3 in magnitude is needed)", static_cast<double>(w));
        check(refused(withW(-0.5f, w), kWhyW4), label);
    }
    for (float w : clear) {
        char label[120];
        std::snprintf(label, sizeof(label), "C6.cb2[3].w %g is clear of zero, either sign", static_cast<double>(w));
        check(readsOk(withW(w, 3.76108f)), label);
        std::snprintf(label, sizeof(label), "C6.cb2[4].w %g is clear of zero, either sign", static_cast<double>(w));
        check(readsOk(withW(-0.5f, w)), label);
    }
    check(readsOk(withW(-0.9f, -7.5f)) && readsOk(withW(0.9f, 7.5f)), "C6.large w's of either sign read ok");
}

// ---- C7: cb2[0].x and cb2[0].y ---------------------------------------------------------------------------------------------
void caseC7() {
    struct V { float v; bool ok; };
    const V values[] = {{0.0f, false}, {-4.44444f, false}, {0.4999f, false}, {0.5f, true}, {4.44444f, true}, {2.5f, true}, {50.0f, true}, {50.001f, false}, {1e6f, false}};
    for (const V& x : values) {
        Cb c = capture2();
        c.f[0] = x.v;
        char label[120];
        std::snprintf(label, sizeof(label), "C7.cb2[0].x %g is %s", static_cast<double>(x.v), x.ok ? "a plausible half-width" : "refused as the half-width");
        check(x.ok ? readsOk(c) : refused(c, kWhyX), label);
        c = capture2();
        c.f[1] = x.v;
        std::snprintf(label, sizeof(label), "C7.cb2[0].y %g is %s", static_cast<double>(x.v), x.ok ? "a plausible half-height" : "refused as the half-height");
        check(x.ok ? readsOk(c) : refused(c, kWhyY), label);
    }
    {   // the half-width is x, and not y
        Cb c = capture2();
        c.f[0] = 7.0f;
        c.f[1] = 3.0f;
        const IntroWorldCb r = introReadWorldCb(c.f);
        check(r.ok && r.halfWidth == 7.0f, "C7.the half-width is cb2[0].x (7), not cb2[0].y (3)");
        c.f[0] = 0.5f;
        c.f[1] = 50.0f;
        check(introReadWorldCb(c.f).halfWidth == 0.5f, "C7.at the lower edge the half-width is 0.5");
        c.f[0] = 50.0f;
        c.f[1] = 0.5f;
        check(introReadWorldCb(c.f).halfWidth == 50.0f, "C7.at the upper edge the half-width is 50");
    }
    {   // x and y are judged separately
        Cb c = capture2();
        c.f[0] = 0.4f;
        check(refused(c, kWhyX), "C7.x outside, y fine: the half-width reason");
        c = capture2();
        c.f[1] = 0.4f;
        check(refused(c, kWhyY), "C7.y outside, x fine: the half-height reason");
    }
}

// ---- C8: the depth direction -----------------------------------------------------------------------------------------------
void caseC8() {
    auto toward = [](float w3, float w4) {
        const IntroWorldCb r = introReadWorldCb(withW(w3, w4).f);
        return r.ok ? r.toward : 99;
    };
    check(toward(0.5f, 2.0f) == -1, "C8.cb2[3].w positive, cb2[4].w positive: the same sign, away (-1)");
    check(toward(-0.5f, -2.0f) == -1, "C8.cb2[3].w negative, cb2[4].w negative: the same sign, away (-1)");
    check(toward(0.5f, -2.0f) == 1, "C8.cb2[3].w positive, cb2[4].w negative: opposite signs, toward (+1)");
    check(toward(-0.5f, 2.0f) == 1, "C8.cb2[3].w negative, cb2[4].w positive: opposite signs, toward (+1)");
    check(toward(0.0011f, 100.0f) == -1 && toward(0.0011f, -100.0f) == 1, "C8.only the signs matter, not the sizes");
    {   // nothing else matters: negate every other slot that no rule bounds, and the reading does not change
        Cb c = withW(-0.5f, 2.0f);
        const int skip[] = {0, 1, 15, 19};   // the half-sizes (must stay in range) and the two w's
        for (int i = 0; i < 20; ++i) {
            bool kept = false;
            for (int s : skip) kept = kept || s == i;
            if (!kept) c.f[i] = -c.f[i];
        }
        const IntroWorldCb r = introReadWorldCb(c.f);
        check(r.ok && r.toward == 1 && r.halfWidth == 4.44444f, "C8.negating every other slot changes neither the direction (+1) nor the half-width");
    }
}

// ---- C9: the first failed condition is the one named -----------------------------------------------------------------------
void caseC9() {
    {
        Cb c = stockLeft();
        c.f[2] = kNan;
        check(refused(c, kWhyFinite), "C9.not finite and screen space: finite is named first");
    }
    check(refused(stockLeft(), kWhyScreen), "C9.screen space, a zero cb2[3] and a half-size over 50: screen space is named first");
    check(refused(withCol3(0.0f, 0.0f, 0.0f, 0.0005f), kWhyLength), "C9.a cb2[3] too short and with a w near zero: length is named first");
    {
        Cb c = withW(0.0005f, 0.0005f);
        check(refused(c, kWhyW3), "C9.both w's near zero: cb2[3].w is named first");
        c.f[0] = 0.4f;
        c.f[1] = 60.0f;
        check(refused(c, kWhyW3), "C9.both w's near zero and both half-sizes out: cb2[3].w is named first");
    }
    {
        Cb c = capture2();
        c.f[19] = 0.0005f;
        c.f[0] = 0.4f;
        check(refused(c, kWhyW4), "C9.cb2[4].w near zero and the half-width out: cb2[4].w is named first");
    }
    {
        Cb c = capture2();
        c.f[0] = 0.4f;
        c.f[1] = 60.0f;
        check(refused(c, kWhyX), "C9.both half-sizes out: the half-width is named first");
    }
    {
        Cb c = make({});
        c.f[5] = kInf;
        check(refused(c, kWhyFinite), "C9.all zeros but one infinity: finite is named first, though the length fails too");
    }
    // The direction is asked LAST: a placement that cannot say which way +x runs, and fails anything else too, is refused for the other reason.
    {
        Cb c = edgeOn();
        c.f[1] = 60.0f;
        check(refused(c, kWhyY), "C9.the direction cannot be told and the half-height is out: the half-height is named");
        c = edgeOn();
        c.f[0] = 0.4f;
        check(refused(c, kWhyX), "C9.the direction cannot be told and the half-width is out: the half-width is named");
        c = edgeOn();
        c.f[19] = 0.0005f;
        c.f[16] = static_cast<float>(termA(c) / static_cast<double>(c.f[7]));   // still cancelling
        check(refused(c, kWhyW4), "C9.the direction cannot be told and cb2[4].w is near zero: cb2[4].w is named");
        c = edgeOn();
        c.f[12] = c.f[13] = c.f[14] = 0.0f;
        c.f[15] = 0.1f;
        check(refused(c, kWhyLength), "C9.the direction cannot be told and cb2[3] is too short: the length is named");
        check(refused(edgeOn(), kWhyDir), "C9.the direction cannot be told and nothing else is wrong: the direction reason");
    }
}

// ---- C10: today's screen-space rule on a table with hand-derived answers ---------------------------------------------------
struct Row {
    std::string name;
    Cb v;
    bool screen;
};
void caseC10() {
    std::vector<Row> rows;
    auto add = [&](const std::string& name, const Cb& v, bool screen) { rows.push_back({name, v, screen}); };
    auto slot = [&](const std::string& name, int s, float value, bool screen) {
        Cb c = stockLeft();
        c.f[s] = value;
        char buf[48];
        std::snprintf(buf, sizeof(buf), " (slot %d = %g)", s, static_cast<double>(value));
        add(name + buf, c, screen);
    };
    add("the movie's stock constants, left eye", stockLeft(), true);
    add("the movie's stock constants, right eye", stockRight(), true);
    add("splash capture 2 is world space", capture2(), false);
    add("splash capture 1 is world space", capture1(), false);
    // the half-size: at least 16 in both components, no upper bound
    for (int s = 0; s < 2; ++s) {
        slot("the half-size at 16 is screen space (the edge is inclusive)", s, 16.0f, true);
        slot("the half-size at 15.99 is not", s, 15.99f, false);
        slot("the half-size at 16.01 is screen space", s, 16.01f, true);
        slot("a zero half-size is not", s, 0.0f, false);
        slot("a negative half-size is not", s, -16.0f, false);
        slot("a huge half-size is screen space (there is no upper bound)", s, 1e9f, true);
    }
    {
        Cb c = stockLeft();
        c.f[0] = 16.0f;
        c.f[1] = 15.99f;
        add("x at 16, y at 15.99: not (both must reach 16)", c, false);
        c.f[0] = 15.99f;
        c.f[1] = 16.0f;
        add("x at 15.99, y at 16: not", c, false);
        c.f[0] = 15.99f;
        c.f[1] = 15.99f;
        add("both at 15.99: not", c, false);
        c.f[0] = 16.0f;
        c.f[1] = 16.0f;
        add("both at 16: screen space", c, true);
    }
    // cb2[4].w: 1 within 0.999 .. 1.001, both edges inclusive
    slot("w at 1 is screen space", 19, 1.0f, true);
    slot("w at 0.999 is screen space (the edge is inclusive)", 19, 0.999f, true);
    slot("w at 0.9989 is not", 19, 0.9989f, false);
    slot("w at 1.001 is screen space (the edge is inclusive)", 19, 1.001f, true);
    slot("w at 1.0011 is not", 19, 1.0011f, false);
    slot("w at 0.998 is not", 19, 0.998f, false);
    slot("w at 1.002 is not", 19, 1.002f, false);
    slot("w at 0 is not", 19, 0.0f, false);
    slot("w at -1 is not", 19, -1.0f, false);
    slot("w at 2 is not", 19, 2.0f, false);
    // the zero test on cb2[1].w, cb2[2].w and each component of cb2[3]: the open interval (-1e-9, 1e-9)
    const int zeroSlots[6] = {7, 11, 12, 13, 14, 15};
    for (int s : zeroSlots) {
        slot("zero is screen space", s, 0.0f, true);
        slot("-0 is screen space", s, -0.0f, true);
        slot("0.5e-9 is zero", s, 0.5e-9f, true);
        slot("-0.5e-9 is zero", s, -0.5e-9f, true);
        slot("exactly 1e-9 is not zero (the interval is open)", s, 1e-9f, false);
        slot("exactly -1e-9 is not zero (the interval is open)", s, -1e-9f, false);
        slot("2e-9 is not zero", s, 2e-9f, false);
        slot("-2e-9 is not zero", s, -2e-9f, false);
        slot("1e-6 is not zero", s, 1e-6f, false);
        slot("1 is not zero", s, 1.0f, false);
    }
    // the slots the rule does not read: anything there is still screen space
    const int unread[11] = {2, 3, 4, 5, 6, 8, 9, 10, 16, 17, 18};
    for (int s : unread) {
        slot("an unread slot at 7 does not matter", s, 7.0f, true);
        slot("an unread slot at -100 does not matter", s, -100.0f, true);
    }
    // NaN and infinity, as today's rule answers them: the zero test refuses them, the w and half-size tests (which reject only what lies
    // outside) let a NaN through, and the half-size has no upper bound
    for (int s : zeroSlots) {
        slot("NaN is not zero", s, kNan, false);
        slot("+infinity is not zero", s, kInf, false);
        slot("-infinity is not zero", s, -kInf, false);
    }
    for (int s = 0; s < 2; ++s) {
        slot("a NaN half-size passes (only what lies outside is rejected)", s, kNan, true);
        slot("a +infinity half-size passes (no upper bound)", s, kInf, true);
        slot("a -infinity half-size is below 16", s, -kInf, false);
    }
    slot("a NaN w passes (only what lies outside is rejected)", 19, kNan, true);
    slot("a +infinity w is above 1.001", 19, kInf, false);
    slot("a -infinity w is below 0.999", 19, -kInf, false);
    for (const Row& row : rows) check(introCbLooksScreenSpace(row.v.f) == row.screen, "C10." + row.name);
}

// ---- C11: parity with today's rule -----------------------------------------------------------------------------------------
// intro_panel.cpp's looksScreenSpace, VERBATIM (2026-10-01, HEAD 3f226fcb), kept here so the rig needs none of that file's D3D.
bool referenceLooksScreenSpace(const float* f) {
    auto zero = [](float v) { return v > -1e-9f && v < 1e-9f; };
    if (!zero(f[7]) || !zero(f[11])) return false;       // cb2[1].w, cb2[2].w
    for (uint32_t i = 12; i < 16; ++i) {                 // cb2[3] entirely
        if (!zero(f[i])) return false;
    }
    if (f[19] < 0.999f || f[19] > 1.001f) return false;  // cb2[4].w == 1
    // The scale must be a plausible half-size in pixels. A world-space quad
    // measured 4.4 by 2.5 units; a screen-space one measured 512 by 288.
    if (f[0] < 16.0f || f[1] < 16.0f) return false;
    return true;
}
void caseC11() {
    // The nine slots the rule reads, and for each the values around its thresholds: the threshold itself and the float on either side.
    const std::vector<float> zeroish = {0.0f, -0.0f, 1e-9f, -1e-9f, up(1e-9f), dn(1e-9f), up(-1e-9f), dn(-1e-9f), 0.5e-9f, -0.5e-9f, 2e-9f, -2e-9f,
                                        1e-6f, -1e-6f, 1e-3f, -1e-3f, 0.25f, 1.0f, -1.0f, kNan, kInf, -kInf};
    const std::vector<float> wValues = {1.0f, 0.999f, up(0.999f), dn(0.999f), 1.001f, up(1.001f), dn(1.001f), 0.998f, 1.002f, 0.0f, -1.0f, 2.0f, 3.76108f,
                                        -1.5911f, 1e30f, kNan, kInf, -kInf};
    const std::vector<float> halfValues = {512.0f, 288.0f, 16.0f, up(16.0f), dn(16.0f), 15.99f, 16.01f, 0.0f, -16.0f, 4.44444f, 2.5f, 8.0f, 32.0f, 1e9f, 3.4e38f,
                                           kNan, kInf, -kInf};
    struct Slot {
        int index;
        const std::vector<float>* values;
    };
    const Slot slots[9] = {{0, &halfValues}, {1, &halfValues}, {7, &zeroish}, {11, &zeroish}, {12, &zeroish}, {13, &zeroish}, {14, &zeroish}, {15, &zeroish},
                           {19, &wValues}};
    auto same = [](const Cb& c) { return introCbLooksScreenSpace(c.f) == referenceLooksScreenSpace(c.f); };
    uint64_t compared = 0;
    // one slot at a time, on the stock constants
    for (const Slot& s : slots) {
        for (float v : *s.values) {
            Cb c = stockLeft();
            c.f[s.index] = v;
            ++compared;
            char label[120];
            std::snprintf(label, sizeof(label), "C11.slot %d at %.9g (one slot varied) reads as today's rule reads it", s.index, static_cast<double>(v));
            check(same(c), label);
        }
    }
    // two slots at a time
    for (int a = 0; a < 9; ++a) {
        for (int b = a + 1; b < 9; ++b) {
            for (float va : *slots[a].values) {
                for (float vb : *slots[b].values) {
                    Cb c = stockLeft();
                    c.f[slots[a].index] = va;
                    c.f[slots[b].index] = vb;
                    ++compared;
                    char label[160];
                    std::snprintf(label, sizeof(label), "C11.slots %d at %.9g and %d at %.9g (two slots varied) read as today's rule reads them", slots[a].index,
                                  static_cast<double>(va), slots[b].index, static_cast<double>(vb));
                    check(same(c), label);
                }
            }
        }
    }
    // a deterministic sample of whole vectors: the nine slots from their candidates, every other slot from a pool with NaN and infinity in it
    const float pool[9] = {0.0f, 1.0f, -1.0f, 0.5f, 4.44444f, 512.0f, kNan, kInf, -kInf};
    uint32_t seed = 20261001u;
    auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    for (uint32_t n = 0; n < 60000; ++n) {
        Cb c = {};
        for (int i = 0; i < 20; ++i) c.f[i] = pool[next() % 9];
        for (const Slot& s : slots) c.f[s.index] = (*s.values)[next() % s.values->size()];
        ++compared;
        char label[96];
        std::snprintf(label, sizeof(label), "C11.sampled vector %u reads as today's rule reads it", n);
        check(same(c), label);
    }
    check(compared > 50000, "C11.the sweep compared every vector (a short sweep proves nothing)");
}

// ---- C12: which way +x runs, on the real readings --------------------------------------------------------------------------
void caseC12() {
    struct Real {
        const char* name;
        Cb cb;
    };
    const Real real[] = {{"capture 2 (the panel in front), eye 1", capture2()},
                         {"capture 2, eye 2", capture2Eye2()},
                         {"capture 1 (the panel behind), eye 1", capture1()},
                         {"capture 1, eye 2", capture1Eye2()}};
    for (const Real& r : real) {
        const std::string name = r.name;
        check(introPlacementXDir(r.cb.f) == IntroXDir::kLeft, "C12." + name + ": +x runs to the viewer's left");
        const IntroWorldCb read = introReadWorldCb(r.cb.f);
        check(read.ok && read.xDir == IntroXDir::kLeft && read.halfWidth == 4.44444f && read.toward == 1,
              "C12." + name + " reads ok: xDir left, half-width 4.44444, toward +1");
        check(ratioOf(r.cb) >= 0.87 && ratioOf(r.cb) <= 1.0, "C12." + name + ": the ratio is in the 0.87 to 1.0 the header quotes");
        const Cb flipped = negatedX(r.cb);
        check(introPlacementXDir(flipped.f) == IntroXDir::kRight, "C12." + name + " with the x column negated: +x runs to the right");
        const IntroWorldCb back = introReadWorldCb(flipped.f);
        check(back.ok && back.xDir == IntroXDir::kRight && back.halfWidth == read.halfWidth && back.toward == read.toward,
              "C12." + name + " with the x column negated reads ok, xDir right, and nothing else changes");
        check(ratioOf(flipped) == ratioOf(r.cb), "C12." + name + ": negating the x column leaves the ratio alone");
    }
    // the movie's stock constants, either eye: cb2[1].x is -1/2712, so +x runs left (the first flight of the world-locked movie came out mirrored
    // until its x column was built so), and the reading is refused as a screen-space placement before the direction is asked
    for (int eye = 0; eye < 2; ++eye) {
        const Cb s = eye == 0 ? stockLeft() : stockRight();
        const std::string name = eye == 0 ? "the movie's stock constants, left eye" : "the movie's stock constants, right eye";
        check(introPlacementXDir(s.f) == IntroXDir::kLeft, "C12." + name + ": +x runs to the viewer's left");
        check(introPlacementXDir(negatedX(s).f) == IntroXDir::kRight, "C12." + name + " with the x column negated: +x runs to the right");
        check(ratioOf(s) == 1.0, "C12." + name + ": cb2[1].w is zero, so one term decides and the ratio is exactly 1");
        check(refused(s, kWhyScreen), "C12." + name + " are refused as screen space first, with xDir left unknown");
    }
    // the answer is the sign of cb2[1].x * cb2[4].w - cb2[4].x * cb2[1].w: the real numbers, written out
    {
        const Cb c = capture2();
        check(termA(c) < 0.0 && termB(c) > 0.0 && termA(c) - termB(c) < 0.0, "C12.capture 2: a is -2.936, b is +0.0000688, the minor is negative");
        const Cb b = capture1();
        check(termA(b) < 0.0 && termB(b) < 0.0 && termA(b) - termB(b) < 0.0, "C12.capture 1: a is -1.265, b is -0.0847, the minor is still negative");
    }
}

// ---- C13: the margin, float rounding and the degenerate inputs ----------------------------------------------------------------
void caseC13() {
    // The margin, in the four signs, at five scales: the same vector multiplied through by a power of ten reads the same (the margin is
    // relative). a = cb2[1].x * cb2[4].w = +-2 s^2 and b = cb2[4].x * cb2[1].w a little over or under 0.05 of the two magnitudes from it.
    struct Pat {
        const char* name;
        float f4;
        bool bSmaller;
        float bSign;
        IntroXDir known;
    };
    const Pat pats[] = {{"a and b positive, b the smaller", 1.0f, true, 1.0f, IntroXDir::kRight},
                        {"a and b positive, b the larger", 1.0f, false, 1.0f, IntroXDir::kLeft},
                        {"a and b negative, |b| the smaller", -1.0f, true, -1.0f, IntroXDir::kLeft},
                        {"a and b negative, |b| the larger", -1.0f, false, -1.0f, IntroXDir::kRight}};
    const float scales[] = {1.0f, 1e-30f, 1e-15f, 1e15f, 1e30f};
    for (const Pat& p : pats) {
        for (int side = 0; side < 2; ++side) {   // 0: just above the margin, known; 1: just below it, unknown
            for (float s : scales) {
                const double r = side == 0 ? 0.0501 : 0.0499;
                const double bMag = p.bSmaller ? 2.0 * (1.0 - r) / (1.0 + r) : 2.0 * (1.0 + r) / (1.0 - r);
                const Cb c = withTerms(p.f4 * s, 2.0f * s, static_cast<float>(p.bSign * bMag * s), s);
                char label[200];
                std::snprintf(label, sizeof(label), "C13.%s, scale %g, %s the margin", p.name, static_cast<double>(s), side == 0 ? "just above" : "just below");
                check(std::fabs(ratioOf(c) - r) < 2e-4 && (side == 0 ? ratioOf(c) > kIntroXDirMargin : ratioOf(c) < kIntroXDirMargin),
                      std::string(label) + ": the rig's own ratio is on the intended side");
                check(introPlacementXDir(c.f) == (side == 0 ? p.known : IntroXDir::kUnknown), label);
                if (s == 1.0f) {   // and through the whole reader
                    if (side == 0) {
                        const IntroWorldCb w = introReadWorldCb(c.f);
                        check(w.ok && w.xDir == p.known, std::string(label) + ": reads ok through introReadWorldCb with that direction");
                    } else {
                        check(refused(c, kWhyDir), std::string(label) + ": refused through introReadWorldCb with the direction reason");
                    }
                }
            }
        }
    }
    // The margin is inclusive: a = 21, b = 19 has a minor of 2 and a scale of 40, and 0.05 * 40 is exactly 2 in double.
    check(kIntroXDirMargin * 40.0 == 2.0 && ratioOf(withTerms(21.0f, 1.0f, 19.0f, 1.0f)) == kIntroXDirMargin, "C13.the edge vector sits exactly on the margin (2 of 40)");
    check(introPlacementXDir(withTerms(21.0f, 1.0f, 19.0f, 1.0f).f) == IntroXDir::kRight, "C13.exactly on the margin the direction is known (the edge is inclusive)");
    check(introPlacementXDir(withTerms(-21.0f, 1.0f, -19.0f, 1.0f).f) == IntroXDir::kLeft, "C13.and mirrored (a -21, b -19), known the other way");
    // One term far under the other, in either order and either sign: the ratio is near 1, always known, and the larger term decides.
    check(introPlacementXDir(withTerms(1.0f, 2.0f, -0.001f, 1.0f).f) == IntroXDir::kRight, "C13.a +2, b -0.001: opposite signs, known, right");
    check(introPlacementXDir(withTerms(-1.0f, 2.0f, 0.001f, 1.0f).f) == IntroXDir::kLeft, "C13.a -2, b +0.001: opposite signs, known, left");
    check(introPlacementXDir(withTerms(1.0f, 2.0f, 0.0f, 1.0f).f) == IntroXDir::kRight, "C13.b zero, a +2: the other term decides, right");
    check(introPlacementXDir(withTerms(0.0f, 2.0f, 5.0f, 1.0f).f) == IntroXDir::kLeft, "C13.a zero, b +5: the minor is -5, left");
    check(introPlacementXDir(withTerms(0.0f, 2.0f, -5.0f, 1.0f).f) == IntroXDir::kRight, "C13.a zero, b -5: the minor is +5, right");
    // Float rounding. These two sit within 4e-9 of the margin (found by a search against a copy of the rule that does the arithmetic in float):
    // double arithmetic reads them right, float arithmetic reads the first as unknown and the second as known.
    {
        const Cb above = withTerms(0.556243479f, 1.15046847f, 0.578993857f, 1.0f);   // ratio 0.0500000040
        const Cb below = withTerms(0.68570292f, 0.834858418f, 0.517944396f, 1.0f);   // ratio 0.0499999969
        check(ratioOf(above) > kIntroXDirMargin && ratioOf(above) < kIntroXDirMargin * (1.0 + 1e-6), "C13.the first float-rounding vector is just above the margin");
        check(ratioOf(below) < kIntroXDirMargin && ratioOf(below) > kIntroXDirMargin * (1.0 - 1e-6), "C13.the second float-rounding vector is just below the margin");
        check(introPlacementXDir(above.f) == IntroXDir::kRight, "C13.a ratio 4e-9 above the margin is known (float arithmetic loses it)");
        check(introPlacementXDir(below.f) == IntroXDir::kUnknown, "C13.a ratio 3e-9 below the margin is unknown (float arithmetic finds it known)");
    }
    // Nothing to read: every term zero, by any two floats that make both products zero, on either capture.
    check(introPlacementXDir(make({}).f) == IntroXDir::kUnknown, "C13.all zeros: no term at all, unknown");
    const int aSlots[2] = {4, 19};
    const int bSlots[2] = {16, 7};
    for (int base = 0; base < 2; ++base) {
        for (int sa : aSlots) {
            for (int sb : bSlots) {
                Cb c = base == 0 ? capture2() : capture1();
                c.f[sa] = 0.0f;
                c.f[sb] = 0.0f;
                char label[160];
                std::snprintf(label, sizeof(label), "C13.capture %d with f[%d] and f[%d] zero: both terms vanish, unknown", base == 0 ? 2 : 1, sa, sb);
                check(introPlacementXDir(c.f) == IntroXDir::kUnknown, label);
            }
        }
    }
    // One term zero is no reason to refuse: the other decides (the movie's stock constants have cb2[1].w = 0).
    {
        Cb c = capture2();
        c.f[7] = 0.0f;   // b = 0: a alone, -2.936
        check(introPlacementXDir(c.f) == IntroXDir::kLeft, "C13.capture 2 with cb2[1].w zero: a alone decides, left");
        c = capture2();
        c.f[4] = 0.0f;   // a = 0: the minor is -b, b being +0.0000688
        check(introPlacementXDir(c.f) == IntroXDir::kLeft, "C13.capture 2 with cb2[1].x zero: -b decides, left");
    }
    // Not a number, or infinite, in any of the four floats it reads: unknown, on either capture.
    const int read[4] = {4, 7, 16, 19};
    const float bad[3] = {kNan, kInf, -kInf};
    const char* const badName[3] = {"NaN", "+infinity", "-infinity"};
    for (int base = 0; base < 2; ++base) {
        for (int slot : read) {
            for (int k = 0; k < 3; ++k) {
                Cb c = base == 0 ? capture2() : capture1();
                c.f[slot] = bad[k];
                char label[160];
                std::snprintf(label, sizeof(label), "C13.%s in f[%d] of capture %d: unknown", badName[k], slot, base == 0 ? 2 : 1);
                check(introPlacementXDir(c.f) == IntroXDir::kUnknown, label);
            }
        }
    }
    // Only those four floats are read: the other sixteen can hold anything at all, and the answer is the same.
    const float poison[4] = {kNan, kInf, -kInf, 1e30f};
    for (int base = 0; base < 2; ++base) {
        for (int slot = 0; slot < 20; ++slot) {
            if (slot == 4 || slot == 7 || slot == 16 || slot == 19) continue;
            for (float v : poison) {
                Cb c = base == 0 ? capture2() : capture1();
                c.f[slot] = v;
                char label[160];
                std::snprintf(label, sizeof(label), "C13.f[%d] = %g does not change what capture %d reads", slot, static_cast<double>(v), base == 0 ? 2 : 1);
                check(introPlacementXDir(c.f) == IntroXDir::kLeft, label);
            }
        }
    }
}

// ---- C14: a direction that cannot be told ------------------------------------------------------------------------------------
void caseC14() {
    const Cb edge = edgeOn();
    check(ratioOf(edge) < 1e-6, "C14.the edge-on vector's two terms cancel to within a millionth");
    check(introPlacementXDir(edge.f) == IntroXDir::kUnknown, "C14.edge-on: the direction cannot be told");
    const IntroWorldCb r = introReadWorldCb(edge.f);
    check(!r.ok, "C14.edge-on is not an ok reading");
    check(r.why != nullptr && std::strcmp(r.why, kWhyDir) == 0, "C14.and the reason is the new one, in its words");
    check(r.halfWidth == 0.0f && r.toward == 0 && r.xDir == IntroXDir::kUnknown, "C14.nothing is left behind: half-width 0, depth direction 0, xDir unknown");
    // Every rule before the gate passes it: move one term and the same vector reads ok.
    {
        Cb c = edge;
        c.f[16] = 0.0f;   // b = 0: a alone decides
        const IntroWorldCb ok = introReadWorldCb(c.f);
        check(ok.ok && ok.xDir == IntroXDir::kLeft && ok.toward == 1 && ok.halfWidth == 4.44444f, "C14.the same vector with cb2[4].x zeroed reads ok, xDir left");
    }
    // Every kind of refusal leaves xDir unknown (and every ok reading, in C12, says left or right).
    struct Row {
        const char* name;
        Cb cb;
    };
    std::vector<Row> rows;
    {
        Cb c = capture2();
        c.f[2] = kNan;
        rows.push_back({"not finite", c});
    }
    rows.push_back({"screen space", stockLeft()});
    rows.push_back({"cb2[3] of the wrong length", withCol3(0.1f, 0.0f, 0.0f, -0.4f)});
    rows.push_back({"cb2[3].w near zero", withW(0.0005f, 3.76108f)});
    rows.push_back({"cb2[4].w near zero", withW(-0.5f, 0.0005f)});
    {
        Cb c = capture2();
        c.f[0] = 0.4f;
        rows.push_back({"half-width out of range", c});
        c = capture2();
        c.f[1] = 60.0f;
        rows.push_back({"half-height out of range", c});
    }
    rows.push_back({"the direction cannot be told", edge});
    for (const Row& row : rows) {
        const IntroWorldCb x = introReadWorldCb(row.cb.f);
        check(!x.ok && x.xDir == IntroXDir::kUnknown, std::string("C14.a refusal (") + row.name + ") leaves xDir unknown");
    }
    check(rows.size() == 8, "C14.one row for each of the eight rules");
}

// ---- C15: the answer against geometry computed independently ---------------------------------------------------------------------
struct V3 {
    double x, y, z;
};
struct M3 {
    double m[3][3];
};
M3 matMul(const M3& a, const M3& b) {
    M3 r = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
V3 matVec(const M3& a, const V3& v) {
    return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z, a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z};
}
M3 transposed(const M3& a) {
    M3 r = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
    return r;
}
constexpr double kPi = 3.14159265358979323846;
M3 rotAboutY(double deg) {
    const double c = std::cos(deg * kPi / 180.0), s = std::sin(deg * kPi / 180.0);
    return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
}
M3 rotAboutX(double deg) {
    const double c = std::cos(deg * kPi / 180.0), s = std::sin(deg * kPi / 180.0);
    return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
}
M3 rotAboutZ(double deg) {
    const double c = std::cos(deg * kPi / 180.0), s = std::sin(deg * kPi / 180.0);
    return {{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}};
}
// An asymmetric frustum: the m02 and m12 terms (the frustum's centre is off the axis) are what a careless rule trips on, so they are not zero. The
// depth row is a reverse-Z one with an infinite far plane, as the captures' z terms (about 1e-4 in every column) say: m22 is tiny, so a rule that
// reads a z term where it means a w term is not forgiven.
constexpr double kM00 = 0.7807, kM02 = 0.05, kM11 = 0.788, kM12 = -0.02, kM22 = 0.0001, kM23 = 0.1;
// The projection acting on a direction in view space, and on a point (whose depth has the near-plane offset): cb2[1..3] and cb2[4].
void putColumn(Cb& c, int at, const V3& d) {
    c.f[at + 0] = static_cast<float>(kM00 * d.x + kM02 * d.z);
    c.f[at + 1] = static_cast<float>(kM11 * d.y + kM12 * d.z);
    c.f[at + 2] = static_cast<float>(kM22 * d.z);
    c.f[at + 3] = static_cast<float>(-d.z);
}
void putPoint(Cb& c, int at, const V3& p) {
    c.f[at + 0] = static_cast<float>(kM00 * p.x + kM02 * p.z);
    c.f[at + 1] = static_cast<float>(kM11 * p.y + kM12 * p.z);
    c.f[at + 2] = static_cast<float>(kM22 * p.z + kM23);
    c.f[at + 3] = static_cast<float>(-p.z);
}
struct Pose {
    Cb cb;
    IntroXDir truth;   // which way a step along the panel's +x moves its centre on the screen, from the vectors in double, before any rounding
};
// One pose, the 20 floats the way the shader's reader sees them: view = R^T * world (R the head's rotation: yaw about up, then pitch, then roll),
// cb2[1..3] = the projection acting on the panel's axes in view space, cb2[4] = the projection acting on its centre. The centre is 3.76108 m in
// front of the head at yaw 0 (capture 2's distance); the eyes sit 31.5 mm either side of the head's middle. The intro composite's axes are
// (-X, Y, +Z toward the viewer), the on-foot screen's (+X, Y, -Z away from the viewer); panelYaw turns the panel about the world's up.
Pose makePose(double yaw, double pitch, double roll, double panelYaw, bool introFrame, int eye) {
    const M3 head = matMul(matMul(rotAboutY(yaw), rotAboutX(pitch)), rotAboutZ(roll));   // view -> world
    const M3 toView = transposed(head);
    const M3 turn = rotAboutY(panelYaw);
    const V3 axisX = matVec(toView, matVec(turn, introFrame ? V3{-1, 0, 0} : V3{1, 0, 0}));
    const V3 axisY = matVec(toView, matVec(turn, V3{0, 1, 0}));
    const V3 axisZ = matVec(toView, matVec(turn, introFrame ? V3{0, 0, 1} : V3{0, 0, -1}));
    V3 centre = matVec(toView, V3{0, 0, -3.76108});
    centre.x -= eye * 0.0315;
    Pose p = {};
    p.cb.f[0] = 4.44444f;
    p.cb.f[1] = 2.5f;
    putColumn(p.cb, 4, axisX);
    putColumn(p.cb, 8, axisY);
    putColumn(p.cb, 12, axisZ);
    putPoint(p.cb, 16, centre);
    // d(x_clip / w_clip)/d(step along +x) at the centre has the sign of x_clip' w - x_clip w' (w^2 is positive), in double, from the unrounded vectors
    const double minor = (kM00 * axisX.x + kM02 * axisX.z) * (-centre.z) - (kM00 * centre.x + kM02 * centre.z) * (-axisX.z);
    p.truth = minor < 0.0 ? IntroXDir::kLeft : IntroXDir::kRight;
    return p;
}
void caseC15() {
    // (1) Every yaw (+-180, step 15), pitch (+-60, step 20) and roll (+-60, step 30), both eyes, both frames: 3500 poses. The intro frame is always
    // left and the on-foot frame always right, whatever the head does, a panel behind the viewer included; the whole reader says the same, and
    // the depth direction is the frame's (+1 toward the viewer for the intro composite, -1 for the on-foot screen's).
    unsigned total = 0, unknown = 0;
    for (int frame = 0; frame < 2; ++frame) {
        const bool intro = frame == 0;
        const IntroXDir frameDir = intro ? IntroXDir::kLeft : IntroXDir::kRight;
        for (int eye = -1; eye <= 1; eye += 2) {
            for (int yaw = -180; yaw <= 180; yaw += 15) {
                for (int pitch = -60; pitch <= 60; pitch += 20) {
                    for (int roll = -60; roll <= 60; roll += 30) {
                        const Pose p = makePose(yaw, pitch, roll, 0.0, intro, eye);
                        char label[220];
                        std::snprintf(label, sizeof(label), "C15.%s frame, eye %d, yaw %d, pitch %d, roll %d (ratio %.4f)", intro ? "intro" : "on-foot", eye, yaw, pitch, roll,
                                      ratioOf(p.cb));
                        ++total;
                        check(p.truth == frameDir, std::string(label) + ": the rig's own geometry says the frame's direction");
                        const IntroXDir got = introPlacementXDir(p.cb.f);
                        if (got == IntroXDir::kUnknown) {
                            ++unknown;
                            check(ratioOf(p.cb) < kIntroXDirMargin, std::string(label) + ": answers unknown with a ratio at or above the margin");
                        } else {
                            check(got == frameDir, std::string(label) + ": the wrong direction");
                        }
                        const IntroWorldCb r = introReadWorldCb(p.cb.f);
                        if (r.ok) check(r.xDir == frameDir && r.toward == (intro ? 1 : -1), std::string(label) + ": the whole reader's answer");
                    }
                }
            }
        }
    }
    check(total == 3500, "C15.the grid is 3500 poses");
    check(unknown * 100 <= total, "C15.at most one pose in a hundred answers unknown");

    // (2) A panel turned about the world's up toward edge-on (19 angles, out to 89.9 degrees either side), with nine yaws, three pitches and three
    // rolls of the head, both eyes, both frames: 6156 poses. The truth comes from the vectors (the eye's offset moves the exact edge-on angle by half
    // a degree, so the frame no longer says it): a known answer is always the true one, an unknown answer always has a ratio under the margin, and a
    // ratio at or above the margin is never unknown.
    const double panelYaws[] = {-89.9, -89.7, -89.5, -89.0, -88.0, -85.0, -80.0, -60.0, -30.0, 0.0, 30.0, 60.0, 80.0, 85.0, 88.0, 89.0, 89.5, 89.7, 89.9};
    const int yaws[] = {-180, -135, -90, -45, 0, 45, 90, 135, 180};
    const int tilts[] = {-60, 0, 60};
    unsigned sweepTotal = 0, sweepUnknown = 0, sweepKnown = 0;
    for (int frame = 0; frame < 2; ++frame) {
        const bool intro = frame == 0;
        for (int eye = -1; eye <= 1; eye += 2) {
            for (int yaw : yaws) {
                for (int pitch : tilts) {
                    for (int roll : tilts) {
                        for (double py : panelYaws) {
                            const Pose p = makePose(yaw, pitch, roll, py, intro, eye);
                            char label[240];
                            std::snprintf(label, sizeof(label), "C15.edge-on sweep, %s frame, eye %d, yaw %d, pitch %d, roll %d, panel turned %.1f (ratio %.4f)",
                                          intro ? "intro" : "on-foot", eye, yaw, pitch, roll, py, ratioOf(p.cb));
                            ++sweepTotal;
                            const IntroXDir got = introPlacementXDir(p.cb.f);
                            if (got == IntroXDir::kUnknown) {
                                ++sweepUnknown;
                                check(ratioOf(p.cb) < kIntroXDirMargin, std::string(label) + ": unknown with a ratio at or above the margin");
                            } else {
                                ++sweepKnown;
                                check(ratioOf(p.cb) >= kIntroXDirMargin, std::string(label) + ": known with a ratio under the margin");
                                check(got == p.truth, std::string(label) + ": the wrong direction");
                            }
                        }
                    }
                }
            }
        }
    }
    check(sweepTotal == 6156, "C15.the edge-on sweep is 6156 poses");
    check(sweepKnown == 4014 && sweepUnknown == 2142, "C15.the sweep splits 4014 known to 2142 unknown at the margin of 0.05");
}

// ---- the runner ------------------------------------------------------------------------------------------------------------
struct Case {
    const char* id;
    void (*run)();
};
const Case kCases[] = {{"C1", caseC1}, {"C2", caseC2}, {"C3", caseC3},   {"C4", caseC4},   {"C5", caseC5},   {"C6", caseC6},   {"C7", caseC7},
                       {"C8", caseC8}, {"C9", caseC9}, {"C10", caseC10}, {"C11", caseC11}, {"C12", caseC12}, {"C13", caseC13}, {"C14", caseC14},
                       {"C15", caseC15}};

bool selected(const std::string& only, const char* id) {
    if (only.empty()) return true;
    return ("," + only + ",").find(std::string(",") + id + ",") != std::string::npos;
}

int run(const std::string& only) {
    for (const char* id = only.c_str(); *id;) {
        const char* comma = std::strchr(id, ',');
        const std::string one = comma ? std::string(id, comma) : std::string(id);
        bool known = false;
        for (const Case& c : kCases) known = known || one == c.id;
        if (!known) {
            std::fprintf(stderr, "FAIL: --only names a case that does not exist: %s\n", one.c_str());
            return 1;
        }
        id = comma ? comma + 1 : id + one.size();
    }
    unsigned ran = 0;
    try {
        for (const Case& c : kCases) {
            if (!selected(only, c.id)) continue;
            c.run();
            ++ran;
        }
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
    std::printf("PASS: %u intro curve math checks (%u cases)\n", g_checks, ran);
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    std::string only;
    bool self = false, dry = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test")) self = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else {
            std::fputs("usage: intro_curve_math_test --self-test [--only C1,C4,...] | --dry-run\n", stderr);
            return 2;
        }
    }
    if (dry) {
        std::puts("intro curve math test: dry run (no checks run)");
        return 0;
    }
    if (!self) {
        std::fputs("usage: intro_curve_math_test --self-test [--only C1,C4,...] | --dry-run\n", stderr);
        return 2;
    }
    return run(only);
}
