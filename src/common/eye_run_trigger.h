#pragma once

#include <cstdint>

namespace edvr {

// The motion-triggered eye run is deliberately a tiny CPU-only state machine.
// A rejected rising edge waits for the body to turn off before considering the
// next one, so becoming eligible halfway through one motion episode cannot
// produce a misleading partial capture.
enum class EyeRunMotionState : uint8_t { Idle, WaitForOff, WaitForRise };
enum class EyeRunMotionStep : uint8_t { None, Trigger, Unsupported };

struct EyeRunMotionTrigger {
    EyeRunMotionState state = EyeRunMotionState::Idle;

    void cancel() { state = EyeRunMotionState::Idle; }
    void arm(bool bodyOn) {
        state = bodyOn ? EyeRunMotionState::WaitForOff : EyeRunMotionState::WaitForRise;
    }
    bool armed() const { return state != EyeRunMotionState::Idle; }

    EyeRunMotionStep observe(bool bodyOn, bool eligible) {
        if (state == EyeRunMotionState::Idle) return EyeRunMotionStep::None;
        if (state == EyeRunMotionState::WaitForOff) {
            if (!bodyOn) state = EyeRunMotionState::WaitForRise;
            return EyeRunMotionStep::None;
        }
        if (!bodyOn) return EyeRunMotionStep::None;
        if (eligible) {
            state = EyeRunMotionState::Idle;
            return EyeRunMotionStep::Trigger;
        }
        state = EyeRunMotionState::WaitForOff;
        return EyeRunMotionStep::Unsupported;
    }
};

enum EyeRunMotionMissing : uint32_t {
    EyeRunMotionNoPairedCapture = 1u << 0,
    EyeRunMotionNotNvidia       = 1u << 1,
    EyeRunMotionBadFormat       = 1u << 2,
    EyeRunMotionFoveated        = 1u << 3,
    EyeRunMotionNotFullFrame    = 1u << 4,
    EyeRunMotionNoHistory       = 1u << 5,
    EyeRunMotionNoResources     = 1u << 6,
    EyeRunMotionWrongSize       = 1u << 7,
    EyeRunMotionNoWorld         = 1u << 8,
    EyeRunMotionNoDepth         = 1u << 9,
    EyeRunMotionBadRows         = 1u << 10,
    EyeRunMotionReset           = 1u << 11,
    EyeRunMotionSizeChange      = 1u << 12,
    EyeRunMotionSourceScreen    = 1u << 13,
};

struct EyeRunMotionEligibility {
    bool paired = false;
    bool nvidia = false;
    bool format = false;
    bool nonFoveated = false;
    bool fullFrame = false;
    bool history = false;
    bool resources = false;
    bool sizeMatches = false;
    bool world = false;
    bool depth = false;
    bool rows = false;
    bool noReset = false;
    bool noSizeChange = false;
    bool noSourceScreen = false;
};

inline uint32_t eyeRunMotionMissing(const EyeRunMotionEligibility& e) {
    uint32_t missing = 0;
    if (!e.paired) missing |= EyeRunMotionNoPairedCapture;
    if (!e.nvidia) missing |= EyeRunMotionNotNvidia;
    if (!e.format) missing |= EyeRunMotionBadFormat;
    if (!e.nonFoveated) missing |= EyeRunMotionFoveated;
    if (!e.fullFrame) missing |= EyeRunMotionNotFullFrame;
    if (!e.history) missing |= EyeRunMotionNoHistory;
    if (!e.resources) missing |= EyeRunMotionNoResources;
    if (!e.sizeMatches) missing |= EyeRunMotionWrongSize;
    if (!e.world) missing |= EyeRunMotionNoWorld;
    if (!e.depth) missing |= EyeRunMotionNoDepth;
    if (!e.rows) missing |= EyeRunMotionBadRows;
    if (!e.noReset) missing |= EyeRunMotionReset;
    if (!e.noSizeChange) missing |= EyeRunMotionSizeChange;
    if (!e.noSourceScreen) missing |= EyeRunMotionSourceScreen;
    return missing;
}

}  // namespace edvr
