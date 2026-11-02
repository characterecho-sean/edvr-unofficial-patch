#pragma once
// F206: the ADS view circle, between EDVR's two halves (native on foot). It is the full-detail scope's alone (B3: the scoped Executioner, experimental.native_on_foot_ads_scope_zoom_full_detail, the
// scope zoom on and a temporal mode, with the press's sight eye at both): the left eye is drawn as the zoomed view and its uncovered strip, and the right eye's mirror of it, are black bars at the
// sides; the circle is black outside a circle about the sight in each eye, as wide as it can be and still hide those bars. Same discipline as scope_zoom_window.h and left_eye_share.h: the d3d11
// half publishes one request a frame, the provider's beginFrame reads it and hands it to the runtime in a version 11 answer (five of the retired guard's words, EdvrNativeFrameOutput::
// reservedTerrainGuard[5..9]; layout, size and type unchanged), and the runtime's stereo renderer draws it.
//
// WHAT THE REQUEST IS. Only where the circle's centre is: the direction of the sight's far point in each eye's own view (two numbers a tangent plane each), so the two circles fuse into one. The
// d3d11 half has no radius and no weight to ask for (F205 had the flat game's ADS FOV rule for the first and the sight lock's ramp for the second; F206 removed both). The runtime owns them: the
// circle is drawn exactly while B3's right-eye mirror strip is, by the mirror's own weight, and its radius is the widest that keeps that strip's bars in the black, worked out every frame from the
// centre and the two views (src\openxr\scope_circle.h coverageRadius). So the circle and the bars it hides come and go together.
//
// F210b: the request is still the sight's direction, unchanged on the wire (version 11, words 5..9 as shipped in v0.22.0-rc.5); the RUNTIME turns it down to the middle of the eye's field vertically (the Quest 3's
// field reaches 42 degrees up and 53 down, so a circle on a straight-ahead sight sat high in the view) before it draws and before it works out the radius, and says how far in the press's line.
//
// NEVER DRAWN BY ACCIDENT. A reader that cannot trust the request reads none: nothing published, a word set without the magic, a number that is not one or out of range, a signal that went stale
// (the d3d11 half stopped publishing: past kStaleHoldMs + kStaleDecayMs of silence, the span B3's own strip takes to decay), and any answer that is not version 11. Without the mirror the request does
// nothing at all, so every frame outside the full-detail scope is the image the renderer has always drawn.
//
// The way back. Once a run, when the mirror has gone, the runtime tells the d3d11 half what it drew (EdvrNativeViewCircleReport, through the d3d11.dll export edvrNoteNativeViewCircle), and the d3d11
// half writes it into the graphics log, where --health reads it.
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#define EDVR_NATIVE_VIEW_CIRCLE_REPORT_VERSION_2 2u

// One run of the circle (from the first drawn frame to the first frame the mirror was gone), as the runtime drew it. Every figure is the runtime's.
struct EdvrNativeViewCircleReport {
    uint32_t size, version;
    uint32_t frames;           // frames composed with the circle drawn
    uint32_t refusedFrames;    // frames the circle was asked and not drawn (the views or the centre unusable, or the radius that hides the bars too small to be a view)
    uint32_t sharedFrames;     // frames drawn with the left eye showing the right eye's image (F106)
    float seconds;             // from the first drawn frame to the last
    float radiusDeg;           // the radius the last drawn frame used: the widest that keeps the bars in the black
    float radiusMinDeg;        // ... the smallest radius any drawn frame of the run used
    float radiusMaxDeg;        // ... and the largest
    float edgeDeg;             // the nearest edge of what the other eye covers, from the circle's centre, on the last drawn frame (the radius plus the margin and half the band)
    float weightPeak;          // the largest weight drawn (the mirror's)
    float centre[4];           // the circle's centre in the left and right eye's image on the last drawn frame: left u, v then right u, v (fractions of the image, v down)
    // F210b, appended (the version stays 2, the size says who has them): the pitch the centre was turned by on the last drawn frame (degrees, + up: the middle of the field vertically, scope_circle.h
    // fieldMiddleRad) and the field's up and down angles it is the middle of (the mean over both eyes, degrees, down negative).
    float pitchDeg, fieldUpDeg, fieldDownDeg;
};
// The report's size before F210b: a reader takes a report of at least this many bytes as version 2, and reads the pitch only from one of the full size.
#define EDVR_NATIVE_VIEW_CIRCLE_REPORT_BASE_BYTES offsetof(EdvrNativeViewCircleReport, pitchDeg)
// The export's type, spelled without windows.h (HRESULT is long, WINAPI is __stdcall).
typedef long(__stdcall* EdvrNoteNativeViewCircle)(const EdvrNativeViewCircleReport*);
// The d3d11.dll export (native_on_foot.cpp): checks the report and writes its line. S_OK when written, E_INVALIDARG for a report it cannot read.
extern "C" long __stdcall edvrNoteNativeViewCircle(const EdvrNativeViewCircleReport* report);

namespace edvr {
namespace viewcircle {

constexpr uint32_t kWordsAt = 5;   // the first of reservedTerrainGuard's words the request takes (0..4 are scope_zoom_window.h's)
constexpr uint32_t kWords = 5;
constexpr uint32_t kMagic = 0xAD5C0000u;   // the high half of the first word; the low half is flags
constexpr uint32_t kFlagOn = 1u;
constexpr uint64_t kStaleHoldMs = 200;
constexpr uint64_t kStaleDecayMs = 200;
constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxTc = 4.0;   // a centre, in tangents of the eye's own frame

struct Request {
    bool on = false;
    float tc[2][2] = {};            // the sight's far point as tangents of the left (index 0) and the right (index 1) eye's own view: x right, y up
};

inline uint32_t bitsOf(float v) { uint32_t b = 0; std::memcpy(&b, &v, 4); return b; }
inline float floatOf(uint32_t b) { float v = 0.0f; std::memcpy(&v, &b, 4); return v; }

// A request a reader may act on: a circle needs centres that are numbers inside the bound; anything else is none.
inline Request sanitize(const Request& r) {
    Request o;
    bool ok = r.on;
    for (int e = 0; e < 2 && ok; ++e)
        for (int a = 0; a < 2 && ok; ++a) ok = std::isfinite(r.tc[e][a]) && std::fabs(static_cast<double>(r.tc[e][a])) <= kMaxTc;
    if (!ok) return o;
    o.on = true;
    std::memcpy(o.tc, r.tc, sizeof(o.tc));
    return o;
}
// The wire form: five words. No circle is all zeros, which a reader of any age reads as none.
inline void toWords(const Request& request, uint32_t w[kWords]) {
    const Request r = sanitize(request);
    for (uint32_t i = 0; i < kWords; ++i) w[i] = 0;
    if (!r.on) return;
    w[0] = kMagic | kFlagOn;
    w[1] = bitsOf(r.tc[0][0]);
    w[2] = bitsOf(r.tc[0][1]);
    w[3] = bitsOf(r.tc[1][0]);
    w[4] = bitsOf(r.tc[1][1]);
}
inline Request fromWords(const uint32_t w[kWords]) {
    Request o;
    if ((w[0] & 0xFFFF0000u) != kMagic) return o;
    Request r;
    r.on = (w[0] & kFlagOn) != 0;
    r.tc[0][0] = floatOf(w[1]);
    r.tc[0][1] = floatOf(w[2]);
    r.tc[1][0] = floatOf(w[3]);
    r.tc[1][1] = floatOf(w[4]);
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
// The provider's beginFrame, once a runtime frame: the request as published; none once the d3d11 half has been silent for kStaleHoldMs + kStaleDecayMs (B3's own strip decays in that span, and the
// circle goes with the mirror whatever the request says).
inline Request read(uint64_t nowMs) {
    Slot& s = slot();
    std::lock_guard<std::mutex> lock(s.mu);
    if (!s.published) return Request{};
    const uint64_t age = nowMs >= s.stampMs ? nowMs - s.stampMs : 0;
    if (age > kStaleHoldMs + kStaleDecayMs) return Request{};
    return s.request;
}
inline void clear() {
    Slot& s = slot();
    std::lock_guard<std::mutex> lock(s.mu);
    s.request = Request{};
    s.published = false;
}

}  // namespace viewcircle
}  // namespace edvr
