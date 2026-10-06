// The refusal view on the COPY route (FlatMonoResolveFrame::hdr = false) on WARP.
//
// The view (advanced.temporal_aa_debug = motion_source) paints the prep's per-pixel class over what the finish shows. It began as the HDR route's
// (the finish draw into H); the copy route's compute finish paints it now too, for the SDK backends (DLAA, DLSS, FSR) whatever the render and the
// output sizes: the resolver asks the prep for the class texture (paintView is true for hdr or any mode but EDVR's own TAA), binds it at t11 for
// the finish, and the finish paints refusalPaint(c, refusalClassAt(rasterUv)) over its colour c when debug.y says so. EDVR's own TAA writes its output
// without a finish, so it has nothing to paint in and asks for nothing.
//
// One fixture (steady_depth's Rig: the copy route's 16 x 16 render, a still camera, one pool record, an RGBA8 game frame of a uniform 64 grey), the
// render below the output (16 -> 32) for DLSS and FSR and equal to it for DLAA and DLSS. The stub backend hands back a uniform green (0, 1, 0), so
// in the output a pixel whose history was refused shows the raw 64 grey and any other pixel the backend's green, and the view paints over both:
//   a stale slot (a 4 x 4 block), refused           -> PINK       (y, .4y, .7y)  from the raw grey, y = twice its luma: 128 51 90
//   a corrupt slot                                  -> magenta    (y, 0, y)   128 0 128
//   the out-of-range sentinel                       -> white      (y, y, y)   128 128 128
//   a masked record (a frame of its own)            -> red        (y, 0, 0)   128 0 0
//   a pool surface that is not a rig record         -> blue       (0, .3y, y) from the backend's green, y = 1.43: 0 109 255 (RGBA8 saturates)
//   a joined record (a frame of its own)            -> green      (0, y, 0)   0 255 0: not dimmed, which a pixel with no slot is
//   a stale slot the steady-detail rule KEEPS       -> yellow (y, y, 0) from the backend's green: 255 255 0 (accepted, but the view says what it IS;
//                                                      pink is for the refused one: the same class, the class byte's bit 7 is the difference)
//   a pixel with no engine slot                     -> a quarter of what it showed: the green's 0 64 0
// Every output pixel is checked against a model of the finish (the 2 x 2 raster footprint under the output pixel's sample, the refused class among
// them else the nearest texel's), and the named probes against the colours above.
//
// What is pinned: the view on paints (above); the view off is bit for bit the unpainted finish (the backend's green where the history is trusted,
// the raw frame where it is not), and a frame that asks for neither view nor census makes no class texture; a reset frame paints nothing and makes
// no class texture (every pixel is refused there, which says nothing); the view off again after frames that painted is unpainted, so the paint is
// gated by the frame's debug.y and not by the class texture existing; and on the copy route EDVR's own TAA, view on, paints nothing, makes no class
// texture, and gives the same output as with the view off.
//
// What fails if a piece is reverted, by assertion (checked by mutating a private copy of the resolver and the shader; see the report):
//   the shader's paint line out               -> "view on: ... painted" (every probe and the whole-image model), not "view off"
//   the resolver's t11 binding out            -> the same: the finish reads a null class texture, class 0, and dims every pixel to a quarter
//   paintView back to the HDR route's alone   -> the same, and "the view makes the class texture on the copy route"
//   the finish paints whether the view asked  -> "view off ... unpainted" and "view off again"
//   paintView for EDVR's TAA too              -> "TAA: ... makes no class texture"
//   the stale paint's bit-7 branch out (the shader paints every stale slot yellow)
//                                             -> "a REFUSED stale slot is pink" and "every output pixel is the finish's colour painted by its class"
//   the branch inverted                       -> those two and "a stale slot the steady-detail rule KEEPS is painted yellow"
//   every stale slot pink                     -> "a stale slot the steady-detail rule KEEPS is painted yellow" alone: the pair pins the bit, not a colour
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace copyview {
constexpr UINT R = steadygpu::W;   // the render size, 16

inline const char* msg(const char* fmt, ...) {   // one buffer: every message is consumed by check() before the next is made
    static char buffer[512];
    va_list args; va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}
inline unsigned byteOf(double v) { return unsigned(std::floor((v < 0 ? 0 : v > 1 ? 1 : v) * 255 + .5)); }

// refusalPaint, as the shader has it (flat_mono_shader_source.h), in doubles.
inline void paintModel(double (&c)[3], unsigned v) {
    using namespace edvr;
    const unsigned kind = v & 0x7Fu;
    const double y = std::max(.2126 * c[0] + .7152 * c[1] + .0722 * c[2], .02) * 2;
    double p[3];
    if (kind == kFlatMonoClassJoined) { p[0] = 0; p[1] = y; p[2] = 0; }
    else if (kind == kFlatMonoClassMasked) { p[0] = y; p[1] = 0; p[2] = 0; }
    else if (kind == kFlatMonoClassNotRig) { p[0] = 0; p[1] = .3 * y; p[2] = y; }
    else if (kind == kFlatMonoClassStale) {   // bit 7: refused (pink, the pixel shows the raw frame); clear: kept by the steady-detail rule (yellow)
        if (v & 0x80u) { p[0] = y; p[1] = .4 * y; p[2] = .7 * y; }
        else { p[0] = y; p[1] = y; p[2] = 0; }
    }
    else if (kind == kFlatMonoClassCorrupt) { p[0] = y; p[1] = 0; p[2] = y; }
    else if (kind == kFlatMonoClassStaleStamp) { p[0] = y; p[1] = .5 * y; p[2] = 0; }
    else if (kind == kFlatMonoClassWeapon) { p[0] = 0; p[1] = y; p[2] = y; }
    else if (kind >= kFlatMonoClassSentinel && kind <= kFlatMonoClassWeaponRefused) { p[0] = p[1] = p[2] = y; }
    else { p[0] = c[0] * .25; p[1] = c[1] * .25; p[2] = c[2] * .25; }
    c[0] = p[0]; c[1] = p[1]; c[2] = p[2];
}

// The prep's class per render texel (bit 7: history refused) for one frame of the layout.
struct Classes {
    std::vector<unsigned char> v = std::vector<unsigned char>(R * R, 0);
    void set(UINT x, UINT y, unsigned c) { v[size_t(y) * R + x] = static_cast<unsigned char>(c); }
};

// The finish over the whole D x D output, as the compute kernel has it: per output pixel the 2 x 2 raster texels under its sample (jitter zero), the
// refusal of any of them takes the raw frame, else the backend's result; then, with the view, the paint by the refused class among the four, else the
// class of the texel under the sample. `allRefused` is a reset frame (the prep refuses every pixel, and nothing is painted on it).
inline std::vector<unsigned char> finishModel(const Classes& cls, UINT D, bool paint, double raw, bool allRefused) {
    std::vector<unsigned char> out(size_t(D) * D * 4, 0);
    for (UINT Y = 0; Y < D; ++Y)
        for (UINT X = 0; X < D; ++X) {
            const double u = (X + .5) / D, v = (Y + .5) / D;
            const int qx = int(std::floor(u * R - .5)), qy = int(std::floor(v * R - .5));
            bool reject = allRefused; unsigned best = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int tx = std::min(std::max(qx + dx, 0), int(R) - 1), ty = std::min(std::max(qy + dy, 0), int(R) - 1);
                    const unsigned c = cls.v[size_t(ty) * R + tx];
                    if (c & 0x80u) { reject = true; best = std::max(best, c); }
                }
            const int nx = std::min(std::max(int(std::floor(u * R)), 0), int(R) - 1), ny = std::min(std::max(int(std::floor(v * R)), 0), int(R) - 1);
            const unsigned k = best ? best : cls.v[size_t(ny) * R + nx];
            double c[3] = {reject ? raw : 0.0, reject ? raw : 1.0, reject ? raw : 0.0};
            if (paint) paintModel(c, k);
            unsigned char* o = &out[(size_t(Y) * D + X) * 4];
            o[0] = static_cast<unsigned char>(byteOf(c[0])); o[1] = static_cast<unsigned char>(byteOf(c[1])); o[2] = static_cast<unsigned char>(byteOf(c[2])); o[3] = 255;
        }
    return out;
}
inline unsigned worstDiff(const std::vector<unsigned char>& got, const std::vector<unsigned char>& want) {
    if (got.size() != want.size() || got.empty()) return 999;
    unsigned worst = 0;
    for (size_t i = 0; i < got.size(); ++i) worst = std::max(worst, unsigned(std::abs(int(got[i]) - int(want[i]))));
    return worst;
}
inline bool pixelIs(const std::vector<unsigned char>& out, UINT D, UINT X, UINT Y, int r, int g, int b, int tol = 2) {
    if (out.size() != size_t(D) * D * 4) return false;
    const unsigned char* p = &out[(size_t(Y) * D + X) * 4];
    return std::abs(int(p[0]) - r) <= tol && std::abs(int(p[1]) - g) <= tol && std::abs(int(p[2]) - b) <= tol && p[3] == 255;
}

// The output of one resolved frame, read back (the copy route hands back the output texture's view).
inline bool readOutput(steadygpu::Rig& rig, const ComPtr<ID3D11ShaderResourceView>& view, std::vector<unsigned char>& bytes) {
    bytes.clear();
    if (!view) return false;
    ComPtr<ID3D11Resource> resource; view->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture2d;
    if (!resource || FAILED(resource.As(&texture2d))) return false;
    return readWhole(rig.context, texture2d.Get(), bytes, 4);
}

// The scenario for one SDK mode and one output size D: the unpainted frames, the painted ones, the unpainted again.
inline void sdkScenario(steadygpu::Rig& r, edvr::FlatMonoResolveMode mode, const char* name, UINT D) {
    using namespace edvr;
    const double raw = 64.0 / 255.0;
    const UINT s = D / R;
    auto outX = [&](UINT t) { return s * t + (s - 1); };   // an output pixel whose 2 x 2 footprint starts at render texel t, and whose nearest texel is t
    char what[160];
    flatMonoResolveReset();
    check(!flatMonoResolveTestRefusalResources(), msg("copy-route view (%s): before any frame asks, the view owns no resource", name));
    r.start(mode);
    r.f.outputWidth = r.f.outputHeight = D;
    std::fill(r.rgba.begin(), r.rgba.end(), 0xff404040u);
    auto layoutA = [&] { r.noSlots(); r.fillZ(.01f); r.staleBlock(4, 4, 4, 4); r.put(2, 2, 2, .01f); r.put(13, 3, 4294967296.0f, .02f); r.put(10, 2, 1, .01f); r.setRecord(0); };
    auto layoutB = [&](uint32_t marker) { r.noSlots(); r.fillZ(.01f); r.put(8, 8, 1, .01f); r.setRecord(marker); };
    auto classesA = [&](bool kept) {
        Classes c;
        for (UINT y = 4; y < 8; ++y) for (UINT x = 4; x < 8; ++x) c.set(x, y, kept ? kFlatMonoClassStale : (kFlatMonoClassStale | 0x80u));
        c.set(2, 2, kFlatMonoClassCorrupt | 0x80u); c.set(13, 3, kFlatMonoClassSentinel | 0x80u); c.set(10, 2, kFlatMonoClassNotRig);
        return c;
    };
    auto frame = [&](const char* label, bool view, bool reset, std::vector<unsigned char>& bytes) {
        r.f.refusalView = view ? 1u : 0u; r.f.reset = reset;
        std::snprintf(what, sizeof(what), "copy-route view (%s): %s", name, label);
        ComPtr<ID3D11ShaderResourceView> out;
        const auto seen = r.run(what, true, &out);
        const bool read = seen.ok && readOutput(r, out, bytes);
        check(read && bytes.size() == size_t(D) * D * 4, msg("copy-route view (%s): %s: the output is read back, %u x %u", name, label, D, D));
    };
    std::vector<unsigned char> got;

    // The view off: the unpainted finish, bit for bit, and nothing made. The first frame is the run's reset frame.
    frame("the reset frame that starts the run (view off)", false, true, got);
    check(worstDiff(got, finishModel(Classes{}, D, false, raw, true)) == 0,
          msg("copy-route view (%s), view off: a reset frame is the raw frame everywhere (every pixel refused, nothing painted)", name));
    layoutA();
    frame("a continuing frame (view off)", false, false, got);
    check(worstDiff(got, finishModel(classesA(false), D, false, raw, false)) == 0,
          msg("copy-route view (%s), view off: the finish is bit for bit the unpainted one: the backend's green where history is trusted, the raw grey where it is refused", name));
    check(pixelIs(got, D, outX(5), outX(5), 64, 64, 64, 0) && pixelIs(got, D, outX(0), outX(0), 0, 255, 0, 0),
          msg("copy-route view (%s), view off: a refused stale pixel shows its raw 64 grey and an accepted one the backend's green", name));
    check(!flatMonoResolveTestRefusalResources(),
          msg("copy-route view (%s), view off: a frame that asks for neither the view nor the census makes no class texture, counting pass or counter buffer", name));

    // The view on. A reset frame paints nothing and makes nothing: every pixel is refused there, which says nothing.
    frame("a reset frame (view on)", true, true, got);
    check(worstDiff(got, finishModel(Classes{}, D, false, raw, true)) == 0,
          msg("copy-route view (%s), view on: a reset frame paints nothing", name));
    check(!flatMonoResolveTestRefusalResources(), msg("copy-route view (%s), view on: a reset frame makes no class texture", name));
    frame("a continuing frame (view on)", true, false, got);
    check(flatMonoResolveTestRefusalResources(), msg("copy-route view (%s), view on: the view makes the class texture on the copy route", name));
    check(worstDiff(got, finishModel(classesA(false), D, true, raw, false)) <= 2,
          msg("copy-route view (%s), view on: every output pixel is the finish's colour painted by its class", name));
    check(pixelIs(got, D, outX(5), outX(5), 128, 51, 90), msg("copy-route view (%s), view on: a REFUSED stale slot is pink, painted from the raw grey (128 51 90), not yellow and not the raw grey", name));
    check(pixelIs(got, D, outX(2), outX(2), 128, 0, 128), msg("copy-route view (%s), view on: a corrupt slot is magenta (128 0 128)", name));
    check(pixelIs(got, D, outX(13), outX(3), 128, 128, 128), msg("copy-route view (%s), view on: the out-of-range sentinel is white (128 128 128)", name));
    check(pixelIs(got, D, outX(10), outX(2), 0, 109, 255), msg("copy-route view (%s), view on: a pool surface that is not a rig record is blue, painted from the backend's green (0 109 255)", name));
    check(pixelIs(got, D, outX(0), outX(0), 0, 64, 0), msg("copy-route view (%s), view on: a pixel with no engine slot is dimmed to a quarter of the backend's green (0 64 0)", name));

    // A masked record, then a joined one (a frame of their own: one record serves every code-1 slot).
    layoutB(2);
    frame("a masked record (view on)", true, false, got);
    check(worstDiff(got, finishModel([&] { Classes c; c.set(8, 8, kFlatMonoClassMasked | 0x80u); return c; }(), D, true, raw, false)) <= 2 &&
              pixelIs(got, D, outX(8), outX(8), 128, 0, 0),
          msg("copy-route view (%s), view on: a masked record is red, painted from the raw grey (128 0 0)", name));
    layoutB(1);
    frame("a joined record (view on)", true, false, got);
    check(worstDiff(got, finishModel([&] { Classes c; c.set(8, 8, kFlatMonoClassJoined); return c; }(), D, true, raw, false)) <= 2 &&
              pixelIs(got, D, outX(8), outX(8), 0, 255, 0) && pixelIs(got, D, outX(0), outX(0), 0, 64, 0),
          msg("copy-route view (%s), view on: a joined record is green (0 255 0), not dimmed as a pixel with no slot is (0 64 0)", name));

    // The steady-detail rule keeps the stale block (last frame's depth confirms it): accepted, so the backend's green, and painted YELLOW, not
    // pink: the class is the same stale slot, but its history was not refused (bit 7 clear). The pink block above is the refused one.
    layoutA();
    r.f.steadyDetail = true;
    frame("the steady-detail settle frame (view on)", true, false, got);
    frame("a stale block the steady-detail rule keeps (view on)", true, false, got);
    check(worstDiff(got, finishModel(classesA(true), D, true, raw, false)) <= 2 && pixelIs(got, D, outX(5), outX(5), 255, 255, 0),
          msg("copy-route view (%s), view on: a stale slot the steady-detail rule KEEPS is painted yellow from the backend's green (255 255 0), not pink: the view says what the pixel is", name));
    r.f.steadyDetail = false;

    // The view off again, after frames that painted: unpainted (the class texture exists but the frame does not ask).
    layoutA();
    frame("the settle frame back to the unkept block (view off)", false, false, got);
    frame("the view off again", false, false, got);
    check(worstDiff(got, finishModel(classesA(false), D, false, raw, false)) == 0,
          msg("copy-route view (%s), view off again: after painted frames the finish is bit for bit the unpainted one: the paint follows the frame's request, not the class texture", name));
}

// EDVR's own TAA on the copy route: no finish to paint in, so the view asks for nothing and the output is the same with it on.
inline void taaScenario(steadygpu::Rig& r, UINT D) {
    using namespace edvr;
    std::vector<unsigned char> outputs[2];
    for (const bool view : {false, true}) {
        flatMonoResolveReset();
        r.start(FlatMonoResolveMode::Taa);
        r.f.outputWidth = r.f.outputHeight = D;
        r.staleBlock(4, 4, 4, 4); r.put(2, 2, 2, .01f); r.put(13, 3, 4294967296.0f, .02f); r.put(10, 2, 1, .01f);
        r.f.refusalView = view ? 1u : 0u;
        std::fill(r.rgba.begin(), r.rgba.end(), 0xff808080u);
        r.run(msg("copy-route view (TAA, output %u): the reset frame, view %s", D, view ? "on" : "off"));
        r.f.reset = false;
        for (UINT y = 0; y < R; ++y) for (UINT x = 0; x < R; ++x) r.rgba[size_t(y) * R + x] = ((x + y) & 1) ? 0xffc0c0c0u : 0xff404040u;
        ComPtr<ID3D11ShaderResourceView> out;
        const auto seen = r.run(msg("copy-route view (TAA, output %u): the checker frame, view %s", D, view ? "on" : "off"), true, &out);
        check(seen.ok && readOutput(r, out, outputs[view]) && outputs[view].size() == size_t(D) * D * 4,
              msg("copy-route view (TAA, output %u): the TAA output is read back, view %s", D, view ? "on" : "off"));
        check(!flatMonoResolveTestRefusalResources(),
              msg("copy-route view (TAA, output %u), view %s: EDVR's own TAA has no finish to paint in, so the view asks for no class texture, counting pass or counter buffer", D, view ? "on" : "off"));
    }
    check(!outputs[0].empty() && outputs[0] == outputs[1],
          msg("copy-route view (TAA, output %u): the output with the view on is bit for bit the output with it off: nothing is painted", D));
}
}  // namespace copyview

inline void copyRefusalViewGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using namespace edvr;
    steadygpu::Rig rig(device, context);
    copyview::sdkScenario(rig, FlatMonoResolveMode::Dlaa, "DLAA, native", 16);
    copyview::sdkScenario(rig, FlatMonoResolveMode::Dlss, "DLSS, native", 16);
    copyview::sdkScenario(rig, FlatMonoResolveMode::Dlss, "DLSS, render below the output", 32);
    copyview::sdkScenario(rig, FlatMonoResolveMode::Fsr, "FSR, render below the output", 32);
    copyview::taaScenario(rig, 16);
    copyview::taaScenario(rig, 32);
    flatMonoResolveReset();
}
