#include "stall_watch.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/stall_sampler.h"

namespace edvr {
namespace {

// The watchdog is made once, on the render thread's first Present, and lives for the process: its capture buffer
// is 32 KB and a thread of ours may be inside it at any moment, so it is never destroyed (the logger's and the
// menu worker's rule, the same reason).
std::atomic<stall::Watchdog*> g_dog{nullptr};
std::atomic<int> g_state{static_cast<int>(StallWatchState::NotStarted)};

// The stall that was last sampled, for the FREEZE line. Written by the watchdog thread after each sample, read by
// the render thread when a freeze ends: a spin lock that either side holds for a few stores.
struct Last {
    std::atomic<uint32_t> lock{0};
    int64_t beatQpc = 0;
    uint32_t samples = 0;
    uint32_t maxAgeMs = 0;
    bool edvr = false;
    char owner[96] = {};
};
Last g_last;

struct LastLock {
    LastLock() { while (g_last.lock.exchange(1, std::memory_order_acquire) != 0) YieldProcessor(); }
    ~LastLock() { g_last.lock.store(0, std::memory_order_release); }
};

// Called on the watchdog thread, after the render thread is running again.
void sink(const stall::Report& r, void*) {
    char line[1150];   // the widest stall line is about 1055 characters; Log::note keeps about 1166
    stall::formatStallLine(line, sizeof(line), r);
    Log::get().note("%s", line);
    LastLock lock;
    if (g_last.beatQpc != r.beatQpc) {   // the first sample of a new stall
        g_last.beatQpc = r.beatQpc;
        g_last.samples = 0;
        g_last.maxAgeMs = 0;
        g_last.edvr = false;
        g_last.owner[0] = 0;
    }
    if (r.frames == 0) return;           // a failed sample says so in the log and is not an owner
    ++g_last.samples;
    if (r.ageMs > g_last.maxAgeMs) g_last.maxAgeMs = r.ageMs;
    if (r.edvrFrames) g_last.edvr = true;
    const stall::Resolved& o = r.at[r.owner];
    if (o.known) std::snprintf(g_last.owner, sizeof(g_last.owner), "%s+0x%x", o.module, o.rva);
    else std::snprintf(g_last.owner, sizeof(g_last.owner), "?");
}

void startOnce() {
    int expected = static_cast<int>(StallWatchState::NotStarted);
    // Whoever moves the state off NotStarted starts it; the render thread is the only caller, but the rule is cheap.
    if (!g_state.compare_exchange_strong(expected, static_cast<int>(StallWatchState::Running))) return;
    if (!Config::get().getBool("advanced.freeze_location", true)) {
        g_state.store(static_cast<int>(StallWatchState::Off));
        Log::get().note("stall sampler: off (advanced.freeze_location = off). The log will not name where the "
                        "render thread was during a freeze; the FREEZE and LONG FRAME lines are unaffected.");
        return;
    }
    auto* dog = new (std::nothrow) stall::Watchdog;
    if (!dog || !dog->start(&sink, nullptr)) {
        g_state.store(static_cast<int>(StallWatchState::Failed));
        Log::get().note("stall sampler: could not start its watchdog thread; no stall will be sampled.");
        return;
    }
    g_dog.store(dog, std::memory_order_release);
    const stall::PolicyConfig p;
    Log::get().note(
        "stall sampler: armed. The render thread (thread %lu) is watched from its first Present: a stall is no "
        "Present for %u ms, and the thread is stopped for a few tens of microseconds at %u, %u and %u ms of it, "
        "its stack copied and the thread released, and the log then names the modules on it (stall: lines). "
        "At most %u episodes a session, %.0f back to back and one more every %.0f s. In this process only: no "
        "other thread, no other process, nothing written to the thread. advanced.freeze_location = off turns "
        "it off.",
        static_cast<unsigned long>(GetCurrentThreadId()), p.at[0], p.at[0], p.at[1], p.at[2], p.sessionEpisodes,
        p.burst, p.refillMs / 1000.0);
}

}  // namespace

void stallWatchBeat(int64_t qpc, uint64_t frame) {
    stall::Watchdog* d = g_dog.load(std::memory_order_acquire);
    if (!d) {
        if (g_state.load(std::memory_order_relaxed) != static_cast<int>(StallWatchState::NotStarted)) return;
        guarded("stallWatch/start", [] { startOnce(); });
        d = g_dog.load(std::memory_order_acquire);
        if (!d) return;
    }
    d->beat(qpc, frame);
}

StallWatchState stallWatchState() { return static_cast<StallWatchState>(g_state.load(std::memory_order_relaxed)); }

bool stallWatchEpisodeAround(int64_t beatQpc, double toleranceMs, StallEpisode* out) {
    if (!out || !g_dog.load(std::memory_order_acquire)) return false;
    const int64_t freq = qpcFrequency();
    if (freq <= 0) return false;
    LastLock lock;
    if (!g_last.beatQpc || !g_last.samples) return false;
    const double offMs = static_cast<double>(g_last.beatQpc > beatQpc ? g_last.beatQpc - beatQpc : beatQpc - g_last.beatQpc) *
                         1000.0 / static_cast<double>(freq);
    if (offMs > toleranceMs) return false;
    out->samples = g_last.samples;
    out->maxAgeMs = g_last.maxAgeMs;
    out->edvrOnStack = g_last.edvr;
    std::snprintf(out->owner, sizeof(out->owner), "%s", g_last.owner);
    return true;
}

void stallWatchWriteCounts(const char* reason) {
    stall::Watchdog* d = g_dog.load(std::memory_order_acquire);
    if (!d) return;
    const stall::Counts c = d->counts();
    Log::get().note(
        "stall sampler counts reason=%s episodes=%u samples=%u skipped_rate_limit=%u failures=%u "
        "longest_suspend_us=%u longest_stall_ms=%u; an episode is a run of frames with no Present for 150 ms or "
        "more; skipped_rate_limit counts episodes whose first sample the rate limit refused; a stall of any "
        "length is also a FREEZE line when it reached 250 ms.",
        reason ? reason : "?", c.episodes, c.samples, c.skippedRateLimit, c.failures, c.longestSuspendUs,
        c.longestStallMs);
}

void stallWatchShutdown() {
    if (stall::Watchdog* d = g_dog.load(std::memory_order_acquire)) d->stop();
}

}  // namespace edvr
