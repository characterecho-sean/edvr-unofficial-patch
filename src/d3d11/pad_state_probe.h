#pragma once
// The pad-state probe (TEMPORARY, 2026-10-13): four detours on Elite's docking-state readers, build 332841. Installed when fix.optimized_station is
// on (there is no key and no hotkey). Built v14.23: the probe is the station's trigger input: the game's pad state (the own pad's r0 and r1)
// decides the seal. The detours still never call Elite and never log.
#include <cstdint>

namespace edvr {

// The setting, as applied (applySetting): on installs the readers once (identity first, then each prologue) and opens the observers; off closes them.
void padStateSetOn(bool on) noexcept;
// The station's frame tick (setting on): the change lines and the seal or release line for the OWN pad only (own 0: none), and the 30 s summary.
void padStateTick(uint64_t nowMs, uintptr_t own) noexcept;
// A seal or release happened ("seal" or "release"); the next tick writes the line for the own pad.
void padStateEvent(const char* what) noexcept;

// One object's readers, as the probe last saw them. r0 and r1 are -1 when the reader was never called on the object; the QPC times are 0 then.
struct PadObjState {
    bool have = false;
    uintptr_t self = 0;
    int r0 = -1;
    uint64_t r0Qpc = 0;
    int r1 = -1;
    uint64_t r1Qpc = 0;
};
// The latest r0 call on any object (the station's own pad is the one r0 was last called on this dock).
void padStateLatestR0(PadObjState* out) noexcept;
// One object's r0 and r1.
void padStateObject(uintptr_t self, PadObjState* out) noexcept;
// Milliseconds since a QPC time read by padStateNowQpc (UINT64_MAX when there is none).
uint64_t padStateAgeMs(uint64_t qpc) noexcept;
uint64_t padStateNowQpc() noexcept;

}  // namespace edvr
