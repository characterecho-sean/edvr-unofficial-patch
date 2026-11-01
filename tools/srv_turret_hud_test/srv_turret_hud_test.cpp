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
//   moves by T, one 0.2 m behind the eye comes in front of it, one that stays behind moves too. (h) The translation, then the push. (k) The depth: Elite is reversed Z (clip z the constant 0.025), so a vertex behind the eye or inside the near plane gets the nearest depth, 0.999, and a valid one keeps its own; (l) a triangle with a corner moved behind the eye is output with all three corners moved; (m) the two groups in one frame: the panels' constants translate by the HUD-forward T and the radar's by its own key's; (n) the three depth modes (game: the vertex's own valid depth else 0.999, real: the moved position's own depth clamped to 0..0.999, front: 0.999); (P) the radar disc's plane in the seated frame (the inverse of eyeFrameFrom's step; a turret-fixed square keeps its centre and normal as the head moves and turns, a tilted one reads its tilt) and the list of unmoved family draws; (S) the scissor: a draw the game scissored to the top half of the target, kept (the moved quad is cropped) and opened (it covers below the rectangle; the top half is the same picture), several rectangles (three, sixteen, none with the test on) put back exactly, the rasterizer state object untouched, no target view (the viewport's extent), a look that changes nothing, a second open refused and the boundary settle; (o) signature H only, the Scorpion's hologram: its lighting position moves by the camera's right, up and forward vectors (a stand-in vertex constant buffer at slot 1, bound whole and with a first constant) times the move's eye-space displacement, and not at all where the move leaves a vertex alone. The pictures (I1 to I4) compare the rasterized quad with the
//   plain one (anchor at the eye) and with the quad truly moved in 3D. THE STRIPS: a quad as a triangle strip is the plain strip bit for bit through a window with nothing in it and no
//   translation, under every culling mode (the winding is kept), and lands where the same quad as a list lands once it is moved.
//   THE ANCHOR (srv_turret_math.h AnchorTracker, no device): followed for the settle after the view comes on or a camera key changes, then latched; a recentre (the head jumping more than
//   20 cm in a frame) latches at once; the view going off clears it. THE EYE'S FRAME: the host's 3x4 row-major matrices turned into the anchor and the translation in the eye's space,
//   checked on a level head, a yawed head and a canted eye.
//   THE LINKAGE. Each shader links against its stand-in under the debug layer with no message, its input is the vertex shader's output element for element and its output the same
//   again plus TURRETPRE, the pixel shader's inputs are a subset of it; a vertex shader without the texture coordinates is reported by the layer (the control that it really checks).
//   THE SCENE CONSTANTS AND THE REGISTER COPY: signature H's binding takes the draw's vertex constants at GS slot 1 (with the range the game bound them with) beside the turret constants and gives both back, also through a failed restore and the settle; the disc probe's copy of a pixel-shader register (cb2[11], cb1[90]) is read from the right offset, counted from the bound range's first constant, and refused past the buffer or the range.
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
#include <functional>
#include <iterator>
#include <limits>
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
static ComPtr<ID3D11RasterizerState> g_rsScissor;   // no culling, ScissorEnable: the state the game draws the radar with
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
    D3D11_RASTERIZER_DESC sr{};
    sr.FillMode = D3D11_FILL_SOLID;
    sr.CullMode = D3D11_CULL_NONE;
    sr.DepthClipEnable = TRUE;
    sr.ScissorEnable = TRUE;
    ck(g_dev->CreateRasterizerState(&sr, &g_rsScissor), "CreateRasterizerState (scissor)");
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

// The game's view constants at the vertex stage's slot 1, as the Scorpion hologram's draw has them: 280 registers, rows 277 to 279 the camera's right, up and forward in world axes. render() binds
// it for every draw; the production binding takes it for signature H only. g_sceneFirst > 0 binds it with that first constant (a ranged binding, as the 11.1 interface allows), the rows
// then counted from it.
static float g_axes[3][3] = {};   // zeros unless a case gives the camera's vectors: with none the lighting position does not move, and the pictures compare as for the other signatures
static UINT g_sceneFirst = 0;
static constexpr UINT kSceneRangeConstants = 288;   // a multiple of 16 that holds the 280 registers
static ComPtr<ID3D11Buffer> sceneBuffer() {
    std::vector<float> cb((size_t(g_sceneFirst) + kSceneRangeConstants) * 4, 0.0f);
    for (UINT a = 0; a < 3; ++a)
        for (UINT c = 0; c < 3; ++c) cb[(size_t(g_sceneFirst) + st::kSceneRightRow + a) * 4 + c] = g_axes[a][c];
    return makeBuffer(UINT(sizeof(float) * cb.size()), D3D11_BIND_CONSTANT_BUFFER, cb.data());
}
static void bindScene(ID3D11Buffer* b) {
    ComPtr<ID3D11DeviceContext1> c1;
    if (g_sceneFirst && SUCCEEDED(g_ctx.As(&c1))) {
        const UINT first = g_sceneFirst, count = kSceneRangeConstants;
        c1->VSSetConstantBuffers1(st::kGameCbSlot, 1, &b, &first, &count);
    } else {
        g_ctx->VSSetConstantBuffers(st::kGameCbSlot, 1, &b);
    }
}
static void setScene(const float right[3], const float up[3], const float fwd[3], UINT first) {
    for (int c = 0; c < 3; ++c) {
        g_axes[0][c] = right[c];
        g_axes[1][c] = up[c];
        g_axes[2][c] = fwd[c];
    }
    g_sceneFirst = first;
}
static void resetScene() {
    const float none[3] = {0, 0, 0};
    setScene(none, none, none, 0);
}
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
    bool scissored = false;      // the game's draw has ScissorEnable and the one rectangle scissorRect
    D3D11_RECT scissorRect{0, 0, 0, 0};
    bool openScissor = false;    // the production Scissor opens it around the issue (fix.srv_turret_hud_scissor at open) and gives the game's back
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
    g_ctx->RSSetState(o.scissored ? g_rsScissor.Get() : g_rsOf[o.cull].Get());
    if (o.scissored) g_ctx->RSSetScissorRects(1, &o.scissorRect);
    const UINT stride = sizeof(Vtx), offset = 0;
    ID3D11Buffer* vbs[1] = {vb.Get()};
    g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    g_ctx->IASetInputLayout(g_layout.Get());
    g_ctx->IASetPrimitiveTopology(o.topology);
    g_ctx->VSSetShader(g_vs[g_fam].Get(), nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, cb0.GetAddressOf());
    const auto scene = sceneBuffer();
    bindScene(scene.Get());
    g_ctx->PSSetShader(g_ps[g_fam].Get(), nullptr, 0);
    t.clear();
    st::Binding binding;
    bool bound = false;
    if (gs) {
        ID3D11Buffer* cb = g_constants[o.eye].get(g_ctx.Get(), params);
        check(cb != nullptr, "the turret constants are made");
        st::ExtraCb extra;   // signature H: the draw's own vertex constants, read the way the module reads them
        const bool needScene = g_fam == int(st::kSceneSignature);
        if (needScene) check(st::readGameCb(g_ctx.Get(), st::kGameCbSlot, extra), "the draw's vertex constants are read");
        bound = binding.begin(g_ctx.Get(), gs, cb, st::directSetGs, needScene ? &extra : nullptr) == st::Binding::Begin::kBound;
        check(bound, "the production binding binds the shader");
    }
    st::Scissor scissor;
    if (bound && o.openScissor) {
        st::Scissor::Seen seen;
        check(scissor.open(g_ctx.Get(), seen) == st::Scissor::Open::kOpened && seen.enabled, "the production scissor opens the game's rectangle");
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
    if (scissor.owed()) check(scissor.restore(g_ctx.Get()), "the game's scissor rectangles are put back");
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
    std::vector<std::array<float, 3>> lp;   // the first three floats of the record: for signature H the lighting position, which the stand-in vertex stage sets to (u, v, 0)
};
static float tagU(size_t i) { return 0.1f * float(i) + 0.05f; }
static float tagV(size_t i) { return 0.3f - 0.2f * float(i); }
// The points as triangles through the twin with the stream-output target: the clip position of every vertex as the game's shader made it (pre) and as the geometry shader wrote it (post).
static Captured captureMove(int fam, ID3D11GeometryShader* twin, const std::vector<Pt>& pts, const Cam& cam, const st::TurretParams& params) {
    std::vector<Vtx> v;
    for (size_t i = 0; i < pts.size(); ++i) v.push_back(vtxOf(pts[i], tagU(i), tagV(i)));
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
        m.lp.push_back({s.records[i * stride + 0], s.records[i * stride + 1], s.records[i * stride + 2]});
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
    {   // (n) the depth modes (fix.srv_turret_hud_depth): Elite's clip z is the constant 0.025, so a vertex the game had at view depth d has NDC z 0.025 / d. Translated 0.7 m forward it ends at d + 0.7.
        // game: its own NDC z where that is valid, else 0.999. real: the depth of the moved position, clamp(0.025 / (d + 0.7), 0, 0.999). front: 0.999 for every moved vertex.
        const float t[3] = {0.0f, 0.0f, 0.7f};
        const st::TurretParams base = st::translateParamsFor(frameOfCam(cam, nullptr, t));
        const float depths[7] = {-0.20f, 0.01f, 0.24f, 0.60f, 0.30f, -0.69f, -0.90f};   // behind the eye, inside the near plane, valid x3, ends 1 cm ahead of the eye, ends 20 cm behind it
        std::vector<Pt> pts;
        for (float d : depths) pts.push_back(Pt{0.01f * d, -0.02f * d, d, 0.025f});
        const st::DepthMode modes[3] = {st::DepthMode::kGame, st::DepthMode::kReal, st::DepthMode::kFront};
        for (const st::DepthMode mode : modes) {
            const st::TurretParams p = st::withDepth(base, mode);
            const Captured m = captureMove(fam, twin, pts, cam, p);
            char what[160];
            std::snprintf(what, sizeof(what), "(n) depth %s: every vertex is the CPU reference's", st::depthModeName(mode));
            expectReference(f, what, p, m, 7);
            bool right = true;
            for (size_t i = 0; i < pts.size(); ++i) {
                const double w = m.post[i][3], z = m.post[i][2], q = depths[i] + 0.7;
                right = right && closeTo(w, q, 1e-5);
                if (mode == st::DepthMode::kFront) {
                    right = right && closeTo(z, 0.999 * q, 2e-5);
                } else if (mode == st::DepthMode::kReal) {
                    const double want = std::fabs(q) > 1e-6 ? std::min(0.999, std::max(0.0, 0.025 / q)) : 0.999;
                    right = right && closeTo(z, want * q, 2e-5);
                } else {   // game: valid only where the game's own w is above 0.025 (z / w <= 1)
                    const double own = depths[i] > 0.025 ? 0.025 / depths[i] : 0.999;
                    right = right && closeTo(z, own * q, 2e-5);
                }
            }
            std::snprintf(what, sizeof(what), "(n) depth %s: each vertex's NDC z is the mode's (game: its own where valid; real: the moved position's own, clamped; front: 0.999)", st::depthModeName(mode));
            f.expect(right, what);
        }
        const Captured mg = captureMove(fam, twin, pts, cam, st::withDepth(base, st::DepthMode::kGame));
        const Captured mr = captureMove(fam, twin, pts, cam, st::withDepth(base, st::DepthMode::kReal));
        f.expect(mg.post[2][2] / mg.post[2][3] > 0.100 && mg.post[2][2] / mg.post[2][3] < 0.105 && mr.post[2][2] / mr.post[2][3] > 0.026 && mr.post[2][2] / mr.post[2][3] < 0.027,
                 "(n) a valid vertex 0.24 m out: game keeps its NDC z 0.025 / 0.24 (0.104), real gives the moved depth's 0.025 / 0.94 (0.0266)");
        f.expect(mr.post[5][2] / mr.post[5][3] > 0.998 && mr.post[6][2] == 0.0f && mr.post[6][3] < 0.0f,
                 "(n) real: a vertex ending 1 cm ahead of the eye is at 0.999 (the clamp), one ending 20 cm behind it at z 0 with a negative w (the rasterizer clips it)");
    }
    if (fam == int(st::kSceneSignature)) {   // (o) the Scorpion's hologram: the lighting position follows the vertex, by the camera's right, up and forward vectors
        const float right[3] = {0.8f, 0.0f, 0.6f}, up[3] = {-0.36f, 0.8f, 0.48f}, fwd[3] = {-0.48f, -0.6f, 0.64f};   // orthonormal, right x up = forward: not the identity
        const float a[3] = {0.10f, 0.05f, 0.0f}, t[3] = {0.02f, -0.01f, 0.7f};
        const std::vector<Pt> pts = {Pt{0.0f, 0.0f, 0.1f}, Pt{0.03f, 0.02f, 0.15f}, dirPt(0, 0, 1, 0.3f), dirPt(0.2f, 0.1f, 1, 0.8f), dirPt(-0.3f, 0.25f, 1, 1.4f), Pt{0.0f, 0.0f, -0.2f}};
        for (const UINT first : {0u, 16u}) {   // the vertex constants bound whole, and with a first constant of 16 (the range the game bound them with)
            setScene(right, up, fwd, first);
            const char* how = first ? "ranged" : "whole";
            char what[200];
            {   // translation and push
                const st::TurretParams p = st::pushParamsFor(frameOfCam(cam, a, t), kPushBackM);
                const Captured m = captureMove(fam, twin, pts, cam, p);
                std::snprintf(what, sizeof(what), "(o) the Scorpion hologram, constants %s: every vertex is the CPU reference's", how);
                expectReference(f, what, p, m, int(pts.size()));
                bool shifted = true;
                for (size_t i = 0; i < pts.size(); ++i) {
                    double in[4], out[4], disp[3];
                    for (int c = 0; c < 4; ++c) in[c] = m.pre[i][c];
                    st::anchoredMove<double>(p, in, out, nullptr, nullptr, nullptr, disp);
                    const double l0[3] = {tagU(i), tagV(i), 0.0};
                    for (int c = 0; c < 3; ++c) shifted = shifted && closeTo(m.lp[i][c], l0[c] + right[c] * disp[0] + up[c] * disp[1] + fwd[c] * disp[2], 1e-4);
                }
                std::snprintf(what, sizeof(what), "(o) constants %s: the lighting position moves by right * dX + up * dY + forward * dZ of the move (translation and push)", how);
                f.expect(shifted, what);
            }
            {   // the translation alone: the shift is T in the camera's axes, from the numbers, not from the CPU function
                const st::TurretParams p = st::translateParamsFor(frameOfCam(cam, a, t));
                const Captured m = captureMove(fam, twin, pts, cam, p);
                bool shifted = true;
                for (size_t i = 0; i < pts.size(); ++i) {
                    const double l0[3] = {tagU(i), tagV(i), 0.0};
                    for (int c = 0; c < 3; ++c) shifted = shifted && closeTo(m.lp[i][c], l0[c] + right[c] * 0.02 + up[c] * -0.01 + fwd[c] * 0.7, 1e-4);
                }
                std::snprintf(what, sizeof(what), "(o) constants %s: with the translation alone the lighting position moves by (0.02, -0.01, 0.7) in right, up, forward", how);
                f.expect(shifted, what);
            }
            {   // the push alone: a vertex outside the window is untouched, bit for bit
                const st::TurretParams p = st::pushParamsFor(frameOfCam(cam, a), kPushBackM);
                const Captured m = captureMove(fam, twin, pts, cam, p);
                bool same = true, changed = false;
                for (size_t i = 0; i < pts.size(); ++i) {
                    double in[4], out[4];
                    for (int c = 0; c < 4; ++c) in[c] = m.pre[i][c];
                    const bool mv = st::anchoredMove<double>(p, in, out);
                    const float l0[3] = {tagU(i), tagV(i), 0.0f};
                    if (mv) changed = changed || std::memcmp(m.lp[i].data(), l0, sizeof(l0)) != 0;
                    else same = same && std::memcmp(m.lp[i].data(), l0, sizeof(l0)) == 0;
                }
                std::snprintf(what, sizeof(what), "(o) constants %s: with the push alone a vertex the move leaves alone keeps its lighting position bit for bit, and one it moves does not", how);
                f.expect(same && changed, what);
            }
        }
        resetScene();
    }
    {   // (p) THE CONTENT DEPTH (the radar billboards' correction): with tr.w = 1 / z_c every vertex moves by T w / z_c, so a quad 2.5 m out lands on screen exactly where content at
        // z_c = 0.34 m on the same rays, moved by T, projects (by the camera's own projection); with tr.w = 0 the same vertices move by T at their own depth (as before); a vertex on the
        // eye plane is left alone
        const float t[3] = {0.012f, -0.008f, 0.03f};
        const double zc = 0.34;
        st::TurretParams p = st::translateParamsFor(frameOfCam(cam, nullptr, t));
        p.invZc = static_cast<float>(1.0 / zc);
        Pt c[4];
        boxAt(c, 0.10f, -0.05f, 2.5f, 0.3f, 0.3f);
        std::vector<Pt> pts(c, c + 4);
        pts.push_back(Pt{-0.02f, 0.03f, 0.6f});
        pts.push_back(Pt{0.0f, 0.0f, 0.34f});
        const Captured m = captureMove(fam, twin, pts, cam, p);
        expectReference(f, "(p) the content depth: every vertex is the CPU reference's", p, m, int(pts.size()));
        bool content = true;
        for (size_t i = 0; i < pts.size(); ++i) {
            double pin[3];
            eyePos(m.pre[i], cam, pin);
            const double k = zc / pin[2];
            const double C[3] = {pin[0] * k + t[0], pin[1] * k + t[1], zc + t[2]};
            const double ex = cam.p00 * C[0] / C[2] + cam.offX, ey = cam.p11 * C[1] / C[2] + cam.offY;
            content = content && closeTo(m.post[i][0] / m.post[i][3], ex, 2e-5) && closeTo(m.post[i][1] / m.post[i][3], ey, 2e-5);
        }
        f.expect(content, "(p) with 1 / z_c = 1 / 0.34 a vertex 2.5 m out (and 0.6 m, and 0.34 m) lands on screen where content at 0.34 m on its ray, moved by T, projects");
        double zIn[3], zOut[3];
        eyePos(m.pre[5], cam, zIn);
        eyePos(m.post[5], cam, zOut);
        f.expect(closeTo(zOut[0] - zIn[0], t[0], 3e-5) && closeTo(zOut[1] - zIn[1], t[1], 3e-5) && closeTo(zOut[2] - zIn[2], t[2], 3e-5), "(p) ...and a vertex at z_c itself moves by exactly T");
        double box[8] = {1e30, -1e30, 1e30, -1e30, 1e30, -1e30, 1e30, -1e30};   // the quad's NDC box before (x, y) and after
        for (size_t i = 0; i < 4; ++i) {
            const double v[4] = {m.pre[i][0] / m.pre[i][3], m.pre[i][1] / m.pre[i][3], m.post[i][0] / m.post[i][3], m.post[i][1] / m.post[i][3]};
            for (int j = 0; j < 4; ++j) {
                box[2 * j] = std::min(box[2 * j], v[j]);
                box[2 * j + 1] = std::max(box[2 * j + 1], v[j]);
            }
        }
        const double cx = (box[0] + box[1]) * 0.5, cy = (box[2] + box[3]) * 0.5;
        double e[2] = {};
        f.expect(st::contentShiftNdc(p, cx, cy, t, zc, e) && closeTo((box[4] + box[5]) * 0.5 - cx, e[0], 5e-5) && closeTo((box[6] + box[7]) * 0.5 - cy, e[1], 5e-5),
                 "(p) the quad probe's expectation (st::contentShiftNdc at the NDC box's centre) is the shift of the quad's NDC box centre");
        st::TurretParams plain = p;
        plain.invZc = 0.0f;
        const Captured mp = captureMove(fam, twin, pts, cam, plain);
        expectReference(f, "(p) tr.w = 0: every vertex is the CPU reference's", plain, mp, int(pts.size()));
        bool own = true;
        for (size_t i = 0; i < pts.size(); ++i) {
            double pin[3], pout[3];
            eyePos(mp.pre[i], cam, pin);
            eyePos(mp.post[i], cam, pout);
            own = own && closeTo(pout[0] - pin[0], t[0], 3e-5) && closeTo(pout[1] - pin[1], t[1], 3e-5) && closeTo(pout[2] - pin[2], t[2], 3e-5);
        }
        f.expect(own, "(p) with tr.w = 0 the same vertices move by T at their own depth, as before");
        const std::vector<Pt> plane = {Pt{0.1f, 0.05f, 0.0f}, Pt{-0.1f, 0.02f, 0.0f}, Pt{0.0f, -0.1f, 0.0f}};
        const Captured mz = captureMove(fam, twin, plane, cam, p);
        bool alone = true;
        for (size_t i = 0; i < plane.size(); ++i) alone = alone && std::memcmp(mz.pre[i].data(), mz.post[i].data(), 16) == 0;
        f.expect(alone, "(p) with the content depth a vertex on the eye plane (clip w 0) is still left as the game made it");
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

// ---------------------------------------------------------------- the radar anchor moved in the game (fix.srv_turret_radar_anchor): the key, the offset's bits, the recent test, the families
static void radarAnchorCases() {
    using A = st::RadarAnchorMode;
    check(st::radarAnchorModeOf("on") == A::kOn && st::radarAnchorModeOf("reversed") == A::kReversed && st::radarAnchorModeOf("off") == A::kOff && st::radarAnchorModeOf("") == A::kOn &&
              st::radarAnchorModeOf(nullptr) == A::kOn && st::radarAnchorModeOf("Off") == A::kOn && st::radarAnchorModeOf("reverse") == A::kOn,
          "the radar anchor key's text: on, reversed and off; anything else (and nothing) is on");
    check(!std::strcmp(st::radarAnchorModeName(A::kOn), "on") && !std::strcmp(st::radarAnchorModeName(A::kReversed), "reversed") && !std::strcmp(st::radarAnchorModeName(A::kOff), "off") &&
              st::kRadarAnchorDefault == A::kOn && st::radarAnchorModeOf(st::kRadarAnchorDefaultText) == st::kRadarAnchorDefault && !std::strcmp(st::kRadarAnchorDefaultText, "on"),
          "the modes' names are the key's values, and the default is on (the module's literal and the shipped ini say the same: the wiring scan)");
    const st::RadarAnchorOffset on45 = st::radarAnchorOffset(true, A::kOn, 45), rev30 = st::radarAnchorOffset(true, A::kReversed, 30);
    check(on45.bits[0] == 0u && on45.bits[1] == 0u && on45.bits[2] == 0x3EE66666u && on45.m[0] == 0.0f && on45.m[1] == 0.0f && on45.m[2] == 0.45f && !on45.zero(),
          "on at 45 cm: (0, 0, +0.45) m in the anchor's axes, the float's bits 0x3EE66666 in z and x, y zero");
    check(rev30.bits[0] == 0u && rev30.bits[1] == 0u && rev30.bits[2] == 0xBE99999Au && rev30.m[2] == -0.30f && !rev30.zero(), "reversed at 30 cm: (0, 0, -0.30) m, bits 0xBE99999A");
    const st::RadarAnchorOffset off = st::radarAnchorOffset(true, A::kOff, 45), notOn = st::radarAnchorOffset(false, A::kOn, 45), noF = st::radarAnchorOffset(true, A::kReversed, 0),
                                below = st::radarAnchorOffset(true, A::kOn, -5), big = st::radarAnchorOffset(true, A::kOn, 400);
    check(off.zero() && notOn.zero() && noF.zero() && below.zero() && noF.bits[2] == 0u && noF.m[2] == 0.0f,
          "zero (every bit 0, never a -0.0) with the key off, the view not on, or radar forward 0 (reversed included) or below");
    check(big.m[2] == 1.5f && big.bits[2] == 0x3FC00000u, "radar forward is held to its key's range (150 cm)");
    // The placement setter's rule: ADD to the game's translation (the first build replaced it and dropped the radar 1.96 m), never twice.
    using PM = st::PlacementMove;
    const auto bitsOf = [](float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; };
    const auto sameRows = [](const float* a, const float* b) { return !std::memcmp(a, b, 12 * sizeof(float)) && !std::memcmp(a + 15, b + 15, sizeof(float)); };
    const float P[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.96f, 0.0f, 1.0f};   // the Scorpion's: the hologram 1.96 m above the anchor
    const uint32_t zeroD[3] = {0u, 0u, 0u};
    st::PlacementMemory m;
    float Q[16], again[16];
    const PM first = st::radarPlacementMove(P, on45.bits, m, Q);
    check(first == PM::kAdded && Q[12] == 0.0f && Q[13] == 1.96f && Q[14] == 0.45f && sameRows(Q, P) && m.have && m.in[1] == bitsOf(1.96f) && m.out[2] == bitsOf(0.45f) &&
              m.d[2] == on45.bits[2],
          "the setter's rule ADDS the offset to the game's translation: (0, 1.96, 0) + (0, 0, 0.45) = (0, 1.96, 0.45), every other number of P as the game gave it");
    check(st::radarPlacementMove(P, on45.bits, m, again) == PM::kAdded && !std::memcmp(again, Q, sizeof(Q)),
          "the game setting its own value again (each frame) gets the same sum: nothing accumulates");
    float fed[16];
    check(st::radarPlacementMove(Q, on45.bits, m, fed) == PM::kOurs && !std::memcmp(fed, Q, sizeof(Q)),
          "our own value fed back bit for bit passes through unchanged (a caller that reads the placement back cannot make it climb)");
    float rebased[16];
    check(st::radarPlacementMove(Q, rev30.bits, m, rebased) == PM::kAdded && rebased[13] == 1.96f && rebased[14] == -0.30f && sameRows(rebased, P),
          "our value fed back after a key change is re-based on the game's translation it came from: (0, 1.96, -0.30), not (0, 1.96, 0.15)");
    float restored[16];
    check(st::radarPlacementMove(rebased, zeroD, m, restored) == PM::kRestored && restored[12] == 0.0f && restored[13] == 1.96f && restored[14] == 0.0f && !m.have,
          "our value fed back with the offset gone gets the game's own translation back, and the memory is cleared");
    float plain[16];
    st::PlacementMemory fresh;
    check(st::radarPlacementMove(P, zeroD, fresh, plain) == PM::kNone && !std::memcmp(plain, P, sizeof(P)) && !fresh.have, "no offset and the game's own value: P as the game gave it");
    float P2[16];
    std::memcpy(P2, P, sizeof(P2));
    P2[13] = 2.0f;
    float moved[16];
    check(st::radarPlacementMove(P, on45.bits, m, Q) == PM::kAdded && st::radarPlacementMove(P2, on45.bits, m, moved) == PM::kAdded && moved[13] == 2.0f && moved[14] == 0.45f,
          "a new value of the game's while ours is held is added to afresh (its own place, plus the offset)");
    check(!std::strcmp(st::placementMoveName(PM::kAdded), "added") && !std::strcmp(st::placementMoveName(PM::kOurs), "ours") && !std::strcmp(st::placementMoveName(PM::kRestored), "restored") &&
              !std::strcmp(st::placementMoveName(PM::kNone), "none"),
          "the rule's outcomes have names");
    // The direction (flight of 07bde936: the disc swung with the turret and sat about 5 cm high): d is F along the turret's seated forward u, in the anchor's axes.
    const auto near3 = [](const float* a, double x, double y, double z, double tol) { return std::fabs(a[0] - x) < tol && std::fabs(a[1] - y) < tol && std::fabs(a[2] - z) < tol; };
    const double ident[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    const float camRight[3] = {1.0f, 0.0f, 0.0f}, camUp[3] = {0.0f, 1.0f, 0.0f}, camFwd[3] = {0.0f, 0.0f, 1.0f};
    float uw[3];
    check(st::turretForwardWorld(camRight, camUp, camFwd, ident, uw) && near3(uw, 0.0, 0.0, 1.0, 1e-6), "an eye looking straight ahead in the seat: u is the live camera's forward");
    const double th = 30.0 / 57.29577951308232, c30 = std::cos(th), s30 = std::sin(th);
    const double pitched[9] = {1.0, 0.0, 0.0, 0.0, c30, s30, 0.0, -s30, c30};   // the eye looking 30 deg down in the seat (OpenVR axes: its back axis is (0, sin, cos))
    const float downUp[3] = {0.0f, float(c30), float(s30)}, downFwd[3] = {0.0f, float(-s30), float(c30)};   // ... so the live camera looks 30 deg down in the world
    check(st::turretForwardWorld(camRight, downUp, downFwd, pitched, uw) && near3(uw, 0.0, 0.0, 1.0, 1e-5), "a head pitched 30 deg down: the camera's pitch and the head's cancel, u is still the seat's forward");
    const float yawRight[3] = {0.0f, 0.0f, -1.0f}, yawFwd[3] = {1.0f, 0.0f, 0.0f};   // the turret turned 90 deg: the live camera looks along world +x
    check(st::turretForwardWorld(yawRight, camUp, yawFwd, ident, uw) && near3(uw, 1.0, 0.0, 0.0, 1e-6), "the turret turned 90 deg: u turns with it");
    const float scaledFwd[3] = {0.0f, 0.0f, 2.0f}, skewUp[3] = {0.0f, 0.8f, 0.6f}, nanFwd[3] = {0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN()};
    check(!st::turretForwardWorld(camRight, camUp, scaledFwd, ident, uw) && !st::turretForwardWorld(camRight, skewUp, camFwd, ident, uw) && !st::turretForwardWorld(camRight, camUp, nanFwd, ident, uw),
          "camera rows that are not unit, not orthogonal, or not finite give no u (the move falls back)");
    const float idA[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 5.0f, 6.0f, 7.0f, 1.0f};
    const double p75 = 7.5 / 57.29577951308232;
    const float seatFwd[3] = {0.0f, float(-std::sin(p75)), float(std::cos(p75))};   // the seated forward, 7.5 deg below the vehicle's level (DF3503CD's seated normal (0, 0.992, 0.130))
    float dd[3], ua[3];
    check(st::radarAnchorAlong(seatFwd, idA, 0.45f, dd, ua) && near3(dd, 0.0, -0.05874, 0.44615, 2e-5) && near3(ua, 0.0, -0.13053, 0.99144, 2e-5) &&
              std::fabs(st::angleToZDeg(ua) - 7.5) < 0.01 && std::fabs(st::yawDeg(ua)) < 1e-6,
          "anchor = identity, u 7.5 deg below level: d = 0.45 m along u, (0, -0.059, 0.446), angle to the anchor's z 7.5 deg");
    check(st::radarAnchorAlong(seatFwd, idA, -0.45f, dd, ua) && near3(dd, 0.0, 0.05874, -0.44615, 2e-5), "reversed: the same line the other way");
    const float worldX[3] = {1.0f, 0.0f, 0.0f};
    check(st::radarAnchorAlong(worldX, idA, 0.45f, dd, ua) && near3(dd, 0.45, 0.0, 0.0, 1e-6) && std::fabs(st::angleToZDeg(ua) - 90.0) < 1e-4 && std::fabs(st::yawDeg(ua) - 90.0) < 1e-4,
          "the turret 90 deg to the right of a vehicle facing the anchor's z: d along the anchor's x, yaw 90 deg");
    const float yawA[16] = {0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};   // a vehicle facing world +x
    check(st::radarAnchorAlong(worldX, yawA, 0.45f, dd, ua) && near3(dd, 0.0, 0.0, 0.45, 1e-6), "a vehicle facing world +x with the turret ahead: d along the anchor's z");
    float scaledA[16], skewA[16];
    std::memcpy(scaledA, idA, sizeof(idA));
    std::memcpy(skewA, idA, sizeof(idA));
    scaledA[0] = 2.0f;
    skewA[4] = 0.6f;
    skewA[5] = 0.8f;
    const float notUnit[3] = {0.0f, 0.0f, 0.5f};
    check(!st::radarAnchorAlong(seatFwd, scaledA, 0.45f, dd, ua) && !st::radarAnchorAlong(seatFwd, skewA, 0.45f, dd, ua) && !st::radarAnchorAlong(notUnit, idA, 0.45f, dd, ua),
          "an anchor whose rows are not unit or not orthogonal, or a u that is not unit: refused (the fallback (0, 0, s * F))");
    // The lag (flight of c1275025: the disc lagged the turret): u paired with the anchor of its own host frame, and the aim carried forward over u's age at the last rate, held.
    st::SeqRing<st::Rot9, 4> ring;
    st::Rot9 rotA, rotB;
    rotA.r[0] = 1.0f;
    rotB.r[0] = 2.0f;
    ring.put(10, rotA);
    ring.put(11, rotB);
    check(ring.find(10) && ring.find(10)->r[0] == 1.0f && ring.find(11) && ring.find(11)->r[0] == 2.0f && !ring.find(12) && !ring.find(0),
          "the anchor ring gives the rotation of exactly the sample's frame, and none for a frame it never saw (or frame 0)");
    rotA.r[0] = 3.0f;
    ring.put(10, rotA);
    ring.put(0, rotB);
    check(ring.find(10) && ring.find(10)->r[0] == 3.0f && !ring.find(0), "a second put of the same frame replaces it; frame 0 is never kept");
    ring.put(12, rotA);
    ring.put(13, rotA);
    ring.put(14, rotA);
    check(!ring.find(10) && ring.find(11) && ring.find(14), "a full ring forgets its oldest frame first");
    const float ahead[3] = {0.0f, 0.0f, 1.0f}, right90[3] = {1.0f, 0.0f, 0.0f};
    const float up10[3] = {0.0f, float(std::sin(10.0 / 57.29577951308232)), float(std::cos(10.0 / 57.29577951308232))};
    const st::Aim aAhead = st::aimOf(ahead), aRight = st::aimOf(right90), aUp = st::aimOf(up10);
    st::Aim probe;
    probe.yaw = 37.0;
    probe.pitch = -12.0;
    float probeU[3];
    st::unitOfAim(probe, probeU);
    const st::Aim back = st::aimOf(probeU);
    check(std::fabs(aAhead.yaw) < 1e-9 && std::fabs(aAhead.pitch) < 1e-9 && std::fabs(aRight.yaw - 90.0) < 1e-9 && std::fabs(aUp.pitch - 10.0) < 1e-4 && std::fabs(aUp.yaw) < 1e-9 &&
              std::fabs(back.yaw - 37.0) < 1e-4 && std::fabs(back.pitch + 12.0) < 1e-4 && near3(probeU, std::cos(-12.0 / 57.29577951308232) * std::sin(37.0 / 57.29577951308232),
                                                                                                 std::sin(-12.0 / 57.29577951308232),
                                                                                                 std::cos(-12.0 / 57.29577951308232) * std::cos(37.0 / 57.29577951308232), 1e-6),
          "the aim relative to the vehicle: yaw atan2(x, z), pitch asin(y), and back to a unit vector");
    check(std::fabs(st::wrapDeg(190.0) + 170.0) < 1e-9 && std::fabs(st::wrapDeg(-190.0) - 170.0) < 1e-9 && std::fabs(st::wrapDeg(45.0) - 45.0) < 1e-9, "yaw wraps into +-180");
    // The draw-time correction (flight of c8d79c6a: the lead was right on average, +-1 to 2 deg of the turret's own changes were left): delta = desired - used, in the world.
    {
        const double y5 = 5.0 / 57.29577951308232;
        const float uUsedA[3] = {float(std::sin(y5)), 0.0f, float(std::cos(y5))};   // the aim the shown call used, 5 deg right of the anchor's z
        const float sFc = 0.45f;
        const float dUsedC[3] = {sFc * uUsedA[0], sFc * uUsedA[1], sFc * uUsedA[2]};
        const float ident16[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 3.0f, 4.0f, 5.0f, 1.0f};
        const float uTrueZ[3] = {0.0f, 0.0f, 1.0f};   // the turret now straight ahead
        float dW[3], dS[3];
        check(st::radarCorrectionWorld(uTrueZ, ident16, sFc, dUsedC, dW, dS) && near3(dW, -sFc * std::sin(y5), 0.0, sFc * (1.0 - std::cos(y5)), 1e-6) && near3(dS, 0.0, 0.0, sFc, 1e-6),
              "anchor = identity: delta = sF (u_true - u_used), the desired offset sF along u_true");
        const float yawed[16] = {0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};   // a vehicle facing world +x
        const float uTrueX[3] = {1.0f, 0.0f, 0.0f};
        check(st::radarCorrectionWorld(uTrueX, yawed, sFc, dUsedC, dW, dS) && near3(dW, sFc * (1.0 - std::cos(y5)), 0.0, sFc * std::sin(y5), 1e-6) && near3(dS, 0.0, 0.0, sFc, 1e-6),
              "a rotated anchor: the used offset is put through the anchor's rows into the world, the desired one along u_true, and delta is their difference there");
        // A slightly scaled anchor (rows of length 1.03, which the setter still directs with): dStar is the setter's own offset for u_true (radarAnchorAlong on the same rows), so
        // the aim the call used gives exactly 0, and a different aim gives its difference put through the same rows.
        float scaled16[16];
        std::memcpy(scaled16, ident16, sizeof(scaled16));
        for (int i = 0; i < 3; ++i) scaled16[i * 5] = 1.03f;
        float dUsedS[3], uaS[3], dSetter[3];
        check(st::radarAnchorAlong(uUsedA, scaled16, sFc, dUsedS, uaS) && st::radarCorrectionWorld(uUsedA, scaled16, sFc, dUsedS, dW, dS) && near3(dW, 0.0, 0.0, 0.0, 1e-7),
              "a slightly scaled anchor: the aim the shown call used gives no correction (dStar is the setter's own offset)");
        check(st::radarAnchorAlong(uTrueZ, scaled16, sFc, dSetter, uaS) && st::radarCorrectionWorld(uTrueZ, scaled16, sFc, dUsedS, dW, dS) &&
                  near3(dS, dSetter[0], dSetter[1], dSetter[2], 1e-7) &&
                  near3(dW, 1.03 * (dSetter[0] - dUsedS[0]), 1.03 * (dSetter[1] - dUsedS[1]), 1.03 * (dSetter[2] - dUsedS[2]), 1e-6),
              "a slightly scaled anchor: dStar equals the setter's offset for u_true, and delta is the difference through the same rows");
        float scaled2[16];
        std::memcpy(scaled2, ident16, sizeof(scaled2));
        for (int i = 0; i < 3; ++i) scaled2[i * 5] = 2.0f;
        check(!st::radarCorrectionWorld(uTrueZ, scaled2, sFc, dUsedC, dW, dS), "an anchor the setter would not direct with (rows of length 2): no correction");
        const float uSame[3] = {uUsedA[0], uUsedA[1], uUsedA[2]};
        check(st::radarCorrectionWorld(uSame, ident16, sFc, dUsedC, dW, dS) && near3(dW, 0.0, 0.0, 0.0, 1e-7), "u_true equal to the used aim: no correction");
        check(st::radarCorrectionWorld(uTrueZ, ident16, -sFc, dUsedC, dW, dS) && near3(dS, 0.0, 0.0, -sFc, 1e-6), "reversed: the desired offset runs the other way");
        float skew16[16], uneven16[16];
        std::memcpy(skew16, ident16, sizeof(skew16));
        std::memcpy(uneven16, ident16, sizeof(uneven16));
        skew16[4] = 0.6f;
        skew16[5] = 0.8f;
        uneven16[0] = 1.5f;
        const float halfU[3] = {0.0f, 0.0f, 0.5f};
        check(!st::radarCorrectionWorld(uTrueZ, skew16, sFc, dUsedC, dW, dS) && !st::radarCorrectionWorld(uTrueZ, uneven16, sFc, dUsedC, dW, dS) &&
                  !st::radarCorrectionWorld(halfU, ident16, sFc, dUsedC, dW, dS) && !st::radarCorrectionWorld(uTrueZ, ident16, 0.0f, dUsedC, dW, dS),
              "an anchor whose rows are not orthogonal or not of one length, a u that is not unit, or no offset: no correction (the draw is the game's own)");
        check(st::kCorrectionMaxM == 0.50 && st::kCorrectionLargeM == 0.10, "the correction's bound is 0.5 m, a sanity bound (every draw left to the game pops); over 10 cm is applied and counted as large");
        // The eye conversion on a canted eye (10 deg about up, 3 deg about right): delta in the eye's axes, then signature H's mapping (l += R x + U y + F z) gives delta back.
        const double a10 = 10.0 / 57.29577951308232, a3 = 3.0 / 57.29577951308232;
        const double Rv[3] = {std::cos(a10), 0.0, -std::sin(a10)}, Fv0[3] = {std::sin(a10), 0.0, std::cos(a10)};
        const double Uv[3] = {0.0 * std::cos(a3) - Fv0[0] * std::sin(a3), std::cos(a3) - Fv0[1] * std::sin(a3), 0.0 - Fv0[2] * std::sin(a3)};
        const double Fv[3] = {Fv0[0] * std::cos(a3) + 0.0 * std::sin(a3), Fv0[1] * std::cos(a3) + std::sin(a3), Fv0[2] * std::cos(a3)};
        const float camRows[12] = {float(Rv[0]), float(Rv[1]), float(Rv[2]), 0.0f, float(Uv[0]), float(Uv[1]), float(Uv[2]), 0.0f, float(Fv[0]), float(Fv[1]), float(Fv[2]), 0.0f};
        const float delta[3] = {0.012f, -0.004f, 0.007f};
        float eyeD[3];
        st::radarDeltaInEye(camRows, delta, eyeD);
        float back[3];
        for (int j = 0; j < 3; ++j) back[j] = camRows[j] * eyeD[0] + camRows[4 + j] * eyeD[1] + camRows[8 + j] * eyeD[2];
        check(st::axesUsable(camRows, camRows + 4, camRows + 8) && near3(back, delta[0], delta[1], delta[2], 1e-6) && std::fabs(eyeD[2] - (Fv[0] * 0.012 - Fv[1] * 0.004 + Fv[2] * 0.007)) < 1e-6,
              "a canted eye: the world delta in the eye's axes, put back through signature H's lighting mapping, is the world delta again");
    }
    st::ResidualStats rs;
    rs.add(1.5, 1.5);    // lagging a full frame
    rs.add(-0.3, 1.5);   // leading
    rs.add(0.2, 0.1);    // too slow to say
    check(rs.n == 3 && rs.lagging == 1 && rs.leading == 1 && rs.latN == 2 && std::fabs(rs.mean() - (1.4 / 3.0)) < 1e-12 && std::fabs(rs.maxAbs - 1.5) < 1e-12 &&
              std::fabs(rs.latency() - ((1.0 - 0.2) / 2.0)) < 1e-12 && st::residualSide(-1.0, -2.0) == 1 && st::residualSide(1.0, -2.0) == -1 && st::residualSide(0.0, 2.0) == 0,
          "the residual: lagging with the rate's sign, leading against it, the latency left in frames as residual over rate, slow samples counted but not classed");    check(std::fabs(st::headPitchDeg(ident)) < 1e-9 && std::fabs(st::headPitchDeg(pitched) + 30.0) < 1e-6, "the head's pitch in the seat: 0 looking ahead, -30 deg looking 30 deg down");
    const float before[3] = {0.0f, 1.96f, 0.0f}, stepD[3] = {0.0f, 1.96f, 0.45f}, closeTo[3] = {0.0f, 1.96f, 0.40f}, other[3] = {0.0f, 2.0f, 0.0f}, d45[3] = {0.0f, 0.0f, 0.45f},
                dz[3] = {0.0f, 0.0f, 0.0f};
    check(st::placementStepLooksLikeOffset(before, stepD, d45) && st::placementStepLooksLikeOffset(before, closeTo, d45) && !st::placementStepLooksLikeOffset(before, other, d45) &&
              !st::placementStepLooksLikeOffset(before, stepD, dz),
          "the feedback warning: the game's translation stepping by about the offset (within a quarter of it) is suspect, another step is not, and nothing is with no offset");
    check(st::radarAnchorLeavesFamily(7) && st::radarAnchorLeavesFamily(8) && !st::radarAnchorLeavesFamily(0) && !st::radarAnchorLeavesFamily(2) && !st::radarAnchorLeavesFamily(6) &&
              !st::radarAnchorLeavesFamily(-1) && !st::radarAnchorLeavesFamily(9) && !std::strcmp(st::kFamilies[7].name, "5559BD94.EA02FAC2") && !std::strcmp(st::kFamilies[8].name, "5559BD94.E95634B0"),
          "the native move leaves the two 5559BD94 billboard families to the game and no other");
}

// ---------------------------------------------------------------- the depth modes and the displacement, on the CPU
// THE CONTENT DEPTH on the CPU (srv_turret_math.h): z_c from the V0 instrument only when it has one, read in the last 90 frames, 0.1 to 2 m; paramsUsable takes 1 / z_c in [0, 20] and
// nothing else; the CPU move puts a vertex where content at z_c on its ray, moved by T, projects (st::contentShiftNdc, the quad probe's expectation).
static void contentDepthCases() {
    check(st::kContentInvZMax == 20.0f && st::kContentDepthMinM == 0.10 && st::kContentDepthMaxM == 2.0 && st::kContentDepthFreshFrames == 90u, "the content depth's bounds: 1 / z_c up to 20, z_c 0.1 to 2 m, 90 frames");
    check(st::contentInvDepth(true, 0, 0.34) == static_cast<float>(1.0 / 0.34) && st::contentInvDepth(true, st::kContentDepthFreshFrames, 0.10) == static_cast<float>(1.0 / 0.10) &&
              st::contentInvDepth(true, 5, 2.0) == 0.5f,
          "a fresh V0 read 0.1 to 2 m deep is the content depth (1 / z_c)");
    check(st::contentInvDepth(false, 0, 0.34) == 0.0f && st::contentInvDepth(true, st::kContentDepthFreshFrames + 1, 0.34) == 0.0f && st::contentInvDepth(true, 0, 0.099) == 0.0f &&
              st::contentInvDepth(true, 0, 2.01) == 0.0f && st::contentInvDepth(true, 0, -0.34) == 0.0f && st::contentInvDepth(true, 0, std::nan("")) == 0.0f,
          "no V0 read, a stale one, or a depth out of 0.1..2 m (or not a number): no content depth (0, the plain delta)");
    st::EyeFrame ef;
    ef.l = -1.1f;
    ef.r = 1.0f;
    ef.dn = -1.05f;
    ef.up = 0.95f;
    ef.tRadar[0] = 0.012f;
    ef.tRadar[1] = -0.008f;
    ef.tRadar[2] = 0.03f;
    st::TurretParams cp = st::translateParamsFor(ef, st::Group::kRadar);
    bool usable = st::paramsUsable(cp);
    for (const float k : {static_cast<float>(1.0 / 0.34), 0.5f, 20.0f}) {
        cp.invZc = k;
        usable = usable && st::paramsUsable(cp);
    }
    for (const float k : {20.5f, -0.1f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        cp.invZc = k;
        usable = usable && !st::paramsUsable(cp);
    }
    check(usable, "the constants take 1 / z_c from 0 to 20 (z_c of 5 cm or more) and refuse more, less, NaN and infinity");
    cp.invZc = static_cast<float>(1.0 / 0.34);
    const float t[3] = {0.012f, -0.008f, 0.03f};
    bool same = true;
    for (const double w : {0.2, 0.34, 2.5, 7.0}) {
        const double x = 0.2, y = -0.1, in[4] = {x * w, y * w, 0.025, w};
        double out[4], e[2] = {};
        same = same && st::anchoredMove<double>(cp, in, out) && st::contentShiftNdc(cp, x, y, t, 1.0 / double(cp.invZc), e) && closeTo(out[0] / out[3] - x, e[0], 1e-9) &&
               closeTo(out[1] / out[3] - y, e[1], 1e-9);
    }
    check(same, "the CPU move with 1 / z_c shifts a vertex at any depth (0.2, 0.34, 2.5, 7 m) on screen by exactly the content's shift at z_c (st::contentShiftNdc)");
    double e[2] = {};
    check(!st::contentShiftNdc(cp, 0.0, 0.0, t, 0.0, e) && !st::contentShiftNdc(cp, 0.0, 0.0, t, -0.3, e), "no content shift without a content depth");
    cp.invZc = 0.0f;
    const double in[4] = {0.5, -0.25, 0.025, 2.5};
    double out[4];
    check(st::anchoredMove<double>(cp, in, out) && closeTo(out[3], 2.5 + double(cp.tz), 1e-12), "with 1 / z_c at 0 the CPU move is the plain translation (the quad's own depth moves by T.z)");
}

// THE RADAR'S OWN CAMERA (srv_turret_math.h; camera mode, the slot hook): the mode and its back distance; the camera's move against an independent inverse; the plane pointers' rebase; the
// radar-slot test over a fake memory; the direction rule.
static bool invert4(const double m[16], double out[16]) {   // Gauss-Jordan with partial pivoting, row-major
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-12) return false;
        if (piv != c)
            for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[piv][k]);
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] *= inv;
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                const double f = a[r][c];
                for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
    return true;
}
struct FakeMemory {   // a block of real memory whose reads succeed only inside it
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x4000, 0);
    uintptr_t at(size_t offset) const { return reinterpret_cast<uintptr_t>(bytes.data()) + offset; }
    template <class T>
    void put(size_t offset, T v) { std::memcpy(bytes.data() + offset, &v, sizeof(v)); }
};
static bool fakeRead(void* ctx, uintptr_t address, void* out, size_t n) {
    const FakeMemory& m = *static_cast<const FakeMemory*>(ctx);
    const uintptr_t lo = m.at(0);
    if (address < lo || address - lo > m.bytes.size() || m.bytes.size() - (address - lo) < n) return false;
    std::memcpy(out, reinterpret_cast<const void*>(address), n);
    return true;
}
static void slotCameraCases() {
    using A = st::RadarAnchorMode;
    check(st::radarAnchorModeOf("camera") == A::kCamera && !std::strcmp(st::radarAnchorModeName(A::kCamera), "camera") && st::radarAnchorModeOf("Camera") == A::kOn &&
              !std::strcmp(st::radarAnchorModeName(A::kOn), "on") && !std::strcmp(st::radarAnchorModeName(A::kOff), "off") && !std::strcmp(st::radarAnchorModeName(A::kReversed), "reversed"),
          "fix.srv_turret_radar_anchor takes camera (and only that spelling), and the settings line names it");
    check(st::radarAnchorOffset(true, A::kCamera, 65).zero() && !st::radarAnchorOffset(true, A::kOn, 65).zero(), "camera mode publishes a zero setter offset (the restore gives the game its placement back)");
    check(st::radarCameraBackM(true, A::kCamera, 65) == 0.65f && st::radarCameraBackM(true, A::kCamera, 500) == 1.5f && st::radarCameraBackM(false, A::kCamera, 65) == 0.0f &&
              st::radarCameraBackM(true, A::kOn, 65) == 0.0f && st::radarCameraBackM(true, A::kOff, 65) == 0.0f && st::radarCameraBackM(true, A::kCamera, 0) == 0.0f,
          "the slot camera's back distance is the radar forward key in metres (capped at 150 cm) while the view is on in camera mode, else 0");
    // the move: an orthonormal camera (yaw 30, pitch -12, roll 7 degrees) whose view is the inverse of its world
    const double yaw = 30.0 / 57.29577951308232, pitch = -12.0 / 57.29577951308232, roll = 7.0 / 57.29577951308232;
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch), cr = std::cos(roll), sr = std::sin(roll);
    const double Ry[9] = {cy, 0, -sy, 0, 1, 0, sy, 0, cy}, Rx[9] = {1, 0, 0, 0, cp, sp, 0, -sp, cp}, Rz[9] = {cr, sr, 0, -sr, cr, 0, 0, 0, 1};
    double t1[9] = {}, R[9] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) t1[i * 3 + j] += Rz[i * 3 + k] * Rx[k * 3 + j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) R[i * 3 + j] += t1[i * 3 + k] * Ry[k * 3 + j];
    for (double& r : R) r = double(float(r));   // the float camera is the truth: the inverse below is of exactly what the camera holds
    double world[16] = {R[0], R[1], R[2], 0, R[3], R[4], R[5], 0, R[6], R[7], R[8], 0, 1.25, -0.5, 2.75, 1}, view[16];
    check(invert4(world, view), "the test camera's world inverts");
    alignas(16) uint8_t cam[st::kCamBytes] = {};
    for (size_t i = 0; i < st::kCamBytes; ++i) cam[i] = static_cast<uint8_t>(i * 7 + 3);   // every byte the move must not touch has a value
    float wf[16], vf[16];
    for (int i = 0; i < 16; ++i) {
        wf[i] = static_cast<float>(world[i]);
        vf[i] = static_cast<float>(view[i]);
    }
    std::memcpy(cam, wf, sizeof(wf));
    std::memcpy(cam + st::kCamViewAt, vf, sizeof(vf));
    const uint32_t flags0 = 0x5;
    std::memcpy(cam + st::kCamFlagsAt, &flags0, sizeof(flags0));
    uint8_t before[st::kCamBytes];
    std::memcpy(before, cam, sizeof(before));
    const float d[3] = {0.11f, -0.07f, 0.23f};
    st::cameraMoveBy(cam, d);
    float wAfter[16], vAfter[16];
    std::memcpy(wAfter, cam, sizeof(wAfter));
    std::memcpy(vAfter, cam + st::kCamViewAt, sizeof(vAfter));
    double world2[16], view2[16];
    for (int i = 0; i < 16; ++i) world2[i] = wAfter[i];
    check(invert4(world2, view2), "the moved camera's world inverts");
    bool inverse = true;
    for (int i = 0; i < 16; ++i) inverse = inverse && std::fabs(double(vAfter[i]) - view2[i]) <= 1e-6 * std::max(1.0, std::fabs(view2[i]));
    check(inverse && world2[12] == double(wf[12] + d[0]) && world2[13] == double(wf[13] + d[1]) && world2[14] == double(wf[14] + d[2]),
          "the camera moved by d: its position by d, and its view is the inverse of the moved world (an independent inverse, to 1e-6)");
    uint32_t flags1 = 0;
    std::memcpy(&flags1, cam + st::kCamFlagsAt, sizeof(flags1));
    bool untouched = true;
    for (size_t i = 0; i < st::kCamBytes; ++i) {
        const bool written = (i >= st::kCamPositionAt && i < st::kCamPositionAt + 12) || (i >= st::kCamViewAt + 48 && i < st::kCamViewAt + 60) || (i >= st::kCamFlagsAt && i < st::kCamFlagsAt + 4);
        if (!written) untouched = untouched && cam[i] == before[i];
    }
    check(flags1 == (flags0 | st::kCamVpDirty) && st::kCamVpDirty == 0x8u && untouched,
          "...VP is marked dirty (bit 3, the others kept), and nothing else of the camera changes (the projection, the frustum, the rotation)");
    std::memcpy(cam, before, sizeof(before));
    const float D = 0.65f, back[3] = {static_cast<float>(-D * R[6]), static_cast<float>(-D * R[7]), static_cast<float>(-D * R[8])};
    st::cameraMoveBy(cam, back);
    float vBack[16];
    std::memcpy(vBack, cam + st::kCamViewAt, sizeof(vBack));
    check(std::fabs(double(vBack[14]) - (double(vf[14]) + D)) <= 2e-6 * std::max(1.0, std::fabs(double(vf[14]))) && std::fabs(double(vBack[12]) - vf[12]) <= 1e-4 &&
              std::fabs(double(vBack[13]) - vf[13]) <= 1e-4,
          "d = -D times the camera's forward: view row 3's z grows by exactly D, its x and y stay");
    // the plane pointers' rebase
    alignas(16) uint8_t copy[st::kCamBytes] = {};
    const uintptr_t src = 0x7FF612340000ull, to = reinterpret_cast<uintptr_t>(copy);
    const auto rebased = [&](uintptr_t a, uintptr_t b, uintptr_t& ra, uintptr_t& rb) {
        std::memcpy(copy + st::kCamPlanePtrAt[0], &a, 8);
        std::memcpy(copy + st::kCamPlanePtrAt[1], &b, 8);
        st::cameraRebase(copy, src);
        std::memcpy(&ra, copy + st::kCamPlanePtrAt[0], 8);
        std::memcpy(&rb, copy + st::kCamPlanePtrAt[1], 8);
    };
    uintptr_t ra = 0, rb = 0;
    rebased(src + 0x90, src + 0xF0, ra, rb);
    bool rebase = ra == to + 0x90 && rb == to + 0xF0;
    rebased(src + 0x90 + 0x30, src + 0xF0 + 0x70, ra, rb);
    rebase = rebase && ra == to + 0xC0 && rb == to + 0x160;
    rebased(src + 0xF0 + 0x10, src + 0x90 + 0x10, ra, rb);
    rebase = rebase && ra == to + 0x100 && rb == to + 0xA0;
    check(rebase, "a plane pointer into either of the source's inline blocks (+0x90, 6 x 16; +0xF0, 8 x 16) points at the same place in the copy");
    rebased(src + 0x8F, src + 0x170, ra, rb);
    bool kept = ra == src + 0x8F && rb == src + 0x170;
    rebased(0x1234560000ull, 0, ra, rb);
    kept = kept && ra == 0x1234560000ull && rb == 0;
    check(kept, "...and one outside them (just before, just past, another object's, null) is kept");
    // the radar-slot test over a fake memory: slot 0x000, manager 0x1000, lists 0x1200, component array 0x1400, component 0x1800, link 0x2000, descriptor 0x2400
    const uintptr_t slotVt = 0x1454FB7F8ull, linkVt = 0x145511F18ull;
    const auto build = [&](FakeMemory& m) {
        m.put<uint64_t>(0x0, slotVt);
        m.put<uint32_t>(st::kSlotIndexAt, 1);
        m.put<uint64_t>(st::kSlotManagerAt, m.at(0x1000));
        m.put<uint32_t>(st::kSlotListAt, 2);
        m.put<uint64_t>(0x1000 + st::kManagerCountAt, 3);
        m.put<uint64_t>(0x1000 + st::kManagerListsAt, m.at(0x1200));
        m.put<uint64_t>(0x1200 + 1 * st::kManagerEntryBytes + st::kEntryCountAt, 2);
        m.put<uint64_t>(0x1200 + 1 * st::kManagerEntryBytes + st::kEntryDataAt, m.at(0x1400));
        m.put<uint64_t>(0x1400, 0);   // a null entry is passed over
        m.put<uint64_t>(0x1408, m.at(0x1800));
        m.put<uint64_t>(0x1800 + st::kComponentLinkAt, m.at(0x2000));
        m.put<uint64_t>(0x2000, linkVt);
        m.put<uint64_t>(0x1800 + st::kComponentDescAt, m.at(0x2400));
        m.put<uint32_t>(0x2400 + st::kDescModeAt, st::kRadarModeIcon);
    };
    const auto test = [&](FakeMemory& m, st::RadarSlot& out) { return st::radarSlotTest(m.at(0), slotVt, linkVt, &fakeRead, &m, out); };
    {
        FakeMemory m;
        build(m);
        st::RadarSlot hit;
        check(test(m, hit) == 1 && hit.index == 1 && hit.mode == st::kRadarModeIcon && hit.list == 2, "a slot with the radar's component at its index is the radar's: index 1, mode 5 (the icon), list B");
    }
    const auto miss = [&](const std::function<void(FakeMemory&)>& edit, int want) {
        FakeMemory m;
        build(m);
        edit(m);
        st::RadarSlot hit;
        return test(m, hit) == want;
    };
    check(miss([](FakeMemory& m) { m.put<uint64_t>(0x0, 0x1454FB800ull); }, 0), "...not with another slot vtable");
    check(miss([](FakeMemory& m) { m.put<uint64_t>(0x1000 + st::kManagerCountAt, 1); }, 0), "...not with its index past the manager's lists");
    check(miss([](FakeMemory& m) { m.put<uint64_t>(st::kSlotManagerAt, 0); }, 0), "...not without a manager");
    check(miss([](FakeMemory& m) { m.put<uint64_t>(0x1200 + 1 * st::kManagerEntryBytes + st::kEntryCountAt, 65); }, 0), "...not with more than 64 components at its index");
    check(miss([](FakeMemory& m) { m.put<uint64_t>(0x1800 + st::kComponentLinkAt, 0); }, 0), "...not when the component has no link");
    check(miss([](FakeMemory& m) { m.put<uint64_t>(0x2000, 0x145511DE8ull); }, 0), "...not when the link's vtable is not the radar interface's");
    check(miss([](FakeMemory& m) { m.put<uint64_t>(0x1000 + st::kManagerListsAt, 0x10); }, -1), "...and a read that fails is a fault (-1), not an answer");
    check(miss([](FakeMemory& m) { m.put<uint32_t>(0x2400 + st::kDescModeAt, st::kRadarModeTerrain); m.put<uint32_t>(st::kSlotListAt, 0); }, 1), "...and the terrain host's slot in list A is the radar's too");
    // the direction rule
    const int64_t f = 10000000;
    const float uRaw[3] = {0.0f, 0.0f, 2.0f}, camF[3] = {0.6f, 0.0f, 0.8f}, zero3[3] = {0.0f, 0.0f, 0.0f};
    float dd[3];
    using F = st::CameraBackFrom;
    bool rule = st::radarCameraBack(0.65f, true, f / 20, f, uRaw, camF, dd) == F::kTurret && dd[0] == 0.0f && dd[1] == 0.0f && std::fabs(dd[2] + 0.65f) < 1e-7f;
    rule = rule && st::radarCameraBack(0.65f, true, f / 5, f, uRaw, camF, dd) == F::kCamera && std::fabs(dd[0] + 0.39f) < 1e-6f && std::fabs(dd[2] + 0.52f) < 1e-6f;
    rule = rule && st::radarCameraBack(0.65f, false, 0, f, uRaw, camF, dd) == F::kCamera && st::radarCameraBack(0.65f, true, -5, f, uRaw, camF, dd) == F::kCamera;
    rule = rule && st::radarCameraBack(0.65f, true, 0, f, zero3, camF, dd) == F::kCamera;
    check(rule, "the slot camera moves back D along the turret's forward (unit) from a sample written within 100 ms; a stale, missing, future-dated or zero one falls back to the camera's own forward");
    bool none = st::radarCameraBack(0.65f, false, 0, f, nullptr, zero3, dd) == F::kNone && dd[0] == 0.0f && dd[1] == 0.0f && dd[2] == 0.0f;
    none = none && st::radarCameraBack(0.0f, true, 0, f, uRaw, camF, dd) == F::kNone && dd[2] == 0.0f;
    none = none && st::radarCameraBack(std::numeric_limits<float>::quiet_NaN(), true, 0, f, uRaw, camF, dd) == F::kNone && st::radarCameraBack(-0.3f, true, 0, f, uRaw, camF, dd) == F::kNone;
    check(none, "...and D of 0 (or negative, or NaN), or no usable direction at all, is no move");
}

static void depthMathCases() {
    using M = st::DepthMode;
    check(st::depthModeOf("game") == M::kGame && st::depthModeOf("real") == M::kReal && st::depthModeOf("front") == M::kFront && st::depthModeOf("") == M::kGame &&
              st::depthModeOf(nullptr) == M::kGame && st::depthModeOf("Real") == M::kGame && st::depthModeOf("rea") == M::kGame && st::depthModeOf("off") == M::kGame,
          "the depth key's text: game, real and front; anything else (and nothing) is game");
    check(!std::strcmp(st::depthModeName(M::kGame), "game") && !std::strcmp(st::depthModeName(M::kReal), "real") && !std::strcmp(st::depthModeName(M::kFront), "front"),
          "the modes' names are the key's values");
    const float t[3] = {0.0f, 0.0f, 0.7f};
    const st::EyeFrame e = frameOfCam(Cam{}, nullptr, t);
    const st::TurretParams game = st::translateParamsFor(e), real = st::withDepth(game, M::kReal), front = st::withDepth(game, M::kFront);
    check(st::kDepthDefault == M::kFront && st::depthModeOf(st::kDepthDefaultText) == st::kDepthDefault && !std::strcmp(st::kDepthDefaultText, "front") && !std::strcmp(st::kScissorDefaultText, "open"),
          "the depth key's default is front, the scissor key's open (the module's literals and the shipped ini say the same: the wiring scan)");
    check(game.depth == 0.0f && real.depth == 1.0f && front.depth == 2.0f && st::withDepth(front, M::kGame) == game && !(real == game) && !(front == real),
          "the constants carry the mode as 0, 1 or 2, and a different mode is a different set of numbers (the buffer is rewritten)");
    st::TurretParams bad = game;
    bool refused = true;
    for (float v : {3.0f, -1.0f, 0.5f, 1.5f, NAN, INFINITY}) {
        bad.depth = v;
        refused = refused && !st::paramsUsable(bad);
    }
    check(refused && st::paramsUsable(game) && st::paramsUsable(real) && st::paramsUsable(front), "a depth that is not 0, 1 or 2 is refused; the three modes are usable");
    const auto at = [&](const st::TurretParams& p, double w, double* disp = nullptr, bool* moved = nullptr) {   // NDC z after the move of a vertex at clip w with the constant clip z 0.025
        const double in[4] = {0.02 * w, 0.01 * w, 0.025, w};
        double out[4];
        bool z = false;
        const bool mv = st::anchoredMove<double>(p, in, out, nullptr, nullptr, &z, disp);
        if (moved) *moved = mv;
        return out[2] / out[3];
    };
    double d[3] = {};
    check(closeTo(at(game, 0.24, d), 0.025 / 0.24, 1e-6) && closeTo(at(real, 0.24), 0.025 / 0.94, 1e-6) && closeTo(at(front, 0.24), 0.999, 1e-6),
          "a valid vertex at 0.24 m, moved 0.7 m: game keeps 0.025 / 0.24, real gives 0.025 / 0.94, front 0.999");
    check(closeTo(d[0], 0.0, 1e-6) && closeTo(d[1], 0.0, 1e-6) && closeTo(d[2], 0.7, 1e-6), "...and the displacement is the translation (0, 0, 0.7)");
    check(closeTo(at(game, 0.01), 0.999, 1e-6) && closeTo(at(real, 0.01), 0.025 / 0.71, 1e-6) && closeTo(at(game, -0.2), 0.999, 1e-6) && closeTo(at(real, -0.2), 0.05, 1e-6),
          "a vertex inside the near plane or behind the eye (no valid depth): game 0.999, real the moved position's");
    check(closeTo(at(real, -0.69), 0.999, 1e-6) && at(real, -0.9) == 0.0 / -0.2 && closeTo(at(game, -0.9), 0.999, 1e-6),
          "real clamps: a vertex ending 1 cm ahead of the eye is 0.999, one ending behind it 0; game gives 0.999 to both");
    bool moved = true;
    double zero[3] = {1, 1, 1};
    const st::TurretParams still = st::pushParamsFor(frameOfCam(Cam{}), kPushBackM);
    (void)at(still, 1.4, zero, &moved);
    check(!moved && zero[0] == 0.0 && zero[1] == 0.0 && zero[2] == 0.0, "a vertex left alone has no displacement");
}

// ---------------------------------------------------------------- the disc's plane in the seated frame, and the unmoved-draw list (CPU only)
static void yawPose(float m[12], double degrees, double x, double y, double z) {   // a 3x4 row-major pose: a turn about Y, then a position
    const double a = degrees * 3.14159265358979323846 / 180.0, c = std::cos(a), s = std::sin(a);
    const double rot[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i * 4 + j] = float(rot[i * 3 + j]);
    m[3] = float(x);
    m[7] = float(y);
    m[11] = float(z);
}
// The six records of a seated quad (two triangles: 0 1 2 and 1 3 2) as the game's vertex stage would write their clip positions for the eye (R, E) with these tangents: the inverse of quadInSeated.
static void recordsOfQuad(const double R[9], const double E[3], const double pts[4][3], const float tn[4], float* rec, uint32_t stride, uint32_t pre) {
    const int tri[6] = {0, 1, 2, 1, 3, 2};
    for (int i = 0; i < 6; ++i) {
        float* r = rec + size_t(i) * stride;
        for (uint32_t k = 0; k < stride; ++k) r[k] = 99.0f;   // the other elements of the record: not read
        const double d[3] = {pts[tri[i]][0] - E[0], pts[tri[i]][1] - E[1], pts[tri[i]][2] - E[2]};
        double v[3];
        for (int j = 0; j < 3; ++j) v[j] = R[0 * 3 + j] * d[0] + R[1 * 3 + j] * d[1] + R[2 * 3 + j] * d[2];   // R^T d
        const double X = v[0], Y = v[1], Z = -v[2];
        r[pre + 0] = float((2.0 * X - Z * (double(tn[1]) + tn[0])) / (double(tn[1]) - tn[0]));
        r[pre + 1] = float((2.0 * Y - Z * (double(tn[3]) + tn[2])) / (double(tn[3]) - tn[2]));
        r[pre + 2] = 0.025f;
        r[pre + 3] = float(Z);
    }
}
static void planeAndUnmovedCases() {
    const float tn[4] = {-1.0f, 1.2f, -0.9f, 1.1f};
    float headA[12], headB[12], eyeA[12], eyeB[12];
    yawPose(headA, 0.0, 0.05, 0.10, 0.02);
    yawPose(headB, 35.0, -0.12, 0.14, 0.20);   // the head turned 35 degrees and moved
    yawPose(eyeA, 3.0, -0.032f, 0.004f, 0.015f);   // the left eye, off the head centre and turned a little
    for (int i = 0; i < 12; ++i) eyeB[i] = eyeA[i];
    double RA[9], EA[3], RB[9], EB[3];
    check(st::eyePoseOf(headA, eyeA, RA, EA) && st::eyePoseOf(headB, eyeB, RB, EB), "an eye's pose is the head's times the eye's");
    float bad[12];
    std::memcpy(bad, headA, sizeof(bad));
    bad[5] = NAN;
    double Rx[9], Ex[3];
    check(!st::eyePoseOf(bad, eyeA, Rx, Ex), "a pose that is not finite: refused");
    {   // eyeToSeated is the inverse of eyeFrameFrom's step for the anchor
        const double anchor[3] = {0.20, -0.10, -0.50};
        st::EyeFrame e;
        check(st::eyeFrameFrom(headB, eyeB, anchor, 0.0, 0.0, tn, e), "an eye frame is built");
        const double a[3] = {e.a[0], e.a[1], e.a[2]};
        double back[3];
        st::eyeToSeated(RB, EB, a, back);
        check(closeTo(back[0], anchor[0], 1e-5) && closeTo(back[1], anchor[1], 1e-5) && closeTo(back[2], anchor[2], 1e-5), "the seated point of the anchor's eye-space point is the anchor (the inverse of eyeFrameFrom's step)");
    }
    // a horizontal square 0.4 m on a side, 0.35 m below the head and 0.45 m ahead (seated -z is forward)
    const double C[3] = {0.10, -0.35, -0.45};
    const double square[4][3] = {{C[0] - 0.2, C[1], C[2] - 0.2}, {C[0] + 0.2, C[1], C[2] - 0.2}, {C[0] - 0.2, C[1], C[2] + 0.2}, {C[0] + 0.2, C[1], C[2] + 0.2}};
    const uint32_t stride = 10, pre = 6;
    float recA[6 * 10], recB[6 * 10];
    recordsOfQuad(RA, EA, square, tn, recA, stride, pre);
    recordsOfQuad(RB, EB, square, tn, recB, stride, pre);
    st::SeatedQuad qa, qb;
    check(st::quadInSeated(recA, stride, pre, 0, 6, tn, RA, EA, qa) && st::quadInSeated(recB, stride, pre, 0, 6, tn, RB, EB, qb), "a quad's six records are read back");
    check(closeTo(qa.centre[0], C[0], 1e-4) && closeTo(qa.centre[1], C[1], 1e-4) && closeTo(qa.centre[2], C[2], 1e-4) && closeTo(qa.normal[0], 0.0, 1e-4) && closeTo(qa.normal[1], -1.0, 1e-4) &&
              closeTo(qa.normal[2], 0.0, 1e-4),
          "...the centre is the square's and the normal its vertical (the winding's sign) in the seated frame");
    check(closeTo(qb.centre[0], C[0], 1e-4) && closeTo(qb.centre[1], C[1], 1e-4) && closeTo(qb.centre[2], C[2], 1e-4) && st::angleBetweenDeg(qa.normal, qb.normal) < 0.01,
          "a turret-fixed square seen from a head that moved and turned 35 degrees has the same centre and normal");
    {   // a billboard: the square turned toward the moved head (a tilt of 40 degrees about the seated X axis through its centre) has the normal turned by that much
        const double a = 40.0 * 3.14159265358979323846 / 180.0, c = std::cos(a), s = std::sin(a);
        double tilted[4][3];
        for (int i = 0; i < 4; ++i) {
            const double dy = square[i][1] - C[1], dz = square[i][2] - C[2];
            tilted[i][0] = square[i][0];
            tilted[i][1] = C[1] + c * dy - s * dz;
            tilted[i][2] = C[2] + s * dy + c * dz;
        }
        float recT[6 * 10];
        recordsOfQuad(RB, EB, tilted, tn, recT, stride, pre);
        st::SeatedQuad qt;
        check(st::quadInSeated(recT, stride, pre, 0, 6, tn, RB, EB, qt) && closeTo(st::angleBetweenDeg(qa.normal, qt.normal), 40.0, 1e-4) && closeTo(qt.centre[0], C[0], 1e-4) &&
                  closeTo(qt.centre[1], C[1], 1e-4) && closeTo(qt.centre[2], C[2], 1e-4),
              "a square turned 40 degrees in the seated frame reads as a normal 40 degrees from the fixed one, with the same centre");
        check(closeTo(st::angleBetweenDeg(qa.normal, qa.normal), 0.0, 1e-9), "an angle with itself is 0");
    }
    {   // the second quad of a capture, and the refusals
        float two[12 * 10];
        std::memcpy(two, recA, sizeof(recA));
        std::memcpy(two + 6 * 10, recB, sizeof(recB));
        st::SeatedQuad q2;
        check(st::quadInSeated(two, stride, pre, 6, 12, tn, RB, EB, q2) && closeTo(q2.centre[1], C[1], 1e-4), "the second quad of a capture (records 6 to 11)");
        check(!st::quadInSeated(two, stride, pre, 7, 12, tn, RB, EB, q2) && !st::quadInSeated(two, stride, pre, 0, 5, tn, RB, EB, q2) && !st::quadInSeated(nullptr, stride, pre, 0, 6, tn, RB, EB, q2) &&
                  !st::quadInSeated(two, 9, pre, 0, 12, tn, RB, EB, q2),
              "fewer than six records from the first, no records, a stride that does not hold the record: refused");
        float nan6[6 * 10];
        std::memcpy(nan6, recA, sizeof(nan6));
        nan6[3 * 10 + pre + 1] = NAN;
        float flat[6 * 10];
        for (int i = 0; i < 6; ++i)
            for (uint32_t k = 0; k < stride; ++k) flat[i * 10 + k] = recA[k];   // six copies of the first record: no area
        check(!st::quadInSeated(nan6, stride, pre, 0, 6, tn, RA, EA, q2) && !st::quadInSeated(flat, stride, pre, 0, 6, tn, RA, EA, q2), "a record that is not finite, a quad with no area: refused");
    }
    {   // the angle between vectors
        const double x[3] = {1, 0, 0}, y[3] = {0, 1, 0}, mx[3] = {-1, 0, 0}, zero[3] = {0, 0, 0};
        check(closeTo(st::angleBetweenDeg(x, y), 90.0, 1e-9) && closeTo(st::angleBetweenDeg(x, mx), 180.0, 1e-9) && st::angleBetweenDeg(x, zero) == 0.0, "the angle between vectors: 90, 180, and 0 for a vector with no length");
    }
    {   // the radar picture's camera, from the 4EDE4BC9 draw's cb0 rows 4..7 and 9..11 as logged in the flight of 707c0ab5 (round 1, eye 0; the offline decomposition gave these numbers)
        const float m[16] = {0.013715f, 0.00028609f, 0.00026257f, -0.041348f, -0.00058093f, 0.011863f, 0.016461f, -4.941f,
                             8.6297e-12f, 4.9187e-11f, -4.7421e-10f, 0.01f, -3.6195e-05f, -0.00020631f, 0.001989f, -0.43696f};
        const float a[12] = {-0.00078334f, -0.00067174f, 0.0017132f, -0.33076f, 0.0001677f, 0.0018282f, 0.00079349f, -0.36006f, -0.0018326f, 0.00045444f, -0.00065972f, 0.10097f};
        st::RadarCam rc;
        check(st::radarCamFrom(m, a, rc), "the radar camera is decoded from the logged constants");
        check(closeTo(rc.centre[0], -0.00655, 2e-4) && closeTo(rc.centre[1], -0.24137, 2e-4) && closeTo(rc.centre[2], -0.43696, 2e-4),
              "...the schematic's origin sits at (-0.007, -0.241, -0.437) in its camera: 0.44 m behind it");
        check(closeTo(rc.fwd[0], 0.89364, 1e-4) && closeTo(rc.fwd[1], 0.29875, 1e-4) && closeTo(rc.fwd[2], -0.3349, 1e-4), "...the camera's forward is the 256x256 writers' cb1[279]");
        check(closeTo(rc.tanX[0], -0.14455, 2e-4) && closeTo(rc.tanX[1], 0.14699, 2e-4) && closeTo(rc.tanY[0], -1.27079, 2e-4) && closeTo(rc.tanY[1], -0.97452, 2e-4) && closeTo(rc.scale, 0.002, 1e-5),
              "...the window is tan x -0.145..0.147, tan y -1.271..-0.975, the local scale 0.002");
        float singular[12] = {};
        float nanm[16];
        std::memcpy(nanm, m, sizeof(nanm));
        nanm[5] = NAN;
        check(!st::radarCamFrom(m, singular, rc) && !st::radarCamFrom(nanm, a, rc) && !st::radarCamFrom(nullptr, a, rc), "a singular model, a value that is not finite, no constants: refused");
    }
    {   // the unmoved list as the 10 s line spells it
        uint32_t counts[st::kFamilyCount + 1][st::kUnmovedRoutes] = {};
        char out[512];
        check(st::unmovedList(out, sizeof(out), counts) == 4 && !std::strcmp(out, "none"), "no unmoved draw: `none`");
        counts[2][size_t(st::UnmovedRoute::kForeignDeferred)] = 12;
        counts[0][size_t(st::UnmovedRoute::kFastPath)] = 3;
        counts[st::kFamilyCount][size_t(st::UnmovedRoute::kCommandList)] = 2;
        const size_t n = st::unmovedList(out, sizeof(out), counts);
        check(n == std::strlen(out) && !std::strcmp(out, "1989E6D3 fast-path 3, DF3503CD foreign-deferred 12, ? command-list 2"), "the list is `<family> <route> N` in family then route order, `?` for the unknown family");
        char narrow[40];
        const size_t m = st::unmovedList(narrow, sizeof(narrow), counts);
        check(m == std::strlen(narrow) && m < sizeof(narrow) && !std::strcmp(narrow, "1989E6D3 fast-path 3"), "a list that does not fit ends at the last whole entry, NUL-terminated");
        char one[3];
        check(st::unmovedList(one, sizeof(one), counts) == 0 && one[0] == 0 && st::unmovedList(nullptr, 10, counts) == 0 && st::unmovedList(out, 0, counts) == 0, "a buffer too small for any entry: empty; no buffer: nothing");
        bool names = true;
        for (uint32_t r = 0; r < st::kUnmovedRoutes; ++r) {
            const auto route = static_cast<st::UnmovedRoute>(r);
            names = names && std::strcmp(st::unmovedRouteName(route), "?") != 0 && std::strcmp(st::unmovedRouteWhere(route), "?") != 0;
            for (uint32_t q = 0; q < r; ++q) names = names && std::strcmp(st::unmovedRouteName(route), st::unmovedRouteName(static_cast<st::UnmovedRoute>(q))) != 0;
        }
        check(names && st::kUnmovedRoutes == 9, "every route has its own name and its description");
    }
}

// ---------------------------------------------------------------- the scissor
static double coveredRows(const Frame& f, UINT y0, UINT y1) {
    double n = 0;
    for (UINT y = y0; y < y1 && y < f.h; ++y)
        for (UINT x = 0; x < f.w; ++x) n += f.at(x, y)[3] > 0.5f ? 1 : 0;
    return n;
}
static bool sameRect(const D3D11_RECT& a, const D3D11_RECT& b) { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }
static UINT rectsNow(D3D11_RECT out[st::Scissor::kMax]) {
    UINT n = 0;
    g_ctx->RSGetScissorRects(&n, nullptr);
    UINT take = n > st::Scissor::kMax ? st::Scissor::kMax : n;
    if (take) g_ctx->RSGetScissorRects(&take, out);
    return n;
}
static void scissorCases(ID3D11GeometryShader* gs, Fails& f) {
    const Cam cam;
    {   // (S1) the game scissors the moved quad to the top half of the target: the stale rectangle crops it; opened, the whole quad draws
        const st::TurretParams p = st::pushParamsFor(frameOfCam(cam), kPushBackM);   // anchor at the eye: the push keeps the quad's size on the screen
        Pt c[4];
        boxAt(c, 0.0f, 0.0f, 0.4f, 0.30f, 0.30f);
        std::vector<Vtx> v;
        addQuadPts(v, c);
        DrawOpts game;
        game.scissored = true;
        game.scissorRect = D3D11_RECT{0, 0, 256, 128};
        DrawOpts open = game;
        open.openScissor = true;
        const Frame plain = render(v, cam, nullptr, st::TurretParams{}, 256, 256, game);   // the control: the scissor really crops a plain draw
        const Frame kept = render(v, cam, gs, p, 256, 256, game);                          // fix.srv_turret_hud_scissor = game
        const Frame opened = render(v, cam, gs, p, 256, 256, open);                        // ... = open
        f.expect(coveredRows(plain, 0, 128) > 1000 && coveredRows(plain, 128, 256) == 0, "(S1) control: the game's scissor rectangle (the top half) crops a plain draw to the top half");
        f.expect(coveredRows(kept, 0, 128) > 1000 && coveredRows(kept, 128, 256) == 0, "(S1) with the game's rectangle kept, the moved quad is cropped to the top half too");
        f.expect(coveredRows(opened, 0, 128) > 1000 && coveredRows(opened, 128, 256) > 1000, "(S1) with the scissor opened, the moved quad covers pixels below the game's rectangle as well");
        f.expect(diffCount(kept, opened, 0.02) == size_t(coveredRows(opened, 128, 256)), "(S1) ...and only below it: the top half is the same picture");
        D3D11_RECT now[st::Scissor::kMax];
        ComPtr<ID3D11RasterizerState> rs;
        g_ctx->RSGetState(&rs);
        f.expect(rectsNow(now) == 1 && sameRect(now[0], game.scissorRect) && rs.Get() == g_rsScissor.Get(), "(S1) after the issue the game's one rectangle is back and the rasterizer state object is the same");
    }
    g_ctx->ClearState();
    Target t(64, 64);
    const auto sceneOf = [&](bool scissorState) {
        g_ctx->ClearState();
        g_ctx->OMSetRenderTargets(1, t.rtv.GetAddressOf(), nullptr);
        const D3D11_VIEWPORT vp{0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->RSSetState(scissorState ? g_rsScissor.Get() : nullptr);
    };
    const ULONG rsRefs = refsOf(g_rsScissor.Get());
    {   // (S2) several rectangles come back exactly
        sceneOf(true);
        const D3D11_RECT game[3] = {{1, 2, 30, 40}, {5, 6, 7, 8}, {10, 0, 64, 20}};
        g_ctx->RSSetScissorRects(3, game);
        st::Scissor sc;
        st::Scissor::Seen seen;
        f.expect(sc.open(g_ctx.Get(), seen) == st::Scissor::Open::kOpened && sc.owed() && seen.enabled && seen.rects == 3 && sameRect(seen.first, game[0]) && seen.targetW == 64 && seen.targetH == 64,
                 "(S2) three game rectangles: opened, and what was found is said (three, the first, the target's 64x64)");
        D3D11_RECT now[st::Scissor::kMax];
        bool whole = rectsNow(now) == 3;
        for (int i = 0; i < 3; ++i) whole = whole && sameRect(now[i], D3D11_RECT{0, 0, 64, 64});
        f.expect(whole, "(S2) ...all three are the whole target while open");
        f.expect(sc.restore(g_ctx.Get()) && !sc.owed(), "(S2) restore succeeds");
        bool back = rectsNow(now) == 3;
        for (int i = 0; i < 3; ++i) back = back && sameRect(now[i], game[i]);
        f.expect(back, "(S2) ...and the game's three rectangles are back exactly");
        f.expect(sc.restore(g_ctx.Get()), "(S2) restore with nothing owed is a no-op");
    }
    {   // (S3) the most rectangles the pipeline holds, each different
        sceneOf(true);
        D3D11_RECT game[st::Scissor::kMax];
        for (UINT i = 0; i < st::Scissor::kMax; ++i) game[i] = D3D11_RECT{LONG(i), LONG(i + 1), LONG(20 + i), LONG(30 + i)};
        g_ctx->RSSetScissorRects(st::Scissor::kMax, game);
        st::Scissor sc;
        st::Scissor::Seen seen;
        f.expect(sc.open(g_ctx.Get(), seen) == st::Scissor::Open::kOpened && seen.rects == st::Scissor::kMax, "(S3) sixteen game rectangles: opened");
        D3D11_RECT now[st::Scissor::kMax];
        bool whole = rectsNow(now) == st::Scissor::kMax;
        for (UINT i = 0; i < st::Scissor::kMax; ++i) whole = whole && sameRect(now[i], D3D11_RECT{0, 0, 64, 64});
        f.expect(whole && sc.restore(g_ctx.Get()), "(S3) ...all sixteen the whole target while open, restored");
        bool back = rectsNow(now) == st::Scissor::kMax;
        for (UINT i = 0; i < st::Scissor::kMax; ++i) back = back && sameRect(now[i], game[i]);
        f.expect(back, "(S3) ...and the game's sixteen back exactly");
    }
    {   // (S4) the test on and no rectangle set: one whole-target rectangle while open, none after
        sceneOf(true);
        g_ctx->RSSetScissorRects(0, nullptr);
        st::Scissor sc;
        st::Scissor::Seen seen;
        D3D11_RECT now[st::Scissor::kMax];
        f.expect(sc.open(g_ctx.Get(), seen) == st::Scissor::Open::kOpened && seen.rects == 0, "(S4) the scissor test on with no rectangle: opened");
        f.expect(rectsNow(now) == 1 && sameRect(now[0], D3D11_RECT{0, 0, 64, 64}) && sc.restore(g_ctx.Get()) && rectsNow(now) == 0, "(S4) ...one whole-target rectangle while open and none after");
    }
    {   // (S5) the scissor test off: nothing is touched
        sceneOf(false);
        const D3D11_RECT game[2] = {{3, 3, 9, 9}, {4, 4, 8, 8}};
        g_ctx->RSSetScissorRects(2, game);
        st::Scissor sc;
        st::Scissor::Seen seen;
        D3D11_RECT now[st::Scissor::kMax];
        f.expect(sc.open(g_ctx.Get(), seen) == st::Scissor::Open::kOff && !sc.owed() && !seen.enabled, "(S5) the scissor test off (no rasterizer state): left alone");
        f.expect(rectsNow(now) == 2 && sameRect(now[0], game[0]) && sameRect(now[1], game[1]), "(S5) ...with the game's rectangles as they were");
        f.expect(sc.restore(g_ctx.Get()), "(S5) ...and nothing to give back");
    }
    {   // (S6) looking only: found and said, not changed
        sceneOf(true);
        const D3D11_RECT game[1] = {{2, 2, 20, 20}};
        g_ctx->RSSetScissorRects(1, game);
        st::Scissor sc;
        st::Scissor::Seen seen;
        D3D11_RECT now[st::Scissor::kMax];
        f.expect(sc.open(g_ctx.Get(), seen, false) == st::Scissor::Open::kOff && !sc.owed() && seen.enabled && seen.rects == 1 && sameRect(seen.first, game[0]) && seen.targetW == 64 && rectsNow(now) == 1 &&
                     sameRect(now[0], game[0]),
                 "(S6) with the key at game the draw only looks: the test is found on, the rectangle kept");
    }
    {   // (S7) no target view bound: the first viewport's extent
        g_ctx->ClearState();
        const D3D11_VIEWPORT vp{0.0f, 0.0f, 100.0f, 50.0f, 0.0f, 1.0f};
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->RSSetState(g_rsScissor.Get());
        const D3D11_RECT game[1] = {{5, 5, 10, 10}};
        g_ctx->RSSetScissorRects(1, game);
        st::Scissor sc;
        st::Scissor::Seen seen;
        D3D11_RECT now[st::Scissor::kMax];
        f.expect(sc.open(g_ctx.Get(), seen) == st::Scissor::Open::kOpened && rectsNow(now) == 1 && sameRect(now[0], D3D11_RECT{0, 0, 100, 50}) && seen.targetW == 100 && seen.targetH == 50,
                 "(S7) without a target view the rectangle is the viewport's extent (100x50)");
        f.expect(sc.restore(g_ctx.Get()) && rectsNow(now) == 1 && sameRect(now[0], game[0]), "(S7) ...and the game's is back");
        // and with neither a target nor a viewport there is nothing to open to
        g_ctx->ClearState();
        g_ctx->RSSetState(g_rsScissor.Get());
        g_ctx->RSSetScissorRects(1, game);
        st::Scissor none;
        st::Scissor::Seen seen2;
        f.expect(none.open(g_ctx.Get(), seen2) == st::Scissor::Open::kFailed && !none.owed() && rectsNow(now) == 1 && sameRect(now[0], game[0]), "(S7) with no extent at all: failed, the game's rectangle untouched");
    }
    {   // (S8) a second open while one is owed is refused and leaves the first's saved rectangles; the boundary settle leaves the game's own newer rectangles, and restores ours
        sceneOf(true);
        const D3D11_RECT game[1] = {{2, 3, 40, 41}};
        g_ctx->RSSetScissorRects(1, game);
        st::Scissor sc;
        st::Scissor::Seen seen;
        D3D11_RECT now[st::Scissor::kMax];
        (void)sc.open(g_ctx.Get(), seen);
        st::Scissor::Seen again;
        f.expect(sc.open(g_ctx.Get(), again) == st::Scissor::Open::kFailed && sc.owed(), "(S8) a second open while one is owed: refused");
        f.expect(sc.settle(g_ctx.Get()) && !sc.owed() && rectsNow(now) == 1 && sameRect(now[0], game[0]), "(S8) the boundary settle puts the game's rectangle back while ours are still bound");
        (void)sc.open(g_ctx.Get(), seen);
        const D3D11_RECT theirs[1] = {{7, 7, 9, 9}};
        g_ctx->RSSetScissorRects(1, theirs);
        f.expect(sc.settle(g_ctx.Get()) && !sc.owed() && rectsNow(now) == 1 && sameRect(now[0], theirs[0]), "(S8) ...and leaves the game's own newer rectangle when it set one meanwhile");
    }
    f.expect(refsOf(g_rsScissor.Get()) == rsRefs, "(S) no reference to the rasterizer state kept");
    g_ctx->ClearState();
}

// ---------------------------------------------------------------- signature H's second constant buffer and the register copy
static bool slotIs(UINT slot, ID3D11Buffer* want) {
    ID3D11Buffer* b = nullptr;
    g_ctx->GSGetConstantBuffers(slot, 1, &b);
    const bool ok = b == want;
    if (b) b->Release();
    return ok;
}
static void sceneBindingCases(ID3D11GeometryShader* gs, Fails& f) {
    const std::vector<float> data{7, 8, 9, 10};
    auto sentinel0 = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, data.data());
    auto sentinel1 = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, data.data());
    auto constants = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, data.data());
    resetScene();
    auto scene = sceneBuffer();
    const auto gameState = [&](ID3D11Buffer* vsSlot1, UINT first, UINT count) {
        g_ctx->ClearState();
        g_ctx->GSSetConstantBuffers(st::kGsSlot, 1, sentinel0.GetAddressOf());
        g_ctx->GSSetConstantBuffers(st::kGsGameSlot, 1, sentinel1.GetAddressOf());
        ComPtr<ID3D11DeviceContext1> c1;
        if (first && SUCCEEDED(g_ctx.As(&c1)))
            c1->VSSetConstantBuffers1(st::kGameCbSlot, 1, &vsSlot1, &first, &count);
        else
            g_ctx->VSSetConstantBuffers(st::kGameCbSlot, 1, &vsSlot1);
    };
    {   // the ordinary draw: both slots in, both out again
        gameState(scene.Get(), 0, 0);
        const ULONG sceneRefs = refsOf(scene.Get()), s1Refs = refsOf(sentinel1.Get()), s0Refs = refsOf(sentinel0.Get()), constRefs = refsOf(constants.Get()), gsRefs = refsOf(gs);
        {
            st::ExtraCb ex;
            f.expect(st::readGameCb(g_ctx.Get(), st::kGameCbSlot, ex) && ex.buffer == scene.Get(), "the draw's vertex constants at slot 1 are read");
            st::Binding b;
            f.expect(b.begin(g_ctx.Get(), gs, constants.Get(), st::directSetGs, &ex) == st::Binding::Begin::kBound, "begin with the vertex constants binds the shader, the turret constants and the vertex constants");
            f.expect(slotIs(st::kGsSlot, constants.Get()) && slotIs(st::kGsGameSlot, scene.Get()) && b.needsRestore(), "GS slot 0 holds the turret constants and GS slot 1 the draw's vertex constants");
            f.expect(b.restore(g_ctx.Get()) && !b.needsRestore(), "restore puts the game's back");
            f.expect(slotIs(st::kGsSlot, sentinel0.Get()) && slotIs(st::kGsGameSlot, sentinel1.Get()), "...GS slots 0 and 1 are the game's again");
        }
        f.expect(refsOf(scene.Get()) == sceneRefs && refsOf(sentinel1.Get()) == s1Refs && refsOf(sentinel0.Get()) == s0Refs && refsOf(constants.Get()) == constRefs && refsOf(gs) == gsRefs,
                 "no reference kept to the vertex constants, the game's constants, the turret constants or the shader");
    }
    {   // no extra: GS slot 1 is the game's throughout
        gameState(scene.Get(), 0, 0);
        st::Binding b;
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get()) == st::Binding::Begin::kBound && slotIs(st::kGsGameSlot, sentinel1.Get()), "without the vertex constants GS slot 1 is left alone");
        f.expect(b.restore(g_ctx.Get()) && slotIs(st::kGsGameSlot, sentinel1.Get()), "...and stays the game's");
    }
    {   // an extra with no buffer is refused, nothing moved
        gameState(scene.Get(), 0, 0);
        st::Binding b;
        st::ExtraCb none;
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get(), st::directSetGs, &none) == st::Binding::Begin::kRefused && !b.needsRestore() && slotIs(st::kGsSlot, sentinel0.Get()) && slotIs(st::kGsGameSlot, sentinel1.Get()),
                 "an extra without a buffer: refused, the game's state is where it was");
    }
    {   // nothing bound at the vertex stage's slot 1
        g_ctx->ClearState();
        st::ExtraCb ex;
        f.expect(!st::readGameCb(g_ctx.Get(), st::kGameCbSlot, ex) && !ex.buffer, "no vertex constants at slot 1: nothing read");
        f.expect(!st::readGameCb(nullptr, st::kGameCbSlot, ex), "no context: nothing read");
    }
    {   // a ranged binding: the game bound the buffer from constant 16, and the shader gets the same range
        g_sceneFirst = 16;
        auto ranged = sceneBuffer();
        gameState(ranged.Get(), 16, kSceneRangeConstants);
        st::ExtraCb ex;
        f.expect(st::readGameCb(g_ctx.Get(), st::kGameCbSlot, ex) && ex.buffer == ranged.Get() && ex.ranged && ex.first == 16 && ex.count == kSceneRangeConstants, "a ranged binding is read with its first constant and count");
        st::Binding b;
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get(), st::directSetGs, &ex) == st::Binding::Begin::kBound, "bound with the range");
        ComPtr<ID3D11DeviceContext1> c1;
        ID3D11Buffer* got = nullptr;
        UINT first = 0, count = 0;
        if (SUCCEEDED(g_ctx.As(&c1))) c1->GSGetConstantBuffers1(st::kGsGameSlot, 1, &got, &first, &count);
        f.expect(got == ranged.Get() && first == 16 && count == kSceneRangeConstants, "GS slot 1 holds the buffer with the game's first constant and count");
        if (got) got->Release();
        f.expect(b.restore(g_ctx.Get()) && slotIs(st::kGsGameSlot, sentinel1.Get()), "restored: GS slot 1 is the game's");
        g_sceneFirst = 0;
    }
    {   // a restore that fails: the boundary settles both slots
        gameState(scene.Get(), 0, 0);
        st::ExtraCb ex;
        (void)st::readGameCb(g_ctx.Get(), st::kGameCbSlot, ex);
        st::Binding b;
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get(), st::directSetGs, &ex) == st::Binding::Begin::kBound, "settle scene: bound");
        const auto result = b.finish(g_ctx.Get(), failingSetGs);
        f.expect(result.retried && !result.restored && b.needsRestore(), "a restore that failed twice is still owed");
        f.expect(b.settle(g_ctx.Get()) && !b.needsRestore() && slotIs(st::kGsSlot, sentinel0.Get()) && slotIs(st::kGsGameSlot, sentinel1.Get()), "settled: the game's slots 0 and 1 are back");
        // the game rebound slot 1 while it was owed: its newer state stays
        gameState(scene.Get(), 0, 0);
        f.expect(b.settle(g_ctx.Get()), "a settled binding settles idempotently");
        st::Binding c;
        (void)c.begin(g_ctx.Get(), gs, constants.Get(), st::directSetGs, &ex);
        c.finish(g_ctx.Get(), failingSetGs);
        auto theirs = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, data.data());
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->GSSetConstantBuffers(st::kGsGameSlot, 1, theirs.GetAddressOf());
        f.expect(c.settle(g_ctx.Get()) && slotIs(st::kGsGameSlot, theirs.Get()), "settle with GS slot 1 rebound by the game leaves the game's own newer state");
    }
    g_ctx->ClearState();
}

// The disc probe's register copy (st::copyPsRegister), on WARP: the offsets, the first constant of a ranged binding, and the refusals.
static void copyRegisterCases() {
    const auto filled = [&](UINT registers, UINT row, const float v[4]) {
        std::vector<float> data(size_t(registers) * 4, -1.0f);
        std::memcpy(data.data() + size_t(row) * 4, v, 16);
        return makeBuffer(UINT(data.size() * sizeof(float)), D3D11_BIND_CONSTANT_BUFFER, data.data());
    };
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = 32;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> stage;
    ck(g_dev->CreateBuffer(&sd, nullptr, &stage), "staging buffer");
    const auto readStage = [&](float out[8]) {
        D3D11_MAPPED_SUBRESOURCE m{};
        ck(g_ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m), "Map the register copy");
        std::memcpy(out, m.pData, 32);
        g_ctx->Unmap(stage.Get(), 0);
    };
    const float alpha[4] = {1.0f, 2.0f, 3.0f, 0.75f}, colour[4] = {5.0f, 0.5f, 7.0f, 8.0f}, ranged[4] = {9.0f, 8.0f, 7.0f, 6.0f};
    auto cb2 = filled(16, 11, alpha), cb1 = filled(100, 90, colour);
    g_ctx->ClearState();
    g_ctx->PSSetConstantBuffers(2, 1, cb2.GetAddressOf());
    g_ctx->PSSetConstantBuffers(1, 1, cb1.GetAddressOf());
    check(st::copyPsRegister(g_ctx.Get(), stage.Get(), 2, 11, 0) && st::copyPsRegister(g_ctx.Get(), stage.Get(), 1, 90, 16), "the disc's two registers are copied");
    float got[8];
    readStage(got);
    check(got[0] == 1.0f && got[1] == 2.0f && got[2] == 3.0f && got[3] == 0.75f && got[4] == 5.0f && got[5] == 0.5f && got[6] == 7.0f && got[7] == 8.0f,
          "...cb2[11] at byte 0 and cb1[90] at byte 16, exactly (alpha is .w of the first, the intensity .y of the second)");
    float before[8];
    readStage(before);
    check(!st::copyPsRegister(g_ctx.Get(), stage.Get(), 2, 16, 0) && !st::copyPsRegister(g_ctx.Get(), stage.Get(), 1, 100, 16) && !st::copyPsRegister(g_ctx.Get(), stage.Get(), 6, 0, 0) &&
              !st::copyPsRegister(g_ctx.Get(), nullptr, 2, 11, 0) && !st::copyPsRegister(nullptr, stage.Get(), 2, 11, 0),
          "a register past the buffer, nothing bound at the slot, no staging buffer, no context: refused");
    float after[8];
    readStage(after);
    check(std::memcmp(before, after, sizeof(before)) == 0, "...and the staging buffer is untouched");
    {   // a ranged binding: the register is counted from the first constant the game bound the buffer with
        const float decoy[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        std::vector<float> data(size_t(48) * 4, -1.0f);
        std::memcpy(data.data() + size_t(11) * 4, decoy, 16);     // row 11 of the whole buffer: not the one
        std::memcpy(data.data() + size_t(16 + 11) * 4, ranged, 16);   // row 11 counted from constant 16: the one
        auto big = makeBuffer(UINT(data.size() * sizeof(float)), D3D11_BIND_CONSTANT_BUFFER, data.data());
        ComPtr<ID3D11DeviceContext1> c1;
        check(SUCCEEDED(g_ctx.As(&c1)), "the 11.1 context");
        const UINT first = 16, count = 16;
        ID3D11Buffer* b = big.Get();
        c1->PSSetConstantBuffers1(2, 1, &b, &first, &count);
        check(st::copyPsRegister(g_ctx.Get(), stage.Get(), 2, 11, 0), "a ranged binding: copied");
        readStage(got);
        check(got[0] == 9.0f && got[1] == 8.0f && got[2] == 7.0f && got[3] == 6.0f, "...from the register counted from the first constant (row 11 + 16), not row 11 of the whole buffer");
        readStage(before);
        check(!st::copyPsRegister(g_ctx.Get(), stage.Get(), 2, 16, 0), "a row past the bound range (though inside the buffer): refused");
        readStage(after);
        check(std::memcmp(before, after, sizeof(before)) == 0, "...and the staging buffer is untouched");
    }
    g_ctx->ClearState();
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
    bad = p0;
    bad.depth = 3.0f;
    check(c.get(g_ctx.Get(), bad) == nullptr, "a depth mode that is not 0, 1 or 2: refused");
    check(c.get(g_ctx.Get(), p1) == first, "...and the buffer is still the good one");
    check(c.get(g_ctx.Get(), st::withDepth(p1, st::DepthMode::kReal)) == first && c.written().depth == 1.0f && readBack(first).depth == 1.0f &&
              c.get(g_ctx.Get(), st::withDepth(p1, st::DepthMode::kFront)) == first && readBack(first).depth == 2.0f && c.get(g_ctx.Get(), p1) == first && readBack(first).depth == 0.0f,
          "the depth mode is in the buffer: real, front and game each rewrite it (the register's fourth number)");
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

// ---------------------------------------------------------------- THE LOCK (srv_turret_math.h; the shader's turretLockCentre and turretLock), on WARP
// With the lock flag the twin moves every vertex of a triangle by T - C, C the midpoint of the triangle's longest edge: a quad's centre lands on T, its two triangles agree (the shared
// corners bit for bit), every vertex keeps the game's NDC depth, and signature H's lighting position moves by the same vector. A rectangle (a billboard) and a parallelogram sheared along x
// and tilted in depth, cut along its long diagonal.
static void lockCases(int fam, ID3D11GeometryShader* twin, Fails& f) {
    const Cam cam;
    const float target[3] = {-0.012f, 0.017f, 0.31f};
    st::TurretParams p = st::translateParamsFor(frameOfCam(cam, nullptr, target, target), st::Group::kRadar);
    p.lock = 1.0f;
    f.expect(st::paramsUsable(p), "lock: the constants with the lock flag are usable");
    st::TurretParams half = p;
    half.lock = 0.5f;
    f.expect(!st::paramsUsable(half), "lock: a lock flag that is neither 0 nor 1 is refused");
    Pt rect[4];
    boxAt(rect, 0.03f, -0.02f, 0.33f, 0.06f, 0.04f);
    const float s = 0.025f;
    Pt shear[4] = {Pt{0.03f - 0.06f + s, -0.02f - 0.04f, 0.30f}, Pt{0.03f + 0.06f + s, -0.02f - 0.04f, 0.34f}, Pt{0.03f - 0.06f - s, -0.02f + 0.04f, 0.31f}, Pt{}};
    shear[3] = Pt{shear[1].x + shear[2].x - shear[0].x, shear[1].y + shear[2].y - shear[0].y, shear[1].d + shear[2].d - shear[0].d};
    const float right[3] = {0.8f, 0.0f, 0.6f}, up[3] = {-0.36f, 0.8f, 0.48f}, fwd[3] = {-0.48f, -0.6f, 0.64f};
    const bool scene = fam == int(st::kSceneSignature);
    if (scene) setScene(right, up, fwd, 0);
    for (int which = 0; which < 2; ++which) {
        const Pt* c = which == 0 ? rect : shear;
        const char* name = which == 0 ? "a rectangle" : "a sheared, tilted parallelogram";
        const std::vector<Pt> pts = {c[0], c[1], c[2], c[1], c[3], c[2]};   // addQuadPts' order: the two triangles share c[1]-c[2]
        const double centre[3] = {(double(c[1].x) + c[2].x) * 0.5, (double(c[1].y) + c[2].y) * 0.5, (double(c[1].d) + c[2].d) * 0.5};
        const Captured m = captureMove(fam, twin, pts, cam, p);
        double post[6][3], pre[6][3];
        for (int i = 0; i < 6; ++i) {
            eyePos(m.post[i], cam, post[i]);
            eyePos(m.pre[i], cam, pre[i]);
        }
        double ca[3], cb[3], preC[3];
        st::lockCentre<double>(post[0], post[1], post[2], ca);
        st::lockCentre<double>(post[3], post[4], post[5], cb);
        st::lockCentre<double>(pre[0], pre[1], pre[2], preC);
        char what[200];
        bool onT = true, agree = true, rigid = true, depth = true;
        for (int k = 0; k < 3; ++k) {
            onT = onT && std::fabs(ca[k] - target[k]) < 1e-5 && std::fabs(cb[k] - target[k]) < 1e-5;
            agree = agree && std::fabs(ca[k] - cb[k]) < 1e-5;
        }
        for (int i = 0; i < 6; ++i) {
            for (int k = 0; k < 3; ++k) rigid = rigid && std::fabs((post[i][k] - pre[i][k]) - (target[k] - centre[k])) < 1e-5;
            depth = depth && closeTo(double(m.post[i][2]) / m.post[i][3], double(m.pre[i][2]) / m.pre[i][3], 1e-5);
        }
        std::snprintf(what, sizeof(what), "lock, %s: each triangle's longest-edge midpoint lands on the target to 1e-5 m", name);
        f.expect(onT, what);
        std::snprintf(what, sizeof(what), "lock, %s: the two triangles agree to 1e-5 m, and the shared corners land on the same clip position bit for bit", name);
        f.expect(agree && std::memcmp(m.post[1].data(), m.post[3].data(), 16) == 0 && std::memcmp(m.post[2].data(), m.post[5].data(), 16) == 0, what);
        std::snprintf(what, sizeof(what), "lock, %s: every vertex moves by target - centre (a rigid translation), keeping the game's NDC depth", name);
        f.expect(rigid && depth, what);
        std::snprintf(what, sizeof(what), "lock, %s: the CPU rule (st::lockCentre) finds the quad's centre on the game's corners", name);
        f.expect(std::fabs(preC[0] - centre[0]) < 1e-5 && std::fabs(preC[1] - centre[1]) < 1e-5 && std::fabs(preC[2] - centre[2]) < 1e-5, what);
        if (scene) {
            bool lit = true;
            for (size_t i = 0; i < pts.size(); ++i) {
                const double l0[3] = {tagU(i), tagV(i), 0.0}, k3[3] = {target[0] - centre[0], target[1] - centre[1], target[2] - centre[2]};
                for (int k = 0; k < 3; ++k) lit = lit && closeTo(m.lp[i][k], l0[k] + right[k] * k3[0] + up[k] * k3[1] + fwd[k] * k3[2], 1e-4);
            }
            std::snprintf(what, sizeof(what), "lock, %s: the Scorpion hologram's lighting position moves by the same vector, in the camera's right, up and forward", name);
            f.expect(lit, what);
        }
    }
    if (scene) resetScene();
    // the control: a triangle's own mean is not shared by the sheared quad's two triangles (the longest-edge rule is what makes them agree)
    const double t1 = (double(shear[0].x) + shear[1].x + shear[2].x) / 3.0, t2 = (double(shear[1].x) + shear[3].x + shear[2].x) / 3.0;
    f.expect(std::fabs(t1 - t2) > 1e-3, "lock control: the sheared quad's two triangles have different means (a crack under a mean-centre rule)");
    // without the flag the same constants are the plain translation by T (the delta path is unchanged)
    st::TurretParams plain = p;
    plain.lock = 0.0f;
    const std::vector<Pt> quad = {rect[0], rect[1], rect[2], rect[1], rect[3], rect[2]};
    const Captured m0 = captureMove(fam, twin, quad, cam, plain);
    expectReference(f, "lock: with the flag at 0 the twin is the plain translation, every vertex the CPU reference's", plain, m0, 6);
}

// THE LOCK on the CPU (srv_turret_math.h): the draw's shape, the seated-to-eye conversion, the vehicle's turn, rest, the target's learning and re-learning, and the reset rules.
static void lockCpuCases() {
    check(st::lockShape(4, 6, 1) && st::lockShape(5, 4, 1) && st::lockShape(4, 6, 0), "lock shape: a list of 6 or a strip of 4, one instance, is a single quad");
    check(!st::lockShape(4, 12, 1) && !st::lockShape(5, 6, 1) && !st::lockShape(4, 6, 2) && !st::lockShape(1, 6, 1) && !st::lockShape(4, 3, 1), "lock shape: more triangles, instances or another topology are not");
    const float head[12] = {0.8f, 0.0f, 0.6f, 0.1f, 0.0f, 1.0f, 0.0f, 1.2f, -0.6f, 0.0f, 0.8f, -0.3f}, eyeToHead[12] = {1, 0, 0, -0.032f, 0, 1, 0, 0, 0, 0, 1, 0.01f};
    double R[9], E[3];
    check(st::eyePoseOf(head, eyeToHead, R, E), "lock: an eye pose");
    const double eyePt[3] = {0.03, -0.21, 0.33};
    double seated[3], back[3];
    st::eyeToSeated(R, E, eyePt, seated);
    st::seatedToEye(R, E, seated, back);
    check(std::fabs(back[0] - eyePt[0]) < 1e-6 && std::fabs(back[1] - eyePt[1]) < 1e-6 && std::fabs(back[2] - eyePt[2]) < 1e-6,
          "lock: seatedToEye is eyeToSeated's inverse (to the float rows' orthonormality)");
    const float A[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const double a = 2.0 * 3.14159265358979323846 / 180.0;
    const float B[16] = {float(std::cos(a)), 0, float(std::sin(a)), 0, 0, 1, 0, 0, float(-std::sin(a)), 0, float(std::cos(a)), 0, 0, 0, 0, 1};
    float B2[16];
    for (int i = 0; i < 16; ++i) B2[i] = B[i] * 1.5f;   // a scale on the rows does not count as a turn
    check(std::fabs(st::rotationAngleDeg(A, A)) < 1e-3 && std::fabs(st::rotationAngleDeg(A, B) - 2.0) < 1e-3 && std::fabs(st::rotationAngleDeg(A, B2) - 2.0) < 1e-3,
          "lock: the vehicle's turn between two rotations, in degrees, whatever the rows' scale");
    // rest: every rate under 3 deg/s for 20 frames in a row
    st::RestWatch r;
    for (uint32_t i = 0; i + 1 < st::kRestFrames; ++i) check(!r.step(true, 1.0, -2.0, 2.9), "rest: not before 20 still frames");
    check(r.step(true, 1.0, -2.0, 2.9), "rest: at the 20th still frame");
    check(!r.step(true, 3.5, 0.0, 0.0) && r.frames == 0, "rest: a turret yaw rate of 3 deg/s or more breaks it");
    for (uint32_t i = 0; i < st::kRestFrames; ++i) r.step(true, 0.0, 0.0, 0.0);
    check(r.atRest() && !r.step(true, 0.0, -3.0, 0.0), "rest: ... and so does the pitch");
    for (uint32_t i = 0; i < st::kRestFrames; ++i) r.step(true, 0.0, 0.0, 0.0);
    check(r.atRest() && !r.step(true, 0.0, 0.0, 4.0), "rest: ... and the vehicle's own turn");
    for (uint32_t i = 0; i < st::kRestFrames; ++i) r.step(true, 0.0, 0.0, 0.0);
    check(r.atRest() && !r.step(false, 0.0, 0.0, 0.0) && r.frames == 0, "rest: ... and a frame without the rates");
    // learning: the mean of the first six rest samples; then each moves it by 1 - exp(-dt / 1 s)
    st::LockTarget t;
    const double s1[3] = {0.010, -0.220, -0.050}, s2[3] = {0.004, -0.220, -0.060};
    bool learnedAt = false;
    for (uint32_t i = 0; i < st::kLockLearnSamples; ++i) {
        const bool first = t.add(i % 2 ? s2 : s1, 0.1 * i);
        check(first == (i + 1 == st::kLockLearnSamples), "learning: learned exactly at the sixth rest sample");
        learnedAt = learnedAt || first;
    }
    check(learnedAt && t.learned && t.n == st::kLockLearnSamples && std::fabs(t.seated[0] - 0.007) < 1e-12 && std::fabs(t.seated[2] + 0.055) < 1e-12, "learning: the target is the samples' mean");
    const double moved[3] = {0.017, -0.220, -0.055};
    check(!t.add(moved, 0.5 + 0.5) && t.relearned == 1, "re-learning: a later rest sample is a re-learn, not a first learning");
    const double want = 0.007 + (1.0 - std::exp(-0.5)) * 0.010;
    check(std::fabs(t.seated[0] - want) < 1e-12 && std::fabs(t.seated[2] + 0.055) < 1e-12, "re-learning: an exponential average with a 1 s time constant (dt 0.5 s)");
    const double before = t.seated[0];
    t.add(moved, 100.0);
    check(std::fabs(t.seated[0] - (before + (1.0 - std::exp(-2.0)) * (0.017 - before))) < 1e-12, "re-learning: a long gap counts as 2 s at most");
    const double bad[3] = {NAN, 0.0, 0.0};
    const double kept = t.seated[0];
    check(!t.add(bad, 101.0) && t.seated[0] == kept, "learning: a sample that is not finite is not taken");
    // the resets: the view coming on, and camera back, radar forward or the anchor key changing; nothing else
    check(st::lockResetWhy(false, true, 35, 35, 30, 30, 0, 0) == st::LockReset::kViewOn, "reset: the view comes on");
    check(st::lockResetWhy(true, true, 35, 30, 30, 30, 0, 0) == st::LockReset::kKey && st::lockResetWhy(true, true, 35, 35, 30, 35, 0, 0) == st::LockReset::kKey &&
              st::lockResetWhy(true, true, 35, 35, 30, 30, 0, 1) == st::LockReset::kKey,
          "reset: camera back, radar forward or the anchor key changes");
    check(st::lockResetWhy(true, true, 35, 35, 30, 30, 0, 0) == st::LockReset::kNone && st::lockResetWhy(true, false, 35, 35, 30, 30, 0, 0) == st::LockReset::kNone &&
              st::lockResetWhy(false, false, 35, 35, 30, 30, 0, 0) == st::LockReset::kNone,
          "reset: none while the view stays on or off with the keys unchanged (the view going off is not a reset; the next view-on is)");
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
        depthMathCases();
        radarAnchorCases();
        contentDepthCases();
        slotCameraCases();
        planeAndUnmovedCases();
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
        lockCpuCases();
        Fails production;
        for (uint32_t fam = 0; fam < st::kSignatureCount; ++fam) {
            g_fam = int(fam);
            linkageCases(int(fam), gsOf[fam].Get());
            instrumentCases(int(fam), gsOf[fam].Get());
            moveCases(int(fam), twinOf[fam].Get(), production);
            moveImages(twinOf[fam].Get(), production);
            stripCases(twinOf[fam].Get(), production);
            lockCases(int(fam), twinOf[fam].Get(), production);
        }
        g_fam = 0;
        bindingCases(gsOf[0].Get(), production, true);
        sceneBindingCases(gsOf[st::kSceneSignature].Get(), production);
        scissorCases(gsOf[0].Get(), production);
        copyRegisterCases();
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
            {"the translation dropped", "const float3 p1 = p + tv;", "const float3 p1 = p;"},
            {"the translation's sign", "const float3 p1 = p + tv;", "const float3 p1 = p - tv;"},
            {"the content depth ignored", "tr.w > 0.0 ?", "tr.w > 1000.0 ?"},
            {"the content depth's scale without the vertex's depth", "tr.xyz * (w * tr.w)", "tr.xyz * tr.w"},
            {"the content depth's scale taken at tr.w 0 (every other draw)", "tr.w > 0.0 ?", "tr.w >= 0.0 ?"},
            {"the move about the origin, not the anchor", "q = an.xyz + v * ((d - mv.x) / d);", "q = v * ((d - mv.x) / d);"},
            {"w kept, not the moved depth", "ndcZ * q.z, q.z);", "ndcZ * q.z, w);"},
            {"the NDC z not kept", "ndcZ * q.z", "c.z"},
            {"the unmoved vertex returned when the move ends behind the eye", "if (win) q = an.xyz + v * ((d - mv.x) / d);", "if (win) q = an.xyz + v * ((d - mv.x) / d); if (!(q.z > 0.01)) return c;"},
            {"the depth fallback replaced by z / w", "? zn : 0.999", "? zn : zn"},
            {"the depth fallback at the far end", ": 0.999;", ": 0.0;"},
            {"no fallback for a vertex inside the near plane", "zn >= 0.0 && zn <= 1.0", "zn >= 0.0"},
            {"a `pre` that is the moved position", "o.pre = v[i].pos;", "float3 dd; o.pre = turretMove(v[i].pos, dd);"},
            {"the real depth mode ignored", "else if (mv.w > 0.5)", "else if (mv.w > 5.0)"},
            {"the front depth mode ignored", "if (mv.w > 1.5) ndcZ = 0.999;", "if (mv.w > 5.0) ndcZ = 0.999;"},
            {"the front depth at the far end", "if (mv.w > 1.5) ndcZ = 0.999;", "if (mv.w > 1.5) ndcZ = 0.0;"},
            {"the real depth from the unmoved w", "clamp(c.z / q.z, 0.0, 0.999)", "clamp(c.z / w, 0.0, 0.999)"},
            {"the real depth unclamped", "clamp(c.z / q.z, 0.0, 0.999)", "(c.z / q.z)"},
        };
        // The Scorpion hologram's lighting position (signature H only): the same move cases, which include its case (o).
        const Mutant sceneMutants[] = {
            {"the lighting position not moved", "o.v.l += g[277].xyz * d.x + g[278].xyz * d.y + g[279].xyz * d.z;", ""},
            {"the lighting position's sign", "o.v.l += ", "o.v.l -= "},
            {"the right and forward vectors swapped", "g[277].xyz * d.x + g[278].xyz * d.y + g[279].xyz * d.z", "g[279].xyz * d.x + g[278].xyz * d.y + g[277].xyz * d.z"},
            {"the up vector dropped", " + g[278].xyz * d.y", ""},
            {"the lighting position moved by the translation alone", "g[277].xyz * d.x + g[278].xyz * d.y + g[279].xyz * d.z", "g[277].xyz * tr.x + g[278].xyz * tr.y + g[279].xyz * tr.z"},
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
            if (fam == st::kSceneSignature) {
                for (const Mutant& m : sceneMutants) {
                    const std::string mutated = replaceAll(text, m.from, m.to);
                    check(mutated != text, "a mutant differs from the production text");
                    const auto mg = twinFromText(int(fam), mutated);
                    Fails fails;
                    moveCases(int(fam), mg.Get(), fails);
                    char what[200];
                    std::snprintf(what, sizeof(what), "the mutant \"%s\" of signature %u's shader is caught by the move cases", m.name, fam);
                    check(fails.n > 0, what);
                }
            }
            {   // THE LOCK's mutants: each caught by the lock cases (the last, the lighting position's, in signature H only)
                const Mutant lockMutants[] = {
                    {"the lock flag ignored", "const bool locked = an.w > 0.5;", "const bool locked = an.w > 5.0;"},
                    {"the triangle's mean as its centre", "if (pq >= qr && pq >= rp) return (p + q) * 0.5;", "if (pq >= 0.0) return (p + q + r) / 3.0;"},
                    {"the shortest edge's test", "if (pq >= qr && pq >= rp)", "if (pq <= qr && pq <= rp)"},
                    {"the shift's sign", "tr.xyz - turretLockCentre(v[0].pos, v[1].pos, v[2].pos)", "turretLockCentre(v[0].pos, v[1].pos, v[2].pos) - tr.xyz"},
                    {"the game's depth not kept under the lock", "gz * s.z, s.z);", "0.5 * s.z, s.z);"},
                    {"the lock's displacement not given to the lighting line", "dq = k;", "dq = float3(0.0, 0.0, 0.0);"},
                };
                for (const Mutant& m : lockMutants) {
                    if (std::strcmp(m.from, "dq = k;") == 0 && fam != st::kSceneSignature) continue;
                    const std::string mutated = replaceAll(text, m.from, m.to);
                    check(mutated != text, "a lock mutant differs from the production text");
                    const auto mg = twinFromText(int(fam), mutated);
                    Fails fails;
                    lockCases(int(fam), mg.Get(), fails);
                    char what[200];
                    std::snprintf(what, sizeof(what), "the mutant \"%s\" of signature %u's shader is caught by the lock cases", m.name, fam);
                    check(fails.n > 0, what);
                }
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
        std::printf("PASS srv_turret_hud_test: %u checks; the turret shaders, binding, constants, maths, anchor and Scorpion latch on WARP, twenty-five shader mutants, five lock mutants and the winding mutant in each of eight signatures, six more in signature H and one binding mutant caught\n", g_checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL %s (%u checks)\n", e.what(), g_checks);
        return 1;
    }
}
