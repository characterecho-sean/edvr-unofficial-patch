// The steady-detail depth check of the resolver's prep on WARP (design doc section 82, the depth-validated steady detail).
//
// A pixel whose engine slot a later draw overdrew (a STALE slot) has no motion record of its own. By default its history is refused (the finish
// then shows the raw jittered input, which is what shimmers on a fine pattern). With FlatMonoResolveFrame::steadyDetail it takes the camera term
// instead, but only where last frame's depth confirms it: the depth of the best of the four texels around the position the camera term sends the
// pixel to, in last frame's raster (that position plus the previous phase), must be within max(1e-6, 1% of expected) of the depth this surface
// would have had there had it not moved; where it does not the pixel is refused, exactly as before. The menu's blanket policy (staticScene) wins.
//
// What the scenario pins, on the shipped prep and then, as MUTATION CHECKS, on the prep with one rule flipped at a time (the mutated source is
// compiled here, handed to the resolver through flatMonoResolveTestPrepBytecode, and the scenario must FAIL; a scenario that cannot fail proves
// nothing): the key off refuses a stale block, makes no second depth image and counts nothing; the key on keeps it where the scene did not change;
// the backend is handed THIS frame's depth every frame through every flip of the key (the alternation); a surface that is not the one last frame
// had there is refused and, once it has been there a frame, kept; the tolerance's edges, 0.9% and 1.1%, farther and nearer, and the absolute floor
// far away; a thin line the jitter put on the neighbouring texel last frame is found in the four texels (to the left and to the right, vertical and
// horizontal), one two texels away is not, and the previous phase has its sign; a camera that moved three pixels is looked up where it says the
// pixel was, and one that moved along its view axis is compared with the camera term's expected depth, not the pixel's own; a surface that
// moved away leaves its trailing edge kept (the best of four texels: 9 of 16 stale pixels refused, 7 kept); the menu's blanket policy keeps a
// stale pixel whatever the depth says and the check does not count it; a reset frame refuses everything and counts nothing; a masked record, a corrupt code, the sentinel and the sky stay refused with the key on; the census splits the
// stale pixels into stale-kept and stale-refused. Through EDVR's own TAA (which keeps last frame's depth itself) a kept stale pixel reaches its
// history and a refused one takes the current colour, and FSR is handed the same depth the DLSS stub is.
#pragma once
#include <d3dcompiler.h>
#include "temporal_shader_bytecode.h"   // kEngineMotionCoreHlsl and kFlatMonoPrepBytecode, generated into build\gen
#include "../../src/d3d11/flat_mono_refusal.h"
#include "../../src/d3d11/flat_mono_shader_source.h"

namespace edvr {
// flat_mono_resolve.cpp, at its foot: whether the resolver holds a second depth image for a backend that keeps none. Test-only.
bool flatMonoResolveTestSecondDepth();
// Test-only (flat_mono_resolve.cpp): the prep the next initialisation makes is this bytecode instead of the shipped one.
void flatMonoResolveTestPrepBytecode(const void* bytes, size_t size);
}

namespace steadygpu {
constexpr UINT W = 16, H = 16;

// What the stub backend was handed for one frame (the DLSS and FSR stubs): the prep's output as the SDK sees it.
struct Seen {
    bool ok = false, reset = false;
    std::vector<unsigned char> mask;   // the rejection texture: 255 refused
    std::vector<float> motion;         // two floats a texel
    float depth88 = 0;                 // texel (8, 8) of the depth texture the backend was handed
    unsigned refused(UINT x0, UINT y0, UINT w, UINT h) const {
        unsigned n = 0;
        for (UINT y = y0; y < y0 + h; ++y)
            for (UINT x = x0; x < x0 + w; ++x)
                if (size_t(y) * W + x < mask.size() && mask[size_t(y) * W + x]) ++n;
        return n;
    }
    unsigned refusedAll() const { unsigned n = 0; for (unsigned char b : mask) n += b != 0; return n; }
    bool noMotion(UINT x0, UINT y0, UINT w, UINT h) const {
        for (UINT y = y0; y < y0 + h; ++y)
            for (UINT x = x0; x < x0 + w; ++x) {
                const size_t t = size_t(y) * W + x;
                if (2 * t + 1 >= motion.size() || motion[2 * t] != 0 || motion[2 * t + 1] != 0) return false;
            }
        return true;
    }
};

// The fixture's pieces for the copy route: a 16 x 16 render, a still camera, the slots, one pool record, an R32_FLOAT scene depth and an RGBA8
// colour the game rendered, and the frame the resolver is handed.
struct Rig {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ResolveFixture fx;
    ComPtr<ID3D11Texture2D> depth, color;
    ComPtr<ID3D11ShaderResourceView> depthView, colorView;
    std::vector<float> z, slots;
    std::vector<uint32_t> rgba;
    uint32_t record[84]{};
    edvr::FlatMonoResolveFrame f{};

    Rig(ID3D11Device* d, ID3D11DeviceContext* c)
        : device(d), context(c), fx(d, c), z(W * H, .01f), slots(W * H * 2), rgba(W * H, 0xff808080u) {
        depth = texture(d, W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, z.data(), W * 4);
        depthView = view(d, depth.Get());
        color = texture(d, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, rgba.data(), W * 4);
        colorView = view(d, color.Get());
        start();
    }
    // Everything back to a fresh run: a reset frame is next, the key off, a still camera, no phase, no slot, a uniform depth.
    void start(edvr::FlatMonoResolveMode mode = edvr::FlatMonoResolveMode::Dlss) {
        f = edvr::FlatMonoResolveFrame{};
        f.color = colorView.Get(); f.depth = depthView.Get();
        f.renderWidth = f.renderHeight = f.outputWidth = f.outputHeight = W;
        f.deltaMs = 16; camera(f.camera); camera(f.previousCamera); fx.engine(f);
        f.mode = mode; f.frame = 60000; f.reset = true;
        fillZ(.01f); noSlots(); setRecord(0);
        std::fill(rgba.begin(), rgba.end(), 0xff808080u);
    }
    void fillZ(float v) { std::fill(z.begin(), z.end(), v); }
    void setZ(UINT x0, UINT y0, UINT w, UINT h, float v) {
        for (UINT y = y0; y < y0 + h; ++y) for (UINT x = x0; x < x0 + w; ++x) z[size_t(y) * W + x] = v;
    }
    void noSlots() { for (size_t i = 0; i < slots.size(); i += 2) { slots[i] = -1; slots[i + 1] = .01f; } }
    void put(UINT x, UINT y, float code, float slotDepth) { slots[(size_t(y) * W + x) * 2] = code; slots[(size_t(y) * W + x) * 2 + 1] = slotDepth; }
    // A stale block: a keyed slot (code 1) whose depth is no depth the scene has (.5), so no scene value below ever matches it.
    void staleBlock(UINT x0, UINT y0, UINT w, UINT h) { for (UINT y = y0; y < y0 + h; ++y) for (UINT x = x0; x < x0 + w; ++x) put(x, y, 1, .5f); }
    // The record every code-1 slot points at: a rigid pose whose marker word is joined, masked or absent (0 none, 1 joined, 2 masked).
    void setRecord(uint32_t markerKind) {
        std::memset(record, 0, sizeof(record));
        record[1] = record[77] = bits(1); record[2] = record[78] = 0x7fff7fff; record[3] = record[79] = 0xfffe7fff;
        record[4] = bits(0); record[5] = bits(0); record[6] = bits(2.5f);
        record[73] = bits(-.3125f); record[74] = bits(0); record[75] = bits(2.5f);
        edvr::engine_velocity_emit::Pose np{{record[4], record[5], record[6], record[2], record[3]}};
        edvr::engine_velocity_emit::Pose pp{{record[73], record[74], record[75], record[78], record[79]}};
        record[72] = markerKind == 0 ? 0u : (markerKind == 1 ? 0x7FC0ED01u : 0x7FC0ED02u) ^ edvr::engine_velocity_emit::markerHash(np, pp, 77);
    }
    // The raster phase of this frame (c) and of the last (p), in render pixels, as the world route hands them to the resolver.
    void phases(float cx, float cy, float px, float py) { f.jitterX = cx; f.jitterY = cy; f.previousJitterX = px; f.previousJitterY = py; }
    Seen run(const char* what, bool wantOk = true, ComPtr<ID3D11ShaderResourceView>* out = nullptr) {
        context->UpdateSubresource(depth.Get(), 0, nullptr, z.data(), W * 4, 0);
        context->UpdateSubresource(color.Get(), 0, nullptr, rgba.data(), W * 4, 0);
        context->UpdateSubresource(fx.slotTexture.Get(), 0, nullptr, slots.data(), W * 8, 0);
        context->UpdateSubresource(fx.pool.Get(), 0, nullptr, record, 0, 0);
        expectedJx = f.jitterX; expectedJy = f.jitterY;
        fx.bindOriginal();
        ComPtr<ID3D11ShaderResourceView> resolved; const char* why = nullptr;
        const int before = backendCalls;
        const bool ok = edvr::flatMonoResolve(device, context, f, resolved.GetAddressOf(), &why);
        if (ok != wantOk) std::printf("info: steady-detail scenario \"%s\": resolver reason %s\n", what, why ? why : "none");
        check(ok == wantOk, what);
        check(fx.restored(), "steady detail: the game's whole pipeline comes back untouched");
        Seen s; s.ok = ok;
        if (backendCalls > before) { s.mask = observedMaskAll; s.motion = observedMotionAll; s.depth88 = observedDepth; s.reset = backendReset; }
        if (out) *out = resolved;
        ++f.frame;
        return s;
    }
};

inline refusalgpu::Taken takeAll(ID3D11DeviceContext* context, uint64_t wantFrames) {
    refusalgpu::Taken t;
    for (int i = 0; i < 400 && t.frames < wantFrames; ++i) {
        context->Flush();   // the sample's copy and counting pass are queued until something submits them
        t.add(edvr::flatMonoResolveTakeRefusalCensus());
        if (t.frames < wantFrames) Sleep(2);
    }
    return t;
}

// The scenario, against whichever prep the resolver was last initialised with. Nothing in it is specific to the shipped one.
inline void scenario(Rig& r, edvr::FlatMonoResolveMode mode) {
    using namespace edvr;
    flatMonoResolveReset();
    r.start(mode);
    const UINT b0 = 4, bw = 4;   // the stale block: texels (4..7, 4..7)
    auto block = [&](const Seen& s) { return s.refused(b0, b0, bw, bw); };
    auto rest = [&](const Seen& s) { return s.refusedAll() - s.refused(b0, b0, bw, bw); };
    r.staleBlock(b0, b0, bw, bw);
    const Seen resetFrame = r.run("steady detail: the reset frame that starts the run");
    check(resetFrame.refusedAll() == W * H, "steady detail: a reset frame refuses every pixel");
    r.f.reset = false;
    flatMonoResolveTakeRefusalCensus();

    // 1. The key off: refused, and nothing is made or counted.
    check(!flatMonoResolveTestSecondDepth(), "steady detail (key off): no second depth image before any frame asks");
    const Seen off1 = r.run("steady detail (key off): a continuing frame");
    check(block(off1) == 16 && rest(off1) == 0, "steady detail (key off): the stale block is refused and nothing else is");
    check(off1.noMotion(b0, b0, bw, bw), "steady detail (key off): a refused pixel has no motion");
    check(!flatMonoResolveTestSecondDepth(), "steady detail (key off): the second depth image is not made");
    {
        const auto c = flatMonoResolveTakeRefusalCensus();
        check(c.checked == 0 && c.skipped == 0, "steady detail (key off): the depth check's frames are not counted");
    }

    // 2. The key on in a still scene, flipped on and off: kept when on, refused when off, and the backend is handed THIS frame's depth every
    // time (the depth texture alternates between two images while frames ask; a frame that did not ask writes the first, as always).
    {
        const bool pattern[] = {true, true, false, true, false, false, true, true};
        bool depthRight = true, keptRight = true, refusedRight = true, secondMade = false;
        unsigned asked = 0, i = 0;
        for (bool steady : pattern) {
            r.f.steadyDetail = steady;
            r.z[8 * W + 8] = .01f + .0001f * float(++i);
            const Seen s = r.run("steady detail: the key flipped through a still scene");
            depthRight = depthRight && s.depth88 == r.z[8 * W + 8];
            if (steady) { ++asked; keptRight = keptRight && block(s) == 0 && rest(s) == 0 && s.noMotion(b0, b0, bw, bw); }
            else refusedRight = refusedRight && block(s) == 16 && rest(s) == 0;
            secondMade = secondMade || flatMonoResolveTestSecondDepth();
        }
        r.z[8 * W + 8] = .01f;
        check(depthRight, "steady detail: the depth handed to the backend is this frame's, every frame, through every flip of the key");
        check(keptRight, "steady detail (key on, still scene): last frame's depth confirms the camera term, so the stale block is kept (not refused, no motion)");
        check(refusedRight, "steady detail (key off again): the stale block is refused again, also right after frames that asked");
        check(secondMade, "steady detail: the second depth image is made by the first frame that asks");
        const auto c = flatMonoResolveTakeRefusalCensus();
        check(c.checked == asked && c.skipped == 0, "steady detail: every frame with the key on ran the depth check, and only those");
    }

    // 3. A surface that is not the one last frame had there is refused; once it has been there a frame it is kept.
    r.f.steadyDetail = true;
    r.fillZ(.01f);
    r.run("steady detail: a refresh frame, uniform depth");
    r.setZ(b0, b0, bw, bw, .02f);
    const Seen mover = r.run("steady detail: a surface arrives");
    check(block(mover) == 16 && rest(mover) == 0 && mover.noMotion(b0, b0, bw, bw),
          "steady detail: a stale pixel whose depth is not the one last frame had there (a surface that moved in) is refused");
    const Seen settled = r.run("steady detail: the surface stays");
    check(block(settled) == 0 && rest(settled) == 0, "steady detail: the same surface a frame later is kept: last frame's depth there is its own");
    // A surface that moved away: the block was .02 last frame and the background (.01) is what the pixels show now. A stale pixel whose four
    // texels last frame all held the block (x and y below the block's last column and row: 9 of the 16) is refused. One whose four texels reach
    // the background beside the block (the 7 on its right and bottom edge) finds what it shows a texel over and is kept: the best of four's
    // price at an edge (design doc section 82, the depth-validated steady detail): the check cannot see sub-texel motion.
    r.fillZ(.01f);
    const Seen gone = r.run("steady detail: the surface moves away");
    check(block(gone) == 9 && rest(gone) == 0,
          "steady detail: where a surface moved away, a stale pixel whose four texels all held it is refused (9 of the 16), and one at its trailing edge, whose four reach the background beside it, is kept (7)");

    // 4. The tolerance's edges and its floor.
    auto edge = [&](float previous, float current, bool kept, const char* what) {
        r.fillZ(previous);
        r.run("steady detail: the tolerance's previous frame");
        r.fillZ(previous);
        r.setZ(b0, b0, bw, bw, current);
        const Seen s = r.run(what);
        check(block(s) == (kept ? 0u : 16u) && rest(s) == 0, what);
    };
    edge(.01f, .01f * 1.009f, true, "steady detail: a surface 0.9% farther than last frame's depth is kept (inside the 1% tolerance)");
    edge(.01f, .01f * 1.011f, false, "steady detail: a surface 1.1% farther than last frame's depth is refused");
    edge(.01f, .01f / 1.009f, true, "steady detail: a surface 0.9% nearer than last frame's depth is kept");
    edge(.01f, .01f / 1.011f, false, "steady detail: a surface 1.1% nearer than last frame's depth is refused");
    edge(1e-5f, 1.05e-5f, true, "steady detail: far away (depth 1e-5), 5% apart but inside the 1e-6 absolute floor, a surface is kept");
    edge(1e-5f, 1.2e-5f, false, "steady detail: far away, 20% apart and outside the floor, a surface is refused");

    // 5. The four texels around the previous raster position: a thin line the jitter put on the neighbouring texel last frame is found, one two
    // texels away is not, and the previous phase has its sign. The line is 12 texels long, the stale pixels are the 6 in its middle.
    auto line = [&](bool vertical, UINT prevAt, UINT curAt, float cx, float cy, float px, float py, bool kept, const char* what) {
        r.noSlots(); r.setRecord(0);
        r.phases(0, 0, 0, 0);
        r.fillZ(.01f);
        for (UINT k = 2; k <= 13; ++k) r.setZ(vertical ? prevAt : k, vertical ? k : prevAt, 1, 1, .02f);
        r.run("steady detail: the line's previous frame");
        r.fillZ(.01f);
        for (UINT k = 2; k <= 13; ++k) r.setZ(vertical ? curAt : k, vertical ? k : curAt, 1, 1, .02f);
        for (UINT k = 4; k <= 9; ++k) r.put(vertical ? curAt : k, vertical ? k : curAt, 1, .5f);
        r.phases(cx, cy, px, py);
        const Seen s = r.run(what);
        r.phases(0, 0, 0, 0);
        const unsigned stale = vertical ? s.refused(curAt, 4, 1, 6) : s.refused(4, curAt, 6, 1);
        check(stale == (kept ? 0u : 6u), what);
    };
    line(true, 5, 6, 0, 0, -.5f, 0, true,
         "steady detail: a vertical line one texel to the left last frame is in the four texels when the previous phase puts them there (-0.5): kept");
    line(true, 5, 6, 0, 0, +.5f, 0, false,
         "steady detail: ... and is not when the previous phase moves them the other way (+0.5): refused");
    line(true, 4, 6, 0, 0, -.5f, 0, false, "steady detail: a vertical line two texels away is outside the four texels: refused");
    line(true, 7, 6, 0, 0, +.5f, 0, true, "steady detail: a vertical line one texel to the right is found with the other phase (+0.5): kept");
    line(false, 5, 6, 0, 0, 0, -.5f, true, "steady detail: a horizontal line one texel above is found with the previous phase in y (-0.5): kept");
    line(false, 5, 6, 0, 0, 0, +.5f, false, "steady detail: ... and is not with the other sign (+0.5): refused");
    line(false, 4, 6, 0, 0, 0, -.5f, false, "steady detail: a horizontal line two texels away is outside the four texels: refused");

    // 6. A camera that moved: last frame's depth is read where the CAMERA says the pixel was. At depth .01 the fixture camera moves a pixel 3.2
    // texels per unit of translation, so .9375 is three pixels: the pixel at column 4 was at column 7 (and the surface there, depth .01, is a patch
    // in a farther (.02) previous frame; where the pixel IS, column 4, last frame had the farther one).
    {
        r.noSlots(); r.setRecord(0); r.phases(0, 0, 0, 0);
        r.fillZ(.02f); r.setZ(7, 4, 2, 6, .01f);
        r.run("steady detail: the camera case's previous frame");
        r.fillZ(.01f);
        for (UINT k = 4; k <= 9; ++k) r.put(4, k, 1, .5f);
        r.f.camera[5][0] = .9375f;
        const Seen moved = r.run("steady detail: the camera moved three pixels");
        r.f.camera[5][0] = 0;
        check(moved.refused(4, 4, 1, 6) == 0,
              "steady detail: a stale pixel is checked against last frame's depth where the camera term sends it (three pixels away), not where it is");
    }

    // 6b. A camera that moved along its view axis changes the depth a still surface has: the camera term's expected depth is its own. In this
    // fixture (still rotation, near .025) it is depth / (1 + dz * depth / near): .01 becomes .01 / 1.1 for dz = .25. Last frame's depth, the
    // surface's own, is compared with THAT, not with the depth the pixel has now (which is 10% off it).
    {
        r.noSlots(); r.setRecord(0); r.phases(0, 0, 0, 0);
        const float dz = .25f, expectedDepth = .01f / (1.0f + dz * .01f / .025f);
        r.fillZ(expectedDepth);
        r.run("steady detail: the view-axis case's previous frame");
        r.fillZ(.01f);
        r.staleBlock(b0, b0, bw, bw);
        r.f.camera[5][2] = dz;
        const Seen forward = r.run("steady detail: the camera moved along its view axis");
        r.f.camera[5][2] = 0;
        check(block(forward) == 0 && rest(forward) == 0,
              "steady detail: a still surface seen from a camera that moved along its view axis is kept: last frame's depth is compared with the camera term's expected depth, not with the pixel's current depth");
    }

    // 7. The 3D menu's blanket policy wins, and the check neither runs nor counts on a frame it owns.
    {
        r.noSlots(); r.setRecord(0); r.phases(0, 0, 0, 0);
        r.fillZ(.01f);
        r.run("steady detail: the menu case's previous frame");
        r.fillZ(.01f); r.setZ(b0, b0, bw, bw, .03f);   // a depth last frame did not have there: the check alone would refuse it
        r.staleBlock(b0, b0, bw, bw);
        flatMonoResolveTakeRefusalCensus();
        r.f.staticScene = true; r.f.steadyDetail = true;
        const Seen menu = r.run("steady detail: the blanket policy with the key on");
        r.f.staticScene = false;
        const auto c = flatMonoResolveTakeRefusalCensus();
        check(block(menu) == 0 && rest(menu) == 0,
              "steady detail: on a frame the 3D menu's blanket policy owns a stale pixel is kept whatever last frame's depth says");
        check(c.checked == 0 && c.skipped == 0, "steady detail: the depth check does not run, and does not count, on a frame the blanket policy owns");
    }

    // 8. A reset frame with the key on refuses everything, counts nothing, and leaves last frame's depth for the next frame.
    {
        r.noSlots(); r.setRecord(0); r.phases(0, 0, 0, 0);
        r.fillZ(.01f); r.staleBlock(b0, b0, bw, bw);
        r.f.steadyDetail = true;
        flatMonoResolveTakeRefusalCensus();
        r.f.reset = true;
        const Seen rst = r.run("steady detail: a reset frame with the key on");
        r.f.reset = false;
        const auto c = flatMonoResolveTakeRefusalCensus();
        check(rst.refusedAll() == W * H && c.checked == 0 && c.skipped == 0,
              "steady detail: a reset frame refuses every pixel and is neither a checked nor a skipped frame");
        const Seen after = r.run("steady detail: the frame after the reset");
        check(block(after) == 0, "steady detail: the frame after a reset frame has its depth to check against: the stale block is kept");
    }

    // 9. Everything else the key does not touch stays refused.
    {
        r.noSlots(); r.fillZ(.01f); r.staleBlock(b0, b0, bw, bw);
        r.put(2, 2, 2, .01f);                                  // corrupt: an even slot code
        r.put(13, 3, 4294967296.0f, .02f);                     // sentinel: the out-of-range marker
        r.z[12 * W + 12] = 0.0f; r.put(12, 12, 1, .5f);        // sentinel: a slot under a pixel with no depth (sky)
        r.put(10, 10, 1, .01f); r.setRecord(2);                // masked: its record's marker says so (the slot's depth is the pixel's: not stale)
        r.f.steadyDetail = true;
        r.run("steady detail: the classes the key does not touch (a first frame to settle the depth)");
        const Seen s = r.run("steady detail: the classes the key does not touch");
        check(block(s) == 0 && s.refused(2, 2, 1, 1) == 1 && s.refused(13, 3, 1, 1) == 1 && s.refused(12, 12, 1, 1) == 1 && s.refused(10, 10, 1, 1) == 1 &&
                  s.refusedAll() == 4,
              "steady detail: with the key on a corrupt code, the out-of-range sentinel, a sky pixel and a masked record stay refused; the stale block is kept");
        r.fillZ(.01f); r.setRecord(0);
    }

    // 10. The census splits the stale pixels: kept in a still scene, refused where the surface changes every frame.
    {
        r.noSlots(); r.fillZ(.01f); r.staleBlock(b0, b0, bw, bw);
        r.f.steadyDetail = true;
        r.run("steady detail census: settle (nothing has asked for the census yet, so no sample can be in flight from it)");
        flatMonoResolveTakeRefusalCensus();   // zero the check counters the settle frame added
        r.f.refusalCensus = true;
        for (unsigned i = 0; i < 4; ++i) r.run("steady detail census: a still scene, key on");
        const auto still = takeAll(r.context, 1);
        check(still.counts[kFlatMonoClassStale] == 0 && still.counts[kFlatMonoRefusalStaleKept] == 16 && still.refused() == 0 && still.checked == 4,
              "steady detail census: in a still scene the 16 stale pixels are stale-kept, none stale-refused, and four frames ran the check");
        for (unsigned i = 0; i < 4; ++i) {
            // The whole image changes depth every frame, so every stale pixel's four texels last frame held another surface (a block alone
            // would leave its trailing edge matched by the background beside it: the best of four, pinned in section 3).
            r.fillZ((i & 1) ? .01f : .02f);
            r.run("steady detail census: a surface that changes every frame, key on");
        }
        const auto moving = takeAll(r.context, 1);
        check(moving.counts[kFlatMonoClassStale] == 16 && moving.counts[kFlatMonoRefusalStaleKept] == 0 && moving.refused() == 16 && moving.checked == 4,
              "steady detail census: where the surface changes every frame the 16 stale pixels are stale-refused (counted as stale), none stale-kept");
        r.f.refusalCensus = false;
        r.fillZ(.01f);
    }
    r.f.steadyDetail = false;
}

// EDVR's own TAA keeps last frame's depth itself (depth[index ^ 1]): the check reads it, a kept stale pixel reaches its history, a refused one takes
// the current colour. The history is a flat 128 and the current frame a checker of 64 and 192 whose 3x3 box holds 128, so history at weight 0.9 gives
// about 122 (64) and 134 (192), and no history gives the checker itself.
inline void taaScenario(Rig& r) {
    using namespace edvr;
    flatMonoResolveReset();
    r.start(FlatMonoResolveMode::Taa);
    auto grey = [&] { std::fill(r.rgba.begin(), r.rgba.end(), 0xff808080u); };
    auto checker = [&] { for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x) r.rgba[y * W + x] = ((x + y) & 1) ? 0xffc0c0c0u : 0xff404040u; };
    r.staleBlock(4, 4, 4, 4);
    grey();
    r.run("steady detail (TAA): the reset frame");
    r.f.reset = false;
    auto pass = [&](bool steady, float blockZ, int* even, int* odd, int* free) {
        r.f.steadyDetail = steady;
        grey(); r.fillZ(.01f);
        r.run("steady detail (TAA): the history frame");
        checker(); r.fillZ(.01f); r.setZ(4, 4, 4, 4, blockZ);
        ComPtr<ID3D11ShaderResourceView> out;
        r.run("steady detail (TAA): the checker frame", true, &out);
        ComPtr<ID3D11Resource> resource; if (out) out->GetResource(resource.GetAddressOf());
        ComPtr<ID3D11Texture2D> texture2d; if (resource) resource.As(&texture2d);
        auto red = [&](UINT x, UINT y) { uint32_t v = 0; if (!texture2d || !readPixel(r.context, texture2d.Get(), &v, 4, x, y)) return -1; return int(v & 0xff); };
        *even = red(5, 5); *odd = red(5, 6); *free = red(1, 1);
    };
    int e = 0, o = 0, fr = 0;
    pass(false, .01f, &e, &o, &fr);
    check(e >= 63 && e <= 65 && o >= 191 && o <= 193, "steady detail (TAA), key off: a stale pixel is refused and takes the current colour (64 and 192)");
    check(fr >= 118 && fr <= 126, "steady detail (TAA): a pixel with no slot reaches its history through the camera term (the control)");
    pass(true, .01f, &e, &o, &fr);
    check(e >= 118 && e <= 126 && o >= 130 && o <= 138,
          "steady detail (TAA), key on, still scene: last frame's depth (the TAA's own second depth image) confirms the stale pixel, which reaches its history (122 and 134)");
    pass(true, .02f, &e, &o, &fr);
    check(e >= 63 && e <= 65 && o >= 191 && o <= 193,
          "steady detail (TAA), key on, a surface that moved in: the stale pixel is refused and takes the current colour (64 and 192)");
    r.f.steadyDetail = false;
}

// ---- the shader source, one rule flipped at a time -----------------------------------------------------------------------------------
inline void mutationChecks(Rig& r) {
    using fpgpu::Mutant;
    static const char* const kLoop =
        "[unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)best=min(best,abs(HistoryDepth.Load(int3(clamp(base+int2(x,y),0,hi),0))-expected));";
    static const Mutant mutants[] = {
        {"the tolerance is zero (only the floor stays)", "kStaleDepthRel=.01,", "kStaleDepthRel=0,", nullptr},
        {"the tolerance is 5%", "kStaleDepthRel=.01,", "kStaleDepthRel=.05,", nullptr},
        {"the tolerance is 0.5%", "kStaleDepthRel=.01,", "kStaleDepthRel=.005,", nullptr},
        {"the floor is zero", "kStaleDepthFloor=1e-6;", "kStaleDepthFloor=0;", nullptr},
        {"the floor is 1e-3", "kStaleDepthFloor=1e-6;", "kStaleDepthFloor=1e-3;", nullptr},
        {"the tolerance is the smaller of the floor and the relative term", "max(kStaleDepthFloor,expected*kStaleDepthRel)", "min(kStaleDepthFloor,expected*kStaleDepthRel)", nullptr},
        {"there is no depth check", "if(valid && kind==3)valid=historyDepthMatches(prev,expected);", "if(valid && kind==3)valid=true;", nullptr},
        {"the depth check is inverted", "if(valid && kind==3)valid=historyDepthMatches(prev,expected);",
         "if(valid && kind==3)valid=!historyDepthMatches(prev,expected);", nullptr},
        {"the check looks where the pixel is, not where the camera sends it", "valid=historyDepthMatches(prev,expected);",
         "valid=historyDepthMatches(rawUv,expected);", nullptr},
        {"a stale pixel the check refuses is counted as a range refusal", "if(valid && kind==3)valid=historyDepthMatches(prev,expected);",
         "if(valid && kind==3){valid=historyDepthMatches(prev,expected);if(!valid)cls=kClassRange;}", nullptr},
        {"the previous phase is ignored", "prev*float2(size.xy)+jitter.zw-.5", "prev*float2(size.xy)-.5", nullptr},
        {"the previous phase has the wrong sign", "prev*float2(size.xy)+jitter.zw-.5", "prev*float2(size.xy)-jitter.zw-.5", nullptr},
        {"the current phase is used for the previous one", "prev*float2(size.xy)+jitter.zw-.5", "prev*float2(size.xy)+jitter.xy-.5", nullptr},
        {"the half texel is dropped", "prev*float2(size.xy)+jitter.zw-.5", "prev*float2(size.xy)+jitter.zw", nullptr},
        {"one texel, not four", kLoop,
         "[unroll]for(int y=0;y<1;++y)[unroll]for(int x=0;x<1;++x)best=min(best,abs(HistoryDepth.Load(int3(clamp(base+int2(x,y),0,hi),0))-expected));", nullptr},
        {"sixteen texels, not four", kLoop,
         "[unroll]for(int y=-1;y<3;++y)[unroll]for(int x=-1;x<3;++x)best=min(best,abs(HistoryDepth.Load(int3(clamp(base+int2(x,y),0,hi),0))-expected));", nullptr},
        {"the check reads this frame's depth, not last frame's", kLoop,
         "[unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)best=min(best,abs(SceneDepth.Load(int3(clamp(base+int2(x,y),0,hi),0))-expected));", nullptr},
        {"the key is the blanket policy", "return flags.w==1?0:(flags.w==2?3:2);", "return flags.w!=0?0:2;", nullptr},
        {"the key refuses a stale slot", "return flags.w==1?0:(flags.w==2?3:2);", "return flags.w==1?0:2;", nullptr},
        {"the sky is kept with the key on", "if(!(depth>0) || es.x>=4294967296.0){cls=kClassSentinel;return 2;}",
         "if(!(depth>0) || es.x>=4294967296.0){cls=kClassSentinel;return flags.w==2?0:2;}", nullptr},
        {"a corrupt code is kept with the key on", "if(float(code)!=es.x || (code&1)==0){cls=kClassCorrupt;return 2;}",
         "if(float(code)!=es.x || (code&1)==0){cls=kClassCorrupt;return flags.w==2?0:2;}", nullptr},
        {"the check compares with the pixel's own depth, not the camera term's expected depth", "valid=historyDepthMatches(prev,expected);",
         "valid=historyDepthMatches(prev,depth);", nullptr},
    };
    const std::string shipped = edvr::kFlatMonoShaderSource;
    int caught = 0, equivalent = 0;
    auto runMutated = [&](const std::string& hlsl, int* failed, std::string* first) {
        std::vector<unsigned char> bytes;
        if (!fpgpu::compilePrep(hlsl, bytes)) return false;
        ComPtr<ID3D11ComputeShader> probe;
        if (FAILED(r.device->CreateComputeShader(bytes.data(), bytes.size(), nullptr, probe.GetAddressOf()))) return false;
        edvr::flatMonoResolveTestPrepBytecode(bytes.data(), bytes.size());
        edvr::flatMonoResolveReset();
        *failed = 0; mutationFailures = failed; mutationFirst.clear();
        scenario(r, edvr::FlatMonoResolveMode::Dlss);
        mutationFailures = nullptr; if (first) *first = mutationFirst;
        return true;
    };
    {   // The control: the same source, compiled here and run through the same seam, is the shipped prep and passes.
        std::vector<unsigned char> bytes;
        const bool compiled = fpgpu::compilePrep(shipped, bytes);
        check(compiled, "steady detail mutations: the shipped prep source compiles here");
        const bool sameBytes = compiled && bytes.size() == sizeof(edvr::kFlatMonoPrepBytecode) &&
                               !std::memcmp(bytes.data(), edvr::kFlatMonoPrepBytecode, bytes.size());
        std::printf("flat mono resolve: steady-detail mutation harness: the shipped prep source compiled here is %s the generated bytecode (%zu bytes)\n",
                    sameBytes ? "byte-for-byte" : "NOT byte-for-byte", bytes.size());
        int failed = 0; std::string first;
        const bool ran = runMutated(shipped, &failed, &first);
        check(ran && failed == 0, "steady detail mutations: control: the unmutated prep, compiled here and run through the test seam, passes the whole scenario");
    }
    for (const Mutant& m : mutants) {
        bool once = false;
        const std::string hlsl = fpgpu::replaceOnce(shipped, m.from, m.to, &once);
        char what[256];
        std::snprintf(what, sizeof(what), "steady detail mutations: \"%s\": its anchor is in the shader source exactly once (a moved anchor tests nothing)", m.name);
        check(once, what);
        int failed = 0; std::string first;
        const bool ran = once && runMutated(hlsl, &failed, &first);
        std::snprintf(what, sizeof(what), "steady detail mutations: \"%s\" compiles and makes a compute shader", m.name);
        check(ran, what);
        if (!ran) continue;
        if (m.survivesBecause) {
            ++equivalent;
            std::printf("flat mono resolve: steady-detail mutation \"%s\": %s; %d checks fail (equivalent by construction: %s)\n",
                        m.name, failed ? "caught" : "survives", failed, m.survivesBecause);
            continue;
        }
        std::snprintf(what, sizeof(what), "steady detail mutations: \"%s\" is caught by the scenario", m.name);
        check(failed > 0, what);
        if (failed > 0) ++caught;
        std::printf("flat mono resolve: steady-detail mutation \"%s\": %d checks fail; first: %s\n", m.name, failed, first.c_str());
    }
    edvr::flatMonoResolveTestPrepBytecode(nullptr, 0);
    edvr::flatMonoResolveReset();
    std::printf("flat mono resolve: steady-detail mutations: %d of %zu caught by the scenario, %d equivalent by construction\n",
                caught, sizeof(mutants) / sizeof(mutants[0]) - size_t(equivalent), equivalent);
    check(size_t(caught) + size_t(equivalent) == sizeof(mutants) / sizeof(mutants[0]),
          "steady detail mutations: every mutation was either caught or named equivalent");
}

// ---- the contract on the shaders themselves --------------------------------------------------------------------------------------------
inline void sourceTests() {
    using namespace edvr;
    const std::string source = kFlatMonoShaderSource;
    // The HLSL's tolerance is the header's.
    const size_t rel = source.find("kStaleDepthRel=");
    const size_t floorAt = source.find("kStaleDepthFloor=");
    check(rel != std::string::npos && floorAt != std::string::npos &&
              std::strtod(source.c_str() + rel + std::strlen("kStaleDepthRel="), nullptr) == kFlatMonoStaleDepthRelative &&
              std::strtod(source.c_str() + floorAt + std::strlen("kStaleDepthFloor="), nullptr) == kFlatMonoStaleDepthFloor,
          "steady detail: the HLSL's tolerance (kStaleDepthRel, kStaleDepthFloor) is flat_mono_refusal.h's: 1% and 1e-6, the resolver's own TAA's");
    // The resolver's own TAA applies the same rule: its own 1% (floor 1e-6) literal is gone, taa() asks the shared check (section 104;
    // flat_taa_history_depth_gpu_tests.h holds what it does).
    check(source.find("abs(was-predicted)") == std::string::npos &&
              source.find("if(historyDepthMatches(previous,ExpectedDepth.Load(int3(q,0))))weight=.9;") != std::string::npos,
          "steady detail: the resolver's own TAA applies the same check, with the same 1% (floor 1e-6) tolerance, to its history depth: no literal of its own");
    check(source.find("flags.w==1?0:(flags.w==2?3:2)") != std::string::npos,
          "steady detail: the stale-slot policy has three values (0 refuse, 1 the menu's blanket, 2 the depth-checked camera term)");
}
}  // namespace steadygpu

inline void steadyDepthGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using edvr::FlatMonoResolveMode;
    steadygpu::sourceTests();
    steadygpu::Rig rig(device, context);
    // 1. The scenario on the shipped prep, for real, through DLSS's stub and again through FSR's.
    steadygpu::scenario(rig, FlatMonoResolveMode::Dlss);
    steadygpu::scenario(rig, FlatMonoResolveMode::Fsr);
    // 2. EDVR's own TAA, which keeps last frame's depth itself.
    steadygpu::taaScenario(rig);
    edvr::flatMonoResolveReset();
    // 3. The same scenario against the prep with one rule flipped at a time: it must fail each time.
    steadygpu::mutationChecks(rig);
    edvr::flatMonoResolveReset();
}
