// EDVR's own TAA kernel's history-depth check on WARP (design doc section 104, "TAA's history depth check takes the best of four texels").
//
// taa() trusts a pixel's history only where last frame's depth holds the surface the camera term says the pixel showed. That is
// historyDepthMatches (flat_mono_shader_source.h), the rule the prep's steady detail applies to a stale slot (section 82): the best of the four
// texels around the pixel's previous raster position within 1% (floor 1e-6) of the depth the camera term expects. taa() used to read ONE texel,
// the one that position falls in. At a jittered silhouette that texel is the roof one frame and the sky the next, so the edge pixels lost their
// history on alternate frames and crawled while the camera stood still. This file pins the change through the real kernel, last frame's depth
// drawn by hand.
//
// The rig: a still camera (a pixel's camera term is the pixel itself, expected depth its own depth), a history that is a flat 128, and a current
// frame that is a checker of 64 and 192 whose every 3x3 box holds 128. A pixel that kept its history reads 122 or 134 (0.9 of 128 and 0.1 of the
// checker), one that was reset reads the checker itself, 64 or 192. The current phase is 0, so the colour a pixel samples is exactly its texel's;
// the previous phase is +-0.25 per axis. A previous phase under half a texel puts the previous raster position inside the pixel's own texel
// (x, y), which is the one texel the OLD rule read; the four texels are that one and its neighbours toward the phase (x+1 for +0.25, x-1 for
// -0.25; likewise y). The current frame is a uniform roof, so every pixel under test shows the roof now and only last frame's depth decides.
//
// Two tests, through the real kernel and then again, as MUTATION CHECKS, through the kernel with one rule flipped at a time (the mutated source
// is compiled here, handed to the resolver through flatMonoResolveTestTaaBytecode, and the tests must FAIL; a test that cannot fail proves
// nothing). The OLD single-texel rule is one of the mutants and must fail BOTH tests.
//  1. A jittered silhouette keeps its history: a pixel whose own texel was sky last frame but whose neighbour toward the phase was the roof
//     is kept (roof edges against the sky, both axes, both signs); a pixel one texel farther from the edge, whose four texels are all sky, is
//     reset.
//  2. A true disocclusion still resets: where a surface moved away and the pixel now shows the background, a pixel whose four texels all held
//     the occluder is reset; only the pixel at the trailing edge, whose four texels reach the background beside it, is kept (the best of four's
//     price, pinned in section 82 too: the check cannot see sub-texel motion, and this is what separates four texels from sixteen). A depth 1.1%
//     off, 1.1% off the other way, a sky, and a surface twice as near are all a different surface: reset; 0.9% off either way, and 5e-7 off at a
//     depth where the 1e-6 floor is wider than 1%, are the same surface: kept; 2e-6 off at that depth is not.
#pragma once
#include <d3dcompiler.h>
#include <string>
#include <vector>
#include "flat_steady_depth_gpu_tests.h"   // steadygpu::Rig, and the shaders' source and bytecode

namespace edvr {
// flat_mono_resolve.cpp, at its foot: the TAA kernel the next initialisation makes is this bytecode instead of the shipped one. Test-only.
void flatMonoResolveTestTaaBytecode(const void* bytes, size_t size);
}

namespace taadepth {
using steadygpu::Rig;
constexpr UINT W = steadygpu::W, H = steadygpu::H;
constexpr float kRoof = .01f;    // the depth the current frame has everywhere, and the surface the pixels under test show
constexpr float kSky = 0.f;      // reversed-Z, infinite far
constexpr float kNear = .04f;    // an occluder in front of the roof

inline void fillRect(std::vector<float>& z, UINT x0, UINT y0, UINT w, UINT h, float v) {
    for (UINT y = y0; y < y0 + h; ++y) for (UINT x = x0; x < x0 + w; ++x) z[size_t(y) * W + x] = v;
}

// The history frame (flat grey, so the previous output is a flat 128 whatever came before it: its colour box collapses onto 128), drawn with
// `before` as its depth and the phase (px, py); then the frame under test: the checker, `now` as its depth, current phase 0, previous phase
// (px, py). Returns the frame under test's red channel at every pixel, row-major, or nothing if the output could not be read.
inline std::vector<int> runPair(Rig& r, const std::vector<float>& before, const std::vector<float>& now, float px, float py) {
    std::fill(r.rgba.begin(), r.rgba.end(), 0xff808080u);
    r.z = before; r.phases(px, py, 0, 0);
    r.run("taa history depth: the history frame");
    for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x) r.rgba[size_t(y) * W + x] = ((x + y) & 1) ? 0xffc0c0c0u : 0xff404040u;
    r.z = now; r.phases(0, 0, px, py);
    ComPtr<ID3D11ShaderResourceView> out;
    r.run("taa history depth: the frame under test", true, &out);
    r.phases(0, 0, 0, 0);
    std::vector<int> red;
    ComPtr<ID3D11Resource> resource; if (out) out->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture2d; if (resource) resource.As(&texture2d);
    std::vector<unsigned char> bytes;
    if (texture2d && readWhole(r.context, texture2d.Get(), bytes, 4)) {
        red.resize(size_t(W) * H);
        for (size_t i = 0; i < red.size(); ++i) red[i] = bytes[i * 4];
    }
    return red;
}

// 1 = the pixel kept its history, 0 = it was reset, -1 = neither (or nothing was read).
inline int state(const std::vector<int>& red, UINT x, UINT y) {
    if (red.size() != size_t(W) * H || x >= W || y >= H) return -1;
    const bool high = ((x + y) & 1) != 0;
    const int v = red[size_t(y) * W + x], checker = high ? 192 : 64, mixed = high ? 134 : 122;
    if (v >= checker - 1 && v <= checker + 1) return 0;
    if (v >= mixed - 3 && v <= mixed + 3) return 1;
    return -1;
}

// The pixels from (x0, y0) stepping (dx, dy), one letter each (K kept, R reset, ? neither), against what the tests say they must be.
inline void expectRun(const std::vector<int>& red, const char* name, UINT x0, UINT y0, int dx, int dy, const char* want) {
    std::string got;
    for (size_t i = 0; i < std::strlen(want); ++i) {
        const int s = state(red, UINT(int(x0) + dx * int(i)), UINT(int(y0) + dy * int(i)));
        got += s == 1 ? 'K' : (s == 0 ? 'R' : '?');
    }
    char what[400];
    std::snprintf(what, sizeof(what), "taa history depth: %s: expected %s (K history kept, R reset), got %s", name, want, got.c_str());
    check(got == want, what);
}

// A fresh renderer, the reset frame that starts a run, and a continuing frame after it.
inline void startRun(Rig& r) {
    edvr::flatMonoResolveReset();
    r.start(edvr::FlatMonoResolveMode::Taa);
    std::fill(r.rgba.begin(), r.rgba.end(), 0xff808080u);
    r.run("taa history depth: the reset frame");
    r.f.reset = false;
}

// ---- 1. a jittered silhouette keeps its history ---------------------------------------------------------------------------------------
// Expected strings, each run named by what it walks across; the OLD rule's string is in the comment: it differs at the pixel whose own texel was
// the sky and whose neighbour toward the phase was the roof.
inline void silhouetteTest(Rig& r) {
    startRun(r);
    const std::vector<float> roof(size_t(W) * H, kRoof);
    char name[200];
    {   // Previous phase (+0.25, -0.25): the four texels are (x, x+1) by (y-1, y).
        std::vector<float> before = roof;
        fillRect(before, 0, 1, 4, 6, kSky);    // a vertical edge: sky in columns 0..3, rows 1..6; the roof from column 4 on
        fillRect(before, 6, 8, 9, 4, kSky);    // a horizontal edge: sky in rows 8..11, columns 6..14; the roof above and below
        const std::vector<int> red = runPair(r, before, roof, +.25f, -.25f);
        for (UINT y = 2; y <= 5; ++y) {
            std::snprintf(name, sizeof(name), "pair (+0.25, -0.25), a vertical edge with the roof to the right, row %u, columns 0..6", y);
            expectRun(red, name, 0, y, 1, 0, "RRRKKKK");      // old rule: RRRRKKK
        }
        for (UINT x = 7; x <= 13; ++x) {
            std::snprintf(name, sizeof(name), "pair (+0.25, -0.25), a horizontal edge with the roof above, column %u, rows 7..13", x);
            expectRun(red, name, x, 7, 0, 1, "KKRRRKK");      // old rule: KRRRRKK
        }
    }
    {   // Previous phase (-0.25, +0.25): the four texels are (x-1, x) by (y, y+1).
        std::vector<float> before = roof;
        fillRect(before, 12, 1, 4, 6, kSky);   // a vertical edge: sky in columns 12..15, rows 1..6; the roof to the left
        fillRect(before, 1, 7, 7, 4, kSky);    // a horizontal edge: sky in rows 7..10, columns 1..7; the roof below it (the roof's top edge)
        const std::vector<int> red = runPair(r, before, roof, -.25f, +.25f);
        for (UINT y = 2; y <= 5; ++y) {
            std::snprintf(name, sizeof(name), "pair (-0.25, +0.25), a vertical edge with the roof to the left, row %u, columns 9..15", y);
            expectRun(red, name, 9, y, 1, 0, "KKKKRRR");      // old rule: KKKRRRR
        }
        for (UINT x = 2; x <= 7; ++x) {
            std::snprintf(name, sizeof(name), "pair (-0.25, +0.25), a horizontal edge with the roof below (its top edge), column %u, rows 5..12", x);
            expectRun(red, name, x, 5, 0, 1, "KKRRRKKK");     // old rule: KKRRRRKK
        }
    }
}

// ---- 2. a true disocclusion still resets ----------------------------------------------------------------------------------------------
inline void disocclusionTest(Rig& r) {
    startRun(r);
    const std::vector<float> roof(size_t(W) * H, kRoof);
    char name[200];
    {   // An occluder that moved away: four columns wide last frame, the roof (the background) now. Previous phase (+0.25, -0.25).
        std::vector<float> before = roof;
        fillRect(before, 1, 9, 4, 6, kNear);   // columns 1..4, rows 9..14
        const std::vector<int> red = runPair(r, before, roof, +.25f, -.25f);
        std::snprintf(name, sizeof(name), "pair (+0.25, -0.25), an occluder that moved away, row 9 (the row above it is background), columns 0..5");
        expectRun(red, name, 0, 9, 1, 0, "KKKKKK");           // old rule: KRRRRK
        for (UINT y = 10; y <= 14; ++y) {
            std::snprintf(name, sizeof(name), "pair (+0.25, -0.25), an occluder that moved away, row %u, columns 0..5", y);
            expectRun(red, name, 0, y, 1, 0, "KRRRKK");       // old rule: KRRRRK
        }
        std::snprintf(name, sizeof(name), "pair (+0.25, -0.25), an occluder that moved away, row 15 (below it is background), columns 0..5");
        expectRun(red, name, 0, 15, 1, 0, "KKKKKK");
    }
    {   // The same, previous phase (-0.25, +0.25), four columns at 9..12.
        std::vector<float> before = roof;
        fillRect(before, 9, 8, 4, 6, kNear);   // columns 9..12, rows 8..13
        const std::vector<int> red = runPair(r, before, roof, -.25f, +.25f);
        std::snprintf(name, sizeof(name), "pair (-0.25, +0.25), an occluder that moved away, row 7 (above it is background), columns 8..13");
        expectRun(red, name, 8, 7, 1, 0, "KKKKKK");
        for (UINT y = 8; y <= 12; ++y) {
            std::snprintf(name, sizeof(name), "pair (-0.25, +0.25), an occluder that moved away, row %u, columns 8..13", y);
            expectRun(red, name, 8, y, 1, 0, "KKRRRK");       // old rule: KRRRRK
        }
        std::snprintf(name, sizeof(name), "pair (-0.25, +0.25), an occluder that moved away, row 13 (the row below it is background), columns 8..13");
        expectRun(red, name, 8, 13, 1, 0, "KKKKKK");          // old rule: KRRRRK
    }
    {   // Nine 3x3 patches, previous phase (+0.25, +0.25), the four texels (x, x+1) by (y, y+1): every one inside its patch, so the edges of the
        // patches decide nothing and the surface alone does. The centre of each patch is the pixel under test.
        struct Patch { UINT cx, cy; float now, before; bool kept; const char* what; };
        static const Patch patches[] = {
            {3, 3, kRoof, kRoof * 1.009f, true, "0.9% farther than the camera term expects: the same surface"},
            {8, 3, kRoof, kRoof * 1.011f, false, "1.1% farther: another surface"},
            {13, 3, kRoof, kRoof / 1.009f, true, "0.9% nearer: the same surface"},
            {3, 8, kRoof, kRoof / 1.011f, false, "1.1% nearer: another surface"},
            {8, 8, 1e-5f, 1.05e-5f, true, "far away (depth 1e-5), 5% off but 5e-7, inside the 1e-6 floor: the same surface"},
            {13, 8, 1e-5f, 1.2e-5f, false, "far away, 2e-6 off, outside the floor: another surface"},
            {3, 13, kRoof, kSky, false, "the sky: another surface"},
            {8, 13, kRoof, 2 * kRoof, false, "a surface twice as near: another surface"},
            {13, 13, kRoof, kRoof, true, "the same depth: the control that a patch can keep its history in this very frame"},
        };
        std::vector<float> before = roof, now = roof;
        for (const Patch& p : patches) { fillRect(before, p.cx - 1, p.cy - 1, 3, 3, p.before); fillRect(now, p.cx - 1, p.cy - 1, 3, 3, p.now); }
        const std::vector<int> red = runPair(r, before, now, +.25f, +.25f);
        for (const Patch& p : patches) {
            std::snprintf(name, sizeof(name), "pixel (%u, %u): %s", p.cx, p.cy, p.what);
            expectRun(red, name, p.cx, p.cy, 1, 0, p.kept ? "K" : "R");
        }
    }
}

// ---- the kernel's source, one rule flipped at a time ----------------------------------------------------------------------------------
inline bool compileTaa(const std::string& hlsl, std::vector<unsigned char>& bytes) {
    const std::string full = std::string(edvr::kEngineMotionCoreHlsl) + hlsl;
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(full.c_str(), full.size(), "flat_mono_taa_cs", nullptr, nullptr, "taa", "cs_5_0", 0, 0,
                                  code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr) || !code) {
        std::printf("info: taa compile failed: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
        return false;
    }
    const auto* p = static_cast<const unsigned char*>(code->GetBufferPointer());
    bytes.assign(p, p + code->GetBufferSize());
    return true;
}

inline void mutationChecks(Rig& r) {
    using fpgpu::Mutant;
    static const char* const kCall = "if(historyDepthMatches(previous,ExpectedDepth.Load(int3(q,0))))weight=.9;";
    static const char* const kLoop =
        "[unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)best=min(best,abs(HistoryDepth.Load(int3(clamp(base+int2(x,y),0,hi),0))-expected));";
    static const char* const kOldRule =
        "{int2 oldQ=clamp(int2(previous*float2(size.xy)+jitter.zw),0,int2(size.xy)-1);"
        "float was=HistoryDepth.Load(int3(oldQ,0)), predicted=ExpectedDepth.Load(int3(q,0));"
        "if(abs(was-predicted)<=max(1e-6,predicted*.01))weight=.9;}";
    // The first mutant is the one this change replaced; the test below holds it to failing both tests.
    static const Mutant mutants[] = {
        {"the old rule: the one texel the previous raster position falls in", kCall, kOldRule, nullptr},
        {"there is no history depth check in taa()", kCall, "weight=.9;", nullptr},
        {"the history depth check is inverted", kCall, "if(!historyDepthMatches(previous,ExpectedDepth.Load(int3(q,0))))weight=.9;", nullptr},
        {"one texel, not four", kLoop,
         "[unroll]for(int y=0;y<1;++y)[unroll]for(int x=0;x<1;++x)best=min(best,abs(HistoryDepth.Load(int3(clamp(base+int2(x,y),0,hi),0))-expected));", nullptr},
        {"sixteen texels, not four", kLoop,
         "[unroll]for(int y=-1;y<3;++y)[unroll]for(int x=-1;x<3;++x)best=min(best,abs(HistoryDepth.Load(int3(clamp(base+int2(x,y),0,hi),0))-expected));", nullptr},
        {"the check reads this frame's depth, not last frame's", kLoop,
         "[unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)best=min(best,abs(SceneDepth.Load(int3(clamp(base+int2(x,y),0,hi),0))-expected));", nullptr},
        {"the previous phase has the wrong sign", "prev*float2(size.xy)+jitter.zw-.5", "prev*float2(size.xy)-jitter.zw-.5", nullptr},
        {"the half texel is dropped", "prev*float2(size.xy)+jitter.zw-.5", "prev*float2(size.xy)+jitter.zw", nullptr},
        {"the previous phase's axes are swapped", "prev*float2(size.xy)+jitter.zw-.5", "prev*float2(size.xy)+jitter.wz-.5", nullptr},
        {"the tolerance is zero (only the floor stays)", "kStaleDepthRel=.01,", "kStaleDepthRel=0,", nullptr},
        {"the tolerance is 5%", "kStaleDepthRel=.01,", "kStaleDepthRel=.05,", nullptr},
        {"the tolerance is 0.5%", "kStaleDepthRel=.01,", "kStaleDepthRel=.005,", nullptr},
        {"the floor is zero", "kStaleDepthFloor=1e-6;", "kStaleDepthFloor=0;", nullptr},
        {"the floor is 1e-3", "kStaleDepthFloor=1e-6;", "kStaleDepthFloor=1e-3;", nullptr},
        {"the tolerance is the smaller of the floor and the relative term", "max(kStaleDepthFloor,expected*kStaleDepthRel)",
         "min(kStaleDepthFloor,expected*kStaleDepthRel)", nullptr},
    };
    const std::string shipped = edvr::kFlatMonoShaderSource;
    int caught = 0;
    struct Outcome { bool ran = false; int silhouette = 0, disocclusion = 0; std::string firstSilhouette, firstDisocclusion; };
    auto runMutated = [&](const std::string& hlsl) {
        Outcome o;
        std::vector<unsigned char> bytes;
        if (!compileTaa(hlsl, bytes)) return o;
        ComPtr<ID3D11ComputeShader> probe;
        if (FAILED(r.device->CreateComputeShader(bytes.data(), bytes.size(), nullptr, probe.GetAddressOf()))) return o;
        edvr::flatMonoResolveTestTaaBytecode(bytes.data(), bytes.size());
        o.ran = true;
        mutationFailures = &o.silhouette; mutationFirst.clear();
        silhouetteTest(r);
        o.firstSilhouette = mutationFirst;
        mutationFailures = &o.disocclusion; mutationFirst.clear();
        disocclusionTest(r);
        o.firstDisocclusion = mutationFirst;
        mutationFailures = nullptr;
        return o;
    };
    {   // The control: the same source, compiled here and run through the same seam, is the shipped kernel and passes both tests.
        std::vector<unsigned char> bytes;
        const bool compiled = compileTaa(shipped, bytes);
        check(compiled, "taa history depth mutations: the shipped kernel's source compiles here");
        const bool sameBytes = compiled && bytes.size() == sizeof(edvr::kFlatMonoTaaBytecode) &&
                               !std::memcmp(bytes.data(), edvr::kFlatMonoTaaBytecode, bytes.size());
        std::printf("flat mono resolve: taa history-depth mutation harness: the shipped taa source compiled here is %s the generated bytecode (%zu bytes)\n",
                    sameBytes ? "byte-for-byte" : "NOT byte-for-byte", bytes.size());
        const Outcome o = runMutated(shipped);
        check(o.ran && o.silhouette == 0 && o.disocclusion == 0,
              "taa history depth mutations: control: the unmutated kernel, compiled here and run through the test seam, passes both tests");
    }
    for (const Mutant& m : mutants) {
        bool once = false;
        const std::string hlsl = fpgpu::replaceOnce(shipped, m.from, m.to, &once);
        char what[300];
        std::snprintf(what, sizeof(what), "taa history depth mutations: \"%s\": its anchor is in the shader source exactly once (a moved anchor tests nothing)", m.name);
        check(once, what);
        const Outcome o = once ? runMutated(hlsl) : Outcome{};
        std::snprintf(what, sizeof(what), "taa history depth mutations: \"%s\" compiles and makes a compute shader", m.name);
        check(o.ran, what);
        if (!o.ran) continue;
        std::snprintf(what, sizeof(what), "taa history depth mutations: \"%s\" is caught by the two tests", m.name);
        check(o.silhouette + o.disocclusion > 0, what);
        if (o.silhouette + o.disocclusion > 0) ++caught;
        std::printf("flat mono resolve: taa history-depth mutation \"%s\": silhouette test %d checks fail%s%s; disocclusion test %d fail%s%s\n", m.name,
                    o.silhouette, o.silhouette ? ", first: " : "", o.firstSilhouette.c_str(),
                    o.disocclusion, o.disocclusion ? ", first: " : "", o.firstDisocclusion.c_str());
        if (&m == &mutants[0])   // the rule this change replaced
            check(o.silhouette > 0 && o.disocclusion > 0,
                  "taa history depth mutations: the old single-texel rule fails BOTH the silhouette test and the disocclusion test");
    }
    edvr::flatMonoResolveTestTaaBytecode(nullptr, 0);
    edvr::flatMonoResolveReset();
    std::printf("flat mono resolve: taa history-depth mutations: %d of %zu caught by the tests\n", caught, sizeof(mutants) / sizeof(mutants[0]));
    check(size_t(caught) == sizeof(mutants) / sizeof(mutants[0]), "taa history depth mutations: every mutation was caught");
}

// ---- the contract on the shader source itself -----------------------------------------------------------------------------------------
inline void sourceTests() {
    const std::string source = edvr::kFlatMonoShaderSource;
    const size_t at = source.find("void taa(");
    const size_t end = at == std::string::npos ? at : source.find("void finish(", at);
    const std::string taa = at == std::string::npos || end == std::string::npos ? std::string() : source.substr(at, end - at);
    check(!taa.empty() && taa.find("if(historyDepthMatches(previous,ExpectedDepth.Load(int3(q,0))))weight=.9;") != std::string::npos,
          "taa history depth: taa() asks the shared history depth check (the best of four texels, 1% with a 1e-6 floor) before it trusts history");
    check(!taa.empty() && taa.find("HistoryDepth.Load") == std::string::npos && taa.find("oldQ") == std::string::npos,
          "taa history depth: taa() no longer reads one history depth texel itself");
}
}  // namespace taadepth

inline void taaHistoryDepthGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    taadepth::sourceTests();
    steadygpu::Rig rig(device, context);
    // 1 and 2. The two tests on the shipped kernel, for real.
    taadepth::silhouetteTest(rig);
    taadepth::disocclusionTest(rig);
    edvr::flatMonoResolveReset();
    // 3. The same two tests against the kernel with one rule flipped at a time, the old single-texel rule first: each must fail.
    taadepth::mutationChecks(rig);
    edvr::flatMonoResolveReset();
}
