// The load sampler (F24, TEMPORARY, read only, no key): where the render thread and the game window's thread spend their time while the game is
// in a menu or a load pipeline is running. It reuses the stall sampler's capture (src\common\stall_sampler.h: SuspendThread, GetThreadContext,
// a copy of the stack, ResumeThread; the walk and the module naming happen after the resume). Every 100 ms while wanted, each thread is stopped
// once, and the top exclusive locations and the top three-frame callers are logged every 5 s per thread. Nothing is written to any thread.
#pragma once

#include <cstdint>

namespace edvr {

// Called every frame from the native on foot frame tick. `wanted` says whether to sample now; `why` says why not (or why, when wanted), for the
// log. The first wanted call starts the sampler's thread; a sample is never taken before then, and the log says so.
void loadSamplerTick(uint64_t nowMs, bool wanted, const char* why);

}  // namespace edvr
