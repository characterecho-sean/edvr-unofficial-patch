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
    bool abandoning=false, failStamp=false;
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
        if(index==s.pendingQuery)return S_FALSE;
        if(index==s.failedQuery)return s.failedStatus;
        if(!s.scripted)return c->GetData(q,out,size,flags);
        if(x->kind==D3D11_QUERY_TIMESTAMP_DISJOINT){
            if(size!=sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT)){s.bad=true;return E_FAIL;}
            *static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*>(out)={1000,FALSE};
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
void run(){Runtime runtime;Device device(runtime);policyCases(device,runtime);nativeWork(device);std::printf("PASS: %u shared GPU timer lifecycle checks\n",checks);}
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
