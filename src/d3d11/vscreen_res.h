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
// Refuses unless it finds exactly six sites, refuses on an unverified game
// build, rolls back completely if any single write fails, and reverts on
// unload. A partially applied resolution renders worse than none, as an earlier
// version of this established the hard way by shipping one.
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
