// The VR Galaxy Map's replacement shaders (src\d3d11\galaxy_map_vs.h) on a WARP device with the D3D debug layer, through stream output:
//   G1 the icons' transcription (C31238331D8AC3D4, and 12382D2EA45E9632 with its b2 uv) against the formula in that header, on every class of record: the UI frame's, the
//      quads flat on the galactic plane, the identity quaternion, and the map's records that face the eye; both position encodings and a bone-indexed record
//   G2 the faces case with the eye equal to the map camera gives the game's own maths (the signs -v.x, +v.y, -v.z), and with the head turned the quad turns with it
//   G3 the arrow's transcription: the game's maths for a positive scale, the eye origin taken out for a negative one
//   G4 the probe on the GPU: a point at the arrow's tip is counted by an occlusion query where the mask is set and not where it is clear, a record with a positive scale
//      rasterizes nothing, and the layer's pixels do not change (write mask 0)
// The same shaders against the GAME's bytecode (the vs_*.dxbc of the shader dump) are compared by hand, with a one-off program that includes this file (GALAXY_MAP_VS_TEST_NO_MAIN):
// for the records the transcription leaves as the game draws them, every output agrees. The game's bytecode is not in the repository.
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

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "galaxy_map_ui.h"
#include "galaxy_map_vs.h"

using Microsoft::WRL::ComPtr;

namespace gmt {

inline int failures = 0;
inline unsigned checks = 0;
inline void check(bool ok, const std::string& label) {
    ++checks;
    if (!ok) { std::printf("FAIL %s\n", label.c_str()); ++failures; }
}
inline void hr(HRESULT h, const char* what) {
    if (FAILED(h)) { std::printf("FAIL D3D %s 0x%08lx\n", what, static_cast<unsigned long>(h)); std::exit(1); }
}
inline float asf(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
inline uint32_t asu(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

inline std::vector<uint8_t> compile(const char* source, const char* profile) {
    ComPtr<ID3DBlob> code, err;
    const HRESULT h = D3DCompile(source, std::strlen(source), "galaxy_map_vs", nullptr, nullptr, "main", profile, 0, 0, &code, &err);
    if (FAILED(h)) { std::printf("shader: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); std::exit(1); }
    const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
    return std::vector<uint8_t>(p, p + code->GetBufferSize());
}

struct Rig {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11InfoQueue> queue;
    bool debugLayer = false;
    Rig() {
        D3D_FEATURE_LEVEL fl;
        HRESULT h = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
        if (h == DXGI_ERROR_SDK_COMPONENT_MISSING)
            h = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
        else
            debugLayer = SUCCEEDED(h);
        hr(h, "CreateDevice");
        dev.As(&queue);
    }
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
};

// ---- one vertex shader under test, its stream-output twin and what it streams -----------------------------------------
struct Prog {
    std::vector<uint8_t> code;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11GeometryShader> so;
    int outFloats = 0;
    int uvAt = -1, posAt = -1;
};
inline Prog makeProg(Rig& rig, const std::vector<uint8_t>& code) {
    Prog p;
    p.code = code;
    ComPtr<ID3D11ShaderReflection> refl;
    hr(D3DReflect(code.data(), code.size(), __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(refl.GetAddressOf())), "D3DReflect");
    D3D11_SHADER_DESC desc{};
    refl->GetDesc(&desc);
    std::vector<D3D11_SO_DECLARATION_ENTRY> decl;
    std::vector<std::string> names;
    names.reserve(desc.OutputParameters);
    for (UINT i = 0; i < desc.OutputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC o{};
        refl->GetOutputParameterDesc(i, &o);
        const int comps = static_cast<int>(__popcnt(o.Mask));
        names.push_back(std::strcmp(o.SemanticName, "SV_POSITION") == 0 ? "SV_Position" : o.SemanticName);
        D3D11_SO_DECLARATION_ENTRY e{};
        e.SemanticName = names.back().c_str();
        e.SemanticIndex = o.SemanticIndex;
        e.ComponentCount = static_cast<BYTE>(comps);
        decl.push_back(e);
        if (std::strcmp(o.SemanticName, "TEXCOORD") == 0) p.uvAt = p.outFloats;
        if (std::strcmp(o.SemanticName, "SV_POSITION") == 0) p.posAt = p.outFloats;
        p.outFloats += comps;
    }
    UINT stride = static_cast<UINT>(p.outFloats * 4);
    hr(rig.dev->CreateVertexShader(code.data(), code.size(), nullptr, &p.vs), "CreateVertexShader");
    hr(rig.dev->CreateGeometryShaderWithStreamOutput(code.data(), code.size(), decl.data(), static_cast<UINT>(decl.size()), &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &p.so),
       "CreateGeometryShaderWithStreamOutput");
    return p;
}

// ---- the game's data layout -------------------------------------------------------------------------------------------
struct Rec { uint32_t a[4]; float p[4]; float pad[76]; };   // 336 bytes: a.x bone base, a.y the scale as a float, a.zw the packed quaternion; the position at +16
struct Bone { float r[12]; };                               // 48 bytes: three rows of float4
static_assert(sizeof(Rec) == 336 && sizeof(Bone) == 48, "the structured buffers' strides");
using Vert = std::array<uint32_t, 14>;                      // v0 (2), v1 (4), v2 (4), v3 (4)
struct Consts {
    float cb0[12][4] = {};
    float cb1[279][4] = {};
    float cb2[2][4] = {};
};
struct Scene {
    std::vector<Vert> verts;
    std::vector<Rec> recs;
    std::vector<Bone> bones;
    Consts c;
};

inline ComPtr<ID3D11InputLayout> makeLayout(Rig& rig, const std::vector<uint8_t>& code) {
    const D3D11_INPUT_ELEMENT_DESC el[4] = {
        {"INSTANCEANDMODELDATAINDEX", 0, DXGI_FORMAT_R32G32_UINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"PACKEDVERTEXDATAA", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"PACKEDVERTEXDATAB", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"PACKEDVERTEXDATAC", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ComPtr<ID3D11InputLayout> il;
    hr(rig.dev->CreateInputLayout(el, 4, code.data(), code.size(), &il), "CreateInputLayout");
    return il;
}

inline ComPtr<ID3D11Buffer> makeBuf(Rig& rig, const void* data, UINT bytes, UINT bind, UINT misc = 0, UINT stride = 0) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    d.MiscFlags = misc;
    d.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA s{data, 0, 0};
    ComPtr<ID3D11Buffer> b;
    hr(rig.dev->CreateBuffer(&d, data ? &s : nullptr, &b), "CreateBuffer");
    return b;
}
inline ComPtr<ID3D11ShaderResourceView> makeSrv(Rig& rig, ID3D11Buffer* b, UINT n) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    d.Buffer.FirstElement = 0;
    d.Buffer.NumElements = n;
    ComPtr<ID3D11ShaderResourceView> v;
    hr(rig.dev->CreateShaderResourceView(b, &d, &v), "CreateShaderResourceView");
    return v;
}

// One stream-output run of `prog` over the scene's vertices.
inline std::vector<float> run(Rig& rig, const Prog& prog, ID3D11InputLayout* il, const Scene& sc) {
    const UINT n = static_cast<UINT>(sc.verts.size());
    ComPtr<ID3D11Buffer> vb = makeBuf(rig, sc.verts.data(), n * sizeof(Vert), D3D11_BIND_VERTEX_BUFFER);
    ComPtr<ID3D11Buffer> cb0 = makeBuf(rig, sc.c.cb0, sizeof sc.c.cb0, D3D11_BIND_CONSTANT_BUFFER);
    ComPtr<ID3D11Buffer> cb1 = makeBuf(rig, sc.c.cb1, sizeof sc.c.cb1, D3D11_BIND_CONSTANT_BUFFER);
    ComPtr<ID3D11Buffer> cb2 = makeBuf(rig, sc.c.cb2, sizeof sc.c.cb2, D3D11_BIND_CONSTANT_BUFFER);
    Bone zeroBone{};
    const void* boneData = sc.bones.empty() ? &zeroBone : sc.bones.data();
    const UINT boneCount = sc.bones.empty() ? 1u : static_cast<UINT>(sc.bones.size());
    ComPtr<ID3D11Buffer> t33 = makeBuf(rig, sc.recs.data(), static_cast<UINT>(sc.recs.size() * sizeof(Rec)), D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(Rec));
    ComPtr<ID3D11Buffer> t38 = makeBuf(rig, boneData, boneCount * static_cast<UINT>(sizeof(Bone)), D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(Bone));
    ComPtr<ID3D11ShaderResourceView> s33 = makeSrv(rig, t33.Get(), static_cast<UINT>(sc.recs.size())), s38 = makeSrv(rig, t38.Get(), boneCount);
    D3D11_BUFFER_DESC sod{};
    sod.ByteWidth = n * static_cast<UINT>(prog.outFloats) * 4;
    sod.Usage = D3D11_USAGE_DEFAULT;
    sod.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    ComPtr<ID3D11Buffer> sob;
    hr(rig.dev->CreateBuffer(&sod, nullptr, &sob), "so buffer");

    rig.ctx->IASetInputLayout(il);
    ID3D11Buffer* vbs[1] = {vb.Get()};
    UINT stride = sizeof(Vert), offset = 0;
    rig.ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    rig.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    rig.ctx->VSSetShader(prog.vs.Get(), nullptr, 0);
    ID3D11Buffer* raw[3] = {cb0.Get(), cb1.Get(), cb2.Get()};
    rig.ctx->VSSetConstantBuffers(0, 3, raw);
    ID3D11ShaderResourceView* r33[1] = {s33.Get()};
    ID3D11ShaderResourceView* r38[1] = {s38.Get()};
    rig.ctx->VSSetShaderResources(33, 1, r33);
    rig.ctx->VSSetShaderResources(38, 1, r38);
    rig.ctx->GSSetShader(prog.so.Get(), nullptr, 0);
    rig.ctx->PSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer* sot[1] = {sob.Get()};
    UINT soOffset = 0;
    rig.ctx->SOSetTargets(1, sot, &soOffset);
    rig.ctx->Draw(n, 0);
    ID3D11Buffer* none[1] = {nullptr};
    rig.ctx->SOSetTargets(1, none, &soOffset);
    rig.ctx->GSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* noSrv[1] = {nullptr};
    rig.ctx->VSSetShaderResources(33, 1, noSrv);
    rig.ctx->VSSetShaderResources(38, 1, noSrv);

    D3D11_BUFFER_DESC st = sod;
    st.Usage = D3D11_USAGE_STAGING;
    st.BindFlags = 0;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> stage;
    hr(rig.dev->CreateBuffer(&st, nullptr, &stage), "staging");
    rig.ctx->CopyResource(stage.Get(), sob.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    hr(rig.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "Map");
    std::vector<float> out(static_cast<float*>(m.pData), static_cast<float*>(m.pData) + n * prog.outFloats);
    rig.ctx->Unmap(stage.Get(), 0);
    return out;
}

// ---- small double-precision maths -------------------------------------------------------------------------------------
struct V3 { double x, y, z; };
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 normalize(V3 a) { return a * (1.0 / std::sqrt(dot(a, a))); }
struct Q { double x, y, z, w; };
inline V3 rotate(Q q, V3 v) {
    const V3 u{q.x, q.y, q.z};
    return v * (q.w * 2.0 * q.w - 1.0) + u * (2.0 * dot(u, v)) + cross(u, v) * (2.0 * q.w);
}
inline Q quatFromColumns(V3 c0, V3 c1, V3 c2) {   // the rotation matrix with these columns
    const double m00 = c0.x, m10 = c0.y, m20 = c0.z, m01 = c1.x, m11 = c1.y, m21 = c1.z, m02 = c2.x, m12 = c2.y, m22 = c2.z;
    const double tr = m00 + m11 + m22;
    Q q;
    if (tr > 0) { const double s = std::sqrt(tr + 1.0) * 2; q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s}; }
    else if (m00 > m11 && m00 > m22) { const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2; q = {0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s}; }
    else if (m11 > m22) { const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2; q = {(m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s}; }
    else { const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2; q = {(m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s}; }
    return q;
}

// ---- the formulae of galaxy_map_vs.h, in plain code ---------------------------------------------------------------------
inline uint32_t packQ(double v) {   // [-1, 1] to the 16-bit field the game's shader unpacks with *(1/32767) - 1
    const double u = std::floor((v + 1.0) / static_cast<double>(asf(0x38000100u)) + 0.5);
    return static_cast<uint32_t>(std::fmin(65535.0, std::fmax(0.0, u)));
}
inline void writeQ(Rec& r, Q q) {
    r.a[2] = packQ(q.x) | (packQ(q.y) << 16);
    r.a[3] = packQ(q.z) | (packQ(q.w) << 16);
}
inline Q readQ(const Rec& r) {   // as the shader unpacks it, in float
    const float k = asf(0x38000100u);
    return {static_cast<double>(static_cast<float>(r.a[2] & 0xFFFFu) * k - 1.0f), static_cast<double>(static_cast<float>(r.a[2] >> 16) * k - 1.0f),
            static_cast<double>(static_cast<float>(r.a[3] & 0xFFFFu) * k - 1.0f), static_cast<double>(static_cast<float>(r.a[3] >> 16) * k - 1.0f)};
}
inline V3 refVertex(const Vert& v) {
    const uint32_t x = v[2], y = v[3], z = v[4];
    const uint32_t kind = (z >> 24) & 0x7Fu;
    if (kind == 64u) {
        const float lx = static_cast<float>(x & 0xFFFFu), hx = static_cast<float>(x >> 16), ly = static_cast<float>(y & 0xFFFFu), hy = static_cast<float>(y >> 16);
        const double e = std::exp2(static_cast<double>(hy * asf(0x39800080u))) - 1.0;
        const double k = asf(0x38000080u);
        return {(lx * k - 1.0) * e, (hx * k - 1.0) * e, (ly * k - 1.0) * e};
    }
    const int sh = 20 - static_cast<int>(kind);
    const double scale = 1.0 / static_cast<double>((1 << sh) - 1);
    const double off = static_cast<double>(1 << static_cast<int>(z >> 24));
    return {static_cast<double>(x & 0x1FFFFFu) * scale - off, static_cast<double>((x >> 21) + ((y & 0x3FFu) << 11)) * scale - off,
            static_cast<double>((y >> 10) & 0x1FFFFFu) * scale - off};
}
inline V3 refSkin(const Rec& r, const Vert& vert, const std::vector<Bone>& bones, V3 v) {
    if (r.a[0] == 0) return v;
    double m[12] = {};
    uint32_t sx = vert[10], sy = vert[11], n = 4;
    while (sy != 0 && n > 0) {
        const uint32_t bi = r.a[0] + (sx & 255u);
        const double w = static_cast<double>(sy & 255u) * static_cast<double>(asf(0x3B808081u));
        for (int i = 0; i < 12; ++i) m[i] += static_cast<double>(bones[bi].r[i]) * w;
        sx >>= 8; sy >>= 8; --n;
    }
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3], m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7], m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11]};
}
struct Ref { double uv[2]; double pos[4]; bool faces; };
inline double dp4(const float row[4], V3 p) { return row[0] * p.x + row[1] * p.y + row[2] * p.z + row[3]; }
// mode 0: the icon (C31238), 1: the icon with b2 (12382D), 2: the arrow (4EF6DD, replaced)
inline Ref reference(int mode, const Scene& sc, size_t i) {
    const Vert& vert = sc.verts[i];
    const Rec& rec = sc.recs[vert[0]];
    const Consts& c = sc.c;
    const float k = asf(0x38000100u);
    Ref o{};
    V3 v = refSkin(rec, vert, sc.bones, refVertex(vert));
    // uv and the billboard offset
    const uint32_t v2x = vert[6], v2z = vert[8];
    const double uvx = static_cast<double>(static_cast<float>(v2x & 0xFFFFu) * k - 1.0f), uvy = static_cast<double>(static_cast<float>(v2x >> 16) * k - 1.0f);
    const double bx = static_cast<double>(static_cast<float>(v2z & 0xFFFFu) * k - 1.0f) * 16.0, by = static_cast<double>(static_cast<float>(v2z >> 16) * k - 1.0f) * 16.0;
    if (mode == 1) { o.uv[0] = (uvx * c.cb2[1][0]) * 16.0 + c.cb2[1][2]; o.uv[1] = (uvy * c.cb2[1][1]) * 16.0 + c.cb2[1][3]; }
    else { o.uv[0] = uvx * 16.0; o.uv[1] = uvy * 16.0; }
    float s;
    std::memcpy(&s, &rec.a[1], 4);
    const V3 W{rec.p[0], rec.p[1], rec.p[2]};
    const V3 cam{c.cb1[275][0], c.cb1[275][1], c.cb1[275][2]};
    const Q q = readQ(rec);
    if (mode == 2) {
        const V3 center = W - (s < 0 ? V3{0, 0, 0} : cam);
        const V3 p = rotate(q, v) * s + center;
        for (int r = 0; r < 4; ++r) o.pos[r] = c.cb0[4 + r][0] * p.x + c.cb0[4 + r][1] * p.y + c.cb0[4 + r][2] * p.z + c.cb0[4 + r][3];
        return o;
    }
    const V3 center = W - cam;
    const V3 n = rotate(q, {0, 0, 1});
    o.faces = dot(W, W) > 1e6 && std::fabs(n.y) < 0.9995 && n.z < 0.9995;
    V3 pp;
    if (o.faces) {
        const V3 F = normalize({c.cb1[270][3], c.cb1[271][3], c.cb1[272][3]});
        const V3 Rx{c.cb1[270][0], c.cb1[271][0], c.cb1[272][0]}, Ry{c.cb1[270][1], c.cb1[271][1], c.cb1[272][1]};
        const V3 R = normalize(Rx - F * dot(Rx, F)), U = normalize(Ry - F * dot(Ry, F));
        pp = V3{dp4(c.cb0[9], center), dp4(c.cb0[10], center), dp4(c.cb0[11], center)} + (R * -v.x + U * v.y - F * v.z) * s;
    } else {
        const V3 p = rotate(q, v) * s + center;
        pp = {dp4(c.cb0[9], p), dp4(c.cb0[10], p), dp4(c.cb0[11], p)};
    }
    pp = pp + V3{c.cb1[277][0], c.cb1[277][1], c.cb1[277][2]} * bx + V3{c.cb1[278][0], c.cb1[278][1], c.cb1[278][2]} * by;
    for (int r = 0; r < 4; ++r) o.pos[r] = pp.x * c.cb1[270][r] + pp.y * c.cb1[271][r] + pp.z * c.cb1[272][r] + c.cb1[273][r];
    return o;
}

// ---- scenes -----------------------------------------------------------------------------------------------------------
enum class Cls { kUiFrame, kPlane, kIdentity, kFaces, kFacesSkinned, kFixedPoint };
struct Gen {
    std::mt19937 rng{20261104};
    double u(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }
    uint32_t r32() { return static_cast<uint32_t>(rng()); }
    V3 unitVec() { return normalize({u(-1, 1), u(-1, 1), u(-1, 1) + 0.01}); }
    Q randomQ() { Q q{u(-1, 1), u(-1, 1), u(-1, 1), u(-1, 1)}; const double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); return {q.x / l, q.y / l, q.z / l, q.w / l}; }
};
inline void setRow(float (&row)[4], double a, double b, double c, double d) { row[0] = (float)a; row[1] = (float)b; row[2] = (float)c; row[3] = (float)d; }
// A well-conditioned eye: affine rows 9..11 of b0 (a rotation with an offset, or the identity), the projection in rows 270..273 of b1 from the axes R, U, F.
inline void setEye(Gen& g, Consts& c, V3 R, V3 U, V3 F, bool identityWorld) {
    if (identityWorld) {
        setRow(c.cb0[9], 1, 0, 0, 0); setRow(c.cb0[10], 0, 1, 0, 0); setRow(c.cb0[11], 0, 0, 1, 0);
    } else {
        const Q q = g.randomQ();
        const V3 a = rotate(q, {1, 0, 0}) * 0.8, b = rotate(q, {0, 1, 0}) * 0.8, d = rotate(q, {0, 0, 1}) * 0.8;
        setRow(c.cb0[9], a.x, a.y, a.z, g.u(-2, 2)); setRow(c.cb0[10], b.x, b.y, b.z, g.u(-2, 2)); setRow(c.cb0[11], d.x, d.y, d.z, g.u(-2, 2));
    }
    const double fx = 0.94, fy = 0.94, za = 1.0, zb = -0.1;
    setRow(c.cb1[270], fx * R.x, fy * U.x, za * F.x, F.x);
    setRow(c.cb1[271], fx * R.y, fy * U.y, za * F.y, F.y);
    setRow(c.cb1[272], fx * R.z, fy * U.z, za * F.z, F.z);
    setRow(c.cb1[273], 0, 0, zb, 0);
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) c.cb0[4 + i][j] = (float)g.u(-1.5, 1.5);   // the arrow's own clip rows
    setRow(c.cb1[277], g.u(-0.02, 0.02), g.u(-0.02, 0.02), g.u(-0.02, 0.02), 0);
    setRow(c.cb1[278], g.u(-0.02, 0.02), g.u(-0.02, 0.02), g.u(-0.02, 0.02), 0);
    setRow(c.cb2[0], 0, 0, 0, 0);
    setRow(c.cb2[1], g.u(0.2, 1.5), g.u(0.2, 1.5), g.u(-0.5, 0.5), g.u(-0.5, 0.5));
}
inline void randomEye(Gen& g, Consts& c, bool identityWorld = false) {
    Q q = g.randomQ();
    setEye(g, c, rotate(q, {1, 0, 0}), rotate(q, {0, 1, 0}), rotate(q, {0, 0, 1}), identityWorld);
}

// One record and its vertex of the class cls; `bones` is the shared bone array. Positions are kept so the clip outputs stay a few hundred at most.
inline void addVertex(Gen& g, Scene& sc, Cls cls, bool encoding64, bool skinned) {
    Rec rec{};
    const bool map = cls == Cls::kFaces || cls == Cls::kFacesSkinned || cls == Cls::kPlane || cls == Cls::kIdentity;
    V3 W = map ? V3{49000 + g.u(0, 2000), 41070 + g.u(-2, 2), 23000 + g.u(0, 2000)} : V3{g.u(-300, 300), g.u(-300, 300), g.u(-300, 300)};
    rec.p[0] = (float)W.x; rec.p[1] = (float)W.y; rec.p[2] = (float)W.z; rec.p[3] = 1.0f;
    const float s = (float)g.u(0.2, 3.0);
    std::memcpy(&rec.a[1], &s, 4);
    Q q;
    if (cls == Cls::kPlane) {
        const double yaw = g.u(0, 6.2831853), half = yaw * 0.5, c45 = std::sqrt(0.5);
        const Q qy{0, std::sin(half), 0, std::cos(half)}, qx{-c45, 0, 0, c45};   // qy * qx: the plane's normal stays +y
        q = {qy.w * qx.x + qy.x * qx.w + qy.y * qx.z - qy.z * qx.y, qy.w * qx.y - qy.x * qx.z + qy.y * qx.w + qy.z * qx.x,
             qy.w * qx.z + qy.x * qx.y - qy.y * qx.x + qy.z * qx.w, qy.w * qx.w - qy.x * qx.x - qy.y * qx.y - qy.z * qx.z};
    } else if (cls == Cls::kIdentity) {
        q = {0, 0, 0, 1};
    } else if (cls == Cls::kFaces || cls == Cls::kFacesSkinned) {
        // a map quaternion: facing a head-free camera pitched 45 degrees, yawed at random
        const double yaw = g.u(-0.8, 0.8), cy = std::cos(yaw), sy = std::sin(yaw);
        const V3 right{cy, 0, -sy}, up{sy * 0.7071, 0.7071, cy * 0.7071}, fwd{sy * 0.7071, -0.7071, cy * 0.7071};
        q = quatFromColumns(right * -1.0, up, fwd * -1.0);
    } else {
        q = g.randomQ();
    }
    writeQ(rec, q);
    Vert v{};
    const uint32_t id = static_cast<uint32_t>(sc.recs.size());
    v[0] = id;
    if (encoding64) {
        v[2] = g.r32();
        v[3] = (g.r32() & 0xFFFFu) | (static_cast<uint32_t>(g.u(0, 3000)) << 16);   // the exponent word kept small
        v[4] = (64u << 24) | (g.r32() & 0x00FFFFFFu);
    } else {
        const uint32_t kind = 1 + g.r32() % 4;
        v[2] = g.r32();
        v[3] = g.r32();
        v[4] = (kind << 24) | (g.r32() & 0x00FFFFFFu);
    }
    v[6] = g.r32();   // PACKEDVERTEXDATAB.x: the uv
    v[8] = (32768u - 600u + g.r32() % 1200u) | ((32768u - 600u + g.r32() % 1200u) << 16);   // .z: the billboard offset, a few units
    if (skinned) {
        if (sc.bones.empty()) {
            for (int b = 0; b < 8; ++b) { Bone bn{}; for (int i = 0; i < 12; ++i) bn.r[i] = (float)g.u(-1, 1); bn.r[0] += 1; bn.r[5] += 1; bn.r[10] += 1; sc.bones.push_back(bn); }
        }
        rec.a[0] = 1 + g.r32() % 3;
        const unsigned count = 1 + g.r32() % 4;
        uint32_t idx = 0, wt = 0;
        for (unsigned k = 0; k < count; ++k) { idx |= (g.r32() % 4) << (8 * k); wt |= (1 + g.r32() % 255) << (8 * k); }
        v[10] = idx;
        v[11] = wt;
    }
    sc.recs.push_back(rec);
    sc.verts.push_back(v);
}

inline Scene makeScene(Gen& g, Cls cls, int n, bool encoding64, bool skinned, bool identityEye = false) {
    Scene sc;
    for (int i = 0; i < n; ++i) addVertex(g, sc, cls, encoding64, skinned);
    randomEye(g, sc.c, identityEye);
    // the eye origin within 30 units of the first record (all of a class sit within a few thousand of one another, so the centre is large for the others: scale the origin per record instead)
    return sc;
}

// The eye origin is a constant, so a scene's records must sit near it: put every record within 40 units of one point.
inline void gather(Gen& g, Scene& sc, V3 centre) {
    for (Rec& r : sc.recs) { r.p[0] = (float)(centre.x + g.u(-40, 40)); r.p[1] = (float)(centre.y + g.u(-40, 40)); r.p[2] = (float)(centre.z + g.u(-40, 40)); }
    setRow(sc.c.cb1[275], centre.x + g.u(-3, 3), centre.y + g.u(-3, 3), centre.z + g.u(-3, 3), 0);
}

struct Diff { double worst = 0; double mag = 0; size_t at = 0; int faces = 0; };
inline Diff compareRef(int mode, const std::vector<float>& out, const Prog& p, const Scene& sc, bool wantFaces, bool checkFaces) {
    Diff d;
    for (size_t i = 0; i < sc.verts.size(); ++i) {
        const Ref r = reference(mode, sc, i);
        if (r.faces) ++d.faces;
        if (checkFaces && r.faces != wantFaces) { d.worst = 1e9; d.at = i; return d; }
        for (int k = 0; k < 4; ++k) {
            const double got = out[i * p.outFloats + p.posAt + k];
            const double e = std::fabs(got - r.pos[k]);
            d.mag = std::fmax(d.mag, std::fabs(r.pos[k]));
            if (e > d.worst) { d.worst = e; d.at = i; }
        }
        for (int k = 0; k < 2; ++k) {
            const double got = out[i * p.outFloats + p.uvAt + k];
            const double e = std::fabs(got - r.uv[k]);
            if (e > d.worst) { d.worst = e; d.at = i; }
        }
    }
    return d;
}

// ---- the tests --------------------------------------------------------------------------------------------------------
inline void runIconTests(Rig& rig, const Prog& icon, const Prog& iconUv, ID3D11InputLayout* il) {
    Gen g;
    struct Row { const char* name; Cls cls; bool enc64, skinned, faces; };
    const Row rows[] = {
        {"UI frame, fixed-point", Cls::kUiFrame, false, false, false},
        {"UI frame, exponent", Cls::kUiFrame, true, false, false},
        {"UI frame, skinned", Cls::kUiFrame, false, true, false},
        {"galactic plane", Cls::kPlane, false, false, false},
        {"galactic plane, exponent", Cls::kPlane, true, false, false},
        {"identity quaternion", Cls::kIdentity, false, false, false},
        {"map, faces the eye, fixed-point", Cls::kFaces, false, false, true},
        {"map, faces the eye, exponent", Cls::kFaces, true, false, true},
        {"map, faces the eye, skinned", Cls::kFacesSkinned, false, true, true},
    };
    for (int variant = 0; variant < 2; ++variant) {
        const Prog& prog = variant ? iconUv : icon;
        for (const Row& r : rows) {
            Scene sc = makeScene(g, r.cls, 600, r.enc64, r.skinned);
            const bool mapClass = r.cls == Cls::kFaces || r.cls == Cls::kFacesSkinned || r.cls == Cls::kPlane || r.cls == Cls::kIdentity;
            gather(g, sc, mapClass ? V3{49996.0, 41070.0, 23990.0} : V3{g.u(-100, 100), g.u(-100, 100), g.u(-100, 100)});
            const std::vector<float> out = run(rig, prog, il, sc);
            const Diff d = compareRef(variant ? 1 : 0, out, prog, sc, r.faces, true);
            char label[160];
            std::snprintf(label, sizeof label, "G1.%s.%s: worst %.2e over %zu vertices (largest output %.1f), %d facing", variant ? "uv" : "plain", r.name, d.worst, sc.verts.size(), d.mag, d.faces);
            std::printf("%s\n", label);
            check(d.worst < 2e-5 * std::fmax(1.0, d.mag), label);
            check(r.faces ? d.faces == static_cast<int>(sc.verts.size()) : d.faces == 0, std::string("G1.class.") + r.name);
        }
    }
}

inline void runFacesAsGame(Rig& rig, const Prog& icon, ID3D11InputLayout* il) {
    Gen g;
    // the eye IS the map camera: the faces case reproduces the game's own maths (rows (1,0,0), (0,.7071,.7071), (0,-.7071,.7071))
    const V3 R{1, 0, 0}, U{0, 0.70710678, 0.70710678}, F{0, -0.70710678, 0.70710678};
    Scene sc;
    for (int i = 0; i < 800; ++i) addVertex(g, sc, Cls::kFaces, i & 1, false);
    // the quaternion of the CPU's records: rotate(q, ex) = -R, rotate(q, ey) = U, rotate(q, ez) = -F
    const Q q = quatFromColumns(R * -1.0, U, F * -1.0);
    for (Rec& r : sc.recs) writeQ(r, q);
    setEye(g, sc.c, R, U, F, true);
    gather(g, sc, V3{49996.0, 41070.0, 23990.0});
    const std::vector<float> out = run(rig, icon, il, sc);
    // the game's maths for the same inputs: the reference with faces forced off
    double worst = 0, mag = 0;
    int faces = 0;
    for (size_t i = 0; i < sc.verts.size(); ++i) {
        Ref viaFaces = reference(0, sc, i);
        if (viaFaces.faces) ++faces;
        const Vert& vert = sc.verts[i];
        const Rec& rec = sc.recs[vert[0]];
        const V3 v = refVertex(vert);
        float s; std::memcpy(&s, &rec.a[1], 4);
        const V3 center = V3{rec.p[0], rec.p[1], rec.p[2]} - V3{sc.c.cb1[275][0], sc.c.cb1[275][1], sc.c.cb1[275][2]};
        const V3 p = rotate(readQ(rec), v) * s + center;
        V3 pp{dp4(sc.c.cb0[9], p), dp4(sc.c.cb0[10], p), dp4(sc.c.cb0[11], p)};
        const uint32_t v2z = vert[8];
        const double bx = static_cast<double>(static_cast<float>(v2z & 0xFFFFu) * asf(0x38000100u) - 1.0f) * 16.0, by = static_cast<double>(static_cast<float>(v2z >> 16) * asf(0x38000100u) - 1.0f) * 16.0;
        pp = pp + V3{sc.c.cb1[277][0], sc.c.cb1[277][1], sc.c.cb1[277][2]} * bx + V3{sc.c.cb1[278][0], sc.c.cb1[278][1], sc.c.cb1[278][2]} * by;
        for (int k = 0; k < 4; ++k) {
            const double game = pp.x * sc.c.cb1[270][k] + pp.y * sc.c.cb1[271][k] + pp.z * sc.c.cb1[272][k] + sc.c.cb1[273][k];
            worst = std::fmax(worst, std::fabs(out[i * icon.outFloats + icon.posAt + k] - game));
            mag = std::fmax(mag, std::fabs(game));
        }
    }
    char label[160];
    std::snprintf(label, sizeof label, "G2: with the eye equal to the map camera the faces case gives the game's maths: worst %.2e over %zu vertices (largest %.1f), %d facing", worst, sc.verts.size(), mag, faces);
    std::printf("%s\n", label);
    check(faces == static_cast<int>(sc.verts.size()) && worst < 1e-2, label);

    // and with the head turned the quad turns with it: one icon's corners, the eye yawed about its own up axis. The quad's edge on screen times clip w is 2 s fx whatever the yaw
    // (the game's fixed plane would foreshorten by the cosine).
    double edge[2] = {};
    const double yaw[2] = {0.0, 0.5};
    for (int e = 0; e < 2; ++e) {
        Scene sc2;
        Rec rec{};
        rec.p[0] = 49996.0f; rec.p[1] = 41070.0f; rec.p[2] = 23990.0f; rec.p[3] = 1.0f;
        const float one = 1.0f;
        std::memcpy(&rec.a[1], &one, 4);
        writeQ(rec, q);
        sc2.recs.push_back(rec);
        for (int k = 0; k < 2; ++k) {
            Vert v{};
            const uint32_t kind = 4;
            const double scale = 1.0 / ((1 << (20 - kind)) - 1), off = static_cast<double>(1 << kind);
            const double cx = k ? 1.0 : -1.0;
            const uint32_t tx = static_cast<uint32_t>(std::llround((cx + off) / scale)), ty = static_cast<uint32_t>(std::llround(off / scale)), tz = ty;
            v[2] = (tx & 0x1FFFFFu) | ((ty & 0x7FFu) << 21);
            v[3] = ((ty >> 11) & 0x3FFu) | ((tz & 0x1FFFFFu) << 10);
            v[4] = kind << 24;
            v[8] = 32768u | (32768u << 16);   // b = 0 (to within the packing)
            sc2.verts.push_back(v);
        }
        const V3 Ry = R * std::cos(yaw[e]) - F * std::sin(yaw[e]), Fy = F * std::cos(yaw[e]) + R * std::sin(yaw[e]);
        setEye(g, sc2.c, Ry, U, Fy, true);
        setRow(sc2.c.cb1[275], 49996.0 + 0.3, 41070.0 + 0.2, 23990.0 - 2.0, 0);
        setRow(sc2.c.cb1[277], 0, 0, 0, 0);
        setRow(sc2.c.cb1[278], 0, 0, 0, 0);
        const std::vector<float> o2 = run(rig, icon, il, sc2);
        const float* a0 = &o2[0 * icon.outFloats + icon.posAt];
        const float* a1 = &o2[1 * icon.outFloats + icon.posAt];
        edge[e] = std::hypot(a1[0] / a1[3] - a0[0] / a0[3], a1[1] / a1[3] - a0[1] / a0[3]) * a0[3];
    }
    char label2[200];
    std::snprintf(label2, sizeof label2, "G2b: yawing the eye 0.5 rad leaves the icon's edge (screen size times w) at %.5f against %.5f (2 s fx = 1.88000)", edge[0], edge[1]);
    std::printf("%s\n", label2);
    check(std::fabs(edge[0] - 1.88) < 2e-3 && std::fabs(edge[1] - 1.88) < 2e-3, label2);
}

inline void runArrowTests(Rig& rig, const Prog& arrow, ID3D11InputLayout* il) {
    Gen g;
    for (int sign = 0; sign < 2; ++sign) {
        for (int enc = 0; enc < 2; ++enc) {
            Scene sc = makeScene(g, Cls::kUiFrame, 600, enc != 0, sign == 0 && enc == 0);
            gather(g, sc, V3{g.u(-0.5, 0.5), g.u(-0.5, 0.5), g.u(-0.5, 0.5)});
            for (Rec& r : sc.recs) {
                float s; std::memcpy(&s, &r.a[1], 4);
                if (sign) { s = -s; std::memcpy(&r.a[1], &s, 4); }
                r.p[0] = (float)g.u(-1, 1); r.p[1] = (float)g.u(-1, 1); r.p[2] = (float)g.u(-1, 1);
            }
            const std::vector<float> out = run(rig, arrow, il, sc);
            const Diff d = compareRef(2, out, arrow, sc, false, false);
            char label[160];
            std::snprintf(label, sizeof label, "G3.%s.%s: worst %.2e over %zu vertices (largest output %.1f)", sign ? "negative scale" : "positive scale", enc ? "exponent" : "fixed-point", d.worst, sc.verts.size(), d.mag);
            std::printf("%s\n", label);
            check(d.worst < 2e-5 * std::fmax(1.0, d.mag), label);
        }
    }
}

inline void runProbeTest(Rig& rig, const std::vector<uint8_t>& vsCode, const std::vector<uint8_t>& psCode) {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    hr(rig.dev->CreateVertexShader(vsCode.data(), vsCode.size(), nullptr, &vs), "probe vs");
    hr(rig.dev->CreatePixelShader(psCode.data(), psCode.size(), nullptr, &ps), "probe ps");
    const D3D11_INPUT_ELEMENT_DESC el[1] = {{"INSTANCEANDMODELDATAINDEX", 0, DXGI_FORMAT_R32G32_UINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ComPtr<ID3D11InputLayout> il;
    hr(rig.dev->CreateInputLayout(el, 1, vsCode.data(), vsCode.size(), &il), "probe layout");
    const UINT W = 8;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = W;
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> layer, mask;
    hr(rig.dev->CreateTexture2D(&td, nullptr, &layer), "layer");
    td.Format = DXGI_FORMAT_A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr(rig.dev->CreateTexture2D(&td, nullptr, &mask), "mask");
    ComPtr<ID3D11RenderTargetView> layerRtv, maskRtv;
    ComPtr<ID3D11ShaderResourceView> maskSrv;
    hr(rig.dev->CreateRenderTargetView(layer.Get(), nullptr, &layerRtv), "layer rtv");
    hr(rig.dev->CreateRenderTargetView(mask.Get(), nullptr, &maskRtv), "mask rtv");
    hr(rig.dev->CreateShaderResourceView(mask.Get(), nullptr, &maskSrv), "mask srv");
    const float clearLayer[4] = {0.2f, 0.4f, 0.6f, 1.0f}, clearMask[4] = {0, 0, 0, 0};
    rig.ctx->ClearRenderTargetView(layerRtv.Get(), clearLayer);
    rig.ctx->ClearRenderTargetView(maskRtv.Get(), clearMask);
    // the mask: one pixel set, by the accumulating blend (a source alpha of 1 written through a one-pixel scissor)
    const D3D11_BLEND_DESC mbd = edvr::mapui::maskBlendDesc(), pbd = edvr::mapui::probeBlendDesc();
    ComPtr<ID3D11BlendState> maskBlend, probeBlend;
    hr(rig.dev->CreateBlendState(&mbd, &maskBlend), "mask blend");
    hr(rig.dev->CreateBlendState(&pbd, &probeBlend), "probe blend");
    {
        // fill the mask pixel (3, 3) with a pass-through shader pair of our own: clear a one-pixel region through the scissor
        ComPtr<ID3D11RasterizerState> rs;
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.ScissorEnable = TRUE; rd.DepthClipEnable = TRUE;
        hr(rig.dev->CreateRasterizerState(&rd, &rs), "rs");
        const char* fsVs = "float4 main(uint id : SV_VERTEXID) : SV_POSITION { float2 p = float2((id << 1) & 2, id & 2); return float4(p * 2.0 - 1.0, 0, 1); }";
        const char* fsPs = "float4 main() : SV_TARGET { return float4(0, 0, 0, 0.9); }";
        const std::vector<uint8_t> fvs = compile(fsVs, "vs_5_0"), fps = compile(fsPs, "ps_5_0");
        ComPtr<ID3D11VertexShader> v2;
        ComPtr<ID3D11PixelShader> p2;
        hr(rig.dev->CreateVertexShader(fvs.data(), fvs.size(), nullptr, &v2), "fs vs");
        hr(rig.dev->CreatePixelShader(fps.data(), fps.size(), nullptr, &p2), "fs ps");
        ID3D11RenderTargetView* rt[1] = {maskRtv.Get()};
        rig.ctx->OMSetRenderTargets(1, rt, nullptr);
        const D3D11_VIEWPORT vp{0, 0, (float)W, (float)W, 0, 1};
        rig.ctx->RSSetViewports(1, &vp);
        const D3D11_RECT sc{3, 3, 4, 4};
        rig.ctx->RSSetScissorRects(1, &sc);
        rig.ctx->RSSetState(rs.Get());
        rig.ctx->OMSetBlendState(maskBlend.Get(), nullptr, 0xFFFFFFFFu);
        rig.ctx->IASetInputLayout(nullptr);
        rig.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        rig.ctx->VSSetShader(v2.Get(), nullptr, 0);
        rig.ctx->PSSetShader(p2.Get(), nullptr, 0);
        rig.ctx->Draw(3, 0);
        // the accumulate: a second draw of alpha 0.9 gives 1 - 0.1 * 0.1 = 0.99
        rig.ctx->Draw(3, 0);
    }
    // read the mask back
    {
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;
        hr(rig.dev->CreateTexture2D(&td, nullptr, &stage), "mask staging");
        rig.ctx->CopyResource(stage.Get(), mask.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(rig.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "map mask");
        const uint8_t* row = static_cast<const uint8_t*>(m.pData);
        const int a33 = row[3 * m.RowPitch + 3], a00 = row[0], a44 = row[4 * m.RowPitch + 4];
        rig.ctx->Unmap(stage.Get(), 0);
        char label[160];
        std::snprintf(label, sizeof label, "G4.mask: the accumulating blend leaves 1 - 0.1 * 0.1 = 0.99 (252.5) in the pixel drawn twice and nothing around it: %d, %d, %d", a33, a00, a44);
        std::printf("%s\n", label);
        check(a33 >= 252 && a33 <= 253 && a00 == 0 && a44 == 0, label);
    }
    // the probe: records at the pixel (3, 3) with a negative scale, at (3, 3) with a positive scale, and at (5, 5)
    struct Case { const char* name; float s; float ndcX, ndcY; uint64_t expect; };
    const float cx = ((3.5f / W) * 2.0f - 1.0f), cy = 1.0f - (3.5f / W) * 2.0f, ox = ((5.5f / W) * 2.0f - 1.0f), oy = 1.0f - (5.5f / W) * 2.0f;
    const Case cases[3] = {{"the tip on the mask", -0.4f, cx, cy, 1}, {"a positive scale (not the arrow)", 0.4f, cx, cy, 0}, {"the tip off the mask", -0.4f, ox, oy, 0}};
    Consts c{};
    setRow(c.cb0[4], 1, 0, 0, 0); setRow(c.cb0[5], 0, 1, 0, 0); setRow(c.cb0[6], 0, 0, 1, 0); setRow(c.cb0[7], 0, 0, 0, 1);
    setRow(c.cb1[275], 0.5, 0.25, 0, 0);
    ComPtr<ID3D11Buffer> cb0 = makeBuf(rig, c.cb0, 8 * 16, D3D11_BIND_CONSTANT_BUFFER), cb1 = makeBuf(rig, c.cb1, 276 * 16, D3D11_BIND_CONSTANT_BUFFER);
    for (const Case& k : cases) {
        Rec rec{};
        std::memcpy(&rec.a[1], &k.s, 4);
        rec.p[0] = k.ndcX + 0.5f; rec.p[1] = k.ndcY + 0.25f; rec.p[2] = 0.5f; rec.p[3] = 1;
        ComPtr<ID3D11Buffer> t33 = makeBuf(rig, &rec, sizeof rec, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof rec);
        ComPtr<ID3D11ShaderResourceView> s33 = makeSrv(rig, t33.Get(), 1);
        const uint32_t vtx[2] = {0, 0};
        ComPtr<ID3D11Buffer> vb = makeBuf(rig, vtx, sizeof vtx, D3D11_BIND_VERTEX_BUFFER);
        D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
        ComPtr<ID3D11Query> q;
        hr(rig.dev->CreateQuery(&qd, &q), "query");
        ID3D11RenderTargetView* rt[1] = {layerRtv.Get()};
        rig.ctx->OMSetRenderTargets(1, rt, nullptr);
        const D3D11_VIEWPORT vp{0, 0, (float)W, (float)W, 0, 1};
        rig.ctx->RSSetViewports(1, &vp);
        rig.ctx->RSSetState(nullptr);
        rig.ctx->OMSetBlendState(probeBlend.Get(), nullptr, 0xFFFFFFFFu);
        rig.ctx->IASetInputLayout(il.Get());
        ID3D11Buffer* vbs[1] = {vb.Get()};
        UINT stride = 8, offset = 0;
        rig.ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
        rig.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        rig.ctx->VSSetShader(vs.Get(), nullptr, 0);
        ID3D11Buffer* cbs[2] = {cb0.Get(), cb1.Get()};
        rig.ctx->VSSetConstantBuffers(0, 2, cbs);
        ID3D11ShaderResourceView* r33[1] = {s33.Get()};
        rig.ctx->VSSetShaderResources(33, 1, r33);
        rig.ctx->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* m[1] = {maskSrv.Get()};
        rig.ctx->PSSetShaderResources(0, 1, m);
        rig.ctx->Begin(q.Get());
        rig.ctx->Draw(1, 0);
        rig.ctx->End(q.Get());
        UINT64 samples = 99;
        HRESULT h = S_FALSE;
        for (int spin = 0; spin < 100000 && h == S_FALSE; ++spin) h = rig.ctx->GetData(q.Get(), &samples, sizeof samples, 0);
        char label[160];
        std::snprintf(label, sizeof label, "G4.probe.%s: %llu sample(s)", k.name, static_cast<unsigned long long>(samples));
        std::printf("%s\n", label);
        check(h == S_OK && samples == k.expect, label);
        ID3D11ShaderResourceView* none[1] = {nullptr};
        rig.ctx->PSSetShaderResources(0, 1, none);
        rig.ctx->VSSetShaderResources(33, 1, none);
    }
    // the layer is untouched: write mask 0
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D11Texture2D> stage;
    hr(rig.dev->CreateTexture2D(&td, nullptr, &stage), "layer staging");
    rig.ctx->CopyResource(stage.Get(), layer.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    hr(rig.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "map layer");
    bool same = true;
    for (UINT y = 0; y < W; ++y)
        for (UINT x = 0; x < W; ++x) {
            const uint8_t* px = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4;
            same = same && px[0] == 51 && px[1] == 102 && px[2] == 153 && px[3] == 255;
        }
    rig.ctx->Unmap(stage.Get(), 0);
    check(same, "G4.layer: the probe's write mask 0 leaves every pixel of the layer as it was");
}

inline int runAll() {
    Rig rig;
    std::printf("galaxy_map_vs_test: debug layer %s\n", rig.debugLayer ? "ON" : "not installed (messages not checked)");
    const std::vector<uint8_t> icon = compile(edvr::kMapIconFacingVS, "vs_5_0"), iconUv = compile(edvr::kMapIconUvFacingVS, "vs_5_0"), arrow = compile(edvr::kMapArrowDepthVS, "vs_5_0");
    const std::vector<uint8_t> probeVs = compile(edvr::kMapProbeVS, "vs_5_0"), probePs = compile(edvr::kMapProbePS, "ps_5_0");
    const Prog pIcon = makeProg(rig, icon), pIconUv = makeProg(rig, iconUv), pArrow = makeProg(rig, arrow);
    ComPtr<ID3D11InputLayout> il = makeLayout(rig, icon), ilUv = makeLayout(rig, iconUv), ilArrow = makeLayout(rig, arrow);
    check(pIcon.uvAt == 0 && pIcon.posAt == 2 && pIconUv.uvAt == 0 && pIconUv.posAt == 2 && pArrow.uvAt == 0 && pArrow.posAt == 2,
          "G0: the outputs are TEXCOORD0.xy at o0 and SV_POSITION at o1, as the game's");
    runIconTests(rig, pIcon, pIconUv, il.Get());
    runFacesAsGame(rig, pIcon, il.Get());
    runArrowTests(rig, pArrow, ilArrow.Get());
    runProbeTest(rig, probeVs, probePs);
    check(rig.problems() == 0, "the debug layer reported nothing");
    std::printf("galaxy_map_vs_test: %u checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}

} // namespace gmt

#ifndef GALAXY_MAP_VS_TEST_NO_MAIN
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string mode = argv[1];
    if (mode == "--dry-run") {
        std::puts("galaxy_map_vs_test: G0-G4, WARP with the debug layer; reads nothing, writes nothing");
        return 0;
    }
    if (mode != "--self-test") return 2;
    return gmt::runAll();
}
#endif
