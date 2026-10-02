// C1 DISCARD round trip; C2 passthrough; C3 failed maps; C4 interleaving
// C5 cross-thread/context; C6 stale maps/Present; C7 bounds/alignment; C8 decision.
#include "flat_map_bounce.h"
#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <random>
#include <thread>
using namespace edvr::flatmap;
static int failures=0;
static void check(bool ok,const char* label) { if(!ok) { std::printf("FAIL %s\n",label); ++failures; } }
struct FakeDriver {
    struct Slice { std::vector<unsigned char> raw,gpu; uint32_t width;
        explicit Slice(uint32_t n):raw(n+128,0xEE),gpu(n,0xDD),width(n) { std::memset(data(),0xDD,n); }
        unsigned char* data() { return raw.data()+64; }
        bool guards() const { for(unsigned i=0;i<64;++i) if(raw[i]!=0xEE || raw[64+width+i]!=0xEE) return false; return true; }
        void unmap() { std::memcpy(gpu.data(),data(),width); }
    };
    uint64_t clock=0; int refs=0; bool mismatch=false, ownerThread=true;
    void* committedReal=nullptr; std::vector<unsigned char> committedGpu;
    void unmapBeforeFlush(void* real,size_t n) {
        committedReal=real; const auto* p=static_cast<const unsigned char*>(real); committedGpu.assign(p,p+n);
    }
    void unmap(Slice& slice) { if(committedReal==slice.data()) slice.gpu=committedGpu; else slice.unmap(); }
    void retain(uintptr_t,uintptr_t) { ++refs; }
    void release(uintptr_t,uintptr_t) { --refs; }
    uint64_t clockTicks() { return ++clock; }
    uint64_t ticksPerSecond() { return 1000000000; }
    bool verify(void* a,const void* b,size_t n) { return !mismatch && std::memcmp(a,b,n)==0; }
};
using Core=Runtime<FakeDriver>;
static MapRequest request(uintptr_t resource,FakeDriver::Slice& slice,uintptr_t context=10,uint64_t frame=0) {
    MapRequest r; r.resource=resource; r.context=context; r.real=slice.data(); r.width=slice.width;
    r.frame=frame; r.discard=true; r.eligible=true; r.success=true; return r;
}
static void roundtrip() {
    FakeDriver d; auto core=std::make_unique<Core>(d); core->setMode(Mode::On);
    FakeDriver::Slice real(512); auto r=request(2,real);
    std::array<unsigned char,512> seed; seed.fill(0x42); r.seed=seed.data(); r.seedValid=true;
    auto* ptr=static_cast<unsigned char*>(core->install(r));
    check(ptr!=r.real,"C1.bounce"); check(std::memcmp(ptr,seed.data(),512)==0,"C1.seed");
    std::memset(ptr+16,0x31,48); seed.fill(0x42); std::memset(seed.data()+16,0x31,48);
    { auto lease=core->beginUnmap(2,10); check(lease.bounced(),"C1.lease");
      check(std::memcmp(real.data(),seed.data(),512)==0,"C1.flush-before-observer");
      check(std::memcmp(lease.data(),seed.data(),512)==0,"C1.shadow"); }
    d.unmap(real); check(real.gpu==std::vector<unsigned char>(seed.begin(),seed.end()),"C1.gpu");
    check(real.guards(),"C7.flush-guards"); check(d.refs==0,"C1.refs");
    check(core->counters().flushBytes==512,"C1.exact-width");
    check(core->counters().rowsChecked==32 && core->counters().unchangedRows==29,"C1.unchanged-rows");
}
static void passthrough() {
    FakeDriver d; auto core=std::make_unique<Core>(d); FakeDriver::Slice real(256); auto r=request(2,real);
    check(core->install(r)==r.real,"C2.pending"); core->setMode(Mode::Off); check(core->install(r)==r.real,"C2.off");
    core->setMode(Mode::On); r.discard=false; check(core->install(r)==r.real,"C2.not-discard");
    r.discard=true; r.eligible=false; check(core->install(r)==r.real,"C2.ineligible");
    r.eligible=true; r.width=65537; check(core->install(r)==r.real,"C7.width-limit");
    r.width=0; check(core->install(r)==r.real,"C7.zero-width");
}
static void failuresTest() {
    FakeDriver d; auto core=std::make_unique<Core>(d); core->setMode(Mode::On);
    FakeDriver::Slice real(256); auto r=request(2,real); r.success=false;
    check(core->install(r)==r.real,"C3.E_FAIL"); r.real=nullptr;
    check(core->install(r)==nullptr,"C3.WAS_STILL_DRAWING-null"); check(d.refs==0,"C3.no-record");
}
static void interleave() {
    FakeDriver d; auto core=std::make_unique<Core>(d); core->setMode(Mode::On);
    std::vector<std::unique_ptr<FakeDriver::Slice>> real;
    std::array<unsigned char*,8> cached{};
    for(unsigned i=0;i<9;++i) { real.emplace_back(new FakeDriver::Slice(65536)); auto r=request(i+2,*real.back());
      auto* ptr=static_cast<unsigned char*>(core->install(r));
      if(i<8) { cached[i]=ptr; check(ptr!=r.real,"C4.eight-slots"); check(uintptr_t(ptr)%64==0,"C7.aligned");
        std::memset(ptr,int(i+1),65536); for(unsigned j=0;j<i;++j) check(cached[j]!=ptr,"C4.unique"); }
      else check(ptr==r.real,"C4.full"); }
    for(unsigned i=0;i<8;++i) { { auto l=core->beginUnmap(i+2,10);
        check(l.bounced(),"C4.record"); check(real[i]->data()[0]==i+1 && real[i]->data()[65535]==i+1,"C4.bytes");
        if(l.bounced()) { const auto* p=static_cast<const unsigned char*>(l.data());
          for(unsigned g=0;g<64;++g) check(p[65536+g]==0,"C7.cache-guard"); } }
      check(real[i]->guards(),"C7.driver-guard"); }
    check(d.refs==0,"C4.refs");
}
static void contextAndMode() {
    FakeDriver d; auto core=std::make_unique<Core>(d); core->setMode(Mode::On);
    FakeDriver::Slice real(256); auto r=request(2,real); auto* p=core->install(r); std::memset(p,0x62,256);
    { auto wrong=core->beginUnmap(2,11); check(!wrong.bounced(),"C5.context-decline"); }
    check(real.data()[0]==0xDD,"C5.context-no-flush"); check(core->decision()==State::Tripped,"C5.context-trip");
    core->setMode(Mode::Off); // Models stand-down/reset while the game owns the pointer.
    d.ownerThread=false;
    std::thread worker([&] { auto late=core->beginUnmap(2,10);
        check(late.bounced(),"C5.late-any-thread"); check(real.data()[255]==0x62,"C5.late-flush"); });
    worker.join();
    d.ownerThread=true;
    check(d.refs==0,"C5.refs");
    auto clean=std::make_unique<Core>(d); clean->setMode(Mode::On);
    for(unsigned i=0;i<16;++i) { clean->install(r); auto l=clean->beginUnmap(2,10); }
    check(clean->counters().verifySamples==1,"C5.verify-on");
    d.mismatch=true;
    for(unsigned i=0;i<16;++i) { clean->install(r); auto l=clean->beginUnmap(2,10); }
    check(clean->counters().verifyMismatches==1 && clean->tripReason()==Trip::Verify,"C5.verify-trip");
}
static void stale() {
    FakeDriver d; auto core=std::make_unique<Core>(d); core->setMode(Mode::On);
    for(unsigned i=0;i<3;++i) { FakeDriver::Slice real(256); auto r=request(2,real);
      auto* p=core->install(r); std::memset(p,0x63,256); core->preMap(2);
      check(real.data()[0]==0xDD,"C6.stale-no-flush"); check(d.refs==0,"C6.drop-reference"); }
    check(core->counters().abandoned==3 && core->tripReason()==Trip::Remap,"C6.remap-trip");
    auto watchdog=std::make_unique<Core>(d); watchdog->setMode(Mode::On);
    FakeDriver::Slice real(256); auto r=request(3,real,10,7); auto* p=watchdog->install(r); std::memset(p,0x11,256);
    watchdog->present(7); check(watchdog->decision()==State::On,"C6.current-frame"); watchdog->present(8);
    check(watchdog->tripReason()==Trip::Present,"C6.present-trip"); check(real.data()[0]==0xDD,"C6.present-retains");
    { auto l=watchdog->beginUnmap(3,10); check(l.bounced() && real.data()[0]==0x11,"C6.present-late-flush"); }
    check(d.refs==0,"C6.present-refs");
}
static void decisions() {
    for(unsigned test=0;test<4;++test) { FakeDriver d; auto core=std::make_unique<Core>(d);
      const uint64_t ticks=test==0 ? 0 : test==1 ? 255 : test==2 ? 256 : 257;
      core->observeCopy(255,999999); check(core->samples()==0,"C8.min-width");
      for(unsigned i=0;i<31;++i) core->observeCopy(256,ticks);
      check(core->decision()==State::Pending,"C8.pending31"); core->observeCopy(256,ticks);
      check(core->decision()==(test==3 ? State::On : State::Off),"C8.threshold");
      for(unsigned i=0;i<64;++i) core->observeCopy(256,test==3 ? 0 : 1000000);
      check(core->samples()==32 && core->decision()==(test==3 ? State::On : State::Off),"C8.frozen"); }
    FakeDriver d; auto median=std::make_unique<Core>(d);
    for(unsigned batch=0;batch<4;++batch) for(unsigned i=0;i<8;++i) median->observeCopy(256,batch==0 ? 1000000 : 32);
    check(median->decision()==State::Off,"C8.median-not-total");
}
static void randomPrograms() {
    // Defined-byte oracle: DISCARD tails are seeded from the prior shadow.
    // Six resources, two contexts and three logical threads; paused/mode changes
    // after Map must not interrupt the outstanding writeback.
    std::mt19937 rng(0x65CB); FakeDriver d; auto core=std::make_unique<Core>(d); core->setMode(Mode::On);
    std::array<std::array<unsigned char,512>,6> shadow{};
    for(unsigned n=0;n<1200;++n) { const unsigned resource=rng()%6, context=rng()%2, thread=rng()%3;
      FakeDriver::Slice real(512); auto r=request(resource+2,real,context+10);
      r.seed=shadow[resource].data(); r.seedValid=true; core->preMap(r.resource);
      const bool passthrough=(rng()%4)==0; r.discard=!passthrough; r.eligible=thread==0;
      core->setMode(Mode::On); auto* p=static_cast<unsigned char*>(core->install(r)); const bool bounced=p!=r.real;
      check(bounced==(!passthrough && thread==0),"C4.random-decision");
      auto expected=bounced ? shadow[resource] : std::array<unsigned char,512>{};
      if(!bounced) expected.fill(0xDD);
      const unsigned start=rng()%512, count=1+rng()%(512-start); const unsigned char value=static_cast<unsigned char>(rng());
      std::memset(p+start,value,count); std::memset(expected.data()+start,value,count);
      core->setMode(Mode::Off); { auto lease=core->beginUnmap(r.resource,r.context);
        if(bounced) { check(lease.bounced(),"C4.random-lease"); check(std::memcmp(lease.data(),expected.data(),512)==0,"C4.random-shadow"); } }
      d.unmap(real); check(std::memcmp(real.gpu.data(),expected.data(),512)==0,"C4.random-gpu");
      check(real.guards(),"C7.random-guard"); shadow[resource]=expected;
    }
    check(d.refs==0,"C4.random-refs");
}
int main(int argc,char** argv) {
    if(argc<2 || (std::string(argv[1])!="--dry-run" && std::string(argv[1])!="--self-test")) return 2;
    if(std::string(argv[1])=="--dry-run") { std::puts("flat_map_bounce_test: C1-C8 fake-driver tests; no files or GPU device"); return 0; }
    roundtrip(); passthrough(); failuresTest(); interleave(); contextAndMode(); stale(); decisions(); randomPrograms();
    std::printf("flat_map_bounce_test: %s (%d failures)\n",failures ? "FAIL" : "PASS",failures); return failures ? 1 : 0;
}
