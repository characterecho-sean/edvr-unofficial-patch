// Scope zoom, B3: the pure parts of the lens rendered at full detail (docs\design-vr-scope-magnification-2026-10-28.md, section 11). Header only, no game, Windows or D3D11 state, so
// native_frame_test runs the very functions the frame thread's tick, the door's passes, the temporal pass and the runtime use.
//
// A shows the game's zoom as a digital zoom of the aiming (right) eye's picture: 1/M of the output's detail. B3 (the scoped Executioner only, behind
// experimental.native_on_foot_ads_scope_zoom_full_detail) has the game draw the LEFT eye as the zoomed view -- the told frustum becomes the eye's own tangents divided by M, an off-axis
// window about the eye's direction to the far point c, no pose change -- and the lens samples that picture instead. The left eye is covered by the right eye's picture in the headset
// (F162's both mode), so its zoomed picture is never shown; its uncovered strip fades to black while it is zoomed. What is here:
//   1. the window: the zoomed told frustum;
//   2. the reprojection: a lens pixel's world point, from the right eye's depth, through the left eye's zoomed rows (the parallax of the 6.3 cm removed per pixel);
//   3. the decisions: the gate and the hold of the window, the strip's ramp, the lens's source for a frame and why it fell back to A, the reset edge;
//   4. the instrument: the accumulators' final figures and the press line, which tools\edvr_log.py renders its --health fixtures from.
#pragma once

#include "scope_zoom_s1.h"
#include "../common/scope_zoom_window.h"

namespace edvr {

// B3: the temporal pass's word on eye 0 (native_temporal.cpp). nativeTemporalZoom: whether the frame begun at `sequence` has eye 0 drawn with the zoomed window, and both frusta
// ({left, right, down, up} tangents, unjittered), the zoomed one and the one without the zoom. nativeTemporalZoomLatest: the same for the latest frame begun, with its sequence.
// nativeTemporalZoomCounts: the pass's totals since its channel opened (zoomed frames, DLSS resets per eye, resets the zoom's edges made).
bool nativeTemporalZoom(uint64_t sequence, float zoomed[4], float pre[4]);
bool nativeTemporalZoomLatest(uint64_t* sequence, float zoomed[4], float pre[4]);
bool nativeTemporalZoomCounts(uint64_t* zoomFrames, uint64_t resets[2], uint64_t* zoomEdgeResets);

namespace scopezoom {

// ---- 1. the window ------------------------------------------------------------------------------------------------------------------------------------------------------------
constexpr double kB3MaxM = 8.0;            // the most the window narrows by
constexpr double kB3ShareFull = 0.999;     // the zoom starts only with the left eye wholly covered by the right eye's picture ...
constexpr double kB3ShareHold = 0.9;       // ... and is held while the cover is at least this
constexpr double kB3StripRampS = 0.10;     // the left eye's uncovered strip goes black over this long before the zoom is asked for, and comes back over it after
constexpr uint32_t kB3StripHoldTicks = 3;  // ticks the strip stays black after the zoom was asked to end (the runtime's answer lags the ask by a frame or two)
constexpr double kB3CopyHalfDeg = 6.0;     // the part of the left picture the lens may read: this far about the far point's place in it (the parallax of anything past 0.6 m is inside)
constexpr double kB3EdgePx = 2.0;          // output px kept inside the copy for the filter's taps

// The tangents {L, R, D, U} (L, D negative) of the zoomed told frustum: the eye's own span divided by m, centred on the tangent plane's point tc, moved in where it would leave the eye's
// own span. False when m is not in (kLensMinM, kB3MaxM], an input is not a number, the span is empty, or tc lies outside the span.
inline bool b3Window(const double trueTan[4], const double tc[2], double m, double out[4]) noexcept { return scopewin::window(trueTan, tc, m, out); }
// How much the window narrowed the eye: the true span over the zoomed one, horizontal and vertical (m for a window b3Window made); NaN when either is not usable.
inline double b3Narrowing(const double trueTan[4], const double zoomTan[4], int axis) noexcept {
    const int lo = axis ? 2 : 0, hi = axis ? 3 : 1;
    const double t = trueTan[hi] - trueTan[lo], z = zoomTan[hi] - zoomTan[lo];
    return (t > 0.0 && z > 0.0) ? t / z : kNaN;
}
// The zoomed frustum is the one the left rows describe (unjittered): the same spans to a relative tolerance, the same centre to a share of the span.
inline bool b3WindowMatches(const double a[4], const double b[4], double tol) noexcept {
    const double sx = a[1] - a[0], sy = a[3] - a[2];
    if (!(sx > 0.0 && sy > 0.0)) return false;
    return std::fabs((b[1] - b[0]) - sx) <= tol * sx && std::fabs((b[3] - b[2]) - sy) <= tol * sy && std::fabs(b[0] - a[0]) <= tol * sx && std::fabs(b[2] - a[2]) <= tol * sy;
}

// ---- 2. the reprojection ------------------------------------------------------------------------------------------------------------------------------------------------------
// An eye's scene rows (native_on_foot_core.h worldClip's layout: clip_j = (p - o) . rows[0..11] + rows[12 + j]) as one 4 x 4 matrix, row-major, the world point as a row vector
// (x, y, z, 1) times it: the origin folded into the translation row, and the eye's jitter taken out. The rows were drawn with the TAA jitter, a translation of the NDC square by
// (jx, jy) (jitterNdc: twice the uv shift in x, minus twice the uv shift in y); the picture the upscaler hands on is unjittered.
inline void b3ClipMatrix(const float rows[16], const double o[3], double jxNdc, double jyNdc, double a[16]) noexcept {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) a[4 * i + j] = rows[4 * i + j];
    for (int j = 0; j < 4; ++j) a[12 + j] = static_cast<double>(rows[12 + j]) - (o[0] * rows[j] + o[1] * rows[4 + j] + o[2] * rows[8 + j]);
    for (int i = 0; i < 4; ++i) {
        a[4 * i + 0] -= jxNdc * a[4 * i + 3];
        a[4 * i + 1] -= jyNdc * a[4 * i + 3];
    }
}
// Gauss-Jordan with partial pivoting. False for a matrix that is singular to 1e-12 of its largest entry or holds a non-number.
inline bool b3Invert4(const double a[16], double inv[16]) noexcept {
    double m[4][8];
    double big = 0.0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            if (!std::isfinite(a[4 * i + j])) return false;
            big = std::fmax(big, std::fabs(a[4 * i + j]));
            m[i][j] = a[4 * i + j];
            m[i][4 + j] = i == j ? 1.0 : 0.0;
        }
    if (!(big > 0.0)) return false;
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(m[r][c]) > std::fabs(m[p][c])) p = r;
        if (std::fabs(m[p][c]) <= 1.0e-12 * big) return false;
        if (p != c)
            for (int j = 0; j < 8; ++j) std::swap(m[p][j], m[c][j]);
        const double d = m[c][c];
        for (int j = 0; j < 8; ++j) m[c][j] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = m[r][c];
            if (f == 0.0) continue;
            for (int j = 0; j < 8; ++j) m[r][j] -= f * m[c][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) inv[4 * i + j] = m[i][4 + j];
    return true;
}
// T takes a point of the right eye's clip space, as a row (ndc x, ndc y, depth, 1) -- the right picture's pixel with the depth its render holds there -- to the same world point in the
// left eye's clip space, up to the factor 1 / (v . u): T = inverse(A_R) A_L, u = the last column of inverse(A_R) (so that v . u is 1 / clip w of the right eye). The eyes' relative
// transform does not move with the head, so a T made from a frame's rows holds for the frames after it while the two projections stand. False when either matrix cannot be had.
inline bool b3Matrices(const float rowsR[16], const double oR[3], const double jitterR[2], const float rowsL[16], const double oL[3], const double jitterL[2], float T[16], float u[4]) noexcept {
    double aR[16], aL[16], inv[16];
    b3ClipMatrix(rowsR, oR, jitterR[0], jitterR[1], aR);
    b3ClipMatrix(rowsL, oL, jitterL[0], jitterL[1], aL);
    if (!b3Invert4(aR, inv)) return false;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += inv[4 * i + k] * aL[4 * k + j];
            if (!std::isfinite(s)) return false;
            T[4 * i + j] = static_cast<float>(s);
        }
    for (int i = 0; i < 4; ++i) u[i] = static_cast<float>(inv[4 * i + 3]);
    return true;
}
// One lens pixel: the right eye's point (ndc x, ndc y of the UNJITTERED picture, and the depth its render holds there) in the left eye's NDC square. False for a point at or behind the
// left eye, or one the factor cannot place. The shader's text mirrors this step for step.
inline bool b3Reproject(const float T[16], const float u[4], double ndcX, double ndcY, double depth, double out[2]) noexcept {
    const double v[4] = {ndcX, ndcY, depth, 1.0};
    double vt[4];
    for (int j = 0; j < 4; ++j) vt[j] = v[0] * T[j] + v[1] * T[4 + j] + v[2] * T[8 + j] + v[3] * T[12 + j];
    const double den = v[0] * u[0] + v[1] * u[1] + v[2] * u[2] + v[3] * u[3];
    if (!std::isfinite(vt[3]) || !std::isfinite(den) || std::fabs(den) < 1.0e-30 || std::fabs(vt[3]) < 1.0e-30) return false;
    if (!(vt[3] * den > 0.0)) return false;
    out[0] = vt[0] / vt[3];
    out[1] = vt[1] / vt[3];
    return std::isfinite(out[0]) && std::isfinite(out[1]);
}
// The depth a sky pixel (no depth: reverse-Z 0) reads as, so its ray is projected as a point far away and its direction alone decides: the depth of 1e5 m, from the depth of kLensNearM.
constexpr double kB3SkyM = 1.0e5;
inline double b3SkyDepth(double nearDepth) noexcept { return nearDepth * (kLensNearM / kB3SkyM); }
// The left picture's uv of an NDC point of the left eye's square (y up), and the pixel in a picture of w x h.
inline void b3LeftPx(const double ndc[2], uint32_t w, uint32_t h, double px[2]) noexcept {
    px[0] = (ndc[0] * 0.5 + 0.5) * static_cast<double>(w);
    px[1] = (0.5 - ndc[1] * 0.5) * static_cast<double>(h);
}
// The window of the left picture the door copies for the lens (x0, y0, x1, y1 in its pixels): kB3CopyHalfDeg about the window's centre tc in the zoomed tangent plane, cut to the picture.
// False when the picture or the zoomed span is empty, tc is not a number, or nothing is left.
inline bool b3CopyRect(const double zoomTan[4], const double tc[2], uint32_t w, uint32_t h, int rect[4]) noexcept {
    const double sx = zoomTan[1] - zoomTan[0], sy = zoomTan[3] - zoomTan[2];
    if (!w || !h || !(sx > 1.0e-6 && sy > 1.0e-6) || !std::isfinite(tc[0]) || !std::isfinite(tc[1])) return false;
    const double cx = (tc[0] - zoomTan[0]) / sx * static_cast<double>(w), cy = (zoomTan[3] - tc[1]) / sy * static_cast<double>(h);
    const double t = std::tan(kB3CopyHalfDeg / kDeg);
    const double hx = t / sx * static_cast<double>(w), hy = t / sy * static_cast<double>(h);
    rect[0] = std::max(0, static_cast<int>(std::floor(cx - hx)));
    rect[1] = std::max(0, static_cast<int>(std::floor(cy - hy)));
    rect[2] = std::min(static_cast<int>(w), static_cast<int>(std::ceil(cx + hx)));
    rect[3] = std::min(static_cast<int>(h), static_cast<int>(std::ceil(cy + hy)));
    return rect[2] > rect[0] && rect[3] > rect[1];
}
// Where the target at c is in the left picture against where the far point is, in the left picture's pixels: the lens's parallax at c. The depth is the right render's at c; the far
// point is the one the lens is centred on. NaN when either point cannot be placed.
inline double b3ParallaxPx(const float T[16], const float u[4], const double cNdc[2], double depthAtC, double nearDepth, uint32_t w, uint32_t h) noexcept {
    double a[2], b[2], pa[2], pb[2];
    if (!b3Reproject(T, u, cNdc[0], cNdc[1], depthAtC > b3SkyDepth(nearDepth) ? depthAtC : b3SkyDepth(nearDepth), a)) return kNaN;
    if (!b3Reproject(T, u, cNdc[0], cNdc[1], b3SkyDepth(nearDepth), b)) return kNaN;
    b3LeftPx(a, w, h, pa);
    b3LeftPx(b, w, h, pb);
    return std::hypot(pa[0] - pb[0], pa[1] - pb[1]);
}

// ---- 3. the decisions ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The gate of the window. startOk: the window may be latched (it is, once, at the first such frame): the full-detail key on, the lens's gate qualifying, the held entry a rendered one,
// the aiming mode the one that shares the right eye's picture with the left (both), the share at full weight, and the left eye's rows usable. holdOk: the window stays while the key,
// the sights (up, w at 1), the entry and the mode stand and the share still covers (it does not need S or c to be fresh: only the lens does).
struct B3GateIn {
    bool keyOn = false, qualifies = false, rendered = false, shareMode = false, leftRowsOk = false;
    bool sightsFull = false;   // on foot, the sights up, the game's w at 1
    double share = 0.0;
    bool latched = false;
};
struct B3GateOut {
    bool latchNow = false, hold = false;
};
inline B3GateOut b3Gate(const B3GateIn& g) noexcept {
    B3GateOut o;
    const bool common = g.keyOn && g.rendered && g.shareMode;
    const bool startOk = common && g.qualifies && g.leftRowsOk && g.sightsFull && g.share >= kB3ShareFull;
    const bool holdOk = common && g.sightsFull && g.share >= kB3ShareHold;
    o.latchNow = !g.latched && startOk;
    o.hold = g.latched && holdOk;
    return o;
}
// The strip's ramp and the zoom's request. latched is the gate's: the strip goes to 1 over kB3StripRampS while the window is held, the zoom is asked for once it is black, and it comes
// back (after kB3StripHoldTicks ticks of no request) over the same time. dt is the tick's, in seconds.
struct B3Strip {
    double fade = 0.0;
    uint32_t offTicks = 0;
    bool request = false;
};
inline void b3StripStep(B3Strip& s, bool latched, double dt) noexcept {
    const double step = (std::isfinite(dt) && dt > 0.0 ? dt : 0.0) / kB3StripRampS;
    if (latched) {
        s.offTicks = 0;
        s.fade = std::fmin(1.0, s.fade + step);
        s.request = s.fade >= 0.999;
        return;
    }
    s.request = false;
    if (s.offTicks < 1000000u) ++s.offTicks;
    if (s.offTicks > kB3StripHoldTicks) s.fade = std::fmax(0.0, s.fade - step);
}
// The left eye's DLSS history ends where the window starts or ends: its frustum changes M-fold, and a history of the other view would be reprojected into this one.
inline bool b3ResetEdge(bool zoomedBefore, bool zoomedNow) noexcept { return zoomedBefore != zoomedNow; }

// The frame thread's tick, apart from the game's state: what native_on_foot.cpp's s1::b3Tick decides from the facts it gathers (the gate, the window's latch, the strip's ramp, the
// request published to the provider), so native_frame_test runs the very function the tick calls. B3Loop is what the tick keeps between ticks.
struct B3Loop {
    bool latched = false;
    double tc[2] = {kNaN, kNaN}, m = kNaN;   // the window's centre and narrowing, latched with the window and held
    B3Strip strip;
};
struct B3StepIn {
    bool keyOn = false;                     // the scope zoom key and the full-detail key
    bool rendered = false;                  // the held entry is a rendered one (the scoped Executioner)
    bool shareMode = false, qualifies = false, sightsFull = false, leftOk = false;   // b3Gate's facts
    double share = 0.0, dt = 0.0, m = kNaN;
    double tcNow[2] = {kNaN, kNaN};         // the left eye's direction to the far point c on this tick (what the window latches)
};
struct B3StepOut {
    scopewin::Request request;              // published to the provider this tick
    bool asked = false;                     // the key is on and the held entry is a rendered one
    bool latched = false;                   // the window is held after this tick
    bool askedTick = false, noLeftRowsTick = false, heldTick = false, requestTick = false;   // the press line's gate counts, for this tick
    bool frameAsked = false;                // the frame record's asked: the lens qualifies and the window is latched, so the doors' sources count a frame as not zoomed only after the window began
};
inline B3StepOut b3Step(B3Loop& loop, const B3StepIn& in) noexcept {
    B3StepOut o;
    o.asked = in.keyOn && in.rendered;
    B3GateIn g;
    g.keyOn = in.keyOn;
    g.qualifies = in.qualifies;
    g.rendered = in.rendered;
    g.shareMode = in.shareMode;
    g.leftRowsOk = in.leftOk;
    g.sightsFull = in.sightsFull;
    g.share = in.share;
    g.latched = loop.latched;
    const B3GateOut go = b3Gate(g);
    o.askedTick = o.asked && in.shareMode && in.qualifies;
    o.noLeftRowsTick = o.askedTick && in.sightsFull && in.share >= kB3ShareFull && !in.leftOk && !loop.latched;
    const bool now = loop.latched ? go.hold : go.latchNow;
    if (go.latchNow) {   // the window is set at the first frame it can start, with w at 1, and held
        loop.tc[0] = in.tcNow[0];
        loop.tc[1] = in.tcNow[1];
        loop.m = in.m;
    }
    loop.latched = now;
    b3StripStep(loop.strip, now, in.dt);
    o.latched = now;
    o.heldTick = now;
    o.requestTick = loop.strip.request;
    o.request.zoom = loop.strip.request;
    o.request.m = static_cast<float>(loop.m);
    o.request.tc[0] = static_cast<float>(loop.tc[0]);
    o.request.tc[1] = static_cast<float>(loop.tc[1]);
    o.request.strip = static_cast<float>(loop.strip.fade);
    o.frameAsked = in.qualifies && o.asked && now;
    return o;
}
// Outside the sights: no window is held and the strip comes back (it is still published until it has).
inline scopewin::Request b3IdleStep(B3Loop& loop, double dt) noexcept {
    loop.latched = false;
    b3StripStep(loop.strip, false, dt);
    scopewin::Request rq;
    rq.strip = static_cast<float>(loop.strip.fade);
    return rq;
}
// Not armed, or the flat profile: nothing is held and the strip is gone at once.
inline scopewin::Request b3OffStep(B3Loop& loop) noexcept {
    loop.latched = false;
    loop.strip = B3Strip{};
    return scopewin::Request{};
}

// What the readout pin does with eye e's estimate on a frame: both eyes' estimates pair as they always did, but while eye 0 is drawn as the zoomed view its camera is a different one and
// cannot agree with eye 1's, so eye 1's alone vouches for the frame and eye 0's is not put.
enum class B3PinAction : uint32_t { kPair = 0, kSolo = 1, kSkip = 2 };
inline B3PinAction b3PinAction(bool eye0Zoomed, int eye) noexcept {
    if (!eye0Zoomed) return B3PinAction::kPair;
    return eye == 1 ? B3PinAction::kSolo : B3PinAction::kSkip;
}

// A count a press takes as a difference of another module's running total: from the total at the press's start (baseOk: it could be read then) to the total now (nowOk). False -- the
// figure is n/a, never a wrapped difference -- when either reading is missing or the total went backwards (its owner restarted: the temporal pass's channel closed and reopened).
inline bool b3Since(bool baseOk, bool nowOk, uint64_t base, uint64_t now, uint32_t* out) noexcept {
    if (!baseOk || !nowOk || now < base) return false;
    const uint64_t d = now - base;
    *out = d > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(d);
    return true;
}

// What the lens took the frame's picture from. kA is the digital zoom: B3 not asked, or the frame fell back to it for the reason named by the others.
enum class B3Source : uint32_t { kNotAsked = 0, kZoomed = 1, kNotZoomed = 2, kNoCopy = 3, kStale = 4, kNoMatrices = 5, kNoShader = 6 };
constexpr uint32_t kB3Sources = 7;
struct B3SourceIn {
    bool asked = false;        // the full-detail key on and the held entry a rendered one, on a frame the lens qualifies for
    bool zoomedFrame = false;  // the left eye's frustum was told zoomed for this frame (the temporal pass's begin says so)
    bool copyMade = false;     // the left door copied the zoomed picture's window ...
    bool copySameFrame = false;  // ... in this very frame (the same temporal sequence as the right door's)
    bool matricesOk = false;   // T and u from the two eyes' rows, fresh and describing the zoomed window
    bool shaderOk = false;     // the third pass and its resources could be made
};
inline B3Source b3SourceOf(const B3SourceIn& s) noexcept {
    if (!s.asked) return B3Source::kNotAsked;
    if (!s.zoomedFrame) return B3Source::kNotZoomed;
    if (!s.copyMade) return B3Source::kNoCopy;
    if (!s.copySameFrame) return B3Source::kStale;
    if (!s.matricesOk) return B3Source::kNoMatrices;
    if (!s.shaderOk) return B3Source::kNoShader;
    return B3Source::kZoomed;
}

// Whether a builder call of the light grid cache is the zoomed eye's: the zoom stands this frame and the call is the first of the frame at a size an eye is rendered at (the left eye is
// drawn first). A bypass is the cache standing aside for the call -- the game builds it, nothing is stored -- so a wrong guess costs a build, never a wrong grid.
inline bool b3LightGridBypass(bool zoomedFrame, uint32_t eyeCallsBefore, uint32_t width, uint32_t height) noexcept {
    return zoomedFrame && eyeCallsBefore == 0 && width >= 1024 && height >= 1024;
}

// The shadow constants (cb1 rows 298 to 326: 29 rows of four floats): how many rows differ, bitwise, between two copies.
constexpr uint32_t kB3ShadowRows = 29;
inline uint32_t b3RowsChanged(const float* a, const float* b) noexcept {
    uint32_t n = 0;
    for (uint32_t r = 0; r < kB3ShadowRows; ++r)
        if (std::memcmp(a + 4 * r, b + 4 * r, 4 * sizeof(float)) != 0) ++n;
    return n;
}

// ---- 4. the instrument --------------------------------------------------------------------------------------------------------------------------------------------------------
// Index 0 is inside the zoomed span, 1 outside it (the same press's frames before the window and after it).
struct B3Press {
    uint32_t press = 0;
    bool keyOn = false;
    // the frame thread
    uint32_t askedTicks = 0, heldTicks = 0, requestTicks = 0, noLeftRows = 0;   // ticks the window could start, ticks it was held, ticks the zoom was asked for, ticks it could not start for want of the left rows
    double tc[2] = {kNaN, kNaN}, m = kNaN;
    uint32_t driftN = 0;
    double driftSumDeg = 0.0, driftMaxDeg = 0.0;
    // the temporal pass, over the press: counts taken as differences of its running totals (b3Since). temporalKnown false: they could not be taken (no channel at the press's start
    // or end, or its totals went backwards), and the line says n/a for them rather than a difference of totals that were not read.
    uint32_t zoomFrames = 0, resets[2] = {}, zoomEdgeResets = 0;
    bool temporalKnown = true;
    // the door
    uint32_t source[kB3Sources] = {};
    uint32_t copies = 0;
    uint32_t parN = 0;
    double parSumPx = 0.0, parMaxPx = 0.0;
    // the draw path
    uint32_t drawFrames[2] = {};
    uint64_t draws[2][2] = {};   // [eye][inside, outside]: draws summed over the frames
    // the light grid cache (the same rule: lightGridKnown false is n/a)
    uint32_t lgCalls = 0, lgBypass = 0;
    bool lightGridKnown = true;
    // the shadow constants
    uint32_t shN[2][2] = {};
    uint64_t shChanged[2][2] = {};
    uint32_t shPairN[2] = {};
    uint64_t shPairDiff[2] = {};
    // the frame thread's own frame time
    uint32_t ftN[2] = {};
    double ftSum[2] = {}, ftMax[2] = {};
    // the readout pin
    uint32_t pinSolo = 0, pinNoPair = 0;
};
inline double b3Mean(double sum, uint64_t n) noexcept { return n ? sum / static_cast<double>(n) : kNaN; }
// A count that may be unknown: its digits, or n/a.
inline const char* b3Count(bool known, uint32_t v, char* buf, size_t cap) noexcept {
    if (known) std::snprintf(buf, cap, "%u", v);
    else std::snprintf(buf, cap, "n/a");
    return buf;
}
constexpr size_t kB3PressLineBytes = 1168;
inline int formatB3Press(char* buf, size_t n, const B3Press& p) noexcept {
    int len = 0;
    char zf[16], rl[16], rr[16], ed[16], lgb[16], lgc[16];
    b3Count(p.temporalKnown, p.zoomFrames, zf, sizeof(zf));
    b3Count(p.temporalKnown, p.resets[0], rl, sizeof(rl));
    b3Count(p.temporalKnown, p.resets[1], rr, sizeof(rr));
    b3Count(p.temporalKnown, p.zoomEdgeResets, ed, sizeof(ed));
    b3Count(p.lightGridKnown, p.lgBypass, lgb, sizeof(lgb));
    b3Count(p.lightGridKnown, p.lgCalls, lgc, sizeof(lgc));
    char tcx[24], tcy[24], mm[24], dm[24], dx[24], pm[24], px[24];
    char dl[2][2][24], sh[2][2][24], pr[2][24], fm[2][24], fx[2][24];
    const uint64_t dn[2] = {p.drawFrames[0], p.drawFrames[1]};
    for (int e = 0; e < 2; ++e) {
        for (int k = 0; k < 2; ++k) {
            numText(b3Mean(static_cast<double>(p.draws[e][k]), dn[k]), 0, dl[e][k], sizeof(dl[e][k]));
            numText(b3Mean(static_cast<double>(p.shChanged[e][k]), p.shN[e][k]), 2, sh[e][k], sizeof(sh[e][k]));
        }
        numText(b3Mean(static_cast<double>(p.shPairDiff[e]), p.shPairN[e]), 2, pr[e], sizeof(pr[e]));   // [0]: inside, [1]: outside
        numText(b3Mean(p.ftSum[e], p.ftN[e]), 2, fm[e], sizeof(fm[e]));
        numText(p.ftN[e] ? p.ftMax[e] : kNaN, 2, fx[e], sizeof(fx[e]));
    }
    appendf(buf, n, &len, "scope zoom (B3): press %u key %s; asked %u, held %u, zoom asked %u ticks, no left rows %u; zoomed frames %s; tc (%s, %s) M %s, window drift mean %s max %s deg; "
            "DLSS resets L %s R %s (zoom edge %s); ", p.press, p.keyOn ? "on" : "off", p.askedTicks, p.heldTicks, p.requestTicks, p.noLeftRows, zf,
            numText(p.tc[0], 4, tcx, sizeof(tcx)), numText(p.tc[1], 4, tcy, sizeof(tcy)), numText(p.m, 3, mm, sizeof(mm)), numText(b3Mean(p.driftSumDeg, p.driftN), 3, dm, sizeof(dm)),
            numText(p.driftN ? p.driftMaxDeg : kNaN, 3, dx, sizeof(dx)), rl, rr, ed);
    appendf(buf, n, &len, "lens source: zoomed %u, A: not asked %u, not zoomed %u, no copy %u, stale %u, no matrices %u, no shader %u; copies %u; parallax at c mean %s max %s px; ",
            p.source[static_cast<uint32_t>(B3Source::kZoomed)], p.source[static_cast<uint32_t>(B3Source::kNotAsked)], p.source[static_cast<uint32_t>(B3Source::kNotZoomed)],
            p.source[static_cast<uint32_t>(B3Source::kNoCopy)], p.source[static_cast<uint32_t>(B3Source::kStale)], p.source[static_cast<uint32_t>(B3Source::kNoMatrices)],
            p.source[static_cast<uint32_t>(B3Source::kNoShader)], p.copies, numText(b3Mean(p.parSumPx, p.parN), 1, pm, sizeof(pm)), numText(p.parN ? p.parMaxPx : kNaN, 1, px, sizeof(px)));
    appendf(buf, n, &len, "draws a frame in/out L %s/%s R %s/%s (frames %u/%u); light grid bypass %s of %s calls; shadow rows changed a frame in/out L %s/%s R %s/%s, L vs R %s/%s; "
            "frame ms in/out %s/%s (max %s/%s); readout pin solo %u, no pair %u", dl[0][0], dl[0][1], dl[1][0], dl[1][1], p.drawFrames[0], p.drawFrames[1], lgb, lgc,
            sh[0][0], sh[0][1], sh[1][0], sh[1][1], pr[0], pr[1], fm[0], fm[1], fx[0], fx[1], p.pinSolo, p.pinNoPair);
    return finishLine(buf, n, len);
}

// ---- 5. the runtime's report line ---------------------------------------------------------------------------------------------------------------------------------------------------
// The line the d3d11 half writes for the runtime's report of a press (EdvrNativeScopeZoomReport, scope_zoom_window.h; edvrNoteNativeScopeZoom). What each getter last ANSWERED eye 0 with
// (the vertical tangent span, not the frusta the runtime told): the matrix getter's is the narrowed one and the raw getter's the true one, so their ratio is the narrowing exactly. And who read
// the raw getter, by return address as an RVA in the game's executable, so a flight that shows the sky (or anything else) at the wrong scale names the caller to narrow. A span or a caller
// nobody produced is the word none, so a rule can tell a getter that was never read from one that was.
constexpr size_t kB3RuntimeLineBytes = 1168;
inline const char* b3Callers(const EdvrNativeScopeZoomReport& r, char* buf, size_t cap) noexcept {
    size_t used = 0;
    buf[0] = '\0';
    for (unsigned i = 0; i < EDVR_NATIVE_SCOPE_ZOOM_REPORT_CALLERS; ++i) {
        if (!r.rawCallerReads[i]) continue;
        const int w = r.rawCallerRva[i] == 0xFFFFFFFFu ? std::snprintf(buf + used, cap - used, "%soutside:%u", used ? " " : "", r.rawCallerReads[i])
                                                       : std::snprintf(buf + used, cap - used, "%s0x%X:%u", used ? " " : "", r.rawCallerRva[i], r.rawCallerReads[i]);
        if (w < 0 || static_cast<size_t>(w) >= cap - used) break;
        used += static_cast<size_t>(w);
    }
    if (!used) std::snprintf(buf, cap, "none");
    return buf;
}
inline int formatB3Runtime(char* buf, size_t n, const EdvrNativeScopeZoomReport& r) noexcept {
    int len = 0;
    char rs[24], ms[24], ratio[24], callers[96];
    const double rawSpan = r.rawSpan > 0.f ? static_cast<double>(r.rawSpan) : kNaN, matrixSpan = r.matrixSpan > 0.f ? static_cast<double>(r.matrixSpan) : kNaN;
    appendf(buf, n, &len,
            "scope zoom (B3) runtime: zoomed frames %u over %.2f s, peak narrowing %.3f; refused: answer not version 11 %u, share not at full weight %u, share not drawn %u, geometry %u; "
            "strip darkened %u frames, hidden-area mesh withheld %u frames; eye 0's told frustum without the zoom (left, right, down, up tangents) %+.4f/%+.4f/%+.4f/%+.4f, with it "
            "%+.4f/%+.4f/%+.4f/%+.4f; ",
            r.frames, static_cast<double>(r.seconds), static_cast<double>(r.peakM), r.refused[0], r.refused[1], r.refused[2], r.refused[3], r.stripFrames, r.meshWithheld,
            static_cast<double>(r.trueTan[0]), static_cast<double>(r.trueTan[1]), static_cast<double>(r.trueTan[2]), static_cast<double>(r.trueTan[3]), static_cast<double>(r.zoomTan[0]),
            static_cast<double>(r.zoomTan[1]), static_cast<double>(r.zoomTan[2]), static_cast<double>(r.zoomTan[3]));
    appendf(buf, n, &len,
            "the game's reads of eye 0's projection while it stood: raw getter %u (its UI scale reads it; vertical tangent span answered %s), matrix getter %u (the picture is drawn with it; span %s), "
            "span ratio raw over matrix %s; raw getter callers (RVA in the game's executable: reads) %s, other callers %u",
            r.rawReads, numText(rawSpan, 4, rs, sizeof(rs)), r.matrixReads, numText(matrixSpan, 4, ms, sizeof(ms)),
            numText(std::isfinite(rawSpan) && std::isfinite(matrixSpan) ? rawSpan / matrixSpan : kNaN, 3, ratio, sizeof(ratio)), b3Callers(r, callers, sizeof(callers)), r.rawCallerOther);
    return finishLine(buf, n, len);
}

}  // namespace scopezoom
}  // namespace edvr
