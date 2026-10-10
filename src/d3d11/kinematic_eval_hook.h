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

// The relay gate is a cell of its own, open while ANY consumer wants
// callbacks: the probe while attached (attach/detach above), the emit
// (below), and the scheduler stack probe and the cull gate probe (further
// below). (The legacy kinematic tracker, once one of them, retired
// 2026-09-23; the static prop gate and the settlement LOD governor, two more,
// were removed 2026-10-08.)

// --- Engine-record velocity's emit feed (with fix.temporal_aa, phase 1) -----
// FUN_144312E00 (direct producer 0) appends each kinematic rig record's
// 0x150-byte pool records to its owner's node lists. The direct bracket calls
// this observer AFTER the forward with the rig record (param_1), the owner
// (param_2) and owner+0x2A4 read before and after -- the count of records the
// call appended -- so engine_velocity.cpp can write the previous pose into
// exactly those records before the engine's copier uploads them. Runs on the
// producer's job thread, under the eval gate the emit holds open itself
// (kinematicEvalEmitAttach below, since the 2026-09-23 performance review).
// Null = off: one atomic load.
using EngineEmitObserverFn = void (*)(uintptr_t record, uintptr_t owner, int32_t before, int32_t after) noexcept;
void kinematicEvalSetEmitObserver(EngineEmitObserverFn fn) noexcept;
namespace engine_velocity_emit { struct PrimaryIdentity; }
using EnginePrimaryEmitObserverFn = void (*)(const engine_velocity_emit::PrimaryIdentity&, uintptr_t owner,
    uintptr_t key, uintptr_t position, uintptr_t quaternion, int32_t before, int32_t after) noexcept;
void kinematicEvalSetPrimaryEmitObserver(EnginePrimaryEmitObserverFn fn) noexcept;
const char* kinematicEvalPrimaryEmitStatus() noexcept;
void kinematicEvalPrimaryEmitCounters(uint64_t& calls, uint64_t& unowned) noexcept;
using EnginePoolCopyObserverFn = void (*)(uintptr_t mappedBase, uint32_t stride, uintptr_t source,
                                         uint64_t firstSlot, uint32_t count) noexcept;
void kinematicEvalSetPoolCopyObserver(EnginePoolCopyObserverFn fn) noexcept;
const char* kinematicEvalPoolCopyStatus() noexcept;
using EngineMergeBeginFn = void* (*)(uintptr_t destination, uintptr_t source) noexcept;
using EngineMergeEndFn = void (*)(void* plan, bool completed) noexcept;
void kinematicEvalSetMergeObserver(EngineMergeBeginFn begin, EngineMergeEndFn end) noexcept;
const char* kinematicEvalMergeStatus() noexcept;
using EngineClearObserverFn = void (*)(uintptr_t dictionary) noexcept;
void kinematicEvalSetClearObserver(EngineClearObserverFn fn) noexcept;
const char* kinematicEvalClearStatus() noexcept;
// The emit's own want on the shared hook set: validates the executable and
// installs the same hooks as the probe's attach, then holds the gate open
// for the direct-producer relay. Same return vocabulary as
// attachKinematicEvalHooks. Detach closes the want; the gate stays open while
// any other consumer holds it.
const char* kinematicEvalEmitAttach() noexcept;
void kinematicEvalEmitDetach() noexcept;
// Whether the observer above can be called at all: the hook set installed,
// direct producer 0's relay (kinematic-build-144312e00) installed on its
// own -- it stands down alone when CodeHook refuses it -- and the gate open.
// False names the first missing piece in *why (a static string). Three
// relaxed loads: engine-record velocity asks every frame, so that zero emit
// calls can never pass for correct static motion (the 2026-09-23 review of
// engine motion).
bool kinematicEvalEmitHookLive(const char** why) noexcept;

// --- The scheduler stack probe's feed (advanced.scheduler_probe) ------------
// Targets 0/1 of SchedulerStackProbe are the job bodies' own RVAs, which
// already carry this hook's patch -- CodeHook refuses a second patch and
// there is nothing to gain by double-hooking. Instead the job-0/1 relay
// wrappers call schedulerStackNoteJobEntry() (scheduler_stack_hook.h) before
// their timed bracket, at exactly the pre-forward point a dedicated hook
// would sit. For that to happen the eval relays must be live even when the
// eval probe is dark, so the scheduler probe is a gate consumer of its own:
void schedulerStackNoteJobEntry(uint32_t target, uintptr_t entryRsp) noexcept;
// Installs the shared kinematic hook set (validating the executable) and
// holds the eval gate open for the scheduler probe. Same return vocabulary
// as attachKinematicEvalHooks; idempotent across re-arms.
const char* kinematicEvalSchedulerAttach() noexcept;
// Closes the scheduler probe's want; the gate stays open while any other
// consumer holds it.
void kinematicEvalSchedulerDetach() noexcept;

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
