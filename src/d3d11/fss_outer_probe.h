#pragma once

#include "fss_observation.h"

namespace edvr {

// Trace-only reconstruction of the frozen caller gates for sites 57/58.
// Getters are invoked exactly at the source expression that consumes each
// raw value. The caller supplies getters over live globals/frame fields;
// these probes never call the handler or perform graphics/clock work.
// Supply a fresh zero-initialized observation and serialized owner-state
// getters: each stamp is snapshotted once, and skipped fields stay untouched.
template <class GetEnabled, class GetBodyFrame, class GetFrameNo>
bool fssOuterProbePanel(FssPanelObservation& out, GetEnabled getEnabled,
                        GetBodyFrame getBodyFrame, GetFrameNo getFrameNo) {
    const bool enabled = getEnabled();
    out.outerEnabled = {true, true, enabled};
    if (!enabled) return false;

    const uint32_t bodyFrame = getBodyFrame();
    out.bodyFrame = {true, true, bodyFrame};
    if (bodyFrame == 0) return false;

    const uint32_t frameNo = getFrameNo();
    out.frameNo = {true, true, frameNo};
    return uint32_t(frameNo - bodyFrame) <= 2;
}

template <class GetSteady, class GetLockstep, class GetBodyFrame,
          class GetBodyFrameNo, class GetJumpFrame, class GetJumpFrameNo,
          class GetModeLatch>
bool fssOuterProbeReveal(FssRevealObservation& out, GetSteady getSteady,
                         GetLockstep getLockstep, GetBodyFrame getBodyFrame,
                         GetBodyFrameNo getBodyFrameNo,
                         GetJumpFrame getJumpFrame,
                         GetJumpFrameNo getJumpFrameNo,
                         GetModeLatch getModeLatch) {
    const bool steady = getSteady();
    out.outerSteady = {true, true, steady};
    bool outerEnabled = steady;
    if (!steady) {
        const bool lockstep = getLockstep();
        out.outerLockstep = {true, true, lockstep};
        outerEnabled = lockstep;
    }
    if (!outerEnabled) return false;

    const uint32_t bodyFrame = getBodyFrame();
    out.bodyFrame = {true, true, bodyFrame};
    bool bodyFresh = false;
    if (bodyFrame != 0) {
        const uint32_t frameNo = getBodyFrameNo();
        out.bodyFrameNo = {true, true, frameNo};
        bodyFresh = uint32_t(frameNo - bodyFrame) <= 2;
    }
    if (bodyFresh) return true;

    const uint32_t jumpFrame = getJumpFrame();
    out.jumpFrame = {true, true, jumpFrame};
    if (jumpFrame == 0) return false;

    const uint32_t jumpFrameNo = getJumpFrameNo();
    out.jumpFrameNo = {true, true, jumpFrameNo};
    if (uint32_t(jumpFrameNo - jumpFrame) > 600) return false;

    const bool modeLatch = getModeLatch();
    out.modeLatch = {true, true, modeLatch};
    return modeLatch;
}

}  // namespace edvr
