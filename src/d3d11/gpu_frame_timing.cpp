#include "gpu_frame_timing.h"
#include "gpu_timing.h"
#include "../common/gpu_frame_protocol.h"
#include "../common/log.h"
#include "../common/guard.h"
#include <atomic>
#include <new>

namespace edvr {
namespace {
// Low bit means a successful pose wait has armed this sequence. A wait-start
// publishes a disarmed sequence immediately, on any thread, without touching
// the context. The graphics DLL allocates IDs across compositor reinitialization.
std::atomic<uint64_t> g_token{0}, g_nextSequence{0}, g_poisoned{0};
thread_local bool g_internal = false;
struct Internal {
    bool previous = g_internal;
    Internal() noexcept { g_internal = true; }
    ~Internal() { g_internal = previous; }
};
SRWLOCK g_snapshotLock = SRWLOCK_INIT;
GpuFrameSnapshot g_snapshot;

GpuSpanOwner bindOwner(GpuTimingFrameDriver& driver, ID3D11Device* d,
                       ID3D11DeviceContext* c) noexcept {
    return driver.bind(d, c) ? driver.currentOwner() : GpuSpanOwner{};
}
struct Controller {
    GpuTimingFrameDriver driver;
    const GpuSpanOwner owner;
    GpuSpanState ring;
    std::atomic<bool> enabled{false};
    uint64_t seenToken = 0, activeSequence = 0, startedAt = 0;
    std::atomic<uint64_t> sourceFrame{1};
    unsigned ended = 0, commandBudget = 0;
    int activeEye = -1;
    uint64_t validCount = 0, invalidCount = 0, lastReport = 0;
    Controller(ID3D11Device* d, ID3D11DeviceContext* c) noexcept
        : owner(bindOwner(driver, d, c)), ring(driver, owner) {}
    ID3D11DeviceContext* context() const noexcept {
        return reinterpret_cast<ID3D11DeviceContext*>(owner.context);
    }
    bool owns(ID3D11DeviceContext* c) const noexcept {
        return owner.immediate && owner.thread == GetCurrentThreadId() &&
               owner.context && c == context();
    }
    void publish(GpuSpanResult result, uint64_t now) noexcept {
        // A duplicate/foreign-thread submit can reject a pair after its outer
        // marker closed but before readback. Discard affected older pending
        // samples conservatively; a late valid result must not undo rejection.
        if (result.reason == GpuSpanReason::Valid &&
            result.sequence <= g_poisoned.load(std::memory_order_acquire)) {
            result.reason = GpuSpanReason::Incomplete;
            result.outerMs = result.leftMs = result.rightMs = 0;
        }
        if (result.reason == GpuSpanReason::Valid) ++validCount;
        else ++invalidCount;
        AcquireSRWLockExclusive(&g_snapshotLock);
        // Older slots may settle after newer ones. Keep their logged identity,
        // but never let a late result replace a newer frame in the readout.
        if (!g_snapshot.haveResult || result.sequence >= g_snapshot.result.sequence) {
            g_snapshot.result = result;
            g_snapshot.haveResult = true;
            g_snapshot.capturedAtMs = now;
        }
        ReleaseSRWLockExclusive(&g_snapshotLock);
        if (validCount + invalidCount == 1 ||
            (validCount == 1 && result.reason == GpuSpanReason::Valid) ||
            !lastReport || now - lastReport >= 5000) {
            lastReport = now;
            Log::get().note("Render-to-submit GPU: seq %llu frame %llu %s, outer %.4f ms, "
                "submit paths L %.4f R %.4f ms (include runtime Submit), age %llu ms; "
                "valid %llu invalid %llu; context %p thread %lu.",
                result.sequence, result.sourceFrame, gpuFrameReason(result.reason),
                result.outerMs, result.leftMs, result.rightMs, result.ageMs,
                validCount, invalidCount, static_cast<void*>(context()), GetCurrentThreadId());
        }
    }
    void immediate(GpuSpanReason reason, uint64_t sequence, uint64_t now) noexcept {
        GpuSpanResult result{};
        result.reason = reason;
        result.sequence = sequence;
        result.sourceFrame = sourceFrame;
        result.completedAtMs = now;
        publish(result, now);
    }
    void poll(uint64_t now) noexcept {
        GpuSpanState::Results results{};
        const unsigned n = ring.poll(now, driver.currentOwner(), results);
        for (unsigned i = 0; i < n; ++i) publish(results[i], now);
    }
    void invalidate(uint64_t now) noexcept {
        if (activeSequence) ring.invalidateFrame(now, driver.currentOwner());
        activeSequence = 0;
        activeEye = -1;
        ended = 0;
    }
    void sync(uint64_t token, uint64_t now) noexcept {
        if (seenToken != token || (activeSequence &&
            (g_poisoned.load(std::memory_order_acquire) >= activeSequence ||
             now - startedAt > GpuSpanState::kMaxAgeMs))) {
            invalidate(now);
        }
        poll(now);
    }
};
// This pointer is detached only during quiescent cleanup, before device owners
// release their timers. Process termination skips the cleanup entirely.
std::atomic<Controller*> g_controller{nullptr};
void poison(uint64_t sequence) noexcept {
    auto old = g_poisoned.load(std::memory_order_relaxed);
    while (sequence > old && !g_poisoned.compare_exchange_weak(old, sequence,
            std::memory_order_release, std::memory_order_relaxed)) {}
}
} // namespace

const char* gpuFrameReason(GpuSpanReason reason) noexcept {
    switch (reason) {
    case GpuSpanReason::Valid: return "valid";
    case GpuSpanReason::WrongOwner: return "wrong owner";
    case GpuSpanReason::Stopped: return "stopped";
    case GpuSpanReason::OldSequence: return "old sequence";
    case GpuSpanReason::NoOpenFrame: return "no covered render command";
    case GpuSpanReason::Incomplete: return "incomplete pair";
    case GpuSpanReason::BadPair: return "invalid eye pair";
    case GpuSpanReason::Disjoint: return "disjoint";
    case GpuSpanReason::ZeroFrequency: return "zero frequency";
    case GpuSpanReason::BadTimestamps: return "invalid timestamps";
    case GpuSpanReason::Stale: return "expired";
    case GpuSpanReason::RingFull: return "ring full";
    case GpuSpanReason::CreateFailed: return "query allocation failed";
    default: return "query failure";
    }
}
bool gpuFrameInternal() noexcept { return g_internal; }
bool gpuFrameBind(ID3D11Device* d, ID3D11DeviceContext* c, bool enabled) noexcept {
    auto* current = g_controller.load(std::memory_order_acquire);
    if (current) return current->owns(c) && current->owner.device == reinterpret_cast<uintptr_t>(d);
    Internal internal;
    auto* candidate = new (std::nothrow) Controller(d, c);
    if (!candidate) return false;
    if (!candidate->owner.context || !candidate->owner.immediate) {
        candidate->driver.reset();
        delete candidate;
        return false;
    }
    if (!g_controller.compare_exchange_strong(current, candidate)) {
        candidate->driver.reset(c);
        delete candidate;
        return false;
    }
    gpuFrameConfigure(enabled);
    if (!enabled) Log::get().note("Render-to-submit GPU: disabled (advanced.app_gpu_timing).");
    Log::get().note("Render-to-submit GPU: owner bound, first covered immediate command after "
        "pose wait through second accepted Submit plus EDVR follow-up copies. "
        "One shared frequency scope; mirror/post-marker commands excluded.");
    return true;
}
void gpuFrameConfigure(bool enabled) noexcept {
    auto* c = g_controller.load(std::memory_order_acquire);
    if (!c || !c->owns(c->context())) return;
    const bool previous = c->enabled.exchange(enabled, std::memory_order_acq_rel);
    if (previous == enabled) return;
    Internal internal;
    const uint64_t now = GetTickCount64();
    c->invalidate(now);
    if (!enabled) {
        // Drop the local ring on its verified owner so disabled monitoring
        // does not keep polling, retain leases, or resurrect old results when
        // enabled again. Driver stop-on-uncertain-End remains permanent.
        c->ring.shutdown(now, c->driver.currentOwner());
        c->ring.~GpuSpanState();
        new (&c->ring) GpuSpanState(c->driver, c->owner);
    }
    c->seenToken = g_token.load(std::memory_order_acquire); // Wait for a new boundary.
    AcquireSRWLockExclusive(&g_snapshotLock);
    g_snapshot = {};
    g_snapshot.enabled = enabled;
    ReleaseSRWLockExclusive(&g_snapshotLock);
    Log::get().note("Render-to-submit GPU: %s (advanced.app_gpu_timing); independent of compositor timing.",
                    enabled ? "enabled" : "disabled");
}
void gpuFrameCommand(ID3D11DeviceContext* ctx) noexcept {
    if (g_internal) return;
    auto* c = g_controller.load(std::memory_order_acquire);
    if (!c || !c->enabled.load(std::memory_order_relaxed) || ctx != c->context()) return;
    const uint64_t token = g_token.load(std::memory_order_acquire);
    if (!c->owns(ctx)) { poison(token >> 1); return; }
    // Most commands pay only this owner/token check. An unusually long frame
    // without a Present or new pose wait still has a bounded open scope.
    if (c->seenToken == token) {
        if (!c->activeSequence || (++c->commandBudget & 1023u)) return;
    }
    Internal internal;
    const uint64_t now = GetTickCount64();
    c->sync(token, now);
    if (c->seenToken == token) return;
    c->seenToken = token; // Set before issuing query commands through hooked End.
    const uint64_t sequence = token >> 1;
    if (!(token & 1) || !sequence || g_poisoned.load(std::memory_order_acquire) >= sequence) return;
    const auto reason = c->ring.beginFrame(sequence, c->sourceFrame, now, c->driver.currentOwner());
    if (reason == GpuSpanReason::Valid) {
        c->activeSequence = sequence;
        c->startedAt = now;
    } else c->immediate(reason, sequence, now);
}
void gpuFramePresent(ID3D11DeviceContext* ctx, uint64_t nextSourceFrame) noexcept {
    auto* c = g_controller.load(std::memory_order_acquire);
    if (!c || ctx != c->context()) return;
    if (!c->owns(ctx)) {
        // Present may be observed on another thread. It can publish CPU frame
        // identity, but cannot close/poll the immediate context. Reject the
        // affected pair and let its owner consume that rejection later.
        poison(g_token.load(std::memory_order_acquire) >> 1);
        c->sourceFrame.store(nextSourceFrame, std::memory_order_release);
        return;
    }
    Internal internal;
    const uint64_t now = GetTickCount64();
    c->invalidate(now); // Missing eye / bypassed submit cannot span a Present.
    c->seenToken = g_token.load(std::memory_order_acquire);
    c->poll(now);
    c->sourceFrame = nextSourceFrame;
}
GpuFrameSnapshot gpuFrameSnapshot() noexcept {
    AcquireSRWLockShared(&g_snapshotLock);
    auto result = g_snapshot;
    ReleaseSRWLockShared(&g_snapshotLock);
    if (result.haveResult && result.result.reason == GpuSpanReason::Valid &&
        result.result.sequence <= g_poisoned.load(std::memory_order_acquire)) {
        result.result.reason = GpuSpanReason::Incomplete;
        result.result.outerMs = result.result.leftMs = result.result.rightMs = 0;
    }
    return result;
}
void gpuFrameAbandon() noexcept {
    auto* c = g_controller.exchange(nullptr, std::memory_order_acq_rel);
    if (c) { c->driver.reset(); delete c; }
    AcquireSRWLockExclusive(&g_snapshotLock);
    g_snapshot = {};
    ReleaseSRWLockExclusive(&g_snapshotLock);
    g_token.store(0, std::memory_order_release);
}

uint64_t gpuFrameEvent(unsigned protocol, unsigned eventValue, uint64_t sequence,
                       unsigned eye, unsigned flags, void* texture) noexcept {
    if (protocol != kGpuFrameProtocol) return 0;
    const auto event = static_cast<GpuFrameEvent>(eventValue);
    if (event == GpuFrameEvent::WaitBegin) {
        const auto seq = g_nextSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!seq || seq > UINT64_MAX / 2) return 0;
        // Overlapping waits may finish/start out of order; never roll back ID.
        auto old = g_token.load(std::memory_order_relaxed);
        while ((old >> 1) < seq && !g_token.compare_exchange_weak(old, seq << 1,
                std::memory_order_release, std::memory_order_relaxed)) {}
        return seq;
    }
    if (!sequence || sequence > UINT64_MAX / 2) return 0;
    if (event == GpuFrameEvent::WaitEnd) {
        if (flags != 1) { poison(sequence); return 0; }
        auto expected = sequence << 1;
        return g_token.compare_exchange_strong(expected, expected | 1,
            std::memory_order_release, std::memory_order_relaxed) ? 1 : 0;
    }
    if (event == GpuFrameEvent::Cancel) { poison(sequence); return 1; }
    if (event != GpuFrameEvent::SubmitBegin && event != GpuFrameEvent::SubmitEnd) return 0;
    auto* c = g_controller.load(std::memory_order_acquire);
    if (!c || !c->enabled.load(std::memory_order_acquire)) return 0;
    if (!c->owns(c->context())) { poison(sequence); return 0; }
    Internal internal;
    const uint64_t now = GetTickCount64();
    const uint64_t token = g_token.load(std::memory_order_acquire);
    c->sync(token, now);
    if (token != ((sequence << 1) | 1) || g_poisoned.load(std::memory_order_acquire) >= sequence) return 0;
    if (event == GpuFrameEvent::SubmitBegin) {
        // Never start at the submit itself and pretend it included rendering.
        if (!c->activeSequence) {
            c->seenToken = token;
            c->immediate(GpuSpanReason::NoOpenFrame, sequence, now);
            poison(sequence);
            return 0;
        }
        ID3D11Device* device = nullptr;
        if (texture) guarded("GPU frame texture device", [&] {
            static_cast<ID3D11Texture2D*>(texture)->GetDevice(&device);
        });
        const bool sameDevice = device && reinterpret_cast<uintptr_t>(device) == c->owner.device;
        if (device) device->Release();
        if (!sameDevice || eye > 1) { c->invalidate(now); poison(sequence); return 0; }
        const auto reason = c->ring.beginEye(eye, now, c->driver.currentOwner());
        if (reason != GpuSpanReason::Valid) { c->invalidate(now); poison(sequence); return 0; }
        c->activeEye = static_cast<int>(eye);
        return 1;
    }
    if (c->activeSequence != sequence || c->activeEye != static_cast<int>(eye) || flags != 1) {
        c->invalidate(now); poison(sequence); return 0;
    }
    if (c->ring.endEye(eye, now, c->driver.currentOwner()) != GpuSpanReason::Valid) {
        c->invalidate(now); poison(sequence); return 0;
    }
    c->activeEye = -1;
    c->ended |= 1u << eye;
    if (c->ended == 3) {
        c->ring.finishFrame(now, c->driver.currentOwner());
        c->activeSequence = 0;
        c->ended = 0;
    }
    return 1;
}
} // namespace edvr

extern "C" __declspec(dllexport) uint64_t WINAPI edvrGpuFrameEvent(
    unsigned protocol, unsigned event, uint64_t sequence, unsigned eye,
    unsigned flags, void* texture) {
    return edvr::gpuFrameEvent(protocol, event, sequence, eye, flags, texture);
}
