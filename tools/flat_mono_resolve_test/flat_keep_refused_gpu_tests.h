// The temporary A/B bit of the flat prep (experimental.flat_sdk_local_reset = off, design doc section 104) on WARP.
//
// FlatMonoResolveFrame::keepRefusedHistory puts bit 4 in the prep's debug.w for DLAA, DLSS and FSR and never for EDVR's own TAA. With the bit set
// the prep still refuses what it refused (the census counts the same classes, with the refused bit), but where a motion was FORMED for a refused
// pixel it writes no rejection for it and keeps that motion, so the backend keeps its own result there and the HDR finish shows it instead of
// the raw frame. A motion is formed for
//   - a stale slot whose previous-depth check failed (the camera term was formed, the check refused it),
//   - a camera term that leaves the frame (finite, within half-float range),
//   - a record that says nothing about its surface, a corrupt code or the sentinel (the sky included): under the key alone it forms the camera term.
// Nothing is formed, so the pixel is refused with no motion exactly as with the key off, for
//   - a masked or an unreprojectable record (a mover's: its camera term would be a static surface's),
//   - a camera term that cannot be formed (behind the previous camera: class Camera) or lies beyond half-float range,
//   - what the world branch never reaches: a reset frame, a pixel whose depth is no depth, a first-person pixel the foreground map cannot place.
//
// One fixture throughout (the HDR route, which is the flat profile's: an R11G11B10F scene target H, 16 x 16, a still rotation, a two-record pool,
// the game's pipeline to hand back). The current camera is moved 0.9375 along x, which is 3 render pixels at depth .01 (6 at .02, none at no
// depth), so no camera term is zero and a refused pixel's zero cannot pass for one. The layout (texels x, y; depth .01 unless said):
//   (1,1)  a plain pixel, no slot: the camera term, accepted
//   (2,10) a stale slot whose previous-depth check FAILS (the check looks at texels 4..6 x 9..11 in last frame's depth, which hold .02)
//   (2,13) a stale slot whose previous-depth check PASSES (what it looks at is its own depth)
//   (3,3)  a corrupt slot (an even code)         (6,2) the out-of-range sentinel      (10,2) a slot under a pixel with no depth (sky)
//   (11,6) a masked record (record 0)            (9,13) an unreprojectable record: record 1 is a moved JOINED one and the engine's own scene rows
//                                                       are not the pool families' encoding, so its reprojection fails
//   (8,4)  a pixel whose depth is 2: no depth, not the world's
//   x >= 13 every row: the camera term leaves the frame (a range refusal); (14,6) holds a corrupt slot there as well
//   the foreground block adds (8,8) a first-person mark whose map says "ambiguous" (refused) and (10,12) one whose map is valid.
// Every texel is checked against a model computed from the shader's own camera arithmetic, in doubles, so a stray texel fails the case that
// names it; the census counts the classes; and the HDR finish shows, per probe pixel, the raw frame where the backend was handed a rejection and
// the backend's result (the stub's green) where it was not. Three more frames follow: a camera moved along its view axis so that every camera
// term is in range and nothing is refused (bit for bit the same with the key on and off); the same moved 2.4999 toward the surface, where the
// camera term overflows half-float range for most texels, is formed (large, finite) for the 6 x 6 at the centre, and cannot be formed (w < 0)
// for a patch at depth .02, a corrupt slot among it; and a control where the unreprojectable pixel's engine rows are sound.
//
// Every key-on assertion fails if the shader change is reverted (the first mutant below is exactly that); the key-off ones pin that the old
// behaviour is unchanged, and fail if the key is on whether or not the frame asks (the second mutant). The scenario is then run against the prep
// with one rule of the change taken out at a time: a scenario that cannot fail proves nothing.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace keepgpu {
constexpr UINT W = 16, H = 16;
constexpr float kShiftX = .9375f;   // the current camera's x translation: 320 * .9375 * .01 = 3 render pixels at depth .01
constexpr UINT kBandFrom = 13;      // columns 13..15: the camera term sends them past x = 1 at depth .01 or .02

struct Px { UINT x, y; };
constexpr Px kPlain{1, 1}, kStaleFail{2, 10}, kStaleKept{2, 13}, kCorrupt{3, 3}, kSentinel{6, 2}, kSky{10, 2}, kMasked{11, 6}, kUnreproj{9, 13},
    kBadDepth{8, 4}, kBand{14, 4}, kBandCorrupt{14, 6}, kForeignRefused{8, 8}, kForeignValid{10, 12}, kCamCorrupt{2, 2};
inline bool same(Px a, Px b) { return a.x == b.x && a.y == b.y; }

inline const char* msg(const char* fmt, ...) {   // one buffer: every message is consumed by check() before the next is made
    static char buffer[640];
    va_list args; va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

// cameraBefore in doubles for this fixture (still rotation, near .025; the current camera translated by (tx, 0, dz), the previous one at rest):
// the motion in render pixels, whether the term can be formed at all (w > 0), and the prep's own `formed` (finite and within half-float range).
struct Term { double mx = 0, my = 0; bool valid = false, formed = false; };
inline Term cameraTerm(double tx, double dz, UINT x, UINT y, double depth) {
    Term t;
    const double u = (x + .5) / W, v = (y + .5) / H, iz = depth / double(.025f);
    const double bx = (2 * u - 1) + tx * iz, by = 1 - 2 * v, bw = 1 + dz * iz;
    t.valid = bw > 0;
    if (!t.valid) return t;
    t.mx = ((bx / bw * .5 + .5) - u) * W;
    t.my = ((by / bw * -.5 + .5) - v) * H;
    t.formed = std::isfinite(t.mx) && std::isfinite(t.my) && std::abs(t.mx) <= 65504 && std::abs(t.my) <= 65504;
    return t;
}
inline bool closeTo(double got, double want, double rel = .002) { return std::abs(got - want) <= .01 + rel * std::abs(want); }

// What the backend was handed for one frame, and H after the finish.
struct Seen {
    bool ok = false, reset = false;
    std::vector<unsigned char> mask;   // the rejection texture: 255 refused
    std::vector<float> motion;         // two floats a texel
    float depth88 = 0;                 // texel (8, 8) of the depth texture the backend was handed
    uint64_t hash = 0;                 // over the depth, the rejection and the motion, bit for bit
    std::vector<uint32_t> out;         // H (R11G11B10F) after the resolve
    size_t at(UINT x, UINT y) const { return size_t(y) * W + x; }
    bool refused(UINT x, UINT y) const { return at(x, y) < mask.size() && mask[at(x, y)] != 0; }
    float mx(UINT x, UINT y) const { return 2 * at(x, y) + 1 < motion.size() ? motion[2 * at(x, y)] : -1e30f; }
    float my(UINT x, UINT y) const { return 2 * at(x, y) + 1 < motion.size() ? motion[2 * at(x, y) + 1] : -1e30f; }
    unsigned refusedAll() const { unsigned n = 0; for (unsigned char b : mask) n += b != 0; return n; }
    double maxMotion() const { double m = 0; for (float v : motion) m = std::max(m, double(std::abs(v))); return m; }
    // H holds the raw frame (1, 1, 1) at a texel (a rejection was handed) or the backend's result, the stub's (0, 1, 0) (none was).
    bool shows(Px p, bool raw) const {
        if (out.size() != size_t(W) * H) return false;
        double c[3]; hdrgpu::unpack(out[at(p.x, p.y)], c);
        return raw ? (c[0] > .9 && c[1] > .9 && c[2] > .9) : (c[0] < .1 && c[1] > .9 && c[2] < .1);
    }
};

struct Rig {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ResolveFixture fx;
    ComPtr<ID3D11Texture2D> depth, color, map;
    ComPtr<ID3D11Buffer> pool;   // two records: code 1 points at record 0, code 3 at record 1
    ComPtr<ID3D11ShaderResourceView> depthView, colorView, mapView, poolView;
    std::vector<float> z, slots, mapData;
    std::vector<uint32_t> rgb;
    uint32_t records[2][84]{};
    edvr::FlatMonoResolveFrame f{};
    bool foreground = false;

    Rig(ID3D11Device* d, ID3D11DeviceContext* c)
        : device(d), context(c), fx(d, c), z(W * H, .01f), slots(W * H * 2), mapData(W * H * 4, 0.f), rgb(W * H, hdrgpu::pack(1, 1, 1)) {
        color = texture(d, W, H, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, rgb.data(), W * 4);
        depth = texture(d, W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, z.data(), W * 4);
        map = texture(d, W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, mapData.data(), W * 16);
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(records); bd.StructureByteStride = 336;
        bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = records;
        check(SUCCEEDED(d->CreateBuffer(&bd, &initial, pool.GetAddressOf())), "keep refused: the two-record pool buffer");
        colorView = view(d, color.Get()); depthView = view(d, depth.Get()); mapView = view(d, map.Get()); poolView = view(d, pool.Get());
        start(edvr::FlatMonoResolveMode::Dlss);
    }
    // A fresh run: the next frame is a reset frame, a still camera, no slot, a uniform depth, the game's frame a uniform (1, 1, 1), sound engine rows.
    void start(edvr::FlatMonoResolveMode mode) {
        f = edvr::FlatMonoResolveFrame{};
        f.color = colorView.Get(); f.depth = depthView.Get(); f.hdr = true;
        f.renderWidth = f.renderHeight = f.outputWidth = f.outputHeight = W;
        f.deltaMs = 16; camera(f.camera); camera(f.previousCamera); fx.engine(f); f.engine.pool = poolView.Get();
        f.mode = mode; f.frame = 62000; f.reset = true;
        foreground = false;
        std::fill(z.begin(), z.end(), .01f); noSlots(); setRecord(0); engineRows(false);
        std::fill(mapData.begin(), mapData.end(), 0.f);
        std::fill(rgb.begin(), rgb.end(), hdrgpu::pack(1, 1, 1));
    }
    void fillZ(float v) { std::fill(z.begin(), z.end(), v); }
    void noSlots() { for (size_t i = 0; i < slots.size(); i += 2) { slots[i] = -1; slots[i + 1] = .01f; } }
    void put(Px p, float code, float slotDepth) { slots[(size_t(p.y) * W + p.x) * 2] = code; slots[(size_t(p.y) * W + p.x) * 2 + 1] = slotDepth; }
    // One pool record: a rigid pose moving -.3125 in x, whose marker word is joined, masked or absent (0 none, 1 joined, 2 masked).
    static void fillRecord(uint32_t (&r)[84], uint32_t markerKind) {
        std::memset(r, 0, sizeof(r));
        r[1] = r[77] = bits(1); r[2] = r[78] = 0x7fff7fff; r[3] = r[79] = 0xfffe7fff;
        r[4] = bits(0); r[5] = bits(0); r[6] = bits(2.5f);
        r[73] = bits(-.3125f); r[74] = bits(0); r[75] = bits(2.5f);
        edvr::engine_velocity_emit::Pose np{{r[4], r[5], r[6], r[2], r[3]}};
        edvr::engine_velocity_emit::Pose pp{{r[73], r[74], r[75], r[78], r[79]}};
        r[72] = markerKind == 0 ? 0u : (markerKind == 1 ? 0x7FC0ED01u : 0x7FC0ED02u) ^ edvr::engine_velocity_emit::markerHash(np, pp, 77);
    }
    // Record 0 is what every code-1 slot points at (its marker as given); record 1, what code 3 points at, is always a joined one.
    void setRecord(uint32_t markerKind) { fillRecord(records[0], markerKind); fillRecord(records[1], 1); }
    // The engine's own scene constants (EN, b1) the joined records are reprojected by: the fixture's still camera, or rows that are not the pool
    // families' encoding (a column that is not constant in clip z), by which no joined record can be reprojected. The frame stamp stays 77.
    void engineRows(bool broken) {
        float scene[277][4]{}; float cam[6][4]; camera(cam); std::memcpy(scene + 270, cam, sizeof(cam));
        if (broken) scene[270][2] = 1.f;
        const uint32_t stamp = 77; std::memcpy(&scene[276][0], &stamp, 4);
        context->UpdateSubresource(fx.now.Get(), 0, nullptr, scene, 0, 0);
    }
    // The shared layout (see the top of the file). Depth .01 but for a .02 patch where the stale-failed pixel's check looks, the sky and the 2.
    void layout() {
        std::fill(z.begin(), z.end(), .01f);
        for (UINT y = 9; y <= 11; ++y) for (UINT x = 4; x <= 6; ++x) z[size_t(y) * W + x] = .02f;
        z[size_t(kSky.y) * W + kSky.x] = 0.f; z[size_t(kBadDepth.y) * W + kBadDepth.x] = 2.f;
        noSlots(); setRecord(2); engineRows(true);
        put(kStaleFail, 1, .5f);       // the slot's depth is not the pixel's: stale; last frame's depth where the camera term looks is .02, not .01
        put(kStaleKept, 1, .5f);       // stale too, but what its check looks at is its own depth
        put(kCorrupt, 2, .01f);        // an even code
        put(kSentinel, 4294967296.0f, .02f);   // the out-of-range marker
        put(kSky, 1, .5f);             // a slot under a pixel with no depth
        put(kMasked, 1, .01f);         // the pixel's depth is the slot's (not stale) and record 0's marker says EDVR could not follow it
        put(kUnreproj, 3, .01f);       // record 1: joined and moved, and the engine rows above fail its reprojection
        put(kBandCorrupt, 2, .01f);    // an even code where the camera term also leaves the frame
        f.camera[5][0] = kShiftX;
        f.steadyDetail = true;
    }
    // The first-person marks (the owner plane's negative code at the pixel's own raw depth) and the map they are placed by.
    void foregroundMarks() {
        foreground = true;
        const Px marks[] = {kForeignRefused, kForeignValid};
        for (const Px p : marks) {
            put(p, -3, z[size_t(p.y) * W + p.x]);
            float* m = &mapData[(size_t(p.y) * W + p.x) * 4];
            m[0] = 1.75f; m[1] = -.5f; m[2] = .0025f; m[3] = same(p, kForeignValid) ? 1.f : 2.f;   // w: 1 valid, 2 ambiguous history
        }
    }
    Seen run(const char* what, bool wantOk = true) {
        context->UpdateSubresource(color.Get(), 0, nullptr, rgb.data(), W * 4, 0);
        context->UpdateSubresource(depth.Get(), 0, nullptr, z.data(), W * 4, 0);
        context->UpdateSubresource(fx.slotTexture.Get(), 0, nullptr, slots.data(), W * 8, 0);
        context->UpdateSubresource(pool.Get(), 0, nullptr, records, 0, 0);
        context->UpdateSubresource(map.Get(), 0, nullptr, mapData.data(), W * 16, 0);
        f.foregroundMotion = foreground ? mapView.Get() : nullptr;
        f.foregroundQualified = foreground; f.foregroundFrame = foreground ? f.frame : 0;
        expectedJx = f.jitterX; expectedJy = f.jitterY;
        fx.bindOriginal();
        ComPtr<ID3D11ShaderResourceView> resolved; const char* why = nullptr;
        const int before = backendCalls;
        const bool ok = edvr::flatMonoResolve(device, context, f, resolved.GetAddressOf(), &why);
        if (ok != wantOk) std::printf("info: keep-refused scenario \"%s\": resolver reason %s\n", what, why ? why : "none");
        check(ok == wantOk, what);
        check(fx.restored(), "keep refused: the game's whole pipeline comes back untouched");
        Seen s; s.ok = ok;
        if (backendCalls > before) {
            s.mask = observedMaskAll; s.motion = observedMotionAll; s.depth88 = observedDepth; s.hash = observedMotionHash; s.reset = backendReset;
        }
        std::vector<unsigned char> bytes;
        if (readWhole(context, color.Get(), bytes, 4)) { s.out.resize(W * H); std::memcpy(s.out.data(), bytes.data(), bytes.size()); }
        ++f.frame;
        return s;
    }
};

// ---- the model ----------------------------------------------------------------------------------------------------------------------------
// What the backend should be handed for texel (x, y) of the shared layout: whether its history is refused, and its motion.
struct Want { bool refused; double mx, my; };
inline Want wantTexel(UINT x, UINT y, float depth, bool keep, bool fg) {
    const Px p{x, y};
    if (fg && same(p, kForeignValid)) return {false, 1.75, -.5};   // a first-person pixel the map places: the map's motion
    if (fg && same(p, kForeignRefused)) return {true, 0, 0};       // one it cannot place: refused whatever the key says
    if (same(p, kBadDepth)) return {true, 0, 0};                   // no depth: never the world's, so no motion is formed for it
    if (same(p, kMasked) || same(p, kUnreproj)) return {true, 0, 0};   // a mover's record: nothing is formed, key or no key
    const Term t = cameraTerm(kShiftX, 0, x, y, depth);
    const bool refusedByThePrep = x >= kBandFrom || same(p, kStaleFail) || same(p, kCorrupt) || same(p, kSentinel) || same(p, kSky);
    if (!refusedByThePrep) return {false, t.mx, t.my};
    return keep ? Want{false, t.mx, t.my} : Want{true, 0, 0};
}

struct Probe { char group; const char* what; Px p; };
inline const Probe* probes(size_t* count) {
    static const Probe table[] = {
        {'b', "a stale slot whose previous-depth check failed", kStaleFail},
        {'c', "a corrupt slot (an even code)", kCorrupt},
        {'c', "the out-of-range sentinel", kSentinel},
        {'c', "a slot under a pixel with no depth", kSky},
        {'c', "a masked record (a mover's)", kMasked},
        {'c', "an unreprojectable record (a moved joined record whose engine rows fail)", kUnreproj},
        {'d', "a pixel whose camera term leaves the frame", kBand},
        {'d', "a corrupt slot whose camera term also leaves the frame", kBandCorrupt},
        {'e', "a pixel whose depth is no depth", kBadDepth},
        {'f', "a plain pixel", kPlain},
        {'f', "a stale slot whose previous-depth check passed", kStaleKept},
    };
    *count = sizeof(table) / sizeof(table[0]);
    return table;
}

// One resolved frame of the layout against the model: every texel, then each probe by name (rejection, motion, and what H shows).
inline void verifyLayout(const Rig& r, const Seen& s, bool keep, bool fg, const char* tag) {
    const char* key = keep ? "on" : "off";
    unsigned bad = 0; char first[200] = "";
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x) {
            const Want w = wantTexel(x, y, r.z[size_t(y) * W + x], keep, fg);
            const bool ok = s.refused(x, y) == w.refused && closeTo(s.mx(x, y), w.mx) && closeTo(s.my(x, y), w.my);
            if (!ok && !bad++) std::snprintf(first, sizeof(first), "texel (%u,%u): refused %d motion (%.3f,%.3f), the model says refused %d motion (%.3f,%.3f)",
                                             x, y, int(s.refused(x, y)), s.mx(x, y), s.my(x, y), int(w.refused), w.mx, w.my);
        }
    if (bad && !mutationFailures) std::printf("info: keep refused (%s, key %s): %u texels differ; first: %s\n", tag, key, bad, first);
    check(s.ok && s.mask.size() == W * H && s.motion.size() == 2 * W * H && bad == 0,
          msg("keep refused (%s, key %s): every texel's rejection and motion are the model's (with the key on a refused pixel a motion was formed for carries it with no rejection; masked and unreprojectable records and pixels with no depth stay refused with none)", tag, key));
    size_t count = 0; const Probe* table = probes(&count);
    for (size_t i = 0; i < count; ++i) {
        const Probe& pr = table[i];
        const Want w = wantTexel(pr.p.x, pr.p.y, r.z[size_t(pr.p.y) * W + pr.p.x], keep, fg);
        const bool moved = std::abs(s.mx(pr.p.x, pr.p.y)) + std::abs(s.my(pr.p.x, pr.p.y)) > .05;
        check(s.refused(pr.p.x, pr.p.y) == w.refused && closeTo(s.mx(pr.p.x, pr.p.y), w.mx) && closeTo(s.my(pr.p.x, pr.p.y), w.my) &&
                  (w.refused ? !moved : (moved || std::abs(w.mx) + std::abs(w.my) <= .05)),
              msg("keep refused (%c), %s, key %s: %s: %s", pr.group, tag, key, pr.what,
                  w.refused ? "refused, no motion" : "rejection 0 and the motion is the camera term"));
        // The HDR finish: the raw frame where a rejection was handed, the backend's result where none was.
        check(s.shows(pr.p, w.refused),
              msg("keep refused (g), %s, key %s: %s: H shows %s", tag, key, pr.what, w.refused ? "the raw frame" : "the backend's result"));
    }
    if (fg) {
        const Want refusedWant = wantTexel(kForeignRefused.x, kForeignRefused.y, .01f, keep, true);
        check(s.refused(kForeignRefused.x, kForeignRefused.y) == refusedWant.refused && s.mx(kForeignRefused.x, kForeignRefused.y) == 0 &&
                  s.my(kForeignRefused.x, kForeignRefused.y) == 0 && s.depth88 == .0025f && s.shows(kForeignRefused, true),
              msg("keep refused (e), %s, key %s: a first-person pixel the foreground map cannot place stays refused with no motion (canonical depth kept) and H shows the raw frame", tag, key));
        check(!s.refused(kForeignValid.x, kForeignValid.y) && closeTo(s.mx(kForeignValid.x, kForeignValid.y), 1.75) &&
                  closeTo(s.my(kForeignValid.x, kForeignValid.y), -.5),
              msg("keep refused (f), %s, key %s: a first-person pixel the foreground map places keeps the map's motion, accepted", tag, key));
    } else {
        check(s.depth88 == .01f, msg("keep refused, %s, key %s: the depth the backend is handed is the pixel's own, unchanged by the key", tag, key));
    }
}

// The "far" frame: the current camera moved 2.4999 toward the surface along its view axis. At depth .01 w = 1 - 2.4999 * .4 = 4e-5, at .02 it is
// negative. Each texel against the model, the three kinds of texel apart: a camera term formed (large, finite), one beyond half-float range, and one
// that cannot be formed (class Camera).
struct FarResult { bool shaped = false, formedOk = true, unformedOk = true, cameraOk = true; unsigned formed = 0, unformed = 0, camera = 0; };
inline FarResult verifyFar(const Rig& r, const Seen& s, bool keep) {
    FarResult out;
    out.shaped = s.ok && s.mask.size() == W * H && s.motion.size() == 2 * W * H;
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x) {
            const Term t = cameraTerm(0, double(-2.4999f), x, y, double(r.z[size_t(y) * W + x]));
            const double big = std::max(std::abs(t.mx), std::abs(t.my));
            if (t.valid && big > 64000 && big < 67000) continue;   // within a per-cent of the limit: the shader's float arithmetic may land either side
            const bool kept = keep && t.valid && t.formed;           // a motion was formed: no rejection with the key on, and that motion
            const bool ok = s.refused(x, y) == !kept &&
                            (kept ? closeTo(s.mx(x, y), t.mx, .01) && closeTo(s.my(x, y), t.my, .01) : s.mx(x, y) == 0 && s.my(x, y) == 0);
            if (!t.valid) { ++out.camera; out.cameraOk = out.cameraOk && ok; }
            else if (t.formed) { ++out.formed; out.formedOk = out.formedOk && ok; }
            else { ++out.unformed; out.unformedOk = out.unformedOk && ok; }
        }
    return out;
}

inline refusalgpu::Taken takeCensus(ID3D11DeviceContext* context) {
    refusalgpu::Taken taken;
    for (int i = 0; i < 3000 && taken.frames < 1; ++i) {   // the sample lands a few frames later; generous, the rig runs beside others
        context->Flush();
        taken.add(edvr::flatMonoResolveTakeRefusalCensus());
        if (taken.frames < 1) Sleep(2);
    }
    return taken;
}
inline void expectedCensus(bool fg, uint64_t (&want)[edvr::kFlatMonoRefusalSlots]) {
    using namespace edvr;
    for (uint64_t& n : want) n = 0;
    want[kFlatMonoClassStale] = 1;                  // the stale slot whose check failed
    want[kFlatMonoClassCorrupt] = 2;                // the corrupt slot, and the one in the band (its engine class, not the range one)
    want[kFlatMonoClassSentinel] = 2;               // the marker, and the sky
    want[kFlatMonoClassMasked] = 1;
    want[kFlatMonoClassUnreprojectable] = 1;
    want[kFlatMonoClassRange] = 3 * H - 1;          // columns 13..15 but the corrupt slot's texel
    want[kFlatMonoClassDepth] = 1;                  // the pixel whose depth is 2
    want[kFlatMonoRefusalStaleKept] = 1;            // the stale slot whose check passed
    if (fg) want[kFlatMonoClassWeaponRefused] = 1;  // the first-person pixel the map cannot place
}
inline bool sameCounts(const refusalgpu::Taken& t, const uint64_t (&want)[edvr::kFlatMonoRefusalSlots]) {
    bool same = t.frames == 1;
    for (uint32_t i = 0; i < edvr::kFlatMonoRefusalSlots; ++i) same = same && t.counts[i] == want[i];
    return same;
}
inline void printCounts(const char* what, const refusalgpu::Taken& t, const uint64_t (&want)[edvr::kFlatMonoRefusalSlots]) {
    if (mutationFailures) return;
    std::printf("info: keep refused census (%s): frames %llu, counts:", what, static_cast<unsigned long long>(t.frames));
    for (uint32_t i = 0; i < edvr::kFlatMonoRefusalSlots; ++i) std::printf(" %llu/%llu", static_cast<unsigned long long>(t.counts[i]), static_cast<unsigned long long>(want[i]));
    std::printf(" (got/want)\n");
}

// (g) EDVR's own TAA never gets the bit: keepRefusedHistory changes nothing for it. A hot pixel (10000 in a field of 100) at a texel the prep
// accepts is blended into its history (about 111); at a texel it refuses (a corrupt slot) it is the current frame (10000, 9984 once the format
// has rounded it). With bit 4 set under TAA (measured with the resolver's line forced that way) the shader's TAA path takes debug.w != 0 for
// "the alternate-camera inputs are bound", finds them unbound and takes no history anywhere: the accepted hot pixel comes out 9984 as well.
inline void taaScenario(Rig& r) {
    std::vector<uint32_t> outputs[2];
    double accepted[2][3] = {}, refused[2][3] = {};
    for (const bool keep : {false, true}) {
        edvr::flatMonoResolveReset();
        r.start(edvr::FlatMonoResolveMode::Taa);
        r.f.keepRefusedHistory = keep;
        std::fill(r.rgb.begin(), r.rgb.end(), hdrgpu::pack(100, 100, 100));
        r.run(msg("keep refused (TAA): the reset frame, key %s", keep ? "on" : "off"));
        r.f.reset = false;
        r.rgb[size_t(4) * W + 4] = r.rgb[size_t(8) * W + 8] = hdrgpu::pack(10000, 10000, 10000);
        r.put(Px{8, 8}, 2, .01f);   // an even code: a corrupt slot, which the prep refuses in every mode
        const Seen s = r.run(msg("keep refused (TAA): the hot-pixel frame, key %s", keep ? "on" : "off"));
        outputs[keep] = s.out;
        if (s.out.size() == size_t(W) * H) { hdrgpu::unpack(s.out[size_t(4) * W + 4], accepted[keep]); hdrgpu::unpack(s.out[size_t(8) * W + 8], refused[keep]); }
        if (!mutationFailures)
            std::printf("flat mono resolve: keep refused (TAA, key %s): hot pixel %.1f where history is kept, %.1f where it is refused\n",
                        keep ? "on" : "off", accepted[keep][0], refused[keep][0]);
        check(accepted[keep][0] > 90 && accepted[keep][0] < 130 && refused[keep][0] > 9000 && refused[keep][0] < 11000,
              msg("keep refused (g), TAA, key %s: a hot pixel is blended into its history where the prep accepts it (about 111) and is the current frame where it refuses it (10000)",
                  keep ? "on" : "off"));
    }
    check(!outputs[0].empty() && outputs[0] == outputs[1],
          "keep refused (g), TAA: keepRefusedHistory has no effect on EDVR's own TAA (the whole of H is identical with it on and off)");
}

// ---- the scenario, against whichever prep the resolver was last initialised with ----------------------------------------------------------
inline void scenario(Rig& r) {
    using namespace edvr;
    for (const FlatMonoResolveMode mode : {FlatMonoResolveMode::Dlaa, FlatMonoResolveMode::Dlss, FlatMonoResolveMode::Fsr}) {
        const char* name = mode == FlatMonoResolveMode::Dlaa ? "DLAA" : mode == FlatMonoResolveMode::Dlss ? "DLSS" : "FSR";
        const bool census = mode == FlatMonoResolveMode::Dlss;   // the class byte is the prep's, whatever the backend: counted once, through DLSS
        // Frames with the key as given (four when the census asks, of which one is sampled), the last one's result, and the census read back.
        auto frames = [&](bool keep, const char* what, refusalgpu::Taken* taken) {
            r.f.keepRefusedHistory = keep;
            r.f.refusalCensus = census;
            (void)flatMonoResolveTakeRefusalCensus();   // start the sums from zero
            Seen last;
            for (unsigned i = 0; i < (census ? kFlatMonoRefusalEvery : 1u); ++i) last = r.run(what);
            if (census && taken) *taken = takeCensus(r.context);
            r.f.refusalCensus = false;
            return last;
        };
        for (const bool fg : {false, true}) {
            char tag[80]; std::snprintf(tag, sizeof(tag), "%s%s", name, fg ? " with the foreground map" : "");
            char what[200];
            flatMonoResolveReset();
            r.start(mode);
            r.layout();
            if (fg) r.foregroundMarks();

            // The reset frame that starts the run: refused everywhere, key off.
            r.f.keepRefusedHistory = false;
            std::snprintf(what, sizeof(what), "keep refused: %s: the reset frame that starts the run", tag);
            const Seen firstFrame = r.run(what);
            check(firstFrame.reset && firstFrame.refusedAll() == W * H, msg("keep refused, %s: the first frame is a reset frame and refuses every pixel", tag));
            r.f.reset = false;

            // Key off, then on, over continuing frames.
            refusalgpu::Taken taken[2];
            for (const bool keep : {false, true}) {
                std::snprintf(what, sizeof(what), "keep refused: %s: a continuing frame, key %s", tag, keep ? "on" : "off");
                const Seen s = frames(keep, what, &taken[keep]);
                check(!s.reset, msg("keep refused, %s: a continuing frame is not a reset frame", tag));
                verifyLayout(r, s, keep, fg, tag);
                if (census) {
                    uint64_t want[edvr::kFlatMonoRefusalSlots];
                    expectedCensus(fg, want);
                    const bool right = sameCounts(taken[keep], want);
                    if (!right) printCounts(msg("%s, key %s", tag, keep ? "on" : "off"), taken[keep], want);
                    check(right,
                          msg("keep refused, %s, key %s: the census counts the classes the layout has (stale 1, corrupt 2, sentinel 2, masked 1, unreprojectable 1, range 47, depth 1, stale-kept 1%s), refused pixels keep their class",
                              tag, keep ? "on" : "off", fg ? ", weapon-refused 1" : ""));
                }
            }
            if (census) {
                bool equal = taken[0].frames == 1 && taken[1].frames == 1;
                for (uint32_t i = 0; i < edvr::kFlatMonoRefusalSlots; ++i) equal = equal && taken[0].counts[i] == taken[1].counts[i];
                check(equal, msg("keep refused, %s: the census counts the same with the key on and off (the class byte and its refused bit do not depend on it)", tag));
            }

            if (!fg) {
                // (c) The control for the unreprojectable pixel: the same moved joined record with sound engine rows is joined and accepted with the
                // engine's own motion (-1, 0), so its refusal above is the rows' doing and not the record's.
                r.f.keepRefusedHistory = true;
                r.engineRows(false);
                std::snprintf(what, sizeof(what), "keep refused: %s: the control, sound engine rows", tag);
                const Seen joined = r.run(what);
                r.engineRows(true);
                check(!joined.refused(kUnreproj.x, kUnreproj.y) && closeTo(joined.mx(kUnreproj.x, kUnreproj.y), -1) && closeTo(joined.my(kUnreproj.x, kUnreproj.y), 0),
                      msg("keep refused (c), %s: control: the unreprojectable pixel's record with sound engine rows is joined and accepted with the engine's motion (-1, 0)", tag));
            }

            // (e) A reset frame with the key on is still refused everywhere: nothing reached the world branch.
            r.f.keepRefusedHistory = true; r.f.reset = true;
            std::snprintf(what, sizeof(what), "keep refused: %s: a reset frame, key on", tag);
            const Seen resetOn = r.run(what);
            r.f.reset = false;
            check(resetOn.reset && resetOn.refusedAll() == W * H && resetOn.maxMotion() == 0,
                  msg("keep refused (e), %s, key on: a reset frame refuses every pixel with no motion", tag));
            if (fg) continue;

            // (f) A frame with nothing refused in it is bit for bit the same with the key on and off. The camera moves along its view axis, so every
            // pixel's camera term is in range and not zero.
            r.noSlots(); r.setRecord(0); r.engineRows(false); r.fillZ(.01f);
            r.f.camera[5][0] = 0; r.f.camera[5][2] = .25f;
            r.f.keepRefusedHistory = false;
            std::snprintf(what, sizeof(what), "keep refused: %s: a frame with nothing refused, key off", tag);
            const Seen calmOff = r.run(what);
            r.f.keepRefusedHistory = true;
            std::snprintf(what, sizeof(what), "keep refused: %s: a frame with nothing refused, key on", tag);
            const Seen calmOn = r.run(what);
            check(calmOff.refusedAll() == 0 && calmOn.refusedAll() == 0 && calmOff.maxMotion() > .1 && calmOff.hash != 0 && calmOn.hash == calmOff.hash &&
                      calmOn.out == calmOff.out,
                  msg("keep refused (f), %s: a frame in which nothing is refused is bit for bit the same with the key on and off (depth, rejection, motion and H)", tag));

            // (d) and (h) The far frame: the camera term is enormous. In half-float range (the 6 x 6 texels at the centre) it is formed and kept; beyond
            // it, none is formed: refused, no motion, never an infinity. On a patch at depth .02 it cannot be formed at all (class Camera), a corrupt
            // slot among it (class Corrupt, which it keeps): refused, no motion.
            r.fillZ(.01f);
            for (UINT y = 0; y < 4; ++y) for (UINT x = 0; x < 4; ++x) r.z[size_t(y) * W + x] = .02f;
            r.put(kCamCorrupt, 2, .02f);
            r.f.camera[5][2] = -2.4999f;
            Seen farFrame[2]; refusalgpu::Taken farCensus[2];
            for (const bool keep : {false, true}) {
                std::snprintf(what, sizeof(what), "keep refused: %s: a camera term far out of range, key %s", tag, keep ? "on" : "off");
                farFrame[keep] = frames(keep, what, &farCensus[keep]);
            }
            const FarResult farOff = verifyFar(r, farFrame[0], false), farOn = verifyFar(r, farFrame[1], true);
            check(farOff.shaped && farOff.formedOk && farOff.unformedOk && farOff.cameraOk && farFrame[0].refusedAll() == W * H && farFrame[0].maxMotion() == 0,
                  msg("keep refused (d), %s, key off: every pixel of a frame whose camera term is far out of range is refused with no motion", tag));
            check(farOn.shaped && farOn.formed > 0 && farOn.formedOk,
                  msg("keep refused (d), %s, key on: a camera term far off-screen but inside half-float range (%u texels) is kept: rejection 0 and its motion", tag, farOn.formed));
            check(farOn.shaped && farOn.unformed > 0 && farOn.unformedOk,
                  msg("keep refused (d), %s, key on: a camera term beyond half-float range (%u texels) forms no motion: refused, motion 0, never an infinity", tag, farOn.unformed));
            check(farOn.shaped && farOn.camera > 0 && farOn.cameraOk,
                  msg("keep refused (h), %s, key on: a camera term that cannot be formed (behind the previous camera, class Camera; %u texels) stays refused with no motion, as does a corrupt slot there", tag, farOn.camera));
            if (census) {
                uint64_t want[edvr::kFlatMonoRefusalSlots] = {};
                want[kFlatMonoClassCamera] = farOn.camera - 1;   // the corrupt slot's texel keeps its own class
                want[kFlatMonoClassCorrupt] = 1;
                want[kFlatMonoClassRange] = W * H - farOn.camera;   // formed or not, a camera term that was formed and went out of range is a range refusal
                const bool right = sameCounts(farCensus[0], want) && sameCounts(farCensus[1], want);
                if (!right) { printCounts("far frame, key off", farCensus[0], want); printCounts("far frame, key on", farCensus[1], want); }
                check(right, msg("keep refused (h), %s: the census counts the camera class (15), the corrupt slot that keeps its class (1) and the range refusals (240), the same with the key on and off", tag));
            }
            r.f.camera[5][2] = 0;
        }
    }
    taaScenario(r);
}
}  // namespace keepgpu

// The scenario on the shipped prep, then on the prep with the change taken out (the first mutant) and with one rule of it taken out at a time:
// every one must fail the scenario, and the unmutated source compiled here must pass it through the same seam.
inline void keepRefusedGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using namespace edvr;
    keepgpu::Rig rig(device, context);
    keepgpu::scenario(rig);
    struct Mutant { const char* name; const char* from; const char* to; };
    static const char* const kKey = "const bool keepRefused=(debug.w&4)!=0;";
    static const char* const kReject = "OutRejection[q]=(keepRefused && formed)?0:reject;";
    static const char* const kSilent = "const bool silentRecord=kind==2 && (cls==kClassSentinel || cls==kClassCorrupt);";
    static const Mutant mutants[] = {
        {"the key does nothing (the shader change reverted)", kKey, "const bool keepRefused=false;"},
        {"the key is on whether or not the frame asks for it", kKey, "const bool keepRefused=true;"},
        {"a corrupt or sentinel record does not form the camera term",
         "if(kind==0||kind==3||(keepRefused && silentRecord))valid=cameraBefore(rawUv,depth,before);", "if(kind==0||kind==3)valid=cameraBefore(rawUv,depth,before);"},
        {"movers get the camera term (masked and unreprojectable records form motion too)", kSilent, "const bool silentRecord=kind==2;"},
        {"a refused pixel's formed motion is dropped as before", "if(reject!=0 && !(keepRefused && formed))motion=0;", "if(reject!=0)motion=0;"},
        {"a refused pixel is still handed its rejection", kReject, "OutRejection[q]=reject;"},
        {"every refused pixel loses its rejection, formed or not", kReject, "OutRejection[q]=keepRefused?0:reject;"},
        {"rejection dropped without formed motion (the old world-branch flag instead of formed)", kReject,
         "OutRejection[q]=(keepRefused && (formed || (flags.x==0 && !untrusted && !foreground && !attached && isfinite(depth) && depth>=0 && depth<=1)))?0:reject;"},
        {"rejection dropped for a camera term that could not be carried (range and camera classes)", kReject,
         "OutRejection[q]=(keepRefused && (formed || cls==kClassCamera || cls==kClassRange))?0:reject;"},
        {"an engine refusal that leaves the frame is counted as a range refusal", "if(!valid && kind!=2)cls=kClassRange;", "if(!valid)cls=kClassRange;"},
        {"an engine refusal is accepted once its camera term is in range", "if(kind==2)valid=false;", ""},
        {"a camera term beyond half-float range is carried", "formed=all(isfinite(motion)) && all(abs(motion)<=65504);", "formed=all(isfinite(motion));"},
        {"the key moves the motion of every pixel, accepted ones too", "OutMotion[q]=motion;", "OutMotion[q]=keepRefused?motion+float2(.5,0):motion;"},
    };
    const std::string shipped = kFlatMonoShaderSource;
    auto runMutated = [&](const std::string& hlsl, int* failed, std::string* first) {
        std::vector<unsigned char> bytes;
        if (!fpgpu::compilePrep(hlsl, bytes)) return false;
        ComPtr<ID3D11ComputeShader> probe;   // the bytes must make a compute shader, or "the scenario fails" could mean the resolver never started
        if (FAILED(device->CreateComputeShader(bytes.data(), bytes.size(), nullptr, probe.GetAddressOf()))) return false;
        flatMonoResolveTestPrepBytecode(bytes.data(), bytes.size()); flatMonoResolveReset();
        *failed = 0; mutationFailures = failed; mutationFirst.clear();
        keepgpu::scenario(rig);
        mutationFailures = nullptr; if (first) *first = mutationFirst;
        return true;
    };
    {   // The control: the same source, compiled here and run through the same seam, is the shipped prep and passes.
        int failed = 0; std::string first;
        const bool ran = runMutated(shipped, &failed, &first);
        check(ran && failed == 0, "keep refused mutations: control: the unmutated prep, compiled here and run through the test seam, passes the whole scenario");
    }
    unsigned caught = 0;
    for (const Mutant& m : mutants) {
        bool once = false;
        const std::string hlsl = fpgpu::replaceOnce(shipped, m.from, m.to, &once);
        char what[320];
        std::snprintf(what, sizeof(what), "keep refused mutations: \"%s\": its anchor is in the shader source exactly once", m.name);
        check(once, what);
        int failed = 0; std::string first;
        const bool ran = once && runMutated(hlsl, &failed, &first);
        std::snprintf(what, sizeof(what), "keep refused mutations: \"%s\" compiles and makes a compute shader", m.name);
        check(ran, what);
        if (!ran) continue;
        std::snprintf(what, sizeof(what), "keep refused mutations: \"%s\" is caught by the scenario", m.name);
        check(failed > 0, what);
        if (failed > 0) ++caught;
        std::printf("flat mono resolve: keep-refused mutation \"%s\": %d checks fail; first: %s\n", m.name, failed, first.c_str());
    }
    std::printf("flat mono resolve: keep-refused mutations: %u of %zu caught by the scenario\n", caught, sizeof(mutants) / sizeof(mutants[0]));
    flatMonoResolveTestPrepBytecode(nullptr, 0); flatMonoResolveReset();
}
