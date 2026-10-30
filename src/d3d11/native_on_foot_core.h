#pragma once
// Native on-foot VR, flight F0 (docs\design-native-onfoot-vr-2026-10-13.md, "Proposed first flight F0").
//
// Pure decisions and bytes: no Windows, no game, no Config, so a rig could drive them. native_on_foot.cpp holds the hook, the key,
// the journal reads and the log.
//
// THE SETTER. EliteDangerous64.exe build 334925 (retargeted from 332841; the build the avatar hooks pin; PE stamp and image size are avh::kExpectedTimestamp and
// avh::kExpectedImageSize) has a render-mode setter, a virtual function reached only through the vtable at image RVA 0x52EC798, slot 5
// (byte offset 0x28), at function RVA 0x2874920. Its signature is void(__fastcall*)(void* selector, uint32_t mode). It records the mode
// it was asked for in selector+0x20 (the source mode); selector+0x58 is the renderer owner, and owner+0x60 points back at selector.
// The modes come in pairs: each cinema mode (the flat screen shown in the headset) has an HMD partner (the headset's own stereo). The
// setter's remap at 0x2889940 uses the same pairs. Pair A is 5 (cinema) and 3 (HMD); pair B is 6 (cinema) and 4 (HMD). Flight F0 on
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
#include <limits>

#include "avatar_hooks_core.h"   // avh::kExpectedTimestamp and avh::kExpectedImageSize: the PE identity of the build the addresses belong to, shared with the other build-keyed hooks
#include "avatar_skeleton_core.h"   // F49: avh::quatRows, the pose's rotation rows (the pose freeze's rest locals)
#include "native_on_foot_f85.h"          // F85: the pin-step trace, the weapon UI pin and the reticle depth (pure parts)
#include "native_on_foot_f86.h"          // F86: the pose carry, the weapon UI depth (measure only since F87) and the floor probe (pure parts)
#include "native_on_foot_f87.h"          // F87: the standing-height hold, the stream windows' history, the scene rows' fallback, the prompt probes, the hand residual (pure parts)
#include "native_on_foot_f88.h"          // F88: the weapon eye offset, its weight, the pin's share and the hold's wait (pure parts)
#include "native_on_foot_f89.h"          // F89: the ADS transition trace, the hold's open-loop input (pure parts)
#include "native_on_foot_f106.h"         // F106: ADS, the sight in both eyes: the share's decision (pure parts; it replaced F89's left eye close)

namespace edvr {
namespace nofoot {

constexpr uintptr_t kSetterRva = 0x2874920;          // the render-mode setter's entry (EliteDangerous64.exe+0x2874920)
constexpr uintptr_t kSetterVtableRva = 0x52EC798;    // the vtable that holds the setter
constexpr size_t kSetterSlotOffset = 0x28;           // slot 5 of that vtable (5 * 8)
constexpr uint32_t kModeHmdA = 3;                    // HMD stereo, pair A
constexpr uint32_t kModeCinemaA = 5;                 // the flat cinema screen shown in the headset, pair A
constexpr uint32_t kModeHmdB = 4;                    // HMD stereo, pair B (the rig Sean flies on: the main menu runs in 4 too)
constexpr uint32_t kModeCinemaB = 6;                 // the flat cinema screen shown in the headset, pair B
constexpr size_t kSourceModeOffset = 0x20;           // selector+0x20: the source mode the setter writes
constexpr size_t kOwnerOffset = 0x58;                // selector+0x58: the renderer owner
constexpr size_t kOwnerSelectorOffset = 0x60;        // the selector is EMBEDDED in its owner at +0x60: owner+0x60 IS the selector (address arithmetic;
                                                     // nothing is read there). The selector's own first qword is the setter's vtable.

// The first 27 bytes of the setter, read from the executable (332841 and 334925 agree): `mov [rsp+10h],rbx; push rdi; sub rsp,20h; mov edi,edx;
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

// The identity check's answer for a PE stamp and image size: the build the addresses belong to (334925) or not.
inline bool identityMatches(uint32_t timestamp, uint32_t imageSize) {
    return timestamp == avh::kExpectedTimestamp && imageSize == avh::kExpectedImageSize;
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
// The override: int32 at EliteDangerous64.exe+0x5E85B94, -1 auto. The requester at 0x2889F95 reads it (sole reader; no writer in the image).
// The census: owner = [sel+0x58]; settings = [owner+0x1180]; RC = [settings+0x18]; fov = float [RC+0x16C]; mode = dword [settings+0x3694];
// selMode = dword [sel+0x20]; W = [settings+0x2CB8]; slot k at [W+0x90] (k = 0) or [W+0x98] (k = 1); F = [slot+0x2E0]; F70 = byte [F+0x70];
// node = [F+0x30]; fp_on = byte [node+0x30]; pg = [F+0x38]; pg250 = byte [pg+0x250].
constexpr uintptr_t kHudOverrideRva = 0x5E85B94;
constexpr uint32_t kCensusOwnerOff = 0x58, kCensusSettingsOff = 0x1180, kCensusRcOff = 0x18, kCensusFovOff = 0x16C, kCensusFov2Off = 0x17C, kCensusModeOff = 0x3694,
                   kCensusSelModeOff = 0x20, kCensusWOff = 0x2CB8, kCensusSlotA = 0x90, kCensusSlotB = 0x98, kCensusPassOff = 0x2E0,
                   kCensusF70Off = 0x70, kCensusNodeOff = 0x30, kCensusNodeOnOff = 0x30, kCensusPgOff = 0x38, kCensusPg250Off = 0x250;

// ---- F9 (TEMPORARY; native_on_foot.cpp holds the code). Two game code patches (approved by Sean 2026-10-14) and three read-only probes. ----
// Patch A (fix.native_on_foot_panels): 76 73 (jbe 0x19B0DEC) -> 90 90, in the panel gate of HumanoidPanelComponent (0x19B0CD0).
constexpr uintptr_t kPanelPatchRva = 0x19B0D77;
constexpr size_t kPanelPatchLen = 2;
constexpr uint8_t kPanelPatchFrom[2] = {0x76, 0x73};
constexpr uint8_t kPanelPatchTo[2] = {0x90, 0x90};
// Patch B (fix.native_on_foot_hud_mesh): 74 2A (jz +0x2A) -> EB 2A; the first byte only is written (the HRC camera-space placement branch).
constexpr uintptr_t kMeshPatchRva = 0x1A38AD3;
constexpr size_t kMeshPatchLen = 1;
constexpr uint8_t kMeshPatchFrom[2] = {0x74, 0x2A};
constexpr uint8_t kMeshPatchTo[2] = {0xEB, 0x2A};
// Site 2 of the mesh unit (F10): the HRC's creation gate. The six bytes at +0x1A16F46 are the call qword ptr [rax+0x80] on dep = [HRC+0x1D0]
// (the predicate in slot 0x1A8F950); the patch changes the call's displacement byte at +0x1A16F48 from 0x80 to 0x78 (the slot 0x78).
constexpr uintptr_t kMeshSite2Rva = 0x1A16F46;
constexpr uint8_t kMeshSite2Expect[6] = {0xFF, 0x90, 0x80, 0x00, 0x00, 0x00};
constexpr uint8_t kMeshSite2Patched[6] = {0xFF, 0x90, 0x78, 0x00, 0x00, 0x00};
constexpr size_t kMeshSite2ByteOff = 2;
// Weapon UI attachment orientation branch. Preserve the authored attachment basis while native on-foot VR is active.
constexpr uintptr_t kWeaponUiBranchRva = 0x1AB1969;
constexpr uintptr_t kWeaponUiVerifyRva = 0x1AB1960;
constexpr size_t kWeaponUiVerifyLen = 25, kWeaponUiBranchByteOff = 9;
constexpr uint8_t kWeaponUiBranchBefore[kWeaponUiVerifyLen] = {
    0xE8, 0x6B, 0x55, 0x6C, 0x01, 0x80, 0x7F, 0x28, 0x00, 0x74, 0x20, 0x45, 0x0F, 0x10, 0x0F, 0x45, 0x0F, 0x10, 0x57, 0x10, 0x45, 0x0F, 0x10, 0x5F, 0x20};
constexpr uint8_t kWeaponUiBranchAfter[kWeaponUiVerifyLen] = {
    0xE8, 0x6B, 0x55, 0x6C, 0x01, 0x80, 0x7F, 0x28, 0x00, 0xEB, 0x20, 0x45, 0x0F, 0x10, 0x0F, 0x45, 0x0F, 0x10, 0x57, 0x10, 0x45, 0x0F, 0x10, 0x5F, 0x20};
inline bool weaponUiPatchWanted(bool nativeOnFoot, bool session) noexcept { return nativeOnFoot && session; }
inline bool weaponUiBuildGetterCall(const uint8_t original[8], uintptr_t callAddress, uintptr_t target, uint8_t patched[8]) noexcept {
    if (!original || !patched || original[0] != 0xE8 || !callAddress || !target || callAddress > UINTPTR_MAX - 5) return false;
    const uintptr_t next = callAddress + 5;
    int32_t rel = 0;
    if (target >= next) {
        const uintptr_t distance = target - next;
        if (distance > static_cast<uintptr_t>(INT32_MAX)) return false;
        rel = static_cast<int32_t>(distance);
    } else {
        const uintptr_t distance = next - target;
        if (distance > static_cast<uintptr_t>(0x80000000ull)) return false;
        rel = distance == static_cast<uintptr_t>(0x80000000ull) ? INT32_MIN : -static_cast<int32_t>(distance);
    }
    for (int i = 0; i < 8; ++i) patched[i] = original[i];
    std::memcpy(patched + 1, &rel, sizeof(rel));
    return true;
}
inline bool patchBytesMatch(const uint8_t* actual, const uint8_t* expected, size_t len) noexcept {
    if (!actual || !expected || !len || len > 32) return false;
    for (size_t i = 0; i < len; ++i) if (actual[i] != expected[i]) return false;
    return true;
}
inline bool weaponUiGetterCallOwned(const uint8_t current[8], const uint8_t patched[8]) noexcept {
    return patchBytesMatch(current, patched, 8);
}

inline bool weaponUiGetterJoinMatches(uint32_t rowRenderFrame, uint32_t rowItemSerial, uint64_t rowRenderer, uint64_t rowPart, uint64_t rowInstance,
                                      uint64_t getterQpc, uint64_t submitQpc, uint32_t expectedRenderFrame, uint32_t expectedItemSerial,
                                      uint64_t expectedRenderer, uint64_t expectedPart, uint64_t expectedInstance,
                                      bool itemRead, bool fresh) noexcept {
    return itemRead && fresh && rowRenderFrame != 0 && rowItemSerial != 0 && rowRenderer != 0 && rowPart != 0 && rowInstance != 0 &&
           getterQpc != 0 && submitQpc != 0 && getterQpc <= submitQpc &&
           rowRenderFrame == expectedRenderFrame && rowItemSerial == expectedItemSerial &&
           rowRenderer == expectedRenderer && rowPart == expectedPart && rowInstance == expectedInstance;
}
// Probes. P1: panel submit (rcx = set, rdx = view, r8b = flag); P2: the HRC submit (six arguments; rcx = HRC); P3: the per-instance submit.
constexpr size_t kF9ProbeCount = 3;
constexpr size_t kF9Hrc = 3;
constexpr uintptr_t kPanelSubmitRva = 0x4717A80;
constexpr size_t kPanelSubmitStolen = 5;
constexpr size_t kPanelSubmitPrologueBytes = 32;
constexpr uint8_t kPanelSubmitPrologue[kPanelSubmitPrologueBytes] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20,
    0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x99, 0xB8, 0x01, 0x00};
constexpr uintptr_t kHrcSubmitRva = 0x1A16F10;
constexpr size_t kHrcSubmitStolen = 5;   // push rbp (2), push rbx (1), push r12 (2); the rip-relative cookie load at +0x13 is NOT stolen
constexpr size_t kHrcSubmitPrologueBytes = 26;
constexpr uint8_t kHrcSubmitPrologue[kHrcSubmitPrologueBytes] = {
    0x40, 0x55, 0x53, 0x41, 0x54, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xC1, 0x48, 0x81, 0xEC, 0xA8,
    0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x8E, 0x2A, 0x49, 0x04};
constexpr uintptr_t kInstSubmitRva = 0x434FF80;
constexpr size_t kInstSubmitStolen = 5;
constexpr size_t kInstSubmitPrologueBytes = 32;
constexpr uint8_t kInstSubmitPrologue[kInstSubmitPrologueBytes] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
    0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF9, 0x41, 0x8B, 0xD8, 0x48, 0x8B, 0x89, 0x50, 0x01, 0x00};
// The renderer scope provides the verified per-part table while the two AR callers submit instances.
constexpr uintptr_t kHumanoidArRenderRva = 0x1A168C0;
constexpr size_t kHumanoidArRenderStolen = 5, kHumanoidArRenderPrologueBytes = 32;
constexpr uint8_t kHumanoidArRenderPrologue[kHumanoidArRenderPrologueBytes] = {
    0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x49, 0x8D, 0xAB, 0x68, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x88,
    0x02, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xDE, 0x30, 0x49, 0x04, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x85};
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
// F112: the pilot's flown value is 0.19, the setting's step nearest the game's own 0.192.
constexpr float kHudDistanceSettingDefault = 0.19f;
// F13: the HUD size (the basis rows only), the head follow's smoothing (ms), and the visor override test (TEMPORARY).
// F16: size's default is 1.50 and its maximum 3.00 (Sean's tuning hit the old maximum); smoothing's default is 40 ms.
// F19: size's default is Sean's 1.60.
// F112: the pilot's flown value is 1.00.
constexpr float kHudSizeMin = 0.30f, kHudSizeMax = 3.00f, kHudSizeDefault = 1.00f;
constexpr int kHudSmoothMin = 0, kHudSmoothMax = 500, kHudSmoothDefault = 0;   // F34: the shipping default (Sean's tuning)
// F16: the per-group HUD offsets (a fraction of the HUD canvas, live), and the default spread (the margins' extra, F14).
// F19: the spread's default is Sean's 0.04.
constexpr float kHudOffsetMin = -0.30f, kHudOffsetMax = 0.30f;
constexpr float kHudSpreadDefault = 0.05f;   // F98c: the pilot's (0.04 before)
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
constexpr uintptr_t kVisorOverrideRva = 0x5F772E2;   // byte: 1 = the visor override is active (the RE: visorDown = byte[B7] ? byte[B8] : vfunc)
constexpr uintptr_t kVisorDownRva = 0x5F772E3;       // byte: the forced visor state while the override is active (0 = up)
// F17: the HUD element scale (the layout routine 0x1AB5B30, branch at 0x1AB5B91). The byte: non-zero means the layout scale S is the float below;
// zero means S = min(1, aspect / 1.7222), the game's own. The float: S itself (1.0 in the file). The user's setting is 0 (the game's own) or 0.30..2.00.
constexpr uintptr_t kElemScaleModeRva = 0x5F77202;   // byte
constexpr uintptr_t kElemScaleRva = 0x5E7D5E4;       // float
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
// F74: eye alignment translates the seated origin, so the camera moves opposite the requested local HMD-right vector.
// `rightM` is a signed distance in metres; exact zero writes exact zeros without requiring a valid basis.
// F134: the runtime's eye alignment is a SEATED-axes vector (it turns it into LOCAL by the seat's heading before it moves the origin), so the LOCAL translation
// -rightM * localRight is carried back into the seat's axes by the seat's heading `seatYawRad` (the seat convention, yaw + left: LOCAL = Ry(seatYaw) seated). Before F134
// this returned the LOCAL vector itself, which the runtime added to the LOCAL position unturned.
inline bool weaponViewOffsetSeated(double rightM, const double localRight[3], double seatYawRad, double out[3]) noexcept {
    if (!std::isfinite(rightM)) return false;
    if (rightM == 0.0) { out[0] = out[1] = out[2] = 0.0; return true; }
    if (!std::isfinite(seatYawRad)) return false;
    const double n2 = localRight[0] * localRight[0] + localRight[1] * localRight[1] + localRight[2] * localRight[2];
    if (!(n2 > 1e-12) || !std::isfinite(n2)) return false;
    const double inv = 1.0 / std::sqrt(n2);
    const double local[3] = {-rightM * localRight[0] * inv, -rightM * localRight[1] * inv, -rightM * localRight[2] * inv};
    ryApply(-seatYawRad, local, out);
    return true;
}
inline bool weaponViewOffsetActive(bool bodyPinScope, bool equipped, double rightM) noexcept {
    return bodyPinScope && equipped && std::isfinite(rightM) && rightM != 0.0;
}
// F74: the same signed offset in the game-world camera basis. Scene right is taken from the solved cyclopean eye separation.
inline bool weaponViewOffsetWorld(double rightM, const double sceneRight[3], double out[3]) noexcept {
    if (!std::isfinite(rightM)) return false;
    if (rightM == 0.0) { out[0] = out[1] = out[2] = 0.0; return true; }
    const double n2 = sceneRight[0] * sceneRight[0] + sceneRight[1] * sceneRight[1] + sceneRight[2] * sceneRight[2];
    if (!(n2 > 1e-12) || !std::isfinite(n2)) return false;
    const double inv = 1.0 / std::sqrt(n2);
    const double r[3] = {sceneRight[0] * inv, sceneRight[1] * inv, sceneRight[2] * inv};
    for (int i = 0; i < 3; ++i) out[i] = rightM * r[i];
    return true;
}
// F74: convert a world displacement into model axes with the inverse AMC matrix's linear part; translation is intentionally ignored.
inline void ikVectorXform(const double v[3], const float M[16], double out[3]) noexcept {
    for (int c = 0; c < 3; ++c) out[c] = v[0] * M[c] + v[1] * M[4 + c] + v[2] * M[8 + c];
}
inline void weaponViewOffsetTarget(double target[3], const double modelShift[3]) noexcept {
    if (modelShift[0] == 0.0 && modelShift[1] == 0.0 && modelShift[2] == 0.0) return;
    for (int i = 0; i < 3; ++i) target[i] += modelShift[i];
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
// F55: the same window also captures the 3P side's horizontal rest (body axes x and z: the neck, or the anchor's own point), but only at idle:
// the window restarts while the body moves (`moving`, the F49 locomotion gate) as well as on ADS and an unsteady head, and the key-on capture
// sets the height alone. hValid stays false until the first idle window completes (the align keeps the live x and z until then).
struct StandingHeight {
    static constexpr double kSteadyM = 0.02;     // the head's height range that counts as steady
    static constexpr uint64_t kRestMs = 1000;    // how long it must stay steady
    bool valid = false;
    double value = 0.0;                          // the captured standing height (body axes, up, metres)
    bool hValid = false;                         // F55: the horizontal rest was captured at idle
    double hx = 0.0, hz = 0.0;                   // F55: the captured rest, body axes right and forward (metres)
    uint32_t hCaptures = 0;
    uint64_t winStart = 0;
    double headMin = 0.0, headMax = 0.0, anchorSum = 0.0, sumX = 0.0, sumZ = 0.0;
    uint32_t n = 0;
    uint32_t captures = 0;
    void reset() noexcept { *this = StandingHeight{}; }
    // One frame: the 3P head's height and the 3P anchor's height (body axes, up), and the ADS flag. True when this step captured. F55: `moving`
    // (the body's horizontal speed gate) restarts the window like ADS; ax and az are the rest point's body x and z, averaged over the window.
    bool step(uint64_t now, double headUp, double anchorUp, bool ads, bool moving = false, double ax = 0.0, double az = 0.0) noexcept {
        if (ads) {   // ADS is never a rest (nor a key-on capture): the window starts again after it; the heights are not read
            n = 0;
            return false;
        }
        if (!valid) {
            valid = true;
            value = anchorUp;
            ++captures;
            restart(now, headUp, anchorUp, ax, az);
            if (moving) n = 0;
            return true;
        }
        if (moving) {   // F55: walking or running is never a rest
            n = 0;
            return false;
        }
        if (n == 0) {
            restart(now, headUp, anchorUp, ax, az);
            return false;
        }
        const double lo = headUp < headMin ? headUp : headMin, hi = headUp > headMax ? headUp : headMax;
        if (hi - lo > kSteadyM) {
            restart(now, headUp, anchorUp, ax, az);
            return false;
        }
        headMin = lo;
        headMax = hi;
        anchorSum += anchorUp;
        sumX += ax;
        sumZ += az;
        ++n;
        if (now - winStart < kRestMs) return false;
        value = anchorSum / static_cast<double>(n);
        hx = sumX / static_cast<double>(n);
        hz = sumZ / static_cast<double>(n);
        hValid = true;
        ++hCaptures;
        ++captures;
        restart(now, headUp, anchorUp, ax, az);
        return true;
    }
    void restart(uint64_t now, double headUp, double anchorUp, double ax = 0.0, double az = 0.0) noexcept {
        winStart = now;
        headMin = headMax = headUp;
        anchorSum = anchorUp;
        sumX = ax;
        sumZ = az;
        n = 1;
    }
};
// F55: the walking shift. The 3P side's source point with its horizontal taken from the captured rest plus the source's offset from the rest
// point (body axes; for the neck anchor the offset is -kNeckRestOffset), its vertical live. So d_h = FP side_h - (rest_h + offset_h) is constant
// in the body frame while the neck leans with locomotion and aim, and the torso leans with the legs.
inline void walkRestSource(const double live[3], const double offset[3], double restX, double restZ, double out[3]) noexcept {
    out[0] = restX + offset[0];
    out[1] = live[1];
    out[2] = restZ + offset[2];
}
// F48: the 3P side's point with its vertical (body axes, index 1) taken from the standing height; x and z live.
inline void standingApply(const double src[3], double standingUp, double out[3]) noexcept {
    out[0] = src[0];
    out[1] = standingUp;
    out[2] = src[2];
}
// F50: removes v's component along axis (normalised here) and returns it (signed, in v's units); nothing is removed when the axis is degenerate.
// The body's vertical lock: the body run's position and velocity lose their component along the body's up axis (AMC row 1, in world).
inline double dropAlong(double v[3], const double axis[3]) noexcept {
    const double n = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (!(n > 1e-6)) return 0.0;
    const double u[3] = {axis[0] / n, axis[1] / n, axis[2] / n};
    const double k = v[0] * u[0] + v[1] * u[1] + v[2] * u[2];
    for (size_t i = 0; i < 3; ++i) v[i] -= k * u[i];
    return k;
}

// ---- The view's raise (F51's seated y term; F66: the FP rig raise it served is gone, the eye height F64 keeps it) ---------------------------
// The view's share of the raise: a vertical-only seated offset. The calibration (body = diag(-1, -1, +1) . seated at psi 0) makes seated y = -dh
// raise the camera by dh; this is seatedFromBody({0, dh, 0}, 0).
inline void raiseSeated(double dh, double out[3]) noexcept {
    out[0] = 0.0;
    out[1] = -dh;
    out[2] = 0.0;
}
// F53: the neck anchor (the body align's default) with the rest geometry. The 3P neck's target is the FP side plus kNeckRestOffset in body axes
// (right, up, forward): the pov minus the neck at rest, negated. The F42/F46 path captured that offset live over the first second, so it
// carried the stance and the upper-body twist (the pov joint is a root child; the neck follows the stance). As a source for the align's
// (anchor - source) the neck is placed where the pov would be at rest: source = neck - kNeckRestOffset. With the body's height locked
// (vertical off, the default) only x and z of the target take effect.
constexpr double kNeckRestOffset[3] = {0.0, -0.155, -0.161};
// F62a: the eye-to-neck rest offset in full-3P body mode (hide_fp_body and body_unclip on: the head follow's source, the spine bend's target
// and the eye - neck residual): the neck sits 0.155 m below and 0.09 m behind the eye (0.161 m behind put the 3P head too far back). Outside
// full 3P the neck anchor keeps kNeckRestOffset, so nothing changes there.
constexpr double kNeckRestOffsetFull3P[3] = {0.0, -0.155, -0.09};
inline void neckRestSource(const double neckB[3], double out[3]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = neckB[i] - kNeckRestOffset[i];
}
inline void neckRestSource(const double neckB[3], const double offset[3], double out[3]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = neckB[i] - offset[i];
}
// The neck's target for an FP side (body axes): where the align puts the neck once d has converged.
inline void neckRestTarget(const double fpB[3], double out[3]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = fpB[i] + kNeckRestOffset[i];
}
// One frame's move of `applied` toward `target`, at most `cap` (the 5 cm a frame every published offset keeps).
inline double capToward(double applied, double target, double cap) noexcept {
    const double d = target - applied;
    return applied + (d > cap ? cap : d < -cap ? -cap : d);
}
// The shift rule the 3P body shift uses, for the FP records: a record moves only while its position equals the AMC origin bit for bit, so a second
// pass over a shifted record leaves it alone. True when it moved.
inline bool shiftIfAtOrigin(float pos[3], const float origin[3], const float d[3]) noexcept {
    if (std::memcmp(pos, origin, 3 * sizeof(float)) != 0) return false;
    for (size_t i = 0; i < 3; ++i) pos[i] += d[i];
    return true;
}
// F54: the held weapon. The MotionGraphModelComponent submit job (0x43F0500) writes one record per object at obj+0x230, its position at
// obj+0x240 and a copy at obj+0x354, rewritten from the instance's matrix every run (so an add after the job is not cumulative). The local
// first-person weapon is an object with a non-zero view mask (obj+0x380), the viewmodel byte (obj+0x1C8) set, and its record, before any
// shift, within kWeaponMatchM of either FP handAttach joint (51 R, 52 L; world, unshifted).
constexpr float kWeaponMatchM = 0.3f;
inline float dist3f(const float a[3], const float b[3]) noexcept {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
inline bool weaponIsLocalFp(uint64_t mask, uint8_t viewFlag, const float pos[3], const float attach[2][3], bool attachOk) noexcept {
    if (mask == 0 || viewFlag == 0 || !attachOk) return false;
    for (int s = 0; s < 2; ++s)
        if (dist3f(pos, attach[s]) <= kWeaponMatchM) return true;   // a NaN distance never matches
    return false;
}
// ---- F55: the FP X's clavicle subtree (the plan the Animate post-hook snapshots; F66: the shoulder drop that lowered it is gone) ----------
constexpr uint32_t kDropClavL = 19, kDropClavR = 20, kDropHandL = 37, kDropHandR = 44;
// The subtree of joints a and b (both included) over a parent array, in index order; joints whose parent chain is broken (out of range, or
// longer than the joint count) are left out. Returns the member count (at most cap).
inline uint32_t dropSubtree(const uint16_t* parents, uint32_t joints, uint32_t a, uint32_t b, uint16_t* out, uint32_t cap) noexcept {
    uint32_t count = 0;
    for (uint32_t j = 0; j < joints && count < cap; ++j) {
        uint32_t p = j;
        bool in = false;
        for (uint32_t depth = 0; depth <= joints; ++depth) {
            if (p == a || p == b) {
                in = true;
                break;
            }
            if (p >= joints) break;
            const uint16_t up = parents[p];
            if (up == 0xFFFFu || up >= joints) break;
            p = up;
        }
        if (in) out[count++] = static_cast<uint16_t>(j);
    }
    return count;
}

// F51: the pose freeze's gate: no locomotion. The body's horizontal speed (its world position less the part along its up axis), low-passed
// (tau kTauSec), under kMoveMps for kStillMs; ADS ends it (ADS stays excluded). A weapon out does not end it. Fed only new samples.
struct PfzLocoRest {
    static constexpr double kMoveMps = 0.35;
    static constexpr double kTauSec = 0.2;
    static constexpr uint64_t kStillMs = 300;
    bool have = false, resting = false;
    double p[3] = {}, speed = 0.0;
    uint64_t t = 0, stillSince = 0;
    bool step(uint64_t now, const double pos[3], const double up[3], bool ads) noexcept {
        if (!have) {
            have = true;
            for (size_t i = 0; i < 3; ++i) p[i] = pos[i];
            t = stillSince = now;
            speed = 0.0;
            return resting = false;
        }
        if (now > t) {
            const double dtS = static_cast<double>(now - t) / 1000.0;
            double d[3] = {pos[0] - p[0], pos[1] - p[1], pos[2] - p[2]};
            dropAlong(d, up);
            const double v = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / dtS;
            speed += (1.0 - std::exp(-dtS / kTauSec)) * (v - speed);
            for (size_t i = 0; i < 3; ++i) p[i] = pos[i];
            t = now;
        }
        if (ads || speed > kMoveMps) {
            stillSince = now;
            return resting = false;
        }
        return resting = now - stillSince >= kStillMs;
    }
};

// ---- F49 (TEMPORARY): the pose freeze, pure half -------------------------------------------------------------------------------
// The frAnim Animate handler writes one 64-byte row-vector model matrix per joint: rows 0..2 the rotation (w 0), row 3 the position (w 1), and
// out[j] = local[j] x out[parent(j)]. The rest local of a joint is the pose's 32-byte record (position vec4, then the quaternion) as R(q) with the
// position in row 3, R(q) laid out as avh::quatRows. The frozen set F is the torso-side child subtree of the lowest common ancestor of the neck and
// the left foot (the hips); each member's matrix is its local (rest, or blended toward rest) times its parent's new matrix, anchored at the
// animated matrix of the hips.
constexpr uint32_t kPfzMaxF = 0xC0;          // the frozen set's member cap (F53: 192, the joint cap; F51/F52's 128 refused the 185-member set)
constexpr uint32_t kPfzMaxJoints = 0xC0;     // the Animate output's joint cap (the span reader's min(count, 0xC0))
static_assert(kPfzMaxF >= kPfzMaxJoints, "F53: F can hold every joint the Animate output has (the member cap is never the refusal)");
constexpr double kPfzBlendSec = 0.3;         // the entry and exit blend

// Set an affine row-major transform to identity.
inline void pfzIdentity(float out[16]) noexcept {
    for (int i = 0; i < 16; ++i) out[i] = 0.0f;
    out[0] = out[5] = out[10] = out[15] = 1.0f;
}
// The rest local matrix of a pose record (pos vec4 at +0, quaternion at +0x10).
inline void pfzLocalMatrix(const float rec[8], float out[16]) noexcept {
    float r[9];
    avh::quatRows(rec + 4, r);
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
// Validate only the 12 values used by affine point/vector transforms. The remaining
// four elements are non-semantic padding and may be uninitialized by the game.
inline bool pfzAffineElementsFinite(const float m[16]) noexcept {
    if (!m) return false;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) if (!std::isfinite(m[r * 4 + c])) return false;
    for (int c = 0; c < 3; ++c) if (!std::isfinite(m[12 + c])) return false;
    return true;
}
inline bool pfzCanonicalizeAffineMatrix(const float m[16], float out[16], bool* paddingFinite = nullptr) noexcept {
    if (paddingFinite) *paddingFinite = false;
    if (!m || !out || !pfzAffineElementsFinite(m)) return false;
    bool finitePadding = true;
    for (int r = 0; r < 4; ++r) if (!std::isfinite(m[r * 4 + 3])) finitePadding = false;
    if (paddingFinite) *paddingFinite = finitePadding;
    for (int i = 0; i < 16; ++i) out[i] = m[i];
    out[3] = out[7] = out[11] = 0.0f;
    out[15] = 1.0f;
    return true;
}
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
// The quaternion (x, y, z, w) of a rotation in the upper 3x3 (the inverse of avh::quatRows; each row is normalised first, so a uniform scale drops out).
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
// The rest model matrix of joint idx: its rest local times every ancestor's (the uncached path 0xFDF170 computes). False on a bad index or a cycle.
inline bool pfzRestModel(const float* locals, const uint16_t* parents, uint32_t joints, uint32_t idx, float out[16]) noexcept {
    if (idx >= joints) return false;
    pfzLocalMatrix(locals + idx * avh::kJointFloats, out);
    uint32_t steps = 0;
    for (uint32_t a = parents[idx]; a != 0xFFFFu; a = parents[a]) {
        if (a >= joints || ++steps > joints) return false;
        float pm[16], next[16];
        pfzLocalMatrix(locals + a * avh::kJointFloats, pm);
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
        case 5: return "the frozen set has more than 192 members";
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

// ---- F56 (TEMPORARY): the third-person arm IK, pure half ----------------------------------------------------------------------
// fix.native_on_foot_arm_ik = off | probe (the default) | nudge | on. In body mode the 3P hands are put on the FP handAttach joints (51 R, 52 L,
// mapped into the 3P model space) by a two-bone solve per arm in the Animate post-hook. Row-vector 4x4 throughout (v' = v x M; rows 0..2 the
// rotation, row 3 the position). A 3x3 rotation R here is row-vector too: v' = v x R.
constexpr double kIkRampSec = 0.2;                            // the base weight's ramp in and out
constexpr float kIkGateFullM = 0.35f, kIkGateZeroM = 0.8f;    // per arm: full weight under 0.35 m from the animated hand, none from 0.8 m
constexpr float kIkCapPerCallM = 0.05f;                       // the applied hand offset moves at most 5 cm an Animate call (a frame)
constexpr float kIkNudgeM = 0.25f;                            // nudge: the left hand group rises this much (model +y)
constexpr uint64_t kIkFreshMs = 100;                          // the FP snapshot and G older than this refuse the call
constexpr uint32_t kIkGroupMax = 64;                          // the member cap of each group (gU, gF, gH)
constexpr double kIkElbowOutM = 0.06;                         // the swivel clamp: the elbow at least this far out of its OWN shoulder (F59; model x)
constexpr double kIkPi = 3.14159265358979323846;

// The per-arm distance gate: 1 under kIkGateFullM, linear to 0 at kIkGateZeroM, 0 beyond (and for NaN).
inline float ikDistanceGate(float dist) noexcept {
    if (!(dist == dist)) return 0.0f;
    if (dist <= kIkGateFullM) return 1.0f;
    if (dist >= kIkGateZeroM) return 0.0f;
    return (kIkGateZeroM - dist) / (kIkGateZeroM - kIkGateFullM);
}
// The base weight's ramp: toward 1 while wanted and toward 0 otherwise, linearly over `ramp` seconds (kIkRampSec; F57's square-up 0.3 s).
struct IkBlend {
    float w = 0.0f;
    double ramp = kIkRampSec;
    void step(bool want, double dtSec) noexcept {
        const double d = dtSec > 0.0 && ramp > 0.0 ? dtSec / ramp : 0.0;
        double v = static_cast<double>(w) + (want ? d : -d);
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        w = static_cast<float>(v);
    }
};
// One call's move of the applied offset toward the wanted one: the change is at most `cap` long (a vector cap).
inline void ikCapToward(float applied[3], const float want[3], float cap) noexcept {
    const float d[3] = {want[0] - applied[0], want[1] - applied[1], want[2] - applied[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float k = len > cap && len > 0.0f ? cap / len : 1.0f;
    for (int i = 0; i < 3; ++i) applied[i] += d[i] * k;
}

// The arm chain on a parent array: C the clavicle, H the hand, A the hand attach (from the joint probe). F = parents[H], U = parents[F] when
// parents[U] == C (direct); else U is the ancestor of H (up to 4 levels) whose parent is C. Groups, parent first: gU = subtree(U) less
// subtree(F) (U and any joint between U and F are the chain, chainU), gF = subtree(F) less subtree(H), gH = subtree(H) (A in it).
// why: 0 ok, 1 bad input, 2 no ancestor of H within 4 levels has the parent C (or U would be F), 3 A is not under H, 4 a group past kIkGroupMax,
// 5 a parent out of range or a cycle.
struct IkArm {
    uint32_t why = 1;
    uint16_t C = 0xFFFFu, U = 0xFFFFu, F = 0xFFFFu, H = 0xFFFFu, A = 0xFFFFu;
    bool direct = false, attachDirect = false;
    uint32_t depthC = 0, depthH = 0;
    uint16_t nU = 0, nF = 0, nH = 0;
    uint16_t gU[kIkGroupMax] = {}, gF[kIkGroupMax] = {}, gH[kIkGroupMax] = {};
    uint8_t chainU[kIkGroupMax] = {};
};
// The depth of j (the root 0); 0xFFFFFFFF on a broken chain.
inline uint32_t ikDepth(const uint16_t* parents, uint32_t joints, uint32_t j) noexcept {
    uint32_t d = 0;
    for (uint32_t a = j;; ++d) {
        if (a >= joints || d > joints) return 0xFFFFFFFFu;
        const uint16_t p = parents[a];
        if (p == 0xFFFFu) return d;
        a = p;
    }
}
// True when j is root or below it.
inline bool ikUnder(const uint16_t* parents, uint32_t joints, uint32_t j, uint32_t root) noexcept {
    uint32_t steps = 0;
    for (uint32_t a = j; a != 0xFFFFu; a = parents[a]) {
        if (a >= joints || ++steps > joints + 1) return false;
        if (a == root) return true;
    }
    return false;
}
inline IkArm ikArmChain(const uint16_t* parents, uint32_t joints, uint32_t C, uint32_t H, uint32_t A) noexcept {
    IkArm s;
    if (joints == 0 || joints > kPfzMaxJoints || C >= joints || H >= joints || A >= joints) return s;
    s.C = static_cast<uint16_t>(C);
    s.H = static_cast<uint16_t>(H);
    s.A = static_cast<uint16_t>(A);
    s.depthC = ikDepth(parents, joints, C);
    s.depthH = ikDepth(parents, joints, H);
    if (s.depthC == 0xFFFFFFFFu || s.depthH == 0xFFFFFFFFu) {
        s.why = 5;
        return s;
    }
    const uint32_t F = parents[H];
    if (F >= joints) {
        s.why = 2;
        return s;
    }
    uint32_t U = parents[F];
    s.direct = U < joints && parents[U] == C;
    if (!s.direct) {
        U = 0xFFFFu;
        uint32_t a = H;
        for (int level = 0; level < 4; ++level) {
            const uint32_t p = parents[a];
            if (p >= joints) break;
            if (parents[p] == C) {
                U = p;
                break;
            }
            a = p;
        }
    }
    if (U >= joints || U == F || U == H) {
        s.why = 2;
        return s;
    }
    s.U = static_cast<uint16_t>(U);
    s.F = static_cast<uint16_t>(F);
    if (!ikUnder(parents, joints, A, H)) {
        s.why = 3;
        return s;
    }
    s.attachDirect = parents[A] == H;
    // Parent first: by depth, then index.
    const uint32_t dU = ikDepth(parents, joints, U);
    for (uint32_t d = dU; d <= joints; ++d) {
        bool any = false;
        for (uint32_t j = 0; j < joints; ++j) {
            if (!ikUnder(parents, joints, j, U)) continue;
            const uint32_t dj = ikDepth(parents, joints, j);
            if (dj > d) any = true;
            if (dj != d) continue;
            const bool inH = ikUnder(parents, joints, j, H), inF = !inH && ikUnder(parents, joints, j, F);
            uint16_t& n = inH ? s.nH : inF ? s.nF : s.nU;
            if (n >= kIkGroupMax) {
                s.why = 4;
                return s;
            }
            if (inH) {
                s.gH[n++] = static_cast<uint16_t>(j);
            } else if (inF) {
                s.gF[n++] = static_cast<uint16_t>(j);
            } else {
                s.chainU[n] = ikUnder(parents, joints, F, j) ? 1 : 0;
                s.gU[n++] = static_cast<uint16_t>(j);
            }
        }
        if (!any) break;
    }
    s.why = 0;
    return s;
}
inline const char* ikArmWhyText(uint32_t why) noexcept {
    switch (why) {
        case 0: return "ok";
        case 1: return "a joint index out of range or no joints";
        case 2: return "no ancestor of the hand within 4 levels has the clavicle as its parent";
        case 3: return "the attach joint is not under the hand";
        case 4: return "a group has more than 64 members";
        case 5: return "a parent out of range or a cycle";
        default: return "?";
    }
}

// Small double vector helpers.
inline double ikDot(const double a[3], const double b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double ikLen(const double a[3]) noexcept { return std::sqrt(ikDot(a, a)); }
inline void ikCross(const double a[3], const double b[3], double out[3]) noexcept {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
inline bool ikUnit(const double a[3], double out[3]) noexcept {
    const double l = ikLen(a);
    if (!(l > 1e-9)) return false;
    for (int i = 0; i < 3; ++i) out[i] = a[i] / l;
    return true;
}
inline double ikWrapPi(double a) noexcept {
    if (!(a == a)) return 0.0;
    while (a > kIkPi) a -= 2.0 * kIkPi;
    while (a < -kIkPi) a += 2.0 * kIkPi;
    return a;
}

// The two-bone frame: S the shoulder (the upper arm's root), T the target hand position, l1 the upper arm, l2 the forearm. D = |T - S| clamped
// into [|l1 - l2| + 1 mm, 0.999 (l1 + l2)]; dir = unit(T - S); the elbow circle has its centre at S + dir a and radius h; u is the unit
// perpendicular of `ref` to dir (any perpendicular when ref is parallel), v = dir x u. The elbow at swivel theta is S + dir a + h (cos u + sin v);
// the hand the solve reaches is S + dir D (T itself while reach < 1). reach = |T - S| / (l1 + l2); shortfall = |T - S| - D when positive (m).
struct IkTwoBone {
    bool ok = false;
    bool limited = false;   // F57: D differs from |T - S| (a reach or an elbow-angle limit moved the hand off T)
    double S[3] = {}, dir[3] = {}, u[3] = {}, v[3] = {}, hand[3] = {};
    double D = 0.0, a = 0.0, h = 0.0, reach = 0.0, shortfall = 0.0;
    void elbowAt(double theta, double E[3]) const noexcept {
        const double c = std::cos(theta), s = std::sin(theta);
        for (int i = 0; i < 3; ++i) E[i] = S[i] + dir[i] * a + h * (c * u[i] + s * v[i]);
    }
    // The swivel angle (about dir, from u) of a direction p.
    double angleOf(const double p[3]) const noexcept { return std::atan2(ikDot(p, v), ikDot(p, u)); }
};
// F57: the D range of the elbow's interior angle limits (law of cosines: D^2 = l1^2 + l2^2 - 2 l1 l2 cos(angle)), inside the fold and reach
// limits [|l1 - l2| + 1 mm, 0.999 (l1 + l2)]. minDeg 0 and maxDeg 180 give the fold and reach limits alone.
inline void ikElbowDRange(double l1, double l2, double minDeg, double maxDeg, double* lo, double* hi) noexcept {
    const double c1 = std::cos(minDeg * kIkPi / 180.0), c2 = std::cos(maxDeg * kIkPi / 180.0);
    const double dMin = std::sqrt(std::max(0.0, l1 * l1 + l2 * l2 - 2.0 * l1 * l2 * c1));
    const double dMax = std::sqrt(std::max(0.0, l1 * l1 + l2 * l2 - 2.0 * l1 * l2 * c2));
    *lo = std::max(std::fabs(l1 - l2) + 1e-3, dMin);
    *hi = std::min(0.999 * (l1 + l2), dMax);
    if (*hi < *lo) *hi = *lo;
}
// F57: the elbow's interior angle (degrees, 180 straight) for a shoulder-to-hand distance D.
inline double ikElbowInteriorDeg(double l1, double l2, double D) noexcept {
    if (!(l1 > 1e-9) || !(l2 > 1e-9)) return 0.0;
    double c = (l1 * l1 + l2 * l2 - D * D) / (2.0 * l1 * l2);
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    return std::acos(c) * 180.0 / kIkPi;
}
// The frame with D clamped into [lo, hi] (F57: the elbow angle limits through ikElbowDRange).
inline IkTwoBone ikTwoBoneFrameD(const double S[3], const double T[3], double l1, double l2, const double ref[3], double lo, double hi) noexcept {
    IkTwoBone t;
    if (!(l1 > 1e-4) || !(l2 > 1e-4) || !(hi >= lo)) return t;
    const double dv[3] = {T[0] - S[0], T[1] - S[1], T[2] - S[2]};
    const double len = ikLen(dv);
    if (!(len > 1e-6)) return t;
    for (int i = 0; i < 3; ++i) {
        t.S[i] = S[i];
        t.dir[i] = dv[i] / len;
    }
    t.D = len < lo ? lo : len > hi ? hi : len;
    t.limited = t.D != len;
    t.reach = len / (l1 + l2);
    t.shortfall = len > t.D ? len - t.D : 0.0;
    t.a = (l1 * l1 - l2 * l2 + t.D * t.D) / (2.0 * t.D);
    t.h = std::sqrt(std::max(0.0, l1 * l1 - t.a * t.a));
    double perp[3];
    const double rd = ikDot(ref, t.dir);
    for (int i = 0; i < 3; ++i) perp[i] = ref[i] - t.dir[i] * rd;
    if (!ikUnit(perp, t.u)) {   // ref along dir: any perpendicular
        const double axis[3] = {std::fabs(t.dir[0]) < 0.9 ? 1.0 : 0.0, std::fabs(t.dir[0]) < 0.9 ? 0.0 : 1.0, 0.0};
        double c[3];
        ikCross(t.dir, axis, c);
        if (!ikUnit(c, t.u)) return t;
    }
    ikCross(t.dir, t.u, t.v);
    for (int i = 0; i < 3; ++i) t.hand[i] = S[i] + t.dir[i] * t.D;
    t.ok = true;
    return t;
}
// The frame under the elbow's interior angle limits [minDeg, maxDeg] (0 and 180, the defaults: the fold and reach limits alone, as F56).
inline IkTwoBone ikTwoBoneFrame(const double S[3], const double T[3], double l1, double l2, const double ref[3], double minDeg = 0.0,
                                double maxDeg = 180.0) noexcept {
    double lo = 0.0, hi = 0.0;
    ikElbowDRange(l1, l2, minDeg, maxDeg, &lo, &hi);
    return ikTwoBoneFrameD(S, T, l1, l2, ref, lo, hi);
}
// The classic two-bone solve: the elbow on the pole's side of S->T (swivel 0 of the frame built on the pole).
inline IkTwoBone ikTwoBone(const double S[3], const double T[3], double l1, double l2, const double pole[3], double E[3]) noexcept {
    const IkTwoBone t = ikTwoBoneFrame(S, T, l1, l2, pole);
    if (t.ok) t.elbowAt(0.0, E);
    return t;
}
// The swivel clamp: the elbow at least kIkElbowOutM out on the arm's side (out = +1 or -1: the sign of the clavicle's model x) and not above
// the shoulder (E.y <= S.y). Valid authored pistol support uses a separate body-down pole for the left elbow; the slider sets its starting
// lateral angle and swivel trim is added. Other policies retain this clamp and its nearest-angle/least-violation fallback.
// F57: the elbow-out distance is the Elbow out constant, kIkElbowOutCm (F96: its config key was removed). F59: measured from the arm's OWN shoulder,
// out x (E.x - S.x) >= elbowOutM (3.5 cm since F96), not from the midline (F58's flight: the left clamp fired 54 to 100% against the midline).
inline double ikElbowOutOf(const double E[3], const double S[3], double out) noexcept { return out * (E[0] - S[0]); }
// Preserve the existing 3P model-X half-space convention by anatomical rig side. The rig is translated
// during pinStage0, so a joint's current absolute X is not a stable indicator of which arm it belongs to.
inline double ikArmOutSign(int side) noexcept { return side == 0 ? -1.0 : side == 1 ? 1.0 : 0.0; }
inline double ikSwivelClamp(const IkTwoBone& t, double theta, double out, bool* clamped, double elbowOutM = kIkElbowOutM) noexcept {
    if (clamped) *clamped = false;
    if (!t.ok || !(t.h > 1e-6)) return theta;
    const auto violation = [&](double th) noexcept {
        double E[3];
        t.elbowAt(th, E);
        const double vo = elbowOutM - ikElbowOutOf(E, t.S, out), vy = E[1] - t.S[1];
        return (vo > 0.0 ? vo : 0.0) + (vy > 0.0 ? vy : 0.0);
    };
    if (violation(theta) <= 0.0) return theta;
    if (clamped) *clamped = true;
    const double step = 5.0 * kIkPi / 180.0;
    double best = theta, bestV = violation(theta);
    for (int k = 1; k <= 36; ++k) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const double th = theta + sgn * k * step;
            const double vv = violation(th);
            if (vv <= 0.0) return ikWrapPi(th);
            if (vv < bestV) {
                bestV = vv;
                best = th;
            }
        }
    }
    return ikWrapPi(best);
}
// Shared F57 legacy swivel policy, used by the runtime and F84's candidate replay.
inline double ikTrimSign(const IkTwoBone& t, double theta, double out) noexcept;
inline double ikLegacySwivel(const IkTwoBone& tb, const double elbowDir[3], double out, const double* fpPole, double trimRad,
                             double elbowOutM, bool* clamped) noexcept {
    const double hint[3] = {0.6 * out, -0.8, -0.1};
    double pole[3], pp[3];
    for (int i = 0; i < 3; ++i) pole[i] = fpPole ? fpPole[i] : 0.5 * elbowDir[i] + 0.5 * hint[i];
    double pd = ikDot(pole, tb.dir);
    for (int i = 0; i < 3; ++i) pp[i] = pole[i] - tb.dir[i] * pd;
    if (ikLen(pp) < 0.2) {
        pd = ikDot(hint, tb.dir);
        for (int i = 0; i < 3; ++i) pp[i] = hint[i] - tb.dir[i] * pd;
    }
    double th = tb.angleOf(pp);
    if (trimRad != 0.0) th = ikWrapPi(th + ikTrimSign(tb, th, out) * trimRad);
    return ikSwivelClamp(tb, th, out, clamped, elbowOutM);
}

// F76: a physical elbow pole in 3P model/body axes. `reference` is the anatomical direction transported between successful shoulder->hand axes;
// `previous` is the last solved radial, used only when transporting the anatomical direction fails numerically.
struct IkPhysicalPole {
    bool valid = false;
    double axis[3] = {}, reference[3] = {}, previous[3] = {};
};
struct IkPhysicalPoleSet {
    uint32_t generation = 0xFFFFFFFFu;
    uint64_t item = 0;
    bool active = false;
    IkPhysicalPole side[2]{};
};
inline void ikPhysicalPoleReset(IkPhysicalPoleSet& set) noexcept {
    set.active = false;
    set.side[0] = {};
    set.side[1] = {};
}
inline void ikPhysicalPoleResetSide(IkPhysicalPoleSet& set, int side) noexcept {
    if (side >= 0 && side < 2) set.side[side] = {};
}
inline bool ikPhysicalPoleBegin(IkPhysicalPoleSet& set, uint32_t generation, uint64_t item, bool active) noexcept {
    const bool reset = !active || !set.active || set.generation != generation || set.item != item;
    if (reset) ikPhysicalPoleReset(set);
    set.generation = generation;
    set.item = item;
    set.active = active;
    return reset;
}
inline bool ikPhysicalPoleSideEligible(int side, bool supportCupTargetValid, bool offHandTwoHanded) noexcept {
    return side == 1 || (side == 0 && (supportCupTargetValid || offHandTwoHanded));
}
// Keep the exact-rifle physical-pole reference alive while F84 releases its down-pole blend;
// identity and full ADS remain hard boundaries so unrelated items never inherit the seed.
inline bool ikRifleQualityKeepsPhysicalPole(bool exactRifle, double qualityWeight, double detachWeight, bool seedValid) noexcept {
    return exactRifle && seedValid && std::isfinite(qualityWeight) && std::isfinite(detachWeight) &&
           qualityWeight > 0.0 && detachWeight < 1.0;
}
struct IkStableSwivel {
    bool ok = false, usedFallback = false, transportedReference = false;
    double theta = 0.0, violation = 0.0, axis[3] = {}, reference[3] = {}, radial[3] = {};
};
inline void ikRotAxis(const double n[3], double ang, double R[9]) noexcept;
inline bool ikElbowRadial(const IkTwoBone& t, double theta, double out[3]) noexcept {
    if (!t.ok || !(t.h > 1e-6) || !std::isfinite(theta)) return false;
    const double c = std::cos(theta), s = std::sin(theta);
    for (int i = 0; i < 3; ++i) out[i] = c * t.u[i] + s * t.v[i];
    return ikUnit(out, out);
}
inline IkStableSwivel ikStableSwivel(const IkTwoBone& t, const IkPhysicalPole& pole, double outSign, double elbowOutM = kIkElbowOutM) noexcept {
    IkStableSwivel result;
    if (!t.ok || !(t.h > 1e-6) || !(outSign == 1.0 || outSign == -1.0) || !std::isfinite(elbowOutM)) return result;
    const auto transport = [&](const double source[3], double transported[3]) noexcept {
        double from[3], to[3];
        if (!ikUnit(pole.axis, from) || !ikUnit(t.dir, to)) return false;
        const double d = std::max(-1.0, std::min(1.0, ikDot(from, to)));
        double cross[3];
        ikCross(from, to, cross);
        const double sine = ikLen(cross);
        if (sine < 1e-9) {
            // At an exact antipode, rotating about the transported pole itself preserves the pole and maps the axis to its negative.
            if (d < 0.0) {
                if (!ikUnit(source, transported)) return false;
            } else {
                std::memcpy(transported, source, sizeof(double) * 3);
            }
        } else {
            double R[9];
            const double axis[3] = {cross[0] / sine, cross[1] / sine, cross[2] / sine};
            ikRotAxis(axis, std::atan2(sine, d), R);
            for (int c = 0; c < 3; ++c) transported[c] = source[0] * R[c] + source[1] * R[3 + c] + source[2] * R[6 + c];
        }
        const double along = ikDot(transported, to);
        for (int i = 0; i < 3; ++i) transported[i] -= along * to[i];
        return ikUnit(transported, transported);
    };
    double preferred[3] = {};
    if (pole.valid && transport(pole.reference, preferred)) {
        result.transportedReference = true;
    } else {
        result.usedFallback = true;
        if (!(pole.valid && transport(pole.previous, preferred))) {
            const double downOut[3] = {outSign, -1.0, 0.0};
            const double d = ikDot(downOut, t.dir);
            for (int i = 0; i < 3; ++i) preferred[i] = downOut[i] - d * t.dir[i];
            if (ikLen(preferred) < 1e-9 || !ikUnit(preferred, preferred)) {
                const double forward[3] = {0.0, 0.0, 1.0};
                const double fd = ikDot(forward, t.dir);
                for (int i = 0; i < 3; ++i) preferred[i] = forward[i] - fd * t.dir[i];
                if (!ikUnit(preferred, preferred)) return result;
            }
        }
    }
    std::memcpy(result.axis, t.dir, sizeof(result.axis));
    std::memcpy(result.reference, preferred, sizeof(result.reference));
    const double preferredTheta = t.angleOf(preferred);

    // Deficits are nonpositive exactly when each physical constraint holds. Both are linear in cos(theta), sin(theta).
    const double cDef[2] = {elbowOutM - outSign * t.dir[0] * t.a, t.dir[1] * t.a};
    const double aDef[2] = {-t.h * outSign * t.u[0], t.h * t.u[1]};
    const double bDef[2] = {-t.h * outSign * t.v[0], t.h * t.v[1]};
    double candidate[20] = {};
    int count = 0;
    const auto add = [&](double theta) noexcept {
        if (!std::isfinite(theta) || count >= static_cast<int>(sizeof(candidate) / sizeof(candidate[0]))) return;
        candidate[count++] = ikWrapPi(theta);
    };
    add(preferredTheta);
    for (int k = 0; k < 2; ++k) {
        const double radius = std::hypot(aDef[k], bDef[k]);
        if (radius > 1e-12 && std::fabs(cDef[k]) <= radius + 1e-12) {
            const double phase = std::atan2(bDef[k], aDef[k]);
            const double offset = std::acos(std::max(-1.0, std::min(1.0, -cDef[k] / radius)));
            add(phase + offset);
            add(phase - offset);
        }
    }
    // The minimum of the sum of any active subset of the two deficits is a candidate for the global minimum of max(0,d0)+max(0,d1).
    for (int mask = 1; mask < 4; ++mask) {
        double aa = 0.0, bb = 0.0;
        for (int k = 0; k < 2; ++k) if (mask & (1 << k)) { aa += aDef[k]; bb += bDef[k]; }
        if (std::hypot(aa, bb) > 1e-12) add(std::atan2(bb, aa) + kIkPi);
    }

    double bestDistance = std::numeric_limits<double>::infinity();
    bool bestFeasible = false;
    for (int i = 0; i < count; ++i) {
        const double theta = candidate[i];
        double E[3];
        t.elbowAt(theta, E);
        const double violation = std::max(0.0, elbowOutM - ikElbowOutOf(E, t.S, outSign)) + std::max(0.0, E[1] - t.S[1]);
        const bool feasible = violation <= 1e-9;
        const double distance = std::fabs(ikWrapPi(theta - preferredTheta));
        const bool better = (feasible && !bestFeasible) || (feasible == bestFeasible &&
            (feasible ? distance < bestDistance - 1e-10 :
                (violation < result.violation - 1e-10 || (std::fabs(violation - result.violation) <= 1e-10 && distance < bestDistance - 1e-10))));
        if (!result.ok || better) {
            result.ok = true;
            result.theta = theta;
            result.violation = violation;
            bestFeasible = feasible;
            bestDistance = distance;
            if (!ikElbowRadial(t, theta, result.radial)) result.ok = false;
        }
    }
    return result;
}

// F81: the authored left pistol elbow stays below the shoulder and points down/out in the 3P body frame.
// This intentionally replaces only the old model-X half-space for that policy. The lateral basis is
// axis x up: with the documented body axes (+Z forward, +Y up, -X left), it points left for a forward arm
// and rotates continuously with the arm axis. `previousRadial` is consulted only when down projects to
// a singular circle direction; head orientation never changes the non-singular preference.
struct IkPistolDownSwivel {
    bool ok = false, usedFallback = false, belowFeasible = false;
    double theta = 0.0, violation = 0.0, axis[3] = {}, reference[3] = {}, radial[3] = {};
};
inline IkPistolDownSwivel ikPistolDownSwivel(const IkTwoBone& t, const double previousRadial[3], const double upIn[3],
                                              double elbowOutM, double trimRad) noexcept {
    IkPistolDownSwivel out;
    if (!t.ok || !(t.h > 1e-6) || !std::isfinite(t.h) || !std::isfinite(t.a) || !std::isfinite(elbowOutM) ||
        elbowOutM < 0.0 || !std::isfinite(trimRad)) return out;
    for (int i = 0; i < 3; ++i) if (!std::isfinite(t.S[i]) || !std::isfinite(t.dir[i]) || !std::isfinite(t.u[i]) || !std::isfinite(t.v[i])) return out;
    double up[3];
    if (!upIn || !std::isfinite(upIn[0]) || !std::isfinite(upIn[1]) || !std::isfinite(upIn[2]) || !ikUnit(upIn, up)) return out;
    double down[3];
    const double along = ikDot(up, t.dir);
    for (int i = 0; i < 3; ++i) down[i] = -up[i] + along * t.dir[i];
    const bool singularDown = !ikUnit(down, down);
    if (singularDown) {
        out.usedFallback = true;
        bool havePrevious = false;
        if (previousRadial) {
            double p[3];
            const double pa = ikDot(previousRadial, t.dir);
            for (int i = 0; i < 3; ++i) p[i] = previousRadial[i] - pa * t.dir[i];
            havePrevious = ikUnit(p, p);
            if (havePrevious) std::memcpy(down, p, sizeof(down));
        }
        if (!havePrevious) {
            // Deterministic body-left fallback for a vertical shoulder-to-hand axis.
            const double left[3] = {-1.0, 0.0, 0.0};
            const double la = ikDot(left, t.dir);
            for (int i = 0; i < 3; ++i) down[i] = left[i] - la * t.dir[i];
            if (!ikUnit(down, down)) return out;
        }
    }
    double lateral[3] = {};
    if (singularDown) {
        // At singularity the previous radial is already the physical continuity choice. Do not
        // reapply trim on every call, which would rotate a stationary elbow indefinitely.
        if (previousRadial && ikDot(previousRadial, previousRadial) > 1e-12) {
            double p[3];
            const double pa = ikDot(previousRadial, t.dir);
            for (int i = 0; i < 3; ++i) p[i] = previousRadial[i] - pa * t.dir[i];
            if (ikUnit(p, p)) std::memcpy(down, p, sizeof(down));
        }
    } else {
        ikCross(t.dir, up, lateral); // fixed + chirality in the documented body basis.
        if (!ikUnit(lateral, lateral)) return out;
    }
    double beta = std::asin(std::max(0.0, std::min(1.0, elbowOutM / t.h))) + trimRad;
    beta = std::max(-0.5 * kIkPi, std::min(0.5 * kIkPi, beta));
    const double center[3] = {t.S[0] + t.dir[0] * t.a, t.S[1] + t.dir[1] * t.a, t.S[2] + t.dir[2] * t.a};
    const double centerUp = ikDot(center, up) - ikDot(t.S, up);
    const double downUp = ikDot(down, up);
    if (!singularDown && centerUp > 0.0) {
        if (downUp < -1e-9) {
            const double ratio = centerUp / (-t.h * downUp);
            if (ratio <= 1.0) {
                const double betaMax = std::acos(std::max(0.0, std::min(1.0, ratio)));
                beta = std::max(-betaMax, std::min(betaMax, beta));
                out.belowFeasible = true;
            } else {
                beta = 0.0; // no down-facing radial can clear the shoulder; minimize height without inventing reach.
            }
        } else {
            beta = 0.0;
        }
    } else if (!singularDown) out.belowFeasible = true;
    if (singularDown) std::memcpy(out.radial, down, sizeof(out.radial));
    else for (int i = 0; i < 3; ++i) out.radial[i] = std::cos(beta) * down[i] + std::sin(beta) * lateral[i];
    if (!ikUnit(out.radial, out.radial)) return IkPistolDownSwivel{};
    double elbow[3];
    for (int i = 0; i < 3; ++i) elbow[i] = center[i] + t.h * out.radial[i];
    out.theta = t.angleOf(out.radial);
    out.violation = std::max(0.0, ikDot(elbow, up) - ikDot(t.S, up));
    out.belowFeasible = out.violation <= 1e-9;
    std::memcpy(out.axis, t.dir, sizeof(out.axis));
    std::memcpy(out.reference, out.radial, sizeof(out.reference));
    out.ok = std::isfinite(out.theta) && std::isfinite(out.violation);
    return out;
}

// F76 diagnostic-only bounded capture. Its thresholds select log samples and never feed pose behavior.
constexpr double kIkPoseDeviationAngleDeg = 20.0;
constexpr double kIkPoseDeviationPositionM = 0.05;
constexpr uint64_t kIkPoseDeviationMaxGapMs = 100;
constexpr uint32_t kIkPoseDeviationEventsPerItemMinute = 8;
constexpr uint32_t kIkPoseDeviationEventsGlobalPerMinute = 32;
constexpr uint32_t kIkPoseDeviationHistory = 8;
constexpr uint32_t kIkPoseDeviationPost = 8;
struct IkPoseDeviationSample {
    uint64_t qpc = 0;
    float dtMs = 0.0f;
    uint64_t fpRig = 0;
    uint32_t frame = 0, item = 0, generation = 0, fpGeneration = 0, fpAgeMs = 0, gAgeMs = 0;
    bool fresh = false, offInputValid = false, offWithinDistance = false, offSteady = false, offQualified = false, offAllowed = false, offTwo = false;
    bool offDuplicate = false, offGapReset = false, offIdentityReset = false, offReversed = false;
    bool leftWrote = false, rightWrote = false, leftStablePolicy = false, authoredPistolSupport = false, supportRequested = false, supportValid = false, reachMeasured = false, reachActive = false, reachInfeasible = false, reachRejected = false;
    uint8_t targetMode[2] = {}, leftReason = 0, offHistorySamples = 0;
    bool valid[2] = {}, radialValid[2] = {}, outwardWaived[2] = {};
    double shoulder[2][3] = {}, elbow[2][3] = {}, hand[2][3] = {}, target[2][3] = {};
    double preferred[2][3] = {}, axis[2][3] = {}, radial[2][3] = {};
    double weight[2] = {}, violationM[2] = {}, outSign[2] = {}, actualOutM[2] = {}, requiredOutM[2] = {};
    double offDistanceM = 0.0, offChangeDeg = 0.0, offRawWeight = 0.0, offEffectiveWeight = 0.0, leftSolveWeight = 0.0, supportBlend = 0.0;
    double reachRadiusM[2] = {}, reachCandidate[3] = {}, pinReach[3] = {};
};
struct IkPoseDeviationRecord {
    IkPoseDeviationSample sample{};
    uint32_t event = 0;
    int8_t offset = 0;
    bool invalid = false;
};
struct IkPoseDeviationTracker {
    IkPoseDeviationSample history[kIkPoseDeviationHistory]{};
    IkPoseDeviationSample last{};
    uint32_t historyHead = 0, historyCount = 0, event = 0, minuteBucket = 0xFFFFFFFFu, eventsInMinute = 0;
    uint32_t activeItemSerial = 0, itemEventsInMinute = 0, itemSuppressed = 0, globalSuppressed = 0;
    uint32_t coalesced = 0, suppressed = 0, gaps = 0, invalid = 0, duplicates = 0;
    uint8_t postRemaining = 0;
    bool haveLast = false, haveActiveItem = false;

    static bool sameIdentity(const IkPoseDeviationSample& a, const IkPoseDeviationSample& b) noexcept {
        return a.item == b.item && a.generation == b.generation && a.fpGeneration == b.fpGeneration && a.fpRig == b.fpRig;
    }

    void resetHistory() noexcept { historyHead = 0; historyCount = 0; haveLast = false; }
    int step(const IkPoseDeviationSample& sample, uint64_t qpcFrequency, IkPoseDeviationRecord out[kIkPoseDeviationHistory + 1 + kIkPoseDeviationPost]) noexcept {
        int nOut = 0;
        const bool sampleFresh = sample.fresh && sample.fpAgeMs <= kIkPoseDeviationMaxGapMs && sample.gAgeMs <= kIkPoseDeviationMaxGapMs;
        const bool wasCapturing = postRemaining != 0;
        if (haveLast && sampleFresh && sameIdentity(sample, last) && sample.frame == last.frame) {
            ++duplicates;
            return 0;
        }
        bool contiguous = false;
        if (haveLast && sampleFresh && last.fresh && sameIdentity(sample, last) && sample.frame - last.frame == 1 &&
            sample.qpc > last.qpc && qpcFrequency && (sample.qpc - last.qpc) <= qpcFrequency / 10) contiguous = true;

        if (!sampleFresh || (haveLast && !contiguous)) {
            if (postRemaining) {
                out[nOut++] = {sample, event, static_cast<int8_t>(1 + (kIkPoseDeviationPost - postRemaining)), true};
                postRemaining = 0;
                ++invalid;
            }
            if (haveLast) ++gaps;
            resetHistory();
        } else if (postRemaining) {
            out[nOut++] = {sample, event, static_cast<int8_t>(1 + (kIkPoseDeviationPost - postRemaining)), false};
            --postRemaining;
        }

        bool triggered = sampleFresh && (sample.offIdentityReset || sample.offGapReset || sample.offReversed);
        if (sampleFresh && contiguous) {
            triggered = triggered || sample.offTwo != last.offTwo || sample.leftStablePolicy != last.leftStablePolicy ||
                        sample.authoredPistolSupport != last.authoredPistolSupport ||
                        sample.leftWrote != last.leftWrote || sample.supportRequested != last.supportRequested || sample.supportValid != last.supportValid || sample.targetMode[0] != last.targetMode[0] ||
                        sample.targetMode[1] != last.targetMode[1] || sample.reachActive != last.reachActive || sample.reachRejected != last.reachRejected;
            for (int side = 0; side < 2 && !triggered; ++side) {
                if (!sample.valid[side] || !last.valid[side]) continue;
                const double shoulderDelta[3] = {sample.shoulder[side][0] - last.shoulder[side][0], sample.shoulder[side][1] - last.shoulder[side][1],
                                                  sample.shoulder[side][2] - last.shoulder[side][2]};
                if (ikLen(shoulderDelta) >= kIkPoseDeviationPositionM) { triggered = true; break; }
                const double nowDelta[3] = {sample.elbow[side][0] - sample.shoulder[side][0], sample.elbow[side][1] - sample.shoulder[side][1], sample.elbow[side][2] - sample.shoulder[side][2]};
                const double oldDelta[3] = {last.elbow[side][0] - last.shoulder[side][0], last.elbow[side][1] - last.shoulder[side][1], last.elbow[side][2] - last.shoulder[side][2]};
                const double moved[3] = {nowDelta[0] - oldDelta[0], nowDelta[1] - oldDelta[1], nowDelta[2] - oldDelta[2]};
                if (ikLen(moved) >= kIkPoseDeviationPositionM) { triggered = true; break; }
                if (sample.radialValid[side] && last.radialValid[side]) {
                    double a[3], b[3];
                    if (ikUnit(sample.radial[side], a) && ikUnit(last.radial[side], b)) {
                        const double cosine = std::max(-1.0, std::min(1.0, ikDot(a, b)));
                        if (std::acos(cosine) * 180.0 / kIkPi >= kIkPoseDeviationAngleDeg) triggered = true;
                    }
                }
            }
        }
        if (triggered) {
            const uint32_t bucket = qpcFrequency ? static_cast<uint32_t>(sample.qpc / (qpcFrequency * 60u)) : 0;
            if (bucket != minuteBucket) { minuteBucket = bucket; eventsInMinute = 0; itemEventsInMinute = 0; haveActiveItem = false; }
            if (!haveActiveItem || activeItemSerial != sample.item) {
                activeItemSerial = sample.item;
                itemEventsInMinute = 0;
                haveActiveItem = true;
            }
            if (wasCapturing) ++coalesced;
            else if (eventsInMinute >= kIkPoseDeviationEventsGlobalPerMinute) { ++suppressed; ++globalSuppressed; }
            else if (itemEventsInMinute >= kIkPoseDeviationEventsPerItemMinute) { ++suppressed; ++itemSuppressed; }
            else {
                ++eventsInMinute;
                ++itemEventsInMinute;
                ++event;
                const uint32_t oldest = (historyHead + kIkPoseDeviationHistory - historyCount) % kIkPoseDeviationHistory;
                for (uint32_t i = 0; i < historyCount; ++i) out[nOut++] = {history[(oldest + i) % kIkPoseDeviationHistory], event, static_cast<int8_t>(-static_cast<int>(historyCount) + static_cast<int>(i)), false};
                out[nOut++] = {sample, event, 0, false};
                postRemaining = kIkPoseDeviationPost;
            }
        }
        if (sampleFresh) {
            history[historyHead] = sample;
            historyHead = (historyHead + 1) % kIkPoseDeviationHistory;
            if (historyCount < kIkPoseDeviationHistory) ++historyCount;
            last = sample;
            haveLast = true;
        }
        return nOut;
    }
};

// Row-vector rotations: R about the unit axis n by ang (right-handed in the column sense: v x R = Rc v). The shortest arc from unit a to unit b.
inline void ikRotAxis(const double n[3], double ang, double R[9]) noexcept {
    const double c = std::cos(ang), s = std::sin(ang), k = 1.0 - c;
    // R = c I - s [n]x + k n n^T (the transpose of the column-convention Rodrigues matrix)
    R[0] = c + k * n[0] * n[0];
    R[1] = s * n[2] + k * n[0] * n[1];
    R[2] = -s * n[1] + k * n[0] * n[2];
    R[3] = -s * n[2] + k * n[1] * n[0];
    R[4] = c + k * n[1] * n[1];
    R[5] = s * n[0] + k * n[1] * n[2];
    R[6] = s * n[1] + k * n[2] * n[0];
    R[7] = -s * n[0] + k * n[2] * n[1];
    R[8] = c + k * n[2] * n[2];
}
inline void ikRotFromTo(const double a[3], const double b[3], double R[9]) noexcept {
    double k[3];
    ikCross(a, b, k);
    const double s = ikLen(k), c = ikDot(a, b);
    if (s < 1e-9) {
        if (c > 0.0) {
            for (int i = 0; i < 9; ++i) R[i] = (i % 4 == 0) ? 1.0 : 0.0;
            return;
        }
        const double axis[3] = {std::fabs(a[0]) < 0.9 ? 1.0 : 0.0, std::fabs(a[0]) < 0.9 ? 0.0 : 1.0, 0.0};
        double p[3], n[3];
        ikCross(a, axis, p);
        if (!ikUnit(p, n)) n[0] = 0.0, n[1] = 0.0, n[2] = 1.0;
        ikRotAxis(n, kIkPi, R);
        return;
    }
    const double n[3] = {k[0] / s, k[1] / s, k[2] / s};
    ikRotAxis(n, std::atan2(s, c), R);
}
// out = M x T(-P) x R x T(P): the joint matrix M rotated by R about the model-space point P. out may not alias M.
inline void ikPivot(const float M[16], const double R[9], const double P[3], float out[16]) noexcept {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 4 + c] = static_cast<float>(M[r * 4 + 0] * R[0 * 3 + c] + M[r * 4 + 1] * R[1 * 3 + c] + M[r * 4 + 2] * R[2 * 3 + c]);
    for (int r = 0; r < 3; ++r) out[r * 4 + 3] = M[r * 4 + 3];
    const double q[3] = {M[12] - P[0], M[13] - P[1], M[14] - P[2]};
    for (int c = 0; c < 3; ++c) out[12 + c] = static_cast<float>(q[0] * R[0 * 3 + c] + q[1] * R[1 * 3 + c] + q[2] * R[2 * 3 + c] + P[c]);
    out[15] = M[15];
}
// The rotation's quaternion (column sense; w >= 0) and the twist angle about a unit axis (swing-twist): 2 atan2(q.xyz . axis, q.w), in (-pi, pi].
inline void ikQuat(const double R[9], double q[4]) noexcept {
    // Column-convention Rc = R^T: Rc[i][j] = R[j * 3 + i].
    const auto rc = [&](int i, int j) noexcept { return R[j * 3 + i]; };
    const double tr = rc(0, 0) + rc(1, 1) + rc(2, 2);
    double x, y, z, w;
    if (tr > 0.0) {
        const double s = std::sqrt(tr + 1.0) * 2.0;
        w = 0.25 * s;
        x = (rc(2, 1) - rc(1, 2)) / s;
        y = (rc(0, 2) - rc(2, 0)) / s;
        z = (rc(1, 0) - rc(0, 1)) / s;
    } else if (rc(0, 0) > rc(1, 1) && rc(0, 0) > rc(2, 2)) {
        const double s = std::sqrt(1.0 + rc(0, 0) - rc(1, 1) - rc(2, 2)) * 2.0;
        w = (rc(2, 1) - rc(1, 2)) / s;
        x = 0.25 * s;
        y = (rc(0, 1) + rc(1, 0)) / s;
        z = (rc(0, 2) + rc(2, 0)) / s;
    } else if (rc(1, 1) > rc(2, 2)) {
        const double s = std::sqrt(1.0 + rc(1, 1) - rc(0, 0) - rc(2, 2)) * 2.0;
        w = (rc(0, 2) - rc(2, 0)) / s;
        x = (rc(0, 1) + rc(1, 0)) / s;
        y = 0.25 * s;
        z = (rc(1, 2) + rc(2, 1)) / s;
    } else {
        const double s = std::sqrt(1.0 + rc(2, 2) - rc(0, 0) - rc(1, 1)) * 2.0;
        w = (rc(1, 0) - rc(0, 1)) / s;
        x = (rc(0, 2) + rc(2, 0)) / s;
        y = (rc(1, 2) + rc(2, 1)) / s;
        z = 0.25 * s;
    }
    const double sg = w < 0.0 ? -1.0 : 1.0;
    q[0] = x * sg;
    q[1] = y * sg;
    q[2] = z * sg;
    q[3] = w * sg;
}
inline double ikTwistAbout(const double R[9], const double axis[3]) noexcept {
    double q[4];
    ikQuat(R, q);
    return ikWrapPi(2.0 * std::atan2(q[0] * axis[0] + q[1] * axis[1] + q[2] * axis[2], q[3]));
}
// The angle (degrees) between two rotations given as row-vector matrices' upper 3x3 (rows normalised first).
inline double ikRotAngleDeg(const float a[16], const float b[16]) noexcept {
    double A[9], B[9];
    for (int r = 0; r < 3; ++r) {
        const double la = std::sqrt(static_cast<double>(a[r * 4]) * a[r * 4] + static_cast<double>(a[r * 4 + 1]) * a[r * 4 + 1] +
                                    static_cast<double>(a[r * 4 + 2]) * a[r * 4 + 2]);
        const double lb = std::sqrt(static_cast<double>(b[r * 4]) * b[r * 4] + static_cast<double>(b[r * 4 + 1]) * b[r * 4 + 1] +
                                    static_cast<double>(b[r * 4 + 2]) * b[r * 4 + 2]);
        for (int c = 0; c < 3; ++c) {
            A[r * 3 + c] = la > 1e-9 ? a[r * 4 + c] / la : 0.0;
            B[r * 3 + c] = lb > 1e-9 ? b[r * 4 + c] / lb : 0.0;
        }
    }
    double tr = 0.0;   // trace(A^T B) = sum of the row dot products
    for (int r = 0; r < 3; ++r) tr += A[r * 3] * B[r * 3] + A[r * 3 + 1] * B[r * 3 + 1] + A[r * 3 + 2] * B[r * 3 + 2];
    double c = (tr - 1.0) / 2.0;
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    return std::acos(c) * 180.0 / kIkPi;
}
// Orthonormalise a row-vector matrix's rotation rows in place (Gram-Schmidt on rows 0 and 1; row 2 = row 0 x row 1). False when degenerate.
inline bool ikOrthonormalise(float m[16]) noexcept {
    double r0[3] = {m[0], m[1], m[2]}, r1[3] = {m[4], m[5], m[6]}, n0[3], n1[3], n2[3], t[3];
    if (!ikUnit(r0, n0)) return false;
    const double d = ikDot(r1, n0);
    for (int i = 0; i < 3; ++i) t[i] = r1[i] - n0[i] * d;
    if (!ikUnit(t, n1)) return false;
    ikCross(n0, n1, n2);
    // Keep the handedness of the input's row 2.
    const double r2[3] = {m[8], m[9], m[10]};
    const double sg = ikDot(r2, n2) < 0.0 ? -1.0 : 1.0;
    for (int i = 0; i < 3; ++i) {
        m[i] = static_cast<float>(n0[i]);
        m[4 + i] = static_cast<float>(n1[i]);
        m[8 + i] = static_cast<float>(n2[i] * sg);
    }
    return true;
}

// ---- F57 (TEMPORARY): the arm IK refinements, pure half ---------------------------------------------------------------------------
// (1) The pole from the FP elbow: S', E', T' the FP upper arm, forearm and hand mapped into the 3P model by G; pole = unit(perp(unit(E' - S'),
// axis T' - S')); a part across the axis under kIkPoleMinPerp (a nearly straight FP arm) keeps the current pole. (2) The torso square-up: the
// 3P shoulder line (clavicle 36 - 35, xz) turned onto the FP's (20 - 19 mapped by G), clamped to +-50 deg, ramped over 0.3 s, split over the
// spine 16, 21, 24 (1/3, 2/3, 1; rigid on each one's exclusive subtree) about the vertical through joint 34. (3) The elbow's interior angle held
// in [40, 170] deg (ikTwoBoneFrame's limits). (4) The sliders: swivel trim (deg), square-up amount (0..1), elbow out (cm).
constexpr double kIkPoleMinPerp = 0.2;
constexpr double kIkElbowMinDeg = 40.0, kIkElbowMaxDeg = 170.0;
constexpr double kIkSquareMaxDeg = 50.0;
constexpr double kIkSquareRampSec = 0.3;
constexpr uint32_t kIkSpine[3] = {16, 21, 24};       // the spine chain the square-up is split over, hips side first
constexpr double kIkSpineWeight[3] = {1.0 / 3.0, 2.0 / 3.0, 1.0};
constexpr uint32_t kIkSquarePivot = 34;              // the vertical axis runs through this joint's xz
constexpr uint32_t kIkClavL = 35, kIkClavR = 36;     // the 3P clavicles (their parent is the chest root, kIkSpine[2])
// F96: the arm IK's shipped values. Their config keys were removed, so these are the values now (degrees, the 0..1 amount, cm).
constexpr double kIkSwivelTrimDeg = 2.0, kIkSquareAmount = 0.75, kIkElbowOutCm = 3.5;   // the values Sean flew (F96)
constexpr double kWeaponViewOffsetDefaultCm = 0.0;   // F112: the pilot's flown value (F90: 2.0, 2.5 before)
// The FP snapshot's slots (fdAfterAnimate): the attach joints, the hands, the clavicles, the upper arms, the forearms. F63: and joint 14, the FP
// def_c_povCamera_joint (the joint probe of log 054928: FP index 14), the ADS sight lock's sight point.
constexpr uint32_t kIkFpJoints[11] = {51, 52, 37, 44, 19, 20, 25, 26, 31, 34, 14};
constexpr uint32_t kIkFpSlots = 11;
constexpr uint32_t kIkFpSlotPov = 10;
// Per side (0 L, 1 R): the slots of the FP attach, hand, clavicle, upper arm (S'), forearm (E').
constexpr uint32_t kIkFpSlotAttach[2] = {1, 0}, kIkFpSlotHand[2] = {2, 3}, kIkFpSlotClav[2] = {4, 5}, kIkFpSlotUpper[2] = {6, 7},
                   kIkFpSlotFore[2] = {8, 9};

// A point through a row-vector 4x4 (v x M).
inline void ikXform(const float p[3], const float M[16], double out[3]) noexcept {
    for (int c = 0; c < 3; ++c)
        out[c] = static_cast<double>(p[0]) * M[c] + static_cast<double>(p[1]) * M[4 + c] + static_cast<double>(p[2]) * M[8 + c] + M[12 + c];
}
inline void ikXform(const double p[3], const float M[16], double out[3]) noexcept {
    for (int c = 0; c < 3; ++c)
        out[c] = p[0] * M[c] + p[1] * M[4 + c] + p[2] * M[8 + c] + M[12 + c];
}
// (1) The FP pole: true and `pole` the unit part of unit(E - S) across the axis T - S when that part is at least kIkPoleMinPerp; false (pole
// untouched) for a nearly straight arm or a degenerate input.
inline bool ikPoleFromFp(const double S[3], const double E[3], const double T[3], double pole[3]) noexcept {
    const double se[3] = {E[0] - S[0], E[1] - S[1], E[2] - S[2]}, st[3] = {T[0] - S[0], T[1] - S[1], T[2] - S[2]};
    double ue[3], ax[3];
    if (!ikUnit(se, ue) || !ikUnit(st, ax)) return false;
    const double d = ikDot(ue, ax);
    const double pp[3] = {ue[0] - ax[0] * d, ue[1] - ax[1] * d, ue[2] - ax[2] * d};
    if (!(ikLen(pp) >= kIkPoleMinPerp)) return false;
    return ikUnit(pp, pole);
}
// The swivel trim's sign at theta: +1 when a positive change of theta moves the elbow out on the arm's side (out * dE.x/dtheta >= 0), so a
// positive trim swings the elbow outward on both arms.
inline double ikTrimSign(const IkTwoBone& t, double theta, double out) noexcept {
    const double dEx = t.h * (-std::sin(theta) * t.u[0] + std::cos(theta) * t.v[0]);
    return out * dEx >= 0.0 ? 1.0 : -1.0;
}
// (2) The signed yaw (radians, about +y, right-handed: + turns +x toward -z) that turns a's xz direction onto b's. 0 when either is degenerate.
inline double ikYawFromTo(const double a[3], const double b[3]) noexcept {
    const double cr = a[2] * b[0] - a[0] * b[2], dt = a[0] * b[0] + a[2] * b[2];
    if (!(std::fabs(cr) + std::fabs(dt) > 1e-12)) return 0.0;
    return std::atan2(cr, dt);
}
// The square-up's clamp: +-kIkSquareMaxDeg (radians in and out); NaN is 0. *clamped when it bit.
inline double ikSquareClampRad(double phi, bool* clamped) noexcept {
    const double lim = kIkSquareMaxDeg * kIkPi / 180.0;
    if (clamped) *clamped = false;
    if (!(phi == phi)) return 0.0;
    if (phi > lim || phi < -lim) {
        if (clamped) *clamped = true;
        return phi > 0.0 ? lim : -lim;
    }
    return phi;
}
// The spine level of joint j: 2 under kIkSpine[2] (the chest), 1 under kIkSpine[1] only, 0 under kIkSpine[0] only, -1 otherwise.
inline int ikSpineLevel(const uint16_t* parents, uint32_t joints, uint32_t j) noexcept {
    for (int l = 2; l >= 0; --l)
        if (ikUnder(parents, joints, j, kIkSpine[l])) return l;
    return -1;
}
// The square-up's plan: the subtree of kIkSpine[0] with each member's level (any order: the rotations share one axis, so each member takes its
// level's absolute angle). why: 0 ok, 1 bad input, 2 the spine is not a chain (21 under 16, 24 under 21), 3 the clavicles' parent is not
// kIkSpine[2] (the chest root), 4 the pivot is not under the chest root.
struct IkSquarePlan {
    uint32_t why = 1;
    uint16_t chestRoot = 0xFFFFu;
    uint16_t n = 0;
    uint16_t j[kPfzMaxJoints] = {};
    uint8_t level[kPfzMaxJoints] = {};
};
inline IkSquarePlan ikSquarePlan(const uint16_t* parents, uint32_t joints) noexcept {
    IkSquarePlan p;
    if (joints == 0 || joints > kPfzMaxJoints || kIkClavR >= joints || kIkSquarePivot >= joints || kIkSpine[2] >= joints) return p;
    if (!ikUnder(parents, joints, kIkSpine[1], kIkSpine[0]) || !ikUnder(parents, joints, kIkSpine[2], kIkSpine[1]) ||
        kIkSpine[1] == kIkSpine[0] || kIkSpine[2] == kIkSpine[1]) {
        p.why = 2;
        return p;
    }
    p.chestRoot = parents[kIkClavL];
    if (parents[kIkClavL] != parents[kIkClavR] || parents[kIkClavL] != kIkSpine[2]) {
        p.why = 3;
        return p;
    }
    if (!ikUnder(parents, joints, kIkSquarePivot, kIkSpine[2])) {
        p.why = 4;
        return p;
    }
    for (uint32_t j = 0; j < joints; ++j) {
        const int l = ikSpineLevel(parents, joints, j);
        if (l < 0) continue;
        p.j[p.n] = static_cast<uint16_t>(j);
        p.level[p.n] = static_cast<uint8_t>(l);
        ++p.n;
    }
    p.why = 0;
    return p;
}
inline const char* ikSquareWhyText(uint32_t why) noexcept {
    switch (why) {
        case 0: return "ok";
        case 1: return "a joint index out of range or no joints";
        case 2: return "the spine 16, 21, 24 is not a parent chain";
        case 3: return "the clavicles 35 and 36 do not share the parent 24 (the chest root)";
        case 4: return "the pivot joint 34 is not under the chest root";
        default: return "?";
    }
}

// ---- F65b (TEMPORARY, fix.native_on_foot_stand_armed): keep the 3P body standing when armed, the feet planted --------------------------
// The F64 log: a drawn weapon's 3P stance crouches the body about 21 cm (joint 7 drops; pov 1.699 -> 1.489, neck 1.543 -> 1.334) while the
// FP view and weapon stay at standing height. The Animate post-hook raises the upper body UB (subtree(anchor 7) less both thigh subtrees) by
// delta in model y and solves each leg from the raised hip to the animated foot (two bones, the knee bending forward as animated), leaving the
// foot and toe exactly as animated. The legs from the RE (3P): L thigh 18, calf 23, foot 103 (def_l_foot_joint), toe 130; R 17, 22, 100,
// 129 INFERRED by mirror: the census confirms the R foot by FindJoint("def_r_foot_joint") and refuses the legs when anything differs.
constexpr uint32_t kLegFoot[2] = {103, 100}, kLegCalf[2] = {23, 22}, kLegThigh[2] = {18, 17};
constexpr uint32_t kStandPov = 15, kStandHips = 7;
constexpr double kStandReach = 0.995;   // F67c: a leg is asked to reach at most this share of l1 + l2 (the knee slightly bent)
// A leg's chain from its foot: F = parents[foot] (the calf), U = parents[F] (the thigh), C = parents[U] (the pelvis); ikArmChain with H = A =
// the foot: gU = the thigh and its helpers, gF = the calf and its helpers, gH = the foot and the toe (never written). why 2 when U is not
// directly under C.
inline IkArm ikLegChain(const uint16_t* parents, uint32_t joints, uint32_t foot) noexcept {
    IkArm s;
    if (joints == 0 || joints > kPfzMaxJoints || foot >= joints) return s;
    const uint32_t F = parents[foot];
    const uint32_t U = F < joints ? parents[F] : 0xFFFFu;
    const uint32_t C = U < joints ? parents[U] : 0xFFFFu;
    if (C >= joints) {
        s.why = 2;
        return s;
    }
    s = ikArmChain(parents, joints, C, foot, foot);
    if (s.why == 0 && (!s.direct || s.U != U || s.F != F)) s.why = 2;
    return s;
}
// The pair's verdict: 0 ok; a leg's own why (1..5); 6 the legs do not share the pelvis C; 7 parent(C) is not the anchor; 8 an index differs
// from the RE's (the probe's foot, or the chain's calf or thigh).
inline uint32_t ikLegPairWhy(const uint16_t* parents, uint32_t joints, const IkArm& l, const IkArm& r, uint32_t anchor, const uint32_t probeFoot[2]) noexcept {
    if (l.why != 0) return l.why;
    if (r.why != 0) return r.why;
    if (l.C != r.C) return 6;
    if (l.C >= joints || parents[l.C] != anchor) return 7;
    const IkArm* legs[2] = {&l, &r};
    for (int k = 0; k < 2; ++k)
        if (probeFoot[k] != kLegFoot[k] || legs[k]->H != kLegFoot[k] || legs[k]->F != kLegCalf[k] || legs[k]->U != kLegThigh[k]) return 8;
    return 0;
}
inline const char* ikLegWhyText(uint32_t why) noexcept {
    switch (why) {
        case 0: return "ok";
        case 6: return "the legs do not share the pelvis";
        case 7: return "the pelvis's parent is not the anchor (7)";
        case 8: return "an index differs from the RE's (foot 103/100, calf 23/22, thigh 18/17)";
        default: return ikArmWhyText(why);
    }
}
// The upper body: subtree(anchor) less subtree(uL) and subtree(uR), in index order. 0 on bad input.
inline uint32_t ikUpperBody(const uint16_t* parents, uint32_t joints, uint32_t anchor, uint32_t uL, uint32_t uR, uint16_t* out, uint32_t cap) noexcept {
    if (joints == 0 || joints > kPfzMaxJoints || anchor >= joints || uL >= joints || uR >= joints) return 0;
    uint32_t n = 0;
    for (uint32_t j = 0; j < joints && n < cap; ++j)
        if (ikUnder(parents, joints, j, anchor) && !ikUnder(parents, joints, j, uL) && !ikUnder(parents, joints, j, uR)) out[n++] = static_cast<uint16_t>(j);
    return n;
}
// ---- F65c: the body align's neck source ---------------------------------------------------------------------------------------------
// The F64 log (standing unarmed): before the bend the 3P neck sat 11.4 cm left of its target and the bend rolled the torso a mean 15 deg to
// close it. The align's neck source was the census neck, read AFTER the hook wrote the bent pose, so the align saw a neck already under the
// eye and left d alone. The hook now publishes the neck as it stands before the bend (after stage 0); the align takes it while fresh (at
// most kPreNeckMaxTicks frame ticks since its sequence last changed, and under kPreNeckMaxMs), else the census neck, else none.
constexpr uint32_t kPreNeckMaxTicks = 2;
constexpr uint64_t kPreNeckMaxMs = 100;
enum NeckSource : int { kNeckPreBend = 0, kNeckCensus = 1, kNeckNone = 2 };
inline int neckSourcePick(bool preHave, uint32_t ticksSince, uint64_t ageMs, bool censusOk) noexcept {
    if (preHave && ticksSince <= kPreNeckMaxTicks && ageMs <= kPreNeckMaxMs) return kNeckPreBend;
    return censusOk ? kNeckCensus : kNeckNone;
}

// One leg's solve: S = hip0 raised by `raise` (model y), T = the animated foot; l1 = |knee0 - hip0|, l2 = |foot0 - knee0|. The pole is the part
// of (knee0 - hip0) across the original hip -> foot axis, so the knee bends as animated. R1 swings the thigh (and everything under it) about S
// from the animated knee's direction to the solved one; R2 swings the calf about the knee E so its end lands on T. footErr is the carried
// foot's distance from T (about 0 in reach), and the knee's interior angles (degrees, 180 straight) before and after.
struct LegSolve {
    bool ok = false, limited = false;
    double S[3] = {}, E[3] = {}, R1[9] = {}, R2[9] = {};
    double l1 = 0.0, l2 = 0.0, reachBefore = 0.0, reachAfter = 0.0, kneeAnimDeg = 0.0, kneeSolvedDeg = 0.0, footErrM = 0.0;
};
inline void ikRowRotate(const double v[3], const double R[9], double out[3]) noexcept {
    for (int c = 0; c < 3; ++c) out[c] = v[0] * R[c] + v[1] * R[3 + c] + v[2] * R[6 + c];
}
// F67c: the general form. S1 is where the hip is now (hip0 moved with the pelvis), T1 where the foot is to stand (the animated foot, re-planted
// or lifted). The pole is the part of (knee0 - hip0) across the animated hip -> foot axis, so the knee bends as animated.
inline LegSolve legSolveTo(const double hip0[3], const double knee0[3], const double foot0[3], const double S1[3], const double T1[3]) noexcept {
    LegSolve o;
    const double hk[3] = {knee0[0] - hip0[0], knee0[1] - hip0[1], knee0[2] - hip0[2]};
    const double kf[3] = {foot0[0] - knee0[0], foot0[1] - knee0[1], foot0[2] - knee0[2]};
    const double hf[3] = {foot0[0] - hip0[0], foot0[1] - hip0[1], foot0[2] - hip0[2]};
    o.l1 = ikLen(hk);
    o.l2 = ikLen(kf);
    if (!(o.l1 > 1e-4) || !(o.l2 > 1e-4)) return o;
    o.reachBefore = ikLen(hf) / (o.l1 + o.l2);
    o.kneeAnimDeg = ikElbowInteriorDeg(o.l1, o.l2, ikLen(hf));
    double axis[3];
    if (!ikUnit(hf, axis)) return o;
    const double pd = ikDot(hk, axis);
    const double pole[3] = {hk[0] - axis[0] * pd, hk[1] - axis[1] * pd, hk[2] - axis[2] * pd};
    for (int i = 0; i < 3; ++i) o.S[i] = S1[i];
    const IkTwoBone tb = ikTwoBoneFrame(o.S, T1, o.l1, o.l2, pole);
    if (!tb.ok) return o;
    tb.elbowAt(0.0, o.E);
    o.limited = tb.limited;
    o.reachAfter = tb.reach;
    o.kneeSolvedDeg = ikElbowInteriorDeg(o.l1, o.l2, tb.D);
    double u0[3], u1[3];
    const double se[3] = {o.E[0] - o.S[0], o.E[1] - o.S[1], o.E[2] - o.S[2]};
    if (!ikUnit(hk, u0) || !ikUnit(se, u1)) return o;
    ikRotFromTo(u0, u1, o.R1);
    double kfSw[3], d1[3], d2[3];
    ikRowRotate(kf, o.R1, kfSw);   // the calf after swing 1 (the moved foot carried about S)
    const double ft[3] = {T1[0] - o.E[0], T1[1] - o.E[1], T1[2] - o.E[2]};
    if (!ikUnit(kfSw, d1) || !ikUnit(ft, d2)) return o;
    ikRotFromTo(d1, d2, o.R2);
    double kfOut[3];
    ikRowRotate(kfSw, o.R2, kfOut);
    const double err[3] = {o.E[0] + kfOut[0] - T1[0], o.E[1] + kfOut[1] - T1[1], o.E[2] + kfOut[2] - T1[2]};
    o.footErrM = ikLen(err);
    o.ok = true;
    return o;
}
// The old form (F65b): the hip raised by `raise` (model y), the foot where it was.
inline LegSolve legSolve(const double hip0[3], const double knee0[3], const double foot0[3], double raise) noexcept {
    const double S1[3] = {hip0[0], hip0[1] + raise, hip0[2]};
    return legSolveTo(hip0, knee0, foot0, S1, foot0);
}

// ---- F58 (TEMPORARY): the spine bend toward the head (full-3P mode: hide_fp_body and body_unclip on) ------------------------------------
// In the Animate post-hook, after the freeze and before the square-up and the arm IK. The 3P pov (the eye point) N is turned toward the eye
// target T (the job camera less d, in the 3P model space: x right, y up, z forward) about the spine chain's root P (joint kIkSpine[0]). The
// pitch (about the body's right axis +x; positive leans forward, +y toward +z) matches the sagittal angle of N - P to that of T - P; the roll
// (about the forward axis; positive leans right, +y toward +x) then matches the frontal angle. A rotation keeps |N - P|, so this is the
// closest N comes to T about P; the rest (the height) is left and logged. Clamped: forward 0..45 deg, back 0..15 deg, roll +-20 deg. Split
// over the spine levels 1/3, 2/3, 1 (kIkSpineWeight) about the one pivot, as the square-up. Ramped over 0.2 s; scaled by the amount slider.
constexpr double kBendFwdMaxDeg = 45.0, kBendBackMaxDeg = 15.0, kBendRollMaxDeg = 20.0;
constexpr double kBendRampSec = 0.2, kBendAmountDefault = 1.0;
constexpr uint64_t kBendStaleMs = 500;   // the hook's target older than this refuses the bend
inline double bendAmountClamp(double v) noexcept {
    if (!(v == v)) return kBendAmountDefault;
    return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
}
// The residual T - N in body axes, ordered (forward, up, right) = (z, y, x) of the 3P model space.
inline void bendResidualSplit(const double T[3], const double N[3], double out[3]) noexcept {
    out[0] = T[2] - N[2];
    out[1] = T[1] - N[1];
    out[2] = T[0] - N[0];
}

// F59b: the full-3P head follow's body frame. Live: a fresh AMC sample (kStaleMs) with the head census. Without the census but with a fresh
// AMC sample: the AMC matrix (this sample's matrix and job camera, the joints carried over from the last good frame in body axes). Without a
// fresh sample: the last good frame itself. Neither fallback uses a last good frame older than kFollowHoldMs; past it, none.
constexpr uint64_t kFollowHoldMs = 500;
constexpr double kFollowBlendSec = 0.25;   // a change between sources ramps from the last output to the new source's over this
enum FollowSrc : int { kFollowNone = 0, kFollowLive = 1, kFollowMatrix = 2, kFollowLast = 3 };
inline int followPick(bool liveOk, bool matrixFresh, bool haveLast, uint64_t lastStamp, uint64_t now, uint64_t* ageOut) noexcept {
    const uint64_t age = haveLast ? (now > lastStamp ? now - lastStamp : 0) : 0;
    if (ageOut) *ageOut = age;
    if (liveOk) return kFollowLive;
    if (!haveLast || age > kFollowHoldMs) return kFollowNone;
    return matrixFresh ? kFollowMatrix : kFollowLast;
}
// The ramp between sources (world metres, the body run's target). A change between two sources starts from the last output, so the change
// frame outputs exactly the last output, then moves to the new source's target over kFollowBlendSec. From or to none: no ramp (none feeds the
// body filter zero, as a wait always has).
struct FollowBlend {
    int src = kFollowNone;
    double w = 1.0, from[3] = {}, last[3] = {};
    void step(int s, const double target[3], double dt, double out[3]) noexcept {
        if (s != src) {
            if (src != kFollowNone && s != kFollowNone) {
                for (int i = 0; i < 3; ++i) from[i] = last[i];
                w = 0.0;
            } else {
                w = 1.0;
            }
            src = s;
        }
        for (int i = 0; i < 3; ++i) {
            out[i] = w >= 1.0 ? target[i] : from[i] + w * (target[i] - from[i]);
            last[i] = out[i];
        }
        const double d = dt > 0.0 ? dt / kFollowBlendSec : 0.0;
        w = w + d > 1.0 ? 1.0 : w + d;
    }
};
// A world point carried from one AMC frame to another through its body coordinates: b = rowsA . (p - originA), q = originB + rowsB^T b.
inline void carryBody(const float p[3], const float originA[3], const float rowsA[9], const float originB[3], const float rowsB[9],
                      float q[3]) noexcept {
    double b[3] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) b[i] += (static_cast<double>(p[j]) - originA[j]) * static_cast<double>(rowsA[3 * i + j]);
    for (int j = 0; j < 3; ++j) {
        double w = originB[j];
        for (int i = 0; i < 3; ++i) w += b[i] * static_cast<double>(rowsB[3 * i + j]);
        q[j] = static_cast<float>(w);
    }
}
// F59b: the eye-versus-neck residual in body axes (right, up, forward): rows . (eye - neck), metres; the norm is returned.
inline double eyeNeckBody(const double eye[3], const double neck[3], const double rows[9], double out[3]) noexcept {
    double n2 = 0.0;
    for (int i = 0; i < 3; ++i) {
        out[i] = 0.0;
        for (int j = 0; j < 3; ++j) out[i] += (eye[j] - neck[j]) * rows[3 * i + j];
        n2 += out[i] * out[i];
    }
    return std::sqrt(n2);
}
// F59b: the hook's post-bend residual T - N1 (bendResidualSplit's order: forward, up, right) to eye - neck (right, up, forward). The bend's
// target is T = (eye - d) + the rest offset in body axes, so eye - (N1 + d) = (T - N1) - the rest offset. Same units in and out (scale: 1
// for metres, 100 for centimetres, applied to the rest offset). F62a: the bend runs in full 3P only, so its rest offset is kNeckRestOffsetFull3P.
inline double eyeNeckFromBend(const double split[3], double scale, double out[3]) noexcept {
    out[0] = split[2] - scale * kNeckRestOffsetFull3P[0];
    out[1] = split[1] - scale * kNeckRestOffsetFull3P[1];
    out[2] = split[0] - scale * kNeckRestOffsetFull3P[2];
    return std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
}
struct BendClamp {
    bool fwd = false, back = false, roll = false;
};
// The clamps (radians in and out; NaN is 0) at the given limits (degrees). The flags are set when a clamp bites (never cleared here).
inline double bendClampPitchLim(double pitch, double fwdDeg, double backDeg, BendClamp* c) noexcept {
    if (!(pitch == pitch)) return 0.0;
    const double fwd = fwdDeg * kIkPi / 180.0, back = backDeg * kIkPi / 180.0;
    if (pitch > fwd) {
        if (c) c->fwd = true;
        return fwd;
    }
    if (pitch < -back) {
        if (c) c->back = true;
        return -back;
    }
    return pitch;
}
inline double bendClampRollLim(double roll, double limDeg, BendClamp* c) noexcept {
    if (!(roll == roll)) return 0.0;
    const double lim = limDeg * kIkPi / 180.0;
    if (roll > lim || roll < -lim) {
        if (c) c->roll = true;
        return roll > 0.0 ? lim : -lim;
    }
    return roll;
}
// The F58 limits: forward 45, back 15, roll 20 deg.
inline double bendClampPitch(double pitch, BendClamp* c) noexcept { return bendClampPitchLim(pitch, kBendFwdMaxDeg, kBendBackMaxDeg, c); }
inline double bendClampRoll(double roll, BendClamp* c) noexcept { return bendClampRollLim(roll, kBendRollMaxDeg, c); }
// The row-vector rotation of a level with share w: the pitch first (about +x by w pitch), then the roll (about +z by -w roll): R = Rx x Rz.
inline void bendRot(double pitch, double roll, double w, double R[9]) noexcept {
    const double ex[3] = {1.0, 0.0, 0.0}, ez[3] = {0.0, 0.0, 1.0};
    double Rx[9], Rz[9];
    ikRotAxis(ex, w * pitch, Rx);
    ikRotAxis(ez, -w * roll, Rz);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) R[r * 3 + c] = Rx[r * 3 + 0] * Rz[0 * 3 + c] + Rx[r * 3 + 1] * Rz[1 * 3 + c] + Rx[r * 3 + 2] * Rz[2 * 3 + c];
}
inline void bendApply(const double v[3], const double R[9], double out[3]) noexcept {
    for (int c = 0; c < 3; ++c) out[c] = v[0] * R[c] + v[1] * R[3 + c] + v[2] * R[6 + c];
}
// The solve (unclamped, radians): the pitch that matches N - P's sagittal angle atan2(z, y) to T - P's, then the roll that matches the frontal
// angle atan2(x, y) of the pitched N - P to T - P's. False (both 0) when either vector is under 1 cm.
inline bool bendSolve(const double P[3], const double N[3], const double T[3], double* pitch, double* roll) noexcept {
    *pitch = *roll = 0.0;
    const double v[3] = {N[0] - P[0], N[1] - P[1], N[2] - P[2]}, t[3] = {T[0] - P[0], T[1] - P[1], T[2] - P[2]};
    if (!(ikLen(v) >= 0.01) || !(ikLen(t) >= 0.01)) return false;
    const double p = ikWrapPi(std::atan2(t[2], t[1]) - std::atan2(v[2], v[1]));
    double Rx[9], v1[3];
    bendRot(p, 0.0, 1.0, Rx);
    bendApply(v, Rx, v1);
    const double r = ikWrapPi(std::atan2(t[0], t[1]) - std::atan2(v1[0], v1[1]));
    if (!std::isfinite(p) || !std::isfinite(r)) return false;
    *pitch = p;
    *roll = r;
    return true;
}
// One call's move of the applied (pitch, roll) toward the wanted, capped so the pov moves at most kIkCapPerCallM a call (the lever |N - P|,
// at least 0.1 m).
inline void bendCapToward(double applied[2], const double want[2], double leverM) noexcept {
    const double lever = leverM > 0.1 ? leverM : 0.1;
    const double cap = static_cast<double>(kIkCapPerCallM) / lever;
    const double d[2] = {want[0] - applied[0], want[1] - applied[1]};
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1]);
    const double k = len > cap && len > 0.0 ? cap / len : 1.0;
    applied[0] += d[0] * k;
    applied[1] += d[1] * k;
}

// ---- F59 (TEMPORARY): the bend on the neck, the square-up to the FP forward, the clavicle reach assist, ADS -------------------------------
// (1) The bend turns the NECK joint (34, under the chest 24) toward the eye target less the neck's rest offset (kNeckRestOffset): F58's pov
// joint (15) is a child of the root 7, not under the spine root 16, so every F58 call was refused ("pov, spine level 255"). (2) The square-up
// turns the 3P upper-arm line (U40 -> U41, xz) square to the FP forward (G's z row on xz): theta = yaw(U line) - 90 deg relative to the
// forward; the FP clavicles (7.5 cm apart) were a noisy reference. (3) The clavicle reach assist: before swing 1, when |T - S| > 0.97 (l1 + l2),
// the arm groups gU, gF, gH turn rigidly about the clavicle C so S moves toward T by min(excess + 3 cm, 10 cm), the turn capped at 25 deg;
// in ADS the shoulder's raise and forward push ride the same turn. (4) ADS, blended over 0.15 s: the arm parameters lerp to their ADS values,
// the distance gate relaxes from 0.35..0.8 m to 0.6..1.2 m, and the bend adds a forward lean.
constexpr uint32_t kBendNeckJoint = 34;
constexpr double kIkAssistReach = 0.97, kIkAssistMarginM = 0.03, kIkAssistMaxM = 0.10, kIkAssistMaxDeg = 25.0;
constexpr double kIkAdsRampSec = 0.15;
constexpr float kIkGateFullAdsM = 0.6f, kIkGateZeroAdsM = 1.2f;
// F62a: the ADS shoulder-forward is 0 (it was 5 cm: in ADS the right arm folded to the 40 deg limit). F96: the ADS values are constants now
// (their config keys were removed). kIkAdsSquareAmount stays in the ADS blend because the square-up reads the amount whenever it is not armed.
// kIkAdsLeanDeg stays in the bend: the bend weight is 1 - detachW, still above zero while the body detaches, so the lean is not a no-op there.
constexpr double kIkAdsSquareAmount = 1.0, kIkAdsElbowOutCm = 6.0, kIkAdsTrimDeg = 3.0, kIkAdsRaiseCm = 5.5, kIkAdsForwardCm = 0.0, kIkAdsLeanDeg = 8.0;

inline double ikLerp(double a, double b, double w) noexcept { return a + (b - a) * w; }
// The per-arm distance gate with the ADS weight: full under lerp(0.35, 0.6, w), none from lerp(0.8, 1.2, w), linear between (NaN: 0).
inline float ikDistanceGateAds(float dist, float adsW) noexcept {
    if (!(dist == dist)) return 0.0f;
    const float w = adsW < 0.0f ? 0.0f : adsW > 1.0f ? 1.0f : adsW;
    const float full = kIkGateFullM + (kIkGateFullAdsM - kIkGateFullM) * w, zero = kIkGateZeroM + (kIkGateZeroAdsM - kIkGateZeroM) * w;
    if (dist <= full) return 1.0f;
    if (dist >= zero) return 0.0f;
    return (zero - dist) / (zero - full);
}
// (2) The yaw (radians, ikYawFromTo's sense) that turns the upper-arm line uR - uL (xz) square to the forward fwd (xz): onto the forward's
// right (f.z, 0, -f.x) in the 3P model (x right, y up, z forward). theta (the log's) = -phi = yaw(U line) - 90 deg relative to the forward.
// False (phi 0) when the line is under 1 cm or the forward's xz under 0.1.
inline bool ikSquareToForward(const double uL[3], const double uR[3], const double fwd[3], double* phi) noexcept {
    *phi = 0.0;
    const double a[3] = {uR[0] - uL[0], 0.0, uR[2] - uL[2]}, right[3] = {fwd[2], 0.0, -fwd[0]};
    if (!(ikLen(a) > 0.01) || !(ikLen(right) > 0.1)) return false;
    *phi = ikYawFromTo(a, right);
    return std::isfinite(*phi);
}
// (3) The reach part of the assist (metres): min(excess + 3 cm, 10 cm) when dist = |T - S| is past 0.97 (l1 + l2), excess the part past that;
// 0 otherwise. *excess the excess (0 when in reach).
inline double ikAssistReachMove(double dist, double l1, double l2, double* excess) noexcept {
    const double over = dist - kIkAssistReach * (l1 + l2);
    if (excess) *excess = over > 0.0 ? over : 0.0;
    if (!(over > 0.0)) return 0.0;
    const double m = over + kIkAssistMarginM;
    return m < kIkAssistMaxM ? m : kIkAssistMaxM;
}
// The assist's rotation (row-vector, about the clavicle C) that turns S toward S + disp, at most kIkAssistMaxDeg. False (R identity, angle 0)
// when disp is under 1 mm or the turn is degenerate (S on C, or disp along C -> S).
inline bool ikAssistRot(const double C[3], const double S[3], const double disp[3], double R[9], double* angRad, bool* capped) noexcept {
    for (int i = 0; i < 9; ++i) R[i] = (i % 4 == 0) ? 1.0 : 0.0;
    *angRad = 0.0;
    if (capped) *capped = false;
    if (!(ikLen(disp) > 1e-3)) return false;
    const double cs[3] = {S[0] - C[0], S[1] - C[1], S[2] - C[2]}, cs1[3] = {cs[0] + disp[0], cs[1] + disp[1], cs[2] + disp[2]};
    double a[3], b[3], k[3];
    if (!ikUnit(cs, a) || !ikUnit(cs1, b)) return false;
    ikCross(a, b, k);
    const double s = ikLen(k), c = ikDot(a, b);
    if (!(s > 1e-9)) return false;
    double ang = std::atan2(s, c);
    const double lim = kIkAssistMaxDeg * kIkPi / 180.0;
    if (ang > lim) {
        ang = lim;
        if (capped) *capped = true;
    }
    const double n[3] = {k[0] / s, k[1] / s, k[2] / s};
    ikRotAxis(n, ang, R);
    *angRad = ang;
    return true;
}

// ---- F66: native on foot, one solve with exogenous inputs only ---------------------------------------------------------------------------
// (reviews\native-onfoot-body-review-2026-10-16.md sections 2 and 3; docs\design-native-onfoot-vr-2026-10-13.md, F66.) The F60 weapon fit
// (fix.native_on_foot_body_fit_weapon and _fit_lateral: fitApply, FitAdsLat, adsFitDetach) is gone, and with it the bend's limits switching on
// the fit's weight and the bend's suspension: they closed a loop (the bend moved the shoulders, the reach limit read the moved shoulders, the
// published d fed the bend's target) with a discrete switch in it, and the F65 flight measured the 2-frame limit cycle that makes. The rule that
// replaces them: every stage reads only samples the game made (its animation, the FP rig, the eye, its armed and ADS state) or the output of an
// earlier stage in the same pass. Body axes: right, up, forward.

inline double unit01(double v) noexcept { return !(v == v) ? 0.0 : v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v; }
// 0 at or below e0, 1 at or above e1, 3t^2 - 2t^3 between (NaN: 0).
inline double smoothstep3(double e0, double e1, double x) noexcept {
    if (!(x == x)) return 0.0;
    double t = (x - e0) / (e1 - e0);
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    return t * t * (3.0 - 2.0 * t);
}

// ---- F66a: armed from the game, not from hand distance -----------------------------------------------------------------------------------
// Status.json SelectedWeapon (journal_watch.h: kHeldItem for anything but the fists) is the armed flag; the ramp is 0.3 s in and out. The arm
// IK's weight per arm is the ramp itself (0 unarmed whatever the distance gate says, 1 armed): the F65 flight's distance gate read "held" 62 to
// 91 of 90 frames unarmed (the 3P animated hand within about 0.58 m of the FP hand), armed the reach limit and blocked the stand capture.
constexpr double kArmedRampSec = 0.3;
inline float ikArmWeight(float armedRamp) noexcept { return static_cast<float>(unit01(static_cast<double>(armedRamp))); }
struct IkArmedAuthority {
    bool armed = false;
    double ramp = 0.0;
};
// Runtime weapon state comes only from the selected-item decision and its existing ramp.
// Gesture detector data is not an input.
inline IkArmedAuthority ikArmedAuthority(bool selectedItemArmed, double selectedItemRamp) noexcept {
    return {selectedItemArmed, unit01(selectedItemRamp)};
}

// F83: native primary-selection evidence. Journal item serials are deliberately absent: native sink selection,
// sink/inventory identity, and usable source time define the acquisition epoch.
struct NativePrimarySelectionInput {
    bool activityValid = false, sinkRead = false, sinkSignatureValid = false, inventoryRead = false;
    bool selectedRead = false, holsterRead = false, categoryCacheRead = false;
    uintptr_t sink = 0, inventory = 0;
    uint32_t selectedId = 0, holsterId = 0xFFFFFFFFu, categoryId[4] = {};
    uint32_t renderFrame = 0;
    uint64_t qpc = 0;
    bool primaryIntent = false, secondaryIntent = false, toolIntent = false;
};
struct NativePrimarySelectionDecision {
    bool readable = false, suspended = false, primary = false, identityChanged = false, terminate = false;
    bool held = false;   // F99a: the selection is an item in any slot (not the holster, not 0xFFFFFFFF); readable selections only
    bool duplicate = false, reversed = false, frameGap = false, primaryIntent = false;
    uint64_t epoch = 0;
    uint32_t selectedId = 0;
};
class NativePrimarySelectionState {
  public:
    NativePrimarySelectionDecision observe(const NativePrimarySelectionInput& in) noexcept {
        NativePrimarySelectionDecision out;
        out.epoch = epoch_;
        out.primaryIntent = in.primaryIntent;
        const bool readable = in.activityValid && in.sinkRead && in.sinkSignatureValid && in.inventoryRead && in.selectedRead && in.holsterRead &&
                              in.categoryCacheRead && in.sink != 0 && in.inventory != 0 && in.renderFrame != 0 && in.qpc != 0;
        out.readable = readable;
        if (!readable) {
            out.suspended = have_;
            out.terminate = in.sinkRead && !in.sinkSignatureValid && primary_;
            if (out.terminate) primary_ = false;
            out.primary = false;
            out.selectedId = selected_;
            return out;
        }
        const bool sameFrame = have_ && in.renderFrame == frame_ && in.qpc == qpc_;
        if (sameFrame) {
            out.duplicate = true;
            out.primary = primary_;
            out.held = held_;
            out.selectedId = selected_;
            return out;
        }
        if (have_ && (in.renderFrame < frame_ || in.qpc < qpc_)) {
            out.reversed = true;
            out.suspended = true;
            out.primary = primary_;
            out.held = held_;
            out.selectedId = selected_;
            return out;
        }
        out.frameGap = have_ && in.renderFrame > frame_ + 1u;
        const bool uniquePrimary = in.selectedId != in.holsterId && in.categoryId[2] == in.selectedId && in.categoryId[0] != in.selectedId &&
                                   in.categoryId[1] != in.selectedId && in.categoryId[3] != in.selectedId;
        const bool keyChanged = have_ && (in.sink != sink_ || in.inventory != inventory_ || in.selectedId != selected_);
        out.identityChanged = keyChanged;
        // A readable transition between two accepted primaries retires the old epoch,
        // but the new one is eligible to begin its own draw-follow phase.
        out.terminate = primary_ && !uniquePrimary;
        // The first readable selection can already be a primary (for example, the hook is installed
        // while a rifle is equipped). Give that accepted selection its own epoch too.
        if (!have_ || keyChanged) ++epoch_;
        have_ = true;
        primary_ = uniquePrimary;
        held_ = in.selectedId != in.holsterId && in.selectedId != 0xFFFFFFFFu;
        sink_ = in.sink;
        inventory_ = in.inventory;
        selected_ = in.selectedId;
        frame_ = in.renderFrame;
        qpc_ = in.qpc;
        out.primary = primary_;
        out.held = held_;
        out.selectedId = selected_;
        out.epoch = epoch_;
        return out;
    }
    void reset() noexcept {
        have_ = primary_ = held_ = false;
        sink_ = inventory_ = 0;
        selected_ = frame_ = 0;
        qpc_ = 0;
        epoch_ = 0;
    }

  private:
    bool have_ = false, primary_ = false, held_ = false;
    uintptr_t sink_ = 0, inventory_ = 0;
    uint32_t selected_ = 0, frame_ = 0;
    uint64_t qpc_ = 0, epoch_ = 0;
};
struct NativePrimaryPublication { bool primary = false, followEligible = false; };
inline NativePrimaryPublication nativePrimaryPublication(bool previousPrimary, bool previousEligible, bool hadReadable,
                                                         const NativePrimarySelectionDecision& d) noexcept {
    NativePrimaryPublication p{previousPrimary, previousEligible};
    if (d.readable && d.primary && d.identityChanged) return {true, hadReadable};
    if (d.terminate) return {};
    if (d.suspended || !d.readable) return p;
    p.primary = d.primary;
    if (!d.primary) p.followEligible = false;
    else if (!hadReadable) p.followEligible = false;
    else if (d.identityChanged) p.followEligible = true;
    return p;
}
inline bool primaryDrawSourcesCurrent(bool fpRead, bool gRead, uint64_t fpAgeMs, uint64_t gAgeMs, uint64_t fpQpc,
                                      uint64_t gQpc, uint64_t epochQpc, uint64_t nowQpc, uint64_t qpcFreq) noexcept {
    if (!fpRead || !gRead || fpAgeMs > kIkFreshMs || gAgeMs > kIkFreshMs || !fpQpc || !gQpc || !epochQpc || !qpcFreq ||
        fpQpc <= epochQpc || gQpc <= epochQpc || nowQpc < fpQpc || nowQpc < gQpc) return false;
    return nowQpc - fpQpc <= qpcFreq / 10u && nowQpc - gQpc <= qpcFreq / 10u;
}
inline bool nativeJournalPolicyHeld(bool currentEpoch, bool stalePolicyAtEpoch, bool startSerialKnown, uint32_t startSerial,
                                   uint32_t currentSerial) noexcept {
    return currentEpoch && stalePolicyAtEpoch && startSerialKnown && currentSerial == startSerial;
}
inline bool nativePrimaryEpochIdentityAvailable(bool sampleCopied, bool lastAcceptedPrimary, bool primary, bool eligible,
                                                 uint64_t epoch) noexcept {
    return (sampleCopied || lastAcceptedPrimary) && primary && eligible && epoch != 0;
}

struct PrimaryDrawFollowInput {
    bool selectedPrimary = false, suspended = false, terminate = false;
    bool freshFp = false, generationsMatch = false, gameStateValid = false, targetsValid = false;
    bool contactQualified = false;
    uint64_t nativeEpoch = 0, epochQpc = 0, fpQpc = 0;
    uint32_t fpFrame = 0, fpGeneration = 0, threePGeneration = 0;
    double armedWeight = 0.0, normalContactWeight = 0.0;
};
struct PrimaryDrawFollowDecision {
    bool active = false, suspended = false, write = false, completed = false, refused = false;
    double weight = 0.0;
    uint64_t nativeEpoch = 0;
    uint32_t fpFrame = 0;
};
// Draw-follow uses only a new native primary-selection epoch and a fresh same-call FP/G snapshot. It does not create attachment authority.
class PrimaryDrawFollowState {
  public:
    PrimaryDrawFollowDecision step(const PrimaryDrawFollowInput& in) noexcept {
        PrimaryDrawFollowDecision out;
        out.nativeEpoch = in.nativeEpoch;
        out.fpFrame = in.fpFrame;
        if (in.terminate) {
            active_ = false;
            blockedEpoch_ = in.nativeEpoch;
            appliedWeight_ = 0.0;
            return out;
        }
        if (in.suspended) {
            out.active = active_;
            out.suspended = true;
            // The phase survives an unreadable native poll, but its last transform does not.
            // The caller must provide a currently validated FP/G target before any write.
            return out;
        }
        if (!in.selectedPrimary) {
            active_ = false;
            blockedEpoch_ = in.nativeEpoch;
            appliedWeight_ = 0.0;
            return out;
        }
        if (in.nativeEpoch != 0 && in.nativeEpoch != seenEpoch_ && in.nativeEpoch != blockedEpoch_) {
            active_ = true;
            epoch_ = in.nativeEpoch;
            seenEpoch_ = in.nativeEpoch;
            appliedWeight_ = 0.0;
            lastFpQpc_ = 0;
            lastFpFrame_ = 0;
            generationsLatched_ = false;
        }
        if (!active_ || in.nativeEpoch != epoch_ || in.nativeEpoch == blockedEpoch_) return out;
        const bool orderedFresh = in.freshFp && in.fpFrame != 0 && in.epochQpc != 0 && in.fpQpc > in.epochQpc &&
                                  in.fpQpc >= lastFpQpc_ && in.fpFrame >= lastFpFrame_;
        if (!orderedFresh || !in.gameStateValid || !in.targetsValid) {
            out.active = true;
            out.refused = true;
            return out;
        }
        if (generationsLatched_ && (!in.generationsMatch || in.fpGeneration != latchedFpGeneration_ || in.threePGeneration != latchedThreePGeneration_)) {
            active_ = false;
            blockedEpoch_ = in.nativeEpoch;
            appliedWeight_ = 0.0;
            out.active = false;
            out.refused = true;
            return out;
        }
        if (!generationsLatched_ && !in.generationsMatch) {
            out.active = true;
            out.refused = true;
            return out;
        }
        if (!generationsLatched_) {
            latchedFpGeneration_ = in.fpGeneration;
            latchedThreePGeneration_ = in.threePGeneration;
            generationsLatched_ = true;
        }
        lastFpQpc_ = in.fpQpc;
        lastFpFrame_ = in.fpFrame;
        const double armed = unit01(in.armedWeight), normal = unit01(in.normalContactWeight);
        if (in.contactQualified && normal >= std::max(appliedWeight_, armed)) {
            active_ = false;
            out.completed = true;
            return out;
        }
        appliedWeight_ = std::max(appliedWeight_, armed);
        out.active = true;
        out.write = appliedWeight_ > 0.0;
        out.weight = appliedWeight_;
        return out;
    }
    void reset() noexcept {
        active_ = false;
        generationsLatched_ = false;
        epoch_ = blockedEpoch_ = seenEpoch_ = lastFpQpc_ = 0;
        lastFpFrame_ = latchedFpGeneration_ = latchedThreePGeneration_ = 0;
        appliedWeight_ = 0.0;
    }

  private:
    bool active_ = false, generationsLatched_ = false;
    uint64_t epoch_ = 0, blockedEpoch_ = 0, seenEpoch_ = 0, lastFpQpc_ = 0;
    uint32_t lastFpFrame_ = 0, latchedFpGeneration_ = 0, latchedThreePGeneration_ = 0;
    double appliedWeight_ = 0.0;
};
// The old reading of "held" (smoothstep(0.5, 1.0, the smaller grip gate) above 0), kept as a logged measure of how often it was wrong.
inline bool distanceGateHeld(double gateMin) noexcept { return gateMin == gateMin && gateMin > 0.5; }

// ---- F66c: the bend without a loop -------------------------------------------------------------------------------------------------------
// The limits are interpolated by the armed ramp (unarmed F58's forward 45, back 15, roll 20 deg; armed F62a's 8, 8, 6), never switched by
// anything the bend moves. The suspension (a discrete switch at |T - N| = 0.35 m) is a continuous weight, 1 - smoothstep(0.30, 0.45, |T - N0|),
// N0 the neck before the bend.
constexpr double kBendArmedFwdMaxDeg = 8.0, kBendArmedBackMaxDeg = 8.0, kBendArmedRollMaxDeg = 6.0;
inline void bendLimitsArmed(double armedRamp, double* fwdDeg, double* backDeg, double* rollDeg) noexcept {
    const double w = unit01(armedRamp);
    *fwdDeg = kBendFwdMaxDeg + (kBendArmedFwdMaxDeg - kBendFwdMaxDeg) * w;
    *backDeg = kBendBackMaxDeg + (kBendArmedBackMaxDeg - kBendBackMaxDeg) * w;
    *rollDeg = kBendRollMaxDeg + (kBendArmedRollMaxDeg - kBendRollMaxDeg) * w;
}
constexpr double kBendWeightLoM = 0.30, kBendWeightHiM = 0.45;
inline double bendResidualWeight(double residualM) noexcept {
    if (!std::isfinite(residualM)) return 0.0;
    return 1.0 - smoothstep3(kBendWeightLoM, kBendWeightHiM, residualM);
}
inline double bendResidualWeight(const double T[3], const double N[3]) noexcept {
    const double v[3] = {T[0] - N[0], T[1] - N[1], T[2] - N[2]};
    return bendResidualWeight(ikLen(v));
}
// The reach constraint reads U*, never the bent shoulder: a point of the unbent pose turned about the vertical through `pivot` by the
// square-up's angle (what the square-up does to everything under the chest 24: it turns the stage-0 pose about joint 34's vertical).
inline void turnAboutVertical(const double p[3], const double pivot[3], double angleRad, double out[3]) noexcept {
    const double up[3] = {0.0, 1.0, 0.0};
    double R[9];
    ikRotAxis(up, angleRad, R);
    const double q[3] = {p[0] - pivot[0], p[1] - pivot[1], p[2] - pivot[2]};
    for (int c = 0; c < 3; ++c) out[c] = q[0] * R[c] + q[1] * R[3 + c] + q[2] * R[6 + c] + pivot[c];
}

// ---- F66b: the frame clock ----------------------------------------------------------------------------------------------------------------
// dt from QueryPerformanceCounter ticks, never GetTickCount64: the ms clock ticks at 15.6 ms (64 Hz) and about 29% of 90 Hz ticks read dt = 0,
// so cdStep took no step on 2 ticks of 3 while the view moved every frame. 0 for the first tick (no previous) or a clock that did not advance;
// capped at capSec (a stall is not a long step).
inline double qpcDtSec(uint64_t prevTicks, uint64_t nowTicks, uint64_t freq, double capSec) noexcept {
    if (!prevTicks || !freq || nowTicks <= prevTicks) return 0.0;
    const double dt = static_cast<double>(nowTicks - prevTicks) / static_cast<double>(freq);
    return dt > capSec ? capSec : dt;
}

// ---- F66d: the elbow keeps its hinge --------------------------------------------------------------------------------------------------------
// The F56..F65 solve turned the upper arm by the shortest arc from the animated elbow direction to the solved one (swing 1) and the forearm by
// the shortest arc about E (swing 2), so the forearm bent about eh x eT, which is generally not the elbow's hinge. Here: the animated hinge is
// n0 = unit((E0 - S) x (H0 - E0)) and the solved hinge n1 = unit((E - S) x (T - E)), both the positive flexion normal. Swing 1 turns the upper
// arm's subtree about S by the rotation taking the frame (u0, n0, u0 x n0) to (u1, n1, u1 x n1) (u = the unit upper-arm direction); after it the
// forearm direction h1 and the target direction eT both lie in the plane perpendicular to n1, so swing 2 is a rotation about n1 alone, by the
// signed flexion difference. A nearly straight animated arm (the cross under kHingeMinSin of |u||h|) has no hinge: the learned hinge row, else the
// last solved hinge, else the shortest arc (n0 taken so that the swing is the shortest one) stands in; with a stand-in the animated forearm's lean (under 2.87
// degrees) is not in the plane of n1, so swing 2 is then the shortest arc onto the target direction and the hand lands on the grip (F126). Row-vector rotations
// throughout.
constexpr double kHingeMinSin = 0.05;
constexpr double kHingeRowTolDeg = 10.0;
constexpr double kHingeHintPerp = 0.2;   // a hint's part perpendicular to the upper arm must be at least this to be used
inline bool ikUseHingeRow(bool haveRow, bool geo, double rowAngleDeg, bool stablePoleSide) noexcept {
    return haveRow && (!geo || (!stablePoleSide && rowAngleDeg > kHingeRowTolDeg));
}
inline void ikRowRot(const double v[3], const double R[9], double out[3]) noexcept {
    for (int c = 0; c < 3; ++c) out[c] = v[0] * R[c] + v[1] * R[3 + c] + v[2] * R[6 + c];
}
// The animated hinge from the joints alone; false when the arm is nearly straight.
inline bool ikHingeAnimated(const double S[3], const double F0[3], const double H0[3], double n0[3]) noexcept {
    const double a[3] = {F0[0] - S[0], F0[1] - S[1], F0[2] - S[2]}, b[3] = {H0[0] - F0[0], H0[1] - F0[1], H0[2] - F0[2]};
    double c[3];
    ikCross(a, b, c);
    const double la = ikLen(a), lb = ikLen(b), lc = ikLen(c);
    if (!(la > 1e-6) || !(lb > 1e-6) || !(lc > kHingeMinSin * la * lb)) return false;
    for (int i = 0; i < 3; ++i) n0[i] = c[i] / lc;
    return true;
}
// The angle between two unit axes, modulo sign (0..90 degrees).
inline double ikAxisAngleDeg(const double a[3], const double b[3]) noexcept {
    double c = std::fabs(ikDot(a, b));
    c = c > 1.0 ? 1.0 : c;
    return std::acos(c) * 180.0 / kIkPi;
}
// The rotation taking the frame (a0, b0, a0 x b0) to (a1, b1, a1 x b1), all unit and a perpendicular to b: R = sum_k f0_k^T f1_k (v x R).
inline void ikFrameMap(const double a0[3], const double b0[3], const double a1[3], const double b1[3], double R[9]) noexcept {
    double c0[3], c1[3];
    ikCross(a0, b0, c0);
    ikCross(a1, b1, c1);
    const double* f0[3] = {a0, b0, c0};
    const double* f1[3] = {a1, b1, c1};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) R[r * 3 + c] = f0[0][r] * f1[0][c] + f0[1][r] * f1[1][c] + f0[2][r] * f1[2][c];
}
struct HingeSwings {
    bool ok = false;
    int source = 0;                    // n0 from: 0 the animated joints' cross, 1 the last solved hinge, 2 the shortest arc, 3 the given row
    double n0[3] = {}, n1[3] = {};
    double R1[9] = {}, R2[9] = {};     // swing 1 about S (the whole arm), swing 2 about E (the forearm and hand); about n1 outside the near-straight window
    double flexAnimDeg = 0.0, flexSolvedDeg = 0.0;
    double planeDeg = 0.0;             // the twist about the upper arm the shortest-arc swing would have missed: angle(n0 turned by it, n1)
    double oldHingeErrDeg = 0.0;       // the old two swings' forearm axis against the hinge their swing 1 dragged along (computed, not applied)
};
// S the shoulder, F0 and H0 the animated elbow and hand, E the solved elbow, T the solved hand (|T - E| = l2). n0Given (unit; optional) is a
// learned hinge row signed to agree with the animated cross; lastHinge (unit; optional) is the last call's n1. Neither is used while the animated
// cross is defined and n0Given is null.
inline HingeSwings ikHingeSwings(const double S[3], const double F0[3], const double H0[3], const double E[3], const double T[3], const double* n0Given,
                                 const double* lastHinge) noexcept {
    HingeSwings o;
    const double a[3] = {F0[0] - S[0], F0[1] - S[1], F0[2] - S[2]}, h0[3] = {H0[0] - F0[0], H0[1] - F0[1], H0[2] - F0[2]};
    const double e1[3] = {E[0] - S[0], E[1] - S[1], E[2] - S[2]}, t[3] = {T[0] - E[0], T[1] - E[1], T[2] - E[2]};
    double u0[3], u1[3], eT[3], c1[3];
    if (!ikUnit(a, u0) || !ikUnit(e1, u1) || !ikUnit(t, eT)) return o;
    ikCross(u1, eT, c1);
    if (!ikUnit(c1, o.n1)) return o;
    double R1s[9];
    ikRotFromTo(u0, u1, R1s);
    const auto perp = [&](const double* hint, double out[3]) noexcept {
        const double d = ikDot(hint, u0);
        const double p[3] = {hint[0] - d * u0[0], hint[1] - d * u0[1], hint[2] - d * u0[2]};
        return ikLen(p) >= kHingeHintPerp && ikUnit(p, out);
    };
    double n0g[3];
    const bool geo = ikHingeAnimated(S, F0, H0, n0g);
    if (n0Given && perp(n0Given, o.n0)) {
        o.source = 3;
    } else if (geo) {
        for (int i = 0; i < 3; ++i) o.n0[i] = n0g[i];
        o.source = 0;
    } else if (lastHinge && perp(lastHinge, o.n0)) {
        o.source = 1;
    } else {
        for (int j = 0; j < 3; ++j) o.n0[j] = R1s[j * 3] * o.n1[0] + R1s[j * 3 + 1] * o.n1[1] + R1s[j * 3 + 2] * o.n1[2];   // n1 turned back by the shortest arc
        o.source = 2;
    }
    ikFrameMap(u0, o.n0, u1, o.n1, o.R1);
    double h1[3], h1u[3];
    ikRowRot(h0, o.R1, h1);
    if (!ikUnit(h1, h1u)) return o;
    if (geo) {
        // Outside the near-straight window, preserve the selected hinge's flexion: normally the animated hinge, or the learned row deliberately selected by
        // the unarmed policy (F80). The near-straight correction below must not override that choice when a geometric hinge is defined.
        double cx[3];
        ikCross(h1u, eT, cx);
        ikRotAxis(o.n1, std::atan2(ikDot(o.n1, cx), ikDot(h1u, eT)), o.R2);
    } else {
        // F126: inside the near-straight window, a stand-in hinge or approximate learned row need not describe the animated bend. The carried forearm is then out of n1's plane (up to l2 x 0.05 =
        // 14 mm at the hand in the near-straight window), so a rotation about n1 cannot reach the target. Swing 2 is the shortest arc onto the target direction:
        // the same rotation about n1 when the arm is straight or the hint was right, and the hand lands while both segment lengths stay unchanged.
        ikRotFromTo(h1u, eT, o.R2);
    }
    const auto flexDeg = [](const double u[3], const double h[3]) noexcept {
        double hu[3];
        if (!ikUnit(h, hu)) return 0.0;
        double c = ikDot(u, hu);
        c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
        return std::acos(c) * 180.0 / kIkPi;
    };
    o.flexAnimDeg = flexDeg(u0, h0);
    o.flexSolvedDeg = flexDeg(u1, t);
    // The old method's error: its swing 1 is the shortest arc R1s, the hinge it drags is n0 R1s, and its swing 2 bends about eh x eT.
    double n0s[3], eh0[3], eh[3], a2[3], a2u[3];
    ikRowRot(o.n0, R1s, n0s);
    o.planeDeg = std::acos(std::fmax(-1.0, std::fmin(1.0, ikDot(n0s, o.n1)))) * 180.0 / kIkPi;
    ikRowRot(h0, R1s, eh0);
    if (ikUnit(eh0, eh)) {
        ikCross(eh, eT, a2);
        o.oldHingeErrDeg = ikUnit(a2, a2u) ? ikAxisAngleDeg(a2u, n0s) : 0.0;
    }
    o.ok = true;
    return o;
}
// The hand's twist about the forearm folded into +-90 degrees, continuous around the whole circle: the twist itself up to 90, then back to 0 at
// 180 (the wrist carries the rest). The old wrap to (-180, 180] jumped the helpers' roll by up to 360 t at the wrap.
inline double ikTwistFold(double theta) noexcept {
    theta = ikWrapPi(theta);
    const double a = std::fabs(theta);
    const double f = a <= 0.5 * kIkPi ? a : kIkPi - a;
    return theta < 0.0 ? -f : f;
}
inline bool ikTwistLimited(double theta) noexcept { return std::fabs(ikWrapPi(theta)) > 0.5 * kIkPi; }
// Which of a joint matrix's three rotation rows (row-vector 4x4: rows 0..2) is parallel to the unit axis n (modulo sign): the row, the angle in
// degrees and the sign that makes the row agree with n. -1 when a row is degenerate.
inline int ikHingeRow(const float M[16], const double n[3], double* angleDeg, double* sign) noexcept {
    int best = -1;
    double bestC = -1.0, bestSign = 1.0;
    for (int r = 0; r < 3; ++r) {
        const double v[3] = {M[4 * r], M[4 * r + 1], M[4 * r + 2]};
        double u[3];
        if (!ikUnit(v, u)) return -1;
        const double d = ikDot(u, n);
        if (std::fabs(d) > bestC) {
            bestC = std::fabs(d);
            best = r;
            bestSign = d < 0.0 ? -1.0 : 1.0;
        }
    }
    if (angleDeg) *angleDeg = std::acos(bestC > 1.0 ? 1.0 : bestC) * 180.0 / kIkPi;
    if (sign) *sign = bestSign;
    return best;
}


// ---- F61 (TEMPORARY instrument, fix.native_on_foot_rot_calibrate): does a seated rotation reach the game camera's orientation? -----------
// The schedule: four 1 s steps, repeating: a seated yaw of +5 deg about the head, rest, a pitch of +5 deg about the head's right axis, rest.
// No loop and no guard: the rotation is scheduled, never driven by an error. The rest is zero rotation.
constexpr double kRotCalDeg = 5.0;
constexpr uint64_t kRotCalStepMs = 1000;
constexpr int kRotCalSteps = 4;
constexpr double kRotCalReachDeg = 1.0;   // a residual under this reads "no"
inline int rotCalStep(uint64_t elapsedMs) noexcept { return static_cast<int>((elapsedMs / kRotCalStepMs) % static_cast<uint64_t>(kRotCalSteps)); }
// The commanded seated rotation for a step, degrees: yaw (the seat's sign: about the vertical, counter-clockwise from above) and pitch (about
// the head's right axis, nose up positive).
inline void rotCalCommandDeg(int step, double* yawDeg, double* pitchDeg) noexcept {
    *yawDeg = step == 0 ? kRotCalDeg : 0.0;
    *pitchDeg = step == 2 ? kRotCalDeg : 0.0;
}
inline const char* rotCalStepName(int step) noexcept {
    return step == 0 ? "yaw +5 deg" : step == 2 ? "pitch +5 deg" : step == 1 ? "rest (after yaw)" : "rest (after pitch)";
}
// An orientation as three axes (right, up, forward) in one source's own coordinates, summed over a window. Every source is read so that
// right is screen right, up screen up and forward the view direction, so a delta means the same thing whatever the source's handedness.
struct RotFrameSum {
    double r[3] = {}, u[3] = {}, f[3] = {};
    uint32_t n = 0;
    void add(const double rr[3], const double uu[3], const double ff[3]) noexcept {
        for (int i = 0; i < 3; ++i) {
            r[i] += rr[i];
            u[i] += uu[i];
            f[i] += ff[i];
        }
        ++n;
    }
};
inline double rotDot(const double a[3], const double b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline bool rotUnit(const double a[3], double out[3]) noexcept {
    const double l = std::sqrt(rotDot(a, a));
    if (!(l > 1e-9)) return false;
    for (int i = 0; i < 3; ++i) out[i] = a[i] / l;
    return true;
}
// a less its part along the unit f, made unit.
inline bool rotOrtho(const double a[3], const double f[3], double out[3]) noexcept {
    const double d = rotDot(a, f);
    const double v[3] = {a[0] - d * f[0], a[1] - d * f[1], a[2] - d * f[2]};
    return rotUnit(v, out);
}
// The rotation from window b's mean orientation to window a's, in b's own axes, degrees: yaw positive = the view turned LEFT (about up,
// counter-clockwise seen from above: the seat's yaw sign), pitch positive = the view turned UP (about right). False when a window is empty or
// its axes degenerate.
inline bool rotFrameDelta(const RotFrameSum& b, const RotFrameSum& a, double* dYawDeg, double* dPitchDeg) noexcept {
    *dYawDeg = *dPitchDeg = 0.0;
    if (b.n == 0 || a.n == 0) return false;
    double fb[3], rb[3], ub[3], fa[3];
    if (!rotUnit(b.f, fb) || !rotOrtho(b.r, fb, rb) || !rotOrtho(b.u, fb, ub) || !rotUnit(a.f, fa)) return false;
    const double x = rotDot(fa, rb), y = rotDot(fa, ub), z = rotDot(fa, fb);
    const double kDeg = 180.0 / 3.14159265358979323846;
    *dYawDeg = std::atan2(-x, z) * kDeg;
    *dPitchDeg = std::atan2(y, std::sqrt(x * x + z * z)) * kDeg;
    return true;
}
// The three sources' axes. The list-B camera C (4x4 row-major, view to world): rows 0, 1 and 2 read as right, up and forward (F24: +row 2
// is forward). False when an axis is degenerate.
inline bool rotAxesFromC(const float C[16], double r[3], double u[3], double f[3]) noexcept {
    for (int i = 0; i < 3; ++i) {
        r[i] = C[i];
        u[i] = C[4 + i];
        f[i] = C[8 + i];
    }
    double t[3];
    return rotUnit(r, t) && rotUnit(u, t) && rotUnit(f, t);
}
// The scene rows cb1 270..273 (worldClip's layout: clip_j = x rows[j] + y rows[4 + j] + z rows[8 + j] + rows[12 + j]). Forward is the clip w
// column; right and up are the clip x and y columns less their part along forward (screen right and up by definition of clip space).
inline bool rotAxesFromClip(const float rows[16], double r[3], double u[3], double f[3]) noexcept {
    const double cx[3] = {rows[0], rows[4], rows[8]}, cy[3] = {rows[1], rows[5], rows[9]}, cw[3] = {rows[3], rows[7], rows[11]};
    return rotUnit(cw, f) && rotOrtho(cx, f, r) && rotOrtho(cy, f, u);
}
// The runtime's render head (3x4 row-major rigid, seated space, OpenXR axes): x right, y up, -z forward (the rotation's columns).
inline bool rotAxesFromHead(const float m[12], double r[3], double u[3], double f[3]) noexcept {
    for (int i = 0; i < 3; ++i) {
        r[i] = m[4 * i + 0];
        u[i] = m[4 * i + 1];
        f[i] = -m[4 * i + 2];
    }
    double t[3];
    return rotUnit(r, t) && rotUnit(u, t) && rotUnit(f, t);
}
// THE VERDICT. A seated rotation of +cmd turns the reported (render) head by -cmd (the seat moves, the head stays: lookPoll's rule, F22), so
// the head's own motion over the step is render + cmd. The residual is the camera's change less the head's own motion; it reaches the camera
// when the residual is 1 deg or more. gain = |residual / cmd|; sign = the sign of residual / cmd (-1: the camera turned with the reported head,
// against the seated rotation, as a camera that follows the render pose would).
struct RotVerdict {
    bool reaches = false;
    double headDeg = 0.0, residualDeg = 0.0, gain = 0.0;
    int sign = 0;
};
inline RotVerdict rotCalVerdict(double camDeg, double renderDeg, double cmdDeg) noexcept {
    RotVerdict v;
    v.headDeg = renderDeg + cmdDeg;
    v.residualDeg = camDeg - v.headDeg;
    v.reaches = std::fabs(v.residualDeg) >= kRotCalReachDeg && std::fabs(cmdDeg) > 1e-9;
    if (std::fabs(cmdDeg) > 1e-9) {
        const double ratio = v.residualDeg / cmdDeg;
        v.gain = std::fabs(ratio);
        v.sign = ratio > 0.0 ? 1 : ratio < 0.0 ? -1 : 0;
    }
    return v;
}

// ---- F62b (TEMPORARY, fix.native_on_foot_ads_head_aim): the ADS head-aim ---------------------------------------------------------------
// Conventions (radians unless a name says Deg):
//  - The seat convention: yaw + = turned LEFT (about up, counter-clockwise seen from above: the seat's yaw sign), pitch + = nose UP.
//  - F61 (measured: gain 1.00, sign -): a seated rotation of +a moves the game camera by -a. So a seated +a cancels an aim turn of +a.
//  - The game's yaw delta (comp+0x624) is positive for a turn to the RIGHT when the yaw sign is +1 (the default, ASSUMED: the mouse's +x);
//    its pitch delta (comp+0x628) is positive UP when the pitch sign is +1 (the default, ASSUMED). A sign of 0 turns that axis off.
//  - F67a: nothing is cancelled in the seated space and no seated rotation is published. The view is the headset's whatever the aim does (the F66
//    flight 113909: the game camera never takes the injected aim, so F62b's cancel was the only thing turning the view, and held it still).
constexpr double kAdsPi = 3.14159265358979323846;
constexpr double kAdsDeg = kAdsPi / 180.0;
constexpr double kAdsRampInSec = 0.15, kAdsRampOutSec = 0.3;   // the weight's ramp into ADS and out of it
constexpr double kAdsHeadJumpRad = 30.0 * kAdsDeg;   // a head sample this far from the last is a discontinuity (a LOCAL recentre), not motion
constexpr double kAdsHeadJumpM = 0.5;
constexpr double kAdsGainMin = 1e-3, kAdsGainMax = 1e3;   // a measured gain outside this (or not finite) reads as the fallback 1.0
constexpr double kAdsPendingMaxRad = 20.0 * kAdsDeg;      // the input queued for the game's next look update, at most this much turn

inline double adsWrap(double a) noexcept {
    if (!(a == a)) return 0.0;
    while (a > kAdsPi) a -= 2.0 * kAdsPi;
    while (a < -kAdsPi) a += 2.0 * kAdsPi;
    return a;
}
// The weight: up to 1 over kAdsRampInSec while on, down to 0 over kAdsRampOutSec while off.
struct AdsRamp {
    double w = 0.0;
    void step(bool on, double dt) noexcept {
        const double t = dt > 0.0 ? dt : 0.0;
        w += on ? t / kAdsRampInSec : -t / kAdsRampOutSec;
        w = w < 0.0 ? 0.0 : w > 1.0 ? 1.0 : w;
    }
};
// The head's heading (the seat's yaw sign) and elevation (nose up +) from a 3x4 row-major rigid transform in OpenXR axes (x right, y up,
// -z forward; the forward is minus the rotation's third column). False when degenerate or within 11 deg of vertical (no heading there).
inline bool adsHeadAngles(const float m[12], double* yaw, double* pitch) noexcept {
    const double f[3] = {-static_cast<double>(m[2]), -static_cast<double>(m[6]), -static_cast<double>(m[10])};
    const double l = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(l > 1e-6)) return false;
    const double fy = f[1] / l;
    if (std::fabs(fy) > 0.98) return false;
    *yaw = std::atan2(-f[0], -f[2]);
    *pitch = std::asin(fy);
    return true;
}
// The head's turn from prev to now (seat convention, the yaw wrapped). False when either is unusable or the step is a discontinuity.
inline bool adsHeadDelta(const float prev[12], const float now[12], double* dYaw, double* dPitch) noexcept {
    double y0 = 0.0, p0 = 0.0, y1 = 0.0, p1 = 0.0;
    *dYaw = *dPitch = 0.0;
    if (!adsHeadAngles(prev, &y0, &p0) || !adsHeadAngles(now, &y1, &p1)) return false;
    const double dy = adsWrap(y1 - y0), dp = p1 - p0;
    const double dx = static_cast<double>(now[3]) - prev[3], dyy = static_cast<double>(now[7]) - prev[7], dz = static_cast<double>(now[11]) - prev[11];
    if (std::fabs(dy) > kAdsHeadJumpRad || std::fabs(dp) > kAdsHeadJumpRad || std::sqrt(dx * dx + dyy * dyy + dz * dz) > kAdsHeadJumpM) return false;
    *dYaw = dy;
    *dPitch = dp;
    return true;
}
// Seat convention <-> the game's deltas. Yaw: the game's + is right for sign +1, the seat's + is left, so each is minus sign times the other.
// Pitch: both + up for sign +1. A sign of 0 is the axis off (0 both ways).
inline double adsGameFromSeatYaw(double seat, int sign) noexcept { return sign == 0 ? 0.0 : -static_cast<double>(sign > 0 ? 1 : -1) * seat; }
inline double adsSeatFromGameYaw(double game, int sign) noexcept { return sign == 0 ? 0.0 : -static_cast<double>(sign > 0 ? 1 : -1) * game; }
inline double adsGameFromSeatPitch(double seat, int sign) noexcept { return sign == 0 ? 0.0 : static_cast<double>(sign > 0 ? 1 : -1) * seat; }
inline double adsSeatFromGamePitch(double game, int sign) noexcept { return sign == 0 ? 0.0 : static_cast<double>(sign > 0 ? 1 : -1) * game; }
// The gain in use: the measured one (the game's delta over the input, sign included) when it is finite and within kAdsGainMin..Max in size,
// else 1.0 (the fallback).
inline double adsGain(double measured) noexcept {
    const double a = std::fabs(measured);
    return (measured == measured && a >= kAdsGainMin && a <= kAdsGainMax) ? measured : 1.0;
}
// The look input that asks the game for a delta of wantGame (radians, the game's sign).
inline double adsInput(double wantGame, double measuredGain) noexcept { return wantGame / adsGain(measuredGain); }
// The position lock: the seated offset that holds the eye where it was at entry, w (h - hEntry), h the head in natural LOCAL. The seated origin
// moves with the head, so the reported head (and the game's camera) stays put. In F40's body-axes terms at psi 0 this is D (-e_body), D =
// diag(-1, -1, +1), e_body = diag(1, 1, -1) dH the eye's body motion: the camera's net body motion e_body + D s is zero (adsCamBodyMotion).
inline void adsLockOffset(const double h[3], const double hEntry[3], double w, double out[3]) noexcept {
    const double k = !(w == w) ? 0.0 : w < 0.0 ? 0.0 : w > 1.0 ? 1.0 : w;
    for (int i = 0; i < 3; ++i) out[i] = k * (h[i] - hEntry[i]);
}
// F40's map: the camera's body-axes motion for the head's seated motion dH and a seated offset s: diag(1, 1, -1) dH + D s.
inline void adsCamBodyMotion(const double dH[3], const double s[3], double out[3]) noexcept {
    out[0] = dH[0] - s[0];
    out[1] = dH[1] - s[1];
    out[2] = -dH[2] + s[2];
}

// ---- F63: the ADS sight lock (no key; the F62b head-aim's weight drives it) ------------------------------------------------------------
// The sight line runs through P14w (the FP povCamera joint 14, model space through the full F_root, no minus d) along the FP root's forward f.
// The right eye E_R (F110: the job camera, the LEFT eye, plus the IPD along R^; see below; R^ the FP root's right made perpendicular to f, signed to agree with
// the body's right row). The
// error e = P14w - E_R with its component along f removed (right and up of the sight only), in body axes through the AMC rows (right, up,
// forward), then seated = seatedFromBody(e_body, 0) (diag(-1, -1, +1) . body): the eye alignment's target-minus-camera convention, so the
// view run's filter (gain 0.02 a frame, 5 cm a frame) moves the right eye onto the sight line. The applied lock is clamped to 12 cm.
// F70 REPLACED this: the lock aims at P14w + k f (the along part too), is feed-forward, and is clamped to 40 cm (see the F70 section below).
// F162: "the right eye" here is the AIMING eye: the right eye, or the left eye with fix.native_on_foot_ads_sight_eye = left (adsAimingEyeError, below).
constexpr double kAdsSightClampM = 0.40;   // F70: was 0.12; the target eye point is k ahead of P14 (up to 30 cm) plus the perpendicular and along offsets (about 7 cm each)
// F110: the model of the right eye when its rows are not this frame's. F63 took the job camera for the eyes' midpoint and added half the IPD; F66e found the job
// camera is the LEFT eye (both eyes solved in every ADS frame of the F102 flight over Virtual Desktop: L - cam 0.0 cm, R - cam +6.2 cm, IPD 6.18 cm), so that
// model stood at the midpoint, the nose: a tester's sight on the nose whenever no scene draw gave the eyes' rows (log edvr_gfx_20261025_114257, Quest 3 over
// VDXR: every one of 2248 ADS frames on the model, half IPD 0.0313 m). The right eye is the left eye plus the whole IPD: the right eye's head-space x less the
// left's (native_temporal's s->eyes[e][3]); with only the right eye's offset, twice it; neither (or outside 4..9 cm), the fallback, twice F63's 0.0315.
constexpr double kAdsSightIpdFallbackM = 0.063;   // metres, the left eye to the right when the per-eye offsets are not known
constexpr double kAdsSightIpdMinM = 0.04, kAdsSightIpdMaxM = 0.09;
// The IPD from the eyes' head-space x offsets (right less left, as a distance: the model's direction is R^); *measured false on the fallback.
// eyeScale is Elite's eye pair against the headset's (world scale, common/world_scale.h worldScaleEyeFactor of the percent the runtime was told; 1 at 100): the
// offsets are the pair Elite is given, so the 4..9 cm window and the 6.3 cm fallback are those of that pair.
inline double adsSightIpd(bool haveLeft, double leftEyeX, bool haveRight, double rightEyeX, bool* measured, double eyeScale = 1.0) noexcept {
    double ipd = -1.0;
    if (haveLeft && haveRight && std::isfinite(leftEyeX) && std::isfinite(rightEyeX)) ipd = std::fabs(rightEyeX - leftEyeX);
    else if (haveRight && std::isfinite(rightEyeX)) ipd = 2.0 * std::fabs(rightEyeX);
    const bool ok = ipd >= kAdsSightIpdMinM * eyeScale && ipd <= kAdsSightIpdMaxM * eyeScale;
    if (measured) *measured = ok;
    return ok ? ipd : kAdsSightIpdFallbackM * eyeScale;
}
// Where the model put the right eye: the job camera (the left eye) or the solved left eye (its rows this frame's, the right's not), each plus the IPD along R^.
enum AdsModelSource : uint8_t { kAdsModelFromCamera = 0, kAdsModelFromLeftEye = 1, kAdsModelSources = 2 };
inline uint8_t adsModelRightEye(const double cam[3], bool leftSolved, const double leftEye[3], const double rHat[3], double ipdM, double out[3]) noexcept {
    const double* base = leftSolved ? leftEye : cam;
    for (int i = 0; i < 3; ++i) out[i] = base[i] + ipdM * rHat[i];
    return leftSolved ? kAdsModelFromLeftEye : kAdsModelFromCamera;
}
// R^: row0 less its part along the unit f, unit; flipped (and *flipped set) when it points against bodyRight. False on a degenerate input.
inline bool adsSightRight(const double row0[3], const double f[3], const double bodyRight[3], double out[3], bool* flipped) noexcept {
    double fu[3];
    if (!ikUnit(f, fu)) return false;
    const double d = row0[0] * fu[0] + row0[1] * fu[1] + row0[2] * fu[2];
    const double p[3] = {row0[0] - d * fu[0], row0[1] - d * fu[1], row0[2] - d * fu[2]};
    if (!ikUnit(p, out)) return false;
    const bool flip = out[0] * bodyRight[0] + out[1] * bodyRight[1] + out[2] * bodyRight[2] < 0.0;
    if (flip)
        for (int i = 0; i < 3; ++i) out[i] = -out[i];
    if (flipped) *flipped = flip;
    return true;
}
struct AdsSightErr {
    double eW[3] = {}, ePerpW[3] = {}, eBody[3] = {}, seated[3] = {};
    double along = 0.0, perpNorm = 0.0;   // e . f (metres), |e perpendicular|
    // F70: the error to the target eye point P14w + k f: the perpendicular part AND the along part (along + k), world, body axes, seated.
    double eFullW[3] = {}, eFullBody[3] = {}, seatedFull[3] = {};
    double alongFull = 0.0;               // (P14w + k f - eye) . f
};
// rows: the AMC's 3x3 (row-major: right, up, forward, world). False on a degenerate f.
inline bool adsSightErrorEye(const double p14w[3], const double eye[3], const double f[3], const double rows[9], AdsSightErr& out) noexcept {
    double fu[3];
    if (!ikUnit(f, fu)) return false;
    for (int i = 0; i < 3; ++i) out.eW[i] = p14w[i] - eye[i];
    out.along = out.eW[0] * fu[0] + out.eW[1] * fu[1] + out.eW[2] * fu[2];
    for (int i = 0; i < 3; ++i) out.ePerpW[i] = out.eW[i] - out.along * fu[i];
    out.perpNorm = std::sqrt(out.ePerpW[0] * out.ePerpW[0] + out.ePerpW[1] * out.ePerpW[1] + out.ePerpW[2] * out.ePerpW[2]);
    for (int i = 0; i < 3; ++i)
        out.eBody[i] = out.ePerpW[0] * rows[3 * i] + out.ePerpW[1] * rows[3 * i + 1] + out.ePerpW[2] * rows[3 * i + 2];
    seatedFromBody(out.eBody, 0.0, out.seated);
    return true;
}
// F70: the same with the target P14w + k f: the lock moves the eye onto the sight line AND k forward of P14 along it (k = 0: onto P14 itself, the animator's eye
// point). The perpendicular figures (perpNorm, the growth guard's input) are unchanged.
inline bool adsSightErrorEyeK(const double p14w[3], const double eye[3], const double f[3], const double rows[9], double k, AdsSightErr& out) noexcept {
    if (!adsSightErrorEye(p14w, eye, f, rows, out)) return false;
    double fu[3];
    if (!ikUnit(f, fu)) return false;
    out.alongFull = out.along + k;
    for (int i = 0; i < 3; ++i) out.eFullW[i] = out.ePerpW[i] + out.alongFull * fu[i];
    for (int i = 0; i < 3; ++i) out.eFullBody[i] = out.eFullW[0] * rows[3 * i] + out.eFullW[1] * rows[3 * i + 1] + out.eFullW[2] * rows[3 * i + 2];
    seatedFromBody(out.eFullBody, 0.0, out.seatedFull);
    return true;
}
// The error from a model right eye: the job camera plus camToEyeM along the FP root's right (F110: the IPD, the job camera being the left eye).
inline bool adsSightError(const double p14w[3], const double cam[3], const double rHat[3], double camToEyeM, const double f[3], const double rows[9],
                          AdsSightErr& out) noexcept {
    double eye[3];
    for (int i = 0; i < 3; ++i) eye[i] = cam[i] + camToEyeM * rHat[i];
    return adsSightErrorEye(p14w, eye, f, rows, out);
}
// F162 (fix.native_on_foot_ads_sight_eye): the sight lock lines up the AIMING eye, 1 the right eye (both and right: F63 to F161) or 0 the left eye (left). The job camera is the
// left eye and the right eye is the job camera plus the IPD along R^ (F110), so the same lock puts the left eye on the line with the right eye one IPD to its right.
// The model of the aiming eye when its own rows are not this frame's: aim 1 is adsModelRightEye as it was (bit for bit); aim 0 is the job camera, which is the left eye.
inline uint8_t adsModelAimingEye(int aim, const double cam[3], bool leftSolved, const double leftEye[3], const double rHat[3], double ipdM, double out[3]) noexcept {
    if (aim != 0) return adsModelRightEye(cam, leftSolved, leftEye, rHat, ipdM, out);
    for (int i = 0; i < 3; ++i) out[i] = cam[i];
    return kAdsModelFromCamera;
}
// What the lock took for the aiming eye this frame: its solved rendered eye, else the model; ok is false when neither gave an error (no rows and a degenerate right axis).
struct AdsAimPick {
    bool ok = false;
    bool solved = false;
    uint8_t modelSrc = kAdsModelFromCamera;
};
// The lock's error to the target eye point (origin + k f, F70) for the aiming eye. eyeOk / eyeW are the two eyes' solved positions (index 0 left, 1 right); modelOk says R^ is
// usable (the model right eye needs it). modelEye is written whether or not it is used (the model of the aiming eye, as F110's was written for the right eye).
inline AdsAimPick adsAimingEyeError(int aim, const bool eyeOk[2], const double eyeW[2][3], bool modelOk, const double cam[3], const double rHat[3], double ipdM,
                                    const double origin[3], const double f[3], const double rows[9], double k, AdsSightErr& out, double modelEye[3]) noexcept {
    const int a = aim == 0 ? 0 : 1;
    AdsAimPick pick;
    pick.modelSrc = adsModelAimingEye(a, cam, eyeOk[0], eyeW[0], rHat, ipdM, modelEye);
    if (eyeOk[a] && adsSightErrorEyeK(origin, eyeW[a], f, rows, k, out)) {
        pick.ok = true;
        pick.solved = true;
    } else if (modelOk && adsSightErrorEyeK(origin, modelEye, f, rows, k, out)) {
        pick.ok = true;
    }
    return pick;
}
// |v| at most maxM, the direction kept (the eye alignment's clamps: 0.6 m for the view run, kAdsSightClampM for the sight lock).
inline void clampNorm3(double v[3], double maxM) noexcept {
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > maxM)
        for (int i = 0; i < 3; ++i) v[i] *= maxM / n;
}
// The guard (F62b's residual guard, gone in F67a, had this pattern in degrees): true once |e perpendicular| has stayed at or above the floor and grown by kAdsSightGrowM
// from where the run started over kAdsSightGrowMs, never falling kAdsSightFallM below its peak (a fall restarts the run).
constexpr double kAdsSightFloorM = 0.02, kAdsSightGrowM = 0.01, kAdsSightFallM = 0.005;
constexpr uint64_t kAdsSightGrowMs = 500;
struct AdsSightGrowth {
    bool growing = false;
    uint64_t startMs = 0;
    double startAbs = 0.0, peak = 0.0;
    bool step(uint64_t ms, double absM) noexcept {
        if (!(absM == absM) || absM < kAdsSightFloorM) {
            growing = false;
            return false;
        }
        if (!growing || absM < peak - kAdsSightFallM) {
            growing = true;
            startMs = ms;
            startAbs = peak = absM;
            return false;
        }
        if (absM > peak) peak = absM;
        return ms >= startMs + kAdsSightGrowMs && absM - startAbs >= kAdsSightGrowM;
    }
    uint64_t runMs(uint64_t ms) const noexcept { return growing && ms > startMs ? ms - startMs : 0; }
};

// ---- F70: the ADS eye point (F152: experimental.native_on_foot_ads_eye_offset_cm) ---------------------------------------------------------------
// RE (log 152023, dumps 152230 rifle and 152353 pistol): the exe has no sight, scope or eye joint and no per-weapon ADS camera table; the ADS pose is the FP
// animation's, with joint 14 (def_c_povCamera_joint) as the camera, still computed in VR. The line through P14 along the FP root's forward passes through the optics
// (the rifle's lens dot within 0.6 deg, the pistol's rear sight within 1 deg): P14 is the animator's eye point. The F63 lock removed only the perpendicular part of
// the error, so the rifle eye sat 7.3 cm AHEAD of P14 and the pistol eye 5.7 cm BEHIND it. The sights are 33.3 cm (the rifle's lens ring) and 40.8 cm (the pistol's
// rear sight) ahead of P14; the flat game narrows the FOV to make them look bigger, which VR cannot do, so "closer" is the eye moving forward along the sight line:
// the target eye point is P14w + k f, never closer than kAdsEyeClearanceM to any FP weapon point readable on the line. F149/F150/F152: k is the held weapon's depth plus the live trim,
// then the 0..30 cm clamp (adsEyeDistanceClampCm), then the clearance cap (adsEyeDistanceCap). The weapon's depth is its base, the game's own unscoped ADS camera depth for the item (E's
// forward from P14; one value per item, the scope state ignored), plus its baked offset (native_on_foot_f149.h: 0 except for the two weapons whose camera puts the eye inside the
// gun); an item with no table row has its settled line's forward when the copy is known unscoped, else the fallback base (kAdsEyeDistDefaultCm, the 21 cm every weapon had before F149).
// The one k feeds the eye point, the clearance cap, the error to the target and the sight line's shift (F143's Q).
// The lock's target is absolute: on an active ADS tick with a valid rendered-eye measurement, target = applied + measured error. One critically damped filter
// (tau kAdsEyeTauSec) follows that target; a missing measurement holds the last target. The actual ADS state gates the target, independently of the head-aim ramp.
constexpr double kAdsEyeDistDefaultCm = 21.0, kAdsEyeDistMaxCm = 30.0;   // F98c: the pilot's (15 before); F152: the default is the fallback base, the max the depth's clamp
constexpr double kAdsEyeClearanceM = 0.06;      // the eye stays this far behind any readable FP weapon point on the sight line
constexpr double kAdsEyeOnLineM = 0.06;         // "on the line": a point within this of the sight line, ahead of P14
constexpr double kAdsEyeTauSec = 0.05;          // 95% in about 0.25 s (tau 0.07 would take 0.33 s)
constexpr double kAdsEyeCapPerFrameM = 0.08;    // the shift moves at most this much a frame
// ADS can receive the align clock's full 50 ms stall cap (dt/tau = 1), where the shared semi-implicit
// cdStep is unstable. Keep the exact critically damped update local to this filter; retain its explicit
// per-axis displacement cap and carry the analytic velocity through a clipped move.
inline void adsEyeCdStep(double& pos, double& vel, double target, double tau, double dt, double cap) noexcept {
    if (dt <= 0.0) return;
    const double omega = 1.0 / tau;
    const double x = pos - target;
    const double c = vel + omega * x;
    const double decay = std::exp(-omega * dt);
    const double nextX = (x + c * dt) * decay;
    const double nextVel = (vel - omega * c * dt) * decay;
    double move = (target + nextX) - pos;
    if (move > cap) move = cap;
    if (move < -cap) move = -cap;
    pos += move;
    vel = nextVel;
}
inline double adsEyeDistanceClampCm(double cm) noexcept {
    if (!(cm == cm)) return kAdsEyeDistDefaultCm;
    return cm < 0.0 ? 0.0 : cm > kAdsEyeDistMaxCm ? kAdsEyeDistMaxCm : cm;
}
struct AdsEyeCap {
    double kM = 0.0;       // k after the cap (metres)
    bool capped = false;   // a readable point cut it
    int readable = 0;      // how many points lay ahead of P14 within kAdsEyeOnLineM of the line
    double limitM = 1e9;   // the tightest limit those points gave (the nearest point's along less the clearance)
};
// k capped so the eye (P14w + k f) stays kAdsEyeClearanceM behind every point of pts (world) that is ahead of P14 and within kAdsEyeOnLineM of the sight line.
// With none readable the cap is the depth's own bound (the caller logs it).
inline AdsEyeCap adsEyeDistanceCap(double kWantedM, const double p14w[3], const double f[3], const double pts[][3], int n) noexcept {
    AdsEyeCap c;
    c.kM = kWantedM == kWantedM ? kWantedM : 0.0;
    double fu[3];
    if (!ikUnit(f, fu) || n <= 0) return c;
    for (int i = 0; i < n; ++i) {
        const double v[3] = {pts[i][0] - p14w[0], pts[i][1] - p14w[1], pts[i][2] - p14w[2]};
        const double along = v[0] * fu[0] + v[1] * fu[1] + v[2] * fu[2];
        const double pv[3] = {v[0] - along * fu[0], v[1] - along * fu[1], v[2] - along * fu[2]};
        const double perp = std::sqrt(pv[0] * pv[0] + pv[1] * pv[1] + pv[2] * pv[2]);
        if (!(along > 0.0) || !(perp <= kAdsEyeOnLineM)) continue;
        ++c.readable;
        const double limit = along - kAdsEyeClearanceM;
        if (limit < c.limitM) c.limitM = limit;
    }
    if (c.readable && c.kM > c.limitM) {
        c.kM = c.limitM > 0.0 ? c.limitM : 0.0;
        c.capped = true;
    }
    return c;
}
// One eye-shift step (seated, metres). Active ADS uses the full absolute correction from the latest valid rendered-eye measurement; missing data holds that
// absolute target. Release, a guard trip, or any non-active state selects zero while preserving the filter's position and velocity.
inline void adsEyeStep(double pos[3], double vel[3], double target[3], const double e[3], bool have, bool active, bool tripped, double dt) noexcept {
    if (active && !tripped && have) {
        for (int i = 0; i < 3; ++i) target[i] = pos[i] + e[i];
        clampNorm3(target, kAdsSightClampM);
    } else if (!active || tripped) {
        target[0] = target[1] = target[2] = 0.0;
    }
    for (int i = 0; i < 3; ++i) adsEyeCdStep(pos[i], vel[i], target[i], kAdsEyeTauSec, dt, kAdsEyeCapPerFrameM);
    clampNorm3(pos, kAdsSightClampM);
}
constexpr uint64_t kAdsActionFreshUs = 100000;
constexpr uint64_t kAdsActionRejectGraceMs = 1500;
inline bool adsActionSampleFresh(bool eligible, bool valid, uint64_t stampQpc, uint64_t nowQpc, uint64_t eligibleSinceQpc, uint64_t ageUs) noexcept {
    return eligible && valid && stampQpc && nowQpc >= stampQpc && stampQpc >= eligibleSinceQpc && ageUs <= kAdsActionFreshUs;
}
enum AdsInputSource : int { kAdsInputNone = 0, kAdsInputJournal = 1, kAdsInputAction = 2 };
struct AdsInputChoice {
    bool ads = false, actionFresh = false, journalVeto = false;
    int source = kAdsInputNone;
};
struct AdsActionReject {
    bool latched = false;
    uint64_t edgeMs = 0;
    uint64_t statusRevisionAtEdge = 0;
};
// A veto needs changed Status.json content observed after the request and a 1.5 s acceptance grace. Repeated reads of identical content are not fresh evidence.
// Changed content can still report a slow transition: a later authoritative ADS=true clears the veto while held. It also starts a new grace, so a later game
// rejection (for example a reload) can still veto the held request after a new false status. Content freshness is not proof of native weapon readiness.
inline bool adsActionRejectStep(AdsActionReject& s, bool actionAds, uint64_t nowMs, bool journalKnown, bool journalAds, uint64_t statusRevision) noexcept {
    if (!actionAds) {
        s = AdsActionReject{};
        return false;
    }
    if (journalKnown && journalAds) {
        s.latched = false;
        s.edgeMs = nowMs ? nowMs : 1;
        s.statusRevisionAtEdge = statusRevision;
        return false;
    }
    if (s.latched) return true;
    if (!s.edgeMs) {
        s.edgeMs = nowMs ? nowMs : 1;
        s.statusRevisionAtEdge = statusRevision;
        return false;
    }
    if (journalKnown && !journalAds && statusRevision != 0 && statusRevision != s.statusRevisionAtEdge && nowMs >= s.edgeMs &&
        nowMs - s.edgeMs >= kAdsActionRejectGraceMs) s.latched = true;
    return s.latched;
}
// The bound logical action is the prompt request; the journal is fallback when the action sample is absent/stale. `eligible` is restricted by the caller to an
// active session/on-foot context. A veto holds ADS off until logical release or a later authoritative acceptance.
inline AdsInputChoice adsInputChoice(bool eligible, bool journalKnown, bool journalAds, bool actionValid, bool actionAds, uint64_t actionAgeUs, bool journalVeto) noexcept {
    AdsInputChoice c;
    c.actionFresh = eligible && actionValid && actionAgeUs <= kAdsActionFreshUs;
    c.journalVeto = journalVeto;
    if (c.journalVeto) {
        c.ads = false;
        c.source = kAdsInputJournal;
    } else if (c.actionFresh) {
        c.ads = actionAds;
        c.source = kAdsInputAction;
    } else if (journalKnown) {
        c.ads = journalAds;
        c.source = kAdsInputJournal;
    }
    return c;
}
// The camera's body-axes motion for a seated shift S (F40's plant at psi 0): (-S0, -S1, +S2). The body pin and the head-follow take the camera MINUS this,
// mapped back to the world through the AMC rows (right, up, forward; row-major), so the body stays pinned to the unlocked head and does not slide with the ADS
// eye.
inline void adsCamShiftBody(const double S[3], double out[3]) noexcept {
    out[0] = -S[0];
    out[1] = -S[1];
    out[2] = S[2];
}
inline void adsCamShiftWorld(const double S[3], const float rows[9], double out[3]) noexcept {
    double m[3];
    adsCamShiftBody(S, m);
    for (int c = 0; c < 3; ++c) out[c] = m[0] * rows[c] + m[1] * rows[3 + c] + m[2] * rows[6 + c];
}
inline void adsUnlockedCam(const float cam[3], const double S[3], const float rows[9], float out[3]) noexcept {
    double w[3];
    adsCamShiftWorld(S, rows, w);
    for (int c = 0; c < 3; ++c) out[c] = static_cast<float>(static_cast<double>(cam[c]) - w[c]);
}

// ---- F65a: the connection retry's gate (fix.connection_retry, was fix.training_connect_fix) -----------------------------------------------
// A Deactivate arms the recovery only when the destroyed ConnectToServerActivity had not reached sessions (its state at the destroy,
// [act+0x30], is not 10) and the Deactivate came within 10 s of the destroy. The logs: the 3 real stalls were destroyed in states 5, 3 and 5;
// all 24 normal loads in state 10 (and arming one of those crashed a Solo load: StartPending forced LoadGameActivity during the entry movie).
enum RetryDecision : int { kRetryArm = 0, kRetrySessions = 1, kRetryLate = 2, kRetryUnread = 3 };
constexpr uint32_t kRetrySessionsState = 10;
constexpr uint64_t kRetryDeactWindowMs = 10000;
inline int retryDecision(bool stateRead, uint32_t state, uint64_t delayMs) noexcept {
    if (stateRead && state == kRetrySessionsState) return kRetrySessions;
    if (delayMs > kRetryDeactWindowMs) return kRetryLate;
    if (!stateRead) return kRetryUnread;
    return kRetryArm;
}

// ---- F65a: the left hand locked to the weapon ---------------------------------------------------------------------------------------------
// F66a: the arm's solve weight is the armed ramp (ikArmWeight, above): 1 while the game says a weapon or tool is in hand, 0 unarmed whatever the
// distance gate says (the F65 flight: the gate read "held" 62..91 of 90 frames unarmed). The distance gate stays a logged measure.
// The reach limit: per arm k (0 L, 1 R) v_k = the 3P shoulder (the upper-arm joint) minus its grip target T_A, body axes (right, up, forward)
// at the body shift in effect; R_k = l1 + l2 - kReachMarginM. A body shift by delta moves the shoulder with the body, so the vector becomes
// v_k + delta. reachLimitDelta gives the smallest horizontal delta (its up part 0) with |v_k + delta| <= R_k for every arm it has: 0 when that
// already holds; else the nearer projection onto one arm's disk (centre -v_k horizontal, radius sqrt(R_k^2 - v_k,up^2)) that lies in the
// other's; else the nearer crossing of the two circles. When the disks do not meet (or an arm's vertical gap alone exceeds its reach),
// *feasible is false and delta is the point midway between the disks' nearest boundary points. Returns true when delta is not zero.
constexpr double kReachMarginM = 0.02;
constexpr double kReachRampSec = 0.2;
inline bool reachDisk(const double v[3], double R, double c[2], double* r) noexcept {
    c[0] = -v[0];
    c[1] = -v[2];
    const double r2 = R * R - v[1] * v[1];
    *r = R > 0.0 && r2 > 0.0 ? std::sqrt(r2) : 0.0;
    return R > 0.0 && r2 > 0.0;
}
inline bool reachInDisk(const double p[2], const double c[2], double r) noexcept {
    const double dx = p[0] - c[0], dz = p[1] - c[1];
    return dx * dx + dz * dz <= r * r + 1e-12;
}
inline void reachProjDisk(const double p[2], const double c[2], double r, double out[2]) noexcept {
    const double dx = p[0] - c[0], dz = p[1] - c[1], n = std::sqrt(dx * dx + dz * dz);
    if (n <= r || n < 1e-12) {
        out[0] = n < 1e-12 ? c[0] : p[0];
        out[1] = n < 1e-12 ? c[1] : p[1];
        return;
    }
    out[0] = c[0] + dx * r / n;
    out[1] = c[1] + dz * r / n;
}
inline bool reachLimitDelta(const double v[2][3], const double R[2], const bool have[2], double delta[3], bool* feasible) noexcept {
    delta[0] = delta[1] = delta[2] = 0.0;
    *feasible = true;
    double c[2][2] = {}, r[2] = {};
    bool use[2] = {false, false};
    int n = 0;
    for (int k = 0; k < 2; ++k) {
        use[k] = have[k] && std::isfinite(v[k][0]) && std::isfinite(v[k][1]) && std::isfinite(v[k][2]) && std::isfinite(R[k]);
        if (!use[k]) continue;
        if (!reachDisk(v[k], R[k], c[k], &r[k])) *feasible = false;
        ++n;
    }
    if (!n) return false;
    const double o[2] = {0.0, 0.0};
    bool inAll = true;
    for (int k = 0; k < 2; ++k)
        if (use[k] && !reachInDisk(o, c[k], r[k])) inAll = false;
    if (inAll && *feasible) return false;
    double best[2] = {0.0, 0.0}, bestN = -1.0;
    const auto offer = [&](const double p[2]) noexcept {
        const double nn = p[0] * p[0] + p[1] * p[1];
        if (bestN < 0.0 || nn < bestN) {
            bestN = nn;
            best[0] = p[0];
            best[1] = p[1];
        }
    };
    if (*feasible) {
        for (int k = 0; k < 2; ++k) {
            if (!use[k]) continue;
            double p[2];
            reachProjDisk(o, c[k], r[k], p);
            const int j = 1 - k;
            if (!use[j] || reachInDisk(p, c[j], r[j] * (1.0 + 1e-9))) offer(p);
        }
        if (bestN < 0.0 && use[0] && use[1]) {
            const double ux = c[1][0] - c[0][0], uz = c[1][1] - c[0][1], d = std::sqrt(ux * ux + uz * uz);
            if (d > 1e-12 && d <= r[0] + r[1] && d >= std::fabs(r[0] - r[1])) {
                const double a = (r[0] * r[0] - r[1] * r[1] + d * d) / (2.0 * d);
                const double h2 = r[0] * r[0] - a * a, h = h2 > 0.0 ? std::sqrt(h2) : 0.0;
                const double px = c[0][0] + a * ux / d, pz = c[0][1] + a * uz / d;
                const double p1[2] = {px - h * uz / d, pz + h * ux / d}, p2[2] = {px + h * uz / d, pz - h * ux / d};
                offer(p1);
                offer(p2);
            }
        }
        if (bestN < 0.0) *feasible = false;
    }
    if (!*feasible) {
        if (use[0] && use[1]) {
            const double ux = c[1][0] - c[0][0], uz = c[1][1] - c[0][1], d = std::sqrt(ux * ux + uz * uz);
            const double e[2] = {d > 1e-12 ? ux / d : 0.0, d > 1e-12 ? uz / d : 0.0};
            const double p0[2] = {c[0][0] + e[0] * r[0], c[0][1] + e[1] * r[0]}, p1[2] = {c[1][0] - e[0] * r[1], c[1][1] - e[1] * r[1]};
            best[0] = 0.5 * (p0[0] + p1[0]);
            best[1] = 0.5 * (p0[1] + p1[1]);
        } else {
            for (int k = 0; k < 2; ++k)
                if (use[k]) {
                    best[0] = c[k][0];
                    best[1] = c[k][1];
                }
        }
    }
    delta[0] = best[0];
    delta[2] = best[1];
    return delta[0] != 0.0 || delta[2] != 0.0;
}
// The midpoint returned by reachLimitDelta when arm disks do not intersect is a diagnostic compromise, not a shared body pose that satisfies both
// constraints. Preserve it for reporting, but apply no shared body shift unless the common set is feasible.
inline bool reachLimitDeltaFeasibleOnly(const double v[2][3], const double R[2], const bool have[2], double candidate[3], double applied[3], bool* feasible) noexcept {
    const bool active = reachLimitDelta(v, R, have, candidate, feasible);
    for (int i = 0; i < 3; ++i) applied[i] = *feasible ? candidate[i] : 0.0;
    return active;
}

// ---- F66e: ADS, the rendered eyes --------------------------------------------------------------------------------------------------------
// The F65 flight (log 092626): the lock's model of the right eye, E_R = the job camera + 0.0303 m along the FP root's right, closes on itself:
// the engage ended with e perpendicular at 0.0-0.5 cm and the gun still between the eyes. The rendered eye is where its scene rows (cb1 270..273,
// row-vector form, clip = (p - o) . rows[0..11] + rows[12..15], o = cb1[275]) have clip x, y and w all zero: a 3 x 3 solve. False when the
// system is singular or the result is not finite.
inline bool eyeFromRows(const float rows[16], const double o[3], double eye[3]) noexcept {
    static const int kWhich[3] = {0, 1, 3};
    double M[3][3], b[3];
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) M[j][i] = rows[4 * i + kWhich[j]];
        b[j] = -static_cast<double>(rows[12 + kWhich[j]]);
    }
    const auto det3 = [](const double a[3], const double b2[3], const double c[3]) noexcept {
        return a[0] * (b2[1] * c[2] - b2[2] * c[1]) - a[1] * (b2[0] * c[2] - b2[2] * c[0]) + a[2] * (b2[0] * c[1] - b2[1] * c[0]);
    };
    const double d = det3(M[0], M[1], M[2]);
    double scale = 0.0;
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) scale = std::fmax(scale, std::fabs(M[j][i]));
    if (!(std::fabs(d) > 1e-12 * scale * scale * scale) || !std::isfinite(d)) return false;
    double v[3];
    for (int k = 0; k < 3; ++k) {
        double Mk[3][3];
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i) Mk[j][i] = (i == k) ? b[j] : M[j][i];
        v[k] = det3(Mk[0], Mk[1], Mk[2]) / d;
    }
    for (int i = 0; i < 3; ++i) {
        eye[i] = o[i] + v[i];
        if (!std::isfinite(eye[i])) return false;
    }
    return true;
}
// An eye's place against the sight line (through p14w along the unit f): its perpendicular distance and its distance along f to p14w (positive
// when the sight is in front of the eye).
inline void sightLineOfEye(const double eye[3], const double p14w[3], const double f[3], double* perp, double* along) noexcept {
    double fu[3];
    if (!ikUnit(f, fu)) {
        if (perp) *perp = 0.0;
        if (along) *along = 0.0;
        return;
    }
    const double e[3] = {p14w[0] - eye[0], p14w[1] - eye[1], p14w[2] - eye[2]};
    const double al = ikDot(e, fu);
    const double q[3] = {e[0] - al * fu[0], e[1] - al * fu[1], e[2] - al * fu[2]};
    if (perp) *perp = ikLen(q);
    if (along) *along = al;
}
// The rows are this frame's when their frame counter is at most one behind the boundary that is reading them (the draws of frame N run before
// the boundary that stamps N + 1).
inline bool sceneRowsFreshForLock(uint32_t rowsFrame, uint32_t boundaryFrame) noexcept {
    if (rowsFrame == ~0u) return false;
    const int32_t age = static_cast<int32_t>(boundaryFrame - rowsFrame);
    return age >= 0 && age <= 1;
}

// ---- F66g: frame order and parity ------------------------------------------------------------------------------------------------------
// Counters over a sequence of samples. AltCount: how many times the step from one sample to the next (when it exceeds eps) has the opposite sign of
// the step before it: a signal that flips every frame counts every step, a steady drift counts none. ParityMean: a mean per frame parity.
struct AltCount {
    bool haveLast = false, haveDelta = false;
    double last = 0.0, lastDelta = 0.0;
    uint32_t flips = 0, steps = 0;
    void step(double v, double eps) noexcept {
        if (haveLast) {
            const double d = v - last;
            if (std::fabs(d) > eps) {
                ++steps;
                if (haveDelta && ((d > 0.0) != (lastDelta > 0.0))) ++flips;
                lastDelta = d;
                haveDelta = true;
            }
        }
        last = v;
        haveLast = true;
    }
};
struct ParityMean {
    uint32_t n[2] = {};
    double sum[2] = {};
    void add(uint32_t parity, double v) noexcept {
        const uint32_t p = parity & 1u;
        ++n[p];
        sum[p] += v;
    }
    double mean(int p) const noexcept { return n[p & 1] ? sum[p & 1] / static_cast<double>(n[p & 1]) : 0.0; }
    // even minus odd; 0 when either parity has no sample
    double diff() const noexcept { return n[0] && n[1] ? mean(0) - mean(1) : 0.0; }
};
// The order of three events by their QPC ticks: a 3-letter string over {'P' (the eye probe), 'A' (Animate), 'S' (the submit post-hook)} from first
// to last, ties kept in P, A, S order; "???" when a tick is 0 (the event did not run).
inline void frameOrder3(uint64_t tP, uint64_t tA, uint64_t tS, char out[4]) noexcept {
    out[0] = out[1] = out[2] = '?';
    out[3] = '\0';
    if (!tP || !tA || !tS) return;
    const uint64_t t[3] = {tP, tA, tS};
    const char name[3] = {'P', 'A', 'S'};
    int idx[3] = {0, 1, 2};
    for (int i = 1; i < 3; ++i)
        for (int j = i; j > 0 && t[idx[j]] < t[idx[j - 1]]; --j) {
            const int x = idx[j];
            idx[j] = idx[j - 1];
            idx[j - 1] = x;
        }
    for (int i = 0; i < 3; ++i) out[i] = name[idx[i]];
}
// ---- F68a: the ADS head-aim, F62b's map measured properly ---------------------------------------------------------------------------------------
// (reviews\native-onfoot-body-review-2026-10-16.md section 4; the F67 flight edvr_gfx_20261016_132256.) F67a steered the weapon onto the view with a
// closed loop. It wound up: the actuator (look input) turns both sides of its error, and the camera the F66 guard read, C, is the left eye, not the render
// camera. The rendered view (the right eye's rows) does take the injected aim, so the map is F62b's: the headset's turn since the last frame, times the ADS
// weight and divided by the measured gain, goes to the game as look input; the turn the processor applied for it is cancelled in the seated space, so the
// view stays the headset's whatever the game took, and the weapon follows the head. Seat convention throughout (yaw + left, pitch + up); F61 measured a
// seated +a moving the camera by -a. On release the injected yaw and pitch unwind over kAdsUnwindSec (longer for a large turn: at most 60 deg/s, so the
// frame the cancel runs behind shows as under 1 deg) by injecting the opposite (the cancel follows what the game applies, the view stays the headset's),
// so the runtime's look base returns to zero without a view jump; F62b kept the yaw.
constexpr double kAdsUnwindSec = 0.3;                 // the release's unwind: at least this long ...
constexpr double kAdsUnwindMaxRate = 60.0 * kAdsDeg;  // ... and no faster than this (rad/s): the cancel runs a frame behind the game, so a faster swing
                                                      // would show as a view offset of one frame of the swing (0.67 deg at 60 deg/s) ...
constexpr double kAdsUnwindMaxSec = 1.5;              // ... but never longer than this (a very large carried turn swings back faster)
constexpr double kAdsCatchUpSec = 0.2;                // after the unwind, the most time a remainder is chased before it is dropped
constexpr double kAdsPitchClampRad = 60.0 * kAdsDeg;  // the seated pitch is held to +-60 deg (the asks and the publication; F109c: not the account)
constexpr int kAdsFlightFrames = 8;                   // an ask counts as in flight for this many ticks (queued, applied by the game's next look update, seen)
constexpr double kAdsUnwindDoneRad = 0.05 * kAdsDeg;  // an unwind this close to zero is done
constexpr double kAdsResFloorDeg = 8.0;               // the guard: |view turn - head turn - the player's own| at least this, ...
constexpr double kAdsResRatio = 0.5;                  // ... at least this share of the head's own turn since the base, ...
constexpr double kAdsResGrowDeg = 4.0;                // ... grown by at least this ...
constexpr uint64_t kAdsResGrowMs = 500;               // ... over this long without falling 1 deg below its peak
constexpr double kAdsViewJumpDeg = 15.0;              // a view step this big in one tick (a cut, a recentre) is not a turn: the base is taken again

// The share of a seated pitch step that keeps the pitch inside +-60 deg (a step back toward zero always passes whole).
inline double adsPitchLimit(double accum, double delta) noexcept {
    const double next = accum + delta;
    if (next > kAdsPitchClampRad) return delta > 0.0 ? (accum < kAdsPitchClampRad ? kAdsPitchClampRad - accum : 0.0) : delta;
    if (next < -kAdsPitchClampRad) return delta < 0.0 ? (accum > -kAdsPitchClampRad ? -kAdsPitchClampRad - accum : 0.0) : delta;
    return delta;
}
inline double adsClampPitch(double p) noexcept {
    return !(p == p) ? 0.0 : p > kAdsPitchClampRad ? kAdsPitchClampRad : p < -kAdsPitchClampRad ? -kAdsPitchClampRad : p;
}
// How long the release takes to unwind p0 (radians): kAdsUnwindSec, longer for a large turn so the swing stays under kAdsUnwindMaxRate, at most
// kAdsUnwindMaxSec.
inline double adsUnwindDuration(double p0) noexcept {
    const double d = std::fabs(p0 == p0 ? p0 : 0.0) / kAdsUnwindMaxRate;
    return d < kAdsUnwindSec ? kAdsUnwindSec : d > kAdsUnwindMaxSec ? kAdsUnwindMaxSec : d;
}
// The release's target: p0 at the release, falling linearly to 0 over durSec (default kAdsUnwindSec).
inline double adsUnwindTarget(double p0, double elapsedSec, double durSec = kAdsUnwindSec) noexcept {
    double u = (elapsedSec > 0.0 ? elapsedSec : 0.0) / (durSec > 1e-9 ? durSec : kAdsUnwindSec);
    u = u > 1.0 ? 1.0 : u;
    return p0 * (1.0 - u);
}
// The guard: true once |residual| (deg) has stayed at or above the floor and kAdsResRatio x the head's own turn since the base (deg), grown by
// kAdsResGrowDeg from where the run started, for kAdsResGrowMs, never falling 1 deg below its peak (a fall restarts the run).
struct AdsResidualGrowth {
    bool growing = false;
    uint64_t startMs = 0;
    double startAbs = 0.0, peak = 0.0;
    bool step(uint64_t ms, double absResDeg, double absHeadDeg) noexcept {
        const bool over = absResDeg == absResDeg && absResDeg >= kAdsResFloorDeg && absResDeg >= kAdsResRatio * absHeadDeg;
        if (!over) {
            growing = false;
            return false;
        }
        if (!growing || absResDeg < peak - 1.0) {
            growing = true;
            startMs = ms;
            startAbs = peak = absResDeg;
            return false;
        }
        if (absResDeg > peak) peak = absResDeg;
        return ms >= startMs + kAdsResGrowMs && absResDeg - startAbs >= kAdsResGrowDeg;
    }
    uint64_t runMs(uint64_t ms) const noexcept { return growing && ms > startMs ? ms - startMs : 0; }
};
// The residual's reading per axis: about 0 the view follows the head; about -2 that axis's sign is inverted (the aim turned against the head and the
// cancel doubled it); about -1 the view does not take the aim there (the cancel alone turned it).
inline double adsResidualRatio(double residual, double head) noexcept { return std::fabs(head) > 1e-9 ? residual / head : 0.0; }

// ---- F69i: ADS centres the sight on the press ------------------------------------------------------------------------------------------------
// F68 (log 152023, dumps 152230 rifle, 152353 pistol): the right eye is ON the sight line (0.3 / 0.0 cm), but the sight sits 18-20 deg off the right eye's lens
// axis (yaw about -9, pitch +16..19), set by the game at the press and constant after it: F68a keeps that offset for the whole press, only the mouse moves it, and
// Sean looks through the sight by turning his eyes up-left. Sean (F69 questions): yes, bring the aim onto the head's forward at the press. On each engage the sight
// forward (the FP root's, P14's) is measured against the HEAD's forward (the mean of the two eyes' forwards: on a canted-display headset each eye's own forward
// is the lens axis, Pimax 10.75 deg off the head's), once, and the difference in yaw and pitch is injected as look input through F68a's own path over
// kAdsCentreSec (longer only for a swing over 20 deg: at most kAdsCentreMaxRate, so the frame the cancel runs behind stays near 1 deg). The map's in-flight
// account and cancel see it like any other ask, so the view stays the headset's, and the carried yaw and pitch include it: the release unwinds it.
// Bounded by the game's cone (the weapon's yaw against the body at most 40 deg; the snap keeps 3 deg off it): a game that drops ADS is not argued with.
constexpr double kAdsCentreSec = 0.2;
constexpr double kAdsCentreMaxRate = 100.0 * kAdsDeg;   // rad/s
constexpr double kAdsConeDeg = 40.0, kAdsConeMarginDeg = 3.0;
constexpr double kAdsCentreMinDeg = 0.5;                // an error under this is already centred: no snap
struct AdsCentre {
    bool active = false;
    double yaw = 0.0, pitch = 0.0;            // the plan, radians (seat convention)
    double dur = kAdsCentreSec, t = 0.0;
    double doneYaw = 0.0, donePitch = 0.0;    // injected so far
    // yaw, pitch (radians): the turn that carries the sight onto the head's forward. The duration is kAdsCentreSec, longer for a large swing.
    void begin(double y, double p) noexcept {
        active = std::isfinite(y) && std::isfinite(p) && (std::fabs(y) > 0.0 || std::fabs(p) > 0.0);
        yaw = active ? y : 0.0;
        pitch = active ? p : 0.0;
        const double mag = std::sqrt(yaw * yaw + pitch * pitch), need = mag / kAdsCentreMaxRate;
        dur = need > kAdsCentreSec ? need : kAdsCentreSec;
        t = 0.0;
        doneYaw = donePitch = 0.0;
    }
    void cancel() noexcept { active = false; }
    // The share to inject this tick (dt seconds): linear in time; the last tick takes the remainder exactly.
    void step(double dt, double* y, double* p) noexcept {
        *y = *p = 0.0;
        if (!active) return;
        t += dt > 0.0 ? dt : 0.0;
        const double u = t >= dur ? 1.0 : t / dur;
        *y = yaw * u - doneYaw;
        *p = pitch * u - donePitch;
        doneYaw += *y;
        donePitch += *p;
        if (u >= 1.0) active = false;
    }
};
// The snap's yaw (degrees) bounded by the cone: the weapon's yaw offset from the body (offYawDeg, + left, as adsAimStep measures it) after the snap stays within
// kAdsConeDeg - kAdsConeMarginDeg. *bounded says the cone cut it.
inline double adsCentreYawBounded(double offYawDeg, double wantDeg, bool* bounded) noexcept {
    const double lim = kAdsConeDeg - kAdsConeMarginDeg;
    double after = offYawDeg + wantDeg;
    if (bounded) *bounded = false;
    if (after > lim) after = lim;
    else if (after < -lim) after = -lim;
    else return wantDeg;
    // Already outside the allowed band and asked to go further out: no snap in that direction.
    double r = after - offYawDeg;
    if ((wantDeg > 0.0 && r < 0.0) || (wantDeg < 0.0 && r > 0.0)) r = 0.0;
    if (bounded) *bounded = true;
    return r;
}
// The turn (degrees; yaw + left, pitch + up, in the sight's own axes) that carries the sight forward onto the target forward; the screen axes (right, up) of the
// view the angles are read in (the cyclopean view's). Same convention as the guard's (rotFrameDelta), so the injection and the measurement agree.
inline bool adsCentreError(const double sightFwd[3], const double viewRight[3], const double viewUp[3], const double targetFwd[3], double* yawDeg, double* pitchDeg) noexcept {
    RotFrameSum b, a;
    b.add(viewRight, viewUp, sightFwd);
    a.add(viewRight, viewUp, targetFwd);
    return rotFrameDelta(b, a, yawDeg, pitchDeg);
}

// The asks the game has not answered yet, per axis: one slot per tick for the last kAdsFlightFrames ticks. The turn the game applies is taken off the
// oldest asks first (the game answers in order), so what is in flight stays exact while the answers come within the window; an ask the game never
// answers (a cone, a refusal, a gain error) is forgotten when its slot is that old.
struct AdsInFlight {
    double amt[kAdsFlightFrames] = {};
    int pos = 0;
    void clear() noexcept {
        for (double& a : amt) a = 0.0;
    }
    void tick() noexcept {   // a new tick: the oldest slot leaves the account and becomes this tick's
        pos = (pos + 1) % kAdsFlightFrames;
        amt[pos] = 0.0;
    }
    void apply(double applied) noexcept {   // the game applied this much (same sign as an ask it answers)
        double a = applied;
        for (int i = 1; i <= kAdsFlightFrames && a != 0.0; ++i) {
            double& e = amt[(pos + i) % kAdsFlightFrames];
            if (e == 0.0 || (e > 0.0) != (a > 0.0)) continue;
            const double take = std::fabs(a) < std::fabs(e) ? a : e;
            e -= take;
            a -= take;
        }
    }
    void add(double ask) noexcept { amt[pos] += ask; }
    double sum() const noexcept {
        double s = 0.0;
        for (double a : amt) s += a;
        return s;
    }
};
// The map's state (radians, seat convention). step() is one frame-thread tick: the cancel first (what the game applied since the last tick, exactly), then
// what to ask the game for next. The caller queues the injection as look input through the measured gain and the sign keys, publishes yawPublished() through
// the look's total and pitchPublished() through the ADS pitch overlay, and applies the cancel by that publication.
struct AdsAimMap {
    double yawSeat = 0.0;      // the cancel of what the game applied, cumulative (never reset: the runtime re-bases at a reset serial)
    double yawCarried = 0.0;   // its part since the last reset serial
    double pitchSeat = 0.0;    // the pitch cancel of what the game applied, exact (F109c: only its publication is held to +-60 deg)
    AdsInFlight flightYaw, flightPitch;   // asked and not yet seen applied
    double yawInFlight() const noexcept { return flightYaw.sum(); }
    double pitchInFlight() const noexcept { return flightPitch.sum(); }
    // F92b: the cancel published ahead, this tick's ask. The game applies an ask queued at tick N in its next look update and renders that turn in frame N + 1, and the
    // runtime applies a value published at tick N to the pose of frame N + 1; publishing the cancel only once the applied turn was seen (tick N + 1) put it in frame
    // N + 2, a frame after the game's turn (the F91 flight: the view less the head equal to the game's last applied step to 0.0005 deg rms in every still window). So the
    // published seated rotation is what the game applied plus this tick's ask; next tick the ask's applied turn replaces it (a refused ask shows for one frame).
    double aheadYaw = 0.0, aheadPitch = 0.0;
    double yawPublished() const noexcept { return yawSeat + aheadYaw; }
    double pitchPublished() const noexcept { return adsClampPitch(pitchSeat + aheadPitch); }
    // A remainder the runtime holds in its look total since the last reset serial (the carried yaw, and the ask published ahead): a stop or a release's end bumps the
    // serial for it.
    bool yawToReset() const noexcept { return std::fabs(yawCarried) > 1e-9 || std::fabs(aheadYaw) > 1e-9; }
    double relT = 0.0, relY0 = 0.0, relP0 = 0.0, relYDur = kAdsUnwindSec, relPDur = kAdsUnwindSec, relDur = kAdsUnwindSec;
    uint32_t clampHits = 0;
    AdsCentre centre;   // F69i: the press's snap, asked through the same path as the head's step
    void beginCentre(double yawRad, double pitchRad) noexcept { centre.begin(yawRad, pitchRad); }
    void cancelCentre() noexcept { centre.cancel(); }
    struct Out {
        double injYaw = 0.0, injPitch = 0.0;         // asked of the game this tick (seat radians; the caller converts to input)
        double cancelYaw = 0.0, cancelPitch = 0.0;   // cancelled in the seated space this tick (the account's; the publication's clamp can cut the pitch)
    };
    // A new press starts clean: nothing in flight, the release clock off. The seated yaw and pitch are the cancel of what the game really applied and
    // stay (a press that begins in the middle of a release continues from them).
    void clearInFlight() noexcept {
        flightYaw.clear();
        flightPitch.clear();
    }
    void beginPress() noexcept { clearInFlight(); }
    void beginRelease() noexcept {
        relT = 0.0;
        relY0 = yawCarried + yawInFlight();
        relP0 = pitchSeat + pitchInFlight();
        relYDur = adsUnwindDuration(relY0);
        relPDur = adsUnwindDuration(relP0);
        relDur = relYDur > relPDur ? relYDur : relPDur;
    }
    // The feature stops (a trip, a switch-off): the seated rotation returns to zero (the caller bumps the reset serial for the yaw and publishes the pitch).
    void dropSeated() noexcept {
        yawCarried = pitchSeat = 0.0;
        aheadYaw = aheadPitch = 0.0;
        clearInFlight();
        centre.cancel();
    }
    Out step(bool engaged, bool releasing, double dt, double w, bool headStep, double dYaw, double dPitch, double appliedYaw, double appliedPitch,
             bool yawOn, bool pitchOn) noexcept {
        Out o;
        const double t = dt > 0.0 ? dt : 0.0;
        // The cancel: exactly the turn the game applied for our input.
        const double aY = appliedYaw == appliedYaw ? appliedYaw : 0.0, aP = appliedPitch == appliedPitch ? appliedPitch : 0.0;
        yawSeat += aY;
        yawCarried += aY;
        o.cancelYaw = aY;
        // F109c: the account is exact; the publication (pitchPublished) holds it to +-60 deg. F68a clamped the account itself, so a turn the game applied past 60
        // deg was dropped from it and the account parted from the aim for good: the F109b flight (edvr_gfx_20261025_085120, 08:57:11) counted +39.69 deg of
        // pitch while the account moved -20.49 (60.18 deg dropped at +60), and look to aim's follower, which keeps its own unclamped sum, then asked down
        // into the clamp for 20 s. The asks are still held inside +-60 (adsPitchLimit), so only a turn the game makes of its own goes past it.
        const double want = pitchSeat + aP;
        if (std::fabs(want) > kAdsPitchClampRad) ++clampHits;
        const double before = pitchSeat;
        pitchSeat = want;
        o.cancelPitch = pitchSeat - before;
        flightYaw.tick();   // the oldest slot leaves the account; this tick's applied turn is taken off the oldest asks
        flightPitch.tick();
        flightYaw.apply(aY);
        flightPitch.apply(aP);
        const double yOut = yawInFlight(), pOut = pitchInFlight();
        // The ask: engaged, the head's step times the weight (the pitch held inside +-60 deg with what is in flight); releasing, the unwind's schedule,
        // then the remainder chased until the caller drops it.
        double inY = 0.0, inP = 0.0;
        if (engaged && headStep) {
            const double wt = unit01(w);
            inY = wt * dYaw;
            inP = adsPitchLimit(pitchSeat + pOut, wt * dPitch);
        } else if (releasing) {
            centre.cancel();   // F69i: a snap in progress stops; the release unwinds what was injected
            const double tPrev = relT;
            relT += t;
            if (tPrev < relDur) {
                inY = adsUnwindTarget(relY0, relT, relYDur) - adsUnwindTarget(relY0, tPrev, relYDur);
                inP = adsUnwindTarget(relP0, relT, relPDur) - adsUnwindTarget(relP0, tPrev, relPDur);
            } else {
                if (std::fabs(yawCarried + yOut) > kAdsUnwindDoneRad) inY = -(yawCarried + yOut);
                if (std::fabs(pitchSeat + pOut) > kAdsUnwindDoneRad) inP = -(pitchSeat + pOut);
            }
        }
        if (engaged) {   // F69i: the press's snap rides the same ask (its pitch inside the same clamp)
            double cy = 0.0, cp = 0.0;
            centre.step(t, &cy, &cp);
            inY += cy;
            if (cp != 0.0) inP += adsPitchLimit(pitchSeat + pOut + inP, cp);
        }
        if (!yawOn || !(inY == inY)) inY = 0.0;
        if (!pitchOn || !(inP == inP)) inP = 0.0;
        flightYaw.add(inY);
        flightPitch.add(inP);
        aheadYaw = inY;   // F92b: published with what the game applied, for the frame the game renders this ask in
        aheadPitch = inP;
        o.injYaw = inY;
        o.injPitch = inP;
        return o;
    }
    // The unwind is done: its schedule has run and either nothing is left (the seated rotation is back to zero and the last asks have landed) or the
    // chase has had kAdsCatchUpSec.
    bool releaseDone() const noexcept {
        return relT >= relDur &&
               ((std::fabs(yawCarried) <= kAdsUnwindDoneRad && std::fabs(pitchSeat) <= kAdsUnwindDoneRad &&
                 std::fabs(yawInFlight()) <= kAdsUnwindDoneRad && std::fabs(pitchInFlight()) <= kAdsUnwindDoneRad) ||
                relT >= relDur + kAdsCatchUpSec);
    }
};
// The guard's measurement: the rendered view's turn since the base (the right eye's rows: three axes, right / up / forward, as RotFrameSum reads them)
// less the head's turn since the base and less the player's own aim turn. About 0: the view follows the head. A frame without fresh rows or a head sample
// changes nothing (a stale source never trips it). Camera C is not a source: it is the left eye.
// F109b (the F109 flight, edvr_gfx_20261025_071049, two trips while look to aim ran the guard all the time; reviews in the arc doc, 2026-10-25, F109b), three gates
// on what the guard may judge, so it holds rather than trips where its measurement does not hold:
//  - the yaw and pitch since the base are read in the base's own axes, which is exact only near the base: a turn of the player's own about the world's up (a
//    smooth stick turn) reads in a base pitched p (down negative) as a pitch of about asin(sin(2p)/2 (1 - cos(turn))): 191 deg of turn from a view 6-8 deg down
//    at the base is -12 to -15 deg of pitch, and the first trip read -12.2. A player's own turn past kAdsUserRebaseDeg since the base takes a new base;
//  - the growth run restarts after kAdsResGapMs without a fresh measurement (the run's 0.5 s must be observed);
//  - causal (the caller sets it): the head aim can only part the view from the head in proportion to what it asked (a sign inverted: about 2x the net ask on that
//    axis; a camera that does not take the aim: about 1x). A residual past kAdsCausalK x |the net ask on its axis| + kAdsCausalSlackDeg is the game moving the view
//    on its own (the second trip: -17.9 deg of pitch 0.6 s after a base, the head's pitch -0.1, the asks the head's): a new base, counted, not a trip.
constexpr double kAdsUserRebaseDeg = 15.0;
constexpr uint64_t kAdsResGapMs = 200;
constexpr double kAdsCausalK = 2.5, kAdsCausalSlackDeg = 4.0;
struct AdsResidual {
    bool haveBase = false;
    RotFrameSum viewBase, headBase, viewLast;
    double userYaw = 0.0, userPitch = 0.0;                         // the player's own aim turn since the base (seat radians)
    double askYaw = 0.0, askPitch = 0.0;                           // F109b: the head aim's net asks since the base (seat radians)
    AdsResidualGrowth growth;
    double res[2] = {}, head[2] = {}, view[2] = {};                // degrees since the base: the residual, the head's turn, the view's turn
    bool have = false;
    double maxAbs = 0.0;                                           // the largest |residual| since the last clearWindow()
    uint32_t rebases = 0;
    bool causal = false;                                           // F109b: a trip needs the head aim's asks to explain the residual (kept across reset())
    uint64_t lastMs = 0;                                           // F109b: the last fresh measurement
    uint32_t userRebases = 0, gapRestarts = 0, holds = 0;          // F109b: the gates' counts (cumulative)
    double heldRes[2] = {}, heldAsk[2] = {};                       // F109b: the last hold's residual and net asks (degrees)
    enum Result { kNone = 0, kBase = 1, kMeasured = 2, kRebased = 3, kTrip = 4, kHeld = 5 };
    void reset() noexcept {
        haveBase = false;
        viewBase = viewLast = headBase = RotFrameSum{};
        userYaw = userPitch = 0.0;
        askYaw = askPitch = 0.0;
        growth = AdsResidualGrowth{};
        res[0] = res[1] = head[0] = head[1] = view[0] = view[1] = 0.0;
        have = false;
        lastMs = 0;
    }
    void clearWindow() noexcept { maxAbs = 0.0; }
    void addUser(double yaw, double pitch) noexcept {
        userYaw += yaw == yaw ? yaw : 0.0;
        userPitch += pitch == pitch ? pitch : 0.0;
    }
    void addAsk(double yaw, double pitch) noexcept {
        askYaw += yaw == yaw ? yaw : 0.0;
        askPitch += pitch == pitch ? pitch : 0.0;
    }
    void rebaseAt(const RotFrameSum& cn, const RotFrameSum& hn) noexcept {
        haveBase = true;
        viewBase = viewLast = cn;
        headBase = hn;
        userYaw = userPitch = 0.0;
        askYaw = askPitch = 0.0;
        growth = AdsResidualGrowth{};
        have = false;
    }
    Result step(uint64_t ms, bool viewOk, const double vr[3], const double vu[3], const double vf[3], bool headOk, const double hr[3], const double hu[3],
                const double hf[3], bool yawOn, bool pitchOn) noexcept {
        if (!viewOk || !headOk) return kNone;
        RotFrameSum cn, hn;
        cn.add(vr, vu, vf);
        hn.add(hr, hu, hf);
        if (!haveBase) {
            haveBase = true;
            viewBase = viewLast = cn;
            headBase = hn;
            userYaw = userPitch = 0.0;
            askYaw = askPitch = 0.0;
            lastMs = ms;
            return kBase;
        }
        if (lastMs != 0 && ms > lastMs + kAdsResGapMs && growth.growing) {   // F109b: the run's time was not all observed
            growth = AdsResidualGrowth{};
            ++gapRestarts;
        }
        lastMs = ms;
        if (std::fabs(userYaw) > kAdsUserRebaseDeg * kAdsDeg || std::fabs(userPitch) > kAdsUserRebaseDeg * kAdsDeg) {   // F109b: near the base only
            ++userRebases;
            rebaseAt(cn, hn);
            return kRebased;
        }
        double sy = 0.0, sp = 0.0;
        if (rotFrameDelta(viewLast, cn, &sy, &sp) && std::sqrt(sy * sy + sp * sp) > kAdsViewJumpDeg) {
            ++rebases;
            rebaseAt(cn, hn);
            return kRebased;
        }
        double cy = 0.0, cp = 0.0, hy = 0.0, hp = 0.0;
        if (!rotFrameDelta(viewBase, cn, &cy, &cp) || !rotFrameDelta(headBase, hn, &hy, &hp)) return kNone;
        viewLast = cn;
        double ry = cy - hy - userYaw / kAdsDeg, rp = cp - hp - userPitch / kAdsDeg;
        while (ry > 180.0) ry -= 360.0;
        while (ry < -180.0) ry += 360.0;
        if (!yawOn) ry = 0.0;
        if (!pitchOn) rp = 0.0;
        res[0] = ry;
        res[1] = rp;
        head[0] = hy;
        head[1] = hp;
        view[0] = cy;
        view[1] = cp;
        have = true;
        const double absRes = std::sqrt(ry * ry + rp * rp), absHead = std::sqrt(hy * hy + hp * hp);
        if (absRes > maxAbs) maxAbs = absRes;
        if (!growth.step(ms, absRes, absHead)) return kMeasured;
        if (causal) {   // F109b: a residual the head aim's asks cannot explain is the game moving the view: a new base, not a trip
            const double ay = std::fabs(askYaw) / kAdsDeg, ap = std::fabs(askPitch) / kAdsDeg;
            const bool explained = std::fabs(ry) <= kAdsCausalK * ay + kAdsCausalSlackDeg && std::fabs(rp) <= kAdsCausalK * ap + kAdsCausalSlackDeg;
            if (!explained) {
                ++holds;
                heldRes[0] = ry;
                heldRes[1] = rp;
                heldAsk[0] = askYaw / kAdsDeg;
                heldAsk[1] = askPitch / kAdsDeg;
                rebaseAt(cn, hn);
                return kHeld;
            }
        }
        return kTrip;
    }
};
// A guard trip or a failed restore holds the feature off for the rest of that press only: the flag falling clears it, so every press re-arms.
struct AdsPressGate {
    bool heldOff = false;
    void holdOff() noexcept { heldOff = true; }
    // The game's ADS flag this tick; false while this press is held off.
    bool allow(bool adsFlag) noexcept {
        if (!adsFlag) heldOff = false;
        return !heldOff;
    }
};

// ---- F67b: the ADS instruments ------------------------------------------------------------------------------------------------------------------
// The angle (degrees, 0 to 180) between two directions; -1 when either is degenerate.
inline double dirAngleDeg(const double a[3], const double b[3]) noexcept {
    double ua[3], ub[3];
    if (!ikUnit(a, ua) || !ikUnit(b, ub)) return -1.0;
    double c = ikDot(ua, ub);
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    return std::acos(c) * 180.0 / kIkPi;
}
// Transport joint 14's complete rigid basis through F_root. Rows are directions in row-vector convention;
// translation is intentionally excluded. A malformed local or root basis is refused instead of treated as an optic axis.
struct AdsOpticBasis {
    bool valid = false;
    double det = 0.0, maxRowError = 0.0, maxDotError = 0.0;
    double right[3] = {}, up[3] = {}, forward[3] = {};
    double yawDeg = 0.0, pitchDeg = 0.0;
};
inline bool adsOpticBasis(const float p14[16], const float froot[16], AdsOpticBasis& out) noexcept {
    out = AdsOpticBasis{};
    double root[3][3], local[3][3], world[3][3];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
        root[r][c] = froot[r * 4 + c];
        local[r][c] = p14[r * 4 + c];
        if (!std::isfinite(root[r][c]) || !std::isfinite(local[r][c])) return false;
    }
    const auto determinant = [](const double m[3][3]) noexcept {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    const auto rigid = [](const double m[3][3], double& rowErr, double& dotErr) noexcept {
        rowErr = dotErr = 0.0;
        for (int r = 0; r < 3; ++r) {
            const double d = m[r][0] * m[r][0] + m[r][1] * m[r][1] + m[r][2] * m[r][2];
            rowErr = std::max(rowErr, std::fabs(std::sqrt(d) - 1.0));
            for (int q = 0; q < r; ++q) {
                const double v = m[r][0] * m[q][0] + m[r][1] * m[q][1] + m[r][2] * m[q][2];
                dotErr = std::max(dotErr, std::fabs(v));
            }
        }
        return rowErr <= 0.02 && dotErr <= 0.02;
    };
    double rootRow = 0.0, rootDot = 0.0, localRow = 0.0, localDot = 0.0;
    const double rootDet = determinant(root), localDet = determinant(local);
    if (!rigid(root, rootRow, rootDot) || !rigid(local, localRow, localDot) || !(rootDet > 0.95 && rootDet < 1.05) || !(localDet > 0.95 && localDet < 1.05)) return false;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
        world[r][c] = 0.0;
        for (int k = 0; k < 3; ++k) world[r][c] += local[r][k] * root[k][c];
    }
    double worldRow = 0.0, worldDot = 0.0;
    out.det = determinant(world);
    if (!rigid(world, worldRow, worldDot) || !(out.det > 0.95 && out.det < 1.05)) return false;
    out.maxRowError = std::max(std::max(rootRow, localRow), worldRow);
    out.maxDotError = std::max(std::max(rootDot, localDot), worldDot);
    for (int c = 0; c < 3; ++c) { out.right[c] = world[0][c]; out.up[c] = world[1][c]; out.forward[c] = world[2][c]; }
    const double rootRight[3] = {root[0][0], root[0][1], root[0][2]}, rootForward[3] = {root[2][0], root[2][1], root[2][2]};
    out.yawDeg = std::atan2(ikDot(out.forward, rootRight), ikDot(out.forward, rootForward)) * 180.0 / kIkPi;
    const double upDot = std::max(-1.0, std::min(1.0, ikDot(out.forward, root[1])));
    out.pitchDeg = std::asin(upDot) * 180.0 / kIkPi;
    out.valid = std::isfinite(out.yawDeg) && std::isfinite(out.pitchDeg);
    return out.valid;
}
// F79: measured rendered-optic anchor for the laser assault rifle only. It is the mean of 43 fresh joint-51 samples;
// the transverse-shift plausibility envelope is half the measured optic quad width and rejects all captured pistol rows.
// This is not proof of the clear-glass aperture or every unseen weapon transition.
constexpr double kAdsRifleAnchor51[3] = {0.00361396145, 0.13115042061, 0.07047338225};
constexpr double kAdsRifleAnchorQuadHalfWidthM = 0.02217773348093033;
constexpr char kAdsRifleOpticItemName[] = "$wpn_m_assaultrifle_laser_fauto_name;";
inline bool adsRifleOpticItemMatches(const char* name) noexcept { return name && std::strcmp(name, kAdsRifleOpticItemName) == 0; }
inline bool adsRifleOpticItemSnapshotMatches(bool readOk, uint32_t snapshotSerial, uint32_t currentSerial, const char* name) noexcept {
    return readOk && snapshotSerial == currentSerial && adsRifleOpticItemMatches(name);
}
// The joint snapshot and the root are both render-frame stamps, and both reads are already inside the 100 ms freshness window.
// The 18:34 laser ADS flight (edvr_gfx_20261026_182525) refused every sample because the two stamps were not equal, so the lock used the
// rifle root. The snap then left the sight 2 deg to the left of the head. A one-frame gap is the same pose.
inline bool adsRifleOpticFramesClose(uint32_t jointFrame, uint32_t rootFrame) noexcept {
    if (!jointFrame || jointFrame == ~uint32_t(0) || rootFrame == ~uint32_t(0)) return false;
    const uint32_t gap = jointFrame > rootFrame ? jointFrame - rootFrame : rootFrame - jointFrame;
    return gap <= 1u;
}
enum AdsRifleAnchorRefusal : uint8_t { kAdsRifleAnchorNone = 0, kAdsRifleAnchorItem, kAdsRifleAnchorFrame, kAdsRifleAnchorMatrix, kAdsRifleAnchorGeometry };
struct AdsRifleAnchorResult {
    bool applied = false;
    AdsRifleAnchorRefusal refusal = kAdsRifleAnchorItem;
    double rawOrigin[3] = {}, origin[3] = {}, anchorWorld[3] = {}, shift[3] = {}, spanM = 0.0, transverseResidualM = 0.0;
};
inline AdsRifleAnchorResult adsRifleOpticOrigin(bool coherentRifleItem, bool sameRenderFrame, const float p14[16], const float joint51[16],
                                                const float joint52[16], const float froot[16]) noexcept {
    AdsRifleAnchorResult out;
    if (!coherentRifleItem) { out.refusal = kAdsRifleAnchorItem; return out; }
    if (!sameRenderFrame) { out.refusal = kAdsRifleAnchorFrame; return out; }
    AdsOpticBasis basis;
    if (!adsOpticBasis(p14, froot, basis) || !adsOpticBasis(joint51, froot, basis) || !adsOpticBasis(joint52, froot, basis)) {
        out.refusal = kAdsRifleAnchorMatrix;
        return out;
    }
    const double p14Local[3] = {p14[12], p14[13], p14[14]};
    const double joint51Local[3] = {joint51[12], joint51[13], joint51[14]};
    double p14World[3], joint51World[3], f[3] = {froot[8], froot[9], froot[10]};
    ikXform(p14Local, froot, p14World);
    ikXform(joint51Local, froot, joint51World);
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(p14World[i]) || !std::isfinite(joint51World[i]) || !std::isfinite(joint52[12 + i])) {
            out.refusal = kAdsRifleAnchorMatrix;
            return out;
        }
        out.rawOrigin[i] = p14World[i];
        out.origin[i] = p14World[i];
    }
    const double attachDelta[3] = {static_cast<double>(joint51[12]) - joint52[12], static_cast<double>(joint51[13]) - joint52[13],
                                   static_cast<double>(joint51[14]) - joint52[14]};
    out.spanM = ikLen(attachDelta);
    if (!std::isfinite(out.spanM)) { out.refusal = kAdsRifleAnchorMatrix; return out; }
    if (!ikUnit(f, f)) { out.refusal = kAdsRifleAnchorMatrix; return out; }
    double anchorModel[3];
    ikXform(kAdsRifleAnchor51, joint51, anchorModel);
    ikXform(anchorModel, froot, out.anchorWorld);
    if (!std::isfinite(p14World[0]) || !std::isfinite(p14World[1]) || !std::isfinite(p14World[2]) || !std::isfinite(out.anchorWorld[0]) ||
        !std::isfinite(out.anchorWorld[1]) || !std::isfinite(out.anchorWorld[2])) {
        out.refusal = kAdsRifleAnchorMatrix;
        return out;
    }
    const double delta[3] = {out.anchorWorld[0] - p14World[0], out.anchorWorld[1] - p14World[1], out.anchorWorld[2] - p14World[2]};
    const double along = ikDot(delta, f);
    for (int i = 0; i < 3; ++i) {
        out.shift[i] = delta[i] - f[i] * along;
        out.origin[i] = p14World[i] + out.shift[i];
    }
    out.transverseResidualM = ikLen(out.shift);
    if (!std::isfinite(out.transverseResidualM) || out.transverseResidualM > kAdsRifleAnchorQuadHalfWidthM) {
        out.origin[0] = p14World[0]; out.origin[1] = p14World[1]; out.origin[2] = p14World[2];
        out.refusal = kAdsRifleAnchorGeometry;
        return out;
    }
    out.applied = true;
    out.refusal = kAdsRifleAnchorNone;
    return out;
}
// The NDC of the point p + dist * (the unit of f) in an eye's scene rows (row-vector form, clip = (p - o) . rows[0..11] + rows[12..15]). False when
// the point is behind the eye or f is degenerate. P14 itself sits behind the right eye (F66: w < 0), so the sight is sampled in front of it.
inline bool sightPointNdc(const float rows[16], const double o[3], const double p[3], const double f[3], double dist, double ndc[2]) noexcept {
    double fu[3];
    ndc[0] = ndc[1] = 0.0;
    if (!ikUnit(f, fu)) return false;
    const double v[3] = {p[0] + dist * fu[0] - o[0], p[1] + dist * fu[1] - o[1], p[2] + dist * fu[2] - o[2]};
    double c[4];
    for (int j = 0; j < 4; ++j) c[j] = v[0] * rows[j] + v[1] * rows[4 + j] + v[2] * rows[8 + j] + rows[12 + j];
    if (!(c[3] > 1e-9) || !std::isfinite(c[3])) return false;
    ndc[0] = c[0] / c[3];
    ndc[1] = c[1] / c[3];
    return std::isfinite(ndc[0]) && std::isfinite(ndc[1]);
}
// ---- F68b: the sight against the fire line ----------------------------------------------------------------------------------------------------
// The unit direction (world) of the ray through the NDC point (nx, ny) of an eye's scene rows (row-vector form, clip_j = p . column_j + rows[12 + j]; the
// eye at the origin of p): the clip x and y planes through the point cut along (c_x - nx c_w) x (c_y - ny c_w), signed to look forward. False when degenerate.
inline bool ndcRay(const float rows[16], const double ndc[2], double out[3]) noexcept {
    const double cx[3] = {rows[0], rows[4], rows[8]}, cy[3] = {rows[1], rows[5], rows[9]}, cw[3] = {rows[3], rows[7], rows[11]};
    const double a[3] = {cx[0] - ndc[0] * cw[0], cx[1] - ndc[0] * cw[1], cx[2] - ndc[0] * cw[2]};
    const double b[3] = {cy[0] - ndc[1] * cw[0], cy[1] - ndc[1] * cw[1], cy[2] - ndc[1] * cw[2]};
    double v[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    if (v[0] * cw[0] + v[1] * cw[1] + v[2] * cw[2] < 0.0)
        for (double& c : v) c = -c;
    return ikUnit(v, out);
}
// The angle from the ray through `from` to the ray through `to`, in the eye's own axes (right, up, forward from rotAxesFromClip): the horizontal and
// vertical plane angles (degrees; + = to is to the right of / above from) and the total angle between the rays. False when the rows or a ray degenerate.
inline bool ndcAngles(const float rows[16], const double from[2], const double to[2], double* dRightDeg, double* dUpDeg, double* totalDeg) noexcept {
    double r[3], u[3], f[3], a[3], b[3];
    *dRightDeg = *dUpDeg = *totalDeg = 0.0;
    if (!rotAxesFromClip(rows, r, u, f) || !ndcRay(rows, from, a) || !ndcRay(rows, to, b)) return false;
    const double kDeg = 180.0 / 3.14159265358979323846;
    const double ah = std::atan2(ikDot(a, r), ikDot(a, f)), av = std::atan2(ikDot(a, u), ikDot(a, f));
    const double bh = std::atan2(ikDot(b, r), ikDot(b, f)), bv = std::atan2(ikDot(b, u), ikDot(b, f));
    *dRightDeg = (bh - ah) * kDeg;
    *dUpDeg = (bv - av) * kDeg;
    double c = ikDot(a, b);
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    *totalDeg = std::acos(c) * kDeg;
    return true;
}
// ---- F67c: the body pinned to the camera (option B of the F66 review: the view never moves; the body follows it in 3D) ---------------------------
// Replaces F65b's stand height. P1 publishes the job camera and the AMC matrix every frame; the Animate post-hook (stage 0) takes the cyclopean eye
// (the job camera, the LEFT eye, moved half the IPD toward the right eye by the two eyes' solved scene rows) into the 3P model space (= body axes:
// right, up, forward), puts the NECK target below and behind it (0.155 m down, 0.09 m back along the eye's heading, the rest offset of the F66
// flight: the eye sat (0, 0.155, 0.09) from the neck standing), and moves the animated pose so the neck 34 lands on the target: the horizontal part
// (right, forward) is a translation of EVERY joint, the vertical part (up, signed) a translation of the upper body with each leg solved from the moved
// hip to its foot. The foot keeps its animated height; when the leg cannot reach, the foot is first RE-PLANTED (moved horizontally toward under the
// hip along hip -> foot until the leg reaches with the knee slightly bent), and lifted only when even straight below the hip is out of reach (a jump);
// when the hip is lowered past the knee's fold limit the foot is re-planted outward. Nothing captured and nothing read from a previous frame.
// The rest offset follows the eye's HEADING (yaw) only: a head turned 90 deg would otherwise leave the 9 cm "back" on the wrong side.
constexpr double kPinHorizCapM = 1.5, kPinVertCapM = 0.8;   // the largest pin move (a camera farther off the animated neck is clamped; the line counts it)
constexpr double kPinRestUpM = 0.155, kPinRestBackM = 0.09;   // = -kNeckRestOffsetFull3P[1], [2]
constexpr double kLegKneeMinDeg = 30.0;                       // the knee folds no further than this interior angle
// The eye's horizontal heading as a unit vector in model axes (x right, z forward); (0, 0, 1) when the eye looks within ~20 deg of vertical.
inline void pinHeading(const double fwd[3], double out[3]) noexcept {
    const double h = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    const double n = std::sqrt(h * h + fwd[1] * fwd[1]);
    if (!(n > 1e-9) || !(h / n > 0.35)) {
        out[0] = 0.0;
        out[1] = 0.0;
        out[2] = 1.0;
        return;
    }
    out[0] = fwd[0] / h;
    out[1] = 0.0;
    out[2] = fwd[2] / h;
}
// The neck target (model axes): the eye 0.155 m down and 0.09 m back along its heading.
inline void pinNeckTarget(const double eye[3], const double fwd[3], double out[3]) noexcept {
    double h[3];
    pinHeading(fwd, h);
    out[0] = eye[0] - kPinRestBackM * h[0];
    out[1] = eye[1] - kPinRestUpM;
    out[2] = eye[2] - kPinRestBackM * h[2];
}
struct PinMove {
    double th[3] = {};   // the horizontal translation of the whole pose (model x, 0, z)
    double delta = 0.0;  // the signed vertical move of the upper body
    bool hClamped = false, vClamped = false;
};
// F73 ADS body policy: release the head target toward the same-call authored neck while preserving the independent arm/reach solve.
inline double pinDetachTarget(const double headTarget[3], const double authoredNeck[3], double detachW, double out[3]) noexcept {
    const double w = std::isfinite(detachW) ? (detachW < 0.0 ? 0.0 : detachW > 1.0 ? 1.0 : detachW) : 0.0;
    double d2 = 0.0;
    for (int i = 0; i < 3; ++i) {
        out[i] = headTarget[i] + (authoredNeck[i] - headTarget[i]) * w;
        const double d = out[i] - authoredNeck[i];
        d2 += d * d;
    }
    return std::sqrt(d2);
}
inline double pinBodyBendWeight(double detachW) noexcept {
    const double w = std::isfinite(detachW) ? (detachW < 0.0 ? 0.0 : detachW > 1.0 ? 1.0 : detachW) : 0.0;
    return 1.0 - w;
}
inline void pinBendTargetWithReach(const double headTarget[3], const double reach[3], float out[3]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = static_cast<float>(headTarget[i] + reach[i]);
}
inline PinMove pinMove(const double neck[3], const double target[3]) noexcept {
    PinMove m;
    double dx = target[0] - neck[0], dz = target[2] - neck[2], dy = target[1] - neck[1];
    if (!(dx == dx) || !(dy == dy) || !(dz == dz)) return m;
    const double h = std::sqrt(dx * dx + dz * dz);
    if (h > kPinHorizCapM) {
        dx *= kPinHorizCapM / h;
        dz *= kPinHorizCapM / h;
        m.hClamped = true;
    }
    if (dy > kPinVertCapM || dy < -kPinVertCapM) {
        dy = dy > 0.0 ? kPinVertCapM : -kPinVertCapM;
        m.vClamped = true;
    }
    m.th[0] = dx;
    m.th[2] = dz;
    m.delta = dy;
    return m;
}
// A leg's shortest hip -> foot distance: the knee at kLegKneeMinDeg.
inline double legMinReach(double l1, double l2) noexcept {
    const double a = kLegKneeMinDeg * kIkPi / 180.0;
    const double d2 = l1 * l1 + l2 * l2 - 2.0 * l1 * l2 * std::cos(a);
    return d2 > 0.0 ? std::sqrt(d2) : 0.0;
}
// Where a foot is to stand once the hip is at hip1: the animated foot foot0 (its height kept); out of reach (past kStandReach (l1 + l2)) it is
// re-planted toward under the hip (the same bearing, the horizontal distance shortened until the leg reaches), or lifted when straight below the hip
// is still out of reach; closer than the fold limit it is re-planted outward (fallbackDir, a horizontal unit, when it is under the hip). The limits are
// the knee's, widened to the ANIMATED leg's own distance animDist (hip0 -> foot0): a leg the animation itself holds nearly straight or deeply bent is
// not moved until the pin makes it worse.
struct LegPlant {
    double foot[3] = {};
    double replantM = 0.0;   // horizontal distance moved toward under the hip (+) or outward (-)
    double liftM = 0.0;      // the foot raised by this much (a jump)
    double reachBefore = 0.0, reachAfter = 0.0;   // hip -> foot over (l1 + l2), the animated foot and the planned one
    bool closed = true;      // the planned foot is inside the reach range
};
inline LegPlant legPlant(const double hip1[3], const double foot0[3], double l1, double l2, const double fallbackDir[3], double animDist) noexcept {
    LegPlant o;
    for (int i = 0; i < 3; ++i) o.foot[i] = foot0[i];
    const double L = l1 + l2;
    if (!(L > 1e-6)) return o;
    double rMax = kStandReach * L, rMin = legMinReach(l1, l2);
    if (animDist == animDist && animDist > 0.0) {
        rMax = std::max(rMax, std::min(animDist, 0.9995 * L));
        rMin = std::min(rMin, animDist);
    }
    const double v = hip1[1] - foot0[1];
    double hx = foot0[0] - hip1[0], hz = foot0[2] - hip1[2];
    const double h = std::sqrt(hx * hx + hz * hz);
    const double dist = std::sqrt(h * h + v * v);
    o.reachBefore = dist / L;
    if (dist > rMax) {
        if (std::fabs(v) < rMax) {
            const double h1 = std::sqrt(rMax * rMax - v * v);
            if (h > 1e-9) {
                o.foot[0] = hip1[0] + hx * (h1 / h);
                o.foot[2] = hip1[2] + hz * (h1 / h);
            }
            o.replantM = h - h1;
        } else {
            o.foot[0] = hip1[0];
            o.foot[2] = hip1[2];
            o.replantM = h;
            if (v > 0.0) {
                o.foot[1] = hip1[1] - rMax;   // straight below the hip and still out of reach: the foot is lifted
                o.liftM = o.foot[1] - foot0[1];
            }
        }
    } else if (dist < rMin) {
        const double v2 = v * v;
        const double h1 = rMin * rMin > v2 ? std::sqrt(rMin * rMin - v2) : 0.0;
        double dx = hx, dz = hz;
        if (h > 1e-6) {
            dx /= h;
            dz /= h;
        } else {
            dx = fallbackDir ? fallbackDir[0] : 0.0;
            dz = fallbackDir ? fallbackDir[2] : 1.0;
            const double fl = std::sqrt(dx * dx + dz * dz);
            if (fl > 1e-9) {
                dx /= fl;
                dz /= fl;
            } else {
                dx = 0.0;
                dz = 1.0;
            }
        }
        o.foot[0] = hip1[0] + dx * h1;
        o.foot[2] = hip1[2] + dz * h1;
        o.replantM = h - h1;   // negative: outward
    }
    {
        const double fx = o.foot[0] - hip1[0], fy = hip1[1] - o.foot[1], fz = o.foot[2] - hip1[2];
        o.reachAfter = std::sqrt(fx * fx + fy * fy + fz * fz) / L;
        o.closed = o.reachAfter * L <= rMax + 1e-9 && o.reachAfter * L >= rMin - 1e-9;
    }
    return o;
}
// F82: a laser-rifle reach correction can leave the hands outside the hard elbow range after the current clavicle assist.
// Test a torso yaw about the selected neck target. Rotating the chest subtree about chest 24 and then translating it back to
// the neck target gives the same point transform as this yaw about N. The assist is predicted here and is still
// applied exactly once by the normal arm solve later in the pass.
struct CrouchNeckArm {
    bool active = true;
    double clavicle[3] = {}, shoulder[3] = {}, elbow[3] = {}, hand[3] = {}, target[3] = {}, l1 = 0.0, l2 = 0.0, wSolve = 1.0;
    double fpPole[3] = {};
    bool fpPoleValid = false, stableEligible = false;
    IkPhysicalPole poleSeed{};
};
struct CrouchNeckLeg {
    double hip[3] = {}, knee[3] = {}, foot[3] = {};
};
struct CrouchNeckYaw {
    bool feasible = false, qualityAccepted = false;
    double yawRad = 0.0, maxShortfallM = 0.0, poseCost = 0.0, assistDeg[2] = {};
    double baselineYawRad = 0.0, baselinePoseCost = 0.0;
    double rootDelta[3] = {};
    double clavicle[2][3] = {}, shoulder[2][3] = {};
};
inline double ikUnitDirectionCost(const double before[3], const double after[3]) noexcept {
    double a[3], b[3];
    if (!ikUnit(before, a) || !ikUnit(after, b)) return std::numeric_limits<double>::infinity();
    const double d = std::max(-1.0, std::min(1.0, ikDot(a, b)));
    return 2.0 * (1.0 - d);
}
struct CrouchNeckYawPoseMetrics {
    bool valid = false;
    double cost = std::numeric_limits<double>::infinity(), assistDeg[2] = {};
    double leftElbowYFromShoulderL1 = 0.0, leftElbowInteriorDeg = 0.0;
};
// F84: evaluate rifle support geometry in the same body-down/lateral circle policy used by the runtime.
// The legacy angle is the previous policy's output on this candidate circle; qualityWeight fades the
// replacement in from the legacy pole and detachWeight returns it exactly to the legacy ADS endpoint.
inline bool ikRifleSupportTheta(const IkTwoBone& t, double legacyTheta, double detachWeight, double qualityWeight,
                                double elbowOutM, double trimRad, double* thetaOut, double* radialOut = nullptr) noexcept {
    if (!t.ok || !std::isfinite(legacyTheta) || !std::isfinite(detachWeight) || !std::isfinite(qualityWeight) ||
        !std::isfinite(elbowOutM) || !std::isfinite(trimRad)) return false;
    const double detach = std::max(0.0, std::min(1.0, detachWeight));
    const double quality = std::max(0.0, std::min(1.0, qualityWeight));
    const double amount = quality * (1.0 - detach);
    if (amount <= 0.0) {
        *thetaOut = ikWrapPi(legacyTheta);
        if (radialOut && !ikElbowRadial(t, *thetaOut, radialOut)) return false;
        return true;
    }
    double legacyRadial[3];
    if (!ikElbowRadial(t, legacyTheta, legacyRadial)) return false;
    const double up[3] = {0.0, 1.0, 0.0};
    const IkPistolDownSwivel down = ikPistolDownSwivel(t, legacyRadial, up, elbowOutM, trimRad);
    if (!down.ok) return false;
    const double delta = ikWrapPi(down.theta - legacyTheta);
    const double theta = ikWrapPi(legacyTheta + amount * delta);
    if (!std::isfinite(theta)) return false;
    *thetaOut = theta;
    if (radialOut && !ikElbowRadial(t, theta, radialOut)) return false;
    return true;
}
inline bool crouchNeckYawFeasible(const double neck[3], const double chestPivot[3], const CrouchNeckArm arm[2], const CrouchNeckLeg leg[2], double legVertical,
                                  double yawRad, const double adsPush[3],
                                  CrouchNeckYaw* out) noexcept {
    CrouchNeckYaw result;
    if (!std::isfinite(yawRad)) return false;
    if (!std::isfinite(legVertical)) return false;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(neck[i]) || !std::isfinite(chestPivot[i]) || !std::isfinite(adsPush[i])) return false;
    result.yawRad = yawRad;
    double turnedNeck[3];
    turnAboutVertical(neck, chestPivot, yawRad, turnedNeck);
    for (int i = 0; i < 3; ++i) result.rootDelta[i] = neck[i] - turnedNeck[i];
    result.rootDelta[1] = 0.0;
    bool armChecked = false;
    for (int side = 0; side < 2; ++side) {
        if (!arm[side].active) continue;
        armChecked = true;
        const double w = arm[side].wSolve;
        if (!std::isfinite(w) || w < 0.0 || w > 1.0) return false;
        if (!(std::isfinite(arm[side].l1) && std::isfinite(arm[side].l2) && arm[side].l1 > 1e-4 && arm[side].l2 > 1e-4)) return false;
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(arm[side].clavicle[i]) || !std::isfinite(arm[side].shoulder[i]) || !std::isfinite(arm[side].elbow[i]) || !std::isfinite(arm[side].hand[i]) ||
                !std::isfinite(arm[side].target[i])) return false;
        turnAboutVertical(arm[side].clavicle, neck, yawRad, result.clavicle[side]);
        turnAboutVertical(arm[side].shoulder, neck, yawRad, result.shoulder[side]);
        double hand[3];
        turnAboutVertical(arm[side].hand, neck, yawRad, hand);
        const double target[3] = {hand[0] + w * (arm[side].target[0] - hand[0]), hand[1] + w * (arm[side].target[1] - hand[1]),
                                  hand[2] + w * (arm[side].target[2] - hand[2])};
        const double d[3] = {target[0] - result.shoulder[side][0], target[1] - result.shoulder[side][1], target[2] - result.shoulder[side][2]};
        const double preAssistArm[3] = {hand[0] - result.shoulder[side][0], hand[1] - result.shoulder[side][1], hand[2] - result.shoulder[side][2]};
        const double dist = ikLen(d);
        double excess = 0.0;
        double disp[3] = {};
        const double move = ikAssistReachMove(dist, arm[side].l1, arm[side].l2, &excess);
        for (int i = 0; i < 3; ++i) disp[i] = (dist > 1e-9 ? move * d[i] / dist : 0.0) + adsPush[i];
        for (int i = 0; i < 3; ++i) disp[i] *= w;
        double R[9], angle = 0.0;
        bool capped = false;
        if (ikAssistRot(result.clavicle[side], result.shoulder[side], disp, R, &angle, &capped)) {
            const double cs[3] = {result.shoulder[side][0] - result.clavicle[side][0], result.shoulder[side][1] - result.clavicle[side][1],
                                  result.shoulder[side][2] - result.clavicle[side][2]};
            double turned[3];
            ikRowRotate(cs, R, turned);
            for (int i = 0; i < 3; ++i) result.shoulder[side][i] = result.clavicle[side][i] + turned[i];
            result.assistDeg[side] = angle * 180.0 / kIkPi;
        }
        const double after[3] = {target[0] - result.shoulder[side][0], target[1] - result.shoulder[side][1], target[2] - result.shoulder[side][2]};
        const double afterDist = ikLen(after);
        double dLo = 0.0, dHi = 0.0;
        ikElbowDRange(arm[side].l1, arm[side].l2, kIkElbowMinDeg, kIkElbowMaxDeg, &dLo, &dHi);
        double fLo = 0.0, fHi = 0.0;
        ikElbowDRange(arm[side].l1, arm[side].l2, 0.0, 180.0, &fLo, &fHi);
        const double animDist = ikLen(preAssistArm);
        if (animDist < dLo) dLo -= (1.0 - w) * (dLo - animDist);
        if (animDist > dHi) dHi += (1.0 - w) * (animDist - dHi);
        dLo = std::max(dLo, fLo);
        dHi = std::min(dHi, fHi);
        if (dHi < dLo) dHi = dLo;
        if (!(std::isfinite(afterDist) && dHi >= dLo)) return false;
        const double shortfall = afterDist > dHi ? afterDist - dHi : dLo - afterDist;
        if (shortfall > 1e-6) return false;
        result.maxShortfallM = std::max(result.maxShortfallM, std::max(0.0, shortfall));
    }
    if (!armChecked) return false;
    for (int side = 0; side < 2; ++side) {
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(leg[side].hip[i]) || !std::isfinite(leg[side].knee[i]) || !std::isfinite(leg[side].foot[i])) return false;
        const double hip1[3] = {leg[side].hip[0] + result.rootDelta[0], leg[side].hip[1] + result.rootDelta[1] + legVertical,
                                leg[side].hip[2] + result.rootDelta[2]};
        const double hk[3] = {leg[side].knee[0] - leg[side].hip[0], leg[side].knee[1] - leg[side].hip[1], leg[side].knee[2] - leg[side].hip[2]};
        const double kf[3] = {leg[side].foot[0] - leg[side].knee[0], leg[side].foot[1] - leg[side].knee[1], leg[side].foot[2] - leg[side].knee[2]};
        const double hf[3] = {leg[side].foot[0] - leg[side].hip[0], leg[side].foot[1] - leg[side].hip[1], leg[side].foot[2] - leg[side].hip[2]};
        double fb[3] = {hk[0], 0.0, hk[2]};
        if (!ikUnit(fb, fb)) { fb[0] = 0.0; fb[1] = 0.0; fb[2] = 1.0; }
        double heldFoot[3] = {leg[side].foot[0], leg[side].foot[1], leg[side].foot[2]};
        if (std::fabs(yawRad) <= 1e-12) {
            const LegPlant plant = legPlant(hip1, leg[side].foot, ikLen(hk), ikLen(kf), fb, ikLen(hf));
            if (!plant.closed) return false;
            for (int i = 0; i < 3; ++i) heldFoot[i] = plant.foot[i];
        }
        const LegSolve solved = legSolveTo(leg[side].hip, leg[side].knee, leg[side].foot, hip1, heldFoot);
        if (!solved.ok || (std::fabs(yawRad) > 1e-12 && (solved.limited || solved.footErrM > 1e-4))) return false;
    }
    result.feasible = true;
    if (out) *out = result;
    return true;
}
// F84: pose cost after the exact single clavicle assist and the selected support-elbow policy.
// Costs are squared unit-direction differences, so long bones do not dominate just because of length.
inline bool crouchNeckYawPoseEval(const double neck[3], const CrouchNeckArm arm[2], double yawRad, const double adsPush[3],
                                  double detachWeight, double qualityWeight, double elbowOutM, double trimRad,
                                  CrouchNeckYawPoseMetrics* metricsOut) noexcept {
    if (metricsOut) *metricsOut = CrouchNeckYawPoseMetrics{};
    if (!std::isfinite(yawRad) || !std::isfinite(detachWeight) || !std::isfinite(qualityWeight) || !std::isfinite(elbowOutM) ||
        !std::isfinite(trimRad)) return false;
    double total = 0.0;
    bool checked = false;
    for (int side = 0; side < 2; ++side) {
        if (!arm[side].active) continue;
        checked = true;
        const auto& a = arm[side];
        const double w = a.wSolve;
        if (!std::isfinite(w) || w < 0.0 || w > 1.0 || !(a.l1 > 1e-4 && a.l2 > 1e-4)) return false;
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(a.clavicle[i]) || !std::isfinite(a.shoulder[i]) || !std::isfinite(a.elbow[i]) || !std::isfinite(a.hand[i]) ||
                !std::isfinite(a.target[i]) || !std::isfinite(adsPush[i]) || !std::isfinite(neck[i])) return false;
        double C[3], S[3], E[3], H[3];
        turnAboutVertical(a.clavicle, neck, yawRad, C);
        turnAboutVertical(a.shoulder, neck, yawRad, S);
        turnAboutVertical(a.elbow, neck, yawRad, E);
        turnAboutVertical(a.hand, neck, yawRad, H);
        const double target[3] = {H[0] + w * (a.target[0] - H[0]), H[1] + w * (a.target[1] - H[1]), H[2] + w * (a.target[2] - H[2])};
        const double toTarget[3] = {target[0] - S[0], target[1] - S[1], target[2] - S[2]};
        const double animArm[3] = {H[0] - S[0], H[1] - S[1], H[2] - S[2]};
        const double dist = ikLen(toTarget);
        const double animD = ikLen(animArm);
        double excess = 0.0, R[9], angle = 0.0;
        bool capped = false;
        const double move = ikAssistReachMove(dist, a.l1, a.l2, &excess);
        const double disp[3] = {(dist > 1e-9 ? move * toTarget[0] / dist : 0.0) * w + adsPush[0] * w,
                                (dist > 1e-9 ? move * toTarget[1] / dist : 0.0) * w + adsPush[1] * w,
                                (dist > 1e-9 ? move * toTarget[2] / dist : 0.0) * w + adsPush[2] * w};
        if (ikAssistRot(C, S, disp, R, &angle, &capped)) {
            const double cs[3] = {S[0] - C[0], S[1] - C[1], S[2] - C[2]};
            const double ce[3] = {E[0] - C[0], E[1] - C[1], E[2] - C[2]};
            const double ch[3] = {H[0] - C[0], H[1] - C[1], H[2] - C[2]};
            double turnedS[3], turnedE[3], turnedH[3];
            ikRowRotate(cs, R, turnedS); ikRowRotate(ce, R, turnedE); ikRowRotate(ch, R, turnedH);
            for (int i = 0; i < 3; ++i) { S[i] = C[i] + turnedS[i]; E[i] = C[i] + turnedE[i]; H[i] = C[i] + turnedH[i]; }
        }
        if (metricsOut) metricsOut->assistDeg[side] = angle * 180.0 / kIkPi;
        // The hand target is fixed in body space while the assist rotates the source arm chain.
        const double solvedTarget[3] = {target[0], target[1], target[2]};
        double dLo = 0.0, dHi = 0.0, fLo = 0.0, fHi = 0.0;
        ikElbowDRange(a.l1, a.l2, kIkElbowMinDeg, kIkElbowMaxDeg, &dLo, &dHi);
        ikElbowDRange(a.l1, a.l2, 0.0, 180.0, &fLo, &fHi);
        if (animD < dLo) dLo -= (1.0 - w) * (dLo - animD);
        if (animD > dHi) dHi += (1.0 - w) * (animD - dHi);
        dLo = std::max(dLo, fLo); dHi = std::min(dHi, fHi); if (dHi < dLo) dHi = dLo;
        const double ref[3] = {E[0] - S[0], E[1] - S[1], E[2] - S[2]};
        const IkTwoBone tb = ikTwoBoneFrameD(S, solvedTarget, a.l1, a.l2, ref, dLo, dHi);
        double elbowDir[3];
        if (!tb.ok || tb.limited || !ikUnit(ref, elbowDir)) return false;
        bool legacyClamped = false;
        const double legacyTheta = ikLegacySwivel(tb, elbowDir, ikArmOutSign(side), a.fpPoleValid ? a.fpPole : nullptr, trimRad, elbowOutM,
                                             &legacyClamped);
        double theta = w * ikWrapPi(legacyTheta);
        if (a.stableEligible) {
            IkPhysicalPole seed = a.poleSeed;
            if (!seed.valid) {
                if (!ikElbowRadial(tb, legacyTheta, seed.reference)) return false;
                std::memcpy(seed.previous, seed.reference, sizeof(seed.reference));
                std::memcpy(seed.axis, tb.dir, sizeof(seed.axis));
                seed.valid = true;
            }
            const IkStableSwivel stable = ikStableSwivel(tb, seed, ikArmOutSign(side), elbowOutM);
            if (!stable.ok) return false;
            theta = w * ikWrapPi(stable.theta);
        }
        if (side == 0 && qualityWeight > 0.0) {
            if (!ikRifleSupportTheta(tb, theta, detachWeight, qualityWeight, elbowOutM, trimRad, &theta)) return false;
        }
        double finalE[3];
        tb.elbowAt(theta, finalE);
        if (side == 0 && metricsOut) {
            metricsOut->leftElbowYFromShoulderL1 = (finalE[1] - S[1]) / a.l1;
            metricsOut->leftElbowInteriorDeg = ikElbowInteriorDeg(a.l1, a.l2, tb.D);
        }
        const double rawCS[3] = {a.shoulder[0] - a.clavicle[0], a.shoulder[1] - a.clavicle[1], a.shoulder[2] - a.clavicle[2]};
        const double rawSE[3] = {a.elbow[0] - a.shoulder[0], a.elbow[1] - a.shoulder[1], a.elbow[2] - a.shoulder[2]};
        const double rawEH[3] = {a.hand[0] - a.elbow[0], a.hand[1] - a.elbow[1], a.hand[2] - a.elbow[2]};
        const double up[3] = {0.0, 1.0, 0.0};
        double yaw[9], refCS[3], refSE[3], refEH[3];
        ikRotAxis(up, yawRad, yaw);
        ikRowRotate(rawCS, yaw, refCS); ikRowRotate(rawSE, yaw, refSE); ikRowRotate(rawEH, yaw, refEH);
        const double finalCS[3] = {S[0] - C[0], S[1] - C[1], S[2] - C[2]};
        const double finalSE[3] = {finalE[0] - S[0], finalE[1] - S[1], finalE[2] - S[2]};
        const double finalEH[3] = {tb.hand[0] - finalE[0], tb.hand[1] - finalE[1], tb.hand[2] - finalE[2]};
        const double c0 = ikUnitDirectionCost(refCS, finalCS), c1 = ikUnitDirectionCost(refSE, finalSE), c2 = ikUnitDirectionCost(refEH, finalEH);
        if (!(std::isfinite(c0) && std::isfinite(c1) && std::isfinite(c2))) return false;
        total += c0 + c1 + c2;
    }
    if (!checked || !std::isfinite(total)) return false;
    if (metricsOut) { metricsOut->cost = total; metricsOut->valid = true; }
    return true;
}
inline bool crouchNeckYawPoseCost(const double neck[3], const CrouchNeckArm arm[2], double yawRad, const double adsPush[3],
                                  double detachWeight, double qualityWeight, double elbowOutM, double trimRad,
                                  double* costOut) noexcept {
    CrouchNeckYawPoseMetrics metrics;
    if (!crouchNeckYawPoseEval(neck, arm, yawRad, adsPush, detachWeight, qualityWeight, elbowOutM, trimRad, &metrics)) return false;
    if (costOut) *costOut = metrics.cost;
    return true;
}
inline bool crouchNeckYawPoseNoWorse(const CrouchNeckYawPoseMetrics& candidate, const CrouchNeckYawPoseMetrics& baseline) noexcept {
    if (!candidate.valid || !baseline.valid) return false;
    constexpr double kAssistSlackDeg = 0.05, kBendSlackDeg = 0.1, kElbowHeightSlackL1 = 1e-4;
    return candidate.assistDeg[0] <= baseline.assistDeg[0] + kAssistSlackDeg &&
           candidate.assistDeg[1] <= baseline.assistDeg[1] + kAssistSlackDeg &&
           candidate.leftElbowInteriorDeg <= baseline.leftElbowInteriorDeg + kBendSlackDeg &&
           candidate.leftElbowYFromShoulderL1 <= baseline.leftElbowYFromShoulderL1 + kElbowHeightSlackL1;
}
inline bool crouchNeckYawPoseImproved(const CrouchNeckYawPoseMetrics& candidate, const CrouchNeckYawPoseMetrics& baseline) noexcept {
    if (!candidate.valid || !baseline.valid) return false;
    constexpr double kAssistImproveDeg = 0.05, kBendImproveDeg = 0.1, kElbowLowerL1 = 1e-4;
    return candidate.leftElbowInteriorDeg < baseline.leftElbowInteriorDeg - kBendImproveDeg ||
           candidate.leftElbowYFromShoulderL1 < baseline.leftElbowYFromShoulderL1 - kElbowLowerL1 ||
           candidate.assistDeg[0] + candidate.assistDeg[1] < baseline.assistDeg[0] + baseline.assistDeg[1] - kAssistImproveDeg;
}
inline double crouchNeckYawArmShortfall(const double neck[3], const CrouchNeckArm arm[2], double yawRad, const double adsPush[3]) noexcept {
    if (!std::isfinite(yawRad)) return std::numeric_limits<double>::infinity();
    for (int i = 0; i < 3; ++i) if (!std::isfinite(neck[i]) || !std::isfinite(adsPush[i])) return std::numeric_limits<double>::infinity();
    double shortfallMax = 0.0;
    bool active = false;
    for (int side = 0; side < 2; ++side) {
        if (!arm[side].active) continue;
        active = true;
        const double w = arm[side].wSolve;
        if (!std::isfinite(w) || w < 0.0 || w > 1.0 || !(arm[side].l1 > 1e-4 && arm[side].l2 > 1e-4))
            return std::numeric_limits<double>::infinity();
        double C[3], S[3], H[3];
        turnAboutVertical(arm[side].clavicle, neck, yawRad, C);
        turnAboutVertical(arm[side].shoulder, neck, yawRad, S);
        turnAboutVertical(arm[side].hand, neck, yawRad, H);
        const double target[3] = {H[0] + w * (arm[side].target[0] - H[0]), H[1] + w * (arm[side].target[1] - H[1]),
                                  H[2] + w * (arm[side].target[2] - H[2])};
        const double d[3] = {target[0] - S[0], target[1] - S[1], target[2] - S[2]};
        const double preAssistArm[3] = {H[0] - S[0], H[1] - S[1], H[2] - S[2]};
        const double dist = ikLen(d);
        double excess = 0.0, R[9], angle = 0.0;
        bool capped = false;
        const double reach = ikAssistReachMove(dist, arm[side].l1, arm[side].l2, &excess);
        double disp[3];
        for (int i = 0; i < 3; ++i) disp[i] = ((dist > 1e-9 ? reach * d[i] / dist : 0.0) + adsPush[i]) * w;
        if (ikAssistRot(C, S, disp, R, &angle, &capped)) {
            const double cs[3] = {S[0] - C[0], S[1] - C[1], S[2] - C[2]};
            double turned[3];
            ikRowRotate(cs, R, turned);
            for (int i = 0; i < 3; ++i) S[i] = C[i] + turned[i];
        }
        const double after[3] = {target[0] - S[0], target[1] - S[1], target[2] - S[2]};
        const double afterDist = ikLen(after);
        double lo = 0.0, hi = 0.0, fLo = 0.0, fHi = 0.0;
        ikElbowDRange(arm[side].l1, arm[side].l2, kIkElbowMinDeg, kIkElbowMaxDeg, &lo, &hi);
        ikElbowDRange(arm[side].l1, arm[side].l2, 0.0, 180.0, &fLo, &fHi);
        const double animDist = ikLen(preAssistArm);
        if (animDist < lo) lo -= (1.0 - w) * (lo - animDist);
        if (animDist > hi) hi += (1.0 - w) * (animDist - hi);
        lo = std::max(lo, fLo);
        hi = std::min(hi, fHi);
        if (hi < lo) hi = lo;
        shortfallMax = std::max(shortfallMax, std::max(0.0, std::max(afterDist - hi, lo - afterDist)));
    }
    return active ? shortfallMax : std::numeric_limits<double>::infinity();
}
inline CrouchNeckYaw crouchNeckYawSelect(const double neck[3], const double chestPivot[3], const CrouchNeckArm arm[2], const CrouchNeckLeg leg[2],
                                         double legVertical, const double adsPush[3],
                                         double maxYawDeg = 50.0, double stepDeg = 0.25) noexcept {
    CrouchNeckYaw best;
    if (!(maxYawDeg >= 0.0) || !(stepDeg > 0.0) || !std::isfinite(maxYawDeg) || !std::isfinite(stepDeg)) return best;
    const int steps = static_cast<int>(std::ceil(maxYawDeg / stepDeg));
    for (int radius = 0; radius <= steps; ++radius) {
        for (int signIndex = 0; signIndex < (radius == 0 ? 1 : 2); ++signIndex) {
            const double sign = signIndex == 0 ? 1.0 : -1.0;
            const double deg = std::min(maxYawDeg, radius * stepDeg) * sign;
            CrouchNeckYaw candidate;
            if (!crouchNeckYawFeasible(neck, chestPivot, arm, leg, legVertical, deg * kIkPi / 180.0, adsPush, &candidate)) continue;
            if (radius == 0) return candidate;
            // The coarse radial search finds the first feasible side. Refine that boundary so a one-frame
            // arm-weight or ADS change moves the authored yaw smoothly instead of by a 0.25 degree step.
            double lo = std::max(0.0, std::min(maxYawDeg, (radius - 1) * stepDeg));
            double hi = std::fabs(deg);
            CrouchNeckYaw refinedBest = candidate;
            for (int refine = 0; refine < 24 && hi - lo > 1e-5; ++refine) {
                const double mid = 0.5 * (lo + hi);
                CrouchNeckYaw refined;
                if (crouchNeckYawFeasible(neck, chestPivot, arm, leg, legVertical, sign * mid * kIkPi / 180.0, adsPush, &refined)) {
                    refinedBest = refined;
                    hi = mid;
                } else lo = mid;
            }
            return refinedBest;
        }
    }
    return best;
}
inline CrouchNeckYaw crouchNeckYawSelectQuality(const double neck[3], const double chestPivot[3], const CrouchNeckArm arm[2], const CrouchNeckLeg leg[2],
                                                double legVertical, const double adsPush[3], double detachWeight, double qualityWeight,
                                                double elbowOutM, double trimRad, double maxCorrectionDeg = 50.0,
                                                double stepDeg = 0.5) noexcept {
    const CrouchNeckYaw baseline = crouchNeckYawSelect(neck, chestPivot, arm, leg, legVertical, adsPush, 50.0, 0.25);
    if (!baseline.feasible) return baseline;
    CrouchNeckYaw out = baseline;
    out.baselineYawRad = baseline.yawRad;
    CrouchNeckYawPoseMetrics legacyMetrics;
    if (!crouchNeckYawPoseEval(neck, arm, baseline.yawRad, adsPush, detachWeight, 0.0, elbowOutM, trimRad, &legacyMetrics)) return out;
    out.baselinePoseCost = legacyMetrics.cost;
    out.poseCost = legacyMetrics.cost;
    if (!(qualityWeight > 0.0) || !(detachWeight < 1.0) || !(maxCorrectionDeg > 0.0) || !(stepDeg > 0.0)) return out;
    const double blend = std::max(0.0, std::min(1.0, qualityWeight)) *
                         (1.0 - std::max(0.0, std::min(1.0, detachWeight)));
    if (blend < 1e-5) return out;
    const double maxOffset = std::min(50.0, maxCorrectionDeg);
    const double yawLow = std::max(-50.0, baseline.yawRad * 180.0 / kIkPi - maxOffset) * kIkPi / 180.0;
    const double yawHigh = std::min(50.0, baseline.yawRad * 180.0 / kIkPi + maxOffset) * kIkPi / 180.0;
    const int steps = static_cast<int>(std::ceil(maxOffset / stepDeg));
    struct Candidate { double yaw = 0.0, cost = 0.0; bool tested = false; };
    Candidate candidates[402]{};
    int candidateCount = 0;
    for (int i = -steps; i <= steps; ++i) {
        const double offsetDeg = std::max(-maxOffset, std::min(maxOffset, static_cast<double>(i) * stepDeg));
        const double endpointYaw = std::max(yawLow, std::min(yawHigh, baseline.yawRad + offsetDeg * kIkPi / 180.0));
        CrouchNeckYawPoseMetrics candidateMetrics;
        // Compare a full-support endpoint with the true legacy pose. Activation is applied only
        // after selecting a physically safe endpoint, which prevents an acceptance threshold
        // from making the pose jump partway through the contact ramp.
        if (!crouchNeckYawPoseEval(neck, arm, endpointYaw, adsPush, 0.0, 1.0, elbowOutM, trimRad, &candidateMetrics) ||
            !crouchNeckYawPoseNoWorse(candidateMetrics, legacyMetrics) || !crouchNeckYawPoseImproved(candidateMetrics, legacyMetrics)) continue;
        if (candidateCount < static_cast<int>(sizeof(candidates) / sizeof(candidates[0])))
            candidates[candidateCount++] = {endpointYaw, candidateMetrics.cost, false};
    }
    CrouchNeckYaw best = out;
    for (int attempt = 0; attempt < candidateCount; ++attempt) {
        int selected = -1;
        for (int i = 0; i < candidateCount; ++i)
            if (!candidates[i].tested && (selected < 0 || candidates[i].cost < candidates[selected].cost - 1e-10 ||
                (std::fabs(candidates[i].cost - candidates[selected].cost) <= 1e-10 &&
                 std::fabs(candidates[i].yaw - baseline.yawRad) < std::fabs(candidates[selected].yaw - baseline.yawRad)))) selected = i;
        if (selected < 0) break;
        candidates[selected].tested = true;
        const double endpointYaw = candidates[selected].yaw;
        // Evaluate only candidates in authored-cost order. Most fixtures accept the first; the
        // complete path guard is paid only when a higher-ranked endpoint has a bad transition.
        // The endpoint must be physically better than legacy. The transition itself must stay
        // kinematically feasible; forcing every intermediate pose to beat the legacy comfort
        // metrics can rule out a smooth route to a better endpoint.
        bool pathValid = true;
        constexpr int kPathSamples = 16;
        for (int path = 0; path <= kPathSamples; ++path) {
            const double t = static_cast<double>(path) / kPathSamples;
            const double actualYaw = baseline.yawRad + t * (endpointYaw - baseline.yawRad);
            CrouchNeckYaw checked;
            CrouchNeckYawPoseMetrics pathMetrics;
            if (!crouchNeckYawFeasible(neck, chestPivot, arm, leg, legVertical, actualYaw, adsPush, &checked) ||
                !crouchNeckYawPoseEval(neck, arm, actualYaw, adsPush, 0.0, t, elbowOutM, trimRad, &pathMetrics)) {
                pathValid = false;
                break;
            }
        }
        if (!pathValid) continue;
        CrouchNeckYaw applied;
        const double appliedYaw = baseline.yawRad + blend * (endpointYaw - baseline.yawRad);
        CrouchNeckYawPoseMetrics appliedMetrics;
        if (!crouchNeckYawFeasible(neck, chestPivot, arm, leg, legVertical, appliedYaw, adsPush, &applied) ||
            !crouchNeckYawPoseEval(neck, arm, appliedYaw, adsPush, detachWeight, qualityWeight, elbowOutM, trimRad, &appliedMetrics)) continue;
        best = applied;
        best.yawRad = appliedYaw;
        best.poseCost = appliedMetrics.cost;
        best.baselineYawRad = baseline.yawRad;
        best.baselinePoseCost = legacyMetrics.cost;
        best.qualityAccepted = true;
        break;
    }
    if (!best.qualityAccepted) return out;
    best.baselineYawRad = baseline.yawRad;
    best.baselinePoseCost = legacyMetrics.cost;
    return best;
}

// The state bin of a pin sample: 0 standing, 1 walking, 2 running, 3 jumping (airborne), 4 crouching (F68e). speed: the AMC's horizontal speed (m/s),
// vy its vertical speed, crouched whether the camera is down (pinCrouchedWc: F69d).
// Priority: jumping, crouching, running, walking, standing. (Armed is counted beside, not instead.)
// F69d: jumping is the vertical speed ALONE. F68 read "both animated feet high" after the pin had lifted them 8-16 cm, so every rifle and pistol second
// filed as jumping; the pre-pin feet test is kept as a counter in the head-clearance line (not a bin input).
constexpr double kPinWalkMps = 0.35, kPinRunMps = 3.0, kPinAirMps = 0.8, kPinFeetHighM = 0.12;
inline int pinState(double speed, double vy, bool crouched = false) noexcept {
    if (std::fabs(vy) > kPinAirMps) return 3;
    if (crouched) return 4;
    if (speed >= kPinRunMps) return 2;
    if (speed >= kPinWalkMps) return 1;
    return 0;
}
// F69d: a crouch is the CAMERA down (F69a's w_c, from the cyclopean eye's model height against its standing mean), not the animated legs' reach (F68e: the
// rifle stance reads 0.70-0.89 standing, the old threshold 0.80). w_c >= 0.5 is half way up the smoothstep, 32.5 cm down.
constexpr double kPinCrouchWc = 0.5;
inline bool pinCrouchedWc(double wc) noexcept { return wc == wc && wc >= kPinCrouchWc; }

// ---- F68d: diagnostic FP hands-up telemetry ----------------------------------------------------------------------------------------------------
// The F67 flight found Status.json's SelectedWeapon can arrive after the game has drawn the weapon, so hand motion was measured as a possible earlier
// signal. The FP right attach (joint 51, in the FP model space) is "up" more than 12 cm from its unarmed rest pose, captured while Status.json says
// fists and the hand is still. This detector's signal and weight are telemetry only. Weapon/equip authority remains the SelectedWeapon decision and
// its 0.3 s ramp. A long fists-up interval is vetoed for the diagnostic (an emote or tool pose); it never arms the solve. F69f kept that diagnostic
// veto at 3.0 s after Status.json lagged the hand rise by 1.085 s in the F68 flight.
constexpr double kHandsUpM = 0.12, kHandsDownM = 0.09;   // the hysteresis: up past 12 cm, down again under 9 cm from the rest pose
constexpr double kHandsStillMps = 0.10, kHandsStillSec = 0.4;   // "still": under 10 cm/s for 0.4 s
constexpr double kHandsVetoSec = 3.0;                    // diagnostic signal veto after hands-up with the fists (F69f: Status.json lagged the hand rise by 1.085 s)
constexpr double kHandsRecaptureSec = 3.0;               // a vetoed pose held this still this long is the new unarmed rest
constexpr double kHandsRestTauSec = 1.0;                 // the rest follows a slow drift of the unarmed pose while it is near (time constant)
constexpr double kHandsDropSec = 0.3;                    // the weight's fall when the signal ends
struct FpHandsUp {
    bool haveRest = false;
    double rest[3] = {};
    bool havePrev = false;
    double prev[3] = {};
    double stillSec = 0.0, upFistsSec = 0.0, dist = 0.0, w = 0.0;
    bool up = false, vetoed = false;
    struct Out {
        bool up = false, signal = false, vetoed = false, restCaptured = false;
        double dist = 0.0, w = 0.0;
    };
    // One frame: dt seconds since the last (capped at 0.1), fists = Status.json is NOT showing an item (unknown counts as fists), pos = the FP right attach
    // in the FP model space (metres), posOk = it was readable this frame (a frame without it changes nothing but the weight's fall).
    Out step(double dt, bool fists, const double pos[3], bool posOk) noexcept {
        Out o;
        const double d = !(dt > 0.0) ? 0.0 : dt > 0.1 ? 0.1 : dt;
        if (posOk) {
            double speed = 0.0;
            if (havePrev && d > 1e-4) {
                const double dx = pos[0] - prev[0], dy = pos[1] - prev[1], dz = pos[2] - prev[2];
                speed = std::sqrt(dx * dx + dy * dy + dz * dz) / d;
            }
            stillSec = havePrev && speed < kHandsStillMps ? stillSec + d : 0.0;
            prev[0] = pos[0];
            prev[1] = pos[1];
            prev[2] = pos[2];
            havePrev = true;
            if (haveRest) {
                const double dx = pos[0] - rest[0], dy = pos[1] - rest[1], dz = pos[2] - rest[2];
                dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            } else {
                dist = 0.0;
            }
            up = haveRest && (up ? dist > kHandsDownM : dist > kHandsUpM);
            upFistsSec = up && fists ? upFistsSec + d : 0.0;
            vetoed = upFistsSec > kHandsVetoSec;
            if (fists && stillSec >= kHandsStillSec) {
                if (!haveRest) {
                    for (int i = 0; i < 3; ++i) rest[i] = pos[i];
                    haveRest = true;
                    o.restCaptured = true;
                } else if (!up) {
                    const double a = d / kHandsRestTauSec > 1.0 ? 1.0 : d / kHandsRestTauSec;
                    for (int i = 0; i < 3; ++i) rest[i] += a * (pos[i] - rest[i]);
                } else if (vetoed && stillSec >= kHandsRecaptureSec) {
                    for (int i = 0; i < 3; ++i) rest[i] = pos[i];
                    dist = 0.0;
                    up = false;
                    vetoed = false;
                    upFistsSec = 0.0;
                    o.restCaptured = true;
                }
            }
        }
        const bool signal = up && !vetoed;
        if (signal) w = 1.0;
        else w = w - d / kHandsDropSec > 0.0 ? w - d / kHandsDropSec : 0.0;
        o.up = up;
        o.signal = signal;
        o.vetoed = vetoed;
        o.dist = dist;
        o.w = w;
        return o;
    }
};

// ---- F68g: the armed-move trace's gate -------------------------------------------------------------------------------------------------------
// An event starts when the AMC is armed and over 1 m/s, runs 40 calls, and at most 5 start in any minute, at least a second apart.
constexpr double kArmMoveMps = 1.0;
constexpr uint32_t kArmMoveFrames = 40, kArmMoveMaxPerMin = 5;
struct ArmMoveGate {
    uint32_t evNo = 0, left = 0, within = 0, nStarts = 0;
    uint64_t starts[kArmMoveMaxPerMin] = {}, lastStart = 0;
    // One hook call at QPC q (freq ticks a second): true when it belongs to an event, with the event number and the call's index within it.
    bool step(bool armed, double speedMps, uint64_t q, uint64_t freq, uint32_t* ev, uint32_t* idx) noexcept {
        if (left == 0) {
            if (!(armed && speedMps > kArmMoveMps) || (lastStart && q - lastStart < freq)) return false;
            uint32_t recent = 0;
            for (uint64_t s : starts)
                if (s && q - s < 60 * freq) ++recent;
            if (recent >= kArmMoveMaxPerMin) return false;
            starts[nStarts++ % kArmMoveMaxPerMin] = q;
            lastStart = q;
            ++evNo;
            left = kArmMoveFrames;
            within = 0;
        }
        --left;
        *ev = evNo;
        *idx = within++;
        return true;
    }
};

// ---- F68e: the neck target from the animated head ----------------------------------------------------------------------------------------------
// F67c puts the neck at a CONSTANT offset below and behind the eye, (0, -0.155, -0.09) along the eye's heading, whatever the body's pose. The F67 dump
// (133113, a crouch): the pin said "standing", eye - neck exactly (+0.8, +15.5, +9.0), target error 0, the hidden head's neck opening 16 cm ahead of the
// eye and 8 cm below, filling 8.7% of the left eye; to clear the view the neck has to sit about 9 cm further back (18 cm behind the eye looking straight
// down, against 9 standing). A crouch pitches the 3P chest and neck forward, so the animation's own eye point is further forward of the neck than 9 cm.
// The target now follows it: T = E - Ry(heading(eye) - heading(head 44)) . (P44 + R44 e_h - N0), N0 the animated neck 34, P44 and R44 the animated head
// joint's position and rotation, e_h the eye point in the head joint's frame. e_h is captured once, standing (a run of kPinEyeCalFrames consecutive
// frames: unarmed, still, the head 1.5..1.9 m up and level), so that the standing result is EXACTLY the rest offset: with the animated neck-to-eye
// vector in the head's heading axes equal to (0, 0.155, 0.09) there, the target is the F67c target. Which of the head joint's local axes is its forward
// and its up is found at the same time (the one most aligned with the body's forward and up while standing). Until e_h exists, and whenever the head's
// heading cannot be read, the F67c target is used.
constexpr double kPinStandHeadMinM = 1.50, kPinStandHeadMaxM = 1.90, kPinStandPitchSin = 0.57;   // 35 deg of head pitch at most while calibrating
constexpr uint32_t kPinEyeCalFrames = 45;
constexpr double kPinAnimOffsetClampM = 0.35;   // |v - rest| beyond this is not a pose of this body (a glitch, a ragdoll): clamped. A rigid lean of 90 deg moves the vector 25 cm, 120 deg 31 cm.
// The head's horizontal heading (unit, model axes x right, z forward) from its forward and up vectors: the forward's when it is more than ~20 deg off
// vertical, else the up vector's projection (it points forward when the head looks down, backward when it looks up). False when neither is usable.
inline bool pinHeadHeading(const double fwd[3], const double up[3], double h[3]) noexcept {
    const double fn = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    const double fh = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    if (fn > 1e-9 && fh / fn > 0.35) {
        h[0] = fwd[0] / fh;
        h[1] = 0.0;
        h[2] = fwd[2] / fh;
        return true;
    }
    const double un = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    const double uh = std::sqrt(up[0] * up[0] + up[2] * up[2]);
    if (un > 1e-9 && uh / un > 0.35) {
        const double s = fwd[1] < 0.0 ? 1.0 : -1.0;
        h[0] = s * up[0] / uh;
        h[1] = 0.0;
        h[2] = s * up[2] / uh;
        return true;
    }
    return false;
}
// v turned about the vertical by the angle from the horizontal heading `from` to the heading `to` (both unit, y = 0).
inline void pinTurnHeading(const double v[3], const double from[3], const double to[3], double out[3]) noexcept {
    const double c = from[0] * to[0] + from[2] * to[2], s = from[0] * to[2] - from[2] * to[0];
    out[0] = c * v[0] - s * v[2];
    out[1] = v[1];
    out[2] = s * v[0] + c * v[2];
}
// The head joint's frame as the pin reads it: rows = the matrix's three axis rows in model space (rigid, near unit length); fAxis/fSign and uAxis/uSign
// say which row (and which way) is the head's forward and up. Found while standing: the signed row most aligned with the body's forward (0, 0, 1), then,
// among the other two, the one most aligned with up (0, 1, 0).
struct PinHeadAxes {
    int fAxis = -1, uAxis = -1;
    double fSign = 1.0, uSign = 1.0;
    bool have() const noexcept { return fAxis >= 0 && uAxis >= 0; }
    void fwd(const double rows[3][3], double out[3]) const noexcept {
        for (int i = 0; i < 3; ++i) out[i] = fSign * rows[fAxis][i];
    }
    void up(const double rows[3][3], double out[3]) const noexcept {
        for (int i = 0; i < 3; ++i) out[i] = uSign * rows[uAxis][i];
    }
};
inline bool pinFindHeadAxes(const double rows[3][3], PinHeadAxes* ax) noexcept {
    int f = -1;
    double fs = 1.0, best = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double z = rows[i][2];
        if (std::fabs(z) > best) {
            best = std::fabs(z);
            f = i;
            fs = z < 0.0 ? -1.0 : 1.0;
        }
    }
    if (f < 0 || best < 0.7) return false;   // the head is not facing about where the body does
    int u = -1;
    double us = 1.0, bu = 0.0;
    for (int i = 0; i < 3; ++i) {
        if (i == f) continue;
        const double y = rows[i][1];
        if (std::fabs(y) > bu) {
            bu = std::fabs(y);
            u = i;
            us = y < 0.0 ? -1.0 : 1.0;
        }
    }
    if (u < 0 || bu < 0.5) return false;
    ax->fAxis = f;
    ax->fSign = fs;
    ax->uAxis = u;
    ax->uSign = us;
    return true;
}
// The rows are a rigid frame: each of unit length within 20% and mutually near perpendicular.
inline bool pinRowsRigid(const double rows[3][3]) noexcept {
    for (int i = 0; i < 3; ++i) {
        const double l2 = rows[i][0] * rows[i][0] + rows[i][1] * rows[i][1] + rows[i][2] * rows[i][2];
        if (!(l2 > 0.64 && l2 < 1.44)) return false;
        for (int j = i + 1; j < 3; ++j) {
            const double d = rows[i][0] * rows[j][0] + rows[i][1] * rows[j][1] + rows[i][2] * rows[j][2];
            if (!(std::fabs(d) < 0.2)) return false;
        }
    }
    return true;
}
// The eye point in the head joint's frame from one standing frame: the animated neck-to-head vector p44 - n0 plus R44 e_h must equal, in the head's
// heading axes, (right 0, up 0.155, forward 0.09). False when the head's frame is not readable.
inline bool pinEyeInHead(const double p44[3], const double rows[3][3], const PinHeadAxes& ax, const double n0[3], double eh[3]) noexcept {
    if (!ax.have() || !pinRowsRigid(rows)) return false;
    double f[3], u[3], h[3];
    ax.fwd(rows, f);
    ax.up(rows, u);
    if (!pinHeadHeading(f, u, h)) return false;
    const double vt[3] = {kPinRestBackM * h[0], kPinRestUpM, kPinRestBackM * h[2]};
    const double d[3] = {vt[0] + n0[0] - p44[0], vt[1] + n0[1] - p44[1], vt[2] + n0[2] - p44[2]};
    for (int i = 0; i < 3; ++i) eh[i] = d[0] * rows[i][0] + d[1] * rows[i][1] + d[2] * rows[i][2];   // unit rows: the coordinate along each
    return true;
}
// The calibration: kPinEyeCalFrames consecutive standing frames agreeing on the head's axes, averaged. A frame that is not standing (or reads another
// axis assignment) starts the run again. Once done it is kept (the caller resets it for another skeleton).
struct PinEyeCal {
    bool have = false;
    uint32_t n = 0;
    double sum[3] = {}, e[3] = {}, headYSum = 0.0, headY = 0.0;
    double eyeYSum = 0.0, eyeY = 0.0, eyeYRawSum = 0.0, eyeYRaw = 0.0;   // F69a/F73: standing eye height normalized for LOCAL pitch, plus raw fallback
    PinHeadAxes axes, runAxes;
    void reset() noexcept { *this = PinEyeCal{}; }
    void restart() noexcept {
        n = 0;
        sum[0] = sum[1] = sum[2] = 0.0;
        headYSum = 0.0;
        eyeYSum = 0.0;
        eyeYRawSum = 0.0;
        runAxes = PinHeadAxes{};
    }
    // One frame: standing says whether this frame may calibrate (the caller's gate); eyeYNow is the LOCAL-pitch-normalized height and eyeYRawNow is
    // the unmodified 3P model height for the invalid-LOCAL fallback (F73). Returns true on the frame the calibration completes.
    bool add(bool standing, const double p44[3], const double rows[3][3], const double n0[3], double eyeYNow,
             double eyeYRawNow = std::numeric_limits<double>::quiet_NaN()) noexcept {
        if (have) return false;
        if (!std::isfinite(eyeYRawNow)) eyeYRawNow = eyeYNow;   // older callers and no-LOCAL fallback use one height domain
        if (!std::isfinite(eyeYNow)) standing = false;
        if (!std::isfinite(eyeYRawNow)) standing = false;
        PinHeadAxes ax;
        double eh[3];
        bool ok = standing && pinRowsRigid(rows) && pinFindHeadAxes(rows, &ax);
        if (ok) {
            double f[3];
            ax.fwd(rows, f);
            ok = std::fabs(f[1]) <= kPinStandPitchSin;   // the head level (35 deg of pitch at most)
        }
        if (ok && runAxes.have())
            ok = ax.fAxis == runAxes.fAxis && ax.uAxis == runAxes.uAxis && ax.fSign == runAxes.fSign && ax.uSign == runAxes.uSign;
        if (!ok || !pinEyeInHead(p44, rows, ax, n0, eh)) {
            restart();
            return false;
        }
        runAxes = ax;
        for (int i = 0; i < 3; ++i) sum[i] += eh[i];
        headYSum += p44[1];
        eyeYSum += eyeYNow;
        eyeYRawSum += eyeYRawNow;
        if (++n < kPinEyeCalFrames) return false;
        for (int i = 0; i < 3; ++i) e[i] = sum[i] / static_cast<double>(n);
        headY = headYSum / static_cast<double>(n);
        eyeY = eyeYSum / static_cast<double>(n);
        eyeYRaw = eyeYRawSum / static_cast<double>(n);
        axes = runAxes;
        have = true;
        return true;
    }
};
// The head joint at a standing commander's height (F103: also the F98c eye level's test of an upright unarmed stance).
inline bool pinHeadUpright(double headY) noexcept { return headY >= kPinStandHeadMinM && headY <= kPinStandHeadMaxM; }
// The standing gate of the calibration: unarmed and not in ADS, the AMC still, the head joint's height in range (its pitch is gated in add()).
inline bool pinCalStanding(double speed, bool armed, bool ads, double headY) noexcept {
    return !armed && !ads && speed < kPinWalkMps && pinHeadUpright(headY);
}
// ---- F69a: the neck target follows the camera's crouch, not the stance's head -----------------------------------------------------------------------
// F68 (log 152023, dump 152258): the animated neck-to-eye vector of the armed rifle stance is (5.3, 6.6, 15.4) cm, not the rest (0, 15.5, 9.0): the
// stance leans the head 63 deg from the neck (the cheek weld), so the F68e target put the neck 8.9 cm up toward the eye and 6.4 cm back and the eye sat
// at the neck joint. The animated vector is for a CROUCH (the F67 dump: the chest pitched forward, the eye 70 cm down). The signal that tells them apart is
// the camera itself: the cyclopean eye's height in the 3P model space against its mean over the calibration's 45 standing frames (h_stand). The stance
// lowers the game's camera by under 4 cm (rifle 1.58-1.61 m against 1.63 m); a crouch by 70 cm. v = lerp(rest, v_anim, w_c), w_c = smoothstep(0.20, 0.45,
// h_stand - E_y): 0 within 20 cm, 1 from 45 cm down. Exogenous (the game's camera and a calibration), no switch, no memory.
constexpr double kPinCrouchLoM = 0.20, kPinCrouchHiM = 0.45;
inline double pinCrouchWeight(bool have, double hStand, double eyeY) noexcept {
    if (!have || !std::isfinite(hStand) || !std::isfinite(eyeY)) return 0.0;
    return smoothstep3(kPinCrouchLoM, kPinCrouchHiM, hStand - eyeY);
}
inline double pinPitchRestUp(double localPitchRad, bool valid) noexcept {
    if (!valid || !std::isfinite(localPitchRad)) return kPinRestUpM;
    return kPinRestUpM * std::cos(localPitchRad) + kPinRestBackM * std::sin(localPitchRad);
}
inline bool pinNaturalLocalPitch(const float local[12], double* pitchRad) noexcept {
    if (!local || !pitchRad || !std::isfinite(local[6])) return false;
    const double s = std::max(-1.0, std::min(1.0, -static_cast<double>(local[6])));
    *pitchRad = std::asin(s);
    return std::isfinite(*pitchRad);
}
inline double pinEyeHeightNormalized(double eyeY, double localPitchRad, bool valid) noexcept {
    return eyeY - (pinPitchRestUp(localPitchRad, valid) - kPinRestUpM);
}
struct PinAnimTarget {
    bool anim = false;        // the animated vector was computed (a calibration and a readable head); the vector used is the blend below
    bool clamped = false;
    double wc = 0.0;          // F69a: the animated vector's weight (the camera's crouch); F169: held under the caller's cap
    double wcUncapped = 0.0;  // F169: the same weight before the cap (equal to wc at a cap of 1)
    double dEyeY = 0.0;       // F69a: E_y - h_stand (m, negative when down); 0 without a calibration
    double vUsed[3] = {};     // neck -> eye as used (model axes)
    double vAnim[3] = {};     // F69a: the animated vector (after the clamp), whether or not it was used
    double hEye[3] = {}, hHead[3] = {};
    double eyeYSelected = 0.0;   // F73: normalized height when LOCAL pitch is valid; raw height on its yaw-only fallback
};
// The weapon-independent rest target, rotated by physical LOCAL head pitch and scene-eye heading.
// Callers must retain their existing target when the physical pitch is unavailable.
inline bool pinNeckTargetPhysicalRest(const double eye[3], const double eyeFwd[3], const double* eyeUp, double localPitchRad,
                                      bool localPitchValid, double out[3], double offset[3] = nullptr) noexcept {
    if (!eye || !eyeFwd || !eyeUp || !out || !localPitchValid || !std::isfinite(localPitchRad)) return false;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(eye[i]) || !std::isfinite(eyeFwd[i]) || (eyeUp && !std::isfinite(eyeUp[i]))) return false;
    if (!(std::sqrt(eyeFwd[0] * eyeFwd[0] + eyeFwd[1] * eyeFwd[1] + eyeFwd[2] * eyeFwd[2]) > 1e-9)) return false;
    if (!(std::sqrt(eyeUp[0] * eyeUp[0] + eyeUp[1] * eyeUp[1] + eyeUp[2] * eyeUp[2]) > 1e-9)) return false;
    double hEye[3];
    if (!pinHeadHeading(eyeFwd, eyeUp, hEye)) pinHeading(eyeFwd, hEye);
    const double cp = std::cos(localPitchRad), sp = std::sin(localPitchRad);
    const double restUp = kPinRestUpM * cp + kPinRestBackM * sp;
    const double restForward = kPinRestBackM * cp - kPinRestUpM * sp;
    const double v[3] = {restForward * hEye[0], restUp, restForward * hEye[2]};
    for (int i = 0; i < 3; ++i) {
        out[i] = eye[i] - v[i];
        if (offset) offset[i] = v[i];
    }
    return true;
}
inline bool pinRestReferenceGenerationAllowed(bool currentAvailable, uint32_t sampleGeneration, uint32_t currentGeneration) noexcept {
    return sampleGeneration != 0 && (!currentAvailable || sampleGeneration == currentGeneration);
}
// The neck target: E - v, v = lerp(rest, the animated neck-to-eye vector turned from the head's heading to the eye's, w_c). eyeFwd: the eye's forward in
// model axes (its heading is pinHeading's, with the same fallback to the body's forward). Without a calibration or a readable head, the F67c target.
// F169: wcCap is a ceiling on w_c (the caller passes the animated neck's posture weight while the aim is pitched, where the camera's sink reads as a crouch); at 1, the default, w_c is exactly
// what it was.
inline void pinNeckTargetAnim(const double eye[3], const double eyeFwd[3], const double p44[3], const double rows[3][3], const double n0[3],
                              const PinEyeCal& cal, double out[3], PinAnimTarget* info, double localPitchRad = 0.0, bool localPitchValid = false,
                              const double* eyeUp = nullptr, double wcCap = 1.0) noexcept {
    PinAnimTarget local;
    PinAnimTarget& pa = info ? *info : local;
    pa = PinAnimTarget{};
    if (!eyeUp || !pinHeadHeading(eyeFwd, eyeUp, pa.hEye)) pinHeading(eyeFwd, pa.hEye);
    const double pitch = localPitchValid && std::isfinite(localPitchRad) ? localPitchRad : 0.0;
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    const double restUp = kPinRestUpM * cp + kPinRestBackM * sp;
    const double restForward = kPinRestBackM * cp - kPinRestUpM * sp;
    const double rest[3] = {restForward * pa.hEye[0], restUp, restForward * pa.hEye[2]};
    const double restAnimClamp[3] = {kPinRestBackM * pa.hEye[0], kPinRestUpM, kPinRestBackM * pa.hEye[2]};
    pa.eyeYSelected = pinEyeHeightNormalized(eye[1], pitch, localPitchValid);
    for (int i = 0; i < 3; ++i) pa.vUsed[i] = pa.vAnim[i] = rest[i];
    const double hStand = localPitchValid && std::isfinite(localPitchRad) ? cal.eyeY : cal.eyeYRaw;
    pa.wc = pa.wcUncapped = pinCrouchWeight(cal.have, hStand, pa.eyeYSelected);
    if (wcCap < pa.wc) pa.wc = wcCap;   // F169: a cap that is not a number compares false and leaves w_c
    pa.dEyeY = cal.have && std::isfinite(hStand) && std::isfinite(pa.eyeYSelected) ? pa.eyeYSelected - hStand : 0.0;
    if (cal.have && pinRowsRigid(rows)) {
        double f[3], u[3];
        cal.axes.fwd(rows, f);
        cal.axes.up(rows, u);
        if (!pinHeadHeading(f, u, pa.hHead)) {   // the head's heading unreadable: no turn (the animation's own heading stands for the eye's)
            pa.hHead[0] = pa.hEye[0];
            pa.hHead[1] = 0.0;
            pa.hHead[2] = pa.hEye[2];
        }
        double v[3];
        for (int i = 0; i < 3; ++i) v[i] = p44[i] + cal.e[0] * rows[0][i] + cal.e[1] * rows[1][i] + cal.e[2] * rows[2][i] - n0[i];
        double vt[3];
        pinTurnHeading(v, pa.hHead, pa.hEye, vt);
        double dev[3] = {vt[0] - restAnimClamp[0], vt[1] - restAnimClamp[1], vt[2] - restAnimClamp[2]};
        const double dn = std::sqrt(dev[0] * dev[0] + dev[1] * dev[1] + dev[2] * dev[2]);
        if (dn == dn && dn <= kPinAnimOffsetClampM) {
            for (int i = 0; i < 3; ++i) pa.vAnim[i] = vt[i];
            pa.anim = true;
        } else if (dn == dn) {
            for (int i = 0; i < 3; ++i) pa.vAnim[i] = restAnimClamp[i] + dev[i] * kPinAnimOffsetClampM / dn;
            pa.anim = true;
            pa.clamped = true;
        }
        if (pa.anim)
            for (int i = 0; i < 3; ++i) pa.vUsed[i] = rest[i] + pa.wc * (pa.vAnim[i] - rest[i]);
    }
    for (int i = 0; i < 3; ++i) out[i] = eye[i] - pa.vUsed[i];
}

// ---- F67f: while armed, both hands stay on the grips on every frame ----------------------------------------------------------------------------
// The F66 flight: the arm IK's base weight fell to 0.0-0.5 for 0.1-0.3 s whenever the FP snapshot or G was older than 100 ms (the log's "held:
// the FP snapshot is older than 100 ms" and "G is older than 100 ms", on every aim move and every stall), the hand target moved at most 5 cm a call
// and the clavicle assist likewise, and the torso square-up was clamped to +-50 deg through a 0.3 s ramp. Each let the game's animated hand show or
// the solved hand trail. The armed path now: (1) the FP snapshot is a model-space (FP root frame) record, valid at any age, G (this frame's FP root
// and AMC) is used while at most kArmedGMaxMs old; (2) the solve weight is 1 once the armed ramp is; (3) no per-call cap on the hand or the assist;
// (4) the square-up angle is phi itself.
constexpr uint64_t kArmedFpMaxMs = 30000, kArmedGMaxMs = 1000;
inline bool ikSnapshotUsable(bool have, uint64_t ageMs, bool armed) noexcept { return have && (armed ? ageMs <= kArmedFpMaxMs : ageMs <= kIkFreshMs); }
inline bool ikGUsable(bool have, uint64_t ageMs, bool armed) noexcept { return have && (armed ? ageMs <= kArmedGMaxMs : ageMs <= kIkFreshMs); }
// The arm's solve weight: armed and the armed ramp complete: 1 whatever the base weight, the gate or the snapshot's age; otherwise the base weight
// times the ramp (0 unarmed: the game's arms).
inline double ikArmedSolveWeight(bool armed, double armedRamp, double baseWeight) noexcept {
    const double r = unit01(armedRamp);
    if (armed && r >= 1.0) return 1.0;
    return unit01(baseWeight) * r;
}
// The square-up's applied angle (radians). Armed: phi itself (no clamp, no ramp, no amount); else amount x ramp x phi clamped to +-kIkSquareMaxDeg.
// *over: |phi| is past the old limit.
inline double squareApplied(double phi, bool armed, double amount, double ramp, bool* over) noexcept {
    const double lim = kIkSquareMaxDeg * kIkPi / 180.0;
    if (over) *over = phi == phi && std::fabs(phi) > lim;
    if (!(phi == phi)) return 0.0;
    if (armed) return phi;
    bool clamped = false;
    return amount * ramp * ikSquareClampRad(phi, &clamped);
}
// The hand target offset (a vector from the animated hand): armed: the whole offset times the weight (no cap); else capped toward it by the caller.
inline bool handCapApplies(bool armed) noexcept { return !armed; }

// ---- F67e: the aim-jump trigger ------------------------------------------------------------------------------------------------------------------
// The FP root's yaw rate (deg/s, signed, the yaw step wrapped into +-180) between two records dt seconds apart; an event starts when it is over 30 deg/s
// in size while armed.
constexpr double kAimTriggerDps = 30.0;
inline double yawRateDps(double prevDeg, double curDeg, double dtSec) noexcept {
    if (!(dtSec > 0.0) || !(prevDeg == prevDeg) || !(curDeg == curDeg)) return 0.0;
    double d = curDeg - prevDeg;
    while (d > 180.0) d -= 360.0;
    while (d < -180.0) d += 360.0;
    return d / dtSec;
}
inline bool aimTrigger(double rateDps, bool armed) noexcept { return armed && std::fabs(rateDps) > kAimTriggerDps; }

// ---- F68c: G from one game frame ---------------------------------------------------------------------------------------------------------------
// G = F_root x inv(M3P) takes the FP root from the FP AMC's P1 call and the 3P AMC matrix from the local 3P AMC's. The F67 flight (log 132256): the
// two calls come in either order within a frame, and G was built by the 3P call from the FP root of the last call, which on about 40% of the frames
// was the previous frame's: the hands solved exactly onto a grip off the drawn weapon by body speed x one frame (4.4 cm at 4 m/s, 90 Hz), switching
// frame to frame between 0 and 1 frame: the arm stutter. The fix: each call takes the game-frame index (a call of a kind already seen this frame
// opens the next frame) and G is built by whichever of the two comes second in a frame, from the two halves of the SAME index, once.
// F132 (the Frontier SRV flight edvr_gfx_20261027_064839 from 07:03:13, after a new avatar was created, and flight 182525 on rc.1-27): on() assumes the calls come in
// a fixed order and never re-synchronises. Started on an FP call while the game's order is 3P then FP, it paired FP(n) with 3P(n+1), about 11 ms apart, so take()
// refused every frame for good (6927 of 7351 frames), G froze for up to 1 s and the arm IK pulled the hands onto a stale grip. Now a pair refused as far apart
// re-seeds the phase: the refused call (the second of the pair) becomes the FIRST call of a new frame, and the next call of the other kind pairs with it.
constexpr double kP1PairMaxGapMs = 10.0;   // a pair whose two calls are further apart than this is not one frame's (an index shifted by a missing call)
struct P1FramePairing {
    uint32_t frame = 1;                       // the current game frame's index
    bool seen[2] = {false, false};            // [0] the FP AMC's call, [1] the local 3P AMC's call, seen in this frame
    int firstKind = -1;                       // which of the two came first in this frame
    bool published = false;                   // G was built from this frame's pair
    uint32_t pairedFrame = 0;                 // the last frame G was built for
    // Counters for the second's line (the caller reads and clears them).
    uint64_t frames = 0, pairs = 0, noPair = 0, tpFirst = 0, fpFirst = 0, farApart = 0;
    // F132: pairs refused as far apart that re-seeded the phase, the gap of the refused pairs (ms), and which call was first in them.
    uint64_t reseeds = 0, farFpFirst = 0, farTpFirst = 0;
    double farGapMinMs = 0.0, farGapMaxMs = 0.0, farGapSumMs = 0.0;
    void clearSecond() noexcept {
        frames = pairs = noPair = tpFirst = fpFirst = farApart = reseeds = farFpFirst = farTpFirst = 0;
        farGapMinMs = farGapMaxMs = farGapSumMs = 0.0;
    }
    // A call of `kind` (0 FP, 1 3P): its game-frame index. A kind already seen this frame opens the next frame.
    uint32_t on(int kind) noexcept {
        const int k = kind ? 1 : 0;
        if (seen[k]) {
            if (!published && (seen[0] || seen[1])) ++noPair;
            ++frame;
            ++frames;
            seen[0] = seen[1] = false;
            firstKind = -1;
            published = false;
        }
        if (seen[1 - k]) {   // the second call of the frame
            if (firstKind == 1) ++tpFirst;
            else ++fpFirst;
        } else {
            firstKind = k;
        }
        seen[k] = true;
        return frame;
    }
    // The second caller: may G be built from the FP half (frame fpFrame, QPC fpQpc) and the 3P half (frame tpFrame, QPC tpQpc)? Once per frame, same
    // index, and the two calls near each other (ticks: QPC ticks, freq: per second).
    // F132: callerKind (0 FP, 1 3P; the default -1 keeps the old rule) is the call that asks, the second call of the pair. When the pair is refused as far apart the
    // phase is re-seeded: that call becomes the first call of a new frame, whose index is returned in *callerFrame (the caller stores it with its half, so the next call
    // of the other kind, which gets the same index from on(), finds it).
    bool take(bool fpHave, uint32_t fpFrame, uint64_t fpQpc, bool tpHave, uint32_t tpFrame, uint64_t tpQpc, uint64_t freq, int callerKind = -1,
              uint32_t* callerFrame = nullptr) noexcept {
        if (!fpHave || !tpHave || fpFrame != tpFrame || fpFrame == pairedFrame) return false;
        const uint64_t gap = fpQpc > tpQpc ? fpQpc - tpQpc : tpQpc - fpQpc;
        const double gapMs = freq > 0 ? static_cast<double>(gap) * 1000.0 / static_cast<double>(freq) : 0.0;
        if (freq > 0 && gapMs > kP1PairMaxGapMs) {
            ++farApart;
            if (farApart == 1 || gapMs < farGapMinMs) farGapMinMs = gapMs;
            if (gapMs > farGapMaxMs) farGapMaxMs = gapMs;
            farGapSumMs += gapMs;
            if (firstKind == 0) ++farFpFirst;
            else if (firstKind == 1) ++farTpFirst;
            if (callerKind == 0 || callerKind == 1) {
                const uint32_t f = reseed(callerKind);
                if (callerFrame) *callerFrame = f;
            }
            return false;
        }
        pairedFrame = fpFrame;
        published = true;
        ++pairs;
        return true;
    }
    // F132: the refused call of `kind` opens the next frame as its first call: the other kind's next call is its second.
    uint32_t reseed(int kind) noexcept {
        const int k = kind ? 1 : 0;
        if (!published) ++noPair;   // the refused frame never built G
        ++reseeds;
        ++frame;
        ++frames;
        seen[0] = seen[1] = false;
        seen[k] = true;
        firstKind = k;
        published = false;
        return frame;
    }
};

// ---- F75: the armed torso keeps its authored stance; barrel and shoulder yaw are measurements only -----------------------------------------------
// F68 (log 152023, section 4 of the review): the rifle stance blades the upper body 35-40 deg while the weapon points down the aim. The square-up turned the
// SHOULDER LINE square to the aim, which de-bladed the stance, ran the left (foregrip) arm out of reach (1.03) and made the reach rule slide the whole body
// 16-23 cm forward. Armed, the body keeps the game's authored stance because the animated barrel and wrist are overwritten by arm IK; their yaw is logged only.
// The barrel axis of each rig is the signed row of its attach matrix most aligned with the aim
// (G's forward), found as pinFindHeadAxes finds the head's: it is picked per item at the draw (the same pick for kBarrelLatchFrames consecutive armed calls
// latches it; an item change or the end of arming drops it). ONE function, squareYaw, serves the reach rule's U* (stage 0), the stage-1 angle and the
// written turn; unarmed it is ikSquareToForward unchanged. The armed angle is always zero; shoulder and barrel candidates are diagnostic measurements.
constexpr double kBarrelMinDot = 0.7;           // a row within 45 deg of the aim can be the barrel
constexpr double kBarrelMinXz = 0.25;           // the barrel's horizontal part under this (a weapon pointing up or down) has no yaw to turn the torso by
constexpr uint32_t kBarrelLatchFrames = 10;     // consecutive armed calls with the same pick in both rigs
// The signed row of a rigid matrix's three axis rows most aligned with `aim` (|dot| largest, rows and aim taken as directions); false under kBarrelMinDot.
inline bool barrelRow(const double rows[3][3], const double aim[3], int* axis, double* sign) noexcept {
    double au[3];
    if (!ikUnit(aim, au)) return false;
    int best = -1;
    double bd = 0.0, bs = 1.0;
    for (int i = 0; i < 3; ++i) {
        double ru[3];
        if (!ikUnit(rows[i], ru)) return false;
        const double d = ikDot(ru, au);
        if (std::fabs(d) > bd) {
            bd = std::fabs(d);
            best = i;
            bs = d < 0.0 ? -1.0 : 1.0;
        }
    }
    if (best < 0 || !(bd >= kBarrelMinDot)) return false;
    *axis = best;
    *sign = bs;
    return true;
}
struct BarrelDirs {
    bool ok = false;        // both barrels are known this call (latched, or picked now)
    bool latched = false;
    double b3[3] = {}, bF[3] = {};   // the 3P animated attach's barrel and the FP attach's through G (3P model space, unit)
    int a3 = -1, aF = -1;
    double s3 = 1.0, sF = 1.0;
};
struct BarrelAxes {
    bool have = false;
    int a3 = -1, aF = -1;
    double s3 = 1.0, sF = 1.0;
    int runA3 = -1, runAF = -1;
    double runS3 = 1.0, runSF = 1.0;
    uint32_t run = 0;
    void reset() noexcept { *this = BarrelAxes{}; }
    // One armed call: rows3p = the 3P attach's rotation rows in the 3P model space, rowsFp = the FP attach's through G (same space), aim = G's forward.
    BarrelDirs step(const double rows3p[3][3], const double rowsFp[3][3], const double aim[3]) noexcept {
        BarrelDirs o;
        int p3 = -1, pF = -1;
        double q3 = 1.0, qF = 1.0;
        const bool got3 = barrelRow(rows3p, aim, &p3, &q3), gotF = barrelRow(rowsFp, aim, &pF, &qF);
        if (!have) {
            if (got3 && gotF) {
                if (run > 0 && p3 == runA3 && pF == runAF && q3 == runS3 && qF == runSF) {
                    ++run;
                } else {
                    runA3 = p3;
                    runAF = pF;
                    runS3 = q3;
                    runSF = qF;
                    run = 1;
                }
                if (run >= kBarrelLatchFrames) {
                    have = true;
                    a3 = p3;
                    aF = pF;
                    s3 = q3;
                    sF = qF;
                }
                o.a3 = p3;
                o.aF = pF;
                o.s3 = q3;
                o.sF = qF;
                o.ok = true;
            } else {
                run = 0;
            }
        }
        if (have) {
            o.a3 = a3;
            o.aF = aF;
            o.s3 = s3;
            o.sF = sF;
            o.ok = true;
            o.latched = true;
        }
        if (o.ok) {
            double v3[3], vF[3];
            for (int i = 0; i < 3; ++i) {
                v3[i] = o.s3 * rows3p[o.a3][i];
                vF[i] = o.sF * rowsFp[o.aF][i];
            }
            if (!ikUnit(v3, o.b3) || !ikUnit(vF, o.bF)) o.ok = false;
        }
        return o;
    }
};
enum SquareSrc : int { kSquareShoulders = 0, kSquareWeapon = 1, kSquarePreservePose = 2 };
enum SquareWeaponWhy : int { kSquareWeaponReady = 0, kSquareWeaponNoAxes = 1, kSquareWeapon3pVertical = 2, kSquareWeaponFpVertical = 3, kSquareWeaponBothVertical = 4,
                             kSquareWeaponBadYaw = 5 };
struct SquareYaw {
    bool ok = false;               // phi is available
    bool haveSh = false, haveW = false;
    double phi = 0.0;              // radians: the one the callers apply
    double phiSh = 0.0, phiW = 0.0;   // both, for the log
    int src = kSquareShoulders;
    int weaponWhy = kSquareWeaponNoAxes;
    double b3Xz = 0.0, bFXz = 0.0;  // selected latched barrel rows' horizontal lengths, to explain an unavailable yaw
};
// The yaw (ikYawFromTo's sense) that squares the upper body: armed poses preserve the game's authored stance; barrel and shoulder angles are measurements
// only because the pre-solve weapon pose is overwritten by arm IK. Unarmed, keep the upper-arm line's existing square-up to the aim's right.
inline SquareYaw squareYaw(bool armed, const BarrelDirs& bd, const double uL[3], const double uR[3], const double fwd[3]) noexcept {
    SquareYaw o;
    o.haveSh = ikSquareToForward(uL, uR, fwd, &o.phiSh);
    if (bd.ok) {
        const double a[3] = {bd.b3[0], 0.0, bd.b3[2]}, b[3] = {bd.bF[0], 0.0, bd.bF[2]};
        o.b3Xz = ikLen(a);
        o.bFXz = ikLen(b);
        if (!std::isfinite(o.b3Xz) || !std::isfinite(o.bFXz)) {
            o.weaponWhy = kSquareWeaponBadYaw;
        } else if (o.b3Xz >= kBarrelMinXz && o.bFXz >= kBarrelMinXz) {
            o.phiW = ikYawFromTo(a, b);
            o.haveW = std::isfinite(o.phiW);
            if (!o.haveW) o.weaponWhy = kSquareWeaponBadYaw;
            else o.weaponWhy = kSquareWeaponReady;
        } else if (o.b3Xz < kBarrelMinXz && o.bFXz < kBarrelMinXz) {
            o.weaponWhy = kSquareWeaponBothVertical;
        } else if (o.b3Xz < kBarrelMinXz) {
            o.weaponWhy = kSquareWeapon3pVertical;
        } else {
            o.weaponWhy = kSquareWeaponFpVertical;
        }
    }
    if (armed) {
        o.phi = 0.0;
        o.src = kSquarePreservePose;
        o.ok = true;
    } else if (o.haveSh) {
        o.phi = o.phiSh;
        o.ok = true;
    }
    return o;
}
// The unit rows of a row-vector matrix's upper 3x3 (false when a row is degenerate).
inline bool matRowsUnit(const float m[16], double rows[3][3]) noexcept {
    for (int i = 0; i < 3; ++i) {
        const double v[3] = {m[4 * i], m[4 * i + 1], m[4 * i + 2]};
        if (!ikUnit(v, rows[i])) return false;
    }
    return true;
}

// ---- F69d: the head-clearance instrument ------------------------------------------------------------------------------------------------------
// F68: the eye sat at the neck joint with the rifle drawn; the chest 24 and the clavicles 35/36 were never logged. The eye against each, in the eye's
// heading axes (right, up, forward), as finally written; "inside" = the eye under the clavicle line + 8 cm AND less than 12 cm ahead of the chest 24.
constexpr double kInsideBelowClavM = 0.08, kInsideAheadChestM = 0.12;
inline bool headInside(double eyeMinusClavUp, double eyeMinusChestFwd) noexcept {
    return eyeMinusClavUp < kInsideBelowClavM && eyeMinusChestFwd < kInsideAheadChestM;
}
// v (model axes) in the heading h's axes: right = (h.z, 0, -h.x), up, forward = h (h a horizontal unit).
inline void headingAxes(const double h[3], const double v[3], double out[3]) noexcept {
    out[0] = v[0] * h[2] - v[2] * h[0];
    out[1] = v[1];
    out[2] = v[0] * h[0] + v[2] * h[2];
}

// ---- F69g: the grip frames (D1) --------------------------------------------------------------------------------------------------------------
// Γ = A x inv(H): the attach joint's frame in the HAND's own axes (a point in attach axes times Γ lands in hand axes). The FP rig's (51/52 in 44/37, the
// FP snapshot) against the 3P animation's (L_A = M0[A] x inv(M0[H]), 66/67 in 59/58). The hand is put on the grip by T_H = inv(L_A) x T_A, which is right only
// if both Γ are the same item frame; ΔΓ = Γ_3p x inv(Γ_fp) says by how much they differ (identity: the same frame).
inline bool gripFrame(const float A[16], const float H[16], float G[16]) noexcept {
    float invH[16];
    if (!pfzAffineInverse(H, invH)) return false;
    pfzMul(A, invH, G);
    return true;
}
inline bool gripDelta(const float g3p[16], const float gfp[16], float d[16]) noexcept {
    float inv[16];
    if (!pfzAffineInverse(gfp, inv)) return false;
    pfzMul(g3p, inv, d);
    return true;
}
// A frame's rotation as its axis (unit, in the axes the frame is expressed in; right-handed) and angle (degrees, 0..180). The rows are normalised first.
// An angle under 1e-4 deg has no axis (zeros).
inline bool frameAxisAngle(const float m[16], double axis[3], double* deg) noexcept {
    double rows[3][3], R[9], q[4];
    axis[0] = axis[1] = axis[2] = 0.0;
    *deg = 0.0;
    if (!matRowsUnit(m, rows)) return false;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) R[r * 3 + c] = rows[r][c];
    ikQuat(R, q);
    const double s = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    *deg = 2.0 * std::atan2(s, q[3]) * 180.0 / kIkPi;
    if (s > 1e-9 && *deg > 1e-4)
        for (int i = 0; i < 3; ++i) axis[i] = q[i] / s;
    return std::isfinite(*deg);
}
// The wrist after a solve: X (3x3, row-vector) is the hand's rotation beyond the forearm that carried it; swing-twist about the forearm axis eT (unit), the
// swing split along the elbow's hinge normal n1 (flexion) and eT x n1 (deviation). All degrees; a rotation about a bone's own axis is twist.
struct WristAngles {
    bool ok = false;
    double flexDeg = 0.0, devDeg = 0.0, twistDeg = 0.0, swingDeg = 0.0;
};
inline WristAngles wristAngles(const double X[9], const double eT[3], const double n1[3]) noexcept {
    WristAngles w;
    double q[4], a[3], f[3], d[3];
    if (!ikUnit(eT, a) || !ikUnit(n1, f)) return w;
    ikCross(a, f, d);
    if (!ikUnit(d, d)) return w;
    ikQuat(X, q);
    const double proj = q[0] * a[0] + q[1] * a[1] + q[2] * a[2];
    const double tq[4] = {proj * a[0], proj * a[1], proj * a[2], q[3]};   // the twist (unnormalised): its xyz along a
    const double tl = std::sqrt(tq[0] * tq[0] + tq[1] * tq[1] + tq[2] * tq[2] + tq[3] * tq[3]);
    if (!(tl > 1e-12)) return w;
    const double t[4] = {tq[0] / tl, tq[1] / tl, tq[2] / tl, tq[3] / tl};
    // swing = q x conj(t) (Hamilton product, xyzw)
    const double c[4] = {-t[0], -t[1], -t[2], t[3]};
    const double sw[4] = {q[3] * c[0] + q[0] * c[3] + q[1] * c[2] - q[2] * c[1], q[3] * c[1] + q[1] * c[3] + q[2] * c[0] - q[0] * c[2],
                          q[3] * c[2] + q[2] * c[3] + q[0] * c[1] - q[1] * c[0], q[3] * c[3] - q[0] * c[0] - q[1] * c[1] - q[2] * c[2]};
    const double ss = std::sqrt(sw[0] * sw[0] + sw[1] * sw[1] + sw[2] * sw[2]);
    const double swing = 2.0 * std::atan2(ss, sw[3] < 0.0 ? -sw[3] : sw[3]);   // 0..pi
    const double sg = sw[3] < 0.0 ? -1.0 : 1.0;
    const double kDeg = 180.0 / kIkPi;
    w.twistDeg = ikWrapPi(2.0 * std::atan2(proj, q[3])) * kDeg;
    w.swingDeg = swing * kDeg;
    if (ss > 1e-9) {
        const double sa[3] = {sg * sw[0] / ss, sg * sw[1] / ss, sg * sw[2] / ss};
        w.flexDeg = swing * ikDot(sa, f) * kDeg;
        w.devDeg = swing * ikDot(sa, d) * kDeg;
    }
    w.ok = std::isfinite(w.twistDeg) && std::isfinite(w.swingDeg);
    return w;
}
// The grip instrument's steady window, one per item (the held item's serial) and side: the FP attach (FP root frame, metres) still (under
// kHandsStillMps) for kGripStillSec, the separation between the two FP attaches collected over that window. step() is true once, on the sample that
// completes the window.
constexpr double kGripStillSec = 0.5;
struct GripSteady {
    bool done = false;
    uint32_t item = 0;
    bool havePrev = false;
    double prev[3] = {};
    double stillSec = 0.0, lastSpeed = -1.0;   // lastSpeed: the FP attach's speed at the last sample (m/s; -1 none yet)
    uint32_t n = 0;
    double sepSum = 0.0, sepSq = 0.0;
    void reset(uint32_t itemSerial) noexcept {
        *this = GripSteady{};
        item = itemSerial;
    }
    void clearWindow() noexcept {
        stillSec = 0.0;
        n = 0;
        sepSum = sepSq = 0.0;
    }
    bool step(uint32_t itemSerial, bool ok, double dt, const double attach[3], double sep) noexcept {
        if (itemSerial != item) reset(itemSerial);
        if (done) return false;
        if (!ok) {
            havePrev = false;
            clearWindow();
            return false;
        }
        const double d = !(dt > 0.0) ? 0.0 : dt > 0.1 ? 0.1 : dt;
        double speed = 1e9;
        if (havePrev && d > 1e-4) {
            const double v[3] = {attach[0] - prev[0], attach[1] - prev[1], attach[2] - prev[2]};
            speed = ikLen(v) / d;
        }
        for (int i = 0; i < 3; ++i) prev[i] = attach[i];
        havePrev = true;
        lastSpeed = speed < 1e8 ? speed : -1.0;
        if (!(speed < kHandsStillMps)) {
            clearWindow();
            return false;
        }
        stillSec += d;
        ++n;
        sepSum += sep;
        sepSq += sep * sep;
        if (stillSec >= kGripStillSec - 1e-9) {
            done = true;
            return true;
        }
        return false;
    }
    double sepMean() const noexcept { return n ? sepSum / static_cast<double>(n) : 0.0; }
    double sepSd() const noexcept {
        if (n < 2) return 0.0;
        const double m = sepSum / static_cast<double>(n), v = sepSq / static_cast<double>(n) - m * m;
        return v > 0.0 ? std::sqrt(v) : 0.0;
    }
};

// ---- F69h: the off hand on one-handed items follows the game's animation ----------------------------------------------------------------------
// F67f locked BOTH hands to the grips while armed. F68's flight (dumps 152353 to 152442): the pistol's left hand is open, holding nothing; the Energylink's
// left twist is 171.9 deg raw (clamped at +-90 on 82 of 82 calls) with the hand hovering off the handle; the Arc Cutter's left hand rings empty space; the
// Profile Analyser's left fist is crossed under the right arm. The FP rig's left hand is not on those items, so there is no grip to lock to. Sean: the
// 3P off hand follows the game's own 3P animation. Acquisition is measured from unique P1 frames: the FP left handAttach (52) stays within
// kOffHandMaxM (40 cm; the rifle's pair is 32.6 cm) of the right one (51), and its orientation relative to 51 stays under kOffHandSteadyDeg
// across kOffHandSteadyFrames prior samples. Once acquired, valid same-identity attaches retain authority through orientation motion; the off-arm
// IK weight ramps on release over kOffHandRampSec.
constexpr double kOffHandMaxM = 0.40, kOffHandSteadyDeg = 5.0, kOffHandRampSec = 0.15;
// F182c: an input that is invalid only because the FP snapshot is stale (older than kIkPoseDeviationMaxGapMs, in the game's stalls) keeps the decision and the history for up to this much hook
// time (a call counts at most 0.1 s), instead of releasing the hand and needing kOffHandSteadyFrames + 1 fresh frames to take it back. See OffHand::step.
constexpr double kOffHandStaleKeepSec = 3.0;
// F84's same-pass rifle quality activation. Identity/generation changes hard-reset;
// loss of an otherwise known source releases smoothly. Repeated or old frames never
// advance the ramp, so draw-follow handoff cannot pop even when contact was already set.
struct RifleQualityRamp {
    bool initialized = false, epochKnown = false;
    uint32_t item = 0, planGeneration = 0, fpGeneration = 0, lastFrame = 0xFFFFFFFFu;
    uint64_t epoch = 0;
    double weight = 0.0;
    void reset() noexcept { *this = RifleQualityRamp{}; }
    double step(bool identityKnown, uint32_t itemSerial, bool nativeEpochKnown, uint64_t nativeEpoch,
                uint32_t planGen, uint32_t fpGen, uint32_t frame, double dtSec, double target) noexcept {
        if (!identityKnown) { reset(); return 0.0; }
        const bool identityChanged = !initialized || item != itemSerial || planGeneration != planGen || fpGeneration != fpGen ||
                                    (epochKnown && nativeEpochKnown && epoch != nativeEpoch);
        if (identityChanged) {
            weight = 0.0;
            item = itemSerial;
            planGeneration = planGen;
            fpGeneration = fpGen;
            epoch = nativeEpoch;
            epochKnown = nativeEpochKnown;
            lastFrame = frame;
            initialized = true;
            return weight;
        }
        if (!epochKnown && nativeEpochKnown) { epoch = nativeEpoch; epochKnown = true; }
        if (frame == 0xFFFFFFFFu) return weight;
        if (lastFrame == 0xFFFFFFFFu) { lastFrame = frame; return weight; }
        const uint32_t frameDelta = frame - lastFrame;
        if (frameDelta == 0 || frameDelta >= 0x80000000u) return weight;
        lastFrame = frame;
        if (!(dtSec > 0.0) || !std::isfinite(dtSec)) return weight;
        const double amount = std::max(0.0, std::min(1.0, dtSec / kOffHandRampSec));
        weight = ikLerp(weight, unit01(target), amount);
        return weight;
    }
};
constexpr uint32_t kOffHandSteadyFrames = 10;
constexpr double kOffReachRelaxM = 1.0;   // the reach rule's radius for a free off hand's shoulder grows by (1 - weight) x this: at weight 0 it binds nothing
inline double ikOffHandReachRadius(double armLength, double offHandWeight) noexcept {
    const double w = std::isfinite(offHandWeight) ? std::max(0.0, std::min(1.0, offHandWeight)) : 0.0;
    return armLength - kReachMarginM + (1.0 - w) * kOffReachRelaxM;
}
struct OffHand {
    double hist[kOffHandSteadyFrames + 1][9] = {};   // the last unique P1 frames' 52-relative-to-51 rotations (row-vector 3x3), newest at pos
    uint32_t have = 0, pos = 0;
    double w = 0.0;
    uint32_t lastFrame = 0, item = 0, rigGeneration = 0, fpGeneration = 0, syntheticFrame = 0;
    uint64_t fpRig = 0, nativeEpoch = 0;
    bool haveFrame = false, haveIdentity = false, attached = false, releasing = false, lastSteady = false, lastQualified = false;
    double staleSec = 0.0;   // F182c: the hook time of the stale stretch the decision has been kept through
    struct Out {
        bool two = false, ok = false, historyReady = false, withinDistance = false, steady = false, allowed = false, qualified = false;
        bool duplicate = false, gapReset = false, identityReset = false, reversed = false, stablePolicy = false;
        bool kept = false, staleLimit = false;   // F182c: the decision was kept through a stale input this call (ok is false); the stale stretch hit kOffHandStaleKeepSec and the hand is released
        uint8_t historySamples = 0;
        double w = 0.0, dist = 0.0, changeDeg = 0.0;
    };
    // The angle (degrees) between two row-vector rotations a, b (3x3): acos((tr(a b^T) - 1) / 2).
    static double angleDeg(const double a[9], const double b[9]) noexcept {
        double tr = 0.0;
        for (int i = 0; i < 9; ++i) tr += a[i] * b[i];
        double c = (tr - 1.0) * 0.5;
        c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
        return std::acos(c) * 180.0 / kIkPi;
    }
    void clearHistory() noexcept {
        have = pos = 0;
        lastSteady = lastQualified = false;
    }
    // The relative-angle test acquires a two-hand attachment. Once acquired, current readable attaches and separation keep it until the
    // explicit one-hand policy, separation limit, disarm or identity change releases it. History advances only on unique P1 frames.
    Out step(double dt, bool armed, bool ok, const float a51[16], const float a52[16], bool allowTwoHand = true,
             uint32_t frame = 0xFFFFFFFFu, uint32_t itemId = 0, uint32_t rigGen = 0, uint64_t fpRigId = 0, uint32_t fpGen = 0,
             uint64_t acceptedNativeEpoch = 0, bool sourceSuspended = false, bool staleOnly = false) noexcept {
        // staleOnly (F182c): ok is false only because the FP snapshot is older than the gap (the pair, the generation and the rig are otherwise readable). An attached hand keeps its decision and its history
        // through such calls, for up to kOffHandStaleKeepSec of hook time; a fresh call resumes the rule below from the kept history, and a pair that really changed (past kOffHandMaxM, the explicit
        // one-hand policy, an identity change) releases as it always did once fresh frames show it.
        Out o;
        if (!armed) {
            if (sourceSuspended && acceptedNativeEpoch != 0 && haveIdentity && nativeEpoch == acceptedNativeEpoch &&
                rigGeneration == rigGen && fpRig == fpRigId && fpGeneration == fpGen) {
                o.two = attached;
                o.w = w;
                o.stablePolicy = attached || (releasing && w > 0.0);
                o.historySamples = static_cast<uint8_t>(have);
                return o;
            }
            *this = OffHand{};
            return o;
        }
        bool keyedFrame = frame != 0xFFFFFFFFu;
        if (!keyedFrame) frame = ++syntheticFrame;
        const bool nativeIdentity = acceptedNativeEpoch != 0 || nativeEpoch != 0;
        const bool identityChanged = haveIdentity && ((nativeIdentity ? nativeEpoch != acceptedNativeEpoch : item != itemId) || rigGeneration != rigGen || fpRig != fpRigId || fpGeneration != fpGen);
        if (!haveIdentity || identityChanged) {
            if (haveIdentity) o.identityReset = true;
            clearHistory();
            attached = releasing = false;
            w = 0.0;
            haveFrame = false;
            item = itemId; rigGeneration = rigGen; fpRig = fpRigId; fpGeneration = fpGen; nativeEpoch = acceptedNativeEpoch; haveIdentity = true;
        }
        item = itemId;
        if (sourceSuspended) {
            o.two = attached;
            o.ok = false;
            o.w = w;
            o.stablePolicy = attached || (releasing && w > 0.0);
            o.historySamples = static_cast<uint8_t>(have);
            return o;
        }
        bool uniqueFrame = true;
        if (haveFrame) {
            const uint32_t delta = frame - lastFrame;
            if (delta == 0) {
                uniqueFrame = false;
                o.duplicate = true;
            } else if (delta >= 0x80000000u) {
                uniqueFrame = false;
                o.reversed = true;
                clearHistory();
                if (attached) releasing = true;
                attached = false;
                haveFrame = false;
            } else if (delta != 1) {
                o.gapReset = true;
                clearHistory();
            }
        }
        if (uniqueFrame) { lastFrame = frame; haveFrame = true; }
        const double d = !uniqueFrame || !(dt > 0.0) ? 0.0 : dt > 0.1 ? 0.1 : dt;
        bool keepNow = staleOnly && !ok && attached && allowTwoHand;
        if (keepNow) {
            staleSec += d;
            if (staleSec > kOffHandStaleKeepSec) {
                keepNow = false;   // the stretch is too long to trust the decision: released below, as an unreadable input releases
                o.staleLimit = true;
            }
        } else if (ok || !attached) {
            staleSec = 0.0;
        }
        if (!allowTwoHand || (!ok && !keepNow)) clearHistory();
        o.allowed = allowTwoHand;
        bool steadyNow = false, closeEnough = false;
        if (ok) {
            double r51[3][3], r52[3][3];
            if (matRowsUnit(a51, r51) && matRowsUnit(a52, r52)) {
                // R_rel = R52 x R51^T (row-vector): the left attach's orientation in the right attach's axes.
                double rel[9];
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) rel[r * 3 + c] = r52[r][0] * r51[c][0] + r52[r][1] * r51[c][1] + r52[r][2] * r51[c][2];
                const double dx[3] = {static_cast<double>(a52[12]) - a51[12], static_cast<double>(a52[13]) - a51[13], static_cast<double>(a52[14]) - a51[14]};
                o.dist = ikLen(dx);
                o.ok = std::isfinite(o.dist);
                closeEnough = o.ok && o.dist <= kOffHandMaxM;
                if (uniqueFrame && allowTwoHand && o.ok && closeEnough) {
                    pos = (pos + 1) % (kOffHandSteadyFrames + 1);
                    for (int i = 0; i < 9; ++i) hist[pos][i] = rel[i];
                    if (have < kOffHandSteadyFrames + 1) ++have;
                }
                double mx = 0.0;
                if (have >= kOffHandSteadyFrames + 1) {
                    for (uint32_t k = 1; k <= kOffHandSteadyFrames; ++k) {
                        const uint32_t i = (pos + kOffHandSteadyFrames + 1 - k) % (kOffHandSteadyFrames + 1);
                        const double a = angleDeg(rel, hist[i]);
                        mx = a > mx ? a : mx;
                    }
                    steadyNow = true;
                }
                o.changeDeg = mx;
                steadyNow = steadyNow && mx < kOffHandSteadyDeg;
                o.historyReady = have >= kOffHandSteadyFrames + 1;
                o.withinDistance = closeEnough;
                o.steady = steadyNow;
                o.qualified = allowTwoHand && o.ok && closeEnough && uniqueFrame && steadyNow;
                if (uniqueFrame) {
                    lastSteady = steadyNow;
                    lastQualified = o.qualified;
                } else {
                    o.steady = lastSteady;
                    o.qualified = lastQualified;
                }
            }
        }
        // Invalid inputs, explicit free-hand policy and excess separation release and forget acquisition. The 5-degree check only decides acquisition.
        if (keepNow) {
            o.kept = true;   // F182c: neither released nor re-decided: the decision, the history and the weight's target stand
        } else if (!o.ok || !allowTwoHand || !closeEnough) {
            if (attached) releasing = true;
            attached = false;
            clearHistory();
        } else if (!attached && o.qualified) {
            attached = true;
            releasing = false;
        }
        o.two = attached;
        const double target = attached ? 1.0 : 0.0, step = d / kOffHandRampSec;
        w = w < target ? (w + step > target ? target : w + step) : (w - step < target ? target : w - step);
        if (!attached && w <= 0.0) releasing = false;
        o.stablePolicy = attached || (releasing && w > 0.0);
        o.historySamples = static_cast<uint8_t>(have);
        o.w = w;
        return o;
    }
};
enum SupportItem : uint8_t { kSupportNone = 0, kSupportPistol = 1, kSupportTool = 2 };
inline bool authoredPistolSupportEligible(bool armed, SupportItem item, uint32_t itemSerial, uint32_t profileSerial,
                                          bool profileIsPistol, bool attachPairValid, bool withinDistance,
                                          bool anatomyMapped, bool leftTargetValid, uint8_t leftTargetMode) noexcept {
    return armed && item == kSupportPistol && itemSerial == profileSerial && profileIsPistol && attachPairValid && withinDistance && anatomyMapped &&
           leftTargetValid && leftTargetMode == 1;
}
inline SupportItem supportItem(const char* name) noexcept {
    if (!name || !*name) return kSupportNone;
    constexpr char pistol[] = "$wpn_s_pistol_";
    if (std::strncmp(name, pistol, sizeof(pistol) - 1) == 0) return kSupportPistol;
    // Arc Cutter keeps the game's authored left-hand handle grasp; the support-cup target is for pistols only.
    return kSupportNone;
}
// F74: the armed-ramp is the acquisition envelope. Once a requested cup target is valid it is selected in full;
// a still-valid target that is being released retains the existing short blend-out, while invalid geometry clears immediately.
inline double supportCupBlendStep(double current, bool requested, bool targetValid, double step) noexcept {
    if (!targetValid || !std::isfinite(current)) return 0.0;
    if (requested) return 1.0;
    const double s = std::isfinite(step) && step > 0.0 ? step : 0.0;
    const double w = current - s;
    return w > 0.0 ? w : 0.0;
}
inline bool gameAuthoredLeftArm(const char* name) noexcept {
    return name && (std::strcmp(name, "$humanoid_rechargetool_name;") == 0 || std::strcmp(name, "$humanoid_companalyser_name;") == 0);
}

// Anatomical hand transfer primitives. IDs are interned joint names from the
// two rigs; a match is accepted only when it is unique and its parent also
// maps to the same named joint. This deliberately refuses ambiguous fingers.
enum HandMapWhy : uint8_t { kHandMapOk = 0, kHandMapBadInput, kHandMapDuplicate, kHandMapNoMatches, kHandMapTopology, kHandMapPlan, kHandMapSnapshot };
inline const char* handMapWhyText(HandMapWhy why) noexcept {
    switch (why) {
        case kHandMapOk: return "mapped";
        case kHandMapBadInput: return "bad input";
        case kHandMapDuplicate: return "duplicate name IDs";
        case kHandMapNoMatches: return "missing name IDs";
        case kHandMapTopology: return "parent mismatch";
        case kHandMapPlan: return "anatomy plan refused";
        case kHandMapSnapshot: return "optional FP finger snapshot unreadable";
        default: return "unknown refusal";
    }
}
struct HandJointMap {
    HandMapWhy why = kHandMapBadInput;
    uint16_t count = 0;
    uint16_t dst[kPfzMaxJoints] = {};
    bool ok() const noexcept { return why == kHandMapOk && count != 0; }
};
inline HandJointMap matchHandJoints(const uint16_t* srcName, const uint16_t* srcParent, uint32_t srcN, const uint16_t* dstName, const uint16_t* dstParent,
                                    uint32_t dstN) noexcept {
    HandJointMap o;
    if (!srcName || !srcParent || !dstName || !dstParent || srcN == 0 || dstN == 0 || srcN > kPfzMaxJoints || dstN > kPfzMaxJoints) return o;
    o.why = kHandMapNoMatches;
    for (uint32_t i = 0; i < srcN; ++i) {
        o.dst[i] = 0xFFFFu;
        if (srcName[i] == 0) continue;
        uint32_t sc = 0, dc = 0, di = 0;
        for (uint32_t j = 0; j < srcN; ++j) sc += srcName[j] == srcName[i] ? 1u : 0u;
        for (uint32_t j = 0; j < dstN; ++j) if (dstName[j] == srcName[i]) { ++dc; di = j; }
        if (sc > 1 || dc > 1) { o.why = kHandMapDuplicate; o.count = 0; return o; }
        if (dc != 1) continue;
        const uint16_t sp = srcParent[i], dp = dstParent[di];
        if (sp == 0xFFFFu || dp == 0xFFFFu) {
            if (sp != dp) { o.why = kHandMapTopology; o.count = 0; return o; }
        } else if (sp >= srcN || dp >= dstN || srcName[sp] == 0 || srcName[sp] != dstName[dp]) {
            o.why = kHandMapTopology; o.count = 0; return o;
        }
        o.dst[i] = static_cast<uint16_t>(di);
        ++o.count;
    }
    if (o.count == srcN) o.why = kHandMapOk;
    else if (o.count) { o.why = kHandMapNoMatches; o.count = 0; }
    return o;
}
// Transfer the pose through each rig's palm basis. All matrices use the
// project's row-vector convention: inv(B3) * Bfp * Hfp * G.
inline bool handRetarget(const float B3[16], const float Bfp[16], const float Hfp[16], const float G[16], float out[16]) noexcept {
    if (!B3 || !Bfp || !Hfp || !G || !out) return false;
    float invB3[16], t0[16], t1[16], t2[16];
    if (!pfzAffineInverse(B3, invB3)) return false;
    pfzMul(invB3, Bfp, t0);
    pfzMul(t0, Hfp, t1);
    pfzMul(t1, G, t2);
    if (!ikOrthonormalise(t2)) return false;
    std::memcpy(out, t2, sizeof(t2));
    return true;
}
inline bool handBlendRigid(const float from[16], const float to[16], double weight, float out[16]) noexcept {
    if (!from || !to || !out || !std::isfinite(weight)) return false;
    const float w = static_cast<float>(weight < 0.0 ? 0.0 : weight > 1.0 ? 1.0 : weight);
    float qa[4], qb[4], q[4], rows[9];
    pfzQuatFromMatrix(from, qa); pfzQuatFromMatrix(to, qb);
    const float dot = qa[0]*qb[0] + qa[1]*qb[1] + qa[2]*qb[2] + qa[3]*qb[3];
    const float sign = dot < 0.0f ? -1.0f : 1.0f;
    float norm = 0.0f;
    for (int i = 0; i < 4; ++i) { q[i] = (1.0f-w)*qa[i] + w*sign*qb[i]; norm += q[i]*q[i]; }
    if (!(norm > 1e-12f) || !std::isfinite(norm)) return false;
    norm = 1.0f / std::sqrt(norm);
    for (int i = 0; i < 4; ++i) q[i] *= norm;
    avh::quatRows(q, rows);
    pfzIdentity(out);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r*4+c] = rows[r*3+c];
    for (int c = 0; c < 3; ++c) out[12+c] = static_cast<float>((1.0-w)*from[12+c] + w*to[12+c]);
    return ikOrthonormalise(out);
}
inline bool handFingerLocalRetarget(const float Qsrc[16], const float Lsrc[16], const float QsrcParent[16], const float Qdst[16], const float QdstParent[16],
                                    const float targetTranslation[3], bool mirrorPalmNormal, float out[16]) noexcept {
    if (!Qsrc || !Lsrc || !QsrcParent || !Qdst || !QdstParent || !targetTranslation || !out) return false;
    float invQdstParent[16], t0[16], delta[16];
    if (!pfzAffineInverse(QdstParent, invQdstParent)) return false;
    if (mirrorPalmNormal) {
        float invQsrc[16], F[16], t1[16], reflected[16];
        if (!pfzAffineInverse(Qsrc, invQsrc)) return false;
        pfzMul(invQsrc, Lsrc, t0);
        pfzMul(t0, QsrcParent, delta);
        pfzIdentity(F); F[10] = -1.0f;   // F D F reflects curl into the opposite physical palm hemisphere
        pfzMul(F, delta, t1);
        pfzMul(t1, F, reflected);
        std::memcpy(delta, reflected, sizeof(delta));
        pfzMul(Qdst, delta, t0);
        pfzMul(t0, invQdstParent, out);
    } else {
        float C[16], invC[16], Cp[16], invQdstP[16], t1[16];
        if (!pfzAffineInverse(Qdst, invQdstP)) return false;
        pfzMul(Qsrc, invQdstP, C);
        if (!pfzAffineInverse(C, invC)) return false;
        pfzMul(QsrcParent, invQdstParent, Cp);
        pfzMul(invC, Lsrc, t1);
        pfzMul(t1, Cp, out);
    }
    for (int c = 0; c < 3; ++c) out[12 + c] = targetTranslation[c];
    return ikOrthonormalise(out);
}

struct PalmPatch {
    double heel[3] = {}, thumb[3] = {}, normal[3] = {};
    double halfWidth = 0.0, halfLength = 0.0, halfThickness = 0.0;
    bool ok = false;
};
struct SupportPalmTarget {
    double centre[3] = {}, facing[3] = {};
    double gap = 0.0;
    bool ok = false;
};
// F75: the anatomical contact proxy uses the second finger joints (K2), independently of the palm frame used by hand retargeting.
struct SupportContactGeometry {
    double centre[3] = {}, distal[3] = {}, radial[3] = {}, normal[3] = {};
    double width = 0.0, length = 0.0, radius = 0.0;
    bool ok = false;
};
inline SupportContactGeometry supportContactFromK2(const double wrist[3], const double thumbRoot[3], const double k2[4][3]) noexcept {
    SupportContactGeometry o;
    double mean[3] = {};
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(k2[j][i])) return o;
            mean[i] += k2[j][i] * 0.25;
        }
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(wrist[i]) || !std::isfinite(thumbRoot[i])) return o;
    double d[3] = {mean[0] - wrist[0], mean[1] - wrist[1], mean[2] - wrist[2]};
    double r[3] = {thumbRoot[0] - mean[0], thumbRoot[1] - mean[1], thumbRoot[2] - mean[2]};
    if (!ikUnit(d, d)) return o;
    const double along = ikDot(r, d);
    for (int i = 0; i < 3; ++i) r[i] -= along * d[i];
    if (!ikUnit(r, r)) return o;
    double n[3];
    ikCross(d, r, n);
    if (!ikUnit(n, n)) return o;
    double coord[4];
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (int j = 0; j < 4; ++j) {
        const double v[3] = {k2[j][0] - mean[0], k2[j][1] - mean[1], k2[j][2] - mean[2]};
        coord[j] = ikDot(v, r);
        lo = std::min(lo, coord[j]);
        hi = std::max(hi, coord[j]);
    }
    std::sort(coord, coord + 4);
    double spacing[3] = {coord[1] - coord[0], coord[2] - coord[1], coord[3] - coord[2]};
    std::sort(spacing, spacing + 3);
    o.width = hi - lo;
    const double wristToK2[3] = {mean[0] - wrist[0], mean[1] - wrist[1], mean[2] - wrist[2]};
    o.length = ikLen(wristToK2);
    o.radius = spacing[1] * 0.5;
    for (int i = 0; i < 3; ++i) {
        o.centre[i] = (wrist[i] + mean[i]) * 0.5;
        o.distal[i] = d[i];
        o.radial[i] = r[i];
        o.normal[i] = n[i];
    }
    o.ok = std::isfinite(o.width) && std::isfinite(o.length) && std::isfinite(o.radius) && o.width > 0.015 && o.width < 0.20 &&
           o.length > 0.015 && o.length < 0.25 && o.radius > 0.001 && o.radius < 0.04;
    return o;
}
// `normal` is the right hand's thumbward radial direction. The underside is
// its negative; place the left palm below the heel and outside the proxy.
// gap is explicit so callers and logs can verify non-penetration.
inline SupportPalmTarget cupUnderHeel(const PalmPatch& right, const double leftHalfThickness, const double gap) noexcept {
    SupportPalmTarget o;
    if (!right.ok || !(leftHalfThickness >= 0.0) || !(gap >= 0.0) || !std::isfinite(leftHalfThickness) || !std::isfinite(gap)) return o;
    double n[3] = {right.normal[0], right.normal[1], right.normal[2]};
    if (!ikUnit(n, n)) return o;
    const double sep = right.halfThickness + leftHalfThickness + gap;
    if (!(right.halfThickness >= 0.0) || !std::isfinite(sep)) return o;
    for (int i = 0; i < 3; ++i) {
        o.centre[i] = right.heel[i] - n[i] * sep;
        o.facing[i] = right.thumb[i];
    }
    if (!ikUnit(o.facing, o.facing)) return SupportPalmTarget{};
    o.gap = gap;
    o.ok = true;
    return o;
}
inline bool supportPalmFrame(const float rightPalm[16], int rightPalmarSign, int leftPalmarSign, const SupportPalmTarget& contact, float out[16]) noexcept {
    if (!rightPalm || !out || !contact.ok || (rightPalmarSign != -1 && rightPalmarSign != 1) || (leftPalmarSign != -1 && leftPalmarSign != 1)) return false;
    double radial[3] = {rightPalm[4], rightPalm[5], rightPalm[6]}, physicalNormal[3] = {};
    if (!ikUnit(radial, radial)) return false;
    for (int i = 0; i < 3; ++i) physicalNormal[i] = rightPalmarSign * static_cast<double>(rightPalm[8 + i]);
    double distal[3] = {-physicalNormal[0], -physicalNormal[1], -physicalNormal[2]};
    const double radialPart = ikDot(distal, radial);
    for (int i = 0; i < 3; ++i) distal[i] -= radialPart * radial[i];
    if (!ikUnit(distal, distal)) return false;
    double normal[3] = {leftPalmarSign * radial[0], leftPalmarSign * radial[1], leftPalmarSign * radial[2]}, side[3];
    ikCross(normal, distal, side);
    if (!ikUnit(side, side)) return false;
    pfzIdentity(out);
    for (int i = 0; i < 3; ++i) {
        out[i] = static_cast<float>(distal[i]); out[4 + i] = static_cast<float>(side[i]); out[8 + i] = static_cast<float>(normal[i]);
        out[12 + i] = static_cast<float>(contact.centre[i]);
    }
    return ikOrthonormalise(out);
}
// Select the measured weapon-side edge of the two-hand support contact. The right attachment identifies which radial side faces the weapon;
// this keeps the contact side stable across model/weapon basis conventions and rigid transforms.
inline bool supportWeaponSideContact(const float rightContact[16], const float rightAttach[16], double length, double width, double rightRadius,
                                    double leftRadius, double gap, int* radialSide, SupportPalmTarget& contact, double* attachSideM = nullptr) noexcept {
    contact = SupportPalmTarget{};
    if (!rightContact || !rightAttach || !radialSide || !(length > 0.0) || !(width > 0.0) || !(rightRadius >= 0.0) || !(leftRadius >= 0.0) ||
        !(gap >= 0.0) || !std::isfinite(length) || !std::isfinite(width) || !std::isfinite(rightRadius) || !std::isfinite(leftRadius) || !std::isfinite(gap)) return false;
    double radial[3] = {rightContact[4], rightContact[5], rightContact[6]};
    double distal[3] = {rightContact[0], rightContact[1], rightContact[2]};
    if (!ikUnit(radial, radial) || !ikUnit(distal, distal)) return false;
    const double k2[3] = {rightContact[12] + distal[0] * length * 0.5, rightContact[13] + distal[1] * length * 0.5,
                          rightContact[14] + distal[2] * length * 0.5};
    const double attachFromK2[3] = {rightAttach[12] - k2[0], rightAttach[13] - k2[1], rightAttach[14] - k2[2]};
    const double signedSide = ikDot(attachFromK2, radial);
    if (!std::isfinite(signedSide) || std::fabs(signedSide) < 1e-6) return false;
    const int side = signedSide > 0.0 ? 1 : -1;
    PalmPatch patch;
    for (int i = 0; i < 3; ++i) {
        patch.heel[i] = k2[i] + side * radial[i] * width * 0.5;
        patch.thumb[i] = -side * radial[i];
        patch.normal[i] = -side * radial[i]; // cupUnderHeel subtracts this; negative normal moves the palm toward the weapon-side edge.
    }
    patch.halfThickness = rightRadius;
    patch.ok = true;
    contact = cupUnderHeel(patch, leftRadius, gap);
    if (!contact.ok) return false;
    *radialSide = side;
    if (attachSideM) *attachSideM = signedSide;
    return true;
}
inline bool supportPalmFrame(const float rightPalm[16], int rightPalmarSign, int leftPalmarSign, int radialSide, const SupportPalmTarget& contact,
                             float out[16]) noexcept {
    if (!rightPalm || !out || !contact.ok || (rightPalmarSign != -1 && rightPalmarSign != 1) || (leftPalmarSign != -1 && leftPalmarSign != 1) ||
        (radialSide != -1 && radialSide != 1)) return false;
    double radial[3] = {radialSide * static_cast<double>(rightPalm[4]), radialSide * static_cast<double>(rightPalm[5]),
                        radialSide * static_cast<double>(rightPalm[6])};
    double physicalNormal[3] = {rightPalmarSign * static_cast<double>(rightPalm[8]), rightPalmarSign * static_cast<double>(rightPalm[9]),
                                rightPalmarSign * static_cast<double>(rightPalm[10])};
    if (!ikUnit(radial, radial) || !ikUnit(physicalNormal, physicalNormal)) return false;
    double distal[3] = {-physicalNormal[0], -physicalNormal[1], -physicalNormal[2]};
    const double radialPart = ikDot(distal, radial);
    for (int i = 0; i < 3; ++i) distal[i] -= radialPart * radial[i];
    if (!ikUnit(distal, distal)) return false;
    double normal[3] = {-leftPalmarSign * radial[0], -leftPalmarSign * radial[1], -leftPalmarSign * radial[2]}, side[3];
    ikCross(normal, distal, side);
    if (!ikUnit(side, side)) return false;
    pfzIdentity(out);
    for (int i = 0; i < 3; ++i) {
        out[i] = static_cast<float>(distal[i]); out[4 + i] = static_cast<float>(side[i]); out[8 + i] = static_cast<float>(normal[i]);
        out[12 + i] = static_cast<float>(contact.centre[i]);
    }
    return ikOrthonormalise(out);
}

}  // namespace nofoot
}  // namespace edvr
