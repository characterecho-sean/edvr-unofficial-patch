#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <cmath>
#include <vector>
// The UI layer's depth-stencil seed (src/d3d11/ui_layer_seed.h) across the
// three depth formats, every stencil value class, a scale and a jitter, and
// the reuse of one seeder: the readback must be the nearest input texel.
#include "../../src/d3d11/ui_layer_seed.h"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
static ComPtr<ID3DBlob> compile(const char*s,const char*e,const char*p){ComPtr<ID3DBlob>b,x;HRESULT h=D3DCompile(s,strlen(s),"seed_test",nullptr,nullptr,e,p,0,0,&b,&x);if(FAILED(h))throw std::runtime_error(x?std::string((char*)x->GetBufferPointer(),x->GetBufferSize()):"shader compile");return b;}
static const char* kInitVS=R"(struct V{float4 p:SV_POSITION;};V main(uint i:SV_VertexID){float2 p=i==0?float2(-1,-1):(i==1?float2(-1,3):float2(3,-1));V v;v.p=float4(p,0,1);return v;})";
static const char* kInitPS=R"(float main(float4 p:SV_POSITION):SV_Depth{float2 q=p.xy/float2(7,5);return 0.1+0.8*(0.6*q.x+0.4*q.y);})";
static ComPtr<ID3D11Texture2D> makeTex(ID3D11Device*d,UINT w,UINT h,DXGI_FORMAT f,UINT bind,D3D11_USAGE u=D3D11_USAGE_DEFAULT,UINT cpu=0){D3D11_TEXTURE2D_DESC x{};x.Width=w;x.Height=h;x.MipLevels=x.ArraySize=1;x.Format=f;x.SampleDesc.Count=1;x.BindFlags=bind;x.Usage=u;x.CPUAccessFlags=cpu;ComPtr<ID3D11Texture2D>t;if(FAILED(d->CreateTexture2D(&x,nullptr,&t)))throw std::runtime_error("CreateTexture2D");return t;}
static ComPtr<ID3D11DepthStencilView> makeDSV(ID3D11Device*d,ID3D11Resource*r,DXGI_FORMAT f){D3D11_DEPTH_STENCIL_VIEW_DESC x{};x.Format=f;x.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;ComPtr<ID3D11DepthStencilView>v;if(FAILED(d->CreateDepthStencilView(r,&x,&v)))throw std::runtime_error("CreateDSV");return v;}
static ComPtr<ID3D11ShaderResourceView> makeSRV(ID3D11Device*d,ID3D11Resource*r,DXGI_FORMAT f){D3D11_SHADER_RESOURCE_VIEW_DESC x{};x.Format=f;x.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;x.Texture2D.MipLevels=1;ComPtr<ID3D11ShaderResourceView>v;if(FAILED(d->CreateShaderResourceView(r,&x,&v)))throw std::runtime_error("CreateSRV");return v;}
static unsigned g_checks=0;
static void check(bool v,const char*s){++g_checks;if(!v)throw std::runtime_error(s);}
static void runCase(ID3D11Device*d,ID3D11DeviceContext*imm,DXGI_FORMAT dsvf,DXGI_FORMAT resourcef,DXGI_FORMAT depthSRVf,DXGI_FORMAT stencilSRVf,bool stencil,UINT ow,UINT oh){
 const UINT iw=7,ih=5;auto in=makeTex(d,iw,ih,resourcef,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE);auto id=makeDSV(d,in.Get(),dsvf);auto ds=makeSRV(d,in.Get(),depthSRVf);ComPtr<ID3D11ShaderResourceView>ss;if(stencil)ss=makeSRV(d,in.Get(),stencilSRVf);
 auto vb=compile(kInitVS,"main","vs_5_0"),pb=compile(kInitPS,"main","ps_5_0");ComPtr<ID3D11VertexShader>vs;ComPtr<ID3D11PixelShader>ps;check(SUCCEEDED(d->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vs)),"init VS");check(SUCCEEDED(d->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&ps)),"init PS");
 D3D11_DEPTH_STENCIL_DESC z{};z.DepthEnable=TRUE;z.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;z.DepthFunc=D3D11_COMPARISON_ALWAYS;z.StencilEnable=stencil;z.StencilReadMask=255;z.StencilWriteMask=0;z.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};z.BackFace=z.FrontFace;ComPtr<ID3D11DepthStencilState>zs;check(SUCCEEDED(d->CreateDepthStencilState(&z,&zs)),"init state");
 D3D11_VIEWPORT iv{0,0,(float)iw,(float)ih,0,1};imm->RSSetViewports(1,&iv);imm->OMSetRenderTargets(0,nullptr,id.Get());imm->OMSetDepthStencilState(zs.Get(),0);imm->VSSetShader(vs.Get(),nullptr,0);imm->PSSetShader(ps.Get(),nullptr,0);imm->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 edvr_layer_seed::Seeder s;s.init(d);check(s.ensure(d,ow,oh,dsvf),"ensure");ComPtr<ID3D11DeviceContext>def;check(SUCCEEDED(d->CreateDeferredContext(0,&def)),"deferred context");
 D3D11_TEXTURE2D_DESC sd{};sd.Width=ow;sd.Height=oh;sd.MipLevels=sd.ArraySize=1;sd.Format=resourcef;sd.SampleDesc.Count=1;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D>stage;check(SUCCEEDED(d->CreateTexture2D(&sd,nullptr,&stage)),"staging");ID3D11Resource*oraw=nullptr;s.view()->GetResource(&oraw);ComPtr<ID3D11Resource>out;out.Attach(oraw);
 const UINT vals[]={0,1,5,11,15,128,255};for(UINT wanted:vals){imm->ClearDepthStencilView(id.Get(),D3D11_CLEAR_DEPTH|(stencil?D3D11_CLEAR_STENCIL:0),1,static_cast<UINT8>(wanted));imm->Draw(3,0);check(s.record(def.Get(),ds.Get(),ss.Get(),iw,ih,0.25f,-0.25f),"record");ComPtr<ID3D11CommandList>list;check(SUCCEEDED(def->FinishCommandList(FALSE,&list)),"FinishCommandList");imm->ExecuteCommandList(list.Get(),TRUE);imm->CopyResource(stage.Get(),out.Get());D3D11_MAPPED_SUBRESOURCE m{};check(SUCCEEDED(imm->Map(stage.Get(),0,D3D11_MAP_READ,0,&m)),"Map");for(UINT y=0;y<oh;y++)for(UINT x=0;x<ow;x++){auto row=(const unsigned char*)m.pData+y*m.RowPitch;size_t off=x*(resourcef==DXGI_FORMAT_R24G8_TYPELESS?4:(resourcef==DXGI_FORMAT_R32G8X24_TYPELESS?8:4));if(stencil&&row[off+(resourcef==DXGI_FORMAT_R24G8_TYPELESS?3:4)]!=wanted){imm->Unmap(stage.Get(),0);throw std::runtime_error("stencil readback mismatch");}float f=0;if(resourcef==DXGI_FORMAT_R24G8_TYPELESS){UINT packed=row[off]|(row[off+1]<<8)|(row[off+2]<<16);f=packed/16777215.0f;}else memcpy(&f,row+off,4);int sx=(int)floor(((x+0.5f)*iw/ow)+0.25f),sy=(int)floor(((y+0.5f)*ih/oh)-0.25f);sx=(sx<0?0:sx>=int(iw)?int(iw)-1:sx);sy=(sy<0?0:sy>=int(ih)?int(ih)-1:sy);float expected=0.1f+0.8f*(0.6f*((sx+0.5f)/iw)+0.4f*((sy+0.5f)/ih));if(fabs(f-expected)>0.0002f){imm->Unmap(stage.Get(),0);throw std::runtime_error("depth address readback mismatch");}}imm->Unmap(stage.Get(),0);}
 imm->ClearDepthStencilView(id.Get(),D3D11_CLEAR_DEPTH|(stencil?D3D11_CLEAR_STENCIL:0),1,0);imm->Draw(3,0);check(s.record(def.Get(),ds.Get(),ss.Get(),iw,ih,0,0),"reuse record");ComPtr<ID3D11CommandList>reuse;check(SUCCEEDED(def->FinishCommandList(FALSE,&reuse)),"reuse Finish");imm->ExecuteCommandList(reuse.Get(),TRUE);imm->CopyResource(stage.Get(),out.Get());D3D11_MAPPED_SUBRESOURCE rm{};check(SUCCEEDED(imm->Map(stage.Get(),0,D3D11_MAP_READ,0,&rm)),"reuse Map");for(UINT y=0;y<oh;y++)for(UINT x=0;x<ow;x++){auto row=(const unsigned char*)rm.pData+y*rm.RowPitch;size_t off=x*(resourcef==DXGI_FORMAT_R24G8_TYPELESS?4:(resourcef==DXGI_FORMAT_R32G8X24_TYPELESS?8:4));if(stencil&&row[off+(resourcef==DXGI_FORMAT_R24G8_TYPELESS?3:4)]!=0){imm->Unmap(stage.Get(),0);throw std::runtime_error("reuse zero stencil mismatch");}}imm->Unmap(stage.Get(),0);
 std::printf("case dsv=%d stencil=%d specifiedStencil=%d PASS values=0,1,5,11,15,128,255 reuse-zero scale=%.3f/%.3f jitter=+0.25/-0.25\n",(int)dsvf,stencil?1:0,s.usesSpecifiedStencilRef()?1:0,(double)ow/iw,(double)oh/ih);
}

// Patch only the fixture object's vptr, forwarding every real D3D11 command.
template<size_t N> struct VtablePatch {
    void* object; void** original; void* table[N]{};
    VtablePatch(void* o):object(o),original(*reinterpret_cast<void***>(o)) {
        memcpy(table,original,sizeof(table)); replace(table);
    }
    void replace(void** value) {
        DWORD old=0;check(VirtualProtect(object,sizeof(void*),PAGE_READWRITE,&old)!=0,"spy vptr writable");
        *reinterpret_cast<void***>(object)=value;
        DWORD unused=0;VirtualProtect(object,sizeof(void*),old,&unused);
    }
    ~VtablePatch(){replace(original);}
};
struct CommandSpy:VtablePatch<115> {
    using DrawFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
    using ClearFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11DepthStencilView*,UINT,FLOAT,UINT8);
    using MapFn=HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
    static CommandSpy* active;
    UINT clears=0,draws=0;bool failMap=false;
    CommandSpy(ID3D11DeviceContext* c):VtablePatch(c) {
        check(!active,"single command spy");active=this;
        table[13]=reinterpret_cast<void*>(&draw);table[53]=reinterpret_cast<void*>(&clear);
        table[14]=reinterpret_cast<void*>(&map);
    }
    ~CommandSpy(){active=nullptr;}
    static void STDMETHODCALLTYPE draw(ID3D11DeviceContext*c,UINT n,UINT first) {
        ++active->draws;reinterpret_cast<DrawFn>(active->original[13])(c,n,first);
    }
    static void STDMETHODCALLTYPE clear(ID3D11DeviceContext*c,ID3D11DepthStencilView*v,UINT f,FLOAT z,UINT8 s) {
        check(f==D3D11_CLEAR_STENCIL&&s==0,"production stencil clear parameters");
        ++active->clears;reinterpret_cast<ClearFn>(active->original[53])(c,v,f,z,s);
    }
    static HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext*c,ID3D11Resource*r,UINT sub,D3D11_MAP kind,UINT f,D3D11_MAPPED_SUBRESOURCE*m) {
        if(active->failMap&&kind==D3D11_MAP_WRITE_DISCARD)return E_FAIL;
        return reinterpret_cast<MapFn>(active->original[14])(c,r,sub,kind,f,m);
    }
};
CommandSpy* CommandSpy::active=nullptr;
struct FallbackCapability:VtablePatch<43> {
    using FeatureFn=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,D3D11_FEATURE,void*,UINT);
    static FallbackCapability* active;
    UINT optionQueries=0;
    FallbackCapability(ID3D11Device*d):VtablePatch(d) {
        check(!active,"single feature spy");active=this;table[33]=reinterpret_cast<void*>(&feature);
    }
    ~FallbackCapability(){active=nullptr;}
    static HRESULT STDMETHODCALLTYPE feature(ID3D11Device*d,D3D11_FEATURE f,void*p,UINT n) {
        HRESULT h=reinterpret_cast<FeatureFn>(active->original[33])(d,f,p,n);
        if(f==D3D11_FEATURE_D3D11_OPTIONS2&&n==sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS2)) {
            ++active->optionQueries;
            if(SUCCEEDED(h))static_cast<D3D11_FEATURE_DATA_D3D11_OPTIONS2*>(p)->PSSpecifiedStencilRefSupported=FALSE;
        }
        return h;
    }
};
FallbackCapability* FallbackCapability::active=nullptr;
static UINT popcount8(UINT m){UINT n=0;for(UINT b=1;b<256;b<<=1)if(m&b)++n;return n;}
static void observedCase(ID3D11Device*d,ID3D11DeviceContext*imm,bool wideDepth,UINT ow,UINT oh,bool forceFallback) {
    const UINT iw=7,ih=5;
    const DXGI_FORMAT rf=wideDepth?DXGI_FORMAT_R32G8X24_TYPELESS:DXGI_FORMAT_R24G8_TYPELESS;
    const DXGI_FORMAT vf=wideDepth?DXGI_FORMAT_D32_FLOAT_S8X24_UINT:DXGI_FORMAT_D24_UNORM_S8_UINT;
    auto in=makeTex(d,iw,ih,rf,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE);
    auto id=makeDSV(d,in.Get(),vf);
    auto ds=makeSRV(d,in.Get(),wideDepth?DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
    auto ss=makeSRV(d,in.Get(),wideDepth?DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:DXGI_FORMAT_X24_TYPELESS_G8_UINT);
    auto vb=compile(kInitVS,"main","vs_5_0"),pb=compile(kInitPS,"main","ps_5_0");
    ComPtr<ID3D11VertexShader>vs;ComPtr<ID3D11PixelShader>ps;
    check(SUCCEEDED(d->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vs)),"observed init VS");
    check(SUCCEEDED(d->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&ps)),"observed init PS");
    D3D11_DEPTH_STENCIL_DESC z{};z.DepthEnable=TRUE;z.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;z.DepthFunc=D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState>zs;check(SUCCEEDED(d->CreateDepthStencilState(&z,&zs)),"observed init state");
    edvr_layer_seed::Seeder s;
    if(forceFallback){FallbackCapability cap(d);s.init(d);check(cap.optionQueries==1,"fallback capability query observed");}else s.init(d);
    check(!forceFallback||!s.usesSpecifiedStencilRef(),"forced fallback selected");
    check(s.ensure(d,ow,oh,vf),"observed ensure");
    ComPtr<ID3D11DeviceContext>def;check(SUCCEEDED(d->CreateDeferredContext(0,&def)),"observed deferred");
    auto stage=makeTex(d,ow,oh,rf,0,D3D11_USAGE_STAGING,D3D11_CPU_ACCESS_READ);
    ID3D11Resource*raw=nullptr;s.view()->GetResource(&raw);ComPtr<ID3D11Resource>out;out.Attach(raw);
    const bool specified=s.usesSpecifiedStencilRef();UINT cases=0,totalClear=0,totalDraw=0;
    CommandSpy spy(def.Get());
    auto read=[&](){
        imm->CopyResource(stage.Get(),out.Get());D3D11_MAPPED_SUBRESOURCE m{};
        check(SUCCEEDED(imm->Map(stage.Get(),0,D3D11_MAP_READ,0,&m)),"observed map");
        std::vector<unsigned long long> pixels;
        for(UINT y=0;y<oh;++y)for(UINT x=0;x<ow;++x) {
            const BYTE* p=static_cast<const BYTE*>(m.pData)+y*m.RowPitch+x*(wideDepth?8:4);
            UINT depth=0;memcpy(&depth,p,4);if(!wideDepth)depth&=0xffffff;
            pixels.push_back(static_cast<unsigned long long>(depth)|(static_cast<unsigned long long>(p[wideDepth?4:3])<<32));
        }
        imm->Unmap(stage.Get(),0);return pixels;
    };
    auto record=[&](UINT mask,bool needsDepth,float jx,float jy,bool legacy) {
        imm->ClearDepthStencilView(s.view(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,0.93f,0xa5);
        if(legacy)imm->ClearDepthStencilView(s.view(),D3D11_CLEAR_STENCIL,0,0);
        spy.clears=spy.draws=0;
        check(s.record(def.Get(),ds.Get(),ss.Get(),iw,ih,jx,jy,mask,needsDepth),"observed record");
        const bool fullWrite=specified&&(needsDepth||mask!=0);
        check(spy.clears==1,"production exactly one clear for stencil input");
        check(spy.draws==(specified?(fullWrite?1u:0u):((needsDepth?1u:0u)+popcount8(mask))),"production draw count");
        if(!legacy){totalClear+=spy.clears;totalDraw+=spy.draws;}
        ComPtr<ID3D11CommandList>list;check(SUCCEEDED(def->FinishCommandList(FALSE,&list)),"observed finish");
        imm->ExecuteCommandList(list.Get(),TRUE);return read();
    };
    struct Mode{UINT mask;bool depth;};
    const Mode modes[]={{255,true},{1,true},{0,true},{255,false},{1,false},{0,false}};
    for(UINT mi=0;mi<6;++mi)for(UINT value=0;value<256;++value) {
        // Exhaust every stencil byte on the normal pass; boundary classes on each special mode.
        if(mi&&value!=0&&value!=1&&value!=5&&value!=15&&value!=128&&value!=165&&value!=255)continue;
        D3D11_VIEWPORT vp{0,0,float(iw),float(ih),0,1};imm->RSSetState(nullptr);
        imm->RSSetViewports(1,&vp);imm->OMSetRenderTargets(0,nullptr,id.Get());imm->OMSetDepthStencilState(zs.Get(),0);
        imm->VSSetShader(vs.Get(),nullptr,0);imm->PSSetShader(ps.Get(),nullptr,0);
        imm->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        imm->ClearDepthStencilView(id.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,static_cast<UINT8>(value));imm->Draw(3,0);
        float jx=(value&1)?0.25f:-1.75f,jy=(value&2)?-0.25f:1.5f;
        auto actual=record(modes[mi].mask,modes[mi].depth,jx,jy,false);
        auto legacy=record(modes[mi].mask,modes[mi].depth,jx,jy,true);
        check(actual==legacy,"poisoned output equals legacy clear output (useful bits exactly)");
        const bool fullWrite=specified&&(modes[mi].depth||modes[mi].mask);
        const UINT wantedStencil=specified?(fullWrite?value:0):(value&modes[mi].mask);
        for(UINT y=0;y<oh;++y)for(UINT x=0;x<ow;++x) {
            const auto p=actual[y*ow+x];check(UINT(p>>32)==wantedStencil,"all stencil bits overwritten/preserved exactly");
            float depth=0;UINT bits=UINT(p);if(wideDepth)memcpy(&depth,&bits,4);else depth=bits/16777215.0f;
            int sx=int(floor((x+0.5f)*iw/ow+jx)),sy=int(floor((y+0.5f)*ih/oh+jy));
            sx=sx<0?0:(sx>=int(iw)?int(iw)-1:sx);sy=sy<0?0:(sy>=int(ih)?int(ih)-1:sy);
            float expected=(modes[mi].depth||fullWrite)?0.1f+0.8f*(0.6f*(sx+0.5f)/iw+0.4f*(sy+0.5f)/ih):0.93f;
            check(fabs(depth-expected)<0.0002f,"depth and dynamic scale/jitter constants preserved");
        }
        ++cases;
    }
    // A constant-map failure retains the legacy recorded clear and emits no draw.
    imm->ClearDepthStencilView(s.view(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,0.93f,0xa5);
    spy.clears=spy.draws=0;spy.failMap=true;bool threw=false;
    try{s.record(def.Get(),ds.Get(),ss.Get(),iw,ih,0,0);}catch(const std::runtime_error&){threw=true;}
    spy.failMap=false;check(threw,"map failure propagated");check(spy.clears==1&&spy.draws==0,"map failure preserves clear and no draw");
    ComPtr<ID3D11CommandList>failed;check(SUCCEEDED(def->FinishCommandList(FALSE,&failed)),"failed finish");imm->ExecuteCommandList(failed.Get(),TRUE);
    for(auto pixel:read()) {
        check(UINT(pixel>>32)==0,"map failure command list still clears stencil");
        float depth=0;UINT bits=UINT(pixel);if(wideDepth)memcpy(&depth,&bits,4);else depth=bits/16777215.0f;
        check(fabs(depth-0.93f)<0.0002f,"map failure preserves poisoned depth");
    }
    check(!s.record(nullptr,ds.Get(),ss.Get(),iw,ih,0,0),"null context rejected");
    check(!s.record(imm,ds.Get(),ss.Get(),iw,ih,0,0),"immediate context rejected");
    spy.clears=spy.draws=0;threw=false;
    try{s.record(def.Get(),nullptr,ss.Get(),iw,ih,0,0);}catch(const std::runtime_error&){threw=true;}
    check(threw&&spy.clears==0&&spy.draws==0,"invalid depth rejected before clear or draw");
    std::printf("observed dsv=%d specified=%d forcedFallback=%d size=%ux%u cases=%u clears=%u draws=%u PASS poison/legacy/all256/partial/no-draw/map-failure\n",int(vf),specified?1:0,forceFallback?1:0,ow,oh,cases,totalClear,totalDraw);
}
int main(int argc,char**argv) {
    try {
        bool hardware=false;
        for(int i=1;i<argc;++i) {
            if(!strcmp(argv[i],"--dry-run")){std::puts("seed_test dry-run: no files or device commands");return 0;}
            if(!strcmp(argv[i],"--hardware"))hardware=true;
            else if(strcmp(argv[i],"--self-test"))throw std::runtime_error("unknown argument");
        }
        ComPtr<ID3D11Device>d;ComPtr<ID3D11DeviceContext>imm;D3D_FEATURE_LEVEL fl;
        // Match Frontier's current 12_0 device when hardware provides it; WARP stays portable.
        const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_12_0,D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        check(SUCCEEDED(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,hardware?levels:nullptr,hardware?3:0,D3D11_SDK_VERSION,&d,&fl,&imm)),"create test device");
        D3D11_FEATURE_DATA_D3D11_OPTIONS2 options{};
        HRESULT capability=d->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2,&options,sizeof(options));
        ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC description{};
        check(SUCCEEDED(d.As(&dxgi))&&SUCCEEDED(dxgi->GetAdapter(&adapter))&&SUCCEEDED(adapter->GetDesc(&description)),"adapter identity");
        char name[256]{};WideCharToMultiByte(CP_UTF8,0,description.Description,-1,name,sizeof(name),nullptr,nullptr);
        std::printf("device adapter=%s featureLevel=0x%x options2=0x%08lx specifiedStencil=%d\n",name,UINT(fl),static_cast<unsigned long>(capability),options.PSSpecifiedStencilRefSupported?1:0);
        if(FAILED(capability)||!options.PSSpecifiedStencilRefSupported)std::puts("specified-stencil full-write GPU path unavailable; testing actual fallback, never forcing unsupported capability");
        runCase(d.Get(),imm.Get(),DXGI_FORMAT_D24_UNORM_S8_UINT,DXGI_FORMAT_R24G8_TYPELESS,DXGI_FORMAT_R24_UNORM_X8_TYPELESS,DXGI_FORMAT_X24_TYPELESS_G8_UINT,true,13,9);
        runCase(d.Get(),imm.Get(),DXGI_FORMAT_D24_UNORM_S8_UINT,DXGI_FORMAT_R24G8_TYPELESS,DXGI_FORMAT_R24_UNORM_X8_TYPELESS,DXGI_FORMAT_X24_TYPELESS_G8_UINT,true,7,5);
        runCase(d.Get(),imm.Get(),DXGI_FORMAT_D32_FLOAT_S8X24_UINT,DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,DXGI_FORMAT_X32_TYPELESS_G8X24_UINT,true,13,9);
        runCase(d.Get(),imm.Get(),DXGI_FORMAT_D32_FLOAT,DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_UNKNOWN,false,13,9);
        for(bool wide:{false,true})for(bool fallback:{false,true}) {
            observedCase(d.Get(),imm.Get(),wide,13,9,fallback);
            observedCase(d.Get(),imm.Get(),wide,7,5,fallback);
        }
        std::printf("seed_test PASS checks=%u driver=%s\n",g_checks,hardware?"hardware":"WARP");return 0;
    }catch(const std::exception&e){std::fprintf(stderr,"seed_test FAIL: %s\n",e.what());return 1;}
}
