#pragma once
// panel_tests: the on-foot engine path through the PRODUCTION screen shader
// (src/d3d11/screen_motion.h, kScreenMotionPs with the engine-motion core in
// front of it, exactly as screen_motion.cpp compiles it) on WARP.
//
// On foot the world is a flat source image shown through the 2D screen; the
// screen shader recovers each source pixel from the source depth and camera
// and reprojects it as if static. With the source pass's engine data bound
// (engine.x), a source pixel whose MRT6 slot holds a certified rig record is
// carried by the record's own two pose blocks through the source scene
// constants (this frame's and last) to its previous source UV, and the panel
// mapping takes it to the previous eye pixel. The cases (one texel each):
//   A  a MOVING joined record (translated and turned): the exact previous UV,
//      against a CPU double reference, and not the camera term;
//   B  a masked record: no history (0, 0, z, 2);
//   C  an UNMOVED joined record: the camera term, through the record;
//   H  a JOINED record stamped with an OLDER frame: the camera term (kind 6);
//   N  a pool record with a garbage marker (not a rig record): camera term;
//   S  a stale slot (its depth is not the source's): camera term;
//   X  a corrupt (even) code: camera term, never read as another record;
//   Z  no slot at all: camera term.
// With engine.x clear every texel is the camera term (today's behaviour);
// engine.z counts the kinds on the eye pixels of a grid of that stride (u1):
// 1 counts every pixel (diagnostics), kPanelSampleStride only those with both
// coordinates on its grid (the sampled count without diagnostics, flight 5);
// engine.y (the motion_source view) puts 16 + the kind in the validity for the
// compose to paint.
// F2 on foot: engine.w with the source's target 7 at t15 -- a record with a palette base (a skinned character) takes world + the source camera term + E from
// its texel (kind 7: counted as joined and in its own slot, painted as joined) or, with the texel invalid, keeps no history; without the view it is not a rig
// record. Cases K, L and R below; the shader is also run with one rule flipped at a time (the mutants at the foot of the file).
//
// The panel mapping is made trivial on purpose -- a flat screen of half-size
// 1 at the origin, an identity model and eye rows that pass x, y through --
// so the previous eye pixel is the previous source UV times the extent, and
// the source and eye rasters coincide (16 x 16).
//
// docs/kinematic-motion-injection-2026-09-19.md, 2026-09-23 "On foot".

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/d3d11/engine_velocity.h"   // kPanelSampleStride
#include "../../src/d3d11/engine_velocity_emit.h"
#include "../../src/d3d11/screen_motion.h"
#include "temporal_shader_bytecode.h"   // kEngineMotionCoreHlsl, the production text

namespace panel_tests {
using Microsoft::WRL::ComPtr;
namespace ev = edvr::engine_velocity_emit;
using consumer_tests::V3;
using consumer_tests::Camera;
using consumer_tests::Record;

struct Harness {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    void (*check)(bool, const char*) = nullptr;
};

constexpr int kDim = 16;          // source texels == eye pixels
constexpr double kZView = 4.0;    // metres, every texel

// The pool vertex shaders' quaternion decode, in double (engineQuat).
inline void quatOf(const uint16_t lanes[4], double q[4]) {
    for (int i = 0; i < 4; ++i) q[i] = double(lanes[i]) * (1.0 / 32767.0) - 1.0;
}
inline V3 turn(const double q[4], V3 v) {   // engineTurn
    const V3 u{q[0], q[1], q[2]};
    const double w = q[3];
    return consumer_tests::add(consumer_tests::add(consumer_tests::mul(v, 2.0 * w * w - 1.0),
                                                   consumer_tests::mul(u, 2.0 * consumer_tests::dot(u, v))),
                               consumer_tests::mul(consumer_tests::cross(u, v), 2.0 * w));
}
inline V3 roundF(V3 v) { return {double(float(v.x)), double(float(v.y)), double(float(v.z))}; }

// Texel centre -> the shader's NDC (uv * (2, -2) + (-1, 1)).
inline void texelNdc(int x, int y, double& nx, double& ny) {
    nx = (x + 0.5) / kDim * 2.0 - 1.0;
    ny = 1.0 - (y + 0.5) / kDim * 2.0;
}
// A relative (camera-origin) point through rows -> source UV.
inline bool toUv(const std::array<float, 277 * 4>& rows, V3 rel, double& u, double& v) {
    double c[4];
    Camera::clip(rows, rel, c);
    if (!(c[3] > 0.0)) return false;
    u = c[0] / c[3] * 0.5 + 0.5;
    v = c[1] / c[3] * -0.5 + 0.5;
    return true;
}

// Runs every case against `source` (the production screen shader, or one with a rule flipped: `quiet` then keeps the run's lines off the log). False when the
// shader does not compile.
inline bool runWith(const Harness& h, const std::string& source, bool quiet) {
    ID3D11Device* dev = h.device;
    ID3D11DeviceContext* ctx = h.context;

    // --- the production shader, as screen_motion.cpp builds it ---------------
    ComPtr<ID3DBlob> psCode, errors;
    HRESULT hr = D3DCompile(source.c_str(), source.size(), "screen_motion", nullptr, nullptr, "main", "ps_5_0",
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, &psCode, &errors);
    if (FAILED(hr) && errors && !quiet) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    if (FAILED(hr)) { if (!quiet) h.check(false, "panel: the production screen shader compiles with the engine-motion core in front"); return false; }
    if (!quiet) h.check(true, "panel: the production screen shader compiles with the engine-motion core in front");
    const char* vsText = R"HLSL(
struct O { float2 t : __USER_VERTEX_M_TEXCOORD0; float4 p : SV_Position; };
O main(uint id : SV_VertexID) { O o; float2 p = float2((id << 1) & 2, id & 2); o.p = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.t = p; return o; }
)HLSL";
    ComPtr<ID3DBlob> vsCode;
    h.check(SUCCEEDED(D3DCompile(vsText, std::strlen(vsText), "panel_vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsCode, nullptr)),
            "panel: test VS");
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11VertexShader> vs;
    h.check(SUCCEEDED(dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps)), "panel: PS on WARP");
    h.check(SUCCEEDED(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)), "panel: VS on WARP");

    // --- the source camera pair and the records ------------------------------
    const Camera camNow = consumer_tests::camera(0.0, {0.0, 0.0, 0.0}, 0.0, 0.0);
    const Camera camPrev = consumer_tests::camera(1.5, {0.05, 0.0, 0.02}, 0.0, 0.0);
    const auto rowsNow = camNow.rows(), rowsPrev = camPrev.rows();
    const float zr = float(0.025 / kZView);
    struct Px { int x, y; };
    const Px pxA{3, 3}, pxB{3, 8}, pxC{8, 3}, pxN{8, 8}, pxS{12, 3}, pxX{12, 8}, pxZ{12, 12}, pxH{3, 12};
    auto surface = [&](Px p) {   // the camera-relative point under a texel (the origin is 0: absolute too)
        double nx, ny;
        texelNdc(p.x, p.y, nx, ny);
        return consumer_tests::solveRel(rowsNow, nx, ny, kZView);
    };
    std::vector<Record> pool(7);
    const uint16_t* ident = consumer_tests::identLanes();
    // A: moving -- translated and turned 12 degrees about y; the surface point
    // is 0.3/-0.2/0.1 from the record's origin, so the turn moves it.
    const double half = 6.0 * 3.14159265358979323846 / 180.0;
    const uint16_t yaw12[4] = {32767, uint16_t(std::lround((1.0 + std::sin(half)) * 32767.0)), 32767,
                               uint16_t(std::lround((1.0 + std::cos(half)) * 32767.0))};
    const V3 vLocal{0.3, -0.2, 0.1};
    const V3 pANow = roundF(consumer_tests::sub(surface(pxA), vLocal));
    const V3 pAPrev = roundF(consumer_tests::add(pANow, {-0.6, 0.1, 0.2}));
    pool[0].pose(pANow, ident, 1.0f, false);
    pool[0].pose(pAPrev, yaw12, 1.0f, true);
    pool[0].mark(ev::kJoined, math_tests::kStampFrame);
    // B: masked (its pose is irrelevant).
    pool[1].pose(pANow, ident, 1.0f, false);
    pool[1].pose(pAPrev, ident, 1.0f, true);
    pool[1].mark(ev::kMasked, math_tests::kStampFrame);
    // C: joined, unmoved (both blocks equal): the camera term, through the record.
    const V3 pC = roundF(surface(pxC));
    pool[2].pose(pC, ident, 1.0f, false);
    pool[2].pose(pC, ident, 1.0f, true);
    pool[2].mark(ev::kJoined, math_tests::kStampFrame);
    // N: a valid pose with a garbage marker: not a rig record.
    pool[3].pose(pANow, ident, 1.0f, false);
    pool[3].pose(pAPrev, ident, 1.0f, true);
    pool[3].w[72] = 0xDEADBEEFu;
    // H: the moving joined record of case A, but its marker folds an older
    // frame's stamp: the cull case declines to the camera term (kind 6).
    pool[4] = pool[0];
    pool[4].mark(ev::kJoined, math_tests::kStampFrame - 1);
    // K/L (F2 on foot): a skinned character's record: a nonzero palette base, no pose blocks and no marker.
    pool[5].w[0] = 7u;

    // --- resources ------------------------------------------------------------
    auto texture = [&](DXGI_FORMAT fmt, UINT bind, const void* init, UINT pitch, ComPtr<ID3D11Texture2D>& t) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kDim; d.Height = kDim; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1;
        d.Format = fmt; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA s{init, pitch, 0};
        h.check(SUCCEEDED(dev->CreateTexture2D(&d, init ? &s : nullptr, &t)), "panel: texture");
    };
    auto cbuffer = [&](const void* data, UINT bytes, ComPtr<ID3D11Buffer>& b) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = bytes; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA s{data, 0, 0};
        h.check(SUCCEEDED(dev->CreateBuffer(&d, &s, &b)), "panel: constant buffer");
    };
    std::vector<float> depth(size_t(kDim) * kDim, zr);
    std::vector<float> slots(size_t(kDim) * kDim * 2, 0.0f);
    for (size_t i = 0; i < size_t(kDim) * kDim; ++i) slots[i * 2] = -1.0f;   // cleared
    auto setSlot = [&](Px p, float code, float z) { const size_t i = size_t(p.y) * kDim + p.x; slots[i * 2] = code; slots[i * 2 + 1] = z; };
    setSlot(pxA, 1.0f, zr);          // 2*0+1
    setSlot(pxB, 3.0f, zr);          // 2*1+1
    setSlot(pxC, 5.0f, zr);          // 2*2+1
    setSlot(pxN, 7.0f, zr);          // 2*3+1
    setSlot(pxH, 9.0f, zr);          // 2*4+1: joined, but stamped with an older frame
    setSlot(pxS, 1.0f, zr * 1.5f);   // a valid code whose depth is not the source's
    setSlot(pxX, 6.0f, zr);          // even: corrupt
    ComPtr<ID3D11Texture2D> depthTex, slotTex, target;
    texture(DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depth.data(), kDim * 4, depthTex);
    texture(DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, slots.data(), kDim * 8, slotTex);
    texture(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET, nullptr, 0, target);
    ComPtr<ID3D11ShaderResourceView> depthSrv, slotSrv, poolSrv, sizeSrv;
    ComPtr<ID3D11RenderTargetView> rtv;
    h.check(SUCCEEDED(dev->CreateShaderResourceView(depthTex.Get(), nullptr, &depthSrv)) &&
            SUCCEEDED(dev->CreateShaderResourceView(slotTex.Get(), nullptr, &slotSrv)) &&
            SUCCEEDED(dev->CreateRenderTargetView(target.Get(), nullptr, &rtv)), "panel: views");
    {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = UINT(pool.size() * sizeof(Record)); d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = sizeof(Record);
        D3D11_SUBRESOURCE_DATA s{pool.data(), 0, 0};
        ComPtr<ID3D11Buffer> b;
        h.check(SUCCEEDED(dev->CreateBuffer(&d, &s, &b)) && SUCCEEDED(dev->CreateShaderResourceView(b.Get(), nullptr, &poolSrv)),
                "panel: pool snapshot");
        const float sizes[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        D3D11_BUFFER_DESC r{};
        r.ByteWidth = 16; r.Usage = D3D11_USAGE_DEFAULT; r.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        r.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_SUBRESOURCE_DATA rs{sizes, 0, 0};
        ComPtr<ID3D11Buffer> rb;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS; sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.NumElements = 4; sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        h.check(SUCCEEDED(dev->CreateBuffer(&r, &rs, &rb)) && SUCCEEDED(dev->CreateShaderResourceView(rb.Get(), &sd, &sizeSrv)),
                "panel: screen size");
    }
    // The camera-term rows (b2/b3) and the engine rows (b7/b8) are the same
    // cameras here, as they are in the game's single-camera source pass. The
    // arrays carry a 277th float4: the freshness stamp at SEN[276].x.
    ComPtr<ID3D11Buffer> srcCb, oldCb, modelCb, eyeCb, settingsCb, senCb, sebCb;
    cbuffer(rowsNow.data(), 277 * 16, srcCb);
    cbuffer(rowsPrev.data(), 277 * 16, oldCb);
    cbuffer(rowsNow.data(), 277 * 16, senCb);
    cbuffer(rowsPrev.data(), 277 * 16, sebCb);
    std::vector<float> model(12 * 4, 0.0f), eye(274 * 4, 0.0f);
    model[9 * 4 + 0] = 1.0f; model[10 * 4 + 1] = 1.0f; model[11 * 4 + 2] = 1.0f;
    eye[270 * 4 + 0] = 1.0f; eye[271 * 4 + 1] = 1.0f; eye[273 * 4 + 3] = 1.0f;
    cbuffer(model.data(), 12 * 16, modelCb);
    cbuffer(eye.data(), 274 * 16, eyeCb);
    float settings[12] = {0, 0, 0, 0, float(kDim), float(kDim), 0, 0, 0, 0, 0, 0};
    cbuffer(settings, sizeof(settings), settingsCb);
    ComPtr<ID3D11Buffer> counts, countsStaging;
    ComPtr<ID3D11UnorderedAccessView> countsUav;
    {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = 7 * 4; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = 4;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 7;
        D3D11_BUFFER_DESC s = d;
        s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        h.check(SUCCEEDED(dev->CreateBuffer(&d, nullptr, &counts)) && SUCCEEDED(dev->CreateUnorderedAccessView(counts.Get(), &ud, &countsUav)) &&
                SUCCEEDED(dev->CreateBuffer(&s, nullptr, &countsStaging)), "panel: counts");
    }

    // --- one draw, read back --------------------------------------------------
    auto drawWith = [&](float engineOn, float paint, float count, ID3D11ShaderResourceView* slotsOverride = nullptr, ID3D11ShaderResourceView* skinView = nullptr) {
        settings[8] = engineOn; settings[9] = paint; settings[10] = count; settings[11] = skinView ? 1.0f : 0.0f;
        ctx->UpdateSubresource(settingsCb.Get(), 0, nullptr, settings, 0, 0);
        const UINT zeros[4] = {};
        ctx->ClearUnorderedAccessViewUint(countsUav.Get(), zeros);
        const float black[4] = {};
        ctx->ClearRenderTargetView(rtv.Get(), black);
        ID3D11UnorderedAccessView* uavs[1] = {countsUav.Get()};
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, rtv.GetAddressOf(), nullptr, 1, 1, uavs, nullptr);
        D3D11_VIEWPORT vp{0, 0, float(kDim), float(kDim), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11Buffer* cbs[7] = {srcCb.Get(), oldCb.Get(), modelCb.Get(), eyeCb.Get(), settingsCb.Get(), senCb.Get(), sebCb.Get()};
        ctx->PSSetConstantBuffers(2, 7, cbs);
        ID3D11ShaderResourceView* srvs[8] = {depthSrv.Get(), sizeSrv.Get(), nullptr, nullptr, nullptr,
                                             engineOn != 0 ? (slotsOverride ? slotsOverride : slotSrv.Get()) : nullptr, engineOn != 0 ? poolSrv.Get() : nullptr,
                                             engineOn != 0 ? skinView : nullptr};
        ctx->PSSetShaderResources(8, 8, srvs);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nulls[8] = {};
        ctx->PSSetShaderResources(8, 8, nulls);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);   // unbinds the UAV too
        D3D11_TEXTURE2D_DESC d{};
        target->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st;
        h.check(SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &st)), "panel: staging");
        ctx->CopyResource(st.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        h.check(SUCCEEDED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m)), "panel: map");
        std::vector<float> out(size_t(kDim) * kDim * 4);
        for (int y = 0; y < kDim; ++y)
            std::memcpy(&out[size_t(y) * kDim * 4], static_cast<const BYTE*>(m.pData) + y * m.RowPitch, kDim * 16);
        ctx->Unmap(st.Get(), 0);
        return out;
    };
    auto readCounts = [&]() {
        ctx->CopyResource(countsStaging.Get(), counts.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        std::array<uint32_t, 7> c{};
        h.check(SUCCEEDED(ctx->Map(countsStaging.Get(), 0, D3D11_MAP_READ, 0, &m)), "panel: map counts");
        std::memcpy(c.data(), m.pData, sizeof(uint32_t) * 7);
        ctx->Unmap(countsStaging.Get(), 0);
        return c;
    };
    auto at = [&](const std::vector<float>& a, Px p) {
        const size_t i = (size_t(p.y) * kDim + p.x) * 4;
        return std::array<float, 4>{a[i], a[i + 1], a[i + 2], a[i + 3]};
    };

    const auto off = drawWith(0, 0, 1);
    const auto offCounts = readCounts();
    const auto on = drawWith(1, 0, 1);
    const auto onCounts = readCounts();
    const auto painted = drawWith(1, 1, 0);
    const auto sampled = drawWith(1, 0, float(edvr::kPanelSampleStride));
    const auto sampledCounts = readCounts();

    // --- the CPU references -----------------------------------------------------
    // The camera term at a texel: the surface point, the camera's move, last
    // frame's rows -- as the shader's own reprojection does it.
    auto cameraTerm = [&](Px p, double& mx, double& my) {
        const V3 rel = consumer_tests::add(surface(p), consumer_tests::sub(camNow.origin, camPrev.origin));
        double u = 0, v = 0;
        h.check(toUv(rowsPrev, rel, u, v), "panel: the camera term reprojects in front");
        mx = u * kDim - (p.x + 0.5);
        my = v * kDim - (p.y + 0.5);
    };
    double worst = 0.0;
    auto within = [&](const std::array<float, 4>& got, double ex, double ey, double tol) {
        const double e = std::max(std::fabs(got[0] - ex), std::fabs(got[1] - ey));
        worst = std::max(worst, e);
        return e <= tol;
    };

    // Engine off: every texel is today's camera term; nothing is counted.
    bool allCamera = true;
    for (const Px p : {pxA, pxB, pxC, pxN, pxS, pxX, pxZ, pxH}) {
        double ex, ey;
        cameraTerm(p, ex, ey);
        allCamera = allCamera && at(off, p)[3] == 1.0f && within(at(off, p), ex, ey, 1e-3);
    }
    h.check(allCamera, "panel, engine off: every texel is the camera term (today's shader)");
    h.check(offCounts[0] + offCounts[1] + offCounts[2] + offCounts[3] + offCounts[4] + offCounts[5] == 0,
            "panel, engine off: nothing counted");

    // A: the record's exact previous UV (translated and turned).
    {
        double q[4];
        quatOf(yaw12, q);
        const V3 prevRel = consumer_tests::sub(consumer_tests::add(pAPrev, turn(q, vLocal)), camPrev.origin);
        double u = 0, v = 0;
        h.check(toUv(rowsPrev, prevRel, u, v), "panel A: the record's previous point is in front of last frame's camera");
        const double ex = u * kDim - (pxA.x + 0.5), ey = v * kDim - (pxA.y + 0.5);
        const auto got = at(on, pxA);
        if (!quiet && !within(got, ex, ey, 1e-3))
            std::fprintf(stderr, "  panel A: GPU motion (%.6f %.6f) vs double reference (%.6f %.6f)\n", got[0], got[1], ex, ey);
        h.check(got[3] == 1.0f && within(got, ex, ey, 1e-3),
                "panel A: a moving rig record's source pixel carries its own engine motion to the previous eye pixel (1e-3 px)");
        double cx, cy;
        cameraTerm(pxA, cx, cy);
        h.check(std::fabs(ex - cx) + std::fabs(ey - cy) > 0.5,
                "panel A: and that is not the camera term the panel gave it before (the lookup changes the motion)");
        if (!quiet)
            std::printf("  panel: moving record's texel: engine motion (%.3f, %.3f) px against the camera term (%.3f, %.3f) px\n",
                        got[0], got[1], cx, cy);
    }
    // B: masked -> no history.
    {
        const auto got = at(on, pxB);
        h.check(got[0] == 0.0f && got[1] == 0.0f && got[3] == 2.0f && got[2] == zr,
                "panel B: a masked rig record keeps no history (0, 0, z, 2)");
    }
    // C, H, N, S, X, Z: the camera term (C through the record, the rest declined).
    {
        bool ok = true;
        for (const Px p : {pxC, pxH, pxN, pxS, pxX, pxZ}) {
            double ex, ey;
            cameraTerm(p, ex, ey);
            ok = ok && at(on, p)[3] == 1.0f && within(at(on, p), ex, ey, 1e-3);
        }
        h.check(ok, "panel C/H/N/S/X/Z: an unmoved record, an older-frame-stamped record, a non-rig record, a stale "
                    "slot, a corrupt code and no slot all keep the camera term");
        h.check(at(on, pxX)[0] == at(off, pxX)[0] && at(on, pxX)[1] == at(off, pxX)[1],
                "panel X: a corrupt code is declined, never read as another record");
    }
    // The counts: joined A and C, masked B, not a rig record N, stale S, corrupt X,
    // stale stamp H.
    h.check(onCounts[0] == 2 && onCounts[1] == 1 && onCounts[2] == 1 && onCounts[3] == 1 && onCounts[4] == 1 &&
            onCounts[5] == 1,
            "panel: the eye-pixel counts per kind (joined 2, masked 1, not a rig record 1, stale 1, corrupt 1, "
            "stale stamp 1)");
    // Sampled (engine.z = kPanelSampleStride, 4): only eye pixels with both
    // coordinates on the 4 x 4 grid count -- N (8,8) and X (12,8); A, B, C, H
    // and S lie off it and Z has no slot -- and the motion itself is unchanged.
    h.check(sampledCounts[0] == 0 && sampledCounts[1] == 0 && sampledCounts[2] == 1 && sampledCounts[3] == 0 &&
            sampledCounts[4] == 1 && sampledCounts[5] == 0,
            "panel: sampled counting counts only the eye pixels on its grid (not a rig record 1 at N, corrupt 1 at X)");
    {
        bool same = true;
        for (size_t i = 0; i < on.size(); ++i) same = same && on[i] == sampled[i];
        h.check(same, "panel: sampling changes the counts only, never the motion written");
    }
    // The motion_source view's encoding: 16 + the source kind; no slot keeps 1.
    h.check(at(painted, pxA)[3] == 17.0f && at(painted, pxB)[3] == 18.0f && at(painted, pxC)[3] == 17.0f &&
            at(painted, pxN)[3] == 19.0f && at(painted, pxS)[3] == 20.0f && at(painted, pxX)[3] == 21.0f &&
            at(painted, pxH)[3] == 22.0f && at(painted, pxZ)[3] == 1.0f,
            "panel: under the motion_source view the validity carries 16 + the source kind (no slot: unchanged)");

    // ---- F2 on foot: a skinned character's exact motion from the source's target 7 (engine.w, t15) -----------------------------------
    // The same source pass and the same pool, with three more pixels: K a skinned record (a nonzero palette base, no pose blocks, no marker) whose target-7 texel
    // is valid with E = (-80, 30, 50) cm (the texel's interpolated flag 0.9995117); L the same record whose texel has no valid flag; R the MOVING joined record
    // of case A at another pixel, with every texel of target 7 invalid. With the skin view bound K takes world + the source camera term + E, L keeps no history,
    // R is exactly what it is without the view. Without the view (the flat profile's frame, which has none) K and L are not rig records: the camera term.
    {
        const Px pxK{6, 6}, pxL{6, 10}, pxR{10, 6}, pxP{10, 10};
        std::vector<float> slots2 = slots;
        auto put2 = [&](Px p, float code, float z) { const size_t i = size_t(p.y) * kDim + p.x; slots2[i * 2] = code; slots2[i * 2 + 1] = z; };
        put2(pxK, 11.0f, zr);   // 2*5+1: the skinned record
        put2(pxL, 11.0f, zr);
        put2(pxR, 1.0f, zr);    // A's moving joined record
        put2(pxP, 11.0f, zr);   // the skinned record again, with an infinite E
        ComPtr<ID3D11Texture2D> slotTex2, skinTex;
        texture(DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, slots2.data(), kDim * 8, slotTex2);
        std::vector<uint16_t> skin(size_t(kDim) * kDim * 4, 0);
        auto toHalf = [](float f) {
            uint32_t x; std::memcpy(&x, &f, 4);
            const uint32_t sign = (x >> 16) & 0x8000u;
            int32_t exp = int32_t((x >> 23) & 0xFF) - 127 + 15;
            const uint32_t mant = x & 0x007FFFFFu;
            if (exp <= 0) return uint16_t(sign);
            if (exp >= 31) return uint16_t(sign | 0x7C00u);
            uint32_t hv = (uint32_t(exp) << 10) | (mant >> 13);
            const uint32_t rem = mant & 0x1FFFu;
            if (rem > 0x1000u || (rem == 0x1000u && (hv & 1u))) ++hv;
            return uint16_t(sign | hv);
        };
        const float ek[3] = {-80.0f, 30.0f, 50.0f};
        {
            uint16_t* t = &skin[(size_t(pxK.y) * kDim + pxK.x) * 4];
            t[0] = toHalf(ek[0]); t[1] = toHalf(ek[1]); t[2] = toHalf(ek[2]); t[3] = toHalf(0.99951171875f);
            uint16_t* u = &skin[(size_t(pxL.y) * kDim + pxL.x) * 4];
            u[0] = toHalf(ek[0]); u[1] = toHalf(ek[1]); u[2] = toHalf(ek[2]); u[3] = 0;   // E set, no valid flag
            uint16_t* q = &skin[(size_t(pxP.y) * kDim + pxP.x) * 4];
            q[0] = 0x7C00; q[1] = 0; q[2] = 0; q[3] = toHalf(1.0f);   // E infinite, valid flag set
        }
        texture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, skin.data(), kDim * 8, skinTex);
        ComPtr<ID3D11ShaderResourceView> slotSrv2, skinSrv;
        h.check(SUCCEEDED(dev->CreateShaderResourceView(slotTex2.Get(), nullptr, &slotSrv2)) &&
                SUCCEEDED(dev->CreateShaderResourceView(skinTex.Get(), nullptr, &skinSrv)), "panel skin: views");
        const auto without = drawWith(1, 0, 1, slotSrv2.Get(), nullptr);
        const auto withoutCounts = readCounts();
        const auto bound = drawWith(1, 0, 1, slotSrv2.Get(), skinSrv.Get());
        const auto boundCounts = readCounts();
        const auto boundPainted = drawWith(1, 1, 0, slotSrv2.Get(), skinSrv.Get());
        // K's reference: the surface point under the texel, this frame's camera against last frame's, plus E (centimetres) moved by the surface itself.
        {
            const V3 rel = consumer_tests::add(consumer_tests::add(surface(pxK), consumer_tests::sub(camNow.origin, camPrev.origin)),
                                               V3{ek[0] * 0.01, ek[1] * 0.01, ek[2] * 0.01});
            double u = 0, v = 0;
            h.check(toUv(rowsPrev, rel, u, v), "panel K: the skinned surface's previous point is in front of last frame's camera");
            const double ex = u * kDim - (pxK.x + 0.5), ey = v * kDim - (pxK.y + 0.5);
            const auto got = at(bound, pxK);
            if (!quiet && !within(got, ex, ey, 2e-3))
                std::fprintf(stderr, "  panel K: GPU motion (%.6f %.6f) vs double reference (%.6f %.6f)\n", got[0], got[1], ex, ey);
            h.check(got[3] == 1.0f && within(got, ex, ey, 2e-3),
                    "panel K: a skinned character's source pixel carries its exact motion (world + the source camera term + E) to the previous eye pixel (2e-3 px)");
            double cx, cy;
            cameraTerm(pxK, cx, cy);
            h.check(std::fabs(ex - cx) + std::fabs(ey - cy) > 0.3, "panel K: and that is not the camera term (E moved it)");
        }
        h.check(at(bound, pxL)[0] == 0.0f && at(bound, pxL)[1] == 0.0f && at(bound, pxL)[3] == 2.0f && at(bound, pxL)[2] == zr,
                "panel L: a skinned pixel whose target-7 texel has no valid flag keeps no history (0, 0, z, 2)");
        // (the explicit finiteness test is the earlier exit: the reprojection refuses a non-finite point and answers 2 as well, so this case is a behaviour, not a discriminator)
        h.check(at(bound, pxP)[0] == 0.0f && at(bound, pxP)[1] == 0.0f && at(bound, pxP)[3] == 2.0f && at(bound, pxP)[2] == zr,
                "panel P: a skinned pixel whose E is infinite keeps no history (0, 0, z, 2)");
        {
            const auto a = at(bound, pxR), b = at(without, pxR);
            h.check(a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3] && a[3] == 1.0f,
                    "panel R: a RIGID joined record is the same with the skin view bound as without it (target 7 is for a palette base only), whatever its texels say");
        }
        {
            bool same = true;
            for (const Px p : {pxA, pxB, pxC, pxN, pxS, pxX, pxZ, pxH}) {
                const auto a = at(bound, p), b = at(without, p);
                same = same && a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
            }
            h.check(same, "panel: every pixel of the earlier cases is the same with the skin view bound as without it");
        }
        for (const Px p : {pxK, pxL, pxP}) {
            double ex, ey;
            cameraTerm(p, ex, ey);
            h.check(at(without, p)[3] == 1.0f && within(at(without, p), ex, ey, 1e-3),
                    "panel K/L: with no skin view (the flat profile's frame) a record with a palette base and no marker is not a rig record: the camera term, as before");
        }
        // joined: A, C, R; K counts as joined too; masked: B, L; the others as in the earlier cases; skinned (inside joined): K
        h.check(withoutCounts[0] == 3 && withoutCounts[1] == 1 && withoutCounts[2] == 4 && withoutCounts[3] == 1 && withoutCounts[4] == 1 &&
                    withoutCounts[5] == 1 && withoutCounts[6] == 0,
                "panel, no skin view: the counts are joined 3 (A, C and R), masked 1, not a rig record 4 (N, K, L and P), stale 1, corrupt 1, stale stamp 1, skinned 0");
        h.check(boundCounts[0] == 4 && boundCounts[1] == 3 && boundCounts[2] == 1 && boundCounts[3] == 1 && boundCounts[4] == 1 && boundCounts[5] == 1 &&
                    boundCounts[6] == 1,
                "panel, skin view bound: K is counted as joined AND in its own slot (joined 4, masked 3 with L and P, not a rig record 1, skinned 1)");
        h.check(at(boundPainted, pxK)[3] == 17.0f && at(boundPainted, pxL)[3] == 18.0f && at(boundPainted, pxR)[3] == 17.0f,
                "panel: under the motion_source view a skinned pixel with a valid texel paints as joined (17), one without keeps no history (18)");
    }
    if (!quiet)
        std::printf("  panel: production screen shader on WARP -- joined exact (worst %.2e px), masked no-history, "
                    "declined kinds on the camera term, skinned characters from target 7, counts and the view's encoding as specified\n", worst);
    return true;
}

// The production shader, then the same cases against it with one rule flipped at a time: each flip must fail a check.
inline void run(const Harness& h) {
    const std::string production = edvr::screenMotionPsSource(edvr::kEngineMotionCoreHlsl);
    runWith(h, production, false);
    struct Mutant { const char* name; const char* from; const char* to; };
    static const Mutant mutants[] = {
        {"the skin view's flag is ignored (no view: a skinned record reads an unbound texture)", "if(engine.w!=0 && r.data[0].x!=0u) {", "if(r.data[0].x!=0u) {"},
        {"a rigid record takes the skinned branch", "if(engine.w!=0 && r.data[0].x!=0u) {", "if(engine.w!=0) {"},
        {"a texel without the valid flag is accepted", "if(!(sk.w>0.5) || !all(isfinite(sk.xyz)))return 2u;", "if(!all(isfinite(sk.xyz)))return 2u;"},
        {"E is taken as metres", "engineReprojectRowsE(r,true,sk.xyz*0.01,", "engineReprojectRowsE(r,true,sk.xyz,"},
        {"E is previous - current the wrong way round", "engineReprojectRowsE(r,true,sk.xyz*0.01,", "engineReprojectRowsE(r,true,-sk.xyz*0.01,"},
        {"the pose blocks decide, not E", "engineReprojectRowsE(r,true,sk.xyz*0.01,", "engineReprojectRowsE(r,false,sk.xyz*0.01,"},
        {"the previous position is not carried to the panel", "if(sk==1u || sk==7u) {", "if(sk==1u) {"},
        {"a skinned pixel is painted with its own kind", "16.0+(kind==7u?1u:kind)", "16.0+kind"},
        {"a skinned pixel is not counted as joined", "PanelCounts[sk==7u?0u:sk-1u]", "PanelCounts[sk==7u?5u:sk-1u]"},
        {"a skinned pixel is not counted in its own slot", "if(sk==7u)InterlockedAdd(PanelCounts[6],1u);", ""},
        {"a masked skinned pixel is called joined", "if(!(sk.w>0.5) || !all(isfinite(sk.xyz)))return 2u;", "if(!(sk.w>0.5) || !all(isfinite(sk.xyz)))return 1u;"},
    };
    int caught = 0;
    for (const Mutant& m : mutants) {
        const size_t at = production.find(m.from);
        char what[256];
        std::snprintf(what, sizeof(what), "panel mutations: \"%.100s\": its anchor is in the shader exactly once", m.name);
        h.check(at != std::string::npos && production.find(m.from, at + 1) == std::string::npos, what);
        if (at == std::string::npos) continue;
        std::string mutated = production;
        mutated.replace(at, std::strlen(m.from), m.to);
        int failed = 0;
        std::string first;
        struct Count { int* failed; std::string* first; };
        static thread_local Count* active = nullptr;
        Count count{&failed, &first};
        active = &count;
        Harness quiet{h.device, h.context, [](bool ok, const char* text) { if (!ok) { ++*active->failed; if (active->first->empty()) *active->first = text; } }};
        const bool compiled = runWith(quiet, mutated, true);
        active = nullptr;
        std::snprintf(what, sizeof(what), "panel mutations: \"%.100s\" compiles", m.name);
        h.check(compiled, what);
        std::snprintf(what, sizeof(what), "panel mutations: \"%.100s\" is caught by the cases", m.name);
        h.check(failed > 0, what);
        if (failed > 0) ++caught;
        std::printf("  panel mutation \"%s\": %d checks fail; first: %s\n", m.name, failed, first.c_str());
    }
    std::printf("  panel: %d of %zu mutations caught\n", caught, sizeof(mutants) / sizeof(mutants[0]));
}

}  // namespace panel_tests
