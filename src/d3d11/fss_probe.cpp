#include "fss_probe.h"

#include <cstring>   // memcpy for the spec change-detection buffer

#include <windows.h>

#include <d3d11.h>

#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "exposure_fix.h"   // lookupShaderHash

namespace edvr {
namespace {

// The body composite, as every capture since round two has named it.
constexpr uint64_t kCompositeHash = 0x953C8123AD8DC13Bull;

// R11G11B10_FLOAT, the format all four probed slots were measured to
// resolve to (fmt 26). 1.0 in an 11-bit float is exponent 15, mantissa 0.
constexpr uint32_t kFmtR11G11B10 = 26;
constexpr uint32_t kOne11 = 15u << 6;    // R and G channels
constexpr uint32_t kOne10 = 15u << 5;    // B channel

enum class ProbeColor : uint8_t { kBlack, kWhite, kMagenta };

uint32_t packedColor(ProbeColor c) {
    switch (c) {
        case ProbeColor::kBlack:   return 0u;
        case ProbeColor::kWhite:   return kOne11 | (kOne11 << 11) | (kOne10 << 22);
        case ProbeColor::kMagenta: return kOne11 | (kOne10 << 22);
    }
    return 0u;
}

bool       g_armed = false;
bool       g_depthMode = false;   // "depth": disable the depth test for the
                                  // composite instead of touching a slot --
                                  // the one per-eye input (the depth buffer)
                                  // no slot probe can reach
uint32_t   g_slot = 0;
ProbeColor g_color = ProbeColor::kMagenta;
char       g_spec[48] = {};

// The no-test state for depth mode, created once.
ID3D11DepthStencilState* g_noDepth = nullptr;
ID3D11DepthStencilState* g_savedDepth = nullptr;
UINT                     g_savedStencilRef = 0;
bool                     g_depthEngaged = false;

// The substitute, rebuilt when the displaced texture's size or the asked
// colour changes. Full-size, not 1x1: a Load() with texel coordinates
// returns zero out of bounds, and a probe that can silently read as black
// is the exact ambiguity the magenta control exists to remove.
ID3D11Texture2D*          g_tex = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
uint32_t g_texW = 0, g_texH = 0;
ProbeColor g_texColor = ProbeColor::kBlack;
bool g_createFailedNoted = false;
bool g_fmtRefusedNoted = false;

bool                      g_engaged = false;
ID3D11ShaderResourceView* g_displaced = nullptr;

uint64_t g_applied = 0;
bool     g_engagedNoted = false;

FaultBudget g_budget("fssProbe", 8);

ID3D11ShaderResourceView* uniformSrv(ID3D11DeviceContext* ctx, uint32_t w,
                                     uint32_t h) {
    if (g_srv && g_texW == w && g_texH == h && g_texColor == g_color) {
        return g_srv;
    }

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;

    std::vector<uint32_t> texels(static_cast<size_t>(w) * h,
                                 packedColor(g_color));
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = texels.data();
    init.SysMemPitch = w * sizeof(uint32_t);

    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = dev->CreateTexture2D(&td, &init, &tex);
    if (SUCCEEDED(hr) && tex) hr = dev->CreateShaderResourceView(tex, nullptr, &srv);
    dev->Release();
    if (FAILED(hr) || !srv) {
        if (tex) tex->Release();
        if (!g_createFailedNoted) {
            g_createFailedNoted = true;
            Log::get().note("fss probe: could not create the %ux%u substitute "
                            "(hr=0x%08X); the composite draws stock.",
                            w, h, static_cast<unsigned>(hr));
        }
        return nullptr;
    }
    if (g_srv) g_srv->Release();
    if (g_tex) g_tex->Release();
    g_srv = srv;
    g_tex = tex;
    g_texW = w;
    g_texH = h;
    g_texColor = g_color;
    return g_srv;
}

}  // namespace

void fssProbeConfigure(Config& cfg) {
    const std::string spec =
        cfg.getString("advanced.fss_composite_probe", "");
    if (spec.length() >= sizeof(g_spec) || spec == g_spec) return;
    memcpy(g_spec, spec.c_str(), spec.length() + 1);

    const bool wasArmed = g_armed;
    const uint64_t had = g_applied;
    g_armed = false;
    g_depthMode = false;
    g_engagedNoted = false;
    if (spec == "depth") {
        g_armed = true;
        g_depthMode = true;
        Log::get().note(
            "fss probe ARMED: the body composite draws with its DEPTH TEST "
            "OFF. The depth buffer is the one per-eye input no slot probe "
            "reaches -- if the one-eye squares vanish under this, they are "
            "composite pixels being CULLED by per-eye depth content, not "
            "painted by anything. Clear the setting to restore.");
        return;
    }
    if (!spec.empty()) {
        bool ok = spec.length() >= 3 && spec[0] >= '0' && spec[0] <= '3' &&
                  spec[1] == ':';
        if (ok) {
            const std::string c = spec.substr(2);
            if (c == "magenta") {
                g_color = ProbeColor::kMagenta;
            } else if (c == "black") {
                g_color = ProbeColor::kBlack;
            } else if (c == "white") {
                g_color = ProbeColor::kWhite;
            } else {
                ok = false;
            }
        }
        if (!ok) {
            Log::get().note(
                "fss probe: \"%s\" is not SLOT:COLOUR with slot 0-3 and "
                "colour magenta, black or white; refused.",
                spec.c_str());
            return;
        }
        g_slot = static_cast<uint32_t>(spec[0] - '0');
        g_armed = true;
        Log::get().note(
            "fss probe ARMED: the body composite's PS slot %u is replaced "
            "with flat %s for exactly that draw. Zoom a body and look; "
            "magenta appearing NOWHERE means the slot is invisible and a "
            "null on it means nothing. Clear the setting to restore.",
            g_slot, spec.c_str() + 2);
    } else if (wasArmed) {
        Log::get().note("fss probe: cleared (%llu draw(s) were substituted "
                        "while it was set).",
                        static_cast<unsigned long long>(had));
    }
}

bool fssProbeWants() { return g_armed; }

bool fssProbeOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances) {
    if (!g_armed || kind != 'N' || count != 6 || instances != 1 || !ctx) {
        return false;
    }
    uint64_t h = 0;
    guardedBudget(g_budget, [&] {
        ID3D11VertexShader* vs = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        h = lookupShaderHash(vs);
        if (vs) vs->Release();
    });
    return h == kCompositeHash;
}

void fssProbeBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    g_depthEngaged = false;
    if (!ctx || !g_armed) return;
    if (g_depthMode) {
        guardedBudget(g_budget, [&] {
            if (!g_noDepth) {
                ID3D11Device* dev = nullptr;
                ctx->GetDevice(&dev);
                if (!dev) return;
                D3D11_DEPTH_STENCIL_DESC dd{};
                dd.DepthEnable = FALSE;
                dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
                dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
                dev->CreateDepthStencilState(&dd, &g_noDepth);
                dev->Release();
                if (!g_noDepth) return;
            }
            ctx->OMGetDepthStencilState(&g_savedDepth, &g_savedStencilRef);
            ctx->OMSetDepthStencilState(g_noDepth, 0);
            g_depthEngaged = true;
            ++g_applied;
            if (!g_engagedNoted) {
                g_engagedNoted = true;
                Log::get().note("fss probe: engaged -- the composite's depth "
                                "test is off, restored after every draw.");
            }
        });
        return;
    }
    guardedBudget(g_budget, [&] {
        const BindSlot slot = static_cast<BindSlot>(
            static_cast<uint32_t>(BindSlot::PsSrv0) + g_slot);
        void* bound = bindingGet(slot);
        ResourceInfo info;
        if (!bindingResolve(bound, &info) || !info.isTexture2D) return;
        if (info.fmt != kFmtR11G11B10) {
            // The 6x1 strip at s0 is typeless and its view format is not
            // knowable from the desc; substituting blind would probe the
            // wrong question.
            if (!g_fmtRefusedNoted) {
                g_fmtRefusedNoted = true;
                Log::get().note(
                    "fss probe: slot %u resolves to fmt=%u, not the "
                    "R11G11B10 family this probe builds; refusing to "
                    "substitute it. Slots 1-3 are the measured targets.",
                    g_slot, info.fmt);
            }
            return;
        }
        ID3D11ShaderResourceView* sub = uniformSrv(ctx, info.a, info.b);
        if (!sub) return;

        g_displaced = static_cast<ID3D11ShaderResourceView*>(bound);
        ctx->PSSetShaderResources(g_slot, 1, &sub);
        g_engaged = true;
        ++g_applied;
        if (!g_engagedNoted) {
            g_engagedNoted = true;
            Log::get().note(
                "fss probe: engaged -- slot %u (%ux%u) is flat for the "
                "composite, restored after every draw.",
                g_slot, info.a, info.b);
        }
    });
}

void fssProbeEnd(ID3D11DeviceContext* ctx) {
    if (g_depthEngaged && ctx) {
        g_depthEngaged = false;
        ctx->OMSetDepthStencilState(g_savedDepth, g_savedStencilRef);
        if (g_savedDepth) {
            g_savedDepth->Release();
            g_savedDepth = nullptr;
        }
    }
    if (!g_engaged || !ctx) return;
    g_engaged = false;
    ID3D11ShaderResourceView* orig = g_displaced;
    g_displaced = nullptr;
    ctx->PSSetShaderResources(g_slot, 1, &orig);
}

void fssProbeShutdown() {
    if (g_srv) {
        g_srv->Release();
        g_srv = nullptr;
    }
    if (g_tex) {
        g_tex->Release();
        g_tex = nullptr;
    }
    g_texW = 0;
    g_texH = 0;
    if (g_noDepth) {
        g_noDepth->Release();
        g_noDepth = nullptr;
    }
}

}  // namespace edvr
