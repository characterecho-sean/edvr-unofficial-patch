#include "night_vision.h"
#include "night_vision_shader.h"
#include <d3d11.h>
#include <wrl/client.h>
#include "binding_shadow.h"
#include "shader_swap.h"
#include "../common/config.h"
#include "../common/log.h"
namespace edvr { namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
bool enabled=true,configured=false;
struct State {
    Ptr<ID3D11PixelShader> shader,saved;
    ID3D11ClassInstance* classes[256]{};
    UINT count=0;
    bool failed=false,engaged=false,noted=false;
} state;
}
void nightVisionConfigure(Config& cfg){
    bool on=cfg.getBool("fix.night_vision_stability",true);
    if(!configured || on!=enabled)Log::get().note("night vision stability: %s; depth-geometry outlines, no surface fill, radial pulse. AA-independent, live A/B.",on?"on":"off (original shader)");
    configured=true;enabled=on;
}
bool nightVisionMatches(char kind,uint32_t count,uint32_t instances){
    return enabled && !state.failed && kind=='X' && count==240 && instances==1 &&
        bindingShaderHash(BindSlot::Vs)==0xFCF7BD2896751D96ull &&
        bindingShaderHash(BindSlot::Ps)==0xF786D34B5E118D5Eull;
}
void nightVisionBegin(ID3D11DeviceContext* ctx){
    if(!ctx || !enabled || state.failed || state.engaged || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    // No readbacks, copies, replacement surfaces or extra draws. Reject a
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
    if(!state.shader){
        state.shader.Attach(shaderSwapCompilePs(ctx,kNightVisionPs,sizeof(kNightVisionPs)-1,"main","night_vision",nullptr,"night vision stability"));
        if(!state.shader){state.failed=true;return;}
    }
    state.count=256;ctx->PSGetShader(&state.saved,state.classes,&state.count);
    ctx->PSSetShader(state.shader.Get(),nullptr,0);state.engaged=true;
    if(!state.noted){state.noted=true;Log::get().note("night vision stability: engaged at %ux%u; geometry contours without surface fill; original PS restored after each matched draw.",w,h);}
}
void nightVisionEnd(ID3D11DeviceContext* ctx){
    if(!state.engaged)return;
    ctx->PSSetShader(state.saved.Get(),state.classes,state.count);state.saved.Reset();
    for(UINT i=0;i<state.count;++i){state.classes[i]->Release();state.classes[i]=nullptr;}
    state.count=0;state.engaged=false;
}
void nightVisionShutdown(){state=State{};}
}
