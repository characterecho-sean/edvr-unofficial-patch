// The first-person fold-in's PHASE term on WARP (docs/design-flat-temporal-aa-2026-09-23.md section 82, stage 2): the prep kernel's "THE PHASE
// of a jittered world", read from FlatMonoResolveFrame::firstPersonPhaseMode. When the world and the first-person camera carry the same sub-pixel
// phase, the VR weapon map's vector is previous minus current at the two frames' OWN raster phases, and the backend wants the true motion with both
// phases out of it, as the camera and engine terms already have them out.
//
// THE CLAIM, proved here and not assumed. A surface at TRUE (unjittered) positions P_cur (this frame) and P_prev (last frame), in render pixels, is
// rasterised at P_cur + c and P_prev + p (c the current phase, jitter.xy in the prep; p the previous, jitter.zw; positive right and down: content
// moves right by +x and down by +y). The map holds (P_prev + p) - (P_cur + c), so the vector the backend wants, P_prev - P_cur, is the map's plus
// (c - p) = jitter.xy - jitter.zw. The cases never use that relation to build anything: each NAMES P_cur and P_prev, the harness ADDS the phases to get
// the two raster positions independently, stores previous-minus-current at the texel the current raster position lands in, runs the real prep, and
// demands the output equal P_prev - P_cur (mode 1) or the map as stored (mode 0). The expectations come from the positions, never from m + (c - p);
// the one texel that is not built from positions is a raw negative-zero vector, which is there to pin mode 0's bit pattern (see below).
//
// What the scenario pins, on the shipped prep and then, as MUTATION CHECKS, on the prep with one rule flipped at a time (the mutated source is
// compiled here, handed to the resolver through flatMonoResolveTestPrepBytecode, and the scenario must FAIL; a scenario that cannot fail proves
// nothing): in ten phase pairs (c == p, where modes 0 and 1 agree; large opposite phases, both ways; an x phase alone; a y phase alone; one frame
// unjittered and the other not, each way; no phase at all; two non-dyadic Halton points) and twelve surface points each (a static weapon; motion
// along +x, -x, +y and -y; both diagonals; a sixteenth of a pixel; up to six pixels), mode 1 gives P_prev - P_cur and mode 0 gives the map exactly
// as stored. "Gives" means to 1e-4 px, and in the nine pairs whose phases are multiples of 1/16 every quantity is exact in fp16, so the error is 0;
// the Halton pair's phases are not dyadic, so its bound adds the fp16 rounding of the map and of the output (about 2e-3 px at the largest vectors).
// Mode 2 and a value that is no mode (7) reject every attached pixel with no motion, exactly as an invalid texel is rejected; the validity test runs
// on the UNcorrected vector (edge rows on both axes and both sides, phase signs both ways: a previous RASTER position that is outside the frame
// while the true one is inside is rejected in every mode, and one that is inside while the true one is outside is accepted, the frame's edge itself
// included); a texel that is invalid stays rejected in every mode (w 0 and 2, a depth that disagrees, a previous position far outside on each
// axis, NaN); a pixel without the stencil bit equals a run without the inputs bit for bit in every mode; a reset frame rejects every attached pixel
// in mode 1 too; a negative-zero vector (half 0x8000, no positions behind it: the surface did not move in the raster) comes out as negative zero
// in mode 0, so no zero term is added to it, and as c - p in mode 1; a surface that moves exactly as the world does, under a moving camera and the
// same two phases, gets in mode 1 the very vector the world's camera term gives its neighbours, and in mode 0 is off by p - c.
//
// The contract around it, on the shipped prep: leaving the field unset is mode 0 bit for bit, and mode 0 is bit for bit the prep as it was BEFORE the
// mode existed (the same source with the mode line deleted, compiled here, run on the same frames: every output byte equal); the constants carry the
// mode in route.w exactly when the pair is bound (a probe prep that writes route.w and route.z out: bound frames carry the mode, 2 and 7 verbatim;
// absent, partial and refused pairs carry 0); the frames are counted by mode in stats.firstPersonPhaseFrames ([0] mode 0, [1] mode 1, [2] anything
// else), only when the pair is bound; and the mode adds no log line.
#pragma once
#include <cmath>
#include <cstring>

namespace fpphase {

using fpgpu::H;
using fpgpu::Seen;
using fpgpu::W;
using fpgpu::zOk;

// A frame's two raster phases, in render pixels, positive right and down: c is the CURRENT frame's (the prep's jitter.xy), p the PREVIOUS one's
// (jitter.zw). `dyadic`: every value is a multiple of 1/16, so every quantity below is exact in fp16 and in float.
struct Phase { const char* name; float cx, cy, px, py; bool dyadic; };

enum class Kind : uint8_t { Unattached, Valid, Rejected, NegativeZero };

// One surface point: its TRUE (unjittered) position this frame and last frame, render pixels. `kind` is what the map must make of it (Valid, or
// Rejected because its previous RASTER position is outside the frame); `centred` says its current raster position is exactly a pixel centre, so the
// prep's previous position (pixel centre + the map's vector) is exactly its previous raster position, which the edge rows rely on.
struct Point { const char* what; double curX, curY, prevX, prevY; Kind kind = Kind::Valid; bool centred = false; };

// What a texel must come out as, in each mode, worked out from the positions alone.
struct Expect {
    Kind kind = Kind::Unattached;
    double uncorrected[2]{};   // what the map holds (mode 0 must give exactly this)
    double truth[2]{};         // P_prev - P_cur (mode 1 must give this)
};

// The ten phase pairs. Every |value| is at most 0.5, the resolver's own bound on a phase.
constexpr Phase kPairs[] = {
    {"c == p", 0.25f, -0.375f, 0.25f, -0.375f, true},
    {"mixed signs", 0.25f, 0.125f, -0.375f, 0.25f, true},
    {"large opposite phases", 0.5f, -0.5f, -0.5f, 0.5f, true},
    {"large opposite phases, reversed", -0.5f, 0.5f, 0.5f, -0.5f, true},
    {"x phase only, current frame", 0.375f, 0.0f, 0.0f, 0.0f, true},
    {"y phase only, previous frame", 0.0f, 0.0f, 0.0f, -0.4375f, true},
    {"current frame unjittered, previous jittered", 0.0f, 0.0f, 0.4375f, -0.3125f, true},
    {"previous frame unjittered, current jittered", -0.1875f, 0.4375f, 0.0f, 0.0f, true},
    {"no phase at all", 0.0f, 0.0f, 0.0f, 0.0f, true},
    {"two Halton (2,3) points, centred", 0.25f, -0.38888889f, -0.25f, 0.16666667f, false},
};
// The twelve surface points every pair is run with: explicit positions, all interior, so that any phase within 0.5 keeps the previous raster
// position in the frame and no two points share a texel.
constexpr Point kPoints[] = {
    {"a static weapon",              2.5,   3.0,   2.5,    3.0},
    {"moving right",                 5.5,   3.25,  7.0,    3.25},
    {"moving left",                  8.75,  3.0,   6.5,    3.0},
    {"moving down",                  11.5,  3.5,   11.5,   4.25},
    {"moving up",                    2.75,  7.0,   2.75,   5.75},
    {"diagonal, +x -y",              5.5,   7.25,  8.0,    5.5},
    {"diagonal, -x +y",              8.5,   7.0,   7.875,  10.125},
    {"a sixteenth of a pixel",       11.25, 7.0,   11.3125, 6.96875},
    {"large, +x -y",                 3.0,   11.0,  9.0,    6.0},
    {"large, -x +y",                 6.25,  11.5,  1.75,   14.5},
    {"half a pixel, -x +y",          9.5,   11.0,  9.0,    11.5},
    {"two pixels up, nothing across", 12.5, 11.25, 12.5,   9.25},
};
// The frame's edge, under p = (-0.375, -0.25) with c = (0.25, -0.125): every current raster position is a pixel centre. A previous RASTER position
// just outside the frame (-0.125) while the TRUE one is inside (0.25) is rejected; just inside (15.875) while the true one is outside (16.25) is
// accepted; exactly on the edge (0 and 16) is inside. The prep's validity test runs on the map's own vector, so all of this must hold in every mode.
constexpr Phase kEdgeA = {"edge rows, p = (-0.375, -0.25)", 0.25f, -0.125f, -0.375f, -0.25f, true};
constexpr Point kEdgesA[] = {
    {"right edge: raster 15.875, truth 16.25",    15.25, 2.625,  16.25,  2.625,  Kind::Valid,    true},
    {"right edge exactly: raster 16, truth 16.375", 15.25, 4.625, 16.375, 4.625,  Kind::Valid,    true},
    {"bottom edge: raster 15.875, truth 16.125",  2.25,  15.625, 2.25,   16.125, Kind::Valid,    true},
    {"bottom edge exactly: raster 16, truth 16.25", 4.25, 15.625, 4.25,   16.25,  Kind::Valid,    true},
    {"left edge: raster -0.125, truth 0.25",      0.25,  6.625,  0.25,   6.625,  Kind::Rejected, true},
    {"left edge exactly: raster 0, truth 0.375",  0.25,  8.625,  0.375,  8.625,  Kind::Valid,    true},
    {"top edge: raster -0.125, truth 0.125",      6.25,  0.625,  6.25,   0.125,  Kind::Rejected, true},
    {"top edge exactly: raster 0, truth 0.25",    8.25,  0.625,  8.25,   0.25,   Kind::Valid,    true},
};
// The same under p = (+0.375, +0.25), c = (-0.25, 0.125): the other four edges, the other way round.
constexpr Phase kEdgeB = {"edge rows, p = (0.375, 0.25)", -0.25f, 0.125f, 0.375f, 0.25f, true};
constexpr Point kEdgesB[] = {
    {"left edge: raster 0.125, truth -0.25",      0.75,  2.375,  -0.25,  2.375,  Kind::Valid,    true},
    {"left edge exactly: raster 0, truth -0.375", 0.75,  4.375,  -0.375, 4.375,  Kind::Valid,    true},
    {"top edge: raster 0.125, truth -0.125",      2.75,  0.375,  2.75,   -0.125, Kind::Valid,    true},
    {"top edge exactly: raster 0, truth -0.25",   4.75,  0.375,  4.75,   -0.25,  Kind::Valid,    true},
    {"right edge: raster 16.125, truth 15.75",    15.75, 6.375,  15.75,  6.375,  Kind::Rejected, true},
    {"right edge exactly: raster 16, truth 15.625", 15.75, 8.375, 15.625, 8.375, Kind::Valid,    true},
    {"bottom edge: raster 16.125, truth 15.875",  6.75,  15.375, 6.75,   15.875, Kind::Rejected, true},
    {"bottom edge exactly: raster 16, truth 15.75", 8.75, 15.375, 8.75,   15.75,  Kind::Valid,    true},
};

// The modes a case is run in. The first is the field left at its default (never assigned): it must be mode 0, bit for bit.
struct ModeRun { const char* name; bool set; uint32_t mode; };
constexpr ModeRun kModes[] = {
    {"field unset", false, 0}, {"mode 0", true, 0}, {"mode 1", true, 1}, {"mode 2", true, 2}, {"mode 7, which is no mode", true, 7},
};
constexpr size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

inline bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }
// The fp16 value a float is stored as (what the map and the prep's motion output hold).
inline double stored(float v) { return double(half(fpgpu::toHalf(v))); }
// The gap between adjacent fp16 values around v: a budget for the one rounding the prep's output does (and the map's) when a phase is not a
// dyadic fraction. Zero cases need none.
inline double halfSpacing(double v) {
    v = std::fabs(v);
    if (v < 6.1e-5) return 5.96e-8;
    int e = 0; std::frexp(v, &e);
    return std::ldexp(1.0, e - 11);
}

// The texels of one case: a valid-looking map under everything, the surface points with their maps built from positions, the texels an attached
// pixel must reject whatever the mode, and one negative-zero vector.
inline void buildFrame(fpgpu::Fixture& fx, const Phase& ph, const std::vector<Point>& points, std::vector<Expect>& ex) {
    std::vector<fpgpu::Spec>& s = fx.specs;
    s.assign(size_t(W) * H, fpgpu::Spec{});
    ex.assign(size_t(W) * H, Expect{});
    // Everything starts unattached, with a map under it that looks perfectly valid and moves seven pixels: any pixel that takes it by mistake
    // is wrong by far more than a rounding. Half of them carry a stencil value with other bits set, none the first-person bit.
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x) {
            fpgpu::Spec& t = s[y * W + x];
            t.stencil = ((x + y) & 1) ? 0x01 : 0x00; t.depth = zOk; t.map = fpgpu::mapOf(7, 7, zOk);
        }
    char what[256];
    // The surface points. raster_cur = P_cur + c and raster_prev = P_prev + p, worked out here by addition, independently of any relation between
    // them; the map texel sits at the raster pixel of the current frame and holds previous minus current, in rasters.
    for (const Point& pt : points) {
        const double rcx = pt.curX + double(ph.cx), rcy = pt.curY + double(ph.cy);
        const double rpx = pt.prevX + double(ph.px), rpy = pt.prevY + double(ph.py);
        const int qx = int(std::floor(rcx)), qy = int(std::floor(rcy));
        std::snprintf(what, sizeof(what), "first person phase fixture (%s): \"%s\" lands inside the frame", ph.name, pt.what);
        const bool inside = qx >= 0 && qy >= 0 && qx < int(W) && qy < int(H);
        check(inside, what);
        if (!inside) continue;
        Expect& e = ex[size_t(qy) * W + size_t(qx)];
        std::snprintf(what, sizeof(what), "first person phase fixture (%s): \"%s\" has a texel of its own", ph.name, pt.what);
        check(e.kind == Kind::Unattached, what);
        if (e.kind != Kind::Unattached) continue;
        const bool rasterInside = rpx >= 0 && rpy >= 0 && rpx <= double(W) && rpy <= double(H);
        std::snprintf(what, sizeof(what), "first person phase fixture (%s): \"%s\" is declared %s, and its previous raster position is %s the frame",
                      ph.name, pt.what, pt.kind == Kind::Valid ? "valid" : "rejected", rasterInside ? "inside" : "outside");
        check(rasterInside == (pt.kind == Kind::Valid), what);
        if (pt.centred) {
            std::snprintf(what, sizeof(what), "first person phase fixture (%s): \"%s\" is at a pixel centre in the current raster", ph.name, pt.what);
            check(rcx == double(qx) + 0.5 && rcy == double(qy) + 0.5, what);
        }
        const float mx = float(rpx - rcx), my = float(rpy - rcy);
        fpgpu::Spec& t = s[size_t(qy) * W + size_t(qx)];
        t.stencil = 0x10; t.depth = zOk; t.map = fpgpu::mapOf(mx, my, zOk);
        e.kind = pt.kind;
        e.uncorrected[0] = stored(mx); e.uncorrected[1] = stored(my);
        e.truth[0] = pt.prevX - pt.curX; e.truth[1] = pt.prevY - pt.curY;
    }
    // Texels an attached pixel must reject in every mode, whatever the phase.
    struct Invalid { UINT x; fpgpu::Half4 map; };
    const Invalid invalids[] = {
        {0, fpgpu::mapOf(1, 1, zOk, 0.0f)},                      // uncovered
        {2, fpgpu::mapOf(1, 1, zOk, 2.0f)},                      // new
        {4, fpgpu::mapOf(1, 1, zOk * 1.01f)},                    // a depth that disagrees
        {6, fpgpu::mapOf(-10, 0, zOk)},                          // previous x far outside
        {8, fpgpu::Half4{fpgpu::kHalfNaN, 0, fpgpu::toHalf(zOk), fpgpu::toHalf(1)}},   // NaN
        {10, fpgpu::mapOf(0, 12, zOk)},                          // previous y far outside
    };
    for (const Invalid& iv : invalids) {
        fpgpu::Spec& t = s[14 * W + iv.x]; t.stencil = 0x10; t.depth = zOk; t.map = iv.map;
        ex[14 * W + iv.x].kind = Kind::Rejected;
    }
    // A vector that is exactly negative zero: half 0x8000 on both axes.
    { fpgpu::Spec& t = s[14 * W + 14]; t.stencil = 0x10; t.depth = zOk; t.map = fpgpu::Half4{0x8000, 0x8000, fpgpu::toHalf(zOk), fpgpu::toHalf(1)};
      ex[14 * W + 14].kind = Kind::NegativeZero; }
    fx.upload();
}

// One resolve: the frame's own phases are what the stub backend must be told (it checks the current one).
inline Seen resolve(fpgpu::Fixture& fx, const edvr::FlatMonoResolveFrame& f) {
    expectedJx = f.jitterX; expectedJy = f.jitterY;
    return fpgpu::resolveFrame(fx, f);
}
// A frame on the copy route in Dlss mode, still camera unless cameraX says otherwise (the previous camera stays at 0, so a nonzero cameraX is one
// render pixel of motion for every world pixel, which the world route's own test established).
inline edvr::FlatMonoResolveFrame frameFor(fpgpu::Fixture& fx, const Phase& ph, bool inputs, const ModeRun& mode, uint64_t frame, bool reset,
                                           float cameraX = 0.0f) {
    edvr::FlatMonoResolveFrame f = fx.baseFrame(edvr::FlatMonoResolveMode::Dlss);
    f.frame = frame; f.reset = reset; f.camera[5][0] = cameraX;
    f.jitterX = ph.cx; f.jitterY = ph.cy; f.previousJitterX = ph.px; f.previousJitterY = ph.py;
    if (inputs) { f.firstPersonMotion = fx.mapView.Get(); f.firstPersonStencil = fx.stencilView.Get(); }
    if (mode.set) f.firstPersonPhaseMode = mode.mode;
    return f;
}

// The worst error of a valid point against what its mode must give: mode 0 against the map as stored, mode 1 against the true motion, the latter
// split into the dyadic phase pairs (exact in fp16) and the one pair whose phases are not (bounded by the fp16 rounding of its map and its output).
struct Audit { double worst0 = 0, worst1 = 0, worst1Fp16 = 0; size_t cases = 0, valid = 0; };

// The judgement of one frame in one mode. `bare` is the same case without the inputs.
inline void judgePhase(const std::vector<Expect>& ex, const Phase& ph, const ModeRun& mr, const Seen& got, const Seen& bare, const char* caseName,
                  Audit* audit) {
    char what[384];
    const size_t n = size_t(W) * H;
    const bool sized = got.motion.size() == n * 2 && got.mask.size() == n && bare.motion.size() == n * 2 && bare.mask.size() == n;
    std::snprintf(what, sizeof(what), "first person phase (%s, %s, %s): the prep's whole output reached the backend", caseName, ph.name, mr.name);
    check(sized, what);
    if (!sized) return;
    const uint32_t mode = mr.mode;
    // c - p as the prep computes it, in float.
    const double dc[2] = {double(ph.cx - ph.px), double(ph.cy - ph.py)};
    bool okValid = true, okRejected = true, okUnattached = true, okNegZero = true;
    double worst = 0;
    for (size_t t = 0; t < n; ++t) {
        const Expect& e = ex[t];
        const float mx = got.motion[2 * t], my = got.motion[2 * t + 1];
        const bool rej = got.mask[t] != 0, rejectedNoMotion = rej && mx == 0 && my == 0;
        switch (e.kind) {
            case Kind::Unattached:
                if (!(rej == (bare.mask[t] != 0) && sameBits(mx, bare.motion[2 * t]) && sameBits(my, bare.motion[2 * t + 1]))) okUnattached = false;
                break;
            case Kind::Rejected:
                if (!rejectedNoMotion) okRejected = false;
                break;
            case Kind::Valid: {
                if (mode >= 2) { if (!rejectedNoMotion) okValid = false; break; }
                const double* want = mode == 0 ? e.uncorrected : e.truth;
                double tol[2] = {1e-4, 1e-4};
                if (mode == 1 && !ph.dyadic)
                    for (int a = 0; a < 2; ++a) tol[a] += halfSpacing(e.uncorrected[a]) + halfSpacing(e.truth[a]);
                const double errX = std::fabs(double(mx) - want[0]), errY = std::fabs(double(my) - want[1]);
                worst = std::fmax(worst, std::fmax(errX, errY));
                if (rej || errX > tol[0] || errY > tol[1]) okValid = false;
                if (audit) ++audit->valid;
                break;
            }
            case Kind::NegativeZero: {
                if (mode >= 2) { if (!rejectedNoMotion) okNegZero = false; break; }
                if (mode == 0) {
                    // Untouched: the map's -0 comes out as -0, so no zero term was added to it.
                    if (rej || mx != 0 || my != 0 || !std::signbit(mx) || !std::signbit(my)) okNegZero = false;
                    break;
                }
                double tol[2] = {1e-4, 1e-4};
                if (!ph.dyadic) for (int a = 0; a < 2; ++a) tol[a] += halfSpacing(dc[a]);
                if (rej || std::fabs(double(mx) - dc[0]) > tol[0] || std::fabs(double(my) - dc[1]) > tol[1]) okNegZero = false;
                break;
            }
        }
    }
    if (audit) {
        if (mode == 0) audit->worst0 = std::fmax(audit->worst0, worst);
        else if (mode == 1) { double& w = ph.dyadic ? audit->worst1 : audit->worst1Fp16; w = std::fmax(w, worst); }
    }
    auto report = [&](bool ok, const char* text) {
        std::snprintf(what, sizeof(what), "first person phase (%s, %s, %s): %s", caseName, ph.name, mr.name, text); check(ok, what);
    };
    report(okValid, mode == 0 ? "an attached pixel with a valid map takes the map's vector exactly as stored (the uncorrected value)"
                   : mode == 1 ? "an attached pixel with a valid map takes the TRUE motion, P_prev - P_cur, to 1e-4 px: the map's vector with both phases out of it"
                               : "an attached pixel with a valid map is rejected with no motion, as an invalid texel is: the phase is not known to be the world's");
    report(okRejected, "a rejected texel (w 0 or 2, a depth that disagrees, a previous raster position outside the frame, NaN) is rejected with no motion in every mode");
    report(okUnattached, "a pixel without the stencil bit equals a run without the inputs bit for bit, whatever the mode");
    report(okNegZero, mode == 0 ? "a negative-zero vector comes out as negative zero in mode 0: no zero term is added to the map's vector"
                      : mode == 1 ? "a negative-zero vector comes out as c - p in mode 1" : "a negative-zero vector is rejected with no motion when the mode is not 0 or 1");
}

// Every texel rejected with no motion: the reset frame, in whatever mode.
inline void judgeReset(const Seen& got, const char* tag) {
    char what[256];
    const size_t n = size_t(W) * H;
    bool ok = got.motion.size() == n * 2 && got.mask.size() == n;
    for (size_t t = 0; ok && t < n; ++t) ok = got.mask[t] != 0 && got.motion[2 * t] == 0 && got.motion[2 * t + 1] == 0;
    std::snprintf(what, sizeof(what), "first person phase (%s): every pixel is rejected with no motion, attached ones included: a reset frame has no history, map or mode or not", tag);
    check(ok, what);
}

struct Run { fpgpu::Fixture& fx; uint64_t frame; Audit audit; };
struct CaseResult { Seen bare; Seen seen[kModeCount]; std::vector<Expect> ex; };

// One case: the texels built from the points, the same frame without the inputs, then every mode, each judged. Six consecutive frames.
inline CaseResult runCase(Run& r, const Phase& ph, const std::vector<Point>& points, const char* caseName, float cameraX) {
    CaseResult out;
    buildFrame(r.fx, ph, points, out.ex);
    out.bare = resolve(r.fx, frameFor(r.fx, ph, false, kModes[0], r.frame++, false, cameraX));
    ++r.audit.cases;
    for (size_t i = 0; i < kModeCount; ++i) {
        out.seen[i] = resolve(r.fx, frameFor(r.fx, ph, true, kModes[i], r.frame++, false, cameraX));
        judgePhase(out.ex, ph, kModes[i], out.seen[i], out.bare, caseName, &r.audit);
    }
    char what[256];
    std::snprintf(what, sizeof(what), "first person phase (%s, %s): leaving the mode field unset is mode 0, the prep's whole output bit for bit", caseName, ph.name);
    check(out.seen[0].hash != 0 && out.seen[0].hash == out.seen[1].hash, what);
    return out;
}

inline std::vector<Point> general() { return std::vector<Point>(std::begin(kPoints), std::end(kPoints)); }

// The whole scenario, against whichever prep the resolver was last initialised with. Nothing in it is specific to the shipped one.
inline void phaseScenario(fpgpu::Fixture& fx) {
    edvr::flatMonoResolveReset();
    fx.fillColor(fpgpu::grey128);
    Run r{fx, 61000, {}};
    std::vector<Expect> ex;
    char what[256];
    // 0. A reset frame first: every texel rejects in mode 1 too (the phases there are whatever the caller says; there is no history).
    buildFrame(fx, kPairs[1], general(), ex);
    judgeReset(resolve(fx, frameFor(fx, kPairs[1], true, kModes[2], r.frame++, true)), "a reset frame, mode 1");

    // 1. The ten phase pairs.
    for (const Phase& ph : kPairs) {
        const CaseResult c = runCase(r, ph, general(), "surface points", 0.0f);
        if (!mutationFailures && &ph == &kPairs[1] && c.seen[2].motion.size() == size_t(W) * H * 2) {
            // One worked row, for the record: the positions, their rasters, the map, and what the prep makes of it.
            const Point& pt = kPoints[1];
            const double rcx = pt.curX + double(ph.cx), rcy = pt.curY + double(ph.cy), rpx = pt.prevX + double(ph.px), rpy = pt.prevY + double(ph.py);
            const size_t q = size_t(std::floor(rcy)) * W + size_t(std::floor(rcx));
            std::printf("flat mono resolve: first-person phase: worked example, \"%s\" under c=(%g,%g) p=(%g,%g): P_cur=(%g,%g) P_prev=(%g,%g), raster_cur=(%g,%g), "
                        "raster_prev=(%g,%g), map=(%g,%g); the prep gives (%g,%g) in mode 0 and (%g,%g) in mode 1, P_prev - P_cur = (%g,%g)\n",
                        pt.what, double(ph.cx), double(ph.cy), double(ph.px), double(ph.py), pt.curX, pt.curY, pt.prevX, pt.prevY, rcx, rcy, rpx, rpy, rpx - rcx, rpy - rcy,
                        double(c.seen[1].motion[2 * q]), double(c.seen[1].motion[2 * q + 1]), double(c.seen[2].motion[2 * q]), double(c.seen[2].motion[2 * q + 1]),
                        pt.prevX - pt.curX, pt.prevY - pt.curY);
        }
        // The bare run is the camera term of a still camera: valid, at rest. Without that the comparison below could be between two rejected frames.
        bool bareOk = true;
        for (size_t t = 0; t < c.ex.size(); ++t)
            if (c.ex[t].kind == Kind::Unattached && (c.bare.mask[t] != 0 || std::fabs(c.bare.motion[2 * t]) > 1e-3 || std::fabs(c.bare.motion[2 * t + 1]) > 1e-3)) bareOk = false;
        std::snprintf(what, sizeof(what), "first person phase (%s): the run without the inputs gives a still camera's world pixels (valid, at rest), so the comparisons are not vacuous", ph.name);
        check(bareOk, what);
        if (ph.cx == ph.px && ph.cy == ph.py) {
            // c == p: the correction is exactly zero, so modes 0 and 1 agree on every texel.
            bool agree = true;
            for (size_t t = 0; t < c.ex.size(); ++t)
                agree = agree && (c.seen[1].mask[t] == c.seen[2].mask[t]) && c.seen[1].motion[2 * t] == c.seen[2].motion[2 * t] &&
                        c.seen[1].motion[2 * t + 1] == c.seen[2].motion[2 * t + 1];
            std::snprintf(what, sizeof(what), "first person phase (%s): with c == p modes 0 and 1 agree on every texel", ph.name);
            check(agree, what);
        }
    }
    // 2. The frame's edge, both signs of p.
    runCase(r, kEdgeA, std::vector<Point>(std::begin(kEdgesA), std::end(kEdgesA)), "frame edges", 0.0f);
    runCase(r, kEdgeB, std::vector<Point>(std::begin(kEdgesB), std::end(kEdgesB)), "frame edges", 0.0f);

    // 3. A surface that moves as the world does. The camera moves, so the world's own vector under these two phases is one render pixel along x
    // (flat_mono_resolve_test's established reading); first-person surfaces whose true motion is exactly that must, in mode 1, come out as the very
    // vector the world's camera term gives the pixels around them: the phases are out of both, and out the same way.
    {
        const Phase ph = {"the established jitter pair", 0.25f, -0.375f, -0.25f, 0.375f, true};
        buildFrame(fx, ph, {}, ex);
        const Seen world = resolve(fx, frameFor(fx, ph, false, kModes[0], r.frame++, false, .3125f));
        // An interior texel: the camera's one pixel of motion takes the last column's previous position out of the frame (rejected, motion 0).
        const size_t refTexel = size_t(8) * W + 8;
        const bool sized = world.motion.size() == size_t(W) * H * 2 && world.mask.size() == size_t(W) * H && world.mask[refTexel] == 0;
        const double wx = sized ? double(world.motion[2 * refTexel]) : 0, wy = sized ? double(world.motion[2 * refTexel + 1]) : 0;
        check(sized && std::fabs(wx - 1.0) < 1e-3 && std::fabs(wy) < 1e-3, "first person phase (locked to the world): the world's camera term under these phases is the established one render pixel along x");
        std::vector<Point> locked;
        for (size_t i : {size_t(0), size_t(1), size_t(3), size_t(5), size_t(9), size_t(11)}) {
            Point p = kPoints[i]; p.prevX = p.curX + wx; p.prevY = p.curY + wy; p.what = "a first-person surface moving as the world does";
            locked.push_back(p);
        }
        const CaseResult c = runCase(r, ph, locked, "locked to the world", .3125f);
        const Seen& modeZero = c.seen[1];   // kModes[1]
        const Seen& modeOne = c.seen[2];    // kModes[2]
        const bool lockedSized = modeOne.motion.size() == size_t(W) * H * 2 && modeZero.motion.size() == size_t(W) * H * 2;
        // The distance of each locked surface's output from the world's vector (mode 1) and from the world's vector plus p - c, which is what the map
        // alone says (mode 0). Anything rejected counts as infinitely far.
        double farFromWorld = 0, farFromMapAlone = 0;
        for (size_t t = 0; lockedSized && t < c.ex.size(); ++t) {
            if (c.ex[t].kind != Kind::Valid) continue;
            farFromWorld = modeOne.mask[t] != 0 ? HUGE_VAL
                : std::fmax(farFromWorld, std::fmax(std::fabs(double(modeOne.motion[2 * t]) - wx), std::fabs(double(modeOne.motion[2 * t + 1]) - wy)));
            farFromMapAlone = modeZero.mask[t] != 0 ? HUGE_VAL
                : std::fmax(farFromMapAlone, std::fmax(std::fabs(double(modeZero.motion[2 * t]) - (wx + double(ph.px - ph.cx))),
                                                       std::fabs(double(modeZero.motion[2 * t + 1]) - (wy + double(ph.py - ph.cy)))));
        }
        check(lockedSized && farFromWorld <= 1e-4, "first person phase (locked to the world): in mode 1 a first-person surface that moves as the world does gets the vector the world's camera term gives its neighbours");
        // And the vector the map alone gives: off by p - c, which is what the correction exists to remove.
        check(lockedSized && farFromMapAlone <= 1e-3, "first person phase (locked to the world): in mode 0 the same surface is off by p - c, the phase the correction removes");
        if (!mutationFailures)
            std::printf("flat mono resolve: first-person phase: locked to the world: under c=(%g,%g) p=(%g,%g) and a camera that moves one render pixel, the world's camera term gives (%.4f, %.4f) px; "
                        "six first-person surfaces moving exactly as the world does get it to within %.3g px in mode 1, and in mode 0 are off by p - c = (%g, %g) (to within %.3g px)\n",
                        double(ph.cx), double(ph.cy), double(ph.px), double(ph.py), wx, wy, farFromWorld, double(ph.px - ph.cx), double(ph.py - ph.cy), farFromMapAlone);
    }
    if (!mutationFailures) {
        std::printf("flat mono resolve: first-person phase: %zu cases (%zu phase pairs x %zu points, frame edges both ways, a surface locked to the world), %zu valid-point judgements; "
                    "worst error against the true motion in mode 1: %.3g px over the dyadic phases, %.3g px for the non-dyadic pair (fp16 rounding of its map and output); "
                    "against the map as stored in mode 0: %.3g px\n",
                    r.audit.cases, sizeof(kPairs) / sizeof(kPairs[0]), sizeof(kPoints) / sizeof(kPoints[0]), r.audit.valid, r.audit.worst1, r.audit.worst1Fp16, r.audit.worst0);
    }
    edvr::flatMonoResolveReset();
}

// ---- the contract around the prep (shipped prep only) --------------------------------------------------------------------------------

// Every output byte of the prep for the three runs a pair gets in the contract: the field unset, mode 0 and mode 1.
struct Hashes { std::vector<uint64_t> unset, zero, one; };
inline Hashes collectHashes(fpgpu::Fixture& fx) {
    Hashes h;
    edvr::flatMonoResolveReset();
    fx.fillColor(fpgpu::grey128);
    uint64_t frame = 62000;
    std::vector<Expect> ex;
    buildFrame(fx, kPairs[1], general(), ex);
    resolve(fx, frameFor(fx, kPairs[1], true, kModes[0], frame++, true));
    for (const Phase& ph : kPairs) {
        buildFrame(fx, ph, general(), ex);
        h.unset.push_back(resolve(fx, frameFor(fx, ph, true, kModes[0], frame++, false)).hash);
        h.zero.push_back(resolve(fx, frameFor(fx, ph, true, kModes[1], frame++, false)).hash);
        h.one.push_back(resolve(fx, frameFor(fx, ph, true, kModes[2], frame++, false)).hash);
    }
    edvr::flatMonoResolveReset();
    return h;
}

// Install `hlsl` as the prep for the runs until the next installation; false when it does not compile or does not make a compute shader.
inline bool installPrep(fpgpu::Fixture& fx, const std::string& hlsl) {
    std::vector<unsigned char> bytes;
    if (!fpgpu::compilePrep(hlsl, bytes)) return false;
    ComPtr<ID3D11ComputeShader> probe;
    if (FAILED(fx.device->CreateComputeShader(bytes.data(), bytes.size(), nullptr, probe.GetAddressOf()))) return false;
    edvr::flatMonoResolveTestPrepBytecode(bytes.data(), bytes.size());
    edvr::flatMonoResolveReset();
    return true;
}
inline void restoreShippedPrep() { edvr::flatMonoResolveTestPrepBytecode(nullptr, 0); edvr::flatMonoResolveReset(); }

// The line the mode adds to the prep; deleting it gives the prep as it was before the mode existed.
constexpr char kModeLine[] = "if(route.w==1)m.xy+=jitter.xy-jitter.zw; else if(route.w!=0)valid=false;";

inline void contractTests(fpgpu::Fixture& fx) {
    const std::string shipped = edvr::kFlatMonoShaderSource;
    char what[320];

    // A. Mode 0 is bit for bit the prep as it was before the mode existed: the same source without the mode line, compiled here, on the same frames.
    {
        bool once = false;
        const std::string legacy = fpgpu::replaceOnce(shipped, kModeLine, "", &once);
        check(once, "first person phase contract: the mode line is in the shader source exactly once (a moved anchor tests nothing)");
        const bool installed = once && installPrep(fx, legacy);
        check(installed, "first person phase contract: the prep without the mode line compiles and makes a compute shader");
        if (installed) {
            const Hashes before = collectHashes(fx);
            restoreShippedPrep();
            const Hashes now = collectHashes(fx);
            check(before.unset.size() == sizeof(kPairs) / sizeof(kPairs[0]) && before.unset == now.unset && before.zero == now.zero,
                  "first person phase contract: with the mode 0 or unset, the prep's whole output is bit for bit what the prep gave before the mode existed, in every phase pair (a negative-zero vector included)");
            check(before.one == before.zero, "first person phase contract: the prep before the mode existed ignores a mode 1 (control: the comparison can tell modes apart)");
            // Mode 1 must change the output wherever c != p (so the comparison above can tell the modes apart).
            size_t differ = 0, phased = 0;
            for (size_t i = 0; i < now.one.size() && i < sizeof(kPairs) / sizeof(kPairs[0]); ++i)
                if (kPairs[i].cx != kPairs[i].px || kPairs[i].cy != kPairs[i].py) { ++phased; differ += now.one[i] != now.zero[i]; }
            std::snprintf(what, sizeof(what), "first person phase contract: mode 1 changes the prep's output in every phase pair with c != p (%zu of %zu differ from mode 0)", differ, phased);
            check(phased > 0 && differ == phased, what);
            if (!mutationFailures)
                std::printf("flat mono resolve: first-person phase: mode 0 and the unset field are bit for bit the prep as it was before the mode existed, in all %zu phase pairs\n", now.unset.size());
        }
        restoreShippedPrep();
    }

    // B. The counters, on the shipped prep: by mode, only when the pair is bound; and what each kind of frame does to the output.
    {
        edvr::flatMonoResolveReset();
        fx.fillColor(fpgpu::grey128);
        uint64_t frame = 63000;
        std::vector<Expect> ex;
        const Phase& ph = kPairs[1];
        buildFrame(fx, ph, general(), ex);
        resolve(fx, frameFor(fx, ph, true, kModes[0], frame++, true));
        auto stats = [] { return edvr::flatMonoResolveStats(); };
        const auto sum = [](const edvr::FlatMonoResolveStats& s) { return s.firstPersonPhaseFrames[0] + s.firstPersonPhaseFrames[1] + s.firstPersonPhaseFrames[2]; };
        const auto start = stats();
        const ModeRun huge = {"mode 0xFFFFFFFF", true, 0xFFFFFFFFu};
        const ModeRun runs[] = {kModes[0], kModes[1], kModes[2], kModes[3], kModes[4], huge};
        for (const ModeRun& m : runs) {
            const auto was = stats();
            resolve(fx, frameFor(fx, ph, true, m, frame++, false));
            const auto now = stats();
            const size_t slot = m.mode == 0 ? 0 : m.mode == 1 ? 1 : 2;
            bool only = now.firstPersonFrames == was.firstPersonFrames + 1 && sum(now) == sum(was) + 1;
            for (size_t i = 0; i < 3; ++i) only = only && now.firstPersonPhaseFrames[i] == was.firstPersonPhaseFrames[i] + (i == slot ? 1u : 0u);
            std::snprintf(what, sizeof(what), "first person phase contract: a bound frame in %s counts once, in firstPersonPhaseFrames[%zu] and in no other", m.name, slot);
            check(only, what);
        }
        const auto mid = stats();
        check(mid.firstPersonPhaseFrames[0] - start.firstPersonPhaseFrames[0] == 2 && mid.firstPersonPhaseFrames[1] - start.firstPersonPhaseFrames[1] == 1 &&
                  mid.firstPersonPhaseFrames[2] - start.firstPersonPhaseFrames[2] == 3,
              "first person phase contract: the unset field and mode 0 count as mode 0, mode 1 as mode 1, and 2, 7 and the largest value as \"other\"");
        // Frames without a bound pair count nowhere in the phase counters, whatever mode they carry, and their output is the bare run's.
        const Seen bare = resolve(fx, frameFor(fx, ph, false, kModes[0], frame++, false));
        auto nowhere = [&](const edvr::FlatMonoResolveFrame& f, const char* kind) {
            const auto was = stats();
            const Seen s = resolve(fx, f);
            const auto now = stats();
            bool same = now.firstPersonPhaseFrames[0] == was.firstPersonPhaseFrames[0] && now.firstPersonPhaseFrames[1] == was.firstPersonPhaseFrames[1] &&
                        now.firstPersonPhaseFrames[2] == was.firstPersonPhaseFrames[2] && now.firstPersonFrames == was.firstPersonFrames;
            std::snprintf(what, sizeof(what), "first person phase contract: %s with the mode set to 1 is counted in none of the phase counters", kind);
            check(same, what);
            std::snprintf(what, sizeof(what), "first person phase contract: %s with the mode set to 1 leaves the prep's output bit for bit the bare run's", kind);
            check(s.ok && s.hash != 0 && s.hash == bare.hash, what);
        };
        {
            edvr::FlatMonoResolveFrame f = frameFor(fx, ph, false, kModes[2], frame++, false);
            nowhere(f, "a frame with no inputs");
            f = frameFor(fx, ph, false, kModes[2], frame++, false); f.firstPersonMotion = fx.mapView.Get();
            nowhere(f, "a map without its stencil");
            f = frameFor(fx, ph, false, kModes[2], frame++, false); f.firstPersonStencil = fx.stencilView.Get();
            nowhere(f, "a stencil without its map");
            auto mapSmall = texture(fx.device, 8, 8, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
            auto mapSmallView = view(fx.device, mapSmall.Get());
            f = frameFor(fx, ph, true, kModes[2], frame++, false); f.firstPersonMotion = mapSmallView.Get();
            nowhere(f, "a pair the resolver refuses (a map of the wrong size)");
        }
        const auto end = stats();
        check(sum(end) - sum(start) == end.firstPersonFrames - start.firstPersonFrames,
              "first person phase contract: over the whole run the phase counters sum to the frames that bound the pair");
    }

    // C. The constants, as the shader sees them: a probe prep that writes route.w and route.z out as every texel's motion. route.w is the mode exactly
    // when the pair is bound, verbatim (2 and 7 and the largest value included), and 0 otherwise; route.z says whether the pair is bound.
    {
        bool once = false;
        const std::string probe = fpgpu::replaceOnce(shipped, "if(reject!=0)motion=0;", "motion=float2(route.w,route.z);", &once);
        check(once, "first person phase probe: its anchor is in the shader source exactly once");
        const bool installed = once && installPrep(fx, probe);
        check(installed, "first person phase probe: the probe prep compiles and makes a compute shader");
        if (installed) {
            fx.fillColor(fpgpu::grey128);
            uint64_t frame = 64000;
            std::vector<Expect> ex;
            const Phase& ph = kPairs[1];
            buildFrame(fx, ph, general(), ex);
            resolve(fx, frameFor(fx, ph, true, kModes[0], frame++, true));
            auto mapSmall = texture(fx.device, 8, 8, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
            auto mapSmallView = view(fx.device, mapSmall.Get());
            struct Config { const char* what; bool map, stencil, wrongSize; ModeRun mode; float w, z; };
            const Config configs[] = {
                {"a bound pair, the field unset", true, true, false, kModes[0], 0, 1},
                {"a bound pair in mode 0", true, true, false, kModes[1], 0, 1},
                {"a bound pair in mode 1", true, true, false, kModes[2], 1, 1},
                {"a bound pair in mode 2", true, true, false, kModes[3], 2, 1},
                {"a bound pair in mode 7", true, true, false, kModes[4], 7, 1},
                {"no inputs, mode 1 set", false, false, false, kModes[2], 0, 0},
                {"a map without its stencil, mode 1 set", true, false, false, kModes[2], 0, 0},
                {"a stencil without its map, mode 1 set", false, true, false, kModes[2], 0, 0},
                {"a refused map, mode 1 set", true, true, true, kModes[2], 0, 0},
                {"a bound pair again after all of that, mode 1", true, true, false, kModes[2], 1, 1},
            };
            for (const Config& c : configs) {
                edvr::FlatMonoResolveFrame f = frameFor(fx, ph, false, c.mode, frame++, false);
                if (c.map) f.firstPersonMotion = c.wrongSize ? mapSmallView.Get() : fx.mapView.Get();
                if (c.stencil) f.firstPersonStencil = fx.stencilView.Get();
                const Seen s = resolve(fx, f);
                bool ok = s.motion.size() == size_t(W) * H * 2;
                for (size_t t = 0; ok && t < size_t(W) * H; ++t) ok = s.motion[2 * t] == c.w && s.motion[2 * t + 1] == c.z;
                std::snprintf(what, sizeof(what), "first person phase probe: %s: the prep sees route.w = %g and route.z = %g in every texel", c.what, double(c.w), double(c.z));
                check(ok, what);
            }
        }
        restoreShippedPrep();
    }
}

// ---- the shader source, one rule flipped at a time -----------------------------------------------------------------------------------
inline void mutationChecks(fpgpu::Fixture& fx) {
    static const fpgpu::Mutant mutants[] = {
        {"the correction's sign is flipped", "m.xy+=jitter.xy-jitter.zw;", "m.xy-=jitter.xy-jitter.zw;", nullptr},
        {"the current and previous phases are swapped", "jitter.xy-jitter.zw", "jitter.zw-jitter.xy", nullptr},
        {"only x is corrected", "m.xy+=jitter.xy-jitter.zw;", "m.x+=jitter.x-jitter.z;", nullptr},
        {"only y is corrected", "m.xy+=jitter.xy-jitter.zw;", "m.y+=jitter.y-jitter.w;", nullptr},
        {"the x correction is negated", "m.xy+=jitter.xy-jitter.zw;", "m.xy+=(jitter.xy-jitter.zw)*float2(-1,1);", nullptr},
        {"the y correction is negated (y up)", "m.xy+=jitter.xy-jitter.zw;", "m.xy+=(jitter.xy-jitter.zw)*float2(1,-1);", nullptr},
        {"the correction uses the camera rows' phase, not the raster's", "jitter.xy-jitter.zw", "rowsJitter.xy-rowsJitter.zw", nullptr},
        {"mode 0 is corrected too", "if(route.w==1)m.xy+=", "if(route.w<=1)m.xy+=", nullptr},
        {"every odd value counts as mode 1", "if(route.w==1)m.xy+=", "if((route.w&1)!=0)m.xy+=", nullptr},
        {"every non-zero mode is corrected and none rejects", "if(route.w==1)m.xy+=", "if(route.w!=0)m.xy+=", nullptr},
        {"the mode is read from route.y", "if(route.w==1)m.xy+=", "if(route.y==1)m.xy+=", nullptr},
        {"mode 2 and above do not reject", " else if(route.w!=0)valid=false;", "", nullptr},
        {"only mode 2 rejects; other values pass as given", "else if(route.w!=0)valid=false;", "else if(route.w==2)valid=false;", nullptr},
        {"mode 2 leaves the attached pixel to the camera term", "bool attached=route.z!=0 && (FirstPersonStencil", "bool attached=route.z!=0 && route.w<2 && (FirstPersonStencil", nullptr},
        {"the validity test runs on the corrected vector", "float2 prevPx=float2(q)+.5+m.xy;",
         "float2 prevPx=float2(q)+.5+m.xy+(route.w==1?jitter.xy-jitter.zw:0);", nullptr},
        {"the correction is also added to world pixels", "motion=(prev-rawUv)*float2(size.xy);",
         "motion=(prev-rawUv)*float2(size.xy)+(route.w==1?jitter.xy-jitter.zw:0);", nullptr},
        {"the correction is added to every pixel at the end (attached ones twice)", "if(reject!=0)motion=0;",
         "if(route.w==1)motion+=jitter.xy-jitter.zw; if(reject!=0)motion=0;", nullptr},
        {"a mode that is not 0 or 1 rejects every pixel, attached or not", "if(reject!=0)motion=0;",
         "if(route.w>1){reject=1;expected=0;} if(reject!=0)motion=0;", nullptr},
    };
    const std::string shipped = edvr::kFlatMonoShaderSource;
    int caught = 0, equivalent = 0;
    auto runMutated = [&](const std::string& hlsl, int* failed, std::string* first) {
        if (!installPrep(fx, hlsl)) return false;
        *failed = 0; mutationFailures = failed; mutationFirst.clear();
        phaseScenario(fx);
        mutationFailures = nullptr; if (first) *first = mutationFirst;
        return true;
    };
    // The control: the same source, compiled here and run through the same seam, is the shipped prep and passes. Without it a failing mutant could
    // be the harness and not the rule.
    {
        int failed = 0; std::string first;
        const bool ran = runMutated(shipped, &failed, &first);
        check(ran && failed == 0, "first person phase mutations: control: the unmutated prep, compiled here and run through the test seam, passes the whole scenario");
    }
    for (const fpgpu::Mutant& m : mutants) {
        bool once = false;
        const std::string hlsl = fpgpu::replaceOnce(shipped, m.from, m.to, &once);
        char what[320];
        std::snprintf(what, sizeof(what), "first person phase mutations: \"%s\": its anchor is in the shader source exactly once (a moved anchor tests nothing)", m.name);
        check(once, what);
        int failed = 0; std::string first;
        const bool ran = once && runMutated(hlsl, &failed, &first);
        std::snprintf(what, sizeof(what), "first person phase mutations: \"%s\" compiles and makes a compute shader", m.name);
        check(ran, what);
        if (!ran) continue;
        if (m.survivesBecause) {
            ++equivalent;
            std::printf("flat mono resolve: first-person phase mutation \"%s\": %s; %d checks fail (equivalent by construction: %s)\n",
                        m.name, failed ? "caught" : "survives", failed, m.survivesBecause);
            continue;
        }
        std::snprintf(what, sizeof(what), "first person phase mutations: \"%s\" is caught by the scenario", m.name);
        check(failed > 0, what);
        if (failed > 0) ++caught;
        std::printf("flat mono resolve: first-person phase mutation \"%s\": %d checks fail; first: %s\n", m.name, failed, first.c_str());
    }
    restoreShippedPrep();
    std::printf("flat mono resolve: first-person phase mutations: %d of %zu caught by the scenario, %d equivalent by construction\n",
                caught, sizeof(mutants) / sizeof(mutants[0]) - size_t(equivalent), equivalent);
    check(size_t(caught) + size_t(equivalent) == sizeof(mutants) / sizeof(mutants[0]),
          "first person phase mutations: every mutation was either caught or named equivalent");
}

}  // namespace fpphase

inline void firstPersonPhaseGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    fpgpu::Fixture fx(device, context);
    const size_t lines = firstPersonLines.size();
    // 1. The scenario on the shipped prep, for real.
    fpphase::phaseScenario(fx);
    // 2. The contract around the prep: mode 0 is the old prep, the counters, the constants.
    fpphase::contractTests(fx);
    // 3. The same scenario against the prep with one rule flipped at a time: it must fail each time.
    fpphase::mutationChecks(fx);
    check(firstPersonLines.size() == lines, "first person phase: the mode adds no log line");
}
