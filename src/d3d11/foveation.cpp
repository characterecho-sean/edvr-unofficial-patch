#include "foveation.h"

#include <windows.h>

#include <d3d11.h>

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
constexpr uint32_t kIdRegisterDevice   = 0x8C02C4D0u;  // NvAPI_D3D_RegisterDevice (the probe tries it)

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
    PFN_NvQueryInterface   query = nullptr;
    char                   why[240] = {};
};
NvApi       g_nv;
FaultBudget g_budget("foveation", 3);

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
uint32_t g_settingsGen = 1;    // bumped on any change; images refill at their next use
bool     g_configured = false;

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

// The frame's eye-sized targets in first-bind order, for eye attribution.
struct Seen {
    void*    resource;
    uint32_t w, h, fmt;
    int      eye;
};
Seen     g_seen[48];
uint32_t g_seenCount = 0;

uint32_t  g_frame = 0;
IUnknown* g_bound = nullptr;        // the view the context holds, as far as we know
bool      g_boundUnknown = false;   // ClearState: whatever was bound may be gone
bool      g_ratesOn = false;
uint32_t  g_lastRtvGen = ~0u;
Mask*     g_lastMask = nullptr;

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

void fillRates(bool enable, uint32_t mid, uint32_t outer) {
    for (NvViewportRates& r : g_rates) {
        r.enable = enable ? 1 : 0;
        memset(r.pad, 0, sizeof(r.pad));
        for (uint32_t& t : r.table) t = kRate1x1;
        r.table[1] = mid;
        r.table[2] = outer;
    }
    g_ratesDesc.version = kViewportsVer1;
    g_ratesDesc.numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    g_ratesDesc.viewports = g_rates;
}

uint32_t outerRate() { return g_mode == Mode::Quality ? kRate2x2 : kRate4x4; }

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
    fillRates(view != nullptr, kRate2x2, outerRate());
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
        fillRates(false, kRate2x2, outerRate());
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
    if (g_distance <= 0.0f) return;
    float ox = eye == 0 ? -0.032f : 0.032f, oy = 0.0f;
    float off[3];
    if (temporalPassEyeOffset(eye, off) && std::isfinite(off[0]) && std::isfinite(off[1])) {
        ox = off[0];
        oy = off[1];
    }
    *cx = -ox / g_distance;
    *cy = -oy / g_distance;
}

// A tile's rate is the finest its NEAREST point to the centre needs: a tile
// the full-rate disc touches at all is full rate. Radii in tangent space
// about the centre, which is the angle for a centre near the axis.
bool fillMask(ID3D11DeviceContext* ctx, Mask& m) {
    float l, r, top, bot;
    if (!frustumOf(m.eye, m.w, m.h, &l, &r, &top, &bot)) return false;
    float cx, cy;
    centreOf(m.eye, &cx, &cy);
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
            g_scratch[static_cast<size_t>(j) * m.tw + i] = d2 < ri2 ? 0 : (d2 < ro2 ? 1 : 2);
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
            if (m.gen != g_settingsGen && !fillMask(ctx, m)) return nullptr;
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
    int eye = -1;
    for (int e = 0; e < 2; ++e) {
        void* sub = submittedTexture(e);
        if (sub && sub == info.resource) eye = e;
    }
    if (eye >= 0) {
        ++g_submitMatched;
    } else {
        uint32_t alike = 0;
        for (uint32_t i = 0; i < g_seenCount; ++i) {
            if (g_seen[i].w == info.a && g_seen[i].h == info.b && g_seen[i].fmt == info.fmt) ++alike;
        }
        eye = static_cast<int>(alike & 1u);
    }
    if (g_seenCount < sizeof(g_seen) / sizeof(g_seen[0])) {
        g_seen[g_seenCount++] = Seen{info.resource, info.a, info.b, info.fmt, eye};
    }
    ++g_targetsFrame[eye];
    return eye;
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
        "degrees, one per %s beyond; %s. The image binds at the first eye draw.",
        modeName(g_mode),
        sup == 1 ? "the driver says this GPU shades at variable rate" : "the capability query was refused, so the view will decide",
        g_innerDeg, g_distance, g_outerDeg, outerRate() == kRate4x4 ? "4x4" : "2x2",
        g_geometryOnly ? "geometry draws only, the full-screen passes at full rate (advanced.foveation_passes = geometry)"
                       : "every draw into the eye, the full-screen passes included (advanced.foveation_passes = all)");
}

void summary(const char* when) {
    const double f = g_framesArmed ? static_cast<double>(g_framesArmed) : 1.0;
    Log::get().note(
        "foveation: %s -- %u frames with the image armed; %.1f eye-sized targets a frame attributed "
        "to the left eye and %.1f to the right (%.1f a frame by the submitted texture, the rest by "
        "first-bind order); %.1f image switches a frame (most %u); %u images made; %u frames "
        "without published tangents.",
        when, g_framesArmed, static_cast<double>(g_targetsSum[0]) / f,
        static_cast<double>(g_targetsSum[1]) / f, static_cast<double>(g_submitMatched) / f,
        static_cast<double>(g_switches) / f, g_switchesMax, g_imagesMade, g_noTangentFrames);
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
    float dist = cfg.getFloat("advanced.foveation_distance", 0.7f);
    if (!std::isfinite(dist) || dist < 0.0f) dist = 0.0f;
    if (dist > 0.0f && dist < 0.2f) dist = 0.2f;
    const std::string passes = cfg.getString("advanced.foveation_passes", "all");
    const bool geom = passes == "geometry";

    const bool changed = m != g_mode || inner != g_innerDeg || outer != g_outerDeg ||
                         dist != g_distance || geom != g_geometryOnly;
    const bool first = !g_configured;
    g_configured = true;
    g_mode = m;
    g_innerDeg = inner;
    g_outerDeg = outer;
    g_distance = dist;
    g_geometryOnly = geom;
    if (changed) ++g_settingsGen;

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
                        "%s beyond, fixation %.2f m, %s draws. Arms at the first eye draw "
                        "(docs/performance.md, feature 2).",
                        modeName(m), inner, outer, outerRate() == kRate4x4 ? "4x4" : "2x2", dist,
                        geom ? "geometry" : "all");
    } else if (changed && g_phase == Phase::Armed) {
        Log::get().note("foveation: settings changed (fix.foveation = %s, %.0f/%.0f degrees, %.2f m, %s "
                        "draws) -- the images refill at their next use.",
                        modeName(m), inner, outer, dist, geom ? "geometry" : "all");
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
    const bool fullScreenPass = count <= 6 && instances <= 1;
    if (!rtvEyeSized || (g_geometryOnly && fullScreenPass)) {
        if (g_bound || g_boundUnknown) applyView(ctx, nullptr);
        return;
    }
    if (g_phase == Phase::Wanted) {
        arm(ctx);
        if (g_phase != Phase::Armed) return;
    }
    if (rtvGen != g_lastRtvGen) {
        g_lastRtvGen = rtvGen;
        g_lastMask = nullptr;
        ResourceInfo info;
        if (bindingResolve(rtv, &info) && info.isTexture2D && info.a >= kTile && info.b >= kTile) {
            const int eye = eyeOf(info);
            g_lastMask = maskFor(ctx, info.a, info.b, eye);
            if (!g_lastMask && g_phase == Phase::Armed) ++g_noTangentFrames;
        }
    }
    IUnknown* want = g_lastMask ? g_lastMask->view : nullptr;
    if (want != g_bound || g_boundUnknown) applyView(ctx, want);
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
        if (g_switchesFrame > g_switchesMax) g_switchesMax = g_switchesFrame;
        g_targetsSum[0] += g_targetsFrame[0];
        g_targetsSum[1] += g_targetsFrame[1];
        if (!g_summaryDone && g_framesArmed == 600) {
            g_summaryDone = true;
            summary("after 600 frames");
        }
        if (!g_noTangentNoted && g_noTangentFrames >= 600) {
            g_noTangentNoted = true;
            Log::get().note("foveation: %u frames and the openvr half has published no eye tangents to "
                            "centre the image on (no openvr_api.dll installed, or an older one). The "
                            "image stays unbound until they arrive.", g_noTangentFrames);
        }
    }
    g_switchesFrame = 0;
    g_seenCount = 0;
    g_targetsFrame[0] = g_targetsFrame[1] = 0;
    g_lastRtvGen = ~0u;
    g_lastMask = nullptr;
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
    ID3D11PixelShader*         ps = nullptr;
    ID3D11RasterizerState*     rs = nullptr;
    ~ProbeRig() {
        if (rtv) rtv->Release();
        if (target) target->Release();
        if (staging) staging->Release();
        if (image) image->Release();
        if (counts) counts->Release();
        if (countsStaging) countsStaging->Release();
        if (uav) uav->Release();
        if (vs) vs->Release();
        if (ps) ps->Release();
        if (rs) rs->Release();
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
    bool makeImage(bool viaUpdate, char* err, size_t errCap) {
        if (image) { image->Release(); image = nullptr; }
        view = nullptr;
        uint8_t pattern[kPtiles * kPtiles];
        for (uint32_t j = 0; j < kPtiles; ++j) {
            for (uint32_t i = 0; i < kPtiles; ++i) pattern[j * kPtiles + i] = static_cast<uint8_t>((j / 4) & 7);
        }
        D3D11_TEXTURE2D_DESC id = {};
        id.Width = kPtiles;
        id.Height = kPtiles;
        id.MipLevels = 1;
        id.ArraySize = 1;
        id.Format = DXGI_FORMAT_R8_UINT;
        id.SampleDesc.Count = 1;
        id.Usage = D3D11_USAGE_DEFAULT;
        id.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {pattern, kPtiles, 0};
        const HRESULT hr = dev->CreateTexture2D(&id, viaUpdate ? nullptr : &init, &image);
        if (FAILED(hr)) {
            snprintf(err, errCap, "the image could not be created (hr 0x%08lX)", static_cast<unsigned long>(hr));
            return false;
        }
        if (viaUpdate) ctx->UpdateSubresource(image, 0, nullptr, pattern, kPtiles, 0);
        edvr::NvRateViewDesc vd = {};
        vd.version = edvr::kRateViewVer1;
        vd.format = DXGI_FORMAT_R8_UINT;
        vd.dimension = edvr::kDimTexture2D;
        vd.tex2d.mipSlice = 0;
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
// per band from the counter.
bool runVariant(ProbeRig& g, uint32_t table0, Measure& m, char* err, size_t errCap) {
    ID3D11DeviceContext* ctx = g.ctx;
    const FLOAT clear[4] = {-1.0f, -1.0f, 0.0f, 0.0f};
    const UINT zeros[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(g.rtv, clear);
    ctx->ClearUnorderedAccessViewUint(g.uav, zeros);
    ID3D11UnorderedAccessView* uavs[1] = {g.uav};
    D3D11_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(kPw), static_cast<float>(kPh), 0.0f, 1.0f};
    ctx->RSSetState(g.rs);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g.vs, nullptr, 0);
    ctx->PSSetShader(g.ps, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &g.rtv, nullptr, 1, 1, uavs, nullptr);
    ctx->RSSetViewports(1, &vp);

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
    ctx->Draw(3, 0);
    for (edvr::NvViewportRates& v : rates) v.enable = 0;
    edvr::guardedBudget(edvr::g_budget, [&] {
        edvr::g_nv.setRateView(ctx, nullptr);
        edvr::g_nv.setViewportRates(ctx, &desc);
    });
    ID3D11UnorderedAccessView* none[1] = {nullptr};
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 1, none, nullptr);
    ctx->CopyResource(g.staging, g.target);
    ctx->CopyResource(g.countsStaging, g.counts);
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(ctx->Map(g.staging, 0, D3D11_MAP_READ, 0, &map))) {
        snprintf(err, errCap, "the readback could not be mapped");
        return false;
    }
    const uint8_t* base = static_cast<const uint8_t*>(map.pData);
    const float* first = reinterpret_cast<const float*>(base);
    const float* last = reinterpret_cast<const float*>(base + (kPh - 1) * map.RowPitch) + 2 * (kPw - 1);
    m.drew = first[0] >= 0.0f && last[0] >= 0.0f;
    for (uint32_t band = 0; band < kBands; ++band) {
        const uint32_t y0 = band * 4 * edvr::kTile;
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
    ctx->Unmap(g.staging, 0);
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
    for (int variant = 0; variant < 2; ++variant) {
        // Variant 0 is the module's way and the pass condition; variant 1 is
        // the documented quirk, with texel 0 mapped to 4x4 so an unread
        // image shows as 4x4 everywhere rather than as nothing.
        const bool viaUpdate = variant == 0;
        const uint32_t table0 = viaUpdate ? edvr::kRate1x1 : edvr::kRate4x4;
        const char* name = viaUpdate ? "the image written by UpdateSubresource (as the module fills it)"
                                     : "the same image given its texels as initial data at creation";
        char err[240] = {};
        Measure m;
        if (!g.makeImage(viaUpdate, err, sizeof(err)) || !runVariant(g, table0, m, err, sizeof(err))) {
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
               named ? " -- as named" : (!anyEffect ? " -- NO EFFECT" : (allTexel0 ? " -- every tile took texel 0's rate: the image was not read" : " -- an effect, not as named")));
        if (viaUpdate) verdict = named ? 1 : (anyEffect ? 2 : 0);
    }
    if (!anyDrew) return 0;
    return verdict;
}
