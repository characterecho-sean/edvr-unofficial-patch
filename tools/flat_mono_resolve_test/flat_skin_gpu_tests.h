// F2 on foot (design doc: docs\kinematic-motion-injection-2026-09-19.md, the first-person entry) on WARP: the resolver's prep takes a skinned character's exact
// motion from the VR on-foot source's target 7 (EngineVelocityViews::skin, bound at t17 with debug.w bit 2), and the FLAT PROFILE, which has no target 7,
// is untouched.
//
// One fixture (flat_resolve_fixture.h: 16 x 16, a still camera, one pool record, the HDR route). A 4 x 4 block of source texels carries engine slot code 1
// (the pool's one record); the record is a skinned character's when its first word (the palette base) is nonzero. The cases:
//   A  a record with a base and a valid JOINED marker, no target 7 (the flat profile's frame, which never has one): the rigid path, as it was. The whole
//      output is bit for bit the same record with base 0: the base means nothing without the view.
//   E  no view, a base and no marker: the camera term, bit for bit the same as base 0 (a not-rig surface).
//   B  the view bound, a record with a base and no marker (its pose blocks unchanged: they say nothing): a texel with w 1 (or the interpolated 0.9995117) takes
//      its exact motion (previous - current = -31.25 cm in x is one render pixel, +31.25 cm in y is -1 pixel in y), a texel with w 0 or a non-finite E keeps no
//      history (the refused mask); the census counts the accepted ones as skinned and the others as masked.
//   C  the camera term is the SOURCE PASS's own scene constants (EN and EB, the engine views' constant buffers), not the route's rows: those say the camera
//      moved 0.3125 m, the resolver's route rows say it did not, and a skinned pixel with E = 0 moves one pixel.
//   D  the view bound and a RIGID record (base 0) with a joined marker: its own motion as ever, whatever the texel says.
//   F  EDVR's TAA with the view bound: the bit in debug.w is the prep's alone, the TAA kernel's history (its own "debug.w != 0" tests) is not disturbed.
// Then the same scenario against the prep with one rule flipped at a time (the in-rig mutation harness of flat_steady_depth_gpu_tests.h).
#pragma once
#include <limits>

namespace skingpu {
using namespace edvr;

inline bool nearTo(float got, float want) { return std::abs(got - want) < 0.004f; }

struct Result { uint64_t skinned = 0, masked = 0, refusedTotal = 0, frames = 0; };

// Runs the whole scenario against whatever prep the resolver was told to use (the shipped one, or a mutated one through flatMonoResolveTestPrepBytecode).
// `out` gets the census of the skin-view frames when given.
inline void scenario(ID3D11Device* device, ID3D11DeviceContext* context, Result* out) {
    const UINT w = ResolveFixture::w, h = ResolveFixture::h;
    flatMonoResolveReset();
    ResolveFixture fx(device, context);
    std::vector<float> z(w * h, .01f), slots(w * h * 2);
    auto noSlots = [&] { for (size_t i = 0; i < slots.size(); i += 2) { slots[i] = -1; slots[i + 1] = .01f; } };
    auto put = [&](UINT x, UINT y, float code, float slotDepth) { slots[(size_t(y) * w + x) * 2] = code; slots[(size_t(y) * w + x) * 2 + 1] = slotDepth; };
    noSlots();
    auto depth = texture(device, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, z.data(), w * 4);
    auto depthView = view(device, depth.Get());
    auto hTexture = texture(device, w, h, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    auto hSrv = view(device, hTexture.Get());
    std::vector<uint32_t> input(w * h, hdrgpu::pack(1.0, 1.0, 1.0));
    // Target 7: RGBA16F, xyz E in centimetres, w valid. Written whole before every frame.
    std::vector<uint16_t> skinTexels(size_t(w) * h * 4, 0);
    auto setSkin = [&](UINT x, UINT y, float ex, float ey, float ez, float ew) {
        uint16_t* p = &skinTexels[(size_t(y) * w + x) * 4];
        p[0] = fpgpu::toHalf(ex); p[1] = fpgpu::toHalf(ey); p[2] = fpgpu::toHalf(ez); p[3] = fpgpu::toHalf(ew);
    };
    auto skinTex = texture(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, skinTexels.data(), w * 8);
    auto skinView = view(device, skinTex.Get());
    // The source pass's scene constants with the camera origin moved 0.3125 m between last frame (x = -0.5) and this one (x = -0.1875): the route's own rows say
    // the origin did not move (case C), and neither frame's origin is the route's, so either one taken from the route's rows changes the answer.
    ComPtr<ID3D11Buffer> movedNow, movedOld;
    {
        float scene[277][4]{}; float cam[6][4]; camera(cam); std::memcpy(scene + 270, cam, sizeof(cam));
        const uint32_t stamp = 77; std::memcpy(&scene[276][0], &stamp, 4);
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(scene); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{scene, 0, 0};
        scene[275][0] = -.5f;
        check(SUCCEEDED(device->CreateBuffer(&bd, &init, movedOld.GetAddressOf())), "skin: scene constants of last frame");
        scene[275][0] = -.1875f;
        check(SUCCEEDED(device->CreateBuffer(&bd, &init, movedNow.GetAddressOf())), "skin: scene constants of this frame, the origin moved");
    }
    auto upload = [&] {
        context->UpdateSubresource(hTexture.Get(), 0, nullptr, input.data(), w * 4, 0);
        context->UpdateSubresource(fx.slotTexture.Get(), 0, nullptr, slots.data(), w * 8, 0);
        context->UpdateSubresource(depth.Get(), 0, nullptr, z.data(), w * 4, 0);
        context->UpdateSubresource(skinTex.Get(), 0, nullptr, skinTexels.data(), w * 8, 0);
    };
    uint32_t record[84]{};
    // base: the palette base word (0 = a rigid record). markerKind 0 none, 1 joined. moved: the pose blocks differ (-.3125 in x), else they are equal.
    auto setRecord = [&](uint32_t base, uint32_t markerKind, bool moved) {
        record[0] = base;
        record[1] = record[77] = bits(1); record[2] = record[78] = 0x7fff7fff; record[3] = record[79] = 0xfffe7fff;
        record[4] = bits(0); record[5] = bits(0); record[6] = bits(2.5f);
        record[73] = bits(moved ? -.3125f : 0.f); record[74] = bits(0); record[75] = bits(2.5f);
        edvr::engine_velocity_emit::Pose np{{record[4], record[5], record[6], record[2], record[3]}};
        edvr::engine_velocity_emit::Pose pp{{record[73], record[74], record[75], record[78], record[79]}};
        record[72] = markerKind == 0 ? 0u : 0x7FC0ED01u ^ edvr::engine_velocity_emit::markerHash(np, pp, 77);
        context->UpdateSubresource(fx.pool.Get(), 0, nullptr, record, 0, 0);
    };
    FlatMonoResolveFrame f{};
    f.color = hSrv.Get(); f.depth = depthView.Get(); f.renderWidth = w; f.renderHeight = h; f.outputWidth = w; f.outputHeight = h;
    f.deltaMs = 16; camera(f.camera); camera(f.previousCamera); fx.engine(f);
    f.mode = FlatMonoResolveMode::Dlss; f.frame = 6000; f.hdr = true; f.reset = true;
    expectedJx = expectedJy = 0;
    auto run = [&](const char* what) {
        upload();
        fx.bindOriginal();
        ComPtr<ID3D11ShaderResourceView> o; const char* why = nullptr;
        const bool ok = flatMonoResolve(device, context, f, o.GetAddressOf(), &why);
        if (!ok) std::printf("info: skin scenario \"%s\": resolver reason %s\n", what, why ? why : "none");
        check(ok && fx.restored(), what);
        ++f.frame;
        return ok;
    };
    auto motionX = [&](UINT x, UINT y) { return observedMotionAll.size() == size_t(w) * h * 2 ? observedMotionAll[(size_t(y) * w + x) * 2] : 99.f; };
    auto motionY = [&](UINT x, UINT y) { return observedMotionAll.size() == size_t(w) * h * 2 ? observedMotionAll[(size_t(y) * w + x) * 2 + 1] : 99.f; };
    auto rejected = [&](UINT x, UINT y) { return observedMaskAll.size() == size_t(w) * h ? observedMaskAll[size_t(y) * w + x] : 0xEEu; };
    auto takeAll = [&](uint64_t wantFrames) {
        refusalgpu::Taken t;
        for (int i = 0; i < 400 && t.frames < wantFrames; ++i) { context->Flush(); t.add(flatMonoResolveTakeRefusalCensus()); if (t.frames < wantFrames) Sleep(2); }
        return t;
    };
    auto block = [&] { noSlots(); for (UINT y = 4; y < 8; ++y) for (UINT x = 4; x < 8; ++x) put(x, y, 1, .01f); };
    // Target 7 for the block, four quadrants of 2 x 2 texels: (4..5, 4..5) valid 1 with E = (-31.25, 0, 0) cm; (6..7, 4..5) the interpolated valid flag
    // 0.9995117 with E = (0, +31.25, 0); (4..5, 6..7) no valid flag (E still set); (6..7, 6..7) valid 1 and an infinite E. `still`: every E is zero.
    auto quadrants = [&](bool still) {
        std::fill(skinTexels.begin(), skinTexels.end(), uint16_t(0));
        for (UINT y = 4; y < 6; ++y) for (UINT x = 4; x < 6; ++x) setSkin(x, y, still ? 0.f : -31.25f, 0, 0, 1.0f);
        for (UINT y = 4; y < 6; ++y) for (UINT x = 6; x < 8; ++x) setSkin(x, y, 0, still ? 0.f : 31.25f, 0, 0.99951171875f);
        for (UINT y = 6; y < 8; ++y) for (UINT x = 4; x < 6; ++x) setSkin(x, y, -31.25f, 0, 0, 0.0f);
        for (UINT y = 6; y < 8; ++y) for (UINT x = 6; x < 8; ++x) setSkin(x, y, std::numeric_limits<float>::infinity(), 0, 0, 1.0f);
    };
    auto readH = [&](std::vector<uint32_t>& out) {
        std::vector<unsigned char> bytes;
        if (!readWhole(context, hTexture.Get(), bytes, 4)) return false;
        out.resize(w * h);
        std::memcpy(out.data(), bytes.data(), bytes.size());
        return true;
    };

    // ---- the reset frame that starts the run ---------------------------------------------------------------------------------------------
    block(); setRecord(0, 1, true);
    run("skin: the reset frame that starts the run");
    f.reset = false;

    // ---- A. the flat profile's frame: no target 7 ----------------------------------------------------------------------------------------
    f.engine.skin = nullptr;
    setRecord(0, 1, true);
    run("skin A: reference: a rigid joined record");
    const uint64_t rigidHash = observedMotionHash;
    check(nearTo(motionX(5, 5), -1.f) && nearTo(motionY(5, 5), 0.f) && rejected(5, 5) == 0 && nearTo(motionX(7, 7), -1.f) && rejected(7, 7) == 0,
          "skin A: reference: the rigid joined record moves one render pixel (-31.25 cm in x), history kept");
    setRecord(7, 1, true);
    run("skin A: a record with a palette base and a joined marker, no target 7");
    check(observedMotionHash == rigidHash && rigidHash != 0,
          "skin A: the FLAT PROFILE's frame: with no target 7 bound a record with a palette base is the rigid path, the whole output bit for bit the base-0 record's");
    check(nearTo(motionX(5, 5), -1.f) && rejected(5, 5) == 0, "skin A: ...its joined marker still carries its motion");

    // ---- E. no view, a base and no marker: a not-rig surface ----------------------------------------------------------------------------
    setRecord(0, 0, true);
    run("skin E: reference: a record with no marker and base 0");
    const uint64_t notRigHash = observedMotionHash;
    setRecord(7, 0, true);
    run("skin E: a record with a base and no marker, no target 7");
    check(observedMotionHash == notRigHash && notRigHash != 0 && nearTo(motionX(5, 5), 0.f) && rejected(5, 5) == 0,
          "skin E: with no target 7 a base without a marker is a not-rig surface (the camera term) exactly as base 0 is: the output is bit for bit the same");

    // ---- B. the view bound ----------------------------------------------------------------------------------------------------------------
    f.engine.skin = skinView.Get();
    quadrants(false);
    setRecord(7, 0, false);
    run("skin B: target 7 bound, the block's four quadrants");
    check(nearTo(motionX(4, 4), -1.f) && nearTo(motionY(4, 4), 0.f) && rejected(4, 4) == 0 && nearTo(motionX(5, 5), -1.f) && rejected(5, 5) == 0,
          "skin B: a texel with w 1 takes its exact motion: previous - current = -31.25 cm in x is one render pixel, history kept");
    check(nearTo(motionX(6, 4), 0.f) && nearTo(motionY(6, 4), -1.f) && rejected(6, 4) == 0 && nearTo(motionY(7, 5), -1.f) && rejected(7, 5) == 0,
          "skin B: ...also the interpolated flag 0.9995117 (the vertex shader's constant 1 across a triangle: the test is w > 0.5), and +31.25 cm in y is -1 pixel in y");
    check(rejected(4, 6) == 255 && rejected(5, 7) == 255 && nearTo(motionX(4, 6), 0.f),
          "skin B: a texel with w 0 keeps no history: the mask is refused (255), the motion zero");
    check(rejected(6, 6) == 255 && rejected(7, 7) == 255,
          "skin B: a texel whose E is not finite keeps no history either");
    check(nearTo(motionX(1, 1), 0.f) && rejected(1, 1) == 0 && nearTo(motionX(12, 12), 0.f) && rejected(12, 12) == 0,
          "skin B: pixels with no engine slot are untouched (the camera term, still camera)");
    // the census: four asking frames take one sample
    f.refusalCensus = true;
    for (unsigned i = 0; i < 4; ++i) run("skin B: census: four asking frames");
    {
        const auto c = takeAll(1);
        if (out) { out->skinned = c.skinned; out->masked = c.counts[kFlatMonoClassMasked]; out->refusedTotal = c.refused(); out->frames = c.frames; }
        check(c.frames == 1 && c.skinned == 8 && c.counts[kFlatMonoClassMasked] == 8 && c.refused() == 8,
              "skin B: the census counts the 8 skinned pixels that took their exact motion (accepted, in their own slot) and the 8 masked ones (refused), nothing else");
    }
    f.refusalCensus = false;

    // ---- C. the camera term is the source pass's own scene constants ------------------------------------------------------------------------
    f.engine.sceneNow = movedNow.Get(); f.engine.scenePrev = movedOld.Get();
    quadrants(true);
    run("skin C: the source pass's camera moved, E zero");
    check(nearTo(motionX(4, 4), 1.f) && nearTo(motionY(4, 4), 0.f) && rejected(4, 4) == 0 && nearTo(motionX(6, 5), 1.f) && rejected(6, 5) == 0,
          "skin C: a skinned pixel with E = 0 moves with the SOURCE PASS's camera term (its scene constants say the origin moved 0.3125 m: one pixel), not the route's camera rows (still)");
    fx.engine(f); f.engine.skin = skinView.Get();

    // ---- D. a rigid record with the view bound -------------------------------------------------------------------------------------------
    std::fill(skinTexels.begin(), skinTexels.end(), uint16_t(0));   // every texel invalid: a rigid pixel must not look at them
    setRecord(0, 1, true);
    run("skin D: target 7 bound, a rigid joined record (base 0), every texel invalid");
    check(observedMotionHash == rigidHash && nearTo(motionX(5, 5), -1.f) && rejected(5, 5) == 0 && rejected(7, 7) == 0,
          "skin D: with target 7 bound a rigid record (base 0) keeps its own motion: the skinned branch is for a palette base only");

    // ---- F. EDVR's TAA with the view bound -----------------------------------------------------------------------------------------------
    // The same arithmetic as flat_hdr_route_gpu_tests.h section 4 (a hot pixel on a still scene blended at .9 in compressed space, near 111): the TAA kernel
    // reads the constants' debug.w for the alternate-camera coverage (bits 0 and 1), and bit 2 is not its business. A kernel that took any nonzero debug.w
    // for coverage would read an unbound coverage view as "no history" and show the hot pixel as it is (10000).
    {
        noSlots();
        f.mode = FlatMonoResolveMode::Taa; f.reset = true;
        input.assign(w * h, hdrgpu::pack(100.0, 100.0, 100.0));
        run("skin F: TAA, target 7 bound: the reset frame");
        f.reset = false;
        input[8 * w + 8] = hdrgpu::pack(10000.0, 10000.0, 10000.0);
        const bool ok = run("skin F: TAA, target 7 bound: a hot pixel on a still scene");
        const double cur = 10000.0 / 10001.0, hist = 100.0 / 101.0, mixed = cur + (hist - cur) * .9, want = mixed / (1 - mixed);
        std::vector<uint32_t> got; double hot[3] = {0, 0, 0}, distant[3] = {0, 0, 0};
        if (readH(got)) { hdrgpu::unpack(got[8 * w + 8], hot); hdrgpu::unpack(got[2 * w + 2], distant); }
        check(ok && std::abs(hot[0] - want) <= .02 * want && std::abs(distant[0] - 100) <= 1,
              "skin F: with target 7 bound EDVR's TAA still blends the hot pixel into its history (near 111, not the 10000 a disabled history shows)");
        f.mode = FlatMonoResolveMode::Dlss; f.reset = true;
        input.assign(w * h, hdrgpu::pack(1.0, 1.0, 1.0));
    }
    f.engine.skin = nullptr;
}

inline void tests(ID3D11Device* device, ID3D11DeviceContext* context) {
    // The HLSL's pieces the host and the log lines name.
    {
        const std::string source = kFlatMonoShaderSource;
        size_t bare = 0, masked = 0, at = 0;
        while ((at = source.find("debug.w!=0", at)) != std::string::npos) { ++bare; ++at; }
        at = 0;
        while ((at = source.find("(debug.w&3)!=0", at)) != std::string::npos) { ++masked; ++at; }
        check(refusalgpu::hlslConstant(source, "kClassSkinned") == int(kFlatMonoClassSkinned) && kFlatMonoClassSkinned == 15 && kFlatMonoClassSkinned <= kFlatMonoClassMask &&
                  kFlatMonoRefusalSkinned == kFlatMonoRefusalSlots + kFlatMonoWeaponReasons && kFlatMonoRefusalCounters == kFlatMonoRefusalSkinned + 1 &&
                  std::strcmp(flatMonoClassName(kFlatMonoClassSkinned), "skinned") == 0,
              "skin: the skinned class is 15 in the header and the shader, it fits the class byte, its census slot follows the reasons and the stripe is 25 counters");
        check(source.find("register(t17)") != std::string::npos && source.find("(debug.w&4)!=0 && r.data[0].x!=0u") != std::string::npos &&
                  source.find("gRefusal[25]") != std::string::npos && source.find("*25u+gi") != std::string::npos,
              "skin: target 7 is bound at t17 behind debug.w bit 2 and a palette base, and the census kernel counts 25 slots a stripe");
        check(bare == 0 && masked == 3,
              "skin: the TAA kernel's three alternate-camera tests read debug.w bits 0 and 1 only, never bit 2 (the prep's target 7): no bare debug.w != 0 is left");
    }
    Result shipped;
    scenario(device, context, &shipped);
    check(shipped.frames == 1 && shipped.skinned == 8, "skin: the scenario ran against the shipped prep and counted its skinned pixels");
    // The same scenario against the prep with one rule flipped at a time.
    static const fpgpu::Mutant mutants[] = {
        {"the skinned branch ignores the view bit (the flat profile's frame takes it)", "if((debug.w&4)!=0 && r.data[0].x!=0u) {", "if(r.data[0].x!=0u) {", nullptr},
        {"a rigid record takes the skinned branch", "if((debug.w&4)!=0 && r.data[0].x!=0u) {", "if((debug.w&4)!=0) {", nullptr},
        {"a texel without the valid flag is accepted", "if(!(sk.w>0.5) || !all(isfinite(sk.xyz))){cls=kClassMasked;return 2;}",
         "if(!all(isfinite(sk.xyz))){cls=kClassMasked;return 2;}", nullptr},
        {"a non-finite E is accepted", "if(!(sk.w>0.5) || !all(isfinite(sk.xyz))){cls=kClassMasked;return 2;}",
         "if(!(sk.w>0.5)){cls=kClassMasked;return 2;}", nullptr},
        {"the valid flag must be exactly 1", "if(!(sk.w>0.5) || !all(isfinite(sk.xyz))){cls=kClassMasked;return 2;}",
         "if(!(sk.w==1.0) || !all(isfinite(sk.xyz))){cls=kClassMasked;return 2;}", nullptr},
        {"E is previous - current the wrong way round", "engineReprojectRowsE(r,true,sk.xyz*.01,", "engineReprojectRowsE(r,true,-sk.xyz*.01,", nullptr},
        {"E is in metres, not centimetres", "engineReprojectRowsE(r,true,sk.xyz*.01,", "engineReprojectRowsE(r,true,sk.xyz,", nullptr},
        {"E's x and y are swapped", "engineReprojectRowsE(r,true,sk.xyz*.01,", "engineReprojectRowsE(r,true,sk.yxz*.01,", nullptr},
        {"the point is not carried by E (the record's pose blocks decide)", "engineReprojectRowsE(r,true,sk.xyz*.01,", "engineReprojectRowsE(r,false,sk.xyz*.01,", nullptr},
        {"this frame's camera origin is the route's, not the source pass's",
         "unjitterRow(EN[273],rowsJitter.xy),EN[275].xyz,\n            unjitterRow(EB[270],rowsJitter.zw)",
         "unjitterRow(EN[273],rowsJitter.xy),now[5].xyz,\n            unjitterRow(EB[270],rowsJitter.zw)", nullptr},
        {"last frame's camera origin is the route's, not the source pass's",
         "unjitterRow(EB[273],rowsJitter.zw),EB[275].xyz,before)){cls=kClassUnreprojectable;return 2;}\n        cls=kClassSkinned;",
         "unjitterRow(EB[273],rowsJitter.zw),old[5].xyz,before)){cls=kClassUnreprojectable;return 2;}\n        cls=kClassSkinned;", nullptr},
        {"the skinned pixel is called joined, not skinned (the census cannot tell the route read E)", "cls=kClassSkinned;\n        return 1;", "cls=kClassJoined;\n        return 1;", nullptr},
        {"the skinned pixel is read from the wrong texel", "const float4 sk=SkinE.Load(int3(q,0));", "const float4 sk=SkinE.Load(int3(q+int2(1,0),0));", nullptr},
        {"the skinned pixel keeps its history but takes no motion", "cls=kClassSkinned;\n        return 1;", "cls=kClassSkinned;\n        return 0;", nullptr},
    };
    const std::string shippedSource = kFlatMonoShaderSource;
    int caught = 0;
    size_t considered = 0;
    auto runMutated = [&](const std::string& hlsl, int* failed, std::string* first) {
        std::vector<unsigned char> bytes;
        if (!fpgpu::compilePrep(hlsl, bytes)) return false;
        ComPtr<ID3D11ComputeShader> probe;
        if (FAILED(device->CreateComputeShader(bytes.data(), bytes.size(), nullptr, probe.GetAddressOf()))) return false;
        flatMonoResolveTestPrepBytecode(bytes.data(), bytes.size());
        flatMonoResolveReset();
        *failed = 0; mutationFailures = failed; mutationFirst.clear();
        Result r;
        scenario(device, context, &r);
        mutationFailures = nullptr; if (first) *first = mutationFirst;
        return true;
    };
    {   // The control: the shipped source compiled here and run through the same seam passes the whole scenario.
        int failed = 0; std::string first;
        const bool ran = runMutated(shippedSource, &failed, &first);
        check(ran && failed == 0, "skin mutations: control: the unmutated prep, compiled here and run through the test seam, passes the whole scenario");
        if (failed) std::printf("flat mono resolve: skin control: %d checks fail; first: %s\n", failed, first.c_str());
    }
    for (const fpgpu::Mutant& m : mutants) {
        ++considered;
        bool once = false;
        const std::string hlsl = fpgpu::replaceOnce(shippedSource, m.from, m.to, &once);
        char what[256];
        std::snprintf(what, sizeof(what), "skin mutations: \"%.120s\": its anchor is in the shader source exactly once (a moved anchor tests nothing)", m.name);
        check(once, what);
        int failed = 0; std::string first;
        const bool ran = once && runMutated(hlsl, &failed, &first);
        std::snprintf(what, sizeof(what), "skin mutations: \"%.120s\" compiles and makes a compute shader", m.name);
        check(ran, what);
        if (!ran) continue;
        std::snprintf(what, sizeof(what), "skin mutations: \"%.120s\" is caught by the scenario", m.name);
        check(failed > 0, what);
        if (failed > 0) ++caught;
        std::printf("flat mono resolve: skin mutation \"%s\": %d checks fail; first: %s\n", m.name, failed, first.c_str());
    }
    flatMonoResolveTestPrepBytecode(nullptr, 0);
    flatMonoResolveReset();
    std::printf("flat mono resolve: skin mutations: %d of %zu caught by the scenario\n", caught, considered);
    check(size_t(caught) == considered, "skin mutations: every mutation was caught");
}
}  // namespace skingpu

inline void skinGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) { skingpu::tests(device, context); }
