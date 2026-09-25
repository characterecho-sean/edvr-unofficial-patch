#include "screen_motion.h"
#include "weapon_motion.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "binding_shadow.h"
#include "engine_velocity.h"
#include "gpu_census.h"
#include "gpu_interval.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"   // kEngineMotionCoreHlsl (tools/temporal_shader_build)
#include "ui_layer.h"
#include "vscreen.h"
#include "../common/config.h"
#include "../common/temporal_mode.h"
#include "../common/log.h"

namespace edvr {

// The two flags screenMotionLive reads without a call (screen_motion.h).
// OUT of State deliberately: State is reset wholesale in three places, and
// each of them must now say what the arming becomes. That is the point --
// a reset that silently cleared ailed used to be invisible.
namespace detail {
bool g_screenMotionEnabled = false;
bool g_screenMotionFailed = false;
}  // namespace detail
 namespace {
template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
struct Screen {
    Ptr<ID3D11Resource> target;
    Ptr<ID3D11Texture2D> map;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11ShaderResourceView> srv,sizeSrv[2];
    Ptr<ID3D11Buffer> model[2],camera[2],sizes[2],settings;
    float shape[4]{};
    unsigned width=0,height=0,frame=~0u,write=0;
    bool written=false;
};
struct State {
    bool noted=false;
    bool seen=false;
    bool weapon=true,weaponNoted=false;
    unsigned lastScreen=0;
    unsigned frame=0,sourceFrame=~0u,sourceWrite=0,sourcePrevious=~0u;
    Ptr<ID3D11Buffer> camera[2];
    Ptr<ID3D11Texture2D> depth;
    Ptr<ID3D11ShaderResourceView> depthSrv,stencilSrv;
    Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11BlendState> blend;
    Ptr<ID3D11DepthStencilState> ds;
    Ptr<ID3D11Texture2D> ui;
    Ptr<ID3D11Resource> uiTarget;
    Ptr<ID3D11RenderTargetView> uiRtv;
    Ptr<ID3D11ShaderResourceView> uiSrv;
    Ptr<ID3D11BlendState> uiBlend;
    Ptr<ID3D11DepthStencilState> uiDs;
    unsigned uiFrame=~0u,uiDraws=0;
    bool uiNoted=false;
    Screen eyes[2];
    // Engine-record motion on foot: the screen shader's per-kind eye-pixel
    // counts (u1, cleared at the frame's first counted draw), copied out at
    // the frame boundary and read a few frames later without waiting.
    // Without diagnostics the counts are sampled (engine_velocity.h): one
    // frame in kPanelSampleFrames, one eye pixel in kPanelSampleStride^2;
    // the stride travels with each ring slot.
    Ptr<ID3D11Buffer> counts,countsStaging[4];
    Ptr<ID3D11UnorderedAccessView> countsUav;
    unsigned countsDraws[4]{},countsStride[4]{},countsWrite=0,countFrame=~0u,countDraws=0,countStride=1;
    bool countsPending[4]{},engineNoted=false;
    // The naming without terrain (flight 6, docs/kinematic-motion-injection-
    // 2026-09-19.md "The hangar"): a hangar has no terrain or scene draw, so
    // nothing named the source there. The pool family draws (not a first-person
    // weapon or tool shader) into each depth of the screen's size are counted
    // per frame; last frame's busiest names the source at its first such draw
    // this frame, while no terrain or scene draw has named it for
    // kTerrainHoldFrames. The per-frame table resolves each depth view once.
    struct DepthCount { void* dsv=nullptr; bool screenSized=false; unsigned draws=0; };
    DepthCount depthCounts[8]; unsigned depthCountN=0;
    void* screenDepth=nullptr;           // last frame's busiest screen-sized pool depth, identity only
    unsigned screenDepthDraws=0;         // its pool family draws last frame
    unsigned terrainFrame=~0u;           // the last frame a terrain or scene draw named the source
    unsigned unnamed=0,unnamedPoolDraws=0;   // screen frames with no naming, and their screen-sized pool draws
    bool unnamedNoted=false,screenDepthNoted=false;
} g;
constexpr unsigned kTerrainHoldFrames=2;     // a terrain naming this recent keeps the fallback off
constexpr unsigned kUnnamedNoteFrames=90;    // screen frames with no naming before the log says so
// What the screen shader does with the source's engine data beyond using it
// (screenMotionConfigure): count its kinds (advanced.temporal_aa_diagnostics or
// the motion_source view) and hand them to the view (motion_source). Outside
// State: State resets must not forget a setting until the next config poll.
bool g_countKinds=false,g_paintKinds=false;
constexpr unsigned kPanelKinds=5;   // joined, masked, not a rig record, stale, corrupt

constexpr uint64_t kGpuWindowFrames=1800;
constexpr unsigned kGpuDrainFrames=120;
struct GpuMetric {
    GpuIntervals<16> timer;
    uint64_t calls=0,selected=0,submitted=0,budgetSkipped=0;
    unsigned lastSelectedFrame=~0u;
    bool begin(ID3D11DeviceContext* ctx,unsigned stride,uint64_t scope,unsigned frame) {
        const uint64_t ordinal=calls++;
        const uint64_t block=ordinal/stride;
        // Exactly one rotating position in every power-of-two block. This
        // bounds leases and avoids always measuring the first UI/eye draw.
        const unsigned position=unsigned((block*0x9E3779B97F4A7C15ull+scope*0xD1B54A32D192ED03ull) & (stride-1));
        if(unsigned(ordinal&(stride-1))!=position)return false;
        ++selected;
        // A burst of heterogeneous GUI draws can cross several 64-call
        // blocks in one frame. Never let that consume the shared clock's
        // bounded lease supply; the omission is separate from timer skips.
        if(lastSelectedFrame==frame){++budgetSkipped;return false;}
        lastSelectedFrame=frame;
        if(!timer.begin(ctx))return false;
        ++submitted;return true;
    }
    uint64_t pending() const {
        const uint64_t retired=uint64_t(timer.totals.samples)+timer.totals.invalid;
        return submitted>retired?submitted-retired:0;
    }
    void clear(ID3D11DeviceContext* ctx) {
        timer.reset(ctx);calls=selected=submitted=budgetSkipped=0;lastSelectedFrame=~0u;
    }
};
struct GpuDiagnostics {
    enum class Phase { Idle,Collecting,Draining } phase=Phase::Idle;
    uint64_t nextScope=0,scope=0,sourceFrames=0,lastSourceFrame=0;
    unsigned width=0,height=0,drainFrames=0;
    ID3D11Resource* source=nullptr; // identity only; g.depth owns the resource
    bool policyNoted=false;
    GpuMetric uiClear,uiDraw,eyeClear,projection;
    void start(ID3D11Resource* key,unsigned w,unsigned h,unsigned frame) {
        phase=Phase::Collecting;scope=++nextScope;source=key;width=w;height=h;
        sourceFrames=0;lastSourceFrame=frame;drainFrames=0;
    }
    void close(){if(phase==Phase::Collecting)phase=Phase::Draining;}
    uint64_t pending() const{return uiClear.pending()+uiDraw.pending()+eyeClear.pending()+projection.pending();}
    void reset(ID3D11DeviceContext* ctx) {
        uiClear.clear(ctx);uiDraw.clear(ctx);eyeClear.clear(ctx);projection.clear(ctx);
        phase=Phase::Idle;scope=sourceFrames=lastSourceFrame=0;width=height=drainFrames=0;
        source=nullptr;
    }
    void report(ID3D11DeviceContext* ctx,bool cutoff) {
        const auto& c=uiClear.timer.totals;const auto& u=uiDraw.timer.totals;const auto& e=eyeClear.timer.totals;const auto& p=projection.timer.totals;
        const double frames=sourceFrames?double(sourceFrames):1.0;
        Log::get().note(
            "screen motion GPU: scope %llu source %ux%u, %llu source frames%s. Exact added-command intervals; rotating 1/16 clear/projection and 1/64 UI samples, one admission/category/frame. Budget skips can bias bursty calls; zero ready is unavailable, not zero cost.",
            (unsigned long long)scope,width,height,(unsigned long long)sourceFrames,
            cutoff?"; 120-frame drain expired, pending samples abandoned":"");
        Log::get().note(
            "screen motion GPU UI: scope %llu; clears %llu calls (%.2f/frame), %.3f us/sample [%u ready/%llu selected/%llu submitted, %llu begin-fail, %u timer-skip subset, %llu budget-skip, %u invalid, %llu pending]; reissues %llu calls (%.2f/frame), %.3f us/sample [%u/%llu/%llu, %llu begin-fail, %u timer-skip subset, %llu budget-skip, %u invalid, %llu pending].",
            (unsigned long long)scope,
            (unsigned long long)uiClear.calls,uiClear.calls/frames,c.samples?c.ms*1000/c.samples:0,c.samples,(unsigned long long)uiClear.selected,(unsigned long long)uiClear.submitted,(unsigned long long)(uiClear.selected-uiClear.budgetSkipped-uiClear.submitted),c.skipped,(unsigned long long)uiClear.budgetSkipped,c.invalid,(unsigned long long)uiClear.pending(),
            (unsigned long long)uiDraw.calls,uiDraw.calls/frames,u.samples?u.ms*1000/u.samples:0,u.samples,(unsigned long long)uiDraw.selected,(unsigned long long)uiDraw.submitted,(unsigned long long)(uiDraw.selected-uiDraw.budgetSkipped-uiDraw.submitted),u.skipped,(unsigned long long)uiDraw.budgetSkipped,u.invalid,(unsigned long long)uiDraw.pending());
        Log::get().note(
            "screen motion GPU eye: scope %llu; clears %llu calls (%.2f/frame), %.3f us/sample [%u ready/%llu selected/%llu submitted, %llu begin-fail, %u timer-skip subset, %llu budget-skip, %u invalid, %llu pending]; projection draws %llu calls (%.2f/frame), %.3f us/sample [%u/%llu/%llu, %llu begin-fail, %u timer-skip subset, %llu budget-skip, %u invalid, %llu pending].",
            (unsigned long long)scope,
            (unsigned long long)eyeClear.calls,eyeClear.calls/frames,e.samples?e.ms*1000/e.samples:0,e.samples,(unsigned long long)eyeClear.selected,(unsigned long long)eyeClear.submitted,(unsigned long long)(eyeClear.selected-eyeClear.budgetSkipped-eyeClear.submitted),e.skipped,(unsigned long long)eyeClear.budgetSkipped,e.invalid,(unsigned long long)eyeClear.pending(),
            (unsigned long long)projection.calls,projection.calls/frames,p.samples?p.ms*1000/p.samples:0,p.samples,(unsigned long long)projection.selected,(unsigned long long)projection.submitted,(unsigned long long)(projection.selected-projection.budgetSkipped-projection.submitted),p.skipped,(unsigned long long)projection.budgetSkipped,p.invalid,(unsigned long long)projection.pending());
        reset(ctx);
    }
    void noteSource(ID3D11Resource* key,unsigned w,unsigned h,unsigned frame) {
        if(phase==Phase::Collecting && (source!=key || width!=w || height!=h))close();
        if(phase==Phase::Idle)start(key,w,h,frame);
        if(phase!=Phase::Collecting)return;
        if(lastSourceFrame!=frame){lastSourceFrame=frame;++sourceFrames;}
        else if(!sourceFrames)++sourceFrames;
    }
    void tick(ID3D11DeviceContext* ctx,unsigned frame) {
        if(ctx){uiClear.timer.poll(ctx);uiDraw.timer.poll(ctx);eyeClear.timer.poll(ctx);projection.timer.poll(ctx);}
        if(phase==Phase::Collecting && (sourceFrames>=kGpuWindowFrames || (sourceFrames && frame-lastSourceFrame>120)))close();
        if(phase!=Phase::Draining)return;
        const uint64_t left=pending();
        if(!left){report(ctx,false);return;}
        if(++drainFrames>=kGpuDrainFrames)report(ctx,true);
    }
} g_gpu;
bool copyCb(ID3D11DeviceContext* ctx,ID3D11Device* dev,unsigned slot,Ptr<ID3D11Buffer>& out,unsigned minimum) {
    Ptr<ID3D11Buffer> in;ctx->VSGetConstantBuffers(slot,1,&in);if(!in)return false;
    D3D11_BUFFER_DESC bd{},prior{};in->GetDesc(&bd);if(out)out->GetDesc(&prior);
    if(bd.ByteWidth<minimum || bd.ByteWidth>65536)return false;
    if(prior.ByteWidth!=bd.ByteWidth) {
        out.Reset();bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags=bd.MiscFlags=bd.StructureByteStride=0;
        if(FAILED(dev->CreateBuffer(&bd,nullptr,&out)))return false;
    }
    ctx->CopyResource(out.Get(),in.Get());return true;
}
bool prepareCounts(ID3D11Device* dev) {
    if(g.counts)return true;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=kPanelKinds*4;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=4;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=DXGI_FORMAT_UNKNOWN;ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=kPanelKinds;
    D3D11_BUFFER_DESC sd=bd;sd.Usage=D3D11_USAGE_STAGING;sd.BindFlags=0;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    bool ok=SUCCEEDED(dev->CreateBuffer(&bd,nullptr,&g.counts)) && SUCCEEDED(dev->CreateUnorderedAccessView(g.counts.Get(),&ud,&g.countsUav));
    for(auto& s:g.countsStaging)ok=ok && SUCCEEDED(dev->CreateBuffer(&sd,nullptr,&s));
    if(!ok){g.counts.Reset();g.countsUav.Reset();for(auto& s:g.countsStaging)s.Reset();}
    return ok;
}
bool prepare(ID3D11DeviceContext* ctx,ID3D11Device* dev,Screen& e,unsigned w,unsigned h) {
    if(!g.ps) {
        const std::string source=screenMotionPsSource(kEngineMotionCoreHlsl);   // the compose's own arithmetic in front
        g.ps.Attach(shaderSwapCompilePs(ctx,source.c_str(),source.size(),"main","screen motion",nullptr,"screen motion"));
        D3D11_BLEND_DESC b{};b.RenderTarget[0].RenderTargetWriteMask=15;
        D3D11_DEPTH_STENCIL_DESC d{};d.DepthFunc=D3D11_COMPARISON_ALWAYS;
        if(!g.ps || FAILED(dev->CreateBlendState(&b,&g.blend)) || FAILED(dev->CreateDepthStencilState(&d,&g.ds)))return false;
    }
    if(e.width!=w || e.height!=h) {
        e=Screen{};D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;
        td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(dev->CreateTexture2D(&td,nullptr,&e.map)) || FAILED(dev->CreateRenderTargetView(e.map.Get(),nullptr,&e.rtv)) ||
           FAILED(dev->CreateShaderResourceView(e.map.Get(),nullptr,&e.srv)))return false;
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=48;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;   // shape, extent, engine
        if(FAILED(dev->CreateBuffer(&bd,nullptr,&e.settings)))return false;
        bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;sd.Format=DXGI_FORMAT_R32_TYPELESS;
        sd.BufferEx.NumElements=4;sd.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        for(int i=0;i<2;++i)if(FAILED(dev->CreateBuffer(&bd,nullptr,&e.sizes[i])) ||
            FAILED(dev->CreateShaderResourceView(e.sizes[i].Get(),&sd,&e.sizeSrv[i])))return false;
        e.width=w;e.height=h;
    }
    return true;
}
}
void screenMotionConfigure(Config& cfg) {
    bool on=temporalModeEnabled(cfg.getString("fix.temporal_aa","off"));
    if(on!=detail::g_screenMotionEnabled){g_gpu.close();g=State{};detail::g_screenMotionEnabled=on;detail::g_screenMotionFailed=false;}
    if(on&&!g_gpu.policyNoted){g_gpu.policyNoted=true;Log::get().note("on-foot motion GPU diagnostics: enabled for screen UI clears/reissues, per-eye screen projection, and weapon identify/capture/raster; exact sparse query intervals only, one admission per category per frame, 1800 source-frame windows and a 120-frame no-wait drain.");}
    const bool weapon=cfg.getBool("fix.weapon_stability",true);
    if(weapon!=g.weapon)g_gpu.close();
    g.weapon=weapon;
    weaponMotionConfigure(detail::g_screenMotionEnabled && g.weapon);
    const std::string debug=cfg.getString("advanced.temporal_aa_debug","off");
    g_paintKinds=_stricmp(debug.c_str(),"motion_source")==0;
    g_countKinds=g_paintKinds || cfg.getBool("advanced.temporal_aa_diagnostics",false);
}
bool screenMotionRecognize() {
    bool matched=detail::g_screenMotionEnabled && !detail::g_screenMotionFailed && bindingShaderHash(BindSlot::Vs)==0x5C36AF051B98B9F1ull &&
        bindingShaderHash(BindSlot::Ps)==0xCFE84157BC76E921ull;
    if(matched){g.seen=true;g.lastScreen=g.frame;}return matched;
}
// A pool family draw into a depth of the screen's size, counted for this
// frame's census (the naming without terrain); true when that depth is last
// frame's busiest. Each depth view is resolved once a frame.
static bool screenPoolDraw(void* dsv,unsigned w,unsigned h) {
    State::DepthCount* c=nullptr;
    for(unsigned i=0;i<g.depthCountN;++i)if(g.depthCounts[i].dsv==dsv){c=&g.depthCounts[i];break;}
    if(!c) {
        if(g.depthCountN>=sizeof(g.depthCounts)/sizeof(g.depthCounts[0]))return false;
        c=&g.depthCounts[g.depthCountN++];
        c->dsv=dsv;c->draws=0;
        ResourceInfo d;c->screenSized=bindingResolve(dsv,&d) && d.isTexture2D && d.a==w && d.b==h;
    }
    if(!c->screenSized)return false;
    ++c->draws;
    return dsv==g.screenDepth;
}
void screenMotionSource(ID3D11DeviceContext* ctx,unsigned w,unsigned h) {
    if(!detail::g_screenMotionEnabled || detail::g_screenMotionFailed || !g.seen || g.frame-g.lastScreen>2 || !ctx || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    const uint64_t vs=bindingShaderHash(BindSlot::Vs);
    // Terrain or a scene draw names the source (the world camera by
    // construction; the settlements, flight 5). Without either -- a hangar --
    // the first pool family draw (not a first-person weapon or tool shader)
    // into last frame's busiest screen-sized pool depth does, once no terrain
    // or scene draw has named it for kTerrainHoldFrames.
    const bool terrain=vs==0xACE405F428C17EF6ull || vs==0x4435F2E50020E7F3ull;
    if(!terrain) {
        if(!engineVelocityPoolFamilyVs(vs) || weaponMotionFamilyVs(vs))return;
        void* dsvView=bindingGet(BindSlot::Dsv0);
        if(!dsvView || !screenPoolDraw(dsvView,w,h))return;
        if(g.sourceFrame==g.frame || (g.terrainFrame!=~0u && g.frame-g.terrainFrame<=kTerrainHoldFrames))return;
    } else if(g.sourceFrame==g.frame)return;
    ResourceInfo colour;
    if(!bindingResolve(bindingGet(BindSlot::Rtv0),&colour) || !colour.isTexture2D || colour.a!=w || colour.b!=h)return;
    Ptr<ID3D11DepthStencilView> dsv;ctx->OMGetRenderTargets(0,nullptr,&dsv);if(!dsv)return;
    Ptr<ID3D11Resource> res;dsv->GetResource(&res);Ptr<ID3D11Texture2D> tex;if(FAILED(res.As(&tex)))return;
    D3D11_TEXTURE2D_DESC td{};tex->GetDesc(&td);D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dsv->GetDesc(&dd);
    if(td.Width!=w || td.Height!=h || td.ArraySize!=1 || td.SampleDesc.Count!=1 || !(td.BindFlags&D3D11_BIND_SHADER_RESOURCE) ||
       dd.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D)return;
    DXGI_FORMAT fmt=dd.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT?DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        dd.Format==DXGI_FORMAT_D32_FLOAT?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_UNKNOWN;
    if(fmt==DXGI_FORMAT_UNKNOWN)return;
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
    if(g.depth.Get()!=tex.Get()) {
        g.depthSrv.Reset();g.stencilSrv.Reset();D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=fmt;
        sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
        if(FAILED(dev->CreateShaderResourceView(tex.Get(),&sd,&g.depthSrv)))return;
        if(fmt==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS){
            sd.Format=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
            // Optional view of the existing texture, with no new surface.
            dev->CreateShaderResourceView(tex.Get(),&sd,&g.stencilSrv);
        }
        g.depth=tex;g.sourceFrame=~0u;
    }
    unsigned next=1-g.sourceWrite;
    bool copiedSource=false;
    {
        // The census (issue #38): where the copy actually happens, not the
        // call into screenMotionSource, most of which returns before this.
        GpuCensusScope census(ctx,GpuCensusSection::FrameScreenMotion);
        copiedSource=copyCb(ctx,dev.Get(),1,g.camera[next],276*16);
    }
    if(!copiedSource)return;
    g.sourcePrevious=g.sourceFrame;g.sourceFrame=g.frame;g.sourceWrite=next;
    if(terrain)g.terrainFrame=g.frame;
    else if(!g.screenDepthNoted){g.screenDepthNoted=true;Log::get().note("screen motion: no terrain or scene draw names the on-foot source here (a hangar): it is named by its own depth -- the %ux%u depth that took the most pool family draws last frame (%u), at its first pool family draw this frame that is not a first-person weapon or tool shader; that draw's camera is the source camera.",w,h,g.screenDepthDraws);}
    g_gpu.noteSource(tex.Get(),td.Width,td.Height,g.frame);
    weaponMotionSource(tex.Get());
    // The source pass's pool draws take MRT6 into this depth's slot target,
    // held to the camera this draw reads (the one g.camera just copied).
    Ptr<ID3D11Buffer> scene;ctx->VSGetConstantBuffers(1,1,&scene);
    engineVelocityNoteSource(tex.Get(),scene.Get(),terrain?EngineVelocitySourceSignal::Terrain:EngineVelocitySourceSignal::ScreenDepth);
}
void screenMotionUiDraw(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned count,unsigned instances,
                        unsigned start,int base,unsigned startInstance) {
    // A GUI composite the UI layer took (ui_layer.h) is not in the pass's
    // input and its bound target is the layer: no source-UI mask from it.
    if(uiLayerRedirecting())return;
    if(!detail::g_screenMotionEnabled || detail::g_screenMotionFailed || g.sourceFrame!=g.frame || !draw || !ctx ||
       ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    const uint64_t vs=bindingShaderHash(BindSlot::Vs),ps=bindingShaderHash(BindSlot::Ps);
    // Flight 14:40:54, final LDR draws 932..938. These are GUI composites,
    // not the earlier HDR cockpit/world effects or the GUI atlas builders.
    if(!((vs==0xB10B032BDFD46700ull && ps==0xDB899F4BD577F2E5ull) ||
         (vs==0xC4B4B334B26E81A9ull && ps==0x0146ABCC53240479ull) ||
         (vs==0xA888D51024D9798Eull && ps==0x015EF9349EC097E8ull)))return;
    Ptr<ID3D11RenderTargetView> rt;ctx->OMGetRenderTargets(1,&rt,nullptr);if(!rt)return;
    Ptr<ID3D11Resource> res;rt->GetResource(&res);Ptr<ID3D11Texture2D> tex;if(FAILED(res.As(&tex)))return;
    D3D11_TEXTURE2D_DESC td{},depth{};tex->GetDesc(&td);g.depth->GetDesc(&depth);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};rt->GetDesc(&rd);
    if(td.Width!=depth.Width || td.Height!=depth.Height || td.SampleDesc.Count!=1 || td.ArraySize!=1 ||
       rd.Format!=DXGI_FORMAT_R8G8B8A8_UNORM || rd.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || rd.Texture2D.MipSlice)return;
    Ptr<ID3D11BlendState> blend;FLOAT factors[4];UINT mask;ctx->OMGetBlendState(&blend,factors,&mask);if(!blend)return;
    D3D11_BLEND_DESC bd{};blend->GetDesc(&bd);const auto& b=bd.RenderTarget[0];
    if(bd.AlphaToCoverageEnable || !b.BlendEnable || b.BlendOp!=D3D11_BLEND_OP_ADD ||
       (b.SrcBlend!=D3D11_BLEND_SRC_ALPHA && b.SrcBlend!=D3D11_BLEND_ONE) || b.DestBlend!=D3D11_BLEND_INV_SRC_ALPHA || !(b.RenderTargetWriteMask&7))return;
    Ptr<ID3D11DepthStencilState> ds;UINT ref;ctx->OMGetDepthStencilState(&ds,&ref);if(!ds)return;
    D3D11_DEPTH_STENCIL_DESC dd{};ds->GetDesc(&dd);
    if(dd.DepthEnable || (dd.StencilEnable && (dd.FrontFace.StencilFunc!=D3D11_COMPARISON_ALWAYS || dd.BackFace.StencilFunc!=D3D11_COMPARISON_ALWAYS)))return;
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);D3D11_TEXTURE2D_DESC old{};if(g.ui)g.ui->GetDesc(&old);
    if(old.Width!=td.Width || old.Height!=td.Height) {
        g.ui.Reset();g.uiRtv.Reset();g.uiSrv.Reset();g.uiFrame=~0u;
        td.Format=DXGI_FORMAT_R8_UNORM;td.MipLevels=1;td.Usage=D3D11_USAGE_DEFAULT;
        td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;td.CPUAccessFlags=td.MiscFlags=0;
        if(FAILED(dev->CreateTexture2D(&td,nullptr,&g.ui)) || FAILED(dev->CreateRenderTargetView(g.ui.Get(),nullptr,&g.uiRtv)) ||
           FAILED(dev->CreateShaderResourceView(g.ui.Get(),nullptr,&g.uiSrv))) {g.ui.Reset();return;}
    }
    if(!g.uiBlend) {
        // Original PS, UVs, discard and alpha, including premultiplied UI.
        // Clearing to one and multiplying by (1-alpha) unions coverage in
        // one R8 target without changing or even knowing the GUI shader.
        D3D11_BLEND_DESC ub{};auto& r=ub.RenderTarget[0];r.BlendEnable=TRUE;
        r.SrcBlend=D3D11_BLEND_ZERO;r.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;r.BlendOp=D3D11_BLEND_OP_ADD;
        r.SrcBlendAlpha=D3D11_BLEND_ZERO;r.DestBlendAlpha=D3D11_BLEND_ONE;r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=1;
        D3D11_DEPTH_STENCIL_DESC ud{};ud.DepthFunc=D3D11_COMPARISON_ALWAYS;
        if(FAILED(dev->CreateBlendState(&ub,&g.uiBlend)) || FAILED(dev->CreateDepthStencilState(&ud,&g.uiDs))) {g.uiBlend.Reset();return;}
    }
    if(g.uiFrame!=g.frame) {
        const bool timed=g_gpu.phase==GpuDiagnostics::Phase::Collecting&&g_gpu.uiClear.begin(ctx,16,g_gpu.scope,g.frame);
        const float one[4]={1,1,1,1};
        {
            // The census (issue #38): this clear is once per frame, not once
            // per call here -- most calls return above, at g.sourceFrame!=g.frame
            // or the shader/target/blend/stencil checks below.
            GpuCensusScope census(ctx,GpuCensusSection::FrameScreenMotion);
            ctx->ClearRenderTargetView(g.uiRtv.Get(),one);
        }
        if(timed)g_gpu.uiClear.timer.end(ctx);
        g.uiTarget=res;g.uiFrame=g.frame;g.uiDraws=0;
    }
    if(g.uiTarget.Get()!=res.Get())return;
    ID3D11RenderTargetView* saved[8]{};Ptr<ID3D11DepthStencilView> savedDepth;ctx->OMGetRenderTargets(8,saved,&savedDepth);
    vScreenSetRenderTargetsRaw(ctx,1,g.uiRtv.GetAddressOf(),nullptr);ctx->OMSetBlendState(g.uiBlend.Get(),nullptr,mask);ctx->OMSetDepthStencilState(g.uiDs.Get(),0);
    const bool timed=g_gpu.phase==GpuDiagnostics::Phase::Collecting&&g_gpu.uiDraw.begin(ctx,64,g_gpu.scope,g.frame);
    {
        GpuCensusScope census(ctx,GpuCensusSection::FrameScreenMotion);
        draw(ctx,count,instances,start,base,startInstance);
    }
    if(timed)g_gpu.uiDraw.timer.end(ctx);
    ++g.uiDraws;
    vScreenSetRenderTargetsRaw(ctx,8,saved,savedDepth.Get());ctx->OMSetBlendState(blend.Get(),factors,mask);ctx->OMSetDepthStencilState(ds.Get(),ref);
    for(auto* p:saved)if(p)p->Release();
    if(!g.uiNoted){g.uiNoted=true;Log::get().note("screen UI: original late GUI draws supply alpha coverage at %ux%u; R8 mask, no source colour copies. ScreenMotion.w=3 identifies current UI.",td.Width,td.Height);}
}
void screenMotionDraw(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned count,unsigned instances,
                      unsigned start,int base,unsigned startInstance,const float* curve) {
    // fix.ui_quality's layer took this composite (ui_layer.h): the screen is
    // drawn after the upscale, not into the pass's input, so the pass needs
    // no motion for it -- and the bound target is the layer, not the eye.
    if(uiLayerRedirecting())return;
    if(!screenMotionRecognize() || g.sourceFrame!=g.frame || instances!=1 || !draw || !ctx)return;
    Ptr<ID3D11RenderTargetView> target;ctx->OMGetRenderTargets(1,&target,nullptr);if(!target)return;
    Ptr<ID3D11Resource> res;target->GetResource(&res);Ptr<ID3D11Texture2D> texture;if(FAILED(res.As(&texture)))return;
    D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
    D3D11_VIEWPORT vp{};UINT nv=1;ctx->RSGetViewports(&nv,&vp);
    if(nv!=1 || vp.TopLeftX!=0 || vp.TopLeftY!=0 || vp.Width!=td.Width || vp.Height!=td.Height || td.SampleDesc.Count!=1 || td.ArraySize!=1)return;
    Ptr<ID3D11ShaderResourceView> colour;ctx->PSGetShaderResources(0,1,&colour);if(!colour)return;
    Ptr<ID3D11Resource> cr;colour->GetResource(&cr);Ptr<ID3D11Texture2D> ct;if(FAILED(cr.As(&ct)))return;
    D3D11_TEXTURE2D_DESC cd{},dd{};ct->GetDesc(&cd);g.depth->GetDesc(&dd);
    if(cd.Width!=dd.Width || cd.Height!=dd.Height)return;
    unsigned eye=0;
    if(g.eyes[0].frame==g.frame){if(g.eyes[0].target.Get()==res.Get())return;eye=1;}
    Screen& e=g.eyes[eye];if(e.frame==g.frame)return;
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
    if(!prepare(ctx,dev.Get(),e,td.Width,td.Height)){detail::g_screenMotionFailed=true;Log::get().note("screen motion: resource creation failed; original temporal inputs retained.");return;}
    unsigned next=1-e.write;
    bool copiedEye=false;
    {
        // The census (issue #38): both copies as one span, as engine_velocity.cpp's
        // matching pool+scene snapshot does -- they always run together.
        GpuCensusScope census(ctx,GpuCensusSection::FrameScreenMotion);
        copiedEye=copyCb(ctx,dev.Get(),0,e.model[next],12*16) && copyCb(ctx,dev.Get(),1,e.camera[next],274*16);
    }
    if(!copiedEye)return;
    Ptr<ID3D11Buffer> vb;UINT stride=0,offset=0;ctx->IAGetVertexBuffers(1,1,&vb,&stride,&offset);if(!vb || stride<8)return;
    D3D11_BUFFER_DESC vd{};vb->GetDesc(&vd);uint64_t at=uint64_t(offset)+uint64_t(startInstance)*stride;if(at+8>vd.ByteWidth)return;
    D3D11_BOX box{UINT(at),0,0,UINT(at+8),1,1};
    {
        GpuCensusScope census(ctx,GpuCensusSection::FrameScreenMotion);
        ctx->CopySubresourceRegion(e.sizes[next].Get(),0,0,0,0,vb.Get(),0,&box);
    }
    bool consecutive=e.frame+1==g.frame && g.sourcePrevious+1==g.frame && e.model[e.write] && e.camera[e.write] && g.camera[1-g.sourceWrite];
    const bool clearTimed=g_gpu.phase==GpuDiagnostics::Phase::Collecting&&g_gpu.eyeClear.begin(ctx,16,g_gpu.scope,g.frame);
    const float zero[4]{};
    {
        GpuCensusScope census(ctx,GpuCensusSection::FrameScreenMotion);
        ctx->ClearRenderTargetView(e.rtv.Get(),zero);
    }
    if(clearTimed)g_gpu.eyeClear.timer.end(ctx);
    e.written=false;
    if(consecutive) {
        bool ui=g.uiFrame==g.frame && g.uiDraws && g.uiTarget.Get()==cr.Get();
        bool weapon=g.weapon && g.stencilSrv;
        // The source pass's engine data (engine_velocity.h): its MRT6 slot
        // target, pool snapshot and scene constants this frame and last, or
        // nothing -- the shader then keeps today's camera term everywhere.
        EngineVelocityViews ev{};
        const bool engine=engineVelocitySourceViews(g.depth.Get(),&ev);
        // The kinds counted: every eye pixel of every frame with diagnostics
        // or motion_source; otherwise a sample -- one frame in
        // kPanelSampleFrames, one eye pixel in kPanelSampleStride^2 (engine.z
        // carries the grid's stride) -- so the on-foot line always has them.
        const unsigned countGrid=g_countKinds?1u:kPanelSampleStride;
        const bool counting=engine && (g_countKinds || g.frame%kPanelSampleFrames==0) &&
                            (g.countFrame!=g.frame || g.countStride==countGrid) && prepareCounts(dev.Get());
        float data[12]={e.shape[0],e.shape[1],e.shape[2],e.shape[3],float(e.width),float(e.height),ui?1.0f:0.0f,weapon?1.0f:0.0f,
                        engine?1.0f:0.0f,engine && g_paintKinds?1.0f:0.0f,counting?float(countGrid):0.0f,0.0f};
        {
            GpuCensusScope census(ctx,GpuCensusSection::FrameScreenMotion);
            ctx->UpdateSubresource(e.settings.Get(),0,nullptr,data,0,0);
        }
        ID3D11RenderTargetView* savedRt[8]{};Ptr<ID3D11DepthStencilView> savedDepth;ctx->OMGetRenderTargets(8,savedRt,&savedDepth);
        Ptr<ID3D11BlendState> savedBlend;FLOAT factors[4];UINT mask;ctx->OMGetBlendState(&savedBlend,factors,&mask);
        Ptr<ID3D11DepthStencilState> savedDs;UINT stencil;ctx->OMGetDepthStencilState(&savedDs,&stencil);
        Ptr<ID3D11PixelShader> savedPs;ID3D11ClassInstance* classes[256]{};UINT nc=256;ctx->PSGetShader(&savedPs,classes,&nc);
        ID3D11Buffer* savedCb[7]{};ctx->PSGetConstantBuffers(2,7,savedCb);
        ID3D11ShaderResourceView* savedSrv[7]{};ctx->PSGetShaderResources(8,7,savedSrv);
        ID3D11Buffer* cb[7]={g.camera[g.sourceWrite].Get(),g.camera[1-g.sourceWrite].Get(),e.model[e.write].Get(),e.camera[e.write].Get(),e.settings.Get(),
                             engine?ev.sceneNow:nullptr,engine?ev.scenePrev:nullptr};
        ID3D11ShaderResourceView* srvs[7]={g.depthSrv.Get(),e.sizeSrv[e.write].Get(),ui?g.uiSrv.Get():nullptr,weapon?g.stencilSrv.Get():nullptr,weapon?weaponMotionView():nullptr,
                                           engine?ev.slots:nullptr,engine?ev.pool:nullptr};
        vScreenSetRenderTargetsRaw(ctx,1,e.rtv.GetAddressOf(),nullptr);ctx->OMSetBlendState(g.blend.Get(),nullptr,~0u);ctx->OMSetDepthStencilState(g.ds.Get(),0);
        ctx->PSSetConstantBuffers(2,7,cb);ctx->PSSetShaderResources(8,7,srvs);ctx->PSSetShader(g.ps.Get(),nullptr,0);
        // The counts' UAV at u1, beside the one render target. Bound and
        // unbound with the render targets kept (0xFFFFFFFF): the binding
        // hook leaves the shadow alone for exactly that call.
        constexpr UINT kKeepTargets=0xFFFFFFFFu;
        if(counting) {
            if(g.countFrame!=g.frame){
                const UINT zeros[4]{};
                {
                    // The census (issue #38): once per frame, the first counted
                    // draw only -- not every draw into the shared UAV.
                    GpuCensusScope census(ctx,GpuCensusSection::FrameScreenMotion);
                    ctx->ClearUnorderedAccessViewUint(g.countsUav.Get(),zeros);
                }
                g.countFrame=g.frame;g.countDraws=0;g.countStride=countGrid;
            }
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(kKeepTargets,nullptr,nullptr,1,1,g.countsUav.GetAddressOf(),nullptr);
        }
        const bool projectionTimed=g_gpu.phase==GpuDiagnostics::Phase::Collecting&&g_gpu.projection.begin(ctx,16,g_gpu.scope,g.frame);
        {
            GpuCensusScope census(ctx,GpuCensusSection::FrameScreenMotion);
            draw(ctx,count,instances,start,base,startInstance);
        }
        if(projectionTimed)g_gpu.projection.timer.end(ctx);
        if(counting) {
            ID3D11UnorderedAccessView* none=nullptr;
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(kKeepTargets,nullptr,nullptr,1,1,&none,nullptr);
            ++g.countDraws;
        }
        ID3D11ShaderResourceView* nulls[7]{};ctx->PSSetShaderResources(8,7,nulls);
        vScreenSetRenderTargetsRaw(ctx,8,savedRt,savedDepth.Get());ctx->OMSetBlendState(savedBlend.Get(),factors,mask);ctx->OMSetDepthStencilState(savedDs.Get(),stencil);
        ctx->PSSetConstantBuffers(2,7,savedCb);ctx->PSSetShaderResources(8,7,savedSrv);ctx->PSSetShader(savedPs.Get(),classes,nc);
        for(auto* p:savedRt)if(p)p->Release();for(auto* p:savedCb)if(p)p->Release();for(auto* p:savedSrv)if(p)p->Release();for(UINT i=0;i<nc;++i)classes[i]->Release();
        if(ev.slots)ev.slots->Release();if(ev.pool)ev.pool->Release();if(ev.sceneNow)ev.sceneNow->Release();if(ev.scenePrev)ev.scenePrev->Release();
        if(engine && !g.engineNoted){g.engineNoted=true;Log::get().note("screen motion: the source pass's engine data is bound: certified rig records carry their own engine motion to their previous source UV before the panel mapping; masked ones keep no history%s.",g_countKinds?", counted per eye pixel":", counted on a sample (one frame in 300, one eye pixel in 16)");}
        e.written=true;
        if(weapon && !g.weaponNoted){g.weaponNoted=true;Log::get().note("screen motion: first-person stencil selects original-vertex weapon motion; uncovered or invalid history rejected.");}
        if(!g.noted){g.noted=true;Log::get().note("screen motion: source camera/depth projected through the actual screen mesh at %ux%u per eye; GPU-only history, no source colour copies.",e.width,e.height);}
    }
    for(int i=0;i<4;++i)e.shape[i]=curve?curve[i]:0;
    e.frame=g.frame;e.write=next;e.target=res;
}
ID3D11ShaderResourceView* screenMotionView(int eye,unsigned w,unsigned h) {
    if(!detail::g_screenMotionEnabled || eye<0 || eye>1)return nullptr;Screen& e=g.eyes[eye];
    return e.written && e.frame==g.frame && e.width==w && e.height==h?e.srv.Get():nullptr;
}
// The frame's panel counts: copied out once the frame's eyes are drawn, read
// back without waiting a few frames later, oldest first, handed to the engine
// motion summary (per counted eye draw). A ring slot still waiting drops the
// newest sample rather than stall.
static void flushPanelCounts(ID3D11DeviceContext* ctx) {
    if(!ctx || !g.counts)return;
    if(g.countFrame==g.frame && g.countDraws) {
        const unsigned w=g.countsWrite;
        if(!g.countsPending[w]) {
            {
                // The census (issue #38): the frame-boundary readback copy,
                // at most once a frame.
                GpuCensusScope census(ctx,GpuCensusSection::FrameScreenMotion);
                ctx->CopyResource(g.countsStaging[w].Get(),g.counts.Get());
            }
            g.countsDraws[w]=g.countDraws;g.countsStride[w]=g.countStride;g.countsPending[w]=true;g.countsWrite=(w+1)%4;
        }
    }
    for(unsigned k=0;k<4;++k) {
        const unsigned i=(g.countsWrite+k)%4;
        if(!g.countsPending[i])continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if(ctx->Map(g.countsStaging[i].Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m)!=S_OK)break;
        uint32_t c[kPanelKinds]{};std::memcpy(c,m.pData,sizeof(c));ctx->Unmap(g.countsStaging[i].Get(),0);
        engineVelocityNotePanelPixels(c[0],c[1],c[2],c[3],c[4],g.countsDraws[i],g.countsStride[i]);
        g.countsPending[i]=false;
    }
}
void screenMotionFrameBoundary(ID3D11DeviceContext* ctx){
    weaponMotionFrameBoundary(ctx);
    g_gpu.tick(ctx,g.frame);
    flushPanelCounts(ctx);
    // The naming without terrain: this frame's busiest screen-sized pool depth
    // is the next frame's candidate. A frame that showed the 2D screen with no
    // source named is counted, and said once an episode (kUnnamedNoteFrames).
    {
        const State::DepthCount* best=nullptr;
        unsigned poolDraws=0;
        for(unsigned i=0;i<g.depthCountN;++i) {
            const State::DepthCount& c=g.depthCounts[i];
            if(!c.screenSized)continue;
            poolDraws+=c.draws;
            if(!best || c.draws>best->draws)best=&c;
        }
        g.screenDepth=best && best->draws?best->dsv:nullptr;
        g.screenDepthDraws=best?best->draws:0;
        g.depthCountN=0;
        if(g.seen && g.lastScreen==g.frame && g.sourceFrame!=g.frame) {
            ++g.unnamed;
            g.unnamedPoolDraws+=poolDraws;
            if(g.unnamed==kUnnamedNoteFrames && !g.unnamedNoted) {
                g.unnamedNoted=true;
                char why[224];
                if(g.unnamedPoolDraws)_snprintf_s(why,_TRUNCATE,"%.1f pool family draws a frame went to a depth of the screen's size without naming it (its colour target or depth format refused, or a new depth every frame)",double(g.unnamedPoolDraws)/g.unnamed);
                else _snprintf_s(why,_TRUNCATE,"no pool family draw went to a depth of the screen's size");
                Log::get().note("screen motion: the 2D screen showed for %u frames and nothing named its source -- no terrain or scene draw, and %s -- so no screen motion map is made and the engine's on-foot path stands idle.",kUnnamedNoteFrames,why);
            }
        } else if(g.sourceFrame==g.frame) {
            if(g.unnamedNoted)Log::get().note("screen motion: the source is named again after %u screen frames with nothing naming it.",g.unnamed);
            g.unnamed=g.unnamedPoolDraws=0;g.unnamedNoted=false;
        }
    }
    ++g.frame;
    if(g.seen && g.frame-g.lastScreen>120){g_gpu.close();bool weapon=g.weapon;g=State{};g.weapon=weapon;detail::g_screenMotionFailed=false;}
}
ScreenMotionGpuDiagnostics screenMotionGpuDiagnostics(){
    ScreenMotionGpuDiagnostics s{};s.scope=g_gpu.scope;s.sourceFrames=g_gpu.sourceFrames;
    s.uiClearCalls=g_gpu.uiClear.calls;s.uiDrawCalls=g_gpu.uiDraw.calls;s.eyeClearCalls=g_gpu.eyeClear.calls;s.projectionCalls=g_gpu.projection.calls;
    s.uiClearSelected=g_gpu.uiClear.selected;s.uiDrawSelected=g_gpu.uiDraw.selected;s.eyeClearSelected=g_gpu.eyeClear.selected;s.projectionSelected=g_gpu.projection.selected;
    s.uiClearSubmitted=g_gpu.uiClear.submitted;s.uiDrawSubmitted=g_gpu.uiDraw.submitted;s.eyeClearSubmitted=g_gpu.eyeClear.submitted;s.projectionSubmitted=g_gpu.projection.submitted;
    s.uiClearReady=g_gpu.uiClear.timer.totals.samples;s.uiDrawReady=g_gpu.uiDraw.timer.totals.samples;s.eyeClearReady=g_gpu.eyeClear.timer.totals.samples;s.projectionReady=g_gpu.projection.timer.totals.samples;
    s.uiClearSkipped=g_gpu.uiClear.timer.totals.skipped;s.uiDrawSkipped=g_gpu.uiDraw.timer.totals.skipped;s.eyeClearSkipped=g_gpu.eyeClear.timer.totals.skipped;s.projectionSkipped=g_gpu.projection.timer.totals.skipped;
    s.uiClearInvalid=g_gpu.uiClear.timer.totals.invalid;s.uiDrawInvalid=g_gpu.uiDraw.timer.totals.invalid;s.eyeClearInvalid=g_gpu.eyeClear.timer.totals.invalid;s.projectionInvalid=g_gpu.projection.timer.totals.invalid;
    s.collecting=g_gpu.phase==GpuDiagnostics::Phase::Collecting;s.draining=g_gpu.phase==GpuDiagnostics::Phase::Draining;return s;
}
void screenMotionShutdown(){g_gpu.reset(nullptr);g=State{};detail::g_screenMotionEnabled=false;detail::g_screenMotionFailed=false;weaponMotionShutdown();}
}
