// F114b (native on-foot VR, and the flat profile on foot): the panel pointer's second instrument, pure parts. Header only, no game or D3D11 state, so
// native_frame_test runs the very functions the hooks and the frame thread use. Journal: the arc doc, 2026-10-25, F114 and F114b.
//
// F111's flight showed the game in the 2D mode with the window's uv following the mouse, and the sprite the panel's uv-to-point (panel vtable +0x188)
// places moving against it in the pick's camera frame. This instrument splits that chain where each transform enters (build 334925, read by capstone):
//   the panel's pick mesh is refreshed (0x45412E0) from its owner's mesh data through the owner's vtable +0x10 (0x4526E40), which returns the owner's
//   world transform (+0x740 rows, +0x750, +0x760, +0x770 position) in the camera's frame: out rows +0x10/+0x20/+0x30, position +0x40, scale +0x50,
//   each owner axis dotted with the camera's axes. From the owner's rows and that output the camera's own world rows follow, without calling the
//   camera getter. So a sprite moving with the uv can be followed through: uv -> the owner's local axes (the mesh data) -> the world (the owner's
//   rows) -> the pick's camera (its rows) and, beside it, -> the eye's view (the HRC submit's view rows): the stage whose axes turn is named.
// It also reads the click path (the three UI_Mouse_Button actions the controller binds at +0x60/+0x68/+0x70, their state bytes +0x24 now and +0x34
// before, the live byte +0x171 that lets the update queue them into the sink at +0x168), the mode object at +0x188 (its vtable and its +0x40 getter,
// decoded when it is a plain accessor) and the gated note byte (exe+0x603CDB8) with the counter at +0x1B4.
#pragma once

#include "native_on_foot_f111.h"   // Corr, kMoveEps

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f114b {

// ---- the build's addresses (334925) ----
constexpr uintptr_t kOwnerXformRva = 0x4526E40, kOwnerXformSlotRva = 0x55DBA20;   // the pick mesh owner's vtable +0x10 (its vtable at 0x55DBA10)
// mov [rsp+8],rbx; push rdi; sub rsp,0x60; movaps [rsp+0x50],xmm6; mov rbx,rcx; mov rcx,[rcx+0x20]; mov rdi,rdx
constexpr uint8_t kOwnerXformPrologue[25] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x0F, 0x29, 0x74, 0x24, 0x50, 0x48, 0x8B,
                                             0xD9, 0x48, 0x8B, 0x49, 0x20, 0x48, 0x8B, 0xFA};
constexpr size_t kOwnerXformStolen = 5;
constexpr uint32_t kOffOwnerRows = 0x740;            // the owner's world rows (+0x740, +0x750, +0x760) and position (+0x770), 16 floats
constexpr uint32_t kOutRows = 0x10;                  // the output: rows +0x10, +0x20, +0x30 and position +0x40 (16 floats from +0x10)
constexpr uint32_t kOffMeshList = 0x460;             // the panel's pick mesh list; its first qword is the owner
constexpr uint32_t kOffButtons = 0x60, kButtons = 3; // the controller's three button actions (UI_Mouse_Button0..2)
constexpr uint32_t kOffButtonNow = 0x24, kOffButtonBefore = 0x34;
constexpr uint32_t kOffLive = 0x171, kOffInit = 0x170, kOffSink = 0x168, kOffMode = 0x188, kModeGetter = 0x40;
constexpr uintptr_t kNoteByteRva = 0x603CDB8;        // the byte that gates the 2D branch's "%s %.5f %.5f" note (and the +0x1B4 count)

// ---- 3x3 algebra on row-major float[9] ----
inline double det3(const double m[9]) noexcept {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
}
inline bool inv3(const double m[9], double out[9]) noexcept {
    const double d = det3(m);
    if (!(std::fabs(d) > 1e-12)) return false;
    out[0] = (m[4] * m[8] - m[5] * m[7]) / d;
    out[1] = (m[2] * m[7] - m[1] * m[8]) / d;
    out[2] = (m[1] * m[5] - m[2] * m[4]) / d;
    out[3] = (m[5] * m[6] - m[3] * m[8]) / d;
    out[4] = (m[0] * m[8] - m[2] * m[6]) / d;
    out[5] = (m[2] * m[3] - m[0] * m[5]) / d;
    out[6] = (m[3] * m[7] - m[4] * m[6]) / d;
    out[7] = (m[1] * m[6] - m[0] * m[7]) / d;
    out[8] = (m[0] * m[4] - m[1] * m[3]) / d;
    return true;
}
inline double dot3(const double a[3], const double b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double norm3(const double a[3]) noexcept { return std::sqrt(dot3(a, a)); }

// The camera's world rows (its axes) and position from the owner's world rows O and position, and the owner's transform in the camera's frame
// (out rows Q, position q): Q = O C^T and q = C (o - c), so C = (O^-1 Q)^T and c = o - C^T q (C orthonormal). blk16 holds four rows of four floats
// (three axes and the position; the fourth float of each is ignored). False when O is singular. ortho gets |C C^T - I|, the largest entry: about zero
// when the reading of the output is right.
inline bool cameraFromOwner(const float owner16[16], const float out16[16], double C[9], double c[3], double* ortho) noexcept {
    double O[9], Q[9], Oi[9];
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) {
            O[r * 3 + k] = owner16[r * 4 + k];
            Q[r * 3 + k] = out16[r * 4 + k];
        }
    if (!inv3(O, Oi)) return false;
    double Ct[9];   // O^-1 Q
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) Ct[r * 3 + k] = Oi[r * 3 + 0] * Q[0 * 3 + k] + Oi[r * 3 + 1] * Q[1 * 3 + k] + Oi[r * 3 + 2] * Q[2 * 3 + k];
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) C[r * 3 + k] = Ct[k * 3 + r];
    const double q[3] = {out16[12], out16[13], out16[14]};
    for (int k = 0; k < 3; ++k) c[k] = owner16[12 + k] - (C[0 * 3 + k] * q[0] + C[1 * 3 + k] * q[1] + C[2 * 3 + k] * q[2]);
    double worst = 0.0;
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) {
            const double v = C[a * 3] * C[b * 3] + C[a * 3 + 1] * C[b * 3 + 1] + C[a * 3 + 2] * C[b * 3 + 2] - (a == b ? 1.0 : 0.0);
            if (std::fabs(v) > worst) worst = std::fabs(v);
        }
    if (ortho) *ortho = worst;
    return true;
}

// A point p given in a frame (rows R = its axes in the world, origin o) to the world, and a world point into a frame: (p - o) dotted with the axes.
inline void frameToWorld(const double R[9], const double o[3], const double p[3], double w[3]) noexcept {
    for (int k = 0; k < 3; ++k) w[k] = o[k] + R[0 * 3 + k] * p[0] + R[1 * 3 + k] * p[1] + R[2 * 3 + k] * p[2];
}
inline void worldToFrame(const double R[9], const double o[3], const double w[3], double p[3]) noexcept {
    const double d[3] = {w[0] - o[0], w[1] - o[1], w[2] - o[2]};
    for (int r = 0; r < 3; ++r) p[r] = R[r * 3] * d[0] + R[r * 3 + 1] * d[1] + R[r * 3 + 2] * d[2];
}
// The cosine between two frames' same axis (normalised).
inline double axisCos(const double A[9], const double B[9], int axis) noexcept {
    const double a[3] = {A[axis * 3], A[axis * 3 + 1], A[axis * 3 + 2]}, b[3] = {B[axis * 3], B[axis * 3 + 1], B[axis * 3 + 2]};
    const double na = norm3(a), nb = norm3(b);
    return na > 1e-9 && nb > 1e-9 ? dot3(a, b) / (na * nb) : 0.0;
}

// ---- one controller cycle's extra readings (the F111 cycle carries the rest) ----
struct Extra {
    bool owner = false;          // the owner transform was read in this cycle's placements
    float owner16[16] = {};      // the owner's world rows and position
    float out16[16] = {};        // its transform in the camera's frame
    bool cam = false;            // the camera derived
    double C[9] = {}, c[3] = {}, ortho = 0.0;
    bool local = false;          // the sprite in the owner's local axes (through the output rows)
    double localP[2] = {};
    bool eye = false;            // the sprite as the eye sees it (tangents, x right, y up)
    double eyeTan[2] = {};
    bool camEye = false;         // the camera's axes against the eye's
    double camEyeCos[3] = {};
    bool ownerHud = false;       // the owner's axes against the HUD canvas's (HRC +0x430 rows)
    double ownerHudCos[2] = {};
    uint8_t pressed = 0, released = 0;   // bit i: button i went down / up this cycle
    bool live = false;           // +0x171: the update queues the buttons
};

// The sprite p (in the camera's frame) in the owner's local axes: solve p = Q^T l + q for l (Q the output rows: the owner's axes in the camera's frame).
inline bool localOf(const float out16[16], const float p[3], double l[3]) noexcept {
    double Qt[9], Qti[9];
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) Qt[k * 3 + r] = out16[r * 4 + k];
    if (!inv3(Qt, Qti)) return false;
    const double d[3] = {p[0] - out16[12], p[1] - out16[13], p[2] - out16[14]};
    for (int r = 0; r < 3; ++r) l[r] = Qti[r * 3] * d[0] + Qti[r * 3 + 1] * d[1] + Qti[r * 3 + 2] * d[2];
    return true;
}

// A cycle's close: the camera from the owner transform its placement read, the sprite (c.pos, the pick's camera frame) in the owner's local axes and as
// the eye sees it (eyeR the eye's view rows, eyeO its position; null when there is none), the camera's axes against the eye's, and the owner's axes
// against the HUD canvas's (hudR, the HRC's rows; null without one).
inline void closeCycle(Extra& x, const f111::Cycle& c, const double* eyeR, const double eyeO[3], const double* hudR) noexcept {
    x.cam = x.owner && cameraFromOwner(x.owner16, x.out16, x.C, x.c, &x.ortho);
    if (!x.owner || !c.hit) return;
    double l[3] = {};
    if (localOf(x.out16, c.pos, l)) {
        x.local = true;
        x.localP[0] = l[0];
        x.localP[1] = l[1];
    }
    if (x.cam && eyeR) {
        const double p[3] = {c.pos[0], c.pos[1], c.pos[2]};
        double w[3], pe[3];
        frameToWorld(x.C, x.c, p, w);
        worldToFrame(eyeR, eyeO, w, pe);
        if (std::fabs(pe[2]) > 1e-6) {   // the view's forward sign is not assumed: the tangents are over |z|
            x.eye = true;
            x.eyeTan[0] = pe[0] / std::fabs(pe[2]);
            x.eyeTan[1] = pe[1] / std::fabs(pe[2]);
        }
        x.camEye = true;
        for (int i = 0; i < 3; ++i) x.camEyeCos[i] = axisCos(x.C, eyeR, i);
    }
    if (hudR) {
        double O[9];
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) O[r * 3 + k] = x.owner16[r * 4 + k];
        x.ownerHud = true;
        x.ownerHudCos[0] = axisCos(O, hudR, 0);
        x.ownerHudCos[1] = axisCos(O, hudR, 1);
    }
}

struct Sec {
    uint32_t cycles = 0, panelCycles = 0, rayPlacements = 0, windowPlacements = 0, neither = 0, ownerReads = 0, camReads = 0, eyeReads = 0,
             moves = 0, liveCycles = 0;
    uint32_t presses[kButtons] = {}, releases[kButtons] = {}, pressesWithUv = 0;
    double orthoMax = 0.0;
    double camEyeSum[3] = {}, ownerHudSum[2] = {};
    uint32_t camEyeN = 0, ownerHudN = 0;
    // [0] u (right), [1] v (down): the uv's change against the sprite's in the pick's camera frame, in the owner's local axes, and as the eye sees it
    f111::Corr uvCam[2], uvLocal[2], uvEye[2], ndcEye[2];
    double lastPressUv[2] = {};
    void merge(const Sec& o) noexcept {
        cycles += o.cycles;
        panelCycles += o.panelCycles;
        rayPlacements += o.rayPlacements;
        windowPlacements += o.windowPlacements;
        neither += o.neither;
        ownerReads += o.ownerReads;
        camReads += o.camReads;
        eyeReads += o.eyeReads;
        moves += o.moves;
        liveCycles += o.liveCycles;
        for (uint32_t i = 0; i < kButtons; ++i) {
            presses[i] += o.presses[i];
            releases[i] += o.releases[i];
        }
        pressesWithUv += o.pressesWithUv;
        if (o.orthoMax > orthoMax) orthoMax = o.orthoMax;
        for (int i = 0; i < 3; ++i) camEyeSum[i] += o.camEyeSum[i];
        for (int i = 0; i < 2; ++i) {
            ownerHudSum[i] += o.ownerHudSum[i];
            uvCam[i].merge(o.uvCam[i]);
            uvLocal[i].merge(o.uvLocal[i]);
            uvEye[i].merge(o.uvEye[i]);
            ndcEye[i].merge(o.ndcEye[i]);
        }
        camEyeN += o.camEyeN;
        ownerHudN += o.ownerHudN;
        if (o.presses[0] + o.presses[1] + o.presses[2]) {
            lastPressUv[0] = o.lastPressUv[0];
            lastPressUv[1] = o.lastPressUv[1];
        }
    }
    double camEyeMean(int i) const noexcept { return camEyeN ? camEyeSum[i] / camEyeN : 0.0; }
    double ownerHudMean(int i) const noexcept { return ownerHudN ? ownerHudSum[i] / ownerHudN : 0.0; }
};

inline void addCycle(Sec& s, const f111::Cycle& c, const Extra& e) noexcept {
    ++s.cycles;
    if (c.rayCalls || c.windowCalls) ++s.panelCycles;
    if (c.rayCalls) ++s.rayPlacements;
    if (c.windowCalls) ++s.windowPlacements;
    if (c.placements && !c.rayCalls && !c.windowCalls) ++s.neither;
    if (e.owner) ++s.ownerReads;
    if (e.cam) {
        ++s.camReads;
        if (e.ortho > s.orthoMax) s.orthoMax = e.ortho;
    }
    if (e.eye) ++s.eyeReads;
    if (e.live) ++s.liveCycles;
    if (e.camEye) {
        for (int i = 0; i < 3; ++i) s.camEyeSum[i] += e.camEyeCos[i];
        ++s.camEyeN;
    }
    if (e.ownerHud) {
        for (int i = 0; i < 2; ++i) s.ownerHudSum[i] += e.ownerHudCos[i];
        ++s.ownerHudN;
    }
    float u = 0.0f, v = 0.0f;
    const bool uv = f111::cycleUv(c, &u, &v);
    for (uint32_t i = 0; i < kButtons; ++i) {
        if (e.pressed & (1u << i)) {
            ++s.presses[i];
            if (uv) {
                ++s.pressesWithUv;
                s.lastPressUv[0] = u;
                s.lastPressUv[1] = v;
            }
        }
        if (e.released & (1u << i)) ++s.releases[i];
    }
}

// Two consecutive cycles of one controller, both on a panel with a uv and the sprite on it (hit).
inline void addStep(Sec& s, const f111::Cycle& a, const Extra& ea, const f111::Cycle& b, const Extra& eb) noexcept {
    if (!f111::moved(a, b)) return;
    float ua = 0.0f, va = 0.0f, ub = 0.0f, vb = 0.0f;
    if (!f111::cycleUv(a, &ua, &va) || !f111::cycleUv(b, &ub, &vb) || !a.hit || !b.hit) return;
    ++s.moves;
    const double du = double(ub) - ua, dv = double(vb) - va;
    s.uvCam[0].add(du, double(b.pos[0]) - a.pos[0]);
    s.uvCam[1].add(dv, -(double(b.pos[1]) - a.pos[1]));
    if (ea.local && eb.local) {
        s.uvLocal[0].add(du, eb.localP[0] - ea.localP[0]);
        s.uvLocal[1].add(dv, -(eb.localP[1] - ea.localP[1]));
    }
    if (ea.eye && eb.eye) {
        s.uvEye[0].add(du, eb.eyeTan[0] - ea.eyeTan[0]);
        s.uvEye[1].add(dv, -(eb.eyeTan[1] - ea.eyeTan[1]));
        s.ndcEye[0].add(double(b.ndc[0]) - a.ndc[0], eb.eyeTan[0] - ea.eyeTan[0]);
        s.ndcEye[1].add(-(double(b.ndc[1]) - a.ndc[1]), eb.eyeTan[1] - ea.eyeTan[1]);
    }
}

// ---- the verdicts ----
inline int pairSign(const f111::Corr c[2]) noexcept {
    const int a = c[0].sign(), b = c[1].sign();
    if (a > 0 && b > 0) return 1;
    if (a < 0 && b < 0) return -1;
    return 0;
}

enum class Turn : uint8_t { kNoPanel = 0, kFewMoves, kNone, kCamera, kOwner, kMesh, kTurnedEyeUnread, kUnclear };
// Which transform turns the sprite against the uv. The eye's view is the reference (the drawn panel is upright there): the sprite's uv-to-world
// chain is the mesh's uv-to-local (uvLocal) then the owner's rows; the camera only enters the pick's frame (uvCam), not the world.
inline Turn turn(const Sec& s) noexcept {
    if (!s.panelCycles) return Turn::kNoPanel;
    if (s.moves < f111::kMinMoves) return Turn::kFewMoves;
    const int cam = pairSign(s.uvCam), eye = pairSign(s.uvEye), local = pairSign(s.uvLocal);
    if (s.eyeReads && s.uvEye[0].n >= f111::kMinPairs) {
        if (eye > 0) return cam < 0 ? Turn::kCamera : Turn::kNone;   // upright for the eye: only the pick's camera frame turns it, if anything
        if (eye < 0) return local < 0 ? Turn::kMesh : local > 0 ? Turn::kOwner : Turn::kUnclear;
        return Turn::kUnclear;
    }
    if (cam < 0) return Turn::kTurnedEyeUnread;
    if (cam > 0) return Turn::kNone;
    return Turn::kUnclear;
}
inline const char* turnText(Turn t) noexcept {
    switch (t) {
        case Turn::kNoPanel: return "no panel";
        case Turn::kFewMoves: return "too few moves";
        case Turn::kNone: return "none (the sprite follows the uv)";
        case Turn::kCamera: return "the pick camera (upright for the eye, turned in the pick's frame)";
        case Turn::kOwner: return "the owner's world transform (the mesh upright in its own axes, turned for the eye)";
        case Turn::kMesh: return "the mesh data (the uv runs against the owner's own axes)";
        case Turn::kTurnedEyeUnread: return "turned in the pick's frame (the eye not read)";
        default: return "unclear";
    }
}
// The cursor's draw against the sprite: what the eye would see of the sprite's own pose. The pilot's report decides the rest: a cursor seen moving
// against the mouse while this follows it means the draw adds a turn of its own.
inline const char* drawText(const Sec& s) noexcept {
    if (!s.eyeReads || s.ndcEye[0].n < f111::kMinPairs) return "the eye not read";
    const int e = pairSign(s.ndcEye);
    return e > 0 ? "the sprite's pose, as the eye sees it, follows the mouse" : e < 0 ? "the sprite's pose, as the eye sees it, moves against the mouse"
                                                                                       : "unclear";
}
inline const char* modeText(const Sec& s) noexcept {
    if (!s.panelCycles) return "no panel";
    if (s.rayPlacements && s.windowPlacements) return "both";
    return s.rayPlacements ? "ray" : "2D window";
}

// The mode object's +0x40 getter, when it is a plain accessor: a constant, or a byte or dword field of the object.
enum class Getter : uint8_t { kUnknown = 0, kConst, kByteField, kDwordField };
inline Getter decodeModeGetter(const uint8_t* code, size_t n, int32_t* value) noexcept {
    if (n >= 3 && code[0] == 0x33 && code[1] == 0xC0 && code[2] == 0xC3) {   // xor eax,eax; ret
        *value = 0;
        return Getter::kConst;
    }
    if (n >= 3 && code[0] == 0x32 && code[1] == 0xC0 && code[2] == 0xC3) {   // xor al,al; ret
        *value = 0;
        return Getter::kConst;
    }
    if (n >= 6 && code[0] == 0xB8 && code[5] == 0xC3) {   // mov eax,imm32; ret
        int32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<int32_t>(static_cast<uint32_t>(code[1 + i]) << (8 * i));
        *value = v;
        return Getter::kConst;
    }
    if (n >= 3 && code[0] == 0xB0 && code[2] == 0xC3) {   // mov al,imm8; ret
        *value = code[1];
        return Getter::kConst;
    }
    if (n >= 4 && code[0] == 0x8B && code[1] == 0x41 && code[3] == 0xC3) {   // mov eax,[rcx+d8]; ret
        *value = static_cast<int8_t>(code[2]);
        return Getter::kDwordField;
    }
    if (n >= 7 && code[0] == 0x8B && code[1] == 0x81 && code[6] == 0xC3) {   // mov eax,[rcx+d32]; ret
        int32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<int32_t>(static_cast<uint32_t>(code[2 + i]) << (8 * i));
        *value = v;
        return Getter::kDwordField;
    }
    if (n >= 5 && code[0] == 0x0F && code[1] == 0xB6 && code[2] == 0x41 && code[4] == 0xC3) {   // movzx eax,byte [rcx+d8]; ret (5 bytes)
        *value = static_cast<int8_t>(code[3]);
        return Getter::kByteField;
    }
    if (n >= 4 && code[0] == 0x8A && code[1] == 0x41 && code[3] == 0xC3) {   // mov al,[rcx+d8]; ret (4 bytes, its own length guard)
        *value = static_cast<int8_t>(code[2]);
        return Getter::kByteField;
    }
    if (n >= 8 && code[0] == 0x0F && code[1] == 0xB6 && code[2] == 0x81 && code[7] == 0xC3) {   // movzx eax,byte [rcx+d32]; ret
        int32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<int32_t>(static_cast<uint32_t>(code[3 + i]) << (8 * i));
        *value = v;
        return Getter::kByteField;
    }
    return Getter::kUnknown;
}

// A button's transition this cycle, from its state bytes (now, before): 1 down, 2 up, 0 none (the update's own rule at 0x454F770).
inline int buttonEdge(uint8_t now, uint8_t before) noexcept {
    if (now && !before) return 1;
    if (!now && before) return 2;
    return 0;
}

}  // namespace f114b
}  // namespace nofoot
}  // namespace edvr
