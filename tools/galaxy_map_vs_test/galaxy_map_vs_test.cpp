// The VR Galaxy Map's replacement shaders (src\d3d11\galaxy_map_vs.h) on a WARP device with the D3D debug layer, through stream output:
//   G1 the icons' transcription (C31238331D8AC3D4, and 12382D2EA45E9632 with its b2 uv) against the formula in that header, on every class of record: the UI frame's, the
//      quads flat on the galactic plane, the identity quaternion, and the map's records that face along the ray; both position encodings and a bone-indexed record
//   G2 every map quad fixed in the world: the same record with the eye turned (0.5 rad of yaw, 0.2 rad of pitch about the eye's position) gives the same world position at
//      every corner, for a facing icon, a label, an identity-quaternion triangle spanned by b, and a ring
//   G3 square-on and level: the eye looking along the ray with its up in the plane of mapUp and the ray, and rows 277 and 278 equal to its axes, gives a level top edge and the
//      game's own maths for the icon, the b span and the ring; negated rows keep the game's on-screen orientation; a ray within a degree of mapUp uses the eye's right and writes no NaN
//   G4 the ring's transcription against its formula, random instances
//   G5 the arrow's transcription: the game's maths for a positive scale, the eye origin taken out for a negative one
//   G7 the icons point down at the grid: at the map camera's forward the icon's corners are the label's; away from it the icon's up in the image runs along the projected grid
//      vertical (a stem's direction) and the label's does not; a ray within 16 degrees of the vertical takes the camera's up; no NaN at the vertical
//   G8 the sprites' depth with the real states and pixel shaders: reversed depth (a nearer clip position is larger), a far quad drawn after a near one is rejected where the near one's
//      alpha is at or over kMapDepthAlpha and shows where it is under, near over far in either order, a second quad in the same plane draws whole at D16, the bias is four steps farther
//   G6 the probe on the GPU: a point at the arrow's tip is counted by an occlusion query where the mask is set, or set on both sides of it along an axis within reach, and not
//      where it is clear or set on one side only, a record with a positive scale rasterizes nothing, and the layer's pixels do not change (write mask 0)
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
    float cb10[1][4] = {};   // MapCam: mapUp
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
inline std::vector<float> runRaw(Rig& rig, const Prog& prog, ID3D11InputLayout* il, const void* vdata, UINT n, UINT vstride, const std::vector<Rec>& recs, const std::vector<Bone>& bones, const Consts& cc) {
    ComPtr<ID3D11Buffer> vb = makeBuf(rig, vdata, n * vstride, D3D11_BIND_VERTEX_BUFFER);
    ComPtr<ID3D11Buffer> cb0 = makeBuf(rig, cc.cb0, sizeof cc.cb0, D3D11_BIND_CONSTANT_BUFFER);
    ComPtr<ID3D11Buffer> cb1 = makeBuf(rig, cc.cb1, sizeof cc.cb1, D3D11_BIND_CONSTANT_BUFFER);
    ComPtr<ID3D11Buffer> cb2 = makeBuf(rig, cc.cb2, sizeof cc.cb2, D3D11_BIND_CONSTANT_BUFFER);
    ComPtr<ID3D11Buffer> cb10 = makeBuf(rig, cc.cb10, sizeof cc.cb10, D3D11_BIND_CONSTANT_BUFFER);
    Bone zeroBone{};
    const void* boneData = bones.empty() ? &zeroBone : bones.data();
    const UINT boneCount = bones.empty() ? 1u : static_cast<UINT>(bones.size());
    ComPtr<ID3D11Buffer> t33 = makeBuf(rig, recs.data(), static_cast<UINT>(recs.size() * sizeof(Rec)), D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(Rec));
    ComPtr<ID3D11Buffer> t38 = makeBuf(rig, boneData, boneCount * static_cast<UINT>(sizeof(Bone)), D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(Bone));
    ComPtr<ID3D11ShaderResourceView> s33 = makeSrv(rig, t33.Get(), static_cast<UINT>(recs.size())), s38 = makeSrv(rig, t38.Get(), boneCount);
    D3D11_BUFFER_DESC sod{};
    sod.ByteWidth = n * static_cast<UINT>(prog.outFloats) * 4;
    sod.Usage = D3D11_USAGE_DEFAULT;
    sod.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    ComPtr<ID3D11Buffer> sob;
    hr(rig.dev->CreateBuffer(&sod, nullptr, &sob), "so buffer");

    rig.ctx->IASetInputLayout(il);
    ID3D11Buffer* vbs[1] = {vb.Get()};
    UINT stride = vstride, offset = 0;
    rig.ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    rig.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    rig.ctx->VSSetShader(prog.vs.Get(), nullptr, 0);
    ID3D11Buffer* raw[3] = {cb0.Get(), cb1.Get(), cb2.Get()};
    rig.ctx->VSSetConstantBuffers(0, 3, raw);
    rig.ctx->VSSetConstantBuffers(10, 1, cb10.GetAddressOf());
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
inline std::vector<float> run(Rig& rig, const Prog& prog, ID3D11InputLayout* il, const Scene& sc) {
    return runRaw(rig, prog, il, sc.verts.data(), static_cast<UINT>(sc.verts.size()), sizeof(Vert), sc.recs, sc.bones, sc.c);
}

// The rings' vertex (94EA991C3A03DBD9): TEXCOORD0 (4), POSITION0 (3), POSITION2 (4), TEXCOORD3 (4), POSITION4 (4), all float.
struct RingVert { float v0[4]; float v1[3]; float v2[4]; float v3[4]; float v4[4]; };
static_assert(sizeof(RingVert) == 76, "the rings' vertex stride");
struct RingScene {
    std::vector<RingVert> verts;
    Consts c;
};
inline ComPtr<ID3D11InputLayout> makeRingLayout(Rig& rig, const std::vector<uint8_t>& code) {
    const D3D11_INPUT_ELEMENT_DESC el[5] = {
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"POSITION", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 44, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"POSITION", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 60, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ComPtr<ID3D11InputLayout> il;
    hr(rig.dev->CreateInputLayout(el, 5, code.data(), code.size(), &il), "CreateInputLayout (ring)");
    return il;
}
inline std::vector<float> runRing(Rig& rig, const Prog& prog, ID3D11InputLayout* il, const RingScene& sc) {
    const std::vector<Rec> dummy(1);   // the ring reads no record; the rig binds one
    return runRaw(rig, prog, il, sc.verts.data(), static_cast<UINT>(sc.verts.size()), sizeof(RingVert), dummy, std::vector<Bone>(), sc.c);
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
inline double dp3(const float row[4], V3 p) { return row[0] * p.x + row[1] * p.y + row[2] * p.z; }
// galaxy_map_vs.h mapAxes, in double
inline void refAxes(V3 ray, V3 up, V3 Fe, V3 Re, V3& F, V3& R, V3& U) {
    F = dot(ray, ray) > 1e-12 ? normalize(ray) : Fe;
    const V3 c = cross(up, F);
    const double cl = std::sqrt(dot(c, c));
    R = cl > 0.02 ? c * (1.0 / cl) : normalize(Re - F * dot(Re, F));
    U = cross(F, R);
}
// galaxy_map_vs.h mapIconUp, in double: the grid's vertical below 35 degrees from it, the camera's above 16, a smoothstep between
inline V3 refIconUp(V3 ray, V3 gridUp, V3 cameraUp) {
    const double a = dot(ray, ray) > 1e-12 ? std::fabs(dot(normalize(ray), gridUp)) : 0.0;
    double t = std::fmin(1.0, std::fmax(0.0, (a - 0.82) / (0.96 - 0.82)));
    t = t * t * (3.0 - 2.0 * t);
    return normalize(gridUp * (1.0 - t) + cameraUp * t);
}
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
    const bool world = dot(W, W) > 1e6;
    const V3 pcf{dp4(c.cb0[9], center), dp4(c.cb0[10], center), dp4(c.cb0[11], center)};
    const V3 r277{c.cb1[277][0], c.cb1[277][1], c.cb1[277][2]}, r278{c.cb1[278][0], c.cb1[278][1], c.cb1[278][2]};
    V3 F{0, 0, 0}, R{0, 0, 0}, U{0, 0, 0};
    double sx = 1.0, sy = 1.0;
    if (world) {
        const V3 Fe = normalize({c.cb1[270][3], c.cb1[271][3], c.cb1[272][3]});
        const V3 Rx{c.cb1[270][0], c.cb1[271][0], c.cb1[272][0]}, Ry{c.cb1[270][1], c.cb1[271][1], c.cb1[272][1]};
        const V3 Re = normalize(Rx - Fe * dot(Rx, Fe)), Ue = normalize(Ry - Fe * dot(Ry, Fe));
        const V3 mu{c.cb10[0][0], c.cb10[0][1], c.cb10[0][2]};
        const V3 up{dp3(c.cb0[9], mu), dp3(c.cb0[10], mu), dp3(c.cb0[11], mu)};
        const V3 gridUp{c.cb0[9][1], c.cb0[10][1], c.cb0[11][1]};   // dot(float4(0, 1, 0, 0), cb0[9..11])
        refAxes(pcf, mode == 0 ? refIconUp(pcf, gridUp, up) : up, Fe, Re, F, R, U);   // the label (mode 1) keeps the camera's up
        sx = dot(r277, Re) < 0 ? -1.0 : 1.0;
        sy = dot(r278, Ue) < 0 ? -1.0 : 1.0;
    }
    if (o.faces) {
        pp = pcf + (R * -v.x + U * v.y - F * v.z) * s;
    } else {
        const V3 p = rotate(q, v) * s + center;
        pp = {dp4(c.cb0[9], p), dp4(c.cb0[10], p), dp4(c.cb0[11], p)};
    }
    if (world) pp = pp + R * (bx * sx * std::sqrt(dot(r277, r277))) + U * (by * sy * std::sqrt(dot(r278, r278)));
    else pp = pp + r277 * bx + r278 * by;
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
    const V3 mu = normalize({g.u(-0.3, 0.3), g.u(0.7, 1.0), g.u(-0.3, 0.3)});   // the map camera's up (MapCam)
    setRow(c.cb10[0], mu.x, mu.y, mu.z, 0);
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
        {"map, faces along the ray, fixed-point", Cls::kFaces, false, false, true},
        {"map, faces along the ray, exponent", Cls::kFaces, true, false, true},
        {"map, faces along the ray, skinned", Cls::kFacesSkinned, false, true, true},
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

// ---- every map quad fixed in the world --------------------------------------------------------------------------------------
struct Frame { V3 R, U, F; };
// The eye looking along f, its up in the plane of `up` and f: right x up = forward.
inline Frame frameAlong(V3 f, V3 up) {
    const V3 F = normalize(f);
    const V3 U = normalize(up - F * dot(up, F));
    return {cross(U, F), U, F};
}
inline V3 rodrigues(V3 v, V3 axis, double ang) {
    const double c = std::cos(ang), s = std::sin(ang);
    return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0 - c));
}
inline void mat4Mul(const double m[4][4], const double x[4], double out[4]) {
    for (int r = 0; r < 4; ++r) out[r] = m[r][0] * x[0] + m[r][1] * x[1] + m[r][2] * x[2] + m[r][3] * x[3];
}
inline bool mat4Inv(const double m[4][4], double inv[4][4]) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r][c] : (c - 4 == r ? 1.0 : 0.0);
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col + 1; r < 4; ++r) if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
        if (std::fabs(a[piv][col]) < 1e-12) return false;
        for (int c = 0; c < 8; ++c) std::swap(a[col][c], a[piv][c]);
        const double d = a[col][col];
        for (int c = 0; c < 8; ++c) a[col][c] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const double f = a[r][col];
            for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) inv[r][c] = a[r][c + 4];
    return true;
}
// The constants of a clean eye at E looking with axes `fr`: the clip rows in b0 [4..7] (the rings'), the identity frame in b0 [9..11], the projection in b1 [270..273] (the icons'),
// the eye origin at b1 [275], rows 277 and 278 the eye's right and up scaled by lx and ly (negative values negate them), the exponent factor 1 and the uv scale 1, mapUp in b10.
inline Consts cleanEye(const Frame& fr, V3 E, V3 mapUp, double lx, double ly) {
    Consts c{};
    const double fx = 0.94, fy = 0.94, za = 1.0, zb = -0.1;
    setRow(c.cb0[4], fx * fr.R.x, fx * fr.R.y, fx * fr.R.z, -fx * dot(fr.R, E));
    setRow(c.cb0[5], fy * fr.U.x, fy * fr.U.y, fy * fr.U.z, -fy * dot(fr.U, E));
    setRow(c.cb0[6], za * fr.F.x, za * fr.F.y, za * fr.F.z, -za * dot(fr.F, E) + zb);
    setRow(c.cb0[7], fr.F.x, fr.F.y, fr.F.z, -dot(fr.F, E));
    setRow(c.cb0[9], 1, 0, 0, 0); setRow(c.cb0[10], 0, 1, 0, 0); setRow(c.cb0[11], 0, 0, 1, 0);
    setRow(c.cb1[270], fx * fr.R.x, fy * fr.U.x, za * fr.F.x, fr.F.x);
    setRow(c.cb1[271], fx * fr.R.y, fy * fr.U.y, za * fr.F.y, fr.F.y);
    setRow(c.cb1[272], fx * fr.R.z, fy * fr.U.z, za * fr.F.z, fr.F.z);
    setRow(c.cb1[273], 0, 0, zb, 0);
    setRow(c.cb1[275], E.x, E.y, E.z, 0);
    setRow(c.cb1[277], fr.R.x * lx, fr.R.y * lx, fr.R.z * lx, 0);
    setRow(c.cb1[278], fr.U.x * ly, fr.U.y * ly, fr.U.z * ly, 0);
    setRow(c.cb2[0], 1, 0, 0, 0);
    setRow(c.cb2[1], 1, 1, 0, 0);
    setRow(c.cb10[0], mapUp.x, mapUp.y, mapUp.z, 0);
    return c;
}
// clip = M (position, 1): the icons' M from b1 [270..273] and the rings' from b0 [4..7].
inline bool iconUndo(const Consts& c, const float* clip, V3& pos) {
    double m[4][4], inv[4][4];
    for (int r = 0; r < 4; ++r) { for (int j = 0; j < 3; ++j) m[r][j] = c.cb1[270 + j][r]; m[r][3] = c.cb1[273][r]; }
    if (!mat4Inv(m, inv)) return false;
    const double x[4] = {clip[0], clip[1], clip[2], clip[3]};
    double h[4];
    mat4Mul(inv, x, h);
    pos = {h[0] / h[3], h[1] / h[3], h[2] / h[3]};
    return true;
}
inline bool ringUndo(const Consts& c, const float* clip, V3& pos) {
    double m[4][4], inv[4][4];
    for (int r = 0; r < 4; ++r) for (int j = 0; j < 4; ++j) m[r][j] = c.cb0[4 + r][j];
    if (!mat4Inv(m, inv)) return false;
    const double x[4] = {clip[0], clip[1], clip[2], clip[3]};
    double h[4];
    mat4Mul(inv, x, h);
    pos = {h[0] / h[3], h[1] / h[3], h[2] / h[3]};
    return true;
}

constexpr V3 kCam{49996.0, 41070.0, 23990.0};   // an eye origin near the map's records: exact in float, so W - cam is exact
constexpr V3 kRingEye{120.0, -60.0, 340.0};   // the rings read absolute positions through the clip rows: an eye near the origin keeps the float cancellation small
inline uint32_t packB(double b) { return static_cast<uint32_t>(std::floor((b / 16.0 + 1.0) / static_cast<double>(asf(0x38000100u)) + 0.5)); }
inline double decodeB(uint32_t field) { return static_cast<double>(static_cast<float>(field) * asf(0x38000100u) - 1.0f) * 16.0; }
inline void packFixed(Vert& v, V3 p) {   // the fixed-point encoding, kind 4: 16 fraction bits, offset 16
    const uint32_t kind = 4;
    const double scale = 1.0 / ((1 << (20 - kind)) - 1), off = static_cast<double>(1 << kind);
    const uint32_t tx = static_cast<uint32_t>(std::llround((p.x + off) / scale)), ty = static_cast<uint32_t>(std::llround((p.y + off) / scale)), tz = static_cast<uint32_t>(std::llround((p.z + off) / scale));
    v[2] = (tx & 0x1FFFFFu) | ((ty & 0x7FFu) << 21);
    v[3] = ((ty >> 11) & 0x3FFu) | ((tz & 0x1FFFFFu) << 10);
    v[4] = kind << 24;
}
struct Corner { V3 v; double bx, by; };
// One record (quaternion q, scale s) at cam + center with these corners.
inline Scene iconScene(const Consts& c, Q q, V3 center, float s, const std::vector<Corner>& corners) {
    Scene sc;
    sc.c = c;
    Rec rec{};
    rec.p[0] = static_cast<float>(kCam.x + center.x); rec.p[1] = static_cast<float>(kCam.y + center.y); rec.p[2] = static_cast<float>(kCam.z + center.z); rec.p[3] = 1.0f;
    std::memcpy(&rec.a[1], &s, 4);
    writeQ(rec, q);
    sc.recs.push_back(rec);
    for (const Corner& k : corners) {
        Vert v{};
        packFixed(v, k.v);
        v[8] = packB(k.bx) | (packB(k.by) << 16);
        sc.verts.push_back(v);
    }
    return sc;
}
inline V3 iconDecodedB(const Vert& v) { return {decodeB(v[8] & 0xFFFFu), decodeB(v[8] >> 16), 0}; }
// The game's own maths for an icon vertex in the identity frame: rotate(q, v) * s + center + cb1[277] * b.x + cb1[278] * b.y, through the projection.
inline void gameIconClip(const Scene& sc, size_t i, double out[4]) {
    const Vert& vert = sc.verts[i];
    const Rec& rec = sc.recs[vert[0]];
    float s; std::memcpy(&s, &rec.a[1], 4);
    const V3 center = V3{rec.p[0], rec.p[1], rec.p[2]} - V3{sc.c.cb1[275][0], sc.c.cb1[275][1], sc.c.cb1[275][2]};
    const V3 b = iconDecodedB(vert);
    const V3 pp = rotate(readQ(rec), refVertex(vert)) * s + center + V3{sc.c.cb1[277][0], sc.c.cb1[277][1], sc.c.cb1[277][2]} * b.x + V3{sc.c.cb1[278][0], sc.c.cb1[278][1], sc.c.cb1[278][2]} * b.y;
    for (int r = 0; r < 4; ++r) out[r] = pp.x * sc.c.cb1[270][r] + pp.y * sc.c.cb1[271][r] + pp.z * sc.c.cb1[272][r] + sc.c.cb1[273][r];
}
inline Q mapCameraQ() {   // the CPU's record quaternion facing the head-free map camera: rotate(q, ex) = -R, rotate(q, ey) = U, rotate(q, ez) = -F
    const V3 R{1, 0, 0}, U{0, 0.70710678, 0.70710678}, F{0, -0.70710678, 0.70710678};
    return quatFromColumns(R * -1.0, U, F * -1.0);
}
inline RingScene ringScene(const Consts& c, V3 v1, const std::vector<std::array<double, 2>>& bs, float k) {
    RingScene sc;
    sc.c = c;
    for (const auto& b : bs) {
        RingVert r{};
        r.v0[0] = 0.25f; r.v0[1] = -0.5f; r.v0[2] = static_cast<float>(b[0]); r.v0[3] = static_cast<float>(b[1]);
        r.v1[0] = static_cast<float>(v1.x); r.v1[1] = static_cast<float>(v1.y); r.v1[2] = static_cast<float>(v1.z);
        r.v2[0] = 0.3f; r.v2[1] = 0.4f; r.v2[2] = 0.5f; r.v2[3] = 0.6f;
        r.v3[2] = k;
        r.v4[0] = 0.5f; r.v4[1] = 0.25f; r.v4[2] = 0.8f; r.v4[3] = 1.0f;
        sc.verts.push_back(r);
    }
    return sc;
}
inline double ndcY(const float* clip) { return clip[1] / clip[3]; }

struct WorldProgs { const Prog* icon; const Prog* label; const Prog* ring; ID3D11InputLayout* ilIcon; ID3D11InputLayout* ilLabel; ID3D11InputLayout* ilRing; };

// G2: the same record under three eyes (A, A yawed 0.5 rad about its own up, A pitched 0.2 rad about its own right, all at the same position) has the same world position at
// every corner, for the four kinds; the clip outputs do change (the test sees the eye move).
inline void runWorldStability(Rig& rig, const WorldProgs& w) {
    const V3 mapUp = normalize({0.05, 0.97, 0.12});
    const Frame a = frameAlong({0.3, -0.5, 0.8}, {0.12, 1.0, 0.18});   // a rolled head
    const Frame b{rodrigues(a.R, a.U, 0.5), a.U, rodrigues(a.F, a.U, 0.5)};
    const Frame c{a.R, rodrigues(a.U, a.R, 0.2), rodrigues(a.F, a.R, 0.2)};
    const Frame eyes[3] = {a, b, c};
    const char* names[3] = {"the eye as it was", "yawed 0.5 rad", "pitched 0.2 rad"};
    struct Kind { const char* name; bool ring; bool label; Q q; std::vector<Corner> corners; std::vector<std::array<double, 2>> bs; };
    const Q mq = mapCameraQ(), idq{0, 0, 0, 1};
    std::vector<Corner> quad, tri;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
            quad.push_back({{static_cast<double>(sx), static_cast<double>(sy), 0.0}, 0.7, -0.4});
            tri.push_back({{0, 0, 0}, 2.0 * sx, 2.0 * sy});
        }
    const Kind kinds[] = {
        {"a facing icon", false, false, mq, quad, {}},
        {"a facing label", false, true, mq, quad, {}},
        {"an identity-quaternion triangle spanned by b", false, false, idq, tri, {}},
        {"a ring", true, false, idq, {}, {{-2, -2}, {2, -2}, {-2, 2}, {2, 2}}},
    };
    // two centres: 37 degrees from the vertical (the grid's up, fully) and 26 (inside the hand-over from the grid's up to the camera's)
    for (const V3 center : {V3{7, -20, 15}, V3{10, -25, 6}})
    for (const Kind& k : kinds) {
        std::vector<V3> pos[3];
        double clipDelta = 0;
        std::vector<float> clip0;
        for (int e = 0; e < 3; ++e) {
            const V3 E = k.ring ? kRingEye : kCam;
            const Consts cc = cleanEye(eyes[e], E, mapUp, 0.013, 0.011);
            std::vector<float> out;
            const Prog* prog = k.ring ? w.ring : (k.label ? w.label : w.icon);
            if (k.ring) {
                const RingScene sc = ringScene(cc, E + center, k.bs, 2.0f);
                out = runRing(rig, *prog, w.ilRing, sc);
            } else {
                const Scene sc = iconScene(cc, k.q, center, 1.3f, k.corners);
                out = run(rig, *prog, k.label ? w.ilLabel : w.ilIcon, sc);
            }
            const size_t n = out.size() / prog->outFloats;
            for (size_t i = 0; i < n; ++i) {
                V3 p{};
                const float* clip = &out[i * prog->outFloats + prog->posAt];
                const bool ok = k.ring ? ringUndo(cc, clip, p) : iconUndo(cc, clip, p);
                check(ok && std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z), std::string("G2.finite: ") + k.name);
                pos[e].push_back(p);
                if (e == 0) clip0.push_back(clip[0]);
                if (e == 1) clipDelta = std::fmax(clipDelta, std::fabs(clip[0] - clip0[i]));
            }
        }
        double worst = 0;
        for (int e = 1; e < 3; ++e)
            for (size_t i = 0; i < pos[0].size(); ++i) worst = std::fmax(worst, std::sqrt(dot(pos[e][i] - pos[0][i], pos[e][i] - pos[0][i])));
        // the ring's position is absolute (the eye at kCam), the icons' relative to the eye: the undo above is each shader's own frame, and both are fixed under the eye's turn
        char label[200];
        std::snprintf(label, sizeof label, "G2.%s, centre (%g, %g, %g): world position moved by at most %.2e under %s and %s (the clip x moved by %.3f)", k.name, center.x, center.y, center.z, worst, names[1], names[2], clipDelta);
        std::printf("%s\n", label);
        check(worst < 2e-4 && clipDelta > 1e-2, label);
    }
}

inline bool allFinite(const std::vector<float>& v) {
    for (float f : v) if (!std::isfinite(f)) return false;
    return true;
}

// G3: square-on and level, the game's maths when the eye looks along the ray, the signs, and a ray at the up.
inline void runSquareOn(Rig& rig, const WorldProgs& w) {
    // (a) the map camera is the eye and the record sits on its forward: the facing icon is the game's maths within the quaternion's quantisation, and its top edge is level
    {
        const V3 R{1, 0, 0}, U{0, 0.70710678, 0.70710678}, F{0, -0.70710678, 0.70710678};
        const Frame fr{R, U, F};
        const Consts cc = cleanEye(fr, kCam, U, 0.02, 0.02);
        std::vector<Corner> quad;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) quad.push_back({{static_cast<double>(sx), static_cast<double>(sy), 0.0}, 0.9 * sx, -0.6 * sy});
        const Scene sc = iconScene(cc, mapCameraQ(), {0, -24, 24}, 1.7f, quad);
        const std::vector<float> out = run(rig, *w.icon, w.ilIcon, sc);
        double worst = 0;
        for (size_t i = 0; i < quad.size(); ++i) {
            double g[4];
            gameIconClip(sc, i, g);
            for (int k = 0; k < 4; ++k) worst = std::fmax(worst, std::fabs(out[i * w.icon->outFloats + w.icon->posAt + k] - g[k]));
        }
        // quad order: (-1,-1), (-1,+1), (+1,-1), (+1,+1): the top corners are 1 and 3
        const double top = std::fabs(ndcY(&out[1 * w.icon->outFloats + w.icon->posAt]) - ndcY(&out[3 * w.icon->outFloats + w.icon->posAt]));
        char label[200];
        std::snprintf(label, sizeof label, "G3.icon square-on: corners within %.2e of the game's maths (quaternion quantisation), top edge level to %.2e in NDC", worst, top);
        std::printf("%s\n", label);
        check(worst < 2e-3 && top < 1e-4, label);
        // at the map camera's own forward the grid's up and the camera's give the same axes: the icon's corners are the label's
        const std::vector<float> lab = run(rig, *w.label, w.ilLabel, sc);
        double same = 0;
        for (size_t i = 0; i < quad.size(); ++i)
            for (int k = 0; k < 4; ++k) same = std::fmax(same, std::fabs(out[i * w.icon->outFloats + w.icon->posAt + k] - lab[i * w.label->outFloats + w.label->posAt + k]));
        std::snprintf(label, sizeof label, "G7.forward: at the map camera's forward the icon's corners equal the label's to %.2e", same);
        std::printf("%s\n", label);
        check(same < 1e-4, label);
    }
    // (b) a general ray: the facing icon's top edge is level when the eye looks along the ray with its up in the plane of mapUp and the ray
    const V3 mapUp = normalize({0.1, 0.9, 0.2});
    const V3 gridUp{0, 1, 0};
    const V3 ray{7, -20, 15};   // 37 degrees from the vertical: the icons and rings are spanned on the grid's up here, the labels on the map camera's
    const Frame fr = frameAlong(ray, mapUp), frg = frameAlong(ray, gridUp);
    {
        const Consts cc = cleanEye(frg, kCam, mapUp, 0.02, 0.02);
        std::vector<Corner> quad;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) quad.push_back({{static_cast<double>(sx), static_cast<double>(sy), 0.0}, 0.0, 0.0});
        const Scene sc = iconScene(cc, mapCameraQ(), ray, 1.7f, quad);
        const std::vector<float> out = run(rig, *w.icon, w.ilIcon, sc);
        const double top = std::fabs(ndcY(&out[1 * w.icon->outFloats + w.icon->posAt]) - ndcY(&out[3 * w.icon->outFloats + w.icon->posAt]));
        const double bottom = std::fabs(ndcY(&out[0 * w.icon->outFloats + w.icon->posAt]) - ndcY(&out[2 * w.icon->outFloats + w.icon->posAt]));
        char label[200];
        std::snprintf(label, sizeof label, "G3.icon level on a general ray (eye up in the plane of the grid's up and the ray): top edge %.2e and bottom edge %.2e in NDC", top, bottom);
        std::printf("%s\n", label);
        check(top < 1e-4 && bottom < 1e-4, label);
    }
    {
        const Consts cc = cleanEye(fr, kCam, mapUp, 0.02, 0.02);
        std::vector<Corner> quad;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) quad.push_back({{static_cast<double>(sx), static_cast<double>(sy), 0.0}, 0.0, 0.0});
        const Scene sc = iconScene(cc, mapCameraQ(), ray, 1.7f, quad);
        const std::vector<float> out = run(rig, *w.label, w.ilLabel, sc);
        const double top = std::fabs(ndcY(&out[1 * w.label->outFloats + w.label->posAt]) - ndcY(&out[3 * w.label->outFloats + w.label->posAt]));
        const double bottom = std::fabs(ndcY(&out[0 * w.label->outFloats + w.label->posAt]) - ndcY(&out[2 * w.label->outFloats + w.label->posAt]));
        char label[200];
        std::snprintf(label, sizeof label, "G3.label level on a general ray (eye up in the plane of the map camera's up and the ray): top edge %.2e and bottom edge %.2e in NDC", top, bottom);
        std::printf("%s\n", label);
        check(top < 1e-4 && bottom < 1e-4, label);
    }
    // (c) the b span of an identity-quaternion triangle and (d) the ring: level, and the game's maths with rows 277 and 278 equal to the eye's axes; (e) the same with both rows negated
    for (int neg = 0; neg < 2; ++neg) {
        const double lx = neg ? -0.5 : 0.5, ly = neg ? -0.7 : 0.7;
        const Consts cc = cleanEye(frg, kCam, mapUp, lx, ly);
        std::vector<Corner> tri;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) tri.push_back({{0, 0, 0}, 3.0 * sx, 3.0 * sy});
        const Scene sc = iconScene(cc, Q{0, 0, 0, 1}, ray, 1.0f, tri);
        const std::vector<float> out = run(rig, *w.icon, w.ilIcon, sc);
        double worst = 0;
        for (size_t i = 0; i < tri.size(); ++i) {
            double g[4];
            gameIconClip(sc, i, g);
            for (int k = 0; k < 4; ++k) worst = std::fmax(worst, std::fabs(out[i * w.icon->outFloats + w.icon->posAt + k] - g[k]));
        }
        const double top = std::fabs(ndcY(&out[1 * w.icon->outFloats + w.icon->posAt]) - ndcY(&out[3 * w.icon->outFloats + w.icon->posAt]));
        char label[220];
        std::snprintf(label, sizeof label, "G3.b span%s: within %.2e of the game's maths, top edge level to %.2e in NDC", neg ? " with rows 277 and 278 negated" : "", worst, top);
        std::printf("%s\n", label);
        check(worst < 2e-3 && top < 1e-4, label);

        const Consts ccr = cleanEye(frg, kRingEye, mapUp, lx, ly);
        const RingScene rs = ringScene(ccr, kRingEye + ray, {{-3, -3}, {-3, 3}, {3, -3}, {3, 3}}, 1.0f);
        const std::vector<float> ro = runRing(rig, *w.ring, w.ilRing, rs);
        double rworst = 0;
        for (size_t i = 0; i < rs.verts.size(); ++i) {
            const V3 v1{rs.verts[i].v1[0], rs.verts[i].v1[1], rs.verts[i].v1[2]};
            const V3 p = v1 + V3{ccr.cb1[277][0], ccr.cb1[277][1], ccr.cb1[277][2]} * rs.verts[i].v0[2] + V3{ccr.cb1[278][0], ccr.cb1[278][1], ccr.cb1[278][2]} * rs.verts[i].v0[3];
            for (int k = 0; k < 4; ++k) rworst = std::fmax(rworst, std::fabs(ro[i * w.ring->outFloats + w.ring->posAt + k] - dp4(ccr.cb0[4 + k], p)));
        }
        const double rtop = std::fabs(ndcY(&ro[1 * w.ring->outFloats + w.ring->posAt]) - ndcY(&ro[3 * w.ring->outFloats + w.ring->posAt]));
        std::snprintf(label, sizeof label, "G3.ring%s: within %.2e of the game's maths, top edge level to %.2e in NDC", neg ? " with rows 277 and 278 negated" : "", rworst, rtop);
        std::printf("%s\n", label);
        check(rworst < 2e-3 && rtop < 1e-4, label);
    }
    // (f) a ray at the up, and within a degree of it: no NaN anywhere, the basis stays orthonormal, and the right is the eye's right made orthogonal to the ray
    for (const double deg : {0.0, 0.5, 1.0}) {
        const V3 up{0, 1, 0};
        const double r = deg * 3.14159265358979 / 180.0;
        const V3 rayN{30.0 * std::sin(r), 30.0 * std::cos(r), 0.0};
        const Frame eye = frameAlong({0.3, -0.2, 0.9}, {0.1, 1.0, 0.0});
        const Consts cc = cleanEye(eye, kCam, up, 0.5, 0.5);
        std::vector<Corner> quad;
        quad.push_back({{1, 0, 0}, 0, 0});
        quad.push_back({{0, 1, 0}, 0, 0});
        quad.push_back({{0, 0, 1}, 0, 0});
        const Scene sc = iconScene(cc, mapCameraQ(), rayN, 2.0f, quad);
        const std::vector<float> out = run(rig, *w.icon, w.ilIcon, sc);
        const V3 cen{sc.recs[0].p[0] - kCam.x, sc.recs[0].p[1] - kCam.y, sc.recs[0].p[2] - kCam.z};   // the record is stored as floats
        bool ok = allFinite(out);
        // the offsets from the centre: -R, U, -F times the scale
        V3 off[3];
        for (int i = 0; i < 3; ++i) {
            V3 p{};
            ok = ok && iconUndo(cc, &out[i * w.icon->outFloats + w.icon->posAt], p);
            off[i] = p - cen;
        }
        const V3 R = off[0] * (-0.5), U = off[1] * 0.5, F = off[2] * (-0.5);
        const V3 Fx = normalize(cen);
        const V3 reye = normalize(eye.R - Fx * dot(eye.R, Fx));
        const double orth = std::fmax(std::fabs(dot(R, U)), std::fmax(std::fabs(dot(R, F)), std::fabs(dot(U, F))));
        const double lens = std::fabs(dot(R, R) - 1.0) + std::fabs(dot(U, U) - 1.0) + std::fabs(dot(F, F) - 1.0);
        const double fallback = dot(R, reye);   // within the threshold (about 1.15 degrees) the right is the eye's, made orthogonal to the ray
        char label[220];
        std::snprintf(label, sizeof label, "G3.up: a ray %.1f degrees from mapUp: finite %d, orthonormal to %.1e (lengths %.1e), right . eye's right %.6f", deg, allFinite(out) ? 1 : 0, orth, lens, fallback);
        std::printf("%s\n", label);
        check(ok && orth < 2e-4 && lens < 2e-3 && fallback > 0.9999, label);
        // the b span and the ring on the same ray
        const Scene tri = iconScene(cc, Q{0, 0, 0, 1}, rayN, 1.0f, {{{0, 0, 0}, 3, 0}, {{0, 0, 0}, 0, 3}});
        const RingScene rs = ringScene(cleanEye(eye, kRingEye, up, 0.5, 0.5), kRingEye + rayN, {{3, 0}, {0, 3}}, 1.0f);
        check(allFinite(run(rig, *w.icon, w.ilIcon, tri)) && allFinite(runRing(rig, *w.ring, w.ilRing, rs)), "G3.up: the b span and the ring write no NaN on a ray at the up");
    }
}

// G4: the ring's transcription against its formula, every output, random instances and eyes (rows 277 and 278 near the eye's right and up with random signs and sizes).
inline void runRingTests(Rig& rig, const WorldProgs& w) {
    Gen g;
    double worst = 0, mag = 0;
    unsigned n = 0, fallbacks = 0;
    for (int scene = 0; scene < 6; ++scene) {
        const Q qe = g.randomQ();
        const Frame fr{rotate(qe, {1, 0, 0}), rotate(qe, {0, 1, 0}), rotate(qe, {0, 0, 1})};
        const V3 E{std::round(g.u(-300, 300)), std::round(g.u(-300, 300)), std::round(g.u(-300, 300))};
        const V3 mapUp = normalize({g.u(-0.3, 0.3), g.u(0.7, 1.0), g.u(-0.3, 0.3)});
        Consts cc = cleanEye(fr, E, mapUp, g.u(-0.03, 0.03), g.u(-0.03, 0.03));
        setRow(cc.cb2[0], g.u(0.5, 2.0), 0, 0, 0);
        RingScene sc;
        sc.c = cc;
        for (int i = 0; i < 400; ++i) {
            RingVert r{};
            for (float& f : r.v0) f = static_cast<float>(g.u(-3, 3));
            const V3 pos = E + V3{g.u(-60, 60), g.u(-60, 60), g.u(-60, 60)};
            r.v1[0] = static_cast<float>(pos.x); r.v1[1] = static_cast<float>(pos.y); r.v1[2] = static_cast<float>(pos.z);
            for (float& f : r.v2) f = static_cast<float>(g.u(-1, 1));
            for (float& f : r.v3) f = static_cast<float>(g.u(0.2, 2.5));
            for (float& f : r.v4) f = static_cast<float>(g.u(0.1, 3));
            sc.verts.push_back(r);
        }
        const std::vector<float> out = runRing(rig, *w.ring, w.ilRing, sc);
        for (size_t i = 0; i < sc.verts.size(); ++i) {
            const RingVert& r = sc.verts[i];
            const float* o = &out[i * w.ring->outFloats];
            const V3 v1{r.v1[0], r.v1[1], r.v1[2]};
            const V3 Fe = normalize({cc.cb0[7][0], cc.cb0[7][1], cc.cb0[7][2]});
            const V3 Rx{cc.cb0[4][0], cc.cb0[4][1], cc.cb0[4][2]}, Ry{cc.cb0[5][0], cc.cb0[5][1], cc.cb0[5][2]};
            const V3 Re = normalize(Rx - Fe * dot(Rx, Fe)), Ue = normalize(Ry - Fe * dot(Ry, Fe));
            V3 F, R, U;
            const V3 ray = v1 - E;
            const V3 ringUp = refIconUp(ray, {0, 1, 0}, mapUp);
            refAxes(ray, ringUp, Fe, Re, F, R, U);
            const V3 r277{cc.cb1[277][0], cc.cb1[277][1], cc.cb1[277][2]}, r278{cc.cb1[278][0], cc.cb1[278][1], cc.cb1[278][2]};
            const double sx = dot(r277, Re) < 0 ? -1.0 : 1.0, sy = dot(r278, Ue) < 0 ? -1.0 : 1.0;
            const double bx = static_cast<double>(r.v0[2]) * r.v3[2], by = static_cast<double>(r.v0[3]) * r.v3[2];
            if (std::sqrt(dot(cross(ringUp, normalize(ray)), cross(ringUp, normalize(ray)))) <= 0.02) ++fallbacks;
            const V3 pos = v1 + R * (bx * sx * std::sqrt(dot(r277, r277))) + U * (by * sy * std::sqrt(dot(r278, r278)));
            double ref[20];
            ref[0] = r.v1[0]; ref[1] = r.v1[1]; ref[2] = r.v1[2]; ref[3] = r.v3[2];
            for (int k = 0; k < 3; ++k) ref[4 + k] = std::exp2(std::log2(static_cast<double>(r.v4[k])) * cc.cb2[0][0]);
            ref[7] = r.v4[3];
            ref[8] = pos.x; ref[9] = pos.y; ref[10] = pos.z;
            ref[11] = r.v2[0]; ref[12] = r.v2[1];
            ref[13] = r.v0[0]; ref[14] = r.v0[1]; ref[15] = r.v2[3];
            for (int k = 0; k < 4; ++k) ref[16 + k] = dp4(cc.cb0[4 + k], pos);
            for (int k = 0; k < 20; ++k) {
                worst = std::fmax(worst, std::fabs(o[k] - ref[k]));
                mag = std::fmax(mag, std::fabs(ref[k]));
            }
            ++n;
        }
    }
    char label[200];
    std::snprintf(label, sizeof label, "G4.ring: worst %.2e over %u instances, 20 outputs each (largest output %.1f, %u within the up's threshold)", worst, n, mag, fallbacks);
    std::printf("%s\n", label);
    check(w.ring->outFloats == 20 && worst < 2e-5 * std::fmax(1.0, mag), label);
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
            std::snprintf(label, sizeof label, "G5.%s.%s: worst %.2e over %zu vertices (largest output %.1f)", sign ? "negative scale" : "positive scale", enc ? "exponent" : "fixed-point", d.worst, sc.verts.size(), d.mag);
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
    const UINT W = 8, BIG = 1024;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = W;
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> layer, mask, bigLayer;
    hr(rig.dev->CreateTexture2D(&td, nullptr, &layer), "layer");
    td.Width = td.Height = BIG;
    hr(rig.dev->CreateTexture2D(&td, nullptr, &bigLayer), "big layer");
    td.Width = td.Height = W;
    td.Format = DXGI_FORMAT_A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr(rig.dev->CreateTexture2D(&td, nullptr, &mask), "mask");
    ComPtr<ID3D11RenderTargetView> layerRtv, bigRtv, maskRtv;
    ComPtr<ID3D11ShaderResourceView> maskSrv;
    hr(rig.dev->CreateRenderTargetView(layer.Get(), nullptr, &layerRtv), "layer rtv");
    hr(rig.dev->CreateRenderTargetView(bigLayer.Get(), nullptr, &bigRtv), "big layer rtv");
    hr(rig.dev->CreateRenderTargetView(mask.Get(), nullptr, &maskRtv), "mask rtv");
    hr(rig.dev->CreateShaderResourceView(mask.Get(), nullptr, &maskSrv), "mask srv");
    const float clearLayer[4] = {0.2f, 0.4f, 0.6f, 1.0f}, clearMask[4] = {0, 0, 0, 0};
    rig.ctx->ClearRenderTargetView(layerRtv.Get(), clearLayer);
    rig.ctx->ClearRenderTargetView(bigRtv.Get(), clearLayer);
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
        std::snprintf(label, sizeof label, "G6.mask: the accumulating blend leaves 1 - 0.1 * 0.1 = 0.99 (252.5) in the pixel drawn twice and nothing around it: %d, %d, %d", a33, a00, a44);
        std::printf("%s\n", label);
        check(a33 >= 252 && a33 <= 253 && a00 == 0 && a44 == 0, label);
    }
    // the probe: one record, its tip at a pixel of a layer of `size` pixels, the mask at t0
    Consts c{};
    setRow(c.cb0[4], 1, 0, 0, 0); setRow(c.cb0[5], 0, 1, 0, 0); setRow(c.cb0[6], 0, 0, 1, 0); setRow(c.cb0[7], 0, 0, 0, 1);
    setRow(c.cb1[275], 0.5, 0.25, 0, 0);
    ComPtr<ID3D11Buffer> cb0 = makeBuf(rig, c.cb0, 8 * 16, D3D11_BIND_CONSTANT_BUFFER), cb1 = makeBuf(rig, c.cb1, 276 * 16, D3D11_BIND_CONSTANT_BUFFER);
    auto probe = [&](ID3D11RenderTargetView* target, UINT size, ID3D11ShaderResourceView* maskView, float scale, int px, int py, bool& done) -> UINT64 {
        Rec rec{};
        std::memcpy(&rec.a[1], &scale, 4);
        const float ndcX = ((px + 0.5f) / size) * 2.0f - 1.0f, ndcY2 = 1.0f - ((py + 0.5f) / size) * 2.0f;
        rec.p[0] = ndcX + 0.5f; rec.p[1] = ndcY2 + 0.25f; rec.p[2] = 0.5f; rec.p[3] = 1;
        ComPtr<ID3D11Buffer> t33 = makeBuf(rig, &rec, sizeof rec, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof rec);
        ComPtr<ID3D11ShaderResourceView> s33 = makeSrv(rig, t33.Get(), 1);
        const uint32_t vtx[2] = {0, 0};
        ComPtr<ID3D11Buffer> vb = makeBuf(rig, vtx, sizeof vtx, D3D11_BIND_VERTEX_BUFFER);
        D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
        ComPtr<ID3D11Query> q;
        hr(rig.dev->CreateQuery(&qd, &q), "query");
        ID3D11RenderTargetView* rt[1] = {target};
        rig.ctx->OMSetRenderTargets(1, rt, nullptr);
        const D3D11_VIEWPORT vp{0, 0, (float)size, (float)size, 0, 1};
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
        ID3D11ShaderResourceView* m[1] = {maskView};
        rig.ctx->PSSetShaderResources(0, 1, m);
        rig.ctx->Begin(q.Get());
        rig.ctx->Draw(1, 0);
        rig.ctx->End(q.Get());
        UINT64 samples = 99;
        HRESULT h = S_FALSE;
        for (int spin = 0; spin < 100000 && h == S_FALSE; ++spin) h = rig.ctx->GetData(q.Get(), &samples, sizeof samples, 0);
        done = h == S_OK;
        ID3D11ShaderResourceView* none[1] = {nullptr};
        rig.ctx->PSSetShaderResources(0, 1, none);
        rig.ctx->VSSetShaderResources(33, 1, none);
        return samples;
    };
    // the old cases, on the 8-pixel mask (the reach is a fraction of a pixel there): records at the pixel (3, 3) with a negative scale, at (3, 3) with a positive scale, and at (5, 5)
    struct Case { const char* name; float s; int px, py; uint64_t expect; };
    const Case cases[3] = {{"the tip on the mask", -0.4f, 3, 3, 1}, {"a positive scale (not the arrow)", 0.4f, 3, 3, 0}, {"the tip off the mask", -0.4f, 5, 5, 0}};
    for (const Case& k : cases) {
        bool done = false;
        const UINT64 samples = probe(layerRtv.Get(), W, maskSrv.Get(), k.s, k.px, k.py, done);
        char label[160];
        std::snprintf(label, sizeof label, "G6.probe.%s: %llu sample(s)", k.name, static_cast<unsigned long long>(samples));
        std::printf("%s\n", label);
        check(done && samples == k.expect, label);
    }
    // the gaps: a 1024-pixel mask (reach 0.0235 * 1024 = 24.06 pixels, a step of 3.01) built on the CPU
    const double reach = 0.0235 * BIG;
    const int TX = 512, TY = 512;
    struct Axis { const char* name; double x, y; };
    const double r2 = std::sqrt(0.5);
    const Axis axes[4] = {{"horizontal", 1, 0}, {"vertical", 0, 1}, {"diagonal /", r2, r2}, {"diagonal \\", r2, -r2}};
    auto makeMask = [&](const std::vector<uint8_t>& px) {
        D3D11_TEXTURE2D_DESC md{};
        md.Width = md.Height = BIG;
        md.MipLevels = md.ArraySize = 1;
        md.SampleDesc.Count = 1;
        md.Usage = D3D11_USAGE_DEFAULT;
        md.Format = DXGI_FORMAT_A8_UNORM;
        md.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{px.data(), BIG, 0};
        ComPtr<ID3D11Texture2D> t;
        hr(rig.dev->CreateTexture2D(&md, &init, &t), "big mask");
        ComPtr<ID3D11ShaderResourceView> v;
        hr(rig.dev->CreateShaderResourceView(t.Get(), nullptr, &v), "big mask srv");
        return v;
    };
    // a band at distance [lo, lo + thick) from the tip along the axis, on both sides or on the positive side only
    auto band = [&](const Axis& a, double lo, double thick, bool both) {
        std::vector<uint8_t> px(static_cast<size_t>(BIG) * BIG, 0);
        for (int y = 0; y < static_cast<int>(BIG); ++y)
            for (int x = 0; x < static_cast<int>(BIG); ++x) {
                const double u = (x - TX) * a.x + (y - TY) * a.y;
                const double au = std::fabs(u);
                if (au >= lo && au < lo + thick && (both || u > 0)) px[static_cast<size_t>(y) * BIG + x] = 255;
            }
        return px;
    };
    for (const Axis& a : axes) {
        struct G { const char* what; double lo; bool both; uint64_t expect; };
        const G gs[] = {
            {"a gap of 2 * reach - 10 pixels between two strips", reach - 5.0, true, 1},
            {"a gap of 2 * reach + 10 pixels between two strips", reach + 5.0, true, 0},
            {"mask on one side only", reach - 5.0, false, 0},
        };
        for (const G& g : gs) {
            const ComPtr<ID3D11ShaderResourceView> mv = makeMask(band(a, g.lo, 6.0, g.both));
            bool done = false;
            const UINT64 samples = probe(bigRtv.Get(), BIG, mv.Get(), -0.4f, TX, TY, done);
            char label[200];
            std::snprintf(label, sizeof label, "G6.probe.%s, %s: %llu sample(s)", a.name, g.what, static_cast<unsigned long long>(samples));
            std::printf("%s\n", label);
            check(done && samples == g.expect, label);
        }
    }
    {
        // the tip on the mask: one pixel; and a tip 5 pixels outside the edge of a large panel, which stays off it (a panel on one side only)
        std::vector<uint8_t> one(static_cast<size_t>(BIG) * BIG, 0), half(static_cast<size_t>(BIG) * BIG, 0), edge(static_cast<size_t>(BIG) * BIG, 0);
        one[static_cast<size_t>(TY) * BIG + TX] = 255;
        for (int y = 0; y < static_cast<int>(BIG); ++y)
            for (int x = TX + 5; x < static_cast<int>(BIG); ++x) half[static_cast<size_t>(y) * BIG + x] = 255;
        // a strip right of the tip, the tip 5 pixels from the left edge of the texture: the left side reads outside the texture, which is 0 on D3D11
        for (int y = 0; y < static_cast<int>(BIG); ++y)
            for (int x = 5 + 19; x < 5 + 25; ++x) edge[static_cast<size_t>(y) * BIG + x] = 255;
        struct S { const char* what; const std::vector<uint8_t>* px; int tx, ty; float s; uint64_t expect; };
        const S ss[] = {
            {"the tip on the mask", &one, TX, TY, -0.4f, 1},
            {"a tip 5 pixels outside a large panel's edge", &half, TX, TY, -0.4f, 0},
            {"a strip on one side, the other side outside the texture", &edge, 5, TY, -0.4f, 0},
            {"a positive scale, the tip on the mask", &one, TX, TY, 0.4f, 0},
        };
        for (const S& s : ss) {
            const ComPtr<ID3D11ShaderResourceView> mv = makeMask(*s.px);
            bool done = false;
            const UINT64 samples = probe(bigRtv.Get(), BIG, mv.Get(), s.s, s.tx, s.ty, done);
            char label[200];
            std::snprintf(label, sizeof label, "G6.probe.%s: %llu sample(s)", s.what, static_cast<unsigned long long>(samples));
            std::printf("%s\n", label);
            check(done && samples == s.expect, label);
        }
    }
    // the layers are untouched: write mask 0
    for (int which = 0; which < 2; ++which) {
        const UINT size = which ? BIG : W;
        td.Width = td.Height = size;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;
        hr(rig.dev->CreateTexture2D(&td, nullptr, &stage), "layer staging");
        rig.ctx->CopyResource(stage.Get(), which ? bigLayer.Get() : layer.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(rig.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "map layer");
        bool same = true;
        for (UINT y = 0; y < size; ++y)
            for (UINT x = 0; x < size; ++x) {
                const uint8_t* px = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4;
                same = same && px[0] == 51 && px[1] == 102 && px[2] == 153 && px[3] == 255;
            }
        rig.ctx->Unmap(stage.Get(), 0);
        check(same, which ? "G6.layer: the probe's write mask 0 leaves every pixel of the 1024-pixel layer as it was" : "G6.layer: the probe's write mask 0 leaves every pixel of the layer as it was");
    }
}


// ---- G7: the icons point down at the grid -------------------------------------------------------------------------------------
// The NDC of a point (relative to the eye) under the icons' projection, b1 [270..273].
inline void projectNdc(const Consts& c, V3 p, double out[2]) {
    double clip[4];
    for (int r = 0; r < 4; ++r) clip[r] = p.x * c.cb1[270][r] + p.y * c.cb1[271][r] + p.z * c.cb1[272][r] + c.cb1[273][r];
    out[0] = clip[0] / clip[3];
    out[1] = clip[1] / clip[3];
}
// The angle between two image directions, in radians.
inline double imageAngle(const double a[2], const double b[2]) {
    const double c = (a[0] * b[0] + a[1] * b[1]) / (std::sqrt(a[0] * a[0] + a[1] * a[1]) * std::sqrt(b[0] * b[0] + b[1] * b[1]));
    return std::acos(std::fmin(1.0, std::fmax(-1.0, c)));
}
inline void runIconUpTests(Rig& rig, const WorldProgs& w) {
    const V3 mapUp = normalize({0, 0.70710678, 0.70710678});   // the map camera's up
    const V3 grid{0, 1, 0};
    // (a) away from the map camera's forward: the quad's up edge (the corners (0, 1, 0) and (0, -1, 0) of the record) runs along the grid vertical's image through its centre,
    // which is a stem's direction; the label's up edge, on the camera's up, does not. The eye looks off to one side (a general eye, the record well off its axis).
    const Frame eye = frameAlong({0.35, -0.4, 0.85}, {0.04, 1.0, 0.05});
    const Consts cc = cleanEye(eye, kCam, mapUp, 0.02, 0.02);
    for (const V3 center : {V3{-9, -20, 12}, V3{14, -28, 20}, V3{-20, -12, 25}}) {
        const double cosV = std::fabs(center.y) / std::sqrt(dot(center, center));   // |cos| of the ray with the vertical: all three are under cos 35 degrees
        const std::vector<Corner> edge = {{{0, 1, 0}, 0, 0}, {{0, -1, 0}, 0, 0}};
        const Scene sc = iconScene(cc, mapCameraQ(), center, 1.0f, edge);
        const std::vector<float> icon = run(rig, *w.icon, w.ilIcon, sc), label = run(rig, *w.label, w.ilLabel, sc);
        auto edgeDir = [&](const std::vector<float>& out, const Prog& prog, double d[2]) {
            const float* a = &out[0 * prog.outFloats + prog.posAt];
            const float* b = &out[1 * prog.outFloats + prog.posAt];
            d[0] = a[0] / a[3] - b[0] / b[3];
            d[1] = a[1] / a[3] - b[1] / b[3];
        };
        double di[2], dl[2], ds[2], p0[2], p1[2];
        edgeDir(icon, *w.icon, di);
        edgeDir(label, *w.label, dl);
        const V3 cen{sc.recs[0].p[0] - kCam.x, sc.recs[0].p[1] - kCam.y, sc.recs[0].p[2] - kCam.z};
        projectNdc(cc, cen, p0);
        projectNdc(cc, cen + grid * 0.5, p1);
        ds[0] = p1[0] - p0[0];
        ds[1] = p1[1] - p0[1];
        const double aIcon = imageAngle(di, ds), aLabel = imageAngle(dl, ds);
        char text[240];
        std::snprintf(text, sizeof text, "G7.stem: ray (%g, %g, %g), |cos| with the vertical %.2f: the icon's up edge is %.2e rad from the grid vertical's image, the label's %.3f rad", center.x, center.y, center.z, cosV, aIcon, aLabel);
        std::printf("%s\n", text);
        check(cosV < 0.82 && aIcon < 1e-3 && aLabel > 0.02, text);
    }
    // (b) a ray within 16 degrees of the vertical (|cos| >= 0.96) takes the camera's up: the icon's corners are the label's; in between they hand over without a jump
    for (const double deg : {0.0, 5.0, 10.0, 15.9}) {
        const double r = deg * 3.14159265358979 / 180.0;
        const V3 center{30.0 * std::sin(r) * 0.6, -30.0 * std::cos(r), 30.0 * std::sin(r) * 0.8};
        const std::vector<Corner> quad = {{{1, 1, 0}, 0, 0}, {{1, -1, 0}, 0, 0}, {{-1, 1, 0}, 0, 0}, {{-1, -1, 0}, 0, 0}};
        const Scene sc = iconScene(cc, mapCameraQ(), center, 1.5f, quad);
        const std::vector<float> icon = run(rig, *w.icon, w.ilIcon, sc), label = run(rig, *w.label, w.ilLabel, sc);
        double worst = 0;
        for (size_t i = 0; i < quad.size(); ++i)
            for (int k = 0; k < 4; ++k) worst = std::fmax(worst, std::fabs(icon[i * w.icon->outFloats + w.icon->posAt + k] - label[i * w.label->outFloats + w.label->posAt + k]));
        char text[200];
        std::snprintf(text, sizeof text, "G7.nadir: a ray %.1f degrees from the vertical uses the camera's up: icon corners equal the label's to %.2e, finite %d", deg, worst, allFinite(icon) ? 1 : 0);
        std::printf("%s\n", text);
        check(worst < 5e-3 && allFinite(icon), text);   // 5e-3: the smoothstep reaches 1 only at 16 degrees; 15.9 is a hair short of it
    }
    {
        // the hand-over is continuous: the corner moves little between rays a quarter of a degree apart through the zone
        double prev[4] = {0, 0, 0, 0}, jump = 0;
        bool first = true;
        for (double deg = 16.0; deg <= 36.0; deg += 0.25) {
            const double r = deg * 3.14159265358979 / 180.0;
            const V3 center{30.0 * std::sin(r) * 0.6, -30.0 * std::cos(r), 30.0 * std::sin(r) * 0.8};
            const Scene sc = iconScene(cc, mapCameraQ(), center, 1.5f, {{{1, 1, 0}, 0, 0}});
            const std::vector<float> o = run(rig, *w.icon, w.ilIcon, sc);
            const float* q = &o[w.icon->posAt];
            double cur[4];
            for (int k = 0; k < 4; ++k) cur[k] = q[k] / q[3];
            if (!first) jump = std::fmax(jump, std::fmax(std::fabs(cur[0] - prev[0]), std::fabs(cur[1] - prev[1])));
            for (int k = 0; k < 4; ++k) prev[k] = cur[k];
            first = false;
        }
        char text[200];
        std::snprintf(text, sizeof text, "G7.handover: from 16 to 36 degrees, the largest NDC step of a corner between rays 0.25 degrees apart is %.2e", jump);
        std::printf("%s\n", text);
        check(jump < 0.02, text);
    }
}

// ---- G8: the sprites' depth ---------------------------------------------------------------------------------------------------
// The real states (mapui::depthStateDesc, depthRasterDesc), the real pixel shaders (kMapDepthSpritePS) and a D16 depth at 16x16, through WARP with the debug layer. The quads are the
// game's clip positions: clip.z = 0.1 and clip.w the distance (a constant-distance quad), or a tilted plane for the same-plane case. A quad's colour issue tests the depth and writes
// none (a colour pixel shader of the test's); its depth issue is the sprite's real depth pixel shader, no render target, depth write on, the biased rasterizer.
inline void runDepthTests(Rig& rig) {
    const UINT N = 16;
    const char* vsText = R"HLSL(
cbuffer Q : register(b0) { float4 rect; float4 zw; float4 col; };
void main(uint id : SV_VERTEXID, out float2 uv : TEXCOORD0, out float4 pos : SV_POSITION)
{
    float2 c = float2(id & 1, id >> 1);
    float2 xy = lerp(rect.xy, rect.zw, c);
    float w = 1.0 / (zw.y + zw.z * xy.x + zw.w * xy.y);   // zw.y the constant 1/distance, zw.zw its tilt over the screen (a plane)
    pos = float4(xy * w, zw.x, w);                          // clip.z = zw.x (0.1), clip.w the distance
    uv = float2((xy.x + 1.0) * 0.5, 0.5);
}
)HLSL";
    const char* colourPs = R"HLSL(
cbuffer Q : register(b0) { float4 rect; float4 zw; float4 col; };
float4 main(float2 uv : TEXCOORD0) : SV_TARGET { return col; }
)HLSL";
    const std::vector<uint8_t> vsCode = compile(vsText, "vs_5_0"), colourCode = compile(colourPs, "ps_5_0");
    const std::vector<uint8_t> spriteCode = compile(edvr::kMapDepthSpritePS, "ps_5_0"), canvasCode = compile(edvr::kMapDepthCanvasPS, "ps_5_0");
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> colourPsO, spritePs, canvasPs;
    hr(rig.dev->CreateVertexShader(vsCode.data(), vsCode.size(), nullptr, &vs), "depth test vs");
    hr(rig.dev->CreatePixelShader(colourCode.data(), colourCode.size(), nullptr, &colourPsO), "depth test colour ps");
    hr(rig.dev->CreatePixelShader(spriteCode.data(), spriteCode.size(), nullptr, &spritePs), "sprite depth ps");
    hr(rig.dev->CreatePixelShader(canvasCode.data(), canvasCode.size(), nullptr, &canvasPs), "canvas depth ps");

    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = N;
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> colour, depth;
    hr(rig.dev->CreateTexture2D(&td, nullptr, &colour), "depth test colour");
    td.Format = DXGI_FORMAT_R16_TYPELESS;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr(rig.dev->CreateTexture2D(&td, nullptr, &depth), "depth test depth");
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    hr(rig.dev->CreateRenderTargetView(colour.Get(), nullptr, &rtv), "depth test rtv");
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
    dvd.Format = edvr::mapui::kDepthFormat;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    hr(rig.dev->CreateDepthStencilView(depth.Get(), &dvd, &dsv), "depth test dsv");

    ComPtr<ID3D11DepthStencilState> testOnly, testWrite;
    const D3D11_DEPTH_STENCIL_DESC d0 = edvr::mapui::depthStateDesc(false), d1 = edvr::mapui::depthStateDesc(true);
    hr(rig.dev->CreateDepthStencilState(&d0, &testOnly), "ds test");
    hr(rig.dev->CreateDepthStencilState(&d1, &testWrite), "ds write");
    D3D11_RASTERIZER_DESC game{};   // the game's: solid, no cull, depth clip
    game.FillMode = D3D11_FILL_SOLID;
    game.CullMode = D3D11_CULL_NONE;
    game.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rsGame, rsBias;
    const D3D11_RASTERIZER_DESC biased = edvr::mapui::depthRasterDesc(game);
    hr(rig.dev->CreateRasterizerState(&game, &rsGame), "rs game");
    hr(rig.dev->CreateRasterizerState(&biased, &rsBias), "rs bias");
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> samp;
    hr(rig.dev->CreateSamplerState(&sd, &samp), "sampler");
    const D3D11_BLEND_DESC noColour = edvr::mapui::probeBlendDesc();
    ComPtr<ID3D11BlendState> bsNone;
    D3D11_BLEND_DESC opaque{};
    opaque.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> bsOpaque;
    hr(rig.dev->CreateBlendState(&noColour, &bsNone), "bs none");
    hr(rig.dev->CreateBlendState(&opaque, &bsOpaque), "bs opaque");

    struct Quad { float x0, y0, x1, y1; float invD, tx, ty; float col[3]; uint8_t a0, a1; bool canvas; };
    auto alphaTex = [&](uint8_t a0, uint8_t a1) {
        const uint8_t px[8] = {255, 255, 255, a0, 255, 255, 255, a1};   // a 2x1 texture: the alpha of the left and right half
        D3D11_TEXTURE2D_DESC t{};
        t.Width = 2; t.Height = 1; t.MipLevels = t.ArraySize = 1; t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT; t.Format = DXGI_FORMAT_R8G8B8A8_UNORM; t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{px, 8, 0};
        ComPtr<ID3D11Texture2D> tex;
        hr(rig.dev->CreateTexture2D(&t, &init, &tex), "alpha tex");
        ComPtr<ID3D11ShaderResourceView> srv;
        hr(rig.dev->CreateShaderResourceView(tex.Get(), nullptr, &srv), "alpha srv");
        return srv;
    };
    float cb2Data[8] = {1, 1, 1, 1, 1, 1, 1, 1};   // cb2[1].w = 1: the sprite's own alpha multiplier
    ComPtr<ID3D11Buffer> cb2 = makeBuf(rig, cb2Data, sizeof cb2Data, D3D11_BIND_CONSTANT_BUFFER);
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(N), static_cast<float>(N), 0, 1};
    // one quad: the colour issue (test, no write), then the depth issue (the sprite's depth shader, write, biased); `withBias` false uses the game's rasterizer for the depth issue
    auto drawQuad = [&](const Quad& q, bool depthIssue, bool withBias) {
        const float cbv[12] = {q.x0, q.y0, q.x1, q.y1, 0.1f, q.invD, q.tx, q.ty, q.col[0], q.col[1], q.col[2], 1.0f};
        ComPtr<ID3D11Buffer> cb0 = makeBuf(rig, cbv, sizeof cbv, D3D11_BIND_CONSTANT_BUFFER);
        ComPtr<ID3D11ShaderResourceView> tex = alphaTex(q.a0, q.a1);
        rig.ctx->IASetInputLayout(nullptr);
        rig.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        rig.ctx->RSSetViewports(1, &vp);
        rig.ctx->VSSetShader(vs.Get(), nullptr, 0);
        rig.ctx->VSSetConstantBuffers(0, 1, cb0.GetAddressOf());
        ID3D11Buffer* pcb[3] = {cb0.Get(), nullptr, cb2.Get()};
        rig.ctx->PSSetConstantBuffers(0, 3, pcb);
        rig.ctx->PSSetShaderResources(0, 1, tex.GetAddressOf());
        rig.ctx->PSSetSamplers(0, 1, samp.GetAddressOf());
        if (!depthIssue) {
            ID3D11RenderTargetView* rt[1] = {rtv.Get()};
            rig.ctx->OMSetRenderTargets(1, rt, dsv.Get());
            rig.ctx->OMSetDepthStencilState(testOnly.Get(), 0);
            rig.ctx->RSSetState(rsGame.Get());
            rig.ctx->OMSetBlendState(bsOpaque.Get(), nullptr, 0xFFFFFFFFu);
            rig.ctx->PSSetShader(colourPsO.Get(), nullptr, 0);
        } else {
            rig.ctx->OMSetRenderTargets(0, nullptr, dsv.Get());
            rig.ctx->OMSetDepthStencilState(testWrite.Get(), 0);
            rig.ctx->RSSetState(withBias ? rsBias.Get() : rsGame.Get());
            rig.ctx->OMSetBlendState(bsNone.Get(), nullptr, 0xFFFFFFFFu);
            rig.ctx->PSSetShader(q.canvas ? canvasPs.Get() : spritePs.Get(), nullptr, 0);
        }
        rig.ctx->Draw(4, 0);
        ID3D11ShaderResourceView* none[1] = {nullptr};
        rig.ctx->PSSetShaderResources(0, 1, none);
        rig.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    };
    auto clearAll = [&] {
        const float zero[4] = {0, 0, 0, 0};
        rig.ctx->ClearRenderTargetView(rtv.Get(), zero);
        rig.ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
    };
    auto readColour = [&](std::vector<uint8_t>& out) {
        D3D11_TEXTURE2D_DESC st{};
        colour->GetDesc(&st);
        st.Usage = D3D11_USAGE_STAGING; st.BindFlags = 0; st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;
        hr(rig.dev->CreateTexture2D(&st, nullptr, &stage), "colour staging");
        rig.ctx->CopyResource(stage.Get(), colour.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(rig.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "map colour");
        out.assign(N * N * 4, 0);
        for (UINT y = 0; y < N; ++y) std::memcpy(&out[y * N * 4], static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, N * 4);
        rig.ctx->Unmap(stage.Get(), 0);
    };
    auto readDepth = [&](std::vector<uint16_t>& out) {
        D3D11_TEXTURE2D_DESC st{};
        depth->GetDesc(&st);
        st.Usage = D3D11_USAGE_STAGING; st.BindFlags = 0; st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;
        hr(rig.dev->CreateTexture2D(&st, nullptr, &stage), "depth staging");
        rig.ctx->CopyResource(stage.Get(), depth.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(rig.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "map depth");
        out.assign(N * N, 0);
        for (UINT y = 0; y < N; ++y) std::memcpy(&out[y * N], static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, N * 2);
        rig.ctx->Unmap(stage.Get(), 0);
    };
    // colour ids: the pixel's red channel is the quad's id
    const float NEAR_ID[3] = {1.0f, 0, 0}, FAR_ID[3] = {0, 1.0f, 0}, A_ID[3] = {1.0f, 0, 0}, B_ID[3] = {0, 0, 1.0f};
    auto id = [&](const std::vector<uint8_t>& c, UINT x, UINT y) -> char {
        const uint8_t* p = &c[(y * N + x) * 4];
        if (p[0] > 200 && p[1] < 50 && p[2] < 50) return 'N';   // red
        if (p[1] > 200 && p[0] < 50 && p[2] < 50) return 'F';   // green
        if (p[2] > 200 && p[0] < 50 && p[1] < 50) return 'B';   // blue
        return p[3] == 0 ? '.' : '?';
    };
    auto pair = [&](const Quad& first, const Quad& second, std::vector<uint8_t>& c) {
        clearAll();
        drawQuad(first, false, true);
        drawQuad(first, true, true);
        drawQuad(second, false, true);
        drawQuad(second, true, true);
        readColour(c);
    };
    // distances 5 and 10: invD 0.2 and 0.1
    Quad nearQ{-1, -1, 1, 1, 0.2f, 0, 0, {NEAR_ID[0], NEAR_ID[1], NEAR_ID[2]}, 255, 255, false};
    Quad farQ{-1, -1, 1, 1, 0.1f, 0, 0, {FAR_ID[0], FAR_ID[1], FAR_ID[2]}, 255, 255, false};
    std::vector<uint8_t> c;
    auto count = [&](char ch, UINT x0, UINT x1) { unsigned n = 0; for (UINT y = 0; y < N; ++y) for (UINT x = x0; x < x1; ++x) n += id(c, x, y) == ch; return n; };
    char text[240];

    // reversed depth: a nearer clip position (clip.w 5 against 10, clip.z 0.1) has the larger depth
    {
        std::vector<uint16_t> d5, d10;
        clearAll(); drawQuad(nearQ, true, false); readDepth(d5);
        clearAll(); drawQuad(farQ, true, false); readDepth(d10);
        std::snprintf(text, sizeof text, "G8.reversed: clip.z 0.1 over clip.w 5 stores %u and over 10 stores %u of 65535 (0.1 / w: %.1f and %.1f): the nearer is the larger", d5[N * 8 + 8], d10[N * 8 + 8], 0.02 * 65535.0, 0.01 * 65535.0);
        std::printf("%s\n", text);
        check(d5[N * 8 + 8] > d10[N * 8 + 8] && std::abs(static_cast<int>(d5[N * 8 + 8]) - static_cast<int>(std::lround(0.02 * 65535.0))) <= 1 &&
                  std::abs(static_cast<int>(d10[N * 8 + 8]) - static_cast<int>(std::lround(0.01 * 65535.0))) <= 1, text);
        // the bias: the same quad written with the biased rasterizer is kDepthBias steps farther
        std::vector<uint16_t> db;
        clearAll(); drawQuad(nearQ, true, true); readDepth(db);
        const int steps = static_cast<int>(d5[N * 8 + 8]) - static_cast<int>(db[N * 8 + 8]);
        std::snprintf(text, sizeof text, "G8.bias: the biased rasterizer writes %d steps farther than the game's (kDepthBias %d)", steps, edvr::mapui::kDepthBias);
        std::printf("%s\n", text);
        check(steps == -edvr::mapui::kDepthBias, text);
    }
    // near first, then far: the far quad is rejected everywhere the near one is solid
    pair(nearQ, farQ, c);
    std::snprintf(text, sizeof text, "G8.near-then-far: near %u far %u of 256", count('N', 0, N), count('F', 0, N));
    std::printf("%s\n", text);
    check(count('N', 0, N) == N * N && count('F', 0, N) == 0, text);
    // far first, then near: near over far
    pair(farQ, nearQ, c);
    std::snprintf(text, sizeof text, "G8.far-then-near: near %u far %u of 256", count('N', 0, N), count('F', 0, N));
    std::printf("%s\n", text);
    check(count('N', 0, N) == N * N && count('F', 0, N) == 0, text);
    // the near quad half see-through (alpha 0.8 left, 0.1 right): the far quad shows on the right half only
    {
        Quad nearHalf = nearQ; nearHalf.a0 = 204; nearHalf.a1 = 26;
        pair(nearHalf, farQ, c);
        std::snprintf(text, sizeof text, "G8.alpha-split: left half near %u far %u, right half near %u far %u (of 128 each)", count('N', 0, N / 2), count('F', 0, N / 2), count('N', N / 2, N), count('F', N / 2, N));
        std::printf("%s\n", text);
        check(count('N', 0, N / 2) == N * N / 2 && count('F', 0, N / 2) == 0 && count('F', N / 2, N) == N * N / 2, text);
        // at and under the threshold: 77/255 = 0.302 is over kMapDepthAlpha (0.3), 76/255 = 0.298 under it
        Quad nearEdge = nearQ; nearEdge.a0 = 77; nearEdge.a1 = 76;
        pair(nearEdge, farQ, c);
        std::snprintf(text, sizeof text, "G8.threshold: alpha 77/255 blocks the far quad (far %u of 128 on the left), 76/255 does not (far %u of 128 on the right)", count('F', 0, N / 2), count('F', N / 2, N));
        std::printf("%s\n", text);
        check(count('F', 0, N / 2) == 0 && count('F', N / 2, N) == N * N / 2, text);
        // the canvas's shader (alpha is t0.a alone) does the same
        Quad nearCanvas = nearHalf; nearCanvas.canvas = true;
        pair(nearCanvas, farQ, c);
        std::snprintf(text, sizeof text, "G8.canvas: the canvas shader blocks on the left (far %u of 128) and shows on the right (far %u of 128)", count('F', 0, N / 2), count('F', N / 2, N));
        std::printf("%s\n", text);
        check(count('F', 0, N / 2) == 0 && count('F', N / 2, N) == N * N / 2, text);
    }
    // two quads in the same plane, the second larger: it draws whole, no hole where the first was (the plane is tilted, so each quad interpolates its own depths)
    {
        Quad a{-0.5f, -0.5f, 0.5f, 0.5f, 0.12f, 0.03f, 0.02f, {A_ID[0], A_ID[1], A_ID[2]}, 255, 255, false};
        Quad b{-1.0f, -1.0f, 1.0f, 1.0f, 0.12f, 0.03f, 0.02f, {B_ID[0], B_ID[1], B_ID[2]}, 255, 255, false};
        pair(a, b, c);
        const unsigned nb = count('B', 0, N), na = count('N', 0, N);
        std::snprintf(text, sizeof text, "G8.same-plane: the larger second quad covers %u of 256 pixels, the first quad %u (no hole at D16)", nb, na);
        std::printf("%s\n", text);
        check(nb == N * N && na == 0, text);
        // and the other order: the smaller second quad draws over the first in its own rectangle
        pair(b, a, c);
        const unsigned inner = [&] { unsigned n = 0; for (UINT y = 4; y < 12; ++y) for (UINT x = 4; x < 12; ++x) n += id(c, x, y) == 'N'; return n; }();
        std::snprintf(text, sizeof text, "G8.same-plane-small: the smaller second quad covers %u of its 64 pixels", inner);
        std::printf("%s\n", text);
        check(inner == 64, text);
    }
}

inline int runAll() {
    Rig rig;
    std::printf("galaxy_map_vs_test: debug layer %s\n", rig.debugLayer ? "ON" : "not installed (messages not checked)");
    const std::vector<uint8_t> icon = compile(edvr::kMapIconFacingVS, "vs_5_0"), iconUv = compile(edvr::kMapIconUvFacingVS, "vs_5_0"), arrow = compile(edvr::kMapArrowDepthVS, "vs_5_0");
    check(!edvr::mapui::isPanelVs(edvr::mapui::kVsHoverHalo) && edvr::mapui::isPanelVs(edvr::mapui::kVsPanelA) && edvr::mapui::isPanelVs(edvr::mapui::kVsPanelC) &&
              edvr::mapui::panelIndex(edvr::mapui::kVsHoverHalo) == -1 && edvr::mapui::panelIndex(edvr::mapui::kVsPanelC) == 1,
          "G9: B10B032BDFD46700 (the hover halo) is not in the mask set; C4B4B3 and 5DAD41 are");
    const std::vector<uint8_t> probeVs = compile(edvr::kMapProbeVS, "vs_5_0"), probePs = compile(edvr::kMapProbePS, "ps_5_0");
    const std::vector<uint8_t> ring = compile(edvr::kMapRingVS, "vs_5_0");
    const Prog pIcon = makeProg(rig, icon), pIconUv = makeProg(rig, iconUv), pArrow = makeProg(rig, arrow);
    const Prog pRing = makeProg(rig, ring);
    ComPtr<ID3D11InputLayout> il = makeLayout(rig, icon), ilUv = makeLayout(rig, iconUv), ilArrow = makeLayout(rig, arrow);
    ComPtr<ID3D11InputLayout> ilRing = makeRingLayout(rig, ring);
    check(pIcon.uvAt == 0 && pIcon.posAt == 2 && pIconUv.uvAt == 0 && pIconUv.posAt == 2 && pArrow.uvAt == 0 && pArrow.posAt == 2,
          "G0: the outputs are TEXCOORD0.xy at o0 and SV_POSITION at o1, as the game's");
    runIconTests(rig, pIcon, pIconUv, il.Get());
    const WorldProgs wp{&pIcon, &pIconUv, &pRing, il.Get(), ilUv.Get(), ilRing.Get()};
    check(pRing.posAt == 16 && pRing.outFloats == 20, "G0: the ring's outputs are the game's six (TEXCOORD0, 1, 2, 4, 5 and SV_POSITION) in 20 floats");
    runWorldStability(rig, wp);
    runSquareOn(rig, wp);
    runRingTests(rig, wp);
    runIconUpTests(rig, wp);
    runArrowTests(rig, pArrow, ilArrow.Get());
    runProbeTest(rig, probeVs, probePs);
    runDepthTests(rig);
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
        std::puts("galaxy_map_vs_test: G0-G9, WARP with the debug layer; reads nothing, writes nothing");
        return 0;
    }
    if (mode != "--self-test") return 2;
    return gmt::runAll();
}
#endif
