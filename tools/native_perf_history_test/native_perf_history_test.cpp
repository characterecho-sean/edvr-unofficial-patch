#include "../../src/d3d11/native_perf_history.h"
#include <cstdio>
#include <cmath>
#include <cwchar>
#include <limits>

using namespace edvr;
namespace {
unsigned checks=0, failures=0;
void check(bool ok,const char* name){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",name);}}
bool approx(double a,double b){return std::fabs(a-b)<1e-8;}
NativeTimingSnapshot cpu(uint64_t seq,uint64_t at) {
    NativeTimingSnapshot s{};s.active=true;s.generation=7;s.firstSequence=1;s.sequence=seq;
    s.capturedAtMs=at;s.waitMs=4;s.predictedPeriodMs=11;s.haveCpu=true;
    s.cpu={sizeof(s.cpu),EDVR_NATIVE_TIMING_VERSION_2,seq};s.cpu.submitMs[0]=1;s.cpu.submitMs[1]=2;
    return s;
}
GpuFrameSnapshot gpu(uint64_t seq,uint64_t at,uint64_t age=0,double ms=5) {
    GpuFrameSnapshot s{};s.enabled=s.haveResult=true;s.capturedAtMs=at;
    s.result.sequence=seq;s.result.ageMs=age;s.result.outerMs=ms;s.result.reason=GpuSpanReason::Valid;return s;
}
void device(NativeTimingSnapshot& s,uint64_t seq,uint64_t at,uint32_t status=EdvrNativeGpuValid) {
    s.haveDeviceGpu=true;s.deviceGpu={sizeof(s.deviceGpu),EDVR_NATIVE_TIMING_VERSION_2,seq,at,status,{1,2},{3,4}};
}
void averages() {
    NativePerfHistory h;auto t=cpu(1,10000);auto g=gpu(1,10000);device(t,1,10000);
    h.observe(true,t,g,10000);
    for(int i=0;i<100;++i)h.observe(true,t,g,10001);
    check(h.submit(10001,200).count==1&&h.producer(10001,200).count==1&&h.transfer(10001,200).count==1,"duplicates never weight any stream");
    check(approx(h.submit(10001,200).meanMs,3)&&approx(h.wait(10001,200).meanMs,4),"CPU submit and wait remain distinct");
    check(approx(h.producer(10001,200).meanMs,5)&&approx(h.transfer(10001,200).meanMs,3)&&approx(h.compose(10001,200).meanMs,7),"producer, XR copy and XR composition remain independent");
    t=cpu(2,10100);t.cpu.submitMs[0]=7;device(t,2,10100);h.observe(true,t,g,10100);
    check(h.submit(10100,200).count==2&&approx(h.submit(10100,200).meanMs,6)&&h.producer(10100,200).count==1,"asynchronous sources keep separate sample counts");
    check(h.submit(10201,200).count==1&&approx(h.submit(10201,200).meanMs,9)&&!h.producer(10201,200).count,"200ms window ages each source without fallback");
    check(!h.submit(12101,10000).count&&!h.predictedPeriod(12101),"stale current source hides older averages and prediction");
    t=cpu(3,12200);t.cpu.submitMs[0]=t.cpu.submitMs[1]=t.waitMs=0;device(t,3,12200);for(double& x:t.deviceGpu.transferMs)x=0;for(double& x:t.deviceGpu.composeMs)x=0;
    h.observe(true,t,gpu(3,12200,0,0),12200);
    check(h.submit(12200,200).count==1&&h.submit(12200,200).meanMs==0&&h.wait(12200,200).count==1,"measured zero CPU durations count");
    check(h.producer(12200,200).count==1&&h.producer(12200,200).meanMs==0&&h.transfer(12200,200).count==1&&h.compose(12200,200).meanMs==0,"measured zero GPU durations count");
}
void invalidation() {
    NativePerfHistory h;auto t=cpu(10,10000);auto g=gpu(10,10000);device(t,10,10000);h.observe(true,t,g,10000);
    auto bad=t;bad.invalid=true;bad.sequence=bad.cpu.sequence=0;bad.haveDeviceGpu=false;h.observe(true,bad,{},10000);
    check(!h.submit(10000,200).count&&!h.producer(10000,200).count&&!h.transfer(10000,200).count&&!h.predictedPeriod(10000),"recenter clears all histories and prediction");
    h.observe(true,t,g,10000);check(!h.submit(10000,200).count&&!h.producer(10000,200).count&&!h.transfer(10000,200).count,"cleared snapshots cannot replay even when invalidation lost their identity");
    t=cpu(11,10010);device(t,11,10010);g=gpu(11,10010);h.observe(true,t,g,10010);
    check(h.submit(10010,200).count==1&&h.producer(10010,200).count==1,"later completion recovers");
    g.enabled=false;device(t,12,10010,EdvrNativeGpuDisabled);h.observe(true,t,g,10010);
    check(!h.producer(10010,200).count&&!h.transfer(10010,200).count&&h.submit(10010,200).count==1,"disabled GPU streams do not erase CPU");
    g.enabled=true;device(t,12,10010);h.observe(true,t,g,10010);check(!h.producer(10010,200).count&&!h.transfer(10010,200).count,"re-enable cannot replay consumed disabled sequences");
    device(t,13,10020,EdvrNativeGpuPending);h.observe(true,t,gpu(12,10020),10020);device(t,13,10020);h.observe(true,t,gpu(12,10020),10020);
    check(h.transfer(10020,200).count==1,"unconsumed pending device result resolves on same sequence");
    t.haveCpu=t.haveDeviceGpu=false;g={};h.observe(true,t,g,10020);
    check(!h.submit(10020,200).count&&!h.producer(10020,200).count&&!h.transfer(10020,200).count,"missing snapshots clear affected streams");
    t=cpu(14,10030);t.generation=8;t.firstSequence=14;h.observe(true,t,gpu(13,10030),10030);
    check(h.submit(10030,200).count==1&&!h.producer(10030,200).count,"new session rejects prior-session GPU");
    h.observe(false,t,gpu(14,10030),10030);check(!h.submit(10030,200).count&&!h.predictedPeriod(10030),"native off retires everything");
    h.observe(true,t,gpu(14,10030),10030);t.active=false;h.observe(true,t,g,10030);check(!h.submit(10030,200).count,"provider close retires history");
}
void agesAndValues() {
    NativePerfHistory h;auto t=cpu(1,10000);auto g=gpu(1,10000,150);device(t,1,9900);h.observe(true,t,g,10000);
    check(!h.producer(10051,200).count&&h.submit(10051,200).count==1&&h.transfer(10051,200).count==1,"producer age accrued before publication is included");
    float graph[4]{};check(h.graph(true,graph,4,11851)==0,"GPU history vanishes when effective newest sample is stale");
    for(unsigned kind=0;kind<4;++kind){
        h.clear();t=cpu(1,10000);g=gpu(1,10000);
        if(kind==0)g.capturedAtMs=0;if(kind==1)g.capturedAtMs=10001;if(kind==2)g.result.ageMs=UINT64_MAX;if(kind==3)g.result.ageMs=2001;
        h.observe(true,t,g,10000);check(!h.producer(10000,10000).count,"invalid/missing/future/overflow producer timestamp rejected");
    }
    for(unsigned kind=0;kind<6;++kind){
        h.clear();t=cpu(1,10000);device(t,1,10000);
        if(kind==0)t.capturedAtMs=0;if(kind==1)t.capturedAtMs=10001;if(kind==2)t.waitMs=NAN;
        if(kind==3)t.cpu.submitMs[0]=-1;if(kind==4)t.cpu.submitMs[0]=INFINITY;if(kind==5)t.cpu.submitMs[0]=600001;
        h.observe(true,t,gpu(1,10000),10000);check(!h.submit(10000,200).count&&h.producer(10000,200).count==1,"invalid CPU state rejects only CPU history");
    }
    for(unsigned kind=0;kind<5;++kind){
        h.clear();t=cpu(1,10000);device(t,1,10000);
        if(kind==0)t.deviceGpu.completedAtMs=0;if(kind==1)t.deviceGpu.completedAtMs=10001;
        if(kind==2)t.deviceGpu.transferMs[0]=NAN;if(kind==3)t.deviceGpu.composeMs[1]=-1;if(kind==4)t.deviceGpu.status=EdvrNativeGpuQueryFailure;
        h.observe(true,t,gpu(1,10000),10000);check(!h.transfer(10000,200).count&&h.submit(10000,200).count==1,"invalid device result does not erase CPU");
    }
    h.clear();t=cpu(1,10000);h.observe(true,t,gpu(1,10000),10000);
    t=cpu(2,10010);t.predictedPeriodMs=NAN;h.observe(true,t,gpu(2,10010),10010);
    check(h.submit(10010,200).count==2&&!h.predictedPeriod(10010),"invalid new prediction replaces old prediction without losing CPU samples");
    t=cpu(3,10020);t.predictedPeriodMs=0;h.observe(true,t,gpu(3,10020),10020);check(!h.predictedPeriod(10020),"zero period is unavailable");
}
void graphs() {
    NativePerfHistory h;for(uint64_t i=1;i<=950;++i){auto t=cpu(i,10000+i);t.cpu.submitMs[0]=double(i);t.cpu.submitMs[1]=0;h.observe(true,t,gpu(i,10000+i,0,double(i)+100),10000+i);}
    check(h.submit(10950,10000).count==900&&approx(h.submit(10950,10000).meanMs,500.5),"ring wraps without overweighting or losing ordering");
    float values[5]={-99,-99,-99,-99,-99};check(h.graph(false,values,3,10950)==3&&values[0]==948&&values[1]==949&&values[2]==950&&values[3]==-99,"CPU graph uses newest bounded tail oldest-first");
    check(h.graph(true,values,3,10950)==3&&values[0]==1048&&values[2]==1050,"producer graph uses its own observations");
    check(h.graph(true,nullptr,3,10950)==0&&h.graph(true,values,0,10950)==0&&h.graph(true,values,-1,10950)==0,"invalid graph buffers/capacities rejected");
    check(!h.graph(true,values,3,12951),"stale graph never continues to present old results as live");
    check(!h.graph(false,values,3,9000),"time reversal never exposes future observations");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 2;
    if(!std::wcscmp(argv[1],L"--dry-run")){std::puts("native_perf_history_test: dry-run (no runtime, device or files)");return 0;}
    if(std::wcscmp(argv[1],L"--self-test"))return 2;
    averages();invalidation();agesAndValues();graphs();
    std::printf("native_perf_history_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
