// The graphics half's glue for the process memory watch (src\common\mem_watch.h, docs/memory-growth-2026-10-22.md): the
// one call from the Present hook, the sampling thread, the reads of the OS and of Elite's own books, the log lines.
#pragma once

struct ID3D11Device;

namespace edvr {

// The render thread, once per owned Present, from hookedPresent (device_hook.cpp) beside vramWatchTick: both profiles,
// because the Present hook is the one place they share. The first call takes the device's adapter and starts the
// sampling thread, which lives for the process; every later call is one atomic read. Nothing here samples anything on
// the render thread.
void memWatchTick(ID3D11Device* device);

}  // namespace edvr
