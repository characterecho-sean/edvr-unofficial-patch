// The SRV turret view (issue 140; src/d3d11/srv_turret*.h, srv_turret.cpp). The build's own bytecode of the HUD's pass-through geometry shader (kSrvTurretHudGsBytecode: the
// build compiles srv_turret_hud_shader.h's HLSL, and this rig checks the bytes are that text's) and the production binding, constants and maths run on WARP, with the D3D
// debug layer when the machine has it, in front of a stand-in for the game's vertex shader E508648660A352B2 -- the same output signature (TEXCOORD 0 xy at register 0,
// SV_POSITION at register 1; reflection compares the two), the same clip rows (o1.x = dp4(cb0[4], r0), o1.y = dp4(cb0[5], r0), o1.w = dp4(cb0[7], r0), o1.z = |o1.w|).
//
//   THE SHIFT. A quad at view depth w = 1.5 with the gap 1.02 m and s = +0.0259 (Sean's Pimax: P00 0.940, Elite's eye separation 55.1 mm, the left eye) lands shifted by the
//   formula's amount, measured in pixels from the texture-coordinate gradient (sub-pixel): clip x + s gap / (w - gap). The right eye (s = -0.0259) shifts the other way.
//   THE GEOMETRY. The same shift is what a quad truly moved `gap` metres nearer along its ray from the head centre looks like to each eye, rendered with the plain shader
//   and Elite's off-centre projection: the two images agree to a hundredth of a pixel, the quad's size unchanged (the cyclopean direction is kept).
//   THE WINDOW. A vertex at w = 50 is unchanged, as is one at 5 m (the reticle is far) and one nearer than gap + 0.10; with the gap at 0 nothing changes; a mixed draw moves only
//   the quad in range. The texture coordinates are untouched (an integer-pixel translation reproduces the plain image shifted).
//   THE LINKAGE. The shader links against the stand-in under the debug layer with no message; a vertex shader without TEXCOORD 0 is reported by it (the control that the
//   layer really checks).
//   THE BINDING (the production class, on a real context): in and out for one draw with the game's geometry stage and constant slot exactly as they were and no reference leaked or
//   kept; a geometry shader of the game's own refused with nothing touched; the restore that fails twice, the settle at the boundary and the state the game rebound; the
//   geometry stage empty after the issue. THE CONSTANTS: made once per device, rewritten only when a number changes, refused when not finite or not a window.
//   THE MUTANTS, each run through the same cases and each caught: no window, the sign, the divisor, the wrong axis, the far end of the window; and the binding whose restore
//   does nothing. --wiring scans the sources that connect all this to the game, with controls that edit a copy and must trip the pin.
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
#include "../../src/d3d11/srv_turret_hud_binding.h"
#include "../../src/d3d11/srv_turret_hud_shader.h"
#include "../../src/d3d11/srv_turret_math.h"
#include "temporal_shader_bytecode.h"

using Microsoft::WRL::ComPtr;
namespace st = edvr::srv_turret;

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

// ---------------------------------------------------------------- the game's vertex shader, as a stand-in with its output signature and clip rows
static const char* kGameVs = R"HLSL(
cbuffer C0 : register(b0) { float4 cb0[8]; };
struct VIn { float4 p : POSITION; float2 uv : TEXCOORD0; };
struct VOut { float2 uv : TEXCOORD0; float4 pos : SV_Position; };
VOut main(VIn i) {
    float4 r0 = float4(i.p.xyz, 1.0);
    VOut o;
    o.uv = i.uv;
    o.pos.x = dot(cb0[4], r0);
    o.pos.y = dot(cb0[5], r0);
    o.pos.w = dot(cb0[7], r0);
    o.pos.z = abs(o.pos.w);
    return o;
}
)HLSL";
// The pixel stage: the texture coordinate as the colour (the sub-pixel measure), alpha 1.
static const char* kGamePs = "float4 main(float2 uv : TEXCOORD0) : SV_Target { return float4(uv, 0.0, 1.0); }\n";
// A vertex shader WITHOUT the texture coordinate: the linkage control.
static const char* kBadVs = R"HLSL(
cbuffer C0 : register(b0) { float4 cb0[8]; };
float4 main(float4 p : POSITION) : SV_Position { float4 r0 = float4(p.xyz, 1.0); return float4(dot(cb0[4], r0), dot(cb0[5], r0), abs(dot(cb0[7], r0)), dot(cb0[7], r0)); }
)HLSL";

static ComPtr<ID3DBlob> g_vsCode, g_psCode;
static ComPtr<ID3D11VertexShader> g_vs;
static ComPtr<ID3D11PixelShader> g_ps;
static ComPtr<ID3D11InputLayout> g_layout;
static ComPtr<ID3D11RasterizerState> g_rs;   // no culling: the stand-in quads are wound either way, as the game's state does not cull its panels

static void setupGamePair() {
    g_vsCode = compileHlsl(kGameVs, "vs_5_0");
    g_psCode = compileHlsl(kGamePs, "ps_5_0");
    ck(g_dev->CreateVertexShader(g_vsCode->GetBufferPointer(), g_vsCode->GetBufferSize(), nullptr, &g_vs), "CreateVertexShader");
    ck(g_dev->CreatePixelShader(g_psCode->GetBufferPointer(), g_psCode->GetBufferSize(), nullptr, &g_ps), "CreatePixelShader");
    const D3D11_INPUT_ELEMENT_DESC elements[2] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                                  {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ck(g_dev->CreateInputLayout(elements, 2, g_vsCode->GetBufferPointer(), g_vsCode->GetBufferSize(), &g_layout), "CreateInputLayout");
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ck(g_dev->CreateRasterizerState(&rd, &g_rs), "CreateRasterizerState");
}

// ---------------------------------------------------------------- one drawn frame
struct Vtx {
    float p[4];
    float uv[2];
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
        v.push_back(Vtx{{X, Y, Z, 1.0f}, {u, t}});
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

struct DrawOpts {
    int eye = 0;                 // which constants buffer
    bool skipRestore = false;    // MUTANT: a restore that does nothing
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
    g_ctx->RSSetState(g_rs.Get());
    const UINT stride = sizeof(Vtx), offset = 0;
    ID3D11Buffer* vbs[1] = {vb.Get()};
    g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    g_ctx->IASetInputLayout(g_layout.Get());
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, cb0.GetAddressOf());
    g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
    t.clear();
    st::Binding binding;
    bool bound = false;
    if (gs) {
        ID3D11Buffer* cb = g_constants[o.eye].get(g_ctx.Get(), params);
        check(cb != nullptr, "the turret constants are made");
        bound = binding.begin(g_ctx.Get(), gs, cb) == st::Binding::Begin::kBound;
        check(bound, "the production binding binds the shader");
    }
    g_ctx->Draw(UINT(verts.size()), 0);
    if (bound) binding.finish(g_ctx.Get(), o.skipRestore ? noopSetGs : st::directSetGs);
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
    check(st::ipdUsable(0.0551f) && st::ipdUsable(0.0632f * 1.5f) && !st::ipdUsable(0.0f) && !st::ipdUsable(NAN) && !st::ipdUsable(0.5f) && st::p00Usable(0.94f) && !st::p00Usable(0.0f) && !st::p00Usable(INFINITY),
          "the separation and P00 are used only when they are a human head and a projection");
    check(static_cast<int>(st::Refuse::kCount) == 5 && std::strcmp(st::refuseName(st::Refuse::kNoEye), "no-eye") == 0 && std::strcmp(st::refuseName(st::Refuse::kGameGs), "game-gs") == 0 &&
              std::strcmp(st::refuseName(st::Refuse::kTopology), "topology") == 0 && std::strcmp(st::refuseName(st::Refuse::kNotReady), "not-ready") == 0 &&
              std::strcmp(st::refuseName(st::Refuse::kNoGeometry), "no-geometry") == 0,
          "five refusal reasons, in the order the 10 s line prints them");
    check(st::kSpriteVs == 0xE508648660A352B2ull && st::kSpritePs == 0x63ABD86359B57D01ull, "the targeting sprite's shader pair");
}

// ---------------------------------------------------------------- the geometry, run on the production shader and on each mutant
static void geometryCases(ID3D11GeometryShader* gs, Fails& f) {
    const Cam cam;
    const float ipd = 0.0551f, gap = 1.02f;
    const float w0 = 1.5f, qx0 = -0.30f, qx1 = 0.30f, qy0 = -0.20f, qy1 = 0.20f;
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
                 "a quad at w = 1.5 lands shifted by s gap / (w - gap) / w half-widths of the target: the formula's pixels");
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
        for (float w : {50.0f, 5.0f, 4.0f, 1.0f, 1.11f}) {
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
        const Frame plain = render(v, cam, nullptr, st::paramsFor(0.0259f, 0.0f));
        const Frame zero = render(v, cam, gs, st::paramsFor(0.0259f, 0.0f));
        f.expect(covered(plain) > 1000 && maxDiff(plain, zero) == 0.0, "with the gap at 0 the image is the plain one, bit for bit");
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

// ---------------------------------------------------------------- the linkage, with the signatures compared
static void linkageCases(ID3D11GeometryShader* gs) {
    ComPtr<ID3D11ShaderReflection> vsr, gsr;
    ck(D3DReflect(g_vsCode->GetBufferPointer(), g_vsCode->GetBufferSize(), __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(vsr.GetAddressOf())), "reflect the vertex shader");
    {
        // the production bytes: the rig has them as the array
        ck(D3DReflect(edvr::kSrvTurretHudGsBytecode, sizeof(edvr::kSrvTurretHudGsBytecode), __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(gsr.GetAddressOf())),
           "reflect the production geometry shader");
    }
    D3D11_SHADER_DESC vd{}, gd{};
    vsr->GetDesc(&vd);
    gsr->GetDesc(&gd);
    check(vd.OutputParameters == 2 && gd.InputParameters == 2, "two signature elements each: the texture coordinate and the position");
    for (UINT i = 0; i < 2; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC o{}, in{};
        ck(vsr->GetOutputParameterDesc(i, &o), "output element");
        ck(gsr->GetInputParameterDesc(i, &in), "input element");
        check(!std::strcmp(o.SemanticName, in.SemanticName) && o.SemanticIndex == in.SemanticIndex && o.Register == in.Register && o.Mask == in.Mask &&
                  o.ComponentType == in.ComponentType && o.SystemValueType == in.SystemValueType,
              "the geometry shader's input element is the vertex shader's output element: semantic, index, register, mask and type");
        if (i == 0) check(!std::strcmp(o.SemanticName, "TEXCOORD") && o.Register == 0 && o.Mask == 3, "register 0 is TEXCOORD0 xy");
        if (i == 1) check(!std::strcmp(o.SemanticName, "SV_Position") && o.Register == 1 && o.Mask == 15, "register 1 is SV_Position xyzw");
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
        // the control: a vertex shader without TEXCOORD0 is reported
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
    bad.gap = -0.1f;
    check(c.get(g_ctx.Get(), bad) == nullptr, "a negative gap: refused");
    check(c.get(g_ctx.Get(), a) == first, "...and the buffer is still the good one");
    ComPtr<ID3D11Device> d2;
    ComPtr<ID3D11DeviceContext> c2;
    ck(edvr::systemD3D11CreateDevice()(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d2, nullptr, &c2), "second device");
    ID3D11Buffer* other = c.get(c2.Get(), a);
    check(other && other != first && c.written() == a, "another device: the buffer is made again for it");
    c.reset();
    check(c.buffer() == nullptr && !c.failed(), "reset releases the buffer");
    // the production arrays also carry the key defaults the module reads
    check(st::kBackDefaultCm == 60 && st::kGapDefaultCm == 102, "the keys' defaults");
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
        const HRESULT made = edvr::systemD3D11CreateDevice()(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
        if (FAILED(made))
            ck(edvr::systemD3D11CreateDevice()(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx), "device");
        g_dev.As(&g_messages);
        std::printf("srv_turret_hud_test: WARP%s\n", SUCCEEDED(made) ? " with the debug layer" : "");
        setupGamePair();

        // The shader: the build's bytes are the HLSL text's compile, and the device takes them.
        const auto compiled = compileHlsl(edvr::kSrvTurretHudGs, "gs_5_0");
        check(compiled->GetBufferSize() == sizeof(edvr::kSrvTurretHudGsBytecode) &&
                  !std::memcmp(compiled->GetBufferPointer(), edvr::kSrvTurretHudGsBytecode, sizeof(edvr::kSrvTurretHudGsBytecode)),
              "the build's bytecode is exactly the compile of srv_turret_hud_shader.h's text");
        ComPtr<ID3D11GeometryShader> gs;
        ck(g_dev->CreateGeometryShader(edvr::kSrvTurretHudGsBytecode, sizeof(edvr::kSrvTurretHudGsBytecode), nullptr, &gs), "the device takes the production shader");

        constantsCases();
        linkageCases(gs.Get());

        Fails production;
        geometryCases(gs.Get(), production);
        bindingCases(gs.Get(), production, true);
        afterIssueCases(gs.Get(), production, false);
        if (production.n) std::printf("first failing production check: %s (%u failed)\n", production.first.c_str(), production.n);
        check(production.n == 0, "the production shader and binding pass every geometry, binding and issue-end case");

        // THE MUTANTS: each through the same cases, each must fail at least one.
        const std::string text = edvr::kSrvTurretHudGs;
        struct Mutant {
            const char* name;
            std::string text;
        };
        const Mutant mutants[] = {
            {"no depth window", replaceAll(text, "if (w > wMin && w < wMax) o.pos.x +=", "o.pos.x +=")},
            {"the sign flipped", replaceAll(text, "o.pos.x += s * gap / (w - gap)", "o.pos.x -= s * gap / (w - gap)")},
            {"the divisor w, not w - gap", replaceAll(text, "s * gap / (w - gap)", "s * gap / w")},
            {"the wrong axis", replaceAll(text, "o.pos.x += s * gap / (w - gap)", "o.pos.y += s * gap / (w - gap)")},
            {"the far end of the window ignored", replaceAll(text, "w < wMax", "w < 1000.0")},
            {"the near end of the window ignored", replaceAll(text, "w > wMin &&", "w > 0.0 &&")},
        };
        for (const Mutant& m : mutants) {
            check(m.text != text, "a mutant differs from the production text");
            const auto mg = gsFromText(m.text);
            Fails fails;
            geometryCases(mg.Get(), fails);
            char what[160];
            std::snprintf(what, sizeof(what), "the mutant \"%s\" is caught by the geometry cases", m.name);
            check(fails.n > 0, what);
        }
        {
            Fails fails;
            afterIssueCases(gs.Get(), fails, true);
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
        std::printf("PASS srv_turret_hud_test: %u checks; the turret shader, binding, constants and maths on WARP, six shader mutants and one binding mutant caught\n", g_checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL %s (%u checks)\n", e.what(), g_checks);
        return 1;
    }
}
