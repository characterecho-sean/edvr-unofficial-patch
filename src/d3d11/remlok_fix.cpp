#include "temporal_shader_bytecode.h"   // F121: kVisorAxisVsBytecode, the visor's axis-centred vertex shader
#include "remlok_fix.h"

#include <windows.h>

#include <d3d11.h>

#include <atomic>
#include <cmath>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "native_on_foot.h"   // F25: nativeOnFootActive for the on-foot visor match
#include "dxbc_container.h"   // F27: the visor variant's checksum (ComputeHashRetail)
#include "shader_swap.h"      // F121: shaderSwapCreateVs
#include "ui_depth.h"         // F27: uiDepthEyeOfTargetReadOnly, the bound target's eye
#include "visor_axis_math.h"  // F121: the shift per eye, the counts and the log lines (pure; tools\visor_axis_test runs the same)
#include "visor_census_core.h"   // F121b: the census of the HDR scene after the visor draw (pure; the rig runs the same decode and model)
#include <cstring>
#include <string>
#include <vector>

namespace edvr {

// remlokWantsDraws reads this from the header with no call: asked per
// eye draw, and the build has no /GL to fold a cross-TU getter.
// RemlokMode itself is declared in the header, so the inline function
// there can name its kStock value.
namespace detail {
RemlokMode g_remlokMode = RemlokMode::kStock;
}  // namespace detail

namespace {
using Mode = detail::RemlokMode;

// The overlay's shape, exactly as the census measured and the field
// suppression verified it (2026-08-19): a fullscreen triangle, one instance,
// no depth view bound, PS slot 0 sampling the 1024x512 overlay image. The
// size is the discriminating half -- the tonemap chain is made of otherwise
// identical fullscreen triangles that sample eye-sized textures.
constexpr char     kKind = 'N';   // DrawInstanced, as the census letters it
constexpr uint32_t kVertices = 3;
constexpr uint32_t kInstances = 1;
constexpr uint32_t kSrvW = 1024;
constexpr uint32_t kSrvH = 512;
// The shape test itself is remlokOverlayShape (remlok_fix.h), inline so the
// draw path can ask it first; it must say what the three constants say.
static_assert(kKind == 'N' && kVertices == 3 && kInstances == 1,
              "remlokOverlayShape (remlok_fix.h) must match the overlay's measured shape");

float g_keep = 0.55f;
bool  g_swap = false;
bool  g_axisArmedNoted = false;   // F121: the once-per-session "armed" line (remlokConfigure)
std::atomic<bool> g_axisKeyOn{false};   // F121b (TEMPORARY, live): fix.native_on_foot_visor_axis, "on" binds F121's vertex shader; default off
bool  g_axisKeyKnown = false, g_axisKeyLast = false;
bool  g_censusArmedNoted = false;   // F121b: the once-per-session "census armed" line (remlokConfigure)
// F27: the eye parity's swap with the native eye order (in native on foot the game renders the right eye first).
inline bool remlokSwapNow() { return g_swap; }   // F31: the control only (the native flip is gone; first = left everywhere)
// Scale of the substituted viewport for the overlay draw, centred. Below 1
// the whole helmet frame renders smaller, which pulls the edge lines INWARD
// from the image border -- on a wide-FOV headset the border is the lens rim,
// where the game's own placement is invisible (measured on a Pimax: the
// correctly-clipped temporal line could only be confirmed in the desktop
// mirror). 1.0 keeps the game's placement. The manual fallback; the angle
// below is the real setting.
float g_scale = 1.0f;

// Where the line should SIT, in degrees from straight ahead -- the human
// constant the scale was hiding. Tuned by eye on two headsets, the chosen
// fractions agreed on one angle to within a degree once the asymmetric
// image centre is accounted for: 0.70 of a Quest 3 (outer tangent 1.376)
// and 0.60 of a Pimax (1.529) are both the line at ~46 degrees. The scale
// that puts the line there is derived per headset from the true tangents
// the openvr half publishes; 0 turns the derivation off and g_scale rules.
float g_angleDeg = 46.0f;
bool  g_derivedNoted = false;

// Matched draws this frame: 0 is the left eye, 1 the right. Reset at the
// frame boundary so one odd frame cannot invert the pair for the session.
uint32_t g_matchesThisFrame = 0;
bool     g_pendingRight = false;

// The game's rasterizer state, cloned once with ScissorEnable flipped on,
// re-cloned only if the game ever draws the overlay with a different state.
// The original is compared by pointer and never dereferenced after Release.
ID3D11RasterizerState* g_cloneSource = nullptr;
ID3D11RasterizerState* g_clone = nullptr;
bool g_cloneSourceIsDefault = false;

// What begin set, for end to put back. engaged is the contract between the
// two: end restores exactly what begin says it changed.
bool                   g_engaged = false;
bool                   g_vpEngaged = false;
ID3D11RasterizerState* g_savedRS = nullptr;
UINT                   g_savedRectCount = 0;
D3D11_RECT             g_savedRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
UINT                   g_savedVpCount = 0;
D3D11_VIEWPORT         g_savedVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};

uint64_t g_applied = 0;
uint64_t g_hidden = 0;
bool     g_cloneFailedNoted = false;

// The scissor-enabled twin of the game's current rasterizer state. Cached
// against the source pointer; a null source means the default state, whose
// description is spelled out because there is no object to ask.
ID3D11RasterizerState* cloneWithScissor(ID3D11DeviceContext* ctx,
                                        ID3D11RasterizerState* source) {
    if (g_clone && source == g_cloneSource &&
        (source || g_cloneSourceIsDefault)) {
        return g_clone;
    }
    D3D11_RASTERIZER_DESC d{};
    if (source) {
        source->GetDesc(&d);
    } else {
        // The documented defaults of the null rasterizer state.
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_BACK;
        d.DepthClipEnable = TRUE;
    }
    d.ScissorEnable = TRUE;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    ID3D11RasterizerState* clone = nullptr;
    const HRESULT hr = dev->CreateRasterizerState(&d, &clone);
    dev->Release();
    if (FAILED(hr) || !clone) {
        if (!g_cloneFailedNoted) {
            g_cloneFailedNoted = true;
            Log::get().note("remlok: could not create the scissor rasterizer "
                            "state (hr=0x%08X); the overlay draws untouched.",
                            static_cast<unsigned>(hr));
        }
        return nullptr;
    }
    if (g_clone) g_clone->Release();
    g_clone = clone;
    g_cloneSource = source;
    g_cloneSourceIsDefault = source == nullptr;
    return g_clone;
}

}  // namespace

void remlokConfigure(Config& cfg) {
    const Mode was = detail::g_remlokMode;
    const std::string m = cfg.getString("fix.remlok_lines", "stock");
    if (m == "stock") {
        detail::g_remlokMode = Mode::kStock;
    } else if (m == "outer") {
        detail::g_remlokMode = Mode::kOuter;
    } else if (m == "hide") {
        detail::g_remlokMode = Mode::kHide;
    } else {
        detail::g_remlokMode = Mode::kStock;
        Log::get().note("remlok_lines \"%s\" is not stock, outer or hide; "
                        "running stock.", m.c_str());
    }

    float keep = cfg.getFloat("advanced.remlok_keep_fraction", 0.55f);
    // Under half and the two eyes' kept regions no longer meet in the
    // middle -- a gap in anything the overlay draws at centre; near 1.0 the
    // nasal line stops being clipped, which is the whole point. Clamp
    // rather than refuse: this is documented as a live-tuned value.
    if (keep < 0.50f) keep = 0.50f;
    if (keep > 0.95f) keep = 0.95f;
    g_keep = keep;
    g_swap = cfg.getBool("advanced.remlok_swap_eyes", false);

    float scale = cfg.getFloat("advanced.remlok_scale", 1.0f);
    // Below half the helmet frame shrinks into a porthole; above 1 would
    // push the lines further into the lens rim, the opposite of the knob's
    // reason to exist.
    if (scale < 0.50f) scale = 0.50f;
    if (scale > 1.00f) scale = 1.00f;
    g_scale = scale;

    float ang = cfg.getFloat("fix.remlok_line_angle", 46.0f);
    // 0 is the documented off switch; anything else is clamped into the band
    // where the line is neither in the middle of the view nor back at the
    // rim this setting exists to escape.
    if (ang != 0.0f) {
        if (ang < 20.0f) ang = 20.0f;
        if (ang > 60.0f) ang = 60.0f;
    }
    if (ang != g_angleDeg) g_derivedNoted = false;   // re-announce the derivation
    g_angleDeg = ang;

    // F121b (TEMPORARY, live A/B): the vertex shader of F121 is bound only with this key on. Default off: the pilot's scene went black with it on (the log of 2026-10-25 13:29-13:32).
    {
        const bool keyOn = cfg.getString("fix.native_on_foot_visor_axis", "off") == "on";
        g_axisKeyOn.store(keyOn, std::memory_order_relaxed);
        if (!g_axisKeyKnown || keyOn != g_axisKeyLast) {
            g_axisKeyKnown = true;
            g_axisKeyLast = keyOn;
            char key[480];
            visorcensus::formatKey(key, sizeof(key), keyOn);
            Log::get().note("%s", key);
        }
    }
    if (detail::g_remlokMode == Mode::kOuter && !g_axisArmedNoted) {   // F121: said once, so a log with no visor draw reads IDLE and not "never ran"
        g_axisArmedNoted = true;
        char armed[320];
        visoraxis::formatArmed(armed, sizeof(armed));
        Log::get().note("%s", armed);
    }
    if (detail::g_remlokMode == Mode::kOuter && !g_censusArmedNoted) {   // F121b: the census runs with the key on or off
        g_censusArmedNoted = true;
        char armed[400];
        visorcensus::formatArmed(armed, sizeof(armed));
        Log::get().note("%s", armed);
    }

    if (was != detail::g_remlokMode) {
        const char* names[] = {"stock", "outer", "hide"};
        Log::get().note("remlok lines: %s. The overlay is recognised by shape "
                        "(3 vertices, 1 instance, no depth, 1024x512 in PS "
                        "slot 0); outer keeps %.0f%% of each eye's width from "
                        "its own temple.",
                        names[static_cast<uint32_t>(detail::g_remlokMode)], g_keep * 100.0f);
    }
}

namespace {

// The viewport scale that puts the overlay's edge line at g_angleDeg for
// THIS headset, from the true tangents the openvr half publishes -- or the
// manual g_scale when the angle is off or nobody has published (openvr half
// absent, or its hook not yet validated).
//
// The drawn image spans tangents [-outer, +inner] with outward negative, so
// its CENTRE sits (outer-inner)/2 outward of straight ahead -- a centred
// viewport scale scales about that point, not about the gaze. The formula
// is validated by the field tuning it replaced: 0.70 on a Quest 3 and 0.60
// on a Pimax both land within a degree of 46 through it.
float effectiveScale() {
    float outer = 0.0f, inner = 0.0f;
    if (g_angleDeg <= 0.0f || !eyeTangents(&outer, &inner)) return g_scale;

    const float halfSpan = (outer + inner) * 0.5f;
    const float centerOut = (outer - inner) * 0.5f;
    if (halfSpan < 1e-3f) return g_scale;

    const float target = tanf(g_angleDeg * 3.1415926535f / 180.0f);
    float s = (target - centerOut) / halfSpan;
    if (s < 0.50f) s = 0.50f;
    if (s > 1.00f) s = 1.00f;
    if (!g_derivedNoted) {
        g_derivedNoted = true;
        Log::get().note("remlok lines: this headset's outer tangent is %.3f "
                        "(%.1f deg), so the %.0f-degree line means overlay "
                        "scale %.2f. remlok_scale is not consulted while "
                        "remlok_line_angle is set.",
                        outer, atanf(outer) * 180.0f / 3.1415926535f,
                        g_angleDeg, s);
    }
    return s;
}

}  // namespace

// F25: the on-foot visor edge. census_skip of vs:B553BB479B7C0B97 removed it on foot (Sean, F24 flight). The draw is a fullscreen triangle, once per
// eye before the tonemap, with a DSV bound, PS 68ABCB9FEF6CA66C, RT 2016x2016 R11G11B10, SRV2 1024x1024 BC4 and SRV3 512x512 BC5.
constexpr uint64_t kOnFootVisorVs = 0xB553BB479B7C0B97ull;
constexpr uint64_t kOnFootVisorPs = 0x68ABCB9FEF6CA66Cull;
constexpr uint32_t kOnFootVisorRt = 2016, kOnFootVisorRtFormat = 26;   // DXGI_FORMAT_R11G11B10_FLOAT
bool onFootVisorMatches() {
    // F26: the VS alone (census_skip proved it unique to the visor edge; the flight's attribute check did not reject it on foot).
    return bindingShaderHash(BindSlot::Vs) == kOnFootVisorVs;
}

// ---- F27: the on-foot visor's per-eye pixel shader ---------------------------------------------------------------------------------------
// PS 68ABCB9FEF6CA66C draws the visor edge lines with a screen-x-keyed weight (`add_sat r2.z, |r2.z|, -0.666667`): 1 at both eye edges, 0 in the
// middle, so the lines show on both sides. The LEFT variant makes the operand -|r2.z| (weight 1 at x = 0, the temple; 0 for x >= 0.5); the RIGHT
// variant drops the modifier (weight 1 at x = 1, the temple; 0 for x <= 0.5). The change is one byte: the extended operand token's modifier at file
// offset 1724 (0x81 abs -> 0x41 neg, or -> 0x01 none). The length is unchanged, and the DXBC checksum (header bytes 4-19) is recomputed. The 20 bytes
// from offset 1720 must match first, or the variant is refused. The variant is bound by the eye identity (the bound target's eye from the eye filing,
// NOT parity); remlok_swap_eyes swaps the two as a control. Only the outer mode uses it; the draw is not scissored any more.
namespace {
constexpr uint64_t kVisorPsHash = 0x68ABCB9FEF6CA66Cull;
constexpr size_t kVisorEyeOffset = 1724;
const uint8_t kVisorEyeSeq[20] = {0x2A, 0x00, 0x10, 0x80, 0x81, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0xAB, 0xAA, 0x2A, 0xBF};
std::vector<uint8_t> g_visorPsBytes;               // the game's bytecode, kept when the game creates the shader
bool g_visorPsKept = false;
ID3D11PixelShader* g_visorPsVariant[2] = {nullptr, nullptr};   // 0 left, 1 right: created once, kept for the session
bool g_visorPsTried = false;
bool g_visorPending = false;                       // set by remlokOnEyeDraw for the on-foot match in outer mode; consumed by the begin
bool g_visorActive = false;                        // a variant is bound for the draw in flight
ID3D11PixelShader* g_visorSaved = nullptr;         // the game's pixel shader, restored after the draw
uint64_t g_visorUses[2] = {0, 0};                  // per 30 s window
uint64_t g_visorRefusals = 0;
uint64_t g_visorWindowMs = 0;

// ---- F121: the visor frame centred on each eye's optical axis (visor_axis_math.h says why) -----------------------------------------------
// The game's visor vertex shader is replaced, for the visor draw only and per eye, by kVisorAxisVsBytecode: identical but for the interpolated uv,
// which it moves by s (+a/2 left, -a/2 right, a from the headset's eye tangents). F121b: bound only with fix.native_on_foot_visor_axis on (TEMPORARY, default
// off: with it on the pilot's scene went black). Anything it cannot be sure of is refused and counted, and the draw runs the game's own shader.
static_assert(kOnFootVisorVs == visoraxis::kVisorVsHash && kOnFootVisorPs == visoraxis::kVisorPsHash,
              "the visor hashes in visor_axis_math.h (which the rig scans) are the ones the on-foot match uses");
ID3D11VertexShader* g_axisVs = nullptr;           // the replacement, made once
bool g_axisVsTried = false;
ID3D11Buffer* g_axisCb = nullptr;                 // its b3: one float4, x = s; dynamic, written per draw
ID3D11VertexShader* g_axisSaved = nullptr;        // the game's vertex shader, put back after the draw (AddRef'd)
ID3D11Buffer* g_axisSavedCb3 = nullptr;           // what the game had at b3 (null for this draw, but put back exactly)
bool g_axisActive = false;                        // our shader is bound for the draw in flight
bool g_axisFirstNoted = false;
float g_axisNotedS[2] = {0.0f, 0.0f};             // the s the once-line last said (a new line when the tangents move it)
float g_axisLastS[2] = {0.0f, 0.0f};              // the s last worked out for each eye
float g_axisLastOuter = 0.0f, g_axisLastInner = 0.0f;
bool g_axisLastTangents = false;
bool g_axisRefusalNoted[static_cast<unsigned>(visoraxis::Refusal::kCount)] = {false, false, false, false};
visoraxis::Counts g_axisCounts;                   // per 30 s window
uint64_t g_axisWindowMs = 0;

void axisRefuse(visoraxis::Refusal why) {
    ++g_axisCounts.refused[static_cast<unsigned>(why)];
    if (g_axisRefusalNoted[static_cast<unsigned>(why)]) return;
    g_axisRefusalNoted[static_cast<unsigned>(why)] = true;   // said once a session, counted in every window
    char line[512];
    visoraxis::formatRefused(line, sizeof(line), visoraxis::refusalText(why));
    Log::get().note("%s", line);
}

// The window's line (30 s of visor draws), then the counts start over.
void axisNoteWindow(uint64_t now) {
    if (!g_axisWindowMs) {
        g_axisWindowMs = now;
        return;
    }
    if (now - g_axisWindowMs < 30000) return;
    char line[640];
    visoraxis::formatWindow(line, sizeof(line), g_axisCounts, g_axisLastS[0], g_axisLastS[1], g_axisLastOuter, g_axisLastInner, g_axisLastTangents);
    Log::get().note("%s", line);
    g_axisCounts = visoraxis::Counts();
    g_axisWindowMs = now;
}

// The begin: the replacement vertex shader and its shift at b3, around the game's visor draw, for the eye the filing named (0 left, 1 right).
void axisBegin(ID3D11DeviceContext* ctx, int eye, bool gamesPs) {
    ++g_axisCounts.seen[eye];
    if (!g_axisKeyOn.load(std::memory_order_relaxed)) {   // F121b: the test key is off (the default): the game's own visor vertex shader
        ++g_axisCounts.keyOff;
        return;
    }
    // The shift is designed for this pixel shader's visor frame (its uv, from this vertex shader's o1.x): another one is not ours to move.
    if (!gamesPs) {
        axisRefuse(visoraxis::Refusal::kOtherPs);
        return;
    }
    float outer = 0.0f, inner = 0.0f, s = 0.0f;
    if (!eyeTangents(&outer, &inner) || !visoraxis::shiftUv(outer, inner, eye, &s)) {
        g_axisLastTangents = false;
        axisRefuse(visoraxis::Refusal::kNoTangents);
        return;
    }
    g_axisLastTangents = true;
    g_axisLastOuter = outer;
    g_axisLastInner = inner;
    g_axisLastS[eye] = s;
    if (s == 0.0f) {   // a symmetric frustum: the game's frame is already on the axis
        ++g_axisCounts.symmetric;
        return;
    }
    if (!g_axisVsTried) {
        g_axisVsTried = true;
        g_axisVs = shaderSwapCreateVs(ctx, kVisorAxisVsBytecode, sizeof(kVisorAxisVsBytecode), "visor_axis_vs", "visor axis (F121)");
    }
    if (g_axisVs && !g_axisCb) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 16;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            dev->CreateBuffer(&bd, nullptr, &g_axisCb);
            dev->Release();
        }
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (!g_axisVs || !g_axisCb || FAILED(ctx->Map(g_axisCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) {
        axisRefuse(visoraxis::Refusal::kNoShader);
        return;
    }
    float* f = static_cast<float*>(m.pData);
    f[0] = s;
    f[1] = f[2] = f[3] = 0.0f;
    ctx->Unmap(g_axisCb, 0);

    ctx->VSGetShader(&g_axisSaved, nullptr, nullptr);
    ctx->VSGetConstantBuffers(3, 1, &g_axisSavedCb3);
    ID3D11Buffer* ours = g_axisCb;
    ctx->VSSetConstantBuffers(3, 1, &ours);
    ctx->VSSetShader(g_axisVs, nullptr, 0);
    g_axisActive = true;
    ++g_axisCounts.shifted[eye];

    float sL = 0.0f, sR = 0.0f, axis = 0.0f;
    visoraxis::shiftUv(outer, inner, 0, &sL);
    visoraxis::shiftUv(outer, inner, 1, &sR);
    visoraxis::axisNdc(outer, inner, &axis);
    if (!g_axisFirstNoted || std::fabs(sL - g_axisNotedS[0]) > 0.0005f || std::fabs(sR - g_axisNotedS[1]) > 0.0005f) {
        g_axisFirstNoted = true;
        g_axisNotedS[0] = sL;
        g_axisNotedS[1] = sR;
        char line[640];
        visoraxis::formatFirst(line, sizeof(line), sL, sR, axis, outer, inner, static_cast<unsigned>(sizeof(kVisorAxisVsBytecode)));
        Log::get().note("%s", line);
    }
}

void axisEnd(ID3D11DeviceContext* ctx) {
    if (!g_axisActive) return;
    g_axisActive = false;
    ctx->VSSetShader(g_axisSaved, nullptr, 0);
    if (g_axisSaved) {
        g_axisSaved->Release();
        g_axisSaved = nullptr;
    }
    ctx->VSSetConstantBuffers(3, 1, &g_axisSavedCb3);
    if (g_axisSavedCb3) {
        g_axisSavedCb3->Release();
        g_axisSavedCb3 = nullptr;
    }
}

// ---- F121b: the census of the HDR scene after the visor draw (visor_census_core.h says why and what; visor_census_shader.h is the shader) ----------------
// One sample every sixth visor draw an eye, right after the draw: a copy of the eye's HDR target (R11G11B10, the draw's own render target), one dispatch of the census
// over the copy (a copy, because a target still bound for output cannot be bound for reading), the compute stage's shader, views, UAV and constants put back, the
// output copied into a staging ring of three and read on a later sample (DO_NOT_WAIT, the F85c and F119 way). The game's exposure (the visor pixel shader's t0)
// is read in the same dispatch where its view is a float 2D texture. Nothing the game reads is written. A second's line, by the key's state.
struct CensusGpu {
    ID3D11ComputeShader* cs = nullptr;
    bool csTried = false, failed = false;
    ID3D11Buffer* cb = nullptr;
    ID3D11Buffer* out[2] = {};
    ID3D11UnorderedAccessView* uav[2] = {};
    ID3D11Buffer* stage[2][3] = {};
    ID3D11Texture2D* copy[2] = {};
    ID3D11ShaderResourceView* copySrv[2] = {};
    uint32_t copyW[2] = {}, copyH[2] = {};
    struct Slot {
        bool inFlight = false, keyOn = false;
        uint32_t seq = 0, w = 0, h = 0, age = 0;
    };
    Slot slot[2][3];
    uint32_t seq = 0, draws[2] = {0, 0}, ring[2] = {0, 0};
    bool pending = false;
    int pendingEye = -1;
    ID3D11ShaderResourceView* expo = nullptr;   // the game's exposure view at the draw (AddRef'd), until the census has run
    bool saidFormat = false, saidResources = false;
};
CensusGpu g_cen;
visorcensus::Window g_cenWin;
uint64_t g_cenWinMs = 0;

void censusRelease() {
    CensusGpu& g = g_cen;
    for (int e = 0; e < 2; ++e) {
        if (g.copySrv[e]) g.copySrv[e]->Release();
        if (g.copy[e]) g.copy[e]->Release();
        if (g.uav[e]) g.uav[e]->Release();
        if (g.out[e]) g.out[e]->Release();
        for (auto* st : g.stage[e])
            if (st) st->Release();
    }
    if (g.expo) g.expo->Release();
    if (g.cb) g.cb->Release();
    if (g.cs) g.cs->Release();
    g = CensusGpu{};
}

bool censusEnsure(ID3D11DeviceContext* ctx) {
    CensusGpu& g = g_cen;
    if (g.failed) return false;
    if (!g.csTried) {
        g.csTried = true;
        g.cs = shaderSwapCreateCs(ctx, kVisorCensusBytecode, sizeof(kVisorCensusBytecode), "visor_census_cs", "visor census (F121b)");
    }
    if (!g.cs) {
        g.failed = true;
        return false;
    }
    if (g.cb) return true;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = visorcensus::kWords * 4u;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 4;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = visorcensus::kWords;
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = visorcensus::kWords * 4u;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 16;
    cd.Usage = D3D11_USAGE_DYNAMIC;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bool made = SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &g.cb));
    for (int e = 0; e < 2 && made; ++e) {
        made = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g.out[e])) && SUCCEEDED(dev->CreateUnorderedAccessView(g.out[e], &ud, &g.uav[e]));
        for (int i = 0; i < 3 && made; ++i) made = SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g.stage[e][i]));
    }
    dev->Release();
    if (!made) {
        Log::get().note("visor axis (F121b): the GPU resources of the census could not be made; the HDR scene after the visor draw is not measured");
        censusRelease();
        g_cen.csTried = true;
        g_cen.failed = true;
        return false;
    }
    return true;
}

// The copy of an eye's HDR target the census reads: made for its size, replaced when the size moves.
bool censusCopyFor(ID3D11DeviceContext* ctx, int e, const D3D11_TEXTURE2D_DESC& td) {
    CensusGpu& g = g_cen;
    if (g.copy[e] && g.copyW[e] == td.Width && g.copyH[e] == td.Height) return true;
    if (g.copySrv[e]) g.copySrv[e]->Release();
    if (g.copy[e]) g.copy[e]->Release();
    g.copySrv[e] = nullptr;
    g.copy[e] = nullptr;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC cd{};
    cd.Width = td.Width;
    cd.Height = td.Height;
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    cd.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    cd.SampleDesc.Count = 1;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const bool ok = SUCCEEDED(dev->CreateTexture2D(&cd, nullptr, &g.copy[e])) && SUCCEEDED(dev->CreateShaderResourceView(g.copy[e], nullptr, &g.copySrv[e]));
    dev->Release();
    if (!ok) {
        if (g.copySrv[e]) g.copySrv[e]->Release();
        if (g.copy[e]) g.copy[e]->Release();
        g.copySrv[e] = nullptr;
        g.copy[e] = nullptr;
        return false;
    }
    g.copyW[e] = td.Width;
    g.copyH[e] = td.Height;
    return true;
}

// The window's line (a second, or the key changed under it), then a new window.
void censusLine() {
    if (g_cenWin.samples || g_cenWin.skipped) {
        char line[640];
        visorcensus::formatLine(line, sizeof(line), g_cenWin);
        Log::get().note("%s", line);
    }
    const bool key = g_cenWin.keyOn;
    g_cenWin = visorcensus::Window();
    g_cenWin.keyOn = key;
}
void censusAdd(int e, const visorcensus::Census& c, bool keyOn) {
    if (g_cenWin.samples && g_cenWin.keyOn != keyOn) censusLine();
    g_cenWin.keyOn = keyOn;
    g_cenWin.add(e, c);
}

// Earlier samples: each answered readback becomes the second's.
void censusPoll(ID3D11DeviceContext* ctx) {
    CensusGpu& g = g_cen;
    for (int e = 0; e < 2; ++e) {
        for (int i = 0; i < 3; ++i) {
            CensusGpu::Slot& sl = g.slot[e][i];
            if (!sl.inFlight) continue;
            if (++sl.age < 2u) continue;   // a sample is read on the run after the one that followed it
            D3D11_MAPPED_SUBRESOURCE m{};
            const HRESULT hr = ctx->Map(g.stage[e][i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                if (sl.age > 8u) {
                    sl.inFlight = false;
                    ++g_cenWin.skipped;
                }
                continue;
            }
            if (FAILED(hr) || !m.pData) {
                sl.inFlight = false;
                ++g_cenWin.skipped;
                continue;
            }
            uint32_t words[visorcensus::kWords];
            std::memcpy(words, m.pData, sizeof(words));
            ctx->Unmap(g.stage[e][i], 0);
            sl.inFlight = false;
            visorcensus::Census c;
            if (!visorcensus::decode(words, sl.seq, sl.w, sl.h, &c)) {
                ++g_cenWin.skipped;
                continue;
            }
            censusAdd(e, c, sl.keyOn);
        }
    }
}

// The begin: decides whether this draw is a sample (the first, then every sixth, an eye) and keeps the game's exposure view while the game's state is bound.
void censusBegin(ID3D11DeviceContext* ctx, int eye) {
    CensusGpu& g = g_cen;
    g.pending = false;
    if (g.failed) return;
    if ((g.draws[eye]++ % visorcensus::kEvery) != 0u) return;
    g.pending = true;
    g.pendingEye = eye;
    if (g.expo) {
        g.expo->Release();
        g.expo = nullptr;
    }
    ctx->PSGetShaderResources(0, 1, &g.expo);   // AddRef'd, or null
}

// The end: after the visor draw, with the game's render target still bound.
void censusRun(ID3D11DeviceContext* ctx) {
    CensusGpu& g = g_cen;
    if (!g.pending) return;
    g.pending = false;
    const int e = g.pendingEye;
    ID3D11ShaderResourceView* expoView = g.expo;   // owned: released below
    g.expo = nullptr;
    struct Release {
        ID3D11ShaderResourceView*& v;
        ~Release() { if (v) { v->Release(); v = nullptr; } }
    } releaseExpo{expoView};
    if (e < 0 || e > 1 || !censusEnsure(ctx)) return;

    // the second's line, and the readbacks of earlier samples
    censusPoll(ctx);
    const uint64_t now = GetTickCount64();
    if (!g_cenWinMs) {
        g_cenWinMs = now;
    } else if (now - g_cenWinMs >= 1000) {
        if (!g_cenWin.samples) g_cenWin.keyOn = g_axisKeyOn.load(std::memory_order_relaxed);   // a second of samples keeps the state they were taken under
        censusLine();
        g_cenWinMs = now;
    }

    // the eye's HDR target: the draw's own render target
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    rtv->Release();
    if (!res) return;
    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
    res->Release();
    if (!tex) return;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    if (td.Format != DXGI_FORMAT_R11G11B10_FLOAT || td.MipLevels != 1 || td.ArraySize != 1 || td.SampleDesc.Count != 1 || !td.Width || !td.Height) {
        if (!g.saidFormat) {
            g.saidFormat = true;
            char t[200], note[260];
            std::snprintf(t, sizeof(t), "the visor's render target is %ux%u, format %u, %u mips, %u samples: not the R11G11B10 HDR target, so not measured", td.Width, td.Height,
                          static_cast<unsigned>(td.Format), td.MipLevels, td.SampleDesc.Count);
            visorcensus::formatNote(note, sizeof(note), t);
            Log::get().note("%s", note);
        }
        tex->Release();
        return;
    }
    const uint32_t si = g.ring[e] % 3u;
    CensusGpu::Slot& sl = g.slot[e][si];
    if (sl.inFlight || !censusCopyFor(ctx, e, td)) {
        ++g_cenWin.skipped;
        tex->Release();
        return;
    }
    ctx->CopyResource(g.copy[e], tex);
    tex->Release();

    // the game's exposure: a float 2D texture view only
    bool expoOk = false;
    if (expoView) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        expoView->GetDesc(&vd);
        expoOk = vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D &&
                 (vd.Format == DXGI_FORMAT_R32_FLOAT || vd.Format == DXGI_FORMAT_R16_FLOAT || vd.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
                  vd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) {
        ++g_cenWin.skipped;
        return;
    }
    ++g.seq;
    const uint32_t cbv[4] = {td.Width, td.Height, g.seq, expoOk ? 1u : 0u};
    std::memcpy(m.pData, cbv, sizeof(cbv));
    ctx->Unmap(g.cb, 0);

    ID3D11ComputeShader* savedCs = nullptr;
    ID3D11ShaderResourceView* savedSrv[2] = {};
    ID3D11UnorderedAccessView* savedUav = nullptr;
    ID3D11Buffer* savedCb = nullptr;
    ctx->CSGetShader(&savedCs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 2, savedSrv);
    ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
    ctx->CSGetConstantBuffers(0, 1, &savedCb);
    ID3D11ShaderResourceView* nullSrv[2] = {};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 2, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    const UINT zero[4] = {0, 0, 0, 0};
    ctx->ClearUnorderedAccessViewUint(g.uav[e], zero);
    ctx->CSSetShader(g.cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g.cb);
    ID3D11ShaderResourceView* views[2] = {g.copySrv[e], expoOk ? expoView : nullptr};
    ctx->CSSetShaderResources(0, 2, views);
    ctx->CSSetUnorderedAccessViews(0, 1, &g.uav[e], nullptr);
    ctx->Dispatch(visorcensus::kThreads / 8u, visorcensus::kThreads / 8u, 1);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShaderResources(0, 2, nullSrv);
    ctx->CopyResource(g.stage[e][si], g.out[e]);
    ctx->CSSetShader(savedCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 2, savedSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &savedUav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &savedCb);
    if (savedCs) savedCs->Release();
    for (ID3D11ShaderResourceView* v : savedSrv)
        if (v) v->Release();
    if (savedUav) savedUav->Release();
    if (savedCb) savedCb->Release();

    sl.inFlight = true;
    sl.keyOn = g_axisKeyOn.load(std::memory_order_relaxed);
    sl.seq = g.seq;
    sl.w = td.Width;
    sl.h = td.Height;
    sl.age = 0;
    ++g.ring[e];
}
}  // namespace

// Called from the device's pixel-shader creation hook: keeps the visor shader's bytecode (only that hash, once).
void remlokRememberVisorPs(uint64_t hash, const void* bytecode, size_t bytes) {
    if (hash != kVisorPsHash || g_visorPsKept || !bytecode || bytes < kVisorEyeOffset + sizeof(kVisorEyeSeq)) return;
    g_visorPsBytes.assign(static_cast<const uint8_t*>(bytecode), static_cast<const uint8_t*>(bytecode) + bytes);
    g_visorPsKept = true;
}

// The patched bytecode for one side, or why not. Verifies the sequence and the container magic before the one-byte change.
bool visorVariantBytes(bool right, std::vector<uint8_t>& out, std::string& why) {
    const std::vector<uint8_t>& in = g_visorPsBytes;
    if (in.size() < kVisorEyeOffset + sizeof(kVisorEyeSeq) || in[0] != 'D' || in[1] != 'X' || in[2] != 'B' || in[3] != 'C') {
        why = "the kept bytecode is not a DXBC container of the expected size";
        return false;
    }
    if (std::memcmp(in.data() + kVisorEyeOffset - 4, kVisorEyeSeq, sizeof(kVisorEyeSeq)) != 0 || in[kVisorEyeOffset] != 0x81) {
        why = "the token bytes at offset 1720 are not the expected sequence (the modifier at 1724 is not 0x81)";
        return false;
    }
    out = in;
    out[kVisorEyeOffset] = right ? 0x01 : 0x41;
    ComputeHashRetail(out.data() + 20, static_cast<UINT>(out.size() - 20), out.data() + 4);
    return true;
}

// The variant for a side, created on first use (both sides together). Null when refused; the refusal is logged once.
ID3D11PixelShader* visorVariant(ID3D11DeviceContext* ctx, bool right) {
    if (!g_visorPsTried) {
        g_visorPsTried = true;
        std::string why = g_visorPsKept ? "" : "the visor shader's bytecode was not seen at creation";
        ID3D11Device* dev = nullptr;
        if (why.empty()) ctx->GetDevice(&dev);
        for (int side = 0; side < 2 && why.empty(); ++side) {
            std::vector<uint8_t> bytes;
            if (!visorVariantBytes(side == 1, bytes, why)) break;
            ID3D11PixelShader* ps = nullptr;
            const HRESULT hr = dev ? dev->CreatePixelShader(bytes.data(), bytes.size(), nullptr, &ps) : E_FAIL;
            if (FAILED(hr) || !ps) {
                char t[64];
                _snprintf_s(t, _TRUNCATE, "CreatePixelShader 0x%08X", static_cast<unsigned>(hr));
                why = t;
                break;
            }
            g_visorPsVariant[side] = ps;
        }
        if (dev) dev->Release();
        if (why.empty()) {
            Log::get().note("remlok lines (on foot): patched PS created (L, R): the visor's edge weight is per eye (68ABCB9FEF6CA66C, byte 1724 %s / %s, checksum recomputed)",
                            "0x41", "0x01");
        } else {
            ++g_visorRefusals;
            Log::get().note("remlok lines (on foot): NOT patched -- %s. The visor draws unchanged this session.", why.c_str());
        }
    }
    return g_visorPsVariant[right ? 1 : 0];
}

// The begin of the on-foot visor draw (outer mode): binds the variant for this draw's eye, and keeps the game's shader for the end.
void visorSwapBegin(ID3D11DeviceContext* ctx) {
    g_visorPending = false;
    const uint64_t now = GetTickCount64();
    // F121 counts only the visor draws of native stereo on foot (a draw with no eye outside it is not a fault): every one of those passes here, with or without an eye.
    const bool onFoot = nativeOnFootActive();
    if (onFoot) {
        axisNoteWindow(now);   // the 30 s line
        ++g_axisCounts.seenAll;
    }
    ResourceInfo rt{};
    if (!bindingResolve(bindingGet(BindSlot::Rtv0), &rt)) {
        ++g_visorRefusals;
        if (onFoot) axisRefuse(visoraxis::Refusal::kNoEye);
        return;
    }
    const int eye = uiDepthEyeOfTargetReadOnly(rt.resource);   // never registers: the eye the filing gave this target (native order included)
    if (eye < 0) {
        ++g_visorRefusals;
        if (onFoot) axisRefuse(visoraxis::Refusal::kNoEye);
        return;
    }
    const bool gamesPs = bindingShaderHash(BindSlot::Ps) == visoraxis::kVisorPsHash;   // read before our own pixel shader goes on
    if (onFoot) censusBegin(ctx, eye);   // F121b: every sixth draw an eye is a sample (run at the end, with the game's state back)
    const bool right = (eye == 1) != g_swap;                   // the control (remlok_swap_eyes) swaps the two
    ID3D11PixelShader* variant = visorVariant(ctx, right);
    if (variant) {
        ctx->PSGetShader(&g_visorSaved, nullptr, nullptr);     // AddRef'd: released at the end
        ctx->PSSetShader(variant, nullptr, 0);
        g_visorActive = true;
        ++g_visorUses[right ? 1 : 0];
        if (!g_visorWindowMs) {
            g_visorWindowMs = now;
        } else if (now - g_visorWindowMs >= 30000) {
            Log::get().note("remlok lines (on foot): visor PS variants bound in the last 30 s: left eye %llu, right eye %llu (%llu refusals); eye rule %s",
                            static_cast<unsigned long long>(g_visorUses[0]), static_cast<unsigned long long>(g_visorUses[1]),
                            static_cast<unsigned long long>(g_visorRefusals), "cockpit (first = left)");
            g_visorUses[0] = g_visorUses[1] = 0;
            g_visorWindowMs = now;
        }
    }
    if (onFoot) axisBegin(ctx, eye, gamesPs);   // F121: the true eye (not the swap control's): the frame goes onto THAT eye's axis
}

void visorSwapEnd(ID3D11DeviceContext* ctx) {
    axisEnd(ctx);   // F121: the game's vertex shader and its b3 back (a no-op when ours was not bound)
    if (g_visorActive) {
        g_visorActive = false;
        ctx->PSSetShader(g_visorSaved, nullptr, 0);
        if (g_visorSaved) g_visorSaved->Release();
        g_visorSaved = nullptr;
    }
    censusRun(ctx);   // F121b: the census of the HDR target right after the draw (a no-op when this draw is not a sample)
}

// F26: the one-shot diagnostic for the on-foot visor draw (the flight: the match never fired). The first time per session, and every 30 s while
// the draw keeps coming, it logs every attribute the matcher can see and which test rejects the draw. Read only.
void remlokOnFootDiagnose(char kind, uint32_t count, uint32_t instances) {
    if (bindingShaderHash(BindSlot::Vs) != kOnFootVisorVs) return;
    static uint64_t draws = 0, lastMs = 0;
    ++draws;
    const uint64_t nowMs = GetTickCount64();
    if (lastMs && nowMs - lastMs < 30000) return;
    lastMs = nowMs;
    ResourceInfo rt{}, s2{}, s3{};
    const bool rtOk = bindingResolve(bindingGet(BindSlot::Rtv0), &rt);
    const bool s2Ok = bindingResolve(bindingGet(BindSlot::PsSrv2), &s2);
    const bool s3Ok = bindingResolve(bindingGet(BindSlot::PsSrv3), &s3);
    const bool dsv = bindingGet(BindSlot::Dsv0) != nullptr;
    const bool onFoot = nativeOnFootActive();
    const bool wanted = remlokWantsDraws();
    const bool right = (((g_matchesThisFrame & 1u) != 0u) != remlokSwapNow());
    const char* rejected = "none: the draw matches (VS and shape)";
    if (!wanted) rejected = "fix.remlok_lines is stock (remlokWantsDraws)";
    else if (kind != 'N') rejected = "indexed draw (the shape wants non-indexed)";
    else if (count != 3 || instances != 1) rejected = "not 3 vertices and 1 instance";
    Log::get().note("remlok lines (on foot): diagnostic (draw %llu with VS B553BB479B7C0B97): kind %c, %u vertices, %u instances; PS %016llX; "
                    "RTV %s %ux%u format %u; DSV %s; PS SRV2 %s %ux%u format %u; PS SRV3 %s %ux%u format %u; the eye the parity assigns: %s; "
                    "native on foot %s; fix.remlok_lines %s; rejected by: %s",
                    static_cast<unsigned long long>(draws), kind, static_cast<unsigned>(count), static_cast<unsigned>(instances),
                    static_cast<unsigned long long>(bindingShaderHash(BindSlot::Ps)),
                    rtOk ? "resolved" : "NOT resolved", static_cast<unsigned>(rt.a), static_cast<unsigned>(rt.b), static_cast<unsigned>(rt.fmt),
                    dsv ? "bound" : "not bound",
                    s2Ok ? "resolved" : "NOT resolved", static_cast<unsigned>(s2.a), static_cast<unsigned>(s2.b), static_cast<unsigned>(s2.fmt),
                    s3Ok ? "resolved" : "NOT resolved", static_cast<unsigned>(s3.a), static_cast<unsigned>(s3.b), static_cast<unsigned>(s3.fmt),
                    right ? "right" : "left", onFoot ? "now" : "not now", wanted ? "outer or hide" : "stock", rejected);
}

RemlokAction remlokOnEyeDraw(char kind, uint32_t count, uint32_t instances) {
    if (detail::g_remlokMode == Mode::kStock) return RemlokAction::kNone;
    if (!remlokOverlayShape(kind, count, instances)) {
        return RemlokAction::kNone;
    }
    const bool footVisor = onFootVisorMatches();
    if (!footVisor) {
    // The overlay binds no depth; scene and HUD draws do. Checked before the
    // SRV resolve so the resolve only runs for depthless fullscreen
    // triangles, which are a handful a frame.
    if (bindingGet(BindSlot::Dsv0) != nullptr) return RemlokAction::kNone;
    ResourceInfo info;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &info) ||
        !info.isTexture2D || info.a != kSrvW || info.b != kSrvH) {
        return RemlokAction::kNone;
    }
    }

    const uint32_t match = g_matchesThisFrame++;
    if (footVisor) {
        // F27: the outer mode binds the per-eye variant at the draw (visorSwapBegin); not the scissor.
        g_visorPending = detail::g_remlokMode == Mode::kOuter;
        // F27: per-eye counts over each 30 s window prove the parity (the eye rule follows the native eye order, F27).
        static uint64_t footCount = 0, footLeft = 0, footRight = 0, footLastMs = 0;
        ++footCount;
        const bool rightEye = ((match & 1u) != 0u) != remlokSwapNow();
        if (rightEye) ++footRight; else ++footLeft;
        const uint64_t nowMs = GetTickCount64();
        if (footLastMs == 0) {
            footLastMs = nowMs;
            Log::get().note("remlok lines (on foot): first match of VS B553BB479B7C0B97 (non-indexed, 3 vertices, 1 instance): the %s eye; eye rule %s",
                            rightEye ? "right" : "left", "cockpit (first = left)");
        } else if (nowMs - footLastMs >= 30000) {
            Log::get().note("remlok lines (on foot): %llu matches in the last 30 s: left eye %llu, right eye %llu (%llu this session); eye rule %s",
                            static_cast<unsigned long long>(footLeft + footRight), static_cast<unsigned long long>(footLeft),
                            static_cast<unsigned long long>(footRight), static_cast<unsigned long long>(footCount),
                            "cockpit (first = left)");
            footLastMs = nowMs;
            footLeft = footRight = 0;
        }
    }
    if (detail::g_remlokMode == Mode::kHide) {
        if (++g_hidden == 1) {
            Log::get().note("remlok lines: hidden (first overlay draw "
                            "suppressed this session).");
        }
        return RemlokAction::kHide;
    }
    g_pendingRight = ((match & 1u) != 0u) != remlokSwapNow();
    return RemlokAction::kScissor;
}

void remlokScissorBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    if (g_visorPending) {   // F27: the on-foot visor draw takes the per-eye variant, not the scissor
        visorSwapBegin(ctx);
        return;
    }

    ID3D11RasterizerState* current = nullptr;
    ctx->RSGetState(&current);   // AddRef'd when non-null
    ID3D11RasterizerState* scissored = cloneWithScissor(ctx, current);
    if (!scissored) {
        if (current) current->Release();
        return;   // the draw runs untouched, which stock already survives
    }

    UINT vpCount = 1;
    D3D11_VIEWPORT vp{};
    ctx->RSGetViewports(&vpCount, &vp);
    if (vpCount == 0 || vp.Width <= 0.0f) {
        if (current) current->Release();
        return;
    }

    g_savedRS = current;   // keep the reference until end restores it
    g_savedRectCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetScissorRects(&g_savedRectCount, g_savedRects);

    // The substituted viewport, when a scale applies: the same centre, a
    // fraction of the size -- derived from remlok_line_angle and this
    // headset's true tangents when the openvr half has published them,
    // remlok_scale otherwise. The fullscreen triangle renders the whole
    // overlay into it, which moves the edge lines inward from the image
    // border -- the border is the lens rim on a wide-FOV headset, and a
    // line drawn there is a line nobody sees. The undrawn margin ring is
    // territory the overlay used to cover; nothing else draws after it on
    // this target, so leaving it untouched is leaving the scene visible.
    const float scale = effectiveScale();
    g_vpEngaged = false;
    if (scale < 0.999f) {
        g_savedVpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&g_savedVpCount, g_savedVps);
        D3D11_VIEWPORT scaled = vp;
        scaled.Width = vp.Width * scale;
        scaled.Height = vp.Height * scale;
        scaled.TopLeftX = vp.TopLeftX + (vp.Width - scaled.Width) * 0.5f;
        scaled.TopLeftY = vp.TopLeftY + (vp.Height - scaled.Height) * 0.5f;
        ctx->RSSetViewports(1, &scaled);
        g_vpEngaged = true;
        vp = scaled;   // the scissor below clips against what is drawn
    }

    // Each eye keeps the g_keep fraction nearest its own temple: the left
    // eye's rectangle starts at the (possibly scaled) viewport's left edge,
    // the right eye's ends at its right edge. What is clipped is the nasal
    // remainder, where the wrong-side line sat.
    const LONG x0 = static_cast<LONG>(vp.TopLeftX);
    const LONG y0 = static_cast<LONG>(vp.TopLeftY);
    const LONG w = static_cast<LONG>(vp.Width);
    const LONG h = static_cast<LONG>(vp.Height);
    const LONG keep = static_cast<LONG>(vp.Width * g_keep);
    D3D11_RECT r{};
    r.top = y0;
    r.bottom = y0 + h;
    if (g_pendingRight) {
        r.left = x0 + w - keep;
        r.right = x0 + w;
    } else {
        r.left = x0;
        r.right = x0 + keep;
    }
    ctx->RSSetScissorRects(1, &r);
    ctx->RSSetState(scissored);
    g_engaged = true;

    if (++g_applied == 1) {
        Log::get().note("remlok lines: outer mode engaged -- each eye now "
                        "keeps the line at its own temple and loses the one "
                        "along the nose. First applied to the %s eye, %ld of "
                        "%ld px kept, overlay scale %.2f.",
                        g_pendingRight ? "right" : "left", keep, w,
                        scale);
    }
}

void remlokScissorEnd(ID3D11DeviceContext* ctx) {
    if (g_visorActive || g_axisActive || g_cen.pending) {   // F27: restores the game's pixel shader after the visor draw; F121: and its vertex shader; F121b: the census
        visorSwapEnd(ctx);
        return;
    }
    if (!g_engaged) return;
    g_engaged = false;
    ctx->RSSetState(g_savedRS);
    if (g_savedRS) {
        g_savedRS->Release();
        g_savedRS = nullptr;
    }
    ctx->RSSetScissorRects(g_savedRectCount,
                           g_savedRectCount ? g_savedRects : nullptr);
    if (g_vpEngaged) {
        g_vpEngaged = false;
        ctx->RSSetViewports(g_savedVpCount,
                            g_savedVpCount ? g_savedVps : nullptr);
    }
}

void remlokFrameBoundary() { g_matchesThisFrame = 0; }

void remlokShutdown() {
    censusRelease();   // F121b
    if (g_clone) {
        g_clone->Release();
        g_clone = nullptr;
    }
    g_cloneSource = nullptr;
    if (g_savedRS) {
        g_savedRS->Release();
        g_savedRS = nullptr;
    }
}

}  // namespace edvr
