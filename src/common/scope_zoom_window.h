#pragma once
// The scope zoom's window, between EDVR's two halves (native on foot, B3: experimental.native_on_foot_ads_scope_zoom_full_detail; docs\design-vr-scope-magnification-2026-10-28.md,
// section 11). Same discipline as left_eye_share.h: the d3d11 half publishes one request a frame (whether the left eye is to be drawn as the zoomed view, by how much, about which
// direction, and how black its uncovered strip is), the provider's beginFrame reads it and hands it to the runtime in a version 11 answer (five of the retired guard's words,
// EdvrNativeFrameOutput::reservedTerrainGuard[0..4]; layout, size and type unchanged), and the runtime tells the game the left eye's frustum narrowed (src\openxr\native_runtime_host.h).
//
// NEVER ZOOMED BY ACCIDENT. A reader that cannot trust the request reads none: nothing published, a number that is not one or out of range, a word set without the magic, a signal that
// went stale (the d3d11 half stopped publishing: past kStaleHoldMs of silence the zoom is off at once and the strip decays over kStaleDecayMs), and any answer that is not version 11.
//
// The way back. Once a press, when the zoom has ended, the runtime tells the d3d11 half what it did (EdvrNativeScopeZoomReport, through the d3d11.dll export
// edvrNoteNativeScopeZoom), and the d3d11 half writes it into the graphics log, where --health reads it. A d3d11.dll without the export is not told; the runtime's own log has the same
// figures.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#define EDVR_NATIVE_SCOPE_ZOOM_REPORT_VERSION_1 1u
#define EDVR_NATIVE_SCOPE_ZOOM_REPORT_CALLERS 4u

// One press of the scoped sight's zoom, as the runtime did it. Every figure is the runtime's.
struct EdvrNativeScopeZoomReport {
    uint32_t size, version;
    uint32_t frames;            // frames the runtime told the game the left eye's frustum narrowed
    uint32_t refused[4];        // frames the zoom was asked for and not applied: the answer was not version 11, the share was not at full weight, the share was not drawn, the geometry was unusable
    uint32_t stripFrames;       // frames the left eye's uncovered strip was drawn darkened (a strip above 0 on a drawn share)
    float seconds;              // from the first zoomed frame to the last
    float peakM;                // the largest narrowing told (the true span over the zoomed one, vertical)
    float trueTan[4];           // eye 0's told frustum without the zoom, at the last zoomed frame: left, right, down, up tangents
    float zoomTan[4];           // ... and with it
    uint32_t rawReads;          // the game's reads of the projection through the raw getter for eye 0 while the zoom stood (what its UI scale reads)
    uint32_t matrixReads;       // ... and through the matrix getter (what the picture is drawn with)
    float rawSpan;              // the vertical tangent span (bottom minus top) the raw getter last answered eye 0 with while the zoom stood; 0 when it was not read
    float matrixSpan;           // ... and the matrix getter's (2 / m[1][1] of the matrix it returned); the two are in the ratio of the narrowing
    uint32_t meshWithheld;      // frames the hidden-area mesh was withheld because of the zoom
    uint32_t rawCallerRva[EDVR_NATIVE_SCOPE_ZOOM_REPORT_CALLERS];     // the busiest callers of the raw getter while the zoom stood, most first: the return address as an RVA in the game's executable (0xFFFFFFFF: outside it)
    uint32_t rawCallerReads[EDVR_NATIVE_SCOPE_ZOOM_REPORT_CALLERS];   // their reads; 0 in a slot no caller filled
    uint32_t rawCallerOther;    // the raw getter's reads by every other caller
    // B3-3: the right eye's mirror of the left eye's strip (the directions of its view the left view does not see, black with the same edge band), counted over the run of the strip
    uint32_t mirrorFrames;      // right-eye frames composed with the mirror strip drawn (the strip up or the left eye narrowed, with a share drawn)
    float mirrorWidthMax;       // the largest share of the right eye's width it covered on one frame: the columns of its middle row at least half black, 0..1
    uint32_t shareNotDrawnPassed;   // frames the narrowing went on past a share that was not drawn the frame before (one in a row)
};
// The export's type, spelled without windows.h (HRESULT is long, WINAPI is __stdcall): a rig that cannot include windows.h reads the runtime's headers.
typedef long(__stdcall* EdvrNoteNativeScopeZoom)(const EdvrNativeScopeZoomReport*);
// The d3d11.dll export (native_on_foot.cpp): checks the report and writes its line. S_OK when written, E_INVALIDARG for a report it cannot read.
extern "C" long __stdcall edvrNoteNativeScopeZoom(const EdvrNativeScopeZoomReport* report);

namespace edvr {
namespace scopewin {

constexpr uint32_t kWords = 5;
constexpr uint32_t kMagic = 0x5C0E0000u;   // the high half of the first word; the low half is flags
constexpr uint32_t kFlagZoom = 1u;
constexpr uint64_t kStaleHoldMs = 200;
constexpr uint64_t kStaleDecayMs = 200;
constexpr float kMinM = 1.05f, kMaxM = 8.0f;
constexpr float kMaxTc = 1.6f;   // the window's centre, in tangents of the eye's own frame: well inside any eye's span

struct Request {
    bool zoom = false;
    float m = 1.0f;
    float tc[2] = {0.0f, 0.0f};
    float strip = 0.0f;   // how black the left eye's uncovered strip is, 0..1
};

inline uint32_t bitsOf(float v) { uint32_t b = 0; std::memcpy(&b, &v, 4); return b; }
inline float floatOf(uint32_t b) { float v = 0.0f; std::memcpy(&v, &b, 4); return v; }
inline float clamp01(float v) { return v == v ? (v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v) : 0.0f; }

// A request a reader may act on: a zoom needs a narrowing in range and a centre that is a number inside the bound; anything else is no zoom (the strip alone is clamped).
inline Request sanitize(const Request& r) {
    Request o;
    o.strip = clamp01(r.strip);
    const bool ok = r.zoom && std::isfinite(r.m) && r.m >= kMinM && r.m <= kMaxM && std::isfinite(r.tc[0]) && std::isfinite(r.tc[1]) && std::fabs(r.tc[0]) <= kMaxTc && std::fabs(r.tc[1]) <= kMaxTc;
    if (!ok) return o;
    o.zoom = true;
    o.m = r.m;
    o.tc[0] = r.tc[0];
    o.tc[1] = r.tc[1];
    return o;
}
// The wire form: five words. A request that is no zoom and no strip is all zeros, which a reader of any age reads as none.
inline void toWords(const Request& request, uint32_t w[kWords]) {
    const Request r = sanitize(request);
    for (uint32_t i = 0; i < kWords; ++i) w[i] = 0;
    if (!r.zoom && !(r.strip > 0.0f)) return;
    w[0] = kMagic | (r.zoom ? kFlagZoom : 0u);
    w[1] = bitsOf(r.zoom ? r.m : 1.0f);
    w[2] = bitsOf(r.zoom ? r.tc[0] : 0.0f);
    w[3] = bitsOf(r.zoom ? r.tc[1] : 0.0f);
    w[4] = bitsOf(r.strip);
}
// False for a word set that is none (zeros, or no magic); a set that carries the magic and a value out of range reads as none too, with the strip it carried clamped.
inline Request fromWords(const uint32_t w[kWords]) {
    Request o;
    if ((w[0] & 0xFFFF0000u) != kMagic) return o;
    Request r;
    r.zoom = (w[0] & kFlagZoom) != 0;
    r.m = floatOf(w[1]);
    r.tc[0] = floatOf(w[2]);
    r.tc[1] = floatOf(w[3]);
    r.strip = floatOf(w[4]);
    return sanitize(r);
}

// The tangents {L, R, D, U} (L, D negative) of the zoomed told frustum: the eye's own span divided by m, centred on the tangent plane's point tc, moved in where it would leave the eye's own
// span. False when m is not in [kMinM, kMaxM], an input is not a number, the span is empty, or tc lies outside the span. The runtime narrows the left eye's frustum by it, and
// scope_zoom_b3.h's b3Window is it, for the frame thread's checks and the rigs.
inline bool window(const double trueTan[4], const double tc[2], double m, double out[4]) {
    if (!trueTan || !tc || !out) return false;
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(trueTan[i])) return false;
    if (!std::isfinite(tc[0]) || !std::isfinite(tc[1]) || !std::isfinite(m)) return false;
    if (!(m >= static_cast<double>(kMinM) && m <= static_cast<double>(kMaxM))) return false;
    const double spanX = trueTan[1] - trueTan[0], spanY = trueTan[3] - trueTan[2];
    if (!(spanX > 1.0e-6 && spanY > 1.0e-6)) return false;
    if (tc[0] < trueTan[0] || tc[0] > trueTan[1] || tc[1] < trueTan[2] || tc[1] > trueTan[3]) return false;
    const double hx = 0.5 * spanX / m, hy = 0.5 * spanY / m;
    const double cx = std::fmin(std::fmax(tc[0], trueTan[0] + hx), trueTan[1] - hx), cy = std::fmin(std::fmax(tc[1], trueTan[2] + hy), trueTan[3] - hy);
    out[0] = cx - hx;
    out[1] = cx + hx;
    out[2] = cy - hy;
    out[3] = cy + hy;
    return true;
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
// The provider's beginFrame, once a runtime frame: the request as published, or none once it is older than kStaleHoldMs (the strip then decays to 0 over kStaleDecayMs).
inline Request read(uint64_t nowMs) {
    Slot& s = slot();
    std::lock_guard<std::mutex> lock(s.mu);
    Request r = s.request;
    if (!s.published) return Request{};
    const uint64_t age = nowMs >= s.stampMs ? nowMs - s.stampMs : 0;
    if (age <= kStaleHoldMs) return r;
    r.zoom = false;
    r.m = 1.0f;
    r.tc[0] = r.tc[1] = 0.0f;
    const uint64_t over = age - kStaleHoldMs;
    r.strip = over >= kStaleDecayMs ? 0.0f : r.strip * (1.0f - static_cast<float>(over) / static_cast<float>(kStaleDecayMs));
    return r;
}
// Forgotten: reads none until the next publish.
inline void clear() {
    Slot& s = slot();
    std::lock_guard<std::mutex> lock(s.mu);
    s.request = Request{};
    s.published = false;
}

// A report the d3d11 half can read: its own size and version, and every figure finite.
inline bool reportReadable(const EdvrNativeScopeZoomReport* r) {
    if (!r || r->size < sizeof(EdvrNativeScopeZoomReport) || r->version != EDVR_NATIVE_SCOPE_ZOOM_REPORT_VERSION_1) return false;
    const float values[] = {r->seconds, r->peakM, r->trueTan[0], r->trueTan[1], r->trueTan[2], r->trueTan[3], r->zoomTan[0], r->zoomTan[1], r->zoomTan[2], r->zoomTan[3], r->rawSpan, r->matrixSpan, r->mirrorWidthMax};
    for (float v : values)
        if (!(v == v) || v > 1e9f || v < -1e9f) return false;
    return true;
}

}  // namespace scopewin
}  // namespace edvr
