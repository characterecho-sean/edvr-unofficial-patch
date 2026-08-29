#include "scrim_fix.h"

#include <windows.h>

#include <d3d11.h>

#include <string>

#include "../common/config.h"
#include "../common/intro_mode.h"
#include "../common/log.h"
#include "binding_shadow.h"

namespace edvr {
namespace {

// The wash's own texture: sixteen pixels, block-compressed, stretched across
// the whole curved surface. Nothing else in the loader's frame binds a BC1
// that small, which is what makes this the discriminator rather than the
// vertex shader hash.
constexpr uint32_t kWashW = 16;
constexpr uint32_t kWashH = 16;

// The interface surface the wash dims, in slot 1. Only a size FLOOR: the
// census measured 4259x2395 on one rig, but that is a render-scale-dependent
// number and pinning it would switch this fix off silently on anyone else's
// headset -- the mistake holo_fix made in the field on 2026-08-19 and records
// in its own comments.
constexpr uint32_t kUiMinW = 1024;

// A mesh, not a quad. The wash is stretched over the game's curved surface,
// which the census showed at 5760 indices; the floor allows for geometry that
// re-tessellates without letting a six-index HUD quad in.
// The mesh re-tessellates: the field measured 5760 indices under one modal
// and 360 under the next, same shaders and same textures both times. A floor
// of 1000 would have matched the first and silently missed the second.
constexpr char     kKind = 'X';
constexpr uint32_t kMinIndices = 100;
constexpr uint32_t kInstances = 1;

bool isBc1(uint32_t fmt) {
    return fmt == DXGI_FORMAT_BC1_TYPELESS || fmt == DXGI_FORMAT_BC1_UNORM ||
           fmt == DXGI_FORMAT_BC1_UNORM_SRGB;
}

bool     g_on = false;
// 0, and the disassembly is why. From ps 9107E72CB016CC02:
//
//   mad r0.xyzw, r2.xyzw, r0.xxxx, r3.xyzw   ; r0.x, r0.y are the t0 samples
//   add r0.xyzw, r0.xyzw, r1.xyzw            ; + the sharp UI on top
//
// The two samples of t0 MULTIPLY the blurred, desaturated, tinted layer that
// is the wash. White therefore turns it to full strength -- which is what
// this shipped first, and the field duly reported no change for the better.
// Black collapses both terms, leaving r0 = r1: the sharp interface alone,
// with the shader's own discard (all channels < 5/255) throwing the empty
// area away. That is precisely what the no-wash variant 85565E9261812E2F
// does with its own discard, reached by a different route.
uint32_t g_level = 0;        // the uniform's channel value, live-tuned

ID3D11Texture2D*          g_tex = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
uint32_t                  g_texLevel = 0xFFFFFFFFu;
bool                      g_createFailedNoted = false;

bool                      g_engaged = false;
ID3D11ShaderResourceView* g_displaced = nullptr;
uint64_t                  g_applied = 0;

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
    if (SUCCEEDED(hr) && tex) {
        hr = dev->CreateShaderResourceView(tex, nullptr, &srv);
    }
    dev->Release();
    if (FAILED(hr) || !srv) {
        if (tex) tex->Release();
        if (!g_createFailedNoted) {
            g_createFailedNoted = true;
            Log::get().note("loading dim: the uniform texture could not be "
                            "created (0x%08lX); the wash draws stock.",
                            static_cast<unsigned long>(hr));
        }
        return nullptr;
    }
    if (g_srv) g_srv->Release();
    if (g_tex) g_tex->Release();
    g_tex = tex;
    g_srv = srv;
    g_texLevel = g_level;
    return g_srv;
}

}  // namespace

void scrimConfigure(Config& cfg) {
    const bool was = g_on;
    const std::string m = cfg.getString("fix.loading_dim", "screen");
    const LoadingDimMode dm = loadingDimParse(m);
    if (!dm.recognised) {
        Log::get().note("loading_dim \"%s\" is not screen or stock; running "
                        "the default, screen.", m.c_str());
    }
    g_on = dm.washOff;
    g_level = static_cast<uint32_t>(
        cfg.getIntInRange("advanced.loading_dim_level", 0, 0, 255));

    if (was != g_on) {
        Log::get().note(
            "loading dim: %s. The wash the loader's dialog lays over "
            "everything behind it is %s; level %u. Found by diffing two "
            "censuses -- it is a 16x16 texture stretched over the interface "
            "composite, not a draw of its own (docs/loading-scrim.md).",
            g_on ? "OFF" : "stock",
            g_on ? "replaced with a uniform for that one draw"
                 : "the game's own",
            g_level);
    }
}

bool scrimWantsDraws() { return g_on; }

bool scrimOnEyeDraw(char kind, uint32_t count, uint32_t instances) {
    if (!g_on) return false;
    if (kind != kKind || instances != kInstances || count < kMinIndices) {
        return false;
    }
    // Slot 0 first: a 16x16 BC1 is the rare binding, and every other mesh in
    // the frame fails here without paying for a second resolve.
    ResourceInfo wash;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &wash) ||
        !wash.isTexture2D || wash.a != kWashW || wash.b != kWashH ||
        !isBc1(wash.fmt)) {
        return false;
    }
    // Slot 1 must be the interface surface the wash is dimming. Without this
    // the fix would fire on any mesh that happened to carry a small BC1.
    ResourceInfo ui;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv1), &ui) ||
        !ui.isTexture2D || ui.a < kUiMinW) {
        return false;
    }
    return true;
}

void scrimBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    ID3D11ShaderResourceView* uniform = uniformSrv(ctx);
    if (!uniform) return;   // stock behaviour, which the log explained once

    g_displaced = static_cast<ID3D11ShaderResourceView*>(
        bindingGet(BindSlot::PsSrv0));
    // Armed before the substitution: a fault between the two would otherwise
    // leave our texture bound with nothing owing a restore.
    g_engaged = true;
    ctx->PSSetShaderResources(0, 1, &uniform);

    if (++g_applied == 1) {
        Log::get().note("loading dim: OFF engaged -- the wash term is uniform "
                        "level %u for exactly this draw, so the loader's "
                        "dialog sits on undimmed art. The game's texture is "
                        "restored after every draw. Level 0 collapses the "
                        "blur layer the shader multiplies by it; raise it "
                        "towards 255 to dim more, not less.", g_texLevel);
    }
}

void scrimEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged) return;
    g_engaged = false;
    ID3D11ShaderResourceView* orig = g_displaced;
    g_displaced = nullptr;
    ctx->PSSetShaderResources(0, 1, &orig);
}

void scrimShutdown() {
    if (g_srv) { g_srv->Release(); g_srv = nullptr; }
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    g_texLevel = 0xFFFFFFFFu;
}

}  // namespace edvr
