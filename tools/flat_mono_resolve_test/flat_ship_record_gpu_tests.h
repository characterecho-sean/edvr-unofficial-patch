// The ship record on WARP (FlatMonoResolveFrame::shipRecordTerm, debug.w value 32; the flat profile in a ship, never on foot).
//
// The cockpit's body, seat and controls are near surfaces that ride the ship. Some carry no certified record of their own (no slot, an unmarked
// record, a stale slot over an unmarked one), so they took the camera term, which holds them still in the world: at metres a frame a vector of
// hundreds of pixels that reached the backend unchecked. With shipRecordTerm the record that owns the frame's near joined pixels (shipCount counts
// them on a sparse grid, shipPick takes the slot with the most) moves them, last frame's depth validates that, and a near pixel left on the
// camera term is checked against last frame's depth as well.
//
// The fixture is a 96 x 96 render (the grid is every 16th pixel, so 36 samples and the picker needs 32): a joined rig record that rides the
// camera owns the whole image except four 4 x 4 blocks (none of which holds a grid point): near with no slot, near over an unmarked slot, a stale
// slot over an unmarked record, and far with no slot. The camera translates ten texels between the frames (f.camera; the engine rows are still,
// as in flat_stale_record_gpu_tests.h), so the camera term looks ten texels away. Cases:
//   1  last frame's depth at each block is its depth now: the three near blocks are kept with no motion, class skinned (the census's accepted
//      counter); the far block is exactly what it is with the field false
//   2  last frame's depth changed: the near blocks are refused (class camera for the two kind-0 blocks, stale for the stale one), motion zero
//   3  too few samples (the ship record owns under 32 grid points) or no joined record: nothing rides; the near kind-0 blocks fail the camera
//      term's depth check and are refused, the far and stale blocks are exactly today's
//   4  shipRecordTerm false: the old path exactly (the kind-0 blocks keep their unchecked camera vector, the stale block is refused)
// And, as mutation checks on the shader source (the mutated kernel is compiled here and the scenario must FAIL): the prep never takes the
// ship record; the near camera check is gone, or applies without the bit; the ship motion is not validated; far pixels ride the ship; the
// counting kernel counts an uncertified record; the picker has no minimum.
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "flat_stale_record_gpu_tests.h"

namespace edvr {
// flat_mono_resolve.cpp, at its foot. Test-only: the next initialisation's ship record kernel (counting, or the picker) is this bytecode.
void flatMonoResolveTestShipBytecode(bool pick, const void* bytes, size_t size);
}

namespace shipgpu {
constexpr UINT N = 96, kBlock = 4;
struct Spot { UINT x, y; };
constexpr Spot kNone{10, 10}, kUnmarked{26, 10}, kStale{42, 10}, kFar{58, 10};   // each 4 x 4; the grid's points are 8 + 16k

struct Out {
    bool ok = false;
    std::vector<unsigned char> mask;
    std::vector<float> motion;
    unsigned refused(Spot s) const {
        unsigned n = 0;
        for (UINT y = s.y; y < s.y + kBlock; ++y)
            for (UINT x = s.x; x < s.x + kBlock; ++x)
                if (size_t(y) * N + x < mask.size() && mask[size_t(y) * N + x]) ++n;
        return n;
    }
    float maxMotion(Spot s) const {
        float m = 0;
        for (UINT y = s.y; y < s.y + kBlock; ++y)
            for (UINT x = s.x; x < s.x + kBlock; ++x) {
                const size_t t = size_t(y) * N + x;
                if (2 * t + 1 >= motion.size()) return 1e9f;
                m = (std::max)(m, (std::max)(std::fabs(motion[2 * t]), std::fabs(motion[2 * t + 1])));
            }
        return m;
    }
};
// The block's mask and motion, bit for bit, in two outputs.
inline bool sameIn(const Out& a, const Out& b, Spot s) {
    if (!a.ok || !b.ok) return false;
    for (UINT y = s.y; y < s.y + kBlock; ++y)
        for (UINT x = s.x; x < s.x + kBlock; ++x) {
            const size_t t = size_t(y) * N + x;
            if (2 * t + 1 >= a.motion.size() || 2 * t + 1 >= b.motion.size() || t >= a.mask.size() || t >= b.mask.size()) return false;
            if (a.mask[t] != b.mask[t] || a.motion[2 * t] != b.motion[2 * t] || a.motion[2 * t + 1] != b.motion[2 * t + 1]) return false;
        }
    return true;
}
inline bool same(const Out& a, const Out& b) { return a.ok && b.ok && a.mask == b.mask && a.motion == b.motion; }

struct Rig {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ResolveFixture fx;
    ComPtr<ID3D11Texture2D> depth, color;
    ComPtr<ID3D11ShaderResourceView> depthView, colorView;
    std::vector<float> z, slots;
    std::vector<uint32_t> rgba;
    uint32_t records[4][84]{};
    edvr::FlatMonoResolveFrame f{};

    Rig(ID3D11Device* d, ID3D11DeviceContext* c) : device(d), context(c), fx(d, c), z(N * N, .01f), slots(N * N * 2), rgba(N * N, 0xff808080u) {
        depth = texture(d, N, N, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, z.data(), N * 4);
        depthView = view(d, depth.Get());
        color = texture(d, N, N, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, rgba.data(), N * 4);
        colorView = view(d, color.Get());
        // The fixture's slot texture is 16 x 16 and its pool holds one record: this rig's are the render's size and four records.
        fx.slotTexture = texture(d, N, N, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, slots.data(), N * 8);
        fx.slotView = view(d, fx.slotTexture.Get());
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(records); bd.StructureByteStride = 336;
        bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = records;
        fx.pool.Reset(); fx.poolView.Reset();
        check(SUCCEEDED(d->CreateBuffer(&bd, &initial, fx.pool.GetAddressOf())), "ship record: the four-record pool");
        fx.poolView = view(d, fx.pool.Get());
        start();
    }
    void start(edvr::FlatMonoResolveMode mode = edvr::FlatMonoResolveMode::Dlss) {
        f = edvr::FlatMonoResolveFrame{};
        f.color = colorView.Get(); f.depth = depthView.Get();
        f.renderWidth = f.renderHeight = f.outputWidth = f.outputHeight = N;
        f.deltaMs = 16; camera(f.camera); camera(f.previousCamera); fx.engine(f);
        f.mode = mode; f.frame = 60000; f.reset = true;
        std::fill(rgba.begin(), rgba.end(), 0xff808080u);
    }
    void put(UINT x, UINT y, float code, float slotDepth) { slots[(size_t(y) * N + x) * 2] = code; slots[(size_t(y) * N + x) * 2 + 1] = slotDepth; }
    void block(Spot s, float code, float slotDepth) { for (UINT y = s.y; y < s.y + kBlock; ++y) for (UINT x = s.x; x < s.x + kBlock; ++x) put(x, y, code, slotDepth); }
    void fillBlock(Spot s, float v) { for (UINT y = s.y; y < s.y + kBlock; ++y) for (UINT x = s.x; x < s.x + kBlock; ++x) z[size_t(y) * N + x] = v; }
    // The records: 0 rides the camera (a rig record that did not move in the engine rows), joined by `shipMarker` (1 fresh joined, 0 none); 1 is
    // the unmarked record the "unmarked" blocks name.
    void setRecords(uint32_t shipMarker) {
        std::memset(records, 0, sizeof(records));
        for (int i = 0; i < 2; ++i) {
            uint32_t* r = records[i];
            r[1] = r[77] = bits(1); r[2] = r[78] = 0x7fff7fff; r[3] = r[79] = 0xfffe7fff;
            r[4] = r[73] = bits(0); r[5] = r[74] = bits(0); r[6] = r[75] = bits(2.5f);
            edvr::engine_velocity_emit::Pose np{{r[4], r[5], r[6], r[2], r[3]}};
            edvr::engine_velocity_emit::Pose pp{{r[73], r[74], r[75], r[78], r[79]}};
            r[72] = (i == 0 && shipMarker == 1) ? (0x7FC0ED01u ^ edvr::engine_velocity_emit::markerHash(np, pp, 77)) : 0u;
        }
    }
    // The slots: the ship record owns every pixel (code 1, slot depth .01) except the blocks; `lowRows` removes the slot from every row from 56 down
    // (three of the grid's six rows: 18 samples are left).
    void layout(bool lowRows) {
        for (UINT y = 0; y < N; ++y) for (UINT x = 0; x < N; ++x) put(x, y, (lowRows && y >= 56) ? -1.0f : 1.0f, .01f);
        block(kNone, -1.0f, .01f); block(kUnmarked, 3.0f, .01f); block(kStale, 3.0f, .5f); block(kFar, -1.0f, .01f);
    }
    Out run(const char* what) {
        context->UpdateSubresource(depth.Get(), 0, nullptr, z.data(), N * 4, 0);
        context->UpdateSubresource(color.Get(), 0, nullptr, rgba.data(), N * 4, 0);
        context->UpdateSubresource(fx.slotTexture.Get(), 0, nullptr, slots.data(), N * 8, 0);
        context->UpdateSubresource(fx.pool.Get(), 0, nullptr, records, 0, 0);
        expectedJx = f.jitterX; expectedJy = f.jitterY;
        fx.bindOriginal();
        ComPtr<ID3D11ShaderResourceView> resolved; const char* why = nullptr;
        const int before = backendCalls;
        const bool ok = edvr::flatMonoResolve(device, context, f, resolved.GetAddressOf(), &why);
        if (!ok) std::printf("info: ship record scenario \"%s\": resolver reason %s\n", what, why ? why : "none");
        check(ok, what);
        check(fx.restored(), "ship record: the game's whole pipeline comes back untouched");
        Out o; o.ok = ok;
        if (backendCalls > before) { o.mask = observedMaskAll; o.motion = observedMotionAll; }
        ++f.frame;
        return o;
    }
    // Two frames: last frame (`lastNear` in the three near blocks, .02 elsewhere, the far block at its own .001, the camera still), then this one
    // (.01 everywhere, .001 at the far block, the camera ten texels along). `ship` is the field; `census` samples this frame.
    Out frames(bool ship, float lastNear, bool census = false) {
        f.shipRecordTerm = false; f.staleRecordTerm = false; f.refusalCensus = false;
        f.camera[5][0] = 0;
        std::fill(z.begin(), z.end(), .02f);
        fillBlock(kNone, lastNear); fillBlock(kUnmarked, lastNear); fillBlock(kStale, lastNear); fillBlock(kFar, .001f);
        run("ship record: last frame");
        std::fill(z.begin(), z.end(), .01f);
        fillBlock(kFar, .001f);
        f.camera[5][0] = 0.5f;
        f.shipRecordTerm = ship; f.staleRecordTerm = true; f.refusalCensus = census;
        const Out o = run("ship record: this frame");
        f.camera[5][0] = 0; f.shipRecordTerm = false; f.staleRecordTerm = false; f.refusalCensus = false;
        return o;
    }
    void prepare() {
        edvr::flatMonoResolveReset();
        start(f.mode);
    }
};

inline void shipScenario(Rig& r, edvr::FlatMonoResolveMode mode) {
    using namespace edvr;
    flatMonoResolveReset();
    r.start(mode);
    r.setRecords(1);
    r.layout(false);
    r.run("ship record: the reset frame that starts the run");
    r.f.reset = false;
    r.f.steadyDetail = true;
    flatMonoResolveTakeRefusalCensus();

    // 1. Last frame's depth at the blocks is their depth now.
    const Out off = r.frames(false, .01f);
    const Out on = r.frames(true, .01f);
    check(off.refused(kNone) == 0 && off.refused(kUnmarked) == 0 && off.refused(kStale) == 16 && off.maxMotion(kNone) > 3 && off.maxMotion(kUnmarked) > 3,
          "ship record: with the field false the kind-0 near blocks keep their unchecked camera vector (several texels long) and the stale block is refused");
    check(on.refused(kNone) == 0 && on.refused(kUnmarked) == 0 && on.refused(kStale) == 0 &&
              on.maxMotion(kNone) < .05f && on.maxMotion(kUnmarked) < .05f && on.maxMotion(kStale) < .05f,
          "ship record: with the field true the three near blocks ride the ship record (no motion) and are kept, the stale one included");
    check(on.refused(kFar) == 0 && off.maxMotion(kFar) > 0.1f && sameIn(on, off, kFar),
          "ship record: a far pixel with no slot is exactly the camera term it was, unchecked and not refused");
    {
        flatMonoResolveTakeRefusalCensus();
        for (unsigned i = 0; i < 4; ++i) r.frames(true, .01f, true);
        const auto c = steadygpu::takeAll(r.context, 1);
        check(c.skinned == 48 && c.counts[kFlatMonoClassCamera] == 0 && c.counts[kFlatMonoClassStale] == 0,
              "ship record: the census counts the 48 pixels that rode the ship (class skinned, the accepted counter) and refuses none of the blocks");
    }

    // 2. Last frame's depth changed: the surface is not where the ship's record says.
    {
        const Out moved = r.frames(true, .02f);
        check(moved.refused(kNone) == 16 && moved.refused(kUnmarked) == 16 && moved.refused(kStale) == 16 &&
                  moved.maxMotion(kNone) == 0 && moved.maxMotion(kUnmarked) == 0 && moved.maxMotion(kStale) == 0,
              "ship record: with last frame's depth changed the near blocks are refused with no motion (the record's and the camera term's depth checks both fail)");
        flatMonoResolveTakeRefusalCensus();
        for (unsigned i = 0; i < 4; ++i) r.frames(true, .02f, true);
        const auto c = steadygpu::takeAll(r.context, 1);
        check(c.counts[kFlatMonoClassCamera] == 32 && c.counts[kFlatMonoClassStale] == 16 && c.skinned == 0,
              "ship record: the two kind-0 blocks are refused as class camera, the stale block keeps its stale class, and none counts as ship-record");
    }

    // 3. No ship record: too few samples, or no joined record at all.
    {
        r.layout(true);
        const Out fewOff = r.frames(false, .01f);
        const Out few = r.frames(true, .01f);
        check(few.refused(kNone) == 16 && few.refused(kUnmarked) == 16 && few.refused(kStale) == 16 && fewOff.refused(kNone) == 0 && fewOff.refused(kUnmarked) == 0,
              "ship record: with 18 samples (under 32) nothing rides: the near kind-0 blocks fail the camera term's depth check with the bit set and keep it with the bit clear");
        check(sameIn(few, fewOff, kFar) && few.refused(kFar) == 0, "ship record: with too few samples the far block is exactly today's camera term");
        r.layout(false);
        r.setRecords(0);
        const Out noneOff = r.frames(false, .01f);
        const Out none = r.frames(true, .01f);
        check(none.refused(kNone) == 16 && none.refused(kUnmarked) == 16 && none.refused(kStale) == 16 && noneOff.refused(kNone) == 0,
              "ship record: with no joined record anywhere nothing rides, the same as too few samples");
        r.setRecords(1);
    }

    // 4. The field false: the old shader path.
    {
        r.layout(false);
        const Out old = r.frames(false, .01f);
        check(same(old, off), "ship record: the field false gives the same output every time, the old path");
    }
    r.f.steadyDetail = false;
}

// ---- the shader source, one rule flipped at a time -------------------------------------------------------------------------------------
inline bool compileEntry(const std::string& hlsl, const char* entry, std::vector<unsigned char>& bytes) {
    const std::string full = std::string(edvr::kEngineMotionCoreHlsl) + hlsl;
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(full.c_str(), full.size(), "flat_mono_ship_cs", nullptr, nullptr, entry, "cs_5_0", 0, 0, code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr) || !code) {
        std::printf("info: %s compile failed: %s\n", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
        return false;
    }
    const auto* p = static_cast<const unsigned char*>(code->GetBufferPointer());
    bytes.assign(p, p + code->GetBufferSize());
    return true;
}

inline void mutationChecks(Rig& r) {
    struct M { const char* name; const char* from; const char* to; int kernel; };   // kernel: 0 prep, 1 shipCount, 2 shipPick
    static const M mutants[] = {
        {"the prep never takes the ship record", "if((debug.w&32)==0 || !shipNear(depth))return false;", "return false;", 0},
        {"the near camera check is gone", "if(valid && kind==0 && (debug.w&32)!=0 && shipNear(depth)) {", "if(false) {", 0},
        {"the near camera check applies without the bit", "if(valid && kind==0 && (debug.w&32)!=0 && shipNear(depth)) {", "if(valid && kind==0 && shipNear(depth)) {", 0},
        {"the ship record's motion is not validated by last frame's depth", "if(inRange)kept=historyDepthMatches(p,expected);", "if(inRange)kept=true;", 0},
        {"a far pixel rides the ship record", "if((debug.w&32)==0 || !shipNear(depth))return false;", "if((debug.w&32)==0)return false;", 0},
        {"the counting kernel counts an uncertified record",
         "if(engineRecordKind(r,asuint(EN[276].x))!=1)return;\n    if(r.data[18].x!=(0x7FC0ED01u^engineMarkerHash(r,asuint(EN[276].x))))return;\n", "", 1},
        {"the picker has no minimum", "kShipMinSamples=32", "kShipMinSamples=1", 2},
    };
    const std::string shipped = edvr::kFlatMonoShaderSource;
    int caught = 0;
    auto runMutated = [&](const std::string& hlsl, int kernel, int* failed, std::string* first) {
        std::vector<unsigned char> bytes;
        if (!compileEntry(hlsl, kernel == 0 ? "prep" : (kernel == 1 ? "shipCount" : "shipPick"), bytes)) return false;
        ComPtr<ID3D11ComputeShader> probe;
        if (FAILED(r.device->CreateComputeShader(bytes.data(), bytes.size(), nullptr, probe.GetAddressOf()))) return false;
        if (kernel == 0) edvr::flatMonoResolveTestPrepBytecode(bytes.data(), bytes.size());
        else edvr::flatMonoResolveTestShipBytecode(kernel == 2, bytes.data(), bytes.size());
        edvr::flatMonoResolveReset();
        *failed = 0; mutationFailures = failed; mutationFirst.clear();
        shipScenario(r, edvr::FlatMonoResolveMode::Dlss);
        mutationFailures = nullptr; if (first) *first = mutationFirst;
        edvr::flatMonoResolveTestPrepBytecode(nullptr, 0);
        edvr::flatMonoResolveTestShipBytecode(false, nullptr, 0);
        edvr::flatMonoResolveTestShipBytecode(true, nullptr, 0);
        return true;
    };
    {
        int failed = 0; std::string first;
        const bool ran = runMutated(shipped, 0, &failed, &first);
        check(ran && failed == 0, "ship record mutations: control: the unmutated prep, compiled here and run through the test seam, passes the whole scenario");
        failed = 0;
        const bool ranCount = runMutated(shipped, 1, &failed, &first);
        check(ranCount && failed == 0, "ship record mutations: control: the unmutated counting kernel, compiled here, passes the whole scenario");
        failed = 0;
        const bool ranPick = runMutated(shipped, 2, &failed, &first);
        check(ranPick && failed == 0, "ship record mutations: control: the unmutated picker, compiled here, passes the whole scenario");
    }
    for (const M& m : mutants) {
        bool once = false;
        const std::string hlsl = fpgpu::replaceOnce(shipped, m.from, m.to, &once);
        char what[256];
        std::snprintf(what, sizeof(what), "ship record mutations: \"%s\": its anchor is in the shader source exactly once (a moved anchor tests nothing)", m.name);
        check(once, what);
        int failed = 0; std::string first;
        const bool ran = once && runMutated(hlsl, m.kernel, &failed, &first);
        std::snprintf(what, sizeof(what), "ship record mutations: \"%s\" compiles and makes a compute shader", m.name);
        check(ran, what);
        if (!ran) continue;
        std::snprintf(what, sizeof(what), "ship record mutations: \"%s\" is caught by the scenario", m.name);
        check(failed > 0, what);
        if (failed > 0) ++caught;
        std::printf("flat mono resolve: ship-record mutation \"%s\": %d checks fail; first: %s\n", m.name, failed, first.c_str());
    }
    edvr::flatMonoResolveReset();
    std::printf("flat mono resolve: ship-record mutations: %d of %zu caught by the scenario\n", caught, sizeof(mutants) / sizeof(mutants[0]));
    check(size_t(caught) == sizeof(mutants) / sizeof(mutants[0]), "ship record mutations: every mutation was caught");
}
}  // namespace shipgpu

inline void shipRecordGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    const std::string source = edvr::kFlatMonoShaderSource;
    check(source.find("shipRecordBefore") != std::string::npos && source.find("(debug.w&32)") != std::string::npos &&
              source.find("void shipCount(") != std::string::npos && source.find("void shipPick(") != std::string::npos,
          "ship record: the shader carries the helper, the two kernels and the bit (debug.w value 32)");
    shipgpu::Rig rig(device, context);
    shipgpu::shipScenario(rig, edvr::FlatMonoResolveMode::Dlss);
    shipgpu::shipScenario(rig, edvr::FlatMonoResolveMode::Fsr);
    edvr::flatMonoResolveReset();
    shipgpu::mutationChecks(rig);
    edvr::flatMonoResolveReset();
}
