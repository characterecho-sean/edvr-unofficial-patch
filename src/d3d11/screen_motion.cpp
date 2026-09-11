#include "screen_motion.h"
#include <d3d11.h>
#include <wrl/client.h>
#include "binding_shadow.h"
#include "shader_swap.h"
#include "vscreen.h"
#include "../common/config.h"
#include "../common/temporal_mode.h"
#include "../common/log.h"

namespace edvr { namespace {
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
    bool enabled=false,failed=false,noted=false;
    bool seen=false;
    unsigned lastScreen=0;
    unsigned frame=0,sourceFrame=~0u,sourceWrite=0,sourcePrevious=~0u;
    Ptr<ID3D11Buffer> camera[2];
    Ptr<ID3D11Texture2D> depth;
    Ptr<ID3D11ShaderResourceView> depthSrv;
    Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11BlendState> blend;
    Ptr<ID3D11DepthStencilState> ds;
    Screen eyes[2];
} g;

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
bool prepare(ID3D11DeviceContext* ctx,ID3D11Device* dev,Screen& e,unsigned w,unsigned h) {
    if(!g.ps) {
        g.ps.Attach(shaderSwapCompilePs(ctx,kScreenMotionPs,sizeof(kScreenMotionPs)-1,"main","screen motion",nullptr,"screen motion"));
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
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=32;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
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
    if(on!=g.enabled){g=State{};g.enabled=on;}
}
bool screenMotionRecognize() {
    bool matched=g.enabled && !g.failed && bindingShaderHash(BindSlot::Vs)==0x5C36AF051B98B9F1ull &&
        bindingShaderHash(BindSlot::Ps)==0xCFE84157BC76E921ull;
    if(matched){g.seen=true;g.lastScreen=g.frame;}return matched;
}
void screenMotionSource(ID3D11DeviceContext* ctx,unsigned w,unsigned h) {
    if(!g.enabled || g.failed || !g.seen || g.frame-g.lastScreen>2 || g.sourceFrame==g.frame || !ctx || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    uint64_t vs=bindingShaderHash(BindSlot::Vs);
    if(vs!=0xACE405F428C17EF6ull && vs!=0x4435F2E50020E7F3ull)return;
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
        g.depthSrv.Reset();D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=fmt;
        sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
        if(FAILED(dev->CreateShaderResourceView(tex.Get(),&sd,&g.depthSrv)))return;
        g.depth=tex;g.sourceFrame=~0u;
    }
    unsigned next=1-g.sourceWrite;
    if(!copyCb(ctx,dev.Get(),1,g.camera[next],276*16))return;
    g.sourcePrevious=g.sourceFrame;g.sourceFrame=g.frame;g.sourceWrite=next;
}
void screenMotionDraw(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned count,unsigned instances,
                      unsigned start,int base,unsigned startInstance,const float* curve) {
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
    if(!prepare(ctx,dev.Get(),e,td.Width,td.Height)){g.failed=true;Log::get().note("screen motion: resource creation failed; original temporal inputs retained.");return;}
    unsigned next=1-e.write;
    if(!copyCb(ctx,dev.Get(),0,e.model[next],12*16) || !copyCb(ctx,dev.Get(),1,e.camera[next],274*16))return;
    Ptr<ID3D11Buffer> vb;UINT stride=0,offset=0;ctx->IAGetVertexBuffers(1,1,&vb,&stride,&offset);if(!vb || stride<8)return;
    D3D11_BUFFER_DESC vd{};vb->GetDesc(&vd);uint64_t at=uint64_t(offset)+uint64_t(startInstance)*stride;if(at+8>vd.ByteWidth)return;
    D3D11_BOX box{UINT(at),0,0,UINT(at+8),1,1};ctx->CopySubresourceRegion(e.sizes[next].Get(),0,0,0,0,vb.Get(),0,&box);
    bool consecutive=e.frame+1==g.frame && g.sourcePrevious+1==g.frame && e.model[e.write] && e.camera[e.write] && g.camera[1-g.sourceWrite];
    const float zero[4]{};ctx->ClearRenderTargetView(e.rtv.Get(),zero);e.written=false;
    if(consecutive) {
        float data[8]={e.shape[0],e.shape[1],e.shape[2],e.shape[3],float(e.width),float(e.height),0,0};ctx->UpdateSubresource(e.settings.Get(),0,nullptr,data,0,0);
        ID3D11RenderTargetView* savedRt[8]{};Ptr<ID3D11DepthStencilView> savedDepth;ctx->OMGetRenderTargets(8,savedRt,&savedDepth);
        Ptr<ID3D11BlendState> savedBlend;FLOAT factors[4];UINT mask;ctx->OMGetBlendState(&savedBlend,factors,&mask);
        Ptr<ID3D11DepthStencilState> savedDs;UINT stencil;ctx->OMGetDepthStencilState(&savedDs,&stencil);
        Ptr<ID3D11PixelShader> savedPs;ID3D11ClassInstance* classes[256]{};UINT nc=256;ctx->PSGetShader(&savedPs,classes,&nc);
        ID3D11Buffer* savedCb[5]{};ctx->PSGetConstantBuffers(2,5,savedCb);
        ID3D11ShaderResourceView* savedSrv[2]{};ctx->PSGetShaderResources(8,2,savedSrv);
        ID3D11Buffer* cb[5]={g.camera[g.sourceWrite].Get(),g.camera[1-g.sourceWrite].Get(),e.model[e.write].Get(),e.camera[e.write].Get(),e.settings.Get()};
        ID3D11ShaderResourceView* srvs[2]={g.depthSrv.Get(),e.sizeSrv[e.write].Get()};
        vScreenSetRenderTargetsRaw(ctx,1,e.rtv.GetAddressOf(),nullptr);ctx->OMSetBlendState(g.blend.Get(),nullptr,~0u);ctx->OMSetDepthStencilState(g.ds.Get(),0);
        ctx->PSSetConstantBuffers(2,5,cb);ctx->PSSetShaderResources(8,2,srvs);ctx->PSSetShader(g.ps.Get(),nullptr,0);
        draw(ctx,count,instances,start,base,startInstance);
        ID3D11ShaderResourceView* nulls[2]{};ctx->PSSetShaderResources(8,2,nulls);
        vScreenSetRenderTargetsRaw(ctx,8,savedRt,savedDepth.Get());ctx->OMSetBlendState(savedBlend.Get(),factors,mask);ctx->OMSetDepthStencilState(savedDs.Get(),stencil);
        ctx->PSSetConstantBuffers(2,5,savedCb);ctx->PSSetShaderResources(8,2,savedSrv);ctx->PSSetShader(savedPs.Get(),classes,nc);
        for(auto* p:savedRt)if(p)p->Release();for(auto* p:savedCb)if(p)p->Release();for(auto* p:savedSrv)if(p)p->Release();for(UINT i=0;i<nc;++i)classes[i]->Release();
        e.written=true;
        if(!g.noted){g.noted=true;Log::get().note("screen motion: source camera/depth projected through the actual screen mesh at %ux%u per eye; GPU-only history, no source colour copies.",e.width,e.height);}
    }
    for(int i=0;i<4;++i)e.shape[i]=curve?curve[i]:0;
    e.frame=g.frame;e.write=next;e.target=res;
}
ID3D11ShaderResourceView* screenMotionView(int eye,unsigned w,unsigned h) {
    if(!g.enabled || eye<0 || eye>1)return nullptr;Screen& e=g.eyes[eye];
    return e.written && e.frame==g.frame && e.width==w && e.height==h?e.srv.Get():nullptr;
}
void screenMotionFrameBoundary(){
    ++g.frame;
    if(g.seen && g.frame-g.lastScreen>120){bool on=g.enabled;g=State{};g.enabled=on;}
}
void screenMotionShutdown(){g=State{};}
}
