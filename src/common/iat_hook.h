// Patching the game executable's import table, one function at a time.
//
// WHY THIS EXISTS. The settings menu takes the keyboard while it is open
// (docs/settings-menu.md, "The keyboard gate"), and two of the three doors
// Elite reads keys through are plain user32 imports of the EXE:
// GetAsyncKeyState / GetKeyState / GetKeyboardState, and PeekMessageA. The
// smallest hook that reaches exactly those calls and nothing else is the
// import address table of the executable itself: one pointer per function,
// in the EXE's own image, consulted by every call the EXE makes and by no
// call anybody else makes. EDVR's own modules import the same functions
// through their own tables and are untouched, which is what lets the menu
// keep reading the real keyboard while the game is told it is silent.
//
// Not a detour (no code is rewritten, so nothing can half-execute), not a
// system hook (nothing outside this process is touched), and not a
// process-wide export patch (a wrapper mod's own GetAsyncKeyState keeps
// working). All-or-nothing per slot, restored on uninstall only where the
// slot still holds our replacement -- the VTableHook discipline, for the
// same reason: an entry somebody patched after us is theirs now.
#pragma once

#include <cstddef>

namespace edvr {

// One patched import slot: what it was, what it is, and where it lives.
struct IatPatch {
    void** slot = nullptr;       // the IAT entry, inside the EXE's image
    void*  original = nullptr;   // what the slot held before us
    void*  replacement = nullptr;
    bool   applied = false;
};

// Find `function` in `module`'s import descriptor of the process
// executable and swap the slot to `replacement`. `patch` receives the slot
// and the original on success; the original is what the caller forwards
// to -- if another hook patched this slot first, that is what it forwards
// to, and both run. Returns false, changing nothing, when the EXE does not
// import that function from that module, when the page cannot be made
// writable, or when the slot is already ours.
//
// `module` is matched case-insensitively against the import descriptor's
// name ("USER32.dll" in the EXE's table; pass "user32.dll").
bool iatHookInstall(const char* module, const char* function, void* replacement,
                    IatPatch* patch);

// Put the original back, but ONLY where the slot still holds our
// replacement. Safe on a patch that was never applied.
void iatHookUninstall(IatPatch* patch);

// Which module the slot's CURRENT entry points into, for the log: the
// module's file name (e.g. "USER32.dll", "gameoverlayrenderer64.dll") or
// "?" when the address is in no loaded module. `buf` receives it.
void iatHookEntryModule(const void* entry, char* buf, size_t bufLen);

}  // namespace edvr
