#pragma once
// engine_velocity_test: the DXBC patcher, end to end on WARP.
//
// Three synthetic families mirror the real pool families' signatures:
//   A  DATAID.y identity (vs_EB52 -> ps_CB9F/9ABF/3434): no SV_Position in the PS
//   B  FACEINVARIANT.x identity, SV_Position read by the PS (vs_BBE5 -> ps_DB3E)
//   C  UV-only (vs_5B4D -> ps_4375): the VS exports no slot, so it is patched too
// Each is compiled, patched, disassembled, reflected, created and DRAWN: the
// MRT6 readback must hold the exact odd slot code (2 * slot + 1) and the exact
// depth bits the fragment wrote, the cleared value elsewhere, and a later
// UNPATCHED occluder must leave a stale slot the depth-equality test rejects.
//
// blendChecks (the 2026-09-23 review, item 4): family A under the game's
// blend states -- overwrite, additive, blend factor, a partial write mask,
// independent per-target blending, MAX, a blend change between two draws,
// and depth-write-disabled passes. Under the DERIVED state
// (engine_velocity_state.h) the game's four targets must be bit-identical to
// the stock pair's under the game's state, and MRT6 exact. Under the
// INHERITED state (what phase 1 did) the consumer's decode must decline the
// corrupted pixels, never name another record.

#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/d3d11/dxbc_engine_velocity.h"
#include "../../src/d3d11/engine_velocity_state.h"

namespace shader_tests {
using Microsoft::WRL::ComPtr;

// The pool record the synthetic VS reads: base u32 (0), scale f32 (4),
// packed quaternion 4x u16 (8), position f32x3 (16) -- the game's layout.
constexpr char kVsCommon[] = R"HLSL(
struct PoolRecord { uint4 data[21]; };
StructuredBuffer<PoolRecord> Pool : register(t33);
cbuffer Scene : register(b1) { float4 scene[276]; };
float3 turn(float4 q, float3 v) { return (2*q.w*q.w-1)*v + 2*dot(q.xyz,v)*q.xyz + 2*q.w*cross(q.xyz,v); }
float4 place(uint slot, float3 v) {
    PoolRecord r = Pool[slot];
    float scale = asfloat(r.data[0].y);
    uint2 packed = r.data[0].zw;
    float4 q = float4(packed.x & 65535, packed.x >> 16, packed.y & 65535, packed.y >> 16) * (1.0/32767.0) - 1;
    float3 world = asfloat(r.data[1].xyz) - scene[275].xyz + scale * turn(q, v);
    return world.x*scene[270] + world.y*scene[271] + world.z*scene[272] + scene[273];
}
struct VsIn { uint2 inst : INSTANCEANDMODELDATAINDEX; float3 v : PACKEDVERTEXDATAA; };
)HLSL";

constexpr char kVsA[] = R"HLSL(
struct VsOut {
    nointerpolation uint3 id : __USER_MATERIALMODULATION_DATAID;
    float3 n : __USER_VERTEX_M_LIGHTINGNORMAL;
    float3 t : __USER_VERTEX_M_LIGHTINGTANGENT;
    float2 uv : __USER_VERTEX_M_TEXCOORD;
    float4 p : SV_POSITION;
};
VsOut main(VsIn i) {
    VsOut o;
    o.id = uint3(7, (i.inst.x & 0x7fffffff) | 0x80000000, 3);
    o.n = float3(0, 0, 1); o.t = float3(1, 0, 0); o.uv = i.v.xy;
    o.p = place(i.inst.x, i.v);
    return o;
}
)HLSL";
constexpr char kPsA[] = R"HLSL(
struct PsIn {
    nointerpolation uint3 id : __USER_MATERIALMODULATION_DATAID;
    float3 n : __USER_VERTEX_M_LIGHTINGNORMAL;
    float3 t : __USER_VERTEX_M_LIGHTINGTANGENT;
    float2 uv : __USER_VERTEX_M_TEXCOORD;
};
struct PsOut { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; float4 d : SV_Target3; };
PsOut main(PsIn i) {
    PsOut o;
    o.a = float4(i.n, 1); o.b = float4(i.t, (i.id.y & 1) ? 1 : 0); o.c = float4(i.uv, 0, 1); o.d = 0;
    return o;
}
)HLSL";

constexpr char kVsB[] = R"HLSL(
struct VsOut {
    nointerpolation uint id : __USER_VERTEX_FACEINVARIANT;
    float3 n : __USER_VERTEX_M_LIGHTINGNORMAL;
    float3 t : __USER_VERTEX_M_LIGHTINGTANGENT;
    float2 uv : __USER_VERTEX_M_TEXCOORD;
    float4 p : SV_POSITION;
};
VsOut main(VsIn i) {
    VsOut o;
    o.id = (i.inst.x & 0x7fffffff) | 0x80000000;
    o.n = float3(0, 0, 1); o.t = float3(1, 0, 0); o.uv = i.v.xy;
    o.p = place(i.inst.x, i.v);
    return o;
}
)HLSL";
constexpr char kPsB[] = R"HLSL(
struct PsIn {
    nointerpolation uint id : __USER_VERTEX_FACEINVARIANT;
    float3 n : __USER_VERTEX_M_LIGHTINGNORMAL;
    float3 t : __USER_VERTEX_M_LIGHTINGTANGENT;
    float2 uv : __USER_VERTEX_M_TEXCOORD;
    float4 p : SV_POSITION;
};
struct PsOut { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; float4 d : SV_Target3; };
PsOut main(PsIn i) {
    PsOut o;
    o.a = float4(i.n, frac(i.p.x * 0.01)); o.b = float4(i.t, i.id & 1); o.c = float4(i.uv, 0, 1); o.d = 0;
    return o;
}
)HLSL";
// A PS can use the VS position's register for a rasterizer-generated input.
// The patch must add its own SV_Position in a free PS register, preserving the
// original front-face value and all four colour outputs.
constexpr char kPsFrontFace[] = R"HLSL(
struct PsIn {
    nointerpolation uint id : __USER_VERTEX_FACEINVARIANT;
    float3 n : __USER_VERTEX_M_LIGHTINGNORMAL;
    float3 t : __USER_VERTEX_M_LIGHTINGTANGENT;
    float2 uv : __USER_VERTEX_M_TEXCOORD;
    bool front : SV_IsFrontFace;
};
struct PsOut { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; float4 d : SV_Target3; };
PsOut main(PsIn i) {
    PsOut o;
    o.a = float4(i.n, i.front ? 1 : 0); o.b = float4(i.t, i.id & 1);
    o.c = float4(i.uv, 0, 1); o.d = 0;
    return o;
}
)HLSL";

constexpr char kVsC[] = R"HLSL(
struct VsOut { float2 uv : __USER_VERTEX_M_TEXCOORD; float4 p : SV_POSITION; };
VsOut main(VsIn i) {
    VsOut o;
    o.uv = i.v.xy;
    o.p = place(i.inst.x, i.v);
    return o;
}
)HLSL";
constexpr char kPsC[] = R"HLSL(
struct PsIn { float2 uv : __USER_VERTEX_M_TEXCOORD; };
struct PsOut { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; float4 d : SV_Target3; };
PsOut main(PsIn i) { PsOut o; o.a = float4(i.uv, 0, 1); o.b = 1; o.c = 0.5; o.d = 0; return o; }
)HLSL";

struct Harness {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    void (*check)(bool, const char*) = nullptr;
};

inline ComPtr<ID3DBlob> compile(const Harness& h, const std::string& source, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), "engine-velocity-test", nullptr, nullptr, "main",
                                  profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                  &code, &errors);
    if (FAILED(hr) && errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    h.check(SUCCEEDED(hr), "D3DCompile of a synthetic family shader");
    return code;
}

inline std::string disassemble(const std::vector<BYTE>& bytes) {
    ComPtr<ID3DBlob> text;
    if (FAILED(D3DDisassemble(bytes.data(), bytes.size(), 0, nullptr, &text))) return {};
    return std::string(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize());
}

// The t33 record: 336 bytes, only the pose head matters here.
struct Record { uint32_t words[84]; };
static_assert(sizeof(Record) == 336, "t33 stride");

inline uint16_t lane(float c) { return static_cast<uint16_t>(std::lround((c + 1.0f) * 32767.0f)); }
inline Record makeRecord(float x, float y, float z, float scale, const float q[4]) {
    Record r{};
    std::memcpy(&r.words[1], &scale, 4);
    r.words[2] = uint32_t(lane(q[0])) | (uint32_t(lane(q[1])) << 16);
    r.words[3] = uint32_t(lane(q[2])) | (uint32_t(lane(q[3])) << 16);
    std::memcpy(&r.words[4], &x, 4);
    std::memcpy(&r.words[5], &y, 4);
    std::memcpy(&r.words[6], &z, 4);
    return r;
}

struct Family {
    const char* name;
    const char* vs;
    const char* ps;
    uint32_t identityRegister, identityComponent, positionRegister;
    bool slotFromVsPatch;
};

inline void patchStaticChecks(const Harness& h, const Family& f, const std::vector<BYTE>& vs,
                              const std::vector<BYTE>& ps, edvr::EngineVelocityInputs& in,
                              std::vector<BYTE>& patchedVs, std::vector<BYTE>& patchedPs) {
    std::string why;
    h.check(edvr::engineVelocityDeriveInputs(vs.data(), vs.size(), in, why), why.c_str());
    h.check(in.identityRegister == f.identityRegister && in.identityComponent == f.identityComponent &&
            in.positionRegister == f.positionRegister && in.slotFromVsPatch == f.slotFromVsPatch,
            "derived slot/position registers match the family");
    patchedVs = vs;
    if (in.slotFromVsPatch) {
        h.check(edvr::engineVelocityPatchVs(vs.data(), vs.size(), in, patchedVs, why), why.c_str());
        const std::string text = disassemble(patchedVs);
        h.check(text.find(edvr::kEngineVelocitySlotSemantic) != std::string::npos, "patched VS signature names EDVRPOOLSLOT");
        h.check(text.find("mov o2.x, v0.x") != std::string::npos, "patched VS exports v0.x");
    } else {
        std::vector<BYTE> none;
        h.check(!edvr::engineVelocityPatchVs(vs.data(), vs.size(), in, none, why) && none.empty(),
                "a family that exports its slot gets no VS patch");
    }
    h.check(edvr::engineVelocityPatchPs(ps.data(), ps.size(), in, patchedPs, why), why.c_str());
    const std::string text = disassemble(patchedPs);
    h.check(text.find("dcl_output o6.xy") != std::string::npos, "patched PS declares o6.xy");
    h.check(text.find("utof o6.x") != std::string::npos && text.find("mov o6.y") != std::string::npos,
            "patched PS writes the slot and the depth");
    h.check(text.find("0x007fffff") != std::string::npos || text.find("l(8388607)") != std::string::npos,
            "patched PS keeps the slot's low 23 bits");
    h.check(text.find("imad") != std::string::npos && text.find("l(2), l(1)") != std::string::npos,
            "patched PS writes the slot odd: 2 * slot + 1");
    ComPtr<ID3D11ShaderReflection> reflect;
    h.check(SUCCEEDED(D3DReflect(patchedPs.data(), patchedPs.size(), IID_PPV_ARGS(&reflect))), "D3DReflect of the patched PS");
    D3D11_SHADER_DESC desc{};
    reflect->GetDesc(&desc);
    bool target6 = false;
    for (UINT i = 0; i < desc.OutputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p{};
        reflect->GetOutputParameterDesc(i, &p);
        if (p.SemanticIndex == 6 && p.Register == 6 && p.Mask == 3 && p.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32)
            target6 = true;
    }
    h.check(target6, "reflection shows SV_Target6.xy float");
    if (std::strstr(f.name, "FACEINVARIANT.x + SV_Position")) {
        std::vector<BYTE> guarded;
        h.check(edvr::engineVelocityPatchPs(ps.data(), ps.size(), in, guarded, why, true), why.c_str());
        const std::string g = disassemble(guarded);
        h.check(g.find("dcl_resource_texture2d") != std::string::npos &&
                g.find("t3") != std::string::npos && g.find("ld_indexable") != std::string::npos &&
                g.find("movc o6.y") != std::string::npos,
                "guarded overlay PS reads snapshot and chooses depth per pixel");
        ComPtr<ID3D11PixelShader> warpGuarded;
        h.check(SUCCEEDED(h.device->CreatePixelShader(guarded.data(), guarded.size(), nullptr, &warpGuarded)),
                "WARP accepts guarded overlay PS");
    }
    // Idempotence guard: patching the patched PS again must decline, not stack.
    std::vector<BYTE> twice;
    h.check(!edvr::engineVelocityPatchPs(patchedPs.data(), patchedPs.size(), in, twice, why), "a patched PS is not patched twice");
    // A corrupt container declines without output.
    auto bad = ps;
    bad[40] ^= 0x5a;
    std::vector<BYTE> out;
    h.check(!edvr::engineVelocityPatchPs(bad.data(), bad.size(), in, out, why) && out.empty(), "corrupt DXBC declines");
}

inline void drawChecks(const Harness& h, const Family& f, const std::vector<BYTE>& vsOriginal,
                       const std::vector<BYTE>& psOriginal, const std::vector<BYTE>& vsPatched,
                       const std::vector<BYTE>& psPatched) {
    auto* dev = h.device;
    auto* ctx = h.context;
    constexpr UINT W = 64, H = 64;
    ComPtr<ID3D11VertexShader> vs, vsOrig;
    ComPtr<ID3D11PixelShader> ps, psOrig;
    h.check(SUCCEEDED(dev->CreateVertexShader(vsPatched.data(), vsPatched.size(), nullptr, &vs)), "patched VS created on WARP");
    h.check(SUCCEEDED(dev->CreatePixelShader(psPatched.data(), psPatched.size(), nullptr, &ps)), "patched PS created on WARP");
    h.check(SUCCEEDED(dev->CreateVertexShader(vsOriginal.data(), vsOriginal.size(), nullptr, &vsOrig)), "original VS created");
    h.check(SUCCEEDED(dev->CreatePixelShader(psOriginal.data(), psOriginal.size(), nullptr, &psOrig)), "original PS created");
    const D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        {"INSTANCEANDMODELDATAINDEX", 0, DXGI_FORMAT_R32G32_UINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"PACKEDVERTEXDATAA", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ComPtr<ID3D11InputLayout> layout;
    h.check(SUCCEEDED(dev->CreateInputLayout(layoutDesc, 2, vsOriginal.data(), vsOriginal.size(), &layout)), "input layout");

    // Pool: slot 5 near, slot 9 farther and turned 90 degrees about z, slot 12 the occluder.
    std::vector<Record> pool(16);
    const float identity[4] = {0, 0, 0, 1};
    const float s = std::sqrt(0.5f);
    const float turned[4] = {0, 0, s, s};
    pool[5] = makeRecord(-0.5f, 0.0f, -2.0f, 1.0f, identity);
    pool[9] = makeRecord(0.5f, 0.0f, -4.0f, 1.0f, turned);
    pool[12] = makeRecord(-0.5f, 0.0f, -1.0f, 0.5f, identity);
    D3D11_BUFFER_DESC pd{};
    pd.ByteWidth = UINT(pool.size() * sizeof(Record));
    pd.Usage = D3D11_USAGE_DEFAULT;
    pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    pd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    pd.StructureByteStride = sizeof(Record);
    D3D11_SUBRESOURCE_DATA pinit{pool.data(), 0, 0};
    ComPtr<ID3D11Buffer> poolBuffer;
    h.check(SUCCEEDED(dev->CreateBuffer(&pd, &pinit, &poolBuffer)), "pool buffer");
    ComPtr<ID3D11ShaderResourceView> poolSrv;
    h.check(SUCCEEDED(dev->CreateShaderResourceView(poolBuffer.Get(), nullptr, &poolSrv)), "pool SRV");

    // Scene constants: an infinite reversed-Z perspective (the game's
    // 0.025/w form), camera at the origin looking down -z.
    std::vector<float> scene(276 * 4, 0.0f);
    auto reg = [&](int r, float x, float y, float z, float w) { scene[r*4] = x; scene[r*4+1] = y; scene[r*4+2] = z; scene[r*4+3] = w; };
    reg(270, 1.0f, 0, 0, 0);          // clip.x = x
    reg(271, 0, 1.0f, 0, 0);          // clip.y = y
    reg(272, 0, 0, 0, -1.0f);         // clip.w = -z
    reg(273, 0, 0, 0.025f, 0);        // clip.z = 0.025
    reg(275, 0, 0, 0, 0);
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = UINT(scene.size() * 4);
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cinit{scene.data(), 0, 0};
    ComPtr<ID3D11Buffer> sceneCb;
    h.check(SUCCEEDED(dev->CreateBuffer(&cd, &cinit, &sceneCb)), "scene CB");

    const float quad[] = {-0.4f, -0.4f, 0, 0.4f, -0.4f, 0, -0.4f, 0.4f, 0, 0.4f, 0.4f, 0};
    D3D11_BUFFER_DESC vd{};
    vd.ByteWidth = sizeof(quad);
    vd.Usage = D3D11_USAGE_DEFAULT;
    vd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vinit{quad, 0, 0};
    ComPtr<ID3D11Buffer> vertices;
    h.check(SUCCEEDED(dev->CreateBuffer(&vd, &vinit, &vertices)), "vertex buffer");
    const uint32_t instances[] = {5, 0, 9, 0, 12, 0};
    vd.ByteWidth = sizeof(instances);
    D3D11_SUBRESOURCE_DATA iinit{instances, 0, 0};
    ComPtr<ID3D11Buffer> instanceBuffer;
    h.check(SUCCEEDED(dev->CreateBuffer(&vd, &iinit, &instanceBuffer)), "instance buffer");

    ComPtr<ID3D11Texture2D> colour[4], slots, depth;
    ComPtr<ID3D11RenderTargetView> rtv[7];
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    for (int i = 0; i < 4; ++i) {
        h.check(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &colour[i])), "G-buffer target");
        h.check(SUCCEEDED(dev->CreateRenderTargetView(colour[i].Get(), nullptr, &rtv[i])), "G-buffer RTV");
    }
    td.Format = DXGI_FORMAT_R32G32_FLOAT;
    h.check(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &slots)), "slot target");
    h.check(SUCCEEDED(dev->CreateRenderTargetView(slots.Get(), nullptr, &rtv[6])), "slot RTV");
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    h.check(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &depth)), "depth target");
    ComPtr<ID3D11DepthStencilView> dsv;
    h.check(SUCCEEDED(dev->CreateDepthStencilView(depth.Get(), nullptr, &dsv)), "DSV");
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_GREATER;   // reversed Z
    ComPtr<ID3D11DepthStencilState> dss;
    h.check(SUCCEEDED(dev->CreateDepthStencilState(&dsd, &dss)), "depth state");
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rs;
    h.check(SUCCEEDED(dev->CreateRasterizerState(&rd, &rs)), "rasterizer state");

    const float clearSlots[4] = {-1.0f, 0.0f, 0.0f, 0.0f};
    const float black[4] = {};
    for (int i = 0; i < 4; ++i) ctx->ClearRenderTargetView(rtv[i].Get(), black);
    ctx->ClearRenderTargetView(rtv[6].Get(), clearSlots);
    ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
    ID3D11RenderTargetView* bound[7] = {rtv[0].Get(), rtv[1].Get(), rtv[2].Get(), rtv[3].Get(), nullptr, nullptr, rtv[6].Get()};
    ctx->OMSetRenderTargets(7, bound, dsv.Get());
    ctx->OMSetDepthStencilState(dss.Get(), 0);
    ctx->OMSetBlendState(nullptr, nullptr, ~0u);
    D3D11_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rs.Get());
    ctx->IASetInputLayout(layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11Buffer* vbs[2] = {vertices.Get(), instanceBuffer.Get()};
    UINT strides[2] = {12, 8}, offsets[2] = {0, 0};
    ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
    ID3D11ShaderResourceView* poolView = poolSrv.Get();
    ctx->VSSetShaderResources(33, 1, &poolView);
    ID3D11Buffer* cb = sceneCb.Get();
    ctx->VSSetConstantBuffers(1, 1, &cb);
    // The two pool movers through the PATCHED pair...
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ctx->DrawInstanced(4, 2, 0, 0);
    // ...then the occluder through the ORIGINAL pair: it wins the depth test
    // over part of slot 5 and never writes MRT6.
    ctx->VSSetShader(vsOrig.Get(), nullptr, 0);
    ctx->PSSetShader(psOrig.Get(), nullptr, 0);
    ctx->DrawInstanced(4, 1, 0, 2);
    ID3D11RenderTargetView* none[8] = {};
    ctx->OMSetRenderTargets(8, none, nullptr);

    auto readback = [&](ID3D11Texture2D* source, std::vector<float>& out, UINT channels) {
        D3D11_TEXTURE2D_DESC d{};
        source->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        h.check(SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &staging)), "staging texture");
        ctx->CopyResource(staging.Get(), source);
        D3D11_MAPPED_SUBRESOURCE m{};
        h.check(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "map staging");
        out.resize(size_t(W) * H * channels);
        for (UINT y = 0; y < H; ++y)
            std::memcpy(&out[size_t(y) * W * channels], static_cast<const BYTE*>(m.pData) + y * m.RowPitch, W * channels * 4);
        ctx->Unmap(staging.Get(), 0);
    };
    std::vector<float> slotPixels, depthPixels;
    readback(slots.Get(), slotPixels, 2);
    readback(depth.Get(), depthPixels, 1);

    uint32_t covered5 = 0, covered9 = 0, stale = 0, empty = 0, wrong = 0;
    for (UINT i = 0; i < W * H; ++i) {
        const float slot = slotPixels[i * 2], z = slotPixels[i * 2 + 1], sceneZ = depthPixels[i];
        uint32_t zBits = 0, sceneBits = 0;
        std::memcpy(&zBits, &z, 4);
        std::memcpy(&sceneBits, &sceneZ, 4);
        if (slot == -1.0f) {
            if (z != 0.0f) ++wrong;
            ++empty;
            continue;
        }
        // The odd code: 2 * slot + 1.
        if (slot != 11.0f && slot != 19.0f) { ++wrong; continue; }
        if (zBits == sceneBits) { if (slot == 11.0f) ++covered5; else ++covered9; }
        else ++stale;
    }
    std::printf("  family %s: slot 5 %u px, slot 9 %u px, stale (occluded) %u px, empty %u px\n",
                f.name, covered5, covered9, stale, empty);
    h.check(wrong == 0, "MRT6 holds only an exact slot or the cleared value");
    h.check(covered5 > 0 && covered9 > 0, "both movers own pixels with exact depth");
    h.check(stale > 0, "the unpatched occluder leaves stale slots the depth test rejects");
    h.check(empty > 0, "uncovered pixels keep the cleared value");
}

// The consumer's reading of one MRT6 texel against the scene depth, as
// enginePixel does it: 0 none (cleared), 4 stale, 5 corrupt, else 1 with the
// slot. A mirror for the rig's CPU checks; the GPU decode itself is the
// consumer test's.
inline int decodeSlot(float x, float y, float sceneZ, uint32_t* slot) {
    if (!(x >= 1.0f)) return 0;
    uint32_t yb = 0, zb = 0;
    std::memcpy(&yb, &y, 4);
    std::memcpy(&zb, &sceneZ, 4);
    if (!(sceneZ > 0.0f) || yb != zb) return 4;
    const uint32_t code = static_cast<uint32_t>(x);
    if (static_cast<float>(code) != x || (code & 1u) == 0) return 5;
    *slot = code >> 1;
    return 1;
}

inline void blendChecks(const Harness& h, const std::vector<BYTE>& vsBytes, const std::vector<BYTE>& psStockBytes,
                        const std::vector<BYTE>& psPatchedBytes) {
    auto* dev = h.device;
    auto* ctx = h.context;
    constexpr UINT W = 48, H = 48;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> psStock, psPatched;
    h.check(SUCCEEDED(dev->CreateVertexShader(vsBytes.data(), vsBytes.size(), nullptr, &vs)), "blend: VS");
    h.check(SUCCEEDED(dev->CreatePixelShader(psStockBytes.data(), psStockBytes.size(), nullptr, &psStock)), "blend: stock PS");
    h.check(SUCCEEDED(dev->CreatePixelShader(psPatchedBytes.data(), psPatchedBytes.size(), nullptr, &psPatched)), "blend: patched PS");
    const D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        {"INSTANCEANDMODELDATAINDEX", 0, DXGI_FORMAT_R32G32_UINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"PACKEDVERTEXDATAA", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ComPtr<ID3D11InputLayout> layout;
    h.check(SUCCEEDED(dev->CreateInputLayout(layoutDesc, 2, vsBytes.data(), vsBytes.size(), &layout)), "blend: layout");
    // Slot 5 near, slot 9 farther and off to the side (as drawChecks), slot 3
    // behind slot 5 on the same pixels.
    std::vector<Record> pool(16);
    const float identity[4] = {0, 0, 0, 1};
    const float s = std::sqrt(0.5f);
    const float turned[4] = {0, 0, s, s};
    pool[5] = makeRecord(-0.5f, 0.0f, -2.0f, 1.0f, identity);
    pool[9] = makeRecord(0.5f, 0.0f, -4.0f, 1.0f, turned);
    pool[3] = makeRecord(-0.5f, 0.0f, -2.5f, 1.2f, identity);
    D3D11_BUFFER_DESC pd{};
    pd.ByteWidth = UINT(pool.size() * sizeof(Record));
    pd.Usage = D3D11_USAGE_DEFAULT; pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    pd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; pd.StructureByteStride = sizeof(Record);
    D3D11_SUBRESOURCE_DATA pinit{pool.data(), 0, 0};
    ComPtr<ID3D11Buffer> poolBuffer;
    h.check(SUCCEEDED(dev->CreateBuffer(&pd, &pinit, &poolBuffer)), "blend: pool");
    ComPtr<ID3D11ShaderResourceView> poolSrv;
    h.check(SUCCEEDED(dev->CreateShaderResourceView(poolBuffer.Get(), nullptr, &poolSrv)), "blend: pool SRV");
    std::vector<float> scene(276 * 4, 0.0f);
    scene[270 * 4] = 1.0f; scene[271 * 4 + 1] = 1.0f; scene[272 * 4 + 3] = -1.0f; scene[273 * 4 + 2] = 0.025f;
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = UINT(scene.size() * 4); cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cinit{scene.data(), 0, 0};
    ComPtr<ID3D11Buffer> sceneCb;
    h.check(SUCCEEDED(dev->CreateBuffer(&cd, &cinit, &sceneCb)), "blend: scene CB");
    const float quad[] = {-0.4f, -0.4f, 0, 0.4f, -0.4f, 0, -0.4f, 0.4f, 0, 0.4f, 0.4f, 0};
    D3D11_BUFFER_DESC vd{};
    vd.ByteWidth = sizeof(quad); vd.Usage = D3D11_USAGE_DEFAULT; vd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vinit{quad, 0, 0};
    ComPtr<ID3D11Buffer> vertices;
    h.check(SUCCEEDED(dev->CreateBuffer(&vd, &vinit, &vertices)), "blend: vertices");
    const uint32_t instances[] = {5, 0, 9, 0, 3, 0};
    vd.ByteWidth = sizeof(instances);
    D3D11_SUBRESOURCE_DATA iinit{instances, 0, 0};
    ComPtr<ID3D11Buffer> instanceBuffer;
    h.check(SUCCEEDED(dev->CreateBuffer(&vd, &iinit, &instanceBuffer)), "blend: instances");

    ComPtr<ID3D11Texture2D> colour[4], slots, depth;
    ComPtr<ID3D11RenderTargetView> rtv[4], slotRtv;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;   // every bit the PS writes survives
    for (int i = 0; i < 4; ++i) {
        h.check(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &colour[i])), "blend: G-buffer target");
        h.check(SUCCEEDED(dev->CreateRenderTargetView(colour[i].Get(), nullptr, &rtv[i])), "blend: G-buffer RTV");
    }
    td.Format = DXGI_FORMAT_R32G32_FLOAT;
    h.check(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &slots)), "blend: slot target");
    h.check(SUCCEEDED(dev->CreateRenderTargetView(slots.Get(), nullptr, &slotRtv)), "blend: slot RTV");
    td.Format = DXGI_FORMAT_D32_FLOAT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    h.check(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &depth)), "blend: depth");
    ComPtr<ID3D11DepthStencilView> dsv;
    h.check(SUCCEEDED(dev->CreateDepthStencilView(depth.Get(), nullptr, &dsv)), "blend: DSV");
    auto depthState = [&](bool write, D3D11_COMPARISON_FUNC func) {
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = TRUE; d.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = func;
        ComPtr<ID3D11DepthStencilState> st;
        h.check(SUCCEEDED(dev->CreateDepthStencilState(&d, &st)), "blend: depth state");
        return st;
    };
    auto writeGreater = depthState(true, D3D11_COMPARISON_GREATER);
    auto readGreater = depthState(false, D3D11_COMPARISON_GREATER);
    auto readEqual = depthState(false, D3D11_COMPARISON_EQUAL);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rs;
    h.check(SUCCEEDED(dev->CreateRasterizerState(&rd, &rs)), "blend: rasterizer");

    auto readback = [&](ID3D11Texture2D* source, UINT channels) {
        D3D11_TEXTURE2D_DESC d{};
        source->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        h.check(SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &staging)), "blend: staging");
        ctx->CopyResource(staging.Get(), source);
        D3D11_MAPPED_SUBRESOURCE m{};
        h.check(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "blend: map");
        std::vector<float> out(size_t(W) * H * channels);
        for (UINT y = 0; y < H; ++y)
            std::memcpy(&out[size_t(y) * W * channels], static_cast<const BYTE*>(m.pData) + y * m.RowPitch, W * channels * 4);
        ctx->Unmap(staging.Get(), 0);
        return out;
    };
    const float base[4] = {0.25f, 0.5f, 0.75f, 0.5f};
    const float clearSlots[4] = {-1.0f, 0.0f, 0.0f, 0.0f};
    auto begin = [&](bool withSlots) {
        for (auto& r : rtv) ctx->ClearRenderTargetView(r.Get(), base);
        ctx->ClearRenderTargetView(slotRtv.Get(), clearSlots);
        ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        ID3D11RenderTargetView* bound[7] = {rtv[0].Get(), rtv[1].Get(), rtv[2].Get(), rtv[3].Get(), nullptr, nullptr,
                                            withSlots ? slotRtv.Get() : nullptr};
        ctx->OMSetRenderTargets(7, bound, dsv.Get());
        D3D11_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rs.Get());
        ctx->IASetInputLayout(layout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D11Buffer* vbs[2] = {vertices.Get(), instanceBuffer.Get()};
        UINT strides[2] = {12, 8}, offsets[2] = {0, 0};
        ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
        ID3D11ShaderResourceView* poolView = poolSrv.Get();
        ctx->VSSetShaderResources(33, 1, &poolView);
        ID3D11Buffer* cb = sceneCb.Get();
        ctx->VSSetConstantBuffers(1, 1, &cb);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(withSlots ? psPatched.Get() : psStock.Get(), nullptr, 0);
    };
    auto end = [&] { ID3D11RenderTargetView* none[8] = {}; ctx->OMSetRenderTargets(8, none, nullptr); };
    const float factor[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    // draw(pool slot, state, depth state): the instance buffer holds 5, 9, 3.
    auto draw = [&](UINT slot, ID3D11BlendState* state, ID3D11DepthStencilState* ds) {
        ctx->OMSetBlendState(state, factor, ~0u);
        ctx->OMSetDepthStencilState(ds, 0);
        ctx->DrawInstanced(4, 1, 0, slot == 5 ? 0u : slot == 9 ? 1u : 2u);
    };
    auto sameBits = [](const std::vector<float>& a, const std::vector<float>& b) {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * 4) == 0;
    };

    // The game's states.
    auto rt = [](BOOL enable, D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND_OP op, UINT8 mask) {
        D3D11_RENDER_TARGET_BLEND_DESC t{};
        t.BlendEnable = enable; t.SrcBlend = src; t.DestBlend = dst; t.BlendOp = op;
        t.SrcBlendAlpha = src == D3D11_BLEND_SRC_ALPHA ? D3D11_BLEND_ONE : src;
        t.DestBlendAlpha = dst == D3D11_BLEND_INV_SRC_ALPHA ? D3D11_BLEND_ZERO : dst;
        t.BlendOpAlpha = op; t.RenderTargetWriteMask = mask;
        return t;
    };
    struct Case { const char* name; D3D11_BLEND_DESC desc; bool null; bool corrupts; };
    std::vector<Case> cases;
    auto uniform = [&](const char* name, D3D11_RENDER_TARGET_BLEND_DESC t, bool corrupts) {
        D3D11_BLEND_DESC d = edvr::engineVelocityDefaultBlend();
        d.RenderTarget[0] = t;
        cases.push_back({name, d, false, corrupts});
    };
    cases.push_back({"overwrite (null state)", edvr::engineVelocityDefaultBlend(), true, false});
    uniform("additive ONE+ONE, independent off", rt(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL), true);
    uniform("blend factor 0.5/0.5, independent off", rt(TRUE, D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL), true);
    uniform("partial write mask R|A, independent off", rt(FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
                                                          D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_ALPHA), true);
    uniform("MAX, independent off", rt(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX, D3D11_COLOR_WRITE_ENABLE_ALL), false);
    {
        D3D11_BLEND_DESC d = edvr::engineVelocityDefaultBlend();
        d.IndependentBlendEnable = TRUE;
        d.RenderTarget[0] = rt(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL);
        d.RenderTarget[1] = rt(TRUE, D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL);
        d.RenderTarget[2] = rt(FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
                               D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE);
        d.RenderTarget[3] = rt(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MIN, D3D11_COLOR_WRITE_ENABLE_ALL);
        d.RenderTarget[6] = rt(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL);
        cases.push_back({"independent: RT0 add, RT1 factor, RT2 RGB mask, RT3 MIN, RT6 add", d, false, true});
    }

    unsigned exact = 0, declined = 0;
    for (const auto& c : cases) {
        ComPtr<ID3D11BlendState> game;
        if (!c.null) h.check(SUCCEEDED(dev->CreateBlendState(&c.desc, &game)), "blend: the game's state");
        const char* refused = nullptr;
        ComPtr<ID3D11BlendState> derived = edvr::engineVelocityCreateDerivedBlend(dev, game.Get(), &refused);
        h.check(derived && !refused, "blend: a derived state exists for every game state without a logic op");
        D3D11_BLEND_DESC dd{};
        derived->GetDesc(&dd);
        h.check(dd.IndependentBlendEnable && !dd.RenderTarget[6].BlendEnable &&
                dd.RenderTarget[6].RenderTargetWriteMask == (D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN),
                "blend: the derived state writes MRT6 unblended, R and G");
        // The game's own draw: the stock pair under the game's state.
        begin(false);
        draw(5, game.Get(), writeGreater.Get());
        draw(3, game.Get(), writeGreater.Get());
        end();
        std::vector<float> reference[4];
        for (int i = 0; i < 4; ++i) reference[i] = readback(colour[i].Get(), 4);
        // The substituted draw under the derived state.
        begin(true);
        draw(5, derived.Get(), writeGreater.Get());
        draw(3, derived.Get(), writeGreater.Get());
        end();
        for (int i = 0; i < 4; ++i)
            h.check(sameBits(readback(colour[i].Get(), 4), reference[i]),
                    "blend: the game's four targets are bit-identical under the derived state");
        auto slotPixels = readback(slots.Get(), 2), depthPixels = readback(depth.Get(), 1);
        unsigned covered = 0, bad = 0;
        for (UINT i = 0; i < W * H; ++i) {
            uint32_t slot = 0;
            const int kind = decodeSlot(slotPixels[i * 2], slotPixels[i * 2 + 1], depthPixels[i], &slot);
            if (kind == 0) continue;
            if (kind == 1 && (slot == 5 || slot == 3)) ++covered; else ++bad;
        }
        h.check(covered > 0 && bad == 0, "blend: MRT6 exact under the derived state (odd code, the depth written)");
        ++exact;
        // What phase 1 did: the game's state inherited by MRT6. Whatever the
        // arithmetic did, the decode never names a record that did not draw
        // the pixel with the depth it holds.
        begin(true);
        draw(5, game.Get(), writeGreater.Get());
        draw(3, game.Get(), writeGreater.Get());
        end();
        slotPixels = readback(slots.Get(), 2);
        depthPixels = readback(depth.Get(), 1);
        unsigned caseDeclined = 0, wrongRecord = 0;
        for (UINT i = 0; i < W * H; ++i) {
            uint32_t slot = 0;
            const int kind = decodeSlot(slotPixels[i * 2], slotPixels[i * 2 + 1], depthPixels[i], &slot);
            if (kind == 4 || kind == 5) ++caseDeclined;
            if (kind == 1 && slot != 5 && slot != 3) ++wrongRecord;
        }
        h.check(wrongRecord == 0, "blend: inherited arithmetic is declined, never read as another record");
        if (c.corrupts) h.check(caseDeclined > 0, "blend: the inherited state did corrupt MRT6 (and the decode declined it)");
        declined += caseDeclined;
    }

    // A blend change between two keyed draws: each gets its own derived state.
    {
        ComPtr<ID3D11BlendState> add, factorState;
        h.check(SUCCEEDED(dev->CreateBlendState(&cases[1].desc, &add)), "blend: additive");
        h.check(SUCCEEDED(dev->CreateBlendState(&cases[2].desc, &factorState)), "blend: factor");
        const char* refused = nullptr;
        auto dAdd = edvr::engineVelocityCreateDerivedBlend(dev, add.Get(), &refused);
        auto dFactor = edvr::engineVelocityCreateDerivedBlend(dev, factorState.Get(), &refused);
        begin(false);
        draw(5, add.Get(), writeGreater.Get());
        draw(9, factorState.Get(), writeGreater.Get());
        end();
        std::vector<float> reference[4];
        for (int i = 0; i < 4; ++i) reference[i] = readback(colour[i].Get(), 4);
        begin(true);
        draw(5, dAdd.Get(), writeGreater.Get());
        draw(9, dFactor.Get(), writeGreater.Get());
        end();
        for (int i = 0; i < 4; ++i)
            h.check(sameBits(readback(colour[i].Get(), 4), reference[i]), "blend: mid-pass change keeps the game's targets bit-identical");
        auto slotPixels = readback(slots.Get(), 2), depthPixels = readback(depth.Get(), 1);
        unsigned five = 0, nine = 0, bad = 0;
        for (UINT i = 0; i < W * H; ++i) {
            uint32_t slot = 0;
            const int kind = decodeSlot(slotPixels[i * 2], slotPixels[i * 2 + 1], depthPixels[i], &slot);
            if (kind == 0) continue;
            if (kind == 1 && slot == 5) ++five; else if (kind == 1 && slot == 9) ++nine; else ++bad;
        }
        h.check(five > 0 && nine > 0 && bad == 0, "blend: mid-pass change, both draws own exact pixels");
    }

    // Depth writes off: a draw that passes the test without writing depth
    // leaves a slot whose depth is not the scene's (declined as stale); a
    // depth-EQUAL pass over the same surface keeps its own record.
    {
        const char* refused = nullptr;
        auto dNull = edvr::engineVelocityCreateDerivedBlend(dev, nullptr, &refused);
        begin(true);
        draw(5, dNull.Get(), writeGreater.Get());      // writes depth
        draw(9, dNull.Get(), readGreater.Get());       // passes over the cleared depth, writes none
        draw(5, dNull.Get(), readEqual.Get());         // the same surface again, depth EQUAL
        end();
        auto slotPixels = readback(slots.Get(), 2), depthPixels = readback(depth.Get(), 1);
        unsigned owned = 0, stale = 0, bad = 0;
        for (UINT i = 0; i < W * H; ++i) {
            uint32_t slot = 0;
            const int kind = decodeSlot(slotPixels[i * 2], slotPixels[i * 2 + 1], depthPixels[i], &slot);
            if (kind == 1 && slot == 5) ++owned;
            else if (kind == 4) ++stale;
            else if (kind != 0) ++bad;
        }
        h.check(owned > 0 && stale > 0 && bad == 0,
                "blend: depth-write-disabled draws are stale unless over their own depth; the equal pass keeps its record");
    }
    std::printf("  blend: %u game states exact under the derived state; %u corrupted pixels declined under the inherited one\n",
                exact, declined);
}

inline void run(const Harness& h) {
    const Family families[] = {
        {"A (DATAID.y, like vs_EB52/ps_CB9F)", kVsA, kPsA, 0, 1, 4, false},
        {"B (FACEINVARIANT.x + SV_Position, like vs_BBE5/ps_DB3E)", kVsB, kPsB, 0, 0, 4, false},
        {"C (UV-only, like vs_5B4D/ps_4375)", kVsC, kPsC, 2, 0, 1, true},
        {"D (front face at VS position register, like vs_DE54/ps_91F8)", kVsB, kPsFrontFace, 0, 0, 4, false},
    };
    for (const auto& f : families) {
        const auto vsBlob = compile(h, std::string(kVsCommon) + f.vs, "vs_5_0");
        const auto psBlob = compile(h, f.ps, "ps_5_0");
        const std::vector<BYTE> vs(static_cast<const BYTE*>(vsBlob->GetBufferPointer()),
                                   static_cast<const BYTE*>(vsBlob->GetBufferPointer()) + vsBlob->GetBufferSize());
        const std::vector<BYTE> ps(static_cast<const BYTE*>(psBlob->GetBufferPointer()),
                                   static_cast<const BYTE*>(psBlob->GetBufferPointer()) + psBlob->GetBufferSize());
        edvr::EngineVelocityInputs in;
        std::vector<BYTE> patchedVs, patchedPs;
        patchStaticChecks(h, f, vs, ps, in, patchedVs, patchedPs);
        if (&f == &families[3]) {
            const std::string text = disassemble(patchedPs);
            h.check(text.find("v4.x, is_front_face") != std::string::npos &&
                    text.find("v5.z, position") != std::string::npos &&
                    text.find("mov o6.y, v5.z") != std::string::npos,
                    "front-face stays at v4 while MRT6 depth reads new SV_Position v5");
        }
        drawChecks(h, f, vs, ps, patchedVs, patchedPs);
        // Family A is the shape the flight substituted (vs_EB52 -> ps_3434).
        if (&f == &families[0]) blendChecks(h, vs, ps, patchedPs);
    }
    // A vertex shader that does not index the pool by v0.x must not get a
    // slot export invented for it.
    const char* stray = R"HLSL(
struct VsIn { uint2 inst : INSTANCEANDMODELDATAINDEX; float3 v : PACKEDVERTEXDATAA; };
struct VsOut { float2 uv : __USER_VERTEX_M_TEXCOORD; float4 p : SV_POSITION; };
VsOut main(VsIn i) { VsOut o; o.uv = i.v.xy; o.p = float4(i.v, 1); return o; }
)HLSL";
    const auto blob = compile(h, stray, "vs_5_0");
    edvr::EngineVelocityInputs in;
    std::string why;
    h.check(!edvr::engineVelocityDeriveInputs(blob->GetBufferPointer(), blob->GetBufferSize(), in, why),
            "a VS that never reads t33 by v0.x declines");
}

} // namespace shader_tests
