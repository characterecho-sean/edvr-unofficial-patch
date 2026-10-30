#pragma once
// The runtime's half of the scope zoom's window (docs\design-vr-scope-magnification-2026-10-28.md, section 11; the request is src\common\scope_zoom_window.h). The pure parts: whether
// the left eye's frustum is narrowed this frame and to what, and one press of it as the runtime did it, which goes back to the d3d11 half as an EdvrNativeScopeZoomReport.
//
// The narrowing is only the TOLD frustum of eye 0, set in WaitGetPoses where the FOV trim sets its own: tangents only, no pose, no size, no eye-to-head. The game draws eye 0 with it. What
// stays true: the located views the compositor lays the layer on, the raw projection getter's answer for eye 0 (which the game's UI scale reads), the frusta the temporal pass announces,
// and the hidden-area mesh (cut for the true frustum; withheld while the zoom stands). It is applied only while the right eye's picture wholly covers the left eye (the share at full
// weight, drawn the frame before), because the zoomed picture is never meant to be seen; otherwise the left eye keeps its own view and the reason is counted.
#include "projection_math.h"
#include "../common/scope_zoom_window.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <utility>

namespace edvr::openxr {

enum class ScopeZoomWhy : uint32_t { kApplied = 0, kNotAsked = 1, kVersion = 2, kShare = 3, kShareNotDrawn = 4, kGeometry = 5 };
// Index into EdvrNativeScopeZoomReport::refused for a reason that is a refusal (not kApplied, not kNotAsked).
inline uint32_t scopeZoomRefusalIndex(ScopeZoomWhy w) { return static_cast<uint32_t>(w) - static_cast<uint32_t>(ScopeZoomWhy::kVersion); }

struct ScopeZoomInput {
    scopewin::Request request;
    bool answerCarries = false;     // the provider's answer is version 11 (the words are believed)
    float share = 0.f;              // this frame's left eye share
    bool shareDrawn = false;        // the renderer drew the share in its last compose
    bool shareRefused = false;      // ... or refused it (views it could not map)
    XrFovf base{};                  // eye 0's told frustum without the zoom (the trim's, or the located one)
    // B3-2: a zoom that stands (the last frame was narrowed) goes on past ONE frame in a row of a share that was not drawn and not refused. notDrawnRun is how many such frames in a row
    // it has gone on past already. See decideScopeZoom for why that is safe: renderCaptured blacks the left eye on a narrowed frame whose share it does not draw.
    bool standing = false;
    uint32_t notDrawnRun = 0;
};
struct ScopeZoomOutput {
    ScopeZoomWhy why = ScopeZoomWhy::kNotAsked;
    XrFovf fov{};                   // the told frustum with the zoom (the base when it is not applied)
    double trueTan[4] = {}, zoomTan[4] = {};   // left, right, down, up tangents
    double narrowing = 1.0;         // the true vertical span over the zoomed one
    float strip = 0.f;              // how far the left eye's uncovered strip is taken over by the fill this frame
    uint32_t edge = 0;              // what fills it (scopewin::kEdge*)
    bool passedUnshared = false;    // the frame was applied past a share that was not drawn (B3-2)
};
inline ScopeZoomOutput decideScopeZoom(const ScopeZoomInput& in) {
    ScopeZoomOutput o;
    o.fov = in.base;
    o.strip = in.answerCarries ? scopewin::clamp01(in.request.strip) : 0.f;
    o.edge = in.answerCarries ? in.request.edge : scopewin::kEdgeBlack;
    if (!in.answerCarries) { o.why = in.request.zoom ? ScopeZoomWhy::kVersion : ScopeZoomWhy::kNotAsked; return o; }
    if (!in.request.zoom) return o;   // kNotAsked
    // the share must wholly cover the left eye, and have been drawn the frame before: a zoomed picture that could be seen is never made
    if (!(in.share >= 0.999f)) { o.why = ScopeZoomWhy::kShare; return o; }
    // A share that was not drawn the frame before ends a zoom that does not stand, and one that does after a single such frame in a row. A narrowed picture is never shown uncovered even then:
    // D3D11Stereo::renderCaptured, on a frame drawn with eye 0 narrowed (leftZoomed) in which it draws no share (no share asked, views it cannot map, a mask it cannot build), clears the
    // left eye to black in place of its own picture, and with the share drawn the shader makes every uncovered pixel of such a frame the fill. So the frame that passes, and the one after,
    // are covered whether or not their own share is drawn. A share the renderer REFUSED is not passed: its cause (views it cannot map) does not clear on its own.
    if (!in.shareDrawn || in.shareRefused) {
        const bool pass = in.standing && !in.shareDrawn && !in.shareRefused && in.notDrawnRun == 0;
        if (!pass) { o.why = ScopeZoomWhy::kShareNotDrawn; return o; }
        o.passedUnshared = true;
    }
    RawFov raw{};
    if (!fovToRaw(in.base, raw)) { o.why = ScopeZoomWhy::kGeometry; return o; }
    const double trueTan[4] = {raw.left, raw.right, raw.top, raw.bottom};   // RawFov: top is the -Y ray (down), bottom the +Y ray (up)
    const double tc[2] = {in.request.tc[0], in.request.tc[1]};
    double zoomTan[4];
    if (!scopewin::window(trueTan, tc, static_cast<double>(in.request.m), zoomTan)) { o.why = ScopeZoomWhy::kGeometry; return o; }
    XrFovf fov{};
    fov.angleLeft = static_cast<float>(std::atan(zoomTan[0]));
    fov.angleRight = static_cast<float>(std::atan(zoomTan[1]));
    fov.angleDown = static_cast<float>(std::atan(zoomTan[2]));
    fov.angleUp = static_cast<float>(std::atan(zoomTan[3]));
    RawFov check{};
    if (!fovToRaw(fov, check)) { o.why = ScopeZoomWhy::kGeometry; return o; }
    o.why = ScopeZoomWhy::kApplied;
    o.fov = fov;
    for (int i = 0; i < 4; ++i) { o.trueTan[i] = trueTan[i]; o.zoomTan[i] = zoomTan[i]; }
    o.narrowing = (trueTan[3] - trueTan[2]) / (zoomTan[3] - zoomTan[2]);
    return o;
}

// Who read the raw projection getter for eye 0 while the window stood, by the return address as an RVA in the game's executable (openvr_system.cpp frameRva; kFrameOutside for one outside
// it). The raw getter is the one the zoom keeps true (the game's UI scale reads it), and the design answers every caller of it with the true frustum; if a caller turns out to draw
// something from it that should have been narrowed, this names it. A fixed table of the first kSlots distinct callers and a count for the rest; lock-free, because the game's threads
// read the getter and the owner thread resets and reports. A reset while a read is in flight can lose that read; the report is an instrument, not a ledger.
class ScopeZoomCallers {
public:
    static constexpr unsigned kSlots = 8;
    void reset() noexcept {
        for (unsigned i = 0; i < kSlots; ++i) { reads_[i].store(0, std::memory_order_relaxed); key_[i].store(0, std::memory_order_relaxed); }
        overflow_.store(0, std::memory_order_relaxed);
    }
    void note(uint32_t rva) noexcept {
        const uint64_t key = static_cast<uint64_t>(rva) + 1u;   // 0 is an empty slot
        for (unsigned i = 0; i < kSlots; ++i) {
            uint64_t have = key_[i].load(std::memory_order_relaxed);
            if (have == 0) {
                uint64_t none = 0;
                have = key_[i].compare_exchange_strong(none, key, std::memory_order_relaxed) ? key : none;
            }
            if (have == key) { reads_[i].fetch_add(1, std::memory_order_relaxed); return; }
        }
        overflow_.fetch_add(1, std::memory_order_relaxed);
    }
    // The busiest callers, most reads first (ties in the order they were first seen); other is every read not in the top slots. A slot nobody filled has 0 reads.
    void top(uint32_t (&rva)[EDVR_NATIVE_SCOPE_ZOOM_REPORT_CALLERS], uint32_t (&reads)[EDVR_NATIVE_SCOPE_ZOOM_REPORT_CALLERS], uint32_t& other) const noexcept {
        uint32_t r[kSlots], n[kSlots];
        unsigned used = 0;
        for (unsigned i = 0; i < kSlots; ++i) {
            const uint64_t key = key_[i].load(std::memory_order_relaxed);
            const uint32_t count = reads_[i].load(std::memory_order_relaxed);
            if (key == 0 || count == 0) continue;
            r[used] = static_cast<uint32_t>(key - 1u);
            n[used] = count;
            ++used;
        }
        for (unsigned a = 1; a < used; ++a)   // insertion sort, stable
            for (unsigned b = a; b > 0 && n[b] > n[b - 1]; --b) { std::swap(n[b], n[b - 1]); std::swap(r[b], r[b - 1]); }
        uint64_t rest = overflow_.load(std::memory_order_relaxed);
        for (unsigned i = 0; i < EDVR_NATIVE_SCOPE_ZOOM_REPORT_CALLERS; ++i) {
            rva[i] = i < used ? r[i] : 0u;
            reads[i] = i < used ? n[i] : 0u;
        }
        for (unsigned i = EDVR_NATIVE_SCOPE_ZOOM_REPORT_CALLERS; i < used; ++i) rest += n[i];
        other = rest > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(rest);
    }
private:
    std::atomic<uint64_t> key_[kSlots]{};
    std::atomic<uint32_t> reads_[kSlots]{};
    std::atomic<uint32_t> overflow_{0};
};

// One press of the zoom: from the first zoomed frame to the first frame without one.
struct ScopeZoomPress {
    bool open = false;
    uint64_t firstUs = 0, lastUs = 0;
    uint32_t frames = 0, refused[4] = {}, stripFrames = 0, meshWithheld = 0;
    float peakM = 0.f;
    double trueTan[4] = {}, zoomTan[4] = {};
    // the game's reads of the projection for eye 0 while the zoom stood, taken from the system object's counters at the press's end: how many, the vertical tangent span each getter last
    // answered (0: not read), and who read the raw getter
    uint32_t rawReads = 0, matrixReads = 0;
    float rawSpan = 0.f, matrixSpan = 0.f;
    uint32_t callerRva[EDVR_NATIVE_SCOPE_ZOOM_REPORT_CALLERS] = {}, callerReads[EDVR_NATIVE_SCOPE_ZOOM_REPORT_CALLERS] = {}, callerOther = 0;
    // B3-2: what filled the strip (ScopeZoomFillTally, taken at the press's end) and the frames the narrowing went on past a share that was not drawn
    uint32_t edgeMode = 0, fillFrames[4] = {}, heldCopies = 0, heldMissing = 0, heldAgeFrames = 0, notDrawnPassed = 0;
    float heldAgeMs = 0.f, heldRotationMaxDeg = 0.f;
    void begin(uint64_t nowUs) { *this = ScopeZoomPress{}; open = true; firstUs = lastUs = nowUs; }
    void applied(const ScopeZoomOutput& o, uint64_t nowUs, bool stripDrawn, bool meshWithheldNow) {
        ++frames;
        lastUs = nowUs;
        if (stripDrawn) ++stripFrames;
        if (meshWithheldNow) ++meshWithheld;
        if (o.passedUnshared) ++notDrawnPassed;
        if (static_cast<float>(o.narrowing) > peakM) peakM = static_cast<float>(o.narrowing);
        for (int i = 0; i < 4; ++i) { trueTan[i] = o.trueTan[i]; zoomTan[i] = o.zoomTan[i]; }
    }
    void fill(EdvrNativeScopeZoomReport& r) const {
        r = EdvrNativeScopeZoomReport{};
        r.size = sizeof(r);
        r.version = EDVR_NATIVE_SCOPE_ZOOM_REPORT_VERSION_1;
        r.frames = frames;
        for (int i = 0; i < 4; ++i) { r.refused[i] = refused[i]; r.trueTan[i] = static_cast<float>(trueTan[i]); r.zoomTan[i] = static_cast<float>(zoomTan[i]); }
        r.stripFrames = stripFrames;
        r.seconds = static_cast<float>(static_cast<double>(lastUs - firstUs) / 1.0e6);
        r.peakM = peakM;
        r.rawReads = rawReads;
        r.matrixReads = matrixReads;
        r.rawSpan = rawSpan;
        r.matrixSpan = matrixSpan;
        r.meshWithheld = meshWithheld;
        for (unsigned i = 0; i < EDVR_NATIVE_SCOPE_ZOOM_REPORT_CALLERS; ++i) { r.rawCallerRva[i] = callerRva[i]; r.rawCallerReads[i] = callerReads[i]; }
        r.rawCallerOther = callerOther;
        r.edgeMode = edgeMode;
        for (unsigned i = 0; i < 4; ++i) r.fillFrames[i] = fillFrames[i];
        r.heldCopies = heldCopies;
        r.heldMissing = heldMissing;
        r.heldAgeFrames = heldAgeFrames;
        r.heldAgeMs = heldAgeMs;
        r.heldRotationMaxDeg = heldRotationMaxDeg;
        r.shareNotDrawnPassed = notDrawnPassed;
    }
    // The tally of the fill (below) as of now, into this press.
    template <class Tally>
    void takeFill(const Tally& t, uint32_t edge) {
        edgeMode = edge;
        for (unsigned i = 0; i < 4; ++i) fillFrames[i] = t.frames[i];
        heldCopies = t.copies;
        heldMissing = t.missing;
        heldAgeFrames = t.ageFrames;
        heldAgeMs = t.ageMs;
        heldRotationMaxDeg = t.rotationMaxDeg;
    }
};

// What the compositor drew into the left eye's strip, counted over a run of the strip: from the first composed frame with the strip up (or the left eye narrowed) to the first without.
// A press that opens inside the run reports the whole run, so the copies made through the entry ramp are in it. fillMode is the fill the frame drew (scopewin::kEdge*; a narrowed
// frame with no held picture of the run is black and `missing`), copied whether this frame made a held copy, and the age and rotation are the renderer's own (StereoShareFacts).
struct ScopeZoomFillTally {
    bool active = false;
    uint32_t frames[4] = {}, copies = 0, missing = 0, ageFrames = 0;
    float ageMs = 0.f, rotationMaxDeg = 0.f;
    void note(float strip, bool zoomed, bool drawn, uint32_t fillMode, bool copied, bool missingHeld, uint32_t age, float ageMsNow, float rotationDeg) {
        if (!(strip > 0.f) && !zoomed) { active = false; return; }
        if (!active) { *this = ScopeZoomFillTally{}; active = true; }
        if (!drawn) return;
        if (fillMode < 4) ++frames[fillMode];
        if (copied) ++copies;
        if (missingHeld) ++missing;
        ageFrames = age;
        ageMs = ageMsNow;
        if (rotationDeg == rotationDeg && rotationDeg > rotationMaxDeg) rotationMaxDeg = rotationDeg;
    }
};

// The runtime's bookkeeping around one frame's decision (decideScopeZoom, then the geometry the host also needs): which frame opens a press, which ends one and why, and which refusals
// a press carries. A refusal is counted from the first frame the zoom was asked for after it was last not asked for, so the run of asks a press belongs to is what it reports, and
// the refusals of an earlier run (a press that never zoomed, a sight lowered and raised again) are not carried into it. The host traces and tells the d3d11 half what this returns.
struct ScopeZoomEvents {
    bool opened = false;          // a press opened on this frame (the zoom is told, and none was before)
    const char* closed = nullptr; // a press that was open ends on this frame: "asked_off" (no longer asked) or "refused"; the host closes it (reports it, then clears press.open)
    bool whyChanged = false;      // a refusal whose reason differs from the last frame's (traced once per change)
};
struct ScopeZoomRun {
    ScopeZoomPress press;
    ScopeZoomWhy whyNoted = ScopeZoomWhy::kNotAsked;
    uint32_t pending[4] = {};     // refusals of the run of asks in progress that no press has taken
    uint32_t notDrawnRun = 0;     // frames in a row the narrowing went on past a share that was not drawn (the next decision's ScopeZoomInput::notDrawnRun)
    bool standing() const { return press.open; }
    ScopeZoomEvents frame(const ScopeZoomOutput& out, uint64_t nowUs) {
        ScopeZoomEvents ev;
        notDrawnRun = (out.why == ScopeZoomWhy::kApplied && out.passedUnshared) ? notDrawnRun + 1u : 0u;
        if (out.why == ScopeZoomWhy::kApplied) {
            if (!press.open) {
                press.begin(nowUs);
                for (unsigned i = 0; i < 4; ++i) { press.refused[i] = pending[i]; pending[i] = 0; }
                ev.opened = true;
            }
            press.applied(out, nowUs, out.strip > 0.f, true);
            whyNoted = ScopeZoomWhy::kApplied;
            return ev;
        }
        if (press.open) ev.closed = out.why == ScopeZoomWhy::kNotAsked ? "asked_off" : "refused";
        if (out.why == ScopeZoomWhy::kNotAsked) {
            for (unsigned i = 0; i < 4; ++i) pending[i] = 0;
        } else {
            const uint32_t index = scopeZoomRefusalIndex(out.why);
            if (index < 4) ++pending[index];
            ev.whyChanged = out.why != whyNoted;
        }
        whyNoted = out.why;
        return ev;
    }
    // No provider answer or no located geometry on this frame: nothing is narrowed and the run of asks ends. "no_geometry" when a press was open.
    const char* absent() {
        for (unsigned i = 0; i < 4; ++i) pending[i] = 0;
        return press.open ? "no_geometry" : nullptr;
    }
};

}  // namespace edvr::openxr
