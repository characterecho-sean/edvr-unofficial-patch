#include "../../src/d3d11/native_timing.h"
#include "../../src/d3d11/gpu_frame_timing.h"
#include "../../src/d3d11/gpu_timing.h"
#include "../../src/openxr/native_timing_client.h"
#include "../../src/openxr/render_route.h"
#include "../../src/common/system_d3d11.h"
#include <wrl/client.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace edvr::openxr;
#pragma comment(linker, "/EXPORT:edvrAcquireNativeTiming")
#pragma comment(linker, "/EXPORT:edvrReadNativePresentTrace")
namespace {
std::atomic<unsigned> checks{0};
void require(bool value, const char* why) {
    ++checks;
    if (!value) throw std::runtime_error(why);
}
struct Watchdog {
    std::atomic<bool> done{false};
    std::thread worker{[this] {
        const auto deadline=GetTickCount64()+20000;
        while (!done) { if (GetTickCount64()>deadline) std::_Exit(9); Sleep(10); }
    }};
    ~Watchdog() { done=true; worker.join(); }
};
struct Service {
    OwnerService owner;
    RenderThreadDispatcher dispatcher{owner};
    RenderRoute route{dispatcher};
    CountedGraphics graphics{dispatcher};
    Service() { require(route.bind() && owner.start(), "render route and owner start"); }
    ~Service() { dispatcher.close(); owner.stop(); }
};
ComPtr<ID3D11Texture2D> texture(ID3D11Device* device, unsigned color) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width=d.Height=256; d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.BindFlags=D3D11_BIND_RENDER_TARGET;
    std::vector<unsigned> pixels(size_t(d.Width)*d.Height,color);
    D3D11_SUBRESOURCE_DATA data{pixels.data(),d.Width*4,0};
    ComPtr<ID3D11Texture2D> result;
    require(SUCCEEDED(device->CreateTexture2D(&d,&data,&result)), "initialized texture");
    return result;
}
void run() {
    Watchdog watchdog;
    const auto create=edvr::systemD3D11CreateDevice();
    require(create!=nullptr, "system D3D11 module (no proxy import)");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; D3D_FEATURE_LEVEL level{};
    require(create && SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&device,&level,&context)), "WARP device");
    require(edvr::gpuFrameBind(device.Get(),context.Get(),true), "actual production frame clock");
    Service service;
    NativeTimingClient client;
    require(service.route.invoke([&] {
        require(service.graphics.invoke([&] {
            require(client.acquire(GetModuleHandleW(nullptr),device.Get(),41)==S_OK,
                "module-owned production capability on producer");
        }), "acquire producer callback");
    }), "acquire owner route");
    require(client.presentTraceAvailable(), "optional Present trace export resolved from provider module");
    EdvrNativePresentSpan present{100,110,120,130,140,GetCurrentThreadId(),1,7,S_OK};
    const auto presentToken=edvr::nativeTimingPresentBegin(device.Get(),present.beginUs,present.thread);
    edvr::nativeTimingNotePresent(device.Get(),presentToken,present);
    EdvrNativePresentTrace presentTrace{};
    require(client.readPresentTrace(120,130,presentTrace) && presentTrace.generation==41 &&
        presentTrace.totalObserved==1 && presentTrace.count==1 && !presentTrace.overflow &&
        presentTrace.spans[0].thread==present.thread && presentTrace.spans[0].flags==7,
        "client reads owned provider Present trace end to end");
    auto source=texture(device.Get(),0xff4488dd), copy=texture(device.Get(),0xff000000);
    ComPtr<ID3D11RenderTargetView> target;
    require(SUCCEEDED(device->CreateRenderTargetView(source.Get(),nullptr,&target)), "render target");
    uint64_t previousSequence=0;
    double observedOuter=0;
    double observedApplication=0;
    // Both eye orders, then disabled GPU: same owner/producer and CPU path.
    for (unsigned frame=0; frame<3; ++frame) {
        const bool enabled=frame!=2;
        edvr::gpuFrameConfigure(enabled);
        uint64_t sequence=0;
        const auto callbacksBeforeWait=service.graphics.calls;
        require(service.owner.invoke([&] {
            sequence=client.waitBegin();
            require(sequence>previousSequence, "global monotonic sequence");
            require(client.waitEnd(sequence,true,11111111)==S_OK, "CPU-only wait on owner");
        }), "wait owner invocation without render admission");
        require(service.graphics.calls==callbacksBeforeWait, "wait issues no graphics callbacks");
        if (frame) require(edvr::nativeTimingSnapshot().cpu.sequence==previousSequence,
            "previous completed CPU sample survives next wait");
        // The producer admission and first CPU marker happen after the owner
        // route returns. Deliberate work before the marker must stay outside
        // the application interval.
        Sleep(15);
        require(client.producerResume(sequence)==enabled,
            "initial producer GPU admission");
        require(client.applicationSegment(sequence,true), "initial CPU application segment");
        edvr::gpuFrameCommand(context.Get()); // Same pre-command seam used by the game hooks.
        const float color[4]={.1f,.3f,.6f,1};
        context->ClearRenderTargetView(target.Get(),color);
        EdvrNativeTimingFrame cpu{sizeof(cpu),EDVR_NATIVE_TIMING_VERSION_5,sequence};   // no cycle instrument here: caller work absent
        for (unsigned order=0; order<2; ++order) {
            const unsigned eye=order^(frame&1);
            require(client.applicationSegment(sequence,false), "CPU segment closes at submit route entry");
            require(client.producerPause(sequence)==enabled, "producer GPU pause before submit route");
            require(service.route.invoke([&] {
                LARGE_INTEGER began{},ended{},frequency{};
                require(QueryPerformanceCounter(&began)!=FALSE, "submit QPC begin");
                require(service.graphics.invoke([&] {
                    require(client.gpuEye(sequence,eye,true,false,source.Get())==enabled,
                        "GPU begin enabled/disabled state");
                    require(client.applicationSegment(sequence,true), "treatment CPU segment begins");
                    // A nested pass uses the same production frequency scope.
                    edvr::GpuTimer pass;
                    require(pass.begin(device.Get(),context.Get()), "nested pass starts");
                    for(unsigned n=0;n<8;++n)context->CopyResource(copy.Get(),source.Get());
                    require(pass.end(context.Get()), "nested pass ends");
                    pass.reset(context.Get());
                    Sleep(3); // Deliberate measured producer work.
                    require(client.applicationSegment(sequence,false), "treatment CPU segment ends");
                    require(client.producerSegmentEnd(sequence,eye,source.Get())==enabled,
                        "GPU treatment segment ends before transfer");
                }), "producer processing callback");
                require(service.graphics.invoke([&] {
                    context->CopyResource(source.Get(),copy.Get()); // final producer transfer
                }), "producer transfer callback");
                if(enabled) require(service.graphics.invoke([&] {
                    require(client.gpuEye(sequence,eye,false,true,source.Get()), "end after producer transfer");
                }), "post-submit marker callback");
                require(QueryPerformanceCounter(&ended) && QueryPerformanceFrequency(&frequency), "submit QPC end");
                cpu.submitMs[eye]=double(ended.QuadPart-began.QuadPart)*1000.0/double(frequency.QuadPart);
            }), "submit owner route");
            if(order==0) {
                // Between-eye admission starts after the first submit route
                // returns. No producer command is issued here, so the next
                // treatment creates the third GPU segment directly.
                require(client.producerResume(sequence)==enabled,
                    "between-eye producer GPU admission");
                require(client.applicationSegment(sequence,true), "between-eye CPU segment");
            }
        }
        require(service.owner.invoke([&] { require(client.publishCpu(cpu)==S_OK, "complete stereo CPU publication"); }),
            "CPU publish without graphics admission");
        const auto snapshot=edvr::nativeTimingSnapshot();
        require(snapshot.haveCpu && snapshot.cpu.sequence==sequence && !snapshot.invalid &&
            snapshot.predictedPeriodMs>0 && snapshot.cpu.submitMs[0]>0,
            "fresh CPU sample");
        require(snapshot.applicationValid, "segmented application CPU sample is complete");
        require(snapshot.applicationMs>4.0, "segmented application CPU work is measured");
        observedApplication=snapshot.applicationMs;
        // Only this fixture flushes; production readback is nonblocking and DONOTFLUSH.
        context->Flush();
        edvr::GpuFrameSnapshot gpu;
        const auto deadline=GetTickCount64()+1500;
        do {
            edvr::gpuFramePresent(context.Get(),frame+2);
            gpu=edvr::gpuFrameSnapshot();
            if(!enabled || (gpu.haveResult && gpu.result.sequence==sequence))break;
            Sleep(1);
        } while(GetTickCount64()<deadline);
        if(enabled) {
            require(gpu.enabled && gpu.haveResult && gpu.result.sequence==sequence &&
                gpu.result.reason==edvr::GpuSpanReason::Valid, "asynchronous actual GPU span settles valid");
            require(std::isfinite(gpu.result.outerMs) && gpu.result.outerMs>0 &&
                gpu.result.leftMs>=0 && gpu.result.rightMs>=0 &&
                gpu.result.outerMs+0.00001>=gpu.result.leftMs+gpu.result.rightMs,
                "actual GPU timestamps enclose both submits");
            require(gpu.result.sourceFrame==frame+1, "original source frame retained");
            observedOuter=gpu.result.outerMs;
        } else require(!gpu.enabled && !gpu.haveResult && edvr::nativeTimingSnapshot().haveCpu,
            "disabled GPU leaves complete CPU timing available");
        previousSequence=sequence;
    }
    const auto callbacksBeforeClose=service.graphics.calls;
    require(service.owner.invoke([&] {
        require(client.invalidate()==S_OK && !edvr::nativeTimingSnapshot().haveCpu, "CPU invalidation clears sample");
        require(client.close()==S_OK && client.close()==S_FALSE, "CPU close is idempotent");
    }), "CPU close without graphics admission");
    require(service.graphics.calls==callbacksBeforeClose && service.graphics.wrongThread==0 &&
        service.graphics.rejected==0 && !edvr::nativeTimingSnapshot().active, "close has no graphics rendezvous");
    edvr::gpuFrameAbandon();
    require(edvr::gpuTimingShutdown(context.Get()), "explicit producer query cleanup");
    edvr::gpuTimingAbandon();
    std::printf("native_timing_gpu_test: %u checks, 0 failures (actual WARP queries, last span %.4f ms)\n",
        checks.load(),observedOuter);
}
}
int wmain(int argc,wchar_t** argv) {
    SetErrorMode(3);
    if(argc==2 && !wcscmp(argv[1],L"--dry-run")) {
        std::puts("native_timing_gpu_test: dry-run (no device, runtime, thread or files)"); return 0;
    }
    if(argc!=2 || wcscmp(argv[1],L"--self-test"))return 2;
    try { run(); return 0; }
    catch(const std::exception& error) { std::printf("FAIL: %s\n",error.what()); return 1; }
}
