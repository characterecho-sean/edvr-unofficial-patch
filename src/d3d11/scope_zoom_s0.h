// Scope zoom, S0 (measure only, no key): the pure parts. Header only, no game, Windows or D3D11 state, so native_frame_test runs the very functions the camera hook, the
// flat tick and the VR tick use. The design and its open questions (Q1 to Q6): docs\design-vr-scope-magnification-2026-10-28.md, section 6.
//
// S0 changes nothing the player sees and writes four tagged lines (scope zoom (S0) flat, scope zoom (S0) vr, scope lens geometry (S0), scope lens census (S0)); each has a
// --health rule in tools\edvr_log.py, rendered in its self-test from the format literals below. What is here:
//   1. clipZoom: the projection scale and principal offset of an eye's scene rows (cb1 270..273), so a zoom is read from the very rows the draws use.
//   2. PressTracker: the hip value, the settled value and the ramp of one scalar (a FOV, a p11) across one press of the sights.
//   3. CameraTable: the vertical FOV and aspect of the cameras that reach the game's view-constant refresh, noted by the hook on any thread, read by the frame thread.
//   4. probePoints, lensEye: the lens centre c in an eye's pixels and the four pixel-probe points about it.
//   5. The line formats.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "pixel_probe.h"

namespace edvr {
namespace scopezoom {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = 180.0 / kPi;

constexpr double kStableRel = 1.0e-3;     // a value is stable while it stays within 0.1 % of its anchor
constexpr double kHipHoldS = 0.5;         // ... for this long, outside the sights, to be the hip
constexpr double kAdsHoldS = 0.6;         // ... for this long, in the sights, to be the settled value (F144's hold, native_on_foot_f144.h kFullHoldS)
// Flat only: Status.json is rewritten about once a second, so the ADS flag can arrive after the camera's ramp has finished. A hip committed from the ramp's own plateau within
// this long before the press edge is not the hip: the one before it is. The VR trackers are exact (the frame thread's own state) and take 0.
constexpr double kFlatLagGuardS = 3.0;

// A value is within the tolerance of another (relative, 0.1 %). Not named near: windows.h defines that.
inline bool withinTol(double a, double b) noexcept {
    if (!std::isfinite(a) || !std::isfinite(b)) return false;
    const double scale = std::fmax(std::fabs(a), std::fabs(b));
    return std::fabs(a - b) <= kStableRel * (scale > 0.0 ? scale : 1.0);
}

// ---- 1. the projection of an eye's scene rows ------------------------------------------------------------------------------------------------------------------------
// The rows are native_on_foot_core.h's worldClip layout: clip_j = x rows[j] + y rows[4 + j] + z rows[8 + j] + rows[12 + j]. With f the unit of the clip w column and n its
// length, clip x / clip w = p00 * (the tangent along the clip x column's part across f) + offX, and the same for y. p00 and p11 are 1 / tan of the half angles when the frustum
// is centred; offX and offY are the principal point (and the TAA jitter, a translation of the whole projection). The translation row does not enter: it is the eye's place.
struct ClipZoom {
    bool ok = false;
    double p00 = 0.0, p11 = 0.0;      // projection scale, horizontal and vertical
    double offX = 0.0, offY = 0.0;    // the optical axis in NDC
};
inline ClipZoom clipZoom(const float rows[16]) noexcept {
    ClipZoom z;
    if (!rows) return z;
    const double cw[3] = {rows[3], rows[7], rows[11]}, cx[3] = {rows[0], rows[4], rows[8]}, cy[3] = {rows[1], rows[5], rows[9]};
    const double n = std::sqrt(cw[0] * cw[0] + cw[1] * cw[1] + cw[2] * cw[2]);
    if (!std::isfinite(n) || !(n > 1.0e-12)) return z;
    const double f[3] = {cw[0] / n, cw[1] / n, cw[2] / n};
    const auto split = [&](const double c[3], double* scale, double* off) noexcept {
        const double along = c[0] * f[0] + c[1] * f[1] + c[2] * f[2];
        const double perp[3] = {c[0] - along * f[0], c[1] - along * f[1], c[2] - along * f[2]};
        *scale = std::sqrt(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]) / n;
        *off = along / n;
    };
    split(cx, &z.p00, &z.offX);
    split(cy, &z.p11, &z.offY);
    z.ok = std::isfinite(z.p00) && std::isfinite(z.p11) && z.p00 > 1.0e-9 && z.p11 > 1.0e-9 && std::isfinite(z.offX) && std::isfinite(z.offY);
    return z;
}
// The vertical field of view (radians) of a projection scale p11; NaN when p11 is not a positive number.
inline double fovFromP11(double p11) noexcept { return (std::isfinite(p11) && p11 > 0.0) ? 2.0 * std::atan(1.0 / p11) : kNaN; }
// The zoom between two states: p11_ads / p11_hip, or tan(hip / 2) / tan(ads / 2) from two vertical FOVs in radians (the factor the game enlarges the target by). NaN when an
// input is not usable; a FOV outside (0, pi) is not usable.
inline double zoomFromP11(double p11Hip, double p11Ads) noexcept {
    return (std::isfinite(p11Hip) && std::isfinite(p11Ads) && p11Hip > 0.0 && p11Ads > 0.0) ? p11Ads / p11Hip : kNaN;
}
inline double zoomFromFov(double hipRad, double adsRad) noexcept {
    if (!(std::isfinite(hipRad) && std::isfinite(adsRad) && hipRad > 0.0 && adsRad > 0.0 && hipRad < kPi && adsRad < kPi)) return kNaN;
    return std::tan(0.5 * hipRad) / std::tan(0.5 * adsRad);
}
// The tangents {L, R, D, U} (temporalPassEyeTan's order; L and D negative) the projection's NDC -1..1 spans: ndc = p * tangent + off.
inline bool tangentsOf(const ClipZoom& z, float tan4[4]) noexcept {
    if (!z.ok) return false;
    tan4[0] = static_cast<float>((-1.0 - z.offX) / z.p00);
    tan4[1] = static_cast<float>((1.0 - z.offX) / z.p00);
    tan4[2] = static_cast<float>((-1.0 - z.offY) / z.p11);
    tan4[3] = static_cast<float>((1.0 - z.offY) / z.p11);
    return true;
}

// ---- 2. one value across the presses of the sights -------------------------------------------------------------------------------------------------------------------
struct PressResult {
    bool valid = false;
    double hip = kNaN;            // the value before the press (NaN when none had been stable yet)
    double ads = kNaN;            // the settled value, else the last value of the press
    bool settled = false;         // the value held within 0.1 % for 0.6 s while the sights were up
    double rampS = 0.0;           // the settled run's start less the last time the value was within 0.1 % of the hip (0 when it never left the hip, or never settled)
    double holdS = 0.0;           // the press: the flag's rise to its fall
    double minV = kNaN, maxV = kNaN;
    uint32_t samples = 0, missing = 0;   // values read in the press, and frames with none
    bool hipFromBefore = false;   // the lag guard took the hip before the latest plateau
};

// Fed once per frame: feed(tSec, ads, value). value: a FOV, a p11, any scalar; NaN is "not available this frame". A press runs from the flag's rise to its fall.
//   hip     outside the sights, the value once it has stayed within 0.1 % of an anchor for 0.5 s. A ramp is never stable, so a flag that arrives after the ramp began still
//           finds the hip from before it; the lag guard (flat) also covers a flag that arrives after the ramp ended (see kFlatLagGuardS).
//   press   at the rise the hip is recorded; in the press the minimum and maximum, and the first run of values within 0.1 % of an anchor for 0.6 s (the settled value). A run
//           that is already under way at the rise and sits on the hip restarts at the rise (an unscoped press settles 0.6 s after its flag).
//   result  at the fall (feed returns true), or flush().
class PressTracker {
public:
    PressTracker() noexcept = default;
    explicit PressTracker(double lagGuardS) noexcept : lag_(lagGuardS) {}
    bool feed(double tSec, bool ads, double value) noexcept {
        bool ended = false;
        if (ads && !ads_) begin(tSec);
        else if (!ads && ads_) ended = end(tSec);
        ads_ = ads;
        if (std::isfinite(value)) sample(tSec, value);
        else if (ads_) ++cur_.missing;
        return ended;
    }
    // Ends a press that is still open (the source went away, or the session ended). True when one ended.
    bool flush(double tSec) noexcept {
        if (!ads_) return false;
        ads_ = false;
        return end(tSec);
    }
    const PressResult& result() const noexcept { return res_; }
    bool inPress() const noexcept { return ads_; }
    double hip() const noexcept { return hip_; }

private:
    struct Run {
        bool active = false;
        double anchor = 0.0, value = 0.0, startT = 0.0, nearHipT = kNaN;
    };
    void begin(double t) noexcept {
        cur_ = PressResult{};
        t0_ = t;
        double hip = hip_;
        if (lag_ > 0.0 && std::isfinite(hipPrev_) && std::isfinite(hipChangedT_) && t - hipChangedT_ < lag_) {
            hip = hipPrev_;
            cur_.hipFromBefore = true;
        }
        hip_ = hip;                 // the press runs against the hip it recorded; the return to it afterwards is no change
        hipPrev_ = hipChangedT_ = kNaN;
        cur_.hip = hip;
        settled_ = false;
        settledValue_ = settledStart_ = settledNear_ = last_ = kNaN;
        if (run_.active && std::isfinite(hip) && withinTol(run_.value, hip)) run_.active = false;
    }
    bool end(double t) noexcept {
        PressResult r = cur_;
        r.valid = true;
        r.holdS = t - t0_;
        if (settled_) {
            r.ads = settledValue_;
            r.settled = true;
            r.rampS = (std::isfinite(settledNear_) && std::isfinite(settledStart_)) ? std::fmax(0.0, settledStart_ - settledNear_) : 0.0;
        } else {
            r.ads = last_;
        }
        res_ = r;
        cur_ = PressResult{};
        fallT_ = t;                 // a run that spans the fall is the press's own plateau, never the hip
        return true;
    }
    void sample(double t, double v) noexcept {
        if (std::isfinite(hip_) && withinTol(v, hip_)) lastNearHipT_ = t;
        if (!run_.active || !withinTol(v, run_.anchor)) {
            run_ = Run{};
            run_.active = true;
            run_.anchor = v;
            run_.startT = t;
            run_.nearHipT = lastNearHipT_;
        }
        run_.value = v;
        const double dur = t - run_.startT;
        if (!ads_) {
            if (dur >= kHipHoldS && run_.startT >= fallT_) commitHip(t, v);
            return;
        }
        ++cur_.samples;
        cur_.minV = std::isfinite(cur_.minV) ? std::fmin(cur_.minV, v) : v;
        cur_.maxV = std::isfinite(cur_.maxV) ? std::fmax(cur_.maxV, v) : v;
        last_ = v;
        if (!settled_ && dur >= kAdsHoldS) {
            settled_ = true;
            settledValue_ = v;
            settledStart_ = run_.startT;
            settledNear_ = run_.nearHipT;
        }
    }
    void commitHip(double t, double v) noexcept {
        if (!std::isfinite(hip_)) { hip_ = v; return; }        // the first hip is no change
        if (withinTol(v, hip_)) { hip_ = v; return; }
        hipPrev_ = hip_;
        hip_ = v;
        hipChangedT_ = t;
    }

    double lag_ = 0.0;
    bool ads_ = false;
    double t0_ = 0.0;
    double hip_ = kNaN, hipPrev_ = kNaN, hipChangedT_ = kNaN, lastNearHipT_ = kNaN;
    double fallT_ = -std::numeric_limits<double>::infinity();
    Run run_;
    PressResult cur_, res_;
    bool settled_ = false;
    double settledValue_ = kNaN, settledStart_ = kNaN, settledNear_ = kNaN, last_ = kNaN;
};

// ---- 3. the cameras that reach the view-constant refresh -------------------------------------------------------------------------------------------------------------
// note() runs on any thread the game refreshes a camera on (the hook in flat_camera_inject.cpp, reads only); snapshot() runs on the frame thread. Relaxed atomics: a torn read
// of a slot's fields is acceptable for an instrument, and a camera that two threads first note together can take two slots until one is replaced.
class CameraTable {
public:
    static constexpr size_t kSlots = 16;           // the flat census's own table is 16 (FlatCameraCensus); the game refreshes more than 8 cameras a frame
    static constexpr uint64_t kStaleMs = 2000;     // a slot not noted for this long may be replaced
    struct Entry {
        uintptr_t camera = 0;
        uint32_t kind = 0, id = 0;
        float fov = 0.0f, aspect = 0.0f;
        uint64_t ms = 0, calls = 0;
    };
    void note(uintptr_t camera, uint32_t kind, float fovRad, float aspect, uint64_t nowMs) noexcept {
        if (!camera) return;
        for (size_t i = 0; i < kSlots; ++i)
            if (slot_[i].camera.load(std::memory_order_relaxed) == camera) { update(slot_[i], kind, fovRad, aspect, nowMs); return; }
        for (size_t i = 0; i < kSlots; ++i) {
            uintptr_t expected = 0;
            if (!slot_[i].camera.compare_exchange_strong(expected, camera, std::memory_order_relaxed)) continue;
            claimed(i, camera, kind, fovRad, aspect, nowMs);
            return;
        }
        size_t oldest = 0;
        uint64_t oldestMs = ~0ull;
        for (size_t i = 0; i < kSlots; ++i) {
            const uint64_t ms = slot_[i].ms.load(std::memory_order_relaxed);
            if (ms < oldestMs) { oldestMs = ms; oldest = i; }
        }
        if (nowMs >= oldestMs && nowMs - oldestMs > kStaleMs) {
            uintptr_t expected = slot_[oldest].camera.load(std::memory_order_relaxed);
            if (expected && slot_[oldest].camera.compare_exchange_strong(expected, camera, std::memory_order_relaxed)) {
                claimed(oldest, camera, kind, fovRad, aspect, nowMs);
                return;
            }
        }
        drops_.fetch_add(1, std::memory_order_relaxed);
    }
    // The live slots (a camera is held), up to max; returns how many were copied.
    size_t snapshot(Entry* out, size_t max) const noexcept {
        size_t n = 0;
        for (size_t i = 0; i < kSlots && n < max; ++i) {
            const Slot& s = slot_[i];
            Entry e;
            e.camera = s.camera.load(std::memory_order_relaxed);
            if (!e.camera) continue;
            e.kind = s.kind.load(std::memory_order_relaxed);
            e.id = s.id.load(std::memory_order_relaxed);
            e.fov = s.fov.load(std::memory_order_relaxed);
            e.aspect = s.aspect.load(std::memory_order_relaxed);
            e.ms = s.ms.load(std::memory_order_relaxed);
            e.calls = s.calls.load(std::memory_order_relaxed);
            out[n++] = e;
        }
        return n;
    }
    uint64_t drops() const noexcept { return drops_.load(std::memory_order_relaxed); }   // notes of a camera no slot could take
    void clear() noexcept {
        for (Slot& s : slot_) s.camera.store(0, std::memory_order_relaxed);
        drops_.store(0, std::memory_order_relaxed);
    }

private:
    struct Slot {
        std::atomic<uintptr_t> camera{0};
        std::atomic<uint32_t> kind{0}, id{0};
        std::atomic<float> fov{0.0f}, aspect{0.0f};
        std::atomic<uint64_t> ms{0}, calls{0};
    };
    static void update(Slot& s, uint32_t kind, float fov, float aspect, uint64_t nowMs) noexcept {
        if (s.kind.load(std::memory_order_relaxed) != kind) s.kind.store(kind, std::memory_order_relaxed);
        if (s.fov.load(std::memory_order_relaxed) != fov) s.fov.store(fov, std::memory_order_relaxed);
        if (s.aspect.load(std::memory_order_relaxed) != aspect) s.aspect.store(aspect, std::memory_order_relaxed);
        if (s.ms.load(std::memory_order_relaxed) != nowMs) s.ms.store(nowMs, std::memory_order_relaxed);
        s.calls.fetch_add(1, std::memory_order_relaxed);
    }
    void claimed(size_t i, uintptr_t camera, uint32_t kind, float fov, float aspect, uint64_t nowMs) noexcept {
        // two threads that first noted one camera together hold two slots: the later slot gives its claim back
        for (size_t j = 0; j < i; ++j)
            if (slot_[j].camera.load(std::memory_order_relaxed) == camera) {
                slot_[i].camera.store(0, std::memory_order_relaxed);
                update(slot_[j], kind, fov, aspect, nowMs);
                return;
            }
        Slot& s = slot_[i];
        s.id.store(nextId_.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
        s.calls.store(0, std::memory_order_relaxed);
        s.kind.store(kind, std::memory_order_relaxed);
        s.fov.store(fov, std::memory_order_relaxed);
        s.aspect.store(aspect, std::memory_order_relaxed);
        s.ms.store(nowMs, std::memory_order_relaxed);
        s.calls.fetch_add(1, std::memory_order_relaxed);
    }
    Slot slot_[kSlots];
    std::atomic<uint32_t> nextId_{1};
    std::atomic<uint64_t> drops_{0};
};
// The one table: the camera hook notes into it, the frame thread reads it.
inline CameraTable g_cameraTable;

// ---- 4. the lens centre and the pixel probe's points ----------------------------------------------------------------------------------------------------------------
// c is a point's NDC in an eye's scene rows (x right, y up). The eye's target is read with the origin at its top left: a texel is (x, y) = ((ndc x + 1) / 2 * width,
// (1 - ndc y) / 2 * height), the mapping native_on_foot_f85.h's marchSample uses on the very rows. The pixel probe's points are the same fractions of the target (0..1, y down;
// its window origin is the texture's top row, pixel_probe.cpp windowOrigin).
struct LensEye {
    bool ok = false;
    double ndc[2] = {kNaN, kNaN};
    double px[2] = {kNaN, kNaN};      // NaN when the target's size is not known
    uint32_t w = 0, h = 0;
    double offAxisDeg = kNaN;         // the angle of c's ray from the eye's optical axis, from the tangents
};
inline bool tangentsUsable(const float t[4]) noexcept {
    return t && std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2]) && std::isfinite(t[3]) && t[1] - t[0] > 1.0e-6f && t[3] - t[2] > 1.0e-6f;
}
// The tangent of the ray through an NDC point, from the tangents {L, R, D, U} the NDC square spans.
inline void tangentOfNdc(const double ndc[2], const float t[4], double out[2]) noexcept {
    out[0] = (ndc[0] * (static_cast<double>(t[1]) - t[0]) + (static_cast<double>(t[1]) + t[0])) * 0.5;
    out[1] = (ndc[1] * (static_cast<double>(t[3]) - t[2]) + (static_cast<double>(t[3]) + t[2])) * 0.5;
}
inline void ndcOfTangent(const double tg[2], const float t[4], double out[2]) noexcept {
    out[0] = (2.0 * tg[0] - (static_cast<double>(t[1]) + t[0])) / (static_cast<double>(t[1]) - t[0]);
    out[1] = (2.0 * tg[1] - (static_cast<double>(t[3]) + t[2])) / (static_cast<double>(t[3]) - t[2]);
}
inline LensEye lensEye(const double ndc[2], const float tan4[4], uint32_t w, uint32_t h) noexcept {
    LensEye e;
    if (!ndc || !std::isfinite(ndc[0]) || !std::isfinite(ndc[1])) return e;
    e.ndc[0] = ndc[0];
    e.ndc[1] = ndc[1];
    e.w = w;
    e.h = h;
    if (w && h) {
        e.px[0] = (ndc[0] + 1.0) * 0.5 * static_cast<double>(w);
        e.px[1] = (1.0 - ndc[1]) * 0.5 * static_cast<double>(h);
    }
    if (tangentsUsable(tan4)) {
        double tg[2];
        tangentOfNdc(ndc, tan4, tg);
        e.offAxisDeg = std::atan(std::sqrt(tg[0] * tg[0] + tg[1] * tg[1])) * kDeg;
    }
    e.ok = true;
    return e;
}

constexpr uint32_t kProbePoints = 4;
// The angles named by the census line, and each point's direction in the eye's own right/up plane: c; 0.9 deg straight below (where the scope's chevron sits); 1.5 deg above (glass
// only); 4 deg beside, to the right (the frame).
constexpr double kProbeAnglesDeg[kProbePoints] = {0.0, 0.9, 1.5, 4.0};
constexpr double kProbeDirX[kProbePoints] = {0.0, 0.0, 0.0, 1.0};
constexpr double kProbeDirY[kProbePoints] = {0.0, -1.0, 1.0, 0.0};
struct ProbeSet {
    uint32_t n = 0;
    PixelProbePoint pt[kProbePoints];
    double ndc[kProbePoints][2] = {};
    uint32_t clamped = 0;             // bit i: point i fell outside the eye's target and was moved to its edge
};
// c's NDC and the eye's tangents {L, R, D, U} to the four points. A point's offset is a plane angle on each axis from c's ray (atan of the tangent plus the angle), the angular
// measure native_on_foot_core.h's ndcAngles uses. False when c or the tangents are unusable.
inline bool probePoints(const double cNdc[2], const float tan4[4], ProbeSet* out) noexcept {
    if (!out || !cNdc || !std::isfinite(cNdc[0]) || !std::isfinite(cNdc[1]) || !tangentsUsable(tan4)) return false;
    ProbeSet set;
    double tc[2];
    tangentOfNdc(cNdc, tan4, tc);
    for (uint32_t i = 0; i < kProbePoints; ++i) {
        const double a = kProbeAnglesDeg[i] / kDeg;
        const double tg[2] = {std::tan(std::atan(tc[0]) + a * kProbeDirX[i]), std::tan(std::atan(tc[1]) + a * kProbeDirY[i])};
        double nd[2];
        ndcOfTangent(tg, tan4, nd);
        if (!std::isfinite(nd[0]) || !std::isfinite(nd[1])) return false;
        set.ndc[i][0] = nd[0];
        set.ndc[i][1] = nd[1];
        double x = (nd[0] + 1.0) * 0.5, y = (1.0 - nd[1]) * 0.5;
        if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) set.clamped |= 1u << i;
        x = x < 0.0 ? 0.0 : x > 1.0 ? 1.0 : x;
        y = y < 0.0 ? 0.0 : y > 1.0 ? 1.0 : y;
        set.pt[i].x = static_cast<float>(x);
        set.pt[i].y = static_cast<float>(y);
    }
    set.n = kProbePoints;
    *out = set;
    return true;
}

// ---- 5. the lines ------------------------------------------------------------------------------------------------------------------------------------------------------------
// Each is written at most the log's line size (note() holds 1200 bytes); callers pass a buffer of 1100. A number that is not available is the word none, so a rule can read it.
inline const char* numText(double v, int decimals, char* buf, size_t cap) noexcept {
    if (std::isfinite(v)) std::snprintf(buf, cap, "%.*f", decimals, v);
    else std::snprintf(buf, cap, "none");
    return buf;
}
inline void appendf(char* buf, size_t n, int* len, const char* fmt, ...) noexcept {
    if (*len < 0 || static_cast<size_t>(*len) >= n) return;
    va_list ap;
    va_start(ap, fmt);
    const int add = std::vsnprintf(buf + *len, n - static_cast<size_t>(*len), fmt, ap);
    va_end(ap);
    if (add > 0) *len += add;
}
inline int finishLine(char* buf, size_t n, int len) noexcept {
    if (!buf || !n) return 0;
    if (len < 0) len = 0;
    if (static_cast<size_t>(len) >= n) len = static_cast<int>(n) - 1;
    buf[len] = '\0';
    return len;
}
inline const char* itemText(const char* item) noexcept { return item && item[0] ? item : "none"; }
inline double degOf(double rad) noexcept { return std::isfinite(rad) ? rad * kDeg : kNaN; }

// A camera's press, for the flat line and the VR line: the values are the camera's vertical FOV in radians.
struct CamPress {
    uint32_t id = 0;
    uintptr_t camera = 0;
    uint32_t kind = 0;
    double aspect = kNaN;
    PressResult r;
};
// Its FOV moved in the press: the hip and the settled value differ, or the values ranged.
inline bool camChanged(const PressResult& r) noexcept {
    return (std::isfinite(r.hip) && std::isfinite(r.ads) && !withinTol(r.hip, r.ads)) || (std::isfinite(r.minV) && std::isfinite(r.maxV) && !withinTol(r.minV, r.maxV));
}

// scope zoom (S0) flat: the one-time announce. hook: "running", or why it is not.
inline int formatFlatWatching(char* buf, size_t n, const char* hook) noexcept {
    int len = 0;
    appendf(buf, n, &len,
            "scope zoom (S0) flat: watching (measure only, no key): each ADS press (the journal's flag, on foot) logs every live camera's vertical FOV at the hip and settled in "
            "ADS; the FOV source is the view-constant refresh hook (%s).",
            hook ? hook : "unknown");
    return finishLine(buf, n, len);
}
// scope zoom (S0) flat: the journal's flags first readable (said once; without it a log with no press cannot tell "no press" from "the flag was never seen").
inline int formatFlatFlags(char* buf, size_t n, bool onFoot, bool ads) noexcept {
    int len = 0;
    appendf(buf, n, &len, "scope zoom (S0) flat: the journal's flags are readable from Status.json (on foot %d, ADS %d); a press is the ADS flag with on foot set.", onFoot ? 1 : 0,
            ads ? 1 : 0);
    return finishLine(buf, n, len);
}
inline void appendFlatCam(char* buf, size_t n, int* len, const CamPress& c) noexcept {
    char asp[24], hip[24], ads[24], m[24], mn[24], mx[24];
    const double hipDeg = degOf(c.r.hip), adsDeg = degOf(c.r.ads);
    appendf(buf, n, len, "cam %u at 0x%llX kind %u aspect %s hip %s deg ads %s deg M %s ramp %.2f s settled %d (min %s max %s deg)", c.id, static_cast<unsigned long long>(c.camera),
            c.kind, numText(c.aspect, 4, asp, sizeof(asp)), numText(hipDeg, 3, hip, sizeof(hip)), numText(adsDeg, 3, ads, sizeof(ads)),
            numText(zoomFromFov(c.r.hip, c.r.ads), 4, m, sizeof(m)), c.r.rampS, c.r.settled ? 1 : 0, numText(degOf(c.r.minV), 3, mn, sizeof(mn)),
            numText(degOf(c.r.maxV), 3, mx, sizeof(mx)));
}
// scope zoom (S0) flat: one press, every camera the hook saw during it (cams), the ones whose FOV moved first, as many as the line holds.
inline int formatFlatPress(char* buf, size_t n, uint32_t press, const char* item, double holdS, const CamPress* cams, size_t count) noexcept {
    int len = 0;
    appendf(buf, n, &len, "scope zoom (S0) flat: press %u item %s hold %.2f s; ", press, itemText(item), holdS);
    if (!count) {
        appendf(buf, n, &len, "cameras none (the refresh hook noted no camera during the press)");
        return finishLine(buf, n, len);
    }
    size_t listed = 0, unlisted = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < count; ++i) {
            if (camChanged(cams[i].r) != (pass == 0)) continue;
            const int before = len;
            if (listed) appendf(buf, n, &len, "; ");
            appendFlatCam(buf, n, &len, cams[i]);
            if (len < 0 || static_cast<size_t>(len) + 90 >= n) {   // the line is full: leave this one and the rest for the count
                len = before;
                ++unlisted;
                continue;
            }
            ++listed;
        }
    }
    if (unlisted) appendf(buf, n, &len, "; %u more camera(s) not listed", static_cast<unsigned>(unlisted));
    return finishLine(buf, n, len);
}

// scope zoom (S0) vr: the one-time announce, written when native on foot first arms (the VR tick is running). Without it --health cannot tell a build that has no S0 from an S0 tick
// that never ran, and a log of an earlier build would read as the second.
inline int formatVrWatching(char* buf, size_t n) noexcept {
    int len = 0;
    appendf(buf, n, &len,
            "scope zoom (S0) vr: watching (measure only, no key): each press of the sights in native on foot logs the game's ADS fraction w, the readout's two camera FOVs, the eyes' "
            "projection scale (a control) and the VR camera hook's cameras; the press's first settled frame logs the lens geometry and, for the first three scoped presses and the first "
            "unscoped one, a pixel probe census.");
    return finishLine(buf, n, len);
}

// A tracked source for the VR line: "none" when it never had a value, else its hip and its value in the press.
inline void appendSource(char* buf, size_t n, int* len, const PressResult& r, int decimals, double scale) noexcept {
    char hip[24], ads[24];
    if (!std::isfinite(r.hip) && !std::isfinite(r.ads)) {
        appendf(buf, n, len, "none");
        return;
    }
    appendf(buf, n, len, "hip %s ads %s", numText(std::isfinite(r.hip) ? r.hip * scale : kNaN, decimals, hip, sizeof(hip)),
            numText(std::isfinite(r.ads) ? r.ads * scale : kNaN, decimals, ads, sizeof(ads)));
}
struct VrPress {
    uint32_t press = 0;
    const char* item = "none";
    const char* scope = "unknown";
    uint32_t copy = 0;
    double holdS = 0.0;
    double wMax = kNaN, wFullS = 0.0;
    bool settled = false;
    PressResult rc16C, rc17C, eyeL, eyeR;
};
// scope zoom (S0) vr: one press. The RC fields are the readout's two camera FOVs, the eyes' p11 the control (the scene rows should not move), cams the VR camera hook's.
inline int formatVrPress(char* buf, size_t n, const VrPress& p, const CamPress* cams, size_t count) noexcept {
    int len = 0;
    char wmax[24];
    appendf(buf, n, &len, "scope zoom (S0) vr: press %u item %s scope %s copy %u hold %.2f s w_max %s w_full %.2f s settled %d", p.press, itemText(p.item), p.scope ? p.scope : "unknown",
            p.copy, p.holdS, numText(p.wMax, 3, wmax, sizeof(wmax)), p.wFullS, p.settled ? 1 : 0);
    appendf(buf, n, &len, "; rc16C ");
    appendSource(buf, n, &len, p.rc16C, 4, 1.0);
    appendf(buf, n, &len, "; rc17C ");
    appendSource(buf, n, &len, p.rc17C, 4, 1.0);
    appendf(buf, n, &len, "; eyeL p11 ");
    appendSource(buf, n, &len, p.eyeL, 4, 1.0);
    appendf(buf, n, &len, "; eyeR p11 ");
    appendSource(buf, n, &len, p.eyeR, 4, 1.0);
    if (!count) {
        appendf(buf, n, &len, "; cameras none");
        return finishLine(buf, n, len);
    }
    size_t unlisted = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < count; ++i) {
            if (camChanged(cams[i].r) != (pass == 0)) continue;
            const int before = len;
            char hip[24], ads[24];
            appendf(buf, n, &len, "; cam %u at 0x%llX kind %u hip %s ads %s deg", cams[i].id, static_cast<unsigned long long>(cams[i].camera), cams[i].kind,
                    numText(degOf(cams[i].r.hip), 3, hip, sizeof(hip)), numText(degOf(cams[i].r.ads), 3, ads, sizeof(ads)));
            if (len < 0 || static_cast<size_t>(len) + 90 >= n) { len = before; ++unlisted; }
        }
    }
    if (unlisted) appendf(buf, n, &len, "; %u more camera(s) not listed", static_cast<unsigned>(unlisted));
    return finishLine(buf, n, len);
}

inline void appendLensEye(char* buf, size_t n, int* len, const LensEye& e, const char* why) noexcept {
    if (!e.ok) {
        appendf(buf, n, len, "c none (%s)", why && why[0] ? why : "no rows");
        return;
    }
    char pxx[24], pxy[24], off[24];
    appendf(buf, n, len, "c ndc (%.4f, %.4f) px (%s, %s) of %ux%u off-axis %s deg", e.ndc[0], e.ndc[1], numText(e.px[0], 1, pxx, sizeof(pxx)), numText(e.px[1], 1, pxy, sizeof(pxy)),
            e.w, e.h, numText(e.offAxisDeg, 2, off, sizeof(off)));
}
// scope lens geometry (S0): the sight point S as the lens centre c in each eye, once at the first settled frame of a press. eMinusSCm / eyeRToSCm: NaN when unknown.
inline int formatGeometry(char* buf, size_t n, uint32_t press, const char* item, const char* scope, const LensEye& l, const char* whyL, const LensEye& r, const char* whyR,
                          double eMinusSCm, double eyeRToSCm) noexcept {
    int len = 0;
    char es[24], rs[24];
    appendf(buf, n, &len, "scope lens geometry (S0): press %u item %s scope %s; eye L ", press, itemText(item), scope ? scope : "unknown");
    appendLensEye(buf, n, &len, l, whyL);
    appendf(buf, n, &len, "; eye R ");
    appendLensEye(buf, n, &len, r, whyR);
    appendf(buf, n, &len, "; game |E-S| %s cm; eye R to S %s cm", numText(eMinusSCm, 2, es, sizeof(es)), numText(eyeRToSCm, 2, rs, sizeof(rs)));
    return finishLine(buf, n, len);
}

inline void clampText(uint32_t mask, char* buf, size_t cap) noexcept {
    if (!mask) { std::snprintf(buf, cap, "none"); return; }
    size_t at = 0;
    for (uint32_t i = 0; i < kProbePoints && at + 4 < cap; ++i)
        if (mask & (1u << i)) at += static_cast<size_t>(std::snprintf(buf + at, cap - at, "%s%u", at ? "," : "", i));
    if (at == 0) std::snprintf(buf, cap, "none");
}
// scope lens census (S0): the pixel probe was configured with the four points about c and armed; its own lines follow. first: the first run says the points stay. geometryEye: the eye
// whose scene rows the points were worked out from (0 left, 1 right); probeEye: the eye the pixel probe's lines are about (its first-seen eye, 0). Each is printed with its physical
// eye, so a probe that watches another eye than the one the points came from reads as such.
inline const char* physicalEye(uint32_t eye) noexcept { return eye == 0 ? "L" : "R"; }
inline int formatCensusRun(char* buf, size_t n, uint32_t run, uint32_t press, const char* item, const char* scope, const ProbeSet& set, bool first, uint32_t geometryEye = 0,
                           uint32_t probeEye = 0) noexcept {
    int len = 0;
    char list[160] = {0}, clamp[24];
    size_t at = 0;
    for (uint32_t i = 0; i < set.n && at + 24 < sizeof(list); ++i)
        at += static_cast<size_t>(std::snprintf(list + at, sizeof(list) - at, "%s%.3f,%.3f", i ? ";" : "", static_cast<double>(set.pt[i].x), static_cast<double>(set.pt[i].y)));
    clampText(set.clamped, clamp, sizeof(clamp));
    appendf(buf, n, &len,
            "scope lens census (S0): run %u press %u item %s scope %s eye %s points %s at 0, 0.9 below, 1.5 above, 4 beside deg about c (x,y in 0..1 of the eye's target, y down; offsets are "
            "plane angles from c's ray in the eye's scene rows; clamped to the target: %s); probe eye %u is physical eye %s; the pixel probe's lines follow",
            run, press, itemText(item), scope ? scope : "unknown", physicalEye(geometryEye), list, clamp, probeEye, physicalEye(probeEye));
    if (first) appendf(buf, n, &len, "; the probe keeps these points afterwards, so an eye dump taken later probes the same places (until the config reloads)");
    return finishLine(buf, n, len);
}
inline int formatCensusSkip(char* buf, size_t n, const char* why) noexcept {
    int len = 0;
    appendf(buf, n, &len, "scope lens census (S0): run skipped (%s)", why ? why : "unknown");
    return finishLine(buf, n, len);
}

}  // namespace scopezoom
}  // namespace edvr
