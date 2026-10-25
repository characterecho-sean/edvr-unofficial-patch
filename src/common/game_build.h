// Which game build is running, and where the addresses EDVR binds are in it (issue 124; docs\design-game-update-signatures-2026-10-25.md).
//
// The hooks bind fixed RVAs of one build, the known build (the generated table src\common\game_signatures.inc names it). On the known
// build nothing here does any work beyond reading the PE identity once: gameRva returns its argument unchanged and the resolver never
// runs. On any other build the first gameRva call finds every table address at once, by whole-function masked signature
// (game_build_core.h), from the running image's .pdata and code in memory, every read under SEH; an address that is not found uniquely
// is 0 and the hook that needs it stands down as it does today.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

// The PE TimeDateStamp and SizeOfImage of the running executable; false when its header is unreadable (both are then 0).
bool gameBuildIdentity(uint32_t* stamp, uint32_t* size) noexcept;
// True when the identity is the known build's.
bool gameBuildKnown() noexcept;
// For log lines: "build 334925", or "unknown build (PE stamp 0x6ACD513D, image size 0x640C000)", or "unreadable build (no PE header)".
const char* gameBuildText() noexcept;

// knownRva (an RVA of the known build that is in the table) in the running build. The known build: knownRva, always, with no lookup.
// An unknown build: the address found by signature, or 0 when it was not found, the table does not hold it, or the image is unreadable.
// Resolves once, on the first call, under a lock (a few tens of milliseconds); thread-safe.
uintptr_t gameRva(uint32_t knownRva) noexcept;

// A hook's pin, the known build's bytes at [knownRva, knownRva + n), against got, read at the running build's copy of knownRva. The
// known build: an exact compare. An unknown build: the bytes the signature masks inside the matched unit (rel32 and RIP-relative
// fields, relocated pointers, jump-table RVAs) are skipped and every other byte must be equal; false when no found target's unit
// holds the range whole.
bool gamePinMatches(uint32_t knownRva, const uint8_t* got, const uint8_t* pin, size_t n) noexcept;
// True on an unknown build when [knownRva, knownRva + n) lies wholly inside a unit the resolver matched: the masked whole-unit match
// already proves every byte of it a rebuild does not move, so a pinned range or body hash there is satisfied by the match. False on
// the known build (nothing was matched; the hook checks its pin as it always has).
bool gamePinProvenByMatch(uint32_t knownRva, size_t n) noexcept;

// Once, after the log opens: reads advanced.game_update_check (TEMPORARY, off by default). On, on the known build, a background
// thread runs the resolver against the running executable and logs whether every address it finds equals the table's (the
// "game update check:" line); nothing the hooks use changes. Off, or on an unknown build, it does nothing.
void gameBuildStartup() noexcept;

}  // namespace edvr
