// fix.ui_quality -- the interface surfaces' instruments and HMD Quality.
// ui_surfaces.h says what and why; ui_sizing_math.h holds the chains and
// their verdicts, ui_quality_math.h the render width and the panel shape,
// both shared with tools/ui_quality_test.
#include "ui_surfaces.h"

#include "ui_quality_math.h"  // uiQualityInternalDim, uiQualityFovTangent, uiQualityCandidateShape
#include "ui_sizing_math.h"   // the confirmation instrument: chains and verdicts

#include "device_hook.h"      // deviceHookHmdQuality: the .fxcfg's HMD Quality
#include "ui_panel_scale.h"   // uiPanelScaleLive/Factor: a chain's stage under the engine's sizing

#include "../common/log.h"

#include <windows.h>

#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace edvr {

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
struct Lock {
    Lock() { AcquireSRWLockExclusive(&g_lock); }
    ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
};

std::atomic<bool> g_on{false};

// HMD Quality, cached (review P3-2): read from the game's newest .fxcfg --
// a folder scan and a file read -- on configure, and while the key is on
// every five seconds on a thread-pool thread; never on the render thread's
// frame path, never inside a create. The bits of a float, 0 while unknown.
std::atomic<uint32_t> g_hmdBits{0};
std::atomic<bool> g_hmdBusy{false};

float hmdCached() {
    const uint32_t bits = g_hmdBits.load(std::memory_order_acquire);
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

void hmdReadNow() {
    float q = 0.0f;
    if (!deviceHookHmdQuality(&q) || !(q > 0.0f) || !std::isfinite(q)) q = 0.0f;
    uint32_t bits = 0;
    std::memcpy(&bits, &q, sizeof(bits));
    g_hmdBits.store(bits, std::memory_order_release);
}

VOID CALLBACK hmdRefresh(PTP_CALLBACK_INSTANCE, PVOID) {
    hmdReadNow();
    g_hmdBusy.store(false, std::memory_order_release);
}

void hmdRefreshOffThread() {
    bool idle = false;
    if (!g_hmdBusy.compare_exchange_strong(idle, true)) return;  // one in flight
    if (!TrySubmitThreadpoolCallback(hmdRefresh, nullptr, nullptr))
        g_hmdBusy.store(false, std::memory_order_release);
}

// The render state a create is judged in: the panel formula's W -- what the
// game is told now (the host's ask, else the frame's recommendation) times
// HMD Quality, truncated as Elite truncates it -- and the frame's vertical
// frustum. W 0 while either input is unknown.
struct Basis {
    uint32_t W = 0, H = 0;
    bool asked = false;  // from the host's ask, not the frame's recommendation
    float T = 0.0f;      // 2 tan(vFOV/2) of the frame's frustum; 0 unknown
    float vfovDeg = 0.0f;
    float up = 0.0f, down = 0.0f;  // that frustum's vertical tangents (magnitudes)
};

// Each input lock-free (review P1-1): native temporal's values are
// snapshots, HMD Quality the cache above.
Basis readBasis() {
    Basis b;
    uint32_t w = 0, h = 0;
    if (nativeTemporalAsked(&w, &h) && w && h) {
        b.asked = true;
    } else if (!nativeTemporalRecommended(&w, &h)) {
        w = h = 0;
    }
    const float hmd = hmdCached();
    b.W = uiQualityInternalDim(w, hmd);
    b.H = uiQualityInternalDim(h, hmd);
    if (!b.W || !b.H) b.W = b.H = 0;
    float up = 0.0f, down = 0.0f;
    if (nativeTemporalVerticalTangents(&up, &down)) {
        b.up = up;
        b.down = down;
        b.T = uiQualityFovTangent(up, down);
        if (b.T > 0.0f)
            b.vfovDeg = static_cast<float>((std::atan(up) + std::atan(down)) * 57.29577951308232);
    }
    return b;
}

// The confirmation instrument: one chain per distinct size and kind.
struct ChainSeen {
    uint32_t w = 0, h = 0;
    bool depth = false;
    UiChain chain;
    UiChainVerdict verdict;
};
constexpr uint32_t kChainLines = 32;  // the flights' 13 GUI sizes, doubled by depth

// Everything below g_lock.
struct State {
    ChainSeen chains[kChainLines];
    uint32_t chainCount = 0;
    bool chainOverflowNoted = false;
};
State g_s;

// EDVR's frame count (uiSurfacesFrameBoundary, once a frame), read by the
// instruments' lines from the creating threads.
std::atomic<uint32_t> g_frameNo{0};

// The game module's extent, for the chains.
uintptr_t gameBase() {
    static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return base;
}
uintptr_t gameSize() {
    static const uintptr_t size = [] {
        const uintptr_t base = gameBase();
        if (!base) return uintptr_t(0);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return uintptr_t(0);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return uintptr_t(0);
        return static_cast<uintptr_t>(nt->OptionalHeader.SizeOfImage);
    }();
    return size;
}

// This thread's return-address chain: the game's frames, innermost first
// (return addresses only -- no stack memory read, nothing suspended).
UiChain captureChain() {
    void* frames[48] = {};
    const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    uintptr_t addrs[48] = {};
    for (USHORT i = 0; i < n; ++i) addrs[i] = reinterpret_cast<uintptr_t>(frames[i]);
    UiChain c;
    uiChainFromFrames(addrs, n, gameBase(), gameSize(), &c);
    return c;
}

// Under g_lock: the chain entry of this size and kind, or null.
ChainSeen* chainFor(uint32_t w, uint32_t h, bool depth) {
    for (uint32_t i = 0; i < g_s.chainCount; ++i) {
        ChainSeen& c = g_s.chains[i];
        if (c.w == w && c.h == h && c.depth == depth) return &c;
    }
    return nullptr;
}

// The glyph atlas (section 8.3): up to four A8 textures of 1024 or more a
// side, by identity, their writes counted lock-free from the context hooks.
struct Atlas {
    std::atomic<const void*> res{nullptr};
    uint32_t w = 0, h = 0, frame = 0;
    UiChain chain;
    UiChainVerdict verdict;
    std::atomic<uint32_t> writes[3] = {};  // this window: UpdateSubresource, Map, copies
    std::atomic<uint64_t> total{0};        // since it was created
};
constexpr uint32_t kAtlases = 4;
Atlas g_atlas[kAtlases];
std::atomic<uint32_t> g_atlasCount{0};
uint32_t g_atlasNotes = 0;  // creation lines (under g_lock), capped

}  // namespace

// --------------------------------------------------------------- the API

void uiSurfacesSetTarget(float target) {
    // Configure is the one place HMD Quality is read on this thread: it runs
    // when the ini changes, not every frame.
    if (target > 0.0f) hmdReadNow();
    g_on.store(target > 0.0f, std::memory_order_release);
}

bool uiSurfacesWantsChain(const D3D11_TEXTURE2D_DESC& d, bool initialData) {
    // The shape an interface panel's colour or depth target has, less the
    // render size (read only past this): single mip, no MSAA, no initial
    // data, and neither side a power of two (atlases, icon caches and
    // shadow maps are) or a sliver.
    return g_on.load(std::memory_order_acquire) && !initialData && d.ArraySize == 1 &&
           d.SampleDesc.Count == 1 && d.MipLevels <= 1 &&
           (d.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) != 0 && d.Width >= 16 &&
           d.Height >= 16 && (d.Width & (d.Width - 1)) != 0 && (d.Height & (d.Height - 1)) != 0;
}

// Inside the game's create: the line for a panel-shaped create of a size and
// kind not seen yet this session -- a hit and a miss read alike.
void uiSurfacesNoteChain(const D3D11_TEXTURE2D_DESC& d) {
    const Basis b = readBasis();
    // Smaller than the render size on both axes (the eye targets and the
    // 3840x2160 2D screen are not), when the render size is known.
    if (b.W && !uiQualityCandidateShape(d.Width, d.Height, b.W, b.H)) return;
    const bool depth = (d.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
    Lock lock;
    if (chainFor(d.Width, d.Height, depth)) return;
    if (g_s.chainCount >= kChainLines) {
        if (!g_s.chainOverflowNoted) {
            g_s.chainOverflowNoted = true;
            Log::get().note("ui quality: sizing chain: %u sizes logged; further sizes are not logged.",
                            kChainLines);
        }
        return;
    }
    ChainSeen& c = g_s.chains[g_s.chainCount++];
    c.w = d.Width;
    c.h = d.Height;
    c.depth = depth;
    c.chain = captureChain();
    c.verdict = uiChainVerdict(c.chain);
    const double k = uiSizingK(b.T);
    // Under the engine-side sizing the game divides by 1920 x f: the stage is
    // the create's size times f (ui_panel_scale.h).
    const bool engine = uiPanelScaleLive();
    const double f = engine ? uiPanelScaleFactor() : 1.0;
    char rvas[160], verdict[320], sized[64] = "";
    uiChainFormat(c.chain, rvas, sizeof(rvas));
    uiChainVerdictText(c.verdict, verdict, sizeof(verdict));
    if (engine) std::snprintf(sized, sizeof(sized), " (the engine sizing panels x%.4f)", 1.0 / f);
    Log::get().note(
        "ui quality: sizing chain %u: frame %u, a %ux%u %s surface (DXGI format %u, bind 0x%X) -- "
        "W %ux%u (%s), tangents up %.4f down %.4f, vFOV %.1f degrees, k %.4f, implied stage "
        "%.0fx%.0f%s; %u game frames, innermost first: %s; verdict %s.",
        g_s.chainCount, g_frameNo.load(std::memory_order_relaxed), d.Width, d.Height,
        depth ? "depth" : "colour", static_cast<unsigned>(d.Format), d.BindFlags, b.W, b.H,
        !b.W ? "unknown" : b.asked ? "as the game is told" : "the frame's recommendation",
        static_cast<double>(b.up), static_cast<double>(b.down), static_cast<double>(b.vfovDeg), k,
        uiImpliedStage(d.Width, b.W, k) * f, uiImpliedStage(d.Height, b.W, k) * f, sized, c.chain.n,
        c.chain.n ? rvas : "none", verdict);
}

float uiSurfacesHmdQuality() { return hmdCached(); }

// ------------------------------------------------------------ the glyph atlas

namespace detail {
bool g_uiAtlasWatching = false;
}

bool uiSurfacesWantsAtlas(const D3D11_TEXTURE2D_DESC& d) {
    return g_on.load(std::memory_order_acquire) && d.Format == DXGI_FORMAT_A8_UNORM &&
           (d.Width >= 1024 || d.Height >= 1024);
}

void uiSurfacesNoteAtlas(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d, bool initialData) {
    if (!tex) return;
    const UiChain chain = captureChain();
    const UiChainVerdict verdict = uiChainVerdict(chain);
    const uint32_t frame = g_frameNo.load(std::memory_order_relaxed);
    Lock lock;
    // The same address again is a new texture there (the old one released):
    // its slot starts over. Otherwise the next free slot, up to four.
    uint32_t slot = kAtlases;
    const uint32_t count = g_atlasCount.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < count; ++i)
        if (g_atlas[i].res.load(std::memory_order_relaxed) == tex) slot = i;
    if (slot == kAtlases && count < kAtlases) slot = count;
    char rvas[160], text[320];
    uiChainFormat(chain, rvas, sizeof(rvas));
    uiChainVerdictText(verdict, text, sizeof(text));
    if (g_atlasNotes < 8) {
        ++g_atlasNotes;
        Log::get().note(
            "ui quality: glyph atlas: a %ux%u A8_UNORM texture (usage %u, bind 0x%X, CPU access 0x%X, "
            "misc 0x%X, %u mips, %s) created at frame %u -- %u game frames, innermost first: %s; "
            "verdict %s. Its writes are counted every 30 s: Scaleform's raster cache is written as "
            "glyphs arrive, a static font texture never after its creation%s.",
            d.Width, d.Height, static_cast<unsigned>(d.Usage), d.BindFlags, d.CPUAccessFlags,
            d.MiscFlags, d.MipLevels, initialData ? "with initial data" : "no initial data", frame,
            chain.n, chain.n ? rvas : "none", text, slot == kAtlases ? " (not watched: four already are)" : "");
    }
    if (slot == kAtlases) return;
    Atlas& a = g_atlas[slot];
    a.res.store(nullptr, std::memory_order_release);
    a.w = d.Width;
    a.h = d.Height;
    a.frame = frame;
    a.chain = chain;
    a.verdict = verdict;
    for (auto& w : a.writes) w.store(0, std::memory_order_relaxed);
    a.total.store(0, std::memory_order_relaxed);
    a.res.store(tex, std::memory_order_release);
    if (slot == count) g_atlasCount.store(count + 1, std::memory_order_release);
    detail::g_uiAtlasWatching = true;
}

void uiAtlasNoteWriteSlow(const void* res, int how) {
    if (!res || how < 0 || how > 2) return;
    const uint32_t count = g_atlasCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i) {
        Atlas& a = g_atlas[i];
        if (a.res.load(std::memory_order_acquire) != res) continue;
        a.writes[how].fetch_add(1, std::memory_order_relaxed);
        a.total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

void uiSurfacesLogAtlas() {
    Lock lock;  // the slots' sizes and chains are written under it
    const uint32_t count = g_atlasCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i) {
        Atlas& a = g_atlas[i];
        const uint32_t u = a.writes[0].exchange(0, std::memory_order_relaxed);
        const uint32_t m = a.writes[1].exchange(0, std::memory_order_relaxed);
        const uint32_t c = a.writes[2].exchange(0, std::memory_order_relaxed);
        Log::get().note("ui quality: glyph atlas %u: %ux%u (created at frame %u, verdict %s) -- %u writes "
                        "this window (%u UpdateSubresource, %u Map, %u copies into it), %llu since it "
                        "was created.",
                        i + 1, a.w, a.h, a.frame, uiChainVerdictShort(a.verdict), u + m + c, u, m, c,
                        static_cast<unsigned long long>(a.total.load(std::memory_order_relaxed)));
    }
}

void uiSurfacesFrameBoundary() {
    g_frameNo.fetch_add(1, std::memory_order_relaxed);
    if (!g_on.load(std::memory_order_acquire)) return;
    // The render thread's own: HMD Quality for the next five seconds, read on
    // a pool thread (review P3-2: no folder scan on the render thread).
    static uint64_t lastTickMs = 0;
    const uint64_t now = GetTickCount64();
    if (now - lastTickMs < 5000) return;
    lastTickMs = now;
    hmdRefreshOffThread();
}

}  // namespace edvr
