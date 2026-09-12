// Production source capture, screen history and pixel reprojection on WARP.
#include "../../src/d3d11/screen_motion.cpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <DirectXPackedVector.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <fstream>
using Microsoft::WRL::ComPtr;
unsigned checks=0;
void check(bool b,const char* label){++checks;if(!b){std::printf("FAIL: %s\n",label);std::exit(1);}}
void hr(HRESULT h){check(SUCCEEDED(h),"D3D operation");}
ComPtr<ID3DBlob> compile(const char* s,const char* profile,const char* entry="main"){
    ComPtr<ID3DBlob> c,e;HRESULT h=D3DCompile(s,strlen(s),nullptr,nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&c,&e);
    if(FAILED(h)&&e)std::puts(static_cast<const char*>(e->GetBufferPointer()));hr(h);return c;
}
namespace edvr {
ID3D11ShaderResourceView* testWeaponMotion=nullptr;
void weaponMotionConfigure(bool){}
void weaponMotionSource(ID3D11Texture2D*){}
ID3D11ShaderResourceView* weaponMotionView(){return testWeaponMotion;}
void weaponMotionFrameBoundary(){}
void weaponMotionShutdown(){}
uint64_t testVs=0,testPs=0;ID3D11RenderTargetView* testRtv=nullptr;
Log& Log::get(){static Log l;return l;}Log::~Log()=default;void Log::note(const char*,...){}
std::string Config::getString(const char*,const char*)const{return "dlss";}
bool Config::getBool(const char* key,bool def)const{check(!strcmp(key,"fix.weapon_stability")&&def,"weapon temporal motion uses existing live toggle");return true;}
void* bindingGet(BindSlot s){return s==BindSlot::Rtv0?testRtv:nullptr;}
uint64_t bindingShaderHash(BindSlot s){return s==BindSlot::Vs?testVs:s==BindSlot::Ps?testPs:0;}
bool bindingResolve(void* view,ResourceInfo* info){
    if(!view)return false;ComPtr<ID3D11Resource> r;static_cast<ID3D11RenderTargetView*>(view)->GetResource(&r);
    ComPtr<ID3D11Texture2D> t;if(FAILED(r.As(&t)))return false;D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);
    info->isTexture2D=true;info->a=d.Width;info->b=d.Height;return true;
}
ID3D11PixelShader* shaderSwapCompilePs(ID3D11DeviceContext* ctx,const char* s,size_t,const char*,const char*,const SwapMacro*,const char*){
    auto c=compile(s,"ps_5_0");ComPtr<ID3D11Device> d;ctx->GetDevice(&d);ID3D11PixelShader* p=nullptr;
    hr(d->CreatePixelShader(c->GetBufferPointer(),c->GetBufferSize(),nullptr,&p));return p;
}
void vScreenSetRenderTargetsRaw(ID3D11DeviceContext* c,UINT n,ID3D11RenderTargetView*const* r,ID3D11DepthStencilView* d){c->OMSetRenderTargets(n,r,d);}
}
using namespace edvr;
void __stdcall draw(ID3D11DeviceContext* c,unsigned,unsigned,unsigned,int,unsigned){c->Draw(3,0);}
std::vector<float> read(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11Texture2D* t){
    D3D11_TEXTURE2D_DESC td{};t->GetDesc(&td);td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;hr(d->CreateTexture2D(&td,nullptr,&st));c->CopyResource(st.Get(),t);
    const unsigned channels=(td.Format==DXGI_FORMAT_R32_FLOAT || td.Format==DXGI_FORMAT_R32_TYPELESS || td.Format==DXGI_FORMAT_R8_UNORM)?1:td.Format==DXGI_FORMAT_R32G32_FLOAT?2:4;
    D3D11_MAPPED_SUBRESOURCE m{};hr(c->Map(st.Get(),0,D3D11_MAP_READ,0,&m));std::vector<float> a(td.Width*td.Height*channels);
    for(UINT y=0;y<td.Height;++y)for(UINT x=0;x<td.Width*channels;++x){
        const auto* row=static_cast<const unsigned char*>(m.pData)+y*m.RowPitch;
        a[y*td.Width*channels+x]=(td.Format==DXGI_FORMAT_R8_UNORM || td.Format==DXGI_FORMAT_R8G8B8A8_UNORM)?row[x]/255.f:td.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?DirectX::PackedVector::XMConvertHalfToFloat(reinterpret_cast<const uint16_t*>(row)[x]):reinterpret_cast<const float*>(row)[x];
    }c->Unmap(st.Get(),0);return a;
}
#include "screen_consumer_test.h"
int main(int argc,char** argv){
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL fl;
    HRESULT h=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);
    if(h==DXGI_ERROR_SDK_COMPONENT_MISSING)h=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);hr(h);
    ComPtr<ID3D11InfoQueue> queue;dev.As(&queue);
    auto buf=[&](unsigned bytes,unsigned bind,const void* data=nullptr){
        D3D11_BUFFER_DESC b{};b.ByteWidth=bytes;b.BindFlags=bind;D3D11_SUBRESOURCE_DATA sd{};sd.pSysMem=data;
        ComPtr<ID3D11Buffer> p;hr(dev->CreateBuffer(&b,data?&sd:nullptr,&p));return p;
    };
    constexpr UINT W=64,H=48;
    D3D11_TEXTURE2D_DESC td{};td.Width=W;td.Height=H;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> colour,eye[2],depth;
    ComPtr<ID3D11RenderTargetView> rt,er[2];ComPtr<ID3D11ShaderResourceView> csrv;
    hr(dev->CreateTexture2D(&td,nullptr,&colour));hr(dev->CreateRenderTargetView(colour.Get(),nullptr,&rt));hr(dev->CreateShaderResourceView(colour.Get(),nullptr,&csrv));
    for(int i=0;i<2;++i){hr(dev->CreateTexture2D(&td,nullptr,&eye[i]));hr(dev->CreateRenderTargetView(eye[i].Get(),nullptr,&er[i]));}
    td.Format=DXGI_FORMAT_R32_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    hr(dev->CreateTexture2D(&td,nullptr,&depth));D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> ds;hr(dev->CreateDepthStencilView(depth.Get(),&dd,&ds));
    float source[276][4]{},camera[276][4]{},model[12][4]{},size[2]={1,1};
    source[270][0]=source[271][1]=source[272][3]=1;source[273][2]=.025f;
    camera[270][0]=camera[271][1]=camera[272][3]=1;camera[273][2]=.025f;
    model[9][0]=model[10][1]=model[11][2]=model[11][3]=1;
    auto sc=buf(sizeof(source),D3D11_BIND_CONSTANT_BUFFER),ec=buf(sizeof(camera),D3D11_BIND_CONSTANT_BUFFER),mc=buf(sizeof(model),D3D11_BIND_CONSTANT_BUFFER),vb=buf(sizeof(size),D3D11_BIND_VERTEX_BUFFER,size);
    auto vsCode=compile("struct O{float2 uv:__USER_VERTEX_M_TEXCOORD0;float4 p:SV_Position;};O main(uint id:SV_VertexID){O o;o.uv=float2((id<<1)&2,id&2);o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}","vs_5_0");
    ComPtr<ID3D11VertexShader> vs;hr(dev->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));
    auto psCode=compile("float4 main():SV_Target{return 1;}","ps_5_0");ComPtr<ID3D11PixelShader> ps;hr(dev->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));
    ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT vp{0,0,float(W),float(H),0,1};ctx->RSSetViewports(1,&vp);UINT stride=8,offset=0;ctx->IASetVertexBuffers(1,1,vb.GetAddressOf(),&stride,&offset);
    g.enabled=true;
    auto sourceDraw=[&](){
        ID3D11ShaderResourceView* none=nullptr;ctx->PSSetShaderResources(0,1,&none);
        ctx->OMSetRenderTargets(1,rt.GetAddressOf(),ds.Get());ctx->ClearDepthStencilView(ds.Get(),D3D11_CLEAR_DEPTH,.0025f,0);
        testRtv=rt.Get();testVs=0xACE405F428C17EF6ull;testPs=0;
        ctx->UpdateSubresource(sc.Get(),0,nullptr,source,0,0);ctx->VSSetConstantBuffers(1,1,sc.GetAddressOf());screenMotionSource(ctx.Get(),W,H);
    };
    auto screenDraw=[&](int e){
        ctx->OMSetRenderTargets(1,er[e].GetAddressOf(),nullptr);ctx->PSSetShaderResources(0,1,csrv.GetAddressOf());
        testVs=0x5C36AF051B98B9F1ull;testPs=0xCFE84157BC76E921ull;testRtv=er[e].Get();
        ctx->UpdateSubresource(ec.Get(),0,nullptr,camera,0,0);ctx->UpdateSubresource(mc.Get(),0,nullptr,model,0,0);
        ID3D11Buffer* cb[2]={mc.Get(),ec.Get()};ctx->VSSetConstantBuffers(0,2,cb);screenMotionDraw(ctx.Get(),draw,6,1,0,0,0);
        ComPtr<ID3D11RenderTargetView> after;ctx->OMGetRenderTargets(1,&after,nullptr);check(after.Get()==er[e].Get(),"game colour target restored");
        ComPtr<ID3D11PixelShader> afterPs;ctx->PSGetShader(&afterPs,nullptr,nullptr);check(afterPs.Get()==ps.Get(),"game pixel shader restored");
    };
    sourceDraw();check(!g.depth,"source work waits for actual screen");screenDraw(0);screenDraw(1);screenMotionFrameBoundary();
    sourceDraw();screenDraw(0);screenDraw(1);check(!screenMotionView(0,W,H),"first source frame has no invented history");
    screenMotionFrameBoundary();source[275][0]=.1f;sourceDraw();
    // The game clears depth later in the frame: read completed depth now,
    // not a premature copy at the first terrain draw.
    ctx->ClearDepthStencilView(ds.Get(),D3D11_CLEAR_DEPTH,.005f,0);screenDraw(0);screenDraw(1);
    check(screenMotionView(0,W,H) && screenMotionView(1,W,H),"independent eye maps available");
    for(auto& e:g.eyes){auto a=read(dev.Get(),ctx.Get(),e.map.Get());for(unsigned i=0;i<W*H;++i)if(a[i*4+3]==1){check(std::fabs(a[i*4]-.64f)<.001f,"walking uses completed depth and source camera");check(std::fabs(a[i*4+1])<.001f,"walking X does not move Y");}}
    screenMotionFrameBoundary();check(!screenMotionView(0,W,H),"map cannot outlive its frame");
    screenMotionFrameBoundary();sourceDraw();screenDraw(0);screenDraw(1);check(!screenMotionView(0,W,H),"missing screen frame breaks history");
    // Original source UI alpha (including discard), straight/premultiplied
    // blend contracts, source identity and per-frame mask lifetime.
    {
        screenMotionFrameBoundary();source[275][0]+=.1f;sourceDraw();
        auto uc=compile("float4 main(float4 p:SV_Position):SV_Target{if(p.x>48)discard;return float4(0,1,0,p.x<16?0:p.x<32?.25:1);}","ps_5_0");
        ComPtr<ID3D11PixelShader> up;hr(dev->CreatePixelShader(uc->GetBufferPointer(),uc->GetBufferSize(),nullptr,&up));
        D3D11_BLEND_DESC ub{};auto& r=ub.RenderTarget[0];r.BlendEnable=TRUE;r.SrcBlend=D3D11_BLEND_SRC_ALPHA;r.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;r.BlendOp=D3D11_BLEND_OP_ADD;
        r.SrcBlendAlpha=D3D11_BLEND_ONE;r.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;r.BlendOpAlpha=D3D11_BLEND_OP_ADD;r.RenderTargetWriteMask=7;
        ComPtr<ID3D11BlendState> blend;hr(dev->CreateBlendState(&ub,&blend));
        D3D11_DEPTH_STENCIL_DESC ud{};ud.DepthFunc=D3D11_COMPARISON_ALWAYS;
        ComPtr<ID3D11DepthStencilState> uds;hr(dev->CreateDepthStencilState(&ud,&uds));
        ctx->PSSetShader(up.Get(),nullptr,0);ctx->OMSetBlendState(blend.Get(),nullptr,~0u);ctx->OMSetDepthStencilState(uds.Get(),14);
        const float live[4]={.125f,.25f,.5f,1};ctx->ClearRenderTargetView(rt.Get(),live);auto liveBefore=read(dev.Get(),ctx.Get(),colour.Get());
        auto uiDraw=[&](){screenMotionUiDraw(ctx.Get(),draw,6,1,0,0,0);};
        testVs=0xB10B032BDFD46700ull;testPs=0;uiDraw();check(!g.ui,"unknown material cannot allocate or mark source UI");
        testPs=0xDB899F4BD577F2E5ull;uiDraw();check(g.uiDraws==1,"known source GUI draw captured");
        auto coverage=read(dev.Get(),ctx.Get(),g.ui.Get());
        for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x)check(std::fabs(coverage[y*W+x]-(x<16||x>=48?1:x<32?191/255.f:0))<1e-6,"original alpha and discard determine coverage, independent of RGB");
        uiDraw();coverage=read(dev.Get(),ctx.Get(),g.ui.Get());check(std::fabs(coverage[20]-143/255.f)<1e-6,"overlapping source UI accumulates opacity");
        ComPtr<ID3D11RenderTargetView> restored;ComPtr<ID3D11DepthStencilView> restoredDepth;ctx->OMGetRenderTargets(1,&restored,&restoredDepth);
        check(restored.Get()==rt.Get() && restoredDepth.Get()==ds.Get(),"UI reissue restores original targets");
        ComPtr<ID3D11BlendState> restoredBlend;FLOAT f[4];UINT bits;ctx->OMGetBlendState(&restoredBlend,f,&bits);check(restoredBlend.Get()==blend.Get() && bits==~0u,"UI reissue restores blend and sample mask");
        ComPtr<ID3D11DepthStencilState> restoredDs;UINT ref;ctx->OMGetDepthStencilState(&restoredDs,&ref);check(restoredDs.Get()==uds.Get() && ref==14,"UI reissue restores depth/stencil state");
        ComPtr<ID3D11PixelShader> restoredPs;ctx->PSGetShader(&restoredPs,nullptr,nullptr);check(restoredPs.Get()==up.Get(),"UI reissue uses and preserves original shader");
        for(float z:read(dev.Get(),ctx.Get(),depth.Get()))check(std::fabs(z-.0025f)<1e-6,"UI coverage cannot write game depth");
        check(read(dev.Get(),ctx.Get(),colour.Get())==liveBefore,"UI coverage cannot change source colour");
        for(auto pair:{std::pair{0xB10B032BDFD46700ull,0xDB899F4BD577F2E5ull},std::pair{0xC4B4B334B26E81A9ull,0x0146ABCC53240479ull},std::pair{0xA888D51024D9798Eull,0x015EF9349EC097E8ull}}){
            testVs=pair.first;testPs=pair.second;ub.RenderTarget[0].SrcBlend=D3D11_BLEND_ONE;
            ComPtr<ID3D11BlendState> premult;hr(dev->CreateBlendState(&ub,&premult));ctx->OMSetBlendState(premult.Get(),nullptr,~0u);
            unsigned before=g.uiDraws;uiDraw();check(g.uiDraws==before+1,"all observed GUI pairs support premultiplied alpha");
        }
        ud.DepthEnable=TRUE;ud.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
        ComPtr<ID3D11DepthStencilState> worldDs;hr(dev->CreateDepthStencilState(&ud,&worldDs));ctx->OMSetDepthStencilState(worldDs.Get(),0);
        unsigned before=g.uiDraws;uiDraw();check(g.uiDraws==before,"depth-tested world draw cannot claim source UI");ctx->OMSetDepthStencilState(uds.Get(),14);
        ctx->OMSetRenderTargets(1,er[0].GetAddressOf(),nullptr);uiDraw();check(g.uiDraws==before,"different same-size target cannot contaminate source coverage");ctx->OMSetRenderTargets(1,rt.GetAddressOf(),ds.Get());
        ctx->PSSetShader(ps.Get(),nullptr,0);screenDraw(0);screenDraw(1);
        for(auto& e:g.eyes){auto a=read(dev.Get(),ctx.Get(),e.map.Get());for(UINT y=1;y+1<H;++y)for(UINT x=2;x+2<W;++x){unsigned i=(y*W+x)*4;
            if(x>=18&&x<=46){check(a[i+3]==3,"source UI survives projection into both eye maps");check(std::fabs(a[i])<.001f,"source UI does not receive scenery translation");}
            if(x<14||x>50){check(a[i+3]==1,"transparent source/UI discard retains scenery");check(std::fabs(a[i]-.32f)<.001f,"scenery still receives source camera translation");}
        }}
        screenMotionFrameBoundary();sourceDraw();screenDraw(0);
        auto a=read(dev.Get(),ctx.Get(),g.eyes[0].map.Get());for(UINT i=0;i<W*H;++i)check(a[i*4+3]!=3,"absent source UI cannot retain stale coverage");
        screenMotionFrameBoundary();sourceDraw();ctx->PSSetShader(up.Get(),nullptr,0);ctx->OMSetBlendState(blend.Get(),nullptr,~0u);ctx->OMSetDepthStencilState(uds.Get(),14);
        testVs=0xA888D51024D9798Eull;testPs=0x015EF9349EC097E8ull;uiDraw();
        coverage=read(dev.Get(),ctx.Get(),g.ui.Get());check(std::fabs(coverage[20]-191/255.f)<1e-6,"new source frame clears previous opacity");
        ctx->PSSetShader(ps.Get(),nullptr,0);screenDraw(0);
    }
    // The game's first-person stencil is independent of surface distance.
    // Exercise both eyes, scenery at the same depth, the previous screen
    // transform, the live toggle and replacement of a stencil-less source.
    {
        td.Format=DXGI_FORMAT_R32G8X24_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
        depth.Reset();ds.Reset();hr(dev->CreateTexture2D(&td,nullptr,&depth));dd.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;hr(dev->CreateDepthStencilView(depth.Get(),&dd,&ds));
        auto motionDesc=td;motionDesc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;motionDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        std::vector<float> motion(W*H*4);for(UINT i=0;i<W*H;++i){motion[i*4]=2;motion[i*4+2]=.0025f;motion[i*4+3]=1;}
        D3D11_SUBRESOURCE_DATA mi{};mi.pSysMem=motion.data();mi.SysMemPitch=W*16;
        ComPtr<ID3D11Texture2D> mt;ComPtr<ID3D11ShaderResourceView> mv;hr(dev->CreateTexture2D(&motionDesc,&mi,&mt));hr(dev->CreateShaderResourceView(mt.Get(),nullptr,&mv));
        testWeaponMotion=mv.Get();
        for(int pass=0;pass<4;++pass){
            screenMotionFrameBoundary();source[275][0]+=.1f;sourceDraw();
            check(bool(g.stencilSrv),"stencil SRV created from source depth");
            ctx->ClearDepthStencilView(ds.Get(),D3D11_CLEAR_STENCIL,1,pass==2?4:20);
            g.weapon=pass!=1;
            if(pass==2)model[9][3]=.25f;
            // A bound t11 from another draw must survive our use of it.
            ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->PSSetShaderResources(11,1,csrv.GetAddressOf());
            screenDraw(0);screenDraw(1);
            ComPtr<ID3D11ShaderResourceView> restored;ctx->PSGetShaderResources(11,1,&restored);check(restored.Get()==csrv.Get(),"original stencil-slot binding restored");
            for(auto& e:g.eyes){auto a=read(dev.Get(),ctx.Get(),e.map.Get());UINT valid=0;for(UINT i=0;i<W*H;++i){
                if(a[i*4+3]!=1)continue;
                ++valid;
                check(std::fabs(a[i*4]-(pass==3?10:pass==1||pass==2?.32f:2))<.001f,"animated weapon motion composes with screen; Off and scenery retain world motion");
            }check(pass==0 || valid>W*H/2,"weapon vectors actually reach both eye maps");}
            ID3D11ShaderResourceView* none=nullptr;ctx->PSSetShaderResources(11,1,&none);
        }
        // Keep default fixture path unchanged, including missing-stencil fallback.
        testWeaponMotion=nullptr;screenMotionFrameBoundary();sourceDraw();ctx->ClearDepthStencilView(ds.Get(),D3D11_CLEAR_STENCIL,1,20);screenDraw(0);
        auto rejected=read(dev.Get(),ctx.Get(),g.eyes[0].map.Get());for(UINT i=0;i<W*H;++i)check(rejected[i*4+3]!=1,"missing weapon motion rejects history instead of assuming fixed UV");
        ID3D11ShaderResourceView* none=nullptr;ctx->PSSetShaderResources(11,1,&none);
        model[9][3]=0;td.Format=DXGI_FORMAT_R32_TYPELESS;depth.Reset();ds.Reset();hr(dev->CreateTexture2D(&td,nullptr,&depth));dd.Format=DXGI_FORMAT_D32_FLOAT;hr(dev->CreateDepthStencilView(depth.Get(),&dd,&ds));
        screenMotionFrameBoundary();sourceDraw();check(!g.stencilSrv,"new source without stencil drops old mask");screenDraw(0);
    }
    // Optional recorded source/eye matrices and double-precision expected
    // projection. No proprietary assets are committed with the test.
    if(argc>1 && std::strcmp(argv[1],"--self-test")!=0){
        std::ifstream f(argv[1],std::ios::binary);UINT n=0;f.read(reinterpret_cast<char*>(&n),4);check(n>0 && n<10000,"fixture count");
        auto uvCb=buf(16,D3D11_BIND_CONSTANT_BUFFER);auto fixedCode=compile("cbuffer U:register(b7){float4 uv;}struct O{float2 t:__USER_VERTEX_M_TEXCOORD0;float4 p:SV_Position;};O main(uint id:SV_VertexID){O o;float2 p=float2((id<<1)&2,id&2);o.p=float4(p*float2(2,-2)+float2(-1,1),0,1);o.t=uv.xy;return o;}","vs_5_0");
        ComPtr<ID3D11VertexShader> fixed;hr(dev->CreateVertexShader(fixedCode->GetBufferPointer(),fixedCode->GetBufferSize(),nullptr,&fixed));ctx->VSSetShader(fixed.Get(),nullptr,0);ctx->VSSetConstantBuffers(7,1,uvCb.GetAddressOf());
        auto oldCb=buf(sizeof(source),D3D11_BIND_CONSTANT_BUFFER);auto& e=g.eyes[0];
        D3D11_TEXTURE2D_DESC ft{};e.map->GetDesc(&ft);ft.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        ComPtr<ID3D11Texture2D> precise;ComPtr<ID3D11RenderTargetView> preciseRtv;
        hr(dev->CreateTexture2D(&ft,nullptr,&precise));hr(dev->CreateRenderTargetView(precise.Get(),nullptr,&preciseRtv));
        ID3D11Buffer* cb[5]={sc.Get(),oldCb.Get(),mc.Get(),ec.Get(),e.settings.Get()};ctx->PSSetConstantBuffers(2,5,cb);
        ctx->PSSetShader(g.ps.Get(),nullptr,0);ctx->OMSetRenderTargets(1,preciseRtv.GetAddressOf(),nullptr);ctx->OMSetBlendState(g.blend.Get(),nullptr,~0u);ctx->OMSetDepthStencilState(g.ds.Get(),0);
        ID3D11ShaderResourceView* srv[2]={g.depthSrv.Get(),e.sizeSrv[0].Get()};ctx->PSSetShaderResources(8,2,srv);
        float old[276][4]{},settings[8]{},uv[4]{},expected[2]{};
        for(UINT k=0;k<n;++k){
            f.read(reinterpret_cast<char*>(source),sizeof(source));f.read(reinterpret_cast<char*>(old),sizeof(old));f.read(reinterpret_cast<char*>(model),sizeof(model));f.read(reinterpret_cast<char*>(camera),sizeof(camera));
            f.read(reinterpret_cast<char*>(settings),sizeof(settings));f.read(reinterpret_cast<char*>(size),sizeof(size));f.read(reinterpret_cast<char*>(uv),sizeof(uv));f.read(reinterpret_cast<char*>(expected),sizeof(expected));check(bool(f),"fixture complete");
            ctx->UpdateSubresource(sc.Get(),0,nullptr,source,0,0);ctx->UpdateSubresource(oldCb.Get(),0,nullptr,old,0,0);ctx->UpdateSubresource(mc.Get(),0,nullptr,model,0,0);ctx->UpdateSubresource(ec.Get(),0,nullptr,camera,0,0);ctx->UpdateSubresource(e.settings.Get(),0,nullptr,settings,0,0);ctx->UpdateSubresource(uvCb.Get(),0,nullptr,uv,0,0);
            float sized[4]={size[0],size[1],0,0};ctx->UpdateSubresource(e.sizes[0].Get(),0,nullptr,sized,0,0);ctx->ClearDepthStencilView(ds.Get(),D3D11_CLEAR_DEPTH,uv[2],0);ctx->Draw(3,0);
            auto a=read(dev.Get(),ctx.Get(),precise.Get());
            if(!(a[3]==1 && std::fabs(a[0]-expected[0])<.005f && std::fabs(a[1]-expected[1])<.005f))std::printf("fixture %u actual %g,%g,%g expected %g,%g\n",k,a[0],a[1],a[3],expected[0],expected[1]);
            check(a[3]==1 && std::fabs(a[0]-expected[0])<.005f && std::fabs(a[1]-expected[1])<.005f,"captured camera/depth/curved-screen correspondence");
        }
    }
    ctx->ClearState();screenMotionShutdown();
    testScreenConsumers(dev.Get(),ctx.Get());
    if(queue)for(UINT64 i=0;i<queue->GetNumStoredMessages();++i){SIZE_T messageBytes=0;queue->GetMessage(i,nullptr,&messageBytes);std::vector<unsigned char> b(messageBytes);auto* m=reinterpret_cast<D3D11_MESSAGE*>(b.data());queue->GetMessage(i,m,&messageBytes);if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::puts(m->pDescription);check(false,"D3D debug layer clean");}}
    std::printf("PASS: %u screen motion checks.\n",checks);
    return 0;
}
