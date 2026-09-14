#include <windows.h>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include "../../src/common/gpu_disjoint_clock.h"
using namespace edvr;
namespace {
unsigned checks = 0;
void check(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
struct Backend final : DisjointBackend {
    struct Slot { bool created=false, open=false, pending=false; } slots[8];
    GpuSpanOwner identity{1,2,0,true};
    unsigned creates=0,begins=0,ends=0,polls=0,destroys=0,live=0;
    bool bad=false, failCreate=false, failBegin=false, failEnd=false;
    DisjointResult answer{DisjointStatus::Ready,1000,false,DisjointReason::None};
    GpuSpanOwner currentOwner() const noexcept override {
        auto o=identity; o.thread=GetCurrentThreadId(); return o;
    }
    bool create(unsigned i) noexcept override {
        if (i>=8 || slots[i].created) { bad=true; return false; }
        if (failCreate) return false;
        slots[i].created=true; ++creates; return true;
    }
    bool begin(unsigned i) noexcept override {
        auto& s=slots[i];
        if (!s.created || s.open || live) { bad=true; return false; }
        if (failBegin) return false;
        s.open=true; s.pending=false; ++live; ++begins; return true;
    }
    bool end(unsigned i) noexcept override {
        auto& s=slots[i];
        if (!s.open || live!=1) { bad=true; return false; }
        s.open=false; s.pending=true; --live; ++ends;
        return !failEnd; // Failure after End; caller cannot know closure.
    }
    DisjointResult poll(unsigned i) noexcept override {
        ++polls;
        if (!slots[i].pending || slots[i].open) { bad=true; return {DisjointStatus::Failed}; }
        return answer;
    }
    void destroy(unsigned i) noexcept override {
        if (!slots[i].created || slots[i].open) bad=true;
        slots[i]={}; ++destroys;
    }
    void clean() { check(!bad && live==0 && creates==destroys && begins==ends,
                         "backend scopes and allocations balanced"); }
};
void selfTest() {
    {
        Backend b; DisjointClock c(b);
        auto frame=c.startFrame(1), a=c.acquireInterval(2), z=c.acquireInterval(3);
        check(bool(frame)&&bool(a)&&bool(z)&&b.begins==1,"borrowers share one scope");
        check(!c.startFrame(3),"second outer refused");
        check(c.endInterval(z,4)&&c.endInterval(a,5)&&b.ends==0,"borrowers cannot close parent");
        check(!c.endInterval(a,5),"duplicate borrower end refused");
        check(c.poll(a,5).status==DisjointStatus::Pending&&b.polls==0,"no poll of open parent");
        check(c.finishFrame(frame,6)&&b.ends==1,"parent closes once");
        b.answer.status=DisjointStatus::Pending;
        check(c.poll(a,7).status==DisjointStatus::Pending,"native pending retained");
        b.answer.status=DisjointStatus::Ready;
        check(c.poll(a,8).frequency==1000,"ended live lease obtains ready frequency");
        const auto polls=b.polls;
        check(c.poll(z,8).frequency==1000&&c.poll(frame,8).frequency==1000&&b.polls==polls,"ready parent cached across leases");
        check(c.release(a,9)&&!c.release(a,9),"copied token cannot release twice");
        check(c.release(z,9)&&c.release(frame,9),"release all references");
        auto next=c.acquireInterval(10);
        check(bool(next)&&b.creates==1,"completed record reuses query");
        check(c.poll(a,10).reason==DisjointReason::InvalidLease,"old record generation refused");
        check(c.endInterval(next,11)&&c.release(next,12),"standalone closes and releases");
        c.collect(13); c.shutdown(14); b.clean();
    }
    {
        Backend b; DisjointClock c(b), foreign(b);
        auto root=c.acquireInterval(0), child=c.acquireInterval(1);
        check(!foreign.endInterval(child,2)&&!foreign.release(child,2),"foreign clock token refused");
        check(c.endInterval(root,3),"standalone owner closes its scope");
        check(!c.endInterval(child,4)&&c.poll(child,4).reason==DisjointReason::Incomplete,"late borrower invalidated");
        check(!c.finishFrame(root,4),"standalone is not explicit frame");
        check(c.release(root,5)&&c.release(child,5),"release incomplete pair");
        c.collect(6); c.shutdown(7); foreign.shutdown(7); b.clean();
    }
    {
        Backend b; DisjointClock c(b); auto frame=c.startFrame(0);
        auto old=c.acquireInterval(1); check(c.release(old,2),"abandoned borrowed lease releases");
        auto replacement=c.acquireInterval(3);
        check(bool(replacement)&&!c.endInterval(old,4)&&!c.release(old,4),"same-record index reuse rejects stale serial");
        DisjointClock::Lease leases[31]{}; leases[0]=replacement;
        for(unsigned i=1;i<31;++i) leases[i]=c.acquireInterval(4);
        check(!c.acquireInterval(5)&&b.begins==1,"lease exhaustion issues no nested begin");
        for(auto t:leases) check(bool(t)&&c.endInterval(t,6),"every bounded lease ends");
        check(c.finishFrame(frame,7),"full lease record closes");
        for(auto t:leases) c.release(t,8);
        c.release(frame,8); c.collect(9); c.shutdown(10); b.clean();
    }
    {
        Backend b; b.answer.status=DisjointStatus::Pending; DisjointClock c(b);
        DisjointClock::Lease tokens[8]{};
        for(auto& t:tokens) {t=c.acquireInterval(0);check(bool(t)&&c.endInterval(t,10),"fill pending ring");}
        check(!c.acquireInterval(11)&&b.begins==8,"pending/referenced ring skips");
        for(auto t:tokens)c.release(t,12);
        check(!c.acquireInterval(13)&&b.creates==8,"unreferenced pending queries still not reused");
        c.collect(2011);
        check(b.destroys==8,"elapsed expiry releases pending resources");
        auto next=c.acquireInterval(2012);check(bool(next)&&b.creates==9,"ring recovers after expiry");
        c.release(next,2013);c.shutdown(2014);b.clean();
    }
    for(auto reason:{DisjointReason::Disjoint,DisjointReason::ZeroFrequency,DisjointReason::DriverFailure,DisjointReason::Stale}) {
        Backend b;DisjointClock c(b);auto t=c.acquireInterval(0);c.endInterval(t,10);
        if(reason==DisjointReason::Disjoint)b.answer.disjoint=true;
        if(reason==DisjointReason::ZeroFrequency)b.answer.frequency=0;
        if(reason==DisjointReason::DriverFailure)b.answer.status=DisjointStatus::Failed;
        if(reason==DisjointReason::Stale)b.answer.status=DisjointStatus::Pending;
        auto r=c.poll(t,reason==DisjointReason::Stale?2011:11);
        check(r.status==DisjointStatus::Failed&&r.reason==reason&&!r.frequency,"invalid frequency cannot become timing");
        c.release(t,2012);c.shutdown(2013);b.clean();
    }
    {
        Backend b;DisjointClock c(b);b.failCreate=true;
        check(!c.startFrame(1)&&b.begins==0,"allocation failure issues no begin");b.failCreate=false;b.failBegin=true;
        check(!c.startFrame(2)&&b.creates==b.destroys,"failed begin releases query");b.failBegin=false;
        auto t=c.startFrame(3);b.failEnd=true;
        check(!c.finishFrame(t,4)&&!c.startFrame(5),"uncertain closure permanently stops begins");
        check(c.poll(t,5).reason==DisjointReason::DriverFailure,"uncertain closure is invalid");
        check(c.shutdown(6)&&b.ends==1,"shutdown after uncertain end does not retry it");b.clean();
    }
    {
        Backend b;DisjointClock c(b);auto t=c.startFrame(0);auto child=c.acquireInterval(1);
        const auto calls=b.begins+b.ends+b.polls+b.destroys;
        bool rejected=false;
        std::thread wrong([&]{rejected=!c.acquireInterval(2)&&!c.endInterval(child,2)&&!c.finishFrame(t,2)&&
            !c.release(child,2)&&!c.collect(2)&&!c.shutdown(2)&&c.poll(child,2).reason==DisjointReason::WrongOwner;});wrong.join();
        check(rejected&&calls==b.begins+b.ends+b.polls+b.destroys,"OS thread rejection has no backend side effects");
        ++b.identity.context;check(!c.acquireInterval(2),"context mismatch rejected");--b.identity.context;
        check(c.shutdown(3)&&b.ends==1,"shutdown closes open parent even with outstanding leases");
        check(!c.release(child,4)&&!c.startFrame(4)&&!c.shutdown(4),"shutdown invalidates tokens and is terminal");b.clean();
    }
    std::printf("PASS: %u shared-disjoint ownership/lifetime checks\n",checks);
}
}
int wmain(int argc,wchar_t**argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    if(argc==2&&!wcscmp(argv[1],L"--dry-run")){std::puts("dry-run: no files or backend calls");return 0;}
    if(argc!=2||wcscmp(argv[1],L"--self-test")){std::fputs("usage: --self-test | --dry-run\n",stderr);return 2;}
    try {selfTest();return 0;}catch(const std::exception&e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
