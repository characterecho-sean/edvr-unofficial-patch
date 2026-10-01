// The first-person ("weapon") fold-in of the resolver's prep kernel on WARP (docs/design-flat-temporal-aa-2026-09-23.md section 82).
// The VR weapon-motion module rebuilds motion for first-person draws from their own animated vertices (an RGBA16F map, the size of
// the render: xy previous minus current position in render pixels, z depth, w 1 valid / 2 new / 0 uncovered), and the game's depth
// texture carries a first-person stencil bit (0x10). FlatMonoResolveFrame::firstPersonMotion and firstPersonStencil hand both to the
// prep; where the bit is set the pixel takes the map's motion if the map is valid and REJECTS its history if not, and never takes the
// engine or camera term; every other pixel is treated exactly as without the inputs.
//
// What the scenario pins, on the shipped prep and then, as MUTATION CHECKS, on the prep with one rule flipped at a time (the
// mutated source is compiled here, handed to the resolver through flatMonoResolveTestPrepBytecode, and the scenario must FAIL; a
// scenario that cannot fail proves nothing): an attached pixel with a valid map takes the map's motion exactly; w other than 1, a
// depth that disagrees beyond max(|depth| * 0.0005, 3e-8) (the boundary, in both directions and at two depths, because the rule is
// relative), a previous position outside [0, size] (the frame's edge and size themselves are inside), and non-finite texels reject,
// with no motion; a stencil value with bit 0x10 clear, however many others are set, attaches nothing; unattached pixels equal a run
// without the inputs exactly, although the map under them holds motion; a reset frame rejects every attached pixel; with a moving
// camera an attached pixel takes neither the camera term nor the engine record of a joined pixel, and an invalid one is rejected,
// never given the camera term; and, through EDVR's own TAA (the only consumer of `expected`), a valid attached pixel reaches its
// history while a rejected one takes the current colour. The contract around it, on the shipped prep only: absent inputs, one of the
// pair, a pair the resolver refuses, and a pair with no stencil bit set anywhere each leave the prep's whole output bit for bit what
// a run without them gives; a wrong-size or wrong-format map, a wrong-format, wrong-dimension or wrong-size stencil view is refused,
// counted, named and logged once, and never refuses the frame; accepted frames are counted and logged once; and the game's whole
// pipeline, including the compute slots t9 and t10 the pair is bound to, comes back untouched.
#pragma once
#include <d3dcompiler.h>
#include "temporal_shader_bytecode.h"   // kEngineMotionCoreHlsl and kFlatMonoPrepBytecode, generated into build\gen
#include "../../src/d3d11/flat_mono_shader_source.h"

namespace edvr {
// Test-only (flat_mono_resolve.cpp): the prep the next initialisation makes is this bytecode instead of the shipped one.
void flatMonoResolveTestPrepBytecode(const void* bytes, size_t size);
}

namespace fpgpu {

// A float as an IEEE half, rounded to nearest even; the map is RGBA16F.
inline uint16_t toHalf(float f) {
    uint32_t x; std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t mant = x & 0x007FFFFFu;
    int32_t exp = int32_t((x >> 23) & 0xFF);
    if (exp == 0xFF) return uint16_t(sign | 0x7C00u | (mant ? 0x0200u : 0u));
    exp = exp - 127 + 15;
    if (exp >= 31) return uint16_t(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return uint16_t(sign);
        mant |= 0x00800000u;
        const uint32_t shift = uint32_t(14 - exp);
        uint32_t h = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1u), mid = 1u << (shift - 1);
        if (rem > mid || (rem == mid && (h & 1u))) ++h;
        return uint16_t(sign | h);
    }
    uint32_t h = (uint32_t(exp) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;
    return uint16_t(sign | h);
}
constexpr uint16_t kHalfNaN = 0x7E00, kHalfInf = 0x7C00, kHalfNegInf = 0xFC00;

struct Half4 { uint16_t x = 0, y = 0, z = 0, w = 0; };
inline Half4 mapOf(float dx, float dy, float z, float w = 1.0f) { return Half4{toHalf(dx), toHalf(dy), toHalf(z), toHalf(w)}; }

// What a fixture texel is, and so what the prep must make of it.
enum class Cat : uint8_t { Plain, OtherBits, Valid, WrongW, Depth, Outside, NonFinite };
struct Spec {
    uint32_t stencil = 0x01;
    float depth = .01f;
    Half4 map{};
    Cat cat = Cat::Plain;
    float mx = 0, my = 0;   // the motion a Valid texel must come out with
};
constexpr UINT W = 16, H = 16;
constexpr float zOk = .01f;   // the fixture's ordinary depth

inline std::vector<Spec> makeSpecs(std::vector<std::pair<UINT, UINT>>* joined) {
    std::vector<Spec> s(W * H);
    auto at = [&](UINT x, UINT y) -> Spec& { return s[y * W + x]; };
    // Everything starts unattached, with a map under it that looks perfectly valid and moves seven pixels: any pixel that
    // takes it by mistake is wrong by far more than a rounding.
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x) { Spec& t = at(x, y); t.stencil = ((x + y) & 1) ? 0x01 : 0x00; t.map = mapOf(7, 7, zOk); }
    auto valid = [&](UINT x, UINT y, float dx, float dy, uint32_t stencil, float depth = zOk, uint16_t zBits = 0) {
        Spec& t = at(x, y); t.stencil = stencil; t.depth = depth; t.map = mapOf(dx, dy, depth); if (zBits) t.map.z = zBits;
        t.cat = Cat::Valid; t.mx = dx; t.my = dy;
    };
    auto rejected = [&](UINT x, UINT y, Half4 map, Cat cat, float depth = zOk) {
        Spec& t = at(x, y); t.stencil = 0x10; t.depth = depth; t.map = map; t.cat = cat;
    };
    auto other = [&](UINT x, UINT y, uint32_t stencil) { at(x, y).stencil = stencil; at(x, y).cat = Cat::OtherBits; };
    // The attached block: bit 0x10, a valid map of (0.5, 0.25).
    for (UINT y = 2; y <= 13; ++y)
        for (UINT x = 2; x <= 13; ++x) valid(x, y, .5f, .25f, 0x10);
    valid(5, 5, 2.5f, -1.25f, 0x10);
    valid(6, 5, -1.0f, .5f, 0x30);          // other bits beside 0x10
    valid(7, 5, .75f, -.5f, 0xFF);          // every bit
    valid(8, 5, 1.5f, -.75f, 0x10);         // and this one holds a joined rig record: its engine motion must not be used
    if (joined) joined->push_back({8, 5});
    rejected(5, 6, mapOf(2.5f, -1.25f, zOk, 2.0f), Cat::WrongW);                  // new
    rejected(6, 6, mapOf(2.5f, -1.25f, zOk, 0.0f), Cat::WrongW);                  // uncovered
    rejected(7, 6, mapOf(.5f, .25f, zOk * 1.01f), Cat::Depth);                    // one percent too far
    rejected(8, 6, mapOf(.5f, .25f, .0099f), Cat::Depth);                         // and one percent too near
    rejected(5, 7, mapOf(-8, 0, zOk), Cat::Outside);                              // previous x = 5.5 - 8
    rejected(6, 7, mapOf(0, 10, zOk), Cat::Outside);                              // previous y = 7.5 + 10
    rejected(7, 7, mapOf(11, 0, zOk), Cat::Outside);                              // previous x = 7.5 + 11
    rejected(8, 7, mapOf(0, -8, zOk), Cat::Outside);                              // previous y = 7.5 - 8
    rejected(5, 8, Half4{kHalfNaN, 0, toHalf(zOk), toHalf(1)}, Cat::NonFinite);                 // NaN motion
    rejected(6, 8, Half4{toHalf(.5f), toHalf(.25f), kHalfInf, toHalf(1)}, Cat::NonFinite);      // infinite depth
    rejected(7, 8, Half4{0, kHalfNegInf, toHalf(zOk), toHalf(1)}, Cat::NonFinite);              // infinite motion
    // The frame's edge: a previous position of exactly 0 or exactly the size is inside; a quarter of a pixel further is not.
    valid(0, 9, -.5f, 0, 0x10); valid(15, 9, .5f, 0, 0x10); valid(9, 0, 0, -.5f, 0x10); valid(9, 15, 0, .5f, 0x10);
    rejected(0, 10, mapOf(-.75f, 0, zOk), Cat::Outside); rejected(15, 10, mapOf(.75f, 0, zOk), Cat::Outside);
    rejected(10, 0, mapOf(0, -.75f, zOk), Cat::Outside); rejected(10, 15, mapOf(0, .75f, zOk), Cat::Outside);
    // The tolerance is relative: at depth 0.5 the half just below (0x37FF, a gap of 2.44e-4) is inside 0.5 * 0.0005 = 2.5e-4 and the half
    // just above (0x3801, 4.88e-4) is not; at depth 0.01 the half just below it (0x211E, 5.49e-6) is already outside 0.01 * 0.0005 = 5e-6.
    valid(3, 11, 1.0f, -1.0f, 0x10, .5f);
    valid(4, 11, 1.0f, -1.0f, 0x10, .5f, 0x37FF);
    rejected(5, 11, Half4{toHalf(1.0f), toHalf(-1.0f), 0x3801, toHalf(1)}, Cat::Depth, .5f);
    rejected(6, 11, Half4{toHalf(.5f), toHalf(.25f), 0x211E, toHalf(1)}, Cat::Depth);
    // Stencil values around the bit that are not it: nothing attaches, whatever the map beneath says.
    other(9, 9, 0x0F); other(10, 9, 0x20); other(11, 9, 0x08); other(12, 9, 0xEF); other(9, 10, 0x40); other(10, 10, 0x80); other(11, 10, 0x00);
    return s;
}

// Every resource the scenarios need, at the render size, and the planes that fill them.
struct Fixture {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ResolveFixture fx;
    ComPtr<ID3D11Texture2D> ds, map, color;
    ComPtr<ID3D11ShaderResourceView> depthView, stencilView, mapView, colorView;
    std::vector<Spec> specs;
    std::vector<std::pair<UINT, UINT>> joined;
    uint32_t inputFrames = 0;   // resolves made with both halves of the pair set, for the counter check

    Fixture(ID3D11Device* d, ID3D11DeviceContext* c) : device(d), context(c), fx(d, c) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = W; td.Height = H; td.MipLevels = td.ArraySize = 1; td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32G8X24_TYPELESS; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        check(SUCCEEDED(d->CreateTexture2D(&td, nullptr, ds.GetAddressOf())), "first person: the R32G8X24_TYPELESS depth texture");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        sv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        check(SUCCEEDED(d->CreateShaderResourceView(ds.Get(), &sv, depthView.GetAddressOf())), "first person: the depth plane view");
        sv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        check(SUCCEEDED(d->CreateShaderResourceView(ds.Get(), &sv, stencilView.GetAddressOf())), "first person: the stencil plane view");
        map = texture(d, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        mapView = view(d, map.Get());
        color = texture(d, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        colorView = view(d, color.Get());
        specs = makeSpecs(&joined);
        upload();
    }
    // The planes, from `specs`: depth and stencil into the one texture (8 bytes a texel: the float, then the stencil in the low
    // byte of the next word), the map into its own.
    void upload() {
        struct Texel { float depth; uint32_t stencil; };
        std::vector<Texel> dsData(W * H); std::vector<Half4> mapData(W * H);
        for (size_t i = 0; i < specs.size(); ++i) { dsData[i] = {specs[i].depth, specs[i].stencil}; mapData[i] = specs[i].map; }
        context->UpdateSubresource(ds.Get(), 0, nullptr, dsData.data(), W * 8, 0);
        context->UpdateSubresource(map.Get(), 0, nullptr, mapData.data(), W * 8, 0);
    }
    void fillColor(uint32_t (*colorOf)(UINT x, UINT y)) {
        std::vector<uint32_t> texels(W * H);
        for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x) texels[y * W + x] = colorOf(x, y);
        context->UpdateSubresource(color.Get(), 0, nullptr, texels.data(), W * 4, 0);
    }
    // The joined rig record and its engine slot: a pixel that names one takes the record's exact motion -- unless it is attached.
    void joinEngineRecords() {
        constexpr uint32_t kStamp = 77;
        uint32_t record[84]{};
        auto bitsOf = [](float v) { uint32_t b; std::memcpy(&b, &v, 4); return b; };
        record[1] = record[77] = bitsOf(1); record[2] = record[78] = 0x7fff7fff; record[3] = record[79] = 0xfffe7fff;
        record[4] = bitsOf(0); record[5] = bitsOf(0); record[6] = bitsOf(2.5f);
        record[73] = bitsOf(-.3125f); record[74] = bitsOf(0); record[75] = bitsOf(2.5f);
        edvr::engine_velocity_emit::Pose np{{record[4], record[5], record[6], record[2], record[3]}};
        edvr::engine_velocity_emit::Pose pp{{record[73], record[74], record[75], record[78], record[79]}};
        record[72] = 0x7FC0ED01u ^ edvr::engine_velocity_emit::markerHash(np, pp, kStamp);
        context->UpdateSubresource(fx.pool.Get(), 0, nullptr, record, 0, 0);
        std::vector<float> slots(W * H * 2);
        for (size_t i = 0; i < slots.size(); i += 2) { slots[i] = -1; slots[i + 1] = .01f; }
        for (const auto& p : joined) { slots[(p.second * W + p.first) * 2] = 1; slots[(p.second * W + p.first) * 2 + 1] = .01f; }
        context->UpdateSubresource(fx.slotTexture.Get(), 0, nullptr, slots.data(), W * 8, 0);
    }
    edvr::FlatMonoResolveFrame baseFrame(edvr::FlatMonoResolveMode mode) {
        edvr::FlatMonoResolveFrame f{};
        f.color = colorView.Get(); f.depth = depthView.Get(); f.renderWidth = W; f.renderHeight = H; f.outputWidth = W; f.outputHeight = H;
        f.deltaMs = 16; camera(f.camera); camera(f.previousCamera); fx.engine(f); f.mode = mode;
        return f;
    }
};

// What the stub backend was handed for one frame: the prep's whole output, as the SDK sees it.
struct Seen { std::vector<float> motion; std::vector<unsigned char> mask; uint64_t hash = 0; bool reset = false; bool ok = false; };

// One resolve with the game's pipeline around it. The output view of the copy route is handed back through `out` when asked for.
inline Seen resolveFrame(Fixture& fx, const edvr::FlatMonoResolveFrame& frame, bool wanted = true,
                         ComPtr<ID3D11ShaderResourceView>* out = nullptr) {
    fx.fx.bindOriginal();
    ComPtr<ID3D11ShaderResourceView> resolved; const char* reason = nullptr;
    const bool ok = edvr::flatMonoResolve(fx.device, fx.context, frame, resolved.GetAddressOf(), &reason);
    if (ok != wanted) std::printf("info: first-person resolver reason %s\n", reason ? reason : "none");
    check(ok == wanted, "first person: resolver result");
    check(fx.fx.restored(), "first person: the game's whole pipeline, compute slots t9 and t10 included, restored on every exit");
    if (frame.firstPersonMotion && frame.firstPersonStencil) ++fx.inputFrames;
    Seen s; s.ok = ok; s.reset = backendReset; s.hash = observedMotionHash; s.motion = observedMotionAll; s.mask = observedMaskAll;
    if (out) *out = resolved;
    return s;
}

// The scenario's judgement of one frame. `bare` is the same frame without the inputs; `reset` says every texel must reject.
inline void judge(const std::vector<Spec>& specs, const Seen& got, const Seen* bare, bool reset, const char* tag) {
    char what[256];
    const bool sized = got.motion.size() == size_t(W) * H * 2 && got.mask.size() == size_t(W) * H &&
                       (reset || (bare && bare->motion.size() == got.motion.size() && bare->mask.size() == got.mask.size()));
    std::snprintf(what, sizeof(what), "first person (%s): the prep's whole output reached the backend", tag);
    check(sized, what);
    if (!sized) return;
    bool okReset = true, okValid = true, okWrongW = true, okDepth = true, okOutside = true, okNonFinite = true, okOther = true, okPlain = true;
    for (size_t t = 0; t < specs.size(); ++t) {
        const Spec& s = specs[t];
        const float mx = got.motion[2 * t], my = got.motion[2 * t + 1];
        const bool rej = got.mask[t] != 0, rejectedNoMotion = rej && mx == 0 && my == 0;
        if (reset) { if (!rejectedNoMotion) okReset = false; continue; }
        switch (s.cat) {
            case Cat::Valid: if (!(!rej && mx == s.mx && my == s.my)) okValid = false; break;
            case Cat::WrongW: if (!rejectedNoMotion) okWrongW = false; break;
            case Cat::Depth: if (!rejectedNoMotion) okDepth = false; break;
            case Cat::Outside: if (!rejectedNoMotion) okOutside = false; break;
            case Cat::NonFinite: if (!rejectedNoMotion) okNonFinite = false; break;
            case Cat::Plain: case Cat::OtherBits: {
                const bool same = rej == (bare->mask[t] != 0) && mx == bare->motion[2 * t] && my == bare->motion[2 * t + 1];
                if (!same) (s.cat == Cat::Plain ? okPlain : okOther) = false;
                break;
            }
        }
    }
    auto report = [&](bool ok, const char* text) { std::snprintf(what, sizeof(what), "first person (%s): %s", tag, text); check(ok, what); };
    if (reset) {
        report(okReset, "every pixel is rejected with no motion, attached ones included: a reset frame has no history, map or not");
        return;
    }
    report(okValid, "an attached pixel with a valid map takes the map's motion exactly and is not rejected (bits beside 0x10, the frame's edge, both depths, a joined engine record)");
    report(okWrongW, "a map texel whose w is not 1 (new, uncovered) rejects the attached pixel, with no motion");
    report(okDepth, "a map depth that disagrees beyond max(|depth| * 0.0005, 3e-8) rejects it, above and below, at two depths");
    report(okOutside, "a previous position outside [0, size] rejects it, on every side, a quarter pixel past the edge included");
    report(okNonFinite, "a non-finite map texel rejects it");
    report(okOther, "a stencil value without bit 0x10, whatever else it holds, attaches nothing: the pixel equals a run without the inputs");
    report(okPlain, "every other pixel equals a run without the inputs exactly, although the map under it holds motion");
}

// EDVR's own TAA is the only consumer of the prep's `expected` depth: an attached pixel must reach its history when its map is valid
// (expected is its depth) and take the current colour when it is not (rejected). The history is a flat 128, the current frame a
// checker of 64 and 192 whose 3x3 box holds 128, so history at weight 0.9 gives 122 (64) and 134 (192) and no history gives the
// checker itself.
inline uint32_t grey128(UINT, UINT) { return 0xff808080u; }
inline uint32_t checker(UINT x, UINT y) { return ((x + y) & 1) ? 0xffc0c0c0u : 0xff404040u; }
inline void taaScenario(Fixture& fx) {
    using edvr::FlatMonoResolveMode;
    edvr::flatMonoResolveReset();
    std::vector<Spec> specs(W * H);
    for (Spec& t : specs) { t.stencil = 0; t.map = mapOf(7, 7, .01f); }
    for (UINT y = 4; y <= 11; ++y)
        for (UINT x = 4; x <= 11; ++x) { Spec& t = specs[y * W + x]; t.stencil = 0x10; t.map = mapOf(0, 0, .01f); }
    specs[6 * W + 6].map = mapOf(0, 0, .01f, 2.0f);   // a new mesh: rejected
    fx.specs = specs; fx.upload();
    auto frame = fx.baseFrame(FlatMonoResolveMode::Taa);
    frame.firstPersonMotion = fx.mapView.Get(); frame.firstPersonStencil = fx.stencilView.Get();
    frame.frame = 40000; frame.reset = true;
    expectedJx = expectedJy = 0;
    fx.fillColor(grey128);
    resolveFrame(fx, frame);
    fx.fillColor(checker);
    ++frame.frame; frame.reset = false;
    ComPtr<ID3D11ShaderResourceView> out;
    resolveFrame(fx, frame, true, &out);
    ComPtr<ID3D11Resource> resource; if (out) out->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> outTexture; if (resource) resource.As(&outTexture);
    auto red = [&](UINT x, UINT y) { uint32_t v = 0; if (!outTexture || !readPixel(fx.context, outTexture.Get(), &v, 4, x, y)) return -1; return int(v & 0xff); };
    const int valid64 = red(5, 5), valid192 = red(5, 6), rejected64 = red(6, 6), attachedEngine = red(8, 5), unattached = red(1, 1);
    if (!mutationFailures)
        std::printf("flat mono resolve: first-person TAA: attached valid %d and %d (history weighted: ~122 and ~134; current: 64 and 192), attached joined %d, "
                    "attached rejected %d (current: 64), unattached %d\n", valid64, valid192, attachedEngine, rejected64, unattached);
    // (8, 5) is the joined pixel and sits on an odd parity: its current colour is 192.
    check(valid64 >= 118 && valid64 <= 126 && valid192 >= 130 && valid192 <= 138 && attachedEngine >= 130 && attachedEngine <= 138,
          "first person (TAA): an attached pixel with a valid map reaches its history: expected depth is the pixel's own depth (122 and 134, not the current 64 and 192)");
    check(rejected64 >= 63 && rejected64 <= 65,
          "first person (TAA): an attached pixel with an invalid map takes the current colour exactly, no history");
    check(unattached >= 118 && unattached <= 126,
          "first person (TAA): a pixel outside the stencil bit is untouched by the pair and reaches its history through the camera term");
    edvr::flatMonoResolveReset();
}

// The whole scenario, against whichever prep the resolver was last initialised with. Nothing in it is specific to the shipped one.
inline void scenario(Fixture& fx) {
    using edvr::FlatMonoResolveFrame;
    using edvr::FlatMonoResolveMode;
    edvr::flatMonoResolveReset();
    fx.specs = makeSpecs(nullptr); fx.upload(); fx.joinEngineRecords();
    fx.fillColor(grey128);
    FlatMonoResolveFrame base = fx.baseFrame(FlatMonoResolveMode::Dlss);
    base.frame = 30000;
    expectedJx = expectedJy = 0;
    auto frameOf = [&](bool reset, bool inputs, float cameraX) {
        FlatMonoResolveFrame f = base; f.reset = reset; f.frame = ++base.frame; f.camera[5][0] = cameraX;
        if (inputs) { f.firstPersonMotion = fx.mapView.Get(); f.firstPersonStencil = fx.stencilView.Get(); }
        return f;
    };
    // A reset frame first: every texel rejects, attached or not.
    const Seen resetFrame = resolveFrame(fx, frameOf(true, true, 0));
    judge(fx.specs, resetFrame, nullptr, true, "a reset frame");
    // A still camera: the camera term is zero motion, valid, so an attached pixel that takes it instead of the map is at 0 not at the map's.
    const Seen still = resolveFrame(fx, frameOf(false, true, 0));
    const Seen stillBare = resolveFrame(fx, frameOf(false, false, 0));
    judge(fx.specs, still, &stillBare, false, "still camera");
    // A moving camera: the camera term is one render pixel of motion, valid, so an attached pixel that takes it, or takes it on top
    // of the map, or falls back to it when the map is invalid, is at a motion the map never said.
    const Seen moving = resolveFrame(fx, frameOf(false, true, .3125f));
    const Seen movingBare = resolveFrame(fx, frameOf(false, false, .3125f));
    judge(fx.specs, moving, &movingBare, false, "moving camera");
    taaScenario(fx);
}

// ---- the shader source, one rule flipped at a time -----------------------------------------------------------------------------------
struct Mutant {
    const char* name;
    const char* from;
    const char* to;
    const char* survivesBecause;   // null: the scenario must catch it
};
inline std::string replaceOnce(const std::string& text, const char* from, const char* to, bool* foundOnce) {
    const size_t at = text.find(from);
    *foundOnce = at != std::string::npos && text.find(from, at + 1) == std::string::npos;
    if (!*foundOnce) return text;
    std::string out = text;
    out.replace(at, std::strlen(from), to);
    return out;
}
inline bool compilePrep(const std::string& hlsl, std::vector<unsigned char>& bytes) {
    const std::string full = std::string(edvr::kEngineMotionCoreHlsl) + hlsl;
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(full.c_str(), full.size(), "flat_mono_prep_cs", nullptr, nullptr, "prep", "cs_5_0", 0, 0,
                                  code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr) || !code) {
        std::printf("info: prep compile failed: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
        return false;
    }
    const auto* p = static_cast<const unsigned char*>(code->GetBufferPointer());
    bytes.assign(p, p + code->GetBufferSize());
    return true;
}

inline void mutationChecks(Fixture& fx) {
    static const Mutant mutants[] = {
        {"the stencil bit is 0x08, not 0x10", "FirstPersonStencil.Load(int3(q,0)).y&16)!=0", "FirstPersonStencil.Load(int3(q,0)).y&8)!=0", nullptr},
        {"any stencil value attaches", "FirstPersonStencil.Load(int3(q,0)).y&16)!=0", "FirstPersonStencil.Load(int3(q,0)).y&255)!=0", nullptr},
        {"the stencil test is inverted", "FirstPersonStencil.Load(int3(q,0)).y&16)!=0", "FirstPersonStencil.Load(int3(q,0)).y&16)==0", nullptr},
        {"the first-person flag is inverted", "route.z!=0 && (FirstPersonStencil", "route.z==0 && (FirstPersonStencil", nullptr},
        {"a map texel with w != 1 is accepted", "m.w==1 && ", "", nullptr},
        {"a map whose depth disagrees is accepted", "abs(m.z-depth)<=max(abs(depth)*.0005,3e-8) &&", "true &&", nullptr},
        {"the depth tolerance is exact equality", "max(abs(depth)*.0005,3e-8)", "0", nullptr},
        {"the depth tolerance is twice as wide", ".0005,3e-8", ".001,3e-8", nullptr},
        {"the depth tolerance is half as wide", ".0005,3e-8", ".00025,3e-8", nullptr},
        {"a previous position outside the frame is accepted", "all(prevPx>=0) && all(prevPx<=float2(size.xy))", "true", nullptr},
        {"the lower bound of the frame is exclusive", "all(prevPx>=0)", "all(prevPx>0)", nullptr},
        {"the upper bound of the frame is exclusive", "all(prevPx<=float2(size.xy))", "all(prevPx<float2(size.xy))", nullptr},
        {"the previous position drops the half texel", "float2 prevPx=float2(q)+.5+m.xy;", "float2 prevPx=float2(q)+m.xy;", nullptr},
        {"a reset frame takes the map", "flags.x==0 && m.w==1", "m.w==1", nullptr},
        {"the map's motion is negated", "motion=valid?m.xy:0;", "motion=valid?-m.xy:0;", nullptr},
        {"the map's axes are swapped", "motion=valid?m.xy:0;", "motion=valid?m.yx:0;", nullptr},
        {"an invalid map texel is not rejected", "motion=valid?m.xy:0; reject=valid?0:1;", "motion=valid?m.xy:0; reject=0;", nullptr},
        {"an attached pixel also takes the camera and engine term",
         "} else if(flags.x==0 && isfinite(depth) && depth>=0 && depth<=1) {", "}\n    if(flags.x==0 && isfinite(depth) && depth>=0 && depth<=1) {", nullptr},
        {"an invalid map texel leaves the pixel to the camera term",
         "bool attached=route.z!=0 && (FirstPersonStencil.Load(int3(q,0)).y&16)!=0;",
         "bool attached=route.z!=0 && (FirstPersonStencil.Load(int3(q,0)).y&16)!=0 && FirstPersonMotion.Load(int3(q,0)).w==1;", nullptr},
        {"the expected depth is not written for a first-person pixel", "expected=valid?depth:0;", "expected=0;", nullptr},
        // Only a moving camera tells this one from the map alone: with a still camera the camera term it adds is zero.
        {"an attached pixel with a valid map adds the camera term to it", "motion=valid?m.xy:0; reject=valid?0:1;",
         "motion=valid?m.xy:0; { float4 cb; if(valid && cameraBefore(rawUv,depth,cb)) motion+=(cb.xy/cb.w*float2(.5,-.5)+.5-rawUv)*float2(size.xy); } reject=valid?0:1;", nullptr},
        {"the finiteness term is dropped", "all(isfinite(m)) && ", "",
         "the depth and frame-bound terms already refuse every NaN and infinity a half can hold (a NaN or infinite depth or motion fails abs(m.z-depth) <= tolerance or the bounds), so the term is a guard the scenario cannot see"},
        {"the first-person flag is ignored", "route.z!=0 && (FirstPersonStencil", "(FirstPersonStencil",
         "the resolver binds null for both views whenever it leaves the flag at 0, and an unbound stencil view reads 0, so no pixel attaches either way; the flag is the contract's explicit statement, and the scenario cannot bind a stencil view while the flag is 0"},
    };
    const std::string shipped = edvr::kFlatMonoShaderSource;
    int caught = 0, equivalent = 0;
    auto runMutated = [&](const std::string& hlsl, int* failed, std::string* first) {
        std::vector<unsigned char> bytes;
        if (!compilePrep(hlsl, bytes)) return false;
        // The bytes must make a compute shader on this device, or "the scenario fails" would only mean the resolver could not start.
        ComPtr<ID3D11ComputeShader> probe;
        if (FAILED(fx.device->CreateComputeShader(bytes.data(), bytes.size(), nullptr, probe.GetAddressOf()))) return false;
        edvr::flatMonoResolveTestPrepBytecode(bytes.data(), bytes.size());
        edvr::flatMonoResolveReset();
        *failed = 0; mutationFailures = failed; mutationFirst.clear();
        scenario(fx);
        mutationFailures = nullptr; if (first) *first = mutationFirst;
        return true;
    };
    // The control: the same source, compiled here and run through the same seam, is the shipped prep and passes. Without it a failing
    // mutant could be the harness and not the rule.
    {
        std::vector<unsigned char> bytes;
        const bool compiled = compilePrep(shipped, bytes);
        check(compiled, "first person mutations: the shipped prep source compiles here");
        const bool sameBytes = compiled && bytes.size() == sizeof(edvr::kFlatMonoPrepBytecode) &&
                               !std::memcmp(bytes.data(), edvr::kFlatMonoPrepBytecode, bytes.size());
        std::printf("flat mono resolve: first-person mutation harness: the shipped prep source compiled here is %s the generated bytecode (%zu bytes)\n",
                    sameBytes ? "byte-for-byte" : "NOT byte-for-byte", bytes.size());
        int failed = 0; std::string first;
        const bool ran = runMutated(shipped, &failed, &first);
        check(ran && failed == 0, "first person mutations: control: the unmutated prep, compiled here and run through the test seam, passes the whole scenario");
    }
    for (const Mutant& m : mutants) {
        bool once = false;
        const std::string hlsl = replaceOnce(shipped, m.from, m.to, &once);
        char what[256];
        std::snprintf(what, sizeof(what), "first person mutations: \"%s\": its anchor is in the shader source exactly once (a moved anchor tests nothing)", m.name);
        check(once, what);
        int failed = 0; std::string first;
        const bool ran = once && runMutated(hlsl, &failed, &first);
        std::snprintf(what, sizeof(what), "first person mutations: \"%s\" compiles and makes a compute shader", m.name);
        check(ran, what);
        if (!ran) continue;
        if (m.survivesBecause) {
            ++equivalent;
            std::printf("flat mono resolve: first-person mutation \"%s\": %s; %d checks fail (equivalent by construction: %s)\n",
                        m.name, failed ? "caught" : "survives", failed, m.survivesBecause);
            continue;
        }
        std::snprintf(what, sizeof(what), "first person mutations: \"%s\" is caught by the scenario", m.name);
        check(failed > 0, what);
        if (failed > 0) ++caught;
        std::printf("flat mono resolve: first-person mutation \"%s\": %d checks fail; first: %s\n", m.name, failed, first.c_str());
    }
    edvr::flatMonoResolveTestPrepBytecode(nullptr, 0);
    edvr::flatMonoResolveReset();
    std::printf("flat mono resolve: first-person mutations: %d of %zu caught by the scenario, %d equivalent by construction\n",
                caught, sizeof(mutants) / sizeof(mutants[0]) - size_t(equivalent), equivalent);
    check(size_t(caught) + size_t(equivalent) == sizeof(mutants) / sizeof(mutants[0]),
          "first person mutations: every mutation was either caught or named equivalent");
}

// ---- the contract around the prep (shipped prep only) --------------------------------------------------------------------------------
inline void contractTests(Fixture& fx) {
    using edvr::FlatMonoResolveFrame;
    using edvr::FlatMonoResolveMode;
    edvr::flatMonoResolveReset();
    fx.specs = makeSpecs(nullptr); fx.upload(); fx.joinEngineRecords();
    fx.fillColor(grey128);
    FlatMonoResolveFrame base = fx.baseFrame(FlatMonoResolveMode::Dlss);
    base.frame = 50000; base.reset = true;
    expectedJx = expectedJy = 0;
    resolveFrame(fx, base);                 // the reset frame that starts the run; everything after continues
    base.reset = false;
    auto next = [&](ID3D11ShaderResourceView* map, ID3D11ShaderResourceView* stencil) {
        FlatMonoResolveFrame f = base; f.frame = ++base.frame;
        f.firstPersonMotion = map; f.firstPersonStencil = stencil;
        return f;
    };
    auto stats = [] { return edvr::flatMonoResolveStats(); };
    const uint64_t bareHash = resolveFrame(fx, next(nullptr, nullptr)).hash;
    check(bareHash != 0, "first person contract: the bare frame's output reached the backend");
    auto sameAsBare = [&](const FlatMonoResolveFrame& f) { const Seen s = resolveFrame(fx, f); return s.ok && s.hash == bareHash; };

    // Absent and partial.
    auto before = stats();
    check(sameAsBare(next(nullptr, nullptr)), "first person contract: two absent inputs leave the prep's whole output bit for bit what it was");
    check(stats().firstPersonFrames == before.firstPersonFrames && stats().firstPersonRefused == before.firstPersonRefused &&
              stats().firstPersonPartial == before.firstPersonPartial, "first person contract: absent inputs count nowhere");
    check(sameAsBare(next(fx.mapView.Get(), nullptr)) && stats().firstPersonPartial == before.firstPersonPartial + 1,
          "first person contract: a map without its stencil is treated as absent (output unchanged), counted as a partial pair");
    check(sameAsBare(next(nullptr, fx.stencilView.Get())) && stats().firstPersonPartial == before.firstPersonPartial + 2,
          "first person contract: a stencil without its map is treated as absent (output unchanged), counted as a partial pair");
    check(stats().firstPersonRefused == before.firstPersonRefused && stats().firstPersonFrames == before.firstPersonFrames,
          "first person contract: a partial pair is neither a refusal nor a frame that used the pair");
    // A whole, valid pair with no stencil bit set anywhere: bound, counted, and nothing attaches.
    {
        const std::vector<Spec> keep = fx.specs;
        for (Spec& t : fx.specs) t.stencil = 0;
        fx.upload();
        before = stats();
        check(sameAsBare(next(fx.mapView.Get(), fx.stencilView.Get())) && stats().firstPersonFrames == before.firstPersonFrames + 1,
              "first person contract: a valid pair with the stencil bit set nowhere is bound and counted, and the output is bit for bit the bare run's");
        fx.specs = keep; fx.upload();
    }

    // Refused pairs: each is counted and named, never refuses the frame, and leaves the output bit for bit the bare run's.
    struct Refusal { const char* what; const char* key; };
    auto refused = [&](const FlatMonoResolveFrame& f, const Refusal& r) {
        const auto was = stats();
        const bool same = sameAsBare(f);
        const auto now = stats();
        char what[320];
        std::snprintf(what, sizeof(what), "first person contract: %s is refused, the frame still resolves and the output is bit for bit the bare run's", r.what);
        check(same, what);
        std::snprintf(what, sizeof(what), "first person contract: %s is counted as a refusal, not as a frame that used the pair", r.what);
        check(now.firstPersonRefused == was.firstPersonRefused + 1 && now.firstPersonFrames == was.firstPersonFrames &&
                  now.firstPersonPartial == was.firstPersonPartial, what);
        std::snprintf(what, sizeof(what), "first person contract: %s is named (\"%s\")", r.what, r.key);
        check(now.firstPersonRefusal && std::strstr(now.firstPersonRefusal, r.key), what);
    };
    {
        ID3D11Device* d = fx.device;
        auto mapSmall = texture(d, 8, 8, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        auto mapSmallView = view(d, mapSmall.Get());
        auto mapUnorm = texture(d, W, H, DXGI_FORMAT_R16G16B16A16_UNORM, D3D11_BIND_SHADER_RESOURCE);
        auto mapUnormView = view(d, mapUnorm.Get());
        D3D11_TEXTURE2D_DESC arr{}; arr.Width = W; arr.Height = H; arr.MipLevels = 1; arr.ArraySize = 2; arr.SampleDesc.Count = 1;
        arr.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; arr.Usage = D3D11_USAGE_DEFAULT; arr.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> mapArray; check(SUCCEEDED(d->CreateTexture2D(&arr, nullptr, mapArray.GetAddressOf())), "first person contract: array map fixture");
        D3D11_SHADER_RESOURCE_VIEW_DESC av{}; av.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; av.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        av.Texture2DArray.MipLevels = 1; av.Texture2DArray.ArraySize = 1;
        ComPtr<ID3D11ShaderResourceView> mapArrayView; check(SUCCEEDED(d->CreateShaderResourceView(mapArray.Get(), &av, mapArrayView.GetAddressOf())), "first person contract: array map view");
        D3D11_TEXTURE2D_DESC ds{}; ds.Width = 8; ds.Height = 8; ds.MipLevels = 1; ds.ArraySize = 1; ds.SampleDesc.Count = 1;
        ds.Format = DXGI_FORMAT_R32G8X24_TYPELESS; ds.Usage = D3D11_USAGE_DEFAULT; ds.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> stencilSmall; check(SUCCEEDED(d->CreateTexture2D(&ds, nullptr, stencilSmall.GetAddressOf())), "first person contract: small stencil fixture");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> stencilSmallView; check(SUCCEEDED(d->CreateShaderResourceView(stencilSmall.Get(), &sv, stencilSmallView.GetAddressOf())), "first person contract: small stencil view");
        ds.Width = W; ds.Height = H; ds.ArraySize = 2;
        ComPtr<ID3D11Texture2D> stencilArray; check(SUCCEEDED(d->CreateTexture2D(&ds, nullptr, stencilArray.GetAddressOf())), "first person contract: array stencil fixture");
        D3D11_SHADER_RESOURCE_VIEW_DESC sav{}; sav.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT; sav.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sav.Texture2DArray.MipLevels = 1; sav.Texture2DArray.ArraySize = 1;
        ComPtr<ID3D11ShaderResourceView> stencilArrayView; check(SUCCEEDED(d->CreateShaderResourceView(stencilArray.Get(), &sav, stencilArrayView.GetAddressOf())), "first person contract: array stencil view");
        // A map texture with two mips, seen whole at mip 0 and seen at mip 1; a multisampled stencil texture.
        D3D11_TEXTURE2D_DESC mm = arr; mm.ArraySize = 1; mm.MipLevels = 2;
        ComPtr<ID3D11Texture2D> mapMipped; check(SUCCEEDED(d->CreateTexture2D(&mm, nullptr, mapMipped.GetAddressOf())), "first person contract: mipped map fixture");
        D3D11_SHADER_RESOURCE_VIEW_DESC mv0{}; mv0.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; mv0.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; mv0.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> mapMipped0, mapMipped1;
        check(SUCCEEDED(d->CreateShaderResourceView(mapMipped.Get(), &mv0, mapMipped0.GetAddressOf())), "first person contract: mip 0 map view");
        mv0.Texture2D.MostDetailedMip = 1;
        check(SUCCEEDED(d->CreateShaderResourceView(mapMipped.Get(), &mv0, mapMipped1.GetAddressOf())), "first person contract: mip 1 map view");
        ComPtr<ID3D11Texture2D> stencilMsaa; ComPtr<ID3D11ShaderResourceView> stencilMsaaView;
        {
            D3D11_TEXTURE2D_DESC ms = ds; ms.ArraySize = 1; ms.SampleDesc.Count = 4; ms.SampleDesc.Quality = 0;
            D3D11_SHADER_RESOURCE_VIEW_DESC msv{}; msv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT; msv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
            if (FAILED(d->CreateTexture2D(&ms, nullptr, stencilMsaa.GetAddressOf())) ||
                FAILED(d->CreateShaderResourceView(stencilMsaa.Get(), &msv, stencilMsaaView.GetAddressOf())))
                std::printf("info: first person contract: this device makes no 4x multisampled R32G8X24 stencil view; that refusal case is skipped\n");
        }
        // The first refusal of the session is the one the log names: the map texture's size.
        refused(next(mapSmallView.Get(), fx.stencilView.Get()), {"a map texture of the wrong size", "map texture is not the render size"});
        refused(next(mapUnormView.Get(), fx.stencilView.Get()), {"a map view of the wrong format (R16G16B16A16_UNORM)", "map view format"});
        refused(next(mapArrayView.Get(), fx.stencilView.Get()), {"a map view that is a Texture2DArray view", "map view is not a Texture2D view"});
        refused(next(mapMipped1.Get(), fx.stencilView.Get()), {"a map view of mip 1", "map view is not a one-mip view"});
        refused(next(mapMipped0.Get(), fx.stencilView.Get()), {"a map texture with two mips", "map texture is not a one-slice"});
        refused(next(fx.mapView.Get(), fx.depthView.Get()), {"a stencil view of the wrong format (the depth plane's)", "stencil view format"});
        refused(next(fx.mapView.Get(), stencilArrayView.Get()), {"a stencil view that is a Texture2DArray view", "stencil view is not a Texture2D view"});
        if (stencilMsaaView)
            refused(next(fx.mapView.Get(), stencilMsaaView.Get()), {"a multisampled stencil view", "stencil view is not a Texture2D view"});
        refused(next(fx.mapView.Get(), stencilSmallView.Get()), {"a stencil texture of the wrong size", "stencil texture is not the render size"});
    }
    // And a good pair after all of it still works: nothing a refusal did is left behind.
    before = stats();
    check(resolveFrame(fx, next(fx.mapView.Get(), fx.stencilView.Get())).ok && stats().firstPersonFrames == before.firstPersonFrames + 1,
          "first person contract: a valid pair after refusals is bound and counted");

    // The log: each line once a session.
    unsigned bound = 0, refusedLines = 0;
    bool boundText = false, refusedText = false;
    for (const std::string& line : firstPersonLines) {
        if (line.rfind("flat resolve: first-person motion inputs bound", 0) == 0) { ++bound; boundText = line == "flat resolve: first-person motion inputs bound (map 16x16, stencil view)"; }
        if (line.rfind("flat resolve: first-person motion inputs refused: ", 0) == 0) {
            ++refusedLines; refusedText = line == "flat resolve: first-person motion inputs refused: the map texture is not the render size";
        }
    }
    check(bound == 1 && boundText, "first person contract: the accepted pair is logged once, naming the map size and the stencil view");
    check(refusedLines == 1 && refusedText, "first person contract: a refusal is logged once however many follow, naming the first one's reason");
    edvr::flatMonoResolveReset();
}

}  // namespace fpgpu

inline void firstPersonGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    fpgpu::Fixture fx(device, context);
    // 1. The scenario on the shipped prep, for real. Every frame of it that carried a valid pair is counted, and only those.
    const auto before = edvr::flatMonoResolveStats();
    fx.inputFrames = 0;
    fpgpu::scenario(fx);
    const auto after = edvr::flatMonoResolveStats();
    check(fx.inputFrames > 0 && after.firstPersonFrames - before.firstPersonFrames == fx.inputFrames &&
              after.firstPersonRefused == before.firstPersonRefused && after.firstPersonPartial == before.firstPersonPartial,
          "first person: every frame of the scenario that carried a valid pair is counted as one, and none is refused or partial");
    // 2. The contract around the prep: absent, partial and refused pairs, the log.
    fpgpu::contractTests(fx);
    // 3. The same scenario against the prep with one rule flipped at a time: it must fail each time.
    fpgpu::mutationChecks(fx);
}
