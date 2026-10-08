#pragma once
// consumer_tests: drives the PRODUCTION temporal compute shader (the full
// kTemporalCsHlsl of src/d3d11/temporal_shader_source.h, entry "mv") on
// WARP, with the engine-record motion path (fix.temporal_aa on, probe.w
// bit 2048) armed through real ES/EP/scene-depth resources -- unlike
// tools/engine_velocity_test/math_tests.h, which tests the ENGINE_MOTION_HLSL
// block's arithmetic in isolation via a minimal wrapper shader, this drives
// enginePixel() and its integration into mv (the "Engine-record motion LAST"
// block, and the EDVR_TEMPORAL_DIAGNOSTICS counters) exactly as the compose
// calls them. Reuses math_tests.h's small math helpers (camera rows, pose
// records, the CPU-double reference), copied into this file's own namespace
// so the two headers can be included together without name collisions.
//
// docs/kinematic-motion-injection-2026-09-19.md; the 2026-09-23 review that
// asked for this: a masked pixel must make DLSS request no history (the
// sentinel in MV and MK=1), not just move a counter.

#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../../src/d3d11/engine_velocity_emit.h"

namespace consumer_tests {
using Microsoft::WRL::ComPtr;
namespace ev = edvr::engine_velocity_emit;

struct Harness {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    void (*check)(bool, const char*) = nullptr;
};

// ---------------------------------------------------------------------------
// Small math helpers, copied from tools/engine_velocity_test/math_tests.h
// (own namespace: both headers may be included in the same translation unit).
// ---------------------------------------------------------------------------
struct V3 { double x, y, z; };
inline V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 mul(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// A camera: clip rows 270..273 and origin 275, exactly as the game fills
// cb1/cb2 (math_tests.h's Camera, verbatim). The array holds a 277th float4:
// the freshness stamp the compose's EN[276].x reads (math_tests'
// kStampFrame, uint bits).
struct Camera {
    double rot[3][3];
    V3 t;
    V3 origin;
    double fx, fy, nearZ, jx, jy;
    std::array<float, 277 * 4> rows(uint32_t token = math_tests::kStampFrame) const {
        std::array<float, 277 * 4> cb{};
        auto clipLin = [&](V3 v, float* out) {
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
        uint32_t stamp = token;
        std::memcpy(&cb[276 * 4], &stamp, 4);
        return cb;
    }
    // Project a camera-relative point (world - origin) through the FLOAT
    // rows, in double, as the GPU would multiply them (engineReproject's
    // forward half).
    static void clip(const std::array<float, 277 * 4>& cb, V3 rel, double out[4]) {
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

// The inverse of Camera::clip's linear part: the world point (relative to
// the camera's origin) that projects to ndc at view depth zView, through
// camera rows `rows` -- engineReproject's own Cramer's-rule solve
// (temporal_shader_source.h, ENGINE_MOTION_HLSL block), transcribed in
// double. Used only to PLACE a record exactly under a chosen pixel;
// engineReproject itself, on the GPU, only ever runs forward (pose -> clip).
inline V3 solveRel(const std::array<float, 277 * 4>& rows, double ndcX, double ndcY, double zView) {
    const V3 a{rows[270 * 4 + 0], rows[271 * 4 + 0], rows[272 * 4 + 0]};
    const V3 b{rows[270 * 4 + 1], rows[271 * 4 + 1], rows[272 * 4 + 1]};
    const V3 c{rows[270 * 4 + 3], rows[271 * 4 + 3], rows[272 * 4 + 3]};
    const V3 ca = cross(b, c), cb = cross(c, a), cc = cross(a, b);
    const double det = dot(a, ca);
    const V3 rhs{ndcX * zView - rows[273 * 4 + 0], ndcY * zView - rows[273 * 4 + 1], zView};
    return mul(add(add(mul(ca, rhs.x), mul(cb, rhs.y)), mul(cc, rhs.z)), 1.0 / det);
}

// enginePixel's own pixel<->NDC map, with region.xy=0, offset=0 (mv's own
// call) and holoJitter.xy=0 (our setup), so dims == texSize == the tile.
inline void pixelToNdc(int px, int py, int dim, double& ndcX, double& ndcY) {
    ndcX = (double(px) + 0.5) / double(dim) * 2.0 - 1.0;
    ndcY = 1.0 - (double(py) + 0.5) / double(dim) * 2.0;
}
inline void clipToPixel(const double before[4], int dim, double& ppX, double& ppY) {
    ppX = (before[0] / before[3] * 0.5 + 0.5) * double(dim) - 0.5;
    ppY = (before[1] / before[3] * (-0.5) + 0.5) * double(dim) - 0.5;
}

// The t33 pool record (336 bytes), math_tests.h's Record verbatim.
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
    // The marker folds the frame stamp in: mark() takes the token EN[276].x
    // must carry for the join to certify.
    void mark(uint32_t tag, uint32_t token) { w[72] = tag ^ ev::markerHash(block(false), block(true), token); }
};
static_assert(sizeof(Record) == 336, "t33 stride");

// The pool vertex shaders' identity quaternion lanes (math_tests.h's own).
inline const uint16_t* identLanes() {
    static const uint16_t kIdent[4] = {32767, 32767, 32767, 65534};
    return kIdent;
}

// ---------------------------------------------------------------------------
// The production shader: extract kTemporalCsHlsl (screen_consumer_test.h's
// own extraction, verbatim) and compile entry "mv", plain and with
// EDVR_TEMPORAL_DIAGNOSTICS forced to 1 (it already defaults to 1 via the
// source's own #ifndef guard -- compiling both is defensive: this test must
// keep working if that default ever changes).
// ---------------------------------------------------------------------------
inline std::string loadTemporalHlsl(const Harness& h) {
    std::ifstream file("src/d3d11/temporal_shader_source.h", std::ios::binary);
    h.check(static_cast<bool>(file), "src/d3d11/temporal_shader_source.h readable (run from the repository root)");
    std::stringstream text;
    text << file.rdbuf();
    const std::string source = text.str();
    const auto cursor0 = source.find("constexpr char kTemporalCsHlsl[]");
    const auto end = source.find(")HLSL\";", cursor0);
    h.check(cursor0 != std::string::npos && end != std::string::npos, "production temporal shader found");
    std::string hlsl;
    auto cursor = cursor0;
    for (;;) {
        auto begin = source.find("R\"HLSL(", cursor);
        if (begin == std::string::npos || begin > end) break;
        begin += 7;
        const auto close = source.find(")HLSL\"", begin);
        h.check(close != std::string::npos && close <= end, "complete temporal shader literal");
        hlsl += source.substr(begin, close - begin);
        cursor = close + 6;
    }
    return hlsl;
}

inline ComPtr<ID3DBlob> compileMv(const Harness& h, const std::string& hlsl, bool diagnostics, bool trace=false) {
    const std::string source = (trace ? std::string("#define EDVR_TEMPORAL_TRACE 1\n") : std::string()) +
        (diagnostics ? ("#define EDVR_TEMPORAL_DIAGNOSTICS 1\n" + hlsl) : hlsl);
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), "temporal-mv-consumer", nullptr, nullptr, "mv", "cs_5_0",
                                   D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) && errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    h.check(SUCCEEDED(hr), diagnostics ? "production mv compiles as cs_5_0 with EDVR_TEMPORAL_DIAGNOSTICS 1"
                                        : "production mv compiles as cs_5_0 (plain)");
    return code;
}

struct Px { int x, y; };

inline void run(const Harness& h,const uint32_t* primaryRecord=nullptr,uint32_t stampFrame=math_tests::kStampFrame) {
    const int kDim = 16;   // region origin (0,0), size == texSize == 16x16

    const std::string hlsl = loadTemporalHlsl(h);
    const ComPtr<ID3DBlob> plainCode = compileMv(h, hlsl, false);
    const ComPtr<ID3DBlob> diagCode = compileMv(h, hlsl, true);
    ComPtr<ID3D11ComputeShader> csPlain, csDiag;
    h.check(SUCCEEDED(h.device->CreateComputeShader(plainCode->GetBufferPointer(), plainCode->GetBufferSize(), nullptr, &csPlain)),
            "plain mv creates on WARP");
    h.check(SUCCEEDED(h.device->CreateComputeShader(diagCode->GetBufferPointer(), diagCode->GetBufferSize(), nullptr, &csDiag)),
            "EDVR_TEMPORAL_DIAGNOSTICS-1 mv creates on WARP");

    // cbuffer P, located by reflection (screen_consumer_test.h's own
    // approach): a layout change cannot silently leave this test driving
    // different parameters than the shipping shader. P's layout does not
    // depend on EDVR_TEMPORAL_DIAGNOSTICS, so one reflection pass serves
    // both compiled variants.
    ComPtr<ID3D11ShaderReflection> reflection;
    h.check(SUCCEEDED(D3DReflect(diagCode->GetBufferPointer(), diagCode->GetBufferSize(), __uuidof(ID3D11ShaderReflection), &reflection)),
            "reflect mv");
    auto* parameters = reflection->GetConstantBufferByName("P");
    h.check(parameters != nullptr, "cbuffer P found by reflection");
    D3D11_SHADER_BUFFER_DESC pd{};
    h.check(SUCCEEDED(parameters->GetDesc(&pd)), "reflect cbuffer P's size");
    std::vector<char> data(pd.Size, 0);
    auto set = [&](const char* name, const void* value, UINT bytes) {
        auto* var = parameters->GetVariableByName(name);
        h.check(var != nullptr, name);
        D3D11_SHADER_VARIABLE_DESC vd{};
        h.check(SUCCEEDED(var->GetDesc(&vd)), "reflected temporal field");
        h.check(bytes <= vd.Size && vd.StartOffset + bytes <= data.size(), "reflected temporal field extent");
        std::memcpy(data.data() + vd.StartOffset, value, bytes);
    };
    auto floats = [&](const char* name, float a, float b, float c, float d) { float v[4] = {a, b, c, d}; set(name, v, 16); };
    auto ints2 = [&](const char* name, int a, int b) { int v[2] = {a, b}; set(name, v, 8); };
    auto ints4 = [&](const char* name, int a, int b, int c, int d) { int v[4] = {a, b, c, d}; set(name, v, 16); };

    // A trivial, exactly-invertible camera-term baseline: identity dR,
    // tanNow == tanPrev, so a pixel with no engine override reprojects to
    // (very nearly) itself -- the "no-engine result" cases (d)-(g) compare
    // against, and case (c)'s camera-only motion should match.
    ints4("region", 0, 0, kDim, kDim);
    ints2("size", kDim, kDim);
    ints2("texSize", kDim, kDim);
    floats("tanNow", -1, 1, -1, 1);
    floats("tanPrev", -1, 1, -1, 1);
    floats("jit", .25f, -.375f, 0, .5f);
    floats("dR0", 1, 0, 0, 0);
    floats("dR1", 0, 1, 0, 0);
    floats("dR2", 0, 0, 1, 0);
    floats("knobs", 0, 1, .025f, 0);      // y != 0: depth counts as bound
    floats("movers", 0, 0, 0, 0);         // mover mask off
    // holoJitter: z = 1 arms enginePixel; xy = 0 keeps enginePixel's
    // pixel map free of an extra offset term; w = 0 keeps
    // backgroundHistoryHidden's gate closed (avoids needing ZP/t3 bound).
    floats("holoJitter", 0, 0, 1, 0);
    // probe.w is set per dispatch (armed = bit 2048, baseline = 0), below.

    ComPtr<ID3D11Buffer> pCb;
    { D3D11_BUFFER_DESC d{}; d.ByteWidth = pd.Size; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      h.check(SUCCEEDED(h.device->CreateBuffer(&d, nullptr, &pCb)), "P constant buffer"); }
    auto uploadProbe = [&](float w) {
        floats("probe", 1, 0, 0, w);
        h.context->UpdateSubresource(pCb.Get(), 0, nullptr, data.data(), 0, 0);
    };

    // The engine camera: ONE static camera bound as BOTH EngineNow (b1) and
    // EngineBefore (b2), so an unmoved record's camera term (case c) is an
    // exact round-trip -- projecting a reconstructed point back through the
    // SAME rows it came from -- matching the zero-motion baseline above by
    // construction, not by coincidence. The rows carry the freshness stamp
    // at float4 276 (math_tests' kStampFrame): every joined record below is
    // marked with the same token, except the stale-stamp case.
    auto wordFloat=[](uint32_t v){float f;std::memcpy(&f,&v,4);return f;};
    const V3 origin=primaryRecord?V3{wordFloat(primaryRecord[4]),wordFloat(primaryRecord[5]),double(wordFloat(primaryRecord[6]))+50.0}:V3{0,0,0};
    const Camera engineCam = camera(0.0, origin, 0.0, 0.0);
    const std::array<float, 277 * 4> camRows = engineCam.rows(stampFrame);
    const V3 cameraOrigin{camRows[275*4],camRows[275*4+1],camRows[275*4+2]};
    ComPtr<ID3D11Buffer> enebCb;
    { D3D11_BUFFER_DESC d{}; d.ByteWidth = UINT(camRows.size() * 4); d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      D3D11_SUBRESOURCE_DATA init{camRows.data(), 0, 0};
      h.check(SUCCEEDED(h.device->CreateBuffer(&d, &init, &enebCb)), "engine scene constants (EN == EB)"); }

    const float kZBg = 0.0005f;   // the uniform scene depth, and the matching ES.y, for every armed pixel but the stale one
    const double zViewEff = double(camRows[273 * 4 + 2]) / double(kZBg);   // nearZ / zr, as engineReproject itself recomputes it

    // The pool: slot 0 a moving JOINED record (case a), 1 MASKED (b, pose
    // reused from a -- irrelevant, since a masked kind never reaches the
    // reprojection math), 2 an unmoved JOINED record (c), 3 a record with a
    // garbage marker: a valid pose, not a rig record (kind 3), 4 a JOINED
    // record stamped with an OLDER frame (h): the stale-stamp decline, 5 a
    // SKINNED record (F10, 2026-10-08), 6..7 unused.
    std::vector<Record> pool(8);
    const uint16_t* ident = identLanes();

    const Px pxSkin{8, 8};   // off the 4-px grid below: the consumer reads ES at the pixel itself, so its neighbours do not matter
    const Px pxA{2, 2}, pxB{2, 6}, pxC{2, 10};
    const Px pxD1{6, 2}, pxD2{6, 6}, pxD3{6, 10};
    const Px pxE{10, 2}, pxF{10, 6}, pxG{10, 10}, pxNotRig{13, 13}, pxStamp{13, 2};

    double ndcAx, ndcAy;
    pixelToNdc(pxA.x, pxA.y, kDim, ndcAx, ndcAy);
    const V3 posANow = add(solveRel(camRows, ndcAx, ndcAy, zViewEff),cameraOrigin);
    const V3 deltaA{2.0, -1.0, 3.0};   // metres: a landing-ship-scale motion, not camera motion (EN == EB)
    const V3 posAPrev = sub(posANow, deltaA);
    pool[0].pose(posANow, ident, 1.0f, false);
    pool[0].pose(posAPrev, ident, 1.0f, true);
    pool[0].mark(ev::kJoined, stampFrame);
    if(primaryRecord)std::memcpy(pool[0].w,primaryRecord,sizeof(pool[0].w)); // production emit bytes, including its unmodified marker

    pool[1].pose(posANow, ident, 1.0f, false);
    pool[1].pose(posAPrev, ident, 1.0f, true);
    pool[1].mark(ev::kMasked, stampFrame);

    double ndcCx, ndcCy;
    pixelToNdc(pxC.x, pxC.y, kDim, ndcCx, ndcCy);
    const V3 posC = add(solveRel(camRows, ndcCx, ndcCy, zViewEff),cameraOrigin);
    pool[2].pose(posC, ident, 1.0f, false);
    pool[2].pose(posC, ident, 1.0f, true);   // second block == first: engineRecordMoved() is false
    pool[2].mark(ev::kJoined, stampFrame);

    pool[3].pose(posANow, ident, 1.0f, false);
    pool[3].pose(posAPrev, ident, 1.0f, true);
    pool[3].w[72] = 0xDEADBEEFu;   // NOT tag ^ markerHash(...): declines as "not a rig record" (kind 3)

    // 4: the moving joined record of case (a), but its marker folds an older
    // frame's stamp -- the cull case: the record was not re-evaluated this
    // frame, and replaying its stale pose pair would phantom-drift the hull.
    pool[4] = pool[0];
    pool[4].mark(ev::kJoined, stampFrame - 1);

    // 5: a SKINNED record as the game writes it (F10, the walking NPC's body: 20 records, palette bases 7656..10383):
    // word 0, the palette base, is not 0, scale 1.0, and the marker word is 0 -- EDVR's emit leaves a base != 0
    // record native, so it carries no previous pose and no marker. The previous block is made hostile on purpose
    // (case a's 2 / -1 / 3 m delta): were the consumer to read this record as a rig record, the pixel would replay a
    // phantom motion, and the F10 plan (key the skinned pairs so their pixels stop carrying a stale slot) would
    // paint the NPC's body with it. Kind 3: the camera term stands, exactly like the no-engine baseline.
    pool[5].pose(posANow, ident, 1.0f, false);
    pool[5].pose(posAPrev, ident, 1.0f, true);
    pool[5].w[0] = 7656u;

    // ES (t21): x = 2*slot+1 (the patched pool draw's slot code), y = the
    // depth that draw wrote, raw bits. Z (t2): the scene's own depth.
    // Background: cleared (-1, 0) everywhere -- case (f) is simply the
    // default this whole tile starts from.
    std::vector<float> esData(size_t(kDim) * kDim * 2, 0.0f);
    std::vector<float> zData(size_t(kDim) * kDim, kZBg);
    for (int i = 0; i < kDim * kDim; ++i) { esData[size_t(i) * 2 + 0] = -1.0f; esData[size_t(i) * 2 + 1] = 0.0f; }
    auto setEs = [&](Px p, float code, float depth) {
        const size_t i = size_t(p.y) * kDim + p.x;
        esData[i * 2 + 0] = code; esData[i * 2 + 1] = depth;
    };
    setEs(pxA, 1.0f, kZBg);            // 2*0+1: JOINED (moving)
    setEs(pxB, 3.0f, kZBg);            // 2*1+1: MASKED
    setEs(pxC, 5.0f, kZBg);            // 2*2+1: JOINED (unmoved)
    setEs(pxD1, 6.0f, kZBg);           // even: CORRUPT
    setEs(pxD2, 3.5f, kZBg);           // fractional: CORRUPT
    setEs(pxD3, 0.0f, kZBg);           // < 1.0: declines at the es.x >= 1 gate (kind 0, not the corrupt check)
    setEs(pxE, 1.0f, kZBg + 0.25f);    // valid code, depth mismatched against Z: STALE
    setEs(pxF, -1.0f, 0.0f);           // cleared (explicit, though it is also this tile's default)
    setEs(pxG, 199.0f, kZBg);          // 2*99+1: slot 99 >= the pool's 8 records
    setEs(pxNotRig, 7.0f, kZBg);       // 2*3+1: a real, in-range slot with a garbage marker
    setEs(pxStamp, 9.0f, kZBg);        // 2*4+1: joined, but stamped with an older frame
    setEs(pxSkin, 11.0f, kZBg);        // 2*5+1: a skinned record, no marker

    // The self-marking fallback (the game's own target-6 channel, G6 t19,
    // probe.w bit 4096): pxS joins through G6 with ES cleared; pxP has BOTH
    // marked (ES must win); pxT's G6 depth mismatches the scene (stale).
    const Px pxS{13, 6}, pxP{13, 10}, pxT{6, 13};
    setEs(pxP, 5.0f, kZBg);            // ES: 2*2+1, the unmoved joined record
    std::vector<float> g6Data(size_t(kDim) * kDim * 2, 0.0f);
    for (int i = 0; i < kDim * kDim; ++i) { g6Data[size_t(i) * 2 + 0] = -1.0f; g6Data[size_t(i) * 2 + 1] = 0.0f; }
    auto setG6 = [&](Px p, float code, float depth) {
        const size_t i = size_t(p.y) * kDim + p.x;
        g6Data[i * 2 + 0] = code; g6Data[i * 2 + 1] = depth;
    };
    setG6(pxS, 1.0f, kZBg);            // the game's own write: slot 0, the moving joined record
    setG6(pxP, 1.0f, kZBg);            // ...also marks pxP (slot 0), where ES says slot 2
    setG6(pxT, 1.0f, kZBg + 0.25f);    // ...and pxT, at a depth the scene does not have

    // The overlap cases (the 2026-09-27 review's F1): ownership of the scene
    // depth picks the channel, not ES-first ordering.
    const Px pxO1{10, 13}, pxO2{2, 13};
    setEs(pxO1, 1.0f, kZBg * 2.0f);    // a substituted draw's stale marker: the seam has since drawn nearer
    setG6(pxO1, 1.0f, kZBg);           // G6's foreground marker owns the pixel: it must win
    setEs(pxO2, 1.0f, kZBg * 2.0f);    // neither channel owns the pixel: both stale
    setG6(pxO2, 1.0f, kZBg + 0.25f);

    // -------------------------- resources --------------------------
    auto structuredSrv = [&](const void* src, UINT stride, UINT count, ID3D11ShaderResourceView** srv) {
        D3D11_BUFFER_DESC d{}; d.ByteWidth = stride * count; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = stride;
        D3D11_SUBRESOURCE_DATA init{src, 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        h.check(SUCCEEDED(h.device->CreateBuffer(&d, &init, &buffer)), "structured buffer");
        h.check(SUCCEEDED(h.device->CreateShaderResourceView(buffer.Get(), nullptr, srv)), "structured SRV");
    };
    ComPtr<ID3D11ShaderResourceView> epSrv;
    structuredSrv(pool.data(), sizeof(Record), UINT(pool.size()), &epSrv);

    auto makeTexture = [&](DXGI_FORMAT fmt, UINT bind, const void* initData, UINT rowPitch) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = UINT(kDim); td.Height = UINT(kDim); td.Format = fmt;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1; td.BindFlags = bind; td.Usage = D3D11_USAGE_DEFAULT;
        ComPtr<ID3D11Texture2D> tex;
        if (initData) { D3D11_SUBRESOURCE_DATA init{initData, rowPitch, 0}; h.check(SUCCEEDED(h.device->CreateTexture2D(&td, &init, &tex)), "texture"); }
        else { h.check(SUCCEEDED(h.device->CreateTexture2D(&td, nullptr, &tex)), "texture"); }
        return tex;
    };
    const ComPtr<ID3D11Texture2D> esTex = makeTexture(DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, esData.data(), UINT(kDim) * 2 * 4);
    const ComPtr<ID3D11Texture2D> zTex = makeTexture(DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, zData.data(), UINT(kDim) * 4);
    const ComPtr<ID3D11Texture2D> g6Tex = makeTexture(DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, g6Data.data(), UINT(kDim) * 2 * 4);
    ComPtr<ID3D11ShaderResourceView> esSrv, zSrv, g6Srv;
    h.check(SUCCEEDED(h.device->CreateShaderResourceView(esTex.Get(), nullptr, &esSrv)), "ES SRV");
    h.check(SUCCEEDED(h.device->CreateShaderResourceView(zTex.Get(), nullptr, &zSrv)), "Z SRV");
    h.check(SUCCEEDED(h.device->CreateShaderResourceView(g6Tex.Get(), nullptr, &g6Srv)), "G6 SRV");

    const ComPtr<ID3D11Texture2D> mvTex = makeTexture(DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    const ComPtr<ID3D11Texture2D> zcTex = makeTexture(DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    const ComPtr<ID3D11Texture2D> mkTex = makeTexture(DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    ComPtr<ID3D11UnorderedAccessView> mvUav, zcUav, mkUav;
    h.check(SUCCEEDED(h.device->CreateUnorderedAccessView(mvTex.Get(), nullptr, &mvUav)), "MV UAV");
    h.check(SUCCEEDED(h.device->CreateUnorderedAccessView(zcTex.Get(), nullptr, &zcUav)), "ZC UAV");
    h.check(SUCCEEDED(h.device->CreateUnorderedAccessView(mkTex.Get(), nullptr, &mkUav)), "MK UAV");

    const UINT kStatsN = 64;   // covers every index mv's diagnostics ever write (0..54)
    const std::vector<uint32_t> zeroStats(kStatsN, 0);
    ComPtr<ID3D11Buffer> statsBuf;
    { D3D11_BUFFER_DESC d{}; d.ByteWidth = kStatsN * 4; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
      d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = 4;
      D3D11_SUBRESOURCE_DATA init{zeroStats.data(), 0, 0};
      h.check(SUCCEEDED(h.device->CreateBuffer(&d, &init, &statsBuf)), "Stats buffer"); }
    ComPtr<ID3D11UnorderedAccessView> statsUav;
    h.check(SUCCEEDED(h.device->CreateUnorderedAccessView(statsBuf.Get(), nullptr, &statsUav)), "Stats UAV");

    // -------------------------- readback --------------------------
    auto readTex = [&](ID3D11Texture2D* tex, UINT comps) {
        D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        h.check(SUCCEEDED(h.device->CreateTexture2D(&td, nullptr, &staging)), "staging texture");
        h.context->CopyResource(staging.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE m{};
        h.check(SUCCEEDED(h.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "map texture");
        std::vector<float> out(size_t(kDim) * kDim * comps);
        for (int y = 0; y < kDim; ++y)
            std::memcpy(out.data() + size_t(y) * kDim * comps, static_cast<const char*>(m.pData) + size_t(y) * m.RowPitch, size_t(kDim) * comps * 4);
        h.context->Unmap(staging.Get(), 0);
        return out;
    };
    auto readStats = [&]() {
        D3D11_BUFFER_DESC d{}; statsBuf->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0; d.StructureByteStride = 0;
        ComPtr<ID3D11Buffer> staging;
        h.check(SUCCEEDED(h.device->CreateBuffer(&d, nullptr, &staging)), "staging Stats buffer");
        h.context->CopyResource(staging.Get(), statsBuf.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        h.check(SUCCEEDED(h.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "map Stats buffer");
        std::vector<uint32_t> out(static_cast<const uint32_t*>(m.pData), static_cast<const uint32_t*>(m.pData) + kStatsN);
        h.context->Unmap(staging.Get(), 0);
        return out;
    };
    auto mvAt = [&](const std::vector<float>& arr, Px p) { const size_t i = (size_t(p.y) * kDim + p.x) * 2; return std::pair<float, float>{arr[i], arr[i + 1]}; };
    auto mkAt = [&](const std::vector<float>& arr, Px p) { return arr[size_t(p.y) * kDim + p.x]; };

    // -------------------------- bind (fixed across every dispatch) --------------------------
    h.context->CSSetConstantBuffers(0, 1, pCb.GetAddressOf());
    ID3D11Buffer* enEb[2] = {enebCb.Get(), enebCb.Get()};   // b1 = EngineNow, b2 = EngineBefore: the SAME static camera
    h.context->CSSetConstantBuffers(1, 2, enEb);
    h.context->CSSetShaderResources(2, 1, zSrv.GetAddressOf());
    ID3D11ShaderResourceView* esEp[2] = {esSrv.Get(), epSrv.Get()};   // t21 = ES, t22 = EP
    h.context->CSSetShaderResources(21, 2, esEp);
    h.context->CSSetShaderResources(19, 1, g6Srv.GetAddressOf());     // t19 = G6 (the game's own self-marked channel)
    ID3D11UnorderedAccessView* uavs[4] = {statsUav.Get(), mvUav.Get(), zcUav.Get(), mkUav.Get()};   // u2..u5
    h.context->CSSetUnorderedAccessViews(2, 4, uavs, nullptr);

    auto dispatchAndRead = [&](ID3D11ComputeShader* cs, float probeW) {
        uploadProbe(probeW);
        h.context->CSSetShader(cs, nullptr, 0);
        h.context->Dispatch((kDim + 7) / 8, (kDim + 7) / 8, 1);
        return std::make_pair(readTex(mvTex.Get(), 2), readTex(mkTex.Get(), 1));
    };

    // 1) The plain compile, armed -- only to cross-check against the
    //    diagnostics compile below (they must agree: instrumentation must
    //    not change behaviour).
    const auto plainArmed = dispatchAndRead(csPlain.Get(), 2048.0f);

    // 2) The diagnostics compile, armed -- the authoritative MV/MK result,
    //    and (Stats freshly zeroed right before this one dispatch) the
    //    counters.
    h.context->UpdateSubresource(statsBuf.Get(), 0, nullptr, zeroStats.data(), 0, 0);
    const auto diagArmed = dispatchAndRead(csDiag.Get(), 2048.0f);
    const std::vector<uint32_t> stats = readStats();

    // 3) The no-engine baseline: same everything, probe.w bit 2048 clear.
    const auto baseline = dispatchAndRead(csDiag.Get(), 0.0f);

    // 4) The self-marking fallback armed: bit 4096 added, G6 (t19) read where
    //    ES is cleared. Run AFTER the Stats read above: it adds a joined count
    //    of its own. The plain compile's own run follows for the agreement
    //    cross-check.
    const auto g6Armed = dispatchAndRead(csDiag.Get(), 2048.0f + 4096.0f);
    const auto g6Plain = dispatchAndRead(csPlain.Get(), 2048.0f + 4096.0f);

    const std::vector<float>&armedMv = diagArmed.first, &armedMk = diagArmed.second;
    const std::vector<float>&baseMv = baseline.first, &baseMk = baseline.second;

    auto predictBefore=[&](Px point){
    // Independent CPU-double pose inverse/forward for the exact record
    // supplied by production observePrimary, or the synthetic baseline.
    const Record& record=pool[0];
    auto turnRecord=[&](bool previous,V3 v){
        const unsigned qi=previous?78:2;
        const uint16_t lanes[4]={uint16_t(record.w[qi]),uint16_t(record.w[qi]>>16),uint16_t(record.w[qi+1]),uint16_t(record.w[qi+1]>>16)};
        const auto turned=math_tests::turn(math_tests::decode(lanes),{v.x,v.y,v.z});
        return V3{turned.x,turned.y,turned.z};
    };
    const V3 nowPos{wordFloat(record.w[4]),wordFloat(record.w[5]),wordFloat(record.w[6])};
    const V3 prevPos{wordFloat(record.w[73]),wordFloat(record.w[74]),wordFloat(record.w[75])};
    const double scale=wordFloat(record.w[1]),previousScale=wordFloat(record.w[77]);
    const V3 mx=turnRecord(false,{scale,0,0}),my=turnRecord(false,{0,scale,0}),mz=turnRecord(false,{0,0,scale});
    const V3 ix=cross(my,mz),iy=cross(mz,mx),iz=cross(mx,my);
    const double det=dot(mx,ix);
    double nx,ny;pixelToNdc(point.x,point.y,kDim,nx,ny);
    const V3 localWorld=sub(add(solveRel(camRows,nx,ny,zViewEff),cameraOrigin),nowPos);
    const V3 local{dot(ix,localWorld)/det,dot(iy,localWorld)/det,dot(iz,localWorld)/det};
    return sub(add(prevPos,mul(turnRecord(true,local),previousScale)),cameraOrigin);
    };
    // -------------------------- (a) JOINED, moving: exact motion --------------------------
    double worstErr = 0.0;
    {
        const V3 posAPrevF=predictBefore(pxA);
        double before[4];
        Camera::clip(camRows, posAPrevF, before);
        h.check(before[3] > 0.0, "case a: the moved record's previous position reprojects in front of the (static) camera");
        double ppX, ppY;
        clipToPixel(before, kDim, ppX, ppY);
        const auto mv = mvAt(armedMv, pxA);
        const double gotX = double(pxA.x) + double(mv.first), gotY = double(pxA.y) + double(mv.second);
        const double err = std::max(std::fabs(gotX - ppX), std::fabs(gotY - ppY));
        worstErr = std::max(worstErr, err);
        if (err > 1e-3) std::fprintf(stderr, "  case a: GPU previous pixel (%.6f %.6f) vs double reference (%.6f %.6f), error %.2e\n", gotX, gotY, ppX, ppY, err);
        h.check(err <= 1e-3, "JOINED: MV carries the record's exact previous pixel, within 1e-3 px of the CPU double reference");
        h.check(mkAt(armedMk, pxA) != 1.0f, "JOINED: MK is not NVIDIA's no-history sentinel (1.0)");
    }

    // -------------------------- (b) MASKED: no history, not just a counter --------------------------
    {
        const auto mv = mvAt(armedMv, pxB);
        h.check(mv.first == float(kDim) * 2.0f && mv.second == float(kDim) * 2.0f,
                "MASKED: MV is exactly (size)*2 -- NVIDIA's own no-history sentinel, not the record's estimated motion");
        h.check(mkAt(armedMk, pxB) == 1.0f, "MASKED: MK is exactly 1.0 -- the pixel requests no history, not merely a counted event");
    }

    // -------------------------- (c) unmoved JOINED: camera-only motion --------------------------
    {
        const auto mv = mvAt(armedMv, pxC);
        const auto mvBase = mvAt(baseMv, pxC);
        const double err = std::max(std::fabs(double(mv.first) - double(mvBase.first)), std::fabs(double(mv.second) - double(mvBase.second)));
        worstErr = std::max(worstErr, err);
        if (err > 1e-2) std::fprintf(stderr, "  case c: armed MV (%.6f %.6f) vs no-engine baseline (%.6f %.6f), error %.2e\n", mv.first, mv.second, mvBase.first, mvBase.second, err);
        h.check(err <= 1e-2, "unmoved JOINED record (second block == first): MV equals the no-engine camera-only result");
    }

    // -------------------------- (d)-(g), and kind 3: declined exactly like the no-engine baseline --------------------------
    auto expectBaseline = [&](Px p, const char* what) {
        const auto mv = mvAt(armedMv, p);
        const auto mvBase = mvAt(baseMv, p);
        h.check(mv.first == mvBase.first && mv.second == mvBase.second, what);
        h.check(mkAt(armedMk, p) == mkAt(baseMk, p), what);
    };
    expectBaseline(pxD1, "CORRUPT slot code (even, e.g. an additive blend of 2*slot+1 with cleared -1): declines exactly like the no-engine baseline");
    expectBaseline(pxD2, "CORRUPT slot code (fractional, e.g. 3.5): declines exactly like the no-engine baseline");
    expectBaseline(pxD3, "slot code 0 (below the ES.x >= 1 gate): declines exactly like the no-engine baseline");
    expectBaseline(pxE, "STALE (ES depth != scene depth): declines exactly like the no-engine baseline");
    expectBaseline(pxF, "cleared ES (-1, 0): declines exactly like the no-engine baseline");
    expectBaseline(pxG, "slot >= the pool's record count: declines exactly like the no-engine baseline");
    expectBaseline(pxNotRig, "a pool record that is not a rig record (garbage marker): declines exactly like the no-engine baseline");
    expectBaseline(pxStamp, "STALE STAMP (a joined marker from an older frame): declines to the camera term exactly like the no-engine baseline");
    expectBaseline(pxSkin, "F10: a SKINNED record (word 0 != 0, no marker, hostile previous block) is kind 3: the camera term stands exactly like the no-engine baseline");

    // -------------------------- plain vs EDVR_TEMPORAL_DIAGNOSTICS-1: must agree --------------------------
    for (Px p : {pxA, pxB, pxC, pxD1, pxD2, pxD3, pxE, pxF, pxG, pxNotRig, pxStamp, pxSkin}) {
        const auto pv = mvAt(plainArmed.first, p);
        const auto dv = mvAt(armedMv, p);
        h.check(pv.first == dv.first && pv.second == dv.second, "the plain and EDVR_TEMPORAL_DIAGNOSTICS-1 compiles of mv agree on MV at every constructed pixel");
    }

    // Drive the production capture compile through the same real resources:
    // ownership classifications must survive its float flags, while every
    // NVIDIA MV and history-mask result remains bit-identical to production.
    const auto traceCode=compileMv(h,hlsl,true,true);
    ComPtr<ID3D11ComputeShader> traceCs;
    h.check(traceCode && SUCCEEDED(h.device->CreateComputeShader(traceCode->GetBufferPointer(),traceCode->GetBufferSize(),nullptr,&traceCs)),
            "production capture motion shader creates on WARP");
    const auto decisionTex=makeTexture(DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_UNORDERED_ACCESS,nullptr,0);
    ComPtr<ID3D11UnorderedAccessView> decisionUav;
    h.check(SUCCEEDED(h.device->CreateUnorderedAccessView(decisionTex.Get(),nullptr,&decisionUav)),"capture decision UAV");
    h.context->CSSetUnorderedAccessViews(7,1,decisionUav.GetAddressOf(),nullptr);
    const auto captured=dispatchAndRead(traceCs.Get(),2048.0f);
    const auto decisions=readTex(decisionTex.Get(),4);
    for(Px p:{pxA,pxB,pxC,pxE,pxF,pxNotRig,pxStamp,pxSkin}) {
        h.check(mvAt(captured.first,p)==mvAt(armedMv,p) && mkAt(captured.second,p)==mkAt(armedMk,p),
                "capture diagnostics preserve exact production vectors and history masks");
    }
    for(const auto& item:std::vector<std::pair<Px,unsigned>>{{pxA,1},{pxNotRig,3},{pxSkin,3},{pxE,4},{pxStamp,6}}) {
        const size_t i=(size_t(item.first.y)*kDim+item.first.x)*4;
        const unsigned bits=static_cast<unsigned>(decisions[i+3]);
        h.check(decisions[i+3]==float(bits) && ((bits>>12)&7)==item.second,
                "capture flags encode joined/nonrig/stale-slot/stale-stamp without float precision loss");
        const auto motion=mvAt(armedMv,item.first);
        h.check(decisions[i]==motion.first && decisions[i+1]==motion.second,
                "capture ownership bits preserve physical decision motion");
        h.check((bits&15)==(item.second==1?11u:1u) && (bits&1024)!=0 && (bits&256)!=0,
                "capture ownership bits preserve path and projection/depth flags");
    }
    ID3D11UnorderedAccessView* noDecision=nullptr;
    h.context->CSSetUnorderedAccessViews(7,1,&noDecision,nullptr);

    // -------------------------- Stats[50..55]: the diagnostics compile's own counters --------------------------
    // 50 joined (a, c, and pxP's ES marker -- the precedence pixel is a joined
    // unmoved record through ES), 51 masked (b), 52 not-a-rig-record (notRig and
    // the skinned record, F10),
    // 53 stale (e, and the two overlap pixels o1/o2 whose ES marker is stale:
    // with bit 4096 clear the G6 channel is not read), 54 corrupt (d1 even,
    // d2 fractional), 55 stale stamp (h). d3's code 0 fails the ES.x >= 1 gate
    // before the corrupt check runs (kind 0), and kind 0 -- like d3, f and g --
    // is never tallied by mv's own diagnostics (only engineKind != 0 increments
    // a counter), so none of those three add to any of the six buckets checked
    // here. The G6 pixels read baseline with bit 4096 clear, tallying nothing.
    h.check(stats[50] == 3, "Stats[50] (JOINED pixels) == 3 (a, c, pxP)");
    h.check(stats[51] == 1, "Stats[51] (MASKED pixels) == 1 (b)");
    h.check(stats[52] == 2, "Stats[52] (pool records that are not rig records) == 2 (notRig, and the skinned record)");
    h.check(stats[53] == 3, "Stats[53] (STALE pixels) == 3 (e, and the overlap pixels o1/o2, G6 unread without bit 4096)");
    h.check(stats[54] == 2, "Stats[54] (CORRUPT pixels) == 2 (d1, d2)");
    h.check(stats[55] == 1, "Stats[55] (STALE-STAMP pixels) == 1 (h: a joined marker from an older frame)");

    // -------------------------- the self-marking fallback (G6 t19, probe.w 4096) --------------------------
    const std::vector<float>&g6Mv = g6Armed.first;
    {   // pxS: ES cleared, G6 marked with slot 0's code -- joins through the
        // game's own channel with the moving record's exact motion, computed
        // in double as case (a)'s: the pixel's world point carried by the
        // record's pose delta (identity orientation, scale 1).
        const V3 posSPrev = predictBefore(pxS);
        const V3 posSPrevF{double(float(posSPrev.x)), double(float(posSPrev.y)), double(float(posSPrev.z))};
        double before[4];
        Camera::clip(camRows, posSPrevF, before);
        double ppX, ppY;
        clipToPixel(before, kDim, ppX, ppY);
        const auto mv = mvAt(g6Mv, pxS);
        const double gotX = double(pxS.x) + double(mv.first), gotY = double(pxS.y) + double(mv.second);
        const double err = std::max(std::fabs(gotX - ppX), std::fabs(gotY - ppY));
        worstErr = std::max(worstErr, err);
        if (err > 1e-3) std::fprintf(stderr, "  case g6-join: GPU previous pixel (%.6f %.6f) vs double reference (%.6f %.6f), error %.2e\n", gotX, gotY, ppX, ppY, err);
        h.check(err <= 1e-3, "SELF-MARKING: a G6-marked pixel with ES cleared joins with the record's exact motion, within 1e-3 px");
    }
    {   // Bit 4096 is the gate: the same pixel in the 2048-only runs keeps the
        // no-fallback result exactly.
        const auto mv = mvAt(armedMv, pxS);
        const auto mvBase = mvAt(baseMv, pxS);
        h.check(mv.first == mvBase.first && mv.second == mvBase.second,
                "SELF-MARKING: with bit 4096 clear the G6 channel is not read -- the pixel declines like the baseline");
    }
    {   // pxP: ES marked (slot 2, unmoved) and G6 marked (slot 0, moving) --
        // the substituted draws' channel wins.
        const auto mv = mvAt(g6Mv, pxP);
        const auto mvBase = mvAt(baseMv, pxP);
        h.check(mv.first == mvBase.first && mv.second == mvBase.second,
                "SELF-MARKING: ES takes precedence over G6 where both are marked (the unmoved record's camera term)");
    }
    {   // pxT: G6's depth is not the scene's -- stale, declined like kind 4.
        const auto mv = mvAt(g6Mv, pxT);
        const auto mvBase = mvAt(baseMv, pxT);
        h.check(mv.first == mvBase.first && mv.second == mvBase.second,
                "SELF-MARKING: a G6 marker whose depth is not the scene's declines exactly like the stale case");
    }
    {   // pxO1 (the review's F1): a stale ES marker AND a valid foreground G6
        // marker -- ownership decides, the seam's marker wins over the hull's
        // stale one, and the pixel joins with the record's exact motion.
        const V3 posO1Prev = predictBefore(pxO1);
        const V3 posO1PrevF{double(float(posO1Prev.x)), double(float(posO1Prev.y)), double(float(posO1Prev.z))};
        double before[4];
        Camera::clip(camRows, posO1PrevF, before);
        double ppX, ppY;
        clipToPixel(before, kDim, ppX, ppY);
        const auto mv = mvAt(g6Mv, pxO1);
        const double gotX = double(pxO1.x) + double(mv.first), gotY = double(pxO1.y) + double(mv.second);
        const double err = std::max(std::fabs(gotX - ppX), std::fabs(gotY - ppY));
        worstErr = std::max(worstErr, err);
        if (err > 1e-3) std::fprintf(stderr, "  case overlap: GPU previous pixel (%.6f %.6f) vs double reference (%.6f %.6f), error %.2e\n", gotX, gotY, ppX, ppY, err);
        h.check(err <= 1e-3, "OVERLAP: a stale ES marker must not suppress a valid foreground G6 marker -- the G6 marker's motion applies");
    }
    {   // pxO2: neither channel owns the pixel (both depths stale) -- declines
        // exactly like the stale case.
        const auto mv = mvAt(g6Mv, pxO2);
        const auto mvBase = mvAt(baseMv, pxO2);
        h.check(mv.first == mvBase.first && mv.second == mvBase.second,
                "OVERLAP: when neither channel owns the scene pixel, the pixel declines to the camera term");
    }
    {   // The ES path is unaffected by G6's presence: pxA's exact motion holds.
        const auto mv = mvAt(g6Mv, pxA);
        const auto mvArmed = mvAt(armedMv, pxA);
        h.check(mv.first == mvArmed.first && mv.second == mvArmed.second,
                "SELF-MARKING: the substituted path is byte-identical with G6 bound");
    }
    for (Px p : {pxS, pxP, pxT, pxO1, pxO2}) {
        const auto pv = mvAt(g6Plain.first, p);
        const auto dv = mvAt(g6Mv, p);
        h.check(pv.first == dv.first && pv.second == dv.second,
                "the plain and diagnostics compiles agree at the self-marking pixels too");
    }

    std::printf("  consumer: %d pixels: joined 3 (worst error %.2e px), masked 1 (sentinel + MK=1), declined 8, "
                "the self-marking fallback joined/gated/preceded/stale as specified, the overlap cases by ownership, "
                "counters match\n",
                kDim * kDim, worstErr);
}

} // namespace consumer_tests
