#include "temporal_shader_bytecode.h"
#include "weapon_motion.h"
#include "weapon_motion_cost_sites.h"
#include "animated_vertex_history.h"
#include <cstring>
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <algorithm>
#include "gpu_interval.h"
#include "shader_swap.h"
#include "vscreen.h"
#include "../common/log.h"
namespace edvr { namespace weapon_motion_detail {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
constexpr unsigned maxVertices=AnimatedVertexHistory::maxVertices;
bool enabled=false;
struct State {
    unsigned frame=0,sourceFrame=~0u,mapFrame=~0u,width=0,height=0,invalidationNotes=0;
    bool failed=false,ambiguous=false,noted=false,declined=false;
    Ptr<ID3D11Texture2D> source,map;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11ShaderResourceView> srv;
    Ptr<ID3D11VertexShader> vs;
    Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11DepthStencilState> depth;
    Ptr<ID3D11BlendState> blend;
    Ptr<ID3D11Buffer> settings,sequential;
    AnimatedVertexHistory history;
} g;
constexpr uint64_t gpuWindowFrames=1800;
constexpr unsigned gpuDrainFrames=120;
struct GpuMetric {
    GpuIntervals<16> timer;
    uint64_t selected=0,submitted=0;
    bool begin(ID3D11DeviceContext* ctx){++selected;if(!timer.begin(ctx))return false;++submitted;return true;}
    uint64_t pending()const{const uint64_t retired=uint64_t(timer.totals.samples)+timer.totals.invalid;return submitted>retired?submitted-retired:0;}
    void clear(ID3D11DeviceContext* ctx){timer.reset(ctx);selected=submitted=0;}
};
struct GpuDiagnostics {
    enum class Phase {Idle,Collecting,Draining} phase=Phase::Idle;
    uint64_t nextScope=0,scope=0,sourceFrames=0,lastSourceFrame=0,calls=0,selected=0,budgetSkipped=0,clearCalls=0;
    unsigned width=0,height=0,drainFrames=0;
    unsigned lastSelectedFrame=~0u;
    ID3D11Texture2D* source=nullptr; // identity only; g.source owns it
    GpuMetric identify,clear,capture,raster;
    void start(ID3D11Texture2D* key,unsigned w,unsigned h,unsigned frame){phase=Phase::Collecting;scope=++nextScope;source=key;width=w;height=h;sourceFrames=calls=selected=budgetSkipped=clearCalls=0;lastSourceFrame=frame;lastSelectedFrame=~0u;drainFrames=0;}
    void close(){if(phase==Phase::Collecting)phase=Phase::Draining;}
    uint64_t pending()const{return identify.pending()+clear.pending()+capture.pending()+raster.pending();}
    void reset(ID3D11DeviceContext* ctx){identify.clear(ctx);clear.clear(ctx);capture.clear(ctx);raster.clear(ctx);phase=Phase::Idle;scope=sourceFrames=lastSourceFrame=calls=selected=budgetSkipped=clearCalls=0;width=height=drainFrames=0;lastSelectedFrame=~0u;source=nullptr;}
    void report(ID3D11DeviceContext* ctx,bool cutoff){
        const auto&i=identify.timer.totals;const auto&z=clear.timer.totals;const auto&c=capture.timer.totals;const auto&r=raster.timer.totals;const double frames=sourceFrames?double(sourceFrames):1.0;
        Log::get().note(
            "weapon motion GPU: scope %llu source %ux%u, %llu source frames, %llu eligible calls (%.2f/frame), %llu selected, %llu frame-budget skips, %llu map clears%s. Exact command intervals; rotating 1/64 draw samples, one admission/frame, and 1/16 clear samples. Budget skips can bias bursts; zero ready is unavailable, not zero cost.",
            (unsigned long long)scope,width,height,(unsigned long long)sourceFrames,(unsigned long long)calls,calls/frames,(unsigned long long)selected,(unsigned long long)budgetSkipped,
            (unsigned long long)clearCalls,cutoff?"; 120-frame drain expired, pending samples abandoned":"");
        Log::get().note(
            "weapon motion GPU setup: scope %llu; identify %.3f us/sample [%u ready/%llu selected/%llu submitted, %llu begin-fail, %u timer-skip subset, %u invalid, %llu pending]; clear %.3f us/sample [%u/%llu/%llu, %llu begin-fail, %u timer-skip subset, %u invalid, %llu pending].",
            (unsigned long long)scope,
            i.samples?i.ms*1000/i.samples:0,i.samples,(unsigned long long)identify.selected,(unsigned long long)identify.submitted,(unsigned long long)(identify.selected-identify.submitted),i.skipped,i.invalid,(unsigned long long)identify.pending(),
            z.samples?z.ms*1000/z.samples:0,z.samples,(unsigned long long)clear.selected,(unsigned long long)clear.submitted,(unsigned long long)(clear.selected-clear.submitted),z.skipped,z.invalid,(unsigned long long)clear.pending());
        Log::get().note(
            "weapon motion GPU draws: scope %llu; post-VS capture %.3f us/sample [%u ready/%llu selected/%llu submitted, %llu begin-fail, %u timer-skip subset, %u invalid, %llu pending]; raster %.3f us/sample [%u/%llu/%llu, %llu begin-fail, %u timer-skip subset, %u invalid, %llu pending].",
            (unsigned long long)scope,
            c.samples?c.ms*1000/c.samples:0,c.samples,(unsigned long long)capture.selected,(unsigned long long)capture.submitted,(unsigned long long)(capture.selected-capture.submitted),c.skipped,c.invalid,(unsigned long long)capture.pending(),
            r.samples?r.ms*1000/r.samples:0,r.samples,(unsigned long long)raster.selected,(unsigned long long)raster.submitted,(unsigned long long)(raster.selected-raster.submitted),r.skipped,r.invalid,(unsigned long long)raster.pending());
        reset(ctx);
    }
    void noteSource(ID3D11Texture2D* key,unsigned w,unsigned h,unsigned frame){
        if(phase==Phase::Collecting&&(source!=key||width!=w||height!=h))close();
        if(phase==Phase::Idle)start(key,w,h,frame);
        if(phase!=Phase::Collecting)return;
        if(lastSourceFrame!=frame){lastSourceFrame=frame;++sourceFrames;}else if(!sourceFrames)++sourceFrames;
    }
    bool choose(unsigned frame){
        if(phase!=Phase::Collecting)return false;
        const uint64_t ordinal=calls++,block=ordinal/64;
        const unsigned position=unsigned((block*0x9E3779B97F4A7C15ull+scope*0xD1B54A32D192ED03ull)&63u);
        if(unsigned(ordinal&63u)!=position)return false;++selected;
        if(lastSelectedFrame==frame){++budgetSkipped;return false;}lastSelectedFrame=frame;return true;
    }
    bool chooseClear(){
        const uint64_t ordinal=clearCalls++;
        const uint64_t block=ordinal/16;
        return unsigned(ordinal&15u)==unsigned((block*0x9E3779B97F4A7C15ull+scope*0xD1B54A32D192ED03ull)&15u);
    }
    void tick(ID3D11DeviceContext* ctx,unsigned frame){
        if(ctx){identify.timer.poll(ctx);clear.timer.poll(ctx);capture.timer.poll(ctx);raster.timer.poll(ctx);}
        if(phase==Phase::Collecting&&(sourceFrames>=gpuWindowFrames||(sourceFrames&&frame-lastSourceFrame>120)))close();
        if(phase!=Phase::Draining)return;if(!pending()){report(ctx,false);return;}if(++drainFrames>=gpuDrainFrames)report(ctx,true);
    }
} gpu;
bool family(uint64_t hash){
    return hash==0x7B0DC42D383F694Cull || hash==0x8B589D25B2A0ADDCull ||
        hash==0x114AF608F86D9ED8ull || hash==0xAACFDCF2FB9AD809ull || hash==0x174E8D76363BE337ull;
}
bool prepare(ID3D11DeviceContext* ctx,ID3D11Device* dev){
    if(g.rtv)return true;
    g.vs.Attach(shaderSwapCreateVs(ctx,kWeaponMotionVsBytecode,sizeof(kWeaponMotionVsBytecode),"weapon motion","weapon motion"));
    g.ps.Attach(shaderSwapCreatePs(ctx,kWeaponMotionPsBytecode,sizeof(kWeaponMotionPsBytecode),"weapon motion","weapon motion"));
    if(!g.vs || !g.ps || !g.history.initializeIdentity(ctx,dev,"weapon identity","weapon motion"))return false;
    D3D11_TEXTURE2D_DESC td{};td.Width=g.width;td.Height=g.height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    // Keep depth alongside motion so later, untracked opaque draws cannot
    // inherit vectors belonging to an occluded mesh. Half-depth comparison
    // at the consumer includes only its representational rounding error.
    td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    if(FAILED(dev->CreateTexture2D(&td,nullptr,&g.map)) || FAILED(dev->CreateRenderTargetView(g.map.Get(),nullptr,&g.rtv)) ||
       FAILED(dev->CreateShaderResourceView(g.map.Get(),nullptr,&g.srv)))return false;
    D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthFunc=D3D11_COMPARISON_EQUAL;ds.StencilEnable=TRUE;ds.StencilReadMask=16;
    ds.FrontFace.StencilFunc=D3D11_COMPARISON_EQUAL;ds.FrontFace.StencilFailOp=ds.FrontFace.StencilDepthFailOp=ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;ds.BackFace=ds.FrontFace;
    D3D11_BLEND_DESC bd{};bd.RenderTarget[0].RenderTargetWriteMask=15;
    if(FAILED(dev->CreateDepthStencilState(&ds,&g.depth)) || FAILED(dev->CreateBlendState(&bd,&g.blend)))return false;
    D3D11_BUFFER_DESC b{};b.ByteWidth=16;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(dev->CreateBuffer(&b,nullptr,&g.settings)))return false;
    std::vector<unsigned> indices(maxVertices);for(unsigned i=0;i<maxVertices;++i)indices[i]=i;
    b.ByteWidth=maxVertices*4;b.BindFlags=D3D11_BIND_INDEX_BUFFER;b.Usage=D3D11_USAGE_IMMUTABLE;D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=indices.data();
    return SUCCEEDED(dev->CreateBuffer(&b,&initial,&g.sequential));
}
} // namespace weapon_motion_detail
void weaponMotionRememberShader(ID3D11VertexShader* vs,uint64_t hash,const void* bytes,size_t size){
    if(vs && bytes && size && size<=65536 && weapon_motion_detail::family(hash))
        AnimatedVertexHistory::rememberShader(vs,bytes,size);
}
void weaponMotionConfigure(bool on){if(on!=weapon_motion_detail::enabled){weapon_motion_detail::gpu.close();unsigned frame=weapon_motion_detail::g.frame;weapon_motion_detail::g=weapon_motion_detail::State{};weapon_motion_detail::g.frame=frame;weapon_motion_detail::enabled=on;}}
void weaponMotionSource(ID3D11Texture2D* source){
    using namespace weapon_motion_detail;if(!enabled || !source)return;
    D3D11_TEXTURE2D_DESC td{};source->GetDesc(&td);
    if(td.Format!=DXGI_FORMAT_R32G8X24_TYPELESS || td.ArraySize!=1 || td.SampleDesc.Count!=1 || uint64_t(td.Width)*td.Height>16*1024*1024)return;
    if(g.width!=td.Width || g.height!=td.Height){unsigned frame=g.frame;g=State{};g.frame=frame;g.width=td.Width;g.height=td.Height;}
    g.source=source;g.sourceFrame=g.frame;
    gpu.noteSource(source,td.Width,td.Height,g.frame);
}
void weaponMotionDraw(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned count,unsigned instances,unsigned start,int base,unsigned startInstance){
    using namespace weapon_motion_detail;
    if(!enabled || g.failed || g.sourceFrame!=g.frame || !ctx || !draw || instances!=1 || !count || count%3 || count>maxVertices || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    Ptr<ID3D11DepthStencilState> ds;UINT ref=0;ctx->OMGetDepthStencilState(&ds,&ref);if(!ds || !(ref&16))return;
    D3D11_DEPTH_STENCIL_DESC dd{};ds->GetDesc(&dd);
    if(!dd.DepthEnable || dd.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ALL || !dd.StencilEnable || !(dd.StencilWriteMask&16) ||
       dd.FrontFace.StencilPassOp!=D3D11_STENCIL_OP_REPLACE || dd.BackFace.StencilPassOp!=D3D11_STENCIL_OP_REPLACE)return;
    Ptr<ID3D11DepthStencilView> depth;ctx->OMGetRenderTargets(0,nullptr,depth.GetAddressOf());if(!depth)return;
    Ptr<ID3D11Resource> resource;depth->GetResource(&resource);if(resource.Get()!=g.source.Get())return;
    D3D11_VIEWPORT vp{};UINT nv=1;ctx->RSGetViewports(&nv,&vp);
    if(nv!=1 || vp.TopLeftX || vp.TopLeftY || vp.Width!=g.width || vp.Height!=g.height || vp.MinDepth!=0 || vp.MaxDepth!=1)return;
    AnimatedVertexHistory::Capture vertices;
    if(!g.history.prepareCapture(ctx,count,instances,start,base,startInstance,g.frame,vertices)) {
        if(vertices.refusal && std::strcmp(vertices.refusal,"occurrence-cap")==0) {
            g.ambiguous=true;
            if(!g.declined){g.declined=true;Log::get().note("weapon motion: more than four mesh occurrences; rejecting this frame's weapon history.");}
        }
        if(g.history.failed()){g.failed=true;Log::get().note("weapon motion: stream-output creation failed; weapon history rejected.");}
        return;
    }
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
    if(!prepare(ctx,dev.Get())){g.failed=true;Log::get().note("weapon motion: resource creation failed; weapon history rejected.");return;}
    const bool selected=gpu.choose(g.frame);
    const auto& r=vertices.geometry;const unsigned candidates=vertices.candidateCount;const bool valid=candidates!=0;
    const bool identifyTimed=selected&&gpu.identify.begin(ctx);g.history.submitIdentity(ctx,vertices);if(identifyTimed)gpu.identify.timer.end(ctx);
    if(g.mapFrame!=g.frame){
        const bool clearTimed=gpu.phase==GpuDiagnostics::Phase::Collecting&&gpu.chooseClear()&&gpu.clear.begin(ctx);
        float zero[4]{};ctx->ClearRenderTargetView(g.rtv.Get(),zero);if(clearTimed)gpu.clear.timer.end(ctx);g.mapFrame=g.frame;
    }
    g.history.bindPositions(ctx,vertices);
    const bool captureTimed=selected&&gpu.capture.begin(ctx);
    g.history.drawPositions(ctx,draw,startInstance,vertices);
    if(captureTimed)gpu.capture.timer.end(ctx);
    g.history.restorePositions(ctx,vertices);
    ID3D11RenderTargetView* rt[8]{};ctx->OMGetRenderTargets(8,rt,nullptr);
    Ptr<ID3D11BlendState> blend;FLOAT factors[4];UINT mask;ctx->OMGetBlendState(&blend,factors,&mask);
    Ptr<ID3D11PixelShader> ps;ID3D11ClassInstance* pc[256]{},*vc[256]{};UINT np=256,nc=256;
    ctx->PSGetShader(&ps,pc,&np);Ptr<ID3D11VertexShader> vs;ctx->VSGetShader(&vs,vc,&nc);
    Ptr<ID3D11Buffer> cb,vsCb;ctx->PSGetConstantBuffers(0,1,&cb);ctx->VSGetConstantBuffers(0,1,&vsCb);ID3D11ShaderResourceView* srvs[10]{};ctx->VSGetShaderResources(0,10,srvs);
    float settings[4]={float(g.width),float(g.height),float(candidates),0};ctx->UpdateSubresource(g.settings.Get(),0,nullptr,settings,0,0);
    ID3D11ShaderResourceView* in[10]={vertices.currentPositions.Get(),vertices.previousPositions[0].Get(),vertices.previousPositions[1].Get(),vertices.previousPositions[2].Get(),vertices.previousPositions[3].Get(),vertices.currentIdentity.Get(),vertices.previousIdentity[0].Get(),vertices.previousIdentity[1].Get(),vertices.previousIdentity[2].Get(),vertices.previousIdentity[3].Get()};
    vScreenSetRenderTargetsRaw(ctx,1,g.rtv.GetAddressOf(),depth.Get());ctx->OMSetDepthStencilState(g.depth.Get(),16);ctx->OMSetBlendState(g.blend.Get(),nullptr,~0u);
    ctx->VSSetShader(g.vs.Get(),nullptr,0);ctx->VSSetShaderResources(0,10,in);ctx->VSSetConstantBuffers(0,1,g.settings.GetAddressOf());ctx->PSSetShader(g.ps.Get(),nullptr,0);ctx->PSSetConstantBuffers(0,1,g.settings.GetAddressOf());
    const bool rasterTimed=selected&&gpu.raster.begin(ctx);
    ctx->IASetInputLayout(nullptr);ctx->IASetIndexBuffer(g.sequential.Get(),DXGI_FORMAT_R32_UINT,0);draw(ctx,count,1,0,0,0);
    if(rasterTimed)gpu.raster.timer.end(ctx);
    ID3D11ShaderResourceView* none[10]{};ctx->VSSetShaderResources(0,10,none);
    ctx->IASetInputLayout(r.layout.Get());ctx->IASetIndexBuffer(r.indices.Get(),r.format,r.indexOffset);
    ctx->VSSetShader(vs.Get(),vc,nc);ctx->VSSetShaderResources(0,10,srvs);ctx->VSSetConstantBuffers(0,1,vsCb.GetAddressOf());ctx->PSSetShader(ps.Get(),pc,np);ctx->PSSetConstantBuffers(0,1,cb.GetAddressOf());
    vScreenSetRenderTargetsRaw(ctx,8,rt,depth.Get());ctx->OMSetDepthStencilState(ds.Get(),ref);ctx->OMSetBlendState(blend.Get(),factors,mask);
    for(auto* p:rt)if(p)p->Release();for(auto* p:srvs)if(p)p->Release();for(UINT i=0;i<np;++i)pc[i]->Release();for(UINT i=0;i<nc;++i)vc[i]->Release();
    if(valid && !g.noted){g.noted=true;Log::get().note("weapon motion: original animated vertices supply skeleton/projection-matched source motion at %ux%u; aiming, skinning and projection included. GPU-only, bounded 32 MiB vertex history.",g.width,g.height);}
}
void weaponMotionDrawSampledApi(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned count,unsigned instances,unsigned start,int base,unsigned startInstance){
    using namespace weapon_motion_detail;
    if(!enabled || g.failed || g.sourceFrame!=g.frame || !ctx || !draw || instances!=1 || !count || count%3 || count>maxVertices || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    Ptr<ID3D11DepthStencilState> ds;UINT ref=0;
    ctx->OMGetDepthStencilState(&ds,&ref);if(!ds || !(ref&16))return;
    D3D11_DEPTH_STENCIL_DESC dd{};ds->GetDesc(&dd);
    if(!dd.DepthEnable || dd.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ALL || !dd.StencilEnable || !(dd.StencilWriteMask&16) ||
       dd.FrontFace.StencilPassOp!=D3D11_STENCIL_OP_REPLACE || dd.BackFace.StencilPassOp!=D3D11_STENCIL_OP_REPLACE)return;
    Ptr<ID3D11DepthStencilView> depth;
    ctx->OMGetRenderTargets(0,nullptr,depth.GetAddressOf());if(!depth)return;
    Ptr<ID3D11Resource> resource;depth->GetResource(&resource);if(resource.Get()!=g.source.Get())return;
    D3D11_VIEWPORT vp{};UINT nv=1;ctx->RSGetViewports(&nv,&vp);
    if(nv!=1 || vp.TopLeftX || vp.TopLeftY || vp.Width!=g.width || vp.Height!=g.height || vp.MinDepth!=0 || vp.MaxDepth!=1)return;
    AnimatedVertexHistory::Capture vertices;
    if(!g.history.prepareCapture(ctx,count,instances,start,base,startInstance,g.frame,vertices)) {
        if(vertices.refusal && std::strcmp(vertices.refusal,"occurrence-cap")==0) {
            g.ambiguous=true;
            if(!g.declined){g.declined=true;Log::get().note("weapon motion: more than four mesh occurrences; rejecting this frame's weapon history.");}
        }
        if(g.history.failed()){g.failed=true;Log::get().note("weapon motion: stream-output creation failed; weapon history rejected.");}
        return;
    }
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
    if(!prepare(ctx,dev.Get())){g.failed=true;Log::get().note("weapon motion: resource creation failed; weapon history rejected.");return;}
    const bool selected=gpu.choose(g.frame);
    const auto& r=vertices.geometry;const unsigned candidates=vertices.candidateCount;const bool valid=candidates!=0;
    const bool identifyTimed=selected&&gpu.identify.begin(ctx);g.history.submitIdentity(ctx,vertices);if(identifyTimed)gpu.identify.timer.end(ctx);
    if(g.mapFrame!=g.frame){
        const bool clearTimed=gpu.phase==GpuDiagnostics::Phase::Collecting&&gpu.chooseClear()&&gpu.clear.begin(ctx);
        float zero[4]{};ctx->ClearRenderTargetView(g.rtv.Get(),zero);if(clearTimed)gpu.clear.timer.end(ctx);g.mapFrame=g.frame;
    }
    g.history.bindPositions(ctx,vertices);
    const bool captureTimed=selected&&gpu.capture.begin(ctx);
    g.history.drawPositions(ctx,draw,startInstance,vertices);
    if(captureTimed)gpu.capture.timer.end(ctx);
    g.history.restorePositions(ctx,vertices);
    ID3D11RenderTargetView* rt[8]{};
    weapon_motion_cost::note<weapon_motion_cost::Site::TargetRead,plugin_cost::ApiClass::ReadQuery>();
    ctx->OMGetRenderTargets(8,rt,nullptr);
    Ptr<ID3D11BlendState> blend;FLOAT factors[4];UINT mask;
    weapon_motion_cost::note<weapon_motion_cost::Site::BlendRead,plugin_cost::ApiClass::ReadQuery>();
    ctx->OMGetBlendState(&blend,factors,&mask);
    Ptr<ID3D11PixelShader> ps;ID3D11ClassInstance* pc[256]{},*vc[256]{};UINT np=256,nc=256;
    weapon_motion_cost::note<weapon_motion_cost::Site::PixelShaderRead,plugin_cost::ApiClass::ReadQuery>();
    ctx->PSGetShader(&ps,pc,&np);
    Ptr<ID3D11VertexShader> vs;
    weapon_motion_cost::note<weapon_motion_cost::Site::VertexShaderRead,plugin_cost::ApiClass::ReadQuery>();
    ctx->VSGetShader(&vs,vc,&nc);
    Ptr<ID3D11Buffer> cb,vsCb;
    weapon_motion_cost::note<weapon_motion_cost::Site::PixelConstantBufferRead,plugin_cost::ApiClass::ReadQuery>();
    ctx->PSGetConstantBuffers(0,1,&cb);
    weapon_motion_cost::note<weapon_motion_cost::Site::VertexConstantBufferRead,plugin_cost::ApiClass::ReadQuery>();
    ctx->VSGetConstantBuffers(0,1,&vsCb);
    ID3D11ShaderResourceView* srvs[10]{};
    weapon_motion_cost::note<weapon_motion_cost::Site::VertexResourcesRead,plugin_cost::ApiClass::ReadQuery>();
    ctx->VSGetShaderResources(0,10,srvs);
    float settings[4]={float(g.width),float(g.height),float(candidates),0};
    weapon_motion_cost::note<weapon_motion_cost::Site::SettingsWrite,plugin_cost::ApiClass::Transfer>();
    ctx->UpdateSubresource(g.settings.Get(),0,nullptr,settings,0,0);
    ID3D11ShaderResourceView* in[10]={vertices.currentPositions.Get(),vertices.previousPositions[0].Get(),vertices.previousPositions[1].Get(),vertices.previousPositions[2].Get(),vertices.previousPositions[3].Get(),vertices.currentIdentity.Get(),vertices.previousIdentity[0].Get(),vertices.previousIdentity[1].Get(),vertices.previousIdentity[2].Get(),vertices.previousIdentity[3].Get()};
    weapon_motion_cost::note<weapon_motion_cost::Site::MotionTargetBind,plugin_cost::ApiClass::State>();
    vScreenSetRenderTargetsRaw(ctx,1,g.rtv.GetAddressOf(),depth.Get());
    weapon_motion_cost::note<weapon_motion_cost::Site::DepthStateBind,plugin_cost::ApiClass::State>();
    ctx->OMSetDepthStencilState(g.depth.Get(),16);
    weapon_motion_cost::note<weapon_motion_cost::Site::BlendStateBind,plugin_cost::ApiClass::State>();
    ctx->OMSetBlendState(g.blend.Get(),nullptr,~0u);
    weapon_motion_cost::note<weapon_motion_cost::Site::MotionVertexShaderBind,plugin_cost::ApiClass::State>();
    ctx->VSSetShader(g.vs.Get(),nullptr,0);
    weapon_motion_cost::note<weapon_motion_cost::Site::MotionVertexResourcesBind,plugin_cost::ApiClass::State>();
    ctx->VSSetShaderResources(0,10,in);
    weapon_motion_cost::note<weapon_motion_cost::Site::MotionVertexConstantBufferBind,plugin_cost::ApiClass::State>();
    ctx->VSSetConstantBuffers(0,1,g.settings.GetAddressOf());
    weapon_motion_cost::note<weapon_motion_cost::Site::MotionPixelShaderBind,plugin_cost::ApiClass::State>();
    ctx->PSSetShader(g.ps.Get(),nullptr,0);
    weapon_motion_cost::note<weapon_motion_cost::Site::MotionPixelConstantBufferBind,plugin_cost::ApiClass::State>();
    ctx->PSSetConstantBuffers(0,1,g.settings.GetAddressOf());
    const bool rasterTimed=selected&&gpu.raster.begin(ctx);
    weapon_motion_cost::note<weapon_motion_cost::Site::SequentialLayoutBind,plugin_cost::ApiClass::State>();
    ctx->IASetInputLayout(nullptr);
    weapon_motion_cost::note<weapon_motion_cost::Site::SequentialIndexBufferBind,plugin_cost::ApiClass::State>();
    ctx->IASetIndexBuffer(g.sequential.Get(),DXGI_FORMAT_R32_UINT,0);
    weapon_motion_cost::note<weapon_motion_cost::Site::RasterDraw,plugin_cost::ApiClass::Work>();
    draw(ctx,count,1,0,0,0);
    if(rasterTimed)gpu.raster.timer.end(ctx);
    ID3D11ShaderResourceView* none[10]{};
    weapon_motion_cost::note<weapon_motion_cost::Site::VertexResourcesClear,plugin_cost::ApiClass::State>();
    ctx->VSSetShaderResources(0,10,none);
    weapon_motion_cost::note<weapon_motion_cost::Site::HostLayoutRestore,plugin_cost::ApiClass::State>();
    ctx->IASetInputLayout(r.layout.Get());
    weapon_motion_cost::note<weapon_motion_cost::Site::HostIndexBufferRestore,plugin_cost::ApiClass::State>();
    ctx->IASetIndexBuffer(r.indices.Get(),r.format,r.indexOffset);
    weapon_motion_cost::note<weapon_motion_cost::Site::HostVertexShaderRestore,plugin_cost::ApiClass::State>();
    ctx->VSSetShader(vs.Get(),vc,nc);
    weapon_motion_cost::note<weapon_motion_cost::Site::HostVertexResourcesRestore,plugin_cost::ApiClass::State>();
    ctx->VSSetShaderResources(0,10,srvs);
    weapon_motion_cost::note<weapon_motion_cost::Site::HostVertexConstantBufferRestore,plugin_cost::ApiClass::State>();
    ctx->VSSetConstantBuffers(0,1,vsCb.GetAddressOf());
    weapon_motion_cost::note<weapon_motion_cost::Site::HostPixelShaderRestore,plugin_cost::ApiClass::State>();
    ctx->PSSetShader(ps.Get(),pc,np);
    weapon_motion_cost::note<weapon_motion_cost::Site::HostPixelConstantBufferRestore,plugin_cost::ApiClass::State>();
    ctx->PSSetConstantBuffers(0,1,cb.GetAddressOf());
    weapon_motion_cost::note<weapon_motion_cost::Site::HostTargetRestore,plugin_cost::ApiClass::State>();
    vScreenSetRenderTargetsRaw(ctx,8,rt,depth.Get());
    weapon_motion_cost::note<weapon_motion_cost::Site::HostDepthStateRestore,plugin_cost::ApiClass::State>();
    ctx->OMSetDepthStencilState(ds.Get(),ref);
    weapon_motion_cost::note<weapon_motion_cost::Site::HostBlendStateRestore,plugin_cost::ApiClass::State>();
    ctx->OMSetBlendState(blend.Get(),factors,mask);
    for(auto* p:rt)if(p)p->Release();for(auto* p:srvs)if(p)p->Release();for(UINT i=0;i<np;++i)pc[i]->Release();for(UINT i=0;i<nc;++i)vc[i]->Release();
    if(valid && !g.noted){g.noted=true;Log::get().note("weapon motion: original animated vertices supply skeleton/projection-matched source motion at %ux%u; aiming, skinning and projection included. GPU-only, bounded 32 MiB vertex history.",g.width,g.height);}
}
bool weaponMotionFamilyVs(uint64_t vsHash){return weapon_motion_detail::family(vsHash);}
bool weaponMotionWants(uint64_t vsHash){
    using namespace weapon_motion_detail;
    return enabled && !g.failed && g.sourceFrame==g.frame && family(vsHash);
}
ID3D11ShaderResourceView* weaponMotionView(){using namespace weapon_motion_detail;return enabled && !g.failed && !g.ambiguous && g.mapFrame==g.frame?g.srv.Get():nullptr;}
void weaponMotionFrameBoundary(ID3D11DeviceContext* ctx){
    using namespace weapon_motion_detail;gpu.tick(ctx,g.frame);++g.frame;g.ambiguous=false;
    g.history.advance(g.frame);
    if(g.source && g.frame-g.sourceFrame>120){gpu.close();unsigned frame=g.frame;g=State{};g.frame=frame;}
}
WeaponMotionGpuDiagnostics weaponMotionGpuDiagnostics(){using namespace weapon_motion_detail;WeaponMotionGpuDiagnostics s{};s.scope=gpu.scope;s.sourceFrames=gpu.sourceFrames;s.calls=gpu.calls;s.selected=gpu.selected;s.submitted=gpu.identify.submitted;s.identifyReady=gpu.identify.timer.totals.samples;s.captureReady=gpu.capture.timer.totals.samples;s.rasterReady=gpu.raster.timer.totals.samples;s.identifySkipped=gpu.identify.timer.totals.skipped;s.captureSkipped=gpu.capture.timer.totals.skipped;s.rasterSkipped=gpu.raster.timer.totals.skipped;s.identifyInvalid=gpu.identify.timer.totals.invalid;s.captureInvalid=gpu.capture.timer.totals.invalid;s.rasterInvalid=gpu.raster.timer.totals.invalid;s.collecting=gpu.phase==GpuDiagnostics::Phase::Collecting;s.draining=gpu.phase==GpuDiagnostics::Phase::Draining;return s;}
void weaponMotionShutdown(){weapon_motion_detail::gpu.reset(nullptr);weapon_motion_detail::g=weapon_motion_detail::State{};}
void weaponMotionResourceWritten(ID3D11Resource* resource){
    using namespace weapon_motion_detail;if(!enabled)return;
    const unsigned reasons=g.history.resourceWritten(resource);
    for(unsigned reason:{1u,2u,4u})if((reasons&reason) && !(g.invalidationNotes&reason)) {
        g.invalidationNotes|=reason;
        Log::get().note("weapon motion: history invalidated by %s; correspondence will restart on the next captured draw.",
            reason==1?"unknown command-list resource writes":reason==2?"mesh vertex-buffer write":"mesh index-buffer write");
    }
    if(reasons)g.mapFrame=~0u;
}
}
