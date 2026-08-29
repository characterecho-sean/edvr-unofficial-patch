#include "intro_upscale.h"

#include <windows.h>

#include <d3d11.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/intro_mode.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "shader_swap.h"

// AMD's own FSR, CPU side: A_CPU gives FsrEasuCon and FsrRcasCon, which
// compute the constants each pass needs. Transcribing that arithmetic by hand
// is exactly what vendoring exists to avoid.
#define A_CPU 1
// Their headers are a library of helpers, so at /W4 every one this build does
// not call is a C4505 -- about two hundred. Silenced HERE rather than in
// their text, so the vendored files stay byte-identical to upstream and a
// real warning from our own code cannot end up buried in theirs.
#pragma warning(push)
#pragma warning(disable : 4505)
#include "fsr/ffx_a.h"
#include "fsr/ffx_fsr1.h"
#pragma warning(pop)

// The same two files as GPU text, generated at build time.
#include "fsr_hlsl_gen.h"

namespace edvr {
namespace {

// The chain, in order, and why it is that order:
//
//   DEBAND at source size. The movie's blocking is in the ENCODE -- the
//   launch idents measure 2.87 to 15.5 Mbit/s at 1920x1080, and the low one
//   bands heavily in dark gradients. Blocks live in source space, so they are
//   flattened before anything magnifies them.
//   EASU  upscales the debanded frame to the on-foot panel's width.
//   RCAS  sharpens the upscaled result. AMD's own order: EASU then RCAS.
//
// Every stage is optional, and skipping one costs neither its time nor its
// memory -- nothing is allocated for a pass that is off.
constexpr uint32_t kMinFactor = 2;
constexpr uint32_t kMaxWidth = 8192;

// The deband, transcribed from backdrop_fix.cpp rather than reinvented. Its
// two field-bought lessons travel with it:
//
//   the THRESHOLD is the whole difference between a deband and a blur --
//   real structure exceeds it and passes through untouched;
//   the weight is SOFT, not a hard switch. "d < t ? average : centre" puts
//   adjacent pixels on opposite sides of a cliff, and chained passes bake
//   each cliff in and re-average it, which the menu backdrop's first field
//   run saw as blotches that were not in the source.
//
// Sampling is by integer load, so no filtering the game chose composes with
// ours, and the dither is a pure function of position -- one texture feeds
// both eyes, so it cannot differ between them.
const char kDebandHlsl[] =
    "Texture2D<float4> S : register(t0);\n"
    "RWTexture2D<float4> O : register(u0);\n"
    "cbuffer P : register(b0) { float4 p; };\n"
    "float mx3(float3 v) { return max(max(v.x, v.y), v.z); }\n"
    "float ign(float2 q) {\n"
    "    return frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715))));\n"
    "}\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    uint w, h;\n"
    "    O.GetDimensions(w, h);\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    int2 c0 = int2(id.xy);\n"
    "    int r = int(p.x);\n"
    "    int2 lo = int2(0, 0);\n"
    "    int2 hi = int2(int(w) - 1, int(h) - 1);\n"
    "    float4 c = S[c0];\n"
    "    float3 n0 = S[clamp(c0 + int2( r, 0), lo, hi)].rgb;\n"
    "    float3 n1 = S[clamp(c0 + int2(-r, 0), lo, hi)].rgb;\n"
    "    float3 n2 = S[clamp(c0 + int2( 0, r), lo, hi)].rgb;\n"
    "    float3 n3 = S[clamp(c0 + int2( 0,-r), lo, hi)].rgb;\n"
    "    float d = max(max(mx3(abs(n0 - c.rgb)), mx3(abs(n1 - c.rgb))),\n"
    "                  max(mx3(abs(n2 - c.rgb)), mx3(abs(n3 - c.rgb))));\n"
    "    float flatness = saturate(1.0 - d / max(p.y, 1e-6));\n"
    "    float3 o = lerp(c.rgb, (n0 + n1 + n2 + n3) * 0.25, flatness);\n"
    "    if (p.z > 0.0) o += (ign(float2(c0)) - 0.5) * p.z;\n"
    "    O[id.xy] = float4(saturate(o), c.a);\n"
    "}\n";

// Radius and threshold multiplier per pass. backdrop_fix's ladder: five
// passes reaching sixteen texels, because at this magnification a contour is
// several screen pixels wide and a one-texel search never finds its far side.
struct DebandPass {
    int   radius;
    float scale;
};
constexpr DebandPass kDebandPasses[] = {
    {1, 1.00f}, {2, 0.80f}, {4, 0.60f}, {8, 0.50f}, {16, 0.40f}};
constexpr int kDebandCount =
    static_cast<int>(sizeof(kDebandPasses) / sizeof(kDebandPasses[0]));

// EASU and RCAS, wrapped in the callbacks AMD's header asks the calling
// shader to provide. Everything the filtering itself does is theirs.
const char kEasuMain[] =
    "Texture2D<float4> Src : register(t0);\n"
    "SamplerState Smp : register(s0);\n"
    "RWTexture2D<float4> Dst : register(u0);\n"
    "cbuffer P : register(b0) {\n"
    "    uint4 con0; uint4 con1; uint4 con2; uint4 con3; uint2 dstSize;\n"
    "};\n"
    "AF4 FsrEasuRF(AF2 p) { return Src.GatherRed(Smp, p); }\n"
    "AF4 FsrEasuGF(AF2 p) { return Src.GatherGreen(Smp, p); }\n"
    "AF4 FsrEasuBF(AF2 p) { return Src.GatherBlue(Smp, p); }\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= dstSize.x || id.y >= dstSize.y) return;\n"
    "    AF3 c;\n"
    "    FsrEasuF(c, id.xy, con0, con1, con2, con3);\n"
    "    Dst[id.xy] = float4(c, 1.0);\n"
    "}\n";

const char kRcasMain[] =
    "Texture2D<float4> Src : register(t0);\n"
    "RWTexture2D<float4> Dst : register(u0);\n"
    "cbuffer P : register(b0) { uint4 con; uint2 dstSize; };\n"
    "AF4 FsrRcasLoadF(ASU2 p) { return Src.Load(int3(p, 0)); }\n"
    "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= dstSize.x || id.y >= dstSize.y) return;\n"
    "    AF3 c;\n"
    "    FsrRcasF(c.r, c.g, c.b, id.xy, con);\n"
    "    Dst[id.xy] = float4(c, 1.0);\n"
    "}\n";

const char kGpuPrologue[] =
    "#define A_GPU 1\n"
    "#define A_HLSL 1\n";

std::string joinChunks(const char* const* chunks) {
    std::string out;
    for (const char* const* c = chunks; *c; ++c) out += *c;
    return out;
}

// The Catmull-Rom fallback, for a rig where EASU will not compile. Its kernel
// is written out so it can be checked against a reference rather than
// trusted: the standard cubic with a = -0.5.
const char kCubicHlsl[] =
    "Texture2D<float4> Src : register(t0);\n"
    "RWTexture2D<float4> Dst : register(u0);\n"
    "cbuffer P : register(b0) { uint2 srcSize; uint2 dstSize; };\n"
    "float w(float t) {\n"
    "    t = abs(t);\n"
    "    if (t <= 1.0) return 1.5*t*t*t - 2.5*t*t + 1.0;\n"
    "    if (t <  2.0) return -0.5*t*t*t + 2.5*t*t - 4.0*t + 2.0;\n"
    "    return 0.0;\n"
    "}\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= dstSize.x || id.y >= dstSize.y) return;\n"
    "    float2 sp = (float2(id.xy) + 0.5) *\n"
    "                (float2(srcSize) / float2(dstSize)) - 0.5;\n"
    "    int2 b = int2(floor(sp));\n"
    "    float2 f = sp - float2(b);\n"
    "    float wx[4], wy[4];\n"
    "    [unroll] for (int k = 0; k < 4; ++k) {\n"
    "        wx[k] = w(float(k - 1) - f.x);\n"
    "        wy[k] = w(float(k - 1) - f.y);\n"
    "    }\n"
    "    float4 acc = 0.0;\n"
    "    float sum = 0.0;\n"
    "    [unroll] for (int j = 0; j < 4; ++j) {\n"
    "        [unroll] for (int i = 0; i < 4; ++i) {\n"
    "            int2 q = clamp(b + int2(i - 1, j - 1), int2(0, 0),\n"
    "                           int2(srcSize) - 1);\n"
    "            float cw = wx[i] * wy[j];\n"
    "            acc += Src.Load(int3(q, 0)) * cw;\n"
    "            sum += cw;\n"
    "        }\n"
    "    }\n"
    "    if (sum > 0.0) acc /= sum;\n"
    "    Dst[id.xy] = float4(saturate(acc.rgb), acc.a);\n"
    "}\n";

enum class Mode { kOff, kSharp, kFsr };

FaultBudget g_budget("introUpscale", 4);

Mode     g_mode = Mode::kOff;
Mode     g_running = Mode::kOff;
bool     g_on = false;
bool     g_failed = false;
bool     g_configured = false;
uint32_t g_targetW = 0;
float    g_deband = 0.0f;     // flatness threshold, 0..64 of 255; 0 = off
float    g_dither = 1.0f;     // LSBs, on the last deband pass only
float    g_sharpen = -1.0f;   // RCAS stops; negative = off

ID3D11ComputeShader* g_csDeband = nullptr;
ID3D11ComputeShader* g_csUp = nullptr;
ID3D11ComputeShader* g_csRcas = nullptr;
ID3D11Buffer*        g_cb = nullptr;
ID3D11SamplerState*  g_smp = nullptr;

// A texture with the views each stage needs. read is always NON-sRGB (the
// stored values, worked on unconverted); game carries the source view's
// sRGB-ness so the game's own sampler does exactly what it always did --
// backdrop_fix's rule, and the reason its deband did not shift the picture's
// brightness.
struct Tex {
    ID3D11Texture2D*           tex = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    ID3D11ShaderResourceView*  read = nullptr;
    ID3D11ShaderResourceView*  game = nullptr;
    void release() {
        if (uav) { uav->Release(); uav = nullptr; }
        if (read) { read->Release(); read = nullptr; }
        if (game) { game->Release(); game = nullptr; }
        if (tex) { tex->Release(); tex = nullptr; }
    }
};

Tex g_small[2];      // deband ping-pong, source size
Tex g_big[2];        // the upscale's output, then RCAS's
// The game's own surfaces, viewed unconverted -- ONE PER EYE, and kept
// apart from the chain on purpose.
//
// This is the bug that bit twice. The chain (textures, shaders, constants)
// depends only on SIZE and settings; the source view depends on the
// RESOURCE, and the two eyes have their own. Folding the resource into the
// chain's cache test made every second draw a full teardown and rebuild --
// and since the chain runs once a frame, the second eye then bound a texture
// nothing had dispatched into. That is what a flickering eye looks like from
// the outside, and a 1 MB log of rebuild lines is what it looks like from
// in here.
//
// So: two source views, looked up by resource, created on demand, and NEVER
// torn down by a chain rebuild.
struct SrcView {
    void*                     res = nullptr;
    ID3D11ShaderResourceView* raw = nullptr;
};
SrcView g_src[2];
uint32_t g_srcW = 0, g_srcH = 0, g_outW = 0, g_outH = 0;
bool     g_srgb = false;

bool g_doneThisFrame = false;
bool g_bound = false;
ID3D11ShaderResourceView* g_saved = nullptr;
ID3D11ShaderResourceView* g_final = nullptr;   // what the draw samples
uint32_t g_con[16] = {0};
uint32_t g_rcasCon[4] = {0};

void failOnce(const char* why) {
    if (g_failed) return;
    g_failed = true;
    Log::get().note("intro video upscale: %s. The movie is sampled the way "
                    "the game samples it for the rest of this session.", why);
}

// The chain only. The source views outlive it -- they describe the game's
// textures, not ours, and dropping them is what made the rebuild loop.
void releaseAll() {
    for (Tex& t : g_small) t.release();
    for (Tex& t : g_big) t.release();
    g_srcW = g_srcH = g_outW = g_outH = 0;
    g_final = nullptr;
}

void releaseSources() {
    for (SrcView& s : g_src) {
        if (s.raw) s.raw->Release();
        s = SrcView();
    }
}

// Our own NON-sRGB view of a game surface: the stored values, which is what
// its sampler would have decoded. Everything is worked on unconverted and
// the sRGB-ness is put back only in the game-facing view, so the game's
// shader does exactly what it always did.
ID3D11ShaderResourceView* rawViewOf(ID3D11Device* dev, ID3D11Resource* res) {
    for (SrcView& s : g_src) {
        if (s.res == res && s.raw) return s.raw;
    }
    for (SrcView& s : g_src) {
        if (s.raw) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC rd{};
        rd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (FAILED(dev->CreateShaderResourceView(res, &rd, &s.raw))) {
            s.raw = nullptr;
            return nullptr;
        }
        s.res = res;
        return s.raw;
    }
    return nullptr;   // more than two eyes is not a case that exists
}

bool srgbOf(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

bool makeTex(ID3D11Device* dev, uint32_t w, uint32_t h, bool srgb, Tex* out) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &out->tex))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(dev->CreateShaderResourceView(out->tex, &sd, &out->read))) {
        return false;
    }
    sd.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                     : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(dev->CreateShaderResourceView(out->tex, &sd, &out->game))) {
        return false;
    }
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    return SUCCEEDED(dev->CreateUnorderedAccessView(out->tex, &ud, &out->uav));
}

uint32_t targetWidthFor(uint32_t srcW) {
    uint32_t w = g_targetW ? g_targetW : srcW * kMinFactor;
    if (w < srcW * kMinFactor) w = srcW * kMinFactor;
    if (w > kMaxWidth) w = kMaxWidth;
    return w;
}

bool setParams(ID3D11DeviceContext* ctx, const void* data, size_t bytes) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) {
        return false;
    }
    memset(m.pData, 0, 96);
    memcpy(m.pData, data, bytes);
    ctx->Unmap(g_cb, 0);
    return true;
}

bool build(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src) {
    ID3D11Resource* res = nullptr;
    src->GetResource(&res);
    if (!res) return false;
    ID3D11Texture2D* t = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t));
    if (!t) { res->Release(); return false; }
    D3D11_TEXTURE2D_DESC td{};
    t->GetDesc(&td);
    t->Release();
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    src->GetDesc(&sd);
    const bool srgb = srgbOf(sd.Format);
    const uint32_t wantW = targetWidthFor(td.Width);

    // Keyed on SIZE and on what is allocated -- never on which surface was
    // seen first.
    //
    // The two eyes have their OWN source surfaces (measured: distinct
    // resources, identical content), and keying on the resource made each
    // eye's draw destroy the other's build. The second eye of the pair then
    // bound a texture nothing had dispatched into, and sampled undefined
    // memory every frame. One chain serves both eyes; this test is what makes
    // that true rather than aspirational.
    //
    // g_srcRaw is the exception that must follow the resource: it is a view
    // OVER the game's texture, so it is rebuilt when that changes -- which is
    // why the source pointer is compared here and nowhere else.
    if (g_big[0].tex && g_srcW == td.Width && g_srcH == td.Height &&
        g_outW == wantW && g_srgb == srgb) {
        // The chain stands. Make sure THIS eye's source view exists -- that
        // is per-resource work and must not disturb anything else.
        ID3D11Device* d2 = nullptr;
        ctx->GetDevice(&d2);
        const bool haveView = d2 && rawViewOf(d2, res) != nullptr;
        if (d2) d2->Release();
        res->Release();
        return haveView;
    }
    releaseAll();

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) { res->Release(); return false; }

    bool ok = true;
    if (!g_csUp) {
        if (g_mode == Mode::kFsr) {
            const std::string hlsl = std::string(kGpuPrologue) +
                                     joinChunks(kFfxAChunks) +
                                     "#define FSR_EASU_F 1\n" +
                                     joinChunks(kFfxFsr1Chunks) + kEasuMain;
            g_csUp = shaderSwapCompileCs(ctx, hlsl.c_str(), hlsl.size(), "main",
                                         "intro easu", nullptr,
                                         "intro video upscale");
            g_running = g_csUp ? Mode::kFsr : Mode::kOff;
            if (!g_csUp) {
                Log::get().note(
                    "intro video upscale: EASU would not compile; falling "
                    "back to the bicubic, a smaller win than FSR and a much "
                    "larger one than the bilinear the game does.");
            }
        }
        if (!g_csUp) {
            g_csUp = shaderSwapCompileCs(ctx, kCubicHlsl,
                                         sizeof(kCubicHlsl) - 1, "main",
                                         "intro cubic", nullptr,
                                         "intro video upscale");
            g_running = g_csUp ? Mode::kSharp : Mode::kOff;
        }
        if (!g_csUp) { ok = false; failOnce("no resampler would compile"); }
    }
    if (ok && g_running == Mode::kFsr && !g_smp) {
        D3D11_SAMPLER_DESC sm{};
        sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(dev->CreateSamplerState(&sm, &g_smp));
        if (!ok) failOnce("the resampler's sampler could not be created");
    }
    if (ok && g_deband > 0.0f && !g_csDeband) {
        g_csDeband = shaderSwapCompileCs(ctx, kDebandHlsl,
                                         sizeof(kDebandHlsl) - 1, "main",
                                         "intro deband", nullptr,
                                         "intro video upscale");
        if (!g_csDeband) {
            Log::get().note("intro video upscale: the deband would not "
                            "compile; the frame is resampled without it.");
            g_deband = 0.0f;
        }
    }
    if (ok && g_sharpen >= 0.0f && g_running == Mode::kFsr && !g_csRcas) {
        const std::string hlsl = std::string(kGpuPrologue) +
                                 joinChunks(kFfxAChunks) +
                                 "#define FSR_RCAS_F 1\n" +
                                 joinChunks(kFfxFsr1Chunks) + kRcasMain;
        g_csRcas = shaderSwapCompileCs(ctx, hlsl.c_str(), hlsl.size(), "main",
                                       "intro rcas", nullptr,
                                       "intro video upscale");
        if (!g_csRcas) {
            Log::get().note("intro video upscale: RCAS would not compile; the "
                            "frame is upscaled without the sharpening pass.");
            g_sharpen = -1.0f;
        }
    }
    if (ok && !g_cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 96;   // the largest pass's block, rounded to 16
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
        if (!ok) failOnce("the parameter buffer could not be created");
    }

    const uint32_t outW = wantW;
    const uint32_t outH =
        td.Width ? static_cast<uint32_t>(
                       (static_cast<uint64_t>(outW) * td.Height +
                        td.Width / 2) / td.Width)
                 : 0;
    if (ok && (outW == 0 || outH == 0)) ok = false;

    if (ok) {
        ok = rawViewOf(dev, res) != nullptr;
        if (!ok) failOnce("the movie's frame could not be viewed unconverted");
    }
    if (ok && g_deband > 0.0f && g_csDeband) {
        ok = makeTex(dev, td.Width, td.Height, srgb, &g_small[0]) &&
             makeTex(dev, td.Width, td.Height, srgb, &g_small[1]);
        if (!ok) failOnce("the deband buffers could not be created");
    }
    if (ok) {
        ok = makeTex(dev, outW, outH, srgb, &g_big[0]);
        if (!ok) failOnce("the resampled frame could not be created");
    }
    if (ok && g_sharpen >= 0.0f && g_running == Mode::kFsr && g_csRcas) {
        ok = makeTex(dev, outW, outH, srgb, &g_big[1]);
        if (!ok) failOnce("the sharpened frame could not be created");
    }

    if (ok) {
        g_srcW = td.Width;
        g_srcH = td.Height;
        g_outW = outW;
        g_outH = outH;
        g_srgb = srgb;
        g_final = g_big[1].tex ? g_big[1].game : g_big[0].game;
        if (g_running == Mode::kFsr) {
            FsrEasuCon(reinterpret_cast<AU1*>(g_con + 0),
                       reinterpret_cast<AU1*>(g_con + 4),
                       reinterpret_cast<AU1*>(g_con + 8),
                       reinterpret_cast<AU1*>(g_con + 12),
                       static_cast<AF1>(td.Width), static_cast<AF1>(td.Height),
                       static_cast<AF1>(td.Width), static_cast<AF1>(td.Height),
                       static_cast<AF1>(outW), static_cast<AF1>(outH));
            if (g_csRcas && g_sharpen >= 0.0f) {
                FsrRcasCon(reinterpret_cast<AU1*>(g_rcasCon),
                           static_cast<AF1>(g_sharpen));
            }
        }
        const double mb =
            (static_cast<double>(outW) * outH * 4.0 * (g_big[1].tex ? 2 : 1) +
             static_cast<double>(td.Width) * td.Height * 4.0 *
                 (g_small[0].tex ? 2 : 0)) / 1048576.0;
        Log::get().note(
            "intro video upscale: %s -- %ux%u to %ux%u%s%s, and the composite "
            "samples ours. The width is fix.vscreen_res_width, floored at "
            "twice the source; %.0f MB while the intro runs. No resampler "
            "adds detail: this is a 1080p source either way, and its blocking "
            "is in the encode.",
            g_running == Mode::kFsr ? "FSR (AMD's EASU)"
                                    : "SHARP (Catmull-Rom)",
            td.Width, td.Height, outW, outH,
            g_small[0].tex ? ", debanded first" : "",
            g_big[1].tex ? ", then RCAS-sharpened" : "", mb);
    }
    dev->Release();
    res->Release();
    return ok && !g_failed;
}

// One dispatch, with the SRV and UAV unbound first.
//
// A texture cannot be an SRV and a UAV in the same dispatch, and D3D silently
// nulls the SRV if asked -- which reads BLACK rather than the previous pass.
// backdrop_fix records that; a ping-pong would hit it every pass.
void runPass(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs,
             ID3D11ShaderResourceView* in, ID3D11UnorderedAccessView* out,
             uint32_t w, uint32_t h) {
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 1, &nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &in);
    ctx->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

void runChain(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srcRaw) {
    // The compute stage is saved and put back around the WHOLE chain: the
    // exposure fix owns slots here and must not find ours.
    ID3D11ComputeShader* csWas = nullptr;
    ID3D11ShaderResourceView* srvWas = nullptr;
    ID3D11UnorderedAccessView* uavWas = nullptr;
    ID3D11Buffer* cbWas = nullptr;
    ID3D11SamplerState* smpWas = nullptr;
    ctx->CSGetShader(&csWas, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 1, &srvWas);
    ctx->CSGetUnorderedAccessViews(0, 1, &uavWas);
    ctx->CSGetConstantBuffers(0, 1, &cbWas);
    ctx->CSGetSamplers(0, 1, &smpWas);

    ID3D11Buffer* cb = g_cb;
    ctx->CSSetConstantBuffers(0, 1, &cb);
    if (g_smp) {
        ID3D11SamplerState* s = g_smp;
        ctx->CSSetSamplers(0, 1, &s);
    }

    // 1. Deband, at source size, ping-ponging. The first pass reads the
    //    game's surface unconverted; each later one reads what the previous
    //    wrote, and the upscale reads whichever buffer the parity landed on.
    ID3D11ShaderResourceView* upIn = srcRaw;
    if (g_csDeband && g_deband > 0.0f && g_small[0].tex) {
        for (int i = 0; i < kDebandCount; ++i) {
            const int dst = i % 2;
            ID3D11ShaderResourceView* in =
                (i == 0) ? srcRaw : g_small[1 - dst].read;
            const float p[4] = {static_cast<float>(kDebandPasses[i].radius),
                                g_deband * kDebandPasses[i].scale / 255.0f,
                                (i == kDebandCount - 1) ? g_dither / 255.0f
                                                        : 0.0f,
                                0.0f};
            if (!setParams(ctx, p, sizeof(p))) break;
            runPass(ctx, g_csDeband, in, g_small[dst].uav, g_srcW, g_srcH);
            upIn = g_small[dst].read;
        }
    }

    // 2. Upscale.
    if (g_running == Mode::kFsr) {
        uint32_t p[20] = {0};
        memcpy(p, g_con, sizeof(g_con));
        p[16] = g_outW;
        p[17] = g_outH;
        if (setParams(ctx, p, sizeof(p))) {
            runPass(ctx, g_csUp, upIn, g_big[0].uav, g_outW, g_outH);
        }
    } else {
        const uint32_t p[4] = {g_srcW, g_srcH, g_outW, g_outH};
        if (setParams(ctx, p, sizeof(p))) {
            runPass(ctx, g_csUp, upIn, g_big[0].uav, g_outW, g_outH);
        }
    }

    // 3. RCAS, on the upscaled result, which is AMD's own order.
    if (g_csRcas && g_big[1].tex) {
        uint32_t p[6] = {0};
        memcpy(p, g_rcasCon, sizeof(g_rcasCon));
        p[4] = g_outW;
        p[5] = g_outH;
        if (setParams(ctx, p, sizeof(p))) {
            runPass(ctx, g_csRcas, g_big[0].read, g_big[1].uav, g_outW, g_outH);
        }
    }

    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 1, &nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShader(csWas, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &srvWas);
    ctx->CSSetUnorderedAccessViews(0, 1, &uavWas, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &cbWas);
    ctx->CSSetSamplers(0, 1, &smpWas);
    if (csWas) csWas->Release();
    if (srvWas) srvWas->Release();
    if (uavWas) uavWas->Release();
    if (cbWas) cbWas->Release();
    if (smpWas) smpWas->Release();
}

}  // namespace

void introUpscaleConfigure(Config& cfg) {
    const IntroVideoMode ivm =
        introVideoParse(cfg.getString("fix.intro_video", "screen"));
    const Mode want = ivm.upscale == 2
                          ? Mode::kFsr
                          : (ivm.upscale == 1 ? Mode::kSharp : Mode::kOff);
    const float wasDeband = g_deband;
    const float wasSharpen = g_sharpen;
    g_targetW = static_cast<uint32_t>(
        cfg.getIntInRange("fix.vscreen_res_width", 1920, 640, 8192));
    g_deband = static_cast<float>(
        cfg.getIntInRange("advanced.intro_video_deband", 8, 0, 64));
    g_dither = static_cast<float>(
        cfg.getIntInRange("advanced.intro_video_dither", 1, 0, 8));
    {
        // AMD's own unit: STOPS of sharpness reduction, so 0 is the sharpest
        // and 2 the mildest. "off" skips the pass and its memory entirely.
        const std::string s =
            cfg.getString("advanced.intro_video_sharpen", "0.25");
        g_sharpen = (s == "off") ? -1.0f : static_cast<float>(atof(s.c_str()));
        if (g_sharpen > 2.0f) g_sharpen = 2.0f;
    }
    const bool first = !g_configured;
    g_configured = true;
    if (want == g_mode && g_deband == wasDeband && g_sharpen == wasSharpen &&
        !first) {
        return;
    }
    // Anything that changes which passes run changes what is allocated, so
    // the built chain goes and the next matched draw rebuilds it.
    releaseAll();
    if (want != g_mode) {
        if (g_csUp) { g_csUp->Release(); g_csUp = nullptr; }
        if (g_csRcas) { g_csRcas->Release(); g_csRcas = nullptr; }
        g_running = Mode::kOff;
    }
    g_mode = want;
    g_on = want != Mode::kOff;
    if (!g_on) {
        if (!first) {
            Log::get().note("intro video upscale: stock. The movie is sampled "
                            "the way the game samples it.");
        }
        return;
    }
    Log::get().note(
        "intro video upscale: %s. The movie's frame is resampled to the "
        "on-foot panel's width before the game magnifies it across the "
        "screen, which is where the pixelation comes from -- a 1080p frame "
        "drawn about 2.95x linear. Deband %s, sharpening %s. It smooths and "
        "sharpens; it cannot add detail (docs\\intro-video.md).",
        want == Mode::kFsr ? "FSR (AMD's own EASU, vendored)"
                           : "SHARP (Catmull-Rom)",
        g_deband > 0.0f ? "on" : "off",
        (g_sharpen >= 0.0f && want == Mode::kFsr) ? "on" : "off");
}

bool introUpscaleWants() { return g_on && !g_failed; }

bool introUpscaleBegin(ID3D11DeviceContext* ctx,
                       ID3D11ShaderResourceView* srcSrv) {
    if (!introUpscaleWants() || !ctx || !srcSrv) return false;
    bool bound = false;
    guardedBudget(g_budget, [&] {
        if (!build(ctx, srcSrv)) return;
        if (!g_doneThisFrame) {
            // Whichever eye arrives first drives the chain; the other simply
            // binds the result. Their content is identical (same planes, same
            // shaders, measured), so one pass is not just cheaper -- it makes
            // the eyes unable to differ.
            ID3D11Resource* res = nullptr;
            srcSrv->GetResource(&res);
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            ID3D11ShaderResourceView* raw =
                (res && dev) ? rawViewOf(dev, res) : nullptr;
            if (dev) dev->Release();
            if (res) res->Release();
            if (!raw) return;
            runChain(ctx, raw);
            g_doneThisFrame = true;
        }
        if (!g_final) return;
        ctx->PSGetShaderResources(0, 1, &g_saved);
        // Armed BEFORE the substitution: a fault between the two would leave
        // ours bound with nothing owing a restore, and every later draw in
        // the frame would sample a movie frame. backdrop_fix's scar.
        g_bound = true;
        ID3D11ShaderResourceView* mine = g_final;
        ctx->PSSetShaderResources(0, 1, &mine);
        bound = true;
    });
    return bound;
}

void introUpscaleEnd(ID3D11DeviceContext* ctx) {
    if (!g_bound || !ctx) {
        if (g_saved) { g_saved->Release(); g_saved = nullptr; }
        return;
    }
    g_bound = false;
    guardedBudget(g_budget, [&] { ctx->PSSetShaderResources(0, 1, &g_saved); });
    if (g_saved) { g_saved->Release(); g_saved = nullptr; }
}

void introUpscaleFrameEnd() { g_doneThisFrame = false; }

void introUpscaleShutdown() {
    releaseAll();
    releaseSources();
    if (g_csDeband) { g_csDeband->Release(); g_csDeband = nullptr; }
    if (g_csUp) { g_csUp->Release(); g_csUp = nullptr; }
    if (g_csRcas) { g_csRcas->Release(); g_csRcas = nullptr; }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_smp) { g_smp->Release(); g_smp = nullptr; }
    if (g_saved) { g_saved->Release(); g_saved = nullptr; }
    g_bound = false;
}

}  // namespace edvr
