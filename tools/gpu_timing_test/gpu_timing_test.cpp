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
#include <memory>
#include "../../src/d3d11/gpu_timing.h"
#include "../../src/d3d11/gpu_frame_timing.h"
#include "../../src/common/gpu_frame_protocol.h"
extern "C" uint64_t WINAPI edvrGpuFrameEvent(unsigned, unsigned, uint64_t, unsigned, unsigned, void*);
#include "../../src/d3d11/gpu_interval.h"
#include "../../src/d3d11/ui_layer_draw_timing.h"
#include "../../src/d3d11/ui_layer_seed_timing.h"
#include "../../src/d3d11/gpu_disjoint_d3d11.h"
#include "../../src/common/system_d3d11.h"
using Microsoft::WRL::ComPtr;
using namespace edvr;
namespace {
unsigned checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
void hr(HRESULT result){check(result==S_OK,"D3D operation failed");}
struct Runtime {
    decltype(&D3D11CreateDevice) create=systemD3D11CreateDevice();
    Runtime(){check(create!=nullptr,"System32 D3D11CreateDevice");}
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
    bool abandoning=false, failStamp=false, disjoint=false, pendingAll=false, faultNextStamp=false;
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
        if(s.faultNextStamp&&x->kind==D3D11_QUERY_TIMESTAMP){
            s.faultNextStamp=false;
            RaiseException(0xE042ED50u,0,0,nullptr); // before native End returns/first marker issues
        }
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
    std::array<GpuTimer, DisjointClock::kLeases + 8> timers;
    GpuIntervals<2> sampler;
    explicit Fixture(Device& device):d(device){check(gpuTimingBind(d.dev.Get(),d.ctx.Get(),ops.callbacks()),"bind timing owner");}
    ~Fixture(){ops.abandoning=true;gpuTimingAbandon();sampler.reset();for(auto& t:timers)t.reset();}
    ID3D11DeviceContext* ctx(){return d.ctx.Get();}
    bool begin(unsigned i){return timers[i].begin(d.dev.Get(),ctx());}
    bool beginBorrowed(unsigned i){return timers[i].beginBorrowedFrame(d.dev.Get(),ctx());}
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
        Fixture f(d);for(unsigned i=0;i<DisjointClock::kLeases;++i)check(f.begin(i),"fill shared lease table");
        check(!f.sampler.begin(f.ctx()),"lease pressure skips sampler");
        check(f.timers[DisjointClock::kLeases-1].end(f.ctx()),"last borrower ends");f.timers[DisjointClock::kLeases-1].reset(f.ctx());
        check(f.sampler.begin(f.ctx()),"sampler retries after lease release");f.sampler.end(f.ctx());
        for(unsigned i=1;i<DisjointClock::kLeases-1;++i)check(f.timers[i].end(f.ctx()),"other borrowers end");check(f.timers[0].end(f.ctx()),"parent ends last");
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
        Fixture f(d);
        const auto noParentCommands=f.ops.commands;
        check(!f.beginBorrowed(0)&&f.ops.active==0&&f.ops.commands==noParentCommands,
              "borrowed-only timer rejects no parent without query markers");
        check(f.begin(1),"standalone timer opens for borrowed-only rejection");
        const auto standaloneCommands=f.ops.commands;
        check(!f.beginBorrowed(0)&&f.ops.active==1&&f.ops.commands==standaloneCommands,
              "borrowed-only timer rejects a private standalone parent");
        check(f.timers[1].end(f.ctx()),"standalone rejection fixture closes");
        double standaloneMs=0;
        check(f.timers[1].poll(f.ctx(),standaloneMs)==GpuTimerPoll::Ready,
              "standalone rejection fixture retires");

        GpuTimingFrameDriver driver;
        check(driver.bind(d.dev.Get(),f.ctx())&&driver.create(0)&&driver.begin(0)&&
              driver.timestamp(0,0),"shared frame opens for borrowed-only timer");
        const auto sharedCommands=f.ops.commands;
        check(f.beginBorrowed(0)&&f.ops.active==1&&f.ops.commands==sharedCommands+1,
              "borrowed-only timer adds one timestamp and no disjoint Begin");
        check(f.timers[0].end(f.ctx())&&driver.timestamp(0,1)&&driver.end(0),
              "borrowed-only timer and shared frame close");
        GpuSpanRawSample raw{};
        check(driver.poll(0,raw)==GpuSpanPoll::Ready&&raw.frequency!=0,
              "shared frame frequency is available to borrower");
        double borrowedMs=0;
        check(f.timers[0].poll(f.ctx(),borrowedMs)==GpuTimerPoll::Ready&&borrowedMs>0,
              "borrowed-only timer produces a valid duration");
        driver.destroy(0);driver.reset(f.ctx());f.finish();
    }
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
        check(f.ops.active==1&&f.ops.allocated==11,"eight allocated frame timestamps plus borrowed pair use one disjoint");
        check(policy.finishFrame(GetTickCount64(),owner)==GpuSpanReason::Valid,"frame finishes");
        f.ops.pendingQuery=5;
        check(policy.poll(GetTickCount64(),owner,results)==0,"issued right-end timestamp stays pending");
        const auto read0=f.ops.queries[0].reads,readFreq=f.ops.queries[8].reads;
        f.ops.pendingQuery=-1;
        check(policy.poll(GetTickCount64(),owner,results)==1,"partial frame becomes ready");
        check(f.ops.queries[0].reads==read0&&f.ops.queries[8].reads==readFreq,"ready frame timestamp/frequency cached");
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
    for(int allocation=0;allocation<9;++allocation) for(int mode=0;mode<3;++mode) {
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
    check(seq&&event(GpuFrameEvent::WaitEnd,seq,0,kGpuFrameNativeWait),"CPU pose mailbox arms native sequence");
    check(bool(event(GpuFrameEvent::SegmentResume,seq))==gpuFrameSnapshot().enabled,
          "producer return admits rendering only with GPU timing enabled");
    return seq;
}
void controllerCases(Device& d,Runtime& runtime) {
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=16;td.MipLevels=td.ArraySize=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> tex;hr(d.dev->CreateTexture2D(&td,nullptr,&tex));
    Fixture f(d);check(gpuFrameBind(d.dev.Get(),f.ctx(),true),"controller binds actual shared owner");
    uint64_t pairSequence=0;unsigned pairMask=0;
    auto finishEye=[&](uint64_t seq,unsigned eye,bool accepted=true) {
        if(pairSequence!=seq){pairSequence=seq;pairMask=0;}
        check(event(GpuFrameEvent::SegmentPause,seq)==1,"game segment ends before submit route");
        check(event(GpuFrameEvent::SubmitBegin,seq,eye,0,tex.Get())==1,"submit path begins");
        check(event(GpuFrameEvent::SegmentEnd,seq,eye,1,tex.Get())==1,"treatment timestamp precedes transfer");
        const auto result=event(GpuFrameEvent::SubmitEnd,seq,eye,accepted?1:0);
        if(result) {
            pairMask|=1u<<eye;
            if(pairMask!=3)check(event(GpuFrameEvent::SegmentResume,seq)==1,"first submit return admits between-eye work");
        }
        return result;
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
    check(snap.result.sourceFrame==900&&snap.result.source==GpuSpanSource::ApplicationRender&&
          snap.result.outerMs==3,"result uses original frame and three non-overlapping command intervals");
    gpuFrameCommand(f.ctx());check(f.ops.active==0,"mirror commands never reopen completed frame");

    seq=poses();check(!event(GpuFrameEvent::SubmitBegin,seq,0,0,tex.Get()),"submit without covered command rejected");
    gpuFrameCommand(f.ctx());completed(seq,902,GpuSpanReason::NoOpenFrame);

    seq=poses();gpuFrameCommand(f.ctx());check(finishEye(seq,0),"missing-eye first submit");
    completed(seq,903,GpuSpanReason::Incomplete);

    seq=poses();gpuFrameCommand(f.ctx());check(finishEye(seq,0),"duplicate-eye first submit");
    check(!event(GpuFrameEvent::SubmitBegin,seq,0,0,tex.Get()),"duplicate eye rejected");
    completed(seq,904,GpuSpanReason::Incomplete);

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
    check(!event(GpuFrameEvent::WaitEnd,old,0,kGpuFrameNativeWait)&&event(GpuFrameEvent::WaitEnd,newer,0,kGpuFrameNativeWait),"out-of-order wait cannot arm stale sequence");
    check(event(GpuFrameEvent::SegmentResume,newer)==1,"newer route return admits producer commands");
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
    const auto legacy=event(GpuFrameEvent::WaitBegin);
    check(event(GpuFrameEvent::WaitEnd,legacy,0,1)==1,"legacy wait keeps its original measurement contract");
    gpuFrameCommand(f.ctx());
    for(unsigned eye=0;eye<2;++eye) {
        check(event(GpuFrameEvent::SubmitBegin,legacy,eye,0,tex.Get())==1&&
              event(GpuFrameEvent::SubmitEnd,legacy,eye,1)==1,"legacy submit markers remain complete");
    }
    auto legacyResult=completed(legacy,922,GpuSpanReason::Valid).result;
    check(legacyResult.source==GpuSpanSource::RenderToSubmit&&legacyResult.outerMs==5,
          "legacy outer span remains explicitly distinct from native application intervals");
    seq=poses();gpuFrameCommand(f.ctx());finishEye(seq,0);finishEye(seq,1);
    auto nativeResult=completed(seq,923,GpuSpanReason::Valid).result;
    check(nativeResult.source==GpuSpanSource::ApplicationRender&&nativeResult.outerMs==3,
          "native measurement after legacy frame still excludes gaps");
    gpuFrameConfigure(false);gpuFramePresent(f.ctx(),924);gpuFrameAbandon();f.finish();
}
void hdrDrawRouteCases(Device& d) {
    // This is the same scope helper called after the live HDR target bind and
    // before restore in uiLayerEnd/guarded decline. The backend is production
    // GpuTimer, typed WARP COM objects and the shared disjoint lease domain.
    {
        Fixture f(d);
        UiDrawRouteScope scope;
        unsigned begins = 0, ends = 0, missing = 0;
        auto begin = [&](UiRouteStage stage, int eye, uint64_t seq) {
            check(stage == UiRouteStage::kHdrMovedDraw && eye == 0 && seq == 7,
                  "live helper carries the moved stage, eye and frame into timer begin");
            ++begins;
            return f.begin(1) ? 1 : -1;
        };
        auto end = [&](int slot) { ++ends; check(f.timers[slot].end(f.ctx()), "live helper closes real timer"); };
        auto lost = [&](int eye, uint64_t seq) { check(eye == 0 && seq == 7, "disabled coverage retains eye/frame"); ++missing; };
        const auto commands = f.ops.commands, allocations = f.ops.allocated;
        check(scope.begin(false, true, 0, true, 0, 7, begin, lost) == UiDrawRouteStart::Declined,
              "refused live bind opens no interval");
        scope.end(end);
        for (unsigned i = 0; i < 40; ++i) {
            check(scope.begin(true, true, 0, false, 0, 7, begin, lost) == UiDrawRouteStart::Disabled,
                  "diagnostics-off HDR bind remains untimed");
            scope.end(end);
        }
        check(!begins && !ends && missing == 40 && f.ops.commands == commands &&
                  f.ops.allocated == allocations,
              "40 off-window HUD draws add zero query allocations or commands");
        check(scope.begin(true, false, 0, true, 0, 7, begin, lost) == UiDrawRouteStart::Unchanged,
              "ordinary LDR primary rendering keeps existing timing policy");
        scope.end(end);
        check(scope.begin(true, true, 0, true, 0, 7, begin, lost) == UiDrawRouteStart::Opened,
              "diagnostic accepted HDR draw opens production interval");
        check(scope.begin(true, true, 0, true, 0, 7, begin, lost) == UiDrawRouteStart::Declined,
              "nested bind cannot overwrite an unfinished draw timer");
        scope.end(end); scope.end(end);
        double ms = 0;
        check(begins == 1 && ends == 1 && !scope.active && scope.slot == -1 &&
                  f.timers[1].poll(f.ctx(), ms) == GpuTimerPoll::Ready && ms > 0,
              "normal and guarded-cleanup duplicate ends issue one timestamp and measure");
        check(scope.begin(true, true, 0, true, 0, 7,
                          [](UiRouteStage, int, uint64_t) { return -1; }, lost) == UiDrawRouteStart::Unavailable,
              "bounded ring or lease decline remains unavailable");
        scope.end(end);
        check(ends == 1, "an unavailable timer never emits an End command");
        UiRouteStage actual = UiRouteStage::kClear;
        check(scope.begin(true, true, 1, false, 0, 7,
                          [&](UiRouteStage stage, int, uint64_t) { actual = stage; return f.begin(2) ? 2 : -1; },
                          lost) == UiDrawRouteStart::Opened && actual == UiRouteStage::kMultiply,
              "multiply machinery remains timed with diagnostics off");
        scope.end(end);
        check(f.timers[2].poll(f.ctx(), ms) == GpuTimerPoll::Ready, "multiply policy still measures");
        check(scope.begin(true, true, 0, true, -1, 7, begin, lost) == UiDrawRouteStart::Declined &&
                  scope.begin(true, true, 0, true, 0, 0, begin, lost) == UiDrawRouteStart::Declined,
              "invalid eye/frame identity declines before callbacks");
        check(scope.begin(true, true, 0, true, 0, 7, begin, lost) == UiDrawRouteStart::Opened,
              "a scope opens before a simulated guarded-bind fault");
        unsigned cancelled = 0;
        scope.cancel([&](int slot) { ++cancelled; f.timers[slot].reset(f.ctx()); });
        scope.cancel([&](int) { ++cancelled; }); scope.end(end);
        check(cancelled == 1 && ends == 2 && f.timers[1].poll(f.ctx(), ms) == GpuTimerPoll::Invalid,
              "failed bind cancels once and cannot masquerade as measured zero work");
        f.finish();
    }
    {
        Fixture f(d);
        check(f.begin(0), "outer application scope for 40 moved draws");
        const auto commands = f.ops.commands;
        for (unsigned i = 1; i <= 40; ++i) {
            UiDrawRouteScope scope;
            check(scope.begin(true, true, 0, true, int(i & 1), 8,
                              [&](UiRouteStage, int, uint64_t) { return f.begin(i) ? int(i) : -1; },
                              [](int, uint64_t) {}) == UiDrawRouteStart::Opened, "40 draws fit shared lease budget");
            scope.end([&](int slot) { check(f.timers[slot].end(f.ctx()), "draw ends before outer scope"); });
        }
        check(f.ops.commands == commands + 80 && f.ops.active == 1,
              "40 diagnostic HDR draws add 80 timestamps and no disjoint scopes");
        check(f.timers[0].end(f.ctx()), "outer frame closes shared frequency");
        for (unsigned i = 0; i <= 40; ++i) { double ms = 0;
            check(f.timers[i].poll(f.ctx(), ms) == GpuTimerPoll::Ready, "all bounded draw samples retire"); }
        f.finish();
    }
    {
        Fixture f(d); f.ops.scripted = false;
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = desc.Height = 64;
        desc.MipLevels = desc.ArraySize = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D11Texture2D> src, dst;
        hr(d.dev->CreateTexture2D(&desc, nullptr, &src)); hr(d.dev->CreateTexture2D(&desc, nullptr, &dst));
        UiDrawRouteScope scope;
        check(scope.begin(true, true, 0, true, 1, 9,
                          [&](UiRouteStage, int, uint64_t) { return f.begin(0) ? 0 : -1; },
                          [](int, uint64_t) {}) == UiDrawRouteStart::Opened, "real WARP diagnostic scope opens");
        f.ctx()->CopyResource(dst.Get(), src.Get());
        scope.end([&](int slot) { check(f.timers[slot].end(f.ctx()), "real WARP draw-scope end"); });
        // Flush belongs to this headless fixture, never the live route helper.
        f.ctx()->Flush();
        const uint64_t deadline = GetTickCount64() + 5000;
        GpuTimerPoll result = GpuTimerPoll::Pending; double ms = -1;
        do { result = f.timers[0].poll(f.ctx(), ms); if (result == GpuTimerPoll::Pending) Sleep(1); }
        while (result == GpuTimerPoll::Pending && GetTickCount64() < deadline);
        check(result == GpuTimerPoll::Ready && ms >= 0, "live scope helper measures actual WARP GPU work");
        f.finish();
    }
    {
        Fixture f(d);
        check(f.begin(0), "on/off/on samples borrow an outer scope before delayed polling");
        struct Pending { unsigned timer; uint64_t seq; UiRouteStage stage; UiRouteCoverageFlags coverage; };
        std::vector<Pending> pending;
        UiRouteCoverage coverage;
        unsigned timer = 1;
        bool diagnostics = true;
        for (uint64_t seq = 20; seq <= 22; ++seq) {
            diagnostics = seq != 21;
            const unsigned machineryTimer = timer++;
            check(f.begin(machineryTimer) && f.timers[machineryTimer].end(f.ctx()), "queued machinery interval");
            pending.push_back({machineryTimer, seq, UiRouteStage::kHdrSeed,
                               coverage.begin(UiRouteStage::kHdrSeed, 0, seq, diagnostics)});
            UiDrawRouteScope scope;
            const auto start = scope.begin(true, true, 0, diagnostics, 0, seq,
                [&](UiRouteStage stage, int eye, uint64_t frame) {
                    const unsigned drawTimer = timer++;
                    pending.push_back({drawTimer, frame, stage, coverage.begin(stage, eye, frame, diagnostics)});
                    return f.begin(drawTimer) ? int(drawTimer) : -1;
                }, [&](int eye, uint64_t frame) { coverage.missing(UiRouteStage::kHdrMovedDraw, eye, frame); });
            check(start == (diagnostics ? UiDrawRouteStart::Opened : UiDrawRouteStart::Disabled),
                  "direct diagnostics gate follows on/off/on without flushing old samples");
            scope.end([&](int index) { check(f.timers[index].end(f.ctx()), "queued moved interval ends"); });
        }
        diagnostics = false;  // current setting differs from the pending samples' armed epochs
        f.ops.pendingAll = true; double ms = 0;
        check(f.timers[pending[0].timer].poll(f.ctx(), ms) == GpuTimerPoll::Pending,
              "toggle test retains an oldest pending sample");
        f.ops.pendingAll = false;
        check(f.timers[0].end(f.ctx()), "delayed sample frequency closes");
        UiRouteFrameTotals totals; double machinery = -1, combined = -1;
        unsigned closedArmed = 0, closedUnarmed = 0;
        for (const auto& p : pending) {
            check(f.timers[p.timer].poll(f.ctx(), ms) == GpuTimerPoll::Ready, "toggle sample resolves without forced flush");
            const unsigned closed = totals.add(p.stage, p.seq, ms, true, p.coverage.combined,
                                                machinery, combined, p.coverage.machinery);
            if (closed == 3) { ++closedArmed; check(machinery == 1 && combined == 2, "earlier armed frame prices both categories"); }
            if (closed == 1) { ++closedUnarmed; check(machinery == 1, "off epoch retains only machinery cost"); }
        }
        check(!diagnostics && closedArmed == 1 && closedUnarmed == 1 &&
                  totals.close(23, machinery, combined) == 3 && machinery == 1 && combined == 2,
              "on/off/on pending queries retain armed samples and exclude unarmed eye-frames");
        f.finish();
    }
    {
        struct Queued { UiRouteStage stage; int eye; uint64_t seq; UiRouteCoverageFlags flags; };
        UiRouteCoverage coverage;
        Queued queued[] = {
            {UiRouteStage::kHdrSeed, 0, 30, coverage.begin(UiRouteStage::kHdrSeed, 0, 30, true)},
            {UiRouteStage::kComposite, 0, 30, coverage.begin(UiRouteStage::kComposite, 0, 30, true)},
            {UiRouteStage::kHdrSeed, 0, 31, coverage.begin(UiRouteStage::kHdrSeed, 0, 31, true)},
            {UiRouteStage::kComposite, 0, 31, coverage.begin(UiRouteStage::kComposite, 0, 31, true)},
            {UiRouteStage::kHdrSeed, 1, 30, coverage.begin(UiRouteStage::kHdrSeed, 1, 30, true)}
        };
        UiRouteFrameTotals totals; UiRouteSum stageSum; double machinery = -1, combined = -1, stageMs = -1;
        totals.add(UiRouteStage::kHdrSeed, 30, .1, true, true, machinery, combined);
        uiRouteAdd(stageSum, 30, .1, true, &stageMs);  // a ready prefix was already consumed
        for (uint64_t seq : {30ull, 31ull}) {
            check(coverage.missing(UiRouteStage::kHdrSeed, 0, seq), "successive failed frames update coverage");
            totals.lost(UiRouteStage::kHdrSeed, seq); uiRouteLost(stageSum, seq);
            for (auto& q : queued) coverage.apply(q.stage, q.eye, q.seq, q.flags);
        }
        check(!queued[0].flags.stage && !queued[0].flags.machinery && !queued[0].flags.combined &&
                  !queued[2].flags.stage && !queued[2].flags.machinery && !queued[2].flags.combined &&
                  queued[1].flags.stage && queued[3].flags.stage && queued[4].flags.combined,
              "queued failed frames stay spoiled without invalidating other stages or the other eye");
        const auto future = coverage.begin(UiRouteStage::kComposite, 0, 31, true);
        check(future.stage && !future.machinery && !future.combined,
              "later successful commands of the failed frame cannot restore completeness");
        for (const auto& q : queued) {
            if (q.eye) continue;
            check(!totals.add(q.stage, q.seq, .2, true, q.flags.combined,
                              machinery, combined, q.flags.machinery), "neither queued partial total closes as valid");
            if (q.stage == UiRouteStage::kHdrSeed)
                check(!uiRouteAdd(stageSum, q.seq, .2, q.flags.stage, &stageMs), "partial stage cannot close as valid");
        }
        check(!totals.close(32, machinery, combined) && !uiRouteClose(stageSum, 32, &stageMs),
              "two failed frames plus a ready prefix produce no misleading partial price");
        UiRouteCoverage movedOnly;
        auto beforeToggle = movedOnly.begin(UiRouteStage::kComposite, 0, 40, true);
        check(movedOnly.movedMissing(0, 40), "disabled draw spoils its armed pending epoch");
        movedOnly.apply(UiRouteStage::kComposite, 0, 40, beforeToggle);
        movedOnly.movedMissing(0, 41); movedOnly.apply(UiRouteStage::kComposite, 0, 40, beforeToggle);
        check(beforeToggle.stage && beforeToggle.machinery && !beforeToggle.combined,
              "later disabled frame never resurrects an earlier incomplete combined sample");
        UiRouteCoverage sameFrame;
        auto primary = sameFrame.begin(UiRouteStage::kHdrMovedDraw, 0, 50, true);
        auto machineryPart = sameFrame.begin(UiRouteStage::kComposite, 0, 50, true);
        UiRouteSum movedStage; UiRouteFrameTotals mixed;
        uiRouteAdd(movedStage, 50, .3, true, &stageMs);
        mixed.add(UiRouteStage::kHdrMovedDraw, 50, .3, true, true, machinery, combined);
        sameFrame.missing(UiRouteStage::kHdrMovedDraw, 0, 50);
        uiRouteLost(movedStage, 50); mixed.lost(UiRouteStage::kHdrMovedDraw, 50);
        sameFrame.apply(UiRouteStage::kHdrMovedDraw, 0, 50, primary);
        sameFrame.apply(UiRouteStage::kComposite, 0, 50, machineryPart);
        sameFrame.missing(UiRouteStage::kHdrMovedDraw, 0, 51);
        sameFrame.apply(UiRouteStage::kHdrMovedDraw, 0, 50, primary);
        check(!primary.stage && primary.machinery && !primary.combined && machineryPart.stage &&
                  machineryPart.machinery && !machineryPart.combined,
              "same-sequence disabled HDR draw spoils moved coverage without spoiling machinery");
        mixed.add(UiRouteStage::kComposite, 50, .2, true, machineryPart.combined,
                  machinery, combined, machineryPart.machinery);
        check(!uiRouteClose(movedStage, 51, &stageMs) && mixed.close(51, machinery, combined) == 1 &&
                  machinery == .2,
              "a consumed moved prefix plus disabled draw yields no partial moved or combined price");
    }
    {
        UiRouteFrameTotals totals[2]; double machinery = -1, combined = -1;
        check(!totals[0].add(UiRouteStage::kHdrSeed, 10, .4, true, true, machinery, combined) &&
                  !totals[1].add(UiRouteStage::kComposite, 10, .2, true, true, machinery, combined) &&
                  !totals[0].add(UiRouteStage::kHdrMovedDraw, 10, 1.3, true, true, machinery, combined) &&
                  !totals[0].add(UiRouteStage::kComposite, 10, .1, true, true, machinery, combined),
              "production totals accept interleaved eye and draw samples");
        check(totals[0].close(11, machinery, combined) == 3 && std::abs(machinery - .5) < 1e-9 &&
                  std::abs(combined - 1.8) < 1e-9,
              "machinery excludes moved shading while combined includes it exactly once");
        check(totals[1].close(11, machinery, combined) == 3 && machinery == .2 && combined == .2,
              "other eye remains independent");
        totals[0].add(UiRouteStage::kHdrSeed, 12, .4, true, false, machinery, combined);
        totals[0].lost(UiRouteStage::kHdrMovedDraw, 12);
        check(totals[0].close(13, machinery, combined) == 1 && machinery == .4,
              "diagnostics-off or unavailable moved draw cannot create a partial combined figure");
        totals[0].add(UiRouteStage::kHdrSeed, 14, .4, true, true, machinery, combined);
        totals[0].add(UiRouteStage::kHdrMovedDraw, 14, 0, false, true, machinery, combined);
        check(totals[0].close(15, machinery, combined) == 1 && machinery == .4,
              "invalid moved interval spoils combined only");
        totals[0].add(UiRouteStage::kHdrSeed, 16, .4, true, true, machinery, combined);
        totals[0].lost(UiRouteStage::kHdrSeed, 16);
        check(totals[0].close(17, machinery, combined) == 0,
              "missing machinery interval spoils both totals");
    }
}
struct SeedProbeClock {
    uint64_t ms = 1; int64_t tick = 100; bool available = true;
    unsigned millisCalls = 0, counterCalls = 0;
    static bool counter(void* p, int64_t& out) { auto& c = *static_cast<SeedProbeClock*>(p); ++c.counterCalls; out = c.tick++; return c.available; }
    static uint64_t millis(void* p) { auto& c = *static_cast<SeedProbeClock*>(p); ++c.millisCalls; return c.ms; }
    UiHdrSeedGpuProbe::Clock clock() { return {this, counter, millis, 1}; }
};
static_assert(std::is_trivially_destructible<UiHdrSeedGpuProbe>::value,
              "seed probe destruction never issues context, COM or shutdown work");
struct SeedProbeFrame {
    Fixture& f; GpuTimingFrameDriver driver;
    explicit SeedProbeFrame(Fixture& fixture) : f(fixture) {
        check(driver.bind(f.d.dev.Get(), f.ctx()) && driver.create(0), "seed probe frame driver created"); open();
    }
    void open() { check(driver.begin(0) && driver.timestamp(0, 0), "seed probe borrows application frame"); }
    void close() { check(driver.timestamp(0, 1) && driver.end(0), "seed probe outer frame closes"); }
    void consume() { GpuSpanRawSample raw{}; check(driver.poll(0, raw) == GpuSpanPoll::Ready, "seed probe frame remains valid"); }
    void finish() { driver.destroy(0); driver.reset(f.ctx()); }
};
UiHdrSeedTimingMeta seedMeta(uint64_t seq = 32, int eye = 0) {
    UiHdrSeedTimingMeta m; m.seq = seq; m.eye = eye; m.reason = 2; m.needsDepth = true;
    m.passes = 1; m.sourceW = 2037; m.sourceH = 1969; m.width = 4074; m.height = 3938;
    return m;
}
UiHdrSeedGpuProbe::Token seedPair(Fixture& f, UiHdrSeedGpuProbe& probe, const UiHdrSeedTimingMeta& meta) {
    const auto token = probe.beginCopy(f.d.dev.Get(), f.ctx(), true, meta);
    check(token && probe.endCopy(f.ctx(), token) && probe.beginWork(f.ctx(), token) &&
              probe.endWork(f.ctx(), token), "complete production seed-subprice pair queued");
    return token;
}
// A primitive-only thunk catches the injected driver SEH without promising
// /EHsc unwinding through beginCopy. This is the live outer-guard failure gap.
DWORD seedCopyFaultThunk(UiHdrSeedGpuProbe* probe, ID3D11Device* dev, ID3D11DeviceContext* ctx,
                         const UiHdrSeedTimingMeta* meta, UiHdrSeedGpuProbe::Token* result) {
    __try { *result = probe->beginCopy(dev, ctx, true, *meta); return 0; }
    __except(GetExceptionCode() == 0xE042ED50u ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return GetExceptionCode();
    }
}
void hdrSeedProbeCases(Device& d) {
    {
        Fixture f(d); SeedProbeClock clock;
        auto ownedProbe = std::make_unique<UiHdrSeedGpuProbe>(clock.clock()); auto& probe = *ownedProbe;
        const auto commands = f.ops.commands, allocated = f.ops.allocated;
        for (uint64_t seq = 1; seq <= 64; ++seq)
            check(!probe.beginCopy(d.dev.Get(), f.ctx(), false, seedMeta(seq, int(seq & 1))), "disabled seed observer emits no token");
        for (uint64_t seq = 1; seq < 32; ++seq)
            check(!probe.beginCopy(d.dev.Get(), f.ctx(), true, seedMeta(seq)), "all non-selected frames skip GPU work");
        probe.endCopy(f.ctx(), 0); probe.beginWork(f.ctx(), 0); probe.endWork(f.ctx(), 0); probe.poll(f.ctx());
        for (unsigned i = 0; i < 128; ++i) { probe.poll(f.ctx()); check(!probe.pending(), "idle pending check is constant time"); }
        check(f.ops.commands == commands && f.ops.allocated == allocated && probe.health().disabled == 64 &&
                  probe.health().notSelected == 31 && !probe.pending() && !clock.counterCalls && !clock.millisCalls,
              "off/stride/idle gates allocate no GPU queries, markers, clock reads or ring polls");
        check(!probe.beginCopy(d.dev.Get(), f.ctx(), true, seedMeta()) && probe.health().copyBeginFailed == 1 &&
                  f.ops.commands == commands, "no application scope declines instead of opening a private disjoint");
        std::vector<std::string> lines; probe.report(false, [&](const char* line) { lines.emplace_back(line); });
        check(lines.size() == 3 && lines[0].find("subset_of_HDRseed=1") != std::string::npos &&
                  lines[0].find("not_additive=1") != std::string::npos &&
                  lines[2].find("copy_gpu=- (n=0") != std::string::npos,
              "no-run output explicitly distinguishes absent samples from zero cost");
        probe.reset(f.ctx()); f.finish();
    }
    {
        Fixture f(d); SeedProbeFrame frame(f); SeedProbeClock clock;
        auto ownedProbe = std::make_unique<UiHdrSeedGpuProbe>(clock.clock()); auto& probe = *ownedProbe;
        const auto commands = f.ops.commands;
        const auto token = seedPair(f, probe, seedMeta());
        check(f.ops.commands == commands + 4 && f.ops.active == 1 && probe.pending() == 1,
              "one sampled seed adds four timestamps and no frequency scope");
        check(!probe.endCopy(f.ctx(), token) && !probe.beginWork(f.ctx(), token) && !probe.endWork(f.ctx(), token),
              "duplicate or out-of-order scope calls cannot reissue markers");
        std::thread foreign([&] { probe.endWork(f.ctx(), token); probe.poll(f.ctx()); }); foreign.join();
        check(f.ops.commands == commands + 4, "foreign owner cannot touch seed probe queries");
        frame.close(); const unsigned firstCopy = 9; // frame driver allocated eight stamps and one disjoint query
        f.ops.pendingQuery = int(firstCopy + 2); probe.poll(f.ctx());
        check(!probe.validSamples() && probe.pending() == 1, "ready copy plus pending execute never publishes a partial subprice");
        const auto reads = f.ops.queries[firstCopy].reads; probe.poll(f.ctx());
        check(f.ops.queries[firstCopy].reads == reads, "ready copy is cached while its paired execution is pending");
        std::vector<std::string> lines; probe.report(false, [&](const char* line) { lines.emplace_back(line); });
        check(probe.pending() == 1 && lines[0].find("pending=1") != std::string::npos,
              "report/toggle retains an armed pending pair");
        f.ops.pendingQuery = -1; probe.poll(f.ctx());
        check(probe.validSamples() == 1 && probe.health().lateWindow == 1 && probe.health().paired == 1,
              "prior-window armed pair completes after diagnostics turn off");
        lines.clear(); probe.report(false, [&](const char* line) { lines.emplace_back(line); });
        check(lines.size() == 4 && lines[2].find("copy_gpu=1.0000/1.0000/1.0000") != std::string::npos &&
                  lines[2].find("execute_gpu=1.0000/1.0000/1.0000") != std::string::npos &&
                  lines[2].find("record_wall_cpu=1.0000/1.0000/1.0000") != std::string::npos &&
                  lines[3].find("reason=0x2 need_depth=1 stencil_mask=0x00 passes=1") != std::string::npos,
              "paired prices and recorded depth/mask/reason/pass classification reach the log");
        frame.consume(); frame.finish(); probe.reset(f.ctx()); f.finish();
    }
    for (int failure = 0; failure < 6; ++failure) {
        Fixture f(d); SeedProbeFrame frame(f);
        auto ownedProbe = std::make_unique<UiHdrSeedGpuProbe>(); auto& probe = *ownedProbe;
        if (failure == 0) f.ops.failCreate = int(f.ops.creates);
        if (failure == 4) f.ops.failStamp = true;
        auto token = probe.beginCopy(d.dev.Get(), f.ctx(), true, seedMeta());
        if (failure == 0 || failure == 4) check(!token && probe.health().copyBeginFailed == 1, "copy allocation/first-marker failure drops pair");
        else {
            check(token != 0, "failure fixture copy starts");
            if (failure == 1) f.ops.failStamp = true;
            const bool copied = probe.endCopy(f.ctx(), token);
            if (failure == 1) check(!copied && probe.health().copyEndFailed == 1, "copy end-marker failure drops pair");
            else {
                check(copied, "failure fixture copy closes");
                if (failure == 2) f.ops.failCreate = int(f.ops.creates);
                if (failure == 5) f.ops.failStamp = true;
                const bool worked = probe.beginWork(f.ctx(), token);
                if (failure == 2 || failure == 5) check(!worked && probe.health().workBeginFailed == 1, "execution allocation/first-marker failure drops completed copy");
                else { check(worked, "failure fixture execute begins"); f.ops.failStamp = true;
                    check(!probe.endWork(f.ctx(), token) && probe.health().workEndFailed == 1, "execution end-marker failure drops both prices"); }
            }
        }
        f.ops.failStamp = false; f.ops.failCreate = -1;
        check(!probe.pending() && !probe.validSamples(), "no begin/end failure publishes a partial pair");
        seedPair(f, probe, seedMeta(64)); frame.close(); probe.poll(f.ctx());
        check(probe.validSamples() == 1, "query failure retries safely in a later selected frame");
        frame.consume(); frame.finish(); probe.reset(f.ctx()); f.finish();
    }
    {
        Fixture f(d); SeedProbeFrame frame(f);
        auto ownedProbe = std::make_unique<UiHdrSeedGpuProbe>(); auto& probe = *ownedProbe;
        const auto meta = seedMeta(); UiHdrSeedGpuProbe::Token token = 0;
        const unsigned frameOnly = f.ops.allocated - f.ops.released;
        f.ops.faultNextStamp = true;
        check(seedCopyFaultThunk(&probe, d.dev.Get(), f.ctx(), &meta, &token) == 0xE042ED50u,
              "driver first-marker SEH is caught by the outer primitive thunk");
        check(!token && !probe.pending() && !probe.validSamples() && !probe.health().copyStarted &&
                  probe.health().selected == 1 && f.ops.allocated - f.ops.released > frameOnly,
              "partial begin owns timer resources before any helper slot or token is published");
        check(probe.reset(f.ctx()) && f.ops.allocated - f.ops.released == frameOnly && f.ops.active == 1,
              "outer reset reclaims even Phase::Empty timer resources and leaves frame scope intact");
        frame.close(); frame.consume(); frame.open(); seedPair(f, probe, seedMeta(64));
        frame.close(); probe.poll(f.ctx());
        check(probe.validSamples() == 1 && probe.health().paired == 1 && probe.health().copyStarted == 1 &&
                  probe.health().workStarted == 1 && !probe.pending(),
              "next selected frame retries after partial-begin fault without reporting phantom success");
        frame.consume(); frame.finish(); probe.reset(f.ctx()); f.finish();
    }
    for (int failure = 0; failure < 3; ++failure) {
        Fixture f(d); SeedProbeFrame frame(f);
        auto ownedProbe = std::make_unique<UiHdrSeedGpuProbe>(); auto& probe = *ownedProbe;
        const int firstQuery = int(f.ops.queries.size()); seedPair(f, probe, seedMeta()); frame.close();
        if (failure == 2) f.ops.disjoint = true;
        else f.ops.failedQuery = firstQuery + (failure == 0 ? 0 : 2);
        probe.poll(f.ctx());
        check(!probe.pending() && !probe.validSamples() &&
                  (failure == 1 ? probe.health().invalidWork : probe.health().invalidCopy) == 1,
              "copy/execute GetData failure or shared disjoint invalidity never accepts half a pair");
        f.ops.failedQuery = -1; f.ops.disjoint = false;
        frame.finish(); probe.reset(f.ctx()); f.finish();
    }
    {
        Fixture f(d); SeedProbeFrame frame(f);
        auto ownedProbe = std::make_unique<UiHdrSeedGpuProbe>(); auto& probe = *ownedProbe;
        std::array<UiHdrSeedGpuProbe::Token, UiHdrSeedGpuProbe::kSlots> tokens{};
        for (unsigned i = 0; i < tokens.size(); ++i) tokens[i] = seedPair(f, probe, seedMeta(32, int(i & 1)));
        const auto commands = f.ops.commands;
        check(!probe.beginCopy(d.dev.Get(), f.ctx(), true, seedMeta()) && probe.health().busy == 1 &&
                  f.ops.commands == commands, "busy fixed pool drops an entire pair without marker commands");
        check(probe.reset(f.ctx()) && probe.health().resetDropped == 32 && !probe.pending(),
              "owned reset cancels all bounded paired leases");
        const auto replacement = probe.beginCopy(d.dev.Get(), f.ctx(), true, seedMeta(64));
        check(replacement && replacement != tokens[0] && !probe.endCopy(f.ctx(), tokens[0]),
              "generation token rejects stale callbacks after pool reuse");
        check(probe.endCopy(f.ctx(), replacement) && probe.beginWork(f.ctx(), replacement) &&
                  probe.endWork(f.ctx(), replacement), "pool can resume after reset");
        frame.close(); probe.poll(f.ctx()); check(probe.validSamples() == 1, "only replacement pair survives reset");
        frame.consume(); frame.finish(); probe.reset(f.ctx()); f.finish();
    }
    {
        Fixture f(d); SeedProbeFrame frame(f);
        auto ownedProbe = std::make_unique<UiHdrSeedGpuProbe>(); auto& probe = *ownedProbe;
        // Leave exactly one shared lease. Copy can start and finish, but its
        // paired execute cannot acquire a lease until earlier samples drain.
        for (unsigned i = 0; i < DisjointClock::kLeases - 2; ++i)
            check(f.begin(i), "fill application frame shared lease budget");
        const auto token = probe.beginCopy(d.dev.Get(), f.ctx(), true, seedMeta());
        check(token && probe.endCopy(f.ctx(), token), "last available lease measures copy");
        const auto commands = f.ops.commands;
        check(!probe.beginWork(f.ctx(), token) && probe.health().workBeginFailed == 1 &&
                  !probe.pending() && !probe.validSamples() && f.ops.commands == commands,
              "execute lease exhaustion drops whole pair without another marker");
        for (unsigned i = 0; i < DisjointClock::kLeases - 2; ++i)
            check(f.timers[i].end(f.ctx()), "pressure fixture borrowers end");
        frame.close(); double ms = 0;
        for (unsigned i = 0; i < DisjointClock::kLeases - 2; ++i)
            check(f.timers[i].poll(f.ctx(), ms) == GpuTimerPoll::Ready, "pressure fixture leases drain");
        frame.consume(); frame.open(); seedPair(f, probe, seedMeta(64)); frame.close(); probe.poll(f.ctx());
        check(probe.validSamples() == 1, "sample retry succeeds after shared lease pressure drains");
        frame.consume(); frame.finish(); probe.reset(f.ctx()); f.finish();
    }
    for (unsigned phase = 0; phase < 4; ++phase) {
        Fixture f(d); SeedProbeFrame frame(f); SeedProbeClock clock;
        auto ownedProbe = std::make_unique<UiHdrSeedGpuProbe>(clock.clock()); auto& probe = *ownedProbe;
        const auto token = probe.beginCopy(d.dev.Get(), f.ctx(), true, seedMeta()); check(token != 0, "expiry sample starts");
        if (phase >= 1) check(probe.endCopy(f.ctx(), token), "expiry sample copy ends");
        if (phase >= 2) check(probe.beginWork(f.ctx(), token), "expiry sample work starts");
        if (phase >= 3) check(probe.endWork(f.ctx(), token), "expiry sample work ends");
        clock.ms += UiHdrSeedGpuProbe::kExpireMs; probe.poll(f.ctx());
        check(!probe.pending() && !probe.validSamples() && probe.health().expired == 1,
              "unfinished/open/pending scopes expire explicitly without waiting");
        frame.close(); frame.consume(); frame.finish(); probe.reset(f.ctx()); f.finish();
    }
    {
        Fixture f(d); SeedProbeFrame frame(f); SeedProbeClock clock;
        auto ownedProbe = std::make_unique<UiHdrSeedGpuProbe>(clock.clock()); auto& probe = *ownedProbe;
        auto token = probe.beginCopy(d.dev.Get(), f.ctx(), true, seedMeta());
        check(token && probe.endCopy(f.ctx(), token), "recording decline fixture copy ends");
        probe.cancel(f.ctx(), token, UiHdrSeedGpuProbe::Cancel::RecordingFailed);
        token = probe.beginCopy(d.dev.Get(), f.ctx(), true, seedMeta());
        check(token && probe.endCopy(f.ctx(), token) && probe.beginWork(f.ctx(), token), "execution decline fixture opens");
        check(!probe.endWork(f.ctx(), token, false), "execution decline cancels open measurement");
        token = probe.beginCopy(d.dev.Get(), f.ctx(), true, seedMeta()); probe.cancel(f.ctx(), token);
        check(probe.health().recordingFailed == 1 && probe.health().executionFailed == 1 &&
                  probe.health().cancelled == 1 && !probe.pending() && !probe.validSamples(),
              "record/Finish failure, execution failure and explicit cancellation are distinct");
        clock.available = false; seedPair(f, probe, seedMeta(64)); frame.close(); probe.poll(f.ctx());
        check(probe.validSamples() == 1 && probe.health().recordUnavailable == 1,
              "CPU-clock unavailability remains explicit while both GPU intervals are valid");
        frame.consume(); frame.finish(); probe.reset(f.ctx()); f.finish();
    }
    {
        Fixture f(d); SeedProbeFrame frame(f);
        auto ownedProbe = std::make_unique<UiHdrSeedGpuProbe>(); auto& probe = *ownedProbe;
        for (unsigned i = 0; i < 17; ++i) { auto m = seedMeta(); m.reason = i; seedPair(f, probe, m); }
        frame.close(); probe.poll(f.ctx());
        check(probe.validSamples() == 17 && probe.health().classOverflow == 1,
              "classification capacity is explicit and never drops global paired pricing");
        std::vector<std::string> lines; probe.report(true, [&](const char* line) { lines.emplace_back(line); });
        check(lines.size() == 19, "bounded classification emits at most sixteen detail lines");
        frame.consume(); frame.finish(); probe.reset(f.ctx()); f.finish();
    }
    {
        Fixture f(d); SeedProbeFrame frame(f);
        auto ownedProbe = std::make_unique<UiHdrSeedGpuProbe>(); auto& probe = *ownedProbe;
        unsigned warmAllocations = 0;
        for (unsigned round = 1; round <= 20; ++round) {
            if (round > 1) frame.open();
            for (unsigned i = 0; i < 32; ++i) seedPair(f, probe, seedMeta(uint64_t(round) * 32, int(i & 1)));
            frame.close(); probe.poll(f.ctx()); frame.consume();
            check(!probe.pending() && probe.health().paired == uint64_t(round) * 32,
                  "completed pairs release shared leases for the next selected frame");
            if (round == 16) warmAllocations = f.ops.allocated;
        }
        check(f.ops.allocated == warmAllocations, "fixed timer objects reuse query allocations after warmup");
        std::vector<std::string> lines; probe.report(true, [&](const char* line) { lines.emplace_back(line); });
        check(lines[2].find("n=640 retained=512") != std::string::npos,
              "bounded reservoir retains unbiased whole-window pricing beyond capacity");
        frame.finish(); probe.reset(f.ctx()); f.finish();
    }
    {
        Fixture f(d); f.ops.scripted = false; SeedProbeFrame frame(f);
        auto ownedProbe = std::make_unique<UiHdrSeedGpuProbe>(); auto& probe = *ownedProbe;
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        ComPtr<ID3D11Texture2D> a, b; hr(d.dev->CreateTexture2D(&desc, nullptr, &a)); hr(d.dev->CreateTexture2D(&desc, nullptr, &b));
        const auto token = probe.beginCopy(d.dev.Get(), f.ctx(), true, seedMeta()); check(token != 0, "real WARP sampled copy starts");
        f.ctx()->CopyResource(b.Get(), a.Get()); check(probe.endCopy(f.ctx(), token), "real WARP copy interval ends");
        check(probe.beginWork(f.ctx(), token), "real WARP execution interval starts after recording");
        f.ctx()->CopyResource(a.Get(), b.Get()); check(probe.endWork(f.ctx(), token), "real WARP execution interval ends");
        frame.close(); f.ctx()->Flush(); // fixture submission only; live probe never Flushes or waits
        const uint64_t deadline = GetTickCount64() + 5000;
        do { probe.poll(f.ctx()); if (probe.pending()) Sleep(1); } while (probe.pending() && GetTickCount64() < deadline);
        check(probe.validSamples() == 1 && !probe.pending(), "real WARP accepts both subprices with DONOTFLUSH reads");
        frame.consume(); frame.finish(); probe.reset(f.ctx()); f.finish();
    }
}
void run(){Runtime runtime;Device device(runtime);policyCases(device,runtime);frameDriverCases(device);controllerCases(device,runtime);nativeWork(device);nativeFrameWork(device);hdrDrawRouteCases(device);hdrSeedProbeCases(device);std::printf("PASS: %u shared GPU timer lifecycle checks\n",checks);}
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
