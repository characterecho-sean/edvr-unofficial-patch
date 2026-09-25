#pragma once
#include <cstdint>

namespace edvr {
// F10 diagnostic budget only. It grants no rendering or camera admission.
struct FlatCameraProbe {
    uint64_t observed = 0, conflicts = 0, firstFrame = 0, lastFrame = 0;
    uint32_t attempts = 0, complete = 0, missing = 0, actualMismatch = 0;
    bool begin(bool armed, uint64_t vs, uint64_t ps, uint64_t frame, bool conflict) {
        if (!armed || vs != 0x88DCF1164C640EC3ull || ps != 0x494506A63091DF8Cull)
            return false;
        ++observed;
        if (!conflict) return false;
        ++conflicts;
        if (attempts == 2 || (attempts && frame <= lastFrame)) return false;
        if (!attempts) firstFrame = frame;
        lastFrame = frame; ++attempts;
        return true;
    }
    void finish(bool available, bool actualMatches) {
        if (!actualMatches) ++actualMismatch;
        else if (available) ++complete;
        else ++missing;
    }
    const char* result() const {
        return !observed ? "exact-pair-never-observed" : !conflicts ? "observed-without-HDR-camera-conflict" :
            actualMismatch ? "actual-shader-mismatch" : missing ? "partial-missing-evidence" :
            complete ? "captured" : "conflict-without-capture";
    }
};
}
