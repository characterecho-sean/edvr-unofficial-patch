// The test trigger of the headset-lock arc's instruments: advanced.slow_test_ms (docs/headset-lock-vdxr-2026-10-02.md).
//
// TEST ONLY, default 0 (off). A value from 1 to kSlowTestMaxMs makes the OpenXR runtime hold every xrEndFrame it makes
// that many milliseconds longer, inside the call's timed region, for kSlowTestForMs starting kSlowTestStartMs into the
// session: from outside that is what a vendor runtime that stalls in xrEndFrame looks like (the Quest 3 user's 83 ms
// per frame, 10 fps), so one flight can show the long-call episode lines (end_frame_episodes.h) and the slow regime's
// SLOW, still_slow and end lines (slow_regime.h) without a headset that misbehaves. It slows the game on purpose.
//
// Why a key of its own and not advanced.freeze_test_ms: that one makes the render thread sleep once, at 60 s, so the
// freeze lines and the stall sampler can be shown; this one holds the vendor call over and over for 40 s. One number
// cannot say both, and a value of 100 that used to be a 100 ms sleep must not turn into a 40 s regime.
//
// 40 s is long enough for the 30 s "still slow" line to show as well (5 s to begin, 30 s to the still_slow line, the
// end 40 s in), which a 10 s hold could not.
//
// The d3d11 half owns the key and the clock (perf_monitor.cpp's slowTestTick), because only it reads edvr.ini; the
// runtime half only obeys a request across frame_flag (requestEndFrameHold). This header is the schedule: pure, no
// clock, no OS call, tested by tools\slow_regime_test.
#pragma once

#include <cstdint>

namespace edvr {

constexpr uint64_t kSlowTestStartMs = 90000;
constexpr uint64_t kSlowTestForMs = 40000;
constexpr int kSlowTestMaxMs = 500;

class SlowTestSchedule {
public:
    enum class Step { None, Began, Ended };

    // The key's value, read once at the first frame; `nowMs` is the session clock's reading then. A value that is
    // not positive leaves the schedule off for good.
    void arm(uint64_t nowMs, int holdMs) {
        armed_ = true;
        armedMs_ = nowMs;
        holdMs_ = holdMs < 0 ? 0 : holdMs > kSlowTestMaxMs ? kSlowTestMaxMs : holdMs;
    }

    // Every frame. Began once, when the start is reached; Ended once, when the hold has lasted its time.
    Step tick(uint64_t nowMs) {
        if (!armed_ || holdMs_ <= 0 || done_) return Step::None;
        const uint64_t elapsed = nowMs >= armedMs_ ? nowMs - armedMs_ : 0;
        if (!active_) {
            if (elapsed < kSlowTestStartMs) return Step::None;
            active_ = true;
            return Step::Began;
        }
        if (elapsed >= kSlowTestStartMs + kSlowTestForMs) {
            active_ = false;
            done_ = true;
            return Step::Ended;
        }
        return Step::None;
    }

    bool on() const { return armed_ && holdMs_ > 0; }
    bool active() const { return active_; }
    bool done() const { return done_; }
    // The hold to ask the runtime for right now: the key's value while the hold is running, else 0.
    uint32_t holdNowMs() const { return active_ ? static_cast<uint32_t>(holdMs_) : 0u; }
    int configuredMs() const { return holdMs_; }

private:
    bool armed_ = false;
    bool active_ = false;
    bool done_ = false;
    uint64_t armedMs_ = 0;
    int holdMs_ = 0;
};

}  // namespace edvr
