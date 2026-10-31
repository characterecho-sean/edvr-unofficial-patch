// F194 (native on-foot VR): look to aim aims at the middle of the picture, and its eye height settles as mouse look's does. Header only, no game or D3D11 state, so native_frame_test runs the very
// functions the frame thread and the pin hook use. Journal: the arc doc, F194.
//
// The tester (Quest 3, rc.4, look to aim): the aim sits about 12.9 deg above where they look. The pilot, on his Quest 3 (Virtual Desktop): he let the mouse aim settle, looked at the reticle, turned look to
// aim on, and the view dropped; on his Pimax he sees neither. The headset's vertical frustum is not symmetric on a Quest 3 (up 0.9657, down 1.4281 as tangents) and is on a Pimax Crystal Super
// (1.2648 both): the middle of the picture is atan((up - down) / 2) = -13.0 deg from the pose's forward axis on the Quest and 0 on the Pimax. Look to aim took the pose's forward axis (the head's pitch
// from the aim, adsHeadAngles) as the direction to aim at: 13 deg above the middle of the picture on a Quest, level on a Pimax. Horizontally each eye's centre is off by mirror-image amounts, so the two
// eyes' mean (the head's) is 0: the correction is vertical only.
//
// Part 1. The ray from the head through the middle of the picture is the forward axis turned by that pitch about the head's own right axis (a rolled head turns about its own right axis, not about
// world X). centreOffset (native_on_foot_f109.h) takes it, when the key is on and the headset's frustum is known, as the reported head's offset from the aim at an engage (and F133's yaw re-centre
// reads it); the follower's head steps are unchanged (steps do not see a constant offset).
// Part 2. Look to aim refused the stance's settle (F98c: the only publisher of the raise that holds the view's height over the neck) whenever |the carried aim pitch| was 10 deg or more (F109, native_on_foot.cpp).
// That pitch is the account of what the game applied for EDVR's asks since the engage (AdsAimMap::pitchSeat), not the game's aim: with the mouse aim resting at -11 and a level head the engage's
// centring carries +11 and the settle was refused with the aim level; and with part 1 a level head on a Quest carries -13. Mouse look's gate is F139's 20 deg on the FP root's pitch, nothing drawn only
// (nofoot::f138::settleRefused), and it already runs after look to aim's for both modes. With the key on, look to aim's own gate is skipped and F139's covers it exactly as it covers mouse look.
#pragma once

#include "native_on_foot_core.h"   // adsHeadAngles
#include "native_on_foot_f138.h"   // settleRefused, kSettleGateDeg: F139's rule

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f194 {

constexpr double kDeg = kAdsDeg;
constexpr double kMaxPictureCentreDeg = 30.0;   // no headset's picture centre is further from its forward axis; a frustum that says more is a bad answer, not a headset

// ---- the headset's frustum: the middle of the picture ----------------------------------------------------------------------------------------------------------------------------------------

// The pitch of the middle of the picture from the forward axis (radians, up +): the centre of the tangent span, atan((up - down) / 2). 0 for a frustum that is not numbers, not positive, or implausible.
inline double pictureCentrePitchRad(double upTan, double downTan) noexcept {
    if (!std::isfinite(upTan) || !std::isfinite(downTan) || !(upTan > 0.0) || !(downTan > 0.0)) return 0.0;
    const double p = std::atan((upTan - downTan) * 0.5);
    return std::fabs(p) <= kMaxPictureCentreDeg * kDeg ? p : 0.0;
}

// The headset's vertical frustum as the channel (frame_flag.h eyeTangentsVertical) carries it. The channel's two words are OpenVR's pfTop and pfBottom MAGNITUDES, and those API names are reversed
// physically: pfBottom describes the +Y edge and pfTop the -Y edge (frame_flag.h; menu_panel.cpp does the same swap). So up = botMag and down = topMag.
struct Frustum {
    bool have = false;      // the channel answered
    bool usable = false;    // ... with a frustum the correction may use
    double upTan = 0.0, downTan = 0.0, pitchRad = 0.0;   // the physical tangents; the middle of the picture from the forward axis (0 unless usable)
};
inline Frustum frustumFromChannel(bool answered, double topMag, double botMag) noexcept {
    Frustum f;
    f.have = answered;
    if (!answered) return f;
    f.upTan = botMag;
    f.downTan = topMag;
    f.usable = std::isfinite(f.upTan) && std::isfinite(f.downTan) && f.upTan > 0.0 && f.downTan > 0.0 && std::fabs(std::atan((f.upTan - f.downTan) * 0.5)) <= kMaxPictureCentreDeg * kDeg;
    f.pitchRad = f.usable ? pictureCentrePitchRad(f.upTan, f.downTan) : 0.0;
    return f;
}
// Two frusta are the same one to the channel's resolution (a thousandth of a tangent).
inline bool sameFrustum(const Frustum& a, const Frustum& b) noexcept {
    return a.have == b.have && a.usable == b.usable && std::fabs(a.upTan - b.upTan) < 0.0006 && std::fabs(a.downTan - b.downTan) < 0.0006;
}
// The pitch the centring is given: the picture's centre when the key is on and the frustum is usable, else 0 (the forward axis, as before F194).
inline double pitchInForce(bool keyOn, const Frustum& f) noexcept { return keyOn && f.usable ? f.pitchRad : 0.0; }

// ---- the ray -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// The yaw (the seat's sign) and the pitch (nose up +) of the ray from the head through the middle of the picture, from a 3x4 row-major rigid transform in OpenXR axes (x right, y up, -z forward):
// the forward axis (minus the third column) turned by pictureRad about the head's own right axis, f' = cos(d) f + sin(d) u with u the head's up (the second column, made square to f). A pitch of
// exactly 0 is adsHeadAngles itself, bit for bit. False when degenerate or when the ray is within 11 deg of vertical (no heading there), as adsHeadAngles.
inline bool rayAngles(const float m[12], double pictureRad, double* yaw, double* pitch) noexcept {
    if (pictureRad == 0.0) return adsHeadAngles(m, yaw, pitch);
    if (!std::isfinite(pictureRad)) return false;
    double f[3] = {-static_cast<double>(m[2]), -static_cast<double>(m[6]), -static_cast<double>(m[10])};
    const double lf = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(lf > 1e-6)) return false;
    for (double& v : f) v /= lf;
    double u[3] = {static_cast<double>(m[1]), static_cast<double>(m[5]), static_cast<double>(m[9])};
    const double fu = f[0] * u[0] + f[1] * u[1] + f[2] * u[2];
    for (int i = 0; i < 3; ++i) u[i] -= fu * f[i];
    const double lu = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(lu > 1e-6)) return false;
    for (double& v : u) v /= lu;
    const double c = std::cos(pictureRad), s = std::sin(pictureRad);
    double r[3];
    for (int i = 0; i < 3; ++i) r[i] = c * f[i] + s * u[i];
    const double lr = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (!(lr > 1e-6)) return false;
    const double ry = r[1] / lr;
    if (std::fabs(ry) > 0.98) return false;
    *yaw = std::atan2(-r[0], -r[2]);
    *pitch = std::asin(ry);
    return true;
}

// ---- part 2: look to aim's settle gate -------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr double kLtaGateDeg = 10.0;   // F109's own gate: the settle is refused from this |carried aim pitch|
inline bool ltaGateWouldRefuse(double carriedAbsDeg) noexcept { return std::isfinite(carriedAbsDeg) && carriedAbsDeg >= kLtaGateDeg; }
// The gate as the hook applies it: skipped (asMouseLook) when the key is on; the call is counted either way by whoever asks ltaGateWouldRefuse.
inline bool ltaGateRefuses(bool asMouseLook, double carriedAbsDeg) noexcept { return !asMouseLook && ltaGateWouldRefuse(carriedAbsDeg); }
struct SettleVerdict {
    bool tenWould = false;     // look to aim's 10 deg gate on the carried pitch would refuse
    bool tenRefused = false;   // ... and it did (the key is off)
    bool f139Refused = false;  // F139's rule on the FP root's pitch refused (evaluated only when the 10 deg gate did not)
    bool refused() const noexcept { return tenRefused || f139Refused; }
};
// The settle's refusals in the hook's order. carriedAbsDeg: |AdsAimMap::pitchSeat|; rootPitchDeg: the FP root's pitch (nose up +; rootKnown false: not read); itemDrawn: armed or the weapon eye's weight up.
inline SettleVerdict settleVerdict(bool asMouseLook, double carriedAbsDeg, double rootPitchDeg, bool rootKnown, bool itemDrawn) noexcept {
    SettleVerdict v;
    v.tenWould = ltaGateWouldRefuse(carriedAbsDeg);
    v.tenRefused = ltaGateRefuses(asMouseLook, carriedAbsDeg);
    if (!v.tenRefused) v.f139Refused = rootKnown && nofoot::f138::settleRefused(rootPitchDeg, nofoot::f138::kSettleGateDeg, itemDrawn);
    return v;
}

// ---- the pin hook's window ----------------------------------------------------------------------------------------------------------------------------------------------------------------------

// One fresh pin call while look to aim drives the aim: the drawn eye over the animated neck (m, not a number: unknown), the raise published (m, not a number: none yet), and the settle's fate.
struct Call {
    double eyeOverNeckM = std::nan(""), raiseM = std::nan("");
    bool tenWould = false, tenRefused = false, f139Refused = false, taken = false;
};
struct Window {
    uint32_t calls = 0, eyeN = 0, raiseN = 0, tenWould = 0, tenRefused = 0, f139Refused = 0, taken = 0;
    double eyeSum = 0.0, raiseSum = 0.0;
    void add(const Call& c) noexcept {
        ++calls;
        if (std::isfinite(c.eyeOverNeckM)) {
            ++eyeN;
            eyeSum += c.eyeOverNeckM;
        }
        if (std::isfinite(c.raiseM)) {
            ++raiseN;
            raiseSum += c.raiseM;
        }
        if (c.tenWould) ++tenWould;
        if (c.tenRefused) ++tenRefused;
        if (c.f139Refused) ++f139Refused;
        if (c.taken) ++taken;
    }
    bool any() const noexcept { return calls > 0; }
    void clear() noexcept { *this = Window{}; }
};

// ---- the frame thread's second -------------------------------------------------------------------------------------------------------------------------------------------------------------------

// Per tick while look to aim is engaged: the headset's pitch in LOCAL (the physical head), the carried aim pitch (the account), the game's aim pitch (the FP root, when fresh), and the reported head's pitch from
// the aim for the forward axis and for the ray in force (degrees; the seat convention, nose up +).
struct Second {
    uint32_t ticks = 0, localN = 0, rootN = 0, fwdN = 0, rayN = 0;
    double localSum = 0.0, carriedSum = 0.0, rootSum = 0.0, fwdSum = 0.0, raySum = 0.0;
    void add(bool localOk, double localDeg, double carriedDeg, bool rootOk, double rootDeg, bool fwdOk, double fwdDeg, bool rayOk, double rayDeg) noexcept {
        ++ticks;
        carriedSum += std::isfinite(carriedDeg) ? carriedDeg : 0.0;
        if (localOk && std::isfinite(localDeg)) { ++localN; localSum += localDeg; }
        if (rootOk && std::isfinite(rootDeg)) { ++rootN; rootSum += rootDeg; }
        if (fwdOk && std::isfinite(fwdDeg)) { ++fwdN; fwdSum += fwdDeg; }
        if (rayOk && std::isfinite(rayDeg)) { ++rayN; raySum += rayDeg; }
    }
    bool any() const noexcept { return ticks > 0; }
    double mean(double sum, uint32_t n) const noexcept { return n ? sum / n : 0.0; }
};

// ---- the lines -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// When the frustum is first known (or first not), and on change, the line says where the middle of the picture is (the Quest's: 13.0 deg below the forward axis, with the up and down tangents) and whether the
// key is on. Before an answer, and for a frustum that cannot be used, it says the forward axis is what look to aim aims at. (tools\edvr_log.py reads these formats from this file: keep a quoted copy of a line out of
// the comments, where the reader's search would find it first.)
inline int formatFrustum(char* buf, size_t n, const Frustum& f, bool keyOn) noexcept {
    const char* key = keyOn ? "on" : "off";
    if (!f.have)
        return std::snprintf(buf, n, "native on foot look to aim (F194): no frustum from the headset yet: the forward axis; look to aim aims at the picture's centre: %s", key);
    if (!f.usable)
        return std::snprintf(buf, n, "native on foot look to aim (F194): the headset's frustum (up %.3f, down %.3f tangents) is not usable: the forward axis; look to aim aims at the picture's centre: %s",
                             f.upTan, f.downTan, key);
    const double deg = f.pitchRad / kDeg;
    if (std::fabs(deg) < 0.05)
        return std::snprintf(buf, n, "native on foot look to aim (F194): the picture's centre is on the headset's forward axis (up %.3f, down %.3f tangents); look to aim aims at it: %s", f.upTan, f.downTan, key);
    return std::snprintf(buf, n, "native on foot look to aim (F194): the picture's centre is %.1f deg %s the headset's forward axis (up %.3f, down %.3f tangents); look to aim aims at it: %s",
                         std::fabs(deg), deg < 0.0 ? "below" : "above", f.upTan, f.downTan, key);
}

// The 1 s line while look to aim is engaged. centreDeg: the picture-centre pitch in force (0: the forward axis); eyeHeightAsMouse: the key that skips the 10 deg gate.
inline int formatSecond(char* buf, size_t n, const Second& s, const Window& w, double centreDeg, bool keyOn, bool eyeHeightAsMouse) noexcept {
    const double ew = w.eyeN ? static_cast<double>(w.eyeN) : 1.0, rw = w.raiseN ? static_cast<double>(w.raiseN) : 1.0;
    return std::snprintf(buf, n,
                         "native on foot look to aim height (F194) 1 s: the picture-centre pitch in force %+.1f deg (look to aim aims at the picture's centre: %s); the headset's pitch (LOCAL) mean %+.1f deg; "
                         "carried aim pitch mean %+.1f deg (the game's aim %+.1f deg); the reported head's pitch from the aim mean: the forward axis %+.1f deg, the ray in force %+.1f deg; "
                         "the settle: look to aim's 10 deg gate %s %u of %u pin calls, F138/F139's 20 deg gate refused %u, taken %u; the published raise mean %+.1f cm; "
                         "the drawn eye over the animated neck mean %+.1f cm",
                         centreDeg, keyOn ? "on" : "off", s.mean(s.localSum, s.localN), s.mean(s.carriedSum, s.ticks), s.mean(s.rootSum, s.rootN), s.mean(s.fwdSum, s.fwdN),
                         s.mean(s.raySum, s.rayN), eyeHeightAsMouse ? "would have refused (skipped)" : "refused", eyeHeightAsMouse ? w.tenWould : w.tenRefused, w.calls, w.f139Refused, w.taken,
                         w.raiseN ? w.raiseSum / rw * 100.0 : 0.0, w.eyeN ? w.eyeSum / ew * 100.0 : 0.0);
}

// ---- the view's height at an engage and a disengage -------------------------------------------------------------------------------------------------------------------------------------------

// The pilot's "my view dropped", as a number. The view's height is the drawn eye's vertical in the character's model (pc.f97EyeDrawn[1]: the job camera with every add EDVR makes, the head's own motion
// included, in the model whose origin stands at the feet), read once a fresh game frame on foot at the hip with the commander's camera: for a still pilot, the frame that best stands still (against the
// ground the model is placed in; the sights and the camera away are not read). At each edge of look to aim one probe takes the frame before and the first frames at or after 0.5 s, 1 s and 3 s,
// and says the change and the carried aim pitch at each. A moment with no frame inside its grace reads "not read": an engage that moves nothing prints a small change, not nothing.
struct HeightProbe {
    static constexpr int kN = 3;
    static constexpr uint64_t kLateMs = 400;    // a frame read at most this long after its moment counts for it
    static constexpr uint64_t kFreshMs = 250;   // a frame this old, or less, is a fresh read
    static constexpr uint64_t kBeforeMs = 500;  // the frame before an edge: the last read inside this long
    bool active = false, engage = true, haveBefore = false, cut = false;
    uint64_t t0 = 0;
    double beforeY = std::nan(""), beforePitch = std::nan("");
    bool got[kN] = {};
    double y[kN] = {}, pitch[kN] = {};
    int next = 0;
    static uint64_t atMs(int i) noexcept { return i == 0 ? 500u : i == 1 ? 1000u : 3000u; }
    void begin(bool isEngage, uint64_t now, bool beforeOk, double beforeHeightM, double carriedDeg) noexcept {
        *this = HeightProbe{};
        active = true;
        engage = isEngage;
        t0 = now;
        haveBefore = beforeOk && std::isfinite(beforeHeightM);
        beforeY = haveBefore ? beforeHeightM : std::nan("");
        beforePitch = carriedDeg;
    }
    // One frame-thread tick: sampleOk, a fresh read (y metres) at this moment; carriedDeg the carried aim pitch. True when the last moment is resolved.
    bool tick(uint64_t now, bool sampleOk, double heightM, double carriedDeg) noexcept {
        if (!active) return false;
        while (next < kN) {
            const uint64_t due = t0 + atMs(next);
            if (now < due) break;
            if (sampleOk && std::isfinite(heightM) && now - due <= kLateMs) {
                got[next] = true;
                y[next] = heightM;
                pitch[next] = carriedDeg;
                ++next;
                break;   // one read serves one moment: a stall that left two due serves the second on the next tick, if it is still inside its grace
            }
            if (now - due > kLateMs) {
                ++next;
                continue;
            }
            break;   // inside the grace, no fresh read yet
        }
        return next >= kN;
    }
    // The next edge came first: what is not resolved reads "not read (cut short)".
    void cutShort() noexcept {
        cut = true;
        next = kN;
    }
    int format(char* buf, size_t n) const noexcept {
        char before[96], after[kN][128];
        if (haveBefore) std::snprintf(before, sizeof(before), "%.3f m, carried aim pitch %+.1f deg", beforeY, beforePitch);
        else std::snprintf(before, sizeof(before), "not read");
        for (int i = 0; i < kN; ++i) {
            if (!got[i]) std::snprintf(after[i], sizeof(after[i]), "%s", cut ? "not read (cut short by the next edge)" : "not read");
            else if (haveBefore) std::snprintf(after[i], sizeof(after[i]), "%+.1f cm (%.3f m), carried aim pitch %+.1f deg", (y[i] - beforeY) * 100.0, y[i], pitch[i]);
            else std::snprintf(after[i], sizeof(after[i]), "%.3f m (no frame before to change from), carried aim pitch %+.1f deg", y[i], pitch[i]);
        }
        return std::snprintf(buf, n,
                             "native on foot look to aim height (F194) %s: the view's height (the drawn eye above the character's origin, the hip frames read): before %s; after 0.5 s %s; after 1 s %s; after 3 s %s",
                             engage ? "engage" : "disengage", before, after[0], after[1], after[2]);
    }
};

}  // namespace f194
}  // namespace nofoot
}  // namespace edvr
