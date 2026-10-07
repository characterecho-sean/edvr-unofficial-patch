// fix.ui_quality's supercruise bars in the HDR layer (src/d3d11/supercruise_bars*.h, supercruise_bars.cpp; docs/ui-layer-2026-09-23.md,
// "2026-10-07: orbit lines, supercruise bars and space dust in the layer"). The build's own bytecode of the strip geometry shader
// (kSupercruiseBarsGsBytecode: the build compiles supercruise_bars_shader.h's HLSL, and this rig checks the bytes are that text's) and the
// production binding, constants and rasterizer states run on WARP in front of the game's own shader pair for the draw -- vs A47A3315FFF5E2E4 /
// ps 869FFF43E875906E, whose HLSL stand-ins here disassemble to the instruction lists of the game's own (fixtures/*.asm, from the Frontier
// install's 2026-10-06 dump), so the vertex stage, the constants and the pixel stage the strip is fed through are the game's:
//
//   THE TENT. A segment drawn at 0, 30, 45 and 89.9 degrees, at half-widths 2.5 (125% of a 2016 wide render), 2.0 (100%) and 1.0: every pixel
//   within 0.02 of the analytic tent (peak on the line, 0 at the half-width, across it), nothing lit past the half-width or the ends, the
//   profile sampled finely (ten sub-pixel shifts) as wide at half maximum as the half-width and as large in area as peak x half-width -- the
//   weight of the one render pixel the game's line had, which is what the width is made from -- and the colour channels unchanged across it.
//   (A unit-peak tent is as wide as its half-width at half maximum; its base is twice that.)
//   THE CUT. A segment crossing the camera plane (clip w from +1 to -1) draws exactly the part in front, to the cut at w = 0.01; both ends behind
//   the plane, both nearer than the cut, a segment of no length and one of a thousandth of a pixel draw nothing; one end exactly at the cut draws whole.
//   THE VIEWPORT. 160x120, 96x200 and 256x64: the tent is the same number of pixels across whichever way the viewport is wider.
//   THE ATTRIBUTES. Perspective-correct along the segment (two ends at different w): the colour and alpha on the centre line are the hardware
//   line's; seventy segments in one draw each carry the same tent.
//   THE BINDING (the production class, on a real context): in and out for one draw with the game's geometry stage, constant slot and rasterizer
//   state exactly as they were and no reference leaked or kept; a geometry shader of the game's own refused with nothing touched; the game's cull
//   mode (back, front, none, either winding) and scissor enable: the strip is drawn the same through each (it is triangles, which a cull mode made for
//   lines would otherwise take); the restore that fails twice, the settle at the boundary and the state the game rebound; the geometry stage empty
//   after the issue and a plain issue after it a hardware line again.
//   THE CONSTANTS and THE STATES: made once per device, rewritten only when a number changes, refused for a number that is not finite or positive.
//   THE MUTANTS, each run through the same cases and each caught by them: the cut removed, the viewport's two axes swapped, the perpendicular made
//   of the wrong components, the alpha edge removed (a box, not a tent), the half-width ignored; and the binding that keeps the game's own rasterizer
//   state (its cull mode kills the strip) and the one whose restore does nothing (the geometry stage stays bound).
//   --wiring scans the draw hook's source (vscreen, ui_layer, object_probe, supercruise_bars, the shader tables, build.bat) with controls that
//   edit a copy and must trip the pin.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../src/common/log.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/supercruise_bars_binding.h"
#include "../../src/d3d11/supercruise_bars_shader.h"
#include "../../src/d3d11/supercruise_lines.h"
#include "temporal_shader_bytecode.h"

using Microsoft::WRL::ComPtr;
namespace sb = edvr::supercruise_bars;

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
    const HRESULT h = D3DCompile(source.data(), source.size(), "supercruise bars rig", nullptr, nullptr, "main", profile, 0, 0, &code, &error);
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

static std::vector<float> readBuffer(ID3D11Buffer* source) {
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
    std::vector<float> v(bd.ByteWidth / 4);
    std::memcpy(v.data(), m.pData, bd.ByteWidth);
    g_ctx->Unmap(stage.Get(), 0);
    return v;
}

// ---------------------------------------------------------------- the game's pair, as HLSL that disassembles to the game's own
// vs A47A3315FFF5E2E4: NORMAL x (5, 5, 5, 1) is the colour, x and y and w are POSITION against rows 4, 5 and 7 of cb0, z is 0.
// ps 869FFF43E875906E: the colour times cb1[90].y, the alpha from the vertex.
static const char* kGameVs = R"HLSL(
cbuffer C0 : register(b0) { float4 cb0[8]; };
struct VIn { float4 p : POSITION; float4 n : NORMAL; };
struct VOut { float4 colour : TEXCOORD1; float4 pos : SV_Position; };
VOut main(VIn i) {
    VOut o;
    o.colour = i.n * float4(5, 5, 5, 1);
    o.pos = float4(dot(cb0[4], i.p), dot(cb0[5], i.p), 0, dot(cb0[7], i.p));
    return o;
}
)HLSL";
static const char* kGamePs = R"HLSL(
cbuffer C1 : register(b1) { float4 cb1[91]; };
float4 main(float4 colour : TEXCOORD1) : SV_Target { return float4(colour.xyz * cb1[90].y, colour.w); }
)HLSL";

// The instruction lines of a disassembly: what the compiler emitted, without the header comments (a disassembler's banner differs between
// machines; the instructions are the shader).
static std::string instructionsOf(const std::string& text) {
    std::string out, line;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && line.compare(0, 2, "//") != 0) out += line + "\n";
            line.clear();
        } else {
            line += text[i];
        }
    }
    return out;
}
static std::string disassemble(ID3DBlob* code) {
    ComPtr<ID3DBlob> text;
    ck(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &text), "D3DDisassemble");
    return std::string(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize());
}

static ComPtr<ID3DBlob> g_vsCode, g_psCode;
static ComPtr<ID3D11VertexShader> g_vs;
static ComPtr<ID3D11PixelShader> g_ps;
static ComPtr<ID3D11InputLayout> g_layout;
static ComPtr<ID3D11Buffer> g_vsCb, g_psCb;

static void setupGamePair() {
    g_vsCode = compileHlsl(kGameVs, "vs_5_0");
    g_psCode = compileHlsl(kGamePs, "ps_5_0");
    ck(g_dev->CreateVertexShader(g_vsCode->GetBufferPointer(), g_vsCode->GetBufferSize(), nullptr, &g_vs), "CreateVertexShader");
    ck(g_dev->CreatePixelShader(g_psCode->GetBufferPointer(), g_psCode->GetBufferSize(), nullptr, &g_ps), "CreatePixelShader");
    const D3D11_INPUT_ELEMENT_DESC elements[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ck(g_dev->CreateInputLayout(elements, 2, g_vsCode->GetBufferPointer(), g_vsCode->GetBufferSize(), &g_layout), "CreateInputLayout");
    // cb0 rows 4, 5, 7: x' = P.x, y' = P.y, w' = P.w -- POSITION is the clip-space position the cases ask for; z is the shader's 0.
    std::vector<float> cb0(8 * 4, 0.0f);
    cb0[4 * 4 + 0] = 1.0f;
    cb0[5 * 4 + 1] = 1.0f;
    cb0[7 * 4 + 3] = 1.0f;
    g_vsCb = makeBuffer(UINT(sizeof(float) * cb0.size()), D3D11_BIND_CONSTANT_BUFFER, cb0.data());
    std::vector<float> cb1(91 * 4, 1.0f);  // cb1[90].y = 1: the colour passes through
    g_psCb = makeBuffer(UINT(sizeof(float) * cb1.size()), D3D11_BIND_CONSTANT_BUFFER, cb1.data());
}

// ---------------------------------------------------------------- one drawn frame
struct Seg {
    float p0[4], p1[4];  // clip-space x, y, (unused), w
    float c0[4], c1[4];  // the colour (rgb) and alpha the vertex shader is fed after its x5; the 1/5 is applied here
};
struct Frame {
    UINT w = 0, h = 0;
    std::vector<float> rgba;
    const float* at(UINT x, UINT y) const { return rgba.data() + (size_t(y) * w + x) * 4; }
    double lit(double threshold = 1e-4) const {
        double n = 0;
        for (size_t i = 3; i < rgba.size(); i += 4) n += rgba[i] > threshold ? 1 : 0;
        return n;
    }
    double sumAlpha() const {
        double s = 0;
        for (size_t i = 3; i < rgba.size(); i += 4) s += rgba[i];
        return s;
    }
};

// A colour target and the one place it is read back from.
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

static std::vector<float> vertexData(const std::vector<Seg>& segs) {
    std::vector<float> verts;
    for (const Seg& s : segs) {
        for (int e = 0; e < 2; ++e) {
            const float* p = e ? s.p1 : s.p0;
            const float* c = e ? s.c1 : s.c0;
            verts.insert(verts.end(), {p[0], p[1], p[2], p[3], c[0] / 5.0f, c[1] / 5.0f, c[2] / 5.0f, c[3]});
        }
    }
    return verts;
}

// The game's state for a line list draw into `t`: the target, the viewport, the pair, the vertex data, the topology. What the cases vary
// -- the rasterizer state the game had and a scissor -- is bound after it by the caller.
static void bindGameDraw(Target& t, ID3D11Buffer* vb) {
    g_ctx->ClearState();
    g_ctx->OMSetRenderTargets(1, t.rtv.GetAddressOf(), nullptr);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, float(t.w), float(t.h), 0.0f, 1.0f};
    g_ctx->RSSetViewports(1, &vp);
    const UINT stride = 32, offset = 0;
    g_ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g_ctx->IASetInputLayout(g_layout.Get());
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, g_vsCb.GetAddressOf());
    g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
    g_ctx->PSSetConstantBuffers(1, 1, g_psCb.GetAddressOf());
}

// What a draw does with the game's rasterizer state and the binding: the knobs of the cases and of the mutants.
struct DrawOpts {
    ID3D11RasterizerState* gameRs = nullptr;    // the state the game had bound (null: the default)
    bool scissor = false;                       // a scissor rectangle bound
    D3D11_RECT scissorRect{0, 0, 0, 0};
    bool keepGameRaster = false;                // MUTANT: a binding that keeps the game's own rasterizer state
    bool skipRestore = false;                   // MUTANT: a restore that does nothing
};
static void noopSetGs(ID3D11DeviceContext*, ID3D11GeometryShader*, ID3D11ClassInstance* const*, uint32_t) {}

static sb::Constants g_constants;
static sb::RasterStates g_raster;

// One frame of `segs` through the production binding and `gs` (null gs: the game's plain line list).
static Frame render(ID3D11GeometryShader* gs, const std::vector<Seg>& segs, UINT w, UINT h, float halfWidth, const DrawOpts& o = DrawOpts{}) {
    const auto verts = vertexData(segs);
    auto vb = makeBuffer(UINT(verts.size() * sizeof(float)), D3D11_BIND_VERTEX_BUFFER, verts.data());
    Target t(w, h);
    bindGameDraw(t, vb.Get());
    t.clear();
    if (o.gameRs) g_ctx->RSSetState(o.gameRs);
    if (o.scissor) g_ctx->RSSetScissorRects(1, &o.scissorRect);
    sb::Binding binding;
    bool bound = false;
    if (gs) {
        ID3D11Buffer* cb = g_constants.get(g_ctx.Get(), sb::StripParams{float(w), float(h), halfWidth, sb::kClipW});
        check(cb != nullptr, "the strip constants are made");
        ComPtr<ID3D11RasterizerState> game;
        if (o.keepGameRaster) g_ctx->RSGetState(&game);
        bound = binding.begin(g_ctx.Get(), gs, cb, [&](bool scissor) -> ID3D11RasterizerState* {
                    return o.keepGameRaster ? game.Get() : g_raster.get(g_ctx.Get(), scissor);
                }) == sb::Binding::Begin::kBound;
    }
    g_ctx->Draw(UINT(verts.size() / 8), 0);
    if (bound) binding.finish(g_ctx.Get(), o.skipRestore ? noopSetGs : sb::directSetGs);
    return t.read();
}

// ---------------------------------------------------------------- the analytic tent
struct P2 {
    double x, y;
};
static P2 toPx(double ndcX, double ndcY, UINT w, UINT h) { return {(ndcX * 0.5 + 0.5) * w, (0.5 - ndcY * 0.5) * h}; }

// The segment in pixels after the cut at w = kClipW (clip space, double): false when none of it is in front.
static bool analyticSegment(const Seg& s, UINT w, UINT h, P2* a, P2* b) {
    double x0 = s.p0[0], y0 = s.p0[1], w0 = s.p0[3], x1 = s.p1[0], y1 = s.p1[1], w1 = s.p1[3];
    const double cut = sb::kClipW;
    const bool in0 = w0 >= cut, in1 = w1 >= cut;
    if (!in0 && !in1) return false;
    if (!in0) {
        const double t = (cut - w0) / (w1 - w0);
        x0 += (x1 - x0) * t;
        y0 += (y1 - y0) * t;
        w0 = cut;
    } else if (!in1) {
        const double t = (cut - w1) / (w0 - w1);
        x1 += (x0 - x1) * t;
        y1 += (y0 - y1) * t;
        w1 = cut;
    }
    *a = toPx(x0 / w0, y0 / w0, w, h);
    *b = toPx(x1 / w1, y1 / w1, w, h);
    return true;
}

// Where a pixel centre lies against a segment: along it (0 at the start, the length at the end) and the signed distance across it.
struct Rel {
    double along, across, length;
};
static Rel relTo(const P2& a, const P2& b, double px, double py) {
    const double dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
    const double ux = dx / len, uy = dy / len;
    return {(px - a.x) * ux + (py - a.y) * uy, -(px - a.x) * uy + (py - a.y) * ux, len};
}
static double tent(double across, double half) { return std::max(0.0, 1.0 - std::fabs(across) / half); }

// A segment through the viewport's middle at `deg` degrees (pixel space, y down), `len` pixels long, `shift` pixels along its own normal.
static Seg segAt(UINT w, UINT h, double deg, double len, double shift, const float rgba[4]) {
    const double rad = deg * 3.14159265358979323846 / 180.0, ux = std::cos(rad), uy = std::sin(rad);
    const double cx = w * 0.5 - uy * shift, cy = h * 0.5 + ux * shift;
    const P2 a{cx - ux * len * 0.5, cy - uy * len * 0.5}, b{cx + ux * len * 0.5, cy + uy * len * 0.5};
    Seg s{};
    s.p0[0] = float(a.x / w * 2.0 - 1.0);
    s.p0[1] = float(1.0 - a.y / h * 2.0);
    s.p0[3] = 1.0f;
    s.p1[0] = float(b.x / w * 2.0 - 1.0);
    s.p1[1] = float(1.0 - b.y / h * 2.0);
    s.p1[3] = 1.0f;
    std::memcpy(s.c0, rgba, sizeof(s.c0));
    std::memcpy(s.c1, rgba, sizeof(s.c1));
    return s;
}
static Seg segBetween(const float p0[4], const float p1[4], const float c0[4], const float c1[4]) {
    Seg s{};
    std::memcpy(s.p0, p0, sizeof(s.p0));
    std::memcpy(s.p1, p1, sizeof(s.p1));
    std::memcpy(s.c0, c0, sizeof(s.c0));
    std::memcpy(s.c1, c1, sizeof(s.c1));
    return s;
}

// ---------------------------------------------------------------- the tent, measured
struct TentMeasure {
    double maxError = 0;    // the worst pixel against the analytic tent (and any lit pixel past the half-width or the ends)
    double fwhm = -1;       // the width at half maximum, from the finely sampled profile
    double area = 0;        // the profile's area, per unit of length
    double widest = 0;      // the farthest lit pixel from the line
    double rgbError = 0;    // the worst colour channel against the input's
};
static TentMeasure measureTent(ID3D11GeometryShader* gs, UINT w, UINT h, double deg, double half, const float rgba[4]) {
    TentMeasure m;
    std::vector<std::pair<double, double>> scatter;
    const double len = std::min(w, h) * 0.8;
    double areaSum = 0;
    const int kShifts = 10;
    for (int k = 0; k < kShifts; ++k) {
        const Seg s = segAt(w, h, deg, len, k * 0.1, rgba);
        const Frame f = render(gs, {s}, w, h, float(half));
        P2 a, b;
        if (!analyticSegment(s, w, h, &a, &b)) continue;
        double band = 0;
        for (UINT y = 0; y < h; ++y) {
            for (UINT x = 0; x < w; ++x) {
                const Rel r = relTo(a, b, x + 0.5, y + 0.5);
                const float* px = f.at(x, y);
                if (r.along > r.length * 0.25 && r.along < r.length * 0.75) {
                    m.maxError = std::max(m.maxError, std::fabs(px[3] - rgba[3] * tent(r.across, half)));
                    if (px[3] > 1e-4) {
                        m.widest = std::max(m.widest, std::fabs(r.across));
                        for (int c = 0; c < 3; ++c) m.rgbError = std::max(m.rgbError, std::fabs(px[c] - double(rgba[c])));
                    }
                    scatter.push_back({r.across, px[3]});
                    band += px[3];
                }
                // nothing lit past the half-width, nor past the ends (the rasterizer snaps a vertex to 1/256 of a pixel: 0.01 allows it)
                if ((std::fabs(r.across) > half + 0.01 || r.along < -0.01 || r.along > r.length + 0.01) && px[3] > 1e-4)
                    m.maxError = std::max(m.maxError, double(px[3]));
            }
        }
        areaSum += band / (0.5 * len);
    }
    m.area = areaSum / kShifts;
    std::sort(scatter.begin(), scatter.end());
    const double halfMax = rgba[3] * 0.5;
    double left = 0, right = 0;
    bool haveLeft = false, haveRight = false;
    for (size_t i = 1; i < scatter.size(); ++i) {
        const auto& p = scatter[i - 1];
        const auto& q = scatter[i];
        if (!haveLeft && p.first < 0 && p.second < halfMax && q.second >= halfMax) {
            left = p.first + (q.first - p.first) * (halfMax - p.second) / (q.second - p.second);
            haveLeft = true;
        }
        if (!haveRight && q.first > 0 && p.second >= halfMax && q.second < halfMax) {
            right = p.first + (q.first - p.first) * (p.second - halfMax) / (p.second - q.second);
            haveRight = true;
        }
    }
    if (haveLeft && haveRight) m.fwhm = right - left;
    return m;
}

// A frame against the analytic strip of one segment, every pixel (a segment with a far cut end has its along range in the thousands).
static double fieldError(const Frame& f, const Seg& s, double half, double* litOutside = nullptr) {
    P2 a, b;
    double worst = 0, outside = 0;
    const bool any = analyticSegment(s, f.w, f.h, &a, &b);
    for (UINT y = 0; y < f.h; ++y) {
        for (UINT x = 0; x < f.w; ++x) {
            double expect = 0;
            if (any) {
                const Rel r = relTo(a, b, x + 0.5, y + 0.5);
                // The strip's two flat ends are a pixel-centre rule's to decide: a pixel centre within a pixel of an end is not compared (the
                // rasterizer snaps a vertex to 1/256 of a pixel); everything farther in is the tent, everything clearly outside is zero.
                const bool interior = r.along > 1.0 && r.along < r.length - 1.0;
                const bool exterior = r.along < -1.0 || r.along > r.length + 1.0 || std::fabs(r.across) > half + 0.01;
                if (interior && !exterior) expect = double(s.c0[3]) * tent(r.across, half);
                else if (!exterior) continue;
            }
            const double got = f.at(x, y)[3];
            worst = std::max(worst, std::fabs(got - expect));
            if (expect == 0.0 && got > 1e-4) outside += 1;
        }
    }
    if (litOutside) *litOutside = outside;
    return worst;
}

// ---------------------------------------------------------------- the geometry cases, run on the production shader and on each mutant
static void geometryCases(ID3D11GeometryShader* gs, Fails& f) {
    const float rgba[4] = {0.9f, 0.6f, 0.2f, 0.8f};
    for (double half : {2.5, 2.0, 1.0}) {
        for (double deg : {0.0, 30.0, 45.0, 89.9}) {
            const TentMeasure m = measureTent(gs, 160, 120, deg, half, rgba);
            f.expect(m.maxError < 0.02, "the tent: every pixel is within 0.02 of the analytic tent, none lit past the half-width or the ends");
            f.expect(std::fabs(m.fwhm - half) < 0.1, "the tent's width at half maximum is its half-width");
            f.expect(std::fabs(m.area / (rgba[3] * half) - 1.0) < 0.02, "the tent's area is peak x half-width: the weight of a render pixel at the layer's density");
            f.expect(m.widest <= half + 0.011 && m.widest > half - 0.6, "its base reaches the half-width to each side and no further");
            f.expect(m.rgbError < 1e-4, "the colour channels are the vertices' across the whole strip");
        }
    }
    // The viewport: 96x200 and 256x64 -- the tent is the same number of pixels across whichever axis the viewport is wider on.
    for (const auto& vp : {std::pair<UINT, UINT>{96, 200}, std::pair<UINT, UINT>{256, 64}}) {
        for (double deg : {0.0, 45.0, 90.0}) {
            const TentMeasure m = measureTent(gs, vp.first, vp.second, deg, 2.5, rgba);
            char what[200];
            std::snprintf(what, sizeof(what),
                          "a non-square viewport: the tent is 2.5 pixels wide at half maximum at 0, 45 and 90 degrees (%ux%u at %.0f deg: error %.4f, fwhm %.3f, area %.3f)",
                          vp.first, vp.second, deg, m.maxError, m.fwhm, m.area / (rgba[3] * 2.5));
            // (the area is a pixel sum over a band of the segment: on the shorter segments of these viewports its edge rows are worth a few percent)
            f.expect(m.maxError < 0.02 && std::fabs(m.fwhm - 2.5) < 0.1 && std::fabs(m.area / (rgba[3] * 2.5) - 1.0) < 0.04, what);
        }
    }

    // THE CUT.
    const float c4[4] = {1.0f, 0.5f, 0.25f, 0.8f};
    {
        const float a[4] = {0.2f, 0.1f, 0, 1.0f}, b[4] = {-0.5f, 0.3f, 0, -1.0f};
        for (int order = 0; order < 2; ++order) {
            const Seg s = order ? segBetween(b, a, c4, c4) : segBetween(a, b, c4, c4);
            double outside = 0;
            const Frame fr = render(gs, {s}, 160, 120, 2.5f);
            const double err = fieldError(fr, s, 2.5, &outside);
            char what[240];
            std::snprintf(what, sizeof(what),
                          "a segment crossing the camera plane draws exactly the part in front, to the cut: the analytic tent, and not a pixel elsewhere (order %d: error %.4f, %.0f lit, %.0f lit outside the analytic strip)",
                          order, err, fr.lit(), outside);
            f.expect(err < 0.03 && fr.lit() > 100, what);
        }
    }
    {
        const float a[4] = {0.3f, 0.2f, 0, -1.0f}, b[4] = {-0.4f, -0.1f, 0, -2.0f};
        f.expect(render(gs, {segBetween(a, b, c4, c4)}, 160, 120, 2.5f).lit() == 0, "both ends behind the camera plane draw nothing");
        const float n0[4] = {0.3f, 0.2f, 0, 0.005f}, n1[4] = {-0.4f, -0.1f, 0, 0.002f};
        f.expect(render(gs, {segBetween(n0, n1, c4, c4)}, 160, 120, 2.5f).lit() == 0, "both ends in front of the plane but nearer than the cut draw nothing");
        const float z[4] = {0.1f, 0.1f, 0, 1.0f};
        f.expect(render(gs, {segBetween(z, z, c4, c4)}, 160, 120, 2.5f).lit() == 0, "a segment of no length draws nothing");
        const float t1[4] = {0.1f + 0.0005f / 80.0f, 0.1f, 0, 1.0f};   // 0.0005 px at 160 wide
        f.expect(render(gs, {segBetween(z, t1, c4, c4)}, 160, 120, 2.5f).lit() == 0, "a segment of a two-thousandth of a pixel draws nothing");
        const float e0[4] = {-0.5f, 0.0f, 0, sb::kClipW * 100.0f}, e1[4] = {0.5f, 0.0f, 0, sb::kClipW};  // one end exactly at the cut: clip x scaled with w
        const Seg edge = segBetween(e0, e1, c4, c4);
        const Frame fr = render(gs, {edge}, 160, 120, 2.5f);
        f.expect(fieldError(fr, edge, 2.5) < 0.03 && fr.lit() > 50, "one end exactly at the cut is drawn whole (>= the cut, not >)");
    }

    // THE TENT UNDER PERSPECTIVE: a segment whose ends are at depths 1 and 3 (and, below, 1 and 100) has the exact tent across it; perspective
    // interpolation over a strip whose ends differ in w would pull the profile toward the nearer end (a quarter of its height at a ratio of two).
    for (float farDepth : {3.0f, 100.0f}) {   // ("far" is a macro in windows.h)
        const float a[4] = {-0.6f, 0.0f, 0, 1.0f}, b[4] = {0.6f * farDepth, 0.4f * farDepth, 0, farDepth};   // clip x and y scaled with w: NDC (0.6, 0.4)
        const Seg s = segBetween(a, b, c4, c4);
        const Frame fr = render(gs, {s}, 160, 120, 2.5f);
        char what[200];
        const double err = fieldError(fr, s, 2.5);
        std::snprintf(what, sizeof(what), "a segment with ends at depths 1 and %.0f has the exact tent across it, whatever the depth ratio (error %.4f)", static_cast<double>(farDepth), err);
        f.expect(err < 0.03 && fr.lit() > 100, what);
    }
    // THE ATTRIBUTES: perspective-correct along the segment, ends at different w.
    {
        const float a[4] = {-0.6f, 0.0f, 0, 1.0f}, b[4] = {0.6f, 0.4f, 0, 3.0f};
        const float ca[4] = {1.0f, 0.0f, 0.0f, 0.2f}, cb[4] = {0.0f, 0.0f, 1.0f, 0.8f};
        const Seg s = segBetween(a, b, ca, cb);
        const Frame fr = render(gs, {s}, 160, 120, 2.5f);
        P2 p, q;
        analyticSegment(s, 160, 120, &p, &q);
        double worstRgb = 0, worstAlpha = 0;
        unsigned sampled = 0;
        for (UINT y = 0; y < fr.h; ++y) {
            for (UINT x = 0; x < fr.w; ++x) {
                const Rel r = relTo(p, q, x + 0.5, y + 0.5);
                if (r.along < r.length * 0.1 || r.along > r.length * 0.9 || std::fabs(r.across) > 0.5) continue;
                const double u = r.along / r.length, w0 = 1.0, w1 = 3.0;
                const double wa = (1 - u) / w0, wb = u / w1, norm = wa + wb;
                const double red = (wa * ca[0] + wb * cb[0]) / norm, blue = (wa * ca[2] + wb * cb[2]) / norm, alpha = (wa * ca[3] + wb * cb[3]) / norm;
                const float* px = fr.at(x, y);
                worstRgb = std::max({worstRgb, std::fabs(px[0] - red), std::fabs(px[2] - blue)});
                worstAlpha = std::max(worstAlpha, std::fabs(px[3] / tent(r.across, 2.5) - alpha));
                ++sampled;
            }
        }
        f.expect(sampled > 40 && worstRgb < 0.02 && worstAlpha < 0.02, "along a segment with ends at different w the colour and alpha are the hardware line's (perspective-correct)");
    }
    // Seventy segments in one draw, each the same tent (the real draw is 140 vertices).
    {
        std::vector<Seg> bars;
        double expected = 0;
        for (int i = 0; i < 70; ++i) {
            const double x = 6.3 + i * 7.13;   // pixel columns at sub-pixel offsets, spaced wider than the tent's base (5 px): the rig's target replaces, it does not add
            const double top = 12 + (i % 7) * 5.0, bottom = top + 30 + (i % 5) * 8.0;
            Seg s{};
            const double w = 512.0, h = 192.0;
            s.p0[0] = float(x / w * 2 - 1);
            s.p0[1] = float(1 - top / h * 2);
            s.p0[3] = 1;
            s.p1[0] = float(x / w * 2 - 1 + 0.0001 * (i % 3));
            s.p1[1] = float(1 - bottom / h * 2);
            s.p1[3] = 1;
            const float c[4] = {0.8f, 0.7f, 0.1f, 0.5f};
            std::memcpy(s.c0, c, sizeof(c));
            std::memcpy(s.c1, c, sizeof(c));
            bars.push_back(s);
            expected += double(c[3]) * 2.5 * (bottom - top);
        }
        const Frame fr = render(gs, bars, 512, 192, 2.5f);
        char what[200];
        std::snprintf(what, sizeof(what), "seventy segments in one draw carry the weight of seventy tents (peak x half-width x length each; measured / expected %.4f)", fr.sumAlpha() / expected);
        f.expect(std::fabs(fr.sumAlpha() / expected - 1.0) < 0.03, what);
    }
}

// ---------------------------------------------------------------- the binding on a real context
static bool boundIs(ID3D11GeometryShader* gs, ID3D11Buffer* cb, ID3D11RasterizerState* rs) {
    ComPtr<ID3D11GeometryShader> g;
    ComPtr<ID3D11Buffer> b;
    ComPtr<ID3D11RasterizerState> r;
    g_ctx->GSGetShader(&g, nullptr, nullptr);
    g_ctx->GSGetConstantBuffers(sb::kGsSlot, 1, &b);
    g_ctx->RSGetState(&r);
    return g.Get() == gs && b.Get() == cb && r.Get() == rs;
}
static ULONG refsOf(IUnknown* u) {
    u->AddRef();
    return u->Release();
}
static void failingSetGs(ID3D11DeviceContext*, ID3D11GeometryShader*, ID3D11ClassInstance* const*, uint32_t) {
    RaiseException(0xe0421314, 0, 0, nullptr);
}

// A pass-through geometry shader, for "the game bound one of its own": the strip shader's signature in, a line strip of the two ends out.
static const std::vector<char>& sb_testPassThroughBytes() {
    static std::vector<char> bytes;
    if (bytes.empty()) {
        const char* text = "struct V { float4 c : TEXCOORD1; float4 p : SV_Position; };\n"
                           "[maxvertexcount(2)] void main(line V v[2], inout LineStream<V> s) { s.Append(v[0]); s.Append(v[1]); }\n";
        const auto code = compileHlsl(text, "gs_5_0");
        bytes.assign(static_cast<const char*>(code->GetBufferPointer()), static_cast<const char*>(code->GetBufferPointer()) + code->GetBufferSize());
    }
    return bytes;
}

static ComPtr<ID3D11RasterizerState> makeRaster(D3D11_CULL_MODE cull, bool ccw, bool scissor) {
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = cull;
    rd.FrontCounterClockwise = ccw ? TRUE : FALSE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = scissor ? TRUE : FALSE;
    ComPtr<ID3D11RasterizerState> s;
    ck(g_dev->CreateRasterizerState(&rd, &s), "CreateRasterizerState");
    return s;
}

static void bindingCases(ID3D11GeometryShader* gs, Fails& f, bool leakChecks) {
    const std::vector<float> sentinelData{7, 8, 9, 10};
    auto sentinel = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, sentinelData.data());
    auto constants = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, sentinelData.data());
    auto gameRs = makeRaster(D3D11_CULL_BACK, false, false);
    auto ours = makeRaster(D3D11_CULL_NONE, false, false);
    auto pick = [&](bool) -> ID3D11RasterizerState* { return ours.Get(); };
    auto gameState = [&] {
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->GSSetConstantBuffers(sb::kGsSlot, 1, sentinel.GetAddressOf());
        g_ctx->RSSetState(gameRs.Get());
    };
    g_ctx->ClearState();
    const ULONG gsRefs = refsOf(gs), cbRefs = refsOf(sentinel.Get()), constRefs = refsOf(constants.Get()), gameRsRefs = refsOf(gameRs.Get()), oursRefs = refsOf(ours.Get());
    auto quiet = [&] {
        return refsOf(gs) == gsRefs && refsOf(sentinel.Get()) == cbRefs && refsOf(constants.Get()) == constRefs && refsOf(gameRs.Get()) == gameRsRefs &&
               refsOf(ours.Get()) == oursRefs;
    };
    {   // the ordinary draw: in, and out again
        gameState();
        sb::Binding b;
        f.expect(!b.needsRestore(), "a fresh binding owes nothing");
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get(), pick) == sb::Binding::Begin::kBound, "begin binds the strip shader, its constants and the private state");
        f.expect(b.needsRestore() && boundIs(gs, constants.Get(), ours.Get()), "all three are bound, the game's owed back");
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get(), pick) == sb::Binding::Begin::kRefused, "a second begin while one is open is refused");
        f.expect(b.restore(g_ctx.Get()) && !b.needsRestore(), "restore puts the game's back");
        f.expect(boundIs(nullptr, sentinel.Get(), gameRs.Get()), "the game's geometry stage (none), constant slot and rasterizer state are exactly as they were");
        if (leakChecks) f.expect(quiet(), "no reference leaked or kept");
        f.expect(b.restore(g_ctx.Get()), "restore with nothing owed is a no-op");
    }
    {   // the game's default rasterizer state (none bound) comes back as none
        ID3D11Buffer* noBuffer = nullptr;
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->GSSetConstantBuffers(sb::kGsSlot, 1, &noBuffer);
        g_ctx->RSSetState(nullptr);
        sb::Binding b;
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get(), pick) == sb::Binding::Begin::kBound, "begin over the default rasterizer state");
        f.expect(b.restore(g_ctx.Get()) && boundIs(nullptr, nullptr, nullptr), "...gives the default (none) back, and the unbound constant slot with it");
    }
    {   // the game's scissor enable picks the private state's
        bool asked = false, askedScissor = false;
        auto scissorRs = makeRaster(D3D11_CULL_BACK, false, true);
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->RSSetState(scissorRs.Get());
        sb::Binding b;
        const auto begun = b.begin(g_ctx.Get(), gs, constants.Get(), [&](bool scissor) -> ID3D11RasterizerState* {
            asked = true;
            askedScissor = scissor;
            return ours.Get();
        });
        f.expect(begun == sb::Binding::Begin::kBound && asked && askedScissor, "a game state with the scissor test on asks for the private state with it on");
        b.restore(g_ctx.Get());
    }
    {   // a game geometry shader of its own is not ours to replace: nothing is touched
        ComPtr<ID3D11GeometryShader> theirs;
        ck(g_dev->CreateGeometryShader(sb_testPassThroughBytes().data(), sb_testPassThroughBytes().size(), nullptr, &theirs), "the game's own geometry shader");
        gameState();
        g_ctx->GSSetShader(theirs.Get(), nullptr, 0);
        sb::Binding b;
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get(), pick) == sb::Binding::Begin::kGameHasGs, "a game geometry shader bound: refused, kGameHasGs");
        f.expect(!b.needsRestore() && boundIs(theirs.Get(), sentinel.Get(), gameRs.Get()), "...and nothing of the game's state moved");
        g_ctx->GSSetShader(nullptr, nullptr, 0);
    }
    {   // no strip shader, no constants, no context, no private state: refused, nothing moved
        gameState();
        sb::Binding b;
        f.expect(b.begin(g_ctx.Get(), nullptr, constants.Get(), pick) == sb::Binding::Begin::kRefused &&
                     b.begin(g_ctx.Get(), gs, nullptr, pick) == sb::Binding::Begin::kRefused &&
                     b.begin(nullptr, gs, constants.Get(), pick) == sb::Binding::Begin::kRefused &&
                     b.begin(g_ctx.Get(), gs, constants.Get(), [](bool) -> ID3D11RasterizerState* { return nullptr; }) == sb::Binding::Begin::kRefused,
                 "no shader, no buffer, no context or no private state: refused");
        f.expect(!b.needsRestore() && boundIs(nullptr, sentinel.Get(), gameRs.Get()), "...and the game's state is where it was");
        if (leakChecks) f.expect(quiet(), "...with nothing leaked by the getters that ran");
    }
    {   // a restore that fails twice: the references are kept, the boundary settles it
        gameState();
        sb::Binding b;
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get(), pick) == sb::Binding::Begin::kBound, "settle scene: bound");
        const auto result = b.finish(g_ctx.Get(), failingSetGs);
        f.expect(result.retried && !result.restored && b.needsRestore(), "a restore that failed twice is retried once and still owed");
        {
            ComPtr<ID3D11GeometryShader> stuck;
            g_ctx->GSGetShader(&stuck, nullptr, nullptr);
            f.expect(stuck.Get() == gs, "...and EDVR's shader is still bound until it is settled (the slot and the state, whose setters worked, are the game's again)");
        }
        f.expect(b.begin(g_ctx.Get(), gs, constants.Get(), pick) == sb::Binding::Begin::kRefused, "no new draw begins while one is owed");
        f.expect(!b.settle(g_ctx.Get(), failingSetGs) && b.needsRestore(), "a settle whose setter faults changes nothing and is still owed");
        f.expect(b.settle(g_ctx.Get()) && !b.needsRestore(), "the next settle puts the game's back");
        f.expect(boundIs(nullptr, sentinel.Get(), gameRs.Get()), "settled: the game's geometry stage, constant slot and state are bound again");
        if (leakChecks) f.expect(quiet(), "settled: no reference kept");
        f.expect(b.settle(g_ctx.Get()), "a settled binding settles idempotently");
    }
    {   // the game rebound meanwhile: its own state stays
        gameState();
        sb::Binding b;
        b.begin(g_ctx.Get(), gs, constants.Get(), pick);
        b.finish(g_ctx.Get(), failingSetGs);
        auto theirs = makeBuffer(16, D3D11_BIND_CONSTANT_BUFFER, sentinelData.data());
        auto theirRs = makeRaster(D3D11_CULL_FRONT, true, false);
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->GSSetConstantBuffers(sb::kGsSlot, 1, theirs.GetAddressOf());
        g_ctx->RSSetState(theirRs.Get());
        f.expect(b.settle(g_ctx.Get()) && !b.needsRestore(), "settle with all three rebound by the game succeeds");
        f.expect(boundIs(nullptr, theirs.Get(), theirRs.Get()), "...and leaves the game's own newer state, not the saved one");
        gameState();
    }
    {   // a null context cannot settle
        gameState();
        sb::Binding b;
        b.begin(g_ctx.Get(), gs, constants.Get(), pick);
        b.finish(g_ctx.Get(), failingSetGs);
        f.expect(!b.settle(nullptr) && b.needsRestore(), "no context: the settle reports failure and keeps the state");
        b.settle(g_ctx.Get());
        if (leakChecks) f.expect(quiet(), "settled after the null context");
    }
    g_ctx->ClearState();
}

// The cull mode and the scissor of the game's own state: the strip is drawn the same through each, the way a line was.
static void rasterCases(ID3D11GeometryShader* gs, Fails& f, bool mutantKeepsGameRaster) {
    const float rgba[4] = {0.9f, 0.6f, 0.2f, 0.8f};
    const Seg s = segAt(160, 120, 30.0, 80.0, 0.0, rgba);
    const Frame baseline = render(gs, {s}, 160, 120, 2.5f);
    f.expect(baseline.lit() > 200 && fieldError(baseline, s, 2.5) < 0.02, "the baseline strip is the analytic tent");
    for (D3D11_CULL_MODE cull : {D3D11_CULL_NONE, D3D11_CULL_BACK, D3D11_CULL_FRONT}) {
        for (bool ccw : {false, true}) {
            auto state = makeRaster(cull, ccw, false);
            DrawOpts o;
            o.gameRs = state.Get();
            o.keepGameRaster = mutantKeepsGameRaster;
            const Frame fr = render(gs, {s}, 160, 120, 2.5f, o);
            double worst = 0;
            for (size_t i = 3; i < fr.rgba.size(); i += 4) worst = std::max(worst, std::fabs(double(fr.rgba[i]) - baseline.rgba[i]));
            f.expect(worst < 1e-6, "the strip is drawn the same whatever cull mode and winding the game's own state had (none, back, front; both windings)");
        }
    }
    {   // a scissor the game set: the layer remapped it, the strip honours it (the private state keeps the game's scissor enable)
        auto state = makeRaster(D3D11_CULL_BACK, false, true);
        DrawOpts o;
        o.gameRs = state.Get();
        o.scissor = true;
        o.scissorRect = {0, 0, 80, 120};
        o.keepGameRaster = mutantKeepsGameRaster;
        const Frame fr = render(gs, {s}, 160, 120, 2.5f, o);
        double leftLit = 0, rightLit = 0;
        for (UINT y = 0; y < fr.h; ++y)
            for (UINT x = 0; x < fr.w; ++x) (x < 80 ? leftLit : rightLit) += fr.at(x, y)[3] > 1e-4 ? 1 : 0;
        f.expect(leftLit > 50 && rightLit == 0, "a game state with the scissor test on: the strip is cut by the scissor rectangle, nothing past it");
    }
}

// The issue's end: the geometry stage is empty, and a plain issue after it is a hardware line again.
static void afterIssueCases(ID3D11GeometryShader* gs, Fails& f, bool skipRestore) {
    const float rgba[4] = {0.9f, 0.6f, 0.2f, 0.8f};
    const Seg s = segAt(160, 120, 90.0, 60.0, 0.5, rgba);
    const auto verts = vertexData({s});
    auto vb = makeBuffer(UINT(verts.size() * sizeof(float)), D3D11_BIND_VERTEX_BUFFER, verts.data());
    Target t(160, 120);
    bindGameDraw(t, vb.Get());
    t.clear();
    sb::Binding b;
    ID3D11Buffer* cb = g_constants.get(g_ctx.Get(), sb::StripParams{160, 120, 2.5f, sb::kClipW});
    const auto begun = b.begin(g_ctx.Get(), gs, cb, [&](bool scissor) { return g_raster.get(g_ctx.Get(), scissor); });
    f.expect(begun == sb::Binding::Begin::kBound, "the strip is bound for the issue");
    g_ctx->Draw(2, 0);
    b.finish(g_ctx.Get(), skipRestore ? noopSetGs : sb::directSetGs);
    ComPtr<ID3D11GeometryShader> after;
    g_ctx->GSGetShader(&after, nullptr, nullptr);
    f.expect(after == nullptr, "the geometry stage is empty after the issue");
    const Frame strip = t.read();
    t.clear();
    g_ctx->Draw(2, 0);
    const Frame plain = t.read();
    // A hardware line: one pixel across, the vertices' full alpha, 60 pixels long (+/- a few).
    f.expect(plain.lit() > 55 && plain.lit() < 66, "a plain issue after the strip's is a hardware line again: one pixel across, as long as the segment");
    double peak = 0;
    for (size_t i = 3; i < plain.rgba.size(); i += 4) peak = std::max(peak, double(plain.rgba[i]));
    f.expect(std::fabs(peak - rgba[3]) < 1e-5, "...with the vertices' own alpha (no tent: the pixel shader and the blend are the game's, untouched)");
    f.expect(strip.lit() > plain.lit() * 3, "...and the strip it replaced was the wide one");
}

// ---------------------------------------------------------------- the constants and the states
static void constantsCases() {
    sb::Constants c;
    const sb::StripParams p{5040.0f, 4873.0f, 2.5f, sb::kClipW};
    check(c.get(nullptr, p) == nullptr && c.get(g_ctx.Get(), sb::StripParams{0, 100, 2.5f, 0.01f}) == nullptr &&
              c.get(g_ctx.Get(), sb::StripParams{100, 0, 2.5f, 0.01f}) == nullptr && c.get(g_ctx.Get(), sb::StripParams{100, 100, 0.0f, 0.01f}) == nullptr &&
              c.get(g_ctx.Get(), sb::StripParams{100, 100, -1.0f, 0.01f}) == nullptr &&
              c.get(g_ctx.Get(), sb::StripParams{100, 100, std::nanf(""), 0.01f}) == nullptr &&
              c.get(g_ctx.Get(), sb::StripParams{std::nanf(""), 100, 2.5f, 0.01f}) == nullptr,
          "no context, or a number that is not finite and positive, gets no buffer");
    ID3D11Buffer* first = c.get(g_ctx.Get(), p);
    check(first && c.buffer() == first && !c.failed(), "the first use makes the buffer");
    auto v = readBuffer(first);
    check(v.size() == 4 && v[0] == 5040.0f && v[1] == 4873.0f && v[2] == 2.5f && v[3] == sb::kClipW, "it holds the viewport, the half-width and the cut, in the shader's order");
    const sb::StripParams q{4032.0f, 3898.0f, 2.0f, sb::kClipW};
    check(c.get(g_ctx.Get(), q) == first, "a new number rewrites the same buffer");
    v = readBuffer(first);
    check(v[0] == 4032.0f && v[1] == 3898.0f && v[2] == 2.0f, "and holds the new numbers");
    check(c.get(g_ctx.Get(), q) == first && c.written() == q, "the same numbers are a lookup");
    c.reset();
    check(c.buffer() == nullptr, "reset lets it go");
    ComPtr<ID3D11Device> d2;
    ComPtr<ID3D11DeviceContext> c2;
    ck(edvr::systemD3D11CreateDevice()(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d2, nullptr, &c2), "second device");
    sb::Constants two;
    ID3D11Buffer* onFirst = two.get(g_ctx.Get(), p);
    ID3D11Buffer* onSecond = two.get(c2.Get(), p);
    ComPtr<ID3D11Device> owner;
    onSecond->GetDevice(&owner);
    // (the first buffer was let go when the second device was seen, so its address may be reused: the second buffer's OWNER is the proof)
    check(onFirst && onSecond && owner.Get() == d2.Get() && owner.Get() != g_dev.Get(), "a second device gets a buffer of its own, made on that device (a destroyed device's is never reused)");

    sb::RasterStates states;
    ID3D11RasterizerState* off = states.get(g_ctx.Get(), false);
    ID3D11RasterizerState* on = states.get(g_ctx.Get(), true);
    check(off && on && off != on && states.get(g_ctx.Get(), false) == off && states.get(g_ctx.Get(), true) == on && !states.failed(), "the two states are made once and kept");
    D3D11_RASTERIZER_DESC d{};
    off->GetDesc(&d);
    check(d.FillMode == D3D11_FILL_SOLID && d.CullMode == D3D11_CULL_NONE && d.DepthClipEnable && !d.ScissorEnable, "the first culls nothing, fills solid, clips depth, and has no scissor test");
    on->GetDesc(&d);
    check(d.FillMode == D3D11_FILL_SOLID && d.CullMode == D3D11_CULL_NONE && d.DepthClipEnable && d.ScissorEnable, "the second the same with the scissor test on");
    check(states.get(nullptr, false) == nullptr, "no context, no state");

    // The tent's width from the two viewports.
    bool clamped = true;
    check(sb::tentHalfWidth(5040.0f, 2016.0f, &clamped) == 2.5f && !clamped, "a 5040 wide layer over a 2016 wide render: 2.5 layer pixels to a render pixel");
    check(sb::tentHalfWidth(4032.0f, 2016.0f, &clamped) == 2.0f && !clamped, "...4032 over 2016 (100%): 2.0");
    check(sb::tentHalfWidth(2016.0f, 2016.0f, &clamped) == 1.0f && !clamped, "...equal widths: 1.0, not a clamp");
    check(sb::tentHalfWidth(1000.0f, 2016.0f, &clamped) == 1.0f && clamped, "a layer narrower than the render clamps to 1 and says so");
    check(sb::tentHalfWidth(100000.0f, 2016.0f, &clamped) == sb::kMaxTent && clamped, "a layer more than sixteen times the render clamps to the maximum");
    check(sb::tentHalfWidth(0.0f, 2016.0f) == 0.0f && sb::tentHalfWidth(5040.0f, 0.0f) == 0.0f && sb::tentHalfWidth(std::nanf(""), 2016.0f) == 0.0f &&
              sb::tentHalfWidth(5040.0f, -1.0f) == 0.0f,
          "a dead viewport gives 0 (the caller refuses)");
}

// ---------------------------------------------------------------- the mutants
static std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
    size_t at = 0;
    unsigned n = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
        ++n;
    }
    if (!n) throw std::runtime_error("a mutant's anchor is not in the shader text: " + from);
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
            std::puts("supercruise_bars_test: dry-run (no files, no device)");
            return 0;
        }
        if (argc == 2 && !std::strcmp(argv[1], "--wiring")) {
            wiringCases();
            std::printf("PASS supercruise_bars_test --wiring: %u checks\n", g_checks);
            return 0;
        }
        const bool hardware = argc == 2 && !std::strcmp(argv[1], "--hardware-self-test");
        check(argc == 2 && (hardware || !std::strcmp(argv[1], "--self-test")), "usage: supercruise_bars_test --self-test | --hardware-self-test | --wiring | --dry-run");
        const D3D_DRIVER_TYPE driver = hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
        const HRESULT made = edvr::systemD3D11CreateDevice()(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
        if (FAILED(made))
            ck(edvr::systemD3D11CreateDevice()(nullptr, driver, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx), "device");
        g_dev.As(&g_messages);
        std::printf("supercruise_bars_test: %s%s\n", hardware ? "hardware adapter" : "WARP", SUCCEEDED(made) ? " with the debug layer" : "");

        // The game's pair: the stand-ins disassemble to the instruction lists of the game's own shaders.
        setupGamePair();
        const std::string gameVsAsm = instructionsOf(readText("tools/supercruise_bars_test/fixtures/vs_A47A3315FFF5E2E4.asm"));
        const std::string gamePsAsm = instructionsOf(readText("tools/supercruise_bars_test/fixtures/ps_869FFF43E875906E.asm"));
        check(gameVsAsm.size() > 200 && instructionsOf(disassemble(g_vsCode.Get())) == gameVsAsm,
              "the vertex shader stand-in disassembles to the instruction list of the game's vs A47A3315FFF5E2E4 (a 3-row dp4, NORMAL x (5,5,5,1), z = 0)");
        check(gamePsAsm.size() > 100 && instructionsOf(disassemble(g_psCode.Get())) == gamePsAsm,
              "the pixel shader stand-in disassembles to the instruction list of the game's ps 869FFF43E875906E (rgb x cb1[90].y, alpha from the vertex)");

        // The strip shader: the build's bytes are the HLSL text's compile, and the device takes them.
        const auto compiled = compileHlsl(edvr::kSupercruiseBarsGs, "gs_5_0");
        check(compiled->GetBufferSize() == sizeof(edvr::kSupercruiseBarsGsBytecode) &&
                  !std::memcmp(compiled->GetBufferPointer(), edvr::kSupercruiseBarsGsBytecode, sizeof(edvr::kSupercruiseBarsGsBytecode)),
              "the build's bytecode is exactly the compile of supercruise_bars_shader.h's text");
        ComPtr<ID3D11GeometryShader> gs;
        ck(g_dev->CreateGeometryShader(edvr::kSupercruiseBarsGsBytecode, sizeof(edvr::kSupercruiseBarsGsBytecode), nullptr, &gs), "the device takes the production strip shader");

        constantsCases();

        Fails production;
        geometryCases(gs.Get(), production);
        bindingCases(gs.Get(), production, true);
        rasterCases(gs.Get(), production, false);
        afterIssueCases(gs.Get(), production, false);
        if (production.n) std::printf("first failing production check: %s (%u failed)\n", production.first.c_str(), production.n);
        check(production.n == 0, "the production strip shader and binding pass every geometry, binding, state and issue-end case");

        // THE MUTANTS: each through the same cases, each must fail at least one.
        const std::string text = edvr::kSupercruiseBarsGs;
        struct Mutant {
            const char* name;
            std::string text;
        };
        const Mutant mutants[] = {
            {"no cut at the camera plane", replaceAll(replaceAll(text, "const bool in0 = p0.w >= clipW;", "const bool in0 = true;"), "const bool in1 = p1.w >= clipW;", "const bool in1 = true;")},
            {"the viewport's two axes swapped", replaceAll(text, "const float2 pixelsPerNdc = 0.5 * viewport;", "const float2 pixelsPerNdc = 0.5 * viewport.yx;")},
            {"the perpendicular made of the wrong components", replaceAll(text, "float2(-d.y, d.x)", "float2(d.y, d.x)")},
            {"no alpha edge: a box, not a tent", replaceAll(replaceAll(text, "float4(colour[a].rgb, 0)", "colour[a]"), "float4(colour[b].rgb, 0)", "colour[b]")},
            {"the half-width ignored", replaceAll(text, "/ length_ * halfWidth / pixelsPerNdc", "/ length_ * 1.0 / pixelsPerNdc")},
        };
        for (const Mutant& m : mutants) {
            const auto mg = gsFromText(m.text);
            Fails fails;
            geometryCases(mg.Get(), fails);
            char what[160];
            std::snprintf(what, sizeof(what), "the mutant \"%s\" is caught by the geometry cases", m.name);
            check(fails.n > 0, what);
        }
        {
            Fails fails;
            rasterCases(gs.Get(), fails, true);
            check(fails.n > 0, "the mutant \"the binding keeps the game's own rasterizer state\" is caught (its cull mode takes the strip)");
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
        std::printf("PASS supercruise_bars_test: %u checks; the strip shader, binding, constants and states on %s, five shader mutants and two binding mutants caught\n", g_checks,
                    hardware ? "the hardware adapter" : "WARP");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL %s (%u checks)\n", e.what(), g_checks);
        return 1;
    }
}
