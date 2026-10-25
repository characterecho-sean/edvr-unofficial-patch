// F121: the helmet visor's axis-centred vertex shader (src/d3d11/visor_axis_vs.h) against the game's own bytecode (VS B553BB479B7C0B97, kept in
// tools\visor_axis_test\fixtures and held to its hash name), on a WARP device with the D3D debug layer, run through stream output.
//   V1  the fixture is the shader it is named for;
//   V2  signatures and declarations: the input and output signatures and every dcl line equal column for column, ours the one added b3 (one float4);
//   V3  the shift at zero: every output register of the shipped shader equals the game's, over random constants (the tint, the slide, the colour buffer);
//   V4  the shift at s: o1.x moves by exactly s at every vertex and no other output moves;
//   V5  the sign per eye, from the production math (visor_axis_math.h) and the dump's tangents: the left eye's frame centre lands on the NDC axis +a, the
//       right eye's on -a (the pixel where the interpolated uv is 0.5), and the two are 719.5 px apart on a 4032 px image, the far-field offset the dumps
//       measured (-720);
//   V6  a symmetric frustum gives s = 0 exactly, and then the shipped shader is bit for bit the game's;
//   V7  the math refuses what it was not written for;
//   V8  the debug layer says nothing; and the wiring: the shipped bytes are the ones remlok_fix.cpp creates, bound only around the game's visor draw, with the
//       one temporary key (F121b, default off) gating the bind;
//   V9  F121b's census compute shader (the build's kVisorCensusBytecode) against its CPU model and decode (census_checks.h): non-finite, NaN, Inf, black, the
//       largest value, the mean luma, the box of the non-finite taps, the exposure texels, and the decode's refusals.
// The bytes under test are the build's (kVisorAxisVsBytecode and kVisorCensusBytecode from temporal_shader_build), not a compile made here. Output under obj\.
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
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "temporal_shader_bytecode.h"
#include "visor_axis_math.h"
#include "visor_census_core.h"

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

static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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
#include "census_checks.h"   // F121b: the census shader against its CPU model (its template takes check(); it needs the D3D headers and ComPtr above)
// The project's shader content hash (device_hook.cpp: fnv1a64 over the bytecode, the offset basis with the last digit dropped).
static uint64_t fnv1a64(const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// ---- disassembly and signatures ------------------------------------------------------------------------------------
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
struct SigRow { std::string name; int index = 0; std::string mask; int reg = 0; };
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
        if (t.size() < 6 || t[0] == "Name" || t[0][0] == '-') continue;
        SigRow r;
        r.name = t[0]; r.index = std::atoi(t[1].c_str()); r.mask = t[2]; r.reg = std::atoi(t[3].c_str());
        std::string norm;
        for (const auto& w : t) norm += w + " ";
        (section == 1 ? s.in : s.out).push_back(r);
        (section == 1 ? s.inText : s.outText).push_back(norm);
    }
    return s;
}

// ---- the device and one stream-output run --------------------------------------------------------------------------
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

// What the game binds for the visor draw: b1 (the frame block; row 90 is read), b2 (the visor's block, two rows), t0 (the colour buffer), ours at b3.
struct Constants {
    float cb1[91][4] = {};
    float cb2[2][4] = {};
    float cb3[1][4] = {};
    float t0[4][4] = {};
};

// Three vertices, no vertex buffer (SV_VertexID alone), as the game draws it. Returns 3 streamed vertices.
static std::vector<float> run(Rig& rig, const Program& prog, const Constants& c) {
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
    ComPtr<ID3D11Buffer> cb1 = makeCb(c.cb1, sizeof c.cb1), cb2 = makeCb(c.cb2, sizeof c.cb2), cb3 = makeCb(c.cb3, sizeof c.cb3);
    ID3D11Buffer* raw[3] = {cb1.Get(), cb2.Get(), cb3.Get()};
    D3D11_BUFFER_DESC td{};
    td.ByteWidth = sizeof c.t0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    td.StructureByteStride = 16;
    D3D11_SUBRESOURCE_DATA tsd{c.t0, 0, 0};
    ComPtr<ID3D11Buffer> tbuf;
    hr(rig.dev->CreateBuffer(&td, &tsd, &tbuf), "colour buffer");
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_UNKNOWN;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sv.Buffer.FirstElement = 0;
    sv.Buffer.NumElements = 4;
    ComPtr<ID3D11ShaderResourceView> srv;
    hr(rig.dev->CreateShaderResourceView(tbuf.Get(), &sv, &srv), "colour buffer view");

    D3D11_BUFFER_DESC sod{};
    sod.ByteWidth = 3 * static_cast<UINT>(prog.outFloats) * 4;
    sod.Usage = D3D11_USAGE_DEFAULT;
    sod.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    ComPtr<ID3D11Buffer> sob;
    hr(rig.dev->CreateBuffer(&sod, nullptr, &sob), "so buffer");

    rig.ctx->IASetInputLayout(nullptr);
    rig.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    rig.ctx->VSSetShader(prog.vs.Get(), nullptr, 0);
    rig.ctx->VSSetConstantBuffers(1, 3, raw);
    ID3D11ShaderResourceView* views[1] = {srv.Get()};
    rig.ctx->VSSetShaderResources(0, 1, views);
    rig.ctx->GSSetShader(prog.so.Get(), nullptr, 0);
    rig.ctx->PSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer* sot[1] = {sob.Get()};
    UINT soOffset = 0;
    rig.ctx->SOSetTargets(1, sot, &soOffset);
    rig.ctx->Draw(3, 0);
    ID3D11Buffer* none[1] = {nullptr};
    rig.ctx->SOSetTargets(1, none, &soOffset);
    rig.ctx->GSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* noview[1] = {nullptr};
    rig.ctx->VSSetShaderResources(0, 1, noview);

    D3D11_BUFFER_DESC st = sod;
    st.Usage = D3D11_USAGE_STAGING;
    st.BindFlags = 0;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> stage;
    hr(rig.dev->CreateBuffer(&st, nullptr, &stage), "staging");
    rig.ctx->CopyResource(stage.Get(), sob.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    hr(rig.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "Map");
    std::vector<float> out(static_cast<float*>(m.pData), static_cast<float*>(m.pData) + 3 * prog.outFloats);
    rig.ctx->Unmap(stage.Get(), 0);
    return out;
}

// The element offset of a named output.
static int elementOffset(const Program& p, const char* name) {
    for (size_t e = 0; e < p.sig.out.size(); ++e)
        if (p.sig.out[e].name == name) return p.outOffset[e];
    std::printf("no output %s\n", name);
    std::exit(1);
}

static bool nearEq(float a, float b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    return std::fabs(a - b) <= 1e-5f + 2e-4f * std::max(std::fabs(a), std::fabs(b));
}

// Inputs like the game's: the hue wheel's time in cb2[0].x, the slide cb2[1].x (0..1), the saturation and gain, the frame block's exposure in cb1[90].z, and a
// HUD colour in the colour buffer's first element.
static void randomConstants(Constants& c, std::mt19937& rng) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (auto& row : c.cb1) for (float& f : row) f = u(rng) * 4.0f - 1.0f;
    c.cb1[90][2] = 0.2f + u(rng) * 3.0f;
    for (auto& row : c.cb2) for (float& f : row) f = u(rng);
    c.cb2[0][0] = u(rng) * 50.0f;
    c.cb2[0][2] = u(rng) * 50.0f;
    c.cb2[0][3] = u(rng) * 2.0f;
    c.cb2[1][0] = u(rng);
    c.cb2[1][1] = u(rng) * 2.0f;
    c.cb2[1][2] = u(rng) * 2.0f;
    for (auto& row : c.t0) for (float& f : row) f = u(rng) * 2.0f;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string mode = argv[1];
    if (mode == "--dry-run") {
        std::puts("visor_axis_test: V1-V9, WARP with the debug layer; reads the fixture and the sources, writes nothing");
        return 0;
    }
    if (mode != "--self-test" || argc < 3) return 2;
    const std::string root = argv[2];
    Rig rig;
    std::printf("visor_axis_test: debug layer %s\n", rig.debugLayer ? "ON" : "not installed (messages not checked)");

    // V1: the fixture is the shader it is named for.
    const auto original = readFile(root + "\\tools\\visor_axis_test\\fixtures\\vs_B553BB479B7C0B97.dxbc");
    check(!original.empty(), "V1.fixture-readable");
    if (original.empty()) return 1;
    check(fnv1a64(original.data(), original.size()) == edvr::visoraxis::kVisorVsHash, "V1.fixture-content-hashes-to-its-name");
    const std::vector<uint8_t> mine(edvr::kVisorAxisVsBytecode, edvr::kVisorAxisVsBytecode + sizeof(edvr::kVisorAxisVsBytecode));
    check(mine.size() > 4 && std::memcmp(mine.data(), "DXBC", 4) == 0, "V1.shipped-bytes-are-DXBC");

    // V2: signatures and declarations.
    const Sig so = parse(disassemble(original.data(), original.size()));
    Sig sm = parse(disassemble(mine.data(), mine.size()));
    {
        const std::string ours = "dcl_constantbuffer CB3[1], immediateIndexed";
        check(std::count(sm.dcls.begin(), sm.dcls.end(), ours) == 1 && std::count(so.dcls.begin(), so.dcls.end(), ours) == 0,
              "V2.the-only-added-declaration-is-b3-one-float4");
        sm.dcls.erase(std::remove(sm.dcls.begin(), sm.dcls.end(), ours), sm.dcls.end());
    }
    check(so.in.size() == 1 && so.out.size() == 4, "V2.original-signatures-parsed (SV_VERTEXID in; tint, uv, colour, position out)");
    check(so.inText == sm.inText, "V2.input-signature-equal-column-for-column");
    check(so.outText == sm.outText, "V2.output-signature-equal-column-for-column");
    check(so.dcls == sm.dcls, "V2.every-dcl-line-equal");
    if (so.inText != sm.inText || so.outText != sm.outText || so.dcls != sm.dcls) {
        for (size_t i = 0; i < std::max(so.dcls.size(), sm.dcls.size()); ++i)
            std::printf("  dcl %zu: original [%s] mine [%s]\n", i, i < so.dcls.size() ? so.dcls[i].c_str() : "-", i < sm.dcls.size() ? sm.dcls[i].c_str() : "-");
        for (size_t i = 0; i < std::max(so.outText.size(), sm.outText.size()); ++i)
            std::printf("  out %zu: original [%s] mine [%s]\n", i, i < so.outText.size() ? so.outText[i].c_str() : "-", i < sm.outText.size() ? sm.outText[i].c_str() : "-");
    }

    const Program po = makeProgram(rig, original), pm = makeProgram(rig, mine);
    check(po.outFloats == pm.outFloats && po.outFloats == 4 + 2 + 3 + 4, "V2.the-streamed-vertex-is-13-floats-in-both");
    const int uvO = elementOffset(po, "__USER_VERTEX_M_TEXCOORD"), uvM = elementOffset(pm, "__USER_VERTEX_M_TEXCOORD");
    const int posO = elementOffset(po, "SV_POSITION"), posM = elementOffset(pm, "SV_POSITION");
    rig.problems();

    std::mt19937 rng(121);

    // V3 / V4: random constants; the shift at zero, then at three values of s.
    {
        bool zeroEqual = true, zeroBits = true, shiftOnlyU = true, uMovesBy = true, control = false;
        std::string first;
        const float shifts[3] = {0.0892f, -0.0892f, 0.31f};
        for (int round = 0; round < 48; ++round) {
            Constants c;
            randomConstants(c, rng);
            c.cb3[0][0] = 0.0f;
            const auto a = run(rig, po, c), b = run(rig, pm, c);
            for (size_t i = 0; i < a.size(); ++i) {
                if (!nearEq(a[i], b[i])) { zeroEqual = false; if (first.empty()) { char t[160]; std::snprintf(t, sizeof t, "round %d float %zu original %.7g shipped %.7g", round, i, a[i], b[i]); first = t; } }
                if (std::memcmp(&a[i], &b[i], 4) != 0 && (i % 13 == 4 || i % 13 == 5 || i % 13 >= 9)) zeroBits = false;   // the uv and the position: exact
            }
            for (float s : shifts) {
                c.cb3[0][0] = s;
                const auto m = run(rig, pm, c);
                for (int v = 0; v < 3; ++v) {
                    for (int k = 0; k < 13; ++k) {
                        const float x = a[v * 13 + k], y = m[v * 13 + k];
                        const bool isU = (k == uvM);   // o1.x
                        if (isU) {
                            if (std::fabs((x - s) - y) > 1e-6f) { uMovesBy = false; if (first.empty()) first = "o1.x did not move by exactly s"; }
                            if (std::fabs(x - y) > 1e-6f) control = true;   // the strict comparison can fail
                        } else if (!nearEq(x, y)) {
                            shiftOnlyU = false;
                            if (first.empty()) { char t[160]; std::snprintf(t, sizeof t, "shift %.4f vertex %d float %d moved besides o1.x", s, v, k); first = t; }
                        }
                    }
                }
            }
        }
        check(zeroEqual, "V3.shift-zero-every-output-equals-the-game's " + first);
        check(zeroBits, "V3.shift-zero-the-uv-and-the-position-are-bit-for-bit-the-game's");
        check(uMovesBy, "V4.o1.x-moves-by-exactly-s-at-every-vertex " + first);
        check(shiftOnlyU, "V4.no-other-output-moves " + first);
        check(control, "V4.control-the-strict-comparison-does-see-the-shift");
        check(uvO == uvM && posO == posM && uvO == 4 && posO == 9, "V4.the-uv-is-floats-4-5-and-the-position-floats-9-12-in-both");
    }

    // V5: the sign per eye, with the production math and the dump's tangents (outer 1.2546, inner 0.8746: m02 -/+0.1784, the Pimax Crystal Super).
    {
        const float outer = 1.2546f, inner = 0.8746f;
        float a = 0.0f, sL = 0.0f, sR = 0.0f;
        check(edvr::visoraxis::axisNdc(outer, inner, &a) && std::fabs(a - 0.1784f) < 5e-4f, "V5.the-axis-is-0.1784-NDC-from-the-tangents");
        check(edvr::visoraxis::shiftUv(outer, inner, 0, &sL) && edvr::visoraxis::shiftUv(outer, inner, 1, &sR), "V5.both-eyes-have-a-shift");
        check(sL > 0.0f && sR < 0.0f && std::fabs(sL + sR) < 1e-7f && std::fabs(sL - 0.0892f) < 3e-4f, "V5.left-eye-positive-right-eye-negative-+-0.0892");
        // Where the shipped shader puts the pixel whose interpolated uv is 0.5 (the visor frame's centre): along the triangle's bottom edge, vertex 0 (clip -1) to
        // vertex 2 (clip 3). The stock frame's centre is at clip 0; the left eye's axis is at +a and the right eye's at -a.
        float centreX[2] = {0, 0};
        for (int eye = 0; eye < 2; ++eye) {
            Constants c;
            randomConstants(c, rng);
            c.cb2[1][0] = 1.0f;   // the visor fully down: no slide
            c.cb3[0][0] = eye == 0 ? sL : sR;
            const auto m = run(rig, pm, c);
            const float u0 = m[0 * 13 + uvM], u2 = m[2 * 13 + uvM], x0 = m[0 * 13 + posM], x2 = m[2 * 13 + posM];
            centreX[eye] = x0 + (0.5f - u0) / (u2 - u0) * (x2 - x0);
            // the CPU model of the same uv
            check(std::fabs(u0 - edvr::visoraxis::uvAtClipX(x0, eye == 0 ? sL : sR)) < 1e-6f && std::fabs(u2 - edvr::visoraxis::uvAtClipX(x2, eye == 0 ? sL : sR)) < 1e-6f,
                  std::string("V5.the-cpu-model-of-the-uv-matches-the-shader-eye-") + (eye == 0 ? "left" : "right"));
        }
        check(std::fabs(centreX[0] - a) < 2e-4f, "V5.left-eye-the-frame-centre-lands-on-its-axis-at-NDC-+a (the left eye shifts right)");
        check(std::fabs(centreX[1] + a) < 2e-4f, "V5.right-eye-the-frame-centre-lands-on-its-axis-at-NDC--a (the right eye shifts left)");
        // 4032 px image: NDC x -> pixel x = (x + 1) * 2016; the two centres' separation is the far-field disparity the dumps measured, 720 px.
        const float apart = (centreX[0] - centreX[1]) * 2016.0f;
        check(std::fabs(apart - 720.0f) < 2.5f, "V5.the-two-eyes'-frame-centres-are-720-px-apart-on-4032-the-far-field-offset-of-the-dumps");
        check(!std::signbit(sL) && std::signbit(sR), "V5.signs");
    }

    // V6: a symmetric frustum.
    {
        float sL = 9.0f, sR = 9.0f;
        check(edvr::visoraxis::shiftUv(1.0f, 1.0f, 0, &sL) && edvr::visoraxis::shiftUv(1.0f, 1.0f, 1, &sR), "V6.symmetric-tangents-are-accepted");
        check(sL == 0.0f && sR == 0.0f && !std::signbit(sL) && !std::signbit(sR), "V6.s-is-exactly-zero-for-both-eyes-with-no-negative-zero");
        float tiny = 9.0f;
        check(edvr::visoraxis::shiftUv(1.0f, 1.00005f, 0, &tiny) && tiny == 0.0f, "V6.a-sub-0.0001-axis-is-zero-too");
        Constants c;
        randomConstants(c, rng);
        c.cb3[0][0] = sL;
        const auto a = run(rig, po, c), b = run(rig, pm, c);
        bool uvPos = true;
        for (int v = 0; v < 3; ++v) for (int k : {4, 5, 9, 10, 11, 12}) if (std::memcmp(&a[v * 13 + k], &b[v * 13 + k], 4) != 0) uvPos = false;
        check(uvPos, "V6.with-s-zero-the-uv-and-position-are-bit-for-bit-the-game's");
    }

    // V7: what the math refuses.
    {
        float s = 0.0f;
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        check(!edvr::visoraxis::shiftUv(nan, 1.0f, 0, &s) && !edvr::visoraxis::shiftUv(1.0f, nan, 0, &s) && !edvr::visoraxis::shiftUv(inf, 1.0f, 0, &s),
              "V7.non-finite-tangents-refused");
        check(!edvr::visoraxis::shiftUv(0.0f, 1.0f, 0, &s) && !edvr::visoraxis::shiftUv(1.0f, -1.0f, 0, &s), "V7.non-positive-tangents-refused");
        check(!edvr::visoraxis::shiftUv(10.0f, 1.0f, 0, &s), "V7.an-axis-past-0.6-NDC-refused");
        check(!edvr::visoraxis::shiftUv(1.2f, 0.9f, 2, &s) && !edvr::visoraxis::shiftUv(1.2f, 0.9f, -1, &s), "V7.an-eye-other-than-0-and-1-refused");
        check(edvr::visoraxis::shiftUv(1.2f, 0.9f, 0, &s) && s > 0.0f, "V7.control-a-good-value-is-accepted");
    }

    check(rig.problems() == 0, "V8.debug-layer-clean");

    // V8: the wiring.
    {
        const std::string rf = slurp(root + "\\src\\d3d11\\remlok_fix.cpp");
        const std::string gen = slurp(root + "\\tools\\temporal_shader_build\\fixed_visor_axis_variants.h");
        const std::string ini = slurp(root + "\\edvr.ini");
        check(!rf.empty() && !gen.empty() && !ini.empty(), "V8.sources-readable");
        check(countOf(rf, "kVisorAxisVsBytecode") >= 1 && countOf(gen, "kVisorAxisVsBytecode") == 1, "V8.the-shipped-bytes-are-the-ones-remlok_fix.cpp-creates-and-the-generator-builds");
        check(countOf(rf, "visoraxis::kVisorPsHash") >= 1 && countOf(rf, "Refusal::kOtherPs") >= 1, "V8.a-pixel-shader-other-than-the-designed-one-is-refused");
        check(countOf(rf, "VSSetShader(g_axisVs") == 1 && countOf(rf, "VSSetShader(g_axisSaved") == 1, "V8.the-vertex-shader-is-bound-once-and-put-back-once");
        // F121b: the one temporary key, default off, read where remlokConfigure reads fix.remlok_lines; the shift is bound only with it on.
        check(countOf(ini, "\nnative_on_foot_visor_axis = off\n") == 1 && countOf(ini, "native_on_foot_visor_axis") == 1, "V8.F121b-the-ini-has-the-one-key-and-it-defaults-to-off");
        check(countOf(rf, "getString(\"fix.native_on_foot_visor_axis\", \"off\")") == 1, "V8.F121b-the-code-reads-the-key-with-the-same-default");
        check(countOf(rf, "g_axisKeyOn.load(") >= 2 && countOf(rf, "++g_axisCounts.keyOff") == 1, "V8.F121b-the-bind-is-gated-by-the-key-and-a-draw-with-it-off-is-counted");
        // F121b: the census is measure only: it reads a copy, never takes the target off the output merger and never writes the game's resources.
        check(countOf(rf, "kVisorCensusBytecode") >= 1 && countOf(slurp(root + "\\tools\\temporal_shader_build\\temporal_shader_build.cpp"), "kVisorCensusBytecode") == 1,
              "V8.F121b-the-census-bytes-are-the-ones-remlok_fix.cpp-creates-and-the-generator-builds");
        check(countOf(rf, "OMSetRenderTargets") == 0 && countOf(rf, "UpdateSubresource") == 0 && countOf(rf, "ctx->CopyResource(g.copy[e], tex)") == 1 &&
                  countOf(rf, "censusRun(ctx);") == 1 && countOf(rf, "censusBegin(ctx, eye)") == 1,
              "V8.F121b-the-census-copies-the-target-it-does-not-unbind-it-and-runs-once-after-the-draw");
    }

    // V9: the census shader (F121b), the build's bytecode, against the CPU model and the decode.
    censuschecks::census(rig.dev.Get(), rig.ctx.Get(), edvr::kVisorCensusBytecode, sizeof(edvr::kVisorCensusBytecode), [](bool ok, const char* label) { check(ok, label); });
    check(rig.problems() == 0, "V9.debug-layer-clean");
    if (failures) { std::printf("visor_axis_test: %d FAILED of %u\n", failures, checks); return 1; }
    std::printf("visor_axis_test: PASS V1-V9 (%u checks)\n", checks);
    return 0;
}
