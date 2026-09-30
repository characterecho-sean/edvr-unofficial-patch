// Timing for periodic work that runs on Elite's render thread.
//
// WHY. Issue 38 reports random CPU and GPU jumps. The architecture review
// (reviews/architecture-review-2026-09-29.md, P1) found several things EDVR does
// from the frame boundary on a timer -- a Status.json read, a journal tail, a
// directory walk over every journal ever written, a gamepad probe, a loader-lock
// module lookup -- none of which has ever been timed. Their cost is an estimate.
// This is phase 0: time each one, and the flight log can be read for LONG FRAME
// lines that land on the same second as a slow run. If none align, the periodic
// work is ruled out as the cause; if they do, the timestamps name which.
//
// WHAT IT WRITES, per operation (one PeriodicWork each):
//
//   periodic work: <op> n=<runs> total=<ms> max=<ms> at <HH:MM:SS.mmm> slow=<runs>[ <label>=<value>]
//       A summary of one window, at most every 30 s and only if the operation
//       ran. total and max are milliseconds, bare numbers. "at" is the local
//       time of the SLOWEST run in the window, in the same form as the log's
//       own prefix, so it can be laid against a LONG FRAME line. The optional
//       label=value is the operation's own context at that slowest run (the
//       re-glob's is the journal files it enumerated). slow= counts the runs of
//       2.0 ms or more, including the ones whose own line was withheld by the
//       limit below.
//
//   periodic work: <op> SLOW ms=<ms> at <HH:MM:SS.mmm>[ <label>=<value>]
//       One run of 2.0 ms or more, written the moment it finishes, at most one
//       such line per operation per 10 s, so a storm cannot bury the log.
//
// WHAT ABSENCE MEANS. A summary is written for every window in which the
// operation ran, however fast its runs were, so a flight of a minute or more
// with no "periodic work:" line at all means this was never wired in, or the
// operation never ran -- it does not mean all was well. Only the SLOW lines are
// conditional on something having gone wrong.
//
// THE CLOCK IS READ ONLY FOR TIMING. The local time of day costs a system call,
// so it is asked for only when a run sets a new maximum for its window or is
// slow -- a steady state of fast, unremarkable runs never calls it.
//
// STATE IS PER INSTANCE. No statics are shared between operations, which is
// what lets a rig drive two side by side, or one at a time, with a fake clock
// and see exactly what a live run would write. record() is the whole decision
// and is pure: time, the wall clock and the log all come in as arguments.
// Not thread-safe: one instance belongs to one thread. Every user here is the
// Present thread except the journal watcher's three. Its file work moved to a
// worker thread of its own once this timing had priced it (journal_watch.cpp),
// and the three instances went with it: they still time the work, which is no
// longer on the frame.
#pragma once

#include <cstdint>
#include <cstdio>

#include <windows.h>

#include "log.h"

namespace edvr {

// A time of day in the form Log prefixes its own lines with.
struct PeriodicWallClock {
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    unsigned millis = 0;
};

// The thresholds, in integers so a comparison against a run of a given number
// of clock ticks is exact: a run of 1.9999 ms is not slow and one of 2.0000 ms
// is, with no floating-point boundary to argue about.
struct PeriodicWorkPolicy {
    int64_t slowUs = 2000;            // a run this long or longer is written at once
    int64_t summaryEveryMs = 30000;   // a summary at most this often
    int64_t slowLineEveryMs = 10000;  // and at most one slow-run line per this long
};

class PeriodicWork {
public:
    // Both strings are literals: the instance keeps the pointers.
    constexpr explicit PeriodicWork(const char* operation, const char* contextLabel = nullptr,
                                    PeriodicWorkPolicy policy = PeriodicWorkPolicy()) noexcept
        : m_operation(operation), m_contextLabel(contextLabel), m_policy(policy) {}

    // One finished run of `ticks` clock ticks, completed at `nowTicks`, with
    // `ticksPerSecond` ticks in a second. `context` is the operation's own count
    // for this run (0 when it has none). `wall()` returns a PeriodicWallClock and
    // is called AT MOST ONCE, and only for a new window maximum or a slow run;
    // `emit(const char* line)` receives each finished line.
    template <class WallFn, class EmitFn>
    void record(int64_t ticks, int64_t nowTicks, int64_t ticksPerSecond, uint64_t context,
                WallFn&& wall, EmitFn&& emit) {
        if (ticksPerSecond <= 0) ticksPerSecond = 1;
        if (ticks < 0) ticks = 0;
        if (!m_open) {
            m_open = true;
            m_windowStart = nowTicks;
        }
        ++m_runs;
        m_totalTicks += ticks;

        const bool newMax = m_runs == 1 || ticks > m_maxTicks;
        const bool slow = ticks * 1000000 >= m_policy.slowUs * ticksPerSecond;
        PeriodicWallClock at;
        if (newMax || slow) at = wall();
        if (newMax) {
            m_maxTicks = ticks;
            m_maxAt = at;
            m_maxContext = context;
        }

        if (slow) {
            ++m_slowRuns;
            if (!m_slowLogged ||
                (nowTicks - m_lastSlowLine) * 1000 >= m_policy.slowLineEveryMs * ticksPerSecond) {
                m_slowLogged = true;
                m_lastSlowLine = nowTicks;
                char line[224];
                int n = snprintf(line, sizeof(line), "periodic work: %s SLOW ms=%.3f at %02u:%02u:%02u.%03u",
                                 m_operation, toMs(ticks, ticksPerSecond), at.hour, at.minute,
                                 at.second, at.millis);
                appendContext(line, sizeof(line), n, context);
                emit(static_cast<const char*>(line));
            }
        }

        // The window closes on the first run at or past its length, and a new
        // one opens with the next run -- so two summaries are always at least a
        // window apart, and an operation that did not run in one writes none.
        if ((nowTicks - m_windowStart) * 1000 >= m_policy.summaryEveryMs * ticksPerSecond) {
            char line[256];
            int n = snprintf(line, sizeof(line),
                             "periodic work: %s n=%llu total=%.3f max=%.3f at %02u:%02u:%02u.%03u slow=%llu",
                             m_operation, static_cast<unsigned long long>(m_runs),
                             toMs(m_totalTicks, ticksPerSecond), toMs(m_maxTicks, ticksPerSecond),
                             m_maxAt.hour, m_maxAt.minute, m_maxAt.second, m_maxAt.millis,
                             static_cast<unsigned long long>(m_slowRuns));
            appendContext(line, sizeof(line), n, m_maxContext);
            emit(static_cast<const char*>(line));
            closeWindow();
        }
    }

    // The production binding: the real clock, the local time of day, the log.
    void recordDuration(int64_t ticks, uint64_t context = 0) {
        if (m_ticksPerSecond <= 0) m_ticksPerSecond = qpcFrequency();
        record(ticks, qpcNow(), m_ticksPerSecond, context, &localWallClock, &writeToLog);
    }

    // The open window, for a rig to read back.
    uint64_t runs() const { return m_runs; }
    uint64_t slowRuns() const { return m_slowRuns; }
    int64_t maxTicks() const { return m_maxTicks; }

private:
    static double toMs(int64_t ticks, int64_t ticksPerSecond) {
        return static_cast<double>(ticks) * 1000.0 / static_cast<double>(ticksPerSecond);
    }

    void appendContext(char* line, size_t cap, int written, uint64_t value) const {
        if (m_contextLabel == nullptr || written < 0 || static_cast<size_t>(written) >= cap) return;
        snprintf(line + written, cap - static_cast<size_t>(written), " %s=%llu", m_contextLabel,
                 static_cast<unsigned long long>(value));
    }

    void closeWindow() {
        m_open = false;
        m_runs = 0;
        m_totalTicks = 0;
        m_maxTicks = 0;
        m_maxAt = PeriodicWallClock();
        m_maxContext = 0;
        m_slowRuns = 0;
        // The slow-line limiter deliberately survives the window: ten seconds is
        // ten seconds whichever side of a summary it falls.
    }

    static PeriodicWallClock localWallClock() {
        SYSTEMTIME st;
        GetLocalTime(&st);
        PeriodicWallClock at;
        at.hour = st.wHour;
        at.minute = st.wMinute;
        at.second = st.wSecond;
        at.millis = st.wMilliseconds;
        return at;
    }

    static void writeToLog(const char* line) { Log::get().note("%s", line); }

    const char* m_operation;
    const char* m_contextLabel;
    PeriodicWorkPolicy m_policy;

    // The open window.
    bool m_open = false;
    int64_t m_windowStart = 0;
    uint64_t m_runs = 0;
    int64_t m_totalTicks = 0;
    int64_t m_maxTicks = 0;
    PeriodicWallClock m_maxAt;
    uint64_t m_maxContext = 0;
    uint64_t m_slowRuns = 0;

    // Survives windows.
    bool m_slowLogged = false;
    int64_t m_lastSlowLine = 0;

    // The production clock's rate, read once per instance.
    int64_t m_ticksPerSecond = 0;
};

// Times the enclosing scope and records it on the way out -- however the scope
// is left, which matters here: several of the timed blocks leave by return or
// goto on a failed file operation, and those are exactly the runs worth seeing.
class PeriodicWorkScope {
public:
    explicit PeriodicWorkScope(PeriodicWork& work, uint64_t context = 0)
        : m_work(work), m_context(context), m_start(qpcNow()) {}
    ~PeriodicWorkScope() { m_work.recordDuration(qpcNow() - m_start, m_context); }

    // For a count only known once the work is done.
    void setContext(uint64_t context) { m_context = context; }

    PeriodicWorkScope(const PeriodicWorkScope&) = delete;
    PeriodicWorkScope& operator=(const PeriodicWorkScope&) = delete;

private:
    PeriodicWork& m_work;
    uint64_t m_context;
    int64_t m_start;
};

}  // namespace edvr
