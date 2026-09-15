#include "ui_separation.h"
#include "ui_colour_layer.h"
#include "dxbc_fanout.h"
#include "ui_depth.h"
#include "binding_shadow.h"
#include "eye_tonemap_snapshot.h"
#include "../common/config.h"
#include "../common/log.h"
#include <vector>

namespace edvr { namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
const GUID bytesKey={0xf95e498e,0xf733,0x4f23,{0x9f,0xc1,0x62,0x20,0xa1,0xa8,0xd5,0xeb}};
const GUID cloneKey={0xf95e498e,0xf733,0x4f23,{0x9f,0xc1,0x62,0x20,0xa1,0xa8,0xd5,0xec}};
struct Eye {
    UiColourLayer layer;
    Ptr<ID3D11Texture2D> toneOutput,converted;
    Ptr<ID3D11ShaderResourceView> convertedSrv;
    Ptr<ID3D11RenderTargetView> convertedRtv;
    bool complete=false;
    uint32_t uiDraws=0,lateDraws=0;
};
Eye eyes[2];
bool enabled=false,failed=false,noted=false,usedNoted=false;
// Device shader creation may run concurrently with the render-context hooks.
// An internal render operation must not suppress another thread's bytecode.
thread_local bool internal=false;
int active=-1,tone=-1;
Ptr<ID3D11ShaderResourceView> toneInput;
Ptr<ID3D11RenderTargetView> toneTarget;
Ptr<ID3D11DepthStencilView> toneDepth;
struct InternalScope {bool before=internal;InternalScope(){internal=true;}~InternalScope(){internal=before;}};
void decline(const char* why) {
    if(!failed)Log::get().note("UI separation: disabled for this session: %s. Original game colour retained; temporal history will restart once.",why);
    failed=true;for(auto& e:eyes){e.complete=false;e.layer.invalidate();}uiDepthSeparatedInvalidate();
}
Ptr<ID3D11PixelShader> shader(ID3D11DeviceContext* ctx) {
    Ptr<ID3D11PixelShader> original;ctx->PSGetShader(&original,nullptr,nullptr);if(!original)return {};
    Ptr<ID3D11PixelShader> cached;UINT n=sizeof(ID3D11PixelShader*);
    if(SUCCEEDED(original->GetPrivateData(cloneKey,&n,cached.GetAddressOf())) && cached)return cached;
    n=0;HRESULT hr=original->GetPrivateData(bytesKey,&n,nullptr);
    if((FAILED(hr) && hr!=DXGI_ERROR_MORE_DATA) || !n || n>1024*1024){decline("pixel shader bytecode unavailable");return {};}
    std::vector<BYTE> bytes(n),patched;std::string reason;
    if(FAILED(original->GetPrivateData(bytesKey,&n,bytes.data())) || !uiColourFanout(bytes.data(),n,patched,reason)) {
        decline(reason.empty()?"pixel shader retrieval failed":reason.c_str());return {};
    }
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
    if(FAILED(dev->CreatePixelShader(patched.data(),patched.size(),nullptr,&cached)) ||
       FAILED(original->SetPrivateDataInterface(cloneKey,cached.Get()))) {decline("pixel shader fanout creation failed");return {};}
    return cached;
}
void* currentTarget() {
    static uint32_t generation=~0u;static ResourceInfo info{};
    const auto now=bindingGeneration(BindSlot::Rtv0);
    if(now!=generation){generation=now;info={};bindingResolve(bindingGet(BindSlot::Rtv0),&info);}
    return info.resource;
}
bool singleTarget(ID3D11DeviceContext* ctx,Ptr<ID3D11RenderTargetView>& target,Ptr<ID3D11DepthStencilView>& depth) {
    ID3D11RenderTargetView* rt[8]{};ctx->OMGetRenderTargets(8,rt,&depth);target.Attach(rt[0]);bool ok=rt[0]!=nullptr;
    for(unsigned i=1;i<8;++i)if(rt[i]){ok=false;rt[i]->Release();}
    return ok;
}
} // namespace
void uiSeparationRemember(ID3D11PixelShader* ps,const void* data,size_t n,bool linked) {
    if(!ps || !data || linked || internal || n<32 || n>1024*1024)return;
    ps->SetPrivateData(bytesKey,UINT(n),data);
}
void uiSeparationConfigure(Config& cfg) {
    const auto m=cfg.getString("fix.temporal_aa","off");
    enabled=_stricmp(m.c_str(),"dlss")==0 || _stricmp(m.c_str(),"dlaa")==0;
}
bool uiSeparationBegin(ID3D11DeviceContext* ctx) {
    if(!enabled || failed || internal || active>=0 || !ctx)return false;
    int eye=uiDepthTargetSpriteEye();const bool remove=eye>=0;
    void* target=currentTarget();
    if(eye<0)for(int i=0;i<2;++i) {
        if(eyes[i].complete && eyes[i].toneOutput.Get()==target){decline("draw after tone mapping");return false;}
        if(eyes[i].layer.ready() && eyes[i].layer.original()==target)eye=i;
    }
    if(eye<0 || eye>1)return false;
    InternalScope scope;
    auto ps=shader(ctx);if(!ps)return false;
    auto& e=eyes[eye];
    if(!e.layer.ready()) {
        Ptr<ID3D11RenderTargetView> rtv;Ptr<ID3D11DepthStencilView> depth;
        if(!remove || !singleTarget(ctx,rtv,depth) || !e.layer.seed(ctx,rtv.Get())) {decline("unsupported HDR target");return false;}
    }
    if(!e.layer.begin(ctx,ps.Get(),remove)){decline("unsupported blend or output state");return false;}
    active=eye;if(remove){++e.uiDraws;uiDepthSetTargetSeparated(true);}else ++e.lateDraws;
    return true;
}
void uiSeparationEnd(ID3D11DeviceContext* ctx) {
    if(active<0)return;InternalScope scope;eyes[active].layer.end(ctx);active=-1;
}
bool uiSeparationToneBegin(ID3D11DeviceContext* ctx,char kind,uint32_t count,uint32_t instances) {
    if(!enabled || failed || internal || tone>=0 ||
       bindingShaderHash(BindSlot::Vs)!=EyeTonemapSnapshot::kVs || bindingShaderHash(BindSlot::Ps)!=EyeTonemapSnapshot::kPs)return false;
    Ptr<ID3D11ShaderResourceView> hdr;ctx->PSGetShaderResources(1,1,&hdr);if(!hdr)return false;
    Ptr<ID3D11Resource> resource;hdr->GetResource(&resource);int eye=-1;
    for(int i=0;i<2;++i)if(eyes[i].layer.ready() && eyes[i].layer.original()==resource.Get())eye=i;
    if(eye<0)return false;
    if(eyes[eye].complete){decline("repeated tone map");return false;}
    if(kind!='N' || count!=3 || instances!=1){decline("unexpected tone-map geometry");return false;}
    Ptr<ID3D11Predicate> predicate;BOOL value=FALSE;ctx->GetPredication(&predicate,&value);
    if(predicate){decline("predicated tone map");return false;}
    Ptr<ID3D11DepthStencilState> dss;UINT ref=0;ctx->OMGetDepthStencilState(&dss,&ref);
    D3D11_DEPTH_STENCIL_DESC dd{};if(dss)dss->GetDesc(&dd);else dd.DepthEnable=TRUE;
    Ptr<ID3D11BlendState> bs;ctx->OMGetBlendState(&bs,nullptr,nullptr);
    D3D11_BLEND_DESC bd{};if(bs)bs->GetDesc(&bd);
    if(dd.DepthEnable || dd.StencilEnable || bd.AlphaToCoverageEnable || bd.RenderTarget[0].BlendEnable ||
       (bs && (bd.RenderTarget[0].RenderTargetWriteMask&7)!=7)){decline("unsupported tone-map blend or depth");return false;}
    Ptr<ID3D11RasterizerState> raster;ctx->RSGetState(&raster);
    D3D11_RASTERIZER_DESC rd{};if(raster)raster->GetDesc(&rd);else rd.FillMode=D3D11_FILL_SOLID;
    if(rd.ScissorEnable || rd.FillMode!=D3D11_FILL_SOLID){decline("partial tone-map rasterization");return false;}
    Ptr<ID3D11GeometryShader> gs;Ptr<ID3D11HullShader> hs;Ptr<ID3D11DomainShader> ds;
    ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&ds,nullptr,nullptr);
    ID3D11Buffer* so[4]{};ctx->SOGetTargets(4,so);bool extra=gs || hs || ds;
    for(auto* b:so)if(b){extra=true;b->Release();}
    ID3D11UnorderedAccessView* uavs[8]{};ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs);
    for(auto* u:uavs)if(u){extra=true;u->Release();}
    if(extra){decline("tone-map side effects");return false;}
    Ptr<ID3D11RenderTargetView> rtv;Ptr<ID3D11DepthStencilView> depth;
    if(!singleTarget(ctx,rtv,depth)){decline("multiple tone-map targets");return false;}
    Ptr<ID3D11Resource> output;rtv->GetResource(&output);Ptr<ID3D11Texture2D> tex;
    D3D11_RENDER_TARGET_VIEW_DESC vd{};rtv->GetDesc(&vd);
    if(FAILED(output.As(&tex)) || vd.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice || vd.Format!=DXGI_FORMAT_R8G8B8A8_UNORM){decline("unsupported tone-map format");return false;}
    D3D11_TEXTURE2D_DESC td{},hd{};tex->GetDesc(&td);eyes[eye].layer.original()->GetDesc(&hd);
    D3D11_VIEWPORT vp{};UINT vn=1;ctx->RSGetViewports(&vn,&vp);
    if(td.ArraySize!=1 || td.MipLevels!=1 || td.SampleDesc.Count!=1 || td.Width!=hd.Width || td.Height!=hd.Height ||
       vn!=1 || vp.TopLeftX!=0 || vp.TopLeftY!=0 || vp.Width!=td.Width || vp.Height!=td.Height){decline("unsupported tone-map extent");return false;}
    InternalScope scope;auto& e=eyes[eye];
    if(e.toneOutput.Get()!=tex.Get() || !e.converted) {
        e.converted.Reset();e.convertedRtv.Reset();e.convertedSrv.Reset();e.toneOutput=tex;
        td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.Usage=D3D11_USAGE_DEFAULT;
        td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;td.CPUAccessFlags=td.MiscFlags=0;
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(FAILED(dev->CreateTexture2D(&td,nullptr,&e.converted)) || FAILED(dev->CreateRenderTargetView(e.converted.Get(),nullptr,&e.convertedRtv)) ||
           FAILED(dev->CreateShaderResourceView(e.converted.Get(),nullptr,&e.convertedSrv))){decline("tone-map resource allocation failed");return false;}
    }
    // The known triangle covers the whole viewport. Partial rasterization
    // is rejected above, so this needs neither a clear nor an eye-sized copy.
    toneInput=hdr;toneTarget=rtv;toneDepth=depth;tone=eye;
    auto* srv=e.layer.colourView();ctx->PSSetShaderResources(1,1,&srv);
    auto* target=e.convertedRtv.Get();vScreenSetRenderTargetsRaw(ctx,1,&target,depth.Get());return true;
}
void uiSeparationToneEnd(ID3D11DeviceContext* ctx) {
    if(tone<0)return;InternalScope scope;
    auto* target=toneTarget.Get();vScreenSetRenderTargetsRaw(ctx,1,&target,toneDepth.Get());
    auto* input=toneInput.Get();ctx->PSSetShaderResources(1,1,&input);
    eyes[tone].complete=true;tone=-1;toneInput.Reset();toneTarget.Reset();toneDepth.Reset();
}
bool uiSeparationInputs(ID3D11Texture2D* submitted,ID3D11Texture2D* scene,int eye,uint32_t w,uint32_t h,UiSeparatedInputs& out) {
    out={};if(!enabled || failed || eye<0 || eye>1)return false;auto& e=eyes[eye];
    if(!e.complete)return false;
    if(submitted!=e.toneOutput.Get()){decline("submitted colour differs from tracked tone-map output");return false;}
    D3D11_TEXTURE2D_DESC d{};e.converted->GetDesc(&d);if(d.Width!=w || d.Height!=h)return false;
    if(!uiDepthSeparatedCoverage(w,h,eye,scene,&out.mask,&out.holo,&out.edits,&out.depth)){decline("world coverage incomplete");out={};return false;}
    out.colour=e.converted.Get();out.colourView=e.convertedSrv.Get();out.influence=e.layer.influenceView();
    if(!noted){noted=true;Log::get().note("UI separation: prepared current target layer and world-only colour with original-format HDR MRT and game tone map, %ux%u.",w,h);}
    return true;
}
void uiSeparationResourceWrite(ID3D11Resource* destination) {
    if(internal || failed || !enabled || !destination)return;
    for(auto& e:eyes)if((e.layer.ready() && destination==e.layer.original()) || (e.complete && destination==e.toneOutput.Get())){decline("non-draw write to tracked colour");return;}
}
void uiSeparationViewWrite(ID3D11View* destination) {
    if(internal || failed || !enabled || !destination ||
       (!eyes[0].layer.ready() && !eyes[1].layer.ready()))return;
    Ptr<ID3D11Resource> resource;destination->GetResource(&resource);
    uiSeparationResourceWrite(resource.Get());
}
void uiSeparationUnknownWrite(){if(!internal && enabled && (eyes[0].layer.ready()||eyes[1].layer.ready()))decline("untracked draw or command list");}
void uiSeparationFrameBoundary(){for(auto& e:eyes){e.layer.frameBoundary();e.complete=false;e.uiDraws=e.lateDraws=0;}}
bool uiSeparationFailed(){return failed;}
void uiSeparationEvaluated(){if(!usedNoted){usedNoted=true;Log::get().note("UI separation: DLSS evaluated world-only colour; current target sprites composited after temporal reconstruction.");}}
void uiSeparationShutdown(){for(auto& e:eyes)e=Eye{};toneInput.Reset();toneTarget.Reset();toneDepth.Reset();active=tone=-1;enabled=failed=noted=usedNoted=internal=false;}
}
