#include "native_perf_history.h"
#include <algorithm>
#include <cmath>

namespace edvr {
bool NativePerfHistory::duration(double v) noexcept { return std::isfinite(v) && v>=0 && v<=600000; }
bool NativePerfHistory::age(uint64_t now,uint64_t at,uint64_t window) noexcept { return at && at<=now && now-at<=window; }
void NativePerfHistory::Stream::reset(uint64_t consumed) noexcept {
    count=next=0; floor=(std::max)(floor,consumed);
}
void NativePerfHistory::Stream::add(uint64_t sequence,uint64_t stamp,double a,double b) noexcept {
    if(sequence<=floor)return;
    entries[next]={stamp,{a,b}};next=(next+1)%kCapacity;
    if(count<kCapacity)++count;
    floor=sequence;
}
const NativePerfHistory::Entry& NativePerfHistory::Stream::at(unsigned index) const noexcept {
    return entries[(next+kCapacity-count+index)%kCapacity];
}
bool NativePerfHistory::Stream::fresh(uint64_t now) const noexcept {
    return count && age(now,at(count-1).at,2000);
}
NativePerfAverage NativePerfHistory::Stream::average(unsigned field,uint64_t now,uint64_t window) const noexcept {
    NativePerfAverage result{};
    if(!fresh(now))return result;
    double sum=0;
    for(unsigned i=0;i<count;++i)if(age(now,at(i).at,window)) {sum+=at(i).values[field];++result.count;}
    if(result.count)result.meanMs=sum/result.count;
    return result;
}
int NativePerfHistory::Stream::graph(float* out,int max,uint64_t now) const noexcept {
    if(!out||max<=0||!fresh(now))return 0;
    unsigned eligible=0;
    for(unsigned i=0;i<count;++i)if(age(now,at(i).at,10000))++eligible;
    const unsigned limit=(std::min)(eligible,static_cast<unsigned>(max));
    unsigned skip=eligible-limit,n=0;
    for(unsigned i=0;i<count;++i)if(age(now,at(i).at,10000)) {
        if(skip){--skip;continue;}
        out[n++]=static_cast<float>(at(i).values[0]);
    }
    return static_cast<int>(n);
}
void NativePerfHistory::clear() noexcept {
    cpu_.reset(); applicationCpu_.reset(); producer_.reset(); applicationGpu_.reset(); device_.reset();
    cpu_.floor=applicationCpu_.floor=producer_.floor=applicationGpu_.floor=device_.floor=0;
    generation_=firstSequence_=periodAt_=0;period_=0;
    baseHz_=0;baseHzAt_=0;firstPeriodMs_=0;
    cpuSource_=NativeCpuSource::None;
}
NativeCpuFigure NativePerfHistory::cpuFigure(const NativeTimingSnapshot& t) noexcept {
    NativeCpuFigure f{};
    if(!t.haveCpu)return f;
    if(t.cpu.version>=EDVR_NATIVE_TIMING_VERSION_5) {
        f.source=NativeCpuSource::CallerWork;
        f.valid=t.cpu.callerWorkValid==1&&duration(t.cpu.callerWorkMs);
        f.ms=f.valid?t.cpu.callerWorkMs:0;
    } else {
        f.source=NativeCpuSource::PreSubmit;
        f.valid=t.applicationValid&&duration(t.applicationMs);
        f.ms=f.valid?t.applicationMs:0;
    }
    return f;
}
void NativePerfHistory::observe(bool native,const NativeTimingSnapshot& t,const GpuFrameSnapshot& g,uint64_t now) noexcept {
    if(!native||!t.active||!t.generation||!t.firstSequence){clear();return;}
    if(generation_!=t.generation||firstSequence_!=t.firstSequence) {
        clear();generation_=t.generation;firstSequence_=t.firstSequence;
    }
    if(t.invalid) {
        cpu_.reset((std::max)(t.sequence,t.cpu.sequence));
        applicationCpu_.reset((std::max)(t.sequence,t.cpu.sequence));
        producer_.reset(g.haveResult?g.result.sequence:0);
        applicationGpu_.reset(g.haveResult?g.result.sequence:0);
        device_.reset(t.haveDeviceGpu?t.deviceGpu.sequence:0);
        period_=0;periodAt_=0;baseHz_=0;baseHzAt_=0;return;
    }
    const auto& c=t.cpu;
    bool cpuOk=t.haveCpu&&c.sequence>=firstSequence_&&c.sequence==t.sequence&&age(now,t.capturedAtMs,2000)&&duration(t.waitMs)&&duration(c.composeMs);
    for(unsigned eye=0;eye<2;++eye)cpuOk=cpuOk&&duration(c.submitMs[eye])&&duration(c.temporalMs[eye])&&duration(c.menuMs[eye])&&duration(c.transferMs[eye]);
    const double submitMs=c.submitMs[0]+c.submitMs[1];
    cpuOk=cpuOk&&duration(submitMs);
    if(!cpuOk) {
        cpu_.reset(t.haveCpu?c.sequence:0);period_=0;periodAt_=0;baseHz_=0;baseHzAt_=0;
    } else if(c.sequence>cpu_.floor) {
        cpu_.add(c.sequence,t.capturedAtMs,submitMs,t.waitMs);
        period_=std::isfinite(t.predictedPeriodMs)&&t.predictedPeriodMs>0&&t.predictedPeriodMs<=10000?t.predictedPeriodMs:0;
        periodAt_=period_?t.capturedAtMs:0;
        // The base for the throttle flag: the panel rate the ABI publishes
        // (display_frequency) while fresh; when a version 3 host never sends
        // one, the session's first predicted period stands in for it.
        baseHz_=c.version>=EDVR_NATIVE_TIMING_VERSION_4&&std::isfinite(c.baseDisplayHz)&&c.baseDisplayHz>0&&c.baseDisplayHz<=1000?double(c.baseDisplayHz):0;
        baseHzAt_=baseHz_?t.capturedAtMs:0;
        if(!firstPeriodMs_&&period_)firstPeriodMs_=period_;
    }
    // The CPU figure (cpuFigure): the caller work per cycle, or an older
    // runtime's pre-submit application time. A frame without one clears the
    // stream as an invalid application time always did; a change of figure
    // starts it over, so one average is never of both.
    const NativeCpuFigure figure = cpuFigure(t);
    const bool applicationCpuOk = figure.valid &&
        c.sequence >= firstSequence_ && c.sequence == t.sequence &&
        age(now, t.capturedAtMs, 2000);
    if (!applicationCpuOk) {
        applicationCpu_.reset(t.haveCpu ? c.sequence : 0);
    } else if (c.sequence > applicationCpu_.floor) {
        if (figure.source != cpuSource_) {
            applicationCpu_.reset();
            cpuSource_ = figure.source;
        }
        applicationCpu_.add(c.sequence, t.capturedAtMs, figure.ms);
    }
    // Include the age accumulated before publication; subtraction guards keep
    // old or malformed timestamps from becoming fresh through integer wrap.
    const bool stampOk=g.capturedAtMs&&g.capturedAtMs<=now&&g.result.ageMs<g.capturedAtMs;
    const uint64_t gpuAt=stampOk?g.capturedAtMs-g.result.ageMs:0;
    const bool gpuOk=g.enabled&&g.haveResult&&g.result.reason==GpuSpanReason::Valid&&g.result.sequence>=firstSequence_&&age(now,gpuAt,2000)&&duration(g.result.outerMs);
    if(!gpuOk)producer_.reset(g.haveResult?g.result.sequence:0);
    else producer_.add(g.result.sequence,gpuAt,g.result.outerMs);
    const bool applicationGpuOk = g.enabled && g.haveResult &&
        g.result.source == GpuSpanSource::ApplicationRender &&
        g.result.reason == GpuSpanReason::Valid &&
        g.result.sequence >= firstSequence_ && age(now, gpuAt, 2000) &&
        duration(g.result.outerMs);
    if (!applicationGpuOk) applicationGpu_.reset(g.haveResult ? g.result.sequence : 0);
    else applicationGpu_.add(g.result.sequence, gpuAt, g.result.outerMs);
    const auto& d=t.deviceGpu;
    bool deviceOk=t.haveDeviceGpu&&d.sequence>=firstSequence_&&d.status==EdvrNativeGpuValid&&age(now,d.completedAtMs,2000);
    for(unsigned eye=0;eye<2;++eye)deviceOk=deviceOk&&duration(d.transferMs[eye])&&duration(d.composeMs[eye]);
    const double copy=d.transferMs[0]+d.transferMs[1],compose=d.composeMs[0]+d.composeMs[1];
    deviceOk=deviceOk&&duration(copy)&&duration(compose);
    if(!deviceOk) {
        // Pending can resolve valid for this same sequence; terminal refusal
        // consumes it, and an already consumed result can never replay.
        const uint64_t consumed=t.haveDeviceGpu&&d.status!=EdvrNativeGpuPending?d.sequence:0;
        device_.reset(consumed);
    } else device_.add(d.sequence,d.completedAtMs,copy,compose);
}
NativePerfAverage NativePerfHistory::submit(uint64_t n,uint64_t w)const noexcept{return cpu_.average(0,n,w);}
NativePerfAverage NativePerfHistory::applicationCpu(uint64_t n,uint64_t w)const noexcept{return applicationCpu_.average(0,n,w);}
NativePerfAverage NativePerfHistory::applicationGpu(uint64_t n,uint64_t w)const noexcept{return applicationGpu_.average(0,n,w);}
NativePerfAverage NativePerfHistory::wait(uint64_t n,uint64_t w)const noexcept{return cpu_.average(1,n,w);}
NativePerfAverage NativePerfHistory::producer(uint64_t n,uint64_t w)const noexcept{return producer_.average(0,n,w);}
NativePerfAverage NativePerfHistory::transfer(uint64_t n,uint64_t w)const noexcept{return device_.average(0,n,w);}
NativePerfAverage NativePerfHistory::compose(uint64_t n,uint64_t w)const noexcept{return device_.average(1,n,w);}
double NativePerfHistory::predictedPeriod(uint64_t n)const noexcept{return age(n,periodAt_,2000)?period_:0;}
double NativePerfHistory::basePeriodMs(uint64_t n)const noexcept{
    if(age(n,baseHzAt_,2000)&&baseHz_>0)return 1000.0/baseHz_;
    return firstPeriodMs_;
}
bool NativePerfHistory::displayThrottled(double predicted,double base)noexcept{
    return predicted>0&&base>0&&predicted>1.5*base;
}
int NativePerfHistory::graph(bool p,float* o,int m,uint64_t n)const noexcept{return (p?applicationGpu_:applicationCpu_).graph(o,m,n);}
}
