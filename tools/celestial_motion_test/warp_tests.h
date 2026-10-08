// (c) The temporal shader's celestial path, run on WARP. Included by celestial_motion_test.cpp after its helpers.
//
// The production text (src/d3d11/temporal_shader_source.h, kTemporalCsHlsl) is compiled the production way (cs_5_0, flags 0, the
// macros of tools/temporal_shader_build) six ways, concurrently -- the mv entry as the DLSS path runs it (with the decision trace
// and without) and the main entry as the pass's own history runs it -- each with the celestial code compiled in
// (EDVR_CELESTIAL 1) and out (0: the shader as it was before this path existed). A virtual eye a quarter the dump's size sees the
// real moon of drawstate frame 23654 (its centre and radius from cb2[12], the depth of a sphere), a near object in front of it, a
// strip of cockpit, a block at the moon's depth off its screen rectangle, and sky. Three runs per entry:
//   A  the shader without the path (reference);
//   B  the shader with the path, probe.w bit 8192 clear and nothing at t15 -- every output byte for byte A's;
//   C  the shader with the path, the bit set, the records of the moon built from the real constants at t15.
// C differs from A at exactly the pixels the arithmetic says are inside the moon's volume on the world path with a finite depth
// (their decision path 12, their motion the record's, to a thousandth of a pixel) and nowhere else. The diagnostic mv's
// Stats[39] counts those pixels; the main entry's outputs change at them and at no other. Six controls break one token of the path each
// (its gate, the translation's sign, the depth interval, the rectangle, the decision path, the tie rule) and the same judge must refuse every one.
#pragma once
#include <array>
#include <map>
#include <thread>

namespace warp {
using Microsoft::WRL::ComPtr;

constexpr int kVW = 504, kVH = 488;      // the virtual render, a quarter of the dump's 2016 x 1949
constexpr float kDepthB = 0.025f;        // knobs.z: written depth = knobs.x + knobs.z / metres
constexpr float kSplit = 60.0f;          // split.x: the ship's radius, metres
constexpr UINT kStatsN = 64;

// The cbuffer P, as temporal_pass.cpp's PassParams lays it out (528 bytes, 33 rows); checked against the compiled shader's reflection.
struct Params {
    int32_t region[4];
    int32_t size[2];
    int32_t texSize[2];
    float tanNow[4], tanPrev[4], jit[4], dR0[4], dR1[4], dR2[4];
    float cand[4][3][4];
    float blend, gamma;
    int32_t haveHistory, candMask;
    float knobs[4], tvUsed[4], tvCand[4], tvCam[4], split[4], fovea0[4], fovea1[4], movers[4], probe[4], holoJitter[4], skip[4], lead[4];
};
static_assert(sizeof(Params) == 528, "the cbuffer is 33 16-byte rows");

struct Variant {
    const char* name;
    const char* entry;
    const char* diag;
    const char* trace;
    const char* cel;
    ComPtr<ID3DBlob> code;
    std::string log;
    HRESULT hr = E_FAIL;
    ComPtr<ID3D11ComputeShader> cs;
};

void compileOne(Variant* v) {
    const D3D_SHADER_MACRO macros[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", v->diag}, {"EDVR_TEMPORAL_TRACE", v->trace}, {"EDVR_CELESTIAL", v->cel}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> errors;
    v->hr = D3DCompile(edvr::kTemporalCsHlsl, std::strlen(edvr::kTemporalCsHlsl), "temporal", macros, nullptr, v->entry, "cs_5_0", 0, 0,
                       v->code.ReleaseAndGetAddressOf(), errors.GetAddressOf());
    if (errors && errors->GetBufferSize()) v->log.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
}

struct Outputs {
    std::array<std::vector<uint8_t>, 9> v;   // u0..u6, u7 as DT (trace) or ML, then slot 8
};

constexpr int kSlotO = 0, kSlotN = 1, kSlotStats = 2, kSlotMV = 3, kSlotZC = 4, kSlotMK = 5, kSlotUN = 6, kSlotU7 = 7;
const DXGI_FORMAT kFmt[8] = {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32G32_FLOAT,
                             DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
const UINT kBpp[8] = {16, 16, 4, 8, 4, 4, 16, 16};

struct Rig {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11SamplerState> smp;
    ComPtr<ID3D11Resource> res[9], stage[9];
    ComPtr<ID3D11UnorderedAccessView> uav[9];
    std::vector<uint8_t> fill[9];
};

void hr(HRESULT h, const char* what) { check(SUCCEEDED(h), fmt("%s failed (0x%08X)", what, static_cast<unsigned>(h))); }

void initRig(Rig& R) {
    D3D_FEATURE_LEVEL level{};
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &R.dev, &level, &R.ctx), "D3D11CreateDevice(WARP)");
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(Params);
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr(R.dev->CreateBuffer(&cbd, nullptr, &R.cb), "P constant buffer");
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    hr(R.dev->CreateSamplerState(&sd, &R.smp), "sampler");
    const float sentinel = -123.456f;
    for (int s = 0; s < 9; ++s) {
        const int slot = s == 8 ? kSlotU7 : s;
        if (s == kSlotStats) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kStatsN * 4;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = 4;
            ComPtr<ID3D11Buffer> b;
            hr(R.dev->CreateBuffer(&bd, nullptr, &b), "Stats buffer");
            hr(R.dev->CreateUnorderedAccessView(b.Get(), nullptr, &R.uav[s]), "Stats UAV");
            R.res[s] = b;
            D3D11_BUFFER_DESC sb = bd;
            sb.Usage = D3D11_USAGE_STAGING;
            sb.BindFlags = 0;
            sb.MiscFlags = 0;
            sb.StructureByteStride = 0;
            sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Buffer> st;
            hr(R.dev->CreateBuffer(&sb, nullptr, &st), "Stats staging");
            R.stage[s] = st;
            R.fill[s].assign(kStatsN * 4, 0);
            continue;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kVW;
        td.Height = kVH;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = s == 8 ? DXGI_FORMAT_R32G32_FLOAT : kFmt[slot];
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> t;
        hr(R.dev->CreateTexture2D(&td, nullptr, &t), "output texture");
        hr(R.dev->CreateUnorderedAccessView(t.Get(), nullptr, &R.uav[s]), "output UAV");
        R.res[s] = t;
        D3D11_TEXTURE2D_DESC sd2 = td;
        sd2.Usage = D3D11_USAGE_STAGING;
        sd2.BindFlags = 0;
        sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st;
        hr(R.dev->CreateTexture2D(&sd2, nullptr, &st), "output staging");
        R.stage[s] = st;
        R.fill[s].resize(static_cast<size_t>(kVW) * kVH * (s == 8 ? 8u : kBpp[slot]));
        for (size_t o = 0; o < R.fill[s].size(); o += 4) std::memcpy(&R.fill[s][o], &sentinel, 4);
    }
}

// One dispatch. `traceU7`: bind DT (RGBA32F) at u7 for a trace variant, else ML (RG32F).
Outputs dispatch(Rig& R, ID3D11ComputeShader* cs, const Params& p, ID3D11ShaderResourceView* const srv[16], bool traceU7) {
    R.ctx->UpdateSubresource(R.cb.Get(), 0, nullptr, &p, 0, 0);
    for (int s = 0; s < 9; ++s) {
        const int slot = s == 8 ? kSlotU7 : s;
        const UINT pitch = s == kSlotStats ? 0u : kVW * (s == 8 ? 8u : kBpp[slot]);
        R.ctx->UpdateSubresource(R.res[s].Get(), 0, nullptr, R.fill[s].data(), pitch, 0);
    }
    R.ctx->CSSetShader(cs, nullptr, 0);
    R.ctx->CSSetConstantBuffers(0, 1, R.cb.GetAddressOf());
    R.ctx->CSSetSamplers(0, 1, R.smp.GetAddressOf());
    R.ctx->CSSetShaderResources(0, 16, srv);
    ID3D11UnorderedAccessView* uavs[8];
    for (int s = 0; s < 7; ++s) uavs[s] = R.uav[s].Get();
    uavs[7] = traceU7 ? R.uav[7].Get() : R.uav[8].Get();
    R.ctx->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
    R.ctx->Dispatch((kVW + 7) / 8, (kVH + 7) / 8, 1);
    ID3D11UnorderedAccessView* nulls[8] = {};
    R.ctx->CSSetUnorderedAccessViews(0, 8, nulls, nullptr);
    ID3D11ShaderResourceView* nullSrv[16] = {};
    R.ctx->CSSetShaderResources(0, 16, nullSrv);
    Outputs out;
    for (int s = 0; s < 9; ++s) {
        if (s == 7 && !traceU7) continue;
        if (s == 8 && traceU7) continue;
        R.ctx->CopyResource(R.stage[s].Get(), R.res[s].Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(R.ctx->Map(R.stage[s].Get(), 0, D3D11_MAP_READ, 0, &m), "map output");
        const int slot = s == 8 ? kSlotU7 : s;
        if (s == kSlotStats) {
            out.v[s].assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + kStatsN * 4);
        } else {
            const size_t row = static_cast<size_t>(kVW) * (s == 8 ? 8u : kBpp[slot]);
            out.v[s].resize(row * kVH);
            for (int y = 0; y < kVH; ++y)
                std::memcpy(out.v[s].data() + row * static_cast<size_t>(y), static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(m.RowPitch) * static_cast<size_t>(y), row);
        }
        R.ctx->Unmap(R.stage[s].Get(), 0);
    }
    return out;
}

ComPtr<ID3D11ShaderResourceView> texSrv(Rig& R, DXGI_FORMAT f, UINT bpp, const void* data) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kVW;
    td.Height = kVH;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = f;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{data, kVW * bpp, 0};
    ComPtr<ID3D11Texture2D> t;
    hr(R.dev->CreateTexture2D(&td, &init, &t), "texture");
    ComPtr<ID3D11ShaderResourceView> s;
    hr(R.dev->CreateShaderResourceView(t.Get(), nullptr, &s), "texture SRV");
    return s;
}

struct Lcg {
    uint32_t s;
    uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
    float unit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
};

// The virtual eye's ray through a pixel, game convention (+Z forward): u = (dx, dy, 1), exactly the shader's d with its z negated.
void rayOf(const float tan[4], int px, int py, double* dx, double* dy) {
    *dx = double(tan[0]) + (px + 0.5) / kVW * (double(tan[1]) - tan[0]);
    *dy = double(tan[3]) - (py + 0.5) / kVH * (double(tan[3]) - tan[2]);
}

struct Scene {
    std::vector<float> s, h, z;               // colour, history, the game's raw depth
    ComPtr<ID3D11ShaderResourceView> srvS, srvH, srvZ;
    int moonPixels = 0, nearBlock = 0, cockpit = 0, offRect = 0;
    int nbx0 = 0, nby0 = 0, nbx1 = 0, nby1 = 0, obx0 = 0, oby0 = 0, obx1 = 0, oby1 = 0;
};

Scene makeScene(Rig& R, const float tan[4], const double centre[3], double radius, const cel::BodyResult& body) {
    Scene sc;
    sc.s.assign(static_cast<size_t>(kVW) * kVH * 4, 0.0f);
    sc.h.assign(sc.s.size(), 0.0f);
    sc.z.assign(static_cast<size_t>(kVW) * kVH, 0.0f);
    Lcg g{12345};
    for (int y = 0; y < kVH; ++y)
        for (int x = 0; x < kVW; ++x) {
            const size_t i = static_cast<size_t>(y) * kVW + x;
            for (int k = 0; k < 4; ++k) {
                sc.s[i * 4 + k] = k == 3 ? 1.0f : 0.15f + 0.7f * g.unit();
                sc.h[i * 4 + k] = k == 3 ? 1.0f : 0.15f + 0.7f * g.unit();
            }
            // the moon: a sphere in head axes
            double dx, dy;
            rayOf(tan, x, y, &dx, &dy);
            const double a = dx * dx + dy * dy + 1.0;
            const double b = -2.0 * (dx * centre[0] + dy * centre[1] + centre[2]);
            const double c = centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2] - radius * radius;
            const double disc = b * b - 4.0 * a * c;
            if (disc >= 0.0) {
                const double t = (-b - std::sqrt(disc)) / (2.0 * a);
                if (t > 0.0) {
                    sc.z[i] = kDepthB / static_cast<float>(t);
                    ++sc.moonPixels;
                }
            }
        }
    // the moon's volume on the screen, to place the other things clear of or inside it
    const int bx0 = std::max(0, static_cast<int>(body.box[0])), by0 = std::max(0, static_cast<int>(body.box[1]));
    auto block = [&](int x0, int y0, int x1, int y1, float metres) {
        int n = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) { sc.z[static_cast<size_t>(y) * kVW + x] = metres > 0 ? kDepthB / metres : 0.0f; ++n; }
        return n;
    };
    // a near object (5 km, finite, past the split) inside the moon's rectangle: the world path, outside the depth interval
    sc.nbx0 = bx0 + 6; sc.nby0 = by0 + 6; sc.nbx1 = sc.nbx0 + 24; sc.nby1 = sc.nby0 + 16;
    sc.nearBlock = block(sc.nbx0, sc.nby0, sc.nbx1, sc.nby1, 5000.0f);
    // a block at the moon's own depth, off its rectangle (the top left corner): the world path, outside the screen rectangle
    sc.obx0 = 4; sc.oby0 = 4; sc.obx1 = 28; sc.oby1 = 20;
    sc.offRect = block(sc.obx0, sc.oby0, sc.obx1, sc.oby1, 3.0e6f);
    // the cockpit: a strip along the bottom at 1.5 m (the head path), under the moon's rectangle or not
    sc.cockpit = block(0, kVH - 12, kVW, kVH, 1.5f);
    sc.srvS = texSrv(R, DXGI_FORMAT_R32G32B32A32_FLOAT, 16, sc.s.data());
    sc.srvH = texSrv(R, DXGI_FORMAT_R32G32B32A32_FLOAT, 16, sc.h.data());
    sc.srvZ = texSrv(R, DXGI_FORMAT_R32_FLOAT, 4, sc.z.data());
    return sc;
}

Params baseParams(const float tanNow[4], const float tanPrev[4]) {
    Params p{};
    p.region[2] = p.size[0] = p.texSize[0] = kVW;
    p.region[3] = p.size[1] = p.texSize[1] = kVH;
    std::memcpy(p.tanNow, tanNow, 16);
    std::memcpy(p.tanPrev, tanPrev, 16);
    p.jit[2] = 1.0f; p.jit[3] = 0.5f;
    // the head's delta and the world path's delta: identity rotations (the camera term is then its translation alone)
    p.dR0[0] = p.dR1[1] = p.dR2[2] = 1.0f;
    for (int k = 0; k < 4; ++k) { p.cand[k][0][0] = p.cand[k][1][1] = p.cand[k][2][2] = 1.0f; }
    p.blend = 0.7f;
    p.gamma = 1.25f;
    p.knobs[0] = 0.0f; p.knobs[1] = 1.0f; p.knobs[2] = kDepthB; p.knobs[3] = 50000.0f;
    p.tvUsed[0] = 0.02f; p.tvUsed[3] = 1.0f;
    p.tvCam[0] = 0.3f; p.tvCam[1] = -0.2f; p.tvCam[2] = 0.8f; p.tvCam[3] = 1.0f;   // the camera moved a metre: the world path is on
    p.split[0] = kSplit;
    p.probe[0] = 1.0f;
    return p;
}

struct Diff {
    int slot = -1;
    size_t byte = 0;
};
// The first byte two runs disagree on, or slot -1.
Diff firstDiff(const Outputs& a, const Outputs& b) {
    for (int s = 0; s < 9; ++s) {
        if (a.v[s].size() != b.v[s].size()) return {s, 0};
        if (a.v[s].empty()) continue;
        if (std::memcmp(a.v[s].data(), b.v[s].data(), a.v[s].size()) != 0)
            for (size_t i = 0; i < a.v[s].size(); ++i) if (a.v[s][i] != b.v[s][i]) return {s, i};
    }
    return {};
}
float at(const std::vector<uint8_t>& v, size_t index) {
    float f;
    std::memcpy(&f, &v[index * 4], 4);
    return f;
}

// Which records hold a pixel, the shader's own test and tie rule: the narrowest depth span wins, the first of equals. A record with a
// shell (rec[19] > 0) holds the pixel only where its view-space point is rMin..rMax from the body's centre, widened by three pixels'
// footprint at the pixel's depth (the shader's slack), in double here.
int recordAt(const float rec[cel::kMaxBodies][cel::kRecordFloats], const float tan[4], double px, double py, float z) {
    int hit = -1;
    float narrow = 3.0e38f;
    for (int i = 0; i < 16; ++i) {
        if (rec[i][18] == 0.0f) break;
        const float span = rec[i][17] - rec[i][16];
        if (px >= rec[i][12] && px <= rec[i][14] && py >= rec[i][13] && py <= rec[i][15] && z >= rec[i][16] && z <= rec[i][17] && span < narrow) {
            if (rec[i][19] > 0.0f) {
                const double dx = tan[0] + (px + 0.5) / kVW * (double(tan[1]) - tan[0]);
                const double dy = tan[3] - (py + 0.5) / kVH * (double(tan[3]) - tan[2]);
                const double P[3] = {dx * z, dy * z, -double(z)};
                const double c[3] = {rec[i][20], rec[i][21], rec[i][22]};
                const double radial = cel::dist3(P, c);
                const double slack = cel::shellSlack(tan, kVW, kVH, z);
                if (!(radial >= double(rec[i][23]) - slack && radial <= double(rec[i][19]) + slack)) continue;
            }
            hit = i;
            narrow = span;
        }
    }
    return hit;
}

// One virtual eye of one real frame: the build from the real constants, the scene around the real moon, the records at t15.
struct Case {
    uint32_t frame = 0, eye = 0;
    cel::EyeInput in{}, inPrev{};
    cel::BuildResult build;
    Scene sc;
    ComPtr<ID3D11Buffer> crBuf;
    ComPtr<ID3D11ShaderResourceView> crSrv;
    Params off{}, on{};
    float split = kSplit;                 // the ship/world split the scene runs with (split.x): a pixel nearer is the head's, not the world's
    ID3D11ShaderResourceView* withRecords[16] = {};
    ID3D11ShaderResourceView* without[16] = {};
};

void makeCase(Rig& R, const Fixture& fx, uint32_t eye, uint32_t frame, Case& c) {
    const EyeFrame* cur = fx.find(frame, eye);
    const EyeFrame* prev = fx.find(frame - 1, eye);
    c.frame = frame;
    c.eye = eye;
    c.in = eyeOf(fx, frame, eye);
    c.in.w = kVW;
    c.in.h = kVH;
    c.inPrev = eyeOf(fx, frame - 1, eye);
    cel::build(cur->patches.data(), static_cast<uint32_t>(cur->patches.size()), prev->patches.data(), static_cast<uint32_t>(prev->patches.size()), c.in, c.build, scratch());
    check(c.build.records == 1, fmt("frame %u eye %u: the moon's record", frame, eye));
    const cel::BodyResult& moon = c.build.body[c.build.order[0]];
    // the moon's centre in head axes, from cb2[12] and A
    const cel::Patch* mp = nullptr;
    for (const cel::Patch& p : cur->patches) if (p.body[3] == moon.radius) { mp = &p; break; }
    double Arows[9], B[3] = {mp->body[0], mp->body[1], mp->body[2]}, centre[3];
    for (int i = 0; i < 9; ++i) Arows[i] = mp->A[i];
    cel::mulTV(Arows, B, centre);
    c.sc = makeScene(R, c.in.tan, centre, moon.radius, moon);
    check(c.sc.moonPixels > 2000 && c.sc.nearBlock > 200 && c.sc.offRect > 200, fmt("the scene has a moon (%d px), a near block and an off-rectangle block", c.sc.moonPixels));
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(c.build.gpu);
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = cel::kRecordFloats * 4;
    D3D11_SUBRESOURCE_DATA init{c.build.gpu, 0, 0};
    hr(R.dev->CreateBuffer(&bd, &init, &c.crBuf), "record buffer");
    hr(R.dev->CreateShaderResourceView(c.crBuf.Get(), nullptr, &c.crSrv), "record SRV");
    c.withRecords[0] = c.without[0] = c.sc.srvS.Get();
    c.withRecords[1] = c.without[1] = c.sc.srvH.Get();
    c.withRecords[2] = c.without[2] = c.sc.srvZ.Get();
    c.withRecords[15] = c.crSrv.Get();
    c.off = baseParams(c.in.tan, c.inPrev.tan);
    c.on = c.off;
    c.on.probe[3] = 8192.0f;
}

// The same frame with a second, wider volume: a body that holds the whole eye over the moon's depth and farther (a planet behind its moon),
// its own motion offset by 50 km, listed before the moon or after it. A pixel in both takes the NARROWER span's record whichever comes first.
void makeOverlapCase(Rig& R, const Case& base, bool wideFirst, Case& c) {
    c = base;
    float g0[cel::kRecordFloats], g1[cel::kRecordFloats];
    std::memcpy(g0, base.build.gpu[0], sizeof g0);
    std::memcpy(g1, base.build.gpu[0], sizeof g1);
    g1[3] += 5.0e4f;                                      // tv.x: a different motion
    g1[12] = -4.0f; g1[13] = -4.0f; g1[14] = kVW + 4.0f; g1[15] = kVH + 4.0f;
    g1[16] = base.build.gpu[0][16] * 0.5f; g1[17] = base.build.gpu[0][17] * 2.0f;
    std::memset(c.build.gpu, 0, sizeof c.build.gpu);
    std::memcpy(c.build.gpu[0], wideFirst ? g1 : g0, sizeof g0);
    std::memcpy(c.build.gpu[1], wideFirst ? g0 : g1, sizeof g0);
    c.build.records = 2;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(c.build.gpu);
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = cel::kRecordFloats * 4;
    D3D11_SUBRESOURCE_DATA init{c.build.gpu, 0, 0};
    c.crBuf.Reset();
    c.crSrv.Reset();
    hr(R.dev->CreateBuffer(&bd, &init, &c.crBuf), "record buffer (overlap)");
    hr(R.dev->CreateShaderResourceView(c.crBuf.Get(), nullptr, &c.crSrv), "record SRV (overlap)");
    c.withRecords[15] = c.crSrv.Get();
}

// What a run of the shader with the path must be, judged against the reference (the shader without it) and the arithmetic. No dying here:
// the controls below run mutated shaders through the same judge and require it to say no.
struct Verdict {
    bool bitClear = true;    // the bit clear with the records bound: byte for byte the reference
    bool inside = true;      // every pixel in a volume on the world path: decision path 12 and the record's motion
    bool outside = true;     // every other pixel: the reference's path, motion and trace bytes
    bool quiet = true;       // the outputs the path has no business with are untouched
    size_t expected = 0, visible = 0, nearIn = 0;
    double worst = 0.0;
    std::string first;       // the first thing that went wrong
    bool ok() const { return bitClear && inside && outside && quiet; }
};

Verdict judge(const Case& c, const Outputs& A, const Outputs& Bb, const Outputs& C) {
    Verdict v;
    auto bad = [&](bool* flag, const std::string& why) { if (*flag) { *flag = false; if (v.first.empty()) v.first = why; } };
    if (firstDiff(A, Bb).slot >= 0) bad(&v.bitClear, "the bit clear with records bound changed an output");
    for (int y = 0; y < kVH; ++y) {
        for (int x = 0; x < kVW; ++x) {
            const size_t i = static_cast<size_t>(y) * kVW + x;
            float zr = 0.0f;   // the shader's depth: the nearest of the 3x3, the game's raw depth through knobs
            for (int oy = -1; oy <= 1; ++oy)
                for (int ox = -1; ox <= 1; ++ox)
                    zr = std::max(zr, c.sc.z[static_cast<size_t>(std::min(std::max(y + oy, 0), kVH - 1)) * kVW + std::min(std::max(x + ox, 0), kVW - 1)]);
            const bool isFar = zr <= 0.0f;
            const float z = isFar ? 0.0f : kDepthB / zr;
            const uint32_t pathA = static_cast<uint32_t>(at(A.v[kSlotU7], i * 4 + 3)) & 15u;
            const uint32_t pathC = static_cast<uint32_t>(at(C.v[kSlotU7], i * 4 + 3)) & 15u;
            const int hit = (!isFar && z > c.split) ? recordAt(c.build.gpu, c.in.tan, x, y, z) : -1;
            if (hit >= 0) {
                ++v.expected;
                if (pathA != 2u) bad(&v.inside, fmt("pixel (%d,%d): the reference has it on path %u, not the world path", x, y, pathA));
                if (pathC != 12u) bad(&v.inside, fmt("pixel (%d,%d): inside the volume it takes path %u, not 12", x, y, pathC));
                double mx = 0, my = 0;
                if (!cel::shaderMotion(c.build.gpu[hit], c.in.tan, c.inPrev.tan, kVW, kVH, x, y, z, &mx, &my)) bad(&v.inside, "the arithmetic puts the pixel outside");
                const double e = std::hypot(at(C.v[kSlotMV], i * 2) - mx, at(C.v[kSlotMV], i * 2 + 1) - my);
                v.worst = std::max(v.worst, e);
                if (!(e < 2e-3)) bad(&v.inside, fmt("pixel (%d,%d): motion (%.5f, %.5f) is not the record's (%.5f, %.5f)", x, y, at(C.v[kSlotMV], i * 2), at(C.v[kSlotMV], i * 2 + 1), mx, my));
                if (std::hypot(at(A.v[kSlotMV], i * 2) - mx, at(A.v[kSlotMV], i * 2 + 1) - my) > 0.05) ++v.visible;   // the camera term was a visibly different answer here
            } else {
                if (pathC != pathA) bad(&v.outside, fmt("pixel (%d,%d): outside every volume the path is %u, the reference's %u", x, y, pathC, pathA));
                if (std::memcmp(&A.v[kSlotMV][i * 8], &C.v[kSlotMV][i * 8], 8) != 0) bad(&v.outside, fmt("pixel (%d,%d): outside every volume the motion changed", x, y));
                if (std::memcmp(&A.v[kSlotU7][i * 16], &C.v[kSlotU7][i * 16], 16) != 0) bad(&v.outside, fmt("pixel (%d,%d): outside every volume the trace changed", x, y));
                if (pathA == 2u && x >= c.sc.nbx0 && x < c.sc.nbx1 && y >= c.sc.nby0 && y < c.sc.nby1) ++v.nearIn;
            }
        }
    }
    for (int s : {kSlotO, kSlotN, kSlotZC, kSlotMK, kSlotUN})
        if (A.v[s] != C.v[s]) bad(&v.quiet, fmt("output slot %d changed", s));
    return v;
}

// Mutated shaders, each the production text with one token broken; the judge must refuse every one of them (a green run cannot mean the
// harness cannot see the path), and the unmutated shader it just passed.
struct Mutant { const char* name; const char* anchor; const char* mutated; };
void controls(Rig& R, const std::vector<const Case*>& cases, const std::vector<const Outputs*>& refs, const Mutant* ms, size_t kN) {
    std::vector<std::string> text(kN);
    std::vector<ComPtr<ID3DBlob>> code(kN);
    std::vector<HRESULT> result(kN, E_FAIL);
    std::vector<std::string> log(kN);
    for (size_t k = 0; k < kN; ++k) {
        text[k] = edvr::kTemporalCsHlsl;
        size_t n = 0;
        for (size_t at2 = text[k].find(ms[k].anchor); at2 != std::string::npos; at2 = text[k].find(ms[k].anchor, at2 + 1)) ++n;
        check(n == 1, fmt("control '%s': its anchor is in the shader exactly once (%zu)", ms[k].name, n));
        text[k].replace(text[k].find(ms[k].anchor), std::strlen(ms[k].anchor), ms[k].mutated);
    }
    {
        std::vector<std::thread> threads;
        for (size_t k = 0; k < kN; ++k)
            threads.emplace_back([&, k] {
                const D3D_SHADER_MACRO macros[] = {{"EDVR_TEMPORAL_DIAGNOSTICS", "1"}, {"EDVR_TEMPORAL_TRACE", "1"}, {"EDVR_CELESTIAL", "1"}, {nullptr, nullptr}};
                ComPtr<ID3DBlob> errors;
                result[k] = D3DCompile(text[k].data(), text[k].size(), "mutant", macros, nullptr, "mv", "cs_5_0", 0, 0, code[k].ReleaseAndGetAddressOf(), errors.GetAddressOf());
                if (errors && errors->GetBufferSize()) log[k].assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            });
        for (std::thread& t : threads) t.join();
    }
    for (size_t k = 0; k < kN; ++k) {
        check(SUCCEEDED(result[k]), fmt("control '%s' compiles: %s", ms[k].name, log[k].c_str()));
        ComPtr<ID3D11ComputeShader> cs;
        hr(R.dev->CreateComputeShader(code[k]->GetBufferPointer(), code[k]->GetBufferSize(), nullptr, &cs), "CreateComputeShader (control)");
        bool caught = false;
        std::string how;
        for (size_t n = 0; n < cases.size(); ++n) {
            const Case& c = *cases[n];
            const Outputs Bb = dispatch(R, cs.Get(), c.off, c.withRecords, true);
            const Outputs C = dispatch(R, cs.Get(), c.on, c.withRecords, true);
            const Verdict vd = judge(c, *refs[n], Bb, C);
            if (!vd.ok()) { caught = true; how = vd.first; break; }
        }
        check(caught, fmt("control '%s' is caught (the judge found nothing wrong with a broken shader)", ms[k].name));
        std::printf("    control '%s': caught -- %s\n", ms[k].name, how.c_str());
    }
}

#include "shell_tests.h"   // the radial shell of a straddling body (2026-10-08): its scenes, controls and float32 figures

void all(const Fixture& fx) {
    // ---- the six compiles, concurrently ------------------------------------------------------------------------------
    std::vector<Variant> v = {
        {"mv trace, celestial out", "mv", "1", "1", "0"}, {"mv trace, celestial in", "mv", "1", "1", "1"},
        {"mv diag, celestial out", "mv", "1", "0", "0"},   {"mv diag, celestial in", "mv", "1", "0", "1"},
        {"main, celestial out", "main", "1", "0", "0"},    {"main, celestial in", "main", "1", "0", "1"},
    };
    {
        std::vector<std::thread> threads;
        for (Variant& x : v) threads.emplace_back(compileOne, &x);
        for (std::thread& t : threads) t.join();
    }
    for (const Variant& x : v) check(SUCCEEDED(x.hr), fmt("the production text compiles (%s): %s", x.name, x.log.c_str()));
    // the cbuffer is the one PassParams writes
    {
        ComPtr<ID3D11ShaderReflection> refl;
        hr(D3DReflect(v[1].code->GetBufferPointer(), v[1].code->GetBufferSize(), __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(refl.GetAddressOf())), "D3DReflect");
        ID3D11ShaderReflectionConstantBuffer* cbr = refl->GetConstantBufferByName("P");
        D3D11_SHADER_BUFFER_DESC bd{};
        check(cbr && SUCCEEDED(cbr->GetDesc(&bd)) && bd.Size == sizeof(Params), "cbuffer P is 528 bytes, as PassParams");
        std::map<std::string, UINT> off;
        for (UINT i = 0; i < bd.Variables; ++i) {
            D3D11_SHADER_VARIABLE_DESC vd{};
            cbr->GetVariableByIndex(i)->GetDesc(&vd);
            off[vd.Name] = vd.StartOffset;
        }
        check(off["region"] == offsetof(Params, region) && off["tanNow"] == offsetof(Params, tanNow) && off["tvCam"] == offsetof(Params, tvCam) &&
              off["split"] == offsetof(Params, split) && off["probe"] == offsetof(Params, probe) && off["holoJitter"] == offsetof(Params, holoJitter) &&
              off["knobs"] == offsetof(Params, knobs) && off["haveHistory"] == offsetof(Params, haveHistory), "the cbuffer's fields sit where PassParams puts them");
        // t15 is the record buffer, and nothing in t9..t11 (the retired terrain rig reads those as nothing)
        D3D11_SHADER_DESC sd{};
        refl->GetDesc(&sd);
        bool t15 = false;
        for (UINT i = 0; i < sd.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC b{};
            refl->GetResourceBindingDesc(i, &b);
            if (b.BindPoint == 15 && b.Type == D3D_SIT_STRUCTURED && std::string(b.Name) == "CR") t15 = true;
            check(!(b.BindPoint >= 9 && b.BindPoint <= 11), "t9..t11 stay free");
        }
        check(t15, "the celestial records are CR at t15");
    }
    Rig R;
    initRig(R);
    for (Variant& x : v) hr(R.dev->CreateComputeShader(x.code->GetBufferPointer(), x.code->GetBufferSize(), nullptr, &x.cs), "CreateComputeShader");

    unsigned frames = 0, celestialPixels = 0, mainChanged = 0;
    double worstMv = 0.0;
    Outputs controlRef;
    Case controlCase;
    size_t controlExpected = 0;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        for (uint32_t frame : {23651u, 23654u, 23665u}) {
            Case c;
            makeCase(R, fx, eye, frame, c);
            const Outputs A = dispatch(R, v[0].cs.Get(), c.on, c.withRecords, true);    // celestial compiled out: bit and records are no concern of its
            const Outputs Bo = dispatch(R, v[1].cs.Get(), c.off, c.without, true);      // compiled in, bit clear, nothing at t15
            const Diff d1 = firstDiff(A, Bo);
            check(d1.slot < 0, fmt("frame %u eye %u: with no records the shader's outputs are byte for byte the shader without the path (slot %d byte %zu)", frame, eye, d1.slot, d1.byte));
            const Outputs Bb = dispatch(R, v[1].cs.Get(), c.off, c.withRecords, true); // bit clear, records bound anyway: still never read
            const Outputs C = dispatch(R, v[1].cs.Get(), c.on, c.withRecords, true);
            const Verdict vd = judge(c, A, Bb, C);
            check(vd.ok(), fmt("frame %u eye %u: %s", frame, eye, vd.first.c_str()));
            check(vd.expected > 1500, fmt("frame %u eye %u: the moon's pixels took the path (%zu)", frame, eye, vd.expected));
            check(vd.visible > vd.expected / 2, fmt("frame %u eye %u: the record's motion differs visibly from the camera term on most of them (%zu of %zu)", frame, eye, vd.visible, vd.expected));
            check(vd.nearIn > 50, "pixels inside the moon's rectangle but outside its depth interval stay on the world path");
            celestialPixels += static_cast<unsigned>(vd.expected);
            worstMv = std::max(worstMv, vd.worst);
            // the diagnostic variant's count: Stats[39] is the number of path-12 pixels
            {
                const Outputs D = dispatch(R, v[3].cs.Get(), c.on, c.withRecords, false);
                uint32_t counted;
                std::memcpy(&counted, &D.v[kSlotStats][39 * 4], 4);
                check(counted == vd.expected, fmt("frame %u eye %u: Stats[39] counts the path-12 pixels (%u against %zu)", frame, eye, counted, vd.expected));
                const Outputs E0 = dispatch(R, v[2].cs.Get(), c.on, c.withRecords, false);
                uint32_t zero;
                std::memcpy(&zero, &E0.v[kSlotStats][39 * 4], 4);
                check(zero == 0, "the shader without the path counts nothing at Stats[39]");
                const Outputs Eb = dispatch(R, v[3].cs.Get(), c.off, c.without, false);
                check(firstDiff(E0, Eb).slot < 0, "the diagnostic variant, too, is byte for byte the reference with no records");
            }
            // ---- main: the pass's own history ---------------------------------------------------------------------------
            {
                Params mon = c.on, moff = c.off;
                mon.haveHistory = moff.haveHistory = 1;
                const Outputs M0 = dispatch(R, v[4].cs.Get(), mon, c.withRecords, false);
                const Outputs M1 = dispatch(R, v[5].cs.Get(), moff, c.without, false);
                check(firstDiff(M0, M1).slot < 0, fmt("frame %u eye %u: main with no records is byte for byte main without the path", frame, eye));
                const Outputs Mb = dispatch(R, v[5].cs.Get(), moff, c.withRecords, false);
                check(firstDiff(M0, Mb).slot < 0, "main with the bit clear and records bound: byte for byte too");
                const Outputs M2 = dispatch(R, v[5].cs.Get(), mon, c.withRecords, false);
                size_t changed = 0;
                for (int y = 0; y < kVH; ++y)
                    for (int x = 0; x < kVW; ++x) {
                        const size_t i = static_cast<size_t>(y) * kVW + x;
                        const bool differ = std::memcmp(&M0.v[kSlotO][i * 16], &M2.v[kSlotO][i * 16], 16) != 0 || std::memcmp(&M0.v[kSlotN][i * 16], &M2.v[kSlotN][i * 16], 16) != 0;
                        float zr = 0.0f;
                        for (int oy = -1; oy <= 1; ++oy)
                            for (int ox = -1; ox <= 1; ++ox)
                                zr = std::max(zr, c.sc.z[static_cast<size_t>(std::min(std::max(y + oy, 0), kVH - 1)) * kVW + std::min(std::max(x + ox, 0), kVW - 1)]);
                        const float z = zr > 0.0f ? kDepthB / zr : 0.0f;
                        const bool inVolume = zr > 0.0f && z > kSplit && recordAt(c.build.gpu, c.in.tan, x, y, z) >= 0;
                        if (differ) {
                            ++changed;
                            check(inVolume, fmt("frame %u eye %u pixel (%d,%d): main changed a pixel outside every volume", frame, eye, x, y));
                        }
                    }
                check(changed > vd.expected / 2, fmt("frame %u eye %u: main's history moved on most of the moon's pixels (%zu of %zu)", frame, eye, changed, vd.expected));
                mainChanged += static_cast<unsigned>(changed);
            }
            if (eye == 0 && frame == 23654u) { controlRef = A; controlCase = c; controlExpected = vd.expected; }
            ++frames;
        }
    }
    std::printf("(c) %u scenes (both eyes, three frames, the real moon): %u moon pixels took path 12 with the record's motion (worst %.5f px); outside every "
                "volume and with no records the outputs are byte for byte the shader without the path; main's history moved on %u pixels, all inside a volume\n",
                frames, celestialPixels, worstMv, mainChanged);
    // two volumes over one pixel: the narrower span's record wins, whichever is listed first
    Case overA, overB;
    makeOverlapCase(R, controlCase, false, overA);
    makeOverlapCase(R, controlCase, true, overB);
    for (const Case* oc : {&overA, &overB}) {
        const Outputs Bb = dispatch(R, v[1].cs.Get(), oc->off, oc->withRecords, true);
        const Outputs C = dispatch(R, v[1].cs.Get(), oc->on, oc->withRecords, true);
        const Verdict vd = judge(*oc, controlRef, Bb, C);
        check(vd.ok(), fmt("two volumes over the moon: %s", vd.first.c_str()));
        // the wide record holds pixels the moon's does not (the off-rectangle block takes ITS motion), the moon's pixels keep the moon's
        check(vd.expected >= controlExpected + 300, fmt("the second volume claims the block at the moon's depth that the moon's rectangle left alone (%zu against %zu)", vd.expected, controlExpected));
    }
    const Mutant base[] = {
        {"the probe bit's gate removed", "    if ((uint(probe.w + 0.5) & 8192u) == 0u) return false;\n    int hit = -1;", "    int hit = -1;"},
        {"the translation's sign flipped", "dp = float3(dot(r.m0.xyz, P) + r.m0.w,", "dp = float3(dot(r.m0.xyz, P) - r.m0.w,"},
        {"the depth interval dropped", "z >= r.span.x && z <= r.span.y && span < narrow", "span < narrow"},
        {"the decision path left at 2", "                        count39 = 1;\n", ""},
        {"the rectangle dropped", "p.x >= r.box.x && p.x <= r.box.z && p.y >= r.box.y && p.y <= r.box.w &&\n            ", ""},
        {"the narrower span's rule inverted (the widest wins)", "float span = r.span.y - r.span.x;", "float span = r.span.x - r.span.y;"},
    };
    controls(R, {&controlCase, &overA}, {&controlRef, &controlRef}, base, sizeof(base) / sizeof(base[0]));
    std::printf("(c) two volumes over one pixel: the narrower span's record wins in either order; 6 controls: one token of the path broken each, every one caught by the same judge\n");
    shell::all(R, v, fx);
}

}  // namespace warp
