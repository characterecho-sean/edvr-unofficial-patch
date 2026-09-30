#pragma once

#include "ui_layer_math.h"

namespace edvr {

enum class UiDrawRouteStart { Declined, Unchanged, Disabled, Unavailable, Opened };

struct UiRouteCoverageFlags {
    bool stage = true, machinery = true, combined = false;
};

// Missing samples may span several source frames behind a pending FIFO tail.
// Stamp the missing frame into every queued interval immediately; later
// failures must not make an older partial frame look complete again.
struct UiRouteCoverage {
    uint64_t stageMissing[static_cast<size_t>(UiRouteStage::kCount)][2] = {};
    uint64_t machineryMissing[2] = {}, combinedMissing[2] = {};

    bool movedMissing(int eye, uint64_t seq) {
        const bool changed = combinedMissing[eye] != seq;
        combinedMissing[eye] = seq;
        return changed;
    }
    bool missing(UiRouteStage stage, int eye, uint64_t seq) {
        uint64_t& stageSeq = stageMissing[static_cast<size_t>(stage)][eye];
        bool changed = stageSeq != seq;
        stageSeq = seq;
        if (stage != UiRouteStage::kHdrMovedDraw) {
            changed = changed || machineryMissing[eye] != seq;
            machineryMissing[eye] = seq;
        }
        return movedMissing(eye, seq) || changed;
    }
    void apply(UiRouteStage stage, int eye, uint64_t seq, UiRouteCoverageFlags& flags) const {
        flags.stage = flags.stage && seq != stageMissing[static_cast<size_t>(stage)][eye];
        flags.machinery = flags.machinery && seq != machineryMissing[eye];
        flags.combined = flags.combined && seq != combinedMissing[eye];
    }
    UiRouteCoverageFlags begin(UiRouteStage stage, int eye, uint64_t seq, bool armed) const {
        UiRouteCoverageFlags result{true, true, armed};
        apply(stage, eye, seq, result);
        return result;
    }
};

// The two totals share samples but never mistake moved game shading for
// added machinery. Bits 1/2 say which previous eye-frame closed successfully.
struct UiRouteFrameTotals {
    UiRouteSum machinery, withMoved;

    void lost(UiRouteStage stage, uint64_t seq) {
        if (stage != UiRouteStage::kHdrMovedDraw) uiRouteLost(machinery, seq);
        uiRouteLost(withMoved, seq);
    }
    unsigned add(UiRouteStage stage, uint64_t seq, double ms, bool valid, bool armed,
                 double& machineryMs, double& combinedMs, bool machineryComplete = true) {
        unsigned result = 0;
        if (stage != UiRouteStage::kHdrMovedDraw &&
            uiRouteAdd(machinery, seq, ms, valid && machineryComplete, &machineryMs)) result |= 1;
        if (uiRouteAdd(withMoved, seq, ms, valid && armed, &combinedMs)) result |= 2;
        return result;
    }
    unsigned close(uint64_t before, double& machineryMs, double& combinedMs) {
        unsigned result = 0;
        if (uiRouteClose(machinery, before, &machineryMs)) result |= 1;
        if (uiRouteClose(withMoved, before, &combinedMs)) result |= 2;
        return result;
    }
};

// Live bind/end integration, shared with the WARP timing rig. A successful
// primary HDR bind moves game shading; only diagnostics may time that work.
// Multiply re-issues retain their existing, unconditional machinery timer.
// End is idempotent, including after a guarded bind faults or a timer declines.
struct UiDrawRouteScope {
    int slot = -1;
    bool active = false;

    template<class Begin, class Missing>
    UiDrawRouteStart begin(bool accepted, bool hdr, int which, bool diagnostics,
                           int eye, uint64_t seq, Begin&& timerBegin, Missing&& missing) {
        if (!accepted || active || eye < 0 || eye > 1 || !seq) return UiDrawRouteStart::Declined;
        if (which != 1 && !hdr) return UiDrawRouteStart::Unchanged;
        active = true;
        slot = -1;
        if (which != 1 && !diagnostics) {
            missing(eye, seq);
            return UiDrawRouteStart::Disabled;
        }
        slot = timerBegin(which == 1 ? UiRouteStage::kMultiply : UiRouteStage::kHdrMovedDraw,
                          eye, seq);
        return slot >= 0 ? UiDrawRouteStart::Opened : UiDrawRouteStart::Unavailable;
    }

    template<class End>
    void end(End&& timerEnd) {
        const int pending = slot;
        slot = -1;
        active = false;
        if (pending >= 0) timerEnd(pending);
    }

    template<class Cancel>
    void cancel(Cancel&& timerCancel) {
        // A guarded bind failed before the game draw issued. Consume the
        // scope identity once, but discard its measurement rather than price
        // a zero-work or partial draw as successful moved rendering.
        end(static_cast<Cancel&&>(timerCancel));
    }
};

} // namespace edvr
