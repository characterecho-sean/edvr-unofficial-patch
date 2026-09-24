#include "flat_runtime.h"
#include "flat_runtime_model.h"
#include "flat_mono_resolve.h"
#include "flat_projection_recipes.h"
#include "binding_shadow.h"
#include "exposure_fix.h"
#include "device_hook.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "../common/temporal_mode.h"
#include <wrl/client.h>
#include <cstring>
#include <cstdio>
#include <map>
#include <string>
#include <memory>

namespace edvr {
std::atomic<bool> g_flatRuntimeLive{false};
namespace {
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
    uint64_t lastMs = 0, lastReport = 0, accepted = 0, refused = 0;
    uint64_t acceptedResetWindow = 0, acceptedHistoryWindow = 0;
    uint64_t resetMissingWindow = 0, resetGapWindow = 0, resetDepthWindow = 0;
    uint64_t resetColorWindow = 0, resetExtentWindow = 0;
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
    struct AuditDetail { uint64_t vs = 0, ps = 0, cs = 0; uint32_t reason = 0; };
    AuditDetail projectionDetails[32]{}; uint32_t projectionDetailsUsed = 0;
    struct AuditOutcome { uint64_t vs = 0, ps = 0, cs = 0, observations = 0; uint32_t reason = 0; };
    AuditOutcome projectionOutcomes[256]{}; uint32_t projectionOutcomesUsed = 0;
    uint64_t projectionOutcomeOverflow = 0;
    FlatMonoResolvePreflight plannedResolve{};
    FlatMonoResolvePreflightResult resolvePreflight{};
    bool haveResolvePlan = false;
    uint64_t resolvePreflightRetryMs = 0;
    uint64_t spatialFallbacks = 0, spatialFallbackFailures = 0;
};
// Driver objects retire on the owner Present; never release under loader lock.
State& state() { static State* p = new State; return *p; }
bool owner() { return state().thread == GetCurrentThreadId(); }
bool sameResolvePlan(const FlatMonoResolvePreflight& a,const FlatMonoResolvePreflight& b) {
    return a.renderWidth==b.renderWidth && a.renderHeight==b.renderHeight && a.outputWidth==b.outputWidth &&
        a.outputHeight==b.outputHeight && a.mode==b.mode && a.colorViewFormat==b.colorViewFormat &&
        a.depthViewFormat==b.depthViewFormat && a.colorViewIsTexture2D==b.colorViewIsTexture2D &&
        a.depthViewIsTexture2D==b.depthViewIsTexture2D && a.colorMostDetailedMip==b.colorMostDetailedMip &&
        a.depthMostDetailedMip==b.depthMostDetailedMip && a.colorViewMipLevels==b.colorViewMipLevels &&
        a.depthViewMipLevels==b.depthViewMipLevels && a.colorResourceMipLevels==b.colorResourceMipLevels &&
        a.colorArraySize==b.colorArraySize && a.colorSampleCount==b.colorSampleCount &&
        a.depthResourceMipLevels==b.depthResourceMipLevels &&
        a.depthArraySize==b.depthArraySize && a.depthSampleCount==b.depthSampleCount;
}
void projectionDetail(State& s, uint64_t vs, uint64_t ps, uint64_t cs, uint32_t reason, const char* text) {
    bool foundOutcome=false;
    for (uint32_t i=0;i<s.projectionOutcomesUsed;++i) {
        auto& outcome=s.projectionOutcomes[i];
        if (outcome.vs==vs && outcome.ps==ps && outcome.cs==cs && outcome.reason==reason) {
            ++outcome.observations; foundOutcome=true; break;
        }
    }
    if (!foundOutcome) {
        if (s.projectionOutcomesUsed<256) {
            auto& outcome=s.projectionOutcomes[s.projectionOutcomesUsed++];
            outcome={vs,ps,cs,1,reason};
        } else ++s.projectionOutcomeOverflow;
    }
    for (uint32_t i=0;i<s.projectionDetailsUsed;++i) {
        const auto& d=s.projectionDetails[i];
        if (d.vs==vs && d.ps==ps && d.cs==cs && d.reason==reason) return;
    }
    if (s.projectionDetailsUsed==32) return;
    s.projectionDetails[s.projectionDetailsUsed++]={vs,ps,cs,reason};
    Log::get().note("flat projection candidate: VS=%016llX PS=%016llX CS=%016llX reason=%s code=%u; preparation-only, raster-phase=0",
        static_cast<unsigned long long>(vs),static_cast<unsigned long long>(ps),static_cast<unsigned long long>(cs),text,reason);
}
void reportProjection(State& s, const char* event) {
    if (!s.projection) return;
    const auto status=s.projection->status();
    Log::get().note("flat projection readiness: event=%s draws=%llu dispatches=%llu candidates=%llu prepared=%llu refused=%llu depth-unassociated=%llu unknown-scene-draws=%llu full-writes=%llu initial-writes=%llu invalidations=%llu frames-left=%u raster-phase=0 raster-authorized=0 resolve-ready=%u fallback-ready=%u backend-available=%u backend-feature-deferred=%u preflight=%s spatial-fallbacks=%llu spatial-fallback-failures=%llu",
        event,(unsigned long long)s.projectionDraws,(unsigned long long)s.projectionDispatches,(unsigned long long)s.projectionCandidates,
        (unsigned long long)s.projectionReady,(unsigned long long)s.projectionMissing,(unsigned long long)s.projectionUnowned,
        (unsigned long long)s.projectionUnknown,(unsigned long long)status.fullWrites,(unsigned long long)status.initialWrites,
        (unsigned long long)status.invalidations,s.projectionFrames,s.resolvePreflight.rendererReady?1u:0u,
        s.resolvePreflight.spatialFallbackReady?1u:0u,s.resolvePreflight.backendAvailable?1u:0u,
        s.resolvePreflight.backendFeatureCreationDeferred?1u:0u,s.resolvePreflight.reason,
        (unsigned long long)s.spatialFallbacks,(unsigned long long)s.spatialFallbackFailures);
    Log::get().note("flat projection outcome totals: distinct=%u overflow-observations=%llu; result=prepared is reason-code 0, failures retain their code, 101=unknown-recipe",
        s.projectionOutcomesUsed,(unsigned long long)s.projectionOutcomeOverflow);
    static const char* outcomeNames[]={"prepared","wrong-thread","no-context1","capacity","unknown-buffer","missing-full-write","unsupported-range","binding-mismatch","invalid-recipe","private-failure","plan-failure"};
    for(uint32_t i=0;i<s.projectionOutcomesUsed;++i) {
        const auto& outcome=s.projectionOutcomes[i];
        const char* label=outcome.reason<11?outcomeNames[outcome.reason]:
            outcome.reason==100?"actual-shader-mismatch":outcome.reason==101?"unknown-scene-projection-recipe":
            outcome.reason==102?"invalid-render-extent":"other-refusal";
        Log::get().note("flat projection outcome: event=%s VS=%016llX PS=%016llX CS=%016llX result=%s code=%u count=%llu",
            event,(unsigned long long)outcome.vs,(unsigned long long)outcome.ps,(unsigned long long)outcome.cs,
            label,outcome.reason,(unsigned long long)outcome.observations);
    }
    static const char* reasons[]={"none","wrong-thread","no-context1","capacity","unknown-buffer","missing-full-write","unsupported-range","binding-mismatch","invalid-recipe","private-failure","plan-failure"};
    for (uint32_t i=1;i<11;++i) if(status.refusals[i])
        Log::get().note("flat projection refusal: reason=%s cumulative=%llu",reasons[i],(unsigned long long)status.refusals[i]);
    Log::get().note("flat projection cold buffers: queued=%llu completed=%llu stale=%llu failed=%llu pending=%llu timeouts=%llu; asynchronous full snapshots, unchanged-write tokens required",
        (unsigned long long)status.coldQueued,(unsigned long long)status.coldCompleted,(unsigned long long)status.coldStale,
        (unsigned long long)status.coldFailed,(unsigned long long)status.coldPending,(unsigned long long)status.coldTimeouts);
}
void qualifyProjection(State& s, FlatProjectionRecipes recipes, uint32_t width, uint32_t height,
                       uint64_t vs, uint64_t ps, uint64_t cs, bool owned) {
    if (!s.projection || !s.projectionFrames || !recipes.count) return;
    ++s.projectionCandidates;
    if (!owned) ++s.projectionUnowned;
    FlatComputeInternalScope internal;
    // Recipe hashes come from the observer; verify actual shaders before
    // trusting them in a modded context. No bindings are changed by this audit.
    Ptr<ID3D11VertexShader> actualVs; Ptr<ID3D11PixelShader> actualPs; Ptr<ID3D11ComputeShader> actualCs;
    bool shadersMatch = false;
    if (cs) { s.context->CSGetShader(&actualCs,nullptr,nullptr); shadersMatch=lookupShaderHash(actualCs.Get())==cs; }
    else { s.context->VSGetShader(&actualVs,nullptr,nullptr); s.context->PSGetShader(&actualPs,nullptr,nullptr);
        shadersMatch=lookupShaderHash(actualVs.Get())==vs && lookupShaderHash(actualPs.Get())==ps; }
    if (!shadersMatch) { ++s.projectionMissing; projectionDetail(s,vs,ps,cs,100,"actual-shader-mismatch"); return; }
    Ptr<ID3D11Buffer> buffers[3];
    for(uint32_t i=0;i<recipes.count;++i) {
        auto& request=recipes.requests[i];
        switch(request.stage) {
        case FlatProjectionStage::Vertex: s.projectionContext->VSGetConstantBuffers1(request.slot,1,&buffers[i],&request.firstConstant,&request.constantCount); break;
        case FlatProjectionStage::Pixel: s.projectionContext->PSGetConstantBuffers1(request.slot,1,&buffers[i],&request.firstConstant,&request.constantCount); break;
        case FlatProjectionStage::Compute: s.projectionContext->CSGetConstantBuffers1(request.slot,1,&buffers[i],&request.firstConstant,&request.constantCount); break;
        }
        request.original=buffers[i].Get();
    }
    FlatProjectionJitter proposed{};
    // Exercise the actual nonzero private preparation while game raster and
    // backend phases stay zero. Prepared plans are deliberately NOT bound.
    if(!flatProjectionJitter(.25f,-.25f,width,height,proposed)) {
        ++s.projectionMissing;projectionDetail(s,vs,ps,cs,102,"invalid-render-extent");return;
    }
    const bool ready=s.projection->preflight(recipes.requests,recipes.count,proposed,1) &&
        s.projection->prepare(recipes.requests,recipes.count,proposed,1)!=nullptr;
    if(ready) { ++s.projectionReady; projectionDetail(s,vs,ps,cs,0,"prepared"); }
    else { ++s.projectionMissing; projectionDetail(s,vs,ps,cs,static_cast<uint32_t>(s.projection->status().last),"private-preparation-refused"); }
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
        if (index == 64) { s.prefix.uncertain = true; return nullptr; }
    } else ++s.cameraCount;
    auto& c = s.cameras[index]; c = Camera{}; c.buffer = buffer; c.width = d.ByteWidth; return &c;
}
void capture(Camera& c, const void* bytes) {
    c.valid = flatCaptureCameraRows(c.rows, bytes, c.width); c.frame = state().prefix.frame; c.sequence = ++state().prefix.sequence;
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
    auto& s = state(); FlatComputeInternalScope guard; flatMonoResolveReset();
    if (s.projection) { reportProjection(s,"resize-or-stop"); s.projection.reset(); s.projectionContext.Reset(); s.projectionFrames=0; }
    s.haveResolvePlan=false; s.resolvePreflight={}; s.resolvePreflightRetryMs=0;
    s.havePrevious = false; s.previousColor.Reset(); s.output.Reset(); s.sceneDepth.Reset(); s.depthView.Reset();
    for (auto& v : s.views) v = View{};
    for (auto& r : s.colors) r.Reset(); for (auto& r : s.depths) r.Reset();
    for (auto& r : s.uavs) r.Reset();
    for (auto& c : s.cameras) c = Camera{}; s.cameraCount = 0;
    s.prefix = FlatRuntimePrefix{}; s.context.Reset(); s.device.Reset(); s.thread = 0; s.viewportCount = 0;
}
void flatRuntimeBeforePresent() { g_flatRuntimeLive.store(false, std::memory_order_release); }
void flatRuntimeArmProjectionAudit() { if(runtimeFlatProfile()) projectionAuditRequested.store(true,std::memory_order_release); }
void flatRuntimeCreateBuffer(ID3D11Buffer* buffer, const void* initialData) {
    if(owner() && state().projection) state().projection->observeCreateBuffer(buffer,initialData);
}
void flatRuntimePresent(IDXGISwapChain* swap, uint64_t frame, HRESULT hr, UINT flags) {
    if (!runtimeFlatProfile() || !swap || (flags & DXGI_PRESENT_TEST)) return;
    auto& s = state(); if (s.thread && !owner()) return;
    s.thread = GetCurrentThreadId();
    const auto mode = Config::get().requestedTemporalMode();
    const bool enabled = temporalModeEnabled(mode);
    if (mode != s.mode) {
        s.haveResolvePlan=false;s.resolvePreflight={};s.resolvePreflightRetryMs=0;
        s.mode = mode; reset(); engineVelocityConfigure(enabled);
        s.engine = _stricmp(mode.c_str(), "fsr") == 0 ? FlatMonoResolveMode::Fsr :
            _stricmp(mode.c_str(), "dlss") == 0 ? FlatMonoResolveMode::Dlss :
            _stricmp(mode.c_str(), "dlaa") == 0 ? FlatMonoResolveMode::Dlaa : FlatMonoResolveMode::Taa;
        Log::get().note("flat runtime: mode=%s zero-jitter experimental plumbing qualification; game SS controls render size; stereo pipeline remains suppressed", mode.c_str());
    }
    g_flatRuntimeLive.store(false, std::memory_order_release);
    engineVelocityConfigure(enabled);
    if (!enabled) { if (s.device || s.output || s.cameraCount) flatRuntimeResize(); return; }
    FlatComputeInternalScope guard;
    Ptr<ID3D11Device> actualDevice; swap->GetDevice(IID_PPV_ARGS(&actualDevice));
    if (s.device && actualDevice.Get() != s.device.Get()) { flatRuntimeResize(); s.thread = GetCurrentThreadId(); }
    if (!s.device) { swap->GetDevice(IID_PPV_ARGS(&s.device)); if (s.device) s.device->GetImmediateContext(&s.context); }
    if (!s.device || !s.context) return;
    if(s.projection)s.projection->pollColdReadbacks();
    if(s.projectionFrames && --s.projectionFrames==0) {
        reportProjection(s,"complete");s.projection.reset();s.projectionContext.Reset();
    }
    if(projectionAuditRequested.exchange(false,std::memory_order_acq_rel)) {
        if(s.projection)reportProjection(s,"rearmed");
        s.projection.reset(new(std::nothrow) FlatProjectionRuntime);
        s.projectionContext.Reset();s.context.As(&s.projectionContext);
        if(s.projection && s.projectionContext && s.projection->initialize(s.context.Get())) {
            s.projection->enableColdReadback(true);
            s.projectionFrames=900;s.projectionDraws=s.projectionDispatches=s.projectionCandidates=0;
            s.projectionReady=s.projectionMissing=s.projectionUnowned=s.projectionUnknown=0;
            s.projectionDetailsUsed=0;
            s.projectionOutcomesUsed=0;s.projectionOutcomeOverflow=0;
            s.resolvePreflightRetryMs=0;
            s.resolvePreflight=s.haveResolvePlan ? flatMonoResolvePreflight(s.device.Get(),s.context.Get(),s.plannedResolve) : FlatMonoResolvePreflightResult{};
            if(s.haveResolvePlan)s.resolvePreflightRetryMs=GetTickCount64();
            Log::get().note("flat projection: armed 900-frame preparation audit; proposed private phase=(0.25,-0.25), actual raster/backend phase=(0,0); no private plans will be bound");
            // The previous flight observed this pair but retained no file.
            // One attempt per manual arm saves exact creation bytes or emits
            // an explicit missing-cache result; no inferred shader admission.
            captureFlatProbeShader('v',0x5EAFFCD01B97D0C4ull);
            captureFlatProbeShader('p',0xDD371C57C9093BB8ull);
        } else {s.projection.reset();s.projectionContext.Reset();s.projectionFrames=0;
            Log::get().note("flat projection: arm refused (allocation/context1/runtime unavailable), raster-phase=0");}
    }
    const uint64_t preflightNow=GetTickCount64();
    if(s.projection && s.haveResolvePlan && !s.resolvePreflight.readyForRasterJitter() &&
       (!s.resolvePreflightRetryMs || preflightNow-s.resolvePreflightRetryMs>=1000)) {
        s.resolvePreflight=flatMonoResolvePreflight(s.device.Get(),s.context.Get(),s.plannedResolve);
        s.resolvePreflightRetryMs=preflightNow;
    }
    if (!s.treated || hr != S_OK) reset();
    Ptr<ID3D11Texture2D> output; if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&output)))) return;
    if (s.output.Get() != output.Get()) reset(); s.output = output;
    D3D11_TEXTURE2D_DESC d{}; output->GetDesc(&d);
    for (auto& r : s.colors) r.Reset(); for (auto& r : s.depths) r.Reset();
    s.prefix = FlatRuntimePrefix{}; s.prefix.frame = frame + 1;
    s.prefix.output = output.Get(); s.prefix.width = d.Width; s.prefix.height = d.Height; s.prefix.format = d.Format;
    s.namedDepth = s.namedConstants = nullptr; s.treated = false;
    foreignWork.store(false, std::memory_order_release);
    // Retain bounded CB identities across frames: unchanged bindings are legal.
    for (uint32_t i = 0; i < s.cameraCount; ++i) { s.cameras[i].valid = false; s.cameras[i].mapped = nullptr; }
    const auto now = GetTickCount64();
    if (now - s.lastReport >= 5000) {
        if(s.projection)reportProjection(s,"progress");
        if(s.spatialFallbacks || s.spatialFallbackFailures)
            Log::get().note("flat runtime spatial fallback cumulative: recovered=%llu failed=%llu history=invalid-on-recovery",
                (unsigned long long)s.spatialFallbacks,(unsigned long long)s.spatialFallbackFailures);
        Log::get().note("flat runtime: treated=%llu refused=%llu last=%s render-source=game-SS jitter=0 accepted-reset-5s=%llu accepted-history-5s=%llu treated-streak=%llu longest-treated-streak=%llu",
            static_cast<unsigned long long>(s.accepted), static_cast<unsigned long long>(s.refused), s.reason,
            static_cast<unsigned long long>(s.acceptedResetWindow), static_cast<unsigned long long>(s.acceptedHistoryWindow),
            static_cast<unsigned long long>(s.streak), static_cast<unsigned long long>(s.longestStreak));
        for (const auto& entry : s.refusedWindow)
            Log::get().note("flat runtime refusal 5s: reason=%s count=%llu", entry.first.c_str(), static_cast<unsigned long long>(entry.second));
        for (const auto& entry : s.conflictWindow)
            Log::get().note("flat runtime conflict 5s: cause=%s count=%llu", entry.first.c_str(), static_cast<unsigned long long>(entry.second));
        Log::get().note("flat runtime adapter reset 5s: no-previous=%llu frame-gap=%llu depth-change=%llu color-change=%llu extent-change=%llu",
            static_cast<unsigned long long>(s.resetMissingWindow), static_cast<unsigned long long>(s.resetGapWindow),
            static_cast<unsigned long long>(s.resetDepthWindow), static_cast<unsigned long long>(s.resetColorWindow),
            static_cast<unsigned long long>(s.resetExtentWindow));
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
        s.conflictWindow.clear();
        s.lastReport = now;
    }
    g_flatRuntimeLive.store(true, std::memory_order_release);
}
void flatRuntimeViewport(UINT n, const D3D11_VIEWPORT* vp) { if (!owner()) return; auto& s = state(); s.viewportCount = n; if (n == 1 && vp) s.viewport = *vp; }
void flatRuntimeConstantBuffers(UINT start, UINT count, ID3D11Buffer* const* buffers) {
    if (owner() && start <= 1 && 1-start < count && buffers && buffers[1-start]) camera(buffers[1-start], true);
}
void flatRuntimeClearBindings() { if (owner()) { state().viewportCount = 0; for (auto& u : state().uavs) u.Reset(); } }
void flatRuntimeUnknown() { if (owner()) { state().prefix.uncertain = true; state().viewportCount = 0; for (auto& c : state().cameras) c.valid = false; for (auto& u : state().uavs) u.Reset(); if(state().projection)state().projection->invalidateAll(); } }
void flatRuntimeUavs(UINT start, UINT count, ID3D11UnorderedAccessView* const* views) {
    if (!owner()) return;
    for (UINT i = 0; i < count && start + i < 8; ++i) {
        ResourceInfo info{};
        if (views && views[i]) bindingResolve(views[i], &info);
        state().uavs[start+i] = static_cast<ID3D11Resource*>(info.resource);
    }
}
void flatRuntimeDispatch(ID3D11DeviceContext* ctx) {
    auto& s = state(); if (!owner() || ctx != s.context.Get()) { foreignWork.store(true, std::memory_order_release); return; }
    if(s.projection) {
        ++s.projectionDispatches;
        // CSSetShader records only the pointer via bindingSet, unlike the
        // VS/PS hash-caching setters. Resolve its registered hash here, only
        // during the bounded audit; qualifyProjection verifies the actual CS.
        const auto cs=lookupShaderHash(bindingGet(BindSlot::Cs));
        if(cs==0x5998146D464F5C0Eull || cs==0xEB0245DE0BB23BB6ull) {
            FlatComputeInternalScope internal;
            Ptr<ID3D11ShaderResourceView> depth,material;ctx->CSGetShaderResources(3,1,&depth);ctx->CSGetShaderResources(4,1,&material);
            Ptr<ID3D11Resource> depthResource,materialResource;Ptr<ID3D11Texture2D> texture;
            if(depth)depth->GetResource(&depthResource);if(material)material->GetResource(&materialResource);
            if(depthResource)depthResource.As(&texture);
            D3D11_TEXTURE2D_DESC desc{};if(texture)texture->GetDesc(&desc);
            const bool owned=s.namedDepth && materialResource.Get()==s.namedDepth;
            qualifyProjection(s,flatProjectionDispatchRecipes(cs,desc.Width,desc.Height),desc.Width,desc.Height,0,0,cs,owned);
        }
    }
    for (const auto& u : s.uavs) if (u) {
        for (uint32_t i = 0; i < s.prefix.targetsUsed; ++i) {
            auto& target = s.prefix.targets[i];
            // Lighting legitimately writes HDR before tone. Any GPU write
            // into the completed handoff or its HDR input afterwards refuses.
            if (target.tones && (target.resource == u.Get() || target.tone.key.srvResource[1] == u.Get())) s.prefix.uncertain = true;
        }
        for (uint32_t i = 0; i < s.prefix.sourcesUsed; ++i) if (s.prefix.sources[i].key.depth == u.Get()) s.prefix.uncertain = true;
    }
}
void flatRuntimeWritten(ID3D11Resource* res) {
    if (!owner()) return; flatRuntimeWritten(state().prefix, res);
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
    if (auto* c = camera(res, false)) { if (c->mapped) capture(*c, c->mapped); c->mapped = nullptr; }
}
void flatRuntimeUpdate(ID3D11Resource* res, const void* bytes, const D3D11_BOX* box) {
    if (!owner()) return; flatRuntimeWritten(res);
    if (auto* c = camera(res, false)) { if (!box || (box->left == 0 && box->right == c->width)) capture(*c, bytes); }
    if(state().projection)state().projection->observeUpdate(res,bytes,box);
}

FlatRuntimeDrawScope::FlatRuntimeDrawScope(ID3D11DeviceContext* context, uint32_t instances) {
    if (!flatRuntimeActive()) return;
    auto& s = state(); if (!owner() || context != s.context.Get()) { foreignWork.store(true, std::memory_order_release); return; }
    ctx = context; FlatRuntimeDraw d{}; auto& k = d.key;
    const auto rt = view(BindSlot::Rtv0, 0), ds = view(BindSlot::Dsv0, 1);
    k.color = rt.resource; k.rtv = bindingGet(BindSlot::Rtv0); k.width = rt.a; k.height = rt.b; k.format = rt.fmt;
    k.depth = ds.resource; k.dsv = bindingGet(BindSlot::Dsv0); k.depthWidth = ds.a; k.depthHeight = ds.b; k.depthFormat = ds.fmt;
    k.vs = bindingShaderHash(BindSlot::Vs); k.ps = bindingShaderHash(BindSlot::Ps);
    k.b1 = bindingGet(BindSlot::VsCb1); k.viewportCount = s.viewportCount;
    static_assert(sizeof(k.viewport) == sizeof(D3D11_VIEWPORT), "viewport layout"); std::memcpy(k.viewport, &s.viewport, sizeof(k.viewport));
    if (auto* c = camera(static_cast<ID3D11Buffer*>(const_cast<void*>(k.b1)), false)) {
        if (c->valid && c->frame == s.prefix.frame) { std::memcpy(d.camera, c->rows, sizeof(d.camera)); k.camera = d.camera; k.cameraHash = flatCameraHash(d.camera); k.writeEpoch = c->frame; k.writeSeq = c->sequence; }
    }
    d.supported = engineVelocityPoolFamilyPair(k.vs, k.ps); d.instances = instances;
    k.kind = flatContractKind(d.supported, k.color, k.depth, k.width, k.height, k.format, s.prefix.width, s.prefix.height, k.color == s.prefix.output);
    const bool tone = k.vs == flat_mono_detail::kToneVs && k.ps == flat_mono_detail::kTonePs;
    const bool copy = k.vs == flat_mono_detail::kCopyVs && k.ps == flat_mono_detail::kCopyPs && k.color == s.prefix.output;
    if (tone || copy) for (uint32_t slot = 0; slot < 2; ++slot) {
        const auto bind = static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + slot);
        k.srvView[slot] = bindingGet(bind); k.srvResource[slot] = view(bind, 2 + slot).resource;
    }
    if (foreignWork.load(std::memory_order_acquire)) s.prefix.uncertain = true;
    const auto oldTargets = s.prefix.targetsUsed;
    const auto selected = flatRuntimeObserve(s.prefix, d);
    if (s.prefix.targetsUsed > oldTargets) {
        s.colors[oldTargets] = static_cast<ID3D11Resource*>(rt.resource);
        s.depths[oldTargets] = static_cast<ID3D11Resource*>(ds.resource);
    }
    const bool sceneExtent = flatContractKind(false, k.color, k.depth, k.width, k.height, k.format, s.prefix.width, s.prefix.height, false) == kFlatContractScreen;
    const bool sourceCandidate=d.supported && k.camera && k.depth && sceneExtent &&
        (k.format==23 || k.format==26) && flat_mono_detail::fullViewport(k,k.width,k.height);
    if(sourceCandidate && !s.namedDepth) {
        FlatComputeInternalScope guard;
        s.namedDepth=k.depth;s.namedConstants=k.b1;std::memcpy(s.namedCamera,d.camera,sizeof(d.camera));
        engineVelocityNoteSource(static_cast<ID3D11Texture2D*>(const_cast<void*>(k.depth)),static_cast<ID3D11Buffer*>(const_cast<void*>(k.b1)));
    }
    if(s.projection) {
        ++s.projectionDraws;
        if(sceneExtent && k.color!=s.prefix.output && (k.format==23 || k.format==26 || k.format==60)) {
            const auto recipes=flatProjectionDrawRecipes(k.vs,k.ps);
            if(recipes.count)qualifyProjection(s,recipes,k.width,k.height,k.vs,k.ps,0,
                s.namedDepth && k.depth==s.namedDepth);
            else if(k.depth) {++s.projectionUnknown;projectionDetail(s,k.vs,k.ps,0,101,"unknown-scene-projection-recipe");}
        }
    }
    if (sourceCandidate) {
        FlatComputeInternalScope guard;
        if (s.namedDepth == k.depth && s.namedConstants == k.b1 && std::memcmp(s.namedCamera, d.camera, sizeof(d.camera)) == 0) {
            ctx->OMGetRenderTargets(8, targets, &depth); producer = true; engineVelocityBeforeDraw(ctx, false);
        }
    }
    if (!copy) return;
    s.reason = flatMonoReasonName(selected.reason);
    if (!selected.selected()) {
        if (selected.reason == FlatMonoReason::ConflictingHdr) reportConflict(s);
        refuse(s); return;
    }
    if (s.treated || selected.depth != s.namedDepth || selected.sceneConstants != s.namedConstants) {
        s.reason = s.treated ? "already-treated-this-frame" : "producer-source-identity-mismatch"; refuse(s); return;
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
        !depthView(static_cast<ID3D11Texture2D*>(const_cast<void*>(selected.depth)))) { s.reason = "actual-handoff-or-depth-view-refused"; refuse(s); return; }
    FlatMonoResolveFrame f{}; f.color = original; f.depth = s.depthView.Get(); f.renderWidth = selected.renderWidth; f.renderHeight = selected.renderHeight;
    f.outputWidth = selected.outputWidth; f.outputHeight = selected.outputHeight; f.frame = s.prefix.frame; f.mode = s.engine;
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
        if(!s.haveResolvePlan || !sameResolvePlan(plan,s.plannedResolve)) {
            s.resolvePreflight={};s.resolvePreflightRetryMs=0;
        }
        s.plannedResolve=plan;s.haveResolvePlan=true;
    }
    std::memcpy(f.camera, selected.camera, sizeof(f.camera));
    const bool resetMissing = !s.havePrevious;
    const bool resetGap = s.havePrevious && s.previous.frame + 1 != selected.frame;
    const bool resetDepth = s.havePrevious && s.previous.depth != selected.depth;
    const bool resetColor = s.havePrevious && s.previous.color != selected.color;
    const bool resetExtent = s.havePrevious &&
        (s.previous.outputWidth != selected.outputWidth || s.previous.outputHeight != selected.outputHeight ||
         s.previous.renderWidth != selected.renderWidth || s.previous.renderHeight != selected.renderHeight);
    f.reset = resetMissing || resetGap || resetDepth || resetColor || resetExtent;
    std::memcpy(f.previousCamera, f.reset ? selected.camera : s.previous.camera, sizeof(f.previousCamera));
    const auto now = GetTickCount64(); f.deltaMs = s.lastMs ? static_cast<float>(now - s.lastMs) : 16.667f;
    if (!engineVelocitySourceViews(static_cast<ID3D11Texture2D*>(const_cast<void*>(selected.depth)), &f.engine)) { s.reason = "engine-source-not-ready"; refuse(s); return; }
    Ptr<ID3D11ShaderResourceView> engineSlots, enginePool, outputView; Ptr<ID3D11Buffer> nowCb, prevCb;
    engineSlots.Attach(f.engine.slots); enginePool.Attach(f.engine.pool); nowCb.Attach(f.engine.sceneNow); prevCb.Attach(f.engine.scenePrev);
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
    s.reason = "treated-zero-jitter";
    ID3D11ShaderResourceView* replacement = outputView.Get(); ctx->PSSetShaderResources(0, 1, &replacement); replaced = true;
    s.previous = selected; s.previousColor = actualColor; s.havePrevious = s.treated = true; s.lastMs = now; ++s.accepted;
    s.resetMissingWindow += resetMissing; s.resetGapWindow += resetGap; s.resetDepthWindow += resetDepth;
    s.resetColorWindow += resetColor; s.resetExtentWindow += resetExtent;
    if (f.reset) { ++s.acceptedResetWindow; s.streak = 1; }
    else { ++s.acceptedHistoryWindow; ++s.streak; }
    if (s.streak > s.longestStreak) s.longestStreak = s.streak;
}
FlatRuntimeDrawScope::~FlatRuntimeDrawScope() {
    if (!ctx) return; FlatComputeInternalScope guard;
    if (producer) { engineVelocityAfterFlatDraw(ctx); ctx->OMSetRenderTargets(8, targets, depth); }
    if (replaced) ctx->PSSetShaderResources(0, 1, &original);
    for (auto* target : targets) if (target) target->Release();
    if (depth) depth->Release(); if (original) original->Release();
}
} // namespace edvr
