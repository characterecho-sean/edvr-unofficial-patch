#pragma once
// F205: the ADS view circle, between EDVR's two halves (native on foot, fix.native_on_foot_ads_view_circle). Same discipline as scope_zoom_window.h and left_eye_share.h: the d3d11 half
// publishes one request a frame, the provider's beginFrame reads it and hands it to the runtime in a version 11 answer (seven of the retired guard's words, EdvrNativeFrameOutput::
// reservedTerrainGuard[5..11]; layout, size and type unchanged), and the runtime's stereo renderer draws black outside a circle about the sight in each eye.
//
// WHAT IT IS. In flat, ADS narrows the camera from the hip FOV to the weapon's ADS FOV, so the screen keeps (ADS FOV / hip FOV) of what the hip view showed and the periphery is lost. A
// headset cannot change its FOV, so each eye shows its view inside a circle, black outside, whose angular radius is (ADS FOV / hip FOV) x the headset's own half-FOV: the same fraction
// of the view that flat keeps of the screen. The d3d11 half works the radius out per press (the ADS FOV from the weapon, the hip FOV from the scope zoom's reference, the half-FOV from
// the eye's published frustum) and the weight from the sight lock's ramp; the runtime only draws it, and cuts the radius to the circle that hides the full-detail scope's side bars
// (B3) when that is smaller. The centre is the direction of the sight's far point, in each eye's own view (two numbers a tangent plane each), so the two circles fuse into one.
//
// NEVER DRAWN BY ACCIDENT. A reader that cannot trust the request reads none: nothing published, a word set without the magic, a number that is not one or out of range, a signal that
// went stale (the d3d11 half stopped publishing: past kStaleHoldMs of silence the weight decays over kStaleDecayMs), and any answer that is not version 11. A weight of 0 draws nothing at
// all, so hip fire and every frame outside the sights are the images the renderer has always drawn.
//
// The way back. Once a run, when the weight is back at 0, the runtime tells the d3d11 half what it drew (EdvrNativeViewCircleReport, through the d3d11.dll export
// edvrNoteNativeViewCircle), and the d3d11 half writes it into the graphics log, where --health reads it.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#define EDVR_NATIVE_VIEW_CIRCLE_REPORT_VERSION_1 1u

// One run of the circle (from the first drawn frame to the first back at weight 0), as the runtime drew it. Every figure is the runtime's.
struct EdvrNativeViewCircleReport {
    uint32_t size, version;
    uint32_t frames;           // frames composed with the circle drawn
    uint32_t cappedFrames;     // ... of them with the radius cut to the circle that hides the full-detail scope's side bars (B3)
    uint32_t refusedFrames;    // frames the circle was asked and not drawn (the views or the centre unusable, or the cut radius too small to be a view)
    uint32_t sharedFrames;     // frames drawn with the left eye showing the right eye's image (F106)
    float seconds;             // from the first drawn frame to the last
    float radiusDeg;           // the radius the last drawn frame used
    float requestedDeg;        // the d3d11 half's radius (the flat rule) on that frame
    float coverageDeg;         // the radius that hides B3's side bars on the last frame that worked it out; 0 when none did
    float weightPeak;          // the largest weight asked
    float centre[4];           // the circle's centre in the left and right eye's image on the last drawn frame: left u, v then right u, v (fractions of the image, v down)
};
// The export's type, spelled without windows.h (HRESULT is long, WINAPI is __stdcall).
typedef long(__stdcall* EdvrNoteNativeViewCircle)(const EdvrNativeViewCircleReport*);
// The d3d11.dll export (native_on_foot.cpp): checks the report and writes its line. S_OK when written, E_INVALIDARG for a report it cannot read.
extern "C" long __stdcall edvrNoteNativeViewCircle(const EdvrNativeViewCircleReport* report);

namespace edvr {
namespace viewcircle {

constexpr uint32_t kWordsAt = 5;   // the first of reservedTerrainGuard's words the request takes (0..4 are scope_zoom_window.h's)
constexpr uint32_t kWords = 7;
constexpr uint32_t kMagic = 0xAD5C0000u;   // the high half of the first word; the low half is flags
constexpr uint32_t kFlagOn = 1u;
constexpr uint64_t kStaleHoldMs = 200;
constexpr uint64_t kStaleDecayMs = 200;
constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxRadiusRad = 80.0 * kPi / 180.0;   // a circle wider than this is no view of anything
constexpr double kMaxTc = 4.0;                         // a centre, in tangents of the eye's own frame
constexpr double kMinRadiusDeg = 5.0;                  // the smallest radius a view is drawn at; a cut below this is refused (the renderer draws no circle that frame)

struct Request {
    bool on = false;
    float radiusRad = 0.0f;
    float weight = 0.0f;            // 0..1
    float tc[2][2] = {};            // the sight's far point as tangents of the left (index 0) and the right (index 1) eye's own view: x right, y up
};

inline uint32_t bitsOf(float v) { uint32_t b = 0; std::memcpy(&b, &v, 4); return b; }
inline float floatOf(uint32_t b) { float v = 0.0f; std::memcpy(&v, &b, 4); return v; }
inline float clamp01(float v) { return v == v ? (v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v) : 0.0f; }

// A request a reader may act on: a circle needs a radius in range, a weight above 0 and centres that are numbers inside the bound; anything else is none.
inline Request sanitize(const Request& r) {
    Request o;
    const float w = clamp01(r.weight);
    bool ok = r.on && w > 0.0f && std::isfinite(r.radiusRad) && r.radiusRad > 0.0f && static_cast<double>(r.radiusRad) <= kMaxRadiusRad;
    for (int e = 0; e < 2 && ok; ++e)
        for (int a = 0; a < 2 && ok; ++a) ok = std::isfinite(r.tc[e][a]) && std::fabs(static_cast<double>(r.tc[e][a])) <= kMaxTc;
    if (!ok) return o;
    o.on = true;
    o.radiusRad = r.radiusRad;
    o.weight = w;
    std::memcpy(o.tc, r.tc, sizeof(o.tc));
    return o;
}
// The wire form: seven words. No circle is all zeros, which a reader of any age reads as none.
inline void toWords(const Request& request, uint32_t w[kWords]) {
    const Request r = sanitize(request);
    for (uint32_t i = 0; i < kWords; ++i) w[i] = 0;
    if (!r.on) return;
    w[0] = kMagic | kFlagOn;
    w[1] = bitsOf(r.radiusRad);
    w[2] = bitsOf(r.weight);
    w[3] = bitsOf(r.tc[0][0]);
    w[4] = bitsOf(r.tc[0][1]);
    w[5] = bitsOf(r.tc[1][0]);
    w[6] = bitsOf(r.tc[1][1]);
}
inline Request fromWords(const uint32_t w[kWords]) {
    Request o;
    if ((w[0] & 0xFFFF0000u) != kMagic) return o;
    Request r;
    r.on = (w[0] & kFlagOn) != 0;
    r.radiusRad = floatOf(w[1]);
    r.weight = floatOf(w[2]);
    r.tc[0][0] = floatOf(w[3]);
    r.tc[0][1] = floatOf(w[4]);
    r.tc[1][0] = floatOf(w[5]);
    r.tc[1][1] = floatOf(w[6]);
    return sanitize(r);
}

struct Slot {
    std::mutex mu;
    Request request;
    uint64_t stampMs = 0;
    bool published = false;
};
inline Slot& slot() {
    static Slot s;
    return s;
}
// The frame thread, once a frame.
inline void publish(const Request& r, uint64_t nowMs) {
    Slot& s = slot();
    std::lock_guard<std::mutex> lock(s.mu);
    s.request = sanitize(r);
    s.stampMs = nowMs;
    s.published = true;
}
// The provider's beginFrame, once a runtime frame: the request as published; past kStaleHoldMs of silence its weight decays to 0 over kStaleDecayMs.
inline Request read(uint64_t nowMs) {
    Slot& s = slot();
    std::lock_guard<std::mutex> lock(s.mu);
    Request r = s.request;
    if (!s.published) return Request{};
    const uint64_t age = nowMs >= s.stampMs ? nowMs - s.stampMs : 0;
    if (age <= kStaleHoldMs) return r;
    const uint64_t over = age - kStaleHoldMs;
    r.weight = over >= kStaleDecayMs ? 0.0f : r.weight * (1.0f - static_cast<float>(over) / static_cast<float>(kStaleDecayMs));
    return sanitize(r);
}
inline void clear() {
    Slot& s = slot();
    std::lock_guard<std::mutex> lock(s.mu);
    s.request = Request{};
    s.published = false;
}

// ---- the radius ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// The headset's own half-FOV: the vertical half-angle of the eye's frustum, (atan(up) - atan(down)) / 2 radians, from the tangents {L, R, D, U} the eye's texture spans (L and D negative;
// native on foot publishes them with a temporal mode on and, since F198, with it off). NaN for tangents that are not an eye. Vertical because the game's FOVs are the camera's vertical
// FOV (the same camera, the same axis, in the hip and in the sights), and a vertical half-angle is the one a headset's frustum states the same way whatever its width.
inline double halfFovRad(const float tan4[4]) {
    if (!tan4) return std::nan("");
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(tan4[i])) return std::nan("");
    if (!(tan4[3] > tan4[2])) return std::nan("");
    return 0.5 * (std::atan(static_cast<double>(tan4[3])) - std::atan(static_cast<double>(tan4[2])));
}
// The circle's angular radius: (ADS FOV / hip FOV) x the headset's half-FOV, radians. NaN (no circle, no guess) for an ADS FOV or a hip FOV that is not an angle, or a half-FOV that
// is not one. A ratio past 1 (an ADS FOV wider than the hip) is held to 1: the circle then reaches the frustum's vertical edge.
inline double radiusRad(double adsDeg, double hipDeg, double halfFov) {
    if (!(std::isfinite(adsDeg) && std::isfinite(hipDeg) && std::isfinite(halfFov) && adsDeg > 0.0 && hipDeg > 0.0 && halfFov > 0.0)) return std::nan("");
    const double ratio = adsDeg / hipDeg;
    return (ratio > 1.0 ? 1.0 : ratio) * halfFov;
}

// ---- the weight ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// The circle comes in with the sight lock (0.15 s) and goes with it (0.3 s), from the first frame its centre is known to the last: step(want, dt) once a frame.
constexpr double kInSec = 0.15, kOutSec = 0.3;
struct Ramp {
    double w = 0.0;
    void step(bool want, double dt) {
        const double t = std::isfinite(dt) && dt > 0.0 ? dt : 0.0;
        w = want ? std::fmin(1.0, w + t / kInSec) : std::fmax(0.0, w - t / kOutSec);
    }
};

}  // namespace viewcircle
}  // namespace edvr
