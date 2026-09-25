#include "../../src/openxr/producer_gpu_timing.h"
#include "../../src/common/system_d3d11.h"
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
using Microsoft::WRL::ComPtr;
using edvr::openxr::ProducerGpuTiming;
namespace {
unsigned checks=0,failures=0;
void check(bool ok,const char* why){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",why);}}

// Real COM query allocation with deterministic scripted GetData results,
// the same shape as native_device_gpu_test/collector_cases.h's Ops.
struct Ops {
    struct Query { ID3D11Query* ptr; D3D11_QUERY kind; uint64_t tick=0; bool open=false; };
    std::vector<Query> queries;
    unsigned creates=0,releases=0,active=0;
    int failCreate=-1;
    bool pending=false,disjoint=false;
    uint64_t tick=0;
    Query* find(ID3D11Asynchronous* q){for(auto& x:queries)if(x.ptr==q)return &x;return nullptr;}
    static HRESULT create(void* p,ID3D11Device* d,const D3D11_QUERY_DESC* desc,ID3D11Query** out){
        auto& s=*static_cast<Ops*>(p);const auto n=s.creates++;*out=nullptr;
        if(int(n)==s.failCreate)return E_OUTOFMEMORY;
        const auto r=d->CreateQuery(desc,out);
        if(r==S_OK&&*out)s.queries.push_back({*out,desc->Query});
        return r;
    }
    static HRESULT begin(void* p,ID3D11DeviceContext* c,ID3D11Asynchronous* q){
        auto& s=*static_cast<Ops*>(p);auto* x=s.find(q);if(!x)return E_FAIL;
        ++s.active;x->open=true;c->Begin(q);return S_OK;
    }
    static HRESULT end(void* p,ID3D11DeviceContext* c,ID3D11Asynchronous* q){
        auto& s=*static_cast<Ops*>(p);auto* x=s.find(q);if(!x)return E_FAIL;
        if(x->kind==D3D11_QUERY_TIMESTAMP_DISJOINT){x->open=false;--s.active;}
        x->tick=++s.tick;c->End(q);return S_OK;
    }
    static HRESULT data(void* p,ID3D11DeviceContext*,ID3D11Asynchronous* q,void* out,UINT size,UINT flags){
        auto& s=*static_cast<Ops*>(p);auto* x=s.find(q);
        if(!x||flags!=D3D11_ASYNC_GETDATA_DONOTFLUSH)return E_FAIL;
        if(s.pending)return S_FALSE;
        if(x->kind==D3D11_QUERY_TIMESTAMP_DISJOINT){
            if(size!=sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT))return E_FAIL;
            *static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*>(out)={1000u,s.disjoint?TRUE:FALSE};
        } else {
            if(size!=sizeof(UINT64))return E_FAIL;
            *static_cast<UINT64*>(out)=x->tick;
        }
        return S_OK;
    }
    static void release(void* p,ID3D11Query* q) noexcept {
        auto& s=*static_cast<Ops*>(p);auto* x=s.find(q);if(x)x->ptr=nullptr;
        ++s.releases;q->Release();
    }
    edvr::GpuSpanD3D11Ops callbacks(){return {this,create,begin,end,data,release};}
};

ComPtr<ID3D11Texture2D> makeTexture(ID3D11Device* d) {
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=32;
    desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> t;check(SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&t)),"texture");return t;
}

} // namespace

int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(argc!=2)return 2;
    if(!std::wcscmp(argv[1],L"--dry-run")){std::puts("producer_gpu_timing_test: dry-run (no WARP device)");return 0;}
    if(std::wcscmp(argv[1],L"--self-test"))return 2;

    auto create=edvr::systemD3D11CreateDevice();check(create!=nullptr,"system D3D11");if(!create)return 1;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
    check(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context)),"WARP device");
    if(!device)return 1;

    // 1. A deferred (non-immediate) context is refused once, permanently. The
    // caller's own CopyResource -- issued regardless, exactly as
    // shared_texture_transfer.cpp does around beginCopy/endCopy -- still
    // works, and no window ever opens since poll() never leaves initialized_.
    {
        ComPtr<ID3D11DeviceContext> deferred;
        check(SUCCEEDED(device->CreateDeferredContext(0,&deferred)),"deferred context");
        auto a=makeTexture(device.Get()),b=makeTexture(device.Get());
        ProducerGpuTiming t;
        t.beginCopy(device.Get(),deferred.Get());
        deferred->CopyResource(b.Get(),a.Get());
        t.endCopy();
        t.poll(1000000);
        auto s=t.summary();
        check(s.windows==0&&s.samples==0,"refused object opens no window at all");
        t.beginCopy(device.Get(),deferred.Get()); // second attempt: still refused, no re-log, no crash
        deferred->CopyResource(b.Get(),a.Get());
        t.endCopy();
        t.shutdown(); // release-only; safe even though nothing was ever created
    }

    // 2. Query creation failing on an otherwise-valid immediate context: the
    // object itself is enabled, but this one span never opens -- the copy
    // (issued by the caller either way) is unaffected, and the window still
    // closes on schedule with nothing to report.
    {
        Ops o;o.failCreate=0;
        auto a=makeTexture(device.Get()),b=makeTexture(device.Get());
        ProducerGpuTiming t;
        t.beginCopy(device.Get(),context.Get(),o.callbacks());
        context->CopyResource(b.Get(),a.Get());
        t.endCopy();
        context->Flush();
        t.poll(1);
        t.poll(1ull+40000);
        auto s=t.summary();
        check(s.windows==1&&s.samples==0&&s.disjointInvalid==0,
          "a slot that failed to allocate records neither a sample nor a failure");
        t.shutdown();
    }

    // 3. Polling never blocks: GetData scripted to always return S_FALSE.
    // Many polls against a permanently pending query must return promptly,
    // and the span is dropped -- not counted as a sample -- when its window
    // closes still pending.
    {
        Ops o;o.pending=true;
        auto a=makeTexture(device.Get()),b=makeTexture(device.Get());
        ProducerGpuTiming t;
        t.beginCopy(device.Get(),context.Get(),o.callbacks());
        context->CopyResource(b.Get(),a.Get());
        t.endCopy();
        context->Flush();
        const auto began=std::chrono::steady_clock::now();
        uint64_t now=1;
        for(unsigned i=0;i<2000;++i){t.poll(now);now+=10;}
        const auto elapsedMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
        check(elapsedMs<5000.0,"2000 polls against a stuck query return without blocking");
        t.poll(now+40000); // window closes with the span still pending
        auto s=t.summary();
        check(s.windows==1&&s.samples==0,"a span still pending at window close is dropped, not counted");
        t.shutdown();
    }

    // 3b. Steady state creates no queries: a slot whose span resolved goes
    // back to the ring with its queries, so twenty resolved copies cost one
    // slot's three (a disjoint and two timestamps) and release nothing.
    {
        Ops o;
        auto a=makeTexture(device.Get()),b=makeTexture(device.Get());
        ProducerGpuTiming t;
        for(unsigned n=0;n<20;++n){
            t.poll(1+n);
            t.beginCopy(device.Get(),context.Get(),o.callbacks());
            context->CopyResource(b.Get(),a.Get());
            t.endCopy();
        }
        t.poll(21);
        check(o.creates==3&&o.releases==0,"resolved slots are reused: twenty copies create one slot's queries");
        t.poll(21+40000);
        check(t.summary().samples==20,"every reused slot still yields its sample");
        t.shutdown();
    }

    // 4. The bracket records around the copy: a real WARP timestamp pair
    // around real GPU work yields at least one sample once its window closes.
    {
        auto a=makeTexture(device.Get()),b=makeTexture(device.Get());
        ProducerGpuTiming t;
        t.beginCopy(device.Get(),context.Get());
        context->CopyResource(b.Get(),a.Get());
        t.endCopy();
        context->Flush();
        uint64_t now=GetTickCount64();
        for(unsigned i=0;i<300;++i){t.poll(now);Sleep(1);++now;} // let the real query resolve
        t.poll(now+40000); // force the window closed
        auto s=t.summary();
        check(s.windows==1,"exactly one window closed");
        check(s.samples>=1,"a real bracket around real GPU work produces at least one sample");
        t.shutdown();
    }

    // 5. A disjoint result is counted as a session-level failure, never a
    // sample -- distinct from a span dropped merely for still being pending.
    {
        Ops o;o.disjoint=true;
        auto a=makeTexture(device.Get()),b=makeTexture(device.Get());
        ProducerGpuTiming t;
        t.beginCopy(device.Get(),context.Get(),o.callbacks());
        context->CopyResource(b.Get(),a.Get());
        t.endCopy();
        context->Flush();
        t.poll(1);
        t.poll(1ull+40000);
        auto s=t.summary();
        check(s.samples==0&&s.disjointInvalid>=1,"a disjoint result never becomes a sample");
        t.shutdown();
    }

    std::printf("producer_gpu_timing_test: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
