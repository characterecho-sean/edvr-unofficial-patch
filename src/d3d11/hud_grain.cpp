#include "hud_grain.h"

#include <windows.h>

#include <d3d11.h>

#include <cstdlib>
#include <string>

#include "../common/config.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "exposure_fix.h"   // lookupShaderHash
#include "vscreen.h"        // vScreenIsEyeSized

namespace edvr {
namespace {

// The noise table, as the census resolves it: 256x256, B8G8R8A8-class.
constexpr uint32_t kNoiseW = 256;
constexpr uint32_t kNoiseH = 256;

// vs B7790CBFC6554097 on game build 332753 -- the flight HUD's vector
// family. Pinnable, because a game update can recompile a shader without
// changing what it does.
constexpr uint64_t kVsHash = 0xB7790CBFC6554097ull;

bool     g_steady = false;
uint32_t g_level = 255;
uint64_t g_vsHash = kVsHash;

ID3D11Texture2D*          g_tex = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
uint32_t                  g_texLevel = 0xFFFFFFFFu;
bool                      g_createFailedNoted = false;

bool                      g_engaged = false;
ID3D11ShaderResourceView* g_displaced = nullptr;
uint64_t                  g_applied = 0;

// The substitute: a 1x1 immutable texture at g_level, rebuilt when the level
// changes. Every octave then reads the same value, so the noise term is a
// constant. holo_fix's texture exactly.
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
            Log::get().note("hud grain: could not create the uniform texture "
                            "(hr=0x%08X); the HUD draws stock.",
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

void hudGrainConfigure(Config& cfg) {
    const bool was = g_steady;
    const std::string m = cfg.getString("experimental.hud_grain", "stock");
    if (m == "stock") {
        g_steady = false;
    } else if (m == "steady") {
        g_steady = true;
    } else {
        g_steady = false;
        Log::get().note("hud_grain \"%s\" is not stock or steady; running "
                        "stock.", m.c_str());
    }
    g_level = static_cast<uint32_t>(
        cfg.getIntInRange("advanced.hud_grain_level", 255, 0, 255));

    const std::string pin = cfg.getString("advanced.hud_grain_vs", "");
    if (pin.empty()) {
        g_vsHash = kVsHash;
    } else {
        char* end = nullptr;
        const uint64_t h = _strtoui64(pin.c_str(), &end, 16);
        if (end && *end == '\0' && h != 0) {
            g_vsHash = h;
        } else {
            g_vsHash = kVsHash;
            Log::get().note("hud_grain_vs \"%s\" is not a hex shader hash; "
                            "the measured one is used instead.", pin.c_str());
        }
    }

    if (was != g_steady) {
        Log::get().note(
            "hud grain: %s. The flight HUD is vector geometry drawn at full "
            "eye resolution -- there is nothing in it to upscale -- but its "
            "pixel shader lays three octaves of value noise over it from a "
            "256x256 table. steady replaces that table with a uniform at "
            "level %u for those draws, so the shimmer becomes a constant and "
            "the glyph edges are left alone. The level is a taste control, "
            "not a correct value. Watching for vs %016llX.",
            g_steady ? "steady" : "stock", g_level,
            static_cast<unsigned long long>(g_vsHash));
    }
}

bool hudGrainWantsDraws() { return g_steady; }

bool hudGrainOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances) {
    (void)kind; (void)count; (void)instances;
    if (!g_steady) return false;
    // Slot 1 first: the 256x256 is the cheaper resolve and the rarer
    // binding, so most HUD draws fail here without touching slot 0.
    ResourceInfo noise;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv1), &noise) ||
        !noise.isTexture2D || noise.a != kNoiseW || noise.b != kNoiseH) {
        return false;
    }
    // Slot 0 must be the eye-sized depth resolve. Eye-sized by vScreen's
    // answer rather than an equality against the published size -- the
    // lesson holo_fix paid for on a rig with a render scale.
    ResourceInfo depth;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &depth) ||
        !depth.isTexture2D || !vScreenIsEyeSized(depth.a, depth.b)) {
        return false;
    }
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) return false;
    const uint64_t h = lookupShaderHash(vs);
    vs->Release();
    return h == g_vsHash;
}

void hudGrainBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    ID3D11ShaderResourceView* uniform = uniformSrv(ctx);
    if (!uniform) return;   // stock behaviour, which the log explained once

    g_displaced = static_cast<ID3D11ShaderResourceView*>(
        bindingGet(BindSlot::PsSrv1));
    ctx->PSSetShaderResources(1, 1, &uniform);
    g_engaged = true;

    if (++g_applied == 1) {
        Log::get().note("hud grain: steady engaged -- the HUD's noise table "
                        "is a uniform at level %u for exactly these draws, "
                        "and the game's texture is restored after every one.",
                        g_texLevel);
    }
}

void hudGrainEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged) return;
    g_engaged = false;
    // Restore what the game had bound, as binding_shadow last saw it. Null
    // restores an unbind, which is also the truth.
    ID3D11ShaderResourceView* orig = g_displaced;
    g_displaced = nullptr;
    ctx->PSSetShaderResources(1, 1, &orig);
}

void hudGrainShutdown() {
    if (g_srv) { g_srv->Release(); g_srv = nullptr; }
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    g_texLevel = 0xFFFFFFFFu;
}

}  // namespace edvr
