#include "device_gpu_timing.h"
#include <cmath>
#include <new>

namespace edvr::openxr {
namespace {
constexpr unsigned kAllPhases = 0xFu;
constexpr uint64_t kMaxAgeMs = 2000;
constexpr double kMaxFieldMs = 600000.0;
uint64_t nowMs() noexcept { return GetTickCount64(); }
}
DeviceGpuTiming::~DeviceGpuTiming() { abandon(); }

bool DeviceGpuTiming::initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                                 const edvr::GpuSpanD3D11Ops& ops) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_ || abandoned_ || !device || !context) return false;
    auto candidate = std::unique_ptr<edvr::GpuSpanD3D11Driver>(new (std::nothrow)
        edvr::GpuSpanD3D11Driver(device, context, ops, 8));
    if (!candidate || !candidate->isBound()) return false;
    context_ = context; thread_ = GetCurrentThreadId(); driver_ = std::move(candidate);
    initialized_ = true;
    return true;
}
bool DeviceGpuTiming::owner(ID3D11DeviceContext* context) const noexcept {
    return initialized_ && !abandoned_ && context == context_ && GetCurrentThreadId() == thread_;
}
void DeviceGpuTiming::retireFrameLocked(bool all) noexcept {
    for (unsigned i=0; i<kSlots; ++i) {
        const auto& s=slots_[i];
        if (s.queryOpen || (s.pending && (all || !s.accepted))) retireMask_ |= 1u<<i;
    }
    active_=-1;
}
void DeviceGpuTiming::retireLocked() noexcept {
    // Only reached at a GPU-owner boundary. An uncertain End is never retried.
    for (unsigned i=0; i<kSlots; ++i) if (retireMask_ & (1u<<i)) {
        auto& s=slots_[i];
        if (s.queryOpen) {
            if (!driver_->end(i)) stopped_=true;
            s.queryOpen=false;
        }
        driver_->destroy(i); s={};
    }
    retireMask_=0;
}
void DeviceGpuTiming::failCurrentLocked(uint32_t status) noexcept {
    if (active_>=0) {
        retireMask_ |= 1u<<unsigned(active_); active_=-1;
        retireLocked();
    }
    frameStarted_=true; frameStatus_=stopped_?EdvrNativeGpuQueryFailure:status;
    statusAtMs_=nowMs(); statusPending_=true;
}
bool DeviceGpuTiming::beginFrame(uint64_t sequence, bool enabled) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || abandoned_ || !sequence || sequence<=lastSequence_) return false;
    retireFrameLocked(!enabled);
    sequence_=lastSequence_=sequence; enabled_=enabled;
    frameStarted_=frameAccepted_=false;
    frameStatus_=!enabled?EdvrNativeGpuDisabled:(stopped_?EdvrNativeGpuQueryFailure:EdvrNativeGpuPending);
    statusPending_=frameStatus_!=EdvrNativeGpuPending; statusAtMs_=nowMs();
    return true;
}
bool DeviceGpuTiming::acceptFrame(uint64_t sequence) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || abandoned_ || !sequence || sequence!=sequence_ || frameAccepted_) return false;
    frameAccepted_=true;
    if (active_>=0) {
        retireFrameLocked(false); // defer incomplete-query closure to a GPU boundary
        frameStatus_=EdvrNativeGpuIncomplete; statusPending_=true; statusAtMs_=nowMs();
    }
    for (auto& s:slots_) if (s.pending && s.sequence==sequence) { s.accepted=true; return true; }
    if (!statusPending_) {
        frameStatus_=EdvrNativeGpuIncomplete; statusPending_=true; statusAtMs_=nowMs();
    }
    return true;
}
void DeviceGpuTiming::invalidate() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || abandoned_) return;
    retireFrameLocked(true);
    sequence_=0; statusPending_=false; frameAccepted_=false;
}
void DeviceGpuTiming::beginGpuWork(unsigned phase, ID3D11DeviceContext* context) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!owner(context)) return;
    retireLocked();
    if (!sequence_ || frameAccepted_ || !enabled_) return;
    if (stopped_) { failCurrentLocked(EdvrNativeGpuQueryFailure); return; }
    if (phase>3) { failCurrentLocked(EdvrNativeGpuIncomplete); return; }
    if (active_<0) {
        if (frameStarted_) {
            // No second scope in one frame, including duplicate work after closure.
            if (!statusPending_) {
                retireFrameLocked(false);
                failCurrentLocked(EdvrNativeGpuIncomplete);
            }
            return;
        }
        frameStarted_=true;
        if (phase>1) { failCurrentLocked(EdvrNativeGpuIncomplete); return; }
        unsigned index=0;
        while (index<kSlots && (slots_[index].pending || (retireMask_&(1u<<index)))) ++index;
        if (index==kSlots) { failCurrentLocked(EdvrNativeGpuIncomplete); return; }
        auto& s=slots_[index];
        if (!s.allocated) {
            if (!driver_->create(index)) { failCurrentLocked(EdvrNativeGpuQueryFailure); return; }
            s.allocated=true;
        }
        if (!driver_->begin(index)) {
            driver_->destroy(index); s={}; failCurrentLocked(EdvrNativeGpuQueryFailure); return;
        }
        s.sequence=sequence_; s.queryOpen=true; active_=static_cast<int>(index);
    }
    auto& s=slots_[active_];
    if (s.sequence!=sequence_ || s.openPhase>=0 || (s.phaseMask&(1u<<phase)) ||
        (phase>=2 && (s.phaseMask&3u)!=3u)) { failCurrentLocked(EdvrNativeGpuIncomplete); return; }
    if (!driver_->timestamp(unsigned(active_),phase*2)) { failCurrentLocked(EdvrNativeGpuQueryFailure); return; }
    s.openPhase=static_cast<int>(phase);
}
void DeviceGpuTiming::endGpuWork(unsigned phase, ID3D11DeviceContext* context) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!owner(context) || !enabled_ || !sequence_ || frameAccepted_) return;
    if (active_<0) return; // the corresponding begin may have skipped instrumentation
    auto& s=slots_[active_];
    if (phase>3 || s.openPhase!=static_cast<int>(phase)) { failCurrentLocked(EdvrNativeGpuIncomplete); return; }
    if (!driver_->timestamp(unsigned(active_),phase*2+1)) { failCurrentLocked(EdvrNativeGpuQueryFailure); return; }
    s.phaseMask|=1u<<phase; s.order[s.phaseCount++]=phase; s.openPhase=-1;
    if (s.phaseMask==kAllPhases) {
        const bool closed=driver_->end(unsigned(active_)); s.queryOpen=false;
        if (!closed) { stopped_=true; failCurrentLocked(EdvrNativeGpuQueryFailure); return; }
        s.pending=true; s.completedAtMs=nowMs(); active_=-1;
    }
}
unsigned DeviceGpuTiming::poll(EdvrNativeDeviceGpuSample* out, unsigned capacity) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!owner(context_) || !out || !capacity) return 0;
    retireLocked();
    unsigned count=0;
    while (count<capacity) {
        int index=-1;
        for (unsigned i=0; i<kSlots; ++i) if (slots_[i].pending && slots_[i].accepted &&
            (index<0 || slots_[i].sequence<slots_[index].sequence)) index=static_cast<int>(i);
        if (index<0) break;
        auto& s=slots_[index];
        if (s.sequence<=lastPublishedSequence_) { driver_->destroy(unsigned(index)); s={}; continue; }
        EdvrNativeDeviceGpuSample sample{sizeof(sample),EDVR_NATIVE_TIMING_VERSION_3,
            s.sequence,s.completedAtMs,EdvrNativeGpuValid};
        const uint64_t now=nowMs();
        GpuSpanRawSample raw{};
        bool reusable=false;
        if (now<s.completedAtMs || now-s.completedAtMs>kMaxAgeMs) sample.status=EdvrNativeGpuStale;
        else if (stopped_) sample.status=EdvrNativeGpuQueryFailure;
        else {
            const auto result=driver_->poll(unsigned(index),raw);
            if (result==GpuSpanPoll::Pending) break; // do not eclipse earlier asynchronous work
            reusable=result==GpuSpanPoll::Ready;
            if (!reusable || !raw.timestampsReady || raw.disjoint || !raw.frequency)
                sample.status=EdvrNativeGpuQueryFailure;
            else {
                uint64_t previous=0;
                for (unsigned n=0; n<4; ++n) {
                    const unsigned p=s.order[n];
                    const auto begin=raw.ticks[p*2],end=raw.ticks[p*2+1];
                    if (end<begin || (n && begin<previous)) { sample.status=EdvrNativeGpuQueryFailure; break; }
                    previous=end;
                    const double ms=double(end-begin)*1000.0/double(raw.frequency);
                    if (!std::isfinite(ms) || ms>kMaxFieldMs) { sample.status=EdvrNativeGpuQueryFailure; break; }
                    if(p<2) sample.transferMs[p]=ms; else sample.composeMs[p-2]=ms;
                }
            }
        }
        if (sample.status!=EdvrNativeGpuValid) {
            for (double& value:sample.transferMs) value=0;
            for (double& value:sample.composeMs) value=0;
        }
        out[count++]=sample; lastPublishedSequence_=sample.sequence;
        if (!reusable) driver_->destroy(unsigned(index));
        s={}; s.allocated=reusable;
    }
    if (count<capacity && statusPending_ && frameAccepted_ && sequence_>lastPublishedSequence_) {
        // A newer failure supersedes pending older work. Retire that work so it
        // cannot resurrect a valid reading after this terminal status.
        for (unsigned i=0; i<kSlots; ++i)
            if (slots_[i].pending && slots_[i].sequence<sequence_) retireMask_|=1u<<i;
        retireLocked();
        out[count++]={sizeof(EdvrNativeDeviceGpuSample),EDVR_NATIVE_TIMING_VERSION_3,
            sequence_,statusAtMs_,frameStatus_};
        lastPublishedSequence_=sequence_; statusPending_=false;
    }
    return count;
}
void DeviceGpuTiming::abandon() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (abandoned_) return;
    abandoned_=true; initialized_=false; enabled_=false; active_=-1; sequence_=0;
    driver_.reset(); context_=nullptr; slots_={};
}
}
