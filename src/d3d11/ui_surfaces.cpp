// fix.ui_quality -- the surfaces half. ui_surfaces.h says what and why;
// ui_quality_math.h holds the arithmetic and the census, shared with
// tools/ui_quality_test; fss_res.cpp owns the inflation mechanism this
// drives (the tracked ring, the viewport and scissor backstops).
#include "ui_surfaces.h"

#include "ui_quality_math.h"

#include "device_hook.h"  // deviceHookHmdQuality: the .fxcfg's HMD Quality
#include "fss_res.h"      // fssResOrigSize: a surface we grew is learned at its asked size
#include "ui_depth.h"     // uiDepthLearnedSurfaces: the classifier's GUI surfaces
#include "vscreen.h"      // vScreenInternalResolution: the last fallback

#include "../common/config.h"
#include "../common/frame_flag.h"             // eyeTextureSize: what the game submits
#include "../common/game_call_probe.h"        // captureGameCallStack: the RVA instrument
#include "../common/log.h"
#include "../common/native_render_settings.h"  // edvrQueryNativeRenderSizing

#include <windows.h>

#include <d3d11.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace edvr {

namespace {

constexpr uint32_t kMaxRatios = 32;
constexpr uint32_t kMaxDim = 16384;  // D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
constexpr uint32_t kSizeNotes = 8;   // distinct sizes named (with their RVAs) a session
constexpr uint32_t kCandidates = 32; // unmatched candidate creates remembered for learning
constexpr uint32_t kSnapshot = 32;   // learned surfaces kept for the family label
constexpr uint32_t kFirsts = 4;      // resizes quoted on the totals line
constexpr wchar_t kRatioFile[] = L"ui_quality_ratios.txt";

// The internal render resolution, as read at one moment.
struct Basis {
    uint32_t recW = 0, recH = 0;
    int recSource = 0;  // 1 the runtime's frame, 2 its swapchain sizing (before the first frame)
    float hmd = 0.0f;
    uint32_t derivedW = 0, derivedH = 0;    // recommended x HMD Quality, truncated
    uint32_t measuredW = 0, measuredH = 0;  // the eye the game submits
    uint32_t fallbackW = 0, fallbackH = 0;  // vScreen's measurement, when neither is known
};

struct Candidate {
    uint32_t w = 0, h = 0, basisW = 0, basisH = 0;
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

// Everything below g_lock.
struct State {
    float target = 0.0f;
    std::string text = "off";
    bool loaded = false;
    UiQualityRatio table[kMaxRatios];
    uint32_t count = 0;
    uint32_t learnedOnFile = 0;
    // The classifier's surfaces, snapshot each second for the label.
    uint32_t snapW[kSnapshot] = {}, snapH[kSnapshot] = {};
    char snapF[kSnapshot] = {};
    uint32_t snapCount = 0;
    // Unmatched candidate creates, with the basis each was made against.
    Candidate cand[kCandidates];
    uint32_t candNext = 0, candCount = 0;
    // Session counts.
    uint32_t examined = 0;  // render or depth creates offered (0: the match never ran)
    uint32_t resized = 0, resizedCensus = 0, resizedLearned = 0, onSubmitted = 0;
    uint32_t unmatched = 0;
    Resized firsts[kFirsts];
    uint32_t firstCount = 0;
    float lastFactor = 0.0f;
    Basis basis;  // as last read, for the summary
    bool basisRead = false;
    // Once-lines.
    uint32_t sizeW[kSizeNotes] = {}, sizeH[kSizeNotes] = {};
    uint32_t sizeNoted = 0;
    bool noHmdNoted = false, noBasisNoted = false, atTargetNoted = false, agreeNoted = false;
    uint32_t crossNotes = 0;
    uint32_t crossDW = 0, crossDH = 0, crossMW = 0, crossMH = 0;
    uint32_t learnNotes = 0;
    uint64_t lastSecondMs = 0;
};
State g_s;

std::atomic<uint32_t> g_viewports{0}, g_scissors{0}, g_copies{0};

// Reads the inputs: every one is safe from any thread, and none of them is
// read under g_lock (native_temporal takes its own).
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
    float hmd = 0.0f;
    if (deviceHookHmdQuality(&hmd) && hmd > 0.0f) b.hmd = hmd;
    b.derivedW = uiQualityInternalDim(b.recW, b.hmd);
    b.derivedH = uiQualityInternalDim(b.recH, b.hmd);
    if (!b.derivedW || !b.derivedH) b.derivedW = b.derivedH = 0;
    if (eyeTextureSize(&w, &h) && w && h) {
        b.measuredW = w;
        b.measuredH = h;
    }
    if (!b.derivedW && !b.measuredW && vScreenInternalResolution(&w, &h) && w && h) {
        b.fallbackW = w;
        b.fallbackH = h;
    }
    return b;
}

std::wstring ratioPath() {
    const std::wstring& dir = Config::get().logDir();
    return dir.empty() ? std::wstring() : dir + L"\\" + kRatioFile;
}

// Under g_lock. The census first, then whatever this rig learned.
void loadTable() {
    g_s.loaded = true;
    g_s.count = uiQualitySeedTable(g_s.table, kMaxRatios);
    g_s.learnedOnFile = 0;
    const std::wstring path = ratioPath();
    if (path.empty()) return;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[4096] = {};
    DWORD got = 0;
    const BOOL ok = ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    if (!ok || !got) return;
    buf[got] = '\0';
    g_s.learnedOnFile = uiQualityParseRatios(buf, g_s.table, &g_s.count, kMaxRatios);
}

// Under g_lock: the learned entries only (the census is compiled in).
void saveTable() {
    const std::wstring path = ratioPath();
    if (path.empty()) return;
    CreateDirectoryW(Config::get().logDir().c_str(), nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string text =
        "# fix.ui_quality: interface-surface ratios learned on this rig, in ten-thousandths of the\n"
        "# internal render width and height. The census's are compiled in. Delete to forget.\n";
    char line[48];
    for (uint32_t i = 0; i < g_s.count; ++i) {
        if (g_s.table[i].origin != UiQualityOrigin::kLearned) continue;
        _snprintf_s(line, _TRUNCATE, "%u %u\n", g_s.table[i].w, g_s.table[i].h);
        text += line;
    }
    DWORD written = 0;
    WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(f);
}

uint32_t learnedCount() {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_s.count; ++i) n += g_s.table[i].origin == UiQualityOrigin::kLearned;
    return n;
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
    } else if (b.measuredW) {
        appendf(s, "%ux%u (the size the game submits; %s)", b.measuredW, b.measuredH,
                !b.recW ? "no recommendation from the runtime" : "HMD Quality unread");
    } else if (b.fallbackW) {
        appendf(s, "%ux%u (vScreen's measurement)", b.fallbackW, b.fallbackH);
    } else {
        s = "unknown";
    }
    return s;
}

}  // namespace

// --------------------------------------------------------------- the API

void uiSurfacesSetTarget(float target, const char* text) {
    Lock lock;
    const std::string t = text ? text : "off";
    if (target == g_s.target && t == g_s.text) return;
    g_s.target = target;
    g_s.text = t;
    // A real change starts the story over; the table and the size notes stay.
    g_s.noHmdNoted = g_s.noBasisNoted = g_s.atTargetNoted = false;
    g_on.store(target > 0.0f, std::memory_order_release);
}

bool uiSurfacesWantCreates() { return g_on.load(std::memory_order_acquire); }

bool uiSurfacesMatch(D3D11_TEXTURE2D_DESC* d, float* factorOut, char* familyOut) {
    if (!d || !g_on.load(std::memory_order_acquire)) return false;
    const Basis b = readBasis();
    Lock lock;
    ++g_s.examined;
    g_s.basis = b;
    g_s.basisRead = true;
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
    g_s.lastFactor = factor;
    // The bases a ratio is taken against: the derived size, and the size
    // the game submits where it differs (a FOV-trim or cull-guard change in
    // flight, or an HMD Quality the .fxcfg does not hold); vScreen's own
    // measurement only when neither is known.
    uint32_t bw[2] = {}, bh[2] = {};
    int which[2] = {};
    uint32_t nb = 0;
    if (b.derivedW) {
        bw[nb] = b.derivedW;
        bh[nb] = b.derivedH;
        which[nb++] = 0;
    }
    if (b.measuredW && (b.measuredW != b.derivedW || b.measuredH != b.derivedH)) {
        bw[nb] = b.measuredW;
        bh[nb] = b.measuredH;
        which[nb++] = 1;
    }
    if (!nb && b.fallbackW) {
        bw[nb] = b.fallbackW;
        bh[nb] = b.fallbackH;
        which[nb++] = 2;
    }
    if (!nb) {
        if (!g_s.noBasisNoted) {
            g_s.noBasisNoted = true;
            Log::get().note("ui quality: surfaces: the internal render resolution is not known yet "
                            "(no recommendation from the runtime, nothing submitted) -- a surface "
                            "made now is left as the game asks.");
        }
        return false;
    }
    if (!g_s.loaded) loadTable();
    int found = -1;
    uint32_t used = 0;
    bool candidate = false;
    for (uint32_t i = 0; i < nb && found < 0; ++i) {
        if (!uiQualityCandidateShape(d->Width, d->Height, bw[i], bh[i])) continue;
        candidate = true;
        found = uiQualityRatioFind(g_s.table, g_s.count, uiQualityRatioX10000(d->Width, bw[i]),
                                   uiQualityRatioX10000(d->Height, bh[i]));
        used = i;
    }
    if (found < 0) {
        if (candidate) {
            // Remembered with the basis it was made against: if the GUI
            // renderer's draws are later seen landing in a surface of this
            // size, its ratio is learned from THIS basis, not a later one.
            ++g_s.unmatched;
            Candidate& c = g_s.cand[g_s.candNext];
            g_s.candNext = (g_s.candNext + 1) % kCandidates;
            if (g_s.candCount < kCandidates) ++g_s.candCount;
            c.w = d->Width;
            c.h = d->Height;
            c.basisW = bw[0];
            c.basisH = bh[0];
        }
        return false;
    }
    const uint32_t ow = d->Width, oh = d->Height;
    const uint32_t nw = uiQualityRoundDim(ow, factor), nh = uiQualityRoundDim(oh, factor);
    if (nw > kMaxDim || nh > kMaxDim || (nw == ow && nh == oh)) return false;
    char family = 0;
    for (uint32_t i = 0; i < g_s.snapCount; ++i) {
        if (g_s.snapW[i] == ow && g_s.snapH[i] == oh) {
            family = g_s.snapF[i];
            break;
        }
    }
    const UiQualityRatio& r = g_s.table[found];
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
        const GameCallStack stack = captureGameCallStack();
        char rvas[128];
        _snprintf_s(rvas, _TRUNCATE, "%s", stack.rvas);
        unsigned slashes = 0;
        for (char* p = rvas; *p; ++p) {
            if (*p == '/' && ++slashes == 4) {
                *p = '\0';
                break;
            }
        }
        Log::get().note(
            "ui quality: surfaces: a %ux%u interface surface (%s) is made at %ux%u (x%.4f, HMD "
            "Quality %.2f -> %s) -- %s ratio %.4f x %.4f of the internal %ux%u (%s); created from "
            "game RVAs %s (%u of %u captured frames in the game module).",
            ow, oh, familyName(family), nw, nh, static_cast<double>(factor),
            static_cast<double>(b.hmd), g_s.text.c_str(),
            r.origin == UiQualityOrigin::kCensus ? "the census's" : "a learned",
            r.w / 10000.0, r.h / 10000.0, bw[used], bh[used],
            which[used] == 0   ? "derived"
            : which[used] == 1 ? "the size the game submits"
                               : "vScreen's measurement",
            stack.gameFrames ? rvas : "none", stack.gameFrames, stack.captured);
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

void uiSurfacesFrameBoundary() {
    if (!g_on.load(std::memory_order_acquire)) return;
    const uint64_t now = GetTickCount64();
    {
        // Every five seconds: reading HMD Quality scans the game's Graphics
        // options folder, which is not a thing to do on the render thread
        // every frame (deviceHookHmdQuality caches it a second per thread).
        Lock lock;
        if (now - g_s.lastSecondMs < 5000) return;
        g_s.lastSecondMs = now;
    }
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
    // draws landed in (the classifier's evidence that it IS interface), whose
    // create this module saw unmatched, has its ratio taken against the basis
    // of that create and kept for later creates and sessions.
    g_s.snapCount = 0;
    bool learnedAny = false;
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
        // The creates of this size: one basis between them, or none learned.
        uint32_t basisW = 0, basisH = 0;
        bool ambiguous = false;
        for (uint32_t k = 0; k < g_s.candCount; ++k) {
            const Candidate& c = g_s.cand[k];
            if (c.w != w || c.h != h) continue;
            if (basisW && (c.basisW != basisW || c.basisH != basisH)) ambiguous = true;
            basisW = c.basisW;
            basisH = c.basisH;
        }
        if (!basisW || ambiguous || !uiQualityCandidateShape(w, h, basisW, basisH)) continue;
        if (!g_s.loaded) loadTable();
        const uint32_t rw = uiQualityRatioX10000(w, basisW), rh = uiQualityRatioX10000(h, basisH);
        if (!uiQualityLearn(g_s.table, &g_s.count, kMaxRatios, rw, rh)) continue;
        learnedAny = true;
        if (g_s.learnNotes < 8) {
            ++g_s.learnNotes;
            Log::get().note(
                "ui quality: surfaces: learned -- the GUI renderer draws %s into a %ux%u surface "
                "made against an internal %ux%u: ratio %.4f x %.4f, resized from its next "
                "creation (a trip through the main menu) and in later sessions "
                "(ui_quality_ratios.txt beside the logs).",
                familyName(learned[i].family), w, h, basisW, basisH, rw / 10000.0, rh / 10000.0);
        }
    }
    if (learnedAny) saveTable();
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
            appendf(s, ", %u census ratios", kUiQualitySeedCount);
        } else {
            appendf(s, ", ratios %u census + %u learned", kUiQualitySeedCount, learnedCount());
        }
        if (g_s.resized) {
            appendf(s, ", %u made bigger this session (%u by census ratio, %u learned%s:",
                    g_s.resized, g_s.resizedCensus, g_s.resizedLearned,
                    g_s.onSubmitted ? ", some against the submitted size" : "");
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
