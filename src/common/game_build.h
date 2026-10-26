// Which game build is running, and where the addresses EDVR binds are in it (issue 124; docs\design-game-update-signatures-2026-10-25.md).
//
// The hooks bind fixed RVAs of one build, the known build (the generated table src\common\game_signatures.inc names it). On the known
// build nothing here does any work beyond reading the PE identity once: gameRva returns its argument unchanged and the resolver never
// runs. On any other build of the game every table address is found at once (from gameBuildStartup, on a thread of its own), by
// whole-function masked signature (game_build_core.h), from the running image's .pdata and code in memory, every read under SEH; an
// address that is not found uniquely is 0 and the hook that needs it stands down as it does today. A process whose main module is not
// EliteDangerous64.exe (the installer, a tool that loaded the proxy) is not the game: nothing is resolved there and every address is 0.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

// The PE TimeDateStamp and SizeOfImage of the running executable; false when its header is unreadable (both are then 0).
bool gameBuildIdentity(uint32_t* stamp, uint32_t* size) noexcept;
// True when the identity is the known build's.
bool gameBuildKnown() noexcept;
// True when the process is the game (its main module is EliteDangerous64.exe) and the build is not the known one: the build whose
// addresses the resolver finds by signature.
bool gameBuildUnknown() noexcept;
// For log lines: "build 334925", or "unknown build (PE stamp 0x6ACD513D, image size 0x640C000)", or "unreadable build (no PE header)",
// or "not the game (the process is Setup.exe)".
const char* gameBuildText() noexcept;

// knownRva (an RVA of the known build) in the running build. The known build: knownRva, always, with no lookup. An unknown build: for a
// table address, the address found by signature; for an address inside the code unit (.pdata function or leaf stub) of a found table
// address -- a call site, a return address, a site end -- the same offset in the unit found, which a whole-unit match keeps; 0 when it
// was not found, the table holds neither, or the image is unreadable. Not the game: 0. Resolves once, under a lock (gameBuildStartup
// starts it on an unknown build; a caller that comes first, or while it runs, waits); then a binary search; thread-safe.
uintptr_t gameRva(uint32_t knownRva) noexcept;
// Whether gameRva finds every one of knownRvas (true on the known build); the first that it does not in *firstMissing (0 when all are).
bool gameRvasFound(const uint32_t* knownRvas, size_t n, uint32_t* firstMissing) noexcept;

// A hook's pin, the known build's bytes at [knownRva, knownRva + n), against got, read at the running build's copy of knownRva. The
// known build: an exact compare. An unknown build: the bytes the signature masks inside the matched units (rel32 and RIP-relative
// fields, relocated pointers, jump-table RVAs) are skipped and every other byte must be equal; false when the range is not wholly in
// found units that moved together (one unit, or a function's chained fragments).
bool gamePinMatches(uint32_t knownRva, const uint8_t* got, const uint8_t* pin, size_t n) noexcept;
// True on an unknown build when [knownRva, knownRva + n) lies wholly inside units the resolver matched, and they moved together: the
// masked whole-unit match already proves every byte of it a rebuild does not move, so a pinned range or body hash there is satisfied by
// the match. False on the known build (nothing was matched; the hook checks its pin as it always has).
bool gamePinProvenByMatch(uint32_t knownRva, size_t n) noexcept;

// Once, after the log opens. An unknown build: starts the resolve on a background thread, so the hooks that ask later find it done, and
// calls afterResolve (when given) on that thread once the resolve is done: the VR Supersampling hold, which an unknown build keeps out
// of DllMain, arms there. Not the game: one line, nothing resolved. Then reads advanced.game_update_check (TEMPORARY, off by default).
// On, on the known build, a background thread runs the resolver against the running executable and logs whether every address it finds
// equals the table's (the "game update check:" line); nothing the hooks use changes. Off, or on any other build, the key does nothing.
void gameBuildStartup(void (*afterResolve)() = nullptr) noexcept;

}  // namespace edvr
