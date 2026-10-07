// Issue #38's per-feature GPU cost census (gpu_census.h/.cpp): the
// estimator math, the calibration math (a section's own empty-pair
// overhead, subtracted from its timed mean), the rotation and its K cap, a
// real GpuTimer round trip on WARP including the calibration pair, and the
// 30 s line's format -- including "-" for a section that never occurred
// and for a window with no null samples.
//
// Whitebox, tools\ui_depth_test's and hologram_depth_test's own
// convention: the production .cpp is included directly so this rig can
// reach its rotation state and its window formatter, neither of which
// gpu_census.h exposes on purpose (nothing outside gpu_census.cpp needs
// them). gpu_timing.cpp/gpu_frame_timing.cpp/gpu_span_d3d11.cpp are real,
// linked sources (build.bat), so the WARP round trip is a real disjoint
// timestamp pair, not a fake.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../src/common/system_d3d11.h"
#include "../../src/common/log.h"

using Microsoft::WRL::ComPtr;

namespace edvr {
// Captures the formatted lines instead of writing them anywhere, so the
// format checks below can inspect the actual text -- hologram_depth_test's
// own convention, not tools\ui_depth_test's no-op stub. g_lastLog is the
// census's MAIN line ("EDVR GPU census: ..."), which the older checks read;
// g_lines is every line, in order, for the lines that follow it (the frame
// gap's, the altered draws').
std::string g_lastLog;
std::vector<std::string> g_lines;
Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char* fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_lines.push_back(buf);
    if (std::strncmp(buf, "EDVR GPU census:", 16) == 0) g_lastLog = buf;
    std::fputs(buf, stdout);
    std::fputc('\n', stdout);
}
} // namespace edvr

#include "../../src/d3d11/gpu_census.cpp"

namespace {
using namespace edvr;
unsigned g_checks = 0;
void check(bool ok, const char* why) { ++g_checks; if (!ok) throw std::runtime_error(why); }
void hr(HRESULT h) { check(h == S_OK, "D3D operation failed"); }

struct ExpectedGpuOwner final {
    GpuCensusSection section;
    GpuCensusOwner owner;
    GpuCensusAttribution attribution;
    const char* sharedConsumer;
};

void ownershipMapAndAggregateCases() {
    using O = GpuCensusOwner;
    using A = GpuCensusAttribution;
    const ExpectedGpuOwner expected[] = {
        {GpuCensusSection::DoorTemporalWhole, O::TemporalAa, A::Direct, ""},
        {GpuCensusSection::DoorUpscaler, O::TemporalAa, A::NestedBreakdown, ""},
        {GpuCensusSection::DoorMotionPrep, O::TemporalAa, A::NestedBreakdown, ""},
        {GpuCensusSection::DoorHologramResolve, O::TemporalAa, A::NestedBreakdown, ""},
        {GpuCensusSection::DoorUiResolve, O::TemporalAa, A::NestedBreakdown, ""},
        {GpuCensusSection::DoorSharpen, O::TemporalAa, A::Direct, ""},
        {GpuCensusSection::DoorMenu, O::Core, A::Direct, ""},
        {GpuCensusSection::DoorUiLayerComposite, O::TemporalAa, A::Direct, ""},
        {GpuCensusSection::DoorFssHeal, O::Scanners, A::Direct, ""},
        {GpuCensusSection::FrameHologramPasses, O::TemporalAa, A::Direct, ""},
        {GpuCensusSection::FrameUiDepthCoverage, O::TemporalAa, A::Direct, ""},
        {GpuCensusSection::FramePlanet, O::TemporalAa, A::Direct, ""},
        {GpuCensusSection::FrameScreenMotion, O::TemporalAa, A::Direct, ""},
        {GpuCensusSection::FrameWeaponMotion, O::TemporalAa, A::Direct, "on-foot-panel"},
        {GpuCensusSection::FrameEngineVelocity, O::TemporalAa, A::Direct, ""},
        {GpuCensusSection::FrameUiLayerReissues, O::TemporalAa, A::Direct, ""},
        {GpuCensusSection::FrameUiLayerHdrSeed, O::TemporalAa, A::Direct, ""},
        {GpuCensusSection::FrameWorldResolve, O::TemporalAa, A::Direct, ""},
        {GpuCensusSection::FrameWorldMips, O::TemporalAa, A::Direct, ""},
        {GpuCensusSection::FrameWorldLayer, O::TemporalAa, A::Direct, ""},
        {GpuCensusSection::AlteredPoolFamily, O::TemporalAa, A::WrappedGameDraw, ""},
        {GpuCensusSection::AlteredUiLayer, O::TemporalAa, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::Panel), O::OnFootPanel, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::Remlok), O::CockpitVisuals, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::Holo), O::Intro, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::TargetSharp), O::CockpitVisuals, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::NightVision), O::CockpitVisuals, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::IntroPanel), O::Intro, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::GlareClamp), O::CockpitVisuals, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::GlareSteady), O::CockpitVisuals, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::Particle), O::CockpitVisuals, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::FssPanel), O::Scanners, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::FssReveal), O::Scanners, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::FssDump), O::Scanners, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::ResolveBind), O::Scanners, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::Scrim), O::Intro, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::Backdrop), O::Intro, A::WrappedGameDraw, ""},
        {alteredFixSectionOf(AlteredFix::Unnamed), O::Core, A::WrappedGameDraw, ""},
    };
    static_assert(static_cast<uint8_t>(GpuCensusOwner::TemporalAa) == 0 &&
                  static_cast<uint8_t>(GpuCensusOwner::CockpitVisuals) == 1 &&
                  static_cast<uint8_t>(GpuCensusOwner::Exposure) == 2 &&
                  static_cast<uint8_t>(GpuCensusOwner::Scanners) == 3 &&
                  static_cast<uint8_t>(GpuCensusOwner::Intro) == 4 &&
                  static_cast<uint8_t>(GpuCensusOwner::OnFootPanel) == 5 &&
                  static_cast<uint8_t>(GpuCensusOwner::Comfort) == 6 &&
                  static_cast<uint8_t>(GpuCensusOwner::Performance) == 7 &&
                  static_cast<uint8_t>(GpuCensusOwner::Diagnostics) == 8 &&
                  static_cast<uint8_t>(GpuCensusOwner::Core) == 9 &&
                  static_cast<uint8_t>(GpuCensusOwner::Count) == 10,
                  "logical owner IDs are stable");
    static_assert(sizeof(expected) / sizeof(expected[0]) ==
                  static_cast<size_t>(GpuCensusSection::Count),
                  "the test pins every concrete census section exactly once");

    bool seen[static_cast<size_t>(GpuCensusSection::Count)]{};
    std::vector<GpuCensusOwnerObservation> observations;
    observations.reserve(sizeof(expected) / sizeof(expected[0]));
    double expectedTemporalMs = 0.0;
    unsigned expectedTemporalDirect = 0;
    bool weaponMotionShared = false;
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        const auto& row = expected[i];
        const size_t index = static_cast<size_t>(row.section);
        check(index < static_cast<size_t>(GpuCensusSection::Count) && !seen[index],
              "ownership golden rows are unique valid census sections");
        seen[index] = true;
        const GpuCensusOwnership actual = gpuCensusOwnership(row.section);
        check(actual.owner == row.owner && actual.attribution == row.attribution &&
                  actual.implementation == GpuCensusImplementation::Legacy &&
                  std::strcmp(actual.sharedConsumer, row.sharedConsumer) == 0,
              "production owner map matches the canonical owner/attribution row");
        const double ms = static_cast<double>(index + 1) * 0.125;
        observations.push_back({row.section, true, 2, ms});
        if (row.owner == O::TemporalAa && row.attribution == A::Direct) {
            ++expectedTemporalDirect;
            expectedTemporalMs += ms;
        }
        if (row.section == GpuCensusSection::FrameWeaponMotion)
            weaponMotionShared = row.owner == O::TemporalAa &&
                                 std::strcmp(row.sharedConsumer, "on-foot-panel") == 0;
    }
    for (bool present : seen) check(present, "every concrete section has one pinned owner row");
    const GpuCensusOwnership countSentinel = gpuCensusOwnership(GpuCensusSection::Count);
    check(countSentinel.attribution == A::Unowned && countSentinel.owner == O::Core,
          "Count sentinel is explicitly unowned and cannot create a cost bucket");
    check(weaponMotionShared,
          "weapon-motion GPU work remains temporal-aa with on-foot-panel as a shared consumer");

    for (size_t ownerIndex = 0; ownerIndex < static_cast<size_t>(O::Count); ++ownerIndex) {
        const auto owner = static_cast<O>(ownerIndex);
        unsigned direct = 0;
        double ms = 0.0;
        for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
            if (expected[i].owner != owner || expected[i].attribution != A::Direct) continue;
            ++direct;
            const size_t section = static_cast<size_t>(expected[i].section);
            ms += static_cast<double>(section + 1) * 0.125;
        }
        const GpuCensusOwnerSummary summary = aggregateGpuCensusOwner(
            owner, observations.data(), observations.size());
        check(summary.declaredDirectScopes == direct && summary.providedScopes == direct &&
                  summary.occurredScopes == direct && summary.sampledScopes == direct &&
                  summary.timestampSamples == direct * 2 && summary.hasGpuEstimate == (direct != 0) &&
                  !summary.partial() && std::fabs(summary.msPerFrame - ms) < 1.0e-9,
              "owner aggregate sums only observed direct scopes with sampled GPU data");
    }

    std::vector<GpuCensusOwnerObservation> duplicate = observations;
    duplicate.push_back(observations.front());
    const auto whole = aggregateGpuCensusOwner(O::TemporalAa, observations.data(), observations.size());
    const auto deduplicated = aggregateGpuCensusOwner(O::TemporalAa, duplicate.data(), duplicate.size());
    check(deduplicated.providedScopes == whole.providedScopes &&
              deduplicated.timestampSamples == whole.timestampSamples &&
              std::fabs(deduplicated.msPerFrame - whole.msPerFrame) < 1.0e-9,
          "duplicate section observations cannot inflate an owner subtotal");

    auto missing = observations;
    missing.erase(std::remove_if(missing.begin(), missing.end(), [](const auto& row) {
        return row.section == GpuCensusSection::FramePlanet;
    }), missing.end());
    const auto missingSummary = aggregateGpuCensusOwner(O::TemporalAa, missing.data(), missing.size());
    check(missingSummary.declaredDirectScopes == expectedTemporalDirect &&
              missingSummary.providedScopes + 1 == missingSummary.declaredDirectScopes &&
              missingSummary.partial() && missingSummary.hasGpuEstimate,
          "missing direct scopes lower coverage without becoming measured zeroes");

    auto notOccurred = observations;
    for (auto& row : notOccurred) if (row.section == GpuCensusSection::FramePlanet) {
        row.occurred = false;
        row.samples = 0;
    }
    const auto absentSummary = aggregateGpuCensusOwner(O::TemporalAa, notOccurred.data(), notOccurred.size());
    check(absentSummary.providedScopes == absentSummary.declaredDirectScopes &&
              absentSummary.occurredScopes + 1 == absentSummary.declaredDirectScopes &&
              !absentSummary.partial() && absentSummary.hasGpuEstimate &&
              absentSummary.msPerFrame < whole.msPerFrame,
          "provided quiet scopes are complete evidence and contribute no fabricated cost");

    auto untimed = observations;
    for (auto& row : untimed) if (row.section == GpuCensusSection::FramePlanet) row.samples = 0;
    const auto untimedSummary = aggregateGpuCensusOwner(O::TemporalAa, untimed.data(), untimed.size());
    check(untimedSummary.occurredScopes == untimedSummary.declaredDirectScopes &&
              untimedSummary.sampledScopes + 1 == untimedSummary.occurredScopes &&
              untimedSummary.partial() && untimedSummary.hasGpuEstimate &&
              untimedSummary.msPerFrame < whole.msPerFrame,
          "an occurred but untimed scope lowers sampled coverage without adding an inferred estimate");

    std::vector<GpuCensusOwnerObservation> noSamples;
    for (const auto& row : expected) if (row.owner == O::TemporalAa && row.attribution == A::Direct)
        noSamples.push_back({row.section, true, 0, 99.0});
    const auto unmeasured = aggregateGpuCensusOwner(O::TemporalAa, noSamples.data(), noSamples.size());
    check(unmeasured.providedScopes == unmeasured.declaredDirectScopes &&
              unmeasured.occurredScopes == unmeasured.declaredDirectScopes &&
              unmeasured.sampledScopes == 0 && unmeasured.timestampSamples == 0 &&
              !unmeasured.hasGpuEstimate && unmeasured.msPerFrame == 0.0 &&
              unmeasured.partial(),
          "zero timer samples render as unmeasured, never as caller-supplied numeric GPU cost");

    const auto noObservations = aggregateGpuCensusOwner(O::TemporalAa, nullptr, 0);
    check(noObservations.providedScopes == 0 && noObservations.declaredDirectScopes == expectedTemporalDirect &&
              noObservations.partial() && !noObservations.hasGpuEstimate &&
              noObservations.msPerFrame == 0.0,
          "missing owner observations expose static scope coverage and no fabricated estimate");
}

struct Runtime {
    decltype(&D3D11CreateDevice) create = edvr::systemD3D11CreateDevice();
    Runtime() { check(create != nullptr, "System32 D3D11CreateDevice"); }
};
struct Device {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    explicit Device(Runtime& rt) {
        D3D_FEATURE_LEVEL level{};
        hr(rt.create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                      D3D11_SDK_VERSION, &dev, &level, &ctx));
    }
};

ComPtr<ID3D11Texture2D> makeTarget(ID3D11Device* dev, ComPtr<ID3D11RenderTargetView>& rtvOut) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 8;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> tex;
    hr(dev->CreateTexture2D(&td, nullptr, &tex));
    hr(dev->CreateRenderTargetView(tex.Get(), nullptr, &rtvOut));
    return tex;
}
} // namespace

namespace edvr {
namespace {

// What the HDR HUD seed's target is at Sean's two UI qualities on his headset (his log: the layer's depth-stencil is
// 5040x4870 at 125 and 4032x3896 at 100, D32S8 both, 8 bytes a pixel). The game's own depth-stencil is one size in
// both, since only the layer's follows the key.
const GpuCensusSeedTarget kSeedAt125 = {5040, 4870, 8, 3024, 2922, "D32_FLOAT_S8X24_UINT"};
const GpuCensusSeedTarget kSeedAt100 = {4032, 3896, 8, 3024, 2922, "D32_FLOAT_S8X24_UINT"};
constexpr const char* kSeedDetailPrefix = "EDVR GPU census, the HDR HUD depth-stencil seed above";

// ---- 1: the estimator math, as a pure function -------------------------
void estimatorMathCases() {
    SectionState st;
    {
        const auto snap = snapshotOf(st, 100);
        check(!snap.occurred, "estimator: zero occurrences reads as not-occurred ('-')");
    }
    {
        st.occurrences = 3;   // no sample completed yet -- still "occurred", 0 ms/frame
        const auto snap = snapshotOf(st, 100);
        check(snap.occurred, "estimator: an occurrence with no completed sample still counts as occurred");
        check(snap.msPerFrame == 0.0, "estimator: no completed sample means 0 ms/frame, never a guess");
        check(std::fabs(snap.perFrame - 0.03) < 1e-9, "estimator: occurrences/frame = occurrences / frames");
    }
    {
        st = SectionState{};
        st.occurrences = 10;
        st.sampler.totals.ms = 5.0;
        st.sampler.totals.samples = 5;   // 5 completed samples averaging 1 ms each
        const auto snap = snapshotOf(st, 50);   // 10 occurrences / 50 frames = 0.2/frame
        check(std::fabs(snap.perFrame - 0.2) < 1e-9, "estimator: occurrences/frame with real samples");
        check(std::fabs(snap.msPerFrame - 0.2) < 1e-9, "estimator: (ms/occurrence) * (occurrences/frame)");
    }
    {
        // A running GpuIntervals total never resets itself -- the window's
        // own contribution is a delta off a baseline, not the raw total.
        st = SectionState{};
        st.occurrences = 4;
        st.baseMs = 20.0; st.baseSamples = 10;
        st.sampler.totals.ms = 23.0; st.sampler.totals.samples = 12;   // +3 ms over +2 samples this window
        const auto snap = snapshotOf(st, 20);   // 4/20 = 0.2/frame
        check(std::fabs(snap.msPerFrame - (1.5 * 0.2)) < 1e-9,
              "estimator: reads a delta off the running totals, not the totals themselves");
    }
}

// ---- 1b: the calibration math, as a pure function -----------------------
void calibrationMathCases() {
    check(std::fabs(correctedMsPerCall(0.010, 0.008) - 0.002) < 1e-9,
          "calibration: corrected ms/call = timed mean - null mean");
    check(correctedMsPerCall(0.010, 0.020) == 0.0,
          "calibration: a null mean above the timed mean clamps to 0, not negative");
    check(correctedMsPerCall(0.010, 0.010) == 0.0,
          "calibration: a null mean equal to the timed mean also clamps to 0");
    check(correctedMsPerCall(0.010, 0.0) == 0.010,
          "calibration: no null samples yet (mean 0) leaves the timed mean uncorrected");
}

// ---- 2/3: rotation (one active section, the K cap) and a real WARP round trip
void rotationAndRealTimerCase(Device& d) {
    check(gpuTimingBind(d.dev.Get(), d.ctx.Get()), "bind canonical WARP timer owner");
    check(occurrenceCapFor(GpuCensusSection::DoorSharpen) == 4, "K cap: a door section caps at 4 (both eyes, two fovea crops each)");
    check(occurrenceCapFor(GpuCensusSection::FrameUiLayerReissues) == 8, "K cap: a per-draw section caps at 8");
    check(isDoorSection(GpuCensusSection::DoorFssHeal), "section split: the last door section is still door");
    check(!isDoorSection(GpuCensusSection::FrameHologramPasses), "section split: the first in-frame section is not door");

    ComPtr<ID3D11RenderTargetView> rtv;
    auto tex = makeTarget(d.dev.Get(), rtv);
    const float colour[4] = {0.2f, 0.4f, 0.6f, 1.0f};

    for (auto& s : g_section) s = SectionState{};
    g_activeSection = static_cast<int>(GpuCensusSection::DoorSharpen);
    g_activeCalls = g_activeTimed = 0;
    g_activeStride = 1;
    g_activeOffset = 0;
    g_activeNullDone = false;
    auto& st = g_section[static_cast<size_t>(GpuCensusSection::DoorSharpen)];

    // Exactly one section is active: a call for any other section declines
    // -- cheaply, one branch and an increment -- and never touches a timer.
    check(!gpuCensusBegin(d.ctx.Get(), GpuCensusSection::DoorMenu),
          "rotation: a non-active section's begin returns false");
    check(g_section[static_cast<size_t>(GpuCensusSection::DoorMenu)].occurrences == 1,
          "rotation: a non-active section still counts its occurrence");
    gpuCensusEnd(d.ctx.Get(), GpuCensusSection::DoorMenu);   // must be a harmless no-op

    // Four real occurrences this "frame": a genuine Begin/Clear/End pair
    // each time, within the door cap of 4.
    for (int i = 0; i < 4; ++i) {
        check(gpuCensusBegin(d.ctx.Get(), GpuCensusSection::DoorSharpen),
              "rotation: the active section's begin succeeds within its cap");
        d.ctx->ClearRenderTargetView(rtv.Get(), colour);
        gpuCensusEnd(d.ctx.Get(), GpuCensusSection::DoorSharpen);
    }
    check(g_activeTimed == 4, "rotation: four occurrences consumed the door cap");
    check(st.occurrences == 4, "rotation: every occurrence counted");
    // Calibration: the turn's first timed call (i == 0 above) also armed the
    // null pair -- once, not once per occurrence.
    check(g_activeNullDone, "calibration: the turn's first timed call armed the null pair");

    // A fifth, same frame: the K cap declines it before the sampler is
    // ever touched -- no lease spent, no change to the timer's own totals.
    const unsigned samplerSkippedBefore = st.sampler.totals.skipped;
    check(!gpuCensusBegin(d.ctx.Get(), GpuCensusSection::DoorSharpen),
          "rotation: a fifth occurrence this frame is declined (K=4 for a door section)");
    check(g_activeTimed == 4, "rotation: the cap does not advance past K");
    check(st.sampler.totals.skipped == samplerSkippedBefore,
          "rotation: a K-cap decline never touches the timer's own capacity accounting");
    check(st.skippedThisWindow == 0, "rotation: a K-cap decline is by design, never counted as a failed span");
    gpuCensusEnd(d.ctx.Get(), GpuCensusSection::DoorSharpen);   // still a harmless no-op

    // The stride: a per-draw section that ran 40 times a frame this window
    // (400 calls over 10 frames) is timed every 5th call (40 / K=8), from an
    // offset that rotates with its turns (7 turns taken -> offset 2).
    auto& holo = g_section[static_cast<size_t>(GpuCensusSection::FrameHologramPasses)];
    holo.occurrences = 400;
    holo.turns = 7;
    g_windowFrames = 9;                       // gpuCensusFrame counts this frame first: 10
    g_windowStartMs = GetTickCount64();       // no window closes during the case
    g_activeSection = static_cast<int>(GpuCensusSection::FrameHologramPasses) - 1;
    gpuCensusFrame(d.ctx.Get());
    check(g_activeSection == static_cast<int>(GpuCensusSection::FrameHologramPasses),
          "stride: the rotation lands on the next section");
    check(g_activeStride == 5, "stride: calls per frame / K");
    check(g_activeOffset == 2, "stride: the offset rotates with the section's turns");
    check(holo.turns == 8, "stride: the turn is counted");
    // Which calls the stride selects, read from g_activeTimed rather than
    // Begin's result: outside a native frame span every interval here takes
    // a whole disjoint-clock record (8 in all), so whether each selected
    // call also got a timer depends on the rig's own record use, not on
    // the selection under test.
    std::string timedAt;
    for (unsigned call = 0; call < 45; ++call) {
        const unsigned before = g_activeTimed;
        gpuCensusBegin(d.ctx.Get(), GpuCensusSection::FrameHologramPasses);
        if (g_activeTimed > before) timedAt += std::to_string(call) + ",";
        gpuCensusEnd(d.ctx.Get(), GpuCensusSection::FrameHologramPasses);
    }
    check(timedAt == "2,7,12,17,22,27,32,37,", "stride: timed calls are spread across the frame, capped at K=8");
    check(g_activeTimed == 8, "stride: the per-draw cap holds");

    // Only this fixture flushes (native_timing_gpu_test's own comment,
    // verbatim): production readback is nonblocking and DONOTFLUSH, and
    // gpu_census.cpp's own poll still never waits or flushes. Without a
    // Present anywhere in this process, nothing else would ever hand the
    // clear and its timestamps to WARP to execute, and gpu_interval.h's
    // four-poll gate is a COUNT, not a clock, so a tight loop would spin
    // it without ever giving WARP real wall-clock time either way.
    // Both the real sampler (four standalone records, i == 0..3 above) and
    // the null sampler (one more, from i == 0's calibration pair) are
    // outside a native frame span here, so each took a whole disjoint-clock
    // record: five of the eight, comfortably within budget without draining
    // anything first (gpu_disjoint_clock.h).
    d.ctx->Flush();
    const uint64_t deadline = GetTickCount64() + 1500;
    while ((st.sampler.totals.samples == 0 || st.nullSampler.totals.samples == 0) && GetTickCount64() < deadline) {
        gpuCensusFrame(d.ctx.Get());
        if (st.sampler.totals.samples == 0 || st.nullSampler.totals.samples == 0) Sleep(1);
    }
    check(st.sampler.totals.samples > 0,
          "WARP round trip: a real Begin/Clear/End pair produced a completed sample within the poll margin");
    check(st.sampler.totals.invalid == 0, "WARP round trip: no invalid/disjoint result on a clean WARP run");
    check(st.nullSampler.totals.samples == 1,
          "calibration: exactly one null pair completed -- the turn's first timed call only, not all four");
    check(st.nullSampler.totals.invalid == 0, "calibration: no invalid/disjoint result on the null pair either");

    // ---- the HDR HUD seed's scope (gpu_census.h), with a real timer: a sample lands in the seed's own section ----
    // Drain the door section's four first: outside a native frame span each pending interval holds one of the
    // disjoint clock's eight records, and the seed's own three (its null pair and two seeds) must not find them gone.
    {
        const uint64_t drainBy = GetTickCount64() + 1500;
        while ((st.sampler.totals.samples < 4 || st.nullSampler.totals.samples < 1) && GetTickCount64() < drainBy) {
            gpuCensusFrame(d.ctx.Get());
            Sleep(1);
        }
        check(st.sampler.totals.samples == 4, "WARP round trip: the door section's four samples all completed before the seed's are taken");
        SectionState& seed = g_section[static_cast<size_t>(GpuCensusSection::FrameUiLayerHdrSeed)];
        g_activeSection = static_cast<int>(GpuCensusSection::FrameUiLayerHdrSeed);
        g_activeCalls = g_activeTimed = 0;
        g_activeStride = 1;
        g_activeOffset = 0;
        g_activeNullDone = false;
        for (int eye = 0; eye < 2; ++eye) {   // one seed an eye
            GpuCensusSeedScope hdr(d.ctx.Get(), true, kSeedAt125);
            d.ctx->ClearRenderTargetView(rtv.Get(), colour);   // stands in for the copy and the Seeder's passes
        }
        {
            GpuCensusSeedScope ldr(d.ctx.Get(), false, kSeedAt100);   // the 8-bit layer's seed: no section, no timer
            d.ctx->ClearRenderTargetView(rtv.Get(), colour);
        }
        check(seed.occurrences == 2 && g_activeTimed == 2 && g_seedNotes == 2 && !g_seedMixed,
              "seed scope: two HDR HUD seeds on the section's turn are counted, noted and timed; the 8-bit layer's seed is none of these");
        d.ctx->Flush();
        const uint64_t seedDeadline = GetTickCount64() + 1500;
        while ((seed.sampler.totals.samples < 2 || seed.nullSampler.totals.samples < 1) && GetTickCount64() < seedDeadline) {
            gpuCensusFrame(d.ctx.Get());
            if (seed.sampler.totals.samples < 2 || seed.nullSampler.totals.samples < 1) Sleep(1);
        }
        check(seed.sampler.totals.samples == 2 && seed.sampler.totals.invalid == 0,
              "WARP round trip: the two seeds' real Begin/Clear/End pairs completed in the seed's own section");
        check(seed.nullSampler.totals.samples == 1 && seed.nullPairsTaken == 1,
              "WARP round trip: the seed's turn took one empty pair, for its own calibration");
        check(g_section[static_cast<size_t>(GpuCensusSection::FramePlanet)].sampler.totals.samples == 0 &&
                  g_section[static_cast<size_t>(GpuCensusSection::FrameUiLayerReissues)].sampler.totals.samples == 0,
              "WARP round trip: no other in-frame section took a sample from the seed's turn");
    }

    // ---- Elite's altered draws (gpu_census.h): the scope counts and times only a classed draw ----
    check(occurrenceCapFor(GpuCensusSection::AlteredPoolFamily) == 8 && !isDoorSection(GpuCensusSection::AlteredFixFirst) &&
              occurrenceCapFor(alteredFixSectionOf(AlteredFix::Unnamed)) == 8,
          "altered: the altered-draw sections, the fix ones too, are per-draw, K = 8, like the in-frame sections");
    for (auto& s : g_section) s = SectionState{};
    g_windowStartMs = GetTickCount64();
    g_activeSection = static_cast<int>(GpuCensusSection::AlteredPoolFamily);
    g_activeCalls = g_activeTimed = 0;
    g_activeStride = 1;
    g_activeOffset = 0;
    g_activeNullDone = false;
    const auto occurrencesEverywhere = [] {
        uint64_t total = 0;
        for (const auto& s : g_section) total += s.occurrences;
        return total;
    };
    { GpuCensusAlteredScope untouched(d.ctx.Get(), AlteredDrawClass::None); }
    check(occurrencesEverywhere() == 0 && g_activeTimed == 0,
          "altered: a draw EDVR left as the game issued it counts nothing and times nothing");
    auto& pool = g_section[static_cast<size_t>(GpuCensusSection::AlteredPoolFamily)];
    for (int i = 0; i < 3; ++i) {
        GpuCensusAlteredScope timed(d.ctx.Get(), AlteredDrawClass::PoolFamily);
        d.ctx->ClearRenderTargetView(rtv.Get(), colour);   // stands in for the game's draw
    }
    check(pool.occurrences == 3 && g_activeTimed == 3, "altered: three pool-family draws on their section's turn are counted and selected for timing");
    { GpuCensusAlteredScope other(d.ctx.Get(), AlteredDrawClass::UiLayer); }
    { GpuCensusAlteredScope other(d.ctx.Get(), AlteredDraw(AlteredDrawClass::Verdict, AlteredFix::GlareSteady)); }
    check(g_section[static_cast<size_t>(GpuCensusSection::AlteredUiLayer)].occurrences == 1 &&
              g_section[static_cast<size_t>(alteredFixSectionOf(AlteredFix::GlareSteady))].occurrences == 1 &&
              pool.occurrences == 3 && occurrencesEverywhere() == 5 && g_activeTimed == 3,
          "altered: each class, and each fix, counts in its own section only, and a section that is not on its turn is counted, not timed");
    // The rotation reaches every TURN once a cycle, the altered-draw ones included; the fix sections after the
    // first share its turn and are never landed on.
    g_windowFrames = 0;
    g_activeSection = 0;
    bool visited[kSections] = {};
    for (size_t i = 0; i < kSections; ++i) {
        gpuCensusFrame(d.ctx.Get());
        visited[static_cast<size_t>(g_activeSection)] = true;
    }
    bool ownersVisited = true, membersSkipped = true, idleWorldSkipped = true;
    for (size_t i = 0; i < kSections; ++i) {
        // The VR world route's sections are turns only once called this window (nextTurnOwner): none was here.
        if (i >= kWorldFirst && i < kWorldFirst + kWorldSections) { idleWorldSkipped = idleWorldSkipped && !visited[i]; continue; }
        if (turnOwnerOf(static_cast<GpuCensusSection>(i)) == static_cast<GpuCensusSection>(i)) ownersVisited = ownersVisited && visited[i];
        else membersSkipped = membersSkipped && !visited[i];
    }
    check(idleWorldSkipped, "world route: with the key off (its sections never called) the rotation gives them no turn, so the census samples as it did before they existed");
    check(ownersVisited && visited[static_cast<size_t>(GpuCensusSection::AlteredUiLayer)] &&
              visited[static_cast<size_t>(GpuCensusSection::AlteredFixFirst)],
          "altered: the frame rotation visits every turn, the altered-draw ones and the fix sections' shared one too");
    check(membersSkipped, "altered: the fix sections after the first are not turns of their own: the rotation never lands on them");

    gpuCensusShutdown();
    check(gpuTimingShutdown(d.ctx.Get()), "explicit owner shutdown");
}

// ---- 4: the line's format, including "-" for an absent section --------
void logFormatCase() {
    for (auto& s : g_section) s = SectionState{};
    g_activeSection = 0;
    g_activeCalls = g_activeTimed = 0;
    g_windowFrames = 200;
    const uint64_t start = GetTickCount64() - 30000;
    g_windowStartMs = start;
    g_p50Cursor = 0;
    g_p50Count = 0;   // no Application-render samples landed this window

    auto& sharpen = g_section[static_cast<size_t>(GpuCensusSection::DoorSharpen)];
    sharpen.occurrences = 20;
    sharpen.sampler.totals.ms = 4.0;
    sharpen.sampler.totals.samples = 2;   // 2 ms/occurrence * (20/200 = 0.1/frame) = 0.2 ms/frame

    g_lastLog.clear();
    logAndResetWindow(start + 30000);

    check(g_lastLog.rfind("EDVR GPU census:", 0) == 0, "log line: starts with the fixed preamble");
    check(g_lastLog.find("30 s, 200 frames") != std::string::npos, "log line: the window length and frame count");
    check(g_lastLog.find("sharpen 0.200 (0.10/frame)") != std::string::npos,
          "log line: a timed section reports ms/frame with its occurrences/frame");
    check(g_lastLog.find("menu -") != std::string::npos, "log line: an absent door section prints '-'");
    check(g_lastLog.find("planet -") != std::string::npos, "log line: an absent in-frame section prints '-'");
    check(g_lastLog.find("application render p50 -;") != std::string::npos,
          "log line: no Application-render samples this window also prints '-'");
    check(g_lastLog.find("timer floor -;") != std::string::npos,
          "log line: no null samples this window also prints '-' for the timer floor");
    check(g_lastLog.find("failed 0.") != std::string::npos, "log line: failed spans are reported by name");
    check(g_lastLog.find("door 0.200") != std::string::npos,
          "log line: door total folds in the sharpen leaf (one of the five top-level door passes)");

    check(sharpen.occurrences == 0, "log line: the window resets occurrences after logging");
    check(g_windowFrames == 0, "log line: the window resets its frame count after logging");
    check(g_p50Count == 0, "log line: the window resets the p50 sample buffer after logging");

    // With Application-render samples: their median, and the game's share
    // as that median less EDVR's total (0.2 ms from the sharpen leaf above).
    for (auto& s : g_section) s = SectionState{};
    g_windowFrames = 200;
    g_windowStartMs = start;
    sharpen.occurrences = 20;
    sharpen.sampler.totals.ms = 4.0;
    sharpen.sampler.totals.samples = 2;
    g_p50Samples[0] = 12.0; g_p50Samples[1] = 10.0; g_p50Samples[2] = 11.0;
    g_p50Count = 3;
    g_lastLog.clear();
    logAndResetWindow(start + 30000);
    check(g_lastLog.find("application render p50 11.000 ms/frame (game ~10.800)") != std::string::npos,
          "log line: application render reports its median and the game's share (median less EDVR)");
    check(g_lastLog.find("(census over the frame total)") == std::string::npos,
          "log line: the over-total note is absent when EDVR's corrected total does not exceed R");

    // The timer floor and the over-total note: a section with both real and
    // null samples (corrected ms/frame = (4.0/2 - 0.4/2) * (20/200) = 0.18),
    // and R below that corrected total so the game's share goes negative.
    for (auto& s : g_section) s = SectionState{};
    g_windowFrames = 200;
    g_windowStartMs = start;
    sharpen.occurrences = 20;
    sharpen.sampler.totals.ms = 4.0;
    sharpen.sampler.totals.samples = 2;
    sharpen.nullSampler.totals.ms = 0.4;
    sharpen.nullSampler.totals.samples = 2;   // 0.2 ms/pair -> 200 us/pair pooled floor
    g_p50Samples[0] = 0.1;
    g_p50Count = 1;
    g_lastLog.clear();
    logAndResetWindow(start + 30000);
    check(g_lastLog.find("sharpen 0.180 (0.10/frame)") != std::string::npos,
          "log line: a section's ms/frame is corrected by its own null mean before reaching the line");
    check(g_lastLog.find("timer floor 200.0 us/pair") != std::string::npos,
          "log line: the timer floor is the pooled null mean across sections, in us/pair");
    check(g_lastLog.find("(game ~-0.080)") != std::string::npos,
          "log line: the game's share stays raw and negative when EDVR's corrected total exceeds R");
    check(g_lastLog.find("(census over the frame total)") != std::string::npos,
          "log line: EDVR's corrected total over R is flagged, not hidden");
}

void freshWindow(uint64_t start);
const std::string* lineWith(const char* prefix);

const std::string* pluginCostLine(unsigned scope, bool invalid = false) {
    const char* prefix = invalid ? "EDVR plugin cost invalid v1:" : "EDVR plugin cost v1:";
    const std::string wanted = " scope=" + std::to_string(scope) + " ";
    for (const auto& line : g_lines)
        if (line.rfind(prefix, 0) == 0 && line.find(wanted) != std::string::npos) return &line;
    return nullptr;
}

void pluginCostEvidenceCases() {
    const uint64_t start = 1000, end = 31000;
    const auto engine = static_cast<unsigned>(GpuCensusSection::FrameEngineVelocity);
    {
        freshWindow(start);
        g_windowFrames = 0; // a valid close with no denominator
        auto& st = g_section[engine];
        st.occurrences = 5;
        st.baseMs = 1.0; st.baseSamples = 3;
        st.sampler.totals.ms = 6.123456789123; st.sampler.totals.samples = 5;
        st.nullBaseMs = 0.1; st.nullBaseSamples = 1;
        st.nullSampler.totals.ms = 0.5; st.nullSampler.totals.samples = 3;
        const unsigned nullPairsBefore = st.nullPairsTaken;
        logAndResetWindow(end);
        const std::string* line = pluginCostLine(engine);
        check(line && *line == "EDVR plugin cost v1: window_start_ms=1000 window_end_ms=31000 scope=14 owner=0 attribution=0 frames=0 occurrences=5 completed_samples=2 raw_sample_ms=5.1234567891229998 null_samples=2 null_ms=0.40000000000000002 status=measured",
              "plugin evidence: raw per-window deltas retain precision, schema order, enum IDs, and zero-frame denominator");
        check(st.occurrences == 0 && st.baseMs == st.sampler.totals.ms && st.baseSamples == st.sampler.totals.samples &&
                  st.nullBaseMs == st.nullSampler.totals.ms && st.nullBaseSamples == st.nullSampler.totals.samples &&
                  st.sampler.totals.ms == 6.123456789123 && st.sampler.totals.samples == 5 && st.nullPairsTaken == nullPairsBefore,
              "plugin evidence: line is emitted before reset and formatting changes no timer totals or calibration-pair count");
    }
    {
        freshWindow(start);
        const size_t owner = static_cast<size_t>(GpuCensusSection::AlteredFixFirst);
        const unsigned scope = static_cast<unsigned>(GpuCensusSection::AlteredFixFirst) + 3;
        auto& st = g_section[scope];
        auto& nullSt = g_section[owner];
        g_windowFrames = 12;
        st.occurrences = 4;
        st.sampler.totals.ms = 1.0; st.sampler.totals.samples = 1;
        nullSt.nullBaseMs = 0.2; nullSt.nullBaseSamples = 2;
        nullSt.nullSampler.totals.ms = 0.7; nullSt.nullSampler.totals.samples = 4;
        st.nullSampler.totals.ms = 99.0; st.nullSampler.totals.samples = 99; // ignored: fixes share the first fix's pair
        logAndResetWindow(end);
        const std::string* line = pluginCostLine(scope);
        check(line && line->find(" completed_samples=1 raw_sample_ms=1 null_samples=2 null_ms=0.49999999999999994 status=measured") != std::string::npos,
              "plugin evidence: every altered fix uses the shared first-fix null baseline and sample cohort");
    }
    {
        freshWindow(start);
        auto& st = g_section[engine];
        st.occurrences = 2; // helper ran, no timestamp completed
        logAndResetWindow(end);
        const std::string* line = pluginCostLine(engine);
        check(line && line->find("completed_samples=0 raw_sample_ms=0 null_samples=0 null_ms=0 status=unmeasured") != std::string::npos,
              "plugin evidence: observed helper calls without completed timestamps remain explicitly unmeasured");
    }
    {
        freshWindow(start);
        auto& st = g_section[engine];
        st.occurrences = 1;
        st.sampler.totals.ms = 1.0; st.sampler.totals.samples = 2;
        logAndResetWindow(end);
        const std::string* line = pluginCostLine(engine);
        check(line && line->find("completed_samples=2 raw_sample_ms=1 null_samples=0 null_ms=0 status=uncalibrated") != std::string::npos,
              "plugin evidence: completed samples without a null cohort are explicitly uncalibrated");
    }
    {
        freshWindow(start);
        auto& st = g_section[engine];
        st.occurrences = 1;
        st.sampler.totals.ms = 0.4; st.sampler.totals.samples = 2;
        st.nullSampler.totals.ms = 0.5; st.nullSampler.totals.samples = 2;
        logAndResetWindow(end);
        const std::string* line = pluginCostLine(engine);
        check(line && line->find("completed_samples=2 raw_sample_ms=0.40000000000000002 null_samples=2 null_ms=0.5 status=null-floor") != std::string::npos,
              "plugin evidence: a raw per-sample mean at or below the null mean is identified as null-floor");
    }
    for (unsigned aboveFloor = 0; aboveFloor < 2; ++aboveFloor) {
        freshWindow(start);
        auto& st = g_section[engine];
        st.occurrences = 1;
        st.sampler.totals.ms = aboveFloor ? 1.0000000000001 : 1.0;
        st.sampler.totals.samples = 1;
        st.nullSampler.totals.ms = 1.0;
        st.nullSampler.totals.samples = 1;
        logAndResetWindow(end);
        const std::string* line = pluginCostLine(engine);
        const char* expected = aboveFloor ?
            "completed_samples=1 raw_sample_ms=1.0000000000000999 null_samples=1 null_ms=1 status=measured" :
            "completed_samples=1 raw_sample_ms=1 null_samples=1 null_ms=1 status=null-floor";
        check(line && line->find(expected) != std::string::npos,
              "plugin evidence: roundtrip sums preserve exact tie and just-above-floor status without an epsilon");
    }
    {
        freshWindow(start);
        auto& st = g_section[engine];
        st.occurrences = 0; // completions can arrive after the call window rolled over
        st.sampler.totals.ms = 1.0; st.sampler.totals.samples = 2;
        st.nullSampler.totals.ms = 0.2; st.nullSampler.totals.samples = 2;
        logAndResetWindow(end);
        const std::string* line = pluginCostLine(engine);
        check(line && line->find("frames=200 occurrences=0 completed_samples=2") != std::string::npos &&
                  line->find("status=measured") != std::string::npos,
              "plugin evidence: late completion is preserved with zero current-window helper occurrences, not attributed as a call");
    }
    {
        freshWindow(start);
        auto& st = g_section[engine];
        st.occurrences = 1;
        st.baseMs = 2.0;
        st.sampler.totals.ms = 1.0;
        logAndResetWindow(end);
        const std::string* invalid = pluginCostLine(engine, true);
        check(invalid && invalid->find("reason=sample-ms-regressed") != std::string::npos &&
                  pluginCostLine(engine) == nullptr,
              "plugin evidence: invalid cumulative deltas emit a rejectable diagnostic, never a fabricated zero row");
    }
    for (unsigned invalidCase = 0; invalidCase < 6; ++invalidCase) {
        freshWindow(start);
        auto& st = g_section[engine];
        st.occurrences = 1;
        const char* reason = nullptr;
        switch (invalidCase) {
        case 0: st.baseSamples = 1; reason = "reason=sample-counter-regressed"; break;
        case 1: st.nullBaseSamples = 1; reason = "reason=null-counter-regressed"; break;
        case 2: st.sampler.totals.ms = std::numeric_limits<double>::infinity(); reason = "reason=sample-ms-nonfinite"; break;
        case 3: st.nullBaseMs = std::numeric_limits<double>::quiet_NaN(); reason = "reason=null-ms-nonfinite"; break;
        case 4: st.baseMs = std::numeric_limits<double>::quiet_NaN(); reason = "reason=sample-ms-nonfinite"; break;
        case 5: st.nullBaseMs = 2.0; st.nullSampler.totals.ms = 1.0; reason = "reason=null-ms-regressed"; break;
        }
        logAndResetWindow(end);
        const std::string* invalid = pluginCostLine(engine, true);
        check(invalid && reason && invalid->find(reason) != std::string::npos && pluginCostLine(engine) == nullptr,
              "plugin evidence: counter underflow, nonfinite totals/baselines and null regression fail closed");
    }
    for (unsigned nullCohort = 0; nullCohort < 2; ++nullCohort) {
        freshWindow(start);
        auto& st = g_section[engine];
        st.occurrences = 1;
        if (nullCohort) st.nullSampler.totals.ms = 1.0;
        else st.sampler.totals.ms = 1.0;
        logAndResetWindow(end);
        const std::string* invalid = pluginCostLine(engine, true);
        const char* reason = nullCohort ? "reason=null-ms-without-samples" : "reason=sample-ms-without-samples";
        check(invalid && invalid->find(reason) != std::string::npos && pluginCostLine(engine) == nullptr,
              "plugin evidence: a positive sample sum with no completed samples is diagnostic-only");
    }
}

void logicalOwnerReportCases() {
    using O = GpuCensusOwner;
    const uint64_t start = GetTickCount64() - 30000;
    freshWindow(start);
    g_windowFrames = 100;

    auto& whole = g_section[static_cast<size_t>(GpuCensusSection::DoorTemporalWhole)];
    whole.occurrences = 2;
    whole.sampler.totals.ms = 4.0;
    whole.sampler.totals.samples = 2;
    auto& nested = g_section[static_cast<size_t>(GpuCensusSection::DoorUpscaler)];
    nested.occurrences = 2;
    nested.sampler.totals.ms = 200.0;
    nested.sampler.totals.samples = 2;
    auto& menu = g_section[static_cast<size_t>(GpuCensusSection::DoorMenu)];
    menu.occurrences = 1;
    menu.sampler.totals.ms = 2.0;
    menu.sampler.totals.samples = 2;
    auto& wrappedRemlok = g_section[static_cast<size_t>(alteredFixSectionOf(AlteredFix::Remlok))];
    wrappedRemlok.occurrences = 4;
    wrappedRemlok.sampler.totals.ms = 800.0;
    wrappedRemlok.sampler.totals.samples = 4;

    logAndResetWindow(start + 30000);
    const std::string* temporal = lineWith(
        "EDVR GPU census, logical owner temporal-aa (legacy attribution, not module-selection status):");
    const std::string* core = lineWith(
        "EDVR GPU census, logical owner core (legacy attribution, not module-selection status):");
    check(temporal && temporal->find("0.040 ms/frame; observed 1/14 declared direct scopes") != std::string::npos &&
              temporal->find("timestamped 1/1 observed scopes (2 existing timer samples)") != std::string::npos &&
              temporal->find("scope coverage observed scopes timestamped") != std::string::npos &&
              temporal->find("not complete plugin GPU cost") != std::string::npos,
          "logical TemporalAa row sums its direct whole scope once, excludes nested children, and distinguishes observed-scope timing from plugin completeness");
    check(core && core->find("observed 1/1 declared direct scopes") != std::string::npos &&
              core->find("timestamped 1/1 observed scopes (2 existing timer samples)") != std::string::npos &&
              core->find("scope coverage observed scopes timestamped") != std::string::npos,
          "the menu's direct GPU scope is reported in its separate Core owner row");
    check(lineWith("EDVR GPU census, logical owner scanners") == nullptr &&
              lineWith("EDVR GPU census, logical owner cockpit-visuals") == nullptr &&
              lineWith("EDVR GPU census, logical owner on-foot-panel") == nullptr &&
              lineWith("EDVR GPU census ownership note: FrameWeaponMotion") == nullptr,
          "owners with only absent direct scopes or wrapped game draws produce no inactive rows");
    check(temporal && temporal->find("CPU") == std::string::npos &&
              temporal->find("API") == std::string::npos && core &&
              core->find("CPU") == std::string::npos && core->find("API") == std::string::npos,
          "owner reports state GPU evidence only and infer no per-plugin CPU or API count");

    freshWindow(start);
    g_windowFrames = 100;
    auto& weapon = g_section[static_cast<size_t>(GpuCensusSection::FrameWeaponMotion)];
    weapon.occurrences = 3;
    weapon.sampler.totals.ms = 6.0;
    weapon.sampler.totals.samples = 2;
    logAndResetWindow(start + 30000);
    const std::string* shared = lineWith("EDVR GPU census ownership note: FrameWeaponMotion");
    const std::string* temporalWeapon = lineWith(
        "EDVR GPU census, logical owner temporal-aa (legacy attribution, not module-selection status):");
    check(shared && shared->find("reported once under temporal-aa; on-foot-panel consumes the service") != std::string::npos &&
              shared->find("not split into a second cost row") != std::string::npos,
          "shared weapon-motion scope identifies the second consumer without duplicating its owner cost");
    check(temporalWeapon && temporalWeapon->find("observed 1/14 declared direct scopes") != std::string::npos &&
              temporalWeapon->find("timestamped 1/1 observed scopes (2 existing timer samples)") != std::string::npos,
          "weapon-motion GPU work is included once in TemporalAa only when that scope occurred");

    freshWindow(start);
    g_windowFrames = 100;
    auto& scanner = g_section[static_cast<size_t>(GpuCensusSection::DoorFssHeal)];
    scanner.occurrences = 2;
    logAndResetWindow(start + 30000);
    const std::string* unmeasuredScanner = lineWith(
        "EDVR GPU census, logical owner scanners (legacy attribution, not module-selection status):");
    check(unmeasuredScanner &&
              unmeasuredScanner->find("GPU unmeasured; observed 1/1 declared direct scopes, timestamped 0/1 observed scopes; scope coverage partial, not complete plugin GPU cost.") != std::string::npos &&
              unmeasuredScanner->find("0.000 ms/frame") == std::string::npos,
          "an occurring direct owner with no timer samples gets an unmeasured partial row, never a numeric zero");
}

// ---- 5: the frame gap: fake spans with known GPU ticks, through the census's own intake ------------------
// A span is 10 ms wide at 10 MHz (100000 ticks); the gap after it is what each case varies.
constexpr uint64_t kFreq = 10000000;
constexpr uint64_t kSpanTicks = 100000;
GpuSpanResult fakeSpan(uint64_t sequence, uint64_t first, uint64_t last, uint64_t freq = kFreq) {
    GpuSpanResult r;
    r.sequence = sequence;
    r.reason = GpuSpanReason::Valid;
    r.source = GpuSpanSource::ApplicationRender;
    r.outerMs = freq ? static_cast<double>(last - first) * 1000.0 / static_cast<double>(freq) : 0.0;
    r.firstTick = first;
    r.lastTick = last;
    r.frequency = freq;
    return r;
}
// count frames, sequences firstSeq.., gapTicks(i) after frame i.
template <class Gap>
void feedFrames(unsigned count, uint64_t firstSeq, Gap gapTicks) {
    uint64_t t = 5000000;
    for (unsigned i = 0; i < count; ++i) {
        noteApplicationCompletion(fakeSpan(firstSeq + i, t, t + kSpanTicks));
        t += kSpanTicks + gapTicks(i);
    }
}
void freshWindow(uint64_t start) {
    for (auto& s : g_section) s = SectionState{};
    g_windowFrames = 200;
    g_windowStartMs = start;
    g_p50Count = 0;
    g_gap = GpuFrameGap{};
    g_lines.clear();
    g_lastLog.clear();
    resetSeedNotes();
}
const std::string* lineWith(const char* prefix) {
    for (const auto& l : g_lines)
        if (l.rfind(prefix, 0) == 0) return &l;
    return nullptr;
}
void gapCases() {
    const uint64_t start = GetTickCount64() - 30000;

    // Saturated: the next frame's first command follows the last one within 0.05 ms, twice by 2 ms.
    freshWindow(start);
    feedFrames(100, 1, [](unsigned i) -> uint64_t { return (i == 10 || i == 50) ? 20000 : 500; });
    logAndResetWindow(start + 30000);
    check(g_lastLog.find("frame gap p50 0.05 / p95 0.05 ms over 99 pairs;") != std::string::npos,
          "gap: a saturated GPU (0.05 ms between frames) reads p50 0.05 / p95 0.05 over 99 pairs, on the census line");
    const std::string* detail = lineWith("EDVR GPU census, frame gap:");
    check(detail && detail->find("p50 0.05 ms, p95 0.05 ms, max 2.00 ms over 99 pairs of 100 valid frames") != std::string::npos,
          "gap: the detail line adds the max (the two 2 ms gaps) and the pair and frame counts");
    check(g_lastLog.find("application render p50 10.000 ms/frame") != std::string::npos,
          "gap: the same completions still feed the application-render median");
    check(g_gap.finishWindow().frames == 0, "gap: the window starts over after the line");

    // Starved: the GPU waits 5 ms for the CPU between frames.
    freshWindow(start);
    feedFrames(100, 1000, [](unsigned) -> uint64_t { return 50000; });
    logAndResetWindow(start + 30000);
    check(g_lastLog.find("frame gap p50 5.00 / p95 5.00 ms over 99 pairs;") != std::string::npos,
          "gap: a starved GPU (5 ms between frames) reads 5.00");

    // Compositor-sized: about 1 ms. The number is printed as measured, and the line says that it is not proof of idleness.
    freshWindow(start);
    feedFrames(60, 1, [](unsigned) -> uint64_t { return 11000; });
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(g_lastLog.find("frame gap p50 1.10 / p95 1.10 ms over 59 pairs;") != std::string::npos &&
              detail && detail->find("a gap of about 1 ms is not proof of idleness") != std::string::npos &&
              detail->find("SteamVR's compositor (another process on the same GPU)") != std::string::npos,
          "gap: a compositor-sized gap (1.10 ms) is reported, and the line says the compositor shares the GPU");
    check(detail && detail->find("upper bound on GPU idle, not idle") != std::string::npos,
          "gap: the wording calls the figure an upper bound on idle, not idle");

    // A missing pair: frame 3 never completes, so 2->3 and 3->4 make no pair. Two pairs, four frames.
    freshWindow(start);
    {
        uint64_t t = 5000000;
        for (uint64_t seq : {1ull, 2ull, 4ull, 5ull}) {
            noteApplicationCompletion(fakeSpan(seq, t, t + kSpanTicks));
            t += kSpanTicks + 1000;
        }
    }
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(g_lastLog.find("over 2 pairs;") != std::string::npos &&
              detail && detail->find("over 2 pairs of 4 valid frames") != std::string::npos,
          "gap: a frame that never completes leaves its two pairs unmade, said as fewer pairs than frames");

    // The same frames, the missing one arriving LAST (results complete out of order): each pair is made once.
    freshWindow(start);
    {
        uint64_t t[6];
        t[0] = 5000000;
        for (unsigned i = 1; i < 6; ++i) t[i] = t[i - 1] + kSpanTicks + 1000;
        for (unsigned i : {0u, 1u, 3u, 4u, 2u}) noteApplicationCompletion(fakeSpan(1 + i, t[i], t[i] + kSpanTicks));
    }
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(detail && detail->find("over 4 pairs of 5 valid frames") != std::string::npos,
          "gap: the late frame completes both its pairs, once each, whatever order the results settle in");
    // A completion repeated is not a second frame.
    freshWindow(start);
    noteApplicationCompletion(fakeSpan(7, 5000000, 5100000));
    noteApplicationCompletion(fakeSpan(7, 5000000, 5100000));
    noteApplicationCompletion(fakeSpan(8, 5101000, 5201000));
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(detail && detail->find("over 1 pairs of 2 valid frames") != std::string::npos,
          "gap: a duplicated completion is one frame, not two");

    // Nothing paired: absent on the census line, never 0.00.
    freshWindow(start);
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(g_lastLog.find("frame gap - (no pairs);") != std::string::npos && g_lastLog.find("frame gap p50") == std::string::npos &&
              detail && detail->find("no pairs this window (0 valid frames") != std::string::npos,
          "gap: a window with no spans reads '-', not 0.00, on both lines");
    // One frame alone: valid, but no neighbour.
    freshWindow(start);
    noteApplicationCompletion(fakeSpan(9, 5000000, 5100000));
    logAndResetWindow(start + 30000);
    check(g_lastLog.find("frame gap - (no pairs);") != std::string::npos, "gap: one frame alone makes no pair");

    // Clock rules: another frequency, overlapping spans, a stall, an invalid span, the other source.
    freshWindow(start);
    noteApplicationCompletion(fakeSpan(1, 5000000, 5100000, kFreq));
    noteApplicationCompletion(fakeSpan(2, 5101000, 5201000, kFreq * 2));   // the clock changed
    noteApplicationCompletion(fakeSpan(3, 5100000, 5200000, kFreq * 2));   // starts before frame 2 ended: overlap
    noteApplicationCompletion(fakeSpan(4, 5200000 + 30000000, 5300000 + 30000000, kFreq * 2));   // 1.5 s gap: a stall
    GpuSpanResult bad = fakeSpan(5, 0, 0);
    bad.reason = GpuSpanReason::Disjoint;
    noteApplicationCompletion(bad);   // not valid: not fed at all
    GpuSpanResult legacy = fakeSpan(6, 1, 2);
    legacy.source = GpuSpanSource::RenderToSubmit;
    noteApplicationCompletion(legacy);   // the other span: not fed
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(g_lastLog.find("frame gap - (no pairs), 1 over 1 s;") != std::string::npos && detail &&
              detail->find("no pairs this window (4 valid frames") != std::string::npos &&
              detail->find("2 pairs rejected as not comparable (overlapping, or another clock)") != std::string::npos &&
              detail->find("over 1 s") != std::string::npos,
          "gap: another clock and an overlap are rejected (two pairs), an invalid span and the legacy span never fed");
    // A GAP OVER ONE SECOND IS KEPT AND FLAGGED, not discarded (issue 63: the 1858 ms freeze was such a pair).
    check(detail && detail->find("1 pair over 1 s KEPT as stalls (not in the percentiles or the max; longest 1500.0 ms; each is the gap "
                                 "before that runtime sequence): sequence 4 1500.0 ms") != std::string::npos,
          "gap: a 1.5 s gap is not rejected: it is named, with its length and the runtime sequence that ended it");
    // Beside real pairs: the stall stays out of p50, p95 and the max, and the brief line says it in a few words.
    freshWindow(start);
    {
        uint64_t t = 5000000;
        for (unsigned i = 0; i < 20; ++i) {
            noteApplicationCompletion(fakeSpan(100 + i, t, t + kSpanTicks));
            // The gap after frame 109 is 2.5 s; every other gap is 0.5 ms.
            t += kSpanTicks + (i == 9 ? 25000000 : 5000);
        }
    }
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(g_lastLog.find("frame gap p50 0.50 / p95 0.50 ms over 18 pairs, 1 over 1 s;") != std::string::npos,
          "gap: with a stall in the window, 18 of 19 pairs are statistics, p50 and p95 are untouched, the brief line adds ', 1 over 1 s'");
    check(detail && detail->find("max 0.50 ms over 18 pairs of 20 valid frames") != std::string::npos &&
              detail->find("longest 2500.0 ms") != std::string::npos && detail->find("sequence 110 2500.0 ms") != std::string::npos,
          "gap: the max stays the largest gap that was a statistic (0.50), the stall (2500 ms, before sequence 110) is beside it, not in it");
    check(detail && detail->find("rejected as not comparable") == std::string::npos,
          "gap: a stall is no longer counted as a rejected pair");
    // Exactly one second is still a gap, not a stall (the boundary is strict); a hair over is a stall.
    freshWindow(start);
    noteApplicationCompletion(fakeSpan(1, 5000000, 5100000));
    noteApplicationCompletion(fakeSpan(2, 5100000 + 10000000, 5200000 + 10000000));   // gap = exactly 1.000 s
    noteApplicationCompletion(fakeSpan(3, 5200000 + 20000100, 5300000 + 20000100));   // gap = 1.00001 s
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(g_lastLog.find("frame gap p50 1000.00 / p95 1000.00 ms over 1 pairs, 1 over 1 s;") != std::string::npos,
          "gap: a gap of exactly 1000 ms is a pair; one of 1000.01 ms is a stall");
    // Many stalls: the largest three are named, the rest counted, and the longest is the longest of all.
    freshWindow(start);
    {
        uint64_t t = 5000000;
        const uint64_t gaps[6] = {12000000, 30000000, 15000000, 50000000, 11000000, 20000000};   // 1.2, 3.0, 1.5, 5.0, 1.1, 2.0 s
        for (unsigned i = 0; i < 7; ++i) {
            noteApplicationCompletion(fakeSpan(200 + i, t, t + kSpanTicks));
            t += kSpanTicks + (i < 6 ? gaps[i] : 0);
        }
    }
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(g_lastLog.find("frame gap - (no pairs), 6 over 1 s;") != std::string::npos &&
              detail && detail->find("6 pairs over 1 s KEPT as stalls") != std::string::npos &&
              detail->find("longest 5000.0 ms") != std::string::npos &&
              detail->find("sequence 204 5000.0 ms, sequence 202 3000.0 ms, sequence 206 2000.0 ms (and 3 more)") != std::string::npos,
          "gap: six stalls: the three largest are named longest first, the other three are counted");
    // A span with no ticks (a result from before the fields existed, or a poisoned one) is unusable, not a zero gap.
    freshWindow(start);
    noteApplicationCompletion(fakeSpan(1, 0, 0, 0));
    logAndResetWindow(start + 30000);
    detail = lineWith("EDVR GPU census, frame gap:");
    check(detail && detail->find("1 spans unusable") != std::string::npos && g_lastLog.find("frame gap - (no pairs);") != std::string::npos,
          "gap: a valid span carrying no ticks or frequency is counted unusable, never paired");
}

// ---- 6: Elite's altered draws: which class a draw is, and the line that reports them ---------------------------
void alteredClassCases() {
    using C = AlteredDrawClass;
    //                      owner, verdictNone, poolSubstituted, uiLayered
    check(classifyAlteredDraw(true, true, false, false) == C::None, "class: a plain draw EDVR did not touch is not altered");
    check(classifyAlteredDraw(true, true, true, false) == C::PoolFamily,
          "class: a verdict-free draw with EDVR's slot target and shaders bound is a pool-family draw");
    check(classifyAlteredDraw(true, false, true, false) == C::Verdict,
          "class: the pool flag is stale under a verdict (engineVelocityBeforeDraw did not run): the verdict's wrapper, not a pool-family draw");
    check(classifyAlteredDraw(true, true, false, true) == C::UiLayer, "class: a draw redirected into the UI layer");
    check(classifyAlteredDraw(true, false, false, true) == C::UiLayer,
          "class: a verdict draw the UI layer redirected is the redirect (its target moved)");
    check(classifyAlteredDraw(true, false, false, false) == C::Verdict, "class: any other verdict's wrapper");
    check(classifyAlteredDraw(true, true, true, true) == C::PoolFamily &&
              classifyAlteredDraw(true, false, true, true) == C::UiLayer,
          "class: one class per draw, in the order pool family, UI layer, verdict");
    check(classifyAlteredDraw(false, true, true, true) == C::None && classifyAlteredDraw(false, false, false, false) == C::None,
          "class: a foreign (non-owner) context is never counted");
    check(alteredSectionOf(C::PoolFamily) == GpuCensusSection::AlteredPoolFamily &&
              alteredSectionOf(C::UiLayer) == GpuCensusSection::AlteredUiLayer &&
              alteredSectionOf(AlteredDraw(C::Verdict, AlteredFix::Remlok)) == alteredFixSectionOf(AlteredFix::Remlok),
          "class: each class maps to its own section, and a Verdict draw to its fix's");
}

void alteredLineCases() {
    const uint64_t start = GetTickCount64() - 30000;
    // Pool-family draws: 2000 in 200 frames (10 a frame), four timed at 0.2 ms each: 2.000 ms/frame.
    // UI-layer draws: 40 (0.20 a frame), two timed at 0.1 ms: 0.020 ms/frame. Verdict: never ran.
    freshWindow(start);
    auto& pool = g_section[static_cast<size_t>(GpuCensusSection::AlteredPoolFamily)];
    pool.occurrences = 2000;
    pool.sampler.totals.ms = 0.8;
    pool.sampler.totals.samples = 4;
    auto& ui = g_section[static_cast<size_t>(GpuCensusSection::AlteredUiLayer)];
    ui.occurrences = 40;
    ui.sampler.totals.ms = 0.2;
    ui.sampler.totals.samples = 2;
    logAndResetWindow(start + 30000);
    const std::string* line = lineWith("EDVR GPU census, Elite's own draws that EDVR alters");
    check(line != nullptr, "altered line: a line of its own follows the main census line");
    if (line) {
        check(line->find("pool-family draws (EDVR's slot target and shaders) 2.000 (10.00/frame)") != std::string::npos,
              "altered line: pool-family draws: ms/frame with their count a frame");
        check(line->find("UI draws (redirected to EDVR's layer) 0.020 (0.20/frame)") != std::string::npos,
              "altered line: UI-layer draws report too");
        check(line->find("other fix-wrapped draws -") != std::string::npos,
              "altered line: a class that never ran prints '-', never 0.000");
        check(line->find("together 2.020 ms/frame") != std::string::npos, "altered line: the classes are disjoint draws, so together is their sum");
        check(line->find("includes the game's own work in it, not only what EDVR adds") != std::string::npos &&
                  line->find("the game's draw timed whole") != std::string::npos,
              "altered line: it says the figures include the game's own work in those draws");
    }
    check(g_lastLog.find("EDVR ~0.000 ms/frame") != std::string::npos && g_lastLog.find("pool-family") == std::string::npos,
          "altered line: the altered draws are not in EDVR's total and not on the main line");
    check(g_section[static_cast<size_t>(GpuCensusSection::AlteredPoolFamily)].occurrences == 0,
          "altered line: the window resets the altered sections' occurrences");

    // Nothing ran: all three '-', together 0.000 (a sum, not a measurement), and the wording still says what '-' means.
    freshWindow(start);
    logAndResetWindow(start + 30000);
    line = lineWith("EDVR GPU census, Elite's own draws that EDVR alters");
    check(line && line->find("pool-family draws (EDVR's slot target and shaders) -, "
                             "UI draws (redirected to EDVR's layer) -, other fix-wrapped draws -;") != std::string::npos &&
              line->find("\"-\" means no such draw ran this window") != std::string::npos,
          "altered line: a window with no altered draw prints '-' for all three and says what that means");
    // The timer floor and the spans count include the altered sections' own timers.
    freshWindow(start);
    auto& uiClass = g_section[static_cast<size_t>(GpuCensusSection::AlteredUiLayer)];
    uiClass.occurrences = 20;
    uiClass.sampler.totals.ms = 0.4;
    uiClass.sampler.totals.samples = 2;
    uiClass.nullSampler.totals.ms = 0.1;
    uiClass.nullSampler.totals.samples = 2;
    logAndResetWindow(start + 30000);
    line = lineWith("EDVR GPU census, Elite's own draws that EDVR alters");
    check(line && line->find("UI draws (redirected to EDVR's layer) 0.015 (0.10/frame)") != std::string::npos &&
              g_lastLog.find("timer floor 50.0 us/pair") != std::string::npos && g_lastLog.find("spans timed 2,") != std::string::npos,
          "altered line: a class is corrected by its own null pair ((0.4/2 - 0.1/2) x 0.1 = 0.015), and its spans count in the census's totals");
}

// ---- 7: the draws another fix wraps, named fix by fix ---------------------------------------------------------
constexpr const char* kFixLinePrefix = "EDVR GPU census, the other fix-wrapped draws above by the fix that wraps each";
void alteredFixCases() {
    constexpr int kFixes = kAlteredFixCount;
    // The names: one for each fix, fixed strings, all different, in the enum's order.
    bool distinct = true, filled = true;
    for (int i = 0; i < kFixes; ++i) {
        filled = filled && kAlteredFixNames[i] && kAlteredFixNames[i][0];
        for (int j = i + 1; j < kFixes; ++j) distinct = distinct && std::strcmp(kAlteredFixNames[i], kAlteredFixNames[j]) != 0;
    }
    check(filled && distinct, "fix names: one non-empty name for each fix, no two alike");
    check(std::strcmp(kAlteredFixNames[static_cast<int>(AlteredFix::Panel)], "panel distance") == 0 &&
              std::strcmp(kAlteredFixNames[static_cast<int>(AlteredFix::NightVision)], "night vision") == 0 &&
              std::strcmp(kAlteredFixNames[static_cast<int>(AlteredFix::GlareSteady)], "sun glare steady") == 0 &&
              std::strcmp(kAlteredFixNames[static_cast<int>(AlteredFix::Particle)], "particles") == 0 &&
              std::strcmp(kAlteredFixNames[static_cast<int>(AlteredFix::Unnamed)], "unnamed fix") == 0,
          "fix names: each name is at its fix's place in the enum's order");

    // Sections: one each, in order, the last of the census.
    bool sectionsOk = true;
    for (int i = 0; i < kFixes; ++i) {
        const GpuCensusSection sec = alteredSectionOf(AlteredDraw(AlteredDrawClass::Verdict, static_cast<AlteredFix>(i)));
        sectionsOk = sectionsOk && static_cast<int>(sec) == static_cast<int>(GpuCensusSection::AlteredFixFirst) + i &&
                     sec == alteredFixSectionOf(static_cast<AlteredFix>(i)) && sec < GpuCensusSection::Count;
    }
    check(sectionsOk && static_cast<int>(GpuCensusSection::Count) == static_cast<int>(GpuCensusSection::AlteredFixFirst) + kFixes,
          "fix sections: each fix has its own section, in order, and they are the last of the census");
    check(alteredSectionOf(AlteredDraw()) == GpuCensusSection::Count &&
              alteredSectionOf(AlteredDraw(AlteredDrawClass::None, AlteredFix::Remlok)) == GpuCensusSection::Count,
          "fix sections: a draw of no class has no section, whatever fix it carries");
    check(alteredSectionOf(AlteredDraw(AlteredDrawClass::Verdict)) == alteredFixSectionOf(AlteredFix::Unnamed),
          "fix sections: a Verdict draw that names no fix is the unnamed row: visible, not lost");
    check(alteredSectionOf(AlteredDraw(AlteredDrawClass::PoolFamily, AlteredFix::Remlok)) == GpuCensusSection::AlteredPoolFamily,
          "fix sections: a fix carried by a class that is not Verdict changes nothing");

    // Turns: the fix sections share the first one's, every other section keeps its own.
    bool owners = true;
    for (size_t i = 0; i < kSections; ++i) {
        const auto sec = static_cast<GpuCensusSection>(i);
        owners = owners && turnOwnerOf(sec) == (i >= kAlteredFixFirst ? GpuCensusSection::AlteredFixFirst : sec);
    }
    check(owners, "turns: the fix sections share the first one's turn and every other section is its own owner");
    int at = 0;
    unsigned cycle = 0;
    bool onlyOwners = true;
    do {
        at = nextTurnOwner(at);
        ++cycle;
        onlyOwners = onlyOwners && turnOwnerOf(static_cast<GpuCensusSection>(at)) == static_cast<GpuCensusSection>(at);
    } while (at != 0 && cycle < 200);
    check(onlyOwners && cycle == static_cast<unsigned>(kSections) - kAlteredFixCount + 1 - static_cast<unsigned>(kWorldSections) && cycle == 20,
          "turns: the rotation lands only on turn owners, and a cycle is one turn for each section but the later fix ones and the idle world-route ones (20: the 22 there were before the terrain's two sections went, 2026-10-01; the key off changes nothing)");
    // The same cycle once the VR world route's three sections have been called this window: they take turns, 23.
    for (size_t i = kWorldFirst; i < kWorldFirst + kWorldSections; ++i) g_section[i].occurrences = 1;
    at = 0;
    cycle = 0;
    bool worldVisited[kWorldSections] = {};
    do {
        at = nextTurnOwner(at);
        ++cycle;
        if (static_cast<size_t>(at) >= kWorldFirst && static_cast<size_t>(at) < kWorldFirst + kWorldSections)
            worldVisited[static_cast<size_t>(at) - kWorldFirst] = true;
    } while (at != 0 && cycle < 200);
    check(cycle == 23 && worldVisited[0] && worldVisited[1] && worldVisited[2],
          "turns: a world-route section that has been called this window takes its turn (23: the 20 and the three)");
    for (size_t i = kWorldFirst; i < kWorldFirst + kWorldSections; ++i) g_section[i].occurrences = 0;
    for (auto& s : g_section) s = SectionState{};
    g_section[kAlteredFixFirst + 2].occurrences = 7;
    g_section[kAlteredFixFirst + 11].occurrences = 5;
    g_section[static_cast<size_t>(GpuCensusSection::AlteredUiLayer)].occurrences = 100;
    check(turnOccurrences(GpuCensusSection::AlteredFixFirst) == 12 && turnOccurrences(GpuCensusSection::AlteredUiLayer) == 100,
          "turns: the shared turn's calls are every fix's together, another section's are its own");

    // The shared turn in the rotation and the Begin: one call counter, one K, one stride, one empty pair,
    // every call counted for its own fix. (A null context: no timer is asked for; the selection is what is under test.)
    for (auto& s : g_section) s = SectionState{};
    g_windowStartMs = GetTickCount64();   // no window closes here
    auto& owner = g_section[kAlteredFixFirst];
    auto& glare = g_section[static_cast<size_t>(alteredFixSectionOf(AlteredFix::GlareSteady))];
    auto& particle = g_section[static_cast<size_t>(alteredFixSectionOf(AlteredFix::Particle))];
    glare.occurrences = 300;   // two fixes: 300 + 100 calls over ten frames, 40 a frame together
    particle.occurrences = 100;
    owner.turns = 3;
    g_windowFrames = 9;        // gpuCensusFrame counts this frame first: 10
    g_activeSection = static_cast<int>(GpuCensusSection::AlteredUiLayer);
    gpuCensusFrame(nullptr);
    check(g_activeSection == static_cast<int>(GpuCensusSection::AlteredFixFirst),
          "shared turn: the rotation lands on it after the last class");
    check(g_activeStride == 5 && g_activeOffset == 3 && owner.turns == 4,
          "shared turn: the stride is every fix's calls together over K (400 over 10 frames, K 8: every 5th), from the shared turn's own count");
    glare.occurrences = particle.occurrences = 0;
    std::string timedAt;
    for (unsigned call = 0; call < 45; ++call) {
        const GpuCensusSection sec = alteredFixSectionOf(call % 2 ? AlteredFix::Particle : AlteredFix::GlareSteady);
        const unsigned before = g_activeTimed;
        gpuCensusBegin(nullptr, sec);
        if (g_activeTimed > before) timedAt += std::to_string(call) + ",";
        gpuCensusEnd(nullptr, sec);
    }
    check(timedAt == "3,8,13,18,23,28,33,38," && g_activeTimed == 8,
          "shared turn: one call counter and one K across the fixes: every 5th call from the offset, capped at 8");
    check(glare.occurrences == 23 && particle.occurrences == 22,
          "shared turn: each call is counted for its own fix (23 even calls, 22 odd)");
    check(owner.nullPairsTaken == 1 && glare.nullPairsTaken == 0 && particle.nullPairsTaken == 0 && g_activeNullDone,
          "shared turn: ONE empty pair for the turn, kept by the first fix's section whichever fix was timed first");
    // A section that is not the turn's owner is counted and never advances the shared call counter.
    g_activeCalls = g_activeTimed = 0;
    const uint64_t uiBefore = g_section[static_cast<size_t>(GpuCensusSection::AlteredUiLayer)].occurrences;
    check(!gpuCensusBegin(nullptr, GpuCensusSection::AlteredUiLayer) && g_activeCalls == 0 &&
              g_section[static_cast<size_t>(GpuCensusSection::AlteredUiLayer)].occurrences == uiBefore + 1,
          "shared turn: a class section's call during the fixes' turn is counted, not timed, and does not move the fixes' call counter");
    g_activeSection = static_cast<int>(GpuCensusSection::AlteredUiLayer);
    g_activeCalls = g_activeTimed = 0;
    const uint64_t particleBefore = particle.occurrences;
    check(!gpuCensusBegin(nullptr, alteredFixSectionOf(AlteredFix::Particle)) && g_activeCalls == 0 && particle.occurrences == particleBefore + 1,
          "shared turn: a fix's call during another section's turn is counted, not timed");

    // The window's lines. Glare: 1200 calls over 200 frames (6.00 a frame), four timed at 0.4 ms; particles: 200 calls
    // (1.00 a frame), one timed at 0.5 ms; the turn's empty pairs (two, 0.1 ms each) are the first fix's.
    const uint64_t start = GetTickCount64() - 30000;
    freshWindow(start);
    auto& owner2 = g_section[kAlteredFixFirst];
    auto& glare2 = g_section[static_cast<size_t>(alteredFixSectionOf(AlteredFix::GlareSteady))];
    auto& particle2 = g_section[static_cast<size_t>(alteredFixSectionOf(AlteredFix::Particle))];
    auto& remlok2 = g_section[static_cast<size_t>(alteredFixSectionOf(AlteredFix::Remlok))];
    glare2.occurrences = 1200;
    glare2.sampler.totals.ms = 1.6;
    glare2.sampler.totals.samples = 4;
    particle2.occurrences = 200;
    particle2.sampler.totals.ms = 0.5;
    particle2.sampler.totals.samples = 1;
    remlok2.occurrences = 40;   // ran, never timed: 0.000, not '-'
    owner2.nullSampler.totals.ms = 0.2;
    owner2.nullSampler.totals.samples = 2;
    logAndResetWindow(start + 30000);
    const std::string* classes = lineWith("EDVR GPU census, Elite's own draws that EDVR alters");
    const std::string* byFix = lineWith(kFixLinePrefix);
    check(classes != nullptr && byFix != nullptr, "fix line: the classes' line and, after it, a line naming each fix");
    if (classes && byFix) {
        check(classes->find("other fix-wrapped draws 2.200 (7.20/frame)") != std::string::npos &&
                  classes->find("together 2.200 ms/frame") != std::string::npos,
              "fix line: the classes' line keeps its item, now the fixes' sum: (0.4 - 0.1) x 6.00 + (0.5 - 0.1) x 1.00 = 2.200, 7.20 a frame");
        check(byFix->find("sun glare steady 1.800 (6.00/frame)") != std::string::npos &&
                  byFix->find("particles 0.400 (1.00/frame)") != std::string::npos,
              "fix line: each fix's ms/frame with its calls a frame, corrected by the turn's empty pair (first fix's)");
        check(byFix->find("RemLok overlay 0.000 (0.20/frame)") != std::string::npos,
              "fix line: a fix that ran and was never timed reads 0.000 with its calls, like a class does");
        check(byFix->find("night vision -") != std::string::npos && byFix->find("panel distance -") != std::string::npos &&
                  byFix->find("unnamed fix -") != std::string::npos,
              "fix line: a fix that never ran prints '-', including the unnamed row");
        check(byFix->find("\"-\" means no draw of that fix ran this window") != std::string::npos,
              "fix line: it says what '-' means");
        check(byFix->find("sun glare clamp") != std::string::npos && byFix->find("FSS reveal") != std::string::npos &&
                  byFix->find("scanner-body resolve") != std::string::npos && byFix->find("menu backdrop") != std::string::npos,
              "fix line: every named fix has its place on the line, ran or not");
    }
    check(g_lastLog.find("timer floor 100.0 us/pair") != std::string::npos && g_lastLog.find("spans timed 5,") != std::string::npos,
          "fix line: the empty pairs kept by the first fix count in the census's timer floor and spans");
    check(g_lastLog.find("EDVR ~0.000 ms/frame") != std::string::npos && g_lastLog.find("sun glare") == std::string::npos,
          "fix line: the fixes' draws are the game's, timed whole: not in EDVR's total and not on the main line");
    check(glare2.occurrences == 0 && owner2.occurrences == 0, "fix line: the window resets the fix sections' occurrences");

    // Nothing ran: every fix '-', and the sum item on the classes' line reads '-' too.
    freshWindow(start);
    logAndResetWindow(start + 30000);
    classes = lineWith("EDVR GPU census, Elite's own draws that EDVR alters");
    byFix = lineWith(kFixLinePrefix);
    std::string allAbsent;
    for (int i = 0; i < kFixes; ++i) allAbsent += std::string(i ? ", " : "") + kAlteredFixNames[i] + " -";
    check(byFix && byFix->find(allAbsent + ";") != std::string::npos,
          "fix line: a window with no wrapped draw prints all sixteen fixes with '-' in the enum's order");
    check(classes && classes->find("other fix-wrapped draws -;") != std::string::npos,
          "fix line: and the classes' line's sum item is '-' too");

    // A Verdict draw that named no fix is the unnamed row.
    freshWindow(start);
    g_section[static_cast<size_t>(alteredSectionOf(AlteredDraw(AlteredDrawClass::Verdict)))].occurrences = 20;
    logAndResetWindow(start + 30000);
    byFix = lineWith(kFixLinePrefix);
    check(byFix && byFix->find("unnamed fix 0.000 (0.10/frame)") != std::string::npos,
          "fix line: a wrapped draw whose fix has no name shows as the unnamed row, never in another fix's");
}

// ---- 8: the HDR HUD depth-stencil seed: its section, its item and line, its scope, and the call site that holds it ----
SectionState& seedSection() { return g_section[static_cast<size_t>(GpuCensusSection::FrameUiLayerHdrSeed)]; }
// The seed's section as a window would leave it: `occurrences` seeds, `samples` of them timed for `ms` in all.
void seedRan(unsigned occurrences, double ms, unsigned samples) {
    SectionState& s = seedSection();
    s.occurrences = occurrences;
    s.sampler.totals.ms = ms;
    s.sampler.totals.samples = samples;
}
int indexOfLine(const char* prefix) {
    for (size_t i = 0; i < g_lines.size(); ++i)
        if (g_lines[i].rfind(prefix, 0) == 0) return static_cast<int>(i);
    return -1;
}

void seedTableCases() {
    constexpr GpuCensusSection seed = GpuCensusSection::FrameUiLayerHdrSeed;
    check(!isDoorSection(seed) && occurrenceCapFor(seed) == 8 && turnOwnerOf(seed) == seed,
          "seed section: an in-frame section like the others: per draw, K = 8, a turn of its own in the rotation");
    check(static_cast<size_t>(seed) == kDoorSections + 7 && static_cast<size_t>(seed) + 1 == kWorldFirst &&
              kWorldFirst + kWorldSections == kAlteredFirst &&
              seed > GpuCensusSection::FrameUiLayerReissues && seed < GpuCensusSection::AlteredPoolFamily,
          "seed section: the eighth in-frame section (the ninth before the terrain's went, 2026-10-01), followed by the three world-route ones, so the altered-draw sections still follow the in-frame ones");
    bool filled = true, distinct = true;
    for (size_t i = 0; i < kFrameSections; ++i) {
        filled = filled && kFrameBreakdownNames[i] && kFrameBreakdownNames[i][0];
        for (size_t j = i + 1; j < kFrameSections; ++j)
            distinct = distinct && std::strcmp(kFrameBreakdownNames[i], kFrameBreakdownNames[j]) != 0;
    }
    check(kFrameSections == 11 && filled && distinct &&
              std::strcmp(kFrameBreakdownNames[kSeedSection - kDoorSections], "HDR HUD depth-stencil seed") == 0 &&
              std::strcmp(kFrameBreakdownNames[kFrameSections - 3], "world resolve") == 0 &&
              std::strcmp(kFrameBreakdownNames[kFrameSections - 2], "world mips") == 0 &&
              std::strcmp(kFrameBreakdownNames[kFrameSections - 1], "world layer") == 0,
          "seed section: eleven in-frame items, none alike, the eighth named what the UI layer's own line calls the stage, the last three the world route's");
}

void seedLineCases() {
    const uint64_t start = GetTickCount64() - 30000;

    // The seed ran: 700 seeds in 200 frames (3.50 a frame), 96 timed for 24 ms in all (0.25 each), the turn's empty
    // pair 0.05 ms. Beside it the planet reissue, 0.200 ms/frame, so the in-frame total must be the two together.
    freshWindow(start);
    seedRan(700, 24.0, 96);
    seedSection().nullSampler.totals.ms = 0.10;
    seedSection().nullSampler.totals.samples = 2;
    auto& planet = g_section[static_cast<size_t>(GpuCensusSection::FramePlanet)];
    planet.occurrences = 20;
    planet.sampler.totals.ms = 4.0;
    planet.sampler.totals.samples = 2;
    gpuCensusNoteSeedTarget(kSeedAt125);
    gpuCensusNoteSeedTarget(kSeedAt125);   // every seed reports: the same target twice is one target
    logAndResetWindow(start + 30000);
    check(g_lastLog.find("UI layer reissues -, HDR HUD depth-stencil seed 0.700 (3.50/frame)") != std::string::npos,
          "seed line: the seed is the last in-frame item, (0.25 - 0.05) ms x 3.50 a frame = 0.700, with its seeds a frame");
    check(g_lastLog.find("in-frame 0.900 (") != std::string::npos && g_lastLog.find("EDVR ~0.900 ms/frame = door 0.000 (") != std::string::npos,
          "seed line: the seed is IN EDVR's in-frame total and in EDVR's total (0.700 + the planet's 0.200), not beside them");
    const int mainAt = indexOfLine("EDVR GPU census:");
    const int seedAt = indexOfLine(kSeedDetailPrefix);
    const int alteredAt = indexOfLine("EDVR GPU census, Elite's own draws that EDVR alters");
    check(mainAt >= 0 && seedAt == mainAt + 1 && alteredAt > seedAt,
          "seed line: its detail follows the main line at once, before the altered draws' lines");
    const std::string* detail = lineWith(kSeedDetailPrefix);
    check(detail != nullptr &&
              detail->find(": 3.50 seeds a frame, 0.200 ms a seed (96 timed); target 5040x4870 D32_FLOAT_S8X24_UINT (196.4 MB), "
                           "seeded from the game's 3024x2922.") != std::string::npos,
          "seed line: the detail line says what it timed: seeds a frame, ms a seed, timed count, the layer's target and its memory, the game's size");
    check(detail && detail->find("changed inside") == std::string::npos,
          "seed line: two identical reports are one target, not a change");
    check(g_seedNotes == 0 && !g_seedMixed && g_seedFirst.layerW == 0,
          "seed line: the window's reports are cleared with the window");

    // NEGATIVE CONTROL: the seed never ran (the layer off, or no HUD draw that tests the game's depth). '-' on its
    // item, nothing of it in the total, no line of its own -- and nothing left over from the window before.
    freshWindow(start);
    planet.occurrences = 20;
    planet.sampler.totals.ms = 4.0;
    planet.sampler.totals.samples = 2;
    logAndResetWindow(start + 30000);
    check(g_lastLog.find("UI layer reissues -, HDR HUD depth-stencil seed -") != std::string::npos &&
              g_lastLog.find("in-frame 0.200 (") != std::string::npos,
          "seed line (never ran): the item is '-', not 0.000, and the in-frame total is the planet's alone");
    check(lineWith(kSeedDetailPrefix) == nullptr && g_lastLog.find("seed 0.") == std::string::npos,
          "seed line (never ran): no detail line, so '-' cannot be read as a measurement");
    freshWindow(start);
    seedRan(700, 24.0, 96);
    gpuCensusNoteSeedTarget(kSeedAt125);
    logAndResetWindow(start + 30000);
    g_lines.clear();
    g_lastLog.clear();
    g_windowFrames = 200;
    g_windowStartMs = start;
    logAndResetWindow(start + 30000);   // the next window: no seed
    check(lineWith(kSeedDetailPrefix) == nullptr && g_lastLog.find("HDR HUD depth-stencil seed -") != std::string::npos,
          "seed line (never ran): the window after a window with seeds shows '-' and no stale target");

    // The seeds ran and reported no target: the line says so, never a target that was not reported.
    freshWindow(start);
    seedRan(20, 0.4, 2);
    logAndResetWindow(start + 30000);
    detail = lineWith(kSeedDetailPrefix);
    check(detail && detail->find(": 0.10 seeds a frame, 0.200 ms a seed (2 timed); the seeds reported no target.") != std::string::npos &&
              g_lastLog.find("HDR HUD depth-stencil seed 0.020 (0.10/frame)") != std::string::npos,
          "seed line (no target reported): the item is a measurement and the line says the seeds reported no target");

    // Seeds counted, none timed (the section's turn came when the timer was busy): 0.000 with its count, and the line says so.
    freshWindow(start);
    seedRan(20, 0.0, 0);
    gpuCensusNoteSeedTarget(kSeedAt100);
    logAndResetWindow(start + 30000);
    detail = lineWith(kSeedDetailPrefix);
    check(g_lastLog.find("HDR HUD depth-stencil seed 0.000 (0.10/frame)") != std::string::npos && detail &&
              detail->find(": 0.10 seeds a frame, no seed timed this window; target 4032x3896 D32_FLOAT_S8X24_UINT (125.7 MB), "
                           "seeded from the game's 3024x2922.") != std::string::npos,
          "seed line (none timed): 0.000 is said to be no timed seed, and UI 100's smaller target reads 125.7 MB");

    // A window the UI quality changed inside: 125 then 100 then 125 again. Both targets are named, in the order seen,
    // so a figure that mixes two sizes cannot pass for one.
    freshWindow(start);
    seedRan(700, 24.0, 96);
    gpuCensusNoteSeedTarget(kSeedAt125);
    gpuCensusNoteSeedTarget(kSeedAt100);
    gpuCensusNoteSeedTarget(kSeedAt125);
    logAndResetWindow(start + 30000);
    detail = lineWith(kSeedDetailPrefix);
    check(detail &&
              detail->find("the target changed inside this window, so the figure mixes them: first 5040x4870 D32_FLOAT_S8X24_UINT "
                           "(196.4 MB), seeded from the game's 3024x2922; then 4032x3896 D32_FLOAT_S8X24_UINT (125.7 MB), "
                           "seeded from the game's 3024x2922.") != std::string::npos,
          "seed line (mixed): a window with two targets names both, first and then, and says the figure mixes them");
    // A target with no format name is told as such, not dereferenced.
    freshWindow(start);
    seedRan(20, 0.4, 2);
    gpuCensusNoteSeedTarget({640, 480, 4, 320, 240, nullptr});
    logAndResetWindow(start + 30000);
    detail = lineWith(kSeedDetailPrefix);
    check(detail && detail->find("target 640x480 unknown format (1.2 MB), seeded from the game's 320x240.") != std::string::npos,
          "seed line: a report with no format name reads 'unknown format'");
}

// The scope, on a null context: the counting, the noting and the turn's selection are under test, not a timer.
// (The real timer is in rotationAndRealTimerCase, on WARP.)
void seedScopeCases() {
    for (auto& s : g_section) s = SectionState{};
    resetSeedNotes();
    g_windowStartMs = GetTickCount64();   // no window closes here
    const auto occurrencesEverywhere = [] {
        uint64_t total = 0;
        for (const auto& s : g_section) total += s.occurrences;
        return total;
    };
    g_activeSection = static_cast<int>(GpuCensusSection::FrameUiLayerHdrSeed);
    g_activeCalls = g_activeTimed = 0;
    g_activeStride = 1;
    g_activeOffset = 0;
    g_activeNullDone = false;
    { GpuCensusSeedScope hdr(nullptr, true, kSeedAt125); }
    check(seedSection().occurrences == 1 && g_seedNotes == 1 && g_activeTimed == 1 && g_activeCalls == 1,
          "seed scope: an HDR HUD seed on the section's turn is counted, noted, and selected for timing");
    // NEGATIVE CONTROL: the 8-bit layer's seed. Counted nowhere, noted nowhere, and it takes nothing of the turn.
    { GpuCensusSeedScope ldr(nullptr, false, kSeedAt100); }
    check(occurrencesEverywhere() == 1 && g_seedNotes == 1 && !g_seedMixed && g_activeTimed == 1 && g_activeCalls == 1,
          "seed scope (8-bit layer): the 8-bit UI layer's seed is counted in no section, reports no target, and takes nothing of the turn");
    // Another section's turn: counted and noted (the target is a fact about every seed), not timed.
    g_activeSection = static_cast<int>(GpuCensusSection::FramePlanet);
    g_activeCalls = g_activeTimed = 0;
    { GpuCensusSeedScope hdr(nullptr, true, kSeedAt100); }
    check(seedSection().occurrences == 2 && g_seedNotes == 2 && g_seedMixed && g_activeCalls == 0 && g_activeTimed == 0,
          "seed scope: on another section's turn a seed is counted and its target noted, never timed, and moves no call counter");
    check(std::strcmp(g_seedFirst.format, "D32_FLOAT_S8X24_UINT") == 0 && g_seedFirst.layerW == 5040 && g_seedFirst.gameW == 3024 &&
              g_seedOther.layerW == 4032 && g_seedOther.layerH == 3896,
          "seed scope: the first target and the first different one are kept as reported");
    // The K cap: nine seeds in a frame on the section's turn are all counted, eight timed.
    for (auto& s : g_section) s = SectionState{};
    resetSeedNotes();
    g_activeSection = static_cast<int>(GpuCensusSection::FrameUiLayerHdrSeed);
    g_activeCalls = g_activeTimed = 0;
    g_activeNullDone = false;
    for (int i = 0; i < 9; ++i) { GpuCensusSeedScope hdr(nullptr, true, kSeedAt125); }
    check(seedSection().occurrences == 9 && g_activeTimed == 8 && g_seedNotes == 9,
          "seed scope: nine seeds in one frame are all counted and noted, and the per-draw cap of eight is the most timed");
}

// The seed's call site, held by a scan of ui_layer.cpp: the scope is constructed ONCE in seedLayerDepth, before the
// copy, and its layer argument is exactly the HDR HUD stage. gpu_census.h's scope cannot know which layer a seed is
// for; this is what stops it counting the 8-bit layer's, or nothing at all, and still passing every case above.
// Returns the first thing wrong, or nullptr.
std::string withoutLineComments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, 2, "//") == 0) {
            while (i < text.size() && text[i] != '\n') ++i;
        } else {
            out += text[i++];
        }
    }
    return out;
}
const char* seedWiringProblem(const std::string& raw) {
    std::string source;
    for (const char c : raw) if (c != '\r') source += c;
    const size_t fn = source.find("bool seedLayerDepth(");
    if (fn == std::string::npos) return "seedLayerDepth is not in the file";
    const size_t end = source.find("\n}\n", fn);
    if (end == std::string::npos) return "seedLayerDepth has no end";
    const std::string body = withoutLineComments(source.substr(fn, end - fn));
    const std::string name = "GpuCensusSeedScope";
    const size_t scope = body.find(name);
    if (scope == std::string::npos) return "the seed does not construct a GpuCensusSeedScope";
    if (body.find(name, scope + 1) != std::string::npos) return "the seed constructs more than one GpuCensusSeedScope";
    const size_t stmtEnd = body.find(';', scope);
    if (stmtEnd == std::string::npos) return "the scope's statement does not end";
    const std::string stmt = body.substr(scope, stmtEnd - scope);
    if (stmt.find("stage == UiRouteStage::kHdrSeed") == std::string::npos)
        return "the scope's layer argument is not 'stage == UiRouteStage::kHdrSeed'";
    // The target the line reports is the layer's own: its size, its format's bytes, the game's size and the format's name.
    if (stmt.find("{outW, outH, dsBytesPerPixel(vd.Format), td.Width, td.Height, viewName(vd.Format)}") == std::string::npos)
        return "the scope's target is not the layer's size, its format's bytes, the game's size and the format's name";
    const size_t copy = body.find("vScreenCopyResourceRaw(");
    const size_t exec = body.find("vScreenExecuteCommandListRaw(");
    if (copy == std::string::npos || exec == std::string::npos) return "the seed's copy or its execute is not in seedLayerDepth";
    if (scope > copy || scope > exec) return "the scope is entered after the seed's copy: its GPU time is outside the section";
    return nullptr;
}

void seedWiringCases() {
    std::ifstream in("src/d3d11/ui_layer.cpp", std::ios::binary);
    check(bool(in), "seed wiring: src/d3d11/ui_layer.cpp opens (this rig runs from the repository root, as build.bat does)");
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const char* problem = seedWiringProblem(source);
    check(problem == nullptr, problem ? problem : "seed wiring");

    const std::string head = "GpuCensusSeedScope census(";
    const size_t at = source.find(head);
    check(at != std::string::npos, "seed wiring: the scope is named `census` in ui_layer.cpp, so the mutants below can find it");
    const size_t stmtEnd = source.find(';', at) + 1;
    const std::string stmt = source.substr(at, stmtEnd - at);
    const std::string copyCall = "vScreenCopyResourceRaw(ctx, e.dsCopy.Get(), tex.Get());";
    // Mutants of the call site, each of which must be caught by ITS rule (the message says which), and each a real edit.
    struct Mutant { const char* name; std::string text; const char* caughtBy; };
    std::vector<Mutant> mutants;
    mutants.push_back({"the scope deleted", source.substr(0, at) + source.substr(stmtEnd), "does not construct"});
    {
        std::string wrongStage = stmt;
        wrongStage.replace(wrongStage.find("kHdrSeed"), 8, "kSeed");
        mutants.push_back({"the 8-bit layer's stage", source.substr(0, at) + wrongStage + source.substr(stmtEnd), "layer argument"});
        std::string always = stmt;
        always.replace(always.find("stage == UiRouteStage::kHdrSeed"), 31, "true");
        mutants.push_back({"the layer argument always true", source.substr(0, at) + always + source.substr(stmtEnd), "layer argument"});
        std::string swapped = stmt;
        swapped.replace(swapped.find("{outW, outH,"), 12, "{outH, outW,");
        mutants.push_back({"the target's width and height swapped", source.substr(0, at) + swapped + source.substr(stmtEnd), "target is not"});
    }
    mutants.push_back({"the scope commented out", source.substr(0, at) + "// " + stmt + source.substr(stmtEnd), "does not construct"});
    mutants.push_back({"the scope constructed twice", source.substr(0, stmtEnd) + "\n    " + stmt + source.substr(stmtEnd), "more than one"});
    {
        std::string moved = source.substr(0, at) + source.substr(stmtEnd);
        const size_t copyAt = moved.find(copyCall);
        check(copyAt != std::string::npos, "seed wiring: the copy call is spelled as the scan expects");
        moved.insert(copyAt + copyCall.size(), "\n    " + stmt);
        mutants.push_back({"the scope entered after the copy", moved, "after the seed's copy"});
    }
    for (const auto& m : mutants) {
        const char* why = seedWiringProblem(m.text);
        const std::string edited = std::string("seed wiring mutant is a real edit: ") + m.name;
        const std::string caught = std::string("seed wiring mutant caught by its own rule: ") + m.name;
        check(m.text != source, edited.c_str());
        check(why != nullptr && std::strstr(why, m.caughtBy) != nullptr, caught.c_str());
    }
    std::printf("gpu_census_test: the seed's call site held by a scan of ui_layer.cpp; %zu mutants of it, each caught by its own rule\n",
                mutants.size());
}

// Every census line at its worst stays under what the log keeps (about 1166 characters of message).
void lineLengths() {
    const uint64_t start = GetTickCount64() - 30000;
    freshWindow(start);
    for (auto& s : g_section) {
        s.occurrences = 999999;            // 9999.99 a frame over the 100 frames below
        s.sampler.totals.ms = 1.0;         // 1 ms a call: 9999.990 ms/frame, four digits before the point
        s.sampler.totals.samples = 1;
    }
    // The seed's detail line at its widest: two different targets (so it names both) at the largest numbers and the
    // longest format name the note keeps.
    gpuCensusNoteSeedTarget({4294967295u, 4294967295u, 4294967295u, 4294967295u, 4294967295u, "1234567890123456789012345678901"});
    gpuCensusNoteSeedTarget({4294967294u, 4294967294u, 4294967294u, 4294967294u, 4294967294u, "1234567890123456789012345678901"});
    g_windowFrames = 100;
    for (unsigned i = 0; i < 4000; ++i) g_p50Samples[i] = 99999.0;
    g_p50Count = 4000;
    feedFrames(400, 1, [](unsigned i) -> uint64_t { return 1000000ull + i; });
    logAndResetWindow(start + 30000);
    size_t longest = 0;
    for (const auto& l : g_lines) longest = std::max(longest, l.size());
    std::printf("gpu_census_test: the longest census line at its worst is %zu characters (the log keeps about 1166)\n", longest);
    check(longest < 1150, "lines: every census line at its worst fits the log's line, not cut");
    const std::string* byFix = lineWith(kFixLinePrefix);
    const std::string* classes = lineWith("EDVR GPU census, Elite's own draws that EDVR alters");
    check(byFix && classes, "lines: both altered-draw lines were written at the worst case");
    if (byFix && classes) {
        std::printf("gpu_census_test: at their worst the classes' line is %zu characters and the fixes' line %zu\n", classes->size(), byFix->size());
        check(byFix->size() < 1100 && byFix->size() > 700, "lines: the fixes' line names all sixteen at their widest and still fits");
    }
    const std::string* seedDetail = lineWith(kSeedDetailPrefix);
    check(seedDetail != nullptr, "lines: the seed's detail line was written at the worst case");
    if (seedDetail) {
        std::printf("gpu_census_test: at its worst the seed's detail line is %zu characters, and the main line %zu\n", seedDetail->size(), g_lastLog.size());
        check(seedDetail->size() < 900 && seedDetail->find("the target changed inside this window") != std::string::npos,
              "lines: the seed's detail line at its widest (two targets named) still fits");
    }
}

void run() {
    ownershipMapAndAggregateCases();
    estimatorMathCases();
    calibrationMathCases();
    logFormatCase();
    pluginCostEvidenceCases();
    logicalOwnerReportCases();
    gapCases();
    alteredClassCases();
    alteredLineCases();
    alteredFixCases();
    seedTableCases();
    seedLineCases();
    seedScopeCases();
    seedWiringCases();
    lineLengths();
    Runtime runtime;
    Device device(runtime);
    rotationAndRealTimerCase(device);
    std::printf("PASS: %u GPU census checks\n", g_checks);
}

} // namespace
} // namespace edvr

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && !wcscmp(argv[1], L"--dry-run")) {
        std::puts("dry-run: no module, device, queries or files");
        return 0;
    }
    if (argc == 2 && !wcscmp(argv[1], L"--child")) {
        try { edvr::run(); return 0; }
        catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: %s\n", error.what());
            return 1;
        }
    }
    if (argc != 2 || wcscmp(argv[1], L"--self-test")) {
        std::fputs("usage: --self-test | --dry-run\n", stderr);
        return 2;
    }
    // Relaunched as a child (gpu_timing_test's own convention): a WARP
    // device and the shared timing registry are process-global state, and
    // an unclean exit here must never be read as this harness's own.
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return 2;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --child";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessW(executable, &command[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return 2;
    const DWORD waited = WaitForSingleObject(process.hProcess, 30000);
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    else {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
        std::fputs("FAIL: owned test child timed out\n", stderr);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (code != 0) std::fprintf(stderr, "FAIL: owned D3D11 child exited 0x%08lX\n", code);
    return code == 0 ? 0 : 1;
}
