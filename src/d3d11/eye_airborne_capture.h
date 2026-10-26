#pragma once
#include <cstdint>

namespace edvr::eye_airborne_capture {
enum class State:uint8_t {Idle,Waiting,Deferred,Triggered,Expired,Cancelled};
enum class Decision:uint8_t {None,Stale,DeferredBusy,Trigger,Expired,Cancelled};
struct Request {
    State state=State::Idle;
    uint64_t requestQpc=0,baseGeneration=0;
    bool staleNoted=false,busyNoted=false;
    void arm(uint64_t qpc,uint64_t generation) noexcept {
        state=State::Waiting;requestQpc=qpc;baseGeneration=generation;staleNoted=busyNoted=false;
    }
    void retryAfterStartRefusal(uint64_t observedGeneration) noexcept {
        // Do not retry the same sample every render tick if another capture
        // acquired the run between poll() and beginEyeRun().
        baseGeneration=observedGeneration;state=State::Deferred;busyNoted=true;
    }
    Decision poll(uint64_t nowQpc,uint64_t qpcFrequency,bool onFoot,bool snapshotValid,bool snapshotFresh,
                  uint64_t generation,bool airborne,bool busy) noexcept {
        if(state==State::Idle||state==State::Triggered||state==State::Expired||state==State::Cancelled)return Decision::None;
        if(!onFoot){state=State::Cancelled;return Decision::Cancelled;}
        if(!requestQpc||!qpcFrequency||nowQpc<requestQpc){state=State::Cancelled;return Decision::Cancelled;}
        if(nowQpc-requestQpc>=qpcFrequency*30u){state=State::Expired;return Decision::Expired;}
        if(!snapshotValid||!snapshotFresh){if(!staleNoted){staleNoted=true;return Decision::Stale;}return Decision::None;}
        if(generation<=baseGeneration||!airborne)return Decision::None;
        if(busy){state=State::Deferred;if(!busyNoted){busyNoted=true;return Decision::DeferredBusy;}return Decision::None;}
        state=State::Triggered;return Decision::Trigger;
    }
};
} // namespace edvr::eye_airborne_capture
