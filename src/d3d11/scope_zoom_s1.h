// Scope zoom, A1: the pure parts of the lens (docs\design-vr-scope-magnification-2026-10-28.md, section 8). Header only, no game, Windows or D3D11 state, so native_frame_test runs the
// very functions the frame thread's tick, the door's passes and the GPU rig use.
//
// A1 shows the game's zoom inside the scope's lens: after the upscaler and RCAS, the aiming eye's picture inside the lens is rewritten as the picture magnified by M about the sight
// point c. What is here:
//   1. the table of the game's ADS FOV per (item, scope) and M = tan(h / 2) / tan(ads / 2) against the reference FOV h;
//   2. the gate (when there is a lens) and the ease (M from 1 to its value over 0.25 s);
//   3. the lens as a function: the tangent plane of an eye, the 128 rays over the depth, the open-sight rule, r(theta), the mask, the source of a magnified pixel, the window of the
//      picture the passes touch, the layout of the buffer the first pass fills and the second reads, a reference march the GPU rig compares the shader with;
//   4. the instrument's accumulators and its two lines (the announce, one line a press), which tools\edvr_log.py renders its --health fixtures from.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "scope_zoom_s0.h"

namespace edvr {
namespace scopezoom {

// ---- 1. the table ------------------------------------------------------------------------------------------------------------------------------------------------------------
constexpr double kLensMinM = 1.05;               // a lens that would magnify less only costs detail
constexpr double kLensEaseS = 0.25;              // M eases from 1 to its value over this long, from the first qualifying frame
constexpr uint64_t kLensFreshUs = 50000;         // the door takes a c no older than this
constexpr double kLensRefDefaultDeg = 56.3;      // the reference FOV h of experimental.native_on_foot_ads_scope_zoom_fov, degrees
constexpr double kLensRefMinDeg = 30.0, kLensRefMaxDeg = 90.0;

// The scope states as loadout::Scope numbers them (journal_loadout.h; native_on_foot.cpp holds the static_assert).
constexpr int kLensScopeUnknown = 0, kLensScopeNo = 1, kLensScopeYes = 2;

// The ADS FOV the game settles at per item and scope, degrees, from the flat flights (docs\design-vr-scope-magnification-2026-10-28.md, section 7: absolute per weapon, the same at
// HumanoidFOV 56.327 and 64.752). The table holds optical sights only (A2, section 9): a copy with an iron, reflex or holo sight has no entry and no lens. 0 is no value. An item
// matches on its stem, so Status.json's "$wpn_m_sniper_plasma_charged_name;" and the journal's "wpn_m_sniper_plasma_charged" are one.
struct LensEntry {
    const char* stem;
    double bareDeg;      // the copy without a scope, when its own sight is optical; 0: not an optical sight (the unscoped Eclipse)
    double scopedDeg;    // the copy with a scope, measured; 0: unmeasured
    double standInDeg;   // an unmeasured scoped copy takes this: its bare copy's FOV (the Eclipse's scope is the Aphelion's, and its bare copy is not optical, so the bare ADS FOV measured)
};
constexpr LensEntry kLensTable[] = {
    {"m_sniper_plasma_charged", 35.0, 15.0, 0.0},       // M Executioner
    {"m_assaultrifle_plasma_fauto", 48.0, 0.0, 48.0},   // M Oppressor
    {"m_assaultrifle_laser_fauto", 48.0, 0.0, 48.0},    // TK Aphelion: the base sight is the ring scope
    {"m_submachinegun_laser_fauto", 0.0, 0.0, 50.0},    // TK Eclipse: scoped only
};
constexpr size_t kLensTableItems = sizeof(kLensTable) / sizeof(kLensTable[0]);
// The (item, scope) entries the table holds: the number the announce line states.
inline uint32_t lensTableEntries() noexcept {
    uint32_t n = 0;
    for (const LensEntry& e : kLensTable) n += (e.bareDeg > 0.0 ? 1u : 0u) + (e.scopedDeg > 0.0 || e.standInDeg > 0.0 ? 1u : 0u);
    return n;
}

inline char lensLower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
// The stem of an item name: the leading "$" and "wpn_" and the trailing ";" and "_name" taken off, lower case. False for an empty name or one with no stem left.
inline bool lensStem(const char* name, char* out, size_t cap) noexcept {
    if (!out || cap < 2) return false;
    out[0] = '\0';
    if (!name || !name[0]) return false;
    const char* p = name;
    if (*p == '$') ++p;
    if (lensLower(p[0]) == 'w' && lensLower(p[1]) == 'p' && lensLower(p[2]) == 'n' && p[3] == '_') p += 4;
    size_t n = std::strlen(p);
    if (n && p[n - 1] == ';') --n;
    if (n >= 5 && lensLower(p[n - 5]) == '_' && lensLower(p[n - 4]) == 'n' && lensLower(p[n - 3]) == 'a' && lensLower(p[n - 2]) == 'm' && lensLower(p[n - 1]) == 'e') n -= 5;
    if (!n || n >= cap) return false;
    for (size_t i = 0; i < n; ++i) out[i] = lensLower(p[i]);
    out[n] = '\0';
    return true;
}
// The game's ADS FOV (degrees) for the held item and its scope state; false when the pair is not an optical sight (not in the table). An unknown scope state is not an optical sight.
// *fromBare is set when the FOV is the stand-in of an unmeasured scoped copy.
inline bool lensAdsDeg(const char* itemName, int scope, double* deg, bool* fromBare = nullptr) noexcept {
    if (deg) *deg = kNaN;
    if (fromBare) *fromBare = false;
    if (scope != kLensScopeNo && scope != kLensScopeYes) return false;
    char stem[64];
    if (!lensStem(itemName, stem, sizeof(stem))) return false;
    for (const LensEntry& e : kLensTable) {
        if (std::strcmp(stem, e.stem) != 0) continue;
        const bool standIn = scope == kLensScopeYes && !(e.scopedDeg > 0.0);
        const double v = scope == kLensScopeNo ? e.bareDeg : standIn ? e.standInDeg : e.scopedDeg;
        if (!(v > 0.0)) return false;
        if (deg) *deg = v;
        if (fromBare) *fromBare = standIn;
        return true;
    }
    return false;
}
// The reference FOV after its range: a value that is not a number reads as the default.
inline double lensClampRefDeg(double deg) noexcept {
    if (!std::isfinite(deg)) return kLensRefDefaultDeg;
    return deg < kLensRefMinDeg ? kLensRefMinDeg : deg > kLensRefMaxDeg ? kLensRefMaxDeg : deg;
}
// M = tan(h / 2) / tan(ads / 2): the factor the game enlarges the target by when the FOV goes from h to the ADS value. NaN when either is not an angle.
inline double lensZoom(double refDeg, double adsDeg) noexcept {
    if (!(std::isfinite(refDeg) && std::isfinite(adsDeg) && refDeg > 0.0 && adsDeg > 0.0)) return kNaN;
    return zoomFromFov(refDeg / kDeg, adsDeg / kDeg);
}

// ---- 2. the gate and the ease ------------------------------------------------------------------------------------------------------------------------------------------------
// What a frame of the frame thread's tick came to. kIdle is every frame that asks nothing of the lens (the key off, not on foot, not in the sights, w under 1.0): nothing is counted.
// The others are counted by the door under their reason; kQualifies frames are the ones the lens runs on (and the ones the scope's gunsight mark is taken out of the scene for).
enum class LensVerdict : uint32_t { kIdle = 0, kQualifies = 1, kNoS = 2, kNotOptical = 3 };
struct LensGateIn {
    bool keyOn = false, onFoot = false, adsActive = false, wFull = false;   // the key, native on foot, the sights up, the game's w at 1.0
    bool optical = false;                                                    // the held (item, scope) is an optical sight, in the table
    double m = kNaN;                                                         // its M against the reference FOV
    bool sFresh = false, cUsable = false;                                    // S not older than 250 ms; c through the right eye's scene rows
};
inline LensVerdict lensGate(const LensGateIn& g) noexcept {
    if (!g.keyOn || !g.onFoot || !g.adsActive || !g.wFull) return LensVerdict::kIdle;
    if (!g.optical || !(g.m >= kLensMinM)) return LensVerdict::kNotOptical;   // an M under 1.05 is as good as no entry
    if (!g.sFresh || !g.cUsable) return LensVerdict::kNoS;
    return LensVerdict::kQualifies;
}
// M after t seconds of qualifying frames: 1 at t = 0, the target at kLensEaseS and after, a smoothstep of the exponent between (so the zoom grows evenly in size, not in factor).
inline double lensEase(double target, double tS) noexcept {
    if (!(target > 1.0)) return std::isfinite(target) && target > 0.0 ? target : 1.0;
    if (!(tS > 0.0)) return 1.0;
    if (tS >= kLensEaseS) return target;
    const double s = tS / kLensEaseS;
    return std::exp(std::log(target) * (s * s * (3.0 - 2.0 * s)));
}
// The door takes a c this old (microseconds) or younger.
inline bool lensFresh(uint64_t ageUs) noexcept { return ageUs <= kLensFreshUs; }

// ---- 3. the lens as a function -----------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kLensRays = 128;           // rays from c over the depth
constexpr double kLensMaxDeg = 12.0;          // a ray that goes this far without meeting the frame escaped
constexpr uint32_t kLensRunPx = 3;            // render px in a row nearer than kLensNearM that make the frame
constexpr double kLensNearM = 0.6;            // metres from the eye
constexpr uint32_t kLensMaxEscaped = 2;       // more rays than this escaped: an open sight, no lens that frame
constexpr double kLensFeatherPx = 2.0;        // output px over which the lens meets the picture at its edge
constexpr uint32_t kLensStepCap = 768;        // render px a ray may take (12 deg at 64 px a degree)
constexpr double kLensWindowMarginPx = 8.0;   // output px beyond the farthest a ray can reach that the window holds

// The buffer pass 1 fills and pass 2 reads (and the instrument reads back), in bytes from its start. It is a raw buffer with the indirect arguments at its head.
constexpr uint32_t kLensOffArgs = 0;          // 3 uints: the groups of pass 2 (0 for a frame with no lens)
constexpr uint32_t kLensOffBox = 12;          // 4 uints: x, y, width, height of pass 2's box, frame px
constexpr uint32_t kLensOffEscaped = 28;      // uint: the rays that escaped
constexpr uint32_t kLensOffDepthAtC = 32;     // float: the depth at c
constexpr uint32_t kLensOffRadiusPx = 64;     // 128 floats: r(k) in render px, an escaped ray clamped to its neighbours
constexpr uint32_t kLensOffRadiusDeg = kLensOffRadiusPx + 4 * kLensRays;   // 128 floats: r(k) in degrees, as the ray ended (12 for an escaped one)
constexpr uint32_t kLensBufferBytes = kLensOffRadiusDeg + 4 * kLensRays;
static_assert(kLensOffRadiusDeg == 576 && kLensBufferBytes == 1088, "the lens buffer is 272 uints");

// The tangents {L, R, D, U} an eye's image spans (tangentsOf's order). A uv of the image (0..1, y down) is a point of the tangent plane.
inline void lensTangentAtUv(const float tan4[4], double u, double v, double out[2]) noexcept {
    out[0] = static_cast<double>(tan4[0]) + (static_cast<double>(tan4[1]) - tan4[0]) * u;
    out[1] = static_cast<double>(tan4[3]) - (static_cast<double>(tan4[3]) - tan4[2]) * v;
}
// The angle between the rays through two points of the tangent plane, degrees.
inline double lensAngleDeg(double ax, double ay, double bx, double by) noexcept {
    const double c = (1.0 + ax * bx + ay * by) / std::sqrt((1.0 + ax * ax + ay * ay) * (1.0 + bx * bx + by * by));
    return std::acos(c < -1.0 ? -1.0 : c > 1.0 ? 1.0 : c) * kDeg;
}
// Ray k's direction in render px (x right, y down), uniform in the angle about c.
inline void lensRayDir(uint32_t k, double* dx, double* dy) noexcept {
    const double phi = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(kLensRays);
    *dx = std::cos(phi);
    *dy = std::sin(phi);
}

struct LensRay {
    double rPx = 0.0;       // render px from c to the first px of the frame, or to where the ray ended
    double rDeg = 0.0;      // the same in degrees at the eye
    bool escaped = true;    // it went kLensMaxDeg (or off the image, or kLensStepCap steps) without meeting the frame; rDeg is then kLensMaxDeg, whichever way it ended
};
// The reference march of the first pass (the shader's text mirrors it step for step): from c in steps of 1 render px along ray k. depthAt(x, y) is the reverse-Z depth of a render px
// (larger is nearer); nearDepth is the depth of kLensNearM. The frame is the first run of kLensRunPx px nearer than nearDepth; its first px is the ray's end.
template <class DepthAt>
inline LensRay lensMarchRef(DepthAt&& depthAt, int w, int h, double cx, double cy, uint32_t k, const float tan4[4], double nearDepth) {
    LensRay out;
    double dx, dy;
    lensRayDir(k, &dx, &dy);
    double tc[2];
    lensTangentAtUv(tan4, cx / w, cy / h, tc);
    const double cosMax = std::cos(kLensMaxDeg / kDeg);
    uint32_t run = 0;
    for (uint32_t i = 1; i <= kLensStepCap; ++i) {
        const double px = cx + dx * i, py = cy + dy * i;
        const int qx = static_cast<int>(std::floor(px)), qy = static_cast<int>(std::floor(py));
        double t[2];
        lensTangentAtUv(tan4, px / w, py / h, t);
        if (qx < 0 || qy < 0 || qx >= w || qy >= h) {
            out.rPx = i;
            out.rDeg = kLensMaxDeg;   // an escape of any kind reads as the sentinel, so it is never counted as a hit in the hole's statistics
            return out;
        }
        const double cosT = (1.0 + tc[0] * t[0] + tc[1] * t[1]) / std::sqrt((1.0 + tc[0] * tc[0] + tc[1] * tc[1]) * (1.0 + t[0] * t[0] + t[1] * t[1]));
        if (cosT <= cosMax) {
            out.rPx = i;
            out.rDeg = kLensMaxDeg;
            return out;
        }
        run = depthAt(qx, qy) > nearDepth ? run + 1 : 0;
        if (run >= kLensRunPx) {
            const uint32_t first = i - (kLensRunPx - 1);
            double tf[2];
            lensTangentAtUv(tan4, (cx + dx * first) / w, (cy + dy * first) / h, tf);
            out.escaped = false;
            out.rPx = first;
            out.rDeg = lensAngleDeg(tc[0], tc[1], tf[0], tf[1]);
            return out;
        }
    }
    out.rPx = kLensStepCap;
    out.rDeg = kLensMaxDeg;
    return out;
}
// More than kLensMaxEscaped rays reaching the end without meeting the frame: the sight is open (a hip view, a red dot with no frame, the weapon lowered), and there is no lens.
inline bool lensOpenSight(uint32_t escaped) noexcept { return escaped > kLensMaxEscaped; }
// The radius the mask uses: an escaped ray (at most two of them, or the frame is open) takes the smaller of its nearest rays that did not escape, so a gap in the frame does not
// draw a spike of lens through it. The same rule the shader applies; every ray escaped leaves r as it ended.
inline void lensClampEscaped(const LensRay in[kLensRays], float outPx[kLensRays]) noexcept {
    for (uint32_t k = 0; k < kLensRays; ++k) {
        float r = static_cast<float>(in[k].rPx);
        if (in[k].escaped) {
            float a = 1.0e30f, b = 1.0e30f;
            for (uint32_t j = 1; j < kLensRays; ++j) {
                const uint32_t kk = (k + j) % kLensRays;
                if (!in[kk].escaped) { a = static_cast<float>(in[kk].rPx); break; }
            }
            for (uint32_t j = 1; j < kLensRays; ++j) {
                const uint32_t kk = (k + kLensRays - j) % kLensRays;
                if (!in[kk].escaped) { b = static_cast<float>(in[kk].rPx); break; }
            }
            const float use = std::min(a, b);
            if (use < 1.0e29f) r = use;
        }
        outPx[k] = r;
    }
}
// r(theta) in render px, interpolated between the two rays about the angle phi (radians, atan2(dy, dx) of the render px offset from c, y down).
inline double lensRadiusAt(const float r[kLensRays], double phi) noexcept {
    double a = std::fmod(phi, 2.0 * kPi);
    if (a < 0.0) a += 2.0 * kPi;
    const double f = a * static_cast<double>(kLensRays) / (2.0 * kPi);
    const uint32_t k0 = static_cast<uint32_t>(std::floor(f)) % kLensRays, k1 = (k0 + 1) % kLensRays;
    const double t = f - std::floor(f);
    return static_cast<double>(r[k0]) + (static_cast<double>(r[k1]) - r[k0]) * t;
}
// The weight of the magnified picture at a pixel: 1 more than the feather inside the hole, 0 at its edge and outside it. insidePx is r(theta) less the pixel's distance from c,
// in OUTPUT px.
inline double lensWeight(double insidePx) noexcept {
    const double w = insidePx / kLensFeatherPx;
    return w < 0.0 ? 0.0 : w > 1.0 ? 1.0 : w;
}
// The place a magnified pixel is read from: the output pixel (its centre) p comes from c + (p - c) / M. Planar in the eye's tangent plane, so the scaling is the flat zoom's.
inline void lensSourceOf(const double c[2], double m, const double p[2], double out[2]) noexcept {
    out[0] = c[0] + (p[0] - c[0]) / m;
    out[1] = c[1] + (p[1] - c[1]) / m;
}
// Catmull-Rom weights of the four taps about a fraction f (the tap before the pair, the pair, the tap after).
inline void lensCatmullRom(double f, double w[4]) noexcept {
    w[0] = f * (-0.5 + f * (1.0 - 0.5 * f));
    w[1] = 1.0 + f * f * (-2.5 + 1.5 * f);
    w[2] = f * (0.5 + f * (2.0 - 1.5 * f));
    w[3] = f * f * (-0.5 + 0.5 * f);
}

// The window of the picture the passes touch, in frame px: the box about c that holds the farthest a ray can reach (kLensMaxDeg at the eye, in the tangent plane) with a margin,
// cut to the eye's image region. The picture is copied inside it, and pass 2's box lies inside it. tcx, tcy: c's place in the tangent plane. pxPerTan: the output px a unit of
// tangent is. The window is cut to region = {x0, y0, x1, y1} and is empty (x1 <= x0) when c is outside it.
struct LensWindow {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
};
inline LensWindow lensWindow(double cx, double cy, double tcx, double tcy, double pxPerTanX, double pxPerTanY, const uint32_t region[4]) noexcept {
    LensWindow wnd;
    const double a = kLensMaxDeg / kDeg;
    const auto reach = [&](double tc, double pxPerTan) {
        const double ang = std::atan(tc);
        const double hi = std::tan(std::min(ang + a, 1.4)) - tc, lo = tc - std::tan(std::max(ang - a, -1.4));
        return 1.2 * std::max(hi, lo) * pxPerTan + kLensWindowMarginPx;
    };
    const double hx = reach(tcx, pxPerTanX), hy = reach(tcy, pxPerTanY);
    if (!(std::isfinite(hx) && std::isfinite(hy) && std::isfinite(cx) && std::isfinite(cy))) return wnd;
    wnd.x0 = std::max(static_cast<int>(region[0]), static_cast<int>(std::floor(cx - hx)));
    wnd.y0 = std::max(static_cast<int>(region[1]), static_cast<int>(std::floor(cy - hy)));
    wnd.x1 = std::min(static_cast<int>(region[2]), static_cast<int>(std::ceil(cx + hx)));
    wnd.y1 = std::min(static_cast<int>(region[3]), static_cast<int>(std::ceil(cy + hy)));
    return wnd;
}
// Pass 2's box: the farthest ray (render px) in output px about c, plus the feather and the filter's margin, cut to the window.
inline LensWindow lensBox(double cx, double cy, double maxRadiusRenderPx, double scaleX, double scaleY, const LensWindow& wnd) noexcept {
    LensWindow b;
    const double hx = maxRadiusRenderPx * scaleX + 4.0, hy = maxRadiusRenderPx * scaleY + 4.0;
    b.x0 = std::max(wnd.x0, static_cast<int>(std::floor(cx - hx)));
    b.y0 = std::max(wnd.y0, static_cast<int>(std::floor(cy - hy)));
    b.x1 = std::min(wnd.x1, static_cast<int>(std::ceil(cx + hx)));
    b.y1 = std::min(wnd.y1, static_cast<int>(std::ceil(cy + hy)));
    return b;
}

// What the rays buffer holds, decoded from the bytes the staging copy returns (kLensBufferBytes).
struct LensReadback {
    uint32_t args[3] = {};
    uint32_t box[4] = {};
    uint32_t escaped = 0;
    float depthAtC = 0.0f;
    float rPx[kLensRays] = {};
    float rDeg[kLensRays] = {};
};
inline bool lensDecode(const void* bytes, size_t n, LensReadback* out) noexcept {
    if (!bytes || !out || n < kLensBufferBytes) return false;
    const unsigned char* b = static_cast<const unsigned char*>(bytes);
    std::memcpy(out->args, b + kLensOffArgs, sizeof(out->args));
    std::memcpy(out->box, b + kLensOffBox, sizeof(out->box));
    std::memcpy(&out->escaped, b + kLensOffEscaped, sizeof(out->escaped));
    std::memcpy(&out->depthAtC, b + kLensOffDepthAtC, sizeof(out->depthAtC));
    std::memcpy(out->rPx, b + kLensOffRadiusPx, sizeof(out->rPx));
    std::memcpy(out->rDeg, b + kLensOffRadiusDeg, sizeof(out->rDeg));
    return true;
}
// The minimum, median and maximum of the radii (degrees) of the rays that did not escape; false when none did not. Escaped rays are counted by the buffer's own count.
inline bool lensHoleStats(const LensReadback& r, double* rMin, double* rMedian, double* rMax) noexcept {
    double v[kLensRays];
    uint32_t n = 0;
    for (uint32_t k = 0; k < kLensRays; ++k)
        if (std::isfinite(r.rDeg[k]) && r.rDeg[k] < static_cast<float>(kLensMaxDeg) - 1.0e-3f) v[n++] = r.rDeg[k];
    if (!n) return false;
    std::sort(v, v + n);
    *rMin = v[0];
    *rMax = v[n - 1];
    *rMedian = (n & 1u) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    return true;
}

// ---- 4. the instrument -------------------------------------------------------------------------------------------------------------------------------------------------------
// The picture's temporal sequence less the sequence c's draws were made in, in buckets: -1, 0, +1, +2 and any other difference. 0 is c from the picture's own frame.
constexpr uint32_t kLensGapBuckets = 5;
inline uint32_t lensGapBucket(uint64_t pictureSeq, uint64_t cSeq) noexcept {
    const int64_t d = static_cast<int64_t>(pictureSeq - cSeq);
    return d == -1 ? 0u : d == 0 ? 1u : d == 1 ? 2u : d == 2 ? 3u : 4u;
}

// What the door counted in a press, plus what the frame thread knows of it. The door adds to it per right-eye frame it is called on; the frame thread takes it at the press's end.
struct LensDoorStats {
    uint32_t qualifying = 0;          // frames the gate passed
    uint32_t applied = 0;             // of them, the lens ran (or was at M = 1 and had nothing to do) and the sight was not open
    uint32_t noS = 0;                 // declined by the gate: S or c missing, stale (or behind the eye)
    uint32_t staleC = 0;              // a qualifying frame whose c the door found older than 50 ms or off the picture
    uint32_t openSight = 0;           // the rays said the sight is open (counted from the rays read back)
    uint32_t notOptical = 0;          // declined by the gate: the (item, scope) is not an optical sight, or its M is under 1.05
    uint32_t depthUnavailable = 0;    // no depth for the eye at the door, or not at the size c was worked out for
    uint32_t formatRefused = 0;       // the frame route: a format that is not R8G8B8A8 or B8G8R8A8 UNORM, a region, a view, a copy or a shader that could not be had
    uint32_t dispatched = 0;          // frames the passes were issued on
    uint32_t noop = 0;                // qualifying frames at M = 1 (the first of the ease) that the door would otherwise have run on: nothing to do, nothing issued, not applied
    uint32_t seqGap[kLensGapBuckets] = {};   // frames (applied or not) with a c reading, by the picture's temporal sequence less the sequence c's draws were made in: -1, 0, +1, +2, other
    uint32_t markRedirected[2] = {};  // the scope's gunsight mark draws taken out of the scene into the eye's layer, per eye (0 left, 1 right), counted at the draw
    uint32_t markArmedFrames = 0;     // right-eye frames the door saw with the family armed (the gate said qualifies)
    uint32_t markNoDraw = 0;          // of them, frames in which no mark draw was redirected in the right eye
    uint32_t raysRead = 0;            // of them, frames whose rays were read back
    bool holeKnown = false;           // one settled frame's rays
    double rMinDeg = kNaN, rMedianDeg = kNaN, rMaxDeg = kNaN;
    uint32_t holeEscaped = 0;
    double depthAtC = kNaN, depthNear = kNaN;
    uint32_t cN = 0;                  // c in output px over the applied frames
    double cSumX = 0.0, cSumY = 0.0, cSumXX = 0.0, cSumYY = 0.0;
    uint32_t gpuN = 0;                // GPU time of the two passes over the frames that returned a sample
    double raysMsSum = 0.0, raysMsMax = 0.0, magMsSum = 0.0, magMsMax = 0.0;
};
// Frames counted with c from another frame than the picture (every bucket but 0): the count A1's press line carried, derived from the buckets.
inline uint32_t lensOtherFrames(const LensDoorStats& s) noexcept {
    return s.seqGap[0] + s.seqGap[2] + s.seqGap[3] + s.seqGap[4];
}
inline void lensNoteC(LensDoorStats* s, double x, double y) noexcept {
    if (!s || !std::isfinite(x) || !std::isfinite(y)) return;
    ++s->cN;
    s->cSumX += x;
    s->cSumY += y;
    s->cSumXX += x * x;
    s->cSumYY += y * y;
}
// The mean of c and its spread (the root of the summed variances) in output px; NaN with no frame.
inline void lensCStats(const LensDoorStats& s, double* mx, double* my, double* spread) noexcept {
    *mx = *my = *spread = kNaN;
    if (!s.cN) return;
    const double n = static_cast<double>(s.cN);
    *mx = s.cSumX / n;
    *my = s.cSumY / n;
    const double var = (s.cSumXX / n - *mx * *mx) + (s.cSumYY / n - *my * *my);
    *spread = std::sqrt(var > 0.0 ? var : 0.0);
}
inline void lensNoteGpu(LensDoorStats* s, double raysMs, double magMs) noexcept {
    if (!s || !std::isfinite(raysMs) || !std::isfinite(magMs)) return;
    ++s->gpuN;
    s->raysMsSum += raysMs;
    s->magMsSum += magMs;
    s->raysMsMax = std::max(s->raysMsMax, raysMs);
    s->magMsMax = std::max(s->magMsMax, magMs);
}
// A frame's rays read back: the sight was open (counted), and the first settled frame of the press gives the hole.
inline void lensNoteRays(LensDoorStats* s, const LensReadback& r, bool settled) noexcept {
    if (!s) return;
    ++s->raysRead;
    if (lensOpenSight(r.escaped)) ++s->openSight;
    if (!s->holeKnown && settled) {
        double a, b, c;
        if (lensHoleStats(r, &a, &b, &c)) {
            s->holeKnown = true;
            s->rMinDeg = a;
            s->rMedianDeg = b;
            s->rMaxDeg = c;
            s->holeEscaped = r.escaped;
            s->depthAtC = r.depthAtC;
        }
    }
}
// The press's totals, once the door has stopped counting: the frames the lens was applied on are those the passes were issued on less the ones whose rays said the sight is open
// (rays not read back yet count as applied: they are the last few frames of a press). A frame at M = 1 has nothing to do and is not applied, so a press that never gets past its
// first frame, or whose every other frame the door declined, has applied 0. The qualifying frames are the dispatched, the M = 1 ones and the door's own declines.
inline void lensFinish(LensDoorStats* s) noexcept {
    if (!s) return;
    s->applied = s->dispatched >= s->openSight ? s->dispatched - s->openSight : 0u;
    s->qualifying = s->dispatched + s->noop + s->staleC + s->depthUnavailable + s->formatRefused;
}

// What the frame thread counted in the press, from its own gate: the verdicts of the ticks (so a door that is never reached still leaves a press line that says the lens was due), the
// jitter readings it could not get or got from another frame than the rows, and the angle between c through S and c at the far impact point (the lens uses the far one).
struct LensTickStats {
    uint32_t qualified = 0, noS = 0, notOptical = 0, idle = 0;   // the gate's verdicts, one per tick of the press
    uint32_t fovFromBare = 0;                                     // ticks that took the FOV of an unmeasured scoped copy from its bare copy
    uint32_t jitterNone = 0, jitterOther = 0;                     // qualifying ticks with no jitter reading at the draw, or one taken in another frame than the rows
    uint32_t farN = 0;
    double farSumDeg = 0.0, farMaxDeg = 0.0;
};
inline void lensNoteFar(LensTickStats* s, double deg) noexcept {
    if (!s || !std::isfinite(deg)) return;
    ++s->farN;
    s->farSumDeg += deg;
    s->farMaxDeg = std::max(s->farMaxDeg, deg);
}
// The angle at the eye between the rays through two NDC points of an eye whose NDC square spans the tangents {L, R, D, U}; NaN when a point or the tangents are not usable.
inline double lensNdcAngleDeg(const double a[2], const double b[2], const float tan4[4]) noexcept {
    if (!a || !b || !tangentsUsable(tan4) || !std::isfinite(a[0]) || !std::isfinite(a[1]) || !std::isfinite(b[0]) || !std::isfinite(b[1])) return kNaN;
    double ta[2], tb[2];
    tangentOfNdc(a, tan4, ta);
    tangentOfNdc(b, tan4, tb);
    return lensAngleDeg(ta[0], ta[1], tb[0], tb[1]);
}

// The announce: once when the tick first runs in native on foot, and at every change of the key or the reference FOV.
inline int formatLensAnnounce(char* buf, size_t n, bool keyOn, double refDeg, uint32_t entries) noexcept {
    int len = 0;
    appendf(buf, n, &len, "scope zoom (S1): %s, reference FOV %.1f deg, %u table entries; the game's zoom is shown inside the scope's lens in the right eye, one line a press of the sights follows.",
            keyOn ? "on" : "off", refDeg, entries);
    return finishLine(buf, n, len);
}
struct LensPressLine {
    uint32_t press = 0;
    const char* item = "none";
    const char* scope = "unknown";
    bool keyOn = false;
    double adsDeg = kNaN, m = kNaN, refDeg = kNaN;
    LensTickStats t;   // the frame thread's counts
    LensDoorStats d;   // the door's
};
inline int formatLensPress(char* buf, size_t n, const LensPressLine& p) noexcept {
    int len = 0;
    char ads[24], m[24], fmean[24], fmax[24], rmin[24], rmed[24], rmax[24], dc[24], dn[24], cx[24], cy[24], cs[24], rm[24], rx[24], mm[24], mx[24];
    double mcx, mcy, spread;
    lensCStats(p.d, &mcx, &mcy, &spread);
    const LensDoorStats& d = p.d;
    appendf(buf, n, &len, "scope zoom (S1): press %u item %s scope %s key %s ads %s deg M %s h %.1f deg; ", p.press, itemText(p.item), p.scope ? p.scope : "unknown", p.keyOn ? "on" : "off",
            numText(p.adsDeg, 3, ads, sizeof(ads)), numText(p.m, 3, m, sizeof(m)), p.refDeg);
    const LensTickStats& t = p.t;
    appendf(buf, n, &len, "frame thread: qualified %u, no S %u, not an optical sight %u, idle %u; FOV from the bare copy %u; jitter reading none %u, from another frame %u; c to the far point mean %s max %s deg; ",
            t.qualified, t.noS, t.notOptical, t.idle, t.fovFromBare, t.jitterNone, t.jitterOther, numText(t.farN ? t.farSumDeg / static_cast<double>(t.farN) : kNaN, 3, fmean, sizeof(fmean)),
            numText(t.farN ? t.farMaxDeg : kNaN, 3, fmax, sizeof(fmax)));
    appendf(buf, n, &len, "applied %u of %u qualifying frames (dispatched %u, at M 1 %u); declined: no S %u, stale c %u, open sight %u, not an optical sight %u, depth unavailable %u, format refused %u; "
            "picture sequence less c's: -1 %u, 0 %u, +1 %u, +2 %u, other %u; gunsight marks redirected: L %u, R %u; armed frames %u, no mark draw %u; ", d.applied, d.qualifying, d.dispatched, d.noop,
            d.noS, d.staleC, d.openSight, d.notOptical, d.depthUnavailable, d.formatRefused, d.seqGap[0], d.seqGap[1], d.seqGap[2], d.seqGap[3], d.seqGap[4], d.markRedirected[0], d.markRedirected[1],
            d.markArmedFrames, d.markNoDraw);
    appendf(buf, n, &len, "hole r min %s median %s max %s deg, %u of %u rays escaped (rays read on %u frames); depth at c %s (near %s); ", numText(d.holeKnown ? d.rMinDeg : kNaN, 2, rmin, sizeof(rmin)),
            numText(d.holeKnown ? d.rMedianDeg : kNaN, 2, rmed, sizeof(rmed)), numText(d.holeKnown ? d.rMaxDeg : kNaN, 2, rmax, sizeof(rmax)), d.holeKnown ? d.holeEscaped : 0u,
            kLensRays, d.raysRead, numText(d.holeKnown ? d.depthAtC : kNaN, 6, dc, sizeof(dc)), numText(d.depthNear, 6, dn, sizeof(dn)));
    appendf(buf, n, &len, "c mean (%s, %s) px spread %s px; ", numText(mcx, 1, cx, sizeof(cx)), numText(mcy, 1, cy, sizeof(cy)), numText(spread, 2, cs, sizeof(cs)));
    const double gn = d.gpuN ? static_cast<double>(d.gpuN) : 0.0;
    appendf(buf, n, &len, "GPU rays %s ms (max %s), magnify %s ms (max %s)", numText(gn > 0.0 ? d.raysMsSum / gn : kNaN, 3, rm, sizeof(rm)), numText(gn > 0.0 ? d.raysMsMax : kNaN, 3, rx, sizeof(rx)),
            numText(gn > 0.0 ? d.magMsSum / gn : kNaN, 3, mm, sizeof(mm)), numText(gn > 0.0 ? d.magMsMax : kNaN, 3, mx, sizeof(mx)));
    return finishLine(buf, n, len);
}

}  // namespace scopezoom
}  // namespace edvr
