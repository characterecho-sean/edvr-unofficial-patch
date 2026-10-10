#include "flat_runtime.h"
#include "flat_sdk_bench_probe.h"
#include "flat_capture_policy.h"
#include "flat_runtime_model.h"
#include "flat_hdr_route.h"
#include "flat_copy_structure.h"
#include "flat_hdr_crumbs.h"
#include "flat_context_state.h"
#include "flat_mono_resolve.h"
#include "flat_projection_recipes.h"
#include "flat_shader_classifier.h"
#include "flat_projection_ownership.h"
#include "flat_camera_probe.h"
#include "flat_camera_inject.h"
#include "flat_camera_phase.h"
#include "flat_live_phase.h"
#include "flat_draw_capture.h"
#include "flat_draw_packet_capture.h"
#include "flat_weapon_footprint.h"
#include "flat_overlay_layer.h"
#include "flat_replay_query_tracker.h"
#include "flat_untrusted_coverage.h"
#include "flat_foreground_probe.h"
#include "flat_foreground_motion.h"
#include "flat_no_candidate_report.h"
#include "flat_history_report.h"
#include "flat_foreground_shadow.h"
#include "flat_source_spell.h"
#include "flat_foreground_phase.h"
#include "flat_domain_admission.h"
#include "flat_domain_depth_route.h"
#include "dxbc_engine_velocity.h"
#include "flat_pixel_capture_policy.h"
#include "flat_local_reject.h"
#include "flat_trace.h"
#include "flat_dlss_negotiate.h"
#include "flat_negotiated_eval.h"
#include "flat_camera_producer_probe.h"
#include "flat_standdown.h"
#include "flat_witness_bound.h"
#include "flat_cpu.h"
#include "flat_camera_table.h"
#include "flat_query_cut.h"
#include "flat_query_reads.h"
#include "gpu_timing.h"
#include "flat_temporal.h"
#include "engine_velocity.h"
#include "weapon_motion.h"
#include "binding_shadow.h"
#include "exposure_fix.h"
#include "device_hook.h"
#include "journal_watch.h"   // the System Map's GuiFocus for the map-plane motion (flatRuntimeMapPlaneFrame)
#include "dlaa.h"
#include "flat_sharpen.h"
#include "flat_ui_layer_math.h"   // fix.ui_quality's flat layer: the target class and family rule its decision asks
#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "../common/temporal_mode.h"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <memory>

namespace edvr {
void FlatMapBounceD3DDriver::retain(uintptr_t resource, uintptr_t context) {
    reinterpret_cast<ID3D11Resource*>(resource)->AddRef();
    reinterpret_cast<ID3D11DeviceContext*>(context)->AddRef();
}
void FlatMapBounceD3DDriver::release(uintptr_t resource, uintptr_t context) {
    reinterpret_cast<ID3D11DeviceContext*>(context)->Release();
    reinterpret_cast<ID3D11Resource*>(resource)->Release();
}
uint64_t FlatMapBounceD3DDriver::clockTicks() {
    LARGE_INTEGER value{}; QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart);
}
uint64_t FlatMapBounceD3DDriver::ticksPerSecond() {
    static const uint64_t frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value);
        return static_cast<uint64_t>(value.QuadPart); }();
    return frequency;
}
bool FlatMapBounceD3DDriver::verify(void* real, const void* cached, size_t bytes) {
    return std::memcmp(real, cached, bytes) == 0;
}
std::atomic<bool> g_flatRuntimeLive{false};
namespace {
std::atomic<bool> nativeScale{false};
std::atomic<uint64_t> mapBouncePresentEpoch{1};
std::atomic<bool> foreignWork{false};
std::atomic<bool> overlaySuffixActive{false};
std::atomic<bool> foregroundProbeActive{false};
std::atomic<bool> foregroundDomainActive{false};
std::atomic<bool> untrustedCoverageActive{false};
FlatCaptureRequest projectionAuditRequested;
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
// F10-only ingress audit. Keep the counters outside State: a draw on a foreign
// thread must still explain why the runtime did not see it. All draw-side work
// before the bounded witnesses is an atomic load and, while armed, one add.
struct DrawIngressAudit {
    std::atomic<bool> active{false};
    std::atomic<uint64_t> entered{0}, inactive{0}, inactiveLiveOff{0}, inactiveInternal{0};
    std::atomic<uint64_t> wrongThread{0}, wrongContext{0};
    std::atomic<uint64_t> paused{0}, accepted{0}, relevant{0};
    std::atomic<uint32_t> inactiveWitnesses{0}, wrongThreadWitnesses{0}, wrongContextWitnesses{0};
    std::atomic<uint32_t> firstAcceptedWitness{0}, relevantWitnesses{0};
    uint32_t framesLeft = 0; // Present-owner only
};
DrawIngressAudit drawIngressAudit;
void reportDrawIngress(const char* event, bool finish = true) {
    auto& a = drawIngressAudit;
    if (finish) {
        if (!a.active.exchange(false, std::memory_order_acq_rel)) return;
    } else if (!a.active.load(std::memory_order_acquire)) return;
    const auto wrongThread = a.wrongThread.load(std::memory_order_relaxed);
    const auto wrongContext = a.wrongContext.load(std::memory_order_relaxed);
    const auto relevant = a.relevant.load(std::memory_order_relaxed);
    Log::get().note("flat draw ingress audit: event=%s entered=%llu inactive=%llu inactive-live-off=%llu inactive-internal=%llu wrong-thread=%llu wrong-context=%llu paused=%llu accepted=%llu relevant=%llu inactive-witnesses=%u wrong-thread-witnesses=%u wrong-context-witnesses=%u wrong-thread-suppressed=%llu wrong-context-suppressed=%llu first-accepted-witness=%u relevant-witnesses=%u relevant-suppressed=%llu; F10-only, counts include early exits before frame reduction",
        event,
        (unsigned long long)a.entered.load(std::memory_order_relaxed),
        (unsigned long long)a.inactive.load(std::memory_order_relaxed),
        (unsigned long long)a.inactiveLiveOff.load(std::memory_order_relaxed),
        (unsigned long long)a.inactiveInternal.load(std::memory_order_relaxed),
        (unsigned long long)wrongThread, (unsigned long long)wrongContext,
        (unsigned long long)a.paused.load(std::memory_order_relaxed),
        (unsigned long long)a.accepted.load(std::memory_order_relaxed),
        (unsigned long long)relevant,
        std::min(1u, a.inactiveWitnesses.load(std::memory_order_relaxed)),
        std::min(1u, a.wrongThreadWitnesses.load(std::memory_order_relaxed)),
        std::min(2u, a.wrongContextWitnesses.load(std::memory_order_relaxed)),
        (unsigned long long)(wrongThread > 1 ? wrongThread - 1 : 0),
        (unsigned long long)(wrongContext > 2 ? wrongContext - 2 : 0),
        std::min(1u, a.firstAcceptedWitness.load(std::memory_order_relaxed)),
        std::min(3u, a.relevantWitnesses.load(std::memory_order_relaxed)),
        (unsigned long long)(relevant > 3 ? relevant - 3 : 0));
    if (finish) a.framesLeft = 0;
}
void armDrawIngress() {
    auto& a = drawIngressAudit;
    reportDrawIngress("rearmed");
    a.entered.store(0, std::memory_order_relaxed);
    a.inactive.store(0, std::memory_order_relaxed);
    a.inactiveLiveOff.store(0, std::memory_order_relaxed);
    a.inactiveInternal.store(0, std::memory_order_relaxed);
    a.wrongThread.store(0, std::memory_order_relaxed);
    a.wrongContext.store(0, std::memory_order_relaxed);
    a.paused.store(0, std::memory_order_relaxed);
    a.accepted.store(0, std::memory_order_relaxed);
    a.relevant.store(0, std::memory_order_relaxed);
    a.inactiveWitnesses.store(0, std::memory_order_relaxed);
    a.wrongThreadWitnesses.store(0, std::memory_order_relaxed);
    a.wrongContextWitnesses.store(0, std::memory_order_relaxed);
    a.firstAcceptedWitness.store(0, std::memory_order_relaxed);
    a.relevantWitnesses.store(0, std::memory_order_relaxed);
    a.framesLeft = 900;
    a.active.store(true, std::memory_order_release);
    Log::get().note("flat draw ingress audit: event=armed frames=900; F10 scope entries and guard reasons are counted even when no draw reaches the reducer");
}
void drawIngressIdentity(ID3D11DeviceContext* context, const char* reason, DWORD ownerThread,
                         ID3D11DeviceContext* runtimeContext, uint64_t frame, uint32_t work,
                         bool live, bool internal,
                         std::atomic<uint32_t>& witnessCount, uint32_t limit) {
    if (witnessCount.fetch_add(1, std::memory_order_relaxed) >= limit) return;
    Ptr<IUnknown> selfIdentity, runtimeIdentity, selfDeviceIdentity, runtimeDeviceIdentity;
    Ptr<ID3D11Device> selfDevice, runtimeDevice;
    if (context) {
        context->QueryInterface(IID_PPV_ARGS(&selfIdentity));
        context->GetDevice(&selfDevice);
        if (selfDevice) selfDevice->QueryInterface(IID_PPV_ARGS(&selfDeviceIdentity));
    }
    if (runtimeContext) {
        runtimeContext->QueryInterface(IID_PPV_ARGS(&runtimeIdentity));
        runtimeContext->GetDevice(&runtimeDevice);
        if (runtimeDevice) runtimeDevice->QueryInterface(IID_PPV_ARGS(&runtimeDeviceIdentity));
    }
    Log::get().note("flat draw ingress witness: reason=%s frame=%llu self=%p runtime-context=%p draw-thread=%lu owner-thread=%lu work=%u live=%u internal=%u self-IUnknown=%p runtime-IUnknown=%p self-device=%p runtime-device=%p self-device-IUnknown=%p runtime-device-IUnknown=%p; COM identities distinguish pointer aliases from different contexts/devices; null runtime fields on a foreign thread are deliberately not queried",
        reason, (unsigned long long)frame, context, runtimeContext,
        (unsigned long)GetCurrentThreadId(), (unsigned long)ownerThread, work,
        live ? 1u : 0u, internal ? 1u : 0u,
        selfIdentity.Get(), runtimeIdentity.Get(), selfDevice.Get(), runtimeDevice.Get(),
        selfDeviceIdentity.Get(), runtimeDeviceIdentity.Get());
}
// The camera table (flat_camera_table.h): the constant buffers bound to VS b1 that can carry the camera rows, and
// the draw path's kept answer from them. An entry is read through a const pointer here and changed only through
// the table's own operations, each of which invalidates that kept answer: nothing below assigns to one.
using CameraTable = FlatCameraTable<Ptr<ID3D11Buffer>>;
using Camera = CameraTable::Entry;
struct View {
    void* identity = nullptr; uint32_t generation = 0; ResourceInfo info{};
    Ptr<IUnknown> held;
};
struct State {
    FlatDrawCapture drawCapture;
    FlatDrawPacketCapture drawPackets;
    std::map<std::pair<uint64_t,uint64_t>,uint64_t> drawPacketRefusedPairs;
    uint64_t drawPacketHistoryEvictions=0;
    unsigned drawPacketsReported=0;
    Ptr<ID3D11DeviceContext> drawPacketContext;
    Ptr<ID3D11Texture2D> drawPacketOutput;
    DWORD drawPacketThread=0;
    uint64_t drawPacketFrame=0;UINT drawPacketSequence=0;
    bool drawPacketOnlyObserved=false;
    FlatWeaponFootprint weaponFootprint;
    FlatOverlayLayer overlay;
    uint64_t overlayReplayCandidates = 0, overlayReplayCompleted = 0;
    std::map<std::string,uint64_t> overlayReplayRefusals;
    // Session-bounded original-byte export outcomes; never repeat a failed
    // export each draw/window, and never change shader admission.
    struct ReplayShaderCapture {
        bool vsSaved=false,psSaved=false,qualified=false;
        std::string reason;
    };
    std::map<std::pair<uint64_t,uint64_t>,ReplayShaderCapture> overlayReplayShaderCaptures;
    bool overlayReplayShaderCaptureCapReported=false;
    FlatForegroundProbe foreground;
    struct DomainFailure {
        uint64_t frame=~0ull,vs=0,ps=0,camera=0;uint32_t q=0,format=0;
        const void* depth=nullptr;const char* stage="never-failed";const char* reason="none";
        EdvrFlatForegroundStateReceipt state{};
        EdvrFlatForegroundBudgetReceipt budget{};
    } foregroundFirstFailure;
    // Every distinct refusal on a candidate in one frame, not only the first:
    // one flight then names every remaining blocker on the selected H.
    struct DomainFailureKinds {
        struct Kind {uint64_t vs=0,ps=0;const char* stage=nullptr;const char* reason=nullptr;uint32_t count=0,firstQ=0,format=0;};
        std::array<Kind,8> kinds{};unsigned used=0,dropped=0;uint64_t frame=~0ull;
        void note(uint64_t f,uint64_t vs,uint64_t ps,const char* stage,const char* reason,uint32_t q,uint32_t format) {
            if(frame!=f){frame=f;used=dropped=0;}
            for(unsigned i=0;i<used;++i)if(kinds[i].vs==vs && kinds[i].ps==ps &&
                (kinds[i].reason==reason || (kinds[i].reason && reason && std::strcmp(kinds[i].reason,reason)==0))){++kinds[i].count;return;}
            if(used==kinds.size()){++dropped;return;}
            kinds[used++]={vs,ps,stage,reason,1,q,format};
        }
    } foregroundFailureKinds;
    // A draw refused its capture whose pixels the owner plane marks first-person is not a frame refusal (FlatForegroundMotion::coverDraw):
    // its kind is kept here, apart from the frame refusals above, so the two inventories stay readable. The latest frame that had any.
    DomainFailureKinds foregroundCoveredKinds;
    DomainFailure foregroundFirstCovered;
    struct DomainCandidate {
        Ptr<ID3D11Texture2D> depth;
        uint64_t frame=~0ull;
        FlatForegroundMotion motion;
        bool colorWritten=false;
        FlatDomainPendingNull pendingNull;
        Ptr<ID3D11Resource> hdr;
        DomainFailure firstFailure{};
        DomainFailureKinds failureKinds{};
        DomainFailure firstCovered{};
        DomainFailureKinds coveredKinds{};
        // First-person (foreign) draws the domain planned into this depth this frame: the copy route's second witness that the frame is
        // mixed-camera (flatCopyMixedCamera), for a weapon whose draws the model cannot see. Zeroed with the frame.
        uint32_t foreignPlanned=0;
        void beginFrame(uint64_t next) {
            if(frame==next)return;
            frame=next;colorWritten=false;foreignPlanned=0;
            pendingNull.beginFrame(next);hdr.Reset();firstFailure={};firstCovered={};
            motion.beginFrame(static_cast<unsigned>(next));
        }
    };
    static constexpr unsigned kDomainCandidateCap=4;
    FlatDomainDepthRoute<kDomainCandidateCap> foregroundRoute;
    std::array<DomainCandidate,kDomainCandidateCap> foregroundCandidates;
    FlatForegroundMotion::CaptureStats foregroundRetiredCaptureStats;
    // Section 104, the pistol's no-candidate bursts: the cumulative counters as of the last `flat foreground no-candidate 5s:` line (the
    // line prints the window's), and the example lines said this session (each kind is capped).
    FlatForegroundMotion::CaptureStats foregroundMissReported;
    unsigned historyExampleLines=0;
    uint32_t foregroundMissExampleLines=0,foregroundIdentityExampleLines=0;
    // Section 104, the training mission's turned-left view: the spell tracker (flat_source_spell.h), and what the last frame refused for
    // no motion source held on the scene's depth.
    FlatSourceSpell sourceSpell;
    FlatMonoSourceless sourcelessLast;
    uint64_t sourcelessLastFrame=0;
    // What a SOURCE-FREE frame (selected, no pool draw: section 104) held on the scene's depth, and the 5 s window's tally of how many of
    // those frames held nothing but hologram-family draws (the loading screen's hologram ghost, docs\design-flat-ui-quality-2026-10-05.md).
    FlatMonoSourceless sourceFreeLast;
    uint64_t sourceFreeLastFrame=0, sourceFreeSeenWindow=0, sourceFreeEmptyWindow=0, sourceFreeHoloOnlyWindow=0, sourceFreeOtherWindow=0, blankSceneDeclinedWindow=0;
    bool frameSourceFree=false;   // this frame's selection took no motion source and named the world from itself (nameSourceFree)
    uint32_t admissionLines=0;uint64_t lastAdmissionMs=0;   // the overlay admission trace's line budget (see beginActualDraw)
    const void* foregroundSelectedDepth=nullptr;
    struct DomainHReceipt {
        uint64_t frame=~0ull;const void* depth=nullptr;
        unsigned candidates=0,pendingNull=0;bool overflow=false,present=false;
    } foregroundLastH;
    uint64_t foregroundDomainFrame=~0ull;
    uint32_t namedWorldQ=0;
    const char* foregroundHRefusal=nullptr;
    const char* foregroundHRefusalWindow=nullptr;   // the most recent H refusal since the last 5 s line (survives the frame starts)
    uint32_t pendingNullMismatchLogged=0;           // "flat foreground pending-null mismatch" lines said this session (12 at most)
    struct DomainProofEntry { FlatDomainShaderProof proof;uint64_t attemptedFrame=0; };
    std::map<std::pair<uint64_t,uint64_t>,DomainProofEntry> foregroundProofs;
    struct DomainCounts {
        uint64_t foreignSeen=0,captured=0,worldMarkers=0,nullMarkers=0,markerRefused=0;
        uint64_t hAttempts=0,hQualified=0;
        uint64_t predictedWorld=0,worldUnmarked=0,surfacePreserving=0,surfacePreservingForeign=0;
        // Draws refused their capture and covered per pixel, and the qualified frames that held at least one: the frames the whole-frame
        // refusal would have lost (design section 104, the grenade hold).
        uint64_t coveredDraws=0,hCoveredFrames=0;
    } foregroundCounts;
    // The prep's refusal census in flat: sampled for a bounded window after the census key (NumLock), so the per-class
    // line names what the finish shows raw instead of the backend's result.
    uint32_t refusalCensusFrames = 0;
    // The camera term's translation precision (design doc section 104): row 275 is the render origin the camera term subtracts
    // frame to frame (cameraBefore, flat_mono_shader_source.h). One 5 s window over the non-reset frames handed to the resolver.
    struct CameraOriginWindow { uint64_t frames = 0, moved = 0; float maxAbs = 0, maxStep = 0, minStep = 0; float last[3] = {}; };
    CameraOriginWindow origin;
    // The last named world camera's near plane and projection scales. Elite draws world-camera depth prepasses on H before the first
    // supported material draw names the world; a draw with this near AND this projection scale (flatDomainPredictsWorld) is provisionally
    // world and must match the selected world camera at H (FlatDomainPendingNull), else H refuses. Persists across frames.
    FlatDomainWorldReference worldReference;
    // Draws whose near plane was the reference's and whose projection scale was not (aiming down sights: the weapon camera takes the
    // world's near): classified as first-person draws, not as the world. One 5 s window, on the `flat foreground SDK domain` line.
    uint64_t predictedScaleRejectedWindow=0;
    FlatUntrustedCoverage untrusted;
    bool untrustedUnknown = false;
    bool untrustedSupportedAlternate = false;
    FlatUntrustedQualification untrustedQualification{};
    bool untrustedQualificationCalled = false;
    bool untrustedQualificationFailed = false;
    std::string untrustedLastQualification = "not-called";
    FlatUntrustedDiagnosticBudget untrustedDiagnosticBudget;
    FlatUntrustedObservedCamera unclassifiedPool[64]{};
    FlatUntrustedObservedCamera unclassifiedOverflowFirst{};
    uint32_t unclassifiedPoolUsed = 0;
    bool unclassifiedPoolOverflow = false;
    struct UntrustedNomineeDiagnostic {
        uint64_t vs = 0, ps = 0;
        uint32_t bits = 0;
    } untrustedNomineeDiagnostics[32]{};
    uint32_t untrustedNomineeDiagnosticsUsed = 0, untrustedNomineeDiagnosticsDropped = 0;
    bool untrustedStickyDiagnostic[2]{};
    struct UnclassifiedConsumerDiagnostic {
        uint64_t vs = 0, ps = 0;
        uint32_t bits = 0;
    } unclassifiedConsumerDiagnostics[16]{};
    uint32_t unclassifiedConsumerDiagnosticsUsed = 0, unclassifiedConsumerDiagnosticsDropped = 0;
    bool unclassifiedOverflowLogged = false;
    uint64_t untrustedAccepted = 0, untrustedRefused = 0, untrustedTreated = 0;
    uint64_t untrustedWorldExcluded = 0;
    uint32_t untrustedLines = 0;
    bool overlayFailureNoted = false;
    uint64_t overlayPlannedWindow = 0, overlayMarkedWindow = 0;
    uint64_t overlayIsolatedWindow = 0, overlayRefusedWindow = 0;
    std::map<std::string,uint64_t> overlayRefusalWindow;
    std::map<std::string,uint64_t> overlayMutationWindow;
    FlatOverlayBlendDiagnostic overlayBlendDiagnostic{};
    uint64_t sourceWitnessFirstFrame = 0;
    uint32_t sourceWitnessCaptured = 0;
    uint64_t sourceWitnessEligibleWindow = 0, sourceWitnessAmbiguousWindow = 0;
    DWORD thread = 0; Ptr<ID3D11Device> device; Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11Texture2D> output, sceneDepth; Ptr<ID3D11ShaderResourceView> depthView;
    FlatRuntimePrefix prefix{}; CameraTable cameras;
    // 0 Rtv0, 1 Dsv0, then pixel-shader t0..t3: the tone and copy draws read t0 and t1, the HDR route's trigger detector all four.
    View views[6]{}; D3D11_VIEWPORT viewport{}; UINT viewportCount = 0;
    Ptr<ID3D11Resource> colors[128], depths[128], previousColor;
    Ptr<ID3D11Resource> uavs[8];
    const void* namedDepth = nullptr, *namedConstants = nullptr;
    unsigned char namedCamera[kFlatCameraBytes]{};
    // The draw that named the world this frame (its shaders), and how often the camera it named was not the one H selected (design
    // section 104, the grenade hold: a first-person camera that names the world makes every world draw after it a "foreign" draw).
    uint64_t namedVs = 0, namedPs = 0;
    uint64_t namedNotSelected = 0;
    uint32_t namedNotSelectedLogged = 0;
    // Why a frame's untrusted-camera accounting failed (untrustedUnknown set): table overflow, an alternate draw no bucket completed, a
    // same-depth draw without a camera, no alternate bucket to select. Events since the process started.
    uint64_t untrustedUnknownCause[4] = {};
    // THE NAMING VETO (design section 104, the grenade hold). Naming takes the first supported scene draw that is not one of the five weapon
    // vertex shaders (weaponMotionFamilyVs), whatever its camera; a grenade is not one of them, and drawn before the world it named the first
    // person's camera (near 0.0675 for the world's 0.025) for runs of 36 and 28 frames. A pre-naming scene draw whose camera is not the last
    // world's (worldReference: flatDomainPredictsWorld, its near plane and both scales) is not a source candidate, as a weapon-family draw is
    // not; the draw that names the world then is the world's. worldReference is the last named world's, and the camera H selected for a
    // frame replaces it (the selector's choice is the world, naming's is a guess): a reference a first-person draw once set is put right by
    // the next H. A world camera that really changed would veto the world itself; three frames in a row that vetoed and never named give the
    // reference up (kFlatNamingVetoFrames).
    bool namingVetoedThisFrame = false;
    uint32_t namingVetoStreak = 0, namingVetoLogged = 0;
    uint64_t namingVetoes = 0, namingVetoReleases = 0;
    FlatMonoFrame previous{}; bool havePrevious = false, treated = false;
    std::string mode; FlatMonoResolveMode engine = FlatMonoResolveMode::Taa;
    unsigned preset = ~0u;
    uint64_t lastMs = 0, lastReport = 0, accepted = 0, refused = 0;
    uint64_t acceptedResetWindow = 0, acceptedHistoryWindow = 0;
    uint64_t resetMissingWindow = 0, resetGapWindow = 0, resetDepthWindow = 0;
    uint64_t resetColorWindow = 0, resetExtentWindow = 0;
    uint64_t presentNotOkWindow = 0;
    uint32_t presentNotOkLogged = 0;
    uint64_t streak = 0, longestStreak = 0;
    std::map<std::string, uint64_t> refusedWindow;
    std::map<std::string, uint64_t> conflictWindow;
    uint32_t conflictDetails[static_cast<uint32_t>(FlatRuntimeConflict::Count)]{};
    uint64_t lastConflictDetailMs[static_cast<uint32_t>(FlatRuntimeConflict::Count)]{};
    const char* reason = "warming-current-frame";
    std::unique_ptr<FlatProjectionRuntime> projection;
    Ptr<ID3D11DeviceContext1> projectionContext;
    uint32_t projectionFrames = 0;
    uint64_t projectionDraws = 0, projectionDispatches = 0, projectionCandidates = 0;
    uint64_t projectionReady = 0, projectionMissing = 0, projectionUnowned = 0, projectionUnknown = 0;
    uint64_t projectionUnchanged = 0;
    uint64_t projectionViewportChecks = 0, projectionViewportMismatches = 0;
    uint64_t projectionViewportWitnesses = 0, projectionViewportSuppressed = 0, projectionViewportUnrecorded = 0;
    struct AuditDetail { uint64_t vs = 0, ps = 0, cs = 0; uint32_t reason = 0; };
    AuditDetail projectionDetails[32]{}; uint32_t projectionDetailsUsed = 0;
    struct AuditOutcome {
        uint64_t vs = 0, ps = 0, cs = 0, observations = 0; uint32_t reason = 0;
        uint64_t canonical = 0, basisMatch = 0, unmatched = 0, unavailable = 0, unsupported = 0;
        uint64_t residualSamples = 0;
        double spatialDepthError = 0, translationResidual = 0;
        uint64_t viewportDepthClamped = 0, viewportOther = 0;
    };
    AuditOutcome projectionOutcomes[256]{}; uint32_t projectionOutcomesUsed = 0;
    uint64_t projectionOutcomeOverflow = 0;
    // Generic shader classification memo: fixed-size, linear scan, no
    // allocation on the draw path; classification runs at most once per
    // (vs,ps) pair per session. Overflow keeps the existing refuse path.
    struct GenericClassification {
        uint64_t vs = 0, ps = 0;
        FlatShaderPairClassification classification{};
        uint64_t lastSeenFrame = 0;
    };
    GenericClassification genericClassifications[64]{};
    uint32_t genericClassificationsUsed = 0;
    // The memo retires its least-recently-seen entry at 64 rather than
    // refusing ever after; the first eviction of a session is logged once.
    bool genericClassificationOverflowLogged = false;
    // Verdict-log dedup, independent of the 64-entry memo (gate-2 review):
    // a pair's verdict logs once per session even across eviction, from a
    // 256-entry FIFO of pair hashes replaced round-robin.
    uint64_t classificationLogged[256]{};
    uint32_t classificationLoggedNext = 0;
    struct LocalProjectionSample {
        uint64_t firstFrame = 0, frames[2]{};
        const void* color[2]{}, *depth[2]{};
        bool closed[2]{};
        uint32_t attempts = 0, complete = 0;
    };
    LocalProjectionSample localSamples[2]{};
    FlatCameraProbe cameraProbe{};
    struct CopyProvenanceSample {
        uint64_t firstFrame = 0, frames[2]{};
        uint32_t attempts = 0, completed = 0, missingSource = 0, missingDestination = 0;
        uint32_t actualMismatch = 0, rearmedBeforeComplete = 0;
    } copyProvenance{};
    struct UnknownProjectionPair { uint64_t vs=0,ps=0; } unknownProjectionPairs[64]{};
    uint32_t unknownProjectionPairsUsed=0;
    uint64_t unknownProjectionCaptureOverflow=0;
    uint32_t unknownProjectionAutomatic=0,unknownProjectionAudit=0;
    uint32_t unknownProjectionBytesSaved=0,unknownProjectionBytesFailed=0,unknownProjectionStagesAbsent=0;
    uint64_t hdrCopiesAccepted=0,hdrCopiesRefused=0;
    uint64_t menuCopiesAccepted=0,menuCopiesRefused=0;
    uint64_t staticSceneFrames=0;
    FlatMonoResolvePreflight plannedResolve{};
    FlatMonoResolvePreflightResult resolvePreflight{};
    bool haveResolvePlan = false;
    uint64_t resolvePreflightRetryMs = 0;
    uint64_t spatialFallbacks = 0, spatialFallbackFailures = 0;
    FlatLivePhase phase;
    Ptr<ID3D11Resource> phaseDepth,phaseHdr;
    bool frameCoverage = true, temporalAccepted = false;
    // advanced.temporal_aa_jitter_phases as of the last read (8 to 64, default 8), and the one value the log has said it could not use,
    // so a bad value is said once and not every frame. The count a frame runs is flatCameraPhaseCount(route, jitterPhases).
    uint32_t jitterPhases = kTemporalJitterCount;
    bool jitterPhasesBadSaid = false; int jitterPhasesBad = 0;
    uint32_t phaseWidth = 0, phaseHeight = 0;
    uint64_t jitteredFrames = 0, jitterDraws = 0, jitterDispatches = 0, jitterRefusals = 0;
    const char* jitterReason = "warming";
    // The camera injector's rows bookkeeping (docs/design-flat-camera-integration.md,
    // the C3 wiring), one 5s window at a time. previousRows* is the phase the
    // previous ACCEPTED frame's camera rows carried, kept beside s.previous.
    float previousRowsX = 0, previousRowsY = 0;
    uint32_t rowsMismatchLogged = 0;
    bool frameHadPhase = false; // the frame began with a non-zero phase (a failed frame's is zeroed by close time)
    struct RowsWindow {
        uint64_t frames = 0, unjittered = 0, zeroPhase = 0;
        uint64_t legacyAppliedUnderUpstream = 0, legacyPrepSkipped = 0;
        FlatCameraPairStats pairs;
    } rows;
    struct PhaseFailure {
        char reason[64]{};
        uint64_t calls=0, frames=0, treatedFrames=0, acceptedFrames=0;
        uint64_t firstFrame=0, lastFrame=0;
    } phaseFailures[32]{};
    uint32_t phaseFailuresUsed=0;
    uint64_t phaseOverflowCalls=0, phaseOverflowFrames=0, phaseOverflowLastFrame=0;
    uint64_t phaseCensusFrames=0, phaseCensusFailedFrames=0, phaseCensusTreatedFailedFrames=0;
    bool phaseCensusPending=false, phaseCensusFailed=false;

    // --- Partial temporal AA ("local refusal"), redesigned per the 2026-09-26
    // review: one raster phase across shared scene depth/colour. A per-draw-
    // local refusal invalidates the frame's history and returns the runtime
    // to observation until a refusal-free frame requalifies the contract;
    // it is never claimed as treated. See docs/design-flat-temporal-aa-2026-09-23.md.
    // The returned-to-observation state: set by a per-draw-local refusal,
    // cleared by a refusal-free frame. While set, frames run unjittered and
    // the copy-draw treatment is skipped; the contract observation that
    // requalifies keeps running.
    bool observing = false;
    // This frame's copy draw selected through the contract observation --
    // the positive witness the observation exit predicate requires
    // (flat_local_reject.h). Reset every frame.
    bool observingQualifiedHandoff = false;
    // This frame saw a per-draw-local refusal (for the census close-out).
    bool covFrameLocallyRefused = false;
    // The current draw's shader identities, stashed per draw so refuseDraw
    // (State& only) can name the refused pair in the census -- the same
    // convention as s.namedDepth and s.reason.
    uint64_t drawVs = 0, drawPs = 0;

    // Gate 2 step 4's negotiated evaluation size (vendor-queried at plan
    // change): nonzero overrides the route's default E on the resolve frame,
    // but ONLY for the exact (mode, render, output) contract it was
    // negotiated for -- a transition frame must never evaluate with the
    // previous contract's override (the 08:29 ladder refusal).
    uint32_t negotiatedEvalW = 0, negotiatedEvalH = 0;
    FlatMonoResolveMode negotiatedMode = FlatMonoResolveMode::Taa;
    uint32_t negotiatedRenderW = 0, negotiatedRenderH = 0;
    uint32_t negotiatedOutputW = 0, negotiatedOutputH = 0;

    // --- Frame-contract trace (staged-program gate 1) -------------------------
    // The reducer's input events, always recorded into a bounded ring and
    // dumped on the F10 audit arm; the replay rig runs the same reducer over
    // the trace and compares contract hashes. The contract accumulates every
    // copy draw's outcome through the frame and is hashed at seal time.
    // No braces: every member already has a default initializer, and `{}`
    // made MSVC expand all 262,144 events as an aggregate initializer, so
    // this file's compile took ~74 GB of commit and 3.5 min (2026-10-06).
    FlatTraceRing traceRing;
    uint64_t traceDumpFrame = 0;   // an F10 dump waits for full-window frames
    FlatFrameContract traceContract{};

    // --- Partial temporal AA: coverage census (Part B), reset every 5s --------
    uint64_t covSceneDraws = 0, covExact = 0, covGeneric = 0, covInert = 0, covUnchanged = 0;
    uint64_t covLocalRefused = 0, covMemoEvictions = 0;
    uint64_t covFrames = 0, covFramesObserving = 0, covObservationEntries = 0;
    uint64_t covFramesLocallyRefused = 0;
    struct CoverageRefusedPair { uint64_t vs = 0, ps = 0; const char* reason = ""; uint64_t draws = 0; };
    CoverageRefusedPair covRefusedPairs[32]{};
    uint32_t covRefusedPairsUsed = 0;

    // --- Stand-down (flat_standdown.h) ---------------------------------------
    // While every frame is refused for the shape of the post chain the runtime
    // stops its per-draw and per-call work. `work` is the mode of the frame in
    // flight, decided at the Present that started it; `standDown` is the state
    // machine that decides it; frameSeen/frameReason are this frame's verdict on
    // its chain, recorded at the copy draw. A session whose frames are selected
    // never leaves FlatWork::Full, and nothing below the mode checks changes for it.
    FlatStandDown standDown;
    FlatWork work = FlatWork::Full;
    FlatFrameSeen frameSeen = FlatFrameSeen::None;
    FlatMonoReason frameReason = FlatMonoReason::NoOutputCopy;
    bool frameLive = false;      // the frame that just ended was watched (Full or Probe)
    bool enginePaused = false;   // engine motion is configured off for the stand-down

    // --- The HDR route (flat_hdr_route.h, design section 81) ------------------------------------
    // Set at the first Present (the route is always on). The trigger detector runs on every watched frame and the route
    // treats at its trigger; the copy stage leaves the frame to it. Nothing here touches a D3D object.
    FlatHdrKey hdrKey = FlatHdrKey::Off;
    bool hdrKeyRead = false;
    FlatHdrFrame hdr{};              // this frame's detector state, reset at the Present that starts the frame
    FlatHdrWindow hdrWindow{};       // the 5 s window of the census token
    FlatHdrSteps hdrStepsSeen{};     // the resolver's cumulative step counts at the last window (the window prints the difference)
    FlatHdrLatch hdrLatch{};         // three treated frames with late writes turn the route off until the key flips
    FlatMonoFrame hdrSelected{};     // the selection at this frame's trigger (kept here: the draw scope is built per draw)
    bool hdrTreated = false;         // this frame's treatment was the route's: the copy stage must not treat it again
    bool hdrFirstTriggerLogged = false, hdrLateLogged = false;
    uint32_t hdrFlightLines = 0;     // bounded per-session log lines about the route's own decisions

    // --- The final copy's admission by structure (flat_copy_structure.h, design section 83) ------------------------
    // Where the whitelist refuses a copy for its tone pass and the HDR route does not serve the frame (R < D, EDVR's TAA
    // above D, a latched route), the copy is admitted by what it is. Active with the route's key auto; with it off the
    // copy route is the whitelist alone, as before, and the admission only names what it sees (no scene, a render size
    // that does not fit). Nothing here touches a D3D object.
    FlatCopyWindow copyWindow{};     // the 5 s window of the census line
    // The copy route's weapon support (flat_copy_structure.h, flatWeaponRoute): cohort draws flagged, glow draws admitted as
    // alternate HDR writers, frames selected mixed-camera and the foreground contract's H qualification at the copy.
    FlatCopyWeaponWindow copyWeaponWindow{};
    bool copyFirstLogged = false;    // the once-a-session first admission line
    uint32_t copyDeclineLines = 0;   // bounded per-session decline lines, each cause once
    const char* copyDeclineSeen[12]{};

    // --- CPU and GPU census (flat_cpu.h) ------------------------------------------
    // What EDVR's own flat work costs, by family, printed every 5 s while a temporal
    // mode is selected. The GPU spans are timestamp pairs read back without waiting:
    // the whole frame, first game draw to Present, and the resolver's dispatches plus
    // backend call. A timer is owned until its sample is read; with none free the
    // frame is skipped and counted.
    flatcpu::Census census{flatcpu::kRenderPeriod, flatcpu::clocksWanted()};
    static constexpr int kGpuFrameTimers = 4, kGpuResolveTimers = 2;
    GpuTimer gpuFrameTimer[kGpuFrameTimers];
    GpuTimer gpuResolveTimer[kGpuResolveTimers];
    bool gpuFrameBusy[kGpuFrameTimers] = {}, gpuResolveBusy[kGpuResolveTimers] = {};
    int gpuFrameOpen = -1, gpuResolveOpen = -1;   // the timer holding this frame's open span
    bool gpuFrameTried = false;                   // this frame already tried to open its span
    bool censusHooked = false;                    // the resolver's span hooks are installed
    bool crumbGateRead = false;                   // the HDR route's breadcrumbs gate has been set from the DXMT markers (once, first)
};
// Driver objects retire on the owner Present; never release under loader lock.
State& state() { static State* p = new State; return *p; }
// THE SYSTEM MAP'S PLANE (FlatMonoResolveFrame::mapPlane) AND THE MAP FAMILIES (flat_ui_layer_math.h flatUiMapFamilyOf).
// Status.json's GuiFocus 7 is the System Map, 6 the Galaxy Map, 8 the Orrery. The flat runtime reads the journal watcher's
// flat reader (journalFlatGuiFocus) ONCE per frame, at the frame boundary (flatRuntimeMapFocusFrame), and every answer the
// frame gives comes from that read: the map plane's flag (what the resolver's map-plane reduction and the prep's midpoint
// motion wait on; nothing else reads it) and the map families' gate (flatRuntimeMapOpenFrame). A frame's answer does not
// change mid-frame. Each change of the map plane's answer is logged once.
struct MapPlaneWatch {
    bool open = false;          // the System Map open, this frame's answer (GuiFocus 7)
    bool focusKnown = false;    // the watcher has reported GuiFocus at least once this session
    uint32_t focus = 0;         // the last GuiFocus it reported, while known
    bool frameKnown = false;    // this frame's read: the watcher reported a GuiFocus
    uint32_t frameFocus = 0;    // ...and its value, while known
    uint32_t noted = 0;         // transition lines written
};
MapPlaneWatch& mapPlaneWatch() { static MapPlaneWatch* p = new MapPlaneWatch; return *p; }
constexpr uint32_t kMapPlaneNoteCap = 64;
void flatRuntimeMapFocusFrame() {
    MapPlaneWatch& w = mapPlaneWatch();
    uint32_t focus = 0;
    const bool known = journalFlatGuiFocus(&focus);
    if (known) { w.focusKnown = true; w.focus = focus; }
    w.frameKnown = known;
    w.frameFocus = known ? focus : 0;
    const bool open = known && focus == 7;
    if (open != w.open) {
        w.open = open;
        if (w.noted < kMapPlaneNoteCap) {
            ++w.noted;
            if (open)
                Log::get().note("flat map motion: the System Map is open (Status.json GuiFocus 7); pixels with no depth take the map plane's motion");
            else if (known)
                Log::get().note("flat map motion: the System Map is closed (GuiFocus %u); pixels with no depth are at infinity again", focus);
            else
                Log::get().note("flat map motion: the System Map is closed (GuiFocus unknown); pixels with no depth are at infinity again");
        }
    }
}
// The System Map's plane, this frame (the frame's read).
bool flatRuntimeMapPlaneFrame() { return mapPlaneWatch().open; }
// The Galaxy Map (6), the System Map (7) or the Orrery (8) is open, this frame (the frame's read): the map families' gate.
bool flatRuntimeMapOpenFrame() {
    const MapPlaneWatch& w = mapPlaneWatch();
    return w.frameKnown && (w.frameFocus == 6 || w.frameFocus == 7 || w.frameFocus == 8);
}
// Row 275's size and its frame-to-frame step (design doc section 104). The float32 spacing at that size is the finest step the camera
// term can see: a walking step near it reaches the upscaler quantised.
static void noteCameraOrigin(State& s, const FlatMonoResolveFrame& f) {
    if (f.reset) return;
    State::CameraOriginWindow& o = s.origin;
    ++o.frames;
    float step = 0, smallest = 0;
    for (int i = 0; i < 3; ++i) {
        const float a = std::fabs(f.camera[5][i]);
        if (a > o.maxAbs) o.maxAbs = a;
        const float d = std::fabs(f.camera[5][i] - f.previousCamera[5][i]);
        if (d > step) step = d;
        if (d > 0 && (smallest == 0 || d < smallest)) smallest = d;
        o.last[i] = f.camera[5][i];
    }
    if (step > 0) {
        ++o.moved;
        if (step > o.maxStep) o.maxStep = step;
        if (o.minStep == 0 || smallest < o.minStep) o.minStep = smallest;
    }
}
static State::DomainCandidate* domainCandidate(State& s,const void* depth) {
    const int slot=s.foregroundRoute.selected(depth,s.prefix.frame);
    return slot<0?nullptr:&s.foregroundCandidates[slot];
}
static void retireDomainCandidate(State& s,State::DomainCandidate& candidate) {
    s.foregroundRetiredCaptureStats.add(candidate.motion.stats());
    candidate=State::DomainCandidate{};
}
static State::DomainCandidate* observeDomainCandidate(State& s,const void* depth) {
    const auto found=s.foregroundRoute.observe(depth,s.prefix.frame);
    if(found.slot<0)return nullptr;
    auto& candidate=s.foregroundCandidates[found.slot];
    if(found.created)retireDomainCandidate(s,candidate);
    candidate.depth=static_cast<ID3D11Texture2D*>(const_cast<void*>(depth));
    candidate.beginFrame(s.prefix.frame);
    return &candidate;
}
// Only a write to the depth itself (or an unknown one) can change which
// surface a pixel shows; colour copies and compute writes cannot.
// The bytes a copy or an update wrote, when the destination is a buffer and the call says where (section 104, range-aware invalidation).
// A copy with no box writes the whole source into the destination at dstX, so the source's size is the range; anything the call does not say
// is the whole resource.
static uint64_t flatBufferBytes(const void* resource) {
    if(!resource)return 0;
    auto* r=static_cast<ID3D11Resource*>(const_cast<void*>(resource));
    D3D11_RESOURCE_DIMENSION dim=D3D11_RESOURCE_DIMENSION_UNKNOWN;r->GetType(&dim);
    if(dim!=D3D11_RESOURCE_DIMENSION_BUFFER)return 0;
    D3D11_BUFFER_DESC d{};static_cast<ID3D11Buffer*>(r)->GetDesc(&d);return d.ByteWidth;
}
// What a mutation report says about the bytes it wrote: the same extents, from the details the hook already carries.
static HistoryWriteExtent mutationExtent(FlatOverlayMutationOp op,const FlatMutationDetails& d) {
    switch(op) {
    case FlatOverlayMutationOp::Map: case FlatOverlayMutationOp::Unmap: return historyWholeWrite(HistoryWriteEntry::Map);
    case FlatOverlayMutationOp::ClearRtv: case FlatOverlayMutationOp::ClearDsv: case FlatOverlayMutationOp::ClearUav:
        return historyWholeWrite(HistoryWriteEntry::Clear);
    case FlatOverlayMutationOp::CopyResource: return historyWholeWrite(HistoryWriteEntry::CopyResource);
    case FlatOverlayMutationOp::CopyRegion:
        return flatRuntimeCopyExtent(d.dstSub,d.dstX,d.source,d.hasBox?&d.box:nullptr);
    case FlatOverlayMutationOp::UpdateSubresource: return flatRuntimeUpdateExtent(d.dstSub,d.hasBox?&d.box:nullptr);
    default: return historyWholeWrite(HistoryWriteEntry::Other);
    }
}
// `tally` false is a write the mutation report counts too: the prefix model's own notification of the same API call.
static void domainResourceWritten(State& s,ID3D11Resource* resource,const char* depthReason,
                                  const HistoryWriteExtent& extent=HistoryWriteExtent{},bool tally=true) {
    if(!resource)s.foregroundRoute.noteUnknownMutation(s.prefix.frame);
    for(auto& candidate:s.foregroundCandidates)if(candidate.depth) {
        candidate.motion.resourceWritten(resource,extent,tally);
        if(candidate.frame!=s.prefix.frame)continue;
        if(!resource || candidate.depth.Get()==resource)candidate.motion.fail(depthReason);
    }
}
// Section 104, the pistol's no-candidate bursts: why the draws of the last 5 s found no record of their exact geometry key that the frame
// before used (every pattern, zeros included), the draws with priors that the map will not match by identity (the same for the identity
// words), and the first draw of each pattern with its whole key. All of it is read from counters the capture already keeps; the window is
// the difference of the cumulative counters from the last line.
static HistoryWriteStats historyWindowDelta(const HistoryWriteStats& now,const HistoryWriteStats& was) {
    const auto d=[](uint64_t a,uint64_t b){return a>b?a-b:uint64_t(0);};
    HistoryWriteStats w;
    for(unsigned e=0;e<kHistoryWriteEntries;++e) {
        w.observed[e]=d(now.observed[e],was.observed[e]);w.recordsInvalidated[e]=d(now.recordsInvalidated[e],was.recordsInvalidated[e]);
        w.ranged[e]=d(now.ranged[e],was.ranged[e]);w.savedWrites[e]=d(now.savedWrites[e],was.savedWrites[e]);
        for(unsigned r=0;r<kHistoryWriteRoles;++r)for(unsigned t=0;t<kHistoryWriteTimings;++t) {
            w.touching[e][r][t]=d(now.touching[e][r][t],was.touching[e][r][t]);
            w.invalidating[e][r][t]=d(now.invalidating[e][r][t],was.invalidating[e][r][t]);
        }
    }
    for(unsigned t=0;t<kHistoryWriteTimings;++t)w.unknownInvalidating[t]=d(now.unknownInvalidating[t],was.unknownInvalidating[t]);
    w.sparedRecords=d(now.sparedRecords,was.sparedRecords);
    for(unsigned i=0;i<static_cast<unsigned>(HistoryErase::Count);++i)w.erased[i]=d(now.erased[i],was.erased[i]);
    w.allocations=d(now.allocations,was.allocations);w.allocationFailures=d(now.allocationFailures,was.allocationFailures);
    w.extentIssued=d(now.extentIssued,was.extentIssued);w.extentRead=d(now.extentRead,was.extentRead);w.extentFailed=d(now.extentFailed,was.extentFailed);
    w.setFromCache=d(now.setFromCache,was.setFromCache);w.setPending=d(now.setPending,was.setPending);
    w.setApproximate=d(now.setApproximate,was.setApproximate);w.setCancelled=d(now.setCancelled,was.setCancelled);
    w.vertexUnknown=d(now.vertexUnknown,was.vertexUnknown);w.vertexInGap=d(now.vertexInGap,was.vertexInGap);
    w.vertexOutside=d(now.vertexOutside,was.vertexOutside);w.vertexGenuine=d(now.vertexGenuine,was.vertexGenuine);
    w.deferredInvalidated=d(now.deferredInvalidated,was.deferredInvalidated);w.deferredConservative=d(now.deferredConservative,was.deferredConservative);
    return w;
}
static void reportForegroundNoCandidate(State& s,const FlatForegroundMotion::CaptureStats& captures) {
    const auto& was=s.foregroundMissReported;
    const auto delta=[](uint64_t now,uint64_t before){return now>before?now-before:uint64_t(0);};
    FlatNoCandidateWindow w;
    w.submitted=delta(captures.submitted,was.submitted);w.noCandidate=delta(captures.noCandidate,was.noCandidate);
    w.acrossOffset=delta(captures.acrossOffset,was.acrossOffset);w.rescueCancelled=delta(captures.rescueCancelled,was.rescueCancelled);
    w.frames=delta(captures.frames,was.frames);
    w.framesMissing=delta(captures.framesMissing,was.framesMissing);w.framesAllMissing=delta(captures.framesAllMissing,was.framesAllMissing);
    w.resetFrames=delta(captures.resetFrames,was.resetFrames);w.nearChanges=delta(captures.nearChanges,was.nearChanges);
    for(unsigned i=0;i<kHistoryGapCount;++i)w.missBy[i]=delta(captures.missBy[i],was.missBy[i]);
    w.identitySamples=delta(captures.identitySamples,was.identitySamples);
    for(unsigned i=0;i<kIdentityVerdictCount;++i)w.identityBy[i]=delta(captures.identityBy[i],was.identityBy[i]);
    w.allocations=delta(captures.history.allocations,was.history.allocations);
    FlatSiblingWindow sibling;
    sibling.engagedFrames=delta(captures.siblingFrames,was.siblingFrames);sibling.dispatches=delta(captures.siblingDispatches,was.siblingDispatches);
    sibling.readbacks=delta(captures.siblingReads,was.siblingReads);sibling.notReady=delta(captures.siblingNotReady,was.siblingNotReady);
    sibling.failed=delta(captures.siblingFailed,was.siblingFailed);
    for(unsigned p=0;p<kSiblingPatterns;++p)for(unsigned o=0;o<kSiblingOutcomes;++o)sibling.by[p][o]=delta(captures.siblingBy[p][o],was.siblingBy[p][o]);
    struct Taken {FlatForegroundMotion::MissExample miss[FlatForegroundMotion::kMissExamples];unsigned misses=0;
                  FlatIdentitySampler::Sample identity[FlatForegroundMotion::kIdentityExamples];unsigned identities=0;};
    FlatHistoryWindow history;
    history.writes=historyWindowDelta(captures.history,was.history);
    const ShadowStats shadowWindow=flatShadowDelta(captures.shadow,was.shadow);
    HistoryWriteTop tops[kHistoryTopResources*State::kDomainCandidateCap]{};unsigned topCount=0;
    HistoryWriteExample examples[kHistoryWriteCases*kHistoryExamplesPerCase*State::kDomainCandidateCap]{};unsigned exampleCount=0;
    std::array<Taken,State::kDomainCandidateCap> taken{};
    for(unsigned i=0;i<s.foregroundCandidates.size();++i) {
        auto& motion=s.foregroundCandidates[i].motion;
        w.longestRun=(std::max)(w.longestRun,motion.takeLongestMissRun());
        w.identitySkipped+=motion.identitySkipped();w.identityUnread+=motion.identityNotReady();
        taken[i].misses=motion.takeMissExamples(taken[i].miss,FlatForegroundMotion::kMissExamples);
        taken[i].identities=motion.takeIdentityExamples(taken[i].identity,FlatForegroundMotion::kIdentityExamples);
        unsigned peakRecords=0,peakBytes=0;motion.takeHistoryPeaks(peakRecords,peakBytes);
        history.records+=motion.historyRecords();history.bytes+=motion.historyBytes();
        history.peakRecords+=peakRecords;history.peakBytes+=peakBytes;
        topCount+=motion.takeWriteTop(tops+topCount,kHistoryTopResources);
        exampleCount+=motion.takeWriteExamples(examples+exampleCount,kHistoryWriteCases*kHistoryExamplesPerCase);
    }
    // The three resources written most, over the candidates.
    std::sort(tops,tops+topCount,[](const HistoryWriteTop& a,const HistoryWriteTop& b){return a.touching>b.touching;});
    for(unsigned i=0;i<topCount && history.topCount<3;++i)history.top[history.topCount++]=tops[i];
    w.records=history.records;w.bytes=history.bytes;w.peakRecords=history.peakRecords;w.peakBytes=history.peakBytes;
    s.foregroundMissReported=captures;
    char line[4096];
    for(unsigned part=0;part<kFlatNoCandidateLineParts;++part){flatNoCandidateLine(line,sizeof(line),w,part);Log::get().note("%s",line);}
    flatIdentityLine(line,sizeof(line),w);Log::get().note("%s",line);
    for(unsigned part=0;part<kFlatSiblingLineParts;++part){flatSiblingLine(line,sizeof(line),sibling,part);Log::get().note("%s",line);}
    flatHistoryLine(line,sizeof(line),history);Log::get().note("%s",line);
    for(unsigned part=0;part<kFlatHistoryWriteLines;++part){flatHistoryWritesLine(line,sizeof(line),history,part);Log::get().note("%s",line);}
    flatHistoryVertexLine(line,sizeof(line),history);Log::get().note("%s",line);
    for(unsigned i=0;i<exampleCount && s.historyExampleLines<96;++i,++s.historyExampleLines){
        flatHistoryExampleLine(line,sizeof(line),examples[i]);Log::get().note("%s",line);
    }
    // The shadow is off unless a rig or a build switches it on: no line then, and the lines say so by their absence.
    if(shadowWindow.sampledFrames || shadowWindow.failed || shadowWindow.notReady)
        for(unsigned part=0;part<kFlatShadowLineParts;++part){flatShadowLine(line,sizeof(line),shadowWindow,part);Log::get().note("%s",line);}
    for(const auto& t:taken) {
        for(unsigned i=0;i<t.misses && s.foregroundMissExampleLines<48;++i,++s.foregroundMissExampleLines) {
            const auto& e=t.miss[i];
            flatNoCandidateExampleLine(line,sizeof(line),e.frame,e.miss,e.key,e.rescued,e.vs,e.ps);Log::get().note("%s",line);
        }
        for(unsigned i=0;i<t.identities && s.foregroundIdentityExampleLines<24;++i,++s.foregroundIdentityExampleLines) {
            const auto& e=t.identity[i];
            flatIdentityExampleLine(line,sizeof(line),e.frame,e.verdict,e.current,e.prior,e.priors,e.key,e.vs,e.ps);Log::get().note("%s",line);
        }
    }
}
// Section 104, the training mission's turned-left view: the 5 s window of the source spells and the pairs the last frame refused for no
// source held on the scene's depth (flat_source_spell.h), annotated with what only the runtime knows of each pair.
// A selected frame with no pool draw: the draws on its scene depth, classed. empty = nothing drew on that depth; holo-only = every draw there is a
// hologram family's (kHoloFamiliesBuiltIn, by vertex shader); other = something else drew (a real world: ground, sky, a settlement).
// summarizeSourceless names only the top four pairs, so the class counts draws from those plus the total: holo-only needs the four to
// cover every distinct pair.
// A selected, source-free scene with no world: nothing on its depth but inert full-screen filters (flatProjectionDrawUnchanged: no
// geometry, no projection), or nothing at all. A loading or menu screen. Ground and sky have geometry draws on that depth, so are not blank.
static bool blankScene(const FlatMonoFrame& sel) {
    if(!sel.selected() || !sel.sourceFree) return false;
    const auto& n=sel.sourceless;
    if(!n.draws) return true;
    if(n.distinctPairs>n.topCount) return false;   // a pair beyond the named four is unclassified
    for(uint32_t i=0;i<n.topCount;++i)
        if(n.top[i].pool || !flatProjectionDrawUnchanged(n.top[i].vs,n.top[i].ps)) return false;
    return true;
}
static void noteSourceFreeContent(State& s, const FlatMonoFrame& sel) {
    if(!sel.selected() || !sel.sourceFree) return;
    if(s.sourceFreeLastFrame==s.prefix.frame) return;   // the copy and HDR selections of one frame count once
    s.sourceFreeLast=sel.sourceless; s.sourceFreeLastFrame=s.prefix.frame; ++s.sourceFreeSeenWindow;
    const auto& n=sel.sourceless;
    if(!n.draws) { ++s.sourceFreeEmptyWindow; return; }
    if(blankScene(sel)) ++s.sourceFreeHoloOnlyWindow; else ++s.sourceFreeOtherWindow;   // "filter-only" in the line
}
static void reportSourceSpell(State& s) {
    const FlatSourceSpellWindow w=s.sourceSpell.take();
    if(s.sourceFreeSeenWindow) {
        // The source-free frames' content, one line: the counts, the last frame's tally, and its top pairs (flat_source_spell.h's formatter).
        char pairs[1200]; size_t at=0; pairs[0]=0;
        for(uint32_t i=0;i<s.sourceFreeLast.topCount && i<4;++i) {
            const auto& p=s.sourceFreeLast.top[i];
            const int k=std::snprintf(pairs+at,sizeof(pairs)-at,"%s[VS=%016llX PS=%016llX draws=%u same-camera=%u pool-kind=%u]",i?" ":"",
                (unsigned long long)p.vs,(unsigned long long)p.ps,p.draws,p.sameCamera?1u:0u,p.pool?1u:0u);
            if(k<0 || at+size_t(k)>=sizeof(pairs)) break;
            at+=size_t(k);
        }
        if(!at) std::snprintf(pairs,sizeof(pairs),"none");
        Log::get().note("flat source-free content 5s: frames=%llu empty-scene-depth=%llu filter-only=%llu other-draws=%llu blank-scene-declined=%llu; last frame=%llu: "
            "records-on-scene-depth=%u draws=%u same-camera-draws=%u distinct-pairs=%u; top: %s",
            (unsigned long long)s.sourceFreeSeenWindow,(unsigned long long)s.sourceFreeEmptyWindow,(unsigned long long)s.sourceFreeHoloOnlyWindow,
            (unsigned long long)s.sourceFreeOtherWindow,(unsigned long long)s.blankSceneDeclinedWindow,(unsigned long long)s.sourceFreeLastFrame,
            s.sourceFreeLast.records,s.sourceFreeLast.draws,s.sourceFreeLast.sameCameraDraws,s.sourceFreeLast.distinctPairs,pairs);
        s.sourceFreeSeenWindow=s.sourceFreeEmptyWindow=s.sourceFreeHoloOnlyWindow=s.sourceFreeOtherWindow=s.blankSceneDeclinedWindow=0;
    }
    FlatSourcelessPairNote notes[4]{};
    for(uint32_t i=0;i<s.sourcelessLast.topCount && i<4;++i) {
        notes[i].familyVs=engineVelocityPoolFamilyVs(s.sourcelessLast.top[i].vs);
        notes[i].recipe=flatProjectionDrawRecipes(s.sourcelessLast.top[i].vs,s.sourcelessLast.top[i].ps).count!=0;
    }
    char line[3072];
    for(unsigned part=0;part<kFlatSourceLineParts;++part) {
        flatSourceSpellLine(line,sizeof(line),w,s.sourcelessLast,s.sourcelessLastFrame,notes,part);
        Log::get().note("%s",line);
    }
}
static void reportForegroundDomain(State& s) {
    const auto& n=s.foregroundCounts;
    auto captures=s.foregroundRetiredCaptureStats;
    for(const auto& candidate:s.foregroundCandidates)captures.add(candidate.motion.stats());
    reportForegroundNoCandidate(s,captures);
    // Three lines, because the logger cuts a line at about 1167 characters and this one was 1,300: the first keeps the key
    // `flat foreground SDK domain:`. The explanation that trailed it is in the design doc (section 104): counts cover all depth candidates;
    // scale-rejected-5s is the draws at the world's near plane whose projection scale was not the world's, taken as first-person, since the last
    // line; qualification alone is not a completed SDK call; the history of the submitted draws is cumulative (no-candidate found no record of its
    // geometry from the frame before, no-prior-pool/near/absent had candidates and the adapter passed none on, priors-one/several matched on the
    // GPU by identity, repeated-geometry is the draws after the first of their geometry in a frame).
    Log::get().note("flat foreground SDK domain: configured=%s foreign-seen=%llu captured=%llu capture-attempts=%llu gpu-identity-attempts=%llu gpu-identity-submitted=%llu preflight-refused=%llu warmed-after-refusal=%llu no-candidate=%llu no-prior-pool=%llu no-prior-near=%llu no-prior-absent=%llu priors-one=%llu priors-several=%llu repeated-geometry=%llu",
        flatMonoResolveModeName(s.engine),(unsigned long long)n.foreignSeen,(unsigned long long)n.captured,
        (unsigned long long)captures.attempts,(unsigned long long)captures.gpuAttempts,
        (unsigned long long)captures.submitted,(unsigned long long)captures.preflightRefused,
        (unsigned long long)captures.warmedAfterRefusal,
        (unsigned long long)captures.noCandidate,(unsigned long long)captures.noPriorPool,(unsigned long long)captures.noPriorNear,
        (unsigned long long)captures.noPriorAbsent,(unsigned long long)captures.priorsOne,(unsigned long long)captures.priorsSeveral,
        (unsigned long long)captures.repeated);
    Log::get().note("flat foreground SDK domain (2/3): world-markers=%llu null-markers=%llu marker-refused=%llu predicted-world=%llu predicted-near=%.9g scale-rejected-5s=%llu world-unmarked=%llu surface-preserving=%llu (foreign-camera=%llu) H-attempts=%llu H-qualified=%llu",
        (unsigned long long)n.worldMarkers,(unsigned long long)n.nullMarkers,(unsigned long long)n.markerRefused,
        (unsigned long long)n.predictedWorld,s.worldReference.nearPlane,(unsigned long long)s.predictedScaleRejectedWindow,
        (unsigned long long)n.worldUnmarked,
        (unsigned long long)n.surfacePreserving,(unsigned long long)n.surfacePreservingForeign,
        (unsigned long long)n.hAttempts,(unsigned long long)n.hQualified);
    Log::get().note("flat foreground SDK domain (3/3): H-qualified-with-per-pixel-refusals=%llu per-pixel-refused-draws=%llu (occurrence-cap=%llu history-budget=%llu other=%llu) windowed-priors=%llu naming-vetoes=%llu naming-veto-releases=%llu last-refusal=%s",
        (unsigned long long)n.hCoveredFrames,
        (unsigned long long)n.coveredDraws,(unsigned long long)captures.coveredOccurrence,(unsigned long long)captures.coveredBudget,
        (unsigned long long)captures.coveredOther,(unsigned long long)captures.windowed,
        (unsigned long long)s.namingVetoes,(unsigned long long)s.namingVetoReleases,
        s.foregroundHRefusalWindow?s.foregroundHRefusalWindow:"none");
    // last-refusal is the most recent H refusal since the previous line. The per-frame field clears at every frame start, so it read
    // "none" here even while every frame was refused (section 104, supersampled 4K).
    s.foregroundHRefusalWindow=nullptr;
    s.predictedScaleRejectedWindow=0;
    const auto& f=s.foregroundFirstFailure;
    Log::get().note("flat foreground first failure: configured=%s frame=%llu q=%u VS=%016llX PS=%016llX format=%u camera=%016llX depth=%p stage=%s reason=%s pending-world-null=%u selected-H-frame=%llu selected-depth=%p candidates=%u cap=%u overflow=%u candidate-present=%u; first failure on selected H depth in the most recent failed frame, not a shader allowlist",
        flatMonoResolveModeName(s.engine),(unsigned long long)f.frame,f.q,(unsigned long long)f.vs,(unsigned long long)f.ps,
        f.format,(unsigned long long)f.camera,f.depth,f.stage,f.reason,s.foregroundLastH.pendingNull,
        (unsigned long long)s.foregroundLastH.frame,s.foregroundLastH.depth,s.foregroundLastH.candidates,
        State::kDomainCandidateCap,s.foregroundLastH.overflow?1u:0u,s.foregroundLastH.present?1u:0u);
    const auto& st=f.state;
    Log::get().note("flat foreground first failure state: valid=%u frame=%llu q=%u bound=%02X independent=%u alpha-to-coverage=%u sample-mask=%08X depth-enable=%u depth-write=%u depth-func=%u foreign=%u HDR=%u world-named=%u same-world=%u named-world-q=%u",
        st.valid,(unsigned long long)f.frame,f.q,st.boundColors,st.independentBlend,st.alphaToCoverage,
        st.sampleMask,st.depthEnable,st.depthWriteMask,st.depthFunc,st.foreign,st.hdr,
        st.worldNamed,st.sameWorld,st.namedWorldQ);
    Log::get().note("flat foreground first failure stencil: valid=%u frame=%llu q=%u enable=%u read=%02X write=%02X ref=%u front=%u/%u/%u/%u back=%u/%u/%u/%u DSV-flags-valid=%u DSV-flags=%u",
        st.valid,(unsigned long long)f.frame,f.q,st.stencilEnable,st.stencilReadMask,
        st.stencilWriteMask,st.stencilRef,st.front.func,st.front.fail,st.front.depthFail,
        st.front.pass,st.back.func,st.back.fail,st.back.depthFail,st.back.pass,
        st.dsvFlagsValid,st.dsvFlags);
    if(st.valid)for(unsigned i=0;i<6;++i) {
        const auto& slot=st.slot[i];
        if(!(st.boundColors&(1u<<i)) && !slot.componentMask)continue;
        Log::get().note("flat foreground first failure state slot: frame=%llu q=%u slot=%u RTV-format-valid=%u RTV-format=%u PS-components=%X effective-write=%X blend=%u color=%u/%u/%u alpha=%u/%u/%u",
            (unsigned long long)f.frame,f.q,i,slot.viewFormatValid,slot.viewFormat,
            slot.componentMask,slot.effectiveWriteMask,slot.blendEnable,
            slot.src,slot.dst,slot.op,slot.srcAlpha,slot.dstAlpha,slot.opAlpha);
    }
    const auto& b=f.budget;
    Log::get().note("flat foreground first failure budget: valid=%u frame=%llu q=%u requested-bytes=%u record-limit=%u byte-limit=%u records=%u bytes=%u invalid=%u pending=%u current=%u prior=%u older=%u reclaimed-records=%u reclaimed-bytes=%u current-draws=%u previous-draws=%u before-world-current=%u before-world-previous=%u known-mutations=%u unknown-mutations=%u",
        b.valid,(unsigned long long)f.frame,f.q,b.requestedBytes,b.recordLimitHit,b.byteLimitHit,
        b.records,b.bytes,b.invalid,b.pending,b.current,b.prior,b.older,b.reclaimedRecords,
        b.reclaimedBytes,b.currentDraws,b.previousDraws,b.beforeWorldCurrent,b.beforeWorldPrevious,
        b.knownMutations,b.unknownMutations);
    const auto& inventory=s.foregroundFailureKinds;
    Log::get().note("flat foreground refusal inventory: frame=%llu kinds=%u dropped=%u; every distinct refusal on the selected H in that frame, in order of first appearance",
        (unsigned long long)inventory.frame,inventory.used,inventory.dropped);
    for(unsigned i=0;i<inventory.used;++i) {
        const auto& kind=inventory.kinds[i];
        Log::get().note("flat foreground refusal kind: frame=%llu first-q=%u count=%u VS=%016llX PS=%016llX format=%u stage=%s reason=%s",
            (unsigned long long)inventory.frame,kind.firstQ,kind.count,(unsigned long long)kind.vs,(unsigned long long)kind.ps,
            kind.format,kind.stage?kind.stage:"none",kind.reason?kind.reason:"none");
    }
    // The draws that were refused their capture and covered per pixel (the frame stayed qualified): the latest frame that had any.
    // Their pixels are the census's weapon-refused pixels under reason 0 (a pixel with no map sample).
    const auto& covered=s.foregroundCoveredKinds;
    Log::get().note("flat foreground per-pixel refusal inventory: frame=%llu kinds=%u dropped=%u; every distinct draw refused its capture and covered per pixel on a candidate depth in the latest such frame; their pixels are marked first-person with no map sample (census: weapon-refused, reason unspecified)",
        (unsigned long long)covered.frame,covered.used,covered.dropped);
    for(unsigned i=0;i<covered.used;++i) {
        const auto& kind=covered.kinds[i];
        Log::get().note("flat foreground per-pixel refusal kind: frame=%llu first-q=%u count=%u VS=%016llX PS=%016llX format=%u stage=%s reason=%s",
            (unsigned long long)covered.frame,kind.firstQ,kind.count,(unsigned long long)kind.vs,(unsigned long long)kind.ps,
            kind.format,kind.stage?kind.stage:"none",kind.reason?kind.reason:"none");
    }
    const auto& cf=s.foregroundFirstCovered;
    if(cf.frame!=~0ull)
        Log::get().note("flat foreground per-pixel refusal first: frame=%llu q=%u VS=%016llX PS=%016llX format=%u reason=%s budget: valid=%u requested-bytes=%u record-limit=%u byte-limit=%u records=%u bytes=%u invalid=%u pending=%u current=%u prior=%u older=%u reclaimed-records=%u reclaimed-bytes=%u current-draws=%u previous-draws=%u",
            (unsigned long long)cf.frame,cf.q,(unsigned long long)cf.vs,(unsigned long long)cf.ps,cf.format,cf.reason,cf.budget.valid,
            cf.budget.requestedBytes,cf.budget.recordLimitHit,cf.budget.byteLimitHit,cf.budget.records,cf.budget.bytes,cf.budget.invalid,
            cf.budget.pending,cf.budget.current,cf.budget.prior,cf.budget.older,cf.budget.reclaimedRecords,cf.budget.reclaimedBytes,
            cf.budget.currentDraws,cf.budget.previousDraws);
}
static const FlatDomainShaderProof& domainShaderProof(State& s,uint64_t vs,uint64_t ps) {
    const auto key=std::make_pair(vs,ps);auto found=s.foregroundProofs.find(key);
    if(found!=s.foregroundProofs.end() && (found->second.proof.present || found->second.attemptedFrame==s.prefix.frame))
        return found->second.proof;
    if(found==s.foregroundProofs.end() && s.foregroundProofs.size()==1024) {
        static const auto capped=[](){FlatDomainShaderProof p;p.refusal="foreground-shader-proof-cap";return p;}();
        return capped;
    }
    const uint8_t *v=nullptr,*p=nullptr;size_t vn=0,pn=0;
    flatProbeShaderLookup('v',vs,&v,&vn);if(ps)flatProbeShaderLookup('p',ps,&p,&pn);
    auto& entry=s.foregroundProofs[key];entry.proof=flatDomainShaderProof(vs,ps,v,vn,p,pn);entry.attemptedFrame=s.prefix.frame;
    return entry.proof;
}
template<class Key>static void domainFail(State& s,const char* stage,const char* reason,const Key& k,
    const EdvrFlatForegroundStateReceipt* stateReceipt=nullptr,
    const EdvrFlatForegroundBudgetReceipt* budgetReceipt=nullptr) {
    auto* candidate=domainCandidate(s,k.depth);
    const char* why=reason?reason:"foreground-contract";
    if(candidate) {
        candidate->motion.fail(reason);
        candidate->failureKinds.note(s.prefix.frame,k.vs,k.ps,stage,why,s.prefix.sequence,k.format);
        if(candidate->firstFailure.frame!=s.prefix.frame) {
            candidate->firstFailure={s.prefix.frame,k.vs,k.ps,k.cameraHash,s.prefix.sequence,k.format,k.depth,stage,why};
            if(stateReceipt)candidate->firstFailure.state=*stateReceipt;
            if(budgetReceipt)candidate->firstFailure.budget=*budgetReceipt;
        }
    } else if(s.foregroundSelectedDepth==k.depth) {
        s.foregroundHRefusal=reason;
        s.foregroundFailureKinds.note(s.prefix.frame,k.vs,k.ps,stage,why,s.prefix.sequence,k.format);
        if(s.foregroundFirstFailure.frame!=s.prefix.frame) {
            s.foregroundFirstFailure={s.prefix.frame,k.vs,k.ps,k.cameraHash,s.prefix.sequence,k.format,k.depth,stage,why};
            if(stateReceipt)s.foregroundFirstFailure.state=*stateReceipt;
            if(budgetReceipt)s.foregroundFirstFailure.budget=*budgetReceipt;
        }
    }
}
// A draw refused its capture (occurrence cap, history budget, any history-stage reason) whose pixels the owner plane marks first-person: the
// frame stays qualified and the prep refuses those pixels' history (no map sample; never the camera term). Recorded, not refused: the draw's
// kind under the stage "history-covered", and the first of the frame with its receipts.
template<class Key>static void domainCovered(State& s,const char* reason,const Key& k,
    const EdvrFlatForegroundStateReceipt* stateReceipt=nullptr,
    const EdvrFlatForegroundBudgetReceipt* budgetReceipt=nullptr) {
    auto* candidate=domainCandidate(s,k.depth);
    if(!candidate)return;
    const char* why=reason?reason:"foreground-contract";
    ++s.foregroundCounts.coveredDraws;
    candidate->coveredKinds.note(s.prefix.frame,k.vs,k.ps,"history-covered",why,s.prefix.sequence,k.format);
    if(candidate->firstCovered.frame!=s.prefix.frame) {
        candidate->firstCovered={s.prefix.frame,k.vs,k.ps,k.cameraHash,s.prefix.sequence,k.format,k.depth,"history-covered",why};
        if(stateReceipt)candidate->firstCovered.state=*stateReceipt;
        if(budgetReceipt)candidate->firstCovered.budget=*budgetReceipt;
    }
}
// A pre-naming scene draw whose camera is not the last world's cannot name the world (see State::namingVetoedThisFrame). The draw is then no
// source candidate: a first-person draw like any other, classified, captured and marked by the domain.
template<class Draw,class Key>static bool namingVetoed(State& s,const Draw& d,const Key& k) {
    if(!s.worldReference.valid() || !k.camera)return false;
    float rows[6][4];std::memcpy(rows,d.camera,sizeof(rows));
    FlatDomainWorldPrediction p;
    if(flatDomainPredictsWorld(rows,s.worldReference,&p))return false;
    s.namingVetoedThisFrame=true;++s.namingVetoes;
    if(s.namingVetoLogged<12) {
        ++s.namingVetoLogged;
        double p0=0,p1=0;const bool scale=flatCameraProjectionScale(rows,p0,p1);
        Log::get().note("flat world naming vetoed %u/12: frame=%llu seq=%u VS=%016llX PS=%016llX camera=%016llX near=%.9g scale=%.6g,%.6g; world reference near=%.9g scale=%.6g,%.6g; a supported scene draw before the world is named, with a camera that is not the last world's, does not name it",
            s.namingVetoLogged,(unsigned long long)s.prefix.frame,s.prefix.sequence,(unsigned long long)k.vs,(unsigned long long)k.ps,
            (unsigned long long)k.cameraHash,rows[3][2],scale?p0:0.0,scale?p1:0.0,s.worldReference.nearPlane,s.worldReference.p0,s.worldReference.p1);
    }
    return true;
}
bool owner() { return state().thread == GetCurrentThreadId(); }
void armDrawPackets(State& s,uint64_t frame) {
    const auto root=Config::get().logDir()+L"\\flat_draw_packets";
    const auto leaf=root+L"\\capture_"+std::to_wstring(GetTickCount64())+L"_"+std::to_wstring(GetCurrentProcessId());
    const bool rootOk=CreateDirectoryW(root.c_str(),nullptr)||GetLastError()==ERROR_ALREADY_EXISTS;
    const bool leafOk=rootOk&&(CreateDirectoryW(leaf.c_str(),nullptr)||GetLastError()==ERROR_ALREADY_EXISTS);
    if(leafOk){s.drawPackets.arm(leaf,frame,&lookupShaderHash,&flatProbeShaderLookup);s.drawPacketsReported=0;}
    Log::get().note("flat draw packets: arm ready=%u frame=%llu priority-pairs=16 representative-categories=4 draws-per-priority-pair=2 distinct-frames=1 memory=1GiB representatives=256MiB file-chunk=64MiB expiry=900frames/30s directory=%ls; no HDR qualification required",
        leafOk?1u:0u,(unsigned long long)frame,leaf.c_str());
    Log::get().note("flat draw packets: refused-pair history retained=%zu capacity=64 oldest-evictions=%llu; draw-scoped projection/overlay refusals update identities independently of manual arm",
        s.drawPacketRefusedPairs.size(),(unsigned long long)s.drawPacketHistoryEvictions);
}
// Observe query brackets from the first game Begin/End, independently of
// Present, AA mode, resize and State's owner thread. Context/query references
// prevent pointer reuse; a fixed table never evicts an active observation.
struct ReplayQueryObserver {
    struct Context {
        Ptr<ID3D11DeviceContext> context;
        FlatReplayQueryTracker tracker;
        Ptr<ID3D11Asynchronous> holds[64];
    } contexts[8];
    std::mutex mutex;
    bool overflow=false;
    Context* find(ID3D11DeviceContext* ctx) {
        for(auto& entry:contexts)if(entry.context.Get()==ctx)return &entry;
        for(auto& entry:contexts)if(!entry.context) { entry.context=ctx;return &entry; }
        overflow=true;return nullptr;
    }
};
ReplayQueryObserver& replayQueryObserver() { static auto* observer=new ReplayQueryObserver;return *observer; }
bool replayQueriesSafe(ID3D11DeviceContext* ctx) {
    if(!ctx || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    auto& observer=replayQueryObserver();std::lock_guard<std::mutex> lock(observer.mutex);
    auto* entry=observer.find(ctx);
    return !observer.overflow && entry && entry->tracker.safe();
}
FlatReplayQueryTracker::Kind replayQueryKind(ID3D11Asynchronous* async) {
    if (!async) return FlatReplayQueryTracker::Kind::Unknown;
    Ptr<ID3D11Query> query;
    if (FAILED(async->QueryInterface(IID_PPV_ARGS(&query))))
        return FlatReplayQueryTracker::Kind::Unknown;
    D3D11_QUERY_DESC desc{}; query->GetDesc(&desc);
    switch (desc.Query) {
    case D3D11_QUERY_EVENT:
    case D3D11_QUERY_TIMESTAMP:
        return FlatReplayQueryTracker::Kind::EndOnly;
    case D3D11_QUERY_TIMESTAMP_DISJOINT:
        return FlatReplayQueryTracker::Kind::Timing;
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_OCCLUSION_PREDICATE:
    case D3D11_QUERY_PIPELINE_STATISTICS:
    case D3D11_QUERY_SO_STATISTICS:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
    case D3D11_QUERY_SO_STATISTICS_STREAM0:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
    case D3D11_QUERY_SO_STATISTICS_STREAM1:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
    case D3D11_QUERY_SO_STATISTICS_STREAM2:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
    case D3D11_QUERY_SO_STATISTICS_STREAM3:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3:
        return FlatReplayQueryTracker::Kind::Count;
    default: return FlatReplayQueryTracker::Kind::Unknown;
    }
}
FlatMapBounce& mapBounce() {
    static FlatMapBounceD3DDriver* driver = new FlatMapBounceD3DDriver;
    static FlatMapBounce* bounce = new FlatMapBounce(*driver);
    return *bounce;
}
struct MapBounceWindow {
    std::atomic<uint64_t> maps{0}, tracked{0}, otherBuffer{0}, texture{0};
    std::atomic<uint64_t> notDiscard{0}, foreignContext{0}, foreignThread{0};
    std::atomic<uint64_t> internal{0}, paused{0}, untracked{0}, failed{0};
    std::atomic<uint64_t> bankBytes{0}, bankTicks{0};
    std::atomic<uint64_t> bankSamples{0}, totalBankBytes{0};
    std::atomic<uint64_t> widthBuckets[6]{};
    std::atomic<uint64_t> trackedTypes[6]{};
};
MapBounceWindow& mapBounceWindow() { static MapBounceWindow* window = new MapBounceWindow; return *window; }
const char* bounceKeyName(flatmap::Mode mode) {
    return mode == flatmap::Mode::On ? "on" : mode == flatmap::Mode::Off ? "off" : "auto";
}
const char* bounceStateName(flatmap::State state) {
    switch (state) {
    case flatmap::State::On: return "on";
    case flatmap::State::Off: return "off";
    case flatmap::State::Tripped: return "tripped";
    default: return "pending";
    }
}
void reportMapBounce(uint64_t frame, uint64_t now, bool periodic) {
    auto& bounce = mapBounce();
    auto& w = mapBounceWindow();
    static flatmap::Counters previous{};
    static uint64_t lastMaps=0,lastTracked=0,lastOther=0,lastTexture=0;
    static uint64_t lastNotDiscard=0,lastForeignCtx=0,lastForeignThread=0;
    static uint64_t lastInternal=0,lastPaused=0,lastUntracked=0,lastFailed=0;
    static uint64_t lastBankBytes=0,lastBankTicks=0,lastTotalBankBytes=0;
    static uint64_t lastWidth[6]{},lastTypes[6]{};
    static uint64_t lastCbFirst=0;
    static flatmap::Trip lastTrip=flatmap::Trip::None;
    static bool decided=false,pendingNoted=false;
    static uint64_t began=0;
    if (!began) began=now;
    if (!decided && bounce.samples() == 32) {
        decided=true;
        const auto* r=bounce.rates();
        Log::get().note("flat map bounce: decision at frame %llu, batch rates %.3f/%.3f/%.3f/%.3f B/ns, threshold=1.0 -> %s, key=%s",
            (unsigned long long)frame,r[0],r[1],r[2],r[3],
            bounce.decision()==flatmap::State::On?"ON":"OFF",bounceKeyName(bounce.mode()));
    }
    if (!pendingNoted && !decided && now-began>=30000 && bounce.decision()==flatmap::State::Pending) {
        pendingNoted=true;
        Log::get().note("flat map bounce: pending %u/32 qualifying copies at frame %llu",
            bounce.samples(),(unsigned long long)frame);
    }
    if (bounce.tripReason()!=lastTrip) {
        lastTrip=bounce.tripReason();
        const char* reason=lastTrip==flatmap::Trip::Remap?"remap":
            lastTrip==flatmap::Trip::Present?"open-at-present":
            lastTrip==flatmap::Trip::Context?"context-mismatch":
            lastTrip==flatmap::Trip::Verify?"verify-mismatch":"none";
        Log::get().note("flat map bounce: tripped reason=%s frame=%llu; new maps pass through",
            reason,(unsigned long long)frame);
    }
    if (!periodic) return;
    const auto c=bounce.counters();
    const uint64_t maps=w.maps.load(),tracked=w.tracked.load(),other=w.otherBuffer.load(),texture=w.texture.load();
    const uint64_t nd=w.notDiscard.load(),fc=w.foreignContext.load(),ft=w.foreignThread.load();
    const uint64_t in=w.internal.load(),pa=w.paused.load(),un=w.untracked.load(),failed=w.failed.load();
    const uint64_t bb=w.bankBytes.load(),bt=w.bankTicks.load();
    const uint64_t totalBankBytes=w.totalBankBytes.load();
    const uint64_t cbFirst=g_flatCbFirstNonzero.load();
    uint64_t bucket[6]{},type[6]{};
    for (unsigned i=0;i<6;++i) {
        const uint64_t b=w.widthBuckets[i].load(),t=w.trackedTypes[i].load();
        bucket[i]=b-lastWidth[i];type[i]=t-lastTypes[i];
        lastWidth[i]=b;lastTypes[i]=t;
    }
    const uint64_t flushBytes=c.flushBytes-previous.flushBytes,flushTicks=c.flushTicks-previous.flushTicks;
    const uint64_t bankBytes=bb-lastBankBytes,bankTicks=bt-lastBankTicks;
    const double nsPerKb=!bankBytes ? 0.0 :
        double(bankTicks)*1e9*1024.0/(double(bankBytes)*double(FlatMapBounceD3DDriver{}.ticksPerSecond()));
    const double flushNsPerKb=!flushBytes ? 0.0 :
        double(flushTicks)*1e9*1024.0/(double(flushBytes)*double(FlatMapBounceD3DDriver{}.ticksPerSecond()));
    Log::get().note("flat map bounce 5s: state=%s key=%s maps=%llu tracked=%llu other-buffer=%llu texture=%llu bounced=%llu declined-not-discard=%llu declined-foreign-context=%llu declined-foreign-thread=%llu declined-internal=%llu declined-paused=%llu declined-untracked=%llu declined-open-full=%llu declined-width=%llu failed=%llu flushes=%llu flush-bytes=%llu flush-ns-per-kb=%.1f bank-bytes=%llu bank-sample-bytes=%llu bank-ns-per-kb=%.1f abandoned=%llu open-at-present=%llu trips=%llu verify-samples=%llu verify-mismatches=%llu unchanged-rows=%llu rows-checked=%llu width-le64=%llu width-le256=%llu width-le1k=%llu width-le4k=%llu width-le8k=%llu width-le64k=%llu tracked-read=%llu tracked-write=%llu tracked-read-write=%llu tracked-discard=%llu tracked-no-overwrite=%llu cb-first-nonzero=%llu",
        bounceStateName(bounce.decision()),bounceKeyName(bounce.mode()),
        (unsigned long long)(maps-lastMaps),(unsigned long long)(tracked-lastTracked),
        (unsigned long long)(other-lastOther),
        (unsigned long long)(texture-lastTexture),
        (unsigned long long)(c.bounced-previous.bounced),(unsigned long long)(nd-lastNotDiscard),
        (unsigned long long)(fc-lastForeignCtx),(unsigned long long)(ft-lastForeignThread),
        (unsigned long long)(in-lastInternal),(unsigned long long)(pa-lastPaused),
        (unsigned long long)(un-lastUntracked),(unsigned long long)(c.full-previous.full),
        (unsigned long long)(c.width-previous.width),(unsigned long long)(failed-lastFailed),
        (unsigned long long)(c.flushes-previous.flushes),(unsigned long long)flushBytes,flushNsPerKb,
        (unsigned long long)(totalBankBytes-lastTotalBankBytes),
        (unsigned long long)bankBytes,nsPerKb,(unsigned long long)(c.abandoned-previous.abandoned),
        (unsigned long long)(c.openAtPresent-previous.openAtPresent),(unsigned long long)c.trips,
        (unsigned long long)(c.verifySamples-previous.verifySamples),(unsigned long long)c.verifyMismatches,
        (unsigned long long)(c.unchangedRows-previous.unchangedRows),
        (unsigned long long)(c.rowsChecked-previous.rowsChecked),
        (unsigned long long)bucket[0],(unsigned long long)bucket[1],(unsigned long long)bucket[2],
        (unsigned long long)bucket[3],(unsigned long long)bucket[4],(unsigned long long)bucket[5],
        (unsigned long long)type[1],(unsigned long long)type[2],
        (unsigned long long)type[3],
        (unsigned long long)type[4],(unsigned long long)type[5],
        (unsigned long long)(cbFirst-lastCbFirst));
    previous=c;lastMaps=maps;lastTracked=tracked;lastOther=other;lastTexture=texture;
    lastNotDiscard=nd;lastForeignCtx=fc;lastForeignThread=ft;lastInternal=in;
    lastPaused=pa;lastUntracked=un;lastFailed=failed;lastBankBytes=bb;lastBankTicks=bt;
    lastTotalBankBytes=totalBankBytes;lastCbFirst=cbFirst;
}
bool nonzeroPhase(const State& s) { return s.phase.currentX!=0 || s.phase.currentY!=0; }
void reportProjectionFailure(const State& s, const FlatProjectionRecipes& recipes,
    uint64_t vs, uint64_t ps, uint64_t cs) {
    if(!s.projection || !nonzeroPhase(s))return;
    // Failure-only, independent of F10. Startup cannot consume the live-draw
    // budget, and resource resets cannot restart either process-wide budget.
    static uint32_t appliedEvents=0, earlyEvents=0;
    static uint64_t lastFrame=~uint64_t{0};
    uint32_t& events=s.phase.applied?appliedEvents:earlyEvents;
    const uint32_t limit=s.phase.applied?32u:8u;
    if(events>=limit || lastFrame==s.prefix.frame)return;
    const auto f=s.projection->failure();
    if(!f.valid)return;
    lastFrame=s.prefix.frame;++events;
    Log::get().note("flat projection failure event: frame=%llu VS=%016llX PS=%016llX CS=%016llX branch=%s code=%u call=%s applied=%u phase=%u jitter=(%.7g,%.7g) allocation=%u exact-plan=%u topology-plan=%u request=%u patch=%u event=%u/%u",
        (unsigned long long)s.prefix.frame,(unsigned long long)vs,(unsigned long long)ps,(unsigned long long)cs,
        f.branch?f.branch:"unavailable",static_cast<unsigned>(f.reason),f.inPrepare?"prepare":"preflight",
        s.phase.applied,f.phase,s.phase.currentX,s.phase.currentY,f.allowAllocation?1u:0u,
        f.exactPlan?1u:0u,f.topologyPlan?1u:0u,f.requestIndex,f.patchIndex,events,limit);
    Log::get().note("flat projection failure buffer: frame=%llu stage=%u slot=%u resource=%p first=%u count=%u tracked=%u generation=%llu bytes=%u mutation=%llu mapped=%u cold-pending=%u promoted=%u private-ready=%u shadow=%u shadow-write=%llu shadow-epoch=%llu actual-resource=%p actual-first=%u actual-count=%u",
        (unsigned long long)s.prefix.frame,static_cast<unsigned>(f.stage),f.slot,f.buffer,f.firstConstant,f.constantCount,
        f.tracked?1u:0u,(unsigned long long)f.trackedGeneration,f.trackedWidth,(unsigned long long)f.mutationSerial,
        f.mapped?1u:0u,f.pending?1u:0u,f.promoted?1u:0u,f.privateReady?1u:0u,f.shadowPresent?1u:0u,
        (unsigned long long)f.shadowWriteGeneration,(unsigned long long)f.shadowBankEpoch,
        f.actualBuffer,f.actualFirst,f.actualCount);
    // A missing plan can involve more than one CB. Retain all requested
    // bindings and offsets so the first request is not mistaken for the cause.
    for(uint32_t i=0;i<recipes.count;++i) {
        const auto& r=recipes.requests[i];
        for(uint32_t p=0;p<r.patchCount;++p) {
            const auto& patch=r.patches[p];
            Log::get().note("flat projection failure request: frame=%llu request=%u stage=%u slot=%u resource=%p first=%u count=%u patch=%u/%u layout=%u byte-offset=%u",
                (unsigned long long)s.prefix.frame,i,static_cast<unsigned>(r.stage),r.slot,r.original,
                r.firstConstant,r.constantCount,p,r.patchCount,static_cast<unsigned>(patch.layout),patch.byteOffset);
        }
    }
}
void failPhase(State& s,const char* reason) {
    s.frameCoverage=false;
    s.phase.fail();s.jitterReason=reason;++s.jitterRefusals;
    if(s.phaseCensusPending) {
        s.phaseCensusFailed=true;
        uint32_t index=0;
        for(;index<s.phaseFailuresUsed;++index)
            if(std::strcmp(s.phaseFailures[index].reason,reason)==0)break;
        if(index==s.phaseFailuresUsed && index<32) {
            auto& entry=s.phaseFailures[s.phaseFailuresUsed++];
            std::strncpy(entry.reason,reason,sizeof(entry.reason)-1);
        }
        if(index<32) {
            auto& entry=s.phaseFailures[index];
            if(!entry.frames)entry.firstFrame=s.prefix.frame;
            if(entry.lastFrame!=s.prefix.frame) {++entry.frames;entry.lastFrame=s.prefix.frame;}
            ++entry.calls;
        } else {
            ++s.phaseOverflowCalls;
            if(s.phaseOverflowLastFrame!=s.prefix.frame) {++s.phaseOverflowFrames;s.phaseOverflowLastFrame=s.prefix.frame;}
        }
    }
    if(s.jitterRefusals<=12)Log::get().note("flat jitter refusal: frame=%llu reason=%s applied=%u phase=(%.5g,%.5g); temporal history will be rejected",
        (unsigned long long)s.prefix.frame,reason,s.phase.applied,s.phase.currentX,s.phase.currentY);
}
void finishPhaseCensusFrame(State& s) {
    if(!s.phaseCensusPending)return;
    s.phaseCensusPending=false;
    ++s.phaseCensusFrames;
    if(!s.phaseCensusFailed)return;
    ++s.phaseCensusFailedFrames;
    if(s.treated)++s.phaseCensusTreatedFailedFrames;
    for(uint32_t i=0;i<s.phaseFailuresUsed;++i)if(s.phaseFailures[i].lastFrame==s.prefix.frame) {
        s.phaseFailures[i].treatedFrames+=s.treated?1u:0u;
        s.phaseFailures[i].acceptedFrames+=s.temporalAccepted?1u:0u;
    }
    s.phaseCensusFailed=false;
}
void reportPhaseCensus(State& s,const char* event) {
    Log::get().note("flat jitter failure census: event=%s frames=%llu failed-frames=%llu treated-failed-frames=%llu reasons=%u overflow-calls=%llu overflow-frames=%llu; zero failed-frames means no phase refusal in this window",
        event,(unsigned long long)s.phaseCensusFrames,(unsigned long long)s.phaseCensusFailedFrames,
        (unsigned long long)s.phaseCensusTreatedFailedFrames,s.phaseFailuresUsed,
        (unsigned long long)s.phaseOverflowCalls,(unsigned long long)s.phaseOverflowFrames);
    for(uint32_t i=0;i<s.phaseFailuresUsed;++i) {
        const auto& entry=s.phaseFailures[i];
        Log::get().note("flat jitter failure reason: event=%s reason=%s calls=%llu frames=%llu treated-frames=%llu accepted-frames=%llu first-frame=%llu last-frame=%llu",
            event,entry.reason,(unsigned long long)entry.calls,(unsigned long long)entry.frames,
            (unsigned long long)entry.treatedFrames,(unsigned long long)entry.acceptedFrames,
            (unsigned long long)entry.firstFrame,(unsigned long long)entry.lastFrame);
    }
    for(auto& entry:s.phaseFailures)entry=State::PhaseFailure{};
    s.phaseFailuresUsed=0;s.phaseOverflowCalls=s.phaseOverflowFrames=s.phaseOverflowLastFrame=0;
    s.phaseCensusFrames=s.phaseCensusFailedFrames=s.phaseCensusTreatedFailedFrames=0;
}
bool sameResolvePlan(const FlatMonoResolvePreflight& a,const FlatMonoResolvePreflight& b) {
    return a.renderWidth==b.renderWidth && a.renderHeight==b.renderHeight && a.outputWidth==b.outputWidth &&
        a.outputHeight==b.outputHeight && a.evalWidth==b.evalWidth && a.evalHeight==b.evalHeight &&
        a.mode==b.mode && a.hdr==b.hdr && a.colorViewFormat==b.colorViewFormat &&
        a.depthViewFormat==b.depthViewFormat && a.colorViewIsTexture2D==b.colorViewIsTexture2D &&
        a.depthViewIsTexture2D==b.depthViewIsTexture2D && a.colorMostDetailedMip==b.colorMostDetailedMip &&
        a.depthMostDetailedMip==b.depthMostDetailedMip && a.colorViewMipLevels==b.colorViewMipLevels &&
        a.depthViewMipLevels==b.depthViewMipLevels && a.colorResourceMipLevels==b.colorResourceMipLevels &&
        a.colorArraySize==b.colorArraySize && a.colorSampleCount==b.colorSampleCount &&
        a.depthResourceMipLevels==b.depthResourceMipLevels &&
        a.depthArraySize==b.depthArraySize && a.depthSampleCount==b.depthSampleCount;
}
State::AuditOutcome* projectionDetail(State& s, uint64_t vs, uint64_t ps, uint64_t cs, uint32_t reason, const char* text) {
    State::AuditOutcome* foundOutcome=nullptr;
    for (uint32_t i=0;i<s.projectionOutcomesUsed;++i) {
        auto& outcome=s.projectionOutcomes[i];
        if (outcome.vs==vs && outcome.ps==ps && outcome.cs==cs && outcome.reason==reason) {
            ++outcome.observations; foundOutcome=&outcome; break;
        }
    }
    if (!foundOutcome) {
        if (s.projectionOutcomesUsed<256) {
            auto& outcome=s.projectionOutcomes[s.projectionOutcomesUsed++];
            outcome={vs,ps,cs,1,reason};foundOutcome=&outcome;
        } else ++s.projectionOutcomeOverflow;
    }
    for (uint32_t i=0;i<s.projectionDetailsUsed;++i) {
        const auto& d=s.projectionDetails[i];
        if (d.vs==vs && d.ps==ps && d.cs==cs && d.reason==reason) return foundOutcome;
    }
    if (s.projectionDetailsUsed==32) return foundOutcome;
    s.projectionDetails[s.projectionDetailsUsed++]={vs,ps,cs,reason};
    Log::get().note("flat projection candidate: VS=%016llX PS=%016llX CS=%016llX reason=%s code=%u; actual binding counts are reported by flat jitter",
        static_cast<unsigned long long>(vs),static_cast<unsigned long long>(ps),static_cast<unsigned long long>(cs),text,reason);
    return foundOutcome;
}
void recordProjectionViewportFailure(State& s, uint64_t vs, uint64_t ps, uint64_t cs,
                                     uint32_t width, uint32_t height, UINT count,
                                     const D3D11_VIEWPORT& viewport) {
    ++s.projectionMissing;++s.projectionViewportMismatches;
    auto* outcome=projectionDetail(s,vs,ps,cs,104,"projection-viewport-mismatch");
    if(!outcome) {++s.projectionViewportUnrecorded;return;}
    const bool depthClamped=count==1 && viewport.TopLeftX==0 && viewport.TopLeftY==0 &&
        viewport.Width==float(width) && viewport.Height==float(height) &&
        viewport.MinDepth==0 && viewport.MaxDepth==0;
    auto& observations=depthClamped?outcome->viewportDepthClamped:outcome->viewportOther;
    // The first witness of each class belongs to the 256-entry outcome table,
    // independently of the 32 generic candidate-detail lines. Later sightings
    // remain counted, including other failures of the same shader pair.
    if(++observations!=1)return;
    if(s.projectionViewportWitnesses==32) {++s.projectionViewportSuppressed;return;}
    ++s.projectionViewportWitnesses;
    // Fetch the maximum capacity for the diagnostic so count and viewport0
    // remain an unambiguous witness even for a multiple-viewport rejection.
    // This diagnostic query does not replace the live qualification query.
    D3D11_VIEWPORT actualViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT actualCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    s.context->RSGetViewports(&actualCount,actualViewports);
    const auto& actualViewport=actualViewports[0];
    Ptr<ID3D11RenderTargetView> rtv;Ptr<ID3D11DepthStencilView> dsv;
    Ptr<ID3D11Resource> color,depth;
    s.context->OMGetRenderTargets(1,&rtv,&dsv);
    if(rtv)rtv->GetResource(&color);if(dsv)dsv->GetResource(&depth);
    Log::get().note("flat projection viewport witness: frame=%llu q=%u VS=%016llX PS=%016llX CS=%016llX class=%s expected=%ux%u gate-count=%u count=%u viewport0=(%.9g,%.9g,%.9g,%.9g,%.9g,%.9g) RT0=%p color=%p DSV=%p depth=%p named-depth=%p phase-depth=%p; actual state, first witness per pair and class, viewport0 valid only when count>0",
        (unsigned long long)s.prefix.frame,s.prefix.sequence,(unsigned long long)vs,(unsigned long long)ps,(unsigned long long)cs,
        depthClamped?"full-xy-depth-clamped":"other",width,height,count,actualCount,
        actualViewport.TopLeftX,actualViewport.TopLeftY,actualViewport.Width,actualViewport.Height,actualViewport.MinDepth,actualViewport.MaxDepth,
        rtv.Get(),color.Get(),dsv.Get(),depth.Get(),s.namedDepth,s.phaseDepth.Get());
}
void reportUnknownProjection(State& s,const char* event) {
    Log::get().note("flat unknown projection capture: event=%s distinct-pairs=%u automatic-pairs=%u audit-pairs=%u overflow-observations=%llu bytecode-stages-saved=%u bytecode-stages-failed=%u absent-stages=%u; bounded to 64 pairs since process start or manual F10 rearm, capture remains active after audit completion; stage failures include missing creation bytes",
        event,s.unknownProjectionPairsUsed,s.unknownProjectionAutomatic,s.unknownProjectionAudit,
        (unsigned long long)s.unknownProjectionCaptureOverflow,s.unknownProjectionBytesSaved,
        s.unknownProjectionBytesFailed,s.unknownProjectionStagesAbsent);
}
void reportProjection(State& s, const char* event) {
    if (!s.projection) return;
    const auto status=s.projection->status();
    Log::get().note("flat projection readiness: event=%s draws=%llu dispatches=%llu candidates=%llu prepared=%llu refused=%llu depth-unassociated=%llu unknown-scene-draws=%llu full-writes=%llu initial-writes=%llu invalidations=%llu frames-left=%u raster-phase=(%.5g,%.5g) raster-bindings=%u resolve-ready=%u fallback-ready=%u backend-available=%u backend-feature-deferred=%u preflight=%s spatial-fallbacks=%llu spatial-fallback-failures=%llu",
        event,(unsigned long long)s.projectionDraws,(unsigned long long)s.projectionDispatches,(unsigned long long)s.projectionCandidates,
        (unsigned long long)s.projectionReady,(unsigned long long)s.projectionMissing,(unsigned long long)s.projectionUnowned,
        (unsigned long long)s.projectionUnknown,(unsigned long long)status.fullWrites,(unsigned long long)status.initialWrites,
        (unsigned long long)status.invalidations,s.projectionFrames,s.phase.currentX,s.phase.currentY,s.phase.applied,s.resolvePreflight.rendererReady?1u:0u,
        s.resolvePreflight.spatialFallbackReady?1u:0u,s.resolvePreflight.backendAvailable?1u:0u,
        s.resolvePreflight.backendFeatureCreationDeferred?1u:0u,s.resolvePreflight.reason,
        (unsigned long long)s.spatialFallbacks,(unsigned long long)s.spatialFallbackFailures);
    Log::get().note("flat projection viewport audit: event=%s checked=%llu matched=%llu mismatched=%llu witnesses=%llu suppressed-witnesses=%llu unrecorded=%llu; checked=0 means viewport gate was not reached, diagnostic does not change qualification",
        event,(unsigned long long)s.projectionViewportChecks,
        (unsigned long long)(s.projectionViewportChecks-s.projectionViewportMismatches),
        (unsigned long long)s.projectionViewportMismatches,(unsigned long long)s.projectionViewportWitnesses,(unsigned long long)s.projectionViewportSuppressed,
        (unsigned long long)s.projectionViewportUnrecorded);
    Log::get().note("flat projection outcome totals: distinct=%u overflow-observations=%llu unchanged-draws=%llu; result=prepared is reason-code 0, failures retain their code, 101=unknown-recipe, 103=bytecode-unchanged, 104=projection-viewport-mismatch, 105=generic-recipe, 106=generic-inert",
        s.projectionOutcomesUsed,(unsigned long long)s.projectionOutcomeOverflow,(unsigned long long)s.projectionUnchanged);
    static const char* outcomeNames[]={"prepared","wrong-thread","no-context1","capacity","unknown-buffer","missing-full-write","unsupported-range","binding-mismatch","invalid-recipe","private-failure","plan-failure"};
    for(uint32_t i=0;i<s.projectionOutcomesUsed;++i) {
        const auto& outcome=s.projectionOutcomes[i];
        const char* label=outcome.reason<11?outcomeNames[outcome.reason]:
            outcome.reason==100?"actual-shader-mismatch":outcome.reason==101?"unknown-scene-projection-recipe":
            outcome.reason==102?"invalid-render-extent":outcome.reason==103?"bytecode-unchanged":
            outcome.reason==104?"projection-viewport-mismatch":outcome.reason==105?"generic-recipe":
            outcome.reason==106?"generic-inert":"other-refusal";
        Log::get().note("flat projection outcome: event=%s VS=%016llX PS=%016llX CS=%016llX result=%s code=%u count=%llu",
            event,(unsigned long long)outcome.vs,(unsigned long long)outcome.ps,(unsigned long long)outcome.cs,
            label,outcome.reason,(unsigned long long)outcome.observations);
        if(outcome.reason==104)Log::get().note("flat projection viewport outcome: event=%s VS=%016llX PS=%016llX CS=%016llX full-xy-depth-clamped=%llu other=%llu",
            event,(unsigned long long)outcome.vs,(unsigned long long)outcome.ps,(unsigned long long)outcome.cs,
            (unsigned long long)outcome.viewportDepthClamped,(unsigned long long)outcome.viewportOther);
        if(outcome.reason==0)Log::get().note("flat projection reference: event=%s VS=%016llX PS=%016llX CS=%016llX canonical=%llu basis-match=%llu unmatched=%llu unavailable=%llu unsupported=%llu residual-samples=%llu max-spatial-depth-error=%.9g max-translation-residual=%.9g; primary-forward-recipe only, numeric relation is not frame authorization",
            event,(unsigned long long)outcome.vs,(unsigned long long)outcome.ps,(unsigned long long)outcome.cs,
            (unsigned long long)outcome.canonical,(unsigned long long)outcome.basisMatch,(unsigned long long)outcome.unmatched,
            (unsigned long long)outcome.unavailable,(unsigned long long)outcome.unsupported,(unsigned long long)outcome.residualSamples,
            outcome.spatialDepthError,outcome.translationResidual);
    }
    static const char* reasons[]={"none","wrong-thread","no-context1","capacity","unknown-buffer","missing-full-write","unsupported-range","binding-mismatch","invalid-recipe","private-failure","plan-failure"};
    for (uint32_t i=1;i<11;++i) if(status.refusals[i])
        Log::get().note("flat projection refusal: reason=%s cumulative=%llu",reasons[i],(unsigned long long)status.refusals[i]);
    Log::get().note("flat projection cold buffers: queued=%llu completed=%llu stale=%llu failed=%llu pending=%llu timeouts=%llu; asynchronous full snapshots, unchanged-write tokens required",
        (unsigned long long)status.coldQueued,(unsigned long long)status.coldCompleted,(unsigned long long)status.coldStale,
        (unsigned long long)status.coldFailed,(unsigned long long)status.coldPending,(unsigned long long)status.coldTimeouts);
    Log::get().note("flat projection live plans: retargets=%llu; prepared sources only, no new private buffers or cold readbacks",
        (unsigned long long)status.livePlanRetargets);
    for(uint32_t i=0;i<2;++i) Log::get().note(
        "flat local projection capture: event=%s pair=%u attempts=%u complete-captures=%u handoff-links=%u first-frame=%llu; at most two distinct frames, exact F10 pair only",
        event,i,s.localSamples[i].attempts,s.localSamples[i].complete,
        (s.localSamples[i].closed[0]?1u:0u)+(s.localSamples[i].closed[1]?1u:0u),
        (unsigned long long)s.localSamples[i].firstFrame);
    const auto& copy=s.copyProvenance;
    uint64_t cameraObserved=0,cameraConflicts=0;
    uint32_t cameraAttempts=0,cameraComplete=0,cameraMissing=0,cameraMismatch=0;
    for(size_t i=0;i<kFlatCameraProbePairCount;++i) {
        const auto& p=s.cameraProbe.pairs[i];const auto& key=kFlatCameraProbePairs[i];
        cameraObserved+=p.observed;cameraConflicts+=p.conflicts;cameraAttempts+=p.attempts;
        cameraComplete+=p.complete;cameraMissing+=p.missing;cameraMismatch+=p.actualMismatch;
        Log::get().note("flat camera probe pair: event=%s pair=%u VS=%016llX PS=%016llX observed=%llu conflicts=%llu attempts=%u complete=%u missing=%u actual-mismatch=%u first-frame=%llu last-frame=%llu result=%s; F10 only, two distinct conflict frames per pair",
            event,unsigned(i),(unsigned long long)key.vs,(unsigned long long)key.ps,
            (unsigned long long)p.observed,(unsigned long long)p.conflicts,p.attempts,p.complete,p.missing,p.actualMismatch,
            (unsigned long long)p.firstFrame,(unsigned long long)p.lastFrame,p.result());
    }
    Log::get().note("flat camera probe: event=%s observed=%llu conflicts=%llu attempts=%u complete=%u missing=%u actual-mismatch=%u pairs=%u; F10 only, CPU shadows only, no camera admission",
        event,(unsigned long long)cameraObserved,(unsigned long long)cameraConflicts,
        cameraAttempts,cameraComplete,cameraMissing,cameraMismatch,unsigned(kFlatCameraProbePairCount));
    reportUnknownProjection(s,event);
    Log::get().note("flat copy provenance capture: event=%s attempts=%u completed=%u missing-source-record=%u missing-destination-record=%u actual-shader-mismatch=%u rearmed-before-complete=%u first-frame=%llu result=%s; two distinct frames per F10 arm separated by at least 90 frames",
        event,copy.attempts,copy.completed,copy.missingSource,copy.missingDestination,
        copy.actualMismatch,copy.rearmedBeforeComplete,(unsigned long long)copy.firstFrame,
        copy.attempts?"observed":"exact-copy-never-observed");
}
// This HDR copy is distinct from the final-output copy admitted by the resolver.
constexpr uint64_t kHdrCopyVs=0xCFA91824129ECBBCull;
constexpr uint64_t kHdrCopyPs=0xDFCBA0EC70B03C9Bull;
constexpr uint64_t kMenuCopyVs=0xDEF19B035D5EDEDCull;
constexpr uint64_t kMenuCopyPs=0xDED8796049C7BB4Aull;
bool verifyMenuHdrCopy(ID3D11DeviceContext* ctx,FlatRuntimeDraw& draw) {
    auto& k=draw.key;
    if(k.vs!=kMenuCopyVs || k.ps!=kMenuCopyPs || k.format!=26)return false;
    FlatComputeInternalScope guard;
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11RenderTargetView> rt;Ptr<ID3D11DepthStencilView> ds;
    Ptr<ID3D11ShaderResourceView> input;
    ctx->VSGetShader(&vs,nullptr,nullptr);ctx->PSGetShader(&ps,nullptr,nullptr);
    ctx->OMGetRenderTargets(1,&rt,&ds);ctx->PSGetShaderResources(0,1,&input);
    if(lookupShaderHash(vs.Get())!=kMenuCopyVs || lookupShaderHash(ps.Get())!=kMenuCopyPs ||
       !rt || rt.Get()!=k.rtv || ds || !input)return false;
    Ptr<ID3D11Resource> source,destination;input->GetResource(&source);rt->GetResource(&destination);
    if(!source || source.Get()==destination.Get() || destination.Get()!=k.color)return false;
    Ptr<ID3D11Texture2D> sourceTexture,destinationTexture;
    source.As(&sourceTexture);destination.As(&destinationTexture);
    if(!sourceTexture || !destinationTexture)return false;
    D3D11_TEXTURE2D_DESC in{},out{};sourceTexture->GetDesc(&in);destinationTexture->GetDesc(&out);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};input->GetDesc(&srv);
    D3D11_RENDER_TARGET_VIEW_DESC rtv{};rt->GetDesc(&rtv);
    UINT count=1;D3D11_VIEWPORT viewport{};ctx->RSGetViewports(&count,&viewport);
    if(in.Format!=DXGI_FORMAT_R11G11B10_FLOAT || out.Format!=DXGI_FORMAT_R11G11B10_FLOAT ||
       srv.Format!=DXGI_FORMAT_R11G11B10_FLOAT || rtv.Format!=DXGI_FORMAT_R11G11B10_FLOAT ||
       srv.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || srv.Texture2D.MostDetailedMip!=0 ||
       srv.Texture2D.MipLevels!=1 || rtv.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D ||
       rtv.Texture2D.MipSlice!=0 || in.MipLevels!=1 || out.MipLevels!=1 ||
       in.ArraySize!=1 || out.ArraySize!=1 || in.SampleDesc.Count!=1 || out.SampleDesc.Count!=1 ||
       in.Width!=k.width || in.Height!=k.height || out.Width!=k.width || out.Height!=k.height ||
       count!=1 || k.viewportCount!=1 || std::memcmp(&viewport,k.viewport,sizeof(viewport))!=0 ||
       !flat_mono_detail::fullViewport(k,k.width,k.height))return false;
    k.srvView[0]=input.Get();k.srvResource[0]=source.Get();
    return true;
}
bool verifyCameraIndependentImageSource(ID3D11DeviceContext* ctx,const FlatRuntimeDraw& draw) {
    const auto& k=draw.key;
    if(k.format!=9 || !flatRuntimeCameraIndependentImageSourcePair(k.vs,k.ps))return false;
    FlatComputeInternalScope guard;
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11RenderTargetView> rt;Ptr<ID3D11DepthStencilView> ds;
    ctx->VSGetShader(&vs,nullptr,nullptr);ctx->PSGetShader(&ps,nullptr,nullptr);
    ctx->OMGetRenderTargets(1,&rt,&ds);
    if(lookupShaderHash(vs.Get())!=k.vs || lookupShaderHash(ps.Get())!=k.ps ||
       !rt || !ds || rt.Get()!=k.rtv || ds.Get()!=k.dsv)return false;
    Ptr<ID3D11Resource> color,depth;rt->GetResource(&color);ds->GetResource(&depth);
    if(color.Get()!=k.color || depth.Get()!=k.depth)return false;
    Ptr<ID3D11Texture2D> colorTexture,depthTexture;
    color.As(&colorTexture);depth.As(&depthTexture);
    if(!colorTexture || !depthTexture)return false;
    D3D11_TEXTURE2D_DESC c{},d{};colorTexture->GetDesc(&c);depthTexture->GetDesc(&d);
    D3D11_RENDER_TARGET_VIEW_DESC r{};rt->GetDesc(&r);
    D3D11_DEPTH_STENCIL_VIEW_DESC z{};ds->GetDesc(&z);
    UINT count=1;D3D11_VIEWPORT viewport{};ctx->RSGetViewports(&count,&viewport);
    return c.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS && r.Format==DXGI_FORMAT_R16G16B16A16_FLOAT &&
        r.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D && r.Texture2D.MipSlice==0 &&
        z.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2D && z.Texture2D.MipSlice==0 &&
        c.ArraySize==1 && d.ArraySize==1 && c.SampleDesc.Count==1 && d.SampleDesc.Count==1 &&
        c.Width==k.width && c.Height==k.height && d.Width==k.width && d.Height==k.height &&
        d.Format==k.depthFormat && count==1 && k.viewportCount==1 &&
        std::memcmp(&viewport,k.viewport,sizeof(viewport))==0 &&
        flat_mono_detail::fullViewport(k,k.width,k.height);
}
void captureUnknownProjection(State& s,const FlatContractObservation& k) {
    // Unknown draws can first appear after the bounded F10 audit expires.
    // Retain this budget across audit completion, resize and mode changes;
    // only an explicit F10 rearm permits another capture of a known pair.
    const auto vs=k.vs,ps=k.ps;
    for(uint32_t i=0;i<s.unknownProjectionPairsUsed;++i)
        if(s.unknownProjectionPairs[i].vs==vs && s.unknownProjectionPairs[i].ps==ps)return;
    if(s.unknownProjectionPairsUsed==64) {
        if(!s.unknownProjectionCaptureOverflow)
            Log::get().note("flat unknown projection capture: event=capacity-reached frame=%llu capacity=64; later unretained pairs counted without bytecode requests until manual F10 rearm",
                (unsigned long long)s.prefix.frame);
        ++s.unknownProjectionCaptureOverflow;return;
    }
    s.unknownProjectionPairs[s.unknownProjectionPairsUsed++]={vs,ps};
    if(s.projectionFrames)++s.unknownProjectionAudit;else ++s.unknownProjectionAutomatic;
    Log::get().note("flat unknown projection capture: frame=%llu q=%u VS=%016llX PS=%016llX trigger=%s color=%p rtv=%p fmt=%u size=%ux%u depth=%p dsv=%p named-depth=%p phase-depth=%p viewport-count=%u viewport=(%.9g,%.9g,%.9g,%.9g,%.9g,%.9g); observed bindings, requesting exact creation bytes once",
        (unsigned long long)s.prefix.frame,s.prefix.sequence,(unsigned long long)vs,(unsigned long long)ps,
        s.projectionFrames?"F10-audit":"automatic",k.color,k.rtv,k.format,k.width,k.height,
        k.depth,k.dsv,s.namedDepth,s.phaseDepth.Get(),k.viewportCount,
        k.viewport[0],k.viewport[1],k.viewport[2],k.viewport[3],k.viewport[4],k.viewport[5]);
    const auto captureStage=[&](char stage,uint64_t hash) {
        if(!hash) {++s.unknownProjectionStagesAbsent;return;}
        if(captureFlatProbeShader(stage,hash))++s.unknownProjectionBytesSaved;
        else ++s.unknownProjectionBytesFailed;
    };
    captureStage('v',vs);captureStage('p',ps);
}
// The exact copy shader consumes only t0 and UV. Its unused b1 binding is not
// a camera observation. The prefix separately verifies all input writes.
bool verifyHdrCopy(ID3D11DeviceContext* ctx,FlatRuntimeDraw& draw) {
    auto& k=draw.key;
    if(k.vs!=kHdrCopyVs || k.ps!=kHdrCopyPs || k.format!=26)return false;
    FlatComputeInternalScope guard;
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11RenderTargetView> rt;Ptr<ID3D11DepthStencilView> ds;
    Ptr<ID3D11ShaderResourceView> input;
    ctx->VSGetShader(&vs,nullptr,nullptr);ctx->PSGetShader(&ps,nullptr,nullptr);
    ctx->OMGetRenderTargets(1,&rt,&ds);ctx->PSGetShaderResources(0,1,&input);
    if(lookupShaderHash(vs.Get())!=kHdrCopyVs || lookupShaderHash(ps.Get())!=kHdrCopyPs ||
       !rt || ds || !input)return false;
    Ptr<ID3D11Resource> source,destination;input->GetResource(&source);rt->GetResource(&destination);
    if(!source || destination.Get()!=k.color || source.Get()==destination.Get())return false;
    Ptr<ID3D11Texture2D> sourceTexture,destinationTexture;
    source.As(&sourceTexture);destination.As(&destinationTexture);
    if(!sourceTexture || !destinationTexture)return false;
    D3D11_TEXTURE2D_DESC in{},out{};sourceTexture->GetDesc(&in);destinationTexture->GetDesc(&out);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};input->GetDesc(&srv);
    D3D11_RENDER_TARGET_VIEW_DESC rtv{};rt->GetDesc(&rtv);
    UINT count=1;D3D11_VIEWPORT viewport{};ctx->RSGetViewports(&count,&viewport);
    if(in.Format!=DXGI_FORMAT_R16G16B16A16_TYPELESS || srv.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT ||
       srv.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || srv.Texture2D.MostDetailedMip!=0 ||
       srv.Texture2D.MipLevels!=1 || out.Format!=DXGI_FORMAT_R11G11B10_FLOAT ||
       rtv.Format!=DXGI_FORMAT_R11G11B10_FLOAT || rtv.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D ||
       rtv.Texture2D.MipSlice!=0 || in.ArraySize!=1 || out.ArraySize!=1 ||
       in.SampleDesc.Count!=1 || out.SampleDesc.Count!=1 ||
       in.Width!=k.width || in.Height!=k.height || out.Width!=k.width || out.Height!=k.height ||
       count!=1 || k.viewportCount!=1 || std::memcmp(&viewport,k.viewport,sizeof(viewport))!=0 ||
       viewport.TopLeftX!=0 || viewport.TopLeftY!=0 || viewport.Width!=float(k.width) ||
       viewport.Height!=float(k.height) || viewport.MinDepth!=0 || viewport.MaxDepth!=1)return false;
    k.srvView[0]=input.Get();k.srvResource[0]=source.Get();
    return true;
}
void captureCopyProvenance(State& s, ID3D11DeviceContext* ctx, const FlatRuntimeDraw& d) {
    const auto& k=d.key;
    if(!s.projectionFrames || k.vs!=kHdrCopyVs || k.ps!=kHdrCopyPs) return;
    auto& sample=s.copyProvenance;
    if(sample.attempts==2 || (sample.attempts && s.prefix.frame-sample.firstFrame<90))return;
    if(!sample.attempts)sample.firstFrame=s.prefix.frame;
    const uint32_t attempt=++sample.attempts;
    sample.frames[attempt-1]=s.prefix.frame;
    FlatComputeInternalScope guard;
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11RenderTargetView> rt[2];Ptr<ID3D11DepthStencilView> ds;
    Ptr<ID3D11ShaderResourceView> input;
    ctx->VSGetShader(&vs,nullptr,nullptr);ctx->PSGetShader(&ps,nullptr,nullptr);
    ID3D11RenderTargetView* targets[2]{};ctx->OMGetRenderTargets(2,targets,&ds);
    rt[0].Attach(targets[0]);rt[1].Attach(targets[1]);
    ctx->PSGetShaderResources(0,1,&input);
    Ptr<ID3D11Resource> dst[2],source,depthResource;
    for(uint32_t i=0;i<2;++i)if(rt[i])rt[i]->GetResource(&dst[i]);
    if(input)input->GetResource(&source);
    if(ds)ds->GetResource(&depthResource);
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};if(input)input->GetDesc(&viewDesc);
    D3D11_RESOURCE_DIMENSION sourceType=D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if(source)source->GetType(&sourceType);
    Ptr<ID3D11Texture2D> sourceTexture;if(source)source.As(&sourceTexture);
    D3D11_TEXTURE2D_DESC sourceDesc{};if(sourceTexture)sourceTexture->GetDesc(&sourceDesc);
    Ptr<ID3D11Texture2D> destinationTexture;if(dst[0])dst[0].As(&destinationTexture);
    D3D11_TEXTURE2D_DESC destinationDesc{};if(destinationTexture)destinationTexture->GetDesc(&destinationDesc);
    UINT viewportCount=1;D3D11_VIEWPORT viewport{};ctx->RSGetViewports(&viewportCount,&viewport);
    const uint64_t actualVs=lookupShaderHash(vs.Get()),actualPs=lookupShaderHash(ps.Get());
    const bool exact=actualVs==kHdrCopyVs && actualPs==kHdrCopyPs;
    sample.completed+=exact;sample.actualMismatch+=!exact;
    const FlatRuntimeTarget* inputTarget=nullptr,*outputTarget=nullptr;
    for(uint32_t i=0;i<s.prefix.targetsUsed;++i) {
        const auto& target=s.prefix.targets[i];
        if(target.resource==source.Get())inputTarget=&target;
        if(target.resource==dst[0].Get())outputTarget=&target;
    }
    sample.missingSource+=!inputTarget;sample.missingDestination+=!outputTarget;
    Log::get().note("flat copy provenance: attempt=%u frame=%llu pre-seq=%u cached-VS=%016llX cached-PS=%016llX actual-VS=%016llX actual-PS=%016llX exact=%u rt0-view=%p rt0-resource=%p rt1-view=%p rt1-resource=%p dsv=%p dsv-resource=%p ps-t0-view=%p ps-t0-resource=%p viewport-count=%u viewport=(%.1f,%.1f,%.1f,%.1f,%.2f,%.2f)",
        attempt,(unsigned long long)s.prefix.frame,s.prefix.sequence,(unsigned long long)k.vs,(unsigned long long)k.ps,
        (unsigned long long)actualVs,(unsigned long long)actualPs,exact?1u:0u,rt[0].Get(),dst[0].Get(),rt[1].Get(),dst[1].Get(),
        ds.Get(),depthResource.Get(),input.Get(),source.Get(),viewportCount,viewport.TopLeftX,viewport.TopLeftY,
        viewport.Width,viewport.Height,viewport.MinDepth,viewport.MaxDepth);
    Log::get().note("flat copy provenance view: attempt=%u frame=%llu t0-view-dimension=%u t0-view-format=%u t0-mip=%u t0-mip-levels=%u t0-resource-type=%u t0-resource-size=%ux%u t0-resource-format=%u t0-samples=%u t0-array=%u rt0-resource-size=%ux%u rt0-resource-format=%u; missing view/texture fields are zero",
        attempt,(unsigned long long)s.prefix.frame,(unsigned)viewDesc.ViewDimension,(unsigned)viewDesc.Format,
        viewDesc.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D?viewDesc.Texture2D.MostDetailedMip:0u,
        viewDesc.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D?viewDesc.Texture2D.MipLevels:0u,
        (unsigned)sourceType,sourceDesc.Width,sourceDesc.Height,
        (unsigned)sourceDesc.Format,sourceDesc.SampleDesc.Count,sourceDesc.ArraySize,
        destinationDesc.Width,destinationDesc.Height,(unsigned)destinationDesc.Format);
    for(uint32_t role=0;role<2;++role) {
        const auto* target=role?outputTarget:inputTarget;
        if(!target) {
            Log::get().note("flat copy provenance prior: attempt=%u frame=%llu role=%s resource=%p prefix-target=unobserved; no scene identity inferred",
                attempt,(unsigned long long)s.prefix.frame,role?"destination":"source",role?dst[0].Get():source.Get());
            continue;
        }
        const auto& writes=target->writes;
        Log::get().note("flat copy provenance prior: attempt=%u frame=%llu role=%s resource=%p writes=%u first=%u last=%u first-VS=%016llX first-PS=%016llX first-RTV=%p first-depth=%p first-DSV=%p first-b1=%p first-camera-hash=%016llX first-camera-present=%u first-write-epoch=%llu first-write-seq=%u last-camera-write-epoch=%llu last-camera-write-seq=%u hdr-bad=%u image-source-bad=%u bad-cause=%s tones=%u tone-input=%p",
            attempt,(unsigned long long)s.prefix.frame,role?"destination":"source",target->resource,
            writes.draws,writes.first,writes.last,(unsigned long long)writes.key.vs,(unsigned long long)writes.key.ps,
            writes.key.rtv,writes.key.depth,writes.key.dsv,writes.key.b1,(unsigned long long)writes.key.cameraHash,
            writes.key.camera?1u:0u,(unsigned long long)writes.firstWriteEpoch,writes.firstWriteSeq,
            (unsigned long long)writes.lastWriteEpoch,writes.lastWriteSeq,target->hdrBad?1u:0u,target->imageSourceBad?1u:0u,
            flatRuntimeConflictName(target->firstBad.cause),target->tones,flat_mono_detail::toneHdrInput(target->tone.key));
    }
}
struct LocalRows { bool pixel; UINT slot, row, count; const char* name; };
// These are the rows the four captured DXBC shaders actually consume for clip,
// local transforms, depth comparison and material scale. Shader-visible row n
// maps to backing byte (firstConstant+n)*16 when a CB range is bound.
constexpr LocalRows kLocalRowsA[] = {
    {false,0,4,8,"vs-b0-local-clip"},
    {true,1,90,1,"ps-b1-exposure"}, {true,1,123,1,"ps-b1-direction"},
    {true,1,126,1,"ps-b1-depth-scale"}, {true,1,210,1,"ps-b1-viewport-scale"},
    {true,2,2,6,"ps-b2-local-ray-and-scale"}
};
constexpr LocalRows kLocalRowsB[] = {
    {false,0,4,8,"vs-b0-local-clip"},
    {false,1,125,1,"vs-b1-local-origin"}, {false,1,275,1,"vs-b1-scene-origin"},
    {false,1,277,3,"vs-b1-camera-axes"}, {false,2,0,2,"vs-b2-local-scale"},
    {true,1,90,1,"ps-b1-exposure"}, {true,1,126,1,"ps-b1-depth-scale"},
    {true,1,210,1,"ps-b1-viewport-scale"}, {true,2,0,3,"ps-b2-material-scale"}
};
void hexWords(const unsigned char* bytes, uint32_t count, char* text, size_t capacity) {
    size_t used=0;
    for(uint32_t i=0;i<count && used<capacity;++i) {
        uint32_t word=0;std::memcpy(&word,bytes+i*4,4);
        const int n=std::snprintf(text+used,capacity-used,"%s%08X",i?",":"",word);
        if(n<=0 || static_cast<size_t>(n)>=capacity-used)break;
        used+=static_cast<size_t>(n);
    }
}
uint32_t captureCameraConflict(State& s, const FlatRuntimeDraw& draw) {
    // Run on the original game draw, before the observer records/refuses it and
    // before any private jitter or motion bindings. No per-draw work outside F10.
    const auto& k=draw.key;
    if(!s.projectionFrames || FlatCameraProbe::pairIndex(k.vs,k.ps)==kFlatCameraProbePairCount)return 0;
    const FlatRuntimeTarget* target=nullptr;
    for(uint32_t i=0;i<s.prefix.targetsUsed;++i)
        if(s.prefix.targets[i].resource==k.color){target=&s.prefix.targets[i];break;}
    const bool conflict=k.format==26 && k.camera && target && target->hdrCamera &&
        std::memcmp(target->tone.camera,draw.camera,kFlatCameraBytes)!=0;
    const uint32_t attempt=s.cameraProbe.begin(true,k.vs,k.ps,s.prefix.frame,conflict);
    if(!attempt)return 0;
    FlatComputeInternalScope internal;
    Ptr<ID3D11VertexShader> actualVs;Ptr<ID3D11PixelShader> actualPs;
    s.context->VSGetShader(&actualVs,nullptr,nullptr);s.context->PSGetShader(&actualPs,nullptr,nullptr);
    const uint64_t actualVh=lookupShaderHash(actualVs.Get()),actualPh=lookupShaderHash(actualPs.Get());
    const bool actualMatches=actualVh==k.vs && actualPh==k.ps;
    const auto& reference=target->tone;
    Log::get().note("flat camera probe draw: attempt=%u frame=%llu next-seq=%u actual-VS=%016llX actual-PS=%016llX actual-match=%u hdr=%p rtv=%p depth=%p dsv=%p named-depth=%p named-b1=%p reference-q=%u reference-VS=%016llX reference-PS=%016llX reference-b1=%p reference-epoch=%llu reference-write=%u current-b1=%p current-epoch=%llu current-write=%u prior-bad=%s viewport-count=%u viewport=(%.9g,%.9g,%.9g,%.9g,%.9g,%.9g)",
        attempt,(unsigned long long)s.prefix.frame,s.prefix.sequence+1,(unsigned long long)actualVh,(unsigned long long)actualPh,actualMatches?1u:0u,
        k.color,k.rtv,k.depth,k.dsv,s.namedDepth,s.namedConstants,reference.first,
        (unsigned long long)reference.key.vs,(unsigned long long)reference.key.ps,reference.key.b1,
        (unsigned long long)reference.key.writeEpoch,reference.key.writeSeq,k.b1,(unsigned long long)k.writeEpoch,k.writeSeq,
        flatRuntimeConflictName(target->firstBad.cause),k.viewportCount,k.viewport[0],k.viewport[1],k.viewport[2],k.viewport[3],k.viewport[4],k.viewport[5]);
    // These are frozen copies from their owning draws, never a late reread of
    // the reference's live CB (which may now contain the conflicting camera).
    const bool namedPresent=s.namedDepth && s.namedConstants;
    const unsigned char* cameras[]={reference.camera,draw.camera,s.namedCamera};
    const char* names[]={"reference-HDR-frozen","current-draw-frozen","named-scene-frozen"};
    for(uint32_t i=0;i<3;++i){
        char hex[24*9+1]{};const bool present=i!=2 || namedPresent;
        if(present)hexWords(cameras[i],24,hex,sizeof(hex));
        Log::get().note("flat camera probe camera: attempt=%u role=%s result=%s rows270-275=%s",
            attempt,names[i],present?"present":"unavailable",present?hex:"unavailable");
    }
    bool complete=actualMatches && namedPresent && s.projection && s.projectionContext;
    const UINT slots[]={0,1},rows[]={4,270},counts[]={4,6};
    for(uint32_t i=0;i<2;++i){
        Ptr<ID3D11Buffer> buffer;UINT first=0,count=0;
        if(s.projectionContext)s.projectionContext->VSGetConstantBuffers1(slots[i],1,&buffer,&first,&count);
        const bool inRange=buffer && rows[i]<=count && counts[i]<=count-rows[i] && first<=UINT32_MAX/16u-rows[i]-counts[i];
        FlatProjectionShadowMetadata metadata{};
        if(s.projection)metadata=s.projection->constantsMetadata(buffer.Get());
        unsigned char raw[kFlatCameraBytes]{};char hex[24*9+1]{};
        const bool copied=inRange && s.projection && s.projection->copyConstants(buffer.Get(),(first+rows[i])*16u,counts[i]*16u,raw);
        if(copied)hexWords(raw,counts[i]*4,hex,sizeof(hex));else complete=false;
        Log::get().note("flat camera probe rows: attempt=%u VS-b%u row=%u rows=%u buffer=%p first=%u bound-count=%u backing-byte=%u tracked=%u width=%u generation=%llu mutation=%llu mapped=%u pending=%u shadow=%u shadow-write=%llu shadow-epoch=%llu result=%s words=%s",
            attempt,slots[i],rows[i],counts[i],buffer.Get(),first,count,inRange?(first+rows[i])*16u:0u,
            metadata.tracked?1u:0u,metadata.width,(unsigned long long)metadata.generation,(unsigned long long)metadata.mutationSerial,
            metadata.mapped?1u:0u,metadata.pending?1u:0u,metadata.shadowPresent?1u:0u,
            (unsigned long long)metadata.writeGeneration,(unsigned long long)metadata.bankEpoch,
            !s.projectionContext?"no-context1":!buffer?"unbound":!inRange?"range-invalid":copied?"current-full-shadow":"missing-full-shadow",copied?hex:"unavailable");
    }
    Ptr<ID3D11DepthStencilState> depthState;UINT stencilRef=0;
    s.context->OMGetDepthStencilState(&depthState,&stencilRef);
    D3D11_DEPTH_STENCIL_DESC desc{};
    if(depthState)depthState->GetDesc(&desc);
    else {desc.DepthEnable=TRUE;desc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;desc.DepthFunc=D3D11_COMPARISON_LESS;
        desc.StencilReadMask=D3D11_DEFAULT_STENCIL_READ_MASK;desc.StencilWriteMask=D3D11_DEFAULT_STENCIL_WRITE_MASK;
        desc.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};desc.BackFace=desc.FrontFace;}
    Log::get().note("flat camera probe depth-state: attempt=%u state=%p default=%u depth-enable=%u depth-write=%u depth-func=%u stencil-enable=%u stencil-ref=%u read-mask=%u write-mask=%u front=(%u,%u,%u,%u) back=(%u,%u,%u,%u) result=%s",
        attempt,depthState.Get(),depthState?0u:1u,desc.DepthEnable?1u:0u,desc.DepthWriteMask,desc.DepthFunc,desc.StencilEnable?1u:0u,stencilRef,
        desc.StencilReadMask,desc.StencilWriteMask,desc.FrontFace.StencilFailOp,desc.FrontFace.StencilDepthFailOp,desc.FrontFace.StencilPassOp,desc.FrontFace.StencilFunc,
        desc.BackFace.StencilFailOp,desc.BackFace.StencilDepthFailOp,desc.BackFace.StencilPassOp,desc.BackFace.StencilFunc,complete?"complete":"partial");
    s.cameraProbe.finish(attempt,complete,actualMatches);
    return attempt;
}
void captureLocalProjection(State& s, uint64_t vs, uint64_t ps) {
    const uint32_t pair=vs==0x4D516EF05C68FFA5ull && ps==0x147E748F4CD3AE9Aull ? 0u :
        vs==0x5E417E9DF2E7F9E6ull && ps==0xBD801F2FB02522EBull ? 1u : 2u;
    if(pair==2 || !s.projection || !s.projectionContext || !s.projectionFrames)return;
    auto& sample=s.localSamples[pair];
    if(sample.attempts==2 || (sample.attempts && s.prefix.frame-sample.firstFrame<90))return;
    if(!sample.attempts)sample.firstFrame=s.prefix.frame;
    ++sample.attempts;
    FlatComputeInternalScope internal;
    Ptr<ID3D11VertexShader> actualVs;Ptr<ID3D11PixelShader> actualPs;
    s.context->VSGetShader(&actualVs,nullptr,nullptr);s.context->PSGetShader(&actualPs,nullptr,nullptr);
    if(lookupShaderHash(actualVs.Get())!=vs || lookupShaderHash(actualPs.Get())!=ps) {
        Log::get().note("flat local projection sample: pair=%u attempt=%u frame=%llu result=actual-shader-mismatch",
            pair,sample.attempts,(unsigned long long)s.prefix.frame);return;
    }
    const auto* rows=pair ? kLocalRowsB : kLocalRowsA;
    const size_t rowsCount=pair ? sizeof(kLocalRowsB)/sizeof(kLocalRowsB[0]) : sizeof(kLocalRowsA)/sizeof(kLocalRowsA[0]);
    uint32_t missing=0;
    const bool foreign=foreignWork.load(std::memory_order_acquire);
    const bool currentReference=s.namedDepth && s.namedConstants && !s.prefix.uncertain && !foreign;
    Log::get().note("flat local projection sample: pair=%u attempt=%u frame=%llu seq=%u VS=%016llX PS=%016llX named-depth=%p named-b1=%p current-reference=%u uncertain=%u foreign=%u",
        pair,sample.attempts,(unsigned long long)s.prefix.frame,s.prefix.sequence,
        (unsigned long long)vs,(unsigned long long)ps,s.namedDepth,s.namedConstants,
        currentReference?1u:0u,s.prefix.uncertain?1u:0u,foreign?1u:0u);
    char cameraHex[24*9+1]{};hexWords(s.namedCamera,24,cameraHex,sizeof(cameraHex));
    Log::get().note("flat local projection camera: pair=%u attempt=%u source-b1=%p rows270-275=%s; current only when named-depth and named-b1 are nonnull and frame is certain",
        pair,sample.attempts,s.namedConstants,cameraHex);
    for(size_t i=0;i<rowsCount;++i) {
        const auto& r=rows[i];Ptr<ID3D11Buffer> buffer;UINT first=0,constantCount=0;
        if(r.pixel)s.projectionContext->PSGetConstantBuffers1(r.slot,1,&buffer,&first,&constantCount);
        else s.projectionContext->VSGetConstantBuffers1(r.slot,1,&buffer,&first,&constantCount);
        unsigned char raw[8*16]{};char hex[8*4*9+1]{};
        const bool inRange=buffer && r.row<=constantCount && r.count<=constantCount-r.row &&
            first<=UINT32_MAX/16u-r.row-r.count;
        const bool copied=inRange && s.projection->copyConstants(buffer.Get(),(first+r.row)*16u,r.count*16u,raw);
        if(copied)hexWords(raw,r.count*4,hex,sizeof(hex));else ++missing;
        Log::get().note("flat local projection rows: pair=%u attempt=%u %s slot=%u shader-row=%u count=%u buffer=%p first=%u bound-count=%u backing-byte=%u result=%s words=%s",
            pair,sample.attempts,r.name,r.slot,r.row,r.count,buffer.Get(),first,constantCount,
            inRange?(first+r.row)*16u:0u,!buffer?"unbound":!inRange?"range-invalid":copied?"current-full-shadow":"missing-full-shadow",
            copied?hex:"unavailable");
    }
    Ptr<ID3D11RenderTargetView> rtv;Ptr<ID3D11DepthStencilView> dsv;
    Ptr<ID3D11ShaderResourceView> depthSrv;
    // Both exact pixel shaders write only o0; RT slot 0 is the relevant HDR
    // output. t0 is their depth-comparison input.
    s.context->OMGetRenderTargets(1,&rtv,&dsv);s.context->PSGetShaderResources(0,1,&depthSrv);
    Ptr<ID3D11Resource> rtResource,dsResource,depthResource;
    if(rtv)rtv->GetResource(&rtResource);if(dsv)dsv->GetResource(&dsResource);
    if(depthSrv)depthSrv->GetResource(&depthResource);
    D3D11_RENDER_TARGET_VIEW_DESC rtView{};D3D11_DEPTH_STENCIL_VIEW_DESC dsView{};
    D3D11_SHADER_RESOURCE_VIEW_DESC srvView{};
    if(rtv)rtv->GetDesc(&rtView);if(dsv)dsv->GetDesc(&dsView);
    if(depthSrv)depthSrv->GetDesc(&srvView);
    Ptr<ID3D11Texture2D> rtTexture,dsTexture,depthTexture;
    if(rtResource)rtResource.As(&rtTexture);if(dsResource)dsResource.As(&dsTexture);
    if(depthResource)depthResource.As(&depthTexture);
    D3D11_TEXTURE2D_DESC rtDesc{},dsDesc{},depthDesc{};
    if(rtTexture)rtTexture->GetDesc(&rtDesc);if(dsTexture)dsTexture->GetDesc(&dsDesc);
    if(depthTexture)depthTexture->GetDesc(&depthDesc);
    const uint32_t attemptIndex=sample.attempts-1;
    sample.frames[attemptIndex]=s.prefix.frame;
    sample.color[attemptIndex]=rtResource.Get();sample.depth[attemptIndex]=dsResource.Get();
    uint32_t targetIndex=UINT32_MAX,targetDraws=0,targetFirst=0,targetLast=0,targetTones=0;
    const void* targetDepth=nullptr;
    for(uint32_t i=0;i<s.prefix.targetsUsed;++i)if(s.prefix.targets[i].resource==rtResource.Get()) {
        targetIndex=i;const auto& target=s.prefix.targets[i];
        targetDraws=target.writes.draws;targetFirst=target.writes.first;
        targetLast=target.writes.last;targetTones=target.tones;
        targetDepth=target.writes.key.depth;break;
    }
    const UINT rtMip=rtView.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D?rtView.Texture2D.MipSlice:
        rtView.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2DARRAY?rtView.Texture2DArray.MipSlice:0u;
    const UINT dsMip=dsView.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2D?dsView.Texture2D.MipSlice:
        dsView.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2DARRAY?dsView.Texture2DArray.MipSlice:0u;
    const UINT srvMip=srvView.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D?srvView.Texture2D.MostDetailedMip:
        srvView.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DARRAY?srvView.Texture2DArray.MostDetailedMip:0u;
    const UINT rtArray=rtView.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2DARRAY?rtView.Texture2DArray.FirstArraySlice:0u;
    const UINT dsArray=dsView.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2DARRAY?dsView.Texture2DArray.FirstArraySlice:0u;
    const UINT srvArray=srvView.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DARRAY?srvView.Texture2DArray.FirstArraySlice:0u;
    UINT viewportCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    s.context->RSGetViewports(&viewportCount,viewports);
    const D3D11_VIEWPORT vp=viewportCount?viewports[0]:D3D11_VIEWPORT{};
    Log::get().note("flat local projection targets: pair=%u attempt=%u rtv=%p rt=%p view-fmt=%u dim=%u mip=%u slice=%u tex=%ux%u fmt=%u mips=%u array=%u samples=%u dsv=%p ds=%p view-fmt=%u dim=%u mip=%u slice=%u tex=%ux%u fmt=%u mips=%u array=%u samples=%u ps-t0=%p depth=%p view-fmt=%u dim=%u mip=%u slice=%u tex=%ux%u fmt=%u mips=%u array=%u samples=%u depth-is-named=%u",
        pair,sample.attempts,rtv.Get(),rtResource.Get(),(uint32_t)rtView.Format,(uint32_t)rtView.ViewDimension,
        rtMip,rtArray,
        rtDesc.Width,rtDesc.Height,(uint32_t)rtDesc.Format,rtDesc.MipLevels,rtDesc.ArraySize,rtDesc.SampleDesc.Count,
        dsv.Get(),dsResource.Get(),(uint32_t)dsView.Format,(uint32_t)dsView.ViewDimension,
        dsMip,dsArray,
        dsDesc.Width,dsDesc.Height,(uint32_t)dsDesc.Format,dsDesc.MipLevels,dsDesc.ArraySize,dsDesc.SampleDesc.Count,
        depthSrv.Get(),depthResource.Get(),(uint32_t)srvView.Format,(uint32_t)srvView.ViewDimension,
        srvMip,srvArray,
        depthDesc.Width,depthDesc.Height,(uint32_t)depthDesc.Format,depthDesc.MipLevels,depthDesc.ArraySize,
        depthDesc.SampleDesc.Count,depthResource.Get()==s.namedDepth?1u:0u);
    Log::get().note("flat local projection viewport: pair=%u attempt=%u count=%u first=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g missing-shadow-slices=%u result=%s",
        pair,sample.attempts,viewportCount,vp.TopLeftX,vp.TopLeftY,vp.Width,vp.Height,vp.MinDepth,vp.MaxDepth,
        missing,missing?"partial-shadow":"complete-shadow-slices");
    const bool targetValid=rtv && dsv && depthSrv && rtTexture && dsTexture && depthTexture;
    Log::get().note("flat local projection relation: pair=%u attempt=%u frame=%llu prefix-target=%u prefix-draws=%u first=%u last=%u tones=%u target-depth=%p actual-rt=%p actual-dsv-depth=%p ps-t0-depth=%p named-depth=%p target-valid=%u shadow-valid=%u result=%s; final HDR selection is reported at handoff for this frame",
        pair,sample.attempts,(unsigned long long)s.prefix.frame,targetIndex,targetDraws,targetFirst,targetLast,targetTones,
        targetDepth,rtResource.Get(),dsResource.Get(),depthResource.Get(),s.namedDepth,
        targetValid?1u:0u,missing?0u:1u,targetValid && !missing?"complete-capture":"partial-capture");
    if(targetValid && !missing)++sample.complete;
}
void recordProjectionReference(State& s, State::AuditOutcome& outcome,
                               const FlatProjectionRecipes& recipes, bool depthAssociated) {
    // Compare only the primary forward recipe. Inverse/lighting reconstruction
    // and embedded local cameras need their own contracts; never infer them.
    const auto& request=recipes.requests[0];
    const auto& patch=request.patches[0];
    FlatProjectionOwnershipInput input{};
    uint32_t bytes=0;
    if(request.stage==FlatProjectionStage::Vertex && request.slot==1 &&
       patch.byteOffset==270u*16u && patch.layout==FlatProjectionPatchLayout::ForwardColumns) {
        input.layout=FlatProjectionOwnershipLayout::CanonicalVsB1;bytes=kFlatCameraBytes;
    } else if(request.stage==FlatProjectionStage::Vertex && patch.layout==FlatProjectionPatchLayout::ForwardDp4) {
        input.layout=FlatProjectionOwnershipLayout::ForwardDp4;bytes=64;
    }
    unsigned char raw[kFlatCameraBytes]{};
    input.referenceBuffer=s.namedConstants;input.referenceCamera=s.namedCamera;
    input.referenceBytes=sizeof(s.namedCamera);
    input.currentReference=depthAssociated && s.namedDepth && s.namedConstants && !s.prefix.uncertain &&
        !foreignWork.load(std::memory_order_acquire);
    input.candidateBuffer=request.original;input.candidateRows=raw;input.candidateBytes=bytes;
    input.currentCandidate=bytes && s.projection->copyConstants(request.original,patch.byteOffset,bytes,raw);
    const auto result=flatClassifyProjectionOwnership(input);
    switch(result.kind) {
    case FlatProjectionOwnershipKind::CanonicalSceneCamera: ++outcome.canonical; break;
    case FlatProjectionOwnershipKind::SceneBasisMatch: ++outcome.basisMatch; break;
    case FlatProjectionOwnershipKind::Unmatched: ++outcome.unmatched; break;
    case FlatProjectionOwnershipKind::Unavailable: ++outcome.unavailable; break;
    case FlatProjectionOwnershipKind::Unsupported: ++outcome.unsupported; break;
    }
    if(result.residualsAvailable) {
        ++outcome.residualSamples;
        if(result.spatialDepthError>outcome.spatialDepthError)outcome.spatialDepthError=result.spatialDepthError;
        if(result.translationResidual>outcome.translationResidual)outcome.translationResidual=result.translationResidual;
    }
}
// Generic admission: classify the actual creation bytecode once per (vs,ps)
// pair. Missing blobs classify as NoBytecode and keep the existing refuse
// path; a full memo table also keeps it (no per-draw classification).
const char* flatVsClassName(FlatVsProjectionClass c) {
    switch (c) {
    case FlatVsProjectionClass::NoBytecode: return "no-bytecode";
    case FlatVsProjectionClass::InertNoCB: return "inert-no-cb";
    case FlatVsProjectionClass::ForwardColumns: return "forward-columns";
    case FlatVsProjectionClass::ForwardDp4: return "forward-dp4";
    case FlatVsProjectionClass::Unclassified: return "unclassified";
    }
    return "unknown";
}
const char* flatPsSafetyName(FlatPsProjectionSafety p) {
    switch (p) {
    case FlatPsProjectionSafety::NoBytecode: return "no-bytecode";
    case FlatPsProjectionSafety::Clean: return "clean";
    case FlatPsProjectionSafety::Consumer: return "consumer";
    }
    return "unknown";
}
FlatShaderPairClassification classifyFlatProjectionPair(State& s, uint64_t vs, uint64_t ps) {
    for (uint32_t i = 0; i < s.genericClassificationsUsed; ++i) {
        auto& entry = s.genericClassifications[i];
        if (entry.vs == vs && entry.ps == ps) {
            entry.lastSeenFrame = s.prefix.frame;
            return entry.classification;
        }
    }
    State::GenericClassification* slot = nullptr;
    if (s.genericClassificationsUsed < 64) {
        slot = &s.genericClassifications[s.genericClassificationsUsed++];
    } else {
        // LRU retirement (section 72 step 3): the memo never stops
        // classifying; the least-recently-seen entry yields its slot and its
        // pair simply reclassifies if it returns.
        slot = &s.genericClassifications[0];
        for (uint32_t i = 1; i < 64; ++i)
            if (s.genericClassifications[i].lastSeenFrame < slot->lastSeenFrame)
                slot = &s.genericClassifications[i];
        ++s.covMemoEvictions;
        if (!s.genericClassificationOverflowLogged) {
            s.genericClassificationOverflowLogged = true;
            Log::get().note("flat generic classification: memo evicts the least-recently-seen pair at 64 entries (frame=%llu); an evicted pair reclassifies on return (coverage line memo-evictions=)",
                (unsigned long long)s.prefix.frame);
        }
    }
    const uint8_t* vsBytes = nullptr; size_t vsLen = 0;
    const uint8_t* psBytes = nullptr; size_t psLen = 0;
    const bool vsFound = flatProbeShaderLookup('v', vs, &vsBytes, &vsLen);
    const bool psFound = flatProbeShaderLookup('p', ps, &psBytes, &psLen);
    // A missing stage classifies as NoBytecode, which admits nothing; the
    // present stage is still classified so its own reason reaches the log.
    FlatShaderPairClassification result = classifyFlatShaderPair(vsFound ? vsBytes : nullptr, vsFound ? vsLen : 0,
                                    psFound ? psBytes : nullptr, psFound ? psLen : 0);
    slot->vs = vs; slot->ps = ps; slot->classification = result; slot->lastSeenFrame = s.prefix.frame;
    // The verdict and the rule behind it, logged once per pair per session
    // even across memo evictions (the 256-entry FIFO, not the memo, owns
    // that guarantee) -- a refused pair needs no offline review to name.
    const uint64_t pairHash = (vs ^ (ps * 1099511628211ull)) | 1ull;
    bool verdictSeen = false;
    for (uint32_t i = 0; i < 256; ++i) if (s.classificationLogged[i] == pairHash) { verdictSeen = true; break; }
    if (!verdictSeen) {
        s.classificationLogged[s.classificationLoggedNext] = pairHash;
        s.classificationLoggedNext = (s.classificationLoggedNext + 1) % 256;
        const bool clean = result.ps == FlatPsProjectionSafety::Clean;
        const char* verdict = clean && (result.vs == FlatVsProjectionClass::ForwardColumns ||
                                        result.vs == FlatVsProjectionClass::ForwardDp4) ? "generic-recipe" :
                              clean && result.vs == FlatVsProjectionClass::InertNoCB ? "generic-inert" : "refused";
        char vsExtra[32] = "", psExtra[48] = "";
        if (result.vsReason == FlatClassifierReason::UnknownOpcode)
            std::snprintf(vsExtra, sizeof(vsExtra), " vs-opcode=%u", unsigned(result.vsUnknownOpcode));
        if (result.psReason == FlatClassifierReason::UnknownOpcode)
            std::snprintf(psExtra, sizeof(psExtra), " ps-opcode=%u", unsigned(result.psUnknownOpcode));
        else if (result.psReason == FlatClassifierReason::VposConsumer)
            std::snprintf(psExtra, sizeof(psExtra), " ps-rule=%s", flatVposConsumerSubcodeName(result.psConsumerSubcode));
        Log::get().note("flat generic classification: frame=%llu VS=%016llX PS=%016llX verdict=%s vs=%s slot=%u row=%u vs-reason=%s%s ps=%s ps-reason=%s%s",
            (unsigned long long)s.prefix.frame, (unsigned long long)vs, (unsigned long long)ps, verdict,
            flatVsClassName(result.vs), result.vsSlot, result.vsRow, flatClassifierReasonName(result.vsReason), vsExtra,
            flatPsSafetyName(result.ps), flatClassifierReasonName(result.psReason), psExtra);
    }
    return result;
}
// --- Partial temporal AA ("local refusal") --------------------------------
// Redesigned 2026-09-26 per docs/review-flat-temporal-aa-2026-09-26.md
// findings 1, 4 and 5: one raster phase across shared scene depth/colour.
// A scene draw that cannot be jittered for a reason specific to that one
// draw still goes out unjittered (the proxy cannot stop the game's own
// draw); the frame's history is invalidated and the runtime returns to
// observation until a refusal-free frame requalifies the contract. It is
// never claimed as treated.

// Part B per-pair breakdown of the top locally refused (VS,PS,reason)
// triples this window. Overflow past this fixed table only drops out of the
// top-5 breakdown; the aggregate covLocalRefused counter still counts it.
void recordLocallyRefusedPair(State& s, uint64_t vs, uint64_t ps, const char* reason) {
    for (uint32_t i = 0; i < s.covRefusedPairsUsed; ++i) {
        auto& e = s.covRefusedPairs[i];
        if (e.vs == vs && e.ps == ps && std::strcmp(e.reason, reason) == 0) { ++e.draws; return; }
    }
    if (s.covRefusedPairsUsed < 32) {
        auto& e = s.covRefusedPairs[s.covRefusedPairsUsed++];
        e.vs = vs; e.ps = ps; e.reason = reason; e.draws = 1;
    }
}

// Local-refusal entry point for a per-draw-local reason on a SCENE DRAW.
// qualifyProjection's cs!=0 (compute/dispatch) callers must never reach
// this -- they call failPhase directly, gated on cs at each of its three
// call sites -- and every other caller here is a scene draw by construction.
void refuseDraw(State& s, const char* reason) {
    // The upstream camera injector's admission (the C3 plan's R6): on a
    // certified camera lineage under Upstream ownership, an unknown recipe
    // is admitted through the camera, not the shader recipe -- no phase
    // failure, no observation entry. Legacy observation keeps collecting
    // its own evidence with no veto over the certified route.
    if (flatCameraInjectBypassRefusal(reason)) {
        ++s.covInert;
        return;
    }
    if (!flatLocalRefusalReason(reason)) { failPhase(s, reason); return; }
    ++s.covLocalRefused;
    s.covFrameLocallyRefused = true;
    recordLocallyRefusedPair(s, s.drawVs, s.drawPs, reason);
    // The current frame fails coherently either way: zero phase when nothing
    // jittered yet, the spatial fallback otherwise -- never a mixed-phase
    // temporal evaluation.
    failPhase(s, reason);
    if (!s.observing) {
        s.observing = true;
        ++s.covObservationEntries;
        Log::get().note("flat coverage: returned to observation at frame=%llu reason=%s VS=%016llX PS=%016llX; treatment resumes after a refusal-free frame",
            (unsigned long long)s.prefix.frame, reason,
            (unsigned long long)s.drawVs, (unsigned long long)s.drawPs);
    }
}

const FlatProjectionBindingPlan* qualifyProjection(State& s, FlatProjectionRecipes recipes, uint32_t width, uint32_t height,
                       uint64_t vs, uint64_t ps, uint64_t cs, bool owned, bool sceneHdr = false) {
    if (!s.projection || !recipes.count) return nullptr;
    flatcpu::Scope timed(flatcpu::kProjection);   // projection readiness: the checks, preflight and prepare
    const bool audit=s.projectionFrames!=0;
    if(audit) { ++s.projectionCandidates;if(!owned)++s.projectionUnowned; }
    // A previous qualified frame names early depth prepasses before this
    // frame's first scene-camera draw. Never select a camera by matrix equality.
    if(!owned) return nullptr;
    if(nonzeroPhase(s) && (width!=s.phaseWidth || height!=s.phaseHeight)) {
        failPhase(s,"render-extent-changed");return nullptr;
    }
    // Under Upstream ownership nothing consumes the legacy preparation: the
    // camera was jittered at the source, and the scope that would bind private
    // rows is suppressed on both the draw and the dispatch path. Private
    // buffers, shader-identity qualification and their refusals would only
    // cost work and could veto a route that does not use them (the 08:58:57
    // projection-preparation-refused observation entry under Upstream). The
    // depth and extent checks above still run. The F10 audit runs the whole
    // qualification regardless: it is evidence, not treatment.
    if(!audit && flatCameraInjectUpstreamOwns()) { ++s.rows.legacyPrepSkipped; return nullptr; }
    FlatComputeInternalScope internal;
    // Recipe hashes come from the observer; verify actual shaders before
    // trusting them in a modded context. Binding happens in the command scope.
    Ptr<ID3D11VertexShader> actualVs; Ptr<ID3D11PixelShader> actualPs; Ptr<ID3D11ComputeShader> actualCs;
    bool shadersMatch = false;
    if (cs) { s.context->CSGetShader(&actualCs,nullptr,nullptr); shadersMatch=lookupShaderHash(actualCs.Get())==cs; }
    else { s.context->VSGetShader(&actualVs,nullptr,nullptr); s.context->PSGetShader(&actualPs,nullptr,nullptr);
        shadersMatch=lookupShaderHash(actualVs.Get())==vs && lookupShaderHash(actualPs.Get())==ps; }
    if (!shadersMatch) {
        if(audit) { ++s.projectionMissing;projectionDetail(s,vs,ps,cs,100,"actual-shader-mismatch"); }
        // Local refusal only on the draw path (cs==0); the dispatch path
        // stays frame-global.
        if(!cs) refuseDraw(s,"actual-shader-mismatch"); else failPhase(s,"actual-shader-mismatch");
        return nullptr;
    }
    if(!cs) {
        UINT count=1;D3D11_VIEWPORT viewport{};s.context->RSGetViewports(&count,&viewport);
        if(audit)++s.projectionViewportChecks;
        const float actualViewport[]={viewport.TopLeftX,viewport.TopLeftY,viewport.Width,
                                      viewport.Height,viewport.MinDepth,viewport.MaxDepth};
        if(!flatRuntimeProjectionViewport(count,actualViewport,width,height,sceneHdr)) {
            if(audit)recordProjectionViewportFailure(s,vs,ps,cs,width,height,count,viewport);
            // Only reachable with cs==0 (the enclosing !cs block), but spelled
            // out the same way as the other two qualifyProjection refusals.
            if(!cs) refuseDraw(s,"projection-viewport-mismatch"); else failPhase(s,"projection-viewport-mismatch");
            return nullptr;
        }
    }
    Ptr<ID3D11Buffer> buffers[3];
    for(uint32_t i=0;i<recipes.count;++i) {
        auto& request=recipes.requests[i];
        switch(request.stage) {
        case FlatProjectionStage::Vertex: s.projectionContext->VSGetConstantBuffers1(request.slot,1,&buffers[i],&request.firstConstant,&request.constantCount); break;
        case FlatProjectionStage::Pixel: s.projectionContext->PSGetConstantBuffers1(request.slot,1,&buffers[i],&request.firstConstant,&request.constantCount); break;
        case FlatProjectionStage::Compute: s.projectionContext->CSGetConstantBuffers1(request.slot,1,&buffers[i],&request.firstConstant,&request.constantCount); break;
        }
        request.original=buffers[i].Get();
        for(uint32_t p=0;p<request.patchCount;++p)
            if(request.patches[p].layout==FlatProjectionPatchLayout::LightingUvRay) {
                request.patches[p].lighting.pixelX=s.phase.currentX;
                request.patches[p].lighting.pixelY=s.phase.currentY;
            }
    }
    FlatProjectionJitter proposed{};
    if(!flatProjectionJitter(s.phase.currentX,s.phase.currentY,width,height,proposed)) {
        failPhase(s,"invalid-render-extent");return nullptr;
    }
    const bool live=nonzeroPhase(s);
    bool ready=s.projection->preflight(recipes.requests,recipes.count,proposed,s.phase.phaseSequence,!live);
    const FlatProjectionBindingPlan* plan=nullptr;
    if(ready && live) { plan=s.projection->prepare(recipes.requests,recipes.count,proposed,s.phase.phaseSequence);ready=plan!=nullptr; }
    if(ready) {
        if(audit) {
            ++s.projectionReady;
            if(auto* outcome=projectionDetail(s,vs,ps,cs,0,"prepared"))recordProjectionReference(s,*outcome,recipes,owned);
        }
        return plan;
    }
    reportProjectionFailure(s,recipes,vs,ps,cs);
    if(audit) { ++s.projectionMissing;projectionDetail(s,vs,ps,cs,static_cast<uint32_t>(s.projection->status().last),"private-preparation-refused"); }
    // Local refusal only on the draw path (cs==0); the dispatch path stays
    // frame-global.
    if(!cs) refuseDraw(s,"projection-preparation-refused"); else failPhase(s,"projection-preparation-refused");
    return nullptr;
}
void reset() { auto& s = state(); s.havePrevious = false; FlatComputeInternalScope guard; flatMonoResolveInvalidateHistory(); }
void overlayFail(State& s, const char* reason, const void* hdr = nullptr) {
    if(s.engine!=FlatMonoResolveMode::Taa)
        for(auto& candidate:s.foregroundCandidates)
            if(candidate.frame==s.prefix.frame && (!hdr || candidate.hdr.Get()==hdr))
                candidate.motion.fail("foreground-private-overlay-failed");
    if (s.overlayFailureNoted) return;
    s.overlayFailureNoted = true;
    overlaySuffixActive.store(false,std::memory_order_release);
    ++s.overlayRefusedWindow;
    ++s.overlayRefusalWindow[reason?reason:"overlay-suffix-refused"];
    flatTraceMark(s.traceRing,kFlatTraceEventOverlayFailed,hdr);
    s.overlay.invalidate(reason);
    flatRuntimeOverlayFailed(s.prefix, hdr);
    failPhase(s, reason ? reason : "overlay-suffix-refused");
    reset();
}
bool overlayOpen(const State& s) {
    if(!overlaySuffixActive.load(std::memory_order_relaxed))return false;
    for (uint32_t i = 0; i < s.prefix.targetsUsed; ++i)
        if (s.prefix.targets[i].overlayOpen) return true;
    return false;
}
void refuse(State& s) {
    ++s.refused; ++s.refusedWindow[s.reason]; s.streak = 0; reset();
}
void reportConflict(State& s) {
    const auto& w = s.prefix.selectedConflict;
    const auto index = static_cast<uint32_t>(w.cause);
    if (!index || index >= static_cast<uint32_t>(FlatRuntimeConflict::Count)) return;
    ++s.conflictWindow[flatRuntimeConflictName(w.cause)];
    const uint64_t now = GetTickCount64();
    if (s.conflictDetails[index] >= 4 ||
        (s.conflictDetails[index] && now - s.lastConflictDetailMs[index] < 10000)) return;
    ++s.conflictDetails[index]; s.lastConflictDetailMs[index] = now;
    const auto& a = w.reference; const auto& b = w.current;
    uint32_t rowMask = 0, wordMask = 0;
    char words[700]{}; size_t used = 0;
    if (a.hasCamera && b.hasCamera) for (uint32_t i = 0; i < 24; ++i) {
        uint32_t before = 0, after = 0;
        std::memcpy(&before, a.camera + i * 4, 4);
        std::memcpy(&after, b.camera + i * 4, 4);
        if (before == after) continue;
        rowMask |= 1u << (i / 4); wordMask |= 1u << i;
        const int n = std::snprintf(words + used, sizeof(words) - used,
            "%s%u.%u:%08X>%08X", used ? "," : "", 270 + i / 4, i % 4, before, after);
        if (n > 0 && static_cast<size_t>(n) < sizeof(words) - used) used += static_cast<size_t>(n);
    }
    Log::get().note("flat runtime conflict: frame=%llu cause=%s seq=%u hdr=%p ref-q=%u cur-q=%u "
        "ref-vs=%016llX ref-ps=%016llX cur-vs=%016llX cur-ps=%016llX "
        "ref-rtv=%p ref-depth=%p ref-dsv=%p ref-dim=%ux%u/%ux%u ref-fmt=%u/%u "
        "cur-rtv=%p cur-depth=%p cur-dsv=%p cur-dim=%ux%u/%ux%u cur-fmt=%u/%u "
        "ref-vp=%u(%.3g,%.3g,%.3g,%.3g,%.3g,%.3g) cur-vp=%u(%.3g,%.3g,%.3g,%.3g,%.3g,%.3g) "
        "ref-b1=%p ref-cam=%u ref-epoch=%llu ref-write=%u cur-b1=%p cur-cam=%u cur-epoch=%llu cur-write=%u",
        static_cast<unsigned long long>(s.prefix.frame), flatRuntimeConflictName(w.cause), w.sequence, w.hdr,
        a.first, b.first,
        static_cast<unsigned long long>(a.vs), static_cast<unsigned long long>(a.ps),
        static_cast<unsigned long long>(b.vs), static_cast<unsigned long long>(b.ps),
        a.rtv, a.depth, a.dsv, a.width, a.height, a.depthWidth, a.depthHeight, a.format, a.depthFormat,
        b.rtv, b.depth, b.dsv, b.width, b.height, b.depthWidth, b.depthHeight, b.format, b.depthFormat,
        a.viewportCount, a.viewport[0], a.viewport[1], a.viewport[2], a.viewport[3], a.viewport[4], a.viewport[5],
        b.viewportCount, b.viewport[0], b.viewport[1], b.viewport[2], b.viewport[3], b.viewport[4], b.viewport[5],
        a.b1, a.hasCamera ? 1u : 0u, static_cast<unsigned long long>(a.writeEpoch), a.writeSeq,
        b.b1, b.hasCamera ? 1u : 0u, static_cast<unsigned long long>(b.writeEpoch), b.writeSeq);
    Log::get().note("flat runtime conflict camera: frame=%llu cause=%s seq=%u ref-hash=%016llX cur-hash=%016llX row-mask=%02X word-mask=%06X changed-words=%s",
        static_cast<unsigned long long>(s.prefix.frame), flatRuntimeConflictName(w.cause), w.sequence,
        static_cast<unsigned long long>(a.cameraHash), static_cast<unsigned long long>(b.cameraHash),
        rowMask, wordMask, a.hasCamera && b.hasCamera ? words : "unavailable");
}
ResourceInfo view(BindSlot slot, uint32_t cache) {
    auto& v = state().views[cache]; const auto generation = bindingGeneration(slot);
    void* identity = bindingGet(slot);
    if (v.identity != identity || v.generation != generation) {
        v.held.Reset(); v.identity = identity; v.generation = generation; v.info = {};
        if (identity && bindingResolve(identity, &v.info)) v.held = static_cast<IUnknown*>(identity);
    }
    return v.info;
}
// The table's entry for `resource`, or, with `add`, a new one when it is a constant buffer wide enough for the
// camera rows. For reading: an entry is changed only through the table (state().cameras), never through this pointer.
const Camera* camera(ID3D11Resource* resource, bool add) {
    auto& s = state();
    if (const Camera* known = s.cameras.find(resource)) return known;
    if (!add || !resource) return nullptr;
    Ptr<ID3D11Buffer> buffer; if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))) return nullptr;
    D3D11_BUFFER_DESC d{}; buffer->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || d.ByteWidth < kFlatCameraOffset + kFlatCameraBytes) return nullptr;
    const Camera* claimed = s.cameras.claim(std::move(buffer), d.ByteWidth, s.prefix.frame);
    if (!claimed) { s.prefix.uncertain = true; flatTraceMark(s.traceRing, kFlatTraceEventMarkUncertain, nullptr); return nullptr; }
    return claimed;
}
void capture(const Camera& c, const void* bytes) {
    flatcpu::Scope timed(flatcpu::kCameraRows);   // the camera data motion correctness needs, apart from the witness
    auto& s = state();
    s.cameras.capture(c, bytes, s.prefix.frame, ++s.prefix.sequence);
    { flatcpu::Scope trace(flatcpu::kTrace); flatTraceMark(s.traceRing, kFlatTraceEventCameraCapture, nullptr); }
}
// The camera producer witness (design-flat-camera-integration.md, C0/C1):
// where the camera table already captures a camera CB write, capture the
// writer's stack once per unique game call site -- the C1 passive evidence
// for the producer hypotheses, bounded and passive with no mutation. The
// first frame outside EDVR's own image is the game's upload call site; one
// stack per site, up to 16 sites and three buffers per site. Overflow is a
// named line, never silence.
struct CameraWitnessSite { void* address = nullptr; const void* buffers[3]{}; uint32_t bufferCount = 0; uint64_t writes = 0; };
struct CameraWitness {
    CameraWitnessSite sites[16]{};
    uint64_t writes = 0, dedupHits = 0, budgetDropped = 0;
    bool budgetNoted = false;
    // Once every site slot is claimed the producer population is presumed
    // mapped, and the expensive part -- a stack walk plus module queries on
    // the render thread, measured at microseconds per call over hundreds of
    // thousands of writes -- stops. Counts stay cheap and keep coming.
    bool sitesFull = false;
    // The walks are bounded (flat_witness_bound.h; the 2026-09-29 motion-CPU review, C1): a game with
    // fewer producers than slots never fills them, and the walk ran on every camera write of the
    // session. It now stops after a run of walks that learned nothing, or a budget of walks in all,
    // and an F10 audit re-arms it. The camera data itself (capture()) never depends on any of this.
    FlatWitnessBound bound;
};
CameraWitness g_camWitness;
// Brief module-plus-offset naming for witness stacks (vtable_hook.cpp's
// ownerModuleBrief stays internal to it; this is the same formatting with
// EDVR's own image prefixed so a stack never blames the proxy by mistake).
const char* witnessModuleBrief(void* p, char* buf, size_t bufLen) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) || !mbi.AllocationBase) return "no module";
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), path, sizeof(path))) return "no module";
    const char* leaf = path;
    for (const char* c = path; *c; ++c) if (*c == '\\' || *c == '/') leaf = c + 1;
    static HMODULE self = nullptr;
    if (!self) {
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&witnessModuleBrief), &self);
    }
    _snprintf_s(buf, bufLen, _TRUNCATE, "%s%s+0x%llX",
                (self && mbi.AllocationBase == self) ? "EDVR's own " : "", leaf,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p) -
                                                reinterpret_cast<uintptr_t>(mbi.AllocationBase)));
    return buf;
}
// One stack walk. True when it learned something: a producer site the witness did not know, or a new
// buffer at a known one. The caller (cameraWitness) decides whether a walk is wanted at all.
bool witnessWalk(const void* buffer) {
    auto& w = g_camWitness;
    void* frames[10] = {};
    const USHORT n = CaptureStackBackTrace(0, 10, frames, nullptr);
    void* site = nullptr;
    for (USHORT i = 0; i < n; ++i) {
        if (!isExecutableAddress(frames[i])) continue;
        char briefBuf[96];
        const char* brief = witnessModuleBrief(frames[i], briefBuf, sizeof(briefBuf));
        if (std::strncmp(brief, "EDVR's own ", 11) == 0) continue;
        site = frames[i]; break;
    }
    if (!site) { ++w.dedupHits; return false; }
    for (auto& s : w.sites) if (s.address == site) {
        ++s.writes; ++w.dedupHits;
        bool known = false;
        for (uint32_t b = 0; b < s.bufferCount; ++b) known |= s.buffers[b] == buffer;
        if (!known && s.bufferCount < 3) {
            s.buffers[s.bufferCount++] = buffer;
            char briefBuf[96];
            const char* brief = witnessModuleBrief(site, briefBuf, sizeof(briefBuf));
            Log::get().note("flat camera witness: producer site %s also writes %p", brief, buffer);
            return true;
        }
        return false;
    }
    for (auto& s : w.sites) if (!s.address) {
        s.address = site; s.writes = 1; s.buffers[0] = buffer; s.bufferCount = 1;
        char line[768]{}; size_t used = 0;
        for (USHORT i = 0; i < n && used < sizeof(line) - 96; ++i) {
            if (!isExecutableAddress(frames[i])) continue;
            char briefBuf[96];
            const char* brief = witnessModuleBrief(frames[i], briefBuf, sizeof(briefBuf));
            used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, "%s%s", used ? " <- " : "", brief));
        }
        Log::get().note("flat camera witness: new producer site for buffer=%p: %s", buffer, line);
        if (&s == &w.sites[15]) {
            // The last slot just filled: stop the per-write stack walks.
            w.sitesFull = true; w.budgetNoted = true;
            Log::get().note("flat camera witness: all 16 producer site slots are claimed; "
                            "stack collection stops here, write counts continue");
        }
        return true;
    }
    if (!w.budgetNoted) {
        w.budgetNoted = true;
        Log::get().note("flat camera witness: site budget reached; later writers counted without stacks");
    }
    ++w.budgetDropped;
    return false;
}
// Every camera constant-buffer write that reaches the table. The count is cheap and never stops; the
// stack walk behind it is bounded (flat_witness_bound.h) and re-armed by an F10 audit.
void cameraWitness(const void* buffer) {
    flatcpu::Scope timed(flatcpu::kWitness);
    auto& w = g_camWitness; ++w.writes;
    if (w.sitesFull || !w.bound.wantsWalk()) { ++w.dedupHits; return; }
    const bool learned = witnessWalk(buffer);
    const FlatWitnessStop stopped = w.bound.noteWalk(learned);
    if (stopped != FlatWitnessStop::None && !w.sitesFull) {
        uint64_t sites = 0; for (const auto& st : w.sites) sites += st.address != nullptr;
        Log::get().note("flat camera witness: stack collection stopped after %u walks over %llu writes (%s); "
                        "%llu producer sites known; write counts continue; an F10 audit re-arms it",
            w.bound.walks, static_cast<unsigned long long>(w.writes), FlatWitnessBound::stopName(stopped),
            static_cast<unsigned long long>(sites));
    }
}
// An F10 audit asks for a fresh look: the sites are forgotten and the walks come back, up to the bound.
// The write counts are cumulative and go on.
void witnessRearm() {
    auto& w = g_camWitness;
    for (auto& site : w.sites) site = CameraWitnessSite{};
    w.sitesFull = false; w.budgetNoted = false;
    w.bound.rearm();
    Log::get().note("flat camera witness: re-armed by an F10 audit; sites forgotten, up to %u stack walks, "
                    "stopping after %u in a row that learn nothing",
        kFlatWitnessWalkBudget, kFlatWitnessStableWalks);
}
bool depthView(ID3D11Texture2D* depth) {
    auto& s = state(); if (s.sceneDepth.Get() == depth && s.depthView) return true;
    s.sceneDepth.Reset(); s.depthView.Reset(); if (!depth) return false;
    D3D11_TEXTURE2D_DESC d{}; depth->GetDesc(&d); D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
    v.Format = static_cast<DXGI_FORMAT>(flatRuntimeDepthReadFormat(d.Format));
    if (v.Format == DXGI_FORMAT_UNKNOWN) return false;
    if (d.SampleDesc.Count != 1 || !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return false;
    // The flat HDR route's crumbs (flat_hdr_crumbs.h) name the one resource the runtime itself makes for the treatment,
    // the first time: the shader view over the scene depth, with its format, size and result.
    HRESULT hr;
    {
        HdrCrumbSpan span(hdrCrumbLive(), "create-depth-srv", "fmt=%s(%u) size=%ux%u", hdrCrumbFormat(static_cast<uint32_t>(v.Format)),
                          static_cast<unsigned>(v.Format), d.Width, d.Height);
        hr = s.device->CreateShaderResourceView(depth, &v, &s.depthView);
        span.result("hr=0x%08X", static_cast<unsigned>(hr));
    }
    if (FAILED(hr)) return false;
    s.sceneDepth = depth; return true;
}

// --- GPU spans for the census (flat_cpu.h) ----------------------------------------------------
// Timestamp pairs on the shared disjoint clock (gpu_timing.h), read back without ever waiting:
// the whole frame, first game draw to Present, and the resolver's dispatches plus backend call.
// A GpuTimer stays owned until its sample is read, a few frames on; with none free the frame
// is skipped and counted. Nested spans borrow the frame's open disjoint scope, so a resolver
// span costs one timestamp pair.
void gpuFrameOpen(State& s, ID3D11DeviceContext* ctx) {
    s.gpuFrameTried = true;
    for (int i = 0; i < State::kGpuFrameTimers; ++i) {
        if (s.gpuFrameBusy[i]) continue;
        if (s.gpuFrameTimer[i].begin(s.device.Get(), ctx)) { s.gpuFrameBusy[i] = true; s.gpuFrameOpen = i; return; }
        break;
    }
    s.census.noteGpuSkipped();
}
void gpuFrameClose(State& s) {
    if (s.gpuFrameOpen < 0) return;
    s.gpuFrameTimer[s.gpuFrameOpen].end(s.context.Get());
    s.gpuFrameOpen = -1;
}
void gpuPoll(State& s) {
    ID3D11DeviceContext* ctx = s.context.Get();
    double ms = 0;
    for (int i = 0; i < State::kGpuFrameTimers; ++i) {
        if (!s.gpuFrameBusy[i] || i == s.gpuFrameOpen) continue;
        switch (s.gpuFrameTimer[i].poll(ctx, ms)) {
        case GpuTimerPoll::Ready: s.census.noteGpuFrame(ms); s.gpuFrameBusy[i] = false; break;
        case GpuTimerPoll::Invalid: s.census.noteGpuInvalid(); s.gpuFrameBusy[i] = false; break;
        case GpuTimerPoll::Pending: break;
        }
    }
    for (int i = 0; i < State::kGpuResolveTimers; ++i) {
        if (!s.gpuResolveBusy[i] || i == s.gpuResolveOpen) continue;
        switch (s.gpuResolveTimer[i].poll(ctx, ms)) {
        case GpuTimerPoll::Ready: s.census.noteGpuResolve(ms); s.gpuResolveBusy[i] = false; break;
        case GpuTimerPoll::Invalid: s.census.noteGpuInvalid(); s.gpuResolveBusy[i] = false; break;
        case GpuTimerPoll::Pending: break;
        }
    }
}
// A reset drops the spans: only on the verified owner thread, where a span still open can be
// cancelled cleanly (a timer with an open span released from anywhere else would switch the
// shared GPU clock off for the session).
void gpuReset(State& s) {
    ID3D11DeviceContext* ctx = s.context.Get();
    if (!ctx || !gpuTimingOwns(ctx)) return;
    for (auto& t : s.gpuFrameTimer) t.reset(ctx);
    for (auto& t : s.gpuResolveTimer) t.reset(ctx);
    for (auto& b : s.gpuFrameBusy) b = false;
    for (auto& b : s.gpuResolveBusy) b = false;
    s.gpuFrameOpen = s.gpuResolveOpen = -1;
}
// The resolver's hooks (flat_mono_resolve.h): a timestamp pair around its own dispatches and
// backend call. Called on the thread that resolves, inside the treatment scope.
void resolveSpanBegin(ID3D11DeviceContext* ctx) noexcept {
    auto& s = state();
    if (s.gpuResolveOpen >= 0) return;
    for (int i = 0; i < State::kGpuResolveTimers; ++i) {
        if (s.gpuResolveBusy[i]) continue;
        if (s.gpuResolveTimer[i].begin(s.device.Get(), ctx)) { s.gpuResolveBusy[i] = true; s.gpuResolveOpen = i; }
        return;
    }
}
void resolveSpanEnd(ID3D11DeviceContext* ctx) noexcept {
    auto& s = state();
    if (s.gpuResolveOpen < 0) return;
    s.gpuResolveTimer[s.gpuResolveOpen].end(ctx);
    s.gpuResolveOpen = -1;
}

// The refusal state for the F8 panel's settings warning, published for any thread:
// bit 0 the warning is wanted -- the work is stood down for a chain-shape reason that
// found an output copy, never for no-known-output-copy and never before the stand-down
// (flat_standdown.h, warningActive) --, bit 1 the work is stood down (always with bit 0
// now), bits 8..15 the stand-down's current reason.
std::atomic<uint32_t> g_refusalPublished{0};
// The scene's and the output's sizes as the copy stage last saw them (the admission's scene facts, section 83), in one
// word, 16 bits each (flatHdrPackSizes), refreshed at every final copy that has a scene; 0 when no frame has shown one
// since a resize (the swap chain or device went) last cleared it. Written on the draw thread, read by the panel thread
// (flatRuntimeSceneSizes): the F8 words name the render size when it does not fit the output, and EDVR's TAA above it.
std::atomic<uint64_t> g_sceneSizes{0};
void publishRefusal(const State& s, bool warn) {
    uint32_t v = 0;
    // Bit 2 (value 4): the route's key is auto, so the copy is admitted by structure and Bloom and Depth of field never
    // cause a refusal: the F8 words drop that advice (flatRuntimeStructureAdmission). Bit 3 (value 8): EDVR's TAA with the
    // scene rendering above the output, where neither route resolves a chain the whitelist does not know (the HDR route
    // evaluates at R, the display-grid TAA at D): the F8 words say so (flatRuntimeTaaAboveOutput).
    if (warn && s.standDown.warningActive()) {
        uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
        const bool known = flatHdrUnpackSizes(g_sceneSizes.load(std::memory_order_acquire), &rw, &rh, &ow, &oh);
        const bool taaAbove = known && s.engine == FlatMonoResolveMode::Taa && rw > ow && rh > oh;
        v = 1u | (s.standDown.standing ? 2u : 0u) | (s.hdrKey == FlatHdrKey::Auto ? 4u : 0u) | (taaAbove ? 8u : 0u) |
            (static_cast<uint32_t>(s.standDown.reason()) << 8);
    }
    g_refusalPublished.store(v, std::memory_order_release);
}

// --- Stand-down: what each mode does (flat_standdown.h) --------------------------
// The one place the pieces are paused and resumed, called at every Present with the
// mode of the frame that starts now (idempotent). Full is the runtime as it was
// before the stand-down existed. Probe and Paused pull back everything named in
// kFlatStandDownPausedWork; the gate of each piece, for the rig's source scan:
//   coverage classification, legacy projection readiness, jitter preparation and
//     constant-buffer shadow tracking: s.projection is released here and none of
//     them runs without it (the draw scope, the Map/Unmap/Update tees, the dispatch
//     scope and qualifyProjection all test it); the Present creates it in Full only
//   the per-draw scope, and the Map/Unmap/Update/Written/Uavs/Dispatch tees: s.work
//   the camera-write witness: s.work == FlatWork::Full at its call sites
//   engine motion (hooks, tees, substitution): s.enginePaused, which the Present
//     hands to engineVelocityConfigure
//   the camera refresh hook: flatCameraInjectPause
//   the discovery observers: flatTemporalSetPaused, on Paused frames only
void applyWork(State& s, FlatWork next) {
    const FlatWork was = s.work;
    s.work = next;
    if (was != next) flatTemporalSetPaused(next == FlatWork::Paused);
    if (next != FlatWork::Full || was != FlatWork::Full) flatCameraInjectPause(next != FlatWork::Full);
    if ((was == FlatWork::Paused) != (next == FlatWork::Paused)) {
        // The UAV tracker stops with the pause and starts again from unknown, the
        // state ExecuteCommandList leaves it in, so a stale binding never reads as live.
        for (auto& u : s.uavs) u.Reset();
    }
    if (was == FlatWork::Full && next != FlatWork::Full) {
        // Legacy projection readiness stops tracking constant-buffer writes and
        // drops every shadow; it is rebuilt from nothing on resume, as after a resize.
        if (s.projection) s.projection.reset();
        s.projectionContext.Reset();
        s.projectionFrames = 0;
        // Local refusal's observation ends with the contract, as in a resize.
        s.observing = false; s.covFrameLocallyRefused = false;
        s.enginePaused = true;
    } else if (was != FlatWork::Full && next == FlatWork::Full) {
        s.enginePaused = false;
    }
}

// The stand-down ends because something outside the frame changed (the AA mode, a
// device or swap-chain reset, an F10 audit): back to Full at once, with one line if
// a stand-down was in force.
void endStandDown(State& s, uint64_t frame, const char* why) {
    if (s.standDown.wake(GetTickCount64())) {
        char text[512];
        flatStandDownFormatResumed(text, sizeof(text), frame, s.standDown, why);
        Log::get().note("%s", text);
    }
    // Nothing the frame in flight showed says anything about the new question.
    s.frameSeen = FlatFrameSeen::None;
    s.frameReason = FlatMonoReason::NoOutputCopy;
    s.frameLive = false;
    applyWork(s, FlatWork::Full);
    publishRefusal(s, false);
}

// The stand-down's frame boundary: what the frame that just ended showed about its
// chain, the state machine's verdict, and the mode of the frame that starts now.
// Runs once per Present, before anything else in it reads s.work.
void standDownFrame(State& s, uint64_t frame) {
    const uint64_t now = GetTickCount64();
    const bool watched = s.frameLive;
    const FlatFrameSeen seen = s.frameSeen;
    const FlatMonoReason reason = s.frameReason;
    s.frameLive = false;
    s.frameSeen = FlatFrameSeen::None;
    s.frameReason = FlatMonoReason::NoOutputCopy;
    const FlatStandDownEvent event = s.standDown.frameEnded(seen, reason, watched, now);
    char text[1200], sizes[160];
    // The render size's own words (section 83) ride the lines that name render-size-does-not-fit-output: the sizes the copy stage
    // last measured, "Elite renders 2176x1224 on a 2560x1600 screen". Null for a session with none, and the lines are then what
    // they always were.
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    const char* renderSize = nullptr;
    if (flatHdrUnpackSizes(g_sceneSizes.load(std::memory_order_acquire), &rw, &rh, &ow, &oh)) {
        flatRenderSizeWords(sizes, sizeof(sizes), rw, rh, ow, oh);
        renderSize = sizes;
    }
    if (event == FlatStandDownEvent::Entered) {
        flatStandDownFormatEntered(text, sizeof(text), frame, s.standDown, renderSize);
        Log::get().note("%s", text);
    } else if (event == FlatStandDownEvent::Resumed) {
        flatStandDownFormatResumed(text, sizeof(text), frame, s.standDown, nullptr);
        Log::get().note("%s", text);
    } else if (s.standDown.reportDue(now)) {
        flatStandDownFormatStill(text, sizeof(text), frame, s.standDown, now, renderSize);
        Log::get().note("%s", text);
    }
    applyWork(s, s.standDown.nextFrame(now));
    publishRefusal(s, true);
}
}

bool flatRuntimeStructuralRefusal(const char** reasonName, bool* standingDown) {
    const uint32_t v = g_refusalPublished.load(std::memory_order_acquire);
    if (!(v & 1u)) return false;
    if (reasonName) *reasonName = flatMonoReasonName(static_cast<FlatMonoReason>((v >> 8) & 0xFFu));
    if (standingDown) *standingDown = (v & 2u) != 0;
    return true;
}
bool flatRuntimeStructureAdmission() {
    return (g_refusalPublished.load(std::memory_order_acquire) & 5u) == 5u;
}
bool flatRuntimeTaaAboveOutput() {
    return (g_refusalPublished.load(std::memory_order_acquire) & 9u) == 9u;
}
bool flatRuntimeSceneSizes(uint32_t* renderWidth, uint32_t* renderHeight, uint32_t* outputWidth, uint32_t* outputHeight) {
    return flatHdrUnpackSizes(g_sceneSizes.load(std::memory_order_acquire), renderWidth, renderHeight, outputWidth,
                              outputHeight);
}

void flatRuntimeResize() {
    // The game retries a refused ResizeBuffers every frame: thousands of calls in seconds with no Present between
    // them (2026-10-09 Epic logs). The reports below say it once per 5 s with the count of the calls they skipped.
    static ULONGLONG lastResizeReportMs = 0;
    static uint32_t resizeReportsSuppressed = 0;
    const ULONGLONG resizeNowMs = GetTickCount64();
    const bool reportResize = lastResizeReportMs == 0 || resizeNowMs - lastResizeReportMs >= 5000;
    if (reportResize) {
        if (resizeReportsSuppressed)
            Log::get().note("flat resize-or-stop reports: %u further resize-or-stop calls since the last report were not reported (one report per 5 s)",
                resizeReportsSuppressed);
        resizeReportsSuppressed = 0;
        lastResizeReportMs = resizeNowMs;
        reportDrawIngress("resize-or-stop");
    } else ++resizeReportsSuppressed;
    g_flatRuntimeLive.store(false, std::memory_order_release);
    nativeScale.store(false, std::memory_order_release);
    // The swap chain or the device went: engine motion's bound state (the game's render targets among it, held by
    // reference) is forgotten without touching a context that may be gone.
    flatRuntimeSubstitution(nullptr, FlatSubstEvent::kResize);
    auto& s = state(); FlatComputeInternalScope guard; s.drawCapture.cancel("resize-or-stop");
    overlaySuffixActive.store(false,std::memory_order_release);
    s.overlay.reset();
    s.foreground.reset();
    s.foregroundRoute.clear();for(auto& candidate:s.foregroundCandidates)retireDomainCandidate(s,candidate);
    s.foregroundDomainFrame=~0ull;s.foregroundSelectedDepth=nullptr;s.foregroundLastH={};
    s.foregroundProofs.clear();s.foregroundFirstFailure={};s.foregroundHRefusal=nullptr;
    s.untrusted.reset();
    s.untrustedUnknown=false;
    s.untrustedSupportedAlternate=false;
    s.untrustedQualification={};s.untrustedQualificationCalled=false;
    s.untrustedQualificationFailed=false;
    s.unclassifiedPoolUsed=0;s.unclassifiedPoolOverflow=false;
    foregroundProbeActive.store(false,std::memory_order_release);
    foregroundDomainActive.store(false,std::memory_order_release);
    untrustedCoverageActive.store(false,std::memory_order_release);
    s.overlayFailureNoted=false;
    s.weaponFootprint.cancel("resize-or-stop");flatMonoResolveReset();
    // fix.ui_quality's flat layer: its layers, depth targets, composite output and the views over the pictures it
    // composited (none of them the back buffer, all of them released), and the door, so a fresh one arms the layer.
    flatUiLayerRelease();
    s.drawPackets.cancel();
    // A reset (or the mode turned off) ends a stand-down: the new contract may well be
    // one the selector recognises, and every paused piece restarts with it.
    endStandDown(s, s.prefix.frame, "the swap chain or device was reset, or the mode was turned off");
    gpuReset(s);
    flatCameraInjectReset(); // history and the decision do not survive a resize; injected cameras stay known for the flush
    finishPhaseCensusFrame(s);
    if(reportResize && (s.phaseCensusFrames || s.phaseFailuresUsed || s.phaseOverflowCalls))
        reportPhaseCensus(s,"resize-or-stop");
    if (s.projection) { if (reportResize) reportProjection(s,"resize-or-stop"); s.projection.reset(); s.projectionContext.Reset(); s.projectionFrames=0; }
    s.haveResolvePlan=false; s.resolvePreflight={}; s.resolvePreflightRetryMs=0;
    s.phase.resetHistory();s.phaseDepth.Reset();s.phaseHdr.Reset();s.temporalAccepted=false;
    s.phaseWidth=s.phaseHeight=0;s.frameCoverage=true;
    s.havePrevious = false; s.previousColor.Reset(); s.output.Reset(); s.sceneDepth.Reset(); s.depthView.Reset();
    // The draw-packet capture's references: the back buffer (set every frame at the frame boundary) and the context.
    // A counted reference to the back buffer makes the game's ResizeBuffers fail with DXGI_ERROR_INVALID_CALL,
    // and the game retries it every frame (the 2026-10-09 resize loop).
    s.drawPacketOutput.Reset(); s.drawPacketContext.Reset();
    for (auto& v : s.views) v = View{};
    for (auto& r : s.colors) r.Reset(); for (auto& r : s.depths) r.Reset();
    for (auto& r : s.uavs) r.Reset();
    s.cameras.clear();
    // Local refusal's observation ends with the contract: a resize or device
    // change requalifies nothing, but the state itself must not survive.
    s.observing = false; s.covFrameLocallyRefused = false;
    // The HDR route's per-frame detector state names resources of the old device; the key, the latch and the
    // census window are the session's and stay.
    flatHdrBeginFrame(s.hdr, 0, 0); s.hdrTreated = false;
    g_sceneSizes.store(0, std::memory_order_release);
    s.prefix = FlatRuntimePrefix{}; s.context.Reset(); s.device.Reset(); s.thread = 0; s.viewportCount = 0;
}
void flatRuntimeBeforePresent() {
    g_flatRuntimeLive.store(false, std::memory_order_release);
    // Open mappings survive a stand-down or resize. Watch them at every frame
    // boundary, even when the projection runtime no longer exists.
    mapBounce().present(mapBouncePresentEpoch.fetch_add(1,std::memory_order_acq_rel)+1);
    // The flat HDR route's crash-safe breadcrumbs (flat_hdr_crumbs.h): engine motion's state going back and the census span
    // closing are the last work before the real Present, for a frame the resolver has had.
    HdrCrumbSpan routeBeforePresent(hdrCrumbPresentSide(), "before-present");
    // The frame is ending: the game's state goes back where engine motion's is still bound, before the real Present
    // and every EDVR pass that follows it (the lazy form, engine_velocity.h).
    if (owner()) flatRuntimeSubstitution(state().context.Get(), FlatSubstEvent::kPresent);
    // And what engine motion's bracket kept for the frame goes with it.
    if (owner()) engineVelocityFlatFrameEnd();
    // The census's whole-frame GPU span ends here, just before the real Present.
    if (owner()) gpuFrameClose(state());
}
bool flatRuntimeNativeScale() { return nativeScale.load(std::memory_order_acquire); }
void flatRuntimePhaseState(float* x, float* y, uint32_t* w, uint32_t* h, uint32_t* applied) {
    auto& s = state();
    if (x) *x = s.phase.currentX;
    if (y) *y = s.phase.currentY;
    if (w) *w = s.phaseWidth;
    if (h) *h = s.phaseHeight;
    if (applied) *applied = s.phase.applied;
}
void flatRuntimeNoteCameraApplied() { auto& s = state(); s.phase.noteApplied(); ++s.jitterDraws; }
bool flatRuntimeLegacyPlanExists() { return state().projection != nullptr; }
void flatRuntimeArmProjectionAudit(bool full) { if(runtimeFlatProfile()) projectionAuditRequested.request(full); }
void flatRuntimeCreateBuffer(ID3D11Buffer* buffer, const void* initialData) {
    if(owner() && state().projection) {
        flatcpu::Scope shadows(flatcpu::kShadows);
        state().projection->observeCreateBuffer(buffer,initialData);
    }
}
// The trace window for the frame that starts now: full while a capture reads
// the ring (draw packets, a pending F10 dump), the narrow idle window otherwise.
void traceWindow(State& s) {
    s.traceRing.eventLimit=s.traceDumpFrame || s.drawPackets.active()?
        kFlatTraceEventsPerFrame:kFlatTraceIdleEventsPerFrame;
}
// Gate 1 trace dump: write the ring's complete frames to logDir\traces on the
// F10 audit arm. CREATE_ALWAYS: each arm is a new capture of the newest slots.
void flatTraceDumpToLogDir(State& s, uint64_t frame) {
    const auto root = Config::get().logDir() + L"\\traces";
    if (!CreateDirectoryW(root.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        Log::get().note("flat trace dump: cannot create %ls (error %lu)", root.c_str(), GetLastError());
        return;
    }
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%ls\\flat_trace_%llu.bin", root.c_str(), (unsigned long long)frame);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log::get().note("flat trace dump: cannot write %ls (error %lu)", path, GetLastError());
        return;
    }
    // Emitted versus withheld counts are reported separately: the serializer
    // skips the in-flight, empty and truncated slots, and a busy frame that
    // dropped out must not read as captured evidence.
    uint32_t emittedFrames = 0, emittedEvents = 0, skipped = 0,inFlight=0,empty=0;
    for (uint32_t i = 0; i < kFlatTraceFrames; ++i) {
        if (!s.traceRing.slotUsed[i]) continue;
        if(i==s.traceRing.slot)++inFlight;
        if(!s.traceRing.headers[i].eventCount)++empty;
        if (i == s.traceRing.slot || s.traceRing.headers[i].truncated || !s.traceRing.headers[i].eventCount)
            ++skipped;
        else { ++emittedFrames; emittedEvents += s.traceRing.headers[i].eventCount; }
    }
    uint32_t expected = sizeof(FlatTraceHeader);
    for (uint32_t i = 0; i < kFlatTraceFrames; ++i)
        if (s.traceRing.slotUsed[i] && i != s.traceRing.slot && !s.traceRing.headers[i].truncated &&
            s.traceRing.headers[i].eventCount)
            expected += sizeof(FlatTraceFrameHeader) + s.traceRing.headers[i].eventCount * sizeof(FlatTraceEvent);
    bool shortWrite = false;
    const uint32_t bytes = flatTraceDump(s.traceRing, [&](const void* data, uint32_t bytes) {
        DWORD wrote = 0; WriteFile(f, data, bytes, &wrote, nullptr);
        if (wrote != bytes) shortWrite = true;
        return wrote;
    });
    CloseHandle(f);
    const auto stats=flatTraceStats(s.traceRing);
    Log::get().note("flat trace dump: %ls frames=%u events=%u bytes=%u skipped-slots=%u capacity=%u peak-attempted=%u overflow-slots=%u in-flight-slots=%u empty-slots=%u%s",
        path, emittedFrames, emittedEvents, bytes, skipped,stats.capacity,stats.peakAttempted,stats.overflowSlots,inFlight,empty,
        shortWrite || bytes != expected ? " SHORT WRITE -- capture unusable" : "");
}
// --- The HDR route's runtime half (flat_hdr_route.h holds the pure logic and the log lines) -----------------
// The route is always on (design section 81), set at the first Present. It is also the final copy's admission by
// structure (section 83): it admits a copy the whitelist refused for its tone pass wherever the route does not serve the
// frame (render below the output, EDVR's TAA above it).
static void hdrReadKey(State& s, uint64_t frame) {
    if (s.hdrKeyRead) return;
    const FlatHdrKey key = FlatHdrKey::Auto;
    s.hdrKey = key; s.hdrKeyRead = true;
    Log::get().note("flat late overlay blend diagnostic: event=armed ready=1 sample-limit=1-per-5s-window; actual bindings and GetDesc only on dual-source-blend refusal; acceptance unchanged");
    // The crash-safe trail's first line (flat_hdr_crumbs.h): proof, in edvr_breadcrumbs.txt itself, that this build has the
    // crumbs and that the route is on, so a trail without an "admitted" after it is a session that ended before the route
    // took a frame, and a file without it came from a build that has none or from a device that is not DXMT's (the gate: this
    // writes nothing, and the log says nothing, off DXMT). The log says so too, for whoever reads it first.
    if (hdrCrumbArmed(flatHdrKeyName(key)))
        Log::get().note("flat hdr route: crash-safe trail on: edvr_breadcrumbs.txt gets a line before and after every step of the first "
                        "%u frames that reach the resolver, at most %u lines a session; if the process ends inside the treatment, the last "
                        "'gfx: hdr-treat' line there names the step",
            static_cast<unsigned>(kHdrCrumbFrames), static_cast<unsigned>(kHdrCrumbCap));
    Log::get().note("flat hdr route: %s (read at startup) at frame=%llu: %s",
        flatHdrKeyName(key), static_cast<unsigned long long>(frame),
            "the route resolves the game's HDR scene target before its bloom, depth of field and tone where the "
              "render size is at least the output's and the target is R11G11B10F; every other frame keeps the copy route, "
              "which admits the game's final copy by its structure (an R-sized R8G8B8A8 image made after the scene HDR's "
              "first consumer, uniformly scaled to the output) when no whitelisted tone pass wrote it, so bloom, depth of "
              "field and the tone variant do not matter below the output either");
}
// The frame that just ended, as the route saw it: the census token (every watched frame, the key off included), the
// late-write accounting with its latch, and, only with the key auto, the chain verdict of a frame that had an HDR
// target and no consumer. Runs once per Present, before the stand-down reads s.frameSeen.
static void hdrFrameEnd(State& s, uint64_t frame) {
    if (!s.frameLive) return;   // a Paused frame watched nothing
    const FlatHdrFrameVerdict verdict = flatHdrFrameVerdict(s.hdr);
    s.hdrWindow.noteFrame(s.hdr);
    char text[700];
    if (verdict == FlatHdrFrameVerdict::Trigger) {
        if (flatHdrLateWrites(s.hdr)) {
            if (!s.hdrLateLogged) {
                s.hdrLateLogged = true;
                flatHdrFormatLateWrite(text, sizeof(text), frame, s.hdrKey, s.hdr);
                Log::get().note("%s", text);
            }
            if (s.hdrTreated && s.hdrLatch.treatedFrame(true)) {
                flatHdrFormatLatched(text, sizeof(text), frame, s.hdr, s.hdrLatch.frames);
                Log::get().note("%s", text);
            }
        } else if (s.hdrTreated) {
            s.hdrLatch.treatedFrame(false);
        }
    } else if (verdict == FlatHdrFrameVerdict::NoTrigger) {
        s.hdrWindow.lastVerdict = flatMonoReasonName(FlatMonoReason::NoHdrConsumer);
        // A frame that reached the game's output copy, had an HDR target and gave the route no consumer is refused for
        // the shape of its chain. A frame that never reached a copy stays "no final copy" (a loading screen), and a frame
        // the copy stage found no scene in (no-3d-scene: startup, a loading screen, an HDR target with a handful of draws)
        // stays that, which never warns (section 83).
        if (s.hdrKey == FlatHdrKey::Auto && !s.hdrLatch.tripped && s.frameSeen != FlatFrameSeen::None &&
            s.frameSeen <= FlatFrameSeen::Structural && s.frameReason != FlatMonoReason::NoScene) {
            s.frameSeen = FlatFrameSeen::Structural; s.frameReason = FlatMonoReason::NoHdrConsumer;
        }
    }
}
// Called synchronously by the actual HDR selector before its 34 local records
// expire. Every eligible source is reported, including ones after the first
// refusal, so one session distinguishes a second depth from camera/layout.
static void hdrAmbiguousSourceReport(const FlatMonoFrameInput& in, const void* hdrResource,
                                     uint32_t consumerSeq, void* user) {
    auto& s=*static_cast<State*>(user);
    const FlatContractRecord* hdr=nullptr,*hdrCamera=nullptr;
    uint32_t hdrLast=0;
    const uint32_t count=in.worldCount+in.handoffCount;
    for(uint32_t i=0;i<count;++i) {
        const auto& r=flat_mono_detail::record(in,i);
        if(r.key.color!=hdrResource)continue;
        hdr=&r;
        if(r.last>hdrLast)hdrLast=r.last;
        if(r.key.camera && (!hdrCamera || r.first<hdrCamera->first))hdrCamera=&r;
    }
    if(!hdr || !hdrCamera) {
        Log::get().note("flat HDR source witness: frame=%llu result=missing-H-reference records=%u",
            (unsigned long long)s.prefix.frame,count);
        if(!s.sourceWitnessCaptured)s.sourceWitnessFirstFrame=s.prefix.frame;
        ++s.sourceWitnessCaptured;
        return;
    }
    uint32_t eligible=0,firstIndex=~0u;
    FlatHdrSourceIssue firstIssue=FlatHdrSourceIssue::None;
    for(uint32_t i=0;i<count;++i) {
        const auto f=flatHdrSourceFacts(in,i,*hdr,*hdrCamera,hdr->key.width,hdr->key.height,consumerSeq,hdrLast);
        if(!f.eligible)continue;
        ++eligible;
        if(firstIssue==FlatHdrSourceIssue::None && f.sameDepth &&
           (f.issue==FlatHdrSourceIssue::SameDepthLayout || f.issue==FlatHdrSourceIssue::SameDepthCamera)) {
            firstIndex=i;firstIssue=f.issue;
        }
    }
    if(firstIssue==FlatHdrSourceIssue::None)for(uint32_t i=0;i<count;++i) {
        const auto f=flatHdrSourceFacts(in,i,*hdr,*hdrCamera,hdr->key.width,hdr->key.height,consumerSeq,hdrLast);
        if(f.issue==FlatHdrSourceIssue::SecondDepthSameCamera) {
            firstIndex=i;firstIssue=f.issue;break;
        }
    }
    char worldRows[24*9+1]{};
    hexWords(hdrCamera->camera,24,worldRows,sizeof(worldRows));
    Log::get().note("flat HDR source witness: frame=%llu seq=%u result=source-camera-or-depth-not-unique records=%u eligible-pool=%u first-index=%u first-branch=%s H=%p H-extent=%ux%u H-depth=%p H-dsv=%p H-depth-fmt=%u H-b1=%p H-camera=%016llX H-key-write=%llu/%u H-first=%u H-last=%u H-camera-first=%u H-camera-last=%u named-depth=%p named-b1=%p named-camera=%016llX named-same-H=%u H-rows270-275=%s",
        (unsigned long long)s.prefix.frame,consumerSeq,count,eligible,firstIndex,
        flatHdrSourceIssueName(firstIssue),hdrResource,hdr->key.width,hdr->key.height,
        hdr->key.depth,hdr->key.dsv,hdr->key.depthFormat,
        hdrCamera->key.b1,(unsigned long long)hdrCamera->key.cameraHash,
        (unsigned long long)hdr->key.writeEpoch,hdr->key.writeSeq,
        hdr->first,hdrLast,hdrCamera->first,hdrCamera->last,
        s.namedDepth,s.namedConstants,(unsigned long long)flatCameraHash(s.namedCamera),
        s.namedDepth==hdr->key.depth && s.namedConstants==hdrCamera->key.b1 &&
            std::memcmp(s.namedCamera,hdrCamera->camera,sizeof(s.namedCamera))==0?1u:0u,worldRows);
    for(uint32_t i=0;i<count;++i) {
        const auto f=flatHdrSourceFacts(in,i,*hdr,*hdrCamera,hdr->key.width,hdr->key.height,consumerSeq,hdrLast);
        if(!f.eligible)continue;
        const auto& r=flat_mono_detail::record(in,i);const auto& k=r.key;
        char rows[24*9+1]{};hexWords(r.camera,24,rows,sizeof(rows));
        Log::get().note("flat HDR source witness record: frame=%llu index=%u branch=%s same-depth=%u layout=%u current=%u viewport=%u same-camera=%u order=%u VS=%016llX PS=%016llX color=%p rtv=%p fmt=%u depth=%p dsv=%p depth-fmt=%u extent=%ux%u depth-extent=%ux%u b1=%p camera-present=%u camera-hash=%016llX key-write=%llu/%u draws=%u first=%u last=%u first-write=%llu/%u last-write=%llu/%u rows270-275=%s",
            (unsigned long long)s.prefix.frame,i,flatHdrSourceIssueName(f.issue),
            f.sameDepth?1u:0u,f.layoutValid?1u:0u,f.current?1u:0u,
            f.fullViewport?1u:0u,f.sameCamera?1u:0u,f.orderValid?1u:0u,
            (unsigned long long)k.vs,(unsigned long long)k.ps,k.color,k.rtv,k.format,
            k.depth,k.dsv,k.depthFormat,k.width,k.height,k.depthWidth,k.depthHeight,
            k.b1,k.camera?1u:0u,(unsigned long long)k.cameraHash,
            (unsigned long long)k.writeEpoch,k.writeSeq,r.draws,r.first,r.last,
            (unsigned long long)r.firstWriteEpoch,r.firstWriteSeq,
            (unsigned long long)r.lastWriteEpoch,r.lastWriteSeq,rows);
    }
    // Mark completion only after the report, so a dead/short-circuited path
    // cannot spend the two-frame budget without producing a witness.
    if(!s.sourceWitnessCaptured)s.sourceWitnessFirstFrame=s.prefix.frame;
    ++s.sourceWitnessCaptured;
}
// At the trigger draw, in the draw scope: the route's selection over the prefix so far, its verdict into the stand-down
// (key auto only: with it off the route decides nothing), the window token, the once-a-session trigger line and the
// trace's resolve marker. The treatment itself follows in FlatRuntimeDrawScope::treatHdr.
static bool qualifiedUntrustedSource(const FlatContractRecord& record,
                                     const unsigned char* worldCamera,void* user) {
    auto* s=static_cast<State*>(user);
    if(!s)return false;
    FlatUntrustedQualification diagnostic{};
    const bool qualified=s->untrusted.qualifies(record,worldCamera,
        s->phase.currentX,s->phase.currentY,&diagnostic);
    s->untrustedQualificationCalled=true;
    if(!s->untrustedQualificationFailed) {
        s->untrustedQualification=diagnostic;
        s->untrustedLastQualification=diagnostic.reason?diagnostic.reason:"unknown";
    }
    if(!qualified)s->untrustedQualificationFailed=true;
    return qualified;
}
// Diagnostic only. The selector may refuse before it returns a FlatMonoFrame
// identity, so use the trigger's frozen H target to explain both camera buckets.
// This never calls select(), changes a receipt, or affects route admission.
static void reportUntrustedAtH(State& s,const FlatMonoFrame& sel) {
    const FlatRuntimeTarget* target=nullptr;
    for(uint32_t i=0;i<s.prefix.targetsUsed;++i)
        if(s.prefix.targets[i].resource==s.hdr.trigger.hdr) {target=&s.prefix.targets[i];break;}
    float frozenHRows[6][4]{};
    const bool hFrozen=target && target->hdrCamera && !target->hdrBad &&
        !target->hdrLayoutChanged && target->writes.key.format==26 &&
        flat_mono_detail::cameraCurrent(target->tone,s.prefix.frame) &&
        target->writes.key.depth && target->writes.key.dsv &&
        flat_mono_detail::cameraShape(target->tone.camera,frozenHRows);
    const void* hDepth=sel.selected()?sel.depth:hFrozen?target->writes.key.depth:nullptr;
    const void* hDsv=sel.selected()?sel.dsv:hFrozen?target->writes.key.dsv:nullptr;
    const unsigned char* hCamera=sel.selected()?
        reinterpret_cast<const unsigned char*>(sel.camera):hFrozen?target->tone.camera:nullptr;
    bool supportedAlternate=false;
    if(hCamera && hDepth)for(uint32_t i=0;i<s.prefix.sourcesUsed;++i) {
        const auto& r=s.prefix.sources[i];
        if(r.key.kind==kFlatContractPool && r.key.depth==hDepth && r.key.camera &&
           engineVelocityPoolFamilyPair(r.key.vs,r.key.ps) &&
           std::memcmp(r.camera,hCamera,kFlatCameraBytes)!=0) {
            supportedAlternate=true;break;
        }
    }
    supportedAlternate=supportedAlternate || s.untrustedQualificationCalled;
    const bool refused=!sel.selected() || s.untrustedUnknown;
    if(!refused || (!s.untrusted.active() && !supportedAlternate &&
                    !s.untrustedQualificationFailed))return;
    std::string stage="selector",reason=flatMonoReasonName(sel.reason);
    for(uint32_t i=0;i<s.untrusted.bucketCount();++i) {
        const auto b=s.untrusted.diagnoseBucket(i,hDepth,hDsv,hCamera,
            s.phase.currentX,s.phase.currentY);
        if(b.alternate && !b.firstFailure.reason.empty()) {
            stage=b.firstFailure.stage;reason=b.firstFailure.reason;break;
        }
    }
    if(stage=="selector" && s.untrustedQualificationFailed) {
        stage="qualification";
        reason=s.untrustedLastQualification;
    }
    if(!s.untrustedDiagnosticBudget.take(s.prefix.frame,
        supportedAlternate && refused,stage.c_str(),reason.c_str()))return;
    uint32_t observed=0,completed=0,groups=0,missing=0;
    const FlatUntrustedObservedCamera* firstGap=nullptr;
    uint32_t firstGapCompleted=0;
    if(hDepth && hCamera)for(uint32_t i=0;i<s.unclassifiedPoolUsed;++i) {
        const auto& group=s.unclassifiedPool[i];
        if(group.depth!=hDepth ||
           (group.hasCamera && std::memcmp(group.camera,hCamera,kFlatCameraBytes)==0))continue;
        ++groups;observed+=group.draws;
        uint32_t count=0;
        const bool accounted=flatUntrustedObservationAccounted(group,s.untrusted,&count);
        completed+=count;
        if(!group.hasCamera)++missing;
        if(!accounted && !firstGap) {firstGap=&group;firstGapCompleted=count;}
    }
    Log::get().note("flat untrusted H audit: frame=%llu route=%s h-identified=%u h-depth=%p h-dsv=%p h-camera=%016llX supported-alternate=%u stage=%s reason=%s buckets=%u observed=%u completed=%u groups=%u missing-camera=%u overflow=%u global=%s",
        (unsigned long long)s.prefix.frame,flatMonoReasonName(sel.reason),
        sel.selected() || hFrozen?1u:0u,
        hDepth,hDsv,(unsigned long long)(sel.selected()?sel.cameraHash:
            hFrozen?target->tone.key.cameraHash:0),supportedAlternate?1u:0u,
        stage.c_str(),reason.c_str(),s.untrusted.bucketCount(),observed,completed,
        groups,missing,s.unclassifiedPoolOverflow?1u:0u,
        s.untrusted.globalFailure()?s.untrusted.globalFailure():"none");
    char hRows[24*9+1]{};
    if(hCamera)hexWords(hCamera,24,hRows,sizeof(hRows));
    Log::get().note("flat untrusted H camera rows: frame=%llu present=%u rows270-275=%s",
        (unsigned long long)s.prefix.frame,hCamera?1u:0u,hCamera?hRows:"unavailable");
    for(uint32_t i=0;i<s.untrusted.bucketCount();++i) {
        const auto b=s.untrusted.diagnoseBucket(i,hDepth,hDsv,hCamera,
            s.phase.currentX,s.phase.currentY);
        const auto& f=b.firstFailure;
        Log::get().note("flat untrusted H bucket: frame=%llu index=%u role=%s color=%p depth=%p dsv=%p camera=%016llX h-dsv-match=%u pending=%u completed=%u ready=%u shape=%u phase=%u first-stage=%s first-reason=%s first-frame=%llu first-q=%u first-vs=%016llX first-ps=%016llX first-color=%p first-depth=%p first-dsv=%p first-write=%llu/%u first-current=%u first-viewport=%u actual-ps-read=%u actual-ps-object=%p actual-ps-hash=%016llX actual-hash-known=%u actual-match=%u first-pending=%u first-completed=%u",
            (unsigned long long)s.prefix.frame,i,b.world?"world-exact":b.alternate?"alternate":
            b.unrelatedDepth?"unrelated-depth":"H-unknown",b.color,b.depth,b.dsv,
            (unsigned long long)b.cameraHash,b.dsvMatch?1u:0u,b.pending,b.completed,
            b.ready?1u:0u,b.cameraShape?1u:0u,b.phasePair?1u:0u,
            f.stage,f.reason.empty()?"none":f.reason.c_str(),
            (unsigned long long)f.frame,f.sequence,
            (unsigned long long)f.vs,(unsigned long long)f.ps,
            f.color,f.depth,f.dsv,
            (unsigned long long)f.writeEpoch,f.writeSeq,f.current?1u:0u,
            f.viewport?1u:0u,f.actualPsRead?1u:0u,f.actualPsObject,
            (unsigned long long)f.actualPs,
            f.actualPs?1u:0u,
            f.actualPsRead && f.actualPs && f.actualPs==f.ps?1u:0u,
            f.pending,f.completed);
        char bucketRows[24*9+1]{};hexWords(b.camera,24,bucketRows,sizeof(bucketRows));
        Log::get().note("flat untrusted H bucket rows: frame=%llu index=%u rows270-275=%s",
            (unsigned long long)s.prefix.frame,i,bucketRows);
        Log::get().note("flat untrusted H mutations: frame=%llu bucket=%u signatures=%u dropped=%u classes=%u class-dropped=%u; ordinal counts notifications including API and generic-write duplicates, after-q is the current prefix position",
            (unsigned long long)s.prefix.frame,i,s.untrusted.mutationCount(i),s.untrusted.mutationDropped(i),
            s.untrusted.mutationClassCount(i),s.untrusted.mutationClassDropped(i));
        const auto emitMutation=[&](const FlatUntrustedMutationDiagnostic& m,uint32_t j,const char* kind) {
            const auto& d=m.details;
            Log::get().note("flat untrusted H mutation%s: frame=%llu bucket=%u signature=%u first=%u ordinal=%llu after-q=%u entry=%s op=%s role=%s resource=%p view=%p count=%u pending=%u completed=%u ready=%u planned=%u open=%u consumer=%u nominee-q=%u nominee-VS=%016llX nominee-PS=%016llX nominee-camera=%016llX nominee-write=%llu/%u nominee-current=%u nominee-viewport=%u",
                kind,(unsigned long long)m.frame,i,j,j==0?1u:0u,(unsigned long long)m.ordinal,m.afterSequence,
                d.entry,flatMutationOpName(d.op),m.color?(m.depth?"color+depth":"color"):m.depth?"depth":"unknown",
                m.resource,d.view,m.count,m.pending,m.completed,m.ready?1u:0u,m.planned?1u:0u,
                m.open?1u:0u,m.consumer?1u:0u,m.nominee.sequence,
                (unsigned long long)m.nominee.vs,(unsigned long long)m.nominee.ps,
                (unsigned long long)m.nominee.cameraHash,(unsigned long long)m.nominee.writeEpoch,m.nominee.writeSeq,
                m.nominee.current?1u:0u,m.nominee.viewport?1u:0u);
            Log::get().note("flat untrusted H mutation%s payload: frame=%llu bucket=%u signature=%u known=0x%X flags=0x%X clear-depth=%.9g stencil=%u map-type=%u source=%p source-view=%p src-sub=%u dst-sub=%u dst-xyz=(%u,%u,%u) box-present=%u box=(%u,%u,%u,%u,%u,%u) row-pitch=%u depth-pitch=%u format=%u values-present=%u values=%08X,%08X,%08X,%08X",
                kind,(unsigned long long)m.frame,i,j,d.known,d.flags,d.depth,d.stencil,d.mapType,d.source,d.sourceView,
                d.srcSub,d.dstSub,d.dstX,d.dstY,d.dstZ,d.hasBox?1u:0u,
                d.box.left,d.box.top,d.box.front,d.box.right,d.box.bottom,d.box.back,
                d.rowPitch,d.depthPitch,d.format,d.hasValues?1u:0u,d.values[0],d.values[1],d.values[2],d.values[3]);
        };
        for(uint32_t j=0;j<s.untrusted.mutationCount(i);++j)
            emitMutation(*s.untrusted.mutationDiagnostic(i,j),j,"");
        for(uint32_t j=0;j<s.untrusted.mutationClassCount(i);++j) {
            const auto& m=*s.untrusted.mutationClassDiagnostic(i,j);
            int detail=-1;
            for(uint32_t k=0;k<s.untrusted.mutationCount(i);++k)
                if(s.untrusted.mutationDiagnostic(i,k)->ordinal==m.ordinal) {detail=static_cast<int>(k);break;}
            Log::get().note("flat untrusted H mutation class: frame=%llu bucket=%u class=%u op=%s role=%s count=%u first-ordinal=%llu first-after-q=%u detail-signature=%d",
                (unsigned long long)m.frame,i,j,flatMutationOpName(m.details.op),
                m.color?(m.depth?"color+depth":"color"):m.depth?"depth":"unknown",m.count,
                (unsigned long long)m.ordinal,m.afterSequence,detail);
            if(detail<0)emitMutation(m,j," class-first");
        }
        if(b.alternate) {
            Log::get().note("flat untrusted H nominee shaders: frame=%llu bucket=%u pairs=%u dropped=%u; bytecode-only MRT7 preflight, no device creation or draw-state admission",
                (unsigned long long)s.prefix.frame,i,s.untrusted.nomineeCount(i),s.untrusted.nomineeDropped(i));
            for(uint32_t j=0;j<s.untrusted.nomineeCount(i);++j) {
                const auto& n=*s.untrusted.nomineeDiagnostic(i,j);
                const uint8_t* psBytes=nullptr;size_t byteCount=0;
                const bool found=n.ps && flatProbeShaderLookup('p',n.ps,&psBytes,&byteCount);
                std::vector<BYTE> patched;std::string why;
                const bool patch=found && flatOverlayPatchPs(psBytes,byteCount,patched,why);
                const bool vsSaved=n.vs && captureFlatProbeShader('v',n.vs);
                const bool psSaved=n.ps && captureFlatProbeShader('p',n.ps);
                Log::get().note("flat untrusted H nominee shader: frame=%llu bucket=%u pair=%u first-q=%u count=%u first-after-failure=%u VS=%016llX saved=%u PS=%016llX saved=%u creation-bytes=%zu patch=%u reason=%s",
                    (unsigned long long)s.prefix.frame,i,j,n.firstSequence,n.count,n.firstAfterFailure?1u:0u,
                    (unsigned long long)n.vs,vsSaved?1u:0u,(unsigned long long)n.ps,psSaved?1u:0u,
                    found?byteCount:0,patch?1u:0u,!n.ps?"absent-hash":!found?"creation-bytes-missing":patch?"bytecode-patchable":why.c_str());
                Log::get().note("flat untrusted H nominee registry: frame=%llu bucket=%u pair=%u read=%u binding-shadow-PS=%p eligible=%u previously-created=%u reason=%s; captured after original-state restore, no new device work",
                    (unsigned long long)s.prefix.frame,i,j,n.shader.read?1u:0u,n.shader.object,
                    n.shader.eligible?1u:0u,n.shader.created?1u:0u,
                    n.shader.read?n.shader.reason.c_str():"not-read-unqualified-or-cap");
            }
        }
        if(!f.reason.empty()) {
            const bool vsSaved=f.vs && captureFlatProbeShader('v',f.vs);
            const bool psSaved=f.ps && captureFlatProbeShader('p',f.ps);
            const bool actualSaved=f.actualPsRead && f.actualPs &&
                captureFlatProbeShader('p',f.actualPs);
            Log::get().note("flat untrusted H shader bytes: frame=%llu bucket=%u VS=%016llX saved=%u missing=%u absent=%u PS=%016llX saved=%u missing=%u absent=%u actualPS=%016llX read=%u present=%u hash-known=%u saved=%u missing=%u absent=%u",
                (unsigned long long)s.prefix.frame,i,(unsigned long long)f.vs,vsSaved?1u:0u,
                f.vs && !vsSaved?1u:0u,f.vs?0u:1u,
                (unsigned long long)f.ps,psSaved?1u:0u,f.ps && !psSaved?1u:0u,f.ps?0u:1u,
                (unsigned long long)f.actualPs,f.actualPsRead?1u:0u,
                f.actualPsObject?1u:0u,f.actualPs?1u:0u,actualSaved?1u:0u,
                f.actualPsRead && f.actualPs && !actualSaved?1u:0u,
                f.actualPsRead && !f.actualPsObject?1u:0u);
        }
    }
    const auto& q=s.untrustedQualification;
    Log::get().note("flat untrusted H qualification: frame=%llu called=%u reason=%s VS=%016llX PS=%016llX H=%u unique=%u dsv=%u count=%u ready=%u bucket=%u shape=%u phase=%u record-camera=%u color=%u extent=%u frozen-camera=%u receipts=%u/%u",
        (unsigned long long)s.prefix.frame,s.untrustedQualificationCalled?1u:0u,
        s.untrustedQualificationCalled?s.untrustedLastQualification.c_str():"not-called",
        (unsigned long long)q.vs,(unsigned long long)q.ps,q.hIdentity?1u:0u,
        q.uniqueAlternate?1u:0u,q.dsvMatch?1u:0u,q.countPresent?1u:0u,
        q.ready?1u:0u,q.bucketValid?1u:0u,q.cameraShape?1u:0u,q.phasePair?1u:0u,
        q.recordCameraPresent?1u:0u,q.recordColor?1u:0u,q.recordExtent?1u:0u,
        q.frozenCamera?1u:0u,q.matching,q.expected);
    char recordRows[24*9+1]{};
    if(q.recordCameraPresent)hexWords(q.recordCamera,24,recordRows,sizeof(recordRows));
    Log::get().note("flat untrusted H record rows: frame=%llu present=%u first-q=%u last-q=%u rows270-275=%s",
        (unsigned long long)s.prefix.frame,q.recordCameraPresent?1u:0u,q.first,q.last,
        q.recordCameraPresent?recordRows:"unavailable");
    if(firstGap)Log::get().note("flat untrusted H first unaccounted: frame=%llu q=%u VS=%016llX PS=%016llX color=%p depth=%p dsv=%p camera=%016llX has-camera=%u write=%llu/%u observed=%u completed=%u",
        (unsigned long long)s.prefix.frame,firstGap->firstSeq,
        (unsigned long long)firstGap->vs,(unsigned long long)firstGap->ps,
        firstGap->color,firstGap->depth,firstGap->dsv,
        (unsigned long long)firstGap->cameraHash,firstGap->hasCamera?1u:0u,
        (unsigned long long)firstGap->writeEpoch,firstGap->writeSeq,
        firstGap->draws,firstGapCompleted);
}
// Section 104, the pool-less view. A selection that holds no motion source (FlatMonoFrame::sourceFree: the scene drew no pool-family
// draw at all) is named from the selection itself, because no draw named the world. The depth, scene constants and camera are the ones
// the selector took from the HDR's first camera draw, so every comparison against the naming after this (the identity check at the
// resolve, the overlay admission of what draws next) is the selection against itself. Idempotent: a world a draw named stands, and is
// compared. The frame's engine views follow from engineVelocityPrepareSourceFree, at the resolve.
static bool nameSourceFree(State& s, const FlatMonoFrame& sel) {
    if(!sel.selected() || !sel.sourceFree) return false;
    if(s.namedDepth) return s.namedDepth==sel.depth && s.namedConstants==sel.sceneConstants;
    s.namedDepth=sel.depth;s.namedConstants=sel.sceneConstants;s.namedWorldQ=s.prefix.sequence;s.namedVs=s.namedPs=0;
    static_assert(sizeof(sel.camera)==kFlatCameraBytes,"the selected camera is the 96 bytes of rows 270..275");
    std::memcpy(s.namedCamera,sel.camera,sizeof(sel.camera));
    const auto reference=flatDomainWorldReference(sel.camera);
    if(reference.valid())s.worldReference=reference;
    s.frameSourceFree=true;
    return true;
}
static void hdrSelectAtTrigger(State& s) {
    const bool witnessEligible=overlayOpen(s) && s.overlay.markedDraws()!=0;
    if(witnessEligible)++s.sourceWitnessEligibleWindow;
    const bool witnessSample=witnessEligible && flatHdrShouldSampleAmbiguousSource(
        s.prefix.frame,s.sourceWitnessFirstFrame,s.sourceWitnessCaptured);
    FlatMonoFrame sel = flatSelectHdrRoute(s.prefix, s.hdr,
        [](uint64_t, uint64_t) { return true; },
        witnessSample?hdrAmbiguousSourceReport:nullptr,witnessSample?&s:nullptr,
        qualifiedUntrustedSource,&s);
    s.untrustedSupportedAlternate=sel.selected() && sel.mixedCamera;
    noteSourceFreeContent(s, sel);
    // The camera H selected is the world's: the reference for the frames after, whatever naming left (State::namingVetoedThisFrame).
    if(sel.selected()) {
        const auto reference=flatDomainWorldReference(sel.camera);
        if(reference.valid())s.worldReference=reference;
        nameSourceFree(s,sel);
    }
    // The camera a draw named the world with is not the camera H selected: the world's own draws are then every one a different camera
    // from the named one (draws of a first-person camera after naming are the named camera's, and no bucket accounts for them). The
    // grenade hold named a camera of near 0.0675 against the selected world's 0.025 for a run of 28 and 36 frames, each declined.
    if(sel.selected() && s.namedDepth && flatCameraHash(s.namedCamera)!=sel.cameraHash) {
        ++s.namedNotSelected;
        if(s.namedNotSelectedLogged<12) {
            ++s.namedNotSelectedLogged;
            float named[6][4]{};std::memcpy(named,s.namedCamera,sizeof(named));
            double np0=0,np1=0,sp0=0,sp1=0;
            const bool namedScale=flatCameraProjectionScale(named,np0,np1);
            const bool selectedScale=flatCameraProjectionScale(sel.camera,sp0,sp1);
            Log::get().note("flat world naming disagrees with H %u/12: frame=%llu naming-q=%u naming-VS=%016llX naming-PS=%016llX named-camera=%016llX named-near=%.9g named-scale=%.6g,%.6g selected-camera=%016llX selected-near=%.9g selected-scale=%.6g,%.6g reference-near=%.9g; naming takes the first supported non-weapon-family scene draw's camera, whatever camera it is",
                s.namedNotSelectedLogged,(unsigned long long)s.prefix.frame,s.namedWorldQ,(unsigned long long)s.namedVs,
                (unsigned long long)s.namedPs,(unsigned long long)flatCameraHash(s.namedCamera),named[3][2],
                namedScale?np0:0.0,namedScale?np1:0.0,(unsigned long long)sel.cameraHash,sel.camera[3][2],
                selectedScale?sp0:0.0,selectedScale?sp1:0.0,s.worldReference.nearPlane);
        }
    }
    if(sel.selected()) {
        const auto* worldBytes=reinterpret_cast<const unsigned char*>(sel.camera);
        bool alternateObserved=false, missingCamera=false;
        if(s.unclassifiedPoolOverflow) {
            const auto& first=s.unclassifiedOverflowFirst;
            if(!s.unclassifiedOverflowLogged) {
                s.unclassifiedOverflowLogged=true;
                Log::get().note("flat unclassified source table overflow at selected H: frame=%llu first-seq=%u VS=%016llX PS=%016llX color=%p depth=%p H-depth=%p b1=%p camera=%016llX has-camera=%u write=%llu/%u extent=%ux%u",
                    (unsigned long long)s.prefix.frame,first.firstSeq,
                    (unsigned long long)first.vs,(unsigned long long)first.ps,
                    first.color,first.depth,sel.depth,first.b1,
                    (unsigned long long)first.cameraHash,first.hasCamera?1u:0u,
                    (unsigned long long)first.writeEpoch,first.writeSeq,first.width,first.height);
            }
            if(s.untrusted.active() || sel.mixedCamera) {
                s.untrustedUnknown=true;++s.untrustedUnknownCause[0];
                s.untrusted.invalidate("unclassified-source-table-overflow");
            }
        }
        for(uint32_t i=0;i<s.unclassifiedPoolUsed;++i) {
            const auto& unknown=s.unclassifiedPool[i];
            if(unknown.depth!=sel.depth)continue;
            const bool sameCamera=unknown.hasCamera &&
                std::memcmp(unknown.camera,sel.camera,kFlatCameraBytes)==0;
            if(!sameCamera) {
                if(!unknown.hasCamera)missingCamera=true;
                else alternateObserved=true;
                uint32_t completed=0;
                const bool accounted=flatUntrustedObservationAccounted(
                    unknown,s.untrusted,&completed);
                const uint32_t bits=(unknown.hasCamera?1u:0u)|
                    (unknown.depth==sel.depth?2u:0u)|
                    (accounted?4u:0u);
                bool distinct=true;
                for(uint32_t j=0;j<s.unclassifiedConsumerDiagnosticsUsed;++j) {
                    const auto& seen=s.unclassifiedConsumerDiagnostics[j];
                    if(seen.vs==unknown.vs && seen.ps==unknown.ps && seen.bits==bits) {distinct=false;break;}
                }
                if(distinct && s.unclassifiedConsumerDiagnosticsUsed<16) {
                    auto& seen=s.unclassifiedConsumerDiagnostics[s.unclassifiedConsumerDiagnosticsUsed++];
                    seen.vs=unknown.vs;seen.ps=unknown.ps;seen.bits=bits;
                    float alternate[6][4]{};
                    const bool shape=unknown.hasCamera && flat_mono_detail::cameraShape(unknown.camera,alternate);
                    const bool phase=shape && flatCameraCenteredPairAtPhase(sel.camera,alternate,
                        s.phase.currentX,s.phase.currentY,sel.renderWidth,sel.renderHeight);
                    Log::get().note("flat scene source at selected H: frame=%llu first-seq=%u VS=%016llX PS=%016llX color=%p depth=%p H-depth=%p b1=%p H-b1=%p camera=%016llX H-camera=%016llX has-camera=%u shape=%u phase-pair=%u observed=%u completed=%u near=%.9g H-near=%.9g write=%llu/%u extent=%ux%u H-extent=%ux%u",
                        (unsigned long long)s.prefix.frame,unknown.firstSeq,
                        (unsigned long long)unknown.vs,(unsigned long long)unknown.ps,
                        unknown.color,unknown.depth,sel.depth,unknown.b1,sel.sceneConstants,
                        (unsigned long long)unknown.cameraHash,(unsigned long long)sel.cameraHash,
                        unknown.hasCamera?1u:0u,shape?1u:0u,phase?1u:0u,
                        unknown.draws,completed,
                        shape?alternate[3][2]:0.0f,sel.nearPlane,
                        (unsigned long long)unknown.writeEpoch,unknown.writeSeq,
                        unknown.width,unknown.height,sel.renderWidth,sel.renderHeight);
                } else if(distinct)++s.unclassifiedConsumerDiagnosticsDropped;
                if(unknown.hasCamera && !accounted) {
                    s.untrustedUnknown=true;++s.untrustedUnknownCause[1];
                    s.untrusted.invalidate("unaccounted-alternate-source-draw");
                }
            }
        }
        if(alternateObserved || sel.mixedCamera) {
            if(missingCamera) {
                s.untrustedUnknown=true;++s.untrustedUnknownCause[2];
                s.untrusted.invalidate("same-depth-source-missing-camera");
            }
            if(!s.untrusted.select(sel.depth,sel.dsv,worldBytes,
                    s.phase.currentX,s.phase.currentY)){s.untrustedUnknown=true;++s.untrustedUnknownCause[3];}
            if(!s.untrustedUnknown)sel.mixedCamera=true;
        }
    }
    if(s.untrustedUnknown && sel.selected())sel.reason=FlatMonoReason::AmbiguousSource;
    reportUntrustedAtH(s,sel);
    s.hdrSelected=sel;
    s.untrusted.consumer();
    untrustedCoverageActive.store(false,std::memory_order_release);
    if(s.untrusted.active() || s.untrustedUnknown) {
        if(sel.selected() && sel.mixedCamera)++s.untrustedAccepted;
        else if(sel.selected() && !s.untrustedUnknown)++s.untrustedWorldExcluded;
        else ++s.untrustedRefused;
        if((!sel.selected() || sel.mixedCamera || s.untrustedUnknown) && s.untrustedLines++<16)
            Log::get().note("flat untrusted camera coverage: frame=%llu draws=%u ready=%u unknown=%u mixed=%u selector=%s failure=%s; union marks original passing fragments even after world overdraw",
                (unsigned long long)s.prefix.frame,s.untrusted.drawCount(),s.untrusted.view()?1u:0u,
                s.untrustedUnknown?1u:0u,sel.mixedCamera?1u:0u,flatMonoReasonName(sel.reason),
                s.untrusted.failure()?s.untrusted.failure():"none");
    }
    // Passive foreground evidence runs even when the selector rightly refuses
    // the mixed-camera source. Use the same H target and camera record the
    // selector just saw; no later copy or treatment path can shadow it.
    if(s.foreground.active())for(uint32_t i=0;i<s.prefix.targetsUsed;++i) {
        const auto& target=s.prefix.targets[i];
        if(target.resource!=s.hdr.trigger.hdr)continue;
        s.foreground.consumer(s.context.Get(),s.prefix.frame,s.hdr.trigger.sequence,
            static_cast<ID3D11Texture2D*>(const_cast<void*>(target.writes.key.depth)),
            static_cast<ID3D11DepthStencilView*>(const_cast<void*>(target.writes.key.dsv)),
            target.hdrCamera?target.tone.camera:nullptr,foreignWork.load(std::memory_order_acquire));
        break;
    }
    if(witnessEligible && sel.reason==FlatMonoReason::AmbiguousSource)
        ++s.sourceWitnessAmbiguousWindow;
    const bool autoKey = s.hdrKey == FlatHdrKey::Auto;
    s.hdrWindow.lastVerdict = sel.selected() ? "selected" : flatMonoReasonName(sel.reason);
    s.hdrWindow.noteSelection(s.hdrWindow.lastVerdict);   // the key off's answer: what the selector says at each trigger
    if (!s.hdrFirstTriggerLogged) {
        s.hdrFirstTriggerLogged = true;
        char text[700];
        flatHdrFormatFirstTrigger(text, sizeof(text), s.prefix.frame, s.hdrKey, s.hdr);
        Log::get().note("%s; selection: %s", text, sel.selected() ? "the route could resolve this frame" : flatMonoReasonName(sel.reason));
    }
    if (autoKey && !s.hdrLatch.tripped) {
        // The route speaks for the frame's stand-down verdict only where it will resolve the frame (flatHdrTriggerSeen: a
        // selected frame the mode's route evaluates at the render size for, not one below the output and not EDVR's TAA above
        // it; those are the copy's, by its whitelist or by its structure). Its refusals add nothing: merged as themselves they
        // outrank the copy stage's structural one, and at R < D every frame the copy route refuses would read as transient, so
        // the stand-down and the F8 warning would never start.
        const FlatFrameSeen routeSeen = flatHdrTriggerSeen(sel,
            s.engine);
        if (routeSeen == FlatFrameSeen::Treatable) { s.frameSeen = FlatFrameSeen::Treatable; s.frameReason = sel.reason; }
    }
    {
        flatcpu::Scope trace(flatcpu::kTrace);
        flatTraceResolve(s.traceRing, s.hdr.trigger.hdr, s.hdr.trigger.vs, s.hdr.trigger.ps, s.hdr.trigger.sequence,
                         static_cast<uint32_t>(sel.reason));
    }
}
// At the final copy draw, in the draw scope: the admission by structure over what the reducer said (flat_copy_structure.h,
// section 83). What the whitelist selected comes back unchanged. The census token, the once-a-session first admission and
// the decline lines (each cause once, a dozen a session) are here, and the sizes it measured are published for the panel.
static FlatMonoFrame copyAdmit(State& s, const FlatRuntimeDraw& d, const FlatMonoFrame& whitelist) {
    FlatCopyPolicy policy;
    policy.structure = s.hdrKey == FlatHdrKey::Auto;
    policy.mode = s.engine;
    policy.routeLatched = s.hdrLatch.tripped;
    FlatCopyDiag diag;
    const FlatMonoFrame out = flatCopyAdmit(s.prefix, s.hdr, d, whitelist, policy, &diag);
    if (whitelist.selected()) s.copyWindow.noteWhitelist(); else s.copyWindow.note(diag);
    // The scene's size for the panel, from every final copy that has a scene: the admission's own facts where it looked, the
    // prefix model's where it did not (a frame the whitelist selected, or refused for another reason), so the F8 words never
    // read sizes older than the refusal they name.
    uint32_t sceneW = diag.sceneWidth, sceneH = diag.sceneHeight;
    if (!sceneW) {
        const FlatSceneFacts facts = flatSceneFacts(s.prefix, s.prefix.width, s.prefix.height);
        sceneW = facts.width; sceneH = facts.height;
    }
    if (sceneW)
        g_sceneSizes.store(flatHdrPackSizes(sceneW, sceneH, s.prefix.width, s.prefix.height), std::memory_order_release);
    char text[1000];
    if (diag.outcome == FlatCopyOutcome::Admitted && !s.copyFirstLogged) {
        s.copyFirstLogged = true;
        const auto route = flatResolveRoute(s.engine, out.renderWidth, out.renderHeight, out.outputWidth, out.outputHeight);
        flatCopyFormatFirstAdmission(text, sizeof(text), s.prefix.frame, diag, route.name);
        Log::get().note("%s", text);
    } else if (diag.outcome == FlatCopyOutcome::Declined && s.copyDeclineLines < 12) {
        bool seen = false;
        for (uint32_t i = 0; i < s.copyDeclineLines && !seen; ++i) seen = std::strcmp(s.copyDeclineSeen[i], diag.why) == 0;
        if (!seen) {
            s.copyDeclineSeen[s.copyDeclineLines++] = diag.why;
            flatCopyFormatDeclined(text, sizeof(text), s.prefix.frame, diag);
            Log::get().note("%s", text);
        }
    }
    return out;
}

void flatRuntimeWeaponFootprintClear(ID3D11DepthStencilView* dsv,UINT flags,UINT8 stencil) {
    if(!owner())return;
    auto& s=state();
    if(s.foreground.active() && (flags&D3D11_CLEAR_STENCIL) && dsv) {
        Ptr<ID3D11Resource> resource;dsv->GetResource(&resource);
        s.foreground.noteStencilClear(resource.Get());
    }
    if(s.weaponFootprint.active())s.weaponFootprint.clear(dsv,flags,stencil,s.prefix.sequence);
}
void flatRuntimeWeaponFootprintBeforePresent(IDXGISwapChain* swap,UINT flags) {
    if(!swap||(flags&DXGI_PRESENT_TEST))return;
    auto& s=state();if(!s.weaponFootprint.active()||!owner())return;
    FlatComputeInternalScope guard;
    s.weaponFootprint.beforePresent(s.context.Get(),s.prefix.frame,s.prefix.sequence);
}
void flatRuntimePresent(IDXGISwapChain* swap, uint64_t frame, HRESULT hr, UINT flags) {
    if (!runtimeFlatProfile() || !swap || (flags & DXGI_PRESENT_TEST)) return;
    auto& s = state(); if (s.thread && !owner()) return;
    s.untrusted.pollMask(s.context.Get());
    if(frame && frame%300==0 &&
       (s.untrustedAccepted || s.untrustedRefused || s.untrustedTreated || s.untrustedWorldExcluded)) {
        Log::get().note("flat untrusted camera coverage summary: frame=%llu selected=%llu capture-refused=%llu world-excluded=%llu actually-treated=%llu configured=%s attempt-mode=%s last-draws=%u last-ready=%u last-unknown=%u nominee-witnesses=%u nominee-dropped=%u unclassified-witnesses=%u unclassified-dropped=%u last-selector=%s last-failure=%s audit-emitted=%u audit-dropped=%u supported-first=%u supported-second=%u last-qualification=%s unknown-causes(overflow,unaccounted-alternate-draw,missing-camera,no-alternate-bucket)=%llu,%llu,%llu,%llu named-camera-not-selected=%llu",
            (unsigned long long)frame,(unsigned long long)s.untrustedAccepted,
            (unsigned long long)s.untrustedRefused,(unsigned long long)s.untrustedWorldExcluded,
            (unsigned long long)s.untrustedTreated,
            flatMonoResolveModeName(s.engine),
            flatMonoResolveModeName(s.engine),
            s.untrusted.drawCount(),s.untrusted.view()?1u:0u,s.untrustedUnknown?1u:0u,
            s.untrustedNomineeDiagnosticsUsed,s.untrustedNomineeDiagnosticsDropped,
            s.unclassifiedConsumerDiagnosticsUsed,s.unclassifiedConsumerDiagnosticsDropped,
            flatMonoReasonName(s.hdrSelected.reason),
             s.untrusted.failure()?s.untrusted.failure():"none",
             s.untrustedDiagnosticBudget.emitted(),s.untrustedDiagnosticBudget.dropped(),
             s.untrustedDiagnosticBudget.firstSupported()?1u:0u,
             s.untrustedDiagnosticBudget.secondSupported()?1u:0u,
             s.untrustedLastQualification.c_str(),
             (unsigned long long)s.untrustedUnknownCause[0],(unsigned long long)s.untrustedUnknownCause[1],
             (unsigned long long)s.untrustedUnknownCause[2],(unsigned long long)s.untrustedUnknownCause[3],
             (unsigned long long)s.namedNotSelected);
        s.untrustedAccepted=s.untrustedRefused=s.untrustedTreated=s.untrustedWorldExcluded=0;
    }
    if (drawIngressAudit.active.load(std::memory_order_acquire) &&
        drawIngressAudit.framesLeft && --drawIngressAudit.framesLeft == 0)
        reportDrawIngress("complete");
    // The flat HDR route's crash-safe breadcrumbs (flat_hdr_crumbs.h): "frame-end begin" now (the real Present has just
    // returned `hr`), "frame-end end" on every path out of this function, and the gate closed until the route admits
    // another frame. The next frame's preflight, with its resource creations, runs inside it.
    HdrCrumbFrameEnd routeFrameEnd(hr);
    s.thread = GetCurrentThreadId();
    // The Present edge: the frame window the camera injector may inject in
    // closes here and reopens only once the next frame's phase is chosen, so a
    // Present that returns early below (mode off, no device, no swap buffer)
    // can never leave the previous frame's phase injecting.
    flatCameraInjectDisarm();
    // The camera producer probe's per-Present cadence (config-gated inside).
    flatCameraProducerProbeFrame(frame);
    // Account for the completed frame before mode/resize changes or the next
    // prefix clears its identity. A resize flush sees no pending frame twice.
    finishPhaseCensusFrame(s);
    // Part B coverage census: close out the frame that just ended. Always
    // on -- see the 5s report
    // below.
    ++s.covFrames;
    if (s.observing) ++s.covFramesObserving;
    if (s.covFrameLocallyRefused) ++s.covFramesLocallyRefused;
    s.covFrameLocallyRefused = false;
    const auto mode = Config::get().requestedTemporalMode();
    const bool enabled = temporalModeEnabled(mode);
    const auto model = Config::get().getString("fix.temporal_aa_model", "k");
    const auto preset = temporalPresetFor(model);
    if (s.preset != preset.full) {
        s.preset = preset.full;
        dlaaSetPreset(preset.full);
        if (temporalEngineFor(mode) == TemporalEngine::Nvidia) {
            s.haveResolvePlan=false;s.resolvePreflight={};s.resolvePreflightRetryMs=0;
            reset();s.phase.resetHistory();
        }
        Log::get().note("flat runtime: DLSS model=%s preset=%u; applied at frame boundary%s",
            model.c_str(),preset.full,preset.known?"":" (unknown model; using K)");
    }
    if (mode != s.mode) {
        s.haveResolvePlan=false;s.resolvePreflight={};s.resolvePreflightRetryMs=0;
        s.mode = mode; reset();
        // A new mode is a new question for the stand-down: everything restarts.
        endStandDown(s, frame, "the anti-aliasing mode changed");
        engineVelocityConfigure(enabled);
        s.phase.resetHistory();
        s.engine = _stricmp(mode.c_str(), "fsr") == 0 ? FlatMonoResolveMode::Fsr :
            _stricmp(mode.c_str(), "dlss") == 0 ? FlatMonoResolveMode::Dlss :
            _stricmp(mode.c_str(), "dlaa") == 0 ? FlatMonoResolveMode::Dlaa : FlatMonoResolveMode::Taa;
        if(s.engine==FlatMonoResolveMode::Taa) {
            s.foregroundRoute.clear();for(auto& candidate:s.foregroundCandidates)retireDomainCandidate(s,candidate);
            s.foregroundSelectedDepth=nullptr;s.foregroundLastH={};
            foregroundDomainActive.store(false,std::memory_order_release);
        }
        Log::get().note("flat runtime: mode=%s experimental mono temporal; game SS controls render size; jitter uses qualified D3D11 projection scopes", mode.c_str());
    }
    g_flatRuntimeLive.store(false, std::memory_order_release);
    // Engine motion follows the stand-down (s.enginePaused); the mode of the frame that
    // starts now is decided below, and a change of it is handed over there.
    const bool enginePausedThen = s.enginePaused;
    engineVelocityConfigure(enabled && !enginePausedThen);
    // No temporal mode selected: the census stops (its gates close, a scope costs a load and a compare).
    if (!enabled) {
        s.census.idle();if(s.device || s.output || s.cameras.count())flatRuntimeResize();
        const auto request=projectionAuditRequested.take();
        const bool manual=flatCaptureBulk(request);
        if(request==FlatCaptureTier::General)Log::get().note("flat capture general: configured=%s AA=off; bounded desktop discovery only, bulk exporters not armed",mode.c_str());
        if(!manual&&!s.drawPackets.active())return;
        FlatComputeInternalScope internal;Ptr<ID3D11Device> dev;swap->GetDevice(IID_PPV_ARGS(&dev));
        if(dev){dev->GetImmediateContext(&s.drawPacketContext);swap->GetBuffer(0,IID_PPV_ARGS(&s.drawPacketOutput));}
        s.drawPacketThread=GetCurrentThreadId();
        if(s.drawPacketContext && s.drawPackets.active()) {
            flatTraceSeal(s.traceRing,false,0);s.drawPackets.trace(s.drawPacketFrame,s.traceRing,true);
            s.drawPackets.present(s.drawPacketContext.Get(),frame+1);
        }
        if(manual) {
            armDrawPackets(s,frame);
            Log::get().note("flat draw packets: capture-only AA-off ready=%u; original draws only, chronology resource mutations explicitly unavailable",
                s.drawPacketContext?1u:0u);
        }
        if(s.drawPackets.armed()) {D3D11_TEXTURE2D_DESC d{};if(s.drawPacketOutput)s.drawPacketOutput->GetDesc(&d);
            s.drawPacketFrame=frame+1;s.drawPacketSequence=0;
            traceWindow(s);flatTraceBeginFrame(s.traceRing,frame+1,s.drawPacketOutput.Get(),d.Width,d.Height,d.Format);}
        return;
    }
    static bool bounceModeSet=false;
    if (!bounceModeSet) {
        bounceModeSet=true;
        mapBounce().setMode(flatmap::Mode::Auto);
    }
    FlatComputeInternalScope guard;
    Ptr<ID3D11Device> actualDevice; swap->GetDevice(IID_PPV_ARGS(&actualDevice));
    if (s.device && actualDevice.Get() != s.device.Get()) { flatRuntimeResize(); s.thread = GetCurrentThreadId(); }
    if (!s.device) { swap->GetDevice(IID_PPV_ARGS(&s.device)); if (s.device) s.device->GetImmediateContext(&s.context); }
    if (!s.device || !s.context) return;
    // fix.ui_quality's flat layer: a device the layer has not seen (the actual-device-change branch above, or a device
    // adopted after a resize cleared the old one) releases every device child the shared layer keeps; same device, nothing.
    flatUiLayerNoteDevice(s.device.Get());
    // Stand-down (flat_standdown.h): what the frame that just ended showed about its
    // chain, and the mode of the frame that starts now. Before anything below reads s.work.
    bool engineConfiguredPaused = enginePausedThen;
    const auto syncEngine = [&] {
        if (s.enginePaused == engineConfiguredPaused) return;
        engineConfiguredPaused = s.enginePaused;
        engineVelocityConfigure(enabled && !s.enginePaused);
    };
    // The HDR route's breadcrumbs are DXMT's alone (flat_hdr_crumbs.h, THE GATE): the markers decide, once, here, with the device
    // in hand and before the key's first read arms the trail. Only the detection decides, so a Windows device writes no crumb. The 5 s line's step counts below do not depend on it.
    if (!s.crumbGateRead) {
        s.crumbGateRead = true;
        hdrCrumbEnable(flatCrumbsWantedFor(flatDetectDxmt(s.device.Get(), s.context.Get())));
    }
    // The HDR route (flat_hdr_route.h): the frame that just ended is accounted, then the key is read for the frame that
    // starts now. Before the stand-down reads the frame's verdict, which the route may have added to.
    if(overlayOpen(s)) overlayFail(s,"overlay-missing-HDR-consumer");
    hdrFrameEnd(s, frame);
    hdrReadKey(s, frame);
    const bool endedPaused = s.work == FlatWork::Paused;
    // The verdict of the frame that just ended, taken before the stand-down clears it: the source spell below reads it (section 104).
    const FlatFrameSeen endedSeen = s.frameSeen;
    const FlatMonoReason endedReason = s.frameReason;
    standDownFrame(s, frame);
    syncEngine();
    // The CPU and GPU census (flat_cpu.h): the frame that just ended is cut into its families,
    // the GPU spans that finished are read (never waited for), and every 5 s the window is
    // printed, zeros included. Instrument only: nothing below reads any of it.
    if (!s.censusHooked) { flatMonoResolveSetSpanHooks(&resolveSpanBegin, &resolveSpanEnd); s.censusHooked = true; }
    {
        static const int64_t censusFreq = flatcpu::qpcFrequency();
        const int64_t censusNow = EDVR_FLATCPU_NOW();
        const bool censusWasRunning = s.census.running();
        s.census.onFrame(censusNow, censusFreq, endedPaused);
        // Always drained, so the priming frame (or a census that was stopped) leaves no backlog to
        // be read as the first window's.
        const EngineVelocityWrapperCounts wrapper = engineVelocityTakeWrapperCounts();
        if (censusWasRunning) s.census.noteWrapper(wrapper.stateCalls, wrapper.substitutedDraws);
        // The questions the runtime answered from what it already knew (flat_query_cut.h): drained with the wrapper's counts,
        // and the frame that starts now says whether it is one of the checked ones (one in 64).
        const FlatQueryCounts queries = flatQueryCut().take();
        if (censusWasRunning) s.census.noteQueries(queries);
        flatQueryCut().beginFrame(frame);
        gpuPoll(s);
        flatcpu::WindowReport window;
        if (s.census.take(censusNow, s.standDown.standing, window)) {
            flatcpu::Lines lines;
            flatcpu::formatWindow(window, &lines);
            for (int i = 0; i < lines.count; ++i) Log::get().note("%s", lines.line[i]);
        }
        s.gpuFrameTried = false;
    }
    // The completed frame is a draw-capture sample only if it was live: the resolver did not reset
    // it, and it ran at a nonzero phase unless the jitter is off on purpose. The two frames after an
    // F10 arm were neither, and their constants carry no phase (2026-09-29).
    s.drawCapture.present(s.context.Get(),frame,flatCaptureFrameLive(flatMonoResolveLastReset(),s.frameHadPhase));
    {FlatComputeInternalScope internal;s.drawPackets.present(s.context.Get(),frame);}
    if(s.drawPackets.finished()!=s.drawPacketsReported) {
        s.drawPacketsReported=s.drawPackets.finished();
        Log::get().note("flat draw packets: completed=%u incomplete=%u selected=%u skipped=%u armed=%u; independently retained even for unqualified HDR frames",
            s.drawPackets.finished(),s.drawPackets.incomplete(),s.drawPackets.selected(),s.drawPackets.skipped(),s.drawPackets.armed()?1u:0u);
    }
    s.weaponFootprint.present(s.context.Get(),frame,s.prefix.sequence);
    s.foreground.present(s.context.Get(),frame);
    foregroundProbeActive.store(s.foreground.active(),std::memory_order_release);
    flatMonoResolvePollPixels(s.context.Get(),frame);
    if(s.phase.applied)++s.jitteredFrames;
    s.phase.finish(s.temporalAccepted && SUCCEEDED(hr),s.frameCoverage && !s.prefix.uncertain && !foreignWork.load(std::memory_order_acquire));
    // The camera injector closes the frame that just ended with the phase
    // machine's own verdict on it: previousAcceptedValid is exactly finish's
    // "clean" (backend acceptance, complete coverage, no phase failure). The
    // phase is the one the frame BEGAN with -- a failed frame's phase has been
    // zeroed by now, and a frame that failed because nothing landed is the very
    // frame the fallback must count.
    flatCameraInjectClose(s.frameHadPhase,s.phase.applied!=0,s.phase.previousAcceptedValid,s.namedDepth!=nullptr);
    // A frame with a verdict feeds the spell tracker (flat_source_spell.h): refused for no source, treated, and whether it carried a phase.
    if(endedSeen!=FlatFrameSeen::None)
        s.sourceSpell.frame(endedReason==FlatMonoReason::NoSupportedSource,endedSeen==FlatFrameSeen::Treatable,s.frameHadPhase);
    // The jitter cycle's length, read live (advanced.temporal_aa_jitter_phases: a whole number from 8 to 64, default 8).
    // A value the cycle cannot use reads as 8: one Config could not parse says so itself (once), and one outside the range is said here, once
    // per value. A change starts a new cycle, so the phase and the resolver's history restart as the jitter toggle's do, and the log names it.
    {
        const int asked=Config::get().getInt("advanced.temporal_aa_jitter_phases",8);
        bool usable=false;
        const uint32_t phases=temporalJitterPhases(asked,&usable);
        if(!usable) {
            if(!s.jitterPhasesBadSaid || s.jitterPhasesBad!=asked) {
                s.jitterPhasesBadSaid=true;s.jitterPhasesBad=asked;
                Log::get().note("flat jitter: advanced.temporal_aa_jitter_phases = %d is outside %u..%u, so the default %u is used",
                    asked,kTemporalJitterPhasesMin,kTemporalJitterPhasesMax,kTemporalJitterCount);
            }
        } else s.jitterPhasesBadSaid=false;
        if(phases!=s.jitterPhases) {
            const FlatCameraRoute route=flatCameraInjectRoute();
            Log::get().note("flat jitter: the cycle is %u phases (advanced.temporal_aa_jitter_phases, was %u); the %s camera route runs %u; "
                            "the phase and the resolver's history restart",
                phases,s.jitterPhases,flatCameraRouteName(route),flatCameraPhaseCount(route,phases));
            s.phase.resetHistory();reset();
            s.jitterPhases=phases;
        }
    }
    // Local refusal's observation exit: a positively qualified handoff on a
    // completely covered frame (the same coverage trio phase.finish used
    // above) requalifies the contract and resumes warm-up. Empty, failed,
    // uncertain or foreign-work frames keep observing.
    if (flatObservationClears(s.observing, s.observingQualifiedHandoff,
            s.frameCoverage && !s.prefix.uncertain && !foreignWork.load(std::memory_order_acquire))) {
        s.observing = false;
        Log::get().note("flat coverage: contract requalified at frame=%llu; warm-up resumes",
            (unsigned long long)frame);
    }
    // Legacy projection readiness exists in Full frames only: a stand-down releases it and
    // a resume creates it fresh here, as after a resize.
    if(!s.projection && s.work == FlatWork::Full) {
        s.projection.reset(new(std::nothrow) FlatProjectionRuntime);
        if (s.projection) s.projection->setBounceSampleHooks(
            &flatRuntimeMapBounceSamplePending,&flatRuntimeMapBounceObserveCopy);
        if (s.projection) s.projection->setBounceTelemetryHooks(
            &flatRuntimeMapBounceTrackedMap,&flatRuntimeMapBounceBankWrite,
            &flatRuntimeMapBounceRegistered);
        s.context.As(&s.projectionContext);
        if(!s.projection || !s.projectionContext || !s.projection->initialize(s.context.Get())) {
            s.projection.reset();s.projectionContext.Reset();
        }
    }
    if(s.projection)s.projection->pollColdReadbacks();
    if(s.traceDumpFrame && frame>=s.traceDumpFrame) {
        flatTraceDumpToLogDir(s, frame);
        s.traceDumpFrame=0;
    }
    if(s.projectionFrames && --s.projectionFrames==0) {
        reportProjection(s,"complete");
    }
    const auto captureRequest=projectionAuditRequested.take();
    if(captureRequest!=FlatCaptureTier::None) {
        Log::get().note("flat capture general: configured=%s last=%s accepted=%llu refused=%llu tier=%s; backend/refusal and environment counters follow normal census",mode.c_str(),s.reason,(unsigned long long)s.accepted,(unsigned long long)s.refused,flatCaptureBulk(captureRequest)?"full":"general");
        reportForegroundDomain(s);
        s.refusalCensusFrames=600;   // about five seconds of resolves; one in kFlatMonoRefusalEvery is sampled
    }
    if(flatCaptureBulk(captureRequest)) {
        armDrawIngress();
        // A diagnostic asks for everything, refused frames included: the stand-down ends.
        endStandDown(s, frame, "an F10 audit asked for everything");
        witnessRearm();   // the camera-write witness walks stacks again, bounded (flat_witness_bound.h)
        syncEngine();
        flatMonoResolveArmPixels(frame);
        s.drawCapture.arm(frame);
        armDrawPackets(s,frame);
        s.drawPacketContext=s.context;s.drawPacketThread=GetCurrentThreadId();s.drawPacketFrame=frame+1;
        s.weaponFootprint.arm(frame);
        if(s.projectionFrames)reportProjection(s,"rearmed");
        else if(s.unknownProjectionPairsUsed || s.unknownProjectionCaptureOverflow)
            reportUnknownProjection(s,"manual-rearm");
        if(!s.projection) {
            s.projection.reset(new(std::nothrow) FlatProjectionRuntime);
            if (s.projection) s.projection->setBounceSampleHooks(
                &flatRuntimeMapBounceSamplePending,&flatRuntimeMapBounceObserveCopy);
            if (s.projection) s.projection->setBounceTelemetryHooks(
                &flatRuntimeMapBounceTrackedMap,&flatRuntimeMapBounceBankWrite,
                &flatRuntimeMapBounceRegistered);
            s.context.As(&s.projectionContext);
            if(s.projection && !s.projection->initialize(s.context.Get()))s.projection.reset();
        }
        if(s.projection && s.projectionContext) {
            const uint32_t interrupted=s.projectionFrames && s.copyProvenance.attempts<2?
                s.copyProvenance.rearmedBeforeComplete+1:0;
            s.projectionFrames=900;s.projectionDraws=s.projectionDispatches=s.projectionCandidates=0;
            s.projectionReady=s.projectionMissing=s.projectionUnowned=s.projectionUnknown=0;
            s.projectionUnchanged=0;
            s.projectionViewportChecks=s.projectionViewportMismatches=0;
            s.projectionViewportWitnesses=s.projectionViewportSuppressed=s.projectionViewportUnrecorded=0;
            s.projectionDetailsUsed=0;
            s.projectionOutcomesUsed=0;s.projectionOutcomeOverflow=0;
            s.localSamples[0]={};s.localSamples[1]={};
            s.cameraProbe={};
            s.copyProvenance={};s.copyProvenance.rearmedBeforeComplete=interrupted;
            s.unknownProjectionPairsUsed=0;s.unknownProjectionCaptureOverflow=0;
            s.unknownProjectionAutomatic=s.unknownProjectionAudit=0;
            s.unknownProjectionBytesSaved=s.unknownProjectionBytesFailed=s.unknownProjectionStagesAbsent=0;
            s.resolvePreflightRetryMs=0;
            s.resolvePreflight=s.haveResolvePlan ? flatMonoResolvePreflight(s.device.Get(),s.context.Get(),s.plannedResolve) : FlatMonoResolvePreflightResult{};
            if(s.haveResolvePlan)s.resolvePreflightRetryMs=GetTickCount64();
            Log::get().note("flat projection: armed 900-frame live preparation audit; frame phase governs raster and backend; F10 does not reset live projection resources");
            // The idle window may have truncated the frames before the arm:
            // dump the first kFlatTraceFrames-1 frames recorded with the full one.
            s.traceDumpFrame=frame+kFlatTraceFrames;
            // Refresh exact creation bytecode once per manual arm, or emit
            // an explicit missing-cache result; no inferred shader admission.
            captureFlatProbeShader('v',0x5EAFFCD01B97D0C4ull);
            captureFlatProbeShader('p',0xDD371C57C9093BB8ull);
            captureFlatProbeShader('v',kHdrCopyVs);
            captureFlatProbeShader('p',kHdrCopyPs);
            // The gameplay HDR source rejected in the FSR conflict audit.
            // Its creation bytes identify whether camera-free admission is safe.
            captureFlatProbeShader('p',0x07B3F82100F29401ull);
            // Both exact first-bad on-foot camera pairs: F10 requests cached
            // creation bytes even if they predate this arm. Never admits a draw.
            for(size_t i=1;i<kFlatCameraProbePairCount;++i){
                captureFlatProbeShader('v',kFlatCameraProbePairs[i].vs);
                captureFlatProbeShader('p',kFlatCameraProbePairs[i].ps);
            }
            // Exact unknown scene pairs observed in build 0150638a. These
            // creation-cache probes run once per manual F10 arm, never per draw.
            constexpr uint64_t unknownVs[]={0xA1B7CFCD0BE7493Eull,0xCE24A73943632F55ull,
                0x41E245D488BFE83Eull,0xB12F7A618E1BDE98ull,0x203DF51758AADC4Dull,
                0x98397963AAEC45D3ull,0xB75A6FF2CA9FA5D6ull,0x124D7F3F649138D4ull,
                0x5C1D8EF529324A22ull,0x820E5C131B99361Dull};
            constexpr uint64_t unknownPs[]={0x2DB678B6B558B604ull,0x1F64463B15189104ull,
                0x6EF82262EB12A037ull,0x42AC0CACC9CDF72Bull,0xEEAAC839A9F09448ull,
                0xCAB49794BB439D03ull,0xD56F859BE4781431ull,0x8085AE8DD1906CDCull,
                0xC49F999F7D3C801Dull,0x6EAA86EFE135B2D4ull};
            for(uint32_t i=0;i<10;++i){captureFlatProbeShader('v',unknownVs[i]);captureFlatProbeShader('p',unknownPs[i]);}
        } else {s.projection.reset();s.projectionContext.Reset();s.projectionFrames=0;
            Log::get().note("flat projection: arm refused (allocation/context1/runtime unavailable), raster-phase=0");}
    }
    const uint64_t preflightNow=GetTickCount64();
    if(s.projection && s.haveResolvePlan && !s.resolvePreflight.readyForRasterJitter() &&
       (!s.resolvePreflightRetryMs || preflightNow-s.resolvePreflightRetryMs>=1000)) {
        s.resolvePreflight=flatMonoResolvePreflight(s.device.Get(),s.context.Get(),s.plannedResolve);
        s.resolvePreflightRetryMs=preflightNow;
    }
    // A success-status present is not a failed frame: DXGI returns status
    // codes (occlusion and friends) alongside S_OK, and gating on S_OK reset
    // temporal history every frame for as long as the status persisted --
    // the 2026-09-27 0.5x flight's every-frame no-previous storm with the
    // phase pinned at zero. Log the value so the flight names what the game
    // actually returns; reset only on a real failure.
    if (hr != S_OK) {
        ++s.presentNotOkWindow;
        if (s.presentNotOkLogged < 8) {
            ++s.presentNotOkLogged;
            Log::get().note("flat runtime: present result 0x%08lX is not S_OK; history and jitter reset only on FAILED",
                static_cast<unsigned long>(hr));
        }
    }
    if (s.frameSourceFree) s.sourceSpell.sourceFreeFrame(s.treated && SUCCEEDED(hr));
    if (!s.treated || FAILED(hr)) reset();
    Ptr<ID3D11Texture2D> output; if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&output)))) return;
    // Swapchain image rotation does not change render scale. Resize/device
    // teardown clears the published extent; each qualified handoff updates it.
    if (s.output.Get() != output.Get()) reset(); s.output = output;
    D3D11_TEXTURE2D_DESC d{}; output->GetDesc(&d);
    const bool compatible=s.havePrevious && s.temporalAccepted && s.previous.frame==frame &&
        s.previous.outputWidth==d.Width && s.previous.outputHeight==d.Height && s.resolvePreflight.readyForRasterJitter();
    s.phaseWidth=s.haveResolvePlan?s.plannedResolve.renderWidth:0;
    s.phaseHeight=s.haveResolvePlan?s.plannedResolve.renderHeight:0;
    if(s.havePrevious) {
        s.phaseDepth=static_cast<ID3D11Resource*>(const_cast<void*>(s.previous.depth));
        s.phaseHdr=static_cast<ID3D11Resource*>(const_cast<void*>(s.previous.hdr));
    }
    // The camera injector selects this frame's owner BEFORE the phase machine
    // begins: the route decides whether a phase is generated at all. Upstream
    // needs no legacy plan but still yields to observation (a frame the resolve
    // will skip must not be jittered at the source); Legacy and Off are exactly
    // the expression this line had before the wiring. A switch of history
    // identity between the two routes resets history once, here.
    flatCameraInjectFrame(frame + 1,enabled);
    if(flatCameraInjectTakeHistoryReset()) {s.phase.resetHistory();reset();}
    const FlatCameraRoute phaseRoute=flatCameraInjectRoute();
    s.phase.beginFrame(flatCameraPhaseEnabled(phaseRoute,true,s.observing,s.projection!=nullptr),
        compatible,s.phaseWidth,s.phaseHeight,flatCameraPhaseCount(phaseRoute,s.jitterPhases));
    s.frameHadPhase=nonzeroPhase(s);
    flatCameraInjectArm(); // the phase is chosen: the injector's frame window opens
    s.frameCoverage=true;s.temporalAccepted=false;
    s.jitterReason=nonzeroPhase(s)?"live":"warming";
    if(s.projection)s.projection->enableColdReadback(!nonzeroPhase(s));
    for (auto& r : s.colors) r.Reset(); for (auto& r : s.depths) r.Reset();
    // A Paused frame (stand-down) is not watched: no prefix to clear -- the frame's one
    // large reset -- and no trace slot to rotate, so the ring keeps the last watched
    // frames for an F10 dump instead of filling with empty ones.
    if (s.work != FlatWork::Paused) s.prefix = FlatRuntimePrefix{};
    s.prefix.frame = frame + 1;
    if(s.engine!=FlatMonoResolveMode::Taa && s.work==FlatWork::Full) {
        s.foregroundDomainFrame=s.prefix.frame;s.foregroundSelectedDepth=nullptr;
        s.foregroundHRefusal=nullptr;
        s.foregroundRoute.beginFrame(s.prefix.frame);
        foregroundDomainActive.store(true,std::memory_order_release);
    } else foregroundDomainActive.store(false,std::memory_order_release);
    s.overlay.beginFrame(frame + 1);
    s.untrusted.beginFrame(frame + 1);
    s.untrustedUnknown=false;
    s.untrustedSupportedAlternate=false;
    s.untrustedQualification={};s.untrustedQualificationCalled=false;
    s.untrustedQualificationFailed=false;
    s.unclassifiedPoolUsed=0;s.unclassifiedPoolOverflow=false;
    untrustedCoverageActive.store(false,std::memory_order_release);
    overlaySuffixActive.store(false,std::memory_order_release);
    s.overlayFailureNoted = false;
    // Trace ring: seal the frame that just ended with the hash over every
    // copy outcome it produced, then reset the contract for the next frame. Through
    // Paused frames the contract is the last watched frame's, untouched, so sealing
    // again writes the same values into the same slot.
    flatTraceSeal(s.traceRing, !s.drawPacketOnlyObserved&&s.traceContract.produced,
                  !s.drawPacketOnlyObserved&&s.traceContract.produced ? flatFrameContractHash(s.traceContract) : 0);
    s.drawPackets.trace(s.traceRing.headers[s.traceRing.slot].frame,s.traceRing,true);
    s.drawPacketOnlyObserved=false;
    traceWindow(s);
    if (s.work != FlatWork::Paused || s.drawPackets.armed()) {
        s.traceContract = FlatFrameContract{};
        flatTraceBeginFrame(s.traceRing, frame + 1, output.Get(), d.Width, d.Height, d.Format);
    }
    s.drawPacketFrame=frame+1;s.drawPacketSequence=0;
    s.drawPacketContext=s.context;s.drawPacketThread=GetCurrentThreadId();s.drawPacketOutput=output;
    s.phaseCensusPending=s.work != FlatWork::Paused;
    s.phaseCensusFailed=false;
    s.prefix.output = output.Get(); s.prefix.width = d.Width; s.prefix.height = d.Height; s.prefix.format = d.Format;
    flatRuntimeMapFocusFrame();                   // the frame's one GuiFocus read: the map plane and the map families answer from it
    flatUiLayerFrame(flatRuntimeMapOpenFrame());  // fix.ui_quality's flat layer: its 30 s lines (flat_ui_layer.h)
    if(s.namingVetoedThisFrame) {
        // A frame that vetoed a draw: if the world was named anyway the veto did its work; if nothing named it, the reference may be the
        // one that is wrong, and the third such frame in a row gives it up (the next naming and the next H select a new one).
        if(s.namedDepth)s.namingVetoStreak=0;
        else if(++s.namingVetoStreak>=kFlatNamingVetoFrames){s.worldReference=FlatDomainWorldReference{};s.namingVetoStreak=0;++s.namingVetoReleases;}
        s.namingVetoedThisFrame=false;
    } else s.namingVetoStreak=0;
    s.namedDepth = s.namedConstants = nullptr;s.namedWorldQ=0; s.treated = false;s.frameSourceFree=false;
    flatHdrBeginFrame(s.hdr, d.Width, d.Height); s.hdrTreated = false;
    s.observingQualifiedHandoff = false;
    s.drawCapture.begin(frame+1,s.phaseDepth.Get(),s.phaseWidth,s.phaseHeight);
    foreignWork.store(false, std::memory_order_release);
    // Retain bounded CB identities across frames: unchanged bindings are legal.
    s.cameras.newFrame();
    const auto now = GetTickCount64();
    if (now - s.lastReport >= 5000) {
        reportDrawIngress("progress", false);
        reportMapBounce(frame,now,true);
        reportPhaseCensus(s,"5s");
        if(s.projectionFrames)reportProjection(s,"progress");
        else reportUnknownProjection(s,"5s");
        s.foreground.logStatus();
        reportForegroundDomain(s);
        reportSourceSpell(s);
        Log::get().note("flat HDR image continuation: accepted=%llu refused=%llu; source writes require current matching scene provenance",
            (unsigned long long)s.hdrCopiesAccepted,(unsigned long long)s.hdrCopiesRefused);
        // static-scene-frames: frames the resolver was handed with the menu's stale-slot policy on
        // (FlatMonoResolveFrame::staticScene, set from an accepted copy). Zero while accepted grows
        // is what "the policy never ran" looks like: no menu frame reached the resolver, and the
        // flat runtime line's last= and accepted counts say why.
        Log::get().note("flat menu HDR copy: accepted=%llu refused=%llu static-scene-frames=%llu; source requires current scene/depth/camera provenance",
            (unsigned long long)s.menuCopiesAccepted,(unsigned long long)s.menuCopiesRefused,(unsigned long long)s.staticSceneFrames);
        // The steady-detail rule (always on: the resolver is handed steadyDetail = true at both treatment call sites), every window while a
        // temporal mode runs, zeros included: the resolves whose prep ran the depth check against last frame's depth (ran) and the ones
        // that could not (skipped: a reset frame is neither). "depth-check=0/0" is no frame reaching the resolver in the window; ran=0 with
        // skipped above 0 is the check never having had a depth to check against. The token reads "on" in every build's log (the log
        // reader parses it, and older logs carried "off" for a rig that had set the retired key off).
        {
            const FlatMonoRefusalCensus steady=flatMonoResolveTakeRefusalCensus();
            Log::get().note("flat steady detail 5s: steady-detail=on depth-check=%llu/%llu; a stale-slot pixel takes the camera term only where "
                            "last frame's depth confirms it (ran/skipped resolves this window)",
                (unsigned long long)steady.checked,(unsigned long long)steady.skipped);
            if(steady.asked || steady.sampled || steady.frames) {
                char classes[384]{};size_t used=0;
                for(uint32_t i=0;i<kFlatMonoRefusalStaleKept && used<sizeof(classes);++i)if(steady.counts[i]) {
                    const int n=std::snprintf(classes+used,sizeof(classes)-used,"%s%s=%llu",used?" ":"",flatMonoClassName(i),
                        (unsigned long long)steady.counts[i]);
                    if(n<0)break;used+=static_cast<size_t>(n);
                }
                // The refused first-person pixels by the reason the foreground map gave (flat_foreground_motion_shader.h): a split of the
                // weapon-refused class above, in the same pixels.
                char reasons[320]{};size_t usedReasons=0;
                for(uint32_t i=0;i<kFlatMonoWeaponReasons && usedReasons<sizeof(reasons);++i)if(steady.weaponReasons[i]) {
                    const int n=std::snprintf(reasons+usedReasons,sizeof(reasons)-usedReasons,"%s%s=%llu",usedReasons?" ":"",
                        flatMonoWeaponReasonName(i),(unsigned long long)steady.weaponReasons[i]);
                    if(n<0)break;usedReasons+=static_cast<size_t>(n);
                }
                const uint64_t refused=steady.refused();
                Log::get().note("flat refusal census 5s: asked=%llu sampled=%llu read=%llu dropped=%llu every=%u size=%ux%u pixels=%llu "
                                "refused=%llu (%.4f%%) stale-kept=%llu view=%s; refused by class: %s; weapon-refused by reason: %s; with DLSS or FSR a refused pixel "
                                "shows the raw current frame in place of the backend's result",
                    (unsigned long long)steady.asked,(unsigned long long)steady.sampled,(unsigned long long)steady.frames,
                    (unsigned long long)steady.dropped,steady.every,steady.width,steady.height,(unsigned long long)steady.pixels,
                    (unsigned long long)refused,steady.pixels?100.0*double(refused)/double(steady.pixels):0.0,
                    (unsigned long long)steady.counts[kFlatMonoRefusalStaleKept],"off",
                    used?classes:"none",usedReasons?reasons:"none");
            }
            // The camera term's translation precision (section 104): float32 spacing at row 275's size against its per-frame step.
            // A spacing near a walking step (a few centimetres a frame) quantises the motion the camera term hands the upscaler.
            if(s.origin.frames) {
                const State::CameraOriginWindow& o=s.origin;
                const float spacing=std::nextafter(o.maxAbs,std::numeric_limits<float>::infinity())-o.maxAbs;
                Log::get().note("flat camera origin 5s: frames=%llu moved=%llu row275=(%.9g,%.9g,%.9g) max-abs=%.9g float-spacing=%.3g "
                                "step max=%.4g smallest=%.4g; the camera term adds row 275's step, so a spacing near a walking step "
                                "quantises the motion it hands the upscaler",
                    (unsigned long long)o.frames,(unsigned long long)o.moved,o.last[0],o.last[1],o.last[2],o.maxAbs,spacing,
                    o.maxStep,o.minStep);
            }
            s.origin={};
            // The System Map's plane (FlatMonoResolveFrame::mapPlane), every window, zeros included: map-frames the reduction ran for, its
            // dispatches, the last range read back and how many read-backs had no non-zero depth. "map-frames=0" with the map open is the
            // flag never reaching the resolver; "reductions" above map-frames is a reduction that never ran; a range with a midpoint near
            // 0.0011-0.0012 is the map plane (the body depths are 0.00113-0.00119).
            {
                const FlatMonoMapPlane plane=flatMonoResolveTakeMapPlane();
                const MapPlaneWatch& w=mapPlaneWatch();
                char focus[16]="unknown";
                if(w.focusKnown)std::snprintf(focus,sizeof(focus),"%u",w.focus);
                // The decoded range is printed only when it is valid (never NaN); the raw words always, so a bad clear or a bad read is visible as
                // words=FFFFFFFF/FFFFFFFF or an empty plane=none beside them. Word 1 is the inverted far depth (flat_map_plane_range.h).
                char range[96]="none";
                if(plane.haveRange)std::snprintf(range,sizeof(range),"%.6f..%.6f (midpoint %.6f)",plane.minDepth,plane.maxDepth,plane.midpoint());
                char words[24]="none";
                if(plane.haveWords)std::snprintf(words,sizeof(words),"%08X/%08X",plane.nearWord,plane.farWord);
                // The watcher's own state, ungated by profile (journalRawStatus): whether it runs, how many Status.json reads parsed, and the last
                // sample's GuiFocus. statusSamples=0 with watcher=active is the file never being read; gui-known=0 is the field absent.
                const JournalRawStatus raw=journalRawStatus();
                Log::get().note("flat map motion 5s: focus=%s map-frames=%llu reductions=%llu plane=%s words=%s empty=%llu watcher=%s status-samples=%u "
                                "gui-known=%d gui=%u; the System Map's pixels with no depth take the plane's motion while it is open",
                    focus,(unsigned long long)plane.frames,(unsigned long long)plane.reductions,range,words,(unsigned long long)plane.empty,
                    raw.active?"active":"inactive",raw.statusSamples,raw.guiKnown?1:0,raw.gui);
            }
        }
        // The census of unkeyed pairs, every window while a temporal mode runs (empty
        // included: an absent line is what "this block never ran" looks like). A pair
        // named here draws in a known pool family with no keyed pixel shader, so its
        // pixels have no engine slot (engine_velocity_unkeyed.h).
        {
            char unkeyed[560]; engineVelocityFormatUnkeyed(unkeyed,sizeof(unkeyed));
            Log::get().note("%s",unkeyed);
        }
        Log::get().note("flat jitter: enabled=%u wanted=%u phases=%u phase=(%.5g,%.5g) previous=(%.5g,%.5g) warm=%u frames=%llu draws=%llu dispatches=%llu refusals=%llu state=%s history-valid=%u",
            enabled?1u:0u,1u,s.phase.phaseCount,
            s.phase.currentX,s.phase.currentY,s.phase.previousX,s.phase.previousY,s.phase.warmFrames,
            (unsigned long long)s.jitteredFrames,(unsigned long long)s.jitterDraws,(unsigned long long)s.jitterDispatches,
            (unsigned long long)s.jitterRefusals,s.jitterReason,s.phase.previousAcceptedValid?1u:0u);
        Log::get().note("flat late overlay 5s: planned-draws=%llu fully-marked-draws=%llu isolated-consumer-frames=%llu refused-frames=%llu reasons=%zu",
            (unsigned long long)s.overlayPlannedWindow,(unsigned long long)s.overlayMarkedWindow,
            (unsigned long long)s.overlayIsolatedWindow,(unsigned long long)s.overlayRefusedWindow,
            s.overlayRefusalWindow.size());
        Log::get().note("flat late overlay replay 5s: candidates=%llu completed=%llu refusals=%zu",
            (unsigned long long)s.overlayReplayCandidates,
            (unsigned long long)s.overlayReplayCompleted,s.overlayReplayRefusals.size());
        for(const auto& entry:s.overlayReplayRefusals)
            Log::get().note("flat late overlay replay refusal: reason=%s count=%llu",
                entry.first.c_str(),(unsigned long long)entry.second);
        for(const auto& entry:s.overlayRefusalWindow)
            Log::get().note("flat late overlay refusal: reason=%s count=%llu",entry.first.c_str(),
                (unsigned long long)entry.second);
        for(const auto& entry:s.overlayMutationWindow)
            Log::get().note("flat late overlay mutation refusal: %s count=%llu",entry.first.c_str(),
                (unsigned long long)entry.second);
        const auto& blendSample=s.overlayBlendDiagnostic;
        Log::get().note("flat late overlay blend diagnostic 5s: ready=1 guard-failures=%llu samples=%u status=%s",
            (unsigned long long)blendSample.failures,blendSample.captured?1u:0u,
            blendSample.captured?"captured":"no-dual-source-sample");
        if(blendSample.captured) {
            Log::get().note("flat late overlay shader diagnostic: frame=%llu q=%u read=%u eligible=%u previously-created=%u reason=%s raw-VS-available=%u raw-VS-bytes=%zu raw-PS-available=%u raw-PS-bytes=%zu capture-recorded=%u VS-saved=%u PS-saved=%u raw-PS-qualified=%u raw-PS-reason=%s; capture limited to 64 unique pairs per session, preflight only with no admission changes",
                (unsigned long long)blendSample.frame,blendSample.sequence,
                blendSample.shader.read?1u:0u,blendSample.shader.eligible?1u:0u,
                blendSample.shader.created?1u:0u,blendSample.shader.reason.c_str(),
                blendSample.rawVsAvailable?1u:0u,blendSample.rawVsBytes,
                blendSample.rawPsAvailable?1u:0u,blendSample.rawPsBytes,
                blendSample.captureRecorded?1u:0u,blendSample.vsSaved?1u:0u,blendSample.psSaved?1u:0u,
                blendSample.rawPsQualified?1u:0u,blendSample.rawPsReason.empty()?"not-sampled-or-cap":blendSample.rawPsReason.c_str());
            Log::get().note("flat late overlay blend sample: frame=%llu q=%u actual-VS=%016llX VS-present=%u actual-PS=%016llX PS-present=%u VS-object=%p PS-object=%p hdr=%p dsv=%p active-rtv-mask=%02X active-rtv-count=%u alpha-to-coverage=%u independent=%u effective-src1-slots=%02X ps-output-signature-known=%u",
                (unsigned long long)blendSample.frame,blendSample.sequence,
                (unsigned long long)blendSample.vsHash,blendSample.vs?1u:0u,
                (unsigned long long)blendSample.psHash,blendSample.ps?1u:0u,
                blendSample.vs,blendSample.ps,blendSample.hdr,blendSample.dsv,
                blendSample.activeRtvMask,blendSample.activeRtvCount,
                blendSample.blend.AlphaToCoverageEnable?1u:0u,
                blendSample.blend.IndependentBlendEnable?1u:0u,blendSample.effectiveSlots,
                blendSample.psOutputSignatureKnown?1u:0u);
            for(unsigned i=0;i<8;++i) {
                const auto& t=blendSample.blend.RenderTarget[i];
                Log::get().note("flat late overlay blend target: frame=%llu q=%u slot=%u active=%u enabled=%u write-mask=%02X src=%u dst=%u op=%u alpha-src=%u alpha-dst=%u alpha-op=%u effective-src1-channels=%02X",
                    (unsigned long long)blendSample.frame,blendSample.sequence,i,
                    (blendSample.activeRtvMask&(1u<<i))?1u:0u,t.BlendEnable?1u:0u,
                    (unsigned)t.RenderTargetWriteMask,(unsigned)t.SrcBlend,(unsigned)t.DestBlend,
                    (unsigned)t.BlendOp,(unsigned)t.SrcBlendAlpha,(unsigned)t.DestBlendAlpha,
                    (unsigned)t.BlendOpAlpha,(unsigned)blendSample.effectiveChannels[i]);
            }
        }
        s.overlayBlendDiagnostic={};
        Log::get().note("flat HDR source witness 5s: enabled=1 limit=2 captured=%u first-frame=%llu second-earliest=%llu eligible=%llu ambiguous=%llu; automatic on marked overlay ambiguity, no F10",
            s.sourceWitnessCaptured,(unsigned long long)s.sourceWitnessFirstFrame,
            (unsigned long long)(s.sourceWitnessFirstFrame?s.sourceWitnessFirstFrame+60:0),
            (unsigned long long)s.sourceWitnessEligibleWindow,
            (unsigned long long)s.sourceWitnessAmbiguousWindow);
        s.overlayPlannedWindow=s.overlayMarkedWindow=s.overlayIsolatedWindow=s.overlayRefusedWindow=0;
        s.overlayRefusalWindow.clear();
        s.overlayReplayCandidates=s.overlayReplayCompleted=0;
        s.overlayReplayRefusals.clear();
        s.overlayMutationWindow.clear();
        s.sourceWitnessEligibleWindow=s.sourceWitnessAmbiguousWindow=0;
        // The camera injector's row bookkeeping, every window while a temporal mode runs
        // (the camera path is on with it; zeros included: an absent line is what "the
        // wiring never ran" looks like). The tripwire is cumulative on purpose -- once it
        // is nonzero it stays.
        if(flatCameraInjectRoute()!=FlatCameraRoute::Off) {
            FlatCameraRowsFields rowsFields;
            rowsFields.frames=s.rows.frames;rowsFields.unjitteredResolves=s.rows.unjittered;
            rowsFields.zeroPhaseResolves=s.rows.zeroPhase;rowsFields.pairs=s.rows.pairs;
            rowsFields.legacyAppliedUnderUpstream=s.rows.legacyAppliedUnderUpstream;
            rowsFields.legacyPrepSkipped=s.rows.legacyPrepSkipped;
            char rowsText[420];flatCameraFormatRows(rowsText,sizeof(rowsText),rowsFields);
            Log::get().note("%s",rowsText);
            const uint64_t tripwire=s.rows.legacyAppliedUnderUpstream;
            s.rows=State::RowsWindow{};s.rows.legacyAppliedUnderUpstream=tripwire;
        }
        if(s.spatialFallbacks || s.spatialFallbackFailures)
            Log::get().note("flat runtime spatial fallback cumulative: recovered=%llu failed=%llu history=invalid-on-recovery",
                (unsigned long long)s.spatialFallbacks,(unsigned long long)s.spatialFallbackFailures);
        Log::get().note("flat runtime: treated=%llu refused=%llu last=%s render-source=game-SS jitter=(%.5g,%.5g) accepted-reset-5s=%llu accepted-history-5s=%llu treated-streak=%llu longest-treated-streak=%llu",
            static_cast<unsigned long long>(s.accepted), static_cast<unsigned long long>(s.refused), s.reason,s.phase.currentX,s.phase.currentY,
            static_cast<unsigned long long>(s.acceptedResetWindow), static_cast<unsigned long long>(s.acceptedHistoryWindow),
            static_cast<unsigned long long>(s.streak), static_cast<unsigned long long>(s.longestStreak));
        for (const auto& entry : s.refusedWindow)
            Log::get().note("flat runtime refusal 5s: reason=%s count=%llu", entry.first.c_str(), static_cast<unsigned long long>(entry.second));
        // The HDR route's census token, every window while a temporal mode runs, zeros included: an absent line is what
        // "the detector never ran" looks like. With the key off it is the flight's observation; with it auto, the state.
        {
            char hdrText[1024];
            // The resolver's step counts are cumulative; the window prints what they gained since the last one.
            {
                const FlatMonoResolveStats rs = flatMonoResolveStats();
                FlatHdrSteps& seen = s.hdrStepsSeen; FlatHdrSteps& gained = s.hdrWindow.steps;
                gained.captured = rs.hdrCaptured - seen.captured; seen.captured = rs.hdrCaptured;
                gained.copied = rs.hdrCopied - seen.copied; seen.copied = rs.hdrCopied;
                gained.prepped = rs.hdrPrepped - seen.prepped; seen.prepped = rs.hdrPrepped;
                gained.backend = rs.hdrBackend - seen.backend; seen.backend = rs.hdrBackend;
                gained.finished = rs.hdrFinished - seen.finished; seen.finished = rs.hdrFinished;
                gained.restored = rs.hdrRestored - seen.restored; seen.restored = rs.hdrRestored;
            }
            flatHdrFormatWindow(hdrText, sizeof(hdrText), s.hdrKey,
                s.hdrKey == FlatHdrKey::Auto ? (s.hdrLatch.tripped ? FlatHdrState::Latched : FlatHdrState::Active)
                                             : FlatHdrState::Observing, s.hdrWindow);
            Log::get().note("%s", hdrText);
            s.hdrWindow.reset();
        }
        // The final copy's admission by structure (flat_copy_structure.h, section 83), every window while a temporal mode runs,
        // zeros included: an absent line is what "the admission never ran" looks like, and `whitelist=N admitted=0` is it running
        // over frames that all had a known tone pass (or, with the route's key off, `key-off=N` for the frames it left alone).
        {
            char copyText[1200];
            flatCopyFormatWindow(copyText, sizeof(copyText), s.hdrKey == FlatHdrKey::Auto, s.copyWindow);
            Log::get().note("%s", copyText);
            s.copyWindow.reset();
            // The copy route's weapon census, every window, zeros included (see FlatCopyWeaponWindow).
            char weaponText[480];
            flatCopyWeaponFormatWindow(weaponText, sizeof(weaponText), flatMonoResolveModeName(s.engine), s.copyWeaponWindow);
            Log::get().note("%s", weaponText);
            s.copyWeaponWindow.reset();
        }
        // Part B coverage census: always printed, even when every field is
        // zero -- that is how "code never ran" (line absent) differs from
        // "ran, nothing refused" (local-refused=0 on a line that is present).
        Log::get().note("flat coverage 5s: partial=%s observing=%u scene-draws=%llu exact=%llu generic=%llu inert=%llu "
            "unchanged=%llu local-refused=%llu frames=%llu frames-locally-refused=%llu "
            "returned-to-observation=%llu frames-observing=%llu memo-evictions=%llu",
            "on", s.observing?1u:0u,
            static_cast<unsigned long long>(s.covSceneDraws), static_cast<unsigned long long>(s.covExact),
            static_cast<unsigned long long>(s.covGeneric), static_cast<unsigned long long>(s.covInert),
            static_cast<unsigned long long>(s.covUnchanged), static_cast<unsigned long long>(s.covLocalRefused),
            static_cast<unsigned long long>(s.covFrames), static_cast<unsigned long long>(s.covFramesLocallyRefused),
            static_cast<unsigned long long>(s.covObservationEntries), static_cast<unsigned long long>(s.covFramesObserving),
            static_cast<unsigned long long>(s.covMemoEvictions));
        {
            uint32_t order[32];
            for (uint32_t i=0;i<s.covRefusedPairsUsed;++i) order[i]=i;
            std::sort(order, order+s.covRefusedPairsUsed, [&](uint32_t a, uint32_t b) {
                return s.covRefusedPairs[a].draws > s.covRefusedPairs[b].draws;
            });
            for (uint32_t i=0;i<s.covRefusedPairsUsed && i<5;++i) {
                const auto& e = s.covRefusedPairs[order[i]];
                Log::get().note("flat coverage locally-refused 5s: VS=%016llX PS=%016llX reason=%s draws=%llu",
                    static_cast<unsigned long long>(e.vs), static_cast<unsigned long long>(e.ps),
                    e.reason, static_cast<unsigned long long>(e.draws));
            }
        }
        for (const auto& entry : s.conflictWindow)
            Log::get().note("flat runtime conflict 5s: cause=%s count=%llu", entry.first.c_str(), static_cast<unsigned long long>(entry.second));
        uint64_t witnessSites = 0; for (const auto& st : g_camWitness.sites) witnessSites += st.address != nullptr;
        Log::get().note("flat camera witness 5s: writes=%llu unique-sites=%llu dedup-hits=%llu stack-drops=%llu stack-walks=%u (%s); every camera-table write is a producer witness candidate",
            static_cast<unsigned long long>(g_camWitness.writes), static_cast<unsigned long long>(witnessSites),
            static_cast<unsigned long long>(g_camWitness.dedupHits), static_cast<unsigned long long>(g_camWitness.budgetDropped),
            g_camWitness.bound.walks,
            g_camWitness.sitesFull ? "all site slots claimed" : FlatWitnessBound::stopName(g_camWitness.bound.why));
        Log::get().note("flat runtime adapter reset 5s: no-previous=%llu frame-gap=%llu depth-change=%llu color-change=%llu extent-change=%llu present-not-ok=%llu",
            static_cast<unsigned long long>(s.resetMissingWindow), static_cast<unsigned long long>(s.resetGapWindow),
            static_cast<unsigned long long>(s.resetDepthWindow), static_cast<unsigned long long>(s.resetColorWindow),
            static_cast<unsigned long long>(s.resetExtentWindow), static_cast<unsigned long long>(s.presentNotOkWindow));
        const auto renderer = flatMonoResolveStats();
        Log::get().note("flat runtime renderer cumulative: calls=%llu init=%llu context-change=%llu allocations=%llu full-reset=%llu invalidations=%llu accepted-reset=%llu accepted-continue=%llu requested-reset=%llu lost-history=%llu frame-gap=%llu invalid-prev-camera=%llu format-change=%llu camera-cut=%llu backend-failure=%llu continue-run=%llu longest-continue-run=%llu hdr-resolves=%llu hdr-spatial=%llu",
            static_cast<unsigned long long>(renderer.calls), static_cast<unsigned long long>(renderer.initializations),
            static_cast<unsigned long long>(renderer.contextPointerMismatches), static_cast<unsigned long long>(renderer.allocations),
            static_cast<unsigned long long>(renderer.fullResets), static_cast<unsigned long long>(renderer.invalidations),
            static_cast<unsigned long long>(renderer.acceptedResets), static_cast<unsigned long long>(renderer.acceptedContinues),
            static_cast<unsigned long long>(renderer.requestedResets), static_cast<unsigned long long>(renderer.lostHistory),
            static_cast<unsigned long long>(renderer.frameGaps), static_cast<unsigned long long>(renderer.invalidPreviousCameras),
            static_cast<unsigned long long>(renderer.formatChanges), static_cast<unsigned long long>(renderer.cameraCuts),
            static_cast<unsigned long long>(renderer.backendFailures), static_cast<unsigned long long>(renderer.currentContinueRun),
            static_cast<unsigned long long>(renderer.longestContinueRun), static_cast<unsigned long long>(renderer.hdrResolves),
            static_cast<unsigned long long>(renderer.hdrSpatial));
        s.refusedWindow.clear(); s.acceptedResetWindow = s.acceptedHistoryWindow = 0;
        s.resetMissingWindow = s.resetGapWindow = s.resetDepthWindow = s.resetColorWindow = s.resetExtentWindow = 0;
        s.presentNotOkWindow = 0;
        s.conflictWindow.clear();
        s.covSceneDraws = s.covExact = s.covGeneric = s.covInert = s.covUnchanged = 0;
        s.covLocalRefused = s.covMemoEvictions = 0;
        s.covFrames = s.covFramesObserving = s.covObservationEntries = s.covFramesLocallyRefused = 0;
        s.covRefusedPairsUsed = 0;
        s.lastReport = now;
    }
    reportMapBounce(frame,now,false);
    // The frame that starts now is watched unless it is a Paused one (flat_standdown.h).
    s.frameLive = s.work != FlatWork::Paused;
    g_flatRuntimeLive.store(true, std::memory_order_release);
}
// Why the game's state went back (engine_velocity.h counts it under this name).
static EngineVelocityFlushCause flushCauseOf(FlatSubstEvent event) {
    switch (event) {
    case FlatSubstEvent::kDispatch: return EngineVelocityFlushCause::kDispatch;
    case FlatSubstEvent::kClear: return EngineVelocityFlushCause::kClear;
    case FlatSubstEvent::kCopy: return EngineVelocityFlushCause::kCopy;
    case FlatSubstEvent::kResolve: return EngineVelocityFlushCause::kResolve;
    case FlatSubstEvent::kKeepTargets: return EngineVelocityFlushCause::kKeepTargets;
    case FlatSubstEvent::kExecuteCommandList: return EngineVelocityFlushCause::kCommandList;
    case FlatSubstEvent::kPresent: return EngineVelocityFlushCause::kPresent;
    default: return EngineVelocityFlushCause::kOtherDraw;
    }
}
void flatRuntimeSubstitution(ID3D11DeviceContext* ctx, FlatSubstEvent event) {
    if (overlaySuffixActive.load(std::memory_order_acquire) && owner() && overlayOpen(state()) &&
        (event == FlatSubstEvent::kExecuteCommandList || event == FlatSubstEvent::kClearState))
        overlayFail(state(), "overlay-unknown-write-or-command-list");
    // Nothing of engine motion's is bound over the game's: the common case, one load.
    if (!engineVelocityFlatPending()) return;
    if (!owner()) return;
    switch (flatSubstAction(event)) {
    case FlatSubstAction::kFlush:
        // Only the owner's immediate context carries state of ours; a deferred context's calls are its own. The work is
        // engine motion's draw wrapper's, and the census counts it there, not in whatever hook it came from.
        if (ctx && ctx == state().context.Get()) {
            flatcpu::Scope engine(flatcpu::kEngineDraw);
            engineVelocityFlatFlush(ctx, flushCauseOf(event));
        }
        return;
    case FlatSubstAction::kAbandon:
        engineVelocityFlatAbandon();
        return;
    default:
        return;
    }
}
void flatRuntimeViewport(UINT n, const D3D11_VIEWPORT* vp) {
    if (!owner()) return;
    flatcpu::Scope tracker(flatcpu::kTrackers);
    auto& s = state(); s.viewportCount = n; if (n == 1 && vp) s.viewport = *vp;
}
void flatRuntimeConstantBuffers(UINT start, UINT count, ID3D11Buffer* const* buffers) {
    if (owner() && start <= 1 && 1-start < count && buffers && buffers[1-start]) {
        flatcpu::Scope tracker(flatcpu::kTrackers);
        camera(buffers[1-start], true);
    }
}
void flatRuntimeClearBindings() {
    if (owner()) {
        if (state().foreground.active()) state().foreground.noteForeign();
        if (overlayOpen(state())) overlayFail(state(), "overlay-clear-state");
        flatcpu::Scope tracker(flatcpu::kTrackers);
        state().viewportCount = 0; for (auto& u : state().uavs) u.Reset();
        // ClearState took engine motion's bound state with the game's: nothing left to put back, and nothing safe to touch.
        flatRuntimeSubstitution(nullptr, FlatSubstEvent::kClearState);
    }
}
void flatRuntimeReplayQueryBegin(ID3D11DeviceContext* ctx, ID3D11Asynchronous* async) {
    if (ctx && ctx->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        const auto kind=replayQueryKind(async);
        auto& observer=replayQueryObserver();std::lock_guard<std::mutex> lock(observer.mutex);
        auto* entry=observer.find(ctx);if(!entry)return;
        entry->tracker.begin(async,kind);
        // Keep the object's identity alive for the whole bracket. A released
        // query's address could otherwise be reused by an unrelated query.
        if(async) for(size_t i=0;i<64;++i)
            if(entry->tracker.active[i].identity==async && !entry->holds[i]) {
                entry->holds[i]=async; break;
            }
    }
}
void flatRuntimeReplayQueryEnd(ID3D11DeviceContext* ctx, ID3D11Asynchronous* async) {
    if (ctx && ctx->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        const auto kind=replayQueryKind(async);
        auto& observer=replayQueryObserver();std::lock_guard<std::mutex> lock(observer.mutex);
        auto* entry=observer.find(ctx);if(!entry)return;
        size_t release=64;
        for(size_t i=0;i<64;++i)if(entry->tracker.active[i].identity==async) { release=i;break; }
        entry->tracker.end(async,kind);
        if(release<64)entry->holds[release].Reset();
    }
}
void flatRuntimeUnknown() {
    if (!owner()) return;
    flatcpu::Scope tracker(flatcpu::kTrackers);
    auto& s = state();
    if (s.foreground.active()) s.foreground.noteForeign();
    if (overlayOpen(s)) overlayFail(s, "overlay-unknown-context-state");
    // The trackers lose what they knew in every mode; a Paused frame watches nothing
    // else, so no trace mark and no prefix or shadow to invalidate.
    s.viewportCount = 0; for (auto& u : s.uavs) u.Reset();
    if (s.work == FlatWork::Paused) return;
    flatTraceMark(s.traceRing, kFlatTraceEventMarkUncertain, nullptr); s.prefix.uncertain = true;
    s.cameras.invalidateAll();
    if(s.projection) {s.projection->invalidateAll();failPhase(s,"unknown-context-state");}
}
void flatRuntimeOverlayUavBind(ID3D11DeviceContext* ctx, UINT count,
                               ID3D11UnorderedAccessView* const* uavs) {
    if(!overlaySuffixActive.load(std::memory_order_acquire) || !count || !uavs)return;
    for(UINT i=0;i<count;++i)if(uavs[i]) {
        if(!owner() || ctx!=state().context.Get())
            foreignWork.store(true,std::memory_order_release);
        else overlayFail(state(),"overlay-OM-UAV-bound");
        return;
    }
}
void flatRuntimeUavs(UINT start, UINT count, ID3D11UnorderedAccessView* const* views) {
    if (!owner() || state().work == FlatWork::Paused) return;
    flatcpu::Scope tracker(flatcpu::kTrackers);
    for (UINT i = 0; i < count && start + i < 8; ++i) {
        ResourceInfo info{};
        if (views && views[i]) bindingResolve(views[i], &info);
        state().uavs[start+i] = static_cast<ID3D11Resource*>(info.resource);
    }
}
FlatRuntimeDispatchScope::FlatRuntimeDispatchScope(ID3D11DeviceContext* ctx) {
    if(!flatRuntimeActive())return;
    auto& s = state(); if (!owner() || ctx != s.context.Get()) { foreignWork.store(true, std::memory_order_release); return; }
    // A dispatch is not a substituted producer draw: it sees the game's state (engine_velocity.h, the lazy form).
    flatRuntimeSubstitution(ctx, FlatSubstEvent::kDispatch);
    if (s.work == FlatWork::Paused) return;
    flatcpu::Scope shell(flatcpu::kOther);   // the dispatch scope's own time
    if(s.projection) {
        if(s.projectionFrames)++s.projectionDispatches;
        // CSSetShader records only the pointer via bindingSet, unlike the
        // VS/PS hash-caching setters. Resolve its registered hash here, only
        // while the projection path is active; qualification verifies the actual CS.
        const auto cs=lookupShaderHash(bindingGet(BindSlot::Cs));
        if(cs==0x5998146D464F5C0Eull || cs==0xEB0245DE0BB23BB6ull) {
            FlatComputeInternalScope internal;
            Ptr<ID3D11ShaderResourceView> depth,material;ctx->CSGetShaderResources(3,1,&depth);ctx->CSGetShaderResources(4,1,&material);
            Ptr<ID3D11Resource> depthResource,materialResource;Ptr<ID3D11Texture2D> texture;
            if(depth)depth->GetResource(&depthResource);if(material)material->GetResource(&materialResource);
            if(depthResource)depthResource.As(&texture);
            D3D11_TEXTURE2D_DESC desc{};if(texture)texture->GetDesc(&desc);
            const bool owned=materialResource && (materialResource.Get()==s.namedDepth || materialResource.Get()==s.phaseDepth.Get());
            if(const auto* plan=qualifyProjection(s,flatProjectionDispatchRecipes(cs,desc.Width,desc.Height),desc.Width,desc.Height,0,0,cs,owned)) {
                // Under Upstream ownership the compute path's private-row
                // mutation is suppressed too (the camera was jittered at the
                // source); the injector's noteApplied covers the accounting.
                if (!flatCameraInjectUpstreamOwns()) {
                    projection.emplace(*plan);
                    if(projection->active()) {s.phase.noteApplied();++s.jitterDispatches;}
                    else failPhase(s,"compute-binding-refused");
                }
                // Tripwire, structurally unreachable: a legacy application under Upstream.
                if (projection && projection->active() && flatCameraInjectUpstreamOwns()) ++s.rows.legacyAppliedUnderUpstream;
            }
        }
    }
    for (const auto& u : s.uavs) if (u) {
        if(s.engine!=FlatMonoResolveMode::Taa) {
            domainResourceWritten(s,u.Get(),"foreground-dispatch-depth-write",historyWholeWrite(HistoryWriteEntry::Dispatch));
        }
        { flatcpu::Scope trace(flatcpu::kTrace); flatTraceMark(s.traceRing, kFlatTraceEventDispatchWritten, u.Get()); }
        flatRuntimeDispatchObserveWritten(s.prefix, u.Get());
        if (overlayOpen(s)) for (uint32_t i=0; i<s.prefix.targetsUsed; ++i) {
            const auto& t=s.prefix.targets[i];
            if(t.overlayOpen && (t.resource==u.Get() || t.overlayDepth==u.Get())) {
                overlayFail(s,"overlay-dispatch-write",t.resource); break;
            }
        }
        flatHdrObserveDispatchWrite(s.hdr, u.Get());   // a dispatch into the HDR target after the trigger is counted
    }
    if(s.prefix.uncertain && s.projection)failPhase(s,"compute-source-invalidated");
}
// The per-call tees below return at once in a Paused frame (flat_standdown.h). A Probe
// frame runs them as a Full one does except for the camera witness, which is diagnostics
// for a frame that could be treated, and the projection shadows, which do not exist then.
// What a write to a resource does to the prefix model, the camera table and the shadows -- the
// body flatRuntimeWritten, Map and Update share, timed by the caller's scope.
static void resourceWritten(State& s, ID3D11Resource* res,const char* entry,
                            FlatOverlayMutationOp provenance=FlatOverlayMutationOp::Written,
                            const HistoryWriteExtent& extent=HistoryWriteExtent{}) {
    // The mutation report (flatRuntimeOverlayResourceMutation) counts the same API call for the history; this notification only invalidates.
    if(s.engine!=FlatMonoResolveMode::Taa)
        domainResourceWritten(s,res,"foreground-depth-or-unknown-mutation",extent,false);
    if(s.untrusted.active())s.untrusted.noteMutation(res,
        FlatMutationDetails::named(provenance,entry),s.prefix.sequence);
    if (overlayOpen(s)) for (uint32_t i=0; i<s.prefix.targetsUsed; ++i) {
        const auto& t=s.prefix.targets[i];
        if(t.overlayOpen && (t.resource==res || t.overlayDepth==res)) {
            overlayFail(s,"overlay-explicit-resource-write",t.resource); break;
        }
    }
    { flatcpu::Scope trace(flatcpu::kTrace); flatTraceMark(s.traceRing, kFlatTraceEventWriteResource, res); }
    flatRuntimeWritten(s.prefix, res);
    flatHdrObserveExplicitWrite(s.hdr, res);   // a write into the HDR target after the route's trigger is counted
    if (auto* c = camera(res, false)) s.cameras.invalidate(*c);
    if (s.projection) { flatcpu::Scope shadows(flatcpu::kShadows); s.projection->invalidate(res); }
}
// The extents the hooks pass (declared in flat_runtime.h; defined here, outside the anonymous namespace the mutation report is in).
HistoryWriteExtent flatRuntimeCopyExtent(UINT dstSub,UINT dstX,const void* src,const D3D11_BOX* box) {
    if(dstSub!=0)return historyWholeWrite(HistoryWriteEntry::CopyRegion);
    if(box) {
        if(box->right<=box->left)return historyWholeWrite(HistoryWriteEntry::CopyRegion);
        return historyRangedWrite(HistoryWriteEntry::CopyRegion,dstX,uint64_t(dstX)+(box->right-box->left));
    }
    const uint64_t bytes=flatBufferBytes(src);
    return bytes?historyRangedWrite(HistoryWriteEntry::CopyRegion,dstX,uint64_t(dstX)+bytes):historyWholeWrite(HistoryWriteEntry::CopyRegion);
}
HistoryWriteExtent flatRuntimeUpdateExtent(UINT dstSub,const D3D11_BOX* box) {
    if(dstSub!=0 || !box || box->right<=box->left)return historyWholeWrite(HistoryWriteEntry::Update);
    return historyRangedWrite(HistoryWriteEntry::Update,box->left,box->right);
}
void flatRuntimeWritten(ID3D11Resource* res,FlatOverlayMutationOp provenance) {
    if (!owner() || state().work == FlatWork::Paused) return;
    flatcpu::Scope lookup(flatcpu::kResource);   // prefix target and source lookup, camera lookup
    const bool clear=provenance==FlatOverlayMutationOp::ClearRtv || provenance==FlatOverlayMutationOp::ClearDsv ||
                     provenance==FlatOverlayMutationOp::ClearUav;
    resourceWritten(state(), res,"flatRuntimeWritten",provenance,historyWholeWrite(clear?HistoryWriteEntry::Clear:HistoryWriteEntry::Other));
}
void flatRuntimeWrittenExtent(ID3D11Resource* res,const HistoryWriteExtent& extent) {
    if (!owner() || state().work == FlatWork::Paused) return;
    flatcpu::Scope lookup(flatcpu::kResource);
    resourceWritten(state(), res,"flatRuntimeWritten",FlatOverlayMutationOp::Written,extent);
}
namespace {
const char* overlayMutationOpName(FlatOverlayMutationOp op) {
    switch (op) {
    case FlatOverlayMutationOp::Map: return "Map";
    case FlatOverlayMutationOp::Unmap: return "Unmap";
    case FlatOverlayMutationOp::ClearRtv: return "ClearRtv";
    case FlatOverlayMutationOp::ClearDsv: return "ClearDsv";
    case FlatOverlayMutationOp::ClearUav: return "ClearUav";
    case FlatOverlayMutationOp::GenerateMips: return "GenerateMips";
    case FlatOverlayMutationOp::CopyResource: return "CopyResource";
    case FlatOverlayMutationOp::CopyRegion: return "CopyRegion";
    case FlatOverlayMutationOp::CopyStructureCount: return "CopyStructureCount";
    case FlatOverlayMutationOp::UpdateSubresource: return "UpdateSubresource";
    case FlatOverlayMutationOp::Resolve: return "Resolve";
    case FlatOverlayMutationOp::Written: return "Written";
    }
    return "UnknownOp";
}
const char* overlayMutationRoleName(FlatOverlayMutationRole role) {
    switch (role) {
    case FlatOverlayMutationRole::Hdr: return "hdr";
    case FlatOverlayMutationRole::Depth: return "depth";
    case FlatOverlayMutationRole::Unknown: return "unknown";
    default: return "unrelated";
    }
}
}
void flatRuntimeOverlayResourceMutation(ID3D11Resource* resource, FlatOverlayMutationOp op,
                                       const FlatMutationDetails& details) {
    if(foregroundDomainActive.load(std::memory_order_acquire)) {
        if(!owner())foreignWork.store(true,std::memory_order_release);
        else {
            // The API-level report of every mutation: the history counts the write here, with the bytes the call carried.
            domainResourceWritten(state(),resource,"foreground-depth-or-unknown-mutation",mutationExtent(op,details),true);
        }
    }
    if(untrustedCoverageActive.load(std::memory_order_acquire)) {
        if(!owner())foreignWork.store(true,std::memory_order_release);
        else {
            auto payload=details;payload.op=op;
            if(std::strcmp(payload.entry,"unspecified-write")==0)payload.entry=flatMutationOpName(op);
            Ptr<ID3D11Resource> source;
            if(op==FlatOverlayMutationOp::CopyStructureCount && payload.sourceView) {
                FlatComputeInternalScope internal;
                static_cast<ID3D11UnorderedAccessView*>(const_cast<void*>(payload.sourceView))->GetResource(&source);
                payload.source=source.Get();payload.known|=FlatMutationDetails::Source;
            }
            state().untrusted.noteMutation(resource,payload,state().prefix.sequence);
        }
    }
    if(foregroundProbeActive.load(std::memory_order_acquire)) {
        if(!owner())foreignWork.store(true,std::memory_order_release);
        else if(resource)state().foreground.noteDepthMutation(resource);
        else state().foreground.noteForeign();
    }
    if (!overlaySuffixActive.load(std::memory_order_acquire)) return;
    if (!owner()) { foreignWork.store(true,std::memory_order_release); return; }
    auto& s=state();
    if (!overlayOpen(s)) return;
    FlatOverlayMutationRole role=resource ? FlatOverlayMutationRole::Unrelated : FlatOverlayMutationRole::Unknown;
    const void* protectedHdr=nullptr;
    if (resource) for (uint32_t i=0;i<s.prefix.targetsUsed;++i) {
        const auto& t=s.prefix.targets[i];
        if (!t.overlayOpen) continue;
        role=flatRuntimeOverlayMutationRole(resource,t.resource,t.overlayDepth);
        if (role!=FlatOverlayMutationRole::Unrelated) { protectedHdr=t.resource; break; }
    }
    if (role==FlatOverlayMutationRole::Unrelated) return;
    ++s.overlayMutationWindow[std::string("op=")+overlayMutationOpName(op)+" role="+overlayMutationRoleName(role)];
    overlayFail(s,role==FlatOverlayMutationRole::Unknown ?
        "overlay-unresolved-resource-write" : "overlay-explicit-resource-write",protectedHdr);
}
void flatRuntimeOverlayViewMutation(ID3D11View* view, FlatOverlayMutationOp op,
                                   const FlatMutationDetails& details) {
    if (!overlaySuffixActive.load(std::memory_order_acquire) &&
        !foregroundDomainActive.load(std::memory_order_acquire) &&
        !foregroundProbeActive.load(std::memory_order_acquire) &&
        !untrustedCoverageActive.load(std::memory_order_acquire)) return;
    if (!owner()) { foreignWork.store(true,std::memory_order_release); return; }
    Ptr<ID3D11Resource> resource;
    if (view) view->GetResource(&resource);
    auto payload=details;payload.view=view;payload.known|=FlatMutationDetails::View;
    flatRuntimeOverlayResourceMutation(resource.Get(),op,payload);
}
void flatRuntimeOverlayForeignMutation() {
    if (!overlaySuffixActive.load(std::memory_order_acquire) &&
        !foregroundDomainActive.load(std::memory_order_acquire) &&
        !foregroundProbeActive.load(std::memory_order_acquire) &&
        !untrustedCoverageActive.load(std::memory_order_acquire)) return;
    foreignWork.store(true,std::memory_order_release);
    if(owner() && foregroundDomainActive.load(std::memory_order_relaxed))
        for(auto& candidate:state().foregroundCandidates)
            if(candidate.frame==state().prefix.frame)candidate.motion.fail("foreground-foreign-mutation");
    if(owner() && untrustedCoverageActive.load(std::memory_order_relaxed))
        state().untrusted.invalidate("untrusted-foreign-mutation");
    if(owner() && foregroundProbeActive.load(std::memory_order_relaxed))state().foreground.noteForeign();
    if (owner() && overlayOpen(state())) overlayFail(state(),"overlay-foreign-mutation");
}
void flatRuntimeMapBouncePreMap(ID3D11Resource* resource) {
    mapBounce().preMap(reinterpret_cast<uintptr_t>(resource));
}
void flatRuntimeMapBounceNoteMap(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT sub,
                                 D3D11_MAP type, bool internal, bool success) {
    auto& w=mapBounceWindow();
    ++w.maps;
    if (internal) { ++w.internal; return; }
    if (!success || sub!=0) { ++w.failed; return; }
    // READ maps bypass flatRuntimeMap's write observer. This read-only lookup
    // recovers their tracked-CB map type without a descriptor or driver call.
    if (type==D3D11_MAP_READ && owner() && state().projection &&
        state().projection->containsTracked(resource))
        flatRuntimeMapBounceTrackedMap(type);
    if (type!=D3D11_MAP_WRITE_DISCARD) { ++w.notDiscard; return; }
    if (context!=state().context.Get()) { ++w.foreignContext; return; }
    if (!owner()) { ++w.foreignThread; return; }
    if (state().work!=FlatWork::Full || !state().projection) {
        ++w.paused; return;
    }
}
void* flatRuntimeMapBounceInstall(ID3D11DeviceContext* context, ID3D11Resource* resource,
                                  UINT sub, D3D11_MAP type, void* real) {
    auto& bounce=mapBounce();
    if (bounce.decision()!=flatmap::State::On || !flatRuntimeActive() || !real || sub!=0 ||
        type!=D3D11_MAP_WRITE_DISCARD || context!=state().context.Get() ||
        !owner() || state().work!=FlatWork::Full ||
        !state().projection) return real;
    const auto source=state().projection->bounceSource(resource);
    if (!source.eligible) {
        ++mapBounceWindow().untracked;
        return real;
    }
    flatmap::MapRequest request{};
    request.resource=reinterpret_cast<uintptr_t>(resource);
    request.context=reinterpret_cast<uintptr_t>(context);
    request.real=real; request.width=source.width;
    request.frame=mapBouncePresentEpoch.load(std::memory_order_acquire);
    request.discard=true; request.eligible=true; request.success=true;
    request.seed=source.seed; request.seedValid=source.seedValid;
    flatcpu::Scope shadows(flatcpu::kShadows);
    return bounce.install(request);
}
FlatMapBounce::Lease flatRuntimeMapBounceBeginUnmap(ID3D11DeviceContext* context,
                                                     ID3D11Resource* resource) {
    flatcpu::Scope shadows(flatcpu::kShadows);
    return mapBounce().beginUnmap(reinterpret_cast<uintptr_t>(resource),
                                  reinterpret_cast<uintptr_t>(context));
}
bool flatRuntimeMapBounceSamplePending(uint32_t width) {
    return width>=256 && mapBounceWindow().bankSamples.load()<32;
}
void flatRuntimeMapBounceObserveCopy(uint32_t width, uint64_t ticks) {
    mapBounce().observeCopy(width,ticks);
    auto& w=mapBounceWindow();
    w.bankBytes+=width; w.bankTicks+=ticks; ++w.bankSamples;
}
void flatRuntimeMapBounceTrackedMap(D3D11_MAP type) {
    auto& w=mapBounceWindow();
    const unsigned index=static_cast<unsigned>(type);
    if (index<6) ++w.trackedTypes[index];
}
void flatRuntimeMapBounceBankWrite(uint32_t width) {
    auto& w=mapBounceWindow();
    w.totalBankBytes+=width;
    const unsigned bucket=width<=64?0:width<=256?1:width<=1024?2:
        width<=4096?3:width<=8192?4:5;
    ++w.widthBuckets[bucket];
}
void flatRuntimeMapBounceRegistered(const D3D11_BUFFER_DESC& desc) {
    static unsigned lines=0;
    if (lines>=64) return;
    ++lines;
    Log::get().note("flat map bounce: tracked CB %u/64 width=%u usage=%u cpu-access=0x%x bind=0x%x",
        lines,desc.ByteWidth,static_cast<unsigned>(desc.Usage),
        desc.CPUAccessFlags,desc.BindFlags);
}
void flatRuntimeMapBounceNoteKind(ID3D11Resource* resource, bool buffer) {
    auto& w=mapBounceWindow();
    if (!buffer) { ++w.texture; return; }
    if (owner() && state().projection && state().projection->containsTracked(resource))
        ++w.tracked;
    else ++w.otherBuffer;
}
void flatRuntimeMap(ID3D11Resource* res, D3D11_MAP type, void* bytes) {
    if (!owner() || type == D3D11_MAP_READ || state().work == FlatWork::Paused) return;
    flatcpu::Scope lookup(flatcpu::kResource);
    resourceWritten(state(), res,"flatRuntimeMap",FlatOverlayMutationOp::Written,historyWholeWrite(HistoryWriteEntry::Map)); if (auto* c = camera(res, false)) state().cameras.setMapped(*c, bytes);
    if (state().projection) { flatcpu::Scope shadows(flatcpu::kShadows); state().projection->observeMap(res,type,bytes); }
}
void flatRuntimeUnmap(ID3D11Resource* res) {
    if (!owner() || state().work == FlatWork::Paused) return;
    flatcpu::Scope lookup(flatcpu::kResource);
    if (state().projection) { flatcpu::Scope shadows(flatcpu::kShadows); state().projection->observeUnmap(res); }
    if (auto* c = camera(res, false)) {
        if (c->mapped) { capture(*c, c->mapped); if (state().work == FlatWork::Full) cameraWitness(res); }
        state().cameras.setMapped(*c, nullptr);
    }
}
void flatRuntimeUpdate(ID3D11Resource* res, const void* bytes, const D3D11_BOX* box) {
    if (!owner() || state().work == FlatWork::Paused) return;
    flatcpu::Scope lookup(flatcpu::kResource);
    resourceWritten(state(), res,"flatRuntimeUpdate",FlatOverlayMutationOp::Written,flatRuntimeUpdateExtent(0,box));
    if (auto* c = camera(res, false)) {
        if (!box || (box->left == 0 && box->right == c->width)) { capture(*c, bytes); if (state().work == FlatWork::Full) cameraWitness(res); }
    }
    if (state().projection) { flatcpu::Scope shadows(flatcpu::kShadows); state().projection->observeUpdate(res,bytes,box); }
}

// The coverage classification's two questions to the context, asked of the binding shadow and of the context only on the
// sampled frames or once a shadow has been found wrong (flat_query_reads.h holds them; flat_query_cut.h says why).
static const void* coverageDepthResource(ID3D11DeviceContext* ctx, const FlatContractObservation& k, Ptr<ID3D11Resource>& hold) {
    return flatQueryDepth(flatQueryCut(), ctx, k.depth, hold, [](const char* line) { Log::get().note("%s", line); });
}
static bool coverageShadersMatch(ID3D11DeviceContext* context, const FlatContractObservation& k) {
    FlatComputeInternalScope guard;
    return flatQueryShaders(flatQueryCut(), context, k.vs, k.ps, [](void* shader) { return lookupShaderHash(shader); },
                            [&] { flatRuntimeSubstitution(context, FlatSubstEvent::kOtherDraw); },
                            [](const char* line) { Log::get().note("%s", line); });
}

// Only a refused, structurally pure inert shader reaches this expensive
// original-state check. A no-write draw still runs normally (including its
// stencil operation); it simply cannot alter the color/depth owner plane.
static const char* domainOriginalInertRefusal(ID3D11DeviceContext* context,
    const FlatContractObservation& k,const FlatDomainShaderProof& proof) {
    flatRuntimeSubstitution(context,FlatSubstEvent::kOtherDraw);
    FlatComputeInternalScope guard;
    FlatDomainInertBindings b{};
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;
    UINT vsClasses=0,psClasses=0;
    context->VSGetShader(&vs,nullptr,&vsClasses);
    context->PSGetShader(&ps,nullptr,&psClasses);
    b.actualShaderPair=vs && ps && !vsClasses && !psClasses &&
        vs.Get()==bindingGet(BindSlot::Vs) && ps.Get()==bindingGet(BindSlot::Ps) &&
        lookupShaderHash(vs.Get())==k.vs && lookupShaderHash(ps.Get())==k.ps;
    Ptr<ID3D11GeometryShader> gs;Ptr<ID3D11HullShader> hs;Ptr<ID3D11DomainShader> ds;
    UINT gsClasses=0,hsClasses=0,dsClasses=0;
    context->GSGetShader(&gs,nullptr,&gsClasses);
    context->HSGetShader(&hs,nullptr,&hsClasses);
    context->DSGetShader(&ds,nullptr,&dsClasses);
    b.noOtherStages=!gs && !hs && !ds && !gsClasses && !hsClasses && !dsClasses;
    Ptr<ID3D11Device> device;context->GetDevice(&device);
    if(!device)return "foreground-inert-device-unavailable";
    const UINT uavCount=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?
        D3D11_1_UAV_SLOT_COUNT:D3D11_PS_CS_UAV_REGISTER_COUNT;
    ID3D11RenderTargetView* rawTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11UnorderedAccessView* rawUavs[D3D11_1_UAV_SLOT_COUNT]{};
    Ptr<ID3D11DepthStencilView> dsv;
    context->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
        rawTargets,&dsv,0,uavCount,rawUavs);
    for(unsigned i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i) {
        if(rawTargets[i]){b.boundTargets|=1u<<i;rawTargets[i]->Release();}
    }
    b.noUavs=true;
    for(unsigned i=0;i<uavCount;++i)if(rawUavs[i]) {b.noUavs=false;rawUavs[i]->Release();}
    b.originalDsv=dsv && dsv.Get()==k.dsv;
    if(b.originalDsv) {
        Ptr<ID3D11Resource> depth;dsv->GetResource(&depth);
        b.originalDsv=depth.Get()==k.depth;
        D3D11_DEPTH_STENCIL_VIEW_DESC view{};dsv->GetDesc(&view);
        b.readOnlyDepth=(view.Flags&D3D11_DSV_READ_ONLY_DEPTH)!=0;
    }
    Ptr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT sampleMask=0;
    context->OMGetBlendState(&blend,factors,&sampleMask);
    D3D11_BLEND_DESC blendDesc{};
    if(blend){blend->GetDesc(&blendDesc);b.blend=&blendDesc;}
    Ptr<ID3D11DepthStencilState> depthState;UINT stencilRef=0;
    context->OMGetDepthStencilState(&depthState,&stencilRef);
    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    if(depthState){depthState->GetDesc(&depthDesc);b.depth=&depthDesc;}
    ID3D11Buffer* streams[D3D11_SO_BUFFER_SLOT_COUNT]{};
    context->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT,streams);
    b.noStreamOutput=true;
    for(auto* stream:streams)if(stream){b.noStreamOutput=false;stream->Release();}
    Ptr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;
    context->GetPredication(&predicate,&predicateValue);
    b.noPredicate=!predicate;
    return flatDomainInertRefusal(proof,b);
}
// Asked only of a draw the planner would otherwise refuse. The game's state
// is restored first, as for the inert query; a null state writes depth.
static bool domainDrawPreservesSurface(ID3D11DeviceContext* context) {
    flatRuntimeSubstitution(context,FlatSubstEvent::kOtherDraw);
    FlatComputeInternalScope guard;
    Ptr<ID3D11DepthStencilState> depthState;UINT stencilRef=0;
    context->OMGetDepthStencilState(&depthState,&stencilRef);
    D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=TRUE;depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    if(depthState)depthState->GetDesc(&depth);
    return flatDomainPreservesSurface(depth);
}
// The foreground (first-person) contract of a mixed-camera frame (design section 104): an SDK frame is handed the qualified
// first-person map built from the domain's captured draws, or the resolver refuses it. One block for both routes. The HDR route runs it
// at its trigger (treatHdr); the copy route runs it at the game's final copy, for the render-below-output frames its weapon support
// admits (flatWeaponRoute == Copy, flat_copy_structure.h). The map is built from the depth, the owner plane and the captured geometry
// as they are at the call, so a depth write between H and the copy takes those pixels' ownership (tools\weapon_motion_test pins it)
// and the map the SDK is handed is the picture's. `foregroundOutput` outlives the resolve: the frame's map view is borrowed from it.
// Sets f.foregroundRequired and, when that is true, the map, its qualification, frame, reset request and depth convention.
static void foregroundContractAtH(State& s,ID3D11DeviceContext* ctx,const FlatMonoFrame& selected,FlatMonoResolveFrame& f,
                                  FlatForegroundMotion::Output& foregroundOutput) {
    f.foregroundRequired=f.mode!=FlatMonoResolveMode::Taa && selected.mixedCamera;
    if(!f.foregroundRequired)return;
    FlatComputeInternalScope internal;
    s.foregroundSelectedDepth=selected.depth;
    s.foregroundRoute.pin(selected.depth);
    auto* candidate=domainCandidate(s,selected.depth);
    FlatContractObservation failureKey{};failureKey.vs=s.drawVs;failureKey.ps=s.drawPs;
    failureKey.depth=selected.depth;failureKey.format=26;
    failureKey.cameraHash=flatDomainBytecodeHash(selected.camera,sizeof(selected.camera));
    auto failH=[&](const char* why){domainFail(s,"H-qualification",why,failureKey);if(!s.foregroundHRefusal)s.foregroundHRefusal=why;
        if(why)s.foregroundHRefusalWindow=why;};
    ++s.foregroundCounts.hAttempts;
    if(!f.hdr)++s.copyWeaponWindow.hAttempts;   // the copy route's own count (the HDR route's frames carry f.hdr)
    s.foregroundLastH={s.prefix.frame,selected.depth,s.foregroundRoute.count(s.prefix.frame),
        candidate?candidate->pendingNull.count():0,
        s.foregroundRoute.overflowed(s.prefix.frame),candidate!=nullptr};
    if(!candidate)failH("foreground-selected-depth-unobserved-or-overflow");
    if(candidate && !candidate->pendingNull.matches(s.prefix.frame,selected.depth,f.renderWidth,f.renderHeight,
        selected.camera,s.phase.currentX,s.phase.currentY)) {
        failH("foreground-pending-null-not-selected-world");
        // Which witness and what differs (section 104: aiming down sights refused H here). The first 12 such frames of a session.
        if(s.pendingNullMismatchLogged<12) {
            char text[768];
            if(candidate->pendingNull.describeMismatch(s.prefix.frame,selected.depth,f.renderWidth,f.renderHeight,selected.camera,
                                                       s.phase.currentX,s.phase.currentY,text,sizeof(text))) {
                ++s.pendingNullMismatchLogged;
                Log::get().note("flat foreground pending-null mismatch %u/12: frame=%llu %s",s.pendingNullMismatchLogged,
                    (unsigned long long)s.prefix.frame,text);
            }
        }
    }
    if(candidate && candidate->hdr && candidate->hdr.Get()!=selected.hdr)
        failH("foreground-provisional-HDR-not-selected");
    if(foreignWork.load(std::memory_order_acquire) || s.prefix.uncertain)
        failH("foreground-uncertain-frame");
    if(s.foregroundRoute.unknownMutation(s.prefix.frame))
        failH("foreground-prior-unknown-mutation");
    Ptr<ID3D11ShaderResourceView> domainOwners;
    if(!engineVelocityFlatDomainSlots(static_cast<ID3D11Texture2D*>(const_cast<void*>(selected.depth)),&domainOwners))
        failH("foreground-current-owner-plane-unavailable");
    if(candidate)candidate->motion.prepareH(ctx,domainOwners.Get(),s.depthView.Get(),selected.camera,
        static_cast<unsigned>(s.prefix.frame),f.renderWidth,f.renderHeight,foregroundOutput);
    if(candidate && !foregroundOutput.qualified)failH(foregroundOutput.refusal);
    if(candidate && candidate->firstFailure.frame==s.prefix.frame)
        s.foregroundFirstFailure=candidate->firstFailure;
    if(candidate && candidate->failureKinds.frame==s.prefix.frame)
        s.foregroundFailureKinds=candidate->failureKinds;
    if(candidate && candidate->coveredKinds.frame==s.prefix.frame)
        s.foregroundCoveredKinds=candidate->coveredKinds;
    if(candidate && candidate->firstCovered.frame==s.prefix.frame)
        s.foregroundFirstCovered=candidate->firstCovered;
    f.foregroundMotion=foregroundOutput.motion.Get();f.foregroundQualified=foregroundOutput.qualified;
    f.foregroundResetRequired=foregroundOutput.resetRequired;f.foregroundFrame=foregroundOutput.frame;
    f.foregroundDepthNear=foregroundOutput.depthNear;
    if(foregroundOutput.qualified) {
        ++s.foregroundCounts.hQualified;
        if(!f.hdr)++s.copyWeaponWindow.hQualified;
        // A qualified frame that holds a covered draw is one the whole-frame refusal used to lose.
        if(foregroundOutput.coveredDraws)++s.foregroundCounts.hCoveredFrames;
    }
}

FlatRuntimeDrawScope::FlatRuntimeDrawScope(ID3D11DeviceContext* context, uint32_t instances,
                                            char kind, uint32_t count, uint32_t start,
                                            int32_t base, uint32_t startInstance) {
    // Manual packet evidence remains useful with AA off or a stood-down
    // frame. This path never prepares temporal resources or substitutes state.
    if(!g_flatComputeInternal && runtimeFlatProfile() && state().drawPackets.armed() &&
       (!flatRuntimeActive() || state().work==FlatWork::Paused) &&
       context==state().drawPacketContext.Get() && GetCurrentThreadId()==state().drawPacketThread) {
        auto& s=state();FlatComputeInternalScope internal;drawPacketOnly=true;ctx=context;s.drawPacketOnlyObserved=true;
        weaponDrawKind=kind;weaponDrawCount=count;weaponDrawStart=start;weaponDrawBase=base;
        weaponDrawInstances=instances;weaponDrawStartInstance=startInstance;
        Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;Ptr<ID3D11RenderTargetView> rtv;Ptr<ID3D11DepthStencilView> dsv;
        context->VSGetShader(&vs,nullptr,nullptr);context->PSGetShader(&ps,nullptr,nullptr);context->OMGetRenderTargets(1,&rtv,&dsv);
        drawPacketVs=lookupShaderHash(vs.Get());drawPacketPs=lookupShaderHash(ps.Get());const auto pair=std::make_pair(drawPacketVs,drawPacketPs);
        const auto known=s.drawPacketRefusedPairs.find(pair);const bool priority=known!=s.drawPacketRefusedPairs.end();
        Ptr<ID3D11Resource> color,depth;if(rtv)rtv->GetResource(&color);if(dsv)dsv->GetResource(&depth);
        Ptr<ID3D11DepthStencilState> stateDepth;UINT stencilRef=0;context->OMGetDepthStencilState(&stateDepth,&stencilRef);D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;if(stateDepth)stateDepth->GetDesc(&dd);
        const unsigned representative=flat_mono_detail::toneHdrSlot(pair.first,pair.second)!=~0u?3:color.Get()==s.drawPacketOutput.Get()?4:dsv&&dd.DepthEnable&&dd.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL?1:2;
        const UINT q=++s.drawPacketSequence;FlatDrawPacketCapture::Args args{kind,count,
            kind=='D'||kind=='N'?UINT(base):start,instances,startInstance,kind=='D'||kind=='N'?0:base};
        drawPacket=s.drawPackets.before(context,s.drawPacketFrame,q,pair.first,pair.second,args,priority,replayQueriesSafe(context),representative);
        if(drawPacket){s.drawPackets.missing(drawPacket,"chronology.resource_mutations","unsupported-AA-off-or-paused-mutation-observation");
            Log::get().note("flat draw packets: selected frame=%llu q=%u VS=%016llX PS=%016llX capture-only=1 priority=%u category=%u",
                (unsigned long long)s.drawPacketFrame,q,(unsigned long long)pair.first,(unsigned long long)pair.second,priority?1u:0u,representative);}
        FlatRuntimeDraw d{};d.key.vs=pair.first;d.key.ps=pair.second;d.key.color=color.Get();d.key.depth=depth.Get();d.key.rtv=rtv.Get();d.key.dsv=dsv.Get();d.instances=instances;
        flatTraceRecord(s.traceRing,d,false,nullptr,q);return;
    }
    auto& ingress = drawIngressAudit;
    const bool auditing = ingress.active.load(std::memory_order_relaxed);
    if (auditing) ingress.entered.fetch_add(1, std::memory_order_relaxed);
    if (!flatRuntimeActive()) {
        if (auditing) {
            ingress.inactive.fetch_add(1, std::memory_order_relaxed);
            const bool live = g_flatRuntimeLive.load(std::memory_order_relaxed);
            const bool internal = g_flatComputeInternal;
            if (!live) ingress.inactiveLiveOff.fetch_add(1, std::memory_order_relaxed);
            if (internal) ingress.inactiveInternal.fetch_add(1, std::memory_order_relaxed);
            drawIngressIdentity(context, "inactive", 0, nullptr, 0, 0, live, internal,
                                ingress.inactiveWitnesses, 1);
        }
        return;
    }
    auto& s = state();
    if (!owner()) {
        if (auditing) {
            ingress.wrongThread.fetch_add(1, std::memory_order_relaxed);
            // Present may resize and release its context on another thread.
            // Query only the live draw argument here, not State's COM pointer.
            drawIngressIdentity(context, "wrong-thread", s.thread, nullptr, 0, 0, true, false,
                                ingress.wrongThreadWitnesses, 1);
        }
        foreignWork.store(true, std::memory_order_release);
        return;
    }
    if (context != s.context.Get()) {
        if (auditing) {
            ingress.wrongContext.fetch_add(1, std::memory_order_relaxed);
            drawIngressIdentity(context, "wrong-context", s.thread, s.context.Get(), s.prefix.frame,
                                static_cast<uint32_t>(s.work), true, false,
                                ingress.wrongContextWitnesses, 2);
        }
        foreignWork.store(true, std::memory_order_release);
        return;
    }
    // The census's whole-frame GPU span opens at the frame's first game draw, watched or not.
    if (!s.gpuFrameTried) gpuFrameOpen(s, context);
    // A Paused frame (flat_standdown.h) watches nothing: the scope is a no-op, ctx stays
    // null and the destructor returns at its first line.
    if (s.work == FlatWork::Paused && auditing)
        ingress.paused.fetch_add(1, std::memory_order_relaxed);
    if (s.work == FlatWork::Paused) return;
    if (auditing) ingress.accepted.fetch_add(1, std::memory_order_relaxed);
    flatcpu::Scope shell(flatcpu::kOther);   // the scope's own time; the named families below are carved out of it
    ctx = context;
    drawPacketJitterBefore=s.jitterRefusals;drawPacketOverlayBefore=s.overlayRefusedWindow;
    weaponDrawKind=kind;weaponDrawCount=count;weaponDrawStart=start;weaponDrawBase=base;
    weaponDrawInstances=instances;weaponDrawStartInstance=startInstance;
    if(s.engine!=FlatMonoResolveMode::Taa && s.work==FlatWork::Full && s.foregroundDomainFrame!=s.prefix.frame) {
        s.foregroundDomainFrame=s.prefix.frame;s.foregroundSelectedDepth=nullptr;
        s.foregroundHRefusal=nullptr;
        s.foregroundRoute.beginFrame(s.prefix.frame);
        foregroundDomainActive.store(true,std::memory_order_release);
    }
    FlatRuntimeDraw d{}; auto& k = d.key;
    // Lazy substitution (engine_velocity.h): engine motion's state may still be bound from the producer draw before this
    // one, and stays bound only while nothing that could see it runs. A diagnostic capture reads the context, so with
    // one armed the game's state goes back at once and every producer draw restores after itself, as it always did.
    {
        const bool diagnostics = flatTemporalCapturing() || s.drawCapture.active() || s.drawPackets.armed();
        engineVelocityFlatLazy(!diagnostics);
        if (diagnostics) flatRuntimeSubstitution(context, FlatSubstEvent::kOtherDraw);
    }
    const FlatProjectionBindingPlan* projectionPlan=nullptr;
    const auto rt = view(BindSlot::Rtv0, 0), ds = view(BindSlot::Dsv0, 1);
    k.color = rt.resource; k.rtv = bindingGet(BindSlot::Rtv0); k.width = rt.a; k.height = rt.b; k.format = rt.fmt;
    k.depth = ds.resource; k.dsv = bindingGet(BindSlot::Dsv0); k.depthWidth = ds.a; k.depthHeight = ds.b; k.depthFormat = ds.fmt;
    k.vs = bindingShaderHash(BindSlot::Vs); k.ps = bindingShaderHash(BindSlot::Ps);
    drawPacketVs=k.vs;drawPacketPs=k.ps;
    drawPacketJitterBefore=s.jitterRefusals;drawPacketOverlayBefore=s.overlayRefusedWindow;
    if (auditing) {
        const bool relevant = (k.vs == flat_mono_detail::kCopyVs &&
                               k.ps == flat_mono_detail::kCopyPs) ||
                              (s.prefix.output && k.color == s.prefix.output);
        if (relevant) ingress.relevant.fetch_add(1, std::memory_order_relaxed);
        const bool firstAccepted = ingress.firstAcceptedWitness.load(std::memory_order_relaxed) == 0 &&
            ingress.firstAcceptedWitness.exchange(1, std::memory_order_relaxed) == 0;
        const bool sampleRelevant = relevant &&
            ingress.relevantWitnesses.fetch_add(1, std::memory_order_relaxed) < 3;
        if (firstAccepted || sampleRelevant) {
            FlatComputeInternalScope guard;
            Ptr<ID3D11VertexShader> actualVs;
            Ptr<ID3D11PixelShader> actualPs;
            Ptr<ID3D11RenderTargetView> actualRtv;
            Ptr<ID3D11DepthStencilView> actualDsv;
            Ptr<ID3D11Resource> actualColor;
            Ptr<IUnknown> actualColorIdentity, outputIdentity;
            D3D11_VIEWPORT actualVp{};
            UINT actualVpCount = 1;
            context->VSGetShader(&actualVs, nullptr, nullptr);
            context->PSGetShader(&actualPs, nullptr, nullptr);
            context->OMGetRenderTargets(1, &actualRtv, &actualDsv);
            if (actualRtv) actualRtv->GetResource(&actualColor);
            if (actualColor) actualColor->QueryInterface(IID_PPV_ARGS(&actualColorIdentity));
            if (s.output) s.output->QueryInterface(IID_PPV_ARGS(&outputIdentity));
            context->RSGetViewports(&actualVpCount, &actualVp);
            Log::get().note("flat draw ingress binding witness: frame=%llu kind=%s cached-VS=%016llX cached-PS=%016llX cached-RTV=%p cached-color=%p output=%p cached-viewport-count=%u cached-viewport=(%.1f,%.1f,%.1f,%.1f,%.2f,%.2f) actual-VS=%016llX actual-PS=%016llX actual-RTV=%p actual-color=%p actual-color-IUnknown=%p output-IUnknown=%p actual-DSV=%p actual-viewport-count=%u actual-viewport=(%.1f,%.1f,%.1f,%.1f,%.2f,%.2f); F10 bounded",
                (unsigned long long)s.prefix.frame, relevant ? "relevant" : "first-accepted",
                (unsigned long long)k.vs, (unsigned long long)k.ps, k.rtv, k.color,
                s.prefix.output, s.viewportCount,
                s.viewport.TopLeftX, s.viewport.TopLeftY, s.viewport.Width, s.viewport.Height,
                s.viewport.MinDepth, s.viewport.MaxDepth,
                (unsigned long long)lookupShaderHash(actualVs.Get()),
                (unsigned long long)lookupShaderHash(actualPs.Get()),
                actualRtv.Get(), actualColor.Get(), actualColorIdentity.Get(), outputIdentity.Get(),
                actualDsv.Get(), actualVpCount,
                actualVp.TopLeftX, actualVp.TopLeftY, actualVp.Width, actualVp.Height,
                actualVp.MinDepth, actualVp.MaxDepth);
        }
    }
    // Partial temporal AA's current-draw identities (see refuseDraw): the
    // pair a per-draw reason refuses, named in the coverage census. Cheap
    // POD stores, done for every draw so qualifyProjection (State& only) can
    // reach them too.
    s.drawVs = k.vs; s.drawPs = k.ps;
    k.b1 = bindingGet(BindSlot::VsCb1); k.viewportCount = s.viewportCount;
    static_assert(sizeof(k.viewport) == sizeof(D3D11_VIEWPORT), "viewport layout"); std::memcpy(k.viewport, &s.viewport, sizeof(k.viewport));
    {
        // The camera rows the buffer bound at b1 holds for this frame. The table keeps its last answer and serves
        // it to every draw until the binding, the frame or the table changes (flat_camera_table.h: on foot about
        // 5,000 draws a frame ask, about a hundred writes and rebinds change the answer), so the lookup, the
        // copy and the hash are timed as this family only when they are made afresh. The record it fills is the
        // one the search always produced.
        const uint32_t b1Binding = bindingGeneration(BindSlot::VsCb1);
        const FlatCameraRows* kept = s.cameras.probe(k.b1, b1Binding, s.prefix.frame);
        if (!kept) {
            flatcpu::Scope rows(flatcpu::kCameraRows);   // camera table lookup, rows copy and hash: a fresh lookup
            kept = &s.cameras.refresh(k.b1, b1Binding, s.prefix.frame);
        }
        if (kept->have) { std::memcpy(d.camera, kept->rows, sizeof(d.camera)); k.camera = d.camera; k.cameraHash = kept->hash; d.cameraHashTrusted = true; k.writeEpoch = kept->epoch; k.writeSeq = kept->sequence; }
    }
    if(s.drawPackets.armed()) {
        const auto pair=std::make_pair(k.vs,k.ps);const auto refused=s.drawPacketRefusedPairs.find(pair);
        const bool priority=refused!=s.drawPacketRefusedPairs.end() && s.prefix.frame-refused->second<=900;
        drawPacketPriority=priority;
        const bool tone=flat_mono_detail::toneHdrSlot(k.vs,k.ps)!=~0u;
        const bool output=k.color==s.prefix.output;
        Ptr<ID3D11DepthStencilState> stateDepth;UINT stencilRef=0;ctx->OMGetDepthStencilState(&stateDepth,&stencilRef);D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;if(stateDepth)stateDepth->GetDesc(&dd);
        const unsigned representative=tone?3:output?4:k.depth&&dd.DepthEnable&&dd.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL?1:2;
        {
            FlatComputeInternalScope internal;
            FlatDrawPacketCapture::Args args{kind,count,kind=='D'||kind=='N'?UINT(base):start,
                instances,startInstance,kind=='D'||kind=='N'?0:base};
            drawPacket=s.drawPackets.before(ctx,s.prefix.frame,s.prefix.sequence+1,k.vs,k.ps,args,priority,replayQueriesSafe(ctx),representative);
            if(drawPacket)Log::get().note("flat draw packets: selected frame=%llu q=%u VS=%016llX PS=%016llX priority=%u representative-category=%u; original bindings before substitution",
                (unsigned long long)s.prefix.frame,s.prefix.sequence+1,(unsigned long long)k.vs,(unsigned long long)k.ps,priority?1u:0u,representative);
        }
    }
    d.supported = engineVelocityPoolFamilyPair(k.vs, k.ps); d.instances = instances;
    // A pool family's vertex shader left stock (an unkeyed pixel shader), drawn into the scene's depth: it moves with no motion source, so a
    // scene holding one is never source-free (design section 104; the selector's unsupportedFamilyDraws).
    d.poolFamilyVs = !d.supported && k.depth && engineVelocityPoolFamilyVs(k.vs) &&
        flatContractKind(false, k.depth, k.depth, k.depthWidth, k.depthHeight, 26, s.prefix.width, s.prefix.height, false) == kFlatContractScreen;
    // A draw that is not a pool-family draw cannot be a substituted producer: it sees the game's state, and so does
    // everything EDVR reads of the context for it below.
    if (!d.supported) flatRuntimeSubstitution(context, FlatSubstEvent::kOtherDraw);
    k.kind = flatContractKind(d.supported, k.color, k.depth, k.width, k.height, k.format, s.prefix.width, s.prefix.height, k.color == s.prefix.output);
    const bool tone = flat_mono_detail::toneHdrSlot(k.vs, k.ps) != ~0u;
    const bool copy = k.vs == flat_mono_detail::kCopyVs && k.ps == flat_mono_detail::kCopyPs && k.color == s.prefix.output;
    if(!copy && s.drawCapture.active()) {
        FlatComputeInternalScope guard;
        // This repeats the later source predicate only while F10 is armed, so
        // admission can reserve samples for late motion producers before the
        // private MRT6 substitution. The later predicate remains authoritative.
        const bool motionEligible=d.supported && k.camera && k.depth &&
            flatContractKind(false,k.color,k.depth,k.width,k.height,k.format==9?26:k.format,
                s.prefix.width,s.prefix.height,false)==kFlatContractScreen &&
            (k.format==23||k.format==26)&&flat_mono_detail::fullViewport(k,k.width,k.height);
        drawCaptureStarted=s.drawCapture.before(ctx,instances,kind,count,start,base,startInstance,
            k.vs,k.ps,bindingGet(BindSlot::Vs),bindingGet(BindSlot::Ps),motionEligible);
    }
    if (tone || copy) for (uint32_t slot = 0; slot < 2; ++slot) {
        const auto bind = static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + slot);
        k.srvView[slot] = bindingGet(bind); k.srvResource[slot] = view(bind, 2 + slot).resource;
    }
    if (foreignWork.load(std::memory_order_acquire)) {
        s.prefix.uncertain = true;
        if(overlayOpen(s)) overlayFail(s,"overlay-foreign-work");
    }
    {
        // The exact-shader verifications: each returns at once unless this draw is the copy it
        // names, and then reads the pipeline back from the context (shader, targets, view, viewport).
        flatcpu::Scope checks(flatcpu::kCopyChecks);
        d.hdrCopyVerified=verifyHdrCopy(ctx,d);
        d.menuHdrCopyVerified=verifyMenuHdrCopy(ctx,d);
        d.imageSourceCameraIndependentVerified=verifyCameraIndependentImageSource(ctx,d);
        captureCopyProvenance(s,ctx,d);
    }
    const auto oldTargets = s.prefix.targetsUsed;
    const uint32_t oldImageAccepted=s.prefix.imageCopiesAccepted,oldImageRefused=s.prefix.imageCopiesRefused;
    const uint32_t oldMenuAccepted=s.prefix.menuCopiesAccepted,oldMenuRefused=s.prefix.menuCopiesRefused;
    const uint32_t cameraProbeAttempt=[&] {
        flatcpu::Scope checks(flatcpu::kCopyChecks);   // F10-only unless it is the camera-conflict draw
        return captureCameraConflict(s,d);
    }();
    if(s.weaponFootprint.active() && k.vs==0x025B4B9FF54622EDull &&
       k.ps==0x46F92DC71BF8DFA5ull) {
        const FlatRuntimeTarget* target=nullptr;
        for(uint32_t i=0;i<s.prefix.targetsUsed;++i)
            if(s.prefix.targets[i].resource==k.color){target=&s.prefix.targets[i];break;}
        const bool conflict=k.format==26&&k.camera&&target&&target->hdrCamera&&
            std::memcmp(target->tone.camera,d.camera,kFlatCameraBytes)!=0;
        weaponFootprintSeq=s.prefix.sequence+1;
        weaponFootprintStarted=s.weaponFootprint.before(ctx,s.prefix.frame,weaponFootprintSeq,
            k.vs,k.ps,conflict,k.cameraHash,d.camera,
            target?target->tone.key.cameraHash:0,target?target->tone.camera:nullptr);
    }
    // The world camera, predicted before it is named: ONE decision per draw (flatDomainPredictsWorld: the last named world's near plane
    // AND its projection scale), taken lazily and shared by the cohort flag below and by the domain's classification further down. A
    // near plane alone took the weapon's own depth prepass for the world while aiming down sights (design section 104).
    FlatDomainWorldPrediction worldPrediction{};bool worldPredictionKnown=false;
    const auto worldPredicted=[&]()->const FlatDomainWorldPrediction& {
        if(!worldPredictionKnown) {
            worldPredictionKnown=true;
            if(k.camera) {float rows[6][4];std::memcpy(rows,d.camera,sizeof(rows));flatDomainPredictsWorld(rows,s.worldReference,&worldPrediction);}
        }
        return worldPrediction;
    };
    // Where the weapon's two passes are judged (flat_copy_structure.h, flatWeaponRoute; design section 104). The HDR route's frame
    // keeps what it had: a protected overlay for the glow pass below, and the cohort's own qualification at H. Where the copy route
    // judges the frame (DLSS or FSR, and the HDR route will not treat it) the cohort is flagged here, before the model sees the draw,
    // and the glow pass is admitted as an alternate HDR writer instead of planned as an overlay; both ride the trace.
    bool weaponRouteKnown=false,weaponCopyRoute=false;
    const auto copyWeapon=[&]() {
        if(!weaponRouteKnown) {
            weaponRouteKnown=true;
            weaponCopyRoute=flatWeaponRoute(s.hdrKey==FlatHdrKey::Auto,s.hdrLatch.tripped,s.engine,k.width,k.height,
                s.prefix.width,s.prefix.height)==FlatWeaponRoute::Copy;
        }
        return weaponCopyRoute;
    };
    if(d.supported && k.camera && k.depth && k.kind==kFlatContractPool && k.format==23 && weaponMotionFamilyVs(k.vs) &&
       flat_mono_detail::fullViewport(k,k.width,k.height) &&
       flatContractKind(false,k.color,k.depth,k.width,k.height,k.format,s.prefix.width,s.prefix.height,false)==kFlatContractScreen &&
       copyWeapon()) {
        // The first-person camera, not the world's: the world camera is the named one once H has named it, and the last named world's
        // (near plane and projection scale: the domain's own prediction, below) before that, so a weapon-family draw under the world's
        // camera stays a world source.
        const bool predictedWorld=!s.namedDepth && worldPredicted().predicted;
        const bool worldCamera=s.namedDepth && std::memcmp(s.namedCamera,d.camera,sizeof(d.camera))==0;
        d.firstPersonCohort=!predictedWorld && !worldCamera;
        if(d.firstPersonCohort)++s.copyWeaponWindow.cohortDraws;
    }
    // Only a same-pose, same-raster-phase, camera-conflicting HDR draw may
    // enter the protected late-colour suffix. All names below are the current
    // draw's uploaded camera and live binding shadow, never a guessed weapon
    // shader list. The model gets a provisional mark; the actual private MRT
    // binding is verified immediately around the original draw below. Where the
    // copy route judges the frame the same tests admit the draw as an alternate
    // HDR writer instead (d.alternateHdr), with no overlay: the key's value
    // never bars it there, because the HDR route is not the one that treats it.
    // Section 104, the pool-less view: no draw has named the world (a view of ground and sky holds no pool-family draw), and nothing the
    // model saw so far is a source, a first-person cohort draw or a pool-family draw left stock. The HDR target's own first camera is then the
    // world's, and the glow pass is judged against it with the depth the target was drawn with (the search below), as a pool-bearing frame's
    // is against the named one. Without this the pass was a second camera in the HDR, and every frame of such a view was refused as
    // conflicting-hdr-target-or-camera.
    const bool unnamedWorldDepth=!s.namedDepth && s.prefix.sourcesUsed==0 && s.prefix.firstPersonDraws==0 &&
        s.prefix.unsupportedFamilyDraws==0;
    const FlatRuntimeTarget* overlayTarget=nullptr;
    if(s.work==FlatWork::Full && (s.hdrKey==FlatHdrKey::Auto || copyWeapon()) && s.projection &&
       !s.phase.failed && s.frameCoverage &&
       flatCameraInjectUpstreamOwns() && !foreignWork.load(std::memory_order_acquire) &&
       !s.prefix.uncertain && !d.supported && !tone && !copy &&
       k.format==26 && k.color && k.depth && k.dsv && k.camera &&
       (k.depth==s.namedDepth || unnamedWorldDepth) && flat_mono_detail::hdrViewport(k,k.width,k.height) &&
       !flatHdrCouldConsume(s.hdr,k) &&
       flatRuntimeCameraCurrent(d,s.prefix.sequence+1,s.prefix.frame)) {
        for(uint32_t i=0;i<s.prefix.targetsUsed;++i) {
            const auto& t=s.prefix.targets[i];
            if(t.resource==k.color && t.hdrCamera && t.writes.draws && !t.hdrBad &&
               !t.hdrLayoutChanged && !t.menuInherited && t.writes.key.format==26 &&
               t.writes.key.depth==k.depth && t.writes.key.dsv==k.dsv &&
               flat_mono_detail::cameraCurrent(t.tone,s.prefix.frame) &&
               !flatRuntimeSameCamera(t.tone,d)) { overlayTarget=&t; break; }
        }
    }
    // D3D's null depth-stencil state is the default depth-write state. Query
    // only candidates or draws sharing a depth resource with an open suffix.
    bool depthStateNeeded=overlayTarget!=nullptr;
    if(k.depth && overlaySuffixActive.load(std::memory_order_relaxed))
      for(uint32_t i=0;i<s.prefix.targetsUsed;++i)
        if(s.prefix.targets[i].overlayOpen && s.prefix.targets[i].overlayDepth==k.depth)
            depthStateNeeded=true;
    D3D11_DEPTH_STENCIL_DESC effectiveDepth{};
    effectiveDepth.DepthEnable=TRUE;
    effectiveDepth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    if(depthStateNeeded) {
        FlatComputeInternalScope guard;
        Ptr<ID3D11DepthStencilState> boundDepth;
        UINT stencilRef=0;ctx->OMGetDepthStencilState(&boundDepth,&stencilRef);
        if(boundDepth)boundDepth->GetDesc(&effectiveDepth);
        d.effectiveDepthWrite=effectiveDepth.DepthEnable &&
            effectiveDepth.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL;
        d.effectiveStencilWrite=effectiveDepth.StencilEnable && effectiveDepth.StencilWriteMask!=0;
    }
    if(overlayTarget && !d.effectiveDepthWrite &&
       (!d.effectiveStencilWrite || effectiveDepth.StencilWriteMask==0x04u) &&
       (!nonzeroPhase(s) || s.phase.applied)) {
        float worldRows[6][4]{},drawRows[6][4]{};
        const bool shape=flat_mono_detail::cameraShape(overlayTarget->tone.camera,worldRows) &&
            flat_mono_detail::cameraShape(d.camera,drawRows);
        if(shape && flatCameraCenteredPairAtPhase(worldRows,drawRows,
                s.phase.currentX,s.phase.currentY,k.width,k.height)) {
            const auto pair=classifyFlatProjectionPair(s,k.vs,k.ps);
            if((pair.vs==FlatVsProjectionClass::ForwardColumns ||
                pair.vs==FlatVsProjectionClass::ForwardDp4) &&
                pair.ps==FlatPsProjectionSafety::Clean) {
                if(!s.namedDepth)s.sourceSpell.overlayUnnamed();
                if(copyWeapon()) {
                    // The copy route resolves after the game's post chain, so the glow is in the picture the SDK is handed and
                    // there is no clean H to restore: no private MRT bracket, no open suffix, nothing to seal or to fail.
                    d.alternateHdr=true;
                    ++s.copyWeaponWindow.alternateDraws;
                } else {
                    d.overlayProtected=true;
                    overlayPlanned=true;
                    ++s.overlayPlannedWindow;
                    overlayHdr=static_cast<ID3D11Texture2D*>(const_cast<void*>(k.color));
                    overlayDsv=static_cast<ID3D11DepthStencilView*>(const_cast<void*>(k.dsv));
                }
            }
        }
    }
    // A protected HDR suffix can only finish at the HDR consumer. The final
    // LDR copy is never allowed to feed overlay-containing colour to a
    // temporal backend if that consumer was absent or declined.
    if(copy && overlayOpen(s)) overlayFail(s,"overlay-unsealed-at-final-copy");
    // Gate 1 consolidation: the copy draw's selection is produced as the
    // frame contract (identical decision), and every draw is recorded into
    // the trace ring for the reducer replay.
    FlatMonoFrame selected = [&]() -> FlatMonoFrame {
        flatcpu::Scope reduce(flatcpu::kReduce);   // the reducer: the online prefix model and the selector
        return copy
            ? flatRuntimeObserveContract(s.prefix, d, s.traceContract)
            : flatRuntimeObserve(s.prefix, d);
    }();
    if(d.overlayProtected && !s.overlayFailureNoted) {
        bool admitted=false;
        for(uint32_t i=0;i<s.prefix.targetsUsed;++i)
            if(s.prefix.targets[i].resource==overlayHdr &&
               s.prefix.targets[i].overlayOpen && !s.prefix.targets[i].hdrBad)
                admitted=true;
        if(admitted) overlaySuffixActive.store(true,std::memory_order_release);
        else { overlayPlanned=false; overlayHdr=nullptr; overlayDsv=nullptr; }
    }
    if(!s.overlayFailureNoted)for(uint32_t i=0;i<s.prefix.targetsUsed;++i) {
        const auto& target=s.prefix.targets[i];
        if(target.overlayOpen && target.hdrBad) {
            overlayFail(s,"overlay-unprotected-suffix-draw",target.resource);
            break;
        }
    }
    // The HDR route's trigger detector (flat_hdr_route.h, section 81), on every watched draw whatever the key says: rules
    // (i), (ii) and (iv) are comparisons on what this scope already holds, the four pixel-shader slots are read from the
    // binding shadow (no D3D call) only for the few draws that pass them, and with the key off nothing below acts on the
    // answer. The resolved slots ride into the trace, which has no other way to carry them.
    const void* hdrSrv[4] = {};
    bool hdrSrvKnown = false, hdrTrigger = false;
    {
        flatcpu::Scope hdrScope(flatcpu::kHdrRoute);
        if (flatHdrCouldConsume(s.hdr, k)) {
            for (uint32_t slot = 0; slot < 4; ++slot)
                hdrSrv[slot] = view(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + slot), 2 + slot).resource;
            hdrSrvKnown = true;
        }
        hdrTrigger = flatHdrObserveDraw(s.hdr, k, s.prefix.sequence, hdrSrvKnown ? hdrSrv : nullptr, hdrSrvKnown);
    }
    {
        flatcpu::Scope trace(flatcpu::kTrace);
        // Correlation q is separate from the original contract observation.
        flatTraceRecord(s.traceRing, d, foreignWork.load(std::memory_order_acquire), hdrSrvKnown ? hdrSrv : nullptr, s.prefix.sequence);
    }
    // The final copy's admission by structure (flat_copy_structure.h, section 83): after the reducer and the detector, which
    // it reads, and before anything below uses the verdict. What the whitelist selected stays what it was.
    if (copy) { flatcpu::Scope reduce(flatcpu::kReduce); selected = copyAdmit(s, d, selected); }
    if(cameraProbeAttempt)for(uint32_t i=0;i<s.prefix.targetsUsed;++i)if(s.prefix.targets[i].resource==k.color){
        const auto& witness=s.prefix.targets[i].firstBad;
        Log::get().note("flat camera probe observer: attempt=%u frame=%llu draw-seq=%u first-bad=%s first-bad-seq=%u caused-first-camera-conflict=%u",
            cameraProbeAttempt,(unsigned long long)s.prefix.frame,s.prefix.sequence,flatRuntimeConflictName(witness.cause),witness.sequence,
            witness.cause==FlatRuntimeConflict::CameraChange && witness.sequence==s.prefix.sequence?1u:0u);
        break;
    }
    // Section 104, the HDR route's second camera. A draw that made its HDR target a second camera (the model's first camera change) and every gate
    // that would have admitted it as a protected overlay, or on the copy route as an alternate HDR writer, as the draw stood. The 2026-10-07 log
    // (v0.18.3-10-g0d4bc714) named the pair (VS 025B4B9F, PS 46F92DC7: the first person's camera, near 0.0675 against the world's 0.025) and
    // not why nothing admitted it: planned-draws was 0 in every window of it. A few lines of this say which gate refused, and whether a world
    // had been named when it did.
    if(k.format==26 && k.camera && !tone && !copy && s.admissionLines<6 && GetTickCount64()-s.lastAdmissionMs>=10000)
        for(uint32_t i=0;i<s.prefix.targetsUsed;++i) {
            const auto& target=s.prefix.targets[i];
            if(target.resource!=k.color)continue;
            if(target.firstBad.cause!=FlatRuntimeConflict::CameraChange || target.firstBad.sequence!=s.prefix.sequence)break;
            ++s.admissionLines;s.lastAdmissionMs=GetTickCount64();
            const auto flag=[](bool v){return v?1u:0u;};
            const bool hdrCamera=target.hdrCamera,drawn=target.writes.draws!=0;
            Log::get().note("flat overlay admission refused: frame=%llu seq=%u VS=%016llX PS=%016llX route=%s key-auto=%u copy-weapon=%u work-full=%u projection=%u "
                "jitter-wanted=%u phase-ok=%u frame-coverage=%u injector-owns=%u foreign-work=%u uncertain=%u supported=%u color-depth-dsv-camera=%u "
                "viewport=%u hdr-could-consume=%u camera-current=%u world-named=%u depth-is-named=%u pool-less-so-far=%u (sources=%u first-person=%u stock-family=%u) "
                "target-hdr-camera=%u target-drawn=%u target-layout-changed=%u target-menu=%u target-depth-same=%u target-dsv-same=%u target-tone-current=%u "
                "overlay-target-found=%u depth-write=%u stencil-write=%u stencil-mask-04=%u phase-applied-or-zero=%u; the model made this draw the HDR's first second camera, "
                "so none of the admissions took it",
                (unsigned long long)s.prefix.frame,s.prefix.sequence,(unsigned long long)k.vs,(unsigned long long)k.ps,copyWeapon()?"copy":"hdr",
                flag(s.hdrKey==FlatHdrKey::Auto),flag(copyWeapon()),flag(s.work==FlatWork::Full),flag(s.projection!=nullptr),flag(true),
                flag(!s.phase.failed),flag(s.frameCoverage),flag(flatCameraInjectUpstreamOwns()),flag(foreignWork.load(std::memory_order_acquire)),
                flag(s.prefix.uncertain),flag(d.supported),flag(k.color && k.depth && k.dsv && k.camera),
                flag(flat_mono_detail::hdrViewport(k,k.width,k.height)),flag(flatHdrCouldConsume(s.hdr,k)),
                flag(flatRuntimeCameraCurrent(d,s.prefix.sequence,s.prefix.frame)),flag(s.namedDepth!=nullptr),flag(k.depth==s.namedDepth),
                flag(unnamedWorldDepth),s.prefix.sourcesUsed,s.prefix.firstPersonDraws,s.prefix.unsupportedFamilyDraws,
                flag(hdrCamera),flag(drawn),flag(target.hdrLayoutChanged),flag(target.menuInherited),flag(target.writes.key.depth==k.depth),
                flag(target.writes.key.dsv==k.dsv),flag(flat_mono_detail::cameraCurrent(target.tone,s.prefix.frame)),flag(overlayTarget!=nullptr),
                flag(d.effectiveDepthWrite),flag(d.effectiveStencilWrite),flag(effectiveDepth.StencilWriteMask==0x04u),flag(!nonzeroPhase(s) || s.phase.applied));
            break;
        }
    if(weaponFootprintStarted)for(uint32_t i=0;i<s.prefix.targetsUsed;++i)
        if(s.prefix.targets[i].resource==k.color){
            const auto& bad=s.prefix.targets[i].firstBad;
            s.weaponFootprint.confirm(s.prefix.frame,weaponFootprintSeq,bad.sequence,flatRuntimeConflictName(bad.cause));
            break;
        }
    s.hdrCopiesAccepted+=s.prefix.imageCopiesAccepted-oldImageAccepted;
    s.hdrCopiesRefused+=s.prefix.imageCopiesRefused-oldImageRefused;
    s.menuCopiesAccepted+=s.prefix.menuCopiesAccepted-oldMenuAccepted;
    s.menuCopiesRefused+=s.prefix.menuCopiesRefused-oldMenuRefused;
    if (s.prefix.targetsUsed > oldTargets) {
        s.colors[oldTargets] = static_cast<ID3D11Resource*>(rt.resource);
        s.depths[oldTargets] = static_cast<ID3D11Resource*>(ds.resource);
        if(s.prefix.menuCopiesAccepted>oldMenuAccepted)
            for(uint32_t i=0;i<oldTargets;++i)
                if(s.prefix.targets[i].resource==k.srvResource[0]) {
                    s.depths[oldTargets]=s.depths[i];break;
                }
    }
    // Stand-down (flat_standdown.h): this copy draw's verdict on the frame's chain --
    // recorded here, before anything below can return -- merged so that a frame with
    // any selecting copy draw is treatable whatever another copy said.
    if (copy) {
        const FlatFrameSeen seen = flatFrameSeenFor(selected.selected(), selected.reason);
        if (seen >= s.frameSeen) { s.frameSeen = seen; s.frameReason = selected.reason; }
        // What the view held when it had no motion source (section 104); the line names it.
        if (selected.reason == FlatMonoReason::NoSupportedSource) { s.sourcelessLast = selected.sourceless; s.sourcelessLastFrame = s.prefix.frame; }
        noteSourceFreeContent(s, selected);
    }
    // The HDR route's selection at its trigger (and, with the key auto, its verdict into the stand-down): a Probe frame
    // runs it too, so a probe that finds the route's consumer ends the stand-down, as a probe that selects a copy does.
    if (hdrTrigger) { flatcpu::Scope hdrScope(flatcpu::kHdrRoute); hdrSelectAtTrigger(s); }
    if(hdrTrigger && s.weaponFootprint.tracking(s.prefix.frame)) {
        uint32_t firstBadSeq=0;
        for(uint32_t i=0;i<s.prefix.targetsUsed;++i)
            if(s.prefix.targets[i].resource==s.weaponFootprint.colorResource()){
                firstBadSeq=s.prefix.targets[i].firstBad.sequence;break;}
        s.weaponFootprint.consumer(ctx,s.prefix.frame,s.prefix.sequence,s.hdr.trigger.hdr,
            s.hdr.trigger.vs,s.hdr.trigger.ps,s.hdr.trigger.srvSlot,
            s.hdrSelected.selected()?"selected":flatMonoReasonName(s.hdrSelected.reason),firstBadSeq);
    }
    // A Probe frame is the contract observation above and nothing else: the prefix model
    // and the selector, on the same inputs an active frame gives them. No coverage, no
    // projection readiness, no source naming or substitution, no resolve. A copy draw the
    // selector selects ends the stand-down at the Present.
    if (s.work == FlatWork::Probe) return;
    // FP16 image intermediates use the same scene-size predicate; their
    // producer admission remains separate from the format-23/26 motion source.
    const bool sceneExtent = flatContractKind(false, k.color, k.depth, k.width, k.height, k.format==9?26:k.format, s.prefix.width, s.prefix.height, false) == kFlatContractScreen;
    const bool sourceShape=d.supported && !weaponMotionFamilyVs(k.vs) && k.camera && k.depth && sceneExtent &&
        (k.format==23 || k.format==26) && flat_mono_detail::fullViewport(k,k.width,k.height);
    // Before the world is named, a source draw of a camera the selected world's reference rules out is no source candidate (namingVetoed).
    const bool sourceCandidate=sourceShape && !(!s.namedDepth && namingVetoed(s,d,k));
    const auto alternate=flatUntrustedNomination(d,s.namedDepth,
        s.namedDepth?s.namedCamera:nullptr,sceneExtent,
        s.work==FlatWork::Full && s.hdrKey==FlatHdrKey::Auto &&
            !s.untrusted.finished(),
        s.prefix.sequence,s.prefix.frame,sourceCandidate && !s.namedDepth);
    const bool inertSource=alternate.candidate &&
        flatUntrustedProvenCameraIndependent(classifyFlatProjectionPair(s,k.vs,k.ps));
    if(s.work==FlatWork::Full && s.hdrKey==FlatHdrKey::Auto &&
       !s.untrusted.finished() && !inertSource && k.format==23 && sceneExtent &&
       k.color && k.depth) {
        bool inserted=false;
        auto* observed=flatUntrustedObserveCamera(s.unclassifiedPool,64,
            s.unclassifiedPoolUsed,k.depth,k.camera,&inserted);
        if(!observed) {
            if(!s.unclassifiedPoolOverflow) {
                auto& entry=s.unclassifiedOverflowFirst;
                entry={};entry.depth=k.depth;entry.color=k.color;entry.b1=k.b1;entry.dsv=k.dsv;
                entry.vs=k.vs;entry.ps=k.ps;entry.cameraHash=k.cameraHash;
                entry.writeEpoch=k.writeEpoch;entry.writeSeq=k.writeSeq;
                entry.firstSeq=s.prefix.sequence;entry.width=k.width;entry.height=k.height;entry.draws=1;
                entry.hasCamera=k.camera!=nullptr;
                if(k.camera)std::memcpy(entry.camera,k.camera,kFlatCameraBytes);
            }
            s.unclassifiedPoolOverflow=true;
        } else if(inserted) {
            auto& entry=*observed;
            entry.color=k.color;entry.b1=k.b1;entry.dsv=k.dsv;entry.vs=k.vs;entry.ps=k.ps;
            entry.cameraHash=k.cameraHash;entry.writeEpoch=k.writeEpoch;
            entry.firstSeq=s.prefix.sequence;entry.writeSeq=k.writeSeq;
            entry.width=k.width;entry.height=k.height;
        }
    }
    const bool alternateNominee=alternate.candidate && !inertSource;
    if(alternateNominee) {
        // Before H names its world camera, retain every structural scene draw
        // in a camera bucket. The consumer discards the world bucket. A motion
        // source whitelist cannot know which original colour PS owns a pixel.
        const bool preWorld=alternate.preWorld;
        const bool fullViewport=alternate.fullViewport;
        const bool current=alternate.current;
        const bool priorFailure=s.untrusted.failure()!=nullptr;
        if(alternate.admissible()) {
            // The prior world producer may have left MRT6 and a substituted
            // PS bound lazily. Restore the game's state before MRT7 snapshots
            // its PS, blend and render-target binding.
            flatRuntimeSubstitution(context,FlatSubstEvent::kOtherDraw);
        }
        FlatUntrustedDrawDiagnostic nomineeDiagnostic{};
        nomineeDiagnostic.cameraHash=k.cameraHash;
        nomineeDiagnostic.writeEpoch=k.writeEpoch;
        nomineeDiagnostic.writeSeq=k.writeSeq;
        nomineeDiagnostic.current=current;
        nomineeDiagnostic.viewport=fullViewport;
        untrustedPlanned=s.untrusted.plan(s.prefix.frame,s.prefix.sequence,
            static_cast<ID3D11Texture2D*>(const_cast<void*>(k.color)),
            static_cast<ID3D11Texture2D*>(const_cast<void*>(k.depth)),
            static_cast<ID3D11DepthStencilView*>(const_cast<void*>(k.dsv)),
            k.vs,k.ps,d.camera,alternate.admissible(),&nomineeDiagnostic);
        if(alternate.admissible()) {
            FlatComputeInternalScope internal;
            s.untrusted.diagnoseNomineeShader(s.prefix.sequence,
                static_cast<ID3D11PixelShader*>(bindingGet(BindSlot::Ps)));
        }
        if(!untrustedPlanned)s.untrusted.diagnosePlanShader(context,s.prefix.sequence,
            [](void* shader) { return lookupShaderHash(shader); });
        if(untrustedPlanned)untrustedCoverageActive.store(true,std::memory_order_release);
        // Diagnostic only. Keep the first distinct shader/outcome signatures
        // across frames and reserve one line for a good draw blocked by an
        // earlier sticky failure. No context queries or per-draw row hashing.
        const uint32_t bits=(preWorld?1u:0u)|(d.supported?2u:0u)|
            (fullViewport?4u:0u)|(current?8u:0u)|
            (priorFailure?16u:0u)|(untrustedPlanned?32u:0u);
        const uint32_t stickyRole=preWorld?0u:1u;
        const bool stickyGood=d.supported && fullViewport && current &&
            priorFailure && !untrustedPlanned && !s.untrustedStickyDiagnostic[stickyRole];
        bool distinct=true;
        for(uint32_t i=0;i<s.untrustedNomineeDiagnosticsUsed;++i) {
            const auto& seen=s.untrustedNomineeDiagnostics[i];
            if(seen.vs==k.vs && seen.ps==k.ps && seen.bits==bits) {distinct=false;break;}
        }
        if(stickyGood)s.untrustedStickyDiagnostic[stickyRole]=true;
        const bool emit=distinct && s.untrustedNomineeDiagnosticsUsed<32;
        if(emit) {
            auto& seen=s.untrustedNomineeDiagnostics[s.untrustedNomineeDiagnosticsUsed++];
            seen.vs=k.vs;seen.ps=k.ps;seen.bits=bits;
        } else if(distinct)++s.untrustedNomineeDiagnosticsDropped;
        if(emit || stickyGood) {
            float nearValue=0,namedNear=0;
            std::memcpy(&nearValue,d.camera+(3*4+2)*sizeof(float),sizeof(float));
            if(!preWorld)std::memcpy(&namedNear,s.namedCamera+(3*4+2)*sizeof(float),sizeof(float));
            float drawRows[6][4]{},namedRows[6][4]{};
            const bool drawShape=flat_mono_detail::cameraShape(d.camera,drawRows);
            const bool namedShape=!preWorld && flat_mono_detail::cameraShape(s.namedCamera,namedRows);
            const bool phasePair=drawShape && namedShape &&
                flatCameraCenteredPairAtPhase(namedRows,drawRows,s.phase.currentX,s.phase.currentY,k.width,k.height);
            Log::get().note("flat untrusted nominee: frame=%llu seq=%u check-q=%u VS=%016llX PS=%016llX role=%s supported=%u viewport=%u current=%u prior-failure=%u planned=%u color=%p depth=%p dsv=%p named-depth=%p b1=%p named-b1=%p camera=%016llX named-camera=%016llX draw-shape=%u named-shape=%u phase-pair=%u phase=(%.9g,%.9g) near=%.9g named-near=%.9g write=%llu/%u viewport-rect=%u:(%.1f,%.1f,%.1f,%.1f,%.2f,%.2f) extent=%ux%u failure=%s",
                (unsigned long long)s.prefix.frame,s.prefix.sequence,s.prefix.sequence,
                (unsigned long long)k.vs,(unsigned long long)k.ps,preWorld?"pre-world-structural":"post-name-mismatch",
                d.supported?1u:0u,fullViewport?1u:0u,current?1u:0u,
                priorFailure?1u:0u,untrustedPlanned?1u:0u,k.color,k.depth,k.dsv,s.namedDepth,
                k.b1,s.namedConstants,(unsigned long long)k.cameraHash,
                (unsigned long long)(preWorld?0:flatCameraHash(s.namedCamera)),
                drawShape?1u:0u,namedShape?1u:0u,phasePair?1u:0u,
                s.phase.currentX,s.phase.currentY,
                nearValue,namedNear,(unsigned long long)k.writeEpoch,k.writeSeq,
                k.viewportCount,k.viewport[0],k.viewport[1],k.viewport[2],k.viewport[3],
                k.viewport[4],k.viewport[5],k.width,k.height,
                s.untrusted.failure()?s.untrusted.failure():"none");
        }
    }
    // A same-depth draw without camera rows is certified at H through the
    // complete scene-source table. It cannot be assigned to a camera bucket.
    const bool foregroundCandidate=d.supported && weaponMotionFamilyVs(k.vs) && k.camera && k.depth &&
        k.kind==kFlatContractPool && k.format==23 && sceneExtent &&
        flat_mono_detail::fullViewport(k,k.width,k.height);
    // The first-person pool family can draw before the world into the same
    // depth. It is not the scene source: the VR screen-motion naming path uses
    // the same family exclusion before choosing its world camera.
    const bool domainWasBeforeWorld=s.namedDepth==nullptr;
    if(sourceCandidate && !s.namedDepth) {
        FlatComputeInternalScope guard;
        flatcpu::Scope engine(flatcpu::kEngineDraw);
        s.namedDepth=k.depth;s.namedConstants=k.b1;s.namedWorldQ=s.prefix.sequence;s.namedVs=k.vs;s.namedPs=k.ps;
        std::memcpy(s.namedCamera,d.camera,sizeof(d.camera));
        {float namedRows[6][4];std::memcpy(namedRows,d.camera,sizeof(namedRows));s.worldReference=flatDomainWorldReference(namedRows);}
        engineVelocityNoteSource(static_cast<ID3D11Texture2D*>(const_cast<void*>(k.depth)),static_cast<ID3D11Buffer*>(const_cast<void*>(k.b1)));
    }
    const bool foregroundGap=s.foreground.needsPreWorldState(
        static_cast<ID3D11Texture2D*>(const_cast<void*>(k.depth)));
    // Owner marks follow the depth surface (section 104). World surfaces keep
    // the engine producer's slots and the camera term, as single-camera
    // frames always have. Only first-person depth writers are captured and
    // marked, and the consumer trusts such a mark only while its depth is
    // still the pixel's. A draw that cannot write depth changes no surface.
    const bool nativeDepthExtent=k.depthWidth==s.prefix.width && k.depthHeight==s.prefix.height;
    if(s.engine!=FlatMonoResolveMode::Taa && s.work==FlatWork::Full && k.depth && (sceneExtent || nativeDepthExtent)) {
        auto* candidate=observeDomainCandidate(s,k.depth);
        domainProtectedOverlay=overlayPlanned && d.overlayProtected && !d.effectiveDepthWrite &&
            (!d.effectiveStencilWrite || effectiveDepth.StencilWriteMask==0x04u);
        if(domainProtectedOverlay) {
            // Its HDR color is captured separately and restored after AA.
            // Successful original bracket completion is checked below.
            domainDepth=static_cast<ID3D11Texture2D*>(const_cast<void*>(k.depth));
        } else if(candidate) {
            float rows[6][4]{};if(k.camera)std::memcpy(rows,d.camera,sizeof(rows));
            // Before naming, the world camera's prepass shares this depth with the first-person camera's (near 0.025 against 0.0675).
            // A draw with the last named world's near plane AND projection scale is the world's (flatDomainPredictsWorld, the one
            // prediction, shared with the cohort flag); its witness must equal the selected world camera at H, so a wrong prediction
            // only refuses. A weapon aiming down sights takes the world's near plane but keeps its own scale (1.23 to 1.66 times the
            // world's): it is not predicted, and is classified, captured and marked as the first-person draw it is.
            const bool predictedWorld=!s.namedDepth && k.camera && worldPredicted().predicted;
            if(!s.namedDepth && k.camera && worldPredicted().nearEqual && !worldPredicted().predicted)++s.predictedScaleRejectedWindow;
            const bool worldCamera=s.namedDepth && k.camera && std::memcmp(s.namedCamera,d.camera,sizeof(d.camera))==0;
            auto witness=[&](unsigned kind){
                if(!candidate->pendingNull.add(s.prefix.frame,k.depth,k.depthWidth,k.depthHeight,rows,s.phase.currentX,s.phase.currentY,
                                               k.vs,k.ps,kind))
                    domainFail(s,"pending-world","foreground-pending-null-overflow",k);
            };
            if(!k.camera || worldCamera || predictedWorld) {
                // Screen passes and world-camera draws: nothing to mark or capture.
                if(predictedWorld){++s.foregroundCounts.predictedWorld;witness(1u);}
                else ++s.foregroundCounts.worldUnmarked;
            } else {
                const auto& proof=domainShaderProof(s,k.vs,k.ps);
                const auto plan=flatDomainPlan(proof,k.color!=nullptr,k.format,true,s.namedDepth!=nullptr,false);
                if(plan.kind==FlatDomainPlanKind::PendingWorldNull) {
                    // A null prepass before naming at another near is world only
                    // if it matches the selected camera at H.
                    witness(2u);
                } else if(!plan.admitted() && domainDrawPreservesSurface(context)) {
                    // No depth write: the surface and its owner mark are the last
                    // depth writer's. Forward the game's draw unchanged.
                    ++s.foregroundCounts.surfacePreserving;++s.foregroundCounts.surfacePreservingForeign;
                } else if(!plan.admitted() && proof.inertNoSideEffects) {
                    const char* inertRefusal=domainOriginalInertRefusal(context,k,proof);
                    if(inertRefusal)domainFail(s,"inert-state",inertRefusal,k);
                    // Otherwise no private marker/history or ownership nomination:
                    // forward the game's original stencil-only draw unchanged.
                } else if(!plan.admitted())domainFail(s,"planning",plan.refusal,k);
                else {
                    // A first-person depth writer: capture its geometry and mark it.
                    domainPlanned=true;domainDepth=candidate->depth.Get();domainVs=k.vs;domainPs=k.ps;
                    domainBeforeWorld=domainWasBeforeWorld;domainSameWorld=false;
                    domainCameraHash=k.cameraHash;domainFormat=k.format;domainHdrWriter=k.format==26;
                    if(domainHdrWriter) {
                        if(candidate->hdr && candidate->hdr.Get()!=k.color)
                            domainFail(s,"planning-HDR","foreground-multiple-provisional-HDR-targets",k);
                        else candidate->hdr=static_cast<ID3D11Resource*>(const_cast<void*>(k.color));
                    }
                    domainWidth=k.depthWidth;domainHeight=k.depthHeight;
                    domainPool=domainForeign=plan.kind==FlatDomainPlanKind::ForeignPool;
                    domainWriterToken=s.prefix.sequence;
                    std::memcpy(domainCamera,d.camera,sizeof(domainCamera));
                    ++s.foregroundCounts.foreignSeen;++candidate->foreignPlanned;
                }
            }
        }
    }
    FlatForegroundProbe::Draw evidence{};
    if(foregroundCandidate || foregroundGap) {
        evidence.vs=k.vs;evidence.ps=k.ps;evidence.kind=kind;evidence.count=count;
        evidence.seq=s.prefix.sequence;evidence.targetKind=static_cast<uint32_t>(k.kind);
        evidence.format=k.format;evidence.cameraHash=k.cameraHash;
        evidence.supported=d.supported;evidence.family=weaponMotionFamilyVs(k.vs);
        evidence.fullViewport=flat_mono_detail::fullViewport(k,k.width,k.height);
        evidence.start=start;evidence.base=base;evidence.instances=instances;
        evidence.startInstance=startInstance;
        std::memcpy(evidence.camera,d.camera,sizeof(evidence.camera));
        evidence.phaseX=s.phase.currentX;evidence.phaseY=s.phase.currentY;
    }
    if(foregroundCandidate && !alternateNominee) {
        foregroundPlanned=s.foreground.plan(s.prefix.frame,s.prefix.sequence,
            static_cast<ID3D11Texture2D*>(const_cast<void*>(k.color)),
            static_cast<ID3D11Texture2D*>(const_cast<void*>(k.depth)),
            static_cast<ID3D11DepthStencilView*>(const_cast<void*>(k.dsv)),evidence,s.namedDepth!=nullptr);
        if(s.foreground.active())foregroundProbeActive.store(true,std::memory_order_release);
    }
    if(sourceCandidate && s.foreground.active())
        s.foreground.worldSource(ctx,s.prefix.frame,s.prefix.sequence,
            static_cast<ID3D11Texture2D*>(const_cast<void*>(k.depth)),
            static_cast<ID3D11DepthStencilView*>(const_cast<void*>(k.dsv)));
    // A pool-family draw sees the game's state too, unless it can only continue a run of substituted producer draws (the
    // one whose camera, depth and constants this frame's naming holds) AND nothing below reads the context for it. What
    // the coverage classification asks the context in the ordinary (Upstream) route it answers from the binding shadow
    // (coverageDepthResource, coverageShadersMatch: flat_query_cut.h), and asks the context only on a check, after
    // flushing where the answer would be EDVR's. Two routes still read what is bound themselves, and so want the game's
    // state first: an F10 audit's captures (s.projectionFrames), and the legacy route's qualification of the projection
    // (qualifyProjection reads the shaders, the viewport and the constant buffers, and is skipped under Upstream).
    const bool continuesRun = sourceCandidate && s.namedDepth == k.depth && s.namedConstants == k.b1 &&
        std::memcmp(s.namedCamera, d.camera, sizeof(d.camera)) == 0;
    const bool coverageReads = s.projection && sceneExtent && k.color != s.prefix.output &&
        (k.format==9 || k.format==23 || k.format==26 || k.format==60) &&
        (s.projectionFrames != 0 || !flatCameraInjectUpstreamOwns());
    if (d.supported && (!continuesRun || coverageReads)) flatRuntimeSubstitution(context, FlatSubstEvent::kOtherDraw);
    if(s.foreground.active() && !foregroundCandidate) {
        if(foregroundGap) {
            // A lazy producer run can still have EDVR's substitute state
            // bound. Restore the game before querying only pre-world gaps.
            flatRuntimeSubstitution(context,FlatSubstEvent::kOtherDraw);
            s.foreground.noteSameDepthDraw(ctx,static_cast<ID3D11Texture2D*>(const_cast<void*>(k.depth)),evidence);
        } else s.foreground.noteAfterWorldDraw(static_cast<ID3D11Texture2D*>(const_cast<void*>(k.depth)));
    }
    if(s.projection) {
        if(s.projectionFrames)++s.projectionDraws;
        if(sceneExtent && k.color!=s.prefix.output && (k.format==9 || k.format==23 || k.format==26 || k.format==60)) {
            flatcpu::Scope coverage(flatcpu::kCoverage);   // the classification; qualifyProjection carves its own family out of it
            // Part B coverage census (always on, no F10 audit needed): which
            // recipe branch this candidate draw took. Orthogonal to whether
            // qualifyProjection then accepted or locally/globally refused it.
            ++s.covSceneDraws;
            if(s.projectionFrames)captureLocalProjection(s,k.vs,k.ps);
            const auto recipes=flatProjectionDrawRecipes(k.vs,k.ps);
            if(recipes.count) {
                ++s.covExact;
                FlatComputeInternalScope guard;
                Ptr<ID3D11Resource> depthHold;
                const void* depthResource=coverageDepthResource(ctx,k,depthHold);
                const bool owned=depthResource && (depthResource==s.namedDepth || depthResource==s.phaseDepth.Get());
                if(!owned && k.color==s.phaseHdr.Get())failPhase(s,"scene-projection-depth-unassociated");
                if(nonzeroPhase(s) && owned && s.phaseDepth && depthResource!=s.phaseDepth.Get())failPhase(s,"scene-depth-changed");
                const bool sceneHdr=flatRuntimeProjectionHdr(k,s.prefix.output,owned?depthResource:nullptr);
                projectionPlan=qualifyProjection(s,recipes,k.width,k.height,k.vs,k.ps,0,owned,sceneHdr);
            }
            else if(flatProjectionDrawUnchanged(k.vs,k.ps)) {
                ++s.covUnchanged;
                if(coverageShadersMatch(ctx,k)) {
                    if(s.projectionFrames) {++s.projectionUnchanged;projectionDetail(s,k.vs,k.ps,0,103,"bytecode-unchanged");}
                } else {if(s.projectionFrames) {++s.projectionUnknown;projectionDetail(s,k.vs,k.ps,0,100,"actual-shader-mismatch");}refuseDraw(s,"unchanged-shader-mismatch");}
            }
            else if(k.depth && (k.depth==s.namedDepth || k.depth==s.phaseDepth.Get())) {
                // Generic admission: classify the actual bytecode once per
                // pair. Provably forward or inert pairs take the identical
                // owned/sceneHdr/qualify flow as the exact recipes; anything
                // unproven keeps the capture+failPhase behavior below.
                const auto generic=classifyFlatProjectionPair(s,k.vs,k.ps);
                if((generic.vs==FlatVsProjectionClass::ForwardColumns || generic.vs==FlatVsProjectionClass::ForwardDp4) &&
                   generic.ps==FlatPsProjectionSafety::Clean) {
                    ++s.covGeneric;
                    FlatProjectionRecipes genericRecipes;
                    genericRecipes.add(FlatProjectionStage::Vertex,generic.vsSlot,
                        generic.vs==FlatVsProjectionClass::ForwardColumns?FlatProjectionPatchLayout::ForwardColumns:FlatProjectionPatchLayout::ForwardDp4,
                        generic.vsRow);
                    if(s.projectionFrames)projectionDetail(s,k.vs,k.ps,0,105,"generic-recipe");
                    FlatComputeInternalScope guard;
                    Ptr<ID3D11Resource> depthHold;
                    const void* depthResource=coverageDepthResource(ctx,k,depthHold);
                    const bool owned=depthResource && (depthResource==s.namedDepth || depthResource==s.phaseDepth.Get());
                    if(!owned && k.color==s.phaseHdr.Get())failPhase(s,"scene-projection-depth-unassociated");
                    if(nonzeroPhase(s) && owned && s.phaseDepth && depthResource!=s.phaseDepth.Get())failPhase(s,"scene-depth-changed");
                    const bool sceneHdr=flatRuntimeProjectionHdr(k,s.prefix.output,owned?depthResource:nullptr);
                    projectionPlan=qualifyProjection(s,genericRecipes,k.width,k.height,k.vs,k.ps,0,owned,sceneHdr);
                }
                else if(generic.vs==FlatVsProjectionClass::InertNoCB && generic.ps==FlatPsProjectionSafety::Clean) {
                    ++s.covInert;
                    if(coverageShadersMatch(ctx,k)) {
                        if(s.projectionFrames) {++s.projectionUnchanged;projectionDetail(s,k.vs,k.ps,0,106,"generic-inert");}
                    } else {if(s.projectionFrames) {++s.projectionUnknown;projectionDetail(s,k.vs,k.ps,0,100,"actual-shader-mismatch");}refuseDraw(s,"unchanged-shader-mismatch");}
                }
                else {
                    captureUnknownProjection(s,k);
                    if(s.projectionFrames) {++s.projectionUnknown;projectionDetail(s,k.vs,k.ps,0,101,"unknown-scene-projection-recipe");}
                    refuseDraw(s,"unknown-scene-projection-recipe");
                }
            }
        }
    }
    if (continuesRun) {
        FlatComputeInternalScope guard;
        // Engine motion's draw wrapper, lazy form (engine_velocity.h): the game's render targets saved once per binding, then
        // BeforeDraw (the shader substitution, MRT6, the blend state), and EDVR's state stays bound for the next producer
        // draw. True only when the draw is substituted; a declined one has already put the game's state back. The
        // wrapper's D3D state calls are counted where they are made.
        flatcpu::Scope engine(flatcpu::kEngineDraw);
        producer = engineVelocityFlatBeginDraw(ctx, &gameHadTarget6);
    }
    // The earlier capture preserves original game CBs and shader identities.
    // Motion is sampled only after the producer has attached its actual MRT6,
    // and the destructor takes the matching sample before restoring the draw.
    if(drawCaptureStarted) {
        FlatComputeInternalScope guard;
        s.drawCapture.motionBefore(ctx,producer && !gameHadTarget6,sourceCandidate);
    }
    // Engine motion observes unmodified game constants above. Under Upstream
    // ownership the camera was already jittered at the source by the
    // injector, so the legacy row-patching scope is suppressed (a counter
    // elsewhere, never silence); the injector's noteApplied covers the
    // phase machine's application accounting.
    if(projectionPlan && !flatCameraInjectUpstreamOwns()) {
        flatcpu::Scope jitter(flatcpu::kProjection);   // the private constant-buffer binding for the draw
        projection.emplace(*projectionPlan);
        if(projection->active()) {s.phase.noteApplied();++s.jitterDraws;}
        else refuseDraw(s,"draw-binding-refused");
    }
    // Tripwire, structurally unreachable: a legacy application under Upstream.
    if(projection && projection->active() && flatCameraInjectUpstreamOwns())++s.rows.legacyAppliedUnderUpstream;
    // The HDR route treats at its trigger (key auto, the selection selected): the game's pass that reads H next sees the
    // anti-aliased image with its own bindings untouched, and the copy stage below leaves the frame to the route.
    if (hdrTrigger && s.hdrKey == FlatHdrKey::Auto && s.hdrSelected.selected()) treatHdr(s.hdrSelected, s.hdr.trigger.srvSlot);
    // fix.ui_quality's flat layer (flat_ui_layer.h): the cockpit HUD families out of H and into the shared layer as eye 0,
    // the game's tonemap re-issued over it, and at the output copy the door and the composite. Every decision is made
    // here, after the scope has planned everything else it does with this draw (a draw it does anything else with is
    // refused, and stays in H as stock).
    if (flatUiLayerOn()) {
        FlatUiLayerAsk ask = FlatUiLayerAsk::kNotAsked;
        if (copy) {
            uiComposite = true;
        } else {
            uint32_t sceneRw = 0, sceneRh = 0, sceneOw = 0, sceneOh = 0;
            flatRuntimeSceneSizes(&sceneRw, &sceneRh, &sceneOw, &sceneOh);
            FlatUiLayerDraw ui;
            ui.frame = s.prefix.frame; ui.vs = k.vs; ui.ps = k.ps; ui.color = k.color; ui.width = k.width; ui.height = k.height;
            ui.format = k.format;
            ui.hdrTarget = flatUiTargetClass(k.color && k.color == s.prefix.output, k.width, k.height, k.format,
                                             s.prefix.width, s.prefix.height, sceneRw, sceneRh) == FlatUiTarget::kHdr;
            ui.mapOpen = flatRuntimeMapOpenFrame();   // this frame's map answer (the frame's one read): gates the map families only
            ui.otherWork = producer || (projection && projection->active()) || drawCaptureStarted || drawPacket ||
                           overlayPlanned || foregroundPlanned || untrustedPlanned || domainPlanned ||
                           weaponFootprintStarted || d.overlayProtected || d.alternateHdr;
            ui.upstream = flatCameraInjectUpstreamOwns();
            static_assert(sizeof(ui.rows) <= kFlatCameraBytes, "the camera table's rows hold the six rows");
            if (k.camera) { ui.haveRows = true; std::memcpy(ui.rows, d.camera, sizeof(ui.rows)); }
            ui.phaseX = s.phase.currentX; ui.phaseY = s.phase.currentY;
            ask = flatUiLayerDecide(ctx, ui);   // a cockpit HUD family's draw, or kNotAsked
            if (ask == FlatUiLayerAsk::kDecided) { uiTake = true; uiVs = k.vs; uiPs = k.ps; }
            else if (ask == FlatUiLayerAsk::kRefused) {}   // a HUD draw left in H: never also a tone candidate
            // The game's tone pass by its known pair, whatever its vertex count (the 11:32 flight's HDR route read a copy of
            // H, which the structural rule below never matched): the next frame's proof, and this frame's admission.
            // The registry's tone pass, or the map frames' tonemap (flatUiMapToneSlotOf: the UI layer's own table, not the registry).
            else if (tone || flatUiMapToneSlotOf(k.vs, k.ps) != ~0u) {
                const uint32_t slot = tone ? flat_mono_detail::toneHdrSlot(k.vs, k.ps) : flatUiMapToneSlotOf(k.vs, k.ps);
                // The route so far: the HDR route once it treated this frame, else the resolve plan's (trained-native,
                // trained-upscale, ...), or none yet.
                const char* route = s.hdrTreated ? "hdr"
                    : s.haveResolvePlan ? flatResolveRoute(s.plannedResolve.mode, s.plannedResolve.renderWidth, s.plannedResolve.renderHeight,
                                                           s.plannedResolve.outputWidth, s.plannedResolve.outputHeight).name
                                        : "no-plan";
                uiTone = flatUiLayerToneCandidate(ctx, s.prefix.frame, sceneRw, sceneRh, static_cast<int>(slot), k.vs, k.ps,
                                                  slot < 2 ? k.srvResource[slot] : nullptr, k.width, k.height, route);
            }
            // A plain copy reading the HUD's target names the copy the tone may read (the HDR route's post chain).
            // Its source is read from the context, not the binding shadow: when this copy is the HDR route's trigger, the
            // resolve ran inside this scope (treatHdr) and the shadow no longer names the draw's bindings (13:23 flight).
            else if (k.ps == flat_mono_detail::kCopyPs) {
                FlatComputeInternalScope readGuard;
                Ptr<ID3D11ShaderResourceView> copySrv;
                ctx->PSGetShaderResources(0, 1, &copySrv);
                Ptr<ID3D11Resource> copySource;
                if (copySrv) copySrv->GetResource(&copySource);
                flatUiLayerNoteCopy(s.prefix.frame, copySource.Get(), k.color);
            }
            // Any other full-screen triangle may be the game's tonemap under a pair the flat list does not know: VR's
            // structural admission, by the HUD source it reads.
            else if (count == 3 && instances == 1 && (kind == 'D' || kind == 'N'))
                uiTone = flatUiLayerToneAdmit(ctx, s.prefix.frame, k.width, k.height, kind, count, instances, startInstance);
        }
        // A draw the layer leaves, while it holds this frame's HUD: a write of the depth buffer a seed copied makes it stale.
        if (!uiTake && !copy) flatUiLayerNoteSceneDraw(ctx, count, instances, kind);
    }
    if (!copy) return;
    if (s.hdrTreated) {
        // The frame was treated before the post chain, so nothing here resolves again (the contract observation above
        // still ran for the trace). fix.render_sharpness stays at the copy, on the game's own LDR image, and only where
        // the copy route's selector recognised that copy: then t0 is known to be the tone output and RCAS may take it.
        if (selected.selected()) {
            FlatComputeInternalScope guard;
            ctx->PSGetShaderResources(0, 1, &original);
            Ptr<ID3D11Resource> actualColor; if (original) original->GetResource(&actualColor);
            if (actualColor.Get() == selected.color) {
                ID3D11ShaderResourceView* sharpened = flatSharpenView(ctx, original);
                if (sharpened && sharpened != original) { ctx->PSSetShaderResources(0, 1, &sharpened); replaced = true; }
            }
        }
        return;
    }
    // Local refusal's observation: no resolve until a qualified, completely
    // covered frame requalifies the contract; the contract observation above
    // (flatRuntimeObserve) is what requalifies, so it keeps running, and the
    // selector's own result is the exit predicate's positive witness.
    if (s.observing) { s.observingQualifiedHandoff = selected.selected(); return; }
    if(nonzeroPhase(s) && !s.phase.applied)failPhase(s,"no-raster-application");
    s.reason = flatMonoReasonName(selected.reason);
    // Close only the two sampled prefix frames against their actual copy
    // handoff. A pointer here is an identity within this frame, never retained
    // or dereferenced after the frame ends.
    if(s.projection && s.projectionFrames)for(uint32_t pair=0;pair<2;++pair) {
        auto& sample=s.localSamples[pair];
        for(uint32_t i=0;i<sample.attempts;++i)if(!sample.closed[i] && sample.frames[i]==s.prefix.frame) {
            sample.closed[i]=true;
            const void* toneInput=nullptr,*candidateHdr=nullptr;
            if(k.srvResource[0])for(uint32_t t=0;t<s.prefix.targetsUsed;++t)
                if(s.prefix.targets[t].resource==k.srvResource[0]) {
                    toneInput=s.prefix.targets[t].resource;
                    candidateHdr=flat_mono_detail::toneHdrInput(s.prefix.targets[t].tone.key);break;
                }
            Log::get().note("flat local projection handoff-model: pair=%u attempt=%u frame=%llu copy-seq=%u reason=%s sampled-rt=%p sampled-dsv-depth=%p tone-output=%p candidate-hdr=%p model-selected-hdr=%p model-selected-depth=%p rt-is-candidate=%u rt-is-selected=%u depth-is-selected=%u; actual copy handoff validation follows this observer decision",
                pair,i+1,(unsigned long long)s.prefix.frame,s.prefix.sequence,s.reason,
                sample.color[i],sample.depth[i],toneInput,candidateHdr,selected.hdr,selected.depth,
                sample.color[i] && sample.color[i]==candidateHdr?1u:0u,
                sample.color[i] && sample.color[i]==selected.hdr?1u:0u,
                sample.depth[i] && sample.depth[i]==selected.depth?1u:0u);
        }
    }
    if (!selected.selected()) {
        if (selected.reason == FlatMonoReason::ConflictingHdr) reportConflict(s);
        if(s.phase.applied)recover(s.reason);
        refuse(s); return;
    }
    nameSourceFree(s, selected);
    if (s.treated || selected.depth != s.namedDepth || selected.sceneConstants != s.namedConstants) {
        s.reason = s.treated ? "already-treated-this-frame" : "producer-source-identity-mismatch";
        if(!s.treated && s.phase.applied)recover(s.reason);
        refuse(s); return;
    }
    {
        // The weapon the model cannot see (an unsupported first-person pair: the plasma weapon) is known only to the domain, which planned
        // its draws into this frame's depth: either witness makes the frame mixed-camera, where the copy route judges the weapon.
        const auto* planned = domainCandidate(s, selected.depth);
        const bool modelMixed = selected.mixedCamera;
        selected.mixedCamera = flatCopyMixedCamera(modelMixed,
            flatWeaponRoute(s.hdrKey == FlatHdrKey::Auto, s.hdrLatch.tripped, s.engine, selected.renderWidth, selected.renderHeight,
                            selected.outputWidth, selected.outputHeight),
            planned ? planned->foreignPlanned : 0);
        if (selected.mixedCamera) { ++s.copyWeaponWindow.mixedFrames; if (!modelMixed) ++s.copyWeaponWindow.domainMixedFrames; }
    }
    FlatComputeInternalScope guard;
    flatcpu::Scope resolveScope(flatcpu::kResolve);   // the treatment: the handoff checks, the resolver, the sharpen pass
    // Verify the actual handoff once. Cached bindings only nominate this draw.
    Ptr<ID3D11RenderTargetView> actualRt; Ptr<ID3D11DepthStencilView> actualDs;
    ctx->OMGetRenderTargets(1, &actualRt, &actualDs); ctx->PSGetShaderResources(0, 1, &original);
    Ptr<ID3D11VertexShader> actualVs; Ptr<ID3D11PixelShader> actualPs; Ptr<ID3D11Buffer> actualB1;
    ctx->VSGetShader(&actualVs, nullptr, nullptr); ctx->PSGetShader(&actualPs, nullptr, nullptr); ctx->VSGetConstantBuffers(1, 1, &actualB1);
    UINT viewportCount = 1; D3D11_VIEWPORT viewport{}; ctx->RSGetViewports(&viewportCount, &viewport);
    Ptr<ID3D11Resource> actualColor, actualOut;
    if (original) original->GetResource(&actualColor); if (actualRt) actualRt->GetResource(&actualOut);
    if (actualColor.Get() != selected.color || actualOut.Get() != s.output.Get() || actualDs ||
        lookupShaderHash(actualVs.Get()) != k.vs || lookupShaderHash(actualPs.Get()) != k.ps || actualB1.Get() != k.b1 ||
        viewportCount != 1 || std::memcmp(&viewport, &s.viewport, sizeof(viewport)) != 0 ||
        !depthView(static_cast<ID3D11Texture2D*>(const_cast<void*>(selected.depth)))) {
        s.reason="actual-handoff-or-depth-view-refused";if(s.phase.applied)recover(s.reason);refuse(s);return;
    }
    FlatMonoResolveFrame f{}; f.color = original; f.depth = s.depthView.Get(); f.renderWidth = selected.renderWidth; f.renderHeight = selected.renderHeight;
    f.outputWidth = selected.outputWidth; f.outputHeight = selected.outputHeight; f.frame = s.prefix.frame; f.mode = s.engine;
    // The refusal census on the copy route too (render below the output: DLSS or FSR upscaling), as on the HDR route.
    f.refusalView = 0u;
    f.refusalCensus = s.refusalCensusFrames > 0;
    if (s.refusalCensusFrames) --s.refusalCensusFrames;
    nativeScale.store(f.renderWidth >= f.outputWidth && f.renderHeight >= f.outputHeight,
                      std::memory_order_release);
    f.configuredDlssPreset=s.preset;
    // The 3D main menu: this frame's contract came through the verified menu HDR copy (the acceptance
    // the "flat menu HDR copy" line counts). Nothing but the camera moves there, so a pixel whose engine
    // slot an unkeyed draw overdrew takes the camera term instead of refusing history. False for every
    // other frame, and the shader is then bit-identical to what it was before the field.
    f.staticScene=flatFrameThroughMenuCopy(s.prefix,selected.hdr);
    if(f.staticScene)++s.staticSceneFrames;
    f.steadyDetail=true;   // the depth-validated steady detail: always on, no key (the 3D menu's blanket rule above is separate and wins where it applies)
    f.mapPlane=flatRuntimeMapPlaneFrame();   // the System Map open: depth-0 pixels take the map plane's motion (flat_mono_resolve.h)
    // Metadata is frozen from the qualified handoff for a future frame's
    // preflight. It cannot authorize jitter in this already rendered frame.
    Ptr<ID3D11Texture2D> colorTexture;
    if(s.projection && SUCCEEDED(actualColor.As(&colorTexture))) {
        D3D11_TEXTURE2D_DESC colorDesc{},depthDesc{};D3D11_SHADER_RESOURCE_VIEW_DESC colorView{},depthViewDesc{};
        colorTexture->GetDesc(&colorDesc);s.sceneDepth->GetDesc(&depthDesc);
        original->GetDesc(&colorView);s.depthView->GetDesc(&depthViewDesc);
        FlatMonoResolvePreflight plan{};
        plan.renderWidth=f.renderWidth;plan.renderHeight=f.renderHeight;plan.outputWidth=f.outputWidth;plan.outputHeight=f.outputHeight;
        plan.mode=f.mode;plan.colorViewFormat=colorView.Format;plan.depthViewFormat=depthViewDesc.Format;
        plan.colorViewIsTexture2D=colorView.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D;
        plan.depthViewIsTexture2D=depthViewDesc.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D;
        if(plan.colorViewIsTexture2D) {
            plan.colorMostDetailedMip=colorView.Texture2D.MostDetailedMip;plan.colorViewMipLevels=colorView.Texture2D.MipLevels;
        }
        if(plan.depthViewIsTexture2D) {
            plan.depthMostDetailedMip=depthViewDesc.Texture2D.MostDetailedMip;plan.depthViewMipLevels=depthViewDesc.Texture2D.MipLevels;
        }
        plan.colorResourceMipLevels=colorDesc.MipLevels;plan.colorArraySize=colorDesc.ArraySize;plan.colorSampleCount=colorDesc.SampleDesc.Count;
        plan.depthResourceMipLevels=depthDesc.MipLevels;plan.depthArraySize=depthDesc.ArraySize;plan.depthSampleCount=depthDesc.SampleDesc.Count;
        // E is part of the plan identity (rc-since-rc2 review F5): the
        // standing negotiation answers for this contract, so the comparison
        // sees an E-only drift as a change too.
        flatNegotiatedEval(s.negotiatedEvalW, s.negotiatedEvalH, s.negotiatedMode,
            s.negotiatedRenderW, s.negotiatedRenderH, s.negotiatedOutputW, s.negotiatedOutputH,
            plan.mode, plan.renderWidth, plan.renderHeight, plan.outputWidth, plan.outputHeight,
            plan.evalWidth, plan.evalHeight);
        if(!s.haveResolvePlan || !sameResolvePlan(plan,s.plannedResolve)) {
            s.resolvePreflight={};s.resolvePreflightRetryMs=0;
            // Gate 2 discovery: the effective route, logged when the plan
            // changes (startup, extent or backend-mode change).
            const auto route = flatResolveRoute(plan.mode, plan.renderWidth, plan.renderHeight,
                                                plan.outputWidth, plan.outputHeight);
            Log::get().note("flat route: %s R=%ux%u E=%ux%u D=%ux%u%s", route.name,
                plan.renderWidth, plan.renderHeight, route.evalWidth, route.evalHeight,
                plan.outputWidth, plan.outputHeight, route.refused ? " (refused today)" : "");
            // Gate 2 step 4: negotiate the effective treatment with the vendor
            // on every contract change. The request is recorded above; this is
            // the vendor's answer -- serving mode and evaluation size, with an
            // under-floor input cut to the floor it reaches (the game's copy
            // upsamples the rest), never a silent TAA substitution.
            s.negotiatedEvalW = s.negotiatedEvalH = 0;
            s.negotiatedRenderW = s.negotiatedRenderH = 0;
            s.negotiatedOutputW = s.negotiatedOutputH = 0;
            if (plan.mode == FlatMonoResolveMode::Dlss && !route.refused &&
                (plan.renderWidth < plan.outputWidth || plan.renderHeight < plan.outputHeight)) {
                const char* why = nullptr;
                if (dlaaAvailable(s.device.Get(), &why)) {
                    DlssModeRange modes[kDlssModeCount];
                    if (dlssModeRanges(s.device.Get(), plan.outputWidth, plan.outputHeight, modes)) {
                        const auto neg = flatDlssNegotiate(modes, plan.renderWidth, plan.renderHeight,
                                                           plan.outputWidth, plan.outputHeight);
                        Log::get().note("flat route negotiation: dlss R=%ux%u D=%ux%u -> E=%ux%u mode=%s%s",
                            plan.renderWidth, plan.renderHeight, plan.outputWidth, plan.outputHeight,
                            neg.evalWidth, neg.evalHeight,
                            neg.served ? kDlssModeNames[static_cast<int>(neg.mode)] : "unserved",
                            neg.cut ? " (input under the floor; the game's copy upsamples the rest)" :
                            neg.served ? "" : " (the backend refusal path stands)");
                        if (neg.served) {
                            s.negotiatedEvalW = neg.evalWidth; s.negotiatedEvalH = neg.evalHeight;
                            s.negotiatedMode = plan.mode;
                            s.negotiatedRenderW = plan.renderWidth; s.negotiatedRenderH = plan.renderHeight;
                            s.negotiatedOutputW = plan.outputWidth; s.negotiatedOutputH = plan.outputHeight;
                        }
                    }
                }
            }
        }
        // The plan carries the same negotiated E the frame does (gate-2 review
        // F1): preflight allocates at E and the resolve's resource cache keys
        // on E. Computed AFTER the negotiation above (rc-since-rc2 review F5):
        // the first frame of an under-floor contract preflights at the cut E,
        // not the route default it would reallocate from next frame.
        flatNegotiatedEval(s.negotiatedEvalW, s.negotiatedEvalH, s.negotiatedMode,
            s.negotiatedRenderW, s.negotiatedRenderH, s.negotiatedOutputW, s.negotiatedOutputH,
            plan.mode, plan.renderWidth, plan.renderHeight, plan.outputWidth, plan.outputHeight,
            plan.evalWidth, plan.evalHeight);
        s.plannedResolve=plan;s.haveResolvePlan=true;
    }
    // The frame's override comes from the same post-negotiation state as the
    // plan's: an under-floor contract resolves at the cut E on its FIRST
    // frame (rc-since-rc2 review F5), never at the default that the backend
    // refuses and the next frame reallocates.
    flatNegotiatedEval(s.negotiatedEvalW, s.negotiatedEvalH, s.negotiatedMode,
        s.negotiatedRenderW, s.negotiatedRenderH, s.negotiatedOutputW, s.negotiatedOutputH,
        f.mode, f.renderWidth, f.renderHeight, f.outputWidth, f.outputHeight,
        f.evalWidth, f.evalHeight);
    std::memcpy(f.camera, selected.camera, sizeof(f.camera));
    const bool resetMissing = !s.havePrevious;
    const bool resetGap = s.havePrevious && s.previous.frame + 1 != selected.frame;
    const bool resetDepth = s.havePrevious && s.previous.depth != selected.depth;
    const bool resetColor = s.havePrevious && s.previous.color != selected.color;
    const bool resetExtent = s.havePrevious &&
        (s.previous.outputWidth != selected.outputWidth || s.previous.outputHeight != selected.outputHeight ||
         s.previous.renderWidth != selected.renderWidth || s.previous.renderHeight != selected.renderHeight);
    f.reset = resetMissing || resetGap || resetDepth || resetColor || resetExtent ||
        (s.phase.failed || !s.phase.previousAcceptedValid);
    f.jitterX=s.phase.currentX;f.jitterY=s.phase.currentY;
    f.previousJitterX=f.reset?f.jitterX:s.phase.previousX;
    f.previousJitterY=f.reset?f.jitterY:s.phase.previousY;
    std::memcpy(f.previousCamera, f.reset ? selected.camera : s.previous.camera, sizeof(f.previousCamera));
    noteCameraOrigin(s, f);
    // The phase the captured rows carry. Only the injector writes one into the
    // game's own upload, so only an Upstream frame in which an injection landed
    // has any; the resolver removes it from both frames' rows and from the
    // engine's scene snapshots before any reprojection (flat_camera_phase.h).
    // Every other route passes zero, and the shader is then bit-identical to
    // what it was before the field existed.
    const FlatCameraRoute cameraRoute=flatCameraInjectRoute();
    const FlatCameraRowsPhase rowsNow=flatCameraRowsPhase(cameraRoute,s.phase.applied,s.phase.currentX,s.phase.currentY);
    f.rowsJitterX=rowsNow.x;f.rowsJitterY=rowsNow.y;
    f.previousRowsJitterX=f.reset?rowsNow.x:s.previousRowsX;
    f.previousRowsJitterY=f.reset?rowsNow.y:s.previousRowsY;
    const auto now = GetTickCount64(); f.deltaMs = s.lastMs ? static_cast<float>(now - s.lastMs) : 16.667f;
    if(s.phase.needsSpatialFallback()) {s.reason="incomplete-jitter-frame";recover(s.reason);refuse(s);return;}
    // A scene with no pool-family draw has no views a draw made: they are made from nothing for the camera term
    // (engineVelocityPrepareSourceFree). A refusal there leaves the views unprepared, and the request below declines as ever.
    if (selected.sourceFree) {
        float sourceFreeRows[6][4]; std::memcpy(sourceFreeRows, selected.camera, sizeof(sourceFreeRows));
        engineVelocityPrepareSourceFree(ctx, static_cast<ID3D11Texture2D*>(const_cast<void*>(selected.depth)),
            static_cast<ID3D11Buffer*>(const_cast<void*>(selected.sceneConstants)), sourceFreeRows);
    }
    if (!engineVelocitySourceViews(static_cast<ID3D11Texture2D*>(const_cast<void*>(selected.depth)), &f.engine)) {
        s.reason="engine-source-not-ready";if(s.phase.applied)recover(s.reason);refuse(s);return;
    }
    Ptr<ID3D11ShaderResourceView> engineSlots, enginePool, outputView, engineSkin; Ptr<ID3D11Buffer> nowCb, prevCb;
    engineSlots.Attach(f.engine.slots); enginePool.Attach(f.engine.pool); nowCb.Attach(f.engine.sceneNow); prevCb.Attach(f.engine.scenePrev);
    engineSkin.Attach(f.engine.skin);   // (null in the flat profile, which has no target 7; released with the rest)
    // The weapon on the copy route (flatWeaponRoute == Copy, flat_copy_structure.h): a frame that is mixed-camera (the model's cohort
    // in it, or a first-person draw the domain planned: flatCopyMixedCamera, above) asks the same foreground contract the HDR route asks
    // at its trigger, here at the final copy, and the resolver takes a qualified map or refuses. EDVR's TAA is never asked (the call
    // returns with the field false), and a frame with no first-person draw is never mixed-camera, so it asks nothing.
    FlatForegroundMotion::Output foregroundOutput;
    foregroundContractAtH(s,ctx,selected,f,foregroundOutput);
    if (cameraRoute != FlatCameraRoute::Off) {
        ++s.rows.frames;
        if (rowsNow.x != 0.0f || rowsNow.y != 0.0f) ++s.rows.unjittered; else ++s.rows.zeroPhase;
        // The live proof of the row provenance: two consecutive frames' rows differ
        // by exactly the difference of the phases they are CLAIMED to carry,
        // whatever the camera did in between (c2_derive_test A8). A claim that
        // the rows carry a phase they do not (an injection that missed the camera
        // the scene reads, say) leaves the claimed difference behind. Counted,
        // and the first few named; the frame is still resolved -- this is the
        // evidence, not a gate.
        if (!f.reset) {
            const FlatCameraPairResult pair = flatCameraCheckRowPair(f.camera, f.rowsJitterX, f.rowsJitterY,
                f.previousCamera, f.previousRowsJitterX, f.previousRowsJitterY, f.renderWidth, f.renderHeight);
            s.rows.pairs.note(pair);
            if (pair.verdict == FlatCameraPairVerdict::Inconsistent && s.rowsMismatchLogged < 8) {
                ++s.rowsMismatchLogged;
                char text[320];
                flatCameraFormatRowsMismatch(text, sizeof(text), s.prefix.frame, f.rowsJitterX, f.rowsJitterY,
                    f.previousRowsJitterX, f.previousRowsJitterY, pair.maxError);
                Log::get().note("%s", text);
            }
        }
    }
    if (!flatMonoResolve(s.device.Get(), ctx, f, &outputView, &s.reason)) {
        const char* temporalReason=s.reason;
        const char* fallbackReason=nullptr;
        // The SDK may clobber context state before returning failure. The
        // resolver and spatial path each isolate/restore that state. Never
        // count spatial recovery as a temporal evaluation/history success.
        if(flatMonoResolveSpatialFallback(s.device.Get(),ctx,f,&outputView,&fallbackReason)) {
            refuse(s);++s.spatialFallbacks;
            ID3D11ShaderResourceView* fallback=outputView.Get();ctx->PSSetShaderResources(0,1,&fallback);replaced=true;s.treated=true;
            s.reason="spatial-fallback";
            if(s.spatialFallbacks<=4)Log::get().note("flat runtime fallback: temporal=%s output=spatial history=invalid raster-phase=(%.4g,%.4g)",temporalReason,f.jitterX,f.jitterY);
        } else {
            refuse(s);++s.spatialFallbackFailures;
            if(s.spatialFallbackFailures<=4)Log::get().note("flat runtime fallback refused: temporal=%s spatial=%s raster-phase=(%.4g,%.4g)",temporalReason,fallbackReason?fallbackReason:"unknown",f.jitterX,f.jitterY);
        }
        return;
    }
    s.reason = nonzeroPhase(s)?"treated-jittered":"treated-zero-jitter";
    // fix.render_sharpness: the resolve's output through the shared RCAS pass, before the game's own copy
    // samples it (flat_sharpen.h). The interface is drawn after that copy, so it is never sharpened. At 0,
    // after a refusal or for a view the pass cannot use this is the resolve's own view; never in place.
    ID3D11ShaderResourceView* replacement = flatSharpenView(ctx, outputView.Get()); ctx->PSSetShaderResources(0, 1, &replacement); replaced = true;
    s.previous = selected; s.previousColor = actualColor; s.havePrevious = s.treated = true; s.temporalAccepted=true;s.lastMs = now; ++s.accepted;
    s.previousRowsX = f.rowsJitterX; s.previousRowsY = f.rowsJitterY; // the phase the rows now stored in s.previous carry
    s.drawCapture.qualify(s.prefix.frame,selected.depth,selected.hdr,selected.renderWidth,selected.renderHeight);
    s.resetMissingWindow += resetMissing; s.resetGapWindow += resetGap; s.resetDepthWindow += resetDepth;
    s.resetColorWindow += resetColor; s.resetExtentWindow += resetExtent;
    if (f.reset) { ++s.acceptedResetWindow; s.streak = 1; }
    else { ++s.acceptedHistoryWindow; ++s.streak; }
    if (s.streak > s.longestStreak) s.longestStreak = s.streak;
}
bool FlatRuntimeDrawScope::recover(const char* temporalReason) {
    auto& s=state();FlatComputeInternalScope internal;
    failPhase(s,temporalReason);
    // Validate the copy independently of scene/camera selection: even when
    // selection refused, this is the colour the game's own copy would sample.
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11RenderTargetView> rt;Ptr<ID3D11DepthStencilView> ds;
    Ptr<ID3D11Resource> outResource,inResource;Ptr<ID3D11Texture2D> texture;
    ctx->VSGetShader(&vs,nullptr,nullptr);ctx->PSGetShader(&ps,nullptr,nullptr);
    ctx->OMGetRenderTargets(1,&rt,&ds);
    if(!original)ctx->PSGetShaderResources(0,1,&original);
    if(rt)rt->GetResource(&outResource);if(original)original->GetResource(&inResource);
    if(inResource)inResource.As(&texture);
    D3D11_TEXTURE2D_DESC desc{};if(texture)texture->GetDesc(&desc);
    UINT count=1;D3D11_VIEWPORT vp{};ctx->RSGetViewports(&count,&vp);
    const bool valid=lookupShaderHash(vs.Get())==flat_mono_detail::kCopyVs &&
        lookupShaderHash(ps.Get())==flat_mono_detail::kCopyPs && !ds && outResource.Get()==s.output.Get() &&
        original && texture && desc.Width==s.phaseWidth && desc.Height==s.phaseHeight &&
        count==1 && vp.TopLeftX==0 && vp.TopLeftY==0 && vp.Width==float(s.prefix.width) &&
        vp.Height==float(s.prefix.height) && vp.MinDepth==0 && vp.MaxDepth==1;
    FlatMonoResolveFrame f{};f.color=original;f.renderWidth=desc.Width;f.renderHeight=desc.Height;
    f.outputWidth=s.prefix.width;f.outputHeight=s.prefix.height;f.mode=s.engine;
    f.jitterX=s.phase.currentX;f.jitterY=s.phase.currentY;
    Ptr<ID3D11ShaderResourceView> outputView;const char* reason="unverified-copy-handoff";
    if(valid && flatMonoResolveSpatialFallback(s.device.Get(),ctx,f,&outputView,&reason)) {
        ++s.spatialFallbacks;s.treated=true;
        auto* replacement=outputView.Get();ctx->PSSetShaderResources(0,1,&replacement);replaced=true;
        if(s.spatialFallbacks<=8)Log::get().note("flat runtime early fallback: frame=%llu temporal=%s output=spatial phase=(%.5g,%.5g) history=invalid",
            (unsigned long long)s.prefix.frame,temporalReason,f.jitterX,f.jitterY);
        return true;
    }
    ++s.spatialFallbackFailures;
    if(s.spatialFallbackFailures<=8)Log::get().note("flat runtime early fallback refused: frame=%llu temporal=%s spatial=%s; next frame returns to zero phase",
        (unsigned long long)s.prefix.frame,temporalReason,reason?reason:"unknown");
    return false;
}
// The HDR route's treatment (flat_hdr_route.h, design section 81). The same checks as the copy route's, for the same
// reasons, then the resolve of H itself: `hdr` frames write the result back into H, so nothing is bound for the caller
// and the game's pass that reads H next sees the anti-aliased image with its own bindings untouched. A check that fails
// before anything is written DECLINES: the frame is the copy route's, exactly as if the key were off. A resolve the
// backend refuses is recovered by the spatial pixel shader into H (the jitter resampled away), as the copy route recovers
// its own, and counts as the refusal it is; if even that fails H is still the game's and the frame is declined.
void FlatRuntimeDrawScope::treatHdr(const FlatMonoFrame& selected, uint32_t srvSlot) {
    auto& s = state();
    const FlatMonoResolveMode effectiveMode=s.engine;
    // Crash-safe breadcrumbs (flat_hdr_crumbs.h, edvr_breadcrumbs.txt): the route took this frame. The first frames that
    // reach the resolver write a crumb before and after every step from here to the frame's Present, so a session that
    // ends inside the treatment names the step; after the third, this is one compare. They change nothing the route does.
    hdrCrumbAdmit(s.prefix.frame, flatMonoResolveModeName(effectiveMode));
    ++s.hdrWindow.steps.admitted;
    // The frame reaches the resolver at most once, whichever call takes it (the census counts it once, the crumbs number it).
    bool reachedCounted = false;
    const auto reach = [&](const char* step) {
        if (!reachedCounted) { reachedCounted = true; ++s.hdrWindow.steps.reached; }
        hdrCrumbReach(s.prefix.frame, flatMonoResolveModeName(effectiveMode), step);
    };
    const auto decline = [&](const char* why) {
        s.hdrWindow.lastVerdict = why; ++s.hdrWindow.declined;
        if (s.hdrFlightLines < 12) {
            ++s.hdrFlightLines;
            Log::get().note("flat hdr route: declined at frame=%llu seq=%u: %s (the copy route serves this frame)",
                (unsigned long long)s.prefix.frame, s.hdr.trigger.sequence, why);
        }
        hdrCrumbDeclined(why);
    };
    if (s.hdrLatch.tripped) { decline("latched-off"); return; }
    if (s.observing) { decline("returned-to-observation"); return; }
    if (s.treated) { decline("already-treated-this-frame"); return; }
    // A scene with no world (a loading or menu screen: only full-screen filters on its depth) has nothing the camera term can move, so
    // every pixel of its rotating hologram would reach the upscaler with zero motion and keep accumulating (the loading-screen ghost).
    // The copy route's own verdict on such a frame is no-scene: untreated, as at supersampling below 1.
    if (blankScene(selected)) {
        ++s.blankSceneDeclinedWindow; decline("blank-scene"); return;
    }
    if (overlayOpen(s) && foreignWork.load(std::memory_order_acquire)) {
        overlayFail(s,"overlay-foreign-mutation",selected.hdr);
        decline("overlay-foreign-mutation"); return;
    }
    nameSourceFree(s, selected);
    if (selected.depth != s.namedDepth || selected.sceneConstants != s.namedConstants) {
        decline("producer-source-identity-mismatch"); return;
    }
    if (!flatHdrRouteEvaluatesAtRender(effectiveMode, selected.renderWidth, selected.renderHeight,
                                       selected.outputWidth, selected.outputHeight)) {
        decline("route-does-not-evaluate-at-render-size"); return;
    }
    FlatComputeInternalScope guard;
    flatcpu::Scope resolveScope(flatcpu::kResolve);   // the treatment: the handoff checks, the resolver
    // Verify the actual binding once: the shadow only nominated this draw.
    Ptr<ID3D11ShaderResourceView> hdrView; ctx->PSGetShaderResources(srvSlot, 1, &hdrView);
    Ptr<ID3D11Resource> hdrResource; if (hdrView) hdrView->GetResource(&hdrResource);
    if (!hdrView || hdrResource.Get() != selected.hdr ||
        !depthView(static_cast<ID3D11Texture2D*>(const_cast<void*>(selected.depth)))) {
        decline("actual-hdr-binding-or-depth-view-refused"); return;
    }
    bool protectedOverlay=false;
    for(uint32_t i=0;i<s.prefix.targetsUsed;++i) {
        auto& target=s.prefix.targets[i];
        if(target.resource!=selected.hdr || !target.overlayOpen)continue;
        Ptr<ID3D11Texture2D> hdrTexture;
        if(FAILED(hdrResource.As(&hdrTexture)) ||
           !s.overlay.ready(s.prefix.frame,hdrTexture.Get())) {
            overlayFail(s,s.overlay.refusal()?s.overlay.refusal():"overlay-not-ready-at-consumer",selected.hdr);
            decline("overlay-not-ready-at-consumer");
            return;
        }
        // The protected suffix ends at this consumer. Later unrelated game
        // clears/copies are outside it; ordinary post-consumer HDR writes are
        // still guarded by the HDR route's existing write latch.
        if(!flatRuntimeOverlaySeal(s.prefix,selected.hdr)) {
            overlayFail(s,"overlay-seal-refused",selected.hdr);
            decline("overlay-seal-refused");
            return;
        }
        overlaySuffixActive.store(false,std::memory_order_release);
        flatTraceMark(s.traceRing,kFlatTraceEventOverlaySeal,selected.hdr);
        protectedOverlay=true;
        break;
    }
    if (nonzeroPhase(s) && !s.phase.applied) failPhase(s, "no-raster-application");
    s.reason = "hdr-route";
    // The spatial recovery, into H: the jitter resampled away, no history, no SDK. True when H now holds it.
    const auto recoverHdr = [&](const char* temporalReason, const FlatMonoResolveFrame& frame) {
        reach("spatial-recovery");   // the frame is the resolver's from here
        failPhase(s, temporalReason);
        FlatMonoResolveFrame sf{}; sf.color = frame.color; sf.hdr = true; sf.mode = frame.mode;
        sf.renderWidth = frame.renderWidth; sf.renderHeight = frame.renderHeight;
        sf.outputWidth = frame.outputWidth; sf.outputHeight = frame.outputHeight;
        sf.jitterX = s.phase.currentX; sf.jitterY = s.phase.currentY;
        Ptr<ID3D11ShaderResourceView> none; const char* why = nullptr;
        if (flatMonoResolveSpatialFallback(s.device.Get(), ctx, sf, &none, &why)) {
            ++s.spatialFallbacks; s.treated = s.hdrTreated = true;
            s.hdrWindow.lastVerdict = "spatial-fallback";
            if (s.spatialFallbacks <= 8)
                Log::get().note("flat runtime early fallback: frame=%llu temporal=%s output=spatial (HDR route, into the scene target) phase=(%.5g,%.5g) history=invalid",
                    (unsigned long long)s.prefix.frame, temporalReason, sf.jitterX, sf.jitterY);
            return true;
        }
        ++s.spatialFallbackFailures;
        if (s.spatialFallbackFailures <= 8)
            Log::get().note("flat runtime early fallback refused: frame=%llu temporal=%s spatial=%s (HDR route); the copy route serves the frame",
                (unsigned long long)s.prefix.frame, temporalReason, why ? why : "unknown");
        return false;
    };
    FlatMonoResolveFrame f{}; f.color = hdrView.Get(); f.depth = s.depthView.Get(); f.hdr = true;
    f.refusalView = 0u;
    f.refusalCensus = s.refusalCensusFrames > 0;
    if (s.refusalCensusFrames) --s.refusalCensusFrames;
    if(selected.mixedCamera) {
        f.untrustedCameraCoverage=s.untrusted.view();
        if(!f.untrustedCameraCoverage) {
            decline("untrusted-camera-coverage-unavailable");return;
        }
    }
    if(protectedOverlay) {
        f.cleanColor=s.overlay.cleanHdrView();
        f.overlayCoverage=s.overlay.coverageView();
    }
    f.renderWidth = selected.renderWidth; f.renderHeight = selected.renderHeight;
    f.outputWidth = selected.outputWidth; f.outputHeight = selected.outputHeight;
    f.frame = s.prefix.frame; f.mode = effectiveMode;
    nativeScale.store(true, std::memory_order_release);   // the route's gate is R >= D
    f.configuredDlssPreset = s.preset;
    f.staticScene = flatFrameThroughMenuCopy(s.prefix, selected.hdr);
    if (f.staticScene) ++s.staticSceneFrames;
    f.steadyDetail = true;   // the depth-validated steady detail: always on, no key (the 3D menu's blanket rule above is separate and wins where it applies)
    f.mapPlane = flatRuntimeMapPlaneFrame();   // the System Map open: depth-0 pixels take the map plane's motion (flat_mono_resolve.h)
    // The plan, frozen from the qualified trigger for the next frame's preflight, as the copy route freezes its own.
    Ptr<ID3D11Texture2D> colorTexture;
    if (s.projection && SUCCEEDED(hdrResource.As(&colorTexture))) {
        D3D11_TEXTURE2D_DESC colorDesc{}, depthDesc{}; D3D11_SHADER_RESOURCE_VIEW_DESC colorViewDesc{}, depthViewDesc{};
        colorTexture->GetDesc(&colorDesc); s.sceneDepth->GetDesc(&depthDesc);
        hdrView->GetDesc(&colorViewDesc); s.depthView->GetDesc(&depthViewDesc);
        FlatMonoResolvePreflight plan{};
        plan.renderWidth = f.renderWidth; plan.renderHeight = f.renderHeight;
        plan.outputWidth = f.outputWidth; plan.outputHeight = f.outputHeight;
        plan.mode = f.mode; plan.hdr = true;
        plan.colorViewFormat = colorViewDesc.Format; plan.depthViewFormat = depthViewDesc.Format;
        plan.colorViewIsTexture2D = colorViewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
        plan.depthViewIsTexture2D = depthViewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
        if (plan.colorViewIsTexture2D) {
            plan.colorMostDetailedMip = colorViewDesc.Texture2D.MostDetailedMip; plan.colorViewMipLevels = colorViewDesc.Texture2D.MipLevels;
        }
        if (plan.depthViewIsTexture2D) {
            plan.depthMostDetailedMip = depthViewDesc.Texture2D.MostDetailedMip; plan.depthViewMipLevels = depthViewDesc.Texture2D.MipLevels;
        }
        plan.colorResourceMipLevels = colorDesc.MipLevels; plan.colorArraySize = colorDesc.ArraySize; plan.colorSampleCount = colorDesc.SampleDesc.Count;
        plan.depthResourceMipLevels = depthDesc.MipLevels; plan.depthArraySize = depthDesc.ArraySize; plan.depthSampleCount = depthDesc.SampleDesc.Count;
        if (!s.haveResolvePlan || !sameResolvePlan(plan, s.plannedResolve)) {
            s.resolvePreflight = {}; s.resolvePreflightRetryMs = 0;
            const auto route = flatResolveRoute(plan.mode, plan.renderWidth, plan.renderHeight, plan.outputWidth, plan.outputHeight);
            Log::get().note("flat route: %s R=%ux%u E=%ux%u D=%ux%u (HDR route: H is resolved before the game's post chain)", route.name,
                plan.renderWidth, plan.renderHeight, route.evalWidth, route.evalHeight, plan.outputWidth, plan.outputHeight);
            // No standing negotiation applies to a route that evaluates at the render size (R >= D).
            s.negotiatedEvalW = s.negotiatedEvalH = 0; s.negotiatedRenderW = s.negotiatedRenderH = 0;
            s.negotiatedOutputW = s.negotiatedOutputH = 0;
        }
        s.plannedResolve = plan; s.haveResolvePlan = true;
    }
    std::memcpy(f.camera, selected.camera, sizeof(f.camera));
    const bool resetMissing = !s.havePrevious;
    const bool resetGap = s.havePrevious && s.previous.frame + 1 != selected.frame;
    const bool resetDepth = s.havePrevious && s.previous.depth != selected.depth;
    const bool resetColor = s.havePrevious && s.previous.color != selected.color;
    const bool resetExtent = s.havePrevious &&
        (s.previous.outputWidth != selected.outputWidth || s.previous.outputHeight != selected.outputHeight ||
         s.previous.renderWidth != selected.renderWidth || s.previous.renderHeight != selected.renderHeight);
    f.reset = resetMissing || resetGap || resetDepth || resetColor || resetExtent ||
        (s.phase.failed || !s.phase.previousAcceptedValid);
    f.jitterX = s.phase.currentX; f.jitterY = s.phase.currentY;
    f.previousJitterX = f.reset ? f.jitterX : s.phase.previousX;
    f.previousJitterY = f.reset ? f.jitterY : s.phase.previousY;
    std::memcpy(f.previousCamera, f.reset ? selected.camera : s.previous.camera, sizeof(f.previousCamera));
    noteCameraOrigin(s, f);
    const FlatCameraRoute cameraRoute = flatCameraInjectRoute();
    const FlatCameraRowsPhase rowsNow = flatCameraRowsPhase(cameraRoute, s.phase.applied, s.phase.currentX, s.phase.currentY);
    f.rowsJitterX = rowsNow.x; f.rowsJitterY = rowsNow.y;
    f.previousRowsJitterX = f.reset ? rowsNow.x : s.previousRowsX;
    f.previousRowsJitterY = f.reset ? rowsNow.y : s.previousRowsY;
    const auto now = GetTickCount64(); f.deltaMs = s.lastMs ? static_cast<float>(now - s.lastMs) : 16.667f;
    if (s.phase.needsSpatialFallback()) {
        s.reason = "incomplete-jitter-frame";
        if (recoverHdr(s.reason, f)) { refuse(s); } else decline("incomplete-jitter-frame");
        return;
    }
    // A scene with no pool-family draw has no views a draw made: they are made from nothing for the camera term
    // (engineVelocityPrepareSourceFree). A refusal there leaves the views unprepared, and the request below declines as ever.
    if (selected.sourceFree) {
        float sourceFreeRows[6][4]; std::memcpy(sourceFreeRows, selected.camera, sizeof(sourceFreeRows));
        engineVelocityPrepareSourceFree(ctx, static_cast<ID3D11Texture2D*>(const_cast<void*>(selected.depth)),
            static_cast<ID3D11Buffer*>(const_cast<void*>(selected.sceneConstants)), sourceFreeRows);
    }
    if (!engineVelocitySourceViews(static_cast<ID3D11Texture2D*>(const_cast<void*>(selected.depth)), &f.engine)) {
        s.reason = "engine-source-not-ready";
        if (s.phase.applied && recoverHdr(s.reason, f)) refuse(s); else decline("engine-source-not-ready");
        return;
    }
    Ptr<ID3D11ShaderResourceView> engineSlots, enginePool, outputView, engineSkin; Ptr<ID3D11Buffer> nowCb, prevCb;
    engineSlots.Attach(f.engine.slots); enginePool.Attach(f.engine.pool); nowCb.Attach(f.engine.sceneNow); prevCb.Attach(f.engine.scenePrev);
    engineSkin.Attach(f.engine.skin);   // (null in the flat profile, which has no target 7; released with the rest)
    // The foreground contract of a mixed-camera frame (the copy route's final copy asks the same, below its engine source views).
    FlatForegroundMotion::Output foregroundOutput;
    foregroundContractAtH(s,ctx,selected,f,foregroundOutput);
    if (cameraRoute != FlatCameraRoute::Off) {
        ++s.rows.frames;
        if (rowsNow.x != 0.0f || rowsNow.y != 0.0f) ++s.rows.unjittered; else ++s.rows.zeroPhase;
        if (!f.reset) {
            const FlatCameraPairResult pair = flatCameraCheckRowPair(f.camera, f.rowsJitterX, f.rowsJitterY,
                f.previousCamera, f.previousRowsJitterX, f.previousRowsJitterY, f.renderWidth, f.renderHeight);
            s.rows.pairs.note(pair);
            if (pair.verdict == FlatCameraPairVerdict::Inconsistent && s.rowsMismatchLogged < 8) {
                ++s.rowsMismatchLogged;
                char text[320];
                flatCameraFormatRowsMismatch(text, sizeof(text), s.prefix.frame, f.rowsJitterX, f.rowsJitterY,
                    f.previousRowsJitterX, f.previousRowsJitterY, pair.maxError);
                Log::get().note("%s", text);
            }
        }
    }
    reach("resolve");   // the frame is the resolver's from here
    if (!flatMonoResolve(s.device.Get(), ctx, f, &outputView, &s.reason)) {
        const char* temporalReason = s.reason;
        // The backend (or the route's own guard) refused before H was written. Recover the jitter into H, or decline.
        if (recoverHdr(temporalReason, f)) { refuse(s); s.reason = "spatial-fallback"; }
        else { reset(); decline(temporalReason ? temporalReason : "resolve-refused"); }
        return;
    }
    s.reason = nonzeroPhase(s) ? "treated-jittered-hdr" : "treated-zero-jitter-hdr";
    if(protectedOverlay) ++s.overlayIsolatedWindow;
    if(selected.mixedCamera) {
        ++s.untrustedTreated;
        s.untrusted.sampleMask(ctx,s.untrustedSupportedAlternate);
    }
    s.previous = selected; s.previousColor = hdrResource; s.havePrevious = s.treated = s.hdrTreated = s.temporalAccepted = true;
    s.lastMs = now; ++s.accepted; ++s.hdrWindow.treated; s.hdrWindow.lastVerdict = s.reason;
    s.previousRowsX = f.rowsJitterX; s.previousRowsY = f.rowsJitterY;
    s.drawCapture.qualify(s.prefix.frame, selected.depth, selected.hdr, selected.renderWidth, selected.renderHeight);
    s.resetMissingWindow += resetMissing; s.resetGapWindow += resetGap; s.resetDepthWindow += resetDepth;
    s.resetColorWindow += resetColor; s.resetExtentWindow += resetExtent;
    if (f.reset) { ++s.acceptedResetWindow; s.streak = 1; }
    else { ++s.acceptedHistoryWindow; ++s.streak; }
    if (s.streak > s.longestStreak) s.longestStreak = s.streak;
}
static const char* replayDrawGuard(ID3D11DeviceContext* ctx, const FlatRuntimeDrawScope& draw,
                                   ID3D11Buffer* indirectArgs) {
    auto& s=state();
    if(!owner() || ctx!=s.context.Get() || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return "replay-foreign-or-deferred-context";
    if(foreignWork.load(std::memory_order_acquire) || s.prefix.uncertain)
        return "replay-foreign-or-uncertain-state";
    if(draw.foregroundStarted || draw.untrustedStarted)
        return "replay-other-private-draw-active";
    if(indirectArgs || (draw.weaponDrawKind!='D' && draw.weaponDrawKind!='I' &&
                        draw.weaponDrawKind!='N' && draw.weaponDrawKind!='X') ||
       !draw.weaponDrawCount || !draw.weaponDrawInstances)
        return "replay-unsupported-draw-kind-or-arguments";
    if(!replayQueriesSafe(ctx))return "replay-active-or-uncertain-query";
    ID3D11Buffer* streams[D3D11_SO_BUFFER_SLOT_COUNT]{};
    ctx->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT,streams);
    bool streamBound=false;
    for(auto* stream:streams) if(stream) { streamBound=true; stream->Release(); }
    if(streamBound) return "replay-stream-output-bound";
    return nullptr;
}
static void issueExactReplayDraw(ID3D11DeviceContext* ctx,const FlatRuntimeDrawScope& draw) {
    const UINT count=draw.weaponDrawCount, instances=draw.weaponDrawInstances;
    switch(draw.weaponDrawKind) {
    case 'D': ctx->Draw(count,static_cast<UINT>(draw.weaponDrawBase)); break;
    case 'I': ctx->DrawIndexed(count,draw.weaponDrawStart,draw.weaponDrawBase); break;
    case 'N': ctx->DrawInstanced(count,instances,static_cast<UINT>(draw.weaponDrawBase),
                                 draw.weaponDrawStartInstance); break;
    case 'X': ctx->DrawIndexedInstanced(count,instances,draw.weaponDrawStart,
                                       draw.weaponDrawBase,draw.weaponDrawStartInstance); break;
    }
}
static void __stdcall foregroundOriginalDraw(ID3D11DeviceContext* ctx,UINT count,UINT instances,
    UINT start,INT base,UINT startInstance) {
    ctx->DrawIndexedInstanced(count,instances,start,base,startInstance);
}
void FlatRuntimeDrawScope::beginActualDraw(ID3D11Buffer* indirectArgs,UINT indirectOffset) {
    if(!ctx)return;
    if(drawPacketOnly){FlatComputeInternalScope internal;state().drawPackets.execution(ctx,drawPacket,indirectArgs,indirectOffset,"capture-only-AA-off-or-paused");return;}
    // fix.ui_quality's flat layer (flat_ui_layer.h), right before the game's issue: a taken HUD draw is bound into the
    // layer (false: it goes to H as always), and the output copy gets this frame's HUD composited over what it reads.
    if(uiTake) {
        uiTaken=flatUiLayerBegin(ctx);
        flatUiLayerNoteIssue(uiVs,uiPs,uiTaken);
    }
    if(uiComposite) {
        auto& s=state();
        flatUiLayerAtCopy(ctx,s.prefix.frame,s.treated||s.hdrTreated,s.prefix.width,s.prefix.height,&original,&replaced);
    }
    if(domainPlanned && !producer) {
        FlatComputeInternalScope internal;flatcpu::Scope captureCost(flatcpu::kForegroundCapture);
        Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;
        ctx->VSGetShader(&vs,nullptr,nullptr);ctx->PSGetShader(&ps,nullptr,nullptr);
        if(lookupShaderHash(vs.Get())!=domainVs || lookupShaderHash(ps.Get())!=domainPs)
            if(auto* candidate=domainCandidate(state(),domainDepth))
                candidate->motion.fail("foreground-original-shader-pair-mismatch");
    }
    if(untrustedPlanned) {
        untrustedStarted=state().untrusted.beginDraw(ctx,state().prefix.frame,
            [](void* shader) { return lookupShaderHash(shader); });
    }
    if(domainPlanned) {
        auto& s=state();FlatComputeInternalScope internal;
        flatcpu::Scope captureCost(flatcpu::kForegroundCapture);
        auto* candidate=domainCandidate(s,domainDepth);
        if(!candidate)return;
        FlatContractObservation failureKey{};failureKey.vs=domainVs;failureKey.ps=domainPs;
        failureKey.cameraHash=domainCameraHash;failureKey.format=domainFormat;failureKey.depth=domainDepth;
        const auto& projection=domainShaderProof(s,domainVs,domainPs);
        Ptr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT sampleMask=0;
        ctx->OMGetBlendState(&blend,factors,&sampleMask);D3D11_BLEND_DESC bd{};
        if(blend)blend->GetDesc(&bd);else for(auto& rt:bd.RenderTarget)rt.RenderTargetWriteMask=15;
        unsigned boundColors=0;ID3D11RenderTargetView* colors[6]{};ctx->OMGetRenderTargets(6,colors,nullptr);
        std::array<Ptr<ID3D11RenderTargetView>,6> originalColors{};
        // Which colour targets a first-person draw writes does not decide who
        // owns a pixel (section 104); the scene's HDR target is itself a
        // Gbuffer slot, so every forward HDR draw also "writes the Gbuffer".
        for(unsigned i=0;i<6;++i)if(colors[i]) {
            boundColors|=1u<<i;
            originalColors[i].Attach(colors[i]);
        }
        const uint8_t* vsBytes=nullptr;const uint8_t* psBytes=nullptr;size_t vsSize=0,psSize=0;
        const bool bytes=flatProbeShaderLookup('v',domainVs,&vsBytes,&vsSize) &&
            (!domainPs || flatProbeShaderLookup('p',domainPs,&psBytes,&psSize));
        Ptr<ID3D11DepthStencilState> depthState;UINT stencilRef=0;
        ctx->OMGetDepthStencilState(&depthState,&stencilRef);D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
        if(depthState)depthState->GetDesc(&dd);
        EdvrFlatForegroundStateReceipt stateReceipt{};
        auto readStateReceipt=[&]{
            if(!stateReceipt.valid) {
                stateReceipt.valid=1;stateReceipt.boundColors=boundColors;
                stateReceipt.independentBlend=bd.IndependentBlendEnable?1u:0u;
                stateReceipt.alphaToCoverage=bd.AlphaToCoverageEnable?1u:0u;
                stateReceipt.sampleMask=sampleMask;stateReceipt.depthEnable=dd.DepthEnable?1u:0u;
                stateReceipt.depthWriteMask=dd.DepthWriteMask;
                stateReceipt.depthFunc=depthState?dd.DepthFunc:D3D11_COMPARISON_LESS;
                stateReceipt.stencilEnable=dd.StencilEnable?1u:0u;
                stateReceipt.stencilReadMask=depthState?dd.StencilReadMask:D3D11_DEFAULT_STENCIL_READ_MASK;
                stateReceipt.stencilWriteMask=depthState?dd.StencilWriteMask:D3D11_DEFAULT_STENCIL_WRITE_MASK;
                stateReceipt.stencilRef=stencilRef;
                const auto face=[&](EdvrFlatForegroundStateReceipt::Face& out,
                    const D3D11_DEPTH_STENCILOP_DESC& in) {
                    out.func=in.StencilFunc;out.fail=in.StencilFailOp;
                    out.depthFail=in.StencilDepthFailOp;out.pass=in.StencilPassOp;
                };
                if(depthState) {face(stateReceipt.front,dd.FrontFace);face(stateReceipt.back,dd.BackFace);}
                else {
                    D3D11_DEPTH_STENCILOP_DESC defaults{};
                    defaults.StencilFunc=D3D11_COMPARISON_ALWAYS;
                    defaults.StencilFailOp=D3D11_STENCIL_OP_KEEP;
                    defaults.StencilDepthFailOp=D3D11_STENCIL_OP_KEEP;
                    defaults.StencilPassOp=D3D11_STENCIL_OP_KEEP;
                    face(stateReceipt.front,defaults);face(stateReceipt.back,defaults);
                }
                stateReceipt.foreign=domainForeign?1u:0u;stateReceipt.hdr=domainHdrWriter?1u:0u;
                stateReceipt.worldNamed=s.namedDepth?1u:0u;stateReceipt.sameWorld=domainSameWorld?1u:0u;
                stateReceipt.namedWorldQ=s.namedWorldQ;
                for(unsigned i=0;i<6;++i) {
                    const auto& rt=bd.RenderTarget[bd.IndependentBlendEnable?i:0];
                    auto& slot=stateReceipt.slot[i];
                    slot.componentMask=projection.colorComponents[i];
                    slot.effectiveWriteMask=rt.RenderTargetWriteMask;
                    if(originalColors[i]) {
                        D3D11_RENDER_TARGET_VIEW_DESC vd{};originalColors[i]->GetDesc(&vd);
                        slot.viewFormatValid=1;slot.viewFormat=vd.Format;
                    }
                    slot.blendEnable=rt.BlendEnable?1u:0u;
                    slot.src=blend?rt.SrcBlend:D3D11_BLEND_ONE;
                    slot.dst=blend?rt.DestBlend:D3D11_BLEND_ZERO;
                    slot.op=blend?rt.BlendOp:D3D11_BLEND_OP_ADD;
                    slot.srcAlpha=blend?rt.SrcBlendAlpha:D3D11_BLEND_ONE;
                    slot.dstAlpha=blend?rt.DestBlendAlpha:D3D11_BLEND_ZERO;
                    slot.opAlpha=blend?rt.BlendOpAlpha:D3D11_BLEND_OP_ADD;
                }
            }
        };
        auto failDomain=[&](const char* stage,const char* why){
            if(candidate->firstFailure.frame!=s.prefix.frame)readStateReceipt();
            const auto& budget=candidate->motion.budgetReceipt();
            domainFail(s,stage,why,failureKey,stateReceipt.valid?&stateReceipt:nullptr,
                why && std::strcmp(why,"history-budget")==0 && budget.valid?&budget:nullptr);
        };
        const char* rasterRefusal=flatDomainRasterRefusal(projection,bd,dd,boundColors,domainForeign,domainHdrWriter,candidate->colorWritten);
        const bool opaque=rasterRefusal==nullptr;
        const bool colorWrites=flatDomainWritesColor(projection,bd,boundColors);
        if(domainPs && colorWrites)candidate->colorWritten=true;
        const bool queriesUnsafe=opaque && bytes && !replayQueriesSafe(ctx);
        const bool unplannable=!opaque || !bytes || queriesUnsafe || indirectArgs;
        if(unplannable && flatDomainPreservesSurface(dd)) {
            // No depth write: the last depth writer's surface and owner mark
            // stand. No capture, marker or refusal; the game's draw runs as is.
            ++s.foregroundCounts.surfacePreserving;
            if(domainForeign)++s.foregroundCounts.surfacePreservingForeign;
        } else if(unplannable) {
            failDomain("state",!opaque?rasterRefusal:!bytes?
                "foreground-original-shader-unavailable":indirectArgs?"foreground-indirect-writer":"foreground-active-query");
        } else {
            const char* phaseReason=nullptr;
            const char* deferredCapture=nullptr;   // a capture refusal the owner mark below may turn into a per-pixel refusal
            const bool phaseKnown=flatForegroundBoundProjectionPhase(ctx,s.projection.get(),projection.projectionSlot,
                projection.projectionRow,projection.projectionLayout,s.phase.currentX,s.phase.currentY,
                domainWidth,domainHeight,domainCamera[3][2],&phaseReason);
            if(!phaseKnown)failDomain("projection-phase",phaseReason);
            if(domainForeign && domainPool && phaseKnown) {
                FlatForegroundMotion::Inputs inputs{};std::memcpy(inputs.camera,domainCamera,sizeof(inputs.camera));
                inputs.phaseX=s.phase.currentX;inputs.phaseY=s.phase.currentY;
                inputs.writerToken=domainWriterToken;
                inputs.gpuIdentity=true;
                inputs.beforeWorld=domainBeforeWorld;
                inputs.vs=domainVs;inputs.ps=domainPs;
                Ptr<ID3D11VertexShader> originalVs;ctx->VSGetShader(&originalVs,nullptr,nullptr);
                if(originalVs)AnimatedVertexHistory::rememberShader(originalVs.Get(),vsBytes,vsSize);
                if(weaponDrawKind!='X')failDomain("capture","foreground-draw-kind");
                else if(candidate->motion.capture(ctx,&foregroundOriginalDraw,weaponDrawCount,weaponDrawInstances,weaponDrawStart,weaponDrawBase,
                    weaponDrawStartInstance,static_cast<unsigned>(s.prefix.frame),inputs))++s.foregroundCounts.captured;
                // False with no draw refusal is a capture that succeeded under a frame refusal already standing: nothing more to say.
                else deferredCapture=candidate->motion.drawRefusal();
            }
            if(!producer) {
                const char* reason=nullptr;
                domainStarted=engineVelocityFlatDomainBeginDraw(ctx,domainDepth,vsBytes,vsSize,psBytes,psSize,
                    domainForeign?FlatEngineDomain::ForeignPool:domainPool?FlatEngineDomain::WorldPool:FlatEngineDomain::World,&reason,untrustedStarted,
                    domainWriterToken,weaponDrawCount/3);
                if(!domainStarted)failDomain("marker",reason);
                if(!domainStarted)++s.foregroundCounts.markerRefused;
                else if(!domainPs)++s.foregroundCounts.nullMarkers;
                else if(!domainForeign)++s.foregroundCounts.worldMarkers;
            } else if(domainForeign)failDomain("marker","foreground-foreign-old-producer");
            else if(gameHadTarget6)candidate->motion.fail("foreground-native-MRT6-conflict");
            if(deferredCapture) {
                // The draw is not in the map. Its pixels are the frame's loss only if nothing marks them: with the owner mark started
                // (the marker writes the draw's first-person mark where its fragments pass) the prep reads each of them as first
                // person with no sample and refuses its history, with no motion (the camera term never reaches a marked pixel). A
                // marker that did not start (or ends abandoned: foreground-abandoned-writer) leaves the frame refused, as it always was.
                if(domainStarted) {
                    candidate->motion.coverDraw(deferredCapture);
                    if(candidate->firstCovered.frame!=s.prefix.frame)readStateReceipt();
                    const auto& budget=candidate->motion.budgetReceipt();
                    domainCovered(s,deferredCapture,failureKey,stateReceipt.valid?&stateReceipt:nullptr,
                        std::strcmp(deferredCapture,"history-budget")==0 && budget.valid?&budget:nullptr);
                } else failDomain("history",deferredCapture);
            }
        }
    }
    if(foregroundPlanned) foregroundStarted=state().foreground.beginDraw(ctx,state().prefix.frame);
    if(overlayPlanned) {
        auto& s=state();
        if(overlayStarted) overlayFail(s,"overlay-duplicate-begin",overlayHdr);
        else {
            const char* reason=nullptr;
            overlayStarted=s.overlay.beginDraw(ctx,s.prefix.frame,overlayHdr,overlayDsv,&reason,
                false,false,false,&s.overlayBlendDiagnostic);
            auto& sample=s.overlayBlendDiagnostic;
            if(sample.captured && !sample.sequence && sample.frame==s.prefix.frame) {
                sample.sequence=s.prefix.sequence;
                sample.vsHash=sample.vs?lookupShaderHash(const_cast<void*>(sample.vs)):0;
                sample.psHash=sample.ps?lookupShaderHash(const_cast<void*>(sample.ps)):0;
                // Read while the sampled original object is still live. No
                // delayed dereference of the diagnostic's raw COM pointer.
                sample.shader=FlatOverlayLayer::diagnosePixelShader(
                    static_cast<ID3D11PixelShader*>(const_cast<void*>(sample.ps)));
                const uint8_t* raw=nullptr;
                sample.rawVsAvailable=flatProbeShaderLookup('v',sample.vsHash,&raw,&sample.rawVsBytes);
                sample.rawPsAvailable=flatProbeShaderLookup('p',sample.psHash,&raw,&sample.rawPsBytes);
            }
            if(!overlayStarted && reason && std::strcmp(reason,"dual-source-blend")==0) {
                ++s.overlayReplayCandidates;
                // Snapshot before any replay binding work, including a failed
                // restore. Hashes and objects name the actual refused original.
                Ptr<ID3D11VertexShader> refusedVs;Ptr<ID3D11PixelShader> refusedPs;
                ctx->VSGetShader(&refusedVs,nullptr,nullptr);
                ctx->PSGetShader(&refusedPs,nullptr,nullptr);
                const auto pair=std::make_pair(lookupShaderHash(refusedVs.Get()),lookupShaderHash(refusedPs.Get()));
                const char* replayWhy=replayDrawGuard(ctx,*this,indirectArgs);
                if(!replayWhy) {
                    FlatComputeInternalScope internal;
                    // Begin observes before forwarding to D3D. Hold its gate
                    // until private replay/restoration completes, closing the
                    // race between the first safe check and a concurrent Begin.
                    auto& observer=replayQueryObserver();
                    std::unique_lock<std::mutex> queryGate(observer.mutex);
                    auto* queries=observer.find(ctx);
                    if(observer.overflow || !queries || !queries->tracker.safe())
                        replayWhy="replay-active-or-uncertain-query";
                    else {
                        overlayStarted=s.overlay.beginReplayDraw(ctx,s.prefix.frame,overlayHdr,overlayDsv,&replayWhy);
                        if(overlayStarted) {
                            issueExactReplayDraw(ctx,*this);
                            const bool restored=s.overlay.finishReplayDraw(ctx);
                            overlayStarted=false;
                            overlayReplayPending=restored;
                            if(!restored)replayWhy=s.overlay.refusal()?s.overlay.refusal():"replay-restore-failed";
                        }
                    }
                }
                if(!overlayReplayPending) {
                    const char* why=replayWhy?replayWhy:"replay-private-draw-refused";
                    if(s.drawPacketRefusedPairs.size()<64 || s.drawPacketRefusedPairs.count(pair))s.drawPacketRefusedPairs[pair]=s.prefix.frame;
                    // The window's first blend sample may name a different
                    // successful replay. Export this refusal's live pair.
                    auto found=s.overlayReplayShaderCaptures.find(pair);
                    if(found==s.overlayReplayShaderCaptures.end() &&
                       s.overlayReplayShaderCaptures.size()<64) {
                        State::ReplayShaderCapture capture;
                        capture.vsSaved=pair.first && captureFlatProbeShader('v',pair.first);
                        capture.psSaved=pair.second && captureFlatProbeShader('p',pair.second);
                        const uint8_t* raw=nullptr;size_t rawBytes=0;
                        const bool rawFound=flatProbeShaderLookup('p',pair.second,&raw,&rawBytes);
                        if(rawFound) {
                            std::vector<BYTE> patched;
                            capture.qualified=flatOverlayPatchPs(raw,rawBytes,patched,capture.reason);
                            if(capture.qualified)capture.reason="bytecode-patchable";
                        } else capture.reason="creation-bytes-missing";
                        const auto live=FlatOverlayLayer::diagnosePixelShader(refusedPs.Get());
                        Log::get().note("flat late overlay refused shader capture: frame=%llu q=%u actual-VS=%016llX actual-PS=%016llX eligible=%u previously-created=%u registry-reason=%s raw-PS-available=%u raw-PS-bytes=%zu raw-PS-qualified=%u raw-PS-reason=%s VS-saved=%u PS-saved=%u replay-reason=%s",
                            (unsigned long long)s.prefix.frame,s.prefix.sequence,
                            (unsigned long long)pair.first,(unsigned long long)pair.second,
                            live.eligible?1u:0u,live.created?1u:0u,live.reason.c_str(),
                            rawFound?1u:0u,rawBytes,capture.qualified?1u:0u,capture.reason.c_str(),
                            capture.vsSaved?1u:0u,capture.psSaved?1u:0u,why);
                        found=s.overlayReplayShaderCaptures.emplace(pair,std::move(capture)).first;
                    }
                    if(found==s.overlayReplayShaderCaptures.end() && !s.overlayReplayShaderCaptureCapReported) {
                        s.overlayReplayShaderCaptureCapReported=true;
                        Log::get().note("flat late overlay refused shader capture: frame=%llu q=%u actual-VS=%016llX actual-PS=%016llX status=diagnostic-unique-pair-cap-64; no export or raw preflight",
                            (unsigned long long)s.prefix.frame,s.prefix.sequence,
                            (unsigned long long)pair.first,(unsigned long long)pair.second);
                    }
                    const bool sampledPair=sample.captured && sample.vsHash==pair.first && sample.psHash==pair.second;
                    if(sampledPair && found!=s.overlayReplayShaderCaptures.end()) {
                        sample.captureRecorded=true;sample.vsSaved=found->second.vsSaved;sample.psSaved=found->second.psSaved;
                        sample.rawPsQualified=found->second.qualified;sample.rawPsReason=found->second.reason;
                    } else if(sampledPair)sample.rawPsReason="diagnostic-unique-pair-cap-64";
                    ++s.overlayReplayRefusals[why];
                    overlayFail(s,why,overlayHdr);
                }
            } else if(!overlayStarted)
                overlayFail(s,reason?reason:"overlay-private-MRT-refused",overlayHdr);
        }
    }
    if(drawPacket) {
        FlatComputeInternalScope internal;
        state().drawPackets.execution(ctx,drawPacket,indirectArgs,indirectOffset,
            state().overlayRefusedWindow>drawPacketOverlayBefore?
                (state().overlay.refusal()?state().overlay.refusal():"current-overlay-refusal"):
            state().jitterRefusals>drawPacketJitterBefore?state().jitterReason:
            drawPacketPriority?"observed-priority-pair":"representative-observation");
    }
    if(!weaponFootprintStarted)return;
    const uint32_t actualStart=(weaponDrawKind=='D'||weaponDrawKind=='N')?
        static_cast<uint32_t>(weaponDrawBase):weaponDrawStart;
    const int32_t actualBase=(weaponDrawKind=='D'||weaponDrawKind=='N')?0:weaponDrawBase;
    state().weaponFootprint.beginActualDraw(ctx,state().prefix.frame,weaponFootprintSeq,
        weaponDrawKind,weaponDrawCount,actualStart,actualBase,weaponDrawInstances,
        weaponDrawStartInstance,indirectArgs,indirectOffset);
}
void FlatRuntimeDrawScope::endActualDraw() {
    // The game's state back from the layer first, before anything else here reads the context.
    if(uiTaken&&!uiEnded&&ctx){flatUiLayerEnd(ctx);uiEnded=true;}
    if(drawPacket&&ctx){FlatComputeInternalScope internal;state().drawPackets.after(ctx,drawPacket);drawPacketExecuted=true;}
    if(drawPacketOnly)return;
    if(domainProtectedOverlay && !overlayStarted && !overlayReplayPending)
        if(auto* candidate=domainCandidate(state(),domainDepth))
            candidate->motion.fail("foreground-private-overlay-not-captured");
    if(foregroundPlanned&&ctx) {
        state().foreground.endDraw(ctx);
        foregroundEnded=true;foregroundStarted=false;
    }
    if(overlayReplayPending&&ctx) {
        state().overlay.endReplayOriginalDraw(ctx);
        overlayReplayPending=false;
        overlayEnded=true;
        if(const char* reason=state().overlay.refusal()) overlayFail(state(),reason,overlayHdr);
        else { ++state().overlayMarkedWindow; ++state().overlayReplayCompleted; }
    }
    if(overlayStarted&&ctx) {
        state().overlay.endDraw(ctx);
        overlayEnded=true;
        overlayStarted=false;
        if(const char* reason=state().overlay.refusal()) overlayFail(state(),reason,overlayHdr);
        else ++state().overlayMarkedWindow;
    }
    if(weaponFootprintStarted&&ctx)
        state().weaponFootprint.endActualDraw(ctx,state().prefix.frame,weaponFootprintSeq);
    if(domainStarted&&ctx){engineVelocityFlatDomainEndDraw(ctx);domainStarted=false;}
    if(untrustedStarted&&ctx) {
        state().untrusted.endDraw(ctx);
        untrustedEnded=true;untrustedStarted=false;
    }
}
FlatRuntimeDrawScope::~FlatRuntimeDrawScope() {
    if(drawPacketOnly){if(drawPacket&&!drawPacketExecuted)state().drawPackets.abandoned(drawPacket);return;}
    if(ctx && (state().jitterRefusals>drawPacketJitterBefore || state().overlayRefusedWindow>drawPacketOverlayBefore)) {
        auto& s=state();const auto pair=std::make_pair(drawPacketVs,drawPacketPs);
        if(pair.first||pair.second) {
            if(s.drawPacketRefusedPairs.size()>=64 && !s.drawPacketRefusedPairs.count(pair)) {
                auto oldest=std::min_element(s.drawPacketRefusedPairs.begin(),s.drawPacketRefusedPairs.end(),[](const auto& a,const auto& b){return a.second<b.second;});
                s.drawPacketRefusedPairs.erase(oldest);
                ++s.drawPacketHistoryEvictions;
            }s.drawPacketRefusedPairs[pair]=s.prefix.frame;
        }
    }
    if(drawPacket&&!drawPacketExecuted)state().drawPackets.abandoned(drawPacket);
    if (!ctx) return;
    // A taken HUD draw whose End never ran (the hook did not reach endActualDraw): the game's state back from the layer.
    if(uiTaken&&!uiEnded){flatUiLayerEnd(ctx);uiEnded=true;}
    FlatComputeInternalScope guard;
    if(domainStarted){
        engineVelocityFlatDomainEndDraw(ctx);domainStarted=false;
        if(auto* candidate=domainCandidate(state(),domainDepth))candidate->motion.fail("foreground-abandoned-writer");
    }
    flatcpu::Scope shell(flatcpu::kOther);
    if(untrustedStarted) {state().untrusted.endDraw(ctx,false);untrustedStarted=false;}
    if(untrustedPlanned && !untrustedEnded)state().untrusted.abandon();
    if(foregroundStarted) {state().foreground.endDraw(ctx,false);foregroundStarted=false;}
    if(foregroundPlanned && !foregroundEnded)state().foreground.noteScopeIncomplete();
    if(overlayStarted) {
        state().overlay.endDraw(ctx);
        overlayStarted=false;
    }
    if(overlayReplayPending) {
        state().overlay.invalidate("replay-original-draw-incomplete");
        overlayReplayPending=false;
    }
    if(overlayPlanned && !overlayEnded)
        overlayFail(state(),"overlay-draw-scope-incomplete",overlayHdr);
    if(weaponFootprintStarted)
        state().weaponFootprint.endActualDraw(ctx,state().prefix.frame,weaponFootprintSeq,false);
    if(weaponFootprintStarted)state().weaponFootprint.after(ctx,state().prefix.frame,weaponFootprintSeq);
    if(drawCaptureStarted)state().drawCapture.after(ctx);
    {
        flatcpu::Scope jitter(flatcpu::kProjection);   // the binding scope's restore
        projection.reset();
    }
    if (producer) {
        // Engine motion's draw wrapper, the other half. Lazy: nothing goes back here; the game's shader, blend state and
        // MRTs go back once, before anything that could see them (flatRuntimeSubstitution). Eager, with a capture armed
        // or the overlay guard's private t3 bound: back now, as it always was.
        flatcpu::Scope engine(flatcpu::kEngineDraw);
        engineVelocityFlatEndDraw(ctx);
    }
    if (replaced) ctx->PSSetShaderResources(0, 1, &original);
    if (original) original->Release();
}

extern "C" unsigned int __cdecl edvr_selftest_flat_sdk_snapshot(
    EdvrFlatSdkBenchSnapshot* out, unsigned int bytes) {
    if (!out || bytes != sizeof(EdvrFlatSdkBenchSnapshot) ||
        out->size != sizeof(EdvrFlatSdkBenchSnapshot) || out->version != 4 ||
        !runtimeFlatProfile() || !owner())
        return 0;
    const auto& s = state();
    EdvrFlatSdkBenchSnapshot snap{};
    snap.flatProfile = 1;
    snap.live = g_flatRuntimeLive.load(std::memory_order_relaxed) ? 1u : 0u;
    snap.owner = 1;
    snap.deviceReady = s.device ? 1u : 0u;
    snap.contextReady = s.context ? 1u : 0u;
    snap.outputReady = s.output ? 1u : 0u;
    snap.frame = s.prefix.frame;
    snap.drawSequence = s.prefix.sequence;
    snap.work = static_cast<uint32_t>(s.work);
    snap.namedWorld = s.namedDepth ? 1u : 0u;
    snap.candidates = s.foregroundRoute.count(s.prefix.frame);
    snap.foreignSeen = s.foregroundCounts.foreignSeen;
    snap.captured = s.foregroundCounts.captured;
    auto captures = s.foregroundRetiredCaptureStats;
    for (const auto& candidate : s.foregroundCandidates) {
        const auto& stats = candidate.motion.stats();
        captures.attempts += stats.attempts;
        captures.submitted += stats.submitted;
    }
    snap.captureAttempts = captures.attempts;
    snap.gpuIdentitySubmitted = captures.submitted;
    snap.worldMarkers = s.foregroundCounts.worldMarkers;
    snap.hAttempts = s.foregroundCounts.hAttempts;
    snap.hQualified = s.foregroundCounts.hQualified;
    snap.predictedWorld = s.foregroundCounts.predictedWorld;
    snap.surfacePreserving = s.foregroundCounts.surfacePreserving;
    snap.surfacePreservingForeign = s.foregroundCounts.surfacePreservingForeign;
    snap.worldUnmarked = s.foregroundCounts.worldUnmarked;
    snap.failureKinds = s.foregroundFailureKinds.used;
    snap.failureKindsDropped = s.foregroundFailureKinds.dropped;
    snap.coveredDraws = s.foregroundCounts.coveredDraws;
    snap.hCoveredFrames = s.foregroundCounts.hCoveredFrames;
    if (s.namedDepth) std::memcpy(&snap.namedNear, s.namedCamera + (3 * 4 + 2) * sizeof(float), sizeof(float));
    snap.hdrTriggered = s.hdr.triggered ? 1u : 0u;
    snap.hdrSelected = s.hdrSelected.selected() ? 1u : 0u;
    const auto resolver = flatMonoResolveStats();
    snap.resolverCalls = resolver.calls;
    snap.backendFailures = resolver.backendFailures;
    snap.hdrResolves = resolver.hdrResolves;
    snap.hdrSpatial = resolver.hdrSpatial;
    snap.hdrCaptured = resolver.hdrCaptured;
    snap.hdrCopied = resolver.hdrCopied;
    snap.hdrPrepped = resolver.hdrPrepped;
    snap.hdrBackendCompleted = resolver.hdrBackend;
    snap.hdrFinished = resolver.hdrFinished;
    snap.hdrRestored = resolver.hdrRestored;
    std::snprintf(snap.mode, sizeof(snap.mode), "%s", s.mode.c_str());
    const State::DomainFailure* failure = nullptr;
    if (s.prefix.frame && s.foregroundFirstFailure.frame == s.prefix.frame) {
        failure = &s.foregroundFirstFailure;
        snap.firstFailureSelectedH = 1;
    } else {
        for (const auto& candidate : s.foregroundCandidates)
            if (s.prefix.frame && candidate.firstFailure.frame == s.prefix.frame &&
                (!failure || candidate.firstFailure.q < failure->q))
                failure = &candidate.firstFailure;
    }
    if (failure) {
        const auto& f = *failure;
        snap.firstFailureFrame = f.frame;
        snap.firstFailureSequence = f.q;
        snap.firstFailureFormat = f.format;
        snap.firstFailureVs = f.vs;
        snap.firstFailurePs = f.ps;
        std::snprintf(snap.firstFailureStage, sizeof(snap.firstFailureStage), "%s", f.stage);
        std::snprintf(snap.firstFailureReason, sizeof(snap.firstFailureReason), "%s", f.reason);
        snap.firstFailureState=f.state;
        snap.firstFailureBudget=f.budget;
    }
    std::snprintf(snap.hRefusal, sizeof(snap.hRefusal), "%s",
                  s.foregroundHRefusal ? s.foregroundHRefusal : "none");
    std::snprintf(snap.hdrVerdict, sizeof(snap.hdrVerdict), "%s", s.hdrWindow.lastVerdict);
    *out = snap;
    return 1;
}

// The bench may inspect the first-person map the last H drew (the first depth candidate's): RGBA32F at the render size, w = 1 a valid sample
// (x, y its motion), w = 2 a rejected one (x the reason, flat_foreground_motion_shader.h), w = 0 none. This only lends an AddRef'd SRV; readback
// happens in the offline process, and the next H rewrites the map.
extern "C" unsigned int __cdecl edvr_selftest_flat_sdk_foreground_map(
    ID3D11ShaderResourceView** out) {
    if (out) *out = nullptr;
    if (!out || !runtimeFlatProfile() || !owner()) return 0;
    const auto& s = state();
    for (const auto& candidate : s.foregroundCandidates) {
        auto view = candidate.motion.mapView();
        if (!view) continue;
        *out = view.Detach();
        return 1;
    }
    return 0;
}

// The bench may inspect the actual marker plane produced by the draw hooks.
// This only lends an AddRef'd SRV; readback happens in the offline process.
extern "C" unsigned int __cdecl edvr_selftest_flat_sdk_owner_view(
    ID3D11ShaderResourceView** out) {
    if (out) *out = nullptr;
    if (!out || !runtimeFlatProfile() || !owner()) return 0;
    const auto& s = state();
    if (!s.namedDepth) return 0;
    return engineVelocityFlatDomainSlots(
        static_cast<ID3D11Texture2D*>(const_cast<void*>(s.namedDepth)), out) ? 1u : 0u;
}
} // namespace edvr
