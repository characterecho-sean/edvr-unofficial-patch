#include "temporal_pass.h"
#include "temporal_history.h"
#include "../common/runtime_profile.h"
#include "draw_census.h"

#include <algorithm>  // std::sort, the price report's median/p95
#include <cmath>
#include <cstdio>
#include <cstdlib>   // strtof, advanced.temporal_aa_fovea's "edges" vs a width
#include <cstring>
#include <utility>   // std::swap, for the depth carry's pointer swap
#include <vector>    // the eye dump's row buffer
#include <mutex>

#include <windows.h>

#include <d3d11.h>
#include <cstdarg>

#include "../common/config.h"
#include "../common/frame_flag.h"   // fssChromeStampValue: the scanner's screen is up this frame
#include "../common/temporal_mode.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "device_hook.h"   // the auto mip bias's source, to check against a real frame
#include "ui_surfaces.h"   // nativeTemporalRecommended: what Elite was told, for that check
#include "../common/native_render_settings.h"   // the published per-eye render size, for the NGX warm-up
#include "../common/native_frame.h"   // nativeFrameFovTrimDegrees: the FOV trim, for edges-mode fovea
#include "../common/supersample_math.h"   // supersampleRegionFromBounds: one region rule at the door
#include "../common/temporal_math.h"
#include "depth_probe.h"
#include "luma_probe.h"
#include "dlaa.h"
#include "fsr3_engine.h"
#include "object_probe.h"   // objectProbeArmLedger, objectProbeLedgerMark: the eye run's draw ledger
#include "pixel_probe.h"    // pixelProbeArm: who drew this pixel, the same eye run
#include "ui_depth.h"   // uiDepthReactiveMask: the interface's bias mask
#include "ui_resolve.h"
#include "screen_motion.h"
#include "weapon_motion.h"
#include "celestial_motion.h"
#include "cs_stage_save.h"
#include "engine_velocity.h"
#include "scheduler_stack_probe.h"
#include "static_prop_gate.h"
#include "perf_monitor.h"
#include "shader_swap.h"
#include "gpu_timing.h"
#include "gpu_census.h"   // issue #38: the per-feature GPU cost census
#include "temporal_shader_bytecode.h"

namespace edvr {
static void beginEyeRun();

// temporalPassWantsFssChrome reads this from the header with no call:
// asked by vscreen.cpp's beginPanelOverride, and the build has no /GL to
// fold a cross-TU getter.
namespace detail {
bool g_temporalPassWantedFssChrome = false;
}  // namespace detail

namespace {

// The fovea composite (docs/performance.md feature 6), its own tiny shader
// so its resource registers do not collide with the pass's above. NVIDIA
// ran on a crop of the frame -- the fovea -- and the periphery is either
// NVIDIA's DLAA on a reduced copy of the frame (the steady periphery) or
// the pass's own history (the sharp one); this blends the two, full NVIDIA
// inside the fovea, easing to the periphery over the last `band` pixels
// before the fovea's edge so the seam is never a hard line. The fovea is a
// disc (an ellipse where a clamped crop is not square) by default: the eye
// finds a straight edge and a corner at a far lower contrast than a smooth
// radial gradient, which is why a square fovea read as "a square" under a
// small head movement (2026-09-05). Outside the fovea NVIDIA's crop holds
// nothing, and the weight is zero there, so its texture is read only where
// it is valid. A periphery smaller than the output is upscaled here: a
// Catmull-Rom bicubic (nine bilinear taps) when it is, a plain fetch at 1:1.
// With the own periphery at 1:1 the blended result is also written back into
// the own history, so content that leaves the fovea carries NVIDIA's
// converged pixels into the periphery and decays there over frames instead
// of stepping at the seam.
constexpr char kFoveaCsHlsl[] = R"HLSL(
Texture2D<float4> PERIPH : register(t0);   // the periphery: NVIDIA's reduced DLAA or the own history, any size
Texture2D<float4> FOVEA  : register(t1);   // NVIDIA's output, native size, valid in the crop
RWTexture2D<float4> FO   : register(u0);   // the composited native frame, game format
RWTexture2D<float4> HIST : register(u1);   // the own history being written this frame (mode.y)
SamplerState SMP : register(s0);           // bilinear clamp, for the periphery upscale
cbuffer FC : register(b0) {
    float4 crop;   // output crop x0 y0 x1 y1 in native pixels, x1/y1 exclusive
    float4 band;   // x the blend band in pixels; y the output width, z the output height; w the periphery's width
    float4 disc;   // xy the fovea's centre in native pixels, zw its half-extents (the ellipse's semi-axes)
    float4 mode;   // x 1 = round fovea (else the rectangle); y 1 = write the history too; z 1 = the periphery is smaller than the output (bicubic); w the periphery's height
};
// Catmull-Rom through nine bilinear fetches (the pass's own kernel, C = 0.5),
// for a periphery smaller than the output: sharper than the bilinear
// upscale, mild ringing, the standard upscaling kernel.
float4 periphCubic(float2 uv, float2 tsize) {
    const float C = 0.5;
    float2 sp = uv * tsize;
    float2 t1 = floor(sp - 0.5) + 0.5;
    float2 f = sp - t1;
    float2 g0 = 1.0 + f;
    float2 g3 = 2.0 - f;
    float2 w0 = C * (-g0 * g0 * g0 + 5.0 * g0 * g0 - 8.0 * g0 + 4.0);
    float2 w1 = (2.0 - C) * f * f * f + (C - 3.0) * f * f + 1.0;
    float2 h = 1.0 - f;
    float2 w2 = (2.0 - C) * h * h * h + (C - 3.0) * h * h + 1.0;
    float2 w3 = C * (-g3 * g3 * g3 + 5.0 * g3 * g3 - 8.0 * g3 + 4.0);
    float2 w12 = w1 + w2;
    float2 o12 = w2 / w12;
    float2 t0 = (t1 - 1.0) / tsize;
    float2 t3 = (t1 + 2.0) / tsize;
    float2 t12 = (t1 + o12) / tsize;
    float4 r = 0.0;
    r += PERIPH.SampleLevel(SMP, float2(t0.x, t0.y), 0) * w0.x * w0.y;
    r += PERIPH.SampleLevel(SMP, float2(t12.x, t0.y), 0) * w12.x * w0.y;
    r += PERIPH.SampleLevel(SMP, float2(t3.x, t0.y), 0) * w3.x * w0.y;
    r += PERIPH.SampleLevel(SMP, float2(t0.x, t12.y), 0) * w0.x * w12.y;
    r += PERIPH.SampleLevel(SMP, float2(t12.x, t12.y), 0) * w12.x * w12.y;
    r += PERIPH.SampleLevel(SMP, float2(t3.x, t12.y), 0) * w3.x * w12.y;
    r += PERIPH.SampleLevel(SMP, float2(t0.x, t3.y), 0) * w0.x * w3.y;
    r += PERIPH.SampleLevel(SMP, float2(t12.x, t3.y), 0) * w12.x * w3.y;
    r += PERIPH.SampleLevel(SMP, float2(t3.x, t3.y), 0) * w3.x * w3.y;
    return r;
}
[numthreads(8, 8, 1)]
void fovea(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)band.y || id.y >= (uint)band.z) return;
    int2 p = int2(id.xy);
    float b = max(band.x, 1.0);
    float w;
    if (mode.x != 0.0) {
        // The disc: the normalised radius in the ellipse inscribed in the
        // crop, turned back into pixels inside its edge along the minor axis.
        float2 n = (float2(p) + 0.5 - disc.xy) / max(disc.zw, 1.0);
        float d = (1.0 - length(n)) * min(disc.z, disc.w);
        w = saturate(d / b);
    } else {
        float dx = min((float)p.x - crop.x, crop.z - 1.0 - (float)p.x);
        float dy = min((float)p.y - crop.y, crop.w - 1.0 - (float)p.y);
        w = saturate(min(dx, dy) / b);     // pixels inside the crop's nearest edge
    }
    w = w * w * (3.0 - 2.0 * w);           // smoothstep across the band
    // The periphery is sampled by normalised uv, so any size lands on the
    // native output: bicubic when it is smaller, an exact fetch at 1:1 (the
    // uv lands on texel centres). The fovea is native, read where it is
    // valid (strictly inside the fovea, where w > 0).
    float2 uv = (float2(p) + 0.5) / float2(band.y, band.z);
    float4 per = mode.z != 0.0 ? periphCubic(uv, float2(band.w, mode.w))
                               : PERIPH.SampleLevel(SMP, uv, 0);
    float4 fov = w > 0.0 ? FOVEA.Load(int3(p, 0)) : per;
    float4 o = lerp(per, fov, w);
    FO[p] = o;
    // The hand-off into the own history (mode.y, the sharp periphery at 1:1):
    // inside the fovea and its band the history takes the blended result.
    if (mode.y != 0.0 && w > 0.0) HIST[p] = float4(o.rgb, 1.0);
}
)HLSL";

// The steady periphery's reduction (feature 6): the render-size colour,
// depth and motion the trained path built, resampled down to the
// periphery's size for its own DLAA. An AREA-WEIGHTED box: each reduced
// pixel's footprint is exactly [p * ratio, (p + 1) * ratio) in render
// pixels, and every render pixel it overlaps contributes by its overlap, so
// the sample sits exactly where the reduced pixel's centre says it does at
// ANY ratio. (The first build dropped a 2x2 box at floor(p * ratio), exact
// only at a half scale; at 0.7 that put each sample up to 0.6 render pixels
// off in a seven-pixel pattern, a real displacement of the periphery
// against the fovea that the 2026-09-05 flight saw as a shift and, as
// content slid across the pattern under head motion, as a lag before the
// two agreed again.) Colour is the weighted mean; depth the footprint's
// NEAREST (reversed-Z: the largest), which is the dilation every
// depth-aware filter does at an edge; the motion is the vector of that
// nearest sample, scaled into the reduced pixels, so depth and motion stay
// the same surface's.
constexpr char kDownCsHlsl[] = R"HLSL(
Texture2D<float4> DC : register(t0);    // the colour, render size
Texture2D<float>  DZ : register(t1);    // the depth copy, render size
Texture2D<float2> DM : register(t2);    // the motion vectors, render pixels
RWTexture2D<float4> PC : register(u0);  // the reduced colour
RWTexture2D<float>  PZ : register(u1);  // the reduced depth
RWTexture2D<float2> PM : register(u2);  // the reduced motion, reduced pixels
cbuffer DS : register(b0) {
    float4 dims;   // x reduced width, y reduced height, z render width, w render height
    float4 par;    // x unused (was the block side), y the scale (reduced / render), zw unused
};
[numthreads(8, 8, 1)]
void down(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)dims.x || id.y >= (uint)dims.y) return;
    float2 q = dims.zw / dims.xy;                    // render pixels per reduced pixel, > 1
    float2 x0 = float2(id.xy) * q;                   // the footprint [x0, x1) in render pixels
    float2 x1 = x0 + q;
    int2 k0 = int2(floor(x0));
    int2 k1 = min(int2(ceil(x1)), int2(dims.zw));    // exclusive
    float4 c = 0.0;
    float wsum = 0.0;
    float zmax = -1.0;
    float2 mv = 0.0;
    [loop] for (int y = k0.y; y < k1.y; ++y) {
        float wy = min((float)y + 1.0, x1.y) - max((float)y, x0.y);
        [loop] for (int x = k0.x; x < k1.x; ++x) {
            float wx = min((float)x + 1.0, x1.x) - max((float)x, x0.x);
            float w = wx * wy;
            if (w <= 0.0) continue;
            int2 s = int2(x, y);
            c += DC.Load(int3(s, 0)) * w;
            wsum += w;
            float z = DZ.Load(int3(s, 0));
            if (z > zmax) {
                zmax = z;
                mv = DM.Load(int3(s, 0));
            }
        }
    }
    PC[id.xy] = c / max(wsum, 1e-6);
    PZ[id.xy] = max(zmax, 0.0);
    PM[id.xy] = mv * par.y;
}
)HLSL";

// The cbuffer above, laid out to match: 528 bytes, thirty-three 16-byte rows
// (the estimated body, ship and stepped-part rows retired 2026-09-23).
struct PassParams {
    int32_t region[4];
    int32_t size[2];
    int32_t texSize[2];
    float   tanNow[4];
    float   tanPrev[4];
    float   jit[4];
    float   dR0[4];
    float   dR1[4];
    float   dR2[4];
    float   cand[4][3][4];   // candidate, row, xyz + pad
    float   blend;
    float   gamma;
    int32_t haveHistory;
    int32_t candMask;
    float   knobs[4];
    float   tvUsed[4];
    float   tvCand[4];
    float   tvCam[4];    // the camera rows' translation term, w 1 = world path on
    float   split[4];    // x the ship's radius in metres
    float   fovea0[4];   // xy centre px, z inner radius px, w 1/ramp px (feature 6, periphery calming)
    float   fovea1[4];   // x calm strength 0..1, w 1 = fovea on
    float   movers[4];   // x 1 = mover mask on, y tolerance (fraction), z strength 0..1, w 1 = main writes the depth copy (tier 1, docs/per-object-motion.md)
    float   probe[4];    // x the history's scale for the mv entry's probes (outW / w), y 1 = run them (NVIDIA's previous output bound at t1)
    float   holoJitter[4]; // xy raster delta, z consecutive frames, w valid DLSS depth history
    float   skip[4];    // the periphery's own-resolve early-out, x0 y0 x1 y1 in RENDER pixels (this eye's w x h); all zero = no skip
    float   lead[4];    // xy the fovea crop's base slide this frame in RENDER pixels (advanced.temporal_aa_fovea_lead), added to the vectors ML carries for NVIDIA's crop; zero on every dispatch but the fovea prep's
};
static_assert(sizeof(PassParams) == 528, "the cbuffer is 33 16-byte rows");

// The format allowlist -- typeless and UNORM families read and written
// through the family's plain typed view, the source's own format kept on
// the output, sRGB-typed sources refused.
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

const char* motionName(int motion) {
    return motion == 3 ? "head with depth" : motion == 1 ? "head" : "none";
}

// Per-eye owned resources. Release-before-recreate on any size or format
// change; a change of size is also a reset of the history, which cannot
// mean anything across a resize.
struct EyeState {
    ID3D11Texture2D* uiHistory[2] = {};
    ID3D11ShaderResourceView* uiHistorySrv[2] = {};
    ID3D11UnorderedAccessView* uiHistoryUav[2] = {};
    uint32_t uiHistoryW = 0, uiHistoryH = 0;
    int uiHistoryRead = 0;
    bool uiHistoryValid = false;
    void*                      srcRes = nullptr;   // the game's texture the view is over (identity)
    ID3D11ShaderResourceView*  srcSrv = nullptr;
    // The scene's depth for this eye, from the depth probe's held texture:
    // a view typed to the depth channel, keyed on the texture like the
    // source's view. Released when a different texture arrives.
    void*                      depthRes = nullptr;
    ID3D11ShaderResourceView*  depthSrv = nullptr;
    // For the trained pass: the colour copied out typed, the motion
    // vectors and the depth copy it is fed, and its output.
    ID3D11Texture2D*           dlColour = nullptr;
    ID3D11ShaderResourceView*  dlColourSrv = nullptr;   // the steady periphery's reduction reads these three
    ID3D11Texture2D*           dlMv = nullptr;
    ID3D11UnorderedAccessView* dlMvUav = nullptr;
    ID3D11ShaderResourceView*  dlMvSrv = nullptr;
    // The fovea's head lead (advanced.temporal_aa_fovea_lead): the SAME
    // vectors plus this frame's base slide, written beside dlMv by the same
    // dispatch (ML/u7) and handed to NVIDIA's crop in dlMv's place. A second
    // texture because dlMv has three other readers -- the steady periphery's
    // reduction, its DLAA at 1:1 and the UI resolve -- and none of them may
    // see the crop's slide. Made only while the key is on, released with the
    // rest of the dl set.
    ID3D11Texture2D*           dlMvLead = nullptr;
    ID3D11UnorderedAccessView* dlMvLeadUav = nullptr;
    // Exists only while an explicit eye run asks for the capture-only mv
    // variant. Its u7 replaces (never aliases) the fovea lead UAV.
    ID3D11Texture2D*           dlDecision = nullptr;
    ID3D11UnorderedAccessView* dlDecisionUav = nullptr;
    ID3D11Texture2D*           dlDepth = nullptr;
    ID3D11UnorderedAccessView* dlDepthUav = nullptr;
    ID3D11ShaderResourceView*  dlDepthSrv = nullptr;
    ID3D11Texture2D*           dlOut = nullptr;
    ID3D11UnorderedAccessView* dlOutUav = nullptr;   // the debug motion view paints here
    ID3D11ShaderResourceView*  dlOutSrv = nullptr;   // the fovea composite reads NVIDIA's crop through this
    ID3D11Texture2D*           dlSubmit = nullptr;  // NVIDIA's frame copied into the game's own format: what goes out. The fovea path's UI resolve (below, sharing the trained block's own logic) writes the composite's finished frame in here too, sized foW x foH, the same value as oW x oH
    ID3D11UnorderedAccessView* dlSubmitUav = nullptr;
    bool                      uiResolvedHistory = false;
    bool                      screenHistory = false;
    uint32_t                   dlW = 0, dlH = 0;
    uint32_t                   dlOutW = 0, dlOutH = 0;
    // Tier 1 of docs/per-object-motion.md (2026-09-08), the mover mask:
    // LAST frame's depth copy -- the twin of dlDepth, swapped with it after
    // every frame that wrote one, so no copy is ever made -- and the mask
    // NVIDIA is handed (R8_UNORM). zPrevValid says the swap happened last
    // frame at this size; a rebuild, a reset or a frame without a depth
    // write clears it, and the mask stays off until it is true again.
    // Both live and die with the dl set (releaseDl), whichever path made
    // it: the own path makes dlDepth alone when it needs the carry.
    ID3D11Texture2D*           zPrev = nullptr;
    ID3D11ShaderResourceView*  zPrevSrv = nullptr;
    ID3D11UnorderedAccessView* zPrevUav = nullptr;
    bool                       zPrevValid = false;
    ID3D11Texture2D*           dlMask = nullptr;
    ID3D11UnorderedAccessView* dlMaskUav = nullptr;
    // A shader view over the interface's reactive mask (ui_depth.h owns the
    // texture), for the mv entry to fold into dlMask: keyed on the texture's
    // identity, remade when ui_depth remakes it.
    void*                      uiMaskRes = nullptr;
    ID3D11ShaderResourceView*  uiMaskSrv = nullptr;
    ID3D11Texture2D*           copyTex = nullptr;  // the copy-through, for a source that refuses a view
    ID3D11ShaderResourceView*  copySrv = nullptr;
    uint32_t                   copyW = 0, copyH = 0;
    DXGI_FORMAT                copyFmt = DXGI_FORMAT_UNKNOWN;

    ID3D11Texture2D*           hist[2] = {};       // ping-pong: read one, write the other
    ID3D11ShaderResourceView*  histSrv[2] = {};
    ID3D11UnorderedAccessView* histUav[2] = {};
    int                        histRead = 0;
    bool                       haveHistory = false;
    // The trained pass's continuity (NVIDIA's history), kept apart from
    // the pass's own. The review of 2026-09-04 (docs/review-motion-vectors-2026-09-04.md,
    // F1) found the reset flag keyed on haveHistory, which the trained
    // path never sets: NVIDIA was told "the scene changed completely" on
    // every frame and never accumulated a thing.
    bool                       dlHaveHistory = false;
    int64_t                    dlLastQpc = 0;   // the previous evaluation, for NVIDIA's frame delta
    // The headset's poses and this eye's offset, noted by the openvr half
    // before each treat (edvrTemporalAaNoteHead): the world path's
    // composition with the ship's camera rows needs them.
    float                      headPrev[12] = {};
    float                      headNow[12] = {};
    float                      eyeOff[3] = {};
    bool                       headNoted = false;
    float                      rasterJitter[2] = {};
    uint32_t                   jitterFrame = UINT32_MAX;

    ID3D11Texture2D*           outTex = nullptr;
    ID3D11UnorderedAccessView* outUav = nullptr;
    ID3D11ShaderResourceView*  outSrv = nullptr;    // the fovea composite reads the own-history periphery through this

    // The fovea composite (docs/performance.md feature 6): the full frame
    // with NVIDIA's crop blended over the own-history periphery, in the
    // game's own format. Its own texture, not dlSubmit: the compose needs
    // an SRV to hand the shared UI resolve helper in NVIDIA's-output's
    // role (dlSubmit is a submit-only target, never bound as an SRV), and
    // a same-resource SRV+UAV bind is never safe within one dispatch.
    // dlSubmit above holds the FINAL frame once the resolve has run on
    // this texture; when it does not run, this texture is itself what goes
    // out.
    ID3D11Texture2D*           foveaOut = nullptr;
    ID3D11UnorderedAccessView* foveaOutUav = nullptr;
    ID3D11ShaderResourceView*  foveaOutSrv = nullptr;   // the shared UI resolve helper reads the composite through this, standing in for NVIDIA's output
    uint32_t                   foveaW = 0, foveaH = 0;
    // The crop's own NVIDIA history flag.
    bool                       foveaHaveHistory = false;
    // The steady periphery (feature 6): NVIDIA's DLAA on a reduced copy of
    // the frame. The reduced colour, depth and motion (only when the
    // reduction is real: at a small render the render-size inputs serve),
    // NVIDIA's reduced output the composite upscales, and its history flag.
    ID3D11Texture2D*           prColour = nullptr;
    ID3D11UnorderedAccessView* prColourUav = nullptr;
    ID3D11Texture2D*           prDepth = nullptr;
    ID3D11UnorderedAccessView* prDepthUav = nullptr;
    ID3D11Texture2D*           prMv = nullptr;
    ID3D11UnorderedAccessView* prMvUav = nullptr;
    ID3D11Texture2D*           prOut = nullptr;
    ID3D11ShaderResourceView*  prOutSrv = nullptr;
    ID3D11UnorderedAccessView* prOutUav = nullptr;
    uint32_t                   prW = 0, prH = 0;
    bool                       prReduced = false;   // prColour/prDepth/prMv exist (the reduction is real)
    bool                       prHaveHistory = false;

    uint32_t    w = 0, h = 0;
    DXGI_FORMAT outFmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT histFmt = DXGI_FORMAT_UNKNOWN;
};
EyeState g_eye[2];

void releaseSrc(EyeState& e) {
    if (e.srcSrv) { e.srcSrv->Release(); e.srcSrv = nullptr; }
    e.srcRes = nullptr;
}
void releaseDepth(EyeState& e) {
    if (e.depthSrv) { e.depthSrv->Release(); e.depthSrv = nullptr; }
    e.depthRes = nullptr;
}
void releasePeriph(EyeState& e) {
    if (e.prColourUav) { e.prColourUav->Release(); e.prColourUav = nullptr; }
    if (e.prColour) { e.prColour->Release(); e.prColour = nullptr; }
    if (e.prDepthUav) { e.prDepthUav->Release(); e.prDepthUav = nullptr; }
    if (e.prDepth) { e.prDepth->Release(); e.prDepth = nullptr; }
    if (e.prMvUav) { e.prMvUav->Release(); e.prMvUav = nullptr; }
    if (e.prMv) { e.prMv->Release(); e.prMv = nullptr; }
    if (e.prOutUav) { e.prOutUav->Release(); e.prOutUav = nullptr; }
    if (e.prOutSrv) { e.prOutSrv->Release(); e.prOutSrv = nullptr; }
    if (e.prOut) { e.prOut->Release(); e.prOut = nullptr; }
    e.prW = e.prH = 0;
    e.prReduced = false;
    e.prHaveHistory = false;
}
void releaseDl(EyeState& e) {
    releasePeriph(e);   // the periphery's sizes follow the render's
    if (e.zPrevUav) { e.zPrevUav->Release(); e.zPrevUav = nullptr; }
    if (e.zPrevSrv) { e.zPrevSrv->Release(); e.zPrevSrv = nullptr; }
    if (e.zPrev) { e.zPrev->Release(); e.zPrev = nullptr; }
    e.zPrevValid = false;
    if (e.dlMaskUav) { e.dlMaskUav->Release(); e.dlMaskUav = nullptr; }
    if (e.dlMask) { e.dlMask->Release(); e.dlMask = nullptr; }
    if (e.uiMaskSrv) { e.uiMaskSrv->Release(); e.uiMaskSrv = nullptr; }
    e.uiMaskRes = nullptr;
    if (e.dlColourSrv) { e.dlColourSrv->Release(); e.dlColourSrv = nullptr; }
    if (e.dlMvSrv) { e.dlMvSrv->Release(); e.dlMvSrv = nullptr; }
    if (e.dlDepthSrv) { e.dlDepthSrv->Release(); e.dlDepthSrv = nullptr; }
    if (e.dlMvUav) { e.dlMvUav->Release(); e.dlMvUav = nullptr; }
    if (e.dlMvLeadUav) { e.dlMvLeadUav->Release(); e.dlMvLeadUav = nullptr; }
    if (e.dlDecisionUav) { e.dlDecisionUav->Release(); e.dlDecisionUav = nullptr; }
    if (e.dlDepthUav) { e.dlDepthUav->Release(); e.dlDepthUav = nullptr; }
    if (e.dlColour) { e.dlColour->Release(); e.dlColour = nullptr; }
    if (e.dlMv) { e.dlMv->Release(); e.dlMv = nullptr; }
    if (e.dlMvLead) { e.dlMvLead->Release(); e.dlMvLead = nullptr; }
    if (e.dlDecision) { e.dlDecision->Release(); e.dlDecision = nullptr; }
    if (e.dlDepth) { e.dlDepth->Release(); e.dlDepth = nullptr; }
    if (e.dlOutUav) { e.dlOutUav->Release(); e.dlOutUav = nullptr; }
    if (e.dlOutSrv) { e.dlOutSrv->Release(); e.dlOutSrv = nullptr; }
    if (e.dlOut) { e.dlOut->Release(); e.dlOut = nullptr; }
    if (e.dlSubmit) { e.dlSubmit->Release(); e.dlSubmit = nullptr; }
    if (e.dlSubmitUav) { e.dlSubmitUav->Release(); e.dlSubmitUav = nullptr; }
    e.uiResolvedHistory = false;
    e.dlW = e.dlH = 0;
    e.dlOutW = e.dlOutH = 0;
    e.dlHaveHistory = false;
    e.dlLastQpc = 0;
}
void releaseCopy(EyeState& e) {
    if (e.copySrv) { e.copySrv->Release(); e.copySrv = nullptr; }
    if (e.copyTex) { e.copyTex->Release(); e.copyTex = nullptr; }
    e.copyW = e.copyH = 0;
    e.copyFmt = DXGI_FORMAT_UNKNOWN;
}
void releaseNative(EyeState& e) {
    for (int i = 0; i < 2; ++i) {
        if (e.histUav[i]) { e.histUav[i]->Release(); e.histUav[i] = nullptr; }
        if (e.histSrv[i]) { e.histSrv[i]->Release(); e.histSrv[i] = nullptr; }
        if (e.hist[i]) { e.hist[i]->Release(); e.hist[i] = nullptr; }
    }
    if (e.outUav) { e.outUav->Release(); e.outUav = nullptr; }
    if (e.outSrv) { e.outSrv->Release(); e.outSrv = nullptr; }
    if (e.outTex) { e.outTex->Release(); e.outTex = nullptr; }
    e.haveHistory = false;
    e.histRead = 0;
}
void releaseOwned(EyeState& e) {
    releaseNative(e);
    if (e.foveaOutUav) { e.foveaOutUav->Release(); e.foveaOutUav = nullptr; }
    if (e.foveaOutSrv) { e.foveaOutSrv->Release(); e.foveaOutSrv = nullptr; }
    if (e.foveaOut) { e.foveaOut->Release(); e.foveaOut = nullptr; }
    e.foveaW = e.foveaH = 0;
    e.foveaHaveHistory = false;
    releasePeriph(e);
    e.w = e.h = 0;
    e.outFmt = e.histFmt = DXGI_FORMAT_UNKNOWN;
    e.haveHistory = false;
    e.histRead = 0;
}
void releaseUiHistory(EyeState& e) {
    for (int k=0;k<2;++k) {
        if (e.uiHistorySrv[k]) { e.uiHistorySrv[k]->Release(); e.uiHistorySrv[k]=nullptr; }
        if (e.uiHistoryUav[k]) { e.uiHistoryUav[k]->Release(); e.uiHistoryUav[k]=nullptr; }
        if (e.uiHistory[k]) { e.uiHistory[k]->Release(); e.uiHistory[k]=nullptr; }
    }
    e.uiHistoryValid=false; e.uiHistoryRead=0; e.uiHistoryW=e.uiHistoryH=0;
}
void releaseEye(EyeState& e) {
    releaseUiHistory(e);
    releaseSrc(e);
    releaseDepth(e);
    releaseDl(e);
    releaseCopy(e);
    releaseOwned(e);
}

// One slot per treated call: the GPU price by timestamp query, and the
// pass's own count of rejected and clipped pixels copied out to a staging
// buffer. Never awaited (DONOTFLUSH, DO_NOT_WAIT); a slot still in flight
// is read on a later call, and a call that finds every slot busy runs
// unmeasured. Measuring must never be able to stall the pass.
// The price report's regions (docs/foveated-dlss-design-2026-09-14.md Stage
// 0): every dispatch or NGX call temporalInner makes for one eye, timed the
// same way. dlaa.cpp already prices the three NGX roles (full, centre,
// periphery) for Deliverable 2's pooled F8 figures, but only as a running
// count/mean/max -- there is no per-window distribution to read a median or
// p95 off, and no reset when a window here closes. So the three roles are
// timed a second time, right here, with the same per-eye GpuTimer this file
// uses for its own prep/reduce/compose/ui dispatches: the redundant timer is
// the price of a uniform sample -- one array, one poll cadence, one pairing
// path -- for all seven regions instead of four measured one way and three
// unmeasurable the way this deliverable needs.
enum class Region { Prep = 0, Reduce, Periphery, Centre, Full, Compose, Ui, Count };
constexpr int kRegionCount = static_cast<int>(Region::Count);
// Exact labels the price report and the F8 line key off (tools\edvr_log.py
// greps the log text, not this array, but the two must agree by hand).
const char* const kRegionNames[kRegionCount] = {
    "prep", "reduce", "periphery", "centre", "full", "compose", "ui"
};
// Prep's own parts, on the full-frame path (the engine-motion re-fly of
// 2026-09-23, log 093817: "prep" read ~3 ms a stereo pair in the cockpit
// with engine-record motion on against ~0.2 in menus and on foot, and one
// region cannot say which of its dispatches that is). The colour copies
// and the motion-vector dispatch, timed the same way and printed INSIDE
// prep's figure: they are parts of prep, so "other", the seven exported
// medians and the F8 line are unchanged. The rest of prep (the constants,
// the masks' lookups) is prep less the two. (A third part, stage B's
// kinematic coverage pass, went with stage B on 2026-09-23.)
enum class PrepPart { Copy = 0, Mv, Count };
constexpr int kPrepParts = static_cast<int>(PrepPart::Count);
const char* const kPrepPartNames[kPrepParts] = {"copy", "mv"};

// Which path this eye's call is routed through, decided once per call
// (temporalInner, alongside foveaMode) before either branch below runs, so
// a price report window resets on a real mode change and not on a frame
// where NGX happened to fail inside an unchanged mode.
enum class Treatment { None = 0, FullFrame, Fovea };

struct Slot {
    GpuTimer      timer;
    ID3D11Buffer* staging = nullptr;
    bool          inUse = false;
    bool          timeDone = false;
    bool          timing = false;
    bool          statsDone = false;
    bool          engineStats = false;   // Stats 50..54 hold engine-record velocity's counts
    uint64_t      pixels = 0;
    // The instrument's bookkeeping for this call: which candidates had a
    // delta (their pixel totals), the head's turn, whether history ran.
    uint64_t      candPixels[4] = {};
    float         headDeg = 0.0f;
    bool          hadHistory = false;
    // Stage 0 price report: this call's eye and the pass's own frame
    // counter (g_rowsFrame), for stereo pairing; the treatment and output
    // shape, for the window-reset test; and the per-region GPU price this
    // eye's call actually measured (0 and absent when the region did not
    // run, or its lease could not be had).
    int           eye = -1;
    uint32_t      frame = 0;
    Treatment     treatment = Treatment::None;
    // Which engine FullFrame actually was (Own when treatment is not
    // FullFrame): the price line's treatmentName cannot otherwise tell an
    // AMD full-frame run from an NVIDIA one, both Treatment::FullFrame.
    edvr::TemporalEngine engine = edvr::TemporalEngine::Own;
    bool          lean = false;   // the own path's dispatch was the lean shader
    // The motion/compose shader that ran was the diagnostic build (atomics,
    // counters) rather than the lean one -- the price line says which (the
    // performance review's attribution gap 4).
    bool          instrumented = false;
    uint32_t      outW = 0, outH = 0;
    uint32_t      fmt = 0;
    uint32_t      configGen = 0;
    GpuTimer      regionTimer[kRegionCount];
    bool          regionTiming[kRegionCount] = {};
    bool          regionEnded[kRegionCount] = {};
    bool          regionDone[kRegionCount] = {};
    double        regionMs[kRegionCount] = {};
    // Prep's parts, the same bookkeeping (PrepPart above).
    GpuTimer      partTimer[kPrepParts];
    bool          partTiming[kPrepParts] = {};
    bool          partEnded[kPrepParts] = {};
    bool          partDone[kPrepParts] = {};
    double        partMs[kPrepParts] = {};
    bool          regionsDone = false;
    // The total timer's own polled milliseconds, retained here (the poll
    // loop otherwise only folds it into the pooled globals below) so the
    // price report can pair it with the region prices above; priced guards
    // against a slot being folded into the window's samples twice.
    double        totalMs = 0.0;
    bool          priced = false;
    // Set only in pollSlots' Ready branch: totalMs actually came off the
    // GPU this call. A slot that never measures (no lease, an Invalid
    // poll) still reaches the pending ring so its twin is not left
    // waiting, but carries this false so the pair prices as dropped
    // (droppedUnmeasured) instead of pricing an unmeasured 0ms total.
    bool          totalValid = false;
};
constexpr int kSlots = 16;
constexpr int kStatCount = 56;   // 0-28 and 30-38 used; 29 and 39-49 free since the estimated body, ship and stepped-part paths retired (2026-09-23), the rest keeping their numbers; 50-54 engine-record velocity's pixel counts; a 224-byte buffer
Slot g_slots[kSlots];

// Bumped on every temporalPassConfigure call (both its call sites in
// vscreen.cpp): the price report's window key folds this in, so a live
// temporal_aa_* setting change closes the current window even when the
// treatment and output shape happen to read the same.
uint32_t g_configGeneration = 0;

void releaseSlot(Slot& q) {
    q.timer.reset();
    for (auto& t : q.regionTimer) t.reset();
    for (auto& t : q.partTimer) t.reset();
    if (q.staging) { q.staging->Release(); q.staging = nullptr; }
    q = Slot{};
}

// A region's begin/end, nested inside the enclosing total timer's own
// lease: only tried when the total timer itself got one (so a region never
// opens a standalone record of its own), and never blocking or asserting
// when the shared DisjointClock's 32-lease-per-record budget is out --
// dropped and counted instead, per docs/foveated-dlss-design-2026-09-14.md.
uint32_t g_regionBeginFailed = 0;
bool     g_regionBeginFailedNoted = false;
// Stage 0 price report: pairs and calls dropped before ever reaching
// accumulateWindow, broken out on the price line (flushWindow, below) so a
// low pair count is explained rather than silently absorbed. All three
// reset when a window closes, same as g_regionBeginFailed just above.
uint32_t g_droppedUnmeasured = 0;   // both eyes arrived, one or both unmeasured (F1)
uint32_t g_droppedLone = 0;         // an eye evicted or shut down with no twin (F2)
uint32_t g_droppedNoSlot = 0;       // gpuTimingOwns this call but every slot was busy (F3)

void beginRegion(int qs, Region r, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (qs < 0 || !g_slots[qs].timing) return;
    const int i = static_cast<int>(r);
    Slot& q = g_slots[qs];
    q.regionTiming[i] = q.regionTimer[i].begin(dev, ctx);
    if (!q.regionTiming[i]) {
        ++g_regionBeginFailed;
        if (!g_regionBeginFailedNoted) {
            g_regionBeginFailedNoted = true;
            Log::get().note(
                "temporal aa price: a region's GPU timer could not get a lease this frame "
                "(the shared clock's budget was busy). That sample is dropped from the price "
                "report; further drops this session are counted but not logged again.");
        }
    }
}

void endRegion(int qs, Region r, ID3D11DeviceContext* ctx) {
    if (qs < 0) return;
    const int i = static_cast<int>(r);
    Slot& q = g_slots[qs];
    if (q.regionTiming[i] && !q.regionTimer[i].end(ctx)) {
        q.regionTimer[i].reset(ctx);
        q.regionTiming[i] = false;
    }
    q.regionEnded[i] = true;
}

// A part of prep: the same lease rule as a region (only inside a call the
// total timer leased, a refused lease dropped and counted with the regions').
void beginPart(int qs, PrepPart part, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (qs < 0 || !g_slots[qs].timing) return;
    const int i = static_cast<int>(part);
    Slot& q = g_slots[qs];
    q.partTiming[i] = q.partTimer[i].begin(dev, ctx);
    if (!q.partTiming[i]) ++g_regionBeginFailed;
}

void endPart(int qs, PrepPart part, ID3D11DeviceContext* ctx) {
    if (qs < 0) return;
    const int i = static_cast<int>(part);
    Slot& q = g_slots[qs];
    if (q.partTiming[i] && !q.partTimer[i].end(ctx)) {
        q.partTimer[i].reset(ctx);
        q.partTiming[i] = false;
    }
    q.partEnded[i] = true;
}

// The window a price sample falls into: it closes (docs/foveated-dlss-
// design-2026-09-14.md Stage 0) on a treatment change, an output size or
// format change, or an NGX feature recreation. This build's feature
// recreation triggers are all a size (dlaa.h: "rebuilt on a size change")
// or a live setting read inside temporalPassConfigure (the model/preset
// among them) -- both already covered by outW/outH/fmt and configGen, so
// no separate "recreated" flag is tracked.
struct WindowKey {
    Treatment treatment = Treatment::None;
    edvr::TemporalEngine engine = edvr::TemporalEngine::Own;
    uint32_t  outW = 0, outH = 0;
    uint32_t  fmt = 0;
    uint32_t  configGen = 0;
    bool      lean = false;   // the own path ran the lean shader, not the instrumented one
    bool      instrumented = false;   // the diagnostic shader build ran (any path)
    bool operator==(const WindowKey& o) const {
        return treatment == o.treatment && engine == o.engine && outW == o.outW &&
               outH == o.outH && fmt == o.fmt && configGen == o.configGen &&
               lean == o.lean && instrumented == o.instrumented;
    }
    bool operator!=(const WindowKey& o) const { return !(*this == o); }
};

constexpr int kWindowPairs = 600;
WindowKey g_windowKey;
bool      g_windowKeyValid = false;
int       g_windowCount = 0;
double    g_windowTotal[kWindowPairs];
double    g_windowRegion[kRegionCount][kWindowPairs];
double    g_windowPart[kPrepParts][kWindowPairs];   // prep's parts, per pair (PrepPart)

// The most recently CLOSED window's per-region median, for the F8 line's
// live figures; kept even while the next window is still filling.
double    g_lastWindowMedian[kRegionCount] = {};
double    g_lastWindowOtherMedian = 0.0;
uint32_t  g_lastWindowPairs = 0;
bool      g_lastWindowValid = false;
// The four drop counters' sum for the window this closed (F5): unmeasured
// pairs, lone eyes, no-slot frames and region-lease failures, latched at
// close time after which the running counters reset for the next window.
uint32_t  g_lastWindowDropped = 0;

// THE HEAD LEAD (advanced.temporal_aa_fovea_lead). NVIDIA's crop history is
// crop-local, so content entering the rectangle at its leading edge has no
// history there and is soft for its first frames. The lead slides the
// rectangle into a turn of the head or the ship by this many FRAMES of the
// motion the frame's centre is under -- whichever rows the motion pass
// itself uses for far content there, so the two always agree -- and adds
// the slide to the vectors the crop reads, so the history it does have
// stays registered. The gaze is on the leading side of either turn, and
// the fresh strip is a fact about the content, not about the head.
//
// Declared up here, well above the rest of the fovea's settings (g_foveaDeg
// and friends below), because flushWindow -- the price report, right after
// this -- prints the window's lead line and resets its counters.
float g_foveaLeadFrames = 0.0f;   // the key: 0 = off, clamped to 0..12
bool  g_foveaLeadNoted = false;   // the "the head lead is on" line, once per config load
struct FoveaLeadState {
    float    px[2] = {0.0f, 0.0f};   // the smoothed lead, render pixels (one-pole, 0.25)
    int32_t  base[2] = {0, 0};       // the base the crop last RAN at, render pixels
    uint32_t cropW = 0, cropH = 0;   // ...and the crop size it ran at
    bool     valid = false;          // base/cropW/H describe a frame the crop really ran
    bool     ready = false;          // ...and the offset vectors' texture existed on it
    // The price window's counters (flushWindow prints and clears them).
    float    peakPx = 0.0f;          // the largest slide applied this window
    float    peakDeg = 0.0f;
    double   sumDeg = 0.0;
    uint32_t frames = 0;             // frames the lead was computed on
    uint32_t held = 0;               // ...of those, frames the frame's edge held it short
    uint32_t moved = 0;              // ...and frames whose vectors were offset
};
FoveaLeadState g_foveaLead[2];
// Forgotten whenever the crop is not running: a re-engagement starts from
// the unshifted base with no delta, because a base kept across a gap would
// slide the rectangle against a frame NVIDIA's history never saw. The
// window's counters survive -- they belong to the price report, not to one
// engagement.
void foveaLeadForget(int eye) {
    if (eye < 0 || eye > 1) return;
    FoveaLeadState& st = g_foveaLead[eye];
    st.px[0] = st.px[1] = 0.0f;
    st.base[0] = st.base[1] = 0;
    st.cropW = st.cropH = 0;
    st.valid = false;
    st.ready = false;
}

// Sorts v[0..n) in place and reads off one percentile by linear
// interpolation between the two bracketing order statistics -- the usual
// definition, and the one tools\edvr_log.py's own reader (if it ever
// checks these figures) would reproduce with numpy.
double windowPercentile(double* v, int n, double frac) {
    if (n <= 0) return 0.0;
    std::sort(v, v + n);
    const double pos = frac * static_cast<double>(n - 1);
    int lo = static_cast<int>(pos);
    if (lo < 0) lo = 0;
    if (lo > n - 1) lo = n - 1;
    const int hi = (lo + 1 < n) ? lo + 1 : lo;
    const double t = pos - static_cast<double>(lo);
    return v[lo] * (1.0 - t) + v[hi] * t;
}

// engine is only meaningful for FullFrame (Fovea is NVIDIA-only, design
// doc 3.1/3.2's "the fovea keys are NVIDIA-only", so it is always Nvidia
// there; None is always Own). Today's two names are unchanged; AMD's run
// prints fsr3VersionLabel() ("fsr", or "fsr 3.1.2" once the port is linked)
// in place of "full-frame ngx", which is NVIDIA's brand name, not AMD's.
// lean is only meaningful for the own path: which of its two shaders ran.
const char* treatmentName(Treatment t, edvr::TemporalEngine engine, bool lean) {
    switch (t) {
        case Treatment::Fovea:     return "foveated ngx";
        case Treatment::FullFrame: return engine == edvr::TemporalEngine::Amd
                                       ? edvr::fsr3VersionLabel()
                                       : "full-frame ngx";
        default:                   return lean ? "own history (no ngx, lean)"
                                               : "own history (no ngx, instrumented)";
    }
}

// Closes the current window (if any samples reached it) and writes the one
// required log line: treatment, output size, pair count and reason, then
// every region's stereo-sum median/p95, then "other" -- total less the sum
// of the seven regions, taken per pair before the percentile (a median is
// not linear, so subtracting already-reduced medians would not be the same
// number). Also latches the F8 line's live figures from this window.
void flushWindow(const char* reason) {
    if (!g_windowKeyValid || g_windowCount <= 0) { g_windowKeyValid = false; g_windowCount = 0; return; }
    const int n = g_windowCount;
    double scratch[kWindowPairs];
    double regionMed[kRegionCount] = {}, regionP95[kRegionCount] = {};
    for (int ri = 0; ri < kRegionCount; ++ri) {
        for (int k = 0; k < n; ++k) scratch[k] = g_windowRegion[ri][k];
        regionP95[ri] = windowPercentile(scratch, n, 0.95);
        for (int k = 0; k < n; ++k) scratch[k] = g_windowRegion[ri][k];
        regionMed[ri] = windowPercentile(scratch, n, 0.5);
    }
    double otherScratch[kWindowPairs];
    for (int k = 0; k < n; ++k) {
        double sum = 0.0;
        for (int ri = 0; ri < kRegionCount; ++ri) sum += g_windowRegion[ri][k];
        otherScratch[k] = g_windowTotal[k] - sum;
    }
    for (int k = 0; k < n; ++k) scratch[k] = otherScratch[k];
    const double otherP95 = windowPercentile(scratch, n, 0.95);
    for (int k = 0; k < n; ++k) scratch[k] = otherScratch[k];
    const double otherMed = windowPercentile(scratch, n, 0.5);

    double partMed[kPrepParts] = {}, partP95[kPrepParts] = {};
    for (int pi = 0; pi < kPrepParts; ++pi) {
        for (int k = 0; k < n; ++k) scratch[k] = g_windowPart[pi][k];
        partP95[pi] = windowPercentile(scratch, n, 0.95);
        for (int k = 0; k < n; ++k) scratch[k] = g_windowPart[pi][k];
        partMed[pi] = windowPercentile(scratch, n, 0.5);
    }

    // The eye pass's capture work for engine motion (clear, snapshots,
    // refreshes), GPU-timed there and taken here, printed beside prep's
    // parts: it runs BEFORE prep, in the game's own eye pass, so neither prep
    // nor "other" contains it (the performance review, attribution gap 1).
    EngineVelocityCaptureGpu capture{};
    const bool captured = engineVelocityTakeCaptureGpu(&capture);
    char line[1600];
    int len = snprintf(line, sizeof(line),
        "temporal aa price: %s, %ux%u, %s shader, %d stereo pairs (%s), ms per pair "
        "median/p95:",
        treatmentName(g_windowKey.treatment, g_windowKey.engine, g_windowKey.lean),
        g_windowKey.outW,
        g_windowKey.outH, g_windowKey.instrumented ? "diagnostic" : "lean", n, reason);
    for (int ri = 0; ri < kRegionCount && len > 0 && len < static_cast<int>(sizeof(line)); ++ri) {
        len += snprintf(line + len, sizeof(line) - len, " %s %.2f/%.2f",
                        kRegionNames[ri], regionMed[ri], regionP95[ri]);
        // Prep's parts inside its own figure (PrepPart): on the paths that
        // time them; zeros where a part did not run (the own path).
        if (ri == static_cast<int>(Region::Prep) && len > 0 && len < static_cast<int>(sizeof(line))) {
            len += snprintf(line + len, sizeof(line) - len, " (%s %.2f/%.2f %s %.2f/%.2f",
                            kPrepPartNames[0], partMed[0], partP95[0], kPrepPartNames[1], partMed[1], partP95[1]);
            if (captured && len > 0 && len < static_cast<int>(sizeof(line)))
                len += snprintf(line + len, sizeof(line) - len,
                                "; eye-pass capture per event, before prep: clear %.3f/%.3f x%u, snapshots "
                                "%.3f/%.3f x%u, refreshes %.3f/%.3f x%u, %llu untimed, %llu invalid",
                                capture.medianMs[0], capture.p95Ms[0], capture.events[0], capture.medianMs[1],
                                capture.p95Ms[1], capture.events[1], capture.medianMs[2], capture.p95Ms[2],
                                capture.events[2], static_cast<unsigned long long>(capture.untimed),
                                static_cast<unsigned long long>(capture.invalid));
            if (len > 0 && len < static_cast<int>(sizeof(line))) len += snprintf(line + len, sizeof(line) - len, ")");
        }
    }
    if (len > 0 && len < static_cast<int>(sizeof(line))) {
        len += snprintf(line + len, sizeof(line) - len, " other %.2f/%.2f", otherMed, otherP95);
    }
    // F5: the pairs and calls dropped before pricing, per window, so a low
    // pair count against the session's runtime is explained on the same
    // line rather than needing a second instrument to notice.
    if (len > 0 && len < static_cast<int>(sizeof(line))) {
        snprintf(line + len, sizeof(line) - len,
                 " dropped %u unmeasured pairs, %u lone eyes, %u no-slot frames, "
                 "%u region leases",
                 g_droppedUnmeasured, g_droppedLone, g_droppedNoSlot, g_regionBeginFailed);
    }
    Log::get().note("%s", line);

    // The head lead's own line, once per window per eye while the key is on
    // AND the lead ran at least once in the window (st.frames). A still head
    // with the crop engaged prints zeros -- which is the evidence that the
    // lead was live and measured nothing -- while a window the crop never
    // ran in prints nothing at all, the price line above having already said
    // so by its treatment. The counters reset either way, so a window's
    // figures are never another window's.
    if (g_foveaLeadFrames > 0.0f) {
        for (int eyeIdx = 0; eyeIdx < 2; ++eyeIdx) {
            FoveaLeadState& st = g_foveaLead[eyeIdx];
            if (st.frames > 0) {
                const double mean = st.sumDeg / static_cast<double>(st.frames);
                Log::get().note(
                    "temporal aa fovea lead: eye %d peak %.1f deg (%d px) this window, mean %.2f "
                    "deg, held at the frame's edge on %u frames, motion vectors offset on %u frames",
                    eyeIdx, static_cast<double>(st.peakDeg),
                    static_cast<int>(st.peakPx + 0.5f), mean, st.held, st.moved);
            }
            st.peakPx = st.peakDeg = 0.0f;
            st.sumDeg = 0.0;
            st.frames = st.held = st.moved = 0;
        }
    }

    for (int ri = 0; ri < kRegionCount; ++ri) g_lastWindowMedian[ri] = regionMed[ri];
    g_lastWindowOtherMedian = otherMed;
    g_lastWindowPairs = static_cast<uint32_t>(n);
    g_lastWindowValid = true;
    g_lastWindowDropped = g_droppedUnmeasured + g_droppedLone + g_droppedNoSlot + g_regionBeginFailed;
    // The one-shot log note above (g_regionBeginFailedNoted) stays latched
    // for the session; only the per-window counters it and the others feed
    // reset here.
    g_droppedUnmeasured = 0;
    g_droppedLone = 0;
    g_droppedNoSlot = 0;
    g_regionBeginFailed = 0;

    g_windowCount = 0;
    g_windowKeyValid = false;
}

// Folds one resolved stereo pair (both eyes' totals and region prices,
// already summed) into the current window, opening or closing a window as
// the key requires.
void accumulateWindow(const WindowKey& key, double totalSum, const double regionSum[kRegionCount],
                      const double partSum[kPrepParts]) {
    if (!g_windowKeyValid || key != g_windowKey) {
        if (g_windowKeyValid) flushWindow("window closed");
        g_windowKey = key;
        g_windowKeyValid = true;
        g_windowCount = 0;
    }
    if (g_windowCount < kWindowPairs) {
        g_windowTotal[g_windowCount] = totalSum;
        for (int ri = 0; ri < kRegionCount; ++ri) g_windowRegion[ri][g_windowCount] = regionSum[ri];
        for (int pi = 0; pi < kPrepParts; ++pi) g_windowPart[pi][g_windowCount] = partSum[pi];
        ++g_windowCount;
    }
    if (g_windowCount >= kWindowPairs) flushWindow("600 pairs");
}

// Buffers one eye's priced call until its stereo twin (the other eye, same
// frame) is priced too, then hands the summed pair to accumulateWindow. A
// small ring: normally at most the two eyes of the current and previous
// frame are ever pending at once. An entry whose twin never arrives (a
// dropped eye, or this build ever running one-eyed) is evicted oldest-
// first, dropped and counted (droppedLone) rather than held forever or
// priced alone as if it were a true stereo pair.
struct PendingPair {
    bool      used = false;
    uint32_t  frame = 0;
    uint32_t  seq = 0;
    bool      have[2] = {false, false};
    bool      valid[2] = {false, false};
    double    totalMs[2] = {0.0, 0.0};
    double    regionMs[2][kRegionCount] = {};
    double    partMs[2][kPrepParts] = {};
    WindowKey key[2];
};
constexpr int kPendingCap = 8;
PendingPair g_pending[kPendingCap];
uint32_t    g_pendingSeq = 0;

// Complete AND fully-measured pairs are folded into the window; anything
// else is dropped and counted rather than priced as an approximation --
// an incomplete pair (only one eye ever arrived) counts as a lone eye,
// and a complete pair with an unmeasured eye counts as unmeasured.
void finalizePending(PendingPair& p) {
    if (p.have[0] && p.have[1]) {
        if (p.valid[0] && p.valid[1]) {
            double totalSum = p.totalMs[0] + p.totalMs[1];
            double regionSum[kRegionCount] = {};
            for (int ri = 0; ri < kRegionCount; ++ri) regionSum[ri] = p.regionMs[0][ri] + p.regionMs[1][ri];
            double partSum[kPrepParts] = {};
            for (int pi = 0; pi < kPrepParts; ++pi) partSum[pi] = p.partMs[0][pi] + p.partMs[1][pi];
            accumulateWindow(p.key[0], totalSum, regionSum, partSum);
        } else {
            ++g_droppedUnmeasured;
        }
    } else if (p.have[0] || p.have[1]) {
        ++g_droppedLone;
    }
    p = PendingPair{};
}

void priceSlot(const Slot& q) {
    if (q.eye != 0 && q.eye != 1) return;   // an eye index this report cannot place
    const WindowKey key{q.treatment, q.engine, q.outW, q.outH, q.fmt, q.configGen, q.lean, q.instrumented};
    int idx = -1;
    for (int i = 0; i < kPendingCap; ++i) {
        if (g_pending[i].used && g_pending[i].frame == q.frame) { idx = i; break; }
    }
    if (idx < 0) {
        int chosen = 0;
        bool haveFree = false;
        for (int i = 0; i < kPendingCap; ++i) {
            if (!g_pending[i].used) { chosen = i; haveFree = true; break; }
            if (!haveFree && g_pending[i].seq < g_pending[chosen].seq) chosen = i;
        }
        if (!haveFree && g_pending[chosen].used) finalizePending(g_pending[chosen]);
        g_pending[chosen] = PendingPair{};
        g_pending[chosen].used = true;
        g_pending[chosen].frame = q.frame;
        g_pending[chosen].seq = ++g_pendingSeq;
        idx = chosen;
    }
    PendingPair& p = g_pending[idx];
    p.have[q.eye] = true;
    p.valid[q.eye] = q.totalValid;
    p.totalMs[q.eye] = q.totalMs;
    for (int ri = 0; ri < kRegionCount; ++ri) p.regionMs[q.eye][ri] = q.regionMs[ri];
    for (int pi = 0; pi < kPrepParts; ++pi) p.partMs[q.eye][pi] = q.partMs[pi];
    p.key[q.eye] = key;
    if (p.have[0] && p.have[1]) finalizePending(p);
}

uint32_t g_timeCount = 0;
double   g_timeSum = 0.0;
double   g_timeMax = 0.0;
uint64_t g_pixelsSeen = 0;
uint64_t g_rejected = 0;
uint64_t g_clipped = 0;
// The registration instrument's sums: per candidate, and the selected
// delta's clip share by head speed (still, slow, fast).
// Since the last registration line, so every candidate is judged over
// the SAME frames: the depth flight of 2026-09-03 compared a session's
// worth of the rotation-only delta against fifteen seconds of the depth
// candidates and could not tell them apart.
uint64_t g_candPix[4] = {};
uint64_t g_candRej[4] = {};
uint64_t g_candClip[4] = {};
uint64_t g_candSize[4] = {};    // the clips' sizes, 1/255ths of luma, summed
uint64_t g_bucketPix[3] = {};
uint64_t g_bucketClip[3] = {};
uint64_t g_bucketSize[3] = {};
uint32_t g_bucketFrames[3] = {};
uint32_t g_intervalFrames = 0;
uint64_t g_intervalPix = 0;         // pixels this interval, both paths
uint64_t g_worldPix = 0;            // ...of which the world path took
uint64_t g_brightPix = 0;           // bright pixels (luma over 0.6)
uint64_t g_brightNoDepthPix = 0;    // ...of which had no depth
double   g_camHeadDiffSum = 0.0;    // degrees: the camera's delta against the head's
double   g_camMoveSum = 0.0;        // metres: the camera's displacement a frame
uint32_t g_camFrames = 0;
uint32_t g_camDropRot = 0;          // frames whose camera delta was another camera's
uint32_t g_camDropParked = 0;       // ...a parked camera's: from rows a drop left, not turned at all
uint32_t g_camParkedStayMax = 0;    // ...the longest run of such frames, in frames
uint32_t g_camDropMove = 0;         // frames whose camera translation was a jump
// The world path's scene floor (kTemporalSceneDrawFloor; the split's gate
// says why): eye-frames it alone stood the path down, and the last count
// it refused.
uint32_t g_worldFloorRefused = 0;
uint32_t g_worldFloorLast = 0;
// The registration probes and the per-class clip shares, per interval
// (the shader says what they are).
int64_t     g_probeWorldDx = 0, g_probeWorldDy = 0;
uint64_t    g_probeWorldN = 0;
int64_t     g_probeShipDx = 0, g_probeShipDy = 0;
uint64_t    g_probeShipN = 0;
uint64_t    g_classWorldPix = 0, g_classWorldClip = 0;
uint64_t    g_classShipPix = 0, g_classShipClip = 0;
int64_t     g_probeSkyDx = 0, g_probeSkyDy = 0;
uint64_t    g_probeSkyN = 0;
int64_t     g_probeDot[3] = {};    // sum resid.mv * 100: sky, world with a depth, ship
uint64_t    g_probeMm[3] = {};     // sum mv.mv * 100, the same classes
// The rows' delta against the head's, per frame the world path had both:
// the residual rotation's size by head-speed bucket, its regression on
// the head's turn (a scale k: the rows turned (1 + k) times the head),
// per axis, and on the turn's change (a lead in frames).
double      g_rhN[3] = {}, g_rhSum[3] = {};
double      g_rhDot = 0.0, g_rhMm = 0.0;
double      g_rhDotAx[3] = {}, g_rhMmAx[3] = {};
double      g_rhDotLag = 0.0, g_rhMmLag = 0.0;
float       g_omegaPrev[3] = {};
bool        g_omegaPrevValid = false;
// The chooser's ambiguity: frames on which a second continuous reading
// differed from the chosen one, and how far apart they sat.
uint32_t    g_chooseMulti = 0;
// The rows' translation against the head's, per axis, on frames the ship
// stood still (under 2 cm in the rows): the sign says whether the frame
// change is the z flip (+1, +1, +1 after it) or a half turn (-1, -1, -1).
double      g_tvDot[3] = {}, g_tvMm[3] = {};
uint32_t    g_tvFrames = 0;
// The previous frame's rows written again this frame (the game's own
// last-view block): skipped by the chooser, counted here.
uint32_t    g_twinFrames = 0;
double      g_chooseSpreadSum = 0.0, g_chooseSpreadMax = 0.0;

// A rotation as a small vector (degrees about x, y, z): the skew part,
// scaled from sin to the angle. Exact enough under a few degrees.
void temporalSmallRotVecDeg(const float R[9], float v[3]) {
    const float deg = temporalRotationAngleDeg(R);
    const float rad = deg * 3.14159265f / 180.0f;
    const float s = sinf(rad);
    const float scale = (s > 1e-6f ? rad / s : 1.0f) * 0.5f * (180.0f / 3.14159265f);
    v[0] = (R[7] - R[5]) * scale;
    v[1] = (R[2] - R[6]) * scale;
    v[2] = (R[3] - R[1]) * scale;
}
constexpr float kStillDeg = 0.03f;   // under 2 deg/s at 72 Hz: tracking noise
constexpr float kSlowDeg = 0.30f;    // under 22 deg/s: a glance
bool     g_priceLogged = false;
uint32_t g_lastW = 0, g_lastH = 0;
uint64_t g_moverPix = 0;   // pixels the mover mask set this interval (Stats[28]; tier 1)
// NVIDIA's history resets this interval (the run of 18:43, 2026-09-09: the
// station "flickering into sharpness" -- a raw frame every reset, the
// blur back as the history rebuilds on the pass's vectors), and how many
// the openvr half asked for (a withheld frame, or a pose without a delta).
uint64_t g_dlResets = 0, g_dlResetsAsked = 0;
// ...the openvr half's reasons, bits 2-5 of the flags (temporal_aa.cpp): a
// hold or a healed frame, a withheld jump the camera came back from, one
// left unjudged, a pose without a delta.
uint64_t g_dlResetsHeld = 0, g_dlResetsReturned = 0, g_dlResetsUnjudged = 0, g_dlResetsNoDelta = 0;

void maybeLogPrice() {
    if (g_priceLogged || g_timeCount < 120) return;
    g_priceLogged = true;
    if (g_pixelsSeen == 0) {
        Log::get().note(
            "temporal aa: measured %.2f ms per eye on average (max %.2f) at "
            "the temporal work's GPU bracket. History rejection and clipping "
            "were not counted (the lean own shader, or NVIDIA's history; "
            "advanced.temporal_aa_diagnostics = 1 counts them).",
            g_timeSum / static_cast<double>(g_timeCount), g_timeMax);
        return;
    }
    Log::get().note(
        "temporal aa: measured %.2f ms per eye on average (max %.2f) at "
        "the temporal work's GPU bracket. Diagnostic rejection %.1f%%, "
        "clipping %.1f%%; these counters describe the native resolve only "
        "and cannot validate DLSS history or motion.",
        g_timeSum / static_cast<double>(g_timeCount), g_timeMax,
        100.0 * static_cast<double>(g_rejected) / static_cast<double>(g_pixelsSeen),
        100.0 * static_cast<double>(g_clipped) / static_cast<double>(g_pixelsSeen));
}

void pollSlots(ID3D11DeviceContext* ctx) {
    if (!ctx || !gpuTimingOwns(ctx)) return;
    for (Slot& q : g_slots) {
        if (!q.inUse) continue;
        if (!q.timeDone) {
            if (!q.timing) q.timeDone = true;
            else { double ms=0.0; const auto status=q.timer.poll(ctx,ms);
                if(status==GpuTimerPoll::Ready){q.timeDone=true;++g_timeCount;g_timeSum+=ms;if(ms>g_timeMax)g_timeMax=ms;q.totalMs=ms;q.totalValid=true;}
                else if(status==GpuTimerPoll::Invalid) q.timeDone=true;
            }
        }
        if (!q.statsDone) {
            D3D11_MAPPED_SUBRESOURCE m{};
            const HRESULT hr = ctx->Map(q.staging, 0, D3D11_MAP_READ,
                                        D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (SUCCEEDED(hr) && m.pData) {
                const uint32_t* v = static_cast<const uint32_t*>(m.pData);
                if (q.engineStats) engineVelocityNotePixels(v[50], v[51], v[52], v[53], v[54]);
                g_rejected += v[0];
                g_clipped += v[1];
                g_pixelsSeen += q.pixels;
                g_intervalPix += q.pixels;
                g_worldPix += v[15];
                g_brightPix += v[16];
                g_brightNoDepthPix += v[17];
                g_moverPix += v[28];
                g_probeWorldDx += static_cast<int32_t>(v[18]);
                g_probeWorldDy += static_cast<int32_t>(v[19]);
                g_probeWorldN += v[20];
                g_probeShipDx += static_cast<int32_t>(v[21]);
                g_probeShipDy += static_cast<int32_t>(v[22]);
                g_probeShipN += v[23];
                g_classWorldPix += v[24];
                g_classWorldClip += v[25];
                g_classShipPix += v[26];
                g_classShipClip += v[27];
                g_probeSkyDx += static_cast<int32_t>(v[30]);
                g_probeSkyDy += static_cast<int32_t>(v[31]);
                g_probeSkyN += v[32];
                for (int c = 0; c < 3; ++c) {
                    g_probeDot[c] += static_cast<int32_t>(v[33 + c * 2]);
                    g_probeMm[c] += v[34 + c * 2];
                }
                ++g_intervalFrames;
                if (q.hadHistory) {
                    for (int c = 0; c < 4; ++c) {
                        if (!q.candPixels[c]) continue;
                        g_candPix[c] += q.candPixels[c];
                        g_candRej[c] += v[3 + c * 3];
                        g_candClip[c] += v[4 + c * 3];
                        g_candSize[c] += v[5 + c * 3];
                    }
                    const int b = q.headDeg < kStillDeg ? 0 : (q.headDeg < kSlowDeg ? 1 : 2);
                    g_bucketPix[b] += q.pixels;
                    g_bucketClip[b] += v[1];
                    g_bucketSize[b] += v[2];
                    ++g_bucketFrames[b];
                }
                ctx->Unmap(q.staging, 0);
                q.statsDone = true;
            } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
                q.statsDone = true;   // an unreadable sample; drop it
            }
        }
        // Stage 0 price report: each region polled the same way as the
        // total timer above -- a region never begun this call (regionEnded
        // false) or begun but refused a lease (regionEnded true,
        // regionTiming false) is done immediately at 0 ms, same as the
        // deliverable's "absent"/"dropped" contribute 0.
        bool regionsDone = true;
        for (int ri = 0; ri < kRegionCount; ++ri) {
            if (q.regionDone[ri]) continue;
            if (!q.regionEnded[ri] || !q.regionTiming[ri]) {
                q.regionDone[ri] = true;
                continue;
            }
            double rms = 0.0;
            const auto rstatus = q.regionTimer[ri].poll(ctx, rms);
            if (rstatus == GpuTimerPoll::Ready) { q.regionMs[ri] = rms; q.regionDone[ri] = true; }
            else if (rstatus == GpuTimerPoll::Invalid) q.regionDone[ri] = true;
            if (!q.regionDone[ri]) regionsDone = false;
        }
        // Prep's parts, the same way.
        for (int pi = 0; pi < kPrepParts; ++pi) {
            if (q.partDone[pi]) continue;
            if (!q.partEnded[pi] || !q.partTiming[pi]) {
                q.partDone[pi] = true;
                continue;
            }
            double pms = 0.0;
            const auto pstatus = q.partTimer[pi].poll(ctx, pms);
            if (pstatus == GpuTimerPoll::Ready) { q.partMs[pi] = pms; q.partDone[pi] = true; }
            else if (pstatus == GpuTimerPoll::Invalid) q.partDone[pi] = true;
            if (!q.partDone[pi]) regionsDone = false;
        }
        q.regionsDone = regionsDone;
        if (q.timeDone && q.regionsDone && !q.priced) {
            q.priced = true;
            priceSlot(q);
        }
        if (q.timeDone && q.statsDone && q.regionsDone) q.inUse = false;
    }
    maybeLogPrice();
}

int acquireSlot(ID3D11Device* dev) {
    for (int i = 0; i < kSlots; ++i) {
        Slot& q = g_slots[i];
        if (q.inUse) continue;
        if (!q.staging) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kStatCount * 4;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            const bool made = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &q.staging));
            if (!made) {
                releaseSlot(q);
                continue;
            }
        }
        return i;
    }
    return -1;
}

FaultBudget g_budget("temporalPass", 8);

// Every cached shader, query and texture below belongs to one device.
// Retain its identity so even an address reused after Release cannot pass.
ID3D11Device*              g_passDevice = nullptr;
bool                      g_otherDeviceNoted = false;
bool acceptPassDevice(ID3D11Device* dev) {
    if (!dev || deviceHookRecoveryDisabled()) return false;
    if (!g_passDevice) {
        dev->AddRef();
        g_passDevice = dev;
    }
    if (dev == g_passDevice) return true;
    if (!g_otherDeviceNoted) {
        g_otherDeviceNoted = true;
        Log::get().note("temporal aa: refusing device %p; cached GPU resources belong to %p. "
                        "No cross-device commands were issued. Please report this log.",
                        (void*)dev, (void*)g_passDevice);
    }
    return false;
}

ID3D11ComputeShader*       g_cs = nullptr;
bool                       g_csTried = false;
ID3D11ComputeShader*       g_csFast = nullptr;
bool                       g_csFastTried = false;
bool                       g_leanFailNoted = false;   // the lean own shader could not be created, once
bool                       g_leanNoted = false;       // the own path first ran the lean shader, once
ID3D11ComputeShader*       g_csMv = nullptr;     // the motion-vector entry, for DLAA
bool                       g_csMvTried = false;
ID3D11ComputeShader*       g_csMvFast = nullptr;
bool                       g_csMvFastTried = false;
ID3D11ComputeShader*       g_csMvTrace = nullptr;
bool                       g_csMvTraceTried = false;
bool                       g_diagnostics = false;

ID3D11ComputeShader* motionShader(ID3D11DeviceContext* ctx, bool diagnostics) {
    auto*& shader = diagnostics ? g_csMv : g_csMvFast;
    auto& tried = diagnostics ? g_csMvTried : g_csMvFastTried;
    if (!shader && !tried) {
        tried = true;
        shader = shaderSwapCreateCs(ctx,
            diagnostics ? kTemporalMvBytecode : kTemporalMvFastBytecode,
            diagnostics ? sizeof(kTemporalMvBytecode) : sizeof(kTemporalMvFastBytecode),
            diagnostics ? "temporal_mv_cs" : "temporal_mv_fast_cs", "temporal aa");
    }
    return shader;
}
// Mirrors motionShader for the pass's own resolve: the instrumented shader
// (the registration instrument, its probes and its counters) when
// diagnostics is asked for, otherwise the lean variant with that work
// compiled out. g_cs is also created eagerly elsewhere (it doubles as the
// availability check for the whole native path); this lazy create only
// matters the first time the lean variant is wanted.
ID3D11ComputeShader* ownShader(ID3D11DeviceContext* ctx, bool diagnostics) {
    auto*& shader = diagnostics ? g_cs : g_csFast;
    auto& tried = diagnostics ? g_csTried : g_csFastTried;
    if (!shader && !tried) {
        tried = true;
        shader = shaderSwapCreateCs(ctx,
            diagnostics ? kTemporalAaBytecode : kTemporalAaFastBytecode,
            diagnostics ? sizeof(kTemporalAaBytecode) : sizeof(kTemporalAaFastBytecode),
            diagnostics ? "temporal_aa_cs" : "temporal_aa_fast_cs", "temporal aa");
    }
    return shader;
}
ID3D11ComputeShader*       g_csFovea = nullptr;  // the fovea composite (feature 6)
ID3D11ComputeShader*       g_csUiResolve = nullptr;
bool                      g_csUiResolveTried = false, g_uiResolveNoted = false;
ID3D11Buffer*              g_uiResolveTolCb = nullptr;   // UI resolve b1: {tolerance/255, corona hold, 0, 0}
static bool                g_coronaHoldNoted = false;    // corona-smear hold: said once, only when the hold written is > 0
bool                       g_csFoveaTried = false;
ID3D11Buffer*              g_foveaCb = nullptr;   // its crop and edge band
bool                       g_foveaNoted = false;
bool                       g_foveaEdgesNoted[2] = {false, false};   // edges mode's own line, per eye
bool                       g_foveaSizeNoted[2] = {false, false};    // the crop's size stood it down, per eye
bool                       g_foveaFailNoted = false;
// Latched when the fovea's NGX create or eval fails, so it is not retried
// every frame (a create costs tens to hundreds of ms -- a stutter storm).
// Cleared on a config reload and on a frame-size change, so a fixed cause
// (a bad size, a transient) gets another chance without a relaunch. The
// sizes it failed at re-arm it when either changes (the review of
// 2026-09-05, F4: one eye's refusal must not strand both for the session
// across an HMD Quality change).
bool                       g_foveaFailed = false;
uint32_t                   g_foveaFailW = 0, g_foveaFailFoW = 0;
uint32_t                   g_foveaTreats = 0;
ID3D11ComputeShader*       g_csDown = nullptr;    // the steady periphery's reduction (feature 6)
bool                       g_csDownTried = false;
ID3D11Buffer*              g_downCb = nullptr;    // its sizes
bool                       g_periphWholeNoted = false;   // "the periphery would be the whole frame", once
bool                       g_dlaaNoted = false;
bool                       g_dlssNoted = false;
// The refusal line, ONE PER ENGINE. It was a single session-lifetime flag,
// and the hint NVIDIA's refusal now carries ("Set temporal_aa = fsr ...")
// walks the commander straight into the hole that made: NGX refuses and says
// so, he edits the live ini to fsr, AMD refuses too -- and the flag was
// already spent, so the log said NOTHING at all and the flight was flown
// with no reason string (the review of 2026-09-16, F1). Both are cleared in
// temporalPassConfigure when the engine changes, so every engine that
// refuses puts its reason in the log at least once per session.
bool                       g_dlaaFailNoted = false;   // NVIDIA's (dlaa, dlss)
bool                       g_fsrFailNoted = false;    // AMD's (fsr)
bool                       g_trainedNoted = false;   // the first trained frame's line
// "the fovea keys are NVIDIA-only; fsr runs the full frame", once: AMD's
// engine never runs the fovea/periphery crop (design doc 3.1), so this
// notes the one case where foveaConfigured() (a width or edges) asked for
// it anyway.
bool                       g_fsrFoveaNoted = false;
// The AMD engaged/upscale-engaged lines, separate from g_dlaaNoted/
// g_dlssNoted: those are session-lifetime "once" flags, and a live switch
// from dlss to fsr (or back) must still print its own engine's line rather
// than finding the other engine's flag already set.
bool                       g_fsrNoted = false;
bool                       g_fsrUpscaleNoted = false;
uint32_t                   g_dlaaTreats = 0;
ID3D11Buffer*              g_cb = nullptr;
ID3D11SamplerState*        g_samp = nullptr;
ID3D11Buffer*              g_stats = nullptr;
ID3D11UnorderedAccessView* g_statsUav = nullptr;

bool     g_failNoted = false;
bool     g_kindNoted = false;
bool     g_regionNoted = false;
bool     g_fmtUnknownNoted = false;
bool     g_fmtChecked[kFormatCount] = {};
bool     g_fmtSupported[kFormatCount] = {};
bool     g_fmtUnsupportedNoted[kFormatCount] = {};
bool     g_histChecked = false;
DXGI_FORMAT g_histFmt = DXGI_FORMAT_UNKNOWN;
bool     g_firstNoted = false;
uint32_t g_treats = 0;

// The scanner's interface on the head's path (docs/fss-scanner.md,
// 2026-09-16). The FSS composites its own interface -- the bottom bar, the
// spectral text, the signal markers -- at a scene depth past the world/ship
// split, and the split's rule for an interface stroke (keep the camera's
// path at the pixel's depth) assumes the camera is the head, which in the
// scanner it is not: the scanner's camera pans while the panel stays put
// in front of the seat, so NVIDIA was told the panel's static text moved
// by the pan (eye dump 170752: 5 px that frame, 47 two frames on, the head
// still) and doubled it. While the scanner's screen is up the shader gives
// interface-marked pixels the head's delta at whatever depth they read
// (probe.w bit 128). "Up" is the chrome tracker's word (vscreen.cpp,
// beginPanelOverride): it bumps the shared stamp once per frame it sees
// the scanner's screen composited, before the eye is submitted, so a
// stamp that moved since the last treated frame is this frame's answer.
LONG     g_fssChromeStampSeen = 0;
uint32_t g_fssChromeStampFrame = 0;
bool     g_fssChromeStampKnown = false;
bool     g_fssInterfaceNoted = false;
// (fssInterfaceLive, the per-frame question, sits below g_rowsFrame.)

// The configure and warm state.
bool     g_filterCurrent = true;   // advanced.temporal_aa_current = filtered | raw
float    g_historyC = 0.5f;        // advanced.temporal_aa_history_sharp: the cubic's C
float    g_shipMetres = kTemporalShipMetres;    // advanced.temporal_aa_ship_metres: the world/ship split (0 off)
int      g_debugMode = 0;          // advanced.temporal_aa_debug: 0 off, 1 motion, 2 error, 3 depth
// The NGX warm-up (advanced.temporal_aa_warm): NVIDIA initialised and both
// eyes' full-frame features made on a loading-screen frame boundary, once
// openvr_api.dll has published the per-eye render size, instead of inside
// the first submitted frame -- which paid ~0.8 s for it on both installs
// (2026-09-15, 'native timing CPU: seq 3 ... temporal 853.701'). A
// once-per-session state machine: Pending polls the gates every tick; Off,
// Done, Failed and Late are terminal (a config reload never re-arms it).
// g_warmWhy is the last gate's reason, printed by the first treat when the
// warm-up never got there (warmNoteFirstTreat), so the log tells dead code
// from a gate that never opened from a switch that was off.
enum class WarmState { Pending, Off, Done, Failed, Late };
WarmState   g_warmState = WarmState::Pending;
bool        g_trainedWanted = false;   // fix.temporal_aa = dlaa | dlss | fsr (an external engine)
// Which external engine g_trainedWanted means, set alongside it
// (temporalPassConfigure): Own when g_trainedWanted is false. warmTrainedOnce
// reads this to choose dlaaWarm or fsr3Warm and to word its log correctly.
edvr::TemporalEngine g_temporalEngine = edvr::TemporalEngine::Own;
// The engine the TREAT last ran under, as opposed to the one configure last
// read. Set on the render thread, inside the treat, and compared there so the
// pass notices a live switch at the first frame that carries the new engine's
// flag -- which is the only place it is safe to free the engine being left
// (the review of 2026-09-16, F7: fsr3ReleaseFeatures had no caller, so a
// dlss -> fsr A/B kept both footprints allocated, on a headset whose per-eye
// output is 3422x3394). temporalPassConfigure runs on whatever thread reloads
// the config, so it must NOT do this.
edvr::TemporalEngine g_engineRan = edvr::TemporalEngine::Own;
bool        g_warmOn = true;           // advanced.temporal_aa_warm
bool        g_warmOffNoted = false;
const char* g_warmWhy = "no frame boundary reached the warm-up: the tick never ran";
char        g_warmWhyBuf[160] = {};

// The treat side's line, once, when the first trained treat arrives and the
// warm-up had not run (Pending: a gate never opened, or dead code) or was
// switched off. Done and Failed print nothing here; their own lines did.
void warmNoteFirstTreat() {
    if (g_warmState != WarmState::Pending && g_warmState != WarmState::Off) return;
    g_warmState = WarmState::Late;
    Log::get().note(
        "temporal aa: NVIDIA was not warmed before the first submitted frame (%s); it is "
        "initialised now, inside this frame, as before.",
        g_warmWhy);
}
float    g_lastNear = 0.0f;        // the planes the last treat decoded with (temporalPassPlanes)
float    g_lastFar = 0.0f;
float    g_menuMetres = 0.0f;      // advanced.temporal_aa_menu_metres: a depth for depthless pixels in a menu-like scene
// Tier 1 of docs/per-object-motion.md, the mover mask (2026-09-08): off by
// default until it has flown. The tolerance is a fraction of depth, the
// strength how much history a masked pixel loses.
bool     g_moversOn = false;       // experimental.temporal_aa_movers
float    g_moversTol = 0.03f;      // advanced.temporal_aa_movers_tolerance, percent in the ini
float    g_moversStrength = 1.0f;  // advanced.temporal_aa_movers_strength
bool     g_moversNoted = false;    // the engage line, once
// This frame's rows are the view's own (its delta not carried), and the
// frame of the last floating-origin jump (a camera move over 50 m in a
// frame): the eye run's motion trace and the submission history carry both.
bool     g_rowsDeltaOwn = false;
uint32_t g_originJumpFrame = ~0u;

// hotkey.dump_eyes, and the settings menu's "Dump both eyes as seen": the
// treated eye as the compositor receives it -- after DLSS, the fovea
// composite, everything -- to edvr_logs\eyes\eye_HHMMSS_L.bmp and _R.bmp,
// 24-bit, so what the player saw through the lens can be read off the desk
// instead of photographed through it (asked for on 2026-09-09, with a debug
// view up). One staging copy and a map that waits for the GPU: a hitch,
// once per press. Float formats are taken as linear and encoded sRGB for
// the file; the 8- and 10-bit ones are written as they are.
bool     g_eyeDumpArmed[2] = {false, false};
bool     g_eyeDumpDirMade = false;
// THE EYE RUN (2026-09-09, the thirty-seventh flight): the dump key takes
// four consecutive frames of the left eye, each copied to a staging
// texture as it goes out and all written after the fourth, so the frames
// are the game's own consecutive ones -- a write's hitch between captures
// would space them by two hundred milliseconds. What the docking hub
// actually does from one frame to the next is not in the instance pool:
// its records jitter in place by a third of a degree either way while the
// drawn hub turns by a skinning bone the pool never shows (the pool's
// vertex shaders read a 48-byte bone palette at t0 under the record's
// quaternion), so it has to be measured from the picture.
// ...and LONG (2026-09-09 20:29): four raw frames gave a 0.15 deg baseline,
// a tenth of a pixel on a ring 190 px from the axis in the 2862 render,
// under the noise of an aliased frame. So the run is sixteen consecutive
// CROPS of the raw input, kEyeCrop pixels square about its centre (7.8 MB
// of staging each against 32 for a frame), written after the sixteenth,
// with the first treated frame whole for context: a 0.75 deg baseline,
// two pixels on that ring, and the frame-to-frame pattern of a part that
// steps or holds.
constexpr int    kEyeRun = 16;
constexpr uint32_t kEyeCrop = 1400;
ID3D11Texture2D* g_eyeRunStaging[2] = {};   // overview; AA-off also captures the right eye
// ...and the RAW frames beside them (eye_HHMMSS_R0..3.bmp): the game's
// render as the pass hands it to NVIDIA, before any history. The run of
// 18:43 (2026-09-09) showed why both are needed: NVIDIA's output is the
// history reprojected by the pass's own vectors blended with the new
// frame, so a turn measured on it is the vectors' as much as the
// object's; the raw frames alone say what the object did.
ID3D11Texture2D* g_eyeRawStaging[kEyeRun] = {};
// ...and the TREATED form (2026-09-10 06:10, advanced.eye_run_treated): the
// same sixteen crops of NVIDIA's output about its centre instead of the raw
// input's -- "j looks much better, I did still see some flickering": a
// flicker or a shimmer is the history's doing, and only its output shows
// it, frame to frame.
bool             g_eyeRunTreated = false;
bool             g_eyeRunPaired = true; // matching input/output sequence, default for new captures
ID3D11Texture2D* g_eyeTreatedStaging[kEyeRun] = {};
ID3D11Texture2D* g_eyeDecisionStaging[kEyeRun] = {};
ID3D11Texture2D* g_eyePreUiStaging[kEyeRun] = {};
int              g_eyeRunLeft = 0;    // captures still to take
int              g_eyeRunTaken = 0;
wchar_t          g_eyeRunStamp[16] = L"";
bool             g_eyeRunReady = false;
bool             g_eyeRunUntreated = false;
bool             g_eyeOverviewTaken[2] = {};
bool             g_eyeTreatedWritten[kEyeRun] = {};
bool             g_eyeTreatedTaken[kEyeRun] = {};
bool             g_eyeRawWritten[kEyeRun] = {};
// Slot 11 is free since the mesh records' coverage retired (2026-09-23);
// the slots after it keep their numbers. 16..18 (DlssColour, UiInfluence,
// UiDepth) went with the UI separation that filled them (2026-09-23).
constexpr int kEyeInputs=16;
ID3D11Texture2D*  g_eyeInputs[kEyeInputs] = {};
uint32_t         g_eyeInputsFrame=0,g_eyeInputsUiBound=0,g_eyeInputsUiFlags=0;
const wchar_t* const kEyeInputNames[kEyeInputs]={L"MV",L"Z",L"UI",L"Bias",L"SceneZ",L"TerrainIndex",L"TerrainZ",L"HoloCoverage",L"UiEdits",L"ScreenMotion",L"WeaponMotion",L"HoloContribution",L"PrevZ",L"DlssBeforeUi",L"UiPrevious",L"UiNext"};
uint32_t         g_eyeRunWidth = 0, g_eyeRunHeight = 0;
bool             g_eyeRawTaken[kEyeRun] = {};
uint32_t         g_eyeRunFrames[kEyeRun] = {};
uint32_t         g_eyeRawInputW[kEyeRun] = {}, g_eyeRawInputH[kEyeRun] = {};
uint32_t         g_eyeCaptureFrame = 0; // current scene frame, stamped before treatment
enum class EyeUiMode : uint8_t { None, Legacy };
struct EyeDecisionFrame {
    uint32_t frame = 0, diagnosticFrame = 0;
    uint32_t inputW = 0, inputH = 0, outputW = 0, outputH = 0;
    uint32_t decisionCrop[4] = {}, outputCrop[4] = {};
    bool diagnostic = false, preUi = false, dlssSuccess = false;
    bool dlssHistory = false, dlssReset = false;
    EyeUiMode uiMode = EyeUiMode::None;
    const char* error = "capture_not_reached";
};
EyeDecisionFrame g_eyeDecisions[kEyeRun] = {};
struct EyeMotionTrace {
    uint32_t frame, eye, flags, outputWidth, outputHeight;
    bool rowsOk, jumped, dlHistory;
    bool rowsBound;
    int rowsFollow;
    uint32_t sceneDraws;
    float prevRows[12], nowRows[12];
    PassParams params;
};
EyeMotionTrace g_eyeMotionTrace[kEyeRun * 4] = {};
uint32_t g_eyeMotionTraceCount = 0;
void writeEyeMotionTrace(const std::wstring& dir) {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_motion.csv", dir.c_str(), g_eyeRunStamp);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") || !f) {
        Log::get().note("temporal aa: could not write eye motion trace %ls.", path);
        return;
    }
    fprintf(f, "frame,eye,crop,rawCaptured,flags,outW,outH,rowsOk,jumped,dlHistory,history,inputW,inputH");
    auto names = [&](const char* name, int n) { for (int k = 0; k < n; ++k) fprintf(f, ",%s%d", name, k); };
    names("prev",12); names("now",12);
    names("tanNow",4); names("tanPrev",4); names("jitter",4);
    names("cameraR",12); names("cameraTv",4);
    names("headR",12); names("headTv",4);
    fprintf(f, ",projectionA,projectionB,rowsBound,rowsFollow,sceneDraws");
    fprintf(f, "\n");
    for (uint32_t i = 0; i < g_eyeMotionTraceCount; ++i) {
        const EyeMotionTrace& t = g_eyeMotionTrace[i];
        const PassParams& p = t.params;
        int crop = -1;
        for (int k = 0; k < g_eyeRunTaken; ++k) if (g_eyeRunFrames[k] == t.frame) crop = k;
        fprintf(f, "%u,%u,%d,%d,%u,%u,%u,%d,%d,%d,%d,%d,%d",
                t.frame, t.eye, crop, crop >= 0 && g_eyeRawTaken[crop], t.flags, t.outputWidth, t.outputHeight,
                t.rowsOk, t.jumped, t.dlHistory, p.haveHistory, p.size[0], p.size[1]);
        auto values = [&](const float* a, int n) { for (int k = 0; k < n; ++k) fprintf(f, ",%.9g", a[k]); };
        values(t.prevRows,12); values(t.nowRows,12);
        values(p.tanNow,4); values(p.tanPrev,4); values(p.jit,4);
        for (int r = 0; r < 3; ++r) values(p.cand[2][r],4);
        values(p.tvCam,4);
        values(p.dR0,4); values(p.dR1,4); values(p.dR2,4); values(p.tvUsed,4);
        fprintf(f, ",%.9g,%.9g,%d,%d,%u", p.knobs[0], p.knobs[2], t.rowsBound, t.rowsFollow, t.sceneDraws);
        fprintf(f, "\n");
    }
    const bool wrote = !ferror(f);
    const int closed = fclose(f);
    Log::get().note("temporal aa: eye motion trace %ls: %u eye evaluations, %s (scene-frame IDs link both eyes to the crop sequence).",
                    path, g_eyeMotionTraceCount, wrote && closed == 0 ? "written" : "write failed");
}
bool writeEyeBmp(ID3D11DeviceContext* ctx, ID3D11Texture2D* st, const D3D11_TEXTURE2D_DESC& d, int eye,
                 const wchar_t* pathIn);

float halfToFloat(uint16_t h) {
    const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0) {
        v = static_cast<float>(m) / 1024.0f * 6.103515625e-5f;   // subnormal: m * 2^-24
    } else if (e == 31) {
        v = m ? 0.0f : 65504.0f;                                  // nan reads black, inf white
    } else {
        v = (1.0f + static_cast<float>(m) / 1024.0f) * powf(2.0f, static_cast<float>(e) - 15.0f);
    }
    return s ? -v : v;
}

uint8_t dumpByte(float v, bool linear) {
    if (!(v > 0.0f)) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (linear) v = v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

void dumpEye(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int eye) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    ID3D11Texture2D* st = nullptr;
    const HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &st);
    dev->Release();
    if (FAILED(hr) || !st) {
        Log::get().note("temporal aa: the eye dump could not make its staging copy (0x%08lX); nothing written.",
                        static_cast<unsigned long>(hr));
        return;
    }
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, nullptr);
    writeEyeBmp(ctx, st, d, eye, nullptr);
    st->Release();
}

// The staging copy's pixels to a BMP: the given path, or the timestamped
// one (eye_HHMMSS_L.bmp). The staging texture is the caller's to release.
bool writeEyeBmp(ID3D11DeviceContext* ctx, ID3D11Texture2D* st, const D3D11_TEXTURE2D_DESC& d, int eye,
                 const wchar_t* pathIn) {
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms))) {
        Log::get().note("temporal aa: the eye dump could not map its staging copy; nothing written.");
        return false;
    }
    const uint32_t w = d.Width, h = d.Height;
    const uint32_t rowBytes = (w * 3u + 3u) & ~3u;
    std::vector<uint8_t> out(static_cast<size_t>(rowBytes) * h);
    bool known = true;
    for (uint32_t y = 0; y < h && known; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(ms.pData) + static_cast<size_t>(y) * ms.RowPitch;
        uint8_t* dst = out.data() + static_cast<size_t>(h - 1u - y) * rowBytes;   // BMP rows run bottom-up
        for (uint32_t x = 0; x < w; ++x) {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            bool linear = false;
            switch (d.Format) {
                case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                    r = src[x * 4 + 0] / 255.0f;
                    g = src[x * 4 + 1] / 255.0f;
                    b = src[x * 4 + 2] / 255.0f;
                    break;
                case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                case DXGI_FORMAT_B8G8R8X8_TYPELESS:
                case DXGI_FORMAT_B8G8R8X8_UNORM:
                case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
                    b = src[x * 4 + 0] / 255.0f;
                    g = src[x * 4 + 1] / 255.0f;
                    r = src[x * 4 + 2] / 255.0f;
                    break;
                case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                case DXGI_FORMAT_R10G10B10A2_UNORM: {
                    uint32_t v = 0;
                    memcpy(&v, src + x * 4, 4);
                    r = static_cast<float>(v & 1023u) / 1023.0f;
                    g = static_cast<float>((v >> 10) & 1023u) / 1023.0f;
                    b = static_cast<float>((v >> 20) & 1023u) / 1023.0f;
                    break;
                }
                case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                case DXGI_FORMAT_R16G16B16A16_FLOAT: {
                    uint16_t hv[3];
                    memcpy(hv, src + x * 8, 6);
                    r = halfToFloat(hv[0]);
                    g = halfToFloat(hv[1]);
                    b = halfToFloat(hv[2]);
                    linear = true;
                    break;
                }
                case DXGI_FORMAT_R32G32B32A32_FLOAT: {
                    float fv[3];
                    memcpy(fv, src + x * 16, 12);
                    r = fv[0];
                    g = fv[1];
                    b = fv[2];
                    linear = true;
                    break;
                }
                default:
                    known = false;
                    break;
            }
            if (!known) break;
            dst[x * 3 + 0] = dumpByte(b, linear);
            dst[x * 3 + 1] = dumpByte(g, linear);
            dst[x * 3 + 2] = dumpByte(r, linear);
        }
    }
    ctx->Unmap(st, 0);
    if (!known) {
        Log::get().note("temporal aa: the eye dump cannot read DXGI format %d; nothing written.",
                        static_cast<int>(d.Format));
        return false;
    }
    const std::wstring dir = Log::get().dir() + L"\\eyes";
    if (!g_eyeDumpDirMade) {
        g_eyeDumpDirMade = true;
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    SYSTEMTIME stm{};
    GetLocalTime(&stm);
    wchar_t path[MAX_PATH];
    if (pathIn) {
        wcsncpy_s(path, MAX_PATH, pathIn, _TRUNCATE);
    } else {
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%02u%02u%02u_%c.bmp", dir.c_str(),
                     static_cast<unsigned>(stm.wHour), static_cast<unsigned>(stm.wMinute),
                     static_cast<unsigned>(stm.wSecond), eye == 0 ? L'L' : L'R');
    }
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log::get().note("temporal aa: the eye dump could not open %ls for writing.", path);
        return false;
    }
    const uint32_t bytes = rowBytes * h;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + bytes;
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = static_cast<LONG>(h);
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = bytes;
    DWORD wrote = 0;
    bool ok = WriteFile(f, &fh, sizeof(fh), &wrote, nullptr) != 0;
    if (ok) ok = WriteFile(f, &ih, sizeof(ih), &wrote, nullptr) != 0;
    if (ok) ok = WriteFile(f, out.data(), bytes, &wrote, nullptr) != 0;
    CloseHandle(f);
    Log::get().note("temporal aa: eye %d dumped to %ls -- %ux%u, DXGI format %d, the treated frame as the "
                    "compositor receives it%s.",
                    eye, path, w, h, static_cast<int>(d.Format), ok ? "" : " (the write FAILED)");
    return ok;
}

// A staging copy of `tex` into slot k of `ring`, made or remade to its size.
bool stageEyeRun(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, ID3D11Texture2D** ring, int k) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    if (ring[k]) {
        D3D11_TEXTURE2D_DESC sd{};
        ring[k]->GetDesc(&sd);
        if (sd.Width != d.Width || sd.Height != d.Height || sd.Format != d.Format) {
            ring[k]->Release();
            ring[k] = nullptr;
        }
    }
    if (!ring[k]) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        const HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &ring[k]);
        dev->Release();
        if (FAILED(hr) || !ring[k]) {
            ring[k] = nullptr;
            Log::get().note("temporal aa: the eye run could not make a staging copy (0x%08lX); nothing written.",
                            static_cast<unsigned long>(hr));
            return false;
        }
    }
    ctx->CopySubresourceRegion(ring[k], 0, 0, 0, 0, tex, 0, nullptr);
    return true;
}

uint32_t g_rowsFrame = 0; // scene boundary counter, shared by captures and row selection
// Is the scanner's screen up this frame (the state above -- what feeds
// temporalPassWantsFssChrome -- says why it is asked)? Answered per eye
// and idempotent within a frame.
bool fssInterfaceLive() {
    const LONG stamp = fssChromeStampValue();
    if (stamp == 0) return false;
    if (!g_fssChromeStampKnown || stamp != g_fssChromeStampSeen) {
        g_fssChromeStampKnown = true;
        g_fssChromeStampSeen = stamp;
        g_fssChromeStampFrame = g_rowsFrame;
    }
    // This frame's bump, or last frame's: the heal's deferred eye is
    // finished at its partner's submit, and the boundary may land between.
    return g_rowsFrame - g_fssChromeStampFrame <= 1;
}
TemporalHistory<> g_temporalHistory;
std::mutex g_temporalHistoryMutex;
struct TemporalHistoryScope {
    TemporalHistoryEntry entry{};
    TemporalHistoryScope(int eye, unsigned flags, float jx, float jy) {
        LARGE_INTEGER q{}; QueryPerformanceCounter(&q);
        entry.qpc=q.QuadPart;entry.frame=g_rowsFrame;entry.eye=eye;
        entry.flags=flags;entry.jitterX=jx;entry.jitterY=jy;
    }
    ~TemporalHistoryScope() {
        std::lock_guard<std::mutex> lock(g_temporalHistoryMutex);
        g_temporalHistory.record(entry);
    }
};

// Preserve the actual first-frame inputs before the next eye overwrites them.
void stageEyeInputs(ID3D11DeviceContext* ctx,EyeState& e,ID3D11ShaderResourceView* scene,
                    ID3D11Texture2D* ui,float uiBound,float uiFlags) {
    if(g_eyeRunLeft<=0 || g_eyeRunTaken!=0 || g_eyeInputs[0])return;
    ID3D11Texture2D* textures[kEyeInputs]={e.dlMv,e.dlDepth,ui,e.dlMask};
    if(e.dlMv) {
        D3D11_TEXTURE2D_DESC d{};e.dlMv->GetDesc(&d);
        auto* edits=uiDepthContentChanges(d.Width,d.Height,0);
        if(edits) {Microsoft::WRL::ComPtr<ID3D11Resource> r;edits->GetResource(&r);r->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&textures[8]));}
        auto* screen=screenMotionView(0,d.Width,d.Height);
        if(screen){Microsoft::WRL::ComPtr<ID3D11Resource> r;screen->GetResource(&r);r->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&textures[9]));}
        auto* weapon=weaponMotionView();
        if(weapon){Microsoft::WRL::ComPtr<ID3D11Resource> r;weapon->GetResource(&r);r->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&textures[10]));}
    }
    if(scene) {
        ID3D11Resource* res=nullptr;scene->GetResource(&res);
        if(res){res->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&textures[4]));res->Release();}
    }
    celestialMotionStageDump(ctx,textures[4]);
    uiDepthHoloStageDump(ctx,textures[4]);
    if(textures[4]) {
        ID3D11ShaderResourceView* terrain[3]{}; celestialMotionViews(ctx,textures[4],terrain);
        for(int k=0;k<2;++k)if(terrain[k]) {
            ID3D11Resource* res=nullptr;terrain[k]->GetResource(&res);
            if(res){res->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&textures[k+5]));res->Release();}
        }
    }
    if(textures[4]) {
        ID3D11ShaderResourceView* holo[2]{}; uiDepthHoloMotion(0,textures[4],holo);
        if(holo[0]) {
            ID3D11Resource* res=nullptr; holo[0]->GetResource(&res);
            if(res) { res->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&textures[7])); res->Release(); }
        }
        // The generic hologram/icon depth pass's raw contribution (ui_depth.h):
        // the same RGBA16F target its resolve reads, sized like the scene
        // depth textures[4] already is, so a dump shows where coverage landed.
        D3D11_TEXTURE2D_DESC sceneDesc{}; textures[4]->GetDesc(&sceneDesc);
        ID3D11ShaderResourceView* holoContrib=nullptr;
        if(uiDepthHologramContribution(sceneDesc.Width,sceneDesc.Height,0,&holoContrib) && holoContrib) {
            ID3D11Resource* res=nullptr; holoContrib->GetResource(&res);
            if(res) { res->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&textures[11])); res->Release(); }
        }
    }
    // Copy before the depth swap; an absent file means history was invalid.
    if(e.zPrevValid && e.zPrev) { textures[12]=e.zPrev; textures[12]->AddRef(); }
    if(e.uiHistoryValid && e.uiHistory[e.uiHistoryRead]) {
        textures[14]=e.uiHistory[e.uiHistoryRead];textures[14]->AddRef();
    }
    for(int k=0;k<kEyeInputs;++k)if(textures[k])stageEyeRun(ctx,textures[k],g_eyeInputs,k);
    for(int k=5;k<kEyeInputs;++k)if(textures[k])textures[k]->Release();
    if(textures[4])textures[4]->Release();
    g_eyeInputsFrame=g_rowsFrame;g_eyeInputsUiBound=static_cast<uint32_t>(uiBound);
    g_eyeInputsUiFlags=static_cast<uint32_t>(uiFlags);
}
ID3D11ComputeShader* motionTraceShader(ID3D11DeviceContext* ctx) {
    if (!g_csMvTrace && !g_csMvTraceTried) {
        g_csMvTraceTried = true;
        g_csMvTrace = shaderSwapCreateCs(ctx, kTemporalMvTraceBytecode,
            sizeof(kTemporalMvTraceBytecode), "temporal_mv_trace_cs", "temporal aa capture");
    }
    return g_csMvTrace;
}

void writeEyeInputs(ID3D11DeviceContext* ctx,const std::wstring& dir) {
    for(int k=0;k<kEyeInputs;++k) {
        auto* texture=g_eyeInputs[k];if(!texture)continue;
        D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
        uint32_t bytes=0;
        switch(d.Format) {
        case DXGI_FORMAT_R8_UNORM:bytes=1;break;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:bytes=4;break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:bytes=8;break;
        case DXGI_FORMAT_R16_TYPELESS:case DXGI_FORMAT_D16_UNORM:bytes=2;break;
        case DXGI_FORMAT_R16G16_FLOAT:case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:case DXGI_FORMAT_R24G8_TYPELESS:case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R32_UINT:bytes=4;break;
        case DXGI_FORMAT_R32G32_FLOAT:case DXGI_FORMAT_R32G8X24_TYPELESS:case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:bytes=8;break;
        default:break;
        }
        D3D11_MAPPED_SUBRESOURCE map{};
        if(bytes && SUCCEEDED(ctx->Map(texture,0,D3D11_MAP_READ,0,&map))) {
            wchar_t path[MAX_PATH];_snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\eye_%s_%s.bin",dir.c_str(),g_eyeRunStamp,kEyeInputNames[k]);
            FILE* f=nullptr;_wfopen_s(&f,path,L"wb");
            if(f) {
                const uint32_t header[9]={1,d.Width,d.Height,static_cast<uint32_t>(d.Format),d.Width*bytes,g_eyeInputsFrame,0,g_eyeInputsUiBound,g_eyeInputsUiFlags};
                bool ok=fwrite("EDVRTEX1",1,8,f)==8 && fwrite(header,sizeof(header),1,f)==1;
                for(uint32_t y=0;y<d.Height && ok;++y)ok=fwrite(static_cast<const char*>(map.pData)+y*map.RowPitch,1,d.Width*bytes,f)==d.Width*bytes;
                fclose(f);
                Log::get().note("eye capture: %ls input %ls %ux%u format %u, scene frame %u: %s.",g_eyeRunStamp,kEyeInputNames[k],d.Width,d.Height,static_cast<unsigned>(d.Format),g_eyeInputsFrame,ok?"written":"write failed");
            }
            // The MV input's census of the history the pass invalidated
            // (backgroundHistoryHidden's size*2 sentinel), so a dump says in
            // the log how much of the eye NVIDIA was told to start afresh.
            // A still scene reads near zero; the eye run of 2026-09-17 11:48
            // read 1.81% (5.3% of the terrain) before the footprint guard,
            // and that was the terrain's shimmer.
            if(k==0 && d.Format==DXGI_FORMAT_R16G16_FLOAT) {
                uint32_t hidden=0;
                for(uint32_t y=0;y<d.Height;++y) {
                    const uint16_t* row=reinterpret_cast<const uint16_t*>(static_cast<const char*>(map.pData)+y*map.RowPitch);
                    for(uint32_t x=0;x<d.Width;++x) {
                        const uint16_t h=row[x*2];
                        const uint32_t e=(h>>10)&0x1Fu,m=h&0x3FFu;
                        // The sentinel is 2 * width, positive and normal.
                        const float v=(h&0x8000u)||e==0||e==31?0.0f:std::ldexp(1.0f+static_cast<float>(m)/1024.0f,static_cast<int>(e)-15);
                        if(v>static_cast<float>(d.Width))++hidden;
                    }
                }
                const double total=static_cast<double>(d.Width)*d.Height;
                Log::get().note("eye capture: %ls history hidden -- NVIDIA's lookup invalidated at %u of %.0f pixels (%.3f%% of the eye) on scene frame %u; a still scene reads near zero.",
                                g_eyeRunStamp,hidden,total,total>0?100.0*hidden/total:0.0,g_eyeInputsFrame);
            }
            ctx->Unmap(texture,0);
        }
        texture->Release();g_eyeInputs[k]=nullptr;
    }
    celestialMotionWriteDump(ctx,dir.c_str(),g_eyeRunStamp);
    uiDepthHoloWriteDump(ctx,dir.c_str(),g_eyeRunStamp);
}

bool stageEyeCrop(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, ID3D11Texture2D** slot, uint32_t* cwOut,
                  uint32_t* chOut, const uint32_t* region = nullptr,
                  uint32_t wantW = kEyeCrop, uint32_t wantH = kEyeCrop,
                  uint32_t* xOut = nullptr, uint32_t* yOut = nullptr) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    const uint32_t width = region ? region[2] - region[0] : d.Width;
    const uint32_t height = region ? region[3] - region[1] : d.Height;
    const uint32_t cw = width < wantW ? width : wantW;
    const uint32_t ch = height < wantH ? height : wantH;
    if (*slot) {
        D3D11_TEXTURE2D_DESC sd{};
        (*slot)->GetDesc(&sd);
        if (sd.Width != cw || sd.Height != ch || sd.Format != d.Format) {
            (*slot)->Release();
            *slot = nullptr;
        }
    }
    if (!*slot) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Width = cw;
        sd.Height = ch;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        const HRESULT hr = dev->CreateTexture2D(&sd, nullptr, slot);
        dev->Release();
        if (FAILED(hr) || !*slot) {
            *slot = nullptr;
            Log::get().note("temporal aa: the eye run could not make a crop's staging copy (0x%08lX); nothing "
                            "written.", static_cast<unsigned long>(hr));
            return false;
        }
    }
    D3D11_BOX box{};
    box.left = (region ? region[0] : 0) + (width - cw) / 2;
    box.top = (region ? region[1] : 0) + (height - ch) / 2;
    box.right = box.left + cw;
    box.bottom = box.top + ch;
    box.front = 0;
    box.back = 1;
    ctx->CopySubresourceRegion(*slot, 0, 0, 0, 0, tex, 0, &box);
    *cwOut = cw;
    *chOut = ch;
    if (xOut) *xOut = box.left;
    if (yOut) *yOut = box.top;
    return true;
}

void eyeOutputCropSize(int k, ID3D11Texture2D* output, uint32_t* wantW, uint32_t* wantH) {
    *wantW = kEyeCrop;
    *wantH = kEyeCrop;
    if (k < 0 || k >= kEyeRun || !output || !g_eyeRunPaired || !g_eyeRawTaken[k] ||
        !g_eyeRawInputW[k] || !g_eyeRawInputH[k]) return;
    D3D11_TEXTURE2D_DESC raw{}, out{};
    g_eyeRawStaging[k]->GetDesc(&raw);
    output->GetDesc(&out);
    *wantW = static_cast<uint32_t>((static_cast<uint64_t>(raw.Width) * out.Width +
                                    g_eyeRawInputW[k] - 1) / g_eyeRawInputW[k]);
    *wantH = static_cast<uint32_t>((static_cast<uint64_t>(raw.Height) * out.Height +
                                    g_eyeRawInputH[k] - 1) / g_eyeRawInputH[k]);
}

const char* eyeUiModeName(EyeUiMode mode) {
    switch (mode) {
    case EyeUiMode::Legacy: return "legacy";
    default: return "none";
    }
}

bool writeEyeDecisionBin(ID3D11DeviceContext* ctx, ID3D11Texture2D* texture,
                         uint32_t frame, const wchar_t* path) {
    if (!ctx || !texture) return false;
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R32G32B32A32_FLOAT) return false;
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(texture, 0, D3D11_MAP_READ, 0, &map))) return false;
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"wb");
    bool ok = f != nullptr;
    if (f) {
        const uint32_t rowBytes = d.Width * 16;
        const uint32_t header[9] = {1, d.Width, d.Height, static_cast<uint32_t>(d.Format),
                                    rowBytes, frame, 0, 0, 0};
        ok = fwrite("EDVRTEX1", 1, 8, f) == 8 && fwrite(header, sizeof(header), 1, f) == 1;
        for (uint32_t y = 0; y < d.Height && ok; ++y)
            ok = fwrite(static_cast<const char*>(map.pData) + y * map.RowPitch,
                        1, rowBytes, f) == rowBytes;
        if (fclose(f) != 0) ok = false;
    }
    ctx->Unmap(texture, 0);
    return ok;
}

void writeEyeDecisionArtifacts(ID3D11DeviceContext* ctx, const std::wstring& dir) {
    for (int k = 0; k < g_eyeRunTaken; ++k) {
        EyeDecisionFrame& d = g_eyeDecisions[k];
        wchar_t path[MAX_PATH];
        if (d.preUi && g_eyePreUiStaging[k]) {
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_P%02d.bmp", dir.c_str(), g_eyeRunStamp, k);
            D3D11_TEXTURE2D_DESC desc{}; g_eyePreUiStaging[k]->GetDesc(&desc);
            if (!writeEyeBmp(ctx, g_eyePreUiStaging[k], desc, 0, path)) {
                d.preUi = false;
                d.error = "pre_ui_write_failed";
            }
        }
        if (d.diagnostic && g_eyeDecisionStaging[k]) {
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_D%02d.bin", dir.c_str(), g_eyeRunStamp, k);
            if (!writeEyeDecisionBin(ctx, g_eyeDecisionStaging[k], d.diagnosticFrame, path)) {
                d.diagnostic = false;
                d.error = "decision_write_failed";
            }
        }
        if (d.diagnostic && d.preUi && g_eyeRawWritten[k] && g_eyeTreatedWritten[k] && d.dlssSuccess) d.error = "";
        if (g_eyePreUiStaging[k]) { g_eyePreUiStaging[k]->Release(); g_eyePreUiStaging[k]=nullptr; }
        if (g_eyeDecisionStaging[k]) { g_eyeDecisionStaging[k]->Release(); g_eyeDecisionStaging[k]=nullptr; }
    }
    wchar_t manifest[MAX_PATH];
    _snwprintf_s(manifest, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_decisions.json", dir.c_str(), g_eyeRunStamp);
    FILE* f = nullptr;
    if (_wfopen_s(&f, manifest, L"wb") || !f) {
        Log::get().note("eye capture: could not write decision manifest %ls.", manifest);
        for (EyeState& e : g_eye) {
            if (e.dlDecisionUav) { e.dlDecisionUav->Release(); e.dlDecisionUav=nullptr; }
            if (e.dlDecision) { e.dlDecision->Release(); e.dlDecision=nullptr; }
        }
        return;
    }
    fprintf(f, "{\n  \"schema\": 1,\n  \"stamp\": \"%ls\",\n  \"requested\": %d,\n  \"frames\": [\n",
            g_eyeRunStamp, kEyeRun);
    for (int k = 0; k < g_eyeRunTaken; ++k) {
        const EyeDecisionFrame& d = g_eyeDecisions[k];
        fprintf(f, "    {\"index\": %d, \"frame\": %u, \"diagnostic_frame\": %u, ",
                k, d.frame, d.diagnosticFrame);
        fprintf(f, "\"input_size\": [%u, %u], \"decision_crop\": [%u, %u, %u, %u], ",
                d.inputW, d.inputH, d.decisionCrop[0], d.decisionCrop[1],
                d.decisionCrop[2], d.decisionCrop[3]);
        fprintf(f, "\"output_size\": [%u, %u], \"output_crop\": [%u, %u, %u, %u], ",
                d.outputW, d.outputH, d.outputCrop[0], d.outputCrop[1],
                d.outputCrop[2], d.outputCrop[3]);
        if (d.diagnostic) fprintf(f, "\"decision_file\": \"eye_%ls_D%02d.bin\", ", g_eyeRunStamp, k);
        else fputs("\"decision_file\": null, ", f);
        if (d.preUi) fprintf(f, "\"pre_ui_file\": \"eye_%ls_P%02d.bmp\", ", g_eyeRunStamp, k);
        else fputs("\"pre_ui_file\": null, ", f);
        if (g_eyeRawWritten[k]) fprintf(f, "\"raw_file\": \"eye_%ls_C%02d.bmp\", ", g_eyeRunStamp, k);
        else fputs("\"raw_file\": null, ", f);
        if (g_eyeTreatedWritten[k]) fprintf(f, "\"treated_file\": \"eye_%ls_T%02d.bmp\", ", g_eyeRunStamp, k);
        else fputs("\"treated_file\": null, ", f);
        fprintf(f, "\"ui_mode\": \"%s\", \"dlss_success\": %s, \"dlss_history\": %s, "
                   "\"dlss_reset\": %s, \"error\": \"%s\"}%s\n",
                eyeUiModeName(d.uiMode), d.dlssSuccess ? "true" : "false",
                d.dlssHistory ? "true" : "false", d.dlssReset ? "true" : "false",
                d.error ? d.error : "", k + 1 == g_eyeRunTaken ? "" : ",");
    }
    fputs("  ]\n}\n", f);
    const bool clean = !ferror(f);
    const int closed = fclose(f);
    const bool ok = clean && closed == 0;
    Log::get().note("eye capture: per-frame DLSS decision manifest %ls: %s.", manifest,
                    ok ? "written" : "write failed");
    for (EyeState& e : g_eye) {
        if (e.dlDecisionUav) { e.dlDecisionUav->Release(); e.dlDecisionUav=nullptr; }
        if (e.dlDecision) { e.dlDecision->Release(); e.dlDecision=nullptr; }
    }
}

// The run's write after its last crop: the sixteen crops (raw C00.., or
// treated T00..) and the first treated frame whole.
void writeEyeRun(ID3D11DeviceContext* ctx, uint32_t cw, uint32_t ch) {
    const std::wstring dir = Log::get().dir() + L"\\eyes";
    if (!g_eyeDumpDirMade) {
        g_eyeDumpDirMade = true;
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    const bool paired = g_eyeRunPaired && !g_eyeRunUntreated;
    writeEyeInputs(ctx,dir);
    const bool treated = (g_eyeRunPaired || g_eyeRunTreated) && !g_eyeRunUntreated;
    ID3D11Texture2D** ring = treated ? g_eyeTreatedStaging : g_eyeRawStaging;
    int wrote = 0, wroteTreated = 0;
    for (int i = 0; i < g_eyeRunTaken; ++i) {
        wchar_t path[MAX_PATH];
        D3D11_TEXTURE2D_DESC sd{};
        if (ring[i] && (!treated || g_eyeTreatedTaken[i])) {
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_%c%02d.bmp", dir.c_str(), g_eyeRunStamp,
                         treated ? L'T' : L'C', i);
            ring[i]->GetDesc(&sd);
            const bool wroteImage = writeEyeBmp(ctx, ring[i], sd, 0, path);
            if (wroteImage) ++wrote;
            if (treated) {
                g_eyeTreatedWritten[i] = wroteImage;
                if (!wroteImage) g_eyeDecisions[i].error="treated_write_failed";
            }
        }
        if (paired && g_eyeRawTaken[i] && g_eyeRawStaging[i]) {
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_C%02d.bmp", dir.c_str(), g_eyeRunStamp, i);
            g_eyeRawStaging[i]->GetDesc(&sd);
            g_eyeRawWritten[i] = writeEyeBmp(ctx, g_eyeRawStaging[i], sd, 0, path);
            if (g_eyeRawWritten[i]) ++wroteTreated;
            else g_eyeDecisions[i].error="raw_write_failed";
        }
    }
    if (g_eyeRunStaging[0]) {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_L0.bmp", dir.c_str(), g_eyeRunStamp);
        D3D11_TEXTURE2D_DESC sd{};
        g_eyeRunStaging[0]->GetDesc(&sd);
        if (writeEyeBmp(ctx, g_eyeRunStaging[0], sd, 0, path)) ++wroteTreated;
    }
    if (g_eyeRunUntreated) {
        if (g_eyeOverviewTaken[1] && g_eyeRunStaging[1]) {
            wchar_t path[MAX_PATH];D3D11_TEXTURE2D_DESC sd{};
            _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\eye_%s_R0.bmp",dir.c_str(),g_eyeRunStamp);
            g_eyeRunStaging[1]->GetDesc(&sd);writeEyeBmp(ctx,g_eyeRunStaging[1],sd,1,path);
        }
        wchar_t path[MAX_PATH];_snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\eye_%s_capture.csv",dir.c_str(),g_eyeRunStamp);
        FILE* file=nullptr;_wfopen_s(&file,path,L"wb");
        if(file) {
            fprintf(file,"frame,crop,mode,inputW,inputH,cropW,cropH\n");
            for(int k=0;k<g_eyeRunTaken;++k)fprintf(file,"%u,%d,off,%u,%u,%u,%u\n",g_eyeRunFrames[k],k,g_eyeRawInputW[k],g_eyeRawInputH[k],cw,ch);
            fclose(file);
        }
        Log::get().note("eye capture: AA off; %d untreated crops C00..%02d (%ux%u), left/right overviews and capture.csv written for run %ls.",
                        wrote,g_eyeRunTaken-1,cw,ch,g_eyeRunStamp);
    } else if (g_eyeRunPaired) {
        writeEyeDecisionArtifacts(ctx, dir);
        writeEyeMotionTrace(dir);
        Log::get().note("temporal aa: paired eye run %ls: %d treated crops T00..%02d (%ux%u), "
                        "%d raw crops plus overview written. C and T share scene-frame IDs in "
                        "eye_%ls_motion.csv; their pixel scales follow inputW/inputH and outW/outH. "
                        "Copies were taken together; files written after both eyes completed.",
                        g_eyeRunStamp, wrote, g_eyeRunTaken - 1, cw, ch, wroteTreated, g_eyeRunStamp);
    } else if (g_eyeRunTreated) {
        Log::get().note("temporal aa: an eye run of %d consecutive TREATED crops of the left eye is on disk "
                        "(eye_%ls_T00..%02d.bmp, %ux%u about the output's centre, NVIDIA's output as the "
                        "compositor receives it; advanced.eye_run_treated) with the first frame whole (%d "
                        "written, eye_%ls_L0.bmp): what the history made of consecutive frames, for a flicker "
                        "or a shimmer measured frame to frame; the object probe's ledger of the same frames "
                        "follows when it is on (object_probe.h).",
                        wrote, g_eyeRunStamp, g_eyeRunTaken - 1, cw, ch, wroteTreated, g_eyeRunStamp);
    } else {
        Log::get().note("temporal aa: an eye run of %d consecutive raw crops of the left eye is on disk "
                        "(eye_%ls_C00..%02d.bmp, %ux%u about the input's centre, the game's render as handed to "
                        "NVIDIA) with the first treated frame whole (%d written, eye_%ls_L0.bmp): the frames the "
                        "game drew in a row, for what a part does from one to the next; the object probe's ledger "
                        "of the same frames follows when it is on (object_probe.h).",
                        wrote, g_eyeRunStamp, g_eyeRunTaken - 1, cw, ch, wroteTreated, g_eyeRunStamp);
    }
    g_eyeRunTaken = 0;
}

// Called at Submit even when temporal AA is disabled. Copies only; no
// reprojection, history, shader binding or change to the submitted texture.
void captureUntreatedEye(ID3D11Texture2D* tex,int eye,const float* bounds) {
    if (!tex || eye<0 || eye>1 || (g_eyeRunLeft<=0 && !g_eyeRunReady)) return;
    D3D11_TEXTURE2D_DESC td{};tex->GetDesc(&td);
    if(td.SampleDesc.Count!=1 || td.ArraySize!=1 || td.MipLevels!=1) return;
    uint32_t region[4]{};bool flipU=false,flipV=false;
    if(!supersampleRegionFromBounds(td.Width,td.Height,bounds,region,&flipU,&flipV))return;
    ID3D11Device* dev=nullptr;ID3D11DeviceContext* ctx=nullptr;
    tex->GetDevice(&dev);if(!dev)return;dev->GetImmediateContext(&ctx);dev->Release();if(!ctx)return;
    g_eyeRunUntreated=true;
    const uint32_t w=region[2]-region[0],h=region[3]-region[1];uint32_t cw=0,ch=0;
    if(!g_eyeOverviewTaken[eye])g_eyeOverviewTaken[eye]=stageEyeCrop(ctx,tex,&g_eyeRunStaging[eye],&cw,&ch,region,w,h);
    if(eye==0 && g_eyeRunLeft>0 && g_eyeRunTaken<kEyeRun) {
        const int k=g_eyeRunTaken;
        if(stageEyeCrop(ctx,tex,&g_eyeRawStaging[k],&cw,&ch,region)) {
            g_eyeRawTaken[k]=true;g_eyeRawInputW[k]=w;g_eyeRawInputH[k]=h;
            g_eyeRunFrames[k]=g_rowsFrame;objectProbeLedgerMark(k);
            ++g_eyeRunTaken;--g_eyeRunLeft;
            if(g_eyeRunLeft==0){g_eyeRunReady=true;g_eyeRunWidth=cw;g_eyeRunHeight=ch;}
        }
    }
    if(eye==1 && g_eyeRunReady){writeEyeRun(ctx,g_eyeRunWidth,g_eyeRunHeight);g_eyeRunReady=false;}
    ctx->Release();
}

// THE EYE RUN's raw capture: a crop of the pass's input colour about its
// centre into the next slot, and the write after the last (kEyeRun says).
// Under advanced.eye_run_treated the treated hook takes the run instead.
void captureEyeRunRaw(ID3D11DeviceContext* ctx, ID3D11Texture2D* colour) {
    if (g_eyeRunPaired || g_eyeRunTreated) return;
    const int k = g_eyeRunTaken;
    if (!colour || k < 0 || k >= kEyeRun) { g_eyeRunLeft = 0; return; }
    uint32_t cw = 0, ch = 0;
    if (!stageEyeCrop(ctx, colour, &g_eyeRawStaging[k], &cw, &ch)) { g_eyeRunLeft = 0; return; }
    objectProbeLedgerMark(k);   // the ledger's frame for this crop
    ++g_eyeRunTaken;
    --g_eyeRunLeft;
    if (g_eyeRunLeft > 0) return;
    writeEyeRun(ctx, cw, ch);
}

// THE EYE RUN's treated capture: the run's first frame whole, as the
// compositor receives it, for context (the raw crops are the measurement)
// -- or, under advanced.eye_run_treated, the sixteen crops themselves.
void captureEyeRun(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex) {
    if (!g_eyeRunPaired && !g_eyeRunTreated) {
        if (g_eyeRunTaken != 1) return;   // the first raw crop was just taken this frame
        stageEyeRun(ctx, tex, g_eyeRunStaging, 0);
        return;
    }
    const int k = g_eyeRunTaken;
    if (!tex || k < 0 || k >= kEyeRun) {
        if (k >= 0 && k < kEyeRun) ++g_eyeRunTaken;
        g_eyeRunLeft = 0; g_eyeRunReady = g_eyeRunTaken > 0; return;
    }
    if (k == 0) stageEyeRun(ctx, tex, g_eyeRunStaging, 0);
    uint32_t cw = 0, ch = 0;
    uint32_t wantW=0, wantH=0, cropX=0, cropY=0;
    eyeOutputCropSize(k, tex, &wantW, &wantH);
    if (!stageEyeCrop(ctx, tex, &g_eyeTreatedStaging[k], &cw, &ch, nullptr, wantW, wantH,
                      &cropX, &cropY)) {
        g_eyeDecisions[k].error="treated_stage_failed";
        ++g_eyeRunTaken; g_eyeRunLeft = 0; g_eyeRunReady = true; return;
    }
    EyeDecisionFrame& decision = g_eyeDecisions[k];
    if (decision.preUi && (decision.outputCrop[0]!=cropX || decision.outputCrop[1]!=cropY ||
                           decision.outputCrop[2]!=cw || decision.outputCrop[3]!=ch)) {
        decision.preUi=false;decision.error="pre_ui_crop_mismatch";
    }
    decision.outputCrop[0]=cropX;decision.outputCrop[1]=cropY;
    decision.outputCrop[2]=cw;decision.outputCrop[3]=ch;
    g_eyeTreatedTaken[k]=true;
    g_eyeRunFrames[k] = g_eyeCaptureFrame;
    objectProbeLedgerMark(k);
    ++g_eyeRunTaken;
    --g_eyeRunLeft;
    if (g_eyeRunLeft > 0) return;
    if (g_eyeRunPaired) {
        g_eyeRunReady = true;
        g_eyeRunWidth = cw;
        g_eyeRunHeight = ch;
        return;
    }
    writeEyeRun(ctx, cw, ch);
}

// A shader view over the interface's coverage mask (ui_depth.h), cached
// per eye on the texture's identity. Two readers: the mv entry folds it
// into NVIDIA's bias mask, and the body path keeps its hands off the
// pixels it marks -- the station's target brackets and its label sit at
// the station's distance in depth and inside its grid, and they do not
// turn with it (the body path's fourth flight, 2026-09-08: "artifacts
// particularly with the 3d targeting UI", the text "smearing").
bool ensureUiMaskSrv(ID3D11Device* dev, EyeState& e, ID3D11Texture2D* mask) {
    if (!dev || !mask) return false;
    if (e.uiMaskRes == static_cast<void*>(mask) && e.uiMaskSrv) return true;
    if (e.uiMaskSrv) { e.uiMaskSrv->Release(); e.uiMaskSrv = nullptr; }
    e.uiMaskRes = nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC md{};
    md.Format = DXGI_FORMAT_R8_UNORM;
    md.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    md.Texture2D.MipLevels = 1;
    if (SUCCEEDED(dev->CreateShaderResourceView(mask, &md, &e.uiMaskSrv)) && e.uiMaskSrv) {
        e.uiMaskRes = mask;
        return true;
    }
    e.uiMaskSrv = nullptr;
    return false;
}
float    g_foveaDeg = 0.0f;        // advanced.temporal_aa_fovea: NVIDIA runs on a crop this many degrees across; 0 = whole frame
bool     g_foveaEdges = false;     // advanced.temporal_aa_fovea = "edges": the crop is placed by its own edges (below) instead of a width
float    g_foveaVerticalDeg = 0.0f; // advanced.temporal_aa_fovea_vertical: edges mode, degrees off the top AND the bottom
float    g_foveaTopDeg = 0.0f;      // advanced.temporal_aa_fovea_top: edges mode, "same" (default) follows temporal_aa_fovea_vertical, else its own degrees off the top edge
float    g_foveaBottomDeg = 0.0f;   // advanced.temporal_aa_fovea_bottom: edges mode, "same" (default) follows temporal_aa_fovea_vertical, else its own degrees off the bottom edge
float    g_foveaOuterDeg = 0.0f;    // advanced.temporal_aa_fovea_outer: edges mode, degrees off each eye's temple-side edge
float    g_foveaNasalDeg = 0.0f;    // advanced.temporal_aa_fovea_nasal: edges mode, degrees off each eye's nose-side edge
float    g_foveaEdgeDeg = 6.0f;    // advanced.temporal_aa_fovea_edge: the blend band, in degrees
// advanced.temporal_aa_fovea_lead is g_foveaLeadFrames, declared far above
// with its own state and the price report that prints it (THE HEAD LEAD).
float    g_peripheryCalm = 0.4f;   // advanced.temporal_aa_periphery_calm: how much the own history is eased toward the periphery (0 uniform, 1 max), the sharp periphery only
bool     g_periphSteady = true;    // advanced.temporal_aa_periphery: steady (NVIDIA's DLAA on a reduced copy) or sharp (the own history at full size)
float    g_periphScale = 0.5f;     // advanced.temporal_aa_periphery_scale: the steady periphery's size as a fraction of the output each way
bool     g_foveaRound = true;      // advanced.temporal_aa_fovea_shape: round (a disc) or square (the crop)
float    g_foveaDistance = 0.0f;   // advanced.temporal_aa_fovea_distance: where the two eyes' discs meet in depth, metres (0 = infinity: the straight-ahead point); NOT read in edges mode
// True when the fovea is configured on, in EITHER mode -- the gate every
// call site used to spell g_foveaDeg > 0.0f, before edges mode existed.
inline bool foveaConfigured() { return g_foveaDeg > 0.0f || g_foveaEdges; }
int      g_rowsFollow = 0;         // bound populated scene: trusted; auxiliary chain: head-follow score, needs >= 0
bool     g_rowsFollowNoted = false;
bool     g_warmNoted = false;

// The camera capture: a ring of the last writes of every scene-block-sized
// buffer the game maps (the object, the rows, the frame, the order), the
// object bound at this frame's first scene draw, the rows CHOSEN for the
// frame (chooseCameraRows), and last frame's.
struct RowsWrite {
    const void* buf = nullptr;
    float       rows[12] = {};
    float       proj[2] = {};   // the projection's z row: the depth written is proj[0] + proj[1] / z
    uint32_t    frame = 0;
    uint32_t    seq = 0;
    uint32_t    observedSeq = 0; // diagnostic clock, includes rejected non-rotation writes
    bool        valid = false;
};
enum CameraChoiceFlag : uint32_t {
    kChoiceValid          = 1u << 0,
    kChoiceBoundSeen      = 1u << 1,
    kChoiceBoundLatchSeen = 1u << 2,
    kChoiceSelectedBound  = 1u << 3,
    kChoiceContinuous     = 1u << 4,
    kChoiceTwinPresent    = 1u << 5,
    kChoiceTwinFallback   = 1u << 6,
    kChoiceResync         = 1u << 7,
    kChoiceRefollow       = 1u << 8,
    kChoiceBoundLatchRows = 1u << 9,
};
enum CameraDrawFlag : uint32_t {
    kDrawSeen             = 1u << 0,
    kDrawWriteObserved    = 1u << 1,
    kDrawRowsValid        = 1u << 2,
    kDrawPreviousValid    = 1u << 3,
    kDrawSelectedResource = 1u << 4,
    kDrawSelectedWrite    = 1u << 5,
    kDrawSelectedRows     = 1u << 6,
    kDrawSelectionLater   = 1u << 7,
};
struct RowsChoiceCapture {
    const void* selectedResource = nullptr;
    const void* boundResource = nullptr;
    float selectedRows[12] = {};
    float selectedProj[2] = {};
    uint32_t frame = 0, flags = 0;
    uint32_t selectedSeq = 0, boundLatchSeq = 0, twinSeq = 0;
    uint32_t writesAtChoice = 0, observedWritesAtChoice = 0;
    uint32_t observedEvictionsAtChoice = 0;
    uint32_t candidates = 0, boundCandidates = 0;
    uint32_t continuousCandidates = 0, twinCandidates = 0;
};
struct RigidDrawRows {
    const void* resource = nullptr;
    float rows[12] = {};
    float proj[2] = {};
    uint64_t vsHash = 0;
    uint32_t frame = 0, seq = 0, writesAtDraw = 0, observedWritesAtDraw = 0;
    uint32_t observedEvictionsAtDraw = 0;
    bool seen = false, observed = false, valid = false;
};
struct ObservedRowsWrite {
    const void* resource = nullptr;
    float rows[12] = {};
    float proj[2] = {};
    uint32_t frame = 0, seq = 0;
    bool rowsValid = false;
};
// 256: in space the game writes the block over a hundred times a frame
// (114 measured 2026-09-04), and a ring of 48 had lost the frame's early
// writes -- the eyes' among them, drawn before the reflections -- by
// the time the frame was chosen.
constexpr int kRowsRing = 256;
RowsWrite   g_rowsRing[kRowsRing];
uint32_t    g_rowsSeq = 0;           // writes ever, the ring's clock
uint32_t    g_rowsObservedSeq = 0;   // all mapped writes, including rejected rows
uint32_t    g_rowsObservedWrites = 0;
uint32_t    g_rowsObservedEvictions = 0;
ObservedRowsWrite g_rowsObserved[32];
const void* g_boundBuf = nullptr;    // the object bound at this frame's first scene draw
bool        g_boundSeen = false;
uint32_t    g_boundLatchSeq = 0;
bool        g_boundLatchValid = false;
bool        g_boundLatchRowsValid = false;
uint32_t    g_rowsWrites = 0;        // writes this frame
uint64_t    g_rowsWritesSum = 0;     // ...summed over the interval
uint32_t    g_rowsFramesSum = 0;
uint64_t    g_candSumCount = 0;      // this frame's candidate writes, summed
uint32_t    g_chooseBound = 0;       // frames whose chosen rows were the bound object's
uint32_t    g_chooseOther = 0;       // ...another object's, by continuity
uint32_t    g_chooseResync = 0;      // ...nothing followed last frame's: the latest taken
uint32_t    g_chooseRefollow = 0;    // ...the bound block's, taken over a continuous chain that had stopped following the head
uint32_t    g_chooseNone = 0;        // ...no write this frame at all
bool        g_chosenThisFrame = false;
int         g_latchSlotVs = -1;      // where the bound block was found, for the log
int         g_latchSlotPs = -1;
// The world path's gate on the rows' delta (temporal_math.h): the last
// accepted delta, and whether the rows a frame measures from are the view's
// own. Both eyes judge a frame; it moves on at the frame boundary.
TemporalCameraGate g_cameraGate;
uint32_t    g_camCarried = 0;        // frames the ship's delta was carried over a drop
uint32_t    g_camCarriedJump = 0;    // ...of which carried a translation over 50 m: zero by construction
float    g_curRows[12] = {};
float    g_curProj[2] = {};        // the picked write's projection z row (A, B); B > 0 once read
bool     g_projNoted = false;      // the encoding line, once
bool     g_curValid = false;
bool     g_curRowsBound = false;
bool     g_curLatched = false;
float    g_prevRows[12] = {};
bool     g_prevValid = false;
uint32_t g_camPairs = 0;
bool     g_camNoted = false;
RowsChoiceCapture g_rowsChoice;
RigidDrawRows g_rigidDraw[2];
RigidDrawRows g_prevRigidDraw[2];

ObservedRowsWrite* observedRowsSlot(const void* resource) {
    int freeSlot = -1, oldest = 0;
    for (int i = 0; i < static_cast<int>(sizeof(g_rowsObserved) / sizeof(g_rowsObserved[0])); ++i) {
        if (g_rowsObserved[i].resource == resource) return &g_rowsObserved[i];
        if (!g_rowsObserved[i].resource && freeSlot < 0) freeSlot = i;
        if (g_rowsObserved[i].frame < g_rowsObserved[oldest].frame) oldest = i;
    }
    const int at = freeSlot >= 0 ? freeSlot : oldest;
    if (freeSlot < 0) ++g_rowsObservedEvictions;
    g_rowsObserved[at] = ObservedRowsWrite{};
    g_rowsObserved[at].resource = resource;
    return &g_rowsObserved[at];
}

const ObservedRowsWrite* observedRowsCurrent(const void* resource) {
    if (!resource) return nullptr;
    for (const auto& w : g_rowsObserved) {
        if (w.resource == resource && w.frame == g_rowsFrame) return &w;
    }
    return nullptr;
}

void diagnosticDeltaFromRows(const float prev[12], const float now[12],
                             float rotation[9], float translation[3]) {
    float rp[9], rn[9], rpT[9];
    temporalRot3Of34(prev, rp);
    temporalRot3Of34(now, rn);
    temporalTranspose3(rp, rpT);
    temporalMul3(rpT, rn, rotation);
    const float dc[3] = {now[3] - prev[3], now[7] - prev[7], now[11] - prev[11]};
    temporalApply3(rpT, dc, translation);
    rotation[2] = -rotation[2];
    rotation[5] = -rotation[5];
    rotation[6] = -rotation[6];
    rotation[7] = -rotation[7];
    translation[2] = -translation[2];
}

// The frame's camera rows, chosen once per frame at its first treat from
// the frame's writes: the one that FOLLOWS last frame's chosen rows within
// 3 degrees in absolute orientation (a ship turns under 2 a frame; a
// reflection face's or a shadow cascade's camera sits tens away), the
// bound object's preferred, else the latest such write. With nothing
// continuous (the first frame, a cut) the bound object's latest write,
// else the frame's latest. The bound block at the first scene draw held
// a reflection face's camera on half the frames in space (2026-09-04):
// the game draws into the scene's depth before it rewrites the block.
void chooseCameraRows() {
    if (g_chosenThisFrame) return;
    g_chosenThisFrame = true;
    g_curValid = false;
    g_curRowsBound = false;
    g_rowsChoice = RowsChoiceCapture{};
    g_rowsChoice.frame = g_rowsFrame;
    g_rowsChoice.boundResource = g_boundBuf;
    g_rowsChoice.boundLatchSeq = g_boundLatchSeq;
    g_rowsChoice.writesAtChoice = g_rowsWrites;
    g_rowsChoice.observedWritesAtChoice = g_rowsObservedWrites;
    g_rowsChoice.observedEvictionsAtChoice = g_rowsObservedEvictions;
    if (g_boundSeen) g_rowsChoice.flags |= kChoiceBoundSeen;
    if (g_boundLatchValid) g_rowsChoice.flags |= kChoiceBoundLatchSeen;
    if (g_boundLatchRowsValid) g_rowsChoice.flags |= kChoiceBoundLatchRows;
    int bestIdx = -1, fallIdx = -1;
    uint32_t bestSeq = 0, fallSeq = 0;
    bool bestBound = false, fallBound = false;
    uint32_t count = 0, boundCount = 0, continuousCount = 0, twinCount = 0;
    int contIdx[kRowsRing];
    int contN = 0;
    int twinIdx = -1;
    uint32_t twinSeq = 0;
    bool twinBound = false;
    float rpT[9] = {};
    if (g_prevValid) {
        float rp[9];
        temporalRot3Of34(g_prevRows, rp);
        temporalTranspose3(rp, rpT);
    }
    for (int i = 0; i < kRowsRing; ++i) {
        const RowsWrite& w = g_rowsRing[i];
        if (!w.valid || w.frame != g_rowsFrame) continue;
        ++count;
        const bool bound = g_boundSeen && w.buf == g_boundBuf;
        if (bound) ++boundCount;
        bool continuous = false;
        if (g_prevValid) {
            float rn[9], d[9];
            temporalRot3Of34(w.rows, rn);
            temporalMul3(rpT, rn, d);
            continuous = temporalRotationAngleDeg(d) < 3.0f;
        }
        if (continuous) {
            ++continuousCount;
            // A write identical to last frame's chosen rows is the game's
            // own last-view block (nearly every frame in space carried one,
            // a head-turn's angle from the current; 2026-09-04): kept only
            // as the fallback, for a camera that truly stood still.
            if (g_prevValid && memcmp(w.rows, g_prevRows, sizeof(w.rows)) == 0) {
                ++twinCount;
                if (twinIdx < 0 || w.seq > twinSeq) { twinIdx = i; twinSeq = w.seq; twinBound = bound; }
                continue;
            }
            // The BOUND object's latest continuous write, else the latest
            // continuous write of any object. The bound object is the scene
            // camera's by construction (the latch fires at the frame's first
            // draw into the scene pair's depth, depth_probe.cpp), and within
            // one object the frame's last view matrix is the one the eyes
            // were drawn with (an earlier write of the same frame is a staler
            // prediction of the same head). Latest-of-any-object (6677fca)
            // took another block's write on half the frames of every
            // supercruise and arrival interval of 2026-09-04, and the rows
            // then turned a quarter to a half of the head, lagging: a stale
            // camera within three degrees, written after the scene's own.
            // Steady space flight never showed it, since the bound block's
            // write was the latest there (docs/review-temporal-far-warp-
            // darkness-2026-09-04.md, F2).
            const bool better = bestIdx < 0 || (bound && !bestBound) ||
                                (bound == bestBound && w.seq > bestSeq);
            if (better) { bestIdx = i; bestSeq = w.seq; bestBound = bound; }
            contIdx[contN++] = i;
        }
        const bool fbetter = fallIdx < 0 || (bound && !fallBound) ||
                             (bound == fallBound && w.seq > fallSeq);
        if (fbetter) { fallIdx = i; fallSeq = w.seq; fallBound = bound; }
    }
    g_rowsChoice.candidates = count;
    g_rowsChoice.boundCandidates = boundCount;
    g_rowsChoice.continuousCandidates = continuousCount;
    g_rowsChoice.twinCandidates = twinCount;
    g_candSumCount += count;
    bool twinFallback = false;
    if (twinIdx >= 0) {
        g_rowsChoice.flags |= kChoiceTwinPresent;
        g_rowsChoice.twinSeq = g_rowsRing[twinIdx].observedSeq;
        if (bestIdx >= 0) ++g_twinFrames;
        else { bestIdx = twinIdx; bestSeq = twinSeq; bestBound = twinBound; twinFallback = true; }
    }
    // The ambiguity: another continuous write whose rows differ from the
    // chosen (the same matrix written again is no ambiguity). Two cameras
    // within three degrees of each other -- the other eye on canted
    // panels, a pass with a stale view -- would alternate the choice and
    // put their difference into the delta.
    if (bestIdx >= 0) {
        float cT[9], cr[9];
        temporalRot3Of34(g_rowsRing[bestIdx].rows, cr);
        temporalTranspose3(cr, cT);
        float spread = 0.0f;
        bool multi = false;
        for (int k = 0; k < contN; ++k) {
            const int i = contIdx[k];
            if (i == bestIdx) continue;
            if (memcmp(g_rowsRing[i].rows, g_rowsRing[bestIdx].rows, sizeof(float) * 12) == 0) continue;
            float on[9], d[9];
            temporalRot3Of34(g_rowsRing[i].rows, on);
            temporalMul3(cT, on, d);
            const float a = temporalRotationAngleDeg(d);
            multi = true;
            if (a > spread) spread = a;
        }
        if (multi) {
            ++g_chooseMulti;
            g_chooseSpreadSum += spread;
            if (spread > g_chooseSpreadMax) g_chooseSpreadMax = spread;
        }
    }
    int pick = bestIdx;
    // The resync (2026-09-08, a station approach): continuity is self-
    // reinforcing. Once the chain has landed on another object's camera --
    // an auxiliary pass of the station's, written every frame and
    // continuous with itself -- the bound block's real rows are never
    // within three degrees of the chain again, so the chain never comes
    // back on its own: "another's on 1774 frames, the bound block's on 0"
    // for 58 seconds of the approach, with the head-follow score keeping
    // the world path down the whole time and the station smearing under
    // the ship's motion. That score is the detector; this is what it was
    // missing. While the rows have stopped following the head (the path
    // is already down, so a wrong pick costs nothing more) and the bound
    // block wrote this frame, take its latest write over the chain. The
    // score then decides: rows that turn with the head bring the path
    // back within a few dozen frames, and a bound block holding a
    // reflection camera fails the same test and is dropped again.
    if (g_rowsFollow < 0 && fallBound && !(pick >= 0 && bestBound)) {
        pick = fallIdx;
        g_rowsChoice.flags |= kChoiceRefollow;
        ++g_chooseRefollow;
    } else if (pick >= 0) {
        g_rowsChoice.flags |= kChoiceContinuous;
        if (twinFallback) g_rowsChoice.flags |= kChoiceTwinFallback;
        if (bestBound) ++g_chooseBound; else ++g_chooseOther;
    } else if (fallIdx >= 0) {
        pick = fallIdx;
        g_rowsChoice.flags |= kChoiceResync;
        ++g_chooseResync;
    } else {
        ++g_chooseNone;
    }
    if (pick >= 0) {
        g_rowsChoice.flags |= kChoiceValid;
        g_rowsChoice.selectedResource = g_rowsRing[pick].buf;
        g_rowsChoice.selectedSeq = g_rowsRing[pick].observedSeq;
        memcpy(g_rowsChoice.selectedRows, g_rowsRing[pick].rows, sizeof(g_rowsChoice.selectedRows));
        memcpy(g_rowsChoice.selectedProj, g_rowsRing[pick].proj, sizeof(g_rowsChoice.selectedProj));
        memcpy(g_curRows, g_rowsRing[pick].rows, sizeof(g_curRows));
        g_curProj[0] = g_rowsRing[pick].proj[0];
        g_curProj[1] = g_rowsRing[pick].proj[1];
        g_curValid = true;
        g_curRowsBound = g_boundSeen && g_rowsRing[pick].buf == g_boundBuf;
        if (g_curRowsBound) g_rowsChoice.flags |= kChoiceSelectedBound;
    }
}

void failOnce(const char* what) {
    if (g_failNoted) return;
    g_failNoted = true;
    Log::get().note("temporal aa: %s; the pass stands down.", what);
}

ID3D11ComputeShader* createShader(ID3D11DeviceContext* ctx) {
    return shaderSwapCreateCs(ctx, kTemporalAaBytecode, sizeof(kTemporalAaBytecode),
                              "temporal_aa_cs", "temporal aa");
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

bool ensureDecisionTexture(ID3D11Device* dev, EyeState& e, uint32_t w, uint32_t h) {
    if (e.dlDecision) {
        D3D11_TEXTURE2D_DESC d{}; e.dlDecision->GetDesc(&d);
        if (d.Width == w && d.Height == h && d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT &&
            e.dlDecisionUav) return true;
        if (e.dlDecisionUav) { e.dlDecisionUav->Release(); e.dlDecisionUav = nullptr; }
        e.dlDecision->Release(); e.dlDecision = nullptr;
    }
    if (makeTex(dev, w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                D3D11_BIND_UNORDERED_ACCESS, &e.dlDecision, nullptr, &e.dlDecisionUav)) return true;
    if (e.dlDecisionUav) { e.dlDecisionUav->Release(); e.dlDecisionUav = nullptr; }
    if (e.dlDecision) { e.dlDecision->Release(); e.dlDecision = nullptr; }
    return false;
}

// The mask NVIDIA is handed is R8_UNORM written from a compute shader,
// which needs typed unordered access to that format -- checked once, and
// its absence only loses NVIDIA's copy of the mask, never the own pass's.
bool g_maskFmtChecked = false;
bool g_maskFmtOk = false;
bool maskFormatOk(ID3D11Device* dev) {
    if (!g_maskFmtChecked) {
        g_maskFmtChecked = true;
        UINT support = 0;
        g_maskFmtOk = SUCCEEDED(dev->CheckFormatSupport(DXGI_FORMAT_R8_UNORM, &support)) &&
                      (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0;
        if (!g_maskFmtOk) {
            Log::get().note(
                "temporal aa: this GPU/driver reports no typed unordered access for R8_UNORM, "
                "so the mover mask cannot be handed to NVIDIA; the pass's own history still "
                "applies it.");
        }
    }
    return g_maskFmtOk;
}

// Tier 1's textures beside the depth copy (docs/per-object-motion.md,
// 2026-09-08): last frame's depth -- the depth copy's twin, swapped with it
// after every frame that wrote one, so the carry costs no copy -- and the
// mask NVIDIA is handed. Made at the depth copy's size, and the depth copy
// itself when the own path runs without a trained set (a stale set at
// another size goes with it; the trained block rebuilds its own, and its
// test sees the missing output). A failure leaves e.zPrev null, which is
// how the pass knows to keep the mask off; the mask texture is optional.
bool ensureUiHistory(ID3D11Device* dev, EyeState& e, uint32_t w, uint32_t h) {
    if (e.uiHistoryW != w || e.uiHistoryH != h) releaseUiHistory(e);
    if (e.uiHistory[0] && e.uiHistory[1]) return true;
    for (int k=0;k<2;++k) {
        if (!makeTex(dev,w,h,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,
                     D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,
                     &e.uiHistory[k],&e.uiHistorySrv[k],&e.uiHistoryUav[k])) {
            releaseUiHistory(e); return false;
        }
    }
    e.uiHistoryW=w; e.uiHistoryH=h;
    Log::get().note("temporal aa: adaptive UI evidence ready at %ux%u, %.1f MiB "
                    "per eye; UI changes are independent of fixed bias.",
                    w,h,static_cast<double>(w)*h*8.0/1048576.0);
    return true;
}
bool ensureBiasMask(ID3D11Device* dev, EyeState& e, uint32_t w, uint32_t h) {
    if (e.dlMask) return true;
    return maskFormatOk(dev) && makeTex(dev,w,h,DXGI_FORMAT_R8_UNORM,DXGI_FORMAT_R8_UNORM,
        D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,&e.dlMask,nullptr,&e.dlMaskUav);
}
bool ensureMoverPair(ID3D11Device* dev, EyeState& e, uint32_t w, uint32_t h, bool withMask=true) {
    if (!e.dlDepth || e.dlW != w || e.dlH != h) {
        releaseDl(e);
        if (!makeTex(dev, w, h, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
                     D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                     &e.dlDepth, &e.dlDepthSrv, &e.dlDepthUav)) {
            releaseDl(e);
            return false;
        }
        e.dlW = w;
        e.dlH = h;
    }
    if (!e.zPrev) {
        e.zPrevValid = false;
        if (!makeTex(dev, w, h, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
                     D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                     &e.zPrev, &e.zPrevSrv, &e.zPrevUav)) {
            if (e.zPrevUav) { e.zPrevUav->Release(); e.zPrevUav = nullptr; }
            if (e.zPrevSrv) { e.zPrevSrv->Release(); e.zPrevSrv = nullptr; }
            if (e.zPrev) { e.zPrev->Release(); e.zPrev = nullptr; }
            return false;
        }
        Log::get().note("temporal aa: per-eye depth history ready at %ux%u (%.1f MiB); "
                        "available for DLSS background occlusion rejection. "
                        "Depth textures swap without a frame copy.",w,h,double(w)*h*4/1048576.0);
    }
    if (withMask && !e.dlMask && maskFormatOk(dev)) {
        if (!makeTex(dev, w, h, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM,
                     D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                     &e.dlMask, nullptr, &e.dlMaskUav)) {
            if (e.dlMaskUav) { e.dlMaskUav->Release(); e.dlMaskUav = nullptr; }
            if (e.dlMask) { e.dlMask->Release(); e.dlMask = nullptr; }
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

// The history's format: ten bits per channel is enough for an accumulation
// to converge (the 8-bit output stalls within a level of its target; ten
// bits stalls within a quarter of one) at half the memory of float16, and
// this pass holds two of them per eye at render size. Float16 when the
// device cannot store to it.
DXGI_FORMAT pickHistoryFormat(ID3D11Device* dev) {
    UINT support = 0;
    if (SUCCEEDED(dev->CheckFormatSupport(DXGI_FORMAT_R10G10B10A2_UNORM, &support)) &&
        (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    support = 0;
    if (SUCCEEDED(dev->CheckFormatSupport(DXGI_FORMAT_R16G16B16A16_FLOAT, &support)) &&
        (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

bool     g_depthNoted = false;
bool     g_depthHeld = false;      // the last treat had the depth in hand
uint32_t g_depthLostCount = 0;

// Only native TAA (including a refused NVIDIA evaluation) needs this storage.
bool ensureNative(ID3D11Device* dev, EyeState& e, DXGI_FORMAT viewFmt) {
    if (e.outTex && e.hist[0] && e.hist[1]) return true;
    releaseNative(e);
    bool made = makeTex(dev, e.w, e.h, e.outFmt, viewFmt,
                        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                        &e.outTex, &e.outSrv, &e.outUav);
    for (int i = 0; i < 2 && made; ++i) {
        made = makeTex(dev, e.w, e.h, e.histFmt, e.histFmt,
                       D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                       &e.hist[i], &e.histSrv[i], &e.histUav[i]);
    }
    if (!made) { releaseNative(e); failOnce("the native history or output textures could not be created"); }
    return made;
}

// DLSS where you look (docs/performance.md feature 6), the crop's geometry:
// pure, no D3D11, no globals -- cropOf below (temporalInner) is its only
// caller in the pass, and tools\smoke's edvrFoveaRegionSelftest exercises
// it directly with no device.
//
// An edge's own half-angle in the RENDERED frame is atan(|tangent|); the
// region's half-angle at that edge is the frame's own angle there minus
// reducedTrimDeg -- the caller has already reduced the fovea's edge trim by
// however much the FOV trim (fix.fov_trim_vertical/_outer/_nasal) already
// cut that same edge -- floored at 2 degrees so a trim cannot close or
// invert an edge. A trim that reaches (or passes) the frame's own angle
// leaves the region sitting AT that edge: no periphery strip there.
float foveaEdgeRegionDeg(float edgeTan, float reducedTrimDeg) {
    if (!(reducedTrimDeg > 0.0f)) reducedTrimDeg = 0.0f;
    constexpr float kRadToDeg = 57.295779513082321f;
    const float edgeDeg = atanf(fabsf(edgeTan)) * kRadToDeg;
    float regionDeg = edgeDeg - reducedTrimDeg;
    if (regionDeg < 2.0f) regionDeg = 2.0f;
    return regionDeg;
}

// Either the width mode's inputs (today's crop: a width in degrees, and the
// fixation-distance centre offset in tangent units) or the edges mode's
// (the three edge trims, ALREADY reduced by the FOV trim -- see
// foveaEdgeRegionDeg above). temporal_aa_fovea_distance (tcx/tcy) is not
// read in edges mode: the rectangle there is placed by its own edges.
struct FoveaRegionMode {
    bool  edges = false;
    float widthDeg = 0.0f;
    float tcx = 0.0f, tcy = 0.0f;
    float topTrimDeg = 0.0f;
    float bottomTrimDeg = 0.0f;
    float outerTrimDeg = 0.0f;
    float nasalTrimDeg = 0.0f;
};

struct FoveaRegion {
    uint32_t x = 0, y = 0, w = 0, h = 0;
    bool     ok = false;   // false: under minPx, or the mode is off -- run full-frame
};

// Frusta order is native_temporal.h:21's own {left,right,down,up}
// (tanNow[0..3]): down is negative, up positive, and row 0 of the
// rendered frame sits at the up edge (temporalInner's own "py" projection).
//
// l,r,down,up: this eye's four frame tangents. fw,fh: the frame those
// tangents describe, in pixels. eyeIndex: 0 the left eye (its outer edge
// is the left/l tangent, its nasal edge the right/r), 1 the right eye (the
// reverse) -- confirmed by foveation.cpp's own mirrored tangent build (*l
// = eye==0 ? -outer : -inner, *r = eye==0 ? inner : outer) and
// frame_flag.h's "0 left, 1 right". minPx: the smallest crop worth
// NVIDIA's seam (128 today, cropOf's own floor); under it, ok is false and
// the caller runs full-frame -- same as an unreachable r<=l or up<=down
// frame, and the same as the mode being off (widthDeg <= 0 and not
// edges).
//
// Width mode's arithmetic (cx, cy, halfa, hwp, hhp and the four ints) is
// copied verbatim from cropOf as it stood before this extraction, in the
// same order, so its output does not move by a bit; edges mode is new.
// Either way the four ints then get the SAME clamp, even-round and minPx
// floor cropOf always applied -- the caller's own 90%-of-the-frame check
// (a different rule, against the OUTPUT crop after scaling) is unchanged
// and still runs on whichever mode produced fcw/fch.
FoveaRegion computeFoveaRegion(float l, float r, float down, float up,
                               uint32_t fw, uint32_t fh, int eyeIndex,
                               const FoveaRegionMode& mode, uint32_t minPx) {
    FoveaRegion out;
    if (!(r > l) || !(up > down) || fw == 0 || fh == 0) return out;
    const float fwf = static_cast<float>(fw), fhf = static_cast<float>(fh);
    int x0, y0, x1, y1;
    if (!mode.edges) {
        if (!(mode.widthDeg > 0.0f)) return out;
        const float cx = ((mode.tcx - l) / (r - l)) * fwf;
        const float cy = ((up - mode.tcy) / (up - down)) * fhf;
        const float halfa = tanf(mode.widthDeg * 0.5f * 0.01745329252f);
        const float hwp = halfa * fwf / (r - l);
        const float hhp = halfa * fhf / (up - down);
        x0 = static_cast<int>(cx - hwp);
        y0 = static_cast<int>(cy - hhp);
        x1 = static_cast<int>(cx + hwp + 0.5f);
        y1 = static_cast<int>(cy + hhp + 0.5f);
    } else {
        const float leftTrim = (eyeIndex == 0) ? mode.outerTrimDeg : mode.nasalTrimDeg;
        const float rightTrim = (eyeIndex == 0) ? mode.nasalTrimDeg : mode.outerTrimDeg;
        constexpr float kDegToRad = 0.01745329252f;
        const float lo = -tanf(foveaEdgeRegionDeg(l, leftTrim) * kDegToRad);
        const float ro = tanf(foveaEdgeRegionDeg(r, rightTrim) * kDegToRad);
        // down bounds y1 (the bottom row) -> bottomTrimDeg; up bounds y0
        // (row 0, the top row) -> topTrimDeg. See the frusta-order comment
        // above the function.
        const float to = -tanf(foveaEdgeRegionDeg(down, mode.bottomTrimDeg) * kDegToRad);
        const float bo = tanf(foveaEdgeRegionDeg(up, mode.topTrimDeg) * kDegToRad);
        if (!(ro > lo) || !(bo > to)) return out;
        x0 = static_cast<int>(((lo - l) / (r - l)) * fwf);
        y0 = static_cast<int>(((up - bo) / (up - down)) * fhf);
        x1 = static_cast<int>(((ro - l) / (r - l)) * fwf + 0.5f);
        y1 = static_cast<int>(((up - to) / (up - down)) * fhf + 0.5f);
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > static_cast<int>(fw)) x1 = static_cast<int>(fw);
    if (y1 > static_cast<int>(fh)) y1 = static_cast<int>(fh);
    x0 &= ~1; y0 &= ~1; x1 &= ~1; y1 &= ~1;
    const int cw = x1 - x0, ch = y1 - y0;
    if (cw < static_cast<int>(minPx) || ch < static_cast<int>(minPx)) return out;
    out.x = static_cast<uint32_t>(x0);
    out.y = static_cast<uint32_t>(y0);
    out.w = static_cast<uint32_t>(cw);
    out.h = static_cast<uint32_t>(ch);
    out.ok = true;
    return out;
}

// THE HEAD LEAD's geometry (advanced.temporal_aa_fovea_lead, the state and
// the "why" are up at g_foveaLead): three pure functions, no D3D11 and no
// globals, exercised by edvrFoveaRegionSelftest's bit 32.
//
// A far point's motion at the frame's CENTRE, in render pixels, in exactly
// the convention the motion-vector shader writes and NGX reads
// (InMVScaleX/Y = 1, so previous = current + mv). This is the mv entry of
// temporal_shader_source.h transcribed: its direction build (d.x/d.y/d.z
// from tanNow, "d.z = -1.0"), its rotation of that direction into last
// frame's view (dp = the chosen rows times d, taken with NO depth -- the far
// plane's rotation-only case), and its projection through tanPrev to a
// previous pixel, then motion = pp - p. r0/r1/r2 are whichever rotation rows
// a far pixel at the centre would take this frame -- the head's delta
// (dR0..dR2) or the camera's (c2R0..c2R2, the head and the ship together),
// the caller mirrors the shader's own choice -- each being the rotation
// taking THIS frame's view directions to last frame's.
//
// Sign, which the whole feature hangs off: turning right moves world content
// left on screen, so the centre's content WAS to the right last frame,
// pp.x > p.x and mv.x > 0. Row 0 of the frame sits at the UP tangent (the
// frusta-order comment above computeFoveaRegion), so pitching up moves
// content DOWN the rows and mv.y < 0.
bool foveaCentreMotion(const float tanNow[4], const float tanPrev[4], const float r0[3],
                       const float r1[3], const float r2[3], uint32_t fw, uint32_t fh,
                       float* mvX, float* mvY) {
    if (mvX) *mvX = 0.0f;
    if (mvY) *mvY = 0.0f;
    if (!tanNow || !tanPrev || !r0 || !r1 || !r2 || fw == 0 || fh == 0) return false;
    if (!(tanNow[1] > tanNow[0]) || !(tanNow[3] > tanNow[2])) return false;
    if (!(tanPrev[1] > tanPrev[0]) || !(tanPrev[3] > tanPrev[2])) return false;
    // The frame's exact centre: the pixel index whose sample point (p + 0.5)
    // lands at half the frame, so d is the mean of the two tangents each way.
    const float px = 0.5f * static_cast<float>(fw) - 0.5f;
    const float py = 0.5f * static_cast<float>(fh) - 0.5f;
    const float d[3] = {
        tanNow[0] + ((px + 0.5f) / static_cast<float>(fw)) * (tanNow[1] - tanNow[0]),
        tanNow[3] - ((py + 0.5f) / static_cast<float>(fh)) * (tanNow[3] - tanNow[2]),
        -1.0f};
    const float dp[3] = {r0[0] * d[0] + r0[1] * d[1] + r0[2] * d[2],
                         r1[0] * d[0] + r1[1] * d[1] + r1[2] * d[2],
                         r2[0] * d[0] + r2[1] * d[1] + r2[2] * d[2]};
    if (dp[2] >= -1e-6f) return false;   // behind the eye: the shader's own guard
    const float xt = dp[0] / -dp[2];
    const float yt = dp[1] / -dp[2];
    const float ppx = (xt - tanPrev[0]) / (tanPrev[1] - tanPrev[0]) * static_cast<float>(fw) - 0.5f;
    const float ppy = (tanPrev[3] - yt) / (tanPrev[3] - tanPrev[2]) * static_cast<float>(fh) - 0.5f;
    if (!std::isfinite(ppx) || !std::isfinite(ppy)) return false;
    if (mvX) *mvX = ppx - px;
    if (mvY) *mvY = ppy - py;
    return true;
}

// The lead in render pixels: that many frames of the centre's motion.
struct FoveaLead {
    float x = 0.0f, y = 0.0f;
};
FoveaLead foveaLeadPixels(float mvCentreX, float mvCentreY, float frames) {
    FoveaLead out;
    if (!std::isfinite(mvCentreX) || !std::isfinite(mvCentreY) || !std::isfinite(frames) ||
        frames <= 0.0f) {
        return out;
    }
    out.x = mvCentreX * frames;
    out.y = mvCentreY * frames;
    return out;
}

// A pixel distance as an angle, through this eye's own tangent span.
float foveaLeadDegrees(float px, float l, float r, uint32_t fw) {
    if (fw == 0 || !(r > l) || !std::isfinite(px)) return 0.0f;
    constexpr float kRadToDeg = 57.295779513082321f;
    return atanf(fabsf(px) * (r - l) / static_cast<float>(fw)) * kRadToDeg;
}

// The crop's base with the lead applied. The EXTENTS never move: NGX
// re-creates the crop feature when the crop's SIZE changes (tens of ms), so
// the lead is an integer offset to the base alone, taken after
// computeFoveaRegion -- never by shifting the edge tangents into it, where
// rounding would change w/h by a pixel between frames. Even pixels (NGX
// prefers them, and every base here is even today), and clamped so the
// rectangle stays wholly inside the frame: at the edge the applied offset is
// whatever fitted, and clamped says so.
struct FoveaLeadBase {
    uint32_t x = 0, y = 0;         // the base after the lead
    int32_t  dx = 0, dy = 0;       // what was actually applied, render pixels, even
    bool     clamped = false;      // the frame's edge held the lead short
};
FoveaLeadBase foveaLeadBase(const FoveaRegion& g, float leadX, float leadY, uint32_t fw,
                            uint32_t fh) {
    FoveaLeadBase out;
    out.x = g.x;
    out.y = g.y;
    if (!g.ok || g.w > fw || g.h > fh) return out;
    auto axis = [](uint32_t base, uint32_t extent, uint32_t frame, float lead, int32_t* applied,
                   bool* clamped) -> uint32_t {
        if (!std::isfinite(lead)) lead = 0.0f;
        const float half = lead * 0.5f;
        const int32_t steps = static_cast<int32_t>(
            half >= 0.0f ? floorf(half + 0.5f) : -floorf(-half + 0.5f));
        const int32_t off = steps * 2;                       // even pixels
        const int32_t room = static_cast<int32_t>(frame - extent) & ~1;
        int32_t want = static_cast<int32_t>(base) + off;
        if (want < 0) want = 0;
        if (want > room) want = room;
        if (want < 0) want = 0;                              // a frame smaller than the crop
        if (want != static_cast<int32_t>(base) + off) *clamped = true;
        *applied = want - static_cast<int32_t>(base);
        return static_cast<uint32_t>(want);
    };
    out.x = axis(g.x, g.w, fw, leadX, &out.dx, &out.clamped);
    out.y = axis(g.y, g.h, fh, leadY, &out.dy, &out.clamped);
    return out;
}

void* temporalInner(void* srcTex, int eye, const float* bounds,
                    const float* tanNow, const float* tanPrev, float jxNow,
                    float jyNow, const float* deltaHead, const float* headTrans,
                    const float* headTransSwapped, float nearZ, float farZ,
                    float headDeg, int motion, float blend, float clampSigma,
                    unsigned outW, unsigned outH, unsigned flags) {
    lumaProbeBegin(eye);
    TemporalHistoryScope history(eye,flags,jxNow,jyNow);
    auto& trace=history.entry;
    ID3D11Texture2D* src = nullptr;
    static_cast<IUnknown*>(srcTex)->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src));
    if (!src) return nullptr;

    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);

    bool ok = true;
    if (sd.SampleDesc.Count > 1 || sd.ArraySize != 1 || sd.MipLevels != 1) {
        ok = false;
        if (!g_kindNoted) {
            g_kindNoted = true;
            Log::get().note(
                "temporal aa: the submitted texture is %ux%u samples=%u "
                "array=%u mips=%u, a kind the pass does not handle. The "
                "pass stands down.",
                sd.Width, sd.Height, sd.SampleDesc.Count, sd.ArraySize,
                sd.MipLevels);
        }
    }

    uint32_t region[4] = {};
    if (ok && !supersampleRegionFromBounds(sd.Width, sd.Height, bounds,
                                           region, nullptr, nullptr)) {
        ok = false;
        if (!g_regionNoted) {
            g_regionNoted = true;
            Log::get().note(
                "temporal aa: the Submit bounds name no usable eye region "
                "of a %ux%u texture. The pass stands down.",
                sd.Width, sd.Height);
        }
    }
    const uint32_t w = ok ? region[2] - region[0] : 0;
    const uint32_t h = ok ? region[3] - region[1] : 0;
    trace.width=w;trace.height=h;

    int fmtIndex = -1;
    DXGI_FORMAT viewFmt = DXGI_FORMAT_UNKNOWN;
    if (ok) {
        viewFmt = viewFormatOf(sd.Format, &fmtIndex);
        if (fmtIndex < 0) {
            ok = false;
            if (!g_fmtUnknownNoted) {
                g_fmtUnknownNoted = true;
                Log::get().note(
                    "temporal aa: the submitted texture's format is %s "
                    "(DXGI_FORMAT %d), one this pass does not handle -- "
                    "unmeasured formats are refused, not assumed. The pass "
                    "stands down; please report this log.",
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
        if (ok && !acceptPassDevice(dev)) {
            // Leave before the capture-completion path too: its staging
            // textures are also owned by the original device.
            ctx->Release();
            dev->Release();
            src->Release();
            return nullptr;
        }
    }
    if (ok) pollSlots(ctx);

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
                "temporal aa: this GPU/driver reports no typed unordered-"
                "access support for %s, so the result cannot be written "
                "here. The pass stands down.",
                formatName(viewFmt));
        }
    }
    if (ok && !g_histChecked) {
        g_histChecked = true;
        g_histFmt = pickHistoryFormat(dev);
        if (g_histFmt == DXGI_FORMAT_UNKNOWN) {
            failOnce("neither R10G10B10A2_UNORM nor R16G16B16A16_FLOAT can be "
                     "stored to on this GPU/driver, and the history needs one");
        }
    }
    ok = ok && g_histFmt != DXGI_FORMAT_UNKNOWN;

    if (ok && !g_cs && !g_csTried) {
        g_csTried = true;
        g_cs = createShader(ctx);
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
    if (ok && !g_foveaCb) {
        // Four float4s: the crop rectangle, the blend band and the sizes,
        // the disc, the mode flags. Created here beside g_cb so the fovea
        // path never allocates on the render thread; only filled when the
        // fovea is actually on.
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 64;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (!SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_foveaCb))) {
            // Not fatal: the fovea path checks g_foveaCb and stands down to
            // full-frame DLAA if it is null. The main pass runs regardless.
            g_foveaCb = nullptr;
        }
    }
    if (ok && !g_uiResolveTolCb) {
        // One float4: the UI-resolve clamp's bound tolerance (b1), so an
        // ordinary DLSS reconstruction offset does not trip it (issue 36).
        // Created here beside g_cb. Not fatal if it fails: the dispatch
        // site leaves b1 unbound, which the shader reads as zero -- the
        // old exact clamp -- so the resolve pass still runs correctly.
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (!SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_uiResolveTolCb))) {
            g_uiResolveTolCb = nullptr;
        }
    }
    if (ok && !g_downCb) {
        // The steady periphery's reduction: two float4s of sizes. Not fatal
        // either: without it the reduction cannot run and the sharp
        // periphery stands in (the fovea path checks).
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 32;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (!SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_downCb))) g_downCb = nullptr;
    }
    if (ok && !g_samp) {
        D3D11_SAMPLER_DESC smd{};
        smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        smd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        smd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smd.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(dev->CreateSamplerState(&smd, &g_samp));
        if (!ok) failOnce("the sampler could not be created");
    }
    if (ok && !g_stats) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kStatCount * 4;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kStatCount;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_stats)) &&
             SUCCEEDED(dev->CreateUnorderedAccessView(g_stats, &ud, &g_statsUav));
        if (!ok) failOnce("the statistics buffer could not be created");
    }

    EyeState* eptr = ok ? &g_eye[eye] : nullptr;

    // The input view: over the source when it allows one, else the region
    // copied out (the theater's copy-through, the resolve's too).
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
            if (!e.copyTex || e.copyW != w || e.copyH != h || e.copyFmt != sd.Format) {
                releaseCopy(e);
                if (makeTex(dev, w, h, sd.Format, viewFmt, D3D11_BIND_SHADER_RESOURCE,
                            &e.copyTex, &e.copySrv, nullptr)) {
                    e.copyW = w;
                    e.copyH = h;
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

    // The scene's depth for this eye, when the probe has settled on it and
    // the planes are known: a view typed to the depth channel over the
    // game's own texture, held by the probe. Wanted by the depth motion
    // and by the instrument's two depth candidates alike.
    ID3D11ShaderResourceView* depthSrv = nullptr;
    if (ok && nearZ > 0.0f && farZ > nearZ) {
        g_lastNear = nearZ;
        g_lastFar = farZ;
        EyeState& e = *eptr;
        ID3D11Texture2D* dtex = nullptr;
        if (depthProbeSceneDepth(sd.Width, sd.Height, eye, &dtex) && dtex) {
            if (e.depthRes != static_cast<void*>(dtex) || !e.depthSrv) {
                releaseDepth(e);
                D3D11_TEXTURE2D_DESC dd{};
                dtex->GetDesc(&dd);
                DXGI_FORMAT rf = DXGI_FORMAT_UNKNOWN;
                switch (dd.Format) {
                    case DXGI_FORMAT_R32G8X24_TYPELESS:
                    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
                        rf = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
                    case DXGI_FORMAT_R32_TYPELESS:
                    case DXGI_FORMAT_D32_FLOAT:
                        rf = DXGI_FORMAT_R32_FLOAT; break;
                    case DXGI_FORMAT_R24G8_TYPELESS:
                    case DXGI_FORMAT_D24_UNORM_S8_UINT:
                        rf = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
                    default: break;
                }
                if (rf != DXGI_FORMAT_UNKNOWN && (dd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                    vd.Format = rf;
                    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    vd.Texture2D.MipLevels = 1;
                    if (SUCCEEDED(dev->CreateShaderResourceView(dtex, &vd, &e.depthSrv)) &&
                        e.depthSrv) {
                        e.depthRes = dtex;
                    } else {
                        e.depthSrv = nullptr;
                    }
                }
            }
            depthSrv = e.depthSrv;
        }
    }
    if (depthSrv && (!g_depthNoted || !g_depthHeld)) {
        g_depthNoted = true;
        Log::get().note(
            "temporal aa: the scene's depth is in hand -- the depth probe's "
            "%ux%u target for this eye, read through a depth-channel view, "
            "reversed-Z with the game's planes %.3f..%.0f m. The depth motion "
            "reprojects every pixel with the head's translation from here; "
            "the registration line's 'head with depth' and 'depth, eyes "
            "swapped' candidates say whether the eyes are assigned right.",
            sd.Width, sd.Height, static_cast<double>(nearZ), static_cast<double>(farZ));
    } else if (!depthSrv && g_depthHeld && eye == 0) {
        ++g_depthLostCount;
        if (g_depthLostCount <= 3) {
            Log::get().note(
                "temporal aa: the scene's depth went away (the render size "
                "changed, or the probe has not settled on the new targets "
                "yet) -- the pass runs on the head's rotation alone until it "
                "is found again, and says so when it is.");
        }
    }
    if (eye == 0) g_depthHeld = depthSrv != nullptr;
    // The drives' smoke's own depth for this eye, folded into the scene's
    // by zSceneAt (t6): null when the trail drew nothing this frame, or
    // the target is not the scene depth's size.
    ID3D11ShaderResourceView* smokeSrv = nullptr;
    if (depthSrv && !uiDepthSmokeDepth(sd.Width, sd.Height, eye, &smokeSrv)) smokeSrv = nullptr;

    ID3D11ShaderResourceView* uiDepthSrv = nullptr;
    ID3D11ShaderResourceView* terrainSrvs[3] = {};
    ID3D11ShaderResourceView* holoSrvs[2] = {};
    // Engine-record velocity's inputs for this eye (engine_velocity.h): MRT6,
    // the pool snapshot and the scene constants now/before; all four or none.
    EngineVelocityViews engineViews{};
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> engineHeldSrv[2];
    Microsoft::WRL::ComPtr<ID3D11Buffer> engineHeldCb[2];
    // Named apart from the trained block's `engineAvailable` (the upscaler's
    // availability, declared in an inner scope): the flight of 2026-09-23
    // bound these inputs, unbound, on every DLSS dispatch because that inner
    // name shadowed this one.
    bool engineViewsGiven = false;
    ID3D11ShaderResourceView* screenSrv=screenMotionView(eye,sd.Width,sd.Height);
    if (depthSrv) {
        ID3D11Resource* res = nullptr;
        depthSrv->GetResource(&res);
        ID3D11Texture2D* scene = nullptr;
        if (res) {
            res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&scene));
            res->Release();
        }
        if (scene) {
            // The hologram/icon depth resolve (ui_depth.h) writes into the
            // private scene-depth copy, so it runs before
            // uiDepthTemporalDepth hands that copy to the pass. inSrv is
            // this eye's finished, tonemapped colour, for the resolve's
            // FLOOR test only -- its share test reads the game's own HDR
            // render target back separately (never this).
            gpuCensusBegin(ctx, GpuCensusSection::DoorHologramResolve);
            uiDepthHologramResolve(ctx, eye, scene, sd.Width, sd.Height, inSrv);
            uiDepthTemporalDepth(sd.Width, sd.Height, eye, scene, &uiDepthSrv);
            celestialMotionViews(ctx, scene, terrainSrvs);
            gpuCensusEnd(ctx, GpuCensusSection::DoorHologramResolve);
            uiDepthHoloMotion(eye,scene,holoSrvs);
            engineViewsGiven = engineVelocityViews(ctx, eye, scene, &engineViews);
            engineHeldSrv[0].Attach(engineViews.slots);
            engineHeldSrv[1].Attach(engineViews.pool);
            engineHeldCb[0].Attach(engineViews.sceneNow);
            engineHeldCb[1].Attach(engineViews.scenePrev);
            scene->Release();
        }
    }

    // Input identity is independent of native fallback resource allocation.
    if (ok) {
        EyeState& e = *eptr;
        if (e.w != w || e.h != h || e.outFmt != sd.Format || e.histFmt != g_histFmt) {
            trace.events |= 2u;
            releaseOwned(e);
            e.w = w; e.h = h; e.outFmt = sd.Format; e.histFmt = g_histFmt;
        }
    }

    void* result = nullptr;
    bool uiEvidenceWritten = false;
    bool uiResolveWritten = false;
    if (ok) {
        EyeState& e = *eptr;
        if(e.screenHistory!=(screenSrv!=nullptr)) {
            trace.events |= 4u;
            e.haveHistory=e.dlHaveHistory=e.zPrevValid=false;
            e.screenHistory=screenSrv!=nullptr;
        }
        if (flags & 1u) {
            trace.events |= 1u;
            e.haveHistory = false;
            e.dlHaveHistory = false;
            // ...and the depth carry: a withheld frame broke the pose
            // stream's continuity, so last frame's depth is not the frame
            // the delta describes. The mask waits one frame.
            e.zPrevValid = false;
        }

        // This frame's camera rows, chosen from the frame's writes (once).
        chooseCameraRows();
        // The rotation delta for this frame's motion source. The depth
        // motion is the head's rotation with its translation term, and
        // falls back to the rotation alone until the depth is in hand.
        float delta[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        bool haveDelta = motion == 0;
        const bool depthMotion = motion == 3;
        if ((motion == 1 || depthMotion) && deltaHead) {
            memcpy(delta, deltaHead, sizeof(delta));
            haveDelta = true;
        }
        // No delta means no reprojection can be trusted: this frame goes
        // out unblended and the history restarts from it.
        const bool useHistory = e.haveHistory && haveDelta && tanPrev;

        // The registration instrument's candidates, whichever of them
        // exist this frame (temporalPassRegistration): 0 the head's
        // rotation alone, 2 the world path's delta from the rows, 3 the
        // head with depth as used, 1 the same as 3 but reprojected with
        // the OTHER eye's translation.
        //
        // Slot 1 held the rows' other reading until 2026-09-04, when the
        // z flip settled that (docs/anti-aliasing.md). It was then rebuilt
        // for the eyes-swapped question -- the constant buffer carries
        // tvCand, the caller computes tvSwapped and passes it, the shader
        // reads it -- but candValid[1] was never set, so the candidate has
        // never once run, while depth_probe.h and this pass's own runtime
        // line have gone on telling the reader it answers whether the eyes
        // are assigned right. It was armed for a flight on 2026-09-07 and
        // could not have reported. Now it can.
        float cand[4][9];
        bool candValid[4] = {};
        const bool haveDepth = depthSrv != nullptr && headTrans != nullptr;
        if (deltaHead) {
            memcpy(cand[0], deltaHead, sizeof(cand[0]));
            candValid[0] = true;
            if (haveDepth) {
                memcpy(cand[3], deltaHead, sizeof(cand[3]));
                candValid[3] = true;
                // Only when the other eye's translation is genuinely in
                // hand: tvCand falls back to this eye's, which would make
                // candidate 1 a copy of 3 and its verdict meaningless.
                if (headTransSwapped) {
                    memcpy(cand[1], deltaHead, sizeof(cand[1]));
                    candValid[1] = true;
                }
            }
        }
        candValid[2] = g_curValid && g_prevValid;
        // The world path's delta and translation term, from the game's view
        // rows alone. The rows are the FULL view -- the headset's pose is in
        // them -- stored view->world. The motion view of 2026-09-04 showed
        // the world standing still under a head turn while the rows were
        // read world->view and composed with the head (the head cancelled),
        // and moving twice the head's turn under the other reading with the
        // same composition (the head doubled); a still ship's delta is the
        // identity, so the docked figure could not tell, and the earlier
        // 'ship camera without the head' reading of a 0.1 to 0.26 deg/frame
        // docked residual was twice a slow head's rate, not the rate. So:
        // no composition. For rows [R | c] (view->world, c the eye's place
        // in the world) a point P now was, last frame, at
        //   W P + tv,   W = R_p^T R_n,   tv = R_p^T (c_n - c_p).
        // (The other reading, world->view, was an A/B key and the
        // instrument's candidate 1 until 2026-09-04: it stood the world
        // still under a head turn, and is gone.) Docked, W must equal the
        // head's delta whichever way the head turns -- a real check, since
        // the rows carry the head.
        float tvCam[3] = {0.0f, 0.0f, 0.0f};
        float worldDelta[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        bool worldValid = false;
        const uint32_t sceneDraws = depthProbeSceneDraws();
        if (candValid[2]) {
            // The delta and its z flip: temporalWorldFromRows (temporal_math.h).
            float camMove[3];
            temporalWorldFromRows(g_prevRows, g_curRows, worldDelta, tvCam, camMove);
            memcpy(cand[2], worldDelta, sizeof(worldDelta));
            worldValid = true;
            const double move = sqrt(static_cast<double>(camMove[0]) * camMove[0] +
                                     static_cast<double>(camMove[1]) * camMove[1] +
                                     static_cast<double>(camMove[2]) * camMove[2]);
            g_camMoveSum += move;
            float diffDeg = 0.0f;
            if (deltaHead) {
                float ht[9], diff[9];
                temporalTranspose3(deltaHead, ht);
                temporalMul3(worldDelta, ht, diff);
                diffDeg = temporalRotationAngleDeg(diff);
                g_camHeadDiffSum += diffDeg;
                // The residual against the head's turn, on the frames the
                // delta is accepted: its size by head speed (a still head
                // with a residual is noise between the two pose streams; one
                // that grows with the speed is a scale or a lag), and the
                // regressions that name the scale and the lead. The far
                // plane is on this delta alone, and the sky probes read a
                // steady fifth of a pixel off it in space (2026-09-04).
                if (diffDeg <= 3.0f) {
                    float rv[3], hv[3];
                    temporalSmallRotVecDeg(diff, rv);
                    temporalSmallRotVecDeg(deltaHead, hv);
                    const int b = headDeg < kStillDeg ? 0 : (headDeg < kSlowDeg ? 1 : 2);
                    g_rhN[b] += 1.0;
                    g_rhSum[b] += diffDeg;
                    for (int i = 0; i < 3; ++i) {
                        g_rhDot += static_cast<double>(rv[i]) * hv[i];
                        g_rhMm += static_cast<double>(hv[i]) * hv[i];
                        g_rhDotAx[i] += static_cast<double>(rv[i]) * hv[i];
                        g_rhMmAx[i] += static_cast<double>(hv[i]) * hv[i];
                    }
                    if (g_omegaPrevValid) {
                        for (int i = 0; i < 3; ++i) {
                            const double dw = static_cast<double>(hv[i]) - g_omegaPrev[i];
                            g_rhDotLag += rv[i] * dw;
                            g_rhMmLag += dw * dw;
                        }
                    }
                    memcpy(g_omegaPrev, hv, sizeof(g_omegaPrev));
                    g_omegaPrevValid = true;
                }
            }
            if (headTrans && diffDeg <= 3.0f && move < 0.02) {
                for (int i = 0; i < 3; ++i) {
                    g_tvDot[i] += static_cast<double>(tvCam[i]) * headTrans[i];
                    g_tvMm[i] += static_cast<double>(headTrans[i]) * headTrans[i];
                }
                ++g_tvFrames;
            }
            ++g_camFrames;
            // The 15:20 yaw capture supplies correct bound camera rows even
            // while ship and head turns cancel. The old magnitude test shut
            // the world/body paths off for six captured frames (5.6 px sky
            // error), and a still head could leave that score negative.
            // Menus remain excluded by sceneDraws; ambiguous camera chains
            // retain the detector used by chooseCameraRows to resynchronize.
            g_rowsFollow = temporalCameraFollowScore(g_rowsFollow, sceneDraws, g_curRowsBound,
                                                     headDeg, temporalRotationAngleDeg(worldDelta));
            if (g_rowsFollow < 0 && !g_rowsFollowNoted) {
                g_rowsFollowNoted = true;
                Log::get().note("temporal aa: auxiliary camera rows do not follow the head; "
                                "the world path waits for the scene camera.");
            } else if (g_rowsFollow >= 0 && g_rowsFollowNoted) {
                g_rowsFollowNoted = false;
                Log::get().note("temporal aa: scene camera accepted -- the world path is back.");
            }
            // Plausibility, per frame (temporalCameraGateStep): a delta over
            // 3 degrees from the head's is another camera's and the last
            // accepted one is carried in its place; a jump over 50 m drops
            // only the translation, before the last-good store (F3,
            // 2026-09-04); and a delta that does not turn at all, measured
            // from rows a drop left behind, is a parked camera's and is
            // carried over too -- eye run 050423 took one as the view's and
            // carried its zero into the next frame (2026-09-25). A jump frame
            // keeps the last plausible translation as its last-good, and a
            // carried figure over 50 m is counted so the invariant has a
            // witness on the line.
            const TemporalCameraStep step = temporalCameraGateStep(
                g_cameraGate, g_prevRows, g_curRows, move, diffDeg, worldDelta, tvCam);
            if (step.jump) {
                ++g_camDropMove;
                // The jump's frame, for the eye run's trace and the
                // submission history (each eye runs this on the same frame).
                g_originJumpFrame = g_rowsFrame;
            }
            if (step.verdict == TemporalCameraVerdict::Another) ++g_camDropRot;
            if (step.verdict == TemporalCameraVerdict::Parked) ++g_camDropParked;
            if (step.carried) {
                memcpy(cand[2], worldDelta, sizeof(worldDelta));
                ++g_camCarried;
                if (step.carriedJump) ++g_camCarriedJump;
            }
            if (!step.valid) {
                candValid[2] = false;
                worldValid = false;
            }
            // Whether this frame's rows are the view's own: the world path
            // did not carry last frame's delta in their place.
            g_rowsDeltaOwn = step.verdict == TemporalCameraVerdict::Own;
        }

        PassParams p{};
        if (viaCopy) {
            p.region[0] = 0;
            p.region[1] = 0;
            p.region[2] = static_cast<int32_t>(w);
            p.region[3] = static_cast<int32_t>(h);
            p.texSize[0] = static_cast<int32_t>(w);
            p.texSize[1] = static_cast<int32_t>(h);
        } else {
            for (int i = 0; i < 4; ++i) p.region[i] = static_cast<int32_t>(region[i]);
            p.texSize[0] = static_cast<int32_t>(sd.Width);
            p.texSize[1] = static_cast<int32_t>(sd.Height);
        }
        p.size[0] = static_cast<int32_t>(w);
        p.size[1] = static_cast<int32_t>(h);
        memcpy(p.tanNow, tanNow, sizeof(p.tanNow));
        // The trained path's continuity is NVIDIA's, not the pass's: its
        // motion vectors need last frame's frustum whenever that history
        // continues. Keyed on haveHistory, which the trained path never
        // set, the vectors described the wrong previous frustum across a
        // guard re-stage or a resolution change (the review's F7).
        const bool trainedWanted = (flags & 2u) != 0;
        // ...and the fovea's crop history is NVIDIA's too, on a frame where the
        // own periphery history may be invalid but the crop's is not (the
        // review of 2026-09-05, F4): its motion vectors want last frustum too.
        const bool useTanPrev =
            useHistory ||
            (trainedWanted && (e.dlHaveHistory || (foveaConfigured() && (e.foveaHaveHistory || e.prHaveHistory))) &&
             haveDelta && tanPrev);
        memcpy(p.tanPrev, useTanPrev ? tanPrev : tanNow, sizeof(p.tanPrev));
        p.jit[0] = jxNow;
        p.jit[1] = jyNow;
        p.holoJitter[0]=jxNow-e.rasterJitter[0];
        p.holoJitter[1]=jyNow-e.rasterJitter[1];
        p.holoJitter[2]=e.jitterFrame+1==g_rowsFrame ? 1.0f:0.0f;
        p.jit[2] = g_filterCurrent ? 1.0f : 0.0f;
        p.jit[3] = g_historyC;
        // The depth's encoding: what the game wrote is A + B / z, A and B
        // from a usable scene row, else the game's infinite-far scene
        // encoding. The camera rows can arrive without a projection row;
        // the runtime's finite far plane is not a valid fallback for it.
        // The scene row on build 332841 says A = 0,
        // B = 0.025 -- no far plane -- where the runtime's 0.025..50000 m
        // decoded 10 km as 8.3 km, 3 km as 2.8 km, and the body's grid
        // missed the station beyond a few hundred metres (19:52).
        float projA = 0.0f, projB = 0.0f;
        const bool measuredProjection = temporalSceneProjection(g_curProj[0], g_curProj[1], nearZ, &projA, &projB);
        if (!measuredProjection && !g_projNoted && projB > 0.0f) {
            g_projNoted = true;
            Log::get().note("temporal aa: no usable scene projection row; using Elite's infinite-far reversed-Z depth = %.6g / metres. OpenVR's finite far plane does not describe this scene depth.", static_cast<double>(projB));
        }
        if (measuredProjection && !g_projNoted && nearZ > 0.0f && farZ > nearZ) {
            g_projNoted = true;
            const float a = g_curProj[0], b = g_curProj[1];
            const float nearRow = (1.0f - a) != 0.0f ? b / (1.0f - a) : b;
            char farTxt[64];
            if (a >= 0.0f) {
                snprintf(farTxt, sizeof(farTxt), "no far plane (depth = %.3f / z)", static_cast<double>(b));
            } else {
                snprintf(farTxt, sizeof(farTxt), "far %.0f m", static_cast<double>(-b / a));
            }
            const float an = nearZ / (nearZ - farZ), bn = nearZ * farZ / (farZ - nearZ);
            const float zr10 = a + b / 10000.0f;
            const float old10 = (zr10 - an) > 0.0f ? bn / (zr10 - an) : 0.0f;
            Log::get().note(
                "temporal aa: the scene block's projection row says reversed-Z with near %.3f m and %s; "
                "the pass decodes the scene's depth with it from here. The %.3f..%.0f m the game asks "
                "the runtime for is the runtime's projection: decoding with those planes read a surface "
                "at 10 km as %.0f m (2026-09-08: the body's grid missed the station beyond a few "
                "hundred metres for it).",
                static_cast<double>(nearRow), farTxt, static_cast<double>(nearZ),
                static_cast<double>(farZ), static_cast<double>(old10));
        }
        p.knobs[0] = projA;
        p.knobs[1] = (haveDepth || screenSrv) ? 1.0f : 0.0f;
        p.knobs[2] = projB;
        p.knobs[3] = farZ;
        if (haveDepth) {
            for (int i = 0; i < 3; ++i) {
                p.tvUsed[i] = headTrans[i];
                p.tvCand[i] = headTransSwapped ? headTransSwapped[i] : headTrans[i];
            }
        }
        // The used delta carries its translation only under the depth
        // motion; the head motion stays rotation-only, as v1 was.
        p.tvUsed[3] = (depthMotion && haveDepth) ? 1.0f : 0.0f;
        // The world/ship split (the shader says what it is): under the
        // depth motion, with a depth bound and both frames' camera rows
        // read (view->world, the rows' measured convention).
        // ...and only in a REAL scene, by the draws into the scene pair a
        // frame. The main menu's backdrop is a pre-rendered image at the far
        // plane drawn with one or two, and its camera does not follow the
        // head, so the world path detached its hangar wall (2026-09-04).
        // The floor was fifty until 2026-09-16, when the scanner's initial
        // screen in a sparse system took 49 draws on four frames in five
        // (eye dump 182049: w 0 at 49, 1 at 50) and the world path stood
        // down under a 0.2 deg/frame pan -- the whole scanner view was
        // reprojected by the still head and smeared. The menu is one or
        // two, a scene is tens to hundreds; the floor sits between.
        const bool floorRefused = sceneDraws < kTemporalSceneDrawFloor;
        const bool worldOn = depthMotion && haveDepth && g_shipMetres > 0.0f &&
                             candValid[2] && worldValid && !floorRefused &&
                             g_rowsFollow >= 0;
        if (floorRefused && depthMotion && haveDepth && g_shipMetres > 0.0f && candValid[2] &&
            worldValid && g_rowsFollow >= 0) {
            ++g_worldFloorRefused;
            g_worldFloorLast = sceneDraws;
        }
        for (int i = 0; i < 3; ++i) p.tvCam[i] = worldOn ? tvCam[i] : 0.0f;
        p.tvCam[3] = worldOn ? 1.0f : 0.0f;
        // A floating-origin jump this frame: the eye run's motion trace and
        // the submission history carry it.
        const bool jumpedNow = g_originJumpFrame == g_rowsFrame;
        p.split[0] = g_shipMetres;
        p.split[1] = static_cast<float>(g_debugMode);
        p.split[2] = g_menuMetres;
        // The menu's assumed depth for depthless pixels: a menu-like scene by
        // the same floor as the world path (the scanner's sky at 49 draws
        // was a wall a few metres off under head sway until 2026-09-16).
        p.split[3] = (haveDepth && sceneDraws < kTemporalSceneDrawFloor) ? 1.0f : 0.0f;
        // Tier 1's mover mask (docs/per-object-motion.md): on only when last
        // frame's depth is in hand at this size AND the frustum and delta in
        // these constants describe that frame -- compared against any other
        // image it would mask everything. The depth copy is written whenever
        // the mask is wanted and a depth is bound, so the next frame has its
        // carry; the compare itself waits for zPrevValid.
        const bool moversOn = g_moversOn && haveDepth && e.zPrevValid && e.zPrev != nullptr &&
                              e.zPrevSrv != nullptr && useTanPrev && haveDelta;
        p.movers[0] = moversOn ? 1.0f : 0.0f;
        p.movers[1] = g_moversTol;
        p.movers[2] = g_moversStrength;
        p.movers[3] = (g_moversOn && haveDepth) ? 1.0f : 0.0f;
        if (moversOn && !g_moversNoted) {
            g_moversNoted = true;
            Log::get().note(
                "temporal aa: the mover mask is on (experimental.temporal_aa_movers) -- a pixel whose "
                "surface is not where the camera alone would have put it last frame (its depth "
                "off by more than %.0f%% from last frame's at that spot: a mover's edge, a "
                "disocclusion) keeps %.0f%% less history, and under dlaa/dlss NVIDIA is handed "
                "the same mask as its bias-current-colour input. One depth copy per eye more "
                "resident (%.0f MB at %ux%u); the masked share prints on the registration line.",
                100.0 * static_cast<double>(g_moversTol),
                100.0 * static_cast<double>(g_moversStrength),
                static_cast<double>(w) * h * 4.0 / 1048576.0, w, h);
        }
        for (int c = 0; c < 3; ++c) {
            p.dR0[c] = delta[0 * 3 + c];
            p.dR1[c] = delta[1 * 3 + c];
            p.dR2[c] = delta[2 * 3 + c];
        }
        int candMask = 0;
        for (int k = 0; k < 4; ++k) {
            if (!candValid[k]) continue;
            candMask |= 1 << k;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) p.cand[k][r][c] = cand[k][r * 3 + c];
            }
        }
        p.blend = blend;
        p.gamma = clampSigma;
        p.haveHistory = useHistory ? 1 : 0;
        p.candMask = useHistory ? candMask : 0;
        const bool uiTrack = uiDepthWantsDraws() && ensureUiHistory(dev,e,w,h);
        if ((flags & 1u) != 0 || !uiTrack) e.uiHistoryValid = false;
        // The scanner's interface on the head's path (fssInterfaceLive
        // says): the flag rides probe.w bit 128 and the eye dump's header
        // (its uiFlags word), so a dump taken in the scanner shows whether
        // the path was engaged; the note below is the log's word, said once.
        const bool fssInterface = fssInterfaceLive();
        if (fssInterface) {
            if (!g_fssInterfaceNoted) {
                g_fssInterfaceNoted = true;
                Log::get().note(
                    "temporal aa: the scanner's screen is up (frame %u), so its interface "
                    "takes the head's path at whatever depth it reads, not the scanner's "
                    "panning camera's -- the panel stays put in front of the seat while "
                    "the camera pans (docs/fss-scanner.md, 2026-09-16). Said once; the eye "
                    "dump's uiFlags word carries bit 0x80 on every frame it is engaged.",
                    g_rowsFrame);
            }
        }
        auto uiFlags = [&]() {
            return static_cast<float>((uiDepthReactive()>0.0f?1u:0u) |
                (uiTrack && e.uiHistoryValid?2u:0u) | (uiTrack?4u:0u) | (terrainSrvs[0]?8u:0u) | (holoSrvs[0]?16u:0u) | (screenSrv?32u:0u) |
                (fssInterface?128u:0u));
        };


        // Capture before either temporal path changes colour. Paired runs
        // use the submitted eye rectangle, including the native TAA path;
        // the old raw-only hook below remains for legacy single runs.
        if (g_eyeRunPaired && (g_eyeRunLeft > 0 || g_eyeRunReady)) {
            g_eyeCaptureFrame = g_rowsFrame;
            if (eye == 0 && g_eyeRunLeft > 0 && g_eyeRunTaken < kEyeRun) {
                uint32_t cw = 0, ch = 0;
                g_eyeRawTaken[g_eyeRunTaken] = stageEyeCrop(ctx, src, &g_eyeRawStaging[g_eyeRunTaken], &cw, &ch, region);
                g_eyeRawInputW[g_eyeRunTaken]=w; g_eyeRawInputH[g_eyeRunTaken]=h;
                EyeDecisionFrame& decision = g_eyeDecisions[g_eyeRunTaken];
                decision = {};
                decision.frame = g_rowsFrame;
                g_eyeRunFrames[g_eyeRunTaken] = g_rowsFrame;
                decision.inputW = w; decision.inputH = h;
                decision.outputW = outW ? outW : w; decision.outputH = outH ? outH : h;
                decision.decisionCrop[0] = (w - (w < kEyeCrop ? w : kEyeCrop)) / 2;
                decision.decisionCrop[1] = (h - (h < kEyeCrop ? h : kEyeCrop)) / 2;
                decision.decisionCrop[2] = w < kEyeCrop ? w : kEyeCrop;
                decision.decisionCrop[3] = h < kEyeCrop ? h : kEyeCrop;
                decision.dlssHistory = e.dlHaveHistory;
                decision.error = "dlss_capture_unavailable";
            }
            if (g_eyeMotionTraceCount < kEyeRun * 4) {
                EyeMotionTrace& t = g_eyeMotionTrace[g_eyeMotionTraceCount++];
                t = {};
                t.frame = g_rowsFrame; t.eye = eye; t.flags = flags;
                t.outputWidth = outW ? outW : w; t.outputHeight = outH ? outH : h;
                t.rowsOk = g_rowsDeltaOwn; t.jumped = jumpedNow; t.dlHistory = e.dlHaveHistory;
                t.rowsBound = g_curRowsBound; t.rowsFollow = g_rowsFollow; t.sceneDraws = sceneDraws;
                memcpy(t.prevRows, g_prevRows, sizeof(t.prevRows));
                memcpy(t.nowRows, g_curRows, sizeof(t.nowRows));
                t.params = p;
            }
        }

        trace.inputs=(haveDepth?1u:0u) | (e.zPrevValid?2u:0u) |
            (haveDelta?4u:0u) | (p.tvCam[3]!=0?8u:0u) |   // 16 and 128 (the body path, the mesh records) retired 2026-09-23
            (terrainSrvs[0]?32u:0u) | (holoSrvs[0]?64u:0u) |
            (screenSrv?256u:0u) | (p.holoJitter[2]!=0?512u:0u) |
            (e.haveHistory?1024u:0u) | (e.dlHaveHistory?2048u:0u) |
            (g_rowsDeltaOwn?4096u:0u) | (jumpedNow?8192u:0u) | (g_curRowsBound?16384u:0u);
        memcpy(trace.worldTranslation,p.tvCam,12);
        if (g_rowsChoice.frame == g_rowsFrame) {
            trace.cameraChoiceFlags = g_rowsChoice.flags;
            trace.selectedSeq = g_rowsChoice.selectedSeq;
            trace.boundLatchSeq = g_rowsChoice.boundLatchSeq;
            trace.twinSeq = g_rowsChoice.twinSeq;
            trace.writesAtChoice = g_rowsChoice.writesAtChoice;
            trace.observedWritesAtChoice = g_rowsChoice.observedWritesAtChoice;
            trace.observedEvictionsAtChoice = g_rowsChoice.observedEvictionsAtChoice;
            trace.candidates = g_rowsChoice.candidates;
            trace.boundCandidates = g_rowsChoice.boundCandidates;
            trace.continuousCandidates = g_rowsChoice.continuousCandidates;
            trace.twinCandidates = g_rowsChoice.twinCandidates;
            trace.selectedResource = reinterpret_cast<uintptr_t>(g_rowsChoice.selectedResource);
            trace.boundResource = reinterpret_cast<uintptr_t>(g_rowsChoice.boundResource);
            memcpy(trace.selectedRows, g_rowsChoice.selectedRows, sizeof(trace.selectedRows));
            memcpy(trace.selectedProj, g_rowsChoice.selectedProj, sizeof(trace.selectedProj));
        }
        const RigidDrawRows& draw = g_rigidDraw[eye];
        if (draw.seen && draw.frame == g_rowsFrame) {
            trace.cameraDrawFlags |= kDrawSeen;
            trace.drawSeq = draw.seq;
            trace.writesAtDraw = draw.writesAtDraw;
            trace.observedWritesAtDraw = draw.observedWritesAtDraw;
            trace.observedEvictionsAtDraw = draw.observedEvictionsAtDraw;
            trace.drawResource = reinterpret_cast<uintptr_t>(draw.resource);
            trace.drawVsHash = draw.vsHash;
            if (draw.observed) trace.cameraDrawFlags |= kDrawWriteObserved;
            if (draw.valid) {
                trace.cameraDrawFlags |= kDrawRowsValid;
                memcpy(trace.drawRows, draw.rows, sizeof(trace.drawRows));
                memcpy(trace.drawProj, draw.proj, sizeof(trace.drawProj));
            }
            const RigidDrawRows& before = g_prevRigidDraw[eye];
            if (draw.valid && before.valid && before.frame + 1 == draw.frame) {
                trace.cameraDrawFlags |= kDrawPreviousValid;
                float drawRotation[9];
                diagnosticDeltaFromRows(before.rows, draw.rows, drawRotation,
                                        trace.drawTranslation);
                trace.drawRotationDeg = temporalRotationAngleDeg(drawRotation);
            }
            if ((trace.cameraChoiceFlags & kChoiceValid) && draw.valid) {
                if (g_rowsChoice.selectedResource == draw.resource)
                    trace.cameraDrawFlags |= kDrawSelectedResource;
                if (g_rowsChoice.selectedSeq == draw.seq)
                    trace.cameraDrawFlags |= kDrawSelectedWrite;
                if (memcmp(g_rowsChoice.selectedRows, draw.rows, sizeof(draw.rows)) == 0)
                    trace.cameraDrawFlags |= kDrawSelectedRows;
                if (g_rowsChoice.selectedSeq > draw.seq)
                    trace.cameraDrawFlags |= kDrawSelectionLater;
                for (int axis = 0; axis < 3; ++axis) {
                    trace.selectedDrawOffset[axis] =
                        g_rowsChoice.selectedRows[axis * 4 + 3] - draw.rows[axis * 4 + 3];
                }
                float drawR[9], selectedR[9], drawRT[9], mismatchR[9];
                temporalRot3Of34(draw.rows, drawR);
                temporalRot3Of34(g_rowsChoice.selectedRows, selectedR);
                temporalTranspose3(drawR, drawRT);
                temporalMul3(drawRT, selectedR, mismatchR);
                trace.selectedDrawRotationDeg = temporalRotationAngleDeg(mismatchR);
            }
        }

        // Every compute slot the dispatches below touch, put back after them
        // (cs_stage_save.h): t0..t22, u0..u6, b0..b2, s0 and the shader.
        static_assert(CsStageSave::kSrvs >= 23 && CsStageSave::kCbs >= 3,
                      "the save covers every slot the dispatches bind: t0..t22 (engine-record velocity's "
                      "t21/t22 included) and b0..b2");
        static_assert(CsStageSave::kSrvs > kEngineVelocityPoolSrv && CsStageSave::kSrvs > kEngineVelocitySlotsSrv &&
                      CsStageSave::kCbs > kEngineVelocityScenePrevCb && CsStageSave::kCbs > kEngineVelocitySceneNowCb,
                      "engine-record velocity's compute slots are inside the save");
        CsStageSave csSaved;
        csSaved.save(ctx);
        // The game's depth target may still be bound on the output-merger
        // stage at submit, and D3D nulls a shader view over a bound target
        // without a word (the depth probe learned that the hard way). The
        // stage is cleared for the dispatch and put back exactly after.
        ID3D11RenderTargetView* savedRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* savedDsv = nullptr;
        if (depthSrv) {
            ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtv, &savedDsv);
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
        }

        // THE TRAINED PASS, when asked for (flags bit 1) and available: the
        // motion vectors and the depth copy from the same reprojection
        // the history fetch uses, the colour copied out typed, then
        // NVIDIA's evaluation into an owned output that goes out in the
        // pass's place. Any refusal says so once and the pass's own
        // history runs instead, this frame and after.
        // The price and the stats slot, both paths: the trained path's own
        // work (the colour copy and the motion-vector dispatch) is timed
        // too, and its counts (the world path, the bright pixels without
        // depth) come back through the same staging buffer.
        // Keep periodic cost measurements without full-rate readbacks. Captures
        // and explicit diagnostics retain all statistics and registration probes.
        const bool diagnostics = g_diagnostics || g_debugMode != 0 || g_eyeRunLeft > 0 || g_eyeRunReady;
        ID3D11ComputeShader* ownCs = ownShader(ctx, diagnostics);
        const bool leanOwn = !diagnostics && ownCs != nullptr && ownCs == g_csFast;
        if (!ownCs) ownCs = g_cs;
        if (!diagnostics && !g_csFast && !g_leanFailNoted) {
            g_leanFailNoted = true;
            Log::get().note("temporal aa: the lean own shader could not be created; the "
                            "instrumented one runs instead (its registration counters stay on).");
        }
        const bool statsWritten = diagnostics || ((flags & 2u) == 0 && !leanOwn) || foveaConfigured();
        // Engine-record velocity's pixel counts (Stats 50..53) come from the
        // trained path's instrumented mv entry alone, through its own counter
        // array -- no barrier or counter is added to the lean variant.
        bool engineCounted = false;   // the instrumented mv ran with the engine inputs bound
        const bool timingOwner = gpuTimingBind(dev, ctx) && gpuTimingAccepts(ctx);
        // Stage 0 price report: sample every call the timing owner accepts,
        // not just the stats-written 1-in-32 (fovea already ran every call;
        // full-frame DLAA did not, so its price sample was 32x sparser than
        // the fovea's). The STAGING READBACK gate just below is unchanged --
        // this only widens which calls get a GPU-timer lease. The pooled F8
        // "temporal AA ms" average (temporalPassDlaaTotals) is fed by the
        // same widened set, so it now samples every frame instead of 1-in-32;
        // its meaning (an average ms per eye-call) is unchanged.
        const bool timingWanted = gpuTimingOwns(ctx);
        const int qs = timingWanted ? acquireSlot(dev) : -1;
        if (timingWanted && qs < 0) ++g_droppedNoSlot;
        if (qs >= 0) {
            auto& slot = g_slots[qs];
            slot.timing = timingOwner && slot.timer.begin(dev, ctx);
            slot.inUse = true;
            slot.timeDone = !slot.timing;
            // Only the later CopyResource makes staging readable. An aborted
            // pass must not consume a previous frame's staging contents.
            slot.statsDone = true;
            // Stage 0 price report: this ring slot may carry a previous
            // eye-call's region bookkeeping (the slot is reused, not reset,
            // between calls) -- clear it before this call's begin/end pairs
            // below write into it.
            slot.priced = false;
            slot.totalValid = false;
            slot.totalMs = 0.0;
            slot.regionsDone = false;
            for (int ri = 0; ri < kRegionCount; ++ri) {
                slot.regionTiming[ri] = false;
                slot.regionEnded[ri] = false;
                slot.regionDone[ri] = false;
                slot.regionMs[ri] = 0.0;
            }
            for (int pi = 0; pi < kPrepParts; ++pi) {
                slot.partTiming[pi] = false;
                slot.partEnded[pi] = false;
                slot.partDone[pi] = false;
                slot.partMs[pi] = 0.0;
            }
        }
        const UINT zeros[4] = {0, 0, 0, 0};
        if (statsWritten) ctx->ClearUnorderedAccessViewUint(g_statsUav, zeros);

        // DLSS where you look (docs/performance.md feature 6): with a fovea
        // width set, NVIDIA runs on a crop around the straight-ahead point and
        // the own history fills the periphery. Under temporal_aa = dlaa the
        // crop is 1:1 (the game rendered full size); under dlss the game
        // rendered small and NVIDIA upscales just the crop to the native
        // output, the periphery upscaled cheaply in the composite. Decided
        // here so the full-frame trained block below can stand aside; the
        // composition runs after the own-history dispatch it blends over.
        const bool upscale = (outW && outH && (outW != w || outH != h));
        const uint32_t foW = upscale ? outW : w;   // the native output size
        const uint32_t foH = upscale ? outH : h;
        // g_debugMode off: the debug views paint the OWN pass's output, and the
        // fovea would blend NVIDIA's crop over that -- a mixed instrument (the
        // review of 2026-09-05, F6). g_foveaFailed off: a failing crop is not
        // retried every frame (F3).
        // A failed crop re-arms when the render or output size changes (F4).
        if (g_foveaFailed && (w != g_foveaFailW || foW != g_foveaFailFoW)) g_foveaFailed = false;
        // The fovea/periphery crop is NVIDIA's NGX subrect path (dlaa.cpp)
        // and has no AMD counterpart (design doc 3.1): with bit 6 set, fsr
        // always runs the full frame, whatever advanced.temporal_aa_fovea
        // says (a width or edges alike).
        const bool amdWanted = (flags & 2u) != 0 && (flags & 64u) != 0;
        const bool foveaWanted = (flags & 2u) != 0 && !amdWanted && fmtIndex == 0 && foveaConfigured() &&
                                 g_foveaCb != nullptr && g_debugMode == 0 && !g_foveaFailed;
        if (amdWanted && fmtIndex == 0 && foveaConfigured() && g_debugMode == 0 && !g_fsrFoveaNoted) {
            g_fsrFoveaNoted = true;
            Log::get().note(
                "temporal aa: the fovea keys are NVIDIA-only; fsr runs the full frame.");
        }
        uint32_t fcx = 0, fcy = 0, fcw = 0, fch = 0;      // INPUT crop, in the render (w x h) space
        // The head lead's slide since the frame NVIDIA's crop history came
        // from, render pixels (base_now - base_prev): handed to the motion
        // pass below as p.lead, which adds it to the vectors the crop reads.
        // Zero unless the lead is on AND the previous applied base is known.
        int32_t foveaLeadDelta[2] = {0, 0};
        uint32_t focx = 0, focy = 0, focw = 0, foch = 0;  // OUTPUT crop, in the native (foW x foH) space
        bool foveaMode = false;
        bool foveaComposited = false;
        bool foveaEvalOk = false;   // the crop eval ran and NVIDIA accumulated: history is live
        // Whether the own resolve's interior skip (p.skip below) applied
        // this frame, and why not when it did not -- read by the edges
        // mode ENGAGED line. Default covers the frame the own resolve does
        // not run at all (the steady periphery covers the whole output).
        const char* foveaSkipNote = "did not run this frame (the steady periphery covered it)";
        // What actually goes out once the compose has run: the composite
        // itself (e.foveaOut) until the shared UI resolve writes the
        // finished frame into e.dlSubmit instead -- read by result= below
        // and reported on the edges mode ENGAGED line.
        ID3D11Texture2D* foveaSubmit = nullptr;
        const char* foveaUiTreatment = "no UI treatment (ui_depth off)";
        // The steady periphery (feature 6): NVIDIA's DLAA around the fovea
        // too, on a reduced copy of the frame, so both sides of the seam are
        // NVIDIA's and neither breathes under head motion the way the own
        // history does (the 2026-09-05 field lesson: the own periphery
        // resamples itself every frame, blurring while the head moves and
        // sharpening when it stops, and against a fovea that does neither the
        // boundary pulsed). Its size is the OUTPUT scaled, or the render when
        // the game rendered smaller than that (nothing to reduce); at 1:1 a
        // scale that reduces nothing is the whole frame through NVIDIA, which
        // is full-frame DLAA, and that runs instead.
        bool steady = false;             // the periphery is NVIDIA's this frame
        bool reduce = false;             // ...on a reduced copy (rw < w), not the render itself
        uint32_t rw = w, rh = h;         // the periphery's size
        bool periphOk = false;           // the periphery eval ran and accumulated this frame
        if (foveaWanted && !g_csFovea && !g_csFoveaTried) {
            g_csFoveaTried = true;
            g_csFovea = shaderSwapCompileCs(ctx, kFoveaCsHlsl, sizeof(kFoveaCsHlsl) - 1,
                                            "fovea", "temporal_fovea_cs", nullptr,
                                            "temporal aa");
        }
        if (foveaWanted && g_periphSteady && !g_csDown && !g_csDownTried) {
            g_csDownTried = true;
            g_csDown = shaderSwapCompileCs(ctx, kDownCsHlsl, sizeof(kDownCsHlsl) - 1,
                                           "down", "temporal_down_cs", nullptr, "temporal aa");
        }
        if (foveaWanted && g_csFovea) warmNoteFirstTreat();   // before NGX's first ask, below
        if (foveaWanted && g_csFovea && dlaaAvailable(dev, nullptr)) {
            const float l = tanNow[0], r = tanNow[1], t = tanNow[2], b = tanNow[3];
            if (r > l && b > t) {
                // The straight-ahead point (tx = ty = 0) and the crop's
                // half-extents, in a given full size -- off the texture centre
                // in an asymmetric frustum. The SAME NDC region in the render
                // and the native frame, so DLSS upscales the render crop to
                // the native crop; equal sizes (no upscale) are DLAA. Even
                // bases and sizes (NGX prefers them), at least 128 px.
                // The disc's centre: the straight-ahead point (tx = ty = 0), or,
                // with a fixation distance set, the point that far straight
                // ahead of the HEAD as this eye sees it -- shifted toward the
                // nose by the eye's offset over the distance -- so the two eyes'
                // discs fuse at that depth instead of at infinity. Fused at
                // infinity the disc read as an object far behind the cockpit
                // and the splash panel, sliding over them as the head turned
                // ("set in space away from me", the 2026-09-05 flight); OpenXR
                // Toolkit ships the same shift as a fixed 4% of the half-width.
                // The eye offset is the runtime's eye-to-head translation,
                // noted per treat; it persists across a frame without a head
                // delta, so the centre never flickers back to infinity.
                // Not applied in edges mode: the rectangle there is placed by
                // its own edges, not a centre and a half-extent.
                float tcx = 0.0f, tcy = 0.0f;
                if (!g_foveaEdges && g_foveaDistance > 0.0f &&
                    (e.eyeOff[0] != 0.0f || e.eyeOff[1] != 0.0f)) {
                    tcx = -e.eyeOff[0] / g_foveaDistance;
                    tcy = -e.eyeOff[1] / g_foveaDistance;
                }
                auto cropOf = [&](uint32_t fw, uint32_t fh, uint32_t& ox, uint32_t& oy,
                                  uint32_t& ow, uint32_t& oh) -> bool {
                    FoveaRegionMode mode;
                    if (g_foveaEdges) {
                        // vertical, outer, nasal -- kTrimNames' own order. Top
                        // and bottom both reduce against fovTrim[0]: fix.fov_
                        // trim_vertical trims the top and bottom equally, so
                        // there is only the one FOV number for either edge.
                        uint32_t fovTrim[3] = {0, 0, 0};
                        nativeFrameFovTrimDegrees(fovTrim);
                        auto reduced = [](float foveaTrimDeg, uint32_t fovTrimDeg) {
                            const float d = foveaTrimDeg - static_cast<float>(fovTrimDeg);
                            return d > 0.0f ? d : 0.0f;
                        };
                        mode.edges = true;
                        mode.topTrimDeg = reduced(g_foveaTopDeg, fovTrim[0]);
                        mode.bottomTrimDeg = reduced(g_foveaBottomDeg, fovTrim[0]);
                        mode.outerTrimDeg = reduced(g_foveaOuterDeg, fovTrim[1]);
                        mode.nasalTrimDeg = reduced(g_foveaNasalDeg, fovTrim[2]);
                    } else {
                        mode.widthDeg = g_foveaDeg;
                        mode.tcx = tcx;
                        mode.tcy = tcy;
                    }
                    const FoveaRegion g = computeFoveaRegion(l, r, t, b, fw, fh, eye, mode, 128);
                    if (!g.ok) return false;
                    ox = g.x; oy = g.y; ow = g.w; oh = g.h;
                    return true;
                };
                // The OUTPUT crop is the input crop scaled exactly to the
                // native frame -- NOT computed independently, which rounds the
                // two apart by up to a native pixel per edge, differently per
                // eye, and reads as a depth step at the seam (the review of
                // 2026-09-05, F2). At foW == w (DLAA) it is the input crop
                // exactly. Worth it only when the output crop is appreciably
                // smaller than the frame.
                auto scaleTo = [](uint32_t v, uint32_t from, uint32_t to) -> uint32_t {
                    return static_cast<uint32_t>((static_cast<uint64_t>(v) * to / from) & ~1ull);
                };
                if (cropOf(w, h, fcx, fcy, fcw, fch)) {
                    // THE HEAD LEAD (advanced.temporal_aa_fovea_lead): the
                    // rectangle slides into a turn of the head or the ship by
                    // this many frames of the frame centre's own motion, so the
                    // strip entering at its leading edge -- which has no
                    // crop-local NVIDIA history and is soft for its first
                    // frames -- lands further out. The BASE alone moves
                    // (foveaLeadBase says why the extents may not), and the
                    // slide is handed to the motion pass so the history that IS
                    // there stays registered.
                    if (g_foveaLeadFrames > 0.0f && eye >= 0 && eye < 2) {
                        FoveaLeadState& st = g_foveaLead[eye];
                        float mvx = 0.0f, mvy = 0.0f;
                        // Which rows a FAR pixel at the centre would take THIS
                        // frame, so MV_centre is the vector the shader actually
                        // writes there: the mv entry's own selection
                        // (temporal_shader_source.h:836 the head's delta rows,
                        // :862 the camera's -- the head and the ship together)
                        // under its own condition, :843 knobs.y != 0 (a depth
                        // is bound; the whole branch sits inside it) and :857
                        // and :860 (tvCam.w, the world path is on; split.x, a
                        // ship split is set; and a far pixel always satisfies
                        // "far || z > split.x"). Its last term, scannerUi at
                        // :859, is a per-pixel uiCovered() read the CPU cannot
                        // make; the frame-wide flag that feeds it (probe.w bit
                        // 128, fssInterfaceLive) stands in, so while the
                        // scanner's screen is up the whole frame takes the
                        // head's rows -- the right way to be wrong there, the
                        // panel sitting in front of the seat and following the
                        // head.
                        //
                        // A mid-turn stand-down of the world path (the draw
                        // floor, a lost camera row) steps the target between
                        // the two readings; the one-pole below absorbs that,
                        // and the slide handed to NVIDIA is base_now -
                        // base_prev either way, so nothing is mis-registered
                        // by the switch.
                        const bool centreWorldRows = p.knobs[1] != 0.0f && p.tvCam[3] != 0.0f &&
                                                     p.split[0] > 0.0f && !fssInterfaceLive();
                        const float* row0 = centreWorldRows ? p.cand[2][0] : p.dR0;
                        const float* row1 = centreWorldRows ? p.cand[2][1] : p.dR1;
                        const float* row2 = centreWorldRows ? p.cand[2][2] : p.dR2;
                        foveaCentreMotion(p.tanNow, p.tanPrev, row0, row1, row2, w, h, &mvx, &mvy);
                        const FoveaLead want = foveaLeadPixels(mvx, mvy, g_foveaLeadFrames);
                        // One pole at a quarter: a tracker's own noise times N
                        // frames would otherwise jitter the seam every frame.
                        st.px[0] += 0.25f * (want.x - st.px[0]);
                        st.px[1] += 0.25f * (want.y - st.px[1]);
                        // The base only moves when the previous APPLIED base is
                        // known and the offset vectors' texture was in hand on
                        // that frame: the slide NVIDIA is told has to be the
                        // truth, so a frame that cannot deliver one holds the
                        // rectangle still. That is the first engaged frame, a
                        // crop-size change (which recreates the feature and
                        // resets its history anyway), and any frame the crop
                        // did not run.
                        const bool live = st.valid && st.ready && st.cropW == fcw && st.cropH == fch;
                        FoveaRegion unshifted;
                        unshifted.x = fcx;
                        unshifted.y = fcy;
                        unshifted.w = fcw;
                        unshifted.h = fch;
                        unshifted.ok = true;
                        const FoveaLeadBase moved = foveaLeadBase(
                            unshifted, live ? st.px[0] : 0.0f, live ? st.px[1] : 0.0f, w, h);
                        fcx = moved.x;
                        fcy = moved.y;
                        if (live) {
                            foveaLeadDelta[0] = static_cast<int32_t>(fcx) - st.base[0];
                            foveaLeadDelta[1] = static_cast<int32_t>(fcy) - st.base[1];
                        }
                        // Re-earned below by a crop that actually evaluates:
                        // if this frame's does not (no textures, a refused
                        // periphery, a failed eval), the next one finds no
                        // previous base and holds the rectangle still rather
                        // than sliding against a frame NVIDIA never saw.
                        st.valid = false;
                        // The window's figures: how far the rectangle sits from
                        // where it would have been, in pixels and as an angle
                        // through this eye's own tangent span.
                        const float slideX = static_cast<float>(moved.dx);
                        const float slideY = static_cast<float>(moved.dy);
                        const float slidePx = sqrtf(slideX * slideX + slideY * slideY);
                        const float slideDeg = foveaLeadDegrees(slidePx, l, r, w);
                        ++st.frames;
                        st.sumDeg += slideDeg;
                        if (slidePx > st.peakPx) {
                            st.peakPx = slidePx;
                            st.peakDeg = slideDeg;
                        }
                        if (moved.clamped) ++st.held;
                    }
                    focx = scaleTo(fcx, w, foW);
                    focw = scaleTo(fcw, w, foW);
                    focy = scaleTo(fcy, h, foH);
                    foch = scaleTo(fch, h, foH);
                    if (focx + focw > foW) focw = (foW - focx) & ~1u;
                    if (focy + foch > foH) foch = (foH - focy) & ~1u;
                }
                bool sizesOk = fcw >= 128 && focw >= 128 && foch >= 128 &&
                               static_cast<uint64_t>(focw) * foch <=
                                   static_cast<uint64_t>(foW) * foH * 9 / 10;
                if (!sizesOk && eye >= 0 && eye < 2 && !g_foveaSizeNoted[eye]) {
                    // Said out loud since 2026-09-17: trims of 5/5 against a
                    // 5/5/7 FOV trim put the rectangle at 99.9% of the frame,
                    // and a flight ran a minute of full-frame DLSS with
                    // nothing in the log but the price line's label. Once
                    // per eye per config load, like the ENGAGED line.
                    g_foveaSizeNoted[eye] = true;
                    const double share = (foW && foH)
                        ? 100.0 * static_cast<double>(focw) * foch / (static_cast<double>(foW) * foH)
                        : 0.0;
                    const char* why = (fcw >= 128 && focw >= 128 && foch >= 128)
                        ? "above the 90% ceiling, where a periphery could not pay for itself"
                        : "under the 128 px NVIDIA's crop needs each way";
                    if (g_foveaEdges) {
                        uint32_t fovTrim[3] = {0, 0, 0};
                        nativeFrameFovTrimDegrees(fovTrim);
                        Log::get().note(
                            "temporal aa: DLSS where you look (edges) is asked for, but eye %d's "
                            "rectangle at top %.0f, bottom %.0f, outer %.0f, nasal %.0f deg (reduced "
                            "by the FOV trim's %u/%u/%u) is %ux%u of the %ux%u output, %.1f%% of the "
                            "frame: %s. Full-frame DLSS runs instead (the price line says so). "
                            "Larger trims make a smaller rectangle; 20/25/7 was 43%%.",
                            eye, static_cast<double>(g_foveaTopDeg), static_cast<double>(g_foveaBottomDeg),
                            static_cast<double>(g_foveaOuterDeg), static_cast<double>(g_foveaNasalDeg),
                            fovTrim[0], fovTrim[1], fovTrim[2], focw, foch, foW, foH, share, why);
                    } else {
                        Log::get().note(
                            "temporal aa: DLSS where you look is asked for at %.0f deg, but the crop "
                            "is %ux%u of the %ux%u output, %.1f%% of the frame: %s. Full-frame DLSS "
                            "runs instead (the price line says so).",
                            static_cast<double>(g_foveaDeg), focw, foch, foW, foH, share, why);
                    }
                }
                if (sizesOk && g_periphSteady) {
                    // The periphery's size: the output scaled, even, at least
                    // 128 each way, never above the render.
                    uint32_t sw = static_cast<uint32_t>(static_cast<float>(foW) * g_periphScale + 0.5f) & ~1u;
                    uint32_t sh = static_cast<uint32_t>(static_cast<float>(foH) * g_periphScale + 0.5f) & ~1u;
                    if (sw < 128) sw = 128;
                    if (sh < 128) sh = 128;
                    if (sw >= w || sh >= h) { sw = w; sh = h; }
                    const bool canReduce = g_csDown != nullptr && g_downCb != nullptr;
                    reduce = sw < w && canReduce;
                    if (!reduce) { sw = w; sh = h; }
                    if (reduce || upscale) {
                        steady = true;
                        rw = sw;
                        rh = sh;
                    } else if (!canReduce) {
                        // No reduction shader (its compile failure is in the
                        // log): the sharp periphery stands in.
                    } else {
                        sizesOk = false;   // the whole frame: full-frame DLAA runs
                        if (!g_periphWholeNoted) {
                            g_periphWholeNoted = true;
                            Log::get().note(
                                "temporal aa: the fovea's steady periphery at scale %.2f would be the "
                                "whole %ux%u frame, which is full-frame DLAA -- so that runs instead. "
                                "Lower temporal_aa_periphery_scale (0.5 is half the pixels each way) "
                                "or set temporal_aa_periphery = sharp for the own history there.",
                                static_cast<double>(g_periphScale), w, h);
                        }
                    }
                }
                if (sizesOk) {
                    foveaMode = true;
                    // The periphery calming (feature 6, the sharp periphery
                    // only): the own pass reads these from the cbuffer and
                    // eases its history lighter with distance from the fovea
                    // centre. The ramp starts at the fovea's edge -- the disc's
                    // radius, outside the blend band, so the band blends NVIDIA
                    // against the own history at its full weight -- and
                    // reaches full strength at the farthest frame corner.
                    if (!steady && g_peripheryCalm > 0.0f) {
                        const float ccx = static_cast<float>(fcx) + fcw * 0.5f;
                        const float ccy = static_cast<float>(fcy) + fch * 0.5f;
                        const float inner = 0.5f * static_cast<float>(fcw < fch ? fcw : fch);
                        float outer = 0.0f;
                        const float cwf = static_cast<float>(w), chf = static_cast<float>(h);
                        const float cor[4][2] = {{0, 0}, {cwf, 0}, {0, chf}, {cwf, chf}};
                        for (int k = 0; k < 4; ++k) {
                            const float dcx = cor[k][0] - ccx, dcy = cor[k][1] - ccy;
                            const float d = sqrtf(dcx * dcx + dcy * dcy);
                            if (d > outer) outer = d;
                        }
                        float ramp = outer - inner;
                        if (ramp < 1.0f) ramp = 1.0f;
                        p.fovea0[0] = ccx;
                        p.fovea0[1] = ccy;
                        p.fovea0[2] = inner;
                        p.fovea0[3] = 1.0f / ramp;
                        p.fovea1[0] = g_peripheryCalm;
                        p.fovea1[3] = 1.0f;   // the fovea is on: modulate
                    }
                }
            }
        }

        // The head lead's state is forgotten on any frame the crop is not
        // running at all -- the fovea off, stood down, refused for its size,
        // or this eye's geometry unusable. foveaLeadForget says why a base
        // may not be kept across such a gap.
        if (!foveaMode) {
            foveaLeadForget(eye);
            // ...and its vector texture goes back in the two states that can
            // never flicker frame to frame: the fovea not asked for at all,
            // and the latched stand-down. A crop merely refused this frame
            // keeps it, so a flicker could not cost a render-sized create.
            if ((!foveaConfigured() || g_foveaFailed) && e.dlMvLead) {
                if (e.dlMvLeadUav) { e.dlMvLeadUav->Release(); e.dlMvLeadUav = nullptr; }
                e.dlMvLead->Release();
                e.dlMvLead = nullptr;
            }
        } else if (eye == 0 && g_eyeRunPaired && g_eyeRunLeft > 0 && g_eyeRunTaken < kEyeRun) {
            g_eyeDecisions[g_eyeRunTaken].error = "foveated_trace_unsupported";
        }

        bool usedDlaa = false;
        // Tier 1's depth carry: true once a dispatch this frame wrote ZC into
        // e.dlDepth with a depth bound and a twin to swap it with, so the
        // frame's end can make it last frame's.
        bool zcWritten = false;
        // Which engine THIS frame asks for, declared once for the whole
        // trained path below (bit 1 = an external engine at all, bit 6 =
        // AMD's rather than NVIDIA's; temporal_pass.h documents both).
        const bool amdEngine = (flags & 64u) != 0;
        const edvr::TemporalEngine engineNow = (flags & 2u) == 0
                                                   ? edvr::TemporalEngine::Own
                                                   : (amdEngine ? edvr::TemporalEngine::Amd
                                                                : edvr::TemporalEngine::Nvidia);
        // A live engine switch, seen on the render thread at the first treat
        // that carries the new engine (F7). The engine being LEFT is freed
        // here; nothing else ever did, so an A/B kept both allocated.
        // NVIDIA's side is deliberately untouched: dlaa.cpp has no
        // per-feature release, only dlaaShutdown (which would drop NGX
        // whole), so leaving fsr keeps NGX's two features exactly as they
        // were before this change.
        if (engineNow != g_engineRan) {
            if (g_engineRan == edvr::TemporalEngine::Amd) {
                edvr::fsr3ReleaseFeatures();
                Log::get().note(
                    "temporal aa: the engine changed to %s, so AMD's two contexts and their six "
                    "working surfaces are released here, on the render thread.",
                    engineNow == edvr::TemporalEngine::Nvidia ? "NVIDIA's"
                                                              : "the pass's own history");
            }
            g_engineRan = engineNow;
        }
        // The refusal line's once-flag, this engine's own (F1).
        bool& engineFailNoted = amdEngine ? g_fsrFailNoted : g_dlaaFailNoted;
        // The trained path copies the colour into R8G8B8A8_UNORM, which is
        // only legal within that family (the review of 2026-09-04, F9): any
        // other family runs the pass's own history and says so once.
        if ((flags & 2u) != 0 && fmtIndex != 0 && !engineFailNoted) {
            engineFailNoted = true;
            if (amdEngine) {
                Log::get().note(
                    "temporal aa: fsr was asked for, but the game submits %s and AMD is "
                    "handed R8G8B8A8, a different family. The pass's own history runs instead.",
                    formatName(sd.Format));
            } else {
                Log::get().note(
                    "temporal aa: dlaa was asked for, but the game submits %s and NVIDIA is "
                    "handed R8G8B8A8, a different family. The pass's own history runs instead.",
                    formatName(sd.Format));
            }
        }
        // The legacy UI resolve (g_csUiResolve), shared by the trained
        // full-frame path and the fovea path (docs/performance.md feature 6
        // UI parity): parameterised by nvidiaOutput, whichever SRV plays
        // "NVIDIA's output" this call (the trained path's own e.dlOutSrv,
        // or the fovea composite's e.foveaOutSrv) -- every other input is
        // read from state already shared between the two paths, so it is
        // identical by construction rather than by two call sites agreeing:
        // uiTrack (outer scope), e.dlSubmitUav/g_csUiResolve/the lazy
        // compile, p.probe[2] for the UI mask
        // (already set fresh by either the trained block's own preamble or
        // the own-path preamble that runs ahead of the fovea block), and
        // e.uiHistoryValid/e.uiHistoryRead. Returns whether it actually
        // dispatched, so the caller knows whether e.dlSubmit now holds the
        // resolved frame or must fall back (the copy-through on the trained
        // path, the composite itself on the fovea path).
        //
        // withHistory is false on the fovea call: e.uiHistory[1-read] is
        // ONE texture with ONE writer a frame by design -- on the trained path
        // the motion pass writes it as UI evidence only when this resolve
        // will not run (4412), and the own-path epilogue (4581) throws a
        // resolve-written history away before the own resolve reads it as
        // UP. On the fovea path the own resolve (sharp periphery) and the
        // motion pass are that writer and reader, so the resolve here must
        // neither read nor write it, nor mark uiResolveWritten: it runs on
        // the composite from the current raster alone, and the periphery's
        // evidence pipeline stays exactly as it was before UI parity.
        auto applyUiResolve = [&](ID3D11ShaderResourceView* nvidiaOutput, bool withHistory) -> bool {
            if (uiTrack && e.dlSubmitUav && !g_csUiResolveTried) {
                g_csUiResolveTried = true;
                g_csUiResolve = shaderSwapCompileCs(ctx, kUiResolve, sizeof(kUiResolve) - 1, "main", "UI resolve", nullptr, "UI resolve");
            }
            const bool debugPaintHere = g_debugMode == 1 || g_debugMode == 3 ||
                                        g_debugMode == 6;
            const bool uiResolveHere = uiTrack && e.dlSubmitUav && g_csUiResolve && !debugPaintHere;
            if (!uiResolveHere) return false;
            const bool uiBoundHere = p.probe[2] != 0.0f;
            // "ui": the UI resolve dispatch, after DLAA/DLSS/the compose.
            beginRegion(qs, Region::Ui, dev, ctx);
            ID3D11ShaderResourceView* nullSrvR[19] = {};
            ID3D11UnorderedAccessView* nullUavR[7] = {};
            ctx->CSSetShaderResources(0, 19, nullSrvR);
            ctx->CSSetUnorderedAccessViews(0, 7, nullUavR, nullptr);
            auto* resolveScreen = screenSrv;
            if (screenSrv && (p.region[0] != int32_t(region[0]) || p.region[1] != int32_t(region[1]))) {
                // Raw colour is cropped, but the screen map is in
                // the original eye texture even on the copy path.
                PassParams resolveParams = p;
                for (int i = 0; i < 4; ++i) resolveParams.region[i] = int32_t(region[i]);
                if (!setParams(ctx, resolveParams)) resolveScreen = nullptr;
            }
            ID3D11ShaderResourceView* srvs[7] = {
                e.dlColourSrv,
                nvidiaOutput,
                uiBoundHere ? e.uiMaskSrv : nullptr,
                (withHistory && e.uiHistoryValid) ? e.uiHistorySrv[e.uiHistoryRead] : nullptr,
                e.dlMvSrv,
                uiDepthContentChanges(w, h, eye),
                resolveScreen};
            ID3D11UnorderedAccessView* uavs[2] = {
                e.dlSubmitUav, withHistory ? e.uiHistoryUav[1 - e.uiHistoryRead] : nullptr};
            ctx->CSSetShader(g_csUiResolve, nullptr, 0);
            ctx->CSSetShaderResources(0, 7, srvs);
            ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
            // NGX may change compute bindings, including b0.
            ctx->CSSetConstantBuffers(0, 1, &g_cb);
            // b1: the clamp's bound tolerance (issue 36). Save whatever
            // NGX left there, bind ours for the dispatch, then restore
            // it so nothing downstream sees EDVR's own buffer.
            ID3D11Buffer* savedCb1 = nullptr;
            ctx->CSGetConstantBuffers(1, 1, &savedCb1);
            ID3D11Buffer* tolCb = nullptr;
            if (g_uiResolveTolCb) {
                D3D11_MAPPED_SUBRESOURCE tm{};
                if (SUCCEEDED(ctx->Map(g_uiResolveTolCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &tm)) && tm.pData) {
                    const float resolveData[4] = {uiDepthGhostTolerance() / 255.0f, uiDepthCoronaHold(), 0, 0};
                    memcpy(tm.pData, resolveData, sizeof(resolveData));
                    ctx->Unmap(g_uiResolveTolCb, 0);
                    tolCb = g_uiResolveTolCb;
                    if (!g_coronaHoldNoted && resolveData[1] > 0.0f) {
                        g_coronaHoldNoted = true;
                        Log::get().note("corona smear: the UI resolve holds faint flat glow within a step of the frame's own level, up to %d/255 (advanced.corona_smear_level; 0 turns it off). Said once.",
                                        static_cast<int>(resolveData[1] * 255.0f + 0.5f));
                    }
                }
            }
            ctx->CSSetConstantBuffers(1, 1, &tolCb);
            ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            ctx->CSSetConstantBuffers(1, 1, &savedCb1);
            if (savedCb1) savedCb1->Release();
            ctx->CSSetShaderResources(0, 19, nullSrvR);
            ctx->CSSetUnorderedAccessViews(0, 7, nullUavR, nullptr);
            endRegion(qs, Region::Ui, ctx);
            if (withHistory) uiEvidenceWritten = uiResolveWritten = true;
            const bool captureResolve = withHistory && eye == 0 && g_eyeRunLeft > 0 && g_eyeRunTaken == 0 &&
                                         g_eyeInputs[0] && g_eyeInputsFrame == g_rowsFrame && !g_eyeInputs[13];
            if (captureResolve) stageEyeRun(ctx, e.uiHistory[1 - e.uiHistoryRead], g_eyeInputs, 15);
            if (!g_uiResolveNoted) {
                g_uiResolveNoted = true;
                Log::get().note("UI resolve: current-raster bounds applied after DLSS; UI influence follows submitted motion as well as its old screen position. Existing submit/UI-history textures reused; no adaptive colour work in the DLSS motion pass.");
            }
            return true;
        };
        if ((flags & 2u) != 0 && fmtIndex == 0 && !foveaMode) {
            warmNoteFirstTreat();   // before NGX's first ask, below
            // advanced.temporal_aa_fsr_reactive/_debug (design doc 3.1); read
            // once here rather than on every NVIDIA frame, since only fsr uses
            // them (fsr3ReadConfig is also called from fsr3Available's own
            // stub, so a build with no AMD SDK still exercises the read).
            const edvr::Fsr3Settings fsrSettings = amdEngine ? edvr::fsr3ReadConfig() : edvr::Fsr3Settings{};
            const char* why = "";
            const bool engineAvailable = amdEngine ? edvr::fsr3Available(dev, &why)
                                                   : dlaaAvailable(dev, &why);
            if (!engineAvailable) {
                if (!engineFailNoted) {
                    engineFailNoted = true;
                    if (amdEngine) {
                        Log::get().note(
                            "temporal aa: fsr was asked for, but %s. The pass's own "
                            "history runs instead.",
                            why);
                    } else {
                        // The hint only when this build HAS AMD's port: a
                        // build made without it would send the commander to
                        // fsr for a second refusal (fsr3BuiltIn is a
                        // compile-time answer and initialises nothing).
                        Log::get().note(
                            "temporal aa: dlaa was asked for, but %s. The pass's own "
                            "history runs instead.%s",
                            why,
                            edvr::fsr3BuiltIn() ? " Set temporal_aa = fsr for AMD's upscaler, "
                                                  "which runs on any GPU."
                                                : "");
                    }
                }
            } else {
                ID3D11ComputeShader* mvCs = motionShader(ctx, diagnostics);
                const bool traceRequested = eye == 0 && g_eyeRunPaired && g_eyeRunLeft > 0 &&
                                            g_eyeRunTaken < kEyeRun && !amdEngine;
                // The size to come back at: the frame's own, or the larger
                // one asked for (DLSS proper).
                const uint32_t oW = (outW && outH && (outW != w || outH != h)) ? outW : w;
                const uint32_t oH = (outW && outH && (outW != w || outH != h)) ? outH : h;
                bool made = mvCs != nullptr;
                // !e.dlSubmit is in the test because the fovea path rebuilds
                // e.dlColour..e.dlOut at the same size but never dlSubmit (it
                // does not use it) -- so a fovea -> full-DLAA switch would find
                // e.dlOut valid, skip this rebuild, and CopyResource into a
                // null dlSubmit, returning null and standing the whole pass
                // down for the session (the review of 2026-09-05, F2).
                if (made && (!e.dlOut || !e.dlSubmit || e.dlW != w || e.dlH != h ||
                             e.dlOutW != oW || e.dlOutH != oH)) {
                    releaseDl(e);
                    made = makeTex(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   D3D11_BIND_SHADER_RESOURCE, &e.dlColour, &e.dlColourSrv, nullptr) &&
                           makeTex(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   &e.dlMv, &e.dlMvSrv, &e.dlMvUav) &&
                           makeTex(dev, w, h, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   &e.dlDepth, &e.dlDepthSrv, &e.dlDepthUav) &&
                           makeTex(dev, oW, oH, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   &e.dlOut, &e.dlOutSrv, &e.dlOutUav) &&
                           // ...and the texture that goes OUT, in the game's own format
                           // (typeless when the game's is), so the compositor is told the
                           // same kind of texture on every path. NVIDIA writes a typed
                           // UNORM, which the own pass never hands out: a typed texture
                           // admits only a typed view at the compositor where a typeless
                           // one admits an sRGB view, and that was the one uniform
                           // brightness change the trained path could have made (the
                           // review of 2026-09-04, D1).
                           makeTex(dev, oW, oH, sd.Format,
                                   (sd.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || sd.Format==DXGI_FORMAT_R8G8B8A8_UNORM) ? DXGI_FORMAT_R8G8B8A8_UNORM : viewFmt,
                                   D3D11_BIND_SHADER_RESOURCE | ((sd.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || sd.Format==DXGI_FORMAT_R8G8B8A8_UNORM) ? D3D11_BIND_UNORDERED_ACCESS : 0),
                                   &e.dlSubmit, nullptr,
                                   (sd.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || sd.Format==DXGI_FORMAT_R8G8B8A8_UNORM) ? &e.dlSubmitUav : nullptr);
                    if (made) {
                        e.dlW = w;
                        e.dlH = h;
                        e.dlOutW = oW;
                        e.dlOutH = oH;
                        if (haveDepth || g_moversOn) ensureMoverPair(dev, e, w, h, g_moversOn);
                    } else {
                        releaseDl(e);
                    }
                } else if (made && (haveDepth || g_moversOn) && (!e.zPrev || (g_moversOn && !e.dlMask))) {
                    ensureMoverPair(dev, e, w, h, g_moversOn);   // depth became available under a live set
                }
                bool traceReady = false;
                if (traceRequested && made) {
                    ID3D11ComputeShader* traceCs = motionTraceShader(ctx);
                    traceReady = traceCs && ensureDecisionTexture(dev, e, w, h);
                    if (traceReady) mvCs = traceCs;
                    else g_eyeDecisions[g_eyeRunTaken].error = traceCs ? "decision_texture_unavailable"
                                                                       : "decision_shader_unavailable";
                } else if (eye == 0 && g_eyeRunPaired && g_eyeRunLeft > 0 &&
                           g_eyeRunTaken < kEyeRun && amdEngine) {
                    g_eyeDecisions[g_eyeRunTaken].error = "non_nvidia_trace_unsupported";
                }
                if (made && setParams(ctx, p)) {
                    const bool debugPaint = g_debugMode == 1 || g_debugMode == 3 ||
                                            g_debugMode == 6;
                    if(uiTrack && e.dlSubmitUav && !g_csUiResolveTried) {
                        g_csUiResolveTried=true;
                        g_csUiResolve=shaderSwapCompileCs(ctx,kUiResolve,sizeof(kUiResolve)-1,"main","UI resolve",nullptr,"UI resolve");
                    }
                    const bool uiResolve=uiTrack && e.dlSubmitUav && g_csUiResolve && !debugPaint;
                    // luma probe stage 0: the texture the game submits.
                    lumaProbeSample(ctx,src,eye,0);
                    // The colour, typed, whichever way the source came.
                    D3D11_BOX box{};
                    box.left = viaCopy ? 0 : region[0];
                    box.top = viaCopy ? 0 : region[1];
                    box.front = 0;
                    box.right = box.left + w;
                    box.bottom = box.top + h;
                    box.back = 1;
                    // "prep": the colour copy and the motion-vector dispatch
                    // below, NVIDIA's inputs.
                    beginRegion(qs, Region::Prep, dev, ctx);
                    beginPart(qs, PrepPart::Copy, dev, ctx);
                    if (viaCopy) {
                        D3D11_BOX full{};
                        full.left = region[0];
                        full.top = region[1];
                        full.front = 0;
                        full.right = region[2];
                        full.bottom = region[3];
                        full.back = 1;
                        ctx->CopySubresourceRegion(e.copyTex, 0, 0, 0, 0, src, 0, &full);
                        ctx->CopySubresourceRegion(e.dlColour, 0, 0, 0, 0, e.copyTex, 0, &box);
                    } else {
                        ctx->CopySubresourceRegion(e.dlColour, 0, 0, 0, 0, src, 0, &box);
                    }
                    // The eye run's raw frame (g_eyeRawStaging says why).
                    if (eye == 0 && g_eyeRunLeft > 0) captureEyeRunRaw(ctx, e.dlColour);
                    endPart(qs, PrepPart::Copy, ctx);
                    // The motion vectors and the depth copy -- and, with the
                    // mover mask on, last frame's depth read at t3 and the
                    // mask written at u5 (tier 1, docs/per-object-motion.md).
                    // The registration probes against NVIDIA's previous output
                    // (the shader says why, 2026-09-08): its last frame is
                    // still in e.dlOut until the evaluation below overwrites
                    // it, so it is bound at t1 in the own history's place --
                    // never while a debug view is painting into it (a
                    // resource cannot be read and written by one dispatch),
                    // and only once it holds a frame that continued the
                    // history, which is what the prediction is measured
                    // against. The constants were written before this block
                    // decided, so the probe row is patched and rewritten here.
                    const bool probeNv = diagnostics && !debugPaint && e.dlHaveHistory && (flags & 1u) == 0 &&
                                         e.dlOutSrv && g_statsUav != nullptr;
                    p.probe[0] = static_cast<float>(oW) / static_cast<float>(w);
                    p.probe[1] = probeNv ? 1.0f : 0.0f;
                    // The interface's reactive mask, when ui_depth marked one
                    // this frame at this size: content that changes without
                    // moving, which no motion vector can describe (ui_depth.h).
                    // NVIDIA takes ONE bias mask, so with the mover mask on it
                    // is bound at t4 and the mv entry folds it into e.dlMask
                    // (the stronger bias wins per pixel); off, it goes to the
                    // runtime as it is. A shader view over ui_depth's texture
                    // (made with the shader-resource bind), cached per eye on
                    // the texture's identity.
                    ID3D11Texture2D* reactiveMask = nullptr;
                    if (!uiDepthReactiveMask(w, h, eye, &reactiveMask)) reactiveMask = nullptr;
                    ID3D11Texture2D* coverageMask = nullptr;
                    if (!uiDepthCoverageMask(w, h, eye, &coverageMask)) coverageMask = nullptr;
                    // Bound for the fold when the mover mask is on.
                    p.holoJitter[3] = haveDepth && e.zPrevValid && e.zPrevSrv && useTanPrev && haveDelta && p.holoJitter[2] != 0 ? 1.0f : 0.0f;
                    const bool wantUi = coverageMask != nullptr &&
                                        (p.holoJitter[3] != 0.0f || (p.movers[0] != 0.0f && e.dlMaskUav != nullptr) ||
                                         uiTrack || fssInterface);
                    const bool uiBound = wantUi && ensureUiMaskSrv(dev, e, coverageMask);
                    if(uiResolve && !e.uiResolvedHistory) e.uiHistoryValid=false;
                    p.probe[2] = uiBound ? 1.0f : 0.0f;
                    p.probe[3] = uiFlags();
                    // Modern DLSS presets ignore the bias mask. The final UI
                    // resolve writes its own influence history below; avoid
                    // the ineffective adaptive colour work in the MV shader.
                    if(uiResolve) p.probe[3]=float(uint32_t(p.probe[3])&~6u);
                    // Engine-record velocity (with fix.temporal_aa on): bit 2048
                    // and t21/t22/b1/b2 below; clear = byte-identical.
                    const bool engineBound = engineViewsGiven && depthSrv;
                    if (engineBound) p.probe[3] =
                        static_cast<float>(static_cast<uint32_t>(p.probe[3]) | 2048u);
                    engineCounted = engineBound && diagnostics;
                    // A masked engine pixel sets the bias mask too, for the
                    // presets and FSR that read it (modern DLSS honours the
                    // history-invalidate vector the same pixel also gets).
                    if (uiTrack || engineBound) ensureBiasMask(dev,e,w,h);
                    setParams(ctx, p);
                    ID3D11ShaderResourceView* nullSrvM[23] = {};
                    // t19..t22 are touched only while engine-record velocity is bound.
                    const UINT srvCountM = engineBound ? 23u : 19u;
                    ID3D11UnorderedAccessView* nullUavM[8] = {};
                    ID3D11UnorderedAccessView* savedTraceUav = nullptr;
                    if (traceReady) ctx->CSGetUnorderedAccessViews(7, 1, &savedTraceUav);
                    ctx->CSSetShaderResources(0, srvCountM, nullSrvM);
                    ctx->CSSetUnorderedAccessViews(0, traceReady ? 8 : 7, nullUavM, nullptr);
                    ctx->CSSetShader(mvCs, nullptr, 0);
                    ID3D11ShaderResourceView* srvsM[23] = {inSrv,
                                                          probeNv ? e.dlOutSrv : e.histSrv[e.histRead],
                                                          depthSrv,
                                                          (p.movers[0] != 0.0f || p.holoJitter[3] != 0.0f) ? e.zPrevSrv : nullptr,
                                                          uiBound ? e.uiMaskSrv : nullptr,
                                                          nullptr,   // t5: free since the body path retired (2026-09-23)
                                                          smokeSrv, uiDepthSrv,
                                                          uiTrack && e.uiHistoryValid ? e.uiHistorySrv[e.uiHistoryRead] : nullptr,
                                                          terrainSrvs[0], terrainSrvs[1], terrainSrvs[2], holoSrvs[0], holoSrvs[1], screenSrv, nullptr, nullptr, nullptr, nullptr,   // t15..t18: free since 2026-09-23 (the mesh records, the static owner's promotion)
                                                          nullptr, nullptr,   // t19/t20: free since stage B's removal
                                                          engineBound ? engineViews.slots : nullptr, engineBound ? engineViews.pool : nullptr};
                    ID3D11UnorderedAccessView* uavsM[8] = {debugPaint ? e.dlOutUav : nullptr,
                                                           nullptr, g_statsUav, e.dlMvUav,
                                                           e.dlDepthUav, e.dlMaskUav,
                                                            uiTrack && !uiResolve ? e.uiHistoryUav[1-e.uiHistoryRead] : nullptr,
                                                            traceReady ? e.dlDecisionUav : nullptr};
                    ID3D11Buffer* cbM[3] = {g_cb, engineViews.sceneNow, engineViews.scenePrev};
                    // b1/b2 only while engine-record velocity is bound, and put
                    // back after: the pass's own save/restore covers b0 alone.
                    ID3D11Buffer* savedCbM[2] = {};
                    if (engineBound) ctx->CSGetConstantBuffers(1, 2, savedCbM);
                    ID3D11SamplerState* smpM = g_samp;
                    ctx->CSSetShaderResources(0, srvCountM, srvsM);
                    ctx->CSSetUnorderedAccessViews(0, traceReady ? 8 : 7, uavsM, nullptr);
                    ctx->CSSetConstantBuffers(0, engineBound ? 3 : 1, cbM);
                    ctx->CSSetSamplers(0, 1, &smpM);
                    gpuCensusBegin(ctx, GpuCensusSection::DoorMotionPrep);
                    beginPart(qs, PrepPart::Mv, dev, ctx);
                    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                    endPart(qs, PrepPart::Mv, ctx);
                    gpuCensusEnd(ctx, GpuCensusSection::DoorMotionPrep);
                    if (engineBound) {
                        ctx->CSSetConstantBuffers(1, 2, savedCbM);
                        for (auto* b : savedCbM) if (b) b->Release();
                    }
                    ctx->CSSetShaderResources(0, srvCountM, nullSrvM);
                    ctx->CSSetUnorderedAccessViews(0, traceReady ? 8 : 7, nullUavM, nullptr);
                    if (traceReady) {
                        EyeDecisionFrame& decision = g_eyeDecisions[g_eyeRunTaken];
                        uint32_t cw=0,ch=0,cx=0,cy=0;
                        if (stageEyeCrop(ctx,e.dlDecision,&g_eyeDecisionStaging[g_eyeRunTaken],&cw,&ch,
                                         nullptr,kEyeCrop,kEyeCrop,&cx,&cy)) {
                            decision.diagnostic=true;decision.diagnosticFrame=g_rowsFrame;
                            decision.decisionCrop[0]=cx;decision.decisionCrop[1]=cy;
                            decision.decisionCrop[2]=cw;decision.decisionCrop[3]=ch;
                            decision.error="dlss_treatment_failed";
                        } else decision.error="decision_stage_failed";
                    }
                    if (traceReady) {
                        ctx->CSSetUnorderedAccessViews(7,1,&savedTraceUav,nullptr);
                        if(savedTraceUav)savedTraceUav->Release();
                    }
                    endRegion(qs, Region::Prep, ctx);
                    if (haveDepth && e.zPrev) zcWritten = true;
                    if (uiTrack && !uiResolve) uiEvidenceWritten = true;
                    if(eye==0)stageEyeInputs(ctx,e,depthSrv,coverageMask,p.probe[2],p.probe[3]);
                    // What NVIDIA is handed: the union when the mover mask
                    // ran this frame, else the interface's alone (as before
                    // the mover mask existed), else nothing. Engine-record
                    // velocity's masked pixels are in the union too.
                    ID3D11Texture2D* biasMask = ((p.movers[0] != 0.0f || uiTrack || engineBound) && e.dlMask) ? e.dlMask : reactiveMask;
                    // NVIDIA's evaluation. Its history restarts only when it is
                    // broken: this eye's first frame, a withhold (flags bit 0),
                    // rebuilt textures, or a frame the pass's own history ran in
                    // between -- never every frame (the review's F1, 2026-09-04).
                    const bool resetHist = (flags & 1u) != 0 || !e.dlHaveHistory;
                    if (eye == 0 && g_eyeRunPaired && g_eyeRunLeft > 0 && g_eyeRunTaken < kEyeRun) {
                        g_eyeDecisions[g_eyeRunTaken].dlssReset = resetHist;
                        g_eyeDecisions[g_eyeRunTaken].dlssHistory = !resetHist;
                    }
                    trace.events |= 8u | (resetHist?16u:0u);
                    if (resetHist) {
                        ++g_dlResets;
                        if (flags & 1u) {
                            ++g_dlResetsAsked;
                            if (flags & 4u) ++g_dlResetsHeld;
                            if (flags & 8u) ++g_dlResetsReturned;
                            if (flags & 16u) ++g_dlResetsUnjudged;
                            if (flags & 32u) ++g_dlResetsNoDelta;
                        }
                    }
                    // The time since this eye's previous evaluation, which the
                    // runtime uses to weigh motion against frame rate; zero on
                    // a restart, when there is no previous frame to measure to.
                    LARGE_INTEGER qNow{}, qFreq{};
                    QueryPerformanceCounter(&qNow);
                    QueryPerformanceFrequency(&qFreq);
                    float frameMs = 0.0f;
                    if (!resetHist && e.dlLastQpc && qFreq.QuadPart > 0) {
                        frameMs = static_cast<float>(
                            static_cast<double>(qNow.QuadPart - e.dlLastQpc) * 1000.0 /
                            static_cast<double>(qFreq.QuadPart));
                        if (frameMs < 1.0f || frameMs > 100.0f) frameMs = 0.0f;
                    }
                    e.dlLastQpc = qNow.QuadPart;
                    // FSR's vertical field of view, in radians (design doc
                    // 3.3: cameraFovAngleVertical = atan(t) + atan(b)).
                    // tanNow is temporalInner's own parameter, already in
                    // scope here -- no ABI change needed to reach it. Sign
                    // convention matches native_temporal.cpp's own top/
                    // bottom (top = -frusta[2], bottom = frusta[3]):
                    // tanNow[2] is the negative down tangent, tanNow[3] the
                    // positive up one.
                    const float fovY = amdEngine
                                           ? std::atan(-tanNow[2]) + std::atan(tanNow[3])
                                           : 0.0f;
                    if (debugPaint) {
                        trace.events |= 32u;
                        // The motion, depth and motion-source views: the mv entry
                        // painted into the output; NVIDIA is skipped and
                        // starts afresh after.
                        usedDlaa = true;
                        e.dlHaveHistory = false;
                    } else if ((gpuCensusBegin(ctx, GpuCensusSection::DoorUpscaler),
                                beginRegion(qs, Region::Full, dev, ctx),
                                amdEngine
                                    ? edvr::fsr3Evaluate(ctx, static_cast<unsigned>(eye),
                                                        e.dlColour,
                                                        e.dlDepth, e.dlMv,
                                                        fsrSettings.reactive ? biasMask : nullptr, e.dlOut,
                                                        w, h, oW, oH, jxNow, jyNow, resetHist, frameMs,
                                                        nearZ, farZ, fovY, &why)
                                    : dlaaEvaluate(ctx, eye, e.dlColour, e.dlDepth, e.dlMv, e.dlOut,
                                                  biasMask, w, h,
                                                  oW, oH, jxNow, jyNow, resetHist, frameMs, &why))) {
                        endRegion(qs, Region::Full, ctx);
                        gpuCensusEnd(ctx, GpuCensusSection::DoorUpscaler);
                        usedDlaa = true;
                        e.dlHaveHistory = true;
                        if (amdEngine) {
                            if (!g_fsrNoted || (oW != w && !g_fsrUpscaleNoted)) {
                                g_fsrNoted = true;
                                if (oW != w) g_fsrUpscaleNoted = true;
                                Log::get().note(
                                    "temporal aa: %s engaged -- AMD's history takes the "
                                    "%ux%u frame, its depth%s and the pass's own motion "
                                    "vectors, jittered as before%s; the pass's history and "
                                    "clip stand aside. Its price prints in the totals.",
                                    edvr::fsr3VersionLabel(), w, h,
                                    depthSrv ? "" : " (none in hand yet: no depth until the "
                                                    "probe finds it)",
                                    oW != w ? " and brings it back to the unit-quality size" : "");
                            }
                        } else if (!g_dlaaNoted || (oW != w && !g_dlssNoted)) {
                            g_dlaaNoted = true;
                            if (oW != w) g_dlssNoted = true;
                            Log::get().note(
                                "temporal aa: %s engaged -- NVIDIA's history takes the "
                                "%ux%u frame, its depth%s and the pass's own motion "
                                "vectors, jittered as before%s; the pass's history and "
                                "clip stand aside. Its price prints in the totals.",
                                oW != w ? "DLSS" : "DLAA", w, h,
                                depthSrv ? "" : " (none in hand yet: no depth until the "
                                                "probe finds it)",
                                oW != w ? " and brings it back to the unit-quality size" : "");
                            // advanced.texture_lod_bias = auto had to decide
                            // before a frame existed, from Elite's own
                            // multiplier. This is the first moment the REAL
                            // fraction is known, so check the two against each
                            // other while there is something to compare. NVIDIA
                            // only: AMD's own render-fraction agreement is not
                            // instrumented here (out of scope for Track B).
                            float autoMult = 0.0f, autoBias = 0.0f;
                            if (deviceHookAutoBiasSource(&autoMult, &autoBias) && autoMult > 0.0f) {
                                // ONLY when NVIDIA is actually upscaling. Under
                                // DLAA the render size IS the output size, so
                                // there is no ratio here to check the launch
                                // figure against -- and comparing against 1.0
                                // told a commander at HMD Quality 0.75 that his
                                // bias disagreed and to restart, which was
                                // false (the pre-release review of 2026-09-07).
                                if (oW && oW != w) {
                                    // Elite's fraction is of what it was TOLD (the host's
                                    // recommendation), which the output equals unless the
                                    // served floor cut it (native_temporal.cpp,
                                    // floorOutput): measured against a cut output, HMD
                                    // Quality 0.45 would read as 0.50 and "disagree".
                                    uint32_t toldW = 0, toldH = 0;
                                    const uint32_t base =
                                        nativeTemporalRecommended(&toldW, &toldH) && toldW >= oW
                                            ? toldW : oW;
                                    const float seen =
                                        static_cast<float>(w) / static_cast<float>(base);
                                    const bool agree = fabsf(seen - autoMult) < 0.02f;
                                    Log::get().note(
                                        "texture filtering: the mip bias %+.2f was derived at launch "
                                        "from Elite's render fraction of %.3f, and this frame's "
                                        "fraction is %.3f -- %s",
                                        static_cast<double>(autoBias), static_cast<double>(autoMult),
                                        static_cast<double>(seen),
                                        agree ? "they agree, so the mips are right for this frame."
                                              : "they DISAGREE. A mip bias is baked into every sampler "
                                                "at creation, so this session's textures are wrong by "
                                                "the difference and only a restart can fix it. If a "
                                                "restart does not, HMDRenderTargetMultiplier no longer "
                                                "means the render fraction and auto needs rethinking.");
                                } else {
                                    Log::get().note(
                                        "texture filtering: the mip bias %+.2f was derived at launch "
                                        "from Elite's render fraction of %.3f. NVIDIA is not "
                                        "upscaling this frame, so there is no ratio here to check it "
                                        "against; the compositor scales the finished frame to the "
                                        "panel instead.",
                                        static_cast<double>(autoBias), static_cast<double>(autoMult));
                                }
                            }
                        }
                    } else {
                        endRegion(qs, Region::Full, ctx);
                        gpuCensusEnd(ctx, GpuCensusSection::DoorUpscaler);
                        e.dlHaveHistory = false;
                        if (!engineFailNoted) {
                            engineFailNoted = true;
                            if (amdEngine) {
                                Log::get().note(
                                    "temporal aa: fsr was asked for, but %s. The pass's own "
                                    "history runs instead.",
                                    why);
                            } else {
                                Log::get().note(
                                    "temporal aa: dlaa was asked for, but %s. The pass's own "
                                    "history runs instead.%s",
                                    why,
                                    edvr::fsr3BuiltIn()
                                        ? " Set temporal_aa = fsr for AMD's upscaler, which runs "
                                          "on any GPU."
                                        : "");
                            }
                        }
                    }
                    // One requested first-eye capture, after NGX and before
                    // UI bounds. Separates model artifacts from retained UI
                    // influence without changing any rendering or bindings.
                    const bool captureResolve=eye==0 && g_eyeRunLeft>0 && g_eyeRunTaken==0 &&
                        g_eyeInputs[0] && g_eyeInputsFrame==g_rowsFrame && !g_eyeInputs[13];
                    if(usedDlaa && captureResolve)stageEyeRun(ctx,e.dlOut,g_eyeInputs,13);
                    if (usedDlaa && eye == 0 && g_eyeRunPaired && g_eyeRunLeft > 0 &&
                        g_eyeRunTaken < kEyeRun) {
                        EyeDecisionFrame& decision = g_eyeDecisions[g_eyeRunTaken];
                        decision.dlssSuccess = !amdEngine && !debugPaint;
                        decision.outputW = oW; decision.outputH = oH;
                        decision.uiMode = uiResolve ? EyeUiMode::Legacy : EyeUiMode::None;
                        uint32_t wantW=0,wantH=0,cw=0,ch=0,cx=0,cy=0;
                        eyeOutputCropSize(g_eyeRunTaken,e.dlOut,&wantW,&wantH);
                        if (stageEyeCrop(ctx,e.dlOut,&g_eyePreUiStaging[g_eyeRunTaken],&cw,&ch,
                                         nullptr,wantW,wantH,&cx,&cy)) {
                            decision.preUi=true;
                            decision.outputCrop[0]=cx;decision.outputCrop[1]=cy;
                            decision.outputCrop[2]=cw;decision.outputCrop[3]=ch;
                        } else decision.error="pre_ui_stage_failed";
                    }
                    // The frame that goes out, in the game's own format (dlSubmit
                    // says why). Inside the timed region, so the price is honest.
                    // applyUiResolve (shared with the fovea path, above) folds
                    // uiResolve and the dispatch itself into one call: it returns false exactly when the
                    // old `else` branch used to run, so the copy-through below
                    // is unchanged. (Main's corona hold, b1's second float and
                    // its once-only note, lives inside the helper.)
                    if (usedDlaa) {
                        gpuCensusBegin(ctx, GpuCensusSection::DoorUiResolve);
                        const bool uiResolvedHere = applyUiResolve(e.dlOutSrv, true);
                        gpuCensusEnd(ctx, GpuCensusSection::DoorUiResolve);
                        if (!uiResolvedHere) ctx->CopyResource(e.dlSubmit, e.dlOut);
                    }
                    // luma probe stage 1, dlss_out: e.dlSubmit already holds it
                    // here on every usedDlaa path, UI-resolved or copied
                    // straight from e.dlOut above.
                    lumaProbeSample(ctx, usedDlaa ? e.dlSubmit : nullptr, eye, 1);
                }
            }
        }

        if (viaCopy && !usedDlaa) {
            D3D11_BOX box{};
            box.left = region[0];
            box.top = region[1];
            box.front = 0;
            box.right = region[2];
            box.bottom = region[3];
            box.back = 1;
            ctx->CopySubresourceRegion(e.copyTex, 0, 0, 0, 0, src, 0, &box);
        }
        const int readIdx = e.histRead;
        const int writeIdx = 1 - readIdx;
        // The own path: the interface's coverage mask at t4 for the body
        // path's exclusion (the trained block above binds its own).
        bool engineOwn = false; // engine-record velocity's inputs bound for the own path (fovea mv, main)
        if (!usedDlaa) {
            if(e.uiResolvedHistory){e.uiHistoryValid=false;e.uiResolvedHistory=false;}
            ID3D11Texture2D* rm = nullptr;
            const bool uiOwn = (trainedWanted || uiTrack || fssInterface) && uiDepthCoverageMask(w, h, eye, &rm) && rm &&
                               ensureUiMaskSrv(dev, e, rm);
            p.probe[2] = uiOwn ? 1.0f : 0.0f;
            p.probe[3] = uiFlags();
            engineOwn = engineViewsGiven && depthSrv;
            if (engineOwn) p.probe[3] = static_cast<float>(static_cast<uint32_t>(p.probe[3]) | 2048u);
        }
        bool ran = usedDlaa || (ensureNative(dev, e, viewFmt) && setParams(ctx, p));
        // A native fallback also writes counters, even when the requested
        // trained path was running without diagnostics.
        if (ran && !usedDlaa && !leanOwn && !statsWritten) ctx->ClearUnorderedAccessViewUint(g_statsUav, zeros);
        bool ownRan = false;
        if (ran && !usedDlaa) {
            // THE FOVEA (docs/performance.md feature 6), NVIDIA's part first:
            // the colour copied out typed, the motion vectors and the depth
            // copy, then the steady periphery's reduction and DLAA, then the
            // fovea crop. The own pass runs after, and only when it is needed
            // -- the whole frame when the fovea is off or stood down this
            // frame, the periphery when the periphery is the sharp one. The
            // composite then blends whichever periphery there is under
            // NVIDIA's crop. All Elite's frames are the R8G8B8A8 family (the
            // full DLAA path's CopyResource proves it), so the crop and the
            // periphery share a channel order and a UNORM view, and the blend
            // is in the stored representation the compositor samples as sRGB
            // -- the same convention as the full trained path.
            bool compositeReady = false;   // g_foveaCb holds this frame's composite parameters
            float bandPx = 0.0f;
            const char* whyF = "";
            auto standDown = [&](const char* why) {
                // Latched: a failing crop (an NGX create or eval error) is not
                // retried every frame -- a create costs tens to hundreds of ms
                // (F3). The own history runs full-frame for the rest of the
                // session, or until a config reload or size change re-arms it.
                g_foveaFailed = true;
                g_foveaFailW = w;
                g_foveaFailFoW = foW;
                foveaLeadForget(eye);   // no crop ran: this frame's base is not a base to slide from
                if (!g_foveaFailNoted) {
                    g_foveaFailNoted = true;
                    Log::get().note(
                        "temporal aa: the fovea was asked for, but %s. It is stood down for the "
                        "session (the own history runs full-frame); edit the ini or change the "
                        "render size to try again.",
                        why);
                }
            };
            if (foveaMode) {
                // The lean variant unless diagnostics are asked for, exactly
                // as the full-frame path above: this dispatch leaves u2 (the
                // stats buffer) unbound anyway (see its own comment below),
                // so the instrumented entry's counters were dropped here in
                // any case, and nothing else it binds is instrumented-only.
                ID3D11ComputeShader* mvCs = motionShader(ctx, diagnostics);
                bool made = mvCs != nullptr;
                if (made && (!e.dlOut || !e.dlColourSrv || !e.dlDepthSrv || !e.dlMvSrv ||
                             e.dlW != w || e.dlH != h || e.dlOutW != foW || e.dlOutH != foH)) {
                    releaseDl(e);
                    made = makeTex(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   D3D11_BIND_SHADER_RESOURCE, &e.dlColour, &e.dlColourSrv, nullptr) &&
                           makeTex(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   &e.dlMv, &e.dlMvSrv, &e.dlMvUav) &&
                           makeTex(dev, w, h, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   &e.dlDepth, &e.dlDepthSrv, &e.dlDepthUav) &&
                           makeTex(dev, foW, foH, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   &e.dlOut, &e.dlOutSrv, &e.dlOutUav);
                    if (made) {
                        e.dlW = w; e.dlH = h; e.dlOutW = foW; e.dlOutH = foH;
                        if (haveDepth || g_moversOn) ensureMoverPair(dev, e, w, h, g_moversOn);
                    } else {
                        releaseDl(e);
                    }
                } else if (made && (haveDepth || g_moversOn) && (!e.zPrev || (g_moversOn && !e.dlMask))) {
                    ensureMoverPair(dev, e, w, h, g_moversOn);
                }
                // The head lead's own vector texture (the field's comment says
                // why it cannot be dlMv itself), made only while the key is on
                // and given straight back when it goes off: it is another
                // render-sized R16G16 per eye. A failure to make it is not a
                // stand-down -- the lead simply never goes live, because the
                // base is held still until the texture is in hand (the lead
                // block above), and the window's lead line then says the
                // vectors were offset on no frames.
                if (made && g_foveaLeadFrames > 0.0f && !e.dlMvLead) {
                    if (!makeTex(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                 D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                 &e.dlMvLead, nullptr, &e.dlMvLeadUav)) {
                        if (e.dlMvLeadUav) { e.dlMvLeadUav->Release(); e.dlMvLeadUav = nullptr; }
                        if (e.dlMvLead) { e.dlMvLead->Release(); e.dlMvLead = nullptr; }
                    } else if (!g_foveaLeadNoted) {
                        g_foveaLeadNoted = true;
                        Log::get().note(
                            "temporal aa: the fovea's head lead is on at %g frames "
                            "(advanced.temporal_aa_fovea_lead) -- the crop slides into a turn of "
                            "the head or the ship, and NVIDIA's crop reads its own copy of the "
                            "motion vectors with that slide added, %.0f MB more resident per eye "
                            "at %ux%u. The price report's own lead line says how far it actually "
                            "moved.",
                            static_cast<double>(g_foveaLeadFrames),
                            static_cast<double>(w) * h * 4.0 / 1048576.0, w, h);
                    }
                } else if (g_foveaLeadFrames <= 0.0f && e.dlMvLead) {
                    if (e.dlMvLeadUav) { e.dlMvLeadUav->Release(); e.dlMvLeadUav = nullptr; }
                    e.dlMvLead->Release();
                    e.dlMvLead = nullptr;
                }
                // Built ahead of the compose, in the game's own format
                // (that field's own comment says why). NOT the compose's
                // own UAV target -- that is e.foveaOut, next block, kept
                // separate so the shared UI resolve helper below has an SRV
                // to read the finished composite from while it writes here.
                // Mirrors the full-frame trained block's own dlSubmit
                // creation above: identical conditional format/UAV logic,
                // at foW x foH (== oW x oH there always: both are outW/outH
                // under an upscale, w/h otherwise).
                if (made && (!e.dlSubmit || e.dlOutW != foW || e.dlOutH != foH)) {
                    if (e.dlSubmitUav) { e.dlSubmitUav->Release(); e.dlSubmitUav = nullptr; }
                    if (e.dlSubmit) { e.dlSubmit->Release(); e.dlSubmit = nullptr; }
                    made = makeTex(dev, foW, foH, sd.Format,
                                   (sd.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || sd.Format==DXGI_FORMAT_R8G8B8A8_UNORM) ? DXGI_FORMAT_R8G8B8A8_UNORM : viewFmt,
                                   D3D11_BIND_SHADER_RESOURCE | ((sd.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || sd.Format==DXGI_FORMAT_R8G8B8A8_UNORM) ? D3D11_BIND_UNORDERED_ACCESS : 0),
                                   &e.dlSubmit, nullptr,
                                   (sd.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || sd.Format==DXGI_FORMAT_R8G8B8A8_UNORM) ? &e.dlSubmitUav : nullptr);
                }
                // The composite's own texture: the compose dispatch's UAV
                // target, with an SRV so the shared UI resolve helper below
                // can bind it in NVIDIA's-output's role. Kept apart from
                // dlSubmit (above) on purpose -- the resolve reads this SRV
                // and writes dlSubmit's UAV in the same dispatch, and a
                // resource can never be bound as both at once.
                if (made && (!e.foveaOut || e.foveaW != foW || e.foveaH != foH)) {
                    if (e.foveaOutUav) { e.foveaOutUav->Release(); e.foveaOutUav = nullptr; }
                    if (e.foveaOutSrv) { e.foveaOutSrv->Release(); e.foveaOutSrv = nullptr; }
                    if (e.foveaOut) { e.foveaOut->Release(); e.foveaOut = nullptr; }
                    made = makeTex(dev, foW, foH, sd.Format, viewFmt,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   &e.foveaOut, &e.foveaOutSrv, &e.foveaOutUav);
                    if (made) { e.foveaW = foW; e.foveaH = foH; }
                }
                // The steady periphery's textures: NVIDIA's reduced output
                // always; the reduced inputs only when the reduction is real.
                if (made && steady &&
                    (!e.prOut || e.prW != rw || e.prH != rh || e.prReduced != reduce)) {
                    releasePeriph(e);
                    made = makeTex(dev, rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   &e.prOut, &e.prOutSrv, &e.prOutUav);
                    if (made && reduce) {
                        made = makeTex(dev, rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                       D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                       &e.prColour, nullptr, &e.prColourUav) &&
                               makeTex(dev, rw, rh, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
                                       D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                       &e.prDepth, nullptr, &e.prDepthUav) &&
                               makeTex(dev, rw, rh, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                       D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                       &e.prMv, nullptr, &e.prMvUav);
                    }
                    if (made) {
                        e.prW = rw; e.prH = rh; e.prReduced = reduce;
                    } else {
                        releasePeriph(e);
                    }
                }
                p.holoJitter[3] = haveDepth && e.zPrevValid && e.zPrevSrv && useTanPrev && haveDelta && p.holoJitter[2] != 0 ? 1.0f : 0.0f;
                // The head lead's slide for the motion pass below: NVIDIA's
                // crop is the only reader (through e.dlMvLead/ML), so it is
                // zero unless that texture is bound on this dispatch. Zeroed
                // again straight after it, so no later dispatch on this frame
                // -- the own resolve's, in particular -- can see it.
                const bool leadBound = e.dlMvLeadUav != nullptr;
                p.lead[0] = leadBound ? static_cast<float>(foveaLeadDelta[0]) : 0.0f;
                p.lead[1] = leadBound ? static_cast<float>(foveaLeadDelta[1]) : 0.0f;
                if (made && e.outSrv && e.dlOutSrv && setParams(ctx,p)) {
                    // The colour, typed, whichever way the source came (the
                    // full path's copy logic).
                    D3D11_BOX box{};
                    beginRegion(qs, Region::Prep, dev, ctx);
                    // Prep's parts on the foveated route too (the performance
                    // review's attribution gap 2): the colour copy, then the
                    // motion-vector dispatch.
                    beginPart(qs, PrepPart::Copy, dev, ctx);
                    box.left = viaCopy ? 0 : region[0];
                    box.top = viaCopy ? 0 : region[1];
                    box.front = 0;
                    box.right = box.left + w;
                    box.bottom = box.top + h;
                    box.back = 1;
                    if (viaCopy) {
                        D3D11_BOX full{};
                        full.left = region[0]; full.top = region[1]; full.front = 0;
                        full.right = region[2]; full.bottom = region[3]; full.back = 1;
                        ctx->CopySubresourceRegion(e.copyTex, 0, 0, 0, 0, src, 0, &full);
                        ctx->CopySubresourceRegion(e.dlColour, 0, 0, 0, 0, e.copyTex, 0, &box);
                    } else {
                        ctx->CopySubresourceRegion(e.dlColour, 0, 0, 0, 0, src, 0, &box);
                    }
                    endPart(qs, PrepPart::Copy, ctx);
                    // Motion vectors and the depth copy, full frame (NVIDIA
                    // reads the crop's sub-rectangle of them; the reduction
                    // reads them whole). The mover mask is computed here too
                    // (t3, u5) but only the own periphery pass applies it:
                    // the crop and periphery evaluations are not handed it.
                    //
                    // Eight UAVs here, not seven: u7 (ML) is the head lead's
                    // own copy of the vectors, and this is the ONE dispatch
                    // that binds it -- everywhere else the slot stays null and
                    // the shader's store there is dropped.
                    ID3D11ShaderResourceView* nullSrvM[23] = {};
                    const UINT srvCountM = engineOwn ? 23u : 19u;
                    ID3D11UnorderedAccessView* nullUavM[8] = {};
                    ctx->CSSetShaderResources(0, srvCountM, nullSrvM);
                    ctx->CSSetUnorderedAccessViews(0, 8, nullUavM, nullptr);
                    ctx->CSSetShader(mvCs, nullptr, 0);
                    ID3D11ShaderResourceView* srvsM[23] = {inSrv, e.histSrv[readIdx], depthSrv,
                                                          (p.movers[0] != 0.0f || p.holoJitter[3] != 0.0f) ? e.zPrevSrv : nullptr,
                                                          p.probe[2] != 0.0f ? e.uiMaskSrv : nullptr,
                                                          nullptr,   // t5: free since the body path retired (2026-09-23)
                                                          smokeSrv, uiDepthSrv,
                                                          uiTrack && e.uiHistoryValid ? e.uiHistorySrv[e.uiHistoryRead] : nullptr,
                                                          terrainSrvs[0], terrainSrvs[1], terrainSrvs[2], holoSrvs[0], holoSrvs[1], screenSrv, nullptr, nullptr, nullptr, nullptr,   // t15..t18: free since 2026-09-23 (the mesh records, the static owner's promotion)
                                                          nullptr, nullptr,   // t19/t20: free since stage B's removal
                                                          engineOwn ? engineViews.slots : nullptr, engineOwn ? engineViews.pool : nullptr};
                    // u2 (the stats buffer) is left UNBOUND here: the own pass
                    // writes its stats when it runs, and the mv entry writes
                    // the same slots (15-17), so binding it would double them
                    // (the review of 2026-09-05, F5). Atomics on a null UAV
                    // are dropped.
                    ID3D11UnorderedAccessView* uavsM[8] = {nullptr, nullptr, nullptr,
                                                           e.dlMvUav, e.dlDepthUav, e.dlMaskUav,
                                                           uiTrack ? e.uiHistoryUav[1-e.uiHistoryRead] : nullptr,
                                                           e.dlMvLeadUav};
                    ID3D11Buffer* cbM[3] = {g_cb, engineViews.sceneNow, engineViews.scenePrev};
                    ID3D11Buffer* savedCbM[2] = {};
                    if (engineOwn) ctx->CSGetConstantBuffers(1, 2, savedCbM);
                    ID3D11SamplerState* smpM = g_samp;
                    ctx->CSSetShaderResources(0, srvCountM, srvsM);
                    ctx->CSSetUnorderedAccessViews(0, 8, uavsM, nullptr);
                    ctx->CSSetConstantBuffers(0, engineOwn ? 3 : 1, cbM);
                    ctx->CSSetSamplers(0, 1, &smpM);
                    gpuCensusBegin(ctx, GpuCensusSection::DoorMotionPrep);
                    beginPart(qs, PrepPart::Mv, dev, ctx);
                    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                    endPart(qs, PrepPart::Mv, ctx);
                    gpuCensusEnd(ctx, GpuCensusSection::DoorMotionPrep);
                    if (engineOwn) {
                        ctx->CSSetConstantBuffers(1, 2, savedCbM);
                        for (auto* b : savedCbM) if (b) b->Release();
                    }
                    ctx->CSSetShaderResources(0, srvCountM, nullSrvM);
                    ctx->CSSetUnorderedAccessViews(0, 8, nullUavM, nullptr);
                    endRegion(qs, Region::Prep, ctx);
                    // The vectors NVIDIA's crop will read carried the slide on
                    // this frame: counted here, at the dispatch that wrote
                    // them, not where the slide was decided -- a frame that
                    // never dispatched must not be counted as offset.
                    if (leadBound && (p.lead[0] != 0.0f || p.lead[1] != 0.0f) &&
                        eye >= 0 && eye < 2) {
                        ++g_foveaLead[eye].moved;
                    }
                    // ...and the field goes back to zero, so the own resolve's
                    // own setParams below cannot ship a stale slide.
                    p.lead[0] = p.lead[1] = 0.0f;
                    if (haveDepth && e.zPrev) zcWritten = true;
                    if (uiTrack) uiEvidenceWritten = true;

                    // NVIDIA's frame delta, shared by the periphery and the
                    // crop (evaluated together): the time since this eye's
                    // previous evaluation, zero when unknown or absurd.
                    LARGE_INTEGER qn{}, qf{};
                    QueryPerformanceCounter(&qn);
                    QueryPerformanceFrequency(&qf);
                    float frameMs = 0.0f;
                    if (e.dlLastQpc && qf.QuadPart > 0) {
                        frameMs = static_cast<float>(
                            static_cast<double>(qn.QuadPart - e.dlLastQpc) * 1000.0 /
                            static_cast<double>(qf.QuadPart));
                        if (frameMs < 1.0f || frameMs > 100.0f) frameMs = 0.0f;
                    }
                    e.dlLastQpc = qn.QuadPart;
                    // The steady periphery: the reduction (when it is real),
                    // then DLAA over the whole reduced frame.
                    if (steady) {
                        bool inputsOk = true;
                        if (reduce) {
                            // "reduce": the periphery's own downsample dispatch.
                            beginRegion(qs, Region::Reduce, dev, ctx);
                            D3D11_MAPPED_SUBRESOURCE dm{};
                            if (SUCCEEDED(ctx->Map(g_downCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &dm)) &&
                                dm.pData) {
                                const float ratio = static_cast<float>(w) / static_cast<float>(rw);
                                float* dc = static_cast<float*>(dm.pData);
                                dc[0] = static_cast<float>(rw);
                                dc[1] = static_cast<float>(rh);
                                dc[2] = static_cast<float>(w);
                                dc[3] = static_cast<float>(h);
                                dc[4] = ceilf(ratio - 1e-4f);   // unused since the area-weighted box; kept for the layout
                                dc[5] = static_cast<float>(rw) / static_cast<float>(w);
                                dc[6] = 0.0f;
                                dc[7] = 0.0f;
                                ctx->Unmap(g_downCb, 0);
                                ID3D11ShaderResourceView* dsrv[3] = {e.dlColourSrv, e.dlDepthSrv, e.dlMvSrv};
                                ID3D11UnorderedAccessView* duav[3] = {e.prColourUav, e.prDepthUav, e.prMvUav};
                                ID3D11ShaderResourceView* nullD3[3] = {};
                                ID3D11UnorderedAccessView* nullDu[3] = {};
                                ctx->CSSetShaderResources(0, 3, nullD3);
                                ctx->CSSetUnorderedAccessViews(0, 3, nullDu, nullptr);
                                ctx->CSSetShader(g_csDown, nullptr, 0);
                                ctx->CSSetShaderResources(0, 3, dsrv);
                                ctx->CSSetUnorderedAccessViews(0, 3, duav, nullptr);
                                ctx->CSSetConstantBuffers(0, 1, &g_downCb);
                                ctx->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
                                ctx->CSSetShaderResources(0, 3, nullD3);
                                ctx->CSSetUnorderedAccessViews(0, 3, nullDu, nullptr);
                                endRegion(qs, Region::Reduce, ctx);
                            } else {
                                endRegion(qs, Region::Reduce, ctx);
                                inputsOk = false;
                            }
                        }
                        if (inputsOk) {
                            // The jitter in the periphery's own pixels: the
                            // render's offset scaled by the reduction.
                            const float sx = static_cast<float>(rw) / static_cast<float>(w);
                            const float sy = static_cast<float>(rh) / static_cast<float>(h);
                            const bool resetP = (flags & 1u) != 0 || !e.prHaveHistory;
                            // "periphery": the steady periphery's own NGX role.
                            gpuCensusBegin(ctx, GpuCensusSection::DoorUpscaler);
                            beginRegion(qs, Region::Periphery, dev, ctx);
                            periphOk = dlaaEvaluatePeriphery(
                                ctx, eye, reduce ? e.prColour : e.dlColour,
                                reduce ? e.prDepth : e.dlDepth, reduce ? e.prMv : e.dlMv, e.prOut,
                                rw, rh, jxNow * sx, jyNow * sy, resetP, frameMs, &whyF);
                            endRegion(qs, Region::Periphery, ctx);
                            gpuCensusEnd(ctx, GpuCensusSection::DoorUpscaler);
                            if (!periphOk) standDown(whyF);
                        }
                    }
                    // The fovea crop -- unless the steady periphery it is
                    // composited over just failed (the latch has it).
                    if (!steady || periphOk) {
                        const bool resetHist = (flags & 1u) != 0 || !e.foveaHaveHistory;
                        // The vectors NVIDIA's crop reads: its own copy with
                        // this frame's slide in them when the head lead is
                        // live, the shared ones otherwise. Nothing else ever
                        // reads e.dlMvLead, and e.dlMv is untouched either way.
                        ID3D11Texture2D* cropMv = e.dlMvLead ? e.dlMvLead : e.dlMv;
                        // "centre": the fovea crop's own NGX role.
                        if ((gpuCensusBegin(ctx, GpuCensusSection::DoorUpscaler),
                             beginRegion(qs, Region::Centre, dev, ctx),
                             dlssEvaluateFovea(ctx, eye, e.dlColour, e.dlDepth, cropMv, e.dlOut, w, h,
                                              foW, foH, fcx, fcy, fcw, fch, focx, focy, focw, foch,
                                              jxNow, jyNow, resetHist, frameMs, &whyF))) {
                            endRegion(qs, Region::Centre, ctx);
                            gpuCensusEnd(ctx, GpuCensusSection::DoorUpscaler);
                            foveaEvalOk = true;   // e.foveaHaveHistory is set from this at frame end
                            // The head lead's carry: the base NVIDIA's crop
                            // history now belongs to, the size it ran at, and
                            // whether the offset vectors were there to make the
                            // next frame's slide deliverable.
                            if (eye >= 0 && eye < 2) {
                                FoveaLeadState& st = g_foveaLead[eye];
                                st.base[0] = static_cast<int32_t>(fcx);
                                st.base[1] = static_cast<int32_t>(fcy);
                                st.cropW = fcw;
                                st.cropH = fch;
                                st.valid = true;
                                st.ready = e.dlMvLeadUav != nullptr;
                            }
                        } else {
                            endRegion(qs, Region::Centre, ctx);
                            gpuCensusEnd(ctx, GpuCensusSection::DoorUpscaler);
                            standDown(whyF);
                        }
                    }
                }
                // This frame's composite parameters, written BEFORE the own
                // pass decides whether to run: if they cannot be, the own pass
                // runs whole and nothing is composited this frame.
                if (foveaEvalOk) {
                    const bool perSteady = periphOk;   // else the own periphery
                    const uint32_t perW = perSteady ? rw : w, perH = perSteady ? rh : h;
                    D3D11_MAPPED_SUBRESOURCE fm{};
                    if (SUCCEEDED(ctx->Map(g_foveaCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &fm)) &&
                        fm.pData) {
                        // The band, the crop and the disc are in the NATIVE
                        // output space (foW x foH), where the composite runs.
                        float band = g_foveaEdgeDeg *
                                     (static_cast<float>(foW) / (tanNow[1] - tanNow[0])) *
                                     0.01745329252f;
                        const float maxBand = 0.5f * static_cast<float>(focw < foch ? focw : foch);
                        if (band > maxBand) band = maxBand;
                        if (band < 1.0f) band = 1.0f;
                        float* fc = static_cast<float*>(fm.pData);
                        fc[0] = static_cast<float>(focx);
                        fc[1] = static_cast<float>(focy);
                        fc[2] = static_cast<float>(focx + focw);
                        fc[3] = static_cast<float>(focy + foch);
                        fc[4] = band;
                        fc[5] = static_cast<float>(foW);
                        fc[6] = static_cast<float>(foH);
                        fc[7] = static_cast<float>(perW);
                        fc[8] = static_cast<float>(focx) + 0.5f * static_cast<float>(focw);
                        fc[9] = static_cast<float>(focy) + 0.5f * static_cast<float>(foch);
                        fc[10] = 0.5f * static_cast<float>(focw);
                        fc[11] = 0.5f * static_cast<float>(foch);
                        fc[12] = g_foveaRound ? 1.0f : 0.0f;
                        // The hand-off into the own history: the sharp
                        // periphery at 1:1 only (the history is render size).
                        fc[13] = (!perSteady && perW == foW && perH == foH) ? 1.0f : 0.0f;
                        fc[14] = (perW < foW || perH < foH) ? 1.0f : 0.0f;
                        fc[15] = static_cast<float>(perH);
                        ctx->Unmap(g_foveaCb, 0);
                        compositeReady = true;
                        bandPx = band;
                    }
                }
            }
            // THE OWN PASS: the whole frame, unless the steady periphery and
            // the fovea both ran and the composite is ready to take them.
            const bool ownNeeded = !(foveaMode && steady && periphOk && foveaEvalOk && compositeReady);
            if (ownNeeded) {
                // Tier 1's carry on the own path: last frame's depth at t3
                // when the mask is on, this frame's written at u4 whenever
                // the mask is wanted and a depth is bound. The textures are
                // the trained set's depth copy and its twin, made here alone
                // when no trained set exists; u3/u5 stay unbound (MV and the
                // mask are NVIDIA's inputs, and writes to a null UAV drop).
                const bool carry = g_moversOn && haveDepth && ensureMoverPair(dev, e, w, h);
                // The periphery's own resolve skips the crop's interior's
                // COLOUR RESOLVE whenever the compose is certain to run and
                // overwrite that rectangle at weight 1 this frame: the fovea
                // on, the centre crop evaluated, the composite parameters
                // ready. That is the whole gate now. The contract the shader
                // holds up in there (temporal_shader_source.h, the sibling
                // branch after the resolve): the three writes the NEXT frame
                // reads back all still happen -- the UI evidence history
                // (UN/u6, uiTrack), which adaptiveUiReactive reads back at a
                // reprojected position, and the mover mask's depth carry
                // (ZC/u4, carry), which comes back as ZP -- and the colour
                // history (N/u1) is refreshed from the RAW current frame
                // there. So none of the three can go stale, and the three
                // conditions that used to stand the skip down (an upscale,
                // uiTrack, carry) no longer have anything to protect. The
                // compose's own 1:1 hand-off into that history (mode.y,
                // fc[13]) still overwrites the interior with the composite
                // afterwards when it is on, which is at 1:1 only; under an
                // upscale it is off and the raw frame is what the interior's
                // history keeps, which is what a pass-through resolve would
                // have left there anyway. Only the colour resolve is saved.
                p.skip[0] = p.skip[1] = p.skip[2] = p.skip[3] = 0.0f;
                if (foveaMode && foveaEvalOk && compositeReady) {
                    float band = g_foveaEdgeDeg *
                                 (static_cast<float>(w) / (tanNow[1] - tanNow[0])) *
                                 0.01745329252f;
                    const float maxBand = 0.5f * static_cast<float>(fcw < fch ? fcw : fch);
                    if (band > maxBand) band = maxBand;
                    if (band < 1.0f) band = 1.0f;
                    // A whole extra pixel past the band, since the compose's
                    // weight only reaches exactly 1 AT the band's edge and
                    // this is GPU-side floating point recomputing the same
                    // boundary a second way; costs at most another pixel or
                    // two of the own resolve's work.
                    const float margin = band + 1.0f;
                    float sx0 = 0.0f, sy0 = 0.0f, sx1 = 0.0f, sy1 = 0.0f;
                    if (g_foveaRound) {
                        // The compose's TRUE weight-1 region (fovea() in
                        // kFoveaCsHlsl): d = (1-length(n))*min(rz,rw), so the
                        // level set d=band is an ellipse scaled by k = 1 -
                        // margin/min(rz,rw) on BOTH semi-axes -- not "each
                        // axis minus the band" independently, which is not a
                        // SUBSET of the true region when the crop is not
                        // square (routine in edges mode) and would make the
                        // skip unsafe there. The inscribed axis-aligned
                        // rectangle of that true ellipse is exactly safe.
                        const float rz = 0.5f * static_cast<float>(fcw);
                        const float rw = 0.5f * static_cast<float>(fch);
                        const float m = rz < rw ? rz : rw;
                        const float k = 1.0f - margin / m;
                        if (k > 0.0f) {
                            const float halfX = rz * k * 0.70710678f;
                            const float halfY = rw * k * 0.70710678f;
                            const float ccx = static_cast<float>(fcx) + rz;
                            const float ccy = static_cast<float>(fcy) + rw;
                            sx0 = ccx - halfX; sy0 = ccy - halfY;
                            sx1 = ccx + halfX; sy1 = ccy + halfY;
                        }
                    } else {
                        sx0 = static_cast<float>(fcx) + margin;
                        sy0 = static_cast<float>(fcy) + margin;
                        sx1 = static_cast<float>(fcx + fcw) - margin;
                        sy1 = static_cast<float>(fcy + fch) - margin;
                    }
                    if (sx1 > sx0 + 1.0f && sy1 > sy0 + 1.0f) {
                        p.skip[0] = sx0; p.skip[1] = sy0; p.skip[2] = sx1; p.skip[3] = sy1;
                        foveaSkipNote = "skips the interior's colour resolve (history refreshed from the raw frame there)";
                    } else {
                        foveaSkipNote = "does not skip the interior (the crop is too small for the band's margin)";
                    }
                }
                setParams(ctx, p);
                ID3D11ShaderResourceView* nullSrv[23] = {};
                const UINT srvCount = engineOwn ? 23u : 19u;
                ID3D11UnorderedAccessView* nullUav[7] = {};
                ctx->CSSetShaderResources(0, srvCount, nullSrv);
                ctx->CSSetUnorderedAccessViews(0, 7, nullUav, nullptr);
                ctx->CSSetShader(ownCs, nullptr, 0);
                if (leanOwn && !g_leanNoted) {
                    g_leanNoted = true;
                    Log::get().note(
                        "temporal aa: the pass's own history runs the lean shader -- the "
                        "registration instrument (up to four candidate reprojections per "
                        "pixel, counted and not used), its probes and its counters are "
                        "compiled out; advanced.temporal_aa_diagnostics = 1 runs the "
                        "instrumented one, live, and the price line names which ran.");
                }
                ID3D11ShaderResourceView* srvs[23] = {inSrv, e.histSrv[readIdx], depthSrv,
                                                     (carry && p.movers[0] != 0.0f) ? e.zPrevSrv : nullptr,
                                                     p.probe[2] != 0.0f ? e.uiMaskSrv : nullptr,
                                                     nullptr,   // t5: free since the body path retired (2026-09-23)
                                                     smokeSrv, uiDepthSrv,
                                                     uiTrack && e.uiHistoryValid ? e.uiHistorySrv[e.uiHistoryRead] : nullptr,
                                                          terrainSrvs[0], terrainSrvs[1], terrainSrvs[2], holoSrvs[0], holoSrvs[1], screenSrv, nullptr, nullptr, nullptr, nullptr,   // t15..t18: free since 2026-09-23 (the mesh records, the static owner's promotion)
                                                          nullptr, nullptr,   // t19/t20: free since stage B's removal
                                                          engineOwn ? engineViews.slots : nullptr, engineOwn ? engineViews.pool : nullptr};
                ID3D11UnorderedAccessView* uavs[7] = {e.outUav, e.histUav[writeIdx],
                                                      g_statsUav, nullptr,
                                                      carry ? e.dlDepthUav : nullptr, nullptr,
                                                      uiTrack ? e.uiHistoryUav[1-e.uiHistoryRead] : nullptr};
                ID3D11Buffer* cbs[3] = {g_cb, engineViews.sceneNow, engineViews.scenePrev};
                ID3D11Buffer* savedCbs[2] = {};
                if (engineOwn) ctx->CSGetConstantBuffers(1, 2, savedCbs);
                ID3D11SamplerState* smp = g_samp;
                ctx->CSSetShaderResources(0, srvCount, srvs);
                ctx->CSSetUnorderedAccessViews(0, 7, uavs, nullptr);
                ctx->CSSetConstantBuffers(0, engineOwn ? 3 : 1, cbs);
                ctx->CSSetSamplers(0, 1, &smp);
                // Untimed (falls into the price report's "other") when the
                // fovea is off -- ordinary, non-fovea TAA's own resolve
                // always ran this dispatch and was never charged to a named
                // region; that is unchanged. With the fovea on, this
                // dispatch IS the periphery whenever it runs (steady's own
                // NGX role failed, or sharp mode, where it always runs) --
                // "periphery" is the region name the price line already
                // prints for that role.
                if (foveaMode) beginRegion(qs, Region::Periphery, dev, ctx);
                ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                if (foveaMode) endRegion(qs, Region::Periphery, ctx);
                if (engineOwn) {
                    ctx->CSSetConstantBuffers(1, 2, savedCbs);
                    for (auto* b : savedCbs) if (b) b->Release();
                }
                ctx->CSSetShaderResources(0, srvCount, nullSrv);
                ctx->CSSetUnorderedAccessViews(0, 7, nullUav, nullptr);
                if (carry) zcWritten = true;
                if (uiTrack) uiEvidenceWritten = true;
                ownRan = true;
            }
            // THE COMPOSITE: NVIDIA's crop over whichever periphery there is.
            if (foveaMode && foveaEvalOk && compositeReady && (periphOk || ownRan)) {
                // "compose": the fovea/periphery blend dispatch.
                beginRegion(qs, Region::Compose, dev, ctx);
                ID3D11ShaderResourceView* csrv[2] = {periphOk ? e.prOutSrv : e.outSrv, e.dlOutSrv};
                // u1 is the own history being written this frame, for the
                // hand-off (the shader writes it only when mode.y says so).
                // u0 is e.foveaOutUav, the composite's own texture -- never
                // dlSubmit, which the UI resolve helper below binds as an
                // SRV+UAV pair with THIS texture (never itself: see its own
                // comment on the struct field).
                ID3D11UnorderedAccessView* cuav[2] = {
                    e.foveaOutUav, (!periphOk && !upscale) ? e.histUav[writeIdx] : nullptr};
                ID3D11ShaderResourceView* nullC2[2] = {};
                ID3D11UnorderedAccessView* nullCu[2] = {};
                ID3D11SamplerState* smpC = g_samp;
                ctx->CSSetShaderResources(0, 2, nullC2);
                ctx->CSSetUnorderedAccessViews(0, 2, nullCu, nullptr);
                ctx->CSSetShader(g_csFovea, nullptr, 0);
                ctx->CSSetShaderResources(0, 2, csrv);
                ctx->CSSetUnorderedAccessViews(0, 2, cuav, nullptr);
                ctx->CSSetConstantBuffers(0, 1, &g_foveaCb);
                ctx->CSSetSamplers(0, 1, &smpC);
                ctx->Dispatch((foW + 7) / 8, (foH + 7) / 8, 1);
                ctx->CSSetShaderResources(0, 2, nullC2);
                ctx->CSSetUnorderedAccessViews(0, 2, nullCu, nullptr);
                endRegion(qs, Region::Compose, ctx);
                foveaComposited = true;
                // UI parity with the full-frame trained path: the same
                // choice it makes (the UI resolve, else none), sharing
                // applyUiResolve above so the two paths can never drift.
                // e.foveaOutSrv stands in for "NVIDIA's output" -- the
                // compose already blended NVIDIA's crop into the composite,
                // so the resolve reads the FINISHED frame here, not the bare
                // crop.
                gpuCensusBegin(ctx, GpuCensusSection::DoorUiResolve);
                const bool foveaUiResolved = applyUiResolve(e.foveaOutSrv, false);
                gpuCensusEnd(ctx, GpuCensusSection::DoorUiResolve);
                if (foveaUiResolved) {
                    foveaSubmit = e.dlSubmit;
                    foveaUiTreatment = "the UI resolve (from the current raster alone: the UI history stays the periphery's)";
                } else {
                    foveaSubmit = e.foveaOut;
                    foveaUiTreatment = "no UI treatment (ui_depth off)";
                }
                ++g_foveaTreats;
                if ((!g_foveaEdges && !g_foveaNoted) ||
                    (g_foveaEdges && eye >= 0 && eye < 2 && !g_foveaEdgesNoted[eye])) {
                    char per[200];
                    if (periphOk) {
                        snprintf(per, sizeof(per),
                                 "NVIDIA's too -- DLAA on a %ux%u copy (%.0f%% of the output each "
                                 "way), upscaled bicubically; the pass's own history stands aside",
                                 rw, rh, 100.0 * static_cast<double>(rw) / static_cast<double>(foW));
                    } else {
                        snprintf(per, sizeof(per), "the pass's own history (%ux%u render%s)", w, h,
                                 upscale ? ", upscaled bicubically to the output" : "");
                    }
                    if (!g_foveaEdges) {
                        g_foveaNoted = true;
                        Log::get().note(
                            "temporal aa: DLSS where you look ENGAGED -- NVIDIA runs on a %ux%u->%ux%u "
                            "crop (%s, %.0f deg, %s) around the %s at (%u, %u) of the "
                            "%ux%u output, %.1f%% of its pixels; the periphery is %s; blended over "
                            "%.0f deg (%.0f px). NVIDIA's price is in the DLAA totals.",
                            fcw, fch, focw, foch, upscale ? "DLSS" : "DLAA",
                            static_cast<double>(g_foveaDeg), g_foveaRound ? "round" : "square",
                            g_foveaDistance > 0.0f ? "fixation point (the discs meet at the set depth)"
                                                   : "straight-ahead point (the discs meet at infinity)",
                            focx + focw / 2, focy + foch / 2, foW, foH,
                            100.0 * static_cast<double>(focw) * foch /
                                (static_cast<double>(foW) * foH),
                            per, static_cast<double>(g_foveaEdgeDeg), static_cast<double>(bandPx));
                    } else {
                        g_foveaEdgesNoted[eye] = true;
                        uint32_t fovTrim[3] = {0, 0, 0};   // vertical, outer, nasal
                        nativeFrameFovTrimDegrees(fovTrim);
                        // The region printed is this frame's, head lead and
                        // all: the line is latched per eye per config load, so
                        // it is a sample of where the rectangle sat, not a
                        // fixed place -- the lead line in the price report is
                        // what says how far it moves.
                        char leadTxt[64];
                        if (g_foveaLeadFrames > 0.0f) {
                            snprintf(leadTxt, sizeof(leadTxt), "head lead %g frames",
                                     static_cast<double>(g_foveaLeadFrames));
                        } else {
                            snprintf(leadTxt, sizeof(leadTxt), "no head lead");
                        }
                        Log::get().note(
                            "temporal aa: DLSS where you look ENGAGED (edges) -- eye %d requested "
                            "top %.0f, bottom %.0f, outer %.0f, nasal %.0f deg, reduced by the FOV "
                            "trim's %u/%u/%u; region %u,%u-%u,%u of %ux%u (%.1f%% of the frame); the "
                            "periphery is %s; the own resolve %s; UI treatment: %s; %s. NVIDIA's "
                            "price is in the DLAA totals.",
                            eye, static_cast<double>(g_foveaTopDeg), static_cast<double>(g_foveaBottomDeg),
                            static_cast<double>(g_foveaOuterDeg), static_cast<double>(g_foveaNasalDeg),
                            fovTrim[0], fovTrim[1], fovTrim[2], focx, focy, focx + focw, focy + foch,
                            foW, foH,
                            100.0 * static_cast<double>(focw) * foch /
                                (static_cast<double>(foW) * foH),
                            per, foveaSkipNote, foveaUiTreatment, leadTxt);
                    }
                }
            }
        }
        if (qs >= 0) {
            if (g_slots[qs].timing && !g_slots[qs].timer.end(ctx)) {
                g_slots[qs].timer.reset(ctx); g_slots[qs].timing=false;
            }
            if (statsWritten || (ran && !usedDlaa && !leanOwn)) ctx->CopyResource(g_slots[qs].staging, g_stats);
            g_slots[qs].inUse = true;
            g_slots[qs].timeDone = !g_slots[qs].timing;
            g_slots[qs].statsDone = !(statsWritten || (ran && !usedDlaa && !leanOwn));
            g_slots[qs].engineStats = engineCounted;
            g_slots[qs].pixels = static_cast<uint64_t>(w) * h;
            g_slots[qs].hadHistory = useHistory;
            g_slots[qs].headDeg = headDeg;
            for (int k = 0; k < 4; ++k) {
                g_slots[qs].candPixels[k] =
                    (useHistory && candValid[k]) ? static_cast<uint64_t>(w) * h : 0;
            }
            // Stage 0 price report: this call's identity, for stereo pairing
            // (eye + frame) and the window-reset test (treatment, output
            // shape, config generation). foW/foH read the same as this
            // branch's own output size either way (the full-frame branch's
            // local oW/oH match them exactly when it runs).
            g_slots[qs].eye = eye;
            g_slots[qs].frame = g_rowsFrame;
            // Stage 0 price report: labelled by what actually ran this call,
            // not what the flags/format/fovea setup intended (F4) -- a
            // persistent stand-down (NVIDIA refused, the fovea crop failed)
            // must not be mislabelled by the intent that failed to happen.
            g_slots[qs].treatment = foveaComposited ? Treatment::Fovea
                                    : usedDlaa       ? Treatment::FullFrame
                                                     : Treatment::None;
            // Fovea is always NVIDIA's (foveaComposited implies dlaaEvaluate
            // ran the crop); a full frame is whichever engine flags bit 6
            // asked for; nothing else ran means Own, same as treatment.
            g_slots[qs].engine = foveaComposited ? edvr::TemporalEngine::Nvidia
                                 : !usedDlaa      ? edvr::TemporalEngine::Own
                                 : (flags & 64u)   ? edvr::TemporalEngine::Amd
                                                    : edvr::TemporalEngine::Nvidia;
            g_slots[qs].lean = leanOwn;
            // The trained paths' motion shader follows `diagnostics`; the own
            // path's compose is instrumented unless it ran lean.
            g_slots[qs].instrumented = (foveaComposited || usedDlaa) ? diagnostics : !leanOwn;
            g_slots[qs].outW = foW;
            g_slots[qs].outH = foH;
            g_slots[qs].fmt = static_cast<uint32_t>(sd.Format);
            g_slots[qs].configGen = g_configGeneration;
        }

        // The pass's own views off the compute stage first (every slot the
        // save covers), so none of them is bound for reading when the game's
        // targets go back on the output merger; then the game's compute
        // bindings, exactly.
        ID3D11ShaderResourceView* nullSrv2[CsStageSave::kSrvs] = {};
        ID3D11UnorderedAccessView* nullUav2[CsStageSave::kUavs] = {};
        ctx->CSSetShaderResources(0, CsStageSave::kSrvs, nullSrv2);
        ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, nullUav2, nullptr);
        if (depthSrv) {
            ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtv, savedDsv);
            for (auto* v : savedRtv) if (v) v->Release();
            if (savedDsv) savedDsv->Release();
        }
        csSaved.restore(ctx);

        // NVIDIA's crop history is live only if the crop eval ran and
        // accumulated THIS frame. Any frame that did not composite the fovea
        // -- full DLAA, the own history alone, a failed eval -- clears it, so
        // a later re-engage of the fovea resets rather than blending a stale
        // crop against fresh motion (the review of 2026-09-05, F4).
        e.foveaHaveHistory = foveaEvalOk;
        e.rasterJitter[0]=jxNow; e.rasterJitter[1]=jyNow; e.jitterFrame=g_rowsFrame;
        e.prHaveHistory = periphOk;

        // Tier 1's depth carry: this frame's copy becomes last frame's by
        // swapping the two textures and their views -- no copy -- and the
        // mask may compare against it next frame. A frame that wrote none
        // (no depth in hand, no dispatch with the copy bound) breaks the
        // carry, and the mask waits for the next one that does.
        if (zcWritten && e.zPrev) {
            std::swap(e.dlDepth, e.zPrev);
            std::swap(e.dlDepthSrv, e.zPrevSrv);
            std::swap(e.dlDepthUav, e.zPrevUav);
            e.zPrevValid = true;
        } else {
            e.zPrevValid = false;
        }

        if (ran && usedDlaa) {
            trace.output=2;trace.outputWidth=e.dlOutW;trace.outputHeight=e.dlOutH;
            // The trained pass's frame goes out; the pass's own history is
            // marked broken so a switch back starts afresh.
            e.haveHistory = false;
            releaseNative(e); // successful full-frame DLSS owns its own history
            result = e.dlSubmit;
            ++g_treats;
            ++g_dlaaTreats;
            if (!g_trainedNoted) {
                // What NVIDIA is handed, once: the review of 2026-09-04
                // found the trained path invisible in the log (F12).
                g_trainedNoted = true;
                Log::get().note(
                    "temporal aa: first trained frame -- the game submits %s (DXGI_FORMAT "
                    "%d)%s, %ux%u per eye; NVIDIA is handed the colour as R8G8B8A8_UNORM, "
                    "the depth as R32_FLOAT (%s), the motion as R16G16_FLOAT in render "
                    "pixels and this frame's jitter (%+.3f, %+.3f) px, and answers at "
                    "%ux%u, handed on in the game's own format.",
                    formatName(sd.Format), static_cast<int>(sd.Format),
                    viaCopy ? " (copied out first: the source refuses a shader view)" : "",
                    w, h,
                    depthSrv ? "the scene's, reversed-Z"
                             : "none yet: zeros until the probe finds it",
                    static_cast<double>(jxNow), static_cast<double>(jyNow), e.dlOutW,
                    e.dlOutH);
            }
        } else if (ran) {
            trace.output=foveaComposited?3u:1u;trace.outputWidth=w;trace.outputHeight=h;
            if (ownRan) {
                e.histRead = writeIdx;
                e.haveHistory = true;
            } else {
                // The own pass stood aside (the steady periphery took its
                // place): its history did not see this frame and restarts
                // when it is next needed.
                e.haveHistory = false;
            }
            e.dlHaveHistory = false;   // NVIDIA's FULL-frame history did not see this frame
            // The fovea composite, when it ran, is what goes out: foveaSubmit
            // -- e.dlSubmit once the shared UI resolve has run on it (the
            // same post-resolve state the full-frame trained branch
            // submits), or the bare composite when it did not. The own history is still the periphery it
            // was blended over, so its ping-pong above stands. NVIDIA's
            // crop history lives in the fovea feature (e.foveaHaveHistory),
            // kept apart from both.
            result = foveaComposited ? foveaSubmit : e.outTex;
            ++g_treats;
            if (foveaComposited) ++g_dlaaTreats;
            g_lastW = w;
            g_lastH = h;
            if (!g_firstNoted) {
                g_firstNoted = true;
                const double mb = static_cast<double>(w) * h *
                                  (2.0 * (g_histFmt == DXGI_FORMAT_R10G10B10A2_UNORM ? 4.0 : 8.0) +
                                   4.0) / 1048576.0;
                Log::get().note(
                    "temporal aa: first treated frame -- the game submits %s "
                    "(DXGI_FORMAT %d), read and written through %s views%s; "
                    "%ux%u per eye, history in %s, about %.0f MB per eye "
                    "resident; motion from the %s, blend %.2f, clip %.2f "
                    "sigma.",
                    formatName(sd.Format), static_cast<int>(sd.Format),
                    formatName(viewFmt),
                    viaCopy ? " (copied out first: the source refuses a "
                              "shader view)"
                            : "",
                    w, h, formatName(g_histFmt), mb, motionName(motion),
                    static_cast<double>(blend), static_cast<double>(clampSigma));
            }
        } else {
            failOnce("the parameter buffer could not be written");
        }
        // luma probe stage 2, final, and the end-of-round report: `result`
        // is the true texture handed to the VR half (e.dlSubmit on the
        // trained path, the own pass's or the fovea composite's output
        // otherwise, or null on the failure branch just above). No
        // submit-affecting write and no early return happens between there
        // and here, so this is the last safe moment.
        lumaProbeSample(ctx, static_cast<ID3D11Texture2D*>(result), eye, 2);
        lumaProbeEnd(ctx, eye);
    }

    // The eye dump, armed by its key or the menu: this treated eye, as it goes
    // out, before the references are dropped.
    if (eptr) {
        eptr->uiResolvedHistory = result && uiResolveWritten;
        if (result && uiEvidenceWritten) {
            eptr->uiHistoryRead = 1 - eptr->uiHistoryRead;
            eptr->uiHistoryValid = true;
        } else eptr->uiHistoryValid = false;
    }
    if (result && ctx && g_eyeDumpArmed[eye]) {
        g_eyeDumpArmed[eye] = false;
        dumpEye(ctx, static_cast<ID3D11Texture2D*>(result), eye);
    }
    if (result && ctx && eye == 0 && g_eyeRunLeft > 0) {
        captureEyeRun(ctx, static_cast<ID3D11Texture2D*>(result));
    }
    if(eye==1 && ctx && g_eyeRunReady) {
        writeEyeRun(ctx,g_eyeRunWidth,g_eyeRunHeight);g_eyeRunReady=false;
    }
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    src->Release();
    if(!result) { trace.output=0;trace.outputWidth=trace.outputHeight=0; }
    return result;
}

}  // namespace

void temporalPassDumpHistory(const char* trigger) {
    std::vector<TemporalHistoryEntry> entries;
    {
        std::lock_guard<std::mutex> lock(g_temporalHistoryMutex);
        entries.reserve(g_temporalHistory.size());
        for(uint32_t i=0;i<g_temporalHistory.size();++i)entries.push_back(g_temporalHistory.oldest(i));
    }
    LARGE_INTEGER now{},freq{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&freq);
    Log::get().note("--- temporal submission history: %zu eye calls, rows-frame now=%u, %s. "
        "Times are relative to this dump. Missing frames mean no temporal call; output=0 refused, "
        "1 native, 2 NVIDIA, 3 native/NVIDIA fovea. flags are the OpenVR request. "
        "events hex: 1 requested reset,2 size/format change,4 source-screen change,8 NVIDIA attempt,"
        "10 NVIDIA reset,20 diagnostic paint. inputs hex: 1 depth,2 previous depth,4 delta,8 world,"
        "10 body,20 terrain,40 holo,80 mesh,100 source-screen,200 consecutive,400 native history,"
        "800 NVIDIA history,1000 camera rows,2000 origin step,4000 bound rows. CPU only. ---",
        entries.size(),g_rowsFrame,trigger?trigger:"diagnostic request");
    constexpr size_t kTcamEntries = 512;
    const size_t tcamFirst = entries.size() > kTcamEntries ? entries.size() - kTcamEntries : 0;
    Log::get().note(
        "TCAM legend: choice flags 1 valid,2 bound seen,4 bound write observed,8 selected bound,"
        "10 continuous,20 identical-to-previous twin present,40 twin fallback,80 resync,100 refollow,"
        "200 bound rows valid. Draw flags 1 eligible draw seen,2 mapped write observed,4 rows valid,"
        "8 previous draw rows valid,10 same resource,20 same observed write,40 same rows,80 selection later. "
        "Rows are literal scene rows; their origin is not assumed to be headset centre or eye pose. "
        "Detailed rows follow for the newest %zu of %zu eye calls; %zu older calls retain TEMP only.",
        entries.size() - tcamFirst, entries.size(), tcamFirst);
    if(entries.empty())Log::get().note("TEMP no temporal submissions recorded; this is not evidence that AA ran successfully.");
    size_t entryIndex = 0;
    for(const auto& e:entries) {
        const double ago=freq.QuadPart?double(int64_t(e.qpc)-now.QuadPart)*1000.0/double(freq.QuadPart):0;
        Log::get().note("TEMP %+.1fms f%u eye=%u flags=%X events=%X inputs=%X out=%u size=%ux%u->%ux%u "
            "jitter=(%+.5f,%+.5f) world=(%+.7g,%+.7g,%+.7g)",
            ago,e.frame,e.eye,e.flags,e.events,e.inputs,e.output,e.width,e.height,e.outputWidth,e.outputHeight,
            double(e.jitterX),double(e.jitterY),double(e.worldTranslation[0]),double(e.worldTranslation[1]),
            double(e.worldTranslation[2]));
        if (entryIndex++ < tcamFirst) continue;
        Log::get().note(
            "TCAM f%u eye=%u cf=%X df=%X sel=%p:%u bound=%p:%u twin=%u "
            "cand=%u/%u/%u/%u writes=%u/%u evict=%u draw=%p:%u@%u/%u evict=%u vs=%016llX "
            "sel-draw=(%+.9g,%+.9g,%+.9g;%.7gdeg) draw-delta=(%+.9g,%+.9g,%+.9g;%.7gdeg) "
            "proj=(%+.9g,%+.9g)/(%+.9g,%+.9g) "
            "S=[%+.9g,%+.9g,%+.9g,%+.9g;%+.9g,%+.9g,%+.9g,%+.9g;%+.9g,%+.9g,%+.9g,%+.9g] "
            "D=[%+.9g,%+.9g,%+.9g,%+.9g;%+.9g,%+.9g,%+.9g,%+.9g;%+.9g,%+.9g,%+.9g,%+.9g]",
            e.frame,e.eye,e.cameraChoiceFlags,e.cameraDrawFlags,
            reinterpret_cast<void*>(static_cast<uintptr_t>(e.selectedResource)),e.selectedSeq,
            reinterpret_cast<void*>(static_cast<uintptr_t>(e.boundResource)),e.boundLatchSeq,e.twinSeq,
            e.candidates,e.boundCandidates,e.continuousCandidates,e.twinCandidates,
            e.writesAtChoice,e.observedWritesAtChoice,e.observedEvictionsAtChoice,
            reinterpret_cast<void*>(static_cast<uintptr_t>(e.drawResource)),e.drawSeq,
            e.writesAtDraw,e.observedWritesAtDraw,e.observedEvictionsAtDraw,
            static_cast<unsigned long long>(e.drawVsHash),
            double(e.selectedDrawOffset[0]),double(e.selectedDrawOffset[1]),double(e.selectedDrawOffset[2]),
            double(e.selectedDrawRotationDeg),
            double(e.drawTranslation[0]),double(e.drawTranslation[1]),double(e.drawTranslation[2]),
            double(e.drawRotationDeg),double(e.selectedProj[0]),double(e.selectedProj[1]),
            double(e.drawProj[0]),double(e.drawProj[1]),
            double(e.selectedRows[0]),double(e.selectedRows[1]),double(e.selectedRows[2]),double(e.selectedRows[3]),
            double(e.selectedRows[4]),double(e.selectedRows[5]),double(e.selectedRows[6]),double(e.selectedRows[7]),
            double(e.selectedRows[8]),double(e.selectedRows[9]),double(e.selectedRows[10]),double(e.selectedRows[11]),
            double(e.drawRows[0]),double(e.drawRows[1]),double(e.drawRows[2]),double(e.drawRows[3]),
            double(e.drawRows[4]),double(e.drawRows[5]),double(e.drawRows[6]),double(e.drawRows[7]),
            double(e.drawRows[8]),double(e.drawRows[9]),double(e.drawRows[10]),double(e.drawRows[11]));
    }
    Log::get().note("--- end temporal submission history ---");
}

// Engine motion's diagnostics (the 2026-09-23 performance review, item 1):
// the emit's census runs only while something reads it --
// advanced.temporal_aa_diagnostics or an eye run (from its arming to the
// first config poll after it is written). Engine-record velocity holds its
// own hooks and needs none of it. (The legacy kinematic tracker that also
// ran here retired on 2026-09-23.)
static void applyEngineMotionDiagnostics() {
    const bool on = detail::g_temporalPassWantedFssChrome &&
                    (g_diagnostics || g_eyeRunLeft > 0 || g_eyeRunReady);
    engineVelocityDiagnostics(on);
}

void temporalPassConfigure(Config& cfg) {
    // Stage 0 price report: every call (both its call sites) may change a
    // live temporal_aa_* setting, so every call closes the report's current
    // window regardless of whether the values read back the same.
    ++g_configGeneration;
    const std::string mode = cfg.getString("fix.temporal_aa", "off");
    const bool wasWanted = detail::g_temporalPassWantedFssChrome;
    detail::g_temporalPassWantedFssChrome = temporalModeEnabled(mode);
    if (detail::g_temporalPassWantedFssChrome != wasWanted) {
        memset(g_rowsObserved, 0, sizeof(g_rowsObserved));
        g_rowsObservedWrites = 0;
        g_rowsObservedEvictions = 0;
        g_rowsChoice = RowsChoiceCapture{};
        for (int eye = 0; eye < 2; ++eye) {
            g_rigidDraw[eye] = RigidDrawRows{};
            g_prevRigidDraw[eye] = RigidDrawRows{};
        }
        g_boundLatchSeq = 0;
        g_boundLatchValid = false;
        g_boundLatchRowsValid = false;
    }
    // The same test native_temporal.cpp's Settings::dlaa makes (flags bit 1
    // at the treat): only an external engine (dlaa, dlss, fsr) touches NGX
    // or FSR, so only they warm one.
    g_trainedWanted = temporalExternalEngine(mode);
    const edvr::TemporalEngine engineBefore = g_temporalEngine;
    g_temporalEngine = temporalEngineFor(mode);
    if (g_temporalEngine != engineBefore) {
        // Both, not just the one being switched to: whichever engine the
        // commander lands on must be able to say why it refuses, and a
        // refusal already printed under the old engine must not be trusted
        // to describe the new one (the review of 2026-09-16, F1). Freeing
        // the engine being LEFT is NOT done here -- this runs on whatever
        // thread reloaded the config; the treat does it (g_engineRan).
        g_dlaaFailNoted = false;
        g_fsrFailNoted = false;
    }
    // The two per-draw hooks inside the game's own passes can be stood down
    // on their own while the mode stays on, so one flight can price them
    // against the frame (the terrain frame-time arc, 2026-09-17). Off leaves
    // the camera's motion on their pixels, which ghosts on a moving body,
    // which is the point: a lever, not a setting to fly with.
    // Engine-record velocity (docs/kinematic-motion-injection-2026-09-19.md)
    // is part of fix.temporal_aa in every mode, with no key of its own (the
    // separate key retired 2026-09-23): the shared kinematic eval hook set, the
    // emit bracket writing each rig record's previous pose into the pool, the
    // pool families' own draws recording slot and depth, and the compose
    // taking the record's exact motion there. It arms and disarms with the
    // mode, live; a hook that cannot install stands it down whole, logged.
    const bool engineMotionOn = runtimeFlatProfile()
        ? temporalModeEnabled(cfg.requestedTemporalMode()) : detail::g_temporalPassWantedFssChrome;
    celestialMotionConfigure(detail::g_temporalPassWantedFssChrome && cfg.getBool("advanced.terrain_motion", true));
    // The emit holds the shared eval hooks itself; the emit's census is
    // diagnostic-only (applyEngineMotionDiagnostics, below the debug mode's
    // read).
    engineVelocityConfigure(engineMotionOn);
    // The scheduler stack-capture probe (docs/engine-render-pipeline.md
    // stage 0): read-only return-address signatures at the four
    // scheduler-fed worker entries, naming the frame scheduler the vtable
    // tables hide from static RE. Independent of the temporal pass fixes:
    // it observes the engine, not the renderer, so it arms on its own key.
    schedulerStackProbeConfigure(cfg.getBool("advanced.scheduler_probe", false));
    // The static prop gate (docs/engine-render-performance-2026-09-19.md,
    // 2026-09-21 design entry): change-gated render-data updates at the
    // job-0 entry. Default off; Phase 1 build, the Phase-2 flight must show
    // census draw-count equality before this can default on.
    staticPropGateConfigure(cfg.getBool("fix.static_prop_updates", false));
    const std::string cur = cfg.getString("advanced.temporal_aa_current", "filtered");
    g_filterCurrent = _stricmp(cur.c_str(), "raw") != 0;
    float c = cfg.getFloat("advanced.temporal_aa_history_sharp", 0.5f);
    if (!std::isfinite(c)) c = 0.5f;
    if (c < 0.5f) c = 0.5f;
    if (c > 1.0f) c = 1.0f;
    g_historyC = c;
    float ship = cfg.getFloat("advanced.temporal_aa_ship_metres", kTemporalShipMetres);
    if (!std::isfinite(ship) || ship < 0.0f) ship = 0.0f;
    if (ship > 100000.0f) ship = 100000.0f;
    g_shipMetres = ship;
    g_eyeRunTreated = cfg.getBool("advanced.eye_run_treated", false);
    g_eyeRunPaired = cfg.getBool("advanced.eye_run_paired", true);
    g_diagnostics = cfg.getBool("advanced.temporal_aa_diagnostics", false);
    g_warmOn = cfg.getBool("advanced.temporal_aa_warm", true);   // g_warmState is NOT re-armed here
    const std::string dbg = cfg.getString("advanced.temporal_aa_debug", "off");
    g_debugMode = _stricmp(dbg.c_str(), "motion") == 0 ? 1 : _stricmp(dbg.c_str(), "error") == 0 ? 2
                : _stricmp(dbg.c_str(), "depth") == 0 ? 3
                : _stricmp(dbg.c_str(), "motion_source") == 0 ? 6 : 0;   // 4 and 5 retired 2026-09-23
    applyEngineMotionDiagnostics();
    float menu = cfg.getFloat("advanced.temporal_aa_menu_metres", 0.0f);
    if (!std::isfinite(menu) || menu < 0.0f) menu = 0.0f;
    if (menu > 50.0f) menu = 50.0f;
    g_menuMetres = menu;
    // Tier 1's mover mask (docs/per-object-motion.md). The tolerance is a
    // percent of depth in the ini and a fraction here; bounded below where
    // the reprojection's own noise would fire it everywhere and above where
    // nothing could ever trip it. All three live.
    const std::string movers = cfg.getString("experimental.temporal_aa_movers", "off");
    g_moversOn = _stricmp(movers.c_str(), "on") == 0;
    float tol = cfg.getFloat("advanced.temporal_aa_movers_tolerance", 3.0f);
    if (!std::isfinite(tol)) tol = 3.0f;
    if (tol < 0.5f) tol = 0.5f;
    if (tol > 25.0f) tol = 25.0f;
    g_moversTol = tol / 100.0f;
    float strength = cfg.getFloat("advanced.temporal_aa_movers_strength", 1.0f);
    if (!std::isfinite(strength)) strength = 1.0f;
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;
    g_moversStrength = strength;
    // DLSS where you look (docs/performance.md feature 6). Either a width in
    // degrees of visual angle (0 is the whole frame, today's behaviour;
    // bounded below by a size worth cropping and above at a full
    // hemisphere), or the word "edges": the crop is then the headset's own
    // field of view trimmed on each side by the three keys below instead of
    // a width. A non-numeric value other than "edges" is off, same as 0.
    // Live either way: the branch reads g_foveaDeg/g_foveaEdges each frame.
    const std::string foveaStr = cfg.getString("advanced.temporal_aa_fovea", "0");
    const bool foveaEdges = _stricmp(foveaStr.c_str(), "edges") == 0;
    float fov = 0.0f;
    if (!foveaEdges) {
        char* foveaEnd = nullptr;
        const float parsed = strtof(foveaStr.c_str(), &foveaEnd);
        if (foveaEnd != foveaStr.c_str() && std::isfinite(parsed) && parsed > 0.0f) fov = parsed;
    }
    if (fov > 0.0f && fov < 10.0f) fov = 10.0f;   // below this the fovea is not worth the seam
    if (fov > 120.0f) fov = 120.0f;
    g_foveaEdges = foveaEdges;
    g_foveaDeg = foveaEdges ? 0.0f : fov;
    // A live reload re-logs the ENGAGED line with the new numbers -- these
    // latches only ever mean "already logged", never "already engaged".
    g_foveaNoted = false;
    g_foveaEdgesNoted[0] = g_foveaEdgesNoted[1] = false;
    g_foveaSizeNoted[0] = g_foveaSizeNoted[1] = false;
    // Edges mode's three trims: plain degrees (not a per-headset list, unlike
    // fix.fov_trim_vertical/_outer/_nasal, which this reduces against -- see
    // cropOf/computeFoveaRegion in temporal_pass.cpp). 0..45, default 0.
    float vertTrim = cfg.getFloat("advanced.temporal_aa_fovea_vertical", 0.0f);
    if (!std::isfinite(vertTrim) || vertTrim < 0.0f) vertTrim = 0.0f;
    if (vertTrim > 45.0f) vertTrim = 45.0f;
    g_foveaVerticalDeg = vertTrim;
    // advanced.temporal_aa_fovea_top/_bottom: "same" (or empty) means this
    // edge follows temporal_aa_fovea_vertical above; any other value is
    // plain degrees, parsed and floored/capped exactly like vertical/outer/
    // nasal. Splits the combined vertical trim so the two edges can differ;
    // temporal_aa_fovea_vertical alone still sets both when neither is set.
    auto sameOrDegrees = [&](const std::string& s) {
        if (s.empty() || _stricmp(s.c_str(), "same") == 0) return g_foveaVerticalDeg;
        char* end = nullptr;
        float parsed = strtof(s.c_str(), &end);
        if (!std::isfinite(parsed) || parsed < 0.0f) parsed = 0.0f;
        if (parsed > 45.0f) parsed = 45.0f;
        return parsed;
    };
    g_foveaTopDeg = sameOrDegrees(cfg.getString("advanced.temporal_aa_fovea_top", "same"));
    g_foveaBottomDeg = sameOrDegrees(cfg.getString("advanced.temporal_aa_fovea_bottom", "same"));
    float outerTrim = cfg.getFloat("advanced.temporal_aa_fovea_outer", 0.0f);
    if (!std::isfinite(outerTrim) || outerTrim < 0.0f) outerTrim = 0.0f;
    if (outerTrim > 45.0f) outerTrim = 45.0f;
    g_foveaOuterDeg = outerTrim;
    float nasalTrim = cfg.getFloat("advanced.temporal_aa_fovea_nasal", 0.0f);
    if (!std::isfinite(nasalTrim) || nasalTrim < 0.0f) nasalTrim = 0.0f;
    if (nasalTrim > 45.0f) nasalTrim = 45.0f;
    g_foveaNasalDeg = nasalTrim;
    float edge = cfg.getFloat("advanced.temporal_aa_fovea_edge", 6.0f);
    // Floored at 1 deg when the fovea is on: DLSS treats the crop's edge as the
    // image edge (clamped taps, no history beyond it), so a hard seam lets its
    // border artefacts in at full weight (the review of 2026-09-05, F11).
    if (!std::isfinite(edge) || edge < 1.0f) edge = 1.0f;
    if (edge > 30.0f) edge = 30.0f;
    g_foveaEdgeDeg = edge;
    // THE HEAD LEAD: how many FRAMES of the centre's own motion the crop leads
    // a turn of the head or the ship by, so the strip entering at its leading
    // edge -- with no crop-local NVIDIA history, and soft for its first frames
    // -- sits further out. Frames, not seconds, because that is the unit DLSS
    // converges in. 0 is off; above a dozen frames the rectangle would be
    // somewhere the pilot is not looking. Live, and a change forgets the
    // slide's state (the crop must never slide against a base from the old
    // setting).
    float lead = cfg.getFloat("advanced.temporal_aa_fovea_lead", 0.0f);
    if (!std::isfinite(lead) || lead < 0.0f) lead = 0.0f;
    if (lead > 12.0f) lead = 12.0f;
    g_foveaLeadFrames = lead;
    g_foveaLeadNoted = false;
    foveaLeadForget(0);
    foveaLeadForget(1);
    float calm = cfg.getFloat("advanced.temporal_aa_periphery_calm", 0.4f);
    if (!std::isfinite(calm) || calm < 0.0f) calm = 0.0f;
    if (calm > 1.0f) calm = 1.0f;
    g_peripheryCalm = calm;
    // The periphery around the fovea: steady (NVIDIA's DLAA on a reduced copy
    // of the frame, the default) or sharp (the pass's own history at full
    // size). dlaa and own are the mechanism names, kept as silent aliases.
    const std::string per = cfg.getString("advanced.temporal_aa_periphery", "steady");
    g_periphSteady = !(_stricmp(per.c_str(), "sharp") == 0 || _stricmp(per.c_str(), "own") == 0);
    float pscale = cfg.getFloat("advanced.temporal_aa_periphery_scale", 0.5f);
    if (!std::isfinite(pscale)) pscale = 0.5f;
    if (pscale < 0.25f) pscale = 0.25f;
    if (pscale > 1.0f) pscale = 1.0f;
    g_periphScale = pscale;
    const std::string shape = cfg.getString("advanced.temporal_aa_fovea_shape", "round");
    g_foveaRound = _stricmp(shape.c_str(), "square") != 0;
    // Where the two eyes' discs meet in depth: 0 is infinity (the straight-ahead
    // point, the flown behaviour); bounded below at arm's length.
    float dist = cfg.getFloat("advanced.temporal_aa_fovea_distance", 0.0f);
    if (!std::isfinite(dist) || dist < 0.0f) dist = 0.0f;
    if (dist > 0.0f && dist < 0.3f) dist = 0.3f;
    if (dist > 1000.0f) dist = 1000.0f;
    g_foveaDistance = dist;
    // K is the default in every mode. The legacy "steady" alias uses K for the full
    // frame and the periphery in every mode, L for the fovea crop when it
    // upscales (the desk found L converging fastest from fresh content and
    // softening least under motion, which is what a crop needs, and priced
    // it: on the full frame the models cost the same, under Performance L
    // costs 1.8x K, on a crop the difference is hundredths of a millisecond);
    // quality = K everywhere; responsive = J everywhere (NVIDIA: slightly less
    // ghosting, a little more flicker); auto = the driver's own choice per
    // mode (K for DLAA, Quality and Balanced, M for Performance, L for Ultra
    // Performance). The letters are silent aliases for one model everywhere
    // (L and M are never applied under DLAA: five times the price). Live: a
    // change recreates the features.
    const std::string model = cfg.getString("fix.temporal_aa_model", "k");
    const auto preset = temporalPresetFor(model);
    // The letters stop here, and deliberately. NVSDK_NGX_DLSS_Hint_Render_
    // Preset (nvsdk_ngx_defs.h, DLSS SDK 310.4) has no A, B, C or D at all
    // -- they were removed, with the header saying to use J or K instead --
    // and it marks G, H, I, N and O as reverting to default behaviour if
    // asked for. E and F are deprecated; this UI exposes J, K, L and M.
    // A tidier-looking letter range would offer four presets that no
    // longer exist.
    if (!preset.known) {
        // Never silently fall through to the default: this branch has
        // been bitten four times by a setting whose effective state was
        // not printed.
        static bool modelWarned = false;
        if (!modelWarned) {
            modelWarned = true;
            Log::get().note(
                "temporal aa: fix.temporal_aa_model = \"%s\" is not a model this build knows "
                "(k, j, l, m, auto, or legacy steady/quality/responsive). Running preset K.",
                model.c_str());
        }
    }
    // Flat owns model changes at its Present boundary, together with history
    // reset and preflight. A reload must not change its feature mid-frame.
    if (!runtimeFlatProfile()) dlaaSetPreset(preset.full, preset.fovea);
    // A config reload re-arms the fovea after a failure stood it down (F3):
    // the user may have changed the width, or the transient may be gone.
    g_foveaFailed = false;
}

// The NGX warm-up, once per session, from the frame boundary
// (docs/intro-video.md, 2026-09-15). The first submitted frame paid 857 ms
// on the Steam rig, most of it NVIDIA's own one-time initialisation and the
// two features' creation; the tick reaches here on every loading-screen
// Present, about two seconds before that frame, on the same thread and
// device the treat will use. Every gate is a cheap CPU check that fails
// CLOSED to today's behaviour: the first submitted frame initialises NVIDIA
// itself, as it always did, and warmNoteFirstTreat says why the warm-up
// never got there. Off, Done, Failed and Late are terminal for the session.
void warmFailNote(double ms) {   // L-FAIL: one string, whichever gate or refusal it was
    const bool amdEngine = g_temporalEngine == edvr::TemporalEngine::Amd;
    Log::get().note(
        "temporal aa: %s warm-up did not complete after %.0f ms -- %s. The first submitted "
        "frame runs as before: a refusal by %s is remembered for the session and the '%s "
        "was asked for, but' line repeats why, while a failed feature create is retried there.",
        amdEngine ? "AMD" : "NVIDIA", ms, g_warmWhy, amdEngine ? "AMD" : "NVIDIA",
        amdEngine ? "fsr" : "dlaa");
}
void warmTrainedOnce(ID3D11DeviceContext* ctx) {
    if (g_warmState != WarmState::Pending || !ctx) return;                              // G1
    if (!g_trainedWanted) {                                                             // G2
        g_warmWhy = "fix.temporal_aa was not an external engine (dlaa, dlss or fsr) when the "
                    "warm-up first ran; a reload does not re-arm it";
        g_warmState = WarmState::Off;
        return;
    }
    if (!g_warmOn) {                                                                    // G3
        g_warmWhy = "advanced.temporal_aa_warm = 0";
        g_warmState = WarmState::Off;
        if (!g_warmOffNoted) {
            g_warmOffNoted = true;
            const bool amdEngine = g_temporalEngine == edvr::TemporalEngine::Amd;
            Log::get().note(
                "temporal aa: %s warm-up is off (advanced.temporal_aa_warm = 0); the first "
                "submitted frame initialises %s itself, as before.",
                amdEngine ? "AMD" : "NVIDIA", amdEngine ? "AMD" : "NVIDIA");
        }
        return;
    }
    if (g_debugMode != 0) {                                                             // G4
        g_warmWhy = "advanced.temporal_aa_debug is on";
        g_warmState = WarmState::Off;
        return;
    }
    // The size the game will render both eyes at: the runtime's recommended
    // per-eye size, which GetRecommendedRenderTargetSize answers as the max
    // over both eyes (src/openxr/openvr_system.cpp). Published at Init by the
    // openxr half; a mutex and an 80-byte copy per poll until it is.
    EdvrNativeRenderSizing s{};                                                         // G5
    if (!edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1, sizeof(s), &s) ||
        s.valid != 1 || !s.activeWidth[0] || !s.activeHeight[0] || !s.activeWidth[1] ||
        !s.activeHeight[1]) {
        g_warmWhy = "openvr_api.dll had not published a native render size";
        return;   // Pending: polled again next tick
    }
    const uint32_t w = s.activeWidth[0] > s.activeWidth[1] ? s.activeWidth[0] : s.activeWidth[1];
    const uint32_t h = s.activeHeight[0] > s.activeHeight[1] ? s.activeHeight[0] : s.activeHeight[1];
    ID3D11Device* chanDev = nullptr;
    unsigned long chanThread = 0;
    if (!nativeTemporalWarmTarget(&chanDev, &chanThread) || !chanDev) {                // G6
        g_warmWhy = "the native temporal channel was not acquired (a flat session, the OpenVR "
                    "path, or VR still starting)";
        return;   // Pending
    }
    const unsigned long here = GetCurrentThreadId();
    if (here != chanThread) {                                                           // G7
        snprintf(g_warmWhyBuf, sizeof(g_warmWhyBuf),
                 "this frame boundary is on thread %lu but the native channel was acquired on %lu",
                 here, chanThread);
        g_warmWhy = g_warmWhyBuf;
        return;   // Pending
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) {
        g_warmWhy = "the frame boundary's device is not the native channel's game device";
        return;   // Pending
    }
    {                                                                                   // G8
        IUnknown* a = nullptr;
        IUnknown* b = nullptr;
        const bool same = SUCCEEDED(dev->QueryInterface(IID_IUnknown, (void**)&a)) &&
                          SUCCEEDED(chanDev->QueryInterface(IID_IUnknown, (void**)&b)) && a == b;
        if (a) a->Release();
        if (b) b->Release();
        if (!same) {
            dev->Release();
            g_warmWhy = "the frame boundary's device is not the native channel's game device";
            return;   // Pending
        }
    }
    const HRESULT removed = dev->GetDeviceRemovedReason();
    dev->Release();
    if (FAILED(removed)) {                                                              // G9
        snprintf(g_warmWhyBuf, sizeof(g_warmWhyBuf), "the device is removed (0x%08X)",
                 static_cast<unsigned>(removed));
        g_warmWhy = g_warmWhyBuf;
        g_warmState = WarmState::Failed;
        warmFailNote(0.0);
        return;
    }
    if (w == 0 || h == 0 || w > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        h > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {                                     // G10
        snprintf(g_warmWhyBuf, sizeof(g_warmWhyBuf),
                 "the published render size %ux%u is out of range", w, h);
        g_warmWhy = g_warmWhyBuf;
        g_warmState = WarmState::Failed;
        warmFailNote(0.0);
        return;
    }
    // COMMIT. Failed before the call: a fault must never retry. Done only on
    // a whole success. G11 (no SDK in the build, NGX refusing, a create
    // refusing) is dlaaWarm's own reason.
    g_warmState = WarmState::Failed;
    const bool amdEngine = g_temporalEngine == edvr::TemporalEngine::Amd;
    // The fovea (a width or edges) makes its own crop features at the
    // treat -- but it is NVIDIA-only (design doc 3.1), so under fsr there
    // is no crop to defer and the full warm always runs.
    const bool features = amdEngine || !foveaConfigured();
    if (amdEngine) {
        Log::get().note(
            "temporal aa: warming AMD's FSR before the first submitted frame -- %ux%u in and "
            "out for both eyes on the render thread. If this is the log's last line the "
            "warm-up hung: set advanced.temporal_aa_warm = 0 and report this log.",
            w, h);
    } else {
        Log::get().note(
            "temporal aa: warming NVIDIA before the first submitted frame -- %ux%u in and out (DLAA) "
            "for both eyes on the render thread%s. If this is the log's last line the warm-up hung: "
            "set advanced.temporal_aa_warm = 0 and report this log.",
            w, h, features ? "" : ", initialisation only: advanced.temporal_aa_fovea is on");
    }
    bool ok = false;
    double initMs = 0.0;
    double createMs[2] = {0.0, 0.0};
    const char* why = "";
    // Told apart from a fault so the L-FAULT line does not claim a fresh
    // crash when this feature's shared budget (g_budget, spent by whatever
    // else in this pass has faulted this session) was already at zero:
    // guardedBudget returns false either way, without running the lambda
    // when the budget was already spent.
    const bool budgetAlreadySpent = !g_budget.shouldRun();
    const int64_t t0 = qpcNow();
    const bool survived = guardedBudget(g_budget, [&] {
        if (amdEngine) {
            // 1:1, and it cannot be anything else here. FSR keys its context
            // on all four sizes, and at this moment only ONE of them is
            // known: w x h is the runtime's recommended per-eye size, which
            // is exactly the OUTPUT size the treat will ask for (native_
            // temporal.cpp's outW/outH are s->recW/recH). The RENDER size is
            // Elite's own HMD Quality applied to that, and the game has not
            // submitted a frame yet, so nothing here has it. (The launch-time
            // figure deviceHookAutoBiasSource reports is Elite's
            // HMDRenderTargetMultiplier, not a measured size: the pass's own
            // check at the first engaged frame exists precisely because the
            // two can disagree, and a key wrong by one pixel costs the same
            // rebuild as no warm-up at all, plus the VRAM.) So under an
            // upscale the first submitted frame remakes both contexts, and
            // the log says so at both ends -- here, and in fsr3_engine.cpp's
            // "the context for eye N is remade" line.
            ok = edvr::fsr3Warm(ctx, w, h, w, h, &initMs, &why);
        } else {
            ok = dlaaWarm(ctx, w, h, features, &initMs, createMs, &why);
        }
    });
    const double totalMs = qpcFrequency() > 0
                               ? static_cast<double>(qpcNow() - t0) * 1000.0 /
                                     static_cast<double>(qpcFrequency())
                               : 0.0;
    // Hygiene, whatever happened: the slots the treat unbinds after its own
    // dispatches, so the game's next loading frame inherits nothing NGX may
    // have bound.
    {
        ID3D11UnorderedAccessView* nullUav[7] = {};
        ID3D11ShaderResourceView* nullSrv[19] = {};
        ctx->CSSetShader(nullptr, nullptr, 0);
        ctx->CSSetUnorderedAccessViews(0, 7, nullUav, nullptr);
        ctx->CSSetShaderResources(0, 19, nullSrv);
    }
    if (survived && ok) {
        g_warmState = WarmState::Done;
        if (amdEngine) {
            Log::get().note(
                "temporal aa: AMD's FSR warmed before the first submitted frame -- %.0f ms at "
                "%ux%u -> %ux%u. That key is 1:1 because the render size is not known until the "
                "game submits: at HMD Quality 1 the first submitted frame finds this context "
                "made and 'native timing CPU: seq' should show temporal well under 150 ms, and "
                "BELOW HMD Quality 1 it does not -- the first upscaled frame remakes both "
                "contexts at the real key and says so ('the context for eye N is remade'), so "
                "that frame still pays for a create.",
                initMs, w, h, w, h);
        } else if (features) {
            Log::get().note(
                "temporal aa: NVIDIA warmed before the first submitted frame -- initialisation "
                "%.0f ms, the feature for eye 0 %.0f ms and eye 1 %.0f ms at %ux%u -> %ux%u, "
                "%.0f ms in all; the first submitted frame finds them made, so 'native timing "
                "CPU: seq' should show temporal well under 150 ms.",
                initMs, createMs[0], createMs[1], w, h, w, h, totalMs);
        } else {
            Log::get().note(
                "temporal aa: NVIDIA warmed its initialisation only (%.0f ms): "
                "advanced.temporal_aa_fovea is on, and its crop features are made at the first "
                "submitted frame, as before.",
                initMs);
        }
    } else if (survived) {
        g_warmWhy = (why && *why) ? why : (amdEngine ? "fsr3Warm refused without a reason"
                                                      : "dlaaWarm refused without a reason");
        warmFailNote(totalMs);
    } else if (budgetAlreadySpent) {
        g_warmWhy = "not attempted: the pass's fault budget was already spent";
        Log::get().note(
            "temporal aa: %s warm-up not attempted -- the pass's fault budget (%s) was "
            "already spent by something else this session. The first submitted frame runs as "
            "before: a refusal by %s is remembered for the session and the '%s was asked "
            "for, but' line repeats why, while a failed feature create is retried there.",
            amdEngine ? "AMD" : "NVIDIA", g_budget.name(), amdEngine ? "AMD" : "NVIDIA",
            amdEngine ? "fsr" : "dlaa");
    } else {
        g_warmWhy = "the warm-up faulted; see the fault report";
        Log::get().note(
            "temporal aa: %s warm-up faulted after %.0f ms and is not retried. The first "
            "submitted frame runs as before: a refusal by %s is remembered for the session "
            "and the '%s was asked for, but' line repeats why, while a failed feature create "
            "is retried there. Please report this log.",
            amdEngine ? "AMD" : "NVIDIA", totalMs, amdEngine ? "AMD" : "NVIDIA",
            amdEngine ? "fsr" : "dlaa");
    }
}

void temporalPassTick(ID3D11DeviceContext* ctx) {
    if (!ctx || (!detail::g_temporalPassWantedFssChrome && !g_eyeRunReady)) return;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    const bool accepted = acceptPassDevice(dev);
    if (dev) dev->Release();
    if (!accepted) return;
    if (g_eyeRunReady && ctx) {
        writeEyeRun(ctx, g_eyeRunWidth, g_eyeRunHeight);
        g_eyeRunReady = false;
    }
    if (!detail::g_temporalPassWantedFssChrome || !ctx) return;
    // Create both precompiled variants during warm-up so arming an eye dump
    // does not introduce shader creation work in the captured head movement.
    motionShader(ctx, false);
    motionShader(ctx, true);
    if (!g_cs && !g_csTried) {
        g_csTried = true;
        g_cs = createShader(ctx);
        if (g_cs && !g_warmNoted) {
            g_warmNoted = true;
            Log::get().note(
                "temporal aa: precompiled shader warmed at session start; "
                "no runtime HLSL compilation.");
        }
    }
    // NVIDIA's own warm-up, once its gates open (the published render size,
    // the acquired native channel on this thread and device); a no-op after.
    warmTrainedOnce(ctx);
}

void temporalPassNoteSceneWrite(const void* res, const void* data, uint32_t bytes) {
    // The true view matrix lives at float offset 932 of the big scene
    // block (measured by the sun-glare fix's two-shot dump: three 3x4
    // rows, rotation plus translation). Every write of every block of
    // that size goes into the ring, stamped with the frame and the order;
    // chooseCameraRows picks the frame's at its first treat. Kept only
    // when the rows are a rotation.
    if (!detail::g_temporalPassWantedFssChrome || !data || bytes < 944 * 4) return;
    const float* f = static_cast<const float*>(data) + 932;
    const float* pz = static_cast<const float*>(data) + 792;
    const bool rowsValid = temporalRowsAreRotation(f);
    ++g_rowsObservedWrites;
    const uint32_t observedSeq = g_rowsObservedSeq++;
    ObservedRowsWrite* observed = res ? observedRowsSlot(res) : nullptr;
    if (observed) {
        observed->frame = g_rowsFrame;
        observed->seq = observedSeq;
        observed->rowsValid = rowsValid;
        if (rowsValid) {
            memcpy(observed->rows, f, sizeof(observed->rows));
            observed->proj[0] = pz[2];
            observed->proj[1] = pz[3];
        }
    }
    // The production chooser keeps its existing contract: only rotations enter
    // its ring. The diagnostic record above is deliberately wider so a rejected
    // overwrite before a draw is reported as such, rather than reviving an
    // earlier valid write from the same buffer.
    if (!rowsValid) return;
    ++g_rowsWrites;
    RowsWrite& w = g_rowsRing[g_rowsSeq % kRowsRing];
    w.buf = res;
    memcpy(w.rows, f, sizeof(w.rows));
    // The projection's z row sits at row 198 of the same block (float
    // offset 792), read the column-vector way the x and y rows' off-centre
    // terms confirm: clip z = A * view z + B with the view z as clip w, so
    // the depth the game writes is A + B / z. On build 332841 it reads
    // A = 0, B = 0.025 -- reversed-Z with NO far plane, depth = 0.025 / z.
    // The 0.025..50000 m the game asks the runtime for is the runtime's
    // projection, not this one, and decoding with those planes read 10 km
    // as 8.3 km (2026-09-08 19:52: the body's grid missed the station
    // beyond a few hundred metres for it).
    w.proj[0] = pz[2];
    w.proj[1] = pz[3];
    w.frame = g_rowsFrame;
    w.seq = g_rowsSeq;
    w.observedSeq = observedSeq;
    w.valid = true;
    ++g_rowsSeq;
}

void temporalPassNoteFirstEyeDraw(ID3D11DeviceContext* ctx) {
    if (!detail::g_temporalPassWantedFssChrome || g_curLatched) return;
    g_curLatched = true;
    g_boundSeen = false;
    g_boundBuf = nullptr;
    g_boundLatchSeq = 0;
    g_boundLatchValid = false;
    g_boundLatchRowsValid = false;
    if (!ctx) return;
    // The block bound at the scene's first draw: the vertex stage's
    // constant buffers first, then the pixel stage's, the lowest slot
    // holding an object the ring has a write for. Two queries a frame.
    // Only the OBJECT is kept: which of its writes is the frame's camera
    // is chooseCameraRows's question, answered by continuity.
    ID3D11Buffer* vs[8] = {};
    ID3D11Buffer* ps[8] = {};
    ctx->VSGetConstantBuffers(0, 8, vs);
    ctx->PSGetConstantBuffers(0, 8, ps);
    auto inRing = [&](const void* b) {
        if (!b) return false;
        for (int i = 0; i < kRowsRing; ++i) {
            if (g_rowsRing[i].valid && g_rowsRing[i].buf == b) return true;
        }
        return false;
    };
    for (int i = 0; i < 8 && !g_boundSeen; ++i) {
        if (inRing(vs[i])) { g_boundBuf = vs[i]; g_boundSeen = true; g_latchSlotVs = i; }
    }
    for (int i = 0; i < 8 && !g_boundSeen; ++i) {
        if (inRing(ps[i])) { g_boundBuf = ps[i]; g_boundSeen = true; g_latchSlotPs = i; }
    }
    if (g_boundSeen) {
        const ObservedRowsWrite* observed = observedRowsCurrent(g_boundBuf);
        if (observed) {
            g_boundLatchValid = true;
            g_boundLatchRowsValid = observed->rowsValid;
            g_boundLatchSeq = observed->seq;
        }
    }
    for (int i = 0; i < 8; ++i) {
        if (vs[i]) vs[i]->Release();
        if (ps[i]) ps[i]->Release();
    }
}

bool temporalPassWantsRigidDraw(int eye) {
    if (!detail::g_temporalPassWantedFssChrome || eye < 0 || eye > 1) return false;
    return !g_rigidDraw[eye].seen || g_rigidDraw[eye].frame != g_rowsFrame;
}

void temporalPassNoteRigidDraw(int eye, const void* resource, uint64_t vertexShaderHash) {
    if (!temporalPassWantsRigidDraw(eye)) return;
    RigidDrawRows sample{};
    sample.seen = true;
    sample.frame = g_rowsFrame;
    sample.resource = resource;
    sample.vsHash = vertexShaderHash;
    sample.writesAtDraw = g_rowsWrites;
    sample.observedWritesAtDraw = g_rowsObservedWrites;
    sample.observedEvictionsAtDraw = g_rowsObservedEvictions;
    const ObservedRowsWrite* observed = observedRowsCurrent(resource);
    if (observed) {
        sample.observed = true;
        sample.valid = observed->rowsValid;
        sample.seq = observed->seq;
        if (sample.valid) {
            memcpy(sample.rows, observed->rows, sizeof(sample.rows));
            memcpy(sample.proj, observed->proj, sizeof(sample.proj));
        }
    }
    g_rigidDraw[eye] = sample;
}

void temporalPassNoteHead(int eye, const float* prevPose, const float* nowPose,
                          const float* eyeOffset) {
    if (eye < 0 || eye > 1) return;
    EyeState& e = g_eye[eye];
    e.headNoted = false;
    if (!prevPose || !nowPose || !eyeOffset) return;
    memcpy(e.headPrev, prevPose, sizeof(e.headPrev));
    memcpy(e.headNow, nowPose, sizeof(e.headNow));
    memcpy(e.eyeOff, eyeOffset, sizeof(e.eyeOff));
    e.headNoted = true;
}

void temporalPassFrameBoundary() {
    if (!detail::g_temporalPassWantedFssChrome) return;
    for (int eye = 0; eye < 2; ++eye) {
        if (g_rigidDraw[eye].seen && g_rigidDraw[eye].frame == g_rowsFrame) {
            g_prevRigidDraw[eye] = g_rigidDraw[eye];
        } else {
            g_prevRigidDraw[eye] = RigidDrawRows{};
        }
    }
    ++g_rowsFrame;
    g_rowsWritesSum += g_rowsWrites;
    ++g_rowsFramesSum;
    g_rowsWrites = 0;
    g_rowsObservedWrites = 0;
    g_rowsObservedEvictions = 0;
    g_chosenThisFrame = false;
    // This frame's rows become the next frame's reference, the view's own
    // only when a delta to them was measured and neither eye refused it.
    temporalCameraGateAdvance(g_cameraGate, g_curValid);
    if (g_cameraGate.parkedRun > g_camParkedStayMax) g_camParkedStayMax = g_cameraGate.parkedRun;
    if (g_curValid) {
        if (g_prevValid) ++g_camPairs;
        memcpy(g_prevRows, g_curRows, sizeof(g_prevRows));
        g_prevValid = true;
        if (!g_camNoted && g_camPairs >= 2) {
            g_camNoted = true;
            Log::get().note(
                "temporal aa: the game's camera is being read -- the view "
                "rows at float 932 of the scene block, the write of the frame that "
                "follows last frame's (the block bound at the scene's first draw is "
                "at VS b%d / PS b%d, -1 = not seen); they are the full view with the "
                "headset in it, read view->world for the world path.",
                g_latchSlotVs, g_latchSlotPs);
        }
    } else {
        g_prevValid = false;
    }
    g_curLatched = false;
    g_curValid = false;
}


bool temporalPassTotals(uint32_t* treated, double* avgMs, double* maxMs,
                        double* rejectPct, double* clipPct) {
    if (g_treats == 0) return false;
    if (treated) *treated = g_treats;
    if (avgMs) *avgMs = g_timeCount ? g_timeSum / static_cast<double>(g_timeCount) : 0.0;
    if (maxMs) *maxMs = g_timeMax;
    if (g_pixelsSeen == 0) {
        // Not counted: the lean own shader writes no Stats, and NVIDIA's
        // history is never instrumented either. -1 tells callers to say so
        // rather than print a spurious 0%.
        if (rejectPct) *rejectPct = -1.0;
        if (clipPct) *clipPct = -1.0;
    } else {
        const double px = static_cast<double>(g_pixelsSeen);
        if (rejectPct) *rejectPct = 100.0 * static_cast<double>(g_rejected) / px;
        if (clipPct) *clipPct = 100.0 * static_cast<double>(g_clipped) / px;
    }
    return true;
}

void regAppend(char* buf, size_t n, size_t& used, const char* fmt, ...) {
    if (used >= n) return;
    va_list ap;
    va_start(ap, fmt);
    const int m = vsnprintf(buf + used, n - used, fmt, ap);
    va_end(ap);
    if (m > 0) used += static_cast<size_t>(m);
    if (used > n) used = n;
}

bool temporalPassRegistration(char* buf, size_t n, char* buf2, size_t n2, char* buf3, size_t n3) {
    if (buf2 && n2) buf2[0] = 0;
    if (buf3 && n3) buf3[0] = 0;
    if (!buf || n == 0 || g_treats == 0 || g_intervalFrames == 0) return false;
    static const char* const kNames[4] = {"head, rotation only", "depth, eyes swapped",
                                          "world, the rows' delta", "head with depth"};
    size_t used = 0;
    regAppend(buf, n, used, "over the last %u eye-frames: ", g_intervalFrames);
    bool anyCand = false;
    for (int k = 0; k < 4; ++k) if (g_candPix[k]) anyCand = true;
    if (!anyCand) {
        regAppend(buf, n, used, "the candidates were not judged (NVIDIA's history ran, or the "
                                "pass's own had no history yet)");
    } else {
        bool firstCand = true;
        for (int k = 0; k < 4; ++k) {
            if (!g_candPix[k]) continue;   // each prints once it has a delta to judge
            regAppend(buf, n, used, "%s%s ", firstCand ? "" : "; ", kNames[k]);
            firstCand = false;
            const double px = static_cast<double>(g_candPix[k]);
            // The mean clip size over the CLIPPED pixels: how far a clipped
            // history had strayed, 1/255ths of luma.
            const double meanSize = g_candClip[k]
                ? static_cast<double>(g_candSize[k]) / static_cast<double>(g_candClip[k])
                : 0.0;
            regAppend(buf, n, used, "clipped %.1f%% by %.1f/255 on average, off %.1f%%",
                      100.0 * static_cast<double>(g_candClip[k]) / px, meanSize,
                      100.0 * static_cast<double>(g_candRej[k]) / px);
        }
        static const char* const kBuckets[3] = {"still", "slow", "fast"};
        bool anyBucket = false;
        for (int b = 0; b < 3; ++b) if (g_bucketPix[b]) anyBucket = true;
        if (anyBucket) {
            regAppend(buf, n, used,
                      ". The used delta's clip share by head speed (under %.2f, under %.2f, over "
                      "that, degrees per frame): ",
                      static_cast<double>(kStillDeg), static_cast<double>(kSlowDeg));
            for (int b = 0; b < 3; ++b) {
                if (!g_bucketPix[b]) {
                    regAppend(buf, n, used, "%s%s none", b ? ", " : "", kBuckets[b]);
                    continue;
                }
                const double meanSize = g_bucketClip[b]
                    ? static_cast<double>(g_bucketSize[b]) / static_cast<double>(g_bucketClip[b])
                    : 0.0;
                regAppend(buf, n, used, "%s%s %.1f%% by %.1f/255 (%u eye-frames)",
                          b ? ", " : "", kBuckets[b],
                          100.0 * static_cast<double>(g_bucketClip[b]) /
                              static_cast<double>(g_bucketPix[b]),
                          meanSize, g_bucketFrames[b]);
            }
        }
    }
    // The world/ship split's share, the bright pixels without depth, and
    // the camera rows against the head (docked: zero and zero).
    if (g_intervalPix) {
        regAppend(buf, n, used,
                  ". The world path (the camera's delta beyond %.0f m and at the far plane) took "
                  "%.1f%% of pixels; %.1f%% of the bright pixels (luma over 0.6) had no depth",
                  static_cast<double>(g_shipMetres),
                  100.0 * static_cast<double>(g_worldPix) / static_cast<double>(g_intervalPix),
                  g_brightPix ? 100.0 * static_cast<double>(g_brightNoDepthPix) /
                                    static_cast<double>(g_brightPix)
                              : 0.0);
    }
    if (g_camFrames) {
        regAppend(buf, n, used,
                  "; the world delta (the game's view rows, the head in them) differed "
                  "from the head's by %.3f deg/frame on average and the eye moved %.4f "
                  "m/frame in the rows (docked: the first reads 0 whichever way the head "
                  "turns, the second a head's sway)",
                  g_camHeadDiffSum / g_camFrames, g_camMoveSum / g_camFrames);
    }
    if (g_rowsFramesSum) {
        const uint32_t chosen = g_chooseBound + g_chooseOther + g_chooseResync + g_chooseNone +
                                g_chooseRefollow;
        regAppend(buf, n, used,
                  "; the scene block was written %.1f times a frame (%.1f candidates); the rows "
                  "chosen by continuity were the bound block's on %u frames and another's on "
                  "%u, nothing followed last frame's on %u, no write on %u, the bound block's "
                  "taken over a chain that had stopped following the head on %u; the ship's "
                  "delta was carried over a drop on %u frames",
                  static_cast<double>(g_rowsWritesSum) / static_cast<double>(g_rowsFramesSum),
                  chosen ? static_cast<double>(g_candSumCount) / static_cast<double>(chosen) : 0.0,
                  g_chooseBound, g_chooseOther, g_chooseResync, g_chooseNone, g_chooseRefollow,
                  g_camCarried);
    }
    // The second line: the logger caps a line at 1200 characters, and the
    // probes' figures fell off the end of the first (2026-09-04).
    buf = buf2;
    n = buf2 ? n2 : 0;
    used = 0;
    if (g_camDropRot || g_camDropParked || g_camDropMove) {
        regAppend(buf, n, used,
                  "; the camera's delta was dropped on %u eye-frames as another camera's (over 3 "
                  "deg from the head's), on %u as a parked camera's (not turned at all, from "
                  "rows a drop had left; the longest stay %u frames) and its translation on %u "
                  "as a jump (over 50 m); a jump was carried on %u (zero by construction)",
                  g_camDropRot, g_camDropParked, g_camParkedStayMax, g_camDropMove,
                  g_camCarriedJump);
    }
    if (g_worldFloorRefused) {
        regAppend(buf, n, used,
                  "; the world path stood down on the scene's draw floor alone on %u eye-frames "
                  "(last count %u, floor %u)",
                  g_worldFloorRefused, g_worldFloorLast, kTemporalSceneDrawFloor);
    }
    if (g_classWorldPix || g_classShipPix) {
        regAppend(buf, n, used,
                  "; the used delta clipped %.1f%% of the world path's pixels and %.1f%% of the ship's",
                  g_classWorldPix ? 100.0 * static_cast<double>(g_classWorldClip) /
                                        static_cast<double>(g_classWorldPix)
                                  : 0.0,
                  g_classShipPix ? 100.0 * static_cast<double>(g_classShipClip) /
                                       static_cast<double>(g_classShipPix)
                                 : 0.0);
    }
    // Tier 1's mover mask: its share of the interval's pixels. With the
    // head still and the ship docked this should read near zero (what fires
    // then is the reprojection's own noise against the tolerance); in the
    // slot it is the rim's edges and whatever moves past.
    if (g_moversOn && g_intervalPix) {
        regAppend(buf, n, used,
                  "; the mover mask set %.2f%% of pixels (tolerance %.1f%% of depth, strength "
                  "%.2f)",
                  100.0 * static_cast<double>(g_moverPix) / static_cast<double>(g_intervalPix),
                  100.0 * static_cast<double>(g_moversTol), static_cast<double>(g_moversStrength));
    }
    if (g_dlResets) {
        regAppend(buf, n, used,
                  "; NVIDIA's history was reset on %llu eye-frames, %llu of them asked by the openvr half (%llu "
                  "for a hold or a healed frame, %llu for a withheld jump the camera came back from, %llu for "
                  "one left unjudged, %llu for a pose without a delta) and the rest for want of a history",
                  static_cast<unsigned long long>(g_dlResets), static_cast<unsigned long long>(g_dlResetsAsked),
                  static_cast<unsigned long long>(g_dlResetsHeld),
                  static_cast<unsigned long long>(g_dlResetsReturned),
                  static_cast<unsigned long long>(g_dlResetsUnjudged),
                  static_cast<unsigned long long>(g_dlResetsNoDelta));
    }
    // The third line: the probes and the rows against the head.
    buf = buf3;
    n = buf3 ? n3 : 0;
    used = 0;
    if (g_probeSkyN) {
        regAppend(buf, n, used,
                  "; the history's best match sat (%+.2f, %+.2f) px from the prediction on the sky "
                  "(%llu probes)",
                  static_cast<double>(g_probeSkyDx) / 100.0 / static_cast<double>(g_probeSkyN),
                  static_cast<double>(g_probeSkyDy) / 100.0 / static_cast<double>(g_probeSkyN),
                  static_cast<unsigned long long>(g_probeSkyN));
    }
    if (g_probeWorldN || g_probeShipN) {
        regAppend(buf, n, used,
                  "; the history's best match sat (%+.2f, %+.2f) px from the prediction on the world "
                  "with a depth (%llu probes) and (%+.2f, %+.2f) px on the ship (%llu probes) -- a "
                  "steady offset that follows the motion is a lag or a scale, noise averages to zero",
                  g_probeWorldN ? static_cast<double>(g_probeWorldDx) / 100.0 / static_cast<double>(g_probeWorldN) : 0.0,
                  g_probeWorldN ? static_cast<double>(g_probeWorldDy) / 100.0 / static_cast<double>(g_probeWorldN) : 0.0,
                  static_cast<unsigned long long>(g_probeWorldN),
                  g_probeShipN ? static_cast<double>(g_probeShipDx) / 100.0 / static_cast<double>(g_probeShipN) : 0.0,
                  g_probeShipN ? static_cast<double>(g_probeShipDy) / 100.0 / static_cast<double>(g_probeShipN) : 0.0,
                  static_cast<unsigned long long>(g_probeShipN));
    }
    if (g_probeMm[0] || g_probeMm[1] || g_probeMm[2]) {
        auto kOf = [](int c) {
            return g_probeMm[c] ? static_cast<double>(g_probeDot[c]) / static_cast<double>(g_probeMm[c]) : 0.0;
        };
        auto pxOf = [](int c, uint64_t cnt) {
            return cnt ? sqrt(static_cast<double>(g_probeMm[c]) / 100.0 / static_cast<double>(cnt)) : 0.0;
        };
        regAppend(buf, n, used,
                  "; against its own vector the match scaled the motion by 1+k with k = %+.3f on the "
                  "sky (%.1f px rms), %+.3f on the world (%.1f px), %+.3f on the ship (%.1f px) -- k "
                  "under zero: the vector overshot the scene's turn",
                  kOf(0), pxOf(0, g_probeSkyN), kOf(1), pxOf(1, g_probeWorldN), kOf(2), pxOf(2, g_probeShipN));
    }
    if (g_rhN[0] + g_rhN[1] + g_rhN[2] > 0.0) {
        regAppend(buf, n, used,
                  "; the rows' delta sat %.4f deg/frame from the head's still (%.0f frames), %.4f "
                  "slow (%.0f), %.4f fast (%.0f); the rows turned (1+k) times the head, k = %+.3f "
                  "(x %+.3f, y %+.3f, z %+.3f), leading by %+.2f frames",
                  g_rhN[0] ? g_rhSum[0] / g_rhN[0] : 0.0, g_rhN[0],
                  g_rhN[1] ? g_rhSum[1] / g_rhN[1] : 0.0, g_rhN[1],
                  g_rhN[2] ? g_rhSum[2] / g_rhN[2] : 0.0, g_rhN[2],
                  g_rhMm > 0.0 ? g_rhDot / g_rhMm : 0.0,
                  g_rhMmAx[0] > 0.0 ? g_rhDotAx[0] / g_rhMmAx[0] : 0.0,
                  g_rhMmAx[1] > 0.0 ? g_rhDotAx[1] / g_rhMmAx[1] : 0.0,
                  g_rhMmAx[2] > 0.0 ? g_rhDotAx[2] / g_rhMmAx[2] : 0.0,
                  g_rhMmLag > 0.0 ? g_rhDotLag / g_rhMmLag : 0.0);
    }
    if (g_tvFrames) {
        regAppend(buf, n, used,
                  "; with the ship still (%u frames) the rows' translation followed the head's by "
                  "(%+.2f, %+.2f, %+.2f) per axis",
                  g_tvFrames,
                  g_tvMm[0] > 0.0 ? g_tvDot[0] / g_tvMm[0] : 0.0,
                  g_tvMm[1] > 0.0 ? g_tvDot[1] / g_tvMm[1] : 0.0,
                  g_tvMm[2] > 0.0 ? g_tvDot[2] / g_tvMm[2] : 0.0);
    }
    if (g_chooseMulti || g_twinFrames) {
        regAppend(buf, n, used,
                  "; on %u frames a second continuous reading differed from the chosen one, by %.2f "
                  "deg on average and %.2f at most; last frame's rows came again on %u",
                  g_chooseMulti, g_chooseMulti ? g_chooseSpreadSum / g_chooseMulti : 0.0,
                  g_chooseSpreadMax, g_twinFrames);
    }
    // The interval starts afresh: the next line judges the next stretch.
    memset(g_candPix, 0, sizeof(g_candPix));
    memset(g_candRej, 0, sizeof(g_candRej));
    memset(g_candClip, 0, sizeof(g_candClip));
    memset(g_candSize, 0, sizeof(g_candSize));
    memset(g_bucketPix, 0, sizeof(g_bucketPix));
    memset(g_bucketClip, 0, sizeof(g_bucketClip));
    memset(g_bucketSize, 0, sizeof(g_bucketSize));
    memset(g_bucketFrames, 0, sizeof(g_bucketFrames));
    g_intervalFrames = 0;
    g_intervalPix = 0;
    g_worldPix = 0;
    g_brightPix = 0;
    g_brightNoDepthPix = 0;
    g_camHeadDiffSum = 0.0;
    g_camMoveSum = 0.0;
    g_camFrames = 0;
    g_camDropRot = 0;
    g_camDropParked = 0;
    g_camParkedStayMax = 0;
    g_camDropMove = 0;
    g_worldFloorRefused = 0;
    g_rowsWritesSum = 0;
    g_rowsFramesSum = 0;
    g_candSumCount = 0;
    g_chooseBound = 0;
    g_chooseOther = 0;
    g_chooseResync = 0;
    g_chooseRefollow = 0;
    g_chooseNone = 0;
    g_camCarried = 0;
    g_camCarriedJump = 0;
    g_probeWorldDx = g_probeWorldDy = 0;
    g_probeWorldN = 0;
    g_probeShipDx = g_probeShipDy = 0;
    g_probeShipN = 0;
    g_classWorldPix = g_classWorldClip = 0;
    g_classShipPix = g_classShipClip = 0;
    g_moverPix = 0;
    g_dlResets = 0;
    g_dlResetsAsked = 0;
    g_dlResetsHeld = g_dlResetsReturned = g_dlResetsUnjudged = g_dlResetsNoDelta = 0;
    g_probeSkyDx = g_probeSkyDy = 0;
    g_probeSkyN = 0;
    memset(g_probeDot, 0, sizeof(g_probeDot));
    memset(g_probeMm, 0, sizeof(g_probeMm));
    memset(g_rhN, 0, sizeof(g_rhN));
    memset(g_rhSum, 0, sizeof(g_rhSum));
    g_rhDot = g_rhMm = 0.0;
    memset(g_rhDotAx, 0, sizeof(g_rhDotAx));
    memset(g_rhMmAx, 0, sizeof(g_rhMmAx));
    g_rhDotLag = g_rhMmLag = 0.0;
    g_chooseMulti = 0;
    g_chooseSpreadSum = g_chooseSpreadMax = 0.0;
    memset(g_tvDot, 0, sizeof(g_tvDot));
    memset(g_tvMm, 0, sizeof(g_tvMm));
    g_tvFrames = 0;
    g_twinFrames = 0;
    return true;
}

bool temporalPassDlaaTotals(uint32_t* frames, double* avgMs, double* maxMs,
                            uint32_t* resets) {
    if (g_dlaaTreats == 0) return false;
    uint32_t evals = 0, rs = 0;
    if (!dlaaTotals(&evals, avgMs, maxMs, &rs)) return false;
    if (frames) *frames = g_dlaaTreats;
    if (resets) *resets = rs;
    return true;
}

bool temporalPassTrainedTotals(uint32_t* frames, double* avgMs, double* maxMs, uint32_t* resets,
                               const char** engineLabel, bool* amd) {
    // The CURRENT engine's price, not "whichever one has a count". Both
    // displays used to try NVIDIA's totals and fall through to AMD's only
    // when NVIDIA's count was zero, and neither total is reset on a live
    // switch -- so after a dlss -> fsr A/B the tile in the headset kept
    // printing NVIDIA's average beside a price line that said fsr, and the
    // wrong one was the one on Sean's face (the review of 2026-09-16, F6).
    // The label and `amd` are written whatever the answer, so a caller can
    // word itself (and hide the NVIDIA-only per-role figures) even when
    // nothing has run yet.
    const bool amdEngine = g_temporalEngine == edvr::TemporalEngine::Amd;
    if (amd) *amd = amdEngine;
    if (engineLabel) *engineLabel = amdEngine ? edvr::fsr3VersionLabel() : "NVIDIA";
    if (amdEngine) return edvr::fsr3Totals(frames, avgMs, maxMs, resets);
    return temporalPassDlaaTotals(frames, avgMs, maxMs, resets);
}

// The same price, split by role and eye, straight from dlaa.cpp: unlike
// temporalPassDlaaTotals above, these need no g_dlaaTreats gate of their
// own -- each role/eye's own count already says whether it has run.
bool temporalPassDlaaFullTotals(int eye, uint32_t* frames, double* avgMs, double* maxMs) {
    return dlaaFullTotals(eye, frames, avgMs, maxMs);
}
bool temporalPassDlaaCentreTotals(int eye, uint32_t* frames, double* avgMs, double* maxMs) {
    return dlaaCentreTotals(eye, frames, avgMs, maxMs);
}
bool temporalPassDlaaPeripheryTotals(int eye, uint32_t* frames, double* avgMs, double* maxMs) {
    return dlaaPeripheryTotals(eye, frames, avgMs, maxMs);
}

// Stage 0 price report: the last CLOSED window's per-region median, in
// the fixed order prep/reduce/periphery/centre/full/compose/ui, plus
// "other" (total less the sum of those seven), the pair count the window
// covered, and (F5) how many pairs/calls that window dropped before
// pricing -- unmeasured pairs, lone eyes, no-slot frames and region-lease
// failures, summed. False until a window has closed this session (every
// 600 stereo pairs, or sooner on a treatment/size/format/setting change).
bool temporalPassPriceWindow(double regionMedianMs[7], double* otherMedianMs,
                              uint32_t* pairs, uint32_t* droppedTotal) {
    if (!g_lastWindowValid) return false;
    if (regionMedianMs) {
        for (int ri = 0; ri < kRegionCount; ++ri) regionMedianMs[ri] = g_lastWindowMedian[ri];
    }
    if (otherMedianMs) *otherMedianMs = g_lastWindowOtherMedian;
    if (pairs) *pairs = g_lastWindowPairs;
    if (droppedTotal) *droppedTotal = g_lastWindowDropped;
    return true;
}

static void beginEyeRun() {
    if (g_eyeRunLeft > 0 || g_eyeRunReady) return;
    perfMonitorNoteEvent(kEvEyeDump);
    // This request can occur after the trigger frame's scene draws. The eye
    // crops and decision controls include that frame; the accompanying draw
    // census starts here and can begin with its following frame.
    drawCensusAutoRequest();
    Log::get().note("eye capture: requested accompanying eye/offscreen/compute census for LOD investigation; AA-independent, an already active census keeps its current coverage.");
    SYSTEMTIME stm{};
    GetLocalTime(&stm);
    _snwprintf_s(g_eyeRunStamp, 16, _TRUNCATE, L"%02u%02u%02u", static_cast<unsigned>(stm.wHour),
                 static_cast<unsigned>(stm.wMinute), static_cast<unsigned>(stm.wSecond));
    g_eyeRunTaken = 0;
    g_eyeRunLeft = kEyeRun;
    applyEngineMotionDiagnostics();   // the census runs for the run
    g_eyeMotionTraceCount = 0;
    g_eyeInputsFrame=0;
    g_eyeRunUntreated=false;
    memset(g_eyeOverviewTaken,0,sizeof(g_eyeOverviewTaken));
    memset(g_eyeTreatedWritten,0,sizeof(g_eyeTreatedWritten));
    memset(g_eyeTreatedTaken,0,sizeof(g_eyeTreatedTaken));
    memset(g_eyeRawWritten,0,sizeof(g_eyeRawWritten));
    for(int k=0;k<kEyeRun;++k) {
        if(g_eyeDecisionStaging[k]){g_eyeDecisionStaging[k]->Release();g_eyeDecisionStaging[k]=nullptr;}
        if(g_eyePreUiStaging[k]){g_eyePreUiStaging[k]->Release();g_eyePreUiStaging[k]=nullptr;}
        g_eyeDecisions[k]=EyeDecisionFrame{};
    }
    for(auto& texture:g_eyeInputs)if(texture){texture->Release();texture=nullptr;}
    memset(g_eyeRawTaken, 0, sizeof(g_eyeRawTaken));
    memset(g_eyeRawInputW, 0, sizeof(g_eyeRawInputW));
    memset(g_eyeRawInputH, 0, sizeof(g_eyeRawInputH));
    memset(g_eyeRunFrames, 0, sizeof(g_eyeRunFrames));
    // The object ledger is armed at the same seam; its first complete draw
    // ledger can be the following frame, and the capture manifest's frame
    // IDs keep that explicit.
    objectProbeArmLedger(g_eyeRunStamp);
    // advanced.pixel_probe rides the same eye run, for the same reason: no
    // second keypress, and its one frame is this run's first.
    pixelProbeArm();
}

void temporalPassShutdown() {
    { std::lock_guard<std::mutex> lock(g_temporalHistoryMutex);g_temporalHistory.clear(); }
    dlaaShutdown();
    fsr3Shutdown();
    if (g_csMv) { g_csMv->Release(); g_csMv = nullptr; }
    if (g_csMvFast) { g_csMvFast->Release(); g_csMvFast = nullptr; }
    if (g_csMvTrace) { g_csMvTrace->Release(); g_csMvTrace = nullptr; }
    g_csMvTraceTried = false;
    if (g_passDevice) { g_passDevice->Release(); g_passDevice = nullptr; }
    if (g_csFovea) { g_csFovea->Release(); g_csFovea = nullptr; }
    if (g_csUiResolve) { g_csUiResolve->Release(); g_csUiResolve=nullptr; }
    g_csUiResolveTried=g_uiResolveNoted=false;
    if (g_uiResolveTolCb) { g_uiResolveTolCb->Release(); g_uiResolveTolCb=nullptr; }
    g_coronaHoldNoted=false;
    if (g_foveaCb) { g_foveaCb->Release(); g_foveaCb = nullptr; }
    if (g_csDown) { g_csDown->Release(); g_csDown = nullptr; }
    if (g_downCb) { g_downCb->Release(); g_downCb = nullptr; }
    // Stage 0 price report: any eye still waiting for its stereo twin at
    // shutdown is never going to get one -- drop it and count it (F2)
    // rather than price it alone, before the window it would have landed
    // in is closed below.
    for (PendingPair& p : g_pending) {
        if (p.used) finalizePending(p);
    }
    // The current window, even short of 600 pairs, is still evidence --
    // write it rather than drop it on the floor.
    flushWindow("shutdown");
    if (g_treats > 0) {
        Log::get().note("temporal aa: %u eye-submits treated this session.",
                        g_treats);
    }
    for (EyeState& e : g_eye) releaseEye(e);
    for (Slot& q : g_slots) releaseSlot(q);
    for(auto& overview:g_eyeRunStaging)if(overview){overview->Release();overview=nullptr;}
    for (int k = 0; k < kEyeRun; ++k) {
        if (g_eyeRawStaging[k]) { g_eyeRawStaging[k]->Release(); g_eyeRawStaging[k] = nullptr; }
        if (g_eyeTreatedStaging[k]) { g_eyeTreatedStaging[k]->Release(); g_eyeTreatedStaging[k] = nullptr; }
        if (g_eyeDecisionStaging[k]) { g_eyeDecisionStaging[k]->Release(); g_eyeDecisionStaging[k] = nullptr; }
        if (g_eyePreUiStaging[k]) { g_eyePreUiStaging[k]->Release(); g_eyePreUiStaging[k] = nullptr; }
    }
    g_eyeRunLeft = 0;
    g_eyeRunTaken = 0;
    g_eyeRunReady = false;
    g_eyeMotionTraceCount = 0;
    g_eyeInputsFrame=0;
    g_eyeRunUntreated=false;
    memset(g_eyeOverviewTaken,0,sizeof(g_eyeOverviewTaken));
    memset(g_eyeTreatedWritten,0,sizeof(g_eyeTreatedWritten));
    memset(g_eyeTreatedTaken,0,sizeof(g_eyeTreatedTaken));
    memset(g_eyeRawWritten,0,sizeof(g_eyeRawWritten));
    for(int k=0;k<kEyeRun;++k) g_eyeDecisions[k]=EyeDecisionFrame{};
    for(auto& texture:g_eyeInputs)if(texture){texture->Release();texture=nullptr;}
    if (g_statsUav) { g_statsUav->Release(); g_statsUav = nullptr; }
    if (g_stats) { g_stats->Release(); g_stats = nullptr; }
    if (g_samp) { g_samp->Release(); g_samp = nullptr; }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_cs) { g_cs->Release(); g_cs = nullptr; }
    if (g_csFast) { g_csFast->Release(); g_csFast = nullptr; }
}

bool temporalPassEyeOffset(int eye, float out[3]) {
    if (eye < 0 || eye > 1 || !out) return false;
    const EyeState& e = g_eye[eye];
    if (e.eyeOff[0] == 0.0f && e.eyeOff[1] == 0.0f && e.eyeOff[2] == 0.0f) return false;
    memcpy(out, e.eyeOff, sizeof(e.eyeOff));
    return true;
}

}  // namespace edvr

namespace edvr {

bool temporalPassPlanes(float* nearZ, float* farZ) {
    if (!nearZ || !farZ) return false;
    *nearZ = g_lastNear;
    *farZ = g_lastFar;
    return g_lastNear > 0.0f && g_lastFar > g_lastNear;
}

void temporalPassArmEyeDump() {
    // The key takes a RUN of the left eye (kEyeRun says why); a run already
    // under way is left alone.
    if (g_eyeRunLeft > 0 || g_eyeRunReady) return;
    beginEyeRun();
}

float temporalPassDepthAt(float metres) {
    if (!(metres > 0.0f)) return 0.0f;
    float a=0.0f,b=0.0f;
    temporalSceneProjection(g_curProj[0],g_curProj[1],g_lastNear,&a,&b);
    return a+b/metres;
}

}  // namespace edvr

extern "C" __declspec(dllexport) void edvrEyeCaptureUntreated(void* texture,int eye,const float* bounds) {
    if (edvr::deviceHookRecoveryDisabled()) return;
    edvr::guarded("eye capture/untreated",[&]{edvr::captureUntreatedEye(static_cast<ID3D11Texture2D*>(texture),eye,bounds);});
}
// Also available to the diagnostic tools; the hotkey uses the same arm.
extern "C" __declspec(dllexport) void edvrEyeCaptureArm() { edvr::temporalPassArmEyeDump(); }

extern "C" __declspec(dllexport) void* edvrTemporalAa(
    void* srcTex, int eye, const float* bounds, const float* tanNow,
    const float* tanPrev, float jxNow, float jyNow, const float* deltaHead,
    const float* headTrans, const float* headTransSwapped, float nearZ,
    float farZ, float headDeg, int motion, float blend, float clampSigma,
    unsigned outW, unsigned outH, unsigned flags) {
    if (!srcTex || eye < 0 || eye > 1 || !tanNow || edvr::deviceHookRecoveryDisabled()) return nullptr;
    void* out = nullptr;
    // The door census's whole-pass span (gpu_census.h): timed at this outer
    // boundary, not inside temporalInner -- that function has many early
    // returns, and an outer Begin/End closes correctly no matter which one
    // it takes. ctx is derived the same way temporalInner derives its own
    // (2281's idiom: get the device, get its context, release the device
    // immediately, keep the context for the call).
    ID3D11DeviceContext* censusCtx = nullptr;
    ID3D11Texture2D* censusTex = nullptr;
    static_cast<IUnknown*>(srcTex)->QueryInterface(__uuidof(ID3D11Texture2D),
                                                    reinterpret_cast<void**>(&censusTex));
    if (censusTex) {
        ID3D11Device* censusDev = nullptr;
        censusTex->GetDevice(&censusDev);
        if (censusDev) { censusDev->GetImmediateContext(&censusCtx); censusDev->Release(); }
        censusTex->Release();
    }
    edvr::gpuCensusBegin(censusCtx, edvr::GpuCensusSection::DoorTemporalWhole);
    edvr::guardedBudget(edvr::g_budget, [&] {
        out = edvr::temporalInner(srcTex, eye, bounds, tanNow, tanPrev, jxNow,
                                  jyNow, deltaHead, headTrans, headTransSwapped,
                                  nearZ, farZ, headDeg, motion, blend,
                                  clampSigma, outW, outH, flags);
    });
    edvr::gpuCensusEnd(censusCtx, edvr::GpuCensusSection::DoorTemporalWhole);
    if (censusCtx) censusCtx->Release();
    return out;
}

// For tools/smoke: the fovea's settings set directly (the harness has no
// ini), the failure latch cleared, and the count of composited frames so
// far returned -- so a desk case can tell a fovea that ran from one that
// quietly stood down to full-frame DLAA. A dev instrument; nothing else
// calls it.
extern "C" __declspec(dllexport) unsigned edvrTemporalAaFoveaDev(float deg, float edgeDeg, int steady,
                                                                 float scale, int round) {
    if (!std::isfinite(deg) || deg < 0.0f) deg = 0.0f;
    if (deg > 0.0f && deg < 10.0f) deg = 10.0f;
    if (deg > 120.0f) deg = 120.0f;
    edvr::g_foveaDeg = deg;
    if (!std::isfinite(edgeDeg) || edgeDeg < 1.0f) edgeDeg = 1.0f;
    if (edgeDeg > 30.0f) edgeDeg = 30.0f;
    edvr::g_foveaEdgeDeg = edgeDeg;
    edvr::g_periphSteady = steady != 0;
    if (!std::isfinite(scale) || scale < 0.25f) scale = 0.25f;
    if (scale > 1.0f) scale = 1.0f;
    edvr::g_periphScale = scale;
    edvr::g_foveaRound = round != 0;
    edvr::g_foveaFailed = false;
    return edvr::g_foveaTreats;
}

extern "C" __declspec(dllexport) void edvrTemporalAaNoteHead(int eye, const float* prevPose,
                                                             const float* nowPose,
                                                             const float* eyeOffset) {
    edvr::temporalPassNoteHead(eye, prevPose, nowPose, eyeOffset);
}

// For tools/smoke: the Stage 0 price report's last closed window, read
// directly rather than through the menu's formatted status line. A thin
// wrapper over temporalPassPriceWindow; medians7 must hold 7 doubles.
// Any of the four pointers may be null.
extern "C" __declspec(dllexport) void edvrTemporalAaPriceWindow(double* medians7, double* other,
                                                                unsigned* pairs, unsigned* dropped) {
    edvr::temporalPassPriceWindow(medians7, other, pairs, dropped);
}

// tools/smoke wires this in next to edvrEyeMaskSelftest, same pattern: pure
// geometry, no device, one bit per independent check, 63 (all six) is pass.
// Bit 1: width mode against a rectangle hand-derived from cropOf's own
// arithmetic before this extraction, checked twice independently -- not
// the design note's "1128px" figure, which neither derivation reproduced;
// the figure asserted here is the one this file's own formula produces.
// Bit 2: foveaEdgeRegionDeg's reduced-trim arithmetic, directly, on three
// edges that do not hit the 2 degree floor.
// Bit 4: edges mode's left/right trim-role swap between the two eyes
// (eye 0 = left, outer edge at its l tangent -- foveation.cpp:924-925)
// produces a mirror image of the same physical crop. y is untouched by
// the swap and comes out bit-identical; x is checked with a small pixel
// tolerance (two independent tangent-to-pixel paths land ~2px apart at
// this frame size, confirmed numerically before writing this tolerance).
// Bit 8: trims well past both the ini's 0..45 range and this frame's own
// edge angles floor each edge at 2 degrees rather than closing or
// inverting the rectangle; the surviving centre square is real but under
// minPx, so this exercises the ok=false path the caller falls back to
// full-frame on.
// Bit 16: temporal_aa_fovea_top and _bottom split apart -- the 20/25/7
// case's own reduced trims (outer 20, nasal 0, vertical 20 reduced by the
// 5/5/7 FOV trim to top=bottom=15), then bottom alone re-asked at 30
// (reduced to 25) with top left at 15, then top alone re-asked at 30
// (reduced to 25) with bottom left at 15. Expected x/y/w/h are hand-derived
// on paper from this file's own tan(A-B) identity, not read off the code:
// tan(atan(E) - D) = (E - tanD) / (1 + E*tanD) for each edge's tangent
// magnitude E and reduced trim D, then the same pixel, clamp and
// even-round arithmetic as computeFoveaRegion. native_temporal.h:21's
// {left,right,down,up} order puts row 0 at the up edge, so mode.topTrimDeg
// (feeding "bo", the up tangent) bounds y; mode.bottomTrimDeg (feeding
// "to", the down tangent) bounds y+h. Moving bottom alone must hold y and
// move y+h; moving top alone must hold y+h (equal to the unmoved case) and
// move y; x/w (outer/nasal) are untouched throughout.
// Bit 32: the head lead (advanced.temporal_aa_fovea_lead) -- the base offset
// with its even rounding and its two clamps, the lead's direction from a
// hand case, and the centre motion's own mirror of the shader's convention
// (no rotation is no motion; a yaw right is mv.x > 0; a pitch up is
// mv.y < 0). The case's own comment carries the derivations.
extern "C" __declspec(dllexport) unsigned edvrFoveaRegionSelftest() {
    using namespace edvr;
    unsigned bits = 0;

    // Crystal Super-shaped tangents, the same worked example eye_mask.cpp
    // uses; fw/fh a plausible per-eye render size for it. down/up match
    // native_temporal.h:21's {left,right,down,up} frusta order.
    const float l = -1.529f, r = 1.032f, down = -1.265f, up = 1.265f;
    const uint32_t fw = 2576, fh = 2544;

    {
        FoveaRegionMode mode;
        mode.edges = false;
        mode.widthDeg = 40.0f;
        const FoveaRegion g = computeFoveaRegion(l, r, down, up, fw, fh, 0, mode, 128);
        if (g.ok && g.x == 1170 && g.y == 906 && g.w == 734 && g.h == 732) bits |= 1u;
    }

    {
        constexpr float kDegToRad = 0.01745329252f;
        const float outer = foveaEdgeRegionDeg(tanf(51.8f * kDegToRad), 20.0f);
        const float nasal = foveaEdgeRegionDeg(tanf(38.9f * kDegToRad), 0.0f);
        const float vertical = foveaEdgeRegionDeg(tanf(46.7f * kDegToRad), 15.0f);
        if (fabsf(outer - 31.8f) < 0.05f && fabsf(nasal - 38.9f) < 0.05f &&
            fabsf(vertical - 31.7f) < 0.05f) {
            bits |= 2u;
        }
    }

    {
        FoveaRegionMode mode;
        mode.edges = true;
        mode.outerTrimDeg = 10.0f;
        mode.nasalTrimDeg = 5.0f;
        mode.topTrimDeg = 8.0f;
        mode.bottomTrimDeg = 8.0f;
        const FoveaRegion g0 = computeFoveaRegion(l, r, down, up, fw, fh, 0, mode, 128);
        const FoveaRegion g1 = computeFoveaRegion(-r, -l, down, up, fw, fh, 1, mode, 128);
        const bool yMatch = g0.ok && g1.ok && g0.y == g1.y && g0.h == g1.h;
        const int mirrorX0 = static_cast<int>(fw) - static_cast<int>(g0.x) - static_cast<int>(g0.w);
        const int mirrorX1 = static_cast<int>(fw) - static_cast<int>(g0.x);
        const int dx0 = mirrorX0 - static_cast<int>(g1.x);
        const int dx1 = mirrorX1 - static_cast<int>(g1.x) - static_cast<int>(g1.w);
        if (yMatch && dx0 >= -4 && dx0 <= 4 && dx1 >= -4 && dx1 <= 4) bits |= 4u;
    }

    {
        FoveaRegionMode mode;
        mode.edges = true;
        mode.outerTrimDeg = 100.0f;
        mode.nasalTrimDeg = 100.0f;
        mode.topTrimDeg = 100.0f;
        mode.bottomTrimDeg = 100.0f;
        const FoveaRegion g = computeFoveaRegion(l, r, down, up, fw, fh, 0, mode, 128);
        if (!g.ok) bits |= 8u;
    }

    {
        // The 20/25/7 case (bit 2's own scenario) split into top and bottom:
        // outer reduced to 20 (25 - 5), nasal reduced to 0 (7 - 7, floored),
        // vertical reduced to 15 (20 - 5) -- set on BOTH edges first (matches
        // today's single-vertical-trim behaviour bit-for-bit); then bottom
        // alone re-asked at 30 raw (reduced to 25) with top left at 20/15;
        // then top alone re-asked at 30 raw (reduced to 25) with bottom left
        // at 20/15.
        //
        // computeFoveaRegion's "bo" (built from the up tangent) is gated by
        // mode.topTrimDeg and bounds y0 (row 0, native_temporal.h:21's up
        // edge); "to" (built from the down tangent) is gated by
        // mode.bottomTrimDeg and bounds y1 = y+h. So the bottom key alone
        // must hold y and move y+h; the top key alone must hold y+h and
        // move y -- re-derived here on paper, not assumed from either key's
        // name or mirrored from a previous (wrong) wiring.
        //
        // Hand derivation (l=-1.529, r=1.032, down=-1.265, up=1.265,
        // fw=2576, fh=2544, eye 0 so leftTrim=outer=20, rightTrim=nasal=0):
        // tan(atan(E) - D) = (E - tanD) / (1 + E*tanD), tan15=0.2679492,
        // tan20=0.3639702, tan25=0.4663077.
        //   left  (E=1.529, D=20): lo = -0.7484882
        //   right (E=1.032, D=0):  ro =  1.032 (unchanged, D=0)
        //   trim=15 (E=1.265): tangent  0.7446480
        //   trim=25 (E=1.265): tangent  0.5023604
        // x0=int(((lo-l)/(r-l))*fw)=int(785.08)=785 -> &~1 -> 784
        // x1=int(((ro-l)/(r-l))*fw+0.5)=int(2576.5)=2576 -> &~1 -> 2576, w=1792
        // (x/w are the same in all three cases: outer/nasal never change.)
        //
        // unmoved (top=bottom=15): bo=+0.7446480 (topTrimDeg=15),
        //   to=-0.7446480 (bottomTrimDeg=15)
        //   y0=int(((up-bo)/(up-down))*fh)=int(523.23)=523 -> &~1 -> 522
        //   y1=int(((up-to)/(up-down))*fh+0.5)=int(2021.27)=2021 -> &~1 -> 2020
        //   h=2020-522=1498
        // bottom moved (top=15, bottom=25): bo UNCHANGED (topTrimDeg still
        //   15) -> y0=522 unchanged; to=-0.5023604 (bottomTrimDeg=25) ->
        //   y1=int(((1.265+0.5023604)/2.530)*2544+0.5)=int(1777.64)=1777
        //   -> &~1 -> 1776, h=1776-522=1254
        // top moved (top=25, bottom=15): to UNCHANGED (bottomTrimDeg still
        //   15) -> y1=2020 unchanged; bo=+0.5023604 (topTrimDeg=25) ->
        //   y0=int(((1.265-0.5023604)/2.530)*2544)=int(766.86)=766
        //   -> &~1 -> 766, h=2020-766=1254
        FoveaRegionMode mode;
        mode.edges = true;
        mode.outerTrimDeg = 20.0f;
        mode.nasalTrimDeg = 0.0f;
        mode.topTrimDeg = 15.0f;
        mode.bottomTrimDeg = 15.0f;
        const FoveaRegion gUnmoved = computeFoveaRegion(l, r, down, up, fw, fh, 0, mode, 128);
        mode.bottomTrimDeg = 25.0f;
        const FoveaRegion gBottomMoved = computeFoveaRegion(l, r, down, up, fw, fh, 0, mode, 128);
        mode.bottomTrimDeg = 15.0f;
        mode.topTrimDeg = 25.0f;
        const FoveaRegion gTopMoved = computeFoveaRegion(l, r, down, up, fw, fh, 0, mode, 128);
        // Not named "near": windef.h's legacy near/far macros expand to
        // nothing and turn "const auto near = ..." into invalid syntax.
        const auto withinTol = [](uint32_t v, int want) {
            const int d = static_cast<int>(v) - want;
            return d >= -2 && d <= 2;
        };
        const bool unmovedOk = gUnmoved.ok && withinTol(gUnmoved.x, 784) && withinTol(gUnmoved.y, 522) &&
                               withinTol(gUnmoved.w, 1792) && withinTol(gUnmoved.h, 1498);
        const bool bottomMovedOk = gBottomMoved.ok && withinTol(gBottomMoved.x, 784) &&
                                  withinTol(gBottomMoved.y, 522) && withinTol(gBottomMoved.w, 1792) &&
                                  withinTol(gBottomMoved.h, 1254);
        const bool topMovedOk = gTopMoved.ok && withinTol(gTopMoved.x, 784) &&
                                withinTol(gTopMoved.y, 766) && withinTol(gTopMoved.w, 1792) &&
                                withinTol(gTopMoved.h, 1254);
        // Bottom key alone: x/w untouched, y bit-identical, only y+h (the
        // bottom row) moves.
        const bool bottomMovesOnlyBottom =
            gUnmoved.x == gBottomMoved.x && gUnmoved.w == gBottomMoved.w &&
            gUnmoved.y == gBottomMoved.y &&
            (gUnmoved.y + gUnmoved.h) != (gBottomMoved.y + gBottomMoved.h);
        // Top key alone: x/w untouched, y+h bit-identical to the unmoved
        // case, only y (the top row) moves.
        const bool topMovesOnlyTop =
            gUnmoved.x == gTopMoved.x && gUnmoved.w == gTopMoved.w &&
            (gUnmoved.y + gUnmoved.h) == (gTopMoved.y + gTopMoved.h) &&
            gUnmoved.y != gTopMoved.y;
        if (unmovedOk && bottomMovedOk && topMovedOk && bottomMovesOnlyBottom && topMovesOnlyTop) {
            bits |= 16u;
        }
    }

    {
        // Bit 32: THE HEAD LEAD's pure parts (foveaCentreMotion,
        // foveaLeadPixels, foveaLeadBase).
        //
        // The base offset: zero leaves the rectangle exactly where
        // computeFoveaRegion put it; a lead with room moves the base by the
        // even-rounded amount and NEVER changes w/h (a size change would
        // recreate NVIDIA's feature); an odd lead rounds to even; a lead past
        // the frame's edge stops at it and says it was held; a negative lead
        // stops at 0.
        //
        // The direction, hand-derived from the shader's convention (previous =
        // current + mv, render pixels): mv = (+10, 0) px with 6 frames is a
        // +60 px move, to the RIGHT, which is where the head was turning when
        // the content at the centre came from the right; mv = (0, -10) is -60,
        // UP the rows, row 0 being the up edge.
        //
        // And the mirror itself: with equal frusta and no rotation there is no
        // motion at all; a 2-degree yaw to the right gives mv.x > 0; a
        // 2-degree pitch up gives mv.y < 0. The rotation rows are built the
        // way the cbuffer's dR0..dR2 are -- the rotation taking THIS frame's
        // view directions to last frame's -- so a yaw right puts the current
        // forward (0,0,-1) at (sin a, 0, -cos a) in last frame's frame.
        FoveaRegionMode mode;
        mode.edges = false;
        mode.widthDeg = 40.0f;
        const FoveaRegion g = computeFoveaRegion(l, r, down, up, fw, fh, 0, mode, 128);
        const int32_t room = static_cast<int32_t>(fw - g.w);   // 2576 - 734 = 1842

        const FoveaLeadBase none = foveaLeadBase(g, 0.0f, 0.0f, fw, fh);
        const bool zeroHolds = g.ok && none.x == g.x && none.y == g.y && none.dx == 0 &&
                               none.dy == 0 && !none.clamped;

        const FoveaLeadBase right = foveaLeadBase(g, 60.0f, 0.0f, fw, fh);
        const bool movesRight = right.x == g.x + 60 && right.y == g.y && right.dx == 60 &&
                                right.dy == 0 && !right.clamped;

        const FoveaLeadBase odd = foveaLeadBase(g, 61.0f, -3.0f, fw, fh);
        const bool evenRounded = (odd.dx % 2) == 0 && (odd.dy % 2) == 0 && odd.dx == 62 &&
                                 odd.dy == -4;

        const FoveaLeadBase far_ = foveaLeadBase(g, 5000.0f, 5000.0f, fw, fh);
        const bool heldAtEdge = far_.clamped && far_.x == static_cast<uint32_t>(room) &&
                                far_.y == static_cast<uint32_t>(fh - g.h) &&
                                far_.x + g.w <= fw && far_.y + g.h <= fh;

        const FoveaLeadBase back = foveaLeadBase(g, -5000.0f, -5000.0f, fw, fh);
        const bool heldAtZero = back.clamped && back.x == 0 && back.y == 0 &&
                                back.dx == -static_cast<int32_t>(g.x) &&
                                back.dy == -static_cast<int32_t>(g.y);

        const FoveaLead sideways = foveaLeadPixels(10.0f, 0.0f, 6.0f);
        const FoveaLead upward = foveaLeadPixels(0.0f, -10.0f, 6.0f);
        const FoveaLeadBase bySide = foveaLeadBase(g, sideways.x, sideways.y, fw, fh);
        const FoveaLeadBase byUp = foveaLeadBase(g, upward.x, upward.y, fw, fh);
        const bool directionOk = bySide.dx == 60 && bySide.dy == 0 && byUp.dx == 0 &&
                                 byUp.dy == -60 && bySide.x == g.x + 60 && byUp.y == g.y - 60;

        // w/h are never touched by any of the above.
        const bool sizeHeld = g.ok && right.x + g.w <= fw && odd.x + g.w <= fw &&
                              bySide.x + g.w <= fw && byUp.y + g.h <= fh;

        const float tanSym[4] = {-1.0f, 1.0f, -1.0f, 1.0f};
        const float ident[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        float mvx = 1.0f, mvy = 1.0f;
        const bool still = foveaCentreMotion(tanSym, tanSym, ident[0], ident[1], ident[2], 1000,
                                             1000, &mvx, &mvy) &&
                           fabsf(mvx) < 1e-3f && fabsf(mvy) < 1e-3f;
        constexpr float kDegToRadSelf = 0.01745329252f;
        const float a = 2.0f * kDegToRadSelf;
        const float yaw[3][3] = {{cosf(a), 0.0f, -sinf(a)}, {0.0f, 1.0f, 0.0f},
                                 {sinf(a), 0.0f, cosf(a)}};
        float yawX = 0.0f, yawY = 0.0f;
        const bool yawRight = foveaCentreMotion(tanSym, tanSym, yaw[0], yaw[1], yaw[2], 1000, 1000,
                                                &yawX, &yawY) &&
                              yawX > 1.0f && fabsf(yawY) < 1e-2f;
        const float pitch[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, cosf(a), -sinf(a)},
                                   {0.0f, sinf(a), cosf(a)}};
        float pitchX = 0.0f, pitchY = 0.0f;
        const bool pitchUp = foveaCentreMotion(tanSym, tanSym, pitch[0], pitch[1], pitch[2], 1000,
                                               1000, &pitchX, &pitchY) &&
                             pitchY < -1.0f && fabsf(pitchX) < 1e-2f;

        if (zeroHolds && movesRight && evenRounded && heldAtEdge && heldAtZero && directionOk &&
            sizeHeld && still && yawRight && pitchUp) {
            bits |= 32u;
        }
    }

    return bits;
}
