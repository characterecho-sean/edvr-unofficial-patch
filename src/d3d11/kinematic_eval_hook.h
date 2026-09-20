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
using KinematicTrackerObserverFn = void (*)(uintptr_t descriptor) noexcept;
void kinematicEvalSetTrackerObserver(KinematicTrackerObserverFn fn) noexcept;
// Validates the executable (PE header + evaluator prologue, or our own patch
// already in place) and installs the same hooks, then opens the gate for the
// tracker. Same return vocabulary as attachKinematicEvalHooks.
const char* kinematicEvalTrackerAttach() noexcept;
// Closes the tracker's want; the gate stays open while the probe holds it.
void kinematicEvalTrackerDetach() noexcept;

} // namespace edvr
