#include "fss_theater.h"

#include <cstring>

#include <windows.h>

#include <d3d11.h>

#include "../common/guard.h"
#include "../common/log.h"
#include "shader_swap.h"

namespace edvr {
namespace {

// Per output pixel: build this eye's view ray from the frustum tangents,
// rotate it into the frozen-head frame (the head's look-around since the
// zoom began), intersect the panel plane at -Z*dist, sample the captured
// image inside the panel or return black. The surround was black in the
// game's own render, so the panel's edges land invisibly at first and
// only head-look reveals the screen.
constexpr char kTheaterCsHlsl[] = R"HLSL(
Texture2D<float4> C : register(t0);
SamplerState S0 : register(s0);
RWTexture2D<float4> O : register(u0);
cbuffer P : register(b0) {
    float4 tans;   // x = left-extent tan, y = right-extent tan,
                   // z = vertical half tan, w = panel distance
    float4 m0;     // delta rotation rows (current-head -> frozen-head);
    float4 m1;     // the rows' .w = this eye's ray origin in frozen-head
    float4 m2;     // space, translation and eye offset folded in
    float4 misc;   // x = vertical band (fraction of the texture's height
                   // the screen shows, centre-cropped), y = half width
                   // along the surface, z = half height, w = curve
    float4 rect;   // the scanner screen's own rectangle in the content
                   // (u0,v0,u1,v1); z > x means valid and it replaces
                   // the centred band
}
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    O.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float u = (id.x + 0.5) / w;
    float v = (id.y + 0.5) / h;
    // View-space ray, OpenVR convention: -Z forward, +Y up, +X right;
    // pixel rows run top to bottom.
    float tx = lerp(-tans.x, tans.y, u);
    float ty = lerp(tans.z, -tans.z, v);
    float3 dv = float3(tx, ty, -1.0);
    float3 df = float3(dot(m0.xyz, dv), dot(m1.xyz, dv), dot(m2.xyz, dv));
    float3 org = float3(m0.w, m1.w, m2.w);
    float4 outc = float4(0, 0, 0, 1);
    float su = -1.0, sv = -1.0;
    if (misc.w > 0.005) {
        // Curved screen: a vertical cylinder of radius dist/curve whose
        // arc centre sits at the screen distance; u runs along the arc so
        // the content keeps its width. The viewer is always inside the
        // cylinder (zc < R), so the ray's forward intersection is the +
        // root of the quadratic.
        float R = tans.w / misc.w;
        float zc = R - tans.w;
        float a = df.x * df.x + df.z * df.z;
        float b = 2.0 * (org.x * df.x + (org.z - zc) * df.z);
        float c = org.x * org.x + (org.z - zc) * (org.z - zc) - R * R;
        float disc = b * b - 4.0 * a * c;
        if (disc > 0 && a > 1e-8) {
            float t = (-b + sqrt(disc)) / (2.0 * a);
            if (t > 0) {
                float3 hit = org + t * df;
                float th = atan2(hit.x, zc - hit.z);
                su = (th * R + misc.y) / (2.0 * misc.y);
                sv = (hit.y + misc.z) / (2.0 * misc.z);
            }
        }
    } else if (df.z < -1e-4) {
        float t = (-tans.w - org.z) / df.z;
        if (t > 0) {
            float3 hit = org + t * df;
            su = (hit.x + misc.y) / (2.0 * misc.y);
            sv = (hit.y + misc.z) / (2.0 * misc.z);
        }
    }
    if (su >= 0 && su <= 1 && sv >= 0 && sv <= 1) {
        // Screen fractions -> content coordinates: the derived rectangle
        // of the scanner's own screen when the bridge supplied one, the
        // centred band otherwise. sv runs bottom-up in space, v runs
        // top-down in the texture.
        float pu, pv;
        if (rect.z > rect.x) {
            pu = lerp(rect.x, rect.z, su);
            pv = lerp(rect.y, rect.w, 1.0 - sv);
        } else {
            pu = su;
            pv = 0.5 + (0.5 - sv) * misc.x;
        }
        outc = float4(C.SampleLevel(S0, float2(pu, pv), 0).rgb, 1);
    }
    O[id.xy] = outc;
}
)HLSL";

DXGI_FORMAT theaterTypedOf(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:                                return f;
    }
}

ID3D11ComputeShader*       g_cs = nullptr;
bool                       g_csTried = false;
ID3D11Texture2D*           g_out[2] = {};
ID3D11UnorderedAccessView* g_outUav[2] = {};
// The copy-through pair: when the submitted texture refuses a shader view
// (the series capture met the same refusal), the content is copied into
// this own samplable texture instead.
ID3D11Texture2D*           g_copy = nullptr;
ID3D11ShaderResourceView*  g_copySrv = nullptr;
uint32_t g_outW = 0, g_outH = 0;
ID3D11Buffer*       g_cb = nullptr;
ID3D11SamplerState* g_samp = nullptr;
uint64_t g_frames = 0;
bool     g_engagedNoted = false;
bool     g_failNoted = false;

FaultBudget g_budget("fssTheater", 8);

void failOnce(const char* what) {
    if (!g_failNoted) {
        g_failNoted = true;
        Log::get().note("fss theater: %s; the eyes submit stock.", what);
    }
}

void* theaterInner(void* contentTex, int eye, float outerMag, float innerMag,
                   const float* xf, float dist, float scale, float curve,
                   float aspect, const float* rect) {
    ID3D11Texture2D* ct = nullptr;
    static_cast<IUnknown*>(contentTex)
        ->QueryInterface(__uuidof(ID3D11Texture2D),
                         reinterpret_cast<void**>(&ct));
    if (!ct) return nullptr;
    D3D11_TEXTURE2D_DESC cd{};
    ct->GetDesc(&cd);

    ID3D11Device* dev = nullptr;
    ct->GetDevice(&dev);
    if (!dev) {
        ct->Release();
        return nullptr;
    }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) {
        dev->Release();
        ct->Release();
        return nullptr;
    }

    bool ok = true;
    const DXGI_FORMAT typed = theaterTypedOf(cd.Format);

    if (!g_out[0] || g_outW != cd.Width || g_outH != cd.Height) {
        for (int e = 0; e < 2; ++e) {
            if (g_outUav[e]) { g_outUav[e]->Release(); g_outUav[e] = nullptr; }
            if (g_out[e]) { g_out[e]->Release(); g_out[e] = nullptr; }
        }
        if (g_copySrv) { g_copySrv->Release(); g_copySrv = nullptr; }
        if (g_copy) { g_copy->Release(); g_copy = nullptr; }
        for (int e = 0; e < 2 && ok; ++e) {
            D3D11_TEXTURE2D_DESC od = cd;
            od.Format = typed;
            od.Usage = D3D11_USAGE_DEFAULT;
            od.BindFlags = D3D11_BIND_SHADER_RESOURCE |
                           D3D11_BIND_UNORDERED_ACCESS;
            od.CPUAccessFlags = 0;
            od.MiscFlags = 0;
            od.MipLevels = 1;
            ok = SUCCEEDED(dev->CreateTexture2D(&od, nullptr, &g_out[e])) &&
                 SUCCEEDED(dev->CreateUnorderedAccessView(g_out[e], nullptr,
                                                          &g_outUav[e]));
        }
        if (!ok) failOnce("the output textures could not be created");
        g_outW = cd.Width;
        g_outH = cd.Height;
    }
    if (ok && !g_cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 96;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
        if (!ok) failOnce("the constant buffer could not be created");
    }
    if (ok && !g_samp) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(dev->CreateSamplerState(&sd, &g_samp));
        if (!ok) failOnce("the sampler could not be created");
    }
    if (ok && !g_cs && !g_csTried) {
        g_csTried = true;
        g_cs = shaderSwapCompileCs(ctx, kTheaterCsHlsl,
                                   sizeof(kTheaterCsHlsl) - 1, "main",
                                   "fss_theater_cs", nullptr, "fss theater");
    }
    ok = ok && g_cs != nullptr;

    ID3D11ShaderResourceView* cs = nullptr;
    if (ok) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = typed;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        if (!(cd.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
            FAILED(dev->CreateShaderResourceView(ct, &vd, &cs))) {
            cs = nullptr;
            if (!g_copy) {
                D3D11_TEXTURE2D_DESC pd = cd;
                pd.Format = typed;
                pd.Usage = D3D11_USAGE_DEFAULT;
                pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                pd.CPUAccessFlags = 0;
                pd.MiscFlags = 0;
                pd.MipLevels = 1;
                pd.SampleDesc.Count = 1;
                pd.SampleDesc.Quality = 0;
                if (FAILED(dev->CreateTexture2D(&pd, nullptr, &g_copy)) ||
                    FAILED(dev->CreateShaderResourceView(g_copy, &vd,
                                                         &g_copySrv))) {
                    if (g_copy) { g_copy->Release(); g_copy = nullptr; }
                    g_copySrv = nullptr;
                }
            }
            if (g_copy && g_copySrv) {
                if (cd.SampleDesc.Count > 1) {
                    ctx->ResolveSubresource(g_copy, 0, ct, 0, typed);
                } else {
                    ctx->CopySubresourceRegion(g_copy, 0, 0, 0, 0, ct, 0,
                                               nullptr);
                }
                g_copySrv->AddRef();
                cs = g_copySrv;
            }
        }
        ok = cs != nullptr;
        if (!ok) failOnce("the content texture refuses a shader view");
    }

    if (ok) {
        // The eye's frustum: the OUTER tangent sits temporal (left edge of
        // the left eye, right edge of the right), the INNER nasal. The
        // panel matches the content's own frustum span so the image
        // appears at its native angular size and the swap-in is seamless.
        const float lt = eye == 0 ? outerMag : innerMag;
        const float rt = eye == 0 ? innerMag : outerMag;
        const float vt =
            (outerMag + innerMag) * 0.5f *
            (static_cast<float>(cd.Height) / static_cast<float>(cd.Width));
        // The screen's half-extents: the content's native angular span
        // at the chosen distance, times the user's scale. The eye capture
        // is nearly SQUARE (the VR frustum is), which the field read as
        // "more vertical than horizontal" -- so the aspect knob makes the
        // screen widescreen and centre-crops the content's height to
        // match, undistorted: full width kept, the top and bottom of the
        // square frustum (mostly void) trimmed. band is the fraction of
        // the texture's height shown; aspect 0 = the native square.
        const float halfW = dist * (outerMag + innerMag) * 0.5f * scale;
        const float nativeHalfH =
            halfW * (static_cast<float>(cd.Height) /
                     static_cast<float>(cd.Width));
        float halfH = nativeHalfH;
        float band = 1.0f;
        if (aspect > 0.01f) {
            halfH = halfW / aspect;
            if (halfH > nativeHalfH) halfH = nativeHalfH;
            band = halfH / nativeHalfH;
        }
        float cbData[24] = {
            lt, rt, vt, dist,
            xf[0], xf[1], xf[2], xf[9],
            xf[3], xf[4], xf[5], xf[10],
            xf[6], xf[7], xf[8], xf[11],
            band, halfW, halfH, curve,
            rect ? rect[0] : 0.0f, rect ? rect[1] : 0.0f,
            rect ? rect[2] : -1.0f, rect ? rect[3] : 0.0f,
        };
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) &&
            m.pData) {
            memcpy(m.pData, cbData, sizeof(cbData));
            ctx->Unmap(g_cb, 0);
        } else {
            ok = false;
        }
    }

    void* result = nullptr;
    if (ok) {
        ID3D11ComputeShader* savedCs = nullptr;
        ID3D11ShaderResourceView* savedSrv = nullptr;
        ID3D11UnorderedAccessView* savedUav = nullptr;
        ID3D11Buffer* savedCb = nullptr;
        ID3D11SamplerState* savedSamp = nullptr;
        ctx->CSGetShader(&savedCs, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 1, &savedSrv);
        ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
        ctx->CSGetConstantBuffers(0, 1, &savedCb);
        ctx->CSGetSamplers(0, 1, &savedSamp);

        UINT keep = 0;
        ctx->CSSetShader(g_cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &cs);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_outUav[eye], &keep);
        ctx->CSSetConstantBuffers(0, 1, &g_cb);
        ctx->CSSetSamplers(0, 1, &g_samp);
        ctx->Dispatch((g_outW + 15) / 16, (g_outH + 15) / 16, 1);

        ID3D11ShaderResourceView* nullSrv = nullptr;
        ID3D11UnorderedAccessView* nullUav = nullptr;
        ctx->CSSetShaderResources(0, 1, &nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
        ctx->CSSetShader(savedCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &savedSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &savedUav, &keep);
        ctx->CSSetConstantBuffers(0, 1, &savedCb);
        ctx->CSSetSamplers(0, 1, &savedSamp);
        if (savedCs) savedCs->Release();
        if (savedSrv) savedSrv->Release();
        if (savedUav) savedUav->Release();
        if (savedCb) savedCb->Release();
        if (savedSamp) savedSamp->Release();

        ++g_frames;
        if (!g_engagedNoted) {
            g_engagedNoted = true;
            Log::get().note(
                "fss theater: engaged -- the zoomed scanner is a single "
                "rendering shown to both eyes as a screen at %.1f m "
                "(scale %.2f, curve %.2f, aspect %.2f). One render, two "
                "displays: no per-eye artifact can exist.",
                static_cast<double>(dist), static_cast<double>(scale),
                static_cast<double>(curve),
                static_cast<double>(aspect));
        }
        result = g_out[eye];
    }

    if (cs) cs->Release();
    ctx->Release();
    dev->Release();
    ct->Release();
    return result;
}

}  // namespace

void fssTheaterWarm(ID3D11DeviceContext* ctx) {
    if (g_cs || g_csTried) return;
    g_csTried = true;
    g_cs = shaderSwapCompileCs(ctx, kTheaterCsHlsl, sizeof(kTheaterCsHlsl) - 1,
                               "main", "fss_theater_cs", nullptr,
                               "fss theater");
    if (g_cs) {
        Log::get().note(
            "fss theater: shader warmed at session start -- the first "
            "engage pays no compile.");
    }
}

}  // namespace edvr

extern "C" __declspec(dllexport) void* edvrFssTheater(void* contentTex,
                                                      int eye,
                                                      float outerMag,
                                                      float innerMag,
                                                      const float* xf,
                                                      float dist,
                                                      float scale,
                                                      float curve,
                                                      float aspect,
                                                      const float* rect) {
    if (!contentTex || !xf || eye < 0 || eye > 1) return nullptr;
    void* out = nullptr;
    edvr::guardedBudget(edvr::g_budget, [&] {
        out = edvr::theaterInner(contentTex, eye, outerMag, innerMag, xf,
                                 dist, scale, curve, aspect, rect);
    });
    return out;
}
