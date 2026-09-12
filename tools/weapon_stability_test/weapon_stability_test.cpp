// Exercise the production GPU attachment correction, including its draw gate
// and state restoration. Optional --capture DIR replays exported flight inputs.
#include "../../src/d3d11/weapon_stability.cpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>
#include <fstream>
#include <filesystem>
using Microsoft::WRL::ComPtr;
unsigned checks=0,draws=0;ID3D11ShaderResourceView* drawnPool=nullptr;
void check(bool b,const char* label){++checks;if(!b){std::printf("FAIL: %s\n",label);std::exit(1);}}
void hr(HRESULT h){check(SUCCEEDED(h),"D3D operation");}
ComPtr<ID3DBlob> compile(const char* s,const char* entry){
    ComPtr<ID3DBlob> c,e;HRESULT h=D3DCompile(s,strlen(s),nullptr,nullptr,nullptr,entry,"cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&c,&e);
    if(FAILED(h)&&e)std::puts(static_cast<const char*>(e->GetBufferPointer()));hr(h);return c;
}
namespace edvr {
bool testEnabled=true;
Config& Config::get(){static Config cfg;return cfg;}
bool Config::getBool(const char* key,bool def) const {
    check(!strcmp(key,"fix.weapon_stability") && def,"weapon stability config key defaults on");return testEnabled;
}
uint64_t testVs=0,testPs=0;ID3D11RenderTargetView* testRtv=nullptr;
Log& Log::get(){static Log l;return l;}Log::~Log()=default;void Log::note(const char*,...){}
void* bindingGet(BindSlot s){return s==BindSlot::Rtv0?testRtv:nullptr;}
uint64_t bindingShaderHash(BindSlot s){return s==BindSlot::Vs?testVs:s==BindSlot::Ps?testPs:0;}
bool bindingResolve(void* view,ResourceInfo* info){
    if(!view)return false;ComPtr<ID3D11Resource> r;static_cast<ID3D11RenderTargetView*>(view)->GetResource(&r);
    ComPtr<ID3D11Texture2D> t;if(FAILED(r.As(&t)))return false;D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);
    info->isTexture2D=true;info->a=d.Width;info->b=d.Height;return true;
}
ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext* ctx,const char* s,size_t,const char* entry,const char*,const SwapMacro*,const char*){
    auto code=compile(s,entry);ComPtr<ID3D11Device> d;ctx->GetDevice(&d);ID3D11ComputeShader* p=nullptr;
    hr(d->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&p));return p;
}
}
using namespace edvr;
void __stdcall draw(ID3D11DeviceContext* ctx,unsigned n,unsigned instances,unsigned start,int base,unsigned si){
    check(n==24 && instances==1 && start==3 && base==4 && si==5,"original draw arguments preserved");
    ComPtr<ID3D11ShaderResourceView> p;ctx->VSGetShaderResources(33,1,&p);drawnPool=p.Get();++draws;
}
struct Instance {uint32_t words[84]{};};
static_assert(sizeof(Instance)==336);
void setFloat(Instance& r,unsigned word,float v){memcpy(&r.words[word],&v,4);}
float getFloat(const Instance& r,unsigned word){float f;memcpy(&f,&r.words[word],4);return f;}
void position(Instance& r,float x,float y,float z){setFloat(r,4,x);setFloat(r,5,y);setFloat(r,6,z);setFloat(r,1,1);}
struct Harness {
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;ComPtr<ID3D11InfoQueue> queue;
    ComPtr<ID3D11Texture2D> rtTex;ComPtr<ID3D11RenderTargetView> rt;
    ComPtr<ID3D11Buffer> pool,bones,camera;ComPtr<ID3D11ShaderResourceView> ps,bs;
    Harness(){
        D3D_FEATURE_LEVEL fl;HRESULT h=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);
        if(h==DXGI_ERROR_SDK_COMPONENT_MISSING)h=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);hr(h);dev.As(&queue);
        D3D11_TEXTURE2D_DESC td{};td.Width=64;td.Height=48;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
        hr(dev->CreateTexture2D(&td,nullptr,&rtTex));hr(dev->CreateRenderTargetView(rtTex.Get(),nullptr,&rt));
        testRtv=rt.Get();ctx->OMSetRenderTargets(1,rt.GetAddressOf(),nullptr);D3D11_VIEWPORT vp{0,0,64,48,0,1};ctx->RSSetViewports(1,&vp);
    }
    ~Harness(){weaponStabilityShutdown();ctx->ClearState();}
    ComPtr<ID3D11Buffer> buffer(const void* data,unsigned bytes,unsigned stride,unsigned bind){
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=bytes;bd.StructureByteStride=stride;bd.BindFlags=bind;bd.MiscFlags=stride?D3D11_RESOURCE_MISC_BUFFER_STRUCTURED:0;
        D3D11_SUBRESOURCE_DATA sd{};sd.pSysMem=data;ComPtr<ID3D11Buffer> p;hr(dev->CreateBuffer(&bd,data?&sd:nullptr,&p));return p;
    }
    void inputs(const void* p,unsigned np,const void* b,unsigned nb,const void* c,unsigned nc){
        pool=buffer(p,np,336,D3D11_BIND_SHADER_RESOURCE);bones=buffer(b,nb,48,D3D11_BIND_SHADER_RESOURCE);camera=buffer(c,nc,0,D3D11_BIND_CONSTANT_BUFFER);
        ps.Reset();bs.Reset();hr(dev->CreateShaderResourceView(pool.Get(),nullptr,&ps));hr(dev->CreateShaderResourceView(bones.Get(),nullptr,&bs));
        ctx->VSSetShaderResources(33,1,ps.GetAddressOf());ctx->VSSetShaderResources(38,1,bs.GetAddressOf());ctx->VSSetConstantBuffers(1,1,camera.GetAddressOf());
    }
    void screen(){testVs=0x5C36AF051B98B9F1ull;testPs=0xCFE84157BC76E921ull;weaponStabilityObserveScreen();testVs=0x8B589D25B2A0ADDCull;testPs=0;}
    bool run(){return weaponStabilityDraw(ctx.Get(),draw,24,1,3,4,5,64,48);}
    std::vector<unsigned char> read(ID3D11Buffer* b){
        D3D11_BUFFER_DESC d{};b->GetDesc(&d);unsigned n=d.ByteWidth;d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=d.StructureByteStride=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;hr(dev->CreateBuffer(&d,nullptr,&staging));ctx->CopyResource(staging.Get(),b);D3D11_MAPPED_SUBRESOURCE m{};hr(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m));
        std::vector<unsigned char> a(n);memcpy(a.data(),m.pData,n);ctx->Unmap(staging.Get(),0);return a;
    }
    void clean(){
        if(queue)for(UINT64 i=0;i<queue->GetNumStoredMessagesAllowedByRetrievalFilter();++i){
            SIZE_T n=0;queue->GetMessage(i,nullptr,&n);std::vector<char> bytes(n);auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());hr(queue->GetMessage(i,m,&n));
            if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::puts(m->pDescription);check(false,"no D3D debug warnings/errors");}
        }
    }
};
void selfTest(){
    Harness h;std::vector<Instance> p(256);float b[16][3][4]{};float c[276][4]{};
    weaponStabilityConfigure(Config::get());
    c[270][0]=c[271][1]=c[272][3]=1;c[273][2]=.025f;c[275][0]=10;c[275][1]=20;c[275][2]=30;
    for(auto& r:p)position(r,100,100,100);
    for(unsigned i:{7u,8u}){b[i][0][0]=b[i][1][1]=b[i][2][2]=1;b[i][1][3]=-1.7f;b[i][2][3]=-.08f;}
    position(p[12],10.04f,20,30);p[12].words[0]=7;p[12].words[2]=123;p[12].words[3]=456;
    p[90]=p[12];p[90].words[0]=8;
    position(p[52],10.2f,20.3f,30.1f);position(p[45],10.04f,19,30);p[45].words[0]=9; // world body, own root
    position(p[46],10.14f,20,30);p[46].words[0]=9; // nearby skinned scenery, not attachment
    auto upload=[&](){h.inputs(p.data(),unsigned(p.size()*sizeof(Instance)),b,sizeof(b),c,sizeof(c));};
    upload();testVs=0x8B589D25B2A0ADDCull;check(!h.run() && !g.fixed,"no correction before on-foot screen");h.screen();
    // Exercise restoration with nonempty CS slots rather than only defaults.
    auto u0=h.buffer(nullptr,64,16,D3D11_BIND_UNORDERED_ACCESS),u1=h.buffer(nullptr,64,16,D3D11_BIND_UNORDERED_ACCESS);
    ComPtr<ID3D11UnorderedAccessView> uv0,uv1;hr(h.dev->CreateUnorderedAccessView(u0.Get(),nullptr,&uv0));hr(h.dev->CreateUnorderedAccessView(u1.Get(),nullptr,&uv1));
    ID3D11UnorderedAccessView* uavs[]={uv0.Get(),uv1.Get()};h.ctx->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
    ID3D11ShaderResourceView* sv[]={h.ps.Get(),h.bs.Get()};h.ctx->CSSetShaderResources(0,2,sv);h.ctx->CSSetConstantBuffers(0,1,h.camera.GetAddressOf());
    auto code=compile("[numthreads(1,1,1)]void main(){}","main");ComPtr<ID3D11ComputeShader> old;hr(h.dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&old));h.ctx->CSSetShader(old.Get(),nullptr,0);
    check(h.run() && draws==1 && drawnPool==g.fixedSrv.Get(),"draw reads private corrected pool exactly once");
    // Separate materials on the same attachment must use the very same pool
    // as its opaque mesh, without another dispatch or another correction.
    for(uint64_t vs:{0xAACFDCF2FB9AD809ull,0x34CCFAAB1EAD90BEull,0x174E8D76363BE337ull,0x025B4B9FF54622EDull,0x7F9B650EC1A1E570ull}) {
        auto* fixed=g.fixed.Get();const unsigned before=draws;testVs=vs;
        check(h.run() && draws==before+1 && drawnPool==g.fixedSrv.Get() && g.fixed.Get()==fixed,"additional material uses shared corrected pool exactly once");
    }
    for(uint64_t vs:{0xB10B032BDFD46700ull,0xC4B4B334B26E81A9ull,0xA888D51024D9798Eull,0xCFCA8FFC6B058630ull,0x88DCF1164C640EC3ull}) {
        testVs=vs;const unsigned before=draws;check(!h.run() && draws==before,"camera-relative UI and full-screen passes stay original");
    }
    testVs=0x8B589D25B2A0ADDCull;
    auto raw=h.read(h.pool.Get()),result=h.read(g.fixed.Get());check(!memcmp(raw.data(),p.data(),raw.size()),"original pool untouched");
    auto expected=p;float dx=c[275][0]-getFloat(p[12],4);for(unsigned i:{12u,90u,52u})setFloat(expected[i],4,getFloat(expected[i],4)+dx);
    check(!memcmp(result.data(),expected.data(),result.size()),"only weapon/arm translation changes; animation and other records byte-identical");
    ComPtr<ID3D11ShaderResourceView> restored;h.ctx->VSGetShaderResources(33,1,&restored);check(restored.Get()==h.ps.Get(),"VS pool restored after draw");
    ComPtr<ID3D11ComputeShader> restoredCs;h.ctx->CSGetShader(&restoredCs,nullptr,nullptr);check(restoredCs.Get()==old.Get(),"CS restored");
    ID3D11ShaderResourceView* rs[2]{};h.ctx->CSGetShaderResources(0,2,rs);for(int i=0;i<2;++i){check(rs[i]==sv[i],"CS SRVs restored");rs[i]->Release();}
    ID3D11UnorderedAccessView* ru[2]{};h.ctx->CSGetUnorderedAccessViews(0,2,ru);for(int i=0;i<2;++i){check(ru[i]==uavs[i],"CS UAVs restored");ru[i]->Release();}
    ComPtr<ID3D11Buffer> cb;h.ctx->CSGetConstantBuffers(0,1,&cb);check(cb.Get()==h.camera.Get(),"CS CB restored");
    check(h.run() && g.prepared==g.frame,"same frame reuses private pool");weaponStabilityResourceWritten(h.bones.Get());check(g.prepared==~0u,"bone rewrite invalidates cache");check(h.run(),"bone rewrite recovers");
    auto* compiled=g.find.Get();auto* fixed=g.fixed.Get();const unsigned beforeToggle=draws;
    weaponStabilityConfigure(Config::get());check(g.prepared==g.frame,"unchanged config preserves prepared pool");
    testEnabled=false;weaponStabilityConfigure(Config::get());
    check(!h.run() && draws==beforeToggle && !g.pending && g.prepared==~0u,"live off immediately bypasses correction and clears pending work");
    c[275][0]+=.05f;h.ctx->UpdateSubresource(h.camera.Get(),0,nullptr,c,0,0);
    testEnabled=true;weaponStabilityConfigure(Config::get());check(h.run(),"live on resumes without screen rediscovery");
    check(g.find.Get()==compiled && g.fixed.Get()==fixed,"live toggle retains shader and buffer allocations");
    result=h.read(g.fixed.Get());
    check(std::fabs(getFloat(reinterpret_cast<Instance*>(result.data())[52],4)-(getFloat(p[52],4)+c[275][0]-getFloat(p[12],4)))<1e-5,"live on uses current camera rather than cached correction");
    weaponStabilityResourceWritten(h.pool.Get());check(g.prepared==~0u,"instance rewrite invalidates cache");check(h.run(),"instance rewrite recovers");
    c[275][0]+=.1f;h.ctx->UpdateSubresource(h.camera.Get(),0,nullptr,c,0,0);weaponStabilityResourceWritten(h.camera.Get());check(h.run(),"same-buffer camera update regenerates");
    result=h.read(g.fixed.Get());check(reinterpret_cast<Instance*>(result.data())[52].words[0]==0,"rigid skin base preserved");
    check(std::fabs(getFloat(reinterpret_cast<Instance*>(result.data())[52],4)-(getFloat(p[52],4)+c[275][0]-getFloat(p[12],4)))<1e-5,"same-frame camera data is fresh");
    weaponStabilityResourceWritten(nullptr);check(g.prepared==~0u,"command list invalidates cache");
    auto stock=[&](const char* label){upload();h.screen();check(h.run(),"guarded draw forwarded");auto a=h.read(g.fixed.Get());check(!memcmp(a.data(),p.data(),a.size()),label);};
    c[273][3]=1;stock("non-source projection remains stock");c[273][3]=0;
    p[90].words[0]=7;stock("duplicate reference to one palette root is not an arm pair");p[90].words[0]=8;
    p[90].words[0]=17;stock("out of bounds bone base remains stock");p[90].words[0]=8;
    p[90].words[2]=789;stock("unrelated arm orientation remains stock");p[90].words[2]=123;
    b[8][1][3]=-.2f;stock("world-body bind pose cannot act as first-person root");b[8][1][3]=-1.7f;
    for(unsigned i:{7u,8u}) {
        b[i][1][1]=b[i][2][2]=std::cos(.03f);b[i][1][2]=-std::sin(.03f);b[i][2][1]=std::sin(.03f);
    }
    upload();h.screen();check(h.run(),"animated rigid arm roots remain eligible");result=h.read(g.fixed.Get());
    check(std::fabs(getFloat(reinterpret_cast<Instance*>(result.data())[52],4)-(getFloat(p[52],4)+c[275][0]-getFloat(p[12],4)))<1e-5,"walking root rotation does not drop weapon correction");
    b[8][0][0]=1.1f;stock("scaled root is not a rigid attachment pair");b[8][0][0]=1;
    b[8][0][0]=-1;stock("reflected root is not a rigid attachment pair");b[8][0][0]=1;
    b[8][0][3]=.02f;stock("paired attachment origins with different bind transforms are rejected");b[8][0][3]=0;
    for(unsigned i:{7u,8u}) {b[i][1][1]=b[i][2][2]=1;b[i][1][2]=b[i][2][1]=0;}
    std::swap(p[12],p[202]);std::swap(p[90],p[191]);upload();h.screen();check(h.run(),"record repacking works");result=h.read(g.fixed.Get());
    check(std::fabs(getFloat(reinterpret_cast<Instance*>(result.data())[202],4)-c[275][0])<1e-5,"repacked attachment follows current camera");
    testVs=0xEB5234DB6ADB491Dull;check(!h.run(),"other scenery shader families excluded");testVs=0x8B589D25B2A0ADDCull;
    D3D11_VIEWPORT bad{0,0,32,48,0,1};h.ctx->RSSetViewports(1,&bad);check(!h.run(),"partial viewport excluded");bad.Width=64;h.ctx->RSSetViewports(1,&bad);
    for(int i=0;i<3;++i)weaponStabilityFrameBoundary(h.ctx.Get());check(!h.run(),"stale on-foot screen expires");
    testEnabled=false;weaponStabilityConfigure(Config::get());
    for(int i=0;i<121;++i)weaponStabilityFrameBoundary(h.ctx.Get());check(!g.fixed && !g.seen,"inactive resources released");
    h.screen();check(!h.run() && !g.fixed,"off setting survives leaving and reentering on-foot screen");
    testEnabled=true;weaponStabilityConfigure(Config::get());check(h.run(),"on setting resumes after resource release");h.clean();
}
std::vector<unsigned char> file(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);check(bool(f),"capture input exists");return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
void capture(const std::filesystem::path& dir){
    Harness h;auto p=file(dir/"pool.bin"),b=file(dir/"bones.bin"),c=file(dir/"camera.bin"),e=file(dir/"pool-expected.bin");
    h.inputs(p.data(),unsigned(p.size()),b.data(),unsigned(b.size()),c.data(),unsigned(c.size()));h.screen();check(h.run(),"captured shader/buffer contract accepted");
    auto a=h.read(g.fixed.Get());check(a.size()==e.size(),"captured pool size preserved");
    unsigned changed=0;float maxError=0;
    for(size_t i=0;i<a.size();i+=4){
        if(i%336>=16 && i%336<28){float af,ef;memcpy(&af,&a[i],4);memcpy(&ef,&e[i],4);if(memcmp(&a[i],&e[i],4))maxError=(std::max)(maxError,std::fabs(af-ef));}
        else check(!memcmp(&a[i],&e[i],4),"all nontranslation fields are byte-identical");
        if(memcmp(&a[i],&p[i],4))++changed;
    }
    check(maxError<1e-5,"production GPU output matches independent captured-transform reconstruction");check(changed>0,"captured correction is active");
    auto anchor=h.read(g.anchor.Get());const auto* v=reinterpret_cast<const float*>(anchor.data());
    std::printf("capture %s: offset %.6f %.6f %.6f, roots %.0f, max error %.9f, changed words %u\n",dir.filename().string().c_str(),v[0],v[1],v[2],v[7],maxError,changed);h.clean();
}
void emitterTest(){
    Harness h;std::vector<Instance> p(128);float bones[10][3][4]{},cam[276][4]{},model[13][4]{};
    cam[270][0]=cam[271][1]=cam[272][3]=1;cam[273][2]=.0675f;
    for(auto& r:p)position(r,100,100,100);
    for(unsigned i:{7u,8u}){bones[i][0][0]=bones[i][1][1]=bones[i][2][2]=1;bones[i][1][3]=-1.7f;}
    position(p[12],.04f,.03f,-.02f);p[12].words[0]=7;p[90]=p[12];p[90].words[0]=8;
    for(unsigned i=0;i<13;++i)for(unsigned j=0;j<4;++j)model[i][j]=float(i*4+j);
    for(unsigned i=9;i<12;++i){for(unsigned j=0;j<3;++j)model[i][j]=float(i-9==j);model[i][3]=getFloat(p[12],4+i-9);}
    model[11][3]+=.075f;
    h.inputs(p.data(),unsigned(p.size()*336),bones,sizeof(bones),cam,sizeof(cam));h.screen();check(h.run(),"emitter test has current-frame mesh anchor");
    auto input=h.buffer(model,sizeof(model),0,D3D11_BIND_CONSTANT_BUFFER);h.ctx->VSSetConstantBuffers(0,1,input.GetAddressOf());
    auto sentinel=h.buffer(nullptr,64,16,D3D11_BIND_UNORDERED_ACCESS);ComPtr<ID3D11UnorderedAccessView> sentinelUav;
    hr(h.dev->CreateUnorderedAccessView(sentinel.Get(),nullptr,&sentinelUav));h.ctx->CSSetUnorderedAccessViews(2,1,sentinelUav.GetAddressOf(),nullptr);
    h.ctx->CSSetConstantBuffers(1,1,input.GetAddressOf());
    auto run=[&](bool corrected){
        testVs=0x9AEC596A2B036EA6ull;testPs=0x3789CA2062E196FBull;
        h.ctx->UpdateSubresource(input.Get(),0,nullptr,model,0,0);h.ctx->UpdateSubresource(h.camera.Get(),0,nullptr,cam,0,0);weaponStabilityResourceWritten(h.camera.Get());
        check(h.run(),"verified particle draw is forwarded");auto bytes=h.read(g.emitterCb.Get());auto* result=reinterpret_cast<float*>(bytes.data());
        for(unsigned i=0;i<52;++i){float expected=model[i/4][i%4];if(corrected && i>=39 && i<=47 && i%4==3)expected-=getFloat(p[12],4+(i/4-9));
            check(std::fabs(result[i]-expected)<1e-6f,"emitter changes only the same attachment translation");}
        auto original=h.read(input.Get());check(!memcmp(original.data(),model,sizeof(model)),"original emitter CB unmodified");
        ComPtr<ID3D11Buffer> cb;h.ctx->VSGetConstantBuffers(0,1,&cb);check(cb.Get()==input.Get(),"emitter VS CB restored");
        cb.Reset();h.ctx->CSGetConstantBuffers(1,1,&cb);check(cb.Get()==input.Get(),"emitter CS CB restored");
        ComPtr<ID3D11UnorderedAccessView> u;h.ctx->CSGetUnorderedAccessViews(2,1,&u);check(u.Get()==sentinelUav.Get(),"emitter CS UAV restored");
    };
    run(true);
    cam[273][2]=.025f;run(false);cam[273][2]=.0675f;
    model[9][3]+=10;run(false);model[9][3]-=10;
    model[9][0]=2;run(false);model[9][0]=1;
    p[90].words[0]=7;h.ctx->UpdateSubresource(h.pool.Get(),0,nullptr,p.data(),0,0);weaponStabilityResourceWritten(h.pool.Get());run(false);
    testPs=0;check(!h.run(),"other particle materials excluded");testPs=0x3789CA2062E196FBull;
    weaponStabilityResourceWritten(nullptr);check(!h.run(),"unknown command-list state cannot reuse an emitter anchor");
    testEnabled=false;weaponStabilityConfigure(Config::get());check(!h.run(),"live weapon-stability off also disables emitter correction");
    testEnabled=true;weaponStabilityConfigure(Config::get());h.clean();
}
int main(int argc,char** argv){
    if(argc==2 && !strcmp(argv[1],"--self-test")){selfTest();emitterTest();}
    else if(argc==3 && !strcmp(argv[1],"--capture"))capture(argv[2]);
    else {std::puts("Usage: weapon_stability_test --self-test | --capture DIR");return 2;}
    std::printf("PASS: weapon stability (%u checks)\n",checks);return 0;
}
