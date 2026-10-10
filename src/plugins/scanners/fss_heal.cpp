#include "temporal_shader_bytecode.h"
#include "../../d3d11/fss_heal.h"
#include "../../d3d11/graphics_runtime.h"

#include <windows.h>

#include <d3d11.h>

#include "../../common/guard.h"
#include "../../common/log.h"
#include "../../d3d11/shader_swap.h"
#include "../../d3d11/gpu_census.h"   // issue #38: the per-feature GPU cost census

namespace edvr {
namespace {

// Per pixel: keep the left's value unless it is hard black AND the right's
// pixel for the same infinity direction is lit -- the measured signature
// of a reveal-gated tile, and of nothing else.


// Mode 2, the MIRROR: instead of filling the left's squares with content,
// stamp them into the right -- both eyes then show the intended art, the
// flat screen's look, binocularly fused. Per right pixel: if the LEFT's
// pixel for the same infinity direction is a gated black (the same
// interior and square-scale tests, in left space) and the right here is
// lit content, write black. A misclassified pixel writes black onto a
// mostly-dark surface -- near-invisible -- where the fill direction
// pasted bright content at the wrong disparity and doubled.


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
ID3D11ComputeShader* g_mirrorCs = nullptr;
bool                 g_mirrorTried = false;
ID3D11Texture2D*           g_out = nullptr;
ID3D11UnorderedAccessView* g_outUav = nullptr;
uint32_t g_outW = 0, g_outH = 0;
ID3D11Buffer* g_cb = nullptr;
ID3D11Device* g_device = nullptr;
DXGI_FORMAT g_outFormat = DXGI_FORMAT_UNKNOWN;
bool     g_engagedNoted = false;
bool     g_failNoted = false;

FaultBudget g_budget("fssHeal", 8);

void releaseResources() {
    if(g_cs){g_cs->Release();g_cs=nullptr;}
    if(g_mirrorCs){g_mirrorCs->Release();g_mirrorCs=nullptr;}
    if(g_outUav){g_outUav->Release();g_outUav=nullptr;}
    if(g_out){g_out->Release();g_out=nullptr;}
    if(g_cb){g_cb->Release();g_cb=nullptr;}
    if(g_device){g_device->Release();g_device=nullptr;}
    g_outW=g_outH=0;g_outFormat=DXGI_FORMAT_UNKNOWN;
    g_csTried=g_mirrorTried=false;g_engagedNoted=g_failNoted=false;
}

void failOnce(const char* what) {
    if (!g_failNoted) {
        g_failNoted = true;
        Log::get().note("fss heal: %s; the left eye submits stock.", what);
    }
}

void* healInner(void* leftTex, void* rightTex, float outerMag,
                float innerMag, int mode, const float* rect) {
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
    ID3D11Device* rightDevice=nullptr;rt->GetDevice(&rightDevice);
    const bool compatible=rightDevice==dev&&ld.Width==rd.Width&&ld.Height==rd.Height&&
        ld.ArraySize==1&&rd.ArraySize==1&&ld.MipLevels==1&&rd.MipLevels==1&&
        ld.SampleDesc.Count==1&&rd.SampleDesc.Count==1;
    if(rightDevice)rightDevice->Release();
    if(!compatible){dev->Release();lt->Release();rt->Release();return nullptr;}
    if(g_device!=dev){releaseResources();g_device=dev;g_device->AddRef();}
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

    if (!g_out || !g_outUav || g_outW != ld.Width || g_outH != ld.Height || g_outFormat != typed) {
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
        g_outFormat = typed;
    }
    if (ok && !g_cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 32;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
        if (!ok) failOnce("the constant buffer could not be created");
    }
    ID3D11ComputeShader** useCs = mode == 2 ? &g_mirrorCs : &g_cs;
    if (ok && !*useCs) {
        bool* tried = mode == 2 ? &g_mirrorTried : &g_csTried;
        if (!*tried) {
            *tried = true;
            *useCs = shaderSwapCreateCs(
                ctx, mode == 2 ? kFssMirrorBytecode : kFssHealBytecode,
                mode == 2 ? sizeof(kFssMirrorBytecode) : sizeof(kFssHealBytecode),
                mode == 2 ? "fss_mirror_cs" : "fss_heal_cs",
                mode == 2 ? "fss mirror" : "fss heal");
        }
    }
    ok = ok && *useCs != nullptr;

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
            // p.yzw + q: the screen's AABB in texture UV -- the fill
            // exists only inside it (round 48e: the near-field neon frame
            // doubled when its surroundings were filled at the infinity
            // disparity). No rect = a degenerate box = no fill.
            float vals[8] = {dx,
                             rect ? rect[0] : 2.0f,
                             rect ? rect[1] : 2.0f,
                             rect ? rect[2] : -1.0f,
                             rect ? rect[3] : -1.0f,
                             0, 0, 0};
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
        ctx->CSSetShader(*useCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 2, ins);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_outUav, &keep);
        ctx->CSSetConstantBuffers(0, 1, &g_cb);
        gpuCensusBegin(ctx, GpuCensusSection::DoorFssHeal);
        ctx->Dispatch((g_outW + 15) / 16, (g_outH + 15) / 16, 1);
        gpuCensusEnd(ctx, GpuCensusSection::DoorFssHeal);

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

        if (!g_engagedNoted) {
            g_engagedNoted = true;
            Log::get().note(
                mode == 2
                    ? "fss mirror: engaged -- the left eye's gated squares "
                      "are stamped into the right at the infinity shift "
                      "(%.0f px): both eyes show the art, the flat "
                      "screen's look."
                    : "fss heal: engaged -- the left eye's hard-black "
                      "pixels are filled from the right eye's image at "
                      "the infinity shift (%.0f px), stereo untouched "
                      "everywhere else.",
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
void fssHealRelease() { releaseResources(); }
}  // namespace edvr

extern "C" __declspec(dllexport) void* edvrFssHealLeft(void* leftTex,
                                                       void* rightTex,
                                                       float outerMag,
                                                       float innerMag,
                                                       int mode,
                                                       const float* rect) {
    if (edvr::graphicsRuntimeDisabled() || !leftTex || !rightTex) return nullptr;
    void* out = nullptr;
    edvr::guardedBudget(edvr::g_budget, [&] {
        out = edvr::healInner(leftTex, rightTex, outerMag, innerMag, mode,
                              rect);
    });
    return out;
}
