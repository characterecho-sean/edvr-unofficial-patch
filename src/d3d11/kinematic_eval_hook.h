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

} // namespace edvr
