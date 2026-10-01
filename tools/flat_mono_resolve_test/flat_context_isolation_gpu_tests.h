// The resolver's context isolation on WARP (src\d3d11\flat_context_isolation.h, flat_context_state.h; docs\macos-dxmt-2026-09-30.md).
//
// WHY. On DXMT (Elite under CrossOver on a Mac) ID3D11DeviceContext1::SwapDeviceContextState is UNIMPLEMENTED() and aborts the
// process, so the resolver isolates the game's pipeline state there with an explicit capture: Get calls out, ClearState, Set calls
// back. The capture cannot be flown on a Mac from here, so the rig proves what a rig can: that it is complete, exact and
// leak-free on a real D3D11 runtime, and that nothing changed for the device that swaps.
//
// WHAT IS PINNED.
//  1. The decision: the key's text, the four DXMT markers (the two private interface IIDs spelt as DXMT's source spells them, the
//     module's version resource read from a real Windows blob patched to say DXMT, the adapter name), the choice for every key and
//     marker, and the one log line for each case. The real WARP device answers none of the markers, so auto is swap.
//  2. The explicit block, stage by stage and slot by slot. The game's state fills EVERY stage and slot range the block documents
//     (IA with vertex buffers out to slot 31, six shader stages with all 128 shader-resource slots, 14 constant-buffer slots
//     including D3D11.1 offset windows, 16 samplers, CS and OM UAVs out to the last slot, stream output, render targets, depth,
//     blend, depth-stencil, rasterizer, three viewports, two scissors, predication). An oracle built from nothing but the context's
//     own Get calls snapshots every value of those ranges (1,291 on the rig's own feature level 11_0 device, 1,403 on a second
//     WARP device at 11_1, whose UAV ranges are 64 slots) and every object's reference count; the block captures, ClearState runs,
//     the context is proved to be at its defaults (what the resolver relies on), a stand-in backend dirties every stage and slot
//     with other objects and runs a dispatch, ClearState runs again, the block restores, and the snapshot and every reference count
//     must be what they were. Then the oracle is held to its word: 54 single changes, one in each stage and kind of slot, must
//     each be seen at the right label, a leaked reference must be seen in the counts, a context cleared and never restored must
//     differ, and a capture whose ranges stop short must leave exactly the far slots behind.
//
// THE DEBUG LAYER. Where the D3D debug layer is installed (the optional Graphics Tools feature) the calls are also held to its
// validation, and the rig's final message check covers them. A machine without it runs on a plain device; the rig says so, and its
// debug-layer checks are then skipped, not passed. The oracle is what holds the calls either way: a call the runtime refuses leaves
// its slot unbound, and the snapshot no longer matches.
//  3. The resolver, in both modes, on the copy route, the HDR route, a refused backend and the spatial recovery, with the rig's
//     backend dirtying every stage after its own ClearState: the game's whole state and every reference count come back, the swap
//     is what the default route uses (mode "swap", no explicit capture counted, the crumbs carry by=swap and none of the block's
//     groups), the explicit capture is used exactly when it is asked for (by=capture, the eleven groups written once, for the first
//     capture and the first restore, and their end lines saying what the game had bound).
#pragma once

#include <d3dcompiler.h>

#include <functional>
#include <map>

namespace edvr {
// Test-only (flat_mono_resolve.cpp, at its foot; not part of flat_mono_resolve.h's contract): the log-line budget starts over.
void flatMonoResolveTestResetIsolationLog();
}  // namespace edvr

namespace isogpu {

inline ComPtr<ID3DBlob> hlsl(const char* source, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL0, 0,
                                  code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        std::printf("context isolation rig: %s did not compile: %s\n", target, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

// The per-stage calls that differ only by name, as pointers to the interface's own methods.
struct StageApi {
    const char* name;
    void (STDMETHODCALLTYPE ID3D11DeviceContext::*setSrv)(UINT, UINT, ID3D11ShaderResourceView* const*);
    void (STDMETHODCALLTYPE ID3D11DeviceContext::*getSrv)(UINT, UINT, ID3D11ShaderResourceView**);
    void (STDMETHODCALLTYPE ID3D11DeviceContext::*setSmp)(UINT, UINT, ID3D11SamplerState* const*);
    void (STDMETHODCALLTYPE ID3D11DeviceContext::*getSmp)(UINT, UINT, ID3D11SamplerState**);
    void (STDMETHODCALLTYPE ID3D11DeviceContext::*setCb)(UINT, UINT, ID3D11Buffer* const*);
    void (STDMETHODCALLTYPE ID3D11DeviceContext1::*setCb1)(UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
    void (STDMETHODCALLTYPE ID3D11DeviceContext1::*getCb1)(UINT, UINT, ID3D11Buffer**, UINT*, UINT*);
};
#define ISO_STAGE(P, N)                                                                                               \
    {N, &ID3D11DeviceContext::P##SetShaderResources, &ID3D11DeviceContext::P##GetShaderResources,                     \
     &ID3D11DeviceContext::P##SetSamplers, &ID3D11DeviceContext::P##GetSamplers,                                      \
     &ID3D11DeviceContext::P##SetConstantBuffers, &ID3D11DeviceContext1::P##SetConstantBuffers1,                      \
     &ID3D11DeviceContext1::P##GetConstantBuffers1}
inline const StageApi* stageApis() {
    static const StageApi table[6] = {ISO_STAGE(VS, "VS"), ISO_STAGE(HS, "HS"), ISO_STAGE(DS, "DS"),
                                      ISO_STAGE(GS, "GS"), ISO_STAGE(PS, "PS"), ISO_STAGE(CS, "CS")};
    return table;
}
#undef ISO_STAGE
constexpr unsigned kStages = 6;

// Everything the context says about its own state, through nothing but its Get calls, as label -> value. Pointers are kept as
// addresses and the references the Get calls hand back are dropped at once, so taking one disturbs no count.
struct Snap {
    std::map<std::string, uint64_t> v;
    void put(const std::string& key, uint64_t value) { v[key] = value; }
    template <class T>
    void put(const std::string& key, T* object) { v[key] = reinterpret_cast<uintptr_t>(object); }
};
inline uint64_t floatBits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
template <class T>
inline T* drop(T* p) { if (p) p->Release(); return p; }   // the address of an object whose reference was just taken

inline Snap snapshot(ID3D11DeviceContext1* c, UINT uavSlots) {
    Snap s;
    char key[96];
    const auto k = [&](const char* fmt, auto... a) { std::snprintf(key, sizeof(key), fmt, a...); return std::string(key); };
    // IA
    { ID3D11InputLayout* l = nullptr; c->IAGetInputLayout(&l); s.put("IA.layout", drop(l)); }
    { D3D11_PRIMITIVE_TOPOLOGY t{}; c->IAGetPrimitiveTopology(&t); s.put("IA.topology", uint64_t(t)); }
    {
        ID3D11Buffer* b[32] = {}; UINT stride[32] = {}, offset[32] = {};
        c->IAGetVertexBuffers(0, 32, b, stride, offset);
        for (UINT i = 0; i < 32; ++i) {
            s.put(k("IA.vb[%u].buf", i), drop(b[i]));
            s.put(k("IA.vb[%u].stride", i), uint64_t(stride[i]));
            s.put(k("IA.vb[%u].offset", i), uint64_t(offset[i]));
        }
        ID3D11Buffer* ib = nullptr; DXGI_FORMAT format{}; UINT off = 0;
        c->IAGetIndexBuffer(&ib, &format, &off);
        s.put("IA.ib.buf", drop(ib)); s.put("IA.ib.format", uint64_t(format)); s.put("IA.ib.offset", uint64_t(off));
    }
    // The six shader stages
    const StageApi* api = stageApis();
    for (unsigned st = 0; st < kStages; ++st) {
        const char* n = api[st].name;
        switch (st) {
        case 0: { ID3D11VertexShader* x = nullptr; c->VSGetShader(&x, nullptr, nullptr); s.put(k("%s.shader", n), drop(x)); break; }
        case 1: { ID3D11HullShader* x = nullptr; c->HSGetShader(&x, nullptr, nullptr); s.put(k("%s.shader", n), drop(x)); break; }
        case 2: { ID3D11DomainShader* x = nullptr; c->DSGetShader(&x, nullptr, nullptr); s.put(k("%s.shader", n), drop(x)); break; }
        case 3: { ID3D11GeometryShader* x = nullptr; c->GSGetShader(&x, nullptr, nullptr); s.put(k("%s.shader", n), drop(x)); break; }
        case 4: { ID3D11PixelShader* x = nullptr; c->PSGetShader(&x, nullptr, nullptr); s.put(k("%s.shader", n), drop(x)); break; }
        default: { ID3D11ComputeShader* x = nullptr; c->CSGetShader(&x, nullptr, nullptr); s.put(k("%s.shader", n), drop(x)); break; }
        }
        ID3D11ShaderResourceView* srv[128] = {};
        (c->*api[st].getSrv)(0, 128, srv);
        for (UINT i = 0; i < 128; ++i) s.put(k("%s.srv[%u]", n, i), drop(srv[i]));
        ID3D11Buffer* cb[14] = {}; UINT first[14], num[14];
        for (UINT i = 0; i < 14; ++i) { first[i] = 0; num[i] = 4096; }
        (c->*api[st].getCb1)(0, 14, cb, first, num);
        for (UINT i = 0; i < 14; ++i) {
            ID3D11Buffer* held = drop(cb[i]);
            s.put(k("%s.cb[%u]", n, i), held);
            if (held) { s.put(k("%s.cb[%u].first", n, i), uint64_t(first[i])); s.put(k("%s.cb[%u].num", n, i), uint64_t(num[i])); }
        }
        ID3D11SamplerState* smp[16] = {};
        (c->*api[st].getSmp)(0, 16, smp);
        for (UINT i = 0; i < 16; ++i) s.put(k("%s.sampler[%u]", n, i), drop(smp[i]));
    }
    { ID3D11UnorderedAccessView* u[64] = {}; c->CSGetUnorderedAccessViews(0, uavSlots, u); for (UINT i = 0; i < uavSlots; ++i) s.put(k("CS.uav[%u]", i), drop(u[i])); }
    // SO
    { ID3D11Buffer* so[4] = {}; c->SOGetTargets(4, so); for (UINT i = 0; i < 4; ++i) s.put(k("SO[%u]", i), drop(so[i])); }
    // OM
    {
        ID3D11RenderTargetView* rtv[8] = {}; ID3D11DepthStencilView* dsv = nullptr; ID3D11UnorderedAccessView* uav[64] = {};
        c->OMGetRenderTargetsAndUnorderedAccessViews(8, rtv, &dsv, 0, uavSlots, uav);
        for (UINT i = 0; i < 8; ++i) s.put(k("OM.rtv[%u]", i), drop(rtv[i]));
        s.put("OM.dsv", drop(dsv));
        for (UINT i = 0; i < uavSlots; ++i) s.put(k("OM.uav[%u]", i), drop(uav[i]));
        ID3D11BlendState* bs = nullptr; FLOAT factor[4] = {}; UINT mask = 0;
        c->OMGetBlendState(&bs, factor, &mask);
        s.put("OM.blend", drop(bs)); s.put("OM.sampleMask", uint64_t(mask));
        for (unsigned i = 0; i < 4; ++i) s.put(k("OM.blendFactor[%u]", i), floatBits(factor[i]));
        ID3D11DepthStencilState* ds = nullptr; UINT ref = 0;
        c->OMGetDepthStencilState(&ds, &ref);
        s.put("OM.ds", drop(ds)); s.put("OM.stencilRef", uint64_t(ref));
    }
    // RS
    {
        ID3D11RasterizerState* rs = nullptr; c->RSGetState(&rs); s.put("RS.state", drop(rs));
        UINT n = 16; D3D11_VIEWPORT vp[16] = {}; c->RSGetViewports(&n, vp);
        s.put("RS.viewportCount", uint64_t(n));
        for (UINT i = 0; i < n && i < 16; ++i) {
            const float f[6] = {vp[i].TopLeftX, vp[i].TopLeftY, vp[i].Width, vp[i].Height, vp[i].MinDepth, vp[i].MaxDepth};
            for (unsigned j = 0; j < 6; ++j) s.put(k("RS.viewport[%u].%u", i, j), floatBits(f[j]));
        }
        UINT m = 16; D3D11_RECT rc[16] = {}; c->RSGetScissorRects(&m, rc);
        s.put("RS.scissorCount", uint64_t(m));
        for (UINT i = 0; i < m && i < 16; ++i) {
            const LONG r[4] = {rc[i].left, rc[i].top, rc[i].right, rc[i].bottom};
            for (unsigned j = 0; j < 4; ++j) s.put(k("RS.scissor[%u].%u", i, j), uint64_t(uint32_t(r[j])));
        }
    }
    // Predication
    { ID3D11Predicate* p = nullptr; BOOL value = FALSE; c->GetPredication(&p, &value); s.put("PRED.obj", drop(p)); s.put("PRED.value", uint64_t(value ? 1 : 0)); }
    return s;
}
// The first label whose value differs (or exists in only one), or an empty string; `count` gets how many do.
inline std::string firstDifference(const Snap& a, const Snap& b, size_t* count = nullptr) {
    std::string first;
    size_t n = 0;
    for (const auto& e : a.v) {
        const auto it = b.v.find(e.first);
        if (it == b.v.end() || it->second != e.second) { if (first.empty()) first = e.first; ++n; }
    }
    for (const auto& e : b.v)
        if (a.v.find(e.first) == a.v.end()) { if (first.empty()) first = e.first; ++n; }
    if (count) *count = n;
    return first;
}

// The objects of a fixture and their public reference counts (AddRef then Release reports the count before).
using Objects = std::vector<std::pair<std::string, IUnknown*>>;
inline std::vector<ULONG> refCounts(const Objects& objects) {
    std::vector<ULONG> out;
    for (const auto& o : objects) { o.second->AddRef(); out.push_back(o.second->Release()); }
    return out;
}

// Every stage and slot range of the pipeline with an object in it, and another set to dirty it with.
struct Fixture {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ComPtr<ID3D11DeviceContext1> c1;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    UINT uavSlots = 8;
    bool ok = true;
    ComPtr<ID3D11VertexShader> vs[2];
    ComPtr<ID3D11HullShader> hs[2];
    ComPtr<ID3D11DomainShader> ds[2];
    ComPtr<ID3D11GeometryShader> gs[2];
    ComPtr<ID3D11PixelShader> ps[2];
    ComPtr<ID3D11ComputeShader> cs[2], csWrite;
    ComPtr<ID3D11InputLayout> layout[2];
    ComPtr<ID3D11Buffer> vb[6], ib[2], cbuf[8], so[3];
    ComPtr<ID3D11Texture2D> srvTex[6], uavTex[16], rtTex[6], dsTex[2];
    ComPtr<ID3D11ShaderResourceView> srv[6];
    ComPtr<ID3D11UnorderedAccessView> uav[16];
    ComPtr<ID3D11RenderTargetView> rtv[6];
    ComPtr<ID3D11DepthStencilView> dsv[2];
    ComPtr<ID3D11SamplerState> smp[6];
    ComPtr<ID3D11BlendState> blend[2];
    ComPtr<ID3D11DepthStencilState> depthState[2];
    ComPtr<ID3D11RasterizerState> raster[2];
    ComPtr<ID3D11Predicate> pred[2];

    Fixture(ID3D11Device* d, ID3D11DeviceContext* c) : device(d), context(c) {
        c->QueryInterface(IID_PPV_ARGS(c1.GetAddressOf()));
        level = d->GetFeatureLevel();
        uavSlots = level >= D3D_FEATURE_LEVEL_11_1 ? 64 : 8;
        auto need = [&](HRESULT hr, const char* what) { if (FAILED(hr)) { std::printf("context isolation rig: %s failed 0x%08X\n", what, unsigned(hr)); ok = false; } };
        const auto code = [&](const char* src, const char* target) { return hlsl(src, target); };
        const auto vsCode = code("float4 main(float4 p : POSITION) : SV_Position { return p; }", "vs_5_0");
        const auto psCode = code("float4 main(float4 p : SV_Position) : SV_Target { return float4(1, 0, 0, 1); }", "ps_5_0");
        const auto csCode = code("[numthreads(1, 1, 1)] void main() {}", "cs_5_0");
        const auto csWriteCode = code("RWTexture2D<float> u : register(u0); [numthreads(1, 1, 1)] void main() { u[uint2(0, 0)] = 1.0; }", "cs_5_0");
        const auto gsCode = code("struct O { float4 p : SV_Position; };"
                                 "[maxvertexcount(3)] void main(triangle float4 p[3] : SV_Position, inout TriangleStream<O> s) {"
                                 " for (int i = 0; i < 3; ++i) { O o; o.p = p[i]; s.Append(o); } }", "gs_5_0");
        const auto hsCode = code("struct CP { float4 p : POSITION; };"
                                 "struct PC { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; };"
                                 "PC pcf(InputPatch<CP, 3> p) { PC o; o.e[0] = o.e[1] = o.e[2] = 1; o.i = 1; return o; }"
                                 "[domain(\"tri\")][partitioning(\"integer\")][outputtopology(\"triangle_cw\")][outputcontrolpoints(3)][patchconstantfunc(\"pcf\")]"
                                 "CP main(InputPatch<CP, 3> p, uint i : SV_OutputControlPointID) { return p[i]; }", "hs_5_0");
        const auto dsCode = code("struct CP { float4 p : POSITION; };"
                                 "struct PC { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; };"
                                 "[domain(\"tri\")] float4 main(PC pc, float3 uvw : SV_DomainLocation, const OutputPatch<CP, 3> p) : SV_Position {"
                                 " return p[0].p * uvw.x + p[1].p * uvw.y + p[2].p * uvw.z; }", "ds_5_0");
        if (!vsCode || !psCode || !csCode || !csWriteCode || !gsCode || !hsCode || !dsCode) { ok = false; return; }
        for (int i = 0; i < 2; ++i) {
            need(d->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, vs[i].GetAddressOf()), "vertex shader");
            need(d->CreateHullShader(hsCode->GetBufferPointer(), hsCode->GetBufferSize(), nullptr, hs[i].GetAddressOf()), "hull shader");
            need(d->CreateDomainShader(dsCode->GetBufferPointer(), dsCode->GetBufferSize(), nullptr, ds[i].GetAddressOf()), "domain shader");
            need(d->CreateGeometryShader(gsCode->GetBufferPointer(), gsCode->GetBufferSize(), nullptr, gs[i].GetAddressOf()), "geometry shader");
            need(d->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, ps[i].GetAddressOf()), "pixel shader");
            need(d->CreateComputeShader(csCode->GetBufferPointer(), csCode->GetBufferSize(), nullptr, cs[i].GetAddressOf()), "compute shader");
            D3D11_INPUT_ELEMENT_DESC el{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, UINT(i), 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
            need(d->CreateInputLayout(&el, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), layout[i].GetAddressOf()), "input layout");
        }
        need(d->CreateComputeShader(csWriteCode->GetBufferPointer(), csWriteCode->GetBufferSize(), nullptr, csWrite.GetAddressOf()), "writing compute shader");
        const auto buffer = [&](ComPtr<ID3D11Buffer>& out, UINT bind, UINT bytes) {
            D3D11_BUFFER_DESC bd{}; bd.ByteWidth = bytes; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = bind;
            need(d->CreateBuffer(&bd, nullptr, out.GetAddressOf()), "buffer");
        };
        for (auto& b : vb) buffer(b, D3D11_BIND_VERTEX_BUFFER, 256);
        for (auto& b : ib) buffer(b, D3D11_BIND_INDEX_BUFFER, 256);
        static const UINT cbBytes[8] = {272, 512, 4096, 1024, 256, 2048, 4096, 4096};   // 272 is 17 constants: not a window a runtime may take whole
        for (int i = 0; i < 8; ++i) buffer(cbuf[i], D3D11_BIND_CONSTANT_BUFFER, cbBytes[i]);
        for (auto& b : so) buffer(b, D3D11_BIND_STREAM_OUTPUT, 1024);
        for (int i = 0; i < 6; ++i) {
            srvTex[i] = texture(d, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
            need(d->CreateShaderResourceView(srvTex[i].Get(), nullptr, srv[i].GetAddressOf()), "shader resource view");
            rtTex[i] = texture(d, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
            need(d->CreateRenderTargetView(rtTex[i].Get(), nullptr, rtv[i].GetAddressOf()), "render target view");
        }
        for (int i = 0; i < 2; ++i) {
            dsTex[i] = texture(d, 16, 16, DXGI_FORMAT_D32_FLOAT, D3D11_BIND_DEPTH_STENCIL);
            need(d->CreateDepthStencilView(dsTex[i].Get(), nullptr, dsv[i].GetAddressOf()), "depth-stencil view");
        }
        for (int i = 0; i < 16; ++i) {
            uavTex[i] = texture(d, 4, 4, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
            need(d->CreateUnorderedAccessView(uavTex[i].Get(), nullptr, uav[i].GetAddressOf()), "unordered access view");
        }
        for (int i = 0; i < 6; ++i) {
            D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MipLODBias = .25f * float(i) + .125f; sd.MaxLOD = D3D11_FLOAT32_MAX;   // every one its own description, so its own object, and none the resolver's own (the device shares identical state objects)
            need(d->CreateSamplerState(&sd, smp[i].GetAddressOf()), "sampler state");
        }
        for (int i = 0; i < 2; ++i) {
            D3D11_BLEND_DESC bd{}; bd.AlphaToCoverageEnable = i ? TRUE : FALSE;
            auto& rt = bd.RenderTarget[0]; rt.BlendEnable = TRUE; rt.SrcBlend = i ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA;
            rt.DestBlend = i ? D3D11_BLEND_ONE : D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_ZERO; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            need(d->CreateBlendState(&bd, blend[i].GetAddressOf()), "blend state");
            D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = i ? FALSE : TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc = i ? D3D11_COMPARISON_LESS : D3D11_COMPARISON_GREATER; dd.StencilEnable = i ? FALSE : TRUE;
            dd.StencilReadMask = 0xF0; dd.StencilWriteMask = 0x0F;
            dd.FrontFace = dd.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
            need(d->CreateDepthStencilState(&dd, depthState[i].GetAddressOf()), "depth-stencil state");
            D3D11_RASTERIZER_DESC rd{}; rd.FillMode = i ? D3D11_FILL_SOLID : D3D11_FILL_WIREFRAME; rd.CullMode = i ? D3D11_CULL_NONE : D3D11_CULL_FRONT;
            rd.DepthClipEnable = TRUE; rd.ScissorEnable = i ? FALSE : TRUE; rd.DepthBias = 3 + i;   // not the resolver's no-cull state: the device shares identical ones
            need(d->CreateRasterizerState(&rd, raster[i].GetAddressOf()), "rasterizer state");
            D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
            need(d->CreatePredicate(&qd, pred[i].GetAddressOf()), "predicate");
            c->Begin(pred[i].Get()); c->End(pred[i].Get());   // issued, so SetPredication has a finished query to name
        }
    }

    // Which UAV slots each side uses: the game's, and the work's.
    std::vector<UINT> gameCsUavSlots() const { return uavSlots == 64 ? std::vector<UINT>{0, 1, 2, 5, 40, 63} : std::vector<UINT>{0, 1, 2, 5, 6, 7}; }
    std::vector<UINT> workCsUavSlots() const { return uavSlots == 64 ? std::vector<UINT>{1, 3, 62} : std::vector<UINT>{1, 3, 4}; }

    void setShader(ID3D11DeviceContext* c, unsigned st, int which) {
        switch (st) {
        case 0: c->VSSetShader(vs[which].Get(), nullptr, 0); break;
        case 1: c->HSSetShader(hs[which].Get(), nullptr, 0); break;
        case 2: c->DSSetShader(ds[which].Get(), nullptr, 0); break;
        case 3: c->GSSetShader(gs[which].Get(), nullptr, 0); break;
        case 4: c->PSSetShader(ps[which].Get(), nullptr, 0); break;
        default: c->CSSetShader(cs[which].Get(), nullptr, 0); break;
        }
    }
    // Fills every shader-stage slot of every stage: `base` 0 is the game's objects, 3 the work's.
    void bindStages(ID3D11DeviceContext* c, int which, int base) {
        ID3D11DeviceContext1* c1p = nullptr;
        c->QueryInterface(IID_PPV_ARGS(&c1p));
        const StageApi* api = stageApis();
        for (unsigned st = 0; st < kStages; ++st) {
            setShader(c, st, which);
            ID3D11ShaderResourceView* s[128]; for (UINT i = 0; i < 128; ++i) s[i] = srv[base + (i + st) % 3].Get();
            (c->*api[st].setSrv)(0, 128, s);
            ID3D11SamplerState* m[16]; for (UINT i = 0; i < 16; ++i) m[i] = smp[base + (i + st) % 3].Get();
            (c->*api[st].setSmp)(0, 16, m);
            // Every slot a whole buffer through the legacy call (the runtime's own answer for its window is read back by the oracle,
            // so whatever it reports is what must come back), then D3D11.1 windows into a 4096-byte buffer over two of them.
            ID3D11Buffer* b[14];
            for (UINT i = 0; i < 14; ++i) b[i] = cbuf[(which ? 4 : 0) + (i + st) % (which ? 2 : 4)].Get();
            (c->*api[st].setCb)(0, 14, b);
            if (which == 0) {
                ID3D11Buffer* w = cbuf[6].Get(); const UINT first3 = 16, num3 = 32, first7 = 32, num7 = 16;
                (c1p->*api[st].setCb1)(3, 1, &w, &first3, &num3);
                (c1p->*api[st].setCb1)(7, 1, &w, &first7, &num7);
            } else {
                ID3D11Buffer* w = cbuf[7].Get(); const UINT first = 48, num = 16;
                (c1p->*api[st].setCb1)(1, 1, &w, &first, &num);
            }
        }
        c1p->Release();
    }
    // The game's pipeline: every stage and every range, one object per slot.
    void bindGame() {
        context->ClearState();
        // IA
        context->IASetInputLayout(layout[0].Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        {
            ID3D11Buffer* b[32] = {}; UINT stride[32] = {}, offset[32] = {};
            b[0] = vb[0].Get(); stride[0] = 16; offset[0] = 0;
            b[1] = vb[1].Get(); stride[1] = 32; offset[1] = 4;
            b[5] = vb[2].Get(); stride[5] = 48; offset[5] = 8;
            b[31] = vb[3].Get(); stride[31] = 64; offset[31] = 12;
            context->IASetVertexBuffers(0, 32, b, stride, offset);
        }
        context->IASetIndexBuffer(ib[0].Get(), DXGI_FORMAT_R16_UINT, 6);
        bindStages(context, 0, 0);
        // CS UAVs: the game's slots, out to the last
        {
            ID3D11UnorderedAccessView* u[64] = {}; int n = 0;
            for (UINT s : gameCsUavSlots()) u[s] = uav[n++].Get();
            context->CSSetUnorderedAccessViews(0, uavSlots, u, nullptr);
        }
        // SO: slots 0 and 2
        {
            ID3D11Buffer* t[3] = {so[0].Get(), nullptr, so[1].Get()}; const UINT off[3] = {0, 0, 0};
            context->SOSetTargets(3, t, off);
        }
        // OM: three render targets, depth, two UAVs at slots 4 and 6, blend, depth-stencil
        {
            ID3D11RenderTargetView* r[3] = {rtv[0].Get(), rtv[1].Get(), rtv[2].Get()};
            ID3D11UnorderedAccessView* u[3] = {uav[6].Get(), nullptr, uav[7].Get()};
            context->OMSetRenderTargetsAndUnorderedAccessViews(3, r, dsv[0].Get(), 4, 3, u, nullptr);
            const FLOAT factor[4] = {.25f, .5f, .75f, 1.f};
            context->OMSetBlendState(blend[0].Get(), factor, 0xFFFFFFF0u);
            context->OMSetDepthStencilState(depthState[0].Get(), 0x42);
        }
        // RS
        context->RSSetState(raster[0].Get());
        {
            const D3D11_VIEWPORT vp[3] = {{0, 0, 16, 16, 0, 1}, {1, 2, 3, 4, .1f, .9f}, {5, 6, 7, 8, .2f, .8f}};
            context->RSSetViewports(3, vp);
            const D3D11_RECT rc[2] = {{1, 1, 9, 9}, {2, 2, 12, 12}};
            context->RSSetScissorRects(2, rc);
        }
        context->SetPredication(pred[0].Get(), TRUE);
    }
    // What a backend would leave: every stage, every slot range and every state given another object, and a dispatch run.
    // `withPredication` is for the block-level case; a predicate would also predicate the resolver's own later work.
    void dirty(ID3D11DeviceContext* c, bool withPredication) {
        c->IASetInputLayout(layout[1].Get());
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        {
            ID3D11Buffer* b[4] = {vb[4].Get(), vb[5].Get(), nullptr, nullptr}; const UINT stride[4] = {8, 24, 0, 0}, offset[4] = {2, 6, 0, 0};
            c->IASetVertexBuffers(2, 4, b, stride, offset);
        }
        c->IASetIndexBuffer(ib[1].Get(), DXGI_FORMAT_R32_UINT, 8);
        bindStages(c, 1, 3);
        {   // the work: a dispatch that writes a UAV, then the state it leaves bound
            ID3D11UnorderedAccessView* w = uav[15].Get();
            c->CSSetShader(csWrite.Get(), nullptr, 0);
            c->CSSetUnorderedAccessViews(0, 1, &w, nullptr);
            c->Dispatch(1, 1, 1);
            c->CSSetShader(cs[1].Get(), nullptr, 0);
        }
        {
            ID3D11UnorderedAccessView* u[64] = {}; int n = 8;
            for (UINT s : workCsUavSlots()) u[s] = uav[n++].Get();
            c->CSSetUnorderedAccessViews(0, uavSlots, u, nullptr);
        }
        {
            ID3D11Buffer* t[2] = {nullptr, so[2].Get()}; const UINT off[2] = {0, 0};
            c->SOSetTargets(2, t, off);
        }
        {
            ID3D11RenderTargetView* r[2] = {rtv[3].Get(), rtv[4].Get()};
            ID3D11UnorderedAccessView* u[2] = {uav[11].Get(), uav[12].Get()};
            c->OMSetRenderTargetsAndUnorderedAccessViews(2, r, dsv[1].Get(), 5, 2, u, nullptr);
            const FLOAT factor[4] = {1.f, 0.f, 0.f, 0.f};
            c->OMSetBlendState(blend[1].Get(), factor, 0x0000FFFFu);
            c->OMSetDepthStencilState(depthState[1].Get(), 7);
        }
        c->RSSetState(raster[1].Get());
        {
            const D3D11_VIEWPORT vp[2] = {{2, 2, 5, 5, 0, .5f}, {3, 3, 6, 6, .5f, 1}};
            c->RSSetViewports(2, vp);
            const D3D11_RECT rc[4] = {{0, 0, 1, 1}, {1, 1, 2, 2}, {2, 2, 3, 3}, {3, 3, 4, 4}};
            c->RSSetScissorRects(4, rc);
        }
        if (withPredication) c->SetPredication(pred[1].Get(), FALSE);
    }
    Objects objects() const {
        Objects o;
        const auto add = [&](const char* name, int i, IUnknown* p) { if (p) o.emplace_back(std::string(name) + "[" + std::to_string(i) + "]", p); };
        for (int i = 0; i < 2; ++i) {
            add("vs", i, vs[i].Get()); add("hs", i, hs[i].Get()); add("ds", i, ds[i].Get()); add("gs", i, gs[i].Get()); add("ps", i, ps[i].Get());
            add("cs", i, cs[i].Get()); add("layout", i, layout[i].Get()); add("ib", i, ib[i].Get()); add("dsTex", i, dsTex[i].Get());
            add("dsv", i, dsv[i].Get()); add("blend", i, blend[i].Get()); add("depthState", i, depthState[i].Get());
            add("raster", i, raster[i].Get()); add("pred", i, pred[i].Get());
        }
        add("csWrite", 0, csWrite.Get());
        for (int i = 0; i < 6; ++i) {
            add("vb", i, vb[i].Get()); add("srvTex", i, srvTex[i].Get()); add("srv", i, srv[i].Get()); add("rtTex", i, rtTex[i].Get());
            add("rtv", i, rtv[i].Get()); add("smp", i, smp[i].Get());
        }
        for (int i = 0; i < 8; ++i) add("cbuf", i, cbuf[i].Get());
        for (int i = 0; i < 3; ++i) add("so", i, so[i].Get());
        for (int i = 0; i < 16; ++i) { add("uavTex", i, uavTex[i].Get()); add("uav", i, uav[i].Get()); }
        return o;
    }
};


// A one-slot change to the game's state that the oracle must notice: the label it must name, and the change.
struct Mutation {
    std::string label;
    std::function<void(Fixture&)> apply;
};

inline std::vector<Mutation> mutations() {
    std::vector<Mutation> m;
    // shaders
    m.push_back({"VS.shader", [](Fixture& f) { f.context->VSSetShader(f.vs[1].Get(), nullptr, 0); }});
    m.push_back({"HS.shader", [](Fixture& f) { f.context->HSSetShader(f.hs[1].Get(), nullptr, 0); }});
    m.push_back({"DS.shader", [](Fixture& f) { f.context->DSSetShader(f.ds[1].Get(), nullptr, 0); }});
    m.push_back({"GS.shader", [](Fixture& f) { f.context->GSSetShader(f.gs[1].Get(), nullptr, 0); }});
    m.push_back({"PS.shader", [](Fixture& f) { f.context->PSSetShader(f.ps[1].Get(), nullptr, 0); }});
    m.push_back({"CS.shader", [](Fixture& f) { f.context->CSSetShader(f.cs[1].Get(), nullptr, 0); }});
    // shader resources, one slot in each stage, at the near and far ends of the range
    static const struct { unsigned stage; UINT slot; } srvSlots[] = {{0, 0}, {1, 127}, {2, 64}, {3, 31}, {4, 77}, {5, 100}, {4, 1}, {5, 126}};
    for (const auto& e : srvSlots) {
        char label[48];
        std::snprintf(label, sizeof(label), "%s.srv[%u]", stageApis()[e.stage].name, e.slot);
        m.push_back({label, [e](Fixture& f) { ID3D11ShaderResourceView* v = f.srv[5].Get(); (f.context->*stageApis()[e.stage].setSrv)(e.slot, 1, &v); }});
    }
    // samplers
    static const struct { unsigned stage; UINT slot; } smpSlots[] = {{0, 15}, {1, 0}, {2, 7}, {3, 8}, {4, 9}, {5, 14}};
    for (const auto& e : smpSlots) {
        char label[48];
        std::snprintf(label, sizeof(label), "%s.sampler[%u]", stageApis()[e.stage].name, e.slot);
        m.push_back({label, [e](Fixture& f) { ID3D11SamplerState* s = f.smp[5].Get(); (f.context->*stageApis()[e.stage].setSmp)(e.slot, 1, &s); }});
    }
    // constant buffers: the buffer, then the window
    static const struct { unsigned stage; UINT slot; } cbSlots[] = {{0, 0}, {1, 13}, {2, 5}, {3, 1}, {4, 12}, {5, 9}};
    for (const auto& e : cbSlots) {
        char label[48];
        std::snprintf(label, sizeof(label), "%s.cb[%u]", stageApis()[e.stage].name, e.slot);
        m.push_back({label, [e](Fixture& f) {
            ID3D11Buffer* b = f.cbuf[6].Get(); const UINT first = 64, num = 16;
            (f.c1.Get()->*stageApis()[e.stage].setCb1)(e.slot, 1, &b, &first, &num);
        }});
    }
    m.push_back({"PS.cb[3].first", [](Fixture& f) { ID3D11Buffer* b = f.cbuf[6].Get(); const UINT first = 64, num = 32; f.c1->PSSetConstantBuffers1(3, 1, &b, &first, &num); }});
    m.push_back({"CS.cb[7].num", [](Fixture& f) { ID3D11Buffer* b = f.cbuf[6].Get(); const UINT first = 32, num = 32; f.c1->CSSetConstantBuffers1(7, 1, &b, &first, &num); }});
    // UAVs
    m.push_back({"CS.uav[5]", [](Fixture& f) { ID3D11UnorderedAccessView* u = f.uav[13].Get(); f.context->CSSetUnorderedAccessViews(5, 1, &u, nullptr); }});
    m.push_back({"CS.uav[2]", [](Fixture& f) { ID3D11UnorderedAccessView* u = nullptr; f.context->CSSetUnorderedAccessViews(2, 1, &u, nullptr); }});
    m.push_back({"OM.uav[6]", [](Fixture& f) {
        ID3D11RenderTargetView* r[3] = {f.rtv[0].Get(), f.rtv[1].Get(), f.rtv[2].Get()}; ID3D11UnorderedAccessView* u[3] = {f.uav[6].Get(), nullptr, f.uav[14].Get()};
        f.context->OMSetRenderTargetsAndUnorderedAccessViews(3, r, f.dsv[0].Get(), 4, 3, u, nullptr); }});
    // SO
    m.push_back({"SO[2]", [](Fixture& f) { ID3D11Buffer* t[3] = {f.so[0].Get(), nullptr, nullptr}; const UINT off[3] = {}; f.context->SOSetTargets(3, t, off); }});
    m.push_back({"SO[0]", [](Fixture& f) { ID3D11Buffer* t[3] = {f.so[2].Get(), nullptr, f.so[1].Get()}; const UINT off[3] = {}; f.context->SOSetTargets(3, t, off); }});
    // OM
    m.push_back({"OM.rtv[2]", [](Fixture& f) {
        ID3D11RenderTargetView* r[3] = {f.rtv[0].Get(), f.rtv[1].Get(), f.rtv[5].Get()}; f.context->OMSetRenderTargets(3, r, f.dsv[0].Get());
        ID3D11UnorderedAccessView* u[3] = {f.uav[6].Get(), nullptr, f.uav[7].Get()};
        f.context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, 4, 3, u, nullptr); }});
    m.push_back({"OM.dsv", [](Fixture& f) {
        ID3D11RenderTargetView* r[3] = {f.rtv[0].Get(), f.rtv[1].Get(), f.rtv[2].Get()};
        ID3D11UnorderedAccessView* u[3] = {f.uav[6].Get(), nullptr, f.uav[7].Get()};
        f.context->OMSetRenderTargetsAndUnorderedAccessViews(3, r, f.dsv[1].Get(), 4, 3, u, nullptr); }});
    m.push_back({"OM.blend", [](Fixture& f) { const FLOAT factor[4] = {.25f, .5f, .75f, 1.f}; f.context->OMSetBlendState(f.blend[1].Get(), factor, 0xFFFFFFF0u); }});
    m.push_back({"OM.blendFactor[3]", [](Fixture& f) { const FLOAT factor[4] = {.25f, .5f, .75f, .5f}; f.context->OMSetBlendState(f.blend[0].Get(), factor, 0xFFFFFFF0u); }});
    m.push_back({"OM.sampleMask", [](Fixture& f) { const FLOAT factor[4] = {.25f, .5f, .75f, 1.f}; f.context->OMSetBlendState(f.blend[0].Get(), factor, 0xFFFFFFF1u); }});
    m.push_back({"OM.ds", [](Fixture& f) { f.context->OMSetDepthStencilState(f.depthState[1].Get(), 0x42); }});
    m.push_back({"OM.stencilRef", [](Fixture& f) { f.context->OMSetDepthStencilState(f.depthState[0].Get(), 0x43); }});
    // RS
    m.push_back({"RS.state", [](Fixture& f) { f.context->RSSetState(f.raster[1].Get()); }});
    m.push_back({"RS.viewport[2]", [](Fixture& f) { const D3D11_VIEWPORT vp[3] = {{0, 0, 16, 16, 0, 1}, {1, 2, 3, 4, .1f, .9f}, {5, 6, 7, 9, .2f, .8f}}; f.context->RSSetViewports(3, vp); }});
    m.push_back({"RS.viewportCount", [](Fixture& f) { const D3D11_VIEWPORT vp[2] = {{0, 0, 16, 16, 0, 1}, {1, 2, 3, 4, .1f, .9f}}; f.context->RSSetViewports(2, vp); }});
    m.push_back({"RS.scissor[1]", [](Fixture& f) { const D3D11_RECT rc[2] = {{1, 1, 9, 9}, {2, 2, 12, 13}}; f.context->RSSetScissorRects(2, rc); }});
    m.push_back({"RS.scissorCount", [](Fixture& f) { const D3D11_RECT rc[1] = {{1, 1, 9, 9}}; f.context->RSSetScissorRects(1, rc); }});
    // IA
    m.push_back({"IA.layout", [](Fixture& f) { f.context->IASetInputLayout(f.layout[1].Get()); }});
    m.push_back({"IA.topology", [](Fixture& f) { f.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); }});
    m.push_back({"IA.vb[31].stride", [](Fixture& f) { ID3D11Buffer* b = f.vb[3].Get(); const UINT stride = 60, offset = 12; f.context->IASetVertexBuffers(31, 1, &b, &stride, &offset); }});
    m.push_back({"IA.vb[5].offset", [](Fixture& f) { ID3D11Buffer* b = f.vb[2].Get(); const UINT stride = 48, offset = 12; f.context->IASetVertexBuffers(5, 1, &b, &stride, &offset); }});
    m.push_back({"IA.vb[1].buf", [](Fixture& f) { ID3D11Buffer* b = f.vb[4].Get(); const UINT stride = 32, offset = 4; f.context->IASetVertexBuffers(1, 1, &b, &stride, &offset); }});
    m.push_back({"IA.ib.format", [](Fixture& f) { f.context->IASetIndexBuffer(f.ib[0].Get(), DXGI_FORMAT_R32_UINT, 8); }});
    m.push_back({"IA.ib.offset", [](Fixture& f) { f.context->IASetIndexBuffer(f.ib[0].Get(), DXGI_FORMAT_R16_UINT, 8); }});
    // predication
    m.push_back({"PRED.obj", [](Fixture& f) { f.context->SetPredication(f.pred[1].Get(), TRUE); }});
    m.push_back({"PRED.value", [](Fixture& f) { f.context->SetPredication(f.pred[0].Get(), FALSE); }});
    return m;
}

// ---- 1. the decision --------------------------------------------------------------------------------------------------------
// A stand-in for a device or a context: answers QueryInterface for the IIDs it is told to, and nothing else.
class Answerer : public IUnknown {
public:
    explicit Answerer(std::vector<GUID> answers) : answers_(std::move(answers)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        bool yes = riid == __uuidof(IUnknown);
        for (const GUID& g : answers_) yes = yes || riid == g;
        if (!yes) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    ULONG refs() const { return refs_; }

private:
    std::vector<GUID> answers_;
    ULONG refs_ = 1;
};
inline std::string guidText(const GUID& g) {
    char text[64];
    std::snprintf(text, sizeof(text), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
                  g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return text;
}

inline void decisionTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using namespace edvr;
    // The two IIDs, against DXMT's own text (src/d3d11/d3d11_interfaces.hpp: IMTLD3D11DeviceExt and IMTLD3D11ContextExt).
    check(guidText(kDxmtDeviceExtIid) == "efc77ae6-2179-4c0a-b844-7661ca0dcde7" && guidText(kDxmtContextExtIid) == "43ace3ce-1956-448b-a4eb-aee68bdeb283",
          "isolation: the two DXMT interface IDs are the ones DXMT's d3d11_interfaces.hpp defines");
    // The key.
    check(flatContextIsolationFromText("auto") == FlatContextIsolation::Auto && flatContextIsolationFromText("swap") == FlatContextIsolation::Swap &&
              flatContextIsolationFromText("capture") == FlatContextIsolation::Capture && flatContextIsolationFromText("CAPTURE") == FlatContextIsolation::Capture &&
              flatContextIsolationFromText("Swap") == FlatContextIsolation::Swap && flatContextIsolationFromText("") == FlatContextIsolation::Auto &&
              flatContextIsolationFromText("explicit") == FlatContextIsolation::Auto && flatContextIsolationFromText(nullptr) == FlatContextIsolation::Auto,
          "isolation: the key reads auto, swap and capture, in any case, and anything else (or nothing) as auto");
    // The markers, one at a time, on stand-in objects that answer the private interfaces as DXMT does.
    {
        Answerer none({}), onlyDevice({kDxmtDeviceExtIid}), onlyContext({kDxmtContextExtIid}), decoy({kDxmtContextExtIid});
        auto d = flatDetectDxmt(&none, &none);
        check(!d.dxmt() && d.markers == 0 && d.adapter[0] == '\0', "isolation: an object that answers no private interface is not DXMT");
        d = flatDetectDxmt(&onlyDevice, &none);
        check(d.markers == kDxmtDeviceInterface && d.positive(), "isolation: a device that answers IMTLD3D11DeviceExt is DXMT, by that marker alone");
        d = flatDetectDxmt(&none, &onlyContext);
        check(d.markers == kDxmtContextInterface && d.positive(), "isolation: a context that answers IMTLD3D11ContextExt is DXMT, by that marker alone");
        d = flatDetectDxmt(&onlyDevice, &onlyContext);
        check(d.markers == (kDxmtDeviceInterface | kDxmtContextInterface), "isolation: both interfaces answering are both named");
        d = flatDetectDxmt(&decoy, &none);   // the context's IID asked of the DEVICE: DXMT's device refuses it by name, and a stand-in that answers it is not the device marker
        check(d.markers == 0, "isolation: the device marker is the device's IID, not the context's");
        check(onlyDevice.refs() == 1 && onlyContext.refs() == 1 && none.refs() == 1, "isolation: every reference the markers took is released");
        check(!flatDetectDxmt(nullptr, nullptr).dxmt(), "isolation: no objects, no marker, no fault");
    }
    // The version resource: a real Windows blob (kernel32's), then the same blob patched to say DXMT where it says its product.
    {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        HRSRC found = k32 ? FindResourceW(k32, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16)) : nullptr;
        HGLOBAL loaded = found ? LoadResource(k32, found) : nullptr;
        const DWORD size = found ? SizeofResource(k32, found) : 0;
        const unsigned char* data = loaded ? static_cast<const unsigned char*>(LockResource(loaded)) : nullptr;
        check(data && size > 100, "isolation: kernel32's version resource can be read (the layout the scanner is held to)");
        if (data && size > 100) {
            std::vector<unsigned char> blob(data, data + size);
            check(!flatVersionBlobNamesDxmt(blob.data(), blob.size()) && !flatModuleNamesDxmt(k32), "isolation: a real Windows version resource does not say DXMT");
            // Find "ProductName" and overwrite the start of its value with "DXMT".
            static const wchar_t key[] = L"ProductName";
            size_t at = 0; bool have = false;
            for (size_t i = 0; i + sizeof(key) <= blob.size(); i += 2)
                if (!std::memcmp(blob.data() + i, key, sizeof(key))) { at = i + sizeof(key); have = true; break; }
            check(have, "isolation: kernel32's version resource names a ProductName");
            if (have) {
                if (at + 2 <= blob.size() && blob[at] == 0 && blob[at + 1] == 0) at += 2;   // the padding word, when the layout has one
                static const wchar_t dxmt[] = L"DXMT";
                if (at + sizeof(dxmt) <= blob.size()) std::memcpy(blob.data() + at, dxmt, sizeof(dxmt));
                check(flatVersionBlobNamesDxmt(blob.data(), blob.size()), "isolation: the scanner finds DXMT in ProductName's value in a real blob's layout");
                // The wrong key: DXMT as the company and another product does not count.
                std::vector<unsigned char> other(blob);
                static const wchar_t product[] = L"Other";
                std::memcpy(other.data() + at, product, sizeof(product));
                check(!flatVersionBlobNamesDxmt(other.data(), other.size()), "isolation: the scanner answers to ProductName's own value");
            }
        }
        // Synthetic layouts: the value directly after the key, and after one padding word.
        const auto make = [](bool pad, const wchar_t* value) {
            std::vector<unsigned char> b(16, 0);
            static const wchar_t key[] = L"ProductName";
            b.insert(b.end(), reinterpret_cast<const unsigned char*>(key), reinterpret_cast<const unsigned char*>(key) + sizeof(key));
            if (pad) { b.push_back(0); b.push_back(0); }
            const size_t n = (std::wcslen(value) + 1) * sizeof(wchar_t);
            b.insert(b.end(), reinterpret_cast<const unsigned char*>(value), reinterpret_cast<const unsigned char*>(value) + n);
            return b;
        };
        check(flatVersionBlobNamesDxmt(make(false, L"DXMT").data(), make(false, L"DXMT").size()) && flatVersionBlobNamesDxmt(make(true, L"DXMT").data(), make(true, L"DXMT").size()) &&
                  !flatVersionBlobNamesDxmt(make(true, L"DXMT2").data(), make(true, L"DXMT2").size()) && !flatVersionBlobNamesDxmt(make(false, L"dxmt").data(), make(false, L"dxmt").size()) &&
                  !flatVersionBlobNamesDxmt(nullptr, 10) && !flatVersionBlobNamesDxmt(make(true, L"DXMT").data(), 20),
              "isolation: the scanner takes the value after the key with or without a padding word, exactly DXMT, and a truncated or missing blob is no");
        check(!flatModuleNamesDxmt(nullptr) && !flatModuleNamesDxmt(GetModuleHandleW(nullptr)) && !flatObjectModuleNamesDxmt(nullptr),
              "isolation: no module, and this executable (which has no version resource), are not DXMT");
    }
    // The adapter name.
    check(flatAdapterNameIsApple("Apple M4 Max") && flatAdapterNameIsApple("APPLE M1") && !flatAdapterNameIsApple("NVIDIA GeForce RTX 4090") &&
              !flatAdapterNameIsApple("Pineapple") && !flatAdapterNameIsApple("") && !flatAdapterNameIsApple(nullptr),
          "isolation: the adapter fallback is a name that begins with Apple");
    // The real device: WARP answers no marker, so auto is the swap.
    {
        const FlatDxmtDetection real = flatDetectDxmt(device, context);
        std::printf("flat context isolation: the WARP device answers %s (adapter \"%s\")\n", real.markers ? "a DXMT marker" : "no DXMT marker", real.adapter);
        check(!real.dxmt() && real.markers == 0, "isolation: the real WARP device answers none of the four DXMT markers, through the real QueryInterface and module lookups");
        check(flatChooseContextIsolation(FlatContextIsolation::Auto, real).mode == FlatContextIsolation::Swap, "isolation: auto on the real device is the swap");
    }
    // The choice, for every key and every kind of marker.
    {
        FlatDxmtDetection none, adapterOnly, positive, both;
        adapterOnly.markers = kDxmtAdapterName;
        positive.markers = kDxmtDeviceInterface;
        both.markers = kDxmtContextInterface | kDxmtAdapterName;
        const auto c = [](FlatContextIsolation r, const FlatDxmtDetection& d) { return flatChooseContextIsolation(r, d); };
        check(c(FlatContextIsolation::Auto, none).mode == FlatContextIsolation::Swap && !c(FlatContextIsolation::Auto, none).forced &&
                  c(FlatContextIsolation::Auto, adapterOnly).mode == FlatContextIsolation::Capture && c(FlatContextIsolation::Auto, positive).mode == FlatContextIsolation::Capture &&
                  c(FlatContextIsolation::Auto, both).mode == FlatContextIsolation::Capture && !c(FlatContextIsolation::Auto, both).forced,
              "isolation: auto is the swap unless a marker answered, and capture when any did (the adapter name alone included, as the fallback)");
        check(c(FlatContextIsolation::Swap, positive).mode == FlatContextIsolation::Swap && c(FlatContextIsolation::Swap, positive).forced &&
                  c(FlatContextIsolation::Capture, none).mode == FlatContextIsolation::Capture && c(FlatContextIsolation::Capture, none).forced,
              "isolation: a forced mode is honoured whatever the device says");
        // The breadcrumbs' device gate is the markers' answer and nothing else: no key, no isolation mode is an argument, so forcing the
        // capture on a Windows device (or anywhere) cannot open it, and forcing the swap on DXMT cannot shut it.
        check(!flatCrumbsWantedFor(none) && flatCrumbsWantedFor(adapterOnly) && flatCrumbsWantedFor(positive) && flatCrumbsWantedFor(both),
              "isolation: the crumbs' gate is open for a device any marker calls DXMT (the adapter name included), and shut for one none does");
        bool independent = true;
        for (const FlatContextIsolation request : {FlatContextIsolation::Auto, FlatContextIsolation::Swap, FlatContextIsolation::Capture})
            for (const FlatDxmtDetection* d : {&none, &adapterOnly, &positive, &both})
                independent = independent && flatCrumbsWantedFor(*d) == d->dxmt() &&
                              (request != FlatContextIsolation::Capture || d->dxmt() || !flatCrumbsWantedFor(*d));
        check(independent && flatChooseContextIsolation(FlatContextIsolation::Capture, none).mode == FlatContextIsolation::Capture && !flatCrumbsWantedFor(none) &&
                  flatChooseContextIsolation(FlatContextIsolation::Swap, positive).mode == FlatContextIsolation::Swap && flatCrumbsWantedFor(positive),
              "isolation: forcing the capture on a device no marker calls DXMT leaves the crumbs' gate shut, and forcing the swap on a DXMT device leaves it open");
        // The one line, for each case.
        char line[512];
        flatFormatContextIsolationLine(c(FlatContextIsolation::Auto, none), none, line, sizeof(line));
        check(!std::strcmp(line, "flat resolver: context isolation by context state swap (no DXMT marker)"), "isolation: the log line for the default route");
        FlatDxmtDetection named = positive; named.markers |= kDxmtContextInterface | kDxmtVersionResource | kDxmtAdapterName;
        std::strcpy(named.adapter, "Apple M4 Max");
        flatFormatContextIsolationLine(c(FlatContextIsolation::Auto, named), named, line, sizeof(line));
        check(!std::strcmp(line, "flat resolver: context isolation by explicit state capture (DXMT: device interface IMTLD3D11DeviceExt, context interface "
                                 "IMTLD3D11ContextExt, module version resource ProductName=DXMT, adapter name \"Apple M4 Max\")"),
              "isolation: the log line for DXMT names every marker that answered");
        FlatDxmtDetection fallback; fallback.markers = kDxmtAdapterName; std::strcpy(fallback.adapter, "Apple M4 Max");
        flatFormatContextIsolationLine(c(FlatContextIsolation::Auto, fallback), fallback, line, sizeof(line));
        check(!std::strcmp(line, "flat resolver: context isolation by explicit state capture (DXMT: adapter name \"Apple M4 Max\")"),
              "isolation: the log line when only the adapter name answered says so");
        flatFormatContextIsolationLine(c(FlatContextIsolation::Capture, none), none, line, sizeof(line));
        check(!std::strcmp(line, "flat resolver: context isolation by explicit state capture (advanced.flat_context_isolation=capture; DXMT markers: none)"),
              "isolation: the log line for a forced capture");
        flatFormatContextIsolationLine(c(FlatContextIsolation::Swap, positive), positive, line, sizeof(line));
        check(!std::strcmp(line, "flat resolver: context isolation by context state swap (advanced.flat_context_isolation=swap; DXMT markers: device interface "
                                 "IMTLD3D11DeviceExt; DXMT aborts in the swap)"),
              "isolation: the log line for a forced swap on a DXMT device says it aborts");
        // The ranges.
        const FlatContextRanges a = flatContextRanges(D3D_FEATURE_LEVEL_11_1, false), b = flatContextRanges(D3D_FEATURE_LEVEL_11_0, false),
                                 dx = flatContextRanges(D3D_FEATURE_LEVEL_12_1, true);
        check(a.vertexBuffers == 32 && a.uavs == 64 && b.vertexBuffers == 32 && b.uavs == 8 && dx.vertexBuffers == 16 && dx.uavs == 64,
              "isolation: the capture reaches D3D11's ranges (vertex buffers 32, UAVs 64 from feature level 11_1 and 8 below), and DXMT's vertex-buffer table (16)");
    }
}

// ---- 2. the block, stage by stage and slot by slot ------------------------------------------------------------------------
inline void blockTests(Fixture& fx) {
    namespace ct = hdr_crumb_trail;
    using namespace edvr;
    ID3D11DeviceContext* context = fx.context;
    ID3D11DeviceContext1* c1 = fx.c1.Get();
    const FlatContextRanges ranges = flatContextRanges(fx.level, false);
    const Objects objects = fx.objects();
    check(ranges.uavs == fx.uavSlots, "isolation rig: the block's UAV range is the oracle's");
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;

    // The oracle reads the WHOLE of every documented range: count what it covers.
    fx.bindGame();
    const Snap before = snapshot(c1, fx.uavSlots);
    const std::vector<ULONG> refsBefore = refCounts(objects);
    size_t boundValues = 0;
    for (const auto& e : before.v) boundValues += e.second != 0;
    std::printf("flat context isolation: the oracle reads %zu values of the game's state (%zu non-zero), and %zu objects' reference counts; feature level 0x%X, %u UAV slots\n",
                before.v.size(), boundValues, objects.size(), unsigned(fx.level), fx.uavSlots);
    {
        // Exactly the ranges the block documents: IA (layout, topology, 32 vertex buffers with stride and offset, the index buffer's
        // three), six stages of shader + 128 shader resources + 14 constant buffers + 16 samplers, the two window values of each
        // bound constant buffer (all 14 are bound), the CS UAVs, SO's four, OM (8 render targets, depth, the UAVs, blend with its
        // mask and factor, depth-stencil with its reference), RS (state, three viewports at six values, two scissors at four) and predication.
        const size_t expected = 2 + 32 * 3 + 3 + kStages * (1 + 128 + 14 + 16) + kStages * 14 * 2 + fx.uavSlots + 4 + (8 + 1 + fx.uavSlots) + 8 +
                                (1 + 1 + 3 * 6 + 1 + 2 * 4) + 2;
        check(before.v.size() == expected, "isolation: the oracle reads exactly the stages and slot ranges the block documents, and all of them");
        if (before.v.size() != expected) std::printf("  (the oracle read %zu values where the documented ranges make %zu)\n", before.v.size(), expected);
    }
    // The game's state really is full, in every stage: the oracle sees a bound object at the ends and middle of every range.
    {
        bool filled = true;
        for (unsigned st = 0; st < kStages; ++st) {
            const char* n = stageApis()[st].name;
            for (UINT slot : {0u, 1u, 63u, 64u, 126u, 127u}) { char key[64]; std::snprintf(key, sizeof(key), "%s.srv[%u]", n, slot); filled = filled && before.v.at(key) != 0; }
            for (UINT slot : {0u, 7u, 13u}) { char key[64]; std::snprintf(key, sizeof(key), "%s.cb[%u]", n, slot); filled = filled && before.v.at(key) != 0; }
            for (UINT slot : {0u, 9u, 15u}) { char key[64]; std::snprintf(key, sizeof(key), "%s.sampler[%u]", n, slot); filled = filled && before.v.at(key) != 0; }
            char key[64]; std::snprintf(key, sizeof(key), "%s.shader", n); filled = filled && before.v.at(key) != 0;
        }
        filled = filled && before.v.at("IA.layout") != 0 && before.v.at("IA.vb[31].buf") != 0 && before.v.at("IA.ib.buf") != 0 && before.v.at("CS.uav[0]") != 0 &&
                 before.v.at("SO[0]") != 0 && before.v.at("SO[2]") != 0 && before.v.at("OM.rtv[2]") != 0 && before.v.at("OM.dsv") != 0 && before.v.at("OM.uav[4]") != 0 &&
                 before.v.at("OM.uav[6]") != 0 && before.v.at("OM.blend") != 0 && before.v.at("OM.ds") != 0 && before.v.at("RS.state") != 0 && before.v.at("RS.viewportCount") == 3 &&
                 before.v.at("RS.scissorCount") == 2 && before.v.at("PRED.obj") != 0 && before.v.at("PRED.value") == 1 &&
                 before.v.at("PS.cb[3].first") == 16 && before.v.at("PS.cb[3].num") == 32 && before.v.at("CS.cb[7].first") == 32 && before.v.at("CS.cb[7].num") == 16;
        if (fx.uavSlots == 64) filled = filled && before.v.at("CS.uav[63]") != 0 && before.v.at("CS.uav[40]") != 0;
        check(filled, "isolation: the game's state has an object in every stage, every range's ends and middle, both UAV kinds, SO, predication and the D3D11.1 windows");
    }

    // ---- the cycle the resolver makes, with the crumbs live (the DXMT case: the crumbs' device gate open) --------------------------
    crumbLines.clear();
    edvr::hdrCrumbReset();
    edvr::hdrCrumbEnable(true);
    edvr::hdrCrumbAdmit(1, "dlss");
    edvr::hdrCrumbReach(1, "dlss", "resolve");
    FlatContextState block;
    block.capture(c1, ranges, edvr::hdrCrumbFirstCapture(true));
    check(block.holding(), "isolation: a block that has captured holds the state");
    check(firstDifference(before, snapshot(c1, fx.uavSlots)).empty(), "isolation: capturing changes nothing on the context");
    // The resolver's own ClearState, then what it relies on: the defaults.
    context->ClearState();
    {
        const Snap cleared = snapshot(c1, fx.uavSlots);
        std::string nonDefault;
        for (const auto& e : cleared.v) {
            const std::string& key = e.first;
            uint64_t want = 0;
            if (key == "OM.sampleMask") want = 0xFFFFFFFFu;
            else if (key.rfind("OM.blendFactor[", 0) == 0) want = floatBits(1.0f);
            else if (key.find(".cb[") != std::string::npos && (key.size() > 6 && (key.compare(key.size() - 6, 6, ".first") == 0 || key.compare(key.size() - 4, 4, ".num") == 0))) continue;
            if (e.second != want) { nonDefault = key; break; }
        }
        check(nonDefault.empty(), "isolation: after ClearState the context is at the defaults the resolver relies on (every slot of every stage empty, no predicate, no viewport)");
        if (!nonDefault.empty()) std::printf("  (first non-default value after ClearState: %s = %llu)\n", nonDefault.c_str(), static_cast<unsigned long long>(cleared.v.at(nonDefault)));
    }
    // A backend's work: every stage, every slot range, another object in each, and a dispatch; then ClearState, as ~Isolate does.
    fx.dirty(context, true);
    {
        size_t differing = 0;
        firstDifference(before, snapshot(c1, fx.uavSlots), &differing);
        check(differing > 1000, "isolation: the stand-in backend changes most of the values the oracle reads (every stage and slot)");
        std::printf("flat context isolation: the stand-in backend changed %zu of %zu values\n", differing, before.v.size());
    }
    context->ClearState();
    block.restore(c1, edvr::hdrCrumbFirstRestore(true));
    { edvr::HdrCrumbFrameEnd end(0); }
    check(!block.holding(), "isolation: a block that has restored holds nothing");
    {
        size_t differing = 0;
        const std::string first = firstDifference(before, snapshot(c1, fx.uavSlots), &differing);
        check(first.empty(), "isolation: after capture, ClearState, a backend's dirtying of every stage, ClearState and restore, every value the oracle reads is what it was");
        if (!first.empty()) std::printf("  (%zu values differ; the first is %s)\n", differing, first.c_str());
        const std::vector<ULONG> refsAfter = refCounts(objects);
        std::string leaked;
        for (size_t i = 0; i < objects.size(); ++i) if (refsAfter[i] != refsBefore[i]) { leaked = objects[i].first + " " + std::to_string(refsBefore[i]) + "->" + std::to_string(refsAfter[i]); break; }
        check(leaked.empty(), "isolation: every object's reference count is what it was: the capture and the restore take none they do not give back");
        if (!leaked.empty()) std::printf("  (first count that moved: %s)\n", leaked.c_str());
    }
    // The crumbs of that cycle: eleven groups each way, in order, balanced, with what was bound on the capture's ends.
    {
        const auto trail = ct::trail(crumbLines);
        std::string gap;
        check(ct::balanced(trail, &gap), "isolation: the explicit capture's crumbs balance");
        static const char* groups[] = {"ia", "vs", "hs", "ds", "gs", "ps", "cs", "so", "om", "rs", "predication"};
        int at = -1; bool order = true;
        for (const char* g : groups) {
            const std::string step = std::string("capture-") + g;
            const int b = ct::find(trail, step.c_str(), "begin", size_t(at + 1)), e = ct::find(trail, step.c_str(), "end", size_t(at + 1));
            order = order && b >= 0 && e == b + 1;
            at = e;
        }
        for (const char* g : groups) {
            const std::string step = std::string("restore-") + g;
            const int b = ct::find(trail, step.c_str(), "begin", size_t(at + 1)), e = ct::find(trail, step.c_str(), "end", size_t(at + 1));
            order = order && b >= 0 && e == b + 1;
            at = e;
        }
        check(order, "isolation: the capture writes its eleven groups, then the restore its eleven, each a begin and an end in order");
        const int ps = ct::find(trail, "capture-ps", "end");
        check(ps >= 0 && trail[ps].detail == "shader=1 srv=128 cb=14 sampler=16 uav=0", "isolation: the capture's end line for a stage says what the game had bound there (shader, 128 shader resources, 14 constant buffers, 16 samplers)");
        const int cs = ct::find(trail, "capture-cs", "end");
        char want[96]; std::snprintf(want, sizeof(want), "shader=1 srv=128 cb=14 sampler=16 uav=6");
        check(cs >= 0 && trail[cs].detail == want, "isolation: and the compute stage's names its six UAVs");
        const int om = ct::find(trail, "capture-om", "end"), rs = ct::find(trail, "capture-rs", "end"), so = ct::find(trail, "capture-so", "end"), pr = ct::find(trail, "capture-predication", "end");
        check(om >= 0 && trail[om].detail == "rtv=3 dsv=1 uav=2 blend=1 depth-stencil=1" && rs >= 0 && trail[rs].detail == "state=1 viewports=3 scissors=2" &&
                  so >= 0 && trail[so].detail == "targets=2" && pr >= 0 && trail[pr].detail == "predicate=1 value=1",
              "isolation: and the output merger's, rasterizer's, stream output's and predication's");
        const int ia = ct::find(trail, "capture-ia", "end");
        check(ia >= 0 && trail[ia].detail == "layout=1 vb=4 ib=1 topology=35", "isolation: and the input assembler's");
        // A second cycle, with the crumbs' first-use flags spent: no group is written again, and the state still comes back.
        const size_t written = crumbLines.size();
        edvr::hdrCrumbAdmit(2, "dlss");
        edvr::hdrCrumbReach(2, "dlss", "resolve");
        block.capture(c1, ranges, edvr::hdrCrumbFirstCapture(true));
        context->ClearState();
        block.restore(c1, edvr::hdrCrumbFirstRestore(true));
        { edvr::HdrCrumbFrameEnd end(0); }
        const auto second = ct::trail(crumbLines);
        check(ct::count(second, "capture-ia", "begin") == 1 && ct::count(second, "restore-rs", "begin") == 1 && crumbLines.size() >= written,
              "isolation: the groups are written for the session's first capture and first restore only");
        check(firstDifference(before, snapshot(c1, fx.uavSlots)).empty(), "isolation: the second cycle, which writes no group crumb, restores the same state");
    }

    // ---- the oracle is held to its word: each single change is seen, at its own label ------------------------------------------
    {
        const auto list = mutations();
        size_t seen = 0, notSeen = 0;
        for (const Mutation& m : list) {
            fx.bindGame();
            check(firstDifference(before, snapshot(c1, fx.uavSlots)).empty(), "isolation: re-binding the game's state from scratch reproduces the snapshot (the mutation base)");
            m.apply(fx);
            const std::string first = firstDifference(before, snapshot(c1, fx.uavSlots));
            const bool hit = !first.empty() && first.find(m.label) != std::string::npos;
            if (hit) ++seen;
            else {
                ++notSeen;
                std::printf("  (oracle: a change named '%s' was reported as '%s')\n", m.label.c_str(), first.empty() ? "no difference" : first.c_str());
            }
        }
        check(notSeen == 0 && seen == list.size() && list.size() >= 45,
              "isolation: a single change in each stage and each kind of slot is seen by the oracle, at the label of the slot that changed");
        std::printf("flat context isolation: the oracle saw %zu of %zu single-slot changes\n", seen, list.size());
        // A leaked reference is seen in the counts.
        fx.bindGame();
        fx.srv[0]->AddRef();
        const std::vector<ULONG> leakedCounts = refCounts(objects);
        fx.srv[0]->Release();
        bool seenLeak = false;
        for (size_t i = 0; i < objects.size(); ++i) seenLeak = seenLeak || leakedCounts[i] != refsBefore[i];
        check(seenLeak, "isolation: a leaked reference is seen in the reference counts");
        // And so is a block that does not restore (the mutation of the block itself): capture, clear, never restore.
        FlatContextState lazy;
        fx.bindGame();
        lazy.capture(c1, ranges, false);
        context->ClearState();
        check(!firstDifference(before, snapshot(c1, fx.uavSlots)).empty(), "isolation: a context that is cleared and not restored differs from the game's state (the oracle is not vacuous)");
        lazy.restore(c1, false);
        check(firstDifference(before, snapshot(c1, fx.uavSlots)).empty(), "isolation: the restore puts it back");
        const std::vector<ULONG> refs = refCounts(objects);
        bool same = true;
        for (size_t i = 0; i < objects.size(); ++i) same = same && refs[i] == refsBefore[i];
        check(same, "isolation: and gives every reference back");
    }

    // The block's own ranges held to their word: a capture that stops short of the far slots leaves exactly those slots behind.
    if (fx.uavSlots == 64) {
        FlatContextRanges shortRanges = ranges;
        shortRanges.vertexBuffers = 16;
        shortRanges.uavs = 8;
        fx.bindGame();
        FlatContextState shortBlock;
        shortBlock.capture(c1, shortRanges, false);
        context->ClearState();
        shortBlock.restore(c1, false);
        const Snap after = snapshot(c1, fx.uavSlots);
        check(after.v.at("IA.vb[31].buf") == 0 && after.v.at("CS.uav[40]") == 0 && after.v.at("CS.uav[63]") == 0 &&
                  after.v.at("IA.vb[5].buf") == before.v.at("IA.vb[5].buf") && after.v.at("CS.uav[5]") == before.v.at("CS.uav[5]"),
              "isolation: a capture whose ranges stop short leaves exactly the slots beyond them behind, so the ranges are what holds the far slots");
        fx.bindGame();
    }

    // ---- degenerate states: nothing bound, and a typical one ---------------------------------------------------------------------
    {
        context->ClearState();
        const Snap empty = snapshot(c1, fx.uavSlots);
        const std::vector<ULONG> refsEmpty = refCounts(objects);
        FlatContextState b;
        b.capture(c1, ranges, false);
        context->ClearState();
        b.restore(c1, false);
        check(firstDifference(empty, snapshot(c1, fx.uavSlots)).empty() && !b.holding(), "isolation: an empty pipeline captures and restores as an empty one");
        check(refCounts(objects) == refsEmpty, "isolation: and holds nothing");
        // A typical draw state: VS, PS, layout, one VB, one CB and SRV, a render target and depth, a viewport.
        context->ClearState();
        ID3D11Buffer* vbuf = fx.vb[0].Get(); const UINT stride = 16, offset = 0;
        context->IASetVertexBuffers(0, 1, &vbuf, &stride, &offset);
        context->IASetInputLayout(fx.layout[0].Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(fx.vs[0].Get(), nullptr, 0);
        context->PSSetShader(fx.ps[0].Get(), nullptr, 0);
        ID3D11ShaderResourceView* sv = fx.srv[1].Get(); context->PSSetShaderResources(2, 1, &sv);
        ID3D11Buffer* cbv = fx.cbuf[1].Get(); context->VSSetConstantBuffers(0, 1, &cbv);
        ID3D11RenderTargetView* rt = fx.rtv[0].Get(); context->OMSetRenderTargets(1, &rt, fx.dsv[0].Get());
        const D3D11_VIEWPORT vp{0, 0, 16, 16, 0, 1}; context->RSSetViewports(1, &vp);
        const Snap typical = snapshot(c1, fx.uavSlots);
        const std::vector<ULONG> refsTypical = refCounts(objects);
        FlatContextState t;
        t.capture(c1, ranges, false);
        context->ClearState();
        fx.dirty(context, false);
        context->ClearState();
        t.restore(c1, false);
        check(firstDifference(typical, snapshot(c1, fx.uavSlots)).empty() && refCounts(objects) == refsTypical,
              "isolation: a typical draw's state, and its reference counts, come back through a full dirtying");
        // A block that captures twice lets the first go (the counts do not climb), and one never restored releases on release().
        FlatContextState twice;
        twice.capture(c1, ranges, false);
        twice.capture(c1, ranges, false);
        twice.release();
        check(!twice.holding() && refCounts(objects) == refsTypical, "isolation: capturing twice holds one capture, and release() gives it back");
    }
    fx.bindGame();
}

// ---- 3. the resolver, in both modes ---------------------------------------------------------------------------------------------
inline Fixture* g_fixture = nullptr;

inline void resolverTests(ID3D11Device* device, ID3D11DeviceContext* context, Fixture& fx) {
    namespace ct = hdr_crumb_trail;
    using edvr::FlatMonoResolveFrame;
    using edvr::FlatMonoResolveMode;
    ID3D11DeviceContext1* c1 = fx.c1.Get();
    const UINT w = ResolveFixture::w, h = ResolveFixture::h;
    ResolveFixture env(device, context);
    const Objects objects = fx.objects();
    std::vector<uint32_t> red(w * h, 0xff0000ff);
    std::vector<float> z(w * h, .01f);
    auto color = texture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, red.data(), w * 4);
    auto depth = texture(device, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, z.data(), w * 4);
    auto colorView = view(device, color.Get()), depthView = view(device, depth.Get());
    auto hTexture = texture(device, w, h, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    auto hView = view(device, hTexture.Get());
    std::vector<uint32_t> texels(w * h);
    for (size_t i = 0; i < texels.size(); ++i) texels[i] = 0x2a8a5000u + uint32_t(i) * 0x00010441u;
    context->UpdateSubresource(hTexture.Get(), 0, nullptr, texels.data(), w * 4, 0);

    FlatMonoResolveFrame f{};
    f.color = colorView.Get(); f.depth = depthView.Get();
    f.renderWidth = w; f.renderHeight = h; f.outputWidth = w; f.outputHeight = h;
    f.deltaMs = 16; camera(f.camera); camera(f.previousCamera); env.engine(f);
    f.mode = FlatMonoResolveMode::Dlss; f.frame = 50000; f.reset = true;
    expectedJx = expectedJy = 0;

    g_fixture = &fx;
    backendDirtyHook = [](ID3D11DeviceContext* c) { if (g_fixture) g_fixture->dirty(c, false); };

    const auto modeName = [](edvr::FlatContextIsolation m) { return edvr::flatContextIsolationName(m); };
    for (const edvr::FlatContextIsolation mode : {edvr::FlatContextIsolation::Auto, edvr::FlatContextIsolation::Capture, edvr::FlatContextIsolation::Swap}) {
        const bool expectCapture = mode == edvr::FlatContextIsolation::Capture;
        const char* expectName = expectCapture ? "capture" : "swap";
        char what[256];
        const auto say = [&](const char* text) { std::snprintf(what, sizeof(what), "isolation (%s): %s", modeName(mode), text); return what; };
        edvr::flatMonoResolveSetIsolation(mode);
        edvr::flatMonoResolveReset();
        edvr::flatMonoResolveTestResetIsolationLog();
        isolationLines.clear();
        crumbLines.clear();
        edvr::hdrCrumbReset();
        edvr::hdrCrumbEnable(true);   // the DXMT case; the gate shut (every Windows device) is flat_hdr_route_gpu_tests.h's (g), forced capture included

        // The game's state bound from scratch, its snapshot, and its objects' reference counts as they stand with it bound.
        std::vector<ULONG> refsBase;
        const auto fresh = [&] { fx.bindGame(); refsBase = refCounts(objects); return snapshot(c1, fx.uavSlots); };
        const auto same = [&](const Snap& before, const char* after) {
            size_t n = 0;
            const std::string first = firstDifference(before, snapshot(c1, fx.uavSlots), &n);
            if (!first.empty()) std::printf("  (%s: %zu values differ after %s; the first is %s)\n", modeName(mode), n, after, first.c_str());
            const std::vector<ULONG> refs = refCounts(objects);
            bool counts = true;
            for (size_t i = 0; i < objects.size(); ++i)
                if (refs[i] != refsBase[i]) {
                    if (counts) std::printf("  (%s: after %s the reference count of %s is %lu, it was %lu)\n", modeName(mode), after, objects[i].first.c_str(), refs[i], refsBase[i]);
                    counts = false;
                }
            return first.empty() && counts;
        };

        // (a) the copy route, backend dirties every stage after its own ClearState
        {
            const auto stats0 = edvr::flatMonoResolveStats();
            const Snap before = fresh();
            ComPtr<ID3D11ShaderResourceView> out; const char* reason = nullptr;
            FlatMonoResolveFrame frame = f; frame.frame = ++f.frame;
            const int calls = backendCalls;
            const bool ok = edvr::flatMonoResolve(device, context, frame, out.GetAddressOf(), &reason);
            if (!ok) std::printf("  (%s: copy route reason %s)\n", modeName(mode), reason ? reason : "none");
            check(ok && out && backendCalls == calls + 1, say("the copy route resolves, through a backend that dirties every stage"));
            check(same(before, "the copy route's resolve"), say("the copy route leaves the game's whole pipeline state and every reference count as they were"));
            const auto stats1 = edvr::flatMonoResolveStats();
            check(stats1.isolation && !std::strcmp(stats1.isolation, expectName), say("the renderer reports the isolation it chose"));
            check(expectCapture ? (stats1.isolationCaptures == stats0.isolationCaptures + 1 && stats1.isolationSwaps == stats0.isolationSwaps)
                                : (stats1.isolationSwaps == stats0.isolationSwaps + 1 && stats1.isolationCaptures == stats0.isolationCaptures),
                  say("one call is counted under the isolation that ran it, and none under the other"));
            bool logged = isolationLines.size() == 1;
            if (logged) {
                const std::string& line = isolationLines[0];
                logged = expectCapture ? line == "flat resolver: context isolation by explicit state capture (advanced.flat_context_isolation=capture; DXMT markers: none)"
                         : mode == edvr::FlatContextIsolation::Swap ? line == "flat resolver: context isolation by context state swap (advanced.flat_context_isolation=swap; DXMT markers: none)"
                                                                      : line == "flat resolver: context isolation by context state swap (no DXMT marker)";
            }
            if (!logged) for (const auto& l : isolationLines) std::printf("  (logged: %s)\n", l.c_str());
            check(logged, say("the renderer says which isolation it chose, and why, in one log line"));
            check(crumbLines.empty(), say("a call that is not the HDR route's writes no crumb, whichever isolation it uses"));
        }
        // (b) a refused backend, then the spatial recovery, on the copy route
        {
            const Snap before = fresh();
            ComPtr<ID3D11ShaderResourceView> out; const char* reason = nullptr;
            FlatMonoResolveFrame frame = f; frame.frame = ++f.frame; frame.reset = false;
            backendFail = true;
            const bool refused = edvr::flatMonoResolve(device, context, frame, out.GetAddressOf(), &reason);
            backendFail = false;
            check(!refused && !out, say("a refused backend refuses the frame"));
            check(same(before, "the refused backend"), say("a refused backend, which dirtied every stage, still leaves the game's state and counts as they were"));
            ComPtr<ID3D11ShaderResourceView> recovered; const char* why = nullptr;
            const bool ok = edvr::flatMonoResolveSpatialFallback(device, context, frame, recovered.GetAddressOf(), &why);
            check(ok && recovered && same(before, "the spatial recovery"), say("the spatial recovery after it recovers and leaves the game's state and counts as they were"));
        }
        // (c) the HDR route, with the crumbs live: two frames, the second's first-use flags spent
        {
            FlatMonoResolveFrame hdr = f; hdr.color = hView.Get(); hdr.hdr = true; hdr.reset = true;
            std::vector<Snap> befores;
            bool all = true, resolved = true;
            for (int n = 0; n < 2; ++n) {
                const Snap before = fresh();
                hdr.frame = ++f.frame; hdr.reset = n == 0;
                edvr::hdrCrumbAdmit(hdr.frame, "dlss");
                edvr::hdrCrumbReach(hdr.frame, "dlss", "resolve");
                ComPtr<ID3D11ShaderResourceView> out; const char* reason = nullptr;
                const bool ok = edvr::flatMonoResolve(device, context, hdr, out.GetAddressOf(), &reason);
                if (!ok) std::printf("  (%s: HDR route reason %s)\n", modeName(mode), reason ? reason : "none");
                { edvr::HdrCrumbFrameEnd end(0); }
                resolved = resolved && ok && !out;
                all = all && same(before, "the HDR route's resolve");
            }
            check(resolved, say("the HDR route resolves twice, writing into the game's target"));
            check(all, say("the HDR route leaves the game's whole pipeline state and every reference count as they were, both frames"));
            const auto trail = ct::trail(crumbLines);
            std::string gap;
            check(ct::balanced(trail, &gap), say("the HDR route's crumbs balance"));
            const int cap = ct::find(trail, "capture-state", "begin"), res = ct::find(trail, "restore-state", "begin");
            check(cap >= 0 && res >= 0 && trail[cap].detail == (expectCapture ? "by=capture" : "by=swap") && trail[res].detail == (expectCapture ? "by=capture" : "by=swap"),
                  say("the capture-state and restore-state crumbs say which isolation ran"));
            check(ct::count(trail, "capture-state", "begin") == 2 && ct::count(trail, "restore-state", "begin") == 2 && ct::count(trail, "capture-state", "end") == 2 &&
                      ct::count(trail, "restore-state", "end") == 2,
                  say("each of the two frames is bracketed by its capture and restore"));
            if (expectCapture) {
                // The eleven groups of each, once: inside the first frame's capture-state and restore-state, none in the second's.
                const int capEnd = ct::find(trail, "capture-state", "end"), ia = ct::find(trail, "capture-ia", "begin"), pr = ct::find(trail, "capture-predication", "end");
                const int resEnd = ct::find(trail, "restore-state", "end"), rs = ct::find(trail, "restore-ia", "begin"), rp = ct::find(trail, "restore-predication", "end");
                check(ia > cap && pr < capEnd && rs > res && rp < resEnd && ct::count(trail, "capture-ia", "begin") == 1 && ct::count(trail, "restore-ia", "begin") == 1 &&
                          ct::count(trail, "capture-predication", "begin") == 1 && ct::count(trail, "restore-predication", "begin") == 1,
                      say("the eleven groups are written once, inside the first frame's capture-state and restore-state"));
                const int ps = ct::find(trail, "capture-ps", "end");
                check(ps >= 0 && trail[ps].detail == "shader=1 srv=128 cb=14 sampler=16 uav=0", say("the resolver's capture-ps end line says what the game had bound"));
            } else {
                check(ct::count(trail, "capture-ia", "begin") == 0 && ct::count(trail, "capture-ps", "begin") == 0 && ct::count(trail, "restore-om", "begin") == 0,
                      say("the swap writes none of the explicit capture's groups"));
            }
            check(edvr::flatMonoResolveStats().hdrCaptured >= 2 && edvr::flatMonoResolveStats().hdrRestored >= 2, say("the HDR route's own step counts still count both ends"));
        }
        // (d) the HDR route's spatial recovery: the same, through the pixel-shader draw
        {
            FlatMonoResolveFrame hdr = f; hdr.color = hView.Get(); hdr.hdr = true; hdr.frame = ++f.frame;
            const Snap before = fresh();
            ComPtr<ID3D11ShaderResourceView> out; const char* why = nullptr;
            const bool ok = edvr::flatMonoResolveSpatialFallback(device, context, hdr, out.GetAddressOf(), &why);
            check(ok && !out && same(before, "the HDR spatial recovery"), say("the HDR route's spatial recovery leaves the game's state and counts as they were"));
        }
    }
    backendDirtyHook = nullptr;
    g_fixture = nullptr;
    edvr::flatMonoResolveSetIsolation(edvr::FlatContextIsolation::Auto);
    edvr::flatMonoResolveReset();
    edvr::hdrCrumbReset();
    crumbLines.clear();
    // The default route, again, after all of that: the swap, with the state object made and the crumbs it always had.
    {
        edvr::flatMonoResolveTestResetIsolationLog();
        isolationLines.clear();
        const Snap before = [&] { fx.bindGame(); return snapshot(c1, fx.uavSlots); }();
        ComPtr<ID3D11ShaderResourceView> out; const char* reason = nullptr;
        FlatMonoResolveFrame frame = f; frame.frame = ++f.frame;
        const auto stats0 = edvr::flatMonoResolveStats();
        const bool ok = edvr::flatMonoResolve(device, context, frame, out.GetAddressOf(), &reason);
        const auto stats1 = edvr::flatMonoResolveStats();
        check(ok && stats1.isolationSwaps == stats0.isolationSwaps + 1 && stats1.isolationCaptures == stats0.isolationCaptures && stats1.isolation && !std::strcmp(stats1.isolation, "swap"),
              "isolation: with the key at its default the resolver takes the swap on this device, and counts no explicit capture");
        check(firstDifference(before, snapshot(c1, fx.uavSlots)).empty(), "isolation: and on the default route the game's whole pipeline state comes back, which the oracle reads as it read the capture's");
        check(isolationLines.size() == 1 && isolationLines[0] == "flat resolver: context isolation by context state swap (no DXMT marker)", "isolation: and says so in the log");
    }
    edvr::flatMonoResolveReset();
}

// Is the D3D debug layer on this device? (It is the optional Graphics Tools feature; a machine without it runs the rigs on a plain
// device, and the checks that ask the layer for its verdict say so instead of passing.)
inline bool hasDebugLayer(ID3D11Device* device) {
    ComPtr<ID3D11InfoQueue> queue;
    device->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf()));
    return queue != nullptr;
}
// What a device's debug layer said since it was made or last cleared, at warning level or worse: counted, and the first printed.
inline size_t debugWarnings(ID3D11Device* device, const char* what) {
    ComPtr<ID3D11InfoQueue> queue;
    device->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf()));
    if (!queue) return 0;
    size_t bad = 0;
    for (UINT64 i = 0; i < queue->GetNumStoredMessages(); ++i) {
        SIZE_T n = 0;
        queue->GetMessage(i, nullptr, &n);
        std::vector<unsigned char> bytes(n);
        auto* msg = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
        queue->GetMessage(i, msg, &n);
        if (msg->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) { if (!bad) std::printf("  (%s: D3D says: %s)\n", what, msg->pDescription); ++bad; }
    }
    queue->ClearStoredMessages();
    return bad;
}

inline void contextIsolationGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    {
        const bool layer = hasDebugLayer(device);
        std::printf("flat context isolation: the D3D debug layer is %s on the rig's device (%s)\n", layer ? "on" : "not installed",
                    layer ? "every call below is held to it, and to the rig's final message check" : "the calls are held to the oracle and to the runtime's results only");
    }
    decisionTests(device, context);
    // The block on a second WARP device at feature level 11_1 as well: the rig's own device is 11_0 (a null level list never offers
    // 11_1), where the UAV ranges are 8 slots; at 11_1 they reach 64, and the game's UAVs sit at slots 40 and 63.
    {
        const auto create = edvr::systemD3D11CreateDevice();
        ComPtr<ID3D11Device> d11;
        ComPtr<ID3D11DeviceContext> c11;
        D3D_FEATURE_LEVEL got{};
        const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = create ? create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, want, 2, D3D11_SDK_VERSION, d11.GetAddressOf(), &got, c11.GetAddressOf()) : E_FAIL;
        if (FAILED(hr) && create) hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 2, D3D11_SDK_VERSION, d11.ReleaseAndGetAddressOf(), &got, c11.ReleaseAndGetAddressOf());
        check(SUCCEEDED(hr) && got >= D3D_FEATURE_LEVEL_11_1, "isolation rig: a WARP device at feature level 11_1 is available for the 64-slot UAV ranges");
        if (SUCCEEDED(hr) && got >= D3D_FEATURE_LEVEL_11_1) {
            Fixture fx11(d11.Get(), c11.Get());
            check(fx11.ok && fx11.c1 && fx11.uavSlots == 64, "isolation rig: the 11_1 fixture is made");
            if (fx11.ok && fx11.c1) {
                const bool layer = hasDebugLayer(d11.Get());
                debugWarnings(d11.Get(), "11_1 fixture");
                blockTests(fx11);
                if (layer) check(debugWarnings(d11.Get(), "11_1 block tests") == 0, "isolation: the explicit capture and restore draw no debug-layer error or warning at feature level 11_1");
                else std::printf("flat context isolation: the 11_1 device has no debug layer, so the layer's validation of the capture's calls was not run there\n");
            }
            c11->ClearState();
        }
    }
    Fixture fx(device, context);
    check(fx.ok && fx.c1, "isolation rig: the fixture (shaders for all six stages, buffers, textures, views, states, predicates) is made");
    if (!fx.ok || !fx.c1) return;
    blockTests(fx);
    resolverTests(device, context, fx);
    context->ClearState();
    edvr::hdrCrumbEnable(false);   // the rig's default for whoever runs next: shut, as on a Windows device
}
}  // namespace isogpu

inline void contextIsolationGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) { isogpu::contextIsolationGpuTests(device, context); }
