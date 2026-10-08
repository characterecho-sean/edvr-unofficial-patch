#pragma once

// skin_clone_tests: the second skin (src/d3d11/dxbc_skin_clone.h), driven on WARP
// through a REAL skinned vertex shader and its real pixel shader (the game's
// bytecode from a local edvr_logs dump; not in the repository) or through a
// vertex shader compiled from HLSL here that has the same instruction chain
// (the build gate's stand-in).
//
// There is no oracle flight for this patch, so this is the proof. What it
// establishes, for one (vertex shader, pixel shader) pair:
//  1. the patched pair writes the game's own targets (SV_Target0..3 and depth)
//     bit-identically to the stock pair, under several vertex and state sets;
//  2. IDENTITY: when the previous frame's palette is the current one (placed at
//     a different base) and the previous pose the current one, E is exactly
//     zero at every covered pixel and valid is one. A clone that did not
//     reproduce the game's chain operation for operation could not do this;
//  3. POSE: moving only the previous record's position gives E = 100 (prev -
//     cur) in centimetres, to float rounding;
//  4. PALETTE: translating every previous palette row gives E = 100 * scale *
//     d (the pose's rotation is the identity to 1e-4), and swapping the two
//     states gives exactly the negated E (bit for bit);
//  5. NO HISTORY: join entry zero, or a base beyond the join table, gives E = 0
//     and valid = 0;
//  6. TEETH: pointing the join at a different palette block makes E nonzero,
//     so (2) was not satisfied vacuously.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "corpus_identity.h"

namespace skin_clone_tests {
using Microsoft::WRL::ComPtr;
namespace ci = corpus_identity::detail;

constexpr unsigned kRows = 64;     // palette rows per buffer
constexpr unsigned kRecords = 8;   // pool records
constexpr unsigned kJoin = 64;     // join table elements
constexpr unsigned kPoseElems = 64;

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed * 2654435761u + 12345u) {}
    uint32_t next() { s = ci::mix(s + 0x9e3779b9u); return s; }
    float unit() { return float(next() >> 8) / 16777216.0f; }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
};

// One frame's skinning state for the test vertices: where the palette block
// sits, its rows (3 x float4: a 3x4 matrix, translation in .w), and the pose.
struct State {
    uint32_t base = 3;
    std::vector<float> rows;       // 12 floats per bone
    float scale = 1.0f;
    uint16_t quat[4]{32768, 32768, 32768, 65535};   // x y z w, u16 -> u / 32768 - 1
    float pos[3]{0, 0, 0};
};

struct Vertex {
    uint32_t a[4], b[4], c[4];
};

// Three vertices (a triangle that covers a good part of the screen under the
// scene matrix below) skinned to `bones` palette entries with weights summing
// to 255. The compressed position branch (A.z bits 24..30 = 64) is used: the
// position is 2^(16 e / 65535) * (2x/65535 - 1, ...) with e = 4096 -> scale ~2.
inline void makeVertices(Rng& rng, unsigned bones, Vertex (&v)[3]) {
    static const int px[3] = {-700, 700, 0}, py[3] = {-700, -700, 700};
    uint32_t idx = 0, wgt = 0;
    unsigned weights[4] = {255, 0, 0, 0};
    if (bones == 2) { weights[0] = 140; weights[1] = 115; }
    if (bones == 3) { weights[0] = 120; weights[1] = 80; weights[2] = 55; }
    if (bones == 4) { weights[0] = 100; weights[1] = 80; weights[2] = 50; weights[3] = 25; }
    for (unsigned i = 0; i < bones; ++i) {
        idx |= i << (8 * i);
        wgt |= weights[i] << (8 * i);
    }
    for (unsigned k = 0; k < 3; ++k) {
        const uint32_t x = uint32_t((px[k] + 1000) * 65535 / 2000), y = uint32_t((py[k] + 1000) * 65535 / 2000);
        v[k].a[0] = x | (y << 16);
        v[k].a[1] = 4096u << 16 | (32768u + (rng.next() & 255u));
        v[k].a[2] = (64u << 24) | (rng.next() & 0x00ffffffu);
        v[k].a[3] = rng.next();
        for (unsigned i = 0; i < 4; ++i) v[k].b[i] = rng.next();
        v[k].c[0] = idx;
        v[k].c[1] = wgt;
        v[k].c[2] = rng.next();
        v[k].c[3] = rng.next();
    }
}

inline State makeState(Rng& rng, unsigned bones, uint32_t base) {
    State s;
    s.base = base;
    s.rows.resize(size_t(bones) * 12);
    for (unsigned b = 0; b < bones; ++b) {
        float* m = &s.rows[size_t(b) * 12];
        for (unsigned r = 0; r < 3; ++r)
            for (unsigned c = 0; c < 3; ++c) m[r * 4 + c] = (r == c ? 1.0f : 0.0f) + rng.range(-0.04f, 0.04f);
        for (unsigned r = 0; r < 3; ++r) m[r * 4 + 3] = rng.range(-0.1f, 0.1f);
    }
    s.scale = rng.range(0.8f, 1.2f);
    s.quat[0] = uint16_t(32768 + int(rng.range(-300, 300)));
    s.quat[1] = uint16_t(32768 + int(rng.range(-300, 300)));
    s.quat[2] = uint16_t(32768 + int(rng.range(-300, 300)));
    s.quat[3] = 65535;
    for (float& p : s.pos) p = rng.range(-2.0f, 2.0f);
    return s;
}

// The pool record: words 0..3 (base, scale, packed quaternion) and the position
// at byte 16; every other byte is patterned, because some of the game's shaders
// read fields beyond these at offsets the vertex data names.
inline void writeRecord(const State& s, uint32_t salt, uint8_t (&rec)[336]) {
    uint32_t w[84];
    for (unsigned i = 0; i < 84; ++i) w[i] = ci::mix(i * 2246822519u + salt) & 0x3f7fffffu;
    uint32_t scale;
    std::memcpy(&scale, &s.scale, 4);
    w[0] = s.base;
    w[1] = scale;
    w[2] = uint32_t(s.quat[0]) | uint32_t(s.quat[1]) << 16;
    w[3] = uint32_t(s.quat[2]) | uint32_t(s.quat[3]) << 16;
    std::memcpy(&w[4], s.pos, 12);
    std::memcpy(rec, w, 336);
}

struct Draw {
    std::vector<BYTE> target[4], depth;
    std::vector<float> e;          // RT7: R32G32B32A32_FLOAT, kSize^2 x 4
    std::vector<BYTE> slot;
    bool ok = false;
};

struct Rig {
    std::vector<BYTE> vsStock, vsPatched, psStock, psPatched;
    ComPtr<ID3D11VertexShader> vs0, vs1;
    ComPtr<ID3D11PixelShader> ps0, ps1;
    ComPtr<ID3D11InputLayout> layout;
    std::vector<ci::Decl> decls;
    DXGI_FORMAT formats[4]{};
    std::string why;
    std::string name;
    void (*check)(bool, const char*) = nullptr;
    // The pixel shader's b2 when set: zeros except these (float index, value). The real pixel shaders gate their
    // alpha tests and discards on it; the dummy data would otherwise discard every fragment.
    bool psMaterial = false;
    std::vector<std::pair<unsigned, float>> psMaterialSet;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
};

inline ComPtr<ID3D11Buffer> structured(ID3D11Device* dev, const void* data, unsigned bytes, unsigned stride) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = stride;
    const D3D11_SUBRESOURCE_DATA init{data, 0, 0};
    ComPtr<ID3D11Buffer> b;
    if (FAILED(dev->CreateBuffer(&bd, &init, &b))) return nullptr;
    return b;
}
inline ComPtr<ID3D11ShaderResourceView> view(ID3D11Device* dev, ID3D11Buffer* b, unsigned count) {
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = DXGI_FORMAT_UNKNOWN;
    vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    vd.Buffer.NumElements = count;
    ComPtr<ID3D11ShaderResourceView> v;
    if (FAILED(dev->CreateShaderResourceView(b, &vd, &v))) return nullptr;
    return v;
}
inline ComPtr<ID3D11Buffer> constants(ID3D11Device* dev, const void* data, unsigned bytes) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const D3D11_SUBRESOURCE_DATA init{data, 0, 0};
    ComPtr<ID3D11Buffer> b;
    if (FAILED(dev->CreateBuffer(&bd, &init, &b))) return nullptr;
    return b;
}

// What a draw is given.
struct Scene {
    Vertex verts[3];
    State cur, prev;
    bool patched = true;
    uint32_t joinTo = ~0u;        // join[cur.base] = joinTo (0xFFFFFFFF: prev.base; 0: none)
    uint32_t joinAt = ~0u;        // which join entry (default cur.base)
    unsigned dataSeed = 0;        // the pixel shader's dummies
    uint32_t instance = 5;        // the pool slot
    bool poseFromPrev = true;
    bool nullPs = false;          // depth only: where the vertex shader puts the triangle, whatever the pixel shader discards
    float vsFill = 0.0f;          // every float of the vertex shader's b2 (the game's displacement blocks sit behind it)
};

inline Draw draw(Rig& r, const Scene& sc) {
    Draw out;
    ID3D11Device* dev = r.dev;
    ID3D11DeviceContext* ctx = r.ctx;
    ctx->ClearState();
    // pool: kRecords records, the instance's own carrying the current state
    std::vector<uint8_t> pool(size_t(kRecords) * 336);
    for (unsigned i = 0; i < kRecords; ++i) {
        State filler;
        filler.base = 0;
        uint8_t rec[336];
        writeRecord(filler, 77u + i, rec);
        std::memcpy(&pool[size_t(i) * 336], rec, 336);
    }
    {
        uint8_t rec[336];
        writeRecord(sc.cur, 99u, rec);
        std::memcpy(&pool[size_t(sc.instance) * 336], rec, 336);
    }
    // palettes: cur rows at cur.base in t38, prev rows at prev.base in t108
    std::vector<float> cur(size_t(kRows) * 12, 0.0f), prv(size_t(kRows) * 12, 0.0f);
    for (size_t i = 0; i < sc.cur.rows.size(); ++i) cur[size_t(sc.cur.base) * 12 + i] = sc.cur.rows[i];
    for (size_t i = 0; i < sc.prev.rows.size(); ++i) prv[size_t(sc.prev.base) * 12 + i] = sc.prev.rows[i];
    std::vector<uint32_t> join(kJoin, 0);
    const uint32_t at = sc.joinAt == ~0u ? sc.cur.base : sc.joinAt;
    if (at < kJoin) join[at] = sc.joinTo == ~0u ? sc.prev.base : sc.joinTo;
    std::vector<uint8_t> pose(size_t(kPoseElems) * 32, 0);
    {
        uint8_t rec[336];
        writeRecord(sc.prev, 123u, rec);
        if (sc.prev.base < kPoseElems) std::memcpy(&pose[size_t(sc.prev.base) * 32], rec, 32);
    }
    auto poolBuf = structured(dev, pool.data(), unsigned(pool.size()), 336);
    auto curBuf = structured(dev, cur.data(), unsigned(cur.size() * 4), 48);
    auto prvBuf = structured(dev, prv.data(), unsigned(prv.size() * 4), 48);
    auto joinBuf = structured(dev, join.data(), unsigned(join.size() * 4), 4);
    auto poseBuf = structured(dev, pose.data(), unsigned(pose.size()), 32);
    if (!poolBuf || !curBuf || !prvBuf || !joinBuf || !poseBuf) return out;
    auto poolV = view(dev, poolBuf.Get(), kRecords), curV = view(dev, curBuf.Get(), kRows), prvV = view(dev, prvBuf.Get(), kRows),
         joinV = view(dev, joinBuf.Get(), kJoin), poseV = view(dev, poseBuf.Get(), kPoseElems);
    float scene[276][4]{};
    scene[270][0] = 0.4f;
    scene[271][1] = 0.4f;
    scene[273][2] = 0.5f;
    scene[273][3] = 1.0f;
    std::memcpy(scene[275], sc.cur.pos, 12);        // camera at the current record: rel = skinned vertex
    float material[10][4]{};
    for (auto& row : material) for (float& v : row) v = sc.vsFill;
    auto sceneBuf = constants(dev, scene, sizeof(scene));
    auto materialBuf = constants(dev, material, sizeof(material));
    // vertices
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(sc.verts);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    const D3D11_SUBRESOURCE_DATA vinit{sc.verts, 0, 0};
    ComPtr<ID3D11Buffer> vbuf, ibuf;
    const uint32_t instance[2] = {sc.instance, 0};
    bd.ByteWidth = sizeof(instance);
    const D3D11_SUBRESOURCE_DATA iinit{instance, 0, 0};
    D3D11_BUFFER_DESC vbd = bd;
    vbd.ByteWidth = sizeof(sc.verts);
    if (FAILED(dev->CreateBuffer(&vbd, &vinit, &vbuf)) || FAILED(dev->CreateBuffer(&bd, &iinit, &ibuf))) return out;
    if (!sceneBuf || !materialBuf) return out;
    ctx->VSSetConstantBuffers(1, 1, sceneBuf.GetAddressOf());
    ctx->VSSetConstantBuffers(2, 1, materialBuf.GetAddressOf());
    ID3D11ShaderResourceView* v33 = poolV.Get();
    ctx->VSSetShaderResources(33, 1, &v33);
    ID3D11ShaderResourceView* v38 = curV.Get();
    ctx->VSSetShaderResources(38, 1, &v38);
    if (sc.patched) {
        ID3D11ShaderResourceView* a = prvV.Get(); ctx->VSSetShaderResources(edvr::kSkinPrevPaletteSlot, 1, &a);
        ID3D11ShaderResourceView* b = joinV.Get(); ctx->VSSetShaderResources(edvr::kSkinJoinSlot, 1, &b);
        ID3D11ShaderResourceView* c = poseV.Get(); ctx->VSSetShaderResources(edvr::kSkinPoseSlot, 1, &c);
    }
    for (const auto& d : r.decls) {
        std::string why;
        if (!ci::bind(dev, ctx, d, sc.dataSeed, why)) return out;
    }
    if (r.psMaterial) {
        float m[32][4]{};
        for (const auto& kv : r.psMaterialSet) m[kv.first / 4][kv.first % 4] = kv.second;
        auto b = constants(dev, m, sizeof(m));
        if (!b) return out;
        ctx->PSSetConstantBuffers(2, 1, b.GetAddressOf());
    }
    ctx->IASetInputLayout(r.layout.Get());
    ID3D11Buffer* bufs[2] = {vbuf.Get(), ibuf.Get()};
    const UINT strides[2] = {sizeof(Vertex), sizeof(instance)};
    const UINT offsets[2] = {0, 0};
    ctx->IASetVertexBuffers(0, 2, bufs, strides, offsets);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // targets
    ComPtr<ID3D11Texture2D> tex[8];
    ComPtr<ID3D11RenderTargetView> rtv[8];
    ID3D11RenderTargetView* views[8]{};
    const float slotClear[4] = {-1, 0, 0, 0}, eClear[4] = {-1, -1, -1, -1};
    for (unsigned i = 0; i < 8; ++i) {
        DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
        if (i < 4) f = r.formats[i];
        else if (i == 6 && sc.patched) f = DXGI_FORMAT_R32G32_FLOAT;
        else if (i == 7 && sc.patched) f = DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (f == DXGI_FORMAT_UNKNOWN) continue;
        tex[i] = ci::texture(dev, f, D3D11_BIND_RENDER_TARGET);
        if (!tex[i] || FAILED(dev->CreateRenderTargetView(tex[i].Get(), nullptr, &rtv[i]))) return out;
        ctx->ClearRenderTargetView(rtv[i].Get(), i == 6 ? slotClear : i == 7 ? eClear : ci::clearColor(f));
        views[i] = rtv[i].Get();
    }
    const ComPtr<ID3D11Texture2D> depth = ci::texture(dev, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11DepthStencilState> dss;
    ComPtr<ID3D11RasterizerState> rs;
    if (!depth || FAILED(dev->CreateDepthStencilView(depth.Get(), &dd, &dsv)) ||
        FAILED(dev->CreateDepthStencilState(&ds, &dss)) || FAILED(dev->CreateRasterizerState(&rd, &rs))) return out;
    ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, float(ci::kSize), float(ci::kSize), 0.0f, 1.0f};
    ctx->OMSetRenderTargets(8, views, dsv.Get());
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    ctx->OMSetDepthStencilState(dss.Get(), 0);
    ctx->RSSetState(rs.Get());
    ctx->RSSetViewports(1, &vp);
    ctx->VSSetShader(sc.patched ? r.vs1.Get() : r.vs0.Get(), nullptr, 0);
    ctx->PSSetShader(sc.nullPs ? nullptr : sc.patched ? r.ps1.Get() : r.ps0.Get(), nullptr, 0);
    ctx->Draw(3, 0);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    for (unsigned i = 0; i < 4; ++i) if (tex[i]) out.target[i] = ci::readback(dev, ctx, tex[i].Get(), 16);
    out.depth = ci::readback(dev, ctx, depth.Get(), 4);
    if (sc.patched) {
        const auto raw = ci::readback(dev, ctx, tex[7].Get(), 16);
        out.e.resize(raw.size() / 4);
        if (!raw.empty()) std::memcpy(out.e.data(), raw.data(), raw.size());
        out.slot = ci::readback(dev, ctx, tex[6].Get(), 8);
    }
    out.ok = !out.depth.empty() && (!sc.patched || (!out.e.empty() && !out.slot.empty()));
    return out;
}

inline bool covered(const Draw& d, unsigned p) {
    float z;
    std::memcpy(&z, &d.depth[size_t(p) * 4], 4);
    return z > 0.0f;
}

struct Totals {
    unsigned pixels = 0, bad = 0;
    double worst = 0.0;
};

// Every covered pixel must hold E within `tolerance` of expect (cm) and the
// valid flag wanted (1: above 0.99; 0: exactly 0). tolerance 0: bit-exact.
inline Totals judge(const Draw& d, const float (&expect)[3], double absTol, double relTol, bool wantValid) {
    Totals t;
    for (unsigned p = 0; p < ci::kSize * ci::kSize; ++p) {
        if (!covered(d, p)) continue;
        const float* e = &d.e[size_t(p) * 4];
        ++t.pixels;
        bool bad = wantValid ? !(e[3] > 0.99f && e[3] < 1.01f) : e[3] != 0.0f;
        for (unsigned c = 0; c < 3; ++c) {
            const double diff = std::fabs(double(e[c]) - double(expect[c]));
            const double allow = absTol + relTol * std::fabs(double(expect[c]));
            if (!(diff <= allow)) bad = true;   // NaN fails
            t.worst = std::fmax(t.worst, diff);
        }
        if (bad) ++t.bad;
    }
    return t;
}

// Runs the six properties above for one pair. Returns the number of covered
// pixels examined in the identity test (0 means the pair was not exercised).
inline unsigned run(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::vector<BYTE>& vsCode,
                    const std::vector<BYTE>& psCode, const char* name, void (*check)(bool, const char*),
                    const std::vector<std::pair<unsigned, float>>* psMaterial = nullptr, const char* tag = "K") {
    Rig r;
    if (psMaterial) {
        r.psMaterial = true;
        r.psMaterialSet = *psMaterial;
    }
    r.dev = dev;
    r.ctx = ctx;
    r.vsStock = vsCode;
    r.psStock = psCode;
    r.name = name;
    r.check = check;
    edvr::EngineVelocityInputs in;
    std::string why;
    const auto fail = [&](const char* kind, const std::string& message) {
        char full[400];
        std::snprintf(full, sizeof(full), "%s.%s -- skin %s: %s", tag, kind, name, message.c_str());
        check(false, full);
        return 0u;
    };
    if (!edvr::engineVelocityDeriveInputs(vsCode.data(), vsCode.size(), in, why)) return fail("derive", why);
    if (in.skinRegister >= 32) return fail("derive", "the vertex shader takes the second skin");
    if (!edvr::engineVelocityPatchVsSkin(vsCode.data(), vsCode.size(), in.skinRegister, r.vsPatched, why)) return fail("patch", why);
    in.skinExport = true;
    if (!edvr::engineVelocityPatchPs(psCode.data(), psCode.size(), in, r.psPatched, why)) return fail("patch", why);
    if (FAILED(dev->CreateVertexShader(vsCode.data(), vsCode.size(), nullptr, &r.vs0)) ||
        FAILED(dev->CreateVertexShader(r.vsPatched.data(), r.vsPatched.size(), nullptr, &r.vs1)) ||
        FAILED(dev->CreatePixelShader(psCode.data(), psCode.size(), nullptr, &r.ps0)) ||
        FAILED(dev->CreatePixelShader(r.psPatched.data(), r.psPatched.size(), nullptr, &r.ps1))) {
        return fail("create", "the patched pair creates on WARP");
    }
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"INSTANCEANDMODELDATAINDEX", 0, DXGI_FORMAT_R32G32_UINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"PACKEDVERTEXDATAA", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"PACKEDVERTEXDATAB", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"PACKEDVERTEXDATAC", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(dev->CreateInputLayout(elements, 4, vsCode.data(), vsCode.size(), &r.layout))) {
        return fail("create", "input layout");
    }
    if (!ci::declarations(psCode, r.decls, why) || !ci::outputs(psCode, r.formats, why)) return fail("create", why);

    char label[200];
    unsigned examined = 0;
    // every check carries the case tag ("K4.identity -- skin <pair>: ...") so a mutation run can name the case that must catch it
    const auto emit = [&](bool ok, const char* kind) {
        char full[320];
        std::snprintf(full, sizeof(full), "%s.%s -- %s", tag, kind, label);
        check(ok, full);
    };
    // 1. the game's own targets, patched pair against stock pair
    for (unsigned seed = 1; seed <= 6; ++seed) {
        Rng rng(seed * 7919u);
        const unsigned bones = 1 + seed % 4;
        Scene sc;
        makeVertices(rng, bones, sc.verts);
        sc.cur = makeState(rng, bones, 3);
        sc.prev = sc.cur;
        sc.prev.base = 9;
        sc.dataSeed = (seed & 1u) ? 1u : 0u;   // set 1 has zeros and small integers in the buffers; it may discard everything
        sc.patched = false;
        const Draw stock = draw(r, sc);
        sc.patched = true;
        const Draw patched = draw(r, sc);
        std::snprintf(label, sizeof(label), "skin %s: the draws read back", name);
        emit(stock.ok && patched.ok, "draw");
        if (!stock.ok || !patched.ok) return 0;
        bool same = stock.depth == patched.depth;
        for (unsigned t = 0; t < 4; ++t) if (r.formats[t] != DXGI_FORMAT_UNKNOWN) same = same && stock.target[t] == patched.target[t];
        std::snprintf(label, sizeof(label), "skin %s: stock and patched pair write the game's targets and depth byte for byte (seed %u, %u bones)", name, seed, bones);
        emit(same, "targets");
        unsigned cover = 0;
        for (unsigned p = 0; p < ci::kSize * ci::kSize; ++p) cover += covered(stock, p) ? 1u : 0u;
        std::snprintf(label, sizeof(label), "skin %s: the triangle covers pixels (seed %u)", name, seed);
        emit(sc.dataSeed == 1 || cover > 20, "cover");
    }
    // 2. identity: previous state = current state, at another base
    unsigned totalPixels = 0;
    for (unsigned seed = 1; seed <= 12; ++seed) {
        Rng rng(seed * 104729u);
        const unsigned bones = 1 + seed % 4;
        Scene sc;
        makeVertices(rng, bones, sc.verts);
        sc.cur = makeState(rng, bones, 3 + seed % 5);
        sc.prev = sc.cur;
        sc.prev.base = 20 + seed % 7;
        const Draw d = draw(r, sc);
        const float zero[3] = {0, 0, 0};
        const Totals t = judge(d, zero, 0.0, 0.0, true);
        std::snprintf(label, sizeof(label), "skin %s: IDENTITY -- E is exactly zero and valid at every covered pixel (seed %u, %u bones; worst %g cm)", name, seed, bones, t.worst);
        emit(d.ok && t.pixels > 10 && t.bad == 0, "identity");
        totalPixels += t.pixels;
        // a covered pixel the patched PS did not write would read back as the clear value
        unsigned unwritten = 0;
        for (unsigned p = 0; p < ci::kSize * ci::kSize && d.ok; ++p) if (covered(d, p) && !(d.e[size_t(p) * 4 + 3] >= 0.0f)) ++unwritten;
        std::snprintf(label, sizeof(label), "skin %s: every covered pixel carries E (seed %u; %u unwritten)", name, seed, unwritten);
        emit(d.ok && unwritten == 0, "carries");
    }
    examined = totalPixels;
    // 3. pose: only the previous record's position moves
    for (unsigned seed = 1; seed <= 4; ++seed) {
        Rng rng(seed * 15485863u);
        const unsigned bones = 1 + seed % 4;
        Scene sc;
        makeVertices(rng, bones, sc.verts);
        sc.cur = makeState(rng, bones, 5);
        sc.prev = sc.cur;
        sc.prev.base = 11;
        const float dp[3] = {rng.range(-0.3f, 0.3f), rng.range(-0.3f, 0.3f), rng.range(-0.3f, 0.3f)};
        for (unsigned c = 0; c < 3; ++c) sc.prev.pos[c] += dp[c];
        const Draw d = draw(r, sc);
        const float expect[3] = {100.0f * dp[0], 100.0f * dp[1], 100.0f * dp[2]};
        const Totals t = judge(d, expect, 2e-3, 1e-4, true);
        std::snprintf(label, sizeof(label), "skin %s: POSE -- E is 100 x (previous - current position) cm (seed %u; worst %g)", name, seed, t.worst);
        emit(d.ok && t.pixels > 10 && t.bad == 0, "pose");
    }
    // 3b. the displacement the game adds after the pose chain is not part of E
    for (unsigned seed = 1; seed <= 3; ++seed) {
        Rng rng(seed * 49979687u);
        const unsigned bones = 1 + seed % 4;
        Scene sc;
        makeVertices(rng, bones, sc.verts);
        sc.cur = makeState(rng, bones, 6);
        sc.prev = sc.cur;
        sc.prev.base = 17;
        const float dp[3] = {rng.range(-0.3f, 0.3f), rng.range(-0.3f, 0.3f), rng.range(-0.3f, 0.3f)};
        for (unsigned c = 0; c < 3; ++c) sc.prev.pos[c] += dp[c];
        sc.vsFill = 1.0f;
        const Draw d = draw(r, sc);
        const float expect[3] = {100.0f * dp[0], 100.0f * dp[1], 100.0f * dp[2]};
        const Totals t = judge(d, expect, 2e-3, 1e-4, true);
        std::snprintf(label, sizeof(label), "skin %s: DISPLACEMENT -- with the vertex shader's b2 switched on E is still 100 x the position change (seed %u; worst %g)", name, seed, t.worst);
        emit(d.ok && t.pixels > 10 && t.bad == 0, "displacement");
    }
    // 4. palette translation, and the swap
    for (unsigned seed = 1; seed <= 4; ++seed) {
        Rng rng(seed * 32452843u);
        const unsigned bones = 1 + seed % 4;
        Scene sc;
        makeVertices(rng, bones, sc.verts);
        sc.cur = makeState(rng, bones, 4);
        for (unsigned c = 0; c < 3; ++c) sc.cur.quat[c] = 32768;   // the rotation is the identity to 1.5e-5
        sc.cur.quat[3] = 65535;
        sc.prev = sc.cur;
        sc.prev.base = 13;
        const float dv[3] = {rng.range(-0.2f, 0.2f), rng.range(-0.2f, 0.2f), rng.range(-0.2f, 0.2f)};
        for (unsigned b = 0; b < bones; ++b)
            for (unsigned c = 0; c < 3; ++c) sc.prev.rows[size_t(b) * 12 + c * 4 + 3] += dv[c];
        const Draw d = draw(r, sc);
        const float expect[3] = {100.0f * sc.cur.scale * dv[0], 100.0f * sc.cur.scale * dv[1], 100.0f * sc.cur.scale * dv[2]};
        const Totals t = judge(d, expect, 3e-2, 5e-3, true);
        std::snprintf(label, sizeof(label), "skin %s: PALETTE -- E is 100 x scale x the translation (seed %u; worst %g)", name, seed, t.worst);
        emit(d.ok && t.pixels > 10 && t.bad == 0, "palette");
        // swapped: the same two states with the roles exchanged give -E (float rounding: the interpolation weights differ)
        Scene sw = sc;
        std::swap(sw.cur, sw.prev);
        sw.cur.base = 4;
        sw.prev.base = 13;
        const Draw e = draw(r, sw);
        bool exact = d.ok && e.ok;
        unsigned compared = 0;
        for (unsigned p = 0; p < ci::kSize * ci::kSize && exact; ++p) if (covered(d, p) && covered(e, p)) {
            ++compared;
            for (unsigned c = 0; c < 3; ++c)
                exact = exact && std::fabs(double(d.e[size_t(p) * 4 + c]) + double(e.e[size_t(p) * 4 + c])) <= 1e-3 + 1e-5 * std::fabs(double(d.e[size_t(p) * 4 + c]));
        }
        // the covered sets differ slightly (the swap moves the triangle by a few millimetres); the interior must overlap
        std::snprintf(label, sizeof(label), "skin %s: SWAP -- exchanging the two frames negates E (seed %u; %u pixels)", name, seed, compared);
        emit(exact && compared > 10, "swap");
    }
    // 5. no history
    for (unsigned mode = 0; mode < 2; ++mode) {
        Rng rng(2468u + mode);
        Scene sc;
        makeVertices(rng, 3, sc.verts);
        sc.cur = makeState(rng, 3, 3);
        sc.prev = sc.cur;
        sc.prev.base = 11;
        sc.prev.pos[0] += 0.25f;
        if (mode == 0) sc.joinTo = 0;               // the join says no previous entity
        else sc.joinAt = 12, sc.joinTo = 11;        // the join has an entry, but not for this record's base
        const Draw d = draw(r, sc);
        const float zero[3] = {0, 0, 0};
        const Totals t = judge(d, zero, 0.0, 0.0, false);
        std::snprintf(label, sizeof(label), "skin %s: NO HISTORY (%s) -- E is zero and valid is zero", name, mode ? "no entry for the base" : "entry 0");
        emit(d.ok && t.pixels > 10 && t.bad == 0, "nohistory");
    }
    // 6. teeth: a join into the wrong block must give a nonzero E
    {
        Rng rng(13579u);
        Scene sc;
        makeVertices(rng, 3, sc.verts);
        sc.cur = makeState(rng, 3, 3);
        sc.prev = sc.cur;
        sc.prev.base = 11;
        // a different state sits at base 11 in the previous palette and pose
        State other = makeState(rng, 3, 11);
        other.pos[0] += 0.5f;
        sc.prev = other;
        const Draw d = draw(r, sc);
        unsigned nonzero = 0, pixels = 0;
        for (unsigned p = 0; p < ci::kSize * ci::kSize && d.ok; ++p) if (covered(d, p)) {
            ++pixels;
            nonzero += (d.e[size_t(p) * 4] != 0.0f || d.e[size_t(p) * 4 + 1] != 0.0f || d.e[size_t(p) * 4 + 2] != 0.0f) ? 1u : 0u;
        }
        std::snprintf(label, sizeof(label), "skin %s: TEETH -- a different previous state gives a nonzero E (%u of %u pixels)", name, nonzero, pixels);
        emit(d.ok && pixels > 10 && nonzero == pixels, "teeth");
    }
    std::printf("  skin clone: %s -- %u covered pixels in the identity test, E exactly zero; pose, palette, swap, no-history, teeth\n", name, examined);
    ctx->ClearState();
    return examined;
}

} // namespace skin_clone_tests
