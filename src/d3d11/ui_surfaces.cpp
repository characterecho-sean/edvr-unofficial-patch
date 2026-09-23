// fix.ui_quality -- the surfaces half. ui_surfaces.h says what and why;
// ui_quality_math.h holds the arithmetic, the census, the learning and the
// pair memo, shared with tools/ui_quality_test; fss_res.cpp owns the
// inflation mechanism this drives (the tracked ring, the viewport and
// scissor backstops).
#include "ui_surfaces.h"

#include "ui_quality_math.h"
#include "ui_sizing_math.h"  // the confirmation instrument: chains and verdicts

#include "device_hook.h"  // deviceHookHmdQuality: the .fxcfg's HMD Quality
#include "fss_res.h"      // fssResOrigSize: a surface we grew is learned at its asked size
#include "ui_depth.h"     // uiDepthLearnedSurfaces: the classifier's GUI surfaces
#include "vscreen.h"      // vScreenInternalResolution: the last fallback

#include "../common/config.h"
#include "../common/frame_flag.h"             // eyeTextureSize: what the game submits
#include "../common/log.h"
#include "../common/native_render_settings.h"  // edvrQueryNativeRenderSizing

#include <windows.h>

#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace edvr {

namespace {

constexpr uint32_t kMaxDim = 16384;  // D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
constexpr uint32_t kSizeNotes = 8;   // distinct sizes named (with their RVAs) a session
constexpr uint32_t kCandidates = 32; // unmatched candidate creates remembered for learning
constexpr uint32_t kSnapshot = 32;   // learned surfaces kept for the family label
constexpr uint32_t kFirsts = 6;      // resizes quoted on the totals line (the census has five panels)
constexpr uint32_t kLearnNotes = 16; // learning lines a session
// The panels' file. The first builds' ui_quality_ratios.txt held ratios to
// the render size, which is not what the game sizes panels by (the rule in
// ui_quality_math.h); it is not read.
constexpr wchar_t kRatioFile[] = L"ui_quality_panels.txt";

// The internal render resolution and the vertical field of view, as read
// at one moment.
struct Basis {
    uint32_t recW = 0, recH = 0;
    int recSource = 0;  // 1 the runtime's frame, 2 its swapchain sizing (before the first frame)
    float hmd = 0.0f;
    uint32_t derivedW = 0, derivedH = 0;    // recommended x HMD Quality, truncated
    uint32_t askedW = 0, askedH = 0;        // what the game is told now (the host's ask)
    uint32_t askedDerivedW = 0, askedDerivedH = 0;  // ... x HMD Quality, when it differs
    uint32_t measuredW = 0, measuredH = 0;  // the eye the game submits
    uint32_t fallbackW = 0, fallbackH = 0;  // vScreen's measurement, when neither is known
    float T = 0.0f;                         // 2 tan(vFOV/2) of the frame's frustum; 0 unknown
    float vfovDeg = 0.0f;
    float up = 0.0f, down = 0.0f;           // that frustum's vertical tangents (magnitudes)
};

struct Candidate {
    uint32_t w = 0, h = 0;
    UiQualityBasis b;
};

struct Resized {
    uint32_t ow = 0, oh = 0, nw = 0, nh = 0;
    char family = 0;
};

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

// Everything below g_lock.
struct State {
    float target = 0.0f;
    std::string text = "off";
    bool loaded = false;
    UiQualityLearning learn;  // the census, learned ratios, pending sightings, fixed sizes
    UiQualityMemo memo;       // the last decisions, by the size the game asked for
    // The classifier's surfaces, snapshot every five seconds for the label.
    uint32_t snapW[kSnapshot] = {}, snapH[kSnapshot] = {};
    char snapF[kSnapshot] = {};
    uint32_t snapCount = 0;
    // Unmatched candidate creates, with the basis each was made against.
    Candidate cand[kCandidates];
    uint32_t candNext = 0, candCount = 0;
    // Session counts.
    uint32_t examined = 0;  // render or depth creates offered (0: the match never ran)
    uint32_t resized = 0, resizedCensus = 0, resizedLearned = 0, onSubmitted = 0;
    uint32_t unmatched = 0, memoHits = 0;
    Resized firsts[kFirsts];
    uint32_t firstCount = 0;
    Basis basis;  // as last read, for the summary
    bool basisRead = false;
    // Once-lines.
    uint32_t sizeW[kSizeNotes] = {}, sizeH[kSizeNotes] = {};
    uint32_t sizeNoted = 0;
    bool noHmdNoted = false, noBasisNoted = false, atTargetNoted = false, agreeNoted = false;
    bool noFovNoted = false;
    uint32_t crossNotes = 0;
    uint32_t crossDW = 0, crossDH = 0, crossMW = 0, crossMH = 0;
    uint32_t learnNotes = 0;
    uint64_t lastTickMs = 0;
    // The confirmation instrument: one chain per distinct size and kind.
    struct ChainSeen {
        uint32_t w = 0, h = 0;
        bool depth = false;
        UiChain chain;
        UiChainVerdict verdict;
    };
    static constexpr uint32_t kChainLines = 32;  // the flight's 13 GUI sizes, doubled by depth
    ChainSeen chains[kChainLines];
    uint32_t chainCount = 0;
    uint32_t chainOverflow = 0;
    bool chainOverflowNoted = false;
};
State g_s;

// EDVR's frame count (uiSurfacesFrameBoundary, once a frame), read by the
// instrument's lines from the creating threads.
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
State::ChainSeen* chainFor(uint32_t w, uint32_t h, bool depth) {
    for (uint32_t i = 0; i < g_s.chainCount; ++i) {
        State::ChainSeen& c = g_s.chains[i];
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

std::atomic<uint32_t> g_viewports{0}, g_scissors{0}, g_copies{0};

// Reads the inputs, each safe from any thread and none of them a lock that
// anything holds around a create: native temporal's recommendation is a
// lock-free snapshot (review P1-1), the runtime's sizing is an 80-byte copy
// under a mutex nothing holds while creating, HMD Quality is the cache
// above, the submitted size is a shared-memory word.
Basis readBasis() {
    Basis b;
    uint32_t w = 0, h = 0;
    if (nativeTemporalRecommended(&w, &h) && w && h) {
        b.recW = w;
        b.recH = h;
        b.recSource = 1;
    } else {
        EdvrNativeRenderSizing s{};
        if (edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1, sizeof(s), &s) &&
            s.valid == 1) {
            uiQualityRecommendedFromEyes(s.activeWidth, s.activeHeight, &w, &h);
            if (w && h) {
                b.recW = w;
                b.recH = h;
                b.recSource = 2;
            }
        }
    }
    b.hmd = hmdCached();
    b.derivedW = uiQualityInternalDim(b.recW, b.hmd);
    b.derivedH = uiQualityInternalDim(b.recH, b.hmd);
    if (!b.derivedW || !b.derivedH) b.derivedW = b.derivedH = 0;
    // The ask leads the frame's recommendation through an adoption (P3-1):
    // judged first where the two differ.
    if (nativeTemporalAsked(&w, &h) && w && h && (w != b.recW || h != b.recH)) {
        b.askedW = w;
        b.askedH = h;
        b.askedDerivedW = uiQualityInternalDim(w, b.hmd);
        b.askedDerivedH = uiQualityInternalDim(h, b.hmd);
        if (!b.askedDerivedW || !b.askedDerivedH) b.askedDerivedW = b.askedDerivedH = 0;
    }
    if (eyeTextureSize(&w, &h) && w && h) {
        b.measuredW = w;
        b.measuredH = h;
    }
    if (!b.derivedW && !b.measuredW && vScreenInternalResolution(&w, &h) && w && h) {
        b.fallbackW = w;
        b.fallbackH = h;
    }
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

std::wstring ratioPath() {
    const std::wstring& dir = Config::get().logDir();
    return dir.empty() ? std::wstring() : dir + L"\\" + kRatioFile;
}

// Under g_lock. The census first, then what this rig learned, is waiting
// on, and knows not to scale.
void loadTable() {
    g_s.loaded = true;
    g_s.learn.reset();
    const std::wstring path = ratioPath();
    if (path.empty()) return;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[8192] = {};
    DWORD got = 0;
    const BOOL ok = ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    if (!ok || !got) return;
    buf[got] = '\0';
    uiQualityParseLearning(buf, g_s.learn);
}

// Under g_lock.
void saveTable() {
    const std::wstring path = ratioPath();
    if (path.empty()) return;
    CreateDirectoryW(Config::get().logDir().c_str(), nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const std::string text = uiQualityFormatLearning(g_s.learn);
    DWORD written = 0;
    WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(f);
}

const char* familyName(char f) {
    return f == 'V' ? "vector" : f == 'T' ? "text" : f == 'I' ? "icon" : "unlabelled";
}

void appendf(std::string& s, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) s.append(buf, static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n) : sizeof(buf) - 1);
}

// "the runtime's 3070x3032 x HMD Quality 0.65 = 1995x1970"
std::string basisText(const Basis& b) {
    std::string s;
    if (b.derivedW) {
        appendf(s, "%ux%u (the runtime's %s%ux%u x HMD Quality %.2f)", b.derivedW, b.derivedH,
                b.recSource == 1 ? "" : "swapchain ", b.recW, b.recH, static_cast<double>(b.hmd));
        if (b.measuredW && (b.measuredW != b.derivedW || b.measuredH != b.derivedH))
            appendf(s, " -- but the game submits %ux%u", b.measuredW, b.measuredH);
        else if (b.measuredW)
            appendf(s, ", as the game submits");
        if (b.askedDerivedW) {
            appendf(s, " -- the game is told %ux%u now (%ux%u: a size change in flight)", b.askedW,
                    b.askedH, b.askedDerivedW, b.askedDerivedH);
        }
    } else if (b.measuredW) {
        appendf(s, "%ux%u (the size the game submits; %s)", b.measuredW, b.measuredH,
                !b.recW ? "no recommendation from the runtime" : "HMD Quality unread");
    } else if (b.fallbackW) {
        appendf(s, "%ux%u (vScreen's measurement)", b.fallbackW, b.fallbackH);
    } else {
        s = "unknown";
    }
    if (b.T > 0.0f) {
        const uint32_t W = b.derivedW ? b.derivedW : b.measuredW ? b.measuredW : b.fallbackW;
        appendf(s, ", with a vertical FOV of %.1f degrees", static_cast<double>(b.vfovDeg));
        if (W) appendf(s, " (U = %u / 2 tan(vFOV/2) = %.1f)", W, static_cast<double>(W / b.T));
    } else {
        appendf(s, ", vertical FOV not known yet");
    }
    return s;
}

// Under g_lock: what one sighting taught, said at most kLearnNotes times.
void noteLearning(UiQualityLearn v, char family, uint32_t w, uint32_t h, const UiQualityBasis& b,
                  uint32_t lw, uint32_t lh) {
    if (g_s.learnNotes >= kLearnNotes) return;
    if (v == UiQualityLearn::kPending) {
        ++g_s.learnNotes;
        const State::ChainSeen* seen = chainFor(w, h, false);
        Log::get().note(
            "ui quality: surfaces: pending -- the GUI renderer draws %s into a %ux%u surface made "
            "at %ux%u with 2 tan(vFOV/2) %.4f (ratio %.4f x %.4f of U = %.1f) that is on no panel "
            "ratio; it is resized once the same ratio is seen at a U more than 1%% different (it "
            "scales), never if the same size is (it does not). Its chain: %s.",
            familyName(family), w, h, b.W, b.H, static_cast<double>(b.T),
            uiQualityPanelX10000(w, b) / 10000.0, uiQualityPanelX10000(h, b) / 10000.0, b.unit(),
            seen ? uiChainVerdictShort(seen->verdict) : "not captured (past the instrument's cap)");
    } else if (v == UiQualityLearn::kPromoted) {
        ++g_s.learnNotes;
        Log::get().note(
            "ui quality: surfaces: learned -- a %s surface's ratio %.4f x %.4f of U held at a "
            "second U (now %ux%u at U = %.1f): it scales, so it is resized from its next creation "
            "(a trip through the main menu) and in later sessions (ui_quality_panels.txt beside "
            "the logs).",
            familyName(family), lw / 10000.0, lh / 10000.0, w, h, b.unit());
    } else if (v == UiQualityLearn::kNotScaling) {
        ++g_s.learnNotes;
        Log::get().note(
            "ui quality: surfaces: a %ux%u %s surface kept its pixel size at a second U (%.1f, "
            "%ux%u): it does not scale, and is never resized (kept in ui_quality_panels.txt).",
            w, h, familyName(family), b.unit(), b.W, b.H);
    } else if (v == UiQualityLearn::kFull) {
        ++g_s.learnNotes;
        Log::get().note("ui quality: surfaces: the learning table is full; a %ux%u surface is "
                        "not recorded.",
                        w, h);
    }
}

}  // namespace

// --------------------------------------------------------------- the API

void uiSurfacesSetTarget(float target, const char* text) {
    // Configure is the one place HMD Quality is read on this thread: it runs
    // when the ini changes, not every frame.
    if (target > 0.0f) hmdReadNow();
    Lock lock;
    const std::string t = text ? text : "off";
    if (target == g_s.target && t == g_s.text) return;
    g_s.target = target;
    g_s.text = t;
    // A real change starts the story over; the table and the size notes stay.
    g_s.noHmdNoted = g_s.noBasisNoted = g_s.atTargetNoted = false;
    g_s.memo.clear();
    g_on.store(target > 0.0f, std::memory_order_release);
}

bool uiSurfacesWantCreates() { return g_on.load(std::memory_order_acquire); }

// The render size the instrument judges a create by: what the game is told
// now during an adoption, else the derived, else the submitted, else
// vScreen's measurement.
void judgedBasis(const Basis& b, uint32_t* W, uint32_t* H, const char** label) {
    *W = *H = 0;
    *label = "unknown";
    if (b.askedDerivedW) {
        *W = b.askedDerivedW;
        *H = b.askedDerivedH;
        *label = "as the game is told now";
    } else if (b.derivedW) {
        *W = b.derivedW;
        *H = b.derivedH;
        *label = "derived";
    } else if (b.measuredW) {
        *W = b.measuredW;
        *H = b.measuredH;
        *label = "as the game submits";
    } else if (b.fallbackW) {
        *W = b.fallbackW;
        *H = b.fallbackH;
        *label = "vScreen's measurement";
    }
}

// Under g_lock, inside the game's create: the confirmation instrument's line
// for a surface-shaped create of a size and kind not seen yet this session
// (section 8.2) -- before any decision, so pending, learned and made-bigger
// surfaces alike carry one, and a miss reads as plainly as a hit.
void noteChainIfNew(const D3D11_TEXTURE2D_DESC* d, const Basis& b) {
    uint32_t W = 0, H = 0;
    const char* src = "unknown";
    judgedBasis(b, &W, &H, &src);
    const bool shaped = W ? uiQualityCandidateShape(d->Width, d->Height, W, H)
                          : d->Width >= 16 && d->Height >= 16 && (d->Width & (d->Width - 1)) != 0 &&
                                (d->Height & (d->Height - 1)) != 0;
    if (!shaped) return;
    const bool depth = (d->BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
    if (chainFor(d->Width, d->Height, depth)) return;
    if (g_s.chainCount >= State::kChainLines) {
        ++g_s.chainOverflow;
        if (!g_s.chainOverflowNoted) {
            g_s.chainOverflowNoted = true;
            Log::get().note("ui quality: sizing chain: %u sizes logged; further sizes are counted on "
                            "the totals line, not logged.",
                            State::kChainLines);
        }
        return;
    }
    State::ChainSeen& c = g_s.chains[g_s.chainCount++];
    c.w = d->Width;
    c.h = d->Height;
    c.depth = depth;
    c.chain = captureChain();
    c.verdict = uiChainVerdict(c.chain);
    const double k = uiSizingK(b.T);
    char rvas[160], verdict[320];
    uiChainFormat(c.chain, rvas, sizeof(rvas));
    uiChainVerdictText(c.verdict, verdict, sizeof(verdict));
    Log::get().note(
        "ui quality: sizing chain %u: frame %u, a %ux%u %s surface (DXGI format %u, bind 0x%X) -- "
        "W %ux%u (%s), tangents up %.4f down %.4f, vFOV %.1f degrees, k %.4f, implied stage "
        "%.0fx%.0f; %u game frames, innermost first: %s; verdict %s.",
        g_s.chainCount, g_frameNo.load(std::memory_order_relaxed), d->Width, d->Height,
        depth ? "depth" : "colour", static_cast<unsigned>(d->Format), d->BindFlags, W, H, src,
        static_cast<double>(b.up), static_cast<double>(b.down), static_cast<double>(b.vfovDeg), k,
        uiImpliedStage(d->Width, W, k), uiImpliedStage(d->Height, W, k), c.chain.n,
        c.chain.n ? rvas : "none", verdict);
}

bool uiSurfacesMatch(D3D11_TEXTURE2D_DESC* d, float* factorOut, char* familyOut) {
    if (!d || !g_on.load(std::memory_order_acquire)) return false;
    const Basis b = readBasis();
    const uint64_t now = GetTickCount64();
    Lock lock;
    ++g_s.examined;
    g_s.basis = b;
    g_s.basisRead = true;
    noteChainIfNew(d, b);
    // The pair: a create of a size decided within the last two seconds gets
    // that decision, whatever the basis says now (a colour target and its
    // depth partner must stay the same size).
    if (const UiQualityDecision* m = g_s.memo.find(d->Width, d->Height, now)) {
        if (m->nw == m->ow && m->nh == m->oh) return false;
        ++g_s.memoHits;
        d->Width = m->nw;
        d->Height = m->nh;
        if (factorOut) *factorOut = m->factor;
        if (familyOut) *familyOut = m->family;
        if (m->origin == UiQualityOrigin::kLearned) {
            ++g_s.resizedLearned;
        } else {
            ++g_s.resizedCensus;
        }
        return true;
    }
    float factor = 0.0f;
    if (!b.hmd) {
        if (!g_s.noHmdNoted) {
            g_s.noHmdNoted = true;
            Log::get().note("ui quality: surfaces: HMD Quality could not be read (no "
                            "Options\\Graphics\\*.fxcfg under this profile, or no "
                            "HMDRenderTargetMultiplier in the newest) -- no surface is resized "
                            "until it can be.");
        }
        return false;
    }
    if (!uiQualityFactor(g_s.target, b.hmd, &factor)) {
        if (!g_s.atTargetNoted && g_s.target > 0.0f) {
            g_s.atTargetNoted = true;
            Log::get().note("ui quality: surfaces: HMD Quality %.2f is already at or above %s -- "
                            "the game makes its surfaces that size or bigger; nothing to resize.",
                            static_cast<double>(b.hmd), g_s.text.c_str());
        }
        return false;
    }
    // The bases a panel is judged against: the size the game is told now,
    // where it differs from the frame's (a FOV-trim or cull-guard adoption:
    // the game re-creates its surfaces for the new ask before a frame of it
    // arrives -- review P3-1, the 13:23 flight's 1254x705 judged against the
    // old size); the derived render size; the size the game submits where it
    // differs (an HMD Quality the .fxcfg does not hold); vScreen's own
    // measurement only when none is known. Each with the frame's vertical
    // field of view: the game sizes a panel by W / 2 tan(vFOV/2)
    // (ui_quality_math.h's rule), not by the render size alone.
    UiQualityBasis bases[3];
    int which[3] = {};
    uint32_t nb = 0;
    auto add = [&](uint32_t W, uint32_t H, int w) {
        for (uint32_t i = 0; i < nb; ++i)
            if (bases[i].W == W && bases[i].H == H) return;
        bases[nb].W = W;
        bases[nb].H = H;
        bases[nb].T = b.T;
        which[nb++] = w;
    };
    if (b.askedDerivedW) add(b.askedDerivedW, b.askedDerivedH, 3);
    if (b.derivedW) add(b.derivedW, b.derivedH, 0);
    if (b.measuredW) add(b.measuredW, b.measuredH, 1);
    if (!nb && b.fallbackW) add(b.fallbackW, b.fallbackH, 2);
    if (!nb) {
        if (!g_s.noBasisNoted) {
            g_s.noBasisNoted = true;
            Log::get().note("ui quality: surfaces: the internal render resolution is not known yet "
                            "(no recommendation from the runtime, nothing submitted) -- a surface "
                            "made now is left as the game asks.");
        }
        return false;
    }
    if (!(b.T > 0.0f)) {
        if (!g_s.noFovNoted) {
            g_s.noFovNoted = true;
            Log::get().note("ui quality: surfaces: the game's vertical field of view is not known yet "
                            "(no frame has begun in EDVR's runtime) -- a surface made now is left "
                            "as the game asks; the cockpit's panels are made after it is.");
        }
        return false;
    }
    if (!g_s.loaded) loadTable();
    const UiQualityLearning& L = g_s.learn;
    int found = -1;
    uint32_t used = 0;
    bool candidate = false;
    for (uint32_t i = 0; i < nb && found < 0; ++i) {
        if (!uiQualityCandidateShape(d->Width, d->Height, bases[i].W, bases[i].H)) continue;
        candidate = true;
        found = uiQualityRatioFind(L.table, L.n, uiQualityPanelX10000(d->Width, bases[i]),
                                   uiQualityPanelX10000(d->Height, bases[i]));
        used = i;
    }
    const uint32_t ow = d->Width, oh = d->Height;
    if (found < 0) {
        if (candidate) {
            // Remembered with the basis it was made against: if the GUI
            // renderer's draws are later seen landing in a surface of this
            // size, its ratio is taken against THIS basis, not a later one.
            ++g_s.unmatched;
            Candidate& c = g_s.cand[g_s.candNext];
            g_s.candNext = (g_s.candNext + 1) % kCandidates;
            if (g_s.candCount < kCandidates) ++g_s.candCount;
            c.w = ow;
            c.h = oh;
            c.b = bases[0];
            UiQualityDecision keep;
            keep.ow = keep.nw = ow;
            keep.oh = keep.nh = oh;
            keep.ms = now;
            g_s.memo.put(keep);
        }
        return false;
    }
    const uint32_t nw = uiQualityRoundDim(ow, factor), nh = uiQualityRoundDim(oh, factor);
    if (nw > kMaxDim || nh > kMaxDim || (nw == ow && nh == oh)) return false;
    char family = 0;
    for (uint32_t i = 0; i < g_s.snapCount; ++i) {
        if (g_s.snapW[i] == ow && g_s.snapH[i] == oh) {
            family = g_s.snapF[i];
            break;
        }
    }
    const UiQualityRatio& r = L.table[found];
    if (which[used] == 1) ++g_s.onSubmitted;
    // Once per distinct size a session: what matched, against what, and the
    // game's allocating call -- the route to sizing it in the engine instead,
    // the way vscreen_res.h sizes the on-foot screen.
    bool noted = false;
    for (uint32_t i = 0; i < g_s.sizeNoted; ++i) noted = noted || (g_s.sizeW[i] == ow && g_s.sizeH[i] == oh);
    if (!noted && g_s.sizeNoted < kSizeNotes) {
        g_s.sizeW[g_s.sizeNoted] = ow;
        g_s.sizeH[g_s.sizeNoted] = oh;
        ++g_s.sizeNoted;
        // Its chain, as the instrument took it at this create (above): all
        // twelve game frames and the verdict (section 8).
        const State::ChainSeen* seen = chainFor(ow, oh, (d->BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0);
        const UiChain chain = seen ? seen->chain : captureChain();
        char rvas[160], verdict[320];
        uiChainFormat(chain, rvas, sizeof(rvas));
        uiChainVerdictText(seen ? seen->verdict : uiChainVerdict(chain), verdict, sizeof(verdict));
        Log::get().note(
            "ui quality: surfaces: made bigger -- a %ux%u interface surface (%s) is made at %ux%u "
            "(x%.4f, HMD Quality %.2f -> %s): %s panel ratio %.4f x %.4f of U = %.1f (render %ux%u "
            "%s, vertical FOV %.1f degrees); created from game RVAs %s, verdict %s.",
            ow, oh, familyName(family), nw, nh, static_cast<double>(factor),
            static_cast<double>(b.hmd), g_s.text.c_str(),
            r.origin == UiQualityOrigin::kCensus ? "the census's" : "a learned",
            r.w / 10000.0, r.h / 10000.0, bases[used].unit(), bases[used].W, bases[used].H,
            which[used] == 0   ? "derived"
            : which[used] == 1 ? "as the game submits"
            : which[used] == 3 ? "as the game is told now, a size change in flight"
                               : "vScreen's measurement",
            static_cast<double>(b.vfovDeg), chain.n ? rvas : "none", verdict);
    }
    d->Width = nw;
    d->Height = nh;
    if (factorOut) *factorOut = factor;
    if (familyOut) *familyOut = family;
    if (r.origin == UiQualityOrigin::kLearned) {
        ++g_s.resizedLearned;
    } else {
        ++g_s.resizedCensus;
    }
    UiQualityDecision made;
    made.ow = ow;
    made.oh = oh;
    made.nw = nw;
    made.nh = nh;
    made.factor = factor;
    made.family = family;
    made.origin = r.origin;
    made.ms = now;
    g_s.memo.put(made);
    return true;
}

void uiSurfacesNoteCreated(uint32_t origW, uint32_t origH, uint32_t newW, uint32_t newH,
                           char family) {
    Lock lock;
    ++g_s.resized;
    // The first few distinct sizes, for the totals line.
    for (uint32_t i = 0; i < g_s.firstCount; ++i) {
        if (g_s.firsts[i].ow == origW && g_s.firsts[i].oh == origH) return;
    }
    if (g_s.firstCount < kFirsts) g_s.firsts[g_s.firstCount++] = {origW, origH, newW, newH, family};
}

void uiSurfacesNoteViewport() { g_viewports.fetch_add(1, std::memory_order_relaxed); }
void uiSurfacesNoteScissor() { g_scissors.fetch_add(1, std::memory_order_relaxed); }
void uiSurfacesNoteCopy() { g_copies.fetch_add(1, std::memory_order_relaxed); }

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
    const uint64_t now = GetTickCount64();
    {
        Lock lock;
        if (now - g_s.lastTickMs < 5000) return;
        g_s.lastTickMs = now;
    }
    // HMD Quality for the next five seconds, read on a pool thread; this
    // tick uses the cache (review P3-2: no folder scan on the render thread).
    hmdRefreshOffThread();
    const Basis b = readBasis();
    UiDepthLearnedSurface learned[kSnapshot];
    const uint32_t nLearned = uiDepthLearnedSurfaces(learned, kSnapshot);
    Lock lock;
    g_s.basis = b;
    g_s.basisRead = true;
    // The cross-check: the derived size against the one the game submits.
    if (b.derivedW && b.measuredW) {
        const bool agree = b.derivedW == b.measuredW && b.derivedH == b.measuredH;
        if (agree && !g_s.agreeNoted) {
            g_s.agreeNoted = true;
            Log::get().note(
                "ui quality: surfaces: the internal render resolution derived from the runtime's "
                "%s%ux%u x HMD Quality %.2f is %ux%u, and the game submits %ux%u -- they agree; "
                "surfaces made before the first submit were matched against the derived size.",
                b.recSource == 1 ? "" : "swapchain ", b.recW, b.recH, static_cast<double>(b.hmd),
                b.derivedW, b.derivedH, b.measuredW, b.measuredH);
        } else if (!agree && g_s.crossNotes < 4 &&
                   (b.derivedW != g_s.crossDW || b.derivedH != g_s.crossDH ||
                    b.measuredW != g_s.crossMW || b.measuredH != g_s.crossMH)) {
            ++g_s.crossNotes;
            g_s.crossDW = b.derivedW;
            g_s.crossDH = b.derivedH;
            g_s.crossMW = b.measuredW;
            g_s.crossMH = b.measuredH;
            Log::get().note(
                "ui quality: surfaces: DISAGREE -- the runtime's %s%ux%u x HMD Quality %.2f gives "
                "%ux%u, but the game submits %ux%u (a FOV-trim or cull-guard change in flight, or "
                "an HMD Quality the newest .fxcfg does not hold); a surface is matched against "
                "either until they agree. Said at most four times.",
                b.recSource == 1 ? "" : "swapchain ", b.recW, b.recH, static_cast<double>(b.hmd),
                b.derivedW, b.derivedH, b.measuredW, b.measuredH);
        }
    }
    // The label snapshot, and the learning: a surface the GUI renderer's own
    // draws landed in (the classifier: it IS interface), whose create this
    // module saw unmatched, is observed at the basis of each such create --
    // pending at the first, learned when the same ratio holds at a second
    // resolution, refused for good when the same pixel size does.
    g_s.snapCount = 0;
    bool changed = false;
    for (uint32_t i = 0; i < nLearned; ++i) {
        uint32_t w = learned[i].w, h = learned[i].h;
        uint32_t ow = 0, oh = 0;
        if (fssResOrigSize(const_cast<void*>(learned[i].res), &ow, &oh)) {
            w = ow;
            h = oh;
        }
        if (g_s.snapCount < kSnapshot) {
            g_s.snapW[g_s.snapCount] = w;
            g_s.snapH[g_s.snapCount] = h;
            g_s.snapF[g_s.snapCount] = learned[i].family;
            ++g_s.snapCount;
        }
        // The distinct bases this size was created against.
        UiQualityBasis bases[4];
        uint32_t nBases = 0;
        for (uint32_t k = 0; k < g_s.candCount; ++k) {
            const Candidate& c = g_s.cand[k];
            if (c.w != w || c.h != h || !c.b.valid()) continue;
            bool seen = false;
            for (uint32_t j = 0; j < nBases; ++j)
                seen = seen || (bases[j].W == c.b.W && bases[j].H == c.b.H && bases[j].T == c.b.T);
            if (!seen && nBases < 4) bases[nBases++] = c.b;
        }
        if (!nBases) continue;
        if (!g_s.loaded) loadTable();
        for (uint32_t j = 0; j < nBases; ++j) {
            uint32_t lw = 0, lh = 0;
            const UiQualityLearn v = g_s.learn.observe(w, h, bases[j], &lw, &lh);
            if (v == UiQualityLearn::kPending || v == UiQualityLearn::kPromoted ||
                v == UiQualityLearn::kNotScaling) {
                changed = true;
            }
            noteLearning(v, learned[i].family, w, h, bases[j], lw, lh);
        }
    }
    if (changed) saveTable();
}

void uiSurfacesSummary(char* out, size_t n) {
    if (!out || !n) return;
    Lock lock;
    std::string s;
    if (!(g_s.target > 0.0f)) {
        s = "off";
    } else {
        const Basis& b = g_s.basis;
        if (!g_s.basisRead) {
            s = "no surface created since the key went on";
        } else {
            appendf(s, "internal %s", basisText(b).c_str());
            float factor = 0.0f;
            if (b.hmd && uiQualityFactor(g_s.target, b.hmd, &factor)) {
                appendf(s, ", factor %.4f", static_cast<double>(factor));
            } else if (b.hmd) {
                appendf(s, ", HMD Quality at or above the target (nothing to resize)");
            } else {
                appendf(s, ", HMD Quality unread");
            }
        }
        // Commas only inside: the totals line puts "; layer:" after this.
        if (!g_s.loaded) {
            appendf(s, ", %u census panels", kUiQualitySeedCount);
        } else {
            appendf(s, ", panels %u census + %u learned (%u pending a second resolution, %u sizes "
                       "that do not scale)",
                    kUiQualitySeedCount, g_s.learn.learnedCount(), g_s.learn.np, g_s.learn.nf);
        }
        if (g_s.resized) {
            appendf(s, ", %u made bigger this session (%u by census panel, %u learned%s, %u as a "
                       "pair's partner:",
                    g_s.resized, g_s.resizedCensus, g_s.resizedLearned,
                    g_s.onSubmitted ? ", some against the submitted size" : "", g_s.memoHits);
            for (uint32_t i = 0; i < g_s.firstCount; ++i) {
                const Resized& r = g_s.firsts[i];
                appendf(s, "%s %ux%u -> %ux%u %s", i ? "," : "", r.ow, r.oh, r.nw, r.nh,
                        familyName(r.family));
            }
            s += ")";
        } else {
            appendf(s, ", none made bigger yet (an open panel is resized the next time the game "
                       "makes it -- a trip through the main menu)");
        }
        appendf(s, ", %u render and depth creates examined (%u the shape of a surface but on no "
                   "ratio), %u viewports and %u scissors rescaled into them, %u copies touching one",
                g_s.examined, g_s.unmatched, g_viewports.load(), g_scissors.load(), g_copies.load());
    }
    _snprintf_s(out, n, _TRUNCATE, "%s", s.c_str());
}

}  // namespace edvr
