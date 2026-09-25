#include "gpu_census.h"
#include "gpu_interval.h"
#include "gpu_frame_timing.h"
#include "../common/log.h"
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <cstdio>
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
// The in-frame breakdown, FrameHologramPasses..FrameFoveation (indices 9..18).
constexpr const char* kFrameBreakdownNames[10] = {
    "hologram passes", "UI depth coverage", "planet", "terrain",
    "screen motion", "weapon motion", "engine velocity",
    "UI layer reissues", "eye mask", "foveation"
};

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

struct Snapshot {
    bool occurred = false;
    double msPerFrame = 0.0;
    double perFrame = 0.0;
};

// A section's corrected ms per call. The null mean is the timer pair's own
// overhead, measured empty; it can only ever inflate a real sample, never
// deflate it, so a null mean at or above the timed mean means the true cost
// is below what this window's timer can resolve, and reads as 0 rather than
// negative.
double correctedMsPerCall(double timedMeanMs, double nullMeanMs) noexcept {
    return std::max(0.0, timedMeanMs - nullMeanMs);
}

Snapshot snapshotOf(const SectionState& st, uint64_t frames) noexcept {
    Snapshot s;
    s.occurred = st.occurrences > 0;
    if (!s.occurred) return s;
    const auto& t = st.sampler.totals;
    const double windowMs = t.ms - st.baseMs;
    const unsigned windowSamples = t.samples >= st.baseSamples ? t.samples - st.baseSamples : 0;
    const double msPerOccurrence = windowSamples ? windowMs / static_cast<double>(windowSamples) : 0.0;
    const auto& nt = st.nullSampler.totals;
    const double nullWindowMs = nt.ms - st.nullBaseMs;
    const unsigned nullWindowSamples = nt.samples >= st.nullBaseSamples ? nt.samples - st.nullBaseSamples : 0;
    const double nullMsPerOccurrence = nullWindowSamples ? nullWindowMs / static_cast<double>(nullWindowSamples) : 0.0;
    s.perFrame = frames ? static_cast<double>(st.occurrences) / static_cast<double>(frames) : 0.0;
    s.msPerFrame = correctedMsPerCall(msPerOccurrence, nullMsPerOccurrence) * s.perFrame;
    return s;
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

void logAndResetWindow(uint64_t now) {
    const uint64_t frames = g_windowFrames;
    const double seconds = static_cast<double>(now - g_windowStartMs) / 1000.0;

    // "door D" is the wrapped whole temporalInner (both eyes) plus the
    // other TOP-LEVEL door passes -- sharpen, menu, UI layer composite,
    // FSS heal -- never the sum of the parenthetical breakdown: upscaler,
    // motion prep, hologram resolve and UI resolve all run INSIDE
    // temporalInner, so DoorTemporalWhole's own ms already include them.
    // Adding those four again would double their cost. The same
    // reasoning does not apply to "in-frame F": its ten parts are
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
    for (int i = 0; i < 10; ++i) {
        const Snapshot s = snapshotOf(g_section[kDoorSections + static_cast<size_t>(i)], frames);
        appendItem(frameItems, kFrameBreakdownNames[i], s);
        frameTotal += s.msPerFrame;
    }

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

    Log::get().note(
        "EDVR GPU census: %.0f s, %llu frames; EDVR ~%.3f ms/frame = door %.3f "
        "(%s) + in-frame %.3f (%s); application render p50 %s; "
        "timer floor %s; spans timed %llu, failed %llu.",
        seconds, static_cast<unsigned long long>(frames), doorTotal + frameTotal, doorTotal,
        doorItems.c_str(), frameTotal, frameItems.c_str(), rBuf, floorBuf,
        static_cast<unsigned long long>(spansTimed), static_cast<unsigned long long>(spansSkipped));

    for (auto& st : g_section) {
        st.baseMs = st.sampler.totals.ms;
        st.baseSamples = st.sampler.totals.samples;
        st.baseInvalid = st.sampler.totals.invalid;
        st.nullBaseMs = st.nullSampler.totals.ms;
        st.nullBaseSamples = st.nullSampler.totals.samples;
        st.occurrences = 0;
        st.skippedThisWindow = 0;
    }
    g_windowFrames = 0;
    g_windowStartMs = now;
    g_p50Count = 0;
}

} // namespace

bool gpuCensusBegin(ID3D11DeviceContext* ctx, GpuCensusSection section) noexcept {
    if (section >= GpuCensusSection::Count) return false;
    SectionState& st = g_section[static_cast<size_t>(section)];
    ++st.occurrences;   // Cheap and unconditional: the estimate needs every occurrence counted.
    if (static_cast<int>(section) != g_activeSection) return false;
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
        st.nullSampler.begin(ctx);
        st.nullSampler.end(ctx);
    }
    if (!st.sampler.begin(ctx)) {
        ++st.skippedThisWindow;
        return false;
    }
    return true;
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
    g_activeSection = (g_activeSection + 1) % static_cast<int>(kSections);
    SectionState& next = g_section[static_cast<size_t>(g_activeSection)];
    const unsigned cap = occurrenceCapFor(static_cast<GpuCensusSection>(g_activeSection));
    const double perFrame = static_cast<double>(next.occurrences) / static_cast<double>(g_windowFrames);
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
        if (!c.haveResult || c.result.reason != GpuSpanReason::Valid ||
            c.result.source != GpuSpanSource::ApplicationRender) continue;
        if (g_p50Count < kP50Capacity) g_p50Samples[g_p50Count++] = c.result.outerMs;
    }

    if (now - g_windowStartMs < 30000) return;
    logAndResetWindow(now);
}

void gpuCensusShutdown() noexcept {
    for (auto& st : g_section) { st.sampler.reset(); st.nullSampler.reset(); }
}

} // namespace edvr
