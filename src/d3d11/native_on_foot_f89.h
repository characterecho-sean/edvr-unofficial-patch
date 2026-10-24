// F89 (native on-foot VR): the pure parts of the ADS transition trace, the left eye closing in ADS and the standing-height hold's open-loop input. Design: the arc doc, F89;
// journal: the arc doc, 2026-10-24, F89.
//
// Header only, no game or D3D11 state, so native_frame_test runs the very functions the hooks use.
//
// ADS release (the trace). F88 gated the weapon eye offset's weight on ADS (the journal's flag or the resolved ADS state) and so dropped the eye to the un-offset position
// when the sights came down, then ramped it back once the journal's flag, which lags the release, cleared. The weight no longer depends on ADS (native_on_foot_f88.h). The
// trace proves the sequence in the next flight: one line per frame for kTraceWindowS after each ADS press and release, with the journal flag, the ADS state, the lock weight
// and the weight of the offset logged separately, and the eye's frame-to-frame step.
//
// Close left eye. In ADS the right eye is the eye the sight lock lines up; with fix.native_on_foot_ads_close_left_eye on, the left eye's final image fades to black by the
// ADS lock's weight (0.15 s in, 0.3 s out) and the runtime applies it as the last pass over that eye's image (d3d11_stereo.cpp). darken() is the whole decision: never above 0
// outside ADS, with the key off, unarmed or off foot (the guard, whose trip is logged).
//
// The hold's input. The hold measured dev = the render head's height + the eye alignment published last tick - the neck pivot's drop, which is the head's posture only if the
// pose carries exactly that alignment. The runtime applies a published alignment a frame or two later, so dev carried the hold's own output as published less as applied,
// and at a settle time of 0 (dev is the filter's output) the loop had gain 1 with a delay and rang: the pilot's "freak out" at 0 s, calmer at 0.1 s. The runtime now reports,
// in the frame input, the serial of the alignment its seated origin carried when it located that frame's pose; the frame thread keeps the y of every serial it published
// (AlignHistory) and takes out of the pose exactly the alignment that was applied to it, matched by serial and not by timing. The input is then the head's posture alone and
// the loop is open at any delay.
#pragma once

#include "native_on_foot_f87.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f89 {

// ---- the ADS transition trace --------------------------------------------------------------------------------------------------------------------------
constexpr double kTraceWindowS = 1.5;       // each press and each release is traced for this long
constexpr int kTraceMaxPerMinute = 4;       // at most this many events in any minute; the rest are counted and not traced
constexpr uint64_t kTraceMinuteMs = 60000;
struct TraceFrame {
    bool trace = false;       // this frame is in a traced window
    bool press = false;       // the window is a press's (else a release's)
    double t = 0.0;           // seconds since the edge (0 on the edge's own frame)
    bool started = false;     // this frame is the edge of a traced event
    bool suppressed = false;  // this frame is the edge of an event the cap kept out
};
struct AdsTrace {
    bool wasActive = false, open = false, press = false;
    double t = 0.0;
    uint64_t stamps[kTraceMaxPerMinute] = {};
    int count = 0;
    uint32_t suppressed = 0;
    // active: the resolved ADS state (the bound action's, falling back to the journal's) is on; nowMs: the frame's clock; dt: seconds since the last frame.
    TraceFrame step(bool active, uint64_t nowMs, double dt) noexcept {
        TraceFrame f;
        if (active != wasActive) {
            wasActive = active;
            int keep = 0;
            for (int i = 0; i < count; ++i)
                if (!(nowMs >= stamps[i] && nowMs - stamps[i] >= kTraceMinuteMs)) stamps[keep++] = stamps[i];
            count = keep;
            if (count < kTraceMaxPerMinute) {
                stamps[count++] = nowMs;
                open = true;
                press = active;
                t = 0.0;
                f.started = true;
            } else {
                open = false;
                ++suppressed;
                f.suppressed = true;
            }
        } else if (open) {
            t += dt > 0.0 ? dt : 0.0;
        }
        if (open && t > kTraceWindowS) open = false;
        if (open) {
            f.trace = true;
            f.press = press;
            f.t = t;
        }
        return f;
    }
};
// The eye's frame-to-frame motion in the character's frame (the right eye less F_root in the character's axes, metres in, centimetres out). A gap in the samples restarts it.
struct EyeStep {
    bool have = false;
    double last[3] = {};
    double step(const double d[3]) noexcept {
        double cm = 0.0;
        if (have) {
            const double dx = d[0] - last[0], dy = d[1] - last[1], dz = d[2] - last[2];
            cm = std::sqrt(dx * dx + dy * dy + dz * dz) * 100.0;
        }
        for (int i = 0; i < 3; ++i) last[i] = d[i];
        have = true;
        return cm;
    }
    void drop() noexcept { have = false; }
};

// ---- the left eye closing in ADS -----------------------------------------------------------------------------------------------------------------------
struct Darken {
    double level = 0.0;     // 0 (the image as it is) to 1 (black): the left eye only
    bool tripped = false;   // the guard forced 0 while the ADS lock's weight was above 0 (unarmed or off foot with the key on)
};
// lockWeight: the ADS lock's weight (the head-aim ramp, 0.15 s in and 0.3 s out), 0 outside ADS. The level is that weight with the key on, in native on foot, with a weapon
// or tool drawn; anything else is exactly 0 (positive zero), and an armed/foot failure while the weight is above 0 is the guard's trip.
inline Darken darken(bool keyOn, bool onFoot, bool armed, double lockWeight) noexcept {
    Darken d;
    const double w = lockWeight == lockWeight ? (lockWeight < 0.0 ? 0.0 : lockWeight > 1.0 ? 1.0 : lockWeight) : 0.0;
    if (!keyOn || !(w > 0.0)) return d;
    if (!onFoot || !armed) {
        d.tripped = true;
        return d;
    }
    d.level = w;
    return d;
}

// ---- the hold's open-loop input ------------------------------------------------------------------------------------------------------------------------
// The frame input's word: 0 = the runtime does not report (older than F89), else the serial of the alignment its seated origin carried, plus 1.
inline uint32_t appliedWord(uint32_t serial) noexcept { return serial + 1u; }
constexpr uint32_t kHistN = 256;
struct Applied {
    bool reported = false;   // the runtime reported a serial
    bool matched = false;    // ... and the frame thread still has that serial's value
    double y = 0.0;          // the y of the alignment the pose carried
    uint32_t lag = 0;        // publications since: the newest serial less the applied one
};
// The y of every alignment the frame thread published, by serial (serial 0 is the zero alignment before any publication).
struct AlignHistory {
    struct Slot {
        uint32_t serial = ~0u;
        double y = 0.0;
    };
    Slot s[kHistN];
    uint32_t newest = 0;
    void record(uint32_t serial, double y) noexcept {
        Slot& d = s[serial % kHistN];
        d.serial = serial;
        d.y = y;
        newest = serial;
    }
    Applied applied(uint32_t word) const noexcept {
        Applied a;
        if (word == 0) return a;
        a.reported = true;
        const uint32_t serial = word - 1u;
        a.lag = newest >= serial ? newest - serial : 0u;
        if (serial == 0) {
            a.matched = true;
            return a;
        }
        const Slot& d = s[serial % kHistN];
        if (d.serial != serial) return a;
        a.matched = true;
        a.y = d.y;
        return a;
    }
};
// Which alignment the input takes out of the pose: the applied one (matched), the one last published (a runtime that does not report: F88's measurement, with its wait),
// or none (the runtime reported a serial the frame thread no longer has: the hold waits on its last good input).
enum class Source : int { kMatched = 0, kLegacy = 1, kMissed = 2 };
struct HoldFeed {
    bool haveGood = false;
    double good = 0.0;
    struct Out {
        bool devValid = false;
        double dev = 0.0;
        Source source = Source::kMatched;
        bool wait = false;   // the input is not this frame's: the hold's filter waits
    };
    // pose: the render head and the LOCAL pitch were read; renderY: the render head's height in the seated space; drop: the neck pivot's drop; publishedY: the y last published.
    Out step(bool pose, double renderY, double drop, const Applied& a, double publishedY) noexcept {
        Out o;
        o.source = a.matched ? Source::kMatched : !a.reported ? Source::kLegacy : Source::kMissed;
        if (o.source == Source::kMissed) {
            o.wait = true;
            if (pose && haveGood) {
                o.devValid = true;
                o.dev = good;
            }
            return o;
        }
        if (!pose || !std::isfinite(renderY)) return o;
        o.dev = f87::devOf(renderY, o.source == Source::kMatched ? a.y : publishedY, drop);
        o.devValid = std::isfinite(o.dev);
        if (o.devValid) {
            haveGood = true;
            good = o.dev;
        }
        return o;
    }
};
// The hold line's second: how far behind the pose's alignment was, whether it was found, and what is left in the input once it is out. dev + drop is the head's height in the
// seated space; less the head's height in LOCAL it is a constant (the seated origin's height) when the input is the head's posture alone, so its range over the second is the
// residual: 0 with the applied alignment out, the size of the hold's own motion with the published one.
struct HoldAlignSecond {
    uint32_t n = 0, matched = 0, legacy = 0, missed = 0, lagMax = 0, residN = 0;
    uint64_t lagSum = 0;
    double newMin = 1e9, newMax = -1e9, oldMin = 1e9, oldMax = -1e9;
    uint32_t generation = 0;
    bool haveGeneration = false;
    void add(Source source, uint32_t lag) noexcept {
        ++n;
        if (source == Source::kMatched) {
            ++matched;
            lagSum += lag;
            lagMax = std::max(lagMax, lag);
        } else if (source == Source::kLegacy) {
            ++legacy;
        } else {
            ++missed;
        }
    }
    // the input less LOCAL y, with the applied alignment out (newR) and with the last published one out (oldR), metres
    void addResidual(double newR, double oldR) noexcept {
        if (!std::isfinite(newR) || !std::isfinite(oldR)) return;
        ++residN;
        newMin = std::min(newMin, newR);
        newMax = std::max(newMax, newR);
        oldMin = std::min(oldMin, oldR);
        oldMax = std::max(oldMax, oldR);
    }
    double newSpreadMm() const noexcept { return residN ? (newMax - newMin) * 1000.0 : 0.0; }
    double oldSpreadMm() const noexcept { return residN ? (oldMax - oldMin) * 1000.0 : 0.0; }
    double lagMean() const noexcept { return matched ? static_cast<double>(lagSum) / matched : 0.0; }
    // the seated origin's height moves on a new reference generation: the residual starts again
    void generationSeen(uint32_t g) noexcept {
        if (haveGeneration && g != generation) {
            residN = 0;
            newMin = oldMin = 1e9;
            newMax = oldMax = -1e9;
        }
        generation = g;
        haveGeneration = true;
    }
    void reset() noexcept {
        n = matched = legacy = missed = lagMax = residN = 0;
        lagSum = 0;
        newMin = oldMin = 1e9;
        newMax = oldMax = -1e9;
    }
};

}  // namespace f89
}  // namespace nofoot
}  // namespace edvr
