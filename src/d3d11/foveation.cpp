#include "foveation.h"

#include <windows.h>

#include <d3d11.h>
#include <d3d11_1.h>   // ID3D11RasterizerState1: the forced sample count, one of the states a game may set
#include <psapi.h>     // the module census in the ARMED line

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "shader_swap.h"
#include "temporal_pass.h"

namespace edvr {
namespace {

// ------------------------------------------------------------------ NvAPI
//
// Resolved at run time through nvapi64.dll's nvapi_QueryInterface by the
// IDs NVIDIA publishes in nvapi_interface.h (github.com/NVIDIA/nvapi, MIT
// licence), with three structures transcribed from the NvAPI reference
// documentation (R590). Nothing of NVIDIA's is vendored. Every structure
// carries its size in its version word, so a transcription error is
// refused by the driver as an incompatible version rather than acted on;
// the rate values and the view dimension are the two things the driver
// cannot check, and the desk probe at the end of this file measures them.
typedef int32_t NvStatus;  // NvAPI_Status; 0 is NVAPI_OK
constexpr NvStatus kNvOk = 0;
constexpr uint32_t kIdInitialize       = 0x0150E828u;  // NvAPI_Initialize
constexpr uint32_t kIdGetErrorMessage  = 0x6C2D048Cu;  // NvAPI_GetErrorMessage
constexpr uint32_t kIdGraphicsCaps     = 0x52B1499Au;  // NvAPI_D3D1x_GetGraphicsCapabilities
constexpr uint32_t kIdCreateRateView   = 0x99CA2DFFu;  // NvAPI_D3D11_CreateShadingRateResourceView
constexpr uint32_t kIdSetRateView      = 0x1B0C2F83u;  // NvAPI_D3D11_RSSetShadingRateResourceView
constexpr uint32_t kIdSetViewportRates = 0x34F7938Fu;  // NvAPI_D3D11_RSSetViewportsPixelShadingRates
constexpr uint32_t kIdRegisterDevice   = 0x8C02C4D0u;  // NvAPI_D3D_RegisterDevice (unused; resolved for the record)
constexpr uint32_t kIdEnumPhysicalGpus = 0xE5AC921Fu;  // NvAPI_EnumPhysicalGPUs
constexpr uint32_t kIdGpuDynamicPstate = 0x60DED2EDu;  // NvAPI_GPU_GetDynamicPstatesInfoEx

constexpr uint32_t nvVersion(uint32_t size, uint32_t ver) { return size | (ver << 16); }

// NV_D3D1x_GRAPHICS_CAPS_V2: three capability bits and 29 reserved in one
// word, four SM version shorts, thirteen reserved words. The function
// takes the version as its own argument, so the structure carries none.
struct NvGraphicsCapsV2 {
    uint32_t bits;
    uint16_t majorSm, minorSm, majorCudaSm, minorCudaSm;
    uint32_t reserved[13];
};
static_assert(sizeof(NvGraphicsCapsV2) == 64, "NV_D3D1x_GRAPHICS_CAPS_V2 is 64 bytes");
constexpr uint32_t kCapsVer2 = nvVersion(sizeof(NvGraphicsCapsV2), 2);
constexpr uint32_t kCapsBitVrs = 1u << 1;  // bVariablePixelRateShadingSupported

// NV_D3D11_VIEWPORT_SHADING_RATE_DESC_V1: an NvBool (one byte) and sixteen
// NV_PIXEL_SHADING_RATE values, indexed by the image's texel value.
constexpr uint32_t kRateTableSize = 16;
struct NvViewportRates {
    uint8_t  enable;
    uint8_t  pad[3];
    uint32_t table[kRateTableSize];
};
static_assert(sizeof(NvViewportRates) == 68, "NV_D3D11_VIEWPORT_SHADING_RATE_DESC_V1 is 68 bytes");
// NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_V1.
struct NvViewportsRates {
    uint32_t         version;
    uint32_t         numViewports;
    NvViewportRates* viewports;
};
static_assert(sizeof(NvViewportsRates) == 16, "NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_V1 is 16 bytes");
constexpr uint32_t kViewportsVer1 = nvVersion(sizeof(NvViewportsRates), 1);

// NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_V1: version, the DXGI format,
// the view dimension, then the Texture2D / Texture2DArray union.
struct NvRateViewDesc {
    uint32_t version;
    uint32_t format;
    uint32_t dimension;
    union {
        struct { uint32_t mipSlice; } tex2d;
        struct { uint32_t mipSlice, firstSlice, arraySize; } tex2dArray;
    };
};
static_assert(sizeof(NvRateViewDesc) == 24, "NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_V1 is 24 bytes");
constexpr uint32_t kRateViewVer1 = nvVersion(sizeof(NvRateViewDesc), 1);
constexpr uint32_t kDimTexture2D = 4;  // NV_SRRV_DIMENSION_TEXTURE2D

// NV_PIXEL_SHADING_RATE: one shade per raster pixel, and per block.
constexpr uint32_t kRateCull = 0;  // NV_PIXEL_X0_CULL_RASTER_PIXELS: the tile is not rasterised
constexpr uint32_t kRate1x1 = 5;   // NV_PIXEL_X1_PER_RASTER_PIXEL
constexpr uint32_t kRate2x1 = 6;   // NV_PIXEL_X1_PER_2X1_RASTER_PIXELS
constexpr uint32_t kRate1x2 = 7;   // NV_PIXEL_X1_PER_1X2_RASTER_PIXELS
constexpr uint32_t kRate2x2 = 8;   // NV_PIXEL_X1_PER_2X2_RASTER_PIXELS
constexpr uint32_t kRate4x2 = 9;   // NV_PIXEL_X1_PER_4X2_RASTER_PIXELS
constexpr uint32_t kRate2x4 = 10;  // NV_PIXEL_X1_PER_2X4_RASTER_PIXELS
constexpr uint32_t kRate4x4 = 11;  // NV_PIXEL_X1_PER_4X4_RASTER_PIXELS
constexpr uint32_t kTile = 16;     // NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH and _HEIGHT

typedef void*    (__cdecl* PFN_NvQueryInterface)(uint32_t id);
typedef NvStatus (__cdecl* PFN_NvInitialize)();
typedef NvStatus (__cdecl* PFN_NvGetErrorMessage)(NvStatus status, char* out64);
typedef NvStatus (__cdecl* PFN_NvGraphicsCaps)(IUnknown* device, uint32_t structVersion, void* caps);
typedef NvStatus (__cdecl* PFN_NvCreateRateView)(ID3D11Device* device, ID3D11Resource* resource,
                                                 const NvRateViewDesc* desc, IUnknown** view);
typedef NvStatus (__cdecl* PFN_NvSetRateView)(IUnknown* context, IUnknown* view);
typedef NvStatus (__cdecl* PFN_NvSetViewportRates)(IUnknown* context, NvViewportsRates* desc);
typedef NvStatus (__cdecl* PFN_NvRegisterDevice)(IUnknown* device);

// NV_GPU_DYNAMIC_PSTATES_INFO_EX: version, flags, and eight utilisation
// domains of {bIsPresent:1, percentage} -- domain 0 is the graphics engine,
// "percentage of time where the domain is considered busy in the last 1
// second interval". The whole GPU, not this process: the question it
// answers is whether the GPU was the frame's limit at all.
struct NvGpuUtilisation {
    uint32_t present;   // bit 0
    uint32_t percentage;
};
struct NvDynamicPstates {
    uint32_t         version, flags;
    NvGpuUtilisation utilisation[8];
};
static_assert(sizeof(NvDynamicPstates) == 72, "NV_GPU_DYNAMIC_PSTATES_INFO_EX is 72 bytes");
constexpr uint32_t kDynamicPstatesVer1 = nvVersion(sizeof(NvDynamicPstates), 1);
constexpr uint32_t kMaxPhysicalGpus = 64;  // NVAPI_MAX_PHYSICAL_GPUS
typedef NvStatus (__cdecl* PFN_NvEnumPhysicalGpus)(void** handles, uint32_t* count);
typedef NvStatus (__cdecl* PFN_NvGpuDynamicPstates)(void* gpu, NvDynamicPstates* info);

struct NvApi {
    bool                   tried = false;
    bool                   ok = false;
    PFN_NvInitialize       initialize = nullptr;
    PFN_NvGetErrorMessage  errorMessage = nullptr;
    PFN_NvGraphicsCaps     graphicsCaps = nullptr;
    PFN_NvCreateRateView   createRateView = nullptr;
    PFN_NvSetRateView      setRateView = nullptr;
    PFN_NvSetViewportRates setViewportRates = nullptr;
    PFN_NvRegisterDevice   registerDevice = nullptr;    // optional
    PFN_NvEnumPhysicalGpus enumGpus = nullptr;          // optional: the busy figure
    PFN_NvGpuDynamicPstates dynamicPstates = nullptr;   // optional
    PFN_NvQueryInterface   query = nullptr;
    void*                  gpu = nullptr;               // the first physical GPU, once enumerated
    bool                   gpuTried = false;
    char                   why[240] = {};
};
NvApi       g_nv;
FaultBudget g_budget("foveation", 3);

// Every line this file builds goes through this.
//
// snprintf returns what it WOULD have written, so `len += snprintf(...)`
// walks the cursor PAST the buffer as soon as one field is truncated, and
// the next call is handed a pointer past the end and a size that wrapped
// through zero -- which writes over whatever static follows, and what
// follows here is the rate table NvAPI reads by pointer. The pre-ship
// review of 2026-09-06 found three builders doing exactly that, one of
// them a summary line that grows with the number of render target sizes a
// session has seen.
struct Text {
    char*  buf;
    size_t cap;
    size_t len = 0;
    bool   full() const { return cap == 0 || len + 1 >= cap; }
    template <class... A>
    void add(const char* fmt, A... a) {
        if (full()) return;
        const int n = snprintf(buf + len, cap - len, fmt, a...);
        if (n <= 0) return;
        const size_t room = cap - len - 1;
        len += (static_cast<size_t>(n) < room) ? static_cast<size_t>(n) : room;
    }
};

const char* nvError(NvStatus s, char* buf, size_t n) {
    char msg[64] = {};
    bool got = false;
    if (g_nv.errorMessage) {
        guardedBudget(g_budget, [&] { got = g_nv.errorMessage(s, msg) == kNvOk; });
    }
    msg[63] = 0;
    if (got && msg[0]) snprintf(buf, n, "%s (%d)", msg, s);
    else snprintf(buf, n, "NvAPI status %d", s);
    return buf;
}

// nvapi64.dll and the six entry points, once; a miss is final for the
// session and g_nv.why says what it was.
bool nvArm() {
    if (g_nv.tried) return g_nv.ok;
    g_nv.tried = true;
    HMODULE lib = LoadLibraryW(L"nvapi64.dll");
    if (!lib) {
        snprintf(g_nv.why, sizeof(g_nv.why),
                 "nvapi64.dll is not on this machine (no NVIDIA driver)");
        return false;
    }
    PFN_NvQueryInterface query =
        reinterpret_cast<PFN_NvQueryInterface>(GetProcAddress(lib, "nvapi_QueryInterface"));
    if (!query) {
        snprintf(g_nv.why, sizeof(g_nv.why), "nvapi64.dll exports no nvapi_QueryInterface");
        return false;
    }
    g_nv.query = query;
    bool survived = guardedBudget(g_budget, [&] {
        g_nv.initialize = reinterpret_cast<PFN_NvInitialize>(query(kIdInitialize));
        g_nv.registerDevice = reinterpret_cast<PFN_NvRegisterDevice>(query(kIdRegisterDevice));
        g_nv.enumGpus = reinterpret_cast<PFN_NvEnumPhysicalGpus>(query(kIdEnumPhysicalGpus));
        g_nv.dynamicPstates = reinterpret_cast<PFN_NvGpuDynamicPstates>(query(kIdGpuDynamicPstate));
        g_nv.errorMessage = reinterpret_cast<PFN_NvGetErrorMessage>(query(kIdGetErrorMessage));
        g_nv.graphicsCaps = reinterpret_cast<PFN_NvGraphicsCaps>(query(kIdGraphicsCaps));
        g_nv.createRateView = reinterpret_cast<PFN_NvCreateRateView>(query(kIdCreateRateView));
        g_nv.setRateView = reinterpret_cast<PFN_NvSetRateView>(query(kIdSetRateView));
        g_nv.setViewportRates = reinterpret_cast<PFN_NvSetViewportRates>(query(kIdSetViewportRates));
    });
    if (!survived) {
        snprintf(g_nv.why, sizeof(g_nv.why), "nvapi_QueryInterface FAULTED (caught)");
        return false;
    }
    if (!g_nv.initialize || !g_nv.createRateView || !g_nv.setRateView || !g_nv.setViewportRates) {
        snprintf(g_nv.why, sizeof(g_nv.why),
                 "this driver's NvAPI has no D3D11 shading-rate entry points (a driver older "
                 "than R435, or not NVIDIA's)");
        return false;
    }
    NvStatus rc = -1;
    survived = guardedBudget(g_budget, [&] { rc = g_nv.initialize(); });
    if (!survived || rc != kNvOk) {
        char e[96];
        snprintf(g_nv.why, sizeof(g_nv.why), "NvAPI_Initialize answered %s",
                 survived ? nvError(rc, e, sizeof(e)) : "a fault (caught)");
        return false;
    }
    g_nv.ok = true;
    return true;
}

// The capability bit: 1 supported, 0 not, -1 the query was refused or is
// absent (an older driver's structure), in which case the view decides.
int nvVrsSupported(ID3D11Device* dev, char* smOut, size_t smCap) {
    if (!g_nv.graphicsCaps || !dev) return -1;
    NvGraphicsCapsV2 caps = {};
    NvStatus rc = -1;
    if (!guardedBudget(g_budget, [&] { rc = g_nv.graphicsCaps(dev, kCapsVer2, &caps); })) return -1;
    if (rc != kNvOk) return -1;
    if (smOut) snprintf(smOut, smCap, "SM %u.%u", caps.majorSm, caps.minorSm);
    return (caps.bits & kCapsBitVrs) ? 1 : 0;
}

// ------------------------------------------------------------- settings
enum class Mode { Off, Quality, Balanced, Performance };
Mode     g_mode = Mode::Off;
float    g_innerDeg = 50.0f;   // degrees across the full-rate disc
float    g_outerDeg = 84.0f;   // degrees across the 2x2 ring's outer edge
float    g_distance = 0.7f;    // metres, the nasal shift's fixation distance
bool     g_geometryOnly = false;
bool     g_followEyes = true;   // fix.foveation_centre = eyes
// advanced.foveation_outer_rate. The sentinel for "the preset's own" is NOT
// zero: zero is NV_PIXEL_X0_CULL_RASTER_PIXELS, the cull rate itself, and
// while it was the sentinel the cull setting fell straight through to the
// preset -- the flight of 2026-09-06 06:39 ran with "cull" set and its own
// ARMED line said "one per 4x4 beyond". Nothing was ever culled, and the
// periphery Sean watched was 4x4-shaded, not undrawn.
constexpr uint32_t kRatePreset = 0xFFFFFFFFu;
uint32_t g_outerOverride = kRatePreset;
bool     g_cullInner = false;   // the diagnostic that culls the full-rate disc itself
bool     g_cullAll = false;     // ...and every tile of the eye
// THE STRIP ONLY ONE EYE CAN SEE.
//
// The two frusta are mirrored and asymmetric: on the Crystal Super each
// eye reaches 1.529 in tangent to its own temporal side and 1.032 to the
// nose. So the far temporal strip of each eye, everything past the other
// eye's nasal reach, is territory NO other eye covers -- and equally,
// nothing there can disagree with the other eye, because the other eye
// has nothing there to disagree with.
//
// That is what makes it the one place culling is free of rivalry. Cull
// inside the binocular overlap and one eye goes black where its partner
// still draws, which the brain will not let you ignore. Cull the
// monocular strip and the only cost is a slightly narrower field, at the
// very edge of vision, in both eyes symmetrically.
//
// It is about a fifth of each eye's width here, and unlike a shading rate
// a cull stops the WRITES as well as the shading, which is where the eye
// draws actually spend their time (see docs/performance.md, feature 2).
// Sean's idea, 2026-09-06.
bool     g_monoEdge = false;
// The attribution test: blacken every tile of the eye the module BELIEVES
// it is drawing. Which of the commander's own eyes goes dark is then the
// measurement -- "cull_left" darkening the right eye says the two are
// swapped, which the flight of 2026-09-06 08:00 suspected from the rings
// landing on the wrong side. -1 is off; 0 and 1 are the module's own
// numbering, 0 being the eye the openvr half submits as Eye_Left.
int      g_cullEye = -1;
uint32_t g_settingsGen = 1;    // bumped on any change; images refill at their next use
bool     g_configured = false;

// The eye-tracked centre (frame_flag.h: the openvr half's gaze source).
// The rings are refilled when the gaze has moved past a dead band from the
// centre they were last built on; a gaze published as lost, or silent for
// a second, returns the centre to straight ahead.
bool     g_gazeValid = false;
float    g_gazeTx = 0.0f, g_gazeTy = 0.0f;
uint32_t g_gazeStamp = 0;
uint32_t g_gazeAge = 0;
bool     g_maskCentreValid = false;
float    g_maskTx = 0.0f, g_maskTy = 0.0f;
float    g_builtTx = 0.0f, g_builtTy = 0.0f;   // the centre the last mask was actually built on
uint32_t g_maskLines = 0;       // the geometry lines a session prints, capped
uint32_t g_masksMade = 0;
uint64_t g_maskRefills = 0;    // refills done at the frame boundary
uint32_t g_gazeRefills = 0;
uint32_t g_gazeLosses = 0;      // frames the source published "lost", blinks included
uint32_t g_wantedFrames = 0;    // frames wanted but never armed
uint64_t g_wantedSince = 0;     // tick of the first such frame
bool     g_wantedNoted = false;
uint64_t g_gazeFramesFollowed = 0;
bool     g_gazeNotedOn = false;
bool     g_gazeNotedLost = false;
constexpr float kGazeDeadBand = 0.026f;   // tangent, about 1.5 degrees

const char* modeName(Mode m) {
    switch (m) {
        case Mode::Quality: return "quality";
        case Mode::Balanced: return "balanced";
        case Mode::Performance: return "performance";
        default: return "off";
    }
}

// --------------------------------------------------------------- state
enum class Phase { Off, Wanted, Armed, Down };
Phase g_phase = Phase::Off;

// One shading-rate image per eye-texture size and eye. The views are never
// released: OpenXR Toolkit found releasing them, and NvAPI_Unload, crashed,
// and a handful of tiny views per session is the cheaper bargain.
struct Mask {
    bool             used = false;
    ID3D11Texture2D* tex = nullptr;
    IUnknown*        view = nullptr;
    uint32_t         w = 0, h = 0, tw = 0, th = 0;
    int              eye = 0;
    uint32_t         gen = 0;        // the settings generation it was filled at; 0 = unfilled
    uint32_t         lastFrame = 0;
};
Mask g_masks[8];

// The frame counter, declared here rather than with the rest of the
// per-frame state further down because the target table below ages its
// entries by it.
uint32_t g_frame = 0;

// WHICH EYE A TARGET BELONGS TO
//
// It matters more than it looks: the two eyes' frusta are mirrored, so
// their straight-ahead points sit a fifth of the image apart (NDC +0.194
// and -0.194 on the Crystal Super), and a target wearing the other eye's
// mask has its disc that far out. That is the stereo rivalry Sean saw
// under the cull on 2026-09-06, and the census had already said the
// attribution was wrong: 6.5 targets a frame to the left eye against 4.6
// to the right, where the truth is one each.
//
// The parity guess that produced those numbers -- the first target of a
// size is the left eye's, the second the right -- breaks on any size the
// game uses an odd number of times. What is true instead: the textures
// the openvr half sees submitted ARE each eye's last target, so every
// target bound before one, since the previous submitted target, belongs
// to the same eye. So the frame boundary walks the frame's targets
// backwards and gives each the eye of the next submitted one, and that
// verdict is remembered per resource for every frame after. The first
// frame after a target appears may be wrong; from the second it is the
// game's own order that says so.
struct Seen {
    void*    resource;
    uint32_t w, h, fmt;
    int      eye;
    bool     bySubmit;   // this one WAS a submitted texture: ground truth
};
Seen     g_seen[48];
uint32_t g_seenCount = 0;

// The verdicts that outlive the frame.
struct Known {
    void*    resource = nullptr;
    uint32_t lastSeen = 0;   // the frame it was last drawn into
    int      eye = 0;
    bool     settled = false;   // by a submitted texture, or by the walk behind one
    bool     everSubmitted = false;
    int      submittedAs = -1;  // the eye it was seen submitted as, if ever
    uint32_t w = 0, h = 0, fmt = 0;
    uint64_t draws = 0;
    uint32_t corrections = 0;
};
Known    g_known[64];
uint32_t g_knownCount = 0;
uint64_t g_settledBySubmit = 0, g_settledByOrder = 0, g_unsettled = 0, g_corrections = 0;
uint32_t g_knownEvictions = 0;

Known* knownFor(void* resource) {
    for (uint32_t i = 0; i < g_knownCount; ++i) {
        if (g_known[i].resource == resource) {
            g_known[i].lastSeen = g_frame;
            return &g_known[i];
        }
    }
    Known* slot = nullptr;
    if (g_knownCount < sizeof(g_known) / sizeof(g_known[0])) {
        slot = &g_known[g_knownCount++];
    } else {
        // Full: take the one least recently drawn into rather than
        // returning nothing. Returning nothing left every later target --
        // the submitted ones included -- unattributed and so at full rate
        // for the rest of the session, silently (the pre-ship review of
        // 2026-09-06). A table this size only fills when the game has
        // recreated its targets many times over, and the oldest entry is
        // then the one least likely to be a live texture.
        slot = &g_known[0];
        for (Known& k : g_known) {
            if (k.lastSeen < slot->lastSeen) slot = &k;
        }
        ++g_knownEvictions;
    }
    *slot = Known();
    slot->resource = resource;
    slot->lastSeen = g_frame;
    return slot;
}

// The eye targets named one by one, busiest first: the masks are right --
// the flight of 2026-09-06 08:23 printed both eyes' geometry exactly as
// the runtime measured it -- so what is left is which mask each target
// gets, and that cannot be read from totals.
const char* targetsText() {
    static char buf[900];
    Text t{buf, sizeof(buf)};
    const Known* order[64] = {};
    int n = 0;
    for (uint32_t i = 0; i < g_knownCount; ++i) {
        if (g_known[i].draws) order[n++] = &g_known[i];
    }
    for (int i = 1; i < n; ++i) {
        const Known* k = order[i];
        int j = i;
        while (j > 0 && order[j - 1]->draws < k->draws) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = k;
    }
    for (int i = 0; i < n && i < 8 && !t.full(); ++i) {
        const Known& k = *order[i];
        t.add("%s#%d %ux%u fmt %u: %s, settled %s%s, %llu draws%s", t.len ? "; " : "",
              static_cast<int>(&k - g_known), k.w, k.h, k.fmt, k.eye == 0 ? "LEFT" : "RIGHT",
              k.settled ? "yes" : "NO",
              k.everSubmitted ? (k.submittedAs == 0 ? " (submitted as LEFT)" : " (submitted as RIGHT)") : "",
              static_cast<unsigned long long>(k.draws), k.corrections ? " [corrected]" : "");
    }
    return t.len ? buf : "none";
}

// The census: every target the game draws into while the feature is armed,
// by size and format, with its draws split into those under the image,
// eye-sized draws without it, and draws the census called something other
// than an eye. One resolve per rebind, one counter per draw. The 18:15
// flight's single summary landed in the loading screen and said nothing
// about the scene, so this is periodic, and it names the targets the
// coarse shading did NOT reach as well as the ones it did.
struct Sig {
    bool     used = false;
    uint32_t w = 0, h = 0, fmt = 0;
    uint64_t under = 0;   // eye draws into it with the image bound
    uint64_t bare = 0;    // eye draws into it with no image (none made yet, or the passes filter)
    uint64_t other = 0;   // draws into it while the census called it not eye-sized
};
Sig      g_sigs[16];
Sig*     g_censusSig = nullptr;
struct Known;
Known*   g_censusKnown = nullptr;
uint32_t g_censusGen = ~0u;
uint64_t g_eyeDraws = 0;       // eye-sized draws while armed
uint64_t g_eyeDrawsUnder = 0;  // ...with the image bound
uint64_t g_otherDraws = 0;     // draws into anything else while armed
uint64_t g_unknownEyeDraws = 0;   // ...left at full rate because the eye is not known

// The state the game's eye draws run under, for the summary: the cull
// flight of 2026-09-06 06:39 left the periphery lit with every eye draw
// under the image, so something the game sets defeats it that the desk
// never set. Per target, once: the view's dimension and the texture's
// array size, sample count and mips. Per sampled eye draw: the rasteriser
// state's flags (fill, cull, multisample, antialiased lines, scissor, depth
// clip, forced sample count), whether a geometry shader is bound, the
// viewport count and the first viewport's rectangle.
struct SigDesc {
    bool     known = false;
    uint32_t viewDim = 0;      // D3D11_RTV_DIMENSION
    uint32_t arraySize = 0, samples = 0, mips = 0, bind = 0, misc = 0;
};
SigDesc g_sigDesc[16];

struct RasterSeen {
    bool     used = false;
    uint32_t packed = 0;
    uint32_t forcedSamples = 0;
    uint64_t draws = 0;
};
RasterSeen g_rasters[8];
uint64_t   g_rasterNullDraws = 0;   // draws with no rasteriser state object bound (the default state)
uint64_t   g_gsDraws = 0, g_sampledDraws = 0;
uint64_t   g_a2cDraws = 0, g_hsDraws = 0;
// A rasteriser state with a forced sample count, seen on an eye draw. With
// the image bound that combination REMOVED THE DEVICE on the desk
// (2026-09-06), so the feature stands down the moment one is seen rather
// than waiting to find out. Elite's own eye draws read zero in every
// flight sampled, so this is a guard against a pass none of them caught.
uint32_t   g_forcedSamplesSeen = 0;
uint32_t   g_topologies[4] = {}, g_topologyDraws[4] = {};
uint32_t   g_vpCount = 0;
float      g_vp[4] = {};
uint32_t   g_vpTallies = 0, g_vpOddTallies = 0;   // viewports not at the origin or not the target's size

void describeTarget(void* rtv, Sig* sig) {
    if (!rtv || !sig) return;
    const size_t index = static_cast<size_t>(sig - g_sigs);
    if (index >= 16 || g_sigDesc[index].known) return;
    SigDesc& d = g_sigDesc[index];
    d.known = true;
    guardedBudget(g_budget, [&] {
        ID3D11RenderTargetView* view = static_cast<ID3D11RenderTargetView*>(rtv);
        D3D11_RENDER_TARGET_VIEW_DESC vd = {};
        view->GetDesc(&vd);
        d.viewDim = static_cast<uint32_t>(vd.ViewDimension);
        ID3D11Resource* res = nullptr;
        view->GetResource(&res);
        if (res) {
            ID3D11Texture2D* tex = nullptr;
            res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
            if (tex) {
                D3D11_TEXTURE2D_DESC td = {};
                tex->GetDesc(&td);
                d.arraySize = td.ArraySize;
                d.samples = td.SampleDesc.Count;
                d.mips = td.MipLevels;
                d.bind = td.BindFlags;
                d.misc = td.MiscFlags;
                tex->Release();
            }
            res->Release();
        }
    });
}

void sampleDrawState(ID3D11DeviceContext* ctx) {
    ++g_sampledDraws;
    guardedBudget(g_budget, [&] {
        ID3D11RasterizerState* rs = nullptr;
        ctx->RSGetState(&rs);
        if (!rs) {
            ++g_rasterNullDraws;
        } else {
            D3D11_RASTERIZER_DESC rd = {};
            rs->GetDesc(&rd);
            uint32_t forced = 0;
            ID3D11RasterizerState1* rs1 = nullptr;
            rs->QueryInterface(__uuidof(ID3D11RasterizerState1), reinterpret_cast<void**>(&rs1));
            if (rs1) {
                D3D11_RASTERIZER_DESC1 rd1 = {};
                rs1->GetDesc1(&rd1);
                forced = rd1.ForcedSampleCount;
                rs1->Release();
            }
            if (forced != 0) g_forcedSamplesSeen = forced;
            const uint32_t packed = (rd.FillMode & 3u) | ((rd.CullMode & 3u) << 2) | ((rd.FrontCounterClockwise ? 1u : 0u) << 4) |
                                    ((rd.DepthClipEnable ? 1u : 0u) << 5) | ((rd.ScissorEnable ? 1u : 0u) << 6) |
                                    ((rd.MultisampleEnable ? 1u : 0u) << 7) | ((rd.AntialiasedLineEnable ? 1u : 0u) << 8) |
                                    ((rd.DepthBias ? 1u : 0u) << 9) | ((forced & 0xFu) << 10);
            rs->Release();
            RasterSeen* slot = nullptr;
            for (RasterSeen& r : g_rasters) {
                if (r.used && r.packed == packed) { slot = &r; break; }
            }
            if (!slot) {
                for (RasterSeen& r : g_rasters) {
                    if (!r.used) { r.used = true; r.packed = packed; r.forcedSamples = forced; slot = &r; break; }
                }
            }
            if (slot) ++slot->draws;
        }
        ID3D11GeometryShader* gs = nullptr;
        ctx->GSGetShader(&gs, nullptr, nullptr);
        if (gs) {
            ++g_gsDraws;
            gs->Release();
        }
        ID3D11HullShader* hs = nullptr;
        ctx->HSGetShader(&hs, nullptr, nullptr);
        if (hs) {
            ++g_hsDraws;
            hs->Release();
        }
        ID3D11BlendState* bs = nullptr;
        FLOAT factor[4];
        UINT mask = 0;
        ctx->OMGetBlendState(&bs, factor, &mask);
        if (bs) {
            D3D11_BLEND_DESC bd = {};
            bs->GetDesc(&bd);
            if (bd.AlphaToCoverageEnable) ++g_a2cDraws;
            bs->Release();
        }
        D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ctx->IAGetPrimitiveTopology(&topo);
        for (int i = 0; i < 4; ++i) {
            if (g_topologyDraws[i] == 0 || g_topologies[i] == static_cast<uint32_t>(topo)) {
                g_topologies[i] = static_cast<uint32_t>(topo);
                ++g_topologyDraws[i];
                break;
            }
        }
        UINT n = 0;
        ctx->RSGetViewports(&n, nullptr);
        if (n > 0 && n <= D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) {
            D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
            ctx->RSGetViewports(&n, vps);
            g_vpCount = n;
            g_vp[0] = vps[0].TopLeftX;
            g_vp[1] = vps[0].TopLeftY;
            g_vp[2] = vps[0].Width;
            g_vp[3] = vps[0].Height;
            ++g_vpTallies;
            if (vps[0].TopLeftX != 0.0f || vps[0].TopLeftY != 0.0f) ++g_vpOddTallies;
        }
    });
}

const char* stateText() {
    static char buf[900];
    Text t{buf, sizeof(buf)};
    t.add("targets:");
    for (int i = 0; i < 16 && !t.full(); ++i) {
        if (!g_sigs[i].used || !g_sigDesc[i].known) continue;
        const SigDesc& d = g_sigDesc[i];
        t.add(" %ux%u view dim %u (4 = tex2d, 5 = tex2d array), array %u, %u sample(s), %u mip(s), bind 0x%X;",
              g_sigs[i].w, g_sigs[i].h, d.viewDim, d.arraySize, d.samples, d.mips, d.bind);
    }
    t.add(" rasteriser states at %llu sampled eye draws (%llu with none bound):",
          static_cast<unsigned long long>(g_sampledDraws), static_cast<unsigned long long>(g_rasterNullDraws));
    for (const RasterSeen& r : g_rasters) {
        if (!r.used || t.full()) continue;
        const uint32_t p = r.packed;
        t.add(" [%llu draws: fill %u, cull %u, ccw %u, depthclip %u, scissor %u, MULTISAMPLE %u, aalines %u, bias %u, forced samples %u]",
              static_cast<unsigned long long>(r.draws), p & 3u, (p >> 2) & 3u, (p >> 4) & 1u, (p >> 5) & 1u,
              (p >> 6) & 1u, (p >> 7) & 1u, (p >> 8) & 1u, (p >> 9) & 1u, r.forcedSamples);
    }
    t.add("; geometry shader bound on %llu of them, hull shader on %llu, alpha-to-coverage on %llu; topologies (4 = triangle list, 5 = strip, 33+ = patches):",
          static_cast<unsigned long long>(g_gsDraws), static_cast<unsigned long long>(g_hsDraws),
          static_cast<unsigned long long>(g_a2cDraws));
    for (int i = 0; i < 4 && !t.full(); ++i) {
        if (g_topologyDraws[i]) t.add(" %u x%u", g_topologies[i], g_topologyDraws[i]);
    }
    t.add("; viewports %u, the first at (%.0f, %.0f) %.0fx%.0f, off the origin on %u of %u tallies",
          g_vpCount, g_vp[0], g_vp[1], g_vp[2], g_vp[3], g_vpOddTallies, g_vpTallies);
    return buf;
}

Sig* sigFor(const ResourceInfo& info) {
    for (Sig& s : g_sigs) {
        if (s.used && s.w == info.a && s.h == info.b && s.fmt == info.fmt) return &s;
    }
    for (Sig& s : g_sigs) {
        if (!s.used) {
            s.used = true;
            s.w = info.a;
            s.h = info.b;
            s.fmt = info.fmt;
            return &s;
        }
    }
    return nullptr;
}

const char* fmtName(uint32_t fmt) {
    switch (fmt) {
        case 2: return "R32G32B32A32_FLOAT";
        case 10: return "R16G16B16A16_FLOAT";
        case 11: return "R16G16B16A16_UNORM";
        case 16: return "R32G32_FLOAT";
        case 23: return "R10G10B10A2_TYPELESS";
        case 24: return "R10G10B10A2_UNORM";
        case 26: return "R11G11B10_FLOAT";
        case 27: return "R8G8B8A8_TYPELESS";
        case 28: return "R8G8B8A8_UNORM";
        case 29: return "R8G8B8A8_UNORM_SRGB";
        case 33: return "R16G16_TYPELESS";
        case 34: return "R16G16_FLOAT";
        case 39: return "R32_TYPELESS";
        case 41: return "R32_FLOAT";
        case 53: return "R16_TYPELESS";
        case 54: return "R16_FLOAT";
        case 56: return "R16_UNORM";
        case 60: return "R8_TYPELESS";
        case 61: return "R8_UNORM";
        case 87: return "B8G8R8A8_UNORM";
        case 90: return "B8G8R8A8_TYPELESS";
        case 91: return "B8G8R8A8_UNORM_SRGB";
        default: return nullptr;
    }
}

IUnknown* g_bound = nullptr;        // the view the context holds, as far as we know
bool      g_boundUnknown = false;   // ClearState: whatever was bound may be gone
bool      g_ratesOn = false;
uint32_t  g_lastRtvGen = ~0u;
Mask*     g_lastMask = nullptr;
bool      g_lastEyeUnknown = false;
uint32_t  g_appliedGen = ~0u;        // the binding generation the image was last applied at

NvViewportRates  g_rates[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
NvViewportsRates g_ratesDesc = {};

uint8_t* g_scratch = nullptr;
size_t   g_scratchCap = 0;

// Statistics for the summary lines.
uint32_t g_framesArmed = 0;
uint64_t g_switches = 0;
uint32_t g_switchesFrame = 0;
uint32_t g_switchesMax = 0;
uint64_t g_targetsSum[2] = {};
uint32_t g_targetsFrame[2] = {};
uint64_t g_submitMatched = 0;
uint32_t g_imagesMade = 0;
uint32_t g_noTangentFrames = 0;
bool     g_noTangentNoted = false;
bool     g_summaryDone = false;
bool     g_boundOnce = false;
// The GPU's busy figure, one sample a second since the last summary.
uint64_t g_gpuBusySum = 0;
uint32_t g_gpuBusyN = 0;
uint32_t g_gpuBusyMax = 0;
uint32_t g_gpuBusyAbsent = 0;

void gpuBusySample() {
    if (!g_nv.ok || !g_nv.enumGpus || !g_nv.dynamicPstates) return;
    if (!g_nv.gpuTried) {
        g_nv.gpuTried = true;
        void* handles[kMaxPhysicalGpus] = {};
        uint32_t count = 0;
        NvStatus rc = -1;
        guardedBudget(g_budget, [&] { rc = g_nv.enumGpus(handles, &count); });
        if (rc == kNvOk && count > 0) g_nv.gpu = handles[0];
    }
    if (!g_nv.gpu) return;
    NvDynamicPstates info = {};
    info.version = kDynamicPstatesVer1;
    NvStatus rc = -1;
    guardedBudget(g_budget, [&] { rc = g_nv.dynamicPstates(g_nv.gpu, &info); });
    if (rc != kNvOk || !(info.utilisation[0].present & 1u)) {
        ++g_gpuBusyAbsent;
        return;
    }
    const uint32_t pct = info.utilisation[0].percentage > 100 ? 100 : info.utilisation[0].percentage;
    g_gpuBusySum += pct;
    ++g_gpuBusyN;
    if (pct > g_gpuBusyMax) g_gpuBusyMax = pct;
}

void fillRates(bool enable, uint32_t inner, uint32_t mid, uint32_t outer) {
    for (NvViewportRates& r : g_rates) {
        r.enable = enable ? 1 : 0;
        memset(r.pad, 0, sizeof(r.pad));
        for (uint32_t& t : r.table) t = kRate1x1;
        r.table[0] = inner;
        r.table[1] = mid;
        r.table[2] = outer;
        // Entry 3 is the attribution test's: only the masks of the eye
        // under test are written with it, and nothing else ever is.
        r.table[3] = kRateCull;
    }
    g_ratesDesc.version = kViewportsVer1;
    g_ratesDesc.numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    g_ratesDesc.viewports = g_rates;
}

uint32_t innerRate() { return (g_cullAll || g_cullInner) ? kRateCull : kRate1x1; }
uint32_t midRate() { return g_cullAll ? kRateCull : kRate2x2; }
uint32_t outerRate() {
    if (g_cullAll) return kRateCull;
    if (g_outerOverride != kRatePreset) return g_outerOverride;
    return g_mode == Mode::Quality ? kRate2x2 : kRate4x4;
}
const char* rateName(uint32_t rate) {
    switch (rate) {
        case kRateCull: return "CULLED (not drawn at all)";
        case kRate1x1: return "1x1";
        case kRate2x2: return "2x2";
        case kRate4x4: return "4x4";
        default: return "?";
    }
}
const char* outerRateName() { return rateName(outerRate()); }
const char* monoEdgeName(bool skip) {
    return skip ? "the strip only one eye can see is SKIPPED entirely "
                  "(advanced.foveation_monocular_edge = skip)"
                : "every part of each eye is drawn (advanced.foveation_monocular_edge = draw)";
}

void releaseMask(Mask& m) {
    if (m.tex) m.tex->Release();
    m = Mask();
}

void standDown(ID3D11DeviceContext* ctx, const char* why);

// Sets or clears the image on the context: both the per-viewport table and
// the view, every time -- the table is per-viewport state and the game
// sets its viewports between our calls.
void applyView(ID3D11DeviceContext* ctx, IUnknown* view) {
    if (!g_nv.ok || !ctx) return;
    fillRates(view != nullptr, innerRate(), midRate(), outerRate());
    NvStatus rc1 = kNvOk, rc2 = kNvOk;
    const bool survived = guardedBudget(g_budget, [&] {
        rc1 = g_nv.setViewportRates(ctx, &g_ratesDesc);
        rc2 = g_nv.setRateView(ctx, view);
    });
    if (!survived || rc1 != kNvOk || rc2 != kNvOk) {
        g_bound = nullptr;
        g_boundUnknown = false;
        g_ratesOn = false;
        char e[96], why[240];
        snprintf(why, sizeof(why), "%s answered %s while %s the image",
                 rc1 != kNvOk ? "NvAPI_D3D11_RSSetViewportsPixelShadingRates"
                              : "NvAPI_D3D11_RSSetShadingRateResourceView",
                 survived ? nvError(rc1 != kNvOk ? rc1 : rc2, e, sizeof(e)) : "a fault (caught)",
                 view ? "binding" : "clearing");
        standDown(ctx, why);
        return;
    }
    if (view && !g_boundOnce) {
        g_boundOnce = true;
        Log::get().note("foveation: the shading-rate image is bound for the first time -- the game "
                        "now shades the edges of each eye coarsely.");
    }
    g_bound = view;
    g_boundUnknown = false;
    g_ratesOn = view != nullptr;
    ++g_switches;
    ++g_switchesFrame;
}

void standDown(ID3D11DeviceContext* ctx, const char* why) {
    if (g_phase == Phase::Down) return;
    g_phase = Phase::Down;
    Log::get().note("foveation: OFF for this session -- %s. The game shades at full rate everywhere, "
                    "as with fix.foveation off.", why);
    if (g_nv.ok && ctx && (g_bound || g_boundUnknown)) {
        // Best effort, unbudgeted for the answer: a failure here has nothing
        // left to stand down.
        fillRates(false, kRate1x1, kRate2x2, outerRate());
        guardedBudget(g_budget, [&] {
            g_nv.setRateView(ctx, nullptr);
            g_nv.setViewportRates(ctx, &g_ratesDesc);
        });
    }
    g_bound = nullptr;
    g_boundUnknown = false;
    g_ratesOn = false;
}

// The eye's frustum in the RENDERED image: the true tangents the openvr
// half publishes (frame_flag.h), widened by the cull guard's lie when it is
// live, since the image is rendered to the lied frustum. Left and right
// from the outer/inner magnitudes: the eyes mirror. Top and bottom are
// magnitudes; y is up.
bool frustumOf(int eye, uint32_t w, uint32_t h, float* l, float* r, float* top, float* bot) {
    float outer = 0.0f, inner = 0.0f;
    if (!eyeTangents(&outer, &inner) || outer <= 0.0f || inner <= 0.0f) return false;
    float t = 0.0f, b = 0.0f;
    if (!eyeTangentsVertical(&t, &b) || t <= 0.0f || b <= 0.0f) {
        // An older openvr half: symmetric, from the horizontal span and
        // the texture's shape, the derivation frame_flag.h documents.
        t = b = 0.5f * (outer + inner) * static_cast<float>(h) / static_cast<float>(w ? w : 1);
    }
    const CullGuardState g = decodeCullGuardState(cullGuardStatePacked());
    if (g.stage == 2) {
        const float fh = 1.0f + static_cast<float>(g.hPerMille) / 1000.0f;
        const float fv = 1.0f + static_cast<float>(g.vPerMille) / 1000.0f;
        outer *= fh;
        inner *= fh;
        t *= fv;
        b *= fv;
    }
    *l = eye == 0 ? -outer : -inner;
    *r = eye == 0 ? inner : outer;
    *top = t;
    *bot = b;
    return true;
}

// The fixation point in tangent space: straight ahead, shifted toward the
// nose by the eye's offset over the fixation distance -- the runtime's
// eye-to-head translation when the temporal pass has noted it, a typical
// half interpupillary distance otherwise.
void centreOf(int eye, float* cx, float* cy) {
    *cx = 0.0f;
    *cy = 0.0f;
    // The nasal shift, and ONLY the nasal shift, is what the fixation
    // distance governs: each eye's line to a point that far ahead of the
    // head, so the two eyes' rings fuse at that depth rather than at
    // infinity. A distance of zero means "fuse at infinity", which is no
    // shift -- it does not mean "no centre". The early return that used to
    // stand here took the gaze with it, so setting the distance to zero
    // silently nailed the rings to straight ahead while the census still
    // reported the gaze it was no longer using (2026-09-06, caught by
    // Sean: the culled inner disc did not move with his eyes).
    if (g_distance > 0.0f) {
        float ox = eye == 0 ? -0.032f : 0.032f, oy = 0.0f;
        float off[3];
        if (temporalPassEyeOffset(eye, off) && std::isfinite(off[0]) && std::isfinite(off[1])) {
            ox = off[0];
            oy = off[1];
        }
        *cx = -ox / g_distance;
        *cy = -oy / g_distance;
    }
    // The eye-tracked centre, when the source publishes one: the gaze's
    // head-frame tangents, on top of whatever shift the distance asked for.
    if (g_followEyes && g_gazeValid && g_maskCentreValid) {
        *cx += g_maskTx;
        *cy += g_maskTy;
    }
}

// A tile's rate is the finest its NEAREST point to the centre needs: a tile
// the full-rate disc touches at all is full rate. Radii in tangent space
// about the centre, which is the angle for a centre near the axis.
bool fillMask(ID3D11DeviceContext* ctx, Mask& m) {
    float l, r, top, bot;
    if (!frustumOf(m.eye, m.w, m.h, &l, &r, &top, &bot)) return false;
    float cx, cy;
    centreOf(m.eye, &cx, &cy);
    g_builtTx = cx;
    g_builtTy = cy;
    // The geometry this mask is built on, said outright for the first few:
    // which eye, the frustum the tangents gave, where the rings' centre
    // landed in that frustum, and what that is as NDC. The left eye's
    // straight-ahead sits RIGHT of its image centre (+0.194 on the Crystal
    // Super) and the right eye's LEFT of it (-0.194); a mask whose centre
    // reads the other eye's sign is built on the wrong frustum, which is
    // what puts its rings on the wrong side of the frame.
    if (g_maskLines < 8) {
        ++g_maskLines;
        const float ndcx = (r > l) ? (2.0f * (cx - l) / (r - l) - 1.0f) : 0.0f;
        const float ndcy = (bot + top > 0.0f) ? (2.0f * (top - cy) / (top + bot) - 1.0f) : 0.0f;
        float outerT = 0.0f, innerT = 0.0f;
        eyeTangents(&outerT, &innerT);
        Log::get().note(
            "foveation: the %s eye's %ux%u mask is built on the frustum l %+.3f r %+.3f top %+.3f bottom "
            "%+.3f (the published tangents are outer %.3f, inner %.3f), with the rings centred at tangent "
            "(%+.3f, %+.3f) -- NDC (%+.3f, %+.3f), where the LEFT eye's straight ahead should read about "
            "+0.19 and the RIGHT eye's about -0.19. Full rate within %.3f of that, the ring to %.3f.",
            m.eye == 0 ? "LEFT" : "RIGHT", m.w, m.h, l, r, top, bot, outerT, innerT, cx, cy, ndcx, -ndcy,
            tanf(0.5f * g_innerDeg * 0.01745329252f), tanf(0.5f * g_outerDeg * 0.01745329252f));
    }
    const float deg2rad = 0.01745329252f;
    float inner = g_innerDeg, outer = g_outerDeg;
    if (outer > 178.0f) outer = 178.0f;
    if (inner > outer) inner = outer;
    const float ri = tanf(0.5f * inner * deg2rad), ro = tanf(0.5f * outer * deg2rad);
    const float ri2 = ri * ri, ro2 = ro * ro;
    const size_t need = static_cast<size_t>(m.tw) * m.th;
    if (need > g_scratchCap) {
        delete[] g_scratch;
        g_scratch = new uint8_t[need];
        g_scratchCap = need;
    }
    // The attribution test: this eye's every tile culled, the other eye's
    // built as usual, so the commander's own eyes name the mapping.
    if (g_cullEye == m.eye) {
        memset(g_scratch, 3, need);
        ctx->UpdateSubresource(m.tex, 0, nullptr, g_scratch, m.tw, 0);
        m.gen = g_settingsGen;
        return true;
    }
    // The monocular strip's boundary, in this eye's own tangents. The
    // other eye reaches to this eye's nasal tangent mirrored, so for the
    // left eye (l -outer, r +inner) everything below -r is territory the
    // right eye does not cover, and for the right eye (l -inner, r +outer)
    // everything above -l is. Symmetric frusta put the boundary on the
    // image edge, which culls nothing, which is correct.
    const bool  monoLeft = m.eye == 0;
    const float monoEdge = monoLeft ? -r : -l;
    const float fw = static_cast<float>(m.w), fh = static_cast<float>(m.h);
    for (uint32_t j = 0; j < m.th; ++j) {
        const float py0 = static_cast<float>(j * kTile);
        float py1 = static_cast<float>((j + 1) * kTile);
        if (py1 > fh) py1 = fh;
        const float ty1 = top - py0 / fh * (top + bot);   // the tile's upper edge, y up
        const float ty0 = top - py1 / fh * (top + bot);   // its lower edge
        for (uint32_t i = 0; i < m.tw; ++i) {
            const float px0 = static_cast<float>(i * kTile);
            float px1 = static_cast<float>((i + 1) * kTile);
            if (px1 > fw) px1 = fw;
            const float tx0 = l + px0 / fw * (r - l);
            const float tx1 = l + px1 / fw * (r - l);
            const float nx = cx < tx0 ? tx0 : (cx > tx1 ? tx1 : cx);
            const float ny = cy < ty0 ? ty0 : (cy > ty1 ? ty1 : cy);
            const float d2 = (nx - cx) * (nx - cx) + (ny - cy) * (ny - cy);
            uint8_t rate = d2 < ri2 ? 0 : (d2 < ro2 ? 1 : 2);
            // Whole tiles only: a tile straddling the boundary still holds
            // pixels the other eye can see, and half a culled tile at the
            // seam would be a hard edge in the overlap.
            if (g_monoEdge && (monoLeft ? (tx1 <= monoEdge) : (tx0 >= monoEdge))) rate = 3;
            g_scratch[static_cast<size_t>(j) * m.tw + i] = rate;
        }
    }
    ctx->UpdateSubresource(m.tex, 0, nullptr, g_scratch, m.tw, 0);
    m.gen = g_settingsGen;
    return true;
}

// The image for this size and eye, made on first sight, refilled when the
// settings changed since it was filled. Null when the tangents have not
// been published yet (nothing to centre on) or after a stand-down.
Mask* maskFor(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, int eye) {
    for (Mask& m : g_masks) {
        if (m.used && m.w == w && m.h == h && m.eye == eye) {
            m.lastFrame = g_frame;
            // Never hand out an image whose texels have never been written.
            // The slot is marked used before its first fill, and that fill
            // fails while the openvr half has published no tangents to
            // centre on -- so without this the second call for that size
            // would find the slot, skip the fill, and bind a texture with
            // undefined contents, whose texels the rate table reads as
            // rates (the pre-ship review of 2026-09-06). The boundary
            // refill fills it the moment the tangents arrive.
            if (m.gen == 0) {
                ++g_noTangentFrames;
                return nullptr;
            }
            // NOT refilled here. A mask rewritten in the middle of a frame
            // shades that frame's earlier draws at one set of rates and its
            // later ones at another, and with the centre following the eyes
            // a refill lands mid-frame whenever the gaze crosses the dead
            // band -- which is what left blocky work INSIDE the disc, in
            // whichever eye's passes the rewrite fell between (the flight
            // of 2026-09-06 09:36). The frame boundary refills instead, so
            // a frame is always shaded by one mask per eye, at most one
            // frame behind the gaze.
            return &m;
        }
    }
    Mask* slot = nullptr;
    for (Mask& m : g_masks) {
        if (!m.used) { slot = &m; break; }
    }
    if (!slot) {
        slot = &g_masks[0];
        for (Mask& m : g_masks) {
            if (m.lastFrame < slot->lastFrame) slot = &m;
        }
        releaseMask(*slot);
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (w + kTile - 1) / kTile;
    td.Height = (h + kTile - 1) / kTile;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* tex = nullptr;
    const HRESULT hr = dev->CreateTexture2D(&td, nullptr, &tex);
    if (FAILED(hr) || !tex) {
        dev->Release();
        char why[160];
        snprintf(why, sizeof(why), "the shading-rate image's texture (%ux%u R8_UINT) could not be "
                                   "created (hr 0x%08lX)", td.Width, td.Height, static_cast<unsigned long>(hr));
        standDown(ctx, why);
        return nullptr;
    }
    NvRateViewDesc vd = {};
    vd.version = kRateViewVer1;
    vd.format = DXGI_FORMAT_R8_UINT;
    vd.dimension = kDimTexture2D;
    vd.tex2d.mipSlice = 0;
    IUnknown* view = nullptr;
    NvStatus rc = -1;
    const bool survived = guardedBudget(g_budget, [&] { rc = g_nv.createRateView(dev, tex, &vd, &view); });
    dev->Release();
    if (!survived || rc != kNvOk || !view) {
        tex->Release();
        char e[96], why[200];
        snprintf(why, sizeof(why), "NvAPI_D3D11_CreateShadingRateResourceView answered %s",
                 survived ? nvError(rc, e, sizeof(e)) : "a fault (caught)");
        standDown(ctx, why);
        return nullptr;
    }
    slot->used = true;
    slot->tex = tex;
    slot->view = view;
    slot->w = w;
    slot->h = h;
    slot->tw = td.Width;
    slot->th = td.Height;
    slot->eye = eye;
    slot->gen = 0;
    slot->lastFrame = g_frame;
    ++g_imagesMade;
    ++g_masksMade;
    // ...and the slot keeps gen 0 until a fill succeeds, so the branch
    // above refuses it until then.
    if (g_imagesMade <= 8) {
        Log::get().note("foveation: image %u made for the %s eye at %ux%u (%u x %u tiles).", g_imagesMade,
                        eye == 0 ? "LEFT" : "RIGHT", w, h, td.Width, td.Height);
    }
    if (!fillMask(ctx, *slot)) return nullptr;  // kept; filled when the tangents arrive
    return slot;
}

// Which eye a target is: the texture the openvr half submitted for that eye
// when the target IS one, else the depth probe's rule -- of two targets
// alike in size and format, the first bound in the frame is the left eye's.
int eyeOf(const ResourceInfo& info) {
    for (uint32_t i = 0; i < g_seenCount; ++i) {
        if (g_seen[i].resource == info.resource) return g_seen[i].eye;
    }
    bool bySubmit = false;
    int eye = -1;
    for (int e = 0; e < 2; ++e) {
        void* sub = submittedTexture(e);
        if (sub && sub == info.resource) eye = e;
    }
    Known* k = knownFor(info.resource);
    if (k) {
        k->w = info.a;
        k->h = info.b;
        k->fmt = info.fmt;
    }
    // A target the runtime has EVER submitted keeps the eye it was
    // submitted as, on every frame after as well as the one it was seen
    // on. The final target is double-buffered, so only one of each eye's
    // two is the current submitted texture in any given frame, and on the
    // frames where it was not, the pairing below was free to reassign it
    // -- the flight of 2026-09-06 09:21 caught it doing exactly that, with
    // one target reading RIGHT while the same line said it had been
    // submitted as LEFT, which put the right eye's mask on the left eye's
    // final image. Ground truth outranks the pairing, always.
    if (eye < 0 && k && k->everSubmitted) eye = k->submittedAs;
    if (eye >= 0) {
        bySubmit = true;
        ++g_submitMatched;
        ++g_settledBySubmit;
        if (k) {
            if (k->settled && k->eye != eye) {
                ++g_corrections;
                ++g_settingsGen;   // its mask was built for the other eye
            }
            k->eye = eye;
            k->settled = true;
            k->everSubmitted = true;
            k->submittedAs = eye;
        }
        // Rooted for this frame too, so the pairing takes its phase from
        // this one and never writes over it.
    } else if (k && k->settled) {
        eye = k->eye;
        ++g_settledByOrder;
    } else {
        // Not yet placed: the frame boundary's walk settles it behind the
        // next submitted target. Until then the left eye, which is what
        // the old guess would have said for a target seen first.
        eye = k ? k->eye : 0;
        ++g_unsettled;
    }
    if (g_seenCount < sizeof(g_seen) / sizeof(g_seen[0])) {
        g_seen[g_seenCount++] = Seen{info.resource, info.a, info.b, info.fmt, eye, bySubmit};
    }
    ++g_targetsFrame[eye];
    return eye;
}

// The eyes, settled by PAIRING rather than by order.
//
// The census of 2026-09-06 09:14 showed the shape of the thing: every size
// and format the game draws into has its targets in matched pairs, one per
// eye, with draw counts within a fraction of a percent of each other --
// 187,012 against 179,201 for the scene's R10G10B10A2 pair, 13,038 against
// 12,677 for the R11G11B10 one, and four targets for the final R8G8B8A8,
// which is that pair double-buffered. The two submitted textures root one
// of those groups outright, and within a frame each group's members are
// bound in the same eye order as that rooted one. So: rank each size's
// targets by their first bind in the frame, take the phase from a rooted
// member where the group has one and from the frame's first rooted target
// otherwise, and the ranks alternate from there.
//
// This replaces the walk back from each submitted texture, which settled
// eight targets a frame and was wrong about all of them: it assumed each
// eye's targets form a contiguous block ending at that eye's submitted
// one, and Elite interleaves the two eyes' draws (eye_split.h) and binds
// its submitted textures nowhere near last.
void settleEyesByOrder() {
    // The frame's phase: the eye of the first target the submitted
    // textures identified, in bind order.
    int framePhase = -1;
    for (uint32_t i = 0; i < g_seenCount; ++i) {
        if (g_seen[i].bySubmit) {
            framePhase = g_seen[i].eye;
            break;
        }
    }
    if (framePhase < 0) return;   // nothing rooted this frame: leave it alone
    for (uint32_t i = 0; i < g_seenCount; ++i) {
        // This target's rank among its own size and format, in bind order,
        // and the phase its group carries if a submitted texture rooted one.
        uint32_t rank = 0;
        int groupPhase = -1;
        uint32_t groupRank = 0;
        for (uint32_t j = 0; j < g_seenCount; ++j) {
            if (g_seen[j].w != g_seen[i].w || g_seen[j].h != g_seen[i].h || g_seen[j].fmt != g_seen[i].fmt) continue;
            if (j < i) ++rank;
            if (groupPhase < 0 && g_seen[j].bySubmit) {
                groupPhase = g_seen[j].eye;
                groupRank = 0;
                for (uint32_t m = 0; m < j; ++m) {
                    if (g_seen[m].w == g_seen[j].w && g_seen[m].h == g_seen[j].h && g_seen[m].fmt == g_seen[j].fmt) ++groupRank;
                }
            }
        }
        const int phase = groupPhase >= 0 ? groupPhase : framePhase;
        const uint32_t base = groupPhase >= 0 ? groupRank : 0;
        const int eye = (((rank - base) & 1u) == 0) ? phase : 1 - phase;
        if (g_seen[i].bySubmit) continue;   // ground truth already
        Known* k = knownFor(g_seen[i].resource);
        if (!k) continue;
        if (k->everSubmitted) {
            // Belt and braces: never let the pairing move a target the
            // runtime itself has named.
            k->eye = k->submittedAs;
            k->settled = true;
            continue;
        }
        if (k->settled && k->eye == eye) continue;
        if (k->settled && k->eye != eye) {
            ++g_corrections;
            ++k->corrections;
        }
        k->eye = eye;
        k->settled = true;
        ++g_settingsGen;   // rebuild that size's masks around the right centre
    }
}

// The modules in the process whose names suggest they might touch the
// same state -- another injector, an overlay, a VR layer, NVIDIA's own --
// for the ARMED line. A census, not a verdict.
const char* suspectModules() {
    static char buf[600];
    Text t{buf, sizeof(buf)};
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return "(the module list could not be read)";
    const size_t n = needed / sizeof(HMODULE) < 512 ? needed / sizeof(HMODULE) : 512;
    const char* keys[] = {"nv", "pimax", "magic", "xr", "vr", "reshade", "overlay", "hook", "inject", "layer",
                          "toolkit", "steam", "discord", "rtss", "afterburner", "fps", "d3d", "dxgi"};
    for (size_t i = 0; i < n && !t.full(); ++i) {
        char name[MAX_PATH] = {};
        if (!GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, sizeof(name))) continue;
        char lower[MAX_PATH];
        size_t k = 0;
        for (; name[k] && k < sizeof(lower) - 1; ++k) lower[k] = static_cast<char>(tolower(static_cast<unsigned char>(name[k])));
        lower[k] = 0;
        bool hit = false;
        for (const char* key : keys) {
            if (strstr(lower, key)) { hit = true; break; }
        }
        if (!hit) continue;
        t.add("%s%s", t.len ? ", " : "", name);
    }
    return t.len ? buf : "(none of the suspect names)";
}

void arm(ID3D11DeviceContext* ctx) {
    if (GetModuleHandleW(L"LibMagicD3D1164.dll")) {
        standDown(ctx, "Pimax Play's own foveated rendering (LibMagicD3D1164.dll) is loaded in this "
                       "process and holds the shading-rate image; two holders cannot coexist. Turn "
                       "its foveated rendering off in Pimax Play to use this one");
        return;
    }
    if (!nvArm()) {
        standDown(ctx, g_nv.why);
        return;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    char sm[32] = "SM ?";
    const int sup = nvVrsSupported(dev, sm, sizeof(sm));
    if (dev) dev->Release();
    if (sup == 0) {
        standDown(ctx, "this GPU reports no variable-rate shading (it needs an NVIDIA RTX 20-series "
                       "/ GTX 16-series or newer)");
        return;
    }
    g_phase = Phase::Armed;
    Log::get().note(
        "foveation: ARMED (fix.foveation = %s) -- NvAPI is up and %s. Full-rate shading inside %.0f "
        "degrees about each eye's fixation point (%.2f m), one shade per 2x2 pixels out to %.0f "
        "degrees, one per %s beyond; %s; %s. The rate table reads %s inside, %s in the ring, %s beyond. The image "
        "binds at the first eye draw. Modules in the process with names worth knowing: %s.",
        modeName(g_mode),
        sup == 1 ? "the driver says this GPU shades at variable rate" : "the capability query was refused, so the view will decide",
        g_innerDeg, g_distance, g_outerDeg, outerRateName(),
        g_geometryOnly ? "geometry draws only, the full-screen passes at full rate (advanced.foveation_passes = geometry)"
                       : "every draw into the eye, the full-screen passes included (advanced.foveation_passes = all)",
        monoEdgeName(g_monoEdge), rateName(innerRate()), rateName(midRate()), rateName(outerRate()),
        suspectModules());
}

const char* centreText() {
    // 200 was too small for the built-on centre this line now carries, and
    // snprintf would have dropped exactly the field the fix added, at the
    // end -- the same way the census once dropped its own evidence.
    static char text[320];
    if (!g_followEyes) {
        snprintf(text, sizeof(text), "straight ahead (fix.foveation_centre = ahead)");
    } else if (g_gazeValid) {
        // The gaze AND the centre the masks were actually built on. They
        // differ whenever something between the two drops the gaze, which
        // is exactly the fault that hid behind this line reading only the
        // first of them.
        snprintf(text, sizeof(text),
                 "following the eyes -- %llu frames so far, %u refills for gaze motion past the "
                 "%.1f-degree dead band, %u frames the gaze was published lost (blinks included); "
                 "the gaze is at tangents (%+.3f, %+.3f) and the masks were last built on "
                 "(%+.3f, %+.3f)",
                 static_cast<unsigned long long>(g_gazeFramesFollowed), g_gazeRefills,
                 atanf(kGazeDeadBand) * 57.2957795f, g_gazeLosses, g_gazeTx, g_gazeTy, g_builtTx,
                 g_builtTy);
    } else {
        snprintf(text, sizeof(text),
                 "straight ahead -- no gaze published%s (%llu frames followed so far, %u refills)",
                 g_gazeStamp ? " right now" : " yet", static_cast<unsigned long long>(g_gazeFramesFollowed),
                 g_gazeRefills);
    }
    return text;
}

const char* gpuBusyText() {
    static char text[160];
    if (g_gpuBusyN == 0) {
        snprintf(text, sizeof(text), "no reading (%s)",
                 !g_nv.enumGpus || !g_nv.dynamicPstates ? "this NvAPI has no utilisation entry points"
                 : !g_nv.gpu                             ? "no physical GPU enumerated"
                 : g_gpuBusyAbsent                       ? "the graphics domain answered absent"
                                                         : "no sample yet");
        return text;
    }
    snprintf(text, sizeof(text),
             "%.0f%% on average, most %u%%, over %u one-second samples of the whole GPU (under 90 is "
             "a frame the GPU is not the limit of)",
             static_cast<double>(g_gpuBusySum) / g_gpuBusyN, g_gpuBusyMax, g_gpuBusyN);
    return text;
}

void summary(const char* when) {
    const double f = g_framesArmed ? static_cast<double>(g_framesArmed) : 1.0;
    // Split across several lines: one line carrying all of this was cut by
    // the log's own limit on 2026-09-06 09:05, and everything after the
    // target list -- the whole point of it -- was lost.
    Log::get().note(
        "foveation: %s -- %u frames with the image armed; %.1f eye-sized targets a frame attributed "
        "to the left eye and %.1f to the right; %.1f image switches a frame (most %u); %u images made; "
        "%u frames without published tangents; %.2f mask refills a frame, all at the frame boundary.",
        when, g_framesArmed, static_cast<double>(g_targetsSum[0]) / f,
        static_cast<double>(g_targetsSum[1]) / f, static_cast<double>(g_switches) / f, g_switchesMax,
        g_imagesMade, g_noTangentFrames, static_cast<double>(g_maskRefills) / f);
    Log::get().note(
        "foveation: %s, the draws -- %.0f a frame into eye-sized targets, %.0f of them under the image, "
        "%.0f left at full rate because the eye is not known, %.0f into everything else. Eyes settled: "
        "%.1f a frame by a submitted texture, %.1f by the pairing of their size's targets, %.1f not placed, "
        "%llu corrections, %u evictions.",
        when, static_cast<double>(g_eyeDraws) / f, static_cast<double>(g_eyeDrawsUnder) / f,
        static_cast<double>(g_unknownEyeDraws) / f, static_cast<double>(g_otherDraws) / f,
        static_cast<double>(g_settledBySubmit) / f, static_cast<double>(g_settledByOrder) / f,
        static_cast<double>(g_unsettled) / f, static_cast<unsigned long long>(g_corrections), g_knownEvictions);
    {
        // The targets by draws, largest first, at most eight.
        const Sig* order[16] = {};
        int n = 0;
        for (const Sig& s : g_sigs) {
            if (s.used) order[n++] = &s;
        }
        for (int i = 1; i < n; ++i) {
            const Sig* s = order[i];
            int j = i;
            while (j > 0 && order[j - 1]->under + order[j - 1]->bare + order[j - 1]->other < s->under + s->bare + s->other) {
                order[j] = order[j - 1];
                --j;
            }
            order[j] = s;
        }
        char by[700];
        int bl = 0;
        for (int i = 0; i < n && i < 8; ++i) {
            const Sig& s = *order[i];
            const char* name = fmtName(s.fmt);
            char fmtBuf[24];
            if (!name) {
                snprintf(fmtBuf, sizeof(fmtBuf), "fmt %u", s.fmt);
                name = fmtBuf;
            }
            const int wrote = snprintf(by + bl, sizeof(by) - bl, "%s%ux%u %s: %.0f under, %.0f bare, %.0f as not-an-eye",
                                       bl ? "; " : "", s.w, s.h, name, static_cast<double>(s.under) / f,
                                       static_cast<double>(s.bare) / f, static_cast<double>(s.other) / f);
            if (wrote < 0 || bl + wrote >= static_cast<int>(sizeof(by)) - 1) break;
            bl += wrote;
        }
        Log::get().note("foveation: %s, by target size -- %s.", when, bl ? by : "none seen");
    }
    Log::get().note("foveation: %s, the eye targets -- %s.", when, targetsText());
    Log::get().note("foveation: %s, the GPU busy since the last summary: %s. The centre: %s.", when,
                    gpuBusyText(), centreText());
    Log::get().note("foveation: %s, the state the eye draws run under -- %s.", when, stateText());
    g_gpuBusySum = 0;
    g_gpuBusyN = 0;
    g_gpuBusyMax = 0;
    g_gpuBusyAbsent = 0;
    g_settledBySubmit = 0;
    g_settledByOrder = 0;
    g_unsettled = 0;
    g_unknownEyeDraws = 0;
    g_maskRefills = 0;
}

}  // namespace

void foveationConfigure(Config& cfg) {
    const std::string mode = cfg.getString("fix.foveation", "off");
    Mode m = Mode::Off;
    bool unknown = false;
    if (mode == "quality") m = Mode::Quality;
    else if (mode == "balanced" || mode == "on" || mode == "1") m = Mode::Balanced;
    else if (mode == "performance") m = Mode::Performance;
    else if (mode != "off" && mode != "0" && !mode.empty()) unknown = true;

    float inner = 70.0f, outer = 100.0f;
    if (m == Mode::Balanced) { inner = 50.0f; outer = 84.0f; }
    if (m == Mode::Performance) { inner = 38.0f; outer = 70.0f; }
    const float innerKey = cfg.getFloat("advanced.foveation_inner", 0.0f);
    const float outerKey = cfg.getFloat("advanced.foveation_outer", 0.0f);
    if (std::isfinite(innerKey) && innerKey >= 10.0f) inner = innerKey > 170.0f ? 170.0f : innerKey;
    if (std::isfinite(outerKey) && outerKey >= 10.0f) outer = outerKey > 178.0f ? 178.0f : outerKey;
    if (outer < inner + 4.0f) outer = inner + 4.0f;
    const std::string centre = cfg.getString("fix.foveation_centre", "eyes");
    const bool follow = centre != "ahead";
    // Anything that is not "ahead" follows the eyes, which means a typo
    // gets the default silently. Sean's own ini was found carrying
    // "follows" on 2026-09-06 -- the second word of this key's settings
    // label, hand-copied -- and it had been working by accident for an
    // unknown number of flights. Say so once: a value this file does not
    // recognise should never pass without the log naming what it became.
    static bool centreWarned = false;
    if (!centreWarned && centre != "eyes" && centre != "ahead") {
        centreWarned = true;
        Log::get().note(
            "foveation: fix.foveation_centre = \"%s\" is not one of this key's values (eyes, ahead). "
            "Reading it as eyes, so the rings follow the gaze; write \"eyes\" to say so outright, or "
            "\"ahead\" for the fixed centre.",
            centre.c_str());
    }
    // THE DEFAULT DEPENDS ON WHO IS AIMING THE DISC.
    //
    // The shift aims each eye's disc at a point this far ahead, so the two
    // discs fuse at that depth and NOWHERE ELSE: at any other distance the
    // eyes' coarse/fine boundaries sit apart by the vergence angle, and
    // whatever falls between them is sharp in one eye and blocky in the
    // other. That is a rivalry the commander cannot look away from.
    //
    // It exists for the FIXED centre, where a disc pinned to each eye's
    // straight ahead otherwise fuses at infinity and reads as a window set
    // in space rather than glasses on the face. When the EYES aim the disc
    // there is nothing to correct: the gaze is already a direction in the
    // head frame and both eyes take the same one, so a shift can only pull
    // them apart. Twice now a flight has found the same symptom with the
    // fixed default under the eye-tracked centre -- "the left eye had VRS
    // shading in the middle" on 2026-09-06, at 0.086 tangent between the
    // two discs, which is 5 degrees of rivalry on menu text.
    //
    // So: no shift when the eyes are driving, the cockpit's depth when
    // they are not. Either can still be set outright.
    float dist = cfg.getFloat("advanced.foveation_distance", follow ? 0.0f : 0.7f);
    if (!std::isfinite(dist) || dist < 0.0f) dist = 0.0f;
    if (dist > 0.0f && dist < 0.2f) dist = 0.2f;
    const std::string passes = cfg.getString("advanced.foveation_passes", "all");
    const bool geom = passes == "geometry";
    const std::string monoKey = cfg.getString("advanced.foveation_monocular_edge", "draw");
    const bool mono = monoKey == "skip";
    const std::string outerRateKey = cfg.getString("advanced.foveation_outer_rate", "preset");
    uint32_t outerOverride = kRatePreset;
    bool cullInner = false, cullAll = false;
    if (outerRateKey == "cull") outerOverride = kRateCull;
    else if (outerRateKey == "2x2") outerOverride = kRate2x2;
    else if (outerRateKey == "4x4") outerOverride = kRate4x4;
    else if (outerRateKey == "cull_inner") cullInner = true;
    else if (outerRateKey == "cull_all") cullAll = true;
    int cullEye = -1;
    if (outerRateKey == "cull_left") cullEye = 0;
    else if (outerRateKey == "cull_right") cullEye = 1;

    const bool changed = m != g_mode || inner != g_innerDeg || outer != g_outerDeg ||
                         dist != g_distance || geom != g_geometryOnly || follow != g_followEyes ||
                         outerOverride != g_outerOverride || cullInner != g_cullInner || cullAll != g_cullAll ||
                         cullEye != g_cullEye || mono != g_monoEdge;
    const bool first = !g_configured;
    g_configured = true;
    g_mode = m;
    g_innerDeg = inner;
    g_outerDeg = outer;
    g_distance = dist;
    g_geometryOnly = geom;
    g_followEyes = follow;
    g_outerOverride = outerOverride;
    g_cullInner = cullInner;
    g_cullAll = cullAll;
    g_cullEye = cullEye;
    g_monoEdge = mono;
    if (changed) ++g_settingsGen;
    if ((first || changed) && cullEye >= 0) {
        Log::get().note(
            "foveation: advanced.foveation_outer_rate = %s -- every tile of the eye this module believes is the "
            "%s one is NOT DRAWN. THE TEST: shut one eye at a time. If the eye that goes black is the %s one, "
            "the module's eyes are the runtime's; if it is the other, they are swapped, which is what puts the "
            "rings on the wrong side of the frame.",
            outerRateKey.c_str(), cullEye == 0 ? "LEFT" : "RIGHT", cullEye == 0 ? "left" : "right");
    }
    if ((first || changed) && (outerOverride == kRateCull || cullInner || cullAll)) {
        Log::get().note(
            "foveation: advanced.foveation_outer_rate = %s -- %s NOT DRAWN. A diagnostic: what is culled goes "
            "BLACK, which proves by eye that the image reaches the game's pixels, and the frame time under it "
            "is the most any shading rate could save on this scene.",
            outerRateKey.c_str(),
            cullAll   ? "EVERY tile of each eye is"
            : cullInner ? "the full-rate disc itself, where you are looking, is"
                        : "the tiles beyond the outer ring are");
    }

    if (unknown && (first || changed)) {
        Log::get().note("foveation: fix.foveation = \"%s\" is not a choice here (off, quality, balanced, "
                        "performance); treated as off.", mode.c_str());
    }
    if (m == Mode::Off) {
        if (g_phase != Phase::Down) {
            if (g_phase == Phase::Armed) {
                Log::get().note("foveation: off (fix.foveation) -- the image is cleared at the next draw.");
            }
            g_phase = Phase::Off;
        }
        return;
    }
    if (g_phase == Phase::Off) {
        g_phase = Phase::Wanted;
        Log::get().note("foveation: ON (fix.foveation = %s): full rate inside %.0f degrees, 2x2 to %.0f, "
                        "%s beyond, fixation %.2f m, %s draws, %s. Arms at the first eye draw "
                        "(docs/performance.md, feature 2).",
                        modeName(m), inner, outer, outerRate() == kRate4x4 ? "4x4" : "2x2", dist,
                        geom ? "geometry" : "all", monoEdgeName(mono));
    } else if (changed && g_phase == Phase::Armed) {
        Log::get().note("foveation: settings changed (fix.foveation = %s, %.0f/%.0f degrees, %.2f m, %s "
                        "draws, %s; the rate table now reads %s inside, %s in the ring, %s beyond) -- the images "
                        "refill at their next use.",
                        modeName(m), inner, outer, dist, geom ? "geometry" : "all", monoEdgeName(mono),
                        rateName(innerRate()), rateName(midRate()), rateName(outerRate()));
    }
}

bool foveationWantsDraws() {
    return g_phase == Phase::Wanted || g_phase == Phase::Armed || g_bound != nullptr || g_boundUnknown;
}

void foveationOnDraw(ID3D11DeviceContext* ctx, bool rtvEyeSized, void* rtv, uint32_t rtvGen,
                     char /*kind*/, uint32_t count, uint32_t instances) {
    if (!ctx) return;
    if (g_phase == Phase::Off || g_phase == Phase::Down) {
        if (g_bound || g_boundUnknown) {
            if (g_phase == Phase::Off && g_nv.ok) applyView(ctx, nullptr);
            g_bound = nullptr;
            g_boundUnknown = false;
        }
        return;
    }
    // The census's resolve, once per rebind, for every target.
    if (g_phase == Phase::Armed && rtvGen != g_censusGen) {
        g_censusGen = rtvGen;
        g_censusSig = nullptr;
        g_censusKnown = nullptr;
        ResourceInfo info;
        if (rtv && bindingResolve(rtv, &info) && info.isTexture2D) {
            g_censusSig = sigFor(info);
            if (rtvEyeSized) {
                describeTarget(rtv, g_censusSig);
                g_censusKnown = knownFor(info.resource);
            }
        }
    }
    const bool fullScreenPass = count <= 6 && instances <= 1;
    if (!rtvEyeSized) {
        if (g_phase == Phase::Armed) {
            ++g_otherDraws;
            if (g_censusSig) ++g_censusSig->other;
        }
        if (g_bound || g_boundUnknown) applyView(ctx, nullptr);
        return;
    }
    if (g_geometryOnly && fullScreenPass) {
        if (g_phase == Phase::Armed) {
            ++g_eyeDraws;
            if (g_censusSig) ++g_censusSig->bare;
        }
        if (g_bound || g_boundUnknown) applyView(ctx, nullptr);
        return;
    }
    if (g_phase == Phase::Wanted) {
        arm(ctx);
        if (g_phase != Phase::Armed) return;
    }
    if (g_forcedSamplesSeen) {
        char why[200];
        snprintf(why, sizeof(why),
                 "an eye draw ran under a rasteriser state with a forced sample count of %u, and that "
                 "combination removes the device outright",
                 g_forcedSamplesSeen);
        standDown(ctx, why);
        return;
    }
    if (rtvGen != g_lastRtvGen) {
        g_lastRtvGen = rtvGen;
        g_lastMask = nullptr;
        g_lastEyeUnknown = false;
        ResourceInfo info;
        if (bindingResolve(rtv, &info) && info.isTexture2D && info.a >= kTile && info.b >= kTile) {
            const int eye = eyeOf(info);
            // ONLY where the eye is known. The two eyes' frusta are
            // mirrored, so a target wearing the other eye's mask has its
            // rings a fifth of the image out, and that is rivalry rather
            // than blur -- the flight of 2026-09-06 09:05 measured ten
            // eye-sized targets a frame with only the two submitted ones
            // identified, so eight of them were wearing the left eye's
            // mask by default and the right eye saw the left eye's
            // pattern. Elite interleaves the two eyes' draws
            // (eye_split.h), so no ordering rule can place the rest; an
            // unplaced target is left at full rate, which costs coverage
            // and cannot be seen.
            const Known* k = knownFor(info.resource);
            if (k && k->settled) {
                g_lastMask = maskFor(ctx, info.a, info.b, eye);
                if (!g_lastMask && g_phase == Phase::Armed) ++g_noTangentFrames;
            } else {
                g_lastEyeUnknown = true;
            }
        }
    }
    IUnknown* want = g_lastMask ? g_lastMask->view : nullptr;
    // Applied again after EVERY rebind of the target (the generation moves
    // at each OMSetRenderTargets), not only when the wanted view changes:
    // the working implementations set the image after every bind, and a
    // driver that drops it on a rebind would otherwise leave this module
    // believing it bound while the game shaded at full rate.
    if (want != g_bound || g_boundUnknown || (want && rtvGen != g_appliedGen)) {
        applyView(ctx, want);
        g_appliedGen = rtvGen;
    }
    if (g_phase == Phase::Armed) {
        ++g_eyeDraws;
        if (g_lastEyeUnknown) ++g_unknownEyeDraws;
        if (want) {
            ++g_eyeDrawsUnder;
            if (g_censusSig) ++g_censusSig->under;
        } else if (g_censusSig) {
            ++g_censusSig->bare;
        }
        if (g_censusKnown) ++g_censusKnown->draws;
        if (g_eyeDraws % 61 == 0) sampleDrawState(ctx);
    }
}

void foveationFrameBoundary(ID3D11DeviceContext* ctx) {
    ++g_frame;
    if (g_phase == Phase::Armed) {
        // Unbound between frames: the draws between the last eye draw and
        // the next frame's first are not all seen here (indirect draws,
        // replayed command lists), and an image of the wrong size under
        // them is not a state to leave lying about.
        if (g_bound || g_boundUnknown) applyView(ctx, nullptr);
        ++g_framesArmed;
        if (g_framesArmed % 90 == 0) gpuBusySample();
        // Every mask the settings or the gaze have moved on from, rewritten
        // here where no draw of this frame or the next has begun.
        for (Mask& m : g_masks) {
            if (m.used && m.gen != g_settingsGen) {
                if (fillMask(ctx, m)) ++g_maskRefills;
            }
        }
        if (g_followEyes) {
            float tx = 0.0f, ty = 0.0f;
            uint32_t stamp = 0;
            const bool present = gazeCentre(&tx, &ty, &stamp);
            if (stamp != g_gazeStamp) {
                g_gazeStamp = stamp;
                if (present) {
                    g_gazeTx = tx;
                    g_gazeTy = ty;
                    g_gazeAge = 0;
                    if (!g_gazeValid) {
                        g_gazeValid = true;
                        ++g_settingsGen;
                        g_gazeNotedLost = false;
                        if (!g_gazeNotedOn) {
                            g_gazeNotedOn = true;
                            Log::get().note("foveation: the centre follows the eyes from here -- the gaze "
                                            "source is publishing (fix.foveation_centre = eyes).");
                        }
                    }
                } else {
                    // A gaze published as LOST is usually a blink: the read
                    // declines, or its length leaves the band, for a few
                    // frames. Falling back on the first of them snapped the
                    // rings to straight ahead and back, with two refills,
                    // every time the commander blinked (the pre-ship review
                    // of 2026-09-06), so a loss now has to persist as long
                    // as a silence does before the centre moves.
                    ++g_gazeAge;
                    ++g_gazeLosses;
                }
            } else {
                ++g_gazeAge;
            }
            if (g_gazeValid && g_gazeAge > 90) {
                g_gazeValid = false;
                g_maskCentreValid = false;
                ++g_settingsGen;
                if (!g_gazeNotedLost) {
                    g_gazeNotedLost = true;
                    Log::get().note("foveation: the gaze is lost or silent -- the centre is straight ahead "
                                    "until it returns. Said once per loss.");
                }
            }
            if (g_gazeValid) {
                ++g_gazeFramesFollowed;
                const float dx = g_gazeTx - g_maskTx, dy = g_gazeTy - g_maskTy;
                if (!g_maskCentreValid || dx * dx + dy * dy > kGazeDeadBand * kGazeDeadBand) {
                    g_maskTx = g_gazeTx;
                    g_maskTy = g_gazeTy;
                    g_maskCentreValid = true;
                    ++g_settingsGen;
                    ++g_gazeRefills;
                }
            }
        } else if (g_gazeValid) {
            g_gazeValid = false;
            g_maskCentreValid = false;
            ++g_settingsGen;
        }
        if (g_switchesFrame > g_switchesMax) g_switchesMax = g_switchesFrame;
        g_targetsSum[0] += g_targetsFrame[0];
        g_targetsSum[1] += g_targetsFrame[1];
        // The first summary early, then one every 1800 frames (twenty
        // seconds at 90 Hz): the loading screen's targets are not the
        // scene's, and the scene changes.
        if (g_framesArmed == 600) {
            g_summaryDone = true;
            summary("after 600 frames");
        } else if (g_framesArmed % 1800 == 0) {
            char when[48];
            snprintf(when, sizeof(when), "after %u frames", g_framesArmed);
            summary(when);
        }
        if (!g_noTangentNoted && g_noTangentFrames >= 600) {
            g_noTangentNoted = true;
            Log::get().note("foveation: %u frames and the openvr half has published no eye tangents to "
                            "centre the image on (no openvr_api.dll installed, or an older one). The "
                            "image stays unbound until they arrive.", g_noTangentFrames);
        }
    }
    settleEyesByOrder();
    // Wanted, and nothing has ever arrived to arm on: no eye-sized target,
    // which on a headset whose eye textures are small enough to fall to the
    // size guess means the openvr half never published one. Said once,
    // because a feature doing nothing in silence is the worst failure this
    // codebase has (the pre-ship review of 2026-09-06 found this path had
    // no line at all).
    //
    // Frames AND seconds, because frames alone cried wolf: Elite's loading
    // screen runs uncapped at about 200 a second, so 1800 of them passed
    // nine seconds in and this line printed one second before the feature
    // armed perfectly well (the flight of 2026-09-06 11:14). A count is not
    // a clock on a screen with nothing to draw.
    if (g_phase == Phase::Wanted && !g_wantedNoted) {
        ++g_wantedFrames;
        if (g_wantedSince == 0) g_wantedSince = GetTickCount64();
    }
    if (g_phase == Phase::Wanted && !g_wantedNoted && g_wantedFrames >= 1800 && g_wantedSince != 0 &&
        GetTickCount64() - g_wantedSince >= 60000) {
        g_wantedNoted = true;
        Log::get().note(
            "foveation: ON, but a minute of frames has passed without a single eye-sized target to arm on. "
            "Either openvr_api.dll is not installed beside the game (or is an older one, which cannot "
            "tell this half the eye size), or this headset's eye textures are not being recognised. "
            "Nothing is being shaded coarsely. Said once.");
    }
    g_switchesFrame = 0;
    g_seenCount = 0;
    g_targetsFrame[0] = g_targetsFrame[1] = 0;
    g_lastRtvGen = ~0u;
    g_lastMask = nullptr;
    g_censusGen = ~0u;
    g_censusSig = nullptr;
    g_lastEyeUnknown = false;
    // A size the game stopped rendering at (a resolution change) ages out.
    for (Mask& m : g_masks) {
        if (m.used && g_frame - m.lastFrame > 900) releaseMask(m);
    }
}

void foveationOnClearState() {
    if (g_bound) g_boundUnknown = true;
}

void foveationShutdown() {
    if (g_phase == Phase::Armed && g_framesArmed) summary("at exit");
    // No NvAPI calls here: the views are left to the process (see Mask) and
    // NvAPI_Unload is never called, on OpenXR Toolkit's experience of both.
    for (Mask& m : g_masks) releaseMask(m);
    delete[] g_scratch;
    g_scratch = nullptr;
    g_scratchCap = 0;
    g_bound = nullptr;
    g_boundUnknown = false;
    g_ratesOn = false;
}

}  // namespace edvr

// ------------------------------------------------------------ the probe
//
// What the desk rounds of 2026-09-05 settled, so the two variants below
// are the ones that matter: every NvAPI call answers OK whatever the
// binding order, flags or view dimension; the rate values and the view
// dimension are what nvapi.h says; and the driver never reads the INITIAL
// DATA of a shading-rate texture -- an image filled at creation reads as
// zeros and every tile takes texel 0's rate, while the same bytes written
// by UpdateSubresource or CopyResource are read tile for tile. The module
// fills its images with UpdateSubresource, which is why it is the first
// variant here and the pass condition. (NvAPI's VRS helper, which builds
// and binds a pattern itself, also shaded coarsely on this GPU; it is not
// used and not probed -- the run that examined it did not come back.)
namespace {

struct Report {
    char*    buf;
    unsigned cap;
    unsigned pos = 0;
    void line(const char* fmt, ...) {
        if (!buf || pos + 2 >= cap) return;
        va_list ap;
        va_start(ap, fmt);
        const int n = vsnprintf(buf + pos, cap - pos - 1, fmt, ap);
        va_end(ap);
        if (n > 0) pos += static_cast<unsigned>(n) < cap - pos - 1 ? static_cast<unsigned>(n) : cap - pos - 1;
        if (pos < cap - 1) buf[pos++] = '\n';
        buf[pos] = 0;
    }
};

// The full-screen triangle, wound clockwise in y-up clip space (bottom-left,
// top-left, bottom-right) so a back-face cull keeps it; the first cut of this
// probe wound it the other way and measured the clear colour as 16x16 blocks
// in every band.
const char kProbeVs[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p = float2(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0);\n"
    "    return float4(p, 0.5, 1.0);\n"
    "}\n";
// The pixel shader writes its own position AND counts its invocations per
// band of four tile rows into a raw buffer. The count is the detector that
// does not depend on how SV_Position behaves under coarse shading: a 2x2
// band runs the shader a quarter as often, whatever position it reports.
// (Measured: the position IS the coarse pixel's, so the two agree.)
const char kProbePs[] =
    "RWByteAddressBuffer counts : register(u1);\n"
    "float2 main(float4 pos : SV_Position) : SV_Target {\n"
    "    uint band = min(uint(pos.y) / 64u, 7u);\n"
    "    counts.InterlockedAdd(band * 4u, 1u);\n"
    "    return pos.xy;\n"
    "}\n";

constexpr uint32_t kPw = 512, kPh = 512, kPtiles = kPw / edvr::kTile, kBands = 8;
const uint32_t kCodes[kBands]   = {edvr::kRate1x1, edvr::kRate2x1, edvr::kRate1x2, edvr::kRate2x2,
                                   edvr::kRate4x2, edvr::kRate2x4, edvr::kRate4x4, edvr::kRate1x1};
const char*    kNames[kBands]   = {"1x1", "2x1", "1x2", "2x2", "4x2", "2x4", "4x4", "1x1"};
const uint32_t kExpectW[kBands] = {1, 2, 1, 2, 4, 2, 4, 1};
const uint32_t kExpectH[kBands] = {1, 1, 2, 2, 2, 4, 4, 1};

// The same pixel shader, writing depth as well: a pixel shader that
// outputs SV_Depth is one of the things a driver may hold at full rate.
const char kProbePsDepth[] =
    "RWByteAddressBuffer counts : register(u1);\n"
    "float2 main(float4 pos : SV_Position, out float depth : SV_Depth) : SV_Target {\n"
    "    uint band = min(uint(pos.y) / 64u, 7u);\n"
    "    counts.InterlockedAdd(band * 4u, 1u);\n"
    "    depth = 0.5;\n"
    "    return pos.xy;\n"
    "}\n";

// The vertex shader with a varying, for the pixel shaders that read one at
// sample frequency or beside SV_Coverage -- the two shader features a
// driver holds at full rate by the D3D12 rule, if it applies them here.
const char kProbeVsUv[] =
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V main(uint id : SV_VertexID) {\n"
    "    V v;\n"
    "    float2 p = float2(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0);\n"
    "    v.pos = float4(p, 0.5, 1.0);\n"
    "    v.uv = p * 0.5 + 0.5;\n"
    "    return v;\n"
    "}\n";
const char kProbePsSample[] =
    "RWByteAddressBuffer counts : register(u1);\n"
    "float2 main(float4 pos : SV_Position, sample float2 uv : TEXCOORD0) : SV_Target {\n"
    "    uint band = min(uint(pos.y) / 64u, 7u);\n"
    "    counts.InterlockedAdd(band * 4u, 1u);\n"
    "    return pos.xy + uv * 0.0;\n"
    "}\n";
const char kProbePsAlpha[] =
    "RWByteAddressBuffer counts : register(u1);\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "    uint band = min(uint(pos.y) / 64u, 7u);\n"
    "    counts.InterlockedAdd(band * 4u, 1u);\n"
    "    return float4(pos.xy, 0.0, 1.0);\n"
    "}\n";
const char kProbePsMrt[] =
    "RWByteAddressBuffer counts : register(u3);\n"
    "struct O { float2 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; };\n"
    "O main(float4 pos : SV_Position) {\n"
    "    uint band = min(uint(pos.y) / 64u, 7u);\n"
    "    counts.InterlockedAdd(band * 4u, 1u);\n"
    "    O o;\n"
    "    o.a = pos.xy;\n"
    "    o.b = float4(0.25, 0.5, 0.75, 1.0);\n"
    "    o.c = float4(1.0, 0.75, 0.5, 0.25);\n"
    "    return o;\n"
    "}\n";
const char kProbePsCoverage[] =
    "RWByteAddressBuffer counts : register(u1);\n"
    "float2 main(float4 pos : SV_Position, float2 uv : TEXCOORD0, uint cov : SV_Coverage) : SV_Target {\n"
    "    uint band = min(uint(pos.y) / 64u, 7u);\n"
    "    counts.InterlockedAdd(band * 4u, 1u);\n"
    "    return pos.xy + uv * 0.0 + float(cov & 0u);\n"
    "}\n";

// A candidate state for the draw, one per variant: what the game might
// set that the plain probe never did.
struct Candidate {
    const char* name;
    bool        arrayTarget;    // the target an array of two, the view over slice 0 as TEXTURE2DARRAY
    bool        arrayImage;     // the image an array of one, the view TEXTURE2DARRAY
    bool        depth;          // a depth buffer bound, depth test on
    bool        multisample;    // rasteriser MultisampleEnable
    bool        aaLines;        // rasteriser AntialiasedLineEnable
    bool        scissor;        // rasteriser ScissorEnable with a full-target rect
    uint32_t    forcedSamples;  // rasteriser ForcedSampleCount (11.1) -- REMOVED the device on the desk; not run
    uint32_t    psKind;         // 0 plain, 1 writes SV_Depth, 2 reads a varying at sample frequency, 3 reads SV_Coverage, 4 writes alpha 1 under alpha-to-coverage, 5 three targets
    uint32_t    commandList;    // 0 no; 1 the draw replayed from a command list with the image set on the immediate context; 2 with it set on the deferred context before recording
    // The target's own properties: a size of its own (0 = the 512x512),
    // the texture and view formats (0 = R32G32_FLOAT, whose positions can
    // be read back; a typed view over a typeless texture counts only), the
    // image sized by the floor of the tile quotient rather than its
    // ceiling, three targets bound at once, a clear after the bind.
    uint32_t    targetW, targetH;
    uint32_t    texFormat, viewFormat;
    bool        floorTiles;
    bool        mrt;
    bool        clearAfterBind;
    uint32_t    drawKind;       // 0 Draw; 1 DrawIndexed; 2 DrawIndexedInstanced x2; 3 DrawInstanced x2
    bool        slice1;         // the array target's view over slice 1 rather than 0
    bool        imageSlices2;   // the array image of two slices (the pattern in both)
};

// A target of a candidate's own: texture, view, staging copy.
struct TargetSet {
    bool                    used = false;
    uint32_t                w = 0, h = 0, texFormat = 0, viewFormat = 0;
    bool                    floatFormat = true;
    ID3D11Texture2D*        tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D*        staging = nullptr;
    void release() {
        if (rtv) rtv->Release();
        if (tex) tex->Release();
        if (staging) staging->Release();
        *this = TargetSet();
    }
};

struct ProbeRig {
    ID3D11Device*              dev = nullptr;
    ID3D11DeviceContext*       ctx = nullptr;
    ID3D11Texture2D*           target = nullptr;
    ID3D11Texture2D*           staging = nullptr;
    ID3D11Texture2D*           image = nullptr;
    ID3D11RenderTargetView*    rtv = nullptr;
    IUnknown*                  view = nullptr;   // left to the process, as the module's own are
    ID3D11Buffer*              counts = nullptr;
    ID3D11Buffer*              countsStaging = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    ID3D11VertexShader*        vs = nullptr;
    ID3D11VertexShader*        vsUv = nullptr;
    ID3D11PixelShader*         ps = nullptr;
    ID3D11PixelShader*         psDepth = nullptr;
    ID3D11PixelShader*         psSample = nullptr;
    ID3D11PixelShader*         psCoverage = nullptr;
    ID3D11PixelShader*         psAlpha = nullptr;
    ID3D11PixelShader*         psMrt = nullptr;
    ID3D11BlendState*          blendA2c = nullptr;
    ID3D11RasterizerState*     rs = nullptr;
    TargetSet                  custom[4];
    ID3D11Texture2D*           mrtTex[2] = {};
    ID3D11RenderTargetView*    mrtRtv[2] = {};
    // The candidates' resources, made on demand.
    ID3D11Texture2D*           targetArr = nullptr;
    ID3D11RenderTargetView*    rtvArr = nullptr;
    ID3D11RenderTargetView*    rtvArr1 = nullptr;   // slice 1
    ID3D11Buffer*              indices = nullptr;
    ID3D11Texture2D*           depthTex = nullptr;
    ID3D11DepthStencilView*    dsv = nullptr;
    ID3D11DepthStencilState*   dss = nullptr;
    ID3D11RasterizerState*     rsCandidate = nullptr;
    ~ProbeRig() {
        if (rtv) rtv->Release();
        if (target) target->Release();
        if (staging) staging->Release();
        if (image) image->Release();
        if (counts) counts->Release();
        if (countsStaging) countsStaging->Release();
        if (uav) uav->Release();
        if (vs) vs->Release();
        if (vsUv) vsUv->Release();
        if (ps) ps->Release();
        if (psDepth) psDepth->Release();
        if (psSample) psSample->Release();
        if (psCoverage) psCoverage->Release();
        if (psAlpha) psAlpha->Release();
        if (psMrt) psMrt->Release();
        if (blendA2c) blendA2c->Release();
        if (rs) rs->Release();
        for (TargetSet& t : custom) t.release();
        for (int i = 0; i < 2; ++i) {
            if (mrtRtv[i]) mrtRtv[i]->Release();
            if (mrtTex[i]) mrtTex[i]->Release();
        }
        if (targetArr) targetArr->Release();
        if (rtvArr) rtvArr->Release();
        if (rtvArr1) rtvArr1->Release();
        if (indices) indices->Release();
        if (depthTex) depthTex->Release();
        if (dsv) dsv->Release();
        if (dss) dss->Release();
        if (rsCandidate) rsCandidate->Release();
    }
    bool makeCandidates(char* err, size_t errCap) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = kPw;
        td.Height = kPh;
        td.MipLevels = 1;
        td.ArraySize = 2;
        td.Format = DXGI_FORMAT_R32G32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &targetArr))) {
            snprintf(err, errCap, "the array target could not be created");
            return false;
        }
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = DXGI_FORMAT_R32G32_FLOAT;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rd.Texture2DArray.FirstArraySlice = 0;
        rd.Texture2DArray.ArraySize = 1;
        if (FAILED(dev->CreateRenderTargetView(targetArr, &rd, &rtvArr))) {
            snprintf(err, errCap, "the array target's view could not be created");
            return false;
        }
        rd.Texture2DArray.FirstArraySlice = 1;
        if (FAILED(dev->CreateRenderTargetView(targetArr, &rd, &rtvArr1))) {
            snprintf(err, errCap, "the array target's slice-1 view could not be created");
            return false;
        }
        const uint32_t idx[3] = {0, 1, 2};
        D3D11_BUFFER_DESC ib = {};
        ib.ByteWidth = sizeof(idx);
        ib.Usage = D3D11_USAGE_IMMUTABLE;
        ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA idata = {idx, 0, 0};
        if (FAILED(dev->CreateBuffer(&ib, &idata, &indices))) {
            snprintf(err, errCap, "the index buffer could not be created");
            return false;
        }
        D3D11_TEXTURE2D_DESC dd = {};
        dd.Width = kPw;
        dd.Height = kPh;
        dd.MipLevels = 1;
        dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.SampleDesc.Count = 1;
        dd.Usage = D3D11_USAGE_DEFAULT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(dev->CreateTexture2D(&dd, nullptr, &depthTex)) || FAILED(dev->CreateDepthStencilView(depthTex, nullptr, &dsv))) {
            snprintf(err, errCap, "the depth buffer could not be created");
            return false;
        }
        D3D11_DEPTH_STENCIL_DESC sd = {};
        sd.DepthEnable = TRUE;
        sd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        sd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (FAILED(dev->CreateDepthStencilState(&sd, &dss))) {
            snprintf(err, errCap, "the depth-stencil state could not be created");
            return false;
        }
        psDepth = edvr::shaderSwapCompilePs(ctx, kProbePsDepth, sizeof(kProbePsDepth) - 1, "main",
                                            "foveation_probe_ps_depth", nullptr, "foveation probe");
        vsUv = edvr::shaderSwapCompileVs(ctx, kProbeVsUv, sizeof(kProbeVsUv) - 1, "main", "foveation_probe_vs_uv",
                                         nullptr, "foveation probe");
        psSample = edvr::shaderSwapCompilePs(ctx, kProbePsSample, sizeof(kProbePsSample) - 1, "main",
                                             "foveation_probe_ps_sample", nullptr, "foveation probe");
        psCoverage = edvr::shaderSwapCompilePs(ctx, kProbePsCoverage, sizeof(kProbePsCoverage) - 1, "main",
                                               "foveation_probe_ps_coverage", nullptr, "foveation probe");
        psAlpha = edvr::shaderSwapCompilePs(ctx, kProbePsAlpha, sizeof(kProbePsAlpha) - 1, "main",
                                            "foveation_probe_ps_alpha", nullptr, "foveation probe");
        psMrt = edvr::shaderSwapCompilePs(ctx, kProbePsMrt, sizeof(kProbePsMrt) - 1, "main",
                                          "foveation_probe_ps_mrt", nullptr, "foveation probe");
        for (int i = 0; i < 2; ++i) {
            D3D11_TEXTURE2D_DESC md = {};
            md.Width = kPw;
            md.Height = kPh;
            md.MipLevels = 1;
            md.ArraySize = 1;
            md.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            md.SampleDesc.Count = 1;
            md.Usage = D3D11_USAGE_DEFAULT;
            md.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(dev->CreateTexture2D(&md, nullptr, &mrtTex[i])) || FAILED(dev->CreateRenderTargetView(mrtTex[i], nullptr, &mrtRtv[i]))) {
                snprintf(err, errCap, "the extra targets could not be created");
                return false;
            }
        }
        if (!psDepth || !vsUv || !psSample || !psCoverage || !psAlpha || !psMrt) {
            snprintf(err, errCap, "a candidate shader did not compile (depth %d, uv vs %d, sample %d, coverage %d, alpha %d)",
                     psDepth ? 1 : 0, vsUv ? 1 : 0, psSample ? 1 : 0, psCoverage ? 1 : 0, psAlpha ? 1 : 0);
            return false;
        }
        D3D11_BLEND_DESC bd = {};
        bd.AlphaToCoverageEnable = TRUE;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(dev->CreateBlendState(&bd, &blendA2c)) || !blendA2c) {
            snprintf(err, errCap, "the alpha-to-coverage blend state could not be created");
            return false;
        }
        return true;
    }
    // A target of the candidate's own size and formats, made once per shape.
    TargetSet* targetFor(const Candidate& c, char* err, size_t errCap) {
        const uint32_t texFmt = c.texFormat ? c.texFormat : DXGI_FORMAT_R32G32_FLOAT;
        const uint32_t viewFmt = c.viewFormat ? c.viewFormat : texFmt;
        for (TargetSet& t : custom) {
            if (t.used && t.w == c.targetW && t.h == c.targetH && t.texFormat == texFmt && t.viewFormat == viewFmt) return &t;
        }
        TargetSet* slot = nullptr;
        for (TargetSet& t : custom) {
            if (!t.used) { slot = &t; break; }
        }
        if (!slot) {
            custom[0].release();
            slot = &custom[0];
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = c.targetW;
        td.Height = c.targetH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = static_cast<DXGI_FORMAT>(texFmt);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &slot->tex))) {
            snprintf(err, errCap, "a %ux%u target of format %u could not be created", c.targetW, c.targetH, texFmt);
            return nullptr;
        }
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = static_cast<DXGI_FORMAT>(viewFmt);
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateRenderTargetView(slot->tex, &rd, &slot->rtv))) {
            snprintf(err, errCap, "the %ux%u target's view (format %u) could not be created", c.targetW, c.targetH, viewFmt);
            slot->release();
            return nullptr;
        }
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &slot->staging))) {
            snprintf(err, errCap, "the %ux%u target's staging copy could not be created", c.targetW, c.targetH);
            slot->release();
            return nullptr;
        }
        slot->used = true;
        slot->w = c.targetW;
        slot->h = c.targetH;
        slot->texFormat = texFmt;
        slot->viewFormat = viewFmt;
        slot->floatFormat = texFmt == DXGI_FORMAT_R32G32_FLOAT;
        return slot;
    }
    // The rasteriser state a candidate asks for, replacing the last one.
    bool makeRasteriser(const Candidate& c, char* err, size_t errCap) {
        if (rsCandidate) { rsCandidate->Release(); rsCandidate = nullptr; }
        if (c.forcedSamples) {
            ID3D11Device1* dev1 = nullptr;
            dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1));
            if (!dev1) {
                snprintf(err, errCap, "no ID3D11Device1 for a forced sample count");
                return false;
            }
            D3D11_RASTERIZER_DESC1 rd = {};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            rd.ScissorEnable = c.scissor ? TRUE : FALSE;
            rd.MultisampleEnable = c.multisample ? TRUE : FALSE;
            rd.AntialiasedLineEnable = c.aaLines ? TRUE : FALSE;
            rd.ForcedSampleCount = c.forcedSamples;
            const HRESULT hr = dev1->CreateRasterizerState1(&rd, reinterpret_cast<ID3D11RasterizerState1**>(&rsCandidate));
            dev1->Release();
            if (FAILED(hr) || !rsCandidate) {
                snprintf(err, errCap, "the forced-sample rasteriser state could not be created (hr 0x%08lX)", static_cast<unsigned long>(hr));
                return false;
            }
            return true;
        }
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        rd.ScissorEnable = c.scissor ? TRUE : FALSE;
        rd.MultisampleEnable = c.multisample ? TRUE : FALSE;
        rd.AntialiasedLineEnable = c.aaLines ? TRUE : FALSE;
        if (FAILED(dev->CreateRasterizerState(&rd, &rsCandidate)) || !rsCandidate) {
            snprintf(err, errCap, "the candidate rasteriser state could not be created");
            return false;
        }
        return true;
    }
    bool makeCommon(Report& r) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = kPw;
        td.Height = kPh;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &target)) ||
            FAILED(dev->CreateRenderTargetView(target, nullptr, &rtv))) {
            r.line("foveation probe: the 512x512 target could not be created");
            return false;
        }
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) {
            r.line("foveation probe: the staging copy could not be created");
            return false;
        }
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = kBands * 4;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kBands;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth = kBands * 4;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &counts)) ||
            FAILED(dev->CreateUnorderedAccessView(counts, &ud, &uav)) ||
            FAILED(dev->CreateBuffer(&sd, nullptr, &countsStaging))) {
            r.line("foveation probe: the invocation counter could not be created");
            return false;
        }
        vs = edvr::shaderSwapCompileVs(ctx, kProbeVs, sizeof(kProbeVs) - 1, "main", "foveation_probe_vs",
                                       nullptr, "foveation probe");
        ps = edvr::shaderSwapCompilePs(ctx, kProbePs, sizeof(kProbePs) - 1, "main", "foveation_probe_ps",
                                       nullptr, "foveation probe");
        if (!vs || !ps) {
            r.line("foveation probe: the probe's shaders did not compile");
            return false;
        }
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&rd, &rs);
        return true;
    }
    // The image and its view, remade per variant (the old view is left to
    // the process). Tile rows cycle through the eight table entries; the
    // texels arrive as initial data or by UpdateSubresource.
    bool makeImage(bool viaUpdate, char* err, size_t errCap, bool asArray = false, uint32_t tilesW = kPtiles,
                   uint32_t tilesH = kPtiles, uint32_t slices = 1) {
        if (image) { image->Release(); image = nullptr; }
        view = nullptr;
        static uint8_t pattern[512 * 512];
        if (tilesW > 512 || tilesH > 512) {
            snprintf(err, errCap, "an image of %ux%u tiles is past the probe's buffer", tilesW, tilesH);
            return false;
        }
        for (uint32_t j = 0; j < tilesH; ++j) {
            for (uint32_t i = 0; i < tilesW; ++i) pattern[j * tilesW + i] = static_cast<uint8_t>((j / 4) & 7);
        }
        D3D11_TEXTURE2D_DESC id = {};
        id.Width = tilesW;
        id.Height = tilesH;
        id.MipLevels = 1;
        id.ArraySize = slices;
        id.Format = DXGI_FORMAT_R8_UINT;
        id.SampleDesc.Count = 1;
        id.Usage = D3D11_USAGE_DEFAULT;
        id.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {pattern, tilesW, 0};
        const HRESULT hr = dev->CreateTexture2D(&id, viaUpdate ? nullptr : &init, &image);
        if (FAILED(hr)) {
            snprintf(err, errCap, "the image could not be created (hr 0x%08lX)", static_cast<unsigned long>(hr));
            return false;
        }
        if (viaUpdate) {
            for (uint32_t s = 0; s < slices; ++s) ctx->UpdateSubresource(image, s, nullptr, pattern, tilesW, 0);
        }
        edvr::NvRateViewDesc vd = {};
        vd.version = edvr::kRateViewVer1;
        vd.format = DXGI_FORMAT_R8_UINT;
        if (asArray) {
            vd.dimension = 5;   // NV_SRRV_DIMENSION_TEXTURE2DARRAY
            vd.tex2dArray.mipSlice = 0;
            vd.tex2dArray.firstSlice = 0;
            vd.tex2dArray.arraySize = slices;
        } else {
            vd.dimension = edvr::kDimTexture2D;
            vd.tex2d.mipSlice = 0;
        }
        edvr::NvStatus rc = -1;
        const bool survived = edvr::guardedBudget(edvr::g_budget, [&] { rc = edvr::g_nv.createRateView(dev, image, &vd, &view); });
        if (!survived || rc != edvr::kNvOk || !view) {
            char e[96];
            snprintf(err, errCap, "NvAPI_D3D11_CreateShadingRateResourceView answered %s",
                     survived ? edvr::nvError(rc, e, sizeof(e)) : "a fault (caught)");
            view = nullptr;
            return false;
        }
        return true;
    }
};

struct Measure {
    bool     drew = false;
    uint32_t bw[kBands] = {}, bh[kBands] = {}, count[kBands] = {};
};

// One draw through the image, bound the way the module binds it (the table,
// then the view, all sixteen viewports), and the measurement: the shaded
// block's size per band from the positions, and the shader's invocations
// per band from the counter. viewportsAfter sets the viewport again AFTER
// the rates and the view, as a game does between our bind and its draw.
bool runVariant(ProbeRig& g, uint32_t table0, bool viewportsAfter, bool rebindAfter, const Candidate* c, Measure& m, char* err, size_t errCap) {
    ID3D11DeviceContext* ctx = g.ctx;
    const FLOAT clear[4] = {-1.0f, -1.0f, 0.0f, 0.0f};
    const UINT zeros[4] = {0, 0, 0, 0};
    TargetSet* custom = nullptr;
    if (c && c->targetW) {
        custom = g.targetFor(*c, err, errCap);
        if (!custom) return false;
    }
    ID3D11RenderTargetView* rtv = custom ? custom->rtv : (c && c->arrayTarget) ? (c->slice1 ? g.rtvArr1 : g.rtvArr) : g.rtv;
    ID3D11DepthStencilView* dsv = (c && c->depth) ? g.dsv : nullptr;
    const uint32_t tw = custom ? custom->w : kPw, th = custom ? custom->h : kPh;
    const bool floatTarget = custom ? custom->floatFormat : true;
    const bool mrt = c && c->mrt;
    ID3D11RenderTargetView* rtvs[3] = {rtv, g.mrtRtv[0], g.mrtRtv[1]};
    const UINT rtvCount = mrt ? 3u : 1u;
    const UINT uavSlot = mrt ? 3u : 1u;
    ctx->ClearRenderTargetView(rtv, clear);
    if (dsv) ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ctx->ClearUnorderedAccessViewUint(g.uav, zeros);
    ID3D11UnorderedAccessView* uavs[1] = {g.uav};
    D3D11_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(tw), static_cast<float>(th), 0.0f, 1.0f};
    ctx->RSSetState(c && g.rsCandidate ? g.rsCandidate : g.rs);
    if (c && c->scissor) {
        D3D11_RECT rect = {0, 0, static_cast<LONG>(kPw), static_cast<LONG>(kPh)};
        ctx->RSSetScissorRects(1, &rect);
    }
    ID3D11VertexShader* vs = (c && (c->psKind == 2 || c->psKind == 3)) ? g.vsUv : g.vs;
    ID3D11PixelShader* ps = !c ? g.ps : c->psKind == 1 ? g.psDepth : c->psKind == 2 ? g.psSample : c->psKind == 3 ? g.psCoverage : c->psKind == 4 ? g.psAlpha : c->psKind == 5 ? g.psMrt : g.ps;
    ID3D11BlendState* blend = (c && c->psKind == 4) ? g.blendA2c : nullptr;
    ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(dsv ? g.dss : nullptr, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(rtvCount, rtvs, dsv, uavSlot, 1, uavs, nullptr);
    ctx->RSSetViewports(1, &vp);
    // The command-list path: the draw recorded on a deferred context with the
    // same state, replayed on the immediate one -- a game that records its
    // scene that way replays draws no hook here sees.
    ID3D11CommandList* commandList = nullptr;
    ID3D11DeviceContext* deferred = nullptr;
    if (c && c->commandList) {
        if (FAILED(g.dev->CreateDeferredContext(0, &deferred)) || !deferred) {
            snprintf(err, errCap, "no deferred context");
            return false;
        }
        deferred->RSSetState(c && g.rsCandidate ? g.rsCandidate : g.rs);
        deferred->OMSetBlendState(blend, nullptr, 0xFFFFFFFFu);
        deferred->OMSetDepthStencilState(dsv ? g.dss : nullptr, 0);
        deferred->IASetInputLayout(nullptr);
        deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        deferred->VSSetShader(vs, nullptr, 0);
        deferred->PSSetShader(ps, nullptr, 0);
        deferred->GSSetShader(nullptr, nullptr, 0);
        deferred->OMSetRenderTargetsAndUnorderedAccessViews(rtvCount, rtvs, dsv, uavSlot, 1, uavs, nullptr);
        deferred->RSSetViewports(1, &vp);
    }

    edvr::NvViewportRates rates[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    for (edvr::NvViewportRates& v : rates) {
        v.enable = 1;
        memset(v.pad, 0, sizeof(v.pad));
        for (uint32_t k = 0; k < edvr::kRateTableSize; ++k) v.table[k] = k < kBands ? kCodes[k] : edvr::kRate1x1;
        v.table[0] = table0;
    }
    edvr::NvViewportsRates desc = {edvr::kViewportsVer1, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, rates};
    edvr::NvStatus rc1 = -1, rc2 = -1;
    const bool survived = edvr::guardedBudget(edvr::g_budget, [&] {
        rc1 = edvr::g_nv.setViewportRates(ctx, &desc);
        rc2 = edvr::g_nv.setRateView(ctx, g.view);
    });
    if (!survived || rc1 != edvr::kNvOk || rc2 != edvr::kNvOk) {
        char e[96];
        snprintf(err, errCap, "binding answered %s (rates %d, view %d)",
                 survived ? edvr::nvError(rc1 != edvr::kNvOk ? rc1 : rc2, e, sizeof(e)) : "a fault (caught)", rc1, rc2);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        return false;
    }
    if (viewportsAfter) {
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(g.rs);
    }
    if (rebindAfter) {
        // The same target bound again, as a game rebinds its eye target
        // between passes: does the image survive the rebind?
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(rtvCount, rtvs, dsv, uavSlot, 1, uavs, nullptr);
        ctx->RSSetViewports(1, &vp);
    }
    if (c && c->clearAfterBind) {
        // A clear of the target after the image is bound, as a game clears
        // its targets at the start of a pass.
        ctx->ClearRenderTargetView(rtv, clear);
    }
    if (deferred) {
        if (c->commandList == 2) {
            // The image set on the deferred context itself, before the
            // draw is recorded: does NvAPI take it there at all?
            edvr::NvStatus rcD1 = -1, rcD2 = -1;
            edvr::guardedBudget(edvr::g_budget, [&] {
                rcD1 = edvr::g_nv.setViewportRates(deferred, &desc);
                rcD2 = edvr::g_nv.setRateView(deferred, g.view);
            });
            if (rcD1 != edvr::kNvOk || rcD2 != edvr::kNvOk) {
                snprintf(err, errCap, "NvAPI refused the deferred context (rates %d, view %d)", rcD1, rcD2);
                deferred->Release();
                ctx->OMSetRenderTargets(0, nullptr, nullptr);
                return false;
            }
        }
        deferred->Draw(3, 0);   // the command-list candidates draw plainly
        if (FAILED(deferred->FinishCommandList(FALSE, &commandList)) || !commandList) {
            snprintf(err, errCap, "FinishCommandList failed");
            deferred->Release();
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            return false;
        }
        ctx->ExecuteCommandList(commandList, FALSE);
        commandList->Release();
        deferred->Release();
        // The replay restores nothing (FALSE): rebind for the unbind and copy.
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(rtvCount, rtvs, dsv, uavSlot, 1, uavs, nullptr);
    } else if (c && c->drawKind == 1) {
        ctx->IASetIndexBuffer(g.indices, DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed(3, 0, 0);
    } else if (c && c->drawKind == 2) {
        ctx->IASetIndexBuffer(g.indices, DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexedInstanced(3, 2, 0, 0, 0);
    } else if (c && c->drawKind == 3) {
        ctx->DrawInstanced(3, 2, 0, 0);
    } else {
        ctx->Draw(3, 0);
    }
    for (edvr::NvViewportRates& v : rates) v.enable = 0;
    edvr::guardedBudget(edvr::g_budget, [&] {
        edvr::g_nv.setRateView(ctx, nullptr);
        edvr::g_nv.setViewportRates(ctx, &desc);
    });
    ID3D11UnorderedAccessView* none[1] = {nullptr};
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, uavSlot, 1, none, nullptr);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    ctx->RSSetState(g.rs);
    ID3D11Texture2D* staging = custom ? custom->staging : g.staging;
    if (custom) ctx->CopyResource(staging, custom->tex);
    else if (c && c->arrayTarget) ctx->CopySubresourceRegion(g.staging, 0, 0, 0, 0, g.targetArr, c->slice1 ? 1 : 0, nullptr);
    else ctx->CopyResource(g.staging, g.target);
    ctx->CopyResource(g.countsStaging, g.counts);
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
        snprintf(err, errCap, "the readback could not be mapped");
        return false;
    }
    const uint8_t* base = static_cast<const uint8_t*>(map.pData);
    if (floatTarget) {
        const float* first = reinterpret_cast<const float*>(base);
        const float* last = reinterpret_cast<const float*>(base + (th - 1) * map.RowPitch) + 2 * (tw - 1);
        m.drew = first[0] >= 0.0f && last[0] >= 0.0f;
        for (uint32_t band = 0; band < kBands; ++band) {
            const uint32_t y0 = band * 4 * edvr::kTile;
            if (y0 + 16 > th) { m.bw[band] = m.bh[band] = 1; continue; }
            const float* row = reinterpret_cast<const float*>(base + y0 * map.RowPitch);
            uint32_t bw = 1;
            while (bw < 16 && row[2 * bw] == row[0]) ++bw;
            uint32_t bh = 1;
            while (bh < 16) {
                const float* rowk = reinterpret_cast<const float*>(base + (y0 + bh) * map.RowPitch);
                if (rowk[1] != row[1]) break;
                ++bh;
            }
            m.bw[band] = bw;
            m.bh[band] = bh;
        }
    } else {
        m.drew = true;   // judged by the counter alone
        for (uint32_t band = 0; band < kBands; ++band) m.bw[band] = m.bh[band] = 1;
    }
    ctx->Unmap(staging, 0);
    D3D11_MAPPED_SUBRESOURCE cmap = {};
    if (FAILED(ctx->Map(g.countsStaging, 0, D3D11_MAP_READ, 0, &cmap))) {
        snprintf(err, errCap, "the counter readback could not be mapped");
        return false;
    }
    memcpy(m.count, cmap.pData, sizeof(m.count));
    ctx->Unmap(g.countsStaging, 0);
    return true;
}

}  // namespace

extern "C" __declspec(dllexport) int edvrFoveationProbe(void* device, void* context, char* report,
                                                        unsigned reportBytes) {
    Report r{report, reportBytes};
    if (report && reportBytes) report[0] = 0;
    ProbeRig g;
    g.dev = static_cast<ID3D11Device*>(device);
    g.ctx = static_cast<ID3D11DeviceContext*>(context);
    if (!g.dev || !g.ctx) {
        r.line("foveation probe: no device");
        return -1;
    }
    if (!edvr::nvArm()) {
        r.line("foveation probe: NvAPI did not arm -- %s", edvr::g_nv.why);
        return -1;
    }
    char sm[32] = "SM ?";
    const int sup = edvr::nvVrsSupported(g.dev, sm, sizeof(sm));
    r.line("foveation probe: NvAPI is up; the capability query %s%s%s",
           sup == 1 ? "says variable-rate shading is supported (" : sup == 0 ? "says it is NOT supported (" : "was refused (an older structure version?)",
           sup >= 0 ? sm : "", sup >= 0 ? ")" : "");
    if (sup == 0) return -1;
    if (!g.makeCommon(r)) return 0;

    const uint32_t bandFull = kPw * 4 * edvr::kTile;
    int verdict = 0;
    bool anyDrew = false;
    for (int variant = 0; variant < 4; ++variant) {
        // Variant 0 is the module's way and the pass condition; variant 1 is
        // the documented quirk, with texel 0 mapped to 4x4 so an unread
        // image shows as 4x4 everywhere rather than as nothing; variant 2 is
        // the module's way with the viewport set again after the bind, as a
        // game sets its viewports between our bind and its draws; variant 3
        // rebinds the target itself after the bind, as a game does between
        // its passes -- if the image does not survive that, the module must
        // apply it again after every rebind, which it now does.
        const bool viaUpdate = variant != 1;
        const bool viewportsAfter = variant == 2;
        const bool rebindAfter = variant == 3;
        const uint32_t table0 = viaUpdate ? edvr::kRate1x1 : edvr::kRate4x4;
        const char* name = variant == 0 ? "the image written by UpdateSubresource (as the module fills it)"
                         : variant == 1 ? "the same image given its texels as initial data at creation"
                         : variant == 2 ? "the module's way, with the viewport and rasteriser state set again after the bind"
                                        : "the module's way, with the TARGET bound again after the bind";
        char err[240] = {};
        Measure m;
        if (!g.makeImage(viaUpdate, err, sizeof(err)) || !runVariant(g, table0, viewportsAfter, rebindAfter, nullptr, m, err, sizeof(err))) {
            r.line("foveation probe: %s -- %s", name, err);
            continue;
        }
        if (!m.drew) {
            r.line("foveation probe: %s -- the triangle rasterised NOTHING (the probe's own draw failed)", name);
            continue;
        }
        anyDrew = true;
        char blocks[200], counts[200];
        int bl = 0, cl = 0;
        bool named = true, anyEffect = false;
        for (uint32_t band = 0; band < kBands; ++band) {
            bl += snprintf(blocks + bl, sizeof(blocks) - bl, "%s%ux%u", band ? " " : "", m.bw[band], m.bh[band]);
            cl += snprintf(counts + cl, sizeof(counts) - cl, "%s%u", band ? " " : "", m.count[band]);
            const uint32_t ew = (band == 0 && table0 == edvr::kRate4x4) ? 4 : kExpectW[band];
            const uint32_t eh = (band == 0 && table0 == edvr::kRate4x4) ? 4 : kExpectH[band];
            const uint32_t expectCount = bandFull / (ew * eh);
            if (m.bw[band] != ew || m.bh[band] != eh) named = false;
            if (m.count[band] > expectCount + expectCount / 8 || m.count[band] < expectCount - expectCount / 8) named = false;
            if (m.bw[band] != 1 || m.bh[band] != 1 || m.count[band] < bandFull * 3 / 4) anyEffect = true;
        }
        bool allTexel0 = true;
        for (uint32_t band = 1; band < kBands; ++band) {
            if (m.bw[band] != m.bw[0] || m.bh[band] != m.bh[0]) allTexel0 = false;
        }
        r.line("foveation probe: %s -- blocks per band (named %s %s %s %s %s %s %s %s): %s; invocations per band: %s "
               "(full rate %u)%s",
               name, kNames[0], kNames[1], kNames[2], kNames[3], kNames[4], kNames[5], kNames[6], kNames[7], blocks, counts,
               bandFull,
               named ? " -- as named" : (!anyEffect ? (variant == 3 ? " -- NO EFFECT: a rebind of the target DROPS the image" : " -- NO EFFECT") : (allTexel0 ? " -- every tile took texel 0's rate: the image was not read" : " -- an effect, not as named")));
        if (variant == 0) verdict = named ? 1 : (anyEffect ? 2 : 0);
        if (variant == 2 && verdict == 1 && !named) verdict = 2;
        // Variant 3 is informational: the module re-applies after every
        // rebind either way.
    }
    // The candidate states: what a game might set that the plain draw did
    // not. Informational -- each names itself as named, or as holding the
    // shading at full rate.
    {
        char err[240] = {};
        if (!g.makeCandidates(err, sizeof(err))) {
            r.line("foveation probe: the candidate states could not be set up -- %s", err);
        } else {
            // ForcedSampleCount is not among these: with the image bound it
            // removed the device on the desk (2026-09-06), which is its own
            // finding and not one to repeat in every smoke.
            const Candidate candidates[] = {
                {"the target an ARRAY of two, the view over slice 0 (TEXTURE2DARRAY), the image 2D", true, false, false, false, false, false, 0, 0, 0, 0, 0, 0, 0, false, false, false, 0, false, false},
                {"the target an ARRAY of two, the image an array of one (TEXTURE2DARRAY view)", true, true, false, false, false, false, 0, 0, 0, 0, 0, 0, 0, false, false, false, 0, false, false},
                {"a DEPTH buffer bound, depth test on", false, false, true, false, false, false, 0, 0, 0, 0, 0, 0, 0, false, false, false, 0, false, false},
                {"rasteriser MultisampleEnable", false, false, false, true, false, false, 0, 0, 0, 0, 0, 0, 0, false, false, false, 0, false, false},
                {"rasteriser AntialiasedLineEnable", false, false, false, false, true, false, 0, 0, 0, 0, 0, 0, 0, false, false, false, 0, false, false},
                {"rasteriser ScissorEnable with a full rect", false, false, false, false, false, true, 0, 0, 0, 0, 0, 0, 0, false, false, false, 0, false, false},
                {"the pixel shader writing SV_Depth", false, false, false, false, false, false, 0, 1, 0, 0, 0, 0, 0, false, false, false, 0, false, false},
                {"the pixel shader reading a varying at SAMPLE frequency", false, false, false, false, false, false, 0, 2, 0, 0, 0, 0, 0, false, false, false, 0, false, false},
                {"the pixel shader reading SV_Coverage", false, false, false, false, false, false, 0, 3, 0, 0, 0, 0, 0, false, false, false, 0, false, false},
                {"a blend state with ALPHA-TO-COVERAGE, the pixel shader writing alpha 1", false, false, false, false, false, false, 0, 4, 0, 0, 0, 0, 0, false, false, false, 0, false, false},
                {"the draw replayed from a COMMAND LIST, the image set on the immediate context", false, false, false, false, false, false, 0, 0, 1, 0, 0, 0, 0, false, false, false, 0, false, false},
                {"the draw replayed from a COMMAND LIST, the image set on the deferred context before recording", false, false, false, false, false, false, 0, 0, 2, 0, 0, 0, 0, false, false, false, 0, false, false},
                // The target's own properties, which the plain probe never varied.
                {"a 500x500 target (NOT a multiple of 16), the image 32x32 tiles (the quotient rounded UP, as the module sizes it)", false, false, false, false, false, false, 0, 0, 0, 500, 500, 0, 0, false, false, false, 0, false, false},
                {"a 500x500 target, the image 31x31 tiles (the quotient rounded DOWN)", false, false, false, false, false, false, 0, 0, 0, 500, 500, 0, 0, true, false, false, 0, false, false},
                {"the game's 4336x4284 target, the image 271x268 tiles (rounded up)", false, false, false, false, false, false, 0, 0, 0, 4336, 4284, 0, 0, false, false, false, 0, false, false},
                {"the game's 4336x4284 target, the image 271x267 tiles (rounded down)", false, false, false, false, false, false, 0, 0, 0, 4336, 4284, 0, 0, true, false, false, 0, false, false},
                {"a TYPELESS R8G8B8A8 texture under an R8G8B8A8_UNORM view (counted only)", false, false, false, false, false, false, 0, 0, 0, 512, 512, 27, 28, false, false, false, 0, false, false},
                {"a TYPELESS R10G10B10A2 texture under an R10G10B10A2_UNORM view (counted only)", false, false, false, false, false, false, 0, 0, 0, 512, 512, 23, 24, false, false, false, 0, false, false},
                {"THREE targets bound at once, the counter at u3", false, false, false, false, false, false, 0, 5, 0, 0, 0, 0, 0, false, true, false, 0, false, false},
                {"a CLEAR of the target after the image is bound", false, false, false, false, false, false, 0, 0, 0, 0, 0, 0, 0, false, false, true, 0, false, false},
                // The draws a game makes, and the other eye's slice.
                {"DrawIndexed through an index buffer", false, false, false, false, false, false, 0, 0, 0, 0, 0, 0, 0, false, false, false, 1, false, false},
                {"DrawIndexedInstanced, two instances (counts double)", false, false, false, false, false, false, 0, 0, 0, 0, 0, 0, 0, false, false, false, 2, false, false},
                {"DrawInstanced, two instances (counts double)", false, false, false, false, false, false, 0, 0, 0, 0, 0, 0, 0, false, false, false, 3, false, false},
                {"the array target's SLICE 1 under a 2D image", true, false, false, false, false, false, 0, 0, 0, 0, 0, 0, 0, false, false, false, 0, true, false},
                {"the array target's SLICE 1 under an array image of one slice", true, true, false, false, false, false, 0, 0, 0, 0, 0, 0, 0, false, false, false, 0, true, false},
                {"the array target's SLICE 1 under an array image of TWO slices", true, true, false, false, false, false, 0, 0, 0, 0, 0, 0, 0, false, false, false, 0, true, true},
            };
            for (const Candidate& c : candidates) {
                Measure m;
                char cerr[240] = {};
                const uint32_t tw = c.targetW ? c.targetW : kPw, th = c.targetH ? c.targetH : kPh;
                const uint32_t tilesW = c.floorTiles ? tw / edvr::kTile : (tw + edvr::kTile - 1) / edvr::kTile;
                const uint32_t tilesH = c.floorTiles ? th / edvr::kTile : (th + edvr::kTile - 1) / edvr::kTile;
                if (!g.makeRasteriser(c, cerr, sizeof(cerr)) ||
                    !g.makeImage(true, cerr, sizeof(cerr), c.arrayImage, tilesW, tilesH, c.imageSlices2 ? 2u : 1u) ||
                    !runVariant(g, edvr::kRate1x1, false, false, &c, m, cerr, sizeof(cerr))) {
                    r.line("foveation probe: %s -- %s", c.name, cerr);
                    continue;
                }
                if (!m.drew) {
                    r.line("foveation probe: %s -- the triangle rasterised NOTHING", c.name);
                    continue;
                }
                bool named = true, anyEffect = false;
                char counts[200];
                int cl = 0;
                const uint32_t instances = (c.drawKind == 2 || c.drawKind == 3) ? 2u : 1u;
                for (uint32_t band = 0; band < kBands; ++band) {
                    cl += snprintf(counts + cl, sizeof(counts) - cl, "%s%u", band ? " " : "", m.count[band]);
                    const uint32_t expectCount = instances * bandFull / (kExpectW[band] * kExpectH[band]);
                    if (m.count[band] > expectCount + expectCount / 8 || m.count[band] < expectCount - expectCount / 8) named = false;
                    if (m.count[band] < instances * bandFull * 3 / 4) anyEffect = true;
                }
                r.line("foveation probe: %s -- invocations per band: %s%s", c.name, counts,
                       named ? " -- as named" : (!anyEffect ? " -- FULL RATE: this state holds the shading at 1x1" : " -- an effect, not as named"));
            }
        }
    }
    if (!anyDrew) return 0;
    return verdict;
}
