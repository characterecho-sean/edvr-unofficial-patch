#include "supersample_pass.h"

#include <cstring>

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/frame_flag.h"   // glitchConsumerPresent: is a compositor hook alive
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/supersample_math.h"
#include "../common/timing.h"
#include "shader_swap.h"

namespace edvr {
namespace {

// The filter, one compute shader for both passes: `dir` picks the axis, the
// cbuffer carries the region and the kernel. kernelAt and the tap range are
// transcriptions of supersampleKernel and supersampleTapRange in
// src/common/supersample_math.h -- the C++ is the reference the test pins,
// this is its GPU twin, and a change to one is a change to both. Desk-
// compiled by tools/compile_variants.py --target=cs_5_0 before it ships.
//
// The taps clamp INTO the region and never past it. Elite submits one
// double-wide texture and names each eye by its bounds, so the other eye is
// one pixel beyond the region's edge; under the cull guard the crop's
// margin would be. Edge replication keeps the filter's support inside the
// eye's own pixels at the border, at the price of a slightly heavier edge
// pixel, which is the right trade for something the lens then distorts
// further out anyway.
constexpr char kResolveCsHlsl[] = R"HLSL(
Texture2D<float4> S : register(t0);
RWTexture2D<float4> O : register(u0);
cbuffer P : register(b0) {
    int4  region;    // x0 y0 x1 y1: the pixels of S this pass may read (x1, y1 exclusive)
    int2  outSize;   // this pass's output extent
    int   dir;       // 0: filter along x (the first pass), 1: along y (the second)
    int   filt;      // 0 calm (gaussian), 1 crisp (mitchell-netravali)
    float scale;     // input pixels per output pixel along dir, >= 1
    float width;     // kernel radius in output pixels
    int   decode;    // 1: sRGB-decode every tap read (gamma content, first pass)
    int   encode;    // 1: sRGB-encode the result (gamma content, second pass)
};
// supersampleKernel, transcribed: calm is a Gaussian with sigma = width/2,
// crisp is Mitchell-Netravali (B = C = 1/3) with its support of 2 mapped
// onto `width`.
float kernelAt(float dOut) {
    float t = abs(dOut);
    if (t >= width) return 0.0;
    if (filt == 1) {
        float x = 2.0 * t / width;
        if (x < 1.0) return (7.0 * x * x * x - 12.0 * x * x + 16.0 / 3.0) / 6.0;
        return (-7.0 / 3.0 * x * x * x + 12.0 * x * x - 20.0 * x + 32.0 / 3.0) / 6.0;
    }
    float sigma = width * 0.5;
    float u = t / sigma;
    return exp(-0.5 * u * u);
}
// The sRGB transfer function, both ways, on the colour channels only.
float3 srgbToLinear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
    return c <= 0.04045 ? lo : hi;
}
float3 linearToSrgb(float3 c) {
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return c <= 0.0031308 ? lo : hi;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)outSize.x || id.y >= (uint)outSize.y) return;
    // supersampleTapRange, transcribed: the output pixel's centre in
    // region-relative input pixels along the filtered axis, and the input
    // pixels whose centres fall within the radius; the nearest one always
    // taps, whatever a tiny radius rounds to.
    int o = dir == 0 ? int(id.x) : int(id.y);
    float centre = (float(o) + 0.5) * scale;
    float radius = width * scale;
    int first = int(ceil(centre - radius - 0.5));
    int last = int(floor(centre + radius - 0.5));
    int nearest = int(floor(centre));
    first = min(first, nearest);
    last = max(last, nearest);
    // The filtered axis's span inside S, and the other axis's coordinate,
    // which passes straight through (offset into the region on the first
    // pass; the intermediate is region-sized on that axis for the second).
    int lo = dir == 0 ? region.x : region.y;
    int hi = (dir == 0 ? region.z : region.w) - 1;
    int across = dir == 0 ? region.y + int(id.y) : region.x + int(id.x);
    float4 acc = 0.0;
    float wsum = 0.0;
    [loop] for (int i = first; i <= last; ++i) {
        float w = kernelAt((float(i) + 0.5 - centre) / scale);
        int ic = clamp(lo + i, lo, hi);
        int2 p = dir == 0 ? int2(ic, across) : int2(across, ic);
        float4 s = S[p];
        if (decode != 0) s.rgb = srgbToLinear(s.rgb);
        acc += s * w;
        wsum += w;
    }
    float4 r;
    if (wsum > 1e-6) {
        r = acc / wsum;
    } else {
        // Unreachable for the widths supersample_math.h allows (the nearest
        // tap is positive for both kernels); the nearest pixel, plainly, if
        // it ever is.
        int ic = clamp(lo + nearest, lo, hi);
        r = S[dir == 0 ? int2(ic, across) : int2(across, ic)];
        if (decode != 0) r.rgb = srgbToLinear(r.rgb);
    }
    if (encode != 0) r.rgb = linearToSrgb(saturate(r.rgb));
    O[id.xy] = r;
}
)HLSL";

// The cbuffer above, laid out to match: 48 bytes, three 16-byte rows.
struct PassParams {
    int32_t region[4];
    int32_t outSize[2];
    int32_t dir;
    int32_t filt;
    float   scale;
    float   width;
    int32_t decode;
    int32_t encode;
};
static_assert(sizeof(PassParams) == 48, "the cbuffer is three 16-byte rows");

// The format allowlist: what the resolve reads and writes, by family.
//
// The output texture keeps the SOURCE's format verbatim -- a typeless
// submission stays typeless, a UNORM one UNORM -- so the compositor is
// handed the same KIND of texture the game submits (guardCropCopy's rule);
// the views over both are the family's plain typed format. The sRGB-typed
// variants are refused rather than guessed at: a UAV cannot be typed sRGB,
// and re-labelling the output typeless would change what the compositor
// is told about it. docs/anti-aliasing.md Phase 0 item 2 asks for the
// submitted formats measured, not assumed; the first-frame line below is
// that measurement, and a refusal here is the log line that says an
// unlisted one turned up. Measured so far: R8G8B8A8_TYPELESS on a Quest 3
// over Virtual Desktop (2026-09-02, the render-scale work).
DXGI_FORMAT viewFormatOf(DXGI_FORMAT f, int* index) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            *index = 0;
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            *index = 1;
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            *index = 2;
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            *index = 3;
            return DXGI_FORMAT_R16G16B16A16_UNORM;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            *index = 4;
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            *index = -1;
            return DXGI_FORMAT_UNKNOWN;
    }
}
constexpr int kFormatCount = 5;

const char* formatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return "R8G8B8A8_TYPELESS";
        case DXGI_FORMAT_R8G8B8A8_UNORM:        return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return "B8G8R8A8_TYPELESS";
        case DXGI_FORMAT_B8G8R8A8_UNORM:        return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return "R10G10B10A2_TYPELESS";
        case DXGI_FORMAT_R10G10B10A2_UNORM:     return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
        case DXGI_FORMAT_R16G16B16A16_UNORM:    return "R16G16B16A16_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
        default:                                return "?";
    }
}

const char* filterName(int filter) {
    return filter == kSupersampleCrisp ? "crisp (mitchell)" : "calm (gaussian)";
}

// Per-eye owned resources. Release-before-recreate on any size or format
// change, guardCropCopy's cache discipline; keyed on what WE need, never on
// the game's resource -- except the one view over it, which is keyed on
// exactly that and nothing else.
struct EyeState {
    // A typed view over the game's own eye texture, when it allows one.
    // Held across frames because Elite reuses one texture per eye for a
    // session (measured); holding the view holds the texture, so its
    // address cannot be recycled under the cache -- the recurring bug class
    // that reasoning exists for -- and it is released the moment a
    // different texture arrives for this eye.
    void*                      srcRes = nullptr;
    ID3D11ShaderResourceView*  srcSrv = nullptr;

    // The copy-through pair, for a source that refuses a shader view (the
    // theater met the same refusal): the eye's region, copied out, so the
    // filter reads it instead. Region-sized, so the region becomes {0, 0,
    // w, h} for the pass.
    ID3D11Texture2D*           copyTex = nullptr;
    ID3D11ShaderResourceView*  copySrv = nullptr;
    uint32_t                   copyW = 0, copyH = 0;
    DXGI_FORMAT                copyFmt = DXGI_FORMAT_UNKNOWN;

    // The horizontal pass's target: outW wide, region-high, float16 so the
    // second pass reads unquantised sums.
    ID3D11Texture2D*           midTex = nullptr;
    ID3D11ShaderResourceView*  midSrv = nullptr;
    ID3D11UnorderedAccessView* midUav = nullptr;
    uint32_t                   midW = 0, midH = 0;

    // The result: outW x outH in the source's own format, what the openvr
    // half submits.
    ID3D11Texture2D*           outTex = nullptr;
    ID3D11UnorderedAccessView* outUav = nullptr;
    uint32_t                   outW = 0, outH = 0;
    DXGI_FORMAT                outFmt = DXGI_FORMAT_UNKNOWN;
};
EyeState g_eye[2];

void releaseSrc(EyeState& e) {
    if (e.srcSrv) { e.srcSrv->Release(); e.srcSrv = nullptr; }
    e.srcRes = nullptr;
}
void releaseCopy(EyeState& e) {
    if (e.copySrv) { e.copySrv->Release(); e.copySrv = nullptr; }
    if (e.copyTex) { e.copyTex->Release(); e.copyTex = nullptr; }
    e.copyW = e.copyH = 0;
    e.copyFmt = DXGI_FORMAT_UNKNOWN;
}
void releaseMid(EyeState& e) {
    if (e.midUav) { e.midUav->Release(); e.midUav = nullptr; }
    if (e.midSrv) { e.midSrv->Release(); e.midSrv = nullptr; }
    if (e.midTex) { e.midTex->Release(); e.midTex = nullptr; }
    e.midW = e.midH = 0;
}
void releaseOut(EyeState& e) {
    if (e.outUav) { e.outUav->Release(); e.outUav = nullptr; }
    if (e.outTex) { e.outTex->Release(); e.outTex = nullptr; }
    e.outW = e.outH = 0;
    e.outFmt = DXGI_FORMAT_UNKNOWN;
}
void releaseEye(EyeState& e) {
    releaseSrc(e);
    releaseCopy(e);
    releaseMid(e);
    releaseOut(e);
}

// The GPU-price ring: a few {disjoint, begin, end} query triples bracketing
// one call's two dispatches. Never awaited -- a slot that is not ready is
// tried again next call (D3D11_ASYNC_GETDATA_DONOTFLUSH throughout), and a
// call that finds every slot in flight runs untimed. Timing must never be
// able to stall or refuse the treatment it is measuring.
struct QuerySlot {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* begin = nullptr;
    ID3D11Query* end = nullptr;
    bool         inUse = false;
};
constexpr int kQueryRing = 8;
QuerySlot g_qring[kQueryRing];

void releaseQuerySlot(QuerySlot& q) {
    if (q.disjoint) { q.disjoint->Release(); q.disjoint = nullptr; }
    if (q.begin) { q.begin->Release(); q.begin = nullptr; }
    if (q.end) { q.end->Release(); q.end = nullptr; }
    q.inUse = false;
}

uint32_t g_timeCount = 0;
double   g_timeSum = 0.0;
double   g_timeMax = 0.0;
bool     g_timeLogged = false;
// What the last treated call looked like, for the price line's own words.
uint32_t g_lastRegionW = 0, g_lastRegionH = 0, g_lastOutW = 0, g_lastOutH = 0;
int      g_lastFilter = 0;
float    g_lastWidth = 0.0f;

// Said once, after enough samples to mean something: the price the design
// asks for (docs/anti-aliasing.md Phase 0 item 6), quoted the way the
// guard quotes its margins -- the number, and the key that moves it.
void maybeLogTiming() {
    if (g_timeLogged || g_timeCount < 120) return;
    g_timeLogged = true;
    Log::get().note(
        "supersample resolve: measured %.2f ms per eye on average (max "
        "%.2f) resolving %ux%u to %ux%u -- two dispatches, %s kernel at "
        "radius %.2f px (supersample_width moves it, live). That is Phase 0 "
        "item 6's price in docs\\anti-aliasing.md, measured; its softness "
        "half still wants the held-view comparison.",
        g_timeSum / static_cast<double>(g_timeCount), g_timeMax,
        g_lastRegionW, g_lastRegionH, g_lastOutW, g_lastOutH,
        filterName(g_lastFilter), static_cast<double>(g_lastWidth));
}

void pollTimingRing(ID3D11DeviceContext* ctx) {
    for (QuerySlot& q : g_qring) {
        if (!q.inUse) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        const HRESULT hrDj = ctx->GetData(q.disjoint, &dj, sizeof(dj),
                                          D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hrDj != S_OK) continue;
        UINT64 t0 = 0, t1 = 0;
        const HRESULT hr0 = ctx->GetData(q.begin, &t0, sizeof(t0),
                                         D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const HRESULT hr1 = ctx->GetData(q.end, &t1, sizeof(t1),
                                         D3D11_ASYNC_GETDATA_DONOTFLUSH);
        q.inUse = false;
        if (dj.Disjoint || hr0 != S_OK || hr1 != S_OK || dj.Frequency == 0) {
            continue;   // an unreliable or incomplete sample; discard it
        }
        const double ms = static_cast<double>(t1 - t0) * 1000.0 /
                          static_cast<double>(dj.Frequency);
        ++g_timeCount;
        g_timeSum += ms;
        if (ms > g_timeMax) g_timeMax = ms;
    }
    maybeLogTiming();
}

// A free slot, its queries created on first use; -1 when every slot is
// still in flight, and this call simply goes untimed.
int acquireQuerySlot(ID3D11Device* dev) {
    for (int i = 0; i < kQueryRing; ++i) {
        QuerySlot& q = g_qring[i];
        if (q.inUse) continue;
        if (!q.disjoint) {
            D3D11_QUERY_DESC qdd{};
            qdd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            D3D11_QUERY_DESC qdt{};
            qdt.Query = D3D11_QUERY_TIMESTAMP;
            const bool made = SUCCEEDED(dev->CreateQuery(&qdd, &q.disjoint)) &&
                              SUCCEEDED(dev->CreateQuery(&qdt, &q.begin)) &&
                              SUCCEEDED(dev->CreateQuery(&qdt, &q.end));
            if (!made) {
                releaseQuerySlot(q);
                continue;
            }
        }
        return i;
    }
    return -1;
}

// Same budget and same reasoning as the guard's crop and the theater: every
// call dereferences a handle the game owns, and a fault that repeats must
// retire the pass rather than bleed the log or, worse, the frame.
FaultBudget g_budget("supersamplePass", 8);

ID3D11ComputeShader* g_cs = nullptr;
bool                 g_csTried = false;
ID3D11Buffer*        g_cb = nullptr;

bool     g_failNoted = false;
bool     g_kindNoted = false;
bool     g_regionNoted = false;
bool     g_growNoted = false;
bool     g_fmtUnknownNoted = false;
bool     g_fmtChecked[kFormatCount] = {};
bool     g_fmtSupported[kFormatCount] = {};
bool     g_fmtUnsupportedNoted[kFormatCount] = {};
bool     g_midChecked = false;
bool     g_midSupported = false;
bool     g_firstNoted = false;
uint32_t g_treats = 0;

// The warm and the missing-hook note (supersamplePassConfigure/Tick).
bool     g_wanted = false;
char     g_mode[16] = "auto";
bool     g_warmNoted = false;
bool     g_noHookNoted = false;
uint64_t g_firstTickMs = 0;
constexpr uint64_t kNoHookNoteMs = 30000;

void failOnce(const char* what) {
    if (g_failNoted) return;
    g_failNoted = true;
    Log::get().note("supersample resolve: %s; the pass stands down.", what);
}

ID3D11ComputeShader* compileShader(ID3D11DeviceContext* ctx) {
    return shaderSwapCompileCs(ctx, kResolveCsHlsl, sizeof(kResolveCsHlsl) - 1,
                               "main", "supersample_resolve_cs", nullptr,
                               "supersample resolve");
}

bool makeTex(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT texFmt,
             DXGI_FORMAT viewFmt, UINT bindFlags, ID3D11Texture2D** outTex,
             ID3D11ShaderResourceView** outSrv,
             ID3D11UnorderedAccessView** outUav) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = texFmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bindFlags;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, outTex)) || !*outTex) {
        return false;
    }
    if (outSrv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = viewFmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(*outTex, &sd, outSrv))) {
            return false;
        }
    }
    if (outUav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = viewFmt;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateUnorderedAccessView(*outTex, &ud, outUav))) {
            return false;
        }
    }
    return true;
}

bool setParams(ID3D11DeviceContext* ctx, const PassParams& p) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) ||
        !m.pData) {
        return false;
    }
    memcpy(m.pData, &p, sizeof(p));
    ctx->Unmap(g_cb, 0);
    return true;
}

// One dispatch, SRV and UAV unbound first -- intro_upscale.cpp's runPass
// and the same reason: a texture cannot be an SRV and a UAV in the same
// dispatch, D3D silently nulls the SRV if asked, and the first pass's
// output is the second pass's input.
void runPass(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* in,
             ID3D11UnorderedAccessView* out, uint32_t w, uint32_t h) {
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 1, &nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShaderResources(0, 1, &in);
    ctx->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

void* resolveInner(void* srcTex, int eye, const float* bounds, uint32_t outW,
                   uint32_t outH, int filter, float widthIn, int gamma) {
    ID3D11Texture2D* src = nullptr;
    static_cast<IUnknown*>(srcTex)->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src));
    if (!src) return nullptr;

    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);

    bool ok = true;

    // The kinds this pass does not handle, refused rather than guessed at:
    // guardCropCopy's list, for its reasons.
    if (sd.SampleDesc.Count > 1 || sd.ArraySize != 1 || sd.MipLevels != 1) {
        ok = false;
        if (!g_kindNoted) {
            g_kindNoted = true;
            Log::get().note(
                "supersample resolve: the submitted texture is %ux%u "
                "samples=%u array=%u mips=%u, a kind the pass does not "
                "handle. The pass stands down.",
                sd.Width, sd.Height, sd.SampleDesc.Count, sd.ArraySize,
                sd.MipLevels);
        }
    }

    // The eye's region, from the bounds -- the double-wide and the flipped
    // cases both land here (supersample_math.h).
    uint32_t region[4] = {};
    if (ok && !supersampleRegionFromBounds(sd.Width, sd.Height, bounds,
                                           region, nullptr, nullptr)) {
        ok = false;
        if (!g_regionNoted) {
            g_regionNoted = true;
            Log::get().note(
                "supersample resolve: the Submit bounds (u %.3f..%.3f, v "
                "%.3f..%.3f) name no usable eye region of a %ux%u texture. "
                "The pass stands down.",
                bounds ? bounds[0] : 0.0f, bounds ? bounds[2] : 1.0f,
                bounds ? bounds[1] : 0.0f, bounds ? bounds[3] : 1.0f,
                sd.Width, sd.Height);
        }
    }
    const uint32_t regionW = ok ? region[2] - region[0] : 0;
    const uint32_t regionH = ok ? region[3] - region[1] : 0;

    // Shrink only. Asked to grow, or to do nothing, the pass refuses: an
    // upscale is performance.md's feature with its own reconstruction, and
    // a 1:1 pass would be a blur with no supersampling behind it.
    if (ok && (!outW || !outH || outW > regionW || outH > regionH ||
               (outW == regionW && outH == regionH))) {
        ok = false;
        if (!g_growNoted) {
            g_growNoted = true;
            Log::get().note(
                "supersample resolve: asked for %ux%u from a %ux%u region -- "
                "this pass only shrinks, and only when there is something "
                "to shrink. The pass stands down.",
                outW, outH, regionW, regionH);
        }
    }

    // The format allowlist.
    int fmtIndex = -1;
    DXGI_FORMAT viewFmt = DXGI_FORMAT_UNKNOWN;
    if (ok) {
        viewFmt = viewFormatOf(sd.Format, &fmtIndex);
        if (fmtIndex < 0) {
            ok = false;
            if (!g_fmtUnknownNoted) {
                g_fmtUnknownNoted = true;
                Log::get().note(
                    "supersample resolve: the submitted texture's format is "
                    "%s (DXGI_FORMAT %d), one this pass does not handle -- "
                    "unmeasured formats are refused, not assumed (docs\\"
                    "anti-aliasing.md Phase 0 item 2). The pass stands down; "
                    "please report this log.",
                    formatName(sd.Format), static_cast<int>(sd.Format));
            }
        }
    }

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (ok) {
        src->GetDevice(&dev);
        if (dev) dev->GetImmediateContext(&ctx);
        ok = dev != nullptr && ctx != nullptr;
    }

    // Drain whatever the timing ring already has, whatever this call
    // itself does: the ring's job is to keep the readback moving.
    if (ok) pollTimingRing(ctx);

    // Typed UAV stores on the family's plain format, and on the float16
    // intermediate, are what the two dispatches need; neither is promised
    // by every feature level for every family, so ask once.
    if (ok && !g_fmtChecked[fmtIndex]) {
        g_fmtChecked[fmtIndex] = true;
        UINT support = 0;
        g_fmtSupported[fmtIndex] =
            SUCCEEDED(dev->CheckFormatSupport(viewFmt, &support)) &&
            (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0;
    }
    if (ok && !g_fmtSupported[fmtIndex]) {
        ok = false;
        if (!g_fmtUnsupportedNoted[fmtIndex]) {
            g_fmtUnsupportedNoted[fmtIndex] = true;
            Log::get().note(
                "supersample resolve: this GPU/driver reports no typed "
                "unordered-access support for %s, so the result cannot be "
                "written here. The pass stands down.",
                formatName(viewFmt));
        }
    }
    if (ok && !g_midChecked) {
        g_midChecked = true;
        UINT support = 0;
        g_midSupported =
            SUCCEEDED(dev->CheckFormatSupport(DXGI_FORMAT_R16G16B16A16_FLOAT,
                                              &support)) &&
            (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0;
        if (!g_midSupported) {
            failOnce("this GPU/driver reports no typed unordered-access "
                     "support for R16G16B16A16_FLOAT, the intermediate the "
                     "two passes meet in");
        }
    }
    ok = ok && g_midSupported;

    // The shader, compiled once and cached. A session that calls through
    // the export without supersamplePassTick having warmed it (the smoke
    // harness) still works, paying the compile on the first call.
    if (ok && !g_cs && !g_csTried) {
        g_csTried = true;
        g_cs = compileShader(ctx);
    }
    ok = ok && g_cs != nullptr;

    if (ok && !g_cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(PassParams);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
        if (!ok) failOnce("the parameter buffer could not be created");
    }

    EyeState* eptr = ok ? &g_eye[eye] : nullptr;

    // The input view: over the source directly when it allows one, else
    // the eye's region copied into an owned texture and viewed there.
    ID3D11ShaderResourceView* inSrv = nullptr;
    bool viaCopy = false;
    if (ok) {
        EyeState& e = *eptr;
        if (sd.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
            if (e.srcRes != static_cast<void*>(src) || !e.srcSrv) {
                releaseSrc(e);
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = viewFmt;
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = 1;
                if (SUCCEEDED(dev->CreateShaderResourceView(src, &vd, &e.srcSrv)) &&
                    e.srcSrv) {
                    e.srcRes = src;
                } else {
                    e.srcSrv = nullptr;
                }
            }
            inSrv = e.srcSrv;
        }
        if (!inSrv) {
            viaCopy = true;
            if (!e.copyTex || e.copyW != regionW || e.copyH != regionH ||
                e.copyFmt != sd.Format) {
                releaseCopy(e);
                if (makeTex(dev, regionW, regionH, sd.Format, viewFmt,
                            D3D11_BIND_SHADER_RESOURCE, &e.copyTex, &e.copySrv,
                            nullptr)) {
                    e.copyW = regionW;
                    e.copyH = regionH;
                    e.copyFmt = sd.Format;
                } else {
                    releaseCopy(e);
                }
            }
            inSrv = e.copySrv;
        }
        if (!inSrv) {
            ok = false;
            failOnce("the submitted texture refuses a shader view and could "
                     "not be copied");
        }
    }
    if (ok) {
        EyeState& e = *eptr;
        if (!e.midTex || e.midW != outW || e.midH != regionH) {
            releaseMid(e);
            if (makeTex(dev, outW, regionH, DXGI_FORMAT_R16G16B16A16_FLOAT,
                        DXGI_FORMAT_R16G16B16A16_FLOAT,
                        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                        &e.midTex, &e.midSrv, &e.midUav)) {
                e.midW = outW;
                e.midH = regionH;
            } else {
                releaseMid(e);
                ok = false;
                failOnce("the intermediate texture could not be created");
            }
        }
    }
    if (ok) {
        EyeState& e = *eptr;
        if (!e.outTex || e.outW != outW || e.outH != outH ||
            e.outFmt != sd.Format) {
            releaseOut(e);
            if (makeTex(dev, outW, outH, sd.Format, viewFmt,
                        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                        &e.outTex, nullptr, &e.outUav)) {
                e.outW = outW;
                e.outH = outH;
                e.outFmt = sd.Format;
            } else {
                releaseOut(e);
                ok = false;
                failOnce("the output texture could not be created");
            }
        }
    }

    void* result = nullptr;
    if (ok) {
        EyeState& e = *eptr;
        const float width = supersampleEffectiveWidth(filter, widthIn);
        const int filt = filter == kSupersampleCrisp ? 1 : 0;
        // Gamma content is decoded on the way in and encoded on the way
        // out; a float texture is linear already and the hint is moot.
        const bool linearise = gamma != 0 && viewFmt != DXGI_FORMAT_R16G16B16A16_FLOAT;

        // The compute stage is saved and put back around both dispatches:
        // the exposure fix and others own slots here and must not find
        // ours (intro_upscale.cpp's runChain, same reasoning).
        ID3D11ComputeShader* savedCs = nullptr;
        ID3D11ShaderResourceView* savedSrv = nullptr;
        ID3D11UnorderedAccessView* savedUav = nullptr;
        ID3D11Buffer* savedCb = nullptr;
        ctx->CSGetShader(&savedCs, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 1, &savedSrv);
        ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
        ctx->CSGetConstantBuffers(0, 1, &savedCb);

        const int qs = acquireQuerySlot(dev);
        if (qs >= 0) {
            ctx->Begin(g_qring[qs].disjoint);
            ctx->End(g_qring[qs].begin);
        }

        // The copy-through, when the source refused a view: the region
        // out, verbatim, on the immediate context behind this frame's
        // rendering -- the guard's crop, same reasoning.
        if (viaCopy) {
            D3D11_BOX box{};
            box.left = region[0];
            box.top = region[1];
            box.front = 0;
            box.right = region[2];
            box.bottom = region[3];
            box.back = 1;
            ctx->CopySubresourceRegion(e.copyTex, 0, 0, 0, 0, src, 0, &box);
        }

        ctx->CSSetShader(g_cs, nullptr, 0);
        ID3D11Buffer* cb = g_cb;
        ctx->CSSetConstantBuffers(0, 1, &cb);

        // Pass 1, horizontal: the eye's region of the source (or all of
        // the copy) to outW x regionH.
        PassParams p{};
        if (viaCopy) {
            p.region[0] = 0;
            p.region[1] = 0;
            p.region[2] = static_cast<int32_t>(regionW);
            p.region[3] = static_cast<int32_t>(regionH);
        } else {
            for (int i = 0; i < 4; ++i) p.region[i] = static_cast<int32_t>(region[i]);
        }
        p.outSize[0] = static_cast<int32_t>(outW);
        p.outSize[1] = static_cast<int32_t>(regionH);
        p.dir = 0;
        p.filt = filt;
        p.scale = static_cast<float>(regionW) / static_cast<float>(outW);
        p.width = width;
        p.decode = linearise ? 1 : 0;
        p.encode = 0;
        bool ran = setParams(ctx, p);
        if (ran) runPass(ctx, inSrv, e.midUav, outW, regionH);

        // Pass 2, vertical: the intermediate to outW x outH, encoded back.
        if (ran) {
            p.region[0] = 0;
            p.region[1] = 0;
            p.region[2] = static_cast<int32_t>(outW);
            p.region[3] = static_cast<int32_t>(regionH);
            p.outSize[0] = static_cast<int32_t>(outW);
            p.outSize[1] = static_cast<int32_t>(outH);
            p.dir = 1;
            p.scale = static_cast<float>(regionH) / static_cast<float>(outH);
            p.decode = 0;
            p.encode = linearise ? 1 : 0;
            ran = setParams(ctx, p);
            if (ran) runPass(ctx, e.midSrv, e.outUav, outW, outH);
        }

        // SEAM: sharpening. docs/performance.md's render_sharpness, when it
        // lands in this tree, runs AMD's RCAS here on e.outTex at that
        // strength -- the vendored FSR under src/d3d11/fsr/, the wrapper
        // intro_upscale.cpp already carries -- into a second owned
        // texture, which then becomes the result. The key is not in this
        // tree and is not invented here; until it is, the resolve's own
        // output is what goes out.

        if (qs >= 0) {
            ctx->End(g_qring[qs].end);
            ctx->End(g_qring[qs].disjoint);
            g_qring[qs].inUse = true;
        }

        ID3D11ShaderResourceView* nullSrv = nullptr;
        ID3D11UnorderedAccessView* nullUav = nullptr;
        ctx->CSSetShaderResources(0, 1, &nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        ctx->CSSetShader(savedCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &savedSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &savedUav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &savedCb);
        if (savedCs) savedCs->Release();
        if (savedSrv) savedSrv->Release();
        if (savedUav) savedUav->Release();
        if (savedCb) savedCb->Release();

        // A parameter write that fails means a dispatch never ran, and a
        // texture nothing dispatched into must never go out as a frame:
        // the call answers null and the openvr half stands down.
        if (ran) {
            result = e.outTex;
            ++g_treats;
            g_lastRegionW = regionW;
            g_lastRegionH = regionH;
            g_lastOutW = outW;
            g_lastOutH = outH;
            g_lastFilter = filt;
            g_lastWidth = width;
            if (!g_firstNoted) {
                g_firstNoted = true;
                // The measurement docs/anti-aliasing.md's Phase 0 asks for
                // (item 2, the eye-texture format): a success prints
                // nothing else, so it would otherwise never reach a log.
                Log::get().note(
                    "supersample resolve: first resolved frame -- the game "
                    "submits %s (DXGI_FORMAT %d), read and written through "
                    "%s views%s; a %ux%u region resolved to %ux%u with the "
                    "%s kernel at radius %.2f px%s. Phase 0 item 2 of "
                    "docs\\anti-aliasing.md, the submitted format: measured.",
                    formatName(sd.Format), static_cast<int>(sd.Format),
                    formatName(viewFmt),
                    viaCopy ? " (copied out first: the source refuses a "
                              "shader view)"
                            : "",
                    regionW, regionH, outW, outH, filterName(filt),
                    static_cast<double>(width),
                    linearise ? ", in linear light (sRGB-decoded around the "
                                "filter, as the compositor's own sampler "
                                "would)"
                              : ", on the stored values");
            }
        } else {
            failOnce("the parameter buffer could not be written");
        }
    }

    if (ctx) ctx->Release();
    if (dev) dev->Release();
    src->Release();
    return result;
}

}  // namespace edvr::(anonymous)

void supersamplePassConfigure(Config& cfg) {
    const std::string mode = cfg.getString("experimental.supersample_resolve", "auto");
    g_wanted = _stricmp(mode.c_str(), "off") != 0 && !mode.empty();
    strncpy_s(g_mode, mode.c_str(), _TRUNCATE);
}

void supersamplePassTick(ID3D11DeviceContext* ctx) {
    if (!g_wanted || !ctx) return;
    if (g_firstTickMs == 0) g_firstTickMs = stampMs();
    if (!g_cs && !g_csTried) {
        g_csTried = true;
        g_cs = compileShader(ctx);
        if (g_cs && !g_warmNoted) {
            g_warmNoted = true;
            Log::get().note(
                "supersample resolve: shader warmed at session start -- the "
                "first resolved eye pays no compile.");
        }
    }
    // The other half's absence, said from this side: the resolve runs in
    // the openvr half's Submit hook, and if none has announced itself half
    // a minute in, there is none -- openvr_api.dll not installed, or every
    // feature that needs its hook was off when the game started, which is
    // how the setting can be flipped on live and do nothing.
    if (!g_noHookNoted && !glitchConsumerPresent() &&
        elapsedMs(g_firstTickMs, kNoHookNoteMs)) {
        g_noHookNoted = true;
        Log::get().note(
            "supersample resolve: experimental.supersample_resolve is %s, but no "
            "compositor hook has announced itself after %llu s. The resolve "
            "runs inside the openvr_api.dll half's Submit hook -- install "
            "that file, or restart the game with the setting on so the hook "
            "installs for it. Nothing is resolved until then.",
            g_mode, static_cast<unsigned long long>(kNoHookNoteMs / 1000));
    }
}

bool supersamplePassTotals(uint32_t* treated, double* avgMs, double* maxMs) {
    if (g_treats == 0) return false;
    if (treated) *treated = g_treats;
    if (avgMs) *avgMs = g_timeCount ? g_timeSum / static_cast<double>(g_timeCount) : 0.0;
    if (maxMs) *maxMs = g_timeMax;
    return true;
}

void supersamplePassShutdown() {
    if (g_treats > 0) {
        Log::get().note(
            "supersample resolve: %u eye-submits resolved this session%s.",
            g_treats,
            g_timeCount ? "" : " (no timing sample completed)");
        if (g_timeCount) {
            Log::get().note(
                "supersample resolve: measured %.2f ms per eye on average "
                "(max %.2f) over %u timed passes.",
                g_timeSum / static_cast<double>(g_timeCount), g_timeMax,
                g_timeCount);
        }
    }
    for (EyeState& e : g_eye) releaseEye(e);
    for (QuerySlot& q : g_qring) releaseQuerySlot(q);
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_cs) { g_cs->Release(); g_cs = nullptr; }
}

}  // namespace edvr

extern "C" __declspec(dllexport) void* edvrSupersampleResolve(
    void* srcTex, int eye, const float* bounds, unsigned outW, unsigned outH,
    int filter, float width, int gamma) {
    if (!srcTex || eye < 0 || eye > 1) return nullptr;
    void* out = nullptr;
    edvr::guardedBudget(edvr::g_budget, [&] {
        out = edvr::resolveInner(srcTex, eye, bounds, outW, outH, filter, width,
                                 gamma);
    });
    return out;
}
