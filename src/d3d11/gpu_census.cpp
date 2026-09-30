#include "gpu_census.h"
#include "gpu_interval.h"
#include "gpu_frame_gap.h"
#include "gpu_frame_timing.h"
#include "../common/log.h"
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace edvr {
namespace {

constexpr size_t kSections = static_cast<size_t>(GpuCensusSection::Count);
// DoorTemporalWhole .. DoorFssHeal are indices 0..8; the in-frame sections
// start at FrameHologramPasses. Both ranges are contiguous by construction
// (gpu_census.h), so a single index split is enough.
constexpr size_t kDoorSections = static_cast<size_t>(GpuCensusSection::FrameHologramPasses);
constexpr unsigned kDoorOccurrenceCap = 4;   // both eyes, and the fovea's two crops per eye
constexpr unsigned kFrameOccurrenceCap = 8;  // per-draw sections

bool isDoorSection(GpuCensusSection section) noexcept {
    return static_cast<size_t>(section) < kDoorSections;
}
unsigned occurrenceCapFor(GpuCensusSection section) noexcept {
    return isDoorSection(section) ? kDoorOccurrenceCap : kFrameOccurrenceCap;
}

// The door's parenthetical breakdown, DoorUpscaler..DoorFssHeal (indices
// 1..8) -- DoorTemporalWhole (index 0) has no slot of its own; it is the
// "whole" that four of these nest inside (see gpuCensusLogLine).
constexpr const char* kDoorBreakdownNames[8] = {
    "upscaler", "motion prep", "hologram resolve+celestial", "UI resolve",
    "sharpen", "menu", "UI layer composite", "FSS heal"
};
// The in-frame breakdown, FrameHologramPasses..FrameUiLayerHdrSeed (indices 9..17): the item names, in the
// sections' order. "HDR HUD depth-stencil seed" is the name the UI layer's own 30 s line gives the same stage.
constexpr const char* kFrameBreakdownNames[] = {
    "hologram passes", "UI depth coverage", "planet", "terrain",
    "screen motion", "weapon motion", "engine velocity",
    "UI layer reissues", "HDR HUD depth-stencil seed"
};
constexpr size_t kFrameSections = sizeof(kFrameBreakdownNames) / sizeof(kFrameBreakdownNames[0]);
// Elite's own draws that EDVR alters (gpu_census.h): the game's draws timed whole, so they are
// reported on their own lines and never summed into EDVR's total. AlteredPoolFamily,
// AlteredTerrain and AlteredUiLayer are one class each (indices 18..20); the draws another fix
// wraps are one section per fix from AlteredFixFirst on (indices 21..36), reported as one item
// on the classes' line (their sum) and one by one on the line after it.
constexpr size_t kAlteredFirst = static_cast<size_t>(GpuCensusSection::AlteredPoolFamily);
constexpr size_t kSeedSection = static_cast<size_t>(GpuCensusSection::FrameUiLayerHdrSeed);
constexpr size_t kAlteredClassSections = static_cast<size_t>(GpuCensusSection::AlteredFixFirst) - kAlteredFirst;
constexpr size_t kAlteredFixFirst = static_cast<size_t>(GpuCensusSection::AlteredFixFirst);
constexpr const char* kAlteredNames[3] = {
    "pool-family draws (EDVR's slot target and shaders)", "terrain prepasses (EDVR's motion target and shader)",
    "UI draws (redirected to EDVR's layer)"
};
constexpr const char* kAlteredFixSumName = "other fix-wrapped draws";
// The fix names, in AlteredFix's order: fixed strings, never built from a draw.
constexpr const char* kAlteredFixNames[kAlteredFixCount] = {
    "panel distance", "RemLok overlay", "loading hologram", "target indicator", "night vision", "intro panel",
    "sun glare clamp", "sun glare steady", "particles", "FSS panel", "FSS reveal", "FSS dump",
    "scanner-body resolve", "loading scrim", "menu backdrop", "unnamed fix"
};
static_assert(kAlteredClassSections == 3, "one name for each altered-draw class");
static_assert(kAlteredFirst == kDoorSections + kFrameSections, "one name for each in-frame section, and the altered sections follow them");
static_assert(kSeedSection == kAlteredFirst - 1, "the seed is the last in-frame section, so its item is the last of the in-frame ones");
static_assert(kAlteredFixFirst + kAlteredFixCount == kSections, "the fix sections are the last ones");

struct SectionState {
    // Capacity 8 covers both K=2 (door) and K=8 (per-draw) sections; a door
    // section simply never asks for more than 2 of its 8 slots in a frame.
    // One instance per SECTION (not one shared instance) so a completed
    // sample is never ambiguous about which section it timed: GpuIntervals'
    // own totals are the per-section accumulator, not a label we would
    // otherwise have to attach and recover asynchronously, 3-4 frames later,
    // ourselves.
    GpuIntervals<8> sampler;
    // The calibration pair: one empty begin/end, taken once per turn
    // alongside sampler's first timed call (gpuCensusBegin), from the same
    // place in the frame. A separate instance so a completed null sample is
    // never confused with a real one.
    GpuIntervals<8> nullSampler;
    uint64_t occurrences = 0;        // every call this window, timed or not
    uint32_t skippedThisWindow = 0;  // timer begins that failed on the section's turn (not the K cap)
    // sampler.totals/nullSampler.totals never reset themselves; these are
    // their value at the start of the current window, so the window's
    // contribution is a delta.
    double baseMs = 0.0;
    unsigned baseSamples = 0, baseInvalid = 0;
    double nullBaseMs = 0.0;
    unsigned nullBaseSamples = 0;
    uint32_t turns = 0;              // turns taken, for the sampling offset
    uint32_t nullPairsTaken = 0;     // empty pairs begun into this section's nullSampler, ever (the rig reads which section keeps a turn's)
};
SectionState g_section[kSections];

// The active section's sampling this frame. Calls are timed at a stride
// spread across the frame, not the first K: a per-draw section's cost
// varies from call to call (engine velocity mostly returns without GPU
// work), so the first K would be a biased sample. The offset rotates each
// turn so every position in the draw order is sampled over a window.
int g_activeSection = 0;
unsigned g_activeCalls = 0, g_activeTimed = 0, g_activeStride = 1, g_activeOffset = 0;
// Whether this turn's null pair has already been taken (gpuCensusBegin):
// once per turn, at the first timed call, never per occurrence.
bool g_activeNullDone = false;

uint64_t g_windowStartMs = 0;
uint64_t g_windowFrames = 0;

// R (design item 3): our own cursor into gpu_frame_timing's completion
// ring, and this window's Application-render outerMs samples. Read via
// gpuFrameReadCompletions -- already exported for exactly this kind of
// consumer (perf_monitor.cpp's native benchmark collector reads the same
// ring the same way) -- rather than the native benchmark's own p50, which
// only reports at its own scoped-window close and would leave this line
// silent whenever no benchmark window happens to be open. No new accessor
// was added to gpu_frame_timing for this.
uint64_t g_p50Cursor = 0;
constexpr unsigned kP50Capacity = 8192;
double g_p50Samples[kP50Capacity];
unsigned g_p50Count = 0;

// The gap between consecutive frames of the game device's GPU work, from the
// same completions (gpu_frame_gap.h says what it is and is not: an upper bound
// on idle, with the compositor's share inside it). Fed with the Application-
// render spans' own first and last GPU ticks, so it follows the census's clock
// rules: validated spans only, one frequency, no overlap, no stall.
GpuFrameGap g_gap;

// One completion, as gpuCensusFrame reads it from the ring: a valid
// Application-render span feeds both this window's render-time median and the
// frame gap. Separate from the ring read so the rig can hand it fake spans.
void noteApplicationCompletion(const GpuSpanResult& r) {
    if (r.reason != GpuSpanReason::Valid || r.source != GpuSpanSource::ApplicationRender) return;
    if (g_p50Count < kP50Capacity) g_p50Samples[g_p50Count++] = r.outerMs;
    g_gap.feed(r.sequence, r.firstTick, r.lastTick, r.frequency);
}

struct Snapshot {
    bool occurred = false;
    double msPerFrame = 0.0;
    double perFrame = 0.0;
    unsigned samples = 0;   // the timed occurrences that completed this window (what the figure is a mean of)
};

// A section's corrected ms per call. The null mean is the timer pair's own
// overhead, measured empty; it can only ever inflate a real sample, never
// deflate it, so a null mean at or above the timed mean means the true cost
// is below what this window's timer can resolve, and reads as 0 rather than
// negative.
double correctedMsPerCall(double timedMeanMs, double nullMeanMs) noexcept {
    return std::max(0.0, timedMeanMs - nullMeanMs);
}

// `nullSt` is the section whose empty pairs calibrate this one: itself for every section but the
// fix sections, which share a turn and so one null pair a turn, taken into the first of them.
Snapshot snapshotOf(const SectionState& st, const SectionState& nullSt, uint64_t frames) noexcept {
    Snapshot s;
    s.occurred = st.occurrences > 0;
    if (!s.occurred) return s;
    const auto& t = st.sampler.totals;
    const double windowMs = t.ms - st.baseMs;
    const unsigned windowSamples = t.samples >= st.baseSamples ? t.samples - st.baseSamples : 0;
    const double msPerOccurrence = windowSamples ? windowMs / static_cast<double>(windowSamples) : 0.0;
    const auto& nt = nullSt.nullSampler.totals;
    const double nullWindowMs = nt.ms - nullSt.nullBaseMs;
    const unsigned nullWindowSamples = nt.samples >= nullSt.nullBaseSamples ? nt.samples - nullSt.nullBaseSamples : 0;
    const double nullMsPerOccurrence = nullWindowSamples ? nullWindowMs / static_cast<double>(nullWindowSamples) : 0.0;
    s.perFrame = frames ? static_cast<double>(st.occurrences) / static_cast<double>(frames) : 0.0;
    s.msPerFrame = correctedMsPerCall(msPerOccurrence, nullMsPerOccurrence) * s.perFrame;
    s.samples = windowSamples;
    return s;
}
Snapshot snapshotOf(const SectionState& st, uint64_t frames) noexcept { return snapshotOf(st, st, frames); }
// A fix section's figure: calibrated by the null pairs of the turn it shares.
Snapshot fixSnapshot(size_t i, uint64_t frames) noexcept {
    return snapshotOf(g_section[kAlteredFixFirst + i], g_section[kAlteredFixFirst], frames);
}

// The section the rotation's turn is for, and the calls per frame that decide its stride: for the
// fix sections' shared turn that is every fix's, together.
uint64_t turnOccurrences(GpuCensusSection owner) noexcept {
    if (owner != GpuCensusSection::AlteredFixFirst) return g_section[static_cast<size_t>(owner)].occurrences;
    uint64_t total = 0;
    for (size_t i = 0; i < static_cast<size_t>(kAlteredFixCount); ++i) total += g_section[kAlteredFixFirst + i].occurrences;
    return total;
}
// The next section to hold a turn: the fix sections after the first are not turns of their own.
int nextTurnOwner(int current) noexcept {
    int next = current;
    do {
        next = (next + 1) % static_cast<int>(kSections);
    } while (turnOwnerOf(static_cast<GpuCensusSection>(next)) != static_cast<GpuCensusSection>(next));
    return next;
}

void appendItem(std::string& out, const char* name, const Snapshot& s) {
    if (!out.empty()) out += ", ";
    char buf[96];
    if (!s.occurred) {
        std::snprintf(buf, sizeof(buf), "%s -", name);
    } else {
        std::snprintf(buf, sizeof(buf), "%s %.3f (%.2f/frame)", name, s.msPerFrame, s.perFrame);
    }
    out += buf;
}

// The HDR HUD seed's target as this window's seeds reported it (GpuCensusSeedScope): the first, and the first that
// differed from it, so a window the UI quality changed inside says so instead of averaging two sizes into one figure
// without a word.
struct SeedGeometry {
    uint32_t layerW = 0, layerH = 0, bytesPerPixel = 0, gameW = 0, gameH = 0;
    char format[32] = {};
};
SeedGeometry g_seedFirst, g_seedOther;
bool g_seedMixed = false;    // a seed this window reported a target unlike the first one's
uint64_t g_seedNotes = 0;    // seeds this window that reported a target

bool sameGeometry(const SeedGeometry& a, const SeedGeometry& b) noexcept {
    return a.layerW == b.layerW && a.layerH == b.layerH && a.bytesPerPixel == b.bytesPerPixel &&
           a.gameW == b.gameW && a.gameH == b.gameH && std::strcmp(a.format, b.format) == 0;
}
void resetSeedNotes() noexcept {
    g_seedFirst = SeedGeometry{};
    g_seedOther = SeedGeometry{};
    g_seedMixed = false;
    g_seedNotes = 0;
}
// "5040x4870 D32_FLOAT_S8X24_UINT (196.4 MB), seeded from the game's 4032x3896": the memory is the layer target's
// (decimal MB, as the UI layer's own creation line counts it).
void describeSeedGeometry(char* out, size_t n, const SeedGeometry& g) {
    const double mb = static_cast<double>(g.layerW) * static_cast<double>(g.layerH) * static_cast<double>(g.bytesPerPixel) / 1.0e6;
    std::snprintf(out, n, "%ux%u %s (%.1f MB), seeded from the game's %ux%u", g.layerW, g.layerH, g.format, mb, g.gameW, g.gameH);
}
// The line after the main one when the seed ran this window: what the item timed and on what. Three states of the
// target are told apart, none of them silent: one geometry, a window that saw two (the UI quality was changed inside
// it, so the figure mixes them), and seeds that ran without reporting one (the scope entered without a target).
void formatSeedDetail(char* out, size_t n, const Snapshot& s, uint64_t notes, bool mixed,
                      const SeedGeometry& first, const SeedGeometry& other) {
    char cost[64];
    if (s.samples > 0 && s.perFrame > 0.0) {
        std::snprintf(cost, sizeof(cost), "%.3f ms a seed (%u timed)", s.msPerFrame / s.perFrame, s.samples);
    } else {
        std::snprintf(cost, sizeof(cost), "no seed timed this window");
    }
    char target[448];
    if (notes == 0) {
        std::snprintf(target, sizeof(target), "the seeds reported no target");
    } else if (!mixed) {
        char one[160];
        describeSeedGeometry(one, sizeof(one), first);
        std::snprintf(target, sizeof(target), "target %s", one);
    } else {
        char a[160], b[160];
        describeSeedGeometry(a, sizeof(a), first);
        describeSeedGeometry(b, sizeof(b), other);
        std::snprintf(target, sizeof(target), "the target changed inside this window, so the figure mixes them: first %s; then %s", a, b);
    }
    std::snprintf(out, n,
                  "EDVR GPU census, the HDR HUD depth-stencil seed above (the copy of the game's depth-stencil, then the passes "
                  "that write it into the HUD layer's own): %.2f seeds a frame, %s; %s.",
                  s.perFrame, cost, target);
}

void logAndResetWindow(uint64_t now) {
    const uint64_t frames = g_windowFrames;
    const double seconds = static_cast<double>(now - g_windowStartMs) / 1000.0;

    // "door D" is the wrapped whole temporalInner (both eyes) plus the
    // other TOP-LEVEL door passes -- sharpen, menu, UI layer composite,
    // FSS heal -- never the sum of the parenthetical breakdown: upscaler,
    // motion prep, hologram resolve and UI resolve all run INSIDE
    // temporalInner, so DoorTemporalWhole's own ms already include them.
    // Adding those four again would double their cost. The same
    // reasoning does not apply to "in-frame F": its nine parts are
    // independent call sites (no one of them wraps another), so F is
    // their direct sum.
    const Snapshot doorWhole = snapshotOf(g_section[static_cast<size_t>(GpuCensusSection::DoorTemporalWhole)], frames);
    double doorTotal = doorWhole.msPerFrame;
    std::string doorItems;
    for (int i = 0; i < 8; ++i) {
        const Snapshot s = snapshotOf(g_section[1 + static_cast<size_t>(i)], frames);
        appendItem(doorItems, kDoorBreakdownNames[i], s);
        const auto section = static_cast<GpuCensusSection>(1 + i);
        if (section == GpuCensusSection::DoorSharpen || section == GpuCensusSection::DoorMenu ||
            section == GpuCensusSection::DoorUiLayerComposite || section == GpuCensusSection::DoorFssHeal) {
            doorTotal += s.msPerFrame;
        }
    }
    double frameTotal = 0.0;
    std::string frameItems;
    Snapshot seedSnap;
    for (size_t i = 0; i < kFrameSections; ++i) {
        const Snapshot s = snapshotOf(g_section[kDoorSections + i], frames);
        appendItem(frameItems, kFrameBreakdownNames[i], s);
        frameTotal += s.msPerFrame;
        if (kDoorSections + i == kSeedSection) seedSnap = s;
    }

    // Elite's own draws that EDVR alters (gpu_census.h): the game's draw timed whole, so
    // these are NOT EDVR's cost and stay out of both totals above. The draws another fix wraps
    // are one item on this line (their sum: what the line said before it was split) and are
    // named fix by fix on the line after it.
    double alteredTotal = 0.0;
    std::string alteredItems;
    for (size_t i = 0; i < kAlteredClassSections; ++i) {
        const Snapshot s = snapshotOf(g_section[kAlteredFirst + i], frames);
        appendItem(alteredItems, kAlteredNames[i], s);
        alteredTotal += s.msPerFrame;
    }
    Snapshot fixSum;
    std::string fixItems;
    for (size_t i = 0; i < static_cast<size_t>(kAlteredFixCount); ++i) {
        const Snapshot s = fixSnapshot(i, frames);
        appendItem(fixItems, kAlteredFixNames[i], s);
        fixSum.occurred = fixSum.occurred || s.occurred;
        fixSum.msPerFrame += s.msPerFrame;
        fixSum.perFrame += s.perFrame;
    }
    appendItem(alteredItems, kAlteredFixSumName, fixSum);
    alteredTotal += fixSum.msPerFrame;

    uint64_t spansTimed = 0, spansSkipped = 0;
    for (const auto& st : g_section) {
        const auto& t = st.sampler.totals;
        spansTimed += t.samples >= st.baseSamples ? t.samples - st.baseSamples : 0;
        spansSkipped += st.skippedThisWindow;
        spansSkipped += t.invalid >= st.baseInvalid ? t.invalid - st.baseInvalid : 0;
    }

    // The timer floor: every section's null pairs this window, pooled (not
    // per-section then averaged, so a section that took few turns does not
    // weigh the same as one that ran the whole window) into one mean --
    // GpuIntervals keeps running sums, not individual samples, so a mean is
    // what the data actually supports; a median would need its own sample
    // buffer for no real gain, since these pairs are already close together.
    double nullMsTotal = 0.0;
    uint64_t nullSamplesTotal = 0;
    for (const auto& st : g_section) {
        const auto& nt = st.nullSampler.totals;
        nullMsTotal += nt.ms - st.nullBaseMs;
        nullSamplesTotal += nt.samples >= st.nullBaseSamples ? nt.samples - st.nullBaseSamples : 0;
    }
    char floorBuf[32];
    if (nullSamplesTotal) {
        std::snprintf(floorBuf, sizeof(floorBuf), "%.1f us/pair",
                      (nullMsTotal / static_cast<double>(nullSamplesTotal)) * 1000.0);
    } else {
        std::snprintf(floorBuf, sizeof(floorBuf), "-");
    }

    // R covers the game's rendering, EDVR's in-frame work and the door on the
    // game's device (not the XR device's transfer and compose), so the game's
    // own share is roughly R minus EDVR's total: a median less a mean, hence ~.
    // That share stays raw, negative included, when EDVR's corrected total
    // still exceeds R: negative is the witness that something still
    // overcounts, not a fault to hide, so the line says so instead.
    char rBuf[128];
    if (g_p50Count) {
        std::sort(g_p50Samples, g_p50Samples + g_p50Count);
        const double r = g_p50Samples[g_p50Count / 2];
        const double edvrTotal = doorTotal + frameTotal;
        if (edvrTotal > r) {
            std::snprintf(rBuf, sizeof(rBuf), "%.3f ms/frame (game ~%.3f) (census over the frame total)",
                          r, r - edvrTotal);
        } else {
            std::snprintf(rBuf, sizeof(rBuf), "%.3f ms/frame (game ~%.3f)", r, r - edvrTotal);
        }
    } else {
        std::snprintf(rBuf, sizeof(rBuf), "-");
    }

    // The gap between consecutive frames of the game device's GPU work
    // (gpu_frame_gap.h): its p50/p95 and pair count ride on this line; the line
    // after it says what the figure is and is not (the compositor's share is in it).
    const GpuFrameGap::Report gap = g_gap.finishWindow();
    char gapBrief[96];
    formatGapBrief(gapBrief, sizeof(gapBrief), gap);

    Log::get().note(
        "EDVR GPU census: %.0f s, %llu frames; EDVR ~%.3f ms/frame = door %.3f "
        "(%s) + in-frame %.3f (%s); application render p50 %s; %s; "
        "timer floor %s; spans timed %llu, failed %llu.",
        seconds, static_cast<unsigned long long>(frames), doorTotal + frameTotal, doorTotal,
        doorItems.c_str(), frameTotal, frameItems.c_str(), rBuf, gapBrief, floorBuf,
        static_cast<unsigned long long>(spansTimed), static_cast<unsigned long long>(spansSkipped));
    // The HDR HUD seed's own line, only when a seed ran: "-" on the main line alone means none did (the layer is
    // off, or it drew no HUD that tests the game's depth or stencil); a line here means the item above is a
    // measurement, and says which target it was taken on.
    if (seedSnap.occurred) {
        char seedDetail[1024];
        formatSeedDetail(seedDetail, sizeof(seedDetail), seedSnap, g_seedNotes, g_seedMixed, g_seedFirst, g_seedOther);
        Log::get().note("%s", seedDetail);
    }
    // Elite's own draws that EDVR alters: what the AA path's GPU cost looks like from
    // outside, inside draws the census would otherwise count as the game's. Each is the
    // game's draw timed whole, so the figures INCLUDE the game's own work in those draws.
    Log::get().note(
        "EDVR GPU census, Elite's own draws that EDVR alters (each is the game's draw timed whole, so a figure "
        "includes the game's own work in it, not only what EDVR adds, and none of it is in EDVR ~%.3f above): "
        "%s; together %.3f ms/frame; \"-\" means no such draw ran this window.",
        doorTotal + frameTotal, alteredItems.c_str(), alteredTotal);
    // The "other fix-wrapped draws" above, one fix at a time: which code wraps the draws that cost.
    Log::get().note(
        "EDVR GPU census, the other fix-wrapped draws above by the fix that wraps each (the same draws, the game's own "
        "work in each figure as above): %s; \"-\" means no draw of that fix ran this window.",
        fixItems.c_str());
    char gapDetail[900];
    formatGapDetail(gapDetail, sizeof(gapDetail), gap);
    Log::get().note("%s", gapDetail);

    for (auto& st : g_section) {
        st.baseMs = st.sampler.totals.ms;
        st.baseSamples = st.sampler.totals.samples;
        st.baseInvalid = st.sampler.totals.invalid;
        st.nullBaseMs = st.nullSampler.totals.ms;
        st.nullBaseSamples = st.nullSampler.totals.samples;
        st.occurrences = 0;
        st.skippedThisWindow = 0;
    }
    resetSeedNotes();
    g_windowFrames = 0;
    g_windowStartMs = now;
    g_p50Count = 0;
}

} // namespace

bool gpuCensusBegin(ID3D11DeviceContext* ctx, GpuCensusSection section) noexcept {
    if (section >= GpuCensusSection::Count) return false;
    SectionState& st = g_section[static_cast<size_t>(section)];
    ++st.occurrences;   // Cheap and unconditional: the estimate needs every occurrence counted.
    const GpuCensusSection owner = turnOwnerOf(section);
    if (static_cast<int>(owner) != g_activeSection) return false;
    const unsigned call = g_activeCalls++;
    if (call < g_activeOffset || (call - g_activeOffset) % g_activeStride != 0) return false;
    if (g_activeTimed >= occurrenceCapFor(section)) return false;   // K reached: counted, not timed
    ++g_activeTimed;
    if (!g_activeNullDone) {
        // The turn's first timed call also times one empty pair, immediately
        // before the real one, nothing between: the timer's own overhead,
        // from the same place in the frame as the sample it calibrates
        // (logAndResetWindow's "timer floor", snapshotOf's correction). Begin
        // and End are safe unconditionally either way (gpu_census.h).
        g_activeNullDone = true;
        SectionState& nullSt = g_section[static_cast<size_t>(owner)];   // the turn's owner keeps the turn's pair
        ++nullSt.nullPairsTaken;
        nullSt.nullSampler.begin(ctx);
        nullSt.nullSampler.end(ctx);
    }
    if (!st.sampler.begin(ctx)) {
        ++st.skippedThisWindow;
        return false;
    }
    return true;
}

void gpuCensusNoteSeedTarget(const GpuCensusSeedTarget& target) noexcept {
    SeedGeometry g;
    g.layerW = target.layerW;
    g.layerH = target.layerH;
    g.bytesPerPixel = target.bytesPerPixel;
    g.gameW = target.gameW;
    g.gameH = target.gameH;
    std::snprintf(g.format, sizeof(g.format), "%s", target.format ? target.format : "unknown format");
    if (g_seedNotes == 0) {
        g_seedFirst = g;
    } else if (!g_seedMixed && !sameGeometry(g_seedFirst, g)) {
        g_seedMixed = true;
        g_seedOther = g;
    }
    ++g_seedNotes;
}

void gpuCensusEnd(ID3D11DeviceContext* ctx, GpuCensusSection section) noexcept {
    if (section >= GpuCensusSection::Count) return;
    // Safe even when the matching Begin returned false: GpuIntervals::end()
    // is a no-op with nothing open (gpu_interval.h), so a plain Begin/End
    // pair at a call site never needs to branch on Begin's result.
    g_section[static_cast<size_t>(section)].sampler.end(ctx);
}

void gpuCensusFrame(ID3D11DeviceContext* ctx) noexcept {
    for (auto& st : g_section) { st.sampler.poll(ctx); st.nullSampler.poll(ctx); }

    ++g_windowFrames;

    // Next frame's section, and its stride from this window's calls per frame.
    g_activeSection = nextTurnOwner(g_activeSection);
    SectionState& next = g_section[static_cast<size_t>(g_activeSection)];
    const unsigned cap = occurrenceCapFor(static_cast<GpuCensusSection>(g_activeSection));
    const double perFrame = static_cast<double>(turnOccurrences(static_cast<GpuCensusSection>(g_activeSection))) /
                            static_cast<double>(g_windowFrames);
    g_activeStride = perFrame > cap ? static_cast<unsigned>(perFrame / cap) : 1u;
    g_activeOffset = g_activeStride > 1 ? next.turns % g_activeStride : 0u;
    ++next.turns;
    g_activeCalls = g_activeTimed = 0;
    g_activeNullDone = false;
    const uint64_t now = GetTickCount64();
    if (g_windowStartMs == 0) g_windowStartMs = now;

    GpuFrameSnapshot completions[32]{};
    uint64_t dropped = 0;
    const unsigned n = gpuFrameReadCompletions(g_p50Cursor, completions, 32, dropped);
    for (unsigned i = 0; i < n; ++i) {
        const GpuFrameSnapshot& c = completions[i];
        if (!c.haveResult) continue;
        noteApplicationCompletion(c.result);
    }

    if (now - g_windowStartMs < 30000) return;
    logAndResetWindow(now);
}

void gpuCensusShutdown() noexcept {
    for (auto& st : g_section) { st.sampler.reset(); st.nullSampler.reset(); }
    resetSeedNotes();
}

} // namespace edvr
