// The VR Galaxy Map's run of sprites (src\d3d11\map_run_core.h, docs\galaxy-map-vr-2026-11-03.md "After the fifth flight"):
//   R1 the state machine alone: a run of N sprites of colour and depth issues with no game calls costs one bind and one end, and the test FAILS above two binds a run. The same run under the
//      scheme before the run (the layer's targets bound and the game's rebound around every issue) is counted beside it, so a count that never moved would show
//   R2 the game sets exactly the targets the run saved: no bind, the run stays held; it sets others (fewer, more, a different depth, a UAV beside them): the hold is dropped with no bind
//   R3 any draw that is not a run draw, an issue of another kind or for another eye or layer, a ClearState, a frame boundary: the run ends once (one bind) and ending again does nothing
//   R4 on a WARP device with the sprites' real depth-stencil states, rasterizer and probe blend: a held run draws the sprites in the layer (near before far, far before near, a second quad in the
//      same plane), binds the layer and its D16 twice in all (the begin and the end), leaves the game's R11G11B10 target untouched, and after the end OMGetRenderTargets returns the game's targets
// R4's driver is this file's own sequence of D3D calls in the order ui_layer.cpp's beginInner and restore() make them, made through the same Core; ui_layer.cpp itself is not linked here.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "galaxy_map_ui.h"
#include "map_run_core.h"

using Microsoft::WRL::ComPtr;
using edvr::maprun::Core;
using edvr::maprun::GameSet;
using edvr::maprun::Step;

namespace {

int failures = 0;
unsigned checks = 0;
void check(bool ok, const std::string& label) {
    ++checks;
    if (!ok) { std::printf("FAIL %s\n", label.c_str()); ++failures; }
}
void hr(HRESULT h, const char* what) {
    if (FAILED(h)) { std::printf("FAIL D3D %s 0x%08lx\n", what, static_cast<unsigned long>(h)); std::exit(1); }
}

// Distinct addresses to stand for views.
int g_obj[64];
const void* obj(int i) { return &g_obj[i]; }

struct Targets {
    const void* rtv[edvr::maprun::kTargets] = {};
    const void* dsv = nullptr;
};
Targets gameTargets() {
    Targets t;
    t.rtv[0] = obj(0);
    t.dsv = obj(1);
    return t;
}

// What ui_layer.cpp does for one issue of a run kind, as a count: begin (and the end it implies), the bind, the hand-over at restore.
struct Driver {
    Core core;
    unsigned realBinds = 0;
    void issue(int eye, const void* layerRtv, const void* layerDsv, bool runKind = true) {
        const Step step = core.begin(runKind, eye, layerRtv, layerDsv);
        if (step == Step::kEndThenBind) ++realBinds;   // the game's targets back
        if (step != Step::kKeep) ++realBinds;           // the issue's own bind
        if (runKind) {
            const Targets g = gameTargets();
            const void* game[edvr::maprun::kTargets] = {};
            for (int i = 0; i < edvr::maprun::kTargets; ++i) game[i] = g.rtv[i];
            core.handOver(eye, layerRtv, layerDsv, game, g.dsv);
        } else {
            ++realBinds;   // restore() binds the game's targets
            core.drop();
        }
    }
    void end() { if (core.end()) ++realBinds; }
};

void runCoreTests() {
    // R1: 432 sprites, a colour issue and a depth issue each, no game calls in between
    {
        Driver d;
        const int sprites = 432;
        for (int i = 0; i < sprites; ++i) {
            d.issue(0, obj(10), obj(11));   // colour
            d.issue(0, obj(10), obj(11));   // depth
        }
        d.end();
        char label[200];
        std::snprintf(label, sizeof label, "R1: %d sprites (%d issues) cost %u real binds and the core counted %llu; %llu run(s) started and %llu ended", sprites, sprites * 2, d.realBinds,
                      static_cast<unsigned long long>(d.core.binds), static_cast<unsigned long long>(d.core.started), static_cast<unsigned long long>(d.core.ended));
        std::printf("%s\n", label);
        check(d.realBinds == 2 && d.core.binds == 2 && d.core.started == 1 && d.core.ended == 1, label);
        check(d.realBinds <= 2, "R1: no more than two binds a run");
        // the scheme before the run: the layer's targets bound and the game's rebound around every issue
        const unsigned before = static_cast<unsigned>(sprites * 2 * 2);
        std::snprintf(label, sizeof label, "R1.before: the layer's and the game's targets bound around every issue is %u binds for the same sprites (the test fails above two)", before);
        std::printf("%s\n", label);
        check(before > 2, label);
        // both eyes: a run per eye, each its own D16: the eye changes end the first run
        Driver e;
        for (int i = 0; i < 10; ++i) { e.issue(0, obj(10), obj(11)); }
        for (int i = 0; i < 10; ++i) { e.issue(1, obj(12), obj(13)); }
        e.end();
        check(e.core.started == 2 && e.core.ended == 2 && e.realBinds == 4, "R1.eyes: a run in each eye is two runs, four binds");
    }
    // R2: the game sets what the run saved, or something else
    {
        Driver d;
        d.issue(0, obj(10), obj(11));
        const void* same[1] = {obj(0)};
        check(d.core.onGameSet(1, same, obj(1), false) == GameSet::kKeep && d.core.held, "R2: the same targets are kept, the run stays held");
        d.issue(0, obj(10), obj(11));   // and the next issue binds nothing
        check(d.realBinds == 1 && d.core.setsKept == 1, "R2: a game set of the same targets costs no bind, and the next issue of the run none");
        const unsigned binds = d.realBinds;
        const void* other[1] = {obj(5)};
        check(d.core.onGameSet(1, other, obj(1), false) == GameSet::kDrop && !d.core.held && d.core.setsDropped == 1, "R2: other targets drop the hold");
        check(d.realBinds == binds && d.core.binds == 1, "R2: a dropped run makes no bind");
        check(d.core.onGameSet(1, same, obj(1), false) == GameSet::kNotHeld, "R2: with no run the game's call is made");
        // each way to differ
        struct Case { const char* name; unsigned n; const void* rtv[2]; const void* dsv; bool uav; };
        const Case cases[] = {
            {"a different depth view", 1, {obj(0), nullptr}, obj(7), false},
            {"no depth view", 1, {obj(0), nullptr}, nullptr, false},
            {"a second target beside the first", 2, {obj(0), obj(8)}, obj(1), false},
            {"no targets", 0, {nullptr, nullptr}, obj(1), false},
            {"the same targets with a UAV beside them", 1, {obj(0), nullptr}, obj(1), true},
        };
        for (const Case& c : cases) {
            Driver x;
            x.issue(0, obj(10), obj(11));
            check(x.core.onGameSet(c.n, c.rtv, c.dsv, c.uav) == GameSet::kDrop && !x.core.held, std::string("R2: ") + c.name + " drops the hold");
        }
    }
    // R2.own: the game reads the targets inside a run, which returns the layer's, and sets what it read
    {
        Driver d;
        const void* own[1] = {obj(10)};
        check(!d.core.onOwnSet(1, own, obj(11)), "R2.own: with no run the game's call is made");
        d.issue(0, obj(10), obj(11));
        check(d.core.onOwnSet(1, own, obj(11)) && d.core.held && d.core.setsOwn == 1, "R2.own: the layer's own targets keep the run held");
        const void* readBack[8] = {obj(10)};
        check(d.core.onOwnSet(8, readBack, obj(11)) && d.core.setsOwn == 2, "R2.own: so do eight slots read back with the last seven empty");
        check(d.core.gameRtv[0] == obj(0) && d.core.gameDsv == obj(1), "R2.own: the game's targets stay the saved ones");
        d.issue(0, obj(10), obj(11));
        check(d.realBinds == 1, "R2.own: it costs no bind, and the next issue of the run none");
        const void* game[1] = {obj(0)};
        const void* beside[2] = {obj(10), obj(8)};
        check(!d.core.onOwnSet(1, game, obj(1)) && !d.core.onOwnSet(1, own, obj(1)) && !d.core.onOwnSet(2, beside, obj(11)) && !d.core.onOwnSet(0, own, obj(11)) && d.core.setsOwn == 2,
              "R2.own: the saved targets, another depth view, a second target and no targets are not the layer's own");
        check(d.core.take().setsOwn == 2 && d.core.setsOwn == 0, "R2.own: take() hands the count over and clears it");
    }
    // R3: a draw that is not a run draw ends the run once
    {
        Driver d;
        for (int i = 0; i < 20; ++i) d.issue(0, obj(10), obj(11));
        check(d.core.end(), "R3: a non-run draw ends a held run");
        ++d.realBinds;
        check(!d.core.end() && d.core.ended == 1, "R3: ending again does nothing");
        check(d.realBinds == 2 && d.core.binds == 2, "R3: a run ended by a non-run draw cost one bind and one end");
        // an issue of another kind while held ends the run first
        Driver k;
        k.issue(0, obj(10), obj(11));
        k.issue(0, obj(20), nullptr, false);   // a mask or probe issue
        check(!k.core.held && k.core.ended == 1, "R3: an issue of another kind ends the run before it binds");
        // another layer and another eye
        Driver l;
        l.issue(0, obj(10), obj(11));
        l.issue(0, obj(14), obj(11));
        check(l.core.started == 2 && l.core.ended == 1, "R3: another render target view is another run");
        // dropped by a ClearState: no bind, and an end afterwards does nothing
        Driver m;
        m.issue(0, obj(10), obj(11));
        const unsigned b = m.realBinds;
        m.core.drop();
        m.end();
        check(m.realBinds == b && !m.core.held, "R3: a ClearState drops the run with no bind");
        const Core::Counts c = m.core.take();
        check(c.started == 1 && c.ended == 1 && m.core.binds == 0, "R3: the counters are taken and cleared");
    }
}

// ---- R4: WARP -------------------------------------------------------------------------------------------------------------
const char kVs[] = R"HLSL(
cbuffer C : register(b0) { float4 rect; float4 color; float4 zv; };
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 t = float2(id & 1, id >> 1);
    return float4(lerp(rect.xy, rect.zw, t), zv.x, 1.0);
}
)HLSL";
const char kPs[] = R"HLSL(
cbuffer C : register(b0) { float4 rect; float4 color; float4 zv; };
float4 main() : SV_Target { return color; }
)HLSL";

std::vector<uint8_t> compile(const char* source, const char* profile) {
    ComPtr<ID3DBlob> code, err;
    const HRESULT h = D3DCompile(source, std::strlen(source), "map_run_test", nullptr, nullptr, "main", profile, 0, 0, &code, &err);
    if (FAILED(h)) { std::printf("shader: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); std::exit(1); }
    const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
    return std::vector<uint8_t>(p, p + code->GetBufferSize());
}

constexpr UINT kSize = 64;

struct Warp {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> gameTex, layerTex, gameDepth, layerDepth;
    ComPtr<ID3D11RenderTargetView> gameRtv, layerRtv;
    ComPtr<ID3D11DepthStencilView> gameDsv, layerDsv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11DepthStencilState> testState, writeState, gameState;
    ComPtr<ID3D11RasterizerState> raster, depthRaster;
    ComPtr<ID3D11BlendState> colourBlend, writeMask0;
    unsigned realBinds = 0;
};

ComPtr<ID3D11Texture2D> makeTex(Warp& w, DXGI_FORMAT f, UINT bind) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = kSize;
    d.MipLevels = d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    ComPtr<ID3D11Texture2D> t;
    hr(w.dev->CreateTexture2D(&d, nullptr, &t), "CreateTexture2D");
    return t;
}

std::vector<uint8_t> readback(Warp& w, ID3D11Texture2D* tex) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    hr(w.dev->CreateTexture2D(&d, nullptr, &st), "staging");
    w.ctx->CopyResource(st.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    hr(w.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m), "Map");
    std::vector<uint8_t> out(kSize * kSize * 4);
    for (UINT y = 0; y < kSize; ++y) std::memcpy(&out[y * kSize * 4], static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, kSize * 4);
    w.ctx->Unmap(st.Get(), 0);
    return out;
}

// One sprite: a rect in NDC at depth z, drawn into the targets bound now.
void drawQuad(Warp& w, float x0, float y0, float x1, float y1, float z, const float color[4]) {
    const float c[12] = {x0, y0, x1, y1, color[0], color[1], color[2], color[3], z, 0, 0, 0};
    w.ctx->UpdateSubresource(w.cb.Get(), 0, nullptr, c, 0, 0);
    w.ctx->Draw(4, 0);
}

// ui_layer.cpp beginInner / restore(), for one issue of the run, through the Core. `depthIssue`: the depth state that writes, the biased rasterizer, write mask 0.
struct Run {
    Warp& w;
    Core core;
    ID3D11RenderTargetView* savedRtv = nullptr;   // the game's, owned by the run once started
    ID3D11DepthStencilView* savedDsv = nullptr;
    bool heldRefs = false;

    explicit Run(Warp& warp) : w(warp) {}
    void bind(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv) { w.ctx->OMSetRenderTargets(1, &rtv, dsv); ++w.realBinds; }
    void issue(bool depthIssue, float x0, float y0, float x1, float y1, float z, const float color[4]) {
        const Step step = core.begin(true, 0, w.layerRtv.Get(), w.layerDsv.Get());
        ID3D11RenderTargetView* gameRtv[8] = {};
        ID3D11DepthStencilView* gameDsv = nullptr;
        if (step == Step::kEndThenBind) endNow();
        if (step == Step::kKeep) {
            savedRtv->AddRef(); gameRtv[0] = savedRtv;
            savedDsv->AddRef(); gameDsv = savedDsv;
        } else {
            w.ctx->OMGetRenderTargets(8, gameRtv, &gameDsv);
            bind(w.layerRtv.Get(), w.layerDsv.Get());
        }
        ID3D11DepthStencilState* dss0 = nullptr; UINT ref = 0;
        w.ctx->OMGetDepthStencilState(&dss0, &ref);
        ID3D11RasterizerState* rs0 = nullptr;
        w.ctx->RSGetState(&rs0);
        ID3D11BlendState* bs0 = nullptr; FLOAT f[4]; UINT sm = 0;
        w.ctx->OMGetBlendState(&bs0, f, &sm);
        w.ctx->OMSetDepthStencilState(depthIssue ? w.writeState.Get() : w.testState.Get(), 0);
        w.ctx->RSSetState(depthIssue ? w.depthRaster.Get() : w.raster.Get());
        w.ctx->OMSetBlendState(depthIssue ? w.writeMask0.Get() : w.colourBlend.Get(), nullptr, 0xFFFFFFFFu);
        drawQuad(w, x0, y0, x1, y1, z, color);
        // restore(): the game's states back each issue; the targets pass to the run
        w.ctx->OMSetDepthStencilState(dss0, ref);
        w.ctx->RSSetState(rs0);
        w.ctx->OMSetBlendState(bs0, f, sm);
        if (dss0) dss0->Release();
        if (rs0) rs0->Release();
        if (bs0) bs0->Release();
        const void* game[edvr::maprun::kTargets] = {gameRtv[0]};
        if (core.handOver(0, w.layerRtv.Get(), w.layerDsv.Get(), game, gameDsv)) {
            savedRtv = gameRtv[0];     // the references move to the run
            savedDsv = gameDsv;
            heldRefs = true;
        } else {
            if (gameRtv[0]) gameRtv[0]->Release();
            if (gameDsv) gameDsv->Release();
        }
    }
    void endNow() {
        if (!core.end()) return;
        ID3D11RenderTargetView* r = savedRtv;
        w.ctx->OMSetRenderTargets(1, &r, savedDsv);
        ++w.realBinds;
        release();
    }
    void release() {
        if (savedRtv) savedRtv->Release();
        if (savedDsv) savedDsv->Release();
        savedRtv = nullptr;
        savedDsv = nullptr;
        heldRefs = false;
    }
};

void runWarpTest() {
    Warp w;
    D3D_FEATURE_LEVEL fl{};
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &w.dev, &fl, &w.ctx), "D3D11CreateDevice (WARP)");
    w.gameTex = makeTex(w, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_RENDER_TARGET);
    w.layerTex = makeTex(w, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    w.gameDepth = makeTex(w, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D11_BIND_DEPTH_STENCIL);
    w.layerDepth = makeTex(w, edvr::mapui::kDepthFormat, D3D11_BIND_DEPTH_STENCIL);
    hr(w.dev->CreateRenderTargetView(w.gameTex.Get(), nullptr, &w.gameRtv), "game rtv");
    hr(w.dev->CreateRenderTargetView(w.layerTex.Get(), nullptr, &w.layerRtv), "layer rtv");
    hr(w.dev->CreateDepthStencilView(w.gameDepth.Get(), nullptr, &w.gameDsv), "game dsv");
    hr(w.dev->CreateDepthStencilView(w.layerDepth.Get(), nullptr, &w.layerDsv), "layer dsv");
    const std::vector<uint8_t> vs = compile(kVs, "vs_5_0"), ps = compile(kPs, "ps_5_0");
    hr(w.dev->CreateVertexShader(vs.data(), vs.size(), nullptr, &w.vs), "vs");
    hr(w.dev->CreatePixelShader(ps.data(), ps.size(), nullptr, &w.ps), "ps");
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 48;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr(w.dev->CreateBuffer(&bd, nullptr, &w.cb), "cb");
    const D3D11_DEPTH_STENCIL_DESC testD = edvr::mapui::depthStateDesc(false), writeD = edvr::mapui::depthStateDesc(true);
    hr(w.dev->CreateDepthStencilState(&testD, &w.testState), "test state");
    hr(w.dev->CreateDepthStencilState(&writeD, &w.writeState), "write state");
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr(w.dev->CreateRasterizerState(&rd, &w.raster), "raster");
    const D3D11_RASTERIZER_DESC bias = edvr::mapui::depthRasterDesc(rd);
    hr(w.dev->CreateRasterizerState(&bias, &w.depthRaster), "depth raster");
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].BlendEnable = FALSE;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr(w.dev->CreateBlendState(&bl, &w.colourBlend), "blend");
    const D3D11_BLEND_DESC probe = edvr::mapui::probeBlendDesc();
    hr(w.dev->CreateBlendState(&probe, &w.writeMask0), "probe blend");

    // The game's frame: its targets cleared and bound, the shaders and constants it would have bound, a viewport.
    const float gameClear[4] = {0.25f, 0.5f, 0.125f, 1.0f}, layerClear[4] = {0, 0, 0, 0};
    w.ctx->ClearRenderTargetView(w.gameRtv.Get(), gameClear);
    w.ctx->ClearRenderTargetView(w.layerRtv.Get(), layerClear);
    w.ctx->ClearDepthStencilView(w.layerDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);   // reversed depth: the clear is 0
    w.ctx->ClearDepthStencilView(w.gameDsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
    ID3D11RenderTargetView* g = w.gameRtv.Get();
    w.ctx->OMSetRenderTargets(1, &g, w.gameDsv.Get());
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1};
    w.ctx->RSSetViewports(1, &vp);
    w.ctx->VSSetShader(w.vs.Get(), nullptr, 0);
    w.ctx->PSSetShader(w.ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = w.cb.Get();
    w.ctx->VSSetConstantBuffers(0, 1, &cb);
    w.ctx->PSSetConstantBuffers(0, 1, &cb);
    w.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    w.ctx->OMSetDepthStencilState(nullptr, 0);
    const std::vector<uint8_t> gameBefore = readback(w, w.gameTex.Get());

    const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1};
    w.realBinds = 0;
    Run run(w);
    // near sprite (left, z 0.8), then a far sprite (z 0.4) over its right half and beyond, then a quad in the near sprite's plane (z 0.8) over its lower half; each a colour issue then a depth issue
    const float nearRect[4] = {-0.75f, -0.75f, 0.25f, 0.75f}, farRect[4] = {-0.25f, -0.5f, 0.75f, 0.5f}, planeRect[4] = {-0.5f, -0.75f, 0.0f, -0.25f};
    run.issue(false, nearRect[0], nearRect[1], nearRect[2], nearRect[3], 0.8f, red);
    run.issue(true, nearRect[0], nearRect[1], nearRect[2], nearRect[3], 0.8f, red);
    run.issue(false, farRect[0], farRect[1], farRect[2], farRect[3], 0.4f, green);
    run.issue(true, farRect[0], farRect[1], farRect[2], farRect[3], 0.4f, green);
    run.issue(false, planeRect[0], planeRect[1], planeRect[2], planeRect[3], 0.8f, blue);
    run.issue(true, planeRect[0], planeRect[1], planeRect[2], planeRect[3], 0.8f, blue);
    // while the run is held the context has the layer's targets and the game's frame is untouched
    ID3D11RenderTargetView* held[8] = {};
    ID3D11DepthStencilView* heldDsv = nullptr;
    w.ctx->OMGetRenderTargets(8, held, &heldDsv);
    check(held[0] == w.layerRtv.Get() && heldDsv == w.layerDsv.Get() && run.core.held, "R4: while a run is held the context has the layer's target and the eye's D16");
    if (held[0]) held[0]->Release();
    if (heldDsv) heldDsv->Release();
    run.endNow();
    ID3D11RenderTargetView* back[8] = {};
    ID3D11DepthStencilView* backDsv = nullptr;
    w.ctx->OMGetRenderTargets(8, back, &backDsv);
    check(back[0] == w.gameRtv.Get() && backDsv == w.gameDsv.Get() && !back[1], "R4: after the end the context's bound targets are the game's, by OMGetRenderTargets");
    for (auto*& r : back) if (r) { r->Release(); r = nullptr; }
    if (backDsv) backDsv->Release();
    char label[220];
    std::snprintf(label, sizeof label, "R4: six issues made %u real target binds (the run's begin and its end)", w.realBinds);
    std::printf("%s\n", label);
    check(w.realBinds == 2 && run.core.binds == 2 && run.core.started == 1 && run.core.ended == 1, label);
    check(!run.heldRefs && !run.savedRtv && !run.savedDsv, "R4: no reference to the game's targets is held after the end");
    check(readback(w, w.gameTex.Get()) == gameBefore, "R4: the game's R11G11B10 target is untouched by the run");

    // the layer: the near sprite is red where the far sprite is behind it, the far sprite is green outside the near one, and the same-plane quad is drawn whole over the near one
    const std::vector<uint8_t> px = readback(w, w.layerTex.Get());
    auto at = [&](float nx, float ny, int c) { const UINT x = static_cast<UINT>((nx * 0.5f + 0.5f) * kSize), y = static_cast<UINT>((1.0f - (ny * 0.5f + 0.5f)) * kSize); return px[(y * kSize + x) * 4 + c]; };
    check(at(-0.6f, 0.5f, 0) == 255 && at(-0.6f, 0.5f, 1) == 0, "R4: the near sprite is drawn red in the layer");
    check(at(0.1f, 0.3f, 0) == 255 && at(0.1f, 0.3f, 1) == 0, "R4: the far sprite is rejected by the near sprite's depth where they overlap (red, not green)");
    check(at(0.5f, 0.3f, 1) == 255 && at(0.5f, 0.3f, 0) == 0, "R4: the far sprite is drawn green outside the near one");
    check(at(-0.4f, -0.5f, 2) == 255 && at(-0.4f, -0.5f, 0) == 0, "R4: a second quad in the same plane is drawn whole (the bias leaves it equal or nearer)");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string mode = argv[1];
    if (mode == "--dry-run") {
        std::puts("map_run_test: R1-R4, the run's state machine and a WARP run; reads nothing, writes nothing");
        return 0;
    }
    if (mode != "--self-test") return 2;
    runCoreTests();
    runWarpTest();
    std::printf("map_run_test: %u checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
