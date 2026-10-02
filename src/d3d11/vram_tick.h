// The graphics half's glue for the graphics memory watch (src\common\vram_watch.h, docs/headset-lock-vdxr-2026-10-02.md,
// instrument 1): the Present tick, the adapter, the log lines.
#pragma once

#include <cstdint>

struct ID3D11Device;

namespace edvr {

// The render thread, once per owned Present, from hookedPresent (device_hook.cpp) beside stallWatchBeat: both
// profiles, because the Present hook is the one place they share. One compare of two integers per frame until the
// sample interval (one second) is up; the first call opens the adapter and writes the armed line, or the line that
// says why there will be none. Everything inside runs under a fault budget.
void vramWatchTick(int64_t qpc, ID3D11Device* device);

}  // namespace edvr
