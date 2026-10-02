// The surface strip, RENDERED (job 4, the mirror fix; docs/intro-video.md).
//
// Every other rig pins the strip's numbers: its bytes, its flags, its state. None of them draws a picture, and the first flight of the curved movie and
// splash came out MIRRORED left to right with every one of those numbers right: the strip's u ran with its x, and the intro composite's placement has
// its +x running to the viewer's LEFT. This rig draws the production strip through the intro composite's own vertex shader and asks the only question
// that matters: does left stay left and up stay up.
//
// THE PIPELINE, on a WARP device with System32's d3d11 (no proxy): a vertex shader that is exactly docs/shaders/intro-composite-vs.asm, written back as
// HLSL (POSITION float3 at 0, TEXCOORD float2 at 12, stride 20; cbuffer b2 of five float4s; xy = pos.xy * c[0].xy; sv = xy.x c[1] + xy.y c[2] + pos.z c[3] +
// c[4]; uv passed through), a pixel shader that point-samples t0 at uv, a 128x72 target, and an EXPLICIT rasterizer state that culls back faces, as the
// game's does -- so the production cull-off derivation (panel_curve.cpp, panelCurveSurfaceDraw) is exercised and a strip that comes out facing away from
// the viewer is a blank picture. The texture is 8x8 with four distinct quadrant colours: top left red, top right green, bottom left blue, bottom right
// white. The picture is read back and classified at 25 % and 75 % of the bounding box of what was drawn, in both directions; upright is red green over
// blue white, left to right and top to bottom.
//
//   R1 THE ON-FOOT PIN    the production on-foot strip (panelCurveSubstitute, the z-gain override so nothing is learned, as panel_curve_test C2 does)
//                         through a STANDARD transform in the rig's own constants (+x right, +y up): left stays left, up stays up, at curvature
//                         0.05, 0.3 and 0.6. This is what the screen's strip has always been and what the surface strip's flag must never change.
//   R2 THE MOVIE          production introPanelOnComposite (stubs as intro_curve_test has them: pose, tangents, resampler, binding shadow) binds EDVR's world
//                         constants at VS b2, then panelCurveSurfaceDraw(introPanelStripGain(), +1, introPanelStripReverseU(), ...) draws the strip, then
//                         introPanelEndDraw: both eyes, a head turned 0 and +-10 degrees, curvature 0.05, 0.3 and 0.6. Left stays left, up stays up.
//   R3 THE SPLASH         production introCurveOnComposite on the game's own constants -- the 2026-08-28 capture 2, both eyes, bound at VS b2 --
//                         learned by its settle ticks, then panelCurveSurfaceDraw(introCurveGain(), introCurveToward(), introCurveReverseU(), ...):
//                         left stays left, up stays up, at the same three curvatures.
//   R4 CONTROLS           the checks can fail, and the flag is derived, not a constant: the same R2 and R3 draws with reverseU forced to the OTHER value
//                         come out MIRRORED (asserted: red and green swapped, blue and white swapped, and the upright test says no); a placement
//                         whose +x runs to the viewer's RIGHT -- the splash capture with its x column negated, the movie's own with a reflected pose --
//                         read by the production code as running right, drawn with u along x, and left still stays left.
//
// tools\surface_strip_render_test\mutants.py compiles this rig against copies of the production sources with ONE rule flipped each (the strip's u
// ignoring the flag, the flag inverted, the cull-off not applied, the direction rule's sign, the movie's and the splash's flags ...) and requires the rig to
// fail on the case that belongs to the rule: every check carries a label "R<case>.<what>", and that prefix is what the tool looks for.
//
// Usage: --self-test [<repo root, ignored>] [--dump <dir>]   |   --dry-run (nothing run, nothing written)
// --dump writes every picture the cases look at as a P6 PPM into <dir> (the one thing this rig can write, and only when asked).
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../../src/common/config.h"
#include "../../src/common/frame_flag.h"
#include "../../src/common/log.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/system_d3d11.h"
#include "binding_shadow.h"
#include "intro_curve.h"
#include "intro_curve_math.h"
#include "intro_panel.h"
#include "intro_upscale.h"
#include "panel_curve.h"
#include "screen_motion.h"

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------------------------------------------------------------------------
// The harness: every failing check is printed once under its label, and the run goes on (one run shows every rule that broke).
// ---------------------------------------------------------------------------------------------------------------------------------
unsigned g_checks = 0;
unsigned g_failed = 0;
std::map<std::string, unsigned> g_failCount;
std::string g_note;          // what the loop is on (curvature, pose, eye), appended to a failure's line
std::string g_dumpDir;       // --dump: where the pictures go; empty writes nothing

std::string fmt(const char* f, ...) {
    char buf[700];
    va_list ap;
    va_start(ap, f);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, f, ap);
    va_end(ap);
    return std::string(buf);
}

void check(bool ok, const std::string& label, const std::string& detail = std::string()) {
    ++g_checks;
    if (ok) return;
    ++g_failed;
    if (++g_failCount[label] == 1) {
        std::string line = "FAIL: " + label;
        if (!detail.empty()) line += " -- " + detail;
        if (!g_note.empty()) line += " [" + g_note + "]";
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The picture: classified at 25 % and 75 % of the bounding box of what was drawn.
// ---------------------------------------------------------------------------------------------------------------------------------
constexpr UINT kW = 128, kH = 72;
constexpr uint32_t kFillW = 1920, kFillH = 1080;   // the surface the movie is converted into: only compared, by the movie's module

// The clear colour is a grey none of the texture's four colours is, so "drawn" is "not that grey".
constexpr float kClear[4] = {0.125f, 0.125f, 0.125f, 1.0f};   // 32, 32, 32 in RGBA8
struct Rgb {
    int r, g, b;
};
constexpr Rgb kRed{255, 0, 0}, kGreen{0, 255, 0}, kBlue{0, 0, 255}, kWhite{255, 255, 255}, kGrey{32, 32, 32};

enum class Colour { kNone, kRed, kGreen, kBlue, kWhite, kOther };
const char* nameOf(Colour c) {
    switch (c) {
    case Colour::kNone: return "nothing";
    case Colour::kRed: return "red";
    case Colour::kGreen: return "green";
    case Colour::kBlue: return "blue";
    case Colour::kWhite: return "white";
    default: return "other";
    }
}

struct Image {
    std::vector<uint8_t> px;   // kW x kH, RGBA8, row 0 at the top
    const uint8_t* at(int x, int y) const { return &px[(static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4]; }
};

bool within(const uint8_t* p, Rgb c, int tol) { return std::abs(p[0] - c.r) <= tol && std::abs(p[1] - c.g) <= tol && std::abs(p[2] - c.b) <= tol; }
Colour classify(const uint8_t* p) {
    if (within(p, kGrey, 8)) return Colour::kNone;
    if (within(p, kRed, 40)) return Colour::kRed;
    if (within(p, kGreen, 40)) return Colour::kGreen;
    if (within(p, kBlue, 40)) return Colour::kBlue;
    if (within(p, kWhite, 40)) return Colour::kWhite;
    return Colour::kOther;
}

struct Quads {
    bool drawn = false;                 // a box of at least 16 x 8 pixels was drawn into
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;   // the bounding box of what was drawn
    Colour tl = Colour::kNone, tr = Colour::kNone, bl = Colour::kNone, br = Colour::kNone;
};
Quads sampleQuads(const Image& img) {
    Quads q;
    q.x0 = static_cast<int>(kW);
    q.y0 = static_cast<int>(kH);
    for (int y = 0; y < static_cast<int>(kH); ++y)
        for (int x = 0; x < static_cast<int>(kW); ++x) {
            if (classify(img.at(x, y)) == Colour::kNone) continue;
            q.x0 = std::min(q.x0, x);
            q.y0 = std::min(q.y0, y);
            q.x1 = std::max(q.x1, x);
            q.y1 = std::max(q.y1, y);
        }
    if (q.x1 < q.x0 || q.y1 < q.y0) return q;
    q.drawn = q.x1 - q.x0 + 1 >= 16 && q.y1 - q.y0 + 1 >= 8;
    const double w = q.x1 - q.x0, h = q.y1 - q.y0;
    const int xl = q.x0 + static_cast<int>(std::lround(0.25 * w)), xr = q.x0 + static_cast<int>(std::lround(0.75 * w));
    const int yt = q.y0 + static_cast<int>(std::lround(0.25 * h)), yb = q.y0 + static_cast<int>(std::lround(0.75 * h));
    // A panel seen at an angle (the splash's real captures, the eye whose panel is off to one side) is a skewed quadrilateral, and a corner of its
    // bounding box's middle half can fall just outside it. Such a sample is moved along its own column toward the middle of the box until it is on the
    // picture, at most 8 pixels: the same column, so the same side of the picture's vertical centre line, and the same half vertically while the
    // move is that short. A sample that is still off the picture after that is "nothing", and the case fails on it.
    const int yMiddle = (q.y0 + q.y1) / 2;
    const auto sample = [&](int x, int y) {
        Colour c = classify(img.at(x, y));
        for (int step = 0; c == Colour::kNone && step < 8; ++step) {
            y += y < yMiddle ? 1 : -1;
            c = classify(img.at(x, y));
        }
        return c;
    };
    q.tl = sample(xl, yt);
    q.tr = sample(xr, yt);
    q.bl = sample(xl, yb);
    q.br = sample(xr, yb);
    return q;
}
// Left stays left and up stays up: the texture's own arrangement.
bool upright(const Quads& q) { return q.drawn && q.tl == Colour::kRed && q.tr == Colour::kGreen && q.bl == Colour::kBlue && q.br == Colour::kWhite; }
// The same picture mirrored left to right: what a strip whose u runs the wrong way draws.
bool mirrored(const Quads& q) { return q.drawn && q.tl == Colour::kGreen && q.tr == Colour::kRed && q.bl == Colour::kWhite && q.br == Colour::kBlue; }
std::string describe(const Quads& q) {
    if (!q.drawn) return fmt("nothing (or less than 16 x 8 pixels) was drawn: box %d,%d to %d,%d", q.x0, q.y0, q.x1, q.y1);
    std::string what = "an unrecognised arrangement";
    if (upright(q)) what = "upright";
    else if (mirrored(q)) what = "MIRRORED left to right";
    else if (q.tl == Colour::kBlue && q.tr == Colour::kWhite && q.bl == Colour::kRed && q.br == Colour::kGreen) what = "UPSIDE DOWN";
    else if (q.tl == Colour::kWhite && q.tr == Colour::kBlue && q.bl == Colour::kGreen && q.br == Colour::kRed) what = "turned half way round";
    return fmt("%s: top left %s, top right %s, bottom left %s, bottom right %s (box %d,%d to %d,%d)", what.c_str(), nameOf(q.tl), nameOf(q.tr), nameOf(q.bl), nameOf(q.br), q.x0, q.y0,
               q.x1, q.y1);
}

void dumpImage(const std::string& name, const Image& img) {
    if (g_dumpDir.empty()) return;
    std::string safe;
    for (char c : name) safe.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' ? c : '_');
    std::ofstream out(g_dumpDir + "\\" + safe + ".ppm", std::ios::binary);
    out << "P6\n" << kW << " " << kH << "\n255\n";
    for (int y = 0; y < static_cast<int>(kH); ++y)
        for (int x = 0; x < static_cast<int>(kW); ++x) out.write(reinterpret_cast<const char*>(img.at(x, y)), 3);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// What the modules call that this rig does not link: the vr half's published channel, the resampler, the binding shadow's resolver, the screen's
// motion pass; and the binding shadow's storage (the splash's module reads the bound VS hash and the view at PS slot 0 from it: this rig is the game
// that bound them).
// ---------------------------------------------------------------------------------------------------------------------------------
namespace stub {
float pose[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
// A wide symmetric frustum (half angle 58 degrees across, 42 down), so the panel -- 106 degrees wide at the movie's distance -- is in view and the
// picture has room for four quadrants; the eye is read from the game's own constants, not from these.
float outer = 1.6f, inner = 1.6f;
float top = 0.9f, bot = 0.9f;
}  // namespace stub

namespace edvr {
bool headPose(float* out12) {
    std::memcpy(out12, stub::pose, sizeof(stub::pose));
    return true;
}
bool eyeTangents(float* outerMag, float* innerMag) {
    *outerMag = stub::outer;
    *innerMag = stub::inner;
    return true;
}
bool eyeTangentsVertical(float* topMag, float* botMag) {
    *topMag = stub::top;
    *botMag = stub::bot;
    return true;
}
void requestIntroRecentre() {}
bool sceneArrived() { return false; }

bool introUpscaleWants() { return false; }
bool introUpscaleBegin(ID3D11DeviceContext*, ID3D11ShaderResourceView*) { return false; }
void introUpscaleEnd(ID3D11DeviceContext*) {}
void introUpscaleFrameEnd() {}
void introUpscaleShutdown() {}

// The real resolver is binding_shadow.cpp, which carries the whole hook layer. The modules read only a buffer's byte width from it (info.a).
bool bindingResolveResource(void* resource, ResourceInfo* out) {
    if (!resource || !out) return false;
    ID3D11Resource* r = static_cast<ID3D11Resource*>(resource);
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC d{};
    static_cast<ID3D11Buffer*>(resource)->GetDesc(&d);
    *out = ResourceInfo{};
    out->isBuffer = true;
    out->a = d.ByteWidth;
    out->b = d.StructureByteStride;
    out->resource = resource;
    return true;
}

// The on-foot screen's per-eye motion pass (screen_motion.cpp), which the substitution issues with the strip still bound: nothing here reaches it.
void screenMotionDraw(ID3D11DeviceContext*, PanelCurveDrawFn, unsigned, unsigned, unsigned, int, unsigned, const float*) {}

namespace detail {
BindingSlot g_bindingSlots[static_cast<size_t>(BindSlot::Count)];
}  // namespace detail
}  // namespace edvr

namespace {

using namespace edvr;

// ---------------------------------------------------------------------------------------------------------------------------------
// The game's constants, cb2[0..4] = 20 floats.
// ---------------------------------------------------------------------------------------------------------------------------------
// The movie's own, docs/frontier-intro-video-report.md: 512 x 288 PIXELS, pixels to NDC (cb2[1].x is NEGATIVE: +x runs to the viewer's left), w a
// constant 1, the frustum centre in cb2[4].x, positive in the left eye.
const float kStockL[20] = {512.0f, 288.0f, 0, 0,   -0.000368732f, 0, 0, 0,   0, 0.000373413f, 0, 0,   0, 0, 0, 0,   0.193907f, 0, 0, 1.0f};
const float kStockR[20] = {512.0f, 288.0f, 0, 0,   -0.000368732f, 0, 0, 0,   0, 0.000373413f, 0, 0,   0, 0, 0, 0,   -0.1940f, 0, 0, 1.0f};
// The splash's own, the 2026-08-28 capture 2 (Frontier edvr_gfx_20260828_182818.log, the `DCW read` lines): the panel in front of the viewer, both eyes.
// cb2[1].x is -0.7807 in both: the same convention, +x running to the viewer's left. (The same numbers tools\intro_curve_module_test and
// tools\intro_curve_math_test hold.)
const float kSplashA[20] = {4.44444f, 2.5f, 0.0f, 0.0f,   -0.780684f, -0.0197438f, 9.48621e-08f, -0.000948464f,   -0.0047429f, 0.788105f, -7.60756e-06f, 0.076063f,
                            -0.194148f, 0.0601386f, 9.97268e-05f, -0.997103f,   -0.0725725f, -0.247872f, 0.0996338f, 3.76108f};
const float kSplashB[20] = {4.44444f, 2.5f, 0.0f, 0.0f,   -0.780317f, -0.0197438f, 9.48621e-08f, -0.000948464f,   -0.0342501f, 0.788105f, -7.60756e-06f, 0.076063f,
                            0.192659f, 0.0601386f, 9.97268e-05f, -0.997103f,   -1.57885f, -0.247872f, 0.0996338f, 3.76108f};
// A reflected pose (a mirror in x, determinant -1: not a head -- the movie's module takes any 12 floats): the movie builds its +x axis as minus the
// head's x axis, so under a reflection the placement's +x runs to the viewer's RIGHT. The movie's own never does; this is how the rig gets one.
const float kPoseMirror[12] = {-1.0f, 0.0f, 0.0f, 0.0f,   0.0f, 1.0f, 0.0f, 0.0f,   0.0f, 0.0f, 1.0f, 0.0f};
const float kPoseMirrorYaw[12] = {-0.96f, 0.0f, -0.28f, 0.10f,   0.0f, 1.0f, 0.0f, 0.05f,   -0.28f, 0.0f, 0.96f, -0.20f};

// ---------------------------------------------------------------------------------------------------------------------------------
// The shaders: the intro composite's own vertex shader (docs/shaders/intro-composite-vs.asm) and a pixel shader that samples t0 at uv.
// ---------------------------------------------------------------------------------------------------------------------------------
const char* const kVsText = R"(
cbuffer b2 : register(b2) { float4 c[5]; };
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float2 uv : TEXCOORD0; float4 sv : SV_Position; };
VSOut main(VSIn i) {
    VSOut o;
    o.uv = i.uv;
    float2 xy = i.pos.xy * c[0].xy;
    o.sv = xy.x * c[1] + xy.y * c[2] + i.pos.z * c[3] + c[4];
    return o;
}
)";
const char* const kPsText = R"(
Texture2D<float4> t0 : register(t0);
SamplerState s0 : register(s0);
float4 main(float2 uv : TEXCOORD0) : SV_Target { return t0.Sample(s0, uv); }
)";

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> il;
    ComPtr<ID3D11Texture2D> tex, rt, stage;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rs;    // CULL_BACK: the game's, so a strip that faces away is a blank picture
    ComPtr<ID3D11DepthStencilState> dss;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11Buffer> ownCb;          // R1's placement
    bool ok = false;
};

bool compileShader(const char* src, const char* target, ComPtr<ID3DBlob>* out) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(src, std::strlen(src), "surface_strip_render_test", nullptr, nullptr, "main", target, 0, 0, out->GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr) || !*out) {
        std::printf("FAIL: R0.shader: %s did not compile: %s\n", target, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no message)");
        return false;
    }
    return true;
}

ComPtr<ID3D11Buffer> makeCb(Gpu& g, const float* f20) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = 80;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{f20, 0, 0};
    ComPtr<ID3D11Buffer> out;
    g.dev->CreateBuffer(&d, &init, &out);
    return out;
}

Gpu makeGpu() {
    Gpu g;
    const PFN_D3D11_CREATE_DEVICE create = systemD3D11CreateDevice();
    if (!create) return g;
    D3D_FEATURE_LEVEL fl{};
    if (FAILED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx)) || !g.dev || !g.ctx) return g;
    ID3D11Device* dev = g.dev.Get();

    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!compileShader(kVsText, "vs_5_0", &vsBlob) || !compileShader(kPsText, "ps_5_0", &psBlob)) return g;
    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g.vs);
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g.ps);
    const D3D11_INPUT_ELEMENT_DESC layout[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                                               {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    dev->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g.il);

    // The picture the strip shows: 8 x 8, row 0 at the top, four quadrants of four distinct colours.
    uint8_t texels[8 * 8 * 4];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const bool topHalf = y < 4, leftHalf = x < 4;
            const Rgb c = topHalf ? (leftHalf ? kRed : kGreen) : (leftHalf ? kBlue : kWhite);
            uint8_t* p = &texels[(y * 8 + x) * 4];
            p[0] = static_cast<uint8_t>(c.r);
            p[1] = static_cast<uint8_t>(c.g);
            p[2] = static_cast<uint8_t>(c.b);
            p[3] = 255;
        }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 8;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{texels, 8 * 4, 0};
    dev->CreateTexture2D(&td, &sd, &g.tex);
    if (g.tex) dev->CreateShaderResourceView(g.tex.Get(), nullptr, &g.srv);

    td.Width = kW;
    td.Height = kH;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    dev->CreateTexture2D(&td, nullptr, &g.rt);
    if (g.rt) dev->CreateRenderTargetView(g.rt.Get(), nullptr, &g.rtv);
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&td, nullptr, &g.stage);

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.ComparisonFunc = D3D11_COMPARISON_NEVER;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&smp, &g.sampler);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;   // the game's: back faces culled (clockwise front), so a surface that faces away from the viewer is not drawn
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g.rs);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dev->CreateDepthStencilState(&dd, &g.dss);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, &g.blend);

    const float zero[20] = {0};
    g.ownCb = makeCb(g, zero);
    g.ok = g.vs && g.ps && g.il && g.tex && g.srv && g.rt && g.rtv && g.stage && g.sampler && g.rs && g.dss && g.blend && g.ownCb;
    return g;
}

// The pipeline around the draw, bound once and never rebound: the strip changes the input assembler's vertex buffer, index buffer and topology for its
// draw and puts them back, and the rasterizer state for its draw and puts it back; the vertex shader's b2 is the placement the case binds.
void bindPipeline(Gpu& g) {
    ID3D11DeviceContext* c = g.ctx.Get();
    c->IASetInputLayout(g.il.Get());
    c->VSSetShader(g.vs.Get(), nullptr, 0);
    c->PSSetShader(g.ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = g.srv.Get();
    c->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = g.sampler.Get();
    c->PSSetSamplers(0, 1, &smp);
    c->RSSetState(g.rs.Get());
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
    c->RSSetViewports(1, &vp);
    ID3D11RenderTargetView* rtv = g.rtv.Get();
    c->OMSetRenderTargets(1, &rtv, nullptr);
    c->OMSetBlendState(g.blend.Get(), nullptr, 0xFFFFFFFFu);
    c->OMSetDepthStencilState(g.dss.Get(), 0);
}

void clearTarget(Gpu& g) { g.ctx->ClearRenderTargetView(g.rtv.Get(), kClear); }

Image readTarget(Gpu& g) {
    Image img;
    img.px.assign(static_cast<size_t>(kW) * kH * 4, 0);
    g.ctx->CopyResource(g.stage.Get(), g.rt.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(g.stage.Get(), 0, D3D11_MAP_READ, 0, &m)) || !m.pData) return img;
    for (UINT y = 0; y < kH; ++y) std::memcpy(&img.px[static_cast<size_t>(y) * kW * 4], static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, kW * 4);
    g.ctx->Unmap(g.stage.Get(), 0);
    return img;
}

// The game's real DrawIndexedInstanced, as vscreen.cpp hands it to the strip: issued through the context.
void __stdcall realDraw(ID3D11DeviceContext* ctx, UINT count, UINT instances, UINT start, INT base, UINT startInstance) {
    ctx->DrawIndexedInstanced(count, instances, start, base, startInstance);
}

void setPose(const float* p) { std::memcpy(stub::pose, p, sizeof(stub::pose)); }
// A head turned about the vertical (a pose is a row-major 3x4: the rotation, then the translation): yaw 0 is straight ahead.
void setYaw(double degrees) {
    const double a = degrees * 3.14159265358979323846 / 180.0;
    const float c = static_cast<float>(std::cos(a)), s = static_cast<float>(std::sin(a));
    const float p[12] = {c, 0.0f, s, 0.0f,   0.0f, 1.0f, 0.0f, 0.0f,   -s, 0.0f, c, 0.0f};
    setPose(p);
}

void setCurvature(const char* v) {
    Config& c = Config::get();
    c.set("fix.panel_curvature", v);
    panelCurveConfigure(c);
    introPanelConfigure(c);
}

// One shot: what the module said, what was drawn, and what the picture shows.
struct Shot {
    bool bound = false, armed = false, reverse = false, drew = false;
    Image img;
    Quads q;
};

void look(Shot* s, Gpu& g, const std::string& name) {
    s->img = readTarget(g);
    s->q = sampleQuads(s->img);
    dumpImage(name, s->img);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// R1: the on-foot strip through a standard transform.
// ---------------------------------------------------------------------------------------------------------------------------------
void caseR1(Gpu& g) {
    std::printf("R1 the on-foot strip through a standard transform (+x right, +y up): left stays left, up stays up\n");
    ID3D11DeviceContext* ctx = g.ctx.Get();
    Config& c = Config::get();
    c.set("advanced.panel_curvature_z_gain", "35.556");   // the override: nothing is learned from the game's buffers (panel_curve_test C2 does the same)
    // A standard placement: cb2[0] = (1, 1), cb2[1] = (0.8, 0, 0, 0) -- one unit of x is 0.8 of the view to the RIGHT --, cb2[2] = (0, 0.45, 0, 0) -- one unit
    // of y is 0.45 UP --, no z column, and a w of one: an orthographic screen, the simplest transform that has the handedness of the on-foot panel's.
    const float placement[20] = {1.0f, 1.0f, 0.0f, 0.0f,   0.8f, 0.0f, 0.0f, 0.0f,   0.0f, 0.45f, 0.0f, 0.0f,   0.0f, 0.0f, 0.0f, 0.0f,   0.0f, 0.0f, 0.5f, 1.0f};
    ctx->UpdateSubresource(g.ownCb.Get(), 0, nullptr, placement, 0, 0);
    ID3D11Buffer* own = g.ownCb.Get();
    ctx->VSSetConstantBuffers(2, 1, &own);
    for (const char* curvature : {"0.05", "0.3", "0.6"}) {
        g_note = fmt("curvature %s", curvature);
        setCurvature(curvature);
        clearTarget(g);
        const bool drew = panelCurveSubstitute(ctx, realDraw, false);
        Shot s;
        s.drew = drew;
        look(&s, g, fmt("R1_curvature_%s", curvature));
        check(drew, "R1.substituted", "panelCurveSubstitute did not draw the on-foot strip");
        check(s.q.drawn, "R1.drawn", describe(s.q) + " (the on-foot strip faces the viewer through a standard transform, so the game's back-face cull keeps it)");
        check(upright(s.q), "R1.upright", "the on-foot strip's picture is not upright: " + describe(s.q));
    }
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// R2: the movie.
// ---------------------------------------------------------------------------------------------------------------------------------
struct MovieGame {
    ComPtr<ID3D11Buffer> cb[2];   // the game's per-eye cb2 buffers: the movie's stock constants
};

// One eye of the movie as vscreen.cpp plays it: ask the module, draw the strip in place of the quad when it is armed (forceReverse < 0: the flag the
// module says; 0 or 1: that value instead, for the controls), put the game's buffer back.
Shot movieEye(Gpu& g, MovieGame& game, int eye, int forceReverse, const std::string& name) {
    ID3D11DeviceContext* ctx = g.ctx.Get();
    Shot s;
    clearTarget(g);
    ID3D11Buffer* cb = game.cb[eye].Get();
    ctx->VSSetConstantBuffers(2, 1, &cb);
    s.bound = introPanelOnComposite(ctx, 'X', 6, 1, kFillW, kFillH);
    s.armed = introPanelStripArmed();
    s.reverse = introPanelStripReverseU();
    if (s.armed) s.drew = panelCurveSurfaceDraw(ctx, introPanelStripGain(), 1, forceReverse < 0 ? s.reverse : forceReverse != 0, realDraw);
    if (s.bound) introPanelEndDraw(ctx);
    look(&s, g, name);
    return s;
}
void movieFrame(Gpu& g, MovieGame& game, int forceReverse, Shot out[2], const std::string& name) {
    introPanelNoteFill(kFillW, kFillH);
    for (int e = 0; e < 2; ++e) out[e] = movieEye(g, game, e, forceReverse, name + (e ? "_right" : "_left"));
    introPanelTick(g.ctx.Get(), false);
}

void caseR2andR4Movie(Gpu& g) {
    std::printf("R2 the movie: production introPanelOnComposite, then the strip with introPanelStripReverseU(): left stays left, up stays up\n");
    std::printf("R4 (movie) the same draws with the flag forced the other way come out mirrored; a placement whose +x runs right is drawn upright with u along x\n");
    Config& c = Config::get();
    MovieGame game;
    game.cb[0] = makeCb(g, kStockL);
    game.cb[1] = makeCb(g, kStockR);
    c.set("fix.intro_video", "screen");
    setYaw(0.0);
    setCurvature("0.3");
    // The settle frames: the first composite of each buffer starts the readback of the game's constants, and four frame edges later the module binds.
    {
        Shot scratch[2];
        for (int f = 0; f < 6; ++f) movieFrame(g, game, -1, scratch, "settle");
    }
    const struct {
        const char* name;
        double yaw;
    } poses[] = {{"yaw0", 0.0}, {"yaw+10", 10.0}, {"yaw-10", -10.0}};
    for (const char* curvature : {"0.05", "0.3", "0.6"}) {
        setCurvature(curvature);
        for (const auto& pose : poses) {
            setYaw(pose.yaw);
            Shot prod[2], other[2];
            movieFrame(g, game, -1, prod, fmt("R2_%s_%s", curvature, pose.name));
            const int forced = prod[0].reverse ? 0 : 1;   // the other value than the module's own
            movieFrame(g, game, forced, other, fmt("R4_movie_forced_%s_%s", curvature, pose.name));
            for (int e = 0; e < 2; ++e) {
                g_note = fmt("curvature %s, head %s, %s eye", curvature, pose.name, e ? "right" : "left");
                check(prod[e].bound && prod[e].armed && prod[e].reverse, "R2.armed",
                      fmt("bound %d, armed %d, reverseU %d: the movie's placement has its +x running left, so u runs against x", prod[e].bound, prod[e].armed, prod[e].reverse));
                check(prod[e].drew, "R2.drew", "panelCurveSurfaceDraw did not draw the surface strip");
                check(prod[e].q.drawn, "R2.drawn", describe(prod[e].q) + " (the strip is drawn with the cull off, so a placement that mirrors it still shows)");
                check(upright(prod[e].q), "R2.upright", "the movie's picture is not upright: " + describe(prod[e].q));
                // the control: the flag forced the other way is the picture mirrored, and the same test that passes the real one fails it
                check(other[e].drew && other[e].q.drawn, "R4.movie-control-drawn", "the control draw (reverseU forced the other way) drew nothing: " + describe(other[e].q));
                check(mirrored(other[e].q), "R4.movie-control-mirrored", "reverseU forced the other way is not the picture mirrored left to right: " + describe(other[e].q));
                check(!upright(other[e].q), "R4.movie-control-fails", "the upright check passed a picture drawn with the flag forced the wrong way: it cannot fail");
            }
        }
    }
    g_note.clear();

    // R4: a placement whose +x runs to the viewer's RIGHT. The movie's own never does, so the module is given a REFLECTED pose; the production code reads
    // the bound constants as running right, draws u along x, and left still stays left.
    setCurvature("0.3");
    for (const float* pose : {kPoseMirror, kPoseMirrorYaw}) {
        setPose(pose);
        Shot prod[2], other[2];
        const char* which = pose == kPoseMirror ? "mirror" : "mirror-yaw";
        movieFrame(g, game, -1, prod, fmt("R4_movie_right_running_%s", which));
        movieFrame(g, game, prod[0].reverse ? 0 : 1, other, fmt("R4_movie_right_running_forced_%s", which));
        for (int e = 0; e < 2; ++e) {
            g_note = fmt("reflected pose %s, %s eye", which, e ? "right" : "left");
            check(prod[e].bound && prod[e].armed && !prod[e].reverse, "R4.movie-right-running-flag",
                  fmt("bound %d, armed %d, reverseU %d: a placement whose +x runs right must run u along x", prod[e].bound, prod[e].armed, prod[e].reverse));
            check(prod[e].drew && upright(prod[e].q), "R4.movie-right-running-upright", "left does not stay left when the placement's +x runs right: " + describe(prod[e].q));
            check(other[e].drew && mirrored(other[e].q), "R4.movie-right-running-control", "the flag forced the other way is not mirrored for a right-running placement: " + describe(other[e].q));
        }
    }
    g_note.clear();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// R3: the splash.
// ---------------------------------------------------------------------------------------------------------------------------------
// One eye of the splash as vscreen.cpp plays it: the draw is the composite's (the VS hash and the view at PS slot 0 are in the shadow), ask the module,
// draw the strip with the numbers it hands over when it is armed, disarm.
Shot splashEye(Gpu& g, ID3D11Buffer* cb, int forceReverse, const std::string& name) {
    ID3D11DeviceContext* ctx = g.ctx.Get();
    Shot s;
    clearTarget(g);
    ID3D11Buffer* bound = cb;
    ctx->VSSetConstantBuffers(2, 1, &bound);
    s.armed = introCurveOnComposite(ctx, 'X', 6, 1);
    s.reverse = introCurveReverseU();
    if (s.armed) s.drew = panelCurveSurfaceDraw(ctx, introCurveGain(), introCurveToward(), forceReverse < 0 ? s.reverse : forceReverse != 0, realDraw);
    introCurveEndDraw();
    look(&s, g, name);
    return s;
}
void splashFrame(Gpu& g, ID3D11Buffer* const cbs[2], int forceReverse, Shot out[2], const std::string& name) {
    for (int e = 0; e < 2; ++e) out[e] = splashEye(g, cbs[e], forceReverse, name + (e ? "_right" : "_left"));
    introCurveTick(g.ctx.Get(), false);
}
// Frames until both eyes' pairs have been read back and armed (the module reads a copy a few frame edges after the first draw of a pair).
bool learnSplash(Gpu& g, ID3D11Buffer* const cbs[2]) {
    for (int f = 0; f < 12; ++f) {
        Shot s[2];
        splashFrame(g, cbs, -1, s, "learn");
        if (s[0].armed && s[1].armed) return true;
    }
    return false;
}

void caseR3andR4Splash(Gpu& g) {
    std::printf("R3 the splash: production introCurveOnComposite on the game's own constants, then the strip with introCurveReverseU(): left stays left, up stays up\n");
    std::printf("R4 (splash) the same draws with the flag forced the other way come out mirrored; the capture with its x column negated is drawn upright with u along x\n");
    // The binding shadow, as the game's draw leaves it: the intro composite's vertex shader, the picture at PS slot 0.
    detail::g_bindingSlots[static_cast<size_t>(BindSlot::Vs)].hash = kIntroCompositeVsHash;
    detail::g_bindingSlots[static_cast<size_t>(BindSlot::PsSrv0)].ptr = g.srv.Get();

    setCurvature("0.3");
    ComPtr<ID3D11Buffer> cbs[2] = {makeCb(g, kSplashA), makeCb(g, kSplashB)};
    ID3D11Buffer* const raw[2] = {cbs[0].Get(), cbs[1].Get()};
    check(learnSplash(g, raw), "R3.learned", "the splash module did not arm both eyes' constants within twelve frames");
    for (const char* curvature : {"0.05", "0.3", "0.6"}) {
        setCurvature(curvature);
        Shot prod[2], other[2];
        splashFrame(g, raw, -1, prod, fmt("R3_%s", curvature));
        splashFrame(g, raw, prod[0].reverse ? 0 : 1, other, fmt("R4_splash_forced_%s", curvature));
        for (int e = 0; e < 2; ++e) {
            g_note = fmt("curvature %s, %s eye", curvature, e ? "right" : "left");
            check(prod[e].armed && prod[e].reverse, "R3.armed", fmt("armed %d, reverseU %d: the splash's placement has its +x running left, so u runs against x", prod[e].armed, prod[e].reverse));
            check(prod[e].drew, "R3.drew", "panelCurveSurfaceDraw did not draw the surface strip");
            check(prod[e].q.drawn, "R3.drawn", describe(prod[e].q));
            check(upright(prod[e].q), "R3.upright", "the splash's picture is not upright: " + describe(prod[e].q));
            check(other[e].drew && other[e].q.drawn, "R4.splash-control-drawn", "the control draw (reverseU forced the other way) drew nothing: " + describe(other[e].q));
            check(mirrored(other[e].q), "R4.splash-control-mirrored", "reverseU forced the other way is not the picture mirrored left to right: " + describe(other[e].q));
            check(!upright(other[e].q), "R4.splash-control-fails", "the upright check passed a picture drawn with the flag forced the wrong way: it cannot fail");
        }
    }
    g_note.clear();

    // R4: the capture with its x column negated (cb2[1] = minus the game's): a placement whose +x runs to the viewer's RIGHT. The production code reads it
    // as running right and draws u along x: left still stays left.
    float negA[20], negB[20];
    std::memcpy(negA, kSplashA, sizeof(negA));
    std::memcpy(negB, kSplashB, sizeof(negB));
    for (int i = 4; i < 8; ++i) {
        negA[i] = -negA[i];
        negB[i] = -negB[i];
    }
    ComPtr<ID3D11Buffer> neg[2] = {makeCb(g, negA), makeCb(g, negB)};
    ID3D11Buffer* const rawNeg[2] = {neg[0].Get(), neg[1].Get()};
    setCurvature("0.3");
    check(learnSplash(g, rawNeg), "R4.splash-right-running-learned", "the splash module did not arm the right-running constants within twelve frames");
    Shot prod[2], other[2];
    splashFrame(g, rawNeg, -1, prod, "R4_splash_right_running");
    splashFrame(g, rawNeg, prod[0].reverse ? 0 : 1, other, "R4_splash_right_running_forced");
    for (int e = 0; e < 2; ++e) {
        g_note = fmt("x column negated, %s eye", e ? "right" : "left");
        check(prod[e].armed && !prod[e].reverse, "R4.splash-right-running-flag", fmt("armed %d, reverseU %d: a placement whose +x runs right must run u along x", prod[e].armed, prod[e].reverse));
        check(prod[e].drew && upright(prod[e].q), "R4.splash-right-running-upright", "left does not stay left when the placement's +x runs right: " + describe(prod[e].q));
        check(other[e].drew && mirrored(other[e].q), "R4.splash-right-running-control", "the flag forced the other way is not mirrored for a right-running placement: " + describe(other[e].q));
    }
    g_note.clear();
}

}  // namespace

int main(int argc, char** argv) {
    bool self = false, dry = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test")) self = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) g_dumpDir = argv[++i];
        else if (argv[i][0] != '-') continue;   // the repo root build.bat passes, ignored
        else {
            std::fputs("usage: surface_strip_render_test --self-test [<repo root, ignored>] [--dump <dir>] | --dry-run\n", stderr);
            return 2;
        }
    }
    if (dry) {
        std::puts("surface_strip_render_test: --dry-run: nothing run, nothing written");
        return 0;
    }
    if (!self) {
        std::fputs("usage: surface_strip_render_test --self-test [<repo root, ignored>] [--dump <dir>] | --dry-run\n", stderr);
        return 2;
    }
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
    Gpu gpu = makeGpu();
    check(gpu.ok, "R0.gpu", "a WARP device, the shaders and the target are available");
    if (gpu.ok) {
        check(edvr::reportSystemD3D11Only("surface_strip_render_test"), "R0.system-d3d11", "the process runs on System32's d3d11.dll only (no EDVR proxy beside the rig)");
        bindPipeline(gpu);
        caseR1(gpu);
        caseR2andR4Movie(gpu);
        caseR3andR4Splash(gpu);
    }
    panelCurveShutdown();
    gpu = Gpu{};
    if (g_failed) {
        std::printf("FAIL: surface strip render: %u of %u checks failed, %zu distinct\n", g_failed, g_checks, g_failCount.size());
        return 1;
    }
    std::printf("PASS: %u surface strip render checks (4 cases: the on-foot pin, the movie, the splash and the controls, rendered on WARP)\n", g_checks);
    return 0;
}
