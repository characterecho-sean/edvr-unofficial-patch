#include "ui_depth.h"

#include <windows.h>

#include <d3d11.h>

#include <cstdlib>   // _strtoui64: the hash lists
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "exposure_fix.h"   // lookupShaderHash

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

constexpr uint32_t kMaxSurfaces = 64;    // a session showed six
constexpr uint32_t kMaxHashes = 16;
constexpr uint32_t kMaxStates = 16;      // the game has a handful of depth states
constexpr uint32_t kMaxMemo = 256;       // sampled-view memos
constexpr uint32_t kMemoLifeFrames = 120;
constexpr uint32_t kChecksPerTarget = 64; // hashes asked per newly bound target
constexpr uint32_t kTotalsFrames = 1800;  // about 20 s at 90 Hz

FaultBudget g_budget("uiDepth", 5);

bool     g_on = false;
bool     g_stoodDown = false;
bool     g_announced = false;
uint32_t g_frame = 0;

uint64_t g_families[kMaxHashes];
uint32_t g_familyCount = 0;
uint64_t g_exclude[kMaxHashes];
uint32_t g_excludeCount = 0;

// Learned surfaces: resource identities, compared and never dereferenced
// (binding_shadow.h's bargain). A ring: a surface the game released and
// recreated simply re-learns.
void*    g_surfaces[kMaxSurfaces];
uint32_t g_surfaceCount = 0;
uint32_t g_surfaceNext = 0;

// The offscreen learner's state for the currently bound target.
uint32_t g_rtvGen = ~0u;
bool     g_rtvKnown = false;
uint32_t g_rtvChecks = 0;
void*    g_rtvRes = nullptr;

// Sampled-view memos: a view pointer resolved once to "surface or not",
// re-resolved after kMemoLifeFrames so a recycled pointer cannot lie for
// long. The identity is compared only; the resolve goes through the guarded
// probe.
struct Memo {
    void*    view = nullptr;
    bool     surface = false;
    uint32_t frame = 0;
};
Memo     g_memo[kMaxMemo];
uint32_t g_memoNext = 0;

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

// Per draw.
bool                     g_engaged = false;
ID3D11DepthStencilState* g_savedDss = nullptr;
UINT                     g_savedRef = 0;

// Counters: this window, and the session.
uint32_t g_wComposite = 0, g_wDirect = 0, g_wWrote = 0, g_wNoDepth = 0,
         g_wNoState = 0, g_wLearned = 0, g_wFrames = 0;
uint64_t g_sessionWrote = 0;

bool isSurface(void* res) {
    if (!res) return false;
    for (uint32_t i = 0; i < g_surfaceCount; ++i) {
        if (g_surfaces[i] == res) return true;
    }
    return false;
}

void addSurface(void* res) {
    if (isSurface(res)) return;
    if (g_surfaceCount < kMaxSurfaces) {
        g_surfaces[g_surfaceCount++] = res;
    } else {
        g_surfaces[g_surfaceNext] = res;
        g_surfaceNext = (g_surfaceNext + 1) % kMaxSurfaces;
    }
    ++g_wLearned;
}

bool inList(const uint64_t* list, uint32_t n, uint64_t h) {
    for (uint32_t i = 0; i < n; ++i) {
        if (list[i] == h) return true;
    }
    return false;
}

uint64_t boundVsHash(ID3D11DeviceContext* ctx) {
    uint64_t h = 0;
    guardedBudget(g_budget, [&] {
        ID3D11VertexShader* vs = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        if (vs) {
            h = lookupShaderHash(vs);
            vs->Release();
        }
    });
    return h;
}

// Is this sampled view over a learned surface? Memoised by view identity.
bool viewIsSurface(void* view) {
    if (!view) return false;
    Memo* slot = nullptr;
    for (uint32_t i = 0; i < kMaxMemo; ++i) {
        if (g_memo[i].view == view) {
            slot = &g_memo[i];
            break;
        }
    }
    if (slot && g_frame - slot->frame < kMemoLifeFrames) return slot->surface;
    ResourceInfo info;
    const bool surface = bindingResolve(view, &info) && info.isTexture2D &&
                         isSurface(info.resource);
    if (!slot) {
        slot = &g_memo[g_memoNext];
        g_memoNext = (g_memoNext + 1) % kMaxMemo;
    }
    slot->view = view;
    slot->surface = surface;
    slot->frame = g_frame;
    return surface;
}

// The writing twin of the game's depth-stencil state, or null when the game
// already writes (nothing to do) or no twin can be made (the draw stays as
// the game issued it).
ID3D11DepthStencilState* writingTwin(ID3D11DeviceContext* ctx,
                                     ID3D11DepthStencilState* game) {
    // A null state is D3D's default: depth on, LESS, write all. Writes.
    if (!game) return nullptr;
    for (uint32_t i = 0; i < g_stateCount; ++i) {
        if (g_states[i].game == game) {
            return g_states[i].alreadyWrites ? nullptr : g_states[i].ours;
        }
    }
    D3D11_DEPTH_STENCIL_DESC d{};
    game->GetDesc(&d);
    const bool writes = d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
    ID3D11DepthStencilState* ours = nullptr;
    if (!writes) {
        // The game's test kept where it had one (the cockpit occludes its
        // panels); ALWAYS where it had none (a menu writes over what is
        // behind it). Stencil exactly as the game's.
        if (!d.DepthEnable) {
            d.DepthEnable = TRUE;
            d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        }
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return nullptr;
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
            return nullptr;
        }
    }
    if (g_stateCount >= kMaxStates) {
        // Not cached: a twin made per draw would leak. Refused, counted,
        // said once.
        if (ours) ours->Release();
        if (!g_statesFullNoted) {
            g_statesFullNoted = true;
            Log::get().note("ui depth: more than %u distinct depth states at UI "
                            "draws; the ones past the table are left as the game "
                            "issued them.",
                            kMaxStates);
        }
        return nullptr;
    }
    game->AddRef();
    StatePair& p = g_states[g_stateCount++];
    p.game = game;
    p.ours = ours;
    p.alreadyWrites = writes;
    return writes ? nullptr : ours;
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
}

}  // namespace

void uiDepthConfigure(Config& cfg) {
    const std::string mode = cfg.getString("fix.ui_depth", "off");
    const bool was = g_on;
    g_on = mode == "on";
    if (!g_on && mode != "off") {
        Log::get().note("ui depth: fix.ui_depth = \"%s\" is not on or off; off.",
                        mode.c_str());
    }
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
    if (g_on && !was) {
        g_stoodDown = false;
        g_announced = false;
        // A fresh window, so the engage line counts this switch-on and not
        // whatever the last window had accumulated before the key went off
        // (the first flight's second engage line read 79564 draws).
        g_wComposite = g_wDirect = g_wWrote = g_wNoDepth = g_wNoState = 0;
        g_wLearned = 0;
        g_wFrames = 0;
        Log::get().note("ui depth: ON -- the interface's composites and the flight HUD "
                        "will write their depth for the temporal pass (%u direct "
                        "famil%s, %u excluded). It says so again when the first "
                        "frame writes.",
                        g_familyCount, g_familyCount == 1 ? "y" : "ies",
                        g_excludeCount);
    } else if (!g_on && was) {
        Log::get().note("ui depth: off. The interface draws as the game issues it.");
    }
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
            g_rtvKnown = isSurface(g_rtvRes);
        } else {
            g_rtvChecks = kChecksPerTarget;   // nothing here to learn
        }
    }
    if (g_rtvKnown || !g_rtvRes || g_rtvChecks >= kChecksPerTarget) return;
    ++g_rtvChecks;
    const uint64_t h = boundVsHash(ctx);
    if (h == kGuiVector || h == kGuiText || h == kGuiIcons) {
        addSurface(g_rtvRes);
        g_rtvKnown = true;
    }
}

bool uiDepthOnEyeDraw(ID3D11DeviceContext* ctx) {
    if (!g_on || g_stoodDown) return false;
    bool composite = false;
    static const BindSlot kSlots[4] = {BindSlot::PsSrv0, BindSlot::PsSrv1,
                                       BindSlot::PsSrv2, BindSlot::PsSrv3};
    for (const BindSlot slot : kSlots) {
        if (viewIsSurface(bindingGet(slot))) {
            composite = true;
            break;
        }
    }
    const uint64_t h = boundVsHash(ctx);
    if (h && inList(g_exclude, g_excludeCount, h)) return false;
    const bool direct = !composite && h && inList(g_families, g_familyCount, h);
    if (!composite && !direct) return false;
    // A piece of the interface with no depth target bound has nowhere to
    // write. Counted here, after the classification, so the figure is
    // interface draws left alone -- the first flight counted every
    // depthless eye draw (the post chain's) and read 10 a frame for nothing.
    if (!bindingGet(BindSlot::Dsv0)) {
        ++g_wNoDepth;
        return false;
    }
    if (composite) {
        ++g_wComposite;
    } else {
        ++g_wDirect;
    }
    return true;
}

void uiDepthBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    if (!g_on || g_stoodDown) return;
    const bool ok = guardedBudget(g_budget, [&] {
        ID3D11DepthStencilState* game = nullptr;
        UINT ref = 0;
        ctx->OMGetDepthStencilState(&game, &ref);
        ID3D11DepthStencilState* ours = writingTwin(ctx, game);
        if (!ours) {
            if (game) game->Release();
            ++g_wNoState;
            return;
        }
        g_savedDss = game;   // the AddRef from OMGet, released at End
        g_savedRef = ref;
        ctx->OMSetDepthStencilState(ours, ref);
        g_engaged = true;
        ++g_wWrote;
        ++g_sessionWrote;
    });
    if (!ok && !g_budget.shouldRun()) {
        g_stoodDown = true;
        Log::get().note("ui depth: STANDING DOWN for the session -- the depth "
                        "state swap faulted repeatedly. The interface draws as "
                        "the game issues it.");
    }
}

void uiDepthEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged) return;
    g_engaged = false;
    guardedBudget(g_budget, [&] { ctx->OMSetDepthStencilState(g_savedDss, g_savedRef); });
    if (g_savedDss) {
        g_savedDss->Release();
        g_savedDss = nullptr;
    }
}

void uiDepthFrameBoundary() {
    ++g_frame;
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
        if (g_wWrote || g_wNoDepth || g_wNoState || g_wLearned) {
            Log::get().note("ui depth totals: %.1f interface draws a frame wrote depth "
                            "(%.1f composites, %.1f direct); %u surfaces known, %u "
                            "learned this window; left alone: %u with no depth "
                            "target, %u with no writable state; %u depth states "
                            "derived; %llu written this session.",
                            static_cast<double>(g_wWrote) / g_wFrames,
                            static_cast<double>(g_wComposite) / g_wFrames,
                            static_cast<double>(g_wDirect) / g_wFrames,
                            g_surfaceCount, g_wLearned, g_wNoDepth, g_wNoState,
                            g_stateCount,
                            static_cast<unsigned long long>(g_sessionWrote));
        }
        g_wComposite = g_wDirect = g_wWrote = g_wNoDepth = g_wNoState = 0;
        g_wLearned = 0;
        g_wFrames = 0;
    }
}

void uiDepthShutdown() {
    if (g_savedDss) {
        g_savedDss->Release();
        g_savedDss = nullptr;
    }
    g_engaged = false;
    releaseStates();
    g_surfaceCount = 0;
    g_surfaceNext = 0;
    for (uint32_t i = 0; i < kMaxMemo; ++i) g_memo[i] = Memo();
    g_on = false;
}

}  // namespace edvr
