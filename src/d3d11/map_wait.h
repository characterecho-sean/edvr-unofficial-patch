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
//
// ...AND ONLY WHILE SOMETHING WILL READ THEM. The totals have exactly one
// consumer, the "native timing CPU:" line in native_timing.cpp, which exists
// only while a native timing context is current. Outside that -- a session on
// another runtime, before the runtime acquires, after it closes -- the pair of
// clock reads and the three relaxed atomics were pure cost on a path the game
// takes about 1100 times a frame. mapWaitArmed() is the flag that says the
// consumer is there; hookedMap forwards without timing when it is false.
//
// It is NOT a config key and does not answer "is this instrument wanted": with
// EDVR's own OpenXR runtime the context is acquired for the whole session, so
// in an ordinary VR flight this stays armed and the instrument costs what it
// always did. What it buys is that the cost now has the same lifetime as the
// only line that reports it.
#include <atomic>
#include <cstdint>
#include <d3d11.h>

namespace edvr {

struct MapWaitTotals {
    uint64_t readCalls = 0, readTicks = 0;    // D3D11_MAP_READ and READ_WRITE
    uint64_t writeCalls = 0, writeTicks = 0;  // the WRITE kinds
    uint64_t longestTicks = 0;                // the longest single call
    uint64_t slowCalls = 0;                   // calls past 100 us
};

namespace detail {
// Atomic because the arming side is the runtime's acquire/close, which is not
// necessarily the thread that Maps. A relaxed load is a plain mov on x86-64.
extern std::atomic<bool> g_mapWaitArmed;
}  // namespace detail

// Is the consumer of these totals present? Read once per Map, before the
// clock reads it gates.
inline bool mapWaitArmed() {
    return detail::g_mapWaitArmed.load(std::memory_order_relaxed);
}

// Called by native_timing when a timing context becomes current and when it
// closes. Zeroes the totals on the way down so a later session cannot inherit
// ticks nobody will report.
void mapWaitArm(bool on);

void mapWaitNote(D3D11_MAP type, uint64_t ticks);
MapWaitTotals mapWaitTake();     // the totals since the last take, then zero
double mapWaitMs(uint64_t ticks);

} // namespace edvr
