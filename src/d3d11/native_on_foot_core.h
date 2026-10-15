#pragma once
// Native on-foot VR, flight F0 (docs\design-native-onfoot-vr-2026-10-13.md, "Proposed first flight F0").
//
// Pure decisions and bytes: no Windows, no game, no Config, so a rig could drive them. native_on_foot.cpp holds the hook, the key,
// the journal reads and the log.
//
// THE SETTER. EliteDangerous64.exe build 332841 (the build Explorer Cam pins; PE stamp and image size are ecm::kExpectedTimestamp and
// ecm::kExpectedImageSize) has a render-mode setter, a virtual function reached only through the vtable at image RVA 0x52E9608, slot 5
// (byte offset 0x28), at function RVA 0x28723C0. Its signature is void(__fastcall*)(void* selector, uint32_t mode). It records the mode
// it was asked for in selector+0x20 (the source mode); selector+0x58 is the renderer owner, and owner+0x60 points back at selector.
// The modes come in pairs: each cinema mode (the flat screen shown in the headset) has an HMD partner (the headset's own stereo). The
// setter's remap at 0x28873C0 uses the same pairs. Pair A is 5 (cinema) and 3 (HMD); pair B is 6 (cinema) and 4 (HMD). Flight F0 on
// Epic (Pimax OpenXR) logged mode 6 at disembark, with the rig's HMD mode 4; the first build assumed only the A pair and never fired.
//
// THE SUBSTITUTION. An incoming cinema mode (5 or 6) is forwarded as its HMD partner (3 or 4), once, on the caller's thread, when every
// gate below admits it. Anything else is forwarded unchanged. Nothing here calls the setter itself, writes selector+0x20, or touches
// the settings words.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "explorer_cam_core.h"   // ecm::kExpectedTimestamp and ecm::kExpectedImageSize: build 332841's PE identity, shared with the other build-keyed hooks
#include "explorer_cam_follow_core.h"   // F49: ecm::quatRows, the pose's rotation rows (the pose freeze's rest locals)

namespace edvr {
namespace nofoot {

constexpr uintptr_t kSetterRva = 0x28723C0;          // the render-mode setter's entry (EliteDangerous64.exe+0x28723C0)
constexpr uintptr_t kSetterVtableRva = 0x52E9608;    // the vtable that holds the setter
constexpr size_t kSetterSlotOffset = 0x28;           // slot 5 of that vtable (5 * 8)
constexpr uint32_t kModeHmdA = 3;                    // HMD stereo, pair A
constexpr uint32_t kModeCinemaA = 5;                 // the flat cinema screen shown in the headset, pair A
constexpr uint32_t kModeHmdB = 4;                    // HMD stereo, pair B (the rig Sean flies on: the main menu runs in 4 too)
constexpr uint32_t kModeCinemaB = 6;                 // the flat cinema screen shown in the headset, pair B
constexpr size_t kSourceModeOffset = 0x20;           // selector+0x20: the source mode the setter writes
constexpr size_t kOwnerOffset = 0x58;                // selector+0x58: the renderer owner
constexpr size_t kOwnerSelectorOffset = 0x60;        // the selector is EMBEDDED in its owner at +0x60: owner+0x60 IS the selector (address arithmetic;
                                                     // nothing is read there). The selector's own first qword is the setter's vtable.

// The first 27 bytes of the setter, read from build 332841's executable: `mov [rsp+10h],rbx; push rdi; sub rsp,20h; mov edi,edx;
// mov rbx,rcx; cmp [rcx+20h],edx; jne +0Dh; cmp dword [rcx+2B4h],0`. No rip-relative byte. CodeHook steals the first instruction
// (5 bytes, `mov [rsp+10h],rbx`), and no branch lands inside those 5 bytes.
constexpr size_t kPrologueBytes = 27;
constexpr uint8_t kPrologue[kPrologueBytes] = {
    0x48, 0x89, 0x5C, 0x24, 0x10,              // mov [rsp+10h], rbx
    0x57,                                      // push rdi
    0x48, 0x83, 0xEC, 0x20,                    // sub rsp, 20h
    0x8B, 0xFA,                                // mov edi, edx
    0x48, 0x8B, 0xD9,                          // mov rbx, rcx
    0x39, 0x51, 0x20,                          // cmp [rcx+20h], edx
    0x75, 0x0D,                                // jne +0Dh
    0x83, 0xB9, 0xB4, 0x02, 0x00, 0x00, 0x00   // cmp dword [rcx+2B4h], 0
};

// Why a call was or was not substituted. The order is the order of the checks.
enum class Reason : uint8_t {
    NotCinema,        // the incoming mode is not a cinema mode (5 or 6): forwarded unchanged (HMD modes always are)
    KeyOff,           // incoming cinema mode, but fix.native_on_foot is off
    GateMethod,       // incoming cinema mode, key on: the gate is the method (F34: the only one), so the setter never substitutes
};

struct Inputs {
    bool keyOn = false;            // fix.native_on_foot read as on (published by the frame thread)
    uint32_t incoming = 0;         // the mode the caller asked for
    bool selectorValid = false;    // selector+0x58 -> owner and owner+0x60 == selector, read under SEH
    bool selectorFrozen = false;   // the selector is the one frozen at the first valid call
    bool gameSession = false;      // journalGameplay(): LoadGame seen in this process and no Shutdown since
};

inline bool isCinemaMode(uint32_t mode) { return mode == kModeCinemaA || mode == kModeCinemaB; }
inline bool isHmdMode(uint32_t mode) { return mode == kModeHmdA || mode == kModeHmdB; }

// The admission rule. Pure: the hook supplies the facts it has just read, and the answer is the only thing that substitutes.
inline Reason admit(const Inputs& in) {
    if (!isCinemaMode(in.incoming)) return Reason::NotCinema;
    if (!in.keyOn) return Reason::KeyOff;
    return Reason::GateMethod;   // F34: the gate is the only method; nothing is substituted
}

// The mode the original is forwarded with: the HMD partner of an admitted cinema mode (5 -> 3, 6 -> 4), otherwise what was asked.
inline uint32_t effectiveMode(Reason, uint32_t incoming) {   // F34: nothing is substituted, so the mode forwarded is the one asked for
    return incoming;
}

inline const char* reasonName(Reason r) {
    switch (r) {
        case Reason::NotCinema: return "not_cinema";
        case Reason::KeyOff: return "key_off";
        case Reason::GateMethod: return "method_gate";
    }
    return "unknown";
}

// A selector is the setter's renderer object when its vtable is the setter's vtable and its owner's embedded selector is it. The caller
// passes 0 for any value that could not be read; 0 never passes. Pure: no reads here.
inline bool selectorShapeOk(uintptr_t vtable, uintptr_t expectedVtable, uintptr_t owner, uintptr_t selector) {
    if (!selector || !owner || !vtable) return false;
    if (vtable != expectedVtable) return false;
    return owner + kOwnerSelectorOffset == selector;
}

// The identity check's answer for a PE stamp and image size: build 332841 or not.
inline bool identityMatches(uint32_t timestamp, uint32_t imageSize) {
    return timestamp == ecm::kExpectedTimestamp && imageSize == ecm::kExpectedImageSize;
}

// The on-foot HUD issuers (F3 stack probe; flat_projection_recipes.h:154-155 names both as vertex-projected UI). Drawn into the flat frame
// in cinema, with zero draws in native stereo on foot (F2). The index is the table position, or -1 for any other vertex shader.
constexpr uint64_t kHudIssuerVs[2] = {0x46546443FD3C3F88ull, 0x95D01BA609BF7500ull};
inline int hudIssuerIndex(uint64_t vs) {
    return vs == kHudIssuerVs[0] ? 0 : vs == kHudIssuerVs[1] ? 1 : -1;
}
// F45/F46 (TEMPORARY, the marker probe): the interface composite that reads the tagged marker record, a world-space billboard drawn into the
// post-tonemap eye target the UI layer redirects. Its b0 matters only in rows 4, 5 and 7 (clip.x, clip.y, w = |r7 . p|).
constexpr uint64_t kMarkerCompositeVs = 0xB10B032BDFD46700ull, kMarkerCompositePs = 0xDB899F4BD577F2E5ull;

// ---- F8: the first-person HUD override and its census (TEMPORARY; native_on_foot.cpp holds the code; approved by Sean 2026-10-13). ----
// The override: int32 at EliteDangerous64.exe+0x5E82B48, -1 auto. The requester at 0x2887A15 reads it (sole reader; no writer in the image).
// The census: owner = [sel+0x58]; settings = [owner+0x1180]; RC = [settings+0x18]; fov = float [RC+0x16C]; mode = dword [settings+0x3694];
// selMode = dword [sel+0x20]; W = [settings+0x2CB8]; slot k at [W+0x90] (k = 0) or [W+0x98] (k = 1); F = [slot+0x2E0]; F70 = byte [F+0x70];
// node = [F+0x30]; fp_on = byte [node+0x30]; pg = [F+0x38]; pg250 = byte [pg+0x250].
constexpr uintptr_t kHudOverrideRva = 0x5E82B48;
constexpr uint32_t kCensusOwnerOff = 0x58, kCensusSettingsOff = 0x1180, kCensusRcOff = 0x18, kCensusFovOff = 0x16C, kCensusModeOff = 0x3694,
                   kCensusSelModeOff = 0x20, kCensusWOff = 0x2CB8, kCensusSlotA = 0x90, kCensusSlotB = 0x98, kCensusPassOff = 0x2E0,
                   kCensusF70Off = 0x70, kCensusNodeOff = 0x30, kCensusNodeOnOff = 0x30, kCensusPgOff = 0x38, kCensusPg250Off = 0x250;

// ---- F9 (TEMPORARY; native_on_foot.cpp holds the code). Two game code patches (approved by Sean 2026-10-14) and three read-only probes. ----
// Patch A (fix.native_on_foot_panels): 76 73 (jbe 0x19B15FC) -> 90 90, in the panel gate of HumanoidPanelComponent (0x19B14E0).
constexpr uintptr_t kPanelPatchRva = 0x19B1587;
constexpr size_t kPanelPatchLen = 2;
constexpr uint8_t kPanelPatchFrom[2] = {0x76, 0x73};
constexpr uint8_t kPanelPatchTo[2] = {0x90, 0x90};
// Patch B (fix.native_on_foot_hud_mesh): 74 2A (jz +0x2A) -> EB 2A; the first byte only is written (the HRC camera-space placement branch).
constexpr uintptr_t kMeshPatchRva = 0x1A36453;
constexpr size_t kMeshPatchLen = 1;
constexpr uint8_t kMeshPatchFrom[2] = {0x74, 0x2A};
constexpr uint8_t kMeshPatchTo[2] = {0xEB, 0x2A};
// Site 2 of the mesh unit (F10): the HRC's creation gate. The six bytes at +0x1A149A6 are the call qword ptr [rax+0x80] on dep = [HRC+0x1D0]
// (the predicate in slot 0x1A8D190); the patch changes the call's displacement byte at +0x1A149A8 from 0x80 to 0x78 (the slot 0x78).
constexpr uintptr_t kMeshSite2Rva = 0x1A149A6;
constexpr uint8_t kMeshSite2Expect[6] = {0xFF, 0x90, 0x80, 0x00, 0x00, 0x00};
constexpr uint8_t kMeshSite2Patched[6] = {0xFF, 0x90, 0x78, 0x00, 0x00, 0x00};
constexpr size_t kMeshSite2ByteOff = 2;
// Probes. P1: panel submit (rcx = set, rdx = view, r8b = flag); P2: the HRC submit (six arguments; rcx = HRC); P3: the per-instance submit.
constexpr size_t kF9ProbeCount = 3;
constexpr size_t kF9Hrc = 3;
constexpr uintptr_t kPanelSubmitRva = 0x4714FE0;
constexpr size_t kPanelSubmitStolen = 5;
constexpr size_t kPanelSubmitPrologueBytes = 32;
constexpr uint8_t kPanelSubmitPrologue[kPanelSubmitPrologueBytes] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20,
    0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x99, 0xB8, 0x01, 0x00};
constexpr uintptr_t kHrcSubmitRva = 0x1A14970;
constexpr size_t kHrcSubmitStolen = 5;   // push rbp (2), push rbx (1), push r12 (2); the rip-relative cookie load at +0x13 is NOT stolen
constexpr size_t kHrcSubmitPrologueBytes = 26;
constexpr uint8_t kHrcSubmitPrologue[kHrcSubmitPrologueBytes] = {
    0x40, 0x55, 0x53, 0x41, 0x54, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xC1, 0x48, 0x81, 0xEC, 0xA8,
    0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xDE, 0x1F, 0x49, 0x04};
constexpr uintptr_t kInstSubmitRva = 0x434D320;
constexpr size_t kInstSubmitStolen = 5;
constexpr size_t kInstSubmitPrologueBytes = 32;
constexpr uint8_t kInstSubmitPrologue[kInstSubmitPrologueBytes] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
    0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF9, 0x41, 0x8B, 0xD8, 0x48, 0x8B, 0x89, 0x50, 0x01, 0x00};
// The fields. HRC: byte [+0x291], [+0x470], [+0x4C0]; qwords [+0x418], [+0x420], [+0x428] (the three mesh instances); dep = qword [+0x1D0].
// View (the instance submit's rdx): dword [+0x8E8] (the view id); floats [+0x50], [+0x54], [+0x58] (the camera position).
constexpr uint32_t kF9HrcG291Off = 0x291, kF9HrcG470Off = 0x470, kF9HrcG4C0Off = 0x4C0, kF9HrcInst0Off = 0x418, kF9HrcDepOff = 0x1D0,
                   kF9ViewIdOff = 0x8E8, kF9ViewCamOff = 0x50;
// F11: the view's rotation rows start at +0x20 (the camera position follows at +0x50); the HRC's world matrix is 16 floats at +0x430.
constexpr uint32_t kF9ViewRotOff = 0x20, kF9HrcMatrixOff = 0x430;
// F12: the HUD distance. The second placement matrix sits at +0x480; the default distance is the game's own (0.192 m), and the range is the setting's.
constexpr uint32_t kHudMatrix2Off = 0x480;
constexpr float kHudDefaultDistance = 0.192f;
constexpr float kHudDistanceMin = 0.01f, kHudDistanceMax = 0.40f;
// F16: the setting's default distance (Sean's tuning), which is not the game's 0.192 above; used only as the setting's fallback.
// F19: Sean's tuning is 0.01 (the minimum of the range).
constexpr float kHudDistanceSettingDefault = 0.01f;
// F13: the HUD size (the basis rows only), the head follow's smoothing (ms), and the visor override test (TEMPORARY).
// F16: size's default is 1.50 and its maximum 3.00 (Sean's tuning hit the old maximum); smoothing's default is 40 ms.
// F19: size's default is Sean's 1.60.
constexpr float kHudSizeMin = 0.30f, kHudSizeMax = 3.00f, kHudSizeDefault = 1.60f;
constexpr int kHudSmoothMin = 0, kHudSmoothMax = 500, kHudSmoothDefault = 0;   // F34: the shipping default (Sean's tuning)
// F16: the per-group HUD offsets (a fraction of the HUD canvas, live), and the default spread (the margins' extra, F14).
// F19: the spread's default is Sean's 0.04.
constexpr float kHudOffsetMin = -0.30f, kHudOffsetMax = 0.30f;
constexpr float kHudSpreadDefault = 0.04f;
// F16: the HUD group of a layout part, by the name the layout passes (rec+0). 0 left, 1 top, 2 right, 3 centre; -1 is a name not in the table: no
// offset for it, and the layout logs it once. The names are the ones the F14 layout probe logged.
constexpr int kHudGroupCount = 4;
inline int hudGroupOf(const char* name) {
    struct Entry {
        const char* name;
        int group;
    };
    static const Entry kEntries[] = {
        {"RadarComponent", 0},       {"Vitals", 0},           {"AccessWheel", 0},  {"ItemWheel", 0},
        {"ChatWindow", 0},           {"WingMembers", 0},      {"Compass", 1},      {"Alerts", 1},
        {"ConflictZoneStatus", 1},   {"CapturePointProgress", 1}, {"ToolProgress", 1}, {"HumanoidInfoPanel", 2},
        {"ConversationInfo", 2},     {"MiningRigMeters", 2},  {"GrenadeIndicator", 3}, {"SamplingTool", 3},
        {"TutorialMessage", 3},
    };
    if (!name) return -1;
    for (const Entry& e : kEntries)
        if (std::strcmp(name, e.name) == 0) return e.group;
    return -1;
}
constexpr uintptr_t kVisorOverrideRva = 0x5F741B7;   // byte: 1 = the visor override is active (the RE: visorDown = byte[B7] ? byte[B8] : vfunc)
constexpr uintptr_t kVisorDownRva = 0x5F741B8;       // byte: the forced visor state while the override is active (0 = up)
// F17: the HUD element scale (the layout routine 0x1AB32F0, branch at 0x1AB3351). The byte: non-zero means the layout scale S is the float below;
// zero means S = min(1, aspect / 1.7222), the game's own. The float: S itself (1.0 in the file). The user's setting is 0 (the game's own) or 0.30..2.00.
constexpr uintptr_t kElemScaleModeRva = 0x5F741CA;   // byte
constexpr uintptr_t kElemScaleRva = 0x5E7A760;       // float
constexpr float kHudElemScaleMin = 0.30f, kHudElemScaleMax = 2.00f;
// F38: the render head's own translation. The runtime folds the applied eye offset into the seated origin, so the render pose's translation is
// the head less that offset: the head is render + applied. The F38 flight's 1 s line prints both to confirm the sign.
inline void alignHeadTrue(const float render[12], const double applied[3], double out[3]) noexcept {
    for (size_t i = 0; i < 3; ++i) out[i] = static_cast<double>(render[4 * i + 3]) + applied[i];
}
// F38: the angle in degrees between the rotations of two 3x4 poses (rotation in [4i+j], translation in column 3): arccos((trace(Ra Rb^T) - 1) / 2).
inline double rotDeltaDeg(const float a[12], const float b[12]) noexcept {
    double tr = 0.0;
    for (size_t i = 0; i < 3; ++i)
        for (size_t k = 0; k < 3; ++k) tr += static_cast<double>(a[4 * i + k]) * static_cast<double>(b[4 * i + k]);
    const double c = std::max(-1.0, std::min(1.0, (tr - 1.0) * 0.5));
    return std::acos(c) * 180.0 / 3.14159265358979323846;
}
// F38: the waypoint lead. r0 is the newest pose, r1 the one before it. D = R0 R1^T is the rotation over one frame; its axis and angle, scaled
// by L, give D^L, applied to R0 (Rodrigues). The translation runs on linearly: t0 + L (t0 - t1). out takes the same 3x4 layout.
inline void leadPose(const float r0[12], const float r1[12], double L, float out[12]) noexcept {
    double R0[3][3], D[3][3];
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 3; ++j) {
            R0[i][j] = static_cast<double>(r0[4 * i + j]);
            double acc = 0.0;
            for (size_t k = 0; k < 3; ++k) acc += static_cast<double>(r0[4 * i + k]) * static_cast<double>(r1[4 * j + k]);
            D[i][j] = acc;
        }
    const double tr = D[0][0] + D[1][1] + D[2][2];
    const double th = std::acos(std::max(-1.0, std::min(1.0, (tr - 1.0) * 0.5)));
    const double s = std::sin(th);
    double K[3][3] = {};
    if (th > 1e-9 && s > 1e-9) {
        const double ax = (D[2][1] - D[1][2]) / (2.0 * s);
        const double ay = (D[0][2] - D[2][0]) / (2.0 * s);
        const double az = (D[1][0] - D[0][1]) / (2.0 * s);
        K[0][1] = -az; K[0][2] = ay;
        K[1][0] = az;  K[1][2] = -ax;
        K[2][0] = -ay; K[2][1] = ax;
    }
    const double phi = L * th;
    const double sp = std::sin(phi), cp = std::cos(phi);
    double K2[3][3] = {}, Rl[3][3] = {}, P[3][3] = {};
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 3; ++j)
            for (size_t k = 0; k < 3; ++k) K2[i][j] += K[i][k] * K[k][j];
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 3; ++j) Rl[i][j] = (i == j ? 1.0 : 0.0) + sp * K[i][j] + (1.0 - cp) * K2[i][j];
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 3; ++j)
            for (size_t k = 0; k < 3; ++k) P[i][j] += Rl[i][k] * R0[k][j];
    for (size_t i = 0; i < 3; ++i) {
        for (size_t j = 0; j < 3; ++j) out[4 * i + j] = static_cast<float>(P[i][j]);
        const double t0 = static_cast<double>(r0[4 * i + 3]);
        const double t1 = static_cast<double>(r1[4 * i + 3]);
        out[4 * i + 3] = static_cast<float>(t0 + L * (t0 - t1));
    }
}
// F40: rotation about +y by psi: out = Ry(psi) . v, Ry = [c 0 s; 0 1 0; -s 0 c].
inline void ryApply(double psi, const double v[3], double out[3]) noexcept {
    const double c = std::cos(psi), s = std::sin(psi);
    out[0] = c * v[0] + s * v[2];
    out[1] = v[1];
    out[2] = -s * v[0] + c * v[2];
}
// F40: the body error (target minus camera, body axes) to the seated offset: seated = D . Ry(psi) . body, D = diag(-1, -1, +1).
inline void seatedFromBody(const double body[3], double psi, double out[3]) noexcept {
    double r[3] = {};
    ryApply(psi, body, r);
    out[0] = -r[0];
    out[1] = -r[1];
    out[2] = r[2];
}
// F40: the head's seated translation, in body axes: Ry(psi)^-1 . diag(1, 1, -1) . tS (OpenXR +z is back, body +z is forward).
inline void hmdInBody(const double ts[3], double psi, double out[3]) noexcept {
    const double v[3] = {ts[0], ts[1], -ts[2]};
    ryApply(-psi, v, out);
}
// F41: one critically damped step toward `target` (one axis): acceleration (target - pos) / tau^2 - 2 vel / tau, the move at most `cap` a
// frame. The views' and the body's filters, and the rig, run this one function. dt <= 0 does nothing.
inline void cdStep(double& pos, double& vel, double target, double tau, double dt, double cap) noexcept {
    if (dt <= 0.0) return;
    const double acc = (target - pos) / (tau * tau) - 2.0 * vel / tau;
    vel += acc * dt;
    double move = vel * dt;
    if (move > cap) move = cap;
    if (move < -cap) move = -cap;
    pos += move;
}
// F48: the standing height. The body's vertical placement holds the 3P anchor's standing height (body axes, up) instead of the live one, so a
// drawn weapon (the 3P head drops 22 to 29 cm) does not lift the body. Captured at key-on (the first step after reset not under ADS) and again whenever the
// stance is at rest: not aiming down sights and the 3P head's height steady (within kSteadyM) for kRestMs; the capture is the anchor's mean
// height over that window. x and z stay live (standingApply).
struct StandingHeight {
    static constexpr double kSteadyM = 0.02;     // the head's height range that counts as steady
    static constexpr uint64_t kRestMs = 1000;    // how long it must stay steady
    bool valid = false;
    double value = 0.0;                          // the captured standing height (body axes, up, metres)
    uint64_t winStart = 0;
    double headMin = 0.0, headMax = 0.0, anchorSum = 0.0;
    uint32_t n = 0;
    uint32_t captures = 0;
    void reset() noexcept { *this = StandingHeight{}; }
    // One frame: the 3P head's height and the 3P anchor's height (body axes, up), and the ADS flag. True when this step captured.
    bool step(uint64_t now, double headUp, double anchorUp, bool ads) noexcept {
        if (ads) {   // ADS is never a rest (nor a key-on capture): the window starts again after it; the heights are not read
            n = 0;
            return false;
        }
        if (!valid) {
            valid = true;
            value = anchorUp;
            ++captures;
            restart(now, headUp, anchorUp);
            return true;
        }
        if (n == 0) {
            restart(now, headUp, anchorUp);
            return false;
        }
        const double lo = headUp < headMin ? headUp : headMin, hi = headUp > headMax ? headUp : headMax;
        if (hi - lo > kSteadyM) {
            restart(now, headUp, anchorUp);
            return false;
        }
        headMin = lo;
        headMax = hi;
        anchorSum += anchorUp;
        ++n;
        if (now - winStart < kRestMs) return false;
        value = anchorSum / static_cast<double>(n);
        ++captures;
        restart(now, headUp, anchorUp);
        return true;
    }
    void restart(uint64_t now, double headUp, double anchorUp) noexcept {
        winStart = now;
        headMin = headMax = headUp;
        anchorSum = anchorUp;
        n = 1;
    }
};
// F48: the 3P side's point with its vertical (body axes, index 1) taken from the standing height; x and z live.
inline void standingApply(const double src[3], double standingUp, double out[3]) noexcept {
    out[0] = src[0];
    out[1] = standingUp;
    out[2] = src[2];
}

// ---- F49 (TEMPORARY): the pose freeze, pure half -------------------------------------------------------------------------------
// The frAnim Animate handler writes one 64-byte row-vector model matrix per joint: rows 0..2 the rotation (w 0), row 3 the position (w 1), and
// out[j] = local[j] x out[parent(j)]. The rest local of a joint is the pose's 32-byte record (position vec4, then the quaternion) as R(q) with the
// position in row 3, R(q) laid out as ecm::quatRows. The frozen set F is the torso-side child subtree of the lowest common ancestor of the neck and
// the left foot (the hips); each member's matrix is its local (rest, or blended toward rest) times its parent's new matrix, anchored at the
// animated matrix of the hips.
constexpr uint32_t kPfzMaxF = 128;           // the frozen set's member cap
constexpr uint32_t kPfzMaxJoints = 0xC0;     // the Animate output's joint cap (the span reader's min(count, 0xC0))
constexpr double kPfzBlendSec = 0.3;         // the entry and exit blend

// The rest local matrix of a pose record (pos vec4 at +0, quaternion at +0x10).
inline void pfzLocalMatrix(const float rec[8], float out[16]) noexcept {
    float r[9];
    ecm::quatRows(rec + 4, r);
    for (int i = 0; i < 3; ++i) {
        out[i * 4 + 0] = r[i * 3 + 0];
        out[i * 4 + 1] = r[i * 3 + 1];
        out[i * 4 + 2] = r[i * 3 + 2];
        out[i * 4 + 3] = 0.0f;
    }
    out[12] = rec[0];
    out[13] = rec[1];
    out[14] = rec[2];
    out[15] = 1.0f;
}
// out = a x b (row-vector 4x4, row-major). out may not alias a or b.
inline void pfzMul(const float a[16], const float b[16], float out[16]) noexcept {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
            out[i * 4 + j] = s;
        }
}
// The inverse of an affine row-vector matrix (rotation and scale in the upper 3x3, position in row 3). False when the 3x3 is singular.
inline bool pfzAffineInverse(const float m[16], float out[16]) noexcept {
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    if (!(std::fabs(det) > 1e-12)) return false;
    const double inv[9] = {A / det, -(b * i - c * h) / det, (b * f - c * e) / det,
                           B / det, (a * i - c * g) / det, -(a * f - c * d) / det,
                           C / det, -(a * h - b * g) / det, (a * e - b * d) / det};
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) out[r * 4 + k] = static_cast<float>(inv[r * 3 + k]);
        out[r * 4 + 3] = 0.0f;
    }
    for (int k = 0; k < 3; ++k)   // pos' = -t x inv(A)
        out[12 + k] = static_cast<float>(-(m[12] * inv[0 * 3 + k] + m[13] * inv[1 * 3 + k] + m[14] * inv[2 * 3 + k]));
    out[15] = 1.0f;
    return true;
}
// The quaternion (x, y, z, w) of a rotation in the upper 3x3 (the inverse of ecm::quatRows; each row is normalised first, so a uniform scale drops out).
inline void pfzQuatFromMatrix(const float m[16], float q[4]) noexcept {
    double R[3][3];
    for (int i = 0; i < 3; ++i) {
        const double len = std::sqrt(static_cast<double>(m[i * 4]) * m[i * 4] + static_cast<double>(m[i * 4 + 1]) * m[i * 4 + 1] +
                                     static_cast<double>(m[i * 4 + 2]) * m[i * 4 + 2]);
        const double k = len > 1e-9 ? 1.0 / len : 1.0;
        for (int j = 0; j < 3; ++j) R[i][j] = m[i * 4 + j] * k;
    }
    double x, y, z, w;
    const double tr = R[0][0] + R[1][1] + R[2][2];
    if (tr > 0.0) {
        const double s = std::sqrt(tr + 1.0) * 2.0;   // 4w
        w = 0.25 * s;
        x = (R[1][2] - R[2][1]) / s;
        y = (R[2][0] - R[0][2]) / s;
        z = (R[0][1] - R[1][0]) / s;
    } else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) {
        const double s = std::sqrt(1.0 + R[0][0] - R[1][1] - R[2][2]) * 2.0;   // 4x
        x = 0.25 * s;
        w = (R[1][2] - R[2][1]) / s;
        y = (R[0][1] + R[1][0]) / s;
        z = (R[0][2] + R[2][0]) / s;
    } else if (R[1][1] > R[2][2]) {
        const double s = std::sqrt(1.0 + R[1][1] - R[0][0] - R[2][2]) * 2.0;   // 4y
        y = 0.25 * s;
        w = (R[2][0] - R[0][2]) / s;
        x = (R[0][1] + R[1][0]) / s;
        z = (R[1][2] + R[2][1]) / s;
    } else {
        const double s = std::sqrt(1.0 + R[2][2] - R[0][0] - R[1][1]) * 2.0;   // 4z
        z = 0.25 * s;
        w = (R[0][1] - R[1][0]) / s;
        x = (R[0][2] + R[2][0]) / s;
        y = (R[1][2] + R[2][1]) / s;
    }
    const double n = std::sqrt(x * x + y * y + z * z + w * w);
    const double k = n > 1e-12 ? 1.0 / n : 1.0;
    q[0] = static_cast<float>(x * k);
    q[1] = static_cast<float>(y * k);
    q[2] = static_cast<float>(z * k);
    q[3] = static_cast<float>(w * k);
}
// The local record (pos vec4, quaternion) between an animated local matrix and a rest record: w 0 the animated one, w 1 the rest one exactly
// (the quaternion by the short-arc normalised lerp, the position by lerp).
inline void pfzBlendRecord(const float animLocal[16], const float rest[8], float w, float out[8]) noexcept {
    if (w >= 1.0f) {
        for (int i = 0; i < 8; ++i) out[i] = rest[i];
        return;
    }
    float qa[4];
    pfzQuatFromMatrix(animLocal, qa);
    const float* qr = rest + 4;
    const float dot = qa[0] * qr[0] + qa[1] * qr[1] + qa[2] * qr[2] + qa[3] * qr[3];
    const float sr = dot < 0.0f ? -1.0f : 1.0f;
    float q[4], n = 0.0f;
    for (int i = 0; i < 4; ++i) {
        q[i] = (1.0f - w) * qa[i] + w * sr * qr[i];
        n += q[i] * q[i];
    }
    n = n > 1e-12f ? 1.0f / std::sqrt(n) : 1.0f;
    for (int i = 0; i < 3; ++i) out[i] = (1.0f - w) * animLocal[12 + i] + w * rest[i];
    out[3] = 1.0f;
    for (int i = 0; i < 4; ++i) out[4 + i] = q[i] * n;
}
// The rest model matrix of joint idx: its rest local times every ancestor's (the uncached path 0xFDE0D0 computes). False on a bad index or a cycle.
inline bool pfzRestModel(const float* locals, const uint16_t* parents, uint32_t joints, uint32_t idx, float out[16]) noexcept {
    if (idx >= joints) return false;
    pfzLocalMatrix(locals + idx * ecm::kJointFloats, out);
    uint32_t steps = 0;
    for (uint32_t a = parents[idx]; a != 0xFFFFu; a = parents[a]) {
        if (a >= joints || ++steps > joints) return false;
        float pm[16], next[16];
        pfzLocalMatrix(locals + a * ecm::kJointFloats, pm);
        pfzMul(out, pm, next);
        std::memcpy(out, next, sizeof(next));
    }
    return true;
}

// The frozen set. why: 0 ok, 1 bad input (no joints, neck out of range, too many joints), 2 a parent out of range or a cycle, 3 the foot shares no
// ancestor with the neck, 4 the anchor is the neck itself, 5 F has more than kPfzMaxF members. footResolved false: the foot was not given and the
// anchor is the root's child on the neck's path.
struct PfzFrozenSet {
    uint32_t why = 1;
    uint32_t anchor = 0xFFFFu, child = 0xFFFFu, count = 0;
    bool footResolved = false;
    uint16_t member[kPfzMaxF] = {};      // parent first
    int16_t parentSlot[kPfzMaxF] = {};   // the member slot of each member's parent; -1 = the anchor
    int slotOf(uint32_t j) const noexcept {
        for (uint32_t k = 0; k < count; ++k)
            if (member[k] == j) return static_cast<int>(k);
        return -1;
    }
};
inline const char* pfzWhyText(uint32_t why) noexcept {
    switch (why) {
        case 0: return "ok";
        case 1: return "no joints, the neck out of range or more than 0xC0 joints";
        case 2: return "a parent out of range or a cycle";
        case 3: return "the foot shares no ancestor with the neck";
        case 4: return "the anchor is the neck itself";
        case 5: return "the frozen set has more than 128 members";
        default: return "?";
    }
}
inline PfzFrozenSet pfzFrozenSet(const uint16_t* parents, uint32_t joints, uint32_t neck, uint32_t foot) noexcept {
    PfzFrozenSet s;
    if (joints == 0 || joints > kPfzMaxJoints || neck >= joints) return s;
    // The neck's path to the root (neck first).
    uint16_t path[kPfzMaxJoints];
    uint32_t n = 0;
    for (uint32_t j = neck; j != 0xFFFFu; j = parents[j]) {
        if (j >= joints || n >= joints) {
            s.why = 2;
            return s;
        }
        path[n++] = static_cast<uint16_t>(j);
    }
    auto onPath = [&](uint32_t j) noexcept {
        for (uint32_t k = 0; k < n; ++k)
            if (path[k] == j) return static_cast<int>(k);
        return -1;
    };
    int at = -1;
    if (foot < joints) {
        s.footResolved = true;
        uint32_t steps = 0;
        for (uint32_t j = foot; j != 0xFFFFu; j = parents[j]) {
            if (j >= joints || ++steps > joints) {
                s.why = 2;
                return s;
            }
            at = onPath(j);
            if (at >= 0) break;
        }
        if (at < 0) {
            s.why = 3;
            return s;
        }
    } else {
        at = n >= 2 ? static_cast<int>(n - 2) : -1;   // the root's child on the neck's path
        if (at < 0) {
            s.why = 4;
            return s;
        }
    }
    if (at == 0) {
        s.why = 4;
        return s;
    }
    s.anchor = path[at];
    s.child = path[at - 1];
    // Members: every joint whose chain reaches the child; the depth below the child orders them parent first.
    uint32_t depth[kPfzMaxJoints];
    for (uint32_t j = 0; j < joints; ++j) {
        depth[j] = 0xFFFFFFFFu;
        uint32_t d = 0;
        for (uint32_t a = j; a != 0xFFFFu; a = parents[a]) {
            if (a >= joints || d > joints) break;
            if (a == s.child) {
                depth[j] = d;
                break;
            }
            ++d;
        }
    }
    uint32_t count = 0;
    for (uint32_t d = 0; d <= joints; ++d)
        for (uint32_t j = 0; j < joints; ++j) {
            if (depth[j] != d) continue;
            if (count >= kPfzMaxF) {
                s.why = 5;
                s.count = 0;
                return s;
            }
            s.member[count++] = static_cast<uint16_t>(j);
        }
    s.count = count;
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t p = parents[s.member[k]];
        s.parentSlot[k] = p == s.anchor ? static_cast<int16_t>(-1) : static_cast<int16_t>(s.slotOf(p));
    }
    s.why = 0;
    return s;
}

// The entry and exit blend: the weight moves toward 1 while wanted and toward 0 otherwise, over kPfzBlendSec.
struct PfzBlend {
    float w = 0.0f;
    void step(bool want, double dtSec) noexcept {
        const double d = dtSec > 0.0 ? dtSec / kPfzBlendSec : 0.0;
        double v = static_cast<double>(w) + (want ? d : -d);
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        w = static_cast<float>(v);
    }
};
// The F48 rest test on the animated head: not ADS, and the head's height within kSteadyM for kRestMs. resting holds until a sample leaves the band
// (or ADS); then the window starts again.
struct PfzHeadRest {
    static constexpr double kSteadyM = 0.02;
    static constexpr uint64_t kRestMs = 1000;
    bool resting = false;
    bool have = false;
    uint64_t winStart = 0;
    double lo = 0.0, hi = 0.0;
    bool step(uint64_t now, double headUp, bool ads) noexcept {
        if (ads) {
            resting = have = false;
            return false;
        }
        if (!have) {
            have = true;
            winStart = now;
            lo = hi = headUp;
            return resting = false;
        }
        const double nlo = headUp < lo ? headUp : lo, nhi = headUp > hi ? headUp : hi;
        if (nhi - nlo > kSteadyM) {
            winStart = now;
            lo = hi = headUp;
            return resting = false;
        }
        lo = nlo;
        hi = nhi;
        resting = now - winStart >= kRestMs;
        return resting;
    }
};
}  // namespace nofoot
}  // namespace edvr
