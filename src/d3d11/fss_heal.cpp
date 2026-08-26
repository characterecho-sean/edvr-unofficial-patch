#include "fss_heal.h"

#include <windows.h>

#include <d3d11.h>

#include "../common/guard.h"
#include "../common/log.h"
#include "shader_swap.h"

namespace edvr {
namespace {

// Per pixel: keep the left's value unless it is hard black AND the right's
// pixel for the same infinity direction is lit -- the measured signature
// of a reveal-gated tile, and of nothing else.
constexpr char kHealCsHlsl[] = R"HLSL(
Texture2D<float4> L : register(t0);
Texture2D<float4> R : register(t1);
RWTexture2D<float4> O : register(u0);
cbuffer P : register(b0) { float4 p; }   // p.x = dx pixels (left minus right)
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    O.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 l = L[id.xy];
    float4 o = l;
    if (dot(l.rgb, float3(0.299, 0.587, 0.114)) < 0.004) {
        int rx = int(id.x) - int(round(p.x));
        uint rw, rh;
        R.GetDimensions(rw, rh);
        if (rx >= 0 && rx < int(rw)) {
            // The horizontal shift is exact only for a perfectly
            // symmetric vertical frustum; the field measured ~16px of
            // vertical offset on a canted headset, enough to land an
            // edge tile's counterpart in dark space. Three taps cover
            // it; the first lit one wins.
            const int dys[3] = {0, -16, 16};
            [unroll] for (int i = 0; i < 3; ++i) {
                int ry = int(id.y) + dys[i];
                if (ry < 0 || ry >= int(rh)) continue;
                float4 r = R[uint2(uint(rx), uint(ry))];
                if (dot(r.rgb, float3(0.299, 0.587, 0.114)) > 0.008) {
                    o = r;
                    break;
                }
            }
        }
    }
    O[id.xy] = o;
}
)HLSL";

DXGI_FORMAT healTypedOf(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:                                return f;
    }
}

ID3D11ComputeShader* g_cs = nullptr;
bool                 g_csTried = false;
ID3D11Texture2D*           g_out = nullptr;
ID3D11UnorderedAccessView* g_outUav = nullptr;
uint32_t g_outW = 0, g_outH = 0;
ID3D11Buffer* g_cb = nullptr;
uint64_t g_healed = 0;
bool     g_engagedNoted = false;
bool     g_failNoted = false;

FaultBudget g_budget("fssHeal", 8);

void failOnce(const char* what) {
    if (!g_failNoted) {
        g_failNoted = true;
        Log::get().note("fss heal: %s; the left eye submits stock.", what);
    }
}

void* healInner(void* leftTex, void* rightTex, float outerMag,
                float innerMag) {
    ID3D11Texture2D* lt = nullptr;
    ID3D11Texture2D* rt = nullptr;
    static_cast<IUnknown*>(leftTex)->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&lt));
    static_cast<IUnknown*>(rightTex)->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&rt));
    if (!lt || !rt) {
        if (lt) lt->Release();
        if (rt) rt->Release();
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC ld{}, rd{};
    lt->GetDesc(&ld);
    rt->GetDesc(&rd);

    ID3D11Device* dev = nullptr;
    lt->GetDevice(&dev);
    if (!dev) {
        lt->Release();
        rt->Release();
        return nullptr;
    }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) {
        dev->Release();
        lt->Release();
        rt->Release();
        return nullptr;
    }

    const DXGI_FORMAT typed = healTypedOf(ld.Format);
    bool ok = true;

    if (!g_out || g_outW != ld.Width || g_outH != ld.Height) {
        if (g_outUav) { g_outUav->Release(); g_outUav = nullptr; }
        if (g_out) { g_out->Release(); g_out = nullptr; }
        D3D11_TEXTURE2D_DESC od = ld;
        od.Format = typed;
        od.Usage = D3D11_USAGE_DEFAULT;
        od.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                       D3D11_BIND_UNORDERED_ACCESS;
        od.CPUAccessFlags = 0;
        od.MiscFlags = 0;
        od.MipLevels = 1;
        ok = SUCCEEDED(dev->CreateTexture2D(&od, nullptr, &g_out)) &&
             SUCCEEDED(dev->CreateUnorderedAccessView(g_out, nullptr,
                                                      &g_outUav));
        if (!ok) failOnce("the healed texture could not be created (UAV "
                          "store unsupported for the submitted format?)");
        g_outW = ld.Width;
        g_outH = ld.Height;
    }
    if (ok && !g_cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
        if (!ok) failOnce("the constant buffer could not be created");
    }
    if (ok && !g_cs && !g_csTried) {
        g_csTried = true;
        g_cs = shaderSwapCompileCs(ctx, kHealCsHlsl, sizeof(kHealCsHlsl) - 1,
                                   "main", "fss_heal_cs", nullptr,
                                   "fss heal");
    }
    ok = ok && g_cs != nullptr;

    ID3D11ShaderResourceView* ls = nullptr;
    ID3D11ShaderResourceView* rs = nullptr;
    if (ok) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = typed;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        D3D11_SHADER_RESOURCE_VIEW_DESC rvd = vd;
        rvd.Format = healTypedOf(rd.Format);
        ok = (ld.BindFlags & D3D11_BIND_SHADER_RESOURCE) &&
             (rd.BindFlags & D3D11_BIND_SHADER_RESOURCE) &&
             SUCCEEDED(dev->CreateShaderResourceView(lt, &vd, &ls)) &&
             SUCCEEDED(dev->CreateShaderResourceView(rt, &rvd, &rs));
        if (!ok) failOnce("the submitted textures refuse shader views");
    }

    if (ok) {
        // The infinity shift, from the frustum tangents: straight-ahead
        // lands at outer/(outer+inner) across the left image and mirrored
        // across the right, so identical far content sits dx further right
        // in the left image.
        const float denom = outerMag + innerMag;
        const float dx =
            denom > 0.0f
                ? static_cast<float>(ld.Width) * (outerMag - innerMag) / denom
                : 0.0f;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) &&
            m.pData) {
            float vals[4] = {dx, 0, 0, 0};
            memcpy(m.pData, vals, sizeof(vals));
            ctx->Unmap(g_cb, 0);
        } else {
            ok = false;
        }
    }

    if (ok) {
        ID3D11ComputeShader* savedCs = nullptr;
        ID3D11ShaderResourceView* savedSrv[2] = {};
        ID3D11UnorderedAccessView* savedUav = nullptr;
        ID3D11Buffer* savedCb = nullptr;
        ctx->CSGetShader(&savedCs, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 2, savedSrv);
        ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
        ctx->CSGetConstantBuffers(0, 1, &savedCb);

        ID3D11ShaderResourceView* ins[2] = {ls, rs};
        UINT keep = 0;
        ctx->CSSetShader(g_cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 2, ins);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_outUav, &keep);
        ctx->CSSetConstantBuffers(0, 1, &g_cb);
        ctx->Dispatch((g_outW + 15) / 16, (g_outH + 15) / 16, 1);

        ID3D11ShaderResourceView* nullSrv[2] = {};
        ID3D11UnorderedAccessView* nullUav = nullptr;
        ctx->CSSetShaderResources(0, 2, nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
        ctx->CSSetShader(savedCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 2, savedSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &savedUav, &keep);
        ctx->CSSetConstantBuffers(0, 1, &savedCb);
        if (savedCs) savedCs->Release();
        for (ID3D11ShaderResourceView* v : savedSrv) {
            if (v) v->Release();
        }
        if (savedUav) savedUav->Release();
        if (savedCb) savedCb->Release();

        ++g_healed;
        if (!g_engagedNoted) {
            g_engagedNoted = true;
            Log::get().note(
                "fss heal: engaged -- the left eye's hard-black pixels are "
                "filled from the right eye's image at the infinity shift "
                "(%.0f px), stereo untouched everywhere else.",
                static_cast<double>(ld.Width) *
                    (outerMag - innerMag) / (outerMag + innerMag));
        }
    }

    if (ls) ls->Release();
    if (rs) rs->Release();
    ctx->Release();
    dev->Release();
    lt->Release();
    rt->Release();
    return ok ? g_out : nullptr;
}

}  // namespace
}  // namespace edvr

extern "C" __declspec(dllexport) void* edvrFssHealLeft(void* leftTex,
                                                       void* rightTex,
                                                       float outerMag,
                                                       float innerMag) {
    if (!leftTex || !rightTex) return nullptr;
    void* out = nullptr;
    edvr::guardedBudget(edvr::g_budget, [&] {
        out = edvr::healInner(leftTex, rightTex, outerMag, innerMag);
    });
    return out;
}
