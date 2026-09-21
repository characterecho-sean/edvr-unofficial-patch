#pragma once
// CodeHook installation for KinematicEvalProbe. Kept apart from the probe so
// test rigs can link the probe with stub hook functions (the pattern set by
// object_record_writer_probe/hook).
#include <cstdint>

namespace edvr {

class KinematicEvalProbe;

// Installs process-lifetime hooks (evaluator + job bodies) on first call.
// Observation is gated on the probe's armed state; an unarmed hook is one
// atomic load plus the trampoline call.
const char* attachKinematicEvalHooks(KinematicEvalProbe* probe) noexcept;
// True when the evaluator target still carries OUR patch (used to accept an
// already-hooked prologue during validation).
bool kinematicEvalHooksMatch(uintptr_t evalTarget) noexcept;

// Stops new callbacks into this probe (relay gates read a null observer and
// fall straight through to the original). Hooks and relays stay installed
// for the process lifetime; the probe itself is a global, so a bracket that
// already loaded the pointer finishes safely.
void detachKinematicEvalHooks(KinematicEvalProbe* probe) noexcept;

// --- The kinematic tracker's feed (fix.engine_motion) -----------------------
// The tracker needs the eval stream WITHOUT an eye dump armed, so the relay
// gate is a cell of its own, open while EITHER consumer wants callbacks:
// the probe while attached (attach/detach above) or the tracker while
// fix.engine_motion is on (below). The tracker registers a raw callback;
// the eval relay invokes it after the probe's observe, gated the same way.
// jobMask carries the TLS bracket bits (1u<<jobId) active at observation --
// the job-attribution discriminator (kinematic doc, 2026-09-20 17:10).
using KinematicTrackerObserverFn = void (*)(uintptr_t descriptor, uint32_t jobMask) noexcept;
void kinematicEvalSetTrackerObserver(KinematicTrackerObserverFn fn) noexcept;
// Validates the executable (PE header + evaluator prologue, or our own patch
// already in place) and installs the same hooks, then opens the gate for the
// tracker. Same return vocabulary as attachKinematicEvalHooks.
const char* kinematicEvalTrackerAttach() noexcept;
// Closes the tracker's want; the gate stays open while the probe holds it.
void kinematicEvalTrackerDetach() noexcept;

// --- The scheduler stack probe's feed (advanced.scheduler_probe) ------------
// Targets 0/1 of SchedulerStackProbe are the job bodies' own RVAs, which
// already carry this hook's patch -- CodeHook refuses a second patch and
// there is nothing to gain by double-hooking. Instead the job-0/1 relay
// wrappers call schedulerStackNoteJobEntry() (scheduler_stack_hook.h) before
// their timed bracket, at exactly the pre-forward point a dedicated hook
// would sit. For that to happen the eval relays must be live even when the
// eval probe and the tracker are both dark, so the scheduler probe is a
// third gate consumer:
void schedulerStackNoteJobEntry(uint32_t target, uintptr_t entryRsp) noexcept;
// Installs the shared kinematic hook set (validating the executable) and
// holds the eval gate open for the scheduler probe. Same return vocabulary
// as attachKinematicEvalHooks; idempotent across re-arms.
const char* kinematicEvalSchedulerAttach() noexcept;
// Closes the scheduler probe's want; the gate stays open while any other
// consumer holds it.
void kinematicEvalSchedulerDetach() noexcept;

// --- The static prop gate's feed (fix.static_prop_updates) ------------------
// Job 0's relay (FUN_144321940) is the gate's hook site -- CodeHook refuses
// a second patch, and there is nothing to gain by double-hooking. The
// bracket consults the registered observer BEFORE its timed region and, on
// a skip verdict, forwards past the call entirely: skipped calls never
// enter the timed bracket, so the gate's wall share reads off the jobs[0]
// counter's drop against a no-gate baseline. The gate module (which owns
// the cache) registers decide here, exactly the way the tracker registers
// its observer above; this file carries no link dependency on it.
using StaticGateDecideFn = uint32_t (*)(uintptr_t job0Param) noexcept;
void kinematicEvalSetStaticGateObserver(StaticGateDecideFn fn) noexcept;
// Installs the shared kinematic hook set (validating the executable) and
// holds the eval gate open for the static prop gate. Same return vocabulary
// as attachKinematicEvalHooks; idempotent across re-arms.
const char* kinematicEvalStaticGateAttach() noexcept;
// Closes the static gate's want; the gate stays open while any other
// consumer holds it.
void kinematicEvalStaticGateDetach() noexcept;

} // namespace edvr
