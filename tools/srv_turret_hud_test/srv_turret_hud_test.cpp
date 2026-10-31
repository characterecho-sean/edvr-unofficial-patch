// The SRV turret view (issue 140; src/d3d11/srv_turret*.h, srv_turret.cpp, journal_vehicle_latch.h). The build's own bytecode of the HUD's eight pass-through geometry shaders
// (kSrvTurretHudGsABytecode to kSrvTurretHudGsHBytecode: the build compiles srv_turret_hud_shader.h's HLSL, and this rig checks the bytes are that text's) and the production
// binding, constants and maths run on WARP, with the D3D debug layer when the machine has it, in front of stand-ins for the nine families' vertex and pixel shaders. A family is a
// shader pair (srv_turret_math.h kFamilies: the two hologram families 1989E6D3 and 2CECEC30, the radar-named billboards DF3503CD, 44DDCDEB, 9B34C331, 9611A454 and B932058F, the target
// hologram sphere 5559BD94 with either of its pixel shaders; the flight HUD's vectors B7790CBF are not a family); there is one geometry shader for each distinct vertex-shader OUTPUT
// SIGNATURE (kSignatures, eight). The families are in two GROUPS, each with its own key: the weapon panels (the two hologram families) and the radar (the other seven). Each stand-in has the game's output signature, element for element (read from the fxc disassembly of the dumps' bytecode, the `__USER_...` semantics
// included; reflection compares the two) and the same clip rows (o.x = dp4(cb0[4], r0), o.y = dp4(cb0[5], r0), o.w = dp4(cb0[7], r0)), and the pixel stand-ins read what the game's
// pixel shaders read (some also SV_POSITION.xy). The reticle's sprite (vs E508648660A352B2), at about 500 m, is not one of them, nor are the 5334-index shells (at gigametres) or the
// glare sprite (a constant clip w of 1).
//
//   THE SHIFT (the pull, the first build's forward move: a gap of 0.96 m is what its default was). A quad at view depth w = 1.4 (a deep panel) with a gap of 0.96 m and s = +0.0259
//   (Sean's Pimax: P00 0.940, Elite's eye separation 55.1 mm, the left eye) lands shifted by the formula's amount, measured in pixels from the texture-coordinate gradient
//   (sub-pixel): clip x + s gap / (w - gap). The right eye shifts the other way.
//   THE PUSH (the shipped move: a NEGATIVE gap, window 0.02 to 1.0 m, 1.10 m by default). A quad at w = 0.4 (a weapon panel) lands where the same quad truly moved 1.10 m DEEPER along its
//   ray from the head centre is seen from that eye, shifted by -s d / (w + d) in clip x, its size unchanged; a vertex at 1.0 m or more (the larger panels, 1.4 m) and one at 0.02 m or
//   less is unchanged; the left eye's image shifts the other way from the pull's.
//   THE GEOMETRY. The same shift is what a quad truly moved `gap` metres nearer along its ray from the head centre looks like to each eye, rendered with the plain shader
//   and Elite's off-centre projection: the two images agree to a hundredth of a pixel, the quad's size unchanged (the cyclopean direction is kept).
//   THE WINDOW. A vertex at w = 500 (the reticle), 50, 5 or 4 m, or nearer than gap + 0.10 (1.0 m, and the near layer's 0.42 m), is unchanged by the pull; a window with nothing in it,
//   or s = 0, is a copy; a mixed draw moves only the quad in range. The texture coordinates are untouched (an integer-pixel translation reproduces the plain image shifted). All of it for every signature.
//   THE STRIPS. A quad as a triangle strip (some of the HUD's strokes are drawn as one) is the plain strip bit for bit through a window with nothing in it, under every culling mode (the
//   winding is kept), and lands where the same quad as a list lands, for the pull and for the push.
//   THE LINKAGE. Each shader links against its stand-in under the debug layer with no message, its input is the vertex shader's output element for element and its output the same
//   again, the pixel shader's inputs are a subset of it; a vertex shader without the texture coordinates is reported by the layer (the control that it really checks).
//   THE BINDING (the production class, on a real context): in and out for one draw with the game's geometry stage and constant slot exactly as they were and no reference leaked or
//   kept; a geometry shader of the game's own refused with nothing touched; the restore that fails twice, the settle at the boundary and the state the game rebound; the
//   geometry stage empty after the issue. THE CONSTANTS: made once per device, rewritten only when a number changes, refused when not finite, not a window, a gap of 0 or beyond 2 m;
//   a push (negative gap) is accepted. THE GROUPS: every family has one, the two hologram families are the panels, the other seven the radar; a group's push is used when above 0, else
//   the gap, else the group is off; B7790CBF is no family.
//   THE INSTRUMENT: the pipeline statistics (gs 0 for a plain draw, 2 for a quad through a shader), the stream-output twin of each signature (its record is the vertex whole, from
//   the signature table) and the capture summary: under the pull a quad at 1.4 m reads in_window 6, the near layer's 0.42 m and the reticle's 500 m read in_window 0 with the picture
//   unchanged; under the push a quad at 0.4 m reads in_window 6 with a negative shift and one at 1.4 m reads in_window 0.
//   THE SCORPION LATCH (journal_vehicle_latch.h, no device): LoadGame's Ship and LaunchSRV's SRVType set it for combat_multicrew_srv_01 in any case and clear it for anything
//   else; the main menu's music, DockSRV, SRVDestroyed and Shutdown clear it; a localised name, a missing, escaped, shorter, longer or too distant field is unknown, which is
//   false; a session fed in two pieces cut at every byte and byte by byte gives the same changes; a new file resets it; the state has no room for a line.
//   THE MUTANTS, each run through the same cases (the pull's and the push's) in every signature's text and each caught: no window, the sign, the divisor, the wrong axis, both ends of the window, and a
//   pass-through that reverses the winding (caught by the strip cases); and the binding whose restore does nothing. --wiring scans the sources that connect all this to the game,
//   with controls that edit a copy and must trip the pin.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../src/common/log.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/journal_vehicle_latch.h"
#include "../../src/d3d11/srv_turret_hud_binding.h"
#include "../../src/d3d11/srv_turret_hud_shader.h"
#include "../../src/d3d11/srv_turret_math.h"
#include "temporal_shader_bytecode.h"

using Microsoft::WRL::ComPtr;
namespace st = edvr::srv_turret;
namespace jv = edvr::journal_vehicle;

namespace edvr {
Log& Log::get() {
    static Log log;
    return log;
}
Log::~Log() {}
void Log::note(const char*, ...) {}
void breadcrumb(const char*) {}
}  // namespace edvr

static unsigned g_checks = 0;
// The pull's gap the cases below run with: the first build's default (96 cm), a deep panel's 1.39 m less the near layer's 0.42 m, less a centimetre. The shipped default is 0 now.
static const float kPullGapM = 0.96f;
// The push's: the shipped default of both push keys (110 cm), and a weapon panel's depth (0.27 to 0.70 m measured).
static const float kPushBackM = st::kPushDefaultCm / 100.0f, kPanelW = 0.40f;
static void check(bool ok, const char* why) {
    ++g_checks;
    if (!ok) throw std::runtime_error(why);
}
static void ck(HRESULT h, const char* what = "D3D call") {
    if (FAILED(h)) {
        std::printf("HRESULT=%08lx at %s\n", static_cast<unsigned long>(h), what);
        throw std::runtime_error(what);
    }
}
// A soft check, for the cases a mutant is run through too: it counts what failed instead of throwing.
struct Fails {
    unsigned n = 0;
    std::string first;
    void expect(bool ok, const char* what) {
        ++g_checks;
        if (ok) return;
        if (!n) first = what;
        ++n;
    }
};
static std::string readText(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());  // build.bat is CRLF on disk, the sources LF
    return t;
}

// ---------------------------------------------------------------- the device
static ComPtr<ID3D11Device> g_dev;
static ComPtr<ID3D11DeviceContext> g_ctx;
static ComPtr<ID3D11InfoQueue> g_messages;

static ComPtr<ID3DBlob> compileHlsl(const std::string& source, const char* profile, bool* ok = nullptr) {
    ComPtr<ID3DBlob> code, error;
    const HRESULT h = D3DCompile(source.data(), source.size(), "srv turret rig", nullptr, nullptr, "main", profile, 0, 0, &code, &error);
    if (ok) *ok = SUCCEEDED(h);
    if (FAILED(h)) {
        if (error && !ok) std::puts(static_cast<const char*>(error->GetBufferPointer()));
        if (!ok) ck(h, "D3DCompile");
        return nullptr;
    }
    return code;
}
static ComPtr<ID3D11Buffer> makeBuffer(UINT bytes, UINT bind, const void* data) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.BindFlags = bind;
    const D3D11_SUBRESOURCE_DATA init{data, 0, 0};
    ComPtr<ID3D11Buffer> b;
    ck(g_dev->CreateBuffer(&bd, data ? &init : nullptr, &b), "CreateBuffer");
    return b;
}

// ---------------------------------------------------------------- the game's shader pairs, as stand-ins with their signatures and clip rows
// One stand-in pair for each output signature (srv_turret_math.h kSignatures, the vertex shaders' outputs from the fxc disassembly of the dumps' bytecode): the vertex stand-in
// declares exactly that signature, element for element (the game's `__USER_...` semantics included) and writes o.x = dp4(cb0[4], r0), o.y = dp4(cb0[5], r0), o.w = dp4(cb0[7], r0);
// the pixel stand-in reads every interpolant (the real pixel shaders read a subset of the vertex shader's output, in every pair all of it) and, for the signatures whose pixel
// shader does, SV_POSITION.xy. The first element carries the texture coordinate (u, v), the second the extra interpolant's first value, which the colour shows.
static std::string semanticOf(const st::SigElement& e) { return e.index ? std::string(e.name) + std::to_string(e.index) : std::string(e.name); }
static const char* const kTypeOf[5] = {"", "float", "float2", "float3", "float4"};
static const char* const kSwizzleOf[5] = {"", ".x", ".xy", ".xyz", ".xyzw"};
static std::string standInVs(uint32_t sig) {
    const st::Signature& S = st::kSignatures[sig];
    std::string decl, assign;
    for (uint32_t e = 0; e + 1 < S.count; ++e) {
        decl += std::string("    ") + kTypeOf[S.el[e].comps] + " e" + std::to_string(e) + " : " + semanticOf(S.el[e]) + ";\n";
        assign += "    o.e" + std::to_string(e) + " = i." + (e == 0 ? "t0" : "t1") + kSwizzleOf[S.el[e].comps] + ";\n";
    }
    return "cbuffer C0 : register(b0) { float4 cb0[8]; };\n"
           "struct VIn { float4 p : POSITION; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; };\n"
           "struct VOut {\n" + decl + "    float4 pos : SV_Position;\n};\n"
           "VOut main(VIn i) {\n"
           "    float4 r0 = float4(i.p.xyz, 1.0);\n"
           "    VOut o;\n" + assign +
           "    o.pos.x = dot(cb0[4], r0);\n"
           "    o.pos.y = dot(cb0[5], r0);\n"
           "    o.pos.w = dot(cb0[7], r0);\n"
           "    o.pos.z = abs(o.pos.w);\n"
           "    return o;\n"
           "}\n";
}
// The pixel stage: the texture coordinate as the colour (the sub-pixel measure: red is u, green is v), the second element's first value in blue, alpha 1 (a pixel shader that
// reads SV_POSITION: alpha from SV_POSITION.x).
static std::string standInPs(uint32_t sig) {
    const st::Signature& S = st::kSignatures[sig];
    std::string args;
    for (uint32_t e = 0; e + 1 < S.count; ++e) args += std::string(kTypeOf[S.el[e].comps]) + " e" + std::to_string(e) + " : " + semanticOf(S.el[e]) + ", ";
    if (S.psReadsPosition) args += "float4 sp : SV_Position, ";
    args.resize(args.size() - 2);
    return "float4 main(" + args + ") : SV_Target { return float4(e0.x, e0.y, " + (S.count > 2 ? "e1.x" : "0.0") + ", " + (S.psReadsPosition ? "step(0.0, sp.x)" : "1.0") + "); }\n";
}
// A vertex shader WITHOUT the texture coordinates: the linkage control.
static const char* kBadVs = R"HLSL(
cbuffer C0 : register(b0) { float4 cb0[8]; };
float4 main(float4 p : POSITION) : SV_Position { float4 r0 = float4(p.xyz, 1.0); return float4(dot(cb0[4], r0), dot(cb0[5], r0), abs(dot(cb0[7], r0)), dot(cb0[7], r0)); }
)HLSL";

static ComPtr<ID3DBlob> g_vsCode[st::kSignatureCount], g_psCode[st::kSignatureCount];
static ComPtr<ID3D11VertexShader> g_vs[st::kSignatureCount];
static ComPtr<ID3D11PixelShader> g_ps[st::kSignatureCount];
static ComPtr<ID3D11InputLayout> g_layout;
static ComPtr<ID3D11RasterizerState> g_rsOf[3];   // no culling (the stand-in quads are wound either way, as the game's state does not cull its panels), back-face culling, front-face culling
static int g_fam = 0;                             // the signature the cases are running: render() draws it with that signature's stand-in pair

static void setupGamePair() {
    for (uint32_t f = 0; f < st::kSignatureCount; ++f) {
        g_vsCode[f] = compileHlsl(standInVs(f), "vs_5_0");
        g_psCode[f] = compileHlsl(standInPs(f), "ps_5_0");
        ck(g_dev->CreateVertexShader(g_vsCode[f]->GetBufferPointer(), g_vsCode[f]->GetBufferSize(), nullptr, &g_vs[f]), "CreateVertexShader");
        ck(g_dev->CreatePixelShader(g_psCode[f]->GetBufferPointer(), g_psCode[f]->GetBufferSize(), nullptr, &g_ps[f]), "CreatePixelShader");
    }
    const D3D11_INPUT_ELEMENT_DESC elements[3] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                                  {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                                  {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ck(g_dev->CreateInputLayout(elements, 3, g_vsCode[0]->GetBufferPointer(), g_vsCode[0]->GetBufferSize(), &g_layout), "CreateInputLayout");
    const D3D11_CULL_MODE modes[3] = {D3D11_CULL_NONE, D3D11_CULL_BACK, D3D11_CULL_FRONT};
    for (int i = 0; i < 3; ++i) {
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = modes[i];
        rd.DepthClipEnable = TRUE;
        ck(g_dev->CreateRasterizerState(&rd, &g_rsOf[i]), "CreateRasterizerState");
    }
}

// ---------------------------------------------------------------- one drawn frame
struct Vtx {
    float p[4];
    float t0[4];   // TEXCOORD0: u, v, 0, 1
    float t1[4];   // the extra interpolant (TEXCOORD6 xyz, or TEXCOORD7 xy)
};
struct Frame {
    UINT w = 0, h = 0;
    std::vector<float> rgba;
    const float* at(UINT x, UINT y) const { return rgba.data() + (size_t(y) * w + x) * 4; }
};
struct Target {
    UINT w, h;
    ComPtr<ID3D11Texture2D> rt, stage;
    ComPtr<ID3D11RenderTargetView> rtv;
    Target(UINT width, UINT height) : w(width), h(height) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ck(g_dev->CreateTexture2D(&td, nullptr, &rt), "render target");
        ck(g_dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv), "RTV");
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ck(g_dev->CreateTexture2D(&td, nullptr, &stage), "staging");
    }
    void clear() {
        const float zero[4] = {0, 0, 0, 0};
        g_ctx->ClearRenderTargetView(rtv.Get(), zero);
    }
    Frame read() {
        g_ctx->CopyResource(stage.Get(), rt.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        ck(g_ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "Map the frame");
        Frame f;
        f.w = w;
        f.h = h;
        f.rgba.resize(size_t(w) * h * 4);
        for (UINT y = 0; y < h; ++y) std::memcpy(f.rgba.data() + size_t(y) * w * 4, static_cast<const char*>(m.pData) + size_t(y) * m.RowPitch, size_t(w) * 16);
        g_ctx->Unmap(stage.Get(), 0);
        return f;
    }
};

// Elite's projection for one eye: clip x = P00 X + offX w, clip y = P11 Y + offY w, w = -Z (OpenVR: forward is -z), against the eye-relative position. Rows 4, 5 and 7 of cb0.
struct Cam {
    float p00 = 0.94f, p11 = 0.94f, offX = 0.03f, offY = -0.02f;
};
static ComPtr<ID3D11Buffer> cameraBuffer(const Cam& c) {
    std::vector<float> cb0(8 * 4, 0.0f);
    cb0[4 * 4 + 0] = c.p00;
    cb0[4 * 4 + 2] = -c.offX;
    cb0[5 * 4 + 1] = c.p11;
    cb0[5 * 4 + 2] = -c.offY;
    cb0[7 * 4 + 2] = -1.0f;
    return makeBuffer(UINT(sizeof(float) * cb0.size()), D3D11_BIND_CONSTANT_BUFFER, cb0.data());
}

// A quad (two triangles) of the head frame at view depth w: x in [x0, x1], y in [y0, y1]; seen from an eye `ex` metres off the head centre (the left eye at -ipd/2), with the
// vertices moved by k about the head centre first (k = 1 leaves them; the truly nearer geometry is k = (w - gap) / w).
static void addQuad(std::vector<Vtx>& v, float x0, float x1, float y0, float y1, float w, float ex, float k = 1.0f) {
    auto vert = [&](float x, float y, float u, float t) {
        const float X = x * k - ex, Y = y * k, Z = -w * k;
        v.push_back(Vtx{{X, Y, Z, 1.0f}, {u, t, 0.0f, 1.0f}, {0.25f, 0.5f, 0.75f, 0.0f}});
    };
    vert(x0, y0, 0, 0);
    vert(x1, y0, 1, 0);
    vert(x0, y1, 0, 1);
    vert(x1, y0, 1, 0);
    vert(x1, y1, 1, 1);
    vert(x0, y1, 0, 1);
}

static st::Constants g_constants[2];
static void noopSetGs(ID3D11DeviceContext*, ID3D11GeometryShader*, ID3D11ClassInstance* const*, uint32_t) {}

// What the GPU instrument reads back for a sampled draw: the pipeline statistics, the stream-output statistics and the records the twin streamed out.
struct Sample {
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
    D3D11_QUERY_DATA_SO_STATISTICS so{};
    std::vector<float> records;   // kCaptureBufferBytes / 4 floats
    bool soTargetAfter = true, gsAfter = true;
};
struct DrawOpts {
    int eye = 0;                 // which constants buffer
    bool skipRestore = false;    // MUTANT: a restore that does nothing
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;   // the draw's topology
    int cull = 0;                // 0 no culling, 1 back faces culled, 2 front faces culled
    Sample* sample = nullptr;    // the instrument: queries around the scope (and, with soTarget, the stream-output target of the twin)
    bool soTarget = false;
};
// One frame of `verts` through the stand-in pair and, when gs is given, the production binding with `params` (null gs: the plain shader alone).
static Frame render(const std::vector<Vtx>& verts, const Cam& cam, ID3D11GeometryShader* gs, const st::TurretParams& params, UINT w = 256, UINT h = 256, const DrawOpts& o = DrawOpts{}) {
    auto vb = makeBuffer(UINT(verts.size() * sizeof(Vtx)), D3D11_BIND_VERTEX_BUFFER, verts.data());
    auto cb0 = cameraBuffer(cam);
    Target t(w, h);
    g_ctx->ClearState();
    g_ctx->OMSetRenderTargets(1, t.rtv.GetAddressOf(), nullptr);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_rsOf[o.cull].Get());
    const UINT stride = sizeof(Vtx), offset = 0;
    ID3D11Buffer* vbs[1] = {vb.Get()};
    g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    g_ctx->IASetInputLayout(g_layout.Get());
    g_ctx->IASetPrimitiveTopology(o.topology);
    g_ctx->VSSetShader(g_vs[g_fam].Get(), nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, cb0.GetAddressOf());
    g_ctx->PSSetShader(g_ps[g_fam].Get(), nullptr, 0);
    t.clear();
    st::Binding binding;
    bool bound = false;
    if (gs) {
        ID3D11Buffer* cb = g_constants[o.eye].get(g_ctx.Get(), params);
        check(cb != nullptr, "the turret constants are made");
        bound = binding.begin(g_ctx.Get(), gs, cb) == st::Binding::Begin::kBound;
        check(bound, "the production binding binds the shader");
    }
    ComPtr<ID3D11Query> statsQ, soQ;
    ComPtr<ID3D11Buffer> soBuf, staging;
    if (o.sample) {   // Begin right after the bind
        const D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS, 0}, sd{D3D11_QUERY_SO_STATISTICS, 0};
        ck(g_dev->CreateQuery(&qd, &statsQ), "the pipeline-statistics query");
        if (o.soTarget) {
            ck(g_dev->CreateQuery(&sd, &soQ), "the stream-output-statistics query");
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = st::kCaptureBufferBytes;
            bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
            ck(g_dev->CreateBuffer(&bd, nullptr, &soBuf), "the stream-output buffer");
            bd.BindFlags = 0;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ck(g_dev->CreateBuffer(&bd, nullptr, &staging), "the staging buffer");
            ID3D11Buffer* target = soBuf.Get();
            const UINT zero = 0;
            g_ctx->SOSetTargets(1, &target, &zero);
        }
        g_ctx->Begin(statsQ.Get());
        if (o.soTarget) g_ctx->Begin(soQ.Get());
    }
    g_ctx->Draw(UINT(verts.size()), 0);
    if (o.sample) {   // End right before the restore
        g_ctx->End(statsQ.Get());
        if (o.soTarget) {
            g_ctx->End(soQ.Get());
            g_ctx->SOSetTargets(0, nullptr, nullptr);
        }
    }
    if (bound) binding.finish(g_ctx.Get(), o.skipRestore ? noopSetGs : st::directSetGs);
    if (o.sample) {
        g_ctx->Flush();
        for (int i = 0; i < 2000 && g_ctx->GetData(statsQ.Get(), &o.sample->stats, sizeof(o.sample->stats), 0) != S_OK; ++i) Sleep(1);
        check(g_ctx->GetData(statsQ.Get(), &o.sample->stats, sizeof(o.sample->stats), 0) == S_OK, "the pipeline statistics come back");
        if (o.soTarget) {
            for (int i = 0; i < 2000 && g_ctx->GetData(soQ.Get(), &o.sample->so, sizeof(o.sample->so), 0) != S_OK; ++i) Sleep(1);
            check(g_ctx->GetData(soQ.Get(), &o.sample->so, sizeof(o.sample->so), 0) == S_OK, "the stream-output statistics come back");
            g_ctx->CopyResource(staging.Get(), soBuf.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            ck(g_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "Map the stream-output copy");
            o.sample->records.resize(st::kCaptureBufferBytes / 4);
            std::memcpy(o.sample->records.data(), m.pData, st::kCaptureBufferBytes);
            g_ctx->Unmap(staging.Get(), 0);
        }
        ComPtr<ID3D11GeometryShader> now;
        g_ctx->GSGetShader(&now, nullptr, nullptr);
        o.sample->gsAfter = now != nullptr;
        ID3D11Buffer* so[1] = {};
        g_ctx->SOGetTargets(1, so);
        o.sample->soTargetAfter = so[0] != nullptr;
        if (so[0]) so[0]->Release();
    }
    return t.read();
}

// ---------------------------------------------------------------- the quad's place in pixels, from the texture-coordinate gradient (sub-pixel)
struct Edges {
    bool ok = false;
    double xl = 0, xr = 0;   // where u = 0 and u = 1 lie on the row, in pixels
};
static Edges edgesOnRow(const Frame& f, UINT y) {
    // linear regression of u (the red channel) on the pixel centre over the covered pixels of the row
    double n = 0, sx = 0, su = 0, sxx = 0, sxu = 0;
    for (UINT x = 0; x < f.w; ++x) {
        const float* p = f.at(x, y);
        if (p[3] < 0.5f) continue;
        const double xc = x + 0.5, u = p[0];
        ++n;
        sx += xc;
        su += u;
        sxx += xc * xc;
        sxu += xc * u;
    }
    Edges e;
    if (n < 8) return e;
    const double den = n * sxx - sx * sx;
    if (std::fabs(den) < 1e-9) return e;
    const double m = (n * sxu - sx * su) / den, b = (su - m * sx) / n;
    if (!(m > 1e-6)) return e;
    e.ok = true;
    e.xl = -b / m;
    e.xr = (1.0 - b) / m;
    return e;
}
static double maxDiff(const Frame& a, const Frame& b) {
    double worst = 0;
    for (size_t i = 0; i < a.rgba.size() && i < b.rgba.size(); ++i) worst = std::max(worst, std::fabs(double(a.rgba[i]) - b.rgba[i]));
    return a.rgba.size() == b.rgba.size() ? worst : 1e9;
}
static double covered(const Frame& f) {
    double n = 0;
    for (size_t i = 3; i < f.rgba.size(); i += 4) n += f.rgba[i] > 0.5f ? 1 : 0;
    return n;
}

// ---------------------------------------------------------------- the maths (no device)
static void mathCases() {
    check(st::turretViewFromFlags(st::kFlagsTurretView | st::kFlagsInSrv) && st::turretViewFromFlags(0x2000u | (1u << 26) | 0x1u), "Flags bit 13 in an SRV (bit 26) is the turret view");
    check(!st::turretViewFromFlags(st::kFlagsTurretView) && !st::turretViewFromFlags(st::kFlagsInSrv) && !st::turretViewFromFlags(0) && !st::turretViewFromFlags(1u << 24),
          "bit 13 outside an SRV, an SRV without it, and a main ship are no turret view");
    check(st::kFlagsTurretView == 0x2000u && st::kFlagsInSrv == 0x4000000u && st::kSeatKindSrv == 4u, "the bits are Frontier's: 13 is 0x2000, the SRV seat bit is 26 (journal seat kind 0x4)");
    const double zOff = st::backSeatedZ(false, 60), zZero = st::backSeatedZ(true, 0), zNeg = st::backSeatedZ(true, -5);
    check(zOff == 0.0 && !std::signbit(zOff) && zZero == 0.0 && !std::signbit(zZero) && zNeg == 0.0, "outside the turret view, or with the key at 0, the camera term is exactly +0.0");
    check(st::backSeatedZ(true, 60) == -0.6 && st::backSeatedZ(true, 5) == -0.05 && st::backSeatedZ(true, 150) == -1.5 && st::backSeatedZ(true, 999) == -1.5,
          "D centimetres back is the seated z = -D metres (+z is backward, the view moves by minus the published value), capped at the key's top");
    const float tan[4] = {-1.0638f, 1.0638f, -1.1f, 1.1f};
    check(std::fabs(st::p00OfTangents(tan) - 0.94f) < 0.0005f, "P00 = 2 / (tanRight - tanLeft): Sean's Pimax 0.940");
    const float asym[4] = {-1.2f, 0.7f, -0.9f, 0.9f};
    check(std::fabs(st::p00OfTangents(asym) - 2.0f / 1.9f) < 1e-5f, "...for an off-centre frustum too");
    const float flat[4] = {0.5f, 0.5f, -1, 1}, nan[4] = {NAN, 1, -1, 1};
    check(st::p00OfTangents(flat) == 0.0f && st::p00OfTangents(nan) == 0.0f && st::p00OfTangents(nullptr) == 0.0f, "tangents that are no frustum give no P00");
    const float ipd = 0.0551f;
    check(st::eyeHx(0, ipd) > 0.0f && st::eyeHx(1, ipd) < 0.0f && st::eyeHx(0, ipd) == -st::eyeHx(1, ipd) && std::fabs(st::eyeHx(0, ipd) - 0.02755f) < 1e-6f,
          "hx is +ipd/2 for the left eye (index 0) and -ipd/2 for the right");
    const float sL = 0.94f * st::eyeHx(0, ipd), sR = 0.94f * st::eyeHx(1, ipd);
    check(std::fabs(sL - 0.0259f) < 0.0001f && std::fabs(sR + 0.0259f) < 0.0001f, "s = P00 hx: +0.0259 for the left eye, -0.0259 for the right (P00 0.940, ipd 55.1 mm)");
    const st::TurretParams p = st::paramsFor(sL, 1.02f);
    check(p.s == sL && p.gap == 1.02f && std::fabs(p.wMin - 1.12f) < 1e-6f && p.wMax == 4.0f, "the window is gap + 0.10 to 4.0");
    check(std::fabs(st::clipXShift(p, 1.5f) - sL * 1.02f / 0.48f) < 1e-6f && std::fabs(st::clipXShift(p, 1.5f) - 0.05504f) < 0.0001f, "w = 1.5: s gap / (w - gap)");
    check(st::clipXShift(p, 50.0f) == 0.0f && st::clipXShift(p, 5.0f) == 0.0f && st::clipXShift(p, 4.0f) == 0.0f && st::clipXShift(p, 1.0f) == 0.0f && st::clipXShift(p, 1.12f) == 0.0f,
          "w = 50, 5 and 4 (the far end), 1.0 and 1.12 (the near end, exactly) are outside the window");
    check(st::clipXShift(st::paramsFor(sL, 0.0f), 1.5f) == 0.0f, "with the gap at 0 the shift is exactly 0");
    {   // the push: a negative gap, window 0.02 to 1.0 m
        const st::TurretParams q = st::pushParamsFor(sL, 1.10f);
        check(q.s == sL && q.gap == -1.10f && q.wMin == st::kPushNearM && q.wMax == st::kPushSplitM && st::kPushNearM == 0.02f && st::kPushSplitM == 1.0f && q.wMin > q.gap && q.wMax > q.wMin,
              "the push of 1.10 m is the constants {s, -1.10, 0.02, 1.0}: a window above the (negative) gap");
        // the analytic shift of moving a point from w to w + d along its ray from the head centre, its angular size unchanged: an eye hx off the centre sees x at
        // ndc = P00 (X + hx) / w, and after the move (X k + hx) / (w k), k = (w + d) / w; the clip x it needs with w kept is the difference times w. The pull is the same with d < 0.
        bool analytic = true, outside = true;
        for (const float d : {0.05f, 0.5f, 1.10f, 2.0f})
            for (const float w : {0.03f, 0.11f, 0.27f, 0.4f, 0.7f, 0.99f}) {
                const st::TurretParams pp = st::pushParamsFor(sL, d);
                const double expect = double(0.94f * st::eyeHx(0, ipd)) * (1.0 / (double(w) + d) - 1.0 / double(w)) * double(w);   // P00 hx (1 / (w + d) - 1 / w) w
                analytic = analytic && std::fabs(double(st::clipXShift(pp, w)) - expect) < 2e-6 && st::clipXShift(pp, w) < 0.0f &&
                           std::fabs(double(st::clipXShift(pp, w)) + double(sL) * d / (w + d)) < 2e-6;
                for (const float deeper : {1.0f, 1.3f, 1.4f, 2.8f, 4.0f, 500.0f}) outside = outside && st::clipXShift(pp, deeper) == 0.0f;
                for (const float nearer : {0.02f, 0.01f, 0.0f, -1.0f}) outside = outside && st::clipXShift(pp, nearer) == 0.0f;
            }
        check(analytic, "the push's clip x shift is -s d / (w + d): the analytic shift of moving a point from w to w + d along its head-centre ray at unchanged angular size (the pull's, sign flipped)");
        check(outside, "...and 0 outside (0.02, 1.0): at 0.02 and below (and at or behind the eye plane), at 1.0, the larger panels' 1.3 to 2.8 m, 4 m and the reticle's 500 m");
        check(std::fabs(st::clipXShift(q, 0.4f) + sL * 1.10f / 1.5f) < 1e-6f && std::fabs(st::clipXShift(q, 0.4f) + 0.019f) < 0.0001f && st::clipXShift(st::pushParamsFor(-sL, 1.10f), 0.4f) > 0.0f,
              "w = 0.4 with the default push: -s d / (w + d) = -0.0190 for the left eye, +0.0190 for the right (the opposite of the pull's)");
        check(st::clipXShift(st::pushParamsFor(0.0f, 1.10f), 0.4f) == 0.0f, "with s = 0 (the cyclopean eye) the push moves nothing");
        // the push the pass would bind is one the constants accept (the module never binds a gap of 0)
        check(std::isfinite(q.gap) && q.gap != 0.0f && std::fabs(q.gap) <= st::kGapLimitM && q.wMin > q.gap && q.wMax > q.wMin && std::fabs(st::pushParamsFor(sL, st::kPushMaxCm / 100.0f).gap) <= st::kGapLimitM,
              "pushParamsFor passes the binding's validity test (finite, gap != 0, |gap| <= 2 m, wMin above the gap, wMax above wMin), at the key's top too");
    }
    {   // which constants a group's draws run with: the push when above 0, else the pull when the gap is above 0, else none
        st::TurretParams out{};
        check(st::paramsForGroup(sL, 110, 0, out) && out == st::pushParamsFor(sL, 1.10f), "a push above 0 and the gap at 0: the push");
        check(st::paramsForGroup(sL, 110, 96, out) && out == st::pushParamsFor(sL, 1.10f), "...and with the gap above 0 too the push is used, not the pull");
        check(st::paramsForGroup(sL, 0, 96, out) && out == st::paramsFor(sL, 0.96f), "no push and the gap above 0: the pull");
        check(!st::paramsForGroup(sL, 0, 0, out), "no push and no gap: no constants (the draw is the game's own)");
        check(st::paramsForGroup(sL, 999, 0, out) && out == st::pushParamsFor(sL, 2.0f) && st::paramsForGroup(sL, 0, 999, out) && out == st::paramsFor(sL, 2.0f), "a key past its range is held to 200 cm");
        check(st::pushCmOf(st::Group::kPanels, 11, 22) == 11 && st::pushCmOf(st::Group::kRadar, 11, 22) == 22, "a group reads its own push key");
        check(!std::strcmp(st::groupName(st::Group::kPanels), "panels") && !std::strcmp(st::groupName(st::Group::kRadar), "radar"), "the groups are named panels and radar");
    }
    check(st::ipdUsable(0.0551f) && st::ipdUsable(0.0632f * 1.5f) && !st::ipdUsable(0.0f) && !st::ipdUsable(NAN) && !st::ipdUsable(0.5f) && st::p00Usable(0.94f) && !st::p00Usable(0.0f) && !st::p00Usable(INFINITY),
          "the separation and P00 are used only when they are a human head and a projection");
    check(static_cast<int>(st::Refuse::kCount) == 5 && std::strcmp(st::refuseName(st::Refuse::kNoEye), "no-eye") == 0 && std::strcmp(st::refuseName(st::Refuse::kGameGs), "game-gs") == 0 &&
              std::strcmp(st::refuseName(st::Refuse::kTopology), "topology") == 0 && std::strcmp(st::refuseName(st::Refuse::kNotReady), "not-ready") == 0 &&
              std::strcmp(st::refuseName(st::Refuse::kNoGeometry), "no-geometry") == 0,
          "five refusal reasons, in the order the 10 s line prints them");
    // the families: the pairs, their records, and that the reticle's sprite and the excluded families are not among them
    check(st::kFamilyCount == 9 && st::kSignatureCount == 8, "nine families (shader pairs) over eight output signatures");
    struct Pair {
        uint64_t vs, ps;
        const char* name;
        uint32_t sig;
    };
    const Pair pairs[9] = {
        {0x1989E6D3B405FDE0ull, 0xEAB8A1C95A13FFBEull, "1989E6D3", 0},         {0x2CECEC3065EF0D4Aull, 0xA4D03619D631B186ull, "2CECEC30", 1},
        {0xDF3503CD07F9B10Cull, 0x8C08EB252B0F6095ull, "DF3503CD", 2},         {0x44DDCDEBAEA137D8ull, 0xCB53257EFE8B1696ull, "44DDCDEB", 3},
        {0x9B34C331902DC1EDull, 0x3B3433E4FEBBC37Bull, "9B34C331", 4},         {0x9611A454527F7FEBull, 0x1E1C49DC51C0E509ull, "9611A454", 5},
        {0xB932058F26B76691ull, 0x65861AC394D51526ull, "B932058F", 6},         {0x5559BD94B6852E83ull, 0xEA02FAC2BD6C643Cull, "5559BD94.EA02FAC2", 7},
        {0x5559BD94B6852E83ull, 0xE95634B0F61D218Full, "5559BD94.E95634B0", 7},
    };
    bool pairsOk = true, crossOk = true;
    for (uint32_t i = 0; i < 9; ++i) {
        pairsOk = pairsOk && st::kFamilies[i].vs == pairs[i].vs && st::kFamilies[i].ps == pairs[i].ps && !std::strcmp(st::kFamilies[i].name, pairs[i].name) &&
                  st::kFamilies[i].sig == pairs[i].sig && st::familyOf(pairs[i].vs, pairs[i].ps) == int(i) && st::isFamilyVs(pairs[i].vs);
        for (uint32_t j = 0; j < 9; ++j)
            if (i != j && pairs[i].vs != pairs[j].vs) crossOk = crossOk && st::familyOf(pairs[i].vs, pairs[j].ps) == -1;
    }
    check(pairsOk, "the nine families: the shader pairs of the dumps, in the table's order, each found by BOTH of its shaders, its vertex shader found by isFamilyVs");
    check(crossOk && st::familyOf(pairs[7].vs, pairs[8].ps) == 8 && st::familyOf(pairs[8].vs, pairs[7].ps) == 7 && st::familyOf(0, 0) == -1,
          "a draw is a family's only when BOTH shaders of the pair match (a vertex shader with another family's pixel shader is none; the sphere's two pixel shaders are two families)");
    check(st::familyOf(0xE508648660A352B2ull, 0x63ABD86359B57D01ull) == -1 && !st::isFamilyVs(0xE508648660A352B2ull),
          "the reticle's sprite (vs E508648660A352B2, ps 63ABD86359B57D01) is not a family: it is at about 500 m and is left alone");
    check(!st::isFamilyVs(0xB12F7A618E1BDE98ull) && !st::isFamilyVs(0x203DF51758AADC4Dull) && !st::isFamilyVs(0x94D5C556DFD6D705ull) && st::familyOf(0xB12F7A618E1BDE98ull, 0x42AC0CACC9CDF72Bull) == -1 &&
              st::familyOf(0x94D5C556DFD6D705ull, 0x912477AEF6958379ull) == -1,
          "the excluded families (the 5334-index shells B12F7A61 and 203DF517 at gigametres, the glare sprite 94D5C556 with a constant clip w of 1) are not in the table");
    {   // the flight HUD's vectors (vs B7790CBFC6554097, ps 8DEF46452FA459F5; clip w -658 to +3029 m in the flight of 9fdad3a1) are no family, no signature, and no name in the tables
        bool gone = st::familyOf(0xB7790CBFC6554097ull, 0x8DEF46452FA459F5ull) == -1 && !st::isFamilyVs(0xB7790CBFC6554097ull);
        for (uint32_t i = 0; i < st::kFamilyCount; ++i) gone = gone && std::strcmp(st::kFamilies[i].name, "B7790CBF") != 0 && st::kFamilies[i].vs != 0xB7790CBFC6554097ull && st::kFamilies[i].sig < st::kSignatureCount;
        for (uint32_t s = 0; s < st::kSignatureCount; ++s) gone = gone && std::strcmp(st::kSignatures[s].name, "B7790CBF") != 0 && st::kSignatures[s].count <= 4;
        check(gone, "the flight HUD's vectors (B7790CBF) are out of the pass: no family, no signature, no pixel-shader pairing");
    }
    {   // the groups: every family has one, the two hologram families are the panels, the rest the radar
        bool ok = true;
        uint32_t panels = 0, radar = 0;
        for (uint32_t i = 0; i < st::kFamilyCount; ++i) {
            const st::Group g = st::kFamilies[i].group;
            ok = ok && (g == st::Group::kPanels || g == st::Group::kRadar);
            panels += g == st::Group::kPanels ? 1 : 0;
            radar += g == st::Group::kRadar ? 1 : 0;
            ok = ok && ((i < 2) == (g == st::Group::kPanels)) && !std::strcmp(st::kFamilies[i].name, pairs[i].name);
        }
        ok = ok && panels == 2 && radar == 7 && st::kGroupCount == 2 && st::kFamilies[0].vs == 0x1989E6D3B405FDE0ull && st::kFamilies[1].vs == 0x2CECEC3065EF0D4Aull;
        check(ok, "every family has a group: 1989E6D3 and 2CECEC30 are the panels, the other seven (DF3503CD, 44DDCDEB, 9B34C331, 9611A454, B932058F and the sphere's two) the radar");
    }
    {   // the stream-output records: every family's equals its signature's
        const uint32_t expectBytes[8] = {44, 40, 24, 52, 44, 36, 28, 36}, expectPos[8] = {7, 6, 2, 9, 7, 5, 3, 5};
        bool ok = true;
        for (uint32_t s = 0; s < 8; ++s) ok = ok && st::sigFloats(s) * 4 == expectBytes[s] && st::sigPosFloat(s) == expectPos[s] && st::sigPosFloat(s) * 4 + 16 == expectBytes[s];
        for (uint32_t i = 0; i < 9; ++i) ok = ok && st::kFamilies[i].strideBytes == expectBytes[st::kFamilies[i].sig] && st::kFamilies[i].posFloat == expectPos[st::kFamilies[i].sig];
        check(ok, "the stream-output records: each signature's floats in order, the clip position last (44, 40, 24, 52, 44, 36, 28 and 36 bytes), and each family's are its signature's");
    }
    {   // the topologies: lists and strips, nothing else
        static_assert(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST == 4 && D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP == 5, "the topology constants srv_turret_math.h carries are D3D11's");
        bool ok = st::topologyCarried(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) && st::topologyCarried(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        for (int t : {int(D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED), int(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST), int(D3D11_PRIMITIVE_TOPOLOGY_LINELIST), int(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP),
                      int(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ), int(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ), int(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST), -1, 99})
            ok = ok && !st::topologyCarried(t);
        check(ok, "triangle lists and triangle strips are carried; points, lines, adjacency, patches and anything else are refused");
    }
    // the measured depths: the deep panels at 1.39 m move at the default gap, the near layer at 0.41 to 0.47 m and the reticle at 500 m do not
    const st::TurretParams d = st::paramsFor(sL, kPullGapM);
    check(st::kGapDefaultCm == 0 && st::kPushDefaultCm == 110 && st::kPushMaxCm == 200 && st::kGapMaxCm == 200 && std::fabs(d.wMin - 1.06f) < 1e-6f && st::clipXShift(d, 1.39f) > 0.05f && st::clipXShift(d, 1.4f) > 0.0f && st::clipXShift(d, 0.42f) == 0.0f &&
              st::clipXShift(d, 0.41f) == 0.0f && st::clipXShift(d, 0.47f) == 0.0f && st::clipXShift(d, 500.0f) == 0.0f,
          "at a pull of 96 cm (the first build's default; the shipped default is 0, the push keys' 110): the deep band is 1.06 to 4.0 m; w = 1.39 moves, w = 0.41 to 0.47 (the near layer) and 500 m (the reticle) do not");
    {   // the measured depths under the push: the weapon panels (0.27 to 0.70 m) and the radar (0.11 to 0.44 m) move, the larger panels (1.3 to 2.8 m) and the reticle do not
        const st::TurretParams q = st::pushParamsFor(sL, kPushBackM);
        bool ok = true;
        for (const float w : {0.11f, 0.27f, 0.32f, 0.34f, 0.42f, 0.44f, 0.65f, 0.70f, 0.85f}) ok = ok && st::clipXShift(q, w) < -0.005f;
        for (const float w : {1.3f, 1.39f, 1.41f, 1.5f, 1.7f, 2.8f, 500.0f}) ok = ok && st::clipXShift(q, w) == 0.0f;
        check(ok, "at the default push (110 cm): the radar (0.11 to 0.44 m) and the weapon panels (0.27 to 0.70 m) move, the larger panels (1.3 to 2.8 m) and the reticle (500 m) do not");
        check(std::fabs((0.40f + kPushBackM) - 1.50f) < 1e-5f && std::fabs((0.30f + kPushBackM) - 1.40f) < 1e-5f, "...and a panel at 0.30 to 0.40 m lands at 1.40 to 1.50 m, where the larger panels are");
    }
    check(std::fabs((1.39f - d.gap) - 0.43f) < 1e-5f, "...and the panel lands at 0.43 m, where the near layer is");
}

// ---------------------------------------------------------------- the geometry, run on the production shader and on each mutant
static void geometryCases(ID3D11GeometryShader* gs, Fails& f) {
    const Cam cam;
    const float ipd = 0.0551f, gap = kPullGapM;   // the pull's gap (the first build's default), and a deep panel's depth
    const float w0 = 1.4f, qx0 = -0.30f, qx1 = 0.30f, qy0 = -0.20f, qy1 = 0.20f;
    for (int eye = 0; eye < 2; ++eye) {
        const float hx = st::eyeHx(eye, ipd), ex = -hx;   // the eye's place against the head centre
        const float s = cam.p00 * hx;
        const st::TurretParams params = st::paramsFor(s, gap);
        DrawOpts o;
        o.eye = eye;
        std::vector<Vtx> nearQuad, truth;
        addQuad(nearQuad, qx0, qx1, qy0, qy1, w0, ex);
        addQuad(truth, qx0, qx1, qy0, qy1, w0, ex, (w0 - gap) / w0);
        const Frame plain = render(nearQuad, cam, nullptr, params);
        const Frame moved = render(nearQuad, cam, gs, params, 256, 256, o);
        const Frame real = render(truth, cam, nullptr, params);
        const Edges ep = edgesOnRow(plain, 128), em = edgesOnRow(moved, 128), er = edgesOnRow(real, 128);
        f.expect(ep.ok && em.ok && er.ok, "the quad's edges are found in all three images");
        const double expectPx = double(st::clipXShift(params, w0)) / w0 * 128.0;   // ndc * (width / 2)
        f.expect(std::fabs(expectPx) > 3.0 && std::fabs(expectPx) < 8.0, "the case is a shift of several pixels");
        f.expect((eye == 0) == (em.xl > ep.xl), "the left eye's image shifts towards the right, the right eye's towards the left");
        f.expect(std::fabs((em.xl - ep.xl) - expectPx) < 0.03 && std::fabs((em.xr - ep.xr) - expectPx) < 0.03,
                 "a quad at w = 1.4 with the pull (96 cm) lands shifted by s gap / (w - gap) / w half-widths of the target: the formula's pixels");
        f.expect(std::fabs((em.xr - em.xl) - (ep.xr - ep.xl)) < 0.03, "...with its size unchanged (a translation: the cyclopean direction is kept)");
        f.expect(std::fabs(em.xl - er.xl) < 0.03 && std::fabs(em.xr - er.xr) < 0.03 && std::fabs((er.xr - er.xl) - (ep.xr - ep.xl)) < 0.03,
                 "...and it is where the geometry truly moved `gap` nearer along its ray from the head centre is seen from that eye, with Elite's off-centre projection");
        // the rows: y is not moved
        const Edges emTop = edgesOnRow(moved, 120), epTop = edgesOnRow(plain, 120);
        f.expect(emTop.ok && epTop.ok && std::fabs((emTop.xl - epTop.xl) - expectPx) < 0.03, "the shift is the same on every row (the quad is flat at one depth)");
        f.expect(covered(moved) > 1500 && std::fabs(covered(moved) - covered(plain)) < 40, "the quad covers as many pixels as before");
        // y, z, w and the texture coordinates are untouched: the shifted image is the plain one translated, row for row
        double yWorst = 0;
        for (UINT y = 0; y < 256; ++y) {
            const bool a = plain.at(128, y)[3] > 0.5f, b = moved.at(128, y)[3] > 0.5f;
            yWorst += a != b ? 1 : 0;
        }
        f.expect(yWorst <= 1, "the same rows are covered (y is untouched)");
    }
    {   // the window: far, reticle-far, near, the gap at 0, a mixed draw
        const float hx = st::eyeHx(0, ipd), ex = -hx;
        const st::TurretParams loud = st::paramsFor(3.0f, gap);   // a big s, so a vertex wrongly moved at w = 50 would show
        for (float w : {500.0f, 50.0f, 5.0f, 4.0f, 1.0f, 0.42f}) {
            std::vector<Vtx> v;
            addQuad(v, -0.3f * w / 1.5f, 0.3f * w / 1.5f, -0.2f * w / 1.5f, 0.2f * w / 1.5f, w, ex);   // the same size on the screen at any depth
            const Frame plain = render(v, cam, nullptr, loud);
            const Frame through = render(v, cam, gs, loud);
            char what[160];
            std::snprintf(what, sizeof(what), "a quad at w = %.2f is outside the window (%.2f to 4.00) and the image is the plain one, bit for bit", double(w), double(loud.wMin));
            f.expect(covered(plain) > 1000 && maxDiff(plain, through) == 0.0, what);
        }
        std::vector<Vtx> v;
        addQuad(v, -0.3f, 0.3f, -0.2f, 0.2f, w0, ex);
        const st::TurretParams empty{0.0259f, gap, 100.0f, 200.0f};   // a valid window with nothing in it (the constants refuse a gap of 0: the module never binds one)
        const Frame plain = render(v, cam, nullptr, empty);
        const Frame zero = render(v, cam, gs, empty);
        f.expect(covered(plain) > 1000 && maxDiff(plain, zero) == 0.0, "with a window nothing lies in the image is the plain one, bit for bit");
        const Frame zeroS = render(v, cam, gs, st::paramsFor(0.0f, gap));
        f.expect(maxDiff(plain, zeroS) == 0.0, "...and with s = 0 (the cyclopean eye) too");
        // a mixed draw: the near quad in the window moves, the far one (the reticle) does not
        std::vector<Vtx> mixed, nearOnly, farOnly;
        addQuad(mixed, -0.3f, 0.3f, 0.05f, 0.25f, w0, ex);
        addQuad(mixed, -0.3f, 0.3f, -0.45f, -0.25f, 6.0f, ex);
        addQuad(nearOnly, -0.3f, 0.3f, 0.05f, 0.25f, w0, ex);
        addQuad(farOnly, -0.3f, 0.3f, -0.45f, -0.25f, 6.0f, ex);
        const st::TurretParams p = st::paramsFor(0.0259f, gap);
        const Frame mixedMoved = render(mixed, cam, gs, p);
        const Frame nearMoved = render(nearOnly, cam, gs, p);
        const Frame farPlain = render(farOnly, cam, nullptr, p);
        const Frame nearPlain = render(nearOnly, cam, nullptr, p);
        double worst = 0;
        for (UINT y = 0; y < 256; ++y)
            for (UINT x = 0; x < 256; ++x)
                for (int c = 0; c < 4; ++c) {
                    const float expected = std::max(nearMoved.at(x, y)[c], farPlain.at(x, y)[c]);
                    worst = std::max(worst, std::fabs(double(mixedMoved.at(x, y)[c]) - expected));
                }
        f.expect(worst == 0.0 && maxDiff(nearMoved, nearPlain) > 0.05, "a draw of two quads moves the one in range and leaves the other: the rule is per vertex");
    }
    {   // the texture coordinates are untouched: an integer-pixel translation reproduces the plain image shifted
        const float ex = 0.0f;
        const float k = 5.0f;   // pixels, to the right
        const float s = k * 2.0f * w0 * (w0 - gap) / (256.0f * gap);
        std::vector<Vtx> v;
        addQuad(v, -0.3f, 0.3f, -0.2f, 0.2f, w0, ex);
        Cam centred;
        centred.offX = centred.offY = 0.0f;
        const Frame plain = render(v, centred, nullptr, st::paramsFor(s, gap));
        const Frame moved = render(v, centred, gs, st::paramsFor(s, gap));
        double worst = 0;
        for (UINT y = 0; y < 256; ++y)
            for (UINT x = 8; x + 8 < 256; ++x)
                for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(double(moved.at(x + 5, y)[c]) - plain.at(x, y)[c]));
        f.expect(worst < 2e-3, "the texture coordinates and the pixel stage are untouched: a 5 px translation reproduces the plain image 5 px along");
    }
}

// ---------------------------------------------------------------- the push-back: a negative gap, window 0.02 to 1.0 m
// The shipped move, through the same shader and the same constants buffer with a gap of -d: a vertex nearer than 1 m goes d metres DEEPER along its ray from the head centre (its size on
// the screen kept), and a vertex at 1 m or deeper (the larger panels, 1.3 m and more; the reticle) or at 0.02 m or nearer stays. A weapon panel is at about 0.4 m.
static void pushCases(ID3D11GeometryShader* gs, Fails& f) {
    const Cam cam;
    const float ipd = 0.0551f, back = kPushBackM, w0 = kPanelW;
    const float sc = w0 / 1.5f;   // the same size on the screen as the pull's quad at 1.5 m
    const float qx0 = -0.30f * sc, qx1 = 0.30f * sc, qy0 = -0.20f * sc, qy1 = 0.20f * sc;
    for (int eye = 0; eye < 2; ++eye) {
        const float hx = st::eyeHx(eye, ipd), ex = -hx;   // the eye's place against the head centre
        const float s = cam.p00 * hx;
        const st::TurretParams params = st::pushParamsFor(s, back);
        DrawOpts o;
        o.eye = eye;
        std::vector<Vtx> panel, truth;
        addQuad(panel, qx0, qx1, qy0, qy1, w0, ex);
        addQuad(truth, qx0, qx1, qy0, qy1, w0, ex, (w0 + back) / w0);   // the truly deeper geometry: k = (w + d) / w
        const Frame plain = render(panel, cam, nullptr, params);
        const Frame moved = render(panel, cam, gs, params, 256, 256, o);
        const Frame real = render(truth, cam, nullptr, params);
        const Edges ep = edgesOnRow(plain, 128), em = edgesOnRow(moved, 128), er = edgesOnRow(real, 128);
        f.expect(ep.ok && em.ok && er.ok, "push: the quad's edges are found in all three images");
        const double expectPx = double(st::clipXShift(params, w0)) / w0 * 128.0;   // ndc * (width / 2)
        f.expect(std::fabs(expectPx) > 3.0 && std::fabs(expectPx) < 8.0, "push: the case is a shift of several pixels");
        f.expect((eye == 0) == (em.xl < ep.xl), "push: the left eye's image shifts towards the left, the right eye's towards the right (the pull's opposite)");
        f.expect(std::fabs((em.xl - ep.xl) - expectPx) < 0.03 && std::fabs((em.xr - ep.xr) - expectPx) < 0.03,
                 "push: a quad at w = 0.4 with the default push (110 cm) lands shifted by -s d / (w + d) / w half-widths of the target: the formula's pixels");
        f.expect(std::fabs((em.xr - em.xl) - (ep.xr - ep.xl)) < 0.03, "push: ...with its size unchanged (a translation: the cyclopean direction is kept)");
        f.expect(std::fabs(em.xl - er.xl) < 0.03 && std::fabs(em.xr - er.xr) < 0.03 && std::fabs((er.xr - er.xl) - (ep.xr - ep.xl)) < 0.03,
                 "push: ...and it is where the geometry truly moved `d` DEEPER along its ray from the head centre is seen from that eye, with Elite's off-centre projection");
        const Edges emTop = edgesOnRow(moved, 120), epTop = edgesOnRow(plain, 120);
        f.expect(emTop.ok && epTop.ok && std::fabs((emTop.xl - epTop.xl) - expectPx) < 0.03, "push: the shift is the same on every row (the quad is flat at one depth)");
        f.expect(covered(moved) > 1500 && std::fabs(covered(moved) - covered(plain)) < 40, "push: the quad covers as many pixels as before");
        double yWorst = 0;
        for (UINT y = 0; y < 256; ++y) yWorst += (plain.at(128, y)[3] > 0.5f) != (moved.at(128, y)[3] > 0.5f) ? 1 : 0;
        f.expect(yWorst <= 1, "push: the same rows are covered (y is untouched)");
    }
    {   // the window: the larger panels, the reticle and both edges stay; s = 0 is a copy; a mixed draw moves only what lies nearer than 1 m
        const st::TurretParams loud = st::pushParamsFor(3.0f, back);   // a big s, so a vertex wrongly moved would show
        for (const float w : {500.0f, 50.0f, 5.0f, 4.0f, 2.8f, 1.4f, 1.3f, 1.0f, 0.02f, 0.01f}) {
            std::vector<Vtx> v;
            addQuad(v, -0.3f * w / 1.5f, 0.3f * w / 1.5f, -0.2f * w / 1.5f, 0.2f * w / 1.5f, w, 0.0f);   // the same size on the screen at any depth
            const Frame plain = render(v, cam, nullptr, loud);
            const Frame through = render(v, cam, gs, loud);
            char what[200];
            std::snprintf(what, sizeof(what), "push: a quad at w = %.2f is outside the window (%.2f to %.2f) and the image is the plain one, bit for bit", double(w), double(loud.wMin), double(loud.wMax));
            f.expect(covered(plain) > 1000 && maxDiff(plain, through) == 0.0, what);
        }
        const float hx = st::eyeHx(0, ipd), ex = -hx;
        std::vector<Vtx> v;
        addQuad(v, qx0, qx1, qy0, qy1, w0, ex);
        const Frame plain = render(v, cam, nullptr, st::pushParamsFor(0.0f, back));
        const Frame zeroS = render(v, cam, gs, st::pushParamsFor(0.0f, back));
        f.expect(covered(plain) > 1000 && maxDiff(plain, zeroS) == 0.0, "push: with s = 0 (the cyclopean eye) the image is the plain one, bit for bit");
        std::vector<Vtx> mixed, nearOnly, farOnly;   // a weapon panel at 0.4 m above the centre, a larger panel at 1.4 m below it
        addQuad(mixed, -0.0857f, 0.0857f, 0.0143f, 0.0714f, w0, ex);
        addQuad(mixed, -0.3f, 0.3f, -0.45f, -0.25f, 1.4f, ex);
        addQuad(nearOnly, -0.0857f, 0.0857f, 0.0143f, 0.0714f, w0, ex);
        addQuad(farOnly, -0.3f, 0.3f, -0.45f, -0.25f, 1.4f, ex);
        const st::TurretParams p = st::pushParamsFor(0.0259f, back);
        const Frame mixedMoved = render(mixed, cam, gs, p);
        const Frame nearMoved = render(nearOnly, cam, gs, p);
        const Frame farPlain = render(farOnly, cam, nullptr, p);
        const Frame nearPlain = render(nearOnly, cam, nullptr, p);
        double worst = 0;
        for (UINT y = 0; y < 256; ++y)
            for (UINT x = 0; x < 256; ++x)
                for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(double(mixedMoved.at(x, y)[c]) - std::max(nearMoved.at(x, y)[c], farPlain.at(x, y)[c])));
        f.expect(covered(farPlain) > 500 && worst == 0.0 && maxDiff(nearMoved, nearPlain) > 0.05,
                 "push: a draw of a weapon panel at 0.4 m and a larger panel at 1.4 m moves the first and leaves the second: the rule is per vertex");
    }
    {   // the texture coordinates are untouched: an integer-pixel translation, to the left, reproduces the plain image shifted
        const float k = 5.0f;   // pixels
        const float s = k * 2.0f * w0 * (w0 + back) / (256.0f * back);
        std::vector<Vtx> v;
        addQuad(v, qx0, qx1, qy0, qy1, w0, 0.0f);
        Cam centred;
        centred.offX = centred.offY = 0.0f;
        const Frame plain = render(v, centred, nullptr, st::pushParamsFor(s, back));
        const Frame moved = render(v, centred, gs, st::pushParamsFor(s, back));
        double worst = 0;
        for (UINT y = 0; y < 256; ++y)
            for (UINT x = 8; x + 8 < 256; ++x)
                for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(double(moved.at(x - 5, y)[c]) - plain.at(x, y)[c]));
        f.expect(worst < 2e-3 && covered(plain) > 1000, "push: the texture coordinates and the pixel stage are untouched: a 5 px translation to the left reproduces the plain image 5 px along");
    }
}

// ---------------------------------------------------------------- the linkage, with the signatures compared
// The production bytes of each signature's geometry shader, as the build generates them, and the HLSL text they are the compile of.
struct SigBytes {
    const unsigned char* data;
    size_t size;
    const char* text;
};
#define EDVR_RIG_SIG_BYTES(L) {edvr::kSrvTurretHudGs##L##Bytecode, sizeof(edvr::kSrvTurretHudGs##L##Bytecode), edvr::kSrvTurretHudGs##L}
static const SigBytes kBytes[st::kSignatureCount] = {EDVR_RIG_SIG_BYTES(A), EDVR_RIG_SIG_BYTES(B), EDVR_RIG_SIG_BYTES(C), EDVR_RIG_SIG_BYTES(D), EDVR_RIG_SIG_BYTES(E),
                                                     EDVR_RIG_SIG_BYTES(F), EDVR_RIG_SIG_BYTES(G), EDVR_RIG_SIG_BYTES(H)};
#undef EDVR_RIG_SIG_BYTES

static void linkageCases(int fam, ID3D11GeometryShader* gs) {
    const st::Signature& S = st::kSignatures[fam];
    ComPtr<ID3D11ShaderReflection> vsr, psr, gsr;
    ck(D3DReflect(g_vsCode[fam]->GetBufferPointer(), g_vsCode[fam]->GetBufferSize(), __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(vsr.GetAddressOf())), "reflect the vertex shader");
    ck(D3DReflect(g_psCode[fam]->GetBufferPointer(), g_psCode[fam]->GetBufferSize(), __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(psr.GetAddressOf())), "reflect the pixel shader");
    ck(D3DReflect(kBytes[fam].data, kBytes[fam].size, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(gsr.GetAddressOf())), "reflect the production geometry shader");
    D3D11_SHADER_DESC vd{}, pd{}, gd{};
    vsr->GetDesc(&vd);
    psr->GetDesc(&pd);
    gsr->GetDesc(&gd);
    check(vd.OutputParameters == S.count && gd.InputParameters == S.count && gd.OutputParameters == S.count, "the signature's elements, the same number in the vertex shader's output and the geometry shader's input and output");
    for (UINT i = 0; i < S.count; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC o{}, in{}, out{};
        ck(vsr->GetOutputParameterDesc(i, &o), "output element");
        ck(gsr->GetInputParameterDesc(i, &in), "input element");
        ck(gsr->GetOutputParameterDesc(i, &out), "geometry shader output element");
        check(!std::strcmp(o.SemanticName, in.SemanticName) && o.SemanticIndex == in.SemanticIndex && o.Register == in.Register && o.Mask == in.Mask &&
                  o.ComponentType == in.ComponentType && o.SystemValueType == in.SystemValueType,
              "the geometry shader's input element is the vertex shader's output element: semantic, index, register, mask and type");
        check(!std::strcmp(o.SemanticName, out.SemanticName) && o.SemanticIndex == out.SemanticIndex && o.Register == out.Register && o.Mask == out.Mask,
              "...and its output element is the same again: a pass-through of the signature");
        check(!std::strcmp(o.SemanticName, S.el[i].name) && o.SemanticIndex == S.el[i].index && o.Register == i && o.Mask == ((1u << S.el[i].comps) - 1u),
              "...and each element is what srv_turret_math.h's signature table says (the semantic the game's shaders spell, the register in order, the components written)");
    }
    // the pixel shader's inputs are a subset of what the geometry shader writes (SV_Position is the rasterizer's)
    for (UINT i = 0; i < pd.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC in{};
        ck(psr->GetInputParameterDesc(i, &in), "pixel shader input element");
        if (!std::strcmp(in.SemanticName, "SV_Position")) continue;
        bool found = false;
        for (UINT j = 0; j < gd.OutputParameters; ++j) {
            D3D11_SIGNATURE_PARAMETER_DESC out{};
            ck(gsr->GetOutputParameterDesc(j, &out), "geometry shader output element");
            found = found || (!std::strcmp(in.SemanticName, out.SemanticName) && in.SemanticIndex == out.SemanticIndex && (in.Mask & ~out.Mask) == 0);
        }
        check(found, "each input of the pixel shader is written by the geometry shader (a subset of its output)");
    }
    check(gsr->GetGSInputPrimitive() == D3D_PRIMITIVE_TRIANGLE && gd.GSOutputTopology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP && gd.GSMaxOutputVertexCount == 3,
          "the geometry shader takes triangles and emits at most three vertices");
    // under the debug layer, the draw links without a word
    if (g_messages) g_messages->ClearStoredMessages();
    std::vector<Vtx> v;
    addQuad(v, -0.3f, 0.3f, -0.2f, 0.2f, 1.5f, 0.0f);
    (void)render(v, Cam{}, gs, st::paramsFor(0.0259f, 1.02f));
    if (g_messages) {
        UINT64 errors = 0;
        for (UINT64 i = 0; i < g_messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            g_messages->GetMessage(i, nullptr, &size);
            std::vector<char> data(size);
            auto* m = reinterpret_cast<D3D11_MESSAGE*>(data.data());
            ck(g_messages->GetMessage(i, m, &size), "debug message");
            if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
                std::puts(m->pDescription);
                ++errors;
            }
        }
        check(errors == 0, "the shader links against the stand-in for the game's vertex shader under the debug layer with no error");
        if (fam != 0) {
            g_ctx->ClearState();
            return;
        }
        // the control (once): a vertex shader without the texture coordinates is reported
        g_messages->ClearStoredMessages();
        ComPtr<ID3D11VertexShader> bad;
        const auto badCode = compileHlsl(kBadVs, "vs_5_0");
        ck(g_dev->CreateVertexShader(badCode->GetBufferPointer(), badCode->GetBufferSize(), nullptr, &bad), "the control vertex shader");
        const D3D11_INPUT_ELEMENT_DESC el[1] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        ComPtr<ID3D11InputLayout> layout;
        ck(g_dev->CreateInputLayout(el, 1, badCode->GetBufferPointer(), badCode->GetBufferSize(), &layout), "the control layout");
        auto vb = makeBuffer(UINT(v.size() * sizeof(Vtx)), D3D11_BIND_VERTEX_BUFFER, v.data());
        auto cb0 = cameraBuffer(Cam{});
        Target t(64, 64);
        g_ctx->ClearState();
        g_ctx->OMSetRenderTargets(1, t.rtv.GetAddressOf(), nullptr);
        const D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
        g_ctx->RSSetViewports(1, &vp);
        const UINT stride = sizeof(Vtx), offset = 0;
        ID3D11Buffer* vbs[1] = {vb.Get()};
        g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
        g_ctx->IASetInputLayout(layout.Get());
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(bad.Get(), nullptr, 0);
        g_ctx->VSSetConstantBuffers(0, 1, cb0.GetAddressOf());
        auto cb = g_constants[0].get(g_ctx.Get(), st::paramsFor(0.0259f, 1.02f));
        st::Binding b;
        check(b.begin(g_ctx.Get(), gs, cb) == st::Binding::Begin::kBound, "control: bound");
        g_ctx->Draw(UINT(v.size()), 0);
        b.finish(g_ctx.Get());
        UINT64 reported = 0;
        for (UINT64 i = 0; i < g_messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            g_messages->GetMessage(i, nullptr, &size);
            std::vector<char> data(size);
            auto* m = reinterpret_cast<D3D11_MESSAGE*>(data.data());
            ck(g_messages->GetMessage(i, m, &size), "debug message");
            if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) ++reported;
        }
        check(reported > 0, "the control: the debug layer does report a vertex shader whose output lacks the texture coordinate (so its silence above means something)");
        g_messages->ClearStoredMessages();
        g_ctx->ClearState();
    } else {
        std::puts("srv_turret_hud_test: no D3D debug layer on this machine; the linkage control is skipped (the signatures were compared by reflection)");
    }
}

// ---------------------------------------------------------------- the binding on a real context
static bool boundIs(ID3D11GeometryShader* gs, ID3D11Buffer* cb) {
    ComPtr<ID3D11GeometryShader> g;
    ComPtr<ID3D11Buffer> b;
    g_ctx->GSGetShader(&g, nullptr, nullptr);
    g_ctx->GSGetConstantBuffers(st::kGsSlot, 1, &b);
    return g.Get() == gs && b.Get() == cb;
}
static ULONG refsOf(IUnknown* u) {
    u->AddRef();
    return u->Release();
}
static void failingSetGs(ID3D11DeviceContext*, ID3D11GeometryShader*, ID3D11ClassInstance* const*, uint32_t) {
    RaiseException(0xe0421314, 0, 0, nullptr);
}
static const std::vector<char>& passThroughBytes() {
    static std::vector<char> bytes;
    if (bytes.empty()) {
        const char* text = "struct V { float2 uv : TEXCOORD0; float4 p : SV_Position; };\n"
                           "[maxvertexcount(3)] void main(triangle V v[3], inout TriangleStream<V> s) { s.Append(v[0]); s.Append(v[1]); s.Append(v[2]); }\n";
        const auto code = compileHlsl(text, "gs_5_0");
        bytes.assign(static_cast<const char*>(code->GetBufferPointer()), static_cast<const char*>(code->GetBufferPointer()) + code->GetBufferSize());
    }
    return bytes;
}

static void bindingCases(ID3D11GeometryShader* gs, Fails& f, bool leakChecks) {
    const std::vector<float> sentinelData{7, 8, 9, 10};
    auto sentinel = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, sentinelData.data());
    auto constants = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, sentinelData.data());
    auto gameState = [&] {
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->GSSetConstantBuffers(st::kGsSlot, 1, sentinel.GetAddressOf());
    };
    g_ctx->ClearState();
    const ULONG gsRefs = refsOf(gs), cbRefs = refsOf(sentinel.Get()), constRefs = refsOf(constants.Get());
    auto quiet = [&] { return refsOf(gs) == gsRefs && refsOf(sentinel.Get()) == cbRefs && refsOf(constants.Get()) == constRefs; };
    {   // the ordinary draw: in, and out again
        gameState();
        st::Binding b;
        f.expect(!b.needsRestore(), "a fresh binding owes nothing");
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get()) == st::Binding::Begin::kBound, "begin binds the shader and its constants");
        f.expect(b.needsRestore() && boundIs(gs, constants.Get()), "both are bound, the game's owed back");
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get()) == st::Binding::Begin::kRefused, "a second begin while one is open is refused");
        f.expect(b.restore(g_ctx.Get()) && !b.needsRestore(), "restore puts the game's back");
        f.expect(boundIs(nullptr, sentinel.Get()), "the game's geometry stage (none) and constant slot are exactly as they were");
        if (leakChecks) f.expect(quiet(), "no reference leaked or kept");
        f.expect(b.restore(g_ctx.Get()), "restore with nothing owed is a no-op");
    }
    {   // the game's unbound constant slot comes back unbound
        ID3D11Buffer* noBuffer = nullptr;
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->GSSetConstantBuffers(st::kGsSlot, 1, &noBuffer);
        st::Binding b;
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get()) == st::Binding::Begin::kBound, "begin over an unbound constant slot");
        f.expect(b.restore(g_ctx.Get()) && boundIs(nullptr, nullptr), "...gives the unbound slot back");
    }
    {   // a game geometry shader of its own is not ours to replace: nothing is touched
        ComPtr<ID3D11GeometryShader> theirs;
        ck(g_dev->CreateGeometryShader(passThroughBytes().data(), passThroughBytes().size(), nullptr, &theirs), "the game's own geometry shader");
        gameState();
        g_ctx->GSSetShader(theirs.Get(), nullptr, 0);
        st::Binding b;
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get()) == st::Binding::Begin::kGameHasGs, "a game geometry shader bound: refused, kGameHasGs");
        f.expect(!b.needsRestore() && boundIs(theirs.Get(), sentinel.Get()), "...and nothing of the game's state moved");
        g_ctx->GSSetShader(nullptr, nullptr, 0);
    }
    {   // no shader, no constants, no context: refused, nothing moved
        gameState();
        st::Binding b;
        f.expect(b.begin(g_ctx.Get(), nullptr, constants.Get()) == st::Binding::Begin::kRefused && b.begin(g_ctx.Get(), gs, nullptr) == st::Binding::Begin::kRefused &&
                     b.begin(nullptr, gs, constants.Get()) == st::Binding::Begin::kRefused,
                 "no shader, no buffer or no context: refused");
        f.expect(!b.needsRestore() && boundIs(nullptr, sentinel.Get()), "...and the game's state is where it was");
        if (leakChecks) f.expect(quiet(), "...with nothing leaked by the getters that ran");
    }
    {   // a restore that fails twice: the references are kept, the boundary settles it
        gameState();
        st::Binding b;
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get()) == st::Binding::Begin::kBound, "settle scene: bound");
        const auto result = b.finish(g_ctx.Get(), failingSetGs);
        f.expect(result.retried && !result.restored && b.needsRestore(), "a restore that failed twice is retried once and still owed");
        {
            ComPtr<ID3D11GeometryShader> stuck;
            g_ctx->GSGetShader(&stuck, nullptr, nullptr);
            f.expect(stuck.Get() == gs, "...and EDVR's shader is still bound until it is settled (the constant slot, whose setter worked, is the game's again)");
        }
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get()) == st::Binding::Begin::kRefused, "no new draw begins while one is owed");
        f.expect(!b.settle(g_ctx.Get(), failingSetGs) && b.needsRestore(), "a settle whose setter faults changes nothing and is still owed");
        f.expect(b.settle(g_ctx.Get()) && !b.needsRestore(), "the next settle puts the game's back");
        f.expect(boundIs(nullptr, sentinel.Get()), "settled: the game's geometry stage and constant slot are bound again");
        if (leakChecks) f.expect(quiet(), "settled: no reference kept");
        f.expect(b.settle(g_ctx.Get()), "a settled binding settles idempotently");
    }
    {   // the game rebound meanwhile: its own state stays
        gameState();
        st::Binding b;
        b.begin(g_ctx.Get(), gs, constants.Get());
        b.finish(g_ctx.Get(), failingSetGs);
        auto theirs = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, sentinelData.data());
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->GSSetConstantBuffers(st::kGsSlot, 1, theirs.GetAddressOf());
        f.expect(b.settle(g_ctx.Get()) && !b.needsRestore(), "settle with both rebound by the game succeeds");
        f.expect(boundIs(nullptr, theirs.Get()), "...and leaves the game's own newer state, not the saved one");
        gameState();
    }
    {   // a null context cannot settle
        gameState();
        st::Binding b;
        b.begin(g_ctx.Get(), gs, constants.Get());
        b.finish(g_ctx.Get(), failingSetGs);
        f.expect(!b.settle(nullptr) && b.needsRestore(), "no context: the settle reports failure and keeps the state");
        b.settle(g_ctx.Get());
        if (leakChecks) f.expect(quiet(), "settled after the null context");
    }
    g_ctx->ClearState();
}

// The issue's end: the geometry stage is empty, and a plain issue after it is the plain image again.
static void afterIssueCases(ID3D11GeometryShader* gs, Fails& f, bool skipRestore) {
    const Cam cam;
    std::vector<Vtx> v;
    addQuad(v, -0.3f, 0.3f, -0.2f, 0.2f, 1.5f, -0.02755f);
    DrawOpts o;
    o.skipRestore = skipRestore;
    const st::TurretParams p = st::paramsFor(0.0259f, 1.02f);
    const Frame plainBefore = render(v, cam, nullptr, p);
    (void)render(v, cam, gs, p, 256, 256, o);
    ComPtr<ID3D11GeometryShader> after;
    g_ctx->GSGetShader(&after, nullptr, nullptr);
    f.expect(after == nullptr, "the geometry stage is empty after the issue");
    g_ctx->ClearState();
    const Frame plainAfter = render(v, cam, nullptr, p);
    f.expect(maxDiff(plainBefore, plainAfter) == 0.0, "a plain issue after the shifted one is the plain image again");
}

// ---------------------------------------------------------------- the constants
static void constantsCases() {
    st::Constants c;
    const st::TurretParams a = st::paramsFor(0.0259f, 1.02f);
    check(c.get(nullptr, a) == nullptr, "no context: no buffer");
    ID3D11Buffer* first = c.get(g_ctx.Get(), a);
    check(first && c.buffer() == first && c.written() == a && !c.failed(), "the buffer is made once, with the numbers");
    check(c.get(g_ctx.Get(), a) == first && c.written() == a, "the same numbers: the same buffer, not rewritten");
    const st::TurretParams b = st::paramsFor(-0.0259f, 1.02f);
    check(c.get(g_ctx.Get(), b) == first && c.written() == b, "other numbers: the same buffer, rewritten");
    // what the shader sees is the four numbers
    const auto readBack = [&](ID3D11Buffer* source) {
        D3D11_BUFFER_DESC bd{};
        source->GetDesc(&bd);
        bd.BindFlags = bd.MiscFlags = bd.StructureByteStride = 0;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> stage;
        ck(g_dev->CreateBuffer(&bd, nullptr, &stage), "staging");
        g_ctx->CopyResource(stage.Get(), source);
        D3D11_MAPPED_SUBRESOURCE m{};
        ck(g_ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "Map");
        st::TurretParams out;
        std::memcpy(&out, m.pData, sizeof(out));
        g_ctx->Unmap(stage.Get(), 0);
        return out;
    };
    check(readBack(first) == b, "the buffer holds {s, gap, wMin, wMax} as written");
    st::TurretParams bad = a;
    bad.s = NAN;
    check(c.get(g_ctx.Get(), bad) == nullptr, "a number that is not finite: refused");
    bad = a;
    bad.wMin = bad.gap;
    check(c.get(g_ctx.Get(), bad) == nullptr, "a window that starts at the gap (a divisor of zero): refused");
    bad = a;
    bad.wMax = bad.wMin;
    check(c.get(g_ctx.Get(), bad) == nullptr, "an empty window: refused");
    bad = a;
    bad.gap = 0.0f;
    check(c.get(g_ctx.Get(), bad) == nullptr, "a gap of 0 (it moves nothing, and the module never binds one): refused");
    bad = a;
    bad.gap = 2.5f;
    bad.wMin = 2.6f;
    check(c.get(g_ctx.Get(), bad) == nullptr, "a gap beyond 2 m: refused");
    bad = st::pushParamsFor(0.0259f, 2.5f);
    check(c.get(g_ctx.Get(), bad) == nullptr, "...and a push beyond 2 m: refused");
    bad = st::pushParamsFor(0.0259f, 1.10f);
    bad.wMin = -2.0f;
    check(c.get(g_ctx.Get(), bad) == nullptr, "a push whose window starts at or below its (negative) gap: refused");
    bad = st::pushParamsFor(0.0259f, 1.10f);
    bad.gap = -INFINITY;
    check(c.get(g_ctx.Get(), bad) == nullptr, "a gap that is not finite: refused");
    check(c.get(g_ctx.Get(), a) == first, "...and the buffer is still the good one");
    const st::TurretParams push = st::pushParamsFor(0.0259f, 1.10f), pushTop = st::pushParamsFor(-0.0259f, st::kPushMaxCm / 100.0f);
    check(c.get(g_ctx.Get(), push) == first && c.written() == push, "a push (a negative gap, window 0.02 to 1.0): accepted, the same buffer rewritten");
    check(readBack(first) == push, "...and the buffer holds the push's numbers");
    check(c.get(g_ctx.Get(), pushTop) == first && c.written() == pushTop, "a push of the key's top (2.00 m) is accepted too");
    check(c.get(g_ctx.Get(), a) == first && c.written() == a, "...and the pull again rewrites it");
    ComPtr<ID3D11Device> d2;
    ComPtr<ID3D11DeviceContext> c2;
    ck(edvr::systemD3D11CreateDevice()(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d2, nullptr, &c2), "second device");
    ID3D11Buffer* other = c.get(c2.Get(), a);
    check(other && other != first && c.written() == a, "another device: the buffer is made again for it");
    c.reset();
    check(c.buffer() == nullptr && !c.failed(), "reset releases the buffer");
    // the production arrays also carry the key defaults the module reads
    check(st::kBackDefaultCm == 60 && st::kGapDefaultCm == 0 && st::kPushDefaultCm == 110, "the keys' defaults: 60 cm back, no gap (the pull is off), a 110 cm push on both groups");
}

// ---------------------------------------------------------------- triangle strips (the flight HUD's strokes are drawn as one)
// A quad as a four-vertex strip. The input assembler hands the geometry shader's `triangle` input each strip triangle in the winding the rasterizer would have used, and the
// shader emits each as a strip of its own, so: (1) through a window nothing lies in the picture is the plain strip's bit for bit under every culling mode, and in the mode that shows the
// strip the other mode shows nothing (the winding is consistent, the control that the odd triangle is not turned inside out); (2) with the pull, and with the push, the strip lands
// exactly where the same quad as a list lands.
static void stripCases(ID3D11GeometryShader* gs, Fails& f) {
    const Cam cam;
    const float ipd = 0.0551f, gap = kPullGapM, w0 = 1.4f;
    const float hx = st::eyeHx(0, ipd), ex = -hx;
    std::vector<Vtx> list, strip;
    addQuad(list, -0.30f, 0.30f, -0.20f, 0.20f, w0, ex);
    strip = {list[0], list[1], list[2], list[4]};   // (x0,y0) (x1,y0) (x0,y1) (x1,y1)
    const st::TurretParams p = st::paramsFor(cam.p00 * hx, gap), zero{cam.p00 * hx, gap, 100.0f, 200.0f};   // zero: a valid window with nothing in it
    double shown[3] = {};
    for (int cull = 0; cull < 3; ++cull) {
        DrawOpts o;
        o.cull = cull;
        o.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        const Frame plain = render(strip, cam, nullptr, zero, 256, 256, o);
        const Frame copy = render(strip, cam, gs, zero, 256, 256, o);
        shown[cull] = covered(plain);
        f.expect(maxDiff(plain, copy) == 0.0, "a triangle strip through the shader with nothing in its window is the plain strip, bit for bit, under every culling mode");
        DrawOpts ol = o;
        ol.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        const Frame moved = render(strip, cam, gs, p, 256, 256, o);
        const Frame listMoved = render(list, cam, gs, p, 256, 256, ol);
        f.expect(maxDiff(moved, listMoved) == 0.0, "a strip quad under the pull lands exactly where the same quad as a list lands");
        const Frame plainList = render(list, cam, nullptr, p, 256, 256, ol);
        f.expect(cull != 0 || (covered(moved) > 1500 && maxDiff(moved, plainList) > 0.05), "...and it is shifted (the picture is not the plain quad's)");
    }
    f.expect(shown[0] > 1500 && ((shown[1] > 1500) != (shown[2] > 1500)) && (shown[1] < 1.0 || shown[2] < 1.0),
             "the control: the plain strip is whole with no culling and with one facing culled, and gone with the other (both triangles share a winding)");
    {   // the push: a weapon panel at 0.4 m as a strip lands where the same quad as a list lands, under every culling mode
        const float sc = kPanelW / 1.5f;
        std::vector<Vtx> plist, pstrip;
        addQuad(plist, -0.30f * sc, 0.30f * sc, -0.20f * sc, 0.20f * sc, kPanelW, ex);
        pstrip = {plist[0], plist[1], plist[2], plist[4]};
        const st::TurretParams push = st::pushParamsFor(cam.p00 * hx, kPushBackM);
        for (int cull = 0; cull < 3; ++cull) {
            DrawOpts o;
            o.cull = cull;
            o.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            DrawOpts ol = o;
            ol.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            const Frame moved = render(pstrip, cam, gs, push, 256, 256, o);
            const Frame listMoved = render(plist, cam, gs, push, 256, 256, ol);
            f.expect(maxDiff(moved, listMoved) == 0.0, "a strip quad under the push lands exactly where the same quad as a list lands");
            const Frame plainList = render(plist, cam, nullptr, push, 256, 256, ol);
            f.expect(cull != 0 || (covered(moved) > 1500 && maxDiff(moved, plainList) > 0.05), "...and it is shifted (the picture is not the plain quad's)");
        }
    }
}

// ---------------------------------------------------------------- the GPU instrument (srv_turret.cpp's probe*): the same calls, on WARP
static void instrumentCases(int fam, ID3D11GeometryShader* gs) {
    const st::Signature& S = st::kSignatures[fam];
    const uint32_t strideFloats = st::sigFloats(fam), posFloat = st::sigPosFloat(fam);
    // the twin, made as the module makes it: the signature's bytecode, its stream-output entries, stream 0 rasterized as well
    ComPtr<ID3D11GeometryShader> twin;
    D3D11_SO_DECLARATION_ENTRY decl[st::kMaxSigElements] = {};
    const UINT entries = st::soDeclaration(fam, decl);
    const UINT stride = strideFloats * 4;
    ck(g_dev->CreateGeometryShaderWithStreamOutput(kBytes[fam].data, kBytes[fam].size, decl, entries, &stride, 1, 0, nullptr, &twin), "the stream-output twin is made from the signature's bytecode");
    const Cam cam;
    const float ipd = 0.0551f, gap = kPullGapM, w0 = 1.4f;   // the pull's gap and a deep panel's depth
    const float hx = st::eyeHx(0, ipd), ex = -hx;
    const st::TurretParams params = st::paramsFor(cam.p00 * hx, gap);
    std::vector<Vtx> quad;
    addQuad(quad, -0.30f, 0.30f, -0.20f, 0.20f, w0, ex);
    DrawOpts base;

    // a plain draw: the vertex shader ran, the geometry shader did not (gs = 0 with ia_prims above 0 is how a flight reads "never ran")
    Sample plain;
    DrawOpts o = base;
    o.sample = &plain;
    const Frame plainFrame = render(quad, cam, nullptr, params, 256, 256, o);
    check(plain.stats.IAPrimitives == 2 && plain.stats.VSInvocations >= 4 && plain.stats.GSInvocations == 0 && plain.stats.GSPrimitives == 0 && plain.stats.PSInvocations > 1000,
          "a quad drawn with no geometry shader: two primitives in, vertex shader and pixel shader ran, the geometry shader's invocation count is 0");

    // through the production shader: it ran once for each triangle
    Sample prod;
    o.sample = &prod;
    const Frame prodFrame = render(quad, cam, gs, params, 256, 256, o);
    check(prod.stats.IAPrimitives == 2 && prod.stats.GSInvocations == 2 && prod.stats.GSPrimitives == 2 && prod.stats.CInvocations >= 2 && prod.stats.PSInvocations > 1000 &&
              !prod.gsAfter,
          "...and through the production shader: two primitives in, the geometry shader ran twice and emitted two, and the context's geometry stage is empty after");
    check(plain.stats.PSInvocations != prod.stats.PSInvocations || maxDiff(plainFrame, prodFrame) > 0.01, "...and the statistics see the shift: the pixels differ from the plain draw");

    // through the twin with its stream-output target: the same statistics, the same pixels, and the records of what the shader wrote
    Sample cap;
    o.sample = &cap;
    o.soTarget = true;
    const Frame twinFrame = render(quad, cam, twin.Get(), params, 256, 256, o);
    check(cap.stats.GSInvocations == 2 && cap.stats.GSPrimitives == 2 && cap.stats.IAPrimitives == 2 && cap.so.NumPrimitivesWritten == 2 && cap.so.PrimitivesStorageNeeded == 2,
          "through the twin: two invocations, two primitives written to the stream-output target, none lost");
    check(maxDiff(prodFrame, twinFrame) == 0.0, "...and it rasterizes exactly as the production shader does (the instrument does not change the picture)");
    check(!cap.soTargetAfter && !cap.gsAfter, "...and the stream-output target and the geometry stage are empty after the scope");
    const st::CaptureSummary c = st::summarizeCapture(cap.records.data(), 6, strideFloats, posFloat, params);
    check(c.vertices == 6 && c.inWindow == 6 && std::fabs(c.wMin - w0) < 1e-4f && std::fabs(c.wMax - w0) < 1e-4f, "the six captured vertices lie at view depth 1.4 (a deep panel) and in the window");
    const float shift = st::clipXShift(params, w0);
    check(std::fabs(c.shiftMin - shift) < 1e-6f && std::fabs(c.shiftMax - shift) < 1e-6f && shift > 0.05f, "...and the shift the rule gives them is s gap / (w - gap)");
    // the captured NDC x is the shifted position: (P00 X + offX w + shift) / w for the quad's left and right edges
    const float xl = (cam.p00 * (-0.30f - ex) + cam.offX * w0 + shift) / w0, xr = (cam.p00 * (0.30f - ex) + cam.offX * w0 + shift) / w0;
    check(std::fabs(c.ndcXMin - xl) < 1e-5f && std::fabs(c.ndcXMax - xr) < 1e-5f, "...and the streamed clip x / w is the shifted edge: the shader wrote the position the rule says");
    // the captured record is the vertex whole: the interpolants as the vertex shader wrote them, not only the position (the first vertex is u = 0, v = 0, and the second element
    // starts with 0.25; the second vertex starts with u = 1)
    bool whole = std::fabs(cap.records[0] - 0.0f) < 1e-6f && std::fabs(cap.records[1] - 0.0f) < 1e-6f && std::fabs(cap.records[strideFloats] - 1.0f) < 1e-6f;
    if (S.el[0].comps == 4) whole = whole && std::fabs(cap.records[3] - 1.0f) < 1e-6f;
    if (S.count > 2) whole = whole && std::fabs(cap.records[S.el[0].comps] - 0.25f) < 1e-6f;
    check(whole, "...with the interpolants intact in the record (the texture coordinate and the second element): the shader passes them through");
    // the unshifted position of the same quad from the twin with s = 0: the difference is the shift
    Sample capPlain;
    DrawOpts op = base;
    op.sample = &capPlain;
    op.soTarget = true;
    (void)render(quad, cam, twin.Get(), st::paramsFor(0.0f, gap), 256, 256, op);   // s = 0: the twin writes the game's own positions
    const st::CaptureSummary c0 = st::summarizeCapture(capPlain.records.data(), 6, strideFloats, posFloat, st::paramsFor(0.0f, gap));
    check(std::fabs((c.ndcXMin - c0.ndcXMin) - shift / w0) < 1e-5f && std::fabs(c.ndcYMin - c0.ndcYMin) < 1e-6f && std::fabs(c.wMin - c0.wMin) < 1e-6f,
          "the captured positions differ from the unshifted ones by the shift in x only (y and w are the game's)");

    // the near layer (w = 0.42) and the reticle's depth (500 m): the shader RUNS (gs = 2) and moves nothing, the pixels are the plain draw's, and the capture says why
    // (in_window 0 and the depth range): this is what a flight reads for the near layer's draws
    for (const float depth : {0.42f, 500.0f}) {
        std::vector<Vtx> outside;
        const float scale = depth / w0;   // the same size on the screen at any depth
        addQuad(outside, -0.30f * scale, 0.30f * scale, -0.20f * scale, 0.20f * scale, depth, ex);
        Sample outCap, outPlain;
        DrawOpts oo = base;
        oo.sample = &outCap;
        oo.soTarget = true;
        const Frame twinOut = render(outside, cam, twin.Get(), params, 256, 256, oo);
        DrawOpts ob = base;
        ob.sample = &outPlain;
        const Frame plainOut = render(outside, cam, nullptr, params, 256, 256, ob);
        const st::CaptureSummary co = st::summarizeCapture(outCap.records.data(), 6, strideFloats, posFloat, params);
        char what[220];
        std::snprintf(what, sizeof(what), "a quad at %.2f m: the geometry shader ran twice, the capture reads in_window 0 and w = %.2f, and the picture is the plain draw's bit for bit", double(depth),
                      double(depth));
        check(outCap.stats.GSInvocations == 2 && co.vertices == 6 && co.inWindow == 0 && std::fabs(co.wMin - depth) < depth * 1e-3f && co.shiftMin == 0.0f && co.shiftMax == 0.0f &&
                  maxDiff(twinOut, plainOut) == 0.0 && covered(twinOut) > 1000,
              what);
    }
    {   // the push: a weapon panel (w 0.4) is IN the window (0.02, 1.0) with a negative shift, a larger panel (1.4) and the reticle (500) are not
        const st::TurretParams push = st::pushParamsFor(cam.p00 * hx, kPushBackM);
        const float sc = kPanelW / w0;   // the same size on the screen
        std::vector<Vtx> panel;
        addQuad(panel, -0.30f * sc, 0.30f * sc, -0.20f * sc, 0.20f * sc, kPanelW, ex);
        Sample pushCap, pushPlain;
        DrawOpts opush = base;
        opush.sample = &pushCap;
        opush.soTarget = true;
        const Frame twinPush = render(panel, cam, twin.Get(), push, 256, 256, opush);
        DrawOpts oprod = base;
        oprod.sample = &pushPlain;
        const Frame prodPush = render(panel, cam, gs, push, 256, 256, oprod);
        const st::CaptureSummary cp = st::summarizeCapture(pushCap.records.data(), 6, strideFloats, posFloat, push);
        const float pshift = st::clipXShift(push, kPanelW);
        check(pushCap.stats.GSInvocations == 2 && cp.vertices == 6 && cp.inWindow == 6 && std::fabs(cp.wMin - kPanelW) < 1e-4f && std::fabs(cp.wMax - kPanelW) < 1e-4f,
              "under the push a quad at 0.4 m: the geometry shader ran twice and all six captured vertices lie in the window");
        check(pshift < -0.01f && std::fabs(cp.shiftMin - pshift) < 1e-6f && std::fabs(cp.shiftMax - pshift) < 1e-6f && maxDiff(twinPush, prodPush) == 0.0,
              "...with the NEGATIVE shift the rule gives them (-s d / (w + d)), and the twin rasterizes exactly as the production shader does");
        const float pl = (cam.p00 * (-0.30f * sc - ex) + cam.offX * kPanelW + pshift) / kPanelW, pr = (cam.p00 * (0.30f * sc - ex) + cam.offX * kPanelW + pshift) / kPanelW;
        check(std::fabs(cp.ndcXMin - pl) < 1e-5f && std::fabs(cp.ndcXMax - pr) < 1e-5f, "...and the streamed clip x / w is the pushed edge: the shader wrote the position the rule says");
        std::vector<Vtx> deep;
        addQuad(deep, -0.30f, 0.30f, -0.20f, 0.20f, w0, ex);
        Sample deepCap;
        DrawOpts odeep = base;
        odeep.sample = &deepCap;
        odeep.soTarget = true;
        (void)render(deep, cam, twin.Get(), push, 256, 256, odeep);
        const st::CaptureSummary cd = st::summarizeCapture(deepCap.records.data(), 6, strideFloats, posFloat, push);
        check(deepCap.stats.GSInvocations == 2 && cd.vertices == 6 && cd.inWindow == 0 && std::fabs(cd.wMin - w0) < 1e-4f && cd.shiftMin == 0.0f && cd.shiftMax == 0.0f,
              "...and a larger panel at 1.4 m: the shader ran, the capture reads in_window 0 and no shift (a healthy row under the push)");
    }
    // the summary's own edges
    std::vector<float> junk(2 * strideFloats, 0.0f);
    junk[posFloat] = NAN;
    junk[posFloat + 3] = 1.0f;
    junk[strideFloats + posFloat + 3] = -1.0f;
    const st::CaptureSummary cj = st::summarizeCapture(junk.data(), 2, strideFloats, posFloat, params);
    check(cj.vertices == 1 && cj.inWindow == 0 && st::summarizeCapture(nullptr, 6, strideFloats, posFloat, params).vertices == 0 &&
              st::summarizeCapture(junk.data(), 2, 3, posFloat, params).vertices == 0,
          "the summary skips a vertex that is not finite, reads nothing from nothing and refuses a stride too short for the position");
    g_ctx->ClearState();
}

// ---------------------------------------------------------------- the Scorpion latch (journal_vehicle_latch.h; no device)
struct LatchRun {
    jv::Latch latch;
    std::vector<std::pair<bool, jv::Event>> changes;
    void feed(const std::string& text) {
        latch.feed(text.data(), uint32_t(text.size()), [this](bool scorpion, jv::Event cause) { changes.emplace_back(scorpion, cause); });
    }
};
static std::string line(const std::string& event, const std::string& fields) {
    return "{ \"timestamp\":\"2026-10-30T16:13:52Z\", \"event\":\"" + event + "\"" + (fields.empty() ? "" : ", " + fields) + " }\n";
}
static std::string loadGame(const std::string& ship) {
    return line("LoadGame", "\"FID\":\"F1234567\", \"Commander\":\"Cmdr Test\", \"Horizons\":true, \"Odyssey\":true, \"Ship\":\"" + ship + "\", \"Ship_Localised\":\"" + ship + " (localised)\", \"ShipID\":4, "
                            "\"ShipName\":\"\", \"ShipIdent\":\"\", \"FuelLevel\":16.0, \"FuelCapacity\":16.0, \"GameMode\":\"Open\", \"Credits\":1234567, \"Loan\":0");
}
static std::string launchSrv(const std::string& type) {
    return line("LaunchSRV", "\"Loadout\":\"starter\", \"ID\":1, \"SRVType\":\"" + type + "\", \"SRVType_Localised\":\"" + type + " (localised)\", \"PlayerControlled\":true");
}
static std::string music(const std::string& track) { return line("Music", "\"MusicTrack\":\"" + track + "\""); }

static void latchCases() {
    const std::string kScorpion = "Combat_Multicrew_SRV_01";
    {   // the Scorpion loaded, the Scorpion launched, the Scarab launched, the other ships
        LatchRun r;
        check(!r.latch.scorpion(), "a new latch is false: unknown is not the Scorpion");
        r.feed(loadGame(kScorpion));
        check(r.latch.scorpion() && r.changes.size() == 1 && r.changes[0].first && r.changes[0].second == jv::Event::kLoadGame, "LoadGame into Combat_Multicrew_SRV_01 sets the latch, once, naming the event");
        r.feed(loadGame(kScorpion));
        check(r.latch.scorpion() && r.changes.size() == 1, "...and the same value again is no change (each change is said once)");
        r.feed(loadGame("Anaconda"));
        check(!r.latch.scorpion() && r.changes.size() == 2 && !r.changes[1].first && r.changes[1].second == jv::Event::kLoadGame, "a LoadGame of any other ship clears it");
        r.feed(launchSrv("combat_multicrew_srv_01"));
        check(r.latch.scorpion() && r.changes.back().second == jv::Event::kLaunchSrv, "LaunchSRV with combat_multicrew_srv_01 (lower case) sets it");
        r.feed(launchSrv("testbuggy"));
        check(!r.latch.scorpion() && !r.changes.back().first, "LaunchSRV with testbuggy (the Scarab) clears it");
        const size_t n = r.changes.size();
        r.feed(launchSrv("testbuggy"));
        check(r.changes.size() == n, "...and a second Scarab is no change");
        r.feed(launchSrv("COMBAT_MULTICREW_SRV_01"));
        check(r.latch.scorpion(), "the comparison is case-insensitive");
    }
    {   // what clears it
        const std::pair<std::string, jv::Event> clears[] = {
            {music("MainMenu"), jv::Event::kMusic}, {line("DockSRV", "\"SRVType\":\"combat_multicrew_srv_01\", \"ID\":1"), jv::Event::kDockSrv},
            {line("SRVDestroyed", "\"SRVType\":\"combat_multicrew_srv_01\", \"ID\":1"), jv::Event::kSrvDestroyed}, {line("Shutdown", ""), jv::Event::kShutdown}};
        for (const auto& c : clears) {
            LatchRun r;
            r.feed(launchSrv("combat_multicrew_srv_01"));
            r.feed(c.first);
            char what[160];
            std::snprintf(what, sizeof(what), "%s clears the latch, naming the event", jv::eventName(c.second));
            check(!r.latch.scorpion() && r.changes.size() == 2 && !r.changes[1].first && r.changes[1].second == c.second, what);
        }
        LatchRun r;
        r.feed(launchSrv("combat_multicrew_srv_01"));
        r.feed(music("Combat_Unknown") + music("Exploration") + music("NoTrack") + music("mainmenuX") + music("Main"));
        check(r.latch.scorpion() && r.changes.size() == 1, "any other music track, and a name that only starts like the main menu's, leave it alone");
        r.feed(music("mainmenu"));
        check(!r.latch.scorpion(), "the track name is compared case-insensitively too");
    }
    {   // fields that are not the field: a localised name, a missing field, a field too far on, a value that is only a prefix or has more
        LatchRun r;
        r.feed(line("LoadGame", "\"FID\":\"F1\", \"Ship_Localised\":\"combat_multicrew_srv_01\", \"ShipID\":4"));
        check(!r.latch.scorpion() && r.changes.empty(), "a LoadGame with only Ship_Localised (no Ship) is not the Scorpion");
        r.feed(launchSrv("combat_multicrew_srv_01"));
        r.feed(line("LaunchSRV", "\"SRVType_Localised\":\"combat_multicrew_srv_01\", \"ID\":1"));
        check(!r.latch.scorpion(), "a LaunchSRV with a localised name and no SRVType is unknown: the latch clears");
        r.feed(launchSrv("combat_multicrew_srv_01"));
        r.feed(line("LoadGame", "\"FID\":\"F1\", \"Commander\":\"X\", \"Horizons\":true"));
        check(!r.latch.scorpion(), "a LoadGame line that ends without its Ship clears the latch");
        r.feed(launchSrv("combat_multicrew_srv_01"));
        r.feed(line("LaunchSRV", "\"Pad\":\"" + std::string(600, 'x') + "\", \"SRVType\":\"combat_multicrew_srv_01\""));
        check(!r.latch.scorpion(), "a field further than 512 bytes from the event's name is out of reach: unknown");
        r.feed(launchSrv("combat_multicrew_srv_0"));
        check(!r.latch.scorpion(), "a value that is only a prefix of the Scorpion's is not the Scorpion");
        r.feed(launchSrv("combat_multicrew_srv_01"));
        r.feed(launchSrv("combat_multicrew_srv_011"));
        check(!r.latch.scorpion(), "a value with a character more is not the Scorpion");
        r.feed(launchSrv("combat_multicrew_srv_01"));
        r.feed(launchSrv("combat_multicrew_srv_0\\u0031"));
        check(!r.latch.scorpion(), "a value with an escape in it is not the Scorpion");
        r.feed(launchSrv("combat_multicrew_srv_01"));
        r.feed(line("Died", "\"SRVType\":\"testbuggy\"") + line("Touchdown", "\"Ship\":\"Anaconda\"") + line("Docked", "\"StationName\":\"X\""));
        check(r.latch.scorpion(), "events that are not the five (Died, Touchdown, Docked) are skipped, whatever fields they carry");
        r.feed("garbage \"event\":\"LoadGame\" without a newline and \"Ship\":\"Anaconda\"\n");
        check(!r.latch.scorpion(), "the event token is found wherever it stands in a line");
        r.feed("\"event\":\"LoadGame\", \"Ship\":\"Combat_Multicrew_SRV_01\"\r\n");
        check(r.latch.scorpion(), "a CRLF line ends like an LF line");
    }
    {   // the stream: a whole session fed in every split gives the same changes as fed whole
        const std::string session = line("Fileheader", "\"part\":1, \"language\":\"English\\\\UK\", \"gameversion\":\"4.0.0.1\"") + loadGame("Cobra") + launchSrv("testbuggy") +
                                    launchSrv("combat_multicrew_srv_01") + line("DockSRV", "\"ID\":1") + loadGame(kScorpion) + music("MainMenu") + loadGame("TestBuggy") + launchSrv("Combat_Multicrew_SRV_01") +
                                    line("Shutdown", "");
        LatchRun whole;
        whole.feed(session);
        check(whole.changes.size() == 6 && !whole.latch.scorpion(), "the sample session: set, cleared, set, cleared, set and cleared by the shutdown (six changes)");
        bool allSame = true;
        for (size_t cut = 0; cut <= session.size(); ++cut) {
            LatchRun split;
            split.feed(session.substr(0, cut));
            split.feed(session.substr(cut));
            allSame = allSame && split.changes == whole.changes;
        }
        check(allSame, "fed in two pieces cut at EVERY byte, the session gives the same changes (a read can end inside a name, a key, a value)");
        LatchRun bytes;
        for (char ch : session) bytes.feed(std::string(1, ch));
        check(bytes.changes == whole.changes, "...and fed one byte at a time too");
    }
    {   // a new file: the stream's place and the latch are forgotten
        LatchRun r;
        r.feed(launchSrv("combat_multicrew_srv_01"));
        check(r.latch.scorpion() && r.latch.reset() && !r.latch.scorpion(), "reset clears the latch and says it was set");
        check(!r.latch.reset(), "...and says nothing when it was not");
        r.feed("{ \"event\":\"LaunchSRV\", \"SRVType\":\"combat_mul");
        r.latch.reset();
        r.feed("ticrew_srv_01\" }\n");
        check(!r.latch.scorpion(), "a half-read line does not carry into the next file");
        r.feed(std::string("{ \"event\":\"LaunchSRV\", \"SRVType\":\"combat_multicrew_srv_01\" }\n"));
        check(r.latch.scorpion(), "...and the next file's lines are read afresh");
    }
    {   // privacy: of a line the latch keeps an event name and a bool, not the vehicle's name or anything else
        check(sizeof(jv::Latch) <= 40, "the latch is a few bytes of state (an event name of at most 15 bytes, a bool, counters): it has no room to keep a line or a value");
        check(!std::strcmp(jv::eventName(jv::Event::kLoadGame), "LoadGame") && !std::strcmp(jv::eventName(jv::Event::kLaunchSrv), "LaunchSRV") && !std::strcmp(jv::eventName(jv::Event::kMusic), "Music") &&
                  !std::strcmp(jv::eventName(jv::Event::kDockSrv), "DockSRV") && !std::strcmp(jv::eventName(jv::Event::kSrvDestroyed), "SRVDestroyed") &&
                  !std::strcmp(jv::eventName(jv::Event::kShutdown), "Shutdown") && !std::strcmp(jv::eventName(jv::Event::kNewJournal), "NewJournal"),
              "the log names the event that moved the latch, by the journal's own event names");
    }
}

// ---------------------------------------------------------------- the mutants
static std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
    size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}
static ComPtr<ID3D11GeometryShader> gsFromText(const std::string& text) {
    bool ok = false;
    const auto code = compileHlsl(text, "gs_5_0", &ok);
    check(ok, "a mutant compiles");
    ComPtr<ID3D11GeometryShader> gs;
    ck(g_dev->CreateGeometryShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &gs), "CreateGeometryShader (a mutant)");
    return gs;
}

#include "wiring_cases.h"

int main(int argc, char** argv) {
    try {
        if (argc == 2 && !std::strcmp(argv[1], "--dry-run")) {
            std::puts("srv_turret_hud_test: dry-run (no files, no device)");
            return 0;
        }
        if (argc == 2 && !std::strcmp(argv[1], "--wiring")) {
            wiringCases();
            std::printf("PASS srv_turret_hud_test --wiring: %u checks\n", g_checks);
            return 0;
        }
        check(argc == 2 && !std::strcmp(argv[1], "--self-test"), "usage: srv_turret_hud_test --self-test | --wiring | --dry-run");
        mathCases();
        latchCases();
        const HRESULT made = edvr::systemD3D11CreateDevice()(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
        if (FAILED(made))
            ck(edvr::systemD3D11CreateDevice()(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx), "device");
        g_dev.As(&g_messages);
        std::printf("srv_turret_hud_test: WARP%s\n", SUCCEEDED(made) ? " with the debug layer" : "");
        setupGamePair();

        // The shaders: the build's bytes are each HLSL text's compile, and the device takes them.
        ComPtr<ID3D11GeometryShader> gsOf[st::kSignatureCount];
        for (uint32_t fam = 0; fam < st::kSignatureCount; ++fam) {
            const auto compiled = compileHlsl(kBytes[fam].text, "gs_5_0");
            check(compiled->GetBufferSize() == kBytes[fam].size && !std::memcmp(compiled->GetBufferPointer(), kBytes[fam].data, kBytes[fam].size),
                  "the build's bytecode of each signature's shader is exactly the compile of srv_turret_hud_shader.h's text");
            ck(g_dev->CreateGeometryShader(kBytes[fam].data, kBytes[fam].size, nullptr, &gsOf[fam]), "the device takes the production shader");
        }

        constantsCases();
        Fails production;
        for (uint32_t fam = 0; fam < st::kSignatureCount; ++fam) {
            g_fam = int(fam);
            linkageCases(int(fam), gsOf[fam].Get());
            instrumentCases(int(fam), gsOf[fam].Get());
            geometryCases(gsOf[fam].Get(), production);
            pushCases(gsOf[fam].Get(), production);
            stripCases(gsOf[fam].Get(), production);
        }
        g_fam = 0;
        bindingCases(gsOf[0].Get(), production, true);
        afterIssueCases(gsOf[0].Get(), production, false);
        if (production.n) std::printf("first failing production check: %s (%u failed)\n", production.first.c_str(), production.n);
        check(production.n == 0, "the production shaders and binding pass every geometry, push, strip, binding and issue-end case, for all eight signatures");

        // THE MUTANTS: each through the same cases, each must fail at least one, in each signature's text.
        struct Mutant {
            const char* name;
            const char* from;
            const char* to;
        };
        const Mutant mutants[] = {
            {"no depth window", "if (w > wMin && w < wMax) o.pos.x +=", "o.pos.x +="},
            {"the sign flipped", "o.pos.x += s * gap / (w - gap)", "o.pos.x -= s * gap / (w - gap)"},
            {"the divisor w, not w - gap", "s * gap / (w - gap)", "s * gap / w"},
            {"the wrong axis", "o.pos.x += s * gap / (w - gap)", "o.pos.y += s * gap / (w - gap)"},
            {"the far end of the window ignored", "w < wMax", "w < 1000.0"},
            {"the near end of the window ignored", "w > wMin &&", "w > 0.0 &&"},
        };
        for (uint32_t fam = 0; fam < st::kSignatureCount; ++fam) {
            g_fam = int(fam);
            const std::string text = kBytes[fam].text;
            for (const Mutant& m : mutants) {
                const std::string mutated = replaceAll(text, m.from, m.to);
                check(mutated != text, "a mutant differs from the production text");
                const auto mg = gsFromText(mutated);
                Fails fails;
                geometryCases(mg.Get(), fails);
                pushCases(mg.Get(), fails);
                char what[200];
                std::snprintf(what, sizeof(what), "the mutant \"%s\" of signature %u's shader is caught by the geometry and push cases", m.name, fam);
                check(fails.n > 0, what);
            }
            {   // a pass-through that reverses every triangle's winding (what a shader that did not keep the input assembler's order would do to a strip): caught by the strip cases
                const std::string flipped = replaceAll(text, "Vertex o = v[i];", "Vertex o = v[(i == 0) ? 0 : (3 - i)];");
                check(flipped != text, "the winding mutant differs from the production text");
                const auto mg = gsFromText(flipped);
                Fails fails;
                stripCases(mg.Get(), fails);
                char what[200];
                std::snprintf(what, sizeof(what), "the mutant \"a pass-through that reverses the winding\" of signature %u's shader is caught by the strip cases", fam);
                check(fails.n > 0, what);
            }
        }
        g_fam = 0;
        {
            Fails fails;
            afterIssueCases(gsOf[0].Get(), fails, true);
            check(fails.n > 0, "the mutant \"a restore that does nothing\" is caught (the geometry stage stays bound)");
        }
        g_ctx->ClearState();

        if (g_messages) {
            for (UINT64 i = 0; i < g_messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                SIZE_T size = 0;
                g_messages->GetMessage(i, nullptr, &size);
                std::vector<char> data(size);
                auto* m = reinterpret_cast<D3D11_MESSAGE*>(data.data());
                ck(g_messages->GetMessage(i, m, &size), "debug message");
                if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
                    std::puts(m->pDescription);
                    check(false, "D3D debug layer");
                }
            }
        }
        wiringCases();
        std::printf("PASS srv_turret_hud_test: %u checks; the turret shaders, binding, constants, maths and Scorpion latch on WARP, six shader mutants (and the push's cases) in each of eight signatures and one binding mutant caught\n", g_checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL %s (%u checks)\n", e.what(), g_checks);
        return 1;
    }
}
