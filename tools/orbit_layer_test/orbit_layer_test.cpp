// The orbit lines in the HDR layer, on the real dump (docs/ui-layer-2026-09-23.md, "2026-10-07: orbit lines, supercruise bars and space dust in
// the layer"). The game's own vertex shader (vs C7FA0C0F5DD49180, tools/orbital_width_test/fixtures) through the PRODUCTION patched copy, cache,
// constants and binding (orbital_width_patch.h), the PRODUCTION depth-stencil seed (ui_layer_seed.h) and the PRODUCTION map and sizes
// (ui_layer_math.h, ui_sizing_math.h), on WARP, over one draw of a field dump and the crop of the eye's own SceneZ beneath it:
//
//   tools/orbit_layer_test/fixtures/orbit_050020_29722_312.bin  (EDVROL01, made by tools/layer_fixtures.py orbit from the Frontier install's
//   edvr_logs/pool/drawstate_050020.bin and aux_050020_29722.bin, frame 29722 ordinal 312, and eyes/eye_050020_SceneZ.bin): the draw's VS b1
//   (333 rows), vertex buffer 0 (8194 strip vertices), vertex buffer 1 (6 instances: the orange orbit w 2.0, five cyan rings w 1.5) and the
//   826 x 630 crop at (1190, 700) of the 2016 x 1949 eye's depth and stencil: 94% sky (depth 0, stencil 5), the cockpit and the planet's near
//   edge (stencil 0x10, 0x90, 0x95, depth 0.008..0.015), a far body (0x15, depth ~1e-7) and 6659 stencil-5 pixels with depth 0.003..0.006 at
//   the bottom edge. The crop times 2.5 is whole (2065 x 1575), so the layer's own 2.5 x map (5040 x 4873 for a 2016 x 1949 render at
//   fix.ui_quality 125: f = 2016 / (4032 x 1.25) = 0.4) runs on it with no resampling of the geometry.
//
// WHAT RUNS. The layer's viewport is uiLayerMapViewport of the game's, shifted to the crop. The strip is drawn by the production patched copy
// with b13 = 2f from Constants::get (f from uiPanelFactor), per instance, into an R32G32 float target (lit, depth) by an HLSL stand-in for the
// game's pixel shader 6EEF165A350DA30F (its colour alpha test; the alpha ramp stays 1: the footprint is the geometry's), against the layer's
// depth-stencil that the production Seeder made from the game's crop, with the game's own state (census frame 1 row #154: depth GEQUAL no write,
// stencil EQUAL ref 1 under read mask 0x81, nothing written). Each instance is drawn twice, with the tests (L) and without any (U), and
// the rig asserts:
//
//   * the strip is 2w LAYER pixels wide (4.000 for the orbit, 3.000 for a ring: the stream-out of the patched shader read at the layer's size),
//     the width the take is for -- and f x (layer / render) is 1;
//   * the seed is the game's crop at the layer's size, texel for texel (the nearest sample, either side of an exact boundary);
//   * the tests are the oracle's, pixel for pixel: L = U where the seeded stencil passes (bit 0 set, bit 7 clear) and the line is not behind the seeded
//     depth, nothing else; so there are 0 lit pixels where stencil & 0x81 != 1 and 0 where the scene is nearer, and the data is not vacuous (U lights
//     thousands of pixels on the cockpit and on stencil-5 scene, L none of them);
//   * a lit fraction against the 2016 reference: of the pixels the game's own patched draw at the render size (the scene's f = 0.4 line, the
//     game's depth and stencil) lights over stencil 5, at least 98% have a lit layer pixel in their 2.5 x 2.5 block;
//   * a line behind a nearer scene is rejected and one in front accepted: the same draw over a constant depth below the line's own (all accepted), above it
//     (none), and split (the nearer half rejects);
//
// and the mutants -- the factor not applied (the strip is 10 px), the factor applied twice, the seed skipped (no depth-stencil bound: the line lights
// the cockpit), the stencil read mask wrong (0x01, and 0xFF), the depth test off, the seed taken from a texel away, the seed's stencil mask without
// bit 7 -- are each caught by the very comparator that passes the real thing. --dry-run writes nothing; --hardware-self-test runs the lot on the machine's own
// adapter (not a gate).
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "../../src/common/log.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/orbital_width_patch.h"
#include "../../src/d3d11/ui_layer_math.h"
#include "../../src/d3d11/ui_layer_seed.h"
#include "../../src/d3d11/ui_sizing_math.h"

using Microsoft::WRL::ComPtr;
using namespace edvr::orbital_width;

namespace edvr {
Log& Log::get() {
    static Log log;
    return log;
}
Log::~Log() {}
void Log::note(const char*, ...) {}
void breadcrumb(const char*) {}
}  // namespace edvr

static unsigned g_checks = 0;
static void check(bool ok, const char* why) {
    ++g_checks;
    if (!ok) throw std::runtime_error(why);
}
static void ck(HRESULT h, const char* what) {
    if (FAILED(h)) {
        std::printf("HRESULT=%08lx at %s\n", static_cast<unsigned long>(h), what);
        throw std::runtime_error(what);
    }
}

static std::vector<BYTE> readBytes(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// ---------------------------------------------------------------- the fixture
struct Orbit {
    uint32_t rows = 0, verts = 0, inst = 0, per = 0, targetW = 0, targetH = 0, cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    std::vector<float> cb1, vb0, vb1;
    std::vector<uint8_t> stencil;   // the crop, row by row
    std::vector<float> depth;
};

static Orbit loadOrbit(const char* path) {
    const auto b = readBytes(path);
    check(b.size() > 8 + 44 && !std::memcmp(b.data(), "EDVROL01", 8), "the orbit fixture is readable and EDVROL01");
    size_t at = 8;
    auto u32 = [&]() {
        check(at + 4 <= b.size(), "the orbit fixture is complete");
        uint32_t v;
        std::memcpy(&v, &b[at], 4);
        at += 4;
        return v;
    };
    Orbit o;
    check(u32() == 1, "the orbit fixture is version 1");
    o.rows = u32();
    o.verts = u32();
    o.inst = u32();
    o.per = u32();
    o.targetW = u32();
    o.targetH = u32();
    o.cropX = u32();
    o.cropY = u32();
    o.cropW = u32();
    o.cropH = u32();
    auto floats = [&](std::vector<float>& v, size_t n) {
        check(at + n * 4 <= b.size(), "the orbit fixture is complete");
        v.resize(n);
        std::memcpy(v.data(), &b[at], n * 4);
        at += n * 4;
    };
    floats(o.cb1, size_t(o.rows) * 4);
    floats(o.vb0, size_t(o.verts) * 4);
    floats(o.vb1, size_t(o.inst) * o.per);
    const size_t total = size_t(o.cropW) * o.cropH;
    const uint32_t runs = u32();
    o.stencil.reserve(total);
    for (uint32_t i = 0; i < runs; ++i) {
        const uint32_t len = u32();
        check(at < b.size(), "the stencil runs are complete");
        o.stencil.insert(o.stencil.end(), len, b[at++]);
    }
    check(o.stencil.size() == total, "the stencil runs cover the crop");
    const uint32_t tokens = u32();
    o.depth.reserve(total);
    for (uint32_t i = 0; i < tokens; ++i) {
        const uint32_t zeros = u32(), literals = u32();
        o.depth.insert(o.depth.end(), zeros, 0.0f);
        check(at + size_t(literals) * 4 <= b.size(), "the depth tokens are complete");
        const size_t n = o.depth.size();
        o.depth.resize(n + literals);
        std::memcpy(&o.depth[n], &b[at], size_t(literals) * 4);
        at += size_t(literals) * 4;
    }
    check(o.depth.size() == total && at == b.size(), "the depth tokens cover the crop and nothing trails");
    return o;
}

// ---------------------------------------------------------------- the device
static ComPtr<ID3D11Device> g_dev;
static ComPtr<ID3D11DeviceContext> g_ctx;
static ComPtr<ID3D11InfoQueue> g_messages;

static ComPtr<ID3D11Buffer> makeBuffer(UINT bytes, UINT bind, const void* data) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.BindFlags = bind;
    const D3D11_SUBRESOURCE_DATA init{data, 0, 0};
    ComPtr<ID3D11Buffer> out;
    ck(g_dev->CreateBuffer(&bd, data ? &init : nullptr, &out), "CreateBuffer");
    return out;
}

static std::vector<uint8_t> readTexture(ID3D11Texture2D* src, UINT bytesPerPixel) {
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;
    ck(g_dev->CreateTexture2D(&d, nullptr, &stage), "staging texture");
    g_ctx->CopyResource(stage.Get(), src);
    D3D11_MAPPED_SUBRESOURCE m{};
    ck(g_ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "Map");
    std::vector<uint8_t> out(size_t(d.Width) * d.Height * bytesPerPixel);
    for (UINT y = 0; y < d.Height; ++y)
        std::memcpy(&out[size_t(y) * d.Width * bytesPerPixel], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(d.Width) * bytesPerPixel);
    g_ctx->Unmap(stage.Get(), 0);
    return out;
}

static std::vector<float> readBuffer(ID3D11Buffer* source) {
    D3D11_BUFFER_DESC bd{};
    source->GetDesc(&bd);
    bd.BindFlags = bd.MiscFlags = bd.StructureByteStride = 0;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> stage;
    ck(g_dev->CreateBuffer(&bd, nullptr, &stage), "staging buffer");
    g_ctx->CopyResource(stage.Get(), source);
    D3D11_MAPPED_SUBRESOURCE m{};
    ck(g_ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "Map buffer");
    std::vector<float> v(bd.ByteWidth / 4);
    std::memcpy(v.data(), m.pData, bd.ByteWidth);
    g_ctx->Unmap(stage.Get(), 0);
    return v;
}

static ComPtr<ID3DBlob> compileHlsl(const char* source, const char* profile) {
    ComPtr<ID3DBlob> code, error;
    const HRESULT h = D3DCompile(source, std::strlen(source), "orbit layer stand-in", nullptr, nullptr, "main", profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &error);
    if (FAILED(h) && error) std::puts(static_cast<const char*>(error->GetBufferPointer()));
    ck(h, "D3DCompile");
    return code;
}

// ---------------------------------------------------------------- the strip's geometry, at the layer's size (stream-out of the production patched shader)
constexpr size_t kOut = 9;   // colour (4), the signed distance (1), SV_POSITION (4)

struct Strip {
    std::vector<double> edge, perp;
};
struct Pixel {
    double x, y;
    bool front;
};
static Pixel toPixel(const float* o, double w, double h) {
    const double cw = o[8];
    return {(o[5] / cw * 0.5 + 0.5) * w, (-o[6] / cw * 0.5 + 0.5) * h, cw > 0.0 && std::isfinite(cw)};
}
// The pairs' edge length and the width perpendicular to the line, as the orbit-line rig measures them, in pixels of a w x h target.
static Strip measureStrip(const std::vector<float>& so, unsigned vertices, unsigned instance, double w, double h) {
    Strip s;
    const float* base = so.data() + size_t(instance) * vertices * kOut;
    auto at = [&](unsigned v) { return base + size_t(v) * kOut; };
    auto centre = [&](unsigned k, bool* front) {
        const Pixel a = toPixel(at(2 * k), w, h), b = toPixel(at(2 * k + 1), w, h);
        *front = a.front && b.front && std::isfinite(a.x + a.y + b.x + b.y);
        return Pixel{(a.x + b.x) / 2, (a.y + b.y) / 2, *front};
    };
    for (unsigned k = 1; k + 1 < vertices / 2; ++k) {
        bool f0, f1, f2;
        const Pixel c0 = centre(k - 1, &f0), c1 = centre(k, &f1), c2 = centre(k + 1, &f2);
        if (!f0 || !f1 || !f2) continue;
        if (!(c1.x >= 0 && c1.x <= w && c1.y >= 0 && c1.y <= h)) continue;
        double tx = c2.x - c0.x, ty = c2.y - c0.y;
        const double tl = std::hypot(tx, ty);
        if (tl < 1e-6) continue;
        tx /= tl;
        ty /= tl;
        const Pixel a = toPixel(at(2 * k), w, h), b = toPixel(at(2 * k + 1), w, h);
        const double ex = a.x - b.x, ey = a.y - b.y;
        s.edge.push_back(std::hypot(ex, ey));
        s.perp.push_back(std::fabs(ex * ty - ey * tx));
    }
    return s;
}
static double lowest(const std::vector<double>& v) { return *std::min_element(v.begin(), v.end()); }
static double highest(const std::vector<double>& v) { return *std::max_element(v.begin(), v.end()); }
// Every pair `expected` pixels wide, by the edge to 0.002 px and perpendicular to the line to 0.01 px, and enough pairs to mean something.
static bool widthIs(const Strip& s, double expected, unsigned atLeast) {
    if (s.edge.size() < atLeast) return false;
    return lowest(s.edge) >= expected - 0.002 && highest(s.edge) <= expected + 0.002 && lowest(s.perp) >= expected - 0.01 && highest(s.perp) <= expected + 0.01;
}

// ---------------------------------------------------------------- the game's depth-stencil crop, and the layer's seed of it
struct GameDs {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> depth, stencil;
    ComPtr<ID3D11DepthStencilView> dsv;
    UINT w = 0, h = 0;
};

// R32G8X24 texels are 8 bytes: the float depth, then the stencil in the low byte of the second dword (what a staging copy of the game's own reads as).
static GameDs makeGameDs(UINT w, UINT h, const std::vector<float>& depth, const std::vector<uint8_t>& stencil) {
    GameDs g;
    g.w = w;
    g.h = h;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    td.SampleDesc.Count = 1;
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Texture2D> stage;
    ck(g_dev->CreateTexture2D(&sd, nullptr, &stage), "the crop's staging texture");
    D3D11_MAPPED_SUBRESOURCE m{};
    ck(g_ctx->Map(stage.Get(), 0, D3D11_MAP_WRITE, 0, &m), "Map write");
    for (UINT y = 0; y < h; ++y) {
        uint8_t* row = static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        for (UINT x = 0; x < w; ++x) {
            const float d = depth[size_t(y) * w + x];
            const uint32_t s = stencil[size_t(y) * w + x];
            std::memcpy(row + size_t(x) * 8, &d, 4);
            std::memcpy(row + size_t(x) * 8 + 4, &s, 4);
        }
    }
    g_ctx->Unmap(stage.Get(), 0);
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
    ck(g_dev->CreateTexture2D(&td, nullptr, &g.tex), "the game's depth-stencil crop");
    g_ctx->CopyResource(g.tex.Get(), stage.Get());
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    sv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    ck(g_dev->CreateShaderResourceView(g.tex.Get(), &sv, &g.depth), "the crop's depth view");
    sv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    ck(g_dev->CreateShaderResourceView(g.tex.Get(), &sv, &g.stencil), "the crop's stencil view");
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ck(g_dev->CreateDepthStencilView(g.tex.Get(), &dv, &g.dsv), "the crop's depth-stencil view");
    return g;
}

struct Planes {
    std::vector<float> depth;
    std::vector<uint8_t> stencil;
};
static Planes readPlanes(ID3D11Texture2D* tex) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    const auto raw = readTexture(tex, 8);
    Planes p;
    p.depth.resize(size_t(d.Width) * d.Height);
    p.stencil.resize(p.depth.size());
    for (size_t i = 0; i < p.depth.size(); ++i) {
        std::memcpy(&p.depth[i], &raw[i * 8], 4);
        p.stencil[i] = raw[i * 8 + 4];
    }
    return p;
}

struct Seeded {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11DepthStencilView> dsv;
    Planes planes;
    UINT w = 0, h = 0;
};
// The layer's seed, as ui_layer.cpp seedLayerDepth does it: the layer's own depth-stencil target at the layer's size in the game's format, the production
// Seeder recording on a deferred context, executed on the immediate one.
static Seeded seedLayer(edvr_layer_seed::Seeder& seeder, const GameDs& game, UINT outW, UINT outH, UINT stencilMask, bool needsDepth, float jitterX = 0.0f) {
    Seeded s;
    s.w = outW;
    s.h = outH;
    D3D11_TEXTURE2D_DESC ld{};
    ld.Width = outW;
    ld.Height = outH;
    ld.MipLevels = ld.ArraySize = 1;
    ld.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    ld.SampleDesc.Count = 1;
    ld.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ck(g_dev->CreateTexture2D(&ld, nullptr, &s.tex), "the layer's depth-stencil");
    D3D11_DEPTH_STENCIL_VIEW_DESC lv{};
    lv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    lv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ck(g_dev->CreateDepthStencilView(s.tex.Get(), &lv, &s.dsv), "the layer's depth-stencil view");
    ComPtr<ID3D11DeviceContext> deferred;
    ck(g_dev->CreateDeferredContext(0, &deferred), "deferred context");
    seeder.seed(deferred.Get(), game.depth.Get(), game.stencil.Get(), s.dsv.Get(), game.w, game.h, outW, outH, jitterX, 0.0f, stencilMask, needsDepth);
    ComPtr<ID3D11CommandList> list;
    ck(deferred->FinishCommandList(FALSE, &list), "FinishCommandList");
    g_ctx->ExecuteCommandList(list.Get(), TRUE);
    s.planes = readPlanes(s.tex.Get());
    return s;
}

// The nearest-sample address of a layer pixel: the seed's own, and the texel on the other side of it when the pixel's centre sits exactly on a boundary
// (2.5 x puts one there every five pixels, and float rounding may go either way).
static void texelsOf(UINT p, UINT outSize, UINT inSize, int* c) {
    const double u = (p + 0.5) * double(inSize) / double(outSize);
    c[0] = std::max(0, std::min(int(inSize) - 1, int(std::floor(u - 1e-3))));
    c[1] = std::max(0, std::min(int(inSize) - 1, int(std::floor(u + 1e-3))));
}

// Layer pixels whose seeded (depth, stencil under the mask) is neither candidate texel's of the game's crop.
static unsigned seedMismatches(const Seeded& s, const std::vector<float>& depth, const std::vector<uint8_t>& stencil, UINT inW, UINT inH, uint8_t validMask) {
    unsigned bad = 0;
    for (UINT y = 0; y < s.h; ++y) {
        int ty[2];
        texelsOf(y, s.h, inH, ty);
        for (UINT x = 0; x < s.w; ++x) {
            int tx[2];
            texelsOf(x, s.w, inW, tx);
            const float sd = s.planes.depth[size_t(y) * s.w + x];
            const uint8_t ss = static_cast<uint8_t>(s.planes.stencil[size_t(y) * s.w + x] & validMask);
            bool match = false;
            for (int a = 0; a < 2 && !match; ++a)
                for (int b = 0; b < 2 && !match; ++b) {
                    const size_t t = size_t(ty[a]) * inW + size_t(tx[b]);
                    match = std::memcmp(&sd, &depth[t], 4) == 0 && ss == static_cast<uint8_t>(stencil[t] & validMask);
                }
            if (!match) ++bad;
        }
    }
    return bad;
}

// ---------------------------------------------------------------- the draw
// The game's pixel shader 6EEF165A350DA30F, stood in for: the alpha test of the colour the shader's own source has (stellar_coverage.h kOrbitalCoveragePs
// with the alpha ramp at 1), writing "lit" and the fragment's depth. The footprint is the vertex shader's strip and the tests' verdict; the ramp's soft edge
// is not under test here.
static const char kStandInPs[] = R"HLSL(
struct In {
    float4 colour : __USER_STELLARVERTEX_COLOUR;
    float distance : __USER_STELLARVERTEX_STABLESIGNEDUNITDISTANCEPERSPECTIVE;
    float4 pos : SV_Position;
};
float4 main(In i) : SV_Target0 {
    clip(i.colour.a - 1.0 / 255.0);
    return float4(1.0, i.pos.z, 0.0, 0.0);
}
)HLSL";

struct Target {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT w = 0, h = 0;
};
static Target makeTarget(UINT w, UINT h) {
    Target t;
    t.w = w;
    t.h = h;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ck(g_dev->CreateTexture2D(&td, nullptr, &t.tex), "render target");
    ck(g_dev->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv), "render target view");
    return t;
}

// One instance's fragments: lit and the fragment's depth, per pixel of the target.
struct Fragments {
    std::vector<uint8_t> lit;
    std::vector<float> z;
    unsigned count = 0;
};

static ComPtr<ID3D11DepthStencilState> makeDsState(bool depthTest, UINT8 stencilReadMask) {
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = depthTest ? TRUE : FALSE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    d.StencilEnable = TRUE;
    d.StencilReadMask = stencilReadMask;
    d.StencilWriteMask = 0;
    d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_EQUAL};
    d.BackFace = d.FrontFace;
    ComPtr<ID3D11DepthStencilState> s;
    ck(g_dev->CreateDepthStencilState(&d, &s), "depth-stencil state");
    return s;
}

struct Rig {
    const Orbit& o;
    std::vector<BYTE> gameVs;
    ComPtr<ID3D11VertexShader> original, patched;
    ComPtr<ID3D11Buffer> cb1, vb0, vb1, motion;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11RasterizerState> rs;
    Constants constants;
    Cache cache;

    explicit Rig(const Orbit& orbit, const std::vector<BYTE>& vsBytes) : o(orbit), gameVs(vsBytes) {
        cb1 = makeBuffer(UINT(o.cb1.size() * 4), D3D11_BIND_CONSTANT_BUFFER, o.cb1.data());
        vb0 = makeBuffer(UINT(o.vb0.size() * 4), D3D11_BIND_VERTEX_BUFFER, o.vb0.data());
        vb1 = makeBuffer(UINT(o.vb1.size() * 4), D3D11_BIND_VERTEX_BUFFER, o.vb1.data());
        const UINT zero[4] = {0, 0, 2, o.inst};
        motion = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, zero);
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSTANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"OSTOWST", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"OSTOWSR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"OSTOWSS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"COLOUR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 44, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
        ck(g_dev->CreateInputLayout(elements, 5, gameVs.data(), gameVs.size(), &layout), "input layout");
        ck(g_dev->CreateVertexShader(gameVs.data(), gameVs.size(), nullptr, &original), "the game's vertex shader");
        const uint64_t hash = hashOf(gameVs.data(), gameVs.size());
        check(cache.remember(original.Get(), hash, gameVs.data(), gameVs.size(), false).ok(), "the production cache remembers the game's shader");
        Result why;
        patched = cache.prepare(g_ctx.Get(), &why);
        check(patched && why.ok(), "the production cache makes the patched copy");
        const auto code = compileHlsl(kStandInPs, "ps_5_0");
        ck(g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps), "stand-in pixel shader");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        ck(g_dev->CreateRasterizerState(&rd, &rs), "rasterizer state");
    }

    void setupInput() const {
        g_ctx->ClearState();
        g_ctx->IASetInputLayout(layout.Get());
        ID3D11Buffer* vbs[] = {vb0.Get(), vb1.Get()};
        const UINT strides[] = {16, 60}, offsets[] = {0, 0};
        g_ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
        g_ctx->VSSetConstantBuffers(1, 1, cb1.GetAddressOf());
        g_ctx->VSSetConstantBuffers(12, 1, motion.GetAddressOf());
    }

    // The strip's vertices through the patched shader, the factor in b13 as the production binding puts it, streamed out: positions in clip space.
    std::vector<float> streamOut(double bufferFactor) {
        const D3D11_SO_DECLARATION_ENTRY decl[] = {{0, "__USER_STELLARVERTEX_COLOUR", 0, 0, 4, 0},
                                                   {0, "__USER_STELLARVERTEX_STABLESIGNEDUNITDISTANCEPERSPECTIVE", 0, 0, 1, 0},
                                                   {0, "SV_POSITION", 0, 0, 4, 0}};
        const UINT stride = UINT(kOut * 4);
        ComPtr<ID3D11GeometryShader> gs;
        ck(g_dev->CreateGeometryShaderWithStreamOutput(gameVs.data(), gameVs.size(), decl, 3, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs), "stream-out shader");
        auto out = makeBuffer(UINT(size_t(o.verts) * o.inst * kOut * 4), D3D11_BIND_STREAM_OUTPUT, nullptr);
        ID3D11Buffer* cb = constants.get(g_ctx.Get(), bufferFactor);
        check(cb != nullptr, "the production constants make the factor's buffer");
        setupInput();
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        g_ctx->GSSetShader(gs.Get(), nullptr, 0);
        const UINT zero = 0;
        g_ctx->SOSetTargets(1, out.GetAddressOf(), &zero);
        g_ctx->VSSetShader(original.Get(), nullptr, 0);
        Binding binding;
        check(binding.begin(g_ctx.Get(), patched.Get(), cb, [&](ID3D11VertexShader* now) { return Cache::isOriginal(now, g_ctx.Get()); }), "the production binding takes the game's shader");
        g_ctx->DrawInstanced(o.verts, o.inst, 0, 0);
        check(binding.finish(g_ctx.Get()).restored, "and gives it back");
        g_ctx->SOSetTargets(0, nullptr, nullptr);
        auto v = readBuffer(out.Get());
        g_ctx->ClearState();
        return v;
    }

    // One instance of the strip into `t`: the patched shader bound by the production binding with `bufferFactor` in b13, the viewport given, the depth-stencil
    // view (or none) and the state, the game's stencil reference 1.
    Fragments draw(const Target& t, const D3D11_VIEWPORT& vp, ID3D11DepthStencilView* dsv, ID3D11DepthStencilState* state, double bufferFactor, unsigned instance, UINT stencilRef = 1) {
        ID3D11Buffer* cb = constants.get(g_ctx.Get(), bufferFactor);
        check(cb != nullptr, "the production constants make the factor's buffer");
        setupInput();
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        g_ctx->VSSetShader(original.Get(), nullptr, 0);
        g_ctx->PSSetShader(ps.Get(), nullptr, 0);
        g_ctx->RSSetState(rs.Get());
        g_ctx->RSSetViewports(1, &vp);
        const float clear[4] = {0, 0, 0, 0};
        g_ctx->ClearRenderTargetView(t.rtv.Get(), clear);
        g_ctx->OMSetRenderTargets(1, t.rtv.GetAddressOf(), dsv);
        g_ctx->OMSetDepthStencilState(state, stencilRef);
        Binding binding;
        check(binding.begin(g_ctx.Get(), patched.Get(), cb, [&](ID3D11VertexShader* now) { return Cache::isOriginal(now, g_ctx.Get()); }), "the production binding takes the game's shader");
        g_ctx->DrawInstanced(o.verts, 1, 0, instance);
        check(binding.finish(g_ctx.Get()).restored, "and gives it back");
        const auto raw = readTexture(t.tex.Get(), 8);
        g_ctx->ClearState();
        Fragments f;
        const size_t n = size_t(t.w) * t.h;
        f.lit.assign(n, 0);
        f.z.assign(n, 0.0f);
        for (size_t i = 0; i < n; ++i) {
            float lit, z;
            std::memcpy(&lit, &raw[i * 8], 4);
            std::memcpy(&z, &raw[i * 8 + 4], 4);
            if (lit > 0.5f) {
                f.lit[i] = 1;
                f.z[i] = z;
                ++f.count;
            }
        }
        return f;
    }
};

// ---------------------------------------------------------------- the case: everything the real data is held to
struct Config {
    double bufferFactor = 0.4;      // the factor in b13 (production: uiPanelFactor's f, the same one the game's own draw is patched with)
    bool seedDepthStencil = true;   // false: no depth-stencil view bound (the seed skipped)
    UINT8 readMask = 0x81;          // the game's stencil read mask
    UINT stencilRef = 1;            // the game's stencil reference
    bool depthTest = true;          // the depth test in the state under test
    bool expectDepth = true;        // whether the oracle's expectation includes the depth test (false isolates the stencil test)
    UINT seedMask = 0x81;           // the stencil bits the seed asks for (production: the draw's read mask)
    float seedJitter = 0.0f;        // the seed's address shift in game texels
    unsigned instances = 6;
};

struct Metrics {
    bool widthOk = false;
    double edgeMin = 0, edgeMax = 0, perpMin = 0, perpMax = 0;
    unsigned litTotal = 0, litSky = 0, litStencilReject = 0, litDepthReject = 0, oracleMismatch = 0, litOnCockpitAll = 0, seedMismatch = 0;
    double fraction = 0;
    unsigned referenceLit = 0;
};

static bool accepted(const Metrics& m) {
    return m.widthOk && m.litStencilReject == 0 && m.litDepthReject == 0 && m.oracleMismatch == 0 && m.litOnCockpitAll == 0 && m.seedMismatch == 0 && m.fraction >= 0.98 &&
           m.litSky > 5000;
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && !std::strcmp(argv[1], "--dry-run")) {
            std::puts("orbit_layer_test: dry-run (no device, no files)");
            return 0;
        }
        // --hardware-self-test runs the same checks on the machine's own adapter, where the seed takes its one-pass SV_StencilRef path: not a gate (a
        // build machine may have no adapter), the evidence that a real driver keeps the seed and the tests as WARP does.
        const bool hardware = argc == 2 && !std::strcmp(argv[1], "--hardware-self-test");
        check(argc == 2 && (hardware || !std::strcmp(argv[1], "--self-test")), "usage: orbit_layer_test --self-test | --hardware-self-test | --dry-run");
        const D3D_DRIVER_TYPE driver = hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
        const HRESULT made = edvr::systemD3D11CreateDevice()(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
        if (FAILED(made))
            ck(edvr::systemD3D11CreateDevice()(nullptr, driver, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx), "device");
        g_dev.As(&g_messages);
        std::printf("orbit_layer_test: %s%s\n", hardware ? "hardware adapter" : "WARP", SUCCEEDED(made) ? " with the debug layer" : "");
        {
            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> adapter;
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(g_dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)))
                std::printf("adapter: %ls (vendor=%04X device=%04X)\n", desc.Description, desc.VendorId, desc.DeviceId);
        }

        // ---- the inputs
        const auto gameVs = readBytes("tools/orbital_width_test/fixtures/vs_C7FA0C0F5DD49180.dxbc");
        check(gameVs.size() == kBytes, "the game's shader fixture is 2412 bytes");
        const Orbit o = loadOrbit("tools/orbit_layer_test/fixtures/orbit_050020_29722_312.bin");
        check(o.rows == 333 && o.verts == 8194 && o.inst == 6 && o.per == 15 && o.targetW == 2016 && o.targetH == 1949 && o.cropX == 1190 && o.cropY == 700 && o.cropW == 826 &&
                  o.cropH == 630,
              "the orbit fixture is the captured draw's shape: 333 rows, 8194 vertices, 6 instances, the 2016x1949 eye, the 826x630 crop at (1190, 700)");
        check(o.cb1[332 * 4] == 2016.0f && o.cb1[332 * 4 + 1] == 1949.0f && std::fabs(o.cb1[332 * 4 + 2] - 1.0f / 2016.0f) < 1e-9f &&
                  std::fabs(o.cb1[332 * 4 + 3] - 1.0f / 1949.0f) < 1e-9f,
              "cb1[332] is (2016, 1949, 1/2016, 1/1949)");
        const double halfWidth[6] = {2.0, 1.5, 1.5, 1.5, 1.5, 1.5};
        for (unsigned i = 0; i < 6; ++i) check(o.vb1[i * 15 + 3] == float(halfWidth[i]), "the instances' half-widths are 2.0 and five of 1.5");
        for (unsigned i = 0; i < 3; ++i) check(o.vb1[i * 15 + 14] > 1.0f / 255.0f, "the first three instances' colour alpha passes the shader's alpha test");

        // ---- the production numbers: the layer, its map, the factor
        const edvr::UiLayerSize ls = edvr::uiLayerSize(4032, 3898, 1.25f);
        check(ls.w == 5040 && ls.h == 4873, "the layer of a 2016x1949 render at fix.ui_quality 125 is 5040x4873");
        check(edvr::uiLayerWiderThanRender(ls.w, ls.h, 2016, 1949), "that layer is wider than the render: the take is made");
        edvr::UiPanelInputs in;
        in.renderW = 2016;
        in.fovTangent = in.trueTangent = 2.0f;
        in.outputW = 4032;
        in.target = 1.25f;
        double f = 0;
        check(edvr::uiPanelFactor(in, &f) && std::fabs(f - 0.4) < 1e-9, "the panel patch's factor for that frame is 0.4");
        const edvr::UiLayerMap m = edvr::uiLayerMapFromRegion(0.0f, 0.0f, 2016.0f, 1949.0f, ls.w, ls.h);
        check(std::fabs(f * double(ls.w) / 2016.0 - 1.0) < 1e-9, "f x (layer / render) is 1 across: the patched strip's 2wf render pixels are 2w layer pixels");
        const edvr::UiViewport gv{0, 0, 2016, 1949, 0, 1};
        const edvr::UiViewport lv = edvr::uiLayerMapViewport(m, gv, 0.0f, 0.0f);
        check(lv.w == 5040.0f && std::fabs(lv.h - 4873.0f) < 0.01f && lv.x == 0.0f && lv.y == 0.0f, "the game's viewport through the production map is the whole layer");
        // the crop of the layer the 826x630 crop of the eye is: the layer's own pixels (1190 x 2.5, 700 x 2.50026) .. +2065 x 1575
        const UINT cropLX = UINT(std::lround(1190.0 * m.ax)), cropLY = UINT(std::floor(700.0 * m.ay));
        const UINT cropLW = UINT(std::lround(826.0 * m.ax)), cropLH = UINT(std::lround(630.0 * m.ay));
        check(cropLX == 2975 && cropLY == 1750 && cropLW == 2065 && cropLH == 1575, "the crop is the layer's (2975, 1750) 2065x1575");
        const D3D11_VIEWPORT layerVp{lv.x - float(cropLX), lv.y - float(cropLY), lv.w, lv.h, 0.0f, 1.0f};
        const D3D11_VIEWPORT renderVp{-1190.0f, -700.0f, 2016.0f, 1949.0f, 0.0f, 1.0f};

        Rig rig(o, gameVs);
        edvr_layer_seed::Seeder seeder;
        seeder.init(g_dev.Get());
        check(seeder.ensure(g_dev.Get(), 8, 8, DXGI_FORMAT_D32_FLOAT_S8X24_UINT), "the production seeder takes the game's depth format");
        const bool specified = seeder.usesSpecifiedStencilRef();
        const uint8_t validMask = specified ? 0xFF : 0x81;
        std::printf("orbit layer: layer %ux%u (x%.4f, x%.4f of the render), f %.4f, crop %ux%u at layer (%u, %u); the seed copies %s\n", ls.w, ls.h, double(m.ax), double(m.ay), f, cropLW, cropLH,
                    cropLX, cropLY, specified ? "depth and all eight stencil bits in one pass (SV_StencilRef)" : "the depth, then one pass per stencil bit asked");

        const GameDs game = makeGameDs(o.cropW, o.cropH, o.depth, o.stencil);
        {   // the crop's planes made it into the texture the seeder reads
            const Planes p = readPlanes(game.tex.Get());
            check(p.depth.size() == o.depth.size() && !std::memcmp(p.depth.data(), o.depth.data(), o.depth.size() * 4) && p.stencil == o.stencil, "the game's depth-stencil crop is the fixture's, bit for bit");
        }
        const auto gameState = makeDsState(true, 0x81);
        const Target layerTarget = makeTarget(cropLW, cropLH);
        const Target renderTarget = makeTarget(o.cropW, o.cropH);

        // ---- the unobstructed draw (U) and the reference (R) are the same for every case below
        std::vector<Fragments> U(o.inst), R(o.inst);
        for (unsigned i = 0; i < o.inst; ++i) U[i] = rig.draw(layerTarget, layerVp, nullptr, gameState.Get(), f, i);
        unsigned uTotal = 0;
        float zMin = 1e30f, zMax = 0.0f;
        for (unsigned i = 0; i < o.inst; ++i) {
            uTotal += U[i].count;
            for (size_t p = 0; p < U[i].lit.size(); ++p)
                if (U[i].lit[p]) {
                    zMin = std::min(zMin, U[i].z[p]);
                    zMax = std::max(zMax, U[i].z[p]);
                }
        }
        std::printf("orbit layer: unobstructed lit layer pixels %u / %u / %u / %u / %u / %u (the orbit, then five rings), the line's depth %.3g .. %.3g\n", U[0].count, U[1].count, U[2].count,
                    U[3].count, U[4].count, U[5].count, double(zMin), double(zMax));
        check(U[0].count > 8000 && U[1].count > 2000 && U[2].count > 2000 && U[3].count == 0 && U[4].count == 0 && U[5].count == 0,
              "unobstructed, the orbit and two rings light the crop (thousands of layer pixels each) and the other three rings are not in it");
        check(zMin > 0.0f && zMax > zMin, "the line has a depth range of its own to test against (all fragments in front of the far plane)");
        // the reference: the game's own patched draw at the render size, the game's depth and stencil, the same state
        for (unsigned i = 0; i < o.inst; ++i) R[i] = rig.draw(renderTarget, renderVp, game.dsv.Get(), gameState.Get(), f, i);

        const size_t texels = size_t(o.cropW) * o.cropH;
        std::vector<uint8_t> refLit(texels, 0);
        unsigned referenceLit = 0;
        for (unsigned i = 0; i < o.inst; ++i)
            for (size_t t = 0; t < texels; ++t)
                if (R[i].lit[t] && o.stencil[t] == 5 && !refLit[t]) {
                    refLit[t] = 1;
                    ++referenceLit;
                }
        check(referenceLit > 1000, "the game's own patched draw lights thousands of stencil-5 pixels of the crop at the render size");

        // ---- one case, held to everything
        auto runCase = [&](const Config& cfg, const char* name, bool verbose) {
            Metrics mt;
            // geometry
            const auto so = rig.streamOut(cfg.bufferFactor);
            const Strip orbit = measureStrip(so, o.verts, 0, double(ls.w), double(ls.h));
            const Strip ring1 = measureStrip(so, o.verts, 1, double(ls.w), double(ls.h)), ring2 = measureStrip(so, o.verts, 2, double(ls.w), double(ls.h));
            mt.widthOk = widthIs(orbit, 2 * halfWidth[0], 1000) && widthIs(ring1, 2 * halfWidth[1], 1000) && widthIs(ring2, 2 * halfWidth[2], 1000);
            if (!orbit.edge.empty()) {
                mt.edgeMin = lowest(orbit.edge);
                mt.edgeMax = highest(orbit.edge);
                mt.perpMin = lowest(orbit.perp);
                mt.perpMax = highest(orbit.perp);
            }
            // the seed
            const Seeded seeded = seedLayer(seeder, game, cropLW, cropLH, cfg.seedMask, true, cfg.seedJitter);
            mt.seedMismatch = seedMismatches(seeded, o.depth, o.stencil, o.cropW, o.cropH, validMask);
            const auto state = makeDsState(cfg.depthTest, cfg.readMask);
            std::vector<uint8_t> litBlock(texels, 0);
            for (unsigned i = 0; i < cfg.instances; ++i) {
                const Fragments L = rig.draw(layerTarget, layerVp, cfg.seedDepthStencil ? seeded.dsv.Get() : nullptr, state.Get(), cfg.bufferFactor, i, cfg.stencilRef);
                for (size_t p = 0; p < L.lit.size(); ++p) {
                    const float sd = seeded.planes.depth[p];
                    const uint8_t ss = seeded.planes.stencil[p];
                    const bool stencilPass = (ss & 0x81) == 1;
                    const bool expected = U[i].lit[p] && stencilPass && (!cfg.expectDepth || U[i].z[p] >= sd);   // the oracle: the game's state on the seeded values
                    if (bool(L.lit[p]) != expected) ++mt.oracleMismatch;
                    if (!L.lit[p]) continue;
                    ++mt.litTotal;
                    if (!stencilPass) ++mt.litStencilReject;
                    if (L.z[p] < sd) ++mt.litDepthReject;
                    if (sd == 0.0f && stencilPass) ++mt.litSky;
                    const UINT x = UINT(p % cropLW), y = UINT(p / cropLW);
                    int tx[2], ty[2];
                    texelsOf(x, cropLW, o.cropW, tx);
                    texelsOf(y, cropLH, o.cropH, ty);
                    bool allReject = true;
                    for (int a = 0; a < 2; ++a)
                        for (int b = 0; b < 2; ++b) {
                            const size_t t = size_t(ty[a]) * o.cropW + size_t(tx[b]);
                            litBlock[t] = 1;
                            if ((o.stencil[t] & 0x81) == 1) allReject = false;
                        }
                    if (allReject) ++mt.litOnCockpitAll;   // the game's own stencil, every texel the pixel could be the nearest sample of, refuses it
                }
            }
            unsigned covered = 0;
            for (size_t t = 0; t < texels; ++t)
                if (refLit[t] && litBlock[t]) ++covered;
            mt.referenceLit = referenceLit;
            mt.fraction = referenceLit ? double(covered) / double(referenceLit) : 0.0;
            if (verbose)
                std::printf("orbit layer [%s]: strip edge %.4f..%.4f px, perpendicular %.4f..%.4f px (2w = 4); lit %u (sky %u), on a stencil the game refuses %u, behind the seeded depth %u, "
                            "oracle mismatches %u, seed mismatches %u; lit fraction vs the 2016 reference %.4f of %u\n",
                            name, mt.edgeMin, mt.edgeMax, mt.perpMin, mt.perpMax, mt.litTotal, mt.litSky, mt.litStencilReject, mt.litDepthReject, mt.oracleMismatch, mt.seedMismatch,
                            mt.fraction, mt.referenceLit);
            return mt;
        };

        // ---- the real thing
        const Metrics real = runCase(Config{}, "the take", true);
        check(real.widthOk && real.edgeMin >= 3.998 && real.edgeMax <= 4.002 && real.perpMin >= 3.99 && real.perpMax <= 4.01, "the strip is 2w = 4.000 layer pixels wide (the rings 3.000)");
        check(real.seedMismatch == 0, "the layer's depth-stencil is the game's crop at the layer's size, texel for texel");
        check(real.oracleMismatch == 0, "the draw's tests are the game's state on the seeded values, pixel for pixel, for every instance");
        check(real.litStencilReject == 0 && real.litOnCockpitAll == 0, "0 lit pixels where stencil & 0x81 != 1");
        check(real.litDepthReject == 0, "0 lit pixels where the scene is nearer than the line");
        check(real.litSky > 5000 && real.litTotal >= real.litSky, "and the line is drawn over the sky (thousands of pixels)");
        check(real.fraction >= 0.98, "at least 98% of the pixels the game's own 2016 draw lights over stencil 5 have a lit layer pixel in their block");
        check(accepted(real), "the comparator accepts the real thing");
        // not vacuous: unobstructed, the line crosses the cockpit and the nearer scene
        {
            const Seeded seeded = seedLayer(seeder, game, cropLW, cropLH, 0x81, true);
            unsigned onStencilReject = 0, onDepthReject = 0;
            float bodyMin = 1.0f, bodyMax = 0.0f;
            for (unsigned i = 0; i < 3; ++i)
                for (size_t p = 0; p < U[i].lit.size(); ++p) {
                    if (!U[i].lit[p]) continue;
                    if ((seeded.planes.stencil[p] & 0x81) != 1) {
                        ++onStencilReject;
                    } else if (U[i].z[p] < seeded.planes.depth[p]) {
                        ++onDepthReject;
                        bodyMin = std::min(bodyMin, seeded.planes.depth[p]);
                        bodyMax = std::max(bodyMax, seeded.planes.depth[p]);
                    }
                }
            std::printf("orbit layer: unobstructed the line would light %u pixels on stencils the game refuses (the cockpit and the planet), %u behind a nearer scene that the stencil passes (stencil 5, depth %.4g .. %.4g "
                        "against the line's 1e-11); the layer lights none\n",
                        onStencilReject, onDepthReject, double(bodyMin), double(bodyMax));
            check(onStencilReject > 500, "not vacuous: the unobstructed line lights hundreds of pixels on the cockpit and the planet's stencil classes");
            check(onDepthReject > 100 && bodyMin > zMax, "not vacuous: and over a hundred behind a nearer scene that the stencil alone would pass (the ship's lower edge, stencil 5 at depth 0.003..0.004)");
        }
        // the mass: the layer's coverage in render pixels against the reference's
        {
            unsigned layerLit = 0, refAll = 0;
            const Seeded seeded = seedLayer(seeder, game, cropLW, cropLH, 0x81, true);
            for (unsigned i = 0; i < 3; ++i) {
                const Fragments L = rig.draw(layerTarget, layerVp, seeded.dsv.Get(), gameState.Get(), f, i);
                layerLit += L.count;
                refAll += R[i].count;
            }
            const double ratio = double(layerLit) / 6.25 / double(refAll);
            std::printf("orbit layer: the layer lights %u pixels = %.0f render pixels of 2.5 x 2.5; the game's own 2016 draw lights %u (ratio %.3f; pixel-centre coverage of a 1.6 px line is not exact)\n", layerLit,
                        double(layerLit) / 6.25, refAll, ratio);
            check(ratio > 0.8 && ratio < 1.25, "the layer's coverage is the patched scene line's, at the layer's density (within the sampling of a 1.6 px line)");
        }

        // ---- a line behind a nearer scene is rejected, one in front accepted: the same draw over a constant depth, and over a split
        {
            const size_t split = o.cropW / 2;
            auto scenario = [&](float leftDepth, float rightDepth, const char* name) {
                std::vector<float> depth(texels);
                std::vector<uint8_t> stencil(texels, 5);
                for (UINT y = 0; y < o.cropH; ++y)
                    for (UINT x = 0; x < o.cropW; ++x) depth[size_t(y) * o.cropW + x] = x < split ? leftDepth : rightDepth;
                const GameDs g = makeGameDs(o.cropW, o.cropH, depth, stencil);
                const Seeded seeded = seedLayer(seeder, g, cropLW, cropLH, 0x81, true);
                unsigned lit = 0, litLeft = 0, litRight = 0, uLeft = 0, uRight = 0, mismatches = 0;
                for (unsigned i = 0; i < 3; ++i) {
                    const Fragments L = rig.draw(layerTarget, layerVp, seeded.dsv.Get(), gameState.Get(), f, i);
                    for (size_t p = 0; p < L.lit.size(); ++p) {
                        const bool expected = U[i].lit[p] && U[i].z[p] >= seeded.planes.depth[p];
                        if (bool(L.lit[p]) != expected) ++mismatches;
                        const UINT x = UINT(p % cropLW);
                        const bool leftSide = x < UINT(split * 2.5) - 2, rightSide = x > UINT(split * 2.5) + 2;
                        if (U[i].lit[p] && leftSide) ++uLeft;
                        if (U[i].lit[p] && rightSide) ++uRight;
                        if (!L.lit[p]) continue;
                        ++lit;
                        if (leftSide) ++litLeft;
                        if (rightSide) ++litRight;
                    }
                }
                std::printf("orbit layer: %-52s lit %u (left %u of %u, right %u of %u), oracle mismatches %u\n", name, lit, litLeft, uLeft, litRight, uRight, mismatches);
                check(mismatches == 0, "the tests over a constant or split depth are the oracle's, pixel for pixel");
                return std::tuple<unsigned, unsigned, unsigned, unsigned, unsigned>(lit, litLeft, uLeft, litRight, uRight);
            };
            const float front = zMin * 0.5f, behind = zMax * 2.0f;
            const auto inFront = scenario(front, front, "scene farther than the line (depth below its own):");
            check(std::get<0>(inFront) == U[0].count + U[1].count + U[2].count && std::get<0>(inFront) > 20000, "a line in front of the scene is accepted: every unobstructed pixel is lit");
            const auto behindAll = scenario(behind, behind, "scene nearer than the line (depth above its own):");
            check(std::get<0>(behindAll) == 0, "a line behind the scene is rejected: nothing is lit");
            const auto halves = scenario(behind, front, "a nearer planet on the left half, farther on the right:");
            check(std::get<1>(halves) == 0 && std::get<2>(halves) > 3000, "the nearer half rejects what an unobstructed line would light on it");
            check(std::get<3>(halves) == std::get<4>(halves) && std::get<4>(halves) > 3000, "and the farther half accepts every pixel of it");
        }

        // ---- the mutants: each wrong thing through the comparator that accepts the real one
        {
            Config c;
            c.instances = 3;
            c.bufferFactor = 1.0;   // the width patch left at the game's own 2 in the layer: 10 px
            const Metrics m1 = runCase(c, "mutant: the factor not applied", true);
            check(!accepted(m1) && !m1.widthOk, "MUTANT, the factor not applied in the layer: the width check fails");
            check(m1.edgeMin >= 9.99 && m1.edgeMax <= 10.01, "...because the strip is 10 px (4 x 2.5), not 4");
            c.bufferFactor = f * f;
            const Metrics m2 = runCase(c, "mutant: the factor applied twice", false);
            check(!accepted(m2) && !m2.widthOk && m2.edgeMax < 1.7, "MUTANT, the factor applied twice: the width check fails (a 1.6 px strip)");
            Config d;
            d.instances = 3;
            d.seedDepthStencil = false;
            const Metrics m3 = runCase(d, "mutant: the seed skipped (no depth-stencil bound)", true);
            check(!accepted(m3) && m3.litStencilReject > 500 && m3.litOnCockpitAll > 100, "MUTANT, the seed skipped: the line lights the cockpit and the planet's stencil classes");
            // The stencil test alone: the cockpit and the planet are behind the line's depth as well, so the depth test hides a wrong stencil mask in the
            // whole case. With the depth test off in the state and out of the oracle, the real mask is accepted and each wrong one is not.
            auto stencilOnly = [&](UINT8 mask, UINT ref) {
                Config c2;
                c2.instances = 3;
                c2.depthTest = false;
                c2.expectDepth = false;
                c2.readMask = mask;
                c2.stencilRef = ref;
                return c2;
            };
            auto acceptedStencil = [](const Metrics& m) {
                return m.widthOk && m.oracleMismatch == 0 && m.litStencilReject == 0 && m.litOnCockpitAll == 0 && m.seedMismatch == 0 && m.litTotal > 20000;
            };
            const Metrics s0 = runCase(stencilOnly(0x81, 1), "the stencil test alone", true);
            check(acceptedStencil(s0) && s0.litTotal > real.litTotal, "the stencil test alone, with the game's mask 0x81 and reference 1, is accepted (and lights what only the depth test refuses)");
            const Metrics m4 = runCase(stencilOnly(0x01, 1), "mutant: stencil read mask 0x01", true);
            check(!acceptedStencil(m4) && m4.litStencilReject > 0 && m4.oracleMismatch > 0, "MUTANT, the stencil mask 0x01 (bit 7 not read): lit where the game refuses");
            const Metrics m5 = runCase(stencilOnly(0x80, 1), "mutant: stencil read mask 0x80", true);
            check(!acceptedStencil(m5) && m5.litStencilReject > 0 && m5.oracleMismatch > 0, "MUTANT, the stencil mask 0x80 (bit 0 not read): lit where the game refuses");
            const Metrics m5b = runCase(stencilOnly(0x81, 0), "mutant: stencil reference 0", false);
            check(!acceptedStencil(m5b) && m5b.litSky == 0 && m5b.fraction < 0.05 && m5b.oracleMismatch > 20000, "MUTANT, the stencil reference 0 (the sky's bit 0 refused): the line is lost");
            if (specified) {
                const Metrics m5c = runCase(stencilOnly(0xFF, 1), "mutant: stencil read mask 0xFF", false);
                check(!acceptedStencil(m5c) && m5c.litSky == 0 && m5c.fraction < 0.05, "MUTANT, the stencil mask 0xFF (the sky's other bits read): the line is lost");
            }
            Config g;
            g.instances = 3;
            g.depthTest = false;
            const Metrics m6 = runCase(g, "mutant: the depth test off", true);
            check(!accepted(m6) && m6.litDepthReject > 100, "MUTANT, the depth test off: the line lights behind the nearer scene");
            Config h;
            h.instances = 3;
            h.seedJitter = 1.0f;
            const Metrics m7 = runCase(h, "mutant: the seed taken a texel away", false);
            check(!accepted(m7) && m7.seedMismatch > 1000, "MUTANT, the seed taken a texel away: the seed check fails");
            if (!specified) {
                Config k;
                k.instances = 3;
                k.seedMask = 0x01;
                const Metrics m8 = runCase(k, "mutant: the seed's stencil mask without bit 7", true);
                check(!accepted(m8) && m8.seedMismatch > 0, "MUTANT, the seed's stencil mask without bit 7: the seed check fails");
            } else {
                std::puts("orbit layer: the seed copies every stencil bit on this device, so the seed-mask mutant has no effect here (the one-pass-per-bit path is ui_layer_seed_test's)");
            }
        }

        if (g_messages) {
            for (UINT64 i = 0; i < g_messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                SIZE_T size = 0;
                g_messages->GetMessage(i, nullptr, &size);
                std::vector<char> data(size);
                auto* msg = reinterpret_cast<D3D11_MESSAGE*>(data.data());
                ck(g_messages->GetMessage(i, msg, &size), "debug message");
                if (msg->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
                    std::puts(msg->pDescription);
                    check(false, "D3D debug layer");
                }
            }
        }
        std::printf("PASS orbit_layer_test: %u checks; the production patched copy, binding, constants, seed and map on the real dump, on %s\n", g_checks,
                    hardware ? "the hardware adapter" : "WARP");
        return 0;
    } catch (const std::exception& ex) {
        std::printf("FAIL %s (%u checks)\n", ex.what(), g_checks);
        return 1;
    }
}
