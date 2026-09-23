#pragma once
// engine_velocity_test: the compose's arithmetic, run on WARP from the SAME
// text the temporal pass compiles -- the ENGINE_MOTION_HLSL block of
// src/d3d11/temporal_shader_source.h, cut out between its marker lines at run
// time (the rig runs from the repository root, as every rig does) -- against
// a double-precision reference built the way the pool vertex shaders build a
// vertex: world - origin = (pos - origin) + scale * turn(q, v), clip = rows,
// q decoded lane/32767 - 1 (the VS's own immediate). Cases: a still record
// (the camera term exactly), a landing-ship-like mover 540 m out, a scaled
// part, a quantised 90-degree turn (a non-unit quaternion the adjugate must
// invert exactly), the markers (joined, masked, garbage, a swapped hash),
// behind-the-camera and a foreign projection encoding.

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/d3d11/engine_velocity_emit.h"

namespace math_tests {
using Microsoft::WRL::ComPtr;
namespace ev = edvr::engine_velocity_emit;

struct Harness {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    void (*check)(bool, const char*) = nullptr;
};

struct V3 { double x, y, z; };
inline V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 mul(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
struct Q { double x, y, z, w; };
inline V3 turn(Q q, V3 v) {   // the VS's own: (2w^2-1)v + 2(q.v)q + 2w(q x v)
    const V3 qv{q.x, q.y, q.z};
    return add(add(mul(v, 2 * q.w * q.w - 1), mul(qv, 2 * dot(qv, v))), mul(cross(qv, v), 2 * q.w));
}
inline Q axisAngle(V3 axis, double deg) {
    const double n = std::sqrt(dot(axis, axis)), h = deg * 3.14159265358979323846 / 360.0;
    return {axis.x / n * std::sin(h), axis.y / n * std::sin(h), axis.z / n * std::sin(h), std::cos(h)};
}
inline uint16_t lane(double c) { return static_cast<uint16_t>(std::lround((c + 1.0) * 32767.0)); }
inline Q decode(const uint16_t l[4]) {   // exactly as the VS: float(lane) * (1/32767) - 1
    const float s = 1.0f / 32767.0f;
    return {double(float(l[0]) * s - 1.0f), double(float(l[1]) * s - 1.0f), double(float(l[2]) * s - 1.0f), double(float(l[3]) * s - 1.0f)};
}

// A camera: clip rows 270..273 and origin 275 as the game fills cb1.
struct Camera {
    double rot[3][3];   // view = rot * (world - origin) + t
    V3 t;               // t.z must be 0: the game's rows have 273.w == 0
    V3 origin;
    double fx, fy, nearZ, jx, jy;
    std::array<float, 276 * 4> rows() const {
        std::array<float, 276 * 4> cb{};
        auto clipLin = [&](V3 v, float* out) {   // linear part: x, y, (z=0), w
            const double w = -v.z;
            out[0] = float(fx * v.x + jx * w);
            out[1] = float(fy * v.y + jy * w);
            out[2] = 0.0f;
            out[3] = float(w);
        };
        for (int c = 0; c < 3; ++c) clipLin({rot[0][c], rot[1][c], rot[2][c]}, &cb[(270 + c) * 4]);
        clipLin(t, &cb[273 * 4]);
        cb[273 * 4 + 2] = float(nearZ);
        cb[275 * 4 + 0] = float(origin.x);
        cb[275 * 4 + 1] = float(origin.y);
        cb[275 * 4 + 2] = float(origin.z);
        return cb;
    }
    // Project a camera-relative point (world - origin) through the FLOAT rows,
    // in double, as the GPU would multiply them.
    static void clip(const std::array<float, 276 * 4>& cb, V3 rel, double out[4]) {
        for (int k = 0; k < 4; ++k)
            out[k] = rel.x * cb[270 * 4 + k] + rel.y * cb[271 * 4 + k] + rel.z * cb[272 * 4 + k] + cb[273 * 4 + k];
    }
};

inline Camera camera(double yawDeg, V3 origin, double jx, double jy) {
    Camera c{};
    const double a = yawDeg * 3.14159265358979323846 / 180.0;
    const double r[3][3] = {{std::cos(a), 0, -std::sin(a)}, {0, 1, 0}, {std::sin(a), 0, std::cos(a)}};
    std::memcpy(c.rot, r, sizeof(r));
    c.t = {0.03, -0.01, 0.0};
    c.origin = origin;
    c.fx = 1.1; c.fy = 1.0; c.nearZ = 0.025; c.jx = jx; c.jy = jy;
    return c;
}

struct Record {
    uint32_t w[84] = {};
    void pose(V3 pos, const uint16_t q[4], float scale, bool prevBlock) {
        const float p[3] = {float(pos.x), float(pos.y), float(pos.z)};
        const uint32_t lo = uint32_t(q[0]) | (uint32_t(q[1]) << 16), hi = uint32_t(q[2]) | (uint32_t(q[3]) << 16);
        if (!prevBlock) { std::memcpy(&w[1], &scale, 4); w[2] = lo; w[3] = hi; std::memcpy(&w[4], p, 12); }
        else { std::memcpy(&w[77], &scale, 4); w[78] = lo; w[79] = hi; std::memcpy(&w[73], p, 12); }
    }
    ev::Pose block(bool prev) const {
        ev::Pose b;
        if (!prev) { b.w[0] = w[4]; b.w[1] = w[5]; b.w[2] = w[6]; b.w[3] = w[2]; b.w[4] = w[3]; }
        else { b.w[0] = w[73]; b.w[1] = w[74]; b.w[2] = w[75]; b.w[3] = w[78]; b.w[4] = w[79]; }
        return b;
    }
    void mark(uint32_t tag) { w[72] = tag ^ ev::markerHash(block(false), block(true)); }
};
static_assert(sizeof(Record) == 336, "t33 stride");

struct Case { float ndc[2]; float zr; uint32_t slot; };
struct Expect { double ndc[2]; bool valid; uint32_t kind; const char* what; };

// The ENGINE_MOTION_HLSL block, verbatim, from the compose's own source.
inline std::string engineMotionBlock(const Harness& h) {
    std::ifstream file("src\\d3d11\\temporal_shader_source.h", std::ios::binary);
    h.check(static_cast<bool>(file), "src\\d3d11\\temporal_shader_source.h readable (run from the repository root)");
    std::stringstream text;
    text << file.rdbuf();
    const std::string all = text.str();
    const auto begin = all.find("// ENGINE_MOTION_HLSL_BEGIN");
    const auto end = all.find("// ENGINE_MOTION_HLSL_END");
    h.check(begin != std::string::npos && end != std::string::npos && end > begin, "the ENGINE_MOTION_HLSL markers are present");
    const std::string block = all.substr(begin, end - begin);
    h.check(block.find(")HLSL\"") == std::string::npos, "the block sits inside one literal");
    return block;
}

inline void run(const Harness& h) {
    const std::string source = engineMotionBlock(h) + R"HLSL(
struct Case { float2 ndc; float zr; uint slot; };
StructuredBuffer<Case> Cases : register(t0);
RWStructuredBuffer<float4> Out : register(u0);
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint n, s; Cases.GetDimensions(n, s);
    if (id.x >= n) return;
    Case c = Cases[id.x];
    EnginePoolRecord r = EP[c.slot];
    uint kind = engineRecordKind(r);
    float4 before;
    bool ok = engineReproject(r, c.ndc, c.zr, before);
    Out[id.x] = ok ? float4(before.xy / before.w, before.w, float(kind)) : float4(-99, -99, -1, float(kind));
}
)HLSL";
    ComPtr<ID3DBlob> code, errors;
    const HRESULT chr = D3DCompile(source.data(), source.size(), "engine-motion-math", nullptr, nullptr, "main", "cs_5_0",
                                   D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(chr) && errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    h.check(SUCCEEDED(chr), "the shipped engine-motion HLSL compiles as cs_5_0");
    ComPtr<ID3D11ComputeShader> cs;
    h.check(SUCCEEDED(h.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs)), "compute shader on WARP");

    const Camera now = camera(10.0, {1200.0, 30.0, -800.0}, 0.0004, -0.0003);
    const Camera before = camera(10.6, {1200.4, 29.9, -800.7}, -0.0002, 0.0005);
    const auto nowRows = now.rows(), beforeRows = before.rows();

    std::vector<Record> pool(8);
    std::vector<Case> cases;
    std::vector<Expect> expect;
    const uint16_t ident[4] = {32767, 32767, 32767, 65534};
    auto qlanes = [](Q q, uint16_t out[4]) { out[0] = lane(q.x); out[1] = lane(q.y); out[2] = lane(q.z); out[3] = lane(q.w); };

    // A test point: object-local v on record `slot` (current pose), expected
    // through its previous pose and the previous camera.
    auto addPoint = [&](uint32_t slot, V3 pos, const uint16_t q[4], float scale, V3 posPrev, const uint16_t qPrev[4],
                        float scalePrev, V3 v, uint32_t kind, const char* what) {
        const V3 relNow = add(sub({double(float(pos.x)), double(float(pos.y)), double(float(pos.z))},
                                  {double(nowRows[275 * 4]), double(nowRows[275 * 4 + 1]), double(nowRows[275 * 4 + 2])}),
                              mul(turn(decode(q), v), scale));
        double c[4];
        Camera::clip(nowRows, relNow, c);
        Case k{{float(c[0] / c[3]), float(c[1] / c[3])}, float(c[2] / c[3]), slot};
        const V3 relPrev = add(sub({double(float(posPrev.x)), double(float(posPrev.y)), double(float(posPrev.z))},
                                   {double(beforeRows[275 * 4]), double(beforeRows[275 * 4 + 1]), double(beforeRows[275 * 4 + 2])}),
                               mul(turn(decode(qPrev), v), scalePrev));
        double b[4];
        Camera::clip(beforeRows, relPrev, b);
        cases.push_back(k);
        expect.push_back({{b[0] / b[3], b[1] / b[3]}, b[3] > 0, kind, what});
    };

    // 0: a still record, joined: the camera term.
    const V3 still{1210.0, 28.0, -840.0};
    pool[0].pose(still, ident, 1.0f, false); pool[0].pose(still, ident, 1.0f, true); pool[0].mark(ev::kJoined);
    for (V3 v : {V3{0, 0, 0}, V3{3, 1, -2}, V3{-5, 2, 4}})
        addPoint(0, still, ident, 1.0f, still, ident, 1.0f, v, 1, "still record: exactly the camera term");

    // 1: the landing ship, 540 m out: (0.4, -1.8, -0.6) m and 0.8 degrees a frame.
    uint16_t qShip[4], qShipPrev[4];
    qlanes(axisAngle({0.2, 1.0, 0.1}, 37.0), qShip);
    qlanes(axisAngle({0.2, 1.0, 0.1}, 36.2), qShipPrev);
    const V3 ship{1650.0, 60.0, -1300.0}, shipPrev{1649.6, 61.8, -1299.4};
    pool[1].pose(ship, qShip, 1.0f, false); pool[1].pose(shipPrev, qShipPrev, 1.0f, true); pool[1].mark(ev::kJoined);
    for (V3 v : {V3{0, 0, 0}, V3{12, 3, -30}, V3{-15, -2, 22}, V3{40, 5, 1}})
        addPoint(1, ship, qShip, 1.0f, shipPrev, qShipPrev, 1.0f, v, 1, "landing-ship mover");

    // 2: a scaled part.
    pool[2].pose(ship, qShip, 1.3f, false); pool[2].pose(shipPrev, qShipPrev, 1.3f, true); pool[2].mark(ev::kJoined);
    addPoint(2, ship, qShip, 1.3f, shipPrev, qShipPrev, 1.3f, {6, -1, 9}, 1, "scaled part");

    // 3: a quantised 90-degree turn (non-unit q after 16-bit lanes), turning 2 degrees.
    uint16_t q90[4], q88[4];
    qlanes(axisAngle({0, 0, 1}, 90.0), q90);
    qlanes(axisAngle({0, 0, 1}, 88.0), q88);
    const V3 turret{1215.0, 31.0, -830.0};
    pool[3].pose(turret, q90, 1.0f, false); pool[3].pose(turret, q88, 1.0f, true); pool[3].mark(ev::kJoined);
    for (V3 v : {V3{4, 0, 0}, V3{0, 2.5, -1}})
        addPoint(3, turret, q90, 1.0f, turret, q88, 1.0f, v, 1, "quantised turret turn");

    // 4: masked; 5: garbage at 288; 6: a joined tag over the wrong hash.
    pool[4] = pool[1]; pool[4].mark(ev::kMasked);
    pool[5] = pool[1]; pool[5].w[72] = 0x7FC0ED01u;
    pool[6] = pool[1]; pool[6].w[72] = ev::kJoined ^ ev::markerHash(pool[1].block(true), pool[1].block(false));
    addPoint(4, ship, qShip, 1.0f, shipPrev, qShipPrev, 1.0f, {1, 1, 1}, 2, "masked marker");
    addPoint(5, ship, qShip, 1.0f, shipPrev, qShipPrev, 1.0f, {1, 1, 1}, 3, "bare tag (stack garbage shape) is not a join");
    addPoint(6, ship, qShip, 1.0f, shipPrev, qShipPrev, 1.0f, {1, 1, 1}, 3, "swapped-block hash is not a join");

    // 7: last frame the point was behind the camera.
    const V3 behindNow{1210.0, 30.0, -805.0}, behindPrev{1150.0, 30.0, -760.0};
    pool[7].pose(behindNow, ident, 1.0f, false); pool[7].pose(behindPrev, ident, 1.0f, true); pool[7].mark(ev::kJoined);
    addPoint(7, behindNow, ident, 1.0f, behindPrev, ident, 1.0f, {0, 0, 0}, 1, "behind last frame's camera: no history");

    // The GPU run.
    auto structured = [&](const void* data, UINT stride, UINT count, ID3D11Buffer** buffer, ID3D11ShaderResourceView** srv) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = stride * count; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = stride;
        D3D11_SUBRESOURCE_DATA init{data, 0, 0};
        h.check(SUCCEEDED(h.device->CreateBuffer(&d, &init, buffer)), "structured buffer");
        h.check(SUCCEEDED(h.device->CreateShaderResourceView(*buffer, nullptr, srv)), "structured SRV");
    };
    ComPtr<ID3D11Buffer> poolBuffer, caseBuffer;
    ComPtr<ID3D11ShaderResourceView> poolSrv, caseSrv;
    structured(pool.data(), sizeof(Record), UINT(pool.size()), &poolBuffer, &poolSrv);
    structured(cases.data(), sizeof(Case), UINT(cases.size()), &caseBuffer, &caseSrv);
    auto constants = [&](const std::array<float, 276 * 4>& rows, ID3D11Buffer** out) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = UINT(rows.size() * 4); d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{rows.data(), 0, 0};
        h.check(SUCCEEDED(h.device->CreateBuffer(&d, &init, out)), "scene constants");
    };
    ComPtr<ID3D11Buffer> nowCb, beforeCb;
    constants(nowRows, &nowCb);
    constants(beforeRows, &beforeCb);
    auto foreign = nowRows;
    foreign[270 * 4 + 2] = 0.5f;   // a projection whose clip z is not constant
    ComPtr<ID3D11Buffer> foreignCb;
    constants(foreign, &foreignCb);

    D3D11_BUFFER_DESC od{};
    od.ByteWidth = UINT(cases.size() * 16); od.Usage = D3D11_USAGE_DEFAULT; od.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    od.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; od.StructureByteStride = 16;
    ComPtr<ID3D11Buffer> out;
    h.check(SUCCEEDED(h.device->CreateBuffer(&od, nullptr, &out)), "output buffer");
    ComPtr<ID3D11UnorderedAccessView> outUav;
    h.check(SUCCEEDED(h.device->CreateUnorderedAccessView(out.Get(), nullptr, &outUav)), "output UAV");
    od.Usage = D3D11_USAGE_STAGING; od.BindFlags = 0; od.CPUAccessFlags = D3D11_CPU_ACCESS_READ; od.MiscFlags = 0;
    ComPtr<ID3D11Buffer> staging;
    h.check(SUCCEEDED(h.device->CreateBuffer(&od, nullptr, &staging)), "staging buffer");

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
        h.check(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "map results");
        results.assign(static_cast<const float*>(m.pData), static_cast<const float*>(m.pData) + cases.size() * 4);
        ctx->Unmap(staging.Get(), 0);
    };
    std::vector<float> results;
    dispatch(nowCb.Get(), results);
    double worst = 0.0;
    for (size_t i = 0; i < cases.size(); ++i) {
        const float* r = &results[i * 4];
        const uint32_t kind = uint32_t(r[3] + 0.5f);
        h.check(kind == expect[i].kind, expect[i].what);
        if (expect[i].kind != 1) continue;
        if (!expect[i].valid) { h.check(r[2] == -1.0f, "a point behind last frame's camera reprojects to nothing"); continue; }
        h.check(r[2] > 0.0f, "a joined point reprojects");
        const double e = std::max(std::fabs(r[0] - expect[i].ndc[0]), std::fabs(r[1] - expect[i].ndc[1]));
        worst = std::max(worst, e);
        if (e > 2e-5) std::fprintf(stderr, "  %s: GPU (%.7f %.7f) vs reference (%.7f %.7f), error %.2e\n", expect[i].what,
                                   r[0], r[1], expect[i].ndc[0], expect[i].ndc[1], e);
        h.check(e <= 2e-5, "previous NDC within 2e-5 of the double reference (0.02 px at 2000 px)");
    }
    // The foreign projection declines every joined point.
    std::vector<float> declined;
    dispatch(foreignCb.Get(), declined);
    for (size_t i = 0; i < cases.size(); ++i)
        if (expect[i].kind == 1) h.check(declined[i * 4 + 2] == -1.0f, "a projection that is not the pool families' encoding declines");
    std::printf("  math: %zu cases, worst previous-NDC error %.2e (limit 2e-5)\n", cases.size(), worst);
}

} // namespace math_tests
