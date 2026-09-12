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
#include "../../src/d3d11/gpu_span_d3d11.h"
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
        check(create!=nullptr,"typed D3D11CreateDevice");
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
        return s.failEnd&&x->kind==D3D11_QUERY_TIMESTAMP_DISJOINT?E_FAIL:S_OK;
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
        if(!x||x->open)s.bad=true;
        if(x)x->ptr=nullptr;
        ++s.released;q->Release();
    }
    GpuSpanD3D11Ops callbacks(){return{this,create,begin,end,data,release};}
    void clean(){check(!bad&&active==0&&allocated==released,"native query ownership/commands balanced");}
};
void adapterCases(Device& d,Runtime& runtime){
    ComPtr<ID3D11DeviceContext> deferred;hr(d.dev->CreateDeferredContext(0,&deferred));
    {GpuSpanD3D11Driver x(d.dev.Get(),deferred.Get());check(!x.isBound()&&!x.create(0),"fresh deferred context rejected");}
    {Device other(runtime);GpuSpanD3D11Driver x(other.dev.Get(),d.ctx.Get());check(!x.isBound()&&!x.create(0),"mismatched devices rejected");}
    {GpuSpanD3D11Ops partial;partial.createQuery=Ops::create;GpuSpanD3D11Driver x(d.dev.Get(),d.ctx.Get(),partial);
        check(!x.isBound(),"incomplete operation table rejected");}
    for(int fail=0;fail<7;++fail)for(int mode=0;mode<3;++mode){
        Ops o;o.failCreate=fail;o.nonnullFailure=mode==1;o.nullSuccess=mode==2;
        {GpuSpanD3D11Driver x(d.dev.Get(),d.ctx.Get(),o.callbacks());
            check(x.isBound()&&!x.create(0),"every partial allocation fails safely");
            check(o.allocated==o.released,"all partially allocated queries released");
            o.failCreate=-1;check(x.create(0),"same slot can retry after creation failure");x.shutdown();}
        o.clean();
    }
    {
        Ops o;GpuSpanD3D11Driver x(d.dev.Get(),d.ctx.Get(),o.callbacks());
        check(x.create(0)&&x.create(1)&&x.begin(0),"allocated idle slots begin");
        check(!x.begin(1)&&!x.timestamp(1,0)&&!x.timestamp(0,6),"cross-slot and invalid markers rejected");
        check(x.timestamp(0,0)&&!x.timestamp(0,0)&&x.timestamp(0,1)&&x.end(0),"partial cycle closes; duplicate marker refused");
        check(!x.begin(0),"pending query cannot be reused");
        const auto commands=o.commands;bool rejected=false;
        std::thread thread([&]{GpuSpanRawSample raw{};rejected=!x.create(2)&&!x.begin(1)&&!x.timestamp(0,2)&&
            !x.end(0)&&x.poll(0,raw)==GpuSpanPoll::Failed&&!x.shutdown();x.destroy(1);});thread.join();
        check(rejected&&o.commands==commands,"actual wrong-thread calls issue no D3D operations");
        GpuSpanRawSample raw{};raw.frequency=999;
        o.pendingQuery=0;check(x.poll(0,raw)==GpuSpanPoll::Pending&&!raw.frequency,"disjoint S_FALSE retains pending and clears output");
        o.pendingQuery=2;check(x.poll(0,raw)==GpuSpanPoll::Pending,"partial timestamp readiness");
        const auto disjointReads=o.queries[0].reads,firstReads=o.queries[1].reads;
        o.pendingQuery=-1;
        check(x.poll(0,raw)==GpuSpanPoll::Ready&&raw.timestampsReady&&raw.frequency==1000,"complete issued subset becomes ready");
        check(o.queries[0].reads==disjointReads&&o.queries[1].reads==firstReads,"cached results are not polled twice");
        check(raw.ticks[0]==1&&raw.ticks[1]==2&&raw.ticks[2]==0,"cached tick values survive partial readiness");
        for(unsigned i=3;i<7;++i)check(o.queries[i].reads==0,"unissued timestamps never polled");
        check(x.begin(0)&&x.timestamp(0,5)&&x.end(0),"ready slot reused with fresh issued mask");
        check(x.poll(0,raw)==GpuSpanPoll::Ready&&raw.ticks[0]==0&&raw.ticks[5]>0,"reused slot discards old ticks");
        check(x.begin(1)&&x.shutdown(),"shutdown closes active scope");check(!x.create(2)&&!x.shutdown(),"shutdown terminal");o.clean();
    }
    for(int query=0;query<7;++query)for(HRESULT status:{E_FAIL,static_cast<HRESULT>(2)}){
        Ops o;o.failedQuery=query;o.failedStatus=status;
        GpuSpanD3D11Driver x(d.dev.Get(),d.ctx.Get(),o.callbacks());check(x.create(0)&&x.begin(0),"failure fixture begin");
        for(unsigned i=0;i<6;++i)check(x.timestamp(0,i),"failure fixture issued all markers");
        check(x.end(0),"failure fixture closed");GpuSpanRawSample raw{};
        check(x.poll(0,raw)==GpuSpanPoll::Failed&&!raw.timestampsReady&&!raw.frequency,"HRESULT failure or unknown success never ready");
        check(!x.begin(0),"failed slot cannot reuse until destroyed");x.destroy(0);x.shutdown();o.clean();
    }
    {
        Ops o;o.failBegin=true;GpuSpanD3D11Driver x(d.dev.Get(),d.ctx.Get(),o.callbacks());
        check(x.create(0)&&!x.begin(0)&&o.active==0,"failed begin opens nothing");x.destroy(0);
        o.failBegin=false;check(x.create(0)&&x.begin(0),"recovery after failed begin");o.failEnd=true;
        check(!x.end(0)&&!x.begin(0),"uncertain end stops future begins");const auto commands=o.commands;
        check(x.shutdown()&&commands==o.commands,"cleanup never retries uncertain end");o.clean();
    }
    {
        Ops o;GpuSpanD3D11Driver x(d.dev.Get(),d.ctx.Get(),o.callbacks());GpuSpanState policy(x,x.currentOwner());
        const auto owner=x.currentOwner();check(policy.beginFrame(1,42,0,owner)==GpuSpanReason::Valid,"partial policy frame starts");
        check(policy.beginEye(0,0,owner)==GpuSpanReason::Valid&&policy.endEye(0,0,owner)==GpuSpanReason::Valid,"one eye issued");
        check(policy.invalidateFrame(0,owner)==GpuSpanReason::Incomplete,"missing other eye invalidates");
        GpuSpanState::Results results{};check(policy.poll(1,owner,results)==1&&results[0].reason==GpuSpanReason::Incomplete,"partial-issued frame retires without waiting on unissued markers");
        policy.shutdown(2,owner);x.shutdown();o.clean();
    }
    std::printf("PASS: %u typed D3D11 adapter failure/ownership checks\n",checks);
}
struct Workload {
    ComPtr<ID3D11Texture2D> texture,copy;
    ComPtr<ID3D11RenderTargetView> rtv;
    explicit Workload(Device& d){D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=128;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET;hr(d.dev->CreateTexture2D(&desc,nullptr,&texture));
        hr(d.dev->CreateRenderTargetView(texture.Get(),nullptr,&rtv));hr(d.dev->CreateTexture2D(&desc,nullptr,&copy));}
    void issue(Device& d,unsigned eye){const float colour[4]={eye?0.f:1.f,eye?1.f:0.f,0,1};
        for(unsigned i=0;i<16;++i){d.ctx->ClearRenderTargetView(rtv.Get(),colour);d.ctx->CopyResource(copy.Get(),texture.Get());}}
    void verify(Device& d,unsigned eye){D3D11_TEXTURE2D_DESC desc{};copy->GetDesc(&desc);
        desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;hr(d.dev->CreateTexture2D(&desc,nullptr,&stage));d.ctx->CopyResource(stage.Get(),copy.Get());
        D3D11_MAPPED_SUBRESOURCE map{};hr(d.ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&map));bool correct=true;
        for(UINT y=0;y<desc.Height;++y){auto* row=reinterpret_cast<const float*>(static_cast<const char*>(map.pData)+y*map.RowPitch);
            for(UINT x=0;x<desc.Width;++x){auto*p=row+x*4;correct=correct&&p[0]==(eye?0.f:1.f)&&p[1]==(eye?1.f:0.f)&&p[2]==0&&p[3]==1;}}
        d.ctx->Unmap(stage.Get(),0);check(correct,"every copied pixel has the expected eye colour");}
};
void realWork(Device& d){
    GpuSpanD3D11Driver driver(d.dev.Get(),d.ctx.Get());
    GpuSpanState policy(driver,driver.currentOwner());const auto owner=driver.currentOwner();
    Workload left(d),right(d);Workload* eyes[2]={&left,&right};
    uint64_t sequence=0;
    for(unsigned order=0;order<2;++order){
        const auto now=GetTickCount64();++sequence;
        check(policy.beginFrame(sequence,100+sequence,now,owner)==GpuSpanReason::Valid,"real frame begins");
        for(unsigned pass=0;pass<2;++pass){const unsigned eye=order?1-pass:pass;
            check(policy.beginEye(eye,GetTickCount64(),owner)==GpuSpanReason::Valid,"real eye begins");
            eyes[eye]->issue(d,eye);
            check(policy.endEye(eye,GetTickCount64(),owner)==GpuSpanReason::Valid,"real eye ends");}
        check(policy.finishFrame(GetTickCount64(),owner)==GpuSpanReason::Valid,"real frame closes");
        // Test-owned submission/readback, OUTSIDE the measured intervals. The
        // production adapter never Flushes or blocks for telemetry.
        d.ctx->Flush();left.verify(d,0);right.verify(d,1);
        GpuSpanState::Results results{};unsigned count=0;const auto deadline=GetTickCount64()+1500;
        do{count=policy.poll(GetTickCount64(),owner,results);if(!count)Sleep(1);}while(!count&&GetTickCount64()<deadline);
        check(count==1&&results[0].reason==GpuSpanReason::Valid&&results[0].sourceFrame==100+sequence,"real WARP sample completes with original frame association");
        auto&r=results[0];check(r.outerMs>0&&r.leftMs>0&&r.rightMs>0&&r.outerMs+1e-9>=r.leftMs+r.rightMs,"real intervals are positive and nested within outer");
        std::printf("PASS: WARP %s order frame %llu: outer %.4f ms, left %.4f ms, right %.4f ms; exact pixels verified\n",
                    order?"right-left":"left-right",r.sourceFrame,r.outerMs,r.leftMs,r.rightMs);
    }
    policy.shutdown(GetTickCount64(),owner);driver.shutdown();
}
void sharedWork(Device& d){
    Ops ops;ops.scripted=false;
    DisjointD3D11Backend backend(d.dev.Get(),d.ctx.Get(),ops.callbacks());
    DisjointClock clock(backend);
    Workload work(d);
    std::array<ComPtr<ID3D11Query>,4> ticks;
    const D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP,0};
    for(auto& tick:ticks)hr(d.dev->CreateQuery(&desc,&tick));
    auto frame=clock.startFrame(GetTickCount64());
    auto outer=clock.acquireInterval(GetTickCount64());
    d.ctx->End(ticks[0].Get());
    auto inner=clock.acquireInterval(GetTickCount64());
    d.ctx->End(ticks[1].Get());work.issue(d,0);d.ctx->End(ticks[2].Get());
    check(clock.endInterval(inner,GetTickCount64()),"real nested borrower ends");
    d.ctx->End(ticks[3].Get());check(clock.endInterval(outer,GetTickCount64()),"real outer borrower ends");
    check(clock.finishFrame(frame,GetTickCount64()),"real shared parent closes");
    d.ctx->Flush();work.verify(d,0);
    DisjointResult result{};const auto deadline=GetTickCount64()+1500;
    do{result=clock.poll(inner,GetTickCount64());if(result.status==DisjointStatus::Pending)Sleep(1);}
    while(result.status==DisjointStatus::Pending&&GetTickCount64()<deadline);
    check(result.status==DisjointStatus::Ready&&result.frequency>0,"real shared frequency ready");
    check(clock.poll(outer,GetTickCount64()).frequency==result.frequency,"real borrowers share cached frequency");
    UINT64 t[4]{};
    for(unsigned i=0;i<4;++i){HRESULT status=S_FALSE;
        do{status=d.ctx->GetData(ticks[i].Get(),&t[i],sizeof(t[i]),D3D11_ASYNC_GETDATA_DONOTFLUSH);if(status==S_FALSE)Sleep(1);}
        while(status==S_FALSE&&GetTickCount64()<deadline);hr(status);}
    check(t[0]<=t[1]&&t[1]<t[2]&&t[2]<=t[3],"real nested timestamp ordering");
    check(ops.allocated==1&&ops.active==0,"nested native intervals used exactly one disjoint query");
    clock.release(inner,GetTickCount64());clock.release(outer,GetTickCount64());clock.release(frame,GetTickCount64());
    auto reuse=clock.acquireInterval(GetTickCount64());check(bool(reuse)&&ops.allocated==1,"shared native query reused");
    clock.endInterval(reuse,GetTickCount64());clock.release(reuse,GetTickCount64());
    clock.shutdown(GetTickCount64());ops.clean();
    std::printf("PASS: nested WARP intervals share one disjoint query, %.4f ms workload; exact pixels verified\n",
                double(t[2]-t[1])*1000.0/double(result.frequency));
}
void run(){Runtime runtime;Device device(runtime);adapterCases(device,runtime);realWork(device);sharedWork(device);}
}
int wmain(int argc,wchar_t**argv){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc==2&&!wcscmp(argv[1],L"--dry-run")){std::puts("dry-run: no module, device, queries or files");return 0;}
    if(argc==2&&!wcscmp(argv[1],L"--child")){
        try{run();return 0;}catch(const std::exception&e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
    if(argc!=2||wcscmp(argv[1],L"--self-test")){std::fputs("usage: --self-test | --dry-run\n",stderr);return 2;}
    wchar_t executable[32768]{};const DWORD n=GetModuleFileNameW(nullptr,executable,32768);
    if(!n||n>=32768)return 2;
    std::wstring command=L"\""+std::wstring(executable)+L"\" --child";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    if(!CreateProcessW(executable,&command[0],nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return 2;
    const DWORD waited=WaitForSingleObject(process.hProcess,30000);DWORD code=1;
    if(waited==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&code);
    else{TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,1000);std::fputs("FAIL: owned test child timed out\n",stderr);}
    CloseHandle(process.hThread);CloseHandle(process.hProcess);
    if(code!=0)std::fprintf(stderr,"FAIL: owned D3D11 child exited 0x%08lX\n",code);
    return code==0?0:1;
}
