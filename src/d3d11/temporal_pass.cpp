#include "temporal_pass.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>   // std::swap, for the depth carry's pointer swap
#include <vector>    // the eye dump's row buffer

#include <windows.h>

#include <d3d11.h>
#include <cstdarg>

#include "../common/config.h"
#include "../common/temporal_mode.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "device_hook.h"   // the auto mip bias's source, to check against a real frame
#include "../common/supersample_math.h"   // supersampleRegionFromBounds: one region rule at the door
#include "../common/temporal_math.h"
#include "depth_probe.h"
#include "dlaa.h"
#include "object_probe.h"   // objectMotionGet: the dominant body's own motion, for the body's path (tier 2)
#include "ui_depth.h"   // uiDepthReactiveMask: the interface's bias mask
#include "ui_resolve.h"
#include "screen_motion.h"
#include "celestial_motion.h"
#include "shader_swap.h"
#include "gpu_timing.h"
#include "temporal_shader_bytecode.h"

namespace edvr {
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

// The cbuffer above, laid out to match: 480 bytes, thirty 16-byte rows.
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
    float   st0[4];      // the body's path (tier 2, docs/per-object-motion.md): the composite delta's rows...
    float   st1[4];
    float   st2[4];
    float   tvSt[4];     // ...xyz its translation term, w 1 = on this frame
    float   st2_0[4];    // the second body's path (object_probe.h, 2026-09-09): the same shape
    float   st2_1[4];
    float   st2_2[4];
    float   tv2St[4];    // ...w 1 = on this frame
    float   st3R[36][4]; // the stepped parts' twelve composite deltas (object_probe.h), three rows each
    float   tv3[12][4];  // ...and their translation terms, w 1 = filled this frame
    float   objects[4];  // x the reach in metres, for the record
    float   wR0[4];      // this frame's camera rows, view -> world, with the position in w
    float   wR1[4];
    float   wR2[4];
    float   box0[4];     // the body's box, low corner
    float   box1[4];     // ...high corner
    float   ships[4];    // x the moving ships in hand this frame (object_probe.h, 2026-09-09)
    float   shR[kObjectShipsMax * 3][4];   // per ship, three rows of its composite delta
    float   shTv[kObjectShipsMax][4];      // xyz its translation term
    float   shBox0[kObjectShipsMax][4];    // xyz its box, low corner (world)
    float   shBox1[kObjectShipsMax][4];    // xyz ...high corner
    float   shDir[kObjectShipsMax][4];     // xyz its way (unit), w its tail plane
    float   shParts[kObjectShipsMax * kObjectShipParts][4];   // its parts' positions, shBox0[i].w of them
    float   shRect[kObjectShipsMax][4];    // its box's footprint on the image, pixels
    float   holoJitter[4];
};
static_assert(sizeof(PassParams) == 6624, "the cbuffer is 414 16-byte rows");

// The format allowlist: the supersample resolve's, for its reasons
// (supersample_pass.cpp) -- typeless and UNORM families read and written
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
    ID3D11Texture2D*           dlDepth = nullptr;
    ID3D11UnorderedAccessView* dlDepthUav = nullptr;
    ID3D11ShaderResourceView*  dlDepthSrv = nullptr;
    ID3D11Texture2D*           dlOut = nullptr;
    ID3D11UnorderedAccessView* dlOutUav = nullptr;   // the debug motion view paints here
    ID3D11ShaderResourceView*  dlOutSrv = nullptr;   // the fovea composite reads NVIDIA's crop through this
    ID3D11Texture2D*           dlSubmit = nullptr;  // NVIDIA's frame copied into the game's own format: what goes out
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
    // game's own format, and the crop's own NVIDIA history flag.
    ID3D11Texture2D*           foveaOut = nullptr;
    ID3D11UnorderedAccessView* foveaOutUav = nullptr;
    uint32_t                   foveaW = 0, foveaH = 0;
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
    if (e.dlDepthUav) { e.dlDepthUav->Release(); e.dlDepthUav = nullptr; }
    if (e.dlColour) { e.dlColour->Release(); e.dlColour = nullptr; }
    if (e.dlMv) { e.dlMv->Release(); e.dlMv = nullptr; }
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
struct Slot {
    GpuTimer      timer;
    ID3D11Buffer* staging = nullptr;
    bool          inUse = false;
    bool          timeDone = false;
    bool          timing = false;
    bool          statsDone = false;
    uint64_t      pixels = 0;
    // The instrument's bookkeeping for this call: which candidates had a
    // delta (their pixel totals), the head's turn, whether history ran.
    uint64_t      candPixels[4] = {};
    float         headDeg = 0.0f;
    bool          hadHistory = false;
};
constexpr int kSlots = 8;
constexpr int kStatCount = 52;   // 50 used since 2026-09-09 (39-45 the moving ships, 46 the second body, 47-49 the stepped parts); a 208-byte buffer
Slot g_slots[kSlots];

void releaseSlot(Slot& q) {
    q.timer.reset();
    if (q.staging) { q.staging->Release(); q.staging = nullptr; }
    q = Slot{};
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
uint32_t g_camDropMove = 0;         // frames whose camera translation was a jump
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
uint64_t g_bodyPix = 0;    // pixels that took the body's path this interval (Stats[29]; tier 2)
uint64_t g_body2Pix = 0;   // ...the second body's (Stats[46]; object_probe.h, 2026-09-09)
uint64_t g_body2Frames = 0;   // frames the second body's path was on
uint64_t g_steppedPix = 0;    // ...a stepped part's (Stats[47]; object_probe.h, 2026-09-09)
// The stamps are OFF (2026-09-09 18:22, the thirty-seventh flight): the
// objects view had the hub white with orange specks -- the stamped cells
// miss the hub's visible pixels -- and the multiples they carry are the
// pool records' jitter, not what is drawn: the hub's records jitter in
// place a third of a degree either way, frame about, with no net turn,
// while the drawn hub turns by a skinning bone the pool never shows. The
// Tracking runs only during captures or explicit diagnostics.
// The shared producer/consumer switch lives in object_probe.h.
// NVIDIA's history resets this interval (the run of 18:43, 2026-09-09: the
// station "flickering into sharpness" -- a raw frame every reset, the
// blur back as the history rebuilds on the pass's vectors), and how many
// the openvr half asked for (a withheld frame, or a pose without a delta).
uint64_t g_dlResets = 0, g_dlResetsAsked = 0;
// ...the openvr half's reasons, bits 2-5 of the flags (temporal_aa.cpp): a
// hold or a healed frame, a withheld jump the camera came back from, one
// left unjudged, a pose without a delta.
uint64_t g_dlResetsHeld = 0, g_dlResetsReturned = 0, g_dlResetsUnjudged = 0, g_dlResetsNoDelta = 0;
uint64_t g_steppedCellPix = 0;   // pixels whose cell was a stepped part's (Stats[48])...
uint64_t g_steppedOffPix = 0;    // ...and whose path was refused there (Stats[49])
uint64_t g_steppedFrames = 0; // frames with stepped cells stamped
uint64_t g_shipPix = 0;    // pixels that took a moving ship's path this interval (Stats[39]; 2026-09-09)
uint64_t g_shipFoot = 0;         // ...and in a ship's footprint with a depth, not claimed (Stats[40])
uint64_t g_shipOutBox = 0;       // of those, outside the box at their depth (41)
uint64_t g_shipBehind = 0;       // behind the tail (42)
uint64_t g_shipFar = 0;          // beyond the parts' reach (43)
int64_t  g_shipOutBoxDm = 0;     // the sum over 41 of the depth less the box centre's, decimetres (44)
uint64_t g_shipFootNoDepth = 0;  // in a footprint with no depth (45)

void maybeLogPrice() {
    if (g_priceLogged || g_timeCount < 120 || g_pixelsSeen == 0) return;
    g_priceLogged = true;
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
                if(status==GpuTimerPoll::Ready){q.timeDone=true;++g_timeCount;g_timeSum+=ms;if(ms>g_timeMax)g_timeMax=ms;}
                else if(status==GpuTimerPoll::Invalid) q.timeDone=true;
            }
        }
        if (!q.statsDone) {
            D3D11_MAPPED_SUBRESOURCE m{};
            const HRESULT hr = ctx->Map(q.staging, 0, D3D11_MAP_READ,
                                        D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (SUCCEEDED(hr) && m.pData) {
                const uint32_t* v = static_cast<const uint32_t*>(m.pData);
                g_rejected += v[0];
                g_clipped += v[1];
                g_pixelsSeen += q.pixels;
                g_intervalPix += q.pixels;
                g_worldPix += v[15];
                g_brightPix += v[16];
                g_brightNoDepthPix += v[17];
                g_moverPix += v[28];
                g_bodyPix += v[29];
                g_body2Pix += v[46];
                g_steppedPix += v[47];
                g_steppedCellPix += v[48];
                g_steppedOffPix += v[49];
                g_shipPix += v[39];
                g_shipFoot += v[40];
                g_shipOutBox += v[41];
                g_shipBehind += v[42];
                g_shipFar += v[43];
                g_shipOutBoxDm += static_cast<int32_t>(v[44]);
                g_shipFootNoDepth += v[45];
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
        if (q.timeDone && q.statsDone) q.inUse = false;
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
ID3D11ComputeShader*       g_csMv = nullptr;     // the motion-vector entry, for DLAA
bool                       g_csMvTried = false;
ID3D11ComputeShader*       g_csMvFast = nullptr;
bool                       g_csMvFastTried = false;
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
ID3D11ComputeShader*       g_csFovea = nullptr;  // the fovea composite (feature 6)
ID3D11ComputeShader*       g_csUiResolve = nullptr;
bool                      g_csUiResolveTried = false, g_uiResolveNoted = false;
bool                       g_csFoveaTried = false;
ID3D11Buffer*              g_foveaCb = nullptr;   // its crop and edge band
bool                       g_foveaNoted = false;
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
bool                       g_dlaaFailNoted = false;
bool                       g_trainedNoted = false;   // the first trained frame's line
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

// The configure and warm state.
bool     g_wanted = false;
bool     g_filterCurrent = true;   // advanced.temporal_aa_current = filtered | raw
float    g_historyC = 0.5f;        // advanced.temporal_aa_history_sharp: the cubic's C
float    g_shipMetres = kTemporalShipMetres;    // advanced.temporal_aa_ship_metres: the world/ship split (0 off)
int      g_debugMode = 0;          // advanced.temporal_aa_debug: 0 off, 1 motion, 2 error, 3 depth
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
// Tier 2: the dominant body's own path (fix.temporal_aa_objects), from the
// instance pool's largest rigid cluster (object_probe.cpp), taken per pixel
// against the camera's by a 3x3 match with this margin.
bool     g_objectsOn = false;      // fix.temporal_aa_objects
float    g_objectsReach = 1500.0f; // advanced.temporal_aa_objects_reach, metres
bool     g_objectsNoted = false;   // the engage line, once
ObjectMotion g_bodyLast = {};      // the motion last handed to the shader, for the log
bool     g_bodyLastValid = false;
float    g_bodyDtMs = 11.1f;       // this frame's length, for the body's rates
float    g_shipsRangeM = 1000.0f;  // advanced.temporal_aa_objects_ships_metres: moving ships within this take their own path (0 off)
constexpr float kShipReachM = 30.0f;   // a ship's claim around each recorded part: the probe pads its box by the same (kShipPadM there)
bool     g_shipsNoted = false;     // the ships' engage line, once
ObjectShip g_shipsLast = {};       // the nearest ship last handed to the shader, for the log
uint32_t g_shipsLastN = 0;         // how many were, this frame
uint32_t g_shipsLastAge = 0;
// The body's pair and this frame's rows must be in the same frame: the
// pool's positions and the box are in the frame of the pair's own camera
// rows, and the floating origin moves on an approach (a rebase of 13 km
// read from the dumps of 2026-09-08). A pair whose camera stands over this
// far from the frame's rows is in another frame -- a rebase since it, or
// another camera's rows this frame -- and the body waits for a pair taken
// in this one. (A twelve-frame hold after every camera jump did this on
// 2026-09-08, and the player saw each hold as the station blurring and
// resolving again: 13-26 frames an interval.) Four kilometres since
// 2026-09-09, from five hundred: the pair's camera also stands where the
// camera WAS, and a body held up to 120 frames behind a ship at boost
// covered the five hundred in a second -- "the body stood down 10-28
// frames with its pair in another frame" an interval on the ships'
// flights, each a frame the station fell to the camera's path, and the
// player felt it as the station juddering. A rebase is thirteen
// kilometres (the dumps of 2026-09-08); four covers a boost.
constexpr float kBodyFrameM = 4000.0f;
constexpr float kBodyNearM = 50.0f;  // the body's near floor, metres (the probe shares it)
uint32_t g_bodyFrameHolds = 0;     // frames stood down with the pair in another frame this interval
bool     g_bodyRowsOk = false;     // this frame's rows are the view's own (its delta not carried)
uint32_t g_bodyRowsHolds = 0;      // frames the body composed with the carried camera delta this interval
// After the origin moves (a camera jump of over 50 m in a frame) the held
// pair's positions and box are in the OLD frame. Rather than standing down
// until a fresh pair -- up to twenty frames, each one the station on the
// camera's path, the flash the player saw on 2026-09-09 -- the jump's own
// vector carries the body over: the box shifts by it, the translation term
// by (I - R) times it (a rotation about an axis a is (I - R) a, and the
// axis moved with everything else), and the pair's camera by it for the
// test above. Summed over jumps, cleared when a pair agrees with the rows
// unshifted; a flip's return sums back to nought on its own. The body
// uses the carried camera on the jump frame itself.
float    g_bodyShift[3] = {};
float    g_bodyOriginStep[3] = {}; // this jump alone; previous camera -> current origin
uint32_t g_bodyShiftFrame = ~0u;   // the frame of the last jump
uint32_t g_bodyShiftUsed = 0;      // frames carried over by the shift this interval
// A worker can publish between eye submissions. Freeze the station sample
// for the scene frame so its rates, coordinate stamp and grid version agree
// in both eyes; ensureBodyGrid uploads that version on the first use.
ObjectMotion g_frameBody{};
uint32_t g_frameBodyFrame = ~0u;
bool g_frameBodyValid = false;

// hotkey.dump_eyes, and the settings menu's "Dump both eyes as seen": the
// treated eye as the compositor receives it -- after DLSS, the fovea
// composite, everything -- to edvr_logs\eyes\eye_HHMMSS_L.bmp and _R.bmp,
// 24-bit, so what the player saw through the lens can be read off the desk
// instead of photographed through it (asked for on 2026-09-09, with a debug
// view up). One staging copy and a map that waits for the GPU: a hitch,
// once per press. Float formats are taken as linear and encoded sRGB for
// the file; the 8- and 10-bit ones are written as they are.
bool     g_eyeDumpArmed[2] = {false, false};
uint32_t g_eyeDumps = 0;
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
int              g_eyeRunLeft = 0;    // captures still to take
int              g_eyeRunTaken = 0;
wchar_t          g_eyeRunStamp[16] = L"";
bool             g_eyeRunReady = false;
bool             g_eyeRunUntreated = false;
bool             g_eyeOverviewTaken[2] = {};
constexpr int kEyeInputs=10;
ID3D11Texture2D*  g_eyeInputs[kEyeInputs] = {};
uint32_t         g_eyeInputsFrame=0,g_eyeInputsUiBound=0,g_eyeInputsUiFlags=0;
const wchar_t* const kEyeInputNames[kEyeInputs]={L"MV",L"Z",L"UI",L"Bias",L"SceneZ",L"TerrainIndex",L"TerrainZ",L"HoloCoverage",L"UiEdits",L"ScreenMotion"};
uint32_t         g_eyeRunWidth = 0, g_eyeRunHeight = 0;
bool             g_eyeRawTaken[kEyeRun] = {};
uint32_t         g_eyeRunFrames[kEyeRun] = {};
uint32_t         g_eyeRawInputW[kEyeRun] = {}, g_eyeRawInputH[kEyeRun] = {};
uint32_t         g_eyeCaptureFrame = 0; // current scene frame, stamped before treatment
struct EyeMotionTrace {
    uint32_t frame, eye, flags, outputWidth, outputHeight;
    bool bodyValid, rowsOk, jumped, dlHistory;
    bool rowsBound;
    int rowsFollow;
    uint32_t sceneDraws;
    float dtMs, prevRows[12], nowRows[12], shift[3], originStep[3];
    ObjectMotion body; // only scalar fields are written; grid pointer is never dereferenced
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
    fprintf(f, "frame,eye,crop,rawCaptured,flags,outW,outH,bodyValid,rowsOk,jumped,dlHistory,dtMs,gridVersion,age,records,pairDtMs,fitRms,history,inputW,inputH");
    auto names = [&](const char* name, int n) { for (int k = 0; k < n; ++k) fprintf(f, ",%s%d", name, k); };
    names("prev",12); names("now",12); names("shift",3); names("originStep",3);
    names("pairCam",3); names("omega",3); names("rateT",3);
    names("tanNow",4); names("tanPrev",4); names("jitter",4);
    names("bodyR",12); names("bodyTv",4); names("body2R",12); names("body2Tv",4);
    names("cameraR",12); names("cameraTv",4); names("boxLow",4); names("boxHigh",4);
    names("headR",12); names("headTv",4);
    fprintf(f, ",projectionA,projectionB,rowsBound,rowsFollow,sceneDraws");
    fprintf(f, "\n");
    for (uint32_t i = 0; i < g_eyeMotionTraceCount; ++i) {
        const EyeMotionTrace& t = g_eyeMotionTrace[i];
        const PassParams& p = t.params;
        int crop = -1;
        for (int k = 0; k < g_eyeRunTaken; ++k) if (g_eyeRunFrames[k] == t.frame) crop = k;
        fprintf(f, "%u,%u,%d,%d,%u,%u,%u,%d,%d,%d,%d,%.9g,%u,%u,%u,%.9g,%.9g,%d,%d,%d",
                t.frame, t.eye, crop, crop >= 0 && g_eyeRawTaken[crop], t.flags, t.outputWidth, t.outputHeight,
                t.bodyValid, t.rowsOk, t.jumped, t.dlHistory, t.dtMs, t.body.gridVersion, t.body.age,
                t.body.records, t.body.dtMs, t.body.rms, p.haveHistory, p.size[0], p.size[1]);
        auto values = [&](const float* a, int n) { for (int k = 0; k < n; ++k) fprintf(f, ",%.9g", a[k]); };
        values(t.prevRows,12); values(t.nowRows,12); values(t.shift,3); values(t.originStep,3);
        values(t.body.camPos,3); values(t.body.omegaPerMs,3); values(t.body.tPerMs,3);
        values(p.tanNow,4); values(p.tanPrev,4); values(p.jit,4);
        values(p.st0,4); values(p.st1,4); values(p.st2,4); values(p.tvSt,4);
        values(p.st2_0,4); values(p.st2_1,4); values(p.st2_2,4); values(p.tv2St,4);
        for (int r = 0; r < 3; ++r) values(p.cand[2][r],4);
        values(p.tvCam,4); values(p.box0,4); values(p.box1,4);
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
    ++g_eyeDumps;
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
    }
    if(scene) {
        ID3D11Resource* res=nullptr;scene->GetResource(&res);
        if(res){res->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&textures[4]));res->Release();}
    }
    celestialMotionStageDump(ctx,textures[4]);
    uiDepthHoloStageDump(ctx,textures[4]);
    if(textures[4]) {
        ID3D11ShaderResourceView* terrain[3]{}; celestialMotionViews(textures[4],terrain);
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
    }
    for(int k=0;k<kEyeInputs;++k)if(textures[k])stageEyeRun(ctx,textures[k],g_eyeInputs,k);
    for(int k=5;k<kEyeInputs;++k)if(textures[k])textures[k]->Release();
    if(textures[4])textures[4]->Release();
    g_eyeInputsFrame=g_rowsFrame;g_eyeInputsUiBound=static_cast<uint32_t>(uiBound);
    g_eyeInputsUiFlags=static_cast<uint32_t>(uiFlags);
}
void writeEyeInputs(ID3D11DeviceContext* ctx,const std::wstring& dir) {
    for(int k=0;k<kEyeInputs;++k) {
        auto* texture=g_eyeInputs[k];if(!texture)continue;
        D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
        uint32_t bytes=0;
        switch(d.Format) {
        case DXGI_FORMAT_R8_UNORM:bytes=1;break;
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
            ctx->Unmap(texture,0);
        }
        texture->Release();g_eyeInputs[k]=nullptr;
    }
    celestialMotionWriteDump(ctx,dir.c_str(),g_eyeRunStamp);
    uiDepthHoloWriteDump(ctx,dir.c_str(),g_eyeRunStamp);
}

bool stageEyeCrop(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, ID3D11Texture2D** slot, uint32_t* cwOut,
                  uint32_t* chOut, const uint32_t* region = nullptr,
                  uint32_t wantW = kEyeCrop, uint32_t wantH = kEyeCrop) {
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
    return true;
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
        if (!ring[i]) continue;
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_%c%02d.bmp", dir.c_str(), g_eyeRunStamp,
                     treated ? L'T' : L'C', i);
        D3D11_TEXTURE2D_DESC sd{};
        ring[i]->GetDesc(&sd);
        if (writeEyeBmp(ctx, ring[i], sd, 0, path)) ++wrote;
        if (paired && g_eyeRawTaken[i] && g_eyeRawStaging[i]) {
            _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_C%02d.bmp", dir.c_str(), g_eyeRunStamp, i);
            g_eyeRawStaging[i]->GetDesc(&sd);
            if (writeEyeBmp(ctx, g_eyeRawStaging[i], sd, 0, path)) ++wroteTreated;
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
    if (!tex || k < 0 || k >= kEyeRun) { g_eyeRunLeft = 0; return; }
    if (k == 0) stageEyeRun(ctx, tex, g_eyeRunStaging, 0);
    uint32_t cw = 0, ch = 0;
    uint32_t wantW=kEyeCrop, wantH=kEyeCrop;
    if (g_eyeRunPaired && g_eyeRawTaken[k] && g_eyeRawInputW[k] && g_eyeRawInputH[k]) {
        // Under DLSS, 1400 output pixels show less of the scene than 1400
        // input pixels. Preserve angular coverage so both sequences include
        // the same station panels. Each image retains its native scale.
        D3D11_TEXTURE2D_DESC raw{}, output{};
        g_eyeRawStaging[k]->GetDesc(&raw); tex->GetDesc(&output);
        wantW=static_cast<uint32_t>((static_cast<uint64_t>(raw.Width)*output.Width+g_eyeRawInputW[k]-1)/g_eyeRawInputW[k]);
        wantH=static_cast<uint32_t>((static_cast<uint64_t>(raw.Height)*output.Height+g_eyeRawInputH[k]-1)/g_eyeRawInputH[k]);
    }
    if (!stageEyeCrop(ctx, tex, &g_eyeTreatedStaging[k], &cw, &ch, nullptr, wantW, wantH)) { g_eyeRunLeft = 0; return; }
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
// The body's occupancy grid on the GPU (object_probe.h): one for both
// eyes, uploaded when the probe's version moves.
ID3D11Texture3D*          g_bodyGrid = nullptr;
ID3D11ShaderResourceView* g_bodyGridSrv = nullptr;
uint32_t                  g_bodyGridVersion = 0;

bool ensureBodyGrid(ID3D11Device* dev, ID3D11DeviceContext* ctx, const ObjectMotion& om) {
    if (!dev || !ctx || !om.grid) return false;
    if (!g_bodyGrid) {
        D3D11_TEXTURE3D_DESC td{};
        td.Width = td.Height = td.Depth = kObjectGrid;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture3D(&td, nullptr, &g_bodyGrid)) || !g_bodyGrid) {
            g_bodyGrid = nullptr;
            return false;
        }
        if (FAILED(dev->CreateShaderResourceView(g_bodyGrid, nullptr, &g_bodyGridSrv)) || !g_bodyGridSrv) {
            g_bodyGridSrv = nullptr;
            g_bodyGrid->Release();
            g_bodyGrid = nullptr;
            return false;
        }
        g_bodyGridVersion = 0;
    }
    if (om.gridVersion != g_bodyGridVersion) {
        ctx->UpdateSubresource(g_bodyGrid, 0, nullptr, om.grid, kObjectGrid, kObjectGrid * kObjectGrid);
        g_bodyGridVersion = om.gridVersion;
    }
    return true;
}

// THE STEPPED PARTS' cells (object_probe.h): this frame's stamps over the
// worker's grid, uploaded as the box that holds them -- a few kilobytes a
// frame against the grid's two megabytes -- widened to last frame's box so
// the cells stamped then and not now go back to the worker's bytes.
D3D11_BOX g_steppedBox = {};
bool g_steppedBoxValid = false;
std::vector<uint8_t> g_steppedScratch;
void applySteppedCells(ID3D11DeviceContext* ctx, const ObjectMotion& om, const SteppedCell* cells, uint32_t n) {
    if (!g_bodyGrid || !om.grid) return;
    const uint32_t gn = kObjectGrid;
    D3D11_BOX box{};
    if (n) {
        uint32_t lo[3] = {gn, gn, gn}, hi[3] = {0, 0, 0};
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t c[3] = {cells[i].index % gn, (cells[i].index / gn) % gn, cells[i].index / (gn * gn)};
            for (int k = 0; k < 3; ++k) {
                if (c[k] < lo[k]) lo[k] = c[k];
                if (c[k] > hi[k]) hi[k] = c[k];
            }
        }
        box.left = lo[0]; box.right = hi[0] + 1;
        box.top = lo[1]; box.bottom = hi[1] + 1;
        box.front = lo[2]; box.back = hi[2] + 1;
        if (g_steppedBoxValid) {
            if (g_steppedBox.left < box.left) box.left = g_steppedBox.left;
            if (g_steppedBox.right > box.right) box.right = g_steppedBox.right;
            if (g_steppedBox.top < box.top) box.top = g_steppedBox.top;
            if (g_steppedBox.bottom > box.bottom) box.bottom = g_steppedBox.bottom;
            if (g_steppedBox.front < box.front) box.front = g_steppedBox.front;
            if (g_steppedBox.back > box.back) box.back = g_steppedBox.back;
        }
    } else if (g_steppedBoxValid) {
        box = g_steppedBox;
    } else {
        return;
    }
    const uint32_t w = box.right - box.left, h = box.bottom - box.top, d = box.back - box.front;
    if (!w || !h || !d || box.right > gn || box.bottom > gn || box.back > gn) { g_steppedBoxValid = false; return; }
    g_steppedScratch.resize(static_cast<size_t>(w) * h * d);
    for (uint32_t z = 0; z < d; ++z) {
        for (uint32_t y = 0; y < h; ++y) {
            memcpy(&g_steppedScratch[(static_cast<size_t>(z) * h + y) * w],
                   om.grid + (static_cast<size_t>(box.front + z) * gn + (box.top + y)) * gn + box.left, w);
        }
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t x = cells[i].index % gn, y = (cells[i].index / gn) % gn, z = cells[i].index / (gn * gn);
        g_steppedScratch[(static_cast<size_t>(z - box.front) * h + (y - box.top)) * w + (x - box.left)] = cells[i].value;
    }
    ctx->UpdateSubresource(g_bodyGrid, 0, &box, g_steppedScratch.data(), w, w * h);
    if (n) {
        g_steppedBox = box;
        g_steppedBoxValid = true;
    } else {
        g_steppedBoxValid = false;
    }
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
float    g_foveaEdgeDeg = 6.0f;    // advanced.temporal_aa_fovea_edge: the blend band, in degrees
float    g_peripheryCalm = 0.4f;   // advanced.temporal_aa_periphery_calm: how much the own history is eased toward the periphery (0 uniform, 1 max), the sharp periphery only
bool     g_periphSteady = true;    // advanced.temporal_aa_periphery: steady (NVIDIA's DLAA on a reduced copy) or sharp (the own history at full size)
float    g_periphScale = 0.5f;     // advanced.temporal_aa_periphery_scale: the steady periphery's size as a fraction of the output each way
bool     g_foveaRound = true;      // advanced.temporal_aa_fovea_shape: round (a disc) or square (the crop)
float    g_foveaDistance = 0.0f;   // advanced.temporal_aa_fovea_distance: where the two eyes' discs meet in depth, metres (0 = infinity: the straight-ahead point)
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
    bool        valid = false;
};
// 256: in space the game writes the block over a hundred times a frame
// (114 measured 2026-09-04), and a ring of 48 had lost the frame's early
// writes -- the eyes' among them, drawn before the reflections -- by
// the time the frame was chosen.
constexpr int kRowsRing = 256;
RowsWrite   g_rowsRing[kRowsRing];
uint32_t    g_rowsSeq = 0;           // writes ever, the ring's clock
const void* g_boundBuf = nullptr;    // the object bound at this frame's first scene draw
bool        g_boundSeen = false;
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
float       g_lastGoodC[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // the last accepted ship delta
float       g_lastGoodTv[3] = {};                            // ...and its translation term
bool        g_lastGoodValid = false;
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
    int bestIdx = -1, fallIdx = -1;
    uint32_t bestSeq = 0, fallSeq = 0;
    bool bestBound = false, fallBound = false;
    uint32_t count = 0;
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
        bool continuous = false;
        if (g_prevValid) {
            float rn[9], d[9];
            temporalRot3Of34(w.rows, rn);
            temporalMul3(rpT, rn, d);
            continuous = temporalRotationAngleDeg(d) < 3.0f;
        }
        if (continuous) {
            // A write identical to last frame's chosen rows is the game's
            // own last-view block (nearly every frame in space carried one,
            // a head-turn's angle from the current; 2026-09-04): kept only
            // as the fallback, for a camera that truly stood still.
            if (g_prevValid && memcmp(w.rows, g_prevRows, sizeof(w.rows)) == 0) {
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
    g_candSumCount += count;
    if (twinIdx >= 0) {
        if (bestIdx >= 0) ++g_twinFrames;
        else { bestIdx = twinIdx; bestSeq = twinSeq; bestBound = twinBound; }
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
        ++g_chooseRefollow;
    } else if (pick >= 0) {
        if (bestBound) ++g_chooseBound; else ++g_chooseOther;
    } else if (fallIdx >= 0) {
        pick = fallIdx;
        ++g_chooseResync;
    } else {
        ++g_chooseNone;
    }
    if (pick >= 0) {
        memcpy(g_curRows, g_rowsRing[pick].rows, sizeof(g_curRows));
        g_curProj[0] = g_rowsRing[pick].proj[0];
        g_curProj[1] = g_rowsRing[pick].proj[1];
        g_curValid = true;
        g_curRowsBound = g_boundSeen && g_rowsRing[pick].buf == g_boundBuf;
        // The object probe stamps its pairs with THIS camera (the frame's
        // chosen rows), not the scene buffer's end-of-frame contents, which
        // are whichever camera wrote it last -- another's, often enough that
        // the body's frame test stood the body down for 16-25 frames an
        // interval with nothing to carry it over (2026-09-09 05:48).
        const float cam[3] = {g_curRows[3], g_curRows[7], g_curRows[11]};
        objectProbeNoteCamera(cam);
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
bool ensureMoverPair(ID3D11Device* dev, EyeState& e, uint32_t w, uint32_t h) {
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
    }
    if (!e.dlMask && maskFormatOk(dev)) {
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

void* temporalInner(void* srcTex, int eye, const float* bounds,
                    const float* tanNow, const float* tanPrev, float jxNow,
                    float jyNow, const float* deltaHead, const float* headTrans,
                    const float* headTransSwapped, float nearZ, float farZ,
                    float headDeg, int motion, float blend, float clampSigma,
                    unsigned outW, unsigned outH, unsigned flags) {
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
            uiDepthTemporalDepth(sd.Width, sd.Height, eye, scene, &uiDepthSrv);
            celestialMotionViews(scene, terrainSrvs);
            uiDepthHoloMotion(eye,scene,holoSrvs);
            scene->Release();
        }
    }

    // Input identity is independent of native fallback resource allocation.
    if (ok) {
        EyeState& e = *eptr;
        if (e.w != w || e.h != h || e.outFmt != sd.Format || e.histFmt != g_histFmt) {
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
            e.haveHistory=e.dlHaveHistory=e.zPrevValid=false;
            e.screenHistory=screenSrv!=nullptr;
        }
        if (flags & 1u) {
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
        auto worldFromRows = [](const float prev[12], const float now[12],
                                float W[9], float tv[3], float camMove[3]) {
            float Rp[9], Rn[9], RpT[9];
            temporalRot3Of34(prev, Rp);
            temporalRot3Of34(now, Rn);
            temporalTranspose3(Rp, RpT);
            const float colP[3] = {prev[3], prev[7], prev[11]};
            const float colN[3] = {now[3], now[7], now[11]};
            temporalMul3(RpT, Rn, W);
            const float dc[3] = {colN[0] - colP[0], colN[1] - colP[1], colN[2] - colP[2]};
            temporalApply3(RpT, dc, tv);
            for (int i = 0; i < 3; ++i) camMove[i] = dc[i];
            // The game's view space runs z forward (DirectX), the runtime's
            // eye space z back. A rotation read in the one and applied in
            // the other has its pitch and yaw reversed and its roll kept,
            // which is exactly what the regression measured: over a dozen
            // intervals in space the rows turned -1 times the head about x
            // and y and +1 about z (k = -2, -2, 0; 2026-09-04), and the far
            // plane, on this delta alone, moved the wrong way by the head's
            // whole turn -- the sky's smear, and the station's under a head
            // turn, both gone with the world path off. Conjugating by the
            // z flip carries the delta into the eye's frame; the translation
            // term takes the same flip (a reflection, not a half turn: the
            // still-ship regression on the third line says which).
            W[2] = -W[2];
            W[5] = -W[5];
            W[6] = -W[6];
            W[7] = -W[7];
            tv[2] = -tv[2];
        };
        if (candValid[2]) {
            float camMove[3];
            worldFromRows(g_prevRows, g_curRows, worldDelta, tvCam, camMove);
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
            // Plausibility, per frame: the rows' delta is the head's plus the
            // ship's turn, and no ship turns 270 degrees a second; a delta
            // beyond 3 degrees from the head's is another camera's rows or
            // a stale latch, and last frame's accepted delta is carried in
            // its place (a far better guess than the head alone, which
            // smeared the world on every dropped frame). A jump over 50 m
            // is the floating origin moving: only the translation is dropped.
            // The jump is dropped BEFORE the last-good store, so a jump never
            // becomes the translation a later dropped frame carries: stored
            // first, a jump of hundreds of metres to tens of kilometres was
            // carried into the next dropped frame and moved every pixel with
            // a depth on the world path by it for one frame (the review of
            // 2026-09-04, F3). A jump frame keeps the last plausible
            // translation as its last-good, and a carried figure over 50 m is
            // counted so the invariant has a witness on the line.
            const bool jump = move >= 50.0;
            if (jump) {
                for (int i = 0; i < 3; ++i) tvCam[i] = 0.0f;
                ++g_camDropMove;
                // The body's path: the jump's vector, summed, carries the
                // held body into the new frame (g_bodyShift says how); a
                // flip's return adds the opposite vector back. ONCE A SCENE
                // FRAME: this runs for each eye on the rows chosen once a
                // frame, and until the review of 2026-09-10 the second eye
                // added the same jump again -- a 13 km move became 26 km,
                // the agreement gate (kBodyFrameM) refused the held pair,
                // and the station fell to the camera's path until a new pair
                // agreed unshifted: the seventeen stand-downs an interval on
                // the boost flights of 06:21 and 06:36.
                if (g_bodyShiftFrame != g_rowsFrame) {
                    for (int i = 0; i < 3; ++i) g_bodyShift[i] += camMove[i];
                    memcpy(g_bodyOriginStep, camMove, sizeof(g_bodyOriginStep));
                    g_bodyShiftFrame = g_rowsFrame;
                }
            }
            if (diffDeg > 3.0f) {
                ++g_camDropRot;
                if (g_lastGoodValid) {
                    memcpy(worldDelta, g_lastGoodC, sizeof(worldDelta));
                    memcpy(tvCam, g_lastGoodTv, sizeof(tvCam));
                    memcpy(cand[2], worldDelta, sizeof(worldDelta));
                    ++g_camCarried;
                    const double carried = sqrt(static_cast<double>(tvCam[0]) * tvCam[0] +
                                                static_cast<double>(tvCam[1]) * tvCam[1] +
                                                static_cast<double>(tvCam[2]) * tvCam[2]);
                    if (carried >= 50.0) ++g_camCarriedJump;
                } else {
                    candValid[2] = false;
                    worldValid = false;
                }
            } else {
                memcpy(g_lastGoodC, worldDelta, sizeof(g_lastGoodC));
                if (!jump) memcpy(g_lastGoodTv, tvCam, sizeof(g_lastGoodTv));
                g_lastGoodValid = true;
            }
            // The body's path takes the raw rows, so a frame whose delta the
            // world path carried (another camera's rotation, or a stale
            // latch) does not get the body either: its pixels take the
            // world path's carried delta with the body's turn unvectored
            // for the frame, rather than a body composed with rows that are
            // not the view's.
            g_bodyRowsOk = diffDeg <= 3.0f;
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
            (trainedWanted && (e.dlHaveHistory || (g_foveaDeg > 0.0f && (e.foveaHaveHistory || e.prHaveHistory))) &&
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
        // ...and only in a REAL scene: fifty draws into the scene pair a
        // frame. The main menu's backdrop is a pre-rendered image at the far
        // plane drawn with one or two, and its camera does not follow the
        // head, so the world path detached its hangar wall (2026-09-04).
        const bool worldOn = depthMotion && haveDepth && g_shipMetres > 0.0f &&
                             candValid[2] && worldValid && sceneDraws >= 50u &&
                             g_rowsFollow >= 0;
        for (int i = 0; i < 3; ++i) p.tvCam[i] = worldOn ? tvCam[i] : 0.0f;
        p.tvCam[3] = worldOn ? 1.0f : 0.0f;
        // Tier 2: the body's path, the camera's composed with the dominant
        // body's own turn (temporalBodyPath), on whenever the world path is
        // and the pool has given a body. The trained block below may still
        // stand it down for a frame it cannot compare against.
        bool bodyOn = false;
        uint32_t shipsOn = 0;   // moving ships handed to the shader this frame
        // A jump this frame (the shift just took it), or rows the world path
        // did not take (another camera's, a stale latch): the body composes
        // with the camera delta the world path carries this frame instead
        // of the rows (temporalBodyPathCarried), and no longer stands down.
        const bool jumpedNow = g_bodyShiftFrame == g_rowsFrame;
        const float* bodyOriginStep = jumpedNow ? g_bodyOriginStep : nullptr;
        const bool carriedRows = !g_bodyRowsOk || jumpedNow;
        // The pair's frame against the rows' (kBodyFrameM says why): the
        // pair's own camera position rides in the motion record. Unshifted
        // agreement clears the shift (the pair is in this frame); shifted
        // agreement carries the body over by it; neither stands the body
        // down.
        float bodyShift[3] = {0.0f, 0.0f, 0.0f};
        float shipShift[3] = {0.0f, 0.0f, 0.0f};
        // ...for the station body (own) and for the ships, each with its own
        // pair's camera and its own shift. Only the station's call may clear
        // the accumulated jump: the ships' pair is often newer than the
        // body's (a pair whose largest cluster was another object keeps the
        // last body, and its ships), and on the ships' first flights the
        // ships' call, agreeing unshifted in the new frame, cleared the shift
        // the body still needed, and the body stood down until its next pair.
        auto bodyFrameAgrees = [&](const float* pairCam, float* shiftOut, bool own) {
            static uint32_t s_frameHold = 0;
            static uint32_t s_frameUsed = 0;
            static bool s_held = false;
            static uint32_t s_holdStart = 0;
            const double cn[3] = {g_curRows[3], g_curRows[7], g_curRows[11]};
            double dNo = 0.0, dSh = 0.0;
            for (int i = 0; i < 3; ++i) {
                const double a = cn[i] - pairCam[i];
                const double b = a - g_bodyShift[i];
                dNo += a * a;
                dSh += b * b;
            }
            const double lim = static_cast<double>(kBodyFrameM) * kBodyFrameM;
            if (own && s_held && (dNo < lim || dSh < lim)) {
                Log::get().note("temporal aa: body frame gate recovered at frame %u after %u frames; "
                                "grid %u age %u, distance %.1f m, shifted %.1f m.",
                                g_rowsFrame, g_rowsFrame - s_holdStart, g_frameBody.gridVersion,
                                g_frameBody.age, sqrt(dNo), sqrt(dSh));
                s_held = false;
            }
            if (dNo < lim) {
                if (own) {
                    for (int i = 0; i < 3; ++i) g_bodyShift[i] = 0.0f;
                }
                for (int i = 0; i < 3; ++i) shiftOut[i] = 0.0f;
                return true;
            }
            if (dSh < lim) {
                for (int i = 0; i < 3; ++i) shiftOut[i] = g_bodyShift[i];
                if (own && s_frameUsed != g_rowsFrame) {
                    s_frameUsed = g_rowsFrame;
                    ++g_bodyShiftUsed;
                }
                return true;
            }
            if (own && s_frameHold != g_rowsFrame) {
                if (!s_held) {
                    s_held = true;
                    s_holdStart = g_rowsFrame;
                    Log::get().note("temporal aa: body frame gate rejected at frame %u; grid %u age %u, "
                                    "distance %.1f m, shifted %.1f m; camera (%.3f %.3f %.3f), "
                                    "pair (%.3f %.3f %.3f), shift (%.3f %.3f %.3f), jump %d rowsOk %d.",
                                    g_rowsFrame, g_frameBody.gridVersion, g_frameBody.age, sqrt(dNo), sqrt(dSh),
                                    cn[0], cn[1], cn[2], pairCam[0], pairCam[1], pairCam[2],
                                    g_bodyShift[0], g_bodyShift[1], g_bodyShift[2], jumpedNow, g_bodyRowsOk);
                }
                s_frameHold = g_rowsFrame;
                ++g_bodyFrameHolds;
            }
            return false;
        };
        if (g_objectsOn && worldOn) {
            // The body's motion over THIS frame's length: its rates
            // times the interval since the last frame (a station turns
            // at a constant rate; a pair measured on a long frame is a
            // larger turn, and the frame it is applied to may be short).
            LARGE_INTEGER qNowB{}, qFreqB{};
            QueryPerformanceCounter(&qNowB);
            QueryPerformanceFrequency(&qFreqB);
            static LONGLONG s_lastBodyQpc = 0;
            static uint32_t s_lastBodyFrame = 0;
            float dtMs = 11.1f;
            if (s_lastBodyQpc && qFreqB.QuadPart > 0 && s_lastBodyFrame != g_rowsFrame) {
                dtMs = static_cast<float>(static_cast<double>(qNowB.QuadPart - s_lastBodyQpc) * 1000.0 /
                                          static_cast<double>(qFreqB.QuadPart));
            }
            if (s_lastBodyFrame != g_rowsFrame) {
                s_lastBodyQpc = qNowB.QuadPart;
                s_lastBodyFrame = g_rowsFrame;
                // The frame's length, eased: at a steady 90 Hz the
                // interval is a constant with a little scheduling
                // noise on it, and the body's turn should not carry
                // that noise; a frame a fifth longer or shorter than
                // the run is a real one and taken as it is.
                const float m = (dtMs >= 5.0f && dtMs <= 50.0f) ? dtMs : 11.1f;
                if (m > 1.2f * g_bodyDtMs || m < 0.8f * g_bodyDtMs) {
                    g_bodyDtMs = m;
                } else {
                    g_bodyDtMs = 0.75f * g_bodyDtMs + 0.25f * m;
                }
            }
            if (g_frameBodyFrame != g_rowsFrame) {
                g_frameBodyFrame = g_rowsFrame;
                g_frameBodyValid = objectMotionGet(&g_frameBody);
            }
            const ObjectMotion& om = g_frameBody;
            if (g_frameBodyValid && bodyFrameAgrees(om.camPos, bodyShift, true) && ensureBodyGrid(dev, ctx, om)) {
                const float wF[3] = {om.omegaPerMs[0] * g_bodyDtMs, om.omegaPerMs[1] * g_bodyDtMs,
                                     om.omegaPerMs[2] * g_bodyDtMs};
                const float tF[3] = {om.tPerMs[0] * g_bodyDtMs, om.tPerMs[1] * g_bodyDtMs,
                                     om.tPerMs[2] * g_bodyDtMs};
                float Rf[9];
                temporalRodrigues(wF, Rf);
                // The origin's move since the pair, if any (bodyFrameAgrees):
                // the translation term shifts by (I - R) times it.
                float tFs[3];
                for (int k = 0; k < 3; ++k) {
                    const float rs = Rf[k * 3 + 0] * bodyShift[0] + Rf[k * 3 + 1] * bodyShift[1] +
                                     Rf[k * 3 + 2] * bodyShift[2];
                    tFs[k] = tF[k] + bodyShift[k] - rs;
                }
                float W[9], tv[3];
                if (!carriedRows) {
                    temporalBodyPath(g_prevRows, g_curRows, Rf, tFs, W, tv);
                } else {
                    temporalBodyPathCarried(g_prevRows, Rf, tFs, worldDelta, tvCam, W, tv, bodyOriginStep);
                    static uint32_t s_carriedFrame = 0;
                    if (s_carriedFrame != g_rowsFrame) {
                        s_carriedFrame = g_rowsFrame;
                        ++g_bodyRowsHolds;
                    }
                }
                float* rows[3] = {p.st0, p.st1, p.st2};
                float* wrows[3] = {p.wR0, p.wR1, p.wR2};
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        rows[r][c] = W[r * 3 + c];
                        wrows[r][c] = g_curRows[r * 4 + c];
                    }
                    rows[r][3] = 0.0f;
                    wrows[r][3] = g_curRows[r * 4 + 3];
                }
                for (int i = 0; i < 3; ++i) {
                    p.tvSt[i] = tv[i];
                    p.box0[i] = om.bmin[i] + bodyShift[i];
                    p.box1[i] = om.bmax[i] + bodyShift[i];
                }
                p.box0[3] = p.box1[3] = 0.0f;
                // THE SECOND BODY's path (ObjectMotion::body2), composed as
                // the body's is, with the same shift; its cells hold 128.
                if (om.body2) {
                    const float w2F[3] = {om.omega2PerMs[0] * g_bodyDtMs, om.omega2PerMs[1] * g_bodyDtMs,
                                          om.omega2PerMs[2] * g_bodyDtMs};
                    const float t2F[3] = {om.t2PerMs[0] * g_bodyDtMs, om.t2PerMs[1] * g_bodyDtMs,
                                          om.t2PerMs[2] * g_bodyDtMs};
                    float R2f[9];
                    temporalRodrigues(w2F, R2f);
                    float t2Fs[3];
                    for (int k = 0; k < 3; ++k) {
                        const float rs = R2f[k * 3 + 0] * bodyShift[0] + R2f[k * 3 + 1] * bodyShift[1] +
                                         R2f[k * 3 + 2] * bodyShift[2];
                        t2Fs[k] = t2F[k] + bodyShift[k] - rs;
                    }
                    float W2[9], tv2[3];
                    if (!carriedRows) {
                        temporalBodyPath(g_prevRows, g_curRows, R2f, t2Fs, W2, tv2);
                    } else {
                        temporalBodyPathCarried(g_prevRows, R2f, t2Fs, worldDelta, tvCam, W2, tv2, bodyOriginStep);
                    }
                    float* rows2[3] = {p.st2_0, p.st2_1, p.st2_2};
                    for (int r = 0; r < 3; ++r) {
                        for (int c = 0; c < 3; ++c) rows2[r][c] = W2[r * 3 + c];
                        rows2[r][3] = 0.0f;
                    }
                    for (int i = 0; i < 3; ++i) p.tv2St[i] = tv2[i];
                    p.tv2St[3] = 1.0f;
                    ++g_body2Frames;
                }
                // THE STEPPED PARTS' table (object_probe.h): the body's path
                // for every multiple m of its turn from kSteppedMin to
                // kSteppedMax, composed as the body's own is. The turn's axis
                // point c and its axial part come from the body's own (R, t):
                // t = (I - R) c + t_par with c = t_perp / 2 + (axis x t_perp) /
                // (2 tan(theta / 2)), so the m-th is (I - R^m) c + m t_par, and
                // m = 1 gives t back.
                if (kObjectSteppedMotionEnabled) {
                    const float th = sqrtf(wF[0] * wF[0] + wF[1] * wF[1] + wF[2] * wF[2]);
                    float ah[3] = {0.0f, 0.0f, 0.0f}, tPar[3] = {0.0f, 0.0f, 0.0f}, cAx[3] = {0.0f, 0.0f, 0.0f};
                    if (th > 1e-7f) {
                        for (int k = 0; k < 3; ++k) ah[k] = wF[k] / th;
                        const float along = tF[0] * ah[0] + tF[1] * ah[1] + tF[2] * ah[2];
                        float tPerp[3];
                        for (int k = 0; k < 3; ++k) {
                            tPar[k] = along * ah[k];
                            tPerp[k] = tF[k] - tPar[k];
                        }
                        const float cx[3] = {ah[1] * tPerp[2] - ah[2] * tPerp[1], ah[2] * tPerp[0] - ah[0] * tPerp[2],
                                             ah[0] * tPerp[1] - ah[1] * tPerp[0]};
                        // sin / (2 - 2 cos) is 1 / (2 tan(theta / 2)), and the
                        // second form keeps its digits: at the station's turn a
                        // frame (0.00075 rad) 2 - 2 cos loses six percent to
                        // float32's spacing near one, 240 m on a 4 km axis point.
                        const float ht = tanf(0.5f * th);
                        const float k2 = ht > 1e-12f ? 0.5f / ht : 0.0f;
                        for (int k = 0; k < 3; ++k) cAx[k] = 0.5f * tPerp[k] + k2 * cx[k];
                    } else {
                        for (int k = 0; k < 3; ++k) tPar[k] = tF[k];
                    }
                    for (int m = kSteppedMin; m <= kSteppedMax; ++m) {
                        const int e = m - kSteppedMin;
                        const float fm = static_cast<float>(m);
                        const float wM[3] = {wF[0] * fm, wF[1] * fm, wF[2] * fm};
                        float RM[9];
                        temporalRodrigues(wM, RM);
                        float tMs[3];
                        for (int k = 0; k < 3; ++k) {
                            const float rc = RM[k * 3 + 0] * cAx[0] + RM[k * 3 + 1] * cAx[1] + RM[k * 3 + 2] * cAx[2];
                            const float rs = RM[k * 3 + 0] * bodyShift[0] + RM[k * 3 + 1] * bodyShift[1] +
                                             RM[k * 3 + 2] * bodyShift[2];
                            tMs[k] = (cAx[k] - rc + tPar[k] * fm) + bodyShift[k] - rs;
                        }
                        float WM[9], tvM[3];
                        if (!carriedRows) {
                            temporalBodyPath(g_prevRows, g_curRows, RM, tMs, WM, tvM);
                        } else {
                            temporalBodyPathCarried(g_prevRows, RM, tMs, worldDelta, tvCam, WM, tvM, bodyOriginStep);
                        }
                        for (int r = 0; r < 3; ++r) {
                            for (int c = 0; c < 3; ++c) p.st3R[e * 3 + r][c] = WM[r * 3 + c];
                            p.st3R[e * 3 + r][3] = 0.0f;
                        }
                        for (int k = 0; k < 3; ++k) p.tv3[e][k] = tvM[k];
                        p.tv3[e][3] = 1.0f;
                    }
                }
                // ...and their cells this frame, stamped over the grid.
                if (kObjectSteppedMotionEnabled) {
                    const SteppedCell* cells = nullptr;
                    const uint32_t nCells = objectSteppedCells(&cells);
                    if (kObjectSteppedMotionEnabled) {
                        applySteppedCells(ctx, om, cells, nCells);
                        if (nCells) ++g_steppedFrames;
                    }
                }
                bodyOn = true;
                g_bodyLast = om;
                g_bodyLastValid = true;
                if (!g_objectsNoted) {
                    g_objectsNoted = true;
                    Log::get().note(
                        "temporal aa: the dominant body's own path is on -- the instance pool's largest "
                        "rigid cluster (%u records, %.0f%% of the pool's movers, fit to %.3f m) turns "
                        "%.4f deg and moves %.3f m over its pair's %.1f ms in the world, its parts fill a "
                        "box %.0f x %.0f x %.0f m, and each world-path pixel whose depth places it within "
                        "%.0f m of a part takes that path over the camera's, scaled to the frame's own "
                        "length. The registration line's share says how many do.",
                        om.records, 100.0 * static_cast<double>(om.share), static_cast<double>(om.rms),
                        static_cast<double>(temporalRotationAngleDeg(om.R)),
                        sqrt(static_cast<double>(om.t[0]) * om.t[0] + static_cast<double>(om.t[1]) * om.t[1] +
                             static_cast<double>(om.t[2]) * om.t[2]),
                        static_cast<double>(om.dtMs),
                        static_cast<double>(om.bmax[0] - om.bmin[0]),
                        static_cast<double>(om.bmax[1] - om.bmin[1]),
                        static_cast<double>(om.bmax[2] - om.bmin[2]),
                        static_cast<double>(g_objectsReach));
                }
            }
            // The moving ships (object_probe.h): each on the same path as the
            // body -- its own rates over this frame's length, the same frame
            // test and origin shift, the same composition on carried frames --
            // with its box in place of a grid; the shader tests the boxes
            // before the grid (insideShip says why).
            if (g_shipsRangeM > 0.0f) {
                ObjectShip ships[kObjectShipsMax];
                float shipCam[3] = {0.0f, 0.0f, 0.0f};
                uint32_t shipAge = 0;
                const uint32_t nShips = objectShipsGet(ships, kObjectShipsMax, shipCam, &shipAge);
                if (nShips && bodyFrameAgrees(shipCam, shipShift, false)) {
                    for (uint32_t i = 0; i < nShips && shipsOn < kObjectShipsMax; ++i) {
                        const ObjectShip& sh = ships[i];
                        const float wS[3] = {sh.omegaPerMs[0] * g_bodyDtMs, sh.omegaPerMs[1] * g_bodyDtMs,
                                             sh.omegaPerMs[2] * g_bodyDtMs};
                        const float tS[3] = {sh.tPerMs[0] * g_bodyDtMs, sh.tPerMs[1] * g_bodyDtMs,
                                             sh.tPerMs[2] * g_bodyDtMs};
                        float Rs[9];
                        temporalRodrigues(wS, Rs);
                        float tSs[3];
                        for (int k = 0; k < 3; ++k) {
                            const float rs = Rs[k * 3 + 0] * shipShift[0] + Rs[k * 3 + 1] * shipShift[1] +
                                             Rs[k * 3 + 2] * shipShift[2];
                            tSs[k] = tS[k] + shipShift[k] - rs;
                        }
                        float Ws[9], tvs[3];
                        if (!carriedRows) {
                            temporalBodyPath(g_prevRows, g_curRows, Rs, tSs, Ws, tvs);
                        } else {
                            temporalBodyPathCarried(g_prevRows, Rs, tSs, worldDelta, tvCam, Ws, tvs, bodyOriginStep);
                        }
                        for (int r = 0; r < 3; ++r) {
                            for (int c = 0; c < 3; ++c) p.shR[shipsOn * 3 + r][c] = Ws[r * 3 + c];
                            p.shR[shipsOn * 3 + r][3] = 0.0f;
                        }
                        // The box, carried to THIS frame. The pair's positions are
                        // up to eleven frames old by the time they are applied (the
                        // copy's three frames to the diff and eight to the next
                        // pair), and a ship at five metres a frame has left its own
                        // padded box in six: the ships' first flight (2026-09-09
                        // 11:19) had a ship in hand at two hundred metres and claimed
                        // a hundredth of a percent of pixels. The parts move by the
                        // ship's translation a frame (p_now = R^-1 (p_prev - t), minus
                        // t to the turn's approximation), and the box widens by a
                        // fifth of the way carried plus five metres for what those
                        // frames may have changed.
                        const float lagMs = static_cast<float>(shipAge) * g_bodyDtMs;
                        float carry[3];
                        for (int k = 0; k < 3; ++k) carry[k] = sh.movePerMs[k] * lagMs;   // the centroid's own motion (ObjectShip says)
                        const float grow = 5.0f + 0.2f * sqrtf(carry[0] * carry[0] + carry[1] * carry[1] +
                                                               carry[2] * carry[2]);
                        for (int k = 0; k < 3; ++k) {
                            p.shTv[shipsOn][k] = tvs[k];
                            p.shBox0[shipsOn][k] = sh.bmin[k] + shipShift[k] + carry[k] - grow;
                            p.shBox1[shipsOn][k] = sh.bmax[k] + shipShift[k] + carry[k] + grow;
                        }
                        p.shTv[shipsOn][3] = p.shBox1[shipsOn][3] = 0.0f;
                        // The parts and the tail, carried the same way; the tail
                        // plane's offset moves by the carry's component along it.
                        const uint32_t np = sh.partCount < kObjectShipParts ? sh.partCount : kObjectShipParts;
                        p.shBox0[shipsOn][3] = static_cast<float>(np);
                        for (uint32_t j = 0; j < np; ++j) {
                            float* pt = p.shParts[shipsOn * kObjectShipParts + j];
                            for (int k = 0; k < 3; ++k) pt[k] = sh.parts[j][k] + shipShift[k] + carry[k];
                            pt[3] = 0.0f;
                        }
                        float along = 0.0f;
                        for (int k = 0; k < 3; ++k) {
                            p.shDir[shipsOn][k] = sh.dir[k];
                            along += (shipShift[k] + carry[k]) * sh.dir[k];
                        }
                        p.shDir[shipsOn][3] = sh.rear > -1e29f ? sh.rear + along : -1e30f;
                        // The box's footprint on the image, for the counters:
                        // its corners through this frame's rows (view = R^T
                        // (w - c), the game's z forward) and the eye's tangents;
                        // the whole image when a corner is behind the eye.
                        float rx0 = 1e9f, ry0 = 1e9f, rx1 = -1e9f, ry1 = -1e9f;
                        bool behindEye = false;
                        for (int corner = 0; corner < 8 && !behindEye; ++corner) {
                            float rel[3];
                            for (int k = 0; k < 3; ++k) {
                                const float cw = ((corner >> k) & 1) ? p.shBox1[shipsOn][k] : p.shBox0[shipsOn][k];
                                rel[k] = cw - g_curRows[k * 4 + 3];
                            }
                            float v[3];
                            for (int k = 0; k < 3; ++k) {
                                v[k] = g_curRows[0 * 4 + k] * rel[0] + g_curRows[1 * 4 + k] * rel[1] +
                                       g_curRows[2 * 4 + k] * rel[2];
                            }
                            if (v[2] <= 0.01f) {
                                behindEye = true;
                                break;
                            }
                            const float px = (v[0] / v[2] - p.tanNow[0]) / (p.tanNow[1] - p.tanNow[0]) *
                                             static_cast<float>(p.size[0]);
                            const float py = (p.tanNow[3] - v[1] / v[2]) / (p.tanNow[3] - p.tanNow[2]) *
                                             static_cast<float>(p.size[1]);
                            if (px < rx0) rx0 = px;
                            if (py < ry0) ry0 = py;
                            if (px > rx1) rx1 = px;
                            if (py > ry1) ry1 = py;
                        }
                        if (behindEye) {
                            // Empty: a box with a corner behind the eye is too
                            // near to count (the whole image counted, every sky
                            // pixel was "in a footprint" on the fourth flight).
                            rx0 = ry0 = -1.0f;
                            rx1 = ry1 = -2.0f;
                        }
                        p.shRect[shipsOn][0] = rx0;
                        p.shRect[shipsOn][1] = ry0;
                        p.shRect[shipsOn][2] = rx1;
                        p.shRect[shipsOn][3] = ry1;
                        ++shipsOn;
                    }
                    if (shipsOn && !bodyOn) {
                        // The camera rows for the shader's world point (the
                        // body's block fills them when the body is on).
                        float* wrows[3] = {p.wR0, p.wR1, p.wR2};
                        for (int r = 0; r < 3; ++r) {
                            for (int c = 0; c < 4; ++c) wrows[r][c] = g_curRows[r * 4 + c];
                        }
                    }
                    g_shipsLast = ships[0];
                    g_shipsLastAge = shipAge;
                    if (shipsOn && !g_shipsNoted) {
                        g_shipsNoted = true;
                        const float* ow = ships[0].omegaPerMs;
                        const float* ot = ships[0].movePerMs;
                        Log::get().note(
                            "temporal aa: a moving ship's own path is on -- %u ship%s within %.0f m from the "
                            "instance pool's other rigid clusters, the nearest %u parts at %.0f m (fit to "
                            "%.3f m) turning %.4f deg and moving %.3f m a frame; each world-path pixel whose "
                            "depth places it in a ship's box (its parts, padded) takes that ship's path over "
                            "the camera's and the station's. The registration line's share says how many.",
                            shipsOn, shipsOn == 1 ? "" : "s", static_cast<double>(g_shipsRangeM),
                            ships[0].records, static_cast<double>(ships[0].distM),
                            static_cast<double>(ships[0].rms),
                            static_cast<double>(sqrtf(ow[0] * ow[0] + ow[1] * ow[1] + ow[2] * ow[2]) *
                                                g_bodyDtMs * 57.2957795f),
                            static_cast<double>(sqrtf(ot[0] * ot[0] + ot[1] * ot[1] + ot[2] * ot[2]) *
                                                g_bodyDtMs));
                    }
                }
            }
        }
        p.ships[0] = static_cast<float>(shipsOn);
        p.ships[1] = p.ships[2] = p.ships[3] = 0.0f;
        g_shipsLastN = shipsOn;
        p.tvSt[3] = bodyOn ? 1.0f : 0.0f;
        p.objects[0] = g_objectsReach;
        p.objects[1] = kBodyNearM;
        p.objects[2] = kShipReachM * kShipReachM;
        p.objects[3] = 0.0f;
        p.split[0] = g_shipMetres;
        p.split[1] = static_cast<float>(g_debugMode);
        p.split[2] = g_menuMetres;
        p.split[3] = (haveDepth && sceneDraws < 50u) ? 1.0f : 0.0f;
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
                "resident (%.0f MB at %ux%u); the masked share prints on the registration line, "
                "and advanced.temporal_aa_debug = movers paints it.",
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
        auto uiFlags = [&]() {
            return static_cast<float>((uiDepthReactive()>0.0f?1u:0u) |
                (uiTrack && e.uiHistoryValid?2u:0u) | (uiTrack?4u:0u) | (terrainSrvs[0]?8u:0u) | (holoSrvs[0]?16u:0u) | (screenSrv?32u:0u));
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
            }
            if (g_eyeMotionTraceCount < kEyeRun * 4) {
                EyeMotionTrace& t = g_eyeMotionTrace[g_eyeMotionTraceCount++];
                t = {};
                t.frame = g_rowsFrame; t.eye = eye; t.flags = flags;
                t.outputWidth = outW ? outW : w; t.outputHeight = outH ? outH : h;
                t.bodyValid = g_frameBodyFrame == g_rowsFrame && g_frameBodyValid;
                if (t.bodyValid) t.body = g_frameBody;
                t.rowsOk = g_bodyRowsOk; t.jumped = jumpedNow; t.dlHistory = e.dlHaveHistory;
                t.dtMs = g_bodyDtMs;
                t.rowsBound = g_curRowsBound; t.rowsFollow = g_rowsFollow; t.sceneDraws = sceneDraws;
                memcpy(t.prevRows, g_prevRows, sizeof(t.prevRows));
                memcpy(t.nowRows, g_curRows, sizeof(t.nowRows));
                memcpy(t.shift, g_bodyShift, sizeof(t.shift));
                if (bodyOriginStep) memcpy(t.originStep, bodyOriginStep, sizeof(t.originStep));
                t.params = p;
            }
        }

        ID3D11ComputeShader* savedCs = nullptr;
        ID3D11ShaderResourceView* savedSrv[15] = {};
        ID3D11UnorderedAccessView* savedUav[7] = {};
        ID3D11Buffer* savedCb = nullptr;
        ID3D11SamplerState* savedSamp = nullptr;
        ctx->CSGetShader(&savedCs, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 15, savedSrv);
        ctx->CSGetUnorderedAccessViews(0, 7, savedUav);
        ctx->CSGetConstantBuffers(0, 1, &savedCb);
        ctx->CSGetSamplers(0, 1, &savedSamp);
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
        const bool statsWritten = diagnostics || (flags & 2u) == 0 || g_foveaDeg > 0.0f;
        const bool timingOwner = gpuTimingBind(dev, ctx) && gpuTimingAccepts(ctx);
        const int qs = gpuTimingOwns(ctx) && (statsWritten || (g_rowsFrame & 31u) == 0)
            ? acquireSlot(dev) : -1;
        if (qs >= 0) {
            auto& slot = g_slots[qs];
            slot.timing = timingOwner && slot.timer.begin(dev, ctx);
            slot.inUse = true;
            slot.timeDone = !slot.timing;
            // Only the later CopyResource makes staging readable. An aborted
            // pass must not consume a previous frame's staging contents.
            slot.statsDone = true;
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
        const bool foveaWanted = (flags & 2u) != 0 && fmtIndex == 0 && g_foveaDeg > 0.0f &&
                                 g_foveaCb != nullptr && g_debugMode == 0 && !g_foveaFailed;
        uint32_t fcx = 0, fcy = 0, fcw = 0, fch = 0;      // INPUT crop, in the render (w x h) space
        uint32_t focx = 0, focy = 0, focw = 0, foch = 0;  // OUTPUT crop, in the native (foW x foH) space
        bool foveaMode = false;
        bool foveaComposited = false;
        bool foveaEvalOk = false;   // the crop eval ran and NVIDIA accumulated: history is live
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
                float tcx = 0.0f, tcy = 0.0f;
                if (g_foveaDistance > 0.0f && (e.eyeOff[0] != 0.0f || e.eyeOff[1] != 0.0f)) {
                    tcx = -e.eyeOff[0] / g_foveaDistance;
                    tcy = -e.eyeOff[1] / g_foveaDistance;
                }
                auto cropOf = [&](uint32_t fw, uint32_t fh, uint32_t& ox, uint32_t& oy,
                                  uint32_t& ow, uint32_t& oh) -> bool {
                    const float cx = ((tcx - l) / (r - l)) * static_cast<float>(fw);
                    const float cy = ((b - tcy) / (b - t)) * static_cast<float>(fh);
                    const float halfa = tanf(g_foveaDeg * 0.5f * 0.01745329252f);
                    const float hwp = halfa * static_cast<float>(fw) / (r - l);
                    const float hhp = halfa * static_cast<float>(fh) / (b - t);
                    int x0 = static_cast<int>(cx - hwp), y0 = static_cast<int>(cy - hhp);
                    int x1 = static_cast<int>(cx + hwp + 0.5f), y1 = static_cast<int>(cy + hhp + 0.5f);
                    if (x0 < 0) x0 = 0;
                    if (y0 < 0) y0 = 0;
                    if (x1 > static_cast<int>(fw)) x1 = static_cast<int>(fw);
                    if (y1 > static_cast<int>(fh)) y1 = static_cast<int>(fh);
                    x0 &= ~1; y0 &= ~1; x1 &= ~1; y1 &= ~1;
                    const int cw = x1 - x0, ch = y1 - y0;
                    if (cw < 128 || ch < 128) return false;
                    ox = static_cast<uint32_t>(x0); oy = static_cast<uint32_t>(y0);
                    ow = static_cast<uint32_t>(cw); oh = static_cast<uint32_t>(ch);
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

        bool usedDlaa = false;
        // Tier 1's depth carry: true once a dispatch this frame wrote ZC into
        // e.dlDepth with a depth bound and a twin to swap it with, so the
        // frame's end can make it last frame's.
        bool zcWritten = false;
        // The trained path copies the colour into R8G8B8A8_UNORM, which is
        // only legal within that family (the review of 2026-09-04, F9): any
        // other family runs the pass's own history and says so once.
        if ((flags & 2u) != 0 && fmtIndex != 0 && !g_dlaaFailNoted) {
            g_dlaaFailNoted = true;
            Log::get().note(
                "temporal aa: dlaa was asked for, but the game submits %s and NVIDIA is "
                "handed R8G8B8A8, a different family. The pass's own history runs instead.",
                formatName(sd.Format));
        }
        if ((flags & 2u) != 0 && fmtIndex == 0 && !foveaMode) {
            const char* why = "";
            if (!dlaaAvailable(dev, &why)) {
                if (!g_dlaaFailNoted) {
                    g_dlaaFailNoted = true;
                    Log::get().note(
                        "temporal aa: dlaa was asked for, but %s. The pass's own "
                        "history runs instead.",
                        why);
                }
            } else {
                ID3D11ComputeShader* mvCs = motionShader(ctx, diagnostics);
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
                        if (g_moversOn) ensureMoverPair(dev, e, w, h);
                    } else {
                        releaseDl(e);
                    }
                } else if (made && g_moversOn && !e.zPrev) {
                    ensureMoverPair(dev, e, w, h);   // the mask switched on under a live set
                }
                if (made && setParams(ctx, p)) {
                    // The colour, typed, whichever way the source came.
                    D3D11_BOX box{};
                    box.left = viaCopy ? 0 : region[0];
                    box.top = viaCopy ? 0 : region[1];
                    box.front = 0;
                    box.right = box.left + w;
                    box.bottom = box.top + h;
                    box.back = 1;
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
                    // The motion vectors and the depth copy -- and, with the
                    // mover mask on, last frame's depth read at t3 and the
                    // mask written at u5 (tier 1, docs/per-object-motion.md).
                    const bool debugPaint = g_debugMode == 1 || g_debugMode == 3 || g_debugMode == 4 ||
                                            g_debugMode == 5;
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
                    // Bound for the fold when the mover mask is on, and for
                    // the body path's exclusion whenever that is on.
                    const bool wantUi = coverageMask != nullptr &&
                                        ((p.movers[0] != 0.0f && e.dlMaskUav != nullptr) || p.tvSt[3] != 0.0f ||
                                         p.ships[0] != 0.0f || uiTrack);
                    const bool uiBound = wantUi && ensureUiMaskSrv(dev, e, coverageMask);
                    if(uiTrack && e.dlSubmitUav && !g_csUiResolveTried) {
                        g_csUiResolveTried=true;
                        g_csUiResolve=shaderSwapCompileCs(ctx,kUiResolve,sizeof(kUiResolve)-1,"main","UI resolve",nullptr,"UI resolve");
                    }
                    const bool uiResolve=uiTrack && e.dlSubmitUav && g_csUiResolve && !debugPaint;
                    if(uiResolve && !e.uiResolvedHistory) e.uiHistoryValid=false;
                    p.probe[2] = uiBound ? 1.0f : 0.0f;
                    p.probe[3] = uiFlags();
                    // Modern DLSS presets ignore the bias mask. The final UI
                    // resolve writes its own influence history below; avoid
                    // the ineffective adaptive colour work in the MV shader.
                    if(uiResolve) p.probe[3]=float(uint32_t(p.probe[3])&~6u);
                    if (uiTrack) ensureBiasMask(dev,e,w,h);
                    setParams(ctx, p);
                    ID3D11ShaderResourceView* nullSrvM[15] = {};
                    ID3D11UnorderedAccessView* nullUavM[7] = {};
                    ctx->CSSetShaderResources(0, 15, nullSrvM);
                    ctx->CSSetUnorderedAccessViews(0, 7, nullUavM, nullptr);
                    ctx->CSSetShader(mvCs, nullptr, 0);
                    ID3D11ShaderResourceView* srvsM[15] = {inSrv,
                                                          probeNv ? e.dlOutSrv : e.histSrv[e.histRead],
                                                          depthSrv,
                                                          p.movers[0] != 0.0f ? e.zPrevSrv : nullptr,
                                                          uiBound ? e.uiMaskSrv : nullptr,
                                                          p.tvSt[3] != 0.0f ? g_bodyGridSrv : nullptr,
                                                          smokeSrv, uiDepthSrv,
                                                          uiTrack && e.uiHistoryValid ? e.uiHistorySrv[e.uiHistoryRead] : nullptr,
                                                          terrainSrvs[0], terrainSrvs[1], terrainSrvs[2], holoSrvs[0], holoSrvs[1], screenSrv};
                    ID3D11UnorderedAccessView* uavsM[7] = {debugPaint ? e.dlOutUav : nullptr,
                                                           nullptr, g_statsUav, e.dlMvUav,
                                                           e.dlDepthUav, e.dlMaskUav,
                                                           uiTrack && !uiResolve ? e.uiHistoryUav[1-e.uiHistoryRead] : nullptr};
                    ID3D11Buffer* cbM = g_cb;
                    ID3D11SamplerState* smpM = g_samp;
                    ctx->CSSetShaderResources(0, 15, srvsM);
                    ctx->CSSetUnorderedAccessViews(0, 7, uavsM, nullptr);
                    ctx->CSSetConstantBuffers(0, 1, &cbM);
                    ctx->CSSetSamplers(0, 1, &smpM);
                    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                    ctx->CSSetShaderResources(0, 15, nullSrvM);
                    ctx->CSSetUnorderedAccessViews(0, 7, nullUavM, nullptr);
                    if (haveDepth && e.zPrev) zcWritten = true;
                    if (uiTrack && !uiResolve) uiEvidenceWritten = true;
                    if(eye==0)stageEyeInputs(ctx,e,depthSrv,coverageMask,p.probe[2],p.probe[3]);
                    // What NVIDIA is handed: the union when the mover mask
                    // ran this frame, else the interface's alone (as before
                    // the mover mask existed), else nothing.
                    ID3D11Texture2D* biasMask = ((p.movers[0] != 0.0f || uiTrack) && e.dlMask) ? e.dlMask : reactiveMask;
                    // NVIDIA's evaluation. Its history restarts only when it is
                    // broken: this eye's first frame, a withhold (flags bit 0),
                    // rebuilt textures, or a frame the pass's own history ran in
                    // between -- never every frame (the review's F1, 2026-09-04).
                    const bool resetHist = (flags & 1u) != 0 || !e.dlHaveHistory;
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
                    if (debugPaint) {
                        // The motion, depth and mover views: the mv entry
                        // painted into the output; NVIDIA is skipped and
                        // starts afresh after.
                        usedDlaa = true;
                        e.dlHaveHistory = false;
                    } else if (dlaaEvaluate(ctx, eye, e.dlColour, e.dlDepth, e.dlMv, e.dlOut,
                                            biasMask, w, h,
                                            oW, oH, jxNow, jyNow, resetHist, frameMs, &why)) {
                        usedDlaa = true;
                        e.dlHaveHistory = true;
                        if (!g_dlaaNoted || (oW != w && !g_dlssNoted)) {
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
                            // other while there is something to compare.
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
                                    const float seen =
                                        static_cast<float>(w) / static_cast<float>(oW);
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
                        e.dlHaveHistory = false;
                        if (!g_dlaaFailNoted) {
                            g_dlaaFailNoted = true;
                            Log::get().note(
                                "temporal aa: dlaa was asked for, but %s. The pass's own "
                                "history runs instead.",
                                why);
                        }
                    }
                    // The frame that goes out, in the game's own format (dlSubmit
                    // says why). Inside the timed region, so the price is honest.
                    if (usedDlaa && uiResolve) {
                        ctx->CSSetShaderResources(0,15,nullSrvM);ctx->CSSetUnorderedAccessViews(0,7,nullUavM,nullptr);
                        auto* resolveScreen=screenSrv;
                        if(screenSrv && (p.region[0]!=int32_t(region[0]) || p.region[1]!=int32_t(region[1]))) {
                            // Raw colour is cropped, but the screen map is in
                            // the original eye texture even on the copy path.
                            PassParams resolveParams=p;
                            for(int i=0;i<4;++i)resolveParams.region[i]=int32_t(region[i]);
                            if(!setParams(ctx,resolveParams))resolveScreen=nullptr;
                        }
                        ID3D11ShaderResourceView* srvs[7]={e.dlColourSrv,e.dlOutSrv,uiBound?e.uiMaskSrv:nullptr,e.uiHistoryValid?e.uiHistorySrv[e.uiHistoryRead]:nullptr,e.dlMvSrv,uiDepthContentChanges(w,h,eye),resolveScreen};
                        ID3D11UnorderedAccessView* uavs[2]={e.dlSubmitUav,e.uiHistoryUav[1-e.uiHistoryRead]};
                        ctx->CSSetShader(g_csUiResolve,nullptr,0);ctx->CSSetShaderResources(0,7,srvs);ctx->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
                        // NGX may change compute bindings, including b0.
                        ctx->CSSetConstantBuffers(0,1,&g_cb);
                        ctx->Dispatch((w+7)/8,(h+7)/8,1);
                        ctx->CSSetShaderResources(0,15,nullSrvM);ctx->CSSetUnorderedAccessViews(0,7,nullUavM,nullptr);
                        uiEvidenceWritten=uiResolveWritten=true;
                        if(!g_uiResolveNoted){g_uiResolveNoted=true;Log::get().note("UI resolve: current-raster bounds applied after DLSS; UI influence follows submitted motion as well as its old screen position. Existing submit/UI-history textures reused; no adaptive colour work in the DLSS motion pass.");}
                    } else if (usedDlaa) ctx->CopyResource(e.dlSubmit, e.dlOut);
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
        if (!usedDlaa) {
            if(e.uiResolvedHistory){e.uiHistoryValid=false;e.uiResolvedHistory=false;}
            ID3D11Texture2D* rm = nullptr;
            const bool uiOwn = (p.tvSt[3] != 0.0f || p.ships[0] != 0.0f || uiTrack) && uiDepthCoverageMask(w, h, eye, &rm) && rm &&
                               ensureUiMaskSrv(dev, e, rm);
            p.probe[2] = uiOwn ? 1.0f : 0.0f;
            p.probe[3] = uiFlags();
        }
        bool ran = usedDlaa || (ensureNative(dev, e, viewFmt) && setParams(ctx, p));
        // A native fallback also writes counters, even when the requested
        // trained path was running without diagnostics.
        if (ran && !usedDlaa && !statsWritten) ctx->ClearUnorderedAccessViewUint(g_statsUav, zeros);
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
                ID3D11ComputeShader* mvCs = motionShader(ctx, true);
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
                        if (g_moversOn) ensureMoverPair(dev, e, w, h);
                    } else {
                        releaseDl(e);
                    }
                } else if (made && g_moversOn && !e.zPrev) {
                    ensureMoverPair(dev, e, w, h);
                }
                if (made && (!e.foveaOut || e.foveaW != foW || e.foveaH != foH)) {
                    if (e.foveaOutUav) { e.foveaOutUav->Release(); e.foveaOutUav = nullptr; }
                    if (e.foveaOut) { e.foveaOut->Release(); e.foveaOut = nullptr; }
                    made = makeTex(dev, foW, foH, sd.Format, viewFmt,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   &e.foveaOut, nullptr, &e.foveaOutUav);
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
                if (made && e.outSrv && e.dlOutSrv) {
                    // The colour, typed, whichever way the source came (the
                    // full path's copy logic).
                    D3D11_BOX box{};
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
                    // Motion vectors and the depth copy, full frame (NVIDIA
                    // reads the crop's sub-rectangle of them; the reduction
                    // reads them whole). The mover mask is computed here too
                    // (t3, u5) but only the own periphery pass applies it:
                    // the crop and periphery evaluations are not handed it.
                    ID3D11ShaderResourceView* nullSrvM[15] = {};
                    ID3D11UnorderedAccessView* nullUavM[7] = {};
                    ctx->CSSetShaderResources(0, 15, nullSrvM);
                    ctx->CSSetUnorderedAccessViews(0, 7, nullUavM, nullptr);
                    ctx->CSSetShader(mvCs, nullptr, 0);
                    ID3D11ShaderResourceView* srvsM[15] = {inSrv, e.histSrv[readIdx], depthSrv,
                                                          p.movers[0] != 0.0f ? e.zPrevSrv : nullptr,
                                                          p.probe[2] != 0.0f ? e.uiMaskSrv : nullptr,
                                                          p.tvSt[3] != 0.0f ? g_bodyGridSrv : nullptr,
                                                          smokeSrv, uiDepthSrv,
                                                          uiTrack && e.uiHistoryValid ? e.uiHistorySrv[e.uiHistoryRead] : nullptr,
                                                          terrainSrvs[0], terrainSrvs[1], terrainSrvs[2], holoSrvs[0], holoSrvs[1], screenSrv};
                    // u2 (the stats buffer) is left UNBOUND here: the own pass
                    // writes its stats when it runs, and the mv entry writes
                    // the same slots (15-17), so binding it would double them
                    // (the review of 2026-09-05, F5). Atomics on a null UAV
                    // are dropped.
                    ID3D11UnorderedAccessView* uavsM[7] = {nullptr, nullptr, nullptr,
                                                           e.dlMvUav, e.dlDepthUav, e.dlMaskUav,
                                                           uiTrack ? e.uiHistoryUav[1-e.uiHistoryRead] : nullptr};
                    ID3D11Buffer* cbM = g_cb;
                    ID3D11SamplerState* smpM = g_samp;
                    ctx->CSSetShaderResources(0, 15, srvsM);
                    ctx->CSSetUnorderedAccessViews(0, 7, uavsM, nullptr);
                    ctx->CSSetConstantBuffers(0, 1, &cbM);
                    ctx->CSSetSamplers(0, 1, &smpM);
                    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                    ctx->CSSetShaderResources(0, 15, nullSrvM);
                    ctx->CSSetUnorderedAccessViews(0, 7, nullUavM, nullptr);
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
                            } else {
                                inputsOk = false;
                            }
                        }
                        if (inputsOk) {
                            // The jitter in the periphery's own pixels: the
                            // render's offset scaled by the reduction.
                            const float sx = static_cast<float>(rw) / static_cast<float>(w);
                            const float sy = static_cast<float>(rh) / static_cast<float>(h);
                            const bool resetP = (flags & 1u) != 0 || !e.prHaveHistory;
                            periphOk = dlaaEvaluatePeriphery(
                                ctx, eye, reduce ? e.prColour : e.dlColour,
                                reduce ? e.prDepth : e.dlDepth, reduce ? e.prMv : e.dlMv, e.prOut,
                                rw, rh, jxNow * sx, jyNow * sy, resetP, frameMs, &whyF);
                            if (!periphOk) standDown(whyF);
                        }
                    }
                    // The fovea crop -- unless the steady periphery it is
                    // composited over just failed (the latch has it).
                    if (!steady || periphOk) {
                        const bool resetHist = (flags & 1u) != 0 || !e.foveaHaveHistory;
                        if (dlssEvaluateFovea(ctx, eye, e.dlColour, e.dlDepth, e.dlMv, e.dlOut, w, h,
                                              foW, foH, fcx, fcy, fcw, fch, focx, focy, focw, foch,
                                              jxNow, jyNow, resetHist, frameMs, &whyF)) {
                            foveaEvalOk = true;   // e.foveaHaveHistory is set from this at frame end
                        } else {
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
                ID3D11ShaderResourceView* nullSrv[15] = {};
                ID3D11UnorderedAccessView* nullUav[7] = {};
                ctx->CSSetShaderResources(0, 15, nullSrv);
                ctx->CSSetUnorderedAccessViews(0, 7, nullUav, nullptr);
                ctx->CSSetShader(g_cs, nullptr, 0);
                ID3D11ShaderResourceView* srvs[15] = {inSrv, e.histSrv[readIdx], depthSrv,
                                                     (carry && p.movers[0] != 0.0f) ? e.zPrevSrv : nullptr,
                                                     p.probe[2] != 0.0f ? e.uiMaskSrv : nullptr,
                                                     p.tvSt[3] != 0.0f ? g_bodyGridSrv : nullptr,
                                                     smokeSrv, uiDepthSrv,
                                                     uiTrack && e.uiHistoryValid ? e.uiHistorySrv[e.uiHistoryRead] : nullptr,
                                                          terrainSrvs[0], terrainSrvs[1], terrainSrvs[2], holoSrvs[0], holoSrvs[1], screenSrv};
                ID3D11UnorderedAccessView* uavs[7] = {e.outUav, e.histUav[writeIdx],
                                                      g_statsUav, nullptr,
                                                      carry ? e.dlDepthUav : nullptr, nullptr,
                                                      uiTrack ? e.uiHistoryUav[1-e.uiHistoryRead] : nullptr};
                ID3D11Buffer* cb = g_cb;
                ID3D11SamplerState* smp = g_samp;
                ctx->CSSetShaderResources(0, 15, srvs);
                ctx->CSSetUnorderedAccessViews(0, 7, uavs, nullptr);
                ctx->CSSetConstantBuffers(0, 1, &cb);
                ctx->CSSetSamplers(0, 1, &smp);
                ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                ctx->CSSetShaderResources(0, 15, nullSrv);
                ctx->CSSetUnorderedAccessViews(0, 7, nullUav, nullptr);
                if (carry) zcWritten = true;
                if (uiTrack) uiEvidenceWritten = true;
                ownRan = true;
            }
            // THE COMPOSITE: NVIDIA's crop over whichever periphery there is.
            if (foveaMode && foveaEvalOk && compositeReady && (periphOk || ownRan)) {
                ID3D11ShaderResourceView* csrv[2] = {periphOk ? e.prOutSrv : e.outSrv, e.dlOutSrv};
                // u1 is the own history being written this frame, for the
                // hand-off (the shader writes it only when mode.y says so).
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
                foveaComposited = true;
                ++g_foveaTreats;
                if (!g_foveaNoted) {
                    g_foveaNoted = true;
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
                }
            }
        }
        if (qs >= 0) {
            if (g_slots[qs].timing && !g_slots[qs].timer.end(ctx)) {
                g_slots[qs].timer.reset(ctx); g_slots[qs].timing=false;
            }
            if (statsWritten || (ran && !usedDlaa)) ctx->CopyResource(g_slots[qs].staging, g_stats);
            g_slots[qs].inUse = true;
            g_slots[qs].timeDone = !g_slots[qs].timing;
            g_slots[qs].statsDone = !(statsWritten || (ran && !usedDlaa));
            g_slots[qs].pixels = static_cast<uint64_t>(w) * h;
            g_slots[qs].hadHistory = useHistory;
            g_slots[qs].headDeg = headDeg;
            for (int k = 0; k < 4; ++k) {
                g_slots[qs].candPixels[k] =
                    (useHistory && candValid[k]) ? static_cast<uint64_t>(w) * h : 0;
            }
        }

        ID3D11ShaderResourceView* nullSrv2[15] = {};
        ID3D11UnorderedAccessView* nullUav2[7] = {};
        ctx->CSSetShaderResources(0, 15, nullSrv2);
        ctx->CSSetUnorderedAccessViews(0, 7, nullUav2, nullptr);
        if (depthSrv) {
            ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtv, savedDsv);
            for (auto* v : savedRtv) if (v) v->Release();
            if (savedDsv) savedDsv->Release();
        }
        ctx->CSSetShader(savedCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 15, savedSrv);
        ctx->CSSetUnorderedAccessViews(0, 7, savedUav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &savedCb);
        ctx->CSSetSamplers(0, 1, &savedSamp);
        if (savedCs) savedCs->Release();
        for (auto* v : savedSrv) if (v) v->Release();
        for (auto* v : savedUav) if (v) v->Release();
        if (savedCb) savedCb->Release();
        if (savedSamp) savedSamp->Release();

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
            // The fovea composite, when it ran, is what goes out: the own
            // history is still the periphery it was blended over, so its
            // ping-pong above stands. NVIDIA's crop history lives in the fovea
            // feature (e.foveaHaveHistory), kept apart from both.
            result = foveaComposited ? e.foveaOut : e.outTex;
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
    return result;
}

}  // namespace

void temporalPassConfigure(Config& cfg) {
    const std::string mode = cfg.getString("fix.temporal_aa", "off");
    g_wanted = temporalModeEnabled(mode);
    celestialMotionConfigure(g_wanted);
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
    const std::string dbg = cfg.getString("advanced.temporal_aa_debug", "off");
    g_debugMode = _stricmp(dbg.c_str(), "motion") == 0 ? 1 : _stricmp(dbg.c_str(), "error") == 0 ? 2
                : _stricmp(dbg.c_str(), "depth") == 0 ? 3 : _stricmp(dbg.c_str(), "movers") == 0 ? 4
                : _stricmp(dbg.c_str(), "objects") == 0 ? 5 : 0;
    g_objectsOn = g_wanted;
    float reach = cfg.getFloat("advanced.temporal_aa_objects_reach", 1500.0f);
    if (!std::isfinite(reach)) reach = 1500.0f;
    if (reach < 1.0f) reach = 1.0f;
    if (reach > 2000.0f) reach = 2000.0f;
    g_objectsReach = reach;
    objectMotionSetReach(reach);
    float shipsM = cfg.getFloat("advanced.temporal_aa_objects_ships_metres", 1000.0f);
    if (!std::isfinite(shipsM) || shipsM < 0.0f) shipsM = 0.0f;
    if (shipsM > 5000.0f) shipsM = 5000.0f;
    g_shipsRangeM = shipsM;
    objectShipsSetRange(shipsM);
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
    // DLSS where you look (docs/performance.md feature 6). The crop's width
    // in degrees of visual angle; 0 is the whole frame, today's behaviour.
    // Bounded below by a size worth cropping (a crop wider than the frame is
    // the whole frame) and above at a full hemisphere. Live: the branch
    // reads g_foveaDeg each frame, so it can be tuned from inside a headset.
    float fov = cfg.getFloat("advanced.temporal_aa_fovea", 0.0f);
    if (!std::isfinite(fov) || fov < 0.0f) fov = 0.0f;
    if (fov > 0.0f && fov < 10.0f) fov = 10.0f;   // below this the fovea is not worth the seam
    if (fov > 120.0f) fov = 120.0f;
    g_foveaDeg = fov;
    float edge = cfg.getFloat("advanced.temporal_aa_fovea_edge", 6.0f);
    // Floored at 1 deg when the fovea is on: DLSS treats the crop's edge as the
    // image edge (clamped taps, no history beyond it), so a hard seam lets its
    // border artefacts in at full weight (the review of 2026-09-05, F11).
    if (!std::isfinite(edge) || edge < 1.0f) edge = 1.0f;
    if (edge > 30.0f) edge = 30.0f;
    g_foveaEdgeDeg = edge;
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
    unsigned preset = 11, presetFov = 11;
    if (_stricmp(model.c_str(), "quality") == 0 || _stricmp(model.c_str(), "k") == 0) { preset = 11; presetFov = 11; }
    else if (_stricmp(model.c_str(), "steady") == 0) { preset = 11; presetFov = 12; }
    else if (_stricmp(model.c_str(), "auto") == 0 || _stricmp(model.c_str(), "default") == 0) { preset = 0; presetFov = 0; }
    else if (_stricmp(model.c_str(), "responsive") == 0 || _stricmp(model.c_str(), "j") == 0) { preset = 10; presetFov = 10; }
    else if (_stricmp(model.c_str(), "l") == 0) { preset = 12; presetFov = 12; }
    else if (_stricmp(model.c_str(), "m") == 0) { preset = 13; presetFov = 13; }
    // The letters stop here, and deliberately. NVSDK_NGX_DLSS_Hint_Render_
    // Preset (nvsdk_ngx_defs.h, DLSS SDK 310.4) has no A, B, C or D at all
    // -- they were removed, with the header saying to use J or K instead --
    // and it marks G, H, I, N and O as reverting to default behaviour if
    // asked for. E and F are deprecated; this UI exposes J, K, L and M.
    // A tidier-looking letter range would offer four presets that no
    // longer exist.
    else if (!model.empty()) {
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
    dlaaSetPreset(preset, presetFov);
    // A config reload re-arms the fovea after a failure stood it down (F3):
    // the user may have changed the width, or the transient may be gone.
    g_foveaFailed = false;
}

void temporalPassTick(ID3D11DeviceContext* ctx) {
    if (!ctx || (!g_wanted && !g_eyeRunReady)) return;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    const bool accepted = acceptPassDevice(dev);
    if (dev) dev->Release();
    if (!accepted) return;
    if (g_eyeRunReady && ctx) {
        writeEyeRun(ctx, g_eyeRunWidth, g_eyeRunHeight);
        g_eyeRunReady = false;
    }
    if (!g_wanted || !ctx) return;
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
}

void temporalPassNoteSceneWrite(const void* res, const void* data, uint32_t bytes) {
    // The true view matrix lives at float offset 932 of the big scene
    // block (measured by the sun-glare fix's two-shot dump: three 3x4
    // rows, rotation plus translation). Every write of every block of
    // that size goes into the ring, stamped with the frame and the order;
    // chooseCameraRows picks the frame's at its first treat. Kept only
    // when the rows are a rotation.
    if (!g_wanted || !data || bytes < 944 * 4) return;
    const float* f = static_cast<const float*>(data) + 932;
    if (!temporalRowsAreRotation(f)) return;
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
    const float* pz = static_cast<const float*>(data) + 792;
    w.proj[0] = pz[2];
    w.proj[1] = pz[3];
    w.frame = g_rowsFrame;
    w.seq = g_rowsSeq;
    w.valid = true;
    ++g_rowsSeq;
}

void temporalPassNoteFirstEyeDraw(ID3D11DeviceContext* ctx) {
    if (!g_wanted || g_curLatched) return;
    g_curLatched = true;
    g_boundSeen = false;
    g_boundBuf = nullptr;
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
    for (int i = 0; i < 8; ++i) {
        if (vs[i]) vs[i]->Release();
        if (ps[i]) ps[i]->Release();
    }
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
    if (!g_wanted) return;
    ++g_rowsFrame;
    g_rowsWritesSum += g_rowsWrites;
    ++g_rowsFramesSum;
    g_rowsWrites = 0;
    g_chosenThisFrame = false;
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
    const double px = g_pixelsSeen ? static_cast<double>(g_pixelsSeen) : 1.0;
    if (rejectPct) *rejectPct = 100.0 * static_cast<double>(g_rejected) / px;
    if (clipPct) *clipPct = 100.0 * static_cast<double>(g_clipped) / px;
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
    if (g_camDropRot || g_camDropMove) {
        regAppend(buf, n, used,
                  "; the camera's delta was dropped on %u eye-frames as another camera's (over 3 "
                  "deg from the head's) and its translation on %u as a jump (over 50 m); a "
                  "jump was carried on %u (zero by construction)",
                  g_camDropRot, g_camDropMove, g_camCarriedJump);
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
    // Tier 2: how many pixels took the body's path, and the body it was --
    // a station should read as a few percent of the frame far off and
    // most of it in the slot, with its turn steady from interval to
    // interval; zero with a body in hand means the margin never let it in.
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
    if (g_objectsOn && g_intervalPix) {
        if (g_bodyLastValid) {
            regAppend(buf, n, used,
                      "; the body's path took %.2f%% of pixels (the body: %u records, %.0f%% of the "
                      "pool's movers, fit to %.3f m, %.4f deg and %.3f m over its pair's %.1f ms, a box "
                      "%.0f x %.0f x %.0f m, %u frames old)",
                      100.0 * static_cast<double>(g_bodyPix) / static_cast<double>(g_intervalPix),
                      g_bodyLast.records, 100.0 * static_cast<double>(g_bodyLast.share),
                      static_cast<double>(g_bodyLast.rms),
                      static_cast<double>(temporalRotationAngleDeg(g_bodyLast.R)),
                      sqrt(static_cast<double>(g_bodyLast.t[0]) * g_bodyLast.t[0] +
                           static_cast<double>(g_bodyLast.t[1]) * g_bodyLast.t[1] +
                           static_cast<double>(g_bodyLast.t[2]) * g_bodyLast.t[2]),
                      static_cast<double>(g_bodyLast.dtMs),
                      static_cast<double>(g_bodyLast.bmax[0] - g_bodyLast.bmin[0]),
                      static_cast<double>(g_bodyLast.bmax[1] - g_bodyLast.bmin[1]),
                      static_cast<double>(g_bodyLast.bmax[2] - g_bodyLast.bmin[2]),
                      g_bodyLast.age);
            if (g_bodyLast.body2) {
                regAppend(buf, n, used,
                          "; the second body's path took %.2f%% of pixels (%u records turning %.4f deg over "
                          "the pair, on %llu frames)",
                          100.0 * static_cast<double>(g_body2Pix) / static_cast<double>(g_intervalPix),
                          g_bodyLast.records2, static_cast<double>(temporalRotationAngleDeg(g_bodyLast.R2)),
                          static_cast<unsigned long long>(g_body2Frames));
            }
            if (g_steppedPix || g_steppedFrames || g_steppedCellPix) {
                regAppend(buf, n, used,
                          "; the stepped parts (updated by the game at a lower rate; object_probe.h) took %.2f%% of "
                          "pixels on their own multiples of the turn, cells stamped on %llu frames; %.3f%% of pixels "
                          "sat in a stamped cell and the path was refused on %.0f%% of those",
                          100.0 * static_cast<double>(g_steppedPix) / static_cast<double>(g_intervalPix),
                          static_cast<unsigned long long>(g_steppedFrames),
                          100.0 * static_cast<double>(g_steppedCellPix) / static_cast<double>(g_intervalPix),
                          g_steppedCellPix ? 100.0 * static_cast<double>(g_steppedOffPix) /
                                                 static_cast<double>(g_steppedCellPix)
                                           : 0.0);
            }
        } else {
            regAppend(buf, n, used, "; the body's path is on but no body is in hand (no pool, or no pair yet)");
        }
        if (g_bodyFrameHolds || g_bodyRowsHolds || g_bodyShiftUsed) {
            regAppend(buf, n, used,
                      "; the body stood down %u frames with its pair in another frame, composed with the "
                      "carried camera delta on %u (another camera's rows, or a jump's), and was carried "
                      "over %u frames by the origin's move since its pair",
                      g_bodyFrameHolds, g_bodyRowsHolds, g_bodyShiftUsed);
        }
        // The moving ships (2026-09-09): their share and the nearest one.
        if (g_shipsRangeM > 0.0f) {
            if (g_shipsLastN) {
                regAppend(buf, n, used,
                          "; the moving ships' path took %.2f%% of pixels (%u ship%s in hand within %.0f m, the "
                          "nearest %u parts at %.0f m fit to %.3f m, %u frames old)",
                          100.0 * static_cast<double>(g_shipPix) / static_cast<double>(g_intervalPix),
                          g_shipsLastN, g_shipsLastN == 1 ? "" : "s", static_cast<double>(g_shipsRangeM),
                          g_shipsLast.records, static_cast<double>(g_shipsLast.distM),
                          static_cast<double>(g_shipsLast.rms), g_shipsLastAge);
            } else if (g_shipPix) {
                regAppend(buf, n, used,
                          "; the moving ships' path took %.2f%% of pixels this interval (none in hand now)",
                          100.0 * static_cast<double>(g_shipPix) / static_cast<double>(g_intervalPix));
            }
            // The claim by reason (Stats 40-45): what the footprints held.
            const uint64_t foot = g_shipPix + g_shipFoot + g_shipFootNoDepth;
            if (foot) {
                const double f = static_cast<double>(foot);
                regAppend(buf, n, used,
                          "; in the ships' footprints %.1fk pixels: %.0f%% claimed, %.0f%% without depth, "
                          "%.0f%% outside the box at their depth (%+.1f m along the ray from the box's "
                          "centre on average), %.0f%% behind the tail, %.0f%% beyond the parts' reach",
                          f / 1000.0, 100.0 * static_cast<double>(g_shipPix) / f,
                          100.0 * static_cast<double>(g_shipFootNoDepth) / f,
                          100.0 * static_cast<double>(g_shipOutBox) / f,
                          g_shipOutBox ? 0.1 * static_cast<double>(g_shipOutBoxDm) / static_cast<double>(g_shipOutBox)
                                       : 0.0,
                          100.0 * static_cast<double>(g_shipBehind) / f,
                          100.0 * static_cast<double>(g_shipFar) / f);
            }
        }
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
    g_camDropMove = 0;
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
    g_bodyPix = 0;
    g_body2Pix = 0;
    g_body2Frames = 0;
    g_steppedPix = 0;
    g_steppedCellPix = 0;
    g_steppedOffPix = 0;
    g_steppedFrames = 0;
    g_dlResets = 0;
    g_dlResetsAsked = 0;
    g_dlResetsHeld = g_dlResetsReturned = g_dlResetsUnjudged = g_dlResetsNoDelta = 0;
    g_shipPix = 0;
    g_shipFoot = g_shipOutBox = g_shipBehind = g_shipFar = g_shipFootNoDepth = 0;
    g_shipOutBoxDm = 0;
    g_bodyFrameHolds = 0;
    g_bodyRowsHolds = 0;
    g_bodyShiftUsed = 0;
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

void temporalPassShutdown() {
    dlaaShutdown();
    if (g_csMv) { g_csMv->Release(); g_csMv = nullptr; }
    if (g_csMvFast) { g_csMvFast->Release(); g_csMvFast = nullptr; }
    if (g_passDevice) { g_passDevice->Release(); g_passDevice = nullptr; }
    if (g_csFovea) { g_csFovea->Release(); g_csFovea = nullptr; }
    if (g_csUiResolve) { g_csUiResolve->Release(); g_csUiResolve=nullptr; }
    g_csUiResolveTried=g_uiResolveNoted=false;
    if (g_foveaCb) { g_foveaCb->Release(); g_foveaCb = nullptr; }
    if (g_csDown) { g_csDown->Release(); g_csDown = nullptr; }
    if (g_downCb) { g_downCb->Release(); g_downCb = nullptr; }
    if (g_treats > 0) {
        Log::get().note("temporal aa: %u eye-submits treated this session.",
                        g_treats);
    }
    for (EyeState& e : g_eye) releaseEye(e);
    for (Slot& q : g_slots) releaseSlot(q);
    if (g_bodyGridSrv) { g_bodyGridSrv->Release(); g_bodyGridSrv = nullptr; }
    for(auto& overview:g_eyeRunStaging)if(overview){overview->Release();overview=nullptr;}
    for (int k = 0; k < kEyeRun; ++k) {
        if (g_eyeRawStaging[k]) { g_eyeRawStaging[k]->Release(); g_eyeRawStaging[k] = nullptr; }
        if (g_eyeTreatedStaging[k]) { g_eyeTreatedStaging[k]->Release(); g_eyeTreatedStaging[k] = nullptr; }
    }
    g_eyeRunLeft = 0;
    g_eyeRunTaken = 0;
    g_eyeRunReady = false;
    g_eyeMotionTraceCount = 0;
    g_eyeRunUntreated=false;
    memset(g_eyeOverviewTaken,0,sizeof(g_eyeOverviewTaken));
    for(auto& texture:g_eyeInputs)if(texture){texture->Release();texture=nullptr;}
    g_frameBodyValid = false;
    g_frameBodyFrame = ~0u;
    if (g_bodyGrid) { g_bodyGrid->Release(); g_bodyGrid = nullptr; }
    if (g_statsUav) { g_statsUav->Release(); g_statsUav = nullptr; }
    if (g_stats) { g_stats->Release(); g_stats = nullptr; }
    if (g_samp) { g_samp->Release(); g_samp = nullptr; }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_cs) { g_cs->Release(); g_cs = nullptr; }
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
    // The key takes a RUN of the left eye (kEyeRun says why): sixteen raw
    // crops and the first treated frame; a run under way is left to finish.
    if (g_eyeRunLeft > 0 || g_eyeRunReady) return;
    SYSTEMTIME stm{};
    GetLocalTime(&stm);
    _snwprintf_s(g_eyeRunStamp, 16, _TRUNCATE, L"%02u%02u%02u", static_cast<unsigned>(stm.wHour),
                 static_cast<unsigned>(stm.wMinute), static_cast<unsigned>(stm.wSecond));
    g_eyeRunTaken = 0;
    g_eyeRunLeft = kEyeRun;
    g_eyeMotionTraceCount = 0;
    g_eyeRunUntreated=false;
    memset(g_eyeOverviewTaken,0,sizeof(g_eyeOverviewTaken));
    for(auto& texture:g_eyeInputs)if(texture){texture->Release();texture=nullptr;}
    memset(g_eyeRawTaken, 0, sizeof(g_eyeRawTaken));
    memset(g_eyeRawInputW, 0, sizeof(g_eyeRawInputW));
    memset(g_eyeRawInputH, 0, sizeof(g_eyeRawInputH));
    memset(g_eyeRunFrames, 0, sizeof(g_eyeRunFrames));
    // ...and the object probe's ledger of the same frames (object_probe.h),
    // when the probe is on; the crops' names and its share the stamp.
    objectProbeArmLedger(g_eyeRunStamp);
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
    edvr::guardedBudget(edvr::g_budget, [&] {
        out = edvr::temporalInner(srcTex, eye, bounds, tanNow, tanPrev, jxNow,
                                  jyNow, deltaHead, headTrans, headTransSwapped,
                                  nearZ, farZ, headDeg, motion, blend,
                                  clampSigma, outW, outH, flags);
    });
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
