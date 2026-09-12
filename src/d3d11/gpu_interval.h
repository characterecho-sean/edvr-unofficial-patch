#pragma once
#include "gpu_timing.h"
#include <wrl/client.h>
#include <cstdint>

namespace edvr {
// Bounded, optional timestamps for sampled draw work. A busy ring skips a
// measurement; it never waits or flushes the game's command stream. Poll
// once per frame, at least four frames after submission. Failure/disjoint
// counts distinguish unavailable timestamps from a measured zero cost.
template<unsigned Capacity> class GpuIntervals {
    struct Slot {
        GpuTimer timer;
        uint64_t frame=0;
        bool pending=false;
    } slots_[Capacity];
    int open_=-1;
    uint64_t frame_=0;
public:
    struct Totals { double ms=0; unsigned samples=0,skipped=0,invalid=0; } totals;
    bool begin(ID3D11DeviceContext* ctx) {
        if(!ctx)return false;
        Microsoft::WRL::ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(!dev || !gpuTimingBind(dev.Get(),ctx) || !gpuTimingAccepts(ctx))return false;
        if(open_>=0){++totals.skipped;return false;}
        for(unsigned i=0;i<Capacity;++i) {
            auto& s=slots_[i];if(s.pending)continue;
            // Clock/lease pressure is transient: skipping must not permanently
            // disable a sampler whose own timestamp ring still has capacity.
            if(!s.timer.begin(dev.Get(),ctx)){++totals.skipped;return false;}
            open_=int(i);return true;
        }
        ++totals.skipped;return false;
    }
    void end(ID3D11DeviceContext* ctx) {
        if(!ctx || !gpuTimingAccepts(ctx) || open_<0)return;
        auto& s=slots_[open_];
        if(!s.timer.end(ctx)){s.timer.reset(ctx);++totals.invalid;open_=-1;return;}
        s.pending=true;s.frame=frame_;open_=-1;
    }
    void poll(ID3D11DeviceContext* ctx) {
        if(!ctx || !gpuTimingOwns(ctx))return;
        ++frame_;
        if(open_>=0) {
            double ignored=0;
            if(slots_[open_].timer.poll(ctx,ignored)==GpuTimerPoll::Invalid) {
                ++totals.invalid;open_=-1;
            }
        }
        for(auto& s:slots_) {
            if(!s.pending || frame_-s.frame<4)continue;
            double ms=0.0; const auto status=s.timer.poll(ctx,ms);
            if(status==GpuTimerPoll::Pending)continue; s.pending=false;
            if(status==GpuTimerPoll::Ready){totals.ms+=ms;++totals.samples;} else ++totals.invalid;
        }
    }
    void reset(ID3D11DeviceContext* ctx=nullptr) noexcept {
        // After global shutdown/abandon detaches the registry, use reset()
        // without a context for Release-only cleanup of the quiescent sampler.
        if(ctx && !gpuTimingOwns(ctx))return;
        ID3D11DeviceContext* owner=ctx;
        for(auto& s:slots_){s.timer.reset(owner);s=Slot{};}
        open_=-1;frame_=0;totals={};
    }
};
}
