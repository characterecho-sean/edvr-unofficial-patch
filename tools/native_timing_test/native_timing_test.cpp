#include "../../src/common/native_timing.h"
#include "../../src/d3d11/native_timing.h"
#include "../../src/common/gpu_frame_protocol.h"
#include "../../src/common/config.h"
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <thread>
#include <atomic>
#include <cmath>
#include <string>
using Microsoft::WRL::ComPtr;
#pragma comment(linker, "/EXPORT:edvrAcquireNativeTiming")
static std::atomic<uint64_t> nextSequence{0};
static std::atomic<unsigned> eventCalls[8], cancelCalls{0};
static std::atomic<DWORD> waitBeginThread{0}, waitEndThread{0}, submitThread{0};
static bool gpuEnabled = true;
extern "C" uint64_t WINAPI edvrGpuFrameEvent(unsigned protocol, unsigned event,
    uint64_t, unsigned, unsigned flags, void*) {
    if (protocol != edvr::kGpuFrameProtocol || event > static_cast<unsigned>(edvr::GpuFrameEvent::SegmentEnd)) return 0;
    ++eventCalls[event];
    if (event == static_cast<unsigned>(edvr::GpuFrameEvent::WaitBegin)) { waitBeginThread=GetCurrentThreadId(); return ++nextSequence; }
    if (event == static_cast<unsigned>(edvr::GpuFrameEvent::WaitEnd)) { waitEndThread=GetCurrentThreadId(); return flags == edvr::kGpuFrameNativeWait; }
    if (event == static_cast<unsigned>(edvr::GpuFrameEvent::Cancel)) { ++cancelCalls; return 1; }
    submitThread=GetCurrentThreadId();
    return gpuEnabled && (event != static_cast<unsigned>(edvr::GpuFrameEvent::SubmitEnd) || flags == 1) ? 1 : 0;
}
struct Checks { unsigned count=0, failures=0; void check(bool ok,const char* s){++count;if(!ok){++failures;std::printf("FAIL: %s\n",s);}} };
static EdvrNativeTimingFrame frame(uint64_t seq) {
    EdvrNativeTimingFrame f{sizeof(f),EDVR_NATIVE_TIMING_VERSION_3,seq};
    f.submitMs[0]=.2;f.submitMs[1]=.3;f.temporalMs[0]=.1;f.temporalMs[1]=.1;
    f.menuMs[0]=.05;f.menuMs[1]=.06;f.transferMs[0]=.4;f.transferMs[1]=.5;f.composeMs=.7;return f;
}
static uint64_t waitValid(EdvrNativeTimingTable& t,Checks& c,int64_t period=11111111) {
    uint64_t seq=0;HRESULT result=E_FAIL;std::thread cpu([&]{seq=t.waitBegin(t.context);result=t.waitEnd(t.context,seq,1,period);});cpu.join();
    c.check(seq&&result==S_OK,"CPU wait callbacks");c.check(waitBeginThread!=GetCurrentThreadId()&&waitEndThread!=GetCurrentThreadId(),"CPU callback thread recorded");return seq;
}
static EdvrNativeTimingTable acquire(ID3D11Device* d,uint64_t gen,Checks& c){
    EdvrNativeTimingRequest r{sizeof(r),EDVR_NATIVE_TIMING_VERSION_3,d,gen};EdvrNativeTimingTable t{sizeof(t),EDVR_NATIVE_TIMING_VERSION_3};
    c.check(edvrAcquireNativeTiming(&r,&t)==S_OK&&t.context,"acquire");return t;
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;if(!std::wcscmp(argv[1],L"--dry-run")){std::puts("native_timing_test: dry-run (no WARP device)");return 0;}if(std::wcscmp(argv[1],L"--self-test"))return 2;
    SetErrorMode(3);
    Checks c;ComPtr<ID3D11Device>d;ComPtr<ID3D11DeviceContext>dc;D3D_FEATURE_LEVEL fl{};
    wchar_t system[MAX_PATH]{};const auto len=GetSystemDirectoryW(system,MAX_PATH);
    if(!len||len>=MAX_PATH)return 1;
    const auto module=LoadLibraryW((std::wstring(system)+L"\\d3d11.dll").c_str());
    const auto create=module?reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(module,"D3D11CreateDevice")):nullptr;
    c.check(create&&SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&fl,&dc)),"WARP device");if(!d)return 1;
    EdvrNativeTimingRequest r{sizeof(r)-1,EDVR_NATIVE_TIMING_VERSION_3,d.Get(),1};EdvrNativeTimingTable bad{sizeof(bad),EDVR_NATIVE_TIMING_VERSION_3};c.check(edvrAcquireNativeTiming(&r,&bad)==E_INVALIDARG,"wrong request size");
    r={sizeof(r),EDVR_NATIVE_TIMING_VERSION_3+1,d.Get(),1};c.check(edvrAcquireNativeTiming(&r,&bad)==E_INVALIDARG,"wrong request version");r={sizeof(r),EDVR_NATIVE_TIMING_VERSION_3,d.Get(),1};bad={sizeof(bad)-1,EDVR_NATIVE_TIMING_VERSION_3};c.check(edvrAcquireNativeTiming(&r,&bad)==E_INVALIDARG,"wrong table size");bad={sizeof(bad),EDVR_NATIVE_TIMING_VERSION_3+1};c.check(edvrAcquireNativeTiming(&r,&bad)==E_INVALIDARG,"wrong table version");
    auto t=acquire(d.Get(),7,c);if(!t.context)return 1;c.check(t.gpuEnabled && t.publishDeviceGpu,"GPU callbacks published");auto s1=waitValid(t,c);auto f1=frame(s1);c.check(t.publishCpu(t.context,&f1)==S_OK,"publish frame");auto a=edvr::nativeTimingSnapshot();c.check(a.haveCpu&&a.sequence==s1&&a.firstSequence==s1,"snapshot identity");c.check(t.publishCpu(t.context,&f1)==E_INVALIDARG,"duplicate publish");
    auto s2=waitValid(t,c,22222222);auto mid=edvr::nativeTimingSnapshot();c.check(mid.haveCpu&&mid.sequence==s1&&mid.predictedPeriodMs==a.predictedPeriodMs,"preserve complete snapshot");
    // Four required wall segments bracket the producer's initial game work,
    // both treatments, and the between-eye route. Delays outside markers are
    // deliberately much larger than the measured sleeps.
    LARGE_INTEGER markerBegan{},markerEnded{},markerFrequency{};
    QueryPerformanceCounter(&markerBegan);
    Sleep(20);
    for(unsigned segment=0;segment<4;++segment) {
        c.check(t.gpuEye(t.context,s2,2,1,1,nullptr)==1,"application CPU segment begins");
        Sleep(1);
        c.check(t.gpuEye(t.context,s2,2,0,1,nullptr)==1,"application CPU segment ends");
    }
    QueryPerformanceCounter(&markerEnded);QueryPerformanceFrequency(&markerFrequency);
    EdvrNativeDeviceGpuSample g2{sizeof(g2),EDVR_NATIVE_TIMING_VERSION_3,s2,GetTickCount64(),EdvrNativeGpuPending,{0,0},{0,0}};g2.transferMs[0]=.4;g2.transferMs[1]=.5;g2.composeMs[0]=.6;g2.composeMs[1]=.7;c.check(t.publishDeviceGpu(t.context,&g2)==S_OK&&edvr::nativeTimingSnapshot().deviceGpu.status==EdvrNativeGpuPending,"publish pending device GPU");g2.status=EdvrNativeGpuValid;c.check(t.publishDeviceGpu(t.context,&g2)==S_OK&&edvr::nativeTimingSnapshot().haveDeviceGpu,"publish device GPU independently");auto f2=frame(s2);f2.composeMs=1.2;c.check(t.publishCpu(t.context,&f2)==S_OK,"second publish");auto b=edvr::nativeTimingSnapshot();const double markerWall=double(markerEnded.QuadPart-markerBegan.QuadPart)*1000.0/double(markerFrequency.QuadPart);c.check(b.sequence==s2&&b.predictedPeriodMs!=a.predictedPeriodMs&&b.haveDeviceGpu,"CPU application snapshot identity");c.check(b.applicationValid,"CPU application segments complete");c.check(b.applicationMs>=3.0&&b.applicationMs+10.0<markerWall,"CPU application segments exclude outside delays");
    auto s3=waitValid(t,c);c.check(edvr::nativeTimingSnapshot().haveDeviceGpu,"next wait preserves last device GPU");EdvrNativeDeviceGpuSample g3=g2;g3.sequence=s3;g3.completedAtMs=GetTickCount64();EdvrNativeDeviceGpuSample badGpu=g3;badGpu.size--;c.check(t.publishDeviceGpu(t.context,&badGpu)==E_INVALIDARG,"device GPU ABI rejected");badGpu=g3;badGpu.status=99;c.check(t.publishDeviceGpu(t.context,&badGpu)==E_INVALIDARG,"device GPU status rejected");badGpu=g3;badGpu.transferMs[0]=-1;c.check(t.publishDeviceGpu(t.context,&badGpu)==E_INVALIDARG,"device GPU negative rejected");badGpu=g3;badGpu.completedAtMs=GetTickCount64()+5000;c.check(t.publishDeviceGpu(t.context,&badGpu)==E_INVALIDARG,"device GPU future timestamp rejected");c.check(t.publishDeviceGpu(t.context,&g3)==S_OK,"publish current device GPU");auto f3=frame(s3);f3.submitMs[1]=-1;c.check(t.publishCpu(t.context,&f3)==E_INVALIDARG&&!edvr::nativeTimingSnapshot().haveCpu,"negative number clears");
    auto sNan=waitValid(t,c);auto nanFrame=frame(sNan);nanFrame.composeMs=NAN;c.check(t.publishCpu(t.context,&nanFrame)==E_INVALIDARG&&!edvr::nativeTimingSnapshot().haveCpu,"NaN clears");
    auto sInf=waitValid(t,c);auto infFrame=frame(sInf);infFrame.transferMs[0]=INFINITY;c.check(t.publishCpu(t.context,&infFrame)==E_INVALIDARG&&!edvr::nativeTimingSnapshot().haveCpu,"infinity clears");
    auto badWait=t.waitBegin(t.context);c.check(t.waitEnd(t.context,badWait,0,11111111)==E_INVALIDARG,"invalid wait flag");
    auto badValidity=t.waitBegin(t.context);c.check(t.waitEnd(t.context,badValidity,2,11111111)==E_INVALIDARG,"invalid validity value");
    auto s4=waitValid(t,c);auto badFrame=frame(s4);badFrame.size--;c.check(t.publishCpu(t.context,&badFrame)==E_INVALIDARG,"wrong frame size");badFrame=frame(s4);badFrame.version++;c.check(t.publishCpu(t.context,&badFrame)==E_INVALIDARG,"wrong frame version");
    auto badPeriod=t.waitBegin(t.context);c.check(t.waitEnd(t.context,badPeriod,1,-1)==E_INVALIDARG,"invalid period");
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=4;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;ComPtr<ID3D11Texture2D>tex;c.check(SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&tex)),"texture");
    auto s5=waitValid(t,c);c.check(t.gpuEye(t.context,s5,0,1,0,tex.Get())==1,"GPU begin");c.check(!t.gpuEye(t.context,s5,0,1,0,tex.Get()),"duplicate eye begin rejected");c.check(!t.gpuEye(t.context,s5,1,0,1,tex.Get()),"overlap eye end rejected");c.check(t.gpuEye(t.context,s5,0,0,0,tex.Get())==0,"abandoned pair rejected");c.check(!edvr::nativeTimingSnapshot().haveCpu,"abandoned pair clears CPU after wait end");
    auto s5b=waitValid(t,c);c.check(t.gpuEye(t.context,s5b,0,1,0,tex.Get())==1&&t.gpuEye(t.context,s5b,0,0,1,tex.Get())==1,"GPU markers");c.check(submitThread==GetCurrentThreadId(),"producer identity");uint32_t wrong=1;std::thread other([&]{wrong=t.gpuEye(t.context,s5b,1,1,0,tex.Get());});other.join();c.check(!wrong,"wrong producer rejected");c.check(!t.gpuEye(t.context,s5b,2,1,0,tex.Get()),"invalid eye rejected");
    ComPtr<ID3D11Device> foreign;ComPtr<ID3D11DeviceContext> foreignContext;D3D_FEATURE_LEVEL foreignLevel{};c.check(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&foreign,&foreignLevel,&foreignContext)),"foreign WARP device");ComPtr<ID3D11Texture2D> foreignTexture;if(foreign){c.check(SUCCEEDED(foreign->CreateTexture2D(&desc,nullptr,&foreignTexture)),"foreign texture");c.check(!t.gpuEye(t.context,s5b,1,1,0,foreignTexture.Get()),"wrong device rejected");}
    const auto staleSeq=waitValid(t,c);EdvrNativeDeviceGpuSample late=g3;late.sequence=staleSeq;late.completedAtMs=GetTickCount64()-2001;late.status=EdvrNativeGpuValid;
    c.check(t.publishDeviceGpu(t.context,&late)==E_INVALIDARG,"current sequence stale valid duration rejected");
    late.status=EdvrNativeGpuStale;c.check(t.publishDeviceGpu(t.context,&late)==S_OK&&edvr::nativeTimingSnapshot().deviceGpu.status==EdvrNativeGpuStale,"current sequence stale metadata retained");
    gpuEnabled=false;const auto independentSeq=waitValid(t,c);c.check(!t.gpuEye(t.context,independentSeq,0,1,0,tex.Get()),"missing producer span");
    auto independent=g3;independent.sequence=independentSeq;independent.completedAtMs=GetTickCount64();
    c.check(t.publishDeviceGpu(t.context,&independent)==S_OK,"separate XR GPU remains independent of unavailable producer span");
    auto independentCpu=frame(independentSeq);c.check(t.publishCpu(t.context,&independentCpu)==S_OK,"independent frame publishes");
    gpuEnabled=true;auto s6=waitValid(t,c);auto cancels=cancelCalls.load();gpuEnabled=false;
    c.check(t.gpuEye(t.context,s6,0,1,0,tex.Get())==0,"disabled GPU marker reports unavailable");EdvrNativeDeviceGpuSample disabled{sizeof(disabled),EDVR_NATIVE_TIMING_VERSION_3,s6,GetTickCount64(),EdvrNativeGpuDisabled,{0,0},{0,0}};c.check(t.publishDeviceGpu(t.context,&disabled)==S_OK&&edvr::nativeTimingSnapshot().haveDeviceGpu&&edvr::nativeTimingSnapshot().deviceGpu.status==EdvrNativeGpuDisabled,"disabled device GPU status is retained");
    auto f6=frame(s6);c.check(t.publishCpu(t.context,&f6)==S_OK&&edvr::nativeTimingSnapshot().haveCpu,"CPU survives disabled GPU");c.check(cancelCalls.load()==cancels,"disabled GPU no cancel");gpuEnabled=true;
    edvr::Config::get().set("advanced.app_gpu_timing","false");c.check(t.gpuEnabled(t.context)==0&&edvr::nativeTimingSnapshot().deviceGpu.status==EdvrNativeGpuDisabled,"config disables GPU timing");edvr::Config::get().set("advanced.app_gpu_timing","true");c.check(t.gpuEnabled(t.context)!=0,"config re-enables GPU timing");
    independent.completedAtMs=GetTickCount64();c.check(t.publishDeviceGpu(t.context,&independent)==E_INVALIDARG,"pre-disable GPU sample cannot resurrect");
    c.check(t.invalidate(t.context)==S_OK&&!edvr::nativeTimingSnapshot().haveCpu&&!edvr::nativeTimingSnapshot().haveDeviceGpu,"invalidate clears snapshots");c.check(t.publishDeviceGpu(t.context,&g2)==E_INVALIDARG,"old device GPU rejected after invalidation");c.check(t.close(t.context)==S_OK&&t.close(t.context)==S_FALSE,"repeated close");auto stale=t;auto t2=acquire(d.Get(),8,c);c.check(stale.invalidate(stale.context)==E_INVALIDARG,"old context cannot clear new");c.check(stale.close(stale.context)==S_FALSE,"old context close remains retired");c.check(t2.close(t2.context)==S_OK,"new close");
    uint64_t cursor=0,dropped=0;edvr::NativeTimingSnapshot completed[256]{};
    const unsigned count=edvr::nativeTimingReadCompletions(cursor,completed,256,dropped);
    bool ordered=true,negativeSeen=false,nanSeen=false,allIdentified=true;
    for(unsigned i=0;i<count;++i) {
        ordered=ordered&&(!i||completed[i].sequence>completed[i-1].sequence);
        allIdentified=allIdentified&&completed[i].firstSequence&&completed[i].capturedAtMs;
        if(completed[i].sequence==s3)negativeSeen=completed[i].invalid&&!completed[i].applicationValid;
        if(completed[i].sequence==sNan)nanSeen=completed[i].invalid&&!completed[i].haveCpu;
    }
    c.check(count>2&&!dropped&&ordered&&allIdentified,"completion history has one identified record per frame");
    c.check(negativeSeen&&nanSeen,"rejected frames remain visible as invalid benchmark coverage");
    c.check(!edvr::nativeTimingReadCompletions(cursor,completed,256,dropped),"completion cursor never replays records");
    auto t3=acquire(d.Get(),9,c);
    for(unsigned i=0;i<270;++i) {
        auto sequence=waitValid(t3,c);auto value=frame(sequence);
        value.inputWidth[0]=1234;value.inputHeight[0]=567;
        value.outputWidth[0]=2345;value.outputHeight[0]=678;
        value.gameFov[0][0]=-.75f;value.featureEpoch=42;value.treatments[0]=7;
        c.check(t3.publishCpu(t3.context,&value)==S_OK,"queue pressure frame publication");
    }
    dropped=0;
    c.check(edvr::nativeTimingReadCompletions(cursor,completed,256,dropped)==256&&dropped==14,
            "bounded completion queue reports exact overwritten count across rebind");
    const auto& last=completed[255].cpu;
    c.check(last.inputWidth[0]==1234&&last.inputHeight[0]==567&&last.outputWidth[0]==2345&&
            last.outputHeight[0]==678&&last.gameFov[0][0]==-.75f&&last.featureEpoch==42&&last.treatments[0]==7,
            "version three metadata travels with its own completion");
    c.check(t3.close(t3.context)==S_OK,"queue pressure context closes");
    EdvrNativeTimingRequest old{sizeof(old),2,d.Get(),10};
    EdvrNativeTimingTable modern{sizeof(modern),EDVR_NATIVE_TIMING_VERSION_3};
    c.check(edvrAcquireNativeTiming(&old,&modern)==E_INVALIDARG,"version two cannot silently use version three frame semantics");
    std::printf("native_timing_test: %u checks, %u failures\n",c.count,c.failures);return c.failures?1:0;
}
