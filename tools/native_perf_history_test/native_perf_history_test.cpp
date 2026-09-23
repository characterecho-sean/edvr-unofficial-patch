#include "../../src/d3d11/native_perf_history.h"
#include "../../src/d3d11/native_benchmark_collector.h"
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
    s.applicationMs=3;s.applicationValid=true;
    s.cpu={sizeof(s.cpu),EDVR_NATIVE_TIMING_VERSION_5,seq};s.cpu.submitMs[0]=1;s.cpu.submitMs[1]=2;s.cpu.baseDisplayHz=90;
    s.cpu.callerWorkMs=14.4;s.cpu.callerWorkValid=1;   // the caller work per cycle: the monitor's CPU on a version 5 frame
    return s;
}
GpuFrameSnapshot gpu(uint64_t seq,uint64_t at,uint64_t age=0,double ms=5) {
    GpuFrameSnapshot s{};s.enabled=s.haveResult=true;s.capturedAtMs=at;
    s.result.sequence=seq;s.result.ageMs=age;s.result.outerMs=ms;
    s.result.source=GpuSpanSource::ApplicationRender;s.result.reason=GpuSpanReason::Valid;return s;
}
void device(NativeTimingSnapshot& s,uint64_t seq,uint64_t at,uint32_t status=EdvrNativeGpuValid) {
    s.haveDeviceGpu=true;s.deviceGpu={sizeof(s.deviceGpu),EDVR_NATIVE_TIMING_VERSION_3,seq,at,status,{1,2},{3,4}};
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
    auto partial=t;partial.cpu.callerWorkValid=0;partial.cpu.callerWorkMs=0;h.observe(true,partial,g,10000);
    check(!h.applicationCpu(10000,200).count&&h.applicationGpu(10000,200).count==1&&
          h.submit(10000,200).count==1,"incomplete CPU figure (no caller work) does not substitute submit wall or erase GPU");
    h.clear();auto oldDefinition=g;oldDefinition.result.source=GpuSpanSource::RenderToSubmit;
    h.observe(true,t,oldDefinition,10000);
    check(h.applicationCpu(10000,200).count==1&&!h.applicationGpu(10000,200).count,
          "legacy GPU interval cannot become an application benchmark measurement");
    h.clear();h.observe(true,t,g,10000);
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
void displayBase() {
    NativePerfHistory h;
    // The spec cases: 22.222 ms predicted against a 90 Hz base is a 45 Hz
    // display rate and THROTTLED; 11.111 ms is 90 Hz, no flag.
    auto t=cpu(1,10000);t.predictedPeriodMs=22.222;auto g=gpu(1,10000);
    h.observe(true,t,g,10000);
    const double predicted=h.predictedPeriod(10000);
    const double base=h.basePeriodMs(10000);
    check(predicted>22.0&&predicted<22.3&&base>11.0&&base<11.2&&
          NativePerfHistory::displayThrottled(predicted,base),
          "predicted 22.222 ms with base 90 Hz reads a 45 Hz display and THROTTLED");
    t=cpu(2,10010);t.predictedPeriodMs=11.111;h.observe(true,t,gpu(2,10010),10010);
    check(!NativePerfHistory::displayThrottled(h.predictedPeriod(10010),h.basePeriodMs(10010)),
          "predicted 11.111 ms with base 90 Hz shows no flag");
    // A version 3 host never sends baseDisplayHz: the session's first
    // predicted period stands in as the base.
    h.clear();t=cpu(3,10020);t.cpu.version=EDVR_NATIVE_TIMING_VERSION_3;t.cpu.baseDisplayHz=0;t.predictedPeriodMs=11.111;
    h.observe(true,t,gpu(3,10020),10020);
    t=cpu(4,10030);t.cpu.version=EDVR_NATIVE_TIMING_VERSION_3;t.cpu.baseDisplayHz=0;t.predictedPeriodMs=22.222;
    h.observe(true,t,gpu(4,10030),10030);
    check(NativePerfHistory::displayThrottled(h.predictedPeriod(10030),h.basePeriodMs(10030)),
          "version 3 host falls back to the session's first predicted period");
    // The published base is ring-fresh like the prediction: past 2 s the
    // first-period fallback answers, and a new session resets both.
    h.clear();t=cpu(5,10040);t.predictedPeriodMs=12;h.observe(true,t,gpu(5,10040),10040);
    check(h.basePeriodMs(10040)>11.0&&h.basePeriodMs(10040)<11.2&&approx(h.basePeriodMs(12041),12),
          "stale published base falls back to the first predicted period");
    t=cpu(6,10050);t.generation=9;t.firstSequence=6;t.predictedPeriodMs=17;
    h.observe(true,t,gpu(6,10050),10050);
    check(NativePerfHistory::displayThrottled(h.predictedPeriod(10050),h.basePeriodMs(10050)),
          "new session resets the base to the new session's prediction");
}
// The monitor's CPU figure (NativePerfHistory::cpuFigure, the settlement
// governor's rule): on a version 5 frame the caller work per cycle -- the
// game's thread per frame, which on the 2026-09-23 flights read 11.7-12.7 ms
// while the pre-submit application time beside it read under 10 -- labelled
// as the plain CPU (cpuSource CallerWork); a version 5 frame without valid
// caller work has no figure, never the application time beside it. An older
// runtime (version 3 or 4: no caller work) falls back to the application
// time, labelled pre-submit, and no average ever holds both.
void callerWorkCrossing() {
    NativePerfHistory h;
    check(h.cpuSource()==NativeCpuSource::None,"no frame yet: the CPU figure has no source");
    auto t=cpu(1,10000);   // version 5: caller work 14.4 ms beside app 3 ms
    h.observe(true,t,gpu(1,10000),10000);
    check(t.cpu.version==EDVR_NATIVE_TIMING_VERSION_5&&t.cpu.callerWorkValid==1&&
          h.applicationCpu(10000,200).count==1&&approx(h.applicationCpu(10000,200).meanMs,14.4)&&
          h.cpuSource()==NativeCpuSource::CallerWork,
          "version 5: the monitor's CPU is the caller work per cycle (14.4), not the pre-submit app time (3)");
    float graph[2]{};
    check(h.graph(false,graph,2,10000)==1&&graph[0]==14.4f,"version 5: the CPU graph plots the caller work");
    check(h.basePeriodMs(10000)>11.0&&h.basePeriodMs(10000)<11.2,
          "version 5: the base display rate still reads (version 4 and later)");
    t=cpu(2,10010);t.cpu.callerWorkValid=0;t.cpu.callerWorkMs=0;
    h.observe(true,t,gpu(2,10010),10010);
    check(!h.applicationCpu(10010,200).count&&h.cpuSource()==NativeCpuSource::CallerWork,
          "version 5 without valid caller work: no CPU figure, never the app time beside it");
    t=cpu(3,10020);t.cpu.callerWorkMs=12.2;h.observe(true,t,gpu(3,10020),10020);
    check(h.applicationCpu(10020,200).count==1&&approx(h.applicationCpu(10020,200).meanMs,12.2),
          "version 5: the next frame with caller work is the figure again");
    // An older runtime (a new session, version 4, no caller work): the
    // pre-submit application time stands in, and says so.
    t=cpu(4,10030);t.generation=8;t.firstSequence=4;
    t.cpu.version=EDVR_NATIVE_TIMING_VERSION_4;t.cpu.size=EDVR_NATIVE_TIMING_FRAME_SIZE_4;
    t.cpu.callerWorkMs=0;t.cpu.callerWorkValid=0;
    h.observe(true,t,gpu(4,10030),10030);
    check(h.applicationCpu(10030,200).count==1&&approx(h.applicationCpu(10030,200).meanMs,3)&&
          h.cpuSource()==NativeCpuSource::PreSubmit&&h.basePeriodMs(10030)>11.0&&h.basePeriodMs(10030)<11.2,
          "version 4 (no caller work): the CPU falls back to applicationMs, labelled pre-submit; the base rate reads");
    t=cpu(5,10040);t.generation=8;t.firstSequence=4;t.cpu.version=EDVR_NATIVE_TIMING_VERSION_4;
    t.cpu.size=EDVR_NATIVE_TIMING_FRAME_SIZE_4;t.cpu.callerWorkMs=0;t.cpu.callerWorkValid=0;t.applicationValid=false;
    h.observe(true,t,gpu(5,10040),10040);
    check(!h.applicationCpu(10040,200).count,"version 4 without app time: no CPU figure");
    // Never one average of both: a version 5 frame after version 4 samples in
    // the same session starts the stream over.
    t=cpu(6,10050);t.generation=8;t.firstSequence=4;t.applicationMs=3;
    h.observe(true,t,gpu(6,10050),10050);
    auto older=cpu(7,10060);older.generation=8;older.firstSequence=4;older.cpu.version=EDVR_NATIVE_TIMING_VERSION_4;
    older.cpu.size=EDVR_NATIVE_TIMING_FRAME_SIZE_4;older.cpu.callerWorkMs=0;older.cpu.callerWorkValid=0;
    h.observe(true,older,gpu(7,10060),10060);
    check(h.applicationCpu(10060,200).count==1&&approx(h.applicationCpu(10060,200).meanMs,3)&&
          h.cpuSource()==NativeCpuSource::PreSubmit,"a change of figure starts the average over");
    // The rule itself, frame by frame.
    NativeTimingSnapshot f=cpu(8,10070);
    NativeCpuFigure x=NativePerfHistory::cpuFigure(f);
    check(x.valid&&x.source==NativeCpuSource::CallerWork&&approx(x.ms,14.4),"rule: version 5 with caller work");
    f.cpu.callerWorkValid=0;x=NativePerfHistory::cpuFigure(f);
    check(!x.valid&&x.source==NativeCpuSource::CallerWork,"rule: version 5 without it has no figure");
    f=cpu(9,10080);f.cpu.callerWorkMs=NAN;x=NativePerfHistory::cpuFigure(f);
    check(!x.valid,"rule: a NaN caller work is no figure");
    f=cpu(10,10090);f.cpu.version=EDVR_NATIVE_TIMING_VERSION_3;f.cpu.baseDisplayHz=0;x=NativePerfHistory::cpuFigure(f);
    check(x.valid&&x.source==NativeCpuSource::PreSubmit&&approx(x.ms,3),"rule: version 3 falls back to applicationMs");
    f.haveCpu=false;x=NativePerfHistory::cpuFigure(f);
    check(!x.valid&&x.source==NativeCpuSource::None,"rule: no CPU frame, no figure");
    h.clear();
    check(h.cpuSource()==NativeCpuSource::None,"clear forgets the figure's source");
}
void graphs() {
    NativePerfHistory h;for(uint64_t i=1;i<=950;++i){auto t=cpu(i,10000+i);t.cpu.submitMs[0]=double(i);t.cpu.submitMs[1]=0;t.cpu.callerWorkMs=double(i);h.observe(true,t,gpu(i,10000+i,0,double(i)+100),10000+i);}
    check(h.submit(10950,10000).count==900&&approx(h.submit(10950,10000).meanMs,500.5),"ring wraps without overweighting or losing ordering");
    float values[5]={-99,-99,-99,-99,-99};check(h.graph(false,values,3,10950)==3&&values[0]==948&&values[1]==949&&values[2]==950&&values[3]==-99,"CPU graph uses newest bounded tail oldest-first");
    check(h.graph(true,values,3,10950)==3&&values[0]==1048&&values[2]==1050,"producer graph uses its own observations");
    check(h.graph(true,nullptr,3,10950)==0&&h.graph(true,values,0,10950)==0&&h.graph(true,values,-1,10950)==0,"invalid graph buffers/capacities rejected");
    check(!h.graph(true,values,3,12951),"stale graph never continues to present old results as live");
    check(!h.graph(false,values,3,9000),"time reversal never exposes future observations");
}

NativeBenchmarkObservation benchmarkSample(uint64_t seq, uint64_t at,
                                           double cpuMs, double gpuMs) {
    NativeBenchmarkObservation s{};
    s.scope = 1;
    s.cpuSequence = seq; s.cpuAtMs = at; s.cpuMs = cpuMs; s.cpuValid = true;
    s.gpuSequence = seq; s.gpuAtMs = at; s.gpuMs = gpuMs; s.gpuValid = true;
    return s;
}

void benchmarkAdmission() {
    NativeBenchmarkCollector c;
    NativeBenchmarkObservation tick{};tick.scope=1;
    c.observe(tick,1000);c.observe(tick,3000);
    check(c.sampling()&&c.collecting(),"metadata-only monitor ticks start a usable sampling window");
    const auto onlyGpu=[](uint64_t seq,uint64_t at,double value) {
        auto s=benchmarkSample(seq,at,0,value);s.cpuSequence=s.cpuAtMs=0;return s;
    };
    const auto onlyCpu=[](uint64_t seq,uint64_t at,double value) {
        auto s=benchmarkSample(seq,at,value,0);s.gpuSequence=s.gpuAtMs=0;return s;
    };
    c.observe(onlyGpu(1,3010,10),3010);
    c.observe(onlyGpu(257,3011,20),3011); // Same hash bucket.
    c.observe(onlyCpu(1,3001,1),3012); // Removing it must not hide 257.
    c.observe(onlyCpu(257,3002,2),3013);
    c.observe(onlyGpu(3,3014,999),3014);
    c.observe(onlyCpu(3,2999,999),3015); // Pre-warmup, completed late.
    auto invalid=onlyCpu(4,3020,NAN);invalid.cpuValid=false;c.observe(invalid,3020);
    c.observe(onlyGpu(4,3021,0),3021); // Valid measured zero remains valid.
    c.observe({},33000);
    check(c.sampling()&&!c.collecting(),"drain accepts completions without claiming active collection");
    c.observe(onlyGpu(5,33500,30),33500); // Completion in drain before CPU arrival.
    c.observe(onlyCpu(5,32999,3),33501);
    c.observe(onlyCpu(6,33000,999),33502); // Half-open sample interval.
    c.observe(onlyCpu(7,32998,4),35000);
    c.observe(onlyGpu(7,34999,40),35000);
    check(!c.ready(),"deadline batch drains before the final monitor tick");
    c.observe({},35000);
    NativeBenchmarkReport r{};
    check(c.takeReport(&r)&&!r.aborted&&r.cpu.valid==4&&r.cpu.invalid==1&&
          r.gpu.valid==5&&r.gpu.missing==0,"colliding and drain orphans retain their own admitted frames");
    check(approx(r.cpu.p50,2)&&approx(r.cpu.p99,4)&&approx(r.gpu.p50,20)&&
          approx(r.gpu.p99,40),"warmup and boundary frames cannot bias either distribution");
    check(r.startedAtMs==3000&&r.sampleEndedAtMs==33000&&r.drainMs==2000,
          "sample and drain durations are distinct");
    c.observe(tick,40000);c.observe(tick,42000);
    c.observe(onlyCpu(100,42001,7),42001);tick.scope=2;c.observe(tick,42500);
    check(c.takeReport(&r)&&r.aborted&&r.sampleEndedAtMs-r.startedAtMs==500&&r.drainMs==0,
          "partial reports state actual duration, not planned thirty seconds");
}
void benchmarkWindows() {
    NativeBenchmarkCollector c;
    c.observe(benchmarkSample(1, 0, 1, 2), 0);
    check(c.warming() && !c.ready(), "benchmark starts in warmup");
    c.observe(benchmarkSample(1, 1999, 1, 2), 1999);
    check(c.warming(), "two-second warmup excludes early samples");
    c.observe(benchmarkSample(1, 2000, 1, 2), 2000);
    c.observe(benchmarkSample(2, 2010, 9, 6), 2010);
    c.observe(benchmarkSample(2, 2011, 9, 6), 2011);
    check(c.sampling(), "window samples after warmup");
    c.observe({}, 32000);
    check(!c.ready(), "window enters a bounded GPU completion drain");
    c.observe({}, 34000);
    check(c.ready(), "elapsed window and drain produce one report");
    NativeBenchmarkReport report{};
    check(c.takeReport(&report) && report.complete && !report.aborted,
          "completed benchmark report is consumable");
    check(report.cpu.valid == 2 && report.cpu.stored == 2 && report.cpu.missing == 0 &&
          approx(report.cpu.p50, 1) && approx(report.cpu.p95, 9) && approx(report.cpu.p99, 9),
          "CPU distribution uses individual values and nearest-rank percentiles");
    check(report.gpu.valid == 2 && report.gpu.stored == 2 && report.gpu.missing == 0 &&
          approx(report.gpu.p50, 2) && approx(report.gpu.p95, 6),
          "GPU distribution remains independent from CPU");
    check(!c.ready() && !c.takeReport(&report), "report is emitted once and next window is idle");

    c.observe(benchmarkSample(10, 40000, 3, 4), 40000);
    c.observe(benchmarkSample(11, 40001, 5, 6), 40001);
    c.observe(benchmarkSample(200, 40002, 7, 8), 40002);
    c.observe(benchmarkSample(12, 42000, 7, 8), 42000);
    c.observe({}, 74000);
    check(c.ready() && c.takeReport(&report), "second recurring window completes");
    check(report.cpu.valid == 1 && report.gpu.valid == 1 && report.cpu.missing == 0,
          "a recurring window resets values without replaying old samples");

    c.observe(benchmarkSample(300, 80000, 1, 1), 80000);
    c.observe(benchmarkSample(300, 82000, 1, 1), 82000);
    NativeBenchmarkObservation invalid = benchmarkSample(301, 82001, 0, 0);
    invalid.cpuValid = false;
    c.observe(invalid, 82001);
    NativeBenchmarkObservation changed = benchmarkSample(1, 82002, 2, 2);
    changed.scope = 2;
    c.observe(changed, 82002);
    check(c.ready(), "scope change emits an aborted partial report");
    check(c.takeReport(&report) && report.aborted &&
          report.abortReason == kNativeBenchmarkScopeChanged,
          "scope change identifies why a window was aborted");
    c.observe(changed, 82003);
    check(c.warming() && !c.ready(), "scope change starts a new warmup");

    c.reset();
    c.observe(benchmarkSample(1, 0, 1, 1), 0);
    c.observe(benchmarkSample(1, 2000, 1, 1), 2000);
    NativeBenchmarkObservation cpuOnly = benchmarkSample(200, 2001, 2, 0);
    cpuOnly.gpuSequence = cpuOnly.gpuAtMs = 0;
    cpuOnly.gpuValid = false;
    c.observe(cpuOnly, 2001);
    c.observe({}, 32000);
    c.observe({}, 34000);
    check(c.takeReport(&report) && report.cpu.missing == 0 && report.gpu.missing == 1,
          "retirement marks a missing asynchronous GPU counterpart");

    c.reset();
    c.observe(benchmarkSample(1, 0, 1, 1), 0);
    c.observe(benchmarkSample(1, 2000, 1, 3), 2000);
    NativeBenchmarkObservation cpuTwo = benchmarkSample(2, 2001, 2, 0);
    cpuTwo.gpuSequence = cpuTwo.gpuAtMs = 0;
    cpuTwo.gpuValid = false;
    c.observe(cpuTwo, 2001);
    c.observe({}, 32000);
    // GPU completion order and completion timestamps are independent from
    // CPU admission. Both belong to admitted window frames and are allowed
    // through the bounded drain even when they arrive out of order or late.
    NativeBenchmarkObservation lateGpu = benchmarkSample(2, 1000, 0, 4);
    lateGpu.cpuSequence = lateGpu.cpuAtMs = 0;
    lateGpu.cpuValid = false;
    c.observe(lateGpu, 33000);
    NativeBenchmarkObservation oldGpu = benchmarkSample(1, 1001, 0, 3);
    oldGpu.cpuSequence = oldGpu.cpuAtMs = 0;
    oldGpu.cpuValid = false;
    c.observe(oldGpu, 33001);
    c.observe({}, 34000);
    check(c.takeReport(&report) && report.cpu.valid == 2 && report.gpu.valid == 2 &&
          report.gpu.missing == 0 && approx(report.gpu.p50, 3) && approx(report.gpu.p95, 4),
          "late out-of-order GPU completions attach to their own CPU frame");

    c.reset();
    c.observe(benchmarkSample(1, 0, 1, 1), 0);
    c.observe(benchmarkSample(1, 2000, 1, 1), 2000);
    c.noteDropped(true, 2);
    c.noteDropped(false, 3);
    c.observe({}, 32000);
    c.observe({}, 34000);
    check(c.takeReport(&report) && report.aborted &&
          report.abortReason == kNativeBenchmarkTransportLoss &&
          report.cpu.invalid == 2 && report.gpu.invalid == 3,
          "completion queue overwrite is reported as invalid coverage");

    c.reset();
    c.observe(benchmarkSample(1, 0, 1, 1), 0);
    c.observe(benchmarkSample(1, 2000, 1, 1), 2000);
    c.observe({}, 1999);
    check(c.ready() && c.takeReport(&report) && report.aborted &&
          report.abortReason == kNativeBenchmarkClockReversed,
          "clock reversal aborts an active benchmark window");

    c.reset();
    c.observe(benchmarkSample(1, 0, 1, 1), 0);
    c.observe(benchmarkSample(1, 2000, 1, 1), 2000);
    for (unsigned i = 0; i <= NativeBenchmarkCollector::kCapacity; ++i) {
        c.observe(benchmarkSample(100 + i, 2001 + i, 1, 1), 2001 + i);
        if (c.ready()) break;
    }
    check(c.ready() && c.takeReport(&report) && report.aborted && report.overflow,
          "storage overflow aborts instead of silently truncating samples");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 2;
    if(!std::wcscmp(argv[1],L"--dry-run")){std::puts("native_perf_history_test: dry-run (no runtime, device or files)");return 0;}
    if(std::wcscmp(argv[1],L"--self-test"))return 2;
    averages();invalidation();agesAndValues();displayBase();callerWorkCrossing();graphs();benchmarkWindows();benchmarkAdmission();
    std::printf("native_perf_history_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
