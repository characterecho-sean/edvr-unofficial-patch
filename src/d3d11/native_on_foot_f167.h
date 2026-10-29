// F167 (native on foot VR): the left hand lets go of the pistol while sprinting. Header only, no game or D3D11 state, so native_frame_test runs the very functions the Animate post-hook
// and the frame thread use. Journal: the arc doc, F167.
//
// The pilot: with a pistol held and a sprint started, the left hand keeps its grip under the pistol, which looks wrong; the left arm should swing free. The left arm's grip is the arm
// solve's (F67f, F80): its weight is the armed solve envelope times the off hand's weight (offW; the pistol's is 1 through F80's authored support). At weight 0 the side is not solved and
// not written, so the game's own animated arm draws, with its own swing; in between the solve blends from the animated arm to the grip (the hand target, the elbow's swivel and the
// reach assist all scale by the weight). The release is therefore a factor on the off hand's weight, 1 less the release weight.
//
// The sprint is read from the AMC's horizontal speed (g_pinMotion.speed, the 0.15 s low-pass of the root's travel between successive local frames). In the flight
// edvr_gfx_20261028_113409 (the F85a events 6 to 16 and 27 to 36, a sprint with a submachine gun) that low-passed figure stayed within 5.4 and 8.5 m/s after the run-up, while the
// run before and after it (events 25 and 37) read 2 to 3 m/s; single samples swing 4.1 to 12 m/s, which F87a's 6 m/s sprint bin splits in two. The sprint begins at kEnterMps and
// ends under kExitMps, so a speed settling between them does not flap the hand. The release weight rises over kReleaseSec while the sprint holds and falls over kRegripSec once it
// has ended; in the sights (the ADS detach weight) the grip is held: the release is capped at 1 less the detach weight, so pressing the sights while sprinting returns the grip as the
// detach weight rises, and leaving them returns the release as it falls.
//
// Only a pistol (the held item's name starts with $wpn_s_pistol_, the class F90 and F144 take) and only armed: any other item, at any speed, has release 0 and nothing here touches it.
#pragma once

#include "native_on_foot_core.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f167 {

constexpr double kEnterMps = 4.6;            // the low-passed horizontal speed from which the body sprints
constexpr double kExitMps = 3.9;             // ... and under which it no longer does (between the two the state holds)
constexpr double kReleaseSec = 0.20;         // the release weight rises 0 to 1 in this long while sprinting
constexpr double kRegripSec = 0.25;          // ... and falls 1 to 0 in this long once the sprint has ended
constexpr double kMaxStepSec = 0.10;         // a call after a longer gap steps by this much
constexpr uint64_t kMaxSpeedAgeUs = 500000;  // a speed older than this is not a speed: the body is not sprinting on a stalled pin
constexpr double kReleasedHalf = 0.5;        // a frame is released from this weight
constexpr double kReleasedFull = 0.999;      // ... fully released from this one
constexpr double kInSightsFrom = 0.5;        // a frame is in the sights from this detach weight
constexpr double kStillGrippingW = 0.05;     // a left arm solved above this weight at full release is still gripping
constexpr uint32_t kMinSecondFrames = 10;    // fewer sprinting frames at the hip than this are not a second of sprint

// ---- the weight -----------------------------------------------------------------------------------------------------------------------------------------------------------

// The sprint state with a dead band: from `was`, a speed at kEnterMps or more sprints, one under kExitMps does not, anything between keeps `was`. A speed that is not a number does not
// sprint.
inline bool sprintingNow(bool was, double speed) noexcept {
    if (!std::isfinite(speed)) return false;
    return was ? speed >= kExitMps : speed >= kEnterMps;
}

// The factor on the left arm's weight: 1 with the grip, 0 fully released. A release that is not a number is no release.
inline double gripFactor(double release) noexcept { return 1.0 - unit01(release); }

struct In {
    bool pistol = false;       // the held item is a pistol
    bool armed = false;        // a weapon is drawn
    bool speedFresh = false;   // the speed was measured within kMaxSpeedAgeUs
    double speed = 0.0;        // the AMC's low-passed horizontal speed (m/s)
    double detachW = 0.0;      // the ADS detach weight, 0 at the hip and 1 in the sights
    double dtSec = 0.0;        // the time since the previous call
    uint32_t item = 0;         // the held item's serial
};
struct Step {
    bool sprinting = false;    // the sprint state after this call
    double ramp = 0.0;         // the release before the sights hold it
    double release = 0.0;      // the release weight to apply
};

struct State {
    bool sprinting = false, seen = false;
    double ramp = 0.0;
    uint32_t item = 0;

    // Back to the grip and no sprint (the arms stood down).
    void reset() noexcept {
        sprinting = seen = false;
        ramp = 0.0;
        item = 0;
    }
    Step step(const In& in) noexcept {
        Step s;
        const double dt = !(in.dtSec > 0.0) ? 0.0 : in.dtSec > kMaxStepSec ? kMaxStepSec : in.dtSec;
        if (!in.pistol || !in.armed) {   // any other item, at any speed, and a pistol put away: nothing to release, and no memory of the last pistol
            reset();
            return s;
        }
        if (!seen || in.item != item) {   // another pistol, or the first call: the grip, whatever the speed
            sprinting = false;
            ramp = 0.0;
            item = in.item;
            seen = true;
        }
        sprinting = in.speedFresh && sprintingNow(sprinting, in.speed);
        ramp += sprinting ? dt / kReleaseSec : -(dt / kRegripSec);
        ramp = ramp < 0.0 ? 0.0 : ramp > 1.0 ? 1.0 : ramp;
        const double hold = 1.0 - unit01(in.detachW);   // the sights hold the grip
        s.sprinting = sprinting;
        s.ramp = ramp;
        s.release = ramp < hold ? ramp : hold;
        return s;
    }
};

// ---- the figures ----------------------------------------------------------------------------------------------------------------------------------------------------------

// One call of an armed pistol: the speed (not a number when stale), the release applied, the detach weight, and the left arm's solve weight as it ended up.
struct Sample {
    double speed = std::nan(""), release = 0.0, detachW = 0.0, leftW = 0.0;
};
struct Counters {
    uint32_t pistol = 0;                  // armed pistol calls
    uint32_t fast = 0;                    // ... at sprint speed (kEnterMps or more), judged on the speed alone, not on the state
    uint32_t ads = 0, adsGripped = 0, adsReleased = 0;   // of those in the sights; the grip held (release under 0.5); released past what the sights allow (a defect)
    uint32_t hip = 0, released = 0, full = 0, stillGripping = 0;   // of those at the hip; released (0.5 or more); fully (0.999 or more); fully released with the left arm still solved above 0.05
    double speedSum = 0.0, speedMax = 0.0, releaseSum = 0.0, leftWSum = 0.0;   // speed over the fast calls; the release weight and the left arm's weight over the calls at the hip
    void add(const Sample& s) noexcept {
        ++pistol;
        if (!std::isfinite(s.speed) || s.speed < kEnterMps) return;
        ++fast;
        speedSum += s.speed;
        speedMax = s.speed > speedMax ? s.speed : speedMax;
        if (unit01(s.detachW) >= kInSightsFrom) {
            ++ads;
            if (s.release < kReleasedHalf) ++adsGripped;
            if (s.release > kReleasedHalf + 1e-9) ++adsReleased;   // 1 less the detach weight is 0.5 or less in the sights: the cap held, or this is a defect
            return;
        }
        ++hip;
        releaseSum += unit01(s.release);
        leftWSum += unit01(s.leftW);
        if (s.release >= kReleasedHalf) ++released;
        if (s.release >= kReleasedFull) {
            ++full;
            if (s.leftW > kStillGrippingW) ++stillGripping;
        }
    }
    void merge(const Counters& o) noexcept {
        pistol += o.pistol; fast += o.fast;
        ads += o.ads; adsGripped += o.adsGripped; adsReleased += o.adsReleased;
        hip += o.hip; released += o.released; full += o.full; stillGripping += o.stillGripping;
        speedSum += o.speedSum; releaseSum += o.releaseSum; leftWSum += o.leftWSum;
        speedMax = o.speedMax > speedMax ? o.speedMax : speedMax;
    }
};

// The seconds of a window with sprint at the hip (kMinSecondFrames calls or more), and those among them released in half or more of the calls.
struct Seconds {
    uint32_t sprint = 0, released = 0;
    void fold(const Counters& second) noexcept {
        if (second.hip < kMinSecondFrames) return;
        ++sprint;
        if (second.released * 2u >= second.hip) ++released;
    }
};

// ---- the line -------------------------------------------------------------------------------------------------------------------------------------------------------------

// The window's line: the item; the armed pistol calls and those at sprint speed with the speed's mean and largest; at the hip the release weight's mean, the calls released (0.5 or
// more) and fully (0.999 or more), with how many of those fully released still had the left arm solved above 0.05, and the left arm's weight mean; in the sights the calls, those with
// the grip held and those released past what the sights allow; then the seconds. The log's lines end at 1200 bytes; this one runs to about 600.
inline int formatSummary(char* buf, size_t n, uint32_t seconds, const char* item, const Counters& c, const Seconds& s) noexcept {
    const double fn = c.fast ? static_cast<double>(c.fast) : 1.0, hn = c.hip ? static_cast<double>(c.hip) : 1.0;
    return std::snprintf(buf, n,
                         "native on foot pistol sprint release (F167) %u s: item %s; armed pistol calls %u, at sprint speed (%.1f m/s or more) %u, speed mean %.1f max %.1f m/s; at the hip %u: "
                         "release weight mean %.2f, 0.5 or more in %u, fully in %u (the left arm still solved above weight 0.05 in %u), the left arm's weight mean %.2f; in the sights %u: "
                         "grip held in %u, released past the sights' hold in %u; seconds sprinting at the hip %u, released in half or more of them %u",
                         seconds, item ? item : "none", c.pistol, kEnterMps, c.fast, c.speedSum / fn, c.speedMax, c.hip, c.releaseSum / hn, c.released, c.full, c.stillGripping,
                         c.leftWSum / hn, c.ads, c.adsGripped, c.adsReleased, s.sprint, s.released);
}

}  // namespace f167
}  // namespace nofoot
}  // namespace edvr
