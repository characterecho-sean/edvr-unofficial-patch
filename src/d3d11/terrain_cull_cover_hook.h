#pragma once
// The terrain cull cover: a CodeHook at the entry of Frontier's cull-plane builder (EliteDangerous64.exe+0x443E800, build 332841) that widens the vertical fov and aspect it is
// handed to the largest half-angles of the frustum EDVR hands the game. The mechanism, the offsets and the evidence are in terrain_cull_cover_core.h and docs/terrain-culling.md;
// the rig is tools\terrain_cover_test.
//
// Always on in VR: there is no key. Installed ONCE at startup (a flat session says it is inert and patches nothing). A build whose PE stamp is not 332841's is scanned for the call
// site; anything short of one exact match is ONE log line (`terrain cull cover: inert: <why>`) and the hook stays out: never retried. While it is live the log says so, says the first
// call that had the frusta, and says a count every ten minutes (zeros included), so a hook that widened nothing can be told from one that was never reached.
#include <cstddef>
#include <cstdint>

namespace edvr {

// Install the hook (once; later calls do nothing). Called from device_hook after the other startup installers. `noVrScope` true: say it is inert and patch nothing.
// Installed for any scope with VR in it; the detour then passes the builder's call through in the flat profile.
void terrainCullCoverInstall(bool noVrScope);

// Once a second from device_hook's config tick: the ten-minute heartbeat, while the hook is live. `nowMs` is a monotonic millisecond clock.
void terrainCullCoverPoll(uint64_t nowMs);

// The frusta handed to the game for this frame ({left, right, top, bottom} tangents per eye), from the native temporal channel's beginFrame. Publishes T and H for the detour.
// Cheap, thread-safe; a frame that is not finite and non-zero leaves the last known pair alone.
void terrainCullCoverNoteFrusta(const float frusta[2][4]);

#ifdef EDVR_TERRAIN_COVER_TEST
// The rig's seams: install against a synthetic module image instead of the game's, read what the instruments said, and start over (uninstalling the hook from the synthetic
// function). Production never defines this.
struct TerrainCoverTestState {
    bool attempted = false, installed = false, byStamp = false, firstCallSaid = false;
    uint64_t calls = 0, widened = 0;   // the running heartbeat window's
};
void terrainCullCoverTestSetImage(const uint8_t* base);   // the module base to inspect (null: the game's)
void terrainCullCoverTestReset();                         // uninstall, forget the attempt, the counts, the tangents and the lines
TerrainCoverTestState terrainCullCoverTestState();
size_t terrainCullCoverTestLineCount();
const char* terrainCullCoverTestLine(size_t i);
#endif

}  // namespace edvr
