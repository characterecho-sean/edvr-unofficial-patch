#pragma once
// The wall time the game spends inside Map on its own context, by kind.
//
// A Map that has to wait for the GPU -- a staging readback, a renamed
// buffer whose pool is spent -- is counted as the game's CPU by the native
// benchmark (Present-to-Present less the waits it knows about), and the GPU
// drains and idles behind it. Over planetary terrain the game's frame CPU
// rose by the same two milliseconds as the GPU span outside every EDVR
// bracket, in step with the GPU frame's length and not with the hook set
// (the terrain frame-time arc, 2026-09-17); this says how much of a frame
// is Map, so that a flight can tell that stall from work.
//
// Two QueryPerformanceCounter reads per Map on the owner context and a few
// relaxed atomics; nothing waits, nothing flushes, results and flags pass
// through untouched.
#include <cstdint>
#include <d3d11.h>

namespace edvr {

struct MapWaitTotals {
    uint64_t readCalls = 0, readTicks = 0;    // D3D11_MAP_READ and READ_WRITE
    uint64_t writeCalls = 0, writeTicks = 0;  // the WRITE kinds
    uint64_t longestTicks = 0;                // the longest single call
    uint64_t slowCalls = 0;                   // calls past 100 us
};

void mapWaitNote(D3D11_MAP type, uint64_t ticks);
MapWaitTotals mapWaitTake();     // the totals since the last take, then zero
double mapWaitMs(uint64_t ticks);

} // namespace edvr
