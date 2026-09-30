// SEH containment for hook bodies.
//
// A fault anywhere in our code must degrade to vanilla behaviour, never to a
// crash. Every hook entry point runs its body inside guarded(); if a feature
// faults repeatedly it disarms itself and the process carries on with the real
// implementation.
#pragma once

#include <atomic>
#include <string>

#include <windows.h>  // GetExceptionInformation() in guarded()

#include <cstdint>

namespace edvr {

// SEH filter. Logs the exception ONCE PER SITE and returns
// EXCEPTION_EXECUTE_HANDLER.
//
// Once per site, and it means it now -- it used to log every fault, which a
// memory scan over freed pages can turn into thousands of identical lines in
// one frame, burying whatever was being diagnosed. Later faults at a site
// already reported are counted instead.
//
// WHERE, NOT JUST WHICH SITE. A site is a budget's name, and a budget can cover
// unrelated work (deviceHook.createShader spans every create hook; the frame
// boundary was one site for dozens of ticks until each got its own, see
// src/d3d11/boundary_tick.h), so "a fault at site X" used to be all a report
// could say. The note now also carries the faulting instruction's address as
// module+offset and, for an access violation, the address it touched. guarded()
// supplies those through guardFilterAt() below; a caller that reaches this
// function with only a code gets the old note without the location.
//
// The two-argument signature is deliberate and load-bearing: three test rigs
// (depth_scene_pick_test, hologram_depth_test, ui_depth_test) define their own
// stub of exactly this function instead of linking guard.cpp, so changing it
// would break their link.
int guardFilter(unsigned long code, const char* site);

namespace detail {
// The location of the fault guardFilterAt() is judging, handed to guardFilter()
// without changing its signature. Per thread, because the filter runs on the
// faulting thread while the exception is being dispatched, and only for that
// window: valid is cleared before the filter returns. A plain copy of the few
// fields wanted, not the EXCEPTION_POINTERS themselves, which are only valid
// inside the filter expression.
//
// Everything has a constant initialiser, so this needs no dynamic TLS
// initialisation -- it is safe in a DLL the game loads late.
struct GuardFaultWhere {
    const void* ip = nullptr;       // the faulting instruction
    const void* data = nullptr;     // an access violation's target address
    unsigned    access = 0;         // 0 read, 1 write, 8 execute; with hasData
    bool        hasData = false;
    bool        valid = false;
};

inline GuardFaultWhere& guardFaultWhere() {
    static thread_local GuardFaultWhere where;
    return where;
}
}  // namespace detail

// What guarded() evaluates as its filter: the same verdict as guardFilter(),
// but the exception pointers are read HERE, inside the filter expression where
// they are valid, and the location is passed on. Inline and header-only so it
// costs a rig that stubs guardFilter() nothing.
inline int guardFilterAt(EXCEPTION_POINTERS* info, const char* site) {
    unsigned long code = 0;
    detail::GuardFaultWhere& where = detail::guardFaultWhere();
    where = detail::GuardFaultWhere();
    if (info != nullptr && info->ExceptionRecord != nullptr) {
        const EXCEPTION_RECORD& record = *info->ExceptionRecord;
        code = record.ExceptionCode;
        where.ip = record.ExceptionAddress;
        // ExceptionInformation[0] is the access kind and [1] the address, for
        // both of the memory faults; anything else has no data address.
        if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
            record.NumberParameters >= 2) {
            where.access = static_cast<unsigned>(record.ExceptionInformation[0]);
            where.data = reinterpret_cast<const void*>(record.ExceptionInformation[1]);
            where.hasData = true;
        }
        where.valid = true;
    }
    const int verdict = guardFilter(code, site);
    where.valid = false;
    return verdict;
}

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
    } __except (guardFilterAt(GetExceptionInformation(), site)) {
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
