#include "flat_runtime.h"
#include "flat_runtime_model.h"
#include "flat_mono_resolve.h"
#include "flat_projection_recipes.h"
#include "flat_shader_classifier.h"
#include "flat_projection_ownership.h"
#include "flat_camera_probe.h"
#include "flat_camera_inject.h"
#include "flat_camera_phase.h"
#include "flat_live_phase.h"
#include "flat_draw_capture.h"
#include "flat_pixel_capture_policy.h"
#include "flat_local_reject.h"
#include "flat_trace.h"
#include "flat_dlss_negotiate.h"
#include "flat_negotiated_eval.h"
#include "flat_camera_producer_probe.h"
#include "binding_shadow.h"
#include "exposure_fix.h"
#include "device_hook.h"
#include "dlaa.h"
#include "flat_sharpen.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "../common/temporal_mode.h"
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <map>
#include <string>
#include <memory>

namespace edvr {
std::atomic<bool> g_flatRuntimeLive{false};
namespace {
std::atomic<bool> nativeScale{false};
std::atomic<bool> foreignWork{false};
std::atomic<bool> projectionAuditRequested{false};
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
struct Camera {
    Ptr<ID3D11Buffer> buffer; uint32_t width = 0; uint64_t frame = 0;
    uint32_t sequence = 0;
    void* mapped = nullptr; bool valid = false;
    unsigned char rows[kFlatCameraBytes]{};
};
struct View {
    void* identity = nullptr; uint32_t generation = 0; ResourceInfo info{};
    Ptr<IUnknown> held;
};
struct State {
    FlatDrawCapture drawCapture;
    DWORD thread = 0; Ptr<ID3D11Device> device; Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11Texture2D> output, sceneDepth; Ptr<ID3D11ShaderResourceView> depthView;
    FlatRuntimePrefix prefix{}; Camera cameras[64]{}; uint32_t cameraCount = 0;
    View views[4]{}; D3D11_VIEWPORT viewport{}; UINT viewportCount = 0;
    Ptr<ID3D11Resource> colors[128], depths[128], previousColor;
    Ptr<ID3D11Resource> uavs[8];
    const void* namedDepth = nullptr, *namedConstants = nullptr;
    unsigned char namedCamera[kFlatCameraBytes]{};
    FlatMonoFrame previous{}; bool havePrevious = false, treated = false;
    std::string mode; FlatMonoResolveMode engine = FlatMonoResolveMode::Taa;
    unsigned preset = ~0u, foveaPreset = ~0u;
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
    bool jitterWanted = false, frameCoverage = true, temporalAccepted = false;
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
    bool partialWanted = true;
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
    FlatTraceRing traceRing{};
    FlatFrameContract traceContract{};

    // --- Partial temporal AA: coverage census (Part B), reset every 5s --------
    uint64_t covSceneDraws = 0, covExact = 0, covGeneric = 0, covInert = 0, covUnchanged = 0;
    uint64_t covLocalRefused = 0, covMemoEvictions = 0;
    uint64_t covFrames = 0, covFramesObserving = 0, covObservationEntries = 0;
    uint64_t covFramesLocallyRefused = 0;
    struct CoverageRefusedPair { uint64_t vs = 0, ps = 0; const char* reason = ""; uint64_t draws = 0; };
    CoverageRefusedPair covRefusedPairs[32]{};
    uint32_t covRefusedPairsUsed = 0;
};
// Driver objects retire on the owner Present; never release under loader lock.
State& state() { static State* p = new State; return *p; }
bool owner() { return state().thread == GetCurrentThreadId(); }
bool nonzeroPhase(const State& s) { return s.phase.currentX!=0 || s.phase.currentY!=0; }
void reportProjectionFailure(const State& s, const FlatProjectionRecipes& recipes,
    uint64_t vs, uint64_t ps, uint64_t cs) {
    if(!s.projection || !s.jitterWanted || !nonzeroPhase(s))return;
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
    s.frameCoverage=false;if(!s.jitterWanted)return;
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
        a.mode==b.mode && a.colorViewFormat==b.colorViewFormat &&
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
    const auto& cameraProbe=s.cameraProbe;
    Log::get().note("flat camera probe: event=%s observed=%llu conflicts=%llu attempts=%u complete=%u missing=%u actual-mismatch=%u first-frame=%llu last-frame=%llu result=%s; F10 only, two distinct conflict frames, CPU shadows only, no camera admission",
        event,(unsigned long long)cameraProbe.observed,(unsigned long long)cameraProbe.conflicts,
        cameraProbe.attempts,cameraProbe.complete,cameraProbe.missing,cameraProbe.actualMismatch,
        (unsigned long long)cameraProbe.firstFrame,(unsigned long long)cameraProbe.lastFrame,cameraProbe.result());
    reportUnknownProjection(s,event);
    Log::get().note("flat copy provenance capture: event=%s attempts=%u completed=%u missing-source-record=%u missing-destination-record=%u actual-shader-mismatch=%u rearmed-before-complete=%u first-frame=%llu result=%s; two distinct frames per F10 arm separated by at least 90 frames",
        event,copy.attempts,copy.completed,copy.missingSource,copy.missingDestination,
        copy.actualMismatch,copy.rearmedBeforeComplete,(unsigned long long)copy.firstFrame,
        copy.attempts?"observed":"exact-copy-never-observed");
}
// This HDR copy is distinct from the final-output copy admitted by the resolver.
constexpr uint64_t kHdrCopyVs=0xCFA91824129ECBBCull;
constexpr uint64_t kHdrCopyPs=0xDFCBA0EC70B03C9Bull;
constexpr uint64_t kImageFilterPs=0xFCFAD73924BF45B9ull;
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
    if(k.format!=9 || k.vs!=kHdrCopyVs || k.ps!=kImageFilterPs)return false;
    FlatComputeInternalScope guard;
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11RenderTargetView> rt;Ptr<ID3D11DepthStencilView> ds;
    ctx->VSGetShader(&vs,nullptr,nullptr);ctx->PSGetShader(&ps,nullptr,nullptr);
    ctx->OMGetRenderTargets(1,&rt,&ds);
    if(lookupShaderHash(vs.Get())!=kHdrCopyVs || lookupShaderHash(ps.Get())!=kImageFilterPs ||
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
    if(!s.projectionFrames || k.vs!=0x88DCF1164C640EC3ull || k.ps!=0x494506A63091DF8Cull)return 0;
    const FlatRuntimeTarget* target=nullptr;
    for(uint32_t i=0;i<s.prefix.targetsUsed;++i)
        if(s.prefix.targets[i].resource==k.color){target=&s.prefix.targets[i];break;}
    const bool conflict=k.format==26 && k.camera && target && target->hdrCamera &&
        std::memcmp(target->tone.camera,draw.camera,kFlatCameraBytes)!=0;
    if(!s.cameraProbe.begin(true,k.vs,k.ps,s.prefix.frame,conflict))return 0;
    const uint32_t attempt=s.cameraProbe.attempts;
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
    s.cameraProbe.finish(complete,actualMatches);
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
// never claimed as treated. experimental.temporal_aa_partial=off is the
// previous behaviour: failPhase per frame, retrying every frame.

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
    if (s.partialWanted && !s.observing) {
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
Camera* camera(ID3D11Resource* resource, bool add) {
    auto& s = state(); for (uint32_t i = 0; i < s.cameraCount; ++i) if (s.cameras[i].buffer.Get() == resource) return &s.cameras[i];
    if (!add || !resource) return nullptr;
    Ptr<ID3D11Buffer> buffer; if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))) return nullptr;
    D3D11_BUFFER_DESC d{}; buffer->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || d.ByteWidth < kFlatCameraOffset + kFlatCameraBytes) return nullptr;
    uint32_t index = s.cameraCount;
    if (index == 64) {
        for (uint32_t i = 0; i < 64; ++i) if (s.cameras[i].frame != s.prefix.frame && !s.cameras[i].mapped) { index = i; break; }
        if (index == 64) { s.prefix.uncertain = true; flatTraceMark(s.traceRing, kFlatTraceEventMarkUncertain, nullptr); return nullptr; }
    } else ++s.cameraCount;
    auto& c = s.cameras[index]; c = Camera{}; c.buffer = buffer; c.width = d.ByteWidth; return &c;
}
void capture(Camera& c, const void* bytes) {
    c.valid = flatCaptureCameraRows(c.rows, bytes, c.width); c.frame = state().prefix.frame;
    flatTraceMark(state().traceRing, kFlatTraceEventCameraCapture, nullptr);
    c.sequence = ++state().prefix.sequence;
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
void cameraWitness(const void* buffer) {
    auto& w = g_camWitness; ++w.writes;
    if (w.sitesFull) { ++w.dedupHits; return; }
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
    if (!site) { ++w.dedupHits; return; }
    for (auto& s : w.sites) if (s.address == site) {
        ++s.writes; ++w.dedupHits;
        bool known = false;
        for (uint32_t b = 0; b < s.bufferCount; ++b) known |= s.buffers[b] == buffer;
        if (!known && s.bufferCount < 3) {
            s.buffers[s.bufferCount++] = buffer;
            char briefBuf[96];
            const char* brief = witnessModuleBrief(site, briefBuf, sizeof(briefBuf));
            Log::get().note("flat camera witness: producer site %s also writes %p", brief, buffer);
        }
        return;
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
        return;
    }
    if (!w.budgetNoted) {
        w.budgetNoted = true;
        Log::get().note("flat camera witness: site budget reached; later writers counted without stacks");
    }
    ++w.budgetDropped;
}
bool depthView(ID3D11Texture2D* depth) {
    auto& s = state(); if (s.sceneDepth.Get() == depth && s.depthView) return true;
    s.sceneDepth.Reset(); s.depthView.Reset(); if (!depth) return false;
    D3D11_TEXTURE2D_DESC d{}; depth->GetDesc(&d); D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
    v.Format = static_cast<DXGI_FORMAT>(flatRuntimeDepthReadFormat(d.Format));
    if (v.Format == DXGI_FORMAT_UNKNOWN) return false;
    if (d.SampleDesc.Count != 1 || !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE) || FAILED(s.device->CreateShaderResourceView(depth, &v, &s.depthView))) return false;
    s.sceneDepth = depth; return true;
}
}

void flatRuntimeResize() {
    g_flatRuntimeLive.store(false, std::memory_order_release);
    nativeScale.store(false, std::memory_order_release);
    auto& s = state(); FlatComputeInternalScope guard; s.drawCapture.cancel("resize-or-stop"); flatMonoResolveReset();
    flatCameraInjectReset(); // history and the decision do not survive a resize; injected cameras stay known for the flush
    finishPhaseCensusFrame(s);
    if(s.phaseCensusFrames || s.phaseFailuresUsed || s.phaseOverflowCalls)
        reportPhaseCensus(s,"resize-or-stop");
    if (s.projection) { reportProjection(s,"resize-or-stop"); s.projection.reset(); s.projectionContext.Reset(); s.projectionFrames=0; }
    s.haveResolvePlan=false; s.resolvePreflight={}; s.resolvePreflightRetryMs=0;
    s.phase.resetHistory();s.phaseDepth.Reset();s.phaseHdr.Reset();s.temporalAccepted=false;
    s.phaseWidth=s.phaseHeight=0;s.frameCoverage=true;
    s.havePrevious = false; s.previousColor.Reset(); s.output.Reset(); s.sceneDepth.Reset(); s.depthView.Reset();
    for (auto& v : s.views) v = View{};
    for (auto& r : s.colors) r.Reset(); for (auto& r : s.depths) r.Reset();
    for (auto& r : s.uavs) r.Reset();
    for (auto& c : s.cameras) c = Camera{}; s.cameraCount = 0;
    // Local refusal's observation ends with the contract: a resize or device
    // change requalifies nothing, but the state itself must not survive.
    s.observing = false; s.covFrameLocallyRefused = false;
    s.prefix = FlatRuntimePrefix{}; s.context.Reset(); s.device.Reset(); s.thread = 0; s.viewportCount = 0;
}
void flatRuntimeBeforePresent() { g_flatRuntimeLive.store(false, std::memory_order_release); }
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
void flatRuntimeArmProjectionAudit() { if(runtimeFlatProfile()) projectionAuditRequested.store(true,std::memory_order_release); }
void flatRuntimeCreateBuffer(ID3D11Buffer* buffer, const void* initialData) {
    if(owner() && state().projection) state().projection->observeCreateBuffer(buffer,initialData);
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
    uint32_t emittedFrames = 0, emittedEvents = 0, skipped = 0;
    for (uint32_t i = 0; i < kFlatTraceFrames; ++i) {
        if (!s.traceRing.slotUsed[i]) continue;
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
    Log::get().note("flat trace dump: %ls frames=%u events=%u bytes=%u skipped-slots=%u%s",
        path, emittedFrames, emittedEvents, bytes, skipped,
        shortWrite || bytes != expected ? " SHORT WRITE -- capture unusable" : "");
}
void flatRuntimePresent(IDXGISwapChain* swap, uint64_t frame, HRESULT hr, UINT flags) {
    if (!runtimeFlatProfile() || !swap || (flags & DXGI_PRESENT_TEST)) return;
    auto& s = state(); if (s.thread && !owner()) return;
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
    // on, independent of jitterWanted/partialWanted -- see the 5s report
    // below.
    ++s.covFrames;
    if (s.observing) ++s.covFramesObserving;
    if (s.covFrameLocallyRefused) ++s.covFramesLocallyRefused;
    s.covFrameLocallyRefused = false;
    const auto mode = Config::get().requestedTemporalMode();
    const bool enabled = temporalModeEnabled(mode);
    const auto model = Config::get().getString("fix.temporal_aa_model", "k");
    const auto preset = temporalPresetFor(model);
    if (s.preset != preset.full || s.foveaPreset != preset.fovea) {
        s.preset = preset.full; s.foveaPreset = preset.fovea;
        dlaaSetPreset(preset.full, preset.fovea);
        if (temporalEngineFor(mode) == TemporalEngine::Nvidia) {
            s.haveResolvePlan=false;s.resolvePreflight={};s.resolvePreflightRetryMs=0;
            reset();s.phase.resetHistory();
        }
        Log::get().note("flat runtime: DLSS model=%s preset=%u; applied at frame boundary%s",
            model.c_str(),preset.full,preset.known?"":" (unknown model; using K)");
    }
    if (mode != s.mode) {
        s.haveResolvePlan=false;s.resolvePreflight={};s.resolvePreflightRetryMs=0;
        s.mode = mode; reset(); engineVelocityConfigure(enabled);
        s.phase.resetHistory();
        s.engine = _stricmp(mode.c_str(), "fsr") == 0 ? FlatMonoResolveMode::Fsr :
            _stricmp(mode.c_str(), "dlss") == 0 ? FlatMonoResolveMode::Dlss :
            _stricmp(mode.c_str(), "dlaa") == 0 ? FlatMonoResolveMode::Dlaa : FlatMonoResolveMode::Taa;
        Log::get().note("flat runtime: mode=%s experimental mono temporal; game SS controls render size; jitter uses qualified D3D11 projection scopes", mode.c_str());
    }
    g_flatRuntimeLive.store(false, std::memory_order_release);
    engineVelocityConfigure(enabled);
    if (!enabled) { if (s.device || s.output || s.cameraCount) flatRuntimeResize(); return; }
    FlatComputeInternalScope guard;
    Ptr<ID3D11Device> actualDevice; swap->GetDevice(IID_PPV_ARGS(&actualDevice));
    if (s.device && actualDevice.Get() != s.device.Get()) { flatRuntimeResize(); s.thread = GetCurrentThreadId(); }
    if (!s.device) { swap->GetDevice(IID_PPV_ARGS(&s.device)); if (s.device) s.device->GetImmediateContext(&s.context); }
    if (!s.device || !s.context) return;
    // The completed frame is a draw-capture sample only if it was live: the resolver did not reset
    // it, and it ran at a nonzero phase unless the jitter is off on purpose. The two frames after an
    // F10 arm were neither, and their constants carry no phase (2026-09-29).
    s.drawCapture.present(s.context.Get(),frame,flatCaptureFrameLive(flatMonoResolveLastReset(),s.frameHadPhase,s.jitterWanted));
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
    const bool wanted=Config::get().getBool("experimental.temporal_aa_jitter",true);
    if(wanted!=s.jitterWanted) { s.phase.resetHistory();reset(); }
    s.jitterWanted=wanted;
    // Partial temporal AA: read live, same idiom as jitter above. Unlike
    // jitter, toggling it does not change any projection math, so it needs
    // no history reset -- it only gates refuseDraw, checked fresh on every
    // draw from here on. The on->off transition ends observation explicitly:
    // off's per-frame retry resumes (reviews/flat-temporal-main-review-2026-09-26.md).
    const bool partialWanted=Config::get().getBool("experimental.temporal_aa_partial",true);
    const bool observingAfterToggle = flatObservationToggle(s.observing, s.partialWanted, partialWanted);
    if (s.observing && !observingAfterToggle)
        Log::get().note("flat coverage: observation ended by setting change at frame=%llu; per-frame attempts resume",
            (unsigned long long)frame);
    s.observing = observingAfterToggle; s.partialWanted = partialWanted;
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
    if(wanted && !s.projection) {
        s.projection.reset(new(std::nothrow) FlatProjectionRuntime);
        s.context.As(&s.projectionContext);
        if(!s.projection || !s.projectionContext || !s.projection->initialize(s.context.Get())) {
            s.projection.reset();s.projectionContext.Reset();
        }
    }
    if(s.projection)s.projection->pollColdReadbacks();
    if(s.projectionFrames && --s.projectionFrames==0) {
        reportProjection(s,"complete");
    }
    if(projectionAuditRequested.exchange(false,std::memory_order_acq_rel)) {
        flatMonoResolveArmPixels(frame);
        s.drawCapture.arm(frame);
        if(s.projectionFrames)reportProjection(s,"rearmed");
        else if(s.unknownProjectionPairsUsed || s.unknownProjectionCaptureOverflow)
            reportUnknownProjection(s,"manual-rearm");
        if(!s.projection) {
            s.projection.reset(new(std::nothrow) FlatProjectionRuntime);
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
            flatTraceDumpToLogDir(s, frame);
            // Refresh exact creation bytecode once per manual arm, or emit
            // an explicit missing-cache result; no inferred shader admission.
            captureFlatProbeShader('v',0x5EAFFCD01B97D0C4ull);
            captureFlatProbeShader('p',0xDD371C57C9093BB8ull);
            captureFlatProbeShader('v',kHdrCopyVs);
            captureFlatProbeShader('p',kHdrCopyPs);
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
    s.phase.beginFrame(flatCameraPhaseEnabled(flatCameraInjectRoute(),wanted,s.observing,s.projection!=nullptr),
        compatible,s.phaseWidth,s.phaseHeight);
    s.frameHadPhase=nonzeroPhase(s);
    flatCameraInjectArm(); // the phase is chosen: the injector's frame window opens
    s.frameCoverage=true;s.temporalAccepted=false;
    s.jitterReason=nonzeroPhase(s)?"live":"warming";
    if(s.projection)s.projection->enableColdReadback(!nonzeroPhase(s));
    if(!wanted && !s.projectionFrames) {s.projection.reset();s.projectionContext.Reset();}
    for (auto& r : s.colors) r.Reset(); for (auto& r : s.depths) r.Reset();
    s.prefix = FlatRuntimePrefix{}; s.prefix.frame = frame + 1;
    // Trace ring: seal the frame that just ended with the hash over every
    // copy outcome it produced, then reset the contract for the next frame.
    flatTraceSeal(s.traceRing, s.traceContract.produced,
                  s.traceContract.produced ? flatFrameContractHash(s.traceContract) : 0);
    s.traceContract = FlatFrameContract{};
    flatTraceBeginFrame(s.traceRing, frame + 1, output.Get(), d.Width, d.Height, d.Format);
    s.phaseCensusPending=s.jitterWanted;
    s.phaseCensusFailed=false;
    s.prefix.output = output.Get(); s.prefix.width = d.Width; s.prefix.height = d.Height; s.prefix.format = d.Format;
    s.namedDepth = s.namedConstants = nullptr; s.treated = false;
    s.observingQualifiedHandoff = false;
    s.drawCapture.begin(frame+1,s.phaseDepth.Get(),s.phaseWidth,s.phaseHeight);
    foreignWork.store(false, std::memory_order_release);
    // Retain bounded CB identities across frames: unchanged bindings are legal.
    for (uint32_t i = 0; i < s.cameraCount; ++i) { s.cameras[i].valid = false; s.cameras[i].mapped = nullptr; }
    const auto now = GetTickCount64();
    if (now - s.lastReport >= 5000) {
        reportPhaseCensus(s,"5s");
        if(s.projectionFrames)reportProjection(s,"progress");
        else reportUnknownProjection(s,"5s");
        Log::get().note("flat HDR image continuation: accepted=%llu refused=%llu; source writes require current matching scene provenance",
            (unsigned long long)s.hdrCopiesAccepted,(unsigned long long)s.hdrCopiesRefused);
        // static-scene-frames: frames the resolver was handed with the menu's stale-slot policy on
        // (FlatMonoResolveFrame::staticScene, set from an accepted copy). Zero while accepted grows
        // is what "the policy never ran" looks like: no menu frame reached the resolver, and the
        // flat runtime line's last= and accepted counts say why.
        Log::get().note("flat menu HDR copy: accepted=%llu refused=%llu static-scene-frames=%llu; source requires current scene/depth/camera provenance",
            (unsigned long long)s.menuCopiesAccepted,(unsigned long long)s.menuCopiesRefused,(unsigned long long)s.staticSceneFrames);
        // The census of unkeyed pairs, every window while a temporal mode runs (empty
        // included: an absent line is what "this block never ran" looks like). A pair
        // named here draws in a known pool family with no keyed pixel shader, so its
        // pixels have no engine slot (engine_velocity_unkeyed.h).
        {
            char unkeyed[560]; engineVelocityFormatUnkeyed(unkeyed,sizeof(unkeyed));
            Log::get().note("%s",unkeyed);
        }
        Log::get().note("flat jitter: enabled=%u wanted=%u phase=(%.5g,%.5g) previous=(%.5g,%.5g) warm=%u frames=%llu draws=%llu dispatches=%llu refusals=%llu state=%s history-valid=%u",
            enabled?1u:0u,s.jitterWanted?1u:0u,
            s.phase.currentX,s.phase.currentY,s.phase.previousX,s.phase.previousY,s.phase.warmFrames,
            (unsigned long long)s.jitteredFrames,(unsigned long long)s.jitterDraws,(unsigned long long)s.jitterDispatches,
            (unsigned long long)s.jitterRefusals,s.jitterReason,s.phase.previousAcceptedValid?1u:0u);
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
        // Part B coverage census: always printed, even when every field is
        // zero -- that is how "code never ran" (line absent) differs from
        // "ran, nothing refused" (local-refused=0 on a line that is present).
        Log::get().note("flat coverage 5s: partial=%s observing=%u scene-draws=%llu exact=%llu generic=%llu inert=%llu "
            "unchanged=%llu local-refused=%llu frames=%llu frames-locally-refused=%llu "
            "returned-to-observation=%llu frames-observing=%llu memo-evictions=%llu",
            s.partialWanted?"on":"off", s.observing?1u:0u,
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
        Log::get().note("flat camera witness 5s: writes=%llu unique-sites=%llu dedup-hits=%llu stack-drops=%llu; every camera-table write is a producer witness candidate",
            static_cast<unsigned long long>(g_camWitness.writes), static_cast<unsigned long long>(witnessSites),
            static_cast<unsigned long long>(g_camWitness.dedupHits), static_cast<unsigned long long>(g_camWitness.budgetDropped));
        Log::get().note("flat runtime adapter reset 5s: no-previous=%llu frame-gap=%llu depth-change=%llu color-change=%llu extent-change=%llu present-not-ok=%llu",
            static_cast<unsigned long long>(s.resetMissingWindow), static_cast<unsigned long long>(s.resetGapWindow),
            static_cast<unsigned long long>(s.resetDepthWindow), static_cast<unsigned long long>(s.resetColorWindow),
            static_cast<unsigned long long>(s.resetExtentWindow), static_cast<unsigned long long>(s.presentNotOkWindow));
        const auto renderer = flatMonoResolveStats();
        Log::get().note("flat runtime renderer cumulative: calls=%llu init=%llu context-change=%llu allocations=%llu full-reset=%llu invalidations=%llu accepted-reset=%llu accepted-continue=%llu requested-reset=%llu lost-history=%llu frame-gap=%llu invalid-prev-camera=%llu format-change=%llu camera-cut=%llu backend-failure=%llu continue-run=%llu longest-continue-run=%llu",
            static_cast<unsigned long long>(renderer.calls), static_cast<unsigned long long>(renderer.initializations),
            static_cast<unsigned long long>(renderer.contextPointerMismatches), static_cast<unsigned long long>(renderer.allocations),
            static_cast<unsigned long long>(renderer.fullResets), static_cast<unsigned long long>(renderer.invalidations),
            static_cast<unsigned long long>(renderer.acceptedResets), static_cast<unsigned long long>(renderer.acceptedContinues),
            static_cast<unsigned long long>(renderer.requestedResets), static_cast<unsigned long long>(renderer.lostHistory),
            static_cast<unsigned long long>(renderer.frameGaps), static_cast<unsigned long long>(renderer.invalidPreviousCameras),
            static_cast<unsigned long long>(renderer.formatChanges), static_cast<unsigned long long>(renderer.cameraCuts),
            static_cast<unsigned long long>(renderer.backendFailures), static_cast<unsigned long long>(renderer.currentContinueRun),
            static_cast<unsigned long long>(renderer.longestContinueRun));
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
    g_flatRuntimeLive.store(true, std::memory_order_release);
}
void flatRuntimeViewport(UINT n, const D3D11_VIEWPORT* vp) { if (!owner()) return; auto& s = state(); s.viewportCount = n; if (n == 1 && vp) s.viewport = *vp; }
void flatRuntimeConstantBuffers(UINT start, UINT count, ID3D11Buffer* const* buffers) {
    if (owner() && start <= 1 && 1-start < count && buffers && buffers[1-start]) camera(buffers[1-start], true);
}
void flatRuntimeClearBindings() { if (owner()) { state().viewportCount = 0; for (auto& u : state().uavs) u.Reset(); } }
void flatRuntimeUnknown() { if (owner()) { flatTraceMark(state().traceRing, kFlatTraceEventMarkUncertain, nullptr); state().prefix.uncertain = true; state().viewportCount = 0; for (auto& c : state().cameras) c.valid = false; for (auto& u : state().uavs) u.Reset(); if(state().projection) {state().projection->invalidateAll();failPhase(state(),"unknown-context-state");} } }
void flatRuntimeUavs(UINT start, UINT count, ID3D11UnorderedAccessView* const* views) {
    if (!owner()) return;
    for (UINT i = 0; i < count && start + i < 8; ++i) {
        ResourceInfo info{};
        if (views && views[i]) bindingResolve(views[i], &info);
        state().uavs[start+i] = static_cast<ID3D11Resource*>(info.resource);
    }
}
FlatRuntimeDispatchScope::FlatRuntimeDispatchScope(ID3D11DeviceContext* ctx) {
    if(!flatRuntimeActive())return;
    auto& s = state(); if (!owner() || ctx != s.context.Get()) { foreignWork.store(true, std::memory_order_release); return; }
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
        flatTraceMark(s.traceRing, kFlatTraceEventDispatchWritten, u.Get());
        flatRuntimeDispatchObserveWritten(s.prefix, u.Get());
    }
    if(s.prefix.uncertain && s.projection)failPhase(s,"compute-source-invalidated");
}
void flatRuntimeWritten(ID3D11Resource* res) {
    if (!owner()) return; flatTraceMark(state().traceRing, kFlatTraceEventWriteResource, res);
    flatRuntimeWritten(state().prefix, res);
    if (auto* c = camera(res, false)) c->valid = false;
    if(state().projection)state().projection->invalidate(res);
}
void flatRuntimeMap(ID3D11Resource* res, D3D11_MAP type, void* bytes) {
    if (!owner() || type == D3D11_MAP_READ) return;
    flatRuntimeWritten(res); if (auto* c = camera(res, false)) c->mapped = bytes;
    if(state().projection)state().projection->observeMap(res,type,bytes);
}
void flatRuntimeUnmap(ID3D11Resource* res) {
    if (!owner()) return; if(state().projection)state().projection->observeUnmap(res);
    if (auto* c = camera(res, false)) {
        if (c->mapped) { capture(*c, c->mapped); cameraWitness(res); }
        c->mapped = nullptr;
    }
}
void flatRuntimeUpdate(ID3D11Resource* res, const void* bytes, const D3D11_BOX* box) {
    if (!owner()) return; flatRuntimeWritten(res);
    if (auto* c = camera(res, false)) {
        if (!box || (box->left == 0 && box->right == c->width)) { capture(*c, bytes); cameraWitness(res); }
    }
    if(state().projection)state().projection->observeUpdate(res,bytes,box);
}

FlatRuntimeDrawScope::FlatRuntimeDrawScope(ID3D11DeviceContext* context, uint32_t instances,
                                           char kind, uint32_t count, uint32_t start,
                                           int32_t base, uint32_t startInstance) {
    if (!flatRuntimeActive()) return;
    auto& s = state(); if (!owner() || context != s.context.Get()) { foreignWork.store(true, std::memory_order_release); return; }
    ctx = context; FlatRuntimeDraw d{}; auto& k = d.key;
    const FlatProjectionBindingPlan* projectionPlan=nullptr;
    const auto rt = view(BindSlot::Rtv0, 0), ds = view(BindSlot::Dsv0, 1);
    k.color = rt.resource; k.rtv = bindingGet(BindSlot::Rtv0); k.width = rt.a; k.height = rt.b; k.format = rt.fmt;
    k.depth = ds.resource; k.dsv = bindingGet(BindSlot::Dsv0); k.depthWidth = ds.a; k.depthHeight = ds.b; k.depthFormat = ds.fmt;
    k.vs = bindingShaderHash(BindSlot::Vs); k.ps = bindingShaderHash(BindSlot::Ps);
    // Partial temporal AA's current-draw identities (see refuseDraw): the
    // pair a per-draw reason refuses, named in the coverage census. Cheap
    // POD stores, done for every draw so qualifyProjection (State& only) can
    // reach them too.
    s.drawVs = k.vs; s.drawPs = k.ps;
    k.b1 = bindingGet(BindSlot::VsCb1); k.viewportCount = s.viewportCount;
    static_assert(sizeof(k.viewport) == sizeof(D3D11_VIEWPORT), "viewport layout"); std::memcpy(k.viewport, &s.viewport, sizeof(k.viewport));
    if (auto* c = camera(static_cast<ID3D11Buffer*>(const_cast<void*>(k.b1)), false)) {
        if (c->valid && c->frame == s.prefix.frame) { std::memcpy(d.camera, c->rows, sizeof(d.camera)); k.camera = d.camera; k.cameraHash = flatCameraHash(d.camera); k.writeEpoch = c->frame; k.writeSeq = c->sequence; }
    }
    d.supported = engineVelocityPoolFamilyPair(k.vs, k.ps); d.instances = instances;
    k.kind = flatContractKind(d.supported, k.color, k.depth, k.width, k.height, k.format, s.prefix.width, s.prefix.height, k.color == s.prefix.output);
    const bool tone = flat_mono_detail::toneHdrSlot(k.vs, k.ps) != ~0u;
    const bool copy = k.vs == flat_mono_detail::kCopyVs && k.ps == flat_mono_detail::kCopyPs && k.color == s.prefix.output;
    if(!copy && s.drawCapture.active()) {
        FlatComputeInternalScope guard;
        drawCaptureStarted=s.drawCapture.before(ctx,instances,kind,count,start,base,startInstance,
            k.vs,k.ps,bindingGet(BindSlot::Vs),bindingGet(BindSlot::Ps));
    }
    if (tone || copy) for (uint32_t slot = 0; slot < 2; ++slot) {
        const auto bind = static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + slot);
        k.srvView[slot] = bindingGet(bind); k.srvResource[slot] = view(bind, 2 + slot).resource;
    }
    if (foreignWork.load(std::memory_order_acquire)) s.prefix.uncertain = true;
    d.hdrCopyVerified=verifyHdrCopy(ctx,d);
    d.menuHdrCopyVerified=verifyMenuHdrCopy(ctx,d);
    d.imageSourceCameraIndependentVerified=verifyCameraIndependentImageSource(ctx,d);
    captureCopyProvenance(s,ctx,d);
    const auto oldTargets = s.prefix.targetsUsed;
    const uint32_t oldImageAccepted=s.prefix.imageCopiesAccepted,oldImageRefused=s.prefix.imageCopiesRefused;
    const uint32_t oldMenuAccepted=s.prefix.menuCopiesAccepted,oldMenuRefused=s.prefix.menuCopiesRefused;
    const uint32_t cameraProbeAttempt=captureCameraConflict(s,d);
    // Gate 1 consolidation: the copy draw's selection is produced as the
    // frame contract (identical decision), and every draw is recorded into
    // the trace ring for the reducer replay.
    const auto selected = copy
        ? flatRuntimeObserveContract(s.prefix, d, s.traceContract)
        : flatRuntimeObserve(s.prefix, d);
    flatTraceRecord(s.traceRing, d, foreignWork.load(std::memory_order_acquire));
    if(cameraProbeAttempt)for(uint32_t i=0;i<s.prefix.targetsUsed;++i)if(s.prefix.targets[i].resource==k.color){
        const auto& witness=s.prefix.targets[i].firstBad;
        Log::get().note("flat camera probe observer: attempt=%u frame=%llu draw-seq=%u first-bad=%s first-bad-seq=%u caused-first-camera-conflict=%u",
            cameraProbeAttempt,(unsigned long long)s.prefix.frame,s.prefix.sequence,flatRuntimeConflictName(witness.cause),witness.sequence,
            witness.cause==FlatRuntimeConflict::CameraChange && witness.sequence==s.prefix.sequence?1u:0u);
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
    // FP16 image intermediates use the same scene-size predicate; their
    // producer admission remains separate from the format-23/26 motion source.
    const bool sceneExtent = flatContractKind(false, k.color, k.depth, k.width, k.height, k.format==9?26:k.format, s.prefix.width, s.prefix.height, false) == kFlatContractScreen;
    const bool sourceCandidate=d.supported && k.camera && k.depth && sceneExtent &&
        (k.format==23 || k.format==26) && flat_mono_detail::fullViewport(k,k.width,k.height);
    if(sourceCandidate && !s.namedDepth) {
        FlatComputeInternalScope guard;
        s.namedDepth=k.depth;s.namedConstants=k.b1;std::memcpy(s.namedCamera,d.camera,sizeof(d.camera));
        engineVelocityNoteSource(static_cast<ID3D11Texture2D*>(const_cast<void*>(k.depth)),static_cast<ID3D11Buffer*>(const_cast<void*>(k.b1)));
    }
    if(s.projection) {
        if(s.projectionFrames)++s.projectionDraws;
        if(sceneExtent && k.color!=s.prefix.output && (k.format==9 || k.format==23 || k.format==26 || k.format==60)) {
            // Part B coverage census (always on, no F10 audit needed): which
            // recipe branch this candidate draw took. Orthogonal to whether
            // qualifyProjection then accepted or locally/globally refused it.
            ++s.covSceneDraws;
            if(s.projectionFrames)captureLocalProjection(s,k.vs,k.ps);
            const auto recipes=flatProjectionDrawRecipes(k.vs,k.ps);
            if(recipes.count) {
                ++s.covExact;
                FlatComputeInternalScope guard;
                Ptr<ID3D11DepthStencilView> actualDepth;Ptr<ID3D11Resource> depthResource;
                ctx->OMGetRenderTargets(0,nullptr,&actualDepth);
                if(actualDepth)actualDepth->GetResource(&depthResource);
                const bool owned=depthResource && (depthResource.Get()==s.namedDepth || depthResource.Get()==s.phaseDepth.Get());
                if(!owned && k.color==s.phaseHdr.Get())failPhase(s,"scene-projection-depth-unassociated");
                if(nonzeroPhase(s) && owned && s.phaseDepth && depthResource.Get()!=s.phaseDepth.Get())failPhase(s,"scene-depth-changed");
                const bool sceneHdr=flatRuntimeProjectionHdr(k,s.prefix.output,owned?depthResource.Get():nullptr);
                projectionPlan=qualifyProjection(s,recipes,k.width,k.height,k.vs,k.ps,0,owned,sceneHdr);
            }
            else if(flatProjectionDrawUnchanged(k.vs,k.ps)) {
                ++s.covUnchanged;
                FlatComputeInternalScope guard;
                Ptr<ID3D11VertexShader> actualVs;Ptr<ID3D11PixelShader> actualPs;
                ctx->VSGetShader(&actualVs,nullptr,nullptr);ctx->PSGetShader(&actualPs,nullptr,nullptr);
                if(lookupShaderHash(actualVs.Get())==k.vs && lookupShaderHash(actualPs.Get())==k.ps) {
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
                    Ptr<ID3D11DepthStencilView> actualDepth;Ptr<ID3D11Resource> depthResource;
                    ctx->OMGetRenderTargets(0,nullptr,&actualDepth);
                    if(actualDepth)actualDepth->GetResource(&depthResource);
                    const bool owned=depthResource && (depthResource.Get()==s.namedDepth || depthResource.Get()==s.phaseDepth.Get());
                    if(!owned && k.color==s.phaseHdr.Get())failPhase(s,"scene-projection-depth-unassociated");
                    if(nonzeroPhase(s) && owned && s.phaseDepth && depthResource.Get()!=s.phaseDepth.Get())failPhase(s,"scene-depth-changed");
                    const bool sceneHdr=flatRuntimeProjectionHdr(k,s.prefix.output,owned?depthResource.Get():nullptr);
                    projectionPlan=qualifyProjection(s,genericRecipes,k.width,k.height,k.vs,k.ps,0,owned,sceneHdr);
                }
                else if(generic.vs==FlatVsProjectionClass::InertNoCB && generic.ps==FlatPsProjectionSafety::Clean) {
                    ++s.covInert;
                    FlatComputeInternalScope guard;
                    Ptr<ID3D11VertexShader> actualVs;Ptr<ID3D11PixelShader> actualPs;
                    ctx->VSGetShader(&actualVs,nullptr,nullptr);ctx->PSGetShader(&actualPs,nullptr,nullptr);
                    if(lookupShaderHash(actualVs.Get())==k.vs && lookupShaderHash(actualPs.Get())==k.ps) {
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
    if (sourceCandidate) {
        FlatComputeInternalScope guard;
        if (s.namedDepth == k.depth && s.namedConstants == k.b1 && std::memcmp(s.namedCamera, d.camera, sizeof(d.camera)) == 0) {
            ctx->OMGetRenderTargets(8, targets, &depth); producer = true; engineVelocityBeforeDraw(ctx, false);
        }
    }
    // The earlier capture preserves original game CBs and shader identities.
    // Motion is sampled only after the producer has attached its actual MRT6,
    // and the destructor takes the matching sample before restoring the draw.
    if(drawCaptureStarted) {
        FlatComputeInternalScope guard;
        s.drawCapture.motionBefore(ctx,producer && !targets[6] && engineVelocityDrawSubstituted(),sourceCandidate);
    }
    // Engine motion observes unmodified game constants above. Under Upstream
    // ownership the camera was already jittered at the source by the
    // injector, so the legacy row-patching scope is suppressed (a counter
    // elsewhere, never silence); the injector's noteApplied covers the
    // phase machine's application accounting.
    if(projectionPlan && !flatCameraInjectUpstreamOwns()) {
        projection.emplace(*projectionPlan);
        if(projection->active()) {s.phase.noteApplied();++s.jitterDraws;}
        else refuseDraw(s,"draw-binding-refused");
    }
    // Tripwire, structurally unreachable: a legacy application under Upstream.
    if(projection && projection->active() && flatCameraInjectUpstreamOwns())++s.rows.legacyAppliedUnderUpstream;
    if (!copy) return;
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
    if (s.treated || selected.depth != s.namedDepth || selected.sceneConstants != s.namedConstants) {
        s.reason = s.treated ? "already-treated-this-frame" : "producer-source-identity-mismatch";
        if(!s.treated && s.phase.applied)recover(s.reason);
        refuse(s); return;
    }
    FlatComputeInternalScope guard;
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
    nativeScale.store(f.renderWidth >= f.outputWidth && f.renderHeight >= f.outputHeight,
                      std::memory_order_release);
    f.configuredDlssPreset=s.preset;
    // The 3D main menu: this frame's contract came through the verified menu HDR copy (the acceptance
    // the "flat menu HDR copy" line counts). Nothing but the camera moves there, so a pixel whose engine
    // slot an unkeyed draw overdrew takes the camera term instead of refusing history. False for every
    // other frame, and the shader is then bit-identical to what it was before the field.
    f.staticScene=flatFrameThroughMenuCopy(s.prefix,selected.hdr);
    if(f.staticScene)++s.staticSceneFrames;
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
        (s.jitterWanted && (s.phase.failed || !s.phase.previousAcceptedValid));
    f.jitterX=s.phase.currentX;f.jitterY=s.phase.currentY;
    f.previousJitterX=f.reset?f.jitterX:s.phase.previousX;
    f.previousJitterY=f.reset?f.jitterY:s.phase.previousY;
    std::memcpy(f.previousCamera, f.reset ? selected.camera : s.previous.camera, sizeof(f.previousCamera));
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
    if (!engineVelocitySourceViews(static_cast<ID3D11Texture2D*>(const_cast<void*>(selected.depth)), &f.engine)) {
        s.reason="engine-source-not-ready";if(s.phase.applied)recover(s.reason);refuse(s);return;
    }
    Ptr<ID3D11ShaderResourceView> engineSlots, enginePool, outputView; Ptr<ID3D11Buffer> nowCb, prevCb;
    engineSlots.Attach(f.engine.slots); enginePool.Attach(f.engine.pool); nowCb.Attach(f.engine.sceneNow); prevCb.Attach(f.engine.scenePrev);
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
FlatRuntimeDrawScope::~FlatRuntimeDrawScope() {
    if (!ctx) return; FlatComputeInternalScope guard;
    if(drawCaptureStarted)state().drawCapture.after(ctx);
    projection.reset();
    if (producer) { engineVelocityAfterFlatDraw(ctx); ctx->OMSetRenderTargets(8, targets, depth); }
    if (replaced) ctx->PSSetShaderResources(0, 1, &original);
    for (auto* target : targets) if (target) target->Release();
    if (depth) depth->Release(); if (original) original->Release();
}
} // namespace edvr
