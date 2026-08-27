#include "fss_scan.h"

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/log.h"
#include "binding_shadow.h"

namespace edvr {
namespace {

// The matrix, as the round-four census resolved it on every body: one
// 16x16 fmt-60 (R16_UINT) texture, the same resource across five captures,
// written by nothing any hook sees. Size and format ARE the signature --
// nothing else the scanner binds is remotely this shape.
constexpr uint32_t kMatrixW = 16;
constexpr uint32_t kMatrixH = 16;
constexpr uint32_t kMatrixFmt = 60;   // DXGI_FORMAT_R16_UINT, as resolved

bool     g_steady = false;
uint32_t g_level = 0;

// The substitute: a 16x16 R16_UINT filled with the level -- full-size, not
// 1x1, because a uint matrix is as likely read with Load() and tile
// coordinates as sampled, and an out-of-bounds Load returns zero whatever
// the level asks for.
ID3D11Texture2D*          g_tex = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
uint32_t                  g_texLevel = 0xFFFFFFFFu;
bool                      g_createFailedNoted = false;

// Which of PS slots 0-3 the matrix sat in for the draw being wrapped, and
// what each displaced. Several slots at once is legal by construction --
// the ring draw binds it at 0, its detail pair at 3.
uint8_t                   g_slots = 0;
ID3D11ShaderResourceView* g_displaced[4] = {};
bool                      g_engaged = false;

uint64_t g_applied = 0;

ID3D11ShaderResourceView* uniformSrv(ID3D11DeviceContext* ctx) {
    if (g_srv && g_texLevel == g_level) return g_srv;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;

    uint16_t texels[kMatrixW * kMatrixH];
    const uint16_t v = static_cast<uint16_t>(g_level > 0xFFFF ? 0xFFFF : g_level);
    for (uint32_t i = 0; i < kMatrixW * kMatrixH; ++i) texels[i] = v;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kMatrixW;
    td.Height = kMatrixH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = texels;
    init.SysMemPitch = kMatrixW * sizeof(uint16_t);

    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = dev->CreateTexture2D(&td, &init, &tex);
    if (SUCCEEDED(hr) && tex) hr = dev->CreateShaderResourceView(tex, nullptr, &srv);
    dev->Release();
    if (FAILED(hr) || !srv) {
        if (tex) tex->Release();
        if (!g_createFailedNoted) {
            g_createFailedNoted = true;
            Log::get().note("fss scan: could not create the uniform matrix "
                            "(hr=0x%08X); the scanner draws stock.",
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

void fssScanConfigure(Config& cfg) {
    const bool was = g_steady;
    const std::string m = cfg.getString("experimental.fss_scan", "stock");
    if (m == "stock") {
        g_steady = false;
    } else if (m == "steady") {
        g_steady = true;
    } else {
        g_steady = false;
        Log::get().note("fss_scan \"%s\" is not stock or steady; running "
                        "stock.", m.c_str());
    }
    g_level = static_cast<uint32_t>(
        cfg.getIntInRange("advanced.fss_scan_level", 0, 0, 65535));

    if (was != g_steady) {
        Log::get().note(
            "fss scan: %s. The scanner's tile-dissolve matrix -- the black "
            "16-pixel squares while a body resolves -- is %s; level %u. If "
            "the ring HIDES during a scan instead of appearing whole, set "
            "fss_scan_level = 65535 under [advanced]: the level's meaning "
            "depends on the shader's comparison direction, and one flip "
            "tells us which way this one reads.",
            g_steady ? "steady" : "stock",
            g_steady ? "held uniform for exactly the body-layer draws that "
                       "bind it"
                     : "the game's own",
            g_level);
    }
}

bool fssScanWantsDraws() { return g_steady; }

bool fssScanOnBodyDraw() {
    if (!g_steady) return false;
    g_slots = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        const BindSlot slot =
            static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + i);
        ResourceInfo info;
        if (bindingResolve(bindingGet(slot), &info) && info.isTexture2D &&
            info.a == kMatrixW && info.b == kMatrixH &&
            info.fmt == kMatrixFmt) {
            g_slots |= static_cast<uint8_t>(1u << i);
        }
    }
    return g_slots != 0;
}

void fssScanBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    ID3D11ShaderResourceView* uniform = uniformSrv(ctx);
    if (!uniform) return;

    for (uint32_t i = 0; i < 4; ++i) {
        if (!(g_slots & (1u << i))) continue;
        const BindSlot slot =
            static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + i);
        g_displaced[i] = static_cast<ID3D11ShaderResourceView*>(bindingGet(slot));
        ctx->PSSetShaderResources(i, 1, &uniform);
    }
    g_engaged = true;

    if (++g_applied == 1) {
        Log::get().note(
            "fss scan: steady engaged -- the dissolve matrix is uniform "
            "level %u for exactly the body-layer draws that bind it, and "
            "the game's texture is restored after every draw.",
            g_texLevel);
    }
}

void fssScanEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged) return;
    g_engaged = false;
    for (uint32_t i = 0; i < 4; ++i) {
        if (!(g_slots & (1u << i))) continue;
        ID3D11ShaderResourceView* orig = g_displaced[i];
        g_displaced[i] = nullptr;
        ctx->PSSetShaderResources(i, 1, &orig);
    }
    g_slots = 0;
}

void fssScanShutdown() {
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
