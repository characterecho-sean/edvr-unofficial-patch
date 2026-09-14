#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <type_traits>
#include "../../src/d3d11/gpu_timing.h"
#include "../../src/d3d11/gpu_frame_timing.h"
#include "../../src/common/gpu_frame_protocol.h"
extern "C" uint64_t WINAPI edvrGpuFrameEvent(unsigned, unsigned, uint64_t, unsigned, unsigned, void*);
#include "../../src/d3d11/gpu_interval.h"
#include "../../src/d3d11/gpu_disjoint_d3d11.h"
using Microsoft::WRL::ComPtr;
using namespace edvr;
namespace {
unsigned checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
void hr(HRESULT result){check(result==S_OK,"D3D operation failed");}
struct Runtime {
    HMODULE module=nullptr;
    decltype(&D3D11CreateDevice) create=nullptr;
    Runtime(){
        wchar_t dir[MAX_PATH]{};const UINT n=GetSystemDirectoryW(dir,MAX_PATH);
        check(n&&n<MAX_PATH,"system directory");
        const std::wstring path=std::wstring(dir)+L"\\d3d11.dll";
        module=LoadLibraryW(path.c_str());check(module!=nullptr,"system D3D11 module");
        create=reinterpret_cast<decltype(create)>(GetProcAddress(module,"D3D11CreateDevice"));
        if(!create){FreeLibrary(module);module=nullptr;throw std::runtime_error("typed D3D11CreateDevice");}
    }
    ~Runtime(){if(module)FreeLibrary(module);}
};
struct Device {
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;
    explicit Device(Runtime& rt){D3D_FEATURE_LEVEL level{};
        hr(rt.create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&level,&ctx));}
};
// Callbacks always own/operate real typed COM objects. Scripted GetData results
// test adapter failure semantics; separate unmodified calls measure real work.
struct Ops {
    struct Query {ID3D11Query* ptr=nullptr;D3D11_QUERY kind{};bool issued=false,open=false;
                  uint64_t tick=0;unsigned reads=0;};
    std::vector<Query> queries;
    unsigned allocated=0,released=0,creates=0,commands=0,active=0;
    int failCreate=-1, pendingQuery=-1, failedQuery=-1;
    bool abandoning=false, failStamp=false, disjoint=false, pendingAll=false;
    uint64_t frequency=1000;
    bool nonnullFailure=false,nullSuccess=false,bad=false,scripted=true,failBegin=false,failEnd=false;
    HRESULT failedStatus=E_FAIL;
    uint64_t tick=0;
    Query* find(ID3D11Asynchronous* q){for(auto& x:queries)if(x.ptr==q)return &x;bad=true;return nullptr;}
    static HRESULT create(void* p,ID3D11Device* d,const D3D11_QUERY_DESC* desc,ID3D11Query** out){
        auto& s=*static_cast<Ops*>(p);const int index=static_cast<int>(s.creates++);*out=nullptr;
        if(index==s.failCreate&&!s.nonnullFailure)return s.nullSuccess?S_OK:E_OUTOFMEMORY;
        const HRESULT h=d->CreateQuery(desc,out);
        if(h==S_OK&&*out){s.queries.push_back({*out,desc->Query});++s.allocated;}
        return index==s.failCreate?E_OUTOFMEMORY:h;
    }
    static HRESULT begin(void* p,ID3D11DeviceContext* c,ID3D11Asynchronous* q){
        auto& s=*static_cast<Ops*>(p);++s.commands;
        auto* x=s.find(q);if(!x||x->kind!=D3D11_QUERY_TIMESTAMP_DISJOINT||s.active){s.bad=true;return E_FAIL;}
        if(s.failBegin)return E_FAIL;
        x->open=true;x->issued=false;++s.active;c->Begin(q);return S_OK;
    }
    static HRESULT end(void* p,ID3D11DeviceContext* c,ID3D11Asynchronous* q){
        auto& s=*static_cast<Ops*>(p);++s.commands;auto* x=s.find(q);if(!x)return E_FAIL;
        if(x->kind==D3D11_QUERY_TIMESTAMP_DISJOINT){
            if(!x->open||s.active!=1){s.bad=true;return E_FAIL;}x->open=false;--s.active;
        }else if(!s.active){s.bad=true;return E_FAIL;}
        x->issued=true;x->tick=++s.tick;x->reads=0;c->End(q);
        return ((s.failEnd&&x->kind==D3D11_QUERY_TIMESTAMP_DISJOINT) ||
                (s.failStamp&&x->kind==D3D11_QUERY_TIMESTAMP)) ? E_FAIL : S_OK;
    }
    static HRESULT data(void* p,ID3D11DeviceContext* c,ID3D11Asynchronous* q,void* out,UINT size,UINT flags){
        auto& s=*static_cast<Ops*>(p);++s.commands;auto* x=s.find(q);
        if(!x||!x->issued||x->open||flags!=D3D11_ASYNC_GETDATA_DONOTFLUSH){s.bad=true;return E_FAIL;}
        const int index=static_cast<int>(x-s.queries.data());++x->reads;
        if(s.pendingAll||index==s.pendingQuery)return S_FALSE;
        if(index==s.failedQuery)return s.failedStatus;
        if(!s.scripted)return c->GetData(q,out,size,flags);
        if(x->kind==D3D11_QUERY_TIMESTAMP_DISJOINT){
            if(size!=sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT)){s.bad=true;return E_FAIL;}
            *static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*>(out)={s.frequency,s.disjoint ? TRUE : FALSE};
        }else{
            if(size!=sizeof(UINT64)){s.bad=true;return E_FAIL;}
            *static_cast<UINT64*>(out)=x->tick;
        }return S_OK;
    }
    static void release(void* p,ID3D11Query* q)noexcept{
        auto& s=*static_cast<Ops*>(p);auto* x=s.find(q);
        if(!x)s.bad=true;
        if(x&&x->open){if(!s.abandoning)s.bad=true;if(s.active)--s.active;}
        if(x)x->ptr=nullptr;
        ++s.released;q->Release();
    }
    GpuSpanD3D11Ops callbacks(){return{this,create,begin,end,data,release};}
    void clean(){check(!bad&&active==0&&allocated==released,"native query ownership/commands balanced");}
};

static_assert(std::is_trivially_destructible<GpuTimer>::value,"no context/COM work during process exit");
struct Fixture {
    Device& d; Ops ops;
    std::array<GpuTimer,40> timers;
    GpuIntervals<2> sampler;
    explicit Fixture(Device& device):d(device){check(gpuTimingBind(d.dev.Get(),d.ctx.Get(),ops.callbacks()),"bind timing owner");}
    ~Fixture(){ops.abandoning=true;gpuTimingAbandon();sampler.reset();for(auto& t:timers)t.reset();}
    ID3D11DeviceContext* ctx(){return d.ctx.Get();}
    bool begin(unsigned i){return timers[i].begin(d.dev.Get(),ctx());}
    void finish(){check(gpuTimingShutdown(ctx()),"explicit owner shutdown");sampler.reset();for(auto& t:timers)t.reset();ops.clean();}
};
void policyCases(Device& d,Runtime& runtime){
    double ms=123;
    {
        Fixture f(d);check(f.begin(0)&&f.begin(1)&&f.begin(2),"one parent plus two borrowers");
        check(f.timers[2].end(f.ctx())&&f.timers[1].end(f.ctx()),"borrowers close first");
        check(f.ops.active==1&&f.ops.allocated==7,"one disjoint plus three timestamp pairs");
        check(f.timers[1].poll(f.ctx(),ms)==GpuTimerPoll::Pending,"borrower waits for parent frequency");
        check(f.timers[0].end(f.ctx())&&f.ops.active==0,"parent closes sole physical scope");
        f.ops.pendingQuery=1;check(f.timers[0].poll(f.ctx(),ms)==GpuTimerPoll::Pending,"partial timestamp readiness");
        const auto first=f.ops.queries[0].reads,freq=f.ops.queries[2].reads,calls=f.ops.commands;
        check(!f.begin(0)&&f.ops.commands==calls,"pending timestamps cannot be reopened");f.ops.pendingQuery=-1;
        double outer=0,inner=0;check(f.timers[0].poll(f.ctx(),outer)==GpuTimerPoll::Ready,"parent completes");
        check(f.ops.queries[0].reads==first&&f.ops.queries[2].reads==freq,"ready timestamp and frequency cached");
        check(f.timers[2].poll(f.ctx(),inner)==GpuTimerPoll::Ready&&inner>0&&outer>inner,"command-derived nested intervals");
        check(f.timers[1].poll(f.ctx(),ms)==GpuTimerPoll::Ready&&f.ops.queries[2].reads==freq,"borrowers share cached frequency");
        const auto allocated=f.ops.allocated;
        check(f.begin(0)&&f.timers[0].end(f.ctx())&&f.timers[0].poll(f.ctx(),ms)==GpuTimerPoll::Ready,"standalone reuse");
        check(f.ops.allocated==allocated,"completed query allocation reused");f.finish();
    }
    {
        Fixture f(d);check(f.begin(0)&&f.begin(1)&&f.timers[0].end(f.ctx()),"early parent end");
        const auto calls=f.ops.commands;check(!f.timers[1].end(f.ctx())&&f.ops.commands==calls,"invalid borrower emits no late timestamp");
        check(f.timers[1].poll(f.ctx(),ms)==GpuTimerPoll::Invalid,"unfinished borrower invalidated");f.finish();
    }
    for(int failure=0;failure<3;++failure)for(int mode=0;mode<3;++mode){
        Fixture f(d);f.ops.failCreate=failure;f.ops.nonnullFailure=mode==1;f.ops.nullSuccess=mode==2;
        check(!f.begin(0),"partial allocation/null success fails safely");f.ops.failCreate=-1;
        check(f.begin(0)&&f.timers[0].end(f.ctx())&&f.timers[0].poll(f.ctx(),ms)==GpuTimerPoll::Ready,"failed allocation retry");f.finish();
    }
    for(int field=0;field<3;++field)for(HRESULT status:{E_FAIL,HRESULT(2)}){
        Fixture f(d);check(f.begin(0)&&f.timers[0].end(f.ctx()),"poll error sample");
        f.ops.failedQuery=field;f.ops.failedStatus=status;ms=123;
        check(f.timers[0].poll(f.ctx(),ms)==GpuTimerPoll::Invalid&&ms==123,"GetData error or unexpected success is not a measurement");f.finish();
    }
    {
        Fixture f(d);check(f.begin(0)&&f.timers[0].end(f.ctx()),"reversed timestamp sample");f.ops.queries[1].tick=0;
        check(f.timers[0].poll(f.ctx(),ms)==GpuTimerPoll::Invalid,"reversed timestamps invalid");f.finish();
    }
    {
        Fixture f(d);f.ops.failBegin=true;check(!f.begin(0)&&f.ops.active==0,"failed Begin issues no physical Begin");f.finish();
    }
    {
        Fixture f(d);f.ops.failStamp=true;
        check(!f.begin(0)&&f.ops.active==0,"failed first timestamp cancels standalone scope");
        f.ops.failStamp=false;check(f.begin(0),"first-marker failure recovers");
        f.ops.failStamp=true;check(!f.timers[0].end(f.ctx())&&f.ops.active==0,"failed end timestamp still closes frequency scope");
        check(f.timers[0].poll(f.ctx(),ms)==GpuTimerPoll::Invalid,"failed end marker consumed");
        f.ops.failStamp=false;check(f.begin(0)&&f.timers[0].end(f.ctx()),"failed End slot reusable after consumption");f.finish();
    }
    {
        Fixture f(d);check(f.begin(0),"uncertain end sample begins");f.ops.failEnd=true;
        check(!f.timers[0].end(f.ctx()),"uncertain disjoint End fails");
        check(f.timers[0].poll(f.ctx(),ms)==GpuTimerPoll::Invalid&&!f.begin(1),"uncertain closure stops later Begin");
        f.finish();
    }
    {
        Fixture f(d);Device other(runtime);ComPtr<ID3D11DeviceContext> deferred;hr(d.dev->CreateDeferredContext(0,&deferred));
        check(f.begin(0),"owner sample begins");const auto calls=f.ops.commands;
        check(!f.timers[1].begin(other.dev.Get(),other.ctx.Get())&&!f.timers[1].begin(d.dev.Get(),deferred.Get()),"foreign device/context rejected");
        f.timers[0].reset(deferred.Get());bool rejected=false;
        std::thread wrong([&]{double value=7;rejected=!f.begin(1)&&!f.timers[0].end(f.ctx())&&
            f.timers[0].poll(f.ctx(),value)==GpuTimerPoll::Pending&&!gpuTimingShutdown(f.ctx())&&!f.sampler.begin(f.ctx());
            f.sampler.poll(f.ctx());f.timers[0].reset(f.ctx());});wrong.join();
        check(rejected&&f.ops.commands==calls&&f.sampler.totals.skipped==0,"wrong OS thread leaves commands and sampler state untouched");
        check(f.timers[0].end(f.ctx()),"wrong-owner reset preserved valid open sample");f.finish();
    }
    {
        Fixture f(d);check(f.sampler.begin(f.ctx()),"sampler begins");f.sampler.end(f.ctx());const auto calls=f.ops.commands;
        for(int i=0;i<3;++i)f.sampler.poll(f.ctx());
        check(f.ops.commands==calls&&f.sampler.totals.samples==0,"four-frame lag prevents early GetData");
        f.sampler.poll(f.ctx());check(f.sampler.totals.samples==1,"fourth frame consumes sample");
        f.sampler.poll(f.ctx());check(f.sampler.totals.samples==1,"completed sample counted once");f.finish();
    }
    {
        Fixture f(d);for(unsigned i=0;i<8;++i)check(f.begin(i)&&f.timers[i].end(f.ctx()),"retain all eight disjoint records");
        check(!f.sampler.begin(f.ctx())&&f.sampler.totals.skipped==1,"record pressure skips sampler");
        for(unsigned i=0;i<8;++i)check(f.timers[i].poll(f.ctx(),ms)==GpuTimerPoll::Ready,"release record pressure");
        check(f.sampler.begin(f.ctx()),"sampler retries after transient record pressure");f.sampler.end(f.ctx());f.finish();
    }
    {
        Fixture f(d);for(unsigned i=0;i<32;++i)check(f.begin(i),"fill shared lease table");
        check(!f.sampler.begin(f.ctx()),"lease pressure skips sampler");
        check(f.timers[31].end(f.ctx()),"last borrower ends");f.timers[31].reset(f.ctx());
        check(f.sampler.begin(f.ctx()),"sampler retries after lease release");f.sampler.end(f.ctx());
        for(unsigned i=1;i<31;++i)check(f.timers[i].end(f.ctx()),"other borrowers end");check(f.timers[0].end(f.ctx()),"parent ends last");
        for(int i=0;i<4;++i)f.sampler.poll(f.ctx());
        check(f.sampler.totals.samples==1&&f.sampler.totals.skipped==1,"lease pressure preserves sampler totals");f.finish();
    }
    {
        Fixture f(d);for(unsigned i=0;i<8;++i)check(f.begin(i)&&f.timers[i].end(f.ctx()),"inactive producer retains eight records");
        check(!f.begin(8),"new producer initially sees record pressure");Sleep(2150);
        check(f.begin(8)&&f.timers[8].end(f.ctx()),"new producer retires expired inactive leases without their polling");
        for(unsigned i=0;i<8;++i)check(f.timers[i].poll(f.ctx(),ms)==GpuTimerPoll::Invalid,"expired producer cannot read recycled frequency");
        check(f.timers[8].poll(f.ctx(),ms)==GpuTimerPoll::Ready,"new producer measures after inactivity");f.finish();
    }
    {
        Fixture f(d);check(f.begin(0),"inactive producer leaves an open scope");Sleep(2150);
        check(f.begin(1)&&f.timers[1].end(f.ctx()),"other producer closes expired open scope before new scope");
        const auto calls=f.ops.commands;check(!f.timers[0].end(f.ctx())&&f.ops.commands==calls,"expired inactive owner emits no late marker");f.finish();
    }
    {
        Fixture f(d);check(f.begin(0)&&f.begin(1),"aborted nested intervals");Sleep(2050);
        check(f.timers[0].poll(f.ctx(),ms)==GpuTimerPoll::Invalid&&f.ops.active==0,"stale open parent cancelled on owner");
        const auto calls=f.ops.commands;check(!f.timers[1].end(f.ctx())&&f.ops.commands==calls,"stale borrower emits no late marker");f.finish();
    }
    {
        Fixture f(d);check(f.begin(0),"explicit cancellation sample");f.timers[0].reset(f.ctx());
        check(!f.ops.active&&gpuTimingAccepts(f.ctx()),"owner cancellation closes scope without disabling domain");f.finish();
    }
    {
        Fixture f(d);check(f.begin(0),"release-only sample");f.timers[0].reset();
        check(f.ops.active==1&&!gpuTimingAccepts(f.ctx())&&gpuTimingOwns(f.ctx()),"release-only reset stops measurement but retains owner");f.finish();
    }
    {
        Fixture f(d);check(f.sampler.begin(f.ctx())&&f.begin(0),"sampler and borrower before domain stop");
        f.timers[0].reset();f.sampler.reset(f.ctx());
        check(!gpuTimingAccepts(f.ctx())&&f.ops.released+1==f.ops.allocated,"stopped-domain explicit sampler reset releases both timestamp pairs");f.finish();
    }
    {
        Fixture f(d);check(f.begin(0),"quiescent unload sample");const auto calls=f.ops.commands;
        f.ops.abandoning=true;gpuTimingAbandon();f.timers[0].reset();
        check(f.ops.commands==calls&&!gpuTimingOwns(f.ctx()),"unload/reset issues no context commands");f.ops.clean();
    }
}
void nativeWork(Device& d){
    Fixture f(d);f.ops.scripted=false;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=128;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> source,copy,staging;ComPtr<ID3D11RenderTargetView> target;
    hr(d.dev->CreateTexture2D(&desc,nullptr,&source));hr(d.dev->CreateTexture2D(&desc,nullptr,&copy));hr(d.dev->CreateRenderTargetView(source.Get(),nullptr,&target));
    const float color[4]={0.25f,0.5f,0.75f,1.0f};check(f.begin(0)&&f.begin(1)&&f.begin(2),"native nested scopes begin");
    for(int i=0;i<24;++i){d.ctx->ClearRenderTargetView(target.Get(),color);d.ctx->CopyResource(copy.Get(),source.Get());}
    check(f.timers[2].end(f.ctx())&&f.timers[1].end(f.ctx())&&f.timers[0].end(f.ctx()),"native scopes close inside out");
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    hr(d.dev->CreateTexture2D(&desc,nullptr,&staging));d.ctx->CopyResource(staging.Get(),copy.Get());d.ctx->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};hr(d.ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));bool pixels=true;
    for(UINT y=0;y<desc.Height;++y){const auto* row=reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch);
        for(UINT x=0;x<desc.Width;++x)for(unsigned c=0;c<4;++c)pixels=pixels&&row[x*4+c]==color[c];}
    d.ctx->Unmap(staging.Get(),0);check(pixels,"exact entire texture outside measured scopes");
    double times[3]{};const auto deadline=GetTickCount64()+1500;
    for(unsigned i=0;i<3;++i){GpuTimerPoll result;do{result=f.timers[i].poll(f.ctx(),times[i]);if(result==GpuTimerPoll::Pending)Sleep(1);}
        while(result==GpuTimerPoll::Pending&&GetTickCount64()<deadline);check(result==GpuTimerPoll::Ready,"native result ready");}
    check(times[2]>0&&times[0]>=times[1]&&times[1]>=times[2],"native nested interval containment");
    check(f.ops.allocated==7,"one native disjoint query plus three timestamp pairs");f.finish();
    std::printf("PASS: native shared timers %.4f ms outer, %.4f ms nested, exact pixels\n",times[0],times[2]);
}
void nativeFrameWork(Device& d) {
    Fixture f(d); f.ops.scripted=false;
    GpuTimingFrameDriver driver;
    check(driver.bind(d.dev.Get(), f.ctx()), "native frame driver binds");
    GpuSpanState policy(driver, driver.currentOwner());
    GpuSpanState::Results results{};
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=desc.Height=64; desc.MipLevels=desc.ArraySize=1;
    desc.SampleDesc.Count=1; desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> source,copy,staging; ComPtr<ID3D11RenderTargetView> target;
    hr(d.dev->CreateTexture2D(&desc,nullptr,&source));
    hr(d.dev->CreateTexture2D(&desc,nullptr,&copy));
    hr(d.dev->CreateRenderTargetView(source.Get(),nullptr,&target));
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    hr(d.dev->CreateTexture2D(&desc,nullptr,&staging));
    const float color[4]={0.125f,0.375f,0.625f,1.0f};
    const auto owner=driver.currentOwner();
    check(policy.beginFrame(201,909,GetTickCount64(),owner)==GpuSpanReason::Valid,"native frame begins");
    GpuTimer borrower; check(borrower.begin(d.dev.Get(),f.ctx()),"native borrower begins");
    for(unsigned eye=0;eye<2;++eye) {
        check(policy.beginEye(eye,GetTickCount64(),owner)==GpuSpanReason::Valid,"native eye begins");
        for(int i=0;i<32;++i) { d.ctx->ClearRenderTargetView(target.Get(),color); d.ctx->CopyResource(copy.Get(),source.Get()); }
        check(policy.endEye(eye,GetTickCount64(),owner)==GpuSpanReason::Valid,"native eye ends");
    }
    check(borrower.end(f.ctx()),"native borrower ends");
    check(policy.finishFrame(GetTickCount64(),owner)==GpuSpanReason::Valid,"native frame finishes");
    d.ctx->CopyResource(staging.Get(),copy.Get()); d.ctx->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{}; hr(d.ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
    bool pixels=true;
    for(UINT y=0;y<desc.Height;++y) { const auto* row=reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch);
        for(UINT x=0;x<desc.Width;++x) for(unsigned c=0;c<4;++c) pixels=pixels&&row[x*4+c]==color[c]; }
    d.ctx->Unmap(staging.Get(),0); check(pixels,"native frame exact pixels");
    const auto deadline=GetTickCount64()+1500; GpuSpanReason reason=GpuSpanReason::Incomplete;
    do { if(policy.poll(GetTickCount64(),owner,results)==1) { reason=results[0].reason; break; } Sleep(1); }
    while(GetTickCount64()<deadline);
    double borrowerMs=0.0; GpuTimerPoll br;
    do { br=borrower.poll(f.ctx(),borrowerMs); if(br==GpuTimerPoll::Pending) Sleep(1); }
    while(br==GpuTimerPoll::Pending&&GetTickCount64()<deadline);
    check(reason==GpuSpanReason::Valid&&results[0].sourceFrame==909,"native frame result metadata");
    check(br==GpuTimerPoll::Ready&&borrowerMs>0.0,"native borrower ready");
    borrower.reset(f.ctx()); policy.shutdown(GetTickCount64(),owner); driver.reset(f.ctx()); f.finish();
}
void frameDriverCases(Device& d) {
    {
        Fixture f(d); GpuTimingFrameDriver driver;
        check(driver.bind(d.dev.Get(),f.ctx()),"frame driver binds shared domain");
        GpuSpanState policy(driver,driver.currentOwner()); GpuSpanState::Results results{};
        const auto owner=driver.currentOwner();
        check(policy.beginFrame(101,77,GetTickCount64(),owner)==GpuSpanReason::Valid,"frame driver begins source frame");
        check(f.begin(0),"existing pair borrows frame scope");
        for(unsigned eye : {1u,0u}) {
            check(policy.beginEye(eye,GetTickCount64(),owner)==GpuSpanReason::Valid,"reversed eye starts");
            check(policy.endEye(eye,GetTickCount64(),owner)==GpuSpanReason::Valid,"reversed eye ends");
        }
        check(f.timers[0].end(f.ctx()),"borrower ends before parent");
        check(f.ops.active==1&&f.ops.allocated==9,"six markers plus borrowed pair use one disjoint");
        check(policy.finishFrame(GetTickCount64(),owner)==GpuSpanReason::Valid,"frame finishes");
        f.ops.pendingQuery=5;
        check(policy.poll(GetTickCount64(),owner,results)==0,"issued right-end timestamp stays pending");
        const auto read0=f.ops.queries[0].reads,readFreq=f.ops.queries[6].reads;
        f.ops.pendingQuery=-1;
        check(policy.poll(GetTickCount64(),owner,results)==1,"partial frame becomes ready");
        check(f.ops.queries[0].reads==read0&&f.ops.queries[6].reads==readFreq,"ready frame timestamp/frequency cached");
        const auto& r=results[0];
        check(r.sequence==101&&r.sourceFrame==77&&r.reason==GpuSpanReason::Valid,"original metadata preserved");
        check(r.outerMs==7&&r.leftMs==1&&r.rightMs==1,"durations derived from issued command positions");
        double ms=0;check(f.timers[0].poll(f.ctx(),ms)==GpuTimerPoll::Ready&&ms==5,"borrower independently reads same frequency");
        const auto calls=f.ops.commands;
        bool rejected=false;
        std::thread wrong([&]{GpuSpanRawSample raw{};
            rejected=driver.currentOwner().thread==GetCurrentThreadId()&&
                policy.beginFrame(102,78,GetTickCount64(),driver.currentOwner())==GpuSpanReason::WrongOwner&&
                !driver.create(1)&&!driver.begin(0)&&driver.poll(0,raw)==GpuSpanPoll::Pending;
            driver.destroy(0);driver.reset(f.ctx());});wrong.join();
        check(rejected&&f.ops.commands==calls,"actual thread gates policy and driver before mutation");
        policy.shutdown(GetTickCount64(),owner);driver.reset(f.ctx());f.finish();
    }
    for(int allocation=0;allocation<7;++allocation) for(int mode=0;mode<3;++mode) {
        Fixture f(d); f.ops.failCreate=allocation;f.ops.nonnullFailure=mode==1;f.ops.nullSuccess=mode==2;
        GpuTimingFrameDriver driver;check(driver.bind(d.dev.Get(),f.ctx()),"allocation case bind");
        GpuSpanState p(driver,driver.currentOwner());const auto owner=driver.currentOwner();
        const auto reason=p.beginFrame(1,55,GetTickCount64(),owner);
        check(reason==GpuSpanReason::CreateFailed||reason==GpuSpanReason::DriverFailure,"partial/null-success allocation rejected");
        p.shutdown(GetTickCount64(),owner);driver.reset(f.ctx());f.finish();
    }
    for(int invalid=0;invalid<4;++invalid) {
        Fixture f(d);GpuTimingFrameDriver driver;check(driver.bind(d.dev.Get(),f.ctx()),"invalid frame bind");
        GpuSpanState p(driver,driver.currentOwner());const auto owner=driver.currentOwner();GpuSpanState::Results out{};
        check(p.beginFrame(1,66,GetTickCount64(),owner)==GpuSpanReason::Valid,"invalid case starts");
        for(unsigned eye=0;eye<2;++eye) {p.beginEye(eye,GetTickCount64(),owner);p.endEye(eye,GetTickCount64(),owner);}
        f.ops.disjoint=invalid==0;f.ops.frequency=invalid==1?0:1000;
        if(invalid==2) {f.ops.failedQuery=4;f.ops.failedStatus=HRESULT(2);}
        if(invalid==3) f.ops.failEnd=true;
        const auto reason=p.finishFrame(GetTickCount64(),owner);
        check(reason==(invalid==3?GpuSpanReason::DriverFailure:GpuSpanReason::Valid),"end status distinct");
        check(p.poll(GetTickCount64(),owner,out)==1,"invalid frame consumed once");
        const auto expected=invalid==0?GpuSpanReason::Disjoint:invalid==1?GpuSpanReason::ZeroFrequency:GpuSpanReason::DriverFailure;
        check(out[0].reason==expected&&out[0].outerMs==0,"disjoint/error is unavailable not a duration");
        if(invalid==3) {
            const auto calls=f.ops.commands;
            check(p.beginFrame(2,67,GetTickCount64(),owner)==GpuSpanReason::Stopped,"uncertain End permanently stops ring");
            p.shutdown(GetTickCount64(),owner);driver.reset(f.ctx());
            check(f.ops.commands==calls,"uncertain End never retried at cleanup");
        } else {p.shutdown(GetTickCount64(),owner);driver.reset(f.ctx());}
        f.finish();
    }
    {
        Fixture f(d);GpuTimingFrameDriver driver;check(driver.bind(d.dev.Get(),f.ctx()),"pressure bind");
        GpuSpanState p(driver,driver.currentOwner());const auto owner=driver.currentOwner();GpuSpanState::Results out{};
        check(f.begin(0),"standalone sample active");
        check(p.beginFrame(1,1,GetTickCount64(),owner)==GpuSpanReason::DriverFailure,"frame cannot borrow standalone scope");
        check(f.ops.active==1,"rejected frame left standalone intact");
        check(f.timers[0].end(f.ctx()),"standalone closes");double ms=0;f.timers[0].poll(f.ctx(),ms);
        p.poll(GetTickCount64(),owner,out);
        f.ops.pendingAll=true;
        for(uint64_t n=2;n<10;++n) {
            check(p.beginFrame(n,n+100,GetTickCount64(),owner)==GpuSpanReason::Valid,"bounded slot starts");
            for(unsigned eye=0;eye<2;++eye) {p.beginEye(eye,GetTickCount64(),owner);p.endEye(eye,GetTickCount64(),owner);}
            p.finishFrame(GetTickCount64(),owner);
        }
        check(p.beginFrame(10,110,GetTickCount64(),owner)==GpuSpanReason::RingFull,"unread frame slots never reused");
        check(p.poll(GetTickCount64(),owner,out)==0,"all pending frames retained");
        f.ops.pendingAll=false;
        check(p.poll(GetTickCount64(),owner,out)==8,"all pressure results settle");
        for(const auto& r:out) check(r.reason==GpuSpanReason::Valid&&r.sourceFrame==r.sequence+100,"pressure results keep identities");
        p.shutdown(GetTickCount64(),owner);driver.reset(f.ctx());f.finish();
    }
    for(bool open:{false,true}) {
        Fixture f(d);GpuTimingFrameDriver driver;check(driver.bind(d.dev.Get(),f.ctx()),"abandon bind");
        check(driver.create(0)&&driver.begin(0)&&driver.timestamp(0,0),"abandon sample starts");
        if(!open) check(driver.end(0),"pending sample closes");
        const auto calls=f.ops.commands; f.ops.abandoning=true;
        driver.reset();
        check(f.ops.commands==calls&&!gpuTimingAccepts(f.ctx()),"no-context reset issues no clock/context command");
        // Explicit verified owner shutdown remains available to close a scope
        // abandoned on the owner thread without a context parameter.
        f.finish();
    }
}
uint64_t event(GpuFrameEvent e,uint64_t seq=0,unsigned eye=0,unsigned flags=0,void* tex=nullptr) {
    return edvrGpuFrameEvent(kGpuFrameProtocol,static_cast<unsigned>(e),seq,eye,flags,tex);
}
uint64_t poses() {
    const auto seq=event(GpuFrameEvent::WaitBegin);
    check(seq&&event(GpuFrameEvent::WaitEnd,seq,0,1),"CPU pose mailbox arms sequence");return seq;
}
void controllerCases(Device& d,Runtime& runtime) {
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=16;td.MipLevels=td.ArraySize=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> tex;hr(d.dev->CreateTexture2D(&td,nullptr,&tex));
    Fixture f(d);check(gpuFrameBind(d.dev.Get(),f.ctx(),true),"controller binds actual shared owner");
    auto finishEye=[&](uint64_t seq,unsigned eye,bool accepted=true) {
        check(event(GpuFrameEvent::SubmitBegin,seq,eye,0,tex.Get())==1,"submit path begins");
        return event(GpuFrameEvent::SubmitEnd,seq,eye,accepted?1:0);
    };
    auto completed=[&](uint64_t seq,uint64_t next,GpuSpanReason reason) {
        gpuFramePresent(f.ctx(),next);const auto snap=gpuFrameSnapshot();
        check(snap.enabled&&snap.haveResult&&snap.result.sequence==seq&&snap.result.reason==reason,"result identity and validity");
        if(reason!=GpuSpanReason::Valid) check(snap.result.outerMs==0,"invalid result has no duration");
        return snap;
    };
    gpuFramePresent(f.ctx(),900);
    auto seq=poses();const auto commands=f.ops.commands;
    check(!edvrGpuFrameEvent(999,0,0,0,0,nullptr)&&f.ops.commands==commands,"version mismatch issues nothing");
    check(f.ops.commands==commands,"pose mailbox never issues D3D work");
    gpuFrameCommand(f.ctx());const auto began=f.ops.commands;
    for(int n=0;n<30;++n) gpuFrameCommand(f.ctx());
    check(f.ops.commands==began&&f.ops.active==1,"only first covered command starts frame");
    check(finishEye(seq,1)&&finishEye(seq,0),"reversed pair accepted");
    auto snap=completed(seq,901,GpuSpanReason::Valid);
    check(snap.result.sourceFrame==900&&snap.result.outerMs==5,"result uses original source frame and command interval");
    gpuFrameCommand(f.ctx());check(f.ops.active==0,"mirror commands never reopen completed frame");

    seq=poses();check(!event(GpuFrameEvent::SubmitBegin,seq,0,0,tex.Get()),"submit without covered command rejected");
    gpuFrameCommand(f.ctx());completed(seq,902,GpuSpanReason::NoOpenFrame);

    seq=poses();gpuFrameCommand(f.ctx());check(finishEye(seq,0),"missing-eye first submit");
    completed(seq,903,GpuSpanReason::Incomplete);

    seq=poses();gpuFrameCommand(f.ctx());check(finishEye(seq,0),"duplicate-eye first submit");
    check(!event(GpuFrameEvent::SubmitBegin,seq,0,0,tex.Get()),"duplicate eye rejected");
    completed(seq,904,GpuSpanReason::BadPair);

    seq=poses();gpuFrameCommand(f.ctx());check(!finishEye(seq,0,false),"runtime rejected submit invalidates");
    completed(seq,905,GpuSpanReason::Incomplete);

    seq=poses();gpuFrameCommand(f.ctx());check(finishEye(seq,0)&&finishEye(seq,1),"closed pair before late duplicate");
    check(!event(GpuFrameEvent::SubmitBegin,seq,1,0,tex.Get()),"duplicate after outer closure rejected");
    completed(seq,906,GpuSpanReason::NoOpenFrame);

    seq=poses();gpuFrameCommand(f.ctx());const auto calls=f.ops.commands;
    bool rejected=false;
    std::thread wrong([&]{gpuFrameCommand(f.ctx());rejected=!event(GpuFrameEvent::SubmitBegin,seq,0,0,tex.Get());});wrong.join();
    check(rejected&&f.ops.commands==calls,"foreign render thread only invalidates CPU mailbox");
    completed(seq,907,GpuSpanReason::Incomplete);

    seq=poses();gpuFrameCommand(f.ctx());
    Device other(runtime);ComPtr<ID3D11Texture2D> foreign;hr(other.dev->CreateTexture2D(&td,nullptr,&foreign));
    const auto foreignCalls=f.ops.commands;gpuFrameCommand(other.ctx.Get());
    check(f.ops.commands==foreignCalls,"unrelated context ignored");
    check(!event(GpuFrameEvent::SubmitBegin,seq,0,0,foreign.Get()),"foreign texture device rejected");
    completed(seq,908,GpuSpanReason::Incomplete);

    seq=poses();gpuFrameCommand(f.ctx());check(finishEye(seq,0),"incomplete old boundary");
    const auto next=poses();gpuFrameCommand(f.ctx());
    check(finishEye(next,0)&&finishEye(next,1),"new pose starts independent frame");
    completed(next,909,GpuSpanReason::Valid);

    const auto failed=event(GpuFrameEvent::WaitBegin);event(GpuFrameEvent::WaitEnd,failed,0,0);
    check(gpuFrameSnapshot().result.reason!=GpuSpanReason::Valid,"failed pose wait immediately clears previous valid readout");
    const auto beforeFailed=f.ops.commands;gpuFrameCommand(f.ctx());
    check(f.ops.active==0&&f.ops.commands==beforeFailed,"failed pose wait stays disarmed");
    const auto old=event(GpuFrameEvent::WaitBegin),newer=event(GpuFrameEvent::WaitBegin);
    check(!event(GpuFrameEvent::WaitEnd,old,0,1)&&event(GpuFrameEvent::WaitEnd,newer,0,1),"out-of-order wait cannot arm stale sequence");
    gpuFrameCommand(f.ctx());finishEye(newer,0);finishEye(newer,1);completed(newer,910,GpuSpanReason::Valid);

    gpuFrameConfigure(false);const auto disabledCalls=f.ops.commands;
    seq=poses();gpuFrameCommand(f.ctx());event(GpuFrameEvent::SubmitBegin,seq,0,0,tex.Get());
    check(!gpuFrameSnapshot().enabled&&f.ops.commands==disabledCalls,"disabled local instrument issues no query commands");
    gpuFrameConfigure(true);gpuFrameCommand(f.ctx());check(f.ops.active==0,"reenable waits for new pose boundary");
    seq=poses();gpuFrameCommand(f.ctx());finishEye(seq,0);finishEye(seq,1);completed(seq,911,GpuSpanReason::Valid);
    seq=poses();gpuFrameCommand(f.ctx());const auto beforePresent=f.ops.commands;
    std::thread presentThread([&]{gpuFramePresent(f.ctx(),920);});presentThread.join();
    check(f.ops.commands==beforePresent,"foreign Present only publishes CPU identity and rejection");
    seq=poses();gpuFrameCommand(f.ctx());finishEye(seq,0);finishEye(seq,1);
    check(completed(seq,921,GpuSpanReason::Valid).result.sourceFrame==920,"new frame retains published identity after foreign Present");
    gpuFrameConfigure(false);gpuFramePresent(f.ctx(),922);gpuFrameAbandon();f.finish();
}
void run(){Runtime runtime;Device device(runtime);policyCases(device,runtime);frameDriverCases(device);controllerCases(device,runtime);nativeWork(device);nativeFrameWork(device);std::printf("PASS: %u shared GPU timer lifecycle checks\n",checks);}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && !wcscmp(argv[1], L"--dry-run")) {
        std::puts("dry-run: no module, device, queries or files");
        return 0;
    }
    if (argc == 2 && !wcscmp(argv[1], L"--child")) {
        try { run(); return 0; }
        catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: %s\n", error.what());
            return 1;
        }
    }
    if (argc != 2 || wcscmp(argv[1], L"--self-test")) {
        std::fputs("usage: --self-test | --dry-run\n", stderr);
        return 2;
    }
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return 2;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --child";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessW(executable, &command[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return 2;
    const DWORD waited = WaitForSingleObject(process.hProcess, 30000);
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    else {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
        std::fputs("FAIL: owned test child timed out\n", stderr);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (code != 0) std::fprintf(stderr, "FAIL: owned D3D11 child exited 0x%08lX\n", code);
    return code == 0 ? 0 : 1;
}
