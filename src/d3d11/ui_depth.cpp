#include "ui_depth.h"

#include <windows.h>

#include <d3d11.h>

#include <cstdint>
#include <cstdlib>   // _strtoui64, strtod: the hash lists and the planes
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "depth_probe.h"    // depthProbeIsSceneDepth, ...Format: where the pass reads
#include "exposure_fix.h"   // lookupShaderHash
#include "shader_swap.h"    // shaderSwapCompilePs: the alpha-aware depth shaders
#include "temporal_pass.h"  // temporalPassPlanes: the scene's encoding
#include "vscreen.h"        // vScreenSetRenderTargetsRaw: the rebind past the shadow

namespace edvr {
namespace {

// The GUI renderer's three families on game build 332753 (docs/
// crisp-ui-handoff.md, A1): the textureless vector widget shader, the
// glyph-atlas text shader, the BC7 icon shader. What they draw into is a UI
// surface.
constexpr uint64_t kGuiVector = 0x666EF0C4C616F67Eull;
constexpr uint64_t kGuiText   = 0x1012E00B3CB44469ull;
constexpr uint64_t kGuiIcons  = 0xA3E5D3FCBC1165F8ull;
// The flight HUD's vector family, drawn straight into the eye (hud_grain.h).
constexpr uint64_t kFlightHud = 0xB7790CBFC6554097ull;
// The cockpit's holo-panel family (panel_upscale.h).
constexpr uint64_t kHoloPanel = 0x81216C77F90DEDD6ull;
// The two interface composites drawn through the interface projection: the
// menu's and the loader's panel (vs A888D51024D9798E, ps 9107E72CB016CC02)
// and the loader's curved screen (vs 4EF6DDB075A927FA, ps 85565E9261812E2F).
constexpr uint64_t kPanelVs  = 0xA888D51024D9798Eull;
constexpr uint64_t kPanelPs  = 0x9107E72CB016CC02ull;
constexpr uint64_t kScreenVs = 0x4EF6DDB075A927FAull;
constexpr uint64_t kScreenPs = 0x85565E9261812E2Full;
// A mesh draw whose pixel shader (258B95AC99520C1F) reads nothing and writes
// nothing; the interface surface in its slot 0 is a leftover binding, and
// the surface rule took it for a composite on the loading screen
// (2026-09-07). Never treated.
constexpr uint64_t kNullPsMesh = 0xB018D143700AB803ull;

constexpr uint32_t kMaxSurfaces = 64;      // a session showed thirteen
constexpr uint32_t kMaxHashes = 16;
constexpr uint32_t kMaxStates = 16;        // the game has a handful of depth states
constexpr uint32_t kMemoSize = 1024;       // a power of two: the probe masks
constexpr uint32_t kMemoProbe = 4;
constexpr uint32_t kMemoLifeFrames = 120;  // a sampled view's answer
constexpr uint32_t kVsMemoLifeFrames = 600; // a shader's hash
constexpr uint32_t kChecksPerTarget = 64;  // hashes asked of a newly bound target
constexpr uint32_t kExhausted = 64;        // targets asked to exhaustion, remembered
constexpr uint32_t kExhaustedRearmFrames = 600;
constexpr uint32_t kMaxFamilyLines = 16;
constexpr uint32_t kTotalsFrames = 1800;   // about 20 s at 90 Hz

FaultBudget g_budget("uiDepth", 5);

bool     g_keyOn = false;      // fix.ui_depth = on
bool     g_passOn = false;     // fix.temporal_aa is not off
bool     g_on = false;         // both
bool     g_stoodDown = false;
bool     g_announced = false;
bool     g_waitingNoted = false;
bool     g_testAlways = false; // advanced.ui_depth_test = always
bool     g_menus = true;       // advanced.ui_depth_menus: the interface-projection
                               // composites get the alpha-aware depth pass
bool     g_eyesSwapped = false; // advanced.ui_depth_eyes = swapped: the A/B for
                                // the order rule that names the eye
// THE ENCODING. The menu's and the loader's composites are drawn through
// the interface projection -- near 0.1 m, far 1000 m on build 332841 (the
// receiver logs every pair the game asks for) -- while the scene pair the
// pass reads is decoded with the scene's (0.025 m, 50000 m). A reversed-Z
// value is near/z to within a part in a thousand this side of ten metres,
// so the two encodings differ by the ratio of the nears: a composite's
// depth written as it comes decoded four times too near (measured
// 2026-09-07: the menu panel at 0.26 m where 1.03 m is right). The depth
// pass's VIEWPORT depth range carries the correction: MaxDepth =
// sceneNear / uiNear scales what the rasteriser writes, no maths in the
// shader.
float    g_uiNear = 0.1f;       // advanced.ui_depth_planes
float    g_uiFar = 1000.0f;
float    g_alphaFloor = 0.5f;   // advanced.ui_depth_alpha: below it, no depth
bool     g_scaleNoted = false;
constexpr uint32_t kMaxViewports = 16;
D3D11_VIEWPORT g_savedVps[kMaxViewports];
UINT     g_savedVpCount = 0;
bool     g_vpScaled = false;
uint32_t g_frame = 0;

uint64_t g_families[kMaxHashes];
uint32_t g_familyCount = 0;
uint64_t g_exclude[kMaxHashes];
uint32_t g_excludeCount = 0;

// A hashed memo from a pointer to a value, frame-stamped: one probe of a
// few slots, no lock. The pointer is an identity only, never dereferenced
// here (binding_shadow.h's bargain); an answer older than its life is
// asked again, so a recycled address cannot lie for long.
template <uint32_t N>
struct PtrMemo {
    struct Slot {
        const void* key = nullptr;
        uint32_t    frame = 0;
        uint64_t    value = 0;
    };
    Slot slots[N];

    static uint32_t home(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p) >> 4;
        return static_cast<uint32_t>(v * 2654435761u) & (N - 1);
    }
    bool get(const void* p, uint32_t now, uint32_t life, uint64_t* value) const {
        const uint32_t h = home(p);
        for (uint32_t i = 0; i < kMemoProbe; ++i) {
            const Slot& s = slots[(h + i) & (N - 1)];
            if (s.key != p) continue;
            if (now - s.frame >= life) return false;
            *value = s.value;
            return true;
        }
        return false;
    }
    void put(const void* p, uint32_t now, uint64_t value) {
        const uint32_t h = home(p);
        uint32_t victim = h & (N - 1);
        uint32_t oldest = 0;
        for (uint32_t i = 0; i < kMemoProbe; ++i) {
            const uint32_t idx = (h + i) & (N - 1);
            const Slot& s = slots[idx];
            if (s.key == p || s.key == nullptr) {
                victim = idx;
                break;
            }
            const uint32_t age = now - s.frame;
            if (age >= oldest) {
                oldest = age;
                victim = idx;
            }
        }
        slots[victim].key = p;
        slots[victim].frame = now;
        slots[victim].value = value;
    }
    void clear() {
        for (uint32_t i = 0; i < N; ++i) slots[i] = Slot();
    }
};

PtrMemo<kMemoSize> g_viewMemo;   // sampled view -> 1 surface / 0 not
PtrMemo<kMemoSize> g_vsMemo;     // vertex shader -> its bytecode hash
PtrMemo<kMemoSize> g_psMemo;     // pixel shader -> its bytecode hash

// Learned surfaces: resource identities with the shape they had when
// learned. A match is by identity AND shape, so an address the game
// recycled for a different texture drops out instead of lying (the ABA
// binding_shadow.h warns about). A ring past kMaxSurfaces, counted.
struct Surface {
    void*    res = nullptr;
    uint32_t w = 0, h = 0, fmt = 0;
};
Surface  g_surfaces[kMaxSurfaces];
uint32_t g_surfaceCount = 0;
uint32_t g_surfaceNext = 0;
uint32_t g_evictions = 0;      // ring overwrites + recycled-address drops
bool     g_ringNoted = false;

// The offscreen learner's state for the currently bound target.
uint32_t g_rtvGen = ~0u;
bool     g_rtvKnown = false;
uint32_t g_rtvChecks = 0;
void*    g_rtvRes = nullptr;
uint32_t g_rtvW = 0, g_rtvH = 0, g_rtvFmt = 0;

// Targets asked kChecksPerTarget times without a GUI draw: not asked again
// for kExhaustedRearmFrames, so a shadow map rebound a hundred times a
// frame does not cost a hundred hash queries a frame for the session.
struct Exhausted {
    void*    res = nullptr;
    uint32_t frame = 0;
};
Exhausted g_exhausted[kExhausted];
uint32_t  g_exhaustedNext = 0;

// The bound depth target, judged once per frame per view: is it the scene
// pair the pass reads?
const void* g_dsvJudged = nullptr;
uint32_t    g_dsvJudgedFrame = ~0u;
bool        g_dsvIsScene = false;

// Families seen, for the one-line-each log.
uint64_t g_familyLogged[kMaxFamilyLines];
uint32_t g_familyLoggedCount = 0;

// The game's depth-stencil state and its writing twin. The game's is held
// (AddRef) so its pointer stays a valid key for as long as the twin lives.
struct StatePair {
    ID3D11DepthStencilState* game = nullptr;
    ID3D11DepthStencilState* ours = nullptr;
    bool                     alreadyWrites = false;  // twin unneeded
};
StatePair g_states[kMaxStates];
uint32_t  g_stateCount = 0;
bool      g_statesFullNoted = false;
bool      g_createFailedNoted = false;

// THE ALPHA-AWARE DEPTH PASS, for a composite drawn through the interface
// projection. Its own draw is left exactly as the game issued it; a second
// draw of the same geometry follows with no colour target, EDVR's pixel
// shader in place of the game's -- the same surface sample the game's
// takes, clipped below the alpha floor, so the dialog's box and its text
// write depth and its 40% scrim over the ship model does not -- the
// pass's depth for the eye bound where the composite's own is not it,
// the game's nearer-wins test (a model in front of the screen keeps its
// depth), and the viewport depth range converting the encoding. The pixel
// shaders are transcriptions of the game's alpha path, from the dumps of
// 2026-09-06: ps 9107E72CB016CC02 samples the surface at TEXCOORD6 through
// slot 1; ps 85565E9261812E2F at TEXCOORD0 through slot 0.
const char kPanelDepthHlsl[] =
    "Texture2D<float4> Surf : register(t1);\n"
    "SamplerState Smp : register(s1);\n"
    "cbuffer P : register(b13) { float4 floorAndPad; };\n"
    "struct In {\n"
    "    float4 tc0 : TEXCOORD0;\n"
    "    float3 tc2 : TEXCOORD2;\n"
    "    float3 tc4 : TEXCOORD4;\n"
    "    float3 tc5 : TEXCOORD5;\n"
    "    float2 tc6 : TEXCOORD6;\n"
    "};\n"
    "void main(In i) {\n"
    "    float a = Surf.Sample(Smp, i.tc6).a;\n"
    "    clip(a - floorAndPad.x);\n"
    "}\n";
const char kScreenDepthHlsl[] =
    "Texture2D<float4> Surf : register(t0);\n"
    "SamplerState Smp : register(s0);\n"
    "cbuffer P : register(b13) { float4 floorAndPad; };\n"
    "struct In { float2 tc0 : TEXCOORD0; };\n"
    "void main(In i) {\n"
    "    float a = Surf.Sample(Smp, i.tc0).a;\n"
    "    clip(a - floorAndPad.x);\n"
    "}\n";

struct DepthShader {
    uint64_t            ps = 0;         // the game's pixel shader it stands in for
    const char*         hlsl = nullptr;
    size_t              len = 0;
    const char*         name = nullptr;
    ID3D11PixelShader*  shader = nullptr;
    bool                tried = false;
};
DepthShader g_depthShaders[2] = {
    {kPanelPs, kPanelDepthHlsl, sizeof(kPanelDepthHlsl) - 1, "ui_depth_panel_ps", nullptr, false},
    {kScreenPs, kScreenDepthHlsl, sizeof(kScreenDepthHlsl) - 1, "ui_depth_screen_ps", nullptr, false},
};
ID3D11Buffer* g_floorCb = nullptr;
float         g_floorCbValue = -1.0f;
ID3D11DepthStencilState* g_reissueDss = nullptr;   // GEQUAL, write all
bool          g_reissueDssFailedNoted = false;

// The pass's depth view for a rebind: EDVR's own view over the probe's
// texture, in the format the game binds it with; one per eye, remade when
// the pair moves.
constexpr uint32_t kMaxRtvs = 8;
struct PairView {
    ID3D11Texture2D*         tex = nullptr;   // identity only, the probe holds the reference
    ID3D11DepthStencilView*  dsv = nullptr;   // ours, over that texture
};
PairView g_pair[2];
bool     g_pairCreateFailedNoted = false;
bool     g_rebindNoted = false;
// The eye a treated draw belongs to, by the ORDER its colour target first
// appears in the frame among treated draws: the first is the left, the
// depth probe's own convention for the pair (first bound = left).
void*    g_frameRtv[2] = {nullptr, nullptr};
uint32_t g_frameRtvCount = 0;

// Per draw: what the classification decided, consumed by Begin/End and by
// the re-issue.
enum class Mode { kNone, kInPlace, kReissue };
Mode                     g_mode = Mode::kNone;
bool                     g_engaged = false;
ID3D11DepthStencilState* g_savedDss = nullptr;
UINT                     g_savedRef = 0;
bool                     g_wantRebind = false;
uint32_t                 g_rebindW = 0, g_rebindH = 0;
int                      g_rebindEye = -1;
bool                     g_rebound = false;      // the OM was swapped for this draw
ID3D11RenderTargetView*  g_savedRtvs[kMaxRtvs] = {};
ID3D11DepthStencilView*  g_savedDsv = nullptr;
DepthShader*             g_reissueShader = nullptr;
ID3D11PixelShader*       g_savedPs = nullptr;
bool                     g_reissueOn = false;

// Counters: this window, and the session.
uint32_t g_wComposite = 0, g_wDirect = 0, g_wWrote = 0, g_wDepthless = 0,
         g_wNotScene = 0, g_wRebound = 0, g_wNoPair = 0, g_wReissued = 0,
         g_wNoShader = 0, g_wAlreadyWrote = 0, g_wNoTwin = 0, g_wLearned = 0,
         g_wFrames = 0;
uint64_t g_sessionWrote = 0;

void resetWindow() {
    g_wComposite = g_wDirect = g_wWrote = g_wDepthless = g_wNotScene = 0;
    g_wRebound = g_wNoPair = g_wReissued = g_wNoShader = 0;
    g_wAlreadyWrote = g_wNoTwin = g_wLearned = 0;
    g_wFrames = 0;
}

int surfaceIndex(const void* res) {
    for (uint32_t i = 0; i < g_surfaceCount; ++i) {
        if (g_surfaces[i].res == res) return static_cast<int>(i);
    }
    return -1;
}

// A resolved texture: a learned surface, or an address a surface once had
// that now holds something else (dropped, counted).
bool surfaceMatches(const ResourceInfo& info) {
    const int i = surfaceIndex(info.resource);
    if (i < 0) return false;
    Surface& s = g_surfaces[i];
    if (s.w == info.a && s.h == info.b && s.fmt == info.fmt) return true;
    s = g_surfaces[g_surfaceCount - 1];
    g_surfaces[g_surfaceCount - 1] = Surface();
    --g_surfaceCount;
    if (g_surfaceNext >= g_surfaceCount) g_surfaceNext = 0;
    ++g_evictions;
    return false;
}

void addSurface(const void* res, uint32_t w, uint32_t h, uint32_t fmt) {
    if (surfaceIndex(res) >= 0) return;
    Surface s;
    s.res = const_cast<void*>(res);
    s.w = w;
    s.h = h;
    s.fmt = fmt;
    if (g_surfaceCount < kMaxSurfaces) {
        g_surfaces[g_surfaceCount++] = s;
    } else {
        g_surfaces[g_surfaceNext] = s;
        g_surfaceNext = (g_surfaceNext + 1) % kMaxSurfaces;
        ++g_evictions;
        if (!g_ringNoted) {
            g_ringNoted = true;
            Log::get().note("ui depth: more than %u interface surfaces learned; the "
                            "oldest are forgotten from here on and their panels "
                            "stop writing depth until learned again.",
                            kMaxSurfaces);
        }
    }
    ++g_wLearned;
}

bool inList(const uint64_t* list, uint32_t n, uint64_t h) {
    for (uint32_t i = 0; i < n; ++i) {
        if (list[i] == h) return true;
    }
    return false;
}

// The bound vertex shader's hash: one COM call, then the memo instead of
// the registry's lock on every draw.
uint64_t boundVsHash(ID3D11DeviceContext* ctx) {
    uint64_t h = 0;
    guardedBudget(g_budget, [&] {
        ID3D11VertexShader* vs = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        if (!vs) return;
        uint64_t memo = 0;
        if (g_vsMemo.get(vs, g_frame, kVsMemoLifeFrames, &memo)) {
            h = memo;
        } else {
            h = lookupShaderHash(vs);
            g_vsMemo.put(vs, g_frame, h);
        }
        vs->Release();
    });
    return h;
}

// The bound pixel shader's hash, the same way; asked only of composites.
uint64_t boundPsHash(ID3D11DeviceContext* ctx) {
    uint64_t h = 0;
    guardedBudget(g_budget, [&] {
        ID3D11PixelShader* ps = nullptr;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        if (!ps) return;
        uint64_t memo = 0;
        if (g_psMemo.get(ps, g_frame, kVsMemoLifeFrames, &memo)) {
            h = memo;
        } else {
            h = lookupShaderHash(ps);
            g_psMemo.put(ps, g_frame, h);
        }
        ps->Release();
    });
    return h;
}

// Is this sampled view over a learned surface? Memoised by view identity;
// a miss resolves the view (guarded, binding_shadow's budget) while it is
// certainly bound.
bool viewIsSurface(const void* view) {
    if (!view) return false;
    uint64_t memo = 0;
    if (g_viewMemo.get(view, g_frame, kMemoLifeFrames, &memo)) return memo != 0;
    ResourceInfo info;
    const bool surface = bindingResolve(const_cast<void*>(view), &info) &&
                         info.isTexture2D && surfaceMatches(info);
    g_viewMemo.put(view, g_frame, surface ? 1u : 0u);
    return surface;
}

// Is the bound depth target the scene pair's? Once per frame per view.
bool dsvIsSceneDepth(const void* dsv) {
    if (dsv == g_dsvJudged && g_dsvJudgedFrame == g_frame) return g_dsvIsScene;
    g_dsvJudged = dsv;
    g_dsvJudgedFrame = g_frame;
    ResourceInfo info;
    g_dsvIsScene = bindingResolve(const_cast<void*>(dsv), &info) && info.isTexture2D &&
                   depthProbeIsSceneDepth(info.resource);
    return g_dsvIsScene;
}

bool exhaustedRecently(const void* res) {
    for (uint32_t i = 0; i < kExhausted; ++i) {
        if (g_exhausted[i].res == res) {
            return g_frame - g_exhausted[i].frame < kExhaustedRearmFrames;
        }
    }
    return false;
}

void noteExhausted(const void* res) {
    for (uint32_t i = 0; i < kExhausted; ++i) {
        if (g_exhausted[i].res == res) {
            g_exhausted[i].frame = g_frame;
            return;
        }
    }
    g_exhausted[g_exhaustedNext].res = const_cast<void*>(res);
    g_exhausted[g_exhaustedNext].frame = g_frame;
    g_exhaustedNext = (g_exhaustedNext + 1) % kExhausted;
}

// A family seen for the first time: one line with its target, the
// visibility the header promises, so a field log can say what was treated.
void noteFamily(uint64_t vh, uint64_t ph, const char* how) {
    if (!vh || g_familyLoggedCount >= kMaxFamilyLines) return;
    if (inList(g_familyLogged, g_familyLoggedCount, vh)) return;
    g_familyLogged[g_familyLoggedCount++] = vh;
    ResourceInfo rt;
    const bool haveRt = bindingResolve(bindingGet(BindSlot::Rtv0), &rt) && rt.isTexture2D;
    Log::get().note("ui depth: a new interface family -- vs %016llX ps %016llX draws "
                    "into %ux%u DXGI format %u: %s.",
                    static_cast<unsigned long long>(vh),
                    static_cast<unsigned long long>(ph),
                    haveRt ? rt.a : 0u, haveRt ? rt.b : 0u, haveRt ? rt.fmt : 0u, how);
}

enum class TwinWhy { kOk, kAlreadyWrites, kNoTwin };

// The writing twin of the game's depth-stencil state. Null with the reason
// when the game already writes (nothing to do) or no twin can be made (the
// draw stays as the game issued it).
ID3D11DepthStencilState* writingTwin(ID3D11DeviceContext* ctx,
                                     ID3D11DepthStencilState* game, TwinWhy* why) {
    // A null state is D3D's default: depth on, LESS, write all. Writes.
    if (!game) {
        *why = TwinWhy::kAlreadyWrites;
        return nullptr;
    }
    for (uint32_t i = 0; i < g_stateCount; ++i) {
        if (g_states[i].game != game) continue;
        *why = g_states[i].alreadyWrites ? TwinWhy::kAlreadyWrites : TwinWhy::kOk;
        return g_states[i].ours;
    }
    // Room first, then the object: a full table must not create and
    // destroy a state per draw for the session (review finding 10).
    if (g_stateCount >= kMaxStates) {
        if (!g_statesFullNoted) {
            g_statesFullNoted = true;
            Log::get().note("ui depth: more than %u distinct depth states at "
                            "interface draws; the ones past the table are left as "
                            "the game issued them.",
                            kMaxStates);
        }
        *why = TwinWhy::kNoTwin;
        return nullptr;
    }
    D3D11_DEPTH_STENCIL_DESC d{};
    game->GetDesc(&d);
    const bool writes = d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL &&
                        !g_testAlways;
    ID3D11DepthStencilState* ours = nullptr;
    if (!writes) {
        // The game's test kept where it had one (the cockpit occludes its
        // panels); ALWAYS where it had none, or under the instrument.
        // Stencil exactly as the game's.
        if (!d.DepthEnable || g_testAlways) {
            d.DepthEnable = TRUE;
            d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        }
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) {
            *why = TwinWhy::kNoTwin;
            return nullptr;
        }
        const HRESULT hr = dev->CreateDepthStencilState(&d, &ours);
        dev->Release();
        if (FAILED(hr) || !ours) {
            if (!g_createFailedNoted) {
                g_createFailedNoted = true;
                Log::get().note("ui depth: CreateDepthStencilState failed (0x%08lX); "
                                "the draws that need that state stay as the game "
                                "issued them.",
                                static_cast<unsigned long>(hr));
            }
            *why = TwinWhy::kNoTwin;
            return nullptr;
        }
    }
    game->AddRef();
    StatePair& p = g_states[g_stateCount++];
    p.game = game;
    p.ours = ours;
    p.alreadyWrites = writes;
    *why = writes ? TwinWhy::kAlreadyWrites : TwinWhy::kOk;
    return ours;
}

// The re-issue's depth state: the nearer wins, so a ship model in front of
// the loader's screen keeps its depth; stencil left off.
ID3D11DepthStencilState* reissueState(ID3D11DeviceContext* ctx) {
    if (g_reissueDss) return g_reissueDss;
    if (g_reissueDssFailedNoted) return nullptr;
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc = g_testAlways ? D3D11_COMPARISON_ALWAYS : D3D11_COMPARISON_GREATER_EQUAL;
    d.StencilEnable = FALSE;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    const HRESULT hr = dev->CreateDepthStencilState(&d, &g_reissueDss);
    dev->Release();
    if (FAILED(hr) || !g_reissueDss) {
        g_reissueDss = nullptr;
        g_reissueDssFailedNoted = true;
        Log::get().note("ui depth: the depth pass's state could not be made "
                        "(0x%08lX); interface-projection composites stay as the "
                        "game issued them.",
                        static_cast<unsigned long>(hr));
        return nullptr;
    }
    return g_reissueDss;
}

DepthShader* depthShaderFor(ID3D11DeviceContext* ctx, uint64_t ps) {
    for (DepthShader& s : g_depthShaders) {
        if (s.ps != ps) continue;
        if (!s.shader && !s.tried) {
            s.tried = true;
            s.shader = shaderSwapCompilePs(ctx, s.hlsl, s.len, "main", s.name, nullptr,
                                           "ui depth");
        }
        return s.shader ? &s : nullptr;
    }
    return nullptr;
}

// The alpha floor, in a constant buffer of EDVR's at b13 (a slot the
// game's composites leave empty: their pixel stages declare b2 alone).
ID3D11Buffer* floorBuffer(ID3D11DeviceContext* ctx) {
    if (g_floorCb && g_floorCbValue == g_alphaFloor) return g_floorCb;
    if (g_floorCb) {
        g_floorCb->Release();
        g_floorCb = nullptr;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    const float data[4] = {g_alphaFloor, 0.0f, 0.0f, 0.0f};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(data);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = data;
    const HRESULT hr = dev->CreateBuffer(&bd, &sd, &g_floorCb);
    dev->Release();
    if (FAILED(hr)) g_floorCb = nullptr;
    g_floorCbValue = g_alphaFloor;
    return g_floorCb;
}

void parseHashes(const std::string& spec, uint64_t* out, uint32_t* count,
                 uint32_t cap, const char* key) {
    *count = 0;
    const char* p = spec.c_str();
    while (*p && *count < cap) {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        if (!*p) break;
        char* end = nullptr;
        const uint64_t h = _strtoui64(p, &end, 16);
        if (end == p || h == 0) {
            Log::get().note("ui depth: %s holds \"%s\", which is not a list of "
                            "sixteen-hex-digit hashes; ignored from there on.",
                            key, p);
            break;
        }
        out[(*count)++] = h;
        p = end;
    }
}

void releaseStates() {
    for (uint32_t i = 0; i < g_stateCount; ++i) {
        if (g_states[i].ours) g_states[i].ours->Release();
        if (g_states[i].game) g_states[i].game->Release();
        g_states[i] = StatePair();
    }
    g_stateCount = 0;
    g_statesFullNoted = false;
    if (g_reissueDss) {
        g_reissueDss->Release();
        g_reissueDss = nullptr;
    }
    g_reissueDssFailedNoted = false;
}

void releasePairViews() {
    for (int e = 0; e < 2; ++e) {
        if (g_pair[e].dsv) g_pair[e].dsv->Release();
        g_pair[e] = PairView();
    }
}

// EDVR's depth view over the pass's texture for this eye, made once per
// texture. Null when there is no pair or the view cannot be made.
ID3D11DepthStencilView* pairViewFor(ID3D11DeviceContext* ctx, int eye, uint32_t w,
                                    uint32_t h) {
    ID3D11Texture2D* tex = nullptr;
    uint32_t fmt = 0;
    if (!depthProbeSceneDepthFormat(w, h, eye, &tex, &fmt) || !tex) return nullptr;
    PairView& p = g_pair[eye];
    if (p.tex == tex && p.dsv) return p.dsv;
    if (p.dsv) {
        p.dsv->Release();
        p.dsv = nullptr;
    }
    p.tex = tex;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    D3D11_DEPTH_STENCIL_VIEW_DESC d{};
    d.Format = static_cast<DXGI_FORMAT>(fmt);
    d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipSlice = 0;
    const HRESULT hr = dev->CreateDepthStencilView(tex, &d, &p.dsv);
    dev->Release();
    if (FAILED(hr) || !p.dsv) {
        p.dsv = nullptr;
        if (!g_pairCreateFailedNoted) {
            g_pairCreateFailedNoted = true;
            Log::get().note("ui depth: a depth view over the pass's %ux%u target "
                            "(format %u) could not be made (0x%08lX); composites "
                            "outside the scene's depth stay as the game issued them.",
                            w, h, fmt, static_cast<unsigned long>(hr));
        }
        return nullptr;
    }
    return p.dsv;
}

int eyeIndexFor(const void* rtvRes) {
    for (uint32_t i = 0; i < g_frameRtvCount; ++i) {
        if (g_frameRtv[i] == rtvRes) return static_cast<int>(i);
    }
    if (g_frameRtvCount >= 2) return -1;
    g_frameRtv[g_frameRtvCount] = const_cast<void*>(rtvRes);
    return static_cast<int>(g_frameRtvCount++);
}

// The viewport depth range that carries the encoding correction, and its
// undoing. Through the context's own entry: vscreen's hook touches only
// inflated FSS targets.
void scaleViewportsForUi(ID3D11DeviceContext* ctx) {
    float sceneNear = 0.0f, sceneFar = 0.0f;
    if (!temporalPassPlanes(&sceneNear, &sceneFar) || !(g_uiNear > 0.0f)) return;
    float scale = sceneNear / g_uiNear;
    if (!(scale > 0.0f)) return;
    if (scale > 1.0f) scale = 1.0f;
    g_savedVpCount = kMaxViewports;
    ctx->RSGetViewports(&g_savedVpCount, g_savedVps);
    if (g_savedVpCount == 0 || g_savedVpCount > kMaxViewports) {
        g_savedVpCount = 0;
        return;
    }
    D3D11_VIEWPORT scaled[kMaxViewports];
    for (UINT i = 0; i < g_savedVpCount; ++i) {
        scaled[i] = g_savedVps[i];
        scaled[i].MaxDepth = scaled[i].MinDepth + (scaled[i].MaxDepth - scaled[i].MinDepth) * scale;
    }
    ctx->RSSetViewports(g_savedVpCount, scaled);
    g_vpScaled = true;
    if (!g_scaleNoted) {
        g_scaleNoted = true;
        Log::get().note("ui depth: an interface-projection composite's depth is written "
                        "through a viewport depth range of %.4f -- the interface "
                        "projection's near %g m against the scene's %g m -- so its "
                        "reversed-Z value decodes in the scene's encoding "
                        "(advanced.ui_depth_planes).",
                        static_cast<double>(scale), static_cast<double>(g_uiNear),
                        static_cast<double>(sceneNear));
    }
}

void restoreViewports(ID3D11DeviceContext* ctx) {
    if (!g_vpScaled) return;
    g_vpScaled = false;
    if (g_savedVpCount) ctx->RSSetViewports(g_savedVpCount, g_savedVps);
    g_savedVpCount = 0;
}

void releaseSavedOm() {
    for (uint32_t i = 0; i < kMaxRtvs; ++i) {
        if (g_savedRtvs[i]) g_savedRtvs[i]->Release();
        g_savedRtvs[i] = nullptr;
    }
    if (g_savedDsv) g_savedDsv->Release();
    g_savedDsv = nullptr;
}

uint32_t savedRtvCount() {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kMaxRtvs; ++i) {
        if (g_savedRtvs[i]) n = i + 1;
    }
    return n;
}

void restoreOm(ID3D11DeviceContext* ctx) {
    vScreenSetRenderTargetsRaw(ctx, savedRtvCount(), g_savedRtvs, g_savedDsv);
}

}  // namespace

void uiDepthConfigure(Config& cfg) {
    const std::string mode = cfg.getString("fix.ui_depth", "on");
    g_keyOn = mode == "on";
    if (!g_keyOn && mode != "off") {
        Log::get().note("ui depth: fix.ui_depth = \"%s\" is not on or off; off.",
                        mode.c_str());
    }
    // The gate: nothing to register without the pass. Re-read on every
    // configure because fix.temporal_aa is live (depth_probe.cpp's rule).
    const std::string aa = cfg.getString("fix.temporal_aa", "off");
    g_passOn = !aa.empty() && _stricmp(aa.c_str(), "off") != 0;
    // The direct list: the flight HUD built in, the ini's additions after.
    g_familyCount = 0;
    g_families[g_familyCount++] = kFlightHud;
    uint64_t extra[kMaxHashes];
    uint32_t extraCount = 0;
    parseHashes(cfg.getString("advanced.ui_depth_families", ""), extra, &extraCount,
                kMaxHashes, "advanced.ui_depth_families");
    for (uint32_t i = 0; i < extraCount && g_familyCount < kMaxHashes; ++i) {
        if (!inList(g_families, g_familyCount, extra[i])) g_families[g_familyCount++] = extra[i];
    }
    // The exclude list: the null-shader mesh built in, the ini's after.
    g_excludeCount = 0;
    g_exclude[g_excludeCount++] = kNullPsMesh;
    parseHashes(cfg.getString("advanced.ui_depth_exclude", ""), extra, &extraCount,
                kMaxHashes, "advanced.ui_depth_exclude");
    for (uint32_t i = 0; i < extraCount && g_excludeCount < kMaxHashes; ++i) {
        if (!inList(g_exclude, g_excludeCount, extra[i])) g_exclude[g_excludeCount++] = extra[i];
    }
    {
        // The interface projection's planes: "near, far" in metres.
        const std::string planes = cfg.getString("advanced.ui_depth_planes", "0.1, 1000");
        float n = 0.0f, f = 0.0f;
        char* end = nullptr;
        n = static_cast<float>(strtod(planes.c_str(), &end));
        while (end && (*end == ' ' || *end == ',' || *end == '\t')) ++end;
        if (end && *end) f = static_cast<float>(strtod(end, nullptr));
        if (n > 0.0f && f > n) {
            if (n != g_uiNear || f != g_uiFar) {
                g_uiNear = n;
                g_uiFar = f;
                g_scaleNoted = false;
            }
        } else if (planes != "0.1, 1000") {
            Log::get().note("ui depth: advanced.ui_depth_planes = \"%s\" is not "
                            "\"near, far\" in metres with far beyond near; the "
                            "interface projection is taken as 0.1..1000 m.",
                            planes.c_str());
            g_uiNear = 0.1f;
            g_uiFar = 1000.0f;
        }
    }
    {
        float a = cfg.getFloat("advanced.ui_depth_alpha", 0.5f);
        if (!(a >= 0.0f)) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        g_alphaFloor = a;
    }
    const std::string eyes = cfg.getString("advanced.ui_depth_eyes", "as_is");
    const bool swapped = eyes == "swapped";
    if (swapped != g_eyesSwapped) {
        g_eyesSwapped = swapped;
        Log::get().note("ui depth: the eye a rebound composite belongs to is %s.",
                        swapped ? "the REVERSE of its colour target's order in the "
                                  "frame (advanced.ui_depth_eyes = swapped)"
                                : "its colour target's order in the frame, first "
                                  "= left");
    }
    const bool menus = cfg.getBool("advanced.ui_depth_menus", true);
    if (menus != g_menus) {
        g_menus = menus;
        Log::get().note("ui depth: interface-projection composites (the menus, the "
                        "loading screen, the modals) %s.",
                        menus ? "get the alpha-aware depth pass"
                              : "are left alone (advanced.ui_depth_menus = 0)");
    }
    // The test instrument changes what a twin is, so the twins are remade.
    const std::string test = cfg.getString("advanced.ui_depth_test", "as_is");
    const bool always = test == "always";
    if (always != g_testAlways) {
        g_testAlways = always;
        releaseStates();
        Log::get().note("ui depth: the depth test at interface draws is %s.",
                        always ? "ALWAYS (advanced.ui_depth_test = always: the "
                                 "ordering A/B; the cockpit no longer occludes a panel)"
                               : "the game's own");
    }

    const bool was = g_on;
    g_on = g_keyOn && g_passOn;
    if (g_on && !was) {
        g_announced = false;
        g_waitingNoted = false;
        resetWindow();
        Log::get().note("ui depth: ON -- the interface's composites and the flight HUD "
                        "will write their depth for the temporal pass (%u direct "
                        "famil%s, %u excluded, alpha floor %.2f). It says so again "
                        "when the first frame writes.",
                        g_familyCount, g_familyCount == 1 ? "y" : "ies",
                        g_excludeCount, static_cast<double>(g_alphaFloor));
    } else if (!g_on && was) {
        Log::get().note("ui depth: off%s. The interface draws as the game issues it.",
                        g_keyOn ? " while temporal_aa is off" : "");
    } else if (g_keyOn && !g_passOn && !g_waitingNoted) {
        g_waitingNoted = true;
        Log::get().note("ui depth: on, but temporal_aa is off, so it waits -- there "
                        "is nothing to register without the pass, and the game's "
                        "depth is left exactly as it is.");
    }
    // Deliberately NOT re-arming after a stand-down: the budget that stood
    // it down is spent for the session (review finding 15).
}

bool uiDepthWantsDraws() { return g_on && !g_stoodDown; }

void uiDepthNoteOffscreenDraw(ID3D11DeviceContext* ctx) {
    if (!g_on || g_stoodDown) return;
    const uint32_t gen = bindingGeneration(BindSlot::Rtv0);
    if (gen != g_rtvGen) {
        g_rtvGen = gen;
        g_rtvKnown = false;
        g_rtvChecks = 0;
        g_rtvRes = nullptr;
        ResourceInfo info;
        if (bindingResolve(bindingGet(BindSlot::Rtv0), &info) && info.isTexture2D) {
            g_rtvRes = info.resource;
            g_rtvW = info.a;
            g_rtvH = info.b;
            g_rtvFmt = info.fmt;
            g_rtvKnown = surfaceMatches(info);
            if (!g_rtvKnown && exhaustedRecently(g_rtvRes)) g_rtvChecks = kChecksPerTarget;
        } else {
            g_rtvChecks = kChecksPerTarget;   // nothing here to learn
        }
    }
    if (g_rtvKnown || !g_rtvRes || g_rtvChecks >= kChecksPerTarget) return;
    ++g_rtvChecks;
    const uint64_t h = boundVsHash(ctx);
    if (h == kGuiVector || h == kGuiText || h == kGuiIcons) {
        addSurface(g_rtvRes, g_rtvW, g_rtvH, g_rtvFmt);
        g_rtvKnown = true;
    } else if (g_rtvChecks >= kChecksPerTarget) {
        noteExhausted(g_rtvRes);
    }
}

bool uiDepthOnEyeDraw(ID3D11DeviceContext* ctx) {
    g_mode = Mode::kNone;
    g_wantRebind = false;
    g_rebindEye = -1;
    g_reissueShader = nullptr;
    if (!g_on || g_stoodDown) return false;
    // Cheapest first: no depth target, nothing to write (the post chain's
    // fullscreen draws, ten a frame).
    const void* dsv = bindingGet(BindSlot::Dsv0);
    if (!dsv) {
        ++g_wDepthless;
        return false;
    }
    bool composite = false;
    static const BindSlot kSlots[4] = {BindSlot::PsSrv0, BindSlot::PsSrv1,
                                       BindSlot::PsSrv2, BindSlot::PsSrv3};
    for (const BindSlot slot : kSlots) {
        if (viewIsSurface(bindingGet(slot))) {
            composite = true;
            break;
        }
    }
    // The hash: for a composite, the family line and the exclude list
    // (a couple of dozen a frame); otherwise the direct list, which is the
    // only test left for the other draws.
    const uint64_t h = boundVsHash(ctx);
    if (!composite && !(h && inList(g_families, g_familyCount, h))) return false;
    if (h && inList(g_exclude, g_excludeCount, h)) return false;

    // WHICH PROJECTION. The cockpit's families bind the scene pair and
    // test against it, so they share the scene's projection and write
    // depth in place. A composite that does not bind the pair is drawn
    // through the interface projection (the menus, the loader, the modals;
    // measured 2026-09-07) and gets the alpha-aware depth pass instead,
    // whichever depth it binds.
    const bool scenePair = dsvIsSceneDepth(dsv);
    const bool sceneFamily = h == kHoloPanel || inList(g_families, g_familyCount, h);
    if (scenePair && sceneFamily) {
        g_mode = Mode::kInPlace;
        if (g_familyLoggedCount < kMaxFamilyLines && !inList(g_familyLogged, g_familyLoggedCount, h)) {
            noteFamily(h, boundPsHash(ctx),
                       composite ? "samples a learned surface; writes its depth in "
                                   "place, the scene's own encoding"
                                 : "named direct family; writes its depth in place");
        }
        if (composite) {
            ++g_wComposite;
        } else {
            ++g_wDirect;
        }
        return true;
    }
    if (!composite) return false;   // a direct family off the scene pair: left alone
    if (!g_menus) {
        ++g_wNotScene;
        return false;
    }
    // The alpha-aware pass needs a shader for this family's pixel stage,
    // the scene's planes for the encoding, and the pass's depth for the
    // eye when the composite's own is not it.
    const uint64_t ph = boundPsHash(ctx);
    DepthShader* shader = depthShaderFor(ctx, ph);
    if (!shader) {
        ++g_wNoShader;
        noteFamily(h, ph, "samples a learned surface but has no depth shader of "
                          "its own yet; left alone");
        return false;
    }
    float sn = 0.0f, sf = 0.0f;
    if (!temporalPassPlanes(&sn, &sf)) {
        ++g_wNoPair;
        return false;
    }
    if (!scenePair) {
        ResourceInfo rt;
        if (!bindingResolve(bindingGet(BindSlot::Rtv0), &rt) || !rt.isTexture2D) {
            ++g_wNoPair;
            return false;
        }
        int eye = eyeIndexFor(rt.resource);
        if (eye >= 0 && g_eyesSwapped) eye = 1 - eye;
        ID3D11Texture2D* tex = nullptr;
        uint32_t fmt = 0;
        if (eye < 0 || !depthProbeSceneDepthFormat(rt.a, rt.b, eye, &tex, &fmt)) {
            ++g_wNoPair;
            return false;
        }
        g_wantRebind = true;
        g_rebindEye = eye;
        g_rebindW = rt.a;
        g_rebindH = rt.b;
    }
    g_mode = Mode::kReissue;
    g_reissueShader = shader;
    noteFamily(h, ph, scenePair ? "interface projection; its depth written by the "
                                  "alpha-aware pass into its own target, the pass's"
                                : "interface projection; its depth written by the "
                                  "alpha-aware pass into the pass's target for its eye");
    ++g_wComposite;
    return true;
}

bool uiDepthWantsReissue() { return g_mode == Mode::kReissue && g_reissueShader != nullptr; }

void uiDepthBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    if (!g_on || g_stoodDown || g_mode != Mode::kInPlace) return;
    const bool ran = guardedBudget(g_budget, [&] {
        ID3D11DepthStencilState* game = nullptr;
        UINT ref = 0;
        ctx->OMGetDepthStencilState(&game, &ref);
        TwinWhy why = TwinWhy::kNoTwin;
        ID3D11DepthStencilState* ours = writingTwin(ctx, game, &why);
        if (!ours) {
            if (game) game->Release();
            if (why == TwinWhy::kAlreadyWrites) {
                ++g_wAlreadyWrote;
            } else {
                ++g_wNoTwin;
            }
            return;
        }
        g_savedDss = game;   // the AddRef from OMGet, released at End
        g_savedRef = ref;
        ctx->OMSetDepthStencilState(ours, ref);
        g_engaged = true;
        ++g_wWrote;
        ++g_sessionWrote;
    });
    if (!ran && !g_budget.shouldRun() && !g_stoodDown) {
        g_stoodDown = true;
        Log::get().note("ui depth: STANDING DOWN for the session -- the depth "
                        "state swap faulted repeatedly. The interface draws as "
                        "the game issues it.");
    }
}

void uiDepthEnd(ID3D11DeviceContext* ctx) {
    // Under SEH but NOT under the budget: a budget spent between Begin and
    // End would skip a guardedBudget body, and a restore skipped leaves our
    // writing twin on every later draw in the frame (review finding 1;
    // resolve_probe.cpp restores the same way for the same reason).
    if (g_engaged) {
        g_engaged = false;
        guarded("uiDepth.restore", [&] { ctx->OMSetDepthStencilState(g_savedDss, g_savedRef); });
        if (g_savedDss) {
            g_savedDss->Release();
            g_savedDss = nullptr;
        }
    }
    g_mode = Mode::kNone;
}

// True when the second draw is set up to write DEPTH ONLY. False means
// this call declined, and the caller must NOT issue the draw: every
// decline leaves the game's own state exactly as it was, so a draw issued
// anyway is the game's composite a second time, in full colour, over
// itself. The paths that decline -- no depth-stencil state or constant
// buffer, and no pair view to rebind to -- are latched by their own
// one-shot notes, so once one starts failing it fails for every composite
// after, and the doubling would last the session (the pre-release review
// of 2026-09-07). splashDimBegin below has taken this shape all along.
bool uiDepthReissueBegin(ID3D11DeviceContext* ctx) {
    g_reissueOn = false;
    g_rebound = false;
    if (!g_on || g_stoodDown || g_mode != Mode::kReissue || !g_reissueShader) return false;
    const bool rebind = g_wantRebind;
    g_wantRebind = false;
    DepthShader* shader = g_reissueShader;
    const bool ran = guardedBudget(g_budget, [&] {
        ID3D11DepthStencilState* dss = reissueState(ctx);
        ID3D11Buffer* cb = floorBuffer(ctx);
        if (!dss || !cb) {
            ++g_wNoTwin;
            return;
        }
        // The colour target off, the depth target the pass's when the
        // composite's own is not it; through the original entry so the
        // binding shadow keeps describing the game's bindings.
        ctx->OMGetRenderTargets(kMaxRtvs, g_savedRtvs, &g_savedDsv);
        ID3D11DepthStencilView* target = g_savedDsv;
        if (rebind) {
            target = pairViewFor(ctx, g_rebindEye, g_rebindW, g_rebindH);
            if (!target) {
                releaseSavedOm();
                ++g_wNoPair;
                return;
            }
            ++g_wRebound;
            if (!g_rebindNoted) {
                g_rebindNoted = true;
                Log::get().note("ui depth: a composite whose own depth is not the pass's "
                                "has its depth pass bound to the pass's %ux%u depth "
                                "(eye %d by the order its colour target appeared this "
                                "frame); the game's bindings are put back after.",
                                g_rebindW, g_rebindH, g_rebindEye);
            }
        }
        vScreenSetRenderTargetsRaw(ctx, 0, nullptr, target);
        g_rebound = true;
        ctx->OMGetDepthStencilState(&g_savedDss, &g_savedRef);
        ctx->OMSetDepthStencilState(dss, 0);
        ctx->PSGetShader(&g_savedPs, nullptr, nullptr);
        ctx->PSSetShader(shader->shader, nullptr, 0);
        ctx->PSSetConstantBuffers(13, 1, &cb);
        scaleViewportsForUi(ctx);
        g_reissueOn = true;
        ++g_wReissued;
        ++g_wWrote;
        ++g_sessionWrote;
    });
    if (!ran && !g_budget.shouldRun() && !g_stoodDown) {
        g_stoodDown = true;
        Log::get().note("ui depth: STANDING DOWN for the session -- the depth pass "
                        "faulted repeatedly. The interface draws as the game issues it.");
    }
    if (!ran && g_rebound) {
        // A fault after the rebind: everything back now rather than at End,
        // which the caller still calls.
        guarded("uiDepth.reissueRestore", [&] {
            restoreViewports(ctx);
            restoreOm(ctx);
        });
        releaseSavedOm();
        g_rebound = false;
        g_reissueOn = false;
    }
    return g_reissueOn;
}

void uiDepthReissueEnd(ID3D11DeviceContext* ctx) {
    if (g_reissueOn) {
        g_reissueOn = false;
        guarded("uiDepth.reissueRestore", [&] {
            ctx->PSSetShader(g_savedPs, nullptr, 0);
            ID3D11Buffer* none = nullptr;
            ctx->PSSetConstantBuffers(13, 1, &none);
            ctx->OMSetDepthStencilState(g_savedDss, g_savedRef);
            restoreViewports(ctx);
        });
        if (g_savedPs) {
            g_savedPs->Release();
            g_savedPs = nullptr;
        }
        if (g_savedDss) {
            g_savedDss->Release();
            g_savedDss = nullptr;
        }
    }
    if (g_rebound) {
        g_rebound = false;
        guarded("uiDepth.reissueRestore", [&] { restoreOm(ctx); });
        releaseSavedOm();
    }
    g_mode = Mode::kNone;
    g_reissueShader = nullptr;
}

void uiDepthFrameBoundary() {
    ++g_frame;
    g_frameRtv[0] = g_frameRtv[1] = nullptr;
    g_frameRtvCount = 0;
    if (!g_on) return;
    ++g_wFrames;
    if (!g_announced && g_wWrote > 0) {
        g_announced = true;
        Log::get().note("ui depth: engaged -- %u interface draws wrote their depth "
                        "this window (%u composites of %u learned surfaces, %u "
                        "direct, %u through the alpha-aware pass), the game's depth "
                        "test kept where it had one.",
                        g_wWrote, g_wComposite, g_surfaceCount, g_wDirect, g_wReissued);
    }
    if (g_wFrames >= kTotalsFrames) {
        if (g_wWrote || g_wNotScene || g_wRebound || g_wNoPair || g_wNoShader ||
            g_wNoTwin || g_wLearned || g_evictions) {
            Log::get().note("ui depth totals: %.1f interface draws a frame wrote depth "
                            "(%.1f composites, %.1f direct; %.1f through the alpha-aware "
                            "pass, %.1f of those bound to the pass's depth); %u surfaces "
                            "known, %u learned this window, %u forgotten; left alone: "
                            "%.1f a frame with no depth shader for their family, %.1f "
                            "with no pair or planes, %.1f by the menus switch, %u already "
                            "writing, %u with no state; %u depth states; %llu written "
                            "this session.",
                            static_cast<double>(g_wWrote) / g_wFrames,
                            static_cast<double>(g_wComposite) / g_wFrames,
                            static_cast<double>(g_wDirect) / g_wFrames,
                            static_cast<double>(g_wReissued) / g_wFrames,
                            static_cast<double>(g_wRebound) / g_wFrames,
                            g_surfaceCount, g_wLearned, g_evictions,
                            static_cast<double>(g_wNoShader) / g_wFrames,
                            static_cast<double>(g_wNoPair) / g_wFrames,
                            static_cast<double>(g_wNotScene) / g_wFrames,
                            g_wAlreadyWrote, g_wNoTwin, g_stateCount,
                            static_cast<unsigned long long>(g_sessionWrote));
        }
        resetWindow();
    }
}

void uiDepthShutdown() {
    if (g_savedDss) {
        g_savedDss->Release();
        g_savedDss = nullptr;
    }
    if (g_savedPs) {
        g_savedPs->Release();
        g_savedPs = nullptr;
    }
    g_engaged = false;
    g_reissueOn = false;
    g_rebound = false;
    g_mode = Mode::kNone;
    releaseSavedOm();
    releasePairViews();
    releaseStates();
    for (DepthShader& s : g_depthShaders) {
        if (s.shader) s.shader->Release();
        s.shader = nullptr;
        s.tried = false;
    }
    if (g_floorCb) {
        g_floorCb->Release();
        g_floorCb = nullptr;
    }
    g_surfaceCount = 0;
    g_surfaceNext = 0;
    g_viewMemo.clear();
    g_vsMemo.clear();
    g_psMemo.clear();
    for (uint32_t i = 0; i < kExhausted; ++i) g_exhausted[i] = Exhausted();
    g_familyLoggedCount = 0;
    g_on = false;
}

}  // namespace edvr
