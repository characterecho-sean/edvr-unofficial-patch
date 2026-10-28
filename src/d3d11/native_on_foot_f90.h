// F90 (native on-foot VR): the weapon readout at the hip's size while aiming down sights (A) and the pistol's readout placed in the weapon's frame (B). Design:
// reviews\native-onfoot-f90-review-2026-10-24.md; journal: the arc doc, F90. Header only, no game or D3D11 state: native_frame_test runs these functions on the
// F85 two-eye rig, and f85bDecide (native_on_foot.cpp) calls them at each weapon UI draw.
//
// A. Every weapon UI part (a B10B quad of the marker composite) samples one texture, 3525 x 3133 texels and redrawn each frame, through its own UV rectangle
// (UV = unpack(PACKEDVERTEXDATAB) x b2 row 6 .xy x 16 + .zw). The game draws the readout into the rectangle of the part it shows: 29501 at the hip (rifle and
// pistol), 29500 in the rifle's ADS and, inferred from the rifle's ADS texture (no pistol ADS dump yet), 29498 in the pistol's. The content has one texel size
// in the texture, so its world size is set by the part's texels per metre: 29500 maps 16/9 as many per cm as 29501, and its digits are 9/16 of the hip's size
// (the ADS shrink seen since F85). Scaling the part by texels/m(part) / texels/m(29501) about its content centre gives them the hip's size at the game's own
// ADS place. The scale belongs to the part (29500's rectangle is empty at the hip in every frame of the F89 flight's hip dumps), so there is no ADS gate and
// no ramp.
//
// B. The pistol's hip readout (29501) sits over the rear sight, 13 cm behind mid-barrel and 4.7 cm nearer than the gun under it. With a pistol held it moves
// to T = T0 + toward unit(E - T0), T0 = C + R_w (forward, left, up): C the game's content centre, R_w the barrel, left and up of the FP right attach (joint 51)
// through F_root, E the deciding eye. Four TEMPORARY keys; all four 0 leave F85b's rows bit for bit. T is latched once a render frame by the first eye that
// decides 29501 and is reused by the other eye and by 29498 (scaled by A, then moved onto T, so the ADS readout keeps the hip spot and the hip size). A
// translation keeps the game's facing: U3 is untouched.
//
// F91 (reviews\native-onfoot-f91-review-2026-10-24.md, part A): the F90 flight applied nothing, because the table was keyed on the F89 session's part names and
// the game interns them per session. The parts are now known by their record's mesh and scale (partOf), the data F90 read as its guard; the names above are the
// F89 session's, kept as the parts' history. In the F90 session the same parts were 29510 (hip), 29526 (rifle ADS), 29524 (pistol ADS) and 29553 (tool).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "native_on_foot_core.h"   // f85::UFrame, f86::scaleUFrameAbout, barrelRow, ikUnit, f88::clampCm

namespace edvr {
namespace nofoot {
namespace f90 {

// ---- A: the parts -------------------------------------------------------------------------------------------------------------------------------------------
// F91: a part is known by its record's mesh (x and z extents) and scale, each within 1% of a row. The name ids F90 keyed on are interned per session (29501,
// 29500, 29498 and 29495 in the F89 flight, 29510, 29526, 29524 and 29553 in F90's) and the slots follow the first-submit order, so neither names a part; the
// mesh, the record's scale, the layout, b2 row 6 and the texture were bit-identical in both sessions. The four scales are at least 1.3x apart, so a match is
// unambiguous. The record scale is the layout's width x base / mesh x, and the UV span is width x 4008 texels, so texels/m = 4008 / base and A's factor is
// base(hip) / base(part): 1.7777779 and 0.5714286, which the UV-derived factor below matches within 1e-4.
enum Role : int { kRoleNone = 0, kRoleHip = 1, kRoleRifleAds = 2, kRolePistolAds = 3, kRoleTool = 4 };
constexpr double kTexSize[2] = {3525.0, 3133.0};   // the texture every part samples (u, v)
constexpr double kUvSpanPerUnit = 0.9995;           // the packed quad's UV extent per unit of |b2 row 6 .xy| (0.4098 / 0.41), the same for every part: it cancels in a ratio
constexpr double kMatchTol = 0.01;                  // the mesh's extents and the record's scale must be within 1% of a row
struct Part {
    int role;
    double mesh[2];     // the quad's extents in mesh units: x (the panel's right) and z (its up)
    double scale;       // the record's scale (word 1), as both flights measured it
    double base;        // the game's layout base scale (the submit sample's +0x54): the log, and the rig's check of the factor
    double uv[2];       // |b2 row 6 .xy|: the UV rectangle's size per unit of the packed span (the tool's was not dumped: 0)
    double centre[2];   // the content's centre in mesh units (x, z), from the F89 flight's dumps
};
constexpr int kPartCount = 4;
constexpr Part kParts[kPartCount] = {
    {kRoleHip, {2.0, 1.0}, 0.072070308, 0.400000006, {0.41, 0.230625}, {-0.19, 0.20}},
    {kRoleRifleAds, {3.0, 1.0}, 0.0500976555, 0.224999994, {0.76, 0.285}, {-0.96, 0.02}},
    {kRolePistolAds, {2.0, 1.0}, 0.166796863, 0.699999988, {0.542222, 0.305}, {-0.58, 0.02}},
    {kRoleTool, {2.0, 1.0}, 0.0939941406, 0.699999988, {0.0, 0.0}, {0.0, 0.0}},   // never scaled or moved: the row exists so the log names it
};
inline const Part* partOfRole(int role) noexcept {
    for (const Part& p : kParts)
        if (p.role == role) return &p;
    return nullptr;
}
inline const char* roleName(int role) noexcept {
    switch (role) {
        case kRoleHip: return "hip readout";
        case kRoleRifleAds: return "rifle ADS readout";
        case kRolePistolAds: return "pistol ADS readout";
        case kRoleTool: return "tool readout";
        default: return "no table part";
    }
}
inline bool scaledRole(int role) noexcept { return role == kRoleRifleAds || role == kRolePistolAds; }
inline bool within(double v, double want) noexcept { return std::isfinite(v) && std::fabs(v - want) <= kMatchTol * std::fabs(want); }
// The draw is a row's part: its icon mesh was read and its x and z extents and the record's scale are each within 1% of the row's.
inline bool tableMatches(const Part& p, bool meshRead, const double ext[2], double recScale) noexcept {
    return meshRead && within(ext[0], p.mesh[0]) && within(ext[1], p.mesh[1]) && within(recScale, p.scale);
}
// The unique row the record matches, else nullptr (none, or more than one).
inline const Part* partOf(bool meshRead, const double ext[2], double recScale) noexcept {
    const Part* found = nullptr;
    for (const Part& p : kParts)
        if (tableMatches(p, meshRead, ext, recScale)) {
            if (found) return nullptr;
            found = &p;
        }
    return found;
}
// Texels per metre along the panel's right (axis 0, u) or up (axis 1, v), at the row's record scale.
inline double texelsPerM(const Part& p, int axis) noexcept { return p.uv[axis] * kUvSpanPerUnit * kTexSize[axis] / (p.mesh[axis] * p.scale); }
// texels/m(role) / texels/m(hip) along one axis: the factor that gives the part's content the hip's world size (rifle ADS 1.77777, pistol ADS 0.57143, hip 1).
// NaN for the tool (its UV rectangle was not dumped) and for no role.
inline double texelRatio(int role, int axis) noexcept {
    const Part* p = partOfRole(role);
    const Part* hip = partOfRole(kRoleHip);
    if (!p || !hip || axis < 0 || axis > 1 || (role != kRoleHip && !scaledRole(role))) return std::nan("");
    return texelsPerM(*p, axis) / texelsPerM(*hip, axis);
}
// The factor A applies (the scale is one number): the mean of the two axes' ratios, which agree to 1e-5.
inline double sizeFactor(int role) noexcept { return 0.5 * (texelRatio(role, 0) + texelRatio(role, 1)); }
// A mesh point (x, 0, z) where the vertex shader puts it in the hud frame: x0rel + scale * q(v), the transform F86d's panel uses for its corners. Rot is
// drawtag::rotateByPackedQuat (passed in: draw_record_tag.h needs d3d11.h ahead of it, and this header does not).
template <class Rot>
inline void contentPoint(const double x0rel[3], uint32_t q2, uint32_t q3, double scale, const double cMesh[2], Rot rotate, double out[3]) noexcept {
    const double v[3] = {cMesh[0], 0.0, cMesh[1]};
    double rv[3];
    rotate(q2, q3, v, rv);
    for (int i = 0; i < 3; ++i) out[i] = x0rel[i] + scale * rv[i];
}

// ---- the map a decision applies -------------------------------------------------------------------------------------------------------------------------------
// P' = k P + (1 - k) c + t, in the composite frame's world (relative to the eye's rows' origin): A's scale about the content centre c, then B's move t. U3 is
// scaled by k and never turned, so the panel keeps the game's facing.
struct Map {
    bool scale = false, move = false;
    double c[3] = {}, k = 1.0, t[3] = {};
    bool identity() const noexcept { return !scale && !move; }
};
inline void mapPoint(const Map& m, const double p[3], double out[3]) noexcept {
    double q[3] = {p[0], p[1], p[2]};
    if (m.scale)
        for (int i = 0; i < 3; ++i) q[i] = m.k * q[i] + (1.0 - m.k) * m.c[i];
    if (m.move)
        for (int i = 0; i < 3; ++i) q[i] += m.t[i];
    std::memcpy(out, q, sizeof(q));
}
inline void applyMap(f85::UFrame& uf, const Map& m) noexcept {
    if (m.scale) f86::scaleUFrameAbout(uf, m.c, m.k);
    if (m.move)
        for (int a = 0; a < 3; ++a) uf.u4[a] += m.t[a];
}

// ---- B: the weapon frame ----------------------------------------------------------------------------------------------------------------------------------------
enum FrameSrc : int { kFrameNone = 0, kFrameAttach = 1, kFrameRoot = 2 };
struct WeaponFrame {
    int src = kFrameNone;
    double b[3] = {}, l[3] = {}, u[3] = {};   // the barrel (toward the muzzle), left and up, world, unit
    double barrelDeg = 0.0;                   // b against F_root's forward
};
// attachFresh, rootFresh: the FP snapshot and F_root are within kIkFreshMs. attach: the FP right attach (joint 51, snapshot slot kIkFpSlotAttach[1]), FP model
// space, row-vector; froot: F_root, row-vector, world. The attach's rows reach the world through F_root (row x F_root, as ikXform takes a point). b: the attach
// row most aligned with F_root's forward, signed (barrelRow, F69b's rule: within 45 degrees). u: of the two other rows the one most aligned with F_root's up,
// signed toward it and made orthogonal to b. l = b x u, signed so that l . (F_root's right) < 0. A stale snapshot, or no attach row within 45 degrees of the aim,
// takes F_root's own rows (forward, up, minus right); no fresh F_root: none.
inline WeaponFrame weaponFrame(bool attachFresh, const float attach[16], bool rootFresh, const float froot[16]) noexcept {
    WeaponFrame w;
    if (!rootFresh || !froot) return w;
    double R[3][3];
    for (int i = 0; i < 3; ++i) {
        const double r[3] = {froot[4 * i], froot[4 * i + 1], froot[4 * i + 2]};
        if (!ikUnit(r, R[i])) return w;
    }
    const auto finish = [&](const double b[3], const double upHint[3], int src) noexcept {
        WeaponFrame o;
        const double d = ikDot(upHint, b);
        const double u[3] = {upHint[0] - d * b[0], upHint[1] - d * b[1], upHint[2] - d * b[2]};
        double l[3];
        if (!ikUnit(u, o.u)) return o;
        ikCross(b, o.u, l);
        if (ikDot(l, R[0]) > 0.0)
            for (double& x : l) x = -x;
        if (!ikUnit(l, o.l)) return WeaponFrame{};
        std::memcpy(o.b, b, sizeof(o.b));
        const double c = ikDot(o.b, R[2]);
        o.barrelDeg = std::acos(c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c) * f85::kDeg;
        o.src = src;
        return o;
    };
    if (attachFresh && attach) {
        double rows[3][3];
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i) {
            double r[3];
            for (int c = 0; c < 3; ++c)
                r[c] = static_cast<double>(attach[4 * i]) * froot[c] + static_cast<double>(attach[4 * i + 1]) * froot[4 + c] +
                       static_cast<double>(attach[4 * i + 2]) * froot[8 + c];
            ok = ikUnit(r, rows[i]);
        }
        int axis = -1;
        double sign = 1.0;
        if (ok && barrelRow(rows, R[2], &axis, &sign)) {
            const double b[3] = {sign * rows[axis][0], sign * rows[axis][1], sign * rows[axis][2]};
            int best = -1;
            double bd = -1.0, bs = 1.0;
            for (int i = 0; i < 3; ++i) {
                if (i == axis) continue;
                const double d = ikDot(rows[i], R[1]);
                if (std::fabs(d) > bd) {
                    bd = std::fabs(d);
                    best = i;
                    bs = d < 0.0 ? -1.0 : 1.0;
                }
            }
            const double hint[3] = {bs * rows[best][0], bs * rows[best][1], bs * rows[best][2]};
            w = finish(b, hint, kFrameAttach);
            if (w.src == kFrameAttach) return w;
        }
    }
    w = finish(R[2], R[1], kFrameRoot);
    return w;
}

// ---- B: the keys and the target ---------------------------------------------------------------------------------------------------------------------------------
// fix.native_on_foot_pistol_readout_forward_cm, _left_cm, _up_cm, _toward_eye_cm (TEMPORARY, live): clamped to these ranges (f88::clampCm: anything not finite
// reads 0). F92: the built-in values are the pilot's from the F91 flight ("Pistol ui looks good with these offsets"): forward 13, left 3.5, up -3, toward eye 3
// (F90 started from the review's 13, 6, -3, 2). Keys{} stays all zero: a struct with no keys set; the config reader falls back to these.
constexpr double kForwardMinCm = -10.0, kForwardMaxCm = 30.0;
constexpr double kLeftMinCm = -15.0, kLeftMaxCm = 15.0;
constexpr double kUpMinCm = -10.0, kUpMaxCm = 10.0;
constexpr double kTowardMinCm = -5.0, kTowardMaxCm = 10.0;
constexpr double kForwardDefaultCm = 13.0, kLeftDefaultCm = 3.5, kUpDefaultCm = -3.0, kTowardDefaultCm = 3.0;
// F92: the rifle's ADS readout, the same placement in the primary weapon's frame: fix.native_on_foot_rifle_ads_readout_forward_cm, _left_cm, _up_cm,
// _toward_eye_cm (TEMPORARY, live), the pistol's ranges. All four 0 leave F91's rows bit for bit. The built-in values put the readout just clear of the receiver's
// left side (F91 flight, dump 092535205001, the right eye in ADS: the content sat on the receiver's top centre behind the scope, 0.3 cm left of the centre, with
// its lower line under the recess rim; the receiver was about 5.5 cm wide there and the content 2.3 x 1.7 cm): 5 cm left puts its right edge about 1 cm past the
// receiver's left edge, beside the gun and away from the sight line above it. F93a: the pilot's value from the F92 flight ("Rifle ADS ui looks good"): left 3.5.
constexpr double kRifleForwardDefaultCm = 0.0, kRifleLeftDefaultCm = 3.5, kRifleUpDefaultCm = 0.0, kRifleTowardDefaultCm = 0.0;
struct Keys {
    double forwardCm = 0.0, leftCm = 0.0, upCm = 0.0, towardCm = 0.0;
    bool any() const noexcept { return forwardCm != 0.0 || leftCm != 0.0 || upCm != 0.0 || towardCm != 0.0; }
};
// T = T0 + toward unit(E - T0), T0 = C + forward b + left l + up u (absolute world, metres). All keys 0: T = C exactly. False when a key needs what is missing:
// a weapon frame for forward, left or up, the eye for toward.
inline bool target(const double C[3], const WeaponFrame& w, const Keys& k, const double* eye, double T[3]) noexcept {
    if (!k.any()) {
        std::memcpy(T, C, 3 * sizeof(double));
        return true;
    }
    double T0[3] = {C[0], C[1], C[2]};
    if (k.forwardCm != 0.0 || k.leftCm != 0.0 || k.upCm != 0.0) {
        if (w.src == kFrameNone) return false;
        for (int i = 0; i < 3; ++i) T0[i] += 0.01 * (k.forwardCm * w.b[i] + k.leftCm * w.l[i] + k.upCm * w.u[i]);
    }
    if (k.towardCm != 0.0) {
        if (!eye) return false;
        const double d[3] = {eye[0] - T0[0], eye[1] - T0[1], eye[2] - T0[2]};
        double du[3];
        if (!ikUnit(d, du)) return false;
        for (int i = 0; i < 3; ++i) T0[i] += 0.01 * k.towardCm * du[i];
    }
    if (!std::isfinite(T0[0]) || !std::isfinite(T0[1]) || !std::isfinite(T0[2])) return false;
    std::memcpy(T, T0, sizeof(T0));
    return true;
}

// ---- the decision ----------------------------------------------------------------------------------------------------------------------------------------------
// The held item, for the placements: none of their classes (or nothing held: no placement), the held pistol, the held primary weapon (F92: `$wpn_m_`), or a
// name the decision cannot trust (its serial is not the held item's, or the item could not be read): refused and counted.
enum Item : int { kItemOther = 0, kItemPistol = 1, kItemNotHeld = 2, kItemPrimary = 3 };
// A: no table part (F91: the record matches no row: not scaled, not moved, counted), the hip readout, a scaled ADS readout, the tool's readout (left as it is).
enum AState : int { kANotTable = 0, kAHip = 1, kAScaled = 2, kATool = 3 };
// F92: kBRefClass: a part of another class (the other weapon's ADS readout, a tool's readout) was drawn beside this class's (F93a: two classes' parts in the newest
// frame, DrawnTracker's mixed). The hip readout is one part for the rifle and the pistol, so without this the pistol placement moved the rifle's hip readout through
// a pistol-to-rifle switch. kBRefItem (F93a): Status.json names the other weapon class while the drawn class has stood longer than kClassGraceS.
enum BState : int { kBNone = 0, kBMoved, kBMovedPrevious, kBRefItem, kBRefFrame, kBRefEye, kBRefNoTarget, kBRefClass, kBCount };
// A placement's target, latched once a render frame by its first decision (either eye), for one held item.
struct LatchT {
    bool have = false;
    uint32_t frame = 0, serial = 0;
    double T[3] = {}, C[3] = {};   // absolute world
    WeaponFrame wf;                // F144: the weapon frame T was taken in (the pistol's ADS role takes its own target from the hip's C in it)
};
// The pistol's latch (its own fields, as F90 had them) and the rifle's (F92). F144: the shared hip readout while the rifle class is drawn (rifleHip) and the pistol's ADS
// role when its keys differ from the hip's (ads, keyed by the hip latch's frame).
struct Latch : LatchT {
    LatchT rifle;
    LatchT rifleHip;
    LatchT ads;
};
// ---- F93a: the class the game is drawing --------------------------------------------------------------------------------------------------------------------------
// Each class draws a readout part only it has, every frame its item is out (the rifle's ADS readout at the hip too, the pistol's ADS readout at the hip too, the
// tool's readout). Status.json names a switch 0.1 to 1.0 s after the game starts drawing the next item (F91 and F92 flights; F92's rifle-to-pistol swap: the
// pistol's ADS part first decided at 10:29:41.057, Status.json at 10:29:42.072, the first placed frame 10:29:42.082), so the class comes from those parts: the
// class whose own part was decided most recently, within this render frame or the last, is what the game draws; two classes' parts in the newest of those frames
// is a switch in progress (mixed). F92a's guard read the parts too, but placed only once Status.json agreed.
enum DrawnClass : int { kDrawnNone = 0, kDrawnPrimary = 1, kDrawnPistol = 2, kDrawnTool = 3, kDrawnMixed = 4 };
inline const char* drawnName(int c) noexcept {
    return c == kDrawnPrimary ? "primary" : c == kDrawnPistol ? "pistol" : c == kDrawnTool ? "tool" : c == kDrawnMixed ? "mixed" : "none";
}
inline int drawnClassOfRole(int role) noexcept {
    return role == kRoleRifleAds ? kDrawnPrimary : role == kRolePistolAds ? kDrawnPistol : role == kRoleTool ? kDrawnTool : kDrawnNone;
}
// Status.json may still veto a placement once the drawn class has stood this long and it names the other weapon class (a part shared by a weapon this table
// does not know): longer than any lag seen (1.0 s).
constexpr double kClassGraceS = 3.0;
struct DrawnTracker {
    uint32_t last[4] = {~0u, ~0u, ~0u, ~0u};   // [DrawnClass] the last render frame a part of that class was decided; ~0u never
    // A decision of a class's own part (the caller sees it before deciding, so the draw counts for itself).
    void see(int role, uint32_t frame) noexcept {
        const int c = drawnClassOfRole(role);
        if (c != kDrawnNone) last[c] = frame;
    }
    // True when the class's part was decided this frame or the last.
    bool recent(int c, uint32_t frame) const noexcept { return last[c] != ~0u && frame - last[c] <= 1u; }
    int classAt(uint32_t frame) const noexcept {
        uint32_t newest = 0;
        bool any = false;
        for (int c = kDrawnPrimary; c <= kDrawnTool; ++c)
            if (recent(c, frame) && (!any || int32_t(last[c] - newest) > 0)) {
                newest = last[c];
                any = true;
            }
        if (!any) return kDrawnNone;
        int found = kDrawnNone, n = 0;
        for (int c = kDrawnPrimary; c <= kDrawnTool; ++c)
            if (recent(c, frame) && last[c] == newest) {
                found = c;
                ++n;
            }
        return n > 1 ? kDrawnMixed : found;
    }
};
// F93a: one swap, for the log: from the first frame a weapon class's own part is drawn after two frames without it (the first draw frame) to the first frame a
// placement of that class is applied (the first placed frame).
struct SwapWatch {
    bool open = false;
    int from = kDrawnNone, to = kDrawnNone;
    uint32_t firstDraw = 0;
    // Before a class part's decision: `wasRecent` its class had a part this frame or the last; `previous` the drawn class before this draw. A weapon class's
    // part appearing opens a swap to it.
    void onPart(int cls, bool wasRecent, int previous, uint32_t frame) noexcept {
        if (wasRecent || (cls != kDrawnPrimary && cls != kDrawnPistol)) return;
        open = true;
        from = previous;
        to = cls;
        firstDraw = frame;
    }
    // After a decision that placed a part of class `cls`: true, with the gap in frames, when that closes the open swap.
    bool onPlaced(int cls, uint32_t frame, uint32_t* gap) noexcept {
        if (!open || cls != to) return false;
        open = false;
        if (gap) *gap = frame - firstDraw;
        return true;
    }
};
inline int placementClassOfRole(int role) noexcept { return role == kRoleRifleAds ? kDrawnPrimary : role == kRoleHip || role == kRolePistolAds ? kDrawnPistol : kDrawnNone; }
struct In {
    uint32_t frame = 0, name = 0, itemSerial = 0;   // name: the part's name id this session, for the lines only (F91: never an identity); itemSerial (F93a):
                                                    // the drawn class's epoch, the latch's key
    int item = kItemOther;                          // Status.json's held item: its class, for the steady-state veto only (F93a)
    int drawnClass = kDrawnNone;                    // F93a: the class the game is drawing (DrawnTracker), which decides the placements
    double drawnClassAgeS = 0.0;                    // F93a: how long it has been that class
    bool meshRead = false;
    double meshExt[2] = {};   // the icon mesh's x and z extents (mesh units)
    double recScale = 0.0;
    uint32_t q2 = 0, q3 = 0;  // the record's packed quaternion
    double x0rel[3] = {};     // the record's translation less o_hud
    double ow[3] = {};        // the eye's rows' origin (absolute)
    bool eyeOk = false;
    double eye[3] = {};       // the eye's origin from its scene rows (absolute)
    Keys keys;                // the pistol readout's (F144: the pistol class's hip role)
    Keys rifleKeys;           // F92: the rifle's ADS readout's
    // F144: one placement per role. With roleKeys false (every build before F144, and the checks that predate it) the decision is as it was: the pistol's ADS readout takes
    // the hip's target, and the shared hip readout is not placed while the rifle class is drawn. With roleKeys true the pistol's ADS role takes adsKeys (the target taken with
    // them from the hip part's content centre; adsKeys equal to keys is the hip's target itself) and the hip readout while the rifle class is drawn takes rifleHipKeys (all
    // four 0: placed where the game draws it, no move).
    bool roleKeys = false;
    Keys adsKeys;             // F144: the pistol's ADS role
    Keys rifleHipKeys;        // F144: the hip role while the rifle class is drawn
    double adsW = 0.0;        // the ADS weight at the draw, for the line only: the decision never reads it
};
inline bool sameKeys(const Keys& a, const Keys& b) noexcept {
    return a.forwardCm == b.forwardCm && a.leftCm == b.leftCm && a.upCm == b.upCm && a.towardCm == b.towardCm;
}
struct Out {
    int role = kRoleNone;
    int a = kANotTable, b = kBNone;
    double k = 1.0;
    Map map;
    bool haveC = false;
    double C[3] = {}, shown[3] = {};   // the content centre before and after the map (absolute world)
    bool frameComputed = false, latched = false;   // this call computed the weapon frame (wf) and put T into the latch
    WeaponFrame wf;
    bool haveT = false;
    double T[3] = {}, shiftM = 0.0;    // B's target and, when this call latched it, |T - C|
    // F144: the class whose placement was applied or refused for this part (kDrawnPrimary, kDrawnPistol, else none), the content centre in the record's vertex space (the
    // proof inverts the patched rows with it), and the weapon frame the target was taken in (the move is measured along its axes).
    int cls = kDrawnNone;
    double cv[3] = {};
    bool haveWf = false;
    WeaponFrame wfUsed;
};
// One weapon UI draw: A's scale and B's move for the part the record's mesh and scale name (F91: partOf; the name id is not read), given the eye's getter
// frame uf (P - o_world = U3 v + u4). frameFn() returns the weapon frame; it is called only when this draw latches T. The caller applies o.map to uf
// (applyMap) and to the proof's target (mapPoint).
template <class Rot, class FrameFn>
inline Out decide(const In& in, const f85::UFrame& uf, Latch& latch, Rot rotate, FrameFn frameFn) noexcept {
    Out o;
    const Part* p = partOf(in.meshRead, in.meshExt, in.recScale);
    if (!p) return o;   // no table part: not scaled, not moved (counted by the caller)
    o.role = p->role;
    if (p->role == kRoleTool) {   // named for the log, left as the game draws it
        o.a = kATool;
        return o;
    }
    // the placements: the pistol's for the hip readout and the pistol's ADS readout, the rifle's (F92) for the rifle's ADS readout, each only while the game draws
    // its class (F93a: the drawn class, not Status.json's)
    const bool pistolPart = p->role == kRoleHip || p->role == kRolePistolAds;
    const bool riflePart = p->role == kRoleRifleAds;
    const int wantClass = pistolPart ? kDrawnPistol : kDrawnPrimary;
    const int otherItem = pistolPart ? kItemPrimary : kItemPistol;
    double c[3], Crel[3];
    contentPoint(in.x0rel, in.q2, in.q3, in.recScale, p->centre, rotate, c);
    f85::mulVec3(uf.U3, c, Crel);
    for (int i = 0; i < 3; ++i) {
        Crel[i] += uf.u4[i];
        o.C[i] = Crel[i] + in.ow[i];
    }
    o.haveC = true;
    for (int i = 0; i < 3; ++i) o.cv[i] = c[i];
    if (p->role != kRoleHip) {
        o.a = kAScaled;
        o.k = sizeFactor(p->role);
        o.map.scale = true;
        o.map.k = o.k;
        std::memcpy(o.map.c, Crel, sizeof(Crel));
    } else {
        o.a = kAHip;
    }
    // A part's target in the weapon's frame, latched once a render frame: the first decision of the frame (either eye) reads the weapon frame and latches T, the
    // other eye reuses it. All keys 0: no move at all, so the rows are F85b's (the rifle's ADS readout: F91's) bit for bit in both eyes.
    const auto place = [&](LatchT& L, const Keys& keys, int cls) {
        o.cls = cls;
        if (L.have && L.serial == in.itemSerial && L.frame == in.frame) {
            std::memcpy(o.T, L.T, sizeof(o.T));
            o.haveT = true;
            o.wfUsed = L.wf;
            o.haveWf = L.wf.src != kFrameNone;
        } else {
            o.wf = frameFn();
            o.frameComputed = true;
            if (target(o.C, o.wf, keys, in.eyeOk ? in.eye : nullptr, o.T)) {
                L.have = true;
                L.frame = in.frame;
                L.serial = in.itemSerial;
                std::memcpy(L.T, o.T, sizeof(L.T));
                std::memcpy(L.C, o.C, sizeof(L.C));
                L.wf = o.wf;
                o.wfUsed = o.wf;
                o.haveWf = o.wf.src != kFrameNone;
                o.latched = true;
                o.haveT = true;
                const double d[3] = {o.T[0] - o.C[0], o.T[1] - o.C[1], o.T[2] - o.C[2]};
                o.shiftM = ikLen(d);
            } else {
                const bool needFrame = keys.forwardCm != 0.0 || keys.leftCm != 0.0 || keys.upCm != 0.0;
                o.b = needFrame && o.wf.src == kFrameNone ? kBRefFrame : kBRefEye;
            }
        }
        if (o.haveT) {
            o.b = kBMoved;
            if (keys.any()) {
                o.map.move = true;
                for (int i = 0; i < 3; ++i) o.map.t[i] = o.T[i] - o.C[i];
            }
        }
    };
    if (!pistolPart && !riflePart) {
        // no placement for this role
    } else if (in.drawnClass == kDrawnMixed) {
        o.b = kBRefClass;   // two classes' parts in the newest frame: the game is drawing both items (a switch); the shared hip readout could be either's
    } else if (in.roleKeys && p->role == kRoleHip && in.drawnClass == kDrawnPrimary) {
        // F144: the shared hip readout while the rifle class is drawn (it was never placed: "matched only"): its own placement; all four keys 0 draw it where the game does
        if (in.item == kItemPistol && in.drawnClassAgeS > kClassGraceS) o.b = kBRefItem;
        else place(latch.rifleHip, in.rifleHipKeys, kDrawnPrimary);
    } else if (in.drawnClass != wantClass) {
        // the game draws another class (or none): no placement for this part
    } else if (in.item == otherItem && in.drawnClassAgeS > kClassGraceS) {
        o.b = kBRefItem;    // Status.json names the other weapon class long after any switch: a weapon this table does not know shares the part
    } else if (riflePart) {
        place(latch.rifle, in.rifleKeys, kDrawnPrimary);
    } else {
        const bool sameItem = latch.have && latch.serial == in.itemSerial;
        if (p->role == kRoleHip) {
            place(latch, in.keys, kDrawnPistol);
        } else {   // the pistol's ADS readout: this frame's T, else the previous frame's (counted), else none
            o.cls = kDrawnPistol;
            if (sameItem && (latch.frame == in.frame || latch.frame + 1u == in.frame)) {
                bool ok = true;
                o.wfUsed = latch.wf;
                o.haveWf = latch.wf.src != kFrameNone;
                if (in.roleKeys && !sameKeys(in.adsKeys, in.keys)) {
                    // F144: its own keys, taken from the hip part's content centre in the hip's weapon frame, once for the hip latch's frame (both eyes share it)
                    if (latch.ads.have && latch.ads.serial == in.itemSerial && latch.ads.frame == latch.frame) {
                        std::memcpy(o.T, latch.ads.T, sizeof(o.T));
                    } else if (target(latch.C, latch.wf, in.adsKeys, in.eyeOk ? in.eye : nullptr, o.T)) {
                        latch.ads.have = true;
                        latch.ads.frame = latch.frame;
                        latch.ads.serial = in.itemSerial;
                        std::memcpy(latch.ads.T, o.T, sizeof(o.T));
                        std::memcpy(latch.ads.C, latch.C, sizeof(latch.ads.C));
                    } else {
                        ok = false;
                        const bool needFrame = in.adsKeys.forwardCm != 0.0 || in.adsKeys.leftCm != 0.0 || in.adsKeys.upCm != 0.0;
                        o.b = needFrame && latch.wf.src == kFrameNone ? kBRefFrame : kBRefEye;
                    }
                } else {
                    std::memcpy(o.T, latch.T, sizeof(o.T));
                }
                if (ok) {
                    o.haveT = true;
                    o.b = latch.frame == in.frame ? kBMoved : kBMovedPrevious;
                    o.map.move = true;
                    for (int i = 0; i < 3; ++i) o.map.t[i] = o.T[i] - o.C[i];
                }
            } else {
                o.b = kBRefNoTarget;
            }
        }
    }
    double shownRel[3];
    mapPoint(o.map, Crel, shownRel);
    for (int i = 0; i < 3; ++i) o.shown[i] = shownRel[i] + in.ow[i];
    return o;
}

// ---- F91: the role line, once per (held item, slot) ---------------------------------------------------------------------------------------------------------------
// A slot's role is named at its first decision after an item change and whenever it changes (F92: the role or what the decision applied); a state that keeps
// changing for one item is named at most kRoleLinesPerItem times (the rest are counted), so a part whose mesh is read on some frames only cannot flood the log.
constexpr uint32_t kRoleLinesPerItem = 8;
struct RoleLog {
    bool have = false;
    uint32_t serial = 0, lines = 0, suppressed = 0;
    int state = kRoleNone;
    // True when this decision's (serial, state) is to be logged; the state moves on either way.
    bool step(uint32_t heldSerial, int state_) noexcept {
        const bool newItem = !have || heldSerial != serial;
        if (!newItem && state_ == state) return false;
        if (newItem) {
            lines = 0;
            suppressed = 0;
        }
        have = true;
        serial = heldSerial;
        state = state_;
        if (lines >= kRoleLinesPerItem) {
            ++suppressed;
            return false;
        }
        ++lines;
        return true;
    }
};
// F92: the role line's state: the role, whether it was scaled and what its placement did (a move from the previous frame's T counts as a move).
inline int appliedState(const Out& o) noexcept { return o.role * 32 + (o.a == kAScaled ? 16 : 0) + (o.b == kBMovedPrevious ? int(kBMoved) : o.b); }
inline const char* refusalName(int b) noexcept {
    switch (b) {
        case kBRefItem: return "Status.json names the other weapon class";
        case kBRefFrame: return "no weapon frame";
        case kBRefEye: return "no eye origin";
        case kBRefNoTarget: return "no target this frame or the last";
        case kBRefClass: return "another class's part is drawn: a switch";
        default: return "";
    }
}
// F92: the role line's text after "->": what was applied to the part and what was only matched. "applied: scaled, rifle placement", "matched only (no placement
// for the held item)", "applied: scaled; pistol placement refused (another class's part is drawn: a switch)"; the tool and no table part as in F91.
inline void roleText(const Out& o, char* buf, size_t cap) noexcept {
    if (!buf || !cap) return;
    if (o.role == kRoleNone) {
        std::snprintf(buf, cap, "NO TABLE PART: not scaled, not moved");
        return;
    }
    if (o.role == kRoleTool) {
        std::snprintf(buf, cap, "tool readout (not scaled)");
        return;
    }
    const bool scaled = o.a == kAScaled, placed = o.b == kBMoved || o.b == kBMovedPrevious, refused = o.b >= kBRefItem && o.b < kBCount;
    const char* kind = o.role == kRoleRifleAds || o.cls == kDrawnPrimary ? "rifle placement" : "pistol placement";   // F144: the hip readout placed while the rifle class is drawn is the rifle's
    const double k = o.role == kRoleHip ? 1.0 : sizeFactor(o.role);
    char head[96];
    if (scaled || placed)
        std::snprintf(head, sizeof(head), "applied: %s%s%s", scaled ? "scaled" : "", scaled && placed ? ", " : "", placed ? kind : "");
    else
        std::snprintf(head, sizeof(head), "matched only%s", refused ? "" : " (no placement for the held item)");
    if (refused)
        std::snprintf(buf, cap, "%s (x%.4f): %s; %s refused (%s)", roleName(o.role), k, head, kind, refusalName(o.b));
    else
        std::snprintf(buf, cap, "%s (x%.4f): %s", roleName(o.role), k, head);
}

}  // namespace f90
}  // namespace nofoot
}  // namespace edvr
