#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <array>

#include "../../src/common/vtable_hook.h"
#include "../../src/d3d11/gpu_disjoint_d3d11.h"
#include "../../src/d3d11/game_query_probe.h"
#include "../../src/d3d11/game_exit_probe.h"
#include "../../src/common/system_d3d11.h"
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace edvr;

namespace {

unsigned checks = 0;
void check(bool ok, const char* why) {
    ++checks;
    if (!ok) throw std::runtime_error(why);
}
void hr(HRESULT value) { check(value == S_OK, "D3D operation failed"); }

struct Runtime {
    decltype(&D3D11CreateDevice) create = systemD3D11CreateDevice();
    Runtime() { check(create != nullptr, "System32 D3D11CreateDevice"); }
};

struct Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    explicit Device(Runtime& runtime) {
        D3D_FEATURE_LEVEL level{};
        hr(runtime.create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr,
                          0, D3D11_SDK_VERSION, &device, &level, &context));
    }
};

using BeginFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*);
using EndFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*);
// D3D11 SDK ID3D11DeviceContext order: Begin and End are slots 27 and 28.
constexpr size_t kBeginSlot = 27;
constexpr size_t kEndSlot = 28;
// Include every base-interface method: Flush/GetType/GetContextFlags are
// later than the hooked methods. The installed SDK ends at FinishCommandList.
constexpr size_t kContextMethods = 115;
constexpr size_t kVtableProbeLimit = 512; // VTableHook's production default.

struct HookLayer {
    ID3D11DeviceContext* owner = nullptr;
    BeginFn beginForward = nullptr;
    EndFn endForward = nullptr;
    unsigned beginHits = 0;
    unsigned endHits = 0;
};

HookLayer* gLayerOne = nullptr;
HookLayer* gLayerTwo = nullptr;
bool gRoutingFault = false;

void STDMETHODCALLTYPE beginOne(ID3D11DeviceContext* self, ID3D11Asynchronous* query) noexcept {
    HookLayer& layer = *gLayerOne;
    if (self == layer.owner) ++layer.beginHits;
    if (!layer.beginForward) { gRoutingFault = true; return; }
    layer.beginForward(self, query);
}
void STDMETHODCALLTYPE endOne(ID3D11DeviceContext* self, ID3D11Asynchronous* query) noexcept {
    HookLayer& layer = *gLayerOne;
    if (self == layer.owner) ++layer.endHits;
    if (!layer.endForward) { gRoutingFault = true; return; }
    layer.endForward(self, query);
}
void STDMETHODCALLTYPE beginTwo(ID3D11DeviceContext* self, ID3D11Asynchronous* query) noexcept {
    HookLayer& layer = *gLayerTwo;
    if (self == layer.owner) ++layer.beginHits;
    if (!layer.beginForward) { gRoutingFault = true; return; }
    layer.beginForward(self, query);
}
void STDMETHODCALLTYPE endTwo(ID3D11DeviceContext* self, ID3D11Asynchronous* query) noexcept {
    HookLayer& layer = *gLayerTwo;
    if (self == layer.owner) ++layer.endHits;
    if (!layer.endForward) { gRoutingFault = true; return; }
    layer.endForward(self, query);
}

struct Workload {
    ComPtr<ID3D11Texture2D> source;
    ComPtr<ID3D11Texture2D> copy;
    ComPtr<ID3D11RenderTargetView> target;

    explicit Workload(Device& device) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 64;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr(device.device->CreateTexture2D(&desc, nullptr, &source));
        hr(device.device->CreateTexture2D(&desc, nullptr, &copy));
        hr(device.device->CreateRenderTargetView(source.Get(), nullptr, &target));
    }

    void issue(Device& device) {
        const float colour[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        for (unsigned i = 0; i != 24; ++i) {
            device.context->ClearRenderTargetView(target.Get(), colour);
            device.context->CopyResource(copy.Get(), source.Get());
        }
    }

    void verify(Device& device) {
        D3D11_TEXTURE2D_DESC desc{};
        copy->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        hr(device.device->CreateTexture2D(&desc, nullptr, &staging));
        device.context->CopyResource(staging.Get(), copy.Get());
        device.context->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr(device.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        bool valid = true;
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* row = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch;
            for (UINT x = 0; x < desc.Width; ++x) {
                const auto* pixel = row + x * 4;
                valid = valid && pixel[0] == 64 && pixel[1] == 128 &&
                        pixel[2] == 191 && pixel[3] == 255;
            }
        }
        device.context->Unmap(staging.Get(), 0);
        check(valid, "exact pixel readback outside measured scopes");
    }
};

struct Cleanup {
    DisjointClock& clock;
    VTableHook& upper;
    VTableHook& lower;
    bool complete = false;
    ~Cleanup() {
        if (!complete) {
            clock.shutdown(GetTickCount64());
            upper.uninstall();
            lower.uninstall();
        }
    }
};

void queryProbeTests() {
    GameQueryProbe probe;
    auto* q=reinterpret_cast<ID3D11Asynchronous*>(uintptr_t(1));
    check(probe.observe(q,0x123,S_FALSE,100).first,"query probe records first direct observation");
    check(!probe.observe(q,0x123,S_FALSE,1099).detail,"ordinary pending query is not called stalled");
    check(probe.observe(q,0x123,S_FALSE,1100).detail,"repeated pending interval reports after threshold");
    check(!probe.observe(q,0x123,S_FALSE,2100).detail,"same delayed interval reports only once");
    probe.ended(q);
    check(!probe.observe(q,0x123,S_FALSE,2200).detail,"query reuse after End starts a new interval");
    probe.observe(q,0x123,S_OK,2300);
    auto summary=probe.observe(nullptr,0x123,E_FAIL,20100);
    check(summary.summary && summary.outstanding==0 && summary.ready==1 && summary.failed==1,
          "completed query and error retire pending accounting without retaining COM objects");
    GameQueryProbe pressure;
    for(uintptr_t i=1;i<=65;++i)pressure.observe(reinterpret_cast<ID3D11Asynchronous*>(i),0x456,S_FALSE,100);
    check(pressure.observe(q,0x456,S_FALSE,1100).detail,"table pressure preserves existing unresolved query");
    summary=pressure.observe(q,0x456,S_OK,20100);
    check(summary.overflow==1 && summary.outstanding==63,"overflow is explicit and completion frees tracking capacity");
    // Exercise the production forwarding adapter with caller-owned bytes and
    // unusual flags: exactly one underlying call, exact HRESULT, no writes by probe.
    for(HRESULT result:{S_OK,S_FALSE,E_FAIL}) {
        unsigned calls=0;UINT bytes[3]={0xaabbccdd,0x11223344,0x55667788};
        auto forward=[&](ID3D11DeviceContext* ctx,ID3D11Asynchronous* query,void* data,UINT n,UINT flags) {
            ++calls;check(!ctx && query==q && data==&bytes[1] && n==4 && flags==0x1234,"query forward arguments unchanged");
            if(result==S_OK)*static_cast<UINT*>(data)=42;return result;
        };
        const auto got=probe.read(forward,nullptr,q,&bytes[1],4,0x1234,nullptr);
        check(got==result && calls==1 && bytes[0]==0xaabbccdd && bytes[2]==0x55667788 &&
              bytes[1]==(result==S_OK?42u:0x11223344u),"query result and buffer preserved for ready, pending and error");
    }
}

void queryBracketTests(Device& device) {
    auto makeQuery = [&](D3D11_QUERY type) {
        ComPtr<ID3D11Query> query;
        const D3D11_QUERY_DESC desc{type, 0};
        hr(device.device->CreateQuery(&desc, &query));
        return query;
    };

    auto occlusion = makeQuery(D3D11_QUERY_OCCLUSION);
    auto unrelated = makeQuery(D3D11_QUERY_OCCLUSION);
    auto disjoint = makeQuery(D3D11_QUERY_TIMESTAMP_DISJOINT);

    GameQueryProbe probe;
    check(!probe.countingActive(), "query guard starts open");
    check(!probe.guard().counting && !probe.guard().disjoint && !probe.guard().overflow,
          "expanded query guard starts open");
    probe.bracketBegin(nullptr);
    probe.bracketEnd(nullptr);
    check(!probe.countingActive(), "null query brackets leave replay open");
    probe.bracketBegin(disjoint.Get());
    check(!probe.countingActive(), "timestamp-disjoint interval leaves replay open");
    check(probe.guard().disjoint && !probe.guard().counting,
          "timestamp-disjoint interval is separately guarded");
    probe.bracketEnd(disjoint.Get());
    check(!probe.guard().disjoint, "matching disjoint End clears separate guard");

    probe.bracketBegin(occlusion.Get());
    check(probe.countingActive(), "occlusion interval blocks replay");
    check(probe.guard().counting && !probe.guard().disjoint,
          "counting interval is separately classified");
    probe.bracketEnd(unrelated.Get());
    check(probe.countingActive(), "unknown End does not clear an active counting query");
    probe.bracketBegin(occlusion.Get());
    probe.bracketEnd(occlusion.Get());
    check(probe.countingActive(), "duplicate Begin remains blocked until every matching End");
    probe.bracketEnd(nullptr);
    check(probe.countingActive(), "null End does not clear an active counting query");
    probe.bracketEnd(occlusion.Get());
    check(!probe.countingActive(), "final matching End reopens replay");

    GameQueryProbe overflow;
    std::array<ComPtr<ID3D11Query>, 65> active;
    for(auto& query : active) {
        query = makeQuery(D3D11_QUERY_OCCLUSION);
        overflow.bracketBegin(query.Get());
    }
    check(overflow.countingActive(), "more than 64 active query identities blocks replay");
    check(overflow.guard().overflow, "active query identity overflow is exposed to diagnostics");
    overflow.bracketEnd(unrelated.Get());
    check(overflow.countingActive(), "unrelated End cannot clear query-identity overflow");
    for(auto& query : active) overflow.bracketEnd(query.Get());
    check(overflow.countingActive(), "query-identity overflow stays conservative after every known End");
}

void run() {
    {
        GameCallProbeBudget budget;
        for(unsigned i=1;i<=4;++i)check(budget.take(100)==i,"four early call-stack samples");
        check(!budget.take(99)&&!budget.take(20099),"stack sampling handles rollback and suppresses repeated frames");
        check(budget.take(20100)==5,"stack sampling uses elapsed wall time");
        for(unsigned i=6;i<=16;++i)check(budget.take(20100+uint64_t(i-5)*20000)==i,"periodic samples until finite cap");
        check(!budget.take(100000000),"stack sample cap does not reopen later");
        GameExitProbeSchedule schedule;
        check(!schedule.take(1,EDVR_NATIVE_STARTING),"startup does not consume present sample budgets");
        for(unsigned i=0;i<100;++i)schedule.take(20000ull*i,EDVR_NATIVE_RUNNING);
        check(schedule.take(3600000,EDVR_NATIVE_STOPPED)==1 && schedule.take(3601000,EDVR_NATIVE_STOPPED)==2,
              "long session preserves independent post-stop budget");
        check(schedule.take(3602000,EDVR_NATIVE_RETAINED)==3,"retained teardown is observable without claiming cleanup");
        const auto stack=captureGameCallStack();
        check(stack.captured && stack.gameFrames && stack.rvas[0] && stack.rvas[sizeof(stack.rvas)-1]==0,
              "actual return-address capture includes executable RVAs and stays bounded");
        GameCallProbeBudget concurrent;std::atomic<unsigned> accepted{0};
        auto calls=[&]{for(unsigned i=0;i<1000;++i)if(concurrent.take(100))++accepted;};
        std::thread a(calls),b(calls);a.join();b.join();
        check(accepted==4,"concurrent early calls share a fixed sample budget");
    }
    queryProbeTests();
    Runtime runtime; // Retained until Device, hooks, backend and queries release.
    Device device(runtime);
    queryBracketTests(device);
    ID3D11DeviceContext* context = device.context.Get();
    check(context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE, "immediate context");
    ComPtr<ID3D11Device> queriedDevice;
    context->GetDevice(&queriedDevice);
    check(queriedDevice.Get() == device.device.Get(), "context identity GetDevice");

    void** originalVptr = *reinterpret_cast<void***>(context);
    HookLayer one{context}, two{context};
    gLayerOne = &one;
    gLayerTwo = &two;
    VTableHook lower;
    check(lower.attach(context, kVtableProbeLimit) && lower.executablePrefix() >= kContextMethods &&
              lower.setMode(HookMode::LiveCopy), "lower LiveCopy attach/extent");
    check(lower.replace(kBeginSlot, reinterpret_cast<void*>(&beginOne), reinterpret_cast<void**>(&one.beginForward)),
          "lower Begin replacement");
    check(lower.replace(kEndSlot, reinterpret_cast<void*>(&endOne), reinterpret_cast<void**>(&one.endForward)),
          "lower End replacement");
    check(lower.commit(), "lower LiveCopy commit");

    VTableHook upper;
    check(upper.attach(context, kVtableProbeLimit) && upper.executablePrefix() >= kContextMethods &&
              upper.setMode(HookMode::LiveCopy), "upper LiveCopy attach/extent");
    check(upper.replace(kBeginSlot, reinterpret_cast<void*>(&beginTwo), reinterpret_cast<void**>(&two.beginForward)),
          "upper Begin replacement");
    check(upper.replace(kEndSlot, reinterpret_cast<void*>(&endTwo), reinterpret_cast<void**>(&two.endForward)),
          "upper End replacement");
    check(upper.commit(), "upper LiveCopy commit");

    check(context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE, "stacked GetType stable");
    queriedDevice.Reset();
    context->GetDevice(&queriedDevice);
    check(queriedDevice.Get() == device.device.Get(), "stacked GetDevice stable");

    DisjointD3D11Backend backend(device.device.Get(), context);
    const auto owner = backend.currentOwner();
    check(owner.context == reinterpret_cast<uintptr_t>(context) &&
          owner.device == reinterpret_cast<uintptr_t>(device.device.Get()) &&
          owner.thread == GetCurrentThreadId() && owner.immediate,
          "backend owner identity");
    DisjointClock clock(backend);
    Cleanup cleanup{clock, upper, lower};
    Workload work(device);
    std::array<ComPtr<ID3D11Query>, 4> timestamps;
    const D3D11_QUERY_DESC timestampDesc{D3D11_QUERY_TIMESTAMP, 0};
    for (auto& timestamp : timestamps) hr(device.device->CreateQuery(&timestampDesc, &timestamp));

    const auto now = GetTickCount64();
    auto frame = clock.startFrame(now);
    auto outer = clock.acquireInterval(now);
    check(bool(frame) && bool(outer), "parent and outer borrower");
    context->End(timestamps[0].Get());
    auto inner = clock.acquireInterval(GetTickCount64());
    check(bool(inner), "inner borrower");
    context->End(timestamps[1].Get());
    work.issue(device);
    context->End(timestamps[2].Get());
    check(clock.endInterval(inner, GetTickCount64()), "inner borrower ends");
    context->End(timestamps[3].Get());
    check(clock.endInterval(outer, GetTickCount64()), "outer borrower ends");
    check(clock.finishFrame(frame, GetTickCount64()), "one physical disjoint End");

    work.verify(device); // test-owned Flush/Map and readback are outside scopes.
    const auto deadline = GetTickCount64() + 1500;
    DisjointResult result{};
    do {
        result = clock.poll(inner, GetTickCount64());
        if (result.status == DisjointStatus::Pending) Sleep(1);
    } while (result.status == DisjointStatus::Pending && GetTickCount64() < deadline);
    check(result.status == DisjointStatus::Ready && result.frequency != 0, "disjoint result ready");
    const DisjointResult outerResult = clock.poll(outer, GetTickCount64());
    const DisjointResult parentResult = clock.poll(frame, GetTickCount64());
    check(outerResult.status == DisjointStatus::Ready && outerResult.frequency == result.frequency,
          "outer borrower is ready with shared frequency");
    check(parentResult.status == DisjointStatus::Ready && parentResult.frequency == result.frequency,
          "parent is ready with shared frequency");

    UINT64 ticks[4]{};
    for (unsigned i = 0; i != 4; ++i) {
        HRESULT status = S_FALSE;
        do {
            status = context->GetData(timestamps[i].Get(), &ticks[i], sizeof(ticks[i]),
                                      D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (status == S_FALSE) Sleep(1);
        } while (status == S_FALSE && GetTickCount64() < deadline);
        hr(status);
    }
    check(ticks[0] <= ticks[1] && ticks[1] < ticks[2] && ticks[2] <= ticks[3],
          "positive nested timestamp intervals");
    check(!gRoutingFault && one.beginHits == 1 && two.beginHits == 1 && one.endHits == 5 && two.endHits == 5,
          "each stacked Begin/End thunk forwards exactly once per native call");
    const double nestedMs = double(ticks[2] - ticks[1]) * 1000.0 / double(result.frequency);

    check(clock.shutdown(GetTickCount64()), "clock owner-thread shutdown");
    upper.uninstall();
    lower.uninstall();
    check(*reinterpret_cast<void***>(context) == originalVptr, "reverse uninstall restores original vptr");
    cleanup.complete = true;
    std::printf("PASS: two LiveCopy layers, one disjoint scope, nested %.4f ms, %u checks\n",
                nestedMs, checks);
    gLayerTwo = nullptr;
    gLayerOne = nullptr;
}

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
