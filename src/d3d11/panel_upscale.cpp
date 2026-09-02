#include "panel_upscale.h"

#include <windows.h>

#include <d3d11.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "../common/frame_flag.h"   // eyeTextureSize
#include "exposure_fix.h"   // lookupShaderHash
#include "shader_swap.h"

// AMD's FSR, CPU side: A_CPU gives FsrEasuCon and FsrRcasCon, the same
// constants the shaders below consume. Unmodified sources; see
// src/d3d11/fsr/README.md.
#define A_CPU 1
#include "fsr/ffx_a.h"
#include "fsr/ffx_fsr1.h"

#include "fsr_hlsl_gen.h"   // the same two files, as HLSL string chunks

namespace edvr {
namespace {

// vs 81216C77F90DEDD6 on game build 332753 -- the shared holo-panel family.
// Pinnable, because a game update can recompile a shader without changing
// what it does.
constexpr uint64_t kVsHash = 0x81216C77F90DEDD6ull;

// The surface the target indicator is on, as a FRACTION of the render
// resolution rather than a literal size. Sizes are per-session -- reusing a
// stale one is what made this fix read as refuted for most of a day -- so
// the default is a ratio and the ini can override with an exact size.
//
// 2440x1996 on a 5424x5356 eye and 1952x1597 on a 4340x4284 one both give
// these to four figures.
constexpr float kFracW = 0.4498f;
constexpr float kFracH = 0.3726f;
constexpr float kFracTol = 0.004f;   // a little over a pixel at these sizes

// The panel slot. Slots 0 and 1 are the family's shared inputs (a 1536x1536
// and a 512x512); slot 2 is the per-panel surface.
constexpr uint32_t kSlot = 2;

const char kGpuPrologue[] =
    "#define A_GPU 1\n"
    "#define A_HLSL 1\n";

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
    "    // The panel is composited over the cockpit, so ALPHA is its shape.\n"
    "    // EASU has no alpha path; a bilinear tap at the same place keeps\n"
    "    // the coverage the game drew.\n"
    "    float2 uv = (float2(id.xy) + 0.5) / float2(dstSize);\n"
    "    Dst[id.xy] = float4(c, Src.SampleLevel(Smp, uv, 0).a);\n"
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
    "    Dst[id.xy] = float4(c, Src.Load(int3(id.xy, 0)).a);\n"
    "}\n";

std::string joinChunks(const char* const* chunks) {
    std::string out;
    for (const char* const* c = chunks; *c; ++c) out += *c;
    return out;
}

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

FaultBudget g_budget("panelUpscale", 4);

bool     g_sharp = false;
bool     g_failed = false;
uint64_t g_vsHash = kVsHash;
uint32_t g_scale = 2;
float    g_sharpen = 0.25f;      // RCAS stops; negative = off
uint32_t g_wantW = 0, g_wantH = 0;   // an exact size from the ini; 0 = ratio

ID3D11ComputeShader* g_csEasu = nullptr;
ID3D11ComputeShader* g_csRcas = nullptr;
ID3D11SamplerState*  g_smp = nullptr;
ID3D11Buffer*        g_cb = nullptr;

Tex      g_a, g_b;
uint32_t g_srcW = 0, g_srcH = 0, g_outW = 0, g_outH = 0;
ID3D11ShaderResourceView* g_final = nullptr;

bool g_doneThisFrame = false;
bool g_engaged = false;
ID3D11ShaderResourceView* g_displaced = nullptr;
uint64_t g_applied = 0;
uint64_t g_frames = 0;

void standDown(const char* why) {
    if (g_failed) return;
    g_failed = true;
    Log::get().note("holo panels: %s. The panel is sampled the way the game "
                    "samples it for the rest of this session.", why);
}

// Is this the surface we are aimed at? An exact size if the ini gave one,
// otherwise the measured fraction of whatever the eye is -- so the fix
// survives a headset change, which a literal size does not.
bool isTarget(uint32_t w, uint32_t h) {
    if (g_wantW) return w == g_wantW && h == g_wantH;
    uint32_t ew = 0, eh = 0;
    if (!eyeTextureSize(&ew, &eh) || !ew || !eh) return false;
    const float fw = static_cast<float>(w) / static_cast<float>(ew);
    const float fh = static_cast<float>(h) / static_cast<float>(eh);
    return fw > kFracW - kFracTol && fw < kFracW + kFracTol &&
           fh > kFracH - kFracTol && fh < kFracH + kFracTol;
}

bool makeTex(ID3D11Device* dev, uint32_t w, uint32_t h, bool srgb, Tex* t) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h;
    d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &t->tex))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    if (FAILED(dev->CreateUnorderedAccessView(t->tex, &ud, &t->uav))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(t->tex, &sd, &t->read))) return false;
    // The view the GAME gets carries the source view's sRGB-ness, so its own
    // sampler does exactly what it always did.
    sd.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                     : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(dev->CreateShaderResourceView(t->tex, &sd, &t->game))) return false;
    return true;
}

void runPass(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs,
             ID3D11ShaderResourceView* in, ID3D11UnorderedAccessView* out,
             uint32_t w, uint32_t h) {
    // A texture cannot be an SRV and a UAV in the same dispatch, and D3D
    // silently nulls the SRV if asked -- which reads BLACK, not the previous
    // pass. intro_upscale records the same rule.
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 1, &nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &in);
    ctx->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

void writeCb(ID3D11DeviceContext* ctx, const void* data, uint32_t bytes) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, bytes);
        ctx->Unmap(g_cb, 0);
    }
}

// Each piece checked for itself, never "if (g_csEasu) return true": the
// sharpening can be turned on after the first build, and an early return
// would leave RCAS uncompiled while the log said it was on.
bool ensureShaders(ID3D11DeviceContext* ctx, ID3D11Device* dev) {
    if (!g_csEasu) {
        const std::string h = std::string(kGpuPrologue) +
                              joinChunks(kFfxAChunks) +
                              "#define FSR_EASU_F 1\n" +
                              joinChunks(kFfxFsr1Chunks) + kEasuMain;
        g_csEasu = shaderSwapCompileCs(ctx, h.c_str(), h.size(), "main",
                                       "panel easu", nullptr, "holo panels");
        if (!g_csEasu) { standDown("EASU would not compile"); return false; }
    }
    if (g_sharpen >= 0.0f && !g_csRcas) {
        const std::string h = std::string(kGpuPrologue) +
                              joinChunks(kFfxAChunks) +
                              "#define FSR_RCAS_F 1\n" +
                              joinChunks(kFfxFsr1Chunks) + kRcasMain;
        g_csRcas = shaderSwapCompileCs(ctx, h.c_str(), h.size(), "main",
                                       "panel rcas", nullptr, "holo panels");
        if (!g_csRcas) {
            Log::get().note("holo panels: RCAS would not compile; the panel "
                            "is reconstructed without the sharpening pass.");
            g_sharpen = -1.0f;
        }
    }
    if (!g_smp) {
        D3D11_SAMPLER_DESC sm{};
        sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev->CreateSamplerState(&sm, &g_smp))) {
            standDown("the resampler's sampler could not be created");
            return false;
        }
    }
    if (!g_cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 80;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cb))) {
            standDown("the resampler's constant buffer could not be created");
            return false;
        }
    }
    return true;
}

void releaseChain() {
    g_a.release();
    g_b.release();
    g_final = nullptr;
    g_srcW = g_srcH = g_outW = g_outH = 0;
}

// Resample the surface into ours. Once a frame, at the first matched draw.
bool resample(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src,
              uint32_t sw, uint32_t sh, bool srgb) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    if (!ensureShaders(ctx, dev)) { dev->Release(); return false; }

    const uint32_t dw = sw * g_scale, dh = sh * g_scale;
    const bool wantRcas = g_csRcas && g_sharpen >= 0.0f;
    if (!g_a.tex || g_srcW != sw || g_srcH != sh || g_outW != dw ||
        g_outH != dh || (wantRcas && !g_b.tex)) {
        releaseChain();
        bool ok = makeTex(dev, dw, dh, srgb, &g_a);
        if (ok && wantRcas) ok = makeTex(dev, dw, dh, srgb, &g_b);
        if (!ok) {
            releaseChain();
            dev->Release();
            standDown("the resampled panel could not be created");
            return false;
        }
        g_srcW = sw; g_srcH = sh; g_outW = dw; g_outH = dh;
        Log::get().note(
            "holo panels: the %ux%u panel surface is reconstructed to %ux%u "
            "(%ux, EASU%s) every frame, and the game's own draw samples ours. "
            "%.1f MB. It cannot add detail -- the panel's artwork is "
            "rasterised at whatever size the game lays it out at.",
            sw, sh, dw, dh, g_scale, wantRcas ? ", then RCAS" : "",
            static_cast<double>(dw) * dh * 4.0 * (wantRcas ? 2 : 1) / 1048576.0);
    }

    uint32_t con[20] = {0};
    FsrEasuCon(reinterpret_cast<AU1*>(con + 0), reinterpret_cast<AU1*>(con + 4),
               reinterpret_cast<AU1*>(con + 8), reinterpret_cast<AU1*>(con + 12),
               static_cast<AF1>(sw), static_cast<AF1>(sh),
               static_cast<AF1>(sw), static_cast<AF1>(sh),
               static_cast<AF1>(dw), static_cast<AF1>(dh));
    con[16] = dw; con[17] = dh;

    // The compute stage is saved and put back around the whole chain: the
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

    writeCb(ctx, con, sizeof(con));
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    ctx->CSSetSamplers(0, 1, &g_smp);
    runPass(ctx, g_csEasu, src, g_a.uav, dw, dh);
    g_final = g_a.game;
    if (wantRcas) {
        uint32_t rcon[8] = {0};
        FsrRcasCon(reinterpret_cast<AU1*>(rcon), static_cast<AF1>(g_sharpen));
        rcon[4] = dw; rcon[5] = dh;
        writeCb(ctx, rcon, sizeof(rcon));
        runPass(ctx, g_csRcas, g_a.read, g_b.uav, dw, dh);
        g_final = g_b.game;
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
    dev->Release();
    return g_final != nullptr;
}

}  // namespace

void panelUpscaleConfigure(Config& cfg) {
    const bool was = g_sharp;
    const uint32_t wasScale = g_scale;
    const float wasSharpen = g_sharpen;

    const std::string m = cfg.getString("experimental.holo_panels", "stock");
    if (m == "stock") {
        g_sharp = false;
    } else if (m == "sharp") {
        g_sharp = true;
    } else {
        g_sharp = false;
        Log::get().note("holo_panels \"%s\" is not stock or sharp; running "
                        "stock.", m.c_str());
    }

    g_scale = static_cast<uint32_t>(
        cfg.getIntInRange("advanced.holo_panel_scale", 2, 2, 4));

    const std::string s = cfg.getString("advanced.holo_panel_sharpen", "0.25");
    if (s == "off") {
        g_sharpen = -1.0f;
    } else {
        g_sharpen = static_cast<float>(atof(s.c_str()));
        if (g_sharpen < 0.0f) g_sharpen = 0.0f;
        if (g_sharpen > 2.0f) g_sharpen = 2.0f;
    }

    // An exact size, for aiming this at a DIFFERENT panel than the one the
    // ratio picks. Empty is the ratio, which is what survives a headset
    // change.
    const std::string sz = cfg.getString("advanced.holo_panel_size", "");
    uint32_t w = 0, h = 0;
    if (!sz.empty() && sscanf_s(sz.c_str(), "%ux%u", &w, &h) == 2 && w && h) {
        g_wantW = w; g_wantH = h;
    } else {
        if (!sz.empty()) {
            Log::get().note("holo_panel_size \"%s\" is not WIDTHxHEIGHT; the "
                            "measured fraction of the eye is used instead.",
                            sz.c_str());
        }
        g_wantW = g_wantH = 0;
    }

    const std::string pin = cfg.getString("advanced.holo_panel_vs", "");
    if (pin.empty()) {
        g_vsHash = kVsHash;
    } else {
        char* end = nullptr;
        const uint64_t hh = _strtoui64(pin.c_str(), &end, 16);
        if (end && *end == '\0' && hh != 0) {
            g_vsHash = hh;
        } else {
            g_vsHash = kVsHash;
            Log::get().note("holo_panel_vs \"%s\" is not a hex shader hash; "
                            "the measured one is used instead.", pin.c_str());
        }
    }

    if ((g_scale != wasScale || g_sharpen != wasSharpen) && g_a.tex) {
        releaseChain();
        Log::get().note("holo panels: the settings changed, so the chain is "
                        "rebuilt at the next frame -- %ux, sharpening %s.",
                        g_scale, g_sharpen >= 0.0f ? "on" : "off");
    }

    if (was != g_sharp) {
        Log::get().note(
            "holo panels: %s. Every cockpit holo panel is painted by one "
            "shared shader reading a different interface surface; sharp "
            "reconstructs the one carrying the target indicator with AMD's "
            "EASU (%ux, sharpening %s) once a frame and hands the game's own "
            "draw the result. It cannot add detail. Watching for vs %016llX "
            "with a %s surface in slot %u.",
            g_sharp ? "sharp" : "stock", g_scale,
            g_sharpen >= 0.0f ? "on" : "off",
            static_cast<unsigned long long>(g_vsHash),
            g_wantW ? "named" : "measured-fraction", kSlot);
    }
}

bool panelUpscaleWantsDraws() { return g_sharp && !g_failed; }

bool panelUpscaleOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                           uint32_t instances) {
    (void)kind; (void)count; (void)instances;
    if (!panelUpscaleWantsDraws()) return false;
    // Slot 2 first: it is the cheap discriminator, and it is what separates
    // the ONE panel being treated from the twenty-three others the same
    // shader draws.
    ResourceInfo surf;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv2), &surf) ||
        !surf.isTexture2D || !isTarget(surf.a, surf.b)) {
        return false;
    }
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) return false;
    const uint64_t h = lookupShaderHash(vs);
    vs->Release();
    return h == g_vsHash;
}

void panelUpscaleBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    if (g_failed) return;

    ID3D11ShaderResourceView* src = static_cast<ID3D11ShaderResourceView*>(
        bindingGet(BindSlot::PsSrv2));
    if (!src) return;

    ID3D11ShaderResourceView* ours = nullptr;
    guardedBudget(g_budget, [&] {
        if (!g_doneThisFrame) {
            ResourceInfo info;
            if (!bindingResolve(src, &info) || !info.isTexture2D) return;
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            src->GetDesc(&vd);
            const bool srgb =
                vd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                vd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            // Marked done either way: a resample that failed will fail again
            // this frame, and thirty retries would be thirty dispatches.
            g_doneThisFrame = true;
            resample(ctx, src, info.a, info.b, srgb);
        }
        ours = g_final;
    });
    if (!ours) return;   // stock behaviour, which the log explained once

    g_displaced = src;
    ctx->PSSetShaderResources(kSlot, 1, &ours);
    g_engaged = true;

    if (++g_applied == 1) {
        Log::get().note("holo panels: sharp engaged -- the panel draw samples "
                        "our reconstruction for exactly this draw, and the "
                        "game's surface is restored after every one.");
    }
}

void panelUpscaleEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged) return;
    g_engaged = false;
    ID3D11ShaderResourceView* orig = g_displaced;
    g_displaced = nullptr;
    ctx->PSSetShaderResources(kSlot, 1, &orig);
}

void panelUpscaleFrameEnd() {
    g_doneThisFrame = false;
    ++g_frames;
}

void panelUpscaleShutdown() {
    releaseChain();
    if (g_csEasu) { g_csEasu->Release(); g_csEasu = nullptr; }
    if (g_csRcas) { g_csRcas->Release(); g_csRcas = nullptr; }
    if (g_smp) { g_smp->Release(); g_smp = nullptr; }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
}

}  // namespace edvr
