#pragma once
// The vertex-buffer resync: a CodeHook at the entry of Frontier's input-assembler flush (EliteDangerous64.exe+0x522A50, build 332841) that makes the applied
// vertex-buffer cache agree with the desired state before the flush binds it. The mechanism, the offsets and the evidence are in vertex_resync_core.h and
// docs/scanner-body.md; the rig is tools\vertex_resync_test.
//
// Installed ONCE at startup in BOTH profiles (the cache bug is the game's, not VR's), and always on: there is no key. A build that is not 332841, a prologue that is not the one
// read from the exe, or a CodeHook refusal is ONE log line and the hook stays out: never retried. While it is armed the log says so, every ten minutes and once when the
// session ends, with the counts (zero included), so a hook that saw nothing stale can be told from one that was never reached.
#include <cstddef>
#include <cstdint>

namespace edvr {

// Install the hook (once; later calls do nothing). Called from device_hook after the other startup installers, whatever the profile.
void vertexResyncInstall();

// Once a second from device_hook's config tick: the 60 s count while it is non-zero, and the ten-minute heartbeat. `nowMs` is a monotonic millisecond clock.
void vertexResyncPoll(uint64_t nowMs);

// A FreeLibrary teardown: the session line, if the hook is armed. The game never gets here (it never unloads this DLL); its process exit says the same line through
// Log::setExitLine, registered when the hook arms.
void vertexResyncShutdown();

#ifdef EDVR_VERTEX_RESYNC_TEST
// The rig's seams: install against a synthetic function instead of the game's, read what the instruments said, and start over. Production never defines this.
struct VertexResyncTestState {
    bool installed = false;
    uint64_t flushes = 0, repaired = 0, faults = 0;
    uint32_t sightings = 0;
    bool attempted = false;
};
void vertexResyncTestSetTarget(uintptr_t target);   // the address to hook (0: the game's); skips the PE identity check
void vertexResyncTestReset();                       // forget the attempt, the counts and the lines (the hook, once placed, stays in the synthetic function)
VertexResyncTestState vertexResyncTestState();
size_t vertexResyncTestLineCount();
const char* vertexResyncTestLine(size_t i);
int vertexResyncTestExitLine(char* out, size_t cap);   // the function registered with Log::setExitLine
#endif

}  // namespace edvr
