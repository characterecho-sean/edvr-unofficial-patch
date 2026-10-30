// F179 (native on foot VR): the body stays up in standing ADS (F179a), and the eye is kept ahead of the chest (F179b, the neck guard). Header only, no game or D3D11 state, so
// native_frame_test runs the very functions the pin and the frame thread use. Journal: the arc doc, F179. Behaviour, no keys.
//
// The flight edvr_gfx_20261030_095038 (build 466928ca, the plasma shotgun, standing): at the hip the armed body stood at 1.380 m (the animated neck 1.340: the armed raise +4 cm), the eye
// +18.0 cm over the clavicle line and +34 cm ahead of the chest. Aiming down sights, F178's dials at 0, the raise was released with the ADS detach (F97a: "released with the detach") and
// the body took the game's ADS hunch: the neck 1.290 m, animated 1.290, the eye +32.6 cm over the clavicle line (the legs hip to foot over the leg 0.895, nearly straight: the drop is the
// hunch, not a crouch). With F178's forward +14.5 and left +24.5 the body went under the eye (over the clavicles +31.3, ahead of the chest +6.6, min +5.5), and looking down showed the
// open neck ring (the head is hidden by zeroing its part masks, so the ring is open).
//
// ---- F179a: the body stays up in standing ADS
// With the STANDING share (F178's: (1 - F163's crouched weight) x the ADS detach weight) the neck target keeps a raise in the sights: the neck at the eye as drawn at the hip (the pin's eye
// plus the neck reference's raise: F97a's own eyeHipLevelY) less the eye height key (fix.native_on_foot_eye_height_cm), vertical only, a raise (never a lowering: F97a's lowering is the
// crouched share's, and stays as it was). It is the BODY's neck target (nofoot::f97::armedNeckTarget's y, the pin's T and the rest path's physicalTarget), made before the reach rule; the
// leg cap that follows it (legRaiseRoom: no foot lifted) limits it as it limits the hip's raise. It is not the eye alignment raise F138 and F172 publish for the camera, and it moves
// neither the eye, the sight lock nor the grips' targets. The standing share is 0 crouched and at the hip, so those are the earlier values bit for bit; unarmed (F88's weight 0) there is no raise.
//
// ---- F179b: the neck guard
// Whenever the full third-person body is drawn on foot, the eye as drawn sits at least kNeckGuardAheadM = 10 cm ahead of the chest (F97a's "ahead of the chest" measure: the eye as drawn,
// pc.f97EyeDrawn, less the chest joint 24, taken along the eye's horizontal heading, pc.hEye normalised: the forward axis of nofoot::headingAxes) after the writes. The chest's place is
// predicted from the body's horizontal move: the chest joint 24 as animated moves with the neck target (and the reach rule's correction and a positive dial's post-reach add), so the chest at
// the end is its animated place plus that move, and the guard acts in the same frame. A deficit found before the reach rule moves the target back along the heading before it (the grips stay
// reachable); one that only the post-reach add (F169, and F178's forward dial) would cause is taken off that add first; what is left after it, the reach rule's own, is taken off the move
// last (the guard has the last word). The prediction is a translation; the torso's bend and turns are not in it, so the error measured on the call before (measured less predicted, filtered)
// is added to the next prediction. The measured figure after the writes is the line's; "held" is read from it.
#pragma once

#include "native_on_foot_core.h"
#include "native_on_foot_f159.h"
#include "native_on_foot_f178.h"
#include "native_on_foot_f97.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f179 {

// ---- F179a ----------------------------------------------------------------------------------------------------------------------------------------------------------------------

// The standing raise asked, m (>= 0): the neck at the eye as drawn at the hip less the eye height key, over the animated neck, times the standing weight. 0 for any figure that is not a number,
// when the animated neck is already that high, crouched (the standing share 0) and at the hip (the detach weight 0).
inline double standingRaiseM(double n0y, double eyeHipLevelY, double eyeHeightM, double postureW, double detachW) noexcept {
    if (!std::isfinite(n0y) || !std::isfinite(eyeHipLevelY) || !std::isfinite(eyeHeightM)) return 0.0;
    const double high = (eyeHipLevelY - eyeHeightM) - n0y;
    const double raise = high > 0.0 ? high : 0.0;
    return raise * f178::weight(postureW, detachW);
}

// F97a's armed neck target with the standing raise added: the larger of the demand it asked and the standing raise, blended by F88's weight w as F97a's own y is. The base target as it came when the
// standing raise is 0, under the demand already asked, or w is 0 (unarmed).
inline f97::ArmedTarget withStandingRaise(const f97::ArmedTarget& base, double n0y, double w, double standingRaise) noexcept {
    f97::ArmedTarget o = base;
    const double wc = !(w > 0.0) ? 0.0 : w > 1.0 ? 1.0 : w;
    if (!std::isfinite(n0y) || wc == 0.0 || !(standingRaise > 0.0) || !(standingRaise > o.asked)) return o;
    o.asked = standingRaise;
    o.y = n0y + wc * o.asked;
    return o;
}

// ---- F179b ----------------------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr double kNeckGuardAheadCm = 10.0;                       // the least the eye stands ahead of the chest along its heading
constexpr double kNeckGuardAheadM = kNeckGuardAheadCm * 0.01;
constexpr double kHeldBelowCm = 9.0;                              // the health rule: a measured "ahead of the chest" under this after the guard is a guard that did not hold
constexpr double kMinHeadingXz = 0.5;                             // F97a's: a heading whose horizontal part is under this is not read
constexpr double kBiasGain = 0.5, kBiasLimitM = 0.5;              // the filtered prediction error: the share of each new error taken in, and its bound
constexpr uint32_t kMinFrames = 30;                               // the health rules: fewer frames than this are too few to judge

// The contexts the guard is reported in.
enum Ctx : int { kHipUnarmed = 0, kHipArmed = 1, kAdsStanding = 2, kAdsCrouched = 3, kCtxN = 4 };
inline const char* ctxName(int c) noexcept {
    return c == kHipUnarmed ? "hip unarmed" : c == kHipArmed ? "hip armed" : c == kAdsStanding ? "ADS standing" : "ADS crouched";
}
// armed: a weapon is drawn (the pin's armed flag); in the sights = not at the hip (the detach weight from 0.05); crouched = F163's weight from 0.5.
inline int contextOf(bool armed, double detachW, double postureW) noexcept {
    if (!f159::atHip(detachW)) return pinCrouchedWc(postureW) ? kAdsCrouched : kAdsStanding;
    return armed ? kHipArmed : kHipUnarmed;
}

// The eye, the chest and the heading the guard works with, in the model's axes. ok = all three are readable.
struct Geometry {
    bool ok = false;
    double eye[3] = {}, chest[3] = {}, h[3] = {};   // h: the horizontal unit of the eye's heading (x, 0, z)
    // The eye ahead of the chest (m, signed; along h) if the chest, as animated, were moved by th (x and z used).
    double aheadWith(const double th[3]) const noexcept {
        const double dx = eye[0] - (chest[0] + th[0]), dz = eye[2] - (chest[2] + th[2]);
        return dx * h[0] + dz * h[2];
    }
};
inline Geometry makeGeometry(bool eyeOk, const double eye[3], double hx, double hz, bool chestOk, const double chest[3]) noexcept {
    Geometry g;
    const double hn = std::sqrt(hx * hx + hz * hz);
    if (!eyeOk || !chestOk || !std::isfinite(hn) || !(hn > kMinHeadingXz)) return g;
    for (int i = 0; i < 3; ++i) {
        g.eye[i] = eye[i];
        g.chest[i] = chest[i];
        if (!std::isfinite(eye[i]) || !std::isfinite(chest[i])) return Geometry{};
    }
    g.h[0] = hx / hn;
    g.h[1] = 0.0;
    g.h[2] = hz / hn;
    g.ok = true;
    return g;
}

// The ahead of the chest the guard reads, m: not a number reads as no deficit.
inline double deficitM(double aheadM) noexcept { return std::isfinite(aheadM) && aheadM < kNeckGuardAheadM ? kNeckGuardAheadM - aheadM : 0.0; }

// A horizontal point or move taken back along the heading by amountM (the horizontal, x and z).
inline void pushBack(double v[3], const double h[3], double amountM) noexcept {
    v[0] -= h[0] * amountM;
    v[2] -= h[2] * amountM;
}

// The target before the reach rule: if the body moved to it (as pinMove makes the move from the neck n0) would leave the eye less than 10 cm ahead of the chest, the target is taken back along
// the heading by the deficit. bias: the filtered prediction error (m), added to the predicted figure. Returns the push (m, >= 0); 0 and t untouched when the geometry is not ok.
inline double guardTarget(const Geometry& g, const double n0[3], double bias, double t[3]) noexcept {
    if (!g.ok) return 0.0;
    const PinMove mv = pinMove(n0, t);
    const double d = deficitM(g.aheadWith(mv.th) + (std::isfinite(bias) ? bias : 0.0));
    if (!(d > 0.0)) return 0.0;
    pushBack(t, g.h, d);
    return d;
}

// The post-reach add taken back for a deficit: the part of the add along the heading is scaled down by what the deficit takes of it (all of it at most); what is left of the deficit is the
// residual, taken off the move last. scale 1, taken 0 and the whole deficit as the residual when the add does not go forward along the heading.
struct Trim {
    double scale = 1.0, takenM = 0.0, residualM = 0.0;
};
inline Trim trimPost(const double post[3], const double h[3], double deficit) noexcept {
    Trim t;
    if (!(deficit > 0.0)) return t;
    const double along = post[0] * h[0] + post[2] * h[2];
    if (std::isfinite(along) && along > 1e-9) {
        t.takenM = deficit < along ? deficit : along;
        t.scale = 1.0 - t.takenM / along;
    }
    t.residualM = deficit - t.takenM;
    return t;
}

// The filtered prediction error: bias moved toward the new error (measured less predicted, m) by the gain, held to the limit. A new error that is not a number leaves it.
inline double nextBias(double bias, double errorM) noexcept {
    if (!std::isfinite(errorM)) return bias;
    double b = bias + kBiasGain * (errorM - bias);
    b = b > kBiasLimitM ? kBiasLimitM : b < -kBiasLimitM ? -kBiasLimitM : b;
    return b;
}

// ---- the figures --------------------------------------------------------------------------------------------------------------------------------------------------------------

// One call the guard ran in (pushM: the whole push; offPostM: the part taken off the post-reach add; beforeM: the eye ahead of the chest as the pose and dials would have put it, predicted
// without the guard; predM: as predicted with it, before the filtered error).
struct Sample {
    int ctx = kHipUnarmed;
    double pushM = 0.0, offPostM = 0.0, beforeM = std::nan(""), predM = std::nan("");
};
struct Ctx1 {
    uint32_t frames = 0, pushed = 0, offPostN = 0, beforeN = 0, predN = 0, afterN = 0, under9 = 0;
    double pushSum = 0.0, pushMax = 0.0, offPostSum = 0.0, offPostMax = 0.0, beforeSum = 0.0, beforeMin = 1e9, predSum = 0.0, afterSum = 0.0, afterMin = 1e9;
};
struct Counters {
    Ctx1 c[kCtxN];
    static int clampCtx(int x) noexcept { return x < 0 ? 0 : x >= kCtxN ? kCtxN - 1 : x; }
    void addGuard(const Sample& s) noexcept {
        Ctx1& k = c[clampCtx(s.ctx)];
        ++k.frames;
        if (std::isfinite(s.pushM) && s.pushM > 0.0) {
            ++k.pushed;
            k.pushSum += s.pushM;
            k.pushMax = s.pushM > k.pushMax ? s.pushM : k.pushMax;
        }
        if (std::isfinite(s.offPostM) && s.offPostM > 0.0) {
            ++k.offPostN;
            k.offPostSum += s.offPostM;
            k.offPostMax = s.offPostM > k.offPostMax ? s.offPostM : k.offPostMax;
        }
        if (std::isfinite(s.beforeM)) {
            ++k.beforeN;
            k.beforeSum += s.beforeM;
            k.beforeMin = s.beforeM < k.beforeMin ? s.beforeM : k.beforeMin;
        }
        if (std::isfinite(s.predM)) {
            ++k.predN;
            k.predSum += s.predM;
        }
    }
    // The measured figure after the writes (m), for a call the guard ran in.
    void addMeasured(int ctx, double afterM) noexcept {
        if (!std::isfinite(afterM)) return;
        Ctx1& k = c[clampCtx(ctx)];
        ++k.afterN;
        k.afterSum += afterM;
        k.afterMin = afterM < k.afterMin ? afterM : k.afterMin;
        if (afterM * 100.0 < kHeldBelowCm) ++k.under9;
    }
    void merge(const Counters& o) noexcept {
        for (int i = 0; i < kCtxN; ++i) {
            Ctx1& d = c[i];
            const Ctx1& s = o.c[i];
            d.frames += s.frames; d.pushed += s.pushed; d.offPostN += s.offPostN; d.beforeN += s.beforeN; d.predN += s.predN; d.afterN += s.afterN; d.under9 += s.under9;
            d.pushSum += s.pushSum; d.offPostSum += s.offPostSum; d.beforeSum += s.beforeSum; d.predSum += s.predSum; d.afterSum += s.afterSum;
            d.pushMax = s.pushMax > d.pushMax ? s.pushMax : d.pushMax;
            d.offPostMax = s.offPostMax > d.offPostMax ? s.offPostMax : d.offPostMax;
            d.beforeMin = s.beforeMin < d.beforeMin ? s.beforeMin : d.beforeMin;
            d.afterMin = s.afterMin < d.afterMin ? s.afterMin : d.afterMin;
        }
    }
    uint32_t frames() const noexcept {
        uint32_t n = 0;
        for (int i = 0; i < kCtxN; ++i) n += c[i].frames;
        return n;
    }
};

// ---- the line -----------------------------------------------------------------------------------------------------------------------------------------------------------------

// One context's line (printed when it had frames; a window with no push still prints: the guard ran): the item; the frames; those pushed, and the push's mean (over the pushed) and largest (cm);
// the part taken off the post-reach dial add (frames, mean and largest, cm); the eye ahead of the chest before the guard (as the pose and dials would put it: predicted; min and mean), after
// it as measured (the frames it was measured on, min and mean), the prediction of that figure (mean), and the measured frames under 9 cm. cm throughout. Under the log's 1200 bytes.
inline int formatContext(char* buf, size_t n, uint32_t seconds, const char* item, int ctx, const Ctx1& k) noexcept {
    const auto mean = [](double sum, uint32_t cnt) noexcept { return cnt ? sum / static_cast<double>(cnt) : 0.0; };
    return std::snprintf(buf, n,
                         "native on foot neck guard (F179b) %u s: item %s; %s %u frames: pushed %u, the push mean %.1f cm (largest %.1f); off the post-reach dial add %u frames, mean %.1f cm (largest %.1f); "
                         "the eye ahead of the chest before the guard (as the pose and dials would put it) min %+.1f mean %+.1f cm, after it as measured (%u frames) min %+.1f mean %+.1f, predicted "
                         "after mean %+.1f, under %.0f cm %u",
                         seconds, item ? item : "none", ctxName(Counters::clampCtx(ctx)), k.frames, k.pushed, mean(k.pushSum, k.pushed) * 100.0, k.pushMax * 100.0, k.offPostN,
                         mean(k.offPostSum, k.offPostN) * 100.0, k.offPostMax * 100.0, k.beforeN ? k.beforeMin * 100.0 : 0.0, mean(k.beforeSum, k.beforeN) * 100.0, k.afterN,
                         k.afterN ? k.afterMin * 100.0 : 0.0, mean(k.afterSum, k.afterN) * 100.0, mean(k.predSum, k.predN) * 100.0, kHeldBelowCm, k.under9);
}

}  // namespace f179
}  // namespace nofoot
}  // namespace edvr
