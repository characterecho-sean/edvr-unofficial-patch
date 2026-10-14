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

#include <cstddef>
#include <cstdint>

#include "explorer_cam_core.h"   // ecm::kExpectedTimestamp and ecm::kExpectedImageSize: build 332841's PE identity, shared with the other build-keyed hooks

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
    GateMethod,       // incoming cinema mode, key on, but fix.native_on_foot_method is gate: the setter never substitutes (F2)
    SelectorInvalid,  // incoming cinema mode, but the selector's owner does not point back at it: not a renderer this build knows
    SelectorMismatch, // incoming cinema mode on a selector other than the one frozen at the first valid call
    NoGameSession,    // incoming cinema mode with the key on, but the journal has not seen LoadGame (or has seen Shutdown): a menu, not a session
    Admitted          // substituted: forwarded as its HMD partner (5 as 3, 6 as 4)
};

struct Inputs {
    bool keyOn = false;            // fix.native_on_foot read as on (published by the frame thread)
    bool gateMethod = false;       // fix.native_on_foot_method is gate (F2): the byte does the work, the setter only logs
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
    if (in.gateMethod) return Reason::GateMethod;
    if (!in.selectorValid) return Reason::SelectorInvalid;
    if (!in.selectorFrozen) return Reason::SelectorMismatch;
    if (!in.gameSession) return Reason::NoGameSession;
    return Reason::Admitted;
}

// The mode the original is forwarded with: the HMD partner of an admitted cinema mode (5 -> 3, 6 -> 4), otherwise what was asked.
inline uint32_t effectiveMode(Reason r, uint32_t incoming) {
    if (r != Reason::Admitted) return incoming;
    return incoming == kModeCinemaA ? kModeHmdA : kModeHmdB;
}

inline const char* reasonName(Reason r) {
    switch (r) {
        case Reason::NotCinema: return "not_cinema";
        case Reason::KeyOff: return "key_off";
        case Reason::GateMethod: return "method_gate";
        case Reason::SelectorInvalid: return "selector_invalid";
        case Reason::SelectorMismatch: return "selector_mismatch";
        case Reason::NoGameSession: return "no_game_session";
        case Reason::Admitted: return "admitted";
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

// ---- F6: two read-only eye-index probes (TEMPORARY; they go when the arc closes). native_on_foot.cpp holds the hooks. ----
constexpr size_t kEyeCount = 2;
// E1: the eye-index query. rcx = this, rdx = node; returns eax 0, 1 or 2. Stolen: sub rsp,0x28 (4) + mov rcx,[rcx+0x830] (7) = 11.
constexpr uintptr_t kEyeQueryRva = 0x2842FD0;
constexpr size_t kEyeQueryStolen = 11;
constexpr size_t kEyeQueryPrologueBytes = 25;
constexpr uint8_t kEyeQueryPrologue[kEyeQueryPrologueBytes] = {
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x89, 0x30, 0x08, 0x00, 0x00, 0x8B, 0x81, 0x94, 0x36, 0x00,
    0x00, 0x83, 0xE8, 0x03, 0x83, 0xF8, 0x03, 0x77, 0x31};
// E2: the stereo leaf predicate (no pdata). rcx = this; returns al. Stolen: mov rdx,[rcx+0x38] (4) + mov eax,[rdx+0x3694] (6) = 10.
constexpr uintptr_t kStereoPredRva = 0x28540B0;
constexpr size_t kStereoPredStolen = 10;
constexpr size_t kStereoPredPrologueBytes = 18;
constexpr uint8_t kStereoPredPrologue[kStereoPredPrologueBytes] = {
    0x48, 0x8B, 0x51, 0x38, 0x8B, 0x82, 0x94, 0x36, 0x00, 0x00, 0x83, 0xE8, 0x03, 0x83, 0xF8, 0x03, 0x77, 0x0C};
// Fields. E1: S = [this+0x830]; mode = dword [S+0x3694]; W = [S+0x2CB8]; slot0 = [W+0x90], slot1 = [W+0x98]; node = [slot+0x48].
// E2: S = [this+0x38]; mode = dword [S+0x3694]; flag = byte [S+0x2AE0].
constexpr uint32_t kEyeSceneOff = 0x830, kEyeModeOff = 0x3694, kEyeWOff = 0x2CB8, kEyeSlot0Off = 0x90, kEyeSlot1Off = 0x98,
                   kEyeNodeOff = 0x48, kStereoSceneOff = 0x38, kStereoFlagOff = 0x2AE0;

}  // namespace nofoot
}  // namespace edvr
