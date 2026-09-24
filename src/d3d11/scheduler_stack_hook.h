#pragma once
// CodeHook installation for SchedulerStackProbe. Kept apart from the probe
// so test rigs can link the probe with stub hook functions (the pattern set
// by kinematic_eval_probe/hook).
//
// Targets 2/3 (per-record drain 0x42DF940, reset+repopulate 0x36A0F50)
// carry this file's own CodeHooks. Targets 0/1 (the worker entries
// 0x4321940/0x4320340) already carry the kinematic eval hook's patch --
// CodeHook refuses to patch a function whose first instruction is a jump --
// so they are observed through that hook's job-0/1 relays instead; the
// attach below holds the eval gate open via kinematicEvalSchedulerAttach()
// and the wrappers feed schedulerStackNoteJobEntry(). The relays gate on a
// cell recomputed from BOTH consumers of these hooks (the probe and the
// static prop gate's reset feed); a dark consumer is one atomic load plus
// the trampoline call.
#include <cstdint>

namespace edvr {

class SchedulerStackProbe;

// Installs process-lifetime hooks on first call. Same return vocabulary as
// attachKinematicEvalHooks: "installed", "identity_mismatch" (the
// executable is not the hash-verified build), "opcode_mismatch" (a target
// does not carry the expected prologue or our own patch),
// "install_failed", "observer_busy".
const char* attachSchedulerStackHooks(SchedulerStackProbe* probe) noexcept;
// Stops new callbacks into this probe. Hooks and relays stay installed for
// the process lifetime; the probe itself is a global, so a callback that
// already loaded the pointer finishes safely.
void detachSchedulerStackHooks(SchedulerStackProbe* probe) noexcept;
// True when both own targets still carry OUR patch (used to accept an
// already-hooked prologue during validation).
bool schedulerStackHooksMatch(uintptr_t base) noexcept;

// The job-0/1 relay feed (targets 0/1). Called by the kinematic eval
// hook's job wrappers before their timed bracket; `entryRsp` must be the
// observed function's entry RSP (_AddressOfReturnAddress() in the wrapper,
// which the relay tail-jumps into, so that address IS the target's entry
// stack pointer and [entryRsp] is the engine caller's return address).
void schedulerStackNoteJobEntry(uint32_t target, uintptr_t entryRsp) noexcept;

// The static prop gate's reset feed (fix.static_prop_updates). The gate's
// invalidation trigger is THIS hook's reset target (FUN_1436a0f50); a
// second patch is impossible (CodeHook refuses) and unnecessary. The
// resetObserved callback invokes the registered observer before the
// forward; the gate module registers it the way the emit registers its eval
// observer, so this file carries no link dependency on the gate.
using SchedulerResetObserverFn = void (*)(void) noexcept;
void schedulerStackSetResetObserver(SchedulerResetObserverFn fn) noexcept;
// Installs this file's own hook set (validating the executable) and holds
// the relays open for the gate even while the probe itself is dark. Same
// return vocabulary as attachSchedulerStackHooks; idempotent across
// re-arms. Takes the scheduler install mutex only -- callers must not hold
// the kinematic eval hook's install mutex (attachSchedulerStackHooks nests
// scheduler-then-eval; this stays unnested so the two orders agree).
const char* schedulerStackGateAttach() noexcept;
// Closes the gate's want; the relays stay open while the probe holds them.
void schedulerStackGateDetach() noexcept;

} // namespace edvr
