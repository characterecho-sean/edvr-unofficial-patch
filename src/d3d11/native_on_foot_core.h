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
// Mode 3 is HMD stereo, mode 5 is Cinema (the flat screen shown in the headset). Elite asks for 5 when the commander disembarks and 3
// when they return aboard.
//
// THE SUBSTITUTION. An incoming 5 is forwarded as 3, once, on the caller's thread, when every gate below admits it. Anything else is
// forwarded unchanged. Nothing here calls the setter itself, writes selector+0x20, or touches the settings words.

#include <cstddef>
#include <cstdint>

#include "explorer_cam_core.h"   // ecm::kExpectedTimestamp and ecm::kExpectedImageSize: build 332841's PE identity, shared with the other build-keyed hooks

namespace edvr {
namespace nofoot {

constexpr uintptr_t kSetterRva = 0x28723C0;          // the render-mode setter's entry (EliteDangerous64.exe+0x28723C0)
constexpr uintptr_t kSetterVtableRva = 0x52E9608;    // the vtable that holds the setter
constexpr size_t kSetterSlotOffset = 0x28;           // slot 5 of that vtable (5 * 8)
constexpr uint32_t kModeHmd = 3;                     // HMD stereo
constexpr uint32_t kModeCinema = 5;                  // the flat cinema screen, shown in the headset
constexpr size_t kSourceModeOffset = 0x20;           // selector+0x20: the source mode the setter writes
constexpr size_t kOwnerOffset = 0x58;                // selector+0x58: the renderer owner
constexpr size_t kOwnerSelectorOffset = 0x60;        // owner+0x60: must be the selector again

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
    NotCinema,        // the incoming mode is not 5: forwarded unchanged (3 always is; so is anything else)
    KeyOff,           // incoming 5, but fix.native_on_foot is off
    SelectorInvalid,  // incoming 5, but the selector's owner does not point back at it: not a renderer this build knows
    SelectorMismatch, // incoming 5 on a selector other than the one frozen at the first valid call
    NoGameSession,    // incoming 5 with the key on, but the journal has not seen LoadGame (or has seen Shutdown): a menu, not a session
    Admitted          // substituted: forwarded as 3
};

struct Inputs {
    bool keyOn = false;            // fix.native_on_foot read as on (published by the frame thread)
    uint32_t incoming = 0;         // the mode the caller asked for
    bool selectorValid = false;    // selector+0x58 -> owner and owner+0x60 == selector, read under SEH
    bool selectorFrozen = false;   // the selector is the one frozen at the first valid call
    bool gameSession = false;      // journalGameplay(): LoadGame seen in this process and no Shutdown since
};

// The admission rule. Pure: the hook supplies the facts it has just read, and the answer is the only thing that substitutes.
inline Reason admit(const Inputs& in) {
    if (in.incoming != kModeCinema) return Reason::NotCinema;
    if (!in.keyOn) return Reason::KeyOff;
    if (!in.selectorValid) return Reason::SelectorInvalid;
    if (!in.selectorFrozen) return Reason::SelectorMismatch;
    if (!in.gameSession) return Reason::NoGameSession;
    return Reason::Admitted;
}

// The mode the original is forwarded with: 3 for an admitted 5, otherwise what was asked.
inline uint32_t effectiveMode(Reason r, uint32_t incoming) {
    return r == Reason::Admitted ? kModeHmd : incoming;
}

inline const char* reasonName(Reason r) {
    switch (r) {
        case Reason::NotCinema: return "not_cinema";
        case Reason::KeyOff: return "key_off";
        case Reason::SelectorInvalid: return "selector_invalid";
        case Reason::SelectorMismatch: return "selector_mismatch";
        case Reason::NoGameSession: return "no_game_session";
        case Reason::Admitted: return "admitted";
    }
    return "unknown";
}

// The identity check's answer for a PE stamp and image size: build 332841 or not.
inline bool identityMatches(uint32_t timestamp, uint32_t imageSize) {
    return timestamp == ecm::kExpectedTimestamp && imageSize == ecm::kExpectedImageSize;
}

}  // namespace nofoot
}  // namespace edvr
