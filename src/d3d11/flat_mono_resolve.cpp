#include "flat_mono_resolve.h"
#include "dlaa.h"
#include "fsr3_engine.h"
#include "temporal_shader_bytecode.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>
#include "../common/log.h"
#include "flat_isolation_mode.h"
#include "flat_context_state.h"
#include "flat_cpu.h"
#include "flat_hdr_crumbs.h"
#include "flat_pixel_capture.h"
#include "flat_resolve_input_capture.h"
#include "flat_projection_math.h"

namespace edvr {
namespace {
using Microsoft::WRL::ComPtr;
// The GPU census's span hooks (flatMonoResolveSetSpanHooks): null unless the flat runtime installed them.
FlatMonoResolveSpanFn g_spanBegin = nullptr, g_spanEnd = nullptr;
// One timestamp pair around the resolver's own dispatches and backend call, closed by every exit.
struct SpanGuard {
    ID3D11DeviceContext* context;
    bool open = false;
    explicit SpanGuard(ID3D11DeviceContext* c) : context(c) {
        if (g_spanBegin && g_spanEnd) { g_spanBegin(context); open = true; }
    }
    ~SpanGuard() { if (open) g_spanEnd(context); }
};
// The HDR route's breadcrumbs (flat_hdr_crumbs.h): true while a resolver call that belongs to the route is running and the
// session's crumbs are live. The three entry points set it (CrumbScope) and the helpers below read it, so none of them is
// handed the route's bit; a copy-route call in the same frame never sets it, so its steps are never written under the
// route's name. A plain bool: the resolver runs on the owner thread. g_hdrCall is the same scope without the crumbs' gate:
// the call is the HDR route's, which is what its step counts (FlatMonoResolveStats::hdrCaptured and the rest) ask.
bool g_crumbOn = false, g_hdrCall = false;
struct CrumbScope {
    const bool previous, previousCall;
    explicit CrumbScope(bool hdr) : previous(g_crumbOn), previousCall(g_hdrCall) { g_hdrCall = hdr; g_crumbOn = hdr && hdrCrumbLive(); }
    ~CrumbScope() { g_crumbOn = previous; g_hdrCall = previousCall; }
    CrumbScope(const CrumbScope&) = delete;
    CrumbScope& operator=(const CrumbScope&) = delete;
};
// Like renderer state, explicit owner-thread cleanup only: never release live
// driver resources from static destruction under the DLL loader lock.
FlatPixelCapture& pixels=*new FlatPixelCapture;
FlatResolveInputCapture& resolveInputs=*new FlatResolveInputCapture;
struct Image {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11ShaderResourceView> srgb;
    ComPtr<ID3D11UnorderedAccessView> uav;
};
struct State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext1> context;
    // The game's state is isolated one of two ways (flat_isolation_mode.h), chosen at initialisation: by swapping in this fresh
    // state object (every device but DXMT's), or by the explicit capture (capture true, no state object made), whose slot ranges
    // are this device's.
    ComPtr<ID3DDeviceContextState> isolated;
    bool capture=false;
    FlatContextRanges ranges;
    ComPtr<ID3D11ComputeShader> prep, taa, finish, spatial;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    // The HDR route's pixel-shader half (section 81), made on first use (initializeHdr): one triangle vertex shader,
    // the finish and the spatial recovery as pixel shaders, and a rasterizer state that culls nothing. The render-target
    // view over the game's H is cached by the texture it was made for and holds a reference to it, so the address
    // cannot be reused by another texture while it stands; flatMonoResolveReset lets go.
    ComPtr<ID3D11VertexShader> hdrVs;
    ComPtr<ID3D11PixelShader> finishHdr, spatialHdr;
    ComPtr<ID3D11RasterizerState> noCull;
    ComPtr<ID3D11RenderTargetView> hdrRtv;
    ID3D11Texture2D* hdrRtvTexture=nullptr;
    // The refusal census and view (FlatMonoResolveFrame::refusalCensus, refusalView; flat_mono_refusal.h), all made on first use by
    // a frame that asks, so a profile that never asks never pays: the counting pass, the prep's class texture (R8_UINT, render
    // size), the counter buffer the pass adds into, and the staging ring its sums are read back from a few frames later.
    ComPtr<ID3D11ComputeShader> census;
    Image klass;
    uint32_t classWidth=0, classHeight=0;
    ComPtr<ID3D11Buffer> refusalCounts;
    ComPtr<ID3D11UnorderedAccessView> refusalCountsUav;
    ComPtr<ID3D11Buffer> refusalStaging[4];
    bool refusalPending[4]={false,false,false,false};
    uint32_t refusalWidth[4]={0,0,0,0}, refusalHeight[4]={0,0,0,0};
    uint32_t refusalWrite=0;
    Image color, rawOverlay, depth[2], motion, rejection, expected, output[2], outputDomain[2];
    uint32_t width=0, height=0, outWidth=0, outHeight=0, evalWidth=0, evalHeight=0, current=0;
    // The steady-detail depth check's previous depth (FlatMonoResolveFrame::steadyDetail). TAA keeps last frame's depth in depth[current^1]
    // already; the other backends keep none, so depth[1] is made on the first frame that asks, and while frames ask, the depth the
    // backend is handed alternates between the two images: depthLast is the one the previous frame wrote (0 for every frame that did not ask).
    uint32_t depthLast=0;
    uint64_t lastFrame=0;
    FlatMonoResolveMode mode=FlatMonoResolveMode::Taa;
    bool hdr=false;   // the resources below are the HDR route's (an HDR input copy, fp16 outputs)
    bool history=false;
    bool untrustedCoverageLast=false;
    float sdkDepthScaleLast=1.f;
    DXGI_FORMAT inputFormat=DXGI_FORMAT_UNKNOWN;
};
// Explicit owner-thread cleanup only: releasing driver objects from a static
// destructor during DLL detach would run under the loader lock.
State& g=*new State;
FlatMonoResolveStats& stats=*new FlatMonoResolveStats;
// Session budgets survive flatMonoResolveReset: repeated backend/resource
// resets must not restart diagnostic output. Keep camera cuts independently
// visible even if ordinary requested resets use their budget first.
uint32_t resetEventsLogged=0, cameraCutEventsLogged=0;
constexpr uint32_t kResetEventLogCap=32;
// The first-person inputs' two log lines (FlatMonoResolveFrame::firstPersonMotion): each said once a session, like the
// budgets above. The refusal's reason is also kept in stats.firstPersonRefusal, so a later, different one is not lost.
bool firstPersonBoundLogged=false, firstPersonRefusedLogged=false;
// Test-only (flatMonoResolveTestPrepBytecode and flatMonoResolveTestTaaBytecode, at the foot of this file): empty, the only state outside
// tools\flat_mono_resolve_test. Leaked on purpose, like the renderer state: nothing of ours runs from static destruction under the DLL
// loader lock.
std::vector<unsigned char>& g_testPrepBytecode=*new std::vector<unsigned char>;
std::vector<unsigned char>& g_testTaaBytecode=*new std::vector<unsigned char>;
// What the next initialisation is asked for (flatMonoResolveSetIsolation, test rigs only; Auto in production): what the next initialisation is asked for; and the renderer's
// initialisations that said which isolation they chose, in the log, at most kIsolationLogCap a session.
FlatContextIsolation g_isolationRequest=FlatContextIsolation::Auto;
uint32_t isolationLogged=0;
constexpr uint32_t kIsolationLogCap=4;
// The explicit capture's storage (flat_context_state.h): one block, filled and emptied inside one Isolate at a time. Leaked on
// purpose, like the renderer state: a process that dies inside an isolation must not release the game's objects from static
// destruction under the loader lock.
FlatContextState& g_contextBlock=*new FlatContextState;
// The refusal census's totals since the last take (flatMonoResolveTakeRefusalCensus) and the cadence's own counter, which is not
// reset by a take. Leaked on purpose, like the renderer state.
FlatMonoRefusalCensus& g_refusal=*new FlatMonoRefusalCensus;
uint64_t g_refusalCadence=0;
bool g_refusalFailureLogged=false;
// The steady-detail rule's second depth image could not be made: said once, and the frames that ask run as if they had not (refused as before).
bool g_steadyFailureLogged=false;
// rowsJitter: the NDC shift the camera rows themselves carry (current xy, previous zw), the shader removes it; all zero
// when the rows are unjittered, which is every path that does not go through the upstream camera injector.
// route: x = the HDR route (the input is R11G11B10F radiance, the outputs fp16), y = with x, EDVR's TAA output is final
// and the pixel-shader finish only copies it into H, z = the first-person map (t9) and stencil (t10) are bound and valid
// (FlatMonoResolveFrame::firstPersonMotion), w = the phase mode (firstPersonPhaseMode; 0 without z). All zero on the copy route without them, whose shader arithmetic is
// unchanged.
// debug: x = this frame samples the refusal census, y = this frame paints the refusal view (FlatMonoResolveFrame::refusalCensus and
// refusalView; the prep writes its class texture for either). Zero for every frame that asks for neither.
struct Constants { float camera[6][4], previous[6][4]; uint32_t size[4], flags[4]; float jitter[4], rowsJitter[4]; uint32_t route[4], debug[4]; float foregroundDepth[4]; };
static_assert(sizeof(Constants)==304, "HLSL cbuffer layout");
// The game's pipeline state out of the way for the resolver's own work and its backends', and back on every exit. Two ways
// (flat_isolation_mode.h says which a device gets): the context state swap, which every device but DXMT's has always had and
// which is unchanged, or the explicit capture (flat_context_state.h) for DXMT, whose SwapDeviceContextState aborts the process.
// Either way the context is ClearState()d after the game's state is out and before it goes back, so the work starts from the
// defaults and leaves nothing bound for the game to inherit.
struct Isolate {
    ID3D11DeviceContext1* context;
    ComPtr<ID3DDeviceContextState> previous;
    const bool byCapture;
    Isolate(ID3D11DeviceContext1* c, ID3DDeviceContextState* state, bool explicitCapture):context(c),byCapture(explicitCapture) {
        HdrCrumbSpan capture(g_crumbOn,"capture-state","by=%s",byCapture?"capture":"swap");   // the game's pipeline state taken out, ours cleared
        if(byCapture) {
            ++stats.isolationCaptures;
            g_contextBlock.capture(context,g.ranges,hdrCrumbFirstCapture(g_crumbOn));
        } else {
            ++stats.isolationSwaps;
            context->SwapDeviceContextState(state, previous.GetAddressOf());
        }
        context->ClearState();
        if(g_hdrCall)++stats.hdrCaptured;
    }
    ~Isolate() {
        HdrCrumbSpan restore(g_crumbOn,"restore-state","by=%s",byCapture?"capture":"swap");   // ours cleared, the game's put back
        // Keep our reusable state free of resource bindings; restoring the game
        // cannot leave our UAVs aliased with its pending output-copy SRV.
        context->ClearState();
        if(byCapture)g_contextBlock.restore(context,hdrCrumbFirstRestore(g_crumbOn));
        else context->SwapDeviceContextState(previous.Get(), nullptr);
        if(g_hdrCall)++stats.hdrRestored;
    }
    Isolate(const Isolate&)=delete;
    Isolate& operator=(const Isolate&)=delete;
};
bool fail(const char** reason, const char* text) {
    g.history=false;
    stats.currentContinueRun=0;
    if(reason)*reason=text;
    return false;
}
bool cameraValid(const float (&c)[6][4]) {
    for(const auto& row:c)for(float v:row)if(!std::isfinite(v))return false;
    if(c[0][2]!=0 || c[1][2]!=0 || c[2][2]!=0 || c[3][3]!=0 || !(c[3][2]>0))return false;
    const double ax=c[0][0],ay=c[1][0],az=c[2][0], bx=c[0][1],by=c[1][1],bz=c[2][1];
    const double cx=c[0][3],cy=c[1][3],cz=c[2][3];
    const double det=ax*(by*cz-bz*cy)+ay*(bz*cx-bx*cz)+az*(bx*cy-by*cx);
    return std::isfinite(det) && std::abs(det)>1e-8;
}
bool jitterValid(const FlatMonoResolveFrame& f) {
    return std::isfinite(f.jitterX) && std::isfinite(f.jitterY) &&
        std::isfinite(f.previousJitterX) && std::isfinite(f.previousJitterY) &&
        std::abs(f.jitterX)<=.5f && std::abs(f.jitterY)<=.5f &&
        std::abs(f.previousJitterX)<=.5f && std::abs(f.previousJitterY)<=.5f;
}
bool rowsJitterValid(const FlatMonoResolveFrame& f) {
    return std::isfinite(f.rowsJitterX) && std::isfinite(f.rowsJitterY) &&
        std::isfinite(f.previousRowsJitterX) && std::isfinite(f.previousRowsJitterY) &&
        std::abs(f.rowsJitterX)<=.5f && std::abs(f.rowsJitterY)<=.5f &&
        std::abs(f.previousRowsJitterX)<=.5f && std::abs(f.previousRowsJitterY)<=.5f;
}
bool resolveModeValid(FlatMonoResolveMode mode) {
    return mode==FlatMonoResolveMode::Taa || mode==FlatMonoResolveMode::Dlaa ||
        mode==FlatMonoResolveMode::Dlss || mode==FlatMonoResolveMode::Fsr;
}
bool preflightMetadataValid(const FlatMonoResolvePreflight& f,const char** reason) {
    if(!f.renderWidth || !f.renderHeight || !f.outputWidth || !f.outputHeight ||
       f.renderWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       f.renderHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       f.outputWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       f.outputHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        if(reason)*reason="flat-preflight-invalid-extent";
        return false;
    }
    if(!resolveModeValid(f.mode)) {
        if(reason)*reason="flat-preflight-invalid-mode";
        return false;
    }
    // Gate 2 step 2: the route table owns the size refusals (section 72); the
    // preflight's reason tokens keep their preflight prefixes.
    const auto route = flatResolveRoute(f.mode, f.renderWidth, f.renderHeight, f.outputWidth, f.outputHeight);
    if (route.refused) {
        if (reason) *reason = f.mode==FlatMonoResolveMode::Dlaa ?
            "flat-preflight-dlaa-requires-native-render-size" : "flat-preflight-trained-resolve-cannot-downsample";
        return false;
    }
    // The HDR route resolves at E = R and writes into H: a render below the output, or a route that evaluates on
    // another grid (EDVR's TAA above D), stays on the copy route (section 81, decision (c)).
    const uint32_t plannedEvalW=(f.evalWidth&&f.evalHeight)?f.evalWidth:route.evalWidth;
    const uint32_t plannedEvalH=(f.evalWidth&&f.evalHeight)?f.evalHeight:route.evalHeight;
    if(f.hdr && (plannedEvalW!=f.renderWidth || plannedEvalH!=f.renderHeight ||
                 f.renderWidth<f.outputWidth || f.renderHeight<f.outputHeight)) {
        if(reason)*reason="flat-preflight-hdr-requires-render-size-evaluation";
        return false;
    }
    const bool colorFormat=f.hdr ? f.colorViewFormat==DXGI_FORMAT_R11G11B10_FLOAT
        : (f.colorViewFormat==DXGI_FORMAT_R8G8B8A8_UNORM || f.colorViewFormat==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const bool depthFormat=f.depthViewFormat==DXGI_FORMAT_R32_FLOAT ||
        f.depthViewFormat==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ||
        f.depthViewFormat==DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
        f.depthViewFormat==DXGI_FORMAT_R16_UNORM;
    if(!colorFormat || !depthFormat) {
        if(reason)*reason="flat-preflight-unsupported-source-format";
        return false;
    }
    const bool colorViewRange=f.colorViewMipLevels==1 || f.colorViewMipLevels==UINT32_MAX;
    const bool depthViewRange=f.depthViewMipLevels==1 || f.depthViewMipLevels==UINT32_MAX;
    if(!f.colorViewIsTexture2D || !f.depthViewIsTexture2D ||
       f.colorMostDetailedMip!=0 || f.depthMostDetailedMip!=0 ||
       !colorViewRange || !depthViewRange || f.colorResourceMipLevels!=1 ||
       f.depthResourceMipLevels!=1 || f.colorArraySize!=1 || f.depthArraySize!=1 ||
       f.colorSampleCount!=1 || f.depthSampleCount!=1) {
        if(reason)*reason="flat-preflight-source-layout-unsupported";
        return false;
    }
    return true;
}
// `role` names the image in the HDR route's crumbs, which say what each texture is (format, size) before it is made and
// every HRESULT after; E_PENDING in one of them means that call was never reached.
bool image(ID3D11Device* device,uint32_t width,uint32_t height,DXGI_FORMAT format,Image& out,bool writable=true,const char* role="image") {
    HdrCrumbSpan span(g_crumbOn,"create-texture","role=%s fmt=%s(%u) size=%ux%u uav=%u",role,hdrCrumbFormat(static_cast<uint32_t>(format)),
        static_cast<unsigned>(format),width,height,writable?1u:0u);
    HRESULT hrTexture=E_PENDING,hrSrv=E_PENDING,hrUav=E_PENDING,hrSrgb=E_PENDING;
    const auto done=[&](bool ok) {
        span.result("hr=0x%08X srv=0x%08X uav=0x%08X srgb=0x%08X",static_cast<unsigned>(hrTexture),static_cast<unsigned>(hrSrv),
            static_cast<unsigned>(hrUav),static_cast<unsigned>(hrSrgb));
        return ok;
    };
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;desc.Format=format;
    desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|(writable?D3D11_BIND_UNORDERED_ACCESS:0);
    if(FAILED(hrTexture=device->CreateTexture2D(&desc,nullptr,out.texture.GetAddressOf())))return done(false);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=format==DXGI_FORMAT_R8G8B8A8_TYPELESS?DXGI_FORMAT_R8G8B8A8_UNORM:format;
    sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=sd.Format;ud.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
    if(FAILED(hrSrv=device->CreateShaderResourceView(out.texture.Get(),&sd,out.srv.GetAddressOf())) ||
       (writable && FAILED(hrUav=device->CreateUnorderedAccessView(out.texture.Get(),&ud,out.uav.GetAddressOf()))))return done(false);
    if(format==DXGI_FORMAT_R8G8B8A8_TYPELESS) {
        sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if(FAILED(hrSrgb=device->CreateShaderResourceView(out.texture.Get(),&sd,out.srgb.GetAddressOf())))return done(false);
    }
    return done(true);
}
bool initialize(ID3D11Device* device,ID3D11DeviceContext* context,const char** reason) {
    if(g.device.Get()==device && g.context.Get()==context && g.prep && g.taa && g.finish && g.spatial && g.constants && g.sampler &&
       (g.capture || g.isolated))return true;
    if(g.device.Get()==device && g.context && g.context.Get()!=context)++stats.contextPointerMismatches;
    ++stats.initializations;
    g=State{};
    stats.currentContinueRun=0;
    if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return fail(reason,"flat-resolve-requires-immediate-context");
    ComPtr<ID3D11Device> contextDevice;context->GetDevice(contextDevice.GetAddressOf());
    if(contextDevice.Get()!=device)return fail(reason,"flat-resolve-context-device-mismatch");
    ComPtr<ID3D11Device1> d1;
    if(FAILED(context->QueryInterface(IID_PPV_ARGS(g.context.GetAddressOf()))))
        return fail(reason,"flat-resolve-requires-context-state-isolation");
    // Which isolation this device gets, said once in the log with the reason (flat_isolation_mode.h): a test's if it forces
    // one, else the device's, and a device that calls itself DXMT gets the explicit capture, whose swap aborts the process.
    const FlatDxmtDetection dxmt=flatDetectDxmt(device,context);
    const FlatContextIsolationChoice choice=flatChooseContextIsolation(g_isolationRequest,dxmt);
    g.capture=choice.mode==FlatContextIsolation::Capture;
    g.ranges=flatContextRanges(device->GetFeatureLevel(),dxmt.dxmt());
    stats.isolation=g.capture?"capture":"swap";
    if(isolationLogged<kIsolationLogCap) {
        ++isolationLogged;
        char line[448];flatFormatContextIsolationLine(choice,dxmt,line,sizeof(line));
        Log::get().note("%s",line);
    }
    if(!g.capture && FAILED(device->QueryInterface(IID_PPV_ARGS(d1.GetAddressOf()))))
        return fail(reason,"flat-resolve-requires-context-state-isolation");
    D3D_FEATURE_LEVEL level=device->GetFeatureLevel(),selected{};
    UINT flags=(device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED)?D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED:0;
    // The HDR route's crumbs bracket each creation that has never run on a DXMT device (flat_hdr_crumbs.h). E_PENDING in a
    // result is a call that was not reached.
    HRESULT hrState=E_PENDING;
    if(g.capture) {
        // The explicit capture makes no state object (DXMT's is a stub); the compute shaders need feature level 11_0 all the same.
        if(level<D3D_FEATURE_LEVEL_11_0)return fail(reason,"flat-resolve-requires-feature-level-11-0");
    } else {
        {
            HdrCrumbSpan span(g_crumbOn,"create-context-state","level=0x%X flags=%u",static_cast<unsigned>(level),static_cast<unsigned>(flags));
            if(level>=D3D_FEATURE_LEVEL_11_0)
                hrState=d1->CreateDeviceContextState(flags,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&selected,g.isolated.GetAddressOf());
            span.result("hr=0x%08X",static_cast<unsigned>(hrState));
        }
        if(level<D3D_FEATURE_LEVEL_11_0 || FAILED(hrState))return fail(reason,"flat-resolve-context-state-create-failed");
    }
    HRESULT hrShader[4]={E_PENDING,E_PENDING,E_PENDING,E_PENDING};
    bool shadersFailed;
    {
        HdrCrumbSpan span(g_crumbOn,"create-compute-shaders","prep,taa,finish,spatial");
        shadersFailed=
           FAILED(hrShader[0]=device->CreateComputeShader(kFlatMonoPrepBytecode,sizeof(kFlatMonoPrepBytecode),nullptr,g.prep.GetAddressOf())) ||
           FAILED(hrShader[1]=device->CreateComputeShader(kFlatMonoTaaBytecode,sizeof(kFlatMonoTaaBytecode),nullptr,g.taa.GetAddressOf())) ||
           FAILED(hrShader[2]=device->CreateComputeShader(kFlatMonoFinishBytecode,sizeof(kFlatMonoFinishBytecode),nullptr,g.finish.GetAddressOf())) ||
           FAILED(hrShader[3]=device->CreateComputeShader(kFlatMonoSpatialBytecode,sizeof(kFlatMonoSpatialBytecode),nullptr,g.spatial.GetAddressOf()));
        span.result("hr=0x%08X,0x%08X,0x%08X,0x%08X",static_cast<unsigned>(hrShader[0]),static_cast<unsigned>(hrShader[1]),
            static_cast<unsigned>(hrShader[2]),static_cast<unsigned>(hrShader[3]));
    }
    if(shadersFailed)return fail(reason,"flat-resolve-shader-create-failed");
    // Only a rig ever has bytes here (flatMonoResolveTestPrepBytecode, flatMonoResolveTestTaaBytecode): its mutated kernel replaces the
    // shipped one.
    if(!g_testPrepBytecode.empty() &&
       FAILED(device->CreateComputeShader(g_testPrepBytecode.data(),g_testPrepBytecode.size(),nullptr,g.prep.ReleaseAndGetAddressOf())))
        return fail(reason,"flat-resolve-shader-create-failed");
    if(!g_testTaaBytecode.empty() &&
       FAILED(device->CreateComputeShader(g_testTaaBytecode.data(),g_testTaaBytecode.size(),nullptr,g.taa.ReleaseAndGetAddressOf())))
        return fail(reason,"flat-resolve-shader-create-failed");
    HRESULT hrBuffer=E_PENDING,hrSampler=E_PENDING;
    {
        HdrCrumbSpan span(g_crumbOn,"create-constants-sampler","cbuffer=%u bytes",static_cast<unsigned>(sizeof(Constants)));
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Constants);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        hrBuffer=device->CreateBuffer(&cb,nullptr,g.constants.GetAddressOf());
        if(SUCCEEDED(hrBuffer)) {
            D3D11_SAMPLER_DESC sm{};sm.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sm.AddressU=sm.AddressV=sm.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sm.MaxLOD=D3D11_FLOAT32_MAX;
            hrSampler=device->CreateSamplerState(&sm,g.sampler.GetAddressOf());
        }
        span.result("cbuffer=0x%08X sampler=0x%08X",static_cast<unsigned>(hrBuffer),static_cast<unsigned>(hrSampler));
    }
    if(FAILED(hrBuffer))return fail(reason,"flat-resolve-constants-create-failed");
    if(FAILED(hrSampler))return fail(reason,"flat-resolve-sampler-create-failed");
    g.device=device;
    return true;
}
bool resources(const FlatMonoResolveFrame& f,const char** reason) {
    const auto route = flatResolveRoute(f.mode, f.renderWidth, f.renderHeight, f.outputWidth, f.outputHeight);
    const uint32_t evalW = route.refused ? f.outputWidth :
        (f.evalWidth && f.evalHeight) ? f.evalWidth : route.evalWidth;
    const uint32_t evalH = route.refused ? f.outputHeight :
        (f.evalWidth && f.evalHeight) ? f.evalHeight : route.evalHeight;
    // The negotiated E is part of the resource identity (gate-2 review F1): a
    // cache keyed only on mode/R/D would hand a D-sized cache to a cut E, and
    // the finish pass would fill only the E rectangle of it.
    if(g.width==f.renderWidth && g.height==f.renderHeight && g.outWidth==f.outputWidth &&
       g.outHeight==f.outputHeight && g.mode==f.mode && g.hdr==f.hdr &&
       g.evalWidth==evalW && g.evalHeight==evalH)return true;
    ++stats.allocations;
    g.color={};g.rawOverlay={};g.depth[0]={};g.depth[1]={};g.motion={};g.rejection={};g.expected={};g.output[0]={};g.output[1]={};
    g.outputDomain[0]={};g.outputDomain[1]={};g.untrustedCoverageLast=false;
    g.klass={};g.classWidth=g.classHeight=0;   // the refusal census's class texture is the render size: made again by a frame that asks
    g.width=g.height=g.outWidth=g.outHeight=0;g.evalWidth=g.evalHeight=0;g.current=0;g.depthLast=0;g.history=false;g.hdr=false;
    const bool taa=f.mode==FlatMonoResolveMode::Taa;
    // The images' names for the HDR route's crumbs: which of the private textures each creation is.
    const auto role=[&](const Image& i)->const char* {
        return &i==&g.color?"color":&i==&g.rawOverlay?"overlay-raw":&i==&g.depth[0]?"depth0":&i==&g.depth[1]?"depth1":&i==&g.motion?"motion":
               &i==&g.rejection?"rejection":&i==&g.expected?"expected":&i==&g.output[0]?"output0":"output1";
    };
    auto make=[&](Image& out,DXGI_FORMAT format,bool output=false,bool writable=true) {
        return image(g.device.Get(),output?evalW:f.renderWidth,output?evalH:f.renderHeight,format,out,writable,role(out));
    };
    bool made;
    if(f.hdr) {
        // The HDR route (section 81): the input copy is H's own format, the backend's output and TAA's ping-pong
        // history are fp16 (R11G11B10F holds no more than the game's own tone pass would see, and the history must
        // not be requantised every frame). The finish goes into H through a pixel shader, so output[1] is only TAA's
        // second history and is otherwise not made.
        made=make(g.color,DXGI_FORMAT_R11G11B10_FLOAT,false,false) && make(g.depth[0],DXGI_FORMAT_R32_FLOAT) &&
            make(g.motion,DXGI_FORMAT_R16G16_FLOAT) && make(g.rejection,DXGI_FORMAT_R8_UNORM) &&
            make(g.output[0],DXGI_FORMAT_R16G16B16A16_FLOAT,true) &&
            (!taa || (make(g.output[1],DXGI_FORMAT_R16G16B16A16_FLOAT,true) && make(g.depth[1],DXGI_FORMAT_R32_FLOAT) &&
                      make(g.expected,DXGI_FORMAT_R32_FLOAT)));
    } else {
        made=make(g.color,DXGI_FORMAT_R8G8B8A8_UNORM,false,false) && make(g.depth[0],DXGI_FORMAT_R32_FLOAT) &&
            make(g.motion,DXGI_FORMAT_R16G16_FLOAT) && make(g.rejection,DXGI_FORMAT_R8_UNORM) &&
            make(g.output[0],taa?DXGI_FORMAT_R8G8B8A8_TYPELESS:DXGI_FORMAT_R8G8B8A8_UNORM,true) &&
            make(g.output[1],DXGI_FORMAT_R8G8B8A8_TYPELESS,true) &&
            (!taa || (make(g.depth[1],DXGI_FORMAT_R32_FLOAT) && make(g.expected,DXGI_FORMAT_R32_FLOAT)));
    }
    if(!made)return fail(reason,"flat-resolve-texture-create-failed");
    g.width=f.renderWidth;g.height=f.renderHeight;g.outWidth=f.outputWidth;g.outHeight=f.outputHeight;g.mode=f.mode;
    g.evalWidth=evalW;g.evalHeight=evalH;g.hdr=f.hdr;
    return true;
}
// `hdr`: the colour is the game's HDR scene target H itself (FlatMonoResolveFrame::hdr): an R11G11B10_FLOAT shader
// view over an R11G11B10_FLOAT texture the game also renders into, because the result goes back through a render-target
// view the resolver makes over it.
bool inputTexture(ID3D11ShaderResourceView* view,uint32_t width,uint32_t height,bool color,ComPtr<ID3D11Texture2D>& out,bool hdr=false,bool requireTarget=true) {
    if(!view)return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};view->GetDesc(&srv);
    if(srv.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || srv.Texture2D.MostDetailedMip!=0 ||
       (srv.Texture2D.MipLevels!=1 && srv.Texture2D.MipLevels!=UINT(-1)))return false;
    if(color && !hdr && srv.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && srv.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)return false;
    if(color && hdr && srv.Format!=DXGI_FORMAT_R11G11B10_FLOAT)return false;
    if(!color && srv.Format!=DXGI_FORMAT_R32_FLOAT && srv.Format!=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS &&
       srv.Format!=DXGI_FORMAT_R24_UNORM_X8_TYPELESS && srv.Format!=DXGI_FORMAT_R16_UNORM)return false;
    ComPtr<ID3D11Resource> resource;view->GetResource(resource.GetAddressOf());
    if(FAILED(resource.As(&out)))return false;
    D3D11_TEXTURE2D_DESC desc{};out->GetDesc(&desc);
    ComPtr<ID3D11Device> device;out->GetDevice(device.GetAddressOf());
    if(color && hdr && (desc.Format!=DXGI_FORMAT_R11G11B10_FLOAT ||
       (requireTarget && !(desc.BindFlags&D3D11_BIND_RENDER_TARGET))))return false;
    return device.Get()==g.device.Get() && desc.Width==width && desc.Height==height && desc.MipLevels==1 &&
        desc.ArraySize==1 && desc.SampleDesc.Count==1 && (desc.BindFlags&D3D11_BIND_SHADER_RESOURCE)!=0;
}
// ---- the refusal census and view (FlatMonoResolveFrame::refusalCensus, refusalView; flat_mono_refusal.h) -------------------------------
// Everything here is made on first use by a frame that asks, never by initialize() or resources(): a frame that asks for neither
// (every flat frame, and a VR frame with the census key and the view off) touches none of it.
// The prep's class texture: one byte per render pixel (the class, and bit 7 for a refused history).
bool ensureClassTexture(uint32_t width,uint32_t height) {
    if(g.klass.texture && g.klass.srv && g.klass.uav && g.classWidth==width && g.classHeight==height)return true;
    g.klass={};g.classWidth=g.classHeight=0;
    if(!image(g.device.Get(),width,height,DXGI_FORMAT_R8_UINT,g.klass,true,"class")) {g.klass={};return false;}
    g.classWidth=width;g.classHeight=height;
    return true;
}
// The steady-detail depth check's second depth image for the backends that keep none (everything but EDVR's own TAA, whose depth[1]
// resources() makes): made on the first frame that asks, at the render size, like depth[0] (R32_FLOAT, shader and unordered access).
// A reallocation by resources() drops it with the rest and the next asking frame makes it again.
bool ensureSecondDepth(uint32_t width,uint32_t height) {
    if(g.depth[1].texture && g.depth[1].srv && g.depth[1].uav)return true;
    g.depth[1]={};
    if(!image(g.device.Get(),width,height,DXGI_FORMAT_R32_FLOAT,g.depth[1],true,"depth1")) {g.depth[1]={};return false;}
    return true;
}
// Only a flat mixed-camera TAA frame needs output-domain history. These two
// images follow output[2]'s ping-pong index and resource-size key.
bool ensureOutputDomains(uint32_t width,uint32_t height) {
    if(g.outputDomain[0].srv && g.outputDomain[0].uav &&
       g.outputDomain[1].srv && g.outputDomain[1].uav)return true;
    g.outputDomain[0]={};g.outputDomain[1]={};
    return image(g.device.Get(),width,height,DXGI_FORMAT_R8_UNORM,g.outputDomain[0],true,"output-domain0") &&
           image(g.device.Get(),width,height,DXGI_FORMAT_R8_UNORM,g.outputDomain[1],true,"output-domain1");
}
// The counting pass, its counter buffer (16 stripes of 24 counters, raw, UAV) and the four-slot staging ring the sums are read
// back from.
bool ensureRefusalCensus() {
    if(g.census && g.refusalCounts && g.refusalCountsUav && g.refusalStaging[0] && g.refusalStaging[1] && g.refusalStaging[2] && g.refusalStaging[3])
        return true;
    ID3D11Device* device=g.device.Get();
    if(!device)return false;
    if(!g.census && FAILED(device->CreateComputeShader(kFlatMonoCensusBytecode,sizeof(kFlatMonoCensusBytecode),nullptr,g.census.GetAddressOf())))
        return false;
    const UINT bytes=kFlatMonoRefusalStripes*kFlatMonoRefusalCounters*4u;
    if(!g.refusalCounts) {
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=bytes;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if(FAILED(device->CreateBuffer(&bd,nullptr,g.refusalCounts.GetAddressOf())))return false;
    }
    if(!g.refusalCountsUav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=DXGI_FORMAT_R32_TYPELESS;ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements=bytes/4u;ud.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        if(FAILED(device->CreateUnorderedAccessView(g.refusalCounts.Get(),&ud,g.refusalCountsUav.GetAddressOf())))return false;
    }
    for(auto& staging:g.refusalStaging) {
        if(staging)continue;
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=bytes;bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(device->CreateBuffer(&bd,nullptr,staging.GetAddressOf())))return false;
    }
    return true;
}
// Reads back the samples the GPU has finished, oldest first, without waiting; a slot still in flight ends the pass (the ring is
// read in order, so a later sample is never counted before an earlier one).
void pollRefusalCensus(ID3D11DeviceContext* context) {
    if(!context)return;
    for(uint32_t k=0;k<4;++k) {
        const uint32_t i=(g.refusalWrite+k)%4;
        if(!g.refusalPending[i])continue;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(context->Map(g.refusalStaging[i].Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped)!=S_OK)break;
        const uint32_t* sums=static_cast<const uint32_t*>(mapped.pData);
        for(uint32_t stripe=0;stripe<kFlatMonoRefusalStripes;++stripe) {
            const uint32_t* row=sums+stripe*kFlatMonoRefusalCounters;
            for(uint32_t slot=0;slot<kFlatMonoRefusalSlots;++slot)g_refusal.counts[slot]+=row[slot];
            for(uint32_t reason=0;reason<kFlatMonoWeaponReasons;++reason)g_refusal.weaponReasons[reason]+=row[kFlatMonoRefusalSlots+reason];
            g_refusal.skinned+=row[kFlatMonoRefusalSkinned];
        }
        context->Unmap(g.refusalStaging[i].Get(),0);
        ++g_refusal.frames;
        g_refusal.pixels+=static_cast<uint64_t>(g.refusalWidth[i])*g.refusalHeight[i];
        g_refusal.width=g.refusalWidth[i];g_refusal.height=g.refusalHeight[i];
        g.refusalPending[i]=false;
    }
}
// The ring is consumed in order, so the next sample may be taken only if the slot it would write is not still waiting to be read.
bool refusalSlotFree() { return !g.refusalPending[g.refusalWrite]; }
// The first-person inputs (FlatMonoResolveFrame::firstPersonMotion and firstPersonStencil, section 82), checked the way
// inputTexture checks the colour and depth views, and answered by name: null when the view is fit to bind, else a static
// reason. The reason goes to stats.firstPersonRefusal and the log; a refusal never refuses the frame, it only drops the pair.
const char* firstPersonViewRefusal(ID3D11ShaderResourceView* view,uint32_t width,uint32_t height,bool stencil) {
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};view->GetDesc(&srv);
    if(srv.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D)
        return stencil?"the stencil view is not a Texture2D view":"the map view is not a Texture2D view";
    if(srv.Format!=(stencil?DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:DXGI_FORMAT_R16G16B16A16_FLOAT))
        return stencil?"the stencil view format is not X32_TYPELESS_G8X24_UINT":"the map view format is not R16G16B16A16_FLOAT";
    if(srv.Texture2D.MostDetailedMip!=0 || (srv.Texture2D.MipLevels!=1 && srv.Texture2D.MipLevels!=UINT(-1)))
        return stencil?"the stencil view is not a one-mip view":"the map view is not a one-mip view";
    ComPtr<ID3D11Resource> resource;view->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    if(FAILED(resource.As(&texture)))
        return stencil?"the stencil resource is not a Texture2D":"the map resource is not a Texture2D";
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    ComPtr<ID3D11Device> device;texture->GetDevice(device.GetAddressOf());
    if(device.Get()!=g.device.Get() || desc.MipLevels!=1 || desc.ArraySize!=1 || desc.SampleDesc.Count!=1 ||
       !(desc.BindFlags&D3D11_BIND_SHADER_RESOURCE))
        return stencil?"the stencil texture is not a one-slice, one-sample shader resource on the resolver's device"
                      :"the map texture is not a one-slice, one-sample shader resource on the resolver's device";
    if(desc.Width!=width || desc.Height!=height)
        return stencil?"the stencil texture is not the render size":"the map texture is not the render size";
    return nullptr;
}
// The HDR route's pixel-shader half, created on first use so the copy route never pays for it.
bool initializeHdr(ID3D11Device* device,const char** reason) {
    if(g.hdrVs && g.finishHdr && g.spatialHdr && g.noCull)return true;
    // One crumb pair per shader: these three are the route's own, and the first D3D11 shaders of it any DXMT device sees.
    const auto made=[&](const char* step,auto&& create) {
        HdrCrumbSpan span(g_crumbOn,step);
        const HRESULT hr=create();
        span.result("hr=0x%08X",static_cast<unsigned>(hr));
        return hr;
    };
    if(FAILED(made("create-hdr-vs",[&]{return device->CreateVertexShader(kFlatMonoHdrVsBytecode,sizeof(kFlatMonoHdrVsBytecode),nullptr,g.hdrVs.GetAddressOf());})) ||
       FAILED(made("create-hdr-ps-finish",[&]{return device->CreatePixelShader(kFlatMonoFinishHdrBytecode,sizeof(kFlatMonoFinishHdrBytecode),nullptr,g.finishHdr.GetAddressOf());})) ||
       FAILED(made("create-hdr-ps-spatial",[&]{return device->CreatePixelShader(kFlatMonoSpatialHdrBytecode,sizeof(kFlatMonoSpatialHdrBytecode),nullptr,g.spatialHdr.GetAddressOf());})))
        return fail(reason,"flat-resolve-hdr-shader-create-failed");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    if(FAILED(device->CreateRasterizerState(&rd,g.noCull.GetAddressOf())))return fail(reason,"flat-resolve-hdr-rasterizer-create-failed");
    return true;
}
// A render-target view over the game's H, made before anything is written and kept while H is the same texture.
bool hdrTargetView(ID3D11Texture2D* texture,const char** reason) {
    if(g.hdrRtv && g.hdrRtvTexture==texture)return true;
    g.hdrRtv.Reset();g.hdrRtvTexture=nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC rv{};rv.Format=DXGI_FORMAT_R11G11B10_FLOAT;rv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    HRESULT hr;
    {
        D3D11_TEXTURE2D_DESC td{};
        if(g_crumbOn)texture->GetDesc(&td);   // only to say what the view is over
        HdrCrumbSpan span(g_crumbOn,"create-rtv","over H fmt=%s(%u) size=%ux%u",hdrCrumbFormat(static_cast<uint32_t>(rv.Format)),
            static_cast<unsigned>(rv.Format),td.Width,td.Height);
        hr=g.device->CreateRenderTargetView(texture,&rv,g.hdrRtv.GetAddressOf());
        span.result("hr=0x%08X",static_cast<unsigned>(hr));
    }
    if(FAILED(hr))return fail(reason,"flat-resolve-hdr-target-not-renderable");
    g.hdrRtvTexture=texture;
    return true;
}
// One triangle over the render-size target: the pixel shader `ps` (finish or spatial recovery) into H. The caller has
// already isolated the context; this starts from its own clean state, as the compute finish does after an SDK.
void drawHdrTarget(ID3D11DeviceContext* context,ID3D11PixelShader* ps,uint32_t width,uint32_t height,
                   ID3D11ShaderResourceView* const* views,uint32_t viewCount) {
    const char* which=ps==g.spatialHdr.Get()?"spatial":"finish";   // for the crumbs: the route's two pixel shaders
    {
        // H bound as the render target, with everything the draw reads: the crumbs name this step apart from the draw.
        HdrCrumbSpan bind(g_crumbOn,"finish-bind","ps=%s target=%ux%u views=%u",which,width,height,viewCount);
        context->ClearState();
        ID3D11Buffer* cb0=g.constants.Get();context->PSSetConstantBuffers(0,1,&cb0);
        context->PSSetShaderResources(0,viewCount,views);
        ID3D11SamplerState* sampler=g.sampler.Get();context->PSSetSamplers(0,1,&sampler);
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(g.hdrVs.Get(),nullptr,0);context->PSSetShader(ps,nullptr,0);
        context->RSSetState(g.noCull.Get());
        const D3D11_VIEWPORT vp{0.0f,0.0f,static_cast<float>(width),static_cast<float>(height),0.0f,1.0f};
        context->RSSetViewports(1,&vp);
        ID3D11RenderTargetView* rtv=g.hdrRtv.Get();context->OMSetRenderTargets(1,&rtv,nullptr);
    }
    {
        HdrCrumbSpan draw(g_crumbOn,"finish-draw","ps=%s vertices=3",which);
        context->Draw(3,0);
        ++stats.hdrFinished;   // drawHdrTarget is the HDR route's alone: the resolve's finish and the spatial recovery's
    }
    // Nothing of ours stays bound: the isolation guard's destructor clears the state once more before the game's returns.
    context->OMSetRenderTargets(0,nullptr,nullptr);
    ID3D11ShaderResourceView* none[17]={};context->PSSetShaderResources(0,viewCount,none);   // t12 is the optional late-overlay mask, t16 its raw H
}
// The backend's availability ask, where the asker is the HDR route: the first such ask of a session is the SDK's own
// initialisation (NGX's, or AMD's), the first call into code that has never run on a DXMT device, so the crumbs bracket it.
// Later asks are a flag test and are not written. DLAA and DLSS share NGX.
bool backendAvailable(FlatMonoResolveMode mode,ID3D11Device* device,const char** reason) {
    static bool crumbed[2]={false,false};
    const unsigned which=mode==FlatMonoResolveMode::Fsr?1u:0u;
    const bool first=g_crumbOn && !crumbed[which];
    if(first)crumbed[which]=true;
    HdrCrumbSpan span(first,"backend-available","backend=%s",which?"fsr":"ngx");
    const bool ok=which?fsr3Available(device,reason):dlaaAvailable(device,reason);
    span.result("ok=%u reason=%s",ok?1u:0u,(reason && *reason)?*reason:"none");
    return ok;
}
} // namespace

FlatMonoResolveStats flatMonoResolveStats() { return stats; }
bool flatMonoResolveLastReset() { return stats.lastReset; }
FlatMonoRefusalCensus flatMonoResolveTakeRefusalCensus() {
    if(g.context)pollRefusalCensus(g.context.Get());
    FlatMonoRefusalCensus out=g_refusal;
    g_refusal=FlatMonoRefusalCensus{};
    return out;
}
// The preflight proper; the public entry below puts the route's crumbs around it.
static FlatMonoResolvePreflightResult preflightBody(ID3D11Device* device,
    ID3D11DeviceContext* context,const FlatMonoResolvePreflight& planned) {
    FlatMonoResolvePreflightResult result{};
    const char* why=nullptr;
    if(!preflightMetadataValid(planned,&why)) {
        result.status=FlatMonoResolvePreflightStatus::InvalidMetadata;
        result.reason=why;
        return result;
    }
    if(!device || !context) {
        result.status=FlatMonoResolvePreflightStatus::RendererUnavailable;
        result.reason="flat-preflight-missing-device-or-context";
        return result;
    }
    if(!initialize(device,context,&why)) {
        result.status=FlatMonoResolvePreflightStatus::RendererUnavailable;
        result.reason=why?why:"flat-preflight-renderer-initialize-failed";
        return result;
    }
    result.rendererReady=true;
    FlatMonoResolveFrame frame{};
    frame.renderWidth=planned.renderWidth;frame.renderHeight=planned.renderHeight;
    frame.outputWidth=planned.outputWidth;frame.outputHeight=planned.outputHeight;
    frame.mode=planned.mode;
    frame.hdr=planned.hdr;
    frame.evalWidth=planned.evalWidth;frame.evalHeight=planned.evalHeight;
    // The HDR route's pixel-shader half is part of what must be ready before a frame may be jittered for it.
    if(planned.hdr && !initializeHdr(device,&why)) {
        result.status=FlatMonoResolvePreflightStatus::FallbackUnavailable;
        result.reason=why?why:"flat-preflight-hdr-shaders-unavailable";
        return result;
    }
    if(!resources(frame,&why)) {
        result.status=FlatMonoResolvePreflightStatus::FallbackUnavailable;
        result.reason=why?why:"flat-preflight-resource-allocation-failed";
        return result;
    }
    const auto plannedRoute=flatResolveRoute(planned.mode,planned.renderWidth,planned.renderHeight,
                                             planned.outputWidth,planned.outputHeight);
    const uint32_t plannedEvalW=plannedRoute.refused?planned.outputWidth:
        (planned.evalWidth&&planned.evalHeight)?planned.evalWidth:plannedRoute.evalWidth;
    const uint32_t plannedEvalH=plannedRoute.refused?planned.outputHeight:
        (planned.evalWidth&&planned.evalHeight)?planned.evalHeight:plannedRoute.evalHeight;
    // The copy route's fallback is the compute spatial pass into output[1]; the HDR route's is the pixel shader into H.
    const bool fallbackResources=planned.hdr
        ? (g.hdrVs && g.spatialHdr && g.finishHdr && g.noCull && g.color.texture && g.output[0].texture && g.output[0].srv)
        : (g.output[1].texture && g.output[1].srv && g.output[1].uav);
    result.spatialFallbackReady=g.spatial && g.constants && g.sampler && fallbackResources &&
        g.width==planned.renderWidth && g.height==planned.renderHeight &&
        g.outWidth==planned.outputWidth && g.outHeight==planned.outputHeight &&
        g.evalWidth==plannedEvalW && g.evalHeight==plannedEvalH &&
        g.mode==planned.mode && g.hdr==planned.hdr;
    if(!result.spatialFallbackReady) {
        result.status=FlatMonoResolvePreflightStatus::FallbackUnavailable;
        result.reason="flat-preflight-spatial-output-not-ready";
        return result;
    }
    result.backendFeatureCreationDeferred=planned.mode!=FlatMonoResolveMode::Taa;
    // DLAA and DLSS ask NGX, FSR asks AMD's port, EDVR's own TAA needs no SDK.
    if(planned.mode!=FlatMonoResolveMode::Taa)
        result.backendAvailable=backendAvailable(planned.mode,device,&why);
    else result.backendAvailable=true;
    if(!result.backendAvailable) {
        result.status=FlatMonoResolvePreflightStatus::BackendUnavailable;
        result.reason=why?why:"flat-preflight-backend-unavailable";
        return result;
    }
    result.status=FlatMonoResolvePreflightStatus::Ready;
    result.reason=result.backendFeatureCreationDeferred?
        "ready-backend-feature-creation-deferred":"ready";
    return result;
}
FlatMonoResolvePreflightResult flatMonoResolvePreflight(ID3D11Device* device,
    ID3D11DeviceContext* context,const FlatMonoResolvePreflight& planned) {
    // The HDR route's plan, at the Present that ends the frame that made it: the route's crumbs bracket the whole preflight
    // and, through g_crumbOn, every creation inside it (flat_hdr_crumbs.h). A copy-route plan writes nothing.
    CrumbScope crumbs(planned.hdr);
    HdrCrumbSpan span(g_crumbOn,"preflight","plan=%ux%u->%ux%u mode=%s",planned.renderWidth,planned.renderHeight,
        planned.outputWidth,planned.outputHeight,flatMonoResolveModeName(planned.mode));
    const FlatMonoResolvePreflightResult result=preflightBody(device,context,planned);
    span.result("status=%u ready=%u reason=%s",static_cast<unsigned>(result.status),result.readyForRasterJitter()?1u:0u,
        result.reason?result.reason:"none");
    return result;
}
void flatMonoResolveReset() { pixels.cancel();resolveInputs.cancel();++stats.fullResets;stats.currentContinueRun=0;g=State{}; }
void flatMonoResolveSetIsolation(FlatContextIsolation request) { g_isolationRequest=request; }
void flatMonoResolveArmPixels(uint64_t frame) {
    try { pixels.arm(frame); } catch(...) { pixels.cancel(); }
    try { resolveInputs.arm(frame); } catch(...) { resolveInputs.cancel(); }
}
void flatMonoResolvePollPixels(ID3D11DeviceContext* context,uint64_t frame) {
    if(resolveInputs.active()) {
        try { resolveInputs.poll(context,frame); } catch(...) { resolveInputs.cancel(); }
    }
    if(!pixels.active())return;
    try { pixels.poll(context,frame); } catch(...) { pixels.cancel(); }
}
void flatMonoResolveInvalidateHistory() { ++stats.invalidations;stats.currentContinueRun=0;g.history=false; }
void flatMonoResolveSetSpanHooks(FlatMonoResolveSpanFn begin, FlatMonoResolveSpanFn end) {
    // Both or neither: a begin without its end would leave a span open.
    g_spanBegin = begin && end ? begin : nullptr;
    g_spanEnd = begin && end ? end : nullptr;
}

bool flatMonoResolve(ID3D11Device* device,ID3D11DeviceContext* context,const FlatMonoResolveFrame& f,
                     ID3D11ShaderResourceView** output,const char** reason) {
    CrumbScope crumbs(f.hdr);   // the HDR route's breadcrumbs (flat_hdr_crumbs.h) for every step below
    ++stats.calls;
    if(output)*output=nullptr;
    if(reason)*reason=nullptr;
    if(!output || !device || !context || !f.renderWidth || !f.renderHeight || !f.outputWidth || !f.outputHeight ||
       f.renderWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || f.renderHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       f.outputWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || f.outputHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       !std::isfinite(f.deltaMs) || f.deltaMs<0 || !cameraValid(f.camera) || !jitterValid(f))return fail(reason,"flat-resolve-invalid-frame");
    // The phase the rows themselves carry, as the NDC shift the shader removes (flat_camera_phase.h). A phase that
    // cannot be turned into a shift refuses the frame: reprojecting through rows still carrying it would be the
    // half-pixel error this exists to remove, silently.
    FlatProjectionJitter rowsNow{},rowsBefore{};
    if(!rowsJitterValid(f) ||
       !flatProjectionJitter(f.rowsJitterX,f.rowsJitterY,f.renderWidth,f.renderHeight,rowsNow) ||
       !flatProjectionJitter(f.previousRowsJitterX,f.previousRowsJitterY,f.renderWidth,f.renderHeight,rowsBefore))
        return fail(reason,"flat-resolve-invalid-rows-jitter");
    if(f.mode!=FlatMonoResolveMode::Taa && f.mode!=FlatMonoResolveMode::Dlaa && f.mode!=FlatMonoResolveMode::Dlss &&
       f.mode!=FlatMonoResolveMode::Fsr)return fail(reason,"flat-resolve-invalid-mode");
    // The upscaler slot (FlatMonoResolveFrame::slot): a slot no engine has refuses the frame here, before anything is made or written.
    if(f.slot>=kUpscalerSlots)return fail(reason,"flat-resolve-invalid-slot");
    // Gate 2 step 2 (design doc section 72): the route table owns the size
    // refusals. NVIDIA supersampling (R > D) evaluates DLAA at R and lets the
    // game's own copy downsample E = R to D; FSR refuses R > D until the
    // port's Native AA qualifies.
    const auto route = flatResolveRoute(f.mode, f.renderWidth, f.renderHeight, f.outputWidth, f.outputHeight);
    if (route.refused) return fail(reason, route.failReason);
    // The evaluation grid E from the route contract, with the driver's
    // negotiated override honored when present (gate 2 step 4). One contract
    // drives allocation, dispatch and telemetry (gate-2 review G2-2).
    const uint32_t evalW = (!route.refused && f.evalWidth && f.evalHeight) ? f.evalWidth : route.evalWidth;
    const uint32_t evalH = (!route.refused && f.evalWidth && f.evalHeight) ? f.evalHeight : route.evalHeight;
    // The HDR route resolves at E = R into H itself (section 81): a render below the output, or a mode that evaluates
    // on another grid, is the copy route's.
    const bool hdr=f.hdr;
    if(hdr && (evalW!=f.renderWidth || evalH!=f.renderHeight || f.renderWidth<f.outputWidth || f.renderHeight<f.outputHeight))
        return fail(reason,"flat-resolve-hdr-requires-render-size-evaluation");
    const bool untrusted=f.untrustedCameraCoverage!=nullptr;
    // Manual evidence observes full input planes even when the SDK guard
    // below refuses this attempt. It never implies backend evaluation.
    if(resolveInputs.active()) {
        try { resolveInputs.capture(device,context,f); } catch(...) { resolveInputs.cancel(); }
    }
    // The first-person map belongs to neither route (section 104): the HDR route qualifies it at its trigger, the copy route at the
    // game's final copy (a render below the output with DLSS or FSR: flatWeaponRoute, flat_copy_structure.h), and the prep reads it
    // the same either way, a per-pixel owner-and-motion map at the render size whatever the colour's format. A frame that requires it
    // and has no qualified map of its own frame is refused here, before anything is made or written.
    const bool foreground = f.mode!=FlatMonoResolveMode::Taa && f.foregroundMotion;
    if(f.mode!=FlatMonoResolveMode::Taa && (foreground || f.foregroundRequired) &&
       (!foreground || !f.foregroundQualified || f.foregroundFrame!=f.frame))
        return fail(reason,"flat-resolve-foreground-contract-unqualified");
    const float sdkNear=foreground && f.foregroundDepthNear!=0?f.foregroundDepthNear:f.camera[3][2];
    if(foreground && (!std::isfinite(sdkNear) || sdkNear<=0 || sdkNear>f.camera[3][2]))
        return fail(reason,"flat-resolve-foreground-depth-convention-invalid");
    // Untrusted camera coverage (a mask, the HDR route's way of handling a mixed-camera frame) needs the HDR route, and the copy route
    // never takes one: its weapon support hands an SDK backend the qualified first-person map above instead. An SDK backend needs that
    // map, which takes the mask's place (the prep's debug.w is 2 then, and the mask is never read); EDVR's TAA needs native size
    // besides, because its output-domain test maps one output pixel to one render pixel. A supersampled SDK frame (render above output)
    // is the SDK's own business at the render size: the first-person map, the prep and the backend all run there (section 104; the rule
    // was written for TAA in section 102 and refused every on-foot SDK frame above SS 1).
    if(untrusted && ((!foreground && f.mode!=FlatMonoResolveMode::Taa) || !hdr ||
                     (f.mode==FlatMonoResolveMode::Taa && (f.outputWidth!=f.renderWidth || f.outputHeight!=f.renderHeight))))
        return fail(reason,"flat-resolve-untrusted-coverage-requires-native-HDR-TAA");
    if(!initialize(device,context,reason))return false;
    if(hdr && !initializeHdr(device,reason))return false;
    pollRefusalCensus(context);   // the samples the GPU finished since the last call (nothing pending: one flag test per slot)
    ComPtr<ID3D11Texture2D> color,depth,cleanColor,overlayMask,untrustedMask;
    const bool overlay=f.cleanColor || f.overlayCoverage;
    if(overlay && (!hdr || !f.cleanColor || !f.overlayCoverage))
        return fail(reason,"flat-resolve-overlay-input-pair-incomplete");
    if(!inputTexture(f.color,f.renderWidth,f.renderHeight,true,color,hdr) ||
       !inputTexture(f.depth,f.renderWidth,f.renderHeight,false,depth))return fail(reason,"flat-resolve-input-view-mismatch");
    if(overlay) {
        if(!inputTexture(f.cleanColor,f.renderWidth,f.renderHeight,true,cleanColor,true,false) ||
           cleanColor.Get()==color.Get())
            return fail(reason,"flat-resolve-clean-HDR-view-mismatch");
        D3D11_SHADER_RESOURCE_VIEW_DESC mv{};f.overlayCoverage->GetDesc(&mv);
        ComPtr<ID3D11Resource> mr;f.overlayCoverage->GetResource(&mr);
        if(!mr || FAILED(mr.As(&overlayMask)))
            return fail(reason,"flat-resolve-overlay-mask-resource-mismatch");
        D3D11_TEXTURE2D_DESC md{};overlayMask->GetDesc(&md);
        ComPtr<ID3D11Device> maskDevice;overlayMask->GetDevice(&maskDevice);
        if(mv.Format!=DXGI_FORMAT_R8_UNORM || mv.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D ||
           mv.Texture2D.MostDetailedMip!=0 ||
           (mv.Texture2D.MipLevels!=1 && mv.Texture2D.MipLevels!=UINT(-1)) ||
           md.Format!=DXGI_FORMAT_R8_UNORM || md.Width!=f.renderWidth || md.Height!=f.renderHeight ||
           md.MipLevels!=1 || md.ArraySize!=1 || md.SampleDesc.Count!=1 ||
           !(md.BindFlags&D3D11_BIND_SHADER_RESOURCE) || maskDevice.Get()!=g.device.Get())
            return fail(reason,"flat-resolve-overlay-mask-view-mismatch");
    }
    if(untrusted) {
        D3D11_SHADER_RESOURCE_VIEW_DESC uv{};f.untrustedCameraCoverage->GetDesc(&uv);
        ComPtr<ID3D11Resource> resource;f.untrustedCameraCoverage->GetResource(&resource);
        if(!resource || FAILED(resource.As(&untrustedMask)))
            return fail(reason,"flat-resolve-untrusted-coverage-resource-mismatch");
        D3D11_TEXTURE2D_DESC md{};untrustedMask->GetDesc(&md);
        ComPtr<ID3D11Device> maskDevice;untrustedMask->GetDevice(&maskDevice);
        if(uv.Format!=DXGI_FORMAT_R8_UNORM || uv.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D ||
           uv.Texture2D.MostDetailedMip!=0 ||
           (uv.Texture2D.MipLevels!=1 && uv.Texture2D.MipLevels!=UINT(-1)) ||
           md.Format!=DXGI_FORMAT_R8_UNORM || md.Width!=f.renderWidth || md.Height!=f.renderHeight ||
           md.MipLevels!=1 || md.ArraySize!=1 || md.SampleDesc.Count!=1 ||
           !(md.BindFlags&D3D11_BIND_SHADER_RESOURCE) || maskDevice.Get()!=g.device.Get())
            return fail(reason,"flat-resolve-untrusted-coverage-view-mismatch");
    }
    if(foreground) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};f.foregroundMotion->GetDesc(&view);
        ComPtr<ID3D11Resource> resource;f.foregroundMotion->GetResource(&resource);
        ComPtr<ID3D11Texture2D> map;
        if(!resource || FAILED(resource.As(&map)))
            return fail(reason,"flat-resolve-foreground-resource-mismatch");
        D3D11_TEXTURE2D_DESC desc{};map->GetDesc(&desc);
        ComPtr<ID3D11Device> owner;map->GetDevice(&owner);
        if(view.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT || view.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D ||
           view.Texture2D.MostDetailedMip!=0 ||
           (view.Texture2D.MipLevels!=1 && view.Texture2D.MipLevels!=UINT(-1)) ||
           desc.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT || desc.Width!=f.renderWidth || desc.Height!=f.renderHeight ||
           desc.MipLevels!=1 || desc.ArraySize!=1 || desc.SampleDesc.Count!=1 ||
           !(desc.BindFlags&D3D11_BIND_SHADER_RESOURCE) || owner.Get()!=g.device.Get())
            return fail(reason,"flat-resolve-foreground-view-mismatch");
    }
    if(!resources(f,reason))return false;
    if(untrusted && f.mode==FlatMonoResolveMode::Taa && !ensureOutputDomains(f.outputWidth,f.outputHeight))
        return fail(reason,"flat-resolve-output-domain-create-failed");
    if(overlay && !g.rawOverlay.texture &&
       !image(g.device.Get(),f.renderWidth,f.renderHeight,DXGI_FORMAT_R11G11B10_FLOAT,
              g.rawOverlay,false,"overlay-raw"))
        return fail(reason,"flat-resolve-overlay-raw-create-failed");
    // The render-target view over H exists before anything is written: a game texture the resolver cannot render into
    // refuses the frame while H is still the game's own.
    if(hdr && !hdrTargetView(color.Get(),reason))return false;
    const bool taa=f.mode==FlatMonoResolveMode::Taa;
    D3D11_SHADER_RESOURCE_VIEW_DESC colorDesc{};f.color->GetDesc(&colorDesc);
    const float sdkDepthScale=foreground?sdkNear/f.camera[3][2]:1.f;
    const bool depthConventionTransition=g.history && sdkDepthScale!=g.sdkDepthScaleLast;
    const bool requestedReset=f.reset || (foreground && f.foregroundResetRequired) || depthConventionTransition, lostHistory=!g.history, frameGap=f.frame!=g.lastFrame+1;
    const bool coverageTransition=g.history && untrusted!=g.untrustedCoverageLast;
    const bool invalidPreviousCamera=!cameraValid(f.previousCamera);
    const bool formatChange=g.inputFormat!=colorDesc.Format;
    bool cameraCut=false;
    if(!requestedReset && !lostHistory && !frameGap && !invalidPreviousCamera && !formatChange)
        for(unsigned i=0;i<3;++i)if(std::abs(f.camera[5][i]-f.previousCamera[5][i])>50)cameraCut=true;
    const bool reset=requestedReset || lostHistory || frameGap || invalidPreviousCamera || formatChange || cameraCut || coverageTransition;
    stats.lastReset=reset;
    const bool engine=f.engine.slots && f.engine.pool && f.engine.sceneNow && f.engine.scenePrev;
    if(!reset && !engine)return fail(reason,"flat-resolve-engine-source-views-unavailable");
    // F2 on foot: the source pass's target 7 (only the VR world route has one; the flat profile's views carry none), bound at t17 for the prep with the debug.w bit.
    const bool skin=engine && f.engine.skin!=nullptr;
    // All external backend work is inside the same complete state isolation.
    Isolate isolated(g.context.Get(),g.isolated.Get(),g.capture);
    // DLAA and DLSS ask NGX, FSR asks AMD's port; EDVR's own TAA needs no SDK.
    if(f.mode!=FlatMonoResolveMode::Taa && !backendAvailable(f.mode,device,reason)) {++stats.backendFailures;g.history=false;stats.currentContinueRun=0;return false;}
    // The first-person inputs (section 82): validated here, beside the colour and depth views, and bound for the prep kernel
    // only when the pair is whole and fit. A missing half is an absent pair; an unfit one is named, counted and dropped.
    ID3D11ShaderResourceView* firstPersonMap=nullptr,* firstPersonStencil=nullptr;
    if(f.firstPersonMotion && f.firstPersonStencil) {
        const char* refusal=firstPersonViewRefusal(f.firstPersonMotion,f.renderWidth,f.renderHeight,false);
        if(!refusal)refusal=firstPersonViewRefusal(f.firstPersonStencil,f.renderWidth,f.renderHeight,true);
        if(!refusal) {
            firstPersonMap=f.firstPersonMotion;firstPersonStencil=f.firstPersonStencil;
            ++stats.firstPersonFrames;++stats.firstPersonPhaseFrames[f.firstPersonPhaseMode<2u?f.firstPersonPhaseMode:2u];
            if(!firstPersonBoundLogged) {
                firstPersonBoundLogged=true;
                Log::get().note("flat resolve: first-person motion inputs bound (map %ux%u, stencil view)",f.renderWidth,f.renderHeight);
            }
        } else {
            ++stats.firstPersonRefused;stats.firstPersonRefusal=refusal;
            if(!firstPersonRefusedLogged) {
                firstPersonRefusedLogged=true;
                Log::get().note("flat resolve: first-person motion inputs refused: %s",refusal);
            }
        }
    } else if(f.firstPersonMotion || f.firstPersonStencil) ++stats.firstPersonPartial;
    // The refusal census and view. Neither runs on a reset frame (every pixel is refused there, which says nothing) and the view is the
    // HDR route's alone. A frame that asks for neither reaches the end of this block having touched nothing: no resource is made.
    // The HDR route paints in its finish draw; the copy route with an SDK backend in its compute finish. EDVR's own TAA on the copy
    // route writes its output directly, with no finish to paint in.
    const bool paintView=(hdr || !taa) && f.refusalView!=0 && !reset;
    bool sampleNow=false;
    if(f.refusalCensus && !reset) {
        ++g_refusal.asked;
        if(g_refusalCadence++%kFlatMonoRefusalEvery==0) {
            if(refusalSlotFree())sampleNow=true;
            else ++g_refusal.dropped;
        }
    }
    bool paintNow=paintView;
    if(sampleNow || paintNow) {
        if(!ensureClassTexture(f.renderWidth,f.renderHeight) || (sampleNow && !ensureRefusalCensus())) {
            // A diagnostic never refuses the frame: it says so once and the frame runs as if nothing had asked.
            sampleNow=false;paintNow=false;
            if(!g_refusalFailureLogged) {
                g_refusalFailureLogged=true;
                Log::get().note("flat resolve: the refusal census/view could not make its resources (class texture, counting pass or "
                                "counter buffer); it stays off and the frame resolves as usual");
            }
        }
    }
    const bool needClass=sampleNow || paintNow;
    const uint32_t index=taa?g.current:0;
    // THE STEADY-DETAIL DEPTH CHECK (FlatMonoResolveFrame::steadyDetail). The menu's blanket policy wins, so a frame with staticScene has nothing
    // for the check to do. Otherwise the frame needs last frame's depth beside the one it writes: EDVR's TAA has it (depth[index^1]); the
    // other backends get depth[1] here, on the first frame that asks, and the depth they are handed alternates between the two images from then
    // on. A frame that does not ask never touches depth[1]: it writes depth[0], the backend reads depth[0], as always. A reset frame refuses
    // every pixel anyway (the prep never reaches a stale slot), so it only keeps the alternation going.
    const bool steady=f.steadyDetail && !f.staticScene;
    uint32_t depthIndex=index,prevDepthIndex=taa?(index^1u):0u;
    bool steadyAvailable=false;
    if(steady) {
        if(taa)steadyAvailable=true;
        else if(ensureSecondDepth(f.renderWidth,f.renderHeight)) {
            steadyAvailable=true;depthIndex=g.depthLast^1u;prevDepthIndex=g.depthLast;
        } else if(!g_steadyFailureLogged) {
            g_steadyFailureLogged=true;
            Log::get().note("flat resolve: the steady-detail depth check could not make its second depth image (%ux%u R32_FLOAT); the frames that ask "
                            "refuse a stale slot as before",f.renderWidth,f.renderHeight);
        }
    }
    const bool depthCheck=steadyAvailable && !reset;
    if(steady && !reset) {if(depthCheck)++g_refusal.checked;else ++g_refusal.skipped;}
    Constants constants{};std::memcpy(constants.camera,f.camera,sizeof(f.camera));
    std::memcpy(constants.previous,reset?f.camera:f.previousCamera,sizeof(f.previousCamera));
    constants.size[0]=f.renderWidth;constants.size[1]=f.renderHeight;constants.size[2]=evalW;constants.size[3]=evalH;
    constants.flags[0]=reset;constants.flags[1]=engine;constants.flags[2]=taa;constants.flags[3]=f.staticScene?1u:(depthCheck?2u:0u);
    constants.route[0]=hdr?1u:0u;constants.route[1]=(hdr&&taa)?1u:0u;
    constants.route[2]=firstPersonMap?1u:0u;constants.route[3]=firstPersonMap?f.firstPersonPhaseMode:0u;
    constants.debug[0]=sampleNow?1u:0u;constants.debug[1]=paintNow?1u:0u;
    constants.debug[2]=overlay?1u:0u;
    constants.debug[3]=(foreground?2u:(untrusted?1u:0u))|(skin?4u:0u);
    constants.foregroundDepth[0]=sdkDepthScale;
    constants.jitter[0]=f.jitterX;constants.jitter[1]=f.jitterY;
    constants.jitter[2]=f.previousJitterX;constants.jitter[3]=f.previousJitterY;
    // On a reset the previous rows ARE the current rows (above), so they carry the current phase.
    constants.rowsJitter[0]=rowsNow.ndcX;constants.rowsJitter[1]=rowsNow.ndcY;
    constants.rowsJitter[2]=reset?rowsNow.ndcX:rowsBefore.ndcX;constants.rowsJitter[3]=reset?rowsNow.ndcY:rowsBefore.ndcY;
    // The crumbs' copy step covers the census's timestamps, the constants upload and the copy of H into the private input.
    HdrCrumbSpan copyStep(g_crumbOn,"copy-h","fmt=%s(%u) size=%ux%u",hdrCrumbFormat(static_cast<uint32_t>(colorDesc.Format)),
        static_cast<unsigned>(colorDesc.Format),f.renderWidth,f.renderHeight);
    SpanGuard span(context);   // the GPU census's timestamp pair: this call's own dispatches and the backend
    context->UpdateSubresource(g.constants.Get(),0,nullptr,&constants,0,0);
    context->CopyResource(g.color.texture.Get(),overlay?cleanColor.Get():color.Get());
    if(overlay) context->CopyResource(g.rawOverlay.texture.Get(),color.Get());
    if(hdr)++stats.hdrCopied;
    copyStep.close();
    HdrCrumbSpan prepStep(g_crumbOn,"prep","groups=%ux%u",(f.renderWidth+7)/8,(f.renderHeight+7)/8);
    ID3D11Buffer* cb[]={g.constants.Get(),f.engine.sceneNow,f.engine.scenePrev};
    context->CSSetConstantBuffers(0,3,cb);
    // t4..t7 are the later kernels' (motion, rejection, expected depth, history): null for prep. t8 is the later kernels' history depth
    // and the prep's too, but only on a frame whose steady-detail check runs (last frame's depth). t9 and t10 are the first-person map and
    // stencil, null unless the pair was accepted above.
    ID3D11ShaderResourceView* prepViews[]={g.color.srv.Get(),f.depth,f.engine.slots,f.engine.pool,
        nullptr,nullptr,nullptr,nullptr,depthCheck?g.depth[prevDepthIndex].srv.Get():nullptr,firstPersonMap,firstPersonStencil,
        nullptr,nullptr,f.untrustedCameraCoverage,nullptr,foreground?f.foregroundMotion:nullptr,nullptr,skin?f.engine.skin:nullptr};
    const UINT prepViewCount=skin?18:(foreground?16:14);
    context->CSSetShaderResources(0,prepViewCount,prepViews);
    // u5 is the refusal census's class texture, bound only on a frame that samples or paints (u4 is the later kernels' OutColor).
    ID3D11UnorderedAccessView* prepOutputs[]={g.depth[depthIndex].uav.Get(),g.motion.uav.Get(),g.rejection.uav.Get(),g.expected.uav.Get(),
        nullptr,needClass?g.klass.uav.Get():nullptr};
    context->CSSetUnorderedAccessViews(0,needClass?6:4,prepOutputs,nullptr);
    context->CSSetShader(g.prep.Get(),nullptr,0);
    context->Dispatch((f.renderWidth+7)/8,(f.renderHeight+7)/8,1);
    ID3D11UnorderedAccessView* nullUavs[6]={};ID3D11ShaderResourceView* nullViews[18]={};
    context->CSSetUnorderedAccessViews(0,6,nullUavs,nullptr);context->CSSetShaderResources(0,prepViewCount,nullViews);
    if(hdr)++stats.hdrPrepped;
    prepStep.close();
    if(sampleNow) {
        // The census sample: clear the counters, reduce the class texture the prep just wrote into them, copy them to the next
        // staging slot (read back by pollRefusalCensus a few frames from now). b0 (the constants) is still bound from the prep.
        const UINT groupsX=(f.renderWidth+7)/8,groupsY=(f.renderHeight+7)/8;
        const UINT zeros[4]={0,0,0,0};
        context->ClearUnorderedAccessViewUint(g.refusalCountsUav.Get(),zeros);
        ID3D11ShaderResourceView* classView=g.klass.srv.Get();context->CSSetShaderResources(11,1,&classView);
        ID3D11UnorderedAccessView* counts=g.refusalCountsUav.Get();context->CSSetUnorderedAccessViews(6,1,&counts,nullptr);
        context->CSSetShader(g.census.Get(),nullptr,0);
        context->Dispatch(groupsX,groupsY,1);
        ID3D11UnorderedAccessView* noCounts=nullptr;context->CSSetUnorderedAccessViews(6,1,&noCounts,nullptr);
        ID3D11ShaderResourceView* noClass=nullptr;context->CSSetShaderResources(11,1,&noClass);
        const uint32_t slot=g.refusalWrite;
        context->CopyResource(g.refusalStaging[slot].Get(),g.refusalCounts.Get());
        g.refusalPending[slot]=true;g.refusalWidth[slot]=f.renderWidth;g.refusalHeight[slot]=f.renderHeight;
        g.refusalWrite=(slot+1)%4;
        ++g_refusal.sampled;
    }
    HdrCrumbSpan backendStep(g_crumbOn,"backend","mode=%s in=%ux%u out=%ux%u reset=%u",flatMonoResolveModeName(f.mode),
        f.renderWidth,f.renderHeight,evalW,evalH,reset?1u:0u);
    bool ok=true;
    {
        flatcpu::Scope backendScope(flatcpu::kBackend);   // the backend evaluation: NGX, FSR3, or the TAA dispatch
    if(taa) {
        ID3D11ShaderResourceView* views[]={g.color.srv.Get(),nullptr,nullptr,nullptr,g.motion.srv.Get(),
            g.rejection.srv.Get(),g.expected.srv.Get(),g.output[index^1].srv.Get(),g.depth[index^1].srv.Get(),
            nullptr,nullptr,nullptr,nullptr,f.untrustedCameraCoverage,
            untrusted?g.outputDomain[index^1].srv.Get():nullptr};
        context->CSSetShaderResources(0,untrusted?15:9,views);
        ID3D11UnorderedAccessView* out=g.output[index].uav.Get();context->CSSetUnorderedAccessViews(4,1,&out,nullptr);
        if(untrusted) {
            ID3D11UnorderedAccessView* domain=g.outputDomain[index].uav.Get();
            context->CSSetUnorderedAccessViews(7,1,&domain,nullptr);
        }
        ID3D11SamplerState* sampler=g.sampler.Get();context->CSSetSamplers(0,1,&sampler);
        context->CSSetShader(g.taa.Get(),nullptr,0);context->Dispatch((f.outputWidth+7)/8,(f.outputHeight+7)/8,1);
    } else if(f.mode==FlatMonoResolveMode::Fsr) {
        const float sy=std::sqrt(f.camera[0][1]*f.camera[0][1]+f.camera[1][1]*f.camera[1][1]+f.camera[2][1]*f.camera[2][1]);
        ok=fsr3Evaluate(context,f.slot,g.color.texture.Get(),g.depth[depthIndex].texture.Get(),g.motion.texture.Get(),g.rejection.texture.Get(),
            g.output[0].texture.Get(),f.renderWidth,f.renderHeight,evalW,evalH,f.jitterX,f.jitterY,reset,f.deltaMs,
            sdkNear,(std::numeric_limits<float>::max)(),2*std::atan(1/sy),reason,true,hdr);
    } else {
        ok=dlaaEvaluate(context,static_cast<int>(f.slot),g.color.texture.Get(),g.depth[depthIndex].texture.Get(),g.motion.texture.Get(),g.output[0].texture.Get(),
            g.rejection.texture.Get(),f.renderWidth,f.renderHeight,evalW,evalH,f.jitterX,f.jitterY,reset,f.deltaMs,reason,hdr);
    }
    }
    backendStep.result("ok=%u reason=%s",ok?1u:0u,(reason && *reason)?*reason:"none");
    backendStep.close();
    if(hdr && ok)++stats.hdrBackend;
    if(!ok) {++stats.backendFailures;g.history=false;stats.currentContinueRun=0;return false;}
    if(hdr) {
        // The result goes back into the game's HDR target through a pixel-shader draw: per pixel the backend's output,
        // or the raw input where the rejection mask says the history is not to be trusted; for EDVR's TAA (route y) its
        // output, which has already made that choice. The state is our own, cleared at the top of the draw.
        // t0 is always the CLEAN H (g.color, the image the backend was handed). A late-overlay frame adds the overlay mask at t12 and the raw
        // H with the overlays drawn at t16: the finish keeps the world under an overlay and adds the overlay's own contribution (raw minus
        // clean), instead of showing the raw frame there.
        if(paintNow) {
            // The refusal view: the same draw with the prep's class texture at t11, which the finish paints from.
            ID3D11ShaderResourceView* paintViews[17]={g.color.srv.Get(),nullptr,nullptr,nullptr,nullptr,
                g.rejection.srv.Get(),nullptr,g.output[taa?index:0].srv.Get(),nullptr,nullptr,nullptr,g.klass.srv.Get(),
                overlay?f.overlayCoverage:nullptr,nullptr,nullptr,nullptr,overlay?g.rawOverlay.srv.Get():nullptr};
            drawHdrTarget(context,g.finishHdr.Get(),f.renderWidth,f.renderHeight,paintViews,overlay?17:13);
        } else {
            ID3D11ShaderResourceView* views[17]={g.color.srv.Get(),nullptr,nullptr,nullptr,nullptr,
                g.rejection.srv.Get(),nullptr,g.output[taa?index:0].srv.Get(),nullptr,nullptr,nullptr,nullptr,
                overlay?f.overlayCoverage:nullptr,nullptr,nullptr,nullptr,overlay?g.rawOverlay.srv.Get():nullptr};
            drawHdrTarget(context,g.finishHdr.Get(),f.renderWidth,f.renderHeight,views,overlay?17:8);
        }
    } else if(!taa) {
        // SDKs may alter every stage. Start our final composite from the isolated
        // empty state; the outer guard still owns the untouched game's state.
        context->ClearState();
        ID3D11Buffer* cb0=g.constants.Get();context->CSSetConstantBuffers(0,1,&cb0);
        // t11 is the prep's class texture, bound only for the refusal view (the finish paints from it when debug.y says so).
        ID3D11ShaderResourceView* views[12]={g.color.srv.Get(),nullptr,nullptr,nullptr,nullptr,
            g.rejection.srv.Get(),nullptr,g.output[0].srv.Get(),nullptr,nullptr,nullptr,paintNow?g.klass.srv.Get():nullptr};
        context->CSSetShaderResources(0,paintNow?12:8,views);
        ID3D11SamplerState* sampler=g.sampler.Get();context->CSSetSamplers(0,1,&sampler);
        ID3D11UnorderedAccessView* out=g.output[1].uav.Get();context->CSSetUnorderedAccessViews(4,1,&out,nullptr);
        context->CSSetShader(g.finish.Get(),nullptr,0);context->Dispatch((evalW+7)/8,(evalH+7)/8,1);
    }
    if(pixels.active()) {
        if(f.mode!=FlatMonoResolveMode::Dlss && f.mode!=FlatMonoResolveMode::Dlaa)
            pixels.unsupported("unsupported-route-requires-dlss-or-dlaa");
        else {
            ID3D11Texture2D* textures[]={g.color.texture.Get(),g.depth[depthIndex].texture.Get(),g.motion.texture.Get(),
                g.rejection.texture.Get(),g.output[0].texture.Get(),hdr?color.Get():g.output[1].texture.Get()};
            try { pixels.capture(device,context,f,reset,textures,paintNow); } catch(...) { pixels.cancel(); }
        }
    }
    g.history=true;g.lastFrame=f.frame;g.current=index^1;g.inputFormat=colorDesc.Format;
    g.untrustedCoverageLast=untrusted;
    g.sdkDepthScaleLast=sdkDepthScale;
    if(!taa)g.depthLast=depthIndex;   // the image the next asking frame reads as last frame's depth (0 while no frame asks)
    if(reset) {
        ++stats.acceptedResets;stats.currentContinueRun=0;
        if(requestedReset)++stats.requestedResets;
        if(lostHistory)++stats.lostHistory;
        if(frameGap && !lostHistory)++stats.frameGaps;
        if(invalidPreviousCamera)++stats.invalidPreviousCameras;
        if(formatChange && !lostHistory)++stats.formatChanges;
        if(cameraCut)++stats.cameraCuts;
        uint32_t& logged=cameraCut?cameraCutEventsLogged:resetEventsLogged;
        if(logged<kResetEventLogCap) {
            ++logged;
            float maxMatrixDelta=0;
            for(unsigned row=0;row<5;++row)for(unsigned col=0;col<4;++col) {
                const float delta=std::abs(f.camera[row][col]-f.previousCamera[row][col]);
                if(delta>maxMatrixDelta)maxMatrixDelta=delta;
            }
            const char* modeName=f.mode==FlatMonoResolveMode::Taa?"taa":
                f.mode==FlatMonoResolveMode::Dlaa?"dlaa":f.mode==FlatMonoResolveMode::Dlss?"dlss":"fsr";
            Log::get().note("flat resolve reset event: frame=%llu mode=%s render=%ux%u output=%ux%u "
                "requested=%u lost=%u gap=%u invalid-prev-camera=%u format=%u camera-cut=%u "
                "delta-ms=%.9g now=(%.9g,%.9g,%.9g) previous=(%.9g,%.9g,%.9g) "
                "origin-delta=(%.9g,%.9g,%.9g) max-matrix-delta=%.9g "
                "jitter-now=(%.9g,%.9g) jitter-previous=(%.9g,%.9g) event=%u/%u sdk-depth-transition=%u",
                static_cast<unsigned long long>(f.frame),modeName,f.renderWidth,f.renderHeight,f.outputWidth,f.outputHeight,
                requestedReset?1u:0u,lostHistory?1u:0u,frameGap?1u:0u,invalidPreviousCamera?1u:0u,
                formatChange?1u:0u,cameraCut?1u:0u,f.deltaMs,
                f.camera[5][0],f.camera[5][1],f.camera[5][2],
                f.previousCamera[5][0],f.previousCamera[5][1],f.previousCamera[5][2],
                f.camera[5][0]-f.previousCamera[5][0],
                f.camera[5][1]-f.previousCamera[5][1],
                f.camera[5][2]-f.previousCamera[5][2],maxMatrixDelta,
                f.jitterX,f.jitterY,f.previousJitterX,f.previousJitterY,logged,kResetEventLogCap,depthConventionTransition?1u:0u);
        }
    } else {
        ++stats.acceptedContinues;
        if(++stats.currentContinueRun>stats.longestContinueRun)stats.longestContinueRun=stats.currentContinueRun;
    }
    // The HDR route's result is already in H: nothing for the caller to bind, *output stays null.
    if(hdr) {++stats.hdrResolves;return true;}
    *output=(colorDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?g.output[taa?index:1].srgb:g.output[taa?index:1].srv).Get();
    (*output)->AddRef();
    return true;
}

bool flatMonoResolveSpatialFallback(ID3D11Device* device,ID3D11DeviceContext* context,const FlatMonoResolveFrame& f,
                                    ID3D11ShaderResourceView** output,const char** reason) {
    // The HDR route's spatial recovery writes the same crumbs as its resolve (flat_hdr_crumbs.h): capture-state, copy-h,
    // finish-bind, finish-draw, restore-state. The copy route's recovery writes none.
    CrumbScope crumbs(f.hdr);
    if(output)*output=nullptr;
    if(reason)*reason=nullptr;
    g.history=false;stats.currentContinueRun=0;
    if(!output || !device || !context || !f.renderWidth || !f.renderHeight || !f.outputWidth || !f.outputHeight ||
       f.renderWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || f.renderHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       f.outputWidth>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || f.outputHeight>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       !jitterValid(f) || (f.mode!=FlatMonoResolveMode::Taa && f.mode!=FlatMonoResolveMode::Dlaa &&
       f.mode!=FlatMonoResolveMode::Dlss && f.mode!=FlatMonoResolveMode::Fsr))
        return fail(reason,"flat-spatial-invalid-frame");
    const bool hdr=f.hdr;
    if(hdr && (f.renderWidth<f.outputWidth || f.renderHeight<f.outputHeight))return fail(reason,"flat-spatial-hdr-requires-render-at-least-output");
    if(!initialize(device,context,reason))return false;
    if(hdr && !initializeHdr(device,reason))return false;
    ComPtr<ID3D11Texture2D> color;
    if(!inputTexture(f.color,f.renderWidth,f.renderHeight,true,color,hdr))return fail(reason,"flat-spatial-input-view-mismatch");
    if(!resources(f,reason))return false;
    if(hdr && !hdrTargetView(color.Get(),reason))return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC colorDesc{};f.color->GetDesc(&colorDesc);
    Isolate isolated(g.context.Get(),g.isolated.Get(),g.capture);
    {
        HdrCrumbSpan copyStep(g_crumbOn,"copy-h","fmt=%s(%u) size=%ux%u",hdrCrumbFormat(static_cast<uint32_t>(colorDesc.Format)),
            static_cast<unsigned>(colorDesc.Format),f.renderWidth,f.renderHeight);
        context->CopyResource(g.color.texture.Get(),color.Get());
        if(hdr)++stats.hdrCopied;
    }
    if(hdr) {
        // The HDR route's recovery: the jittered H resampled on the unjittered grid and written back into H by a
        // pixel shader (the same draw the resolve ends with), no history and no SDK. *output stays null.
        Constants hc{};
        hc.size[0]=f.renderWidth;hc.size[1]=f.renderHeight;hc.size[2]=f.renderWidth;hc.size[3]=f.renderHeight;
        hc.jitter[0]=f.jitterX;hc.jitter[1]=f.jitterY;hc.route[0]=1;
        context->UpdateSubresource(g.constants.Get(),0,nullptr,&hc,0,0);
        ID3D11ShaderResourceView* views[1]={g.color.srv.Get()};
        drawHdrTarget(context,g.spatialHdr.Get(),f.renderWidth,f.renderHeight,views,1);
        ++stats.hdrSpatial;
        return true;
    }
    // The spatial recovery runs on the route's evaluation grid, exactly as
    // the resolve it substitutes for (section 72's supersample routes).
    const auto route = flatResolveRoute(f.mode, f.renderWidth, f.renderHeight, f.outputWidth, f.outputHeight);
    const uint32_t evalW = route.refused ? f.outputWidth :
        (f.evalWidth && f.evalHeight) ? f.evalWidth : route.evalWidth;
    const uint32_t evalH = route.refused ? f.outputHeight :
        (f.evalWidth && f.evalHeight) ? f.evalHeight : route.evalHeight;
    Constants constants{};
    constants.size[0]=f.renderWidth;constants.size[1]=f.renderHeight;
    constants.size[2]=evalW;constants.size[3]=evalH;
    constants.jitter[0]=f.jitterX;constants.jitter[1]=f.jitterY;
    context->UpdateSubresource(g.constants.Get(),0,nullptr,&constants,0,0);
    ID3D11Buffer* cb=g.constants.Get();context->CSSetConstantBuffers(0,1,&cb);
    ID3D11ShaderResourceView* source=g.color.srv.Get();context->CSSetShaderResources(0,1,&source);
    ID3D11SamplerState* sampler=g.sampler.Get();context->CSSetSamplers(0,1,&sampler);
    ID3D11UnorderedAccessView* target=g.output[1].uav.Get();context->CSSetUnorderedAccessViews(4,1,&target,nullptr);
    context->CSSetShader(g.spatial.Get(),nullptr,0);
    context->Dispatch((evalW+7)/8,(evalH+7)/8,1);
    *output=(colorDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?g.output[1].srgb:g.output[1].srv).Get();
    (*output)->AddRef();
    return true;
}

// Test-only, NOT part of flat_mono_resolve.h's contract (the rig forward-declares it, as tools\fsr3_engine_test does for
// fsr3_engine.cpp's own hooks): the prep kernel the NEXT initialisation makes is this bytecode instead of the shipped one, so
// tools\flat_mono_resolve_test can run its first-person scenario against a prep with one rule flipped and prove the scenario's
// assertions fail. The bytes are copied. Empty bytes (or a null pointer), the state in production and after a rig is done with
// it, mean the shipped bytecode. It takes effect when the renderer is next initialised: call flatMonoResolveReset() after it.
void flatMonoResolveTestPrepBytecode(const void* bytes,size_t size) {
    g_testPrepBytecode.clear();
    if(bytes && size)g_testPrepBytecode.assign(static_cast<const unsigned char*>(bytes),static_cast<const unsigned char*>(bytes)+size);
}
// Test-only, likewise, the same contract for the TAA kernel: tools\flat_mono_resolve_test runs its TAA history-depth scenarios against a
// taa() with one rule flipped (the old single-texel check among them) and proves they fail. Empty bytes mean the shipped bytecode; it
// takes effect when the renderer is next initialised.
void flatMonoResolveTestTaaBytecode(const void* bytes,size_t size) {
    g_testTaaBytecode.clear();
    if(bytes && size)g_testTaaBytecode.assign(static_cast<const unsigned char*>(bytes),static_cast<const unsigned char*>(bytes)+size);
}
// Test-only, likewise: the session's budget of isolation log lines (kIsolationLogCap) starts over, so a rig that has initialised the
// renderer many times can still read the line an initialisation says.
void flatMonoResolveTestResetIsolationLog() { isolationLogged=0; }
// Test-only, likewise: whether any of the refusal census's or the view's resources (the class texture, the counting pass, the counter
// buffer and its staging ring) exists, so a rig can show that a frame which asked for neither made none of them.
bool flatMonoResolveTestRefusalResources() {
    return g.klass.texture || g.census || g.refusalCounts || g.refusalCountsUav || g.refusalStaging[0] || g.refusalStaging[1] ||
           g.refusalStaging[2] || g.refusalStaging[3];
}
// Test-only, likewise: whether the resolver holds a second depth image for a backend that keeps none, which only a frame that asked for the
// steady-detail depth check makes (EDVR's TAA has its own depth[1], made with the rest by resources(); it does not count here).
bool flatMonoResolveTestSecondDepth() {
    return g.mode!=FlatMonoResolveMode::Taa && g.depth[1].texture;
}
} // namespace edvr
