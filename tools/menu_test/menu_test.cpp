// menu_test -- the settings menu's pure parts, asserted without a headset.
//
// WHY. docs/settings-menu.md builds on four pieces of arithmetic and policy
// that a wrong sign or an off-by-one would otherwise reveal only in a
// headset: the keyboard gate's filter (release everything, admit nothing,
// and the summon swallow), the scan-code map the swallow depends on, the
// panel's ray intersection (flat and on the cylinder, the same math the
// shader transcribes), the door's eye transform, and the one-value ini
// write's merge. Each is a table here.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/common/frame_flag.h"
#include "../../src/common/hotkey.h"
#include "../../src/common/iniedit.h"
#include "../../src/common/perf_math.h"
#include "../../src/d3d11/input_gate.h"
#include "../../src/d3d11/menu_panel.h"
#include "../../src/d3d11/perf_monitor.h"
#include "../../src/openvr/menu_door.h"
#include "../../src/openvr/system_hook.h"

using namespace edvr;

namespace edvr {
// menu_door.cpp asks the system hook for the eye offset; this fixture has no
// runtime, so it answers "unknown" and the door's fallback is what is tested.
bool systemHookEyeToHead(vr::EVREye, float[12]) { return false; }
// The panel and the shader compile note events for the monitor's drop
// attribution; this fixture has no monitor, so the note goes nowhere.
void perfMonitorNoteEvent(uint32_t, double) {}
}  // namespace edvr

namespace {

int g_fails = 0;

void ok(const char* what) { printf("  ok    %s\n", what); }
void fail(const char* what, const char* detail) {
    printf("  FAIL  %s -- %s\n", what, detail);
    ++g_fails;
}
void check(bool cond, const char* what, const char* detail = "") {
    if (cond) ok(what);
    else fail(what, detail);
}
bool approx(float a, float b, float eps = 1e-3f) { return fabsf(a - b) <= eps; }

// --- the gate's filter policies ----------------------------------------------

void testFilterState() {
    uint8_t st[256];
    memset(st, 0, sizeof(st));
    st[0x11] = 0x80;   // W held
    st[0x42] = 0x80;   // F8 held
    st[0x1D] = 0x80;   // left control held
    inputGateFilterState(st, /*priv=*/true, 0x42, 0);
    bool allUp = true;
    for (int i = 0; i < 256; ++i) allUp = allUp && st[i] == 0;
    check(allUp, "private zeroes every key, held ones included");

    memset(st, 0, sizeof(st));
    st[0x11] = 0x80;
    st[0x42] = 0x80;
    inputGateFilterState(st, false, 0x42, 0);   // a bare F8 binding
    check(st[0x42] == 0 && st[0x11] == 0x80,
          "shared: the summon key is swallowed and W passes");

    memset(st, 0, sizeof(st));
    st[0x42] = 0x80;
    inputGateFilterState(st, false, 0x42, kHotkeyCtrl);   // CTRL+F8, control not held
    check(st[0x42] == 0x80, "shared: a chord's key passes while its modifier is up");
    st[0x9D] = 0x80;   // right control
    inputGateFilterState(st, false, 0x42, kHotkeyCtrl);
    check(st[0x42] == 0 && st[0x9D] == 0x80,
          "shared: the chord's key is swallowed once either control is down, the modifier passes");

    memset(st, 0, sizeof(st));
    st[0x42] = 0x80;
    inputGateFilterState(st, false, 0, 0);
    check(st[0x42] == 0x80, "no summon key known: nothing swallowed");
}

void testFilterData() {
    DiObjectData d[6] = {};
    // W down, F8 down, W up, A down, F8 up, A up
    d[0] = {0x11, 0x80, 1, 1, 0};
    d[1] = {0x42, 0x80, 2, 2, 0};
    d[2] = {0x11, 0x00, 3, 3, 0};
    d[3] = {0x1E, 0x80, 4, 4, 0};
    d[4] = {0x42, 0x00, 5, 5, 0};
    d[5] = {0x1E, 0x00, 6, 6, 0};
    DiObjectData a[6];
    memcpy(a, d, sizeof(d));
    uint32_t kept = inputGateFilterData(a, 6, /*priv=*/true, 0, false);
    check(kept == 3 && a[0].dwOfs == 0x11 && a[0].dwData == 0 && a[1].dwOfs == 0x42 &&
              a[2].dwOfs == 0x1E && a[2].dwData == 0,
          "private keeps the ups in order and drops every down");

    memcpy(a, d, sizeof(d));
    kept = inputGateFilterData(a, 6, false, 0x42, /*modsHeld=*/true);
    check(kept == 4 && a[0].dwOfs == 0x11 && a[1].dwOfs == 0x11 && a[2].dwOfs == 0x1E &&
              a[3].dwOfs == 0x1E,
          "shared with the chord held: the summon key's down AND up vanish, the rest stay");

    memcpy(a, d, sizeof(d));
    kept = inputGateFilterData(a, 6, false, 0x42, false);
    check(kept == 6, "shared with the chord up: everything passes");

    kept = inputGateFilterData(a, 0, true, 0x42, true);
    check(kept == 0, "an empty buffer stays empty");
}

void testDikMap() {
    check(inputGateDikOf(VK_F8) == 0x42, "F8 maps to DIK_F8");
    check(inputGateDikOf(VK_INSERT) == 0xD2, "Insert maps to DIK_INSERT (extended)");
    check(inputGateDikOf(VK_UP) == 0xC8, "Up maps to DIK_UP (extended)");
    check(inputGateDikOf(VK_PAUSE) == 0xC5, "Pause maps to DIK_PAUSE, not NumLock's code");
    check(inputGateDikOf(VK_NUMLOCK) == 0x45, "NumLock maps to DIK_NUMLOCK, which is not extended");
    check(inputGateDikOf('A') == 0x1E, "A maps to DIK_A");
    check(inputGateDikOf(0) == 0 && inputGateDikOf(300) == 0, "out-of-range keys map to nothing");
}

// --- the panel's ray intersection --------------------------------------------

void testHitFlat() {
    const float org[3] = {0.0f, 0.0f, 0.0f};
    float su = 0, sv = 0;
    // Straight ahead at a panel 1.4 m out, 0.3 m half-width, 0.2 half-height.
    const float ahead[3] = {0.0f, 0.0f, -1.0f};
    check(menuPanelHit(org, ahead, 1.4f, 0.0f, 0.3f, 0.2f, 0.0f, &su, &sv) && approx(su, 0.5f) &&
              approx(sv, 0.5f),
          "flat: straight ahead lands on the centre");
    // Looking right by the panel's half-width at its distance: the right edge.
    const float right[3] = {0.3f / 1.4f, 0.0f, -1.0f};
    check(menuPanelHit(org, right, 1.4f, 0.0f, 0.3f, 0.2f, 0.0f, &su, &sv) && approx(su, 1.0f) &&
              approx(sv, 0.5f),
          "flat: the right edge is su = 1");
    // Looking up by the half-height: the top, sv = 1 (bottom-up).
    const float up[3] = {0.0f, 0.2f / 1.4f, -1.0f};
    check(menuPanelHit(org, up, 1.4f, 0.0f, 0.3f, 0.2f, 0.0f, &su, &sv) && approx(sv, 1.0f),
          "flat: the top edge is sv = 1, so the bitmap's row 0 is the top");
    const float away[3] = {0.0f, 0.0f, 1.0f};
    check(!menuPanelHit(org, away, 1.4f, 0.0f, 0.3f, 0.2f, 0.0f, &su, &sv),
          "flat: looking away misses");
    const float wide[3] = {0.4f, 0.0f, -1.0f};
    check(!menuPanelHit(org, wide, 1.4f, 0.0f, 0.3f, 0.2f, 0.0f, &su, &sv),
          "flat: past the edge misses");
    // A head moved 0.1 m right: the centre now sits left of the look.
    const float orgR[3] = {0.1f, 0.0f, 0.0f};
    check(menuPanelHit(orgR, ahead, 1.4f, 0.0f, 0.3f, 0.2f, 0.0f, &su, &sv) &&
              approx(su, (0.1f + 0.3f) / 0.6f),
          "flat: head translation shifts the hit, not the panel");
}

void testHitCurved() {
    const float org[3] = {0.0f, 0.0f, 0.0f};
    float su = 0, sv = 0;
    const float ahead[3] = {0.0f, 0.0f, -1.0f};
    check(menuPanelHit(org, ahead, 1.4f, 0.3f, 0.3f, 0.2f, 0.0f, &su, &sv) && approx(su, 0.5f) &&
              approx(sv, 0.5f),
          "curved: straight ahead lands on the centre, on the surface at dist");
    // A look 10 degrees right, worked by hand for curve 0.3 (R = 4.667 m,
    // the axis 3.267 m behind the viewer): the ray meets the cylinder at
    // t = 1.3937, x = 0.2457, and the arc from the centre is 0.2458 m --
    // su = (0.2458 + 0.3) / 0.6 = 0.9097. The flat panel puts the same look
    // at x = 0.2468, su = 0.9113: the curve brings the edges nearer, so the
    // same angle covers less of the surface.
    const float right10[3] = {tanf(10.0f * 0.0174533f), 0.0f, -1.0f};
    const bool hit = menuPanelHit(org, right10, 1.4f, 0.3f, 0.3f, 0.2f, 0.0f, &su, &sv);
    check(hit && approx(su, 0.9097f, 0.002f), "curved: a 10-degree look lands at the hand-worked su");
    float suFlat = 0;
    menuPanelHit(org, right10, 1.4f, 0.0f, 0.3f, 0.2f, 0.0f, &suFlat, &sv);
    check(approx(suFlat, 0.9113f, 0.002f) && suFlat > su,
          "curved: the same look covers less of the surface than on the flat panel");
}

// --- the head-locked overlay's angles across the channel ---------------------
//
// They are packed as tenths of a degree into twelve bits each. The bias was
// 4096 -- one bit above the field it is masked into -- so it was thrown
// away and every angle came back 409.6 degrees low: a readout asked for
// straight ahead appeared 49.6 degrees right and 29.6 down. Flown
// 2026-09-08 and reported as exactly that.
void testHeadLockAngles() {
    const float cases[][2] = {{0.0f, 0.0f},   {60.0f, 20.0f}, {-60.0f, -45.0f},
                              {0.0f, 40.0f},  {12.3f, -7.7f}, {180.0f, -180.0f}};
    bool allOk = true;
    for (const auto& c : cases) {
        setMenuHeadLock(true, c[0], c[1]);
        float yaw = 999.0f, pitch = 999.0f;
        if (!menuHeadLock(&yaw, &pitch) || !approx(yaw, c[0], 0.05f) ||
            !approx(pitch, c[1], 0.05f)) {
            allOk = false;
            printf("  head lock: asked %.1f/%.1f, got %.1f/%.1f\n", c[0], c[1], yaw, pitch);
        }
    }
    check(allOk, "head lock: every angle comes back the one that was asked for");
    // Zero is the case that mattered, so it gets its own line.
    setMenuHeadLock(true, 0.0f, 0.0f);
    float yaw = 999.0f, pitch = 999.0f;
    menuHeadLock(&yaw, &pitch);
    check(approx(yaw, 0.0f, 0.001f) && approx(pitch, 0.0f, 0.001f),
          "head lock: zero is straight ahead, not 49.6 right and 29.6 down");
    setMenuHeadLock(false, 0.0f, 0.0f);
    check(!menuHeadLock(&yaw, &pitch), "head lock: off is off");
}

// --- which way the head-locked readout's offsets point ------------------------
//
// The panel sits on its anchor's forward. With an identity head pose the
// anchor IS the rotation, so the panel's centre in head space is minus its
// third column times the distance -- and its x tells you which way the
// offset moved it. Positive yaw must go RIGHT (+x) and positive pitch UP,
// which is what edvr.ini promises; the code had yaw the other way.
void testHeadLockDirection() {
    const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    auto centreOf = [&](float yaw, float pitch, float out[3]) {
        float a[12];
        menuHeadLockAnchor(identity, yaw, pitch, a);
        // The forward is minus the third column; the centre is dist along it.
        out[0] = -a[2];
        out[1] = -a[6];
        out[2] = -a[10];
    };
    float c[3];
    centreOf(0.0f, 0.0f, c);
    check(approx(c[0], 0.0f, 0.001f) && approx(c[1], 0.0f, 0.001f) && approx(c[2], -1.0f, 0.001f),
          "head lock: no offset is straight ahead");
    centreOf(30.0f, 0.0f, c);
    check(c[0] > 0.4f && approx(c[1], 0.0f, 0.001f),
          "head lock: a POSITIVE yaw puts the readout to the RIGHT");
    centreOf(-30.0f, 0.0f, c);
    check(c[0] < -0.4f, "head lock: a negative yaw puts it to the left");
    centreOf(0.0f, 30.0f, c);
    check(c[1] > 0.4f && approx(c[0], 0.0f, 0.001f),
          "head lock: a positive pitch puts it up");
    centreOf(0.0f, -30.0f, c);
    check(c[1] < -0.4f, "head lock: a negative pitch puts it down");
}

// --- the tooltip strip's surface shift ---------------------------------------
//
// The bitmap is wider than the menu card so the tooltip has somewhere to
// sit; the shift slides the bitmap along its own surface so the CARD, not
// the bitmap, is centred on where the user is looking. These check the one
// property the whole arrangement exists for: a look straight ahead must
// land on the card's middle, on a flat panel and on a curved one alike,
// and the card must still be exactly as wide as it was without the strip.
void testHitShift() {
    const float org[3] = {0.0f, 0.0f, 0.0f};
    const float ahead[3] = {0.0f, 0.0f, -1.0f};
    float su = 0, sv = 0;
    // A card of half-width 0.3 with the strip beside it: the bitmap is
    // kTipRatio times as wide, and the shift is what the model computes.
    const float cardHalf = 0.3f;
    const float halfW = cardHalf * kTipRatio;
    const float shift = halfW * (kTipRatio - 1.0f) / kTipRatio;
    const float cardMid = 1.0f / (2.0f * kTipRatio);   // the card's middle in u

    check(menuPanelHit(org, ahead, 1.4f, 0.0f, halfW, 0.2f, shift, &su, &sv) &&
              approx(su, cardMid, 0.0005f),
          "shift: straight ahead lands on the CARD's middle, not the bitmap's");
    // The card's own edges, at exactly the angles they had before the
    // strip. The left one is su = 0, which sits on the boundary the hit
    // test rejects by a rounding bit, so the coordinate is read directly.
    const float leftEdge[3] = {-cardHalf / 1.4f, 0.0f, -1.0f};
    su = 9.0f;
    menuPanelHit(org, leftEdge, 1.4f, 0.0f, halfW, 0.2f, shift, &su, &sv);
    check(approx(su, 0.0f, 0.0005f), "shift: the card's left edge is still at its own half-width");
    const float rightEdge[3] = {cardHalf / 1.4f, 0.0f, -1.0f};
    check(menuPanelHit(org, rightEdge, 1.4f, 0.0f, halfW, 0.2f, shift, &su, &sv) &&
              approx(su, 1.0f / kTipRatio, 0.0005f),
          "shift: the card's right edge is where the strip begins");
    // Curved: the card's middle is on the cylinder's apex, so the same look
    // lands in the same place whatever the curve.
    check(menuPanelHit(org, ahead, 1.4f, 0.3f, halfW, 0.2f, shift, &su, &sv) &&
              approx(su, cardMid, 0.0005f),
          "shift: a curved panel puts the card's middle on the apex too");
    // And the strip really is to the RIGHT: a look past the card's right
    // edge is still on the bitmap.
    const float intoStrip[3] = {1.5f * cardHalf / 1.4f, 0.0f, -1.0f};
    check(menuPanelHit(org, intoStrip, 1.4f, 0.0f, halfW, 0.2f, shift, &su, &sv) &&
              su > 1.0f / kTipRatio,
          "shift: past the card's right edge is the tooltip's strip");
}

// --- the door's eye transform -----------------------------------------------

void testXform() {
    // Anchor: identity at the origin. Current: identity, head moved 0.1 m
    // right; eye offset 0.03 m left. Origin in anchor space: (0.07, 0, 0).
    const float I[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const float moved[12] = {1, 0, 0, 0.1f, 0, 1, 0, 0, 0, 0, 1, 0};
    const float eye[3] = {-0.03f, 0.0f, 0.0f};
    float xf[12];
    menuDoorXform(I, moved, eye, xf);
    check(approx(xf[0], 1) && approx(xf[4], 1) && approx(xf[8], 1) && approx(xf[1], 0) && approx(xf[3], 0),
          "xform: same orientation gives the identity rotation");
    check(approx(xf[9], 0.07f) && approx(xf[10], 0) && approx(xf[11], 0),
          "xform: the eye's origin is the head's move plus the eye offset");

    // Anchor yawed 90 degrees left (facing -X): its forward -Z maps to world -X.
    // Rotation about +Y by +90: x' = z, z' = -x  =>  rows [0 0 1; 0 1 0; -1 0 0].
    const float yawed[12] = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0};
    const float zero[3] = {0, 0, 0};
    menuDoorXform(yawed, I, zero, xf);
    // D = Ra^T * Rc = Ra^T. A current-head forward (0,0,-1) in anchor space:
    // Ra^T * (0,0,-1) = (-(Ra[2][0]), -(Ra[2][1]), -(Ra[2][2])) = (1, 0, 0):
    // the anchor sees the current head looking to ITS right. The panel sits
    // on the anchor's -Z, so a head facing world -Z looks 90 degrees off it.
    const float fx = -xf[2], fy = -xf[5], fz = -xf[8];
    check(approx(fx, 1) && approx(fy, 0) && approx(fz, 0),
          "xform: a head facing world -Z looks along the +X of an anchor that faces -X");
}

// --- the one-value ini write -------------------------------------------------

void testIniWrite() {
    const std::string src =
        "# EDVR settings.\r\n"
        "[fix]\r\n"
        "# Sharpen every outgoing frame. Live.\r\n"
        "# ui: Sharpening | range 0..1 | percent | menu performance\r\n"
        "render_sharpness = 0.0\r\n"
        "\r\n"
        "# A commented expert default.\r\n"
        "#foveation_distance = 0\r\n"
        "[menu]\r\n"
        "developer = off ; inline\r\n";
    MergeReport r;
    std::string out = mergeIni(src, src, &src, {{"fix.render_sharpness", "0.3"}}, &r);
    check(out.find("render_sharpness = 0.3\r\n") != std::string::npos,
          "write: the value lands on the line where the key already lives");
    check(out.find("# ui: Sharpening | range 0..1 | percent | menu performance") != std::string::npos,
          "write: the annotation and the comments are untouched");
    check(out.find("#foveation_distance = 0") != std::string::npos,
          "write: an unrelated commented default stays commented");
    out = mergeIni(src, src, &src, {{"menu.developer", "on"}}, &r);
    check(out.find("developer = on ; inline") != std::string::npos,
          "write: an inline comment survives a rewrite");
    out = mergeIni(src, src, &src, {{"advanced.foveation_distance", "0.7"}}, &r);
    check(out.find("#foveation_distance = 0\r\n") != std::string::npos,
          "write: a key forced under another section leaves the [fix] line of that name alone");
    out = mergeIni(src, src, &src, {}, &r);
    check(out == src, "write: merging a document with itself is a no-op");
}

// --- the Monitor page's statistics ------------------------------------------

void testPerfStats() {
    // 100 frames at 11.1 ms with one 40 ms hitch: the mean barely moves,
    // the max is the hitch, and the 1% low IS the hitch.
    float ms[100];
    for (int i = 0; i < 100; ++i) ms[i] = 11.1f;
    ms[37] = 40.0f;
    const PerfStats s = perfStatsOf(ms, 100);
    check(s.count == 100, "stats: every sample counted");
    check(approx(s.avgMs, 11.389f, 0.01f), "stats: the mean carries the hitch at a hundredth");
    check(approx(s.maxMs, 40.0f), "stats: the max is the hitch");
    check(approx(s.p99Ms, 40.0f), "stats: the 1% low is the hitch when it is one frame in a hundred");
    check(approx(s.p50Ms, 11.1f), "stats: the median is the steady frame");
    // A stall past the cap is left out, so a loading screen does not own the mean.
    ms[37] = 900.0f;
    const PerfStats t = perfStatsOf(ms, 100);
    check(t.count == 99 && approx(t.avgMs, 11.1f) && approx(t.maxMs, 11.1f),
          "stats: a sample over the cap is dropped from every figure");
    // Zeros (frames before the clock started) are dropped too.
    ms[37] = 0.0f;
    check(perfStatsOf(ms, 100).count == 99, "stats: a zero sample is not a frame");
    check(perfStatsOf(nullptr, 5).count == 0 && perfStatsOf(ms, 0).count == 0,
          "stats: nothing in, nothing out");
    check(approx(perfFpsOf(11.1f), 90.09f, 0.01f) && perfFpsOf(0.0f) == 0.0f,
          "stats: fps of a frame time, and none of nothing");
}

}  // namespace

int main() {
    printf("menu_test: the settings menu's pure parts\n");
    testFilterState();
    testFilterData();
    testDikMap();
    testHitFlat();
    testHitCurved();
    testHitShift();
    testHeadLockAngles();
    testHeadLockDirection();
    testXform();
    testIniWrite();
    testPerfStats();
    if (g_fails) {
        printf("MENU TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("MENU TEST PASSED\n");
    return 0;
}
