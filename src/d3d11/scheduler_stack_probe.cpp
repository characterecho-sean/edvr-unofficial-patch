#include "scheduler_stack_probe.h"
#include "scheduler_stack_hook.h"
#include "../common/log.h"
#include <windows.h>
#include <cstring>
#include <string>

namespace edvr {

SchedulerStackProbe schedulerStackProbe;

namespace {
// The ~20 s totals cadence the other instruments keep: frequent enough to
// bracket a short flight, rare enough that
// four target lines per tick stay bounded.
constexpr uint64_t kReportMs=20000;
}

const char* SchedulerStackProbe::targetName(uint32_t target) noexcept {
    switch(target) {
        case 0:return "worker-entry-144321940";
        case 1:return "worker-entry-144320340";
        case 2:return "record-drain-1442df940";
        case 3:return "reset-repopulate-1436a0f50";
    }
    return "unknown";
}

const char* SchedulerStackProbe::hookStatusText() const noexcept {
    switch(hookStatus_) {
        case HookStatus::NotRun:return "not_run";
        case HookStatus::Installed:return "installed";
        case HookStatus::IdentityMismatch:return "identity_mismatch";
        case HookStatus::OpcodeMismatch:return "opcode_mismatch";
        case HookStatus::InstallFailed:return "install_failed";
    }
    return "unknown";
}

uint32_t SchedulerStackProbe::captureStack(uintptr_t top,uintptr_t rangeLo,uintptr_t rangeHi,
                                           uint64_t* out,uint32_t cap,bool* faulted) noexcept {
    uint32_t n=0,gaps=0;
    bool fault=false;
    __try {
        for(uint32_t i=0;i<kScanSlots&&n<cap;++i) {
            uint64_t v=0;
            std::memcpy(&v,reinterpret_cast<const void*>(top+uintptr_t(i)*8),8);
            if(v>=rangeLo&&v<=rangeHi) {
                out[n++]=v;
                gaps=0;
            } else if(n>0&&++gaps>kMaxGaps) {
                break;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // A fault ends the scan where it stood: the collected prefix is
        // still a valid (if shorter) signature, and the fault is counted
        // so a garbage RSP is visible in the report rather than read as a
        // shallow stack.
        fault=true;
    }
    if(faulted)*faulted=fault;
    return n;
}

uint64_t SchedulerStackProbe::fnv1a64(const uint64_t* values,uint32_t count) noexcept {
    uint64_t h=14695981039346656037ull;
    for(uint32_t i=0;i<count;++i) {
        uint64_t v=values[i];
        for(int b=0;b<8;++b) {
            h^=(v>>uint32_t(b*8))&0xFFu;
            h*=1099511628211ull;
        }
    }
    return h;
}

void SchedulerStackProbe::clearLocked() noexcept {
    for(uint32_t i=0;i<kTargetCount;++i) {
        targets_[i].calls.store(0,std::memory_order_relaxed);
        targets_[i].faults.store(0,std::memory_order_relaxed);
        targets_[i].used=0;
        targets_[i].overflow=0;
        targets_[i].collisions=0;
        for(uint32_t k=0;k<kSigCap;++k)targets_[i].sigs[k]=SigEntry{};
    }
    frame_=0;
    reportedDeadWindow_=false;
}

bool SchedulerStackProbe::arm() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    // Validation lives inside the attach, under the hook install mutex --
    // the same race the kinematic probe documented (2026-09-20 review
    // finding 5): an unlocked pre-check can read a half-installed patch.
    const char* result=attachSchedulerStackHooks(this);
    hookStatus_=std::strcmp(result,"installed")==0?HookStatus::Installed
        :std::strcmp(result,"identity_mismatch")==0?HookStatus::IdentityMismatch
        :std::strcmp(result,"opcode_mismatch")==0?HookStatus::OpcodeMismatch
        :HookStatus::InstallFailed;
    if(hookStatus_!=HookStatus::Installed)return false;
    imageBase_=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    clearLocked();
    const uint64_t now=GetTickCount64();
    armedTickMs_=now;
    lastReportTickMs_=now;
    active_.store(true,std::memory_order_release);
    return true;
}

void SchedulerStackProbe::finish() noexcept {
    active_.store(false,std::memory_order_release);
    detachSchedulerStackHooks(this);
}

void SchedulerStackProbe::reset() noexcept {
    active_.store(false,std::memory_order_release);
    detachSchedulerStackHooks(this);
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
}

void SchedulerStackProbe::noteEntry(uint32_t target,uintptr_t entryRsp) noexcept {
    if(!active_.load(std::memory_order_acquire))return;
    if(target>=kTargetCount||!entryRsp)return;
    uint64_t stack[kStackCap];
    bool faulted=false;
    const uint32_t n=captureStack(entryRsp,imageBase_+kRangeLo,imageBase_+kRangeHi,
                                  stack,kStackCap,&faulted);
    targets_[target].calls.fetch_add(1,std::memory_order_relaxed);
    if(faulted)targets_[target].faults.fetch_add(1,std::memory_order_relaxed);
    const uint64_t fnv=fnv1a64(stack,n);
    std::lock_guard<std::mutex> lock(mutex_);
    TargetStat& t=targets_[target];
    for(uint32_t i=0;i<t.used;++i) {
        if(t.sigs[i].fnv!=fnv)continue;
        if(t.sigs[i].depth==n&&
           (n==0||std::memcmp(t.sigs[i].stack,stack,n*sizeof(uint64_t))==0)) {
            ++t.sigs[i].count;
            return;
        }
        // Same FNV-1a-64 over a different stack: astronomically unlikely,
        // and merging it would corrupt both signatures' evidence -- count
        // it, store nothing.
        ++t.collisions;
        return;
    }
    if(t.used>=kSigCap){++t.overflow;return;}
    SigEntry& e=t.sigs[t.used++];
    e.fnv=fnv;
    e.count=1;
    e.depth=n;
    if(n)std::memcpy(e.stack,stack,n*sizeof(uint64_t));
}

SchedulerStackProbe::Summary SchedulerStackProbe::summary() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Summary s{};
    for(uint32_t i=0;i<kTargetCount;++i) {
        s.calls[i]=targets_[i].calls.load(std::memory_order_relaxed);
        s.faults[i]=targets_[i].faults.load(std::memory_order_relaxed);
        s.unique[i]=targets_[i].used;
        s.overflow[i]=targets_[i].overflow;
        s.collisions[i]=targets_[i].collisions;
    }
    return s;
}

void SchedulerStackProbe::notePresentFrame(uint32_t presentFrame) noexcept {
    if(!active_.load(std::memory_order_acquire))return;
    std::string lines;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frame_=presentFrame;
        const uint64_t now=GetTickCount64();
        if(now-lastReportTickMs_<kReportMs)return;
        lastReportTickMs_=now;
        // Snapshot the report text under the lock; Log::note runs after
        // the unlock so a slow log write never stalls a worker thread's
        // capture mid-stack.
        uint64_t totalCalls=0;
        for(uint32_t i=0;i<kTargetCount;++i)
            totalCalls+=targets_[i].calls.load(std::memory_order_relaxed);
        if(totalCalls==0&&!reportedDeadWindow_) {
            reportedDeadWindow_=true;
            lines+="scheduler stack: no observations in the last report window "
                   "-- the feed is dead, not silent-success. The stock path is "
                   "untouched.\n";
        } else if(totalCalls>0) {
            reportedDeadWindow_=false;
        }
        for(uint32_t i=0;i<kTargetCount;++i) {
            const TargetStat& t=targets_[i];
            const uint64_t calls=t.calls.load(std::memory_order_relaxed);
            const uint64_t faults=t.faults.load(std::memory_order_relaxed);
            // Top-3 by count: the table is kSigCap entries, a linear scan
            // for three leaders is nothing at a 20 s cadence.
            uint32_t top[3]={0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu};
            for(uint32_t rank=0;rank<3&&rank<t.used;++rank) {
                uint32_t best=0xFFFFFFFFu;
                for(uint32_t k=0;k<t.used;++k) {
                    if(top[0]==k||top[1]==k||top[2]==k)continue;
                    if(best==0xFFFFFFFFu||t.sigs[k].count>t.sigs[best].count)best=k;
                }
                if(best!=0xFFFFFFFFu)top[rank]=best;
            }
            char head[160];
            std::snprintf(head,sizeof(head),
                "scheduler stack: %s calls=%llu faults=%llu unique=%u overflow=%llu "
                "collisions=%llu",
                targetName(i),
                (unsigned long long)calls,(unsigned long long)faults,
                t.used,(unsigned long long)t.overflow,(unsigned long long)t.collisions);
            lines+=head;
            for(uint32_t rank=0;rank<3;++rank) {
                if(top[rank]==0xFFFFFFFFu)break;
                const SigEntry& e=t.sigs[top[rank]];
                std::snprintf(head,sizeof(head),
                    "; #%u sig=%016llx depth=%u count=%llu (%.0f%%) stack=",
                    rank+1,(unsigned long long)e.fnv,e.depth,
                    (unsigned long long)e.count,
                    calls?100.0*double(e.count)/double(calls):0.0);
                lines+=head;
                // Hex VA list, scan order: index 0 is the immediate caller,
                // each later entry one frame further out.
                for(uint32_t k=0;k<e.depth;++k) {
                    char va[24];
                    std::snprintf(va,sizeof(va),"%s0x%llx",
                                  k?"<-":"",(unsigned long long)e.stack[k]);
                    lines+=va;
                }
            }
            lines+='\n';
        }
    }
    // Bounded volume: at most one dead-feed line plus four target lines
    // every 20 s, each line bounded by top-3 of 24 VAs.
    for(size_t pos=0;pos<lines.size();) {
        const size_t end=lines.find('\n',pos);
        if(end==std::string::npos)break;
        Log::get().note("%s",lines.substr(pos,end-pos).c_str());
        pos=end+1;
    }
}

void SchedulerStackProbe::writeJson(std::ostringstream& j) const {
    std::lock_guard<std::mutex> lock(mutex_);
    j<<"\"schedulerStack\":{\"status\":\""<<hookStatusText()<<"\",\"active\":"
     <<(active()?1:0)<<",\"targets\":[";
    for(uint32_t i=0;i<kTargetCount;++i) {
        if(i)j<<',';
        const TargetStat& t=targets_[i];
        j<<"{\"name\":\""<<targetName(i)<<"\",\"calls\":"
         <<t.calls.load(std::memory_order_relaxed)
         <<",\"faults\":"<<t.faults.load(std::memory_order_relaxed)
         <<",\"unique\":"<<t.used
         <<",\"overflow\":"<<t.overflow
         <<",\"collisions\":"<<t.collisions
         <<",\"signatures\":[";
        for(uint32_t k=0;k<t.used;++k) {
            if(k)j<<',';
            const SigEntry& e=t.sigs[k];
            j<<"{\"sig\":\"0x"<<std::hex<<e.fnv<<std::dec<<"\",\"depth\":"<<e.depth
             <<",\"count\":"<<e.count<<",\"stack\":[";
            for(uint32_t q=0;q<e.depth;++q) {
                if(q)j<<',';
                j<<"\"0x"<<std::hex<<e.stack[q]<<std::dec<<"\"";
            }
            j<<"]}";
        }
        j<<"]}";
    }
    j<<"]}";
}

void SchedulerStackProbe::selfTestPopulateForJson() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
    // Distinctive values, one shape per field, so
    // tools\scheduler_stack_json_selftest.py can assert an exact
    // round-trip: the historical writer bugs were all dropped/misplaced
    // quotes around hex values inside arrays.
    hookStatus_=HookStatus::Installed;
    for(uint32_t i=0;i<kTargetCount;++i) {
        TargetStat& t=targets_[i];
        t.calls.store(1000+i*100+1,std::memory_order_relaxed);
        t.faults.store(i,std::memory_order_relaxed);
        SigEntry& a=t.sigs[0];
        a.fnv=0x1111111100000000ull+uint64_t(i)*0x100000000ull+0xAAAAull;
        a.count=900+i*10+1;
        a.depth=3;
        a.stack[0]=0x1462B4A0ull+uint64_t(i);
        a.stack[1]=0x144321940ull+uint64_t(i);
        a.stack[2]=0x1436A1000ull+uint64_t(i);
        SigEntry& b=t.sigs[1];
        b.fnv=0x2222222200000000ull+uint64_t(i)*0x100000000ull+0xBBBBull;
        b.count=80+i*10+2;
        b.depth=1;
        b.stack[0]=0x145DD120ull+uint64_t(i);
        t.used=2;
        t.overflow=7+i;
        t.collisions=3+i;
    }
}

// --- config-facing lifecycle (driven by the once-per-second
// temporalPassConfigure re-poll) ---------------------------------------

void schedulerStackProbeConfigure(bool on) {
    if(on==schedulerStackProbe.active())return; // 1 Hz re-poll idempotency
    if(!on) {
        const bool was=schedulerStackProbe.active();
        schedulerStackProbe.reset();
        if(was)Log::get().note("scheduler stack: probe stood down, state cleared.");
        return;
    }
    if(!schedulerStackProbe.arm()) {
        static std::string lastFail;
        const char* status=schedulerStackProbe.hookStatusText();
        if(lastFail!=status) {
            lastFail=status;
            Log::get().note("scheduler stack: advanced.scheduler_probe is on but the hook "
                            "refused (%s) -- the probe stands down and the stock path is "
                            "untouched. Retried quietly on later config polls.",status);
        }
        return;
    }
    Log::get().note("scheduler stack: probe live (advanced.scheduler_probe=on) -- "
                    "read-only return-address signatures at the four scheduler-fed "
                    "worker entries. One VA recurring at a consistent frame position "
                    "across signatures is the dispatcher; VAs in 0x1462b0000..0x1462c0000 "
                    "or 0x145dd0000..0x145dde000 are table stubs (the caller above the "
                    "stub is the scheduler). Totals every 20 s per target.");
}

void schedulerStackProbeShutdown() {
    const bool was=schedulerStackProbe.active();
    schedulerStackProbe.reset();
    if(was)Log::get().note("scheduler stack: probe stood down, state cleared.");
}

} // namespace edvr
