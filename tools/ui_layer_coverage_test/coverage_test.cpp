// Production coverage command recording and HLSL on D3D11 WARP.
#include "../../src/d3d11/ui_layer_coverage.h"
#include "../../src/d3d11/ui_layer_shaders.h"
#include "temporal_shader_bytecode.h"
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <array>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
using namespace edvr;
unsigned checks = 0;
void check(bool ok, const char* text) { ++checks; if (!ok) { std::fprintf(stderr,"FAIL: %s\n",text); std::exit(1); } }
void hr(HRESULT result) { check(SUCCEEDED(result),"D3D operation"); }
ComPtr<ID3DBlob> compile(const char* text, const char* profile, UINT flags=0) {
    ComPtr<ID3DBlob> code, error;
    // The live flags-zero path executes the generated payload. The
    // skip-optimization fixture below remains an independent HLSL variant.
    if(flags==0 && (text==kUiLayerCoverageVsHlsl || text==kUiLayerCoveragePsHlsl)) {
        const void* bytes=text==kUiLayerCoverageVsHlsl?static_cast<const void*>(kUiLayerCoverageVsBytecode):kUiLayerCoveragePsBytecode;
        const size_t size=text==kUiLayerCoverageVsHlsl?sizeof(kUiLayerCoverageVsBytecode):sizeof(kUiLayerCoveragePsBytecode);
        hr(D3DCreateBlob(size,&code));std::memcpy(code->GetBufferPointer(),bytes,size);return code;
    }
    const HRESULT result = D3DCompile(text,std::strlen(text),nullptr,nullptr,nullptr,"main",profile,flags,0,&code,&error);
    if (FAILED(result) && error) std::fprintf(stderr,"%s\n",static_cast<const char*>(error->GetBufferPointer()));
    hr(result);return code;
}
struct FinishSpy {
    using Fn = HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,BOOL,ID3D11CommandList**);
    inline static FinishSpy* active = nullptr;
    FinishSpy* previous;ID3D11DeviceContext* ctx;void** original;void* slots[115];unsigned calls = 0;bool failNext = false;
    static HRESULT STDMETHODCALLTYPE finish(ID3D11DeviceContext* c,BOOL restore,ID3D11CommandList** out) {
        FinishSpy* self=active;while(self && self->ctx!=c)self=self->previous;check(self!=nullptr,"Finish spy context found");
        ++self->calls;const HRESULT result = reinterpret_cast<Fn>(self->original[114])(c,restore,out);
        if (self->failNext) { self->failNext=false;if(*out){(*out)->Release();*out=nullptr;}return E_OUTOFMEMORY; }
        return result;
    }
    void setTable(void** table) { DWORD old=0,ignored=0;hr(HRESULT_FROM_WIN32(VirtualProtect(ctx,sizeof(void*),PAGE_READWRITE,&old)?0:GetLastError()));
        *reinterpret_cast<void***>(ctx)=table;check(VirtualProtect(ctx,sizeof(void*),old,&ignored)!=0,"spy protection restored"); }
    explicit FinishSpy(ID3D11DeviceContext* c):previous(active),ctx(c),original(*reinterpret_cast<void***>(c)) {
        std::memcpy(slots,original,sizeof(slots));slots[114]=reinterpret_cast<void*>(&finish);active=this;setTable(slots);
    }
    ~FinishSpy(){setTable(original);active=previous;}
};
unsigned executed = 0;
void executeRaw(ID3D11DeviceContext* ctx, ID3D11CommandList* list, int restore) {
    check(restore==1,"immediate state restoration requested");++executed;ctx->ExecuteCommandList(list,restore);
}
struct Fixture {
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx,deferred;
    ComPtr<ID3D11Texture2D> hdr,out,sentinel;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv,sentinelRtv;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11BlendState> blend,sentinelBlend;
    ComPtr<ID3D11RasterizerState> sentinelRs;
    Fixture() {
        D3D_FEATURE_LEVEL level;hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx));
        hr(dev->CreateDeferredContext(0,&deferred));
        D3D11_TEXTURE2D_DESC d{};d.Width=8;d.Height=4;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        hr(dev->CreateTexture2D(&d,nullptr,&hdr));hr(dev->CreateShaderResourceView(hdr.Get(),nullptr,&srv));
        d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET;
        hr(dev->CreateTexture2D(&d,nullptr,&out));hr(dev->CreateRenderTargetView(out.Get(),nullptr,&rtv));
        hr(dev->CreateTexture2D(&d,nullptr,&sentinel));hr(dev->CreateRenderTargetView(sentinel.Get(),nullptr,&sentinelRtv));
        auto v=compile(kUiLayerCoverageVsHlsl,"vs_5_0"),p=compile(kUiLayerCoveragePsHlsl,"ps_5_0");
        hr(dev->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs));
        hr(dev->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps));
        UiBlendRt alpha;alpha.enable=false;alpha.mask=uiblend::kWriteAlpha;auto bd=uiLayerBlendDesc(alpha);hr(dev->CreateBlendState(&bd,&blend));
        bd.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;hr(dev->CreateBlendState(&bd,&sentinelBlend));
        D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;rs.ScissorEnable=TRUE;
        hr(dev->CreateRasterizerState(&rs,&sentinelRs));
    }
    UiCoverageBindings bindings() {return {srv.Get(),rtv.Get(),vs.Get(),ps.Get(),blend.Get(),8,4};}
    void content(unsigned frame) {
        std::array<uint16_t,8*4*4> pixels{};
        for(unsigned i=0;i<32;++i)for(unsigned c=0;c<4;++c)
            pixels[4*i+c]=DirectX::PackedVector::XMConvertFloatToHalf(c==3?float((i+frame)%5)/4.0f:float(c+1));
        ctx->UpdateSubresource(hdr.Get(),0,nullptr,pixels.data(),8*8,0);
        const float clear[4]={.25f,.5f,.75f,.125f};ctx->ClearRenderTargetView(rtv.Get(),clear);
    }
    void bindSentinel() {
        ID3D11RenderTargetView* rt=sentinelRtv.Get();ctx->OMSetRenderTargets(1,&rt,nullptr);
        ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);
        ID3D11ShaderResourceView* view=srv.Get();ctx->PSSetShaderResources(0,1,&view);ctx->PSSetShaderResources(5,1,&view);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        const float factors[4]={.1f,.2f,.3f,.4f};ctx->OMSetBlendState(sentinelBlend.Get(),factors,0x2468u);
        ctx->RSSetState(sentinelRs.Get());D3D11_VIEWPORT vp{1,2,5,2,.2f,.8f};ctx->RSSetViewports(1,&vp);
        D3D11_RECT rect{1,1,5,3};ctx->RSSetScissorRects(1,&rect);
    }
    void state() {
        ComPtr<ID3D11RenderTargetView> rt;ctx->OMGetRenderTargets(1,&rt,nullptr);check(rt.Get()==sentinelRtv.Get(),"RTV restored");
        ComPtr<ID3D11VertexShader> v;ComPtr<ID3D11PixelShader> p;ctx->VSGetShader(&v,nullptr,nullptr);ctx->PSGetShader(&p,nullptr,nullptr);
        check(v.Get()==vs.Get() && p.Get()==ps.Get(),"VS/PS restored");
        ComPtr<ID3D11ShaderResourceView> s;ctx->PSGetShaderResources(5,1,&s);check(s.Get()==srv.Get(),"unmodified SRV preserved");
        s.Reset();ctx->PSGetShaderResources(0,1,&s);check(s.Get()==srv.Get(),"coverage SRV slot restored");
        D3D11_PRIMITIVE_TOPOLOGY topo;ctx->IAGetPrimitiveTopology(&topo);check(topo==D3D11_PRIMITIVE_TOPOLOGY_LINELIST,"IA topology restored");
        ComPtr<ID3D11BlendState> b;float factors[4];UINT mask;ctx->OMGetBlendState(&b,factors,&mask);
        check(b.Get()==sentinelBlend.Get() && mask==0x2468u && factors[0]==.1f && factors[3]==.4f,"blend factors/mask restored");
        ComPtr<ID3D11RasterizerState> r;ctx->RSGetState(&r);check(r.Get()==sentinelRs.Get(),"rasterizer restored");
        D3D11_VIEWPORT vp{};UINT n=1;ctx->RSGetViewports(&n,&vp);check(n==1 && vp.TopLeftX==1 && vp.Width==5 && vp.MinDepth==.2f,"viewport restored");
        D3D11_RECT rect{};n=1;ctx->RSGetScissorRects(&n,&rect);check(n==1 && rect.left==1 && rect.right==5,"scissor restored");
    }
    std::vector<uint8_t> read(ID3D11Texture2D* texture=nullptr) {
        if(!texture)texture=out.Get();
        D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;hr(dev->CreateTexture2D(&d,nullptr,&stage));ctx->CopyResource(stage.Get(),texture);
        D3D11_MAPPED_SUBRESOURCE m{};hr(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&m));std::vector<uint8_t> bytes(8*4*4);
        for(unsigned y=0;y<4;++y)std::memcpy(bytes.data()+y*32,static_cast<uint8_t*>(m.pData)+y*m.RowPitch,32);
        ctx->Unmap(stage.Get(),0);return bytes;
    }
};
int main(int argc,char** argv) {
    if(argc>1 && std::strcmp(argv[1],"--dry-run")==0){std::puts("ui_layer_coverage_test: dry-run; no GPU work or files");return 0;}
    if(argc>1 && std::strcmp(argv[1],"--self-test")!=0){std::fprintf(stderr,"usage: --self-test | --dry-run\n");return 2;}
    Fixture f;FinishSpy spy(f.deferred.Get());std::array<std::vector<uint8_t>,4> baseline;
    for(unsigned frame=0;frame<4;++frame) {
        f.content(frame);f.bindSentinel();check(uiCoverageExecuteUncached(f.ctx.Get(),f.deferred.Get(),f.bindings(),executeRaw),"uncached production coverage executes");
        f.state();const auto bytes=f.read();baseline[frame]=bytes;
        for(unsigned i=0;i<32;++i){check(bytes[4*i]==64 && bytes[4*i+1]==128 && bytes[4*i+2]==191,"coverage preserves exact RGB");
            const uint8_t alpha[] = {0,64,128,191,255};
            check(bytes[4*i+3]==alpha[(i+frame)%5],"coverage uses current HDR alpha");}
    }
    std::printf("coverage baseline: %u FinishCommandList calls / 4 executions\n",spy.calls);
    check(spy.calls==4 && executed==4,"uncached command count measured");
    UiCoverageCommandCache cache;const unsigned before=spy.calls;
    for(unsigned frame=0;frame<4;++frame) {
        f.content(frame);f.bindSentinel();check(cache.execute(f.ctx.Get(),f.deferred.Get(),f.bindings(),executeRaw),"cached production coverage executes");
        f.state();check(f.read()==baseline[frame],"old/new exact output with dynamically updated source");
    }
    check(spy.calls-before==1 && executed==8,"four cached executions record only once");
    std::printf("coverage cached: %u FinishCommandList calls / 4 executions; dynamic-source exact output\n",spy.calls-before);
    auto b=f.bindings();
    auto mutation=[&](const UiCoverageBindings& changed,const char* why) {
        const unsigned count=spy.calls;f.content(1);f.bindSentinel();
        check(cache.execute(f.ctx.Get(),f.deferred.Get(),changed,executeRaw),why);
        check(spy.calls==count+1,"binding change records once");f.state();
        f.content(2);check(cache.execute(f.ctx.Get(),f.deferred.Get(),changed,executeRaw),"mutated binding reused");
        check(spy.calls==count+1,"unchanged mutated binding does not record again");
        auto expected=baseline[2];
        for(unsigned y=0;y<4;++y)for(unsigned x=0;x<8;++x)
            if(x>=changed.width || y>=changed.height)expected[4*(8*y+x)+3]=32;
        check(f.read()==expected,"mutated coverage preserves exact current output and viewport");
    };
    ComPtr<ID3D11ShaderResourceView> source2;hr(f.dev->CreateShaderResourceView(f.hdr.Get(),nullptr,&source2));
    check(source2.Get()!=b.source,"replacement source view has distinct identity");b.source=source2.Get();mutation(b,"source view identity invalidates");
    check(f.read()==baseline[2],"replacement source samples latest pixels");
    ComPtr<ID3D11RenderTargetView> target2;hr(f.dev->CreateRenderTargetView(f.out.Get(),nullptr,&target2));
    check(target2.Get()!=b.target,"replacement target view has distinct identity");b.target=target2.Get();mutation(b,"target view identity invalidates");
    check(f.read()==baseline[2],"replacement target preserves exact output");
    b.width=4;mutation(b,"width invalidates");b.height=2;mutation(b,"height invalidates");
    b.width=8;b.height=4;mutation(b,"restored dimensions invalidate");
    auto vc=compile(kUiLayerCoverageVsHlsl,"vs_5_0",D3DCOMPILE_SKIP_OPTIMIZATION);
    auto pc=compile(kUiLayerCoveragePsHlsl,"ps_5_0",D3DCOMPILE_SKIP_OPTIMIZATION);
    ComPtr<ID3D11VertexShader> vs2;ComPtr<ID3D11PixelShader> ps2;
    hr(f.dev->CreateVertexShader(vc->GetBufferPointer(),vc->GetBufferSize(),nullptr,&vs2));
    hr(f.dev->CreatePixelShader(pc->GetBufferPointer(),pc->GetBufferSize(),nullptr,&ps2));
    check(vs2.Get()!=b.vs && ps2.Get()!=b.ps,"replacement shader identities differ");
    b.vs=vs2.Get();mutation(b,"VS identity invalidates");b.ps=ps2.Get();mutation(b,"PS identity invalidates");
    check(f.read()==baseline[2],"equivalent replacement shaders preserve exact output");
    D3D11_BLEND_DESC bd{};f.blend->GetDesc(&bd);bd.IndependentBlendEnable=TRUE;
    ComPtr<ID3D11BlendState> blend2;hr(f.dev->CreateBlendState(&bd,&blend2));
    check(blend2.Get()!=b.blend,"replacement blend identity differs");b.blend=blend2.Get();mutation(b,"blend identity invalidates");
    check(f.read()==baseline[2],"equivalent replacement blend preserves exact output");
    { UiCoverageCommandCache left,right;auto leftB=f.bindings(),rightB=f.bindings();
        rightB.source=source2.Get();rightB.target=target2.Get();const unsigned eyeBefore=spy.calls;
        for(unsigned frame=0;frame<4;++frame)for(unsigned eye=0;eye<2;++eye) {
            f.content(frame);f.bindSentinel();
            auto& eyeCache=eye?right:left;const auto& eyeBindings=eye?rightB:leftB;
            check(eyeCache.execute(f.ctx.Get(),f.deferred.Get(),eyeBindings,executeRaw),"alternating Eye cache executes");
            check(f.read()==baseline[frame],"alternating Eye exact dynamic output");f.state();
        }
        check(spy.calls-eyeBefore==2,"two alternating Eyes record twice for eight executions");
        std::printf("coverage alternating Eyes: %u FinishCommandList calls / 8 executions\n",spy.calls-eyeBefore);
    }
    const unsigned resetBefore=spy.calls;cache.reset();check(!cache.hasCommands(),"reset releases cached commands");
    check(cache.execute(f.ctx.Get(),f.deferred.Get(),b,executeRaw),"reset records again");check(spy.calls==resetBefore+1,"reset invalidates list");
    // Failure must not execute old commands for new bindings, and must retry.
    b.source=f.srv.Get();spy.failNext=true;const unsigned failedBefore=executed,finishBefore=spy.calls;
    check(!cache.execute(f.ctx.Get(),f.deferred.Get(),b,executeRaw),"Finish failure reported");
    check(!cache.hasCommands() && executed==failedBefore && spy.calls==finishBefore+1,"failed recording leaves no stale executable list");
    f.content(3);check(cache.execute(f.ctx.Get(),f.deferred.Get(),b,executeRaw),"failed recording retries next pass");
    check(spy.calls==finishBefore+2 && f.read()==baseline[3],"retry records and renders current source exactly");
    ComPtr<ID3D11DeviceContext> deferred2;hr(f.dev->CreateDeferredContext(0,&deferred2));
    {FinishSpy otherSpy(deferred2.Get());check(cache.execute(f.ctx.Get(),deferred2.Get(),b,executeRaw),"deferred identity change records");
        check(cache.execute(f.ctx.Get(),deferred2.Get(),b,executeRaw),"new deferred identity reused");check(otherSpy.calls==1,"deferred replacement records once");}
    Fixture other;FinishSpy deviceSpy(other.deferred.Get());other.content(0);other.bindSentinel();
    check(cache.execute(other.ctx.Get(),other.deferred.Get(),other.bindings(),executeRaw),"device change records own resources");
    check(deviceSpy.calls==1 && other.read()==baseline[0],"new device exact output and recording count");other.state();
    const unsigned wrongBefore=executed;
    check(!cache.execute(other.ctx.Get(),other.deferred.Get(),b,executeRaw),"foreign device resources refused before recording");
    check(!cache.hasCommands() && executed==wrongBefore && deviceSpy.calls==1,"foreign resources clear old cache without commands");
    check(cache.execute(other.ctx.Get(),other.deferred.Get(),other.bindings(),executeRaw),"correct resources retry after refusal");
    check(deviceSpy.calls==2,"device refusal forces fresh valid recording");
    auto invalid=other.bindings();invalid.source=nullptr;
    check(!cache.execute(other.ctx.Get(),other.deferred.Get(),invalid,executeRaw) && !cache.hasCommands(),"missing view clears cache");
    cache.reset();
    // Explicit key references keep caller resources alive, and reset releases
    // them as well as the command list before Eye replacement/shutdown.
    auto refs=[](IUnknown* object){object->AddRef();return object->Release();};
    Fixture lifetime;UiCoverageCommandCache held;
    const ULONG sourceRefs=refs(lifetime.srv.Get()),targetRefs=refs(lifetime.rtv.Get());
    lifetime.content(0);check(held.execute(lifetime.ctx.Get(),lifetime.deferred.Get(),lifetime.bindings(),executeRaw),"lifetime cache executes");
    check(refs(lifetime.srv.Get())>sourceRefs && refs(lifetime.rtv.Get())>targetRefs,"cache retains both views");
    held.reset();check(refs(lifetime.srv.Get())==sourceRefs && refs(lifetime.rtv.Get())==targetRefs,"reset releases view references");
    auto retained=lifetime.bindings();
    check(held.execute(lifetime.ctx.Get(),lifetime.deferred.Get(),retained,executeRaw),"lifetime cache rebuilt");
    lifetime.srv.Reset();lifetime.rtv.Reset();
    check(held.execute(lifetime.ctx.Get(),lifetime.deferred.Get(),retained,executeRaw),"cached resources survive caller reference release");
    check(lifetime.read()==baseline[0],"retained resources still produce exact output");held.reset();
    std::printf("ui_layer_coverage_test: %u checks passed\n",checks);return 0;
}
