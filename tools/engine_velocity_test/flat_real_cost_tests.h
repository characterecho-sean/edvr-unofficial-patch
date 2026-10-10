#pragma once

// A separate WARP link variant: the engine's real sampled restore writes into
// the real plugin-cost collector, then a real PresentBoundary closes its window.
// The runtime caller is source-pinned by flat_lazy_tests; this fixture invokes
// the production helper directly and does not model a hooked Present.
#include <atomic>
#include <thread>

#include "../../src/d3d11/plugin_cost_boundary.h"

namespace flat_real_cost_tests {
using flat_lazy_tests::Emu;
using flat_lazy_tests::Game;
using flat_lazy_tests::Harness;
using flat_lazy_tests::RecOn;
using flat_lazy_tests::SceneCleanup;
using flat_lazy_tests::g_rec;
using flat_lazy_tests::kOMSetBlend;
using flat_lazy_tests::kOMSetRT;

struct RecordedContext {
    explicit RecordedContext(ID3D11DeviceContext* ctx) { flat_lazy_tests::recInstall(ctx); }
    ~RecordedContext() { flat_lazy_tests::recRemove(); }
};

inline void runWindow(const Harness& h, bool shutdownWhileHeld) {
    namespace cost = edvr::plugin_cost;
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    edvr::g_clockForTest = &lifecycle_fake::fakeClock;
    edvr::flatQueryCut().reset();
    Game g(h);
    g.setup();
    SceneCleanup cleanup(g.ctx);
    RecordedContext recording(g.ctx);
    for (auto& slot : lifecycle_fake::g_slots) {
        slot.ptr = nullptr;
        slot.hash = 0;
        ++slot.gen;
    }
    edvr::engineVelocityConfigure(true);
    edvr::engineVelocityFlatLazy(true);
    g.makeSource(40, 24);
    Emu<> e(h, g);
    e.warmUp();
    h.check(e.ok, "real cost: WARP warm-up leaves the game state intact");

    LARGE_INTEGER frequency{};
    h.check(QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0,
            "real cost: QPC frequency is available");
    edvrPluginCostShutdown();
    edvrPluginCostSetOwnerContext(g.ctx);
    edvrPluginCostConfigure(0x2u, static_cast<uint64_t>(frequency.QuadPart));
    cost::PresentBoundary boundary;
    EdvrPluginCostWindowV2 report{};
    h.check(!boundary.close(1, false, &report) && !boundary.apiSampleFrame,
            "real cost: configure discards the first close and starts unsampled");
    for (uint32_t frame = 2; frame <= 15; ++frame)
        h.check(!boundary.close(frame, false, &report) && !boundary.apiSampleFrame,
                "real cost: early frame closes remain unsampled and incomplete");

    // Establish the owed WARP restore before sampling turns on. Producer
    // binding calls then cannot inflate the two expected restore notes.
    e.startFrame();
    h.check(e.producer("real cost held producer") && e.ok &&
                edvr::engineVelocityFlatPending() && !cost::apiSampleHint(),
            "real cost: actual unsampled producer leaves one held restore");
    if (shutdownWhileHeld) {
        edvr::engineVelocityConfigure(false);
        h.check(!edvr::engineVelocityActive() && edvr::engineVelocityFlatPending(),
                "real cost: stand-down retains the owed state restore");
    }
    h.check(!boundary.close(16, false, &report) && boundary.apiSampleFrame &&
                cost::apiSampleHint() && edvrPluginCostApiSampleContext(g.ctx) == 1,
            "real cost: close 16 opens the owner-context sampled frame 17");
    std::atomic<uint8_t> foreignAccepted{1};
    std::thread foreign([&] {
        foreignAccepted.store(edvrPluginCostApiSampleContext(g.ctx), std::memory_order_relaxed);
    });
    foreign.join();
    h.check(foreignAccepted.load(std::memory_order_relaxed) == 0 &&
                edvrPluginCostApiSampleContext(h.device) == 0,
            "real cost: foreign thread and foreign context cannot sample the owner frame");

    g_rec.reset();
    {
        RecOn on;
        edvr::engineVelocityFlatFlushSampledBoundary(
            g.ctx, edvr::EngineVelocityFlushCause::kPresent);
    }
    h.check(flat_lazy_tests::restoreSettersAre(g_rec, 1, 1) &&
                g_rec.calls[kOMSetBlend] == 1 && g_rec.calls[kOMSetRT] == 1 &&
                !edvr::engineVelocityFlatPending(),
            "real cost: sampled Present physically restores blend then targets exactly once");
    e.expectGame("real cost sampled Present restore");
    h.check(e.ok, "real cost: sampled restore returns the actual WARP game state");
    if (!shutdownWhileHeld) e.present();
    h.check(!boundary.close(17, false, &report) && !boundary.apiSampleFrame &&
                !cost::apiSampleHint() && edvrPluginCostApiSampleContext(g.ctx) == 0,
            "real cost: sampled frame 17 closes before unsampled frame 18");

    if (!shutdownWhileHeld) {
        e.startFrame();
        h.check(e.producer("real cost unsampled producer") && e.ok &&
                    edvr::engineVelocityFlatPending(),
                "real cost: frame 18 produces another held WARP restore");
        g_rec.reset();
        {
            RecOn on;
            edvr::engineVelocityFlatFlushSampledBoundary(
                g.ctx, edvr::EngineVelocityFlushCause::kPresent);
        }
        h.check(flat_lazy_tests::restoreSettersAre(g_rec, 1, 1) &&
                    !edvr::engineVelocityFlatPending(),
                "real cost: unsampled frame 18 still performs both physical restores");
        e.expectGame("real cost unsampled Present restore");
        h.check(e.ok, "real cost: unsampled restore returns the actual WARP game state");
        e.present();
    }
    h.check(!boundary.close(18, false, &report),
            "real cost: frame 18 remains inside the collector window");
    for (uint32_t frame = 19; frame <= 1800; ++frame)
        h.check(!boundary.close(frame, false, &report),
                "real cost: boundary-only frames cannot close the window early");
    h.check(boundary.close(1801, false, &report),
            "real cost: the production collector returns the 1800-frame V2 window");

    constexpr unsigned owner = static_cast<unsigned>(cost::Owner::TemporalAa);
    constexpr unsigned state = static_cast<unsigned>(cost::ApiClass::State);
    const auto& row = report.owners[owner];
    const uint64_t restoreSites = (uint64_t(1) << (132 & 63)) |
                                  (uint64_t(1) << (135 & 63));
    bool onlyState = row.apiCalls[state] == 2;
    for (unsigned apiClass = 0; apiClass < cost::kApiClassCount; ++apiClass)
        if (apiClass != state) onlyState &= row.apiCalls[apiClass] == 0;
    bool onlySites = row.apiSiteMask[0] == 0 && row.apiSiteMask[1] == 0 &&
                     row.apiSiteMask[2] == restoreSites && row.apiSiteMask[3] == 0;
    h.check(report.version == cost::kWindowV2Version && report.profileBit == 0x2u &&
                report.firstFrame == 2 && report.lastFrame == 1801 &&
                report.windowFrames == cost::kWindowFrameCount &&
                report.completedCpuSampleFrames == 0 &&
                report.completedApiSampleFrames == 112 &&
                report.cpuTraceSuppressedFrames == 0 &&
                row.owner == owner && row.apiObserved == 1 && row.cpuObserved == 0 &&
                onlyState && onlySites,
            shutdownWhileHeld
                ? "real cost: owed post-shutdown restore is counted in the exact sampled V2 window"
                : "real cost: sampled restore only, not unsampled physical work, reaches the V2 window");
    for (unsigned other = 0; other < cost::kOwnerCount; ++other) {
        if (other == owner) continue;
        for (unsigned apiClass = 0; apiClass < cost::kApiClassCount; ++apiClass)
            h.check(report.owners[other].apiCalls[apiClass] == 0,
                    "real cost: no other plugin owner receives the restore calls");
    }
    edvrPluginCostShutdown();
    h.check(!boundary.close(1802, false, &report) && !boundary.apiSampleFrame &&
                !cost::apiSampleHint(),
            "real cost: shutdown refuses to republish a sampling flag");
}

inline void run(const Harness& h) {
    runWindow(h, false);
    runWindow(h, true);
}
} // namespace flat_real_cost_tests
