#pragma once
// engine_velocity_test: engine_velocity.cpp's DRAW half, linked into the rig
// (build.bat compiles it with EDVR_ENGINE_VELOCITY_RIG and the binding
// shadow external), driven on WARP the way vscreen and device_hook drive it:
// the fake binding shadow below records what the game "binds" exactly as the
// hooks do, the fake depth probe names the two eye depths, and the Map/Unmap
// tees are called in vscreen's order (Map after the real Map, the write tee
// before the real Unmap). The emit hook's state and the log are stubbed so
// the stand-down line and every counter can be read back.
//
// The cases are the 2026-09-23 fix round's (docs/kinematic-motion-injection-
// 2026-09-19.md, "Fix round"; reviews\engine-motion-review-2026-09-23.md):
//   F1  cb1 re-mapped inside an eye pass with rows 270..275 unchanged: kept;
//       rows changed then a draw of the same eye: dropped; the other eye's
//       rows through the same buffer between interleaved passes: kept.
//   R3  t33 or b1 rebound without touching shaders or targets: dropped; the
//       same view again, or another view over the same elements: kept; a PS
//       change alone: kept; a view with another FirstElement: dropped.
//   R3b the pool appended (NO_OVERWRITE) mid-pass: refreshed and kept;
//       replaced (DISCARD) then drawn: dropped; replaced after the eye's last
//       draw: kept.
//   R4  the derived blend state bound for substituted draws, re-derived after
//       a mid-pass blend change, the game's put back by the next unkeyed draw
//       and at the frame boundary; MRT6 exact under an additive game state.
//   F3  a depth that is not 32-bit float: MRT6 refused (counted), nothing
//       given; a new depth pair: the slot target re-created (logged), given.
//   S1  on foot (2026-09-23): a pool draw into the source depth screen_motion
//       names gets MRT6 into a slot target of the SOURCE's size (created,
//       logged; re-created when the source is re-made at another size);
//       the screen's views given from the second frame; an unnamed depth of
//       the same shape gets nothing; kSourceIdleFrames without the source
//       release it (logged).
//   S2  the source's camera rule (flight 5, 2026-09-23 140351): the walk's
//       interleaving -- the naming's world rows, two pool draws of another
//       camera (a 4 cm bob), the world's draws -- drops an EYE-frame (the
//       eyes' rule, unchanged) but on the source declines only the other
//       camera's draws: every frame given from the second, the view's scene
//       constants the world's rows exactly, MRT6 the world's slot only; a
//       draw before the naming declined as such; standing, nothing declined.
//   R6  the emit hook not installed: STOOD DOWN at configure and in the 30 s
//       block, no substitution, every view refused; installed: stands up.
//   R5  CsStageSave (cs_stage_save.h): sentinels in every compute slot the
//       temporal pass touches come back by identity after the engine path
//       and after the fallback path.

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/common/log.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/timing.h"
#include "../../src/d3d11/cs_stage_save.h"
#include "../../src/d3d11/depth_probe.h"
#include "../../src/d3d11/engine_velocity.h"
#include "../../src/d3d11/gpu_census.h"
#include "../../src/d3d11/kinematic_eval_hook.h"
#include "../../src/d3d11/vscreen.h"

namespace lifecycle_fake {
struct Slot { void* ptr = nullptr; uint32_t gen = 1; uint64_t hash = 0; };
Slot g_slots[static_cast<unsigned>(edvr::BindSlot::Count)];
ID3D11DepthStencilView* g_eyeDsv[2] = {};
bool g_hookLive = true;
unsigned g_emitAttaches = 0, g_emitDetaches = 0;
std::vector<std::string> g_log;
uint64_t g_clock = 1000;
uint64_t fakeClock() { return g_clock; }
}  // namespace lifecycle_fake

// --- The stubs engine_velocity.cpp links against -------------------------------
namespace edvr {
void* bindingGet(BindSlot s) { return lifecycle_fake::g_slots[static_cast<unsigned>(s)].ptr; }
uint32_t bindingGeneration(BindSlot s) { return lifecycle_fake::g_slots[static_cast<unsigned>(s)].gen; }
uint64_t bindingShaderHash(BindSlot s) { return lifecycle_fake::g_slots[static_cast<unsigned>(s)].hash; }
void bindingSet(BindSlot s, void* p) {
    auto& x = lifecycle_fake::g_slots[static_cast<unsigned>(s)];
    x.ptr = p;
    ++x.gen;
}
bool depthProbeCurrentSceneEyeOf(ID3D11DepthStencilView* dsv, int* outEye, int* outTargetIndex) {
    for (int i = 0; i < 2; ++i)
        if (dsv && dsv == lifecycle_fake::g_eyeDsv[i]) { *outEye = i; *outTargetIndex = i; return true; }
    return false;
}
void kinematicEvalSetEmitObserver(EngineEmitObserverFn) noexcept {}
bool kinematicEvalEmitHookLive(const char** why) noexcept {
    if (why) *why = lifecycle_fake::g_hookLive ? nullptr : "rig: kinematic-build-144312e00 refused";
    return lifecycle_fake::g_hookLive;
}
// The emit's own want on the hook set: counted, so the rig can see the
// engine path attach and detach it.
const char* kinematicEvalEmitAttach() noexcept { ++lifecycle_fake::g_emitAttaches; return "installed"; }
void kinematicEvalEmitDetach() noexcept { ++lifecycle_fake::g_emitDetaches; }
void vScreenSetRenderTargetsRaw(ID3D11DeviceContext* ctx, uint32_t n, ID3D11RenderTargetView* const* rtvs,
                                ID3D11DepthStencilView* dsv) { ctx->OMSetRenderTargets(n, rtvs, dsv); }
void vScreenVSSetShaderRaw(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs, ID3D11ClassInstance* const* ci,
                           uint32_t n) { ctx->VSSetShader(vs, ci, n); }
void vScreenPSSetShaderRaw(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci,
                           uint32_t n) { ctx->PSSetShader(ps, ci, n); }
void vScreenOMSetBlendStateRaw(ID3D11DeviceContext* ctx, ID3D11BlendState* state, const float factor[4], uint32_t mask) {
    ctx->OMSetBlendState(state, factor, mask);
}
Log& Log::get() { static Log instance; return instance; }
Log::~Log() {}
void Log::note(const char* fmt, ...) {
    char text[8192];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    lifecycle_fake::g_log.push_back(text);
}
int64_t qpcNow() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return t.QuadPart; }
int64_t qpcFrequency() { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart; }
// The GPU census (issue #38) is cross-cutting; this rig is about the draw
// half's own state machine, not the census's rotation or its calibration
// (tools/gpu_census_test covers those), so it is stubbed out.
bool gpuCensusBegin(ID3D11DeviceContext*, GpuCensusSection) noexcept { return false; }
void gpuCensusEnd(ID3D11DeviceContext*, GpuCensusSection) noexcept {}
}  // namespace edvr

namespace lifecycle_tests {
using Microsoft::WRL::ComPtr;
using edvr::BindSlot;
using lifecycle_fake::g_log;
bool g_verbose = false;   // --verbose: print the captured log lines

struct Harness {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    void (*check)(bool, const char*) = nullptr;
};

constexpr uint64_t kVsHash = 0xEB5234DB6ADB491Dull;   // the flight's substituted family
constexpr uint64_t kPsHash = 0xCB9F297EFF264251ull;
constexpr uint64_t kPsHash2 = 0x3434972DB5336AA4ull;
constexpr uint64_t kUnkeyedPs = 0x1111222233334444ull;
constexpr UINT kW = 48, kH = 48;
constexpr UINT kSceneFloats = 5376 / 4;               // the game's cb1

// The last log line starting with prefix (empty if none since `from`).
inline std::string lastLine(const char* prefix, size_t from = 0) {
    for (size_t i = g_log.size(); i > from; --i)
        if (g_log[i - 1].rfind(prefix, 0) == 0) return g_log[i - 1];
    return {};
}
inline bool logged(const char* fragment, size_t from) {
    for (size_t i = from; i < g_log.size(); ++i) if (g_log[i].find(fragment) != std::string::npos) return true;
    return false;
}
// The unsigned number right after label in line (~0 if absent).
inline unsigned long long number(const std::string& line, const char* label) {
    const size_t at = line.find(label);
    if (at == std::string::npos) return ~0ull;
    return std::strtoull(line.c_str() + at + std::strlen(label), nullptr, 10);
}

struct Game {
    const Harness& h;
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps, ps2, unkeyed;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vertices, instanceBuffer;
    ComPtr<ID3D11Texture2D> colour[2][4], depth[2];
    ComPtr<ID3D11RenderTargetView> rtv[2][4];
    ComPtr<ID3D11DepthStencilView> dsv[2];
    ComPtr<ID3D11DepthStencilState> depthState;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11Buffer> poolA, poolB, sceneA, sceneB;
    ComPtr<ID3D11ShaderResourceView> viewA, viewA2, viewB, viewOffset;
    std::vector<shader_tests::Record> pool;
    std::vector<float> rows[2];   // cb1 contents per eye (rows 270..275 differ)
    uint32_t frame = 100;
    // The on-foot source (S1): a depth and colour set of its own size, NOT an
    // eye depth -- the fake depth probe never names it.
    ComPtr<ID3D11Texture2D> sourceColour[4], sourceDepth;
    ComPtr<ID3D11RenderTargetView> sourceRtv[4];
    ComPtr<ID3D11DepthStencilView> sourceDsv;
    UINT sourceW = 0, sourceH = 0;

    explicit Game(const Harness& harness) : h(harness), dev(harness.device), ctx(harness.context) {}

    ComPtr<ID3DBlob> compile(const std::string& source, const char* profile) { return shader_tests::compile({dev, ctx, h.check}, source, profile); }

    void setup() {
        const auto vsBlob = compile(std::string(shader_tests::kVsCommon) + shader_tests::kVsA, "vs_5_0");
        const auto psBlob = compile(shader_tests::kPsA, "ps_5_0");
        const char* plain = R"HLSL(
struct PsIn { nointerpolation uint3 id : __USER_MATERIALMODULATION_DATAID; float3 n : __USER_VERTEX_M_LIGHTINGNORMAL;
              float3 t : __USER_VERTEX_M_LIGHTINGTANGENT; float2 uv : __USER_VERTEX_M_TEXCOORD; };
float4 main(PsIn i) : SV_Target0 { return float4(i.uv, 1, 1); }
)HLSL";
        const auto unkeyedBlob = compile(plain, "ps_5_0");
        h.check(SUCCEEDED(dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs)), "life: VS");
        h.check(SUCCEEDED(dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps)), "life: PS");
        h.check(SUCCEEDED(dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps2)), "life: PS 2");
        h.check(SUCCEEDED(dev->CreatePixelShader(unkeyedBlob->GetBufferPointer(), unkeyedBlob->GetBufferSize(), nullptr, &unkeyed)), "life: unkeyed PS");
        // device_hook's creation tees: the keyed bytecode is remembered.
        edvr::engineVelocityRememberVs(vs.Get(), kVsHash, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), false);
        edvr::engineVelocityRememberPs(ps.Get(), kPsHash, psBlob->GetBufferPointer(), psBlob->GetBufferSize(), false);
        edvr::engineVelocityRememberPs(ps2.Get(), kPsHash2, psBlob->GetBufferPointer(), psBlob->GetBufferSize(), false);
        const D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
            {"INSTANCEANDMODELDATAINDEX", 0, DXGI_FORMAT_R32G32_UINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"PACKEDVERTEXDATAA", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        h.check(SUCCEEDED(dev->CreateInputLayout(layoutDesc, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout)), "life: layout");
        const float quad[] = {-0.4f, -0.4f, 0, 0.4f, -0.4f, 0, -0.4f, 0.4f, 0, 0.4f, 0.4f, 0};
        D3D11_BUFFER_DESC vd{};
        vd.ByteWidth = sizeof(quad); vd.Usage = D3D11_USAGE_DEFAULT; vd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vinit{quad, 0, 0};
        h.check(SUCCEEDED(dev->CreateBuffer(&vd, &vinit, &vertices)), "life: vertices");
        const uint32_t instances[] = {5, 0, 9, 0};
        vd.ByteWidth = sizeof(instances);
        D3D11_SUBRESOURCE_DATA iinit{instances, 0, 0};
        h.check(SUCCEEDED(dev->CreateBuffer(&vd, &iinit, &instanceBuffer)), "life: instances");
        for (int eye = 0; eye < 2; ++eye) makeEye(eye, kW, kH, DXGI_FORMAT_D32_FLOAT);
        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dsd.DepthFunc = D3D11_COMPARISON_GREATER;
        h.check(SUCCEEDED(dev->CreateDepthStencilState(&dsd, &depthState)), "life: depth state");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        h.check(SUCCEEDED(dev->CreateRasterizerState(&rd, &raster)), "life: rasterizer");
        pool.assign(16, shader_tests::Record{});
        const float identity[4] = {0, 0, 0, 1};
        pool[5] = shader_tests::makeRecord(-0.3f, 0.0f, -2.0f, 1.0f, identity);
        pool[9] = shader_tests::makeRecord(0.4f, 0.1f, -3.0f, 1.0f, identity);
        D3D11_BUFFER_DESC pd{};
        pd.ByteWidth = UINT(pool.size() * sizeof(shader_tests::Record));
        pd.Usage = D3D11_USAGE_DEFAULT; pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        pd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; pd.StructureByteStride = sizeof(shader_tests::Record);
        D3D11_SUBRESOURCE_DATA pinit{pool.data(), 0, 0};
        h.check(SUCCEEDED(dev->CreateBuffer(&pd, &pinit, &poolA)) && SUCCEEDED(dev->CreateBuffer(&pd, &pinit, &poolB)), "life: pools");
        h.check(SUCCEEDED(dev->CreateShaderResourceView(poolA.Get(), nullptr, &viewA)) &&
                SUCCEEDED(dev->CreateShaderResourceView(poolA.Get(), nullptr, &viewA2)) &&
                SUCCEEDED(dev->CreateShaderResourceView(poolB.Get(), nullptr, &viewB)), "life: pool views");
        D3D11_SHADER_RESOURCE_VIEW_DESC od{};
        od.Format = DXGI_FORMAT_UNKNOWN; od.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        od.Buffer.FirstElement = 1; od.Buffer.NumElements = 15;
        h.check(SUCCEEDED(dev->CreateShaderResourceView(poolA.Get(), &od, &viewOffset)), "life: offset view");
        for (int eye = 0; eye < 2; ++eye) {
            rows[eye].assign(kSceneFloats, 0.0f);
            auto& r = rows[eye];
            r[270 * 4] = 1.0f; r[271 * 4 + 1] = 1.0f; r[272 * 4 + 3] = -1.0f; r[273 * 4 + 2] = 0.025f;
            r[273 * 4] = eye ? -0.03f : 0.03f;   // the eyes' own offsets: rows 270..275 differ between eyes
        }
        D3D11_BUFFER_DESC cd{};
        cd.ByteWidth = kSceneFloats * 4; cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA cinit{rows[0].data(), 0, 0};
        h.check(SUCCEEDED(dev->CreateBuffer(&cd, &cinit, &sceneA)) && SUCCEEDED(dev->CreateBuffer(&cd, &cinit, &sceneB)), "life: scene CBs");
    }
    void makeEye(int eye, UINT w, UINT hgt, DXGI_FORMAT depthFormat) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = hgt; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        for (int i = 0; i < 4; ++i) {
            colour[eye][i].Reset(); rtv[eye][i].Reset();
            h.check(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &colour[eye][i])), "life: colour");
            h.check(SUCCEEDED(dev->CreateRenderTargetView(colour[eye][i].Get(), nullptr, &rtv[eye][i])), "life: RTV");
        }
        td.Format = depthFormat; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        depth[eye].Reset(); dsv[eye].Reset();
        h.check(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &depth[eye])), "life: depth");
        h.check(SUCCEEDED(dev->CreateDepthStencilView(depth[eye].Get(), nullptr, &dsv[eye])), "life: DSV");
        lifecycle_fake::g_eyeDsv[eye] = dsv[eye].Get();
    }

    // The on-foot source: D32_FLOAT_S8X24 like the flight's #28, any size.
    void makeSource(UINT w, UINT hgt) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = hgt; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        for (int i = 0; i < 4; ++i) {
            sourceColour[i].Reset(); sourceRtv[i].Reset();
            h.check(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &sourceColour[i])), "life: source colour");
            h.check(SUCCEEDED(dev->CreateRenderTargetView(sourceColour[i].Get(), nullptr, &sourceRtv[i])), "life: source RTV");
        }
        td.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        sourceDepth.Reset(); sourceDsv.Reset();
        h.check(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &sourceDepth)), "life: source depth");
        h.check(SUCCEEDED(dev->CreateDepthStencilView(sourceDepth.Get(), nullptr, &sourceDsv)), "life: source DSV");
        sourceW = w; sourceH = hgt;
    }
    // The source pass as the game draws it on foot: rtv0 is no eye target.
    void sourcePass(int count = 1, UINT instance = 5) {
        const float black[4] = {};
        for (auto& r : sourceRtv) ctx->ClearRenderTargetView(r.Get(), black);
        ctx->ClearDepthStencilView(sourceDsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
        ID3D11RenderTargetView* r[4] = {sourceRtv[0].Get(), sourceRtv[1].Get(), sourceRtv[2].Get(), sourceRtv[3].Get()};
        ctx->OMSetRenderTargets(4, r, sourceDsv.Get());
        shadow(BindSlot::Rtv0, r[0]);
        shadow(BindSlot::Dsv0, sourceDsv.Get());
        ctx->OMSetDepthStencilState(depthState.Get(), 0);
        D3D11_VIEWPORT vp{0, 0, float(sourceW), float(sourceH), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(raster.Get());
        ctx->IASetInputLayout(layout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D11Buffer* vbs[2] = {vertices.Get(), instanceBuffer.Get()};
        UINT strides[2] = {12, 8}, offsets[2] = {0, 0};
        ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
        setVs();
        for (int i = 0; i < count; ++i) sourceDraw(instance);
    }
    // One more pool draw of the source pass, its bindings as they stand.
    void sourceDraw(UINT instance = 5) {
        edvr::engineVelocityBeforeDraw(ctx, false);
        ctx->DrawInstanced(4, 1, 0, instance == 9 ? 1 : 0);
    }
    // An on-foot frame as the game draws it: the naming draw's cb1 write,
    // screen_motion naming the source with the camera that draw reads, then
    // the pool draws.
    void sourceFrame(bool named = true) {
        beginFrame();
        writeScene(sceneA.Get(), rows[0]);
        if (named) edvr::engineVelocityNoteSource(sourceDepth.Get(), sceneA.Get());
        sourcePass();
    }
    bool sourceViews(edvr::EngineVelocityViews* out = nullptr) {
        edvr::EngineVelocityViews v{};
        const bool given = edvr::engineVelocitySourceViews(sourceDepth.Get(), &v);
        if (out) *out = v;
        else {
            if (v.slots) v.slots->Release();
            if (v.pool) v.pool->Release();
            if (v.sceneNow) v.sceneNow->Release();
            if (v.scenePrev) v.scenePrev->Release();
        }
        return given;
    }

    // --- The game's calls, each with the shadow update its hook makes ---------
    void shadow(BindSlot s, void* p, uint64_t hash = 0) {
        auto& x = lifecycle_fake::g_slots[static_cast<unsigned>(s)];
        x.ptr = p; x.hash = hash; ++x.gen;
    }
    void setVs() { ctx->VSSetShader(vs.Get(), nullptr, 0); shadow(BindSlot::Vs, vs.Get(), kVsHash); }
    void setPs(ID3D11PixelShader* p, uint64_t hash) { ctx->PSSetShader(p, nullptr, 0); shadow(BindSlot::Ps, p, hash); }
    void setPool(ID3D11ShaderResourceView* v) { ctx->VSSetShaderResources(33, 1, &v); shadow(BindSlot::VsSrv33, v); }
    void setScene(ID3D11Buffer* b) { ctx->VSSetConstantBuffers(1, 1, &b); shadow(BindSlot::VsCb1, b); }
    void setBlend(ID3D11BlendState* b) { const float f[4] = {}; ctx->OMSetBlendState(b, f, ~0u); shadow(BindSlot::Blend, b); }
    void setTargets(int eye) {
        ID3D11RenderTargetView* r[4] = {rtv[eye][0].Get(), rtv[eye][1].Get(), rtv[eye][2].Get(), rtv[eye][3].Get()};
        ctx->OMSetRenderTargets(4, r, dsv[eye].Get());
        shadow(BindSlot::Rtv0, r[0]);
        shadow(BindSlot::Dsv0, dsv[eye].Get());
    }
    // A cb1 write through Map/Unmap as vscreen tees it: the Map tee after the
    // real Map, the game's write, the write tee before the real Unmap.
    void writeScene(ID3D11Buffer* b, const std::vector<float>& content) {
        std::vector<float> mapped = content;   // stands in for the mapped memory
        edvr::engineVelocityResourceMapped(b, mapped.data(), D3D11_MAP_WRITE_DISCARD);
        edvr::engineVelocityResourceWritten(b);
        ctx->UpdateSubresource(b, 0, nullptr, content.data(), 0, 0);
    }
    void writePool(ID3D11Buffer* b, D3D11_MAP type) {
        std::vector<shader_tests::Record> mapped = pool;
        edvr::engineVelocityResourceMapped(b, mapped.data(), type);
        edvr::engineVelocityResourceWritten(b);
        ctx->UpdateSubresource(b, 0, nullptr, pool.data(), 0, 0);
    }
    // One eye pass: the game binds its state and draws `count` pool draws.
    void pass(int eye, int count = 1, UINT instance = 5) {
        setTargets(eye);
        ctx->OMSetDepthStencilState(depthState.Get(), 0);
        D3D11_VIEWPORT vp{0, 0, float(kW), float(kH), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(raster.Get());
        ctx->IASetInputLayout(layout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D11Buffer* vbs[2] = {vertices.Get(), instanceBuffer.Get()};
        UINT strides[2] = {12, 8}, offsets[2] = {0, 0};
        ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
        setVs();
        for (int i = 0; i < count; ++i) draw(instance);
    }
    void draw(UINT instance = 5) {
        edvr::engineVelocityBeforeDraw(ctx, true);
        ctx->DrawInstanced(4, 1, 0, instance == 9 ? 1 : 0);
    }
    void clearEye(int eye) {
        const float black[4] = {};
        for (auto& r : rtv[eye]) ctx->ClearRenderTargetView(r.Get(), black);
        ctx->ClearDepthStencilView(dsv[eye].Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
    }
    // The start of a frame: the present clock, the eyes' clears, the game's
    // default pool/scene/shader bindings.
    void beginFrame() {
        edvr::engineVelocityNotePresentFrame(++frame);
        clearEye(0);
        clearEye(1);
        setPool(viewA.Get());
        setScene(sceneA.Get());
        setPs(ps.Get(), kPsHash);
        setBlend(nullptr);
    }
    void endFrame(bool summary = false) {
        if (summary) lifecycle_fake::g_clock += 31000;
        edvr::engineVelocityFrameBoundary(ctx);
        for (auto& s : lifecycle_fake::g_slots) ++s.gen;   // bindingFrameBoundary
    }
    // The ordinary frame: eye 0's rows, its pass; eye 1's rows, its pass.
    void ordinaryFrame(bool summary = false) {
        beginFrame();
        writeScene(sceneA.Get(), rows[0]);
        pass(0);
        writeScene(sceneA.Get(), rows[1]);
        pass(1);
        endFrame(summary);
    }
    bool views(int eye, edvr::EngineVelocityViews* out = nullptr) {
        edvr::EngineVelocityViews v{};
        const bool given = edvr::engineVelocityViews(ctx, eye, depth[eye].Get(), &v);
        if (out) *out = v;
        else {
            if (v.slots) v.slots->Release();
            if (v.pool) v.pool->Release();
            if (v.sceneNow) v.sceneNow->Release();
            if (v.scenePrev) v.scenePrev->Release();
        }
        return given;
    }
    // Views asked for after the frame's passes, before its boundary.
    template <class Body> void frameWithViews(Body body, bool* eye0, bool* eye1, bool summary = false) {
        beginFrame();
        body();
        if (eye0) *eye0 = views(0);
        if (eye1) *eye1 = views(1);
        endFrame(summary);
    }
    std::string summaryLine(const char* prefix) { return lastLine(prefix); }
};

constexpr UINT kSrvs = edvr::CsStageSave::kSrvs, kUavs = edvr::CsStageSave::kUavs, kCbs = edvr::CsStageSave::kCbs;

inline void csStageSaveChecks(const Harness& h) {
    auto* dev = h.device;
    auto* ctx = h.context;
    h.check(kSrvs > edvr::kEngineVelocityPoolSrv && kSrvs > edvr::kEngineVelocitySlotsSrv &&
            kCbs > edvr::kEngineVelocityScenePrevCb, "cs save: covers engine-record velocity's compute slots");
    ComPtr<ID3D11Buffer> buffers[kSrvs];
    ComPtr<ID3D11ShaderResourceView> sentinels[kSrvs], ours[2];
    ComPtr<ID3D11UnorderedAccessView> uavs[kUavs];
    ComPtr<ID3D11Buffer> cbs[kCbs], ourCbs[2];
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 16;
    for (UINT i = 0; i < kSrvs; ++i) {
        h.check(SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &buffers[i])), "cs save: sentinel buffer");
        h.check(SUCCEEDED(dev->CreateShaderResourceView(buffers[i].Get(), nullptr, &sentinels[i])), "cs save: sentinel SRV");
    }
    ComPtr<ID3D11Buffer> ourBuffers[2];
    for (int i = 0; i < 2; ++i) {
        h.check(SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &ourBuffers[i])), "cs save: pass buffer");
        h.check(SUCCEEDED(dev->CreateShaderResourceView(ourBuffers[i].Get(), nullptr, &ours[i])), "cs save: pass SRV");
    }
    for (UINT i = 0; i < kUavs; ++i) {
        ComPtr<ID3D11Buffer> ub;
        h.check(SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &ub)), "cs save: sentinel UAV buffer");
        h.check(SUCCEEDED(dev->CreateUnorderedAccessView(ub.Get(), nullptr, &uavs[i])), "cs save: sentinel UAV");
    }
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 256; cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    for (UINT i = 0; i < kCbs; ++i) h.check(SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &cbs[i])), "cs save: sentinel CB");
    for (auto& c : ourCbs) h.check(SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &c)), "cs save: pass CB");
    auto bindSentinels = [&] {
        ID3D11ShaderResourceView* srv[kSrvs] = {};
        ID3D11UnorderedAccessView* uav[kUavs] = {};
        ID3D11Buffer* cb[kCbs] = {};
        for (UINT i = 0; i < kSrvs; ++i) srv[i] = sentinels[i].Get();
        for (UINT i = 0; i < kUavs; ++i) uav[i] = uavs[i].Get();
        for (UINT i = 0; i < kCbs; ++i) cb[i] = cbs[i].Get();
        ctx->CSSetShaderResources(0, kSrvs, srv);
        ctx->CSSetUnorderedAccessViews(0, kUavs, uav, nullptr);
        ctx->CSSetConstantBuffers(0, kCbs, cb);
    };
    auto sentinelsBack = [&] {
        ID3D11ShaderResourceView* srv[kSrvs] = {};
        ID3D11UnorderedAccessView* uav[kUavs] = {};
        ID3D11Buffer* cb[kCbs] = {};
        ctx->CSGetShaderResources(0, kSrvs, srv);
        ctx->CSGetUnorderedAccessViews(0, kUavs, uav);
        ctx->CSGetConstantBuffers(0, kCbs, cb);
        bool same = true;
        for (UINT i = 0; i < kSrvs; ++i) { same = same && srv[i] == sentinels[i].Get(); if (srv[i]) srv[i]->Release(); }
        for (UINT i = 0; i < kUavs; ++i) { same = same && uav[i] == uavs[i].Get(); if (uav[i]) uav[i]->Release(); }
        for (UINT i = 0; i < kCbs; ++i) { same = same && cb[i] == cbs[i].Get(); if (cb[i]) cb[i]->Release(); }
        return same;
    };
    // The engine path: t21/t22 and b1/b2 bound for the dispatch, every slot
    // nulled after it, as temporal_pass.cpp does.
    bindSentinels();
    {
        edvr::CsStageSave saved;
        saved.save(ctx);
        ID3D11ShaderResourceView* nulls[kSrvs] = {};
        ctx->CSSetShaderResources(0, kSrvs, nulls);
        ID3D11ShaderResourceView* engine[2] = {ours[0].Get(), ours[1].Get()};
        ctx->CSSetShaderResources(edvr::kEngineVelocitySlotsSrv, 2, engine);
        ID3D11Buffer* engineCbs[2] = {ourCbs[0].Get(), ourCbs[1].Get()};
        ctx->CSSetConstantBuffers(edvr::kEngineVelocitySceneNowCb, 2, engineCbs);
        ctx->CSSetShaderResources(0, kSrvs, nulls);
        ID3D11Buffer* nullCbs[kCbs] = {};
        ctx->CSSetConstantBuffers(0, kCbs, nullCbs);
        saved.restore(ctx);
    }
    h.check(sentinelsBack(), "cs save: every sentinel (t0..t22, u0..u6, b0..b2) back by identity after the engine path");
    // The fallback path: 21 slots, no engine inputs.
    bindSentinels();
    {
        edvr::CsStageSave saved;
        saved.save(ctx);
        ID3D11ShaderResourceView* nulls[21] = {};
        ctx->CSSetShaderResources(0, 21, nulls);
        saved.restore(ctx);
    }
    h.check(sentinelsBack(), "cs save: every sentinel back by identity after the fallback path");
    // A save that is dropped without restore (an early exit) still releases.
    bindSentinels();
    { edvr::CsStageSave saved; saved.save(ctx); }
    h.check(sentinelsBack(), "cs save: an unrestored save changes nothing and leaks nothing");
    ctx->ClearState();
    std::printf("  cs save: %u SRVs, %u UAVs, %u CBs restored by identity on the engine and fallback paths\n", kSrvs, kUavs, kCbs);
}

inline std::vector<float> readTexture(const Harness& h, ID3D11Resource* resource, UINT channels, UINT* width) {
    ComPtr<ID3D11Texture2D> tex;
    h.check(SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&tex))), "life: a texture to read");
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    *width = d.Width;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    h.check(SUCCEEDED(h.device->CreateTexture2D(&d, nullptr, &staging)), "life: staging");
    h.context->CopyResource(staging.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    h.check(SUCCEEDED(h.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "life: map staging");
    std::vector<float> out(size_t(d.Width) * d.Height * channels);
    for (UINT y = 0; y < d.Height; ++y)
        std::memcpy(&out[size_t(y) * d.Width * channels], static_cast<const BYTE*>(m.pData) + y * m.RowPitch, d.Width * channels * 4);
    h.context->Unmap(staging.Get(), 0);
    return out;
}

// A buffer's contents as floats (the views' scene constants).
inline std::vector<float> readBuffer(const Harness& h, ID3D11Buffer* buffer) {
    D3D11_BUFFER_DESC d{};
    buffer->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    d.StructureByteStride = 0;
    ComPtr<ID3D11Buffer> staging;
    h.check(SUCCEEDED(h.device->CreateBuffer(&d, nullptr, &staging)), "life: buffer staging");
    h.context->CopyResource(staging.Get(), buffer);
    D3D11_MAPPED_SUBRESOURCE m{};
    h.check(SUCCEEDED(h.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "life: map buffer staging");
    std::vector<float> out(d.ByteWidth / 4);
    std::memcpy(out.data(), m.pData, out.size() * 4);
    h.context->Unmap(staging.Get(), 0);
    return out;
}

// MRT6 of views against the eye's depth: pixels that decode to `slot`
// exactly, and pixels that decode to anything but none/stale.
inline void ownership(const Harness& h, Game& g, int eye, const edvr::EngineVelocityViews& v, uint32_t slot,
                      unsigned* exact, unsigned* other) {
    ComPtr<ID3D11Resource> slotsRes;
    v.slots->GetResource(&slotsRes);
    UINT w = 0, wd = 0;
    const auto slots = readTexture(h, slotsRes.Get(), 2, &w);
    const auto depth = readTexture(h, g.depth[eye].Get(), 1, &wd);
    *exact = *other = 0;
    for (size_t i = 0; i < depth.size(); ++i) {
        uint32_t s = 0;
        const int kind = shader_tests::decodeSlot(slots[i * 2], slots[i * 2 + 1], depth[i], &s);
        if (kind == 1 && s == slot) ++*exact;
        else if (kind == 1 || kind == 5) ++*other;
    }
}

inline void release(edvr::EngineVelocityViews& v) {
    if (v.slots) v.slots->Release();
    if (v.pool) v.pool->Release();
    if (v.sceneNow) v.sceneNow->Release();
    if (v.scenePrev) v.scenePrev->Release();
    v = {};
}

inline void run(const Harness& h) {
    edvr::g_clockForTest = &lifecycle_fake::fakeClock;
    size_t mark = g_log.size();
    Game g(h);
    g.setup();
    edvr::engineVelocityConfigure(true);
    h.check(logged("engine-record velocity live", mark), "life: configure logs live with the hook installed");
    // P1 (the 2026-09-23 performance review, item 1): the emit holds the
    // shared hooks itself (the legacy tracker retired 2026-09-23).
    h.check(lifecycle_fake::g_emitAttaches == 1, "P1: configure attaches the emit's own want on the hook set");
    const char* joined = "engine motion: movers joined";
    auto body = [&] {
        g.writeScene(g.sceneA.Get(), g.rows[0]); g.pass(0);
        g.writeScene(g.sceneA.Get(), g.rows[1]); g.pass(1);
    };
    bool e0 = false, e1 = false;

    // Two ordinary frames: the second has last frame's scene constants. The
    // substituted shader is bound after a keyed draw, a draw elsewhere puts
    // the game's back, and MRT6 holds the odd code at the depth drawn.
    g.ordinaryFrame();
    g.beginFrame();
    g.writeScene(g.sceneA.Get(), g.rows[0]);
    g.pass(0);
    {
        ComPtr<ID3D11PixelShader> bound;
        h.context->PSGetShader(&bound, nullptr, nullptr);
        h.check(bound && bound.Get() != g.ps.Get(), "life: the keyed draw ran the substituted pixel shader");
        edvr::engineVelocityBeforeDraw(h.context, false);   // a draw into something that is not an eye
        bound.Reset();
        h.context->PSGetShader(&bound, nullptr, nullptr);
        h.check(bound.Get() == g.ps.Get(), "life: the next draw elsewhere puts the game's pixel shader back");
    }
    g.writeScene(g.sceneA.Get(), g.rows[1]);
    g.pass(1);
    {
        edvr::EngineVelocityViews v{};
        h.check(g.views(0, &v), "life: eye 0 given after two ordinary frames");
        unsigned exact = 0, other = 0;
        ownership(h, g, 0, v, 5, &exact, &other);
        h.check(exact > 0 && other == 0, "life: MRT6 names slot 5, exactly, at the depth drawn");
        release(v);
        h.check(g.views(1), "life: eye 1 given");
    }
    g.endFrame(true);
    h.check(!logged("engine motion: tracker", mark), "C: no tracker line: the legacy tracker retired 2026-09-23");
    h.check(logged("(moving rig records the emit wrote a previous pose for); eye-frames", mark),
            "C: the movers line reads without a tracker comparison");
    h.check(logged("(the census is off: it runs only with engine motion's diagnostics", mark),
            "P1: the emit's census is off without diagnostics, and says its zeros are not counts");

    // Read the actual bound MRT6 resource, independent of the view flavor.
    const auto mrt6 = [&] {
        ID3D11RenderTargetView* rt[8] = {};
        h.context->OMGetRenderTargets(8, rt, nullptr);
        ComPtr<ID3D11Resource> result;
        if (rt[edvr::kEngineVelocityTarget]) rt[edvr::kEngineVelocityTarget]->GetResource(&result);
        for (auto* r : rt) if (r) r->Release();
        return result;
    };

    mark = g_log.size();
    edvr::engineVelocityDiagnostics(true);
    g.ordinaryFrame(true);
    h.check(!logged("(the census is off", mark), "P1: with diagnostics the census runs (no off note)");
    edvr::engineVelocityDiagnostics(false);

    // F1: cb1 re-mapped inside eye 0's pass with rows 270..275 unchanged
    // (other registers move, as capture 043720 shows): kept.
    mark = g_log.size();
    g.frameWithViews([&] {
        g.writeScene(g.sceneA.Get(), g.rows[0]); g.pass(0);
        auto other = g.rows[0];
        other[90 * 4] = 7.0f; other[124 * 4 + 1] = -3.0f; other[311 * 4 + 2] = 0.5f;
        g.writeScene(g.sceneA.Get(), other); g.draw();
        g.writeScene(g.sceneA.Get(), g.rows[0]); g.draw();
        g.writeScene(g.sceneA.Get(), g.rows[1]); g.pass(1);
    }, &e0, &e1, true);
    std::string line = lastLine(joined, mark);
    h.check(e0 && e1, "F1: cb1 re-mapped inside the pass with rows 270..275 unchanged keeps the eye-frame");
    h.check(number(line, "rows 270..275 unchanged ") == 2, "F1: both re-maps counted as kept");

    // F1: rows changed inside eye 0's pass, then a draw of eye 0: dropped.
    mark = g_log.size();
    g.frameWithViews([&] {
        g.writeScene(g.sceneA.Get(), g.rows[0]); g.pass(0);
        auto moved = g.rows[0];
        moved[273 * 4] += 0.5f;
        g.writeScene(g.sceneA.Get(), moved); g.draw();
        g.writeScene(g.sceneA.Get(), g.rows[1]); g.pass(1);
    }, &e0, &e1, true);
    line = lastLine(joined, mark);
    h.check(!e0 && e1, "F1: rows 270..275 changed then drawn drops that eye-frame only");
    h.check(number(line, "scene rows 270..275 changed ") == 1, "F1: the drop counted by reason");

    // F1: the capture's interleaving -- eye 0, eye 1, eye 0, eye 1 -- through
    // ONE cb1 whose rows swap between the eyes: both kept.
    mark = g_log.size();
    g.frameWithViews([&] {
        g.writeScene(g.sceneA.Get(), g.rows[0]); g.pass(0);
        g.writeScene(g.sceneA.Get(), g.rows[1]); g.pass(1);
        g.writeScene(g.sceneA.Get(), g.rows[0]); g.pass(0, 3);
        g.writeScene(g.sceneA.Get(), g.rows[1]); g.pass(1, 3);
    }, &e0, &e1, true);
    line = lastLine(joined, mark);
    h.check(e0 && e1, "F1: interleaved eye passes through one cb1 keep both eye-frames");
    h.check(number(line, "invalidated ") == 0, "F1: nothing invalidated by the other eye's rows");

    // R3: sources rebound without touching shaders or targets.
    mark = g_log.size();
    g.frameWithViews([&] {
        g.writeScene(g.sceneA.Get(), g.rows[0]); g.pass(0);
        g.setPool(g.viewB.Get()); g.draw();
        g.setPool(g.viewA.Get());
        g.writeScene(g.sceneA.Get(), g.rows[1]); g.pass(1);
    }, &e0, &e1);
    h.check(!e0 && e1, "R3: t33 rebound to another pool mid-pass drops the eye-frame");
    g.frameWithViews([&] {
        g.writeScene(g.sceneA.Get(), g.rows[0]); g.pass(0);
        g.setScene(g.sceneB.Get()); g.draw();
        g.setScene(g.sceneA.Get());
        g.writeScene(g.sceneA.Get(), g.rows[1]); g.pass(1);
    }, &e0, &e1);
    h.check(!e0 && e1, "R3: b1 rebound to another constant buffer mid-pass drops the eye-frame");
    g.frameWithViews([&] {
        g.writeScene(g.sceneA.Get(), g.rows[0]); g.pass(0);
        g.setPool(g.viewA.Get()); g.draw();          // the same view object again
        g.setPool(g.viewA2.Get()); g.draw();         // another view over the same buffer and elements
        g.setPs(g.ps2.Get(), kPsHash2); g.draw();    // a pixel-shader change alone
        ComPtr<ID3D11PixelShader> bound;
        h.context->PSGetShader(&bound, nullptr, nullptr);
        h.check(bound && bound.Get() != g.ps2.Get() && bound.Get() != g.ps.Get(), "R3: the second keyed PS is substituted too");
        g.setPool(g.viewA.Get()); g.setPs(g.ps.Get(), kPsHash);
        g.writeScene(g.sceneA.Get(), g.rows[1]); g.pass(1);
    }, &e0, &e1);
    h.check(e0 && e1, "R3: the same view, an equal view, a PS change alone: all kept");
    g.frameWithViews([&] {
        g.writeScene(g.sceneA.Get(), g.rows[0]); g.pass(0);
        g.setPool(g.viewOffset.Get()); g.draw();
        g.setPool(g.viewA.Get());
        g.writeScene(g.sceneA.Get(), g.rows[1]); g.pass(1);
    }, &e0, &e1, true);
    h.check(!e0 && e1, "R3: a view with another FirstElement drops the eye-frame");
    line = lastLine(joined, mark);
    h.check(number(line, "pool rebound ") == 1 && number(line, "scene constants rebound ") == 1 &&
            number(line, "pool view changed ") == 1, "R3: each drop counted by its reason");

    // R3b: pool writes between draws of one eye.
    mark = g_log.size();
    g.frameWithViews([&] {
        g.writeScene(g.sceneA.Get(), g.rows[0]); g.pass(0);
        g.writePool(g.poolA.Get(), D3D11_MAP_WRITE_NO_OVERWRITE); g.draw();
        g.writeScene(g.sceneA.Get(), g.rows[1]); g.pass(1);
    }, &e0, &e1);
    h.check(e0 && e1, "R3b: a NO_OVERWRITE append mid-pass refreshes the snapshot and keeps the eye-frame");
    g.frameWithViews([&] {
        g.writeScene(g.sceneA.Get(), g.rows[0]); g.pass(0);
        g.writePool(g.poolA.Get(), D3D11_MAP_WRITE_DISCARD); g.draw();
        g.writeScene(g.sceneA.Get(), g.rows[1]); g.pass(1);
    }, &e0, &e1);
    h.check(!e0 && e1, "R3b: a replaced pool then drawn drops the eye-frame");
    g.frameWithViews([&] {
        g.writeScene(g.sceneA.Get(), g.rows[0]); g.pass(0);
        g.writePool(g.poolA.Get(), D3D11_MAP_WRITE_DISCARD);
        g.writeScene(g.sceneA.Get(), g.rows[1]); g.pass(1);
    }, &e0, &e1, true);
    h.check(e0 && e1, "R3b: a pool replaced after the eye's last draw keeps it");
    line = lastLine(joined, mark);
    h.check(number(line, "pool appended and refreshed ") == 1 && number(line, "pool rewritten ") == 1,
            "R3b: refresh and drop counted");

    // R4: the blend state through the lifecycle.
    {
        D3D11_BLEND_DESC add = edvr::engineVelocityDefaultBlend(), alpha = edvr::engineVelocityDefaultBlend();
        add.RenderTarget[0].BlendEnable = TRUE;
        add.RenderTarget[0].SrcBlend = add.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        add.RenderTarget[0].SrcBlendAlpha = add.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        alpha.RenderTarget[0].BlendEnable = TRUE;
        alpha.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        alpha.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        ComPtr<ID3D11BlendState> additive, blended;
        h.check(SUCCEEDED(h.device->CreateBlendState(&add, &additive)) && SUCCEEDED(h.device->CreateBlendState(&alpha, &blended)),
                "R4: the game's blend states");
        auto current = [&] {
            ComPtr<ID3D11BlendState> s;
            float f[4] = {};
            UINT m = 0;
            h.context->OMGetBlendState(&s, f, &m);
            return s;
        };
        g.beginFrame();
        g.writeScene(g.sceneA.Get(), g.rows[0]);
        g.setBlend(additive.Get());
        g.pass(0);
        auto first = current();
        D3D11_BLEND_DESC d{};
        if (first) first->GetDesc(&d);
        h.check(first && first.Get() != additive.Get(), "R4: a substituted draw runs under a derived state, not the game's");
        h.check(d.RenderTarget[0].BlendEnable && d.RenderTarget[0].DestBlend == D3D11_BLEND_ONE && d.RenderTarget[1].BlendEnable &&
                !d.RenderTarget[6].BlendEnable && d.RenderTarget[6].RenderTargetWriteMask == 3,
                "R4: the derived state keeps the game's blending on its targets and writes MRT6 unblended");
        g.setBlend(blended.Get());
        g.draw();
        auto second = current();
        D3D11_BLEND_DESC d2{};
        if (second) second->GetDesc(&d2);
        h.check(second && second.Get() != blended.Get() && second.Get() != first.Get() &&
                d2.RenderTarget[0].SrcBlend == D3D11_BLEND_SRC_ALPHA && !d2.RenderTarget[6].BlendEnable,
                "R4: a blend change mid-pass is re-derived before the next substituted draw");
        edvr::engineVelocityBeforeDraw(h.context, false);   // a draw elsewhere
        h.check(current().Get() == blended.Get(), "R4: the next unsubstituted draw puts the game's blend state back");
        g.setBlend(additive.Get());
        g.draw();
        g.writeScene(g.sceneA.Get(), g.rows[1]);
        g.pass(1);
        edvr::EngineVelocityViews v{};
        h.check(g.views(0, &v), "R4: blend changes do not drop the eye-frame");
        unsigned exact = 0, other = 0;
        ownership(h, g, 0, v, 5, &exact, &other);
        h.check(exact > 0 && other == 0, "R4: MRT6 exact under an additive game state (the review's counterexample)");
        release(v);
        g.endFrame(true);
        h.check(current().Get() == additive.Get(), "R4: the frame boundary puts the game's blend state back");
        g.setBlend(nullptr);
    }

    // F3: a 24-bit depth is refused MRT6 (counted) and gives nothing; a new
    // depth pair of another size re-creates the slot target (logged) and gives.
    mark = g_log.size();
    g.makeEye(0, kW, kH, DXGI_FORMAT_D24_UNORM_S8_UINT);
    g.ordinaryFrame();
    g.frameWithViews(body, &e0, &e1, true);
    line = lastLine(joined, mark);
    h.check(!e0 && e1, "F3: a depth that is not 32-bit float gets no MRT6 and gives nothing");
    h.check(number(line, "depth not 32-bit float ") >= 2, "F3: the refusal counted");
    mark = g_log.size();
    g.makeEye(0, 32, 32, DXGI_FORMAT_D32_FLOAT);
    g.ordinaryFrame();
    g.frameWithViews(body, &e0, &e1, true);
    h.check(e0 && e1, "F3: a new depth pair gives again");
    h.check(logged("eye 0 slot target re-created 32x32", mark), "F3: the slot target's re-creation is logged");

    // S1: the on-foot source (2026-09-23 "On foot"). A pool draw into the
    // depth screen_motion names -- not an eye's, rtv0 not an eye target --
    // gets MRT6 into a slot target of the SOURCE's size (chosen by the depth,
    // never assumed), logged once; the screen shader's views are given from
    // the second frame; the target follows a re-made source; an unnamed depth
    // of the same shape gets nothing; and kSourceIdleFrames frames without the
    // source release it (logged).
    mark = g_log.size();
    g.makeSource(40, 24);
    g.sourceFrame();
    h.check(!g.sourceViews(), "S1: the first source frame has no previous scene constants: nothing given");
    g.endFrame();
    h.check(logged("on-foot source slot target created 40x24 R32G32 (0.0 MB)", mark),
            "S1: the source's slot target is created at the source's own size, logged with its size");
    g.sourceFrame();
    {
        edvr::EngineVelocityViews v{};
        h.check(g.sourceViews(&v), "S1: the second source frame gives the screen shader its views");
        ComPtr<ID3D11Resource> slotsRes;
        v.slots->GetResource(&slotsRes);
        ComPtr<ID3D11Texture2D> slotsTex;
        h.check(SUCCEEDED(slotsRes.As(&slotsTex)), "S1: a slot texture");
        D3D11_TEXTURE2D_DESC sd{};
        slotsTex->GetDesc(&sd);
        h.check(sd.Width == 40 && sd.Height == 24 && sd.Format == DXGI_FORMAT_R32G32_FLOAT, "S1: the slot target is 40x24 R32G32");
        UINT w = 0, wd = 0;
        const auto slots = readTexture(h, slotsRes.Get(), 2, &w);
        const auto depth = readTexture(h, g.sourceDepth.Get(), 2, &wd);   // D32_FLOAT_S8X24: depth, then stencil
        unsigned exact = 0, other = 0;
        for (size_t i = 0; i < slots.size() / 2; ++i) {
            uint32_t s = 0;
            const int kind = shader_tests::decodeSlot(slots[i * 2], slots[i * 2 + 1], depth[i * 2], &s);
            if (kind == 1 && s == 5) ++exact;
            else if (kind == 1 || kind == 5) ++other;
        }
        h.check(exact > 0 && other == 0, "S1: the source's MRT6 names slot 5, exactly, at the source depth drawn");
        release(v);
        h.check(!g.views(0) && !g.views(1), "S1: on foot the eyes get nothing: their passes drew no pool draw");
    }
    g.endFrame(true);
    line = lastLine("engine motion: on foot:", mark);
    h.check(number(line, "source frames ") >= 2 && number(line, "with MRT6 bound ") >= 2,
            "S1: the on-foot line counts the source frames, MRT6 bound in each");
    h.check(number(line, "screen views asked ") >= 2 && number(line, "given ") >= 1, "S1: and the screen's views asked and given");
    h.check(number(lastLine(joined, mark), "eye-frames ") >= 2 && number(lastLine(joined, mark), "with MRT6 bound ") >= 2,
            "S1: the movers line's eye-frames include the source's, bound");
    // Flat capture brackets source draws too. The source is not an eye RTV:
    // screen_motion names its depth, and engineVelocityBeforeDraw sees
    // rtv0Eye=false. Restore only the game's four MRTs between two draws of
    // this same pass, just as FlatRuntimeDrawScope does after each draw.
    g.beginFrame();
    g.writeScene(g.sceneA.Get(), g.rows[0]);
    edvr::engineVelocityNoteSource(g.sourceDepth.Get(), g.sceneA.Get());
    g.sourcePass(1, 5);
    edvr::EngineVelocityViews sourceFlatViews{};
    h.check(g.sourceViews(&sourceFlatViews), "flat source bracket: first draw has source slot target");
    ComPtr<ID3D11Resource> sourceSlotResource;
    sourceFlatViews.slots->GetResource(&sourceSlotResource);
    h.check(mrt6().Get() == sourceSlotResource.Get(), "flat source bracket: first draw bound MRT6");
    UINT sourceSlotWidth = 0, sourceDepthWidth = 0;
    const auto sourceBefore = readTexture(h, sourceSlotResource.Get(), 2, &sourceSlotWidth);
    edvr::engineVelocityAfterFlatDraw(h.context);
    ID3D11RenderTargetView* sourceOriginals[4] = {
        g.sourceRtv[0].Get(), g.sourceRtv[1].Get(), g.sourceRtv[2].Get(), g.sourceRtv[3].Get()};
    h.context->OMSetRenderTargets(4, sourceOriginals, g.sourceDsv.Get());
    h.check(!mrt6(), "flat source bracket: scope restore removed MRT6 without changing game generations");
    g.sourceDraw(9);
    h.check(mrt6().Get() == sourceSlotResource.Get(), "flat source bracket: second same-pass draw rebound MRT6");
    const auto sourceAfter = readTexture(h, sourceSlotResource.Get(), 2, &sourceSlotWidth);
    const auto sourceDepth = readTexture(h, g.sourceDepth.Get(), 2, &sourceDepthWidth);
    unsigned firstFive = 0, secondNine = 0;
    bool firstPreserved = true;
    for (size_t i = 0; i < sourceBefore.size() / 2; ++i) {
        uint32_t beforeSlot = 0, afterSlot = 0;
        const int beforeKind = shader_tests::decodeSlot(sourceBefore[i * 2], sourceBefore[i * 2 + 1], sourceDepth[i * 2], &beforeSlot);
        const int afterKind = shader_tests::decodeSlot(sourceAfter[i * 2], sourceAfter[i * 2 + 1], sourceDepth[i * 2], &afterSlot);
        if (beforeKind == 1 && beforeSlot == 5) {
            ++firstFive;
            if (afterKind != 1 || afterSlot != 5 ||
                std::memcmp(&sourceBefore[i * 2], &sourceAfter[i * 2], 2 * sizeof(float)) != 0) firstPreserved = false;
        }
        if (afterKind == 1 && afterSlot == 9) ++secondNine;
    }
    h.check(firstFive > 0 && firstPreserved, "flat source bracket: first draw's slot pixels survived the next draw");
    h.check(secondNine > 0, "flat source bracket: second draw wrote distinct slot pixels");
    release(sourceFlatViews);
    g.endFrame();
    // The source re-made at another size: the slot target follows it.
    mark = g_log.size();
    g.makeSource(56, 20);
    g.sourceFrame();
    g.endFrame();
    g.sourceFrame();
    h.check(g.sourceViews(), "S1: a re-made source gives again");
    g.endFrame();
    h.check(logged("on-foot source slot target re-created 56x20", mark), "S1: the slot target follows the source's size");
    // A depth of the source's shape that screen_motion did not name: nothing.
    {
        g.makeSource(56, 20);   // new textures; the engine still holds the named one
        g.sourceFrame(false);
        ComPtr<ID3D11PixelShader> bound;
        h.context->PSGetShader(&bound, nullptr, nullptr);
        h.check(bound.Get() == g.ps.Get(), "S1: a pool draw into an unnamed depth is not substituted");
        h.check(!g.sourceViews(), "S1: and an unnamed depth gets no views");
        g.endFrame();
    }
    // kSourceIdleFrames frames without the source: released.
    mark = g_log.size();
    for (uint32_t i = 0; i < edvr::kSourceIdleFrames + 2; ++i) { g.beginFrame(); g.endFrame(); }
    h.check(logged("on-foot source slot target released (56x20", mark), "S1: the slot target is released when the source stops");
    h.check(!g.sourceViews(), "S1: released, nothing is given");

    // S2: the source's camera rule (flight 5, 2026-09-23 140351: every walking
    // source frame dropped for "scene rows 270..275 changed" after two runs of
    // vs_AACF draws, the first substituted). The frames as the game draws them
    // walking: the naming draw reads the world's rows W(k); two pool draws of
    // another camera (W(k) standing, W(k) with a 4 cm bob walking); the
    // world's pool draws under W(k) again. The same interleaving in an EYE
    // pass still drops the eye-frame (the eyes' rule, unchanged: the flight's
    // drop). On the source: the other camera's draws declined and counted by
    // reason, family and rows; the world's substituted; every frame given from
    // the second; the view's scene constants the world's rows exactly, this
    // frame's and last; MRT6 names only the world's slot. A draw before this
    // frame's naming (last frame's rows still in cb1) is declined as such; a
    // standing frame declines nothing.
    {
        const auto world = [&](int k) {
            std::vector<float> r = g.rows[0];
            r[275 * 4 + 0] = 0.05f * float(k); r[275 * 4 + 2] = -0.05f * float(k);   // walking: the position moves
            return r;
        };
        const auto bobbed = [&](int k) { std::vector<float> r = world(k); r[275 * 4 + 1] += 0.04f; return r; };
        mark = g_log.size();
        g.ordinaryFrame();
        g.beginFrame();
        g.writeScene(g.sceneA.Get(), bobbed(1));
        g.pass(0, 2, 9);
        g.writeScene(g.sceneA.Get(), world(1));
        g.draw();
        g.writeScene(g.sceneA.Get(), g.rows[1]);
        g.pass(1);
        h.check(!g.views(0) && g.views(1), "S2: the flight's interleaving in an eye pass still drops that eye-frame (the eyes' rule)");
        g.endFrame(true);
        h.check(number(lastLine(joined, mark), "scene rows 270..275 changed ") == 1, "S2: counted as the flight counted it");
        mark = g_log.size();
        g.makeSource(40, 24);
        const auto walk = [&](int k, bool walking, bool staleFirst) {
            g.beginFrame();
            if (staleFirst) {   // a pool draw under last frame's rows, before this frame's naming
                g.writeScene(g.sceneA.Get(), world(k - 1));
                g.sourcePass(1, 9);
            }
            g.writeScene(g.sceneA.Get(), world(k));
            edvr::engineVelocityNoteSource(g.sourceDepth.Get(), g.sceneA.Get());
            g.writeScene(g.sceneA.Get(), walking ? bobbed(k) : world(k));
            if (staleFirst) { g.sourceDraw(9); g.sourceDraw(9); }
            else g.sourcePass(2, 9);   // the other camera's two draws, first in the frame
            g.writeScene(g.sceneA.Get(), world(k));
            for (int i = 0; i < 3; ++i) g.sourceDraw(5);   // the world's
        };
        bool given[5] = {};
        for (int k = 1; k <= 4; ++k) {
            walk(k, true, k == 4);
            edvr::EngineVelocityViews v{};
            given[k] = g.sourceViews(&v);
            if (given[k] && k == 3) {
                const auto now = readBuffer(h, v.sceneNow), before = readBuffer(h, v.scenePrev);
                const auto want = world(3), wantBefore = world(2);
                h.check(now.size() >= 276 * 4 && std::memcmp(&now[270 * 4], &want[270 * 4], 24 * 4) == 0,
                        "S2: the source's scene constants now are the world's rows 270..275, exactly");
                h.check(before.size() >= 276 * 4 && std::memcmp(&before[270 * 4], &wantBefore[270 * 4], 24 * 4) == 0,
                        "S2: and last frame's are the world's rows of last frame");
                ComPtr<ID3D11Resource> slotsRes;
                v.slots->GetResource(&slotsRes);
                UINT w = 0, wd = 0;
                const auto slots = readTexture(h, slotsRes.Get(), 2, &w);
                const auto depth = readTexture(h, g.sourceDepth.Get(), 2, &wd);
                unsigned five = 0, nine = 0;
                for (size_t i = 0; i < slots.size() / 2; ++i) {
                    uint32_t s = 0;
                    const int kind = shader_tests::decodeSlot(slots[i * 2], slots[i * 2 + 1], depth[i * 2], &s);
                    if (kind == 1 && s == 5) ++five;
                    if (kind == 1 && s == 9) ++nine;
                }
                h.check(five > 0 && nine == 0, "S2: MRT6 names the world's slot and never the other camera's");
            }
            release(v);
            g.endFrame(k == 4);
        }
        h.check(!given[1] && given[2] && given[3] && given[4], "S2: walking, every source frame is given from the second");
        // Counted per CHECK -- a slow-path visit: a new binding or a cb1
        // write; a draw repeating the last one's state is its twin -- so one
        // world check a frame, one other-camera check, the early one.
        line = lastLine("engine motion: on foot:", mark);
        const auto shown = [&](bool ok) { if (!ok) std::printf("    on-foot line| %s\n", line.c_str()); return ok; };
        h.check(shown(line.find("frames dropped: none") != std::string::npos), "S2: no source frame dropped");
        h.check(shown(number(line, "namings ") == 4 && number(line, "rows not seen ") == 0), "S2: four namings, their rows seen");
        h.check(shown(number(line, "held to the naming's camera ") == 4), "S2: each frame's world check held to the naming's camera");
        h.check(shown(line.find("declined 5 in 4 frames") != std::string::npos), "S2: five checks declined, in all four frames");
        h.check(shown(number(line, "another camera ") == 4), "S2: the other camera's, one a frame, as another camera");
        h.check(shown(number(line, "before this frame's naming ") == 1), "S2: the draw before the naming as such");
        h.check(shown(number(line, "275 on ") == 4 && number(line, "270..272 on ") == 0 && number(line, "273 on ") == 0 &&
                      number(line, "274 on ") == 0), "S2: the other camera changed row 275 only");
        h.check(shown(line.find("its position up to 0.040 m") != std::string::npos), "S2: by the bob's 4 cm");
        h.check(shown(line.find("by family: vs_EB5234DB6ADB491D 4") != std::string::npos), "S2: counted by family");
        mark = g_log.size();
        for (int k = 0; k < 2; ++k) { walk(5, false, false); g.sourceViews(); g.endFrame(k == 1); }
        line = lastLine("engine motion: on foot:", mark);
        h.check(shown(number(line, "declined ") == 0 && number(line, "held to the naming's camera ") == 4),
                "S2: standing still every check is held and none declined");
        // S3 (flight 6, the hangar): a source named by its own depth -- no
        // terrain or scene draw -- is held by the same camera rule and counted
        // under its own signal on the on-foot line.
        mark = g_log.size();
        for (int k = 0; k < 2; ++k) {
            g.beginFrame();
            g.writeScene(g.sceneA.Get(), world(6));
            edvr::engineVelocityNoteSource(g.sourceDepth.Get(), g.sceneA.Get(),
                                           edvr::EngineVelocitySourceSignal::ScreenDepth);
            g.sourcePass(2);
            h.check(g.sourceViews(), "S3: the screen's depth names a source frame like terrain does (given)");
            g.endFrame(k == 1);
        }
        line = lastLine("engine motion: on foot:", mark);
        h.check(shown(number(line, "namings ") == 2 && number(line, "by the screen's own depth ") == 2 &&
                      number(line, "by terrain or a scene draw ") == 0 && line.find("frames dropped: none") != std::string::npos),
                "S3: the on-foot line counts the namings by the screen's own depth under their signal");
    }

    // P2 (the performance review, item 2): eligibility before preparation. An
    // eye whose family draws all carry an unkeyed pixel shader prepares
    // nothing (no slot target clear, no snapshot, no MRT6) and gives nothing;
    // the movers line counts what the old order would have prepared. One
    // accepted draw brings coverage back: its frame lacks last frame's scene
    // constants (the idle frame took none) and is refused as such, the next
    // gives, with MRT6 naming the slot exactly.
    mark = g_log.size();
    g.ordinaryFrame();
    g.beginFrame();
    g.writeScene(g.sceneA.Get(), g.rows[0]);
    g.setPs(g.unkeyed.Get(), kUnkeyedPs);
    g.pass(0, 3);
    g.setPs(g.ps.Get(), kPsHash);
    g.writeScene(g.sceneA.Get(), g.rows[1]);
    g.pass(1);
    h.check(!g.views(0), "P2: an eye with only unkeyed pool draws prepares nothing and gives nothing");
    h.check(g.views(1), "P2: the other eye is untouched");
    g.endFrame(true);
    line = lastLine(joined, mark);
    h.check(number(line, "prepared for nothing ") == 0, "P2: no eye-frame is prepared for nothing");
    h.check(number(line, "under the old order ") >= 1, "P2: the old order would have prepared the unkeyed-only eye-frame");
    mark = g_log.size();
    g.beginFrame();
    g.writeScene(g.sceneA.Get(), g.rows[0]);
    g.setPs(g.unkeyed.Get(), kUnkeyedPs);
    g.pass(0, 2);
    g.setPs(g.ps.Get(), kPsHash);
    g.draw();   // one accepted draw after the declined ones
    g.writeScene(g.sceneA.Get(), g.rows[1]);
    g.pass(1);
    h.check(!g.views(0), "P2: the first accepted frame has no previous scene constants for that eye: refused");
    g.endFrame(true);
    line = lastLine(joined, mark);
    h.check(number(line, "no previous scene constants ") >= 1, "P2: and the refusal says why");
    g.beginFrame();
    g.writeScene(g.sceneA.Get(), g.rows[0]);
    g.pass(0);
    g.writeScene(g.sceneA.Get(), g.rows[1]);
    g.pass(1);
    {
        edvr::EngineVelocityViews v{};
        h.check(g.views(0, &v), "P2: the next frame gives again");
        unsigned exact = 0, other = 0;
        ownership(h, g, 0, v, 5, &exact, &other);
        h.check(exact > 0 && other == 0, "P2: coverage restored -- MRT6 names slot 5 exactly");
        release(v);
    }
    g.endFrame();

    // P3 (item 3): a slow-path visit that only re-verified the sources finds
    // the patched shader still bound and skips the setter; each eye-frame's
    // first substitution still sets it.
    mark = g_log.size();
    g.beginFrame();
    g.writeScene(g.sceneA.Get(), g.rows[0]);
    g.pass(0);
    {
        ComPtr<ID3D11PixelShader> patched, after;
        h.context->PSGetShader(&patched, nullptr, nullptr);
        auto remap = g.rows[0];
        remap[90 * 4] = 3.0f;   // other registers move, rows 270..275 do not
        g.writeScene(g.sceneA.Get(), remap);
        g.draw();
        h.context->PSGetShader(&after, nullptr, nullptr);
        h.check(patched && patched.Get() != g.ps.Get() && after.Get() == patched.Get(),
                "P3: the patched pixel shader stays bound across a source-only visit");
    }
    g.writeScene(g.sceneA.Get(), g.rows[1]);
    g.pass(1);
    g.endFrame(true);
    line = lastLine(joined, mark);
    h.check(number(line, ", skipped ") >= 1, "P3: the setter a source-only visit would have repeated is skipped, counted");
    h.check(number(line, "shader setters issued ") >= 2, "P3: each eye-frame's first substitution still sets it");

    // P4 (item 4, measure only): the snapshot copies counted, with the pool's
    // capacity and the view's exposed records.
    mark = g_log.size();
    g.beginFrame();
    g.writeScene(g.sceneA.Get(), g.rows[0]);
    g.pass(0);
    g.writePool(g.poolA.Get(), D3D11_MAP_WRITE_NO_OVERWRITE);   // an append mid-pass: refreshed
    g.draw();
    g.writeScene(g.sceneA.Get(), g.rows[1]);
    g.pass(1);
    g.endFrame(true);
    line = lastLine("engine motion: snapshots (measure only):", mark);
    h.check(number(line, "pool capacity ") == 16 && number(line, "views expose ") == 16,
            "P4: the pool's capacity and the view's exposed records");
    h.check(number(line, "copies: pool ") >= 2 && number(line, "scene constants ") >= 2,
            "P4: one pool and one scene-constant copy per prepared eye-frame");
    h.check(number(line, "MB) + ") >= 1, "P4: and the append refresh counted");

    // P5 (attribution gap 1): every prepared eye-frame's clear and snapshot,
    // and the refresh, went through a GPU timer -- measured, or counted as
    // untimed/invalid, never silently lost -- and the take resets the window.
    {
        edvr::EngineVelocityCaptureGpu c{};
        edvr::engineVelocityTakeCaptureGpu(&c);
        h.check(c.events[0] + c.events[1] + c.events[2] + c.untimed + c.invalid >= 1,
                "P5: the eye-pass capture is timed or its absence counted");
        edvr::EngineVelocityCaptureGpu again{};
        edvr::engineVelocityTakeCaptureGpu(&again);
        h.check(again.events[0] + again.events[1] + again.events[2] + again.untimed + again.invalid == 0,
                "P5: taking the window resets it");
    }

    // R6: the emit hook not installed -> STOOD DOWN, no substitution, nothing given.
    mark = g_log.size();
    lifecycle_fake::g_hookLive = false;
    g.ordinaryFrame();
    h.check(logged("engine motion: STOOD DOWN -- the emit hook is not installed (rig: kinematic-build-144312e00 refused)", mark),
            "R6: the stand-down is logged when the hook goes");
    g.beginFrame();
    g.writeScene(g.sceneA.Get(), g.rows[0]);
    g.pass(0);
    {
        ComPtr<ID3D11PixelShader> bound;
        h.context->PSGetShader(&bound, nullptr, nullptr);
        h.check(bound.Get() == g.ps.Get(), "R6: stood down, nothing is substituted");
    }
    h.check(!g.views(0), "R6: stood down, every view is refused");
    const size_t windowMark = g_log.size();
    g.endFrame(true);
    h.check(logged("engine motion: STOOD DOWN", windowMark), "R6: the 30 s block repeats the stand-down");
    line = lastLine(joined, windowMark);
    h.check(number(line, "refused: stood down ") >= 1, "R6: the refusals counted");
    mark = g_log.size();
    lifecycle_fake::g_hookLive = true;
    g.ordinaryFrame();
    h.check(logged("engine-record velocity stands up", mark), "R6: installed again, it stands up");
    g.ordinaryFrame();
    g.frameWithViews(body, &e0, &e1);
    h.check(e0 && e1, "R6: and gives again");
    // The real flat producer path, including slowPath's group copy and the
    // FlatRuntimeDrawScope-style MRT restore after every draw.
    {
        edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
        const auto vb = g.compile(std::string(shader_tests::kVsCommon) + shader_tests::kVsB, "vs_5_0");
        const auto pb = g.compile(shader_tests::kPsB, "ps_5_0");
        ComPtr<ID3D11VertexShader> overlayVs;
        ComPtr<ID3D11PixelShader> overlayPs;
        h.check(SUCCEEDED(g.dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &overlayVs)) &&
                SUCCEEDED(g.dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &overlayPs)),
                "flat overlay lifecycle: original shaders created");
        constexpr uint64_t overlayVsHash = 0xBBE58E40FE88EC80ull;
        constexpr uint64_t overlayPsHash = 0xDB3E8D20CF53FBC0ull;
        edvr::engineVelocityRememberVs(overlayVs.Get(), overlayVsHash, vb->GetBufferPointer(), vb->GetBufferSize(), false);
        edvr::engineVelocityRememberPs(overlayPs.Get(), overlayPsHash, pb->GetBufferPointer(), pb->GetBufferSize(), false);
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        ComPtr<ID3D11DepthStencilState> overlayDepth;
        h.check(SUCCEEDED(g.dev->CreateDepthStencilState(&dd, &overlayDepth)), "flat overlay lifecycle: depth-write-off state");
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = sd.Height = sd.MipLevels = sd.ArraySize = sd.SampleDesc.Count = 1;
        sd.Format = DXGI_FORMAT_R32G32_FLOAT; sd.Usage = D3D11_USAGE_DEFAULT; sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> sentinelTexture;
        ComPtr<ID3D11ShaderResourceView> sentinel;
        h.check(SUCCEEDED(g.dev->CreateTexture2D(&sd, nullptr, &sentinelTexture)) &&
                SUCCEEDED(g.dev->CreateShaderResourceView(sentinelTexture.Get(), nullptr, &sentinel)),
                "flat overlay lifecycle: game t3 sentinel");
        ID3D11ShaderResourceView* gameT3 = sentinel.Get();
        g.ctx->PSSetShaderResources(3, 1, &gameT3);
        g.shadow(BindSlot::PsSrv3, gameT3);
        auto gameTargets = [&] {
            ID3D11RenderTargetView* rt[4] = {
                g.sourceRtv[0].Get(), g.sourceRtv[1].Get(), g.sourceRtv[2].Get(), g.sourceRtv[3].Get()};
            g.ctx->OMSetRenderTargets(4, rt, g.sourceDsv.Get());
        };
        auto closeDraw = [&] { edvr::engineVelocityAfterFlatDraw(g.ctx); gameTargets(); };
        auto underlay = [&] {
            g.setVs(); g.setPs(g.ps.Get(), kPsHash);
            g.ctx->OMSetDepthStencilState(g.depthState.Get(), 0);
            g.sourceDraw(5);
            h.check(edvr::engineVelocityDrawSubstituted(), "flat overlay lifecycle: underlay produced MRT6");
            closeDraw();
        };
        auto overlay = [&] {
            g.ctx->VSSetShader(overlayVs.Get(), nullptr, 0); g.shadow(BindSlot::Vs, overlayVs.Get(), overlayVsHash);
            g.setPs(overlayPs.Get(), overlayPsHash);
            g.ctx->OMSetDepthStencilState(overlayDepth.Get(), 5);
            edvr::engineVelocityBeforeDraw(g.ctx, false);
            h.check(edvr::engineVelocityDrawSubstituted(), "flat overlay lifecycle: overlay produced MRT6");
            ComPtr<ID3D11ShaderResourceView> bound;
            g.ctx->PSGetShaderResources(3, 1, &bound);
            h.check(bound && bound.Get() != sentinel.Get(), "flat overlay lifecycle: guarded PS has private snapshot at t3");
            g.ctx->DrawInstanced(4, 1, 0, 0);
            closeDraw();
            bound.Reset(); g.ctx->PSGetShaderResources(3, 1, &bound);
            h.check(bound.Get() == sentinel.Get(), "flat overlay lifecycle: original game t3 restored");
        };
        const size_t overlayMark = g_log.size();
        g.beginFrame(); g.writeScene(g.sceneA.Get(), g.rows[0]);
        edvr::engineVelocityNoteSource(g.sourceDepth.Get(), g.sceneA.Get());
        g.sourcePass(1, 5); closeDraw();
        overlay(); overlay();
        underlay(); overlay();
        g.endFrame();
        g.beginFrame(); g.writeScene(g.sceneA.Get(), g.rows[0]);
        edvr::engineVelocityNoteSource(g.sourceDepth.Get(), g.sceneA.Get());
        g.sourcePass(1, 5); closeDraw();
        overlay();
        g.endFrame(true);
        const std::string summary = lastLine("engine motion: flat overlay guard:", overlayMark);
        h.check(number(summary, "copies ") == 3 && number(summary, "guarded draws ") == 4,
                "flat overlay lifecycle: one base copy per group, refreshed after other producer and next frame");
        h.check(number(summary, "declined state ") == 0 && number(summary, "resource ") == 0 &&
                number(summary, "shader ") == 0, "flat overlay lifecycle: no guarded path fallback");
        edvr::g_runtimeProfile = edvr::RuntimeProfile::LegacyVr;
    }
    const unsigned detachesBefore = lifecycle_fake::g_emitDetaches;
    edvr::engineVelocityShutdown();
    h.check(lifecycle_fake::g_emitDetaches == detachesBefore + 1, "P1: shutdown detaches the emit's want on the hook set");
    mark = g_log.size();
    lifecycle_fake::g_hookLive = false;
    edvr::engineVelocityConfigure(true);
    h.check(logged("engine motion: STOOD DOWN -- the emit hook is not installed", mark), "R6: configure logs the stand-down");
    edvr::engineVelocityShutdown();
    lifecycle_fake::g_hookLive = true;

    csStageSaveChecks(h);
    edvr::g_clockForTest = nullptr;
    if (g_verbose) for (const auto& l : g_log) std::printf("    log| %s\n", l.c_str());
    std::printf("  lifecycle: engine_velocity.cpp's draw half on WARP -- re-maps, interleaved eyes, source swaps, pool "
                "writes, blend states, depth formats, the on-foot source's slot target, stand-down: every case as "
                "specified (%zu log lines)\n", g_log.size());
}

}  // namespace lifecycle_tests
