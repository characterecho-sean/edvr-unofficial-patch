// The second place in edvr that writes to the game's code, and the last.
//
// vscreen_patch.cpp rewrites ONE texture allocation's size arguments. That
// changed 8 of 61 render targets, because the size it patched had already been
// decided somewhere else and handed down.
//
// This patches where it is decided. Reading the executable found six sites of
// the same shape: a comparison against a view mode, and if it matches, the
// panel's 1920x1080 forced as immediates -- otherwise the real size read from a
// struct field. So the resolution is an override on two view-mode values, not a
// quantity threaded through 29 owners as 6m concluded.
//
// What it changes: twelve 32-bit immediates, two per site.
// What it does NOT change: the comparison, the branch, the mode value, the
// struct read, or anything that is not one of those immediates.
//
// The actual policy, which this comment previously overstated in two ways:
//
//   * It accepts between three and twelve sites, all agreeing, and refuses
//     outside that band -- see kMinSites and kMaxExpected. Six is what build
//     330683 has. The band exists so a game update that adds or drops a call
//     site does not disable the feature while the shape it matches is still
//     unambiguous. "Exactly six" was never what the code did.
//
//   * It PROCEEDS on an unverified game build and says so in the log; it does
//     not refuse. What protects an unknown build is the shape match and the site
//     count, not a version number, and refusing on every update would make the
//     feature useless the day Frontier ships one.
//
// Rolling back completely if any single write fails, and reverting on unload,
// were both accurate: a partially applied resolution renders worse than none,
// as an earlier version of this established by shipping one.
//
// This is the one feature that writes to the game's code, so what it claims has
// to match what it does. README.md carried the same "exactly six" and has been
// corrected with it.
#pragma once

#include <cstdint>

namespace edvr {

// Rewrites the forced panel resolution at every site. Returns true only if all
// of them were written. Every failure path leaves the process untouched and
// says why in the log.
bool applyVScreenModeResolution(uint32_t width, uint32_t height);

// Restores 1920x1080 everywhere. Safe to call if nothing was patched.
void revertVScreenModeResolution();

}  // namespace edvr
