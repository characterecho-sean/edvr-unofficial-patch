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

// The reasons, as the header words them (the later log line prints one of these).
const char* const kWhyFinite = "a constant is not a finite number";
const char* const kWhyScreen = "the constants read as a screen-space placement (cb2[3] is zero and w is a constant 1), not a world-space panel";
const char* const kWhyLength = "cb2[3] is not a unit-scale column (its length is outside 0.5 to 2)";
const char* const kWhyW3 = "cb2[3].w is too close to zero to tell which way the panel's depth runs";
const char* const kWhyW4 = "cb2[4].w is too close to zero to tell which way the panel's depth runs";
const char* const kWhyX = "cb2[0].x is not a plausible half-width in metres (outside 0.5 to 50)";
const char* const kWhyY = "cb2[0].y is not a plausible half-height in metres (outside 0.5 to 50)";

// Refused for exactly this reason, and nothing left behind (half-width 0, direction 0).
bool refused(const Cb& c, const char* why) {
    const IntroWorldCb r = introReadWorldCb(c.f);
    return !r.ok && r.halfWidth == 0.0f && r.toward == 0 && r.why != nullptr && std::strcmp(r.why, why) == 0;
}
bool readsOk(const Cb& c) { return introReadWorldCb(c.f).ok; }

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
    // the seven reasons are seven different texts
    const char* all[] = {kWhyFinite, kWhyScreen, kWhyLength, kWhyW3, kWhyW4, kWhyX, kWhyY};
    bool distinct = true;
    for (size_t i = 0; i < 7; ++i)
        for (size_t j = i + 1; j < 7; ++j) distinct = distinct && std::strcmp(all[i], all[j]) != 0;
    check(distinct, "C3.the seven reasons are seven different texts");
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

// ---- the runner ------------------------------------------------------------------------------------------------------------
struct Case {
    const char* id;
    void (*run)();
};
const Case kCases[] = {{"C1", caseC1}, {"C2", caseC2}, {"C3", caseC3},   {"C4", caseC4},   {"C5", caseC5},   {"C6", caseC6},
                       {"C7", caseC7}, {"C8", caseC8}, {"C9", caseC9}, {"C10", caseC10}, {"C11", caseC11}};

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
