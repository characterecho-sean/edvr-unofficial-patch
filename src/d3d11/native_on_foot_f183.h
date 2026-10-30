// F183 (native on foot VR): a heading that does not flip when the view passes vertical (the look-up runaway). Header only, no game or D3D11 state, standalone (native_on_foot_core.h includes it:
// the pin's neck target takes the heading from it), so native_frame_test runs the very functions the pin uses. Journal: the arc doc, F183.
//
// The flight edvr_gfx_20261030_125739 (build fef1e2cc; the pilot: "looking up with a rifle causes the headset to move around repeatedly"; eye dump 130125196002). Per second, with the game's aim
// pitch at +85.2 (its clamp) and the view the aim turned by the head, so a few degrees of head pitch take the view past vertical: the view's motion F95a 0.39 mm rms at +69.5, 3.8 at 13:01:13,
// 20.1 at 13:01:15 and 52.1 mm at 13:01:18; F179b's eye ahead of the chest BEFORE the guard min -33.7 against mean +35.5 cm at 13:01:15 (the same geometry read with the sign flipped on some
// frames); the sight lock's e-perpendicular after the lock 24 cm and F67f's shortfall after the assist 23.7 cm mean (max 48). Every horizontal axis the pin takes from the eye comes from the eye's
// FORWARD: the neck target's rest offset (along the heading), F97a's "ahead of the chest", F179b's heading and F168's, F171's and F178's forward and left axes. When the forward passes vertical
// its horizontal part reverses (and under 0.1 of the unit length F168/F171/F178 drop the move altogether), so the 23 cm left dial, the pin's own offset and the guard push the body the wrong
// way; the arms fall up to 48 cm short and the sight lock chases the moved gun: the view moves.
//
// The stable heading: the eye's horizontal heading (the pin's, as it is now) while the eye's forward has a horizontal length (of the unit forward) of kBlendHiXz (0.5, 60 deg from level) or more:
// bit for bit what it was. From 0.5 down to kBlendLoXz (0.17, 80 deg from level) it is blended linearly toward the body's own forward (model (0, 0, 1), the character's facing) and from there it
// IS the body's forward; and where the eye's heading points behind the body's (dot < 0) with the horizontal length under 0.5, it is the body's forward outright (the view past vertical). A level
// eye looking behind the body (the horizontal length 0.5 or more) keeps its heading as it is: the headset turned that way is not the view past vertical.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f183 {

constexpr double kBlendHiXz = 0.5, kBlendLoXz = 0.17;   // the eye's forward horizontal length (of the unit forward): unchanged from here up, the body's forward from here down
constexpr double kResetDeg = 20.0;                       // F179b: the stable heading turning more than this between calls resets the guard's filtered error
constexpr double kAimOverDeg = 80.0;                     // the health rule: a window with the aim over this and nothing blended or replaced
constexpr double kEyeBlendDeg = 60.0;                    // ... and the eye's own pitch over this (the horizontal length under 0.5): the stable heading had to act

// What one call's stable heading did. valid: an eye was known (a call with no eye rows keeps the body's forward as the pin always did and counts nothing). changed: the heading is not the eye's.
struct Info {
    bool valid = false, changed = false, blended = false, replaced = false;
    double w = 0.0;          // the blend toward the body's forward, 0..1 (1 when replaced)
    double hl = 1.0;         // the eye's forward horizontal length, of the unit forward
    double angleDeg = 0.0;   // the angle between the eye's heading and the stable one
    double h[3] = {0.0, 0.0, 1.0};   // the stable heading (x, 0, z), unit
};

// The stable heading for an eye whose forward is `fwd` (model axes, any length) and whose heading as the pin has it is `eyeH` (unit, y = 0; null reads the body's forward). eyeKnown false: the call
// had no eye rows; nothing is changed and nothing counted.
inline Info stableHeading(const double fwd[3], const double eyeH[3], bool eyeKnown = true) noexcept {
    Info s;
    if (eyeH) {
        s.h[0] = eyeH[0];
        s.h[1] = 0.0;
        s.h[2] = eyeH[2];
    }
    if (!eyeKnown) return s;
    s.valid = true;
    double hl = 0.0;
    if (fwd) {
        const double hn = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
        const double n = std::sqrt(hn * hn + fwd[1] * fwd[1]);
        hl = n > 1e-9 && std::isfinite(hn) && std::isfinite(n) ? hn / n : 0.0;
    }
    s.hl = hl;
    if (hl >= kBlendHiXz) return s;   // well defined and level-ish: the eye's heading, bit for bit
    const double w = std::min(1.0, std::max(0.0, (kBlendHiXz - hl) / (kBlendHiXz - kBlendLoXz)));
    const double ex = eyeH ? eyeH[0] : 0.0, ez = eyeH ? eyeH[2] : 1.0;
    const double dot = ez;   // the eye's heading against the body's forward (0, 0, 1)
    s.changed = true;
    if (!(dot >= 0.0)) {   // behind the body's: the view past vertical (a heading that is not a number reads here too)
        s.replaced = true;
        s.w = 1.0;
        s.h[0] = 0.0;
        s.h[2] = 1.0;
    } else {
        const double bx = (1.0 - w) * ex, bz = (1.0 - w) * ez + w;
        const double n = std::sqrt(bx * bx + bz * bz);
        s.w = w;
        s.blended = w > 0.0;
        s.h[0] = bx / n;
        s.h[2] = bz / n;
    }
    const double c = std::min(1.0, std::max(-1.0, ex * s.h[0] + ez * s.h[2]));
    s.angleDeg = std::acos(c) * 180.0 / 3.14159265358979323846;
    return s;
}

// The angle between two horizontal headings (x, z), deg, 0..180; not a number when either is not a heading.
inline double headingTurnDeg(const double a[2], const double b[2]) noexcept {
    const double an = std::sqrt(a[0] * a[0] + a[1] * a[1]), bn = std::sqrt(b[0] * b[0] + b[1] * b[1]);
    if (!(an > 1e-9) || !(bn > 1e-9)) return std::nan("");
    const double c = std::min(1.0, std::max(-1.0, (a[0] * b[0] + a[1] * b[1]) / (an * bn)));
    return std::acos(c) * 180.0 / 3.14159265358979323846;
}

// The window's figures (one writer, the pin hook): the pin calls with an eye, those blended, those replaced (past vertical), the largest angle between the eye's heading and the stable one, the
// aim's pitch range (game's, deg) and the eye's pitch range (deg).
struct Counters {
    uint32_t frames = 0, blended = 0, replaced = 0, aimN = 0, eyeN = 0;
    double angleMax = 0.0, aimMin = 1.0e9, aimMax = -1.0e9, eyeMax = -1.0e9;
    void add(const Info& i, double aimPitchDeg, double eyePitchDeg) noexcept {
        if (!i.valid) return;
        ++frames;
        if (i.replaced) ++replaced;
        else if (i.blended) ++blended;
        if (i.changed && std::isfinite(i.angleDeg)) angleMax = std::max(angleMax, i.angleDeg);
        if (std::isfinite(aimPitchDeg)) {
            ++aimN;
            aimMin = std::min(aimMin, aimPitchDeg);
            aimMax = std::max(aimMax, aimPitchDeg);
        }
        if (std::isfinite(eyePitchDeg)) {
            ++eyeN;
            eyeMax = std::max(eyeMax, eyePitchDeg);
        }
    }
    void merge(const Counters& o) noexcept {
        frames += o.frames; blended += o.blended; replaced += o.replaced; aimN += o.aimN; eyeN += o.eyeN;
        angleMax = std::max(angleMax, o.angleMax);
        aimMin = std::min(aimMin, o.aimMin);
        aimMax = std::max(aimMax, o.aimMax);
        eyeMax = std::max(eyeMax, o.eyeMax);
    }
};

// The window's line, printed for a window with frames (one with none blended still prints): the frames, those blended, those replaced (past vertical), the largest angle between the eye's heading and
// the stable one, the aim's pitch range and the eye's largest pitch. Under the log's 1200 bytes.
inline int formatLine(char* buf, size_t n, uint32_t seconds, const Counters& c) noexcept {
    return std::snprintf(buf, n,
                         "native on foot heading (F183) %u s: frames %u, blended %u, replaced %u (past vertical); the largest angle between the eye's heading and the stable one %.1f deg; "
                         "the aim pitch %+.1f to %+.1f deg, the eye's pitch up to %+.1f deg",
                         seconds, c.frames, c.blended, c.replaced, c.angleMax, c.aimN ? c.aimMin : 0.0, c.aimN ? c.aimMax : 0.0, c.eyeN ? c.eyeMax : 0.0);
}

}  // namespace f183
}  // namespace nofoot
}  // namespace edvr
