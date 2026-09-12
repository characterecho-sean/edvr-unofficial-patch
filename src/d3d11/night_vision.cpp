#include "night_vision.h"
#include "night_vision_shader.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include "binding_shadow.h"
#include "shader_swap.h"
#include "vscreen.h"
#include "../common/config.h"
#include "../common/log.h"
namespace edvr { namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
bool enabled=true,configured=false;
float brightness=2.0f;
struct State {
    Ptr<ID3D11PixelShader> shader,saved;
    Ptr<ID3D11Buffer> control,savedControl;
    float uploadedBrightness=-1;
    Ptr<ID3D11ComputeShader> classify;
    Ptr<ID3D11Texture2D> exterior;
    Ptr<ID3D11ShaderResourceView> exteriorView,savedExterior;
    Ptr<ID3D11UnorderedAccessView> exteriorUav;
    struct Depth {Ptr<ID3D11Resource> resource;Ptr<ID3D11ShaderResourceView> stencil;} depths[2];
    UINT width=0,height=0,nextDepth=0;
    Ptr<ID3D11BlendState> blend,savedBlend;
    FLOAT blendFactors[4]{};
    UINT sampleMask=~0u;
    ID3D11ClassInstance* classes[256]{};
    UINT count=0;
    bool failed=false,engaged=false,noted=false;
} state;
bool exteriorMask(ID3D11DeviceContext* ctx,UINT w,UINT h){
    Ptr<ID3D11DepthStencilState> stencil;UINT reference;ctx->OMGetDepthStencilState(&stencil,&reference);if(!stencil)return false;
    D3D11_DEPTH_STENCIL_DESC contract{};stencil->GetDesc(&contract);
    if(contract.DepthEnable || !contract.StencilEnable || contract.StencilReadMask!=128 || contract.StencilWriteMask!=4 || (reference&128) ||
       contract.FrontFace.StencilFunc!=D3D11_COMPARISON_EQUAL ||
       contract.FrontFace.StencilPassOp!=D3D11_STENCIL_OP_REPLACE)return false;
    Ptr<ID3D11DepthStencilView> dsv;ctx->OMGetRenderTargets(0,nullptr,&dsv);if(!dsv)return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};dsv->GetDesc(&vd);
    if(vd.Format!=DXGI_FORMAT_D32_FLOAT_S8X24_UINT || vd.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice)return false;
    Ptr<ID3D11Resource> resource;dsv->GetResource(&resource);Ptr<ID3D11Texture2D> texture;
    if(FAILED(resource.As(&texture)))return false;D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
    if(td.Format!=DXGI_FORMAT_R32G8X24_TYPELESS || !(td.BindFlags&D3D11_BIND_SHADER_RESOURCE) || td.ArraySize!=1 ||
       td.SampleDesc.Count!=1 || td.Width!=w || td.Height!=h || uint64_t(w)*h>16*1024*1024)return false;
    // Do not disturb an OM UAV or predicated compute dispatch.
    ID3D11UnorderedAccessView* om[8]{};ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,om);
    bool busy=false;for(auto* p:om)if(p){busy=true;p->Release();}if(busy)return false;
    Ptr<ID3D11Predicate> predicate;BOOL pred;ctx->GetPredication(&predicate,&pred);if(predicate)return false;
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
    State::Depth* cached=nullptr;for(auto& entry:state.depths)if(entry.resource==resource)cached=&entry;
    if(!cached){
        cached=&state.depths[state.nextDepth++%2];*cached=State::Depth{};
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
        if(FAILED(dev->CreateShaderResourceView(resource.Get(),&sd,&cached->stencil)))return false;
        cached->resource=resource;
    }
    if(!state.classify)state.classify.Attach(shaderSwapCompileCs(ctx,kNightExteriorCs,sizeof(kNightExteriorCs)-1,"main","night exterior",nullptr,"night vision exterior"));
    if(!state.classify){state.failed=true;return false;}
    if(!state.exterior || state.width!=w || state.height!=h){
        state.exterior.Reset();state.exteriorView.Reset();state.exteriorUav.Reset();state.width=state.height=0;
        D3D11_TEXTURE2D_DESC out{};out.Width=w;out.Height=h;out.MipLevels=out.ArraySize=out.SampleDesc.Count=1;
        out.Format=DXGI_FORMAT_R8_UNORM;out.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        if(FAILED(dev->CreateTexture2D(&out,nullptr,&state.exterior)) ||
           FAILED(dev->CreateShaderResourceView(state.exterior.Get(),nullptr,&state.exteriorView)) ||
           FAILED(dev->CreateUnorderedAccessView(state.exterior.Get(),nullptr,&state.exteriorUav)))return false;
        state.width=w;state.height=h;
    }
    Ptr<ID3D11ComputeShader> saved;ID3D11ClassInstance* classes[256]{};UINT count=256;ctx->CSGetShader(&saved,classes,&count);
    Ptr<ID3D11ShaderResourceView> srv;ctx->CSGetShaderResources(0,1,&srv);
    Ptr<ID3D11UnorderedAccessView> uav;ctx->CSGetUnorderedAccessViews(0,1,&uav);
    ID3D11RenderTargetView* rt[8]{};ctx->OMGetRenderTargets(8,rt,nullptr);
    vScreenSetRenderTargetsRaw(ctx,0,nullptr,nullptr);
    ctx->CSSetShader(state.classify.Get(),nullptr,0);ctx->CSSetShaderResources(0,1,cached->stencil.GetAddressOf());
    ctx->CSSetUnorderedAccessViews(0,1,state.exteriorUav.GetAddressOf(),nullptr);ctx->Dispatch((w+7)/8,(h+7)/8,1);
    ID3D11UnorderedAccessView* noneUav=nullptr;ID3D11ShaderResourceView* noneSrv=nullptr;
    ctx->CSSetUnorderedAccessViews(0,1,&noneUav,nullptr);ctx->CSSetShaderResources(0,1,&noneSrv);
    vScreenSetRenderTargetsRaw(ctx,8,rt,dsv.Get());for(auto* p:rt)if(p)p->Release();
    ctx->CSSetShader(saved.Get(),classes,count);ctx->CSSetShaderResources(0,1,srv.GetAddressOf());
    UINT keep=~0u;ctx->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),&keep);
    for(UINT i=0;i<count;++i)classes[i]->Release();
    return true;
}
}
void nightVisionConfigure(Config& cfg){
    bool on=cfg.getBool("fix.night_vision_stability",true);
    float gain=cfg.getFloat("advanced.night_vision_brightness",2.0f);
    if(!std::isfinite(gain))gain=2.0f;
    gain=(std::max)(1.0f,(std::min)(gain,16.0f));
    if(!configured || on!=enabled || gain!=brightness)Log::get().note("night vision stability: %s; depth-geometry outlines, neutral terrain brightness up to %.2fx, cockpit/body exclusion, no surface fill, radial pulse. AA-independent, live A/B.",on?"on":"off (original shader)",gain);
    configured=true;enabled=on;brightness=gain;
}
bool nightVisionMatches(char kind,uint32_t count,uint32_t instances){
    return enabled && !state.failed && kind=='X' && count==240 && instances==1 &&
        bindingShaderHash(BindSlot::Vs)==0xFCF7BD2896751D96ull &&
        bindingShaderHash(BindSlot::Ps)==0xF786D34B5E118D5Eull;
}
void nightVisionBegin(ID3D11DeviceContext* ctx){
    if(!ctx || !enabled || state.failed || state.engaged || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    // No readbacks or scene copies. Reject a
    // changed resource contract before compiling/binding the replacement.
    Ptr<ID3D11Buffer> camera,settings;ctx->PSGetConstantBuffers(1,1,&camera);ctx->PSGetConstantBuffers(2,1,&settings);
    if(!camera || !settings)return;
    D3D11_BUFFER_DESC cd{},nd{};camera->GetDesc(&cd);settings->GetDesc(&nd);
    if(cd.ByteWidth<333*16 || nd.ByteWidth!=12*16)return;
    UINT w=0,h=0;
    for(UINT i=1;i<=2;++i){
        Ptr<ID3D11ShaderResourceView> srv;ctx->PSGetShaderResources(i,1,&srv);if(!srv)return;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};srv->GetDesc(&sd);
        if(sd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || sd.Texture2D.MostDetailedMip ||
           sd.Format!=(i==1?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R10G10B10A2_UNORM))return;
        Ptr<ID3D11Resource> res;srv->GetResource(&res);Ptr<ID3D11Texture2D> tex;if(FAILED(res.As(&tex)))return;
        D3D11_TEXTURE2D_DESC td{};tex->GetDesc(&td);
        if(td.ArraySize!=1 || td.SampleDesc.Count!=1)return;
        if(i==1){w=td.Width;h=td.Height;}else if(w!=td.Width || h!=td.Height)return;
    }
    D3D11_VIEWPORT vp{};UINT views=1;ctx->RSGetViewports(&views,&vp);
    if(views!=1 || vp.TopLeftX || vp.TopLeftY || vp.Width!=w || vp.Height!=h)return;
    // Dual-source blending needs a single target 0. Refuse a changed MRT
    // or blend contract rather than sending a multiplier into another RT.
    ID3D11RenderTargetView* targets[8]{};ctx->OMGetRenderTargets(8,targets,nullptr);
    bool single=targets[0]!=nullptr;
    for(UINT i=1;i<8;++i)if(targets[i])single=false;
    bool supported=false;
    if(single){
        D3D11_RENDER_TARGET_VIEW_DESC rd{};targets[0]->GetDesc(&rd);
        Ptr<ID3D11Resource> resource;targets[0]->GetResource(&resource);
        Ptr<ID3D11Texture2D> target;
        if(SUCCEEDED(resource.As(&target))){
            D3D11_TEXTURE2D_DESC td{};target->GetDesc(&td);
            supported=rd.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D && !rd.Texture2D.MipSlice &&
                td.Width==w && td.Height==h && td.ArraySize==1 && td.SampleDesc.Count==1 &&
                (rd.Format==DXGI_FORMAT_R11G11B10_FLOAT || rd.Format==DXGI_FORMAT_R16G16B16A16_FLOAT ||
                 rd.Format==DXGI_FORMAT_R32G32B32A32_FLOAT);
        }
    }
    for(auto* target:targets)if(target)target->Release();
    if(!supported)return;
    Ptr<ID3D11BlendState> original;FLOAT factors[4];UINT mask;
    ctx->OMGetBlendState(&original,factors,&mask);if(!original)return;
    D3D11_BLEND_DESC bd{};original->GetDesc(&bd);const auto& rt=bd.RenderTarget[0];
    if(bd.AlphaToCoverageEnable || !rt.BlendEnable || rt.SrcBlend!=D3D11_BLEND_ONE ||
       rt.DestBlend!=D3D11_BLEND_INV_SRC_ALPHA || rt.BlendOp!=D3D11_BLEND_OP_ADD ||
       rt.RenderTargetWriteMask!=7)return;
    if(!state.shader){
        state.shader.Attach(shaderSwapCompilePs(ctx,kNightVisionPs,sizeof(kNightVisionPs)-1,"main","night_vision",nullptr,"night vision stability"));
        if(!state.shader){state.failed=true;return;}
    }
    if(!state.blend){
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        D3D11_BLEND_DESC replacement{};replacement.RenderTarget[0]=rt;
        replacement.RenderTarget[0].DestBlend=D3D11_BLEND_SRC1_COLOR;
        if(FAILED(dev->CreateBlendState(&replacement,&state.blend))){
            state.failed=true;Log::get().note("night vision stability: blend creation failed; retaining original draw.");return;
        }
    }
    if(!exteriorMask(ctx,w,h))return;
    if(!state.control){
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        D3D11_BUFFER_DESC controlDesc{};controlDesc.ByteWidth=16;controlDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(dev->CreateBuffer(&controlDesc,nullptr,&state.control))){
            state.failed=true;Log::get().note("night vision stability: control buffer creation failed; retaining original draw.");return;
        }
    }
    // Upload only on creation or a live setting change. Keep the game's
    // constants intact; this private slot is restored after the draw.
    if(state.uploadedBrightness!=brightness){
        const float value[4]={brightness,0,0,0};ctx->UpdateSubresource(state.control.Get(),0,nullptr,value,0,0);
        state.uploadedBrightness=brightness;
    }
    ctx->PSGetConstantBuffers(3,1,&state.savedControl);ctx->PSSetConstantBuffers(3,1,state.control.GetAddressOf());
    ctx->PSGetShaderResources(5,1,&state.savedExterior);ctx->PSSetShaderResources(5,1,state.exteriorView.GetAddressOf());
    state.count=256;ctx->PSGetShader(&state.saved,state.classes,&state.count);
    state.savedBlend=original;for(UINT i=0;i<4;++i)state.blendFactors[i]=factors[i];state.sampleMask=mask;
    ctx->OMSetBlendState(state.blend.Get(),factors,mask);
    ctx->PSSetShader(state.shader.Get(),nullptr,0);state.engaged=true;
    if(!state.noted){state.noted=true;Log::get().note("night vision stability: engaged at %ux%u; geometry contours and neutral terrain gain %.2f, cockpit/body and contour-footprint exclusion, R8 GPU mask; original PS/blend restored after each matched draw.",w,h,brightness);}
}
void nightVisionEnd(ID3D11DeviceContext* ctx){
    if(!state.engaged)return;
    ctx->PSSetConstantBuffers(3,1,state.savedControl.GetAddressOf());state.savedControl.Reset();
    ctx->PSSetShaderResources(5,1,state.savedExterior.GetAddressOf());state.savedExterior.Reset();
    ctx->PSSetShader(state.saved.Get(),state.classes,state.count);state.saved.Reset();
    ctx->OMSetBlendState(state.savedBlend.Get(),state.blendFactors,state.sampleMask);state.savedBlend.Reset();
    for(UINT i=0;i<state.count;++i){state.classes[i]->Release();state.classes[i]=nullptr;}
    state.count=0;state.engaged=false;
}
void nightVisionShutdown(){state=State{};}
}
