// GPU regression for night vision without surface fill, geometry contours,
// corrected radial pulse and the live A/B replacement's state contract.
#include "../../src/d3d11/night_vision.cpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
using Microsoft::WRL::ComPtr;
unsigned checks=0;
void check(bool b,const char* why){++checks;if(!b){std::printf("FAIL: %s\n",why);std::exit(1);}}
void hr(HRESULT h){check(SUCCEEDED(h),"D3D operation");}
ComPtr<ID3DBlob> compile(const char* s,const char* entry,const char* profile,const D3D_SHADER_MACRO* macros=nullptr){
    ComPtr<ID3DBlob> c,e;HRESULT h=D3DCompile(s,strlen(s),nullptr,macros,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&c,&e);
    if(FAILED(h)&&e)std::puts(static_cast<const char*>(e->GetBufferPointer()));hr(h);return c;
}
namespace edvr {
bool testOn=true,testFail=false;uint64_t testVs=0xFCF7BD2896751D96ull,testPs=0xF786D34B5E118D5Eull;
Config& Config::get(){static Config c;return c;}
bool Config::getBool(const char* key,bool def)const{check(!strcmp(key,"fix.night_vision_stability")&&def,"live key defaults on");return testOn;}
Log& Log::get(){static Log l;return l;}Log::~Log()=default;void Log::note(const char*,...){}
uint64_t bindingShaderHash(BindSlot s){return s==BindSlot::Vs?testVs:testPs;}
ID3D11PixelShader* shaderSwapCompilePs(ID3D11DeviceContext* ctx,const char* s,size_t,const char* entry,const char*,const SwapMacro*,const char*){
    if(testFail)return nullptr;auto code=compile(s,entry,"ps_5_0");ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);ID3D11PixelShader* p=nullptr;
    hr(dev->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&p));return p;
}
void vScreenSetRenderTargetsRaw(ID3D11DeviceContext* c,UINT n,ID3D11RenderTargetView*const* r,ID3D11DepthStencilView* d){c->OMSetRenderTargets(n,r,d);}
ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext* ctx,const char* s,size_t,const char* entry,const char*,const SwapMacro*,const char*){
    if(testFail)return nullptr;auto code=compile(s,entry,"cs_5_0");ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);ID3D11ComputeShader* p=nullptr;
    hr(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&p));return p;
}
}
using namespace edvr;
struct Rig {
    ComPtr<ID3D11Texture2D> stencil;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11DepthStencilState> stencilState;
    void setStencil(const std::vector<unsigned>& values){
        ctx->OMSetRenderTargets(0,nullptr,nullptr);dsv.Reset();
        std::vector<unsigned> packed(64*64*2);for(unsigned i=0;i<64*64;++i){packed[i*2]=0x3f800000;packed[i*2+1]=values[i];}
        stencil=texture(DXGI_FORMAT_R32G8X24_TYPELESS,packed.data(),64*8,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE);
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        hr(dev->CreateDepthStencilView(stencil.Get(),&dd,&dsv));ctx->OMSetRenderTargets(1,rt.GetAddressOf(),dsv.Get());
    }
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;ComPtr<ID3D11InfoQueue> queue;
    ComPtr<ID3D11Buffer> camera,settings;ComPtr<ID3D11Texture2D> target,stage,depth,normals;
    ComPtr<ID3D11RenderTargetView> rt;
    ComPtr<ID3D11BlendState> originalBlend;
    std::vector<float> scene=std::vector<float>(64*64*4,0);
    ComPtr<ID3D11PixelShader> stock;float c[333][4]{},n[12][4]{};
    Rig(){
        D3D_FEATURE_LEVEL fl;HRESULT h=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);
        if(h==DXGI_ERROR_SDK_COMPONENT_MISSING)h=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);hr(h);dev.As(&queue);
        // The grid is disabled in these fixtures, so the unused TEXCOORD
        // can come from the same position without changing the reference.
        auto vsCode=compile("struct O{float4 p:SV_Position;float2 t:TEXCOORD4;};O main(uint id:SV_VertexID){O o;o.p=float4(id==2?3:-1,id==1?3:-1,0,1);o.t=o.p.xy;return o;}","main","vs_5_0");
        ComPtr<ID3D11VertexShader> vs;hr(dev->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));ctx->VSSetShader(vs.Get(),nullptr,0);
        const D3D_SHADER_MACRO macros[]={{"EDVR_NIGHT_STOCK","1"},{nullptr,nullptr}};auto code=compile(kNightVisionPs,"main","ps_5_0",macros);
        hr(dev->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&stock));ctx->PSSetShader(stock.Get(),nullptr,0);
        camera=buffer(sizeof(c));settings=buffer(sizeof(n));ctx->PSSetConstantBuffers(1,1,camera.GetAddressOf());ctx->PSSetConstantBuffers(2,1,settings.GetAddressOf());
        target=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,nullptr,64*16,D3D11_BIND_RENDER_TARGET);
        hr(dev->CreateRenderTargetView(target.Get(),nullptr,&rt));ctx->OMSetRenderTargets(1,rt.GetAddressOf(),nullptr);
        D3D11_BLEND_DESC bd{};auto& blend=bd.RenderTarget[0];blend.BlendEnable=TRUE;
        blend.SrcBlend=blend.SrcBlendAlpha=D3D11_BLEND_ONE;blend.DestBlend=blend.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
        blend.BlendOp=blend.BlendOpAlpha=D3D11_BLEND_OP_ADD;blend.RenderTargetWriteMask=7;
        hr(dev->CreateBlendState(&bd,&originalBlend));FLOAT factors[4]{.125f,.25f,.5f,.75f};ctx->OMSetBlendState(originalBlend.Get(),factors,~0u);
        D3D11_TEXTURE2D_DESC td{};target->GetDesc(&td);td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;td.BindFlags=0;hr(dev->CreateTexture2D(&td,nullptr,&stage));
        D3D11_VIEWPORT vp{0,0,64,64,0,1};ctx->RSSetViewports(1,&vp);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;ComPtr<ID3D11RasterizerState> rs;hr(dev->CreateRasterizerState(&rd,&rs));ctx->RSSetState(rs.Get());
        D3D11_SAMPLER_DESC sd{};sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        for(UINT i=0;i<2;++i){sd.Filter=i?D3D11_FILTER_MIN_MAG_MIP_POINT:D3D11_FILTER_MIN_MAG_MIP_LINEAR;ComPtr<ID3D11SamplerState> sp;hr(dev->CreateSamplerState(&sd,&sp));ctx->PSSetSamplers(i,1,sp.GetAddressOf());}
        std::vector<float> ones(64*64,1);bind(4,texture(DXGI_FORMAT_R32_FLOAT,ones.data(),64*4,D3D11_BIND_SHADER_RESOURCE));
        std::vector<unsigned> encoded(64*64,(512u<<10)|(512u<<20));normals=texture(DXGI_FORMAT_R10G10B10A2_UNORM,encoded.data(),64*4,D3D11_BIND_SHADER_RESOURCE);bind(2,normals);
        c[332][0]=c[332][1]=64;c[332][2]=c[332][3]=1.f/64;c[1][3]=c[90][1]=1;
        c[270][0]=c[271][1]=c[272][3]=1;c[273][2]=.025f;c[277][0]=c[278][1]=c[279][2]=1;
        n[0][3]=1;n[5][1]=1;n[5][3]=1000;n[6][2]=1;n[7][1]=1000;n[8][0]=n[8][1]=n[8][2]=1;
        n[10][0]=.2f;n[10][1]=.6f;n[10][2]=10;n[11][1]=1;
        // Finite procedural grid derivatives, zero amplitudes.
        n[2][2]=n[3][2]=n[4][2]=.1f;n[2][3]=n[3][3]=n[4][3]=1;
        D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthFunc=D3D11_COMPARISON_ALWAYS;ds.StencilEnable=TRUE;
        ds.StencilReadMask=128;ds.StencilWriteMask=4;ds.FrontFace.StencilFunc=D3D11_COMPARISON_EQUAL;
        ds.FrontFace.StencilFailOp=ds.FrontFace.StencilDepthFailOp=D3D11_STENCIL_OP_KEEP;ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE;ds.BackFace=ds.FrontFace;
        hr(dev->CreateDepthStencilState(&ds,&stencilState));ctx->OMSetDepthStencilState(stencilState.Get(),4);
        setStencil(std::vector<unsigned>(64*64,5));setDepth(400);nightVisionConfigure(Config::get());
    }
    ~Rig(){nightVisionShutdown();ctx->ClearState();}
    ComPtr<ID3D11Buffer> buffer(UINT bytes){D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> b;hr(dev->CreateBuffer(&d,nullptr,&b));return b;}
    ComPtr<ID3D11Texture2D> texture(DXGI_FORMAT format,const void* p,UINT pitch,UINT bind){D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=64;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=format;d.BindFlags=bind;D3D11_SUBRESOURCE_DATA sd{};sd.pSysMem=p;sd.SysMemPitch=pitch;ComPtr<ID3D11Texture2D> t;hr(dev->CreateTexture2D(&d,p?&sd:nullptr,&t));return t;}
    void bind(UINT slot,ComPtr<ID3D11Texture2D> t){ComPtr<ID3D11ShaderResourceView> v;hr(dev->CreateShaderResourceView(t.Get(),nullptr,&v));ctx->PSSetShaderResources(slot,1,v.GetAddressOf());}
    void setDepth(float value){std::vector<float> d(64*64,value);depth=texture(DXGI_FORMAT_R32_FLOAT,d.data(),64*4,D3D11_BIND_SHADER_RESOURCE);bind(1,depth);}
    std::vector<float> draw(bool fix){
        ctx->UpdateSubresource(camera.Get(),0,nullptr,c,0,0);ctx->UpdateSubresource(settings.Get(),0,nullptr,n,0,0);
        ctx->UpdateSubresource(target.Get(),0,nullptr,scene.data(),64*16,0);
        ComPtr<ID3D11BlendState> beforeBlend;FLOAT beforeFactors[4];UINT beforeMask;ctx->OMGetBlendState(&beforeBlend,beforeFactors,&beforeMask);
        if(fix)nightVisionBegin(ctx.Get());ctx->Draw(3,0);if(fix)nightVisionEnd(ctx.Get());
        ComPtr<ID3D11PixelShader> ps;ctx->PSGetShader(&ps,nullptr,nullptr);check(ps.Get()==stock.Get(),"original PS restored");
        ComPtr<ID3D11BlendState> afterBlend;FLOAT afterFactors[4];UINT afterMask;ctx->OMGetBlendState(&afterBlend,afterFactors,&afterMask);
        check(afterBlend==beforeBlend && !memcmp(beforeFactors,afterFactors,sizeof(beforeFactors)) && beforeMask==afterMask,"original blend, factors and sample mask restored");
        ctx->CopyResource(stage.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE m{};hr(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&m));std::vector<float> out(64*64*4);
        for(UINT y=0;y<64;++y)memcpy(out.data()+y*64*4,static_cast<char*>(m.pData)+y*m.RowPitch,64*16);ctx->Unmap(stage.Get(),0);return out;
    }
    void clean(){if(queue)for(UINT64 i=0;i<queue->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T sz=0;queue->GetMessage(i,nullptr,&sz);std::vector<char> b(sz);auto* m=reinterpret_cast<D3D11_MESSAGE*>(b.data());hr(queue->GetMessage(i,m,&sz));if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::puts(m->pDescription);check(false,"no D3D warnings");}}}
};
void test(){
    Rig r;check(nightVisionMatches('X',240,1),"exact night pair accepted");check(!nightVisionMatches('D',240,1)&&!nightVisionMatches('X',6,1)&&!nightVisionMatches('X',240,2),"unrelated draw shapes rejected");
    testPs=0;check(!nightVisionMatches('X',240,1),"unrelated shader rejected");testPs=0xF786D34B5E118D5Eull;
    // The same world point under two camera rotations, placed at pixel
    // centre with asymmetric projection offsets. Its radial pulse must
    // agree with the analytic distance even though forward depth changes.
    // Use the intentionally pixelated artistic mode to isolate the pulse
    // from the new geometry/texture treatment. Calibrate its colour scale
    // using the constant pulse floor; the nonzero pulse has analytic 0.6.
    unsigned one=1;memcpy(&r.n[11][2],&one,4);
    float pulse[2]{},oldPulse[2]{};
    for(int i=0;i<2;++i){
        float angle=i*.6f,cs=cosf(angle),sn=sinf(angle);float x=200*cs-400*sn,z=200*sn+400*cs;
        r.c[270][0]=cs;r.c[272][0]=-sn;r.c[270][3]=sn;r.c[272][3]=cs;
        float offset=1.f/64-x/z;r.c[270][0]+=offset*sn;r.c[272][0]+=offset*cs;
        r.c[271][1]=1;r.c[270][1]=-sn/64;r.c[272][1]=-cs/64;
        r.setDepth(z);r.n[1][0]=547.2136f;
        auto a=r.draw(true),b=r.draw(false);r.n[10][1]=0;auto floor=r.draw(true);r.n[10][1]=.6f;
        pulse[i]=a[(32*64+32)*4+1]/floor[(32*64+32)*4+1]*.2f;oldPulse[i]=b[(32*64+32)*4+1]/floor[(32*64+32)*4+1]*.2f;
        check(fabsf(pulse[i]-.6f)<.0001f,"radial pulse matches fixed-point analytic distance");
    }
    check(fabsf(pulse[0]-pulse[1])<.0001f&&fabsf(oldPulse[0]-oldPulse[1])>.02f,"head rotation moves stock pulse but not corrected pulse");
    // Intentional pixelation keeps its original artistic normal shading.
    r.n[10][1]=0;r.n[9][0]=.218f;r.n[9][1]=.5f;r.n[9][2]=-.5f;
    std::vector<unsigned> pattern(64*64);for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)pattern[y*64+x]=((x%2?750u:512u)<<10)|(512u<<20);
    r.ctx->UpdateSubresource(r.normals.Get(),0,nullptr,pattern.data(),64*4,0);
    auto fixed=r.draw(true),stock=r.draw(false);
    check(fabsf(stock[(32*64+30)*4+1]-stock[(32*64+31)*4+1])<1e-6f,"reference reproduces forced 2x2 normal lookup");
    for(size_t i=0;i<fixed.size();++i)check(std::isfinite(fixed[i])&&fabsf(fixed[i]-stock[i])<1e-5f,"intentional pixelation and non-pulse shading preserved");
    r.n[10][1]=.6f;r.c[270][0]=r.c[271][0]=r.c[272][0]=0;
    fixed=r.draw(true);stock=r.draw(false);
    for(size_t i=0;i<fixed.size();++i)check(std::isfinite(fixed[i])&&fabsf(fixed[i]-stock[i])<1e-5f,"singular camera retains original pulse");
    // A matched hash is insufficient when a future build changes resources.
    r.ctx->PSSetConstantBuffers(2,1,r.camera.GetAddressOf());nightVisionBegin(r.ctx.Get());
    ComPtr<ID3D11PixelShader> ps;r.ctx->PSGetShader(&ps,nullptr,nullptr);check(ps.Get()==r.stock.Get(),"wrong settings size rejected");nightVisionEnd(r.ctx.Get());r.ctx->PSSetConstantBuffers(2,1,r.settings.GetAddressOf());
    r.bind(2,r.depth);fixed=r.draw(true);stock=r.draw(false);check(fixed==stock,"wrong normal format draws stock");r.bind(2,r.normals);stock=r.draw(false);
    // The second shader output is a blend factor, not another render
    // target. A changed MRT/blend contract must retain the original draw.
    r.ctx->OMSetBlendState(nullptr,nullptr,~0u);nightVisionBegin(r.ctx.Get());ps.Reset();r.ctx->PSGetShader(&ps,nullptr,nullptr);
    check(ps==r.stock,"unknown blend keeps original shader");nightVisionEnd(r.ctx.Get());r.ctx->OMSetBlendState(r.originalBlend.Get(),nullptr,~0u);
    auto other=r.texture(DXGI_FORMAT_R32G32B32A32_FLOAT,nullptr,64*16,D3D11_BIND_RENDER_TARGET);ComPtr<ID3D11RenderTargetView> otherRT;
    hr(r.dev->CreateRenderTargetView(other.Get(),nullptr,&otherRT));ID3D11RenderTargetView* mrt[]{r.rt.Get(),otherRT.Get()};
    r.ctx->OMSetRenderTargets(2,mrt,nullptr);nightVisionBegin(r.ctx.Get());ps.Reset();r.ctx->PSGetShader(&ps,nullptr,nullptr);
    check(ps==r.stock,"MRT pass keeps original shader");nightVisionEnd(r.ctx.Get());r.ctx->OMSetRenderTargets(1,r.rt.GetAddressOf(),r.dsv.Get());
    // Nested Begin is harmless, including live disabling before End.
    nightVisionBegin(r.ctx.Get());ps.Reset();r.ctx->PSGetShader(&ps,nullptr,nullptr);check(ps!=r.stock,"known single target engages");
    nightVisionBegin(r.ctx.Get());testOn=false;nightVisionConfigure(Config::get());nightVisionEnd(r.ctx.Get());
    ps.Reset();r.ctx->PSGetShader(&ps,nullptr,nullptr);check(ps==r.stock,"live disabling restores an engaged draw");testOn=true;nightVisionConfigure(Config::get());
    ComPtr<ID3D11DeviceContext> deferred;hr(r.dev->CreateDeferredContext(0,&deferred));nightVisionBegin(deferred.Get());ps.Reset();deferred->PSGetShader(&ps,nullptr,nullptr);check(!ps,"deferred context rejected");
    testOn=false;nightVisionConfigure(Config::get());check(!nightVisionMatches('X',240,1),"live Off bypasses fix");fixed=r.draw(true);
    check(fixed==stock,"live Off draws original pixels");
    testOn=true;nightVisionConfigure(Config::get());check(nightVisionMatches('X',240,1),"live On reengages");
    nightVisionShutdown();testFail=true;fixed=r.draw(true);check(fixed==stock&&!nightVisionMatches('X',240,1),"compile failure draws stock and stands down");testFail=false;
    r.clean();
}
void geometryTest(){
    Rig r;r.n[10][1]=0;r.n[11][0]=40;r.n[5][3]=r.n[7][1]=1000000;
    // Material normal-map ridges on a perfectly flat depth surface must
    // not acquire geometry outlines or directional normal-map colour.
    std::vector<unsigned> pattern(64*64);for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)
        pattern[y*64+x]=((x%8<4?750u:512u)<<10)|(512u<<20);
    r.ctx->UpdateSubresource(r.normals.Get(),0,nullptr,pattern.data(),64*4,0);
    auto fixed=r.draw(true),stock=r.draw(false);float lo=1e9f,hi=0,oldLo=1e9f,oldHi=0;
    for(unsigned y=3;y<61;++y)for(unsigned x=3;x<61;++x){size_t i=(y*64+x)*4+1;lo=(std::min)(lo,fixed[i]);hi=(std::max)(hi,fixed[i]);oldLo=(std::min)(oldLo,stock[i]);oldHi=(std::max)(oldHi,stock[i]);}
    check(hi-lo<1e-6 && oldHi-oldLo>.01,"flat textured surface is shaded, not outlined");
    // A tilted perspective plane has affine reciprocal depth, unlike
    // forward depth. Its surface must stay free of false contour bands.
    std::vector<float> plane(64*64);for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)plane[y*64+x]=1/(.002f+x*.00001f+y*.00002f);
    r.ctx->UpdateSubresource(r.depth.Get(),0,nullptr,plane.data(),64*4,0);fixed=r.draw(true);
    for(unsigned y=3;y<61;++y)for(unsigned x=3;x<61;++x)
        check(std::fabs(fixed[(y*64+x)*4+1])<2e-6,"sloped plane remains free of outlines");
    // A physical depth step survives, with bounded response even against
    // arbitrarily distant background. Scaling the scene keeps its contour.
    float contour[2]{};
    for(int scale=0;scale<2;++scale){
        for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)plane[y*64+x]=(x<32?100.f:300.f)*(scale?10:1);
        r.ctx->UpdateSubresource(r.depth.Get(),0,nullptr,plane.data(),64*4,0);fixed=r.draw(true);
        float fade=1-plane[32*64+31]/1000000;
        contour[scale]=fixed[(32*64+31)*4+1]/.2f/(fade*fade);
        check(contour[scale]>.1 && contour[scale]<40,"actual object silhouette receives a bounded outline");
        check(fixed[(32*64+20)*4+1]<1e-6,"object interior stays a shaded surface");
    }
    check(std::fabs(contour[0]-contour[1])<1e-5,"contours do not depend on absolute scene scale");
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)plane[y*64+x]=x<32?.025f:50000.f;
    r.ctx->UpdateSubresource(r.depth.Get(),0,nullptr,plane.data(),64*4,0);fixed=r.draw(true);
    for(size_t i=0;i<fixed.size();++i)check(std::isfinite(fixed[i])&&fixed[i]>=0,"extreme near/far silhouette stays finite");
    // A neutral brightness lift retains hue and texture contrast instead
    // of flattening it with a colour fill or normal-map overlay.
    r.setDepth(400);r.n[11][0]=0;r.n[8][0]=.012f;r.n[8][1]=1;r.n[8][2]=.742f;
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){size_t i=(y*64+x)*4;float value=x%2?.004f:.001f;
        r.scene[i]=value*.5f;r.scene[i+1]=value;r.scene[i+2]=value*.75f;r.scene[i+3]=.375f;}
    fixed=r.draw(true);
    for(unsigned y=3;y<61;++y)for(unsigned x=3;x<61;++x){size_t i=(y*64+x)*4;
        for(unsigned ch=0;ch<3;++ch)
            check(std::fabs(fixed[i+ch]-r.scene[i+ch]*(2-400.f/1000000))<1e-7,
                "surface channels brighten equally, preserving hue and texture");
        check(fixed[i+3]==r.scene[i+3],"target alpha is untouched");}
    check(std::fabs(fixed[(32*64+31)*4+1]/fixed[(32*64+30)*4+1]-4)<1e-5,"texture contrast is retained");
    auto stable=fixed;r.c[277][0]=0;r.c[277][2]=1;r.c[279][0]=-1;r.c[279][2]=0;fixed=r.draw(true);
    check(fixed==stable,"surface appearance is independent of night vision normal-map lighting");
    // Captured cockpit contract: bit 128 skips static cockpit; bit 16
    // also marks body/chair. Verify both interior pixels and the adjacent
    // terrain pixel that previously inherited a false cockpit contour.
    std::vector<unsigned> stencil(64*64);
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)stencil[y*64+x]=x<16?144:x<32?16:1;
    r.setStencil(stencil);r.n[11][0]=40;
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)plane[y*64+x]=x<32?1.f:400.f;
    r.ctx->UpdateSubresource(r.depth.Get(),0,nullptr,plane.data(),64*4,0);fixed=r.draw(true);
    for(unsigned y=3;y<61;++y)for(unsigned x=3;x<61;++x)for(unsigned ch=0;ch<4;++ch){size_t i=(y*64+x)*4+ch;
        check(std::fabs(fixed[i]-(x<=32?r.scene[i]:stable[i]))<1e-7,"cockpit, body and exterior edge footprint excluded; terrain retained");}
    ComPtr<ID3D11DepthStencilState> savedStencil;UINT savedRef;r.ctx->OMGetDepthStencilState(&savedStencil,&savedRef);
    check(savedStencil==r.stencilState && savedRef==4,"original stencil state and reference stay bound");
    D3D11_TEXTURE2D_DESC sd{};r.stencil->GetDesc(&sd);sd.Usage=D3D11_USAGE_STAGING;sd.BindFlags=0;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;hr(r.dev->CreateTexture2D(&sd,nullptr,&readback));r.ctx->CopyResource(readback.Get(),r.stencil.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};hr(r.ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){auto* row=reinterpret_cast<unsigned*>(static_cast<char*>(mapped.pData)+y*mapped.RowPitch);
        check((row[x*2+1]&255)==(x<16?144u:x<32?20u:5u),"original stencil writes retained on excluded body pixels");}
    r.ctx->Unmap(readback.Get(),0);
    r.setStencil(std::vector<unsigned>(64*64,5));r.setDepth(400);r.n[11][0]=0;
    r.n[0][3]=0;fixed=r.draw(true);check(fixed==r.scene,"disabled effect leaves original scene intact");r.n[0][3]=1;
    r.n[6][2]=0;fixed=r.draw(true);check(fixed==r.scene,"zero effect intensity leaves original scene intact");r.n[6][2]=1;
    r.n[7][1]=100;fixed=r.draw(true);check(fixed==r.scene,"surface outside night vision range is untouched");r.n[7][1]=1000000;
    std::vector<float> zero(64*64,0);r.bind(4,r.texture(DXGI_FORMAT_R32_FLOAT,zero.data(),64*4,D3D11_BIND_SHADER_RESOURCE));
    fixed=r.draw(true);check(fixed==r.scene,"zero effect mask leaves original scene intact");
    r.clean();
}
int main(int argc,char** argv){if(argc!=2||strcmp(argv[1],"--self-test")){std::puts("Usage: night_vision_test --self-test");return 2;}test();geometryTest();std::printf("PASS: night vision (%u checks)\n",checks);}
