#include "hud_sprite.h"

#include <windows.h>

#include <d3d11.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "exposure_fix.h"   // lookupShaderHash
#include "shader_swap.h"

// AMD's FSR, CPU side: A_CPU gives FsrEasuCon and FsrRcasCon, the same
// constants the shader below consumes. Unmodified sources; see
// src/d3d11/fsr/README.md.
#define A_CPU 1
#include "fsr/ffx_a.h"
#include "fsr/ffx_fsr1.h"

#include "fsr_hlsl_gen.h"   // the same two files, as HLSL string chunks

namespace edvr {
namespace {

// The quad's shape, from the bisection that named it.
constexpr char     kKind = 'N';
constexpr uint32_t kVerts = 6;
constexpr uint32_t kInstances = 1;

// vs 5DA53D8B0133341E on game build 332753. Matching on the VERTEX shader
// alone is deliberate: two draws share it (#107 and #108 in the census that
// found this), they sample the same atlas, and both are HUD widgets that
// want the same treatment. Pinnable from the ini for a build that
// recompiles it.
constexpr uint64_t kVsHash = 0x5DA53D8B0133341Eull;

// An atlas, not a render target. Anything larger than this is not the kind
// of thing this fix is for, and refusing keeps a surprise binding from
// costing hundreds of megabytes.
constexpr uint32_t kMaxSrc = 2048;

// Enough atlases for the HUD several times over. Entries are keyed by the
// source RESOURCE and never evicted: the game holds these for the session,
// and a full table simply stops upscaling new ones.
constexpr uint32_t kMaxAtlas = 8;

const char kGpuPrologue[] =
    "#define A_GPU 1\n"
    "#define A_HLSL 1\n";

// EASU and RCAS, wrapped in the callbacks AMD's header asks for. Identical
// in shape to intro_upscale's, because it is the same job: resample a
// texture we do not own into one we do.
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
    "    // ALPHA MATTERS HERE, unlike the intro movie. These are cut-out\n"
    "    // sprites on a transparent atlas, and EASU has no alpha path -- so\n"
    "    // alpha is resampled with a plain bilinear tap at the same place.\n"
    "    float2 uv = (float2(id.xy) + 0.5) / float2(dstSize);\n"
    "    float a = Src.SampleLevel(Smp, uv, 0).a;\n"
    "    Dst[id.xy] = float4(c, a);\n"
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
    ID3D11ShaderResourceView*  read = nullptr;   // non-sRGB, for RCAS input
    ID3D11ShaderResourceView*  game = nullptr;   // what the draw samples
    void release() {
        if (uav) { uav->Release(); uav = nullptr; }
        if (read) { read->Release(); read = nullptr; }
        if (game) { game->Release(); game = nullptr; }
        if (tex) { tex->Release(); tex = nullptr; }
    }
};

struct Atlas {
    void*                     src = nullptr;   // the game's texture, by identity
    ID3D11ShaderResourceView* final = nullptr; // ours, bound in its place
    Tex                       a, b;
    bool                      failed = false;  // tried once, do not retry
};

FaultBudget g_budget("hudSprite", 4);

bool     g_sharp = false;
bool     g_failed = false;
uint64_t g_vsHash = kVsHash;
uint32_t g_scale = 2;
float    g_sharpen = 0.25f;   // RCAS stops; negative = off

ID3D11ComputeShader* g_csEasu = nullptr;
ID3D11ComputeShader* g_csRcas = nullptr;
ID3D11SamplerState*  g_smp = nullptr;
ID3D11Buffer*        g_cb = nullptr;

Atlas    g_atlas[kMaxAtlas];
uint32_t g_atlasCount = 0;

bool                      g_engaged = false;
ID3D11ShaderResourceView* g_displaced = nullptr;
uint64_t                  g_applied = 0;

void standDown(const char* why) {
    if (g_failed) return;
    g_failed = true;
    Log::get().note("hud icons: %s. The HUD's atlases are sampled the way "
                    "the game samples them for the rest of this session.",
                    why);
}

bool makeTex(ID3D11Device* dev, uint32_t w, uint32_t h, bool srgb, Tex* t) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
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
    // sampler does exactly what it always did -- backdrop_fix's rule, and the
    // reason its deband did not shift the picture's brightness.
    sd.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                     : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(dev->CreateShaderResourceView(t->tex, &sd, &t->game))) return false;
    return true;
}

void runPass(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs,
             ID3D11ShaderResourceView* in, ID3D11UnorderedAccessView* out,
             uint32_t w, uint32_t h) {
    // A texture cannot be an SRV and a UAV in the same dispatch, and D3D
    // silently nulls the SRV if asked -- which reads BLACK rather than the
    // previous pass. intro_upscale records the same rule.
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

// Compile once, for the session. False means stood down.
bool ensureShaders(ID3D11DeviceContext* ctx, ID3D11Device* dev) {
    if (g_csEasu) return true;
    const std::string easu = std::string(kGpuPrologue) +
                             joinChunks(kFfxAChunks) +
                             "#define FSR_EASU_F 1\n" +
                             joinChunks(kFfxFsr1Chunks) + kEasuMain;
    g_csEasu = shaderSwapCompileCs(ctx, easu.c_str(), easu.size(), "main",
                                   "hud sprite easu", nullptr, "hud icons");
    if (!g_csEasu) {
        standDown("EASU would not compile");
        return false;
    }
    if (g_sharpen >= 0.0f && !g_csRcas) {
        const std::string rcas = std::string(kGpuPrologue) +
                                 joinChunks(kFfxAChunks) +
                                 "#define FSR_RCAS_F 1\n" +
                                 joinChunks(kFfxFsr1Chunks) + kRcasMain;
        g_csRcas = shaderSwapCompileCs(ctx, rcas.c_str(), rcas.size(), "main",
                                       "hud sprite rcas", nullptr, "hud icons");
        if (!g_csRcas) {
            Log::get().note("hud icons: RCAS would not compile; the atlas is "
                            "upscaled without the sharpening pass.");
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
        bd.ByteWidth = 80;   // four uint4 plus a uint2, rounded to 16
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

Atlas* find(void* src) {
    for (uint32_t i = 0; i < g_atlasCount; ++i) {
        if (g_atlas[i].src == src) return &g_atlas[i];
    }
    return nullptr;
}

// Build the upscaled copy of one atlas. Runs ONCE per source texture.
Atlas* build(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srcSrv,
             void* srcRes, uint32_t sw, uint32_t sh, bool srgb) {
    if (g_atlasCount >= kMaxAtlas) return nullptr;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    if (!ensureShaders(ctx, dev)) { dev->Release(); return nullptr; }

    Atlas& at = g_atlas[g_atlasCount];
    at.src = srcRes;
    const uint32_t dw = sw * g_scale, dh = sh * g_scale;

    bool ok = makeTex(dev, dw, dh, srgb, &at.a);
    const bool wantRcas = ok && g_csRcas && g_sharpen >= 0.0f;
    if (wantRcas) ok = makeTex(dev, dw, dh, srgb, &at.b);
    if (!ok) {
        at.a.release();
        at.b.release();
        at.failed = true;
        ++g_atlasCount;
        Log::get().note("hud icons: a %ux%u atlas could not be given a %ux%u "
                        "copy; it is sampled stock.", sw, sh, dw, dh);
        dev->Release();
        return &at;
    }

    uint32_t con[20] = {0};
    FsrEasuCon(reinterpret_cast<AU1*>(con + 0), reinterpret_cast<AU1*>(con + 4),
               reinterpret_cast<AU1*>(con + 8), reinterpret_cast<AU1*>(con + 12),
               static_cast<AF1>(sw), static_cast<AF1>(sh),
               static_cast<AF1>(sw), static_cast<AF1>(sh),
               static_cast<AF1>(dw), static_cast<AF1>(dh));
    con[16] = dw;
    con[17] = dh;

    // The compute stage is saved and put back around the whole build: the
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
    runPass(ctx, g_csEasu, srcSrv, at.a.uav, dw, dh);
    at.final = at.a.game;

    if (wantRcas) {
        uint32_t rcon[8] = {0};
        FsrRcasCon(reinterpret_cast<AU1*>(rcon), static_cast<AF1>(g_sharpen));
        rcon[4] = dw;
        rcon[5] = dh;
        writeCb(ctx, rcon, sizeof(rcon));
        runPass(ctx, g_csRcas, at.a.read, at.b.uav, dw, dh);
        at.final = at.b.game;
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

    ++g_atlasCount;
    Log::get().note(
        "hud icons: a %ux%u atlas was resampled to %ux%u (%ux, EASU%s) and "
        "the game's own draw now samples ours. Done once; %.1f MB. It cannot "
        "add detail -- the artwork has as many texels as it was authored "
        "with, however it is filtered.",
        sw, sh, dw, dh, g_scale, wantRcas ? ", then RCAS" : "",
        static_cast<double>(dw) * dh * 4.0 * (wantRcas ? 2 : 1) / 1048576.0);
    dev->Release();
    return &at;
}

}  // namespace

void hudSpriteConfigure(Config& cfg) {
    const bool was = g_sharp;
    const std::string m = cfg.getString("experimental.hud_icons", "stock");
    if (m == "stock") {
        g_sharp = false;
    } else if (m == "sharp") {
        g_sharp = true;
    } else {
        g_sharp = false;
        Log::get().note("hud_icons \"%s\" is not stock or sharp; running "
                        "stock.", m.c_str());
    }

    g_scale = static_cast<uint32_t>(
        cfg.getIntInRange("advanced.hud_icon_scale", 2, 2, 4));

    const std::string s = cfg.getString("advanced.hud_icon_sharpen", "0.25");
    if (s == "off") {
        g_sharpen = -1.0f;
    } else {
        g_sharpen = static_cast<float>(atof(s.c_str()));
        if (g_sharpen < 0.0f) g_sharpen = 0.0f;
        if (g_sharpen > 2.0f) g_sharpen = 2.0f;
    }

    const std::string pin = cfg.getString("advanced.hud_icon_vs", "");
    if (pin.empty()) {
        g_vsHash = kVsHash;
    } else {
        char* end = nullptr;
        const uint64_t h = _strtoui64(pin.c_str(), &end, 16);
        if (end && *end == '\0' && h != 0) {
            g_vsHash = h;
        } else {
            g_vsHash = kVsHash;
            Log::get().note("hud_icon_vs \"%s\" is not a hex shader hash; the "
                            "measured one is used instead.", pin.c_str());
        }
    }

    if (was != g_sharp) {
        Log::get().note(
            "hud icons: %s. The target direction indicator is drawn by one "
            "six-vertex quad per eye sampling a fixed-size sprite atlas -- "
            "which is why it never sharpened when render resolution rose. "
            "sharp resamples that atlas ONCE with AMD's EASU (%ux, sharpening "
            "%s) and hands the game's own draw the bigger copy. Watching for "
            "vs %016llX.",
            g_sharp ? "sharp" : "stock", g_scale,
            g_sharpen >= 0.0f ? "on" : "off",
            static_cast<unsigned long long>(g_vsHash));
    }
}

bool hudSpriteWantsDraws() { return g_sharp && !g_failed; }

bool hudSpriteOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                        uint32_t instances) {
    if (!hudSpriteWantsDraws()) return false;
    if (kind != kKind || count != kVerts || instances != kInstances) {
        return false;
    }
    ResourceInfo src;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &src) ||
        !src.isTexture2D || src.a == 0 || src.b == 0 ||
        src.a > kMaxSrc || src.b > kMaxSrc) {
        return false;
    }
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) return false;
    const uint64_t h = lookupShaderHash(vs);
    vs->Release();
    return h == g_vsHash;
}

void hudSpriteBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    if (g_failed) return;

    ID3D11ShaderResourceView* srcSrv = static_cast<ID3D11ShaderResourceView*>(
        bindingGet(BindSlot::PsSrv0));
    if (!srcSrv) return;

    ResourceInfo info;
    if (!bindingResolve(srcSrv, &info) || !info.isTexture2D) return;

    ID3D11ShaderResourceView* ours = nullptr;
    guardedBudget(g_budget, [&] {
        Atlas* at = find(info.resource);
        if (!at) {
            // The source view's own format decides whether the game's copy
            // is sRGB; ours must match or the picture shifts.
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            srcSrv->GetDesc(&vd);
            const bool srgb =
                vd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                vd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                vd.Format == DXGI_FORMAT_BC7_UNORM_SRGB ||
                vd.Format == DXGI_FORMAT_BC1_UNORM_SRGB ||
                vd.Format == DXGI_FORMAT_BC2_UNORM_SRGB ||
                vd.Format == DXGI_FORMAT_BC3_UNORM_SRGB;
            at = build(ctx, srcSrv, info.resource, info.a, info.b, srgb);
        }
        if (at && !at->failed) ours = at->final;
    });
    if (!ours) return;   // stock behaviour, which the log explained once

    g_displaced = srcSrv;
    ctx->PSSetShaderResources(0, 1, &ours);
    g_engaged = true;

    if (++g_applied == 1) {
        Log::get().note("hud icons: sharp engaged -- the atlas quad samples "
                        "our resampled copy for exactly this draw, and the "
                        "game's texture is restored after every one.");
    }
}

void hudSpriteEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged) return;
    g_engaged = false;
    ID3D11ShaderResourceView* orig = g_displaced;
    g_displaced = nullptr;
    // Null restores an unbind, which is also the truth.
    ctx->PSSetShaderResources(0, 1, &orig);
}

void hudSpriteShutdown() {
    for (uint32_t i = 0; i < kMaxAtlas; ++i) {
        g_atlas[i].a.release();
        g_atlas[i].b.release();
        g_atlas[i].src = nullptr;
        g_atlas[i].final = nullptr;
    }
    g_atlasCount = 0;
    if (g_csEasu) { g_csEasu->Release(); g_csEasu = nullptr; }
    if (g_csRcas) { g_csRcas->Release(); g_csRcas = nullptr; }
    if (g_smp) { g_smp->Release(); g_smp = nullptr; }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
}

}  // namespace edvr
