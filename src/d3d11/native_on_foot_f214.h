// F214 (native on-foot VR): unarmed, EDVR's view raise follows the neck while the body moves, not only when settled. Header only, no game or D3D11 state, so native_frame_test runs
// the very functions the pin hook uses. Journal: the arc doc, F213's slope flight and F214.
//
// The game's first-person camera follows the ground's slope while the third-person body stands level: the camera over the neck is +9 cm + 60 cm x tan(slope along the facing),
// at any speed (the slope flight, edvr_gfx_20261105_195735: +0.091 m + 0.595 m x tan, correlation +0.92). Downhill the view falls under the neck and swinging hands reach above
// it; uphill it rides 27 to 37 cm over the neck. F95a takes a raise target only from a settled stance, so nothing corrected this in motion (F95a was added because an unfiltered
// raise followed the walk cycle and the HUD bobbed).
//
// The rule: unarmed, upright, with the body moving and the physical head level, the pin hook's per-call target (nofoot::f94::raiseTarget on the game's camera) goes through a mean
// over the last kWindowS and, once the window holds kFillS, is published like a settled stance's target. The frame thread's filter, clamp and ramp carry it as they carry any target.
// A walk cycle's step is about 19 mm of bob in the view and the mean leaves 1.8 mm of it in the raise (the replay on the flown recording); a slope step of 25 cm is tracked in
// about kWindowS. On level ground the result is about the settled raise.
//
// Leaving the rule: to a settled stance, the settled branch publishes as it always did. To an item drawn or a crouched or hunched posture, the last SETTLED target is published
// once, so behaviour there equals what it was before F214 and does not hold a slope's value. For any other reason (pitched aim or head, no valid pose, the camera away) the raise
// holds, and the window empties by its gap rule.
//
// One writer: the pin hook (under g_pfzBusy). The key (experimental.native_on_foot_view_follows_body_moving) is TEMPORARY, an A/B switch for the pilot; with it off nothing here runs.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f214 {

constexpr double kWindowS = 0.6;       // the mean's window
constexpr double kFillS = 0.3;         // the window must span this much before it publishes
constexpr double kGapS = 0.25;         // a gap between samples over this empties the window
constexpr double kHeadGateDeg = 20.0;  // the physical head's pitch, either way, under which the raise follows (F138's settle gate is the same 20 deg on the aim)
constexpr int kCap = 256;              // samples held (kWindowS at 400 Hz would still fit)

// What a pin call did, in the recording's order (onfoot_recorder_core.h ONF_ENUM(F214State)): not_run is a call that never reached the step (a refused call).
enum State : uint8_t {
    kOff = 0, kNotMoving, kFilling, kFollowed, kHeldAim, kHeldHead, kHeldPose, kHeldPosture, kHeldItem, kHeldF138, kHeldAway, kNotRun, kStates
};
inline const char* stateName(unsigned s) noexcept {
    static const char* const n[kStates] = {"off", "not_moving", "filling", "followed", "held_aim", "held_head", "held_pose", "held_posture", "held_item", "held_f138", "held_away", "not_run"};
    return s < kStates ? n[s] : "?";
}

// ---- the time-windowed mean ---------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Mean {
    double t[kCap] = {}, v[kCap] = {};
    int head = 0, n = 0;   // the oldest sample is at head
    void clear() noexcept {
        head = 0;
        n = 0;
    }
    double lastT() const noexcept { return t[(head + n - 1) % kCap]; }
    // One sample at time tS (s). A gap over kGapS since the last sample (or a time that went back) empties the window first; samples at kWindowS or older leave it.
    void add(double tS, double value) noexcept {
        if (!std::isfinite(tS) || !std::isfinite(value)) return;
        if (n > 0 && (tS - lastT() > kGapS || tS < lastT())) clear();
        if (n == kCap) {
            head = (head + 1) % kCap;
            --n;
        }
        const int at = (head + n) % kCap;
        t[at] = tS;
        v[at] = value;
        ++n;
        while (n > 1 && tS - t[head] >= kWindowS - 1e-9) {
            head = (head + 1) % kCap;
            --n;
        }
    }
    double spanS() const noexcept { return n >= 2 ? lastT() - t[head] : 0.0; }
    bool ready() const noexcept { return n >= 2 && spanS() >= kFillS - 1e-9; }
    double mean() const noexcept {
        if (n == 0) return std::nan("");
        double s = 0.0;
        for (int i = 0; i < n; ++i) s += v[(head + i) % kCap];
        return s / n;
    }
};

// ---- the rule -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct In {
    bool keyOn = false;
    bool moving = false;        // pc.f95Moving
    bool away = false;          // the game's camera is away from the commander
    bool itemDrawn = false;     // armed, F97a's weight, or the sights
    bool lowPosture = false;    // pc.postureW at 0.5 or over, or the head joint not upright (the fists' hunched stance)
    bool owned = false;         // another publisher took this call's target (ADS's, a settled stance's, F138's)
    bool valid = false;         // the settle's own validity (si.valid): the target computable, not in the sights, the camera not away, F138's aim gate
    bool aimRefused = false;    // ... and it was F138's aim gate that refused
    bool poseValid = false;     // the physical head's pose is known (localPitchValid)
    bool fresh = false;         // a new game frame since the last call
    bool sampleOk = false;      // the per-call target is a number
    double headPitchDeg = std::nan("");
    double tS = 0.0;
    double sampleM = std::nan("");
};
struct Out {
    State state = kOff;
    bool publish = false;       // publish publishM now (the follow's mean, or the re-publish)
    bool republish = false;     // ... and it is the last settled target, once
    double publishM = std::nan("");
    double meanM = std::nan("");   // the window's mean when the call followed
};

struct Follow {
    Mean win;
    double settledM = 0.0;
    bool haveSettled = false;   // a settled target was published since the edge of the on-foot camera
    bool dirty = false;         // a moving target is the one in force
    void reset() noexcept {
        win.clear();
        settledM = 0.0;
        haveSettled = false;
        dirty = false;
    }
    // The settled branch published targetM: it is the one to go back to, and it replaced any moving target.
    void noteSettled(double targetM) noexcept {
        if (!std::isfinite(targetM)) return;
        settledM = targetM;
        haveSettled = true;
        dirty = false;
    }
    Out step(const In& in) noexcept {
        Out o;
        if (!in.keyOn) {
            o.state = kOff;
            return o;
        }
        if (in.owned) dirty = false;   // another publisher replaced the moving target
        const auto leave = [&]() noexcept {   // an item or a low posture: the settled target, once
            if (dirty && haveSettled && !in.owned) {
                o.publish = o.republish = true;
                o.publishM = settledM;
            }
            dirty = false;
        };
        if (!in.moving) {
            o.state = kNotMoving;
        } else if (in.away) {
            o.state = kHeldAway;
        } else if (in.itemDrawn) {
            o.state = kHeldItem;
            leave();
        } else if (in.lowPosture) {
            o.state = kHeldPosture;
            leave();
        } else if (in.owned) {
            o.state = kHeldF138;
        } else if (!in.valid) {
            o.state = in.aimRefused ? kHeldAim : kHeldPose;
        } else if (!in.poseValid || !std::isfinite(in.headPitchDeg)) {
            o.state = kHeldPose;
        } else if (!(std::fabs(in.headPitchDeg) < kHeadGateDeg)) {
            o.state = kHeldHead;
        } else {
            if (in.fresh && in.sampleOk) win.add(in.tS, in.sampleM);
            if (!win.ready()) {
                o.state = kFilling;
            } else {
                o.state = kFollowed;
                o.meanM = win.mean();
                if (in.fresh) {
                    o.publish = true;
                    o.publishM = o.meanM;
                    dirty = true;
                }
            }
        }
        return o;
    }
};

// ---- the log window ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Call {
    State state = kNotRun;
    bool movingUnarmed = false;   // the body moving with nothing drawn, whatever the key
    bool keyOn = false;
    double publishedM = std::nan("");   // a target this call published as the follow (not the re-publish)
    bool republished = false;
};
struct Stats {
    uint32_t calls = 0, movingUnarmed = 0, republished = 0, published = 0;
    uint32_t byState[kStates] = {};
    double pubSum = 0.0, pubMin = 0.0, pubMax = 0.0;
    bool keyOn = false;
    void add(const Call& c) noexcept {
        ++calls;
        keyOn = c.keyOn;
        if (c.movingUnarmed) ++movingUnarmed;
        if (c.state < kStates) ++byState[c.state];
        if (c.republished) ++republished;
        if (std::isfinite(c.publishedM)) {
            if (!published || c.publishedM < pubMin) pubMin = c.publishedM;
            if (!published || c.publishedM > pubMax) pubMax = c.publishedM;
            pubSum += c.publishedM;
            ++published;
        }
    }
    bool any() const noexcept { return calls > 0; }
    void clear() noexcept { *this = Stats{}; }
};

// The window's line. A reader tells the key off (key off, nothing followed), nothing eligible (key on, moving calls, all held) and running (followed) apart. tools\edvr_log.py reads it.
inline int formatLine(char* buf, size_t n, uint32_t seconds, const Stats& s) noexcept {
    char pub[96];
    if (s.published) std::snprintf(pub, sizeof(pub), "mean %+.1f cm (min %+.1f, max %+.1f) over %u calls", s.pubSum / s.published * 100.0, s.pubMin * 100.0, s.pubMax * 100.0, s.published);
    else std::snprintf(pub, sizeof(pub), "none");
    return std::snprintf(buf, n,
                         "native on foot view follows body (F214) %u s: experimental.native_on_foot_view_follows_body_moving is %s; pin calls %u, moving and unarmed %u, followed %u; "
                         "held: filling %u, aim pitch %u, head pitch %u, no pose %u, posture %u, item %u, F138 %u, away %u; published target %s; re-published %u",
                         seconds, s.keyOn ? "on" : "off", s.calls, s.movingUnarmed, s.byState[kFollowed], s.byState[kFilling], s.byState[kHeldAim], s.byState[kHeldHead], s.byState[kHeldPose],
                         s.byState[kHeldPosture], s.byState[kHeldItem], s.byState[kHeldF138], s.byState[kHeldAway], pub, s.republished);
}

}  // namespace f214
}  // namespace nofoot
}  // namespace edvr
