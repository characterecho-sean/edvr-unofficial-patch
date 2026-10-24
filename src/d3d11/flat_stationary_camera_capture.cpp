#include "flat_origin_capture.h"
#include "flat_stationary_camera_capture.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <new>

#include "../common/code_hook.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "flat_stationary_camera_core.h"
#include "journal_watch.h"

namespace edvr { namespace {

// Temporary F1 source-topology capture for the verified Steam executable.
// 2874290 is a normal three-argument, void Win64 function. The hook only
// observes its source and stack-local helper, and calls the original once.
constexpr uintptr_t kRendererRva = 0x2874290;
constexpr uint8_t kPrologue[15] = {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20};
constexpr uint32_t kPeTimestamp = 1792704584u;
constexpr uint32_t kPeImageSize = 104906752u;
constexpr unsigned kSourceSlots = 16, kRefreshSlots = 32;
constexpr unsigned kReportRows = 1024;
constexpr uint64_t kCaptureFrames = 120;
using Renderer = void (*)(uintptr_t, uintptr_t, uintptr_t);

bool sehCopy(void*, uintptr_t at, void* out, size_t size) noexcept {
    __try { std::memcpy(out, reinterpret_cast<const void*>(at), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool peMatches(uintptr_t base) noexcept {
    __try {
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096) return false;
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp == kPeTimestamp &&
            nt->OptionalHeader.SizeOfImage == kPeImageSize;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct SourceSlot {
    std::atomic<uintptr_t> key{0};
    std::atomic<bool> ready{false};
    stationcam::Snapshot snapshot;
};
struct RefreshSlot {
    std::atomic<uintptr_t> key{0};
    std::atomic<bool> ready{false};
    uintptr_t camera = 0, ctx = 0;
    uint64_t callNo = 0, callerRva = 0;
    uint32_t kind = 0;
    float origin[4]{};
    bool originRead = false;
};
enum class Phase : uint8_t { Waiting, Capturing, Done, Refused };
struct State {
    CodeHook hook;
    uint8_t* relay = nullptr; // held for process lifetime after install
    std::atomic<uintptr_t> gate{0};
    std::atomic<uintptr_t> forward{0};
    std::atomic<uintptr_t> gameBase{0};
    std::atomic<bool> profileDropped{false};
    std::atomic<uint32_t> inflight{0}, sourceOverflow{0}, refreshOverflow{0};
    std::atomic<uint64_t> captures{0}, refreshes{0};
    std::atomic<uint64_t> rendererCalls{0};
    SourceSlot sources[kSourceSlots];
    RefreshSlot refresh[kRefreshSlots];
    Phase phase = Phase::Waiting; // Present thread only
    uint64_t armFrame = 0, closingFrame = 0, witnessFrame = 0, lastWitnessFrame = 0, lastRejectedFrame = 0;
    uint64_t selectedFrame = 0, lastSelectedFrame = 0;
    uint64_t lastNoteMs = 0;
    stationcam::SceneFrameGate scene;
    float selectedOrigin[3]{};
    float witnessOrigin[3]{};
    const char* waitReason = "no passive scene witness";
    const char* sceneRefusal = "no-trigger-or-camera-witness";
    const char* captureRefusal = nullptr;
    bool selectedReset = true;
};
std::atomic<State*> g_state{nullptr};
State* stateOrNull() noexcept { return g_state.load(std::memory_order_acquire); }
State* ensureState() noexcept {
    if (!flatOriginCaptureEnabled()) return nullptr;
    State* p = stateOrNull();
    if (!p) {
        p = new (std::nothrow) State;
        if (p) g_state.store(p, std::memory_order_release); // Present owns initialization
    }
    return p;
}

uint8_t* allocateRelay(uintptr_t target) noexcept {
    constexpr uintptr_t granularity = 64 * 1024, span = 0x7FFF0000ull;
    const uintptr_t begin = (target - span) & ~(granularity - 1);
    for (uintptr_t candidate = target; candidate >= begin; candidate -= granularity) {
        auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096,
                                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (p) return p;
    }
    return nullptr;
}
void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    // Gate-first relay; the callback is a normal function entry, unlike the
    // mid-function refresh detour. Both branches jump, never call.
    const uint8_t body[44] = {
        0x48,0xB8,0,0,0,0,0,0,0,0, 0x48,0x83,0x38,0,
        0x74,0x0E, 0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0,
        0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &gateAddress, 8);
    std::memcpy(code + 22, &callbackAddress, 8);
}
bool prepareRelay(void* trampoline, void* opaque) noexcept {
    State& s = *static_cast<State*>(opaque);
    const uintptr_t original = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(s.relay + 36, &original, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(s.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), s.relay, 44)) return false;
    s.forward.store(original, std::memory_order_release);
    return true;
}

void observeSource(uintptr_t owner, uintptr_t source, uintptr_t helper, uintptr_t caller,
                   const float before[4], bool beforeRead, uint64_t epoch) noexcept {
    State& s = *stateOrNull(); // install is Present-owned, after publication
    const uintptr_t base = s.gameBase.load(std::memory_order_acquire);
    const uintptr_t key = source ? source : 1;
    SourceSlot* chosen = nullptr;
    for (auto& slot : s.sources) {
        uintptr_t expected = 0;
        if (slot.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
            chosen = &slot; break;
        }
        if (expected == key) return; // one bounded snapshot per renderer source
    }
    if (!chosen) { s.sourceOverflow.fetch_add(1, std::memory_order_relaxed); return; }
    auto& out = chosen->snapshot;
    out.owner = owner; out.source = source; out.helper = helper;
    out.callerRva = stationcam::rva(caller, base);
    out.sourceBeforeRead = beforeRead;
    if (beforeRead) std::memcpy(out.sourceBefore, before, sizeof(out.sourceBefore));
    out.sourceAfterRead = sehCopy(nullptr, source + 0x70, out.sourceAfter, sizeof(out.sourceAfter));
    out.helperAfterRead = sehCopy(nullptr, helper + 0x30, out.helperAfter, sizeof(out.helperAfter));
    if (!out.sourceAfterRead || !out.helperAfterRead) stationcam::fail(out, "source-or-helper-read");
    if (beforeRead && out.sourceAfterRead && std::memcmp(out.sourceBefore, out.sourceAfter, sizeof(out.sourceBefore)) != 0) {
        out.consistent = false; stationcam::fail(out, "source-changed-during-copy");
    }
    if (out.sourceAfterRead && out.helperAfterRead &&
        std::memcmp(out.sourceAfter, out.helperAfter, sizeof(out.sourceAfter)) != 0) {
        out.consistent = false; stationcam::fail(out, "helper-copy-mismatch");
    }
    stationcam::capture(&sehCopy, nullptr, base, out);
    if (s.armFrame != epoch) { out.consistent = false; stationcam::fail(out, "frame-overlap"); }
    chosen->ready.store(true, std::memory_order_release);
    s.captures.fetch_add(1, std::memory_order_relaxed);
}

void rendererDetour(uintptr_t owner, uintptr_t source, uintptr_t helper) {
    State& s = *stateOrNull(); // this callback exists only after install
    s.rendererCalls.fetch_add(1, std::memory_order_relaxed);
    const auto original = reinterpret_cast<Renderer>(s.forward.load(std::memory_order_acquire));
    if (!runtimeFlatProfile()) {
        s.profileDropped.store(true, std::memory_order_release);
        s.gate.store(0, std::memory_order_release);
        original(owner, source, helper);
        return;
    }
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uint64_t epoch = s.armFrame;
    s.inflight.fetch_add(1, std::memory_order_acq_rel);
    float before[4]{};
    const bool beforeRead = source && sehCopy(nullptr, source + 0x70, before, sizeof(before));
    original(owner, source, helper);
    if (runtimeFlatProfile()) observeSource(owner, source, helper, caller, before, beforeRead, epoch);
    else {
        s.profileDropped.store(true, std::memory_order_release);
        s.gate.store(0, std::memory_order_release);
    }
    s.inflight.fetch_sub(1, std::memory_order_acq_rel);
}

bool install(State& s) noexcept {
    const HMODULE game = GetModuleHandleW(L"EliteDangerous64.exe");
    if (!game) { s.waitReason = "game-module-unavailable"; return false; }
    const uintptr_t base = reinterpret_cast<uintptr_t>(game);
    uint8_t found[sizeof(kPrologue)]{};
    if (!peMatches(base) || !sehCopy(nullptr, base + kRendererRva, found, sizeof(found)) ||
        std::memcmp(found, kPrologue, sizeof(found)) != 0) {
        s.waitReason = "verified-exe-or-prologue-mismatch"; return false;
    }
    s.gameBase.store(base, std::memory_order_release);
    s.relay = allocateRelay(base + kRendererRva);
    if (!s.relay) { s.waitReason = "relay-allocation-failed"; return false; }
    buildRelay(s.relay, &s.gate, reinterpret_cast<void*>(&rendererDetour));
    if (!s.hook.install(reinterpret_cast<void*>(base + kRendererRva), s.relay, nullptr,
                        "flat-stationary-camera", &prepareRelay, &s)) {
        VirtualFree(s.relay, 0, MEM_RELEASE);
        s.relay = nullptr;
        s.waitReason = "CodeHook-refused"; return false;
    }
    Log::get().note("flat stationary camera: passive renderer hook installed at +0x%llX; no game getters called or camera writes made",
                    static_cast<unsigned long long>(kRendererRva));
    return true;
}

void logPlacedMatrix(unsigned source, unsigned stage, unsigned activity, const char* kind,
                     const float p[16], bool applicable, bool read) noexcept {
    if (!applicable) {
        Log::get().note("flat stationary Placed-matrix[%u,%u,%u] %s N/A (record-null)",
            source,stage,activity,kind);
        return;
    }
    Log::get().note("flat stationary Placed-matrix[%u,%u,%u] %s read=%u rows=(%.9g %.9g %.9g %.9g | %.9g %.9g %.9g %.9g | %.9g %.9g %.9g %.9g | %.9g %.9g %.9g %.9g)",
        source,stage,activity,kind,read?1u:0u,
        p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],
        p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
}

void report(State& s, uint64_t frame) noexcept {
    unsigned sourceCount = 0, sourceValid = 0, sourceComplete = 0, refreshCount = 0, mainView = 0;
    unsigned mainPlaced = 0, mainPlacedIdentity = 0, mainPlacedComplete = 0;
    unsigned mainPlacedProviders = 0, mainPlacedOverflow = 0;
    unsigned detailRows = 0;
    for (auto& slot : s.sources) if (slot.ready.load(std::memory_order_acquire)) {
        ++sourceCount;
        if (slot.snapshot.validSource) ++sourceValid;
        if (slot.snapshot.complete && slot.snapshot.consistent) ++sourceComplete;
        if (slot.snapshot.complete && slot.snapshot.consistent && slot.snapshot.stageCount &&
            slot.snapshot.stages[0].stackVtable == s.gameBase.load() + 0x5189370 &&
            slot.snapshot.stages[0].nameRead &&
            std::strcmp(slot.snapshot.stages[0].stackName, "MainView") == 0 &&
            slot.snapshot.callerRva == 0x281D259) ++mainView;
        if (slot.snapshot.stageCount && slot.snapshot.stages[0].nameRead &&
            std::strcmp(slot.snapshot.stages[0].stackName, "MainView") == 0) {
            mainPlaced += slot.snapshot.placedSeen;
            mainPlacedOverflow += slot.snapshot.placedOverflow ? 1u : 0u;
            for (unsigned j = 0; j < slot.snapshot.placedCount; ++j) {
                const auto& d = slot.snapshot.placed[j];
                if (d.identityComplete) ++mainPlacedIdentity;
                if (d.identityComplete && (!d.record || (d.localRead && d.cacheRead)) &&
                    d.finalRead && d.poseRereadComplete) ++mainPlacedComplete;
                if (d.identityComplete && d.providerPresent) ++mainPlacedProviders;
            }
            detailRows += slot.snapshot.stageCount;
            for (unsigned j = 0; j < slot.snapshot.stageCount; ++j)
                detailRows += static_cast<unsigned>(slot.snapshot.stages[j].activityCount > stationcam::kActivities ?
                    stationcam::kActivities : slot.snapshot.stages[j].activityCount);
            detailRows += 5 * slot.snapshot.placedCount;
        }
    }
    for (auto& slot : s.refresh) if (slot.ready.load(std::memory_order_acquire)) ++refreshCount;
    detailRows += refreshCount;
    const bool reportBound = detailRows > kReportRows;
    const bool complete = s.witnessFrame >= s.armFrame && !s.captureRefusal &&
        !s.profileDropped.load(std::memory_order_acquire) && sourceCount && sourceComplete && mainView == 1 && refreshCount &&
        mainPlaced && mainPlacedIdentity == mainPlaced && !mainPlacedOverflow &&
        !s.sourceOverflow.load(std::memory_order_acquire) && !s.refreshOverflow.load(std::memory_order_acquire) && !reportBound;
    const char* refusal = s.captureRefusal ? s.captureRefusal : !s.witnessFrame ? "passive-scene-never-qualified" :
        !sourceCount ? "renderer-hook-never-ran" : !refreshCount ? "refresh-hook-never-ran" :
        s.sourceOverflow.load() ? "source-bound" : s.refreshOverflow.load() ? "refresh-bound" :
        reportBound ? "report-bound" : mainView == 0 ? "MainView-root-not-observed" :
        mainView > 1 ? "multiple-MainView-roots" : sourceComplete == 0 ? "topology-incomplete" :
        !mainPlaced ? "Placed-not-observed" : mainPlacedOverflow ? "Placed-bound" :
        mainPlacedIdentity != mainPlaced ? "Placed-identity-incomplete" : "none";
    Log::get().note("flat stationary camera: %s frame=%llu arm=%llu passive-scene=%llu selected=%llu selected-requested-reset-known=%u selected-requested-reset=%u refusal=%s scene-refusal=%s profile-dropped=%u renderer-calls=%llu captured-sources=%llu unique=%u validated=%u complete=%u MainView-root=%u Placed=%u identity-complete=%u detail-complete=%u provider-present=%u placed-overflow=%u refresh-calls=%llu unique=%u source-overflow=%u refresh-overflow=%u report-rows=%u/%u passive-row275=(%.9g %.9g %.9g) selected-row275=(%.9g %.9g %.9g); renderer-source topology and Placed identity only, selected GPU donor UNPROVED",
        complete ? "captured" : "refused/incomplete", static_cast<unsigned long long>(frame),
        static_cast<unsigned long long>(s.armFrame), static_cast<unsigned long long>(s.witnessFrame), static_cast<unsigned long long>(s.selectedFrame),
        s.selectedFrame?1u:0u,s.selectedReset?1u:0u,refusal,s.sceneRefusal,s.profileDropped.load()?1u:0u,
        static_cast<unsigned long long>(s.rendererCalls.load()),
        static_cast<unsigned long long>(s.captures.load()), sourceCount, sourceValid, sourceComplete, mainView,
        mainPlaced,mainPlacedIdentity,mainPlacedComplete,mainPlacedProviders,mainPlacedOverflow,
        static_cast<unsigned long long>(s.refreshes.load()), refreshCount,
        s.sourceOverflow.load(), s.refreshOverflow.load(),detailRows,kReportRows,
        s.witnessOrigin[0], s.witnessOrigin[1], s.witnessOrigin[2],
        s.selectedOrigin[0], s.selectedOrigin[1], s.selectedOrigin[2]);
    unsigned emitted = 0;
    for (unsigned i = 0; i < kSourceSlots; ++i) {
        auto& slot = s.sources[i];
        if (!slot.ready.load(std::memory_order_acquire)) continue;
        const auto& q = slot.snapshot;
        unsigned unknownWeights = 0, unknownStates = 0;
        for (unsigned j = 0; j < q.stageCount; ++j)
            for (unsigned k = 0; k < q.stages[j].activityCount && k < stationcam::kActivities; ++k) {
                const auto& a = q.stages[j].activities[k];
                if (!a.weightKnown) ++unknownWeights;
                if (!a.stateKnown) ++unknownStates;
            }
        Log::get().note("flat stationary source[%u]: caller=+0x%llX owner=%p S=%p helper=%p C=%p source-valid=%u consistent=%u topology-complete=%u unknown-weight=%u unknown-state=%u reason=%s S-before=(%.7g %.7g %.7g) S-after=(%.7g %.7g %.7g) helper-after=(%.7g %.7g %.7g); helper is stack-local",
            i, static_cast<unsigned long long>(q.callerRva), reinterpret_cast<void*>(q.owner),
            reinterpret_cast<void*>(q.source), reinterpret_cast<void*>(q.helper), reinterpret_cast<void*>(q.rootChild),
            q.validSource ? 1u : 0u, q.consistent ? 1u : 0u, q.complete ? 1u : 0u,
            unknownWeights,unknownStates,q.failure,
            q.sourceBefore[0], q.sourceBefore[1], q.sourceBefore[2],
            q.sourceAfter[0], q.sourceAfter[1], q.sourceAfter[2],
            q.helperAfter[0], q.helperAfter[1], q.helperAfter[2]);
        if (!q.stageCount || !q.stages[0].nameRead || std::strcmp(q.stages[0].stackName,"MainView") != 0) continue;
        for (unsigned j = 0; j < q.stageCount; ++j) {
            if (emitted == kReportRows) break;
            ++emitted;
            const auto& z = q.stages[j];
            Log::get().note("flat stationary stage[%u,%u]: edge=%c parent=%d C=%p B=%p B-vt=+0x%llX name=%s base=%p nested-parent=%p mode=%u activities=%llu inactive-nested-omitted=%u pose-changed=%u output=(%.7g %.7g %.7g) inherited=(%.7g %.7g %.7g) output-axis0=(%.7g %.7g %.7g) inherited-axis0=(%.7g %.7g %.7g) valid=%u/%u",
                i,j,z.edge,z.parent,reinterpret_cast<void*>(z.child),reinterpret_cast<void*>(z.stack),
                static_cast<unsigned long long>(stationcam::rva(z.stackVtable,s.gameBase.load())),
                z.nameRead ? z.stackName : "UNKNOWN", reinterpret_cast<void*>(z.predecessor),
                reinterpret_cast<void*>(z.nestedParent),z.mode,static_cast<unsigned long long>(z.activityCount),z.inactiveNestedOmitted,
                z.outputRead && z.baseRead && std::memcmp(z.outputPose,z.basePose,sizeof(z.outputPose)) != 0 ? 1u : 0u,
                z.outputPose[12],z.outputPose[13],z.outputPose[14],
                z.basePose[12],z.basePose[13],z.basePose[14],
                z.outputPose[0],z.outputPose[1],z.outputPose[2],
                z.basePose[0],z.basePose[1],z.basePose[2],z.outputRead?1u:0u,z.baseRead?1u:0u);
            for (unsigned k = 0; k < z.activityCount && k < stationcam::kActivities; ++k) {
                if (emitted == kReportRows) break;
                ++emitted;
                const auto& a = z.activities[k];
                Log::get().note("flat stationary activity[%u,%u,%u]: iface=%p vt=+0x%llX concrete-vt=+0x%llX weight-fn=+0x%llX weight=%s%.7g state-fn=+0x%llX state=%p pose=%s(%.7g %.7g %.7g) name=%s",
                    i,j,k,reinterpret_cast<void*>(a.iface),
                    static_cast<unsigned long long>(stationcam::rva(a.vtable,s.gameBase.load())),
                    static_cast<unsigned long long>(stationcam::rva(a.concreteVtable,s.gameBase.load())),
                    static_cast<unsigned long long>(stationcam::rva(a.weightFn,s.gameBase.load())),
                    a.weightKnown ? "" : "UNKNOWN:",a.weight,
                    static_cast<unsigned long long>(stationcam::rva(a.stateFn,s.gameBase.load())),
                    reinterpret_cast<void*>(a.state),a.poseRead?"":"UNKNOWN:",
                    a.pose[12],a.pose[13],a.pose[14],a.nameRead?a.name:"UNKNOWN");
            }
        }
        for (unsigned j = 0; j < q.placedCount; ++j) {
            if (emitted + 5 > kReportRows) break;
            emitted += 5;
            const auto& d = q.placed[j];
            Log::get().note("flat stationary Placed[%u,%u,%u]: A=%p iface=%p E=%s%p P=%s%p P-vt=%s%p/+0x%llX P-slot20=%s%p/+0x%llX identity-complete=%u provider-present=%s%u provider-known=%s%u reason=%s target-ref=%s%p state=%s%d valid5=%s%u valid8=%s%u; P field is sampled parent-provider slot, parent getter called UNKNOWN, cache refreshed UNKNOWN",
                i,d.stage,d.activity,reinterpret_cast<void*>(d.object),reinterpret_cast<void*>(d.iface),
                d.recordRead?"":"UNKNOWN:",reinterpret_cast<void*>(d.record),
                d.identityComplete?"":"UNVALIDATED:",reinterpret_cast<void*>(d.provider),
                d.identityComplete?"":"UNVALIDATED:",reinterpret_cast<void*>(d.providerVtable),
                static_cast<unsigned long long>(stationcam::rva(d.providerVtable,s.gameBase.load())),
                d.identityComplete?"":"UNVALIDATED:",reinterpret_cast<void*>(d.providerFn),
                static_cast<unsigned long long>(stationcam::rva(d.providerFn,s.gameBase.load())),
                d.identityComplete?1u:0u,
                d.identityComplete?"":"UNVALIDATED:",d.providerPresent?1u:0u,
                d.identityComplete?"":"UNVALIDATED:",d.providerKnown?1u:0u,d.reason,
                d.identityComplete?"":"UNVALIDATED:",reinterpret_cast<void*>(d.targetRef),
                d.targetStateRead?"":
                    d.identityComplete && !d.targetRef?"NULL:":"UNKNOWN:",d.targetState,
                d.identityComplete && d.validatorKnown?"":"UNKNOWN:",d.valid5?1u:0u,
                d.identityComplete && d.validatorKnown?"":"UNKNOWN:",d.valid8?1u:0u);
            Log::get().note("flat stationary Placed-pose[%u,%u,%u]: local=%s(%.7g %.7g %.7g) cache=%s(%.7g %.7g %.7g) final=%s(%.7g %.7g %.7g) finite=%u/%u/%u pose-reread=%u pose-stable=%u cfg=%s%p cfg14=%s%d cfg18=%s%d override-handle=%s%d Q=%s%p Q-vt=%s%p/+0x%llX Q-slot70=%s%p/+0x%llX eligible-at-snapshot=%s%u; override execution UNKNOWN, D adoption UNPROVED",
                i,d.stage,d.activity,d.localRead?"":"UNKNOWN:",d.local[12],d.local[13],d.local[14],
                d.cacheRead?"":"UNKNOWN:",d.cache[12],d.cache[13],d.cache[14],
                d.finalRead?"":"UNKNOWN:",d.final[12],d.final[13],d.final[14],
                d.localFinite?1u:0u,d.cacheFinite?1u:0u,d.finalFinite?1u:0u,
                d.poseRereadComplete?1u:0u,d.poseStable?1u:0u,
                d.identityComplete?"":"UNVALIDATED:",reinterpret_cast<void*>(d.config),
                d.identityComplete?"":"UNVALIDATED:",d.config14,
                d.identityComplete?"":"UNVALIDATED:",d.config18,
                d.identityComplete?"":"UNVALIDATED:",d.overrideHandle,
                d.identityComplete?"":"UNVALIDATED:",reinterpret_cast<void*>(d.overrideObject),
                d.identityComplete?"":"UNVALIDATED:",reinterpret_cast<void*>(d.overrideVtable),
                static_cast<unsigned long long>(stationcam::rva(d.overrideVtable,s.gameBase.load())),
                d.identityComplete?"":"UNVALIDATED:",reinterpret_cast<void*>(d.overrideFn),
                static_cast<unsigned long long>(stationcam::rva(d.overrideFn,s.gameBase.load())),
                d.eligibilityKnown?"":"UNKNOWN:",d.overrideEligible?1u:0u);
            const bool recordAbsent = d.identityComplete && d.recordRead && !d.record;
            logPlacedMatrix(i,d.stage,d.activity,"E+20-local",d.local,!recordAbsent,d.localRead);
            logPlacedMatrix(i,d.stage,d.activity,"E+60-cache",d.cache,!recordAbsent,d.cacheRead);
            logPlacedMatrix(i,d.stage,d.activity,"A+70-final",d.final,true,d.finalRead);
        }
    }
    for (unsigned i = 0; i < kRefreshSlots; ++i) {
        auto& slot = s.refresh[i];
        if (!slot.ready.load(std::memory_order_acquire)) continue;
        if (emitted == kReportRows) break;
        ++emitted;
        Log::get().note("flat stationary refresh[%u]: camera=%p ctx=%p call=%llu caller=+0x%llX kind=%u camera+50=%s(%.7g %.7g %.7g); address/order join to renderer source UNPROVED",
            i,reinterpret_cast<void*>(slot.camera),reinterpret_cast<void*>(slot.ctx),
            static_cast<unsigned long long>(slot.callNo),static_cast<unsigned long long>(slot.callerRva),
            slot.kind,slot.originRead?"":"UNKNOWN:",slot.origin[0],slot.origin[1],slot.origin[2]);
    }
}

} // namespace

void flatStationaryCameraFrame(uint64_t completedFrame) noexcept {
    if (!flatOriginCaptureEnabled()) return;
    if (!runtimeFlatProfile()) return;
    State* state = ensureState();
    if (!state) {
        static bool noted = false;
        if (!noted) { noted = true; Log::get().note("flat stationary camera: refused: capture storage allocation failed"); }
        return;
    }
    State& s = *state;
    const uint64_t now = GetTickCount64();
    // This is the completed prefix frame, before flat_runtime assigns
    // prefix.frame = completedFrame + 1. Qualify its final witness with the
    // current journal snapshot; callbacks never advance the arming FSM.
    const JournalRawStatus raw = journalRawStatus();
    stationcam::CompletedScene completed;
    if ((s.phase == Phase::Waiting || s.phase == Phase::Capturing) && !s.closingFrame) {
        stationcam::ParkInput input;
        input.active = raw.active; input.gameplay = raw.gameplay; input.statusSamples = raw.statusSamples;
        input.guiKnown = raw.guiKnown; input.gui = raw.gui;
        input.dockedKnown = journalStatusDockedKnown(); input.docked = journalStatusDocked();
        input.seatedKnown = journalStatusSeatedKnown(); input.seated = journalStatusSeated();
        completed = s.scene.finish(completedFrame, input);
        if (!completed.processed) return;
        s.sceneRefusal = completed.sceneRefusal;
    }
    if (s.phase == Phase::Capturing) {
        if (s.profileDropped.load(std::memory_order_acquire) && !s.captureRefusal) s.captureRefusal = "profile-switched-during-capture";
        if (!s.closingFrame && completedFrame >= s.armFrame) {
            if (!completed.qualified && !s.captureRefusal)
                s.captureRefusal = std::strcmp(completed.sceneRefusal, "no-trigger-or-camera-witness") == 0 ?
                    "post-arm-no-passive-scene-witness" : "post-arm-parked-scene-gate-lost";
            if (completed.qualified && !s.captureRefusal && !s.witnessFrame) {
                s.witnessFrame = completedFrame;
                for (unsigned i = 0; i < 3; ++i) s.witnessOrigin[i] = completed.origin[i];
            }
        }
        if (!s.closingFrame && (s.captureRefusal || s.witnessFrame >= s.armFrame || completedFrame > s.armFrame + kCaptureFrames)) {
            s.gate.store(0, std::memory_order_release);
            s.closingFrame = completedFrame;
        }
        if (s.closingFrame && completedFrame > s.closingFrame &&
            s.inflight.load(std::memory_order_acquire) == 0) {
            report(s, completedFrame);
            s.phase = Phase::Done;
        }
        return;
    }
    if (s.phase == Phase::Waiting && completed.ready && completed.qualified) {
        if (!install(s)) {
            s.phase = Phase::Refused;
            Log::get().note("flat stationary camera: refused before arm: %s", s.waitReason);
            return;
        }
        s.armFrame = completedFrame + 1;
        s.gate.store(1, std::memory_order_release);
        s.phase = Phase::Capturing;
        Log::get().note("flat stationary camera: armed automatically for parked cockpit passive-scene candidate after %u consecutive qualified-scene frames and %u fresh Status samples; arm-frame=%llu; selected GPU donor unproved; bounded one-shot source/refresh capture",
            s.scene.park.stable,s.scene.park.fresh,static_cast<unsigned long long>(s.armFrame));
        return;
    }
    if (s.phase != Phase::Waiting || now - s.lastNoteMs < 5000) return;
    s.lastNoteMs = now;
    Log::get().note("flat stationary camera: waiting: %s; scene-refusal=%s witness-frame=%llu selected-frame=%llu watcher=%u gameplay=%u status-samples=%u fresh=%u docked=%u/%u seated=%u/%u gui=%u/%u qualified-scene-consecutive=%u; no capture budget consumed",
        s.scene.park.reason,s.sceneRefusal,static_cast<unsigned long long>(s.lastWitnessFrame),static_cast<unsigned long long>(s.lastSelectedFrame),raw.active?1u:0u,raw.gameplay?1u:0u,raw.statusSamples,s.scene.park.fresh,
        journalStatusDockedKnown()?1u:0u,journalStatusDocked()?1u:0u,
        journalStatusSeatedKnown()?1u:0u,journalStatusSeated()?1u:0u,
        raw.guiKnown?1u:0u,raw.gui,s.scene.park.stable);
}

void flatStationaryCameraSceneCandidate(uint64_t frame, const float camera[6][4],
                                        const void* depth) noexcept {
    if (!flatOriginCaptureEnabled()) return;
    if (!runtimeFlatProfile()) return;
    State* state = stateOrNull();
    if (!state) return;
    State& s = *state;
    if (s.phase != Phase::Waiting && s.phase != Phase::Capturing) return;
    if (s.closingFrame) return;
    if (s.scene.candidate(frame, camera ? camera[5] : nullptr, reinterpret_cast<uintptr_t>(depth)))
        s.lastWitnessFrame = frame;
}

void flatStationaryCameraSceneRejected(uint64_t frame, const char* reason) noexcept {
    if (!flatOriginCaptureEnabled()) return;
    if (!runtimeFlatProfile()) return;
    State* state = stateOrNull();
    if (!state) return;
    State& s = *state;
    if (s.phase != Phase::Waiting && s.phase != Phase::Capturing) return;
    if (s.closingFrame) return;
    if (s.scene.reject(frame, reason)) s.lastRejectedFrame = frame;
}

void flatStationaryCameraSelected(uint64_t frame, const float camera[6][4], bool reset,
                                   bool menuContract, const void* depth) noexcept {
    if (!flatOriginCaptureEnabled()) return;
    if (!runtimeFlatProfile() || !camera || !depth || menuContract) return;
    State* state = stateOrNull();
    if (!state) return;
    State& s = *state;
    if (s.lastSelectedFrame == frame) return;
    s.lastSelectedFrame = frame;
    if (s.phase == Phase::Capturing && frame >= s.armFrame && !s.selectedFrame) {
        s.selectedFrame = frame;
        for (unsigned i = 0; i < 3; ++i) s.selectedOrigin[i] = camera[5][i];
        s.selectedReset = reset;
    }
}

void flatStationaryCameraRefresh(uintptr_t camera, uintptr_t ctx, uint64_t callNo,
                                 uint64_t callerRva, uint32_t kind) noexcept {
    if (!flatOriginCaptureEnabled()) return;
    State* state = stateOrNull();
    if (!state) return; // existing refresh hook can precede this instrument's first Present
    State& s = *state;
    if (!runtimeFlatProfile()) {
        if (s.gate.load(std::memory_order_acquire)) {
            s.profileDropped.store(true, std::memory_order_release);
            s.gate.store(0, std::memory_order_release);
        }
        return;
    }
    if (!s.gate.load(std::memory_order_acquire) || !camera || kind != 3) return;
    s.refreshes.fetch_add(1, std::memory_order_relaxed);
    for (auto& slot : s.refresh) {
        uintptr_t expected = 0;
        if (slot.key.compare_exchange_strong(expected, camera, std::memory_order_acq_rel)) {
            slot.camera = camera; slot.ctx = ctx; slot.callNo = callNo;
            slot.callerRva = callerRva; slot.kind = kind;
            slot.originRead = sehCopy(nullptr, camera + 0x50, slot.origin, sizeof(slot.origin));
            slot.ready.store(true, std::memory_order_release);
            return;
        }
        if (expected == camera) return;
    }
    s.refreshOverflow.fetch_add(1, std::memory_order_relaxed);
}

} // namespace edvr
