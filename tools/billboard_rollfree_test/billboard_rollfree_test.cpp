// The roll-free scene block (src\d3d11\billboard_rollfree.h) on a WARP device with the D3D debug layer.
//   R1 the basis: roll-free for any roll, the game's lengths and handedness kept, undefined near world up refused
//   R2 the copy route is legal for a constant-buffer-bound resource (no debug-layer message of warning or worse),
//      rows 277/278 are the roll-free ones and every other byte of the 5376 is the game's
//   R3 a real draw: the vertex stage reads the roll-free rows through slot 1, the PIXEL stage still reads the game's
//      block at its slot 1, and on the unchanged path (the old code) the roll is present in the bound rows
//   R4 state is fully restored after the draw (VS and PS constant buffers, shaders, targets)
//   R5 one copy per write of the block however many draws follow; a new write copies again
//   R6 stock (disabled) does nothing; a block of another size is refused; the table retires
//   R7 several blocks (each eye, any view) each get their own copy
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace edvr { thread_local bool g_flatComputeInternal = false; }
#include "billboard_rollfree.h"

using Microsoft::WRL::ComPtr;
using namespace edvr;

static int failures = 0;
static unsigned checks = 0;
static void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { std::printf("FAIL %s\n", label); ++failures; }
}
static void hr(HRESULT h, const char* what) { if (FAILED(h)) { std::printf("FAIL D3D %s 0x%08lx\n", what, static_cast<unsigned long>(h)); std::exit(1); } }

// ---- the math -----------------------------------------------------------------------------------------------------
struct V3 { float x, y, z; };
static V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static V3 norm(V3 a) { const float l = std::sqrt(dot(a, a)); return a * (1.0f / l); }

// A camera with forward f, rolled by `roll` radians about f, handedness s (+1: [277] = cross([278],[279])).
struct Camera { float r277[4], r278[4], r279[4]; };
static Camera camera(V3 f, float roll, float s = 1.0f, float lenR = 1.0f, float lenU = 1.0f) {
    f = norm(f);
    const V3 right0 = norm(cross({0, 1, 0}, f));
    const V3 up0 = cross(f, right0);
    const V3 right = right0 * std::cos(roll) + up0 * std::sin(roll);
    const V3 up = right0 * -std::sin(roll) + up0 * std::cos(roll);
    Camera c{};
    c.r277[0] = s * right.x * lenR; c.r277[1] = s * right.y * lenR; c.r277[2] = s * right.z * lenR; c.r277[3] = 0.5f;
    c.r278[0] = up.x * lenU; c.r278[1] = up.y * lenU; c.r278[2] = up.z * lenU; c.r278[3] = 0.25f;
    c.r279[0] = f.x; c.r279[1] = f.y; c.r279[2] = f.z; c.r279[3] = 0.125f;
    return c;
}

static void r1Math() {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
    for (int i = 0; i < 400; ++i) {
        V3 f{uni(rng), uni(rng) * 0.9f, uni(rng)};
        if (std::fabs(f.x) + std::fabs(f.z) < 0.1f) continue;
        const float roll = uni(rng) * 3.14159f;
        const float s = (i & 1) ? -1.0f : 1.0f;
        const Camera c = camera(f, roll, s, 1.0f + 0.5f * (i % 3), 0.75f + 0.25f * (i % 4));
        float out[8];
        check(rollfree::basis(c.r277, c.r278, c.r279, out), "R1.defined");
        const V3 R{out[0], out[1], out[2]}, U{out[4], out[5], out[6]}, F{c.r279[0], c.r279[1], c.r279[2]};
        const float lr = std::sqrt(dot(R, R)), lu = std::sqrt(dot(U, U));
        const float lr0 = std::sqrt(c.r277[0] * c.r277[0] + c.r277[1] * c.r277[1] + c.r277[2] * c.r277[2]);
        const float lu0 = std::sqrt(c.r278[0] * c.r278[0] + c.r278[1] * c.r278[1] + c.r278[2] * c.r278[2]);
        check(std::fabs(dot(R, V3{0, 1, 0})) < 1e-5f * lr, "R1.right-is-level");
        check(dot(U, V3{0, 1, 0}) > 0.0f, "R1.up-leans-to-world-up");
        check(std::fabs(lr - lr0) < 1e-4f && std::fabs(lu - lu0) < 1e-4f, "R1.lengths-are-the-games");
        check(std::fabs(dot(R, F)) < 1e-4f * lr && std::fabs(dot(U, F)) < 1e-4f * lu && std::fabs(dot(R, U)) < 1e-4f * lr * lu,
              "R1.orthogonal-to-forward-and-each-other");
        const V3 c3 = cross(U, F);
        check((dot(R, c3) > 0.0f) == (s > 0.0f), "R1.handedness-is-the-games");
        check(out[3] == 0.5f && out[7] == 0.25f, "R1.w-components-kept");
        // The same forward at any other roll gives the same rows: the roll is gone.
        float out2[8];
        const Camera c2 = camera(f, roll + 1.234f, s, 1.0f + 0.5f * (i % 3), 0.75f + 0.25f * (i % 4));
        rollfree::basis(c2.r277, c2.r278, c2.r279, out2);
        bool same = true;
        for (int k = 0; k < 8; ++k) same &= std::fabs(out[k] - out2[k]) < 1e-4f;
        check(same, "R1.independent-of-roll");
    }
    float out[8];
    const Camera up = camera({0, 1, 0.001f}, 0.3f);
    check(!rollfree::basis(up.r277, up.r278, up.r279, out), "R1.straight-up-refused");
    Camera bad = camera({0.2f, 0.1f, 0.9f}, 0.0f);
    bad.r279[0] *= 3.0f;
    check(!rollfree::basis(bad.r277, bad.r278, bad.r279, out), "R1.non-unit-forward-refused");
    Camera zero = camera({0.2f, 0.1f, 0.9f}, 0.0f);
    std::memset(zero.r277, 0, 12);
    check(!rollfree::basis(zero.r277, zero.r278, zero.r279, out), "R1.zero-right-refused");
}

// ---- the device ---------------------------------------------------------------------------------------------------
struct Rig {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11InfoQueue> queue;
    bool debugLayer = false;
    Rig() {
        D3D_FEATURE_LEVEL fl;
        HRESULT h = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION,
                                      &dev, &fl, &ctx);
        if (h == DXGI_ERROR_SDK_COMPONENT_MISSING)
            h = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
        else
            debugLayer = SUCCEEDED(h);
        hr(h, "CreateDevice");
        dev.As(&queue);
        if (queue) queue->ClearStoredMessages();
    }
    // Every message of warning or worse since the last call.
    unsigned problems() {
        unsigned n = 0;
        if (!queue) return 0;
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T sz = 0;
            queue->GetMessage(i, nullptr, &sz);
            std::vector<char> b(sz);
            auto* m = reinterpret_cast<D3D11_MESSAGE*>(b.data());
            queue->GetMessage(i, m, &sz);
            if (m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) { std::printf("debug layer: %s\n", m->pDescription); ++n; }
        }
        queue->ClearStoredMessages();
        return n;
    }
    ComPtr<ID3D11Buffer> dynamicBlock() {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = rollfree::kBlockBytes;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> b;
        hr(dev->CreateBuffer(&d, nullptr, &b), "CreateBuffer dynamic block");
        return b;
    }
    // The game's write: Map, fill, the Unmap tee's capture, Unmap. `bytes` is the whole block.
    void gameWrite(ID3D11Buffer* block, rollfree::Blocks& blocks, const std::vector<uint8_t>& bytes) {
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(ctx->Map(block, 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map");
        std::memcpy(m.pData, bytes.data(), bytes.size());
        blocks.capture(block, true, m.pData, static_cast<uint32_t>(bytes.size()),
                       [](void* d, const void* s, size_t n) { std::memcpy(d, s, n); });
        ctx->Unmap(block, 0);
    }
    std::vector<uint8_t> readBuffer(ID3D11Buffer* src, uint32_t bytes) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = bytes;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> st;
        hr(dev->CreateBuffer(&d, nullptr, &st), "staging");
        ctx->CopyResource(st.Get(), src);
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m), "Map read");
        std::vector<uint8_t> out(static_cast<uint8_t*>(m.pData), static_cast<uint8_t*>(m.pData) + bytes);
        ctx->Unmap(st.Get(), 0);
        return out;
    }
};

static std::vector<uint8_t> blockBytes(const Camera& c, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(rollfree::kBlockBytes);
    for (auto& b : v) b = static_cast<uint8_t>(rng());
    std::memcpy(v.data() + 277 * 16, c.r277, 16);
    std::memcpy(v.data() + 278 * 16, c.r278, 16);
    std::memcpy(v.data() + 279 * 16, c.r279, 16);
    return v;
}
static V3 rowOf(const std::vector<uint8_t>& v, int row) {
    float f[4];
    std::memcpy(f, v.data() + row * 16, 16);
    return {f[0], f[1], f[2]};
}

static void r2Copy(Rig& rig) {
    rollfree::Blocks blocks;
    auto game = rig.dynamicBlock();
    check(blocks.learn(game.Get()) == 0, "R2.learned");
    const Camera cam = camera({0.4f, 0.25f, 0.88f}, 0.6f);
    const auto bytes = blockBytes(cam, 11);
    rig.gameWrite(game.Get(), blocks, bytes);
    check(blocks.needsCopy(0), "R2.a-write-wants-a-copy");
    rig.problems();
    ID3D11Buffer* ours = blocks.prepare(rig.ctx.Get(), 0, game.Get());
    check(ours != nullptr, "R2.copy-made");
    check(rig.problems() == 0, "R2.debug-layer-clean-for-the-copy-route");
    const auto got = rig.readBuffer(ours, rollfree::kBlockBytes);
    check(rig.problems() == 0, "R2.debug-layer-clean-for-the-readback");
    bool same = true;
    for (uint32_t i = 0; i < rollfree::kBlockBytes; ++i)
        if ((i < 277 * 16 || i >= 279 * 16) && got[i] != bytes[i]) same = false;
    check(same, "R2.every-byte-outside-rows-277-278-is-the-games");
    float want[8];
    rollfree::basis(cam.r277, cam.r278, cam.r279, want);
    float gotRows[8];
    std::memcpy(gotRows, got.data() + 277 * 16, 32);
    bool match = true;
    for (int k = 0; k < 8; ++k) match &= gotRows[k] == want[k];
    check(match, "R2.rows-277-278-are-the-roll-free-ones");
    check(std::memcmp(got.data() + 279 * 16, bytes.data() + 279 * 16, 16) == 0, "R2.forward-untouched");
    check(rowOf(got, 277).y != rowOf(bytes, 277).y || rowOf(got, 278).x != rowOf(bytes, 278).x, "R2.the-rows-did-change");
}

static ComPtr<ID3DBlob> compile(const char* src, const char* target) {
    ComPtr<ID3DBlob> code, err;
    const HRESULT h = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", target, 0, 0, &code, &err);
    if (FAILED(h)) { std::printf("shader: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); std::exit(1); }
    return code;
}

struct Draw {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Texture2D> rt[3];
    ComPtr<ID3D11RenderTargetView> rtv[3];
    ComPtr<ID3D11Buffer> psBlock;      // the pixel stage's own slot-1 buffer
    ComPtr<ID3D11RasterizerState> raster;
    explicit Draw(Rig& r) {
        const char* vsSrc =
            "cbuffer Scene : register(b1) { float4 c[280]; };\n"
            "struct O { float4 p : SV_Position; float3 up : TEXCOORD0; float3 right : TEXCOORD1; };\n"
            "O main(uint id : SV_VertexID) { O o; float2 xy = float2((id << 1) & 2, id & 2); o.p = float4(xy * 2 - 1, 0, 1);\n"
            "  o.up = c[278].xyz; o.right = c[277].xyz; return o; }";
        const char* psSrc =
            "cbuffer Scene : register(b1) { float4 c[280]; };\n"
            "struct O { float4 a : SV_Target0; float4 b : SV_Target1; float4 d : SV_Target2; };\n"
            "O main(float4 p : SV_Position, float3 up : TEXCOORD0, float3 right : TEXCOORD1) { O o;\n"
            "  o.a = float4(up, 1); o.b = float4(c[278].xyz, 1); o.d = float4(right, 1); return o; }";
        auto vc = compile(vsSrc, "vs_5_0");
        auto pc = compile(psSrc, "ps_5_0");
        hr(r.dev->CreateVertexShader(vc->GetBufferPointer(), vc->GetBufferSize(), nullptr, &vs), "vs");
        hr(r.dev->CreatePixelShader(pc->GetBufferPointer(), pc->GetBufferSize(), nullptr, &ps), "ps");
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 4;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        for (int i = 0; i < 3; ++i) {
            hr(r.dev->CreateTexture2D(&td, nullptr, &rt[i]), "rt");
            hr(r.dev->CreateRenderTargetView(rt[i].Get(), nullptr, &rtv[i]), "rtv");
        }
        psBlock = r.dynamicBlock();
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        hr(r.dev->CreateRasterizerState(&rd, &raster), "raster");
    }
    void setup(Rig& r, ID3D11Buffer* vsBlock) {
        ID3D11RenderTargetView* v[3] = {rtv[0].Get(), rtv[1].Get(), rtv[2].Get()};
        r.ctx->OMSetRenderTargets(3, v, nullptr);
        D3D11_VIEWPORT vp{0, 0, 4, 4, 0, 1};
        r.ctx->RSSetViewports(1, &vp);
        r.ctx->RSSetState(raster.Get());
        r.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        r.ctx->VSSetShader(vs.Get(), nullptr, 0);
        r.ctx->PSSetShader(ps.Get(), nullptr, 0);
        r.ctx->VSSetConstantBuffers(1, 1, &vsBlock);
        ID3D11Buffer* pb = psBlock.Get();
        r.ctx->PSSetConstantBuffers(1, 1, &pb);
    }
    V3 pixel(Rig& r, int target) {
        D3D11_TEXTURE2D_DESC td{};
        rt[target]->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st;
        hr(r.dev->CreateTexture2D(&td, nullptr, &st), "stage");
        r.ctx->CopyResource(st.Get(), rt[target].Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(r.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m), "Map px");
        float f[4];
        std::memcpy(f, static_cast<uint8_t*>(m.pData) + 2 * m.RowPitch + 2 * 16, 16);
        r.ctx->Unmap(st.Get(), 0);
        return {f[0], f[1], f[2]};
    }
};

static void r3r4Draw(Rig& rig) {
    rollfree::Blocks blocks;
    Draw draw(rig);
    auto game = rig.dynamicBlock();
    blocks.learn(game.Get());
    const Camera cam = camera({0.4f, 0.25f, 0.88f}, 0.6f);   // a head rolled 0.6 rad
    const auto bytes = blockBytes(cam, 21);
    rig.gameWrite(game.Get(), blocks, bytes);
    // The pixel stage's block: the game's bytes, in its own buffer.
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(rig.ctx->Map(draw.psBlock.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map ps block");
        std::memcpy(m.pData, bytes.data(), bytes.size());
        rig.ctx->Unmap(draw.psBlock.Get(), 0);
    }
    draw.setup(rig, game.Get());
    rig.problems();

    // The OLD code: the draw as the game made it. The roll is in the bound rows (the fail-before).
    rig.ctx->Draw(3, 0);
    const V3 oldUp = draw.pixel(rig, 0), oldRight = draw.pixel(rig, 2);
    check(std::fabs(oldRight.y) > 0.1f, "R3.fail-before-the-roll-is-in-the-rows-the-vertex-stage-reads");
    check(std::fabs(oldUp.x * oldUp.x + oldUp.y * oldUp.y + oldUp.z * oldUp.z - 1.0f) < 1e-4f, "R3.fixture-up-is-unit");

    // Snapshot the state, bind the copy, draw, restore, and compare.
    ID3D11Buffer *vs1Before = nullptr, *ps1Before = nullptr;
    rig.ctx->VSGetConstantBuffers(1, 1, &vs1Before);
    rig.ctx->PSGetConstantBuffers(1, 1, &ps1Before);
    ID3D11Buffer* ours = blocks.prepare(rig.ctx.Get(), 0, game.Get());
    check(ours && blocks.bind(rig.ctx.Get(), ours), "R3.bound");
    ID3D11Buffer *vs1During = nullptr, *ps1During = nullptr;
    rig.ctx->VSGetConstantBuffers(1, 1, &vs1During);
    rig.ctx->PSGetConstantBuffers(1, 1, &ps1During);
    check(vs1During == ours, "R3.vertex-slot-1-holds-the-copy");
    check(ps1During == ps1Before, "R3.pixel-slot-1-untouched");
    rig.ctx->Draw(3, 0);
    blocks.restore(rig.ctx.Get());
    const V3 newUp = draw.pixel(rig, 0), newRight = draw.pixel(rig, 2), psUp = draw.pixel(rig, 1);
    check(std::fabs(newRight.y) < 1e-5f, "R3.the-vertex-stage-sees-a-level-right");
    check(newUp.y > 0.9f, "R3.and-an-upright-up");
    check(std::fabs(psUp.x - oldUp.x) < 1e-6f && std::fabs(psUp.y - oldUp.y) < 1e-6f && std::fabs(psUp.z - oldUp.z) < 1e-6f,
          "R3.the-pixel-stage-still-reads-the-games-rows");

    // R4: everything is as it was.
    ID3D11Buffer *vs1After = nullptr, *ps1After = nullptr;
    rig.ctx->VSGetConstantBuffers(1, 1, &vs1After);
    rig.ctx->PSGetConstantBuffers(1, 1, &ps1After);
    check(vs1After == vs1Before && vs1After == game.Get(), "R4.vertex-slot-1-restored-to-the-games-block");
    check(ps1After == ps1Before, "R4.pixel-slot-1-restored");
    ID3D11VertexShader* vsAfter = nullptr;
    ID3D11PixelShader* psAfter = nullptr;
    rig.ctx->VSGetShader(&vsAfter, nullptr, nullptr);
    rig.ctx->PSGetShader(&psAfter, nullptr, nullptr);
    check(vsAfter == draw.vs.Get() && psAfter == draw.ps.Get(), "R4.shaders-untouched");
    ID3D11RenderTargetView* rtvs[3] = {};
    rig.ctx->OMGetRenderTargets(3, rtvs, nullptr);
    check(rtvs[0] == draw.rtv[0].Get() && rtvs[1] == draw.rtv[1].Get() && rtvs[2] == draw.rtv[2].Get(), "R4.targets-untouched");
    check(!blocks.bound(), "R4.not-left-bound");
    // Another slot the draw never owned.
    ID3D11Buffer* slot0 = nullptr;
    rig.ctx->VSGetConstantBuffers(0, 1, &slot0);
    check(slot0 == nullptr, "R4.vertex-slot-0-still-empty");
    for (auto* p : {vs1Before, ps1Before, vs1During, ps1During, vs1After, ps1After}) if (p) p->Release();
    if (vsAfter) vsAfter->Release();
    if (psAfter) psAfter->Release();
    for (auto* p : rtvs) if (p) p->Release();
    check(rig.problems() == 0, "R3.debug-layer-clean-for-the-whole-draw");

    // A second restore, and a restore with nothing bound, are harmless.
    blocks.restore(rig.ctx.Get());
    check(rig.problems() == 0, "R4.a-spare-restore-is-a-no-op");
}

static void r5Lazy(Rig& rig) {
    rollfree::Blocks blocks;
    auto game = rig.dynamicBlock();
    blocks.learn(game.Get());
    const auto bytes = blockBytes(camera({0.4f, 0.25f, 0.88f}, 0.6f), 31);
    rig.gameWrite(game.Get(), blocks, bytes);
    for (int i = 0; i < 25; ++i) {
        ID3D11Buffer* ours = blocks.prepare(rig.ctx.Get(), 0, game.Get());
        check(ours != nullptr, "R5.each-draw-gets-the-copy");
    }
    check(blocks.counters.copies == 1 && blocks.counters.binds == 25, "R5.one-copy-for-25-draws");
    rig.gameWrite(game.Get(), blocks, blockBytes(camera({0.4f, 0.25f, 0.88f}, -0.9f), 32));
    for (int i = 0; i < 7; ++i) blocks.prepare(rig.ctx.Get(), 0, game.Get());
    check(blocks.counters.copies == 2 && blocks.counters.binds == 32, "R5.a-new-write-copies-again-once");
    // Ten writes and no draw: no copy at all.
    for (int i = 0; i < 10; ++i) rig.gameWrite(game.Get(), blocks, blockBytes(camera({0.4f, 0.25f, 0.88f}, 0.1f * i), 40 + i));
    check(blocks.counters.copies == 2, "R5.writes-without-draws-copy-nothing");
    // Forward within a degree of world up: no rebind, no copy.
    rig.gameWrite(game.Get(), blocks, blockBytes(camera({0.001f, 1.0f, 0.001f}, 0.5f), 60));
    check(blocks.prepare(rig.ctx.Get(), 0, game.Get()) == nullptr && blocks.counters.noBasis == 1 && blocks.counters.copies == 2,
          "R5.straight-up-keeps-the-games-block-and-copies-nothing");
    check(rig.problems() == 0, "R5.debug-layer-clean");
}

static void r6r7Misc(Rig& rig) {
    {
        rollfree::Blocks blocks;
        blocks.enabled = false;
        auto game = rig.dynamicBlock();
        check(blocks.learn(game.Get()) == -1, "R6.stock-learns-nothing");
        check(blocks.prepare(rig.ctx.Get(), 0, game.Get()) == nullptr && blocks.counters.copies == 0 && blocks.counters.binds == 0,
              "R6.stock-does-nothing");
        check(blocks.watched()[0] == nullptr, "R6.stock-watches-nothing");
    }
    {
        rollfree::Blocks blocks;
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = 208;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> tiny;
        hr(rig.dev->CreateBuffer(&d, nullptr, &tiny), "tiny");
        check(blocks.learn(tiny.Get()) == -1, "R6.another-size-is-refused");
        D3D11_BUFFER_DESC v{};
        v.ByteWidth = rollfree::kBlockBytes;
        v.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        ComPtr<ID3D11Buffer> vb;
        hr(rig.dev->CreateBuffer(&v, nullptr, &vb), "vb");
        check(blocks.learn(vb.Get()) == -1, "R6.not-a-constant-buffer-is-refused");
        auto game = rig.dynamicBlock();
        check(blocks.learn(game.Get()) == 0 && blocks.watched()[0] == game.Get(), "R6.learned-and-published");
        blocks.clear();
        check(blocks.watched()[0] == nullptr && blocks.find(game.Get()) == -1, "R6.retired-forgets-the-identity");
        check(blocks.prepare(rig.ctx.Get(), 0, game.Get()) == nullptr, "R6.retired-binds-nothing");
        check(blocks.learn(game.Get()) == 0, "R6.re-learned");
    }
    {
        // R7: four blocks, each its own camera, each its own copy; a fifth replaces the oldest.
        rollfree::Blocks blocks;
        std::vector<ComPtr<ID3D11Buffer>> game;
        for (int i = 0; i < 4; ++i) {
            game.push_back(rig.dynamicBlock());
            check(blocks.learn(game.back().Get()) == i, "R7.each-block-gets-a-slot");
            rig.gameWrite(game.back().Get(), blocks, blockBytes(camera({0.4f, 0.25f, 0.88f}, 0.3f * (i + 1)), 70 + i));
        }
        for (int i = 0; i < 4; ++i) {
            ID3D11Buffer* ours = blocks.prepare(rig.ctx.Get(), i, game[i].Get());
            check(ours != nullptr, "R7.each-block-copied");
            for (int j = 0; j < i; ++j) check(ours != blocks.at(j)->copy.Get(), "R7.copies-are-distinct");
        }
        check(blocks.counters.copies == 4, "R7.four-copies");
        // A draw whose slot-1 buffer is not the slot's block gets nothing.
        check(blocks.prepare(rig.ctx.Get(), 0, game[1].Get()) == nullptr, "R7.mismatched-block-is-refused");
        auto fifth = rig.dynamicBlock();
        check(blocks.learn(fifth.Get()) == 0, "R7.the-fifth-replaces-the-oldest");
        check(rig.problems() == 0, "R7.debug-layer-clean");
    }
}

static std::string slurp(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string s;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}
static int countOf(const std::string& s, const std::string& part) {
    int n = 0;
    for (size_t at = 0; (at = s.find(part, at)) != std::string::npos; at += part.size()) ++n;
    return n;
}

// Source pins: the rebind is wired where it runs, and stock reaches none of it.
static void r8Pins(const std::string& root) {
    const std::string pf = slurp(root + "\\src\\d3d11\\particle_fix.cpp");
    const std::string vs = slurp(root + "\\src\\d3d11\\vscreen.cpp");
    check(!pf.empty() && !vs.empty(), "R8.sources-readable");
    check(countOf(vs, "particleRollFreeBegin(self)") == 1 && countOf(vs, "particleRollFreeEnd(ctx)") == 1,
          "R8.begin-and-end-wired-once-in-forwardWithVerdict");
    check(countOf(vs, "particleRollFreeClear();") == 1, "R8.pending-cleared-at-the-top-of-the-ladder");
    check(pf.find("if (detail::g_particleMode != Mode::kSteady || !ctx) return false;") != std::string::npos,
          "R8.stock-returns-before-matching");
    check(countOf(pf, "g_blocks.enabled = ") >= 2 && countOf(pf, "g_blocks.clear()") >= 2, "R8.mode-changes-clear-and-gate-the-table");
    check(countOf(pf, "g_blocks.capture(") == 1, "R8.capture-goes-through-the-table");
    check(countOf(vs, "particleBlockWatched(res)") == 1, "R8.map-tee-asks-the-table");
    check(pf.find("psSetConstantBuffers") == std::string::npos && pf.find("PSSetConstantBuffers") == std::string::npos,
          "R8.the-pixel-stage-is-never-set-by-the-fix");
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string mode = argv[1];
    if (mode == "--dry-run") {
        std::puts("billboard_rollfree_test: R1-R8, WARP with the debug layer; no files");
        return 0;
    }
    if (mode != "--self-test") return 2;
    r1Math();
    Rig rig;
    std::printf("billboard_rollfree_test: debug layer %s\n", rig.debugLayer ? "ON" : "not installed (messages not checked)");
    r2Copy(rig);
    r3r4Draw(rig);
    r5Lazy(rig);
    r6r7Misc(rig);
    if (argc > 2) r8Pins(argv[2]);
    if (failures) { std::printf("billboard_rollfree_test: %d FAILED of %u\n", failures, checks); return 1; }
    std::printf("billboard_rollfree_test: PASS R1-R8 (%u checks)\n", checks);
    return 0;
}
