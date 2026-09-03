#include "depth_probe.h"

#include <cmath>
#include <cstring>

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "shader_swap.h"

namespace edvr {
namespace {

// The sampler: 256 depth values on a 16x16 grid across the target, read
// through a view typed to the depth channel. Load, not Sample -- a depth
// view has no filterable format, and the grid wants exact texels.
constexpr char kSampleCsHlsl[] = R"HLSL(
Texture2D<float> D : register(t0);
RWStructuredBuffer<float> O : register(u0);
cbuffer P : register(b0) { uint2 size; uint2 pad0; };
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= 16 || id.y >= 16) return;
    uint2 p = uint2((id.x * 2 + 1) * size.x / 32, (id.y * 2 + 1) * size.y / 32);
    p = min(p, size - 1);
    O[id.y * 16 + id.x] = D.Load(int3(int2(p), 0));
}
)HLSL";

constexpr int      kMaxTargets = 6;
constexpr uint64_t kSampleIntervalMs = 10000;
constexpr int      kMaxSampleLines = 6;

// The near plane the game names when it asks for its projection (0.025,
// every session's receiver line), for turning a reversed-Z value into
// metres: with an infinite far plane, value = near / z. Believed, not
// measured -- the probe prints the raw values beside it so the belief can
// be checked against a known distance.
constexpr double kNearPlaneM = 0.025;

struct Target {
    void*       dsv = nullptr;          // identity only; dereferenced only inside the game's own call
    uint32_t    w = 0, h = 0;
    DXGI_FORMAT texFmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT dsvFmt = DXGI_FORMAT_UNKNOWN;
    UINT        bindFlags = 0;
    UINT        samples = 1;
    uint32_t    firstEyeDraw = 0;       // the lowest eye-draw index it was bound at
    uint32_t    framesSeen = 0;
    uint64_t    lastSampleMs = 0;
    int         sampleLines = 0;
    float       clearValue = -1.0f;     // what the game clears it to; -1 = not seen
    bool        boundThisFrame = false;
    bool        announced = false;
    bool        unreadableNoted = false;
};
Target   g_targets[kMaxTargets];
int      g_targetCount = 0;
void*    g_lastDsv = nullptr;
bool     g_wanted = false;
uint32_t g_distinctThisFrame = 0;
uint32_t g_maxDistinct = 0;
uint32_t g_eyeFrames = 0;   // frames that had at least one eye draw
bool     g_eyeDrawThisFrame = false;
bool     g_summaryNoted = false;

// The GPU side: the shader, its parameter buffer, the 1 KB result and its
// staging twin, and an owned copy for a target that refuses a view.
ID3D11ComputeShader*       g_cs = nullptr;
bool                       g_csTried = false;
ID3D11Buffer*              g_cb = nullptr;
ID3D11Buffer*              g_out = nullptr;
ID3D11UnorderedAccessView* g_outUav = nullptr;
ID3D11Buffer*              g_staging = nullptr;
bool                       g_stagingInFlight = false;
int                        g_stagingTarget = -1;
bool                       g_stagingCopied = false;
ID3D11Texture2D*           g_copyTex = nullptr;
uint32_t                   g_copyW = 0, g_copyH = 0;
DXGI_FORMAT                g_copyFmt = DXGI_FORMAT_UNKNOWN;

FaultBudget g_budget("depthProbe", 6);

const char* fmtName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R24G8_TYPELESS:            return "R24G8_TYPELESS";
        case DXGI_FORMAT_D24_UNORM_S8_UINT:         return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:     return "R24_UNORM_X8_TYPELESS";
        case DXGI_FORMAT_R32_TYPELESS:              return "R32_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT:                 return "D32_FLOAT";
        case DXGI_FORMAT_R32_FLOAT:                 return "R32_FLOAT";
        case DXGI_FORMAT_R32G8X24_TYPELESS:         return "R32G8X24_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:      return "D32_FLOAT_S8X24_UINT";
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:  return "R32_FLOAT_X8X24_TYPELESS";
        case DXGI_FORMAT_R16_TYPELESS:              return "R16_TYPELESS";
        case DXGI_FORMAT_D16_UNORM:                 return "D16_UNORM";
        case DXGI_FORMAT_R16_UNORM:                 return "R16_UNORM";
        default:                                    return "?";
    }
}

// The view format that reads the depth channel of a texture of this
// format, and the typeless format an owned copy of it must have. UNKNOWN
// when this build knows no such view.
DXGI_FORMAT depthReadFormat(DXGI_FORMAT tex, DXGI_FORMAT* copyFmt) {
    switch (tex) {
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            *copyFmt = DXGI_FORMAT_R24G8_TYPELESS;
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
            *copyFmt = DXGI_FORMAT_R32_TYPELESS;
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            *copyFmt = DXGI_FORMAT_R32G8X24_TYPELESS;
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
            *copyFmt = DXGI_FORMAT_R16_TYPELESS;
            return DXGI_FORMAT_R16_UNORM;
        default:
            *copyFmt = DXGI_FORMAT_UNKNOWN;
            return DXGI_FORMAT_UNKNOWN;
    }
}

int findTarget(void* dsv) {
    for (int i = 0; i < g_targetCount; ++i) {
        if (g_targets[i].dsv == dsv) return i;
    }
    return -1;
}

void releaseGpu() {
    if (g_copyTex) { g_copyTex->Release(); g_copyTex = nullptr; }
    g_copyW = g_copyH = 0;
    g_copyFmt = DXGI_FORMAT_UNKNOWN;
    if (g_staging) { g_staging->Release(); g_staging = nullptr; }
    if (g_outUav) { g_outUav->Release(); g_outUav = nullptr; }
    if (g_out) { g_out->Release(); g_out = nullptr; }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_cs) { g_cs->Release(); g_cs = nullptr; }
    g_stagingInFlight = false;
}

bool ensureGpu(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!g_cs && !g_csTried) {
        g_csTried = true;
        g_cs = shaderSwapCompileCs(ctx, kSampleCsHlsl, sizeof(kSampleCsHlsl) - 1,
                                   "main", "depth_probe_cs", nullptr,
                                   "depth probe");
    }
    if (!g_cs) return false;
    if (!g_out) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 256 * 4;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 256;
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = 256 * 4;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_BUFFER_DESC cd{};
        cd.ByteWidth = 16;
        cd.Usage = D3D11_USAGE_DYNAMIC;
        cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        const bool made = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_out)) &&
                          SUCCEEDED(dev->CreateUnorderedAccessView(g_out, &ud, &g_outUav)) &&
                          SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_staging)) &&
                          SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &g_cb));
        if (!made) {
            releaseGpu();
            return false;
        }
    }
    return true;
}

// Metres for a reversed-Z value under an infinite far plane: z = near /
// value. Zero (the far plane) is infinity; the caller prints that as such.
double metresOf(float v) {
    if (!(v > 0.0f)) return 0.0;
    return kNearPlaneM / static_cast<double>(v);
}

}  // namespace

void depthProbeConfigure(Config& cfg) {
    const std::string mode = cfg.getString("fix.temporal_aa", "off");
    g_wanted = _stricmp(mode.c_str(), "off") != 0 && !mode.empty();
}

void depthProbeNoteEyeDraw(ID3D11DeviceContext* ctx, void* dsv,
                           uint32_t eyeDrawIndex) {
    (void)ctx;
    if (!g_wanted) return;
    g_eyeDrawThisFrame = true;
    // The common case is one compare: the same view as the last eye draw.
    // (An address reused by a different view within a frame would be taken
    // for the old one -- a probe's risk, worth one line of caveat and not
    // a per-draw resolve.)
    if (dsv == g_lastDsv) return;
    g_lastDsv = dsv;
    if (!dsv) return;
    int idx = findTarget(dsv);
    if (idx < 0) {
        if (g_targetCount >= kMaxTargets) return;
        Target t;
        t.dsv = dsv;
        bool ok = false;
        guardedBudget(g_budget, [&] {
            ID3D11DepthStencilView* v = static_cast<ID3D11DepthStencilView*>(dsv);
            D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
            v->GetDesc(&vd);
            ID3D11Resource* res = nullptr;
            v->GetResource(&res);
            if (res) {
                ID3D11Texture2D* tex = nullptr;
                res->QueryInterface(__uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&tex));
                if (tex) {
                    D3D11_TEXTURE2D_DESC td{};
                    tex->GetDesc(&td);
                    t.w = td.Width;
                    t.h = td.Height;
                    t.texFmt = td.Format;
                    t.bindFlags = td.BindFlags;
                    t.samples = td.SampleDesc.Count;
                    t.dsvFmt = vd.Format;
                    ok = true;
                    tex->Release();
                }
                res->Release();
            }
        });
        if (!ok) return;
        t.firstEyeDraw = eyeDrawIndex;
        idx = g_targetCount++;
        g_targets[idx] = t;
    }
    Target& t = g_targets[idx];
    if (!t.boundThisFrame) {
        t.boundThisFrame = true;
        ++t.framesSeen;
        ++g_distinctThisFrame;
        if (eyeDrawIndex < t.firstEyeDraw) t.firstEyeDraw = eyeDrawIndex;
    }
}

bool depthProbeWantsSample(void* current, void* next) {
    if (!g_wanted || !current || current == next) return false;
    if (g_stagingInFlight) return false;
    const int idx = findTarget(current);
    if (idx < 0) return false;
    Target& t = g_targets[idx];
    if (t.sampleLines >= kMaxSampleLines) return false;
    if (t.lastSampleMs && !elapsedMs(t.lastSampleMs, kSampleIntervalMs)) return false;
    return true;
}

void depthProbeSample(ID3D11DeviceContext* ctx, void* dsvPtr) {
    if (!g_wanted || !dsvPtr || !ctx) return;
    const int idx = findTarget(dsvPtr);
    if (idx < 0) return;
    Target& t = g_targets[idx];
    t.lastSampleMs = stampMs();

    if (t.samples > 1) {
        if (!t.unreadableNoted) {
            t.unreadableNoted = true;
            Log::get().note(
                "depth probe: target #%d is multisampled (%u samples); its "
                "values cannot be read with a plain load, so v2 would need a "
                "resolve of its own first.",
                idx, t.samples);
        }
        return;
    }
    DXGI_FORMAT copyFmt = DXGI_FORMAT_UNKNOWN;
    const DXGI_FORMAT readFmt = depthReadFormat(t.texFmt, &copyFmt);
    if (readFmt == DXGI_FORMAT_UNKNOWN) {
        if (!t.unreadableNoted) {
            t.unreadableNoted = true;
            Log::get().note(
                "depth probe: target #%d's format %s (%d) has no depth-"
                "reading view this build knows; v2 would need one. Please "
                "report this log.",
                idx, fmtName(t.texFmt), static_cast<int>(t.texFmt));
        }
        return;
    }

    ID3D11DepthStencilView* dsv = static_cast<ID3D11DepthStencilView*>(dsvPtr);
    guardedBudget(g_budget, [&] {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        if (res) {
            res->QueryInterface(__uuidof(ID3D11Texture2D),
                                reinterpret_cast<void**>(&tex));
        }
        if (tex && ensureGpu(dev, ctx)) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = readFmt;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            ID3D11ShaderResourceView* srv = nullptr;
            bool copied = false;
            if (t.bindFlags & D3D11_BIND_SHADER_RESOURCE) {
                dev->CreateShaderResourceView(tex, &sd, &srv);
            }
            if (!srv) {
                // The copy path: the same typeless family, viewable, ours.
                if (!g_copyTex || g_copyW != t.w || g_copyH != t.h ||
                    g_copyFmt != copyFmt) {
                    if (g_copyTex) { g_copyTex->Release(); g_copyTex = nullptr; }
                    D3D11_TEXTURE2D_DESC td{};
                    td.Width = t.w;
                    td.Height = t.h;
                    td.MipLevels = 1;
                    td.ArraySize = 1;
                    td.Format = copyFmt;
                    td.SampleDesc.Count = 1;
                    td.Usage = D3D11_USAGE_DEFAULT;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g_copyTex)) &&
                        g_copyTex) {
                        g_copyW = t.w;
                        g_copyH = t.h;
                        g_copyFmt = copyFmt;
                    } else {
                        g_copyTex = nullptr;
                    }
                }
                if (g_copyTex) {
                    ctx->CopyResource(g_copyTex, tex);
                    dev->CreateShaderResourceView(g_copyTex, &sd, &srv);
                    copied = true;
                }
            }
            if (srv) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) &&
                    m.pData) {
                    uint32_t prm[4] = {t.w, t.h, 0, 0};
                    memcpy(m.pData, prm, sizeof(prm));
                    ctx->Unmap(g_cb, 0);

                    ID3D11ComputeShader* savedCs = nullptr;
                    ID3D11ShaderResourceView* savedSrv = nullptr;
                    ID3D11UnorderedAccessView* savedUav = nullptr;
                    ID3D11Buffer* savedCb = nullptr;
                    ctx->CSGetShader(&savedCs, nullptr, nullptr);
                    ctx->CSGetShaderResources(0, 1, &savedSrv);
                    ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
                    ctx->CSGetConstantBuffers(0, 1, &savedCb);

                    ID3D11ShaderResourceView* nullSrv = nullptr;
                    ID3D11UnorderedAccessView* nullUav = nullptr;
                    ctx->CSSetShaderResources(0, 1, &nullSrv);
                    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
                    ctx->CSSetShader(g_cs, nullptr, 0);
                    ID3D11Buffer* cb = g_cb;
                    ctx->CSSetConstantBuffers(0, 1, &cb);
                    ctx->CSSetShaderResources(0, 1, &srv);
                    ctx->CSSetUnorderedAccessViews(0, 1, &g_outUav, nullptr);
                    ctx->Dispatch(1, 1, 1);
                    ctx->CSSetShaderResources(0, 1, &nullSrv);
                    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
                    ctx->CopyResource(g_staging, g_out);

                    ctx->CSSetShader(savedCs, nullptr, 0);
                    ctx->CSSetShaderResources(0, 1, &savedSrv);
                    ctx->CSSetUnorderedAccessViews(0, 1, &savedUav, nullptr);
                    ctx->CSSetConstantBuffers(0, 1, &savedCb);
                    if (savedCs) savedCs->Release();
                    if (savedSrv) savedSrv->Release();
                    if (savedUav) savedUav->Release();
                    if (savedCb) savedCb->Release();

                    g_stagingInFlight = true;
                    g_stagingTarget = idx;
                    g_stagingCopied = copied;
                }
                srv->Release();
            }
        }
        if (tex) tex->Release();
        if (res) res->Release();
        dev->Release();
    });
}

void depthProbeNoteClear(ID3D11DepthStencilView* dsv, float depth) {
    if (!g_wanted || !dsv) return;
    const int idx = findTarget(dsv);
    if (idx < 0) return;
    g_targets[idx].clearValue = depth;
}

void depthProbeFrameBoundary(ID3D11DeviceContext* ctx) {
    if (!g_wanted) return;
    if (g_eyeDrawThisFrame) ++g_eyeFrames;
    g_eyeDrawThisFrame = false;
    if (g_distinctThisFrame > g_maxDistinct) g_maxDistinct = g_distinctThisFrame;
    g_distinctThisFrame = 0;
    g_lastDsv = nullptr;   // a new frame: its first eye draw notes its view afresh
    for (int i = 0; i < g_targetCount; ++i) {
        Target& t = g_targets[i];
        t.boundThisFrame = false;
        if (t.announced) continue;
        t.announced = true;
        Log::get().note(
            "depth probe: the eye draws bind depth target #%d -- %ux%u, "
            "texture format %s (%d), view format %s (%d), bind flags 0x%X, "
            "%u sample(s); first bound at eye draw #%u of a frame. %s "
            "(docs\\anti-aliasing.md Phase 0 item 3, measured.)",
            i, t.w, t.h, fmtName(t.texFmt), static_cast<int>(t.texFmt),
            fmtName(t.dsvFmt), static_cast<int>(t.dsvFmt), t.bindFlags,
            t.samples, t.firstEyeDraw,
            (t.bindFlags & D3D11_BIND_SHADER_RESOURCE)
                ? "A shader view can be made over it directly, so v2 reads "
                  "it at submit for free."
                : "No shader-resource bind: v2 would copy it out once per "
                  "eye (CopyResource, same typeless family) before reading.");
    }
    if (!g_summaryNoted && g_eyeFrames >= 120) {
        g_summaryNoted = true;
        if (g_targetCount == 0) {
            Log::get().note(
                "depth probe: no depth-stencil view was bound during the eye "
                "draws of the first 120 frames that had any -- the game may "
                "bind depth by a path the binding shadow does not see, or "
                "draw the eyes without it. v2 needs another way in; please "
                "report this log.");
        } else {
            Log::get().note(
                "depth probe: %d distinct depth target(s) seen at the eye "
                "draws over 120 frames, at most %u in one frame. Each is "
                "sampled at the moment the game unbinds it, its contents "
                "complete, every %llu s, a few times per target.",
                g_targetCount, g_maxDistinct,
                static_cast<unsigned long long>(kSampleIntervalMs / 1000));
        }
    }
    if (g_stagingInFlight && ctx && g_staging) {
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ctx->Map(g_staging, 0, D3D11_MAP_READ,
                                    D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (SUCCEEDED(hr) && m.pData) {
            const float* v = static_cast<const float*>(m.pData);
            const int tIdx = g_stagingTarget >= 0 ? g_stagingTarget : 0;
            Target& t = g_targets[tIdx];
            const float clear = t.clearValue;
            float mn = 1e30f, mx = -1e30f;
            int atClear = 0;
            // Bands, for the reversed-Z reading: value = near / z.
            int bandFar = 0, bandKm = 0, bandHm = 0, bandM = 0, bandNear = 0;
            for (int i = 0; i < 256; ++i) {
                const float d = v[i];
                if (d < mn) mn = d;
                if (d > mx) mx = d;
                if (clear >= 0.0f && fabsf(d - clear) < 1e-7f) ++atClear;
                if (d <= 0.0f) ++bandFar;
                else if (d < 2.5e-6f) ++bandKm;     // beyond 10 km
                else if (d < 2.5e-4f) ++bandHm;     // 100 m .. 10 km
                else if (d < 1.25e-2f) ++bandM;     // 2 m .. 100 m
                else ++bandNear;                    // within 2 m
            }
            const float centre = v[7 * 16 + 7];
            ctx->Unmap(g_staging, 0);
            g_stagingInFlight = false;
            ++t.sampleLines;
            const bool reversed = clear >= 0.0f ? clear < 0.5f : (mx < 0.5f);
            const float nearest = reversed ? mx : mn;
            Log::get().note(
                "depth probe: target #%d sampled on a 16x16 grid at its "
                "unbind%s -- min %.8f, max %.8f, centre %.8f (%.2f m if "
                "reversed-Z with near %.3f m); %d of 256 at the clear value "
                "%.3f (nothing drawn there). Bands, read as reversed-Z: %d at "
                "the far plane, %d beyond 10 km, %d at 100 m..10 km, %d at "
                "2..100 m, %d within 2 m. The nearest sample %.8f is %.2f m "
                "under that reading.",
                tIdx, g_stagingCopied ? " (copied out first)" : "",
                static_cast<double>(mn), static_cast<double>(mx),
                static_cast<double>(centre), metresOf(centre), kNearPlaneM,
                atClear, static_cast<double>(clear), bandFar, bandKm, bandHm,
                bandM, bandNear, static_cast<double>(nearest), metresOf(nearest));
        } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
            g_stagingInFlight = false;   // unreadable; the next interval retries
        }
    }
}

void depthProbeShutdown() {
    if (g_targetCount > 0) {
        Log::get().note("depth probe: %d depth target(s) seen at the eye draws "
                        "this session.", g_targetCount);
    }
    releaseGpu();
    g_targetCount = 0;
    g_lastDsv = nullptr;
}

}  // namespace edvr
