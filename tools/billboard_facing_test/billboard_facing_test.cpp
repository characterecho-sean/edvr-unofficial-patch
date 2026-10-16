// The explosion billboards' facing transcriptions (src\d3d11\billboard_facing_vs.h) against the game's own bytecode, on a
// WARP device with the D3D debug layer. For each of the three shaders the ORIGINAL DXBC (tools\billboard_facing_test\fixtures,
// held to its hash name) and the transcription run on the same vertices through stream output, and every output register is
// compared. Nothing is granted on a likeness:
//   F1 identity: the fixture's content hashes to its name; the signature blocks (input and output, every column including
//      Used) and every dcl_ line of the transcription equal the original's; a transcription compared with another shader's
//      original does NOT match (the comparator can tell the shaders apart)
//   F2 viewer feed absent (pCam.w = 0): every output of every vertex equals the original's, on every alignment mode and
//      random constants -- the game's own look
//   F3 feed present, the aligned modes (1, 2, 3, 5, 6, 7): identical to the original, position included (they never used the
//      camera basis)
//   F4 feed present, viewer on the particle's axis with zero roll: equal to the original in every mode, modes 0 and 4 too
//   F5 feed present, off axis: only the basis-dependent outputs differ; the quad faces the viewer (offsets perpendicular to the
//      face axis, the original's are not: the fail-before), with the original's size, and level (no roll) for a rolled camera
//   F6 a particle directly above the viewer (the degenerate cross) stays finite
//   F8 b2: a transcription whose original has no b2 never reads one (garbage bound there changes nothing); where b2 is read it does
//   F7 source pins from the repo root
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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "billboard_facing_vs.h"

using Microsoft::WRL::ComPtr;

static int failures = 0;
static unsigned checks = 0;
static void check(bool ok, const std::string& label) {
    ++checks;
    if (!ok) { std::printf("FAIL %s\n", label.c_str()); ++failures; }
}
static void hr(HRESULT h, const char* what) {
    if (FAILED(h)) { std::printf("FAIL D3D %s 0x%08lx\n", what, static_cast<unsigned long>(h)); std::exit(1); }
}

// ---- files -------------------------------------------------------------------------------------------------------
static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
// The project's shader content hash (device_hook.cpp: fnv1a64 over the bytecode; log.cpp's offset basis is the one with
// the last digit dropped, kept as is so this equals the name the dump and the runtime use).
static uint64_t fnv1a64(const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// ---- disassembly, signatures, declarations ----------------------------------------------------------------------
static std::string disassemble(const void* code, size_t size) {
    ComPtr<ID3DBlob> text;
    hr(D3DDisassemble(code, size, 0, nullptr, &text), "D3DDisassemble");
    return std::string(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize());
}
static std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        out.push_back(line);
    }
    return out;
}
static std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> t;
    std::istringstream in(s);
    std::string w;
    while (in >> w) t.push_back(w);
    return t;
}
struct SigRow { std::string name; int index = 0; std::string mask; int reg = 0; std::string sys, format, used; };
struct Sig {
    std::vector<SigRow> in, out;
    std::vector<std::string> inText, outText;   // normalised rows: the Used column included
    std::vector<std::string> dcls;              // every dcl_ line but the temps count
};
static Sig parse(const std::string& asmText) {
    Sig s;
    int section = 0;
    for (const std::string& line : splitLines(asmText)) {
        if (line.rfind("// Input signature:", 0) == 0) { section = 1; continue; }
        if (line.rfind("// Output signature:", 0) == 0) { section = 2; continue; }
        if (line.rfind("vs_5_0", 0) == 0) section = 0;
        if (line.rfind("dcl_", 0) == 0 && line.rfind("dcl_temps", 0) != 0) s.dcls.push_back(line);
        if (!section || line.rfind("// ", 0) != 0) continue;
        const auto t = tokens(line.substr(3));
        // Name Index Mask Register SysValue Format [Used]; the Used column is blank for an unread element.
        if (t.size() < 6 || t[0] == "Name" || t[0][0] == '-') continue;
        SigRow r;
        r.name = t[0]; r.index = std::atoi(t[1].c_str()); r.mask = t[2]; r.reg = std::atoi(t[3].c_str());
        r.sys = t[4]; r.format = t[5]; r.used = t.size() > 6 ? t[6] : "";
        std::string norm;
        for (const auto& w : t) norm += w + " ";
        (section == 1 ? s.in : s.out).push_back(r);
        (section == 1 ? s.inText : s.outText).push_back(norm);
    }
    return s;
}

// ---- compile the transcription ---------------------------------------------------------------------------------
static std::vector<uint8_t> compile(const char* source) {
    ComPtr<ID3DBlob> code, err;
    const HRESULT h = D3DCompile(source, std::strlen(source), "billboard_facing", nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, &err);
    if (FAILED(h)) { std::printf("shader: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); std::exit(1); }
    const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
    return std::vector<uint8_t>(p, p + code->GetBufferSize());
}

// ---- the device and one stream-output run -----------------------------------------------------------------------
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

static int comps(const std::string& mask) { return static_cast<int>(mask.size()); }
static DXGI_FORMAT formatFor(int n) {
    return n == 1 ? DXGI_FORMAT_R32_FLOAT : n == 2 ? DXGI_FORMAT_R32G32_FLOAT : n == 3 ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
}

// One shader under test: its bytecode, its pass-through stream-output object, and the layout of what it streams.
struct Program {
    std::vector<uint8_t> code;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11GeometryShader> so;
    Sig sig;
    std::vector<int> outOffset;   // float offset of each output element in a streamed vertex
    int outFloats = 0;
};
static Program makeProgram(Rig& rig, const std::vector<uint8_t>& code) {
    Program p;
    p.code = code;
    p.sig = parse(disassemble(code.data(), code.size()));
    hr(rig.dev->CreateVertexShader(code.data(), code.size(), nullptr, &p.vs), "CreateVertexShader");
    std::vector<D3D11_SO_DECLARATION_ENTRY> decl;
    for (const SigRow& r : p.sig.out) {
        D3D11_SO_DECLARATION_ENTRY e{};
        e.SemanticName = r.name == "SV_POSITION" ? "SV_Position" : r.name.c_str();
        e.SemanticIndex = static_cast<UINT>(r.index);
        e.ComponentCount = static_cast<BYTE>(comps(r.mask));
        decl.push_back(e);
        p.outOffset.push_back(p.outFloats);
        p.outFloats += comps(r.mask);
    }
    UINT stride = static_cast<UINT>(p.outFloats * 4);
    hr(rig.dev->CreateGeometryShaderWithStreamOutput(code.data(), code.size(), decl.data(), static_cast<UINT>(decl.size()), &stride, 1,
                                                     D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &p.so),
       "CreateGeometryShaderWithStreamOutput");
    return p;
}

// Vertex data: one float array per input element, in the original's register order.
struct Vertices {
    std::vector<std::vector<float>> perVertex;   // [vertex][all inputs concatenated]
};
struct Constants {
    float cb0[12][4] = {};
    float cb1[280][4] = {};
    float cb2[3][4] = {};
    float cbp[2][4] = {};
};

static std::vector<float> run(Rig& rig, const Program& prog, ID3D11InputLayout* layout, UINT inStride, const Vertices& v, const Constants& c) {
    const UINT n = static_cast<UINT>(v.perVertex.size());
    std::vector<float> flat;
    for (const auto& pv : v.perVertex) flat.insert(flat.end(), pv.begin(), pv.end());
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>(flat.size() * 4);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{flat.data(), 0, 0};
    ComPtr<ID3D11Buffer> vb;
    hr(rig.dev->CreateBuffer(&bd, &sd, &vb), "vb");
    auto makeCb = [&](const void* data, UINT bytes) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = bytes;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA s{data, 0, 0};
        ComPtr<ID3D11Buffer> b;
        hr(rig.dev->CreateBuffer(&d, &s, &b), "cb");
        return b;
    };
    ComPtr<ID3D11Buffer> cbs[4] = {makeCb(c.cb0, sizeof c.cb0), makeCb(c.cb1, sizeof c.cb1), makeCb(c.cb2, sizeof c.cb2), makeCb(c.cbp, sizeof c.cbp)};
    ID3D11Buffer* raw[4] = {cbs[0].Get(), cbs[1].Get(), cbs[2].Get(), cbs[3].Get()};
    D3D11_BUFFER_DESC sod{};
    sod.ByteWidth = n * static_cast<UINT>(prog.outFloats) * 4;
    sod.Usage = D3D11_USAGE_DEFAULT;
    sod.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    ComPtr<ID3D11Buffer> sob;
    hr(rig.dev->CreateBuffer(&sod, nullptr, &sob), "so buffer");

    rig.ctx->IASetInputLayout(layout);
    ID3D11Buffer* vbs[1] = {vb.Get()};
    UINT stride = inStride, offset = 0;
    rig.ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    rig.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    rig.ctx->VSSetShader(prog.vs.Get(), nullptr, 0);
    rig.ctx->VSSetConstantBuffers(0, 4, raw);
    rig.ctx->GSSetShader(prog.so.Get(), nullptr, 0);
    rig.ctx->PSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer* sot[1] = {sob.Get()};
    UINT soOffset = 0;
    rig.ctx->SOSetTargets(1, sot, &soOffset);
    rig.ctx->Draw(n, 0);
    ID3D11Buffer* none[1] = {nullptr};
    rig.ctx->SOSetTargets(1, none, &soOffset);
    rig.ctx->GSSetShader(nullptr, nullptr, 0);

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

// ---- inputs -----------------------------------------------------------------------------------------------------
struct V3 { float x, y, z; };
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float len(V3 a) { return std::sqrt(dot(a, a)); }
static V3 norm(V3 a) { return a * (1.0f / len(a)); }

struct ParticleSpec {
    V3 pos{0, 0, 10};          // the vertex position (local), before cb0
    float spin = 0.0f;          // v0.w
    int mode = 0;               // alignment byte
    float hx = 1.0f, hy = 0.7f; // half extents
    V3 dir{0.7f, 0.3f, 0.4f};   // DIRECTION, 0..1
    V3 axis{0.2f, 0.9f, 0.5f};  // AXIS, 0..1
    uint32_t atlas = 0x00A00007u;   // the high half >= 0x80 keeps the float's exponent nonzero: no denormal flush at input assembly
    float brightness = 1.3f;
    float uv[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    float colour[4] = {0.9f, 0.8f, 0.7f, 0.6f};
    float texblend = 0.25f;
    float blend = 3.0f;
};

// Four corner vertices of one particle, in the original's input order.
static void addParticle(Vertices& out, const Sig& orig, const ParticleSpec& s) {
    static const float cu[4] = {0, 1, 0, 1}, cv[4] = {0, 0, 1, 1};
    static const float sx[4] = {-1, 1, -1, 1}, sy[4] = {-1, -1, 1, 1};
    for (int c = 0; c < 4; ++c) {
        std::vector<float> v;
        for (const SigRow& r : orig.in) {
            float e[4] = {0, 0, 0, 0};
            if (r.name == "POSITION") { e[0] = s.pos.x; e[1] = s.pos.y; e[2] = s.pos.z; e[3] = s.spin; }
            else if (r.name == "DIRECTION") { e[0] = s.dir.x; e[1] = s.dir.y; e[2] = s.dir.z; e[3] = 0.5f; }
            else if (r.name == "DIMENSIONS") { e[0] = sx[c] * s.hx; e[1] = sy[c] * s.hy; e[2] = 0.1f; }
            else if (r.name == "ALIGNBLENDBRIGHT") { e[0] = static_cast<float>(s.mode) / 255.0f; e[1] = s.blend / 255.0f; e[2] = 0.3f; e[3] = 0.4f; }
            else if (r.name == "BRIGHTNESS") e[0] = s.brightness;
            else if (r.name == "AXIS") { e[0] = s.axis.x; e[1] = s.axis.y; e[2] = s.axis.z; e[3] = 0.35f; }
            else if (r.name == "VERTEXALPHA") { e[0] = cu[c]; e[1] = cv[c]; }
            else if (r.name == "UVSCURRENTDIFFUSE") std::memcpy(e, s.uv, 16);
            else if (r.name == "UVSNEXTDIFFUSE") std::memcpy(e, s.uv + 4, 16);
            else if (r.name == "TEXBLENDDIFFUSE") e[0] = s.texblend;
            else if (r.name == "ATLASINDICESDIFFUSE") std::memcpy(&e[0], &s.atlas, 4);
            else if (r.name == "COLOUR") std::memcpy(e, s.colour, 16);
            else { std::printf("unknown input semantic %s\n", r.name.c_str()); std::exit(1); }
            for (int k = 0; k < comps(r.mask); ++k) v.push_back(e[k]);
        }
        out.perVertex.push_back(v);
    }
}

static void setRow(float (&row)[4], V3 v, float w) { row[0] = v.x; row[1] = v.y; row[2] = v.z; row[3] = w; }
// A camera: forward f, rolled about f, world up (0,1,0) as the roll-free reference.
static void setCamera(Constants& c, V3 f, float roll) {
    f = norm(f);
    const V3 right0 = norm(cross({0, 1, 0}, f));
    const V3 up0 = cross(f, right0);
    const V3 up = right0 * -std::sin(roll) + up0 * std::cos(roll);
    setRow(c.cb1[278], up, 0.0f);
    setRow(c.cb1[279], f, 0.0f);
}
// Identity camera-relative transform and clip rows, so SV_POSITION.xyz is the corner's position itself.
static void identityRows(Constants& c) {
    std::memset(&c, 0, sizeof c);
    setRow(c.cb0[9], {1, 0, 0}, 0);  setRow(c.cb0[10], {0, 1, 0}, 0);  setRow(c.cb0[11], {0, 0, 1}, 0);
    setRow(c.cb1[270], {1, 0, 0}, 0); setRow(c.cb1[271], {0, 1, 0}, 0); setRow(c.cb1[272], {0, 0, 1}, 0); setRow(c.cb1[273], {0, 0, 0}, 1);
    c.cb1[222][2] = 1.0f;
    setCamera(c, {0, 0, 1}, 0.0f);
    // The near fade: a fade distance of 2 over 0.3 of it, and the soft-depth term's scale; both well inside "visible".
    c.cb2[0][1] = 0.0f;  c.cb2[1][3] = 2.0f;  c.cb2[2][0] = 0.3f;
    setRow(c.cbp[1], {0, 1, 0}, 0);
}
static void randomConstants(Constants& c, std::mt19937& rng) {
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    identityRows(c);
    // A rotation-ish camera-relative transform with a translation.
    V3 a = norm({u(rng), u(rng), u(rng)}), b = norm(cross(a, {0.3f, 0.5f, 0.8f})), d = cross(a, b);
    setRow(c.cb0[9], a, u(rng) * 5);  setRow(c.cb0[10], b, u(rng) * 5);  setRow(c.cb0[11], d, u(rng) * 5);
    // Random clip rows, with a clip w that stays positive: BBAD multiplies its fade by 1/sqrt(clip w), and where w <= 0 (a
    // vertex behind the camera, which the rasteriser discards) a zero fade times infinity is a NaN or a 0 depending on
    // the operand order the compiler happened to pick -- WARP's multiply is not symmetric there, and neither shader's
    // result in that regime is ever seen.
    for (int r = 270; r <= 272; ++r) setRow(c.cb1[r], {u(rng), u(rng), u(rng)}, 0.02f * u(rng));
    setRow(c.cb1[273], {u(rng), u(rng), u(rng)}, 50.0f);
    c.cb1[222][2] = 0.5f + std::fabs(u(rng));
    V3 f = norm({u(rng), u(rng), u(rng) + 2.0f});
    setCamera(c, f, u(rng) * 3.0f);
    c.cb2[0][1] = (u(rng) > 0) ? 0.0f : 1.5f * std::fabs(u(rng));
    c.cb2[1][3] = 1.0f + std::fabs(u(rng)) * 3.0f;
    c.cb2[2][0] = 0.1f + 0.5f * std::fabs(u(rng));
}

// ---- comparison -------------------------------------------------------------------------------------------------
static bool nearEq(float a, float b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (std::isinf(a) && std::isinf(b)) return (a > 0) == (b > 0);
    return std::fabs(a - b) <= 1e-5f + 2e-4f * std::max(std::fabs(a), std::fabs(b));
}
// Which output elements depend on the basis, per shader: (element index, component mask by name:index).
struct Dependent { const char* name; int index; int comp; };   // comp -1: every component
static bool isDependent(const std::vector<Dependent>& dep, const SigRow& r, int comp) {
    for (const auto& d : dep) if (r.name == d.name && r.index == d.index && (d.comp < 0 || d.comp == comp)) return true;
    return false;
}
struct Compared { bool equal = true; bool dependentDiffers = false; bool independentDiffers = false; std::string first; };
// Compares vertex by vertex, element by element. `dependent` lists the elements allowed (expected) to differ.
static Compared compare(const Program& a, const std::vector<float>& ra, const Program& b, const std::vector<float>& rb,
                        const std::vector<Dependent>& dependent) {
    Compared c;
    const size_t n = ra.size() / a.outFloats;
    for (size_t v = 0; v < n; ++v)
        for (size_t e = 0; e < a.sig.out.size(); ++e)
            for (int k = 0; k < comps(a.sig.out[e].mask); ++k) {
                const float x = ra[v * a.outFloats + a.outOffset[e] + k], y = rb[v * b.outFloats + b.outOffset[e] + k];
                if (nearEq(x, y)) continue;
                c.equal = false;
                const bool dep = isDependent(dependent, a.sig.out[e], k);
                (dep ? c.dependentDiffers : c.independentDiffers) = true;
                if (c.first.empty()) {
                    char buf[200];
                    std::snprintf(buf, sizeof buf, "vertex %zu %s%d.%c: original %.6g, transcription %.6g", v, a.sig.out[e].name.c_str(),
                                  a.sig.out[e].index, "xyzw"[k], x, y);
                    c.first = buf;
                }
            }
    return c;
}
static V3 elementXYZ(const Program& p, const std::vector<float>& r, size_t vertex, const char* name, int index) {
    for (size_t e = 0; e < p.sig.out.size(); ++e)
        if (p.sig.out[e].name == name && p.sig.out[e].index == index) {
            const float* f = &r[vertex * p.outFloats + p.outOffset[e]];
            return {f[0], f[1], f[2]};
        }
    std::printf("no output %s%d\n", name, index);
    std::exit(1);
}

// ---- one shader's whole story -----------------------------------------------------------------------------------
struct Case {
    const char* hash;
    const char* source;
    std::vector<Dependent> dependent;
    bool noCb2 = false;   // the original has no b2 at all: the transcription must never read one
};

static std::string rootDir;

static void runCase(Rig& rig, const Case& cs, const std::vector<std::vector<uint8_t>>& otherOriginals, const char* const* otherNames) {
    const std::string tag = std::string("F[") + cs.hash + "].";
    const auto original = readFile(rootDir + "\\tools\\billboard_facing_test\\fixtures\\vs_" + cs.hash + ".dxbc");
    check(!original.empty(), tag + "fixture-readable");
    if (original.empty()) return;
    // F1: identity.
    char name[32];
    std::snprintf(name, sizeof name, "%016llX", static_cast<unsigned long long>(fnv1a64(original.data(), original.size())));
    check(std::string(name) == cs.hash, tag + "fixture-content-hashes-to-its-name");
    const auto mine = compile(cs.source);
    // A desk aid, not a gate: EDVR_FACING_ASM=<hash> prints that transcription's own disassembly.
    if (const char* want = std::getenv("EDVR_FACING_ASM"))
        if (std::string(want) == cs.hash) std::printf("%s\n", disassemble(mine.data(), mine.size()).c_str());
    const Sig so = parse(disassemble(original.data(), original.size()));
    Sig sm = parse(disassemble(mine.data(), mine.size()));
    // The one declaration that is ours: b3, the viewer position and world up (pCam, pUp), eight floats. The original has no b3.
    {
        const std::string ours = "dcl_constantbuffer CB3[2], immediateIndexed";
        check(std::count(sm.dcls.begin(), sm.dcls.end(), ours) == 1 && std::count(so.dcls.begin(), so.dcls.end(), ours) == 0,
              tag + "the-only-added-declaration-is-b3-two-float4s");
        sm.dcls.erase(std::remove(sm.dcls.begin(), sm.dcls.end(), ours), sm.dcls.end());
    }
    check(!so.in.empty() && !so.out.empty(), tag + "original-signatures-parsed");
    check(so.inText == sm.inText, tag + "input-signature-equal-column-for-column");
    check(so.outText == sm.outText, tag + "output-signature-equal-column-for-column");
    check(so.dcls == sm.dcls, tag + "every-dcl-line-equal");
    if (so.inText != sm.inText || so.outText != sm.outText || so.dcls != sm.dcls) {
        for (size_t i = 0; i < std::max(so.dcls.size(), sm.dcls.size()); ++i)
            std::printf("  dcl %zu: original [%s] mine [%s]\n", i, i < so.dcls.size() ? so.dcls[i].c_str() : "-", i < sm.dcls.size() ? sm.dcls[i].c_str() : "-");
        for (size_t i = 0; i < std::max(so.outText.size(), sm.outText.size()); ++i)
            std::printf("  out %zu: original [%s] mine [%s]\n", i, i < so.outText.size() ? so.outText[i].c_str() : "-", i < sm.outText.size() ? sm.outText[i].c_str() : "-");
        for (size_t i = 0; i < std::max(so.inText.size(), sm.inText.size()); ++i)
            std::printf("  in %zu: original [%s] mine [%s]\n", i, i < so.inText.size() ? so.inText[i].c_str() : "-", i < sm.inText.size() ? sm.inText[i].c_str() : "-");
    }
    // The comparator tells shaders apart: this transcription against every OTHER original must not match.
    for (size_t i = 0; i < otherOriginals.size(); ++i) {
        if (std::string(otherNames[i]) == cs.hash) continue;
        const Sig other = parse(disassemble(otherOriginals[i].data(), otherOriginals[i].size()));
        check(other.inText != sm.inText || other.outText != sm.outText || other.dcls != sm.dcls, tag + "not-equal-to-another-shaders-original-" + otherNames[i]);
    }

    Program po = makeProgram(rig, original), pm = makeProgram(rig, mine);
    std::vector<D3D11_INPUT_ELEMENT_DESC> elems;
    UINT inStride = 0;
    for (const SigRow& r : so.in) {
        D3D11_INPUT_ELEMENT_DESC e{};
        e.SemanticName = r.name.c_str();
        e.SemanticIndex = static_cast<UINT>(r.index);
        e.Format = formatFor(comps(r.mask));
        e.AlignedByteOffset = inStride;
        e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        elems.push_back(e);
        inStride += static_cast<UINT>(comps(r.mask)) * 4;
    }
    ComPtr<ID3D11InputLayout> layout;
    hr(rig.dev->CreateInputLayout(elems.data(), static_cast<UINT>(elems.size()), original.data(), original.size(), &layout), "CreateInputLayout");
    // The same layout must serve the transcription (it is the game's layout, made for the original).
    ComPtr<ID3D11InputLayout> layoutMine;
    check(SUCCEEDED(rig.dev->CreateInputLayout(elems.data(), static_cast<UINT>(elems.size()), mine.data(), mine.size(), &layoutMine)),
          tag + "the-games-input-layout-is-valid-for-the-transcription-too");
    rig.problems();

    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);

    // F2: the feed absent -- the game's own look -- on every mode and random constants.
    {
        bool allEqual = true;
        std::string first;
        for (int round = 0; round < 24; ++round) {
            Constants c;
            randomConstants(c, rng);
            c.cbp[0][3] = 0.0f;   // pCam.w
            Vertices v;
            for (int i = 0; i < 24; ++i) {
                ParticleSpec s;
                s.pos = {u(rng) * 6 - 3, u(rng) * 6 - 3, u(rng) * 20 + 1};
                s.spin = u(rng) * 6.0f;
                s.mode = (round + i) % 8;
                s.hx = 0.2f + u(rng); s.hy = 0.2f + u(rng);
                s.dir = {u(rng), u(rng), u(rng)};
                s.axis = {u(rng), u(rng), u(rng)};
                s.atlas = ((200u + static_cast<uint32_t>(u(rng) * 30000)) << 16) | static_cast<uint32_t>(u(rng) * 60000);
                s.brightness = u(rng) * 3;
                for (float& f : s.uv) f = u(rng);
                for (float& f : s.colour) f = u(rng);
                addParticle(v, so, s);
            }
            const auto a = run(rig, po, layout.Get(), inStride, v, c), b = run(rig, pm, layout.Get(), inStride, v, c);
            const Compared cmp = compare(po, a, pm, b, {});
            if (!cmp.equal) { allEqual = false; if (first.empty()) first = cmp.first; }
        }
        check(allEqual, tag + "F2-feed-absent-every-output-equals-the-original " + first);
    }

    // The scene for F3..F6: identity transforms so SV_POSITION.xyz is the corner, the camera looking down +z.
    auto scene = [&](V3 viewer, float roll, V3 fwd) {
        Constants c;
        identityRows(c);
        setCamera(c, fwd, roll);
        setRow(c.cbp[0], viewer, 1.0f);
        return c;
    };

    // F3: the feed present, the aligned modes: no difference at all.
    {
        Constants c = scene({0, 0, 0}, 0.4f, {0.2f, 0.1f, 1.0f});
        Vertices v;
        for (int mode : {1, 2, 3, 5, 6, 7}) {
            ParticleSpec s;
            s.mode = mode; s.pos = {3, 1, 8}; s.spin = 1.1f;
            addParticle(v, so, s);
            s.pos = {-4, 2, 12}; s.dir = {0.1f, 0.8f, 0.5f}; s.axis = {0.6f, 0.2f, 0.9f};
            addParticle(v, so, s);
        }
        const auto a = run(rig, po, layout.Get(), inStride, v, c), b = run(rig, pm, layout.Get(), inStride, v, c);
        const Compared cmp = compare(po, a, pm, b, {});
        check(cmp.equal, tag + "F3-aligned-modes-identical-with-the-feed-present " + cmp.first);
    }

    // F4: the viewer on the particle's axis, zero roll: every mode equals the original.
    {
        Constants c = scene({0, 0, 0}, 0.0f, {0, 0, 1});
        Vertices v;
        for (int mode = 0; mode < 8; ++mode)
            for (float z : {2.0f, 9.0f, 40.0f}) {
                ParticleSpec s;
                s.mode = mode; s.pos = {0, 0, z}; s.spin = 0.7f * static_cast<float>(mode);
                addParticle(v, so, s);
            }
        const auto a = run(rig, po, layout.Get(), inStride, v, c), b = run(rig, pm, layout.Get(), inStride, v, c);
        const Compared cmp = compare(po, a, pm, b, {});
        check(cmp.equal, tag + "F4-on-axis-zero-roll-equals-the-original-in-every-mode " + cmp.first);
    }

    // F5: off axis, a rolled camera. Modes 0 and 4 face the viewer; only the basis-dependent outputs differ.
    {
        const V3 viewer{0.5f, -0.3f, 0.2f};
        Constants c = scene(viewer, 0.7f, {0.15f, -0.1f, 1.0f});
        bool dependentDiffered = false, independentDiffered = false, facingAll = true, oldFacesViewer = true, sizeKept = true, level = true;
        std::string first;
        for (int mode : {0, 4}) {
            for (V3 pos : {V3{3, 1, 5}, V3{-4, 2, 9}, V3{1, -3, 14}, V3{6, 5, 3}}) {
                Vertices v;
                ParticleSpec s;
                s.mode = mode; s.pos = pos; s.spin = 0.0f; s.hx = 0.8f; s.hy = 0.5f;
                s.axis = {0.5f, 0.5f, 1.0f};       // mode 4: the spin axis is this (decoded), not the face axis
                s.dir = {0.5f, 0.5f, 1.0f};
                addParticle(v, so, s);
                const auto a = run(rig, po, layout.Get(), inStride, v, c), b = run(rig, pm, layout.Get(), inStride, v, c);
                const Compared cmp = compare(po, a, pm, b, cs.dependent);
                dependentDiffered |= cmp.dependentDiffers;
                if (cmp.independentDiffers) { independentDiffered = true; if (first.empty()) first = cmp.first; }
                if (mode != 0) continue;
                // Mode 0, spin zero: the four corners are p + right*sx + up*sy.
                const V3 face = norm(pos - viewer);
                V3 oc[4], nc[4];
                for (int k = 0; k < 4; ++k) { oc[k] = elementXYZ(po, a, k, "SV_POSITION", 0); nc[k] = elementXYZ(pm, b, k, "SV_POSITION", 0); }
                const V3 centreN = (nc[0] + nc[1] + nc[2] + nc[3]) * 0.25f, centreO = (oc[0] + oc[1] + oc[2] + oc[3]) * 0.25f;
                float worstNew = 0.0f, bestOld = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    const V3 offN = nc[k] - centreN, offO = oc[k] - centreO;
                    worstNew = std::max(worstNew, std::fabs(dot(offN, face)) / len(offN));
                    bestOld = std::max(bestOld, std::fabs(dot(offO, face)) / len(offO));
                    if (std::fabs(len(offN) - len(offO)) > 1e-3f * len(offO)) sizeKept = false;
                }
                if (worstNew > 1e-3f) facingAll = false;
                if (bestOld < 0.02f) oldFacesViewer = false;
                // Level: the quad's horizontal edge (corner 1 - corner 0) has no vertical component for the new basis.
                const V3 edge = nc[1] - nc[0];
                if (std::fabs(edge.y) > 1e-3f * len(edge)) level = false;
            }
        }
        check(dependentDiffered, tag + "F5-the-basis-dependent-outputs-do-differ-off-axis");
        check(!independentDiffered, tag + "F5-nothing-but-the-basis-dependent-outputs-differs " + first);
        check(facingAll, tag + "F5-every-quad-faces-the-viewer");
        check(oldFacesViewer, tag + "F5-FAIL-BEFORE-the-original-quad-does-not-face-the-viewer");
        check(sizeKept, tag + "F5-quad-size-is-the-originals");
        check(level, tag + "F5-no-roll-for-a-rolled-camera");
    }

    // F6: a particle directly above the viewer (face parallel to world up) and one at the viewer: finite everywhere.
    {
        Constants c = scene({0, 0, 0}, 0.3f, {0, 0, 1});
        Vertices v;
        for (V3 pos : {V3{0, 10, 0}, V3{0, -10, 0}, V3{0, 0, 0}, V3{0, 1e-6f, 0}}) {
            ParticleSpec s;
            s.mode = 0; s.pos = pos;
            addParticle(v, so, s);
            s.mode = 4;
            addParticle(v, so, s);
        }
        const auto b = run(rig, pm, layout.Get(), inStride, v, c);
        bool finite = true;
        for (float f : b) finite &= std::isfinite(f);
        check(finite, tag + "F6-the-degenerate-cross-stays-finite");
    }

    // F8: b2. A shader whose original declares no b2 must never read one: a b2 bound with garbage changes NOTHING (every
    // output of every vertex, bit for bit). Where the original does read b2 the same garbage does change the outputs, so the
    // test can fail (the fail-before: it is run on the ORIGINAL and on the shaders that read it).
    {
        Constants a = scene({0.5f, -0.3f, 0.2f}, 0.7f, {0.15f, -0.1f, 1.0f});
        Constants g = a;
        const float garbage[4] = {1.0e30f, -3.5f, 0.0f, 77.0f};
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 4; ++k) g.cb2[r][k] = garbage[(r + k) & 3];
        a.cb2[0][1] = 0.0f;   a.cb2[1][3] = 2.0f;   a.cb2[2][0] = 0.3f;
        Vertices v;
        for (int mode = 0; mode < 8; ++mode) {
            ParticleSpec s;
            s.mode = mode; s.pos = {3, 1, 5 + static_cast<float>(mode)}; s.spin = 0.4f * static_cast<float>(mode);
            addParticle(v, so, s);
        }
        const auto ma = run(rig, pm, layout.Get(), inStride, v, a), mg = run(rig, pm, layout.Get(), inStride, v, g);
        const auto oa = run(rig, po, layout.Get(), inStride, v, a), og = run(rig, po, layout.Get(), inStride, v, g);
        const bool mineSame = compare(pm, ma, pm, mg, {}).equal && ma.size() == mg.size() &&
                              std::memcmp(ma.data(), mg.data(), ma.size() * sizeof(float)) == 0;
        const bool origSame = compare(po, oa, po, og, {}).equal;
        if (cs.noCb2) {
            check(mineSame, tag + "F8-the-transcription-never-reads-b2-garbage-changes-nothing");
            check(origSame, tag + "F8-and-neither-does-the-original");
        } else {
            check(!mineSame, tag + "F8-FAIL-BEFORE-where-the-original-reads-b2-the-same-garbage-does-change-the-outputs-(mine)");
            check(!origSame, tag + "F8-FAIL-BEFORE-...-(the-original)");
        }
    }
    check(rig.problems() == 0, tag + "debug-layer-clean");
}

static std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static int countOf(const std::string& s, const std::string& part) {
    int n = 0;
    for (size_t at = 0; (at = s.find(part, at)) != std::string::npos; at += part.size()) ++n;
    return n;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string mode = argv[1];
    if (mode == "--dry-run") {
        std::puts("billboard_facing_test: F1-F8, WARP with the debug layer; reads the fixtures, writes nothing");
        return 0;
    }
    if (mode != "--self-test" || argc < 3) return 2;
    rootDir = argv[2];
    Rig rig;
    std::printf("billboard_facing_test: debug layer %s\n", rig.debugLayer ? "ON" : "not installed (messages not checked)");

    // The five, with the twins side by side: 9F4B / AFBC (12 in, the same outputs; AFBC has no b2 and no fade) and BBAD / 4BDE
    // (10 in, the same outputs; 4BDE has b2[1] and no fade), and 9AEC, which resembles AFBC more than AFBC's own twin does.
    const char* names[5] = {"9F4BBCFCD3B68BC9", "BBAD1CA808E1E292", "9AEC596A2B036EA6", "AFBCD3ADB9092F78", "4BDE4DBF88C2BA27"};
    std::vector<std::vector<uint8_t>> originals;
    for (const char* n : names) originals.push_back(readFile(rootDir + "\\tools\\billboard_facing_test\\fixtures\\vs_" + n + ".dxbc"));
    const Case cases[5] = {
        {names[0], edvr::kFacing9F4BWorldVS, {{"TEXCOORD", 9, 0}, {"SV_POSITION", 0, -1}}, false},                // o2.x the corner depth, and the position
        {names[1], edvr::kFacingBBADWorldVS, {{"TEXCOORD", 3, 0}, {"TEXCOORD", 3, 2}, {"SV_POSITION", 0, -1}}, false},   // o0.x clip w, o0.z the soft term, and the position
        {names[2], edvr::kFacing9AECWorldVS, {{"TEXCOORD", 6, 0}, {"SV_POSITION", 0, -1}}, true},                 // o1.x the corner depth, and the position
        {names[3], edvr::kFacingAFBCWorldVS, {{"TEXCOORD", 9, 0}, {"SV_POSITION", 0, -1}}, true},                 // o2.x the corner depth, and the position
        {names[4], edvr::kFacing4BDEWorldVS, {{"TEXCOORD", 3, 0}, {"TEXCOORD", 3, 2}, {"SV_POSITION", 0, -1}}, false},   // o0.x clip w, o0.z the soft term, and the position
    };
    for (const Case& c : cases) runCase(rig, c, originals, names);

    // F7: the wiring.
    {
        const std::string pf = slurp(rootDir + "\\src\\d3d11\\particle_fix.cpp");
        const std::string gen = slurp(rootDir + "\\tools\\temporal_shader_build\\fixed_facing_shader_variants.h");
        check(!pf.empty() && !gen.empty(), "F7.sources-readable");
        for (const char* sym : {"kFacing9F4BWorldBytecode", "kFacingBBADWorldBytecode", "kFacing9AECWorldBytecode", "kFacingAFBCWorldBytecode",
                                "kFacing4BDEWorldBytecode"}) {
            check(countOf(pf, sym) >= 1, std::string("F7.table-uses-") + sym);
            check(countOf(gen, sym) == 1, std::string("F7.generator-builds-") + sym);
        }
        // The fireball puffs left the roll-free list: each hash is written once in particle_fix.cpp, as its own variant's.
        check(countOf(pf, "0xAFBCD3ADB9092F78ull") == 1 && countOf(pf, "0x4BDE4DBF88C2BA27ull") == 1,
              "F7.the-fireball-twins-are-transcribed-and-no-longer-in-the-roll-free-list");
        check(countOf(pf, "kWitchspaceStarsVs && g_hideWitchspaceStars") == 1, "F7.starfield-stays-gated-by-fix.witchspace_stars");
    }
    if (failures) { std::printf("billboard_facing_test: %d FAILED of %u\n", failures, checks); return 1; }
    std::printf("billboard_facing_test: PASS F1-F8 (%u checks)\n", checks);
    return 0;
}
