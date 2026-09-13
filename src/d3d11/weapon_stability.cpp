#include "weapon_stability.h"
#include "weapon_motion.h"
#include "mesh_motion.h"
#include <d3d11.h>
#include <wrl/client.h>
#include "binding_shadow.h"
#include "shader_swap.h"
#include "../common/config.h"
#include "../common/log.h"
namespace edvr { namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
// Configuration survives release of inactive on-foot GPU resources.
bool g_enabled=true,g_configured=false;
struct State {
    unsigned frame=0,lastScreen=0,prepared=~0u,sourceFrame=~0u,bytes=0,pendingFrame=0,nextReport=0;
    bool seen=false,failed=false,pending=false;
    Ptr<ID3D11Buffer> pool,bones,camera,fixed,anchor,stage;
    Ptr<ID3D11ShaderResourceView> fixedSrv,poolView,bonesView;
    Ptr<ID3D11UnorderedAccessView> fixedUav,anchorUav;
    Ptr<ID3D11ComputeShader> find,apply;
    Ptr<ID3D11ComputeShader> emitterShader;
    Ptr<ID3D11Buffer> emitterOutput,emitterCb;
    Ptr<ID3D11UnorderedAccessView> emitterUav;
    Ptr<ID3D11ComputeShader> lightShader;
    Ptr<ID3D11Buffer> lightVertices;
    Ptr<ID3D11UnorderedAccessView> lightUav;
    unsigned lightBytes=0;
} g;
bool family(uint64_t vs) {
    return vs==0xF516BF0201303B87ull || vs==0x8B589D25B2A0ADDCull ||
           vs==0x7B0DC42D383F694Cull || vs==0x114AF608F86D9ED8ull ||
           // Tool material surfaces and rifle optics share the same source
           // attachment records. Missing these passes separated their pieces
           // from the corrected opaque mesh (18:21 captures). The late GUI
           // uses an already camera-relative placement and must stay outside.
           vs==0xAACFDCF2FB9AD809ull || vs==0x34CCFAAB1EAD90BEull ||
           vs==0x174E8D76363BE337ull || vs==0x025B4B9FF54622EDull ||
           vs==0x7F9B650EC1A1E570ull ||
           // The small emissive skinned surface in draw 387 of 04:13:00.
           // Its instance 135 shares the arms' attachment origin. Unlike
           // the late GUI, its original VS subtracts camera[275] from t33,
           // so it needs the same correction as the surrounding geometry.
           vs==0x88DCF1164C640EC3ull;
}
bool generate(ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* poolView,ID3D11ShaderResourceView* bonesView,
              ID3D11Buffer* pool,ID3D11Buffer* bones,ID3D11Buffer* camera,unsigned bytes) {
    if(g.prepared==g.frame && g.pool.Get()==pool && g.bones.Get()==bones && g.camera.Get()==camera)return true;
    Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
    if(!g.find) {
        g.find.Attach(shaderSwapCompileCs(ctx,kWeaponStabilityCs,sizeof(kWeaponStabilityCs)-1,"findAnchor","weapon anchor",nullptr,"weapon stability"));
        g.apply.Attach(shaderSwapCompileCs(ctx,kWeaponStabilityCs,sizeof(kWeaponStabilityCs)-1,"applyAnchor","weapon attach",nullptr,"weapon stability"));
        if(!g.find || !g.apply)return false;
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=48;bd.StructureByteStride=16;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        if(FAILED(dev->CreateBuffer(&bd,nullptr,&g.anchor)) || FAILED(dev->CreateUnorderedAccessView(g.anchor.Get(),nullptr,&g.anchorUav)))return false;
        bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0;bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(dev->CreateBuffer(&bd,nullptr,&g.stage)))return false;
    }
    if(g.bytes!=bytes) {
        g.fixed.Reset();g.fixedSrv.Reset();g.fixedUav.Reset();
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=bytes;bd.StructureByteStride=336;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        if(FAILED(dev->CreateBuffer(&bd,nullptr,&g.fixed)) || FAILED(dev->CreateShaderResourceView(g.fixed.Get(),nullptr,&g.fixedSrv)) ||
           FAILED(dev->CreateUnorderedAccessView(g.fixed.Get(),nullptr,&g.fixedUav)))return false;
        g.bytes=bytes;
    }
    Ptr<ID3D11ComputeShader> saved;ID3D11ClassInstance* classes[256]{};UINT nc=256;ctx->CSGetShader(&saved,classes,&nc);
    ID3D11ShaderResourceView* srvs[2]{};ctx->CSGetShaderResources(0,2,srvs);
    ID3D11UnorderedAccessView* uavs[2]{};ctx->CSGetUnorderedAccessViews(0,2,uavs);
    Ptr<ID3D11Buffer> cb;ctx->CSGetConstantBuffers(0,1,&cb);
    ID3D11ShaderResourceView* in[2]={poolView,bonesView};ID3D11UnorderedAccessView* out[2]={g.fixedUav.Get(),g.anchorUav.Get()};
    ctx->CSSetShaderResources(0,2,in);ctx->CSSetConstantBuffers(0,1,&camera);ctx->CSSetUnorderedAccessViews(0,2,out,nullptr);
    ctx->CSSetShader(g.find.Get(),nullptr,0);ctx->Dispatch(1,1,1);
    ctx->CSSetShader(g.apply.Get(),nullptr,0);ctx->Dispatch((bytes/336+63)/64,1,1);
    ID3D11UnorderedAccessView* nullUav[2]{};ctx->CSSetUnorderedAccessViews(0,2,nullUav,nullptr);
    ID3D11ShaderResourceView* nullSrv[2]{};ctx->CSSetShaderResources(0,2,nullSrv);
    ctx->CSSetShaderResources(0,2,srvs);ctx->CSSetUnorderedAccessViews(0,2,uavs,nullptr);ctx->CSSetConstantBuffers(0,1,cb.GetAddressOf());ctx->CSSetShader(saved.Get(),classes,nc);
    for(auto* p:srvs)if(p)p->Release();for(auto* p:uavs)if(p)p->Release();for(UINT i=0;i<nc;++i)classes[i]->Release();
    if(!g.pending && g.frame>=g.nextReport){ctx->CopyResource(g.stage.Get(),g.anchor.Get());g.pending=true;g.pendingFrame=g.frame;g.nextReport=g.frame+1800;}
    g.pool=pool;g.bones=bones;g.camera=camera;g.poolView=poolView;g.bonesView=bonesView;
    g.prepared=g.sourceFrame=g.frame;return true;
}
bool particleDraw(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned count,unsigned instances,unsigned start,int base,unsigned startInstance) {
    // Only the verified local particle PS/VS pair. The shared vertex shader
    // also renders world effects; the GPU validates projection and proximity.
    if(bindingShaderHash(BindSlot::Ps)!=0x3789CA2062E196FBull || g.sourceFrame!=g.frame || !g.poolView || !g.bonesView)return false;
    Ptr<ID3D11Buffer> model,camera;ctx->VSGetConstantBuffers(0,1,&model);ctx->VSGetConstantBuffers(1,1,&camera);
    if(!model || !camera)return false;
    D3D11_BUFFER_DESC md{},cd{};model->GetDesc(&md);camera->GetDesc(&cd);
    if(md.ByteWidth!=13*16 || cd.ByteWidth<276*16)return false;
    // The camera buffer is rewritten between world and viewmodel draws.
    // Refresh the existing GPU anchor with this draw's actual camera rows.
    if(!generate(ctx,g.poolView.Get(),g.bonesView.Get(),g.pool.Get(),g.bones.Get(),camera.Get(),g.bytes))return false;
    if(!g.emitterShader)g.emitterShader.Attach(shaderSwapCompileCs(ctx,kWeaponStabilityCs,sizeof(kWeaponStabilityCs)-1,"applyEmitter","weapon emitter",nullptr,"weapon stability"));
    if(!g.emitterShader)return false;
    if(!g.emitterCb) {
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=13*16;bd.StructureByteStride=16;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        g.emitterOutput.Reset();g.emitterUav.Reset();
        if(FAILED(dev->CreateBuffer(&bd,nullptr,&g.emitterOutput)) || FAILED(dev->CreateUnorderedAccessView(g.emitterOutput.Get(),nullptr,&g.emitterUav)))return false;
        bd.StructureByteStride=bd.MiscFlags=0;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(dev->CreateBuffer(&bd,nullptr,&g.emitterCb)))return false;
    }
    Ptr<ID3D11ComputeShader> saved;ID3D11ClassInstance* classes[256]{};UINT nc=256;ctx->CSGetShader(&saved,classes,&nc);
    ID3D11Buffer* cbs[2]{};ctx->CSGetConstantBuffers(0,2,cbs);
    ID3D11UnorderedAccessView* uavs[2]{};ctx->CSGetUnorderedAccessViews(1,2,uavs);
    Ptr<ID3D11ShaderResourceView> poolSrv;ctx->CSGetShaderResources(0,1,&poolSrv);
    ID3D11Buffer* inputs[2]={camera.Get(),model.Get()};ID3D11UnorderedAccessView* outputs[2]={g.anchorUav.Get(),g.emitterUav.Get()};
    ctx->CSSetConstantBuffers(0,2,inputs);ctx->CSSetUnorderedAccessViews(1,2,outputs,nullptr);
    ctx->CSSetShaderResources(0,1,g.poolView.GetAddressOf());
    ctx->CSSetShader(g.emitterShader.Get(),nullptr,0);ctx->Dispatch(1,1,1);
    ID3D11UnorderedAccessView* nulls[2]{};ctx->CSSetUnorderedAccessViews(1,2,nulls,nullptr);
    ctx->CopyResource(g.emitterCb.Get(),g.emitterOutput.Get());
    ctx->CSSetConstantBuffers(0,2,cbs);ctx->CSSetUnorderedAccessViews(1,2,uavs,nullptr);ctx->CSSetShader(saved.Get(),classes,nc);
    ctx->CSSetShaderResources(0,1,poolSrv.GetAddressOf());
    for(auto* p:cbs)if(p)p->Release();for(auto* p:uavs)if(p)p->Release();for(UINT i=0;i<nc;++i)classes[i]->Release();
    ctx->VSSetConstantBuffers(0,1,g.emitterCb.GetAddressOf());draw(ctx,count,instances,start,base,startInstance);ctx->VSSetConstantBuffers(0,1,model.GetAddressOf());return true;
}
bool lightDraw(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned count,unsigned instances,unsigned start,int base,unsigned startInstance) {
    if(bindingShaderHash(BindSlot::Ps)!=0x81812EF97FB4A361ull || count!=14 || start || base ||
       g.sourceFrame!=g.frame || !g.poolView || !g.bonesView || !g.camera)return false;
    Ptr<ID3D11Buffer> vertices,lightCamera;UINT stride=0,offset=0;
    ctx->IAGetVertexBuffers(1,1,&vertices,&stride,&offset);ctx->VSGetConstantBuffers(2,1,&lightCamera);
    if(!vertices || !lightCamera || stride!=32 || offset%4)return false;
    D3D11_BUFFER_DESC vd{},cd{};vertices->GetDesc(&vd);lightCamera->GetDesc(&cd);
    const uint64_t begin=uint64_t(offset)+uint64_t(startInstance)*32,bytes=uint64_t(instances)*32;
    if(cd.ByteWidth<14*16 || instances>4096 || begin+bytes>vd.ByteWidth)return false;
    if(!generate(ctx,g.poolView.Get(),g.bonesView.Get(),g.pool.Get(),g.bones.Get(),g.camera.Get(),g.bytes))return false;
    if(!g.lightShader)g.lightShader.Attach(shaderSwapCompileCs(ctx,kWeaponStabilityCs,sizeof(kWeaponStabilityCs)-1,"applyLights","weapon lights",nullptr,"weapon stability"));
    if(!g.lightShader)return false;
    if(g.lightBytes<bytes) {
        g.lightVertices.Reset();g.lightUav.Reset();g.lightBytes=0;
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        // Batches alternate 512 and roughly 200 lights. Grow capacity,
        // never allocate again just because the next batch is smaller.
        UINT capacity=64*32;while(capacity<bytes)capacity*=2;
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=capacity;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_UNORDERED_ACCESS;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=DXGI_FORMAT_R32_TYPELESS;ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=capacity/4;ud.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
        if(FAILED(dev->CreateBuffer(&bd,nullptr,&g.lightVertices)) || FAILED(dev->CreateUnorderedAccessView(g.lightVertices.Get(),&ud,&g.lightUav)))return false;
        g.lightBytes=capacity;
    }
    D3D11_BOX box{UINT(begin),0,0,UINT(begin+bytes),1,1};ctx->CopySubresourceRegion(g.lightVertices.Get(),0,0,0,0,vertices.Get(),0,&box);
    Ptr<ID3D11ComputeShader> saved;ID3D11ClassInstance* classes[256]{};UINT nc=256;ctx->CSGetShader(&saved,classes,&nc);
    ID3D11Buffer* cbs[3]{};ctx->CSGetConstantBuffers(0,3,cbs);
    ID3D11UnorderedAccessView* uavs[3]{};ctx->CSGetUnorderedAccessViews(1,3,uavs);
    ID3D11Buffer* inputs[3]={g.camera.Get(),cbs[1],lightCamera.Get()};ID3D11UnorderedAccessView* outputs[3]={g.anchorUav.Get(),uavs[1],g.lightUav.Get()};
    ctx->CSSetConstantBuffers(0,3,inputs);ctx->CSSetUnorderedAccessViews(1,3,outputs,nullptr);
    ctx->CSSetShader(g.lightShader.Get(),nullptr,0);ctx->Dispatch((instances+63)/64,1,1);
    ID3D11UnorderedAccessView* nulls[3]{};ctx->CSSetUnorderedAccessViews(1,3,nulls,nullptr);
    ctx->CSSetConstantBuffers(0,3,cbs);ctx->CSSetUnorderedAccessViews(1,3,uavs,nullptr);ctx->CSSetShader(saved.Get(),classes,nc);
    for(auto* p:cbs)if(p)p->Release();for(auto* p:uavs)if(p)p->Release();for(UINT i=0;i<nc;++i)classes[i]->Release();
    // Nonzero starts are declined at the draw gate, preserving all draw
    // arguments and SV_InstanceID while using a zero-origin private stream.
    UINT zero=0;ctx->IASetVertexBuffers(1,1,g.lightVertices.GetAddressOf(),&stride,&zero);
    draw(ctx,count,instances,start,base,startInstance);ctx->IASetVertexBuffers(1,1,vertices.GetAddressOf(),&stride,&offset);return true;
}
}
void weaponStabilityConfigure(Config& cfg) {
    const bool enabled=cfg.getBool("fix.weapon_stability",true);
    if(g_configured && enabled==g_enabled)return;
    g_enabled=enabled;g_configured=true;
    // Keep compiled shaders and screen recognition for a live A/B. Never
    // reuse a pre-toggle pool or report a pending sample from the old mode.
    g.prepared=g.sourceFrame=~0u;g.pending=false;g.nextReport=0;
    Log::get().note("weapon stability: %s (live; independent of AA and runtime reprojection).",enabled?"on":"off");
}
void weaponStabilityObserveScreen() {
    if(bindingShaderHash(BindSlot::Vs)==0x5C36AF051B98B9F1ull && bindingShaderHash(BindSlot::Ps)==0xCFE84157BC76E921ull) {
        if(!g.seen && g_enabled)Log::get().note("weapon stability: on-foot screen observed; waiting for source attachment draws.");
        g.seen=true;g.lastScreen=g.frame;
    }
}
void weaponStabilityResourceWritten(ID3D11Resource* resource,uint64_t first,uint64_t end) {
    weaponMotionResourceWritten(resource);
    meshMotionResourceWritten(resource,first,end);
    if(!resource)g.sourceFrame=~0u;
    if(!resource || resource==g.pool.Get() || resource==g.bones.Get() || resource==g.camera.Get())g.prepared=~0u;
}
bool weaponStabilityDraw(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned count,unsigned instances,
                         unsigned start,int base,unsigned startInstance,unsigned w,unsigned h) {
    if(!g_enabled)return false;
    const uint64_t vs=bindingShaderHash(BindSlot::Vs);
    const bool particle=vs==0x9AEC596A2B036EA6ull;
    const bool light=vs==0x0357BBB2DEE43C1Full;
    if(!g.seen || g.failed || g.frame-g.lastScreen>2 || (!family(vs) && !particle && !light) || !ctx || !draw || !instances || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    ResourceInfo rt;if(!bindingResolve(bindingGet(BindSlot::Rtv0),&rt) || !rt.isTexture2D || rt.a!=w || rt.b!=h)return false;
    D3D11_VIEWPORT vp{};UINT nv=0;ctx->RSGetViewports(&nv,nullptr);if(nv!=1)return false;ctx->RSGetViewports(&nv,&vp);
    if(nv!=1 || vp.TopLeftX!=0 || vp.TopLeftY!=0 || vp.Width!=w || vp.Height!=h)return false;
    if(particle)return particleDraw(ctx,draw,count,instances,start,base,startInstance);
    if(light)return !startInstance && lightDraw(ctx,draw,count,instances,start,base,startInstance);
    Ptr<ID3D11ShaderResourceView> pv,bv;ctx->VSGetShaderResources(33,1,&pv);ctx->VSGetShaderResources(38,1,&bv);if(!pv || !bv)return false;
    Ptr<ID3D11Buffer> pool,bones,camera;Ptr<ID3D11Resource> pr,br;pv->GetResource(&pr);bv->GetResource(&br);
    if(FAILED(pr.As(&pool)) || FAILED(br.As(&bones)))return false;
    D3D11_BUFFER_DESC pd{},bd{},cd{};pool->GetDesc(&pd);bones->GetDesc(&bd);ctx->VSGetConstantBuffers(1,1,&camera);if(!camera)return false;camera->GetDesc(&cd);
    D3D11_SHADER_RESOURCE_VIEW_DESC ps{},bs{};pv->GetDesc(&ps);bv->GetDesc(&bs);
    if(pd.StructureByteStride!=336 || bd.StructureByteStride!=48 || !pd.ByteWidth || pd.ByteWidth>8*1024*1024 || pd.ByteWidth%336 || cd.ByteWidth<276*16 ||
       ps.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER || bs.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER || ps.Buffer.FirstElement || bs.Buffer.FirstElement || ps.Buffer.NumElements!=pd.ByteWidth/336)return false;
    if(!generate(ctx,pv.Get(),bv.Get(),pool.Get(),bones.Get(),camera.Get(),pd.ByteWidth)){
        g.failed=true;Log::get().note("weapon stability: resource/shader failure; original geometry retained.");return false;
    }
    ctx->VSSetShaderResources(33,1,g.fixedSrv.GetAddressOf());draw(ctx,count,instances,start,base,startInstance);
    weaponMotionDraw(ctx,draw,count,instances,start,base,startInstance);
    ctx->VSSetShaderResources(33,1,pv.GetAddressOf());return true;
}
void weaponStabilityFrameBoundary(ID3D11DeviceContext* ctx) {
    ++g.frame;
    if(g.pending && ctx && g.frame-g.pendingFrame>=3) {
        D3D11_MAPPED_SUBRESOURCE m{};HRESULT hr=ctx->Map(g.stage.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m);
        if(SUCCEEDED(hr)) {
            const auto* p=static_cast<const float*>(m.pData);
            Log::get().note("weapon stability: source attachment %s, offset %.6f %.6f %.6f m, %.0f matching roots; %u-byte private pool. Original animation and compositor timing retained.",p[3]>0?"matched":"unavailable (stock)",p[0],p[1],p[2],p[7],g.bytes);
            ctx->Unmap(g.stage.Get(),0);g.pending=false;
        } else if(g.frame-g.pendingFrame>60)g.pending=false;
    }
    if(g.seen && g.frame-g.lastScreen>120)g=State{};
}
void weaponStabilityShutdown(){g=State{};}
}
