#include "temporal_shader_bytecode.h"
#include "../../d3d11/sharpen_pass.h"
#include "../../common/native_sharpen.h"
#include "../../d3d11/graphics_runtime.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>

#include <d3d11.h>

#include "../../common/config.h"
#include "../../common/frame_flag.h"   // glitchConsumerPresent: is a compositor hook alive
#include "../../common/guard.h"
#include "../../common/log.h"
#include "../../common/runtime_profile.h"   // runtimeFlatProfile: what a "pass" is counted in
#include "../../common/supersample_math.h"   // supersampleRegionFromBounds: the eye's pixels
#include "../../common/temporal_mode.h"      // temporalModeEnabled: why the flat pass may never run
#include "../../common/timing.h"
#include "../../d3d11/shader_swap.h"
#include "../../d3d11/gpu_timing.h"
#include "../../d3d11/gpu_census.h"   // issue #38: the per-feature GPU cost census

// AMD's own FSR, CPU side: FsrRcasCon packs the strength into the constants
// the shader reads. intro_upscale.cpp's arrangement, warnings and all --
// the vendored files stay byte-identical to upstream, so the C4505s their
// unused helpers raise at /W4 are silenced here, not in their text.
#define A_CPU 1
#pragma warning(push)
#pragma warning(disable : 4505)
#include "../../d3d11/fsr/ffx_a.h"
#include "../../d3d11/fsr/ffx_fsr1.h"
#pragma warning(pop)

// The same two files as GPU text, generated at build time.
// Fixed shader bytecode is generated during the build. //

namespace edvr {
namespace {

// RCAS, wrapped in the callbacks AMD's header asks the calling shader to
// provide -- intro_upscale.cpp's kRcasMain with one difference. The load
// clamps INTO the eye's region: RCAS reads the pixel and its four
// neighbours, and at the region's edge a neighbour is the other eye of a
// double-wide texture, or nothing at all (D3D answers zero for a load off
// the texture, which would ring the frame in a dark line). Edge
// replication is the resolve's rule for the same border, for the same
// reason. Everything the sharpening itself does is theirs. Not desk-
// compiled by tools/compile_variants.py (it needs the chunks joined
// first); the smoke harness compiles and runs it on a real device.






// The cbuffer above, laid out to match: 48 bytes, three 16-byte rows.
struct PassParams {
    uint32_t con[4];
    int32_t  region[4];
    int32_t  outSize[2];
    int32_t  pad0[2];
};
static_assert(sizeof(PassParams) == 48, "the cbuffer is three 16-byte rows");

// AMD's unit is STOPS of sharpness reduction, 0 the sharpest and 2 the
// mildest. The setting is 0 (off) to 1 (sharpest), the way a player reads
// a slider, so 1 is 0 stops and the ramp down is linear in stops.
float stopsOf(float strength) {
    if (!std::isfinite(strength)) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;
    if (strength < 0.0f) strength = 0.0f;
    return 2.0f * (1.0f - strength);
}

// What one pass is counted in. VR sharpens each eye it submits; the flat
// profile sharpens one frame a call (flat_sharpen.h), and its log says frames,
// because "per eye" there would read as a stereo pair that does not exist.
bool flatPass() { return runtimeFlatProfile(); }
const char* perUnit() { return flatPass() ? "frame" : "eye"; }
const char* unitsSharpened() { return flatPass() ? "frames" : "eye-submits"; }

// The format allowlist, shared with the temporal pass (temporal_pass.cpp)
// -- typeless and UNORM families read and written through the family's
// plain typed view, the source's own format kept on the output, sRGB-typed
// sources refused rather than guessed at. RCAS runs on the stored values,
// perceptual for gamma content, which is the space AMD wrote it for.
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

// Per-eye owned resources: the resolve's cache discipline, release-before-
// recreate on any size or format change, keyed on what WE need -- except
// the one view over the source, keyed on exactly that resource. Holding
// the view holds the texture, so its address cannot be recycled under the
// cache, and it is released the moment a different texture arrives for
// this eye: the source here is usually the resolve's or the temporal
// pass's own output, which those passes recreate on a size change.
struct EyeState {
    void*                      srcRes = nullptr;
    ID3D11ShaderResourceView*  srcSrv = nullptr;

    // The copy-through pair, for a source that refuses a shader view.
    ID3D11Texture2D*           copyTex = nullptr;
    ID3D11ShaderResourceView*  copySrv = nullptr;
    uint32_t                   copyW = 0, copyH = 0;
    DXGI_FORMAT                copyFmt = DXGI_FORMAT_UNKNOWN;

    // The result: region-sized, the source's own format.
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
void releaseOut(EyeState& e) {
    if (e.outUav) { e.outUav->Release(); e.outUav = nullptr; }
    if (e.outTex) { e.outTex->Release(); e.outTex = nullptr; }
    e.outW = e.outH = 0;
    e.outFmt = DXGI_FORMAT_UNKNOWN;
}
void releaseEye(EyeState& e) {
    releaseSrc(e);
    releaseCopy(e);
    releaseOut(e);
}

// The GPU-price ring, the resolve's: never awaited, a slot not ready is
// tried next call, and a call that finds every slot in flight runs untimed.
struct QuerySlot {
    GpuTimer     timer;
    bool         inUse = false;
};
constexpr int kQueryRing = 8;
QuerySlot g_qring[kQueryRing];

void releaseQuerySlot(QuerySlot& q) {
    q.timer.reset();
    q.inUse = false;
}

uint32_t g_timeCount = 0;
double   g_timeSum = 0.0;
double   g_timeMax = 0.0;
bool     g_timeLogged = false;
uint32_t g_lastW = 0, g_lastH = 0;
float    g_lastStrength = 0.0f;

void maybeLogTiming() {
    if (g_timeLogged || g_timeCount < 120) return;
    g_timeLogged = true;
    Log::get().note(
        "render sharpening: measured %.2f ms per %s on average (max %.2f) "
        "at %ux%u -- one dispatch of AMD's RCAS at strength %.2f (%.2f "
        "stops; render_sharpness moves it, live).",
        g_timeSum / static_cast<double>(g_timeCount), perUnit(), g_timeMax,
        g_lastW, g_lastH, static_cast<double>(g_lastStrength),
        static_cast<double>(stopsOf(g_lastStrength)));
}

void pollTimingRing(ID3D11DeviceContext* ctx) {
    if (!gpuTimingOwns(ctx)) return;
    for (QuerySlot& q : g_qring) {
        if (!q.inUse) continue;
        double ms = 0.0;
        const GpuTimerPoll result = q.timer.poll(ctx, ms);
        if (result == GpuTimerPoll::Pending) continue;
        q.inUse = false;
        if (result != GpuTimerPoll::Ready) continue;
        ++g_timeCount;
        g_timeSum += ms;
        if (ms > g_timeMax) g_timeMax = ms;
    }
    maybeLogTiming();
}

int acquireQuerySlot(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!dev || !ctx) return -1;
    if (!gpuTimingAccepts(ctx) && !gpuTimingBind(dev, ctx)) return -1;
    for (int i = 0; i < kQueryRing; ++i) {
        QuerySlot& q = g_qring[i];
        if (q.inUse) continue;
        if (q.timer.begin(dev, ctx)) { q.inUse = true; return i; }
        return -1; // Shared clock pressure cannot be fixed by trying another free slot.
    }
    return -1;
}

// Same budget and same reasoning as the resolve: every call dereferences a
// handle the game (or a pass before this one) owns, and a fault that
// repeats must retire the pass rather than bleed the log or the frame.
FaultBudget g_budget("sharpenPass", 8);

ID3D11ComputeShader* g_cs = nullptr;
bool                 g_csTried = false;
ID3D11Buffer*        g_cb = nullptr;

bool     g_failNoted = false;
bool     g_kindNoted = false;
bool     g_regionNoted = false;
bool     g_zeroNoted = false;
bool     g_fmtUnknownNoted = false;
bool     g_fmtChecked[kFormatCount] = {};
bool     g_fmtSupported[kFormatCount] = {};
bool     g_fmtUnsupportedNoted[kFormatCount] = {};
uint32_t g_fmtAsks = 0;   // CheckFormatSupport calls: a new device is asked again (sharpenPassHeldForTest)
bool     g_firstNoted = false;
uint32_t g_treats = 0;

// The warm and the missing-hook note (sharpenPassConfigure/Tick).
bool     g_wanted = false;
float    g_strength = 0.0f;
bool     g_warmNoted = false;
bool     g_noHookNoted = false;
uint64_t g_firstTickMs = 0;
constexpr uint64_t kNoHookNoteMs = 30000;

void failOnce(const char* what) {
    if (g_failNoted) return;
    g_failNoted = true;
    Log::get().note("render sharpening: %s; the pass stands down.", what);
}

// THE DEVICE. Everything the pass makes -- the shader, the parameter buffer, both
// eyes' views and textures, the price ring -- belongs to one D3D11 device, and D3D11
// refuses to mix devices: a view over a texture another device made fails (the flat
// wrapper's failed with 0x887A0005 and stood the flat sharpening down for the
// session), and a shader, buffer or UAV from one device bound on another's context is
// no better. The game can recreate its device in a running process, and the state
// above used to be kept until sharpenPassShutdown() whatever device the next frame
// came from (review RC4, F5; a two-device WARP probe reproduced it).
//
// g_owner is the device the state was made on, held with a reference so its address
// cannot be recycled under the comparison (everything above holds one implicitly,
// through its own references, so this pins nothing that was not already pinned). A
// frame from another device, in sharpenInner, releases all of it and starts over.
// Owner-thread state, like the rest of this file.
ID3D11Device* g_owner = nullptr;
uint32_t      g_deviceChanges = 0;
constexpr uint32_t kMaxDeviceChangeNotes = 4;   // two devices trading frames must not fill a log
bool          g_deviceResetOff = false;         // rigs only: sharpenPassDeviceResetOffForTest

// Everything above that belongs to a device: the shader and the latch that says it was
// tried (a compile that failed on the old device says nothing about the new one), the
// parameter buffer, both eyes' views and textures, the price ring, what was learned about
// the device's formats, and the reason for a stand-down (the old device's). Not the counters,
// the timing sums or the once-per-session notes: those are the session's.
void releaseDeviceState() {
    for (EyeState& e : g_eye) releaseEye(e);
    for (QuerySlot& q : g_qring) releaseQuerySlot(q);
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_cs) { g_cs->Release(); g_cs = nullptr; }
    g_csTried = false;
    for (int i = 0; i < kFormatCount; ++i) {
        g_fmtChecked[i] = false;
        g_fmtSupported[i] = false;
        g_fmtUnsupportedNoted[i] = false;
    }
    g_failNoted = false;
}

// The device a frame's source lives on. The first call adopts it; a different one is a
// device change: release the old device's state, say so, adopt the new. Off in a rig that
// asks for the pass as it was before this existed (sharpenPassDeviceResetOffForTest).
void adoptDevice(ID3D11Device* dev) {
    if (g_deviceResetOff || !dev || g_owner == dev) return;
    if (g_owner) {
        ++g_deviceChanges;
        if (g_deviceChanges <= kMaxDeviceChangeNotes) {
            Log::get().note(
                "render sharpening: the D3D device changed (change %u); the shader, "
                "the parameter buffer, both eyes' textures and views and the price "
                "ring made on the old one are released and made again on the new "
                "one.%s",
                g_deviceChanges,
                g_deviceChanges == kMaxDeviceChangeNotes ? " Further changes are not said." : "");
        }
        releaseDeviceState();
        g_owner->Release();
    }
    g_owner = dev;
    dev->AddRef();
}

// Whether the tick may warm the shader on this context's device: the device the pass works
// on, or any device while the pass has none yet (which it then adopts, so the warm compile is
// the first frame's compile and not a wasted one). Never the reverse: a tick on another device
// does not move the pass -- only a frame does -- or two devices alternating would release and
// remake the eyes' textures every frame; that tick simply leaves the compile to the first frame.
bool tickMayWarm(ID3D11DeviceContext* ctx) {
    if (g_deviceResetOff) return true;
    ID3D11Device* dev = nullptr;
    if (!guarded("sharpenPass.tick.device", [&] { ctx->GetDevice(&dev); }) || !dev) {
        if (dev) dev->Release();
        return false;
    }
    bool may = false;
    if (!g_owner) {
        g_owner = dev;
        dev->AddRef();
        may = true;
    } else {
        may = g_owner == dev;
    }
    dev->Release();
    return may;
}

ID3D11ComputeShader* compileShader(ID3D11DeviceContext* ctx) {

    return shaderSwapCreateCs(ctx, kSharpenBytecode, sizeof(kSharpenBytecode), "render_sharpen_cs", "render sharpening");
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

void* sharpenInner(void* srcTex, int eye, const float* bounds, float strength) {
    ID3D11Texture2D* src = nullptr;
    static_cast<IUnknown*>(srcTex)->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src));
    if (!src) return nullptr;

    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);

    bool ok = true;

    // Nothing to do is a refusal too, said once: the openvr half never
    // asks for it, so a call with it is a caller's mistake worth a line.
    if (!(strength > 0.0f)) {
        ok = false;
        if (!g_zeroNoted) {
            g_zeroNoted = true;
            Log::get().note(
                "render sharpening: asked for strength %g, which is nothing "
                "to do. The pass answers null.",
                static_cast<double>(strength));
        }
    }

    // The kinds this pass does not handle, refused rather than guessed at:
    // the resolve's list, for its reasons.
    if (ok && (sd.SampleDesc.Count > 1 || sd.ArraySize != 1 || sd.MipLevels != 1)) {
        ok = false;
        if (!g_kindNoted) {
            g_kindNoted = true;
            Log::get().note(
                "render sharpening: the outgoing texture is %ux%u samples=%u "
                "array=%u mips=%u, a kind the pass does not handle. The pass "
                "stands down.",
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
                "render sharpening: the Submit bounds (u %.3f..%.3f, v "
                "%.3f..%.3f) name no usable eye region of a %ux%u texture. "
                "The pass stands down.",
                bounds ? bounds[0] : 0.0f, bounds ? bounds[2] : 1.0f,
                bounds ? bounds[1] : 0.0f, bounds ? bounds[3] : 1.0f,
                sd.Width, sd.Height);
        }
    }
    const uint32_t regionW = ok ? region[2] - region[0] : 0;
    const uint32_t regionH = ok ? region[3] - region[1] : 0;

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
                    "render sharpening: the outgoing texture's format is %s "
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
    }

    // Before anything below looks at what the pass already holds: a frame from another
    // device than the one it was made on finds none of it (adoptDevice).
    if (ok) adoptDevice(dev);

    if (ok) pollTimingRing(ctx);

    // A typed UAV store on the family's plain format is what the dispatch
    // needs; not promised by every feature level for every family, so ask
    // once.
    if (ok && !g_fmtChecked[fmtIndex]) {
        g_fmtChecked[fmtIndex] = true;
        ++g_fmtAsks;
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
                "render sharpening: this GPU/driver reports no typed "
                "unordered-access support for %s, so the result cannot be "
                "written here. The pass stands down.",
                formatName(viewFmt));
        }
    }

    // The shader, compiled once and cached; a session that calls through
    // the export without the tick having warmed it (the smoke harness)
    // pays the compile on the first call.
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
            failOnce("the outgoing texture refuses a shader view and could "
                     "not be copied");
        }
    }
    if (ok) {
        EyeState& e = *eptr;
        if (!e.outTex || e.outW != regionW || e.outH != regionH ||
            e.outFmt != sd.Format) {
            releaseOut(e);
            if (makeTex(dev, regionW, regionH, sd.Format, viewFmt,
                        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                        &e.outTex, nullptr, &e.outUav)) {
                e.outW = regionW;
                e.outH = regionH;
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

        // The compute stage is saved and put back around the dispatch: the
        // exposure fix and others own slots here and must not find ours.
        ID3D11ComputeShader* savedCs = nullptr;
        ID3D11ShaderResourceView* savedSrv = nullptr;
        ID3D11UnorderedAccessView* savedUav = nullptr;
        ID3D11Buffer* savedCb = nullptr;
        ctx->CSGetShader(&savedCs, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 1, &savedSrv);
        ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
        ctx->CSGetConstantBuffers(0, 1, &savedCb);

        const int qs = acquireQuerySlot(dev, ctx);

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

        PassParams p{};
        FsrRcasCon(reinterpret_cast<AU1*>(p.con),
                   static_cast<AF1>(stopsOf(strength)));
        if (viaCopy) {
            p.region[0] = 0;
            p.region[1] = 0;
            p.region[2] = static_cast<int32_t>(regionW);
            p.region[3] = static_cast<int32_t>(regionH);
        } else {
            for (int i = 0; i < 4; ++i) p.region[i] = static_cast<int32_t>(region[i]);
        }
        p.outSize[0] = static_cast<int32_t>(regionW);
        p.outSize[1] = static_cast<int32_t>(regionH);

        const bool ran = setParams(ctx, p);
        if (ran) {
            ID3D11ShaderResourceView* nullSrv = nullptr;
            ID3D11UnorderedAccessView* nullUav = nullptr;
            ctx->CSSetShaderResources(0, 1, &nullSrv);
            ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
            ctx->CSSetShader(g_cs, nullptr, 0);
            ID3D11Buffer* cb = g_cb;
            ctx->CSSetConstantBuffers(0, 1, &cb);
            ctx->CSSetShaderResources(0, 1, &inSrv);
            ctx->CSSetUnorderedAccessViews(0, 1, &e.outUav, nullptr);
            GpuCensusScope census(ctx, GpuCensusSection::DoorSharpen);
            ctx->Dispatch((regionW + 7) / 8, (regionH + 7) / 8, 1);
        }

        if (qs >= 0) g_qring[qs].timer.end(ctx); // Poll consumes failed End samples too.

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

        // A parameter write that fails means the dispatch never ran, and a
        // texture nothing dispatched into must never go out as a frame.
        if (ran) {
            result = e.outTex;
            ++g_treats;
            g_lastW = regionW;
            g_lastH = regionH;
            g_lastStrength = strength;
            if (!g_firstNoted) {
                g_firstNoted = true;
                Log::get().note(
                    "render sharpening: first sharpened frame -- AMD's RCAS at "
                    "strength %.2f (%.2f stops) over a %ux%u %s (DXGI_FORMAT "
                    "%d) frame, read and written through %s views%s, %s. "
                    "render_sharpness moves the strength, live.",
                    static_cast<double>(strength),
                    static_cast<double>(stopsOf(strength)), regionW, regionH,
                    formatName(sd.Format), static_cast<int>(sd.Format),
                    formatName(viewFmt),
                    viaCopy ? " (copied out first: the source refuses a "
                              "shader view)"
                            : "",
                    flatPass()
                        ? "on the temporal resolve's output, before the game's "
                          "own output copy -- the interface is drawn after it "
                          "and is not sharpened"
                        : "the last pass before the frame leaves");
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

void sharpenPassConfigure(Config& cfg) {
    float v = cfg.getFloat("fix.render_sharpness", 0.0f);
    if (!std::isfinite(v) || v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_strength = v;
    g_wanted = v > 0.0f;
}

void sharpenPassTick(ID3D11DeviceContext* ctx) {
    if (!g_wanted || !ctx) return;
    if (g_firstTickMs == 0) g_firstTickMs = stampMs();
    if (!g_cs && !g_csTried && tickMayWarm(ctx)) {
        g_csTried = true;
        g_cs = compileShader(ctx);
        if (g_cs && !g_warmNoted) {
            g_warmNoted = true;
            Log::get().note(
                "render sharpening: shader warmed at session start -- the "
                "first sharpened %s pays no compile.",
                perUnit());
        }
    }
    if (g_noHookNoted || !elapsedMs(g_firstTickMs, kNoHookNoteMs)) return;
    // The flat profile has no compositor hook to wait for: the pass runs from
    // the flat runtime, on a frame it has resolved, so what it can be waiting
    // on is a frame -- and it is said only if none has come (g_treats).
    // Otherwise the other half's absence, said from this side (the resolve's
    // note, for the same reason).
    const bool say = flatPass()
        ? g_treats == 0
        : !glitchConsumerPresent() && !nativeSharpenActive();
    if (!say) return;
    g_noHookNoted = true;
    char text[640];
    sharpenPassNeverRanText(text, sizeof(text), flatPass(),
                            temporalModeEnabled(Config::get().requestedTemporalMode()),
                            g_strength);
    Log::get().note("%s", text);
}

void sharpenPassNeverRanText(char* out, size_t cap, bool flat, bool antiAliasingOn,
                             float strength) {
    const unsigned long long seconds = kNoHookNoteMs / 1000;
    if (!out || !cap) return;
    if (!flat) {
        snprintf(out, cap,
                 "render sharpening: fix.render_sharpness is %.2f, but no "
                 "compositor hook has announced itself after %llu s. The pass "
                 "runs inside the openvr_api.dll half's Submit hook -- install "
                 "that file, or restart the game with the setting on so the hook "
                 "installs for it. Nothing is sharpened until then.",
                 static_cast<double>(strength), seconds);
    } else if (!antiAliasingOn) {
        snprintf(out, cap,
                 "render sharpening: fix.render_sharpness is %.2f, but no frame "
                 "has been sharpened after %llu s: anti-aliasing is off "
                 "(fix.temporal_aa), and in the flat profile the sharpening runs "
                 "on the temporal pass's output. Turn on TAA, DLSS or FSR and it "
                 "applies at once.",
                 static_cast<double>(strength), seconds);
    } else {
        snprintf(out, cap,
                 "render sharpening: fix.render_sharpness is %.2f, but no frame "
                 "has been sharpened after %llu s, with anti-aliasing on. The "
                 "flat runtime has handed the sharpening no resolved frame -- "
                 "read its 'flat runtime:' lines for why it refuses or falls "
                 "back, and note that a frame it falls back on is left as it "
                 "is.",
                 static_cast<double>(strength), seconds);
    }
}

bool sharpenPassTotals(uint32_t* treated, double* avgMs, double* maxMs) {
    if (g_treats == 0) return false;
    if (treated) *treated = g_treats;
    if (avgMs) *avgMs = g_timeCount ? g_timeSum / static_cast<double>(g_timeCount) : 0.0;
    if (maxMs) *maxMs = g_timeMax;
    return true;
}

void sharpenPassNoteTotals() {
    static uint32_t lastTreats = 0;
    uint32_t treated = 0;
    double avgMs = 0.0, maxMs = 0.0;
    if (!sharpenPassTotals(&treated, &avgMs, &maxMs) || treated == lastTreats) return;
    lastTreats = treated;
    Log::get().note(
        "render sharpening totals: %u %s sharpened this session, %.2f ms per "
        "%s on average (max %.2f).",
        treated, unitsSharpened(), avgMs, perUnit(), maxMs);
}

void sharpenPassShutdown() {
    if (g_treats > 0) {
        Log::get().note(
            "render sharpening: %u %s sharpened this session%s.",
            g_treats, unitsSharpened(),
            g_timeCount ? "" : " (no timing sample completed)");
        if (g_timeCount) {
            Log::get().note(
                "render sharpening: measured %.2f ms per %s on average (max "
                "%.2f) over %u timed passes.",
                g_timeSum / static_cast<double>(g_timeCount), perUnit(),
                g_timeMax, g_timeCount);
        }
    }
    releaseDeviceState();
    if (g_owner) { g_owner->Release(); g_owner = nullptr; }
}

void sharpenPassDeviceResetOffForTest(bool off) {
    g_deviceResetOff = off;
    releaseDeviceState();
    if (g_owner) { g_owner->Release(); g_owner = nullptr; }
    g_deviceChanges = 0;
}

namespace {
// The device a child was made on, as an identity to compare and nothing more: the pass holds the
// device (g_owner) for as long as it holds the child, so the pointer is good for the call.
ID3D11Device* deviceOfChild(ID3D11DeviceChild* child) {
    if (!child) return nullptr;
    ID3D11Device* dev = nullptr;
    child->GetDevice(&dev);
    if (dev) dev->Release();
    return dev;
}
}  // namespace

void sharpenPassHeldForTest(SharpenPassHeld* out) {
    if (!out) return;
    *out = SharpenPassHeld{};
    out->owner = g_owner;
    out->shader = deviceOfChild(g_cs);
    out->buffer = deviceOfChild(g_cb);
    for (int i = 0; i < 2; ++i) {
        out->eyeOut[i] = deviceOfChild(g_eye[i].outTex);
        out->eyeSrcView[i] = deviceOfChild(g_eye[i].srcSrv);
        out->eyeCopy[i] = deviceOfChild(g_eye[i].copyTex);
    }
    out->shaderTried = g_csTried;
    out->formatSupportAsks = g_fmtAsks;
    for (const QuerySlot& q : g_qring) out->queriesInFlight += q.inUse ? 1 : 0;
}

}  // namespace edvr

extern "C" __declspec(dllexport) void* edvrSharpen(void* srcTex, int eye,
                                                   const float* bounds,
                                                   float strength) {
    if (edvr::graphicsRuntimeDisabled() || !srcTex || eye < 0 || eye > 1) return nullptr;
    void* out = nullptr;
    edvr::guardedBudget(edvr::g_budget, [&] {
        out = edvr::sharpenInner(srcTex, eye, bounds, strength);
    });
    return out;
}
