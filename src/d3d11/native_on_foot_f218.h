// F218 (native on-foot VR): with the laser rifle at the hip the torso stays on one solution, and a switch between solutions slides. Header only, no game or D3D11 state, so native_frame_test runs
// the very functions the pin hook uses.
//
// What the flight established. Recording 20261106_090823, walking and sprinting with the rifle at the hip: F82's rest solution (the physical rest target and the torso yaw that protects the neck)
// is refused for a few frames each stride (09:12:18.466 to .488, then .765 to .787) and the pin falls straight back to the legacy target with no ramp. The chest moved 8 cm forward and back in
// one frame, both elbows with it, while the hands stayed on their targets; the spine weight read 0 to 1 about three times a second (the bend runs only while the neck is not protected). The
// left elbow stepped against the chest over 5 cm 4.6 times a second walking and 3.7 sprinting.
//
// The rule has two parts.
//  - RestHold: after a refusal at the hip the rest solution is not tried for kHoldAfterRefusalMs, and while the body moves the hold keeps being extended to at least kHoldWhileMovingMs ahead, so a
//    run stays on the legacy solution and the rest solution comes back 0.3 to 1 s after the body stops. Not tried is the legacy path byte for byte.
//  - Slide: a switch between the two solutions (the legacy move and the rest move differ by a few centimetres) would still step the whole body. The slide is a rigid horizontal offset that
//    keeps the body continuous across the switch: on a change of solution it takes up the difference of the last call's move and this one's, then lets it go at kSlideMps. A difference over
//    kMaxJumpM is something else moving the body (a teleport, a turn of the frame), and is left alone.
//
// One writer: the pin hook (under g_pfzBusy). No key: with the rifle held steadily on one solution nothing here changes the pose.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f218 {

constexpr uint64_t kHoldAfterRefusalMs = 1000;   // the rest solution is not tried this long after a refusal at the hip
constexpr uint64_t kHoldWhileMovingMs = 300;     // ... and while the body moves the hold reaches at least this far ahead
constexpr double kMaxJumpM = 0.20;               // a switch whose difference of moves is over this makes no offset
constexpr double kSlideMps = 0.5;                // the offset falls at this speed
constexpr double kMaxDtS = 0.05;                 // the longest step the decay counts

struct RestHold {
    uint64_t untilMs = 0;
    // One call at the hip with the rifle: whether the rest solution is held back. A hold that ran out stays out until the next refusal.
    bool step(uint64_t nowMs, bool moving) noexcept {
        if (nowMs >= untilMs) return false;
        if (moving && nowMs + kHoldWhileMovingMs > untilMs) untilMs = nowMs + kHoldWhileMovingMs;
        return true;
    }
    void refused(uint64_t nowMs) noexcept { untilMs = nowMs + kHoldAfterRefusalMs; }
    void reset() noexcept { untilMs = 0; }
};

struct Slide {
    bool have = false, lastApplied = false;
    double lastX = 0.0, lastZ = 0.0, offX = 0.0, offZ = 0.0;
    bool switched = false;     // this call's solution differs from the last call's
    double offsetM = 0.0;      // the length of the offset after this call
    // One call: applied (the rest solution was the one written), the final horizontal move of the body (thX, thZ), the time since the last call. out receives the offset to add to every joint's x
    // and z. The first call only remembers.
    void step(bool applied, const double thX, const double thZ, double dtS, double out[2]) noexcept {
        if (!(dtS > 0.0)) dtS = 0.0;
        if (dtS > kMaxDtS) dtS = kMaxDtS;
        switched = have && applied != lastApplied;
        // the offset first lets go, then takes up this switch: on the call of a switch the body moves by the last move plus the offset left, nothing else
        const double len = std::sqrt(offX * offX + offZ * offZ);
        if (len > 0.0) {
            const double left = len - kSlideMps * dtS;
            if (left > 0.0) {
                offX *= left / len;
                offZ *= left / len;
            } else {
                offX = offZ = 0.0;
            }
        }
        if (switched) {
            const double dx = lastX - thX, dz = lastZ - thZ;
            if (std::sqrt(dx * dx + dz * dz) <= kMaxJumpM) {
                offX += dx;
                offZ += dz;
            }
        }
        have = true;
        lastApplied = applied;
        lastX = thX;
        lastZ = thZ;
        out[0] = offX;
        out[1] = offZ;
        offsetM = std::sqrt(offX * offX + offZ * offZ);
    }
    void reset() noexcept { *this = Slide{}; }
};

// ---- the log window ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Call {
    bool applied = false;    // the rest solution was written
    bool refused = false;    // it was tried and refused
    bool held = false;       // it was not tried: the hold
    bool switched = false;   // the solution changed since the last call
    double offsetM = 0.0;
};
struct Stats {
    uint32_t calls = 0, applied = 0, refused = 0, held = 0, switches = 0;
    double offsetMaxM = 0.0;
    void add(const Call& c) noexcept {
        ++calls;
        if (c.applied) ++applied;
        if (c.refused) ++refused;
        if (c.held) ++held;
        if (c.switched) ++switches;
        if (c.offsetM > offsetMaxM) offsetMaxM = c.offsetM;
    }
    bool any() const noexcept { return calls > 0; }
    void clear() noexcept { *this = Stats{}; }
};

// The window's line. A reader tells a rifle at the hip that never reached the step (no line), the rest solution held steadily (applied most calls, few switches) and a rifle that kept
// switching (switches a second). tools\edvr_log.py reads it.
inline int formatLine(char* buf, size_t n, uint32_t seconds, const Stats& s) noexcept {
    return std::snprintf(buf, n, "native on foot rifle torso (F218) %u s: rifle calls %u, rest applied %u, refused %u, held %u; switches %u; largest slide %.1f cm", seconds, s.calls, s.applied, s.refused,
                         s.held, s.switches, s.offsetMaxM * 100.0);
}

}  // namespace f218
}  // namespace nofoot
}  // namespace edvr
