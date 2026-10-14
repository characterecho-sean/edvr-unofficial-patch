#pragma once
// The light grid cache (docs/light-grid-cache-2026-10-13.md; the logic and the evidence are in light_grid_cache_core.h, the rig is tools\light_grid_cache_test): a CodeHook
// on the entry of Frontier's fragment-culling cluster builder (EliteDangerous64.exe+0x58B150, build 332841). The game keeps a one-slot memo of the last build and runs the
// whole grid again whenever the key changes, which with two eyes and a jittered projection is nearly every call (about 0.29 ms, twice a frame). This keeps the last states
// per context and, when a call's key matches one of them bit for bit, swaps that whole state back in instead of building.
//
// Always on, both profiles, no key: it is a pure optimisation and exact (see the core for why, and the verification below for how the flight proves it). Installed ONCE at
// startup; a build whose PE stamp or whose entry's sixteen bytes are not 332841's is one log line and nothing patched, never retried. While it runs, one in 1024 cache hits is
// not served: the game rebuilds and the result is compared with the cached state byte for byte. A difference, or a stored key that is not the one built from the call's inputs,
// turns the cache off for the session with a line saying why.
#include <cstdint>

namespace edvr {

// Install the hook (once; later calls do nothing). Called from device_hook after the other startup installers.
void lightGridCacheInstall();

// Once a second from device_hook's config tick: the heartbeat for a hook that has seen no call.
void lightGridCachePoll(uint64_t nowMs);

// Once a frame (Present's boundary): the frame count for the 30 s line's calls-per-frame.
void lightGridCacheNoteFrame(uint64_t frame);

// EDVR's own jitter for the frame being built (the native temporal channel's begin): the sub-pixel offset in render pixels and each eye's tangent shift. Cheap, thread-safe;
// only the jitter probe's lines read it.
void lightGridCacheNoteJitter(uint64_t frame, float jx, float jy, const float shift[2][2]);

// The detour's status ("installed", "off for this session: <why>", or why not installed).
const char* lightGridCacheStatus();

}  // namespace edvr
