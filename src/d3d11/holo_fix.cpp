#include "holo_fix.h"

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "binding_shadow.h"

namespace edvr {
namespace {

// The hologram composite's shape, as the loading-screen census measured it
// and the field suppression verified it (2026-08-19): a 6-index instanced
// quad whose PS slot 0 resolves to the eye-sized depth texture and slot 1
// to the 256x256 fmt-70 pattern being neutralised.
constexpr char     kKind = 'X';
constexpr uint32_t kIndices = 6;
constexpr uint32_t kInstances = 1;
constexpr uint32_t kPatternW = 256;
constexpr uint32_t kPatternH = 256;
constexpr uint32_t kPatternFmt = 70;   // BC1-class, as the census resolves it

bool    g_steady = false;
uint32_t g_level = 255;      // the uniform's channel value, live-tuned

// The substitute: a 1x1 immutable texture at g_level, rebuilt when the level
// changes. One texture and one SRV for the session otherwise.
ID3D11Texture2D*          g_tex = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
uint32_t                  g_texLevel = 0xFFFFFFFFu;
bool                      g_createFailedNoted = false;

// What begin displaced, for end to put back. The game's own SRV pointer
// comes from binding_shadow, which every PSSetShaderResources call feeds.
bool                      g_engaged = false;
ID3D11ShaderResourceView* g_displaced = nullptr;

uint64_t g_applied = 0;

ID3D11ShaderResourceView* uniformSrv(ID3D11DeviceContext* ctx) {
    if (g_srv && g_texLevel == g_level) return g_srv;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;

    const uint8_t v = static_cast<uint8_t>(g_level > 255 ? 255 : g_level);
    const uint8_t pixel[4] = {v, v, v, 255};
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1;
    td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = pixel;
    init.SysMemPitch = 4;

    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = dev->CreateTexture2D(&td, &init, &tex);
    if (SUCCEEDED(hr) && tex) hr = dev->CreateShaderResourceView(tex, nullptr, &srv);
    dev->Release();
    if (FAILED(hr) || !srv) {
        if (tex) tex->Release();
        if (!g_createFailedNoted) {
            g_createFailedNoted = true;
            Log::get().note("holo pattern: could not create the uniform "
                            "texture (hr=0x%08X); the hologram draws stock.",
                            static_cast<unsigned>(hr));
        }
        return nullptr;
    }
    if (g_srv) g_srv->Release();
    if (g_tex) g_tex->Release();
    g_srv = srv;
    g_tex = tex;
    g_texLevel = g_level;
    return g_srv;
}

}  // namespace

void holoConfigure(Config& cfg) {
    const bool was = g_steady;
    // steady by default since the field verification (2026-08-19): shimmer
    // gone, ship normal, level 255 correct first try -- which also proved
    // the shader multiplies the term, so identity is white.
    const std::string m = cfg.getString("fix.holo_pattern", "steady");
    if (m == "stock") {
        g_steady = false;
    } else if (m == "steady") {
        g_steady = true;
    } else {
        g_steady = false;
        Log::get().note("holo_pattern \"%s\" is not stock or steady; running "
                        "stock.", m.c_str());
    }
    int level = cfg.getIntInRange("advanced.holo_pattern_level", 255, 0, 255);
    g_level = static_cast<uint32_t>(level);

    if (was != g_steady) {
        Log::get().note("holo pattern: %s. The loading hologram's screen-space "
                        "pattern (the head-locked shimmer inside the ship's "
                        "silhouette) is %s; level %u.",
                        g_steady ? "steady" : "stock",
                        g_steady ? "replaced with a uniform for that one draw"
                                 : "the game's own",
                        g_level);
    }
}

bool holoWantsDraws() { return g_steady; }

bool holoOnEyeDraw(char kind, uint32_t count, uint32_t instances) {
    if (!g_steady) return false;
    if (kind != kKind || count != kIndices || instances != kInstances) {
        return false;
    }
    // Slot 1 first: the 256x256 pattern is the cheaper resolve and the rarer
    // binding; most HUD quads fail here without touching slot 0.
    ResourceInfo pattern;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv1), &pattern) ||
        !pattern.isTexture2D || pattern.a != kPatternW ||
        pattern.b != kPatternH || pattern.fmt != kPatternFmt) {
        return false;
    }
    // Slot 0 must be the eye-sized depth resolve -- the discriminator that
    // separates the hologram composite from an atlas-sampling HUD quad that
    // happens to carry a 256x256 in slot 1.
    uint32_t eyeW = 0, eyeH = 0;
    if (!eyeTextureSize(&eyeW, &eyeH)) return false;
    ResourceInfo depth;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &depth) ||
        !depth.isTexture2D || depth.a != eyeW || depth.b != eyeH) {
        return false;
    }
    return true;
}

void holoBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    ID3D11ShaderResourceView* uniform = uniformSrv(ctx);
    if (!uniform) return;   // stock behaviour, which the log explained once

    g_displaced = static_cast<ID3D11ShaderResourceView*>(
        bindingGet(BindSlot::PsSrv1));
    ctx->PSSetShaderResources(1, 1, &uniform);
    g_engaged = true;

    if (++g_applied == 1) {
        Log::get().note("holo pattern: steady engaged -- the hologram's "
                        "pattern term is uniform level %u for exactly this "
                        "draw, so the ship holds still under your head. The "
                        "game's texture is restored after every draw.",
                        g_texLevel);
    }
}

void holoEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged) return;
    g_engaged = false;
    // Restore what the game had bound, as binding_shadow last saw it. Null
    // restores an unbind, which is also the truth.
    ID3D11ShaderResourceView* orig = g_displaced;
    g_displaced = nullptr;
    ctx->PSSetShaderResources(1, 1, &orig);
}

void holoShutdown() {
    if (g_srv) {
        g_srv->Release();
        g_srv = nullptr;
    }
    if (g_tex) {
        g_tex->Release();
        g_tex = nullptr;
    }
    g_texLevel = 0xFFFFFFFFu;
}

}  // namespace edvr
