// The graphics half's glue for the stall sampler (src\common\stall_sampler.h, docs/freeze-diagnostics-2026-10-01.md):
// the Present beat, the configuration key, the log lines, and the record of the last stall that the FREEZE
// line (perf_monitor.cpp) points at.
//
// advanced.freeze_location = on | off (default on): when the game's render thread goes about 150 ms without a
// Present, the log names the module and address it was stuck in. `off` starts no thread and takes no sample;
// the Present beat then costs one load and a branch.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

// The render thread, once per Present, right after the real Present returns (device_hook.cpp's hookedPresent).
// The first call starts the watchdog (or says that it is off). `frame` is the frame number the Present ended.
void stallWatchBeat(int64_t qpc, uint64_t frame);

enum class StallWatchState { NotStarted, Running, Off, Failed };
StallWatchState stallWatchState();

// The stall that began after the Present at `beatQpc` (give or take `toleranceMs`), as the sampler left it: how
// many samples it took, the longest age it sampled at, whether any sample had EDVR's own code on the stack, and
// the owner the last sample named ("nvwgf2umx.dll+0x1a2b3c4"). False when no sample belongs to that stall.
struct StallEpisode {
    uint32_t samples = 0;
    uint32_t maxAgeMs = 0;
    bool edvrOnStack = false;
    char owner[96] = {};
};
bool stallWatchEpisodeAround(int64_t beatQpc, double toleranceMs, StallEpisode* out);

// "stall sampler counts reason=<reason> episodes=.. samples=.. skipped_rate_limit=.. failures=.. longest_suspend_us=..
// longest_stall_ms=.." -- written with the long-frame counts (perf_monitor.cpp); nothing when the sampler is off or
// was never started.
void stallWatchWriteCounts(const char* reason);

// The DLL's own FreeLibrary teardown: the watchdog is told to leave. (Process exit needs nothing: the thread dies
// with the process, and a thread the sampler had stopped cannot exist: the stop never outlasts the capture.)
void stallWatchShutdown();

}  // namespace edvr
