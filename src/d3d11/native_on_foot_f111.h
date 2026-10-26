// F111 (native on-foot VR): the panel pointer instrument's pure parts. Header only, no game or D3D11 state, so native_frame_test runs the very functions
// the hooks and the frame thread use. Journal: the arc doc, 2026-10-25, F111. RE: analysis\decomp\verify_20261024_onfoot_cursor_pick.txt (build 334925).
//
// The game's pointer on an on-foot panel, as read in build 334925:
//   the controller's update (vtable +0x08) reads the mouse point into +0x84/+0x88 (NDC, x right, y DOWN; +0x80 picks it or the other point at +0x8C),
//   unprojects (x, -y, 1, 1) through the camera's projection with the camera's pose replaced by the identity at the origin (x right, y up, z forward),
//   and stores the normalised direction at +0xB0; it then resets the sprite (+0x100 = 0, rows +0x110, position +0x140 = the direction).
//   The placement (vtable +0x10) runs once per panel: in ray mode the ray from the origin along +0xB0 against the panel's triangles (panel vtable +0x178;
//   t < 0 is rejected), the nearest hit posing the sprite (+0x100 = 1, +0x140 = the hit); in the 2D mode the NDC through the panel's cursor window
//   (panel vtable +0x198) to a uv, then to a point on the panel.
// The instrument reads each stage per controller update and correlates their changes from one update to the next, so one flight says which stage turns a
// mouse move the other way: the mouse NDC against the OS cursor (K4), the direction against the NDC (K1), the direction behind the camera (K5: no hit, the
// default sprite on the far side, which a perspective divide shows point-reflected), the hit's uv or the sprite against the direction (K2), the 2D window's
// uv against the NDC (K3).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f111 {

// ---- the build's addresses (334925) ----
constexpr uintptr_t kUpdateRva = 0x454F520, kUpdateSlotRva = 0x55DC7F0;     // the controller's update (its vtable +0x08)
constexpr uintptr_t kPlaceRva = 0x4559C50, kPlaceSlotRva = 0x55DC7F8;       // the controller's placement on one panel (its vtable +0x10)
constexpr uintptr_t kRayHitRva = 0x452B6A0, kRayHitSlotRva = 0x55DC6C0;     // the panel's ray against its triangles (panel vtable +0x178)
constexpr uintptr_t kWindowRva = 0x4541A20, kWindowSlotRva = 0x55DC6E0;     // the panel's cursor window, the 2D mode (panel vtable +0x198)
constexpr size_t kStolen = 5;
// push rsi; push rdi; push r12; sub rsp,0xD0; movaps [rsp+0x90],xmm8
constexpr uint8_t kUpdatePrologue[21] = {0x40, 0x56, 0x57, 0x41, 0x54, 0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00,
                                         0x44, 0x0F, 0x29, 0x84, 0x24, 0x90, 0x00, 0x00, 0x00};
// mov r11,rsp; push rbp; push rsi; push rdi; push r15; lea rbp,[r11-0xE8]; sub rsp,0x1C8
constexpr uint8_t kPlacePrologue[22] = {0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x57, 0x41, 0x57, 0x49, 0x8D, 0xAB, 0x18, 0xFF, 0xFF, 0xFF,
                                        0x48, 0x81, 0xEC, 0xC8, 0x01, 0x00, 0x00};
// mov [rsp+0x10],rbx; push rbp; push rsi; push rdi; push r12; push r13; push r14; push r15
constexpr uint8_t kRayHitPrologue[16] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
// mov [rsp+0x10],rbx; mov [rsp+0x18],rsi; push rdi; sub rsp,0x50; mov rbx,rcx; movaps [rsp+0x40],xmm6
constexpr uint8_t kWindowPrologue[23] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x50,
                                         0x48, 0x8B, 0xD9, 0x0F, 0x29, 0x74, 0x24, 0x40};
// The controller's fields.
constexpr uint32_t kOffSource = 0x80, kOffMouse = 0x84, kOffOther = 0x8C, kOffCamObj = 0xA0, kOffDir = 0xB0, kOffHit = 0x100, kOffPos = 0x140,
                   kOffMovie = 0x1B4;
// The camera: (*[ctl+0xA0])->vtable+0x58() -> vtable+0x30(); rows at +0x00, +0x10, +0x20, position at +0x30.
constexpr uint32_t kCamGetter1 = 0x58, kCamGetter2 = 0x30;

// ---- the statistics ----
constexpr uint32_t kMinPairs = 3;      // a correlation's sign needs three moves
constexpr double kSignR = 0.3;         // |r| under this is no sign
constexpr float kMoveEps = 1e-5f;      // an NDC change smaller than this is not a move
constexpr uint32_t kMinMoves = 3;      // a verdict needs three moves

struct Corr {
    double sxy = 0.0, sxx = 0.0, syy = 0.0;
    uint32_t n = 0;
    void add(double x, double y) noexcept {
        sxy += x * y;
        sxx += x * x;
        syy += y * y;
        ++n;
    }
    // Through the origin (the changes are zero-mean by construction of a back-and-forth move); 0 with too few pairs or no spread.
    double r() const noexcept { return n >= kMinPairs && sxx > 0.0 && syy > 0.0 ? sxy / std::sqrt(sxx * syy) : 0.0; }
    int sign() const noexcept {
        const double v = r();
        return v >= kSignR ? 1 : v <= -kSignR ? -1 : 0;
    }
    void merge(const Corr& o) noexcept {
        sxy += o.sxy;
        sxx += o.sxx;
        syy += o.syy;
        n += o.n;
    }
};

// One controller update and the placements after it, closed when the next update starts (the sprite as the placements left it).
struct Cycle {
    bool open = false;
    float ndc[2] = {};       // the point the ray was made from: x right, y down
    uint8_t source = 0;      // 1 the mouse point (+0x84), 0 the other (+0x8C)
    float dir[3] = {};       // +0xB0: x right, y up, z forward in the pick's camera frame
    uint32_t placements = 0, rayCalls = 0, rayHits = 0, windowCalls = 0;
    float t = 0.0f, u = 0.0f, v = 0.0f;
    bool hitUv = false;      // the nearest accepted ray hit's t and uv
    float win[2] = {};
    bool winUv = false;      // the 2D mode's window output
    bool hit = false;        // +0x100 when the cycle closed
    float pos[3] = {};       // +0x140 when the cycle closed
    uint32_t movie = 0;      // +0x1B4's change over the cycle
    int32_t os[2] = {};
    bool osOk = false;       // the OS cursor (GetCursorPos), x right, y down
};

// The uv a cycle sent: the nearest ray hit's, else the window's.
inline bool cycleUv(const Cycle& c, float* u, float* v) noexcept {
    if (c.hitUv) {
        *u = c.u;
        *v = c.v;
        return true;
    }
    if (c.winUv) {
        *u = c.win[0];
        *v = c.win[1];
        return true;
    }
    return false;
}

struct Sec {
    uint32_t cycles = 0, moves = 0, placements = 0, rayCalls = 0, rayHits = 0, windowCalls = 0, hitCycles = 0, behind = 0, spriteBehind = 0, movie = 0,
             osMoves = 0, tN = 0;
    double tSum = 0.0;
    // [0] x/right, [1] y/up (v and the OS y are down, against the NDC's y, which is down too)
    Corr ndcDir[2], dirSprite[2], ndcSeen[2], ndcUv[2], osNdc[2];
    double kNum[2] = {}, kDen[2] = {};   // the unprojection's slope: d(dir/dir.z) per NDC unit, right and up
    void merge(const Sec& o) noexcept {
        cycles += o.cycles;
        moves += o.moves;
        placements += o.placements;
        rayCalls += o.rayCalls;
        rayHits += o.rayHits;
        windowCalls += o.windowCalls;
        hitCycles += o.hitCycles;
        behind += o.behind;
        spriteBehind += o.spriteBehind;
        movie += o.movie;
        osMoves += o.osMoves;
        tN += o.tN;
        tSum += o.tSum;
        for (int i = 0; i < 2; ++i) {
            ndcDir[i].merge(o.ndcDir[i]);
            dirSprite[i].merge(o.dirSprite[i]);
            ndcSeen[i].merge(o.ndcSeen[i]);
            ndcUv[i].merge(o.ndcUv[i]);
            osNdc[i].merge(o.osNdc[i]);
            kNum[i] += o.kNum[i];
            kDen[i] += o.kDen[i];
        }
    }
    double slope(int i) const noexcept { return kDen[i] > 0.0 ? kNum[i] / kDen[i] : 0.0; }
};

inline void addCycle(Sec& s, const Cycle& c) noexcept {
    ++s.cycles;
    s.placements += c.placements;
    s.rayCalls += c.rayCalls;
    s.rayHits += c.rayHits;
    s.windowCalls += c.windowCalls;
    if (c.hit) ++s.hitCycles;
    if (c.dir[2] < 0.0f) ++s.behind;
    if (c.pos[2] < 0.0f) ++s.spriteBehind;
    s.movie += c.movie;
    if (c.hitUv) {
        s.tSum += c.t;
        ++s.tN;
    }
}

inline bool moved(const Cycle& a, const Cycle& b) noexcept {
    return std::fabs(b.ndc[0] - a.ndc[0]) > kMoveEps || std::fabs(b.ndc[1] - a.ndc[1]) > kMoveEps;
}

// The step from one closed cycle to the next of the same controller: every stage's change against the NDC's (or the direction's).
inline void addStep(Sec& s, const Cycle& a, const Cycle& b) noexcept {
    if (!moved(a, b)) return;
    ++s.moves;
    const double dx = double(b.ndc[0]) - a.ndc[0], dyDown = double(b.ndc[1]) - a.ndc[1], dyUp = -dyDown;
    s.ndcDir[0].add(dx, double(b.dir[0]) - a.dir[0]);
    s.ndcDir[1].add(dyUp, double(b.dir[1]) - a.dir[1]);
    if (a.dir[2] != 0.0f && b.dir[2] != 0.0f) {
        const double qx = double(b.dir[0]) / b.dir[2] - double(a.dir[0]) / a.dir[2], qy = double(b.dir[1]) / b.dir[2] - double(a.dir[1]) / a.dir[2];
        s.kNum[0] += dx * qx;
        s.kDen[0] += dx * dx;
        s.kNum[1] += dyUp * qy;
        s.kDen[1] += dyUp * dyUp;
    }
    if (a.hit && b.hit && a.hitUv && b.hitUv) {   // the sprite at the hit: it moves as the direction does unless the hit's frame turns it
        s.dirSprite[0].add(double(b.dir[0]) - a.dir[0], double(b.pos[0]) - a.pos[0]);
        s.dirSprite[1].add(double(b.dir[1]) - a.dir[1], double(b.pos[1]) - a.pos[1]);
    }
    if (a.pos[2] != 0.0f && b.pos[2] != 0.0f) {   // the sprite as the pick's camera sees it (its perspective divide; z < 0 reflects it)
        s.ndcSeen[0].add(dx, double(b.pos[0]) / b.pos[2] - double(a.pos[0]) / a.pos[2]);
        s.ndcSeen[1].add(dyUp, double(b.pos[1]) / b.pos[2] - double(a.pos[1]) / a.pos[2]);
    }
    float ua = 0.0f, va = 0.0f, ub = 0.0f, vb = 0.0f;
    if (cycleUv(a, &ua, &va) && cycleUv(b, &ub, &vb)) {
        s.ndcUv[0].add(dx, double(ub) - ua);
        s.ndcUv[1].add(dyDown, double(vb) - va);   // v grows downward, as the NDC's y does
    }
    if (a.osOk && b.osOk && (a.os[0] != b.os[0] || a.os[1] != b.os[1])) {
        ++s.osMoves;
        s.osNdc[0].add(double(b.os[0]) - a.os[0], dx);
        s.osNdc[1].add(double(b.os[1]) - a.os[1], dyDown);   // both grow downward
    }
}

enum class Stage : uint8_t { kFewMoves = 0, kMouse, kUnproject, kBehind, kHit, kWindow, kNone };

// The first stage, in the pick's order, that turns a move the other way.
inline Stage verdict(const Sec& s) noexcept {
    if (s.moves < kMinMoves) return Stage::kFewMoves;
    if (s.osNdc[0].sign() < 0 || s.osNdc[1].sign() < 0) return Stage::kMouse;
    if (s.ndcDir[0].sign() < 0 || s.ndcDir[1].sign() < 0) return Stage::kUnproject;
    if (s.behind * 2 > s.cycles) return Stage::kBehind;
    if (s.rayHits > 0 && (s.ndcUv[0].sign() < 0 || s.ndcUv[1].sign() < 0 || s.dirSprite[0].sign() < 0 || s.dirSprite[1].sign() < 0)) return Stage::kHit;
    if (s.windowCalls > 0 && s.rayHits == 0 && (s.ndcUv[0].sign() < 0 || s.ndcUv[1].sign() < 0)) return Stage::kWindow;
    return Stage::kNone;
}

inline const char* stageText(Stage st) noexcept {
    switch (st) {
        case Stage::kFewMoves: return "too few moves";
        case Stage::kMouse: return "the mouse NDC (K4: against the OS cursor)";
        case Stage::kUnproject: return "the unprojection (K1: the direction against the NDC)";
        case Stage::kBehind: return "the direction behind the camera (K5: no hit, the sprite on the far side)";
        case Stage::kHit: return "the hit or the sprite (K2: the panel's frame)";
        case Stage::kWindow: return "the cursor window (K3: the 2D mode)";
        default: return "none in the pick (the inversion is after it)";
    }
}

inline char signChar(int s) noexcept { return s > 0 ? '+' : s < 0 ? '-' : '0'; }

// ---- the camera getters ----
// A getter is followed only when its code is a plain accessor: mov rax,[rcx+d]; ret / lea rax,[rcx+d]; ret / mov rax,rcx; ret / mov rax,[rcx]; ret
// (d an 8- or 32-bit displacement). Then the instrument reads what it would return, and calls nothing of the game's. False for any other code.
inline bool decodeAccessor(const uint8_t* code, size_t n, int32_t* disp, bool* deref) noexcept {
    if (n >= 4 && code[0] == 0x48 && code[1] == 0x8B && code[2] == 0xC1 && code[3] == 0xC3) {
        *disp = 0;
        *deref = false;
        return true;
    }
    if (n >= 4 && code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01 && code[3] == 0xC3) {
        *disp = 0;
        *deref = true;
        return true;
    }
    if (n >= 5 && code[0] == 0x48 && (code[1] == 0x8B || code[1] == 0x8D) && code[2] == 0x41 && code[4] == 0xC3) {
        *disp = static_cast<int8_t>(code[3]);
        *deref = code[1] == 0x8B;
        return true;
    }
    if (n >= 8 && code[0] == 0x48 && (code[1] == 0x8B || code[1] == 0x8D) && code[2] == 0x81 && code[7] == 0xC3) {
        int32_t d = 0;
        for (int i = 0; i < 4; ++i) d |= static_cast<int32_t>(static_cast<uint32_t>(code[3 + i]) << (8 * i));
        *disp = d;
        *deref = code[1] == 0x8B;
        return true;
    }
    return false;
}

}  // namespace f111
}  // namespace nofoot
}  // namespace edvr
