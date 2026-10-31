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
//   THE MOVE (srv_turret_math.h anchoredMove, which the shader is held to): the shader reconstructs a vertex's eye-space position from its clip position and the eye's tangents, translates
//   it by T (the HUD-forward key, every vertex), moves it along its ray from a fixed anchor A by `gap` metres (negative: deeper, the push-back) when its distance from A lies in (dMin, dMax),
//   and projects it back. Every case reads what the shader did to every vertex through the stream-output twin (the production bytecode with a declaration for its record and its own `pre`
//   output, the clip position as the game made it) and compares it with the CPU reference in doubles, and with the geometry itself: the eye-space point rebuilt from the clip position by the
//   camera's own projection, its distance from the anchor, its direction from it. (a) The anchor at the eye: a vertex at distance r keeps its NDC x, y and z and its w becomes w (r + d) / r.
//   (b) The anchor 10 cm to the side and 5 cm up, with vertices 15 cm from it. (c) Asymmetric tangents (the Pimax set, a vertical pair that is not symmetric): the move, and a gap of 10 um
//   round-trips every vertex. (d) A vertex at anchor distance 1.4 m, 2.8 m, 500 m, just past 1 m or inside 2 cm is untouched bit for bit. (e) A pull. (g) The translation alone: every vertex
//   moves by T, one 0.2 m behind the eye comes in front of it, one that stays behind moves too. (h) The translation, then the push. (k) The depth: Elite is reversed Z (clip z the constant 0.025), so a vertex behind the eye or inside the near plane gets the nearest depth, 0.999, and a valid one keeps its own; (l) a triangle with a corner moved behind the eye is output with all three corners moved; (m) the two groups in one frame: the panels' constants translate by the HUD-forward T and the radar's by its own key's. The pictures (I1 to I4) compare the rasterized quad with the
//   plain one (anchor at the eye) and with the quad truly moved in 3D. THE STRIPS: a quad as a triangle strip is the plain strip bit for bit through a window with nothing in it and no
//   translation, under every culling mode (the winding is kept), and lands where the same quad as a list lands once it is moved.
//   THE ANCHOR (srv_turret_math.h AnchorTracker, no device): followed for the settle after the view comes on or a camera key changes, then latched; a recentre (the head jumping more than
//   20 cm in a frame) latches at once; the view going off clears it. THE EYE'S FRAME: the host's 3x4 row-major matrices turned into the anchor and the translation in the eye's space,
//   checked on a level head, a yawed head and a canted eye.
//   THE LINKAGE. Each shader links against its stand-in under the debug layer with no message, its input is the vertex shader's output element for element and its output the same
//   again plus TURRETPRE, the pixel shader's inputs are a subset of it; a vertex shader without the texture coordinates is reported by the layer (the control that it really checks).
//   THE BINDING (the production class, on a real context): in and out for one draw with the game's geometry stage and constant slot exactly as they were and no reference leaked or
//   kept; a geometry shader of the game's own refused with nothing touched; the restore that fails twice, the settle at the boundary and the state the game rebound; the
//   geometry stage empty after the issue. THE CONSTANTS: four registers, made once per device, rewritten only when a number changes, refused when not finite, not a window, a gap of 0 with
//   no translation, a frustum that is none, or an anchor, translation or gap beyond 2 m; a push, a pull and a translation alone are accepted. THE GROUPS: every family has one, the two
//   hologram families are the panels, the other seven the radar; a group's push is used when above 0, else the gap, else the translation alone, else the group is off; B7790CBF is no family.
//   THE INSTRUMENT: the pipeline statistics (gs 0 for a plain draw, 2 for a quad through a shader), the stream-output twin of each signature (its record is the vertex whole, from the
//   signature table, then `pre`) and the capture summary: a quad 0.46 to 0.52 m from the anchor reads six vertices moved, the distance before and 1.2 m more after; one 1.5 m out and one at
//   500 m read in_window 0 with the picture unchanged; with the translation alone every vertex reads as moved.
//   THE SCORPION LATCH (journal_vehicle_latch.h, no device): LoadGame's Ship and LaunchSRV's SRVType set it for combat_multicrew_srv_01 in any case and clear it for anything
//   else; the main menu's music, DockSRV, SRVDestroyed and Shutdown clear it; a localised name, a missing, escaped, shorter, longer or too distant field is unknown, which is
//   false; a session fed in two pieces cut at every byte and byte by byte gives the same changes; a new file resets it; the state has no room for a line.
//   THE MUTANTS, each run through the same move cases in every signature's text and each caught: no window, either end of it, the sign of the anchor, of the gap and of the translation, y
//   read down, the tangents mixed, the translation dropped, the move about the origin, w or the NDC z not kept, the unmoved vertex returned when the move ends behind the eye, the depth fallback gone (z / w, the far end, or none inside the near plane), a `pre` that is the moved position,
//   and a pass-through that reverses the winding (caught by the strip cases); and the binding whose restore does nothing. --wiring scans the sources that connect all this to the game,
//   with controls that edit a copy and must trip the pin.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
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
// The push's: the shipped default of the panels' key (120 cm: the arcs just behind the location panel).
static const float kPushBackM = st::kPanelsDefaultCm / 100.0f;
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
    std::vector<std::string> kinds;   // every distinct message that failed (up to 24), so one run says all of what is wrong
    void expect(bool ok, const char* what) {
        ++g_checks;
        if (ok) return;
        if (!n) first = what;
        ++n;
        if (kinds.size() < 24 && std::find(kinds.begin(), kinds.end(), what) == kinds.end()) kinds.push_back(what);
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
           "    o.pos.z = i.t1.w != 0.0 ? i.t1.w : abs(o.pos.w) * 0.5 + 0.05 * abs(i.p.x);\n"
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

// ---------------------------------------------------------------- comparing frames
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

// ---------------------------------------------------------------- the camera as the shader's frame
// The stand-in vertex shader projects with Cam: clip x = P00 X + offX w, clip y = P11 Y + offY w, so NDC = P * tangent + off, and the tangents the shader is given are the ones that map
// them to -1 and +1. An EyeFrame of a camera, with an anchor and a translation given in the eye's space (X right, Y up, Z forward).
static Cam camOfTangents(float l, float r, float dn, float up) {
    Cam c;
    c.p00 = 2.0f / (r - l);
    c.offX = -(r + l) / (r - l);
    c.p11 = 2.0f / (up - dn);
    c.offY = -(up + dn) / (up - dn);
    return c;
}
static st::EyeFrame frameOfCam(const Cam& c, const float a[3] = nullptr, const float tr[3] = nullptr, const float trRadar[3] = nullptr) {
    st::EyeFrame f;
    f.l = (-1.0f - c.offX) / c.p00;
    f.r = (1.0f - c.offX) / c.p00;
    f.dn = (-1.0f - c.offY) / c.p11;
    f.up = (1.0f - c.offY) / c.p11;
    for (int i = 0; i < 3; ++i) {
        f.a[i] = a ? a[i] : 0.0f;
        f.t[i] = tr ? tr[i] : 0.0f;
        f.tRadar[i] = trRadar ? trRadar[i] : (tr ? tr[i] : 0.0f);   // the radar group translation: its own when given, else the same as the panels
    }
    return f;
}
// The number of pixels of two frames that differ by more than `threshold` in any channel (all of them when the frames are not the same size).
static size_t diffCount(const Frame& a, const Frame& b, double threshold) {
    if (a.rgba.size() != b.rgba.size() || a.rgba.empty()) return a.rgba.size() / 4 + 1;
    size_t differing = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4) {
        bool d = false;
        for (int c = 0; c < 4; ++c) d = d || std::fabs(double(a.rgba[i + c]) - b.rgba[i + c]) > threshold;
        differing += d ? 1 : 0;
    }
    return differing;
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
    check(sizeof(st::TurretParams) == 64 && st::kPreFloats == 4 && !std::strcmp(st::kPreSemantic, "TURRETPRE"),
          "the constants are four registers (64 bytes); the shader's own `pre` output is four floats named TURRETPRE");
    {   // the eye's frame from the host's pose: the anchor and the HUD-forward translation in the shader's axes (X right, Y up, Z forward)
        const float tn[4] = {-1.0f, 1.2f, -0.9f, 1.1f};
        const float level[12] = {1, 0, 0, 0.1f, 0, 1, 0, 0.2f, 0, 0, 1, 0.3f}, leftEye[12] = {1, 0, 0, -0.03f, 0, 1, 0, 0, 0, 0, 1, 0.01f};
        const double atHead[3] = {0.1, 0.2, 0.3};
        st::EyeFrame e;
        check(st::eyeFrameFrom(level, leftEye, atHead, 0.7, 0.4, tn, e), "an eye frame is built from a pose");
        check(std::fabs(e.a[0] - 0.03f) < 1e-6f && std::fabs(e.a[1]) < 1e-6f && std::fabs(e.a[2] - 0.01f) < 1e-6f && std::fabs(e.t[0]) < 1e-6f && std::fabs(e.t[1]) < 1e-6f &&
                  std::fabs(e.t[2] - 0.7f) < 1e-6f && std::fabs(e.tRadar[0]) < 1e-6f && std::fabs(e.tRadar[1]) < 1e-6f && std::fabs(e.tRadar[2] - 0.4f) < 1e-6f && e.l == -1.0f && e.r == 1.2f && e.dn == -0.9f && e.up == 1.1f,
              "level head, anchor at the head: the left eye (3 cm left of the head and 1 cm back of it) sees the anchor 3 cm to its right and 1 cm AHEAD, HUD forward 0.7 m is +0.7 along Z and the radar's 0.4 m is +0.4");
        const double c = std::cos(0.5235987755982988), s = std::sin(0.5235987755982988);   // the head yawed 30 degrees about +y
        const float yawed[12] = {float(c), 0, float(s), 0.1f, 0, 1, 0, 0.2f, float(-s), 0, float(c), 0.3f};
        const double anchor2[3] = {0.1 + 0.1, 0.2, 0.3 - 0.2};   // the head's place + (0.1, 0, -0.2) in the seated axes
        st::EyeFrame y;
        check(st::eyeFrameFrom(yawed, leftEye, anchor2, 0.7, 0.4, tn, y), "an eye frame is built from a yawed pose");
        check(std::fabs(y.a[0] - (0.1 * c + 0.2 * s + 0.03)) < 1e-6 && std::fabs(y.a[1]) < 1e-6 && std::fabs(y.a[2] - (0.2 * c - 0.1 * s + 0.01)) < 1e-6 &&
                  std::fabs(y.t[0] - 0.7 * s) < 1e-6 && std::fabs(y.t[1]) < 1e-6 && std::fabs(y.t[2] - 0.7 * c) < 1e-6 &&
                  std::fabs(y.tRadar[0] - 0.4 * s) < 1e-6 && std::fabs(y.tRadar[1]) < 1e-6 && std::fabs(y.tRadar[2] - 0.4 * c) < 1e-6,
              "the head yawed 30 degrees: the anchor and the seated -z forward are turned into the eye's axes (R^T (A - E), then z negated)");
        // a canted eye (yawed 5 degrees and pitched 3 against the head) keeps lengths: |A_eye| is the distance from the eye to the anchor, |T| the forward distance
        const double cy = std::cos(0.0872664626), sy = std::sin(0.0872664626), cp = std::cos(0.05235987756), sp = std::sin(0.05235987756);
        const float canted[12] = {float(cy), float(sy * sp), float(sy * cp), 0.032f, 0, float(cp), float(-sp), -0.004f, float(-sy), float(cy * sp), float(cy * cp), 0.012f};
        st::EyeFrame k;
        check(st::eyeFrameFrom(yawed, canted, anchor2, 0.7, 0.4, tn, k), "an eye frame is built from a canted eye");
        const double ex = 0.1 + c * 0.032 + s * 0.012, ey = 0.2 - 0.004, ez = 0.3 + (-s) * 0.032 + c * 0.012;   // E = head + R_h t_e
        const double dx = anchor2[0] - ex, dy = anchor2[1] - ey, dz = anchor2[2] - ez;
        check(std::fabs(std::sqrt(double(k.a[0]) * k.a[0] + double(k.a[1]) * k.a[1] + double(k.a[2]) * k.a[2]) - std::sqrt(dx * dx + dy * dy + dz * dz)) < 1e-6 &&
                  std::fabs(std::sqrt(double(k.t[0]) * k.t[0] + double(k.t[1]) * k.t[1] + double(k.t[2]) * k.t[2]) - 0.7) < 1e-6 &&
                  std::fabs(std::sqrt(double(k.tRadar[0]) * k.tRadar[0] + double(k.tRadar[1]) * k.tRadar[1] + double(k.tRadar[2]) * k.tRadar[2]) - 0.4) < 1e-6,
              "...and a canted eye's frame keeps the distance to the anchor and the length of the forward translation");
        float bad[12];
        std::memcpy(bad, level, sizeof(bad));
        bad[3] = NAN;
        const float noFrustum[4] = {1.0f, 1.0f, -1.0f, 1.0f};
        check(!st::eyeFrameFrom(bad, leftEye, atHead, 0.7, 0.4, tn, e) && !st::eyeFrameFrom(level, leftEye, atHead, 0.7, 0.4, noFrustum, e) && !st::eyeFrameFrom(level, leftEye, atHead, 0.7, NAN, tn, e), "a pose that is not finite, or tangents that are no frustum, give no frame");
    }
    {   // the anchor: followed for the settle, then latched; taken again on a key change, a recentre; cleared with the view
        st::AnchorTracker tr;
        double h[3] = {0.10, 0.20, 0.30};
        check(tr.step(h, false, 0, 70) == st::AnchorReason::kNone && !tr.valid(), "the view off: no anchor");
        bool quiet = true;
        for (uint32_t i = 0; i < st::kAnchorSettleFrames; ++i) {   // the first of the 20 has no previous frame and is not counted
            h[0] = 0.10 + 0.001 * i;
            quiet = quiet && tr.step(h, true, 0, 70) == st::AnchorReason::kNone && tr.valid() && tr.settling() && tr.anchor()[0] == h[0];
        }
        check(quiet, "the view comes on: for the settle the anchor FOLLOWS the head (the pass already works, as the old per-eye shift) and nothing is said");
        h[0] = 0.15;
        check(tr.step(h, true, 0, 70) == st::AnchorReason::kViewOn && !tr.settling() && tr.anchor()[0] == 0.15, "...and at the end of it the anchor is latched at the head, reason 'view on'");
        h[0] = 0.19;
        h[2] = 0.33;
        check(tr.step(h, true, 0, 70) == st::AnchorReason::kNone && tr.anchor()[0] == 0.15 && tr.anchor()[2] == 0.30, "a head that moves 4 or 5 cm leaves the anchor where it was");
        h[0] = 0.15;
        h[2] = 0.30;
        (void)tr.step(h, true, 0, 70);
        bool settled = true;
        st::AnchorReason got = st::AnchorReason::kNone;
        for (uint32_t i = 0; i < st::kAnchorSettleFrames; ++i) {
            h[1] = 0.20 + 0.0005 * (i + 1);
            const st::AnchorReason r = tr.step(h, true, 60, 70);   // the camera key changed
            settled = settled && tr.valid() && (i + 1 == st::kAnchorSettleFrames ? r == st::AnchorReason::kCameraBack : r == st::AnchorReason::kNone);
            if (r != st::AnchorReason::kNone) got = r;
        }
        check(settled && got == st::AnchorReason::kCameraBack && tr.anchor()[1] == h[1], "the camera key changes: the anchor follows for the settle and is latched again, reason 'camera back changed'");
        got = st::AnchorReason::kNone;
        for (uint32_t i = 0; i < st::kAnchorSettleFrames; ++i) {
            h[1] += 0.0005;
            const st::AnchorReason r = tr.step(h, true, 60, 40);   // the HUD-forward key changed
            if (r != st::AnchorReason::kNone) got = r;
        }
        check(got == st::AnchorReason::kHudForward, "the HUD-forward key changes: latched again, reason 'HUD forward changed'");
        got = st::AnchorReason::kNone;
        for (uint32_t i = 0; i < st::kAnchorSettleFrames; ++i) {
            h[1] += 0.0005;
            const st::AnchorReason r = tr.step(h, true, 60, 40, 50);   // the radar-forward key changed
            if (r != st::AnchorReason::kNone) got = r;
        }
        check(got == st::AnchorReason::kRadarForward, "the radar-forward key changes: latched again, reason 'radar forward changed'");
        for (uint32_t i = 0; i < st::kAnchorSettleFrames; ++i) {   // (the keys the later cases pass: radar forward back to 0)
            h[1] += 0.0005;
            (void)tr.step(h, true, 60, 40);
        }
        h[0] += 0.15;
        check(tr.step(h, true, 60, 40) == st::AnchorReason::kNone && tr.anchor()[0] != h[0], "a head jump of 15 cm in one frame is no recentre");
        const double before = tr.anchor()[0];
        h[0] += 0.25;
        check(tr.step(h, true, 60, 40) == st::AnchorReason::kRecentre && tr.anchor()[0] == h[0] && tr.anchor()[0] != before && !tr.settling(),
              "a head jump of 25 cm in one frame (a recentre) latches the anchor at once, reason 'recentre'");
        double notFinite[3] = {NAN, 0, 0};
        const double keep = tr.anchor()[0];
        check(tr.step(notFinite, true, 60, 40) == st::AnchorReason::kNone && tr.anchor()[0] == keep, "a head that is not finite is ignored");
        (void)tr.step(h, false, 60, 40);
        check(!tr.valid(), "the view going off clears the anchor");
        check(!std::strcmp(st::anchorReasonName(st::AnchorReason::kViewOn), "view on") && !std::strcmp(st::anchorReasonName(st::AnchorReason::kCameraBack), "camera back changed") &&
                  !std::strcmp(st::anchorReasonName(st::AnchorReason::kHudForward), "HUD forward changed") && !std::strcmp(st::anchorReasonName(st::AnchorReason::kRecentre), "recentre") &&
                  !std::strcmp(st::anchorReasonName(st::AnchorReason::kHeadAway), "head away") && !std::strcmp(st::anchorReasonName(st::AnchorReason::kRadarForward), "radar forward changed"),
              "the reasons the log gives");
    }
    {   // a FROZEN pose (the flight of b5c49237: the headset's pose was the same bits from the launch to the turret view and on, then live): nothing latches on it
        const double frozen[3] = {-0.415, -0.366, -0.682};
        st::AnchorTracker tr;
        bool quiet = true;
        for (int i = 0; i < 200; ++i) quiet = quiet && tr.step(frozen, true, 0, 70) == st::AnchorReason::kNone && tr.valid() && tr.settling() && tr.anchor()[0] == frozen[0] && tr.anchor()[2] == frozen[2];
        check(quiet, "a pose that does not change at all for 200 frames never latches: the anchor follows the head and the latch stays pending");
        double h[3] = {frozen[0], frozen[1], frozen[2]};
        bool early = true;
        st::AnchorReason r = st::AnchorReason::kNone;
        for (int i = 1; i <= static_cast<int>(st::kAnchorSettleFrames); ++i) {   // the pose comes back gradually, 5 mm a frame
            h[0] = frozen[0] + 0.005 * i;
            r = tr.step(h, true, 0, 70);
            early = early && (i < static_cast<int>(st::kAnchorSettleFrames) ? r == st::AnchorReason::kNone : r == st::AnchorReason::kViewOn);
        }
        check(early && tr.anchor()[0] == h[0] && !tr.settling(), "...and once the pose is live the anchor is latched on the live head after 20 moving frames, reason 'view on'");
        st::AnchorTracker jump;
        for (int i = 0; i < 200; ++i) (void)jump.step(frozen, true, 0, 70);
        double live[3] = {frozen[0] + 1.0, frozen[1], frozen[2]};   // the pose goes live a metre away in one step
        check(jump.step(live, true, 0, 70) == st::AnchorReason::kRecentre && jump.anchor()[0] == live[0] && !jump.settling(),
              "...and a live pose a metre away in one step is a recentre: latched on the live head at once");
    }
    {   // the head away from the latched anchor: after 90 frames in a row beyond 35 cm the anchor is taken again; at 30 cm, never
        st::AnchorTracker tr;
        double h[3] = {0.0, 0.0, 0.0};
        for (int i = 0; i <= static_cast<int>(st::kAnchorSettleFrames); ++i) {   // latch near the origin
            h[0] = 0.0001 * i;
            (void)tr.step(h, true, 0, 70);
        }
        const double a0 = tr.anchor()[0];
        bool quiet = !tr.settling();
        for (int k = 1; k <= 3; ++k) {   // 10, 20 and 30 cm: steps under the recentre jump
            h[0] = a0 + 0.1 * k;
            quiet = quiet && tr.step(h, true, 0, 70) == st::AnchorReason::kNone;
        }
        h[0] = a0 + 0.4;   // 40 cm: the first frame beyond 35 cm
        quiet = quiet && tr.step(h, true, 0, 70) == st::AnchorReason::kNone;
        h[0] = a0 + 0.5;
        for (uint32_t i = 2; i < st::kAwayFrames; ++i) quiet = quiet && tr.step(h, true, 0, 70) == st::AnchorReason::kNone;   // frames 2 to 89 beyond 35 cm
        check(quiet && tr.anchor()[0] == a0, "the head 0.5 m from the latched anchor: nothing latches on frame 89 of being beyond 35 cm");
        check(tr.step(h, true, 0, 70) == st::AnchorReason::kHeadAway && tr.anchor()[0] == h[0], "...and on frame 90 the anchor is taken again at the head, reason 'head away'");
        check(tr.step(h, true, 0, 70) == st::AnchorReason::kNone && !tr.settling(), "...once (the head is on the anchor now)");
        st::AnchorTracker near30;
        double g[3] = {0.0, 0.0, 0.0};
        for (int i = 0; i <= static_cast<int>(st::kAnchorSettleFrames); ++i) {
            g[0] = 0.0001 * i;
            (void)near30.step(g, true, 0, 70);
        }
        const double b0 = near30.anchor()[0];
        bool still = true;
        for (int k = 1; k <= 3; ++k) {
            g[0] = b0 + 0.1 * k;
            still = still && near30.step(g, true, 0, 70) == st::AnchorReason::kNone;
        }
        for (int i = 0; i < 1000; ++i) still = still && near30.step(g, true, 0, 70) == st::AnchorReason::kNone;
        check(still && near30.anchor()[0] == b0, "the head 30 cm from the anchor for 1000 frames: no re-latch (leaning keeps the real-panel behaviour)");
        st::AnchorTracker back;   // 35 cm beyond and then back inside resets the count
        double q[3] = {0.0, 0.0, 0.0};
        for (int i = 0; i <= static_cast<int>(st::kAnchorSettleFrames); ++i) {
            q[0] = 0.0001 * i;
            (void)back.step(q, true, 0, 70);
        }
        const double c0 = back.anchor()[0];
        bool counted = true;
        for (int round = 0; round < 3; ++round) {
            for (int k = 1; k <= 4; ++k) {
                q[0] = c0 + 0.1 * k;
                counted = counted && back.step(q, true, 0, 70) == st::AnchorReason::kNone;
            }
            for (int i = 0; i < 60; ++i) counted = counted && back.step(q, true, 0, 70) == st::AnchorReason::kNone;
            for (int k = 3; k >= 0; --k) {
                q[0] = c0 + 0.1 * k;
                counted = counted && back.step(q, true, 0, 70) == st::AnchorReason::kNone;
            }
        }
        check(counted && back.anchor()[0] == c0, "frames beyond 35 cm that are not in a row (60, back inside, 60, ...) never reach 90: the count restarts");
        st::AnchorTracker slow;   // the safety net: a head that walks away slowly (1 cm a frame, no jump) from an anchor latched near it
        double w[3] = {0.0, 0.0, 0.0};
        for (int i = 0; i <= static_cast<int>(st::kAnchorSettleFrames); ++i) {
            w[0] = 0.0001 * i;
            (void)slow.step(w, true, 0, 70);
        }
        int away = 0;
        for (int i = 1; i <= 200; ++i) {
            w[0] = std::min(1.0, 0.01 * i);
            if (slow.step(w, true, 0, 70) == st::AnchorReason::kHeadAway) ++away;
        }
        const double gap = std::fabs(w[0] - slow.anchor()[0]);
        check(away >= 1 && gap <= st::kAwayM && !slow.settling(), "a head that walks a metre away slowly gets its anchor back within 35 cm of it, by 'head away'");
    }
    {   // the move on the CPU (doubles): the properties the shader must have, before the shader is asked
        const st::EyeFrame e0 = frameOfCam(Cam{});
        const st::TurretParams push = st::pushParamsFor(e0, 1.2f);
        bool ok = true;
        const double dirs[3][3] = {{0.0, 0.0, 1.0}, {0.2, 0.1, 1.0}, {-0.3, 0.25, 1.0}};
        for (const double r : {0.15, 0.3, 0.6, 0.95})
            for (const auto& dir : dirs) {
                const double n = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                const Cam cm;
                const double X = r * dir[0] / n, Y = r * dir[1] / n, D = r * dir[2] / n;
                const double in[4] = {cm.p00 * X + cm.offX * D, cm.p11 * Y + cm.offY * D, 0.5 * D, D};
                double out[4], before = 0, after = 0;
                const bool moved = st::anchoredMove<double>(push, in, out, &before, &after);
                ok = ok && moved && std::fabs(out[0] / out[3] - in[0] / in[3]) < 1e-6 && std::fabs(out[1] / out[3] - in[1] / in[3]) < 1e-6 && std::fabs(out[3] - D * (r + 1.2) / r) < 1e-6 &&
                     std::fabs(out[2] / out[3] - in[2] / in[3]) < 1e-9 && std::fabs(before - r) < 1e-6 && std::fabs(after - (r + 1.2)) < 1e-6;
            }
        check(ok, "anchor at the eye: a vertex at distance r keeps its NDC x, y and z, and its w becomes w (r + d) / r (the push keeps the size on the screen)");
        const double distant[4] = {0.3, 0.2, 250.0, 500.0}, zeroClip[4] = {0.0, 0.0, 0.0, 0.0}, eyePlane[4] = {0.1, 0.1, 0.0, 1.0e-9};
        double out[4];
        check(!st::anchoredMove<double>(push, distant, out) && out[3] == 500.0 && !st::anchoredMove<double>(push, zeroClip, out) && !st::anchoredMove<double>(push, eyePlane, out),
              "a vertex 500 m out, one at clip w 0 and one on the eye plane are left alone");
        // the depth: Elite is reversed Z with the constant clip z 0.025, NDC z = 0.025 / w
        check(st::ndcDepthValid<double>(0.025, 0.24) && st::ndcDepthValid<double>(0.0, 1.0) && st::ndcDepthValid<double>(1.0, 1.0) && st::ndcDepthValid<double>(0.025, 25.0) &&
                  !st::ndcDepthValid<double>(0.025, -0.2) && !st::ndcDepthValid<double>(0.025, 0.01) && !st::ndcDepthValid<double>(1.1, 1.0) && !st::ndcDepthValid<double>(-0.1, 1.0) &&
                  !st::ndcDepthValid<double>(0.5, 0.0) && st::kFallbackNdcZ == 0.999f,
              "an NDC depth is valid for w above 0 and 0 <= z / w <= 1 (Elite's 0.025 / w is, from w = 0.025 up); behind the eye or inside the near plane it is not, and the fallback is 0.999");
        {
            const float tz[3] = {0.0f, 0.0f, 0.7f};
            const st::TurretParams tp = st::translateParamsFor(frameOfCam(Cam{}, nullptr, tz));
            const double behindEye[4] = {0.0, 0.0, 0.025, -0.2}, inFront[4] = {0.0, 0.0, 0.025, 0.24}, nearPlane[4] = {0.0, 0.0, 0.025, 0.01};
            double o1[4], o2[4], o3[4];
            bool z1 = false, z2 = true, z3 = false;
            const bool m1 = st::anchoredMove<double>(tp, behindEye, o1, nullptr, nullptr, &z1), m2 = st::anchoredMove<double>(tp, inFront, o2, nullptr, nullptr, &z2),
                       m3 = st::anchoredMove<double>(tp, nearPlane, o3, nullptr, nullptr, &z3);
            check(m1 && m2 && m3 && z1 && !z2 && z3 && std::fabs(o1[2] / o1[3] - 0.999) < 1e-6 && std::fabs(o2[2] / o2[3] - 0.025 / 0.24) < 1e-9 && std::fabs(o3[2] / o3[3] - 0.999) < 1e-6 &&
                      std::fabs(o1[3] - 0.5) < 1e-6 && std::fabs(o2[3] - 0.94) < 1e-6 && std::fabs(o3[3] - 0.71) < 1e-6,
                  "the CPU reference: a vertex at w -0.20 (z 0.025) moved by 0.7 has z/w 0.999 and is on the fallback, one at 0.24 keeps 0.025/0.24, one at 0.01 (NDC z 2.5) gets 0.999");
            const float back[3] = {0.0f, 0.0f, -0.7f};
            const st::TurretParams bp = st::translateParamsFor(frameOfCam(Cam{}, nullptr, back));
            const double ahead[4] = {0.0, 0.0, 0.25, 0.5};
            double o4[4];
            check(st::anchoredMove<double>(bp, ahead, o4) && std::fabs(o4[3] + 0.2) < 1e-6, "...and a vertex the move puts behind the eye is moved all the same (w -0.2), not left as it was");
        }
    }
    {   // which constants a group's draws run with: the push when above 0, else the pull when the gap is above 0, else the translation alone when the HUD-forward key is above 0, else none
        const Cam cm;
        const float a[3] = {0.1f, 0.05f, 0.0f}, tr[3] = {0.0f, 0.0f, 0.7f};
        const st::EyeFrame e = frameOfCam(cm, a, tr);
        st::TurretParams out{};
        const st::Group P = st::Group::kPanels, R = st::Group::kRadar;
        check(st::paramsForGroup(e, P, 120, 0, 0, out) && out == st::pushParamsFor(e, 1.2f), "a push above 0 and the rest at 0: the push");
        check(st::paramsForGroup(e, P, 120, 96, 70, out) && out == st::pushParamsFor(e, 1.2f), "...and with the gap and the HUD-forward key above 0 too the push is used, not the pull");
        check(st::paramsForGroup(e, P, 0, 96, 70, out) && out == st::paramsFor(e, 0.96f), "no push and the gap above 0: the pull");
        check(st::paramsForGroup(e, P, 0, 0, 70, out) && out == st::translateParamsFor(e) && out.gap == 0.0f, "no push and no gap but HUD forward: the translation alone (gap 0)");
        check(!st::paramsForGroup(e, P, 0, 0, 0, out), "all three at 0: no constants (the draw is the game's own)");
        check(st::paramsForGroup(e, P, 999, 0, 0, out) && out == st::pushParamsFor(e, 2.0f) && st::paramsForGroup(e, P, 0, 999, 0, out) && out == st::paramsFor(e, 2.0f), "a key past its range is held to 200 cm");
        {   // the radar group carries its own translation: the same push, the same frame, a different T
            const float tp[3] = {0.0f, 0.0f, 0.7f}, trd[3] = {0.0f, 0.0f, 0.3f};
            const st::EyeFrame two = frameOfCam(cm, a, tp, trd);
            st::TurretParams panelsP, radarP;
            check(st::paramsForGroup(two, P, 120, 0, 70, panelsP) && st::paramsForGroup(two, R, 120, 0, 30, radarP) && panelsP.tz == 0.7f && radarP.tz == 0.3f && panelsP.gap == radarP.gap &&
                      panelsP.ax == radarP.ax && panelsP.l == radarP.l && panelsP == st::pushParamsFor(two, 1.2f, P) && radarP == st::pushParamsFor(two, 1.2f, R),
                  "one eye frame, two groups: the panels' constants carry the HUD-forward translation, the radar's its own");
            check(st::paramsForGroup(two, R, 0, 0, 30, radarP) && radarP == st::translateParamsFor(two, R) && radarP.tz == 0.3f && !st::paramsForGroup(two, R, 0, 0, 0, radarP) &&
                      st::forwardCmOf(P, 70, 30) == 70 && st::forwardCmOf(R, 70, 30) == 30,
                  "the radar alone with its forward key: the translation alone, by the radar's key; the key at 0: none; each group reads its own forward key");
        }
        check(st::pushCmOf(st::Group::kPanels, 11, 22) == 11 && st::pushCmOf(st::Group::kRadar, 11, 22) == 22, "a group reads its own push key");
        check(!std::strcmp(st::groupName(st::Group::kPanels), "panels") && !std::strcmp(st::groupName(st::Group::kRadar), "radar"), "the groups are named panels and radar");
        // the validity test the constants apply (srv_turret_math.h paramsUsable)
        check(st::paramsUsable(st::pushParamsFor(e, 1.2f)) && st::paramsUsable(st::pushParamsFor(e, 2.0f)) && st::paramsUsable(st::paramsFor(e, 0.96f)) && st::paramsUsable(st::translateParamsFor(e)),
              "a push (also of 2 m), a pull and a translation alone are usable");
        const st::EyeFrame still = frameOfCam(cm);
        st::TurretParams bad = st::pushParamsFor(e, 1.2f);
        bool refused = !st::paramsUsable(st::translateParamsFor(still));   // gap 0 and no translation: it does nothing
        bad = st::pushParamsFor(e, 2.5f);
        refused = refused && !st::paramsUsable(bad);
        bad = st::pushParamsFor(e, 1.2f);
        bad.dMin = 0.0f;
        refused = refused && !st::paramsUsable(bad);
        bad = st::paramsFor(e, 0.96f);
        bad.dMin = 0.96f;
        refused = refused && !st::paramsUsable(bad);
        bad = st::pushParamsFor(e, 1.2f);
        bad.dMax = bad.dMin;
        refused = refused && !st::paramsUsable(bad);
        bad = st::pushParamsFor(e, 1.2f);
        bad.r = bad.l;
        refused = refused && !st::paramsUsable(bad);
        bad = st::pushParamsFor(e, 1.2f);
        bad.up = bad.dn;
        refused = refused && !st::paramsUsable(bad);
        bad = st::pushParamsFor(e, 1.2f);
        bad.ax = 2.0f;
        refused = refused && !st::paramsUsable(bad);
        bad = st::pushParamsFor(e, 1.2f);
        bad.tz = 2.5f;
        refused = refused && !st::paramsUsable(bad);
        bad = st::pushParamsFor(e, 1.2f);
        bad.ay = NAN;
        refused = refused && !st::paramsUsable(bad);
        check(refused, "constants that do nothing, a gap beyond 2 m, a window starting at 0 or at the gap, an empty one, a frustum that is none, an anchor or translation 2 m away and a number that is not finite are refused");
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
        const uint32_t expectBytes[8] = {60, 56, 40, 68, 60, 52, 44, 52}, expectPos[8] = {7, 6, 2, 9, 7, 5, 3, 5};
        bool ok = true;
        for (uint32_t s = 0; s < 8; ++s)
            ok = ok && (st::sigFloats(s) + st::kPreFloats) * 4 == expectBytes[s] && st::sigPosFloat(s) == expectPos[s] && st::sigPosFloat(s) * 4 + 16 + 16 == expectBytes[s];
        for (uint32_t i = 0; i < 9; ++i)
            ok = ok && st::kFamilies[i].strideBytes == expectBytes[st::kFamilies[i].sig] && st::kFamilies[i].posFloat == expectPos[st::kFamilies[i].sig] &&
                 st::kFamilies[i].preFloat == st::sigFloats(st::kFamilies[i].sig);
        check(ok, "the stream-output records: each signature's floats in order, the clip position last, then the shader's `pre` (60, 56, 40, 68, 60, 52, 44 and 52 bytes), and each family's are its signature's");
    }
    {   // the topologies: lists and strips, nothing else
        static_assert(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST == 4 && D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP == 5, "the topology constants srv_turret_math.h carries are D3D11's");
        bool ok = st::topologyCarried(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) && st::topologyCarried(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        for (int t : {int(D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED), int(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST), int(D3D11_PRIMITIVE_TOPOLOGY_LINELIST), int(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP),
                      int(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ), int(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ), int(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST), -1, 99})
            ok = ok && !st::topologyCarried(t);
        check(ok, "triangle lists and triangle strips are carried; points, lines, adjacency, patches and anything else are refused");
    }
    check(st::kBackDefaultCm == 0 && st::kForwardDefaultCm == 70 && st::kForwardMaxCm == 150 && st::kPanelsDefaultCm == 120 && st::kRadarDefaultCm == 0 && st::kGapDefaultCm == 0 &&
              st::kPushMaxCm == 200 && st::kGapMaxCm == 200,
          "the keys' defaults: camera back 0 (the stock camera), HUD forward 70, panels back 120, radar back 0, gap 0");
}

// ---------------------------------------------------------------- the move, through the stream-output twin of the production shader and of each mutant
// The twin is the production bytecode with a stream-output declaration (the moved record, then `pre`: the clip position as the game made it), rasterized as well, so what the shader did
// to every vertex is read back exactly. A point is given in the eye's space (X right, Y up, d the distance forward, which is the clip w; negative behind the eye); the stand-in vertex
// shader projects it with Cam. The CPU reference (srv_turret_math.h anchoredMove, in doubles) is the expected result of every vertex; the geometry checks beside it do not use it: they
// rebuild the eye-space point from the clip position with the camera's own projection and test distances and directions.
struct Pt {
    float x, y, d;
    float zc = 0.0f;   // the clip z the game's vertex shader gives it (Elite: the constant 0.025, cb0[6] = (0, 0, 0, 0.025)); 0: the stand-in's own, 0.5 w + 0.05 |X|
};
static Vtx vtxOf(const Pt& p, float u = 0.0f, float v = 0.0f) { return Vtx{{p.x, p.y, -p.d, 1.0f}, {u, v, 0.0f, 1.0f}, {0.25f, 0.5f, 0.75f, p.zc}}; }
static Pt dirPt(float dx, float dy, float dz, float r) {
    const float n = std::sqrt(dx * dx + dy * dy + dz * dz);
    return Pt{r * dx / n, r * dy / n, r * dz / n};
}
// The point at distance r from `from` in the direction (dx, dy, dz).
static Pt aroundPt(const float from[3], float dx, float dy, float dz, float r) {
    const Pt o = dirPt(dx, dy, dz, r);
    return Pt{from[0] + o.x, from[1] + o.y, from[2] + o.d};
}
// A quad (two triangles, the texture coordinate u, v) through four eye-space corners: (x0, y0), (x1, y0), (x0, y1), (x1, y1).
static void addQuadPts(std::vector<Vtx>& v, const Pt c[4]) {
    v.push_back(vtxOf(c[0], 0, 0));
    v.push_back(vtxOf(c[1], 1, 0));
    v.push_back(vtxOf(c[2], 0, 1));
    v.push_back(vtxOf(c[1], 1, 0));
    v.push_back(vtxOf(c[3], 1, 1));
    v.push_back(vtxOf(c[2], 0, 1));
}
static void boxAt(Pt c[4], float cx, float cy, float d, float hx, float hy, float zc = 0.0f) {
    c[0] = Pt{cx - hx, cy - hy, d, zc};
    c[1] = Pt{cx + hx, cy - hy, d, zc};
    c[2] = Pt{cx - hx, cy + hy, d, zc};
    c[3] = Pt{cx + hx, cy + hy, d, zc};
}
// The rule written a third time, on eye-space points in doubles: translate by T, move along the ray from A by `gap` when the translated point's distance from A lies in (dMin, dMax).
static Pt movedPt(const Pt& p, const float a[3], const float t[3], float gap, float dMin, float dMax) {
    const double P1[3] = {double(p.x) + t[0], double(p.y) + t[1], double(p.d) + t[2]};
    const double V[3] = {P1[0] - a[0], P1[1] - a[1], P1[2] - a[2]};
    const double dist = std::sqrt(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]);
    double Q[3] = {P1[0], P1[1], P1[2]};
    if (dist > dMin && dist < dMax)
        for (int i = 0; i < 3; ++i) Q[i] = a[i] + V[i] * (dist - gap) / dist;
    if (!(Q[2] > 0.01)) return p;
    return Pt{float(Q[0]), float(Q[1]), float(Q[2])};
}

struct Captured {
    std::vector<std::array<float, 4>> pre, post;
};
// The points as triangles through the twin with the stream-output target: the clip position of every vertex as the game's shader made it (pre) and as the geometry shader wrote it (post).
static Captured captureMove(int fam, ID3D11GeometryShader* twin, const std::vector<Pt>& pts, const Cam& cam, const st::TurretParams& params) {
    std::vector<Vtx> v;
    for (const Pt& p : pts) v.push_back(vtxOf(p));
    while (v.size() % 3) v.push_back(v.back());
    Sample s;
    DrawOpts o;
    o.sample = &s;
    o.soTarget = true;
    (void)render(v, cam, twin, params, 64, 64, o);
    check(s.so.NumPrimitivesWritten == v.size() / 3 && s.so.PrimitivesStorageNeeded == v.size() / 3, "the twin wrote one triangle for each triangle it was given");
    const uint32_t stride = st::sigFloats(fam) + st::kPreFloats, pos = st::sigPosFloat(fam), pre = st::sigFloats(fam);
    Captured m;
    for (size_t i = 0; i < pts.size(); ++i) {
        std::array<float, 4> a{}, b{};
        for (int c = 0; c < 4; ++c) {
            a[c] = s.records[i * stride + pre + c];
            b[c] = s.records[i * stride + pos + c];
        }
        m.pre.push_back(a);
        m.post.push_back(b);
    }
    return m;
}
static bool closeTo(double a, double b, double tol) { return std::fabs(a - b) <= tol * std::max(1.0, std::max(std::fabs(a), std::fabs(b))); }
static void eyePos(const std::array<float, 4>& c, const Cam& cm, double p[3]) {   // the eye-space point behind a clip position, by the camera's own projection (not the shader's tangents)
    const double w = c[3];
    p[0] = (c[0] - double(cm.offX) * w) / cm.p00;
    p[1] = (c[1] - double(cm.offY) * w) / cm.p11;
    p[2] = w;
}
static double len3(const double v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static bool sameRay(const double a[3], const double b[3]) {   // the same direction from the anchor
    const double cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cz = a[0] * b[1] - a[1] * b[0];
    const double la = len3(a), lb = len3(b);
    return la > 0.0 && lb > 0.0 && std::sqrt(cx * cx + cy * cy + cz * cz) / (la * lb) < 3.0e-4 && a[0] * b[0] + a[1] * b[1] + a[2] * b[2] > 0.0;
}
// Every vertex is what the CPU reference says: moved to its clip position (to 1e-4), or bit for bit what the game made it; and `moved` of them moved.
static void expectReference(Fails& f, const char* what, const st::TurretParams& p, const Captured& m, int expectMoved) {
    bool same = true;
    int moved = 0;
    for (size_t i = 0; i < m.pre.size(); ++i) {
        double in[4], out[4];
        for (int c = 0; c < 4; ++c) in[c] = m.pre[i][c];
        const bool mv = st::anchoredMove<double>(p, in, out);
        moved += mv ? 1 : 0;
        if (mv)
            for (int c = 0; c < 4; ++c) same = same && closeTo(m.post[i][c], out[c], 1.0e-4);
        else
            same = same && std::memcmp(m.pre[i].data(), m.post[i].data(), 16) == 0;
    }
    f.expect(same, what);
    f.expect(moved == expectMoved, what);
}

static void moveCases(int fam, ID3D11GeometryShader* twin, Fails& f) {
    const Cam cam;
    const float dirs[5][3] = {{0.0f, 0.0f, 1.0f}, {0.2f, 0.1f, 1.0f}, {-0.3f, 0.25f, 1.0f}, {0.5f, -0.4f, 1.0f}, {-0.45f, -0.35f, 1.0f}};
    {   // (a) the anchor at the eye: a vertex at distance r keeps its NDC x, y and z, and its w becomes w (r + d) / r
        const st::TurretParams p = st::pushParamsFor(frameOfCam(cam), kPushBackM);
        std::vector<Pt> pts;
        std::vector<float> rs;
        for (const float r : {0.15f, 0.20f, 0.42f, 0.61f, 0.90f})
            for (const auto& d : dirs) {
                pts.push_back(dirPt(d[0], d[1], d[2], r));
                rs.push_back(r);
            }
        const Captured m = captureMove(fam, twin, pts, cam, p);
        expectReference(f, "(a) anchor at the eye, push 1.2 m: every vertex is the CPU reference's", p, m, int(pts.size()));
        bool keeps = true, built = true;
        for (size_t i = 0; i < pts.size(); ++i) {
            const auto& a = m.pre[i];
            const auto& b = m.post[i];
            keeps = keeps && closeTo(b[0] / b[3], a[0] / a[3], 2e-5) && closeTo(b[1] / b[3], a[1] / a[3], 2e-5) && closeTo(b[2] / b[3], a[2] / a[3], 2e-5) &&
                    closeTo(b[3], a[3] * (rs[i] + kPushBackM) / rs[i], 1e-4);
            // the `pre` output is the vertex as the game's shader made it: the stand-in's clip position
            built = built && closeTo(a[0], cam.p00 * pts[i].x + cam.offX * pts[i].d, 1e-5) && closeTo(a[1], cam.p11 * pts[i].y + cam.offY * pts[i].d, 1e-5) && closeTo(a[3], pts[i].d, 1e-5) &&
                    closeTo(a[2], 0.5 * pts[i].d + 0.05 * std::fabs(pts[i].x), 1e-5);
        }
        f.expect(keeps, "(a) anchor at the eye: a vertex at distance r keeps its NDC x, y and z, and its w becomes w (r + d) / r");
        f.expect(built, "...and the `pre` output is the clip position the game's vertex shader made");
    }
    {   // (b) the anchor 10 cm to the side and 5 cm up (and a vertex 15 cm from it, the arcs layer at the stock camera): the CPU reference, and the geometry itself
        const float a[3] = {0.10f, 0.05f, 0.0f};
        const st::TurretParams p = st::pushParamsFor(frameOfCam(cam, a), kPushBackM);
        std::vector<Pt> pts;
        for (const float r : {0.30f, 0.50f})
            for (const auto& d : dirs) pts.push_back(dirPt(d[0], d[1], d[2], r));
        pts.push_back(aroundPt(a, 0.3f, 0.2f, 0.93f, 0.15f));
        pts.push_back(aroundPt(a, -0.5f, 0.1f, 0.8f, 0.15f));
        pts.push_back(aroundPt(a, 0.0f, 0.0f, 1.0f, 0.15f));
        const Captured m = captureMove(fam, twin, pts, cam, p);
        expectReference(f, "(b) anchor (0.10, 0.05, 0): every vertex is the CPU reference's", p, m, int(pts.size()));
        bool geom = true;
        for (size_t i = 0; i < pts.size(); ++i) {
            double pin[3], pout[3];
            eyePos(m.pre[i], cam, pin);
            eyePos(m.post[i], cam, pout);
            const double vin[3] = {pin[0] - a[0], pin[1] - a[1], pin[2] - a[2]}, vout[3] = {pout[0] - a[0], pout[1] - a[1], pout[2] - a[2]};
            geom = geom && closeTo(pin[0], pts[i].x, 2e-5) && closeTo(pin[1], pts[i].y, 2e-5) && closeTo(pin[2], pts[i].d, 2e-5) && closeTo(len3(vout), len3(vin) + kPushBackM, 3e-4) && sameRay(vin, vout);
        }
        f.expect(geom, "(b) the vertex is moved d metres further from the anchor along the ray from it (the distance grows by exactly d, the direction is kept), seen with the camera's own projection");
        double near15[3];
        eyePos(m.pre[pts.size() - 1], cam, near15);
        const double v15[3] = {near15[0] - a[0], near15[1] - a[1], near15[2] - a[2]};
        f.expect(closeTo(len3(v15), 0.15, 1e-4), "...with the last vertex 15 cm from the anchor (it ends 1.35 m from it)");
    }
    {   // (c) asymmetric tangents (the Pimax set, a vertical pair that is not symmetric either): the move, and a gap so small that the vertex returns to itself
        const float tl = std::tan(-0.89775f), tr = std::tan(0.71858f), td = std::tan(-1.05f), tu = std::tan(0.93f);
        const Cam camB = camOfTangents(tl, tr, td, tu);
        const float a[3] = {0.03f, 0.02f, -0.01f};
        const st::EyeFrame eb = frameOfCam(camB, a);
        f.expect(std::fabs(eb.l - tl) < 1e-5f && std::fabs(eb.r - tr) < 1e-5f && std::fabs(eb.dn - td) < 1e-5f && std::fabs(eb.up - tu) < 1e-5f, "(c) the asymmetric camera's tangents are the ones asked for");
        std::vector<Pt> pts;
        for (const float x : {tl * 0.8f, 0.0f, tr * 0.9f})
            for (const float y : {td * 0.7f, tu * 0.8f})
                for (const float d : {0.25f, 0.4f}) pts.push_back(Pt{d * x, d * y, d});
        const st::TurretParams p = st::pushParamsFor(eb, kPushBackM);
        const Captured m = captureMove(fam, twin, pts, camB, p);
        expectReference(f, "(c) asymmetric tangents: every vertex is the CPU reference's", p, m, int(pts.size()));
        bool geom = true;
        for (size_t i = 0; i < pts.size(); ++i) {
            double pin[3], pout[3];
            eyePos(m.pre[i], camB, pin);
            eyePos(m.post[i], camB, pout);
            const double vin[3] = {pin[0] - a[0], pin[1] - a[1], pin[2] - a[2]}, vout[3] = {pout[0] - a[0], pout[1] - a[1], pout[2] - a[2]};
            geom = geom && closeTo(pin[0], pts[i].x, 2e-5) && closeTo(pin[1], pts[i].y, 2e-5) && closeTo(len3(vout), len3(vin) + kPushBackM, 3e-4) && sameRay(vin, vout);
        }
        f.expect(geom, "(c) ...and by the camera's own projection the vertex moved d along its ray from the anchor");
        const st::TurretParams tiny = st::makeParams(eb, 1.0e-5f, 0.02f, 1.0f);
        const Captured r = captureMove(fam, twin, pts, camB, tiny);
        bool back = true;
        for (size_t i = 0; i < pts.size(); ++i)
            for (int c = 0; c < 4; ++c) back = back && closeTo(r.post[i][c], r.pre[i][c], 1.0e-4);
        f.expect(back, "(c) a gap of 10 micrometres round-trips every vertex to its own clip position (the reconstruction and the projection use the same tangents)");
    }
    {   // (d) the window: a vertex at anchor distance 1.4 m, 2.8 m or 500 m, just past 1 m, or just inside 2 cm is untouched; one at 0.5 m moves
        const st::TurretParams p = st::pushParamsFor(frameOfCam(cam), kPushBackM);
        const std::vector<Pt> pts = {dirPt(0, 0, 1, 1.4f), dirPt(0.2f, 0.1f, 1, 2.8f), dirPt(0.1f, 0.05f, 1, 500.0f), dirPt(0, 0, 1, 1.0005f), dirPt(0.1f, 0, 1, 0.0199f), dirPt(0, 0.1f, 1, 0.5f)};
        const Captured m = captureMove(fam, twin, pts, cam, p);
        expectReference(f, "(d) the window: every vertex is the CPU reference's", p, m, 1);
        bool still = true;
        for (size_t i = 0; i < 5; ++i) still = still && std::memcmp(m.pre[i].data(), m.post[i].data(), 16) == 0;
        f.expect(still && std::memcmp(m.pre[5].data(), m.post[5].data(), 16) != 0, "(d) vertices at 1.4 m, 2.8 m and 500 m (the larger panels, the reticle), at 1.0005 m and at 1.9 cm are untouched bit for bit; the one at 0.5 m moved");
    }
    {   // (e) a pull: the first build's gap, window gap + 0.10 to 4.0 m from the anchor
        const float a[3] = {0.10f, 0.05f, 0.0f};
        const st::TurretParams p = st::paramsFor(frameOfCam(cam, a), kPullGapM);
        const std::vector<Pt> pts = {dirPt(0, 0, 1, 1.5f), dirPt(0.2f, 0.1f, 1, 2.0f), dirPt(-0.3f, 0.25f, 1, 3.5f), dirPt(0, 0, 1, 0.5f), dirPt(0, 0, 1, 0.9f), dirPt(0.1f, 0.05f, 1, 500.0f)};
        const Captured m = captureMove(fam, twin, pts, cam, p);
        expectReference(f, "(e) a pull of 0.96 m: every vertex is the CPU reference's", p, m, 3);
        bool geom = true;
        for (size_t i = 0; i < 3; ++i) {
            double pin[3], pout[3];
            eyePos(m.pre[i], cam, pin);
            eyePos(m.post[i], cam, pout);
            const double vin[3] = {pin[0] - a[0], pin[1] - a[1], pin[2] - a[2]}, vout[3] = {pout[0] - a[0], pout[1] - a[1], pout[2] - a[2]};
            geom = geom && closeTo(len3(vout), len3(vin) - kPullGapM, 3e-4) && sameRay(vin, vout);
        }
        bool still = true;
        for (size_t i = 3; i < 6; ++i) still = still && std::memcmp(m.pre[i].data(), m.post[i].data(), 16) == 0;
        f.expect(geom && still, "(e) the pull brings the vertices 1.5 m to 3.5 m from the anchor 0.96 m NEARER along their rays; those at 0.5 m, 0.9 m and 500 m are untouched");
    }
    {   // (g) the translation alone (HUD forward, no push): every vertex moves by T; a vertex behind the eye that T brings in front appears, one that stays behind moves too
        const float t[3] = {0.02f, -0.01f, 0.7f};
        const st::TurretParams p = st::translateParamsFor(frameOfCam(cam, nullptr, t));
        const std::vector<Pt> pts = {dirPt(0, 0, 1, 0.5f), dirPt(0.2f, 0.1f, 1, 1.4f), dirPt(-0.3f, 0.25f, 1, 2.8f), dirPt(0.1f, 0.05f, 1, 500.0f), Pt{0.0f, 0.0f, -0.2f}, Pt{0.05f, 0.02f, -1.0f}};
        const Captured m = captureMove(fam, twin, pts, cam, p);
        expectReference(f, "(g) the translation alone: every vertex is the CPU reference's", p, m, 6);
        bool shifted = true;
        for (size_t i = 0; i < 6; ++i) {
            if (i == 3) continue;   // 500 m out: float precision of the clip x there is a centimetre, the reference above covers it
            double pin[3], pout[3];
            eyePos(m.pre[i], cam, pin);
            eyePos(m.post[i], cam, pout);
            shifted = shifted && closeTo(pout[0] - pin[0], t[0], 3e-5) && closeTo(pout[1] - pin[1], t[1], 3e-5) && closeTo(pout[2] - pin[2], t[2], 3e-5);
        }
        f.expect(shifted, "(g) with no push every vertex moves by exactly T (0.02, -0.01, 0.7), including the one 0.2 m BEHIND the eye, which T brings 0.5 m in front of it, and the one 1 m behind");
        f.expect(std::memcmp(m.pre[5].data(), m.post[5].data(), 16) != 0 && closeTo(m.post[5][3], -0.3, 1e-5), "(g) a vertex 1 m behind the eye ends 0.3 m behind it, MOVED by T and not left as the game made it");
    }
    {   // (h) the translation and the push: the translated vertex is judged against the window from the anchor
        const float a[3] = {0.10f, 0.05f, 0.0f}, t[3] = {0.02f, -0.01f, 0.7f};
        const st::TurretParams p = st::pushParamsFor(frameOfCam(cam, a, t), kPushBackM);
        const std::vector<Pt> pts = {Pt{0.0f, 0.0f, 0.1f}, Pt{0.03f, 0.02f, 0.15f}, dirPt(0, 0, 1, 0.3f), dirPt(0.2f, 0.1f, 1, 1.4f), Pt{0.0f, 0.0f, -0.2f}, Pt{-0.04f, 0.0f, 0.3f}};
        const Captured m = captureMove(fam, twin, pts, cam, p);
        expectReference(f, "(h) the translation and the push: every vertex is the CPU reference's", p, m, 6);
        bool geom = true;
        for (size_t i = 0; i < pts.size(); ++i) {
            double pin[3], pout[3];
            eyePos(m.pre[i], cam, pin);
            eyePos(m.post[i], cam, pout);
            const double p1[3] = {pin[0] + t[0], pin[1] + t[1], pin[2] + t[2]}, v1[3] = {p1[0] - a[0], p1[1] - a[1], p1[2] - a[2]}, vout[3] = {pout[0] - a[0], pout[1] - a[1], pout[2] - a[2]};
            const double d1 = len3(v1);
            if (d1 > 0.02 && d1 < 1.0)
                geom = geom && closeTo(len3(vout), d1 + kPushBackM, 3e-4) && sameRay(v1, vout);
            else
                geom = geom && closeTo(pout[0], p1[0], 3e-5) && closeTo(pout[1], p1[1], 3e-5) && closeTo(pout[2], p1[2], 3e-5);
        }
        f.expect(geom, "(h) a translated vertex within 1 m of the anchor is pushed 1.2 m further along its ray from it, any other is only translated (the arcs layer 0.15 m ahead of the head, and one 0.2 m behind it)");
    }
    {   // (k) the depth. Elite is reversed Z with the constant clip z 0.025 (cb0[6] = (0, 0, 0, 0.025)), so NDC z = 0.025 / w: valid only for w above 0.025. A vertex behind the eye, or inside
        // the near plane, has no valid depth and would be cut by the rasterizer once moved: it is drawn at 0.999, the nearest. A valid one keeps its own.
        const float t[3] = {0.0f, 0.0f, 0.7f};
        const st::TurretParams p = st::translateParamsFor(frameOfCam(cam, nullptr, t));
        const std::vector<Pt> pts = {Pt{0.0f, 0.0f, -0.20f, 0.025f}, Pt{0.03f, 0.01f, 0.24f, 0.025f}, Pt{0.0f, 0.0f, 0.01f, 0.025f}, Pt{0.02f, 0.0f, 0.60f, 0.025f}, Pt{0.0f, 0.0f, -0.01f, 0.025f},
                                     Pt{0.0f, 0.02f, 0.30f, 0.025f}};
        const Captured m = captureMove(fam, twin, pts, cam, p);
        expectReference(f, "(k) the depth: every vertex is the CPU reference's", p, m, int(pts.size()));
        const double want[6] = {0.999, 0.025 / 0.24, 0.999, 0.025 / 0.60, 0.999, 0.025 / 0.30};
        bool depth = true, ahead = true;
        for (size_t i = 0; i < pts.size(); ++i) {
            depth = depth && closeTo(double(m.post[i][2]) / m.post[i][3], want[i], 2e-5);
            ahead = ahead && closeTo(m.post[i][3], pts[i].d + 0.7, 1e-5) && m.post[i][3] > 0.0f;
        }
        f.expect(depth, "(k) a vertex at w = -0.20 (z 0.025) rasterizes with z/w = 0.999; one at w = 0.24 keeps NDC z = 0.025/0.24; one at w = 0.01 (NDC z 2.5) and one at -0.01 get 0.999");
        f.expect(ahead, "(k) ... and each ends T = 0.7 m deeper in clip w, in front of the eye");
    }
    {   // (l) a triangle with a corner moved behind the eye outputs all three corners moved, none the unmoved input
        const float back[3] = {0.0f, 0.0f, -0.7f};
        const st::TurretParams p = st::translateParamsFor(frameOfCam(cam, nullptr, back));
        const std::vector<Pt> pts = {Pt{0.0f, 0.0f, 0.5f}, Pt{0.3f, 0.0f, 1.2f}, Pt{-0.3f, 0.2f, 1.2f}};
        const Captured m = captureMove(fam, twin, pts, cam, p);
        expectReference(f, "(l) a corner moved behind the eye: every vertex is the CPU reference's", p, m, 3);
        bool all = true;
        for (size_t i = 0; i < 3; ++i) all = all && std::memcmp(m.pre[i].data(), m.post[i].data(), 16) != 0;
        f.expect(all && closeTo(m.post[0][3], -0.2, 1e-5) && closeTo(m.post[1][3], 0.5, 1e-5) && closeTo(m.post[2][3], 0.5, 1e-5),
                 "(l) a triangle whose first corner the move puts 0.2 m behind the eye (the others 0.5 m ahead) is output with all three corners moved, none the unmoved input");
    }
    {   // (m) the two groups in one frame: the panels' constants translate by the HUD-forward T (0.7 m), the radar's by its own key (0.3 m), the same vertices, the same eye frame
        const float tp[3] = {0.0f, 0.0f, 0.7f}, trd[3] = {0.0f, 0.0f, 0.3f};
        const st::EyeFrame frame = frameOfCam(cam, nullptr, tp, trd);
        const st::TurretParams panelsP = st::translateParamsFor(frame, st::Group::kPanels), radarP = st::translateParamsFor(frame, st::Group::kRadar);
        const std::vector<Pt> pts = {dirPt(0, 0, 1, 0.5f), dirPt(0.2f, 0.1f, 1, 1.4f), dirPt(-0.3f, 0.25f, 1, 2.8f), Pt{0.0f, 0.0f, -0.2f}, Pt{0.05f, 0.02f, 0.9f}, Pt{-0.05f, 0.0f, 0.6f}};
        const Captured mp = captureMove(fam, twin, pts, cam, panelsP);
        const Captured mr = captureMove(fam, twin, pts, cam, radarP);
        expectReference(f, "(m) the panels' constants: every vertex is the CPU reference's", panelsP, mp, int(pts.size()));
        expectReference(f, "(m) the radar's constants: every vertex is the CPU reference's", radarP, mr, int(pts.size()));
        bool own = true;
        for (size_t i = 0; i < pts.size(); ++i) {
            double pinP[3], poutP[3], pinR[3], poutR[3];
            eyePos(mp.pre[i], cam, pinP);
            eyePos(mp.post[i], cam, poutP);
            eyePos(mr.pre[i], cam, pinR);
            eyePos(mr.post[i], cam, poutR);
            own = own && closeTo(poutP[2] - pinP[2], 0.7, 3e-5) && closeTo(poutR[2] - pinR[2], 0.3, 3e-5) && closeTo(poutP[0] - pinP[0], 0.0, 3e-5) && closeTo(poutR[0] - pinR[0], 0.0, 3e-5) &&
                  closeTo(poutP[1] - pinP[1], 0.0, 3e-5) && closeTo(poutR[1] - pinR[1], 0.0, 3e-5);
        }
        f.expect(own && panelsP.tz == 0.7f && radarP.tz == 0.3f,
                 "(m) one eye frame, two groups: the panels' vertices move 0.7 m along Z (the HUD-forward key) and the radar's 0.3 m (its own key), each only by its own");
    }
}

// The pictures: the move seen through the rasterizer. With the anchor at the eye the picture is the plain one; with the anchor off the eye the picture is that of the quad truly moved.
static void moveImages(ID3D11GeometryShader* twin, Fails& f) {
    const Cam cam;
    {   // (I1) the anchor at the eye: the push keeps the angular size, so the picture does not change (the w of every vertex grows by the same factor)
        const st::TurretParams p = st::pushParamsFor(frameOfCam(cam), kPushBackM);
        Pt c[4];
        boxAt(c, 0.0f, 0.0f, 0.4f, 0.08f, 0.053f);
        std::vector<Vtx> v;
        addQuadPts(v, c);
        const Frame plain = render(v, cam, nullptr, st::TurretParams{});
        const Frame moved = render(v, cam, twin, p);
        f.expect(covered(plain) > 1000 && diffCount(plain, moved, 0.02) <= 0.03 * covered(plain), "(I1) anchor at the eye: the pushed quad's picture is the plain quad's (the push keeps its size on the screen)");
    }
    {   // (I2) the anchor off the eye: the picture is that of the quad truly moved 1.2 m along the rays from the anchor
        const float a[3] = {0.10f, 0.05f, 0.0f}, none[3] = {0, 0, 0};
        const st::TurretParams p = st::pushParamsFor(frameOfCam(cam, a), kPushBackM);
        Pt c[4], q[4];
        boxAt(c, 0.0f, 0.0f, 0.45f, 0.08f, 0.05f);
        for (int i = 0; i < 4; ++i) q[i] = movedPt(c[i], a, none, -kPushBackM, st::kPushNearM, st::kPushSplitM);
        std::vector<Vtx> v, truth;
        addQuadPts(v, c);
        addQuadPts(truth, q);
        const Frame plain = render(v, cam, nullptr, st::TurretParams{});
        const Frame moved = render(v, cam, twin, p);
        const Frame real = render(truth, cam, nullptr, st::TurretParams{});
        f.expect(covered(real) > 300 && diffCount(real, moved, 0.02) <= 0.04 * covered(real) && diffCount(plain, moved, 0.05) > 0.2 * covered(real),
                 "(I2) anchor off the eye: the pushed quad's picture is the picture of the quad truly moved 1.2 m along the rays from the anchor, and not the plain one's");
    }
    {   // (I3) the translation alone
        const float a[3] = {0.10f, 0.05f, 0.0f}, t[3] = {0.02f, -0.01f, 0.7f};
        const st::TurretParams p = st::translateParamsFor(frameOfCam(cam, a, t));
        Pt c[4], q[4];
        boxAt(c, 0.0f, 0.0f, 0.3f, 0.15f, 0.10f);
        for (int i = 0; i < 4; ++i) q[i] = Pt{c[i].x + t[0], c[i].y + t[1], c[i].d + t[2]};
        std::vector<Vtx> v, truth;
        addQuadPts(v, c);
        addQuadPts(truth, q);
        const Frame moved = render(v, cam, twin, p);
        const Frame real = render(truth, cam, nullptr, st::TurretParams{});
        f.expect(covered(real) > 300 && diffCount(real, moved, 0.02) <= 0.04 * covered(real), "(I3) the translation alone: the quad's picture is the picture of the quad moved by T");
    }
    {   // (I5) a quad the game had BEHIND the eye (w -0.2, the constant clip z 0.025 of Elite): after the translation it covers pixels, as the quad truly moved does
        const float t[3] = {0.0f, 0.0f, 0.7f};
        const st::TurretParams p = st::translateParamsFor(frameOfCam(cam, nullptr, t));
        Pt c[4], q[4];
        boxAt(c, 0.0f, 0.0f, -0.2f, 0.06f, 0.04f, 0.025f);
        for (int i = 0; i < 4; ++i) q[i] = Pt{c[i].x, c[i].y, c[i].d + t[2]};
        std::vector<Vtx> v, truth;
        addQuadPts(v, c);
        addQuadPts(truth, q);
        const Frame moved = render(v, cam, twin, p);
        const Frame real = render(truth, cam, nullptr, st::TurretParams{});
        f.expect(covered(real) > 100 && covered(moved) > 100 && diffCount(real, moved, 0.02) <= 0.04 * covered(real),
                 "(I5) a quad the game had behind the eye covers pixels after the translation, the same as the quad truly moved");
    }
    {   // (I4) the translation, then the push (the arcs layer 15 cm in front of the head)
        const float a[3] = {0.10f, 0.05f, 0.0f}, t[3] = {0.02f, -0.01f, 0.7f};
        const st::TurretParams p = st::pushParamsFor(frameOfCam(cam, a, t), kPushBackM);
        Pt c[4], q[4];
        boxAt(c, 0.0f, 0.0f, 0.15f, 0.06f, 0.04f);
        for (int i = 0; i < 4; ++i) q[i] = movedPt(c[i], a, t, -kPushBackM, st::kPushNearM, st::kPushSplitM);
        std::vector<Vtx> v, truth;
        addQuadPts(v, c);
        addQuadPts(truth, q);
        const Frame moved = render(v, cam, twin, p);
        const Frame real = render(truth, cam, nullptr, st::TurretParams{});
        f.expect(covered(real) > 60 && diffCount(real, moved, 0.02) <= 0.04 * covered(real), "(I4) the translation and the push: the picture is that of the quad moved by T and then 1.2 m along the rays from the anchor");
    }
}

// The shipped push of the panels with the anchor at the eye: the constants the binding, linkage and issue-end cases bind.
static st::TurretParams defaultParams() { return st::pushParamsFor(frameOfCam(Cam{}), kPushBackM); }

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
    check(vd.OutputParameters == S.count && gd.InputParameters == S.count && gd.OutputParameters == S.count + 1,
          "the signature's elements, the same number in the vertex shader's output and the geometry shader's input, and one more (`pre`) in its output");
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
    {   // the one extra output element is TURRETPRE, four components, in the register after the signature's
        D3D11_SIGNATURE_PARAMETER_DESC pre{};
        ck(gsr->GetOutputParameterDesc(S.count, &pre), "the extra output element");
        check(!std::strcmp(pre.SemanticName, st::kPreSemantic) && pre.SemanticIndex == 0 && pre.Register == S.count && pre.Mask == 0xF,
              "...and the extra output is TURRETPRE: four components, in the register after the signature's");
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
    (void)render(v, Cam{}, gs, defaultParams());
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
        auto cb = g_constants[0].get(g_ctx.Get(), defaultParams());
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
    const st::TurretParams p = defaultParams();
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
    const Cam cam;
    const float a[3] = {0.10f, 0.05f, 0.0f}, t[3] = {0.02f, -0.01f, 0.7f};
    const st::EyeFrame e = frameOfCam(cam, a);
    const st::TurretParams p0 = st::pushParamsFor(e, 1.2f);
    check(c.get(nullptr, p0) == nullptr, "no context: no buffer");
    ID3D11Buffer* first = c.get(g_ctx.Get(), p0);
    check(first && c.buffer() == first && c.written() == p0 && !c.failed(), "the buffer is made once, with the numbers");
    check(c.get(g_ctx.Get(), p0) == first && c.written() == p0, "the same numbers: the same buffer, not rewritten");
    const st::TurretParams p1 = st::pushParamsFor(frameOfCam(cam, a, t), 1.2f);
    check(c.get(g_ctx.Get(), p1) == first && c.written() == p1, "other numbers: the same buffer, rewritten");
    // what the shader sees is the four registers
    const auto readBack = [&](ID3D11Buffer* source) {
        D3D11_BUFFER_DESC bd{};
        source->GetDesc(&bd);
        check(bd.ByteWidth == 64, "the buffer is four 16-byte registers");
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
    check(readBack(first) == p1, "the buffer holds the move, the tangents, the anchor and the translation as written");
    st::TurretParams bad = p0;
    bad.ax = NAN;
    check(c.get(g_ctx.Get(), bad) == nullptr, "a number that is not finite: refused");
    check(c.get(g_ctx.Get(), st::translateParamsFor(frameOfCam(cam))) == nullptr, "a gap of 0 with no translation (it moves nothing, and the module never binds one): refused");
    check(c.get(g_ctx.Get(), st::pushParamsFor(e, 2.5f)) == nullptr, "a push beyond 2 m: refused");
    bad = st::paramsFor(e, 2.5f);
    check(c.get(g_ctx.Get(), bad) == nullptr, "a pull beyond 2 m: refused");
    bad = p0;
    bad.dMin = 0.0f;
    check(c.get(g_ctx.Get(), bad) == nullptr, "a window that starts at 0: refused");
    bad = st::paramsFor(e, 0.96f);
    bad.dMin = bad.gap;
    check(c.get(g_ctx.Get(), bad) == nullptr, "a pull whose window starts at the gap (a divisor of zero): refused");
    bad = p0;
    bad.dMax = bad.dMin;
    check(c.get(g_ctx.Get(), bad) == nullptr, "an empty window: refused");
    bad = p0;
    bad.r = bad.l;
    check(c.get(g_ctx.Get(), bad) == nullptr, "tangents that are no frustum: refused");
    bad = p0;
    bad.ax = 1.9f;
    bad.ay = 1.9f;
    check(c.get(g_ctx.Get(), bad) == nullptr, "an anchor 2 m or more from the eye: refused");
    bad = p0;
    bad.tz = 2.5f;
    check(c.get(g_ctx.Get(), bad) == nullptr, "a translation beyond 2 m: refused");
    check(c.get(g_ctx.Get(), p1) == first, "...and the buffer is still the good one");
    const st::TurretParams pull = st::paramsFor(e, 0.96f), top = st::pushParamsFor(e, st::kPushMaxCm / 100.0f), only = st::translateParamsFor(frameOfCam(cam, a, t));
    check(c.get(g_ctx.Get(), pull) == first && c.written() == pull && c.get(g_ctx.Get(), top) == first && c.written() == top && c.get(g_ctx.Get(), only) == first && c.written() == only,
          "a pull, a push of the key's top (2.00 m) and a translation alone (gap 0) are accepted, the same buffer rewritten each time");
    check(readBack(first) == only, "...and the buffer holds the translation alone's numbers");
    ComPtr<ID3D11Device> d2;
    ComPtr<ID3D11DeviceContext> c2;
    ck(edvr::systemD3D11CreateDevice()(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d2, nullptr, &c2), "second device");
    ID3D11Buffer* other = c.get(c2.Get(), p0);
    check(other && other != first && c.written() == p0, "another device: the buffer is made again for it");
    c.reset();
    check(c.buffer() == nullptr && !c.failed(), "reset releases the buffer");
    // the production arrays also carry the key defaults the module reads
    check(st::kBackDefaultCm == 0 && st::kForwardDefaultCm == 70 && st::kPanelsDefaultCm == 120 && st::kRadarDefaultCm == 0 && st::kGapDefaultCm == 0,
          "the keys' defaults: the stock camera (0), HUD forward 70, panels back 120, radar back 0, no gap (the pull is off)");
}

// ---------------------------------------------------------------- triangle strips (some of the HUD's strokes are drawn as one)
// A quad as a four-vertex strip. The input assembler hands the geometry shader's `triangle` input each strip triangle in the winding the rasterizer would have used, and the shader emits
// each as a strip of its own, so: (1) through a window nothing lies in, with no translation, the picture is the plain strip's bit for bit under every culling mode, and in the mode that
// shows the strip the other mode shows nothing (the winding is consistent, the control that the odd triangle is not turned inside out); (2) with the translation and the push the strip
// lands exactly where the same quad as a list lands.
static void stripCases(ID3D11GeometryShader* gs, Fails& f) {
    const Cam cam;
    const float a[3] = {0.10f, 0.05f, 0.0f}, t[3] = {0.02f, -0.01f, 0.7f};
    const st::TurretParams moving = st::pushParamsFor(frameOfCam(cam, a, t), kPushBackM);
    const st::TurretParams empty = st::makeParams(frameOfCam(cam), 0.5f, 100.0f, 200.0f);   // a valid window with nothing in it, and no translation: the shader is a copy
    Pt c[4];
    boxAt(c, 0.0f, 0.0f, 0.1f, 0.08f, 0.05f);
    std::vector<Vtx> list, strip;
    addQuadPts(list, c);
    strip = {list[0], list[1], list[2], list[4]};   // (x0,y0) (x1,y0) (x0,y1) (x1,y1)
    double shown[3] = {};
    for (int cull = 0; cull < 3; ++cull) {
        DrawOpts o;
        o.cull = cull;
        o.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        const Frame plain = render(strip, cam, nullptr, empty, 256, 256, o);
        const Frame copy = render(strip, cam, gs, empty, 256, 256, o);
        shown[cull] = covered(plain);
        f.expect(maxDiff(plain, copy) == 0.0, "a triangle strip through the shader with nothing in its window is the plain strip, bit for bit, under every culling mode");
        DrawOpts ol = o;
        ol.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        const Frame moved = render(strip, cam, gs, moving, 256, 256, o);
        const Frame listMoved = render(list, cam, gs, moving, 256, 256, ol);
        f.expect(maxDiff(moved, listMoved) == 0.0, "a strip quad under the translation and the push lands exactly where the same quad as a list lands");
        const Frame plainList = render(list, cam, nullptr, moving, 256, 256, ol);
        f.expect(cull != 0 || (covered(moved) > 100 && maxDiff(moved, plainList) > 0.05), "...and it is moved (the picture is not the plain quad's)");
    }
    f.expect(shown[0] > 100 && ((shown[1] > 100) != (shown[2] > 100)) && (shown[1] < 1.0 || shown[2] < 1.0),
             "the control: the plain strip is whole with no culling and with one facing culled, and gone with the other (both triangles share a winding)");
}

// ---------------------------------------------------------------- the GPU instrument (srv_turret.cpp's probe*): the same calls, on WARP
static ComPtr<ID3D11GeometryShader> twinFromCode(int fam, const void* data, size_t size) {
    D3D11_SO_DECLARATION_ENTRY decl[st::kMaxSigElements] = {};
    const UINT entries = st::soDeclaration(uint32_t(fam), decl);
    const UINT stride = (st::sigFloats(uint32_t(fam)) + st::kPreFloats) * 4;
    ComPtr<ID3D11GeometryShader> twin;
    ck(g_dev->CreateGeometryShaderWithStreamOutput(data, size, decl, entries, &stride, 1, 0, nullptr, &twin), "the stream-output twin is made from the signature's bytecode");
    return twin;
}
static void instrumentCases(int fam, ID3D11GeometryShader* gs) {
    const st::Signature& S = st::kSignatures[fam];
    const uint32_t strideFloats = st::sigFloats(uint32_t(fam)) + st::kPreFloats, posFloat = st::sigPosFloat(uint32_t(fam)), preFloat = st::sigFloats(uint32_t(fam));
    // the twin, made as the module makes it: the signature's bytecode, its stream-output entries (then `pre`), stream 0 rasterized as well
    const ComPtr<ID3D11GeometryShader> twin = twinFromCode(fam, kBytes[fam].data, kBytes[fam].size);
    const Cam cam;
    const float a[3] = {0.10f, 0.05f, 0.0f};
    const st::TurretParams params = st::pushParamsFor(frameOfCam(cam, a), kPushBackM);
    Pt c[4];
    boxAt(c, 0.0f, 0.0f, 0.45f, 0.08f, 0.05f);
    std::vector<Vtx> quad;
    addQuadPts(quad, c);
    DrawOpts base;

    // a plain draw: the vertex shader ran, the geometry shader did not (gs = 0 with ia_prims above 0 is how a flight reads "never ran")
    Sample plain;
    DrawOpts o = base;
    o.sample = &plain;
    const Frame plainFrame = render(quad, cam, nullptr, params, 256, 256, o);
    check(plain.stats.IAPrimitives == 2 && plain.stats.VSInvocations >= 4 && plain.stats.GSInvocations == 0 && plain.stats.GSPrimitives == 0 && plain.stats.PSInvocations > 300,
          "a quad drawn with no geometry shader: two primitives in, vertex shader and pixel shader ran, the geometry shader's invocation count is 0");

    // through the production shader: it ran once for each triangle
    Sample prod;
    o.sample = &prod;
    const Frame prodFrame = render(quad, cam, gs, params, 256, 256, o);
    check(prod.stats.IAPrimitives == 2 && prod.stats.GSInvocations == 2 && prod.stats.GSPrimitives == 2 && prod.stats.CInvocations >= 2 && prod.stats.PSInvocations > 300 && !prod.gsAfter,
          "...and through the production shader: two primitives in, the geometry shader ran twice and emitted two, and the context's geometry stage is empty after");
    check(maxDiff(plainFrame, prodFrame) > 0.01, "...and the pixels differ from the plain draw: the shader moved the quad");

    // through the twin with its stream-output target: the same statistics, the same pixels, and the records of what the shader wrote
    Sample cap;
    o.sample = &cap;
    o.soTarget = true;
    const Frame twinFrame = render(quad, cam, twin.Get(), params, 256, 256, o);
    check(cap.stats.GSInvocations == 2 && cap.stats.GSPrimitives == 2 && cap.stats.IAPrimitives == 2 && cap.so.NumPrimitivesWritten == 2 && cap.so.PrimitivesStorageNeeded == 2,
          "through the twin: two invocations, two primitives written to the stream-output target, none lost");
    check(maxDiff(prodFrame, twinFrame) == 0.0, "...and it rasterizes exactly as the production shader does (the instrument does not change the picture)");
    check(!cap.soTargetAfter && !cap.gsAfter, "...and the stream-output target and the geometry stage are empty after the scope");
    const st::CaptureSummary cs = st::summarizeCapture(cap.records.data(), 6, strideFloats, posFloat, preFloat, params);
    double lo = 1e9, hi = 0;
    for (const Pt& p : c) {
        const double d = std::sqrt((p.x - a[0]) * (p.x - a[0]) + (p.y - a[1]) * (p.y - a[1]) + (p.d - a[2]) * (p.d - a[2]));
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    check(cs.vertices == 6 && cs.inWindow == 6 && std::fabs(cs.beforeMin - lo) < 1e-3 && std::fabs(cs.beforeMax - hi) < 1e-3 && std::fabs(cs.afterMin - (lo + kPushBackM)) < 2e-3 &&
              std::fabs(cs.afterMax - (hi + kPushBackM)) < 2e-3,
          "the capture reads six vertices, all moved, at anchor distance 0.46 to 0.52 m before and 1.2 m further after");
    // the captured record is the vertex whole: the interpolants as the vertex shader wrote them, not only the position (the first vertex is u = 0, v = 0, and the second element
    // starts with 0.25; the second vertex starts with u = 1)
    bool whole = std::fabs(cap.records[0] - 0.0f) < 1e-6f && std::fabs(cap.records[1] - 0.0f) < 1e-6f && std::fabs(cap.records[strideFloats] - 1.0f) < 1e-6f;
    if (S.el[0].comps == 4) whole = whole && std::fabs(cap.records[3] - 1.0f) < 1e-6f;
    if (S.count > 2) whole = whole && std::fabs(cap.records[S.el[0].comps] - 0.25f) < 1e-6f;
    check(whole, "...with the interpolants intact in the record (the texture coordinate and the second element): the shader passes them through");
    check(cs.zFixed == 0, "...none of them on the depth fallback (their own NDC z is valid)");
    // `pre` is the first vertex's clip position as the stand-in vertex shader made it, and the position differs from it
    check(std::fabs(cap.records[preFloat + 0] - (cam.p00 * c[0].x + cam.offX * c[0].d)) < 1e-5f && std::fabs(cap.records[preFloat + 3] - c[0].d) < 1e-5f &&
              std::memcmp(&cap.records[preFloat], &cap.records[posFloat], 16) != 0,
          "...and its `pre` is the clip position before the move (the vertex shader's), different from the position the shader wrote");

    // a quad whose vertices lie 1.4 m or more from the anchor (the larger panels) and the reticle's depth: the shader RUNS (gs = 2) and moves nothing, the pixels are the plain draw's, and
    // the capture says so (in_window 0): this is what a flight reads for the larger panels' draws
    for (const float depth : {1.5f, 500.0f}) {
        std::vector<Vtx> outside;
        Pt q[4];
        boxAt(q, 0.0f, 0.0f, depth, 0.2f * depth, 0.13f * depth);
        addQuadPts(outside, q);
        Sample outCap, outPlain;
        DrawOpts oo = base;
        oo.sample = &outCap;
        oo.soTarget = true;
        const Frame twinOut = render(outside, cam, twin.Get(), params, 256, 256, oo);
        DrawOpts ob = base;
        ob.sample = &outPlain;
        const Frame plainOut = render(outside, cam, nullptr, params, 256, 256, ob);
        const st::CaptureSummary co = st::summarizeCapture(outCap.records.data(), 6, strideFloats, posFloat, preFloat, params);
        char what[220];
        std::snprintf(what, sizeof(what), "a quad at %.1f m: the geometry shader ran twice, the capture reads in_window 0 and w = %.1f, and the picture is the plain draw's bit for bit", double(depth),
                      double(depth));
        check(outCap.stats.GSInvocations == 2 && co.vertices == 6 && co.inWindow == 0 && std::fabs(co.wMin - depth) < depth * 1e-3f && maxDiff(twinOut, plainOut) == 0.0 && covered(twinOut) > 1000, what);
    }
    // the translation alone: every vertex is moved
    {
        const float t[3] = {0.02f, -0.01f, 0.7f};
        const st::TurretParams only = st::translateParamsFor(frameOfCam(cam, a, t));
        Sample ts;
        DrawOpts ot = base;
        ot.sample = &ts;
        ot.soTarget = true;
        (void)render(quad, cam, twin.Get(), only, 256, 256, ot);
        const st::CaptureSummary ct = st::summarizeCapture(ts.records.data(), 6, strideFloats, posFloat, preFloat, only);
        check(ct.vertices == 6 && ct.inWindow == 6 && ct.zFixed == 0 && ct.afterMin > ct.beforeMin + 0.5 && ct.wMin > 1.0f, "the translation alone: the capture reads all six vertices moved (w 0.45 becomes 1.15)");
        // a quad the game had behind the eye (Elite's clip z 0.025): moved, and every vertex on the depth fallback
        Pt behind[4];
        boxAt(behind, 0.0f, 0.0f, -0.2f, 0.06f, 0.04f, 0.025f);
        std::vector<Vtx> bq;
        addQuadPts(bq, behind);
        Sample bs;
        DrawOpts ob = base;
        ob.sample = &bs;
        ob.soTarget = true;
        (void)render(bq, cam, twin.Get(), only, 256, 256, ob);
        const st::CaptureSummary cb = st::summarizeCapture(bs.records.data(), 6, strideFloats, posFloat, preFloat, only);
        check(cb.vertices == 6 && cb.inWindow == 6 && cb.zFixed == 6 && cb.wMin > 0.4f, "a quad the game had behind the eye: all six vertices moved to the front, all six counted on the depth fallback");
    }
    // the summary's own edges
    std::vector<float> junk(2 * strideFloats, 0.0f);
    junk[posFloat] = NAN;
    junk[posFloat + 3] = 1.0f;
    junk[strideFloats + posFloat + 3] = -1.0f;
    const st::CaptureSummary cj = st::summarizeCapture(junk.data(), 2, strideFloats, posFloat, preFloat, params);
    check(cj.vertices == 1 && cj.inWindow == 1 && st::summarizeCapture(nullptr, 6, strideFloats, posFloat, preFloat, params).vertices == 0 &&
              st::summarizeCapture(junk.data(), 2, 3, posFloat, preFloat, params).vertices == 0,
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
static ComPtr<ID3D11GeometryShader> twinFromText(int fam, const std::string& text) {
    bool ok = false;
    const auto code = compileHlsl(text, "gs_5_0", &ok);
    check(ok, "a mutant compiles");
    return twinFromCode(fam, code->GetBufferPointer(), code->GetBufferSize());
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
        ComPtr<ID3D11GeometryShader> gsOf[st::kSignatureCount], twinOf[st::kSignatureCount];
        for (uint32_t fam = 0; fam < st::kSignatureCount; ++fam) {
            const auto compiled = compileHlsl(kBytes[fam].text, "gs_5_0");
            check(compiled->GetBufferSize() == kBytes[fam].size && !std::memcmp(compiled->GetBufferPointer(), kBytes[fam].data, kBytes[fam].size),
                  "the build's bytecode of each signature's shader is exactly the compile of srv_turret_hud_shader.h's text");
            ck(g_dev->CreateGeometryShader(kBytes[fam].data, kBytes[fam].size, nullptr, &gsOf[fam]), "the device takes the production shader");
            twinOf[fam] = twinFromCode(int(fam), kBytes[fam].data, kBytes[fam].size);
        }

        constantsCases();
        Fails production;
        for (uint32_t fam = 0; fam < st::kSignatureCount; ++fam) {
            g_fam = int(fam);
            linkageCases(int(fam), gsOf[fam].Get());
            instrumentCases(int(fam), gsOf[fam].Get());
            moveCases(int(fam), twinOf[fam].Get(), production);
            moveImages(twinOf[fam].Get(), production);
            stripCases(twinOf[fam].Get(), production);
        }
        g_fam = 0;
        bindingCases(gsOf[0].Get(), production, true);
        afterIssueCases(gsOf[0].Get(), production, false);
        if (production.n) {
            std::printf("first failing production check: %s (%u failed)\n", production.first.c_str(), production.n);
            for (const std::string& k : production.kinds) std::printf("  failed: %s\n", k.c_str());
        }
        check(production.n == 0, "the production shaders and binding pass every move, picture, strip, binding and issue-end case, for all eight signatures");

        // THE MUTANTS: each through the same cases (the move's numbers and pictures), each must fail at least one, in each signature's text. They are edits of the shared function turretMove
        // and of the pass-through that calls it.
        struct Mutant {
            const char* name;
            const char* from;
            const char* to;
        };
        const Mutant mutants[] = {
            {"no window", "const bool win = d > mv.y && d < mv.z;", "const bool win = true;"},
            {"the far end of the window ignored", "d < mv.z", "d < 1000.0"},
            {"the near end of the window ignored", "d > mv.y && ", ""},
            {"the sign of the anchor", "const float3 v = p1 - an.xyz;", "const float3 v = p1 + an.xyz;"},
            {"the sign of the gap", "((d - mv.x) / d)", "((d + mv.x) / d)"},
            {"y read down", "c.y * (tn.w - tn.z) * 0.5 +", "-c.y * (tn.w - tn.z) * 0.5 +"},
            {"the left and right tangents mixed", "w * (tn.y + tn.x) * 0.5, c.y", "w * (tn.y - tn.x) * 0.5, c.y"},
            {"the translation dropped", "const float3 p1 = p + tr.xyz;", "const float3 p1 = p;"},
            {"the translation's sign", "const float3 p1 = p + tr.xyz;", "const float3 p1 = p - tr.xyz;"},
            {"the move about the origin, not the anchor", "q = an.xyz + v * ((d - mv.x) / d);", "q = v * ((d - mv.x) / d);"},
            {"w kept, not the moved depth", "ndcZ * q.z, q.z);", "ndcZ * q.z, w);"},
            {"the NDC z not kept", "ndcZ * q.z", "c.z"},
            {"the unmoved vertex returned when the move ends behind the eye", "if (win) q = an.xyz + v * ((d - mv.x) / d);", "if (win) q = an.xyz + v * ((d - mv.x) / d); if (!(q.z > 0.01)) return c;"},
            {"the depth fallback replaced by z / w", "? zn : 0.999", "? zn : zn"},
            {"the depth fallback at the far end", ": 0.999;", ": 0.0;"},
            {"no fallback for a vertex inside the near plane", "zn >= 0.0 && zn <= 1.0", "zn >= 0.0"},
            {"a `pre` that is the moved position", "o.pre = v[i].pos;", "o.pre = turretMove(v[i].pos);"},
        };
        for (uint32_t fam = 0; fam < st::kSignatureCount; ++fam) {
            g_fam = int(fam);
            const std::string text = kBytes[fam].text;
            for (const Mutant& m : mutants) {
                const std::string mutated = replaceAll(text, m.from, m.to);
                check(mutated != text, "a mutant differs from the production text");
                const auto mg = twinFromText(int(fam), mutated);
                Fails fails;
                moveCases(int(fam), mg.Get(), fails);
                moveImages(mg.Get(), fails);
                char what[200];
                std::snprintf(what, sizeof(what), "the mutant \"%s\" of signature %u's shader is caught by the move cases", m.name, fam);
                check(fails.n > 0, what);
            }
            {   // a pass-through that reverses every triangle's winding (what a shader that did not keep the input assembler's order would do to a strip): caught by the strip cases
                const std::string flipped = replaceAll(text, "o.v = v[i];", "o.v = v[(i == 0) ? 0 : (3 - i)];");
                check(flipped != text, "the winding mutant differs from the production text");
                const auto mg = twinFromText(int(fam), flipped);
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
        std::printf("PASS srv_turret_hud_test: %u checks; the turret shaders, binding, constants, maths, anchor and Scorpion latch on WARP, seventeen shader mutants and the winding mutant in each of eight signatures and one binding mutant caught\n", g_checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL %s (%u checks)\n", e.what(), g_checks);
        return 1;
    }
}
