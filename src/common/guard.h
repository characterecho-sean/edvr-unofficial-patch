// SEH containment for hook bodies.
//
// A fault anywhere in our code must degrade to vanilla behaviour, never to a
// crash. Every hook entry point runs its body inside guarded(); if a feature
// faults repeatedly it disarms itself and the process carries on with the real
// implementation.
#pragma once

#include <atomic>
#include <string>

#include <windows.h>  // GetExceptionCode() in guarded()

#include <cstdint>

namespace edvr {

// SEH filter. Logs the exception ONCE PER SITE and returns
// EXCEPTION_EXECUTE_HANDLER.
//
// Once per site, and it means it now -- it used to log every fault, which a
// memory scan over freed pages can turn into thousands of identical lines in
// one frame, burying whatever was being diagnosed. Later faults at a site
// already reported are counted instead.
int guardFilter(unsigned long code, const char* site);

// Print the per-site totals. Call once at shutdown.
//
// "Once per site" must not mean the total is lost: a site that faulted twice
// and one that faulted forty thousand times produce the same first line, and
// they are very different problems.
void reportFaultSites();

// Per-feature fault budget. Once exceeded, shouldRun() stays false for the rest
// of the session.
class FaultBudget {
public:
    explicit FaultBudget(const char* name, int budget = 3)
        : m_name(name), m_remaining(budget) {}

    bool shouldRun() const { return m_remaining.load(std::memory_order_relaxed) > 0; }
    void charge();
    const char* name() const { return m_name; }

private:
    const char* m_name;
    // Atomic, not volatile int. Budgets are shared across threads -- shader
    // creation runs on the loader's threads while the frame boundary runs on the
    // render thread -- and `--m_remaining` on a volatile is a read-modify-write
    // with no atomicity at all. Lost decrements need two overlapping faults;
    // the undefined behaviour needs none.
    std::atomic<int> m_remaining;
};

// Runs f() under SEH. This function itself holds no objects requiring unwind,
// which is what lets __try coexist with C++ callers.
template <class F>
inline bool guarded(const char* site, F&& f) {
    __try {
        f();
        return true;
    } __except (guardFilter(GetExceptionCode(), site)) {
        return false;
    }
}

template <class F>
inline bool guardedBudget(FaultBudget& budget, F&& f) {
    if (!budget.shouldRun()) return false;
    if (guarded(budget.name(), static_cast<F&&>(f))) return true;
    budget.charge();
    return false;
}

// Crash sentinel.
//
// Arming a hook whose vtable index we have not verified against this exact
// build is the riskiest thing the Phase 0 harness does. arm() drops a file
// before installing; confirm() deletes it once the hook has proven itself.
// A file still present at the next startup means the previous run armed and
// never confirmed, so that hook stays disabled until the user clears it.
class Sentinel {
public:
    Sentinel(const wchar_t* dir, const wchar_t* name);

    bool trippedOnStartup() const { return m_tripped; }
    // False if the file could not be written, in which case there is no
    // protection this session and the caller should say so.
    bool arm();
    void confirm();
    // Forget a trip, so a false one costs a single session rather than every
    // future launch. See the comment on the definition.
    void clearTrip();

private:
    wchar_t m_path[520]{};
    bool    m_tripped = false;
    bool    m_armed = false;
};

}  // namespace edvr
