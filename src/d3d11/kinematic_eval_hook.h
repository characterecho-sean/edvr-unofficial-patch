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

// --- The cull gate probe's feed (advanced.cull_gate_capture) ----------------
// FUN_14430EFE0, this file's evaluator target, IS the traversal's per-(record,
// view) gate, and FUN_1442B4420, the bucket bracket's target, is the
// draw-item builder with its own per-view frustum (design doc §9). The probe
// observes both through the relays already here: the gate's verdict AFTER the
// forward (gateCtx, out, view), the builder's inputs BEFORE it (pose, ctx,
// mask, nibbles = rec+0x210). Raw callbacks, no link dependency; null means
// off (one atomic load per call).
//
// The builder observer returns the row it kept for the call (or
// kGateProbeNoRow); the bracket holds it in a thread-local across the forward,
// so every per-part test the builder makes inside that call reaches the part
// observer with its enclosing row. FUN_1442B3FC0 -- the builder's per-(sub-
// item, view) frustum + LOD test (decomp_42B3FC0.txt, called only from the
// builder's sub-item loop at 0x1442B4B91) -- is observed AFTER its forward:
// items = its param_1 (the builder's rbp+0x70 block), out = param_2 {u32 LOD,
// u8 passed}, view = param_3; fromBuilder = its return address is that call
// site's. Read-only: the verdict is never written.
constexpr uint32_t kGateProbeNoRow = 0xFFFFFFFFu;
using GateProbeGateFn = void (*)(uintptr_t gateCtx, uintptr_t out, uintptr_t view) noexcept;
using GateProbeBuilderFn = uint32_t (*)(uintptr_t pose, uintptr_t ctx, uintptr_t mask, uintptr_t nibbles) noexcept;
using GateProbePartFn = void (*)(uintptr_t items, uintptr_t out, uintptr_t view, uint32_t builderRow,
                                 bool fromBuilder) noexcept;
void kinematicEvalSetGateProbeObservers(GateProbeGateFn gate, GateProbeBuilderFn builder,
                                        GateProbePartFn part) noexcept;
// Installs the shared kinematic hook set (validating the executable) and
// holds the eval gate open for the probe's window. Same return vocabulary as
// attachKinematicEvalHooks. It also installs, once, FUN_1442B3FC0's own patch
// -- for this probe only, never for the other consumers -- after a build-keyed
// signature (PE timestamp and size, the prologue, the builder's frame and call
// site the observer reads); its relay has a gate cell of its own, open only
// while the probe is attached. A mismatch stands that hook down alone.
const char* kinematicEvalGateProbeAttach() noexcept;
void kinematicEvalGateProbeDetach() noexcept;
// True while the builder bracket (FUN_1442B4420) is installed: the probe's
// builder verdicts depend on it separately from the evaluator.
bool kinematicEvalBuilderHooked() noexcept;
// FUN_1442B3FC0's hook: "hooked", or why it stood down ("not requested",
// "not build 332841 (PE timestamp/size)", "prologue mismatch at RVA 0x42B3FC0",
// "builder frame/call-site mismatch at RVA 0x...", "CodeHook refused the patch").
const char* kinematicEvalPartTestStatus() noexcept;

} // namespace edvr
