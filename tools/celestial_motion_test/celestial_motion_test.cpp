// celestial_motion_test: the rig for the planet patch motion (docs/terrain-motion-dispatch-cost-2026-09-17.md,
// 2026-10-06 "the design"; src/common/celestial_math.h, src/d3d11/celestial_motion.cpp, the `celestial` path of
// src/d3d11/temporal_shader_source.h).
//
//   (a) clustering, matching and the delta, on the REAL constants of eye dump 180540 (19 frames, both eyes, 6 bodies
//       a frame; tools/celestial_fixture.py): the moon's delta reproduces the doc's D references within 2 m, and the
//       body centre of the previous frame (cb2[12], untouched by any of this arithmetic) within float tolerance;
//   (b) the shader's arithmetic, on the record the build made, reproduces the game's own jitter-free motion of two
//       points of each body (a patch's centre and the body's centre) within 0.01 px -- the game's chain is cb1's
//       columns, so no EDVR convention is shared with the thing it is compared to;
//   (c) the motion shader on WARP: a pixel inside a body's volume takes decision path 12 with the body's motion, a
//       pixel outside takes path 2, and with no records bound the outputs are byte-identical to the shader compiled
//       without the celestial code;
//   (d) the CPU tee: Map/Unmap, UpdateSubresource and the copies that invalidate, the capture at the draw, the consumer's
//       records and the census it feeds;
//   (e) the build's edge cases, each refusal reached by its own reason;
//   (f) the radial shell of a body whose patch boxes reach the eye plane (shell_tests.h): a station 12,213 km from a 4,478 km planet, a ship
//       landed 2 m above it, the float32 the shader subtracts in, the real bodies' radii inside their shells, and the legacy whole-eye
//       record plus five shader mutants that the same judge must refuse;
//   (g) the supercruise gate and the bodies named in the log (gate_tests.h): every (known, supercruise) state, a closed gate's silence, nothing
//       carried across a gap, line budgets, the journal wiring pinned to its text, and mutants of the table and the pin.
// The module under test (src/d3d11/celestial_motion.cpp) is included whole, its dependencies stubbed below, so the tees and the
// capture run as vscreen.cpp calls them.
// Run from the repository root (the fixture is read at a repo-relative path). Exit 0 all passed, 1 a check failed.

#define EDVR_BINDING_SHADOW_EXTERNAL 1   // this rig supplies its own binding-shadow readers (below)
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../../src/d3d11/celestial_motion.cpp"          // the module, whole
#include "../../src/d3d11/temporal_shader_source.h"      // the production shader text

namespace cel = edvr::celestial;

// ---- the module's dependencies, stubbed --------------------------------------------------------------------------
namespace edvr {
std::vector<std::string> g_logLines;                      // every Log::note line, for the census checks
ID3D11DepthStencilView* g_stubDsv = nullptr;              // what bindingGet(Dsv0) answers
ID3D11DepthStencilView* g_stubEyeDsv[2] = {};             // the two eyes' scene depth views, by identity only
Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char* f, ...) {
    char b[2048];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof b, f, a);
    va_end(a);
    g_logLines.push_back(b);
}
void* bindingGet(BindSlot slot) { return slot == BindSlot::Dsv0 ? static_cast<void*>(g_stubDsv) : nullptr; }
bool depthProbeCurrentSceneEyeOf(ID3D11DepthStencilView* dsv, int* outEye, int* outTargetIndex) {
    if (outEye) *outEye = -1;
    if (outTargetIndex) *outTargetIndex = -1;
    for (int i = 0; i < 2; ++i)
        if (dsv && dsv == g_stubEyeDsv[i]) { if (outEye) *outEye = i; if (outTargetIndex) *outTargetIndex = i; return true; }
    return false;
}
int guardFilter(unsigned long, const char*) { return EXCEPTION_EXECUTE_HANDLER; }
void FaultBudget::charge() { m_remaining.fetch_sub(1, std::memory_order_relaxed); }
}  // namespace edvr

namespace {
using edvr::g_logLines;
using edvr::g_stubDsv;
using edvr::g_stubEyeDsv;

unsigned g_checks = 0;
[[noreturn]] void die(const std::string& why) {
    std::fflush(stdout);
    std::fprintf(stderr, "FAIL: %s\n", why.c_str());
    std::fflush(stderr);
    std::exit(1);
}
void check(bool ok, const std::string& why) {
    ++g_checks;
    if (!ok) die(why);
}
std::string fmt(const char* f, ...) {
    char b[512];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof b, f, a);
    va_end(a);
    return b;
}

// ---------------------------------------------------------------------------------------------------------------
// The fixture (tools/celestial_fixture.py writes it; the layout is documented there)
// ---------------------------------------------------------------------------------------------------------------
#pragma pack(push, 1)
struct FixHead { char magic[8]; uint32_t version, eyeFrames, refs; };
struct FixFrame { uint32_t frame, eye, patches, hasMotion; float A[9], cb1[16], tanNow[4], tanPrev[4], jit[2]; };
struct FixPatch { float c[3], q[4], rows[16], o[3], q2[4], body[4]; };
struct FixRef {
    uint32_t frame, eye;
    float radius;
    uint32_t flags;
    double dT[3], p1[3], p1m[2], p2[3], p2m[2];
    float tanNow[4], tanPrev[4], jit[2], jitPrev[2];
};
#pragma pack(pop)
static_assert(sizeof(FixHead) == 20 && sizeof(FixFrame) == 156 && sizeof(FixPatch) == 136 && sizeof(FixRef) == 168,
              "the fixture's records");

struct EyeFrame {
    FixFrame head{};
    std::vector<cel::Patch> patches;
};
struct Fixture {
    std::vector<EyeFrame> frames;
    std::vector<FixRef> refs;
    const EyeFrame* find(uint32_t frame, uint32_t eye) const {
        for (const EyeFrame& f : frames)
            if (f.head.frame == frame && f.head.eye == eye) return &f;
        return nullptr;
    }
};

Fixture loadFixture(const char* path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) die(fmt("cannot open the fixture %s (run from the repository root)", path));
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> data(static_cast<size_t>(size));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) die("short read of the fixture");
    std::fclose(f);
    size_t off = 0;
    auto take = [&](void* dst, size_t n) {
        if (off + n > data.size()) die("the fixture ends early");
        std::memcpy(dst, data.data() + off, n);
        off += n;
    };
    FixHead h{};
    take(&h, sizeof h);
    if (std::memcmp(h.magic, "EDVRCEL1", 8) != 0 || h.version != 1) die("not a celestial fixture of this version");
    Fixture fx;
    for (uint32_t i = 0; i < h.eyeFrames; ++i) {
        EyeFrame ef;
        take(&ef.head, sizeof ef.head);
        for (uint32_t k = 0; k < ef.head.patches; ++k) {
            FixPatch fp{};
            take(&fp, sizeof fp);
            cel::Patch p{};
            std::memcpy(p.c, fp.c, 12);
            std::memcpy(p.q, fp.q, 16);
            std::memcpy(p.rows, fp.rows, 64);
            std::memcpy(p.o, fp.o, 12);
            std::memcpy(p.q2, fp.q2, 16);
            std::memcpy(p.body, fp.body, 16);
            std::memcpy(p.A, ef.head.A, 36);
            p.hash = cel::hashRows(p.rows);
            ef.patches.push_back(p);
        }
        fx.frames.push_back(std::move(ef));
    }
    for (uint32_t i = 0; i < h.refs; ++i) {
        FixRef r{};
        take(&r, sizeof r);
        fx.refs.push_back(r);
    }
    if (off != data.size()) die("trailing bytes in the fixture");
    return fx;
}

constexpr float kMoon = 679444.875f;
constexpr float kFar = 835951.4375f;
constexpr int kW = 2016, kH = 1949;   // the dump's render size

cel::Scratch& scratch() {
    static cel::Scratch s;
    return s;
}

const cel::BodyResult* bodyOf(const cel::BuildResult& r, float radius) {
    for (uint32_t b = 0; b < r.bodies; ++b)
        if (r.body[b].radius == radius) return &r.body[b];
    return nullptr;
}
const float* recordOf(const cel::BuildResult& r, const cel::BodyResult* b) {
    for (uint32_t k = 0; k < r.records; ++k)
        if (&r.body[r.order[k]] == b) return r.gpu[k];
    return nullptr;
}

// The eye's tangents for a frame: the frame's own where the dump has them, else the last frame's (constant in this dump).
cel::EyeInput eyeOf(const Fixture& fx, uint32_t frame, uint32_t eye) {
    const EyeFrame* ef = fx.find(frame, eye);
    if (!ef || !ef->head.hasMotion) {
        for (uint32_t f = frame; f >= 23650; --f)
            if ((ef = fx.find(f, eye)) && ef->head.hasMotion) break;
    }
    if (!ef) die("no tangents for a frame");
    cel::EyeInput in{};
    std::memcpy(in.tan, ef->head.tanNow, 16);
    in.w = kW;
    in.h = kH;
    return in;
}

// ---------------------------------------------------------------------------------------------------------------
// (a) and (b): the real constants
// ---------------------------------------------------------------------------------------------------------------
// The doc's D references (eye 0, the moon), drawstate frame -> world-aligned translation, metres.
struct DocD { uint32_t frame; double t[3]; };
const DocD kDoc[] = {{23651, {-226.7, -365.4, -2788.1}}, {23652, {-204.4, -336.1, -2555.8}}, {23653, {-2017.6, -3255.4, -24794.2}},
                     {23654, {-2225.2, -3585.9, -27316.3}}, {23655, {-858.2, -1383.9, -10535.0}}, {23656, {-596.1, -961.7, -7323.2}},
                     {23657, {-530.6, -857.0, -6514.7}}};

// The same patches seen by a head turned half way round about Y: a head-axes vector (x y z) becomes (-x y -z). The
// patch's own world-aligned placement is unchanged -- c' = Qc, q' = Qq (x) q, A' = A Q -- so D is unchanged and the
// shader's [R|t] is conjugated by Q. The dump's five far bodies are all behind the real camera; this puts them in
// front, where their volumes and their motion can be read at 4e8 to 8e8 m.
cel::Patch yawed(const cel::Patch& p) {
    cel::Patch o = p;
    o.c[0] = -p.c[0]; o.c[2] = -p.c[2];
    o.q[0] = p.q[2]; o.q[1] = p.q[3]; o.q[2] = -p.q[0]; o.q[3] = -p.q[1];
    for (int i = 0; i < 3; ++i) { o.A[i * 3 + 0] = -p.A[i * 3 + 0]; o.A[i * 3 + 2] = -p.A[i * 3 + 2]; }
    return o;
}
std::vector<cel::Patch> yawed(const std::vector<cel::Patch>& v) {
    std::vector<cel::Patch> out;
    for (const cel::Patch& p : v) out.push_back(yawed(p));
    return out;
}

void project(const float tan[4], const double headAxes[3], double* px, double* py, double* z, const std::string& what) {
    check(cel::projectHead(tan, kW, kH, headAxes, px, py, z), fmt("%s (%.4g, %.4g, %.4g) is in front of the eye", what.c_str(), headAxes[0], headAxes[1], headAxes[2]));
}

void testRealConstants(const Fixture& fx) {
    unsigned moonOk = 0, docChecked = 0, centreChecked = 0, pointChecks = 0, farChecked = 0, skipsBehind = 0;
    double worstDoc = 0.0, worstRef = 0.0, worstCentre = 0.0, worstPx = 0.0, worstFarPx = 0.0, worstFarCentre = 0.0;
    // the yaw is a rotation: R(Qq (x) q) = Q R(q)
    {
        const cel::Patch& p = fx.frames[0].patches[0];
        const cel::Patch y = yawed(p);
        double R[9], Ry[9];
        cel::quatToMat(p.q, R);
        cel::quatToMat(y.q, Ry);
        const double Q[9] = {-1, 0, 0, 0, 1, 0, 0, 0, -1};
        double QR[9];
        cel::mulM(Q, R, QR);
        double worst = 0.0;
        for (int i = 0; i < 9; ++i) worst = std::max(worst, std::fabs(QR[i] - Ry[i]));
        check(worst < 1e-6, fmt("the yawed patch's rotation is Q R(q), off by %.2e", worst));
    }
    for (uint32_t eye = 0; eye < 2; ++eye) {
        for (uint32_t frame = 23651; frame <= 23668; ++frame) {
            const EyeFrame* cur = fx.find(frame, eye);
            const EyeFrame* prev = fx.find(frame - 1, eye);
            check(cur && prev, fmt("frame %u eye %u is in the fixture", frame, eye));
            const cel::EyeInput in = eyeOf(fx, frame, eye);
            const cel::EyeInput inPrev = eyeOf(fx, frame - 1, eye);   // last frame's tangents: this frame's tanPrev
            // ---- the real orientation: the moon is in front, the five far bodies behind --------------------------
            cel::BuildResult out;
            cel::build(cur->patches.data(), static_cast<uint32_t>(cur->patches.size()), prev->patches.data(),
                       static_cast<uint32_t>(prev->patches.size()), in, out, scratch());
            check(out.bodies == 6, fmt("frame %u eye %u: six bodies, got %u", frame, eye, out.bodies));
            const cel::BodyResult* moon = bodyOf(out, kMoon);
            check(moon != nullptr, "the moon is in the frame");
            check(moon->ok, fmt("frame %u eye %u: the moon got a delta (fallback %s, skip %u)", frame, eye, cel::fallbackName(moon->fallback), moon->skip));
            ++moonOk;
            check(out.behind == 5 && out.records == 1, fmt("frame %u eye %u: five bodies behind the eye, one record; got behind %u records %u", frame, eye, out.behind, out.records));
            skipsBehind += out.behind;
            check(moon->matched >= 5 && moon->agreeing >= 5, fmt("frame %u eye %u: the moon's faces match and agree (%u/%u)", frame, eye, moon->agreeing, moon->matched));
            // the body centre of the previous frame, from cb2[12] (nothing in the delta's arithmetic reads it)
            {
                const cel::Patch* pc = nullptr;
                const cel::Patch* pp = nullptr;
                for (const cel::Patch& p : cur->patches) if (p.body[3] == kMoon) { pc = &p; break; }
                for (const cel::Patch& p : prev->patches) if (p.body[3] == kMoon) { pp = &p; break; }
                double Bc[3] = {pc->body[0], pc->body[1], pc->body[2]}, Bp[3] = {pp->body[0], pp->body[1], pp->body[2]}, mapped[3];
                cel::mulV(moon->R, Bc, mapped);
                for (int i = 0; i < 3; ++i) mapped[i] += moon->t[i];
                const double e = cel::dist3(mapped, Bp);
                worstCentre = std::max(worstCentre, e);
                const double limit = cel::agreementTolerance(cel::len3(Bc), cel::len3(Bp));   // 12 ulp + 1 m: 6.6 m at 3.9e6 m; the faces spread to 3 m
                check(e < limit, fmt("frame %u eye %u: D carries the moon's centre to last frame's within %.1f m, off by %.2f m", frame, eye, limit, e));
                ++centreChecked;
            }
            // the doc's D references (eye 0 only: body_delta_check took the first target)
            if (eye == 0) {
                for (const DocD& d : kDoc) {
                    if (d.frame != frame) continue;
                    const double e = cel::dist3(moon->t, d.t);
                    worstDoc = std::max(worstDoc, e);
                    check(e < 2.0, fmt("frame %u: the moon's D translation (%.1f %.1f %.1f) is within 2 m of the doc's (%.1f %.1f %.1f), off by %.2f m",
                                       frame, moon->t[0], moon->t[1], moon->t[2], d.t[0], d.t[1], d.t[2], e));
                    ++docChecked;
                }
            }
            // the extractor's per-frame references for the moon: D, and the game's chain at two points (b)
            for (const FixRef& r : fx.refs) {
                if (r.frame != frame || r.eye != eye || r.radius != kMoon) continue;
                const double e = cel::dist3(moon->t, r.dT);
                worstRef = std::max(worstRef, e);
                check(e < 2.0, fmt("frame %u eye %u: the moon's D translation is off the extractor's by %.2f m", frame, eye, e));
                const float* rec = recordOf(out, moon);
                check(rec != nullptr, "the moon has a record");
                for (int pt = 0; pt < 2; ++pt) {
                    const double* P = pt == 0 ? r.p1 : r.p2;
                    const double* want = pt == 0 ? r.p1m : r.p2m;
                    double px, py, z, mx, my;
                    project(r.tanNow, P, &px, &py, &z, fmt("frame %u eye %u moon point %d", frame, eye, pt + 1));
                    check(cel::shaderMotion(rec, r.tanNow, r.tanPrev, kW, kH, px, py, z, &mx, &my),
                          fmt("frame %u eye %u point %d (%.1f, %.1f) at %.4g m is inside the moon's volume", frame, eye, pt + 1, px, py, z));
                    const double e2 = std::hypot(mx - want[0], my - want[1]);
                    worstPx = std::max(worstPx, e2);
                    check(e2 < 0.01, fmt("frame %u eye %u point %d: motion (%.4f, %.4f) is %.4f px from the game's (%.4f, %.4f)",
                                         frame, eye, pt + 1, mx, my, e2, want[0], want[1]));
                    ++pointChecks;
                }
            }
            // ---- the head turned about: the far bodies in front, the moon behind ---------------------------------
            const std::vector<cel::Patch> yc = yawed(cur->patches), yp = yawed(prev->patches);
            cel::BuildResult turned;
            cel::build(yc.data(), static_cast<uint32_t>(yc.size()), yp.data(), static_cast<uint32_t>(yp.size()), in, turned, scratch());
            check(turned.bodies == 6 && turned.behind == 1, fmt("frame %u eye %u yawed: the moon alone is behind; behind %u", frame, eye, turned.behind));
            // four are on the eye's pixels; the fifth (R 699218.9) is 1.33 up at 2.6e8 m and the eye's top tangent is 1.03
            check(turned.records == 4 && turned.offscreen == 1, fmt("frame %u eye %u yawed: four far records and one off the eye's pixels, got %u and %u", frame, eye, turned.records, turned.offscreen));
            for (uint32_t k = 1; k < turned.records; ++k)
                check(turned.body[turned.order[k - 1]].nearest <= turned.body[turned.order[k]].nearest, "records run nearest first");
            for (uint32_t k = 0; k < turned.records; ++k) {
                const cel::BodyResult& b = turned.body[turned.order[k]];
                check(b.ok && b.matched >= 5 && b.agreeing >= 5, fmt("frame %u eye %u far body %.0f: delta from %u/%u faces", frame, eye, b.radius, b.agreeing, b.matched));
                // the centre of the previous frame (cb2[12]) through D, and the game's-chain motion of the centre
                const cel::Patch* pc = nullptr;
                const cel::Patch* pp = nullptr;
                for (const cel::Patch& p : cur->patches) if (p.body[3] == b.radius) { pc = &p; break; }
                for (const cel::Patch& p : prev->patches) if (p.body[3] == b.radius) { pp = &p; break; }
                double Bc[3] = {pc->body[0], pc->body[1], pc->body[2]}, Bp[3] = {pp->body[0], pp->body[1], pp->body[2]}, mapped[3];
                cel::mulV(b.R, Bc, mapped);
                for (int i = 0; i < 3; ++i) mapped[i] += b.t[i];
                const double ec = cel::dist3(mapped, Bp);
                worstFarCentre = std::max(worstFarCentre, ec);
                check(ec < 3.0 * cel::agreementTolerance(cel::len3(Bc), cel::len3(Bp)),
                      fmt("frame %u eye %u far body %.0f (%.3g m): D carries the centre to last frame's within float tolerance, off by %.1f m", frame, eye, b.radius, cel::len3(Bc), ec));
                // true motion of the centre for the turned head: A^T takes world-aligned back to head axes, Q turns it
                const double Q[9] = {-1, 0, 0, 0, 1, 0, 0, 0, -1};
                double Ac[9], Ap[9], hc[3], hp[3], tmp[3];
                for (int i = 0; i < 9; ++i) { Ac[i] = cur->head.A[i]; Ap[i] = prev->head.A[i]; }
                cel::mulTV(Ac, Bc, tmp); cel::mulV(Q, tmp, hc);
                cel::mulTV(Ap, Bp, tmp); cel::mulV(Q, tmp, hp);
                double px, py, z, qx, qy, qz;
                project(in.tan, hc, &px, &py, &z, fmt("frame %u eye %u far body %.0f centre, now", frame, eye, b.radius));
                project(inPrev.tan, hp, &qx, &qy, &qz, fmt("frame %u eye %u far body %.0f centre, before", frame, eye, b.radius));
                const float* rec = turned.gpu[k];
                double mx, my;
                check(cel::shaderMotion(rec, in.tan, inPrev.tan, kW, kH, px, py, z, &mx, &my),
                      fmt("frame %u eye %u far body %.0f: its centre (%.1f, %.1f) at %.4g m is inside its volume", frame, eye, b.radius, px, py, z));
                const double e2 = std::hypot(mx - (qx - px), my - (qy - py));
                worstFarPx = std::max(worstFarPx, e2);
                check(e2 < 0.01, fmt("frame %u eye %u far body %.0f: centre motion (%.4f, %.4f) is %.4f px from the geometry's (%.4f, %.4f)",
                                     frame, eye, b.radius, mx, my, e2, qx - px, qy - py));
                ++farChecked;
            }
            for (const FixRef& r : fx.refs) {
                if (r.frame != frame || r.eye != eye || r.radius != kFar) continue;
                const cel::BodyResult* b = bodyOf(turned, kFar);
                const double tol = 3.0 * cel::agreementTolerance(6.5e8, 6.5e8);
                check(cel::dist3(b->t, r.dT) < tol, fmt("frame %u eye %u: the far body's D translation is off the extractor's by %.1f m", frame, eye, cel::dist3(b->t, r.dT)));
            }
        }
    }
    std::printf("(a) %u moon deltas on real constants, five far bodies behind the eye skipped (%u); doc D within 2 m (%u frames, worst %.2f m), "
                "extractor D worst %.2f m, previous-frame centre worst %.2f m (%u frames)\n",
                moonOk, skipsBehind, docChecked, worstDoc, worstRef, worstCentre, centreChecked);
    std::printf("(a) %u far-body deltas with the head turned about (4e8-8e8 m): previous-frame centre worst %.1f m\n", farChecked, worstFarCentre);
    std::printf("(b) %u moon points through the shader's arithmetic against the game's chain: worst %.5f px; %u far centres against the geometry: worst %.5f px (limit 0.01)\n",
                pointChecks, worstPx, farChecked, worstFarPx);
    // What the build costs on the CPU, for the record (not a check): one eye-frame of the real dump -- 36 patches, six bodies, the moon's six
    // faces matched -- and the same with the five bodies behind the eye left out of the count. The census line prints the live figure.
    {
        const EyeFrame* cur = fx.find(23654, 0);
        const EyeFrame* prev = fx.find(23653, 0);
        const cel::EyeInput in = eyeOf(fx, 23654, 0);
        cel::BuildResult out;
        constexpr int kRuns = 4000;
        LARGE_INTEGER f{}, a{}, b{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        for (int i = 0; i < kRuns; ++i)
            cel::build(cur->patches.data(), static_cast<uint32_t>(cur->patches.size()), prev->patches.data(), static_cast<uint32_t>(prev->patches.size()), in, out, scratch());
        QueryPerformanceCounter(&b);
        std::printf("(cost) cel::build of one eye-frame of the real dump (36 patches, six bodies, one in view): %.1f us\n",
                    1e6 * double(b.QuadPart - a.QuadPart) / double(f.QuadPart) / kRuns);
    }
}

#include "edge_tests.h"
#include "tee_tests.h"
#include "gate_tests.h"
#include "warp_tests.h"

}  // namespace

int main(int argc, char** argv) {
    const char* path = "tools/celestial_motion_test/fixture_180540.bin";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) path = argv[++i];
    }
    const Fixture fx = loadFixture(path);
    std::printf("celestial_motion_test: fixture %s: %zu eye-frames, %zu references\n", path, fx.frames.size(), fx.refs.size());
    testRealConstants(fx);
    edge::all(fx);
    tee::all(fx);
    gate::all(fx);
    warp::all(fx);
    std::printf("PASS: celestial_motion_test, %u checks\n", g_checks);
    return 0;
}
