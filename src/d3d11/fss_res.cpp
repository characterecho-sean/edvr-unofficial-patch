#include "fss_res.h"

#include <windows.h>

#include <d3d11.h>

#include <cstdio>    // _snprintf_s: the hud-quality summary line's pieces
#include <cstdlib>   // strtoul: the surface_inflate spec parser
#include <cstring>   // memcpy: the spec string kept to log only on a change
#include <string>

#include "../common/config.h"
#include "../common/frame_flag.h"   // eyeTextureSize: the published per-eye size
#include "../common/game_call_probe.h"  // captureGameCallStack: the RVA instrument
#include "../common/log.h"
#include "../common/timing.h"       // nowMs/stampMs/elapsedMs/dueMs
#include "device_hook.h"            // deviceHookHmdQuality
#include "hud_quality_math.h"
#include "ui_depth.h"                // uiDepthLearnedSurfaceSizes: the cross-check counter
#include "vscreen.h"                 // vScreenInternalResolution

namespace edvr {

// fssResActive reads this from the header with no call: asked on the
// viewport paths, and the build has no /GL to fold a cross-TU getter.
namespace detail {
uint32_t g_fssResCount = 0;
}  // namespace detail

namespace {

// Entries are never removed -- the game gives no release signal we hook --
// so a stale pointer can sit here until overwritten. It is compared by
// identity and never dereferenced, and a recycled allocation would have to
// land on the same address AND present the pre-inflation viewport against
// that target to be acted on; accepted, and the note below says which
// texture every action was for.
//
// Sized 32, not the 8 that served the FSS alone. Eight was four zoom-ins of
// colour+depth pairs, which was ample when the only matcher produced two
// textures. surface_inflate takes four sizes, each with a depth partner:
// three named surfaces plus an FSS body pair is exactly eight, and the
// moment anything is recreated the ring evicts an entry whose texture is
// STILL BOUND. Its viewport then stops being scaled while its target stays
// inflated, so that panel draws into a corner of itself -- a loud failure,
// but one that costs a flight to diagnose. The array is pointers and two
// sizes; there is no reason to be thrifty with it. fix.hud_quality's ratio
// match can in principle confirm and inflate more distinct surfaces than
// the classifier-based design this replaced (its own table holds up to
// kRatioSlots, 16, candidate ratios; every CONFIRMED one that is actually
// created, plus its depth partner, is two more entries here) -- a cockpit
// census has only ever shown a handful in one session, so 32 is headroom
// today, not a proven ceiling; if a flight ever fills this ring while the
// key is on, that is the number to raise.
constexpr uint32_t kTracked = 32;

// The game asks for eye/2 per axis. Halving an odd size floors or ceils
// depending on the engine's arithmetic, and the published eye size itself is
// rounded from bounds -- so both halves are accepted, the same reasoning as
// the eye test's own two-pixel tolerance.
bool halfOf(uint32_t v, uint32_t whole) {
    return v == whole / 2 || v == (whole + 1) / 2;
}

// Four named surfaces at once. The cockpit census that motivated this found
// four interface surfaces in one frame, and naming them one at a time is how
// you find out which is which -- but naming two at once is how you find out
// whether they are the same panel seen twice.
constexpr uint32_t kSpecs = 4;

// D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION. A spec that would cross it is
// refused when it is read rather than at the create, so the log says the
// number is impossible instead of the texture quietly staying stock. Also
// the runtime ceiling fix.hud_quality's match mode checks at CREATE time,
// since its factor can change from one create to the next (HMD Quality is
// read live) where a named spec's factor cannot.
constexpr uint32_t kMaxDim = 16384;

// How many of ui_depth's learned interface-surface sizes fix.hud_quality
// asks for on each match, now used only to LABEL a ratio-matched surface's
// family for the log (vector/text/icon) when the classifier happens to
// have already learned it -- the cross-check counter, not the gate. The
// ring behind them holds 64 total (chrome included); a cockpit census has
// only ever shown a handful of distinct vector/text/icon panels in one
// session, so this is headroom, not a tight fit.
constexpr uint32_t kHqLearnMax = 16;

// The ratio table: how many distinct (width, height) fractions of the
// internal render resolution fix.hud_quality tracks across sessions at
// once. Generous headroom over the handful of interface surfaces a cockpit
// census has ever shown in one frame.
constexpr uint32_t kRatioSlots = 16;

// Two independent roundings of the same real fraction, taken at different
// pixel counts, do not land on the same four-significant-figure value
// exactly -- fss_res.h's own two-session census differed by a few parts in
// 10000 on the width ratio. Ten (0.1%) accepts that noise without being
// loose enough to conflate two genuinely different panels.
constexpr uint32_t kRatioToleranceX10000 = 10;

// At most this many distinct interface-surface sizes get their creating
// call's RVA chain logged in one session -- the instrument exists to name
// the game's allocating function for a human to decompile, not to grow
// without bound in a session with many small panels.
constexpr uint32_t kRvaLogMax = 8;

struct Spec {
    uint32_t w = 0;
    uint32_t h = 0;
    float    scale = 0.0f;
};

struct Tracked {
    void*         tex = nullptr;
    uint32_t      w = 0;   // the size the game asked for -- viewports arrive in it
    uint32_t      h = 0;
    float         scale = 1.0f;    // what it grew by; the viewport/scissor paths multiply by it
    InflateSource source = InflateSource::kNone;
};

struct State {
    bool        fssRule = false;   // experimental.fss_res: the half-eye matcher
    bool        announced = false;
    Spec        specs[kSpecs];
    uint32_t    specCount = 0;
    char        specStr[128] = {};  // the raw setting, to log only on a change
    std::string hudQualityStr;      // fix.hud_quality's raw text, same reason
    float       hudQualityTarget = 0.0f;   // 0 = off; else 1.0 or 1.25
    Tracked     tracked[kTracked];
    uint32_t    next = 0;
    uint32_t    inflateNotes = 0;
    uint32_t    scaled = 0;         // viewports scaled at RSSetViewports
    uint32_t    scaledLate = 0;     // caught by the draw-time backstop instead
    uint32_t    scaleNotes = 0;
    uint32_t    scissorScaled = 0;  // scissor rects fixed at the draw-time backstop
    uint32_t    scissorNotes = 0;
    uint32_t    copyNotes = 0;      // a copy/resolve touched a tracked texture
};
State g_s;

// fix.hud_quality's own running tally, for its resize summary and the "on
// but nothing matched" warning. Reset whenever the key's TEXT changes (a
// real change starts the story over), never on an unrelated ini reload.
struct HudFamilyRecord {
    bool     have = false;
    uint32_t origW = 0, origH = 0, newW = 0, newH = 0;
};
struct HudQualityStats {
    uint64_t         firstActiveMs = 0;   // stampMs() the first tick saw the key on
    bool             saidNoMatch = false;
    uint64_t         lastSummaryMs = 0;
    uint32_t         resizedCount = 0;    // cockpit-labelled (vector/text/icon) total
    HudFamilyRecord  vec, text, icon;
    uint32_t         fssResized = 0;      // the FSS/DSS half-eye matcher, while the key is on
    uint32_t         otherResized = 0;    // ratio-matched, not classifier-labelled this session
    uint32_t         viewportsRescaled = 0;
    uint32_t         scissorsRescaled = 0;
    uint32_t         copiesSeen = 0;
    float            lastMult = 0.0f;
    float            lastFactor = 0.0f;
    bool             unknownMultNoted = false;
    bool             unknownInternalResNoted = false;
    uint32_t         seenNotResizedNotes = 0;   // capped "seen, not resized" lines
    uint32_t         rvaLoggedCount = 0;        // distinct sizes RVA-logged so far (kRvaLogMax cap)
    uint32_t         rvaLoggedW[kRvaLogMax] = {};
    uint32_t         rvaLoggedH[kRvaLogMax] = {};
};
HudQualityStats g_hq;

// The cross-session ratio table (hud_quality_math.h's pure state machine)
// plus its file, and whether this process has loaded it yet. Loaded lazily
// on the first candidate create rather than at configure time, so a
// session that never turns the key on never touches disk for it.
struct RatioTableState {
    HudQualityRatioSlot slots[kRatioSlots];
    uint32_t            count = 0;
    bool                loaded = false;
};
RatioTableState g_ratioTable;

constexpr wchar_t kRatioStateFile[] = L"hud_quality_ratios.txt";

std::wstring ratioStatePath(const std::wstring& logDir) {
    return logDir + L"\\" + kRatioStateFile;
}

// Same discipline as vscreen_auto_state.cpp's lastKnownEyeWidth/
// noteResolvedEyeWidthForVScreenAuto: raw WinAPI file I/O, no exceptions,
// a corrupt or hand-edited file is ignored a line at a time rather than
// trusted or treated as fatal. Format: a count line, then one line per
// slot of "ratioW ratioH lastInternalW confirmed" (space-separated
// integers; confirmed is 0 or 1).
void hudQualityLoadRatios() {
    g_ratioTable.count = 0;
    g_ratioTable.loaded = true;
    const std::wstring& logDir = Config::get().logDir();
    if (logDir.empty()) return;
    HANDLE f = CreateFileW(ratioStatePath(logDir).c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[2048] = {};
    DWORD got = 0;
    const BOOL ok = ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    if (!ok || !got) return;
    buf[got] = '\0';
    char* p = buf;
    // First line: a declared count, read but not trusted past kRatioSlots
    // or past what the rest of the file actually holds.
    strtoul(p, &p, 10);
    while (*p && g_ratioTable.count < kRatioSlots) {
        while (*p == '\r' || *p == '\n') ++p;
        if (!*p) break;
        char* end = nullptr;
        const unsigned long rw = strtoul(p, &end, 10);
        if (end == p) break;
        p = end;
        const unsigned long rh = strtoul(p, &end, 10);
        if (end == p) break;
        p = end;
        const unsigned long lastW = strtoul(p, &end, 10);
        if (end == p) break;
        p = end;
        const unsigned long conf = strtoul(p, &end, 10);
        if (end == p) break;
        p = end;
        if (rw == 0 || rh == 0 || rw > 10000 || rh > 10000 || lastW < 100) {
            while (*p && *p != '\n') ++p;   // skip the rest of a bad line
            continue;
        }
        HudQualityRatioSlot& s = g_ratioTable.slots[g_ratioTable.count++];
        s.ratioWx10000 = static_cast<uint32_t>(rw);
        s.ratioHx10000 = static_cast<uint32_t>(rh);
        s.lastInternalW = static_cast<uint32_t>(lastW);
        s.confirmed = conf != 0;
    }
}

void hudQualitySaveRatios() {
    const std::wstring& logDir = Config::get().logDir();
    if (logDir.empty()) return;
    CreateDirectoryW(logDir.c_str(), nullptr);
    HANDLE f = CreateFileW(ratioStatePath(logDir).c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char line[128];
    int n = snprintf(line, sizeof(line), "%u\n", g_ratioTable.count);
    DWORD written = 0;
    if (n > 0) WriteFile(f, line, static_cast<DWORD>(n), &written, nullptr);
    for (uint32_t i = 0; i < g_ratioTable.count; ++i) {
        const HudQualityRatioSlot& s = g_ratioTable.slots[i];
        n = snprintf(line, sizeof(line), "%u %u %u %u\n", s.ratioWx10000, s.ratioHx10000,
                    s.lastInternalW, s.confirmed ? 1u : 0u);
        if (n > 0) WriteFile(f, line, static_cast<DWORD>(n), &written, nullptr);
    }
    CloseHandle(f);
}

// "1952x1597:2, 908x1361" -- a size, optionally a factor, comma separated.
// The factor defaults to 2 and is capped at 4, because the point of the
// instrument is to see whether inflation helps at all and 4x is already
// sixteen times the pixels of a surface that may not be the right one.
//
// Refuses the WHOLE setting on any malformed entry rather than applying the
// entries it understood. A probe that half-applies is a probe whose result
// cannot be read: the clear_probe parser next door takes the same line, and
// for the same reason.
//
// The factor is still parsed and bounded as an INTEGER here (strtoul, 2..4)
// -- advanced.surface_inflate's own syntax is unchanged, on purpose (it is
// a hand-typed developer instrument; a typo that means something completely
// different by accident is worse than a feature it doesn't have). Spec::scale
// is a float only so the SAME viewport/texture-size code beneath both
// matchers can serve fix.hud_quality's fractional factor too.
bool parseSpecs(const std::string& text, Spec* out, uint32_t* countOut) {
    uint32_t n = 0;
    const char* p = text.c_str();
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        if (!*p) break;
        if (n == kSpecs) return false;
        char* end = nullptr;
        const unsigned long w = strtoul(p, &end, 10);
        if (end == p || (*end != 'x' && *end != 'X')) return false;
        const char* q = end + 1;
        const unsigned long h = strtoul(q, &end, 10);
        if (end == q) return false;
        unsigned long s = 2;
        if (*end == ':') {
            const char* r = end + 1;
            s = strtoul(r, &end, 10);
            if (end == r) return false;
        }
        while (*end == ' ' || *end == '\t') ++end;
        if (*end && *end != ',') return false;
        if (w == 0 || h == 0 || s < 2 || s > 4) return false;
        if (w * s > kMaxDim || h * s > kMaxDim) return false;
        out[n].w = static_cast<uint32_t>(w);
        out[n].h = static_cast<uint32_t>(h);
        out[n].scale = static_cast<float>(s);
        ++n;
        p = end;
    }
    *countOut = n;
    return true;
}

void hudQualityNoteUnknownMultiplier() {
    if (g_hq.unknownMultNoted) return;
    g_hq.unknownMultNoted = true;
    Log::get().note(
        "hud quality: on, but HMD Quality could not be read (no "
        "Options\\Graphics\\*.fxcfg under this profile, or no "
        "HMDRenderTargetMultiplier in the newest one) -- nothing changes "
        "until it can be. Said once.");
}

void appendFamilyText(char* buf, size_t n, const char* name,
                      const HudFamilyRecord& r) {
    if (r.have) {
        _snprintf_s(buf, n, _TRUNCATE, "%s %ux%u -> %ux%u", name, r.origW,
                   r.origH, r.newW, r.newH);
    } else {
        _snprintf_s(buf, n, _TRUNCATE, "%s none yet", name);
    }
}

// "hud quality: 1.0 (HMD Quality 0.70 -> factor 1.4286): cockpit panels
// resized N (vector WxH -> WxH, text ..., icon ...), FSS N, other N, seen
// not resized N, viewports rescaled M, scissors rescaled S, copies
// touching one: K." At the first surface resized and every 30s after
// (fssResHudQualityTick), so a flight's log has both the instant the
// mechanism first engaged and a running total.
//
// "other" is a ratio match the classifier has not (yet, or ever, since it
// depends on fix.temporal_aa) labelled vector/text/icon -- the cross-check
// only names what it happens to know; the ratio match does not need it to
// have resized the surface. "seen not resized" is the distinct ratio
// candidates on file that are not yet confirmed at a second, different
// internal resolution -- present, not guessed at.
void logHudQualitySummary() {
    char vecBuf[64], textBuf[64], iconBuf[64];
    appendFamilyText(vecBuf, sizeof(vecBuf), "vector", g_hq.vec);
    appendFamilyText(textBuf, sizeof(textBuf), "text", g_hq.text);
    appendFamilyText(iconBuf, sizeof(iconBuf), "icon", g_hq.icon);
    uint32_t pending = 0;
    for (uint32_t i = 0; i < g_ratioTable.count; ++i) {
        if (!g_ratioTable.slots[i].confirmed) ++pending;
    }
    Log::get().note(
        "hud quality: %s (HMD Quality %.2f -> factor %.4f): cockpit panels "
        "resized %u (%s, %s, %s), FSS %u, other %u, seen not resized %u, "
        "viewports rescaled %u, scissors rescaled %u, copies touching "
        "one: %u.",
        g_s.hudQualityStr.c_str(), static_cast<double>(g_hq.lastMult),
        static_cast<double>(g_hq.lastFactor), g_hq.resizedCount, vecBuf,
        textBuf, iconBuf, g_hq.fssResized, g_hq.otherResized, pending,
        g_hq.viewportsRescaled, g_hq.scissorsRescaled, g_hq.copiesSeen);
}

// family is 'V'/'T'/'I' (the cross-check labelled it) or 0 ("other": a
// ratio match the classifier has not named).
void hudQualityNoteMatch(char family, uint32_t origW, uint32_t origH,
                         uint32_t newW, uint32_t newH) {
    HudFamilyRecord* rec = family == 'V'   ? &g_hq.vec
                           : family == 'T' ? &g_hq.text
                           : family == 'I' ? &g_hq.icon
                                           : nullptr;
    const bool first = g_hq.resizedCount == 0 && g_hq.otherResized == 0;
    if (rec) {
        rec->have = true;
        rec->origW = origW;
        rec->origH = origH;
        rec->newW = newW;
        rec->newH = newH;
        ++g_hq.resizedCount;
    } else {
        ++g_hq.otherResized;
    }
    if (first) {
        g_hq.lastSummaryMs = nowMs();
        logHudQualitySummary();
    }
}

void hudQualityNoteUnknownInternalRes() {
    if (g_hq.unknownInternalResNoted) return;
    g_hq.unknownInternalResNoted = true;
    Log::get().note(
        "hud quality: on, but the game's internal render resolution is not "
        "known yet (no scene has rendered enough frames to settle it) -- "
        "nothing can be matched by ratio until it is. Said once.");
}

// A ratio candidate that is on file but not yet confirmed at a second,
// different internal resolution: recorded, not resized. Capped like the
// other diagnostics here.
void hudQualityNoteSeenNotResized(uint32_t w, uint32_t h) {
    if (g_hq.seenNotResizedNotes >= 8) return;
    ++g_hq.seenNotResizedNotes;
    Log::get().note(
        "hud quality: seen, not resized -- a %ux%u candidate's ratio to "
        "the internal render resolution is on file but not yet confirmed "
        "at a second, different resolution. Said at most 8 times.",
        w, h);
}

// Sean asked whether the size could be patched in the engine instead of
// intercepted after the fact, the way the on-foot screen's resolution is
// (vscreen_res.h). This names the allocating call so that can be tried:
// the first four game-module return addresses on the stack the moment
// EDVR decides to inflate a given surface SIZE, once per distinct size
// per session, only while the key is on.
void hudQualityNoteRva(uint32_t w, uint32_t h, char family) {
    for (uint32_t i = 0; i < g_hq.rvaLoggedCount; ++i) {
        if (g_hq.rvaLoggedW[i] == w && g_hq.rvaLoggedH[i] == h) return;
    }
    if (g_hq.rvaLoggedCount >= kRvaLogMax) return;
    g_hq.rvaLoggedW[g_hq.rvaLoggedCount] = w;
    g_hq.rvaLoggedH[g_hq.rvaLoggedCount] = h;
    ++g_hq.rvaLoggedCount;
    const GameCallStack stack = captureGameCallStack();
    // The first four game frames only, matching what a human decompiling
    // the allocator actually needs -- captureGameCallStack's own string can
    // hold more; cut at the fourth '/'.
    char rvas[128];
    _snprintf_s(rvas, _TRUNCATE, "%s", stack.rvas);
    unsigned slashes = 0;
    for (char* p = rvas; *p; ++p) {
        if (*p == '/' && ++slashes == 4) { *p = '\0'; break; }
    }
    Log::get().note(
        "hud quality: interface surface %ux%u (%s) created from game RVAs "
        "%s (%u of %u captured frames were in the game module).",
        w, h, family == 'V' ? "vector" : family == 'T' ? "text"
             : family == 'I' ? "icon" : "other",
        stack.gameFrames ? rvas : "none", stack.gameFrames, stack.captured);
}

}  // namespace

void fssResConfigure(Config& cfg) {
    const bool was = g_s.fssRule;
    g_s.fssRule = cfg.getBool("experimental.fss_res", false);
    if (g_s.fssRule && !was && !g_s.announced) {
        g_s.announced = true;
        Log::get().note(
            "fss res: ON. The scanner's body layer will be created at full "
            "eye resolution instead of half when the FSS is next opened -- "
            "textures are made per zoom, so no restart is needed.");
    }

    // fix.hud_quality, read and logged BEFORE advanced.surface_inflate
    // below on purpose: that block returns early whenever ITS OWN text is
    // unchanged, which is the common case on a reload that touched some
    // other key, and code placed after it would then never run.
    const std::string hq = cfg.getString("fix.hud_quality", "off");
    if (hq != g_s.hudQualityStr) {
        g_s.hudQualityStr = hq;
        bool recognized = true;
        g_s.hudQualityTarget = hudQualityParseTarget(hq.c_str(), &recognized);
        g_hq = HudQualityStats{};   // a real change starts the story over
        if (!recognized) {
            Log::get().note(
                "hud quality: \"%s\" is not one of off, 1.0, 1.25 -- "
                "treated as off. The setting is hud_quality under [fix].",
                hq.c_str());
        } else if (!(g_s.hudQualityTarget > 0.0f)) {
            Log::get().note(
                "hud quality: off. The HUD's interface surfaces follow HMD "
                "Quality as today.");
        } else {
            Log::get().note(
                "hud quality: %s. The cockpit's interface surfaces will be "
                "created at the size they would have at HMD Quality %s, the "
                "next time the game makes one -- a trip through the main "
                "menu for a panel already open. Matched by their fixed "
                "ratio to the internal render resolution, confirmed once "
                "seen at a second, different resolution (any earlier "
                "session counts); until then a candidate is only logged as "
                "\"seen, not resized\".",
                hq.c_str(), hq.c_str());
        }
    }

    // The size-named matcher. Logged only when the setting's TEXT changes,
    // the way census_skip is: this runs on every ini reload, and a probe
    // that reprints itself every few seconds buries the receipts it exists
    // to produce.
    const std::string spec = cfg.getString("advanced.surface_inflate", "");
    if (spec.length() >= sizeof(g_s.specStr)) {
        Log::get().note("surface inflate: the spec is longer than %u "
                        "characters and was ignored.",
                        static_cast<unsigned>(sizeof(g_s.specStr)) - 1);
        return;
    }
    if (spec == g_s.specStr) return;
    memcpy(g_s.specStr, spec.c_str(), spec.length() + 1);

    if (spec.empty()) {
        g_s.specCount = 0;
        Log::get().note("surface inflate: off. Surfaces are created at the "
                        "size the game asks for.");
        return;
    }
    // n == 0 from text that is not empty means a spec of nothing but
    // separators. Refused out loud with the rest: the one outcome this
    // instrument must never have is doing nothing quietly.
    Spec parsed[kSpecs];
    uint32_t n = 0;
    if (!parseSpecs(spec, parsed, &n) || n == 0) {
        g_s.specCount = 0;
        Log::get().note(
            "surface inflate: \"%s\" is not up to %u entries of "
            "WIDTHxHEIGHT[:FACTOR], factor 2 to 4 and no side over %u after "
            "scaling. Refused whole rather than half-applied; no surface is "
            "inflated. The setting is surface_inflate under [advanced].",
            spec.c_str(), kSpecs, kMaxDim);
        return;
    }
    g_s.specCount = n;
    for (uint32_t i = 0; i < n; ++i) {
        g_s.specs[i] = parsed[i];
        const uint32_t scaledW =
            static_cast<uint32_t>(g_s.specs[i].w * g_s.specs[i].scale + 0.5f);
        const uint32_t scaledH =
            static_cast<uint32_t>(g_s.specs[i].h * g_s.specs[i].scale + 0.5f);
        Log::get().note(
            "surface inflate: a %ux%u render target or depth texture will be "
            "created at %ux%u (%ux) and its viewport scaled to match. Takes "
            "effect the next time the game makes one -- for a cockpit panel "
            "that is the next trip through the main menu. If nothing below "
            "says a texture WAS created, the size is not one the game asks "
            "for on this rig: read it from your own census.",
            g_s.specs[i].w, g_s.specs[i].h, scaledW, scaledH,
            static_cast<uint32_t>(g_s.specs[i].scale));
    }
}

bool fssResWantsCreates() {
    return g_s.fssRule || g_s.specCount != 0 || g_s.hudQualityTarget > 0.0f;
}

bool fssResWantsMatch() { return g_s.hudQualityTarget > 0.0f; }

bool fssResMaybeInflate(D3D11_TEXTURE2D_DESC* d, bool hasInitialData,
                        float* scaleOut, InflateSource* sourceOut,
                        char* familyOut) {
    if (!fssResWantsCreates() || !d || hasInitialData) return false;
    // Only the exact shape measured: a single-mip, non-MSAA render target or
    // depth texture. Anything else -- staging, arrays, mip chains -- is not
    // the body layer, whatever its size. An interface surface is the same
    // shape: colour target plus a depth partner, one mip, no MSAA, and a
    // spec naming its size matches BOTH, which is what the panel needs.
    if (d->ArraySize != 1 || d->SampleDesc.Count != 1 || d->MipLevels > 1) {
        return false;
    }
    if (!(d->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))) {
        return false;
    }
    // The FSS rule first: it is the shipped one, and it is the narrower
    // match of the two -- a spec that happened to name eye/2 would then be
    // redundant rather than fighting it.
    if (g_s.fssRule) {
        uint32_t ew = 0, eh = 0;
        if (eyeTextureSize(&ew, &eh) && halfOf(d->Width, ew) &&
            halfOf(d->Height, eh)) {
            d->Width *= 2;
            d->Height *= 2;
            if (scaleOut) *scaleOut = 2.0f;
            if (sourceOut) *sourceOut = InflateSource::kFss;
            return true;
        }
    }
    // The size-named matcher next: an explicit developer choice outranks
    // fix.hud_quality's automatic one below.
    for (uint32_t i = 0; i < g_s.specCount; ++i) {
        const Spec& s = g_s.specs[i];
        if (d->Width != s.w || d->Height != s.h) continue;
        d->Width = static_cast<uint32_t>(d->Width * s.scale + 0.5f);
        d->Height = static_cast<uint32_t>(d->Height * s.scale + 0.5f);
        if (scaleOut) *scaleOut = s.scale;
        if (sourceOut) *sourceOut = InflateSource::kNamed;
        return true;
    }
    // fix.hud_quality: matched by RATIO to the internal render resolution,
    // not through the classifier. The classifier (ui_depth.cpp) learns a
    // surface's size from draws INTO it, which happen after this very
    // call -- so it can never help the create it would need to inform, and
    // every session's first panel (and every panel with fix.temporal_aa
    // off) matched nothing through it. fss_res.h's own census measured the
    // interface surfaces as a fixed fraction of the internal render
    // resolution, stable across sessions at different resolutions; that
    // fraction is knowable the moment the game asks to create the texture,
    // once it has been confirmed by a second session at a different
    // resolution (hudQualityRatioObserve). The classifier is kept only as
    // a cross-check below, to LABEL a match's family for the log.
    if (g_s.hudQualityTarget > 0.0f) {
        float mult = 0.0f;
        if (!deviceHookHmdQuality(&mult) || !(mult > 0.0f)) {
            hudQualityNoteUnknownMultiplier();
        } else {
            float factor = 0.0f;
            if (hudQualityFactor(g_s.hudQualityTarget, mult, &factor)) {
                uint32_t internalW = 0, internalH = 0;
                if (!vScreenInternalResolution(&internalW, &internalH)) {
                    hudQualityNoteUnknownInternalRes();
                } else if (d->Width < internalW && d->Height < internalH &&
                          (d->Width & (d->Width - 1)) != 0 &&
                          (d->Height & (d->Height - 1)) != 0) {
                    // Non-power-of-two on both axes and smaller than the
                    // scene: the documented shape of an interface surface
                    // (fss_res.h). Filters out shadow maps, post buffers
                    // and the scene target itself before they ever reach
                    // the ratio table.
                    if (!g_ratioTable.loaded) hudQualityLoadRatios();
                    g_hq.lastMult = mult;
                    g_hq.lastFactor = factor;
                    const uint32_t rw = hudQualityRatioX10000(d->Width, internalW);
                    const uint32_t rh = hudQualityRatioX10000(d->Height, internalH);
                    const HudQualityRatioVerdict verdict = hudQualityRatioObserve(
                        g_ratioTable.slots, &g_ratioTable.count, kRatioSlots, rw, rh,
                        internalW, kRatioToleranceX10000);
                    if (verdict == HudQualityRatioVerdict::kNewCandidate) {
                        hudQualitySaveRatios();
                        hudQualityNoteSeenNotResized(d->Width, d->Height);
                    } else if (verdict == HudQualityRatioVerdict::kSameSession) {
                        hudQualityNoteSeenNotResized(d->Width, d->Height);
                    } else if (verdict == HudQualityRatioVerdict::kConfirmed) {
                        hudQualitySaveRatios();
                        const uint32_t origW = d->Width, origH = d->Height;
                        const uint32_t newW = hudQualityRoundDim(origW, factor);
                        const uint32_t newH = hudQualityRoundDim(origH, factor);
                        // Refuse rather than create past the API's own
                        // limit, the same ceiling the named matcher is
                        // pre-validated against at configure time -- this
                        // one cannot be, because HMD Quality (and so the
                        // factor) can change between one create and the
                        // next.
                        if (newW <= kMaxDim && newH <= kMaxDim &&
                            (newW != origW || newH != origH)) {
                            d->Width = newW;
                            d->Height = newH;
                            if (scaleOut) *scaleOut = factor;
                            if (sourceOut) *sourceOut = InflateSource::kMatch;
                            // The classifier, as a cross-check only: does it
                            // already know this ORIGINAL (pre-inflate) size
                            // by name? If so, label the log with its family;
                            // otherwise this is "other" -- ratio-matched,
                            // not (yet, or ever, without fix.temporal_aa)
                            // named by the classifier.
                            uint32_t lw[kHqLearnMax], lh[kHqLearnMax];
                            char lf[kHqLearnMax];
                            const uint32_t learned =
                                uiDepthLearnedSurfaceSizes(lw, lh, lf, kHqLearnMax);
                            const int idx =
                                hudQualityMatchLearned(origW, origH, lw, lh, learned);
                            const char family = idx >= 0 ? lf[idx] : 0;
                            if (familyOut) *familyOut = family;
                            hudQualityNoteRva(origW, origH, family);
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

void fssResNoteCreated(void* texture, uint32_t origW, uint32_t origH,
                       uint32_t newW, uint32_t newH, float scale,
                       InflateSource source, char family) {
    if (!texture || !(scale > 1.0f)) return;
    Tracked& t = g_s.tracked[g_s.next];
    g_s.next = (g_s.next + 1) % kTracked;
    t.tex = texture;
    t.w = origW;
    t.h = origH;
    t.scale = scale;
    t.source = source;
    ++detail::g_fssResCount;
    if (g_s.inflateNotes < 8) {
        ++g_s.inflateNotes;
        Log::get().note(
            "fss res: a %ux%u texture was created at %ux%u (%gx). Its "
            "viewports are scaled to match as they arrive. Said at most 8 "
            "times.",
            origW, origH, newW, newH, static_cast<double>(scale));
    }
    if (source == InflateSource::kMatch) {
        hudQualityNoteMatch(family, origW, origH, newW, newH);
    } else if (source == InflateSource::kFss && g_s.hudQualityTarget > 0.0f) {
        // Counted in the hud-quality summary too, so a flight with the FSS
        // open alongside the key reads one total picture of what got
        // bigger this session -- not gated on the key having CAUSED this
        // match, which fix.hud_quality never does for the FSS rule.
        const bool first = g_hq.resizedCount == 0 && g_hq.otherResized == 0 &&
                           g_hq.fssResized == 0;
        ++g_hq.fssResized;
        if (first) {
            g_hq.lastSummaryMs = nowMs();
            logHudQualitySummary();
        }
    }
}

bool fssResIsInflated(void* resource) {
    if (!resource || detail::g_fssResCount == 0) return false;
    for (const Tracked& t : g_s.tracked) {
        if (t.tex == resource) return true;
    }
    return false;
}

bool fssResOrigSize(void* resource, uint32_t* w, uint32_t* h) {
    if (!resource || detail::g_fssResCount == 0) return false;
    for (const Tracked& t : g_s.tracked) {
        if (t.tex == resource) {
            *w = t.w;
            *h = t.h;
            return true;
        }
    }
    return false;
}

float fssResScaleOf(void* resource) {
    if (!resource || detail::g_fssResCount == 0) return 1.0f;
    for (const Tracked& t : g_s.tracked) {
        if (t.tex == resource) return t.scale;
    }
    return 1.0f;
}

InflateSource fssResSourceOf(void* resource) {
    if (!resource || detail::g_fssResCount == 0) return InflateSource::kNone;
    for (const Tracked& t : g_s.tracked) {
        if (t.tex == resource) return t.source;
    }
    return InflateSource::kNone;
}

// Turning a matcher off does NOT untrack what it already inflated: those
// textures are still the wrong size for their viewports, and the game holds
// them until it releases them. So this stays true while anything is tracked.
void fssResNoteViewportScaled(void* resource, bool late) {
    if (late) {
        ++g_s.scaledLate;
    } else {
        ++g_s.scaled;
    }
    if (fssResSourceOf(resource) == InflateSource::kMatch) {
        ++g_hq.viewportsRescaled;
    }
    // The first few of each kind, then silence: the counts prove the
    // mechanism engaged, and "late" firing at all means the game set the
    // viewport before binding the target -- worth knowing, not worth a
    // line per draw.
    if (g_s.scaleNotes < 6) {
        ++g_s.scaleNotes;
        Log::get().note(
            "fss res: an inflated target's viewport was scaled to match it "
            "(%s; %u at set, %u at draw so far).",
            late ? "by the draw-time backstop" : "as it was set", g_s.scaled,
            g_s.scaledLate);
    }
}

// There is no set-time hook for scissor rects (nothing needed one before
// fix.hud_quality): this is the draw-time backstop's only half for them,
// called from the same place the viewport backstop is.
void fssResNoteScissorScaled(void* resource) {
    ++g_s.scissorScaled;
    if (fssResSourceOf(resource) == InflateSource::kMatch) {
        ++g_hq.scissorsRescaled;
    }
    if (g_s.scissorNotes < 6) {
        ++g_s.scissorNotes;
        Log::get().note(
            "fss res: an inflated target's scissor rect was scaled to "
            "match it at the draw-time backstop (%u so far). There is no "
            "hook on the game's own RSSetScissorRects, so a rect narrower "
            "than the whole target is not caught -- only one that still "
            "spans the pre-inflation size exactly.",
            g_s.scissorScaled);
    }
}

void fssResNoteCopyMaybeMismatched(void* dst, void* src) {
    if (detail::g_fssResCount == 0) return;
    void* which = nullptr;
    if (dst && fssResIsInflated(dst)) {
        which = dst;
    } else if (src && fssResIsInflated(src)) {
        which = src;
    }
    if (!which) return;
    uint32_t ow = 0, oh = 0;
    fssResOrigSize(which, &ow, &oh);
    const float scale = fssResScaleOf(which);
    if (fssResSourceOf(which) == InflateSource::kMatch) {
        ++g_hq.copiesSeen;
    }
    if (g_s.copyNotes < 8) {
        ++g_s.copyNotes;
        Log::get().note(
            "fss res: a copy or resolve touched an inflated texture (%s, "
            "tracked at %ux%u -> %.0fx%.0f, %gx). This does not rescale the "
            "copy's own box or check the other side's size -- no such copy "
            "has been observed landing in one of these surfaces, so this is "
            "a detector, not a fix, until one is. Said at most 8 times.",
            which == dst ? "as the destination" : "as the source", ow, oh,
            static_cast<double>(ow) * static_cast<double>(scale),
            static_cast<double>(oh) * static_cast<double>(scale),
            static_cast<double>(scale));
    }
}

void fssResHudQualityTick() {
    if (!(g_s.hudQualityTarget > 0.0f)) return;
    if (g_hq.firstActiveMs == 0) g_hq.firstActiveMs = stampMs();
    const bool anyMatch = g_hq.resizedCount != 0 || g_hq.otherResized != 0 ||
                         g_hq.fssResized != 0;
    if (!anyMatch) {
        if (!g_hq.saidNoMatch && elapsedMs(g_hq.firstActiveMs, 60000)) {
            g_hq.saidNoMatch = true;
            if (!g_ratioTable.loaded) hudQualityLoadRatios();
            uint32_t pending = 0;
            for (uint32_t i = 0; i < g_ratioTable.count; ++i) {
                if (!g_ratioTable.slots[i].confirmed) ++pending;
            }
            Log::get().note(
                "hud quality: on but nothing has matched by ratio yet (%u "
                "candidate ratio(s) seen, none confirmed at a second, "
                "different internal resolution -- an earlier session's "
                "data counts, so this is common only on a fresh install or "
                "one that has always run the same HMD Quality): nothing "
                "changed.",
                pending);
        }
        return;
    }
    if (dueMs(g_hq.lastSummaryMs, 30000)) {
        g_hq.lastSummaryMs = nowMs();
        logHudQualitySummary();
    }
}

}  // namespace edvr
