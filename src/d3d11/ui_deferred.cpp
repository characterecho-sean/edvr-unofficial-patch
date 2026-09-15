#include "ui_deferred.h"
#include "ui_deferred_draw.h"
#include "ui_deferred_depth.h"
#include "ui_deferred_shaders.h"
#include "dxbc_fanout.h"
#include "binding_shadow.h"
#include "eye_tonemap_snapshot.h"
#include "shader_swap.h"
#include "vscreen.h"
#include "../common/config.h"
#include "../common/log.h"
#include <d3d11_1.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace edvr { namespace {
template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
thread_local bool inside=false;
struct Scope { bool before=inside; Scope(){inside=true;} ~Scope(){inside=before;} };
bool enabled=false;
bool failed=false,resetPending[2]{};
Ptr<ID3D11DeviceContext> recorder;
struct Surface {
    Ptr<ID3D11Texture2D> tex;
    Ptr<ID3D11ShaderResourceView> srv;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11UnorderedAccessView> uav;
    bool ensure(ID3D11Device* dev,UINT w,UINT h,DXGI_FORMAT format,bool compute=false) {
        D3D11_TEXTURE2D_DESC d{};if(tex)tex->GetDesc(&d);
        if(tex && srv && (compute?bool(uav):bool(rtv)) && d.Width==w && d.Height==h && d.Format==format && bool(uav)==compute)return true;
        *this={};d={};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=format;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|(compute?D3D11_BIND_UNORDERED_ACCESS:D3D11_BIND_RENDER_TARGET);
        return SUCCEEDED(dev->CreateTexture2D(&d,nullptr,&tex)) &&
            SUCCEEDED(dev->CreateShaderResourceView(tex.Get(),nullptr,&srv)) &&
            (compute?SUCCEEDED(dev->CreateUnorderedAccessView(tex.Get(),nullptr,&uav)):
                     SUCCEEDED(dev->CreateRenderTargetView(tex.Get(),nullptr,&rtv)));
    }
};
struct DepthCopy {
    Ptr<ID3D11Texture2D> before,source;
    Ptr<ID3D11ShaderResourceView> depth,stencil;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    bool seed(ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11DepthStencilView* view) {
        if(!view)return false;
        Ptr<ID3D11Resource> r;view->GetResource(&r);Ptr<ID3D11Texture2D> t;if(FAILED(r.As(&t)))return false;
        D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);D3D11_DEPTH_STENCIL_VIEW_DESC vd{};view->GetDesc(&vd);
        if(d.MipLevels!=1 || d.ArraySize!=1 || d.SampleDesc.Count!=1 || vd.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice)return false;
        DXGI_FORMAT df=DXGI_FORMAT_UNKNOWN,sf=DXGI_FORMAT_UNKNOWN,tf=DXGI_FORMAT_UNKNOWN;
        switch(vd.Format) {
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:tf=DXGI_FORMAT_R32G8X24_TYPELESS;df=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;sf=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:tf=DXGI_FORMAT_R24G8_TYPELESS;df=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;sf=DXGI_FORMAT_X24_TYPELESS_G8_UINT;break;
        case DXGI_FORMAT_D32_FLOAT:tf=DXGI_FORMAT_R32_TYPELESS;df=DXGI_FORMAT_R32_FLOAT;break;
        default:return false;
        }
        if(source.Get()!=t.Get() || !before || !depth || (sf!=DXGI_FORMAT_UNKNOWN && !stencil)) {
            *this={};source=t;format=vd.Format;
            d.Format=tf;d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=0;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_DEPTH_STENCIL;
            vd.Flags=0;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=df;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
            if(FAILED(dev->CreateTexture2D(&d,nullptr,&before)) || FAILED(dev->CreateShaderResourceView(before.Get(),&sd,&depth)))return false;
            if(sf!=DXGI_FORMAT_UNKNOWN){sd.Format=sf;if(FAILED(dev->CreateShaderResourceView(before.Get(),&sd,&stencil)))return false;}
        }
        ctx->CopyResource(before.Get(),t.Get());return true;
    }
};
struct Material {
    Ptr<ID3D11PixelShader> original,fanout;
    Ptr<ID3D11BlendState> originalBlend,transmission,world;
};
std::vector<Material> materials;
UiDeferredSnapshots snapshots;
struct ShaderMask {
    Ptr<ID3D11VertexShader> vs;
    Ptr<ID3D11PixelShader> ps;
    UiDeferredCaptureMask mask;
    bool valid=false;
};
std::vector<ShaderMask> masks;
const UiDeferredCaptureMask* reflect(ID3D11DeviceContext* ctx) {
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> ps;ctx->VSGetShader(&vs,nullptr,nullptr);ctx->PSGetShader(&ps,nullptr,nullptr);
    if(!vs || !ps)return nullptr;
    for(auto& m:masks)if(m.vs==vs && m.ps==ps)return m.valid?&m.mask:nullptr;
    if(masks.size()>=128)return nullptr;
    ShaderMask m;m.vs=vs;m.ps=ps;
    m.valid=uiDeferredReflect(vs.Get(),m.mask.cbVs,m.mask.srvVs) && uiDeferredReflect(ps.Get(),m.mask.cbPs,m.mask.srvPs);
    masks.push_back(std::move(m));return masks.back().valid?&masks.back().mask:nullptr;
}
struct Draw {
    UiDeferredDraw packet;
    size_t material=0;
};
struct Eye {
    Ptr<ID3D11Texture2D> hdr;
    Ptr<ID3D11RenderTargetView> hdrTarget;
    DepthCopy depth;
    Surface cleanHdr,cleanLdr;
    UiDeferredDraw tone;
    std::vector<std::unique_ptr<Draw>> draws;
    std::vector<Ptr<ID3D11Resource>> aliases;
    Ptr<ID3D11CommandList> output;
    UINT count=0,w=0,h=0,changedStencil=0;
    size_t bytes=0;
    bool complete=false,restored=false,aborted=false;
};
Eye eyes[2];
Ptr<ID3D11BlendState> savedBlend;
Ptr<ID3D11PixelShader> savedPs;
Ptr<ID3D11RenderTargetView> savedTarget;
Ptr<ID3D11DepthStencilView> savedDepth;
float savedFactors[4]{};UINT savedMask=0;bool colourMuted=false;
uint64_t captured=0,applied=0,declined=0;
bool noted=false,routeNoted=false;
bool sameIdentity(IUnknown* a,IUnknown* b) {
    if(a==b)return true;if(!a || !b)return false;
    Ptr<IUnknown> x,y;return SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x))) && SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y))) && x==y;
}

void decline(Eye& e,const char* why) {
    ++declined;e.complete=false;e.aborted=true;
    if(!failed){failed=true;enabled=false;resetPending[0]=resetPending[1]=true;
        Log::get().note("Deferred UI: disabled until AA is switched Off and back on, or the game restarts. Existing UI handling resumes on following frames; temporal history restarts once per eye.");}
    static unsigned reports=0;
    if(reports++<12)Log::get().note("Deferred UI: original frame retained: %s (draws=%u, VS=%016llX PS=%016llX).",why,e.count,bindingShaderHash(BindSlot::Vs),bindingShaderHash(BindSlot::Ps));
}
bool context(ID3D11DeviceContext* ctx) {
    if(recorder)return true;
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
    return SUCCEEDED(dev->CreateDeferredContext(0,&recorder));
}
bool finish(Ptr<ID3D11CommandList>& list) {
    list.Reset();return SUCCEEDED(recorder->FinishCommandList(FALSE,&list));
}
void execute(ID3D11DeviceContext* ctx,ID3D11CommandList* list) {
    if(list)vScreenExecuteCommandListRaw(ctx,list,TRUE);
}
// Original game colour is never removed: failure can always use it.
void restore(ID3D11DeviceContext*,Eye& e) { e.restored=true; }

bool target(ID3D11DeviceContext* ctx,Ptr<ID3D11RenderTargetView>& rt,Ptr<ID3D11DepthStencilView>& ds,Ptr<ID3D11Texture2D>& tex) {
    ID3D11RenderTargetView* raw[8]{};ctx->OMGetRenderTargets(8,raw,&ds);rt.Attach(raw[0]);bool single=rt!=nullptr;
    for(UINT i=1;i<8;++i)if(raw[i]){single=false;raw[i]->Release();}
    if(!single)return false;
    Ptr<ID3D11Resource> r;rt->GetResource(&r);return SUCCEEDED(r.As(&tex));
}
size_t material(ID3D11Device* dev,ID3D11PixelShader* ps,ID3D11BlendState* blend) {
    for(size_t i=0;i<materials.size();++i)if(materials[i].original.Get()==ps && materials[i].originalBlend.Get()==blend)return i;
    if(materials.size()>=128)return SIZE_MAX;
    D3D11_BLEND_DESC bd{};
    for(auto& r:bd.RenderTarget){r.SrcBlend=r.SrcBlendAlpha=D3D11_BLEND_ONE;r.DestBlend=r.DestBlendAlpha=D3D11_BLEND_ZERO;r.BlendOp=r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=15;}
    if(blend)blend->GetDesc(&bd);
    Ptr<ID3D11BlendState1> b1;if(blend && SUCCEEDED(blend->QueryInterface(IID_PPV_ARGS(&b1)))){D3D11_BLEND_DESC1 d{};b1->GetDesc1(&d);if(d.RenderTarget[0].LogicOpEnable)return SIZE_MAX;}
    auto r=bd.RenderTarget[0];
    const bool sourceIndependent=r.SrcBlend==D3D11_BLEND_ZERO || r.SrcBlend==D3D11_BLEND_ONE || r.SrcBlend==D3D11_BLEND_SRC_COLOR || r.SrcBlend==D3D11_BLEND_INV_SRC_COLOR || r.SrcBlend==D3D11_BLEND_SRC_ALPHA || r.SrcBlend==D3D11_BLEND_INV_SRC_ALPHA || r.SrcBlend==D3D11_BLEND_BLEND_FACTOR || r.SrcBlend==D3D11_BLEND_INV_BLEND_FACTOR;
    // Only source-independent attenuation can be carried in one scalar.
    if(bd.AlphaToCoverageEnable || (r.RenderTargetWriteMask&7)!=7 ||
       (r.BlendEnable && (!sourceIndependent || r.BlendOp!=D3D11_BLEND_OP_ADD ||
        (r.DestBlend!=D3D11_BLEND_ZERO && r.DestBlend!=D3D11_BLEND_ONE && r.DestBlend!=D3D11_BLEND_INV_SRC_ALPHA) ||
        r.SrcBlend==D3D11_BLEND_DEST_COLOR || r.SrcBlend==D3D11_BLEND_INV_DEST_COLOR ||
        r.SrcBlend==D3D11_BLEND_DEST_ALPHA || r.SrcBlend==D3D11_BLEND_INV_DEST_ALPHA)))return SIZE_MAX;
    UINT n=0;ps->GetPrivateData(kDeferredBytes,&n,nullptr);if(!n || n>1024*1024)return SIZE_MAX;
    std::vector<BYTE> original(n),patched;std::string why;
    if(FAILED(ps->GetPrivateData(kDeferredBytes,&n,original.data())) || !uiColourFanout(original.data(),n,patched,why))return SIZE_MAX;
    Material m;m.original=ps;m.originalBlend=blend;
    if(FAILED(dev->CreatePixelShader(patched.data(),patched.size(),nullptr,&m.fanout)))return SIZE_MAX;
    auto world=bd;world.IndependentBlendEnable=TRUE;world.RenderTarget[1]=r;
    if(FAILED(dev->CreateBlendState(&world,&m.world)))return SIZE_MAX;
    bd.IndependentBlendEnable=TRUE;bd.RenderTarget[1].RenderTargetWriteMask=0;
    auto& t=bd.RenderTarget[2];t={};t.BlendEnable=TRUE;t.SrcBlend=t.SrcBlendAlpha=D3D11_BLEND_ZERO;
    t.DestBlend=t.DestBlendAlpha=r.BlendEnable?r.DestBlend:D3D11_BLEND_ZERO;
    t.BlendOp=t.BlendOpAlpha=D3D11_BLEND_OP_ADD;t.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED;
    if(FAILED(dev->CreateBlendState(&bd,&m.transmission)))return SIZE_MAX;
    materials.push_back(std::move(m));return materials.size()-1;
}

struct Renderer {
    Surface hdr,transmission,base,ui,composite;
    edvr_deferred_depth::Seeder depth;
    bool depthReady=false,shaderTried=false;
    Ptr<ID3D11VertexShader> vs;
    Ptr<ID3D11PixelShader> seed;
    Ptr<ID3D11ComputeShader> combine;
    Ptr<ID3D11SamplerState> sampler;
    Ptr<ID3D11Buffer> params;
    bool ensure(ID3D11DeviceContext* ctx,UINT w,UINT h,DXGI_FORMAT fmt) {
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(!shaderTried){shaderTried=true;vs.Attach(shaderSwapCompileVs(ctx,kDeferredSeed,sizeof(kDeferredSeed)-1,"vs","UI seed",nullptr,"Deferred UI"));
            seed.Attach(shaderSwapCompilePs(ctx,kDeferredSeed,sizeof(kDeferredSeed)-1,"ps","UI seed",nullptr,"Deferred UI"));
            combine.Attach(shaderSwapCompileCs(ctx,kDeferredComposite,sizeof(kDeferredComposite)-1,"main","UI composite",nullptr,"Deferred UI"));}
        if(!vs || !seed || !combine)return false;
        if(!sampler){D3D11_SAMPLER_DESC d{};d.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;d.AddressU=d.AddressV=d.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;d.MaxLOD=D3D11_FLOAT32_MAX;
            if(FAILED(dev->CreateSamplerState(&d,&sampler)))return false;}
        if(!params){D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;if(FAILED(dev->CreateBuffer(&d,nullptr,&params)))return false;}
        if(!depthReady){depth.init(dev.Get());depthReady=true;}
        return depth.ensure(dev.Get(),w,h,fmt) && hdr.ensure(dev.Get(),w,h,DXGI_FORMAT_R11G11B10_FLOAT) &&
            transmission.ensure(dev.Get(),w,h,DXGI_FORMAT_R32_FLOAT) && base.ensure(dev.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM) &&
            ui.ensure(dev.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM) && composite.ensure(dev.Get(),w,h,DXGI_FORMAT_R8G8B8A8_UNORM,true);
    }
} renderer;
} // namespace

bool uiDeferredInternal(){return inside;}
void uiDeferredRemember(ID3D11DeviceChild* shader,const void* data,size_t size,bool linked) {
    if(!shader || !data || linked || inside || size<32 || size>1024*1024)return;
    shader->SetPrivateData(kDeferredBytes,UINT(size),data);
}
void uiDeferredConfigure(Config& cfg) {
    const auto m=cfg.getString("fix.temporal_aa","off");
    const bool requested=(_stricmp(m.c_str(),"dlss")==0 || _stricmp(m.c_str(),"dlaa")==0);
    if(!requested){failed=false;routeNoted=false;}
    enabled=requested && !failed && cfg.getFloat("advanced.temporal_aa_fovea",0)==0;
}
bool uiDeferredFallbackReset(int eye){if(eye<0 || eye>1)return false;bool reset=resetPending[eye];resetPending[eye]=false;return reset;}
static bool begin(ID3D11DeviceContext* ctx,int eye,char kind,UINT count,UINT instances,UINT start,INT base,UINT first) {
    if(!enabled || inside || eye>1 || colourMuted)return false;
    if(eye<0) {
        // Elite interleaves opaque and translucent world draws with UI.
        // Mirror those world draws into the clean target in the same raster
        // pass. The original target and its depth/stencil remain unchanged.
        bool pending=false;for(auto& e:eyes)pending=pending || (e.count && !e.complete && !e.aborted);
        if(!pending)return false;
        Scope scope;Ptr<ID3D11RenderTargetView> rt;Ptr<ID3D11DepthStencilView> ds;Ptr<ID3D11Texture2D> tex;
        ID3D11RenderTargetView* bound[8]{};ctx->OMGetRenderTargets(8,bound,&ds);
        std::array<Ptr<ID3D11RenderTargetView>,8> retained;bool touches=false,multiple=false;
        for(UINT i=0;i<8;++i){retained[i].Attach(bound[i]);if(!bound[i])continue;multiple=multiple || i!=0;Ptr<ID3D11Resource> resource;bound[i]->GetResource(&resource);
            for(auto& e:eyes)if(e.count && !e.complete && !e.aborted && sameIdentity(e.hdr.Get(),resource.Get()))touches=true;}
        if(!touches)return false;
        if(!bound[0] || multiple){for(auto& e:eyes)if(e.count && !e.complete)decline(e,"interleaved MRT world draw");return false;}
        rt=retained[0];Ptr<ID3D11Resource> resource;rt->GetResource(&resource);if(FAILED(resource.As(&tex)))return false;
        for(auto& e:eyes)if(e.count && !e.complete && !e.aborted && e.hdr==tex) {
            Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);Ptr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);
            if(!ps)return false; // A depth-only world draw cannot change colour.
            ID3D11UnorderedAccessView* uavs[8]{};ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs);bool hasUav=false;for(auto* u:uavs)if(u){hasUav=true;u->Release();}
            if(hasUav){decline(e,"interleaved world UAV writes");return false;}
            Ptr<ID3D11DepthStencilState> state;ctx->OMGetDepthStencilState(&state,nullptr);D3D11_DEPTH_STENCIL_DESC dd{};if(state)state->GetDesc(&dd);
            if(dd.StencilEnable && (dd.StencilReadMask&e.changedStencil) && (dd.FrontFace.StencilFunc!=D3D11_COMPARISON_ALWAYS || dd.BackFace.StencilFunc!=D3D11_COMPARISON_ALWAYS)){
                decline(e,"world draw reads stencil changed by UI");return false;}
            Ptr<ID3D11BlendState> blend;ctx->OMGetBlendState(&blend,nullptr,nullptr);
            auto index=ps?material(dev.Get(),ps.Get(),blend.Get()):SIZE_MAX;
            if(index==SIZE_MAX){decline(e,"interleaved world shader/blend unsupported");return false;}
            savedTarget=rt;savedDepth=ds;savedPs=ps;ctx->OMGetBlendState(&savedBlend,savedFactors,&savedMask);
            ID3D11RenderTargetView* targets[2]={rt.Get(),e.cleanHdr.rtv.Get()};
            vScreenSetRenderTargetsRaw(ctx,2,targets,ds.Get());ctx->OMSetBlendState(materials[index].world.Get(),savedFactors,savedMask);ctx->PSSetShader(materials[index].fanout.Get(),nullptr,0);colourMuted=true;
            return false;
        }
        return false;
    }
    auto& e=eyes[eye];if(e.complete || e.aborted || e.restored)return false;
    Scope scope;
    Ptr<ID3D11RenderTargetView> rt;Ptr<ID3D11DepthStencilView> ds;Ptr<ID3D11Texture2D> tex;
    if(!target(ctx,rt,ds,tex))return false;
    D3D11_TEXTURE2D_DESC td{};tex->GetDesc(&td);
    if(td.Format!=DXGI_FORMAT_R11G11B10_FLOAT || td.SampleDesc.Count!=1 || td.MipLevels!=1 || td.ArraySize!=1)return false;
    D3D11_VIEWPORT viewport{};UINT viewportCount=1;ctx->RSGetViewports(&viewportCount,&viewport);
    if(viewportCount!=1 || viewport.TopLeftX!=0 || viewport.TopLeftY!=0 || viewport.Width!=td.Width || viewport.Height!=td.Height)return false;
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
    if(!context(ctx))return false;
    if(!e.count){
        if(!e.cleanHdr.ensure(dev.Get(),td.Width,td.Height,td.Format) || !e.cleanLdr.ensure(dev.Get(),td.Width,td.Height,DXGI_FORMAT_R8G8B8A8_UNORM) || !e.depth.seed(dev.Get(),ctx,ds.Get()))return false;
        e.hdr=tex;e.hdrTarget=rt;e.w=td.Width;e.h=td.Height;ctx->CopyResource(e.cleanHdr.tex.Get(),tex.Get());
    }else {Ptr<ID3D11Resource> depth;if(ds)ds->GetResource(&depth);if(e.hdr.Get()!=tex.Get() || !sameIdentity(e.depth.source.Get(),depth.Get()))return false;}
    if(e.count>=64)return false;
    if(e.draws.size()==e.count)e.draws.push_back(std::make_unique<Draw>());
    auto& d=*e.draws[e.count];
    const auto* mask=reflect(ctx);if(!mask)return false;
    if(!d.packet.capture(ctx,snapshots,kind,count,instances,start,base,first,*mask))return false;
    D3D11_DEPTH_STENCIL_DESC depthState{};
    if(d.packet.depthState())d.packet.depthState()->GetDesc(&depthState);
    else {depthState.DepthEnable=TRUE;depthState.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;}
    // Final world depth can seed native UI only if UI did not replace it.
    if(depthState.DepthEnable && depthState.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL)return false;
    if(depthState.StencilEnable)for(const auto& face:{depthState.FrontFace,depthState.BackFace}){
        if(face.StencilFailOp!=D3D11_STENCIL_OP_KEEP || face.StencilDepthFailOp!=D3D11_STENCIL_OP_KEEP)e.changedStencil|=depthState.StencilWriteMask;
        if(face.StencilPassOp!=D3D11_STENCIL_OP_KEEP)e.changedStencil|=depthState.StencilWriteMask &
            (face.StencilPassOp==D3D11_STENCIL_OP_REPLACE && face.StencilFunc==D3D11_COMPARISON_EQUAL?~depthState.StencilReadMask:255u);
    }
    d.material=material(dev.Get(),d.packet.originalPixelShader(),d.packet.blendState());
    if(d.material==SIZE_MAX)return false;
    ++e.count;++captured;
    if(captured<=12)Log::get().note("Deferred UI: captured %c draw, VS=%016llX PS=%016llX eye=%d %ux%u.",kind,bindingShaderHash(BindSlot::Vs),bindingShaderHash(BindSlot::Ps),eye,e.w,e.h);
    return true;
}
bool uiDeferredBegin(ID3D11DeviceContext* ctx,int eye,char kind,UINT count,UINT instances,UINT start,INT base,UINT first) {
    bool ok=false;
    try { ok=begin(ctx,eye,kind,count,instances,start,base,first); }
    catch(...) {
        uiDeferredEnd(ctx);
        for(auto& e:eyes)if(e.count && !e.complete)decline(e,"draw allocation failed");
    }
    if(!ok && enabled && !inside && eye>=0 && eye<2 && !eyes[eye].aborted) {
        // An unsupported UI in the middle of the sequence must retain its
        // order relative to already captured UI, including the fallback.
        auto& e=eyes[eye];restore(ctx,e);decline(e,"UI state, resources or allocation unsupported");
    }
    return ok;
}
void uiDeferredEnd(ID3D11DeviceContext* ctx) {
    if(!colourMuted)return;Scope scope;vScreenSetRenderTargetsRaw(ctx,1,savedTarget.GetAddressOf(),savedDepth.Get());ctx->OMSetBlendState(savedBlend.Get(),savedFactors,savedMask);ctx->PSSetShader(savedPs.Get(),nullptr,0);savedBlend.Reset();savedPs.Reset();savedTarget.Reset();savedDepth.Reset();colourMuted=false;
}

static void beforeTone(ID3D11DeviceContext* ctx,char kind,UINT count,UINT instances,UINT start,INT base,UINT first) {
    if(inside || !enabled)return;
    if(bindingShaderHash(BindSlot::Vs)!=EyeTonemapSnapshot::kVs || bindingShaderHash(BindSlot::Ps)!=EyeTonemapSnapshot::kPs)return;
    Scope scope;Ptr<ID3D11ShaderResourceView> input;ctx->PSGetShaderResources(1,1,&input);if(!input)return;
    Ptr<ID3D11Resource> source;input->GetResource(&source);
    for(auto& e:eyes)if(e.count && sameIdentity(e.hdr.Get(),source.Get()) && !e.restored && !e.aborted) {
        Ptr<ID3D11RenderTargetView> rt;Ptr<ID3D11DepthStencilView> ds;Ptr<ID3D11Texture2D> tex;
        bool valid=kind=='N' && count==3 && instances==1 && target(ctx,rt,ds,tex);
        D3D11_TEXTURE2D_DESC td{};if(tex)tex->GetDesc(&td);
        valid=valid && td.Width==e.w && td.Height==e.h && td.MipLevels==1 && td.ArraySize==1 && td.SampleDesc.Count==1;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};if(rt)rt->GetDesc(&rd);valid=valid && rd.Format==DXGI_FORMAT_R8G8B8A8_UNORM;
        Ptr<ID3D11DepthStencilState> dss;ctx->OMGetDepthStencilState(&dss,nullptr);D3D11_DEPTH_STENCIL_DESC dd{};if(dss)dss->GetDesc(&dd);else dd.DepthEnable=TRUE;
        Ptr<ID3D11BlendState> bs;ctx->OMGetBlendState(&bs,nullptr,nullptr);D3D11_BLEND_DESC bd{};if(bs)bs->GetDesc(&bd);
        Ptr<ID3D11RasterizerState> rs;ctx->RSGetState(&rs);D3D11_RASTERIZER_DESC rsd{};if(rs)rs->GetDesc(&rsd);else rsd.FillMode=D3D11_FILL_SOLID;
        D3D11_VIEWPORT vp{};UINT vn=1;ctx->RSGetViewports(&vn,&vp);
        valid=valid && !dd.DepthEnable && !dd.StencilEnable && !bd.AlphaToCoverageEnable && !bd.RenderTarget[0].BlendEnable && (!bs || (bd.RenderTarget[0].RenderTargetWriteMask&7)==7) && !rsd.ScissorEnable && rsd.FillMode==D3D11_FILL_SOLID && vn==1 && vp.TopLeftX==0 && vp.TopLeftY==0 && vp.Width==e.w && vp.Height==e.h;
        const auto* reflected=reflect(ctx);valid=valid && reflected;
        UiDeferredCaptureMask mask;if(reflected)mask=*reflected;mask.tone=true;
        if(valid)valid=e.tone.capture(ctx,snapshots,kind,count,instances,start,base,first,mask);
        if(valid){
            ctx->CopyResource(e.depth.before.Get(),e.depth.source.Get());recorder->ClearState();
            valid=e.tone.bind(recorder.Get(),e.tone.originalPixelShader(),e.cleanLdr.rtv.Get(),nullptr,nullptr,e.tone.originalViewport());
            auto* hdr=e.cleanHdr.srv.Get();recorder->PSSetShaderResources(1,1,&hdr);e.tone.draw(recorder.Get());
            Ptr<ID3D11CommandList> list;valid=valid && finish(list);if(valid)execute(ctx,list.Get());
        }
        // The game receives a complete colour image regardless of NGX success,
        // unsupported submit routes, cropped AA modes, or capture-only submits.
        restore(ctx,e);
        if(valid){e.complete=true;e.aliases.clear();e.aliases.push_back(tex);}
        else decline(e,"tone pass unsupported");
        return;
    }
}
void uiDeferredBeforeTone(ID3D11DeviceContext* ctx,char kind,UINT count,UINT instances,UINT start,INT base,UINT first) {
    try { beforeTone(ctx,kind,count,instances,start,base,first); }
    catch(...) { for(auto& e:eyes)if(e.count && !e.restored){restore(ctx,e);decline(e,"tone allocation failed");} }
}

static ID3D11ShaderResourceView* prepare(ID3D11DeviceContext* ctx,ID3D11Texture2D* submitted,
    ID3D11ShaderResourceView* sceneDepth,ID3D11Texture2D* output,int eye,UINT w,UINT h,float jx,float jy) {
    if(!enabled || inside || eye<0 || eye>1 || !submitted || !output || !sceneDepth)return nullptr;
    Eye* match=nullptr;
    for(auto& e:eyes)if(e.complete && e.w==w && e.h==h)for(const auto& alias:e.aliases)if(sameIdentity(alias.Get(),submitted)){if(match && match!=&e)return nullptr;match=&e;break;}
    if(!match){if(!routeNoted && (eyes[0].count || eyes[1].count)){routeNoted=true;Log::get().note("Deferred UI: submit route not matched: submitted=%p tone0=%p tone1=%p; original frame retained.",submitted,eyes[0].aliases.empty()?nullptr:eyes[0].aliases.front().Get(),eyes[1].aliases.empty()?nullptr:eyes[1].aliases.front().Get());decline(eyes[eye],"submitted colour has no complete matching replay");}return nullptr;}
    auto& e=*match;Scope scope;
    UINT stencilRead=0;bool needsDepth=false;
    for(UINT i=0;i<e.count;++i){D3D11_DEPTH_STENCIL_DESC dd{};e.draws[i]->packet.depthState()->GetDesc(&dd);needsDepth=needsDepth || dd.DepthEnable;
        if(dd.StencilEnable && (dd.FrontFace.StencilFunc!=D3D11_COMPARISON_ALWAYS || dd.BackFace.StencilFunc!=D3D11_COMPARISON_ALWAYS))stencilRead|=dd.StencilReadMask;}
    for(UINT i=0;i<e.count;++i){D3D11_DEPTH_STENCIL_DESC dd{};e.draws[i]->packet.depthState()->GetDesc(&dd);
        if(dd.StencilEnable && (dd.StencilWriteMask&stencilRead))for(const auto& face:{dd.FrontFace,dd.BackFace}){
            if(face.StencilFailOp!=D3D11_STENCIL_OP_KEEP || face.StencilDepthFailOp!=D3D11_STENCIL_OP_KEEP ||
               (face.StencilPassOp!=D3D11_STENCIL_OP_KEEP && !(face.StencilPassOp==D3D11_STENCIL_OP_REPLACE && face.StencilFunc==D3D11_COMPARISON_EQUAL && !(stencilRead&dd.StencilWriteMask&~dd.StencilReadMask)))){
                decline(e,"UI changes a stencil bit needed by another UI draw");return nullptr;}
        }}
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);D3D11_TEXTURE2D_DESC od{};output->GetDesc(&od);
    if(!renderer.ensure(ctx,od.Width,od.Height,e.depth.format)){decline(e,"native output resources unavailable");return nullptr;}
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;
    Ptr<ID3D11ShaderResourceView> world;if(FAILED(dev->CreateShaderResourceView(output,&sv,&world)))return nullptr;
    auto* c=recorder.Get();c->ClearState();
    if(!renderer.depth.record(c,e.depth.depth.Get(),e.depth.stencil.Get(),w,h,jx,jy,stencilRead,needsDepth))return nullptr;
    c->ClearState();float p[4]={float(od.Width),float(od.Height),jx/w,jy/h};c->UpdateSubresource(renderer.params.Get(),0,nullptr,p,0,0);
    D3D11_VIEWPORT vp{0,0,float(od.Width),float(od.Height),0,1};c->RSSetViewports(1,&vp);
    c->OMSetRenderTargets(1,renderer.hdr.rtv.GetAddressOf(),nullptr);c->VSSetShader(renderer.vs.Get(),nullptr,0);c->PSSetShader(renderer.seed.Get(),nullptr,0);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->PSSetConstantBuffers(0,1,renderer.params.GetAddressOf());c->PSSetShaderResources(0,1,e.cleanHdr.srv.GetAddressOf());c->PSSetSamplers(0,1,renderer.sampler.GetAddressOf());c->Draw(3,0);
    c->ClearState();
    if(!e.tone.bind(c,e.tone.originalPixelShader(),renderer.base.rtv.Get(),nullptr,nullptr,vp))return nullptr;
    c->PSSetShaderResources(1,1,renderer.hdr.srv.GetAddressOf());e.tone.draw(c);
    const float one[4]={1,1,1,1};c->ClearRenderTargetView(renderer.transmission.rtv.Get(),one);
    D3D11_VIEWPORT uiVp=vp;uiVp.TopLeftX=-jx*od.Width/w;uiVp.TopLeftY=-jy*od.Height/h;
    for(UINT i=0;i<e.count;++i){const auto& d=*e.draws[i];const auto& m=materials[d.material];c->ClearState();
        const auto originalVp=d.packet.originalViewport();uiVp.MinDepth=originalVp.MinDepth;uiVp.MaxDepth=originalVp.MaxDepth;
        auto sc=d.packet.originalScissor();sc.left=LONG(std::floor(sc.left*float(od.Width)/w+uiVp.TopLeftX));sc.right=LONG(std::ceil(sc.right*float(od.Width)/w+uiVp.TopLeftX));sc.top=LONG(std::floor(sc.top*float(od.Height)/h+uiVp.TopLeftY));sc.bottom=LONG(std::ceil(sc.bottom*float(od.Height)/h+uiVp.TopLeftY));
        if(!d.packet.bind(c,m.fanout.Get(),renderer.hdr.rtv.Get(),renderer.transmission.rtv.Get(),renderer.depth.view(),uiVp,&sc))return nullptr;
        d.packet.setBlend(c,m.transmission.Get());d.packet.draw(c);
    }
    c->ClearState();if(!e.tone.bind(c,e.tone.originalPixelShader(),renderer.ui.rtv.Get(),nullptr,nullptr,vp))return nullptr;
    c->PSSetShaderResources(1,1,renderer.hdr.srv.GetAddressOf());e.tone.draw(c);
    c->ClearState();ID3D11ShaderResourceView* views[4]={world.Get(),renderer.base.srv.Get(),renderer.ui.srv.Get(),renderer.transmission.srv.Get()};
    c->CSSetShader(renderer.combine.Get(),nullptr,0);c->CSSetShaderResources(0,4,views);c->CSSetUnorderedAccessViews(0,1,renderer.composite.uav.GetAddressOf(),nullptr);c->CSSetConstantBuffers(0,1,renderer.params.GetAddressOf());c->Dispatch((od.Width+7)/8,(od.Height+7)/8,1);
    c->ClearState();c->CopyResource(output,renderer.composite.tex.Get());
    if(!finish(eyes[eye].output))return nullptr;
    return e.cleanLdr.srv.Get();
}
ID3D11ShaderResourceView* uiDeferredPrepare(ID3D11DeviceContext* ctx,ID3D11Texture2D* submitted,
    ID3D11ShaderResourceView* sceneDepth,ID3D11Texture2D* output,int eye,UINT w,UINT h,float jx,float jy) {
    if(eye>=0 && eye<2)eyes[eye].output.Reset();
    try { return prepare(ctx,submitted,sceneDepth,output,eye,w,h,jx,jy); }
    catch(...) { if(eye>=0 && eye<2)decline(eyes[eye],"output replay allocation failed");return nullptr; }
}
void uiDeferredApply(ID3D11DeviceContext* ctx,int eye) {
    if(eye<0 || eye>1 || !eyes[eye].output)return;Scope scope;execute(ctx,eyes[eye].output.Get());eyes[eye].output.Reset();++applied;
    if(!noted){noted=true;Log::get().note("Deferred UI: active. DLSS receives world colour; captured UI draws run at output resolution after reconstruction, with original shaders, tone map and depth/stencil. No deferred UI motion/history pass.");}
}
void uiDeferredResourceWrite(ID3D11DeviceContext* ctx,ID3D11Resource* r) {
    if(inside || !enabled || !r)return;
    snapshots.written(r);
    for(auto& e:eyes){
        if(!e.restored && e.count && (e.hdr.Get()==r || e.depth.source.Get()==r)){restore(ctx,e);decline(e,"scene overwritten before deferred replay");}
        if(e.complete){e.aliases.erase(std::remove_if(e.aliases.begin(),e.aliases.end(),[&](const auto& a){return a.Get()==r;}),e.aliases.end());e.complete=!e.aliases.empty();}
    }
}
void uiDeferredViewWrite(ID3D11DeviceContext* ctx,ID3D11View* v){if(inside || !enabled || !v)return;Ptr<ID3D11Resource> r;v->GetResource(&r);uiDeferredResourceWrite(ctx,r.Get());}
void uiDeferredCopy(ID3D11Resource* dst,ID3D11Resource* src,bool complete) {
    if(inside || !enabled || !complete || !dst || !src)return;
    for(auto& e:eyes)if(e.count && !e.complete && !e.aborted && sameIdentity(e.hdr.Get(),src))decline(e,"HDR copied to another world input before tone");
    for(auto& e:eyes)if(e.complete && e.aliases.size()<16){bool found=false;for(const auto& a:e.aliases)if(a.Get()==src){found=true;break;}if(found)e.aliases.emplace_back(dst);}
}
void uiDeferredCopyRegion(ID3D11Resource* dst,UINT dstSub,UINT x,UINT y,UINT z,ID3D11Resource* src,UINT srcSub,const D3D11_BOX* box) {
    if(inside || !enabled || !dst || !src || dstSub || srcSub || x || y || z)return;
    bool known=false;for(auto& e:eyes)if(e.complete)for(auto& a:e.aliases)known=known || a.Get()==src;
    if(!known)return;
    Ptr<ID3D11Texture2D> source,dest;if(FAILED(src->QueryInterface(IID_PPV_ARGS(&source))) || FAILED(dst->QueryInterface(IID_PPV_ARGS(&dest))))return;
    D3D11_TEXTURE2D_DESC s{},d{};source->GetDesc(&s);dest->GetDesc(&d);
    if(s.Width!=d.Width || s.Height!=d.Height || s.MipLevels!=1 || d.MipLevels!=1 || s.ArraySize!=1 || d.ArraySize!=1 || s.SampleDesc.Count!=1 || d.SampleDesc.Count!=1)return;
    if(box && (box->left || box->top || box->front || box->right!=s.Width || box->bottom!=s.Height || box->back!=1))return;
    uiDeferredCopy(dst,src,true);
}
void uiDeferredBeforeDraw(ID3D11DeviceContext* ctx) {
    if(inside || !enabled || (!eyes[0].count && !eyes[1].count))return;
    ID3D11RenderTargetView* views[8]{};Ptr<ID3D11DepthStencilView> depth;ctx->OMGetRenderTargets(8,views,&depth);
    if(depth){Ptr<ID3D11Resource> r;depth->GetResource(&r);snapshots.written(r.Get());}
    for(auto* v:views)if(v){Ptr<ID3D11Resource> r;v->GetResource(&r);snapshots.written(r.Get());
        for(auto& e:eyes)if(e.complete){e.aliases.erase(std::remove_if(e.aliases.begin(),e.aliases.end(),[&](const auto& a){return a==r;}),e.aliases.end());e.complete=!e.aliases.empty();}
        v->Release();}
}
void uiDeferredBeforeDispatch(ID3D11DeviceContext* ctx) {
    if(inside || !enabled || (!eyes[0].count && !eyes[1].count))return;
    ID3D11UnorderedAccessView* views[8]{};ctx->CSGetUnorderedAccessViews(0,8,views);
    for(auto* v:views)if(v){uiDeferredViewWrite(ctx,v);v->Release();}
}
void uiDeferredUnknownWrite(ID3D11DeviceContext* ctx){if(inside || !enabled)return;snapshots.unknownWrite();for(auto& e:eyes)if(e.count){restore(ctx,e);decline(e,"untracked command");}}
void uiDeferredFrameBoundary(ID3D11DeviceContext* ctx) {
    static uint64_t frames=0;
    if(enabled && ++frames%600==0)Log::get().note("Deferred UI: totals captured=%llu applied=%llu declined=%llu, snapshots copied=%.2f MiB allocated=%.2f MiB.",captured,applied,declined,snapshots.copiedBytes()/1048576.,snapshots.allocatedBytes()/1048576.);
    for(auto& e:eyes){if(e.count && !e.restored)restore(ctx,e);for(UINT i=0;i<e.count;++i){e.draws[i]->packet=UiDeferredDraw{};}e.tone=UiDeferredDraw{};e.count=e.changedStencil=0;e.bytes=0;e.complete=e.restored=e.aborted=false;e.aliases.clear();e.output.Reset();}
    snapshots.frameBoundary();
}
void uiDeferredShutdown(){for(auto& e:eyes)e=Eye{};renderer=Renderer{};materials.clear();masks.clear();snapshots=UiDeferredSnapshots{};recorder.Reset();savedBlend.Reset();savedPs.Reset();savedTarget.Reset();savedDepth.Reset();enabled=failed=resetPending[0]=resetPending[1]=colourMuted=noted=routeNoted=false;captured=applied=declined=0;}
}
