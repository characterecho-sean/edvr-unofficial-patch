#include "../common/plugin_cost.h"

#include <atomic>
#include <cmath>

namespace {

struct FrameOwner {
    uint64_t ticks = 0;
    uint64_t timedScopes = 0;
    uint64_t reached = 0;
    uint64_t invoked = 0;
    uint64_t notEligible = 0;
    uint64_t cpuSiteMask[2] = {};
    uint64_t apiCalls[edvr::plugin_cost::kApiClassCount] = {};
    uint64_t apiSiteMask[2] = {};
};

struct WindowOwner {
    uint64_t timedScopes = 0;
    uint64_t reached = 0;
    uint64_t invoked = 0;
    uint64_t notEligible = 0;
    uint64_t cpuSiteMask[2] = {};
    double meanMs = 0.0;
    double m2Ms = 0.0;
    uint64_t apiCalls[edvr::plugin_cost::kApiClassCount] = {};
    uint64_t apiSiteMask[2] = {};
    bool cpuObserved = false;
    bool apiObserved = false;
};

FrameOwner g_frame[edvr::plugin_cost::kOwnerCount] = {};
WindowOwner g_window[edvr::plugin_cost::kOwnerCount] = {};
uint8_t g_profileBit = 0;
uint64_t g_qpcFrequency = 0;
uint32_t g_windowFrames = 0;
uint32_t g_firstFrame = 0;
uint32_t g_lastFrame = 0;
uint64_t g_cpuSampleFrames = 0;
uint64_t g_apiSampleFrames = 0;
uint64_t g_cpuTraceSuppressedFrames = 0;
bool g_configured = false;
bool g_skipNextBoundary = false;
bool g_apiSampleFrame = false;
bool g_traceSuppressed = false;
std::atomic<void*> g_ownerContext{nullptr};
std::atomic<uintptr_t> g_ownerThreadToken{0};
std::atomic<uintptr_t> g_nextThreadToken{1};
thread_local uintptr_t g_threadToken = 0;

uintptr_t threadToken() noexcept {
    if (g_threadToken == 0) {
        uintptr_t token = g_nextThreadToken.fetch_add(1, std::memory_order_relaxed);
        // Zero is the unpublished sentinel. Exhaustion is not realistic, but
        // keep wraparound from turning a thread into the sentinel owner.
        if (token == 0) token = g_nextThreadToken.fetch_add(1, std::memory_order_relaxed);
        g_threadToken = token;
    }
    return g_threadToken;
}

void clearFrame() noexcept {
    for (uint8_t i = 0; i < edvr::plugin_cost::kOwnerCount; ++i) g_frame[i] = {};
}

void clearWindow() noexcept {
    for (uint8_t i = 0; i < edvr::plugin_cost::kOwnerCount; ++i) g_window[i] = {};
    g_windowFrames = 0;
    g_firstFrame = 0;
    g_lastFrame = 0;
    g_cpuSampleFrames = 0;
    g_apiSampleFrames = 0;
    g_cpuTraceSuppressedFrames = 0;
}

bool validOwnerSite(uint8_t owner, uint16_t site) noexcept {
    return owner < edvr::plugin_cost::kOwnerCount && site <= edvr::plugin_cost::kMaxSiteId;
}

void setBit(uint64_t mask[2], uint16_t site) noexcept {
    mask[site >> 6] |= uint64_t(1) << (site & 63);
}

void addCpuSample() noexcept {
    ++g_cpuSampleFrames;
    for (uint8_t i = 0; i < edvr::plugin_cost::kOwnerCount; ++i) {
        WindowOwner& window = g_window[i];
        const FrameOwner& frame = g_frame[i];
        window.timedScopes += frame.timedScopes;
        window.reached += frame.reached;
        window.invoked += frame.invoked;
        window.notEligible += frame.notEligible;
        window.cpuSiteMask[0] |= frame.cpuSiteMask[0];
        window.cpuSiteMask[1] |= frame.cpuSiteMask[1];

        // CPU scopes clock one draw in each 64-draw stride. Scale only their
        // owner-local frame total; API call notes below are never stride-scaled.
        const double ms = static_cast<double>(frame.ticks) *
                          static_cast<double>(edvr::plugin_cost::kCpuDrawStride) *
                          1000.0 / static_cast<double>(g_qpcFrequency);
        const uint64_t n = g_cpuSampleFrames;
        const double delta = ms - window.meanMs;
        window.meanMs += delta / static_cast<double>(n);
        window.m2Ms += delta * (ms - window.meanMs);
        if (frame.timedScopes != 0) window.cpuObserved = true;
    }
}

void addApiSample() noexcept {
    ++g_apiSampleFrames;
    for (uint8_t i = 0; i < edvr::plugin_cost::kOwnerCount; ++i) {
        WindowOwner& window = g_window[i];
        const FrameOwner& frame = g_frame[i];
        for (uint8_t c = 0; c < edvr::plugin_cost::kApiClassCount; ++c) {
            window.apiCalls[c] += frame.apiCalls[c];
            if (frame.apiCalls[c] != 0) window.apiObserved = true;
        }
        window.apiSiteMask[0] |= frame.apiSiteMask[0];
        window.apiSiteMask[1] |= frame.apiSiteMask[1];
    }
}

void copyWindow(uint32_t profile, EdvrPluginCostWindowV1* out) noexcept {
    *out = {};
    out->version = edvr::plugin_cost::kWindowVersion;
    out->profileBit = profile;
    out->firstFrame = g_firstFrame;
    out->lastFrame = g_lastFrame;
    out->windowFrames = g_windowFrames;
    out->completedCpuSampleFrames = g_cpuSampleFrames;
    out->completedApiSampleFrames = g_apiSampleFrames;
    out->cpuTraceSuppressedFrames = g_cpuTraceSuppressedFrames;
    for (uint8_t i = 0; i < edvr::plugin_cost::kOwnerCount; ++i) {
        const WindowOwner& src = g_window[i];
        EdvrPluginCostOwnerV1& dst = out->owners[i];
        dst.owner = i;
        dst.cpuObserved = src.cpuObserved ? 1 : 0;
        dst.apiObserved = src.apiObserved ? 1 : 0;
        dst.cpuTimedScopes = src.timedScopes;
        dst.cpuReached = src.reached;
        dst.cpuInvoked = src.invoked;
        dst.cpuNotEligible = src.notEligible;
        dst.cpuSiteMask[0] = src.cpuSiteMask[0];
        dst.cpuSiteMask[1] = src.cpuSiteMask[1];
        dst.cpuMeanMs = src.meanMs;
        dst.cpuStdDevMs = g_cpuSampleFrames > 1
            ? std::sqrt(src.m2Ms / static_cast<double>(g_cpuSampleFrames - 1)) : 0.0;
        for (uint8_t c = 0; c < edvr::plugin_cost::kApiClassCount; ++c)
            dst.apiCalls[c] = src.apiCalls[c];
        dst.apiSiteMask[0] = src.apiSiteMask[0];
        dst.apiSiteMask[1] = src.apiSiteMask[1];
    }
}

} // namespace

extern "C" void edvrPluginCostConfigure(uint8_t profileBit, uint64_t qpcFrequency) noexcept {
    // Render-owner-thread API. Reconfiguration intentionally drops a possibly
    // partial frame and window; the first subsequent close is also discarded.
    const bool validProfile = profileBit == 0x1u || profileBit == 0x2u;
    g_profileBit = validProfile ? profileBit : 0;
    g_qpcFrequency = validProfile ? qpcFrequency : 0;
    g_configured = validProfile && qpcFrequency != 0;
    g_apiSampleFrame = false;
    g_traceSuppressed = false;
    g_skipNextBoundary = g_configured;
    clearFrame();
    clearWindow();
}

extern "C" void edvrPluginCostShutdown() noexcept {
    // Idempotent cold reset; never serializes a partial frame/window.
    g_profileBit = 0;
    g_qpcFrequency = 0;
    g_configured = false;
    g_skipNextBoundary = false;
    g_apiSampleFrame = false;
    g_traceSuppressed = false;
    g_ownerThreadToken.store(0, std::memory_order_release);
    g_ownerContext.store(nullptr, std::memory_order_release);
    clearFrame();
    clearWindow();
}

extern "C" void edvrPluginCostSetOwnerContext(void* context) noexcept {
    // Registration is cold and occurs after hook commit. Clear the published
    // owner first so no thread can use an old render-thread token with a new
    // context during a transfer or reinstall.
    g_ownerThreadToken.store(0, std::memory_order_release);
    g_ownerContext.store(context, std::memory_order_release);
}

extern "C" uint8_t edvrPluginCostApiSampleContext(const void* context) noexcept {
    if (!context || g_ownerContext.load(std::memory_order_acquire) != context) return 0;
    const uintptr_t owner = g_ownerThreadToken.load(std::memory_order_acquire);
    // These context/owner checks use atomics. A foreign callback with the same
    // context pointer but no owner TLS token returns before token allocation
    // or reading the non-atomic sample flag; only frame-boundary code allocates
    // TLS tokens.
    if (owner == 0 || g_threadToken == 0 || g_threadToken != owner) return 0;
    // Only the registered render-owner thread reaches this non-atomic frame
    // flag; deferred/foreign contexts return above by pointer or TLS token.
    return g_configured && g_apiSampleFrame ? 1u : 0u;
}

extern "C" void edvrPluginCostNoteSite(uint8_t owner, uint16_t siteId, uint8_t event) noexcept {
    if (!g_configured || !validOwnerSite(owner, siteId)) return;
    const auto value = static_cast<edvr::plugin_cost::SiteEvent>(event);
    if (value != edvr::plugin_cost::SiteEvent::Reached &&
        value != edvr::plugin_cost::SiteEvent::Invoked &&
        value != edvr::plugin_cost::SiteEvent::NotEligible) return;
    FrameOwner& frame = g_frame[owner];
    setBit(frame.cpuSiteMask, siteId);
    switch (value) {
    case edvr::plugin_cost::SiteEvent::Reached: ++frame.reached; break;
    case edvr::plugin_cost::SiteEvent::Invoked: ++frame.invoked; break;
    case edvr::plugin_cost::SiteEvent::NotEligible: ++frame.notEligible; break;
    default: break;
    }
}

extern "C" void edvrPluginCostNoteCpuTicks(uint8_t owner, uint16_t siteId, uint64_t ticks) noexcept {
    if (!g_configured || !validOwnerSite(owner, siteId)) return;
    FrameOwner& frame = g_frame[owner];
    frame.ticks += ticks;
    ++frame.timedScopes;
    setBit(frame.cpuSiteMask, siteId);
}

extern "C" void edvrPluginCostNoteD3dCall(uint8_t owner, uint16_t siteId,
                                           uint8_t apiClass) noexcept {
    if (!g_configured || !g_apiSampleFrame || !validOwnerSite(owner, siteId) ||
        apiClass >= edvr::plugin_cost::kApiClassCount) return;
    FrameOwner& frame = g_frame[owner];
    ++frame.apiCalls[apiClass];
    setBit(frame.apiSiteMask, siteId);
}

extern "C" void edvrPluginCostSetApiSampleFrame(uint8_t enabled) noexcept {
    g_apiSampleFrame = g_configured && enabled != 0;
}

extern "C" uint8_t edvrPluginCostApiSampleFrame(void) noexcept {
    return g_configured && g_apiSampleFrame ? 1u : 0u;
}

extern "C" void edvrPluginCostMarkTraceSuppressed() noexcept {
    // Called only from a sampled CPU-offset draw in an active replay frame.
    g_traceSuppressed = true;
}

extern "C" uint8_t edvrPluginCostFrameBoundary(uint32_t frameNo,
                                                uint8_t closedCpuSampleFrame,
                                                uint8_t closedApiSampleFrame,
                                                uint8_t nextApiSampleFrame,
                                                uint8_t traceSuppressed,
                                                EdvrPluginCostWindowV1* out) noexcept {
    if (!out) return false;
    if (!g_configured) {
        g_apiSampleFrame = false;
        return false;
    }
    const uintptr_t ownerToken = threadToken();

    // This existing owner-only frame boundary is the only place that
    // publishes/reaffirms the current render thread. A context registration
    // alone cannot make API notes eligible.
    const uintptr_t previousOwner = g_ownerThreadToken.load(std::memory_order_relaxed);
    if (previousOwner != ownerToken) {
        const uintptr_t publishedOwner = g_ownerContext.load(std::memory_order_acquire)
            ? ownerToken : 0;
        if (previousOwner != publishedOwner)
            g_ownerThreadToken.store(publishedOwner, std::memory_order_release);
    }

    const bool suppressed = traceSuppressed != 0 || g_traceSuppressed;
    if (g_skipNextBoundary) {
        // A configure/reload may have happened in the middle of this frame.
        g_skipNextBoundary = false;
        clearFrame();
        g_traceSuppressed = false;
        g_apiSampleFrame = nextApiSampleFrame != 0;
        return false;
    }

    if (g_windowFrames == 0) g_firstFrame = frameNo;
    g_lastFrame = frameNo;
    ++g_windowFrames;

    if (closedCpuSampleFrame) {
        if (suppressed) {
            ++g_cpuTraceSuppressedFrames;
        } else {
            addCpuSample();
        }
    }
    if (closedApiSampleFrame) addApiSample();

    clearFrame();
    g_traceSuppressed = false;
    // Notes after this boundary belong to the next frame and use its flag.
    g_apiSampleFrame = nextApiSampleFrame != 0;

    if (g_windowFrames < edvr::plugin_cost::kWindowFrameCount) return false;
    copyWindow(g_profileBit, out);
    clearWindow();
    return true;
}
