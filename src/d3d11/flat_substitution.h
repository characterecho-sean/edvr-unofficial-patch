// When the flat runtime puts the game's state back where EDVR's substitution is still bound.
//
// Engine motion's draw wrapper (engine_velocity.h, "the flat draw bracket, lazy form") leaves EDVR's state
// -- MRT6, the derived blend, the patched shaders -- bound across consecutive substituted producer draws and
// puts the game's back once. This is the decision of WHEN, in one place, pure, so that the hooks say what
// happened (an event) and the policy says what to do, and a rig can drive the same table with a mistake in it.
//
// The rule is "before anything that could observe or depend on it", and the hook layer only sees hooked
// calls, so every hooked call that is not itself a substituted producer draw is either an event below or a
// setter the game's own binding replaces:
//   - a draw, dispatch, clear, copy, resolve or command-list execution that is not a substituted producer
//     draw depends on the bound state (or runs before the game's next look at it): flush;
//   - the Present is the frame's end, and every EDVR pass that follows it (the panel, the resolver's own
//     work, the frame-boundary probes) must find the game's state: flush;
//   - ClearState and a resize take the state away themselves: abandon, touching nothing, because the
//     context may be gone;
//   - a game setter of the same state replaces EDVR's and is not an event at all: the binding shadow's
//     generations record it, and the restore puts back only what is still EDVR's. One setter does not replace
//     it: OMSetRenderTargetsAndUnorderedAccessViews that KEEPS the render targets and sets the UAVs beside them
//     would find EDVR's MRT6 among the targets the UAV slots must follow, so it is an event (kKeepTargets);
//   - a game GET of that state is not seen at all (no Get is hooked), which is the one exposure the lazy form
//     has; VR's has always had it.
#pragma once

#include <cstdint>

namespace edvr {

enum class FlatSubstEvent : uint8_t {
    kProducerDraw,        // a substituted producer draw: continues the run
    kOtherDraw,           // any draw that is not
    kDispatch,
    kClear,               // ClearRenderTargetView, ClearDepthStencilView, ClearUnorderedAccessView*
    kCopy,                // CopyResource, CopySubresourceRegion, CopyStructureCount, UpdateSubresource, GenerateMips
    kResolve,             // ResolveSubresource
    kKeepTargets,         // OMSetRenderTargetsAndUnorderedAccessViews that keeps the render targets and sets UAVs
    kExecuteCommandList,
    kPresent,
    kClearState,          // the game dropped every binding
    kResize,              // the swap chain or the device went
    kCount
};

enum class FlatSubstAction : uint8_t {
    kKeep,       // nothing to do
    kFlush,      // put the game's state back through the context
    kAbandon     // forget EDVR's state without touching the context
};

// Nothing wrong. The rig instantiates the table with each of these set in turn, to see that its checks
// notice a restore that is missing; production never does.
struct FlatSubstNoFaults {
    static constexpr bool keepOnOtherDraw = false, keepOnDispatch = false, keepOnClear = false, keepOnCopy = false,
                          keepOnResolve = false, keepOnKeepTargets = false, keepOnExecuteCommandList = false,
                          keepOnPresent = false, flushOnResize = false;
};

template <class Faults = FlatSubstNoFaults>
constexpr FlatSubstAction flatSubstAction(FlatSubstEvent e) noexcept {
    switch (e) {
    case FlatSubstEvent::kProducerDraw: return FlatSubstAction::kKeep;
    case FlatSubstEvent::kOtherDraw: return Faults::keepOnOtherDraw ? FlatSubstAction::kKeep : FlatSubstAction::kFlush;
    case FlatSubstEvent::kDispatch: return Faults::keepOnDispatch ? FlatSubstAction::kKeep : FlatSubstAction::kFlush;
    case FlatSubstEvent::kClear: return Faults::keepOnClear ? FlatSubstAction::kKeep : FlatSubstAction::kFlush;
    case FlatSubstEvent::kCopy: return Faults::keepOnCopy ? FlatSubstAction::kKeep : FlatSubstAction::kFlush;
    case FlatSubstEvent::kResolve: return Faults::keepOnResolve ? FlatSubstAction::kKeep : FlatSubstAction::kFlush;
    case FlatSubstEvent::kKeepTargets: return Faults::keepOnKeepTargets ? FlatSubstAction::kKeep : FlatSubstAction::kFlush;
    case FlatSubstEvent::kExecuteCommandList:
        return Faults::keepOnExecuteCommandList ? FlatSubstAction::kKeep : FlatSubstAction::kFlush;
    case FlatSubstEvent::kPresent: return Faults::keepOnPresent ? FlatSubstAction::kKeep : FlatSubstAction::kFlush;
    case FlatSubstEvent::kClearState: return FlatSubstAction::kAbandon;
    case FlatSubstEvent::kResize: return Faults::flushOnResize ? FlatSubstAction::kFlush : FlatSubstAction::kAbandon;
    default: return FlatSubstAction::kKeep;
    }
}

}  // namespace edvr
