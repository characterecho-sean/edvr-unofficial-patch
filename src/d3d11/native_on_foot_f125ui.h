// F125: the reticle's reporting window. Its stateful step/flip and trailing-run trackers still close once a second; a log flush consumes only the
// unreported share of their counters. The classification is made on each complete second, before its trackers are cleared.
#pragma once

#include "native_on_foot_f118.h"
#include "native_on_foot_logwin.h"

namespace edvr {
namespace nofoot {
namespace logwin {

inline uint64_t counterShare(uint64_t value, uint64_t mark) noexcept { return value >= mark ? value - mark : value; }

// Prime at the report's first call, before its first fold. A transition prints the value the old window was collected under, then starts the new value.
template <class T>
struct WindowMetadata {
    T value{};
    bool have = false;
    const T& prime(const T& current) noexcept {
        if (!have) next(current);
        return value;
    }
    void next(const T& current) noexcept { value = current; have = true; }
};

struct ReticleWindow {
    f118::Steps steps, stepMark;
    f118::Trail trail, trailMark;
    f118::FootSecond foot, footMark;
    SecCount edgeSeconds, speedSeconds;
    double foldLongest = 0.0;   // classification still sees the whole second when a partial log flush cleared its logging maximum

    void take(f118::Steps& s, f118::Trail& t, f118::FootSecond& f, bool fold) noexcept {
        steps.reads += counterShare(s.reads, stepMark.reads);
        steps.steps += counterShare(s.steps, stepMark.steps);
        steps.skyHit += counterShare(s.skyHit, stepMark.skyHit);
        steps.hitHit += counterShare(s.hitHit, stepMark.hitHit);
        steps.flips += counterShare(s.flips, stepMark.flips);
        steps.largestDeg = std::max(steps.largestDeg, s.largestDeg);
        trail.frames += counterShare(t.frames, trailMark.frames);
        trail.over += counterShare(t.over, trailMark.over);
        trail.longestMs = std::max(trail.longestMs, t.longestMs);
        foldLongest = std::max(foldLongest, t.longestMs);
        trail.worstDeg = std::max(trail.worstDeg, t.worstDeg);
        foot.read += counterShare(f.read, footMark.read);
        foot.edge += counterShare(f.edge, footMark.edge);
        foot.centreFar += counterShare(f.centreFar, footMark.centreFar);
        foot.incomplete += counterShare(f.incomplete, footMark.incomplete);
        foot.notDispatched += counterShare(f.notDispatched, footMark.notDispatched);
        foot.largestDeg = std::max(foot.largestDeg, f.largestDeg);
        if (fold) {
            edgeSeconds.fold(f.read >= 4 && f.edge * 4 >= f.read && s.steps <= 2);
            speedSeconds.fold(s.flips > 0 || foldLongest >= 50.0);
            foldLongest = 0.0;
            stepMark = f118::Steps{};
            trailMark = f118::Trail{};
            footMark = f118::FootSecond{};
        } else {
            stepMark = s;
            trailMark = t;
            footMark = f;
            // These are logging extrema, not the step/flip history or the live trailing run. A new reporting window needs its own peaks, including a
            // smaller peak after this one. Carry the complete second's longest run separately for its classification at the functional fold.
            s.largestDeg = 0.0;
            t.longestMs = t.worstDeg = 0.0;
            f.largestDeg = 0.0;
        }
    }
    void clearWindow() noexcept {
        steps = f118::Steps{};
        trail = f118::Trail{};
        foot = f118::FootSecond{};
        edgeSeconds.clear();
        speedSeconds.clear();
        // A partial flush leaves the trackers running until their functional second closes. Their consumed marks must survive this reporting reset.
    }
};

}  // namespace logwin
}  // namespace nofoot
}  // namespace edvr
