#pragma once
// skin_compose_tests: the compose's half of the second skin (docs/kinematic-motion-injection-2026-09-19.md, "F2 built"), run on WARP from the SAME
// HLSL text the temporal pass ships and the derived blend state's descriptor. Three cases (every check carries a label "C<case>.<what>"; the mutation
// tool tools\skin_engine_test\mutants.py names the case that must catch each mutation):
//   C1  the arithmetic: engineReprojectSkinned (ENGINE_MOTION_HLSL block) against a double reference -- the previous position of a skinned surface is
//       the pixel's world point plus the camera term (nCam - bCam) plus E (previous - current, centimetres in, metres out). E = 0 is the camera
//       term of a point that did not move, bit for bit; the record's own pose blocks are never read; a previous position behind last frame's camera
//       and a projection that is not the pool families' encoding decline.
//   C2  the production mv pass (the full kTemporalCsHlsl, entry "mv") on real ES/EP/scene-depth/SK resources, probe.w bits 2048 | 16384: a skinned
//       pixel with a valid E takes its exact motion; zero E is the camera term; w = 0, a non-finite E, an unwritten texel and a previous position
//       behind the camera keep NO history (MV = the sentinel, MK = 1); a pixel the record does not own declines like any other; a rigid record never
//       reads target 7; with bit 16384 clear target 7 is not read at all; the plain and diagnostics compiles agree; the counters (skinned joined,
//       masked) and the |E| histogram bins count what they should, and the rigid counters count what they did before.
//   C3  the derived blend state: skinMode 0 leaves target 7 as the game's state has it, 1 turns its writes off, 2 writes all four channels unblended;
//       MRT6 keeps its own rule in every mode and every other target is the game's.

#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "math_tests.h"
#include "consumer_tests.h"
#include "../../src/d3d11/engine_velocity_state.h"

namespace skin_compose_tests {
using Microsoft::WRL::ComPtr;
namespace ct = consumer_tests;
namespace mt = math_tests;

struct Harness {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    void (*check)(bool, const char*) = nullptr;
};

// ---- C1 ------------------------------------------------------------------------------------------------------------------
inline void runMath(const Harness& h) {
    const mt::Harness mh{h.device, h.context, h.check};
    const std::string source = mt::engineMotionBlock(mh) + R"HLSL(
struct Case { float2 ndc; float zr; uint slot; float3 e; float skinned; };
StructuredBuffer<Case> Cases : register(t0);
RWStructuredBuffer<float4> Out : register(u0);
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint n, s; Cases.GetDimensions(n, s);
    if (id.x >= n) return;
    Case c = Cases[id.x];
    EnginePoolRecord r = EP[c.slot];
    float4 before;
    bool ok;
    if (c.skinned > 0.5) ok = engineReprojectSkinned(r, c.e, c.ndc, c.zr, before);   // (not a ?: of two calls: HLSL evaluates both and the last out wins)
    else ok = engineReproject(r, c.ndc, c.zr, before);
    Out[id.x] = ok ? float4(before.xy / before.w, before.w, 1) : float4(-99, -99, -1, 0);
}
)HLSL";
    ComPtr<ID3DBlob> code, errors;
    const HRESULT chr = D3DCompile(source.data(), source.size(), "skin-compose-math", nullptr, nullptr, "main", "cs_5_0",
                                   D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(chr) && errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    h.check(SUCCEEDED(chr), "C1.a the shipped engine-motion HLSL, with engineReprojectSkinned, compiles as cs_5_0");
    if (FAILED(chr)) return;
    ComPtr<ID3D11ComputeShader> cs;
    h.check(SUCCEEDED(h.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs)), "C1.b compute shader on WARP");

    const mt::Camera now = mt::camera(10.0, {1200.0, 30.0, -800.0}, 0.0004, -0.0003);
    const mt::Camera before = mt::camera(10.6, {1200.4, 29.9, -800.7}, -0.0002, 0.0005);
    const auto nowRows = now.rows(), beforeRows = before.rows();
    const uint16_t ident[4] = {32767, 32767, 32767, 65534};

    // Two skinned records: the second's previous pose block is hostile (a 40 m jump and a quarter turn). Neither is ever read.
    std::vector<mt::Record> pool(3);
    const mt::V3 npc{1210.0, 28.0, -840.0};
    pool[0].pose(npc, ident, 1.0f, false);
    pool[0].pose(npc, ident, 1.0f, true);
    pool[0].w[0] = 7656u;
    pool[1] = pool[0];
    uint16_t quarter[4];
    quarter[0] = mt::lane(0.0); quarter[1] = mt::lane(0.0); quarter[2] = mt::lane(std::sin(0.7853981634)); quarter[3] = mt::lane(std::cos(0.7853981634));
    pool[1].pose({npc.x + 40.0, npc.y, npc.z}, quarter, 1.0f, true);
    pool[2].pose(npc, ident, 1.0f, false);   // a rigid record that did not move (both blocks alike): the camera term
    pool[2].pose(npc, ident, 1.0f, true);

    struct Case { float ndc[2]; float zr; uint32_t slot; float e[3]; float skinned; };
    struct Expect { double ndc[2]; bool valid; const char* what; };
    std::vector<Case> cases;
    std::vector<Expect> expect;
    // A point of the surface: camera-relative (to the CURRENT origin) position `rel`; it was at rel + (nowOrigin - beforeOrigin) + E before.
    auto addPoint = [&](uint32_t slot, mt::V3 rel, const float eCm[3], bool skinned, const char* what, bool declines = false) {
        double c[4];
        mt::Camera::clip(nowRows, rel, c);
        Case k{{float(c[0] / c[3]), float(c[1] / c[3])}, float(c[2] / c[3]), slot, {eCm[0], eCm[1], eCm[2]}, skinned ? 1.0f : 0.0f};
        // the world point the GPU recovers from the pixel, through the float rows (the reference solves in double from the same rows)
        const ct::V3 solved = ct::solveRel(nowRows, c[0] / c[3], c[1] / c[3], double(nowRows[273 * 4 + 2]) / double(k.zr));
        const mt::V3 world{solved.x, solved.y, solved.z};
        const mt::V3 camTerm{double(nowRows[275 * 4]) - double(beforeRows[275 * 4]), double(nowRows[275 * 4 + 1]) - double(beforeRows[275 * 4 + 1]),
                             double(nowRows[275 * 4 + 2]) - double(beforeRows[275 * 4 + 2])};
        mt::V3 prev{world.x + camTerm.x, world.y + camTerm.y, world.z + camTerm.z};
        if (skinned) prev = {prev.x + double(eCm[0]) * 0.01, prev.y + double(eCm[1]) * 0.01, prev.z + double(eCm[2]) * 0.01};
        double b[4];
        mt::Camera::clip(beforeRows, {prev.x, prev.y, prev.z}, b);
        cases.push_back(k);
        expect.push_back({{b[0] / b[3], b[1] / b[3]}, b[3] > 0 && !declines, what});
    };
    const float zero[3] = {0, 0, 0};
    const float centimetres[3] = {10, -5, 20};   // cm
    const float metres[3] = {-300, 0, 80};       // 3 m sideways, 0.8 m forward
    const float behind[3] = {0, 0, 9000};        // 90 m toward the camera: last frame's point is behind it
    const mt::V3 rels[3] = {{3.0, 1.0, -40.0}, {-12.0, 2.0, -25.0}, {8.0, -4.0, -60.0}};
    for (const mt::V3& rel : rels) {
        addPoint(0, rel, zero, true, "E = 0 is the camera term");
        addPoint(0, rel, centimetres, true, "a few centimetres");
        addPoint(0, rel, metres, true, "metres");
        addPoint(1, rel, centimetres, true, "the record's own previous block is never read");
        addPoint(0, rel, behind, true, "behind last frame's camera: nothing", true);
    }
    // the rigid still record at the same point: the same arithmetic without E
    const size_t rigidAt = cases.size();
    for (const mt::V3& rel : rels) addPoint(2, rel, zero, false, "a still rigid record: the camera term");

    auto structured = [&](const void* data, UINT stride, UINT count, ID3D11ShaderResourceView** srv) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = stride * count; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = stride;
        D3D11_SUBRESOURCE_DATA init{data, 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        h.check(SUCCEEDED(h.device->CreateBuffer(&d, &init, &buffer)), "C1 structured buffer");
        h.check(SUCCEEDED(h.device->CreateShaderResourceView(buffer.Get(), nullptr, srv)), "C1 structured SRV");
    };
    ComPtr<ID3D11ShaderResourceView> poolSrv, caseSrv;
    structured(pool.data(), sizeof(mt::Record), UINT(pool.size()), &poolSrv);
    structured(cases.data(), sizeof(Case), UINT(cases.size()), &caseSrv);
    static_assert(sizeof(Case) == 32, "the HLSL Case is 32 bytes");
    auto constants = [&](const std::array<float, 277 * 4>& rows, ID3D11Buffer** out) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = UINT(rows.size() * 4); d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{rows.data(), 0, 0};
        h.check(SUCCEEDED(h.device->CreateBuffer(&d, &init, out)), "C1 scene constants");
    };
    ComPtr<ID3D11Buffer> nowCb, beforeCb, foreignCb;
    constants(nowRows, &nowCb);
    constants(beforeRows, &beforeCb);
    auto foreign = nowRows;
    foreign[270 * 4 + 2] = 0.5f;
    constants(foreign, &foreignCb);
    D3D11_BUFFER_DESC od{};
    od.ByteWidth = UINT(cases.size() * 16); od.Usage = D3D11_USAGE_DEFAULT; od.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    od.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; od.StructureByteStride = 16;
    ComPtr<ID3D11Buffer> out;
    h.check(SUCCEEDED(h.device->CreateBuffer(&od, nullptr, &out)), "C1 output buffer");
    ComPtr<ID3D11UnorderedAccessView> outUav;
    h.check(SUCCEEDED(h.device->CreateUnorderedAccessView(out.Get(), nullptr, &outUav)), "C1 output UAV");
    od.Usage = D3D11_USAGE_STAGING; od.BindFlags = 0; od.CPUAccessFlags = D3D11_CPU_ACCESS_READ; od.MiscFlags = 0;
    ComPtr<ID3D11Buffer> staging;
    h.check(SUCCEEDED(h.device->CreateBuffer(&od, nullptr, &staging)), "C1 staging buffer");
    auto dispatch = [&](ID3D11Buffer* nowBuffer, std::vector<float>& results) {
        auto* ctx = h.context;
        ctx->CSSetShader(cs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* t0 = caseSrv.Get();
        ID3D11ShaderResourceView* t22 = poolSrv.Get();
        ctx->CSSetShaderResources(0, 1, &t0);
        ctx->CSSetShaderResources(22, 1, &t22);
        ID3D11Buffer* cbs[2] = {nowBuffer, beforeCb.Get()};
        ctx->CSSetConstantBuffers(1, 2, cbs);
        ID3D11UnorderedAccessView* u = outUav.Get();
        ctx->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        ctx->Dispatch(UINT((cases.size() + 63) / 64), 1, 1);
        ctx->CopyResource(staging.Get(), out.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        h.check(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "C1 map results");
        results.assign(static_cast<const float*>(m.pData), static_cast<const float*>(m.pData) + cases.size() * 4);
        ctx->Unmap(staging.Get(), 0);
        ID3D11UnorderedAccessView* none = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
    };
    std::vector<float> results;
    dispatch(nowCb.Get(), results);
    double worst = 0.0;
    unsigned declined = 0, taken = 0;
    for (size_t i = 0; i < cases.size(); ++i) {
        const float* r = &results[i * 4];
        if (!expect[i].valid) {
            h.check(r[3] == 0.0f, "C1.c a previous position behind last frame's camera reprojects to nothing");
            ++declined;
            continue;
        }
        h.check(r[3] == 1.0f && r[2] > 0.0f, "C1.d a skinned point reprojects");
        const double e = std::max(std::fabs(r[0] - expect[i].ndc[0]), std::fabs(r[1] - expect[i].ndc[1]));
        worst = std::max(worst, e);
        if (e > 2e-5) std::fprintf(stderr, "  %s: GPU (%.7f %.7f) vs reference (%.7f %.7f), error %.2e\n", expect[i].what, r[0], r[1], expect[i].ndc[0], expect[i].ndc[1], e);
        h.check(e <= 2e-5, "C1.e previous NDC within 2e-5 of the double reference: world + (nCam - bCam) + E");
        ++taken;
    }
    // E = 0 is the rigid still record's camera term, bit for bit; the hostile record gives the same as the plain one
    for (size_t k = 0; k < 3; ++k) {
        const float* skinned = &results[(k * 5 + 0) * 4];
        const float* rigid = &results[(rigidAt + k) * 4];
        h.check(skinned[0] == rigid[0] && skinned[1] == rigid[1] && skinned[2] == rigid[2], "C1.f E = 0 gives exactly the unmoved rigid record's camera term");
        const float* plain = &results[(k * 5 + 1) * 4];
        const float* hostile = &results[(k * 5 + 3) * 4];
        h.check(plain[0] == hostile[0] && plain[1] == hostile[1] && plain[2] == hostile[2], "C1.g the record's own pose blocks do not enter a skinned point's previous position");
        const float* base = &results[(k * 5 + 0) * 4];
        h.check(std::fabs(plain[0] - base[0]) + std::fabs(plain[1] - base[1]) > 1e-6, "C1.h and E does move it (the check has teeth)");
    }
    std::vector<float> declinedResults;
    dispatch(foreignCb.Get(), declinedResults);
    for (size_t i = 0; i < cases.size(); ++i)
        if (expect[i].valid) h.check(declinedResults[i * 4 + 3] == 0.0f, "C1.i a projection that is not the pool families' encoding declines a skinned point too");
    std::printf("  skin compose C1: %zu cases (%u taken, %u declined), worst previous-NDC error %.2e (limit 2e-5)\n", cases.size(), taken, declined, worst);
    h.context->ClearState();
}

// ---- C2 ------------------------------------------------------------------------------------------------------------------
inline void runConsumer(const Harness& h) {
    const ct::Harness ch{h.device, h.context, h.check};
    const int kDim = 16;
    const std::string hlsl = ct::loadTemporalHlsl(ch);
    const ComPtr<ID3DBlob> plainCode = ct::compileMv(ch, hlsl, false);
    const ComPtr<ID3DBlob> diagCode = ct::compileMv(ch, hlsl, true);
    if (!plainCode || !diagCode) return;
    ComPtr<ID3D11ComputeShader> csPlain, csDiag;
    h.check(SUCCEEDED(h.device->CreateComputeShader(plainCode->GetBufferPointer(), plainCode->GetBufferSize(), nullptr, &csPlain)), "C2.a plain mv creates on WARP");
    h.check(SUCCEEDED(h.device->CreateComputeShader(diagCode->GetBufferPointer(), diagCode->GetBufferSize(), nullptr, &csDiag)), "C2.b diagnostics mv creates on WARP");

    ComPtr<ID3D11ShaderReflection> reflection;
    h.check(SUCCEEDED(D3DReflect(diagCode->GetBufferPointer(), diagCode->GetBufferSize(), __uuidof(ID3D11ShaderReflection), &reflection)), "C2 reflect mv");
    auto* parameters = reflection->GetConstantBufferByName("P");
    h.check(parameters != nullptr, "C2 cbuffer P found by reflection");
    D3D11_SHADER_BUFFER_DESC pd{};
    parameters->GetDesc(&pd);
    std::vector<char> data(pd.Size, 0);
    auto set = [&](const char* name, const void* value, UINT bytes) {
        auto* var = parameters->GetVariableByName(name);
        D3D11_SHADER_VARIABLE_DESC vd{};
        h.check(var && SUCCEEDED(var->GetDesc(&vd)) && vd.StartOffset + bytes <= data.size(), name);
        if (var) std::memcpy(data.data() + vd.StartOffset, value, bytes);
    };
    auto floats = [&](const char* name, float a, float b, float c, float d) { float v[4] = {a, b, c, d}; set(name, v, 16); };
    auto ints2 = [&](const char* name, int a, int b) { int v[2] = {a, b}; set(name, v, 8); };
    auto ints4 = [&](const char* name, int a, int b, int c, int d) { int v[4] = {a, b, c, d}; set(name, v, 16); };
    ints4("region", 0, 0, kDim, kDim);
    ints2("size", kDim, kDim);
    ints2("texSize", kDim, kDim);
    floats("tanNow", -1, 1, -1, 1);
    floats("tanPrev", -1, 1, -1, 1);
    floats("jit", .25f, -.375f, 0, .5f);
    floats("dR0", 1, 0, 0, 0);
    floats("dR1", 0, 1, 0, 0);
    floats("dR2", 0, 0, 1, 0);
    floats("knobs", 0, 1, .025f, 0);
    floats("movers", 0, 0, 0, 0);
    floats("holoJitter", 0, 0, 1, 0);
    ComPtr<ID3D11Buffer> pCb;
    { D3D11_BUFFER_DESC d{}; d.ByteWidth = pd.Size; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER; h.device->CreateBuffer(&d, nullptr, &pCb); }
    auto uploadProbe = [&](float w) { floats("probe", 1, 0, 0, w); h.context->UpdateSubresource(pCb.Get(), 0, nullptr, data.data(), 0, 0); };

    const ct::Camera cam = ct::camera(0.0, {0, 0, 0}, 0.0, 0.0);
    const std::array<float, 277 * 4> camRows = cam.rows(mt::kStampFrame);
    ComPtr<ID3D11Buffer> enebCb;
    { D3D11_BUFFER_DESC d{}; d.ByteWidth = UINT(camRows.size() * 4); d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      D3D11_SUBRESOURCE_DATA init{camRows.data(), 0, 0}; h.device->CreateBuffer(&d, &init, &enebCb); }
    const float kZBg = 0.0005f;
    const double zViewEff = double(camRows[273 * 4 + 2]) / double(kZBg);

    // The pool: 0 a moving JOINED rigid record (pxA, as consumer_tests'), 1 a skinned record (a nonzero palette base, a hostile previous block),
    // 2 an unmoved JOINED rigid record.
    std::vector<ct::Record> pool(4);
    const uint16_t* ident = ct::identLanes();
    const ct::Px pxA{2, 2}, pxC{2, 10}, pxB{2, 6};
    double nx, ny;
    ct::pixelToNdc(pxA.x, pxA.y, kDim, nx, ny);
    const ct::V3 posANow = ct::add(ct::solveRel(camRows, nx, ny, zViewEff), {0, 0, 0});
    const ct::V3 deltaA{2.0, -1.0, 3.0};
    pool[0].pose(posANow, ident, 1.0f, false);
    pool[0].pose(ct::sub(posANow, deltaA), ident, 1.0f, true);
    pool[0].mark(math_tests::ev::kJoined, mt::kStampFrame);
    pool[1].pose(posANow, ident, 1.0f, false);
    pool[1].pose(ct::sub(posANow, deltaA), ident, 1.0f, true);
    pool[1].w[0] = 7656u;
    ct::pixelToNdc(pxC.x, pxC.y, kDim, nx, ny);
    const ct::V3 posC = ct::solveRel(camRows, nx, ny, zViewEff);
    pool[2].pose(posC, ident, 1.0f, false);
    pool[2].pose(posC, ident, 1.0f, true);
    pool[2].mark(math_tests::ev::kJoined, mt::kStampFrame);
    pool[3] = pool[0];
    pool[3].mark(math_tests::ev::kMasked, mt::kStampFrame);   // pxB: a masked rigid record

    // The pixels. ES: x = 2 * slot + 1, y = the depth the draw wrote (the scene's, kZBg). Z: the scene depth, uniform.
    const ct::Px pxK0{4, 4}, pxK1{4, 8}, pxK2{4, 12}, pxK3{8, 4}, pxK4{8, 12}, pxK5{12, 4}, pxK6{12, 8}, pxK7{12, 12}, pxSkin{8, 8};
    std::vector<float> es(size_t(kDim) * kDim * 2, 0.0f), z(size_t(kDim) * kDim, kZBg), g6(size_t(kDim) * kDim * 2, 0.0f);
    for (int i = 0; i < kDim * kDim; ++i) { es[size_t(i) * 2] = -1.0f; g6[size_t(i) * 2] = -1.0f; }
    auto setEs = [&](ct::Px p, float code, float depth) { const size_t i = size_t(p.y) * kDim + p.x; es[i * 2] = code; es[i * 2 + 1] = depth; };
    setEs(pxA, 1.0f, kZBg);                // slot 0: rigid, moving, joined
    setEs(pxB, 7.0f, kZBg);                // slot 3: rigid, masked
    setEs(pxC, 5.0f, kZBg);                // slot 2: rigid, unmoved, joined
    for (ct::Px p : {pxK0, pxK1, pxK2, pxK3, pxK4, pxK5, pxK6, pxSkin}) setEs(p, 3.0f, kZBg);   // slot 1: skinned
    setEs(pxK7, 3.0f, kZBg + 0.25f);       // skinned, but the depth is not the scene's: not owned
    // SK (target 7): xyz = previous - current in centimetres, w = valid. Untouched texels are zero (w = 0).
    std::vector<float> sk(size_t(kDim) * kDim * 4, 0.0f);
    auto setSk = [&](ct::Px p, float x, float y, float zz, float w) { float* t = &sk[(size_t(p.y) * kDim + p.x) * 4]; t[0] = x; t[1] = y; t[2] = zz; t[3] = w; };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    setSk(pxK0, 0, 0, 0, 1);                    // valid, no motion
    setSk(pxK1, -200, 100, -300, 1);            // valid, 3.7 m
    setSk(pxK2, 5, 5, 5, 0);                    // w = 0: no history
    setSk(pxK3, nan, 0, 0, 1);                  // not finite
    setSk(pxK4, 1.3f, 0, 0, 1);                 // valid, 1.3 cm
    setSk(pxK5, 0.005f, 0, 0, 1);               // valid, 0.005 cm
    setSk(pxK6, 0, 0, 9000, 1);                 // valid, but last frame's point is behind the camera
    setSk(pxK7, 50, 0, 0, 1);                   // valid E at a pixel the record does not own
    setSk(pxA, 999, 999, 999, 1);               // garbage under the rigid records: never read
    setSk(pxC, 999, 999, 999, 1);
    setSk(pxB, 999, 999, 999, 1);

    auto makeTexture = [&](DXGI_FORMAT fmt, UINT bind, const void* initData, UINT rowPitch) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = UINT(kDim); td.Height = UINT(kDim); td.Format = fmt;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1; td.BindFlags = bind; td.Usage = D3D11_USAGE_DEFAULT;
        ComPtr<ID3D11Texture2D> tex;
        if (initData) { D3D11_SUBRESOURCE_DATA init{initData, rowPitch, 0}; h.check(SUCCEEDED(h.device->CreateTexture2D(&td, &init, &tex)), "C2 texture"); }
        else h.check(SUCCEEDED(h.device->CreateTexture2D(&td, nullptr, &tex)), "C2 texture");
        return tex;
    };
    ComPtr<ID3D11ShaderResourceView> epSrv, esSrv, zSrv, g6Srv, skSrv;
    {
        D3D11_BUFFER_DESC d{}; d.ByteWidth = UINT(pool.size() * sizeof(ct::Record)); d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = sizeof(ct::Record);
        D3D11_SUBRESOURCE_DATA init{pool.data(), 0, 0};
        ComPtr<ID3D11Buffer> b;
        h.device->CreateBuffer(&d, &init, &b);
        h.device->CreateShaderResourceView(b.Get(), nullptr, &epSrv);
    }
    const auto esTex = makeTexture(DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, es.data(), UINT(kDim) * 8);
    const auto zTex = makeTexture(DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, z.data(), UINT(kDim) * 4);
    const auto g6Tex = makeTexture(DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, g6.data(), UINT(kDim) * 8);
    const auto skTex = makeTexture(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, sk.data(), UINT(kDim) * 16);
    h.device->CreateShaderResourceView(esTex.Get(), nullptr, &esSrv);
    h.device->CreateShaderResourceView(zTex.Get(), nullptr, &zSrv);
    h.device->CreateShaderResourceView(g6Tex.Get(), nullptr, &g6Srv);
    h.device->CreateShaderResourceView(skTex.Get(), nullptr, &skSrv);
    const auto mvTex = makeTexture(DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    const auto zcTex = makeTexture(DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    const auto mkTex = makeTexture(DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    ComPtr<ID3D11UnorderedAccessView> mvUav, zcUav, mkUav, statsUav;
    h.device->CreateUnorderedAccessView(mvTex.Get(), nullptr, &mvUav);
    h.device->CreateUnorderedAccessView(zcTex.Get(), nullptr, &zcUav);
    h.device->CreateUnorderedAccessView(mkTex.Get(), nullptr, &mkUav);
    const UINT kStatsN = 96;   // every index mv's diagnostics write: 0..89
    const std::vector<uint32_t> zeroStats(kStatsN, 0);
    ComPtr<ID3D11Buffer> statsBuf;
    { D3D11_BUFFER_DESC d{}; d.ByteWidth = kStatsN * 4; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
      d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = 4;
      D3D11_SUBRESOURCE_DATA init{zeroStats.data(), 0, 0}; h.device->CreateBuffer(&d, &init, &statsBuf); }
    h.device->CreateUnorderedAccessView(statsBuf.Get(), nullptr, &statsUav);

    auto readTex = [&](ID3D11Texture2D* tex, UINT comps) {
        D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        h.device->CreateTexture2D(&td, nullptr, &staging);
        h.context->CopyResource(staging.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE m{};
        h.check(SUCCEEDED(h.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "C2 map texture");
        std::vector<float> out(size_t(kDim) * kDim * comps);
        for (int y = 0; y < kDim; ++y) std::memcpy(out.data() + size_t(y) * kDim * comps, static_cast<const char*>(m.pData) + size_t(y) * m.RowPitch, size_t(kDim) * comps * 4);
        h.context->Unmap(staging.Get(), 0);
        return out;
    };
    auto readStats = [&]() {
        D3D11_BUFFER_DESC d{}; statsBuf->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0; d.StructureByteStride = 0;
        ComPtr<ID3D11Buffer> staging;
        h.device->CreateBuffer(&d, nullptr, &staging);
        h.context->CopyResource(staging.Get(), statsBuf.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        h.check(SUCCEEDED(h.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "C2 map stats");
        std::vector<uint32_t> out(static_cast<const uint32_t*>(m.pData), static_cast<const uint32_t*>(m.pData) + kStatsN);
        h.context->Unmap(staging.Get(), 0);
        return out;
    };
    auto mvAt = [&](const std::vector<float>& arr, ct::Px p) { const size_t i = (size_t(p.y) * kDim + p.x) * 2; return std::pair<float, float>{arr[i], arr[i + 1]}; };
    auto mkAt = [&](const std::vector<float>& arr, ct::Px p) { return arr[size_t(p.y) * kDim + p.x]; };

    h.context->CSSetConstantBuffers(0, 1, pCb.GetAddressOf());
    ID3D11Buffer* enEb[2] = {enebCb.Get(), enebCb.Get()};
    h.context->CSSetConstantBuffers(1, 2, enEb);
    h.context->CSSetShaderResources(2, 1, zSrv.GetAddressOf());
    ID3D11ShaderResourceView* esEp[2] = {esSrv.Get(), epSrv.Get()};
    h.context->CSSetShaderResources(21, 2, esEp);
    h.context->CSSetShaderResources(19, 1, g6Srv.GetAddressOf());
    h.context->CSSetShaderResources(23, 1, skSrv.GetAddressOf());
    ID3D11UnorderedAccessView* uavs[4] = {statsUav.Get(), mvUav.Get(), zcUav.Get(), mkUav.Get()};
    h.context->CSSetUnorderedAccessViews(2, 4, uavs, nullptr);
    auto dispatchAndRead = [&](ID3D11ComputeShader* cs, float probeW) {
        uploadProbe(probeW);
        h.context->CSSetShader(cs, nullptr, 0);
        h.context->Dispatch((kDim + 7) / 8, (kDim + 7) / 8, 1);
        return std::make_pair(readTex(mvTex.Get(), 2), readTex(mkTex.Get(), 1));
    };

    const float kBoth = 2048.0f + 16384.0f;
    h.context->UpdateSubresource(statsBuf.Get(), 0, nullptr, zeroStats.data(), 0, 0);
    const auto armed = dispatchAndRead(csDiag.Get(), kBoth);
    const std::vector<uint32_t> stats = readStats();
    const auto armedPlain = dispatchAndRead(csPlain.Get(), kBoth);
    const auto rigidOnly = dispatchAndRead(csDiag.Get(), 2048.0f);      // bit 16384 clear: target 7 is not read
    const auto baseline = dispatchAndRead(csDiag.Get(), 0.0f);          // the no-engine camera term
    const std::vector<float>& mv = armed.first;
    const std::vector<float>& mk = armed.second;

    auto expectPrevious = [&](ct::Px p, const ct::V3& moveM, const char* what) {
        double nx2, ny2;
        ct::pixelToNdc(p.x, p.y, kDim, nx2, ny2);
        const ct::V3 rel = ct::solveRel(camRows, nx2, ny2, zViewEff);
        const ct::V3 prev = ct::add(rel, moveM);
        double before[4];
        ct::Camera::clip(camRows, prev, before);
        double ppX, ppY;
        ct::clipToPixel(before, kDim, ppX, ppY);
        const auto m = mvAt(mv, p);
        const double gotX = double(p.x) + double(m.first), gotY = double(p.y) + double(m.second);
        const double err = std::max(std::fabs(gotX - ppX), std::fabs(gotY - ppY));
        if (err > 1e-3) std::fprintf(stderr, "  %s: GPU previous pixel (%.6f %.6f) vs double reference (%.6f %.6f), error %.2e\n", what, gotX, gotY, ppX, ppY, err);
        h.check(err <= 1e-3 && mkAt(mk, p) != 1.0f, what);
        return std::pair<double, double>{ppX - double(p.x), ppY - double(p.y)};
    };
    auto sentinel = [&](ct::Px p, const char* what) {
        const auto m = mvAt(mv, p);
        h.check(m.first == float(kDim) * 2.0f && m.second == float(kDim) * 2.0f && mkAt(mk, p) == 1.0f, what);
    };
    auto sameTwo = [&](const std::pair<std::vector<float>, std::vector<float>>& x, const std::pair<std::vector<float>, std::vector<float>>& y, ct::Px p, const char* what) {
        const auto a = mvAt(x.first, p), b = mvAt(y.first, p);
        h.check(a.first == b.first && a.second == b.second && mkAt(x.second, p) == mkAt(y.second, p), what);
    };
    auto sameAs = [&](const std::pair<std::vector<float>, std::vector<float>>& other, ct::Px p, const char* what) { sameTwo(armed, other, p, what); };

    // a skinned pixel with a valid E takes its exact motion
    const auto k1 = expectPrevious(pxK1, {-2.0, 1.0, -3.0}, "C2.c a skinned pixel with a valid E (3.7 m) takes its exact previous pixel, within 1e-3 px of the double reference");
    const auto k1base = mvAt(baseline.first, pxK1);
    h.check(std::fabs(k1.first - double(k1base.first)) + std::fabs(k1.second - double(k1base.second)) > 0.1, "C2.d (and that motion is not the camera term: the case has teeth)");
    expectPrevious(pxK4, {0.013, 0.0, 0.0}, "C2.e a 1.3 cm E is applied too");
    // E = 0: the camera term (the no-engine baseline: the camera is static here)
    {
        const auto a = mvAt(mv, pxK0), b = mvAt(baseline.first, pxK0);
        h.check(std::fabs(a.first - b.first) <= 1e-2 && std::fabs(a.second - b.second) <= 1e-2 && mkAt(mk, pxK0) != 1.0f, "C2.f a skinned pixel with E = 0 keeps the camera term (no motion), history kept");
    }
    // no valid E: no history
    sentinel(pxK2, "C2.g w = 0: the pixel keeps no history (MV the sentinel, MK 1)");
    sentinel(pxK3, "C2.h a non-finite E keeps no history");
    sentinel(pxSkin, "C2.i a skinned pixel whose target-7 texel was never written (zero, w = 0) keeps no history");
    sentinel(pxK6, "C2.j a previous position behind last frame's camera keeps no history");
    // not owned: declines like the baseline
    sameAs(baseline, pxK7, "C2.k a skinned record's pixel the record does not own (stale depth) declines to the no-engine result, target 7 unread");
    // rigid records are what they were, and never read target 7
    sameAs(rigidOnly, pxA, "C2.l a rigid moving record is byte-identical with bit 16384 set (target 7 holds garbage under it and is not read)");
    sameAs(rigidOnly, pxC, "C2.m an unmoved rigid record is byte-identical with bit 16384 set");
    sameAs(rigidOnly, pxB, "C2.n a masked rigid record is byte-identical with bit 16384 set");
    {
        const auto m = mvAt(mv, pxB);
        h.check(m.first == float(kDim) * 2.0f && mkAt(mk, pxB) == 1.0f, "C2.o (and it is still masked)");
    }
    // bit 16384 clear: a skinned record is not a rig record (no marker): the camera term, as before this build
    for (ct::Px p : {pxK0, pxK1, pxK2, pxK3, pxK4, pxK5, pxK6, pxSkin}) sameTwo(rigidOnly, baseline, p, "C2.p with bit 16384 clear a skinned record's pixel declines to the no-engine result: target 7 is not read");
    // the plain and diagnostics compiles agree
    for (ct::Px p : {pxA, pxB, pxC, pxK0, pxK1, pxK2, pxK3, pxK4, pxK5, pxK6, pxK7, pxSkin}) {
        const auto a = mvAt(armedPlain.first, p), b = mvAt(mv, p);
        h.check(a.first == b.first && a.second == b.second && mkAt(armedPlain.second, p) == mkAt(mk, p), "C2.q the plain and diagnostics compiles of mv agree at every constructed pixel");
    }
    // the counters: joined 50, masked 51, stale 53; the skinned pixels 56 (joined) and 57 (masked); bins 58..89
    h.check(stats[50] == 2 + 4 && stats[51] == 1 + 4, "C2.r the engine counters count the skinned pixels with the rigid ones: joined A, C and the four with E; masked B and the four without");
    h.check(stats[56] == 4, "C2.s Stats[56] counts the skinned pixels that took their E (K0, K1, K4, K5)");
    h.check(stats[57] == 4, "C2.t Stats[57] counts the skinned pixels with no history (K2, K3, K6, and the unwritten one)");
    h.check(stats[52] == 0 && stats[53] == 1, "C2.u a skinned record is not 'not a rig record', and the stale one is counted as stale");
    unsigned binned = 0;
    for (unsigned k = 0; k < 32; ++k) binned += stats[58 + k];
    h.check(binned == 4 && stats[58 + 0] == 1 && stats[58 + 1] == 1 && stats[58 + 9] == 1 && stats[58 + 19] == 1,
            "C2.v the |E| histogram: 0 -> bin 0, 0.005 cm -> bin 1, 1.3 cm -> bin 9, 374 cm -> bin 19, nothing else");
    std::printf("  skin compose C2: skinned pixels joined %u masked %u, histogram bins 0/1/9/19 one each; rigid records unchanged\n", stats[56], stats[57]);
    h.context->ClearState();
}

// ---- C3 ------------------------------------------------------------------------------------------------------------------
inline void runBlend(const Harness& h) {
    using edvr::engineVelocityDerivedBlend;
    auto rt = [](BOOL enable, D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND_OP op, UINT mask) {
        D3D11_RENDER_TARGET_BLEND_DESC t{};
        t.BlendEnable = enable; t.SrcBlend = src; t.DestBlend = dst; t.BlendOp = op; t.SrcBlendAlpha = src; t.DestBlendAlpha = dst; t.BlendOpAlpha = op;
        t.RenderTargetWriteMask = UINT8(mask);
        return t;
    };
    std::vector<D3D11_BLEND_DESC> games;
    games.push_back(edvr::engineVelocityDefaultBlend());
    {
        D3D11_BLEND_DESC d = edvr::engineVelocityDefaultBlend();
        d.RenderTarget[0] = rt(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL);
        games.push_back(d);   // additive on target 0, independent off: every target inherits it
    }
    {
        D3D11_BLEND_DESC d = edvr::engineVelocityDefaultBlend();
        d.IndependentBlendEnable = TRUE;
        d.RenderTarget[0] = rt(TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL);
        d.RenderTarget[6] = rt(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL);
        d.RenderTarget[7] = rt(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX, D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_BLUE);
        games.push_back(d);   // independent, with its own ideas about targets 6 and 7
    }
    unsigned checked = 0;
    for (const D3D11_BLEND_DESC& game : games) {
        for (int mode = 0; mode < 3; ++mode) {
            const D3D11_BLEND_DESC d = engineVelocityDerivedBlend(game, mode);
            const auto& t6 = d.RenderTarget[edvr::kEngineVelocityTarget];
            h.check(d.IndependentBlendEnable && !t6.BlendEnable && t6.RenderTargetWriteMask == (D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN) &&
                    t6.SrcBlend == D3D11_BLEND_ONE && t6.DestBlend == D3D11_BLEND_ZERO,
                    "C3.a MRT6 keeps its rule in every skin mode: unblended, R and G");
            const auto& t7 = d.RenderTarget[edvr::kSkinTarget];
            if (mode == 0) {
                const D3D11_RENDER_TARGET_BLEND_DESC& was = game.IndependentBlendEnable ? game.RenderTarget[7] : game.RenderTarget[0];
                h.check(t7.BlendEnable == was.BlendEnable && t7.SrcBlend == was.SrcBlend && t7.DestBlend == was.DestBlend && t7.BlendOp == was.BlendOp &&
                        t7.RenderTargetWriteMask == was.RenderTargetWriteMask,
                        "C3.b skin mode 0 leaves target 7 exactly as the game's state has it");
            } else if (mode == 1) {
                h.check(!t7.BlendEnable && t7.RenderTargetWriteMask == 0, "C3.c skin mode 1 turns target 7's writes off");
            } else {
                h.check(!t7.BlendEnable && t7.RenderTargetWriteMask == D3D11_COLOR_WRITE_ENABLE_ALL && t7.SrcBlend == D3D11_BLEND_ONE && t7.DestBlend == D3D11_BLEND_ZERO &&
                        t7.BlendOp == D3D11_BLEND_OP_ADD, "C3.d skin mode 2 writes target 7's four channels unblended");
            }
            for (UINT i = 0; i < 6; ++i) {
                const D3D11_RENDER_TARGET_BLEND_DESC& was = game.IndependentBlendEnable ? game.RenderTarget[i] : game.RenderTarget[0];
                const auto& got = d.RenderTarget[i];
                h.check(got.BlendEnable == was.BlendEnable && got.SrcBlend == was.SrcBlend && got.DestBlend == was.DestBlend && got.BlendOp == was.BlendOp &&
                        got.RenderTargetWriteMask == was.RenderTargetWriteMask,
                        "C3.e the game's own targets 0..5 are untouched in every skin mode");
            }
            ++checked;
        }
    }
    // through the device: the state object carries the descriptor
    {
        const char* refused = nullptr;
        ComPtr<ID3D11BlendState> two = edvr::engineVelocityCreateDerivedBlend(h.device, nullptr, &refused, 2);
        D3D11_BLEND_DESC got{};
        if (two) two->GetDesc(&got);
        h.check(two && !refused && got.RenderTarget[edvr::kSkinTarget].RenderTargetWriteMask == D3D11_COLOR_WRITE_ENABLE_ALL, "C3.f the device's state object for skin mode 2 writes target 7");
        ComPtr<ID3D11BlendState> one = edvr::engineVelocityCreateDerivedBlend(h.device, nullptr, &refused, 1);
        if (one) one->GetDesc(&got);
        h.check(one && got.RenderTarget[edvr::kSkinTarget].RenderTargetWriteMask == 0, "C3.g and for skin mode 1 it does not");
        ComPtr<ID3D11BlendState> zero = edvr::engineVelocityCreateDerivedBlend(h.device, nullptr, &refused, 0);
        if (zero) zero->GetDesc(&got);
        h.check(zero && got.RenderTarget[edvr::kSkinTarget].RenderTargetWriteMask == D3D11_COLOR_WRITE_ENABLE_ALL, "C3.h and for skin mode 0 it is the game's (the default writes all)");
    }
    std::printf("  skin compose C3: %u derived descriptors (3 game states x 3 modes)\n", checked);
}

}  // namespace skin_compose_tests
