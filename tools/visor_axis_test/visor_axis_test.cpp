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
//       largest value, the mean luma, the box of the non-finite taps, the exposure texels, and the decode's refusals;
//   V10 F121c: the inputs the game's shader survives only by its 1e-9 floors (a colour of zero, negative or tiny; an exposure of 0): every output bit for bit the
//       game's, all finite, with the shift at zero and at s (only o1.x moves); and a control, the same shader with the floors at 0.0 (F121 and F121b), which does
//       go non-finite here;
//   V11 F121c: the float immediates of the two token streams audited against each other, read from the tokens (the disassembler's text rounds them to six
//       decimals and prints 1e-9 as 0.000000), with a control that the audit sees a floor changed to 0;
//   V12 F121c: the eye-trust guard (visor_eye_trust.h): pairs trusted, one eye or one target not, the submitted targets overruling, a retarget costing one draw,
//       the state lines after kTrustRun draws, the 10 s window only for an anomaly, and the log lines;
//   V13 F121c: the census's self-off verdict, its line, and the 30 s line's new counters.
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
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "temporal_shader_bytecode.h"
#include "visor_axis_math.h"
#include "visor_axis_vs.h"      // F121c: the HLSL text, for the control that compiles the F121 floors (0.0) beside the shipped bytes
#include "visor_census_core.h"
#include "visor_eye_trust.h"

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

// The 32-bit immediates of a shader's token stream, read from the tokens themselves: a one-component immediate operand is the token 0x00004001 and one value, a
// four-component one 0x00004002 and four (customdata skipped). The disassembler's text rounds them to six decimals and prints 1e-9 as 0.000000.
static std::vector<uint32_t> immediates(const std::vector<uint8_t>& dxbc) {
    std::vector<uint32_t> out;
    if (dxbc.size() < 36 || std::memcmp(dxbc.data(), "DXBC", 4) != 0) return out;
    uint32_t chunks = 0;
    std::memcpy(&chunks, dxbc.data() + 28, 4);
    for (uint32_t c = 0; c < chunks && 32u + 4u * (c + 1) <= dxbc.size(); ++c) {
        uint32_t off = 0;
        std::memcpy(&off, dxbc.data() + 32 + 4 * c, 4);
        if (static_cast<size_t>(off) + 8 > dxbc.size()) break;
        if (std::memcmp(dxbc.data() + off, "SHEX", 4) != 0 && std::memcmp(dxbc.data() + off, "SHDR", 4) != 0) continue;
        uint32_t size = 0;
        std::memcpy(&size, dxbc.data() + off + 4, 4);
        if (static_cast<size_t>(off) + 8 + size > dxbc.size()) break;
        std::vector<uint32_t> t(size / 4);
        std::memcpy(t.data(), dxbc.data() + off + 8, t.size() * 4);
        size_t i = 2;
        while (i < t.size()) {
            const uint32_t op = t[i] & 0x7ffu;
            size_t len = (t[i] >> 24) & 0x7fu;
            if (op == 0x35u) len = i + 1 < t.size() ? t[i + 1] : 0;
            if (len == 0) break;
            if (op != 0x35u) {
                for (size_t j = i + 1; j < i + len && j < t.size();) {
                    if (t[j] == 0x00004001u && j + 1 < i + len) { out.push_back(t[j + 1]); j += 2; }
                    else if (t[j] == 0x00004002u && j + 4 < i + len) { for (int k = 1; k <= 4; ++k) out.push_back(t[j + k]); j += 5; }
                    else ++j;
                }
            }
            i += len;
        }
        break;
    }
    return out;
}
static std::string hexList(const std::set<uint32_t>& s) {
    std::string r;
    for (uint32_t b : s) {
        char t[32];
        float f;
        std::memcpy(&f, &b, 4);
        std::snprintf(t, sizeof t, " %08x(%g)", b, static_cast<double>(f));
        r += t;
    }
    return r;
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
        std::puts("visor_axis_test: V1-V13, WARP with the debug layer; reads the fixture and the sources, writes nothing");
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

    // V10 (F121c): the inputs the game's shader survives only by its 1e-9 floors. A colour at t0[0] of zero, negative or tiny, or a frame block exposure (cb1[90].z) of
    // zero, negative or tiny, would take rsq(0) * 0 to NaN without them; the game's shader gives a finite colour (a zero colour is the direction (1,1,1)/sqrt(3)). The
    // flight of 2026-10-25 15:11 (the RTX 5090) showed F121's floors at 0.0 turn the whole scene NaN. Bit for bit, on WARP, with the shift at zero and at s.
    {
        const float colours[5][4] = {{0.0f, 0.0f, 0.0f, 0.0f}, {-1.0f, -2.0f, -3.0f, 1.0f}, {1.0e-30f, 1.0e-30f, 1.0e-30f, 1.0f}, {5.0e-10f, 0.0f, 0.0f, 1.0f}, {0.2f, 0.7f, 0.9f, 1.0f}};
        const float exposures[5] = {0.0f, -1.0f, 1.0e-30f, 5.0e-10f, 1.0f};
        bool bits = true, finite = true, shiftOnlyU = true;
        std::string first;
        unsigned cases = 0;
        for (int ci = 0; ci < 5; ++ci) {
            for (int ei = 0; ei < 5; ++ei) {
                Constants c;
                randomConstants(c, rng);
                std::memcpy(c.t0[0], colours[ci], 16);
                c.cb1[90][2] = exposures[ei];
                c.cb3[0][0] = 0.0f;
                const auto a = run(rig, po, c), b = run(rig, pm, c);
                ++cases;
                for (size_t i = 0; i < a.size(); ++i) {
                    if (std::memcmp(&a[i], &b[i], 4) != 0) { bits = false; if (first.empty()) { char t[160]; std::snprintf(t, sizeof t, "colour %d exposure %d float %zu original %.9g shipped %.9g", ci, ei, i, a[i], b[i]); first = t; } }
                    if (!std::isfinite(a[i]) || !std::isfinite(b[i])) { finite = false; if (first.empty()) { char t[160]; std::snprintf(t, sizeof t, "colour %d exposure %d float %zu not finite (original %g shipped %g)", ci, ei, i, a[i], b[i]); first = t; } }
                }
                const float s = 0.0892f;
                c.cb3[0][0] = s;
                const auto m = run(rig, pm, c);
                for (int v = 0; v < 3; ++v)
                    for (int k = 0; k < 13; ++k) {
                        const float x = a[v * 13 + k], y = m[v * 13 + k];
                        if (k == uvM) { if (std::fabs((x - s) - y) > 1e-6f) shiftOnlyU = false; }
                        else if (std::memcmp(&x, &y, 4) != 0) shiftOnlyU = false;
                    }
            }
        }
        check(cases == 25, "V10.25-degenerate-cases-ran");
        check(bits, "V10.degenerate-inputs-every-output-bit-for-bit-the-game's " + first);
        check(finite, "V10.degenerate-inputs-every-output-finite-in-both " + first);
        check(shiftOnlyU, "V10.degenerate-inputs-with-the-shift-only-o1.x-moves-and-every-other-output-is-still-bit-for-bit");
        // The control: the floors at 0.0, as F121 and F121b had them. The rig must see that go non-finite, or the checks above prove nothing.
        std::string broken = edvr::kVisorAxisVS;
        int swapped = 0;
        for (size_t at = 0; (at = broken.find("1.0e-9", at)) != std::string::npos; at += 3) { broken.replace(at, 6, "0.0"); ++swapped; }
        check(swapped == 2, "V10.control-the-two-floors-are-in-the-text-to-break");
        ComPtr<ID3DBlob> code, err;
        const HRESULT chr = D3DCompile(broken.c_str(), broken.size(), "visor_axis_vs_floors_zero", nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, &err);
        check(SUCCEEDED(chr) && code, "V10.control-the-zero-floor-text-compiles");
        if (SUCCEEDED(chr) && code) {
            const std::vector<uint8_t> bytes(static_cast<uint8_t*>(code->GetBufferPointer()), static_cast<uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
            const Program pb = makeProgram(rig, bytes);
            Constants c;
            randomConstants(c, rng);
            std::memset(c.t0[0], 0, 16);
            c.cb3[0][0] = 0.0f;
            const auto z = run(rig, pb, c);
            bool nonFinite = false;
            for (float f : z) if (!std::isfinite(f)) nonFinite = true;
            check(nonFinite, "V10.control-the-F121-floors-at-0.0-do-go-non-finite-on-a-zero-colour");
        }
    }

    // V11 (F121c): the float immediates of the two token streams, from the tokens. F121 and F121b were written from the disassembler's text, which prints 1e-9 as
    // l(0.000000) and 0.707103 for 0.70710295: the audit reads the bits.
    {
        const auto io = immediates(original), im = immediates(mine);
        const std::set<uint32_t> so_(io.begin(), io.end()), sm_(im.begin(), im.end());
        check(so_.size() == 19 && so_.count(0x3089705fu) == 1 && so_.count(0x3f3504b2u) == 1 && so_.count(0xbf350534u) == 1,
              "V11.the-game's-19-distinct-immediates-are-read-from-its-tokens " + hexList(so_));
        std::set<uint32_t> missing, extra;
        for (uint32_t b : so_) if (!sm_.count(b)) missing.insert(b);
        for (uint32_t b : sm_) if (!so_.count(b)) extra.insert(b);
        check(missing.empty(), "V11.every-immediate-of-the-game's-shader-is-in-the-shipped-one, missing:" + hexList(missing));
        check(extra.empty(), "V11.the-shipped-shader-has-no-immediate-the-game's-lacks, extra:" + hexList(extra));
        // The control: the game's own stream with its 1e-9 floors zeroed must differ from the shipped one, or the audit proves nothing.
        std::vector<uint8_t> floored = original;
        int zeroed = 0;
        for (size_t at = 0; at + 4 <= floored.size(); at += 4) {
            uint32_t w = 0;
            std::memcpy(&w, floored.data() + at, 4);
            if (w == 0x3089705fu) { std::memset(floored.data() + at, 0, 4); ++zeroed; }
        }
        const auto ib = immediates(floored);
        const std::set<uint32_t> sb(ib.begin(), ib.end());
        check(zeroed == 4 && sb != sm_, "V11.control-the-audit-sees-the-floors-changed-to-0 (three lanes of the colour's and the exposure's: four immediates)");
    }

    // V12 (F121c): the eye-trust guard.
    {
        using edvr::visoraxis::EyeTrust;
        using edvr::visoraxis::FilingState;
        using edvr::visoraxis::Why;
        int a0 = 0, a1 = 0;
        const void* A = &a0;
        const void* B = &a1;
        int c0 = 0;
        const void* C = &c0;
        {   // L R L R ...: the first untrusted (nothing to compare with), the rest trusted, paired after kTrustRun draws
            EyeTrust t;
            int events = 0, trusted = 0, first = 0;
            for (int i = 0; i < 40; ++i) {
                const auto v = t.note(i & 1, (i & 1) ? B : A, -1);
                if (v.event) { ++events; check(t.state() == FilingState::kPaired, "V12.pairs-are-logged-as-paired"); }
                if (v.trusted) ++trusted;
                if (i == 0) first = (v.why == Why::kFirst && !v.trusted) ? 1 : 0;
            }
            check(first == 1 && trusted == 39 && events == 1, "V12.L-R-pairs-the-first-draw-alone-is-untrusted-and-paired-is-logged-once");
            check(t.slot(0) == A && t.slot(1) == B, "V12.the-filed-targets-per-eye-are-kept");
        }
        {   // R L R L ...: either order of the two eyes
            EyeTrust t;
            int trusted = 0;
            for (int i = 0; i < 20; ++i) if (t.note(1 - (i & 1), (i & 1) ? A : B, -1).trusted) ++trusted;
            check(trusted == 19, "V12.R-L-pairs-are-trusted-too");
        }
        {   // the flight of 15:11: one visor draw a frame, every one filed left, on the right eye's target (which the submitted targets know as eye 1)
            EyeTrust t;
            int untrusted = 0, events = 0;
            FilingState last = FilingState::kUnknown;
            for (int i = 0; i < 40; ++i) {
                const auto v = t.note(0, B, 1);
                if (!v.trusted) ++untrusted;
                if (v.event) { ++events; last = t.state(); }
            }
            check(untrusted == 40 && events == 1 && last == FilingState::kDisagrees, "V12.the-flight-all-filed-left-on-the-right-eyes-target-is-never-trusted-and-said-once-as-disagreeing");
        }
        {   // the same, with the submitted targets not known
            EyeTrust t;
            int untrusted = 0, events = 0;
            FilingState last = FilingState::kUnknown;
            for (int i = 0; i < 40; ++i) {
                const auto v = t.note(0, B, -1);
                if (!v.trusted) ++untrusted;
                if (v.event) { ++events; last = t.state(); }
            }
            check(untrusted == 40 && events == 1 && last == FilingState::kOneEye, "V12.one-eye-only-with-no-submitted-targets-is-never-trusted-and-said-once");
        }
        {   // both eyes' draws on one target
            EyeTrust t;
            int trusted = 0;
            for (int i = 0; i < 30; ++i) if (t.note(i & 1, A, -1).trusted) ++trusted;
            check(trusted == 0, "V12.two-eyes-on-one-target-is-never-trusted");
        }
        {   // the submitted targets overrule a pair that alternates
            EyeTrust t;
            int trusted = 0, mismatched = 0;
            for (int i = 0; i < 30; ++i) {
                const int eye = i & 1;
                const auto v = t.note(eye, eye ? B : A, 1 - eye);   // the filing has the eyes swapped against the submitted targets
                if (v.trusted) ++trusted;
                if (v.why == Why::kSubmitted) ++mismatched;
            }
            check(trusted == 0 && mismatched == 30, "V12.a-pair-the-submitted-targets-contradict-is-not-trusted");
            EyeTrust u;
            int ok = 0;
            for (int i = 0; i < 30; ++i) if (u.note(i & 1, (i & 1) ? B : A, i & 1).trusted) ++ok;
            check(ok == 29, "V12.a-pair-the-submitted-targets-confirm-is-trusted");
        }
        {   // a retarget (a resize) costs one draw, and recovers; a broken spell recovers to paired (said again)
            EyeTrust t;
            for (int i = 0; i < 20; ++i) t.note(i & 1, (i & 1) ? B : A, -1);
            const auto m = t.note(0, C, -1);   // the left target moved: the previous draw was the right eye's
            check(!m.trusted && m.why == Why::kMoved, "V12.a-retarget-is-one-untrusted-draw");
            int trusted = 0;
            for (int i = 0; i < 20; ++i) if (t.note(1 - (i & 1), (i & 1) ? C : B, -1).trusted) ++trusted;
            check(trusted == 20, "V12.and-pairs-resume-trusted-on-the-new-target");
            EyeTrust u;
            int events = 0;
            for (int i = 0; i < 30; ++i) if (u.note(i & 1, (i & 1) ? B : A, -1).event) ++events;       // paired
            for (int i = 0; i < 30; ++i) if (u.note(0, A, -1).event) ++events;                           // one eye only
            for (int i = 0; i < 30; ++i) if (u.note(i & 1, (i & 1) ? B : A, -1).event) ++events;       // paired again
            check(events == 3 && u.state() == FilingState::kPaired, "V12.each-change-of-state-is-one-event");
        }
        {   // a spell shorter than kTrustRun is not an event
            EyeTrust t;
            int events = 0;
            for (int i = 0; i < 30; ++i) if (t.note(i & 1, (i & 1) ? B : A, -1).event) ++events;
            for (int i = 0; i < 5; ++i) if (t.note(0, A, -1).event) ++events;
            for (int i = 0; i < 30; ++i) if (t.note((i & 1) ? 0 : 1, (i & 1) ? A : B, -1).event) ++events;
            check(events == 1, "V12.five-bad-draws-are-not-an-event");
        }
        {   // the window: the first is said, a clean one after it is not, an anomalous one is
            EyeTrust t;
            edvr::visoraxis::EyeWindow w;
            for (int i = 0; i < 20; ++i) t.note(i & 1, (i & 1) ? B : A, -1);
            check(t.takeWindow(&w) && w.draws == 20 && w.why[static_cast<unsigned>(Why::kOk)] == 19 && w.tallies == 2, "V12.the-sessions-first-window-is-said");
            for (int i = 0; i < 20; ++i) t.note(i & 1, (i & 1) ? B : A, -1);
            check(!t.takeWindow(&w) && w.draws == 20, "V12.a-clean-window-after-it-is-not-said");
            for (int i = 0; i < 20; ++i) t.note(0, A, -1);
            check(t.takeWindow(&w) && w.anomalous(), "V12.a-window-with-draws-to-one-eye-is-said");
            check(!t.takeWindow(&w) && w.draws == 0, "V12.an-empty-window-is-not-said");
        }
        check(edvr::visoraxis::submittedEyeOf(A, A, B) == 0 && edvr::visoraxis::submittedEyeOf(B, A, B) == 1 && edvr::visoraxis::submittedEyeOf(C, A, B) == -1 &&
                  edvr::visoraxis::submittedEyeOf(A, nullptr, nullptr) == -1 && edvr::visoraxis::submittedEyeOf(A, A, A) == -1 && edvr::visoraxis::submittedEyeOf(nullptr, A, B) == -1,
              "V12.the-submitted-eye-of-a-target-is-0-1-or-unknown");
        // the lines
        {
            char line[640];
            edvr::visoraxis::formatEvent(line, sizeof line, FilingState::kDisagrees, B, nullptr, 0, B, A, B, 1234);
            const std::string s = line;
            check(s.find("visor eyes (F121c): DISAGREES with the submitted eye targets") == 0 && s.find("for the left eye and 0000000000000000 for the right") != std::string::npos &&
                      s.find("this draw is eye 0 on target ") != std::string::npos && s.find("1234 visor draws so far.") != std::string::npos,
                  "V12.the-event-line-reads");
            EyeTrust t;
            for (int i = 0; i < 6; ++i) t.note(0, B, -1);
            edvr::visoraxis::EyeWindow w;
            t.takeWindow(&w);
            edvr::visoraxis::formatWindow(line, sizeof line, w);
            const std::string ws = line;
            check(ws.find("visor eyes (F121c) 10 s: visor draws 6: paired 0, first 1, one eye only 5, target moved 0, submitted-eye mismatch 0; targets: ") == 0 &&
                      ws.find(" eye 0 x 6") != std::string::npos,
                  "V12.the-window-line-reads " + ws);
            edvr::visoraxis::EyeWindow none;
            edvr::visoraxis::formatTargets(line, sizeof line, none);
            check(std::string(line) == "none", "V12.no-targets-read-none");
        }
    }

    // V13 (F121c): the census's self-off verdict, its line, and the 30 s line's new counters.
    {
        edvr::visorcensus::Census clean, bad, notOk;
        clean.ok = true; clean.taps = edvr::visorcensus::kSamples;
        bad = clean; bad.nonFinite = 5; bad.nan = 5;
        notOk = bad; notOk.ok = false;
        check(edvr::visorcensus::selfOffVerdict(true, bad, 0, 0) && !edvr::visorcensus::selfOffVerdict(true, clean, 0, 0) && !edvr::visorcensus::selfOffVerdict(false, bad, 0, 0) &&
                  !edvr::visorcensus::selfOffVerdict(true, notOk, 0, 0),
              "V13.self-off-only-on-a-sample-after-a-draw-through-the-replacement-with-a-non-finite-tap");
        const uint64_t oldAttempt = 7, retryAttempt = oldAttempt + 1;
        bool selfOff = false;   // key off cleared the latch; two older GPU readbacks are still in flight
        for (int pending = 0; pending < 2; ++pending)
            selfOff = selfOff || edvr::visorcensus::selfOffVerdict(true, bad, oldAttempt, retryAttempt);
        check(!selfOff && !edvr::visorcensus::selfOffVerdict(true, clean, retryAttempt, retryAttempt) &&
                  edvr::visorcensus::selfOffVerdict(true, bad, retryAttempt, retryAttempt),
              "V13.key-cycle-retry-ignores-pending-old-NaN-samples-and-still-guards-new-draws");
        char line[480];
        edvr::visoraxis::formatSelfOff(line, sizeof line, 29753344u, 29753344u, 1);
        const std::string s = line;
        check(s.find("visor axis (F121c): SELF-OFF -- the HDR scene right after a draw through the replacement vertex shader had 29753344 non-finite taps of 29753344 (eye 1)") == 0 &&
                  s.find("until fix.native_on_foot_visor_axis is turned off and on again.") != std::string::npos,
              "V13.the-self-off-line-reads");
        edvr::visoraxis::Counts counts;
        counts.refused[static_cast<unsigned>(edvr::visoraxis::Refusal::kOneEye)] = 7;
        counts.selfOff = 3;
        edvr::visoraxis::formatWindow(line, sizeof line, counts, 0.089f, -0.089f, 1.254f, 0.875f, true);
        const std::string w30 = line;
        check(w30.find("refused 7 (no eye 0, no tangents 0, other pixel shader 0, no replacement 0, eye filing 7), self-off 3; s left +0.0890") != std::string::npos,
              "V13.the-30-s-line-counts-the-eye-filing-refusals-and-the-self-off-draws");
        check(std::string(edvr::visoraxis::refusalText(edvr::visoraxis::Refusal::kOneEye)).find("the eye filing is not trusted") == 0, "V13.the-refusal-reads");
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
        // F121c: and its two copies go past the copy hook (the hook tells the UI layer's eye check about a copy of the HDR target), so neither is a ctx->CopyResource.
        check(countOf(rf, "OMSetRenderTargets") == 0 && countOf(rf, "UpdateSubresource") == 0 && countOf(rf, "vScreenCopyResourceRaw(ctx, g.copy[e], tex)") == 1 &&
                  countOf(rf, "vScreenCopyResourceRaw(ctx, g.stage[e][si], g.out[e])") == 1 && countOf(rf, "ctx->CopyResource(") == 0 && countOf(rf, "CopySubresourceRegion") == 0 &&
                  countOf(rf, "censusRun(ctx);") == 1 && countOf(rf, "censusBegin(ctx, eye)") == 1,
              "V8.F121b-the-census-copies-the-target-it-does-not-unbind-it-and-runs-once-after-the-draw-and-F121c-its-copies-skip-the-copy-hook");
        // F125: an axis attempt or detailed logging enables the on-foot census. Detailed logging alone measures the game's unshifted draw; the replacement
        // still requires the axis key, eye trust and a live attempt, and only a shifted sample from that attempt may turn it off.
        check(countOf(rf, "if (onFoot && (g_axisKeyOn.load(std::memory_order_relaxed) || nofoot::logwin::detail())) {\n        censusBegin(ctx, eye);") == 1 &&
                  countOf(rf, "if (detail::g_remlokMode == Mode::kOuter && (g_axisKeyOn.load(std::memory_order_relaxed) || nofoot::logwin::detail()) && !g_censusArmedNoted)") == 1,
              "V8.F125-the-census-is-armed-and-begun-for-axis-on-or-detail-on-and-samples-only-on-foot");
        check(countOf(rf, "if (!g_axisKeyOn.load(std::memory_order_relaxed)) {") == 1 &&
                  countOf(rf, "sl.keyOn = g_axisKeyOn.load(std::memory_order_relaxed);") == 1 &&
                  countOf(rf, "if (sl.attempt == g_axisAttempt) censusAdd(e, c, sl.keyOn);") == 1 &&
                  countOf(rf, "g_cen.pendingShifted = g_axisActive;") == 1,
              "V8.F125-detail-only-census-keeps-the-axis-bind-key-gated-and-preserves-the-sampled-key-attempt-and-shift-state");
        check(countOf(rf, "axisBegin(ctx, eye, gamesPs, g_trustVerdict)") == 1 && countOf(rf, "if (nativeOnFootActive()) eyeWatchNote();") == 1 &&
                  countOf(rf, "Refusal::kOneEye") >= 2 && countOf(rf, "uiLayerSubmittedTarget(0)") == 1 && countOf(rf, "uiLayerSubmittedTarget(1)") == 1,
              "V8.F121c-the-bind-takes-the-eye-trust-verdict-and-the-tracker-sees-every-visor-draw-on-foot");
        check(countOf(rf, "selfOffVerdict(sl.shifted, c, sl.attempt, g_axisAttempt)") == 1 && countOf(rf, "formatSelfOff(") == 1 && countOf(rf, "g_axisSelfOff.load(") == 1 &&
                  countOf(rf, "g_axisSelfOff.store(false") == 1 && countOf(rf, "sl.shifted = g.pendingShifted;") == 1 &&
                  countOf(rf, "if (g_axisKeyKnown && g_axisKeyLast) ++g_axisAttempt;") == 1 && countOf(rf, "sl.attempt = g_axisAttempt;") == 1,
              "V8.F121c-a-sample-after-a-draw-through-the-replacement-with-a-non-finite-tap-turns-it-off-and-the-key-cycled-turns-it-back");
    }

    // V9: the census shader (F121b), the build's bytecode, against the CPU model and the decode.
    censuschecks::census(rig.dev.Get(), rig.ctx.Get(), edvr::kVisorCensusBytecode, sizeof(edvr::kVisorCensusBytecode), [](bool ok, const char* label) { check(ok, label); });
    check(rig.problems() == 0, "V9.debug-layer-clean");
    if (failures) { std::printf("visor_axis_test: %d FAILED of %u\n", failures, checks); return 1; }
    std::printf("visor_axis_test: PASS V1-V13 (%u checks)\n", checks);
    return 0;
}
