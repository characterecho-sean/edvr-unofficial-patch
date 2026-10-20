#include <windows.h>
#include "flat_game_exposure.h"

#include <cstring>

#include "../common/log.h"
#include "flat_compute_readback.h"

namespace edvr {
namespace {

// CS F1FB2EEF, the exposure shader (gpu_pass_profile.cpp, eye-brightness.md). Its u1 is the 6x1
// R32 strip. Texel 0 is what VS 2D78DC3F samples and passes on; exposure_fix.cpp's measured layout
// names that texel raw luminance and texel 2 the gain. The tonemap is given texel 0, so that is
// the value the backends multiply by. The 5 s line prints all six so a flight can tell a luminance
// from a multiplier without another instrument.
constexpr uint32_t kStripWidth = 6;

ID3D11Texture2D* g_strip = nullptr;
ID3D11Texture2D* g_texel = nullptr;
ID3D11ShaderResourceView* g_texelSrv = nullptr;
ID3D11Texture2D* g_staging[2] = {};
uint32_t g_stage = 0;
bool g_stageCopied[2] = {};
bool g_trusted = false;
bool g_heldNoted = false;
bool g_shapeNoted = false;
uint64_t g_lastLogMs = 0;

void releaseAll() {
    if (g_strip) { g_strip->Release(); g_strip = nullptr; }
    if (g_texelSrv) { g_texelSrv->Release(); g_texelSrv = nullptr; }
    if (g_texel) { g_texel->Release(); g_texel = nullptr; }
    for (ID3D11Texture2D*& s : g_staging) if (s) { s->Release(); s = nullptr; }
    g_stageCopied[0] = g_stageCopied[1] = false;
    g_trusted = false;
}

bool ensureTargets(ID3D11Device* dev) {
    if (g_texel && g_staging[0] && g_staging[1]) return true;
    D3D11_TEXTURE2D_DESC td{};
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Width = td.Height = 1;
    if (!g_texel && FAILED(dev->CreateTexture2D(&td, nullptr, &g_texel))) return false;
    if (!g_texelSrv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(g_texel, &sd, &g_texelSrv))) return false;
    }
    td.Width = kStripWidth;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const float zeros[kStripWidth] = {};
    const D3D11_SUBRESOURCE_DATA stagingInit{zeros, sizeof(zeros), 0};
    for (ID3D11Texture2D*& s : g_staging)
        if (!s && FAILED(dev->CreateTexture2D(&td, &stagingInit, &s))) return false;
    return g_texel && g_texelSrv && g_staging[0] && g_staging[1];
}

void pollPrevious(ID3D11DeviceContext* ctx) {
    const uint32_t prev = g_stage ^ 1u;
    if (!g_stageCopied[prev]) return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = ctx->Map(g_staging[prev], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    if (FAILED(hr) || !mapped.pData) return;
    float sample[kStripWidth] = {};
    std::memcpy(sample, mapped.pData, sizeof(sample));
    ctx->Unmap(g_staging[prev], 0);
    const float e = sample[0];
    if (e > 1e-6f && e < 1e8f) g_trusted = true;
    const uint64_t now = GetTickCount64();
    if (g_lastLogMs != 0 && now - g_lastLogMs < 5000) return;
    g_lastLogMs = now;
    Log::get().note("flat exposure: strip %.4g %.4g %.4g %.4g %.4g %.4g (texel 0, smoothed, gain, gain, curve, sun)%s",
                    sample[0], sample[1], sample[2], sample[3], sample[4], sample[5],
                    g_trusted ? "; recorded, not applied (the tonemap divides by texel 0 after bloom)"
                              : "; texel 0 not yet a usable scale");
}

}  // namespace

void flatGameExposureConsider(uint64_t shaderHash, ID3D11UnorderedAccessView* uav1) {
    if (shaderHash != kFlatExposureShader || !uav1) return;
    ID3D11Resource* resource = nullptr;
    uav1->GetResource(&resource);
    if (!resource) return;
    ID3D11Texture2D* tex = nullptr;
    const HRESULT qi = resource->QueryInterface(IID_PPV_ARGS(&tex));
    resource->Release();
    if (FAILED(qi) || !tex) return;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    const bool strip = td.Width == kStripWidth && td.Height == 1 &&
                       (td.Format == DXGI_FORMAT_R32_FLOAT || td.Format == DXGI_FORMAT_R32_TYPELESS);
    if (!strip) {
        if (!g_shapeNoted) {
            g_shapeNoted = true;
            Log::get().note("flat exposure: F1FB2EEF u1 is %ux%u format %u, not the 6x1 R32 strip; HDR-route exposure stays unset",
                            td.Width, td.Height, static_cast<unsigned>(td.Format));
        }
        tex->Release();
        return;
    }
    if (tex == g_strip) { tex->Release(); return; }
    if (g_strip) g_strip->Release();
    g_strip = tex;
    g_trusted = false;
    g_stageCopied[0] = g_stageCopied[1] = false;
    if (!g_heldNoted) {
        g_heldNoted = true;
        Log::get().note("flat exposure: the tonemap strip is held (6x1 R32). Texel 0 is logged and not applied to the HDR route.");
    }
}

FlatGameExposureView flatGameExposurePrepare(ID3D11DeviceContext* ctx) {
    FlatGameExposureView out;
    if (!ctx || !g_strip) return out;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return out;
    const bool ready = ensureTargets(dev);
    dev->Release();
    if (!ready) return out;
    D3D11_BOX one{0, 0, 0, 1, 1, 1};
    D3D11_BOX row{0, 0, 0, kStripWidth, 1, 1};
    {
        FlatComputeInternalScope internal;
        ctx->CopySubresourceRegion(g_texel, 0, 0, 0, 0, g_strip, 0, &one);
        ctx->CopySubresourceRegion(g_staging[g_stage], 0, 0, 0, 0, g_strip, 0, &row);
    }
    g_stageCopied[g_stage] = true;
    pollPrevious(ctx);
    g_stage ^= 1u;
    if (!g_trusted) return out;
    out.texture = g_texel;
    out.srv = g_texelSrv;
    return out;
}

void flatGameExposureShutdown() { releaseAll(); }

}  // namespace edvr
