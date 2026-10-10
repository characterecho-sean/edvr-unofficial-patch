// The hardware write watchpoint on the render context's Supersampling field (ctx+0x3564; H7, H8 in
// docs/vr-supersampling-gate-2026-10-10.md). LOG ONLY: it reports each write to the 4-byte field with the writer's instruction (the
// RIP after the write), a best-effort caller list, the new value, the thread and the frame, for up to 16 writes, then disarms.
//
// Mechanism: DR0 holds the field's address and DR7 enables it as a write breakpoint, set on every other thread of the process
// (suspended, SetThreadContext with CONTEXT_DEBUG_REGISTERS, resumed) by a short-lived worker thread. A vectored exception handler
// receives the single-step exception on the writing thread (DR6 bit 0), logs, clears DR6 and continues. Threads created after
// arming have no debug register; that is a limit of the watch, stated in its arming line.
//
// Build-gated by the caller: armed only from the hold's setter or getter hooks, which exist only after the build check.
#pragma once

#include <cstdint>

namespace edvr {

// Arms the watch on the field at `fieldAddress` (4-byte aligned). Once per session: the first call wins. Returns at once; the
// arming line comes from a worker thread.
void vrContextWatchArm(uintptr_t fieldAddress);

// The render thread, once a frame: performs a disarm the handler asked for (after the cap of hits).
void vrContextWatchFrame();

// DLL unload: clears the debug registers if still armed and removes the handler. Idempotent.
void vrContextWatchShutdown();

}  // namespace edvr
