#include "flat_origin_capture.h"
#include "flat_stationary_camera_capture.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <intrin.h>
#include <iterator>
#include <new>

#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "flat_stationary_camera_core.h"
#include "flat_camera_inject.h"
#include "eye_camera_capture_core.h"
#include "vr_camera_census_core.h"
#include "native_on_foot.h"
#include "ui_layer.h"
#include "../common/native_frame.h"
#include "journal_watch.h"

namespace edvr { namespace {

// Temporary F1 source-topology capture for the verified Steam executable.
// 2874290 is a normal three-argument, void Win64 function. The hook only
// observes its source and stack-local helper, and calls the original once.
// On another build every address here and in stationcam::kGameRvas comes from
// the signature resolver (src\common\game_build.h), or the capture stands down.
constexpr uintptr_t kRendererRva = 0x2874290;
constexpr uint8_t kPrologue[15] = {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20};
constexpr uintptr_t kMainViewCallerRva = 0x281D259; // the MainView root's renderer call returns here
constexpr uint32_t kHookRvas[] = {static_cast<uint32_t>(kRendererRva), static_cast<uint32_t>(kMainViewCallerRva)};
constexpr unsigned kSourceSlots = 16, kRefreshSlots = 32;
constexpr unsigned kReportRows = 1024;
constexpr uint64_t kCaptureFrames = 120;
using Renderer = void (*)(uintptr_t, uintptr_t, uintptr_t);

bool sehCopy(void*, uintptr_t at, void* out, size_t size) noexcept {
    __try { std::memcpy(out, reinterpret_cast<const void*>(at), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// base + an RVA gameRva returned, 0 where it found none.
uintptr_t gameAddress(uintptr_t base, uintptr_t resolved) noexcept { return resolved ? base + resolved : 0; }

struct SourceSlot {
    std::atomic<uintptr_t> key{0};
    std::atomic<bool> ready{false};
    stationcam::Snapshot snapshot;
};
struct RefreshSlot {
    std::atomic<uintptr_t> key{0};
    std::atomic<bool> ready{false};
    uintptr_t camera = 0, ctx = 0;
    uint64_t callNo = 0, observedCallerOffset = 0;
    uint32_t kind = 0;
    float origin[4]{};
    bool originRead = false;
};

struct EyeSource {
    std::atomic<bool> ready{false};
    uint64_t epoch=0, beginQpc=0, endQpc=0;
    uint32_t ledgerFrame=0, nativeFrame=0, endNativeFrame=0, thread=0;
    stationcam::Snapshot snapshot;
};
struct EyeRefresh {
    uintptr_t camera=0, ctx=0, view=0;
    uint64_t call=0, observedCallerOffset=0, preQpc=0, postQpc=0, sourceEpoch=0;
    uint32_t thread=0, sourceOrdinal=UINT32_MAX, nativeFrame=0, postNativeFrame=0;
    bool preRead=false, postRead=false, postSeen=false, injected=false;
    VrCensusSnap pre{}, post{};
};
struct EyeCapture {
    eyecamera::Window window;
    wchar_t stamp[32]{};
    std::atomic<uint64_t> live{0};
    uint64_t epoch=0, frequency=0, beginQpc=0, endQpc=0;
    uint32_t ledgerFrame=0, nativeFrame=0, ownerThread=0;
    std::atomic<uint32_t> sources{0}, inflight{0}, foreignRefresh{0};
    uint32_t refreshes=0;
    EyeSource source[eyecamera::kSources];
    EyeRefresh refresh[eyecamera::kRefresh];
    const char* status="not-armed";
    uint32_t gui=0, suite=0, effective=0;
    bool guiKnown=false, onFoot=false, screenHeld=false, screenNamed=false, poseAvailable=false;
    bool armRefused=false;
    EdvrNativeNowPose pose{};
};
static_assert(sizeof(EyeCapture) <= 8*1024*1024, "manual camera storage must stay bounded");
thread_local uint64_t g_eyeSourceEpoch=0;
thread_local uint32_t g_eyeSourceOrdinal=UINT32_MAX;
enum class Phase : uint8_t { Waiting, Capturing, Done, Refused };
struct State {
    CodeHook hook;
    uint8_t* relay = nullptr; // held for process lifetime after install
    std::atomic<uintptr_t> gate{0};
    std::atomic<uintptr_t> forward{0};
    std::atomic<uintptr_t> gameBase{0};
    std::atomic<bool> automaticLive{false};
    // The resolved addresses (install fills them before it stores gameBase): the decoder's, the MainView call site, the image size.
    stationcam::Game game;
    uintptr_t mainViewCaller = 0, imageSize = 0;
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
    EyeCapture eye;
};
std::atomic<State*> g_state{nullptr};
State* stateOrNull() noexcept { return g_state.load(std::memory_order_acquire); }
State* allocateState() noexcept {
    State* p = stateOrNull();
    if (!p) {
        p = new (std::nothrow) State;
        if (p) g_state.store(p, std::memory_order_release); // Present owns initialization
    }
    return p;
}
State* ensureState() noexcept {
    if (!flatOriginCaptureEnabled()) return nullptr;
    return allocateState();
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

void fillSource(State& s, stationcam::Snapshot& out, uintptr_t owner, uintptr_t source,
                uintptr_t helper, uintptr_t caller, const float before[4], bool beforeRead) noexcept {
    const uintptr_t base = s.gameBase.load(std::memory_order_acquire);
    out.owner = owner; out.source = source; out.helper = helper;
    out.caller = caller;
    out.observedCallerOffset = stationcam::rva(caller, base, s.imageSize);
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
    stationcam::capture(&sehCopy, nullptr, s.game, out);
}
void observeSource(uintptr_t owner, uintptr_t source, uintptr_t helper, uintptr_t caller,
                   const float before[4], bool beforeRead, uint64_t epoch) noexcept {
    State& s = *stateOrNull();
    const uintptr_t key = source ? source : 1;
    SourceSlot* chosen = nullptr;
    for (auto& slot : s.sources) {
        uintptr_t expected = 0;
        if (slot.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) { chosen=&slot; break; }
        if (expected == key) return; // automatic capture retains its original per-source policy
    }
    if (!chosen) { s.sourceOverflow.fetch_add(1, std::memory_order_relaxed); return; }
    auto& out=chosen->snapshot;
    fillSource(s,out,owner,source,helper,caller,before,beforeRead);
    if (s.armFrame != epoch) { out.consistent = false; stationcam::fail(out, "frame-overlap"); }
    chosen->ready.store(true, std::memory_order_release);
    s.captures.fetch_add(1, std::memory_order_relaxed);
}

void rendererDetour(uintptr_t owner, uintptr_t source, uintptr_t helper) {
    State& s = *stateOrNull(); // this callback exists only after install
    s.rendererCalls.fetch_add(1, std::memory_order_relaxed);
    const auto original = reinterpret_cast<Renderer>(s.forward.load(std::memory_order_acquire));
    EyeCapture& e=s.eye;
    const uint64_t manual=e.live.load(std::memory_order_acquire);
    const bool automatic=s.automaticLive.load(std::memory_order_acquire);
    if (!automatic && !manual) {
        s.profileDropped.store(true, std::memory_order_release);
        s.gate.store(0, std::memory_order_release);
        original(owner, source, helper);
        return;
    }
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uint64_t epoch = automatic ? s.armFrame : 0;
    s.inflight.fetch_add(1, std::memory_order_acq_rel);
    float before[4]{};
    const bool beforeRead = source && sehCopy(nullptr, source + 0x70, before, sizeof(before));
    EyeSource* row=nullptr;
    const uint64_t previousEpoch=g_eyeSourceEpoch;
    const uint32_t previousOrdinal=g_eyeSourceOrdinal;
    if (manual) {
        e.inflight.fetch_add(1,std::memory_order_acq_rel);
        if (e.live.load(std::memory_order_acquire)==manual) {
            const uint32_t ordinal=e.sources.fetch_add(1,std::memory_order_relaxed);
            g_eyeSourceEpoch=manual; g_eyeSourceOrdinal=ordinal;
            if (ordinal<eyecamera::kSources) {
                row=&e.source[ordinal]; row->epoch=manual; row->ledgerFrame=e.ledgerFrame;
                row->nativeFrame=nativeOnFootRenderFrame(); row->thread=GetCurrentThreadId();
                LARGE_INTEGER q; QueryPerformanceCounter(&q); row->beginQpc=q.QuadPart;
            }
        }
    }
    original(owner, source, helper);
    if (row) {
        fillSource(s,row->snapshot,owner,source,helper,caller,before,beforeRead);
        row->endNativeFrame=nativeOnFootRenderFrame();
        if(row->nativeFrame!=row->endNativeFrame || e.live.load(std::memory_order_acquire)!=manual) {
            row->snapshot.consistent=false;stationcam::fail(row->snapshot,"manual-frame-overlap");
        }
        LARGE_INTEGER q; QueryPerformanceCounter(&q); row->endQpc=q.QuadPart;
        row->ready.store(true,std::memory_order_release);
    }
    g_eyeSourceEpoch=previousEpoch; g_eyeSourceOrdinal=previousOrdinal;
    if (manual) e.inflight.fetch_sub(1,std::memory_order_acq_rel);
    if (automatic && runtimeFlatProfile()) observeSource(owner, source, helper, caller, before, beforeRead, epoch);
    else if (automatic) {
        s.profileDropped.store(true, std::memory_order_release);
        s.gate.store(0, std::memory_order_release);
    }
    s.inflight.fetch_sub(1, std::memory_order_acq_rel);
}

bool install(State& s) noexcept {
    if (s.hook.installed()) return true;
    const HMODULE game = GetModuleHandleW(L"EliteDangerous64.exe");
    if (!game) { s.waitReason = "game-module-unavailable"; return false; }
    const uintptr_t base = reinterpret_cast<uintptr_t>(game);
    // The addresses in the running build (src\common\game_build.h): build 334925's own; on an unknown build each the signature
    // resolver found, and the capture stands down unless it found every one.
    uint32_t imageSize = 0, missing = 0;
    const bool identityRead = gameBuildIdentity(nullptr, &imageSize);
    const bool knownBuild = identityRead && gameBuildKnown();
    const bool allFound = gameRvasFound(kHookRvas, std::size(kHookRvas), &missing) &&
        gameRvasFound(stationcam::kGameRvas, stationcam::kGameAddresses, &missing);
    if (!allFound) {
        Log::get().note("flat stationary camera: %s, and 0x%X in build 334925 (an address the renderer capture needs) was not found "
                        "by signature; the capture stands down", gameBuildText(), static_cast<unsigned>(missing));
        s.waitReason = "not-found-by-signature"; return false;
    }
    const uintptr_t rendererRva = gameRva(static_cast<uint32_t>(kRendererRva));
    uint8_t found[sizeof(kPrologue)]{};
    if (!sehCopy(nullptr, base + rendererRva, found, sizeof(found)) ||
        !gamePinMatches(static_cast<uint32_t>(kRendererRva), found, kPrologue, sizeof(found))) {
        if (knownBuild) { s.waitReason = "verified-exe-or-prologue-mismatch"; return false; }
        Log::get().note("flat stationary camera: %s: the renderer was found by signature at 0x%llX, but its fifteen prologue bytes are "
                        "not build 334925's outside the masked fields; the capture stands down",
                        gameBuildText(), static_cast<unsigned long long>(rendererRva));
        s.waitReason = "prologue-mismatch-after-signature"; return false;
    }
    for (unsigned i = 0; i < stationcam::kGameAddresses; ++i) s.game.at[i] = gameAddress(base, gameRva(stationcam::kGameRvas[i]));
    s.mainViewCaller = gameAddress(base, gameRva(static_cast<uint32_t>(kMainViewCallerRva)));
    // The bound on the RVAs the log prints: build 334925's, as it always was; another build's SizeOfImage.
    constexpr uintptr_t kKnownLogBound = 0x6000000;
    s.imageSize = knownBuild ? kKnownLogBound : imageSize;
    if (!s.game.found() || !s.mainViewCaller || !s.imageSize) { s.waitReason = "not-found-by-signature"; return false; }
    s.gameBase.store(base, std::memory_order_release);
    s.relay = allocateRelay(base + rendererRva);
    if (!s.relay) { s.waitReason = "relay-allocation-failed"; return false; }
    buildRelay(s.relay, &s.gate, reinterpret_cast<void*>(&rendererDetour));
    if (!s.hook.install(reinterpret_cast<void*>(base + rendererRva), s.relay, nullptr,
                        "flat-stationary-camera", &prepareRelay, &s)) {
        VirtualFree(s.relay, 0, MEM_RELEASE);
        s.relay = nullptr;
        s.waitReason = "CodeHook-refused"; return false;
    }
    if (!knownBuild)
        Log::get().note("flat stationary camera: the renderer (0x2874290 in build 334925) and the capture's %u other addresses were "
                        "found by signature on %s; the renderer is at 0x%llX",
                        static_cast<unsigned>(std::size(kHookRvas) - 1 + stationcam::kGameAddresses), gameBuildText(),
                        static_cast<unsigned long long>(rendererRva));
    Log::get().note("flat stationary camera: passive renderer hook installed at +0x%llX; no game getters called or camera writes made",
                    static_cast<unsigned long long>(rendererRva));
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
            slot.snapshot.stages[0].stackVtable == s.game.at[stationcam::kStackVtable] &&
            slot.snapshot.stages[0].nameRead &&
            std::strcmp(slot.snapshot.stages[0].stackName, "MainView") == 0 &&
            slot.snapshot.caller == s.mainViewCaller) ++mainView;
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
            i, static_cast<unsigned long long>(q.observedCallerOffset), reinterpret_cast<void*>(q.owner),
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
                static_cast<unsigned long long>(stationcam::rva(z.stackVtable,s.gameBase.load(),s.imageSize)),
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
                    static_cast<unsigned long long>(stationcam::rva(a.vtable,s.gameBase.load(),s.imageSize)),
                    static_cast<unsigned long long>(stationcam::rva(a.concreteVtable,s.gameBase.load(),s.imageSize)),
                    static_cast<unsigned long long>(stationcam::rva(a.weightFn,s.gameBase.load(),s.imageSize)),
                    a.weightKnown ? "" : "UNKNOWN:",a.weight,
                    static_cast<unsigned long long>(stationcam::rva(a.stateFn,s.gameBase.load(),s.imageSize)),
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
                static_cast<unsigned long long>(stationcam::rva(d.providerVtable,s.gameBase.load(),s.imageSize)),
                d.identityComplete?"":"UNVALIDATED:",reinterpret_cast<void*>(d.providerFn),
                static_cast<unsigned long long>(stationcam::rva(d.providerFn,s.gameBase.load(),s.imageSize)),
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
                static_cast<unsigned long long>(stationcam::rva(d.overrideVtable,s.gameBase.load(),s.imageSize)),
                d.identityComplete?"":"UNVALIDATED:",reinterpret_cast<void*>(d.overrideFn),
                static_cast<unsigned long long>(stationcam::rva(d.overrideFn,s.gameBase.load(),s.imageSize)),
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
            static_cast<unsigned long long>(slot.callNo),static_cast<unsigned long long>(slot.observedCallerOffset),
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
            s.automaticLive.store(false,std::memory_order_release);
            if (!s.eye.live.load(std::memory_order_acquire)) s.gate.store(0, std::memory_order_release);
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
        s.automaticLive.store(true,std::memory_order_release);
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
                                 uint64_t observedCallerOffset, uint32_t kind) noexcept {
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
            slot.observedCallerOffset = observedCallerOffset; slot.kind = kind;
            slot.originRead = sehCopy(nullptr, camera + 0x50, slot.origin, sizeof(slot.origin));
            slot.ready.store(true, std::memory_order_release);
            return;
        }
        if (expected == camera) return;
    }
    s.refreshOverflow.fetch_add(1, std::memory_order_relaxed);
}

namespace {
uint64_t eyeQpc() noexcept { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
struct EyeJson {
    FILE* file=nullptr;
    size_t bytes=0;
    bool ok=true;
    void put(const char* format, ...) noexcept {
        if (!ok) return;
        char text[2048]; va_list args; va_start(args,format);
        const int count=vsnprintf(text,sizeof(text),format,args); va_end(args);
        if (count<0 || size_t(count)>=sizeof(text) || bytes+size_t(count)>16*1024*1024) { ok=false; return; }
        if (fwrite(text,1,size_t(count),file)!=size_t(count)) { ok=false; return; }
        bytes+=size_t(count);
    }
    void floats(const float* p,unsigned n,bool available=true) noexcept {
        if (!available) { put("null"); return; }
        put("["); for (unsigned i=0;i<n;++i) { if(i)put(","); if(std::isfinite(p[i]))put("%.9g",p[i]); else put("null"); } put("]");
    }
    void string(const char* p) noexcept {
        put("\"");for(;*p;++p) {if(*p=='\"' || *p=='\\')put("\\%c",*p);else put("%c",*p);}put("\"");
    }
    void pose(const char* name,const float* p,bool available) noexcept { put(",\"%s\":",name); floats(p,16,available); }
};
void eyeSnapshotJson(EyeJson& j,const stationcam::Snapshot& s,unsigned& stages,unsigned& activities,unsigned& placed) noexcept {
    j.put("\"owner\":\"%p\",\"source\":\"%p\",\"helper\":\"%p\",\"caller_rva\":%llu,\"valid_source\":%u,\"consistent\":%u,\"complete\":%u,\"failure\":\"%s\",\"source_before\":",
        reinterpret_cast<void*>(s.owner),reinterpret_cast<void*>(s.source),reinterpret_cast<void*>(s.helper),
        s.observedCallerOffset,unsigned(s.validSource),unsigned(s.consistent),unsigned(s.complete),s.failure);
    j.floats(s.sourceBefore,4,s.sourceBeforeRead); j.put(",\"source_after\":"); j.floats(s.sourceAfter,4,s.sourceAfterRead);
    j.put(",\"helper_after\":"); j.floats(s.helperAfter,4,s.helperAfterRead);
    j.put(",\"stage_count\":%u,\"stages\":[",s.stageCount);
    unsigned emitted=0;
    for(unsigned i=0;i<s.stageCount && stages<eyecamera::kStagesWritten;++i,++stages) {
        const auto& a=s.stages[i]; if(emitted++)j.put(",");
        j.put("{\"index\":%u,\"child\":\"%p\",\"stack\":\"%p\",\"predecessor\":\"%p\",\"parent\":%d,\"edge\":\"%c\",\"mode\":%u,\"name\":",i,
            reinterpret_cast<void*>(a.child),reinterpret_cast<void*>(a.stack),reinterpret_cast<void*>(a.predecessor),a.parent,a.edge,a.mode);
        if(a.nameRead)j.string(a.stackName); else j.put("null");
        j.pose("base_pose",a.basePose,a.baseRead); j.pose("output_pose",a.outputPose,a.outputRead);
        j.put(",\"activity_count\":%llu,\"activities\":[",a.activityCount);
        unsigned wrote=0;
        for(unsigned k=0;k<a.activityCount && k<stationcam::kActivities && activities<eyecamera::kActivitiesWritten;++k,++activities) {
            const auto& b=a.activities[k]; if(wrote++)j.put(",");
            j.put("{\"index\":%u,\"iface\":\"%p\",\"vtable\":\"%p\",\"concrete_vtable\":\"%p\",\"weight_fn\":\"%p\",\"state_fn\":\"%p\",\"state\":\"%p\",\"name\":",k,
                reinterpret_cast<void*>(b.iface),reinterpret_cast<void*>(b.vtable),reinterpret_cast<void*>(b.concreteVtable),
                reinterpret_cast<void*>(b.weightFn),reinterpret_cast<void*>(b.stateFn),reinterpret_cast<void*>(b.state));
            if(b.nameRead)j.string(b.name); else j.put("null");
            j.put(",\"weight_known\":%u,\"state_known\":%u,\"weight\":",unsigned(b.weightKnown),unsigned(b.stateKnown));
            j.floats(&b.weight,1,b.weightKnown); j.pose("pose",b.pose,b.poseRead); j.put("}");
        }
        j.put("],\"activities_written\":%u}",wrote);
    }
    j.put("],\"stages_written\":%u,\"placed_count\":%u,\"placed_overflow\":%u,\"placed\":[",emitted,s.placedCount,unsigned(s.placedOverflow));
    emitted=0;
    for(unsigned i=0;i<s.placedCount && placed<eyecamera::kPlacedWritten;++i,++placed) {
        const auto& p=s.placed[i]; if(emitted++)j.put(",");
        j.put("{\"stage\":%u,\"activity\":%u,\"object\":\"%p\",\"record\":\"%p\",\"provider\":\"%p\",\"provider_vtable\":\"%p\",\"provider_fn\":\"%p\",\"identity_complete\":%u,\"provider_known\":%u,\"override_eligible_known\":%u,\"override_eligible\":%u,\"pose_stable\":%u,\"reason\":\"%s\"",p.stage,p.activity,
            reinterpret_cast<void*>(p.object),reinterpret_cast<void*>(p.record),reinterpret_cast<void*>(p.provider),reinterpret_cast<void*>(p.providerVtable),reinterpret_cast<void*>(p.providerFn),
            unsigned(p.identityComplete),unsigned(p.providerKnown),unsigned(p.eligibilityKnown),unsigned(p.overrideEligible),unsigned(p.poseStable),p.reason);
        j.put(",\"config\":\"%p\",\"override_object\":\"%p\",\"override_vtable\":\"%p\",\"override_fn\":\"%p\",\"target_ref\":\"%p\",\"override_handle\":%d,\"config14\":%d,\"config18\":%d,\"target_state\":%d,\"target_state_read\":%u,\"validator_known\":%u,\"record_read\":%u,\"provider_present\":%u,\"override_present\":%u,\"valid5\":%u,\"valid8\":%u,\"pose_reread_complete\":%u",
            reinterpret_cast<void*>(p.config),reinterpret_cast<void*>(p.overrideObject),reinterpret_cast<void*>(p.overrideVtable),reinterpret_cast<void*>(p.overrideFn),reinterpret_cast<void*>(p.targetRef),p.overrideHandle,p.config14,p.config18,p.targetState,
            unsigned(p.targetStateRead),unsigned(p.validatorKnown),unsigned(p.recordRead),unsigned(p.providerPresent),unsigned(p.overridePresent),unsigned(p.valid5),unsigned(p.valid8),unsigned(p.poseRereadComplete));
        j.pose("local",p.local,p.localRead); j.pose("cache",p.cache,p.cacheRead); j.pose("final",p.final,p.finalRead); j.put("}");
    }
    j.put("],\"placed_written\":%u",emitted);
}
void eyeCameraJson(EyeJson& j,const VrCensusSnap& s,bool read) noexcept {
    if(!read) { j.put("null"); return; }
    float origin[4]{},axes[12]{},rows[16]{},view[16]{},projection[16]{},vp[16]{};
    memcpy(origin,s.bytes+(0x50-kVrCensusSnapFrom),sizeof(origin));
    memcpy(axes,s.bytes+(0x20-kVrCensusSnapFrom),sizeof(axes));
    memcpy(view,s.bytes+(0x190-kVrCensusSnapFrom),sizeof(view));
    memcpy(projection,s.bytes+(0x1D0-kVrCensusSnapFrom),sizeof(projection));
    memcpy(vp,s.bytes+(0x210-kVrCensusSnapFrom),sizeof(vp));
    const bool composed=vrCensusComposeRows(s,rows);
    j.put("{\"kind\":%u,\"flags\":%u,\"stored_negative_origin\":",s.u(0x264),s.u(0x250));
    j.floats(origin,4); j.put(",\"axes\":");j.floats(axes,12);
    j.pose("cached_view",view,true);j.pose("cached_projection",projection,true);j.pose("cached_vp",vp,true);j.pose("composed_orientation_projection",rows,composed);j.put("}");
}
}

void flatStationaryCameraEyeArm(uint32_t selectedLedgerFrame,const wchar_t* stamp) noexcept {
    State* s=allocateState();
    if(!s) { Log::get().note("eye camera: arm unavailable (storage allocation failed)"); return; }
    EyeCapture& e=s->eye;
    e.live.store(0,std::memory_order_release);
    e.armRefused=e.inflight.load(std::memory_order_acquire)!=0;
    if(e.armRefused) { Log::get().note("eye camera: arm run=%ls unavailable (previous callback still inflight)",stamp?stamp:L"unknown");return; }
    e.window.arm(selectedLedgerFrame); ++e.epoch;
    wcsncpy_s(e.stamp,stamp?stamp:L"unknown",_TRUNCATE);
    e.sources.store(0);e.foreignRefresh.store(0);e.refreshes=0;e.ledgerFrame=0;e.nativeFrame=0;
    e.beginQpc=0;e.endQpc=0;e.status="queued";
    e.gui=0;e.suite=0;e.effective=0;e.guiKnown=false;e.onFoot=false;e.screenHeld=false;e.screenNamed=false;e.poseAvailable=false;e.pose={};
    for(auto& r:e.source) {r.ready.store(false);r.snapshot=stationcam::Snapshot{};}
    for(auto& r:e.refresh)r=EyeRefresh{};
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);e.frequency=f.QuadPart;
    Log::get().note("eye camera: armed run=%ls selected-ledger-frame=%u; passive existing source renderer and refresh taps, caps %u source calls/%u refresh calls/8 MiB storage; install at Present only",e.stamp,selectedLedgerFrame,eyecamera::kSources,eyecamera::kRefresh);
}
void flatStationaryCameraEyeBoundary(uint32_t ledgerFrame,uint32_t nativeRenderFrame) noexcept {
    State* s=stateOrNull();if(!s || s->eye.armRefused || !s->eye.window.armed)return;
    EyeCapture& e=s->eye;
    if(e.live.exchange(0,std::memory_order_acq_rel)) {e.endQpc=eyeQpc();e.status="closed";}
    if(!s->automaticLive.load(std::memory_order_acquire))s->gate.store(0,std::memory_order_release);
    if(!e.window.boundary(ledgerFrame))return;
    if(!install(*s)) {e.status=s->waitReason;return;}
    e.ledgerFrame=ledgerFrame;e.nativeFrame=nativeRenderFrame;e.ownerThread=GetCurrentThreadId();e.beginQpc=eyeQpc();
    e.guiKnown=journalGuiFocus(&e.gui);e.onFoot=nativeOnFootActive();
    e.suite=g_nativeOnFootCamMode.load(std::memory_order_relaxed);e.effective=g_nativeLastEffective.load(std::memory_order_relaxed);
    e.screenHeld=uiLayerWorldScreenHeld();e.screenNamed=uiLayerLastFrameNamed();e.poseAvailable=nativeFrameNowPose(e.pose);
    e.status="sampling";e.live.store(e.epoch,std::memory_order_release);s->gate.store(1,std::memory_order_release);
}
void flatStationaryCameraEyeRefreshPre(uintptr_t returnSlot,uintptr_t camera,uintptr_t ctx,uintptr_t view,uint64_t callNo,bool offThread) noexcept {
    State* s=stateOrNull();if(!s)return;EyeCapture& e=s->eye;
    if(!e.live.load(std::memory_order_acquire))return;
    if(offThread || GetCurrentThreadId()!=e.ownerThread) {e.foreignRefresh.fetch_add(1,std::memory_order_relaxed);return;}
    const uint32_t index=e.refreshes++;if(index>=eyecamera::kRefresh)return;
    auto& r=e.refresh[index];r.camera=camera;r.ctx=ctx;r.view=view;r.call=callNo;r.thread=GetCurrentThreadId();r.preQpc=eyeQpc();r.nativeFrame=nativeOnFootRenderFrame();
    r.sourceEpoch=g_eyeSourceEpoch;r.sourceOrdinal=g_eyeSourceOrdinal;
    uintptr_t ret=0;const uintptr_t base=s->gameBase.load(std::memory_order_acquire);
    if(sehCopy(nullptr,returnSlot,&ret,sizeof(ret)))r.observedCallerOffset=stationcam::rva(ret,base,s->imageSize);
    r.preRead=camera && sehCopy(nullptr,camera+kVrCensusSnapFrom,r.pre.bytes,sizeof(r.pre.bytes));
}
void flatStationaryCameraEyeRefreshPost(uintptr_t camera,uint64_t callNo,bool injected) noexcept {
    State* s=stateOrNull();if(!s)return;EyeCapture& e=s->eye;
    if(!e.live.load(std::memory_order_acquire) || GetCurrentThreadId()!=e.ownerThread)return;
    const unsigned count=(e.refreshes<eyecamera::kRefresh)?e.refreshes:eyecamera::kRefresh;
    for(unsigned i=count;i>0;--i) {auto& r=e.refresh[i-1];if(r.call!=callNo || r.camera!=camera)continue;
        r.postSeen=true;r.injected=injected;r.postQpc=eyeQpc();r.postNativeFrame=nativeOnFootRenderFrame();r.postRead=sehCopy(nullptr,camera+kVrCensusSnapFrom,r.post.bytes,sizeof(r.post.bytes));return;}
}
void flatStationaryCameraEyeEnd() noexcept {
    State* s=stateOrNull();if(!s)return;
    auto& e=s->eye;if(e.live.exchange(0,std::memory_order_acq_rel)){e.endQpc=eyeQpc();e.status="ended-before-boundary";}
    e.window.end();if(!s->automaticLive.load(std::memory_order_acquire))s->gate.store(0,std::memory_order_release);
}
bool flatStationaryCameraEyeWrite(const wchar_t* path) noexcept {
    State* s=stateOrNull();if(!s || s->eye.armRefused || !path)return false;
    auto& e=s->eye;flatStationaryCameraEyeEnd();
    FILE* file=nullptr;if(_wfopen_s(&file,path,L"wb") || !file)return false;
    EyeJson j;j.file=file;char stamp[32]{};for(unsigned i=0;i<31 && e.stamp[i];++i)stamp[i]=(e.stamp[i]>=32 && e.stamp[i]<127)?char(e.stamp[i]):'?';
    const unsigned inflight=e.inflight.load(std::memory_order_acquire),calls=e.sources.load(std::memory_order_acquire);
    const unsigned sourceCount=calls<eyecamera::kSources?calls:eyecamera::kSources;
    const unsigned refreshCount=e.refreshes<eyecamera::kRefresh?e.refreshes:eyecamera::kRefresh;
    j.put("{\"version\":1,\"capture_id\":\"%s\",\"status\":\"%s\",\"sampled\":%u,\"selected_ledger_frame\":%u,\"ledger_frame\":%u,\"native_render_frame\":%u,\"epoch\":%llu,\"qpc_frequency\":%llu,\"begin_qpc\":%llu,\"end_qpc\":%llu,\"refresh_hook\":\"%s\",\"inflight\":%u,\"source_calls\":%u,\"source_overflow\":%u,\"refresh_calls\":%u,\"refresh_overflow\":%u,\"foreign_refresh_unavailable\":%u,",
        stamp,e.status,unsigned(e.window.sampled),e.window.selected,e.ledgerFrame,e.nativeFrame,e.epoch,e.frequency,e.beginQpc,e.endQpc,flatCameraInjectObserveStatus(),inflight,calls,calls-sourceCount,e.refreshes,e.refreshes-refreshCount,e.foreignRefresh.load());
    j.put("\"source_hook_installed\":%u,\"resolved_mainview_caller_rva\":%llu,",unsigned(s->hook.installed()),stationcam::rva(s->mainViewCaller,s->gameBase.load(std::memory_order_acquire),s->imageSize));
    j.put("\"limits\":{\"sources\":16,\"refreshes\":640,\"stages_written\":192,\"activities_written\":1024,\"placed_written\":64,\"file_bytes\":16777216},\"notes\":\"Original game camera pre-copy precedes EDVR injection/flush. Cached derived pre matrices can be stale. Post is collected only when the existing hook already redirected the return; absent post is unavailable. Source topology is post-original guarded memory, not proof of which activity executed. Same-thread source span is an actual nesting witness; absent span has no direct source join. Parent getter/override execution and HMD activity internals remain unavailable. EDVR pose is separate cached runtime evidence; applied alignment vector is unavailable here. All base/output poses use positive origin; camera+0x50 stores negative origin. Serialization quotas are independent of capture quotas.\",\"context\":{\"gui_known\":%u,\"gui\":%u,\"on_foot\":%u,\"suite_raw_known_bit_256\":%u,\"effective_render_mode\":%u,\"screen_held\":%u,\"previous_frame_screen_named\":%u},\"runtime_pose\":{\"available\":%u,\"frame_sequence\":%llu,\"now_sequence\":%u,\"reference_generation\":%u,\"eye_align_applied_serial\":%u,\"render\":",
        unsigned(e.guiKnown),e.gui,unsigned(e.onFoot),e.suite,e.effective,unsigned(e.screenHeld),unsigned(e.screenNamed),unsigned(e.poseAvailable),e.pose.frameSequence,e.pose.nowSequence,e.pose.referenceGeneration,e.pose.eyeAlignApplied);
    j.floats(e.pose.render,12,e.poseAvailable&&e.pose.renderValid);j.put(",\"now\":");j.floats(e.pose.now,12,e.poseAvailable&&e.pose.nowValid);j.put(",\"natural_local\":");j.floats(e.pose.local,12,e.poseAvailable&&e.pose.localValid);j.put("},\"sources\":[");
    unsigned stages=0,activities=0,placed=0;
    for(unsigned i=0;i<sourceCount;++i) {if(i)j.put(",");const auto& r=e.source[i];const bool ready=r.ready.load(std::memory_order_acquire);
        j.put("{\"ordinal\":%u,\"available\":%u",i,unsigned(ready));
        if(ready) {j.put(",\"epoch\":%llu,\"ledger_frame\":%u,\"native_render_frame\":%u,\"end_native_render_frame\":%u,\"thread\":%u,\"begin_qpc\":%llu,\"end_qpc\":%llu,",r.epoch,r.ledgerFrame,r.nativeFrame,r.endNativeFrame,r.thread,r.beginQpc,r.endQpc);eyeSnapshotJson(j,r.snapshot,stages,activities,placed);}j.put("}");}
    j.put("],\"refreshes\":[");
    for(unsigned i=0;i<refreshCount;++i) {if(i)j.put(",");const auto& r=e.refresh[i];
        j.put("{\"ordinal\":%u,\"call\":%llu,\"camera\":\"%p\",\"ctx\":\"%p\",\"view\":\"%p\",\"caller_rva\":%llu,\"thread\":%u,\"source_epoch\":%llu,\"source_ordinal\":%u,\"pre_qpc\":%llu,\"post_qpc\":%llu,\"post_seen\":%u,\"post_injected\":%u,\"pre\":",i,r.call,reinterpret_cast<void*>(r.camera),reinterpret_cast<void*>(r.ctx),reinterpret_cast<void*>(r.view),r.observedCallerOffset,r.thread,r.sourceEpoch,r.sourceOrdinal,r.preQpc,r.postQpc,unsigned(r.postSeen),unsigned(r.injected));
        eyeCameraJson(j,r.pre,r.preRead);j.put(",\"post\":");eyeCameraJson(j,r.post,r.postRead);j.put(",\"native_render_frame\":%u,\"post_native_render_frame\":%u}",r.nativeFrame,r.postNativeFrame);}
    j.put("],\"serialization\":{\"stages_written\":%u,\"activities_written\":%u,\"placed_written\":%u},\"empty\":%u}",stages,activities,placed,unsigned(!calls&&!e.refreshes));
    const bool flushed=fflush(file)==0;const bool closed=fclose(file)==0;
    const bool ok=j.ok&&flushed&&closed;
    Log::get().note("eye camera: run=%ls ledger=%u native=%u status=%s sources=%u/%u refreshes=%u/%u inflight=%u foreign=%u bytes=%zu %s; zero rows explicitly unavailable, post requires an existing return callback",e.stamp,e.ledgerFrame,e.nativeFrame,e.status,sourceCount,calls,refreshCount,e.refreshes,inflight,e.foreignRefresh.load(),j.bytes,ok?"written":"WRITE FAILED");
    return ok;
}

} // namespace edvr
