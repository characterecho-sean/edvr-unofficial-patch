#include "ui_depth.h"

#include <windows.h>

#include <d3d11.h>

#include <cstdint>
#include <cstdlib>   // _strtoui64: the hash lists
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "depth_probe.h"    // depthProbeIsSceneDepth, ...Format: where the pass reads
#include "exposure_fix.h"   // lookupShaderHash
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

constexpr uint32_t kMaxSurfaces = 64;      // a session showed thirteen
constexpr uint32_t kMaxHashes = 16;
constexpr uint32_t kMaxStates = 16;        // the game has a handful of depth states
constexpr uint32_t kMemoSize = 1024;       // a power of two: the probe masks
constexpr uint32_t kMemoProbe = 4;
constexpr uint32_t kMemoLifeFrames = 120;  // a sampled view's answer
constexpr uint32_t kVsMemoLifeFrames = 600; // a vertex shader's hash
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
bool     g_menus = true;       // advanced.ui_depth_menus: bind the pass's depth at
                               // a composite whose own depth nothing reads
bool     g_eyesSwapped = false; // advanced.ui_depth_eyes = swapped: the A/B for
                                // the order rule that names the eye
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

// THE REBIND, for a composite whose own depth target nothing reads (the
// main menu's panel, the loader's dialog; measured 2026-09-06): the pass's
// depth for that eye is bound in its place for the one draw, through the
// original OM entry, and the game's bindings are put back before anything
// else looks. EDVR's own view over the probe's texture, in the format the
// game binds it with; one per eye, remade when the pair moves.
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

// Per draw.
bool                     g_engaged = false;
ID3D11DepthStencilState* g_savedDss = nullptr;
UINT                     g_savedRef = 0;
bool                     g_wantRebind = false;   // decided at classification
uint32_t                 g_rebindW = 0, g_rebindH = 0;
int                      g_rebindEye = -1;
bool                     g_rebound = false;      // the OM was swapped for this draw
ID3D11RenderTargetView*  g_savedRtvs[kMaxRtvs] = {};
ID3D11DepthStencilView*  g_savedDsv = nullptr;

// Counters: this window, and the session.
uint32_t g_wComposite = 0, g_wDirect = 0, g_wWrote = 0, g_wDepthless = 0,
         g_wNotScene = 0, g_wRebound = 0, g_wNoPair = 0, g_wAlreadyWrote = 0,
         g_wNoTwin = 0, g_wLearned = 0, g_wFrames = 0;
uint64_t g_sessionWrote = 0;

void resetWindow() {
    g_wComposite = g_wDirect = g_wWrote = g_wDepthless = g_wNotScene = 0;
    g_wRebound = g_wNoPair = g_wAlreadyWrote = g_wNoTwin = g_wLearned = 0;
    g_wFrames = 0;
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
void noteFamily(uint64_t vh, bool composite) {
    if (!vh || g_familyLoggedCount >= kMaxFamilyLines) return;
    if (inList(g_familyLogged, g_familyLoggedCount, vh)) return;
    g_familyLogged[g_familyLoggedCount++] = vh;
    ResourceInfo rt;
    const bool haveRt = bindingResolve(bindingGet(BindSlot::Rtv0), &rt) && rt.isTexture2D;
    Log::get().note("ui depth: a new interface family -- vs %016llX (%s) draws into "
                    "%ux%u DXGI format %u and now writes its depth.",
                    static_cast<unsigned long long>(vh),
                    composite ? "samples a learned surface" : "named direct family",
                    haveRt ? rt.a : 0u, haveRt ? rt.b : 0u, haveRt ? rt.fmt : 0u);
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
    parseHashes(cfg.getString("advanced.ui_depth_exclude", ""), g_exclude, &g_excludeCount,
                kMaxHashes, "advanced.ui_depth_exclude");
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
        Log::get().note("ui depth: composites whose own depth nothing reads are %s.",
                        menus ? "bound to the pass's depth for the draw (the menus "
                                "and the loading screen)"
                              : "left alone (advanced.ui_depth_menus = 0)");
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
                        "famil%s, %u excluded). It says so again when the first "
                        "frame writes.",
                        g_familyCount, g_familyCount == 1 ? "y" : "ies",
                        g_excludeCount);
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
    // Only where the pass reads: the scene pair. A composite that binds a
    // depth of its own that nothing reads (the main menu's panel, the
    // loader's dialog; measured 2026-09-06) has the pass's depth bound in
    // its place for the draw, for its eye -- when the pair exists and the
    // eye can be told (the first colour target treated in a frame is the
    // left, the probe's own rule).
    g_wantRebind = false;
    g_rebindEye = -1;
    if (!dsvIsSceneDepth(dsv)) {
        if (!g_menus) {
            ++g_wNotScene;
            return false;
        }
        ResourceInfo rt;
        if (!bindingResolve(bindingGet(BindSlot::Rtv0), &rt) || !rt.isTexture2D) {
            ++g_wNotScene;
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
    noteFamily(h, composite);
    if (composite) {
        ++g_wComposite;
    } else {
        ++g_wDirect;
    }
    return true;
}

void releaseSavedOm() {
    for (uint32_t i = 0; i < kMaxRtvs; ++i) {
        if (g_savedRtvs[i]) g_savedRtvs[i]->Release();
        g_savedRtvs[i] = nullptr;
    }
    if (g_savedDsv) g_savedDsv->Release();
    g_savedDsv = nullptr;
}

void uiDepthBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    g_rebound = false;
    if (!g_on || g_stoodDown) return;
    const bool rebind = g_wantRebind;
    g_wantRebind = false;
    const bool ran = guardedBudget(g_budget, [&] {
        // The rebind first, so the twin is derived against the state the
        // draw will run with (the same state: the OM swap changes no
        // depth-stencil state).
        if (rebind) {
            ID3D11DepthStencilView* ours = pairViewFor(ctx, g_rebindEye, g_rebindW, g_rebindH);
            if (!ours) {
                ++g_wNoPair;
                return;
            }
            ctx->OMGetRenderTargets(kMaxRtvs, g_savedRtvs, &g_savedDsv);
            uint32_t n = 0;
            for (uint32_t i = 0; i < kMaxRtvs; ++i) {
                if (g_savedRtvs[i]) n = i + 1;
            }
            vScreenSetRenderTargetsRaw(ctx, n, g_savedRtvs, ours);
            g_rebound = true;
            ++g_wRebound;
            if (!g_rebindNoted) {
                g_rebindNoted = true;
                Log::get().note("ui depth: a composite whose own depth nothing reads "
                                "is bound to the pass's %ux%u depth for its draw "
                                "(eye %d by the order its colour target appeared "
                                "this frame); the game's bindings are put back after.",
                                g_rebindW, g_rebindH, g_rebindEye);
            }
        }
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
    if (!ran && g_rebound) {
        // A fault after the rebind: the game's bindings go back now rather
        // than at End, which the scope still calls.
        guarded("uiDepth.rebindRestore", [&] {
            uint32_t n = 0;
            for (uint32_t i = 0; i < kMaxRtvs; ++i) {
                if (g_savedRtvs[i]) n = i + 1;
            }
            vScreenSetRenderTargetsRaw(ctx, n, g_savedRtvs, g_savedDsv);
        });
        releaseSavedOm();
        g_rebound = false;
    }
}

void uiDepthEnd(ID3D11DeviceContext* ctx) {
    // Under SEH but NOT under the budget: a budget spent between Begin and
    // End would skip a guardedBudget body, and a restore skipped leaves our
    // writing twin, or our depth view, on every later draw in the frame
    // (review finding 1; resolve_probe.cpp restores the same way for the
    // same reason).
    if (g_engaged) {
        g_engaged = false;
        guarded("uiDepth.restore", [&] { ctx->OMSetDepthStencilState(g_savedDss, g_savedRef); });
        if (g_savedDss) {
            g_savedDss->Release();
            g_savedDss = nullptr;
        }
    }
    if (g_rebound) {
        g_rebound = false;
        guarded("uiDepth.rebindRestore", [&] {
            uint32_t n = 0;
            for (uint32_t i = 0; i < kMaxRtvs; ++i) {
                if (g_savedRtvs[i]) n = i + 1;
            }
            vScreenSetRenderTargetsRaw(ctx, n, g_savedRtvs, g_savedDsv);
        });
        releaseSavedOm();
    }
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
                        "direct), the game's depth test kept where it had one.",
                        g_wWrote, g_wComposite, g_surfaceCount, g_wDirect);
    }
    if (g_wFrames >= kTotalsFrames) {
        if (g_wWrote || g_wNotScene || g_wRebound || g_wNoPair || g_wNoTwin ||
            g_wLearned || g_evictions) {
            Log::get().note("ui depth totals: %.1f interface draws a frame wrote depth "
                            "(%.1f composites, %.1f direct; %.1f bound to the pass's "
                            "depth in place of their own); %u surfaces known, %u "
                            "learned this window, %u forgotten; left alone: %.1f a "
                            "frame not into the scene's depth, %.1f with no pair to "
                            "bind, %u already writing, %u with no twin; %u depth "
                            "states; %llu written this session.",
                            static_cast<double>(g_wWrote) / g_wFrames,
                            static_cast<double>(g_wComposite) / g_wFrames,
                            static_cast<double>(g_wDirect) / g_wFrames,
                            static_cast<double>(g_wRebound) / g_wFrames,
                            g_surfaceCount, g_wLearned, g_evictions,
                            static_cast<double>(g_wNotScene) / g_wFrames,
                            static_cast<double>(g_wNoPair) / g_wFrames,
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
    g_engaged = false;
    g_rebound = false;
    releaseSavedOm();
    releasePairViews();
    releaseStates();
    g_surfaceCount = 0;
    g_surfaceNext = 0;
    g_viewMemo.clear();
    g_vsMemo.clear();
    for (uint32_t i = 0; i < kExhausted; ++i) g_exhausted[i] = Exhausted();
    g_familyLoggedCount = 0;
    g_on = false;
}

}  // namespace edvr
