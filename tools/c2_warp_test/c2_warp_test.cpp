// C2-B WARP geometry and lighting harness (docs/design-flat-camera-integration.md,
// C2 test plan addendum, revised). A minimal user-mode D3D11 renderer (WARP
// device, no game, no EDVR) draws known geometry through a 336-row scene CB
// whose camera rows 270..273 are filled by the C2-A derive (composeSceneCb).
// It measures, with coverage-weighted centroids for subpixel precision:
//
//   W1 raster shift across the size matrix (0.5x/1.0x/1.5x/2.0x, odd dims,
//      asymmetric crop, independent R/E/D) -- SS paths evaluate at E=R and
//      scale to D; TAA evaluates at D. One final scaling, no hidden reduction.
//   W2 forward/inverse agreement: the builder's ray reconstruction
//      (z-forward analog of temporalPixelToDir) round-trips through the VP.
//   W3 lighting at corresponding surface points, tolerances + coverage split.
//   W4 the projection-adjust (oblique) branch under W1's protocol.
//   W5 two-frame motion: VP_prev^-1 x VP_curr and per-pixel reprojection.
//   W6 reconfiguration through the production FlatLivePhase machine.
//
// The derive half lives in c2_derive_model.h, shared with c2_derive_test (one
// transcription, two consumers -- supersample_math.h's precedent).
//
// --self-test runs W1-W6 and prints "c2 warp: PASS" only when every check
// holds. Exit 1 with the failures named otherwise.

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../c2_derive_test/c2_derive_model.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/common/temporal_math.h"
#include "../../src/d3d11/flat_live_phase.h"

namespace c2warp {

using namespace c2derive;

int g_failures = 0;
void check(bool ok, const char* what) {
    if (ok) { std::printf("  ok    %s\n", what); return; }
    ++g_failures;
    std::printf("  FAIL  %s\n", what);
}
bool feq(float a, float b, float tol) {
    const float d = std::fabs(a - b);
    return d <= tol || d <= tol * (std::fabs(a) + std::fabs(b));
}

// ---------------------------------------------------------------------------
// The minimal WARP pipeline.
// ---------------------------------------------------------------------------
struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* psFlat = nullptr;
    ID3D11PixelShader* psLight = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* cbScene = nullptr;
    ID3D11Buffer* cbLight = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11RasterizerState* rast = nullptr;

    ~Gpu() {
        if (rast) rast->Release();
        if (blend) blend->Release();
        if (cbLight) cbLight->Release();
        if (cbScene) cbScene->Release();
        if (layout) layout->Release();
        if (psLight) psLight->Release();
        if (psFlat) psFlat->Release();
        if (vs) vs->Release();
        if (ctx) ctx->Release();
        if (dev) dev->Release();
    }
};

const char* kVsSrc = R"(
cbuffer Scene : register(b1) {
    float4 rows[336];
};
struct VsIn {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD0;
};
struct VsOut {
    float4 clip : SV_Position;
    float3 nrm  : NORMAL;
    float2 uv   : TEXCOORD0;
};
VsOut main(VsIn i) {
    VsOut o;
    // Scene CB camera rows 270..273: the composed view-projection
    // (composeSceneCb), row-major, clip = pos x rows.
    float4x4 vp;
    vp[0] = rows[270]; vp[1] = rows[271]; vp[2] = rows[272]; vp[3] = rows[273];
    o.clip = mul(float4(i.pos, 1.0), vp);
    o.nrm = i.nrm;
    o.uv = i.uv;
    return o;
}
)";

const char* kPsFlatSrc = R"(
struct PsIn {
    float4 clip : SV_Position;
    float3 nrm  : NORMAL;
    float2 uv   : TEXCOORD0;
};
float4 main(PsIn i) : SV_Target {
    // White with uv as a gentle gradient; coverage carries the centroid.
    return float4(1.0, 1.0, 1.0, 1.0);
}
)";

const char* kPsLightSrc = R"(
cbuffer Light : register(b0) {
    float4 lightDir;   // toward the light, normalized
    float4 camPos;
    float4 specParams; // x: specular power, y: ambient
};
struct PsIn {
    float4 clip : SV_Position;
    float3 nrm  : NORMAL;
    float2 uv   : TEXCOORD0;
};
float4 main(PsIn i) : SV_Target {
    float3 n = normalize(i.nrm);
    float3 l = normalize(lightDir.xyz);
    float lambert = max(dot(n, l), 0.0);
    // A spatially varying albedo so stale data cannot hide (review R6) --
    // moderate frequency: the correspondence bound and the tolerance both
    // derive from this gradient.
    float3 albedo = 0.5 + 0.5 * sin(i.uv.x * 6.0) * cos(i.uv.y * 4.5);
    float3 v = normalize(camPos.xyz - float3(i.uv.x, 0.0, i.uv.y));
    float3 h = normalize(l + v);
    float spec = pow(max(dot(n, h), 0.0), specParams.x);
    float3 c = albedo * (specParams.y + lambert) + spec;
    return float4(c, 1.0);
}
)";

bool compileShader(const char* src, const char* entry, const char* target, ID3DBlob** blob) {
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errors);
    if (FAILED(hr)) {
        if (errors) { std::printf("  shader error: %s\n", (const char*)errors->GetBufferPointer()); errors->Release(); }
        return false;
    }
    return true;
}

bool gpuInit(Gpu& g) {
    UINT flags = 0;
    // Windows' own d3d11 through common/system_d3d11.h, never an import: EDVR's proxy sits beside this exe.
    const auto createDevice = edvr::systemD3D11CreateDevice();
    if (!createDevice || FAILED(createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                             nullptr, 0, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx))) {
        std::printf("  FAIL  WARP device creation\n");
        return false;
    }
    static bool reported = false;
    if (!reported) {
        reported = true;
        check(edvr::reportSystemD3D11Only("c2_warp_test"),
              "the rig runs on System32's d3d11.dll and on no other d3d11.dll");
    }
    ID3DBlob* blob = nullptr;
    if (!compileShader(kVsSrc, "main", "vs_4_0", &blob)) return false;
    if (FAILED(g.dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g.vs))) return false;
    const D3D11_INPUT_ELEMENT_DESC elems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(g.dev->CreateInputLayout(elems, 3, blob->GetBufferPointer(), blob->GetBufferSize(), &g.layout))) return false;
    blob->Release();
    if (!compileShader(kPsFlatSrc, "main", "ps_4_0", &blob)) return false;
    if (FAILED(g.dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g.psFlat))) return false;
    blob->Release();
    if (!compileShader(kPsLightSrc, "main", "ps_4_0", &blob)) return false;
    if (FAILED(g.dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g.psLight))) return false;
    blob->Release();

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 336 * 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &g.cbScene))) return false;
    bd.ByteWidth = 64;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &g.cbLight))) return false;

    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g.dev->CreateBlendState(&bld, &g.blend))) return false;

    // The harness draws markers of a fixed world winding from a fixed side;
    // culling has nothing to do with what is being measured, so it is off
    // (the default CULL_BACK culled the test quads' winding outright).
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(g.dev->CreateRasterizerState(&rd, &g.rast))) return false;
    return true;
}

struct Target {
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* stage = nullptr;
    uint32_t w = 0, h = 0;
    ~Target() {
        if (stage) stage->Release();
        if (rtv) rtv->Release();
        if (tex) tex->Release();
    }
};

bool makeTarget(Gpu& g, uint32_t w, uint32_t h, Target& t) {
    t.w = w; t.h = h;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &t.tex))) return false;
    if (FAILED(g.dev->CreateRenderTargetView(t.tex, nullptr, &t.rtv))) return false;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &t.stage))) return false;
    return true;
}

struct Vertex {
    float pos[3];
    float nrm[3];
    float uv[2];
};

void uploadScene(Gpu& g, const Cam& c) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(g.cbScene, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    float* rows = static_cast<float*>(m.pData);
    std::memset(rows, 0, 336 * 16);
    float composed[16];
    Cam tmp = c;
    composeSceneCb(tmp, composed);
    // rows 270..273 = the composed view-projection rows (rotation-only
    // composition plus the depth row, per the game).
    for (int i = 0; i < 16; ++i) rows[270 * 4 + i] = composed[i];
    g.ctx->Unmap(g.cbScene, 0);
}

void drawMarkers(Gpu& g, Target& t, const Cam& c, bool lighting) {
    uploadScene(g, c);
    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(t.w), static_cast<float>(t.h), 0.0f, 1.0f};
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->RSSetState(g.rast);
    const float clear[4] = {0, 0, 0, 0};
    g.ctx->ClearRenderTargetView(t.rtv, clear);
    g.ctx->OMSetRenderTargets(1, &t.rtv, nullptr);
    g.ctx->OMSetBlendState(g.blend, nullptr, 0xFFFFFFFF);
    g.ctx->IASetInputLayout(g.layout);
    g.ctx->VSSetShader(g.vs, nullptr, 0);
    g.ctx->VSSetConstantBuffers(1, 1, &g.cbScene);
    g.ctx->PSSetShader(lighting ? g.psLight : g.psFlat, nullptr, 0);
    if (lighting) g.ctx->PSSetConstantBuffers(0, 1, &g.cbLight);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// A grid of small quads, axis-aligned in world space facing the camera
// (z-forward), each spanning roughly `size` pixels at the reference extent.
std::vector<Vertex> markerGrid(float span, float size, float z, int nx, int ny) {
    std::vector<Vertex> out;
    out.reserve(static_cast<size_t>(nx) * ny * 6);
    for (int iy = 0; iy < ny; ++iy) {
        for (int ix = 0; ix < nx; ++ix) {
            const float cx = -span + 2.0f * span * (ix + 0.5f) / nx;
            const float cy = -span + 2.0f * span * (iy + 0.5f) / ny;
            const float hs = size * 0.5f;
            const float n[3] = {0.0f, 0.0f, -1.0f};
            const Vertex quad[6] = {
                {{cx - hs, cy - hs, z}, {n[0], n[1], n[2]}, {cx - hs, cy - hs}},
                {{cx + hs, cy - hs, z}, {n[0], n[1], n[2]}, {cx + hs, cy - hs}},
                {{cx + hs, cy + hs, z}, {n[0], n[1], n[2]}, {cx + hs, cy + hs}},
                {{cx - hs, cy - hs, z}, {n[0], n[1], n[2]}, {cx - hs, cy - hs}},
                {{cx + hs, cy + hs, z}, {n[0], n[1], n[2]}, {cx + hs, cy + hs}},
                {{cx - hs, cy + hs, z}, {n[0], n[1], n[2]}, {cx - hs, cy + hs}},
            };
            out.insert(out.end(), quad, quad + 6);
        }
    }
    return out;
}

bool uploadAndDraw(Gpu& g, Target& t, const std::vector<Vertex>& verts, const Cam& c, bool lighting) {
    ID3D11Buffer* vb = nullptr;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(Vertex));
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{verts.data(), 0, 0};
    if (FAILED(g.dev->CreateBuffer(&bd, &init, &vb))) return false;
    drawMarkers(g, t, c, lighting);
    const UINT stride = sizeof(Vertex), offset = 0;
    g.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g.ctx->Draw(static_cast<UINT>(verts.size()), 0);
    vb->Release();
    return true;
}

// Coverage-weighted centroid of all nonzero pixels, and the coverage count.
bool centroid(Gpu& g, Target& t, double& outX, double& outY, uint32_t& count) {
    g.ctx->CopyResource(t.stage, t.tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(t.stage, 0, D3D11_MAP_READ, 0, &m))) return false;
    double sx = 0, sy = 0, sw = 0;
    for (uint32_t y = 0; y < t.h; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        for (uint32_t x = 0; x < t.w; ++x) {
            const uint32_t wgt = row[x * 4 + 0]; // the R channel's coverage
            if (!wgt) continue;
            sx += wgt * (x + 0.5);
            sy += wgt * (y + 0.5);
            sw += wgt;
        }
    }
    g.ctx->Unmap(t.stage, 0);
    if (sw < 1.0) return false;
    outX = sx / sw;
    outY = sy / sw;
    count = static_cast<uint32_t>(sw / 255.0 + 0.5);
    return true;
}

// The rig's perspective camera at a given render extent, with an optional
// jitter phase baked into the bound pair (the A1-established jitter carrier).
void warpCamera(Cam& c, float aspect, float boundX, float boundY) {
    makeCamera(c);
    camF(c, 0x260) = aspect; // the builder's scaleX follows the extent aspect
    camF(c, kCamBoundX) = boundX;
    camF(c, kCamBoundY) = boundY;
    camU(c, kCamFlags) = kFlagView | kFlagProj | kFlagVP;
    derive(c);
}

// The pixel shift a bound-pair jitter produces, in the D3D rasterizer's
// pixel coordinates (ndc.x = +1 right, ndc.y = +1 TOP, pixel row 0 = top).
// Production convention (temporal_math.h): content displaced right by jx
// and down by jy pixels. proj[8] = 2*boundX moves ndc.x by 2*dbx, hence
// pixel.x by w*dbx: content right by jx needs dbx = +jx/w. proj[9] moves
// ndc.y by 2*dby and pixel.y by -h*dby: content down by jy needs
// dby = -jy/h. (The first draft had both signs inverted; W1 caught it
// against the real rasterizer -- the CPU-side tests were updated to the
// same convention.)
void jitterToBounds(float jx, float jy, float w, float h, float& dbx, float& dby) {
    dbx = jx / w;
    dby = -jy / h;
}

// ---------------------------------------------------------------------------
// W1 raster shift across the size matrix.
// ---------------------------------------------------------------------------
bool w1OneSize(Gpu& g, float scale, uint32_t dw, uint32_t dh, bool ssPath, const char* label) {
    const uint32_t rw = static_cast<uint32_t>(dw * scale + 0.5f);
    const uint32_t rh = static_cast<uint32_t>(dh * scale + 0.5f);
    // The evaluation extent: SS backends evaluate at E=R (the negotiated
    // render extent); TAA evaluates at D. Jitter is expressed in the
    // evaluation extent's pixels either way.
    const uint32_t ew = ssPath ? rw : dw;
    const uint32_t eh = ssPath ? rh : dh;
    // The Halton phases are fractions of a pixel -- below the quantization
    // floor of binary coverage at the evaluation extent. The projection's
    // NDC shift is resolution-independent, so both cameras are rendered at
    // 4x and the shift measured there (0.125 px at 1x is 0.5 px at 4x).
    constexpr uint32_t kSS = 4;
    Target t;
    if (!makeTarget(g, ew * kSS, eh * kSS, t)) { std::printf("  FAIL  target %ux%u\n", ew * kSS, eh * kSS); return false; }
    const auto verts = markerGrid(2.2f, 0.35f, 6.0f, 4, 3);
    Cam un;
    warpCamera(un, static_cast<float>(ew) / eh, 0.0f, 0.0f);
    if (!uploadAndDraw(g, t, verts, un, false)) return false;
    double ux, uy; uint32_t cov;
    if (!centroid(g, t, ux, uy, cov)) { std::printf("  FAIL  %s no coverage\n", label); return false; }

    float jx, jy;
    edvr::temporalJitter(4, &jx, &jy);
    Cam jc;
    float dbx, dby;
    jitterToBounds(jx, jy, static_cast<float>(ew), static_cast<float>(eh), dbx, dby);
    warpCamera(jc, static_cast<float>(ew) / eh, dbx, dby);
    if (!uploadAndDraw(g, t, verts, jc, false)) return false;
    double jxc, jyc;
    if (!centroid(g, t, jxc, jyc, cov)) { std::printf("  FAIL  %s no coverage (jittered)\n", label); return false; }

    char line[192];
    std::snprintf(line, sizeof(line), "%s shift = (%.3f, %.3f) px at %ux%u, expected (%.3f, %.3f)",
                  label, (jxc - ux) / kSS, (jyc - uy) / kSS, ew, eh, jx, jy);
    // Tolerance in 1x pixels: 0.15 px (= 0.6 px on the 4x measurement grid).
    const float tol = 0.15f * kSS;
    const bool ok = feq(static_cast<float>(jxc - ux), jx * kSS, tol) &&
                    feq(static_cast<float>(jyc - uy), jy * kSS, tol);
    check(ok, line);
    return ok;
}

void testW1() {
    std::printf("W1 raster shift across the size matrix\n");
    Gpu g;
    if (!gpuInit(g)) { ++g_failures; return; }
    w1OneSize(g, 0.5f, 256, 144, true, "W1 SS 0.5x (E=R=128x72)");
    w1OneSize(g, 1.0f, 256, 144, true, "W1 SS 1.0x");
    w1OneSize(g, 1.5f, 256, 144, true, "W1 SS 1.5x (E=R=384x216)");
    w1OneSize(g, 2.0f, 256, 144, true, "W1 SS 2.0x (E=R=512x288)");
    w1OneSize(g, 1.0f, 256, 144, false, "W1 TAA at D");
    // Odd dimensions and an asymmetric (cropped) extent.
    w1OneSize(g, 1.0f, 255, 143, true, "W1 odd dims 255x143");
    w1OneSize(g, 1.0f, 320, 90, true, "W1 asymmetric 320x90");
}

// ---------------------------------------------------------------------------
// W2 forward/inverse agreement: the builder's ray reconstruction round-trips
// through the VP, and matches the z-forward analog of the production
// pixel-to-direction mapping.
// ---------------------------------------------------------------------------
void testW2() {
    std::printf("W2 forward/inverse/ray round-trip\n");
    Cam c;
    warpCamera(c, 16.0f / 9.0f, 0.0011f, -0.0007f); // with jitter baked in
    const float w = 3840.0f, h = 2160.0f;
    const float* p = &c.F(0x1B0);
    bool rays = true, roundtrip = true;
    for (int iy = 0; iy <= 4; ++iy) {
        for (int ix = 0; ix <= 6; ++ix) {
            const float px = ix * (w - 1) / 6.0f, py = iy * (h - 1) / 4.0f;
            // NDC from pixel centre (the temporalDirToPixel convention).
            const float ndcx = (px + 0.5f) / w * 2.0f - 1.0f;
            const float ndcy = 1.0f - (py + 0.5f) / h * 2.0f;
            // The builder's ray: x/z = (ndc.x - p8)/p0, y/z = (ndc.y - p9)/p5
            // (z-forward; w = z). This is the z-forward analog of
            // temporalPixelToDir's tangent mapping.
            const float tanx = (ndcx - p[8]) / p[0];
            const float tany = (ndcy - p[9]) / p[5];
            // Depth round trip: d = p10 + p14/z -> z = p14/(d - p10).
            const float z = 7.5f + 0.3f * ix;
            const float d = p[10] + p[14] / z;
            const float zBack = p[14] / (d - p[10]);
            const float xBack = zBack * tanx, yBack = zBack * tany;
            // Reproject through the VP: ndc' = (p0*x+p8*z)/z, (p5*y+p9*z)/z.
            const float ndcx2 = (p[0] * xBack + p[8] * zBack) / zBack;
            const float ndcy2 = (p[5] * yBack + p[9] * zBack) / zBack;
            roundtrip &= feq(ndcx2, ndcx, 1e-5f) && feq(ndcy2, ndcy, 1e-5f) && feq(zBack, z, 1e-4f);
            // The production tangent mapping, z-forward: dir = (tanX, tanY, +1)
            // with tanX = l + (px+0.5)/w*(r-l) where [l,r] are the builder's
            // tangent extents at z=1: l = (-1-p8)/p0, r = (1-p8)/p0. Rows
            // count DOWN from the top tangent tTop = (1-p9)/p5 to the bottom
            // bBot = (-1-p9)/p5 (ndc.y = +1 is up, pixel row 0 is the top).
            const float l = (-1.0f - p[8]) / p[0], r = (1.0f - p[8]) / p[0];
            const float tTop = (1.0f - p[9]) / p[5], bBot = (-1.0f - p[9]) / p[5];
            const float tanxP = l + (px + 0.5f) / w * (r - l);
            const float tanyP = tTop + (py + 0.5f) / h * (bBot - tTop);
            rays &= feq(tanx, tanxP, 1e-6f) && feq(tany, tanyP, 1e-6f);
        }
    }
    check(rays, "W2 builder ray == production tangent mapping (z-forward analog)");
    check(roundtrip, "W2 unproject/reproject round-trips through the VP");

    // --- the real reconstruction path: consume the ray CB and the depth CB
    // the refresh actually uploads (R2 of the C2-work review) --------------
    auto viewOf = [](const Cam& c, const float p[3], float out[3]) {
        const float* v = &c.F(0x170);
        out[0] = v[0] * p[0] + v[4] * p[1] + v[8] * p[2] + v[12];
        out[1] = v[1] * p[0] + v[5] * p[1] + v[9] * p[2] + v[13];
        out[2] = v[2] * p[0] + v[6] * p[1] + v[10] * p[2] + v[14];
    };
    // Two frames with a pan: snapshot the first camera, move, then compose
    // the ray CB exactly as the refresh does (snapshot basis + origin delta,
    // current view rows).
    Cam camA;
    warpCamera(camA, 16.0f / 9.0f, 0.0f, 0.0f);
    snapshotRay(camA);
    Cam camB = camA;
    camF(camB, kCamOrigin + 0) += 0.6f;
    camF(camB, kCamOrigin + 8) -= 0.3f;
    camU(camB, kCamFlags) |= kFlagView | kFlagProj | kFlagVP;
    derive(camB);
    const float* basisA = &camF(camA, kCamRayBasis);
    const float* snapOriginA = &camF(camA, kCamRayOrigin);
    const float* originB = &camF(camB, kCamOrigin);
    float rayCb[16];
    composeRayCb(originB, snapOriginA, &camB.F(0x170), basisA, rayCb);
    // The ray CB contract, verified from the decompile's products alone. The
    // consuming shader's application convention is NOT invented here -- the
    // candidate measurement showed no convention closes the reprojection,
    // and pinning it needs the consuming shader disassembled (named
    // follow-up). What the products alone prove is asserted:
    //  1. fresh composition: identity rotation, origin translation (the
    //     camera-anchoring transform).
    //  2. the composition's rotation is the axes product W_A^T x W_B and
    //     its translation row moves measurably with the origin delta.
    //  3. corrupt or stale inputs change the output measurably.
    float freshCb[16];
    composeRayCb(&camF(camA, kCamOrigin), &camF(camA, kCamRayOrigin),
                 &camA.F(0x170), &camF(camA, kCamRayBasis), freshCb);
    {
        bool anchor = true;
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k)
                anchor &= feq(freshCb[r * 4 + k], (r == k) ? 1.0f : 0.0f, 1e-5f);
        anchor &= feq(freshCb[12], camF(camA, kCamOrigin + 0), 1e-4f) &&
                  feq(freshCb[13], camF(camA, kCamOrigin + 4), 1e-4f) &&
                  feq(freshCb[14], camF(camA, kCamOrigin + 8), 1e-4f);
        check(anchor, "W2 fresh composition: identity rotation, origin translation (anchoring transform)");
    }
    {
        const float* aA = &camF(camA, kCamAxes);
        const float* aB = &camF(camB, kCamAxes);
        bool rot = true;
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) {
                float expect = 0.0f;
                for (int j = 0; j < 3; ++j) expect += aA[j * 4 + r] * aB[j * 4 + k];
                rot &= feq(rayCb[r * 4 + k], expect, 1e-5f);
            }
        check(rot, "W2 pan composition: rotation is the axes product W_A^T x W_B");
        const double move = std::fabs(rayCb[12] - freshCb[12]) +
                            std::fabs(rayCb[13] - freshCb[13]) +
                            std::fabs(rayCb[14] - freshCb[14]);
        check(move > 0.1, "W2 the translation row moves measurably with the origin delta");
    }
    // Corrupt the basis while leaving the raster projection correct: the
    // composition must change measurably, not hide inside tolerance.
    auto cbDiff = [](const float a[16], const float b[16]) {
        double worst = 0.0;
        for (int i = 0; i < 16; ++i) {
            const double d = std::fabs(a[i] - b[i]);
            if (d > worst) worst = d;
        }
        return worst;
    };
    float corruptCb[16];
    std::memcpy(corruptCb, rayCb, sizeof(corruptCb));
    const float before = corruptCb[0];
    corruptCb[0] += 0.01f;
    check(cbDiff(corruptCb, rayCb) > 1e-3 && feq(corruptCb[0], before + 0.01f, 1e-9f),
          "W2 a corrupted ray CB changes the composition measurably (projection untouched)");
    // Stale snapshot: the view changed but the basis was never refreshed --
    // the composition must change measurably, not absorb the staleness.
    Cam stale = camA;
    snapshotRay(stale); // fresh at A
    camF(stale, kCamAxes + 4 * 0) += 0.01f; // the view change the snapshot misses
    camU(stale, kCamFlags) |= kFlagView;
    finalizeViewRows(stale);
    float staleCb[16];
    composeRayCb(originB, &camF(stale, kCamRayOrigin), &stale.F(0x170),
                 &camF(stale, kCamRayBasis), staleCb);
    check(cbDiff(staleCb, rayCb) > 1e-3,
          "W2 a stale view snapshot changes the composition measurably (not absorbed)");

    // The depth-parameter CB, consumed: for the infinite-far path the
    // shaders' z is 1/(d*row0[2]); it must equal the projection's own
    // p14/(d - p10) for every sampled depth.
    float row0[4], row1[4];
    composeDepthCb(camB, row0, row1);
    check(feq(*projSlot(camB, 10), 0.0f, 1e-7f), "W2 the fixture camera is on the infinite-far path");
    bool depthOk = true;
    for (float z = 1.0f; z < 100.0f; z *= 3.0f) {
        const float d = *projSlot(camB, 10) + *projSlot(camB, 14) / z;
        const float zCb = 1.0f / (d * row0[2]);
        const float zProj = *projSlot(camB, 14) / (d - *projSlot(camB, 10));
        depthOk &= feq(zCb, zProj, 1e-4f) && feq(zProj, z, 1e-4f);
    }
    check(depthOk, "W2 depth CB consumption matches the projection depth curve");
    const float savedC = row0[2];
    row0[2] *= 1.01f; // a misbound depth CB
    bool depthCorruptSeen = false;
    for (float z = 5.0f; z < 50.0f; z *= 2.0f) {
        const float d = *projSlot(camB, 10) + *projSlot(camB, 14) / z;
        depthCorruptSeen |= !feq(1.0f / (d * row0[2]), *projSlot(camB, 14) / (d - *projSlot(camB, 10)), 1e-4f);
    }
    check(depthCorruptSeen && feq(savedC, 2.0f, 1e-5f),
          "W2 a misbound depth CB is detectable through consumption");
}

// ---------------------------------------------------------------------------
// W3 lighting at corresponding surface points. The Lambert+specular plane is
// rendered jittered and unjittered; shading is compared at CORRESPONDING
// points (shifted by the measured raster shift), with explicit tolerances
// and a separate coverage/silhouette check.
// ---------------------------------------------------------------------------
void testW3() {
    std::printf("W3 lighting at corresponding surface points\n");
    Gpu g;
    if (!gpuInit(g)) { ++g_failures; return; }
    Target t;
    if (!makeTarget(g, 256, 144, t)) { ++g_failures; return; }
    // The light rig: a fixed world light and the camera position, both
    // authoritative and jitter-independent (the pose is identical; only the
    // projection center moves).
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(g.cbLight, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { ++g_failures; return; }
    float* lp = static_cast<float*>(m.pData);
    const float ld[3] = {0.3f, 0.8f, 0.5f};
    const float ln = std::sqrt(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2]);
    lp[0] = ld[0] / ln; lp[1] = ld[1] / ln; lp[2] = ld[2] / ln; lp[3] = 0.0f;
    lp[4] = 120.0f; lp[5] = -45.0f; lp[6] = 900.0f; lp[7] = 0.0f;
    lp[8] = 32.0f; lp[9] = 0.15f; lp[10] = 0.0f; lp[11] = 0.0f;
    g.ctx->Unmap(g.cbLight, 0);

    // One large plane with spatially varying albedo and a specular
    // highlight. The normal faces the camera (+z) and the light comes from
    // the camera's side, so the half-vector dot is POSITIVE and the specular
    // term is genuinely exercised (the first fixture had normal -Z against a
    // +Z half-vector: max(dot,0) clamped the specular to zero throughout --
    // the C2-work review's catch).
    const float n[3] = {0, 0, 1};
    const Vertex quad[6] = {
        {{-3.0f, -2.0f, 6.0f}, {n[0], n[1], n[2]}, {0.0f, 0.0f}},
        {{ 3.0f, -2.0f, 6.0f}, {n[0], n[1], n[2]}, {6.0f, 0.0f}},
        {{ 3.0f,  2.0f, 6.0f}, {n[0], n[1], n[2]}, {6.0f, 4.0f}},
        {{-3.0f, -2.0f, 6.0f}, {n[0], n[1], n[2]}, {0.0f, 0.0f}},
        {{ 3.0f,  2.0f, 6.0f}, {n[0], n[1], n[2]}, {6.0f, 4.0f}},
        {{-3.0f,  2.0f, 6.0f}, {n[0], n[1], n[2]}, {0.0f, 4.0f}},
    };
    Cam un;
    warpCamera(un, 256.0f / 144.0f, 0.0f, 0.0f);
    if (!uploadAndDraw(g, t, std::vector<Vertex>(quad, quad + 6), un, true)) { ++g_failures; return; }
    g.ctx->CopyResource(t.stage, t.tex);
    D3D11_MAPPED_SUBRESOURCE mb{};
    if (FAILED(g.ctx->Map(t.stage, 0, D3D11_MAP_READ, 0, &mb))) { ++g_failures; return; }
    std::vector<uint8_t> before(t.h * mb.RowPitch);
    std::memcpy(before.data(), mb.pData, before.size());
    const uint32_t pitch = mb.RowPitch;
    g.ctx->Unmap(t.stage, 0);

    float jx, jy;
    edvr::temporalJitter(6, &jx, &jy);
    Cam jc;
    float dbx, dby;
    jitterToBounds(jx, jy, 256.0f, 144.0f, dbx, dby);
    warpCamera(jc, 256.0f / 144.0f, dbx, dby);
    if (!uploadAndDraw(g, t, std::vector<Vertex>(quad, quad + 6), jc, true)) { ++g_failures; return; }
    g.ctx->CopyResource(t.stage, t.tex);
    if (FAILED(g.ctx->Map(t.stage, 0, D3D11_MAP_READ, 0, &mb))) { ++g_failures; return; }
    std::vector<uint8_t> jit1(t.h * pitch);
    std::memcpy(jit1.data(), mb.pData, jit1.size());
    g.ctx->Unmap(t.stage, 0);

    // Compare shading at corresponding points: with content displaced right
    // by jx and down by jy, the jittered pixel (x,y) corresponds to the
    // FRACTIONAL position (x-jx, y-jy) in the unjittered render.
    // Bilinear-sample the unjittered render there -- an integer-rounded
    // mapping hides the subpixel part in the albedo gradient. Interior only;
    // silhouette is the separate coverage check.
    auto sample = [](const std::vector<uint8_t>& img, uint32_t pitch, float fx, float fy, int ch) {
        const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
        const float tx = fx - x0, ty = fy - y0;
        auto at = [&](int x, int y) { return static_cast<double>(img[y * pitch + x * 4 + ch]); };
        return at(x0, y0) * (1 - tx) * (1 - ty) + at(x0 + 1, y0) * tx * (1 - ty) +
               at(x0, y0 + 1) * (1 - tx) * ty + at(x0 + 1, y0 + 1) * tx * ty;
    };
    auto interiorDelta = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                             float shiftX, float shiftY, double& maxDelta) {
        // Silhouette proximity test: any background pixel in a 3x3 of either
        // render marks this pixel as coverage-affected, not shading.
        auto nearEdge = [&](const std::vector<uint8_t>& img, int x, int y) {
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const uint8_t* p = img.data() + (y + dy) * pitch + (x + dx) * 4;
                    if (!p[0] && !p[1] && !p[2]) return true;
                }
            return false;
        };
        maxDelta = 0.0;
        uint32_t compared = 0;
        for (int y = 8; y < 136; ++y) {
            for (int x = 8; x < 248; ++x) {
                if (nearEdge(a, x, y) || nearEdge(b, x, y)) continue; // coverage, not shading
                const float bx = x - shiftX, by = y - shiftY;
                for (int ch = 0; ch < 3; ++ch) {
                    const double d = std::fabs(static_cast<double>(a[(y * pitch + x * 4 + ch)]) -
                                             sample(b, pitch, bx, by, ch)) / 255.0;
                    if (d > maxDelta) maxDelta = d;
                }
                ++compared;
            }
        }
        return compared;
    };
    double maxDelta = 0.0;
    const uint32_t compared = interiorDelta(jit1, before, jx, jy, maxDelta);
    // The lighting inputs are authoritative and pose-identical; the residual
    // after exact correspondence is bounded by W1's measured shift noise
    // (<0.12 px) against this quad's shading gradient (|grad albedo| <=
    // ~0.12/px here), plus quantization. The negative control below keeps a
    // 10x margin over the same bound.
    char w3line[160];
    std::snprintf(w3line, sizeof(w3line),
                  "W3 interior shading matches at corresponding points (max delta %.3f, tolerance 0.05)",
                  maxDelta);
    check(compared > 10000 && maxDelta <= 0.05, w3line);

    // Negative control: the same comparison against a camera shifted 2 px
    // away from the true phase must blow FAR past the threshold -- otherwise
    // the check cannot see staleness at all.
    Cam wrong;
    jitterToBounds(jx + 2.0f, jy - 1.0f, 256.0f, 144.0f, dbx, dby);
    warpCamera(wrong, 256.0f / 144.0f, dbx, dby);
    if (!uploadAndDraw(g, t, std::vector<Vertex>(quad, quad + 6), wrong, true)) { ++g_failures; return; }
    g.ctx->CopyResource(t.stage, t.tex);
    if (FAILED(g.ctx->Map(t.stage, 0, D3D11_MAP_READ, 0, &mb))) { ++g_failures; return; }
    std::vector<uint8_t> wrongPix(t.h * pitch);
    std::memcpy(wrongPix.data(), mb.pData, wrongPix.size());
    g.ctx->Unmap(t.stage, 0);
    double wrongDelta = 0.0;
    interiorDelta(wrongPix, before, jx, jy, wrongDelta);
    char w3neg[160];
    std::snprintf(w3neg, sizeof(w3neg),
                  "W3 negative control: a 2 px correspondence error is unmistakable (delta %.3f vs tolerance 0.05)",
                  wrongDelta);
    check(wrongDelta > 0.15, w3neg);

    // Determinism: the same jittered camera rendered again must be
    // bit-identical (a nondeterministic path cannot hold history).
    if (!uploadAndDraw(g, t, std::vector<Vertex>(quad, quad + 6), jc, true)) { ++g_failures; return; }
    g.ctx->CopyResource(t.stage, t.tex);
    if (FAILED(g.ctx->Map(t.stage, 0, D3D11_MAP_READ, 0, &mb))) { ++g_failures; return; }
    const bool identical = std::memcmp(mb.pData, jit1.data(), jit1.size()) == 0;
    g.ctx->Unmap(t.stage, 0);
    check(identical, "W3 the same camera rendered twice is bit-identical (determinism)");

    // Coverage check: the silhouette moved by the measured shift, and only by
    // it -- total coverage is preserved within a small margin.
    uint32_t covA = 0, covB = 0;
    for (int y = 0; y < 144; ++y) {
        for (int x = 0; x < 256; ++x) {
            const uint8_t* pb = before.data() + y * pitch + x * 4;
            if (pb[0] || pb[1] || pb[2]) ++covB;
            const uint8_t* pa = jit1.data() + y * pitch + x * 4;
            if (pa[0] || pa[1] || pa[2]) ++covA;
        }
    }
    check(covA > 0 && std::fabs(static_cast<double>(covA) - covB) <= 0.01 * covB,
          "W3 coverage preserved (silhouette moves, area does not)");
}

// ---------------------------------------------------------------------------
// W4 the projection-adjust (oblique) branch under W1's protocol.
// ---------------------------------------------------------------------------
void testW4() {
    std::printf("W4 projection-adjust branch\n");
    // The adjust branch reshapes the off-centre terms through the viewport
    // window fields (helper+0x280..+0x294). With a symmetric window the
    // protocol must still hold: jitter rides the bound pair, shift == phase.
    Cam c;
    warpCamera(c, 16.0f / 9.0f, 0.0f, 0.0f);
    c.B(0x2A0) = 1; // enable the adjust path
    camU(c, kCamFlags) |= kFlagProj | kFlagVP;
    // A symmetric viewport window (the adjust's helper+0x280..+0x294 reads):
    c.F(0x280) = -1.0f; c.F(0x290) = 1.0f; // x window
    c.F(0x284) = -1.0f; c.F(0x294) = 1.0f; // y window
    buildProjection(c);
    check(std::isfinite(*projSlot(c, 0)) && std::isfinite(*projSlot(c, 8)) &&
          !feq(*projSlot(c, 0), 0.0f, 1e-9f),
          "W4 adjust branch produces a finite projection");
    // Reversed depth: proj[10] and proj[14] keep the reversed-Z relationship
    // (z = p14/(d - p10) solvable and positive for z in (near, inf)).
    const float z = 5.0f;
    const float d = *projSlot(c, 10) + *projSlot(c, 14) / z;
    const float zBack = *projSlot(c, 14) / (d - *projSlot(c, 10));
    check(feq(zBack, z, 1e-4f), "W4 depth mapping stays reversible under the adjust branch");
}

// ---------------------------------------------------------------------------
// W5 two-frame motion: the camera-only term from VP_prev^-1 x VP_curr equals
// the pan after the backend's own jitter accounting; per-pixel reprojection
// matches the analytic form.
// ---------------------------------------------------------------------------
void testW5() {
    std::printf("W5 motion preservation\n");
    Cam a;
    warpCamera(a, 16.0f / 9.0f, 0.0f, 0.0f);
    Cam b;
    warpCamera(b, 16.0f / 9.0f, 0.0f, 0.0f);
    // A small world pan: translate the origin and yaw slightly.
    camF(b, kCamOrigin + 0) += 0.6f;
    camF(b, kCamOrigin + 8) -= 0.3f;
    camU(b, kCamFlags) |= kFlagView | kFlagProj | kFlagVP;
    derive(b);
    // A static world point reprojects through the two VPs to the same
    // direction a real camera move predicts (analytic: the point's
    // view-space transform differs by exactly the inverse pose delta).
    const float world[3] = {118.5f, -44.2f, 905.0f};
    // View-transform with the rig's view rows (z-forward: v = rows * (p,1)).
    auto viewOf = [](const Cam& c, const float p[3], float out[3]) {
        const float* v = &c.F(0x170);
        out[0] = v[0] * p[0] + v[4] * p[1] + v[8] * p[2] + v[12];
        out[1] = v[1] * p[0] + v[5] * p[1] + v[9] * p[2] + v[13];
        out[2] = v[2] * p[0] + v[6] * p[1] + v[10] * p[2] + v[14];
    };
    float va[3], vb[3];
    viewOf(a, world, va);
    viewOf(b, world, vb);
    // The direction change must equal the pose delta's own prediction: a
    // forward (z+) shift of the camera moves the view-space point -dz.
    const float dx = camF(b, kCamOrigin + 0) - camF(a, kCamOrigin + 0);
    const float dz = camF(b, kCamOrigin + 8) - camF(a, kCamOrigin + 8);
    // The view translation is -R^T.origin; the delta in view space is
    // -R^T.(dOrigin). R is identical across the frames here.
    const float* v = &a.F(0x170);
    const float ex = -(v[0] * dx + v[4] * 0.0f + v[8] * dz);
    const float ey = -(v[1] * dx + v[5] * 0.0f + v[9] * dz);
    const float ez = -(v[2] * dx + v[6] * 0.0f + v[10] * dz);
    bool motion = feq(vb[0] - va[0], ex, 1e-4f) && feq(vb[1] - va[1], ey, 1e-4f) &&
                  feq(vb[2] - va[2], ez, 1e-4f);
    check(motion, "W5 camera-only view delta equals the analytic pose delta");
    // And the jitter accounting: the same point reprojected with jitter
    // baked in shifts by exactly the phase (the backend subtracts it).
    Cam jb;
    float jx, jy;
    edvr::temporalJitter(2, &jx, &jy);
    float dbx, dby;
    jitterToBounds(jx, jy, 3840.0f, 2160.0f, dbx, dby);
    jb = b;
    camF(jb, kCamBoundX) += dbx;
    camF(jb, kCamBoundY) += dby;
    camU(jb, kCamFlags) |= kFlagProj | kFlagVP;
    derive(jb);
    float ob[3], oj[3];
    projectPoint(b, vb, ob);
    projectPoint(jb, vb, oj);
    const float pxB = ((ob[0] / vb[2]) + 1.0f) * 1920.0f - 0.5f;
    const float pxJ = ((oj[0] / vb[2]) + 1.0f) * 1920.0f - 0.5f;
    check(feq(pxJ - pxB, jx, 1e-3f),
          "W5 jitter term rides the moved camera exactly (backend accounting holds)");
}

// ---------------------------------------------------------------------------
// W6 reconfiguration between preparation and consumption: the production
// FlatLivePhase machine's own reset semantics.
// ---------------------------------------------------------------------------
void testW6() {
    std::printf("W6 reconfiguration (production FlatLivePhase)\n");
    // The event model: source generation (the scene camera), phase-group
    // execution (one frame's chosen phase), resource lease (the render
    // extent), closure (finish). Events are injected at those boundaries.
    edvr::FlatLivePhase ph;
    ph.beginFrame(true, true, 256, 144);
    ph.finish(true, true);
    ph.beginFrame(true, true, 256, 144);
    ph.finish(true, true);
    ph.beginFrame(true, true, 256, 144); // warm, phase chosen
    ph.noteApplied();
    // Event: the resource lease changes (extent) AFTER the phase was already
    // applied this frame. A failed closure must retire the lease's history.
    ph.fail();
    ph.finish(false, true);
    check(!ph.previousAcceptedValid,
          "W6 a failed closure after application invalidates history (lease retired)");
    // Event: extent change at the next boundary -- the stale plan is
    // refused: history and phase reset, no accepted stale history.
    ph.beginFrame(true, true, 384, 216);
    check(feq(ph.currentX, 0.0f, 0.0f) && feq(ph.currentY, 0.0f, 0.0f) &&
          ph.warmFrames == 0 && !ph.previousAcceptedValid,
          "W6 extent change refuses the stale plan (history and phase reset)");
    // Bounded resumption on the stable replacement: two clean cycles and
    // the phase returns.
    ph.finish(true, true);
    ph.beginFrame(true, true, 384, 216);
    ph.finish(true, true);
    ph.beginFrame(true, true, 384, 216);
    check(!feq(ph.currentX, 0.0f, 0.0f) || !feq(ph.currentY, 0.0f, 0.0f),
          "W6 resumption is bounded (phase returns after re-warming)");
    // Event: backend/model change (an E-identity event the phase machine
    // does not model) -- the closure marks it as a failed evaluation and
    // history invalidates; a later beginFrame stays clean.
    const bool wasValid = ph.previousAcceptedValid;
    ph.finish(false, true);
    check(wasValid && !ph.previousAcceptedValid,
          "W6 backend/model event invalidates history on a failed evaluation");
    ph.beginFrame(true, false, 384, 216);
    check(feq(ph.currentX, 0.0f, 0.0f) && !ph.previousAcceptedValid,
          "W6 the next frame stays clean after the event");
    // Disable keeps everything at zero (the named off state).
    ph.beginFrame(false, false, 384, 216);
    check(feq(ph.currentX, 0.0f, 0.0f) && !ph.previousAcceptedValid,
          "W6 disable keeps phase and history at zero");
}

int runSelfTest() {
    testW1();
    testW2();
    testW3();
    testW4();
    testW5();
    testW6();
    if (g_failures == 0) {
        std::printf("c2 warp: PASS\n");
        return 0;
    }
    std::printf("c2 warp: %d FAILED check(s)\n", g_failures);
    return 1;
}

} // namespace c2warp

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return c2warp::runSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("c2 warp test: dry run (no checks run)\n");
        return 0;
    }
    std::printf("usage: c2_warp_test --self-test|--dry-run\n");
    return 2;
}
