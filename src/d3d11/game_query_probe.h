#pragma once
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <cstdint>
#include "../common/log.h"
#include "../common/game_call_probe.h"

namespace edvr {
// Temporary LOD/exit investigation. Observe only calls made directly by the
// executable on the hooked game context. Never retain queries, poll them again,
// flush, change flags, or touch the caller's result buffer.
class GameQueryProbe {
    struct Entry {
        ID3D11Asynchronous* query=nullptr; // Identity only; never dereferenced later.
        uint64_t since=0, polls=0;
        uintptr_t caller=0;
        bool pending=false, reported=false;
    } entries_[64];
    SRWLOCK lock_=SRWLOCK_INIT;
    struct ActiveEntry {
        ID3D11Asynchronous* query=nullptr; // Valid for the Begin/End interval.
        uint64_t depth=0;
    } active_[64];
    SRWLOCK activeLock_=SRWLOCK_INIT;
    bool activeOverflow_=false;
    std::atomic<uint64_t> contended_{0};
    uint64_t calls_=0, ready_=0, pending_=0, failed_=0, overflow_=0, lastReport_=0;
    unsigned reports_=0, details_=0;
    static uintptr_t gameCaller(const void* caller) noexcept {
        static const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        static const size_t size=[] {
            const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
            const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(dos)+dos->e_lfanew);
            return size_t(nt->OptionalHeader.SizeOfImage);
        }();
        const auto address=reinterpret_cast<uintptr_t>(caller);
        return address>=base && address-base<size ? address-base : 0;
    }
    static bool countingQuery(ID3D11Asynchronous* asynchronous) noexcept {
        if(!asynchronous)return false;
        ID3D11Query* query=nullptr;
        if(FAILED(asynchronous->QueryInterface(__uuidof(ID3D11Query),reinterpret_cast<void**>(&query))) || !query)
            return true; // A counter or unknown asynchronous is unsafe to duplicate.
        D3D11_QUERY_DESC desc{};query->GetDesc(&desc);query->Release();
        // EVENT and TIMESTAMP have no Begin interval. TIMESTAMP_DISJOINT is
        // an interval, but records clock validity/frequency rather than draw
        // samples, primitives or pipeline statistics.
        return desc.Query!=D3D11_QUERY_EVENT && desc.Query!=D3D11_QUERY_TIMESTAMP &&
            desc.Query!=D3D11_QUERY_TIMESTAMP_DISJOINT;
    }
public:
    template<class Forward>
    HRESULT read(Forward forward,ID3D11DeviceContext* context,ID3D11Asynchronous* query,
                 void* data,UINT bytes,UINT flags,const void* caller) noexcept {
        const HRESULT result=forward(context,query,data,bytes,flags);
        note(query,caller,result,flags,bytes);
        return result;
    }
    struct Event {
        bool first=false, detail=false, summary=false;
        uint64_t age=0, polls=0, calls=0, ready=0, pending=0, failed=0, overflow=0;
        uintptr_t caller=0;
        unsigned outstanding=0;
    };
    // Fixed-memory CPU state, also driven by the regression fixture. A new End
    // starts another observed query interval; a successful/error read retires it.
    Event observe(ID3D11Asynchronous* query,uintptr_t caller,HRESULT result,uint64_t now) noexcept {
        Event event{};
        if(!TryAcquireSRWLockExclusive(&lock_)) { ++contended_; return event; }
        ++calls_; if(result==S_OK)++ready_;else if(result==S_FALSE)++pending_;else ++failed_;
        event.first=calls_==1;
        Entry* entry=nullptr; Entry* free=nullptr;
        for(auto& e:entries_) {
            if(e.query==query) { entry=&e;break; }
            if(!e.pending && !free)free=&e;
        }
        if(result==S_FALSE && query) {
            if(!entry)entry=free;
            if(!entry)++overflow_;
            else {
                if(entry->query!=query || !entry->pending) *entry={query,now,0,caller,true,false};
                ++entry->polls;
                if(now>=entry->since && now-entry->since>=1000 && !entry->reported && details_<32) {
                    entry->reported=true;++details_;event.detail=true;
                    event.age=now-entry->since;event.polls=entry->polls;event.caller=caller;
                }
            }
        } else if(entry)entry->pending=false;
        if(!lastReport_)lastReport_=now;
        if(now>=lastReport_ && now-lastReport_>=20000 && reports_<60) {
            lastReport_=now;++reports_;event.summary=true;event.calls=calls_;event.ready=ready_;
            event.pending=pending_;event.failed=failed_;event.overflow=overflow_;
            for(const auto& e:entries_)if(e.pending)++event.outstanding;
        }
        ReleaseSRWLockExclusive(&lock_);return event;
    }
    void ended(ID3D11Asynchronous* query) noexcept {
        if(!TryAcquireSRWLockExclusive(&lock_)) { ++contended_;return; }
        for(auto& e:entries_)if(e.query==query) { e.pending=false;break; }
        ReleaseSRWLockExclusive(&lock_);
    }
    // A duplicated draw inside one of these intervals would alter the game's
    // occlusion, pipeline or stream-output result. Keep a fixed identity set;
    // duplicate Begin remains blocked until matching End calls retire it.
    // Table overflow is sticky because an unknown End cannot safely identify
    // which unrecorded interval it closes.
    void bracketBegin(ID3D11Asynchronous* query) noexcept {
        if(!countingQuery(query))return;
        AcquireSRWLockExclusive(&activeLock_);
        ActiveEntry* free=nullptr;
        for(auto& entry:active_) {
            if(entry.query==query) {
                if(entry.depth!=~uint64_t(0))++entry.depth;else activeOverflow_=true;
                ReleaseSRWLockExclusive(&activeLock_);return;
            }
            if(!entry.query && !free)free=&entry;
        }
        if(free)*free={query,1};else activeOverflow_=true;
        ReleaseSRWLockExclusive(&activeLock_);
    }
    void bracketEnd(ID3D11Asynchronous* query) noexcept {
        if(!countingQuery(query))return;
        AcquireSRWLockExclusive(&activeLock_);
        for(auto& entry:active_)if(entry.query==query) {
            if(entry.depth>1)--entry.depth;else entry={};
            ReleaseSRWLockExclusive(&activeLock_);return;
        }
        ReleaseSRWLockExclusive(&activeLock_);
    }
    bool countingActive() noexcept {
        AcquireSRWLockShared(&activeLock_);
        bool active=activeOverflow_!=0;
        if(!active)for(const auto& entry:active_)if(entry.query){active=true;break;}
        ReleaseSRWLockShared(&activeLock_);return active;
    }
    void noteEnd(ID3D11Asynchronous* query,const void* caller) noexcept {
        if(gameCaller(caller))ended(query);
    }
    void note(ID3D11Asynchronous* query,const void* caller,HRESULT result,UINT flags,UINT bytes) noexcept {
        const uintptr_t rva=gameCaller(caller);if(!rva)return;
        const auto event=observe(query,rva,result,GetTickCount64());
        if(event.first)Log::get().note("game query probe: first direct GetData caller_rva=0x%llX result=0x%08X flags=%u bytes=%u.",
            (unsigned long long)rva,unsigned(result),flags,bytes);
        if(event.first || event.summary) {
            const auto stack=captureGameCallStack();
            Log::get().note("game query caller: caller_rva=0x%llX result=0x%08X stack_frames=%u game_frames=%u game_rvas=%s.",
                (unsigned long long)rva,unsigned(result),stack.captured,stack.gameFrames,stack.rvas);
        }
        // Logging is outside the probe lock. These are CPU observations, not
        // GPU duration or proof that a particular query belongs to terrain.
        if(event.detail) {
            UINT type=~0u;ID3D11Query* typed=nullptr;
            if(query && SUCCEEDED(query->QueryInterface(__uuidof(ID3D11Query),reinterpret_cast<void**>(&typed)))) {
                D3D11_QUERY_DESC desc{};typed->GetDesc(&desc);type=desc.Query;typed->Release();
            }
            Log::get().note("game query pending: query=%p type=%u caller_rva=0x%llX age_ms=%llu polls=%llu flags=%u bytes=%u; observed S_FALSE, no extra poll or flush.",
                query,type,(unsigned long long)event.caller,(unsigned long long)event.age,
                (unsigned long long)event.polls,flags,bytes);
        }
        if(event.summary)Log::get().note("game query summary: calls=%llu ready=%llu pending=%llu other_results=%llu tracked_pending=%u overflow=%llu contention=%llu; direct executable callers on game context, cumulative, bounded diagnostic.",
            (unsigned long long)event.calls,(unsigned long long)event.ready,(unsigned long long)event.pending,
            (unsigned long long)event.failed,event.outstanding,(unsigned long long)event.overflow,
            (unsigned long long)contended_.load());
    }
};
}
