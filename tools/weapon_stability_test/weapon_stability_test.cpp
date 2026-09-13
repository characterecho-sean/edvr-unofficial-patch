// Exercise the production GPU attachment correction, including its draw gate
// and state restoration. Optional --capture DIR replays exported flight inputs.
#include "../../src/d3d11/weapon_stability.cpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cstdarg>
#include <vector>
#include <fstream>
#include <filesystem>
using Microsoft::WRL::ComPtr;
unsigned checks=0,draws=0,motionDraws=0;ID3D11ShaderResourceView* drawnPool=nullptr;
D3D_DRIVER_TYPE testDriver=D3D_DRIVER_TYPE_WARP;
std::vector<std::string> testLog;
void check(bool b,const char* label){++checks;if(!b){std::printf("FAIL: %s\n",label);std::exit(1);}}
void hr(HRESULT h){check(SUCCEEDED(h),"D3D operation");}
ComPtr<ID3DBlob> compile(const char* s,const char* entry){
    ComPtr<ID3DBlob> c,e;HRESULT h=D3DCompile(s,strlen(s),nullptr,nullptr,nullptr,entry,"cs_5_0",0,0,&c,&e);
    if(FAILED(h)&&e)std::puts(static_cast<const char*>(e->GetBufferPointer()));hr(h);return c;
}
namespace edvr {
void weaponMotionDraw(ID3D11DeviceContext*,PanelCurveDrawFn,unsigned,unsigned,unsigned,int,unsigned){++motionDraws;}
void weaponMotionResourceWritten(ID3D11Resource*){}
void meshMotionResourceWritten(ID3D11Resource*,uint64_t,uint64_t){}
bool testEnabled=true;
Config& Config::get(){static Config cfg;return cfg;}
bool Config::getBool(const char* key,bool def) const {
    check(!strcmp(key,"fix.weapon_stability") && def,"weapon stability config key defaults on");return testEnabled;
}
uint64_t testVs=0,testPs=0;ID3D11RenderTargetView* testRtv=nullptr;
Log& Log::get(){static Log l;return l;}Log::~Log()=default;
void Log::note(const char* format,...){char line[2048];va_list args;va_start(args,format);vsnprintf(line,sizeof(line),format,args);va_end(args);testLog.emplace_back(line);}
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
        D3D_FEATURE_LEVEL fl;HRESULT h=D3D11CreateDevice(nullptr,testDriver,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);
        if(h==DXGI_ERROR_SDK_COMPONENT_MISSING)h=D3D11CreateDevice(nullptr,testDriver,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);hr(h);dev.As(&queue);
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
    c[270][0]=c[271][1]=c[272][3]=1;c[273][2]=.0675f;c[275][0]=10;c[275][1]=20;c[275][2]=30;
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
    for(uint64_t vs:{0xAACFDCF2FB9AD809ull,0x34CCFAAB1EAD90BEull,0x174E8D76363BE337ull,0x025B4B9FF54622EDull,0x7F9B650EC1A1E570ull,0x88DCF1164C640EC3ull}) {
        auto* fixed=g.fixed.Get();const unsigned before=draws;testVs=vs;
        check(h.run() && draws==before+1 && drawnPool==g.fixedSrv.Get() && g.fixed.Get()==fixed,"additional material uses shared corrected pool exactly once");
    }
    for(uint64_t vs:{0xB10B032BDFD46700ull,0xC4B4B334B26E81A9ull,0xA888D51024D9798Eull,0xCFCA8FFC6B058630ull}) {
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
    h.ctx->CSSetShaderResources(0,1,h.bs.GetAddressOf());
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
        ComPtr<ID3D11ShaderResourceView> srv;h.ctx->CSGetShaderResources(0,1,&srv);check(srv.Get()==h.bs.Get(),"emitter restores displaced CS source view");
    };
    run(true);
    cam[273][2]=.025f;run(false);
    position(p[51],model[9][3],model[10][3],model[11][3]);
    auto updatePool=[&](){h.ctx->UpdateSubresource(h.pool.Get(),0,nullptr,p.data(),0,0);weaponStabilityResourceWritten(h.pool.Get());};
    updatePool();run(false); // ADS emitter keeps the same stock pose as its mesh
    setFloat(p[51],5,getFloat(p[51],5)+.001f);updatePool();run(false); // merely nearby world effect
    setFloat(p[51],5,model[10][3]);p[51].words[0]=9;updatePool();run(false); // independently skinned world body
    p[51].words[0]=0;updatePool();cam[273][2]=.05f;run(false);cam[273][2]=.0675f;
    model[9][3]+=10;run(false);model[9][3]-=10;
    model[9][0]=2;run(false);model[9][0]=1;
    p[90].words[0]=7;h.ctx->UpdateSubresource(h.pool.Get(),0,nullptr,p.data(),0,0);weaponStabilityResourceWritten(h.pool.Get());run(false);
    testPs=0;check(!h.run(),"other particle materials excluded");testPs=0x3789CA2062E196FBull;
    weaponStabilityResourceWritten(nullptr);check(!h.run(),"unknown command-list state cannot reuse an emitter anchor");
    testEnabled=false;weaponStabilityConfigure(Config::get());check(!h.run(),"live weapon-stability off also disables emitter correction");
    testEnabled=true;weaponStabilityConfigure(Config::get());h.clean();
}
ComPtr<ID3D11Buffer> drawnLights;
void __stdcall drawLights(ID3D11DeviceContext* ctx,unsigned n,unsigned instances,unsigned start,int base,unsigned si) {
    check(n==14 && instances==5 && !start && !base && !si,"light draw arguments preserved");
    UINT stride=0,offset=0;drawnLights.Reset();ctx->IAGetVertexBuffers(1,1,&drawnLights,&stride,&offset);
    check(stride==32 && offset==0,"private light stream has original layout and correct origin");
}
void lightTest(){
    Harness h;std::vector<Instance> p(128);float bones[10][3][4]{},cam[276][4]{},lc[14][4]{};
    cam[270][0]=cam[271][1]=cam[272][3]=1;cam[273][2]=.0675f;
    for(auto& r:p)position(r,100,100,100);
    for(unsigned i:{7u,8u}){bones[i][0][0]=bones[i][1][1]=bones[i][2][2]=1;bones[i][1][3]=-1.7f;}
    position(p[12],.04f,.03f,-.02f);p[12].words[0]=7;p[90]=p[12];p[90].words[0]=8;
    lc[2][0]=lc[3][1]=lc[4][2]=1;lc[12][3]=.025f;
    h.inputs(p.data(),unsigned(p.size()*336),bones,sizeof(bones),cam,sizeof(cam));h.screen();check(h.run(),"light test has current-frame mesh anchor");
    // Original IA stream has a nonzero byte offset; packed payload bytes
    // must survive even when reinterpreting them as floats would be NaNs.
    uint32_t raw[4+5*8]{};auto* lights=reinterpret_cast<float*>(raw+4);
    for(unsigned i=0;i<5;++i){lights[i*8]=.1f;lights[i*8+1]=.2f;lights[i*8+2]=.3f;lights[i*8+3]=.025925925f;
        for(unsigned j=4;j<8;++j)raw[4+i*8+j]=0xff800000u+i*8+j;}
    lights[8]=13;lights[2*8+3]=2;lights[3*8+3]=0;lights[4*8+3]=-1;
    auto vb=h.buffer(raw,sizeof(raw),0,D3D11_BIND_VERTEX_BUFFER),cb=h.buffer(lc,sizeof(lc),0,D3D11_BIND_CONSTANT_BUFFER);
    UINT stride=32,offset=16;h.ctx->IASetVertexBuffers(1,1,vb.GetAddressOf(),&stride,&offset);h.ctx->VSSetConstantBuffers(2,1,cb.GetAddressOf());
    ID3D11Buffer* nullCb=nullptr;h.ctx->VSSetConstantBuffers(1,1,&nullCb); // actual point-light draw has no VS b1
    auto sentinel=h.buffer(nullptr,64,16,D3D11_BIND_UNORDERED_ACCESS);ComPtr<ID3D11UnorderedAccessView> uav;
    hr(h.dev->CreateUnorderedAccessView(sentinel.Get(),nullptr,&uav));h.ctx->CSSetUnorderedAccessViews(3,1,uav.GetAddressOf(),nullptr);
    h.ctx->CSSetConstantBuffers(2,1,cb.GetAddressOf());h.ctx->CSSetConstantBuffers(0,1,h.camera.GetAddressOf());
    auto code=compile("[numthreads(1,1,1)]void main(){}","main");ComPtr<ID3D11ComputeShader> old;
    hr(h.dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&old));h.ctx->CSSetShader(old.Get(),nullptr,0);
    auto run=[&](bool corrected){
        testVs=0x0357BBB2DEE43C1Full;testPs=0x81812EF97FB4A361ull;h.ctx->UpdateSubresource(cb.Get(),0,nullptr,lc,0,0);
        check(weaponStabilityDraw(h.ctx.Get(),drawLights,14,5,0,0,0,64,48),"verified point lights forwarded");
        auto result=h.read(drawnLights.Get());auto* rf=reinterpret_cast<float*>(result.data());
        for(unsigned i=0;i<5*8;++i){
            if(corrected && i<3)check(std::fabs(rf[i]-(lights[i]-getFloat(p[12],4+i)))<1e-6,"local light follows full root delta, not projection-scaled delta");
            else check(!memcmp(result.data()+i*4,raw+4+i,4),"world lights, radii and packed payload stay byte-identical");
        }
        auto original=h.read(vb.Get());check(!memcmp(original.data(),raw,sizeof(raw)),"original light stream untouched");
        ComPtr<ID3D11Buffer> restored;UINT s=0,o=0;h.ctx->IAGetVertexBuffers(1,1,&restored,&s,&o);
        check(restored.Get()==vb.Get() && s==32 && o==16,"IA binding restored");
        restored.Reset();h.ctx->VSGetConstantBuffers(1,1,&restored);check(!restored,"unbound VS camera remains unbound");
        restored.Reset();h.ctx->CSGetConstantBuffers(2,1,&restored);check(restored.Get()==cb.Get(),"light CS camera restored");
        ComPtr<ID3D11UnorderedAccessView> ru;h.ctx->CSGetUnorderedAccessViews(3,1,&ru);check(ru.Get()==uav.Get(),"light CS UAV restored");
        ComPtr<ID3D11ComputeShader> cs;h.ctx->CSGetShader(&cs,nullptr,nullptr);check(cs.Get()==old.Get(),"light CS shader restored");
    };
    run(true);auto* allocation=g.lightVertices.Get();run(true);check(allocation==g.lightVertices.Get(),"repeated light batches reuse allocation");
    cam[273][2]=.025f;h.ctx->UpdateSubresource(h.camera.Get(),0,nullptr,cam,0,0);weaponStabilityResourceWritten(h.camera.Get());
    run(false); // ADS must not detach a light by retaining the hip-fire delta.
    cam[273][2]=.0675f;h.ctx->UpdateSubresource(h.camera.Get(),0,nullptr,cam,0,0);weaponStabilityResourceWritten(h.camera.Get());run(true);
    lc[6][3]=.01f;run(false);lc[6][3]=0;
    lc[12][3]=.0675f;run(false);lc[12][3]=.025f;
    lc[2][0]=2;run(false);lc[2][0]=1;
    testPs=0;check(!weaponStabilityDraw(h.ctx.Get(),drawLights,14,5,0,0,0,64,48),"unverified point-light material declined");testPs=0x81812EF97FB4A361ull;
    check(!weaponStabilityDraw(h.ctx.Get(),drawLights,14,5,0,0,1,64,48),"unsupported nonzero start-instance retains stock draw");
    check(!weaponStabilityDraw(h.ctx.Get(),drawLights,14,6,0,0,0,64,48),"light window cannot exceed bound buffer");
    stride=16;h.ctx->IASetVertexBuffers(1,1,vb.GetAddressOf(),&stride,&offset);check(!weaponStabilityDraw(h.ctx.Get(),drawLights,14,5,0,0,0,64,48),"unknown light layout declined");
    stride=32;h.ctx->IASetVertexBuffers(1,1,vb.GetAddressOf(),&stride,&offset);
    weaponStabilityFrameBoundary(h.ctx.Get());check(!weaponStabilityDraw(h.ctx.Get(),drawLights,14,5,0,0,0,64,48),"previous-frame mesh anchor cannot move lights");
    h.ctx->VSSetConstantBuffers(1,1,h.camera.GetAddressOf());h.screen();check(h.run(),"fresh mesh restores light eligibility");testVs=0x0357BBB2DEE43C1Full;testPs=0x81812EF97FB4A361ull;
    weaponStabilityResourceWritten(nullptr);check(!weaponStabilityDraw(h.ctx.Get(),drawLights,14,5,0,0,0,64,48),"command-list invalidation excludes stale light anchors");
    testEnabled=false;weaponStabilityConfigure(Config::get());check(!weaponStabilityDraw(h.ctx.Get(),drawLights,14,5,0,0,0,64,48),"live weapon toggle also bypasses lights");
    testEnabled=true;weaponStabilityConfigure(Config::get());drawnLights.Reset();h.clean();
}
void aimingTest(){
    Harness h;std::vector<Instance> pool(128);float bones[10][3][4]{},camera[276][4]{};
    camera[270][0]=camera[271][1]=camera[272][3]=1;camera[273][2]=.0675f;
    for(auto& r:pool)position(r,100,100,100);
    for(unsigned i:{7u,8u}){bones[i][0][0]=bones[i][1][1]=bones[i][2][2]=1;bones[i][1][3]=-1.7f;}
    position(pool[12],-.041941643f,-.009459019f,.062137604f);pool[12].words[0]=7;pool[90]=pool[12];pool[90].words[0]=8;
    position(pool[52],.1f,.2f,.4f);
    h.inputs(pool.data(),unsigned(pool.size()*336),bones,sizeof(bones),camera,sizeof(camera));h.screen();
    check(h.run(),"hip-fire pose starts with correction");auto hip=h.read(g.fixed.Get());check(memcmp(hip.data(),pool.data(),hip.size())!=0,"hip-fire correction is active before aiming");
    auto updateCamera=[&](){h.ctx->UpdateSubresource(h.camera.Get(),0,nullptr,camera,0,0);weaponStabilityResourceWritten(h.camera.Get());};
    for(float nearZ:{.025f,.04f,.06f,.075f}){
        camera[273][2]=nearZ;camera[270][0]=3;camera[271][1]=4;camera[275][0]+=.03f;updateCamera();
        for(uint64_t vs:{0x8B589D25B2A0ADDCull,0x7B0DC42D383F694Cull,0x025B4B9FF54622EDull,0x7F9B650EC1A1E570ull,0x174E8D76363BE337ull,0x34CCFAAB1EAD90BEull}){
            testVs=vs;const unsigned beforeMotion=motionDraws,beforeDraw=draws;check(h.run(),"aiming and transition material forwarded");auto actual=h.read(g.fixed.Get());
            check(!memcmp(actual.data(),pool.data(),actual.size()),"aiming body, arms, reticle and scope retain every original transform bit");
            check(motionDraws==beforeMotion+1 && draws==beforeDraw+1,"aiming still supplies temporal motion and exactly one original draw");
        }
        auto anchor=h.read(g.anchor.Get());const auto* a=reinterpret_cast<const float*>(anchor.data());
        check(a[3]==0 && a[8]==1 && a[9]==nearZ,"projection fallback is reported distinctly from missing roots");
    }
    camera[273][2]=.0675f;updateCamera();testVs=0x8B589D25B2A0ADDCull;check(h.run(),"leaving ADS resumes hip-fire correction");auto actual=h.read(g.fixed.Get());
    const auto* r=reinterpret_cast<const Instance*>(actual.data());
    check(std::fabs(getFloat(r[52],4)-(getFloat(pool[52],4)+camera[275][0]-getFloat(pool[12],4)))<1e-6f,"return to hip fire uses fresh attachment rather than stale ADS data");h.clean();
}
void aimingTimingTest(){
    Harness h;std::vector<Instance> pool(128);float bones[10][3][4]{},camera[276][4]{};
    camera[270][0]=2;camera[271][1]=3;camera[272][3]=1;camera[273][2]=.025f;
    for(auto& r:pool)position(r,100,100,100);
    for(unsigned i:{7u,8u}){bones[i][0][0]=bones[i][1][1]=bones[i][2][2]=1;bones[i][1][3]=-1.7f;}
    pool[12].words[0]=7;pool[90].words[0]=8;
    float offset[3]={.015f,.010f,-.075f};
    h.inputs(pool.data(),unsigned(pool.size()*336),bones,sizeof(bones),camera,sizeof(camera));
    auto sentinel=h.buffer(nullptr,16,0,D3D11_BIND_CONSTANT_BUFFER);h.ctx->CSSetConstantBuffers(3,1,sentinel.GetAddressOf());
    auto pose=[&](float x,float z,float extraX,float extraZ){
        for(unsigned i:{12u,90u})position(pool[i],x,0,z);
        position(pool[52],x+.12f,.2f,z+.4f); // original animated rigid attachment
        camera[275][0]=x+offset[0]+extraX;camera[275][1]=offset[1];camera[275][2]=z+offset[2]+extraZ;
        h.ctx->UpdateSubresource(h.pool.Get(),0,nullptr,pool.data(),0,0);weaponStabilityResourceWritten(h.pool.Get());
        h.ctx->UpdateSubresource(h.camera.Get(),0,nullptr,camera,0,0);weaponStabilityResourceWritten(h.camera.Get());h.screen();
    };
    auto verify=[&](float dx,float dz,const char* label){
        check(h.run(),"ADS timing draw forwarded");auto result=h.read(g.fixed.Get());auto expected=pool;
        if(dx || dz)for(unsigned i:{12u,90u,52u}){setFloat(expected[i],4,getFloat(expected[i],4)+dx);setFloat(expected[i],6,getFloat(expected[i],6)+dz);}
        const auto* a=reinterpret_cast<const Instance*>(result.data());
        bool correct=true;for(unsigned i=0;i<pool.size();++i)for(unsigned w=0;w<84;++w){
            if((dx||dz) && (i==12 || i==90 || i==52) && (w==4 || w==6))correct&=std::fabs(getFloat(a[i],w)-getFloat(expected[i],w))<1e-6f;
            else correct&=a[i].words[w]==expected[i].words[w];
        }
        check(correct,label);
        auto original=h.read(h.pool.Get());check(!memcmp(original.data(),pool.data(),original.size()),"ADS timing never edits the original pool");
        ComPtr<ID3D11Buffer> restored;h.ctx->CSGetConstantBuffers(3,1,&restored);check(restored.Get()==sentinel.Get(),"ADS sample constant slot restored");
        const auto before=motionDraws;testVs=0x025B4B9FF54622EDull;check(h.run() && motionDraws==before+1,"reticle shares timing correction and temporal forwarding");
        auto reticle=h.read(g.fixed.Get());check(reticle==result,"optic and mesh see identical corrected geometry");
    };
    auto next=[&](){weaponStabilityFrameBoundary(h.ctx.Get());};
    pose(0,0,0,0);verify(0,0,"first ADS sample preserves aim");
    for(unsigned i=0;i<8;++i){pose(0,0,0,0);check(h.run(),"same-frame rewrites forwarded");}
    auto state=h.read(g.anchor.Get());check(reinterpret_cast<float*>(state.data())[(3+(g.frame&1)*8+2)*4+3]==1,"multiple materials/rewrites cannot manufacture synchronized frames");
    next();pose(.04f,0,0,0);verify(0,0,"second synchronized sample preserves aim");
    next();pose(.08f,0,0,0);verify(0,0,"third synchronized sample establishes this sight offset");
    next();pose(.12f,0,.04f,0);verify(.04f,0,"strafe overshoot removes timing error only, preserving full ADS offset");
    // Effects use the same anchor even if their own camera write refreshes it.
    float emitter[13][4]{};for(unsigned i=0;i<3;++i){emitter[9+i][i]=1;emitter[9+i][3]=getFloat(pool[52],4+i)-camera[275][i];}
    auto model=h.buffer(emitter,sizeof(emitter),0,D3D11_BIND_CONSTANT_BUFFER);h.ctx->VSSetConstantBuffers(0,1,model.GetAddressOf());
    auto beforeEffect=h.read(g.anchor.Get());
    testVs=0x9AEC596A2B036EA6ull;testPs=0x3789CA2062E196FBull;weaponStabilityResourceWritten(h.camera.Get());check(h.run(),"ADS emitter with exact part match forwarded");
    auto er=h.read(g.emitterCb.Get());const auto* ef=reinterpret_cast<const float*>(er.data());check(std::fabs(ef[39]-emitter[9][3]-.04f)<1e-6,"ADS emitter follows timing correction without full aim-offset removal");
    auto afterEffect=h.read(g.anchor.Get());check(!memcmp(afterEffect.data()+48,beforeEffect.data()+48,beforeEffect.size()-48),"effect passes cannot replace the mesh timing samples");
    next();pose(.16f,0,0,0);verify(0,0,"camera repeat catches up without an opposite correction");
    next();pose(.16f,.04f,0,0);verify(0,0,"change from strafing to forward translation keeps original pose");
    next();pose(.16f,.08f,0,.04f);verify(0,.04f,"forward overshoot retains sight alignment");
    next();pose(.16f,.12f,0,0);verify(0,0,"forward repeat catches up cleanly");
    next();pose(.16f,.12f,0,.04f);verify(0,.04f,"camera advance with repeated arms uses verified preceding locomotion");
    next();pose(.16f,.16f,0,0);verify(0,0,"repeated arms catch up cleanly");
    next();pose(.16f,.12f,0,0);verify(0,0,"backward movement establishes original pose");
    next();pose(.16f,.08f,0,-.04f);verify(0,-.04f,"backward overshoot retains the same sight offset");
    next();pose(.16f,.04f,0,0);verify(0,0,"backward camera repeat catches up");
    next();pose(.16f,.08f,0,0);verify(0,0,"resume forward walking");
    next();pose(.16f,.12f,0,0);verify(0,0,"stable forward pose after direction change");
    next();offset[0]+=.02f;pose(.16f,.12f,0,0);verify(0,0,"intentional stationary aiming offset change stays original");
    next();pose(.20f,.12f,0,0);verify(0,0,"changed sight calibration needs synchronized samples");
    next();pose(.24f,.12f,0,0);verify(0,0,"new offset establishes without smoothing");
    next();camera[270][0]=3;pose(.28f,.12f,.04f,0);verify(0,0,"scope/FOV transition discards calibration");
    next();pose(.32f,.12f,0,0);verify(0,0,"scope reacquires original pose");
    next();pose(.36f,.12f,0,0);verify(0,0,"scope second sample");
    next();pose(.40f,.12f,0,0);verify(0,0,"scope calibrated");
    next();float angle=.02f;camera[270][0]=3*std::cos(angle);camera[272][0]=-3*std::sin(angle);camera[270][3]=std::sin(angle);camera[272][3]=std::cos(angle);
    pose(.44f,.12f,.04f,0);verify(0,0,"mouse rotation/recoil is not mistaken for translation overshoot");
    camera[270][0]=3;camera[272][0]=camera[270][3]=0;camera[272][3]=1;
    for(unsigned i=0;i<3;++i){next();pose(.48f+.04f*i,.12f,0,0);verify(0,0,"steady aim reacquires after rotation");}
    next();testEnabled=false;weaponStabilityConfigure(Config::get());testEnabled=true;weaponStabilityConfigure(Config::get());
    pose(.60f,.12f,.04f,0);verify(0,0,"live toggle discards pre-toggle calibration");
    for(unsigned i=0;i<3;++i){next();pose(.64f+.04f*i,.12f,0,0);verify(0,0,"calibration after toggle");}
    next();pool[90].words[0]=7;pose(.76f,.12f,.04f,0);verify(0,0,"missing independent arm root rejects correction");pool[90].words[0]=8;
    for(unsigned i=0;i<3;++i){next();pose(.80f+.04f*i,.12f,0,0);verify(0,0,"reacquire after missing paired root");}
    next();weaponStabilityResourceWritten(nullptr);pose(.92f,.12f,.04f,0);verify(0,0,"unknown command-list writes invalidate calibration epoch");
    for(unsigned i=0;i<3;++i){next();pose(.96f+.04f*i,.12f,0,0);verify(0,0,"reacquire after unknown writes");}
    next();next();pose(.80f,.12f,.04f,0);verify(0,0,"missing source frame cannot reuse ADS history");
    next();camera[273][2]=.0675f;pose(.84f,.12f,0,0);
    check(h.run(),"hip-fire reentry forwarded");auto hip=h.read(g.anchor.Get());const auto* ha=reinterpret_cast<const float*>(hip.data());
    check(ha[3]==1 && std::fabs(ha[0]-offset[0])<1e-6 && std::fabs(ha[1]-offset[1])<1e-6 && std::fabs(ha[2]-offset[2])<1e-6,"hip-fire reentry retains direct attachment correction");
    h.clean();
}
void traceTest(){
    Harness h;std::vector<Instance> pool(128);float bones[10][3][4]{},camera[276][4]{};
    camera[270][0]=2;camera[271][1]=3;camera[272][3]=1;camera[273][2]=.025f;
    for(auto& r:pool)position(r,100,100,100);
    for(unsigned i:{7u,8u}){bones[i][0][0]=bones[i][1][1]=bones[i][2][2]=1;bones[i][1][3]=-1.7f;}
    pool[12].words[0]=7;pool[90].words[0]=8;
    h.inputs(pool.data(),unsigned(pool.size()*336),bones,sizeof(bones),camera,sizeof(camera));
    weaponStabilityArmTrace();check(!g.traceRequested,"inactive eye dump does not request weapon readback");
    for(unsigned f=0;f<140;++f){
        for(unsigned i:{12u,90u})position(pool[i],f*.01f,0,0);
        camera[275][0]=f*.01f+.01f;camera[275][1]=.02f;camera[275][2]=-.075f;
        h.ctx->UpdateSubresource(h.pool.Get(),0,nullptr,pool.data(),0,0);weaponStabilityResourceWritten(h.pool.Get());
        h.ctx->UpdateSubresource(h.camera.Get(),0,nullptr,camera,0,0);weaponStabilityResourceWritten(h.camera.Get());h.screen();check(h.run(),"trace ring records ordinary source draw");
        auto result=h.read(g.fixed.Get());check(!memcmp(result.data(),pool.data(),result.size()),"recording trace leaves synchronized geometry bit-identical");
        if(f==139){
            camera[275][0]+=.04f;h.ctx->UpdateSubresource(h.camera.Get(),0,nullptr,camera,0,0);weaponStabilityResourceWritten(h.camera.Get());check(h.run(),"second camera update recorded");
            auto anchor=h.read(g.anchor.Get());auto* a=reinterpret_cast<const float*>(anchor.data());
            check(a[8]==3 && std::fabs(a[0]-.04f)<1e-6,"trace does not alter timing correction");
            check(!g.traceStage && !g.tracePending,"rolling GPU history has no routine staging/readback");
            weaponStabilityArmTrace();check(g.traceRequested,"eye dump requests prehistory");
        }
        weaponStabilityFrameBoundary(h.ctx.Get());
    }
    check(g.tracePending && g.traceFrame==139 && g.traceStage,"eye dump copies bounded history once");
    auto frozen=h.read(g.traceStage.Get());check(frozen.size()==kWeaponTraceFrames*kWeaponTraceRows*16,"readback bounded to 32 KiB");
    auto* p=reinterpret_cast<const float*>(frozen.data());
    for(unsigned frame=12;frame<140;++frame){
        const float* first=p+(frame%kWeaponTraceFrames)*kWeaponTraceRows*4;
        unsigned stamp=0;memcpy(&stamp,first,4);check(stamp==frame,"ring retains last 128 actual frames");
        check(first[3]==1,"first sample is not overwritten by later material");
        const float* last=first+32;
        check(last[3]==(frame==139?2:1),"last sample records same-frame regeneration count");
        if(frame==139){
            check(first[23]==2 && last[23]==3 && std::fabs(last[24]-.04f)<1e-6,"trace distinguishes synchronized first draw and corrected last draw");
            check(unsigned(first[2])==223 && unsigned(last[2])==251,"numeric GPU trace flags survive storage and decoding");
        }
    }
    weaponStabilityArmTrace();check(!g.traceRequested,"pending trace cannot be overwritten by another request");
    testLog.clear();
    for(unsigned i=0;i<4;++i)weaponStabilityFrameBoundary(h.ctx.Get());
    check(!g.tracePending,"ready readback drains asynchronously at frame boundary");
    unsigned records=0;bool complete=false,last=false;
    for(auto& line:testLog){if(line.find("weapon timing frame=")==0)++records;if(line=="weapon timing trace: complete.")complete=true;if(line.find("frame=139 last")!=std::string::npos && line.find("status=3")!=std::string::npos)last=true;}
    check(records==256 && complete && last,"logged trace has full prehistory and correction status");
    testEnabled=false;weaponStabilityConfigure(Config::get());h.screen();weaponStabilityArmTrace();check(!g.traceRequested,"disabled weapon fix does not request readback");
    testEnabled=true;weaponStabilityConfigure(Config::get());h.clean();
}
void sustainedAimingTest(){
    for(unsigned axis:{0u,2u})for(bool delayed:{false,true}){
        Harness h;std::vector<Instance> pool(128);float bones[10][3][4]{},camera[276][4]{};
        camera[270][0]=2;camera[271][1]=3;camera[272][3]=1;camera[273][2]=.025f;
        for(auto& r:pool)position(r,100,100,100);
        for(unsigned i:{7u,8u}){bones[i][0][0]=bones[i][1][1]=bones[i][2][2]=1;bones[i][1][3]=-1.7f;}
        pool[12].words[0]=7;pool[90].words[0]=8;
        h.inputs(pool.data(),unsigned(pool.size()*336),bones,sizeof(bones),camera,sizeof(camera));
        float x=0,previous=0;
        auto sample=[&](float step,bool lag,float expected){
            previous=x;x+=step;const float arm=(lag?previous:x)-.014f;
            for(unsigned i:{12u,90u})position(pool[i],arm,-.021f,.130f);
            position(pool[52],arm+.1f,.2f,.4f);camera[275][0]=x;camera[275][1]=camera[275][2]=0;
            if(axis==2){
                std::swap(camera[275][0],camera[275][2]);
                for(auto& r:pool)std::swap(r.words[4],r.words[6]);
            }
            h.ctx->UpdateSubresource(h.pool.Get(),0,nullptr,pool.data(),0,0);weaponStabilityResourceWritten(h.pool.Get());
            h.ctx->UpdateSubresource(h.camera.Get(),0,nullptr,camera,0,0);weaponStabilityResourceWritten(h.camera.Get());h.screen();check(h.run(),"sustained ADS draw forwarded");
            auto bytes=h.read(g.fixed.Get());const auto* actual=reinterpret_cast<const Instance*>(bytes.data());
            for(unsigned i=0;i<128;++i)for(unsigned w=0;w<84;++w){
                if(expected && (i==12 || i==90 || i==52) && w==4+axis)check(std::fabs(getFloat(actual[i],w)-(getFloat(pool[i],w)+expected))<1e-6,"one-frame translation preserves this weapon's independent aiming offset");
                else check(actual[i].words[w]==pool[i].words[w],"all other geometry and animation bytes preserved");
            }
            auto a=h.read(g.anchor.Get());auto* anchor=reinterpret_cast<const float*>(a.data());
            if(expected)check(anchor[8]==4,"sustained timing correction has a distinct status");
            const unsigned before=motionDraws;testVs=0x025B4B9FF54622EDull;check(h.run() && motionDraws==before+1,"sustained correction still forwards reticle motion");
            check(h.read(g.fixed.Get())==bytes,"ADS optic and weapon share exactly the same timing correction");
            weaponStabilityFrameBoundary(h.ctx.Get());
        };
        const float steps[]={.02f,.028f,.022f,.033f,.019f,.025f,.023f,.031f,.018f,.026f};
        for(unsigned i=0;i<10;++i)sample(steps[i],delayed,delayed && i>=4?steps[i]:0);
        if(delayed){
            for(unsigned i=0;i<4;++i)sample(.025f,true,.025f); // recognized lag survives steady velocity
            sample(-.021f,true,-.021f); // reversal still has measured one-frame correspondence
            camera[270][0]=3;sample(.019f,true,0); // changed scope cannot reuse this calibration
        }
        sample(0,false,0);sample(0,false,0); // stop and synchronized catch-up
        for(unsigned i=0;i<8;++i)sample(.025f,false,0); // constant-speed synchronized ambiguity
        for(unsigned i=0;i<10;++i)sample(steps[i],false,0); // variable synchronized motion
        h.clean();
    }
}
int main(int argc,char** argv){
    if(argc==3 && !strcmp(argv[2],"--hardware")){testDriver=D3D_DRIVER_TYPE_HARDWARE;--argc;}
    if(argc==2 && !strcmp(argv[1],"--self-test")){selfTest();emitterTest();lightTest();aimingTest();aimingTimingTest();traceTest();sustainedAimingTest();}
    else if(argc==3 && !strcmp(argv[1],"--capture"))capture(argv[2]);
    else {std::puts("Usage: weapon_stability_test --self-test [--hardware] | --capture DIR");return 2;}
    std::printf("PASS: weapon stability (%u checks)\n",checks);return 0;
}
