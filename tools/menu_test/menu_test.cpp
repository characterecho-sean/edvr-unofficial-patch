// menu_test -- the settings menu's pure parts, asserted without a headset.
//
// WHY. docs/settings-menu.md builds on pieces of arithmetic and policy that
// a wrong sign or an off-by-one would otherwise reveal only in a headset:
// the keyboard gate's filter (release everything, admit nothing, and the
// summon swallow), the scan-code map the swallow depends on, the panel's
// ray intersection (flat and on the cylinder, the same math the shader
// transcribes), the door's eye transform, the one-value ini write's merge,
// and the key rules of menu_keys.h -- the swallow-until-release tracker,
// which of Elite's panel keys the menu adopts and why each other one is
// refused, and a footer composed against the real GDI ruler so that what
// the panel says is what it draws. Each is a table here.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/common/frame_flag.h"
#include "../../src/common/hotkey.h"
#include "../../src/common/iniedit.h"
#include "../../src/common/perf_math.h"
#include "../../src/d3d11/input_gate.h"
#include "../../src/d3d11/menu_keys.h"
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
    // The keys an Elite UI_Back may sit on (MEASURED at the desk 2026-09-11
    // through MapVirtualKeyW): the release tail must cover a Ctrl-as-Back on
    // both halves, and Backspace, or the closing key reaches the ship.
    check(inputGateDikOf(VK_LCONTROL) == 0x1D, "left Ctrl maps to DIK_LCONTROL");
    check(inputGateDikOf(VK_RCONTROL) == 0x9D, "right Ctrl maps to DIK_RCONTROL (extended)");
    check(inputGateDikOf(VK_LMENU) == 0x38, "left Alt maps to DIK_LMENU");
    check(inputGateDikOf(VK_RMENU) == 0xB8, "right Alt maps to DIK_RMENU (extended)");
    check(inputGateDikOf(VK_BACK) == 0x0E, "Backspace maps to DIK_BACK");
}

// --- the hotkey registry's keys ----------------------------------------------

void testHotkeyRegisteredKeys() {
    hotkeyResetBindings();
    Hotkey a, b;
    a.setBinding("F9");
    b.setBinding("CTRL+ALT+SPACE");
    int vks[16] = {};
    int n = hotkeyRegisteredKeys(vks, 16);
    check(n == 2 && vks[0] == VK_F9 && vks[1] == VK_SPACE,
          "registered keys: exactly the two bindings' keys, in registration order");
    // The registry holds sixteen: fifteen more distinct keys make seventeen
    // registrations, the last of which is dropped in silence -- which is
    // why the menu never registers its aliases there (menu_keys.h, R5).
    const char* more[15] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8",
                            "F10", "F11", "F12", "F13", "F14", "F15", "F16"};
    Hotkey extra[15];
    for (int i = 0; i < 15; ++i) extra[i].setBinding(more[i]);
    n = hotkeyRegisteredKeys(vks, 16);
    bool f16 = false;
    for (int i = 0; i < n; ++i) f16 = f16 || vks[i] == VK_F16;
    check(n == 16 && !f16, "registered keys: the seventeenth registration is dropped and sixteen reported");
    check(hotkeyRegisteredKeys(vks, 4) == 4, "registered keys: a short buffer takes the first few");
    check(hotkeyRegisteredKeys(nullptr, 16) == 0 && hotkeyRegisteredKeys(vks, 0) == 0,
          "registered keys: no buffer, nothing copied");
    hotkeyResetBindings();
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

// Project a fixed panel with the measured runtime matrix, then recover
// its points through the production menu frustum and ray intersection.
void testAsymmetricProjection() {
    announceEyeTangents(1.376f, 0.839f);
    announceEyeTangentsVertical(1.428f, 0.966f);
    const float identity[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};
    bool all = true;
    int samples = 0;
    for (int eye = 0; eye < 2; ++eye) {
        float tans[4];
        menuPanelFrustum(eye, 2528, 2704, tans);
        const float l = eye == 0 ? -1.376f : -0.839f;
        const float r = eye == 0 ? 0.839f : 1.376f;
        const float p00 = 2 / (r-l), p02 = (r+l) / (r-l);
        const float p11 = 2 / (0.966f+1.428f);
        const float p12 = (0.966f-1.428f) / (0.966f+1.428f);
        const float offset[3] = {eye ? 0.0319f : -0.0319f, 0, 0};
        for (float yaw : {-25.0f, 0.0f, 30.0f}) {
            for (float pitch : {-20.0f, 0.0f, 25.0f}) {
                float current[12], xf[12];
                menuHeadLockAnchor(identity, yaw, pitch, current);
                current[3] = 0.07f; current[7] = -0.04f;
                menuDoorXform(identity, current, offset, xf);
                for (float curve : {0.0f, 0.2f}) {
                    for (float x : {-0.2f, 0.0f, 0.2f}) {
                        for (float y : {-0.1f, 0.0f, 0.1f}) {
                            const float R = curve > 0 ? 1.4f / curve : 1.0f;
                            const float world[3] = {curve > 0 ? R*sinf(x/R) : x, y,
                                curve > 0 ? R-1.4f-R*cosf(x/R) : -1.4f};
                            float view[3] = {};
                            for (int i=0;i<3;++i) {
                                for (int j=0;j<3;++j)
                                    view[i] += current[j*4+i]*(world[j]-current[j*4+3]);
                                view[i] -= offset[i];
                            }
                            const float u = (p00*view[0]/-view[2]-p02+1)*0.5f;
                            const float v = (1-(p11*view[1]/-view[2]-p12))*0.5f;
                            const float ray[3] = {-tans[0]+u*(tans[0]+tans[1]),
                                                  tans[2]-v*(tans[2]+tans[3]), -1};
                            float dir[3] = {};
                            for (int i=0;i<3;++i)
                                for (int j=0;j<3;++j) dir[i] += xf[i*3+j]*ray[j];
                            float su=0, sv=0;
                            all &= menuPanelHit(xf+9, dir, 1.4f, curve, 0.3f, 0.2f, 0, &su, &sv)
                                && approx(su, (x+0.3f)/0.6f) && approx(sv, (y+0.2f)/0.4f);
                            ++samples;
                        }
                    }
                }
            }
        }
    }
    check(all && samples == 324, "Quest projection: fixed panel survives stereo, head turns and translation");
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
    PerfRecentTimes recent;
    recent.add(20,1,3,true,9,2);
    recent.add(18,1,3,true,11,4);
    recent.add(22,1,3,false,999,999); // unsettled stamps cannot enter app/GPU means
    check(approx(recent.cpuMs(),3)&&approx(recent.gpuMs(),10),"monitor: both surfaces select settled app time, not longer render-thread time");
    check(approx(recent.threadMs(),16),"monitor: render-thread time remains available separately");
    PerfRecentTimes unavailable;unavailable.add(20,1,3,false,0,0);
    check(!unavailable.appCount&&approx(unavailable.cpuMs(),16)&&unavailable.gpuMs()==0,"monitor: missing compositor timings use an explicitly identifiable thread fallback");
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

// --- the key tracker ---------------------------------------------------------

void testKeyRepeatStep() {
    KeyRepeat k;
    k.vk = 'W';
    check(keyRepeatStep(k, true, 1000, true) == 1, "tracker: the edge fires once");
    check(keyRepeatStep(k, true, 1010, true) == 0, "tracker: held short of the first repeat is silent");
    check(keyRepeatStep(k, true, 1000 + kKeyRepeatFirstMs, true) == 1, "tracker: the first repeat at +400");
    check(keyRepeatStep(k, true, 1000 + kKeyRepeatFirstMs + 82, true) == 0, "tracker: not yet the next");
    check(keyRepeatStep(k, true, 1000 + kKeyRepeatFirstMs + kKeyRepeatMs, true) == 1,
          "tracker: then every 83 ms");
    check(keyRepeatStep(k, false, 2000, true) == 0 && !k.down && k.nextMs == 0,
          "tracker: release resets it");
    // act=false tracks and swallows: the key is parked, and stays parked
    // when the state later allows it -- until it is released.
    check(keyRepeatStep(k, true, 3000, false) == 0 && k.down && k.nextMs == kKeyRepeatParked,
          "tracker: a swallowed press is parked, not fired");
    check(keyRepeatStep(k, true, 13000, true) == 0,
          "tracker: held through a swallowed state and then allowed, ten seconds on: still nothing");
    check(keyRepeatStep(k, true, 23000, true) == 0, "tracker: ...and no repeat train either");
    keyRepeatStep(k, false, 23100, true);
    check(keyRepeatStep(k, true, 23200, true) == 1, "tracker: released and pressed afresh fires");
    // A key held in the repeat train that the state stops allowing is
    // parked from there (E held across Fixes -> Monitor).
    keyRepeatStep(k, true, 23200 + kKeyRepeatFirstMs, true);
    check(keyRepeatStep(k, true, 23200 + kKeyRepeatFirstMs + 10, false) == 0 && k.nextMs == kKeyRepeatParked,
          "tracker: a repeating key that stops being allowed is parked mid-train");
    check(keyRepeatStep(k, true, 30000, true) == 0, "tracker: ...and stays parked once allowed again");
    // Priming from the raw key: down parks, up is fresh.
    keyRepeatPrime(k, true);
    check(k.down && k.nextMs == kKeyRepeatParked, "prime: a key that is down is parked");
    check(keyRepeatStep(k, true, 40000, true) == 0, "prime: a key held at open does nothing until released");
    keyRepeatStep(k, false, 40100, true);
    check(keyRepeatStep(k, true, 40200, true) == 1, "prime: ...and fires once pressed afresh");
    keyRepeatPrime(k, false);
    check(!k.down && k.nextMs == 0, "prime: a key that is up is fresh");
    check(keyRepeatStep(k, true, 50000, true) == 1, "prime: ...and its next press is an edge");
}

// A letter-bound UI_Select opens a Number row for typing while the letter
// is still down: primed from the raw key at beginEdit, the edit tracker
// parks it, and nothing lands in the value it opened.
void testEditPrime() {
    KeyRepeat f;
    f.vk = 'F';
    keyRepeatPrime(f, true);
    check(keyRepeatStep(f, true, 100, true) == 0, "edit prime: the letter that opened the edit does not type");
    check(keyRepeatStep(f, true, 100 + kKeyRepeatFirstMs + 1, true) == 0,
          "edit prime: ...and does not repeat into the value either");
    keyRepeatStep(f, false, 700, true);
    check(keyRepeatStep(f, true, 800, true) == 1, "edit prime: released and pressed again, it types");
}

// --- the alias rules ---------------------------------------------------------

// The menu's own keys as menu.cpp's resolveAliases passes them: initKeys'
// twelve with their navs, Escape, the summon chord, and the registry
// (which holds the summon key too, since setBinding registers it).
MenuFixedKeys fixedKeys(const char* summon, int extraRegistered = 0) {
    MenuFixedKeys f{};
    const int vks[12] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_SPACE,
                         VK_TAB, VK_PRIOR, VK_NEXT, VK_HOME, VK_END, 'R'};
    const MenuNav navs[12] = {kNavUp, kNavDown, kNavLeft, kNavRight, kNavSelect, kNavSelect,
                              kNavPageNext, kNavReadBack, kNavReadOn, kNavHome, kNavEnd, kNavReset};
    for (int i = 0; i < 12; ++i) {
        f.vks[i] = vks[i];
        f.navs[i] = navs[i];
    }
    f.count = 12;
    f.escapeVk = VK_ESCAPE;
    uint32_t mods = 0;
    f.summonVk = virtualKeyFromName(summon, &mods);
    f.summonMods = mods;
    f.registered[f.registeredCount++] = f.summonVk;
    if (extraRegistered) f.registered[f.registeredCount++] = extraRegistered;
    return f;
}

MenuAliasInput uiElement(const char* element) {
    MenuAliasInput in{};
    in.element = element;
    for (const MenuUiElement& e : kMenuUiElements) {
        if (strcmp(e.element, element) == 0) {
            in.nav = e.nav;
            in.repeats = e.repeats;
        }
    }
    return in;
}

void keySlot(MenuAliasInput& in, const char* binding, const char* elite, bool chorded = false,
             bool modifierMain = false) {
    const int s = in.slots++;
    in.binding[s] = binding;
    in.eliteName[s] = elite;
    in.keyboard[s] = true;
    in.chorded[s] = chorded;
    in.modifierMain[s] = modifierMain;
}

void padSlot(MenuAliasInput& in, const char* elite) {
    const int s = in.slots++;
    in.binding[s] = "";
    in.eliteName[s] = elite;
    in.keyboard[s] = false;
}

// The stock KeyboardMouseOnly scheme's ten elements (MEASURED from
// ControlSchemes\KeyboardMouseOnly.binds, 2026-09-11).
int stockInput(MenuAliasInput* in) {
    int n = 0;
    in[n] = uiElement("UI_Up");                keySlot(in[n], "W", "Key_W");         keySlot(in[n], "UP", "Key_UpArrow");       ++n;
    in[n] = uiElement("UI_Down");              keySlot(in[n], "S", "Key_S");         keySlot(in[n], "DOWN", "Key_DownArrow");   ++n;
    in[n] = uiElement("UI_Left");              keySlot(in[n], "A", "Key_A");         keySlot(in[n], "LEFT", "Key_LeftArrow");   ++n;
    in[n] = uiElement("UI_Right");             keySlot(in[n], "D", "Key_D");         keySlot(in[n], "RIGHT", "Key_RightArrow"); ++n;
    in[n] = uiElement("UI_Select");            keySlot(in[n], "SPACE", "Key_Space");                                            ++n;
    in[n] = uiElement("UI_Back");              keySlot(in[n], "BACKSPACE", "Key_Backspace"); padSlot(in[n], "Mouse_2");         ++n;
    in[n] = uiElement("CycleNextPanel");       keySlot(in[n], "E", "Key_E");         keySlot(in[n], "END", "Key_End");          ++n;
    in[n] = uiElement("CyclePreviousPanel");   keySlot(in[n], "Q", "Key_Q");         keySlot(in[n], "DELETE", "Key_Delete");    ++n;
    in[n] = uiElement("CycleNextPage");        keySlot(in[n], "C", "Key_C");         keySlot(in[n], "HOME", "Key_Home");        ++n;
    in[n] = uiElement("CyclePreviousPage");    keySlot(in[n], "Z", "Key_Z");         keySlot(in[n], "INSERT", "Key_Insert");    ++n;
    return n;
}

// The maintainer's Custom.4.2.binds (MEASURED 2026-09-11): a gamepad on
// every Primary, the letters on the Secondaries, and Key_LeftControl as
// UI_Back -- which elite_binds hands over as the raw "0xA2" under the
// menu-only flag.
int seanInput(MenuAliasInput* in) {
    int n = 0;
    in[n] = uiElement("UI_Up");                padSlot(in[n], "GamePad_DPadUp");    keySlot(in[n], "W", "Key_W");                                ++n;
    in[n] = uiElement("UI_Down");              padSlot(in[n], "GamePad_DPadDown");  keySlot(in[n], "S", "Key_S");                                ++n;
    in[n] = uiElement("UI_Left");              padSlot(in[n], "GamePad_DPadLeft");  keySlot(in[n], "A", "Key_A");                                ++n;
    in[n] = uiElement("UI_Right");             padSlot(in[n], "GamePad_DPadRight"); keySlot(in[n], "D", "Key_D");                                ++n;
    in[n] = uiElement("UI_Select");            padSlot(in[n], "GamePad_A");         keySlot(in[n], "SPACE", "Key_Space");                        ++n;
    in[n] = uiElement("UI_Back");              padSlot(in[n], "GamePad_B");         keySlot(in[n], "0xA2", "Key_LeftControl", false, true);      ++n;
    in[n] = uiElement("CycleNextPanel");       padSlot(in[n], "GamePad_RBumper");   keySlot(in[n], "E", "Key_E");                                ++n;
    in[n] = uiElement("CyclePreviousPanel");   padSlot(in[n], "GamePad_LBumper");   keySlot(in[n], "Q", "Key_Q");                                ++n;
    in[n] = uiElement("CycleNextPage");        padSlot(in[n], "GamePad_RTrigger");  keySlot(in[n], "C", "Key_C");                                ++n;
    in[n] = uiElement("CyclePreviousPage");    padSlot(in[n], "GamePad_LTrigger");  keySlot(in[n], "Z", "Key_Z");                                ++n;
    return n;
}

// The invariant every adopted alias must satisfy, whatever the input: a
// unique vk that is not a fixed key, not Escape, not the summon key and
// not a registered hotkey. Polling one of those twice is the bug class.
bool aliasInvariant(const MenuAliasTable& t, const MenuFixedKeys& f, const char** why) {
    *why = "";
    for (int i = 0; i < t.count; ++i) {
        const int vk = t.alias[i].vk;
        for (int j = 0; j < i; ++j) {
            if (t.alias[j].vk == vk) { *why = "a vk adopted twice"; return false; }
        }
        for (int j = 0; j < f.count; ++j) {
            if (f.vks[j] == vk) { *why = "a fixed key adopted"; return false; }
        }
        if (vk == f.escapeVk) { *why = "Escape adopted"; return false; }
        if (vk == f.summonVk) { *why = "the summon key adopted"; return false; }
        for (int j = 0; j < f.registeredCount; ++j) {
            if (f.registered[j] == vk) { *why = "a registered hotkey adopted"; return false; }
        }
    }
    return true;
}

// Which adopted alias sits on `vk`, or null.
const MenuAlias* aliasOn(const MenuAliasTable& t, int vk) {
    for (int i = 0; i < t.count; ++i) {
        if (t.alias[i].vk == vk) return &t.alias[i];
    }
    return nullptr;
}

// The skip recorded for a slot named `name` (its display or Elite name).
const MenuAliasSkipped* skipNamed(const MenuAliasTable& t, const char* name) {
    for (int i = 0; i < t.skippedCount; ++i) {
        if (strcmp(t.skipped[i].name, name) == 0) return &t.skipped[i];
    }
    return nullptr;
}

bool skipIs(const MenuAliasTable& t, const char* name, MenuAliasSkip reason) {
    const MenuAliasSkipped* k = skipNamed(t, name);
    return k && k->reason == reason;
}

void testAliasResolveStock() {
    MenuAliasInput in[kMenuUiElementCount];
    const int n = stockInput(in);
    const MenuFixedKeys f = fixedKeys("F8");
    MenuAliasTable t;
    menuAliasResolve(in, n, f, &t);
    // The adopted set, in table order, with the right navs and repeats.
    struct Want { int vk; MenuNav nav; bool repeats; };
    const Want want[11] = {{'W', kNavUp, true},          {'S', kNavDown, true},
                           {'A', kNavLeft, true},        {'D', kNavRight, true},
                           {VK_BACK, kNavBack, false},   {'E', kNavPageNext, false},
                           {'Q', kNavPagePrev, false},   {VK_DELETE, kNavPagePrev, false},
                           {'C', kNavReadOn, true},      {'Z', kNavReadBack, true},
                           {VK_INSERT, kNavReadBack, true}};
    bool order = t.count == 11;
    for (int i = 0; order && i < 11; ++i) {
        order = t.alias[i].vk == want[i].vk && t.alias[i].nav == want[i].nav &&
                t.alias[i].repeats == want[i].repeats;
    }
    check(order && t.adopted, "stock: W S A D Backspace E Q Del C Z Ins adopted, in table order, navs and repeats right");
    check(skipIs(t, "Up", kSkipSameAsFixed) && skipIs(t, "Down", kSkipSameAsFixed) &&
              skipIs(t, "Left", kSkipSameAsFixed) && skipIs(t, "Right", kSkipSameAsFixed) &&
              skipIs(t, "Space", kSkipSameAsFixed),
          "stock: the arrows and Space are the menu's own keys with the same meaning");
    check(skipIs(t, "End", kSkipFixedWins) && skipIs(t, "Home", kSkipFixedWins),
          "stock: End as next-panel and Home as next-page lose to the menu's last-row and first-row keys");
    check(skipIs(t, "Mouse_2", kSkipNotKeyboard), "stock: the mouse button is recorded, not adopted");
    check(t.skippedCount == 8, "stock: eight slots skipped, no more");
    check(strcmp(t.navName[kNavUp], "W") == 0 && strcmp(t.navName[kNavDown], "S") == 0 &&
              strcmp(t.navName[kNavLeft], "A") == 0 && strcmp(t.navName[kNavRight], "D") == 0 &&
              strcmp(t.navName[kNavSelect], "Space") == 0 && strcmp(t.navName[kNavBack], "Backspace") == 0 &&
              strcmp(t.navName[kNavPageNext], "E") == 0 && strcmp(t.navName[kNavPagePrev], "Q") == 0 &&
              strcmp(t.navName[kNavReadOn], "C") == 0 && strcmp(t.navName[kNavReadBack], "Z") == 0,
          "stock: the legend names W S A D Space Backspace E Q C Z");
    check(strcmp(t.navName[kNavHome], "Home") == 0 && strcmp(t.navName[kNavEnd], "End") == 0 &&
              strcmp(t.navName[kNavReset], "R") == 0,
          "stock: the navs no alias feeds keep the fixed names");
    const char* why = "";
    check(aliasInvariant(t, f, &why), "stock: every adopted vk is unique and none is fixed, Escape, summon or registered", why);
    char text[512];
    menuAliasSummary(t, text, sizeof(text));
    check(strcmp(text, "pick W/S, change A/D, select Space, page Q/E (also Del), read on Z/C (also Ins), back Backspace") == 0,
          "stock: the log's summary", text);
    menuAliasSkippedText(t, text, sizeof(text));
    check(strcmp(text, "Up (pick; already the menu's key), Down (pick; already the menu's key), "
                       "Left (change; already the menu's key), Right (change; already the menu's key), "
                       "Space (select; already the menu's key), Mouse_2 (back; on your mouse), "
                       "End (page; the menu's last-row key keeps its meaning), "
                       "Home (read on; the menu's first-row key keeps its meaning)") == 0,
          "stock: the log's skipped list", text);
    menuAliasStatusValue(t, text, sizeof(text));
    check(strcmp(text, "W/S A/D Space Q/E Z/C Backspace") == 0, "stock: the Status row's value", text);
}

void testAliasResolveSean() {
    MenuAliasInput in[kMenuUiElementCount];
    const int n = seanInput(in);
    const MenuFixedKeys f = fixedKeys("F8");
    MenuAliasTable t;
    menuAliasResolve(in, n, f, &t);
    int pads = 0;
    for (int i = 0; i < t.skippedCount; ++i) pads += t.skipped[i].reason == kSkipNotKeyboard;
    check(pads == 10 && skipIs(t, "GamePad_DPadUp", kSkipNotKeyboard),
          "sean: every gamepad Primary is recorded by its Elite name and never adopted");
    const MenuAlias* back = aliasOn(t, VK_LCONTROL);
    check(back && back->nav == kNavBack && !back->repeats && back->modifierMain &&
              strcmp(back->name, "L-Ctrl") == 0 && strcmp(back->eliteName, "Key_LeftControl") == 0,
          "sean: left Ctrl is adopted as Back, edge-only, named L-Ctrl");
    check(t.count == 9 && aliasOn(t, 'W') && aliasOn(t, 'S') && aliasOn(t, 'A') && aliasOn(t, 'D') &&
              aliasOn(t, 'E') && aliasOn(t, 'Q') && aliasOn(t, 'C') && aliasOn(t, 'Z'),
          "sean: the eight letters and the Ctrl are the whole table");
    check(skipIs(t, "Space", kSkipSameAsFixed) && strcmp(t.navName[kNavSelect], "Space") == 0,
          "sean: Space is the menu's own select key and still names the legend");
    check(strcmp(t.navName[kNavBack], "L-Ctrl") == 0, "sean: the legend's back name is L-Ctrl");
    const char* why = "";
    check(aliasInvariant(t, f, &why), "sean: the invariant holds", why);
    char text[512];
    menuAliasSummary(t, text, sizeof(text));
    check(strcmp(text, "pick W/S, change A/D, select Space, page Q/E, read on Z/C, "
                       "back L-Ctrl (Key_LeftControl, a modifier key used as a key; the menu only)") == 0,
          "sean: the log's summary names the modifier key as one", text);
    menuAliasStatusValue(t, text, sizeof(text));
    check(strcmp(text, "W/S A/D Space Q/E Z/C L-Ctrl") == 0 && strlen(text) == 28,
          "sean: the Status row's value, 28 characters", text);
    // UI_Toggle is absent by construction: the table has no such element.
    bool toggle = false;
    for (const MenuUiElement& e : kMenuUiElements) toggle = toggle || strcmp(e.element, "UI_Toggle") == 0;
    check(!toggle, "sean: UI_Toggle is not among the elements read");
}

void testAliasResolveAdversarial() {
    MenuAliasTable t;
    MenuAliasInput in[3];
    const char* why = "";
    char text[256];

    // A hand-made UI_Select on Tab: the menu's page key keeps its meaning.
    in[0] = uiElement("UI_Select");
    keySlot(in[0], "TAB", "Key_Tab");
    menuAliasResolve(in, 1, fixedKeys("F8"), &t);
    check(t.count == 0 && skipIs(t, "Tab", kSkipFixedWins) && t.skipped[0].fixedNav == kNavPageNext,
          "adversarial: UI_Select on Tab loses to the page key");
    // R2 never names the legend: "Tab select" while Tab pages would be a lie.
    check(strcmp(t.navName[kNavSelect], "Enter") == 0, "adversarial: ...and the legend keeps the fixed name", t.navName[kNavSelect]);
    menuAliasSkippedText(t, text, sizeof(text));
    check(strcmp(text, "Tab (select; the menu's page key keeps its meaning)") == 0,
          "adversarial: ...and the log says which meaning", text);
    // A list that does not fit its buffer says so rather than ending
    // mid-word with the rest missing (the log reads a missing key as
    // "not skipped").
    MenuAliasInput stockIn[kMenuUiElementCount];
    MenuAliasTable stock;
    menuAliasResolve(stockIn, stockInput(stockIn), fixedKeys("F8"), &stock);
    char small[80];
    menuAliasSkippedText(stock, small, sizeof(small));
    check(strlen(small) < sizeof(small) && strlen(small) >= 5 && strcmp(small + strlen(small) - 5, ", ...") == 0 &&
              strstr(small, "Up (pick; already the menu's key)") == small,
          "adversarial: a skipped list that overflows its buffer ends in a marker", small);
    char whole[512];
    menuAliasSkippedText(stock, whole, sizeof(whole));
    check(strstr(whole, ", ...") == nullptr && strstr(whole, "Home (read on;") != nullptr,
          "adversarial: ...and one that fits carries every item and no marker", whole);

    // UI_Down on Down: the same meaning, so the fixed key already does it.
    in[0] = uiElement("UI_Down");
    keySlot(in[0], "DOWN", "Key_DownArrow");
    menuAliasResolve(in, 1, fixedKeys("F8"), &t);
    check(t.count == 0 && skipIs(t, "Down", kSkipSameAsFixed) && strcmp(t.navName[kNavDown], "Down") == 0,
          "adversarial: UI_Down on Down is the menu's own key");

    // CycleNextPanel on Tab: same meaning as the menu's Tab.
    in[0] = uiElement("CycleNextPanel");
    keySlot(in[0], "TAB", "Key_Tab");
    menuAliasResolve(in, 1, fixedKeys("F8"), &t);
    check(t.count == 0 && skipIs(t, "Tab", kSkipSameAsFixed), "adversarial: next-panel on Tab is the menu's own key");

    // UI_Back on Escape: menuTick's own edge already closes.
    in[0] = uiElement("UI_Back");
    keySlot(in[0], "ESCAPE", "Key_Escape");
    menuAliasResolve(in, 1, fixedKeys("F8"), &t);
    check(t.count == 0 && skipIs(t, "Esc", kSkipSameAsFixed), "adversarial: UI_Back on Escape is the menu's own key");

    // UI_Back on the summon key: it would close what it opens.
    in[0] = uiElement("UI_Back");
    keySlot(in[0], "F8", "Key_F8");
    menuAliasResolve(in, 1, fixedKeys("F8"), &t);
    check(t.count == 0 && skipIs(t, "F8", kSkipSummonKey) && !t.skipped[0].summonChord,
          "adversarial: UI_Back on the summon key is refused");
    menuAliasSkippedText(t, text, sizeof(text));
    check(strcmp(text, "F8 (back; it is your menu key (hotkey.menu))") == 0, "adversarial: ...named as the menu key", text);

    // A Ctrl-as-Back with hotkey.menu = CTRL+F8: half of the chord that
    // opens the menu would close it. With a bare F8 it is admitted.
    in[0] = uiElement("UI_Back");
    keySlot(in[0], "0xA2", "Key_LeftControl", false, true);
    menuAliasResolve(in, 1, fixedKeys("CTRL+F8"), &t);
    check(t.count == 0 && skipIs(t, "L-Ctrl", kSkipSummonKey) && t.skipped[0].summonChord,
          "adversarial: left Ctrl as Back is refused under a CTRL+F8 menu key");
    menuAliasSkippedText(t, text, sizeof(text));
    check(strcmp(text, "L-Ctrl (back; half of your menu chord (hotkey.menu))") == 0,
          "adversarial: ...named as half of the chord", text);
    menuAliasResolve(in, 1, fixedKeys("F8"), &t);
    check(t.count == 1 && aliasOn(t, VK_LCONTROL) && aliasOn(t, VK_LCONTROL)->nav == kNavBack,
          "adversarial: ...and admitted under a bare F8");
    // The rule is per modifier FAMILY: Ctrl is half of a CTRL+ chord only,
    // Alt of an ALT+ one. A Ctrl-as-Back under ALT+F8 is admitted (the
    // chord does not contain it), an Alt-as-Back under ALT+F8 is refused,
    // and under CTRL+F8 admitted.
    menuAliasResolve(in, 1, fixedKeys("ALT+F8"), &t);
    check(t.count == 1 && aliasOn(t, VK_LCONTROL) && aliasOn(t, VK_LCONTROL)->nav == kNavBack,
          "adversarial: left Ctrl as Back is admitted under an ALT+F8 menu key");
    in[0] = uiElement("UI_Back");
    keySlot(in[0], "0xA4", "Key_LeftAlt", false, true);
    menuAliasResolve(in, 1, fixedKeys("ALT+F8"), &t);
    check(t.count == 0 && skipIs(t, "L-Alt", kSkipSummonKey) && t.skipped[0].summonChord,
          "adversarial: left Alt as Back is refused under an ALT+F8 menu key");
    menuAliasResolve(in, 1, fixedKeys("CTRL+F8"), &t);
    check(t.count == 1 && aliasOn(t, VK_LMENU) && aliasOn(t, VK_LMENU)->nav == kNavBack,
          "adversarial: ...and admitted under a CTRL+F8 one");

    // Shift as a key: the menu's own modifier.
    in[0] = uiElement("UI_Up");
    keySlot(in[0], "0xA0", "Key_LeftShift", false, true);
    menuAliasResolve(in, 1, fixedKeys("F8"), &t);
    check(t.count == 0 && skipIs(t, "L-Shift", kSkipShiftKey), "adversarial: left Shift is refused");

    // UI_Left on R: the reset key keeps its meaning.
    in[0] = uiElement("UI_Left");
    keySlot(in[0], "R", "Key_R");
    menuAliasResolve(in, 1, fixedKeys("F8"), &t);
    check(t.count == 0 && skipIs(t, "R", kSkipFixedWins) && t.skipped[0].fixedNav == kNavReset,
          "adversarial: UI_Left on R loses to the reset key");
    check(strcmp(t.navName[kNavLeft], "Left") == 0, "adversarial: ...and does not name the legend", t.navName[kNavLeft]);

    // An EDVR hotkey on the key (Scroll Lock, the exposure toggle).
    in[0] = uiElement("UI_Up");
    keySlot(in[0], "SCROLLLOCK", "Key_ScrollLock");
    menuAliasResolve(in, 1, fixedKeys("F8", VK_SCROLL), &t);
    check(t.count == 0 && skipIs(t, "ScrLk", kSkipEdvrHotkey), "adversarial: a registered hotkey's key is refused");
    menuAliasResolve(in, 1, fixedKeys("F8"), &t);
    check(t.count == 1, "adversarial: ...and adopted when nothing is registered on it");

    // A chord: refused, by the slot's flag and by the binding string alike.
    in[0] = uiElement("CycleNextPanel");
    keySlot(in[0], "SHIFT+E", "Key_E", true);
    menuAliasResolve(in, 1, fixedKeys("F8"), &t);
    check(t.count == 0 && skipIs(t, "SHIFT+E", kSkipChord), "adversarial: a chorded slot is refused");
    in[0] = uiElement("CycleNextPanel");
    keySlot(in[0], "SHIFT+E", "Key_E", false);
    menuAliasResolve(in, 1, fixedKeys("F8"), &t);
    check(t.count == 0 && skipIs(t, "SHIFT+E", kSkipChord),
          "adversarial: a binding string with a modifier is a chord whatever the flag says");

    // Two elements on one key: first in table order wins.
    in[0] = uiElement("UI_Select");
    keySlot(in[0], "F", "Key_F");
    in[1] = uiElement("CycleNextPage");
    keySlot(in[1], "F", "Key_F");
    menuAliasResolve(in, 2, fixedKeys("F8"), &t);
    check(t.count == 1 && aliasOn(t, 'F') && aliasOn(t, 'F')->nav == kNavSelect &&
              skipIs(t, "F", kSkipDuplicate) && t.skipped[0].nav == kNavReadOn,
          "adversarial: UI_Select and CycleNextPage both on F: the first wins");
    menuAliasSkippedText(t, text, sizeof(text));
    check(strcmp(text, "F (read on; already adopted for UI_Select)") == 0, "adversarial: ...and the log names the winner", text);

    // Primary and Secondary on the same key: one alias.
    in[0] = uiElement("UI_Up");
    keySlot(in[0], "W", "Key_W");
    keySlot(in[0], "W", "Key_W");
    menuAliasResolve(in, 1, fixedKeys("F8"), &t);
    check(t.count == 1 && t.skippedCount == 1 && skipIs(t, "W", kSkipDuplicate),
          "adversarial: Primary == Secondary makes one alias");

    // A key this build cannot name, carried by Elite's spelling.
    in[0] = uiElement("UI_Back");
    keySlot(in[0], "", "Key_Foo");
    menuAliasResolve(in, 1, fixedKeys("F8"), &t);
    check(t.count == 0 && skipIs(t, "Key_Foo", kSkipUnnamed), "adversarial: an unnamed key carries Elite's name");
    menuAliasSkippedText(t, text, sizeof(text));
    check(text[0] == 0, "adversarial: ...and is left to the log's own please-report line");
    in[0] = uiElement("UI_Back");
    keySlot(in[0], "WOMBAT", "Key_Wombat");
    menuAliasResolve(in, 1, fixedKeys("F8"), &t);
    check(t.count == 0 && skipIs(t, "Key_Wombat", kSkipUnnamed),
          "adversarial: a binding the hotkey parser rejects is unnamed too");

    // Nothing in: nothing adopted, the fixed names throughout.
    menuAliasResolve(nullptr, 0, fixedKeys("F8"), &t);
    check(t.count == 0 && t.skippedCount == 0 && !t.adopted && strcmp(t.navName[kNavUp], "Up") == 0 &&
              strcmp(t.navName[kNavSelect], "Enter") == 0 && strcmp(t.navName[kNavBack], "Esc") == 0 &&
              strcmp(t.navName[kNavPagePrev], "Tab") == 0 && strcmp(t.navName[kNavReadOn], "PgDn") == 0 &&
              strcmp(t.navName[kNavReadBack], "PgUp") == 0,
          "adversarial: empty input gives the fixed names and no aliases");
    menuAliasSummary(t, text, sizeof(text));
    check(text[0] == 0, "adversarial: ...and an empty summary");
    check(aliasInvariant(t, fixedKeys("F8"), &why), "adversarial: the invariant holds on nothing", why);
}

// The same inputs twice give memcmp-equal tables -- padding included --
// which is what "your bindings changed but read the same" relies on.
void testAliasResolvePure() {
    MenuAliasInput in[kMenuUiElementCount];
    const int n = stockInput(in);
    const MenuFixedKeys f = fixedKeys("F8");
    MenuAliasTable a, b;
    memset(&a, 0x5A, sizeof(a));
    memset(&b, 0xA5, sizeof(b));
    menuAliasResolve(in, n, f, &a);
    menuAliasResolve(in, n, f, &b);
    check(memcmp(&a, &b, sizeof(a)) == 0, "pure: the same inputs give byte-identical tables");
    MenuAliasInput other[kMenuUiElementCount];
    const int m = seanInput(other);
    menuAliasResolve(other, m, f, &b);
    check(memcmp(&a, &b, sizeof(a)) != 0, "pure: different inputs give a different table");
}

void testAliasMayAct() {
    check(!menuAliasMayAct(false, false, false) && !menuAliasMayAct(false, true, false) &&
              !menuAliasMayAct(false, false, true) && !menuAliasMayAct(false, true, true),
          "may act: never without the gate holding the game's keyboard");
    check(!menuAliasMayAct(true, true, false) && !menuAliasMayAct(true, true, true),
          "may act: never while a value is typed");
    check(!menuAliasMayAct(true, false, true), "may act: never on a status page");
    check(menuAliasMayAct(true, false, false), "may act: only with the gate, not typing, on a settings page");
    // The page pair follows Tab: it acts on a status page and without the
    // gate (menu.keyboard = shared, a door not reached), and only typing
    // holds it. Every other nav is the predicate above.
    check(menuAliasFollowsTab(kNavPageNext) && menuAliasFollowsTab(kNavPagePrev) &&
              !menuAliasFollowsTab(kNavUp) && !menuAliasFollowsTab(kNavSelect) &&
              !menuAliasFollowsTab(kNavBack) && !menuAliasFollowsTab(kNavReadOn),
          "follows Tab: exactly the two page navs");
    check(menuAliasMayActNav(kNavPageNext, false, false, true) &&
              menuAliasMayActNav(kNavPagePrev, false, false, true) &&
              menuAliasMayActNav(kNavPageNext, false, false, false) &&
              menuAliasMayActNav(kNavPageNext, true, false, false),
          "per nav: a page key acts on a status page and without the gate");
    check(!menuAliasMayActNav(kNavPageNext, true, true, false) &&
              !menuAliasMayActNav(kNavPagePrev, false, true, true),
          "per nav: a page key is held while a value is typed");
    check(!menuAliasMayActNav(kNavUp, false, false, true) && !menuAliasMayActNav(kNavUp, true, false, true) &&
              !menuAliasMayActNav(kNavSelect, false, false, false) &&
              !menuAliasMayActNav(kNavBack, true, false, true) &&
              menuAliasMayActNav(kNavUp, true, false, false),
          "per nav: every other key keeps the strict predicate");
}

void testKeyDisplayName() {
    char s[16];
    auto is = [&](int vk, const char* want) {
        menuKeyDisplayName(vk, s, sizeof(s));
        return strcmp(s, want) == 0;
    };
    check(is(VK_LCONTROL, "L-Ctrl") && is(VK_RMENU, "R-Alt") && is(VK_LSHIFT, "L-Shift"),
          "names: the sided modifiers");
    check(is(VK_NEXT, "PgDn") && is(VK_PRIOR, "PgUp") && is(VK_DELETE, "Del") && is(VK_INSERT, "Ins") &&
              is(VK_BACK, "Backspace") && is(VK_HOME, "Home") && is(VK_END, "End"),
          "names: the cursor cluster");
    // The punctuation row is named by what the calling thread's layout
    // types for it, so the expectation is asked of the same layout: the
    // literal '=' / '[' hold on en-US only (MEASURED there; a German layout
    // gives '+' and a non-ASCII 'ß', which falls back to the hex form), and
    // a build on another locale must not go red for a fact about this one.
    auto layoutNames = [&](int vk) {
        const UINT ch = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) & 0xFFFFu;
        char want[16];
        if (ch > 0x20 && ch < 0x7F) snprintf(want, sizeof(want), "%c", static_cast<char>(ch));
        else snprintf(want, sizeof(want), "0x%02X", vk & 0xFF);
        return is(vk, want);
    };
    check(layoutNames(VK_OEM_PLUS) && layoutNames(VK_OEM_4) && layoutNames(VK_OEM_1) && layoutNames(VK_OEM_5),
          "names: a punctuation key is the character the active layout types, or its hex when not printable");
    if (LOWORD(reinterpret_cast<UINT_PTR>(GetKeyboardLayout(0))) == 0x0409) {
        check(is(VK_OEM_PLUS, "=") && is(VK_OEM_4, "["), "names: on en-US the measured '=' and '['");
    }
    check(is('W', "W") && is('7', "7") && is(VK_NUMPAD5, "Num 5") && is(VK_DECIMAL, "Num .") &&
              is(VK_F12, "F12") && is(VK_F24, "F24") && is(VK_SPACE, "Space") && is(VK_ESCAPE, "Esc"),
          "names: letters, digits, the numpad, the F keys, Space and Esc");
    bool all = true;
    for (int vk = 1; vk < 256; ++vk) {
        menuKeyDisplayName(vk, s, sizeof(s));
        if (!s[0] || strlen(s) > 9) {
            all = false;
            printf("  name: vk 0x%02X -> \"%s\"\n", vk, s);
        }
    }
    check(all, "names: every vk 1..255 has a name of at most nine characters");
}

// --- the footer --------------------------------------------------------------

// A ruler of a fixed width per character: 13 px is Segoe UI's average at
// cap 30 (MEASURED: 745 px for 55 characters), so 770 px is the default
// line and the strings of the design fit or drop as they do in the raster.
int fakeMeasure(const char* s, void* ctx) {
    return static_cast<int>(strlen(s)) * static_cast<int>(reinterpret_cast<intptr_t>(ctx));
}
void* const kPx13 = reinterpret_cast<void*>(static_cast<intptr_t>(13));

// The real ruler, at the em the footer is drawn with.
int gdiMeasure(const char* s, void* ctx) {
    return menuPanelMeasureLine(s, static_cast<int>(reinterpret_cast<intptr_t>(ctx)));
}

MenuFooterInput footerInput(const MenuAliasTable* aliases, bool live) {
    MenuFooterInput in{};
    in.editing = false;
    in.statusPage = false;
    in.pageName = "Fixes";
    in.privateWanted = true;
    in.gatePrivate = true;
    in.adopted = aliases && aliases->adopted;
    in.aliasesLive = live && in.adopted;
    for (int i = 0; i < kNavCount; ++i) in.navName[i] = aliases ? aliases->navName[i] : nullptr;
    in.pendingN = 0;
    in.widthPx = 770;
    return in;
}

bool composeIs(const MenuFooterInput& in, MenuMeasureFn m, void* ctx, const char* want, char* got, size_t n) {
    menuComposeFooter(in, m, ctx, got, n);
    return strcmp(got, want) == 0;
}

void testFooterComposeExact() {
    MenuAliasInput stockIn[kMenuUiElementCount];
    MenuAliasTable stock;
    menuAliasResolve(stockIn, stockInput(stockIn), fixedKeys("F8"), &stock);
    MenuAliasTable none;
    menuAliasResolve(nullptr, 0, fixedKeys("F8"), &none);
    char got[256];

    // 2: no adopted keys, one line of fixed names.
    MenuFooterInput in = footerInput(&none, true);
    check(composeIs(in, fakeMeasure, kPx13, "Arrows pick/change  Enter select  Tab page  Esc close", got, sizeof(got)),
          "footer 2: the fixed names, no second line", got);
    // 3: stock, live: the adopted names and the reminder.
    in = footerInput(&stock, true);
    check(composeIs(in, fakeMeasure, kPx13,
                    "W/S pick  A/D change  Space select  Q/E page  Esc close\nTab, arrows and Enter work too",
                    got, sizeof(got)) &&
              strlen(got) == 86,
          "footer 3: stock live, 86 bytes", got);
    // The legend follows the LIVE predicate, never mere adoption -- except
    // the page pair, which follows Tab and is named wherever it is adopted.
    in = footerInput(&stock, false);
    check(composeIs(in, fakeMeasure, kPx13, "Arrows pick/change  Enter select  Q/E page  Esc close", got, sizeof(got)),
          "footer: adopted but not live shows the fixed names, the page pair excepted", got);
    // 5: the pending count.
    in = footerInput(&stock, true);
    in.pendingN = 2;
    check(composeIs(in, fakeMeasure, kPx13,
                    "W/S pick  A/D change  Space select  Q/E page  Esc close\n2 changes at next launch", got, sizeof(got)),
          "footer 5: two changes at next launch", got);
    in.pendingN = 1;
    check(composeIs(in, fakeMeasure, kPx13,
                    "W/S pick  A/D change  Space select  Q/E page  Esc close\n1 change at next launch", got, sizeof(got)),
          "footer 5: one change at next launch", got);
    // 6: private wanted but the gate is not: the fault line, fixed names.
    in = footerInput(&stock, false);
    in.gatePrivate = false;
    in.pendingN = 2;
    check(composeIs(in, fakeMeasure, kPx13,
                    "Arrows pick/change  Enter select  Q/E page  Esc close\nKEYS SHARED WITH THE GAME", got, sizeof(got)),
          "footer 6: the fault line beats the pending count", got);
    // 7: a status page, adopted and not. The page pair is named and ON
    // there (it follows Tab), so no "off" note.
    in = footerInput(&stock, false);
    in.statusPage = true;
    in.pageName = "Monitor";
    check(composeIs(in, fakeMeasure, kPx13, "Q/E page  Esc close\nkeys shared (Monitor)", got, sizeof(got)),
          "footer 7: Monitor with adopted keys names the page pair", got);
    in.pageName = "Status";
    check(composeIs(in, fakeMeasure, kPx13, "Q/E page  Esc close\nkeys shared (Status)", got, sizeof(got)),
          "footer 7: Status with adopted keys names the page pair", got);
    in = footerInput(&none, false);
    in.statusPage = true;
    in.pageName = "Monitor";
    check(composeIs(in, fakeMeasure, kPx13, "Tab page  Esc close\nkeys shared (Monitor)", got, sizeof(got)),
          "footer 7: Monitor without adopted keys", got);
    // 8: menu.keyboard = shared.
    in = footerInput(&stock, false);
    in.privateWanted = false;
    in.gatePrivate = false;
    in.pendingN = 3;
    // The page pair is on in shared mode too; the off note names the pick
    // pair, the first pair that is adopted and not Tab's.
    check(composeIs(in, fakeMeasure, kPx13,
                    "Arrows pick/change  Enter select  Q/E page  Esc close\nkeys shared (menu.keyboard)  W/S off",
                    got, sizeof(got)),
          "footer 8: shared keyboard with adopted keys beats the pending count", got);
    in = footerInput(&none, false);
    in.privateWanted = false;
    in.gatePrivate = false;
    check(composeIs(in, fakeMeasure, kPx13,
                    "Arrows pick/change  Enter select  Tab page  Esc close\nkeys shared (menu.keyboard)", got, sizeof(got)),
          "footer 8: shared keyboard without adopted keys", got);
    // 9: editing.
    in = footerInput(&stock, false);
    in.editing = true;
    check(composeIs(in, fakeMeasure, kPx13, "Type a value  Backspace deletes  Enter writes  Esc cancels", got, sizeof(got)),
          "footer 9: editing", got);
    in.pendingN = 1;
    check(composeIs(in, fakeMeasure, kPx13, "Type a value  Backspace deletes  Enter writes  Esc cancels\n1 change at next launch",
                    got, sizeof(got)),
          "footer 9: editing keeps the pending count", got);
    // The fault line wins while editing too: a stalled draw drops the gate
    // and the typing keys (W/A/S/D/Space) are the ship's.
    in.gatePrivate = false;
    check(composeIs(in, fakeMeasure, kPx13, "Type a value  Backspace deletes  Enter writes  Esc cancels\nKEYS SHARED WITH THE GAME",
                    got, sizeof(got)),
          "footer 9: the fault line is shown while editing", got);
    // The pending count beats the reminder.
    in = footerInput(&stock, true);
    in.pendingN = 1;
    check(composeIs(in, fakeMeasure, kPx13, "W/S pick  A/D change  Space select  Q/E page  Esc close\n1 change at next launch",
                    got, sizeof(got)),
          "footer: the pending count beats the reminder", got);
    // 11: long names -- a scheme on the cursor cluster -- drop `change` and fit.
    MenuAliasTable cluster = stock;
    strcpy(cluster.navName[kNavUp], "Home");
    strcpy(cluster.navName[kNavDown], "End");
    strcpy(cluster.navName[kNavLeft], "Del");
    strcpy(cluster.navName[kNavRight], "Ins");
    strcpy(cluster.navName[kNavPagePrev], "PgDn");
    strcpy(cluster.navName[kNavPageNext], "PgUp");
    in = footerInput(&cluster, true);
    check(composeIs(in, fakeMeasure, kPx13,
                    "Home/End pick  Space select  PgDn/PgUp page  Esc close\nTab, arrows and Enter work too", got, sizeof(got)),
          "footer 11: long names drop `change` and fit", got);
    // 12: the tooltip's action line, from the footer input's names.
    in = footerInput(&stock, true);
    menuComposeTipAction(in.navName, false, got, sizeof(got));
    check(strcmp(got, "Space or A/D changes it.") == 0, "footer 12: the tooltip's toggle line", got);
    menuComposeTipAction(in.navName, true, got, sizeof(got));
    check(strcmp(got, "Space types a value; A/D steps it.") == 0, "footer 12: the tooltip's typed line", got);
    in = footerInput(&none, true);
    menuComposeTipAction(in.navName, false, got, sizeof(got));
    check(strcmp(got, "Enter or Left/Right changes it.") == 0, "footer 12: the fixed names give today's line", got);
    // With the page keys the menu's own (both slots on End/Home, say), the
    // status page's legend is the fixed "Tab page", and the shared-mode
    // off note still names the pick pair. Half a pair reads "Tab/E page".
    MenuAliasTable noPage = stock;
    strcpy(noPage.navName[kNavPagePrev], "Tab");
    strcpy(noPage.navName[kNavPageNext], "Tab");
    in = footerInput(&noPage, false);
    in.statusPage = true;
    in.pageName = "Monitor";
    check(composeIs(in, fakeMeasure, kPx13, "Tab page  Esc close\nkeys shared (Monitor)", got, sizeof(got)),
          "footer: with no page alias the status page names Tab", got);
    in = footerInput(&noPage, false);
    in.privateWanted = false;
    in.gatePrivate = false;
    check(composeIs(in, fakeMeasure, kPx13,
                    "Arrows pick/change  Enter select  Tab page  Esc close\nkeys shared (menu.keyboard)  W/S off",
                    got, sizeof(got)),
          "footer: with no page alias the shared-mode off note names the pick pair", got);
    MenuAliasTable halfPage = stock;
    strcpy(halfPage.navName[kNavPagePrev], "Tab");
    in = footerInput(&halfPage, false);
    in.statusPage = true;
    in.pageName = "Status";
    check(composeIs(in, fakeMeasure, kPx13, "Tab/E page  Esc close\nkeys shared (Status)", got, sizeof(got)),
          "footer: half an adopted page pair reads Tab/E", got);
    // At most one '\n', never over 159 bytes: the widest names, LIVE so
    // the legend carries them, with no ruler at all (nothing dropped) --
    // line 1 is 107 bytes and line 2 52, 160 with the break, so the cap
    // is actually reached and the string is cut at the buffer's edge.
    // (Not live, the legend is the 53-byte fixed one and the cap is 54
    // bytes away: that case pinned nothing.)
    MenuAliasTable wide = stock;
    for (int i = 0; i < kNavCount; ++i) strcpy(wide.navName[i], "Backspace");
    in = footerInput(&wide, true);
    in.privateWanted = false;
    in.widthPx = 0;
    menuComposeFooter(in, nullptr, nullptr, got, sizeof(got));
    int breaks = 0;
    for (const char* p = got; *p; ++p) breaks += *p == '\n';
    check(breaks == 1 && strlen(got) == 159 &&
              strstr(got, "Backspace/Backspace pick  Backspace/Backspace change  Backspace select  "
                          "Backspace/Backspace page  Esc close\nkeys shared (menu.keyboard)") == got,
          "footer: one line break and exactly 159 bytes at the cap", got);
}

void testFooterComposeDrops() {
    MenuAliasInput stockIn[kMenuUiElementCount];
    MenuAliasTable stock;
    menuAliasResolve(stockIn, stockInput(stockIn), fixedKeys("F8"), &stock);
    MenuAliasTable none;
    menuAliasResolve(nullptr, 0, fixedKeys("F8"), &none);
    char got[256];
    // 10: the narrow card. At 500 px of 13 px characters `change` goes
    // first, then `pick`; `page` and `close` survive whatever the width.
    MenuFooterInput in = footerInput(&stock, true);
    in.widthPx = 500;
    check(composeIs(in, fakeMeasure, kPx13, "Space select  Q/E page  Esc close\nTab, arrows and Enter work too", got, sizeof(got)),
          "drops: `change` then `pick` go at 500 px", got);
    in.widthPx = 400;
    check(composeIs(in, fakeMeasure, kPx13, "Q/E page  Esc close\nTab, arrows and Enter work too", got, sizeof(got)),
          "drops: then `select` at 400 px", got);
    in.widthPx = 100;
    check(composeIs(in, fakeMeasure, kPx13, "Q/E page  Esc close", got, sizeof(got)),
          "drops: `page` and `close` survive a width nothing fits, and the second line is dropped", got);
    in = footerInput(&none, true);
    in.widthPx = 500;
    check(composeIs(in, fakeMeasure, kPx13, "Enter select  Tab page  Esc close", got, sizeof(got)),
          "drops: the fixed legend loses `Arrows pick/change` first", got);
    // Editing drops `Type a value` first, then `Backspace deletes`.
    in = footerInput(&stock, false);
    in.editing = true;
    in.widthPx = 600;
    check(composeIs(in, fakeMeasure, kPx13, "Backspace deletes  Enter writes  Esc cancels", got, sizeof(got)),
          "drops: editing loses `Type a value` first", got);
    in.widthPx = 400;
    check(composeIs(in, fakeMeasure, kPx13, "Enter writes  Esc cancels", got, sizeof(got)),
          "drops: ...then `Backspace deletes`", got);
    // No ruler: nothing is dropped.
    in = footerInput(&stock, true);
    in.widthPx = 100;
    check(composeIs(in, nullptr, nullptr, "W/S pick  A/D change  Space select  Q/E page  Esc close\nTab, arrows and Enter work too",
                    got, sizeof(got)),
          "drops: without a ruler everything is kept", got);
}

// Every string of the design fits its line in the raster's own face --
// asked of GDI, the ruler the raster composes with -- at the three cap
// heights the design measured, and the old footer is pinned as NOT
// fitting, which is the premise of the whole change.
void testFooterFitsGdi() {
    struct Tier { int cap; int card; };
    const Tier tiers[3] = {{22, 600}, {30, 818}, {50, 1364}};
    const char* lines[] = {
        "Arrows pick/change  Enter select  Tab page  Esc close",
        "Arrows pick/change  Enter select  Q/E page  Esc close",
        "W/S pick  A/D change  Space select  Q/E page  Esc close",
        "Tab, arrows and Enter work too",
        "2 changes at next launch",
        "1 change at next launch",
        "KEYS SHARED WITH THE GAME",
        "Tab page  Esc close",
        "Q/E page  Esc close",
        "keys shared (Monitor)",
        "keys shared (Status)",
        "keys shared (menu.keyboard)  W/S off",
        "keys shared (menu.keyboard)",
        "Type a value  Backspace deletes  Enter writes  Esc cancels",
    };
    bool all = true;
    for (const Tier& t : tiers) {
        const int usable = t.card - 2 * (t.cap * 8 / 10);
        for (const char* s : lines) {
            const int px = menuPanelMeasureLine(s, t.cap);
            if (px <= 0 || px > usable) {
                all = false;
                printf("  fit: cap %d: \"%s\" is %d px in %d\n", t.cap, s, px, usable);
            }
        }
    }
    check(all, "fit: every line of the design fits its usable width at cap 22, 30 and 50");
    // The narrow tier: the card clamped at 1600 px with a cap of 80.
    const int narrow = 1600 - 2 * (80 * 8 / 10);
    check(menuPanelMeasureLine("Space select  Q/E page  Esc close", 80) <= narrow &&
              menuPanelMeasureLine("Enter select  Tab page  Esc close", 80) <= narrow,
          "fit: the narrow tier's results fit at cap 80");
    // The premise: today's footer does not fit anywhere.
    const char* baseline = "Up/Down pick   Left/Right change   Enter switch or type   Tab page   "
                           "PgUp/PgDn read on   R twice resets   Esc close";
    bool over = true;
    for (const Tier& t : tiers) {
        over = over && menuPanelMeasureLine(baseline, t.cap) > t.card - 2 * (t.cap * 8 / 10);
    }
    check(over, "fit: the 115-character footer this replaces overflows at every size (the premise, pinned)");
    check(menuPanelMeasureLine("", 30) == 0 && menuPanelMeasureLine("x", 0) == 0,
          "fit: nothing to measure is zero");
    // The composer against the real ruler: the default size keeps the
    // whole stock legend; the narrow tier drops to what the design says.
    MenuAliasInput stockIn[kMenuUiElementCount];
    MenuAliasTable stock;
    menuAliasResolve(stockIn, stockInput(stockIn), fixedKeys("F8"), &stock);
    char got[256];
    MenuFooterInput in = footerInput(&stock, true);
    in.widthPx = 818 - 2 * (30 * 8 / 10);
    check(composeIs(in, gdiMeasure, reinterpret_cast<void*>(static_cast<intptr_t>(30)),
                    "W/S pick  A/D change  Space select  Q/E page  Esc close\nTab, arrows and Enter work too",
                    got, sizeof(got)),
          "fit: composed against GDI at cap 30 the stock legend is whole", got);
    in.widthPx = narrow;
    check(composeIs(in, gdiMeasure, reinterpret_cast<void*>(static_cast<intptr_t>(80)),
                    "Space select  Q/E page  Esc close\nTab, arrows and Enter work too", got, sizeof(got)),
          "fit: composed against GDI at cap 80 in 1472 px it drops to select/page/close", got);
}

// The footer's box is always two lines tall: the panel's height is the
// same with and without a second footer line, so a pending count or a
// warning appearing never moves the rows under the head's aim, and the
// second line has room when it comes.
void testFooterTwoLines() {
    MenuContent c{};
    c.widthPx = 818;
    c.cardPx = 818;
    c.capPx = 30;
    c.tabCount = 2;
    strcpy(c.tabs[0], "Fixes");
    strcpy(c.tabs[1], "Status");
    c.lineCount = 3;
    for (int i = 0; i < 3; ++i) {
        snprintf(c.lines[i].left, sizeof(c.lines[i].left), "Row %d", i);
        c.lines[i].style = i == 1 ? kMenuRowHi : kMenuRow;
    }
    strcpy(c.footer, "W/S pick  A/D change  Space select  Q/E page  Esc close");
    int top1 = 0, bottom1 = 0, top2 = 0, bottom2 = 0, ops1 = 0, ops2 = 0;
    float rows1[6], rows2[6];
    const int h1 = menuPanelLayoutHeightForTest(c, &top1, &bottom1, rows1, 3, &ops1);
    strcat(c.footer, "\n2 changes at next launch");
    const int h2 = menuPanelLayoutHeightForTest(c, &top2, &bottom2, rows2, 3, &ops2);
    check(h1 > 0 && h1 == h2, "two lines: the height is identical with and without a second footer line");
    check(top1 == top2 && bottom1 == bottom2 && bottom1 - top1 == 2 * 30 * 16 / 10,
          "two lines: the footer's box is two lines tall either way");
    check(memcmp(rows1, rows2, sizeof(rows1)) == 0 && rows1[0] >= 0.0f && rows1[5] > rows1[4],
          "two lines: every row rectangle above the footer is identical");
    // The second line is DRAWN as a line of its own, not appended to the
    // legend and ellipsised with it: one single-line op per line present.
    // (Pins the split; with one op for both, the draw would be back to
    // DT_SINGLELINE and the '\n' would not break.)
    check(ops1 == 1 && ops2 == 2, "two lines: one single-line op per footer line present");
    // A toast has no footer box at all.
    c.toast = true;
    int topT = 0, bottomT = 0;
    menuPanelLayoutHeightForTest(c, &topT, &bottomT, nullptr, 0);
    check(topT == -1 && bottomT == -1, "two lines: a toast makes no footer box");
}

}  // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc == 2 && (strcmp(argv[1], "--worker-exit-child") == 0 ||
                      strcmp(argv[1], "--worker-stop-child") == 0)) {
        MenuContent content{};
        content.widthPx = content.cardPx = 256;
        content.capPx = 20;
        content.lineCount = 1;
        strcpy_s(content.lines[0].left, "Exit regression");
        menuPanelSubmit(content);
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (!menuPanelWorkerReadyForTest() && GetTickCount64() < deadline) Sleep(1);
        if (!menuPanelWorkerReadyForTest()) return 2;
        if (strcmp(argv[1], "--worker-stop-child") == 0) {
            menuPanelShutdown();
            menuPanelSubmit(content); // Re-start after a normal explicit stop.
            menuPanelShutdown();
        }
        // Intentionally omit shutdown in the exit case. Returning through the
        // actual CRT catches a joinable static thread destructor; _Exit would
        // bypass that destructor and make the regression test meaningless.
        return 0;
    }
    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    check(length > 0 && length < MAX_PATH, "worker exit: locate self");
    for (const wchar_t* mode : {L"--worker-exit-child", L"--worker-stop-child"}) {
        std::wstring command = L"\"" + std::wstring(executable) + L"\" " + mode;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const bool launched = CreateProcessW(executable, &command[0], nullptr, nullptr, FALSE,
                                             CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
        check(launched, "worker exit: launch isolated child");
        if (!launched) continue;
        const DWORD wait = WaitForSingleObject(process.hProcess, 10000);
        DWORD result = ~0u;
        if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &result);
        else { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 1000); }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        check(wait == WAIT_OBJECT_0 && result == 0, "worker exit/explicit stop: no abort or hang");
    }
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
    testAsymmetricProjection();
    testIniWrite();
    testPerfStats();
    testHotkeyRegisteredKeys();
    testKeyRepeatStep();
    testEditPrime();
    testAliasResolveStock();
    testAliasResolveSean();
    testAliasResolveAdversarial();
    testAliasResolvePure();
    testAliasMayAct();
    testKeyDisplayName();
    testFooterComposeExact();
    testFooterComposeDrops();
    testFooterFitsGdi();
    testFooterTwoLines();
    if (g_fails) {
        printf("MENU TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("MENU TEST PASSED\n");
    return 0;
}
