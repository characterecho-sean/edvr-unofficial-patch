// One frame-boundary tick: its work under a fault budget of its own, then its
// timing mark.
//
// WHY. hookedPresent's frame boundary used to run under ONE FaultBudget,
// "deviceHook.frameBoundary" with eight faults, and that budget covered about
// fifty module ticks, the hotkeys, the menu and the config reload together. A
// fault in any of them was charged to the block, so eight faulting frames in one
// diagnostic probe stopped every other tick for the session -- the menu, the
// hotkeys, the config reload, the head-offset gate's inputs -- and the notes named
// only the block. guard.h's note now says WHERE a fault happened (module and
// offset); this says WHICH tick, in the name the budget carries.
//
// WHAT. A BoundaryTick is a FaultBudget named "frameBoundary/<mark>" and the mark
// itself (frame_ticks.h). run() runs a body under the budget and then marks the
// stretch, so the name in a FAULT ABSORBED or FEATURE-DISABLED note is the name
// the LONG FRAME line's slowest-ticks list uses for the same work. A tick whose
// budget is spent skips its body and still marks (a stretch of nothing), so the
// marks a frame records do not depend on which ticks have stood down.
//
// GROUPING. One tick is one named mark, and work that shares a mark shares a
// budget: it stands or falls together. Where two pieces of work must not -- a
// product key and a diagnostic key, say -- they get marks of their own, which the
// LONG FRAME line then reports separately. Work that is one group on purpose
// (the two halves of a pass where the second reads the first's result) says so
// where it is declared.
//
// THE FAULT COUNT is the block's old one, eight, per tick. A tick that faults on
// eight frames in a row is broken, not unlucky, and eight frames is under a tenth
// of a second; what changed is that the count is no longer shared.
//
// THE COST. A relaxed load of the budget and, on the frame's own clock, the mark
// (frame_ticks.h: one QueryPerformanceCounter read). The SEH region is free until
// something faults.
//
// THREADING. The Present hook's thread only, like the marks. The budget itself is
// atomic (guard.h), the mark is not (frame_ticks.h).
//
// A rig that runs this file alone (gate_test) is why it depends on nothing but
// guard.h and frame_ticks.h.
#pragma once

#include "../common/guard.h"
#include "frame_ticks.h"

namespace edvr {

constexpr int kBoundaryTickFaults = 8;

class BoundaryTick {
public:
    // budgetName is what a FAULT ABSORBED or FEATURE-DISABLED note says; markName
    // is the frame-tick label. Both are literals whose pointers are kept. Use
    // EDVR_BOUNDARY_TICK below so the two cannot drift apart.
    BoundaryTick(const char* budgetName, const char* markName,
                 int faults = kBoundaryTickFaults)
        : m_budget(budgetName, faults), m_mark(markName) {}

    BoundaryTick(const BoundaryTick&) = delete;
    BoundaryTick& operator=(const BoundaryTick&) = delete;

    // The body under this tick's budget, then the mark. False when the body
    // faulted or the budget is spent; the mark is taken either way.
    template <class F>
    bool run(F&& body) {
        const bool ran = guardedBudget(m_budget, static_cast<F&&>(body));
        frameTick(m_mark);
        return ran;
    }

    // Has this tick stood down for the session?
    bool disabled() const { return !m_budget.shouldRun(); }
    const char* mark() const { return m_mark; }
    const char* budgetName() const { return m_budget.name(); }

private:
    FaultBudget m_budget;
    const char* m_mark;
};

}  // namespace edvr

// Declares a tick whose budget is "frameBoundary/<mark>": one literal names the
// mark and, with the prefix, the budget, so a note names the tick the LONG FRAME
// line names.
#define EDVR_BOUNDARY_TICK(id, mark) \
    ::edvr::BoundaryTick id("frameBoundary/" mark, mark)
