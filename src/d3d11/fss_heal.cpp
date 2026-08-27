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
cbuffer P : register(b0) {
    float4 p;   // p.x = dx pixels; p.yz = screen AABB min (u,v);
    float4 q;   // p.w,q.x = AABB max -- packed: yz=min, w+q.x=max
}
// Wireframe blue: the blue channel meaningfully ahead of red. Floorless,
// so dim antialiased edges are caught; red-relative, so neutral and warm
// content (ring, body, white bloom) is not.
bool isWire(float4 c) { return c.b > c.r * 1.35 + 0.02; }
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    O.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 l = L[id.xy];
    float4 o = l;
    // Round 48e, the SPATIAL scope: the fill exists only inside the
    // scanner screen's rectangle -- the squares live on the body inside
    // it, the near-field neon frame lives around it, and filling beside
    // the neon at the infinity disparity painted offset twins.
    float u = (id.x + 0.5) / w;
    float v = (id.y + 0.5) / h;
    bool inScreen = u >= p.y && u <= p.w && v >= p.z && v <= q.x;
    if (!inScreen) {
        O[id.xy] = o;
        return;
    }
    // Round 48b, the WINDOW-ERA classifier: one test. The heal now exists
    // only inside the zoom-press arrival window -- body at optical
    // infinity, virtually no UI -- so the v5 gate stack (interior,
    // square-scale, right-lit, bright-region) that protected menus in
    // its always-on life is pure fill-suppression here: it left the
    // squares over DIM ring content black and speckled tile boundaries.
    // A hard-black left pixel takes the right's pixel at the infinity
    // shift, whatever it is: filling space-black with space-black is a
    // no-op, and bright chrome is never hard-black.
    if (dot(l.rgb, float3(0.299, 0.587, 0.114)) < 0.004) {
        int rx = int(id.x) - int(round(p.x));
        uint rw, rh;
        R.GetDimensions(rw, rh);
        if (rx >= 0 && rx < int(rw)) {
            float4 rp = R[uint2(uint(rx), id.y)];
            // The neon wireframe lives in PLAYER space, not at the
            // body's optical infinity -- its right-eye pixels are the
            // wrong disparity for this shift, and stamping them paints
            // offset twins of the blue lines (the field's report, three
            // times now). During the zoom TRANSIT the source region is
            // void plus wireframe and nothing else, so every visible
            // fill in those ~3 s is contamination by definition. Round
            // 49's veto (b > 0.10 and b > 1.6r) let two tails through:
            // dim antialiased bar edges under the 0.10 floor, and
            // bloom-brightened cores whose lifted red defeats the
            // ratio. The test is now floorless and red-relative, and a
            // core that blooms to near-white is caught by its GLOW: the
            // four axis neighbours at 4 px are tested too -- a bar is
            // thinner than 8 px, so some neighbour is always still
            // blue. A vetoed source keeps the left's black -- the stock
            // look, never a new artifact. (Known cost: strongly
            // blue-dominant body content can keep its squares;
            // preferred over ever painting the wireframe.)
            bool wire = isWire(rp) ||
                        isWire(R[uint2(min(uint(rx) + 4u, rw - 1u), id.y)]) ||
                        isWire(R[uint2(uint(max(rx - 4, 0)), id.y)]) ||
                        isWire(R[uint2(uint(rx), min(id.y + 4u, rh - 1u))]) ||
                        isWire(R[uint2(uint(rx), uint(max(int(id.y) - 4, 0)))]);
            if (!wire) o = rp;
        }
    }
    O[id.xy] = o;
}
)HLSL";

// Mode 2, the MIRROR: instead of filling the left's squares with content,
// stamp them into the right -- both eyes then show the intended art, the
// flat screen's look, binocularly fused. Per right pixel: if the LEFT's
// pixel for the same infinity direction is a gated black (the same
// interior and square-scale tests, in left space) and the right here is
// lit content, write black. A misclassified pixel writes black onto a
// mostly-dark surface -- near-invisible -- where the fill direction
// pasted bright content at the wrong disparity and doubled.
constexpr char kMirrorCsHlsl[] = R"HLSL(
Texture2D<float4> L : register(t0);
Texture2D<float4> R : register(t1);
RWTexture2D<float4> O : register(u0);
cbuffer P : register(b0) { float4 p; }   // p.x = dx pixels (left minus right)
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    O.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 r = R[id.xy];
    float4 o = r;
    float rl = dot(r.rgb, float3(0.299, 0.587, 0.114));
    if (rl > 0.02) {
        uint lw, lh;
        L.GetDimensions(lw, lh);
        int lx = int(id.x) + int(round(p.x));
        if (lx >= 0 && lx < int(lw)) {
            const int dys[3] = {0, -16, 16};
            [unroll] for (int i = 0; i < 3; ++i) {
                int ly = int(id.y) + dys[i];
                if (ly < 0 || ly >= int(lh)) continue;
                if (dot(L[uint2(uint(lx), uint(ly))].rgb,
                        float3(0.299, 0.587, 0.114)) >= 0.004) continue;
                // interior of a black region in the left...
                bool blk = true;
                int2 c = int2(lx, ly);
                int2 offs[4] = {int2(-2, 0), int2(2, 0), int2(0, -2),
                                int2(0, 2)};
                [unroll] for (int k = 0; k < 4; ++k) {
                    int2 q = c + offs[k];
                    if (q.x < 0 || q.y < 0 || q.x >= int(lw) ||
                        q.y >= int(lh)) continue;
                    if (dot(L[uint2(q)].rgb,
                            float3(0.299, 0.587, 0.114)) >= 0.004) {
                        blk = false;
                        break;
                    }
                }
                if (!blk) continue;
                // ...at square scale, not panel background or space
                bool farLit = false;
                int2 far4[4] = {int2(-10, 0), int2(10, 0), int2(0, -10),
                                int2(0, 10)};
                [unroll] for (int k2 = 0; k2 < 4; ++k2) {
                    int2 q2 = c + far4[k2];
                    if (q2.x < 0 || q2.y < 0 || q2.x >= int(lw) ||
                        q2.y >= int(lh)) continue;
                    if (dot(L[uint2(q2)].rgb,
                            float3(0.299, 0.587, 0.114)) >= 0.004) {
                        farLit = true;
                        break;
                    }
                }
                if (farLit) o = float4(0, 0, 0, r.a);
                break;
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
ID3D11ComputeShader* g_mirrorCs = nullptr;
bool                 g_mirrorTried = false;
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
            *useCs = shaderSwapCompileCs(
                ctx, mode == 2 ? kMirrorCsHlsl : kHealCsHlsl,
                mode == 2 ? sizeof(kMirrorCsHlsl) - 1
                          : sizeof(kHealCsHlsl) - 1,
                "main", mode == 2 ? "fss_mirror_cs" : "fss_heal_cs", nullptr,
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
}  // namespace edvr

extern "C" __declspec(dllexport) void* edvrFssHealLeft(void* leftTex,
                                                       void* rightTex,
                                                       float outerMag,
                                                       float innerMag,
                                                       int mode,
                                                       const float* rect) {
    if (!leftTex || !rightTex) return nullptr;
    void* out = nullptr;
    edvr::guardedBudget(edvr::g_budget, [&] {
        out = edvr::healInner(leftTex, rightTex, outerMag, innerMag, mode,
                              rect);
    });
    return out;
}
