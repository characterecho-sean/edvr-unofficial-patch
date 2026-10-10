#pragma once
// The vertex-buffer resync: a CodeHook at the entry of Frontier's input-assembler flush (EliteDangerous64.exe+0x522A50, build 332841) that makes the applied
// vertex-buffer cache agree with the desired state before the flush binds it. The mechanism, the offsets and the evidence are in vertex_resync_core.h and
// docs/scanner-body.md; the rig is tools\vertex_resync_test.
//
// Installed ONCE at startup in BOTH profiles (the cache bug is the game's, not VR's). A build that is not 332841, a prologue that is not the one read from the exe, or
// a CodeHook refusal is ONE log line and the hook stays out: never retried. Live key advanced.vertex_resync = on | off (default on): off counts and writes nothing.
// TEMPORARY, for the scanner-body A/B flight.
#include <cstddef>
#include <cstdint>

namespace edvr {

class Config;

// Install the hook (once; later calls do nothing). Called from device_hook after the other startup installers, whatever the profile.
void vertexResyncInstall();

// Once a second from device_hook's config tick: the live key, and the 60 s count while it is non-zero. `nowMs` is a monotonic millisecond clock.
void vertexResyncPoll(Config& cfg, uint64_t nowMs);

#ifdef EDVR_VERTEX_RESYNC_TEST
// The rig's seams: install against a synthetic function instead of the game's, read what the instruments said, and start over. Production never defines this.
struct VertexResyncTestState {
    bool installed = false;
    bool repair = true;
    uint64_t calls = 0, desynced = 0, repaired = 0, faults = 0;
    uint32_t sightings = 0;
    bool attempted = false;
};
void vertexResyncTestSetTarget(uintptr_t target);   // the address to hook (0: the game's); skips the PE identity check
void vertexResyncTestReset();                       // forget the attempt, the counts and the lines (the hook, once placed, stays in the synthetic function)
void vertexResyncTestSetRepair(bool on);
VertexResyncTestState vertexResyncTestState();
size_t vertexResyncTestLineCount();
const char* vertexResyncTestLine(size_t i);
#endif

}  // namespace edvr
