#include "static_prop_gate.h"
#include "kinematic_eval_hook.h"
#include "scheduler_stack_hook.h"
#include "journal_watch.h"
#include "../common/log.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace edvr {

StaticPropGate staticPropGate;

namespace {
// The ~20 s totals cadence the other instruments keep (SchedulerStackProbe's
// kReportMs): frequent enough to bracket a short flight, rare enough to keep
// the log bounded.
constexpr uint64_t kReportMs = 20000;

// The design calls for one visible record of the change oracle: on a fault
// the call ALWAYS runs (never skip on a faulted read), and the fault is
// counted so a garbage pointer reads as verify_faults in the report, not as
// a shallow hit. Same __try discipline as the scheduler probe's capture.
bool guardedRead(uintptr_t address, void* output, size_t bytes) noexcept {
    __try {
        std::memcpy(output, reinterpret_cast<const void*>(address), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Observer thunks registered with the hook modules: the gate decides through
// these so the hook files carry no link dependency on this one (the
// observer pattern in kinematic_eval_hook.h).
uint32_t decideThunk(uintptr_t job0Param) noexcept {
    return staticPropGate.decide(job0Param) == StaticPropGate::Verdict::Skip ? 1u : 0u;
}
void resetThunk() noexcept { staticPropGate.noteReset(); }
} // namespace

void StaticPropGate::invalidateLocked(InvalidSource source) noexcept {
    // No table walk: entries carry the epoch, so a bumped epoch re-runs each
    // collection once on its next sight and repopulates. Cheap at 4096 and
    // safe mid-bracket.
    ++epoch_;
    switch (source) {
        case InvalidSource::Journal: ++invalidationsJournal_; break;
        case InvalidSource::Reset: ++invalidationsReset_; break;
        case InvalidSource::Session: ++invalidationsSession_; break;
    }
}

void StaticPropGate::clearLocked() noexcept {
    for (uint32_t i = 0; i < kCollectionCap; ++i) table_[i] = CollectionEntry{};
    liveCount_ = 0;
    callsSeen_ = callsSkipped_ = callsRun_ = 0;
    forcedRefreshes_ = 0;
    invalidationsJournal_ = invalidationsReset_ = invalidationsSession_ = 0;
    cacheEvictions_ = oversize_ = verifyFaults_ = 0;
    frame_ = 0;
    lastGameplay_ = false;
    lastJumpTunnel_ = false;
    lastDisembarks_ = 0;
    lastEmbarks_ = 0;
    // epoch_ is NOT reset: entries from before the clear must not compare
    // equal against a freshly populated one within the same epoch.
    ++epoch_;
}

StaticPropGate::Verdict StaticPropGate::decide(uintptr_t job0Param) noexcept {
    if (!active_.load(std::memory_order_acquire)) return Verdict::Run;
    // Param layout verified against decomp_4321940: param_1[5] is the
    // collection's record-array base (local_78/pcVar3), param_1[6] the
    // record count (uVar2); the job walks base+i*0x2F0 with its predicate
    // byte at +0x234. The bracket already hands this struct to the probe's
    // ownership counter, which keys the same fields.
    uint64_t array = 0, count = 0;
    if (!guardedRead(job0Param + 5 * 8, &array, 8) ||
        !guardedRead(job0Param + 6 * 8, &count, 8)) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++callsSeen_;
        ++callsRun_;
        ++verifyFaults_;
        return Verdict::Run;
    }
    if (count > kCountSanityCap) {                 // wild count: run, flag it
        std::lock_guard<std::mutex> lock(mutex_);
        ++callsSeen_;
        ++callsRun_;
        ++verifyFaults_;
        return Verdict::Run;
    }
    if (count == 0 || !array) {
        // Nothing the gate can key: an empty collection's job is a no-op,
        // and a null array with a nonzero count is the engine's own crash,
        // not the gate's. Counted as seen, never cached, always run.
        std::lock_guard<std::mutex> lock(mutex_);
        ++callsSeen_;
        ++callsRun_;
        return Verdict::Run;
    }
    if (count > kRecordsPerCollection) {
        // Fixed pool exhausted for this collection: never cached, so it
        // re-runs every frame (conservative, correct; counted for the JSON).
        std::lock_guard<std::mutex> lock(mutex_);
        ++callsSeen_;
        ++callsRun_;
        ++oversize_;
        return Verdict::Run;
    }
    // Snapshot the truth BEFORE taking the lock: the SEH-guarded game-memory
    // reads stay out of the critical section, and one read serves both the
    // compare and the refresh store.
    RecordSnap snap[kRecordsPerCollection];
    for (uint64_t i = 0; i < count; ++i) {
        const uintptr_t record = static_cast<uintptr_t>(array) +
                                 static_cast<uintptr_t>(i) * kRecordStride;
        snap[i].record = array + i * kRecordStride;
        if (!guardedRead(record + kTruthOffset1, snap[i].bytes, kTruthBytes1) ||
            !guardedRead(record + kTruthOffset2, snap[i].bytes + kTruthBytes1,
                         kTruthBytes2)) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++callsSeen_;
            ++callsRun_;
            ++verifyFaults_;
            return Verdict::Run; // a faulted truth never gates the call off
        }
    }
    const uint32_t n = static_cast<uint32_t>(count);
    std::lock_guard<std::mutex> lock(mutex_);
    ++callsSeen_;
    // Open-addressed linear probe on the array pointer as the collection
    // identity; the cap is a power of two (static_assert below).
    static_assert((kCollectionCap & (kCollectionCap - 1)) == 0,
                  "slot arithmetic masks with kCollectionCap - 1");
    const uint32_t slot = static_cast<uint32_t>(
        (array >> 4) & (kCollectionCap - 1));
    CollectionEntry* entry = nullptr;
    uint32_t emptySlot = 0xFFFFFFFFu;
    for (uint32_t probe = 0; probe < kCollectionCap; ++probe) {
        const uint32_t s = (slot + probe) & (kCollectionCap - 1);
        CollectionEntry& e = table_[s];
        if (e.key == array) { entry = &e; break; }
        if (e.key == 0) { emptySlot = s; break; } // probe stops at the first hole
    }
    const uint32_t now = frame_;
    auto refresh = [&](CollectionEntry& e, uint64_t key) noexcept {
        e.key = key;
        e.count = n;
        e.epoch = epoch_;
        e.lastRunFrame = now;
        for (uint32_t i = 0; i < n; ++i) e.records[i] = snap[i];
    };
    if (!entry) {
        // First sight of the collection (or its epoch went stale without a
        // hit): populate and run. Table full -> evict the oldest by
        // last-run frame; the eviction is counted so a churning table shows.
        uint32_t target = emptySlot;
        if (target == 0xFFFFFFFFu) {
            target = 0;
            for (uint32_t s = 1; s < kCollectionCap; ++s)
                if (table_[s].lastRunFrame < table_[target].lastRunFrame) target = s;
            --liveCount_;
            ++cacheEvictions_;
        }
        refresh(table_[target], array);
        ++liveCount_;
        ++callsRun_;
        return Verdict::Run;
    }
    if (entry->epoch != epoch_ || entry->count != n) {
        refresh(*entry, array);
        ++callsRun_;
        return Verdict::Run;
    }
    bool changed = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (entry->records[i].record != snap[i].record ||
            std::memcmp(entry->records[i].bytes, snap[i].bytes, kRecordBytes) != 0) {
            changed = true;
            break;
        }
    }
    // Forced refresh failsafe (design N=30): a collection whose last run is
    // N frames old re-runs and refreshes even when its truth matches --
    // bounds slot-reuse events the slot-keyed cache cannot see.
    const bool stale = now - entry->lastRunFrame >= kForcedRefreshFrames;
    if (changed || stale) {
        refresh(*entry, array);
        ++callsRun_;
        if (!changed && stale) ++forcedRefreshes_;
        return Verdict::Run;
    }
    ++callsSkipped_;
    return Verdict::Skip;
}

void StaticPropGate::noteReset() noexcept {
    if (!active_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    invalidateLocked(InvalidSource::Reset);
}

void StaticPropGate::notePresentFrame(uint32_t presentFrame) noexcept {
    // The frame clock and the report cadence run only while enabled; the
    // tick is one atomic load when off (same contract as the probes).
    if (!active_.load(std::memory_order_acquire)) return;
    std::string lines;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frame_ = presentFrame;
        // Journal boundaries, polled (the watcher exposes state, not events;
        // a change of state IS the event at once-per-frame resolution):
        // LoadGame (gameplay latch rising) is a session boundary, Disembark
        // and Embark counters stepping are on-foot session boundaries, and
        // the jump-tunnel latch rising is StartJump.
        const bool gameplay = journalGameplay();
        if (gameplay && !lastGameplay_) invalidateLocked(InvalidSource::Session);
        lastGameplay_ = gameplay;
        const uint32_t disembarks = journalDisembarks();
        if (disembarks != lastDisembarks_) {
            lastDisembarks_ = disembarks;
            invalidateLocked(InvalidSource::Journal);
        }
        const uint32_t embarks = journalEmbarks();
        if (embarks != lastEmbarks_) {
            lastEmbarks_ = embarks;
            invalidateLocked(InvalidSource::Journal);
        }
        const bool tunnel = journalInJumpTunnel();
        if (tunnel && !lastJumpTunnel_) invalidateLocked(InvalidSource::Journal);
        lastJumpTunnel_ = tunnel;
        // The 20 s report line.
        const uint64_t now = GetTickCount64();
        if (now - lastReportTickMs_ < kReportMs) return;
        lastReportTickMs_ = now;
        const uint64_t invalidations = invalidationsJournal_ + invalidationsReset_ +
                                       invalidationsSession_;
        char line[512];
        std::snprintf(line, sizeof(line),
            "static_prop_gate: calls_seen=%llu skipped=%llu run=%llu "
            "forced_refreshes=%llu invalidations=%llu (journal=%llu reset=%llu "
            "session=%llu) cache_size=%llu/%llu evictions=%llu verify_faults=%llu "
            "-- skipped calls never enter the jobs[0] timed bracket; the gate's "
            "wall share is that bracket's drop against a no-gate baseline",
            (unsigned long long)callsSeen_, (unsigned long long)callsSkipped_,
            (unsigned long long)callsRun_, (unsigned long long)forcedRefreshes_,
            (unsigned long long)invalidations, (unsigned long long)invalidationsJournal_,
            (unsigned long long)invalidationsReset_,
            (unsigned long long)invalidationsSession_, (unsigned long long)liveCount_,
            (unsigned long long)kCollectionCap, (unsigned long long)cacheEvictions_,
            (unsigned long long)verifyFaults_);
        lines = line;
    }
    // Log::note after the unlock: a slow log write must not stall a worker
    // thread's gate check mid-frame (the scheduler probe's discipline).
    Log::get().note("%s", lines.c_str());
}

bool StaticPropGate::enable() noexcept {
    if (active_.load(std::memory_order_acquire)) return true;
    // Attach order matches attachSchedulerStackHooks' nesting (scheduler
    // install mutex, then the eval install mutex) so the two configure paths
    // can never interleave the opposite way round. Each attach takes and
    // releases its own mutex; nothing here nests.
    kinematicEvalSetStaticGateObserver(&decideThunk);
    schedulerStackSetResetObserver(&resetThunk);
    const char* sched = schedulerStackGateAttach();
    if (std::strcmp(sched, "installed") != 0) {
        kinematicEvalSetStaticGateObserver(nullptr);
        schedulerStackSetResetObserver(nullptr);
        statusText_ = sched;
        return false;
    }
    const char* eval = kinematicEvalStaticGateAttach();
    if (std::strcmp(eval, "installed") != 0) {
        schedulerStackGateDetach();
        kinematicEvalSetStaticGateObserver(nullptr);
        schedulerStackSetResetObserver(nullptr);
        statusText_ = eval;
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
    invalidateLocked(InvalidSource::Session); // enable is a session boundary
    const uint64_t now = GetTickCount64();
    armedTickMs_ = now;
    lastReportTickMs_ = now; // first totals line one cadence after arming
    active_.store(true, std::memory_order_release);
    statusText_ = "installed";
    return true;
}

void StaticPropGate::disable() noexcept {
    if (!active_.load(std::memory_order_acquire) &&
        std::strcmp(statusText_, "not_run") == 0)
        return;
    active_.store(false, std::memory_order_release);
    kinematicEvalStaticGateDetach();
    schedulerStackGateDetach();
    kinematicEvalSetStaticGateObserver(nullptr);
    schedulerStackSetResetObserver(nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
    statusText_ = "not_run";
}

StaticPropGate::Summary StaticPropGate::summary() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Summary s;
    s.callsSeen = callsSeen_;
    s.callsSkipped = callsSkipped_;
    s.callsRun = callsRun_;
    s.forcedRefreshes = forcedRefreshes_;
    s.invalidationsJournal = invalidationsJournal_;
    s.invalidationsReset = invalidationsReset_;
    s.invalidationsSession = invalidationsSession_;
    s.cacheEvictions = cacheEvictions_;
    s.cacheSize = liveCount_;
    s.oversize = oversize_;
    s.verifyFaults = verifyFaults_;
    return s;
}

void StaticPropGate::writeJson(std::ostringstream& j) const {
    std::lock_guard<std::mutex> lock(mutex_);
    j << "\"staticPropGate\":{\"status\":\"" << statusText_ << "\",\"active\":"
      << (active_.load(std::memory_order_acquire) ? 1 : 0)
      << ",\"callsSeen\":" << callsSeen_
      << ",\"callsSkipped\":" << callsSkipped_
      << ",\"callsRun\":" << callsRun_
      << ",\"forcedRefreshes\":" << forcedRefreshes_
      << ",\"invalidations\":{\"journal\":" << invalidationsJournal_
      << ",\"reset\":" << invalidationsReset_
      << ",\"session\":" << invalidationsSession_ << "}"
      << ",\"cacheSize\":" << liveCount_
      << ",\"cacheCap\":" << kCollectionCap
      << ",\"evictions\":" << cacheEvictions_
      << ",\"oversize\":" << oversize_
      << ",\"verifyFaults\":" << verifyFaults_
      << ",\"forcedRefreshFrames\":" << kForcedRefreshFrames
      << ",\"recordsPerCollection\":" << kRecordsPerCollection << "}";
}

void StaticPropGate::selfTestPopulateForJson() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
    statusText_ = "installed";
    callsSeen_ = 4320;
    callsSkipped_ = 3901;
    callsRun_ = 419;
    forcedRefreshes_ = 12;
    invalidationsJournal_ = 3;
    invalidationsReset_ = 2;
    invalidationsSession_ = 1;
    liveCount_ = 1572;
    cacheEvictions_ = 7;
    oversize_ = 5;
    verifyFaults_ = 1;
}

// --- config-facing lifecycle (same configure shape as
// schedulerStackProbeConfigure, driven by the once-per-second
// temporalPassConfigure re-poll) ------------------------------------------

void staticPropGateConfigure(bool on) {
    if (on == staticPropGate.enabled()) return; // 1 Hz re-poll idempotency
    if (!on) {
        const bool was = staticPropGate.enabled();
        staticPropGate.disable();
        if (was)
            Log::get().note("static_prop_gate: stood down, state cleared -- the stock "
                            "path is untouched.");
        return;
    }
    if (!staticPropGate.enable()) {
        static std::string lastFail;
        const char* status = staticPropGate.statusText();
        if (lastFail != status) {
            lastFail = status;
            Log::get().note("static_prop_gate: fix.static_prop_updates is on but the hooks "
                            "refused (%s) -- the gate stands down and the stock path is "
                            "untouched. Retried quietly on later config polls.", status);
        }
        return;
    }
    Log::get().note("static_prop_gate: fix.static_prop_updates=on -- change-gated "
                    "render-data updates live. Settlement collections whose records' "
                    "position, orientation and content mask are bit-identical to their "
                    "last run are not re-composed; first-seen, changed, invalidated and "
                    "refreshed collections re-run whole, so movers are untouched. This is "
                    "change gating, not suppression: bucket items persist and the draws "
                    "re-execute from them -- the Phase-2 flight must show census "
                    "draw-count equality. Totals every 20 s; the gate's skipped wall "
                    "time reads off the kinematic jobs[0] bracket's drop.");
}

} // namespace edvr
