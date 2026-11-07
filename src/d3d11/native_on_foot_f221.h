// F221 (native on-foot VR): with the arc cutter at the hip the left elbow hangs from the body-down pole. Header only, no game or D3D11 state, so native_frame_test runs the very functions the arm
// solve uses.
//
// What the flight established. Recording 20261106_132805 (build 3e9edd3a), the arc cutter ($humanoid_repairtool_name;) at the hip: the left elbow flared when a strafe to the right started. The game's
// animated arm swung from +38 to -42 deg within about 80 ms and was back at +7 by 0.35 s; the F181a cap, measured against that animated arm, took the written elbow from -60 to -101 deg from straight down,
// 8.6 cm over the hand, and it dropped back. The cutter was on the stable pole on 5,715 of 5,903 hip rows, seeded from the first-person elbow; the rifle (F84) and the pistol hang from a body-down pole
// (angle from straight down 11 to 21 deg against the cutter's 34 to 76). Offline, hanging the elbow from the body-down pole keeps it 10 to 20 cm under the hand at every strafe start.
//
// The rule. With the arc cutter armed, the left elbow's pole is blended toward the body-down pole through the same blend as F165 and F192 (nofoot::f192::blendWeight takes the larger weight), the
// weight ramped over kRampS from 0 to 1 and back, and the F181a cap's share falls to 0 with it. The tool is not a support-cup item (supportItem() gives it kSupportNone), so the name is what
// identifies it. Every other item is as it was.
//
// Journal: the arc doc, F221. Behaviour, no key.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f221 {

constexpr double kRampS = 0.2;   // the weight takes this long from 0 to 1 and back

// The held item is the arc cutter, by the name Status.json gives it.
inline bool arcCutter(const char* name) noexcept { return name && std::strcmp(name, "$humanoid_repairtool_name;") == 0; }

// The weight, 0 to 1. A step of no time, or of a time that is not a number, leaves it where it is.
struct Ramp {
    double w = 0.0;
    double step(bool on, double dt) noexcept {
        if (!std::isfinite(dt) || !(dt > 0.0)) return w;
        const double d = dt / kRampS;
        w = on ? w + d : w - d;
        w = w > 1.0 ? 1.0 : w < 0.0 ? 0.0 : w;
        return w;
    }
};

// ---- the log window ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Stats {
    uint32_t solves = 0, hung = 0, guarded = 0, fallback = 0;
    double weightSum = 0.0, elbowOverHandSumCm = 0.0, elbowOverHandMaxCm = 0.0;
    // One left-arm solve with the weight above 0: whether the blend applied, the weight, whether the ceiling clamp moved the elbow, whether the previous radial was used, and the written elbow's y
    // less the hand's y (cm, model space).
    void add(bool wasHung, double weight, bool wasGuarded, bool usedFallback, double elbowOverHandCm) noexcept {
        if (solves == 0 || elbowOverHandCm > elbowOverHandMaxCm) elbowOverHandMaxCm = elbowOverHandCm;
        ++solves;
        if (wasHung) ++hung;
        if (wasGuarded) ++guarded;
        if (usedFallback) ++fallback;
        weightSum += weight;
        elbowOverHandSumCm += elbowOverHandCm;
    }
    bool any() const noexcept { return solves > 0; }
    void clear() noexcept { *this = Stats{}; }
};

// The window's line. tools\edvr_log.py reads it.
inline int formatLine(char* buf, size_t n, double seconds, const Stats& s) noexcept {
    const double k = s.solves ? 1.0 / s.solves : 0.0;
    return std::snprintf(buf, n, "native on foot tool elbow (F221) %.0f s: %u solves with the arc cutter, hung %u (weight mean %.2f), guarded %u, axis vertical %u; the elbow over the hand mean %+.1f cm, highest %+.1f cm", seconds,
                         s.solves, s.hung, s.weightSum * k, s.guarded, s.fallback, s.elbowOverHandSumCm * k, s.elbowOverHandMaxCm);
}

}  // namespace f221
}  // namespace nofoot
}  // namespace edvr
