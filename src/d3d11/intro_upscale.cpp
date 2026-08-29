#include "intro_upscale.h"

#include <windows.h>

#include <d3d11.h>

#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "shader_swap.h"

// AMD's own FSR, CPU side: A_CPU gives FsrEasuCon, which computes the four
// constants EASU needs from the input and output sizes. Transcribing that
// arithmetic by hand is exactly what vendoring exists to avoid.
#define A_CPU 1
// AMD's headers are a library of helpers, so at /W4 every one this build
// does not call is a C4505. They are vendored unmodified and will stay that
// way, so the warning is silenced HERE rather than in their text -- a real
// warning from our own code must not end up buried in two hundred of theirs.
#pragma warning(push)
#pragma warning(disable : 4505)   // unreferenced local function removed
#include "fsr/ffx_a.h"
#include "fsr/ffx_fsr1.h"
#pragma warning(pop)

// The same two files as GPU text, generated at build time.
#include "fsr_hlsl_gen.h"

namespace edvr {
namespace {

// Twice the source in each axis. Three times would land the game's own
// sampler at 1:1, and costs 74 MB against 33; two leaves the final step a
// gentle 1.47x, which is where the returns stop being worth the memory on a
// rig already running a 5424x5356 eye.
constexpr uint32_t kFactor = 2;

// Which resampler ran. "sharp" is the Catmull-Rom below; "fsr" is AMD's
// EASU, with Catmull-Rom as its fallback if the compile fails -- a rig
// without a working compiler for one is unlikely to have one for the other,
// but degrading to a good bicubic beats degrading to bilinear.
enum class Mode { kOff, kSharp, kFsr };
Mode g_mode = Mode::kOff;
Mode g_running = Mode::kOff;

// EASU's entry point, wrapped in the callbacks AMD's header asks the
// calling shader to provide: three gather4s, one per colour channel.
// Everything the resampling itself does is theirs; this is the plumbing
// around it, and the shape comes from their own usage block.
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

// The defines AMD's header documents for an HLSL float build, in the
// order its own usage block gives them.
const char kEasuPrologue[] =
    "#define A_GPU 1\n"
    "#define A_HLSL 1\n";

const char kEasuMiddle[] =
    "#define FSR_EASU_F 1\n";

std::string joinChunks(const char* const* chunks) {
    std::string out;
    for (const char* const* c = chunks; *c; ++c) out += *c;
    return out;
}

// Catmull-Rom, stated so it can be checked rather than trusted. The kernel
// is the standard cubic with a = -0.5:
//
//   w(t) = 1.5|t|^3 - 2.5|t|^2 + 1          for |t| <= 1
//   w(t) = -0.5|t|^3 + 2.5|t|^2 - 4|t| + 2  for 1 < |t| < 2
//
// Sixteen taps, separable, evaluated per output pixel. Sampling is by
// integer load rather than through a sampler, so no filtering the game
// chose can quietly compose with ours.
const char kUpscaleHlsl[] =
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
    "            int2 p = clamp(b + int2(i - 1, j - 1), int2(0, 0),\n"
    "                           int2(srcSize) - 1);\n"
    "            float cw = wx[i] * wy[j];\n"
    "            acc += Src.Load(int3(p, 0)) * cw;\n"
    "            sum += cw;\n"
    "        }\n"
    "    }\n"
    "    if (sum > 0.0) acc /= sum;\n"
    "    Dst[id.xy] = float4(saturate(acc.rgb), acc.a);\n"
    "}\n";

FaultBudget g_budget("introUpscale", 4);

bool g_on = false;
bool g_failed = false;
bool g_configured = false;

ID3D11ComputeShader*      g_cs = nullptr;
ID3D11Buffer*             g_cb = nullptr;
ID3D11Texture2D*          g_tex = nullptr;
ID3D11UnorderedAccessView* g_uav = nullptr;
ID3D11ShaderResourceView* g_gameSrv = nullptr;   // what the draw samples
uint32_t g_srcW = 0, g_srcH = 0;
void*    g_srcRes = nullptr;      // the surface the build was made for

bool g_doneThisFrame = false;
bool g_bound = false;
ID3D11ShaderResourceView* g_saved = nullptr;
ID3D11SamplerState* g_smp = nullptr;   // EASU gathers through a sampler
uint32_t g_dispatches = 0;
// EASU's four constants, from AMD's own FsrEasuCon on the CPU.
uint32_t g_con[16] = {0};

void failOnce(const char* why) {
    if (g_failed) return;
    g_failed = true;
    Log::get().note("intro video upscale: %s. The movie is sampled the way "
                    "the game samples it for the rest of this session.", why);
}

void releaseAll() {
    if (g_uav) { g_uav->Release(); g_uav = nullptr; }
    if (g_gameSrv) { g_gameSrv->Release(); g_gameSrv = nullptr; }
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    g_srcW = g_srcH = 0;
    g_srcRes = nullptr;
}

// Is this SRV's format an sRGB one? Our game-facing view must match, or the
// game's shader would decode our pixels differently from its own -- the
// lesson backdrop_fix records for the same substitution.
bool srgbOf(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
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
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    src->GetDesc(&sd);
    const bool srgb = srgbOf(sd.Format);
    t->Release();

    if (g_tex && g_srcRes == res && g_srcW == td.Width && g_srcH == td.Height) {
        res->Release();
        return true;
    }
    releaseAll();

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) { res->Release(); return false; }

    bool ok = true;
    if (!g_cs) {
        if (g_mode == Mode::kFsr) {
            // AMD's two files ahead of our entry point. HLSL is text:
            // concatenation is what #include would have done, without
            // needing an include handler inside D3DCompile.
            const std::string hlsl = std::string(kEasuPrologue) +
                                     joinChunks(kFfxAChunks) + kEasuMiddle +
                                     joinChunks(kFfxFsr1Chunks) + kEasuMain;
            g_cs = shaderSwapCompileCs(ctx, hlsl.c_str(), hlsl.size(), "main",
                                       "intro easu", nullptr,
                                       "intro video upscale");
            g_running = g_cs ? Mode::kFsr : Mode::kOff;
            if (!g_cs) {
                Log::get().note(
                    "intro video upscale: EASU would not compile; falling "
                    "back to the bicubic, which is a smaller win than FSR "
                    "and a much larger one than the bilinear the game does.");
            }
        }
        if (!g_cs) {
            g_cs = shaderSwapCompileCs(ctx, kUpscaleHlsl,
                                       sizeof(kUpscaleHlsl) - 1, "main",
                                       "intro upscale", nullptr,
                                       "intro video upscale");
            g_running = g_cs ? Mode::kSharp : Mode::kOff;
        }
        if (!g_cs) { ok = false; failOnce("no resampler would compile"); }
    }
    if (ok && g_running == Mode::kFsr && !g_smp) {
        D3D11_SAMPLER_DESC sm{};
        sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(dev->CreateSamplerState(&sm, &g_smp));
        if (!ok) failOnce("the resampler's sampler could not be created");
    }
    const DXGI_FORMAT kTypeless = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const DXGI_FORMAT kTyped = DXGI_FORMAT_R8G8B8A8_UNORM;
    const DXGI_FORMAT kGame =
        srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (ok) {
        D3D11_TEXTURE2D_DESC od{};
        od.Width = td.Width * kFactor;
        od.Height = td.Height * kFactor;
        od.MipLevels = 1;
        od.ArraySize = 1;
        od.Format = kTypeless;
        od.SampleDesc.Count = 1;
        od.Usage = D3D11_USAGE_DEFAULT;
        od.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        ok = SUCCEEDED(dev->CreateTexture2D(&od, nullptr, &g_tex));
        if (!ok) failOnce("the resampled frame could not be created");
    }
    if (ok) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        vd.Format = kGame;
        ok = SUCCEEDED(dev->CreateShaderResourceView(g_tex, &vd, &g_gameSrv));
        if (!ok) failOnce("the resampled frame could not be viewed");
    }
    if (ok) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = kTyped;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        ok = SUCCEEDED(dev->CreateUnorderedAccessView(g_tex, &ud, &g_uav));
        if (!ok) failOnce("the resampled frame is not writable by compute");
    }
    if (ok && !g_cb) {
        D3D11_BUFFER_DESC bd{};
        // Four uint4s of EASU constants plus the output size, rounded to the
        // 16-byte multiple a constant buffer wants. The bicubic uses the
        // first four words of the same buffer.
        bd.ByteWidth = 80;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
        if (!ok) failOnce("the resampler's parameters could not be created");
    }
    if (ok) {
        g_srcW = td.Width;
        g_srcH = td.Height;
        g_srcRes = res;
        if (g_running == Mode::kFsr) {
            // AMD's own constant setup. Viewport and input image are the
            // same here -- the movie is not dynamically scaled -- and the
            // output is what we allocated.
            FsrEasuCon(reinterpret_cast<AU1*>(g_con + 0),
                       reinterpret_cast<AU1*>(g_con + 4),
                       reinterpret_cast<AU1*>(g_con + 8),
                       reinterpret_cast<AU1*>(g_con + 12),
                       static_cast<AF1>(td.Width), static_cast<AF1>(td.Height),
                       static_cast<AF1>(td.Width), static_cast<AF1>(td.Height),
                       static_cast<AF1>(td.Width * kFactor),
                       static_cast<AF1>(td.Height * kFactor));
        }
        Log::get().note(
            "intro video upscale: %s -- the movie's %ux%u frame is "
            "resampled to %ux%u and the "
            "composite samples ours. It leaves the game's own sampler a "
            "%.2fx step instead of about 2.95x. No resampler adds detail: "
            "this is a 1080p source either way.",
            g_running == Mode::kFsr ? "FSR (AMD's EASU)"
                                    : "SHARP (Catmull-Rom, sixteen taps)",
            td.Width, td.Height, td.Width * kFactor, td.Height * kFactor,
            2.95 / kFactor);
    }
    dev->Release();
    res->Release();
    return ok && !g_failed;
}

}  // namespace

void introUpscaleConfigure(Config& cfg) {
    const std::string v = cfg.getString("fix.intro_video_upscale", "stock");
    const bool on = (v == "sharp");
    const bool first = !g_configured;
    g_configured = true;
    if (on == g_on && !first) return;
    g_on = on;
    if (!on) {
        if (!first) {
            Log::get().note("intro video upscale: stock. The movie is sampled "
                            "the way the game samples it.");
        }
        return;
    }
    Log::get().note(
        "intro video upscale: SHARP. The movie's frame is resampled to twice "
        "its size before the game magnifies it across the screen, which is "
        "where the pixelation comes from -- a 1080p frame drawn about 2.95x "
        "linear. It sharpens; it cannot add detail (docs\\intro-video.md).",
        g_mode == Mode::kFsr ? "FSR (AMD's own EASU, vendored)"
                             : "SHARP (Catmull-Rom)");
}

bool introUpscaleWants() { return g_on && !g_failed; }

bool introUpscaleBegin(ID3D11DeviceContext* ctx,
                       ID3D11ShaderResourceView* srcSrv) {
    if (!introUpscaleWants() || !ctx || !srcSrv) return false;
    bool bound = false;
    guardedBudget(g_budget, [&] {
        if (!build(ctx, srcSrv)) return;
        if (!g_doneThisFrame) {
            // The resample, once a frame. Both eyes' surfaces hold identical
            // content, so one pass serves both -- and by construction they
            // cannot end up different, which is the failure the FSS arc spent
            // its longest rounds on.
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) &&
                m.pData) {
                uint32_t p[20] = {0};
                if (g_running == Mode::kFsr) {
                    memcpy(p, g_con, sizeof(g_con));
                    p[16] = g_srcW * kFactor;
                    p[17] = g_srcH * kFactor;
                } else {
                    p[0] = g_srcW;
                    p[1] = g_srcH;
                    p[2] = g_srcW * kFactor;
                    p[3] = g_srcH * kFactor;
                }
                memcpy(m.pData, p, sizeof(p));
                ctx->Unmap(g_cb, 0);
            }
            // The compute stage is saved and put back: the exposure fix owns
            // slots here and a dispatch of ours must not be what it finds.
            ID3D11ComputeShader* csWas = nullptr;
            ID3D11ShaderResourceView* srvWas = nullptr;
            ID3D11UnorderedAccessView* uavWas = nullptr;
            ID3D11Buffer* cbWas = nullptr;
            ctx->CSGetShader(&csWas, nullptr, nullptr);
            ctx->CSGetShaderResources(0, 1, &srvWas);
            ctx->CSGetUnorderedAccessViews(0, 1, &uavWas);
            ctx->CSGetConstantBuffers(0, 1, &cbWas);

            ID3D11ShaderResourceView* in = srcSrv;
            ID3D11UnorderedAccessView* out = g_uav;
            ID3D11Buffer* cb = g_cb;
            ID3D11SamplerState* smpWas = nullptr;
            ctx->CSGetSamplers(0, 1, &smpWas);
            ctx->CSSetShader(g_cs, nullptr, 0);
            if (g_smp) {
                ID3D11SamplerState* s = g_smp;
                ctx->CSSetSamplers(0, 1, &s);
            }
            ctx->CSSetShaderResources(0, 1, &in);
            ctx->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
            ctx->CSSetConstantBuffers(0, 1, &cb);
            ctx->Dispatch((g_srcW * kFactor + 7) / 8,
                          (g_srcH * kFactor + 7) / 8, 1);

            ID3D11ShaderResourceView* nullSrv = nullptr;
            ID3D11UnorderedAccessView* nullUav = nullptr;
            ctx->CSSetShaderResources(0, 1, &nullSrv);
            ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
            ctx->CSSetShader(csWas, nullptr, 0);
            ctx->CSSetShaderResources(0, 1, &srvWas);
            ctx->CSSetUnorderedAccessViews(0, 1, &uavWas, nullptr);
            ctx->CSSetConstantBuffers(0, 1, &cbWas);
            ctx->CSSetSamplers(0, 1, &smpWas);
            if (smpWas) smpWas->Release();
            if (csWas) csWas->Release();
            if (srvWas) srvWas->Release();
            if (uavWas) uavWas->Release();
            if (cbWas) cbWas->Release();

            g_doneThisFrame = true;
            ++g_dispatches;
        }
        if (!g_gameSrv) return;
        ctx->PSGetShaderResources(0, 1, &g_saved);
        // Armed BEFORE the substitution: a fault between the two would
        // otherwise leave ours bound with nothing owing a restore, and every
        // later draw in the frame would sample a movie. backdrop_fix's note,
        // and its scar.
        g_bound = true;
        ID3D11ShaderResourceView* mine = g_gameSrv;
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
    if (g_cs) { g_cs->Release(); g_cs = nullptr; }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_smp) { g_smp->Release(); g_smp = nullptr; }
    if (g_saved) { g_saved->Release(); g_saved = nullptr; }
    g_bound = false;
}

}  // namespace edvr
