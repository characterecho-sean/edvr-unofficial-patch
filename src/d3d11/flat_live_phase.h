#pragma once

#include <cstdint>
#include "../common/temporal_math.h"

namespace edvr {

// Frame-level policy for the live flat projection path. The caller owns
// resource preflight, draw classification, and the temporal/spatial resolve.
// No draw may change phase after another draw has applied it.
struct FlatLivePhase {
    float currentX = 0.0f, currentY = 0.0f;
    float previousX = 0.0f, previousY = 0.0f;
    uint32_t applied = 0;
    uint32_t phaseSequence = 0;
    uint32_t warmFrames = 0;
    bool failed = false;
    bool previousAcceptedValid = false;
    // How many phases the sequence ran through when the last frame began: what
    // the caller asked beginFrame for, never history -- resetHistory leaves it.
    // The 5 s lines print it, so a flight reads the count each path used.
    uint32_t phaseCount = kTemporalJitterCount;

    void resetHistory() {
        currentX = currentY = previousX = previousY = 0.0f;
        applied = phaseSequence = warmFrames = 0;
        failed = previousAcceptedValid = false;
        renderW_ = renderH_ = 0;
    }

    // `phases` is how many frames the sequence runs before it repeats: the fixed
    // eight unless the caller follows the upscale ratio (temporalJitterPhaseCount,
    // experimental.temporal_aa_jitter_follows_upscale). The sequence number keeps
    // counting through a change of count; the phase is its remainder.
    void beginFrame(bool enabled, bool compatiblePreviousFrame,
                    uint32_t renderW, uint32_t renderH,
                    uint32_t phases = kTemporalJitterCount) {
        currentX = currentY = 0.0f;
        applied = 0;
        failed = false;
        phaseCount = phases ? phases : kTemporalJitterCount;
        if (!enabled || !renderW || !renderH) {
            resetHistory();
            return;
        }
        if (renderW_ != renderW || renderH_ != renderH ||
            !compatiblePreviousFrame || !previousAcceptedValid) {
            warmFrames = 0;
            previousX = previousY = 0.0f;
            previousAcceptedValid = false;
        }
        renderW_ = renderW;
        renderH_ = renderH;
        if (warmFrames >= 2 && previousAcceptedValid) {
            temporalJitterPhase(phaseSequence++, phaseCount, &currentX, &currentY);
        }
    }

    // Called only after a nonzero projection patch actually reached a draw.
    void noteApplied() {
        if (!failed && (currentX != 0.0f || currentY != 0.0f)) ++applied;
    }

    // A refusal before the first application keeps the entire frame at zero.
    // Later refusals cannot undo earlier geometry, so keep its chosen phase.
    void fail() {
        failed = true;
        if (!applied) currentX = currentY = 0.0f;
    }

    bool needsSpatialFallback() const { return failed && applied != 0; }

    void finish(bool temporalAccepted, bool completeCoverage) {
        const bool clean = temporalAccepted && completeCoverage && !failed;
        if (clean) {
            previousX = currentX;
            previousY = currentY;
            previousAcceptedValid = true;
            if (currentX == 0.0f && currentY == 0.0f && warmFrames < 2)
                ++warmFrames;
        } else {
            previousX = previousY = 0.0f;
            previousAcceptedValid = false;
            warmFrames = 0;
        }
    }

private:
    uint32_t renderW_ = 0, renderH_ = 0;
};

} // namespace edvr
