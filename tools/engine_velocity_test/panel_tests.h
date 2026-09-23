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
//   N  a pool record with a garbage marker (not a rig record): camera term;
//   S  a stale slot (its depth is not the source's): camera term;
//   X  a corrupt (even) code: camera term, never read as another record;
//   Z  no slot at all: camera term.
// With engine.x clear every texel is the camera term (today's behaviour);
// engine.z counts the kinds per eye pixel (u1); engine.y (the motion_source
// view) puts 16 + the kind in the validity for the compose to paint.
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
inline bool toUv(const std::array<float, 276 * 4>& rows, V3 rel, double& u, double& v) {
    double c[4];
    Camera::clip(rows, rel, c);
    if (!(c[3] > 0.0)) return false;
    u = c[0] / c[3] * 0.5 + 0.5;
    v = c[1] / c[3] * -0.5 + 0.5;
    return true;
}

inline void run(const Harness& h) {
    ID3D11Device* dev = h.device;
    ID3D11DeviceContext* ctx = h.context;

    // --- the production shader, as screen_motion.cpp builds it ---------------
    const std::string source = edvr::screenMotionPsSource(edvr::kEngineMotionCoreHlsl);
    ComPtr<ID3DBlob> psCode, errors;
    HRESULT hr = D3DCompile(source.c_str(), source.size(), "screen_motion", nullptr, nullptr, "main", "ps_5_0",
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, &psCode, &errors);
    if (FAILED(hr) && errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    h.check(SUCCEEDED(hr), "panel: the production screen shader compiles with the engine-motion core in front");
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
    const Px pxA{3, 3}, pxB{3, 8}, pxC{8, 3}, pxN{8, 8}, pxS{12, 3}, pxX{12, 8}, pxZ{12, 12};
    auto surface = [&](Px p) {   // the camera-relative point under a texel (the origin is 0: absolute too)
        double nx, ny;
        texelNdc(p.x, p.y, nx, ny);
        return consumer_tests::solveRel(rowsNow, nx, ny, kZView);
    };
    std::vector<Record> pool(6);
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
    pool[0].mark(ev::kJoined);
    // B: masked (its pose is irrelevant).
    pool[1].pose(pANow, ident, 1.0f, false);
    pool[1].pose(pAPrev, ident, 1.0f, true);
    pool[1].mark(ev::kMasked);
    // C: joined, unmoved (both blocks equal): the camera term, through the record.
    const V3 pC = roundF(surface(pxC));
    pool[2].pose(pC, ident, 1.0f, false);
    pool[2].pose(pC, ident, 1.0f, true);
    pool[2].mark(ev::kJoined);
    // N: a valid pose with a garbage marker: not a rig record.
    pool[3].pose(pANow, ident, 1.0f, false);
    pool[3].pose(pAPrev, ident, 1.0f, true);
    pool[3].w[72] = 0xDEADBEEFu;

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
    // cameras here, as they are in the game's single-camera source pass.
    ComPtr<ID3D11Buffer> srcCb, oldCb, modelCb, eyeCb, settingsCb, senCb, sebCb;
    cbuffer(rowsNow.data(), 276 * 16, srcCb);
    cbuffer(rowsPrev.data(), 276 * 16, oldCb);
    cbuffer(rowsNow.data(), 276 * 16, senCb);
    cbuffer(rowsPrev.data(), 276 * 16, sebCb);
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
        d.ByteWidth = 5 * 4; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = 4;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 5;
        D3D11_BUFFER_DESC s = d;
        s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        h.check(SUCCEEDED(dev->CreateBuffer(&d, nullptr, &counts)) && SUCCEEDED(dev->CreateUnorderedAccessView(counts.Get(), &ud, &countsUav)) &&
                SUCCEEDED(dev->CreateBuffer(&s, nullptr, &countsStaging)), "panel: counts");
    }

    // --- one draw, read back --------------------------------------------------
    auto drawWith = [&](float engineOn, float paint, float count) {
        settings[8] = engineOn; settings[9] = paint; settings[10] = count;
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
        ID3D11ShaderResourceView* srvs[7] = {depthSrv.Get(), sizeSrv.Get(), nullptr, nullptr, nullptr,
                                             engineOn != 0 ? slotSrv.Get() : nullptr, engineOn != 0 ? poolSrv.Get() : nullptr};
        ctx->PSSetShaderResources(8, 7, srvs);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nulls[7] = {};
        ctx->PSSetShaderResources(8, 7, nulls);
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
        std::array<uint32_t, 5> c{};
        h.check(SUCCEEDED(ctx->Map(countsStaging.Get(), 0, D3D11_MAP_READ, 0, &m)), "panel: map counts");
        std::memcpy(c.data(), m.pData, sizeof(uint32_t) * 5);
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
    for (const Px p : {pxA, pxB, pxC, pxN, pxS, pxX, pxZ}) {
        double ex, ey;
        cameraTerm(p, ex, ey);
        allCamera = allCamera && at(off, p)[3] == 1.0f && within(at(off, p), ex, ey, 1e-3);
    }
    h.check(allCamera, "panel, engine off: every texel is the camera term (today's shader)");
    h.check(offCounts[0] + offCounts[1] + offCounts[2] + offCounts[3] + offCounts[4] == 0, "panel, engine off: nothing counted");

    // A: the record's exact previous UV (translated and turned).
    {
        double q[4];
        quatOf(yaw12, q);
        const V3 prevRel = consumer_tests::sub(consumer_tests::add(pAPrev, turn(q, vLocal)), camPrev.origin);
        double u = 0, v = 0;
        h.check(toUv(rowsPrev, prevRel, u, v), "panel A: the record's previous point is in front of last frame's camera");
        const double ex = u * kDim - (pxA.x + 0.5), ey = v * kDim - (pxA.y + 0.5);
        const auto got = at(on, pxA);
        if (!within(got, ex, ey, 1e-3))
            std::fprintf(stderr, "  panel A: GPU motion (%.6f %.6f) vs double reference (%.6f %.6f)\n", got[0], got[1], ex, ey);
        h.check(got[3] == 1.0f && within(got, ex, ey, 1e-3),
                "panel A: a moving rig record's source pixel carries its own engine motion to the previous eye pixel (1e-3 px)");
        double cx, cy;
        cameraTerm(pxA, cx, cy);
        h.check(std::fabs(ex - cx) + std::fabs(ey - cy) > 0.5,
                "panel A: and that is not the camera term the panel gave it before (the lookup changes the motion)");
        std::printf("  panel: moving record's texel: engine motion (%.3f, %.3f) px against the camera term (%.3f, %.3f) px\n",
                    got[0], got[1], cx, cy);
    }
    // B: masked -> no history.
    {
        const auto got = at(on, pxB);
        h.check(got[0] == 0.0f && got[1] == 0.0f && got[3] == 2.0f && got[2] == zr,
                "panel B: a masked rig record keeps no history (0, 0, z, 2)");
    }
    // C, N, S, X, Z: the camera term (C through the record, the rest declined).
    {
        bool ok = true;
        for (const Px p : {pxC, pxN, pxS, pxX, pxZ}) {
            double ex, ey;
            cameraTerm(p, ex, ey);
            ok = ok && at(on, p)[3] == 1.0f && within(at(on, p), ex, ey, 1e-3);
        }
        h.check(ok, "panel C/N/S/X/Z: an unmoved record, a non-rig record, a stale slot, a corrupt code and no slot all keep "
                    "the camera term");
        h.check(at(on, pxX)[0] == at(off, pxX)[0] && at(on, pxX)[1] == at(off, pxX)[1],
                "panel X: a corrupt code is declined, never read as another record");
    }
    // The counts: joined A and C, masked B, not a rig record N, stale S, corrupt X.
    h.check(onCounts[0] == 2 && onCounts[1] == 1 && onCounts[2] == 1 && onCounts[3] == 1 && onCounts[4] == 1,
            "panel: the eye-pixel counts per kind (joined 2, masked 1, not a rig record 1, stale 1, corrupt 1)");
    // The motion_source view's encoding: 16 + the source kind; no slot keeps 1.
    h.check(at(painted, pxA)[3] == 17.0f && at(painted, pxB)[3] == 18.0f && at(painted, pxC)[3] == 17.0f &&
            at(painted, pxN)[3] == 19.0f && at(painted, pxS)[3] == 20.0f && at(painted, pxX)[3] == 21.0f &&
            at(painted, pxZ)[3] == 1.0f,
            "panel: under the motion_source view the validity carries 16 + the source kind (no slot: unchanged)");
    std::printf("  panel: production screen shader on WARP -- joined exact (worst %.2e px), masked no-history, "
                "declined kinds on the camera term, counts and the view's encoding as specified\n", worst);
}

}  // namespace panel_tests
