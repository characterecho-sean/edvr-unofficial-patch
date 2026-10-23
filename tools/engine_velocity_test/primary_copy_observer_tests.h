#pragma once
// Cost and behaviour of the two job-thread observers that watch Elite's list merge (FUN_144351440) and typed-dictionary
// clear (FUN_1436846D0), plus the pool copier's mutex, against the in-process fake of the engine's dictionary/list memory.
// docs\flat-cz-fps-2026-10-11.md. The benchmark (--bench) is opt-in: the normal rig run never executes it.
#include "primary_copy_tests.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <tuple>
#include <string>
#include <thread>
#include <vector>

namespace primary_copy_observer_tests {
namespace copy=edvr::engine_velocity_primary_copy;
using primary_copy_tests::Node;
using primary_copy_tests::Record;
using primary_copy_tests::record;
using primary_copy_tests::claim;

// A fake typed-336 dictionary: one bucket chain of `entryCount` entries, each owning a doubly linked list of
// `nodesPerEntry` full nodes. Every address the observers read lives in this object, laid out as build 334473 is
// (dictionary +0x10 buckets, +0x18 bucket count; entry +0 next, +0x28/+0x30 first/last node; node +0 next, +8 previous,
// +0x18 count, records from +0x20).
struct Tree {
    alignas(16) unsigned char head[48]{};
    uintptr_t buckets[2]{};
    struct Entry { alignas(16) unsigned char raw[56]{}; std::vector<std::unique_ptr<Node>> nodes; };
    std::vector<std::unique_ptr<Entry>> entries;
    Tree(unsigned entryCount,unsigned nodesPerEntry) {
        for(unsigned e=0;e<entryCount;++e) {
            entries.push_back(std::make_unique<Entry>());
            for(unsigned n=0;n<nodesPerEntry;++n)entries.back()->nodes.push_back(std::make_unique<Node>());
        }
        link();
    }
    uintptr_t ptr(){return reinterpret_cast<uintptr_t>(head);}
    uintptr_t sentinel(unsigned b){return reinterpret_cast<uintptr_t>(buckets)+8u*b;}
    uintptr_t entryAddress(unsigned e){return reinterpret_cast<uintptr_t>(entries[e]->raw);}
    uintptr_t anchor(unsigned e=0){return entryAddress(e)+0x28;}
    Node& node(unsigned e,unsigned n){return *entries[e]->nodes[n];}
    Record& item(unsigned e,unsigned n,unsigned i){return node(e,n).item(i);}
    uintptr_t itemAddress(unsigned e,unsigned n,unsigned i){return reinterpret_cast<uintptr_t>(&item(e,n,i));}
    void setCount(unsigned e,unsigned n,uint64_t count){std::memcpy(node(e,n).data+24,&count,8);}
    void setWord(uintptr_t where,uintptr_t value){std::memcpy(reinterpret_cast<void*>(where),&value,8);}
    void link() {
        const uint64_t bucketCount=1;const uintptr_t bp=reinterpret_cast<uintptr_t>(buckets);
        std::memcpy(head+0x10,&bp,8);std::memcpy(head+0x18,&bucketCount,8);
        for(unsigned e=0;e<entries.size();++e) {
            Entry& en=*entries[e];
            const uintptr_t next=e+1<entries.size()?entryAddress(e+1):sentinel(0),list=anchor(e);
            const uintptr_t first=en.nodes.empty()?list:en.nodes.front()->ptr(),last=en.nodes.empty()?list:en.nodes.back()->ptr();
            std::memcpy(en.raw,&next,8);std::memcpy(en.raw+0x28,&first,8);std::memcpy(en.raw+0x30,&last,8);
            for(size_t n=0;n<en.nodes.size();++n) {
                const uint64_t h[4]={n+1<en.nodes.size()?en.nodes[n+1]->ptr():list,n?en.nodes[n-1]->ptr():list,0,8};
                std::memcpy(en.nodes[n]->data,h,32);
            }
        }
        buckets[0]=entries.empty()?sentinel(0):entryAddress(0);buckets[1]=sentinel(1);
    }
};
// An empty destination list head: both links point at itself.
struct Destination {
    uintptr_t links[2]{};
    Destination(){links[0]=links[1]=reinterpret_cast<uintptr_t>(links);}
    uintptr_t ptr(){return reinterpret_cast<uintptr_t>(links);}
};

namespace emit=edvr::engine_velocity_emit;
using Check=void (*)(bool,const char*);
constexpr uint32_t kFrame=42;
// The published pool view (any patch, the active leases as (base, slots)) equals what g_pools holds, and no write is mid-flight.
inline bool viewExact() {
    // Each lease also publishes a bound on its pool's patch slots: sound (every patch inside it) and "none" (lo>hi) exactly when the pool holds none.
    struct Lease { uintptr_t base;uint32_t slots,lo,hi;bool operator<(const Lease& o) const {return std::tie(base,slots,lo,hi)<std::tie(o.base,o.slots,o.lo,o.hi);} };
    std::vector<Lease> truth,seen;bool patches=false;
    for(const auto& entry:copy::g_pools) {
        patches=patches || !entry.second.patches.empty();
        if(!entry.second.active)continue;
        uint32_t lo=~0u,hi=0;for(const auto& p:entry.second.patches){lo=std::min(lo,p.first);hi=std::max(hi,p.first);}
        truth.push_back({entry.second.base,entry.second.bytes/copy::kBytes,lo,hi});
    }
    const uint32_t n=copy::g_poolView.count.load();
    if((copy::g_poolView.seq.load()&1u) || (copy::g_publishSeq.load()&1u) || copy::g_poolView.patches.load()!=patches || n!=truth.size())return false;
    for(uint32_t i=0;i<n;++i)seen.push_back({copy::g_poolView.base[i].load(),copy::g_poolView.slots[i].load(),copy::g_poolView.patchLo[i].load(),copy::g_poolView.patchHi[i].load()});
    std::sort(truth.begin(),truth.end());std::sort(seen.begin(),seen.end());
    for(size_t i=0;i<truth.size();++i) {
        if(truth[i].base!=seen[i].base || truth[i].slots!=seen[i].slots)return false;
        const bool noneTruth=truth[i].lo>truth[i].hi,noneSeen=seen[i].lo>seen[i].hi;
        if(noneTruth!=noneSeen || (!noneTruth && (seen[i].lo>truth[i].lo || seen[i].hi<truth[i].hi)))return false;
    }
    return true;
}
// The published claim snapshot (the live claim addresses, sorted, or "more than it holds"; whether a plan is in flight) equals the containers.
inline bool claimViewExact() {
    copy::ClaimViewState truth,seen;truth.plans=!copy::g_mergePlans.empty();
    std::vector<uintptr_t> keys;for(const auto& e:copy::g_emissions)keys.push_back(e.first);std::sort(keys.begin(),keys.end());
    if(keys.size()>copy::kClaimViewMax)truth.count=copy::kClaimViewMax+1;
    else {truth.count=uint32_t(keys.size());for(size_t i=0;i<keys.size();++i)truth.address[i]=keys[i];}
    seen.count=copy::g_claimView.count.load();seen.plans=copy::g_claimView.plans.load();
    for(uint32_t i=0;i<copy::kClaimViewMax;++i)seen.address[i]=copy::g_claimView.address[i].load();
    return !(copy::g_claimView.seq.load()&1u) && truth==seen;
}
// Everything the lock-free exits read equals the state they stand for.
inline bool published() {
    return copy::g_claimsLive.load()==(!copy::g_emissions.empty() || !copy::g_mergePlans.empty()) &&
           copy::g_emissionsLive.load()==!copy::g_emissions.empty() && viewExact() && claimViewExact();
}
inline uintptr_t addressOf(const Record& r){return reinterpret_cast<uintptr_t>(&r);}
inline bool held(uintptr_t address){return copy::g_emissions.count(address)==1;}

// The clear walk exactly as it was before the small-buffer sets: three std::unordered_sets, the same bounds in the same
// order. The equivalence cases run it and the production walk on identical memory and compare everything they leave behind.
inline void referenceClear(uintptr_t dictionary) noexcept {
    copy::ClaimLock lock;++copy::g_stats.clearCalls;
    auto fail=[](){copy::g_emissions.clear();++copy::g_emissionEpoch;++copy::g_stats.sourceResets;++copy::g_stats.clearFailed;};
    try {
        uintptr_t buckets=0;uint64_t count=0;
        if(!dictionary || dictionary>UINTPTR_MAX-0x20 ||
           !emit::read(dictionary+0x10,&buckets,8) || !emit::read(dictionary+0x18,&count,8) ||
           count>copy::kMaxEmissions || (count && (!buckets || buckets>UINTPTR_MAX-count*8))) {fail();return;}
        std::unordered_set<uintptr_t> entries,nodes,addresses;
        for(uint64_t b=0;b<count;++b){
            const uintptr_t sentinel=buckets+uintptr_t(b)*8;uintptr_t entry=0;
            if(!emit::read(sentinel,&entry,8)){fail();return;}
            while(entry!=sentinel){
                if(!entry || entry>UINTPTR_MAX-0x38 || entries.size()>=copy::kMaxEmissions || !entries.insert(entry).second){fail();return;}
                uintptr_t nextEntry=0,links[2]{};const uintptr_t anchor=entry+0x28;
                if(!emit::read(entry,&nextEntry,8) || !emit::read(anchor,links,16)){fail();return;}
                uintptr_t node=links[0],previous=anchor;
                while(node!=anchor){
                    uint64_t header[4]{};
                    if(!node || node>UINTPTR_MAX-(emit::kNodeRecords+8*copy::kBytes) || nodes.size()>=copy::kMaxEmissions/8 ||
                       !nodes.insert(node).second || !emit::read(node,header,32) || header[1]!=previous || header[3]>8){fail();return;}
                    ++copy::g_stats.clearNodes;
                    for(uint32_t i=0;i<8;++i)addresses.insert(node+emit::kNodeRecords+uintptr_t(i)*copy::kBytes);
                    previous=node;node=uintptr_t(header[0]);
                }
                if(links[1]!=previous){fail();return;}
                entry=nextEntry;
            }
        }
        for(auto address:addresses){const auto n=copy::g_emissions.erase(address);copy::g_stats.invalidated+=n;copy::g_stats.clearedClaims+=n;}
        for(auto* plan:copy::g_mergePlans)for(auto address:plan->addresses)if(addresses.count(address)){plan->invalid=true;break;}
    }catch(...){fail();}
}
// The copier exactly as it was before its lock-free exit.
inline void referenceCopier(uintptr_t base,uint32_t stride,uintptr_t source,uint32_t firstSlot,uint32_t count,uint32_t frame) noexcept {
    copy::ClaimLock lock;copy::g_poolsDirty=true;++copy::g_stats.copies;
    if(!count)return;
    copy::Pool* pool=nullptr;
    uint32_t matches=0;
    for(auto& entry:copy::g_pools)if(entry.second.active && entry.second.base==base) {
        pool=&entry.second;++matches;
    }
    if(matches>1){++copy::g_stats.copierAmbiguous;for(auto& entry:copy::g_pools)if(entry.second.active && entry.second.base==base){entry.second.patches.clear();entry.second.valid=false;++copy::g_stats.invalidated;}pool=nullptr;}
    if(!matches)++copy::g_stats.copierNoLease;
    const bool range=pool && stride==copy::kBytes && count && firstSlot<=pool->bytes/copy::kBytes && count<=pool->bytes/copy::kBytes-firstSlot;
    if(pool) {
        if(!range){pool->patches.clear();pool->valid=false;++copy::g_stats.invalidated;++copy::g_stats.copierInvalidRange;}
        else for(uint32_t i=0;i<count;++i)copy::g_stats.invalidated+=pool->patches.erase(firstSlot+i);
    }
    if(count>copy::kMaxPatches || !source || source>UINTPTR_MAX-uint64_t(count)*copy::kBytes) {
        copy::g_emissions.clear();++copy::g_emissionEpoch;++copy::g_stats.declined;return;
    }
    for(uint32_t i=0;i<count;++i) {
        auto e=copy::g_emissions.find(source+uintptr_t(i)*copy::kBytes);if(e==copy::g_emissions.end())continue;
        const copy::Emission emission=e->second;copy::g_emissions.erase(e);
        if(!range || !pool->active || !pool->valid || pool->frame!=frame || emission.frame!=frame || copy::g_overflowFrame==frame) {++copy::g_stats.declined;continue;}
        std::array<uint32_t,copy::kWords> src{},dst{};
        if(!emit::read(source+uintptr_t(i)*copy::kBytes,src.data(),copy::kBytes) ||
           !emit::read(base+uintptr_t(firstSlot+i)*copy::kBytes,dst.data(),copy::kBytes) ||
           src!=emission.patch.native || dst!=emission.patch.native) {++copy::g_stats.declined;continue;}
        try {
            if(pool->patches.size()>=copy::kMaxPatches){copy::overflow(frame);return;}
            copy::Patch patch=emission.patch;patch.slot=firstSlot+i;copy::notePatch(*pool,patch.slot);pool->patches[patch.slot]=patch;++copy::g_stats.joined;
        } catch(...) {copy::overflow(frame);return;}
    }
}
struct Outcome {
    std::vector<uintptr_t> keys;
    uint64_t clearNodes=0,clearedClaims=0,invalidated=0,clearFailed=0,sourceResets=0,declined=0,epochDelta=0;
    bool planInvalid=false;
    bool operator==(const Outcome& o) const {
        return keys==o.keys && clearNodes==o.clearNodes && clearedClaims==o.clearedClaims && invalidated==o.invalidated &&
               clearFailed==o.clearFailed && sourceResets==o.sourceResets && declined==o.declined && epochDelta==o.epochDelta &&
               planInvalid==o.planInvalid;
    }
};
inline Outcome snapshot(uint64_t epochBefore,bool planInvalid) {
    Outcome o;for(const auto& e:copy::g_emissions)o.keys.push_back(e.first);std::sort(o.keys.begin(),o.keys.end());
    const auto s=copy::stats();
    o.clearNodes=s.clearNodes;o.clearedClaims=s.clearedClaims;o.invalidated=s.invalidated;o.clearFailed=s.clearFailed;
    o.sourceResets=s.sourceResets;o.declined=s.declined;o.epochDelta=copy::g_emissionEpoch-epochBefore;o.planInvalid=planInvalid;return o;
}
// One shape of dictionary for the walk-equivalence cases.
struct Shape {
    const char* name;unsigned entries,nodesPerEntry;
    void (*corrupt)(Tree&);
    unsigned claimEntry[3],claimNode[3],claimSlot[3];unsigned claims;   // claims inside the tree
    bool expectFail;
};
inline void cycleThroughEarlierNode(Tree& t){t.setWord(reinterpret_cast<uintptr_t>(t.node(0,2).data),t.node(0,0).ptr());}
inline void selfLoop(Tree& t){t.setWord(reinterpret_cast<uintptr_t>(t.node(0,1).data),t.node(0,1).ptr());}
inline void countNine(Tree& t){t.setCount(0,1,9);}
inline void unreadableNode(Tree& t){t.setWord(reinterpret_cast<uintptr_t>(t.node(0,0).data),0x10);}
inline void bucketsOverBound(Tree& t){const uint64_t n=copy::kMaxEmissions+1;std::memcpy(t.head+0x18,&n,8);}
inline void aliasedBucket(Tree& t){const uint64_t n=2;std::memcpy(t.head+0x18,&n,8);t.buckets[1]=t.entryAddress(0);}
// The pool copier's lock-free exit: what it takes, what it must leave to the mutex, and that the two agree.
struct CopierOutcome {
    uint64_t copies=0,noLease=0,ambiguous=0,invalidRange=0,invalidated=0,declined=0,joined=0,epochDelta=0;
    std::vector<uintptr_t> keys;std::vector<std::vector<uint64_t>> pools;   // per pool: present, active, valid, patch slots...
    bool operator==(const CopierOutcome& o) const {
        return copies==o.copies && noLease==o.noLease && ambiguous==o.ambiguous && invalidRange==o.invalidRange && invalidated==o.invalidated &&
               declined==o.declined && joined==o.joined && epochDelta==o.epochDelta && keys==o.keys && pools==o.pools;
    }
};
inline void runCopier(ID3D11Device* device,Check check) {
    D3D11_BUFFER_DESC d{};d.ByteWidth=4*336;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.StructureByteStride=336;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    Microsoft::WRL::ComPtr<ID3D11Buffer> poolA,poolB;
    if(!device || FAILED(device->CreateBuffer(&d,nullptr,&poolA)) || FAILED(device->CreateBuffer(&d,nullptr,&poolB))){check(false,"copier: pool fixtures");return;}
    Record src[4]={record(10),record(20),record(30),record(40)},mappedA[4]={src[0],src[1],src[2],src[3]},mappedB[4]={src[0],src[1],src[2],src[3]};
    const uintptr_t baseA=reinterpret_cast<uintptr_t>(mappedA),baseB=reinterpret_cast<uintptr_t>(mappedB),source=addressOf(src[3]);
    auto mapA=[&](uint64_t seq=1){return copy::beginMap(poolA.Get(),mappedA,sizeof(mappedA),336,D3D11_MAP_WRITE_DISCARD,seq,kFrame);};
    auto mapB=[&](uintptr_t base){return copy::beginMap(poolB.Get(),reinterpret_cast<void*>(base),sizeof(mappedB),336,D3D11_MAP_WRITE_DISCARD,1,kFrame);};
    // Production copy vs the pre-change copier, on state the same setup built: everything either leaves behind must match.
    auto capture=[&](uint64_t epoch){
        CopierOutcome o;const auto s=copy::stats();
        o.copies=s.copies;o.noLease=s.copierNoLease;o.ambiguous=s.copierAmbiguous;o.invalidRange=s.copierInvalidRange;
        o.invalidated=s.invalidated;o.declined=s.declined;o.joined=s.joined;o.epochDelta=copy::g_emissionEpoch-epoch;
        for(const auto& e:copy::g_emissions)o.keys.push_back(e.first);std::sort(o.keys.begin(),o.keys.end());
        for(auto* buffer:{poolA.Get(),poolB.Get()}) {
            const auto it=copy::g_pools.find(buffer);
            if(it==copy::g_pools.end()){o.pools.push_back({0});continue;}
            std::vector<uint64_t> state={1,it->second.active?1u:0u,it->second.valid?1u:0u};
            std::vector<uint64_t> slots;for(const auto& p:it->second.patches)slots.push_back(p.first);std::sort(slots.begin(),slots.end());
            state.insert(state.end(),slots.begin(),slots.end());o.pools.push_back(state);
        }
        return o;
    };
    auto equivalent=[&](const char* name,auto&& setup,auto&& call,bool lockFree) {
        CopierOutcome outcome[2];uint64_t lockedCopies=0;
        for(int reference=1;reference>=0;--reference) {
            copy::reset();for(unsigned i=0;i<4;++i){mappedA[i]=src[i];mappedB[i]=src[i];}
            setup();const auto epoch=copy::g_emissionEpoch;const auto before=copy::g_stats.copies;
            call(reference!=0);
            lockedCopies=copy::g_stats.copies-before;
            check(published(),(std::string("copier equivalence (")+name+"): the published flag and view stay exact").c_str());
            outcome[reference]=capture(epoch);
        }
        check(outcome[0]==outcome[1],(std::string("copier equivalence (")+name+"): same counters, claims, patches, lease state and epoch as the locked copier").c_str());
        check((lockedCopies==0)==lockFree,(std::string("copier equivalence (")+name+(lockFree?"): handled without the mutex":"): handled under the mutex")).c_str());
    };
    auto copyOf=[&](bool reference,uintptr_t base,uint32_t stride,uintptr_t from,uint32_t first,uint32_t count){
        if(reference)referenceCopier(base,stride,from,first,count,kFrame);else copy::copier(base,stride,from,first,count,kFrame);
    };
    auto join=[&](unsigned slot){   // a production join: the emission for src[slot] copied into slot `slot` of lease A
        mappedA[slot]=src[slot];claim(src[slot],kFrame);copy::copier(baseA,336,addressOf(src[slot]),slot,1,kFrame);
    };
    equivalent("no lease",[&]{},[&](bool r){copyOf(r,baseA,336,source,0,1);},true);
    equivalent("count zero",[&]{mapA();},[&](bool r){copyOf(r,baseA,336,source,0,0);},true);
    equivalent("one lease that fits",[&]{mapA();},[&](bool r){copyOf(r,baseA,336,source,0,4);},true);
    equivalent("the last slot of the lease",[&]{mapA();},[&](bool r){copyOf(r,baseA,336,source,3,1);},true);
    equivalent("a range past the lease",[&]{mapA();},[&](bool r){copyOf(r,baseA,336,source,3,2);},false);
    equivalent("a first slot past the lease",[&]{mapA();},[&](bool r){copyOf(r,baseA,336,source,5,1);},false);
    equivalent("a stride that is not a record",[&]{mapA();},[&](bool r){copyOf(r,baseA,320,source,0,1);},false);
    equivalent("a lease that ended",[&]{mapA();copy::endMap(poolA.Get(),1);},[&](bool r){copyOf(r,baseA,336,source,0,1);},true);
    equivalent("another base",[&]{mapA();},[&](bool r){copyOf(r,baseB,336,source,0,1);},true);
    equivalent("two leases at one base",[&]{mapA();mapB(baseA);},[&](bool r){copyOf(r,baseA,336,source,0,1);},false);
    equivalent("a patch in the range",[&]{mapA();join(1);},[&](bool r){copyOf(r,baseA,336,source,0,2);},false);
    equivalent("a patch outside the range",[&]{mapA();join(1);},[&](bool r){copyOf(r,baseA,336,source,2,2);},true);   // the claim check (round 6) hands it back
    equivalent("a patch in a lease that ended",[&]{mapA();join(1);copy::endMap(poolA.Get(),1);},[&](bool r){copyOf(r,baseB,336,source,0,1);},true);
    equivalent("a claim that joins",[&]{mapA();mappedA[2]=src[2];claim(src[2],kFrame);},[&](bool r){copyOf(r,baseA,336,addressOf(src[2]),2,1);},false);
    equivalent("a claim with no lease",[&]{claim(src[2],kFrame);},[&](bool r){copyOf(r,baseA,336,addressOf(src[2]),2,1);},false);
    equivalent("a claim elsewhere",[&]{mapA();claim(src[0],kFrame);},[&](bool r){copyOf(r,baseA,336,source,0,1);},true);   // the claim check (round 6) hands it back
    // Round 6: the copier's claim check, against the frozen locked copier, on every shape of claim, patch and lease around the range.
    static Record extra[3]={record(50),record(51),record(52)};
    equivalent("a claim elsewhere, no lease",[&]{claim(src[0],kFrame);},[&](bool r){copyOf(r,baseB,336,source,0,1);},true);
    equivalent("a claim just below the range",[&]{mapA();claim(src[2],kFrame);},[&](bool r){copyOf(r,baseA,336,source,3,1);},true);
    equivalent("a claim just past the range",[&]{mapA();claim(src[3],kFrame);},[&](bool r){copyOf(r,baseA,336,addressOf(src[2]),2,1);},true);
    equivalent("a claim in the range's second record",[&]{mapA();mappedA[3]=src[3];claim(src[3],kFrame);},[&](bool r){copyOf(r,baseA,336,addressOf(src[2]),2,2);},false);
    equivalent("a claim in the range's first record",[&]{mapA();mappedA[2]=src[2];claim(src[2],kFrame);},[&](bool r){copyOf(r,baseA,336,addressOf(src[2]),2,2);},false);
    equivalent("a claim between two records of the range",[&]{mapA();claim(extra[0],kFrame);},[&](bool r){copyOf(r,baseA,336,addressOf(extra[0])-8,0,1);},true);
    equivalent("four claims elsewhere",[&]{mapA();claim(src[0],kFrame);claim(src[1],kFrame);claim(extra[0],kFrame);claim(extra[1],kFrame);},
               [&](bool r){copyOf(r,baseA,336,source,0,1);},true);
    equivalent("five claims elsewhere",[&]{mapA();claim(src[0],kFrame);claim(src[1],kFrame);claim(extra[0],kFrame);claim(extra[1],kFrame);claim(extra[2],kFrame);},
               [&](bool r){copyOf(r,baseA,336,source,0,1);},false);
    equivalent("four claims, the fourth in the range",[&]{mapA();mappedA[3]=src[3];claim(src[0],kFrame);claim(src[1],kFrame);claim(extra[0],kFrame);claim(src[3],kFrame);},
               [&](bool r){copyOf(r,baseA,336,source,3,1);},false);
    equivalent("a patch below the range, a claim elsewhere",[&]{mapA();join(0);claim(src[1],kFrame);},[&](bool r){copyOf(r,baseA,336,source,3,1);},true);
    equivalent("a patch at the range's last slot, a claim elsewhere",[&]{mapA();join(2);claim(src[0],kFrame);},[&](bool r){copyOf(r,baseA,336,source,1,2);},false);
    equivalent("a patch at the range's first slot, a claim elsewhere",[&]{mapA();join(1);claim(src[0],kFrame);},[&](bool r){copyOf(r,baseA,336,source,1,2);},false);
    equivalent("a patch just past the range, a claim elsewhere",[&]{mapA();join(2);claim(src[0],kFrame);},[&](bool r){copyOf(r,baseA,336,source,0,2);},true);
    equivalent("a patch, a claim elsewhere, a range past the lease",[&]{mapA();join(0);claim(src[1],kFrame);},[&](bool r){copyOf(r,baseA,336,source,3,2);},false);
    equivalent("a patch, a claim elsewhere, two leases at the base",[&]{mapA();mapB(baseA);join(0);claim(src[1],kFrame);},[&](bool r){copyOf(r,baseA,336,source,3,1);},false);
    equivalent("a patch in another pool, a claim elsewhere",[&]{mapA();mapB(baseB);mappedB[1]=src[1];claim(src[1],kFrame);
                   copy::copier(baseB,336,addressOf(src[1]),1,1,kFrame);claim(src[0],kFrame);},[&](bool r){copyOf(r,baseA,336,source,0,2);},true);
    equivalent("a plan in flight and a claim elsewhere",[&]{
        static Tree detached(1,1);static Destination dst;
        mapA();detached.item(0,0,0)=record(5);claim(detached.item(0,0,0),kFrame);claim(src[0],kFrame);
        copy::beginMergeOpaque(dst.ptr(),detached.anchor(0),kFrame);   // the detached claim is staged into the plan; src[0] stays a claim
    },[&](bool r){copyOf(r,baseA,336,source,0,1);},false);
    equivalent("a detached merge plan in flight, no emission",[&]{
        static Tree detached(1,1);static Destination dst;
        mapA();detached.item(0,0,0)=record(5);claim(detached.item(0,0,0),kFrame);
        copy::beginMergeOpaque(dst.ptr(),detached.anchor(0),kFrame);   // the claim is staged into the plan: only the plan is live
    },[&](bool r){copyOf(r,baseA,336,source,0,1);},true);   // the copier never reads a plan; the plan's claims are not in g_emissions
    equivalent("a count over the bound, no claims",[&]{mapA();},[&](bool r){copyOf(r,baseA,336,source,0,copy::kMaxPatches+1);},false);
    equivalent("a count over the bound, a claim",[&]{mapA();claim(src[0],kFrame);},[&](bool r){copyOf(r,baseA,336,source,0,copy::kMaxPatches+1);},false);
    equivalent("a null source",[&]{mapA();},[&](bool r){copyOf(r,baseA,336,0,0,1);},false);
    equivalent("a source whose range wraps",[&]{mapA();},[&](bool r){copyOf(r,baseA,336,UINTPTR_MAX-100,0,1);},false);
    {   // The patch that exists is erased by a copy over its slot, and the erase is counted: not skipped.
        copy::reset();mapA();join(1);
        check(copy::g_poolView.patches.load() && published(),"copier: a join publishes that a pool holds a patch");
        const auto before=copy::stats().invalidated;
        copy::copier(baseA,336,source,1,1,kFrame);
        check(copy::stats().invalidated==before+1 && copy::g_pools.at(poolA.Get()).patches.empty() && !copy::g_poolView.patches.load() && published(),
              "copier: a copy over a patched slot still erases the patch and counts it, then the pools are quiet again");
        copy::reset();
    }
    {   // The re-check after the reads: a lease that ends between the reads and the re-check must not count as a lease found.
        copy::reset();mapA();
        static ID3D11Buffer* hookPool;hookPool=poolA.Get();
        copy::g_copierMidRead=+[]() noexcept {copy::endMap(hookPool,1);};
        copy::g_claimPrecheck=false;   // this case is the quiet exit's own re-check; the claim check, which would then hand the call back, is C11
        copy::copier(baseA,336,source,0,4,kFrame);
        copy::g_claimPrecheck=true;
        copy::g_copierMidRead=nullptr;
        check(copy::g_stats.copies==1 && copy::g_stats.copierNoLease==1 && copy::g_copierSkipped.load()==0 && published(),
              "copier: a view that moved between the reads and the re-check sends the copy to the mutex, which then finds no lease");
        copy::reset();
    }
    {   // Counters fold: skipped copies are copies, and a skipped copy with no lease is a no-lease copy.
        copy::reset();mapA();
        for(unsigned i=0;i<10;++i)copy::copier(baseA,336,source,0,4,kFrame);
        for(unsigned i=0;i<3;++i)copy::copier(baseB,336,source,0,1,kFrame);
        auto s=copy::stats();
        check(s.copies==13 && s.copierNoLease==3 && copy::g_stats.copies==0,"copier: lock-free copies are counted as copies and as no-lease copies where they found no lease");
        copy::reset();s=copy::stats();check(s.copies==0 && s.copierNoLease==0,"copier: reset zeroes the lock-free copy counters");
    }
    {   // No heap on the skipped path.
        copy::reset();mapA();rig_allocations::start();
        for(unsigned i=0;i<256;++i){copy::copier(baseA,336,source,0,4,kFrame);copy::copier(baseB,336,source,0,1,kFrame);}
        check(rig_allocations::stop()==0,"copier: the lock-free exit allocates nothing");
        copy::reset();
    }
    {   // Who waits: a skipped copy does not, a copy with a claim or a patch does.
        struct Holder {
            std::thread thread;std::atomic<bool> locked{false},release{false};
            void start(){thread=std::thread([this]{std::unique_lock<std::mutex> hold(copy::g_mutex);locked.store(true);while(!release.load())std::this_thread::yield();});
                         while(!locked.load())std::this_thread::yield();}
            void stop(){release.store(true);thread.join();}
        };
        auto finishesWhileHeld=[&](auto&& fn,unsigned waitMs)->bool {
            Holder holder;holder.start();std::atomic<bool> done{false};std::thread worker([&]{fn();done.store(true);});
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(waitMs);
            while(!done.load() && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const bool finished=done.load();holder.stop();worker.join();return finished;
        };
        copy::reset();mapA();
        check(finishesWhileHeld([&]{copy::copier(baseA,336,source,0,4,kFrame);},5000),"mutex: a quiet copy over a fitting lease returns while another thread holds g_mutex");
        check(finishesWhileHeld([&]{copy::copier(baseB,336,source,0,1,kFrame);},5000),"mutex: a quiet copy with no lease returns while another thread holds g_mutex");
        check(finishesWhileHeld([&]{copy::copier(baseA,336,source,0,0,kFrame);},5000),"mutex: a copy of nothing returns while another thread holds g_mutex");
        check(!finishesWhileHeld([&]{copy::copier(baseA,336,source,3,2,kFrame);},150),"mutex: a copy past the lease blocks until g_mutex is released");
        copy::reset();mapA();join(1);
        check(finishesWhileHeld([&]{copy::copier(baseA,336,source,2,1,kFrame);},5000),"mutex: a copy beside a patch it does not meet returns while another thread holds g_mutex");
        check(!finishesWhileHeld([&]{copy::copier(baseA,336,source,1,1,kFrame);},150),"mutex: a copy over a pool's patch blocks until g_mutex is released");
        copy::reset();mapA();claim(src[0],kFrame);
        check(finishesWhileHeld([&]{copy::copier(baseA,336,source,2,1,kFrame);},5000),"mutex: a copy beside a live claim it does not hold returns while another thread holds g_mutex");
        check(!finishesWhileHeld([&]{copy::copier(baseA,336,addressOf(src[0]),0,1,kFrame);},150),"mutex: a copy of a live claim blocks until g_mutex is released");
        check(published(),"mutex: the blocked copies completed afterwards and left the state exact");
        copy::reset();
    }
    {   // Four threads over the same pools: leases opening and closing, joins, quiet copies, claim traffic.
        constexpr unsigned kRounds=3000;copy::reset();std::atomic<bool> go{false};std::vector<std::thread> pool;
        Record own[2]={record(61),record(62)};
        pool.emplace_back([&]{   // the map owner
            while(!go.load())std::this_thread::yield();
            for(unsigned i=0;i<kRounds;++i){
                mapA(i+1);mappedA[1]=src[1];claim(src[1],kFrame);copy::copier(baseA,336,addressOf(src[1]),1,1,kFrame);
                copy::copier(baseA,336,source,0,4,kFrame);copy::endMap(poolA.Get(),i+1);
            }
        });
        for(unsigned t=0;t<2;++t)pool.emplace_back([&,t]{
            while(!go.load())std::this_thread::yield();
            for(unsigned i=0;i<kRounds*4;++i)copy::copier(t?baseB:baseA,336,source,i&3,1+(i&1),kFrame);
        });
        pool.emplace_back([&]{
            while(!go.load())std::this_thread::yield();
            for(unsigned i=0;i<kRounds*2;++i){if(i&1)claim(own[0],kFrame);else copy::invalidateEmission(addressOf(own[0]));}
        });
        go.store(true);for(auto& th:pool)th.join();
        check(published(),"copier mixed traffic: leases, joins, quiet copies and claims from four threads leave the published state exact");
        copy::reset();
    }
}
// The lock instrument: the sampling arithmetic, that an unsampled acquisition reads no clock, the thread classes, the
// maxima, the relabelled stale skip, every caller reaching its own cell, and the exact text of the two log lines.
namespace lockclock {
inline int64_t now=0;inline unsigned reads=0;
inline int64_t fixed() noexcept {++reads;return now+=100;}   // wait 100 ticks, hold 100 ticks per sampled acquisition
inline const int64_t* script=nullptr;inline size_t scriptAt=0;
inline int64_t scripted() noexcept {++reads;return script[scriptAt++];}
// Records, at each read, whether another thread finds g_mutex held: the three reads must see free, held, held.
inline std::vector<int> mutexHeld;
inline int64_t probing() noexcept {
    bool owned=false;
    std::thread([&]{owned=!edvr::engine_velocity_primary_copy::g_mutex.try_lock();if(!owned)edvr::engine_velocity_primary_copy::g_mutex.unlock();}).join();
    mutexHeld.push_back(owned?1:0);return int64_t(mutexHeld.size())*100;
}
}
// A plan that holds nothing must not keep the merge and copier paths locked. The chain the review found: one emission sets the
// flag, an unrelated merge registers a (transfer-less) plan, the emission is consumed, and that plan kept the one flag true, so
// every merge that began meanwhile registered another plan, every clear walked and every copy took the mutex.
inline void runPlansHoldingNothing(ID3D11Device* device,Check check) {
    D3D11_BUFFER_DESC d{};d.ByteWidth=2*336;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.StructureByteStride=336;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    Microsoft::WRL::ComPtr<ID3D11Buffer> pool;
    if(!device || FAILED(device->CreateBuffer(&d,nullptr,&pool))){check(false,"plans: pool fixture");return;}
    Record e=record(9),mapped[2]={record(1),record(2)},source=record(3);
    const uintptr_t base=reinterpret_cast<uintptr_t>(mapped);
    Tree unrelated(1,1),other(1,1);Destination dstU,dstO;
    unrelated.item(0,0,0)=record(4);other.item(0,0,0)=record(5);
    auto lockedCalls=[]{return copy::g_stats.mergeCalls+copy::g_stats.clearCalls+copy::g_stats.copies;};
    copy::reset();
    copy::beginMap(pool.Get(),mapped,sizeof(mapped),336,D3D11_MAP_WRITE_DISCARD,1,kFrame);
    claim(e,kFrame);
    check(copy::g_emissionsLive.load() && copy::g_claimsLive.load() && published(),"plans: one emission sets both flags");
    // A claim sits in the merged list, but of another frame than the merge's: the merge touches it (so the claim check cannot hand
    // out the token) and stages nothing from it. That is the plan that holds nothing.
    claim(unrelated.item(0,0,0),kFrame);
    void* plan=copy::beginMergeOpaque(dstU.ptr(),unrelated.anchor(0),kFrame+1);
    check(plan!=copy::mergeNoClaimsToken() && copy::g_mergePlans.size()==1 && static_cast<copy::MergePlan*>(plan)->transfers.empty(),
          "plans: with an emission live, a merge that touches a claim of another frame registers a plan that stages nothing");
    copy::copier(base,336,addressOf(e),0,1,kFrame);   // consumes the emission, under the lock (an emission is live)
    check(!copy::g_emissionsLive.load() && copy::g_claimsLive.load() && copy::g_mergePlans.size()==1 && published(),
          "plans: the emission consumed, the plan alone keeps the clear flag and the emissions flag is false");
    const auto locked=lockedCalls(),mergeFree=copy::g_mergeSkipped.load(),copyFree=copy::g_copierSkipped.load();
    void* second=copy::beginMergeOpaque(dstO.ptr(),other.anchor(0),kFrame);
    copy::copier(base,336,addressOf(source),0,1,kFrame);
    check(second==copy::mergeNoClaimsToken() && copy::g_mergePlans.size()==1 && lockedCalls()==locked &&
          copy::g_mergeSkipped.load()==mergeFree+1 && copy::g_copierSkipped.load()==copyFree+1,
          "plans: while that plan is in flight a new merge and a copy both skip the mutex, and no second plan is registered");
    copy::endMergeOpaque(second,true);
    const auto walked=copy::stats().clearNodes;copy::invalidateDictionary(unrelated.ptr());
    check(copy::stats().clearNodes>walked,"plans: a clear still walks while a plan is in flight (it must be able to mark that plan invalid)");
    copy::endMergeOpaque(plan,true);
    check(!copy::g_claimsLive.load() && !copy::g_emissionsLive.load() && copy::g_mergePlans.empty() && published(),"plans: when the plan ends both flags are false");
    const auto afterPlan=lockedCalls();
    copy::endMergeOpaque(copy::beginMergeOpaque(dstO.ptr(),other.anchor(0),kFrame),true);
    copy::invalidateDictionary(other.ptr());copy::copier(base,336,addressOf(source),0,1,kFrame);
    check(lockedCalls()==afterPlan,"plans: afterwards a merge, a clear and a copy all take the lock-free paths");
    {   // The flag may be one release behind (stale-true): under the lock the rule is the same, and the release corrects it.
        copy::reset();copy::g_emissionsLive.store(true);copy::g_claimsLive.store(true);
        void* token=copy::beginMergeOpaque(dstU.ptr(),unrelated.anchor(0),kFrame);
        check(token==copy::mergeNoClaimsToken() && copy::g_mergePlans.empty() && copy::g_stats.mergeCalls==1 && copy::g_stats.mergeWithoutClaims==1 && published(),
              "plans: a merge that finds the emissions flag stale-true gets the token under the lock, and the lock's release corrects the flags");
        copy::endMergeOpaque(token,true);
    }
    {   // The same with a claim-carrying plan in flight and four threads: while it is open, merges and copies stay off the mutex.
        constexpr unsigned kRounds=3000;
        copy::reset();copy::beginMap(pool.Get(),mapped,sizeof(mapped),336,D3D11_MAP_WRITE_DISCARD,1,kFrame);
        Tree heldTree(1,1);Destination dstH;heldTree.item(0,0,0)=record(6);claim(heldTree.item(0,0,0),kFrame);claim(e,kFrame);
        std::atomic<bool> open{false},release{false};void* heldPlan=nullptr;
        std::thread holder([&]{
            heldPlan=copy::beginMergeOpaque(dstH.ptr(),heldTree.anchor(0),kFrame);   // the emission in `held` is staged into this plan
            open.store(true);while(!release.load())std::this_thread::yield();
            copy::endMergeOpaque(heldPlan,true);
        });
        while(!open.load())std::this_thread::yield();
        copy::copier(base,336,addressOf(e),0,1,kFrame);   // consumes the last emission: only the plan is live now
        check(!copy::g_emissionsLive.load() && copy::g_claimsLive.load() && copy::g_mergePlans.size()==1,"plans: four threads: only the held plan is live");
        const auto mergeCalls=copy::g_stats.mergeCalls,copies=copy::g_stats.copies;
        std::vector<std::unique_ptr<Tree>> trees;std::vector<std::unique_ptr<Destination>> dsts;
        for(unsigned t=0;t<2;++t){trees.push_back(std::make_unique<Tree>(1,1));dsts.push_back(std::make_unique<Destination>());}
        std::vector<std::thread> workers;
        for(unsigned t=0;t<2;++t)workers.emplace_back([&,t]{for(unsigned i=0;i<kRounds;++i)copy::endMergeOpaque(copy::beginMergeOpaque(dsts[t]->ptr(),trees[t]->anchor(0),kFrame),true);});
        workers.emplace_back([&]{for(unsigned i=0;i<kRounds;++i)copy::copier(base,336,addressOf(source),0,1,kFrame);});
        for(auto& w:workers)w.join();
        check(copy::g_stats.mergeCalls==mergeCalls && copy::g_stats.copies==copies && copy::g_mergePlans.size()==1,
              "plans: four threads: 6000 merges and 3000 copies beside an open plan took the mutex 0 times and registered no plan");
        release.store(true);holder.join();
        check(copy::g_mergePlans.empty() && held(addressOf(heldTree.item(0,0,0))) && copy::g_emissionsLive.load() && published(),
              "plans: four threads: the plan's end returns the claim it staged, and the flags follow it");
        copy::invalidateEmission(addressOf(heldTree.item(0,0,0)));
        check(!copy::g_claimsLive.load() && !copy::g_emissionsLive.load() && published(),"plans: four threads: and when that claim goes everything is quiet");
    }
    copy::reset();
}
// The exit line: the share of calls that never took the mutex, pinned, and fed by the real paths.
inline void runExitLine(ID3D11Device* device,Check check) {
    copy::Stats s;s.clearLockFree=7;s.clearCalls=10;s.mergeLockFree=3;s.mergeCalls=4;s.copiesLockFree=100;s.copies=120;s.copierNoLeaseLockFree=5;
    char line[copy::kExitLineBytes];copy::formatExitLine(s,line);
    check(std::string(line)=="engine motion: primary copy lock-free exits cumulative (already counted in the figures above; these calls never took the mutex): "
                             "clear 7 of 10 calls, merge 3 of 4 calls, copier 100 of 120 calls (5 of those found no active map). "
                             "Claim check (live claims, none touched): 0 clears and 0 merges of those, 0 calls declined it. "
                             "Copier claim check (claims live or patches held, none in the range): 0 copies of those, 0 copies declined it.","exit line: the text, exactly");
    s.clearPrechecked=4;s.mergePrechecked=2;s.precheckDeclined=9;s.copierPrechecked=11;s.copierPrecheckDeclined=3;copy::formatExitLine(s,line);
    check(std::string(line).find("Claim check (live claims, none touched): 4 clears and 2 merges of those, 9 calls declined it.")!=std::string::npos,"exit line: the claim check figures");
    check(std::string(line).find("Copier claim check (claims live or patches held, none in the range): 11 copies of those, 3 copies declined it.")!=std::string::npos,"exit line: the copier claim check figures");
    copy::Stats big;big.clearLockFree=big.clearCalls=big.mergeLockFree=big.mergeCalls=big.copiesLockFree=big.copies=big.copierNoLeaseLockFree=big.clearPrechecked=big.mergePrechecked=big.precheckDeclined=big.copierPrechecked=big.copierPrecheckDeclined=~0ull;
    copy::formatExitLine(big,line);
    check(std::strlen(line)<copy::kExitLineBytes-1,"exit line: at 20-digit counters it fits its buffer whole");
    D3D11_BUFFER_DESC d{};d.ByteWidth=336;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.StructureByteStride=336;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    Microsoft::WRL::ComPtr<ID3D11Buffer> pool;Record mapped[1]={record(1)},source=record(2);
    if(!device || FAILED(device->CreateBuffer(&d,nullptr,&pool))){check(false,"exit line: pool fixture");return;}
    Tree tree(1,1);Destination dst;
    copy::reset();copy::beginMap(pool.Get(),mapped,sizeof(mapped),336,D3D11_MAP_WRITE_DISCARD,1,kFrame);
    for(unsigned i=0;i<10;++i)copy::invalidateDictionary(tree.ptr());
    for(unsigned i=0;i<4;++i)copy::endMergeOpaque(copy::beginMergeOpaque(dst.ptr(),tree.anchor(0),kFrame),true);
    for(unsigned i=0;i<6;++i)copy::copier(reinterpret_cast<uintptr_t>(mapped),336,addressOf(source),0,1,kFrame);
    for(unsigned i=0;i<3;++i)copy::copier(reinterpret_cast<uintptr_t>(mapped)+4096,336,addressOf(source),0,1,kFrame);   // no lease at that base
    claim(source,kFrame);copy::invalidateDictionary(tree.ptr());   // a live claim the tree does not hold: the claim check handles it
    tree.item(0,0,0)=record(3);claim(tree.item(0,0,0),kFrame);copy::invalidateDictionary(tree.ptr());   // one locked clear: it holds a claim
    const auto st=copy::stats();
    check(st.clearLockFree==11 && st.clearPrechecked==1 && st.clearCalls==12 && st.mergeLockFree==4 && st.mergeCalls==4 && st.copiesLockFree==9 && st.copies==9 &&
          st.copierNoLeaseLockFree==3 && st.copierNoLease==3,"exit line: the lock-free counts come from the real exits and sit inside the totals the other lines print");
    {   // Review 49f28838 F2: the lock-free counters advance outside the mutex, so each must be read ONCE per snapshot. With no locked copy in
        // the state (nothing but lock-free exits), every snapshot taken while a thread keeps counting must still have the lock-free figure
        // equal to its total; two loads per counter let a copy land between them and put the figure above the total.
        copy::reset();
        std::atomic<bool> stop{false};
        std::thread counter([&]{while(!stop.load(std::memory_order_relaxed)){
            copy::g_copierSkipped.fetch_add(1,std::memory_order_relaxed);copy::g_copierNoLeaseSkipped.fetch_add(1,std::memory_order_relaxed);}});
        unsigned torn=0;
        for(unsigned i=0;i<400000;++i){const auto s=copy::stats();
            if(s.copiesLockFree!=s.copies || s.copierNoLeaseLockFree!=s.copierNoLease)++torn;}
        stop.store(true);counter.join();
        if(torn)std::printf("info: %u of 400000 snapshots had a lock-free copier count different from its total\n",torn);
        check(torn==0,"stats(): a snapshot taken beside a counting thread reads each lock-free counter once, so the figure equals its total");
    }
    copy::reset();
}inline void formatBoth(const copy::LockWindow& w,double perUs,double seconds,char (&lines)[2][copy::kLockLineBytes]){for(unsigned half=0;half<2;++half)copy::formatLockLine(w,half,perUs,seconds,lines[half]);}
inline void runLock(ID3D11Device* device,Check check) {
    namespace emcpu=edvr::emcpu;
    using Caller=copy::Caller;
    constexpr unsigned kEmit=unsigned(Caller::Emit),kOther=unsigned(Caller::Other);
    auto drain=[]{return copy::takeLockWindow();};
    Record item=record(1);const uintptr_t where=addressOf(item);
    const auto savedGate=emcpu::g_gate.load();emcpu::g_gate.store(0);   // no render thread unless a case says so
    const auto savedRenderTid=emcpu::g_renderThreadId.load();emcpu::g_renderThreadId.store(0);
    drain();
    {   // Sampling arithmetic: any 6400 consecutive acquisitions of one caller on one thread are exactly 100 samples.
        for(unsigned i=0;i<6400;++i)copy::invalidateEmission(where);
        for(unsigned i=0;i<128;++i)copy::patches(nullptr,kFrame);
        const auto w=drain();
        check(w.n[kEmit][1]==100 && w.n[kOther][1]==2 && w.n[kEmit][0]==0,"lock instrument: one acquisition in 64 per caller is sampled, each caller counted on its own");
    }
    {   // An unsampled acquisition reads no clock; a sampled one reads it exactly three times.
        copy::g_lockClock=&lockclock::fixed;lockclock::reads=0;lockclock::now=0;
        for(unsigned i=0;i<640;++i)copy::invalidateEmission(where);
        copy::g_lockClock=&emcpu::qpcNow;
        const auto w=drain();
        check(lockclock::reads==30 && w.n[kEmit][1]==10,"lock instrument: 640 acquisitions read the clock 30 times, three per sampled one, none for the other 630");
        check(w.wait[kEmit][1]==1000 && w.hold[kEmit][1]==1000 && w.waitMax[kEmit][1]==100 && w.holdMax[kEmit][1]==100,
              "lock instrument: wait is the acquire interval and hold the interval to the release");
    }
    {   // Where the three reads fall: before the acquire (mutex free), after it (held), before the release (still held).
        drain();for(auto& tick:copy::t_lockTick)tick=63;
        lockclock::mutexHeld.clear();copy::g_lockClock=&lockclock::probing;
        copy::invalidateEmission(where);
        copy::g_lockClock=&emcpu::qpcNow;drain();
        check(lockclock::mutexHeld==std::vector<int>({0,1,1}),"lock instrument: the clock is read before the acquire, after it, and before the release");
    }
    {   // Maxima and sums from a script: two sampled acquisitions, waits 100 and 500 ticks, holds 100 and 300.
        static const int64_t ticks[]={0,100,200, 1000,1500,1800};
        drain();
        for(auto& tick:copy::t_lockTick)tick=63;
        lockclock::script=ticks;lockclock::scriptAt=0;lockclock::reads=0;copy::g_lockClock=&lockclock::scripted;
        copy::invalidateEmission(where);                      // 64th: sampled, reads 0,100,200
        for(unsigned i=0;i<63;++i)copy::invalidateEmission(where);
        copy::invalidateEmission(where);                      // 128th: sampled, reads 1000,1500,1800
        copy::g_lockClock=&emcpu::qpcNow;
        const auto w=drain();
        check(lockclock::reads==6 && w.n[kEmit][1]==2 && w.wait[kEmit][1]==600 && w.hold[kEmit][1]==400 && w.waitMax[kEmit][1]==500 && w.holdMax[kEmit][1]==300,
              "lock instrument: sums and maxima over two sampled acquisitions");
    }
    {   // Thread classes: the render thread is the one named in g_renderThreadId; any other is not. The gate is NOT consulted: flat
        // names nobody (kNoThread) in the gate on 15 frames in 16, and 0 on an idle census, and the render thread is the render
        // thread all the same.
        const auto savedTid=emcpu::g_renderThreadId.load();
        emcpu::g_renderThreadId.store(emcpu::currentThreadId());
        emcpu::g_gate.store(0xFFFFFFFFull);   // flat's kNoThread frame
        for(auto& tick:copy::t_lockTick)tick=63;
        copy::invalidateEmission(where);
        std::thread other([&]{for(unsigned i=0;i<64;++i)copy::invalidateEmission(where);});other.join();
        auto w=drain();
        check(w.n[kEmit][0]==1 && w.n[kEmit][1]==1,"lock instrument: with the gate at nobody, a sampled acquisition on the render thread is still render, one elsewhere is other");
        emcpu::g_gate.store(emcpu::currentThreadId());   // the gate names this thread, but the census says another is the render thread
        emcpu::g_renderThreadId.store(emcpu::currentThreadId()+1);
        for(auto& tick:copy::t_lockTick)tick=63;
        copy::invalidateEmission(where);
        w=drain();
        check(w.n[kEmit][0]==0 && w.n[kEmit][1]==1,"lock instrument: the render column follows g_renderThreadId, not the gate");
        emcpu::g_renderThreadId.store(0);   // an idle census: nobody is the render thread
        emcpu::g_gate.store(0);
        for(auto& tick:copy::t_lockTick)tick=63;
        copy::invalidateEmission(where);
        w=drain();
        check(w.n[kEmit][0]==0 && w.n[kEmit][1]==1,"lock instrument: with no render thread published every sample is other");
        emcpu::g_renderThreadId.store(savedTid);
    }
    {   // A clear that finds the flag stale-true is a skip, filed under its own name.
        copy::reset();drain();copy::g_claimsLive.store(true);
        for(auto& tick:copy::t_lockTick)tick=63;
        copy::invalidateDictionary(where);
        const auto w=drain();
        check(w.n[unsigned(Caller::ClearSkip)][1]==1 && w.n[unsigned(Caller::ClearWalk)][1]==0,"lock instrument: a locked clear with nothing to walk is filed as a clear skip");
        copy::reset();
    }
    {   // Every caller reaches its own cell through the real entry points (a dead instrument would read zero everywhere).
        copy::reset();drain();
        Tree tree(1,1);Destination dst;tree.item(0,0,0)=record(2);
        D3D11_BUFFER_DESC d{};d.ByteWidth=672;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.StructureByteStride=336;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        Microsoft::WRL::ComPtr<ID3D11Buffer> pool;Record mapped[2]={record(3),record(4)};
        if(device && SUCCEEDED(device->CreateBuffer(&d,nullptr,&pool))) {
            for(auto& tick:copy::t_lockTick)tick=63;
            claim(tree.item(0,0,0),kFrame);                                                   // emit
            copy::beginMap(pool.Get(),mapped,sizeof(mapped),336,D3D11_MAP_WRITE_DISCARD,1,kFrame);   // beginMap
            copy::copier(reinterpret_cast<uintptr_t>(mapped),336,addressOf(tree.item(0,0,0)),0,1,kFrame);   // copier (a claim is live)
            copy::endMap(pool.Get(),1);                                                        // endMap
            claim(tree.item(0,0,0),kFrame);                                                    // (the copier consumed the first claim)
            copy::invalidateDictionary(tree.ptr());                                            // clear walk (a claim is live)
            claim(tree.item(0,0,0),kFrame);
            void* plan=copy::beginMergeOpaque(dst.ptr(),tree.anchor(0),kFrame);                // merge begin
            copy::endMergeOpaque(plan,true);                                                   // merge end
            copy::patches(pool.Get(),kFrame);                                                  // other
            const auto w=drain();
            bool each=true;
            for(const Caller c:{Caller::MergeBegin,Caller::MergeEnd,Caller::ClearWalk,Caller::Copier,Caller::BeginMap,Caller::EndMap,Caller::Emit,Caller::Other})
                each=each && w.n[unsigned(c)][1]==1;
            check(each && w.n[unsigned(Caller::ClearSkip)][1]==0,"lock instrument: merge begin, merge end, clear walk, copier, beginMap, endMap, emit and other each reach their own cell");
            copy::forget(pool.Get());
        }
        copy::reset();
    }
    {   // The text. One known window, exactly.
        copy::LockWindow w;
        const unsigned mergeBegin=unsigned(Caller::MergeBegin),copier=unsigned(Caller::Copier);
        w.n[mergeBegin][1]=5;w.wait[mergeBegin][1]=5*200;w.waitMax[mergeBegin][1]=500;w.hold[mergeBegin][1]=5*400;w.holdMax[mergeBegin][1]=1000;
        w.n[copier][0]=2;w.wait[copier][0]=2*3000;w.waitMax[copier][0]=4000;w.hold[copier][0]=2*100;w.holdMax[copier][0]=100;
        char lines[2][copy::kLockLineBytes];formatBoth(w,1000.0,30.0,lines);
        check(std::string(lines[0])=="engine motion: primary copy lock (sampled 1 in 64, 30 s, 1 of 2): merge begin: render n=0; other n=5 wait 0.2/0.5 us hold 0.4/1.0 us"
                                    " | merge end: render n=0; other n=0 | clear skip: render n=0; other n=0 | clear walk: render n=0; other n=0"
                                    " | copier: render n=2 wait 3.0/4.0 us hold 0.1/0.1 us; other n=0","lock instrument: line 1 text, exactly");
        check(std::string(lines[1])=="engine motion: primary copy lock (sampled 1 in 64, 30 s, 2 of 2): beginMap: render n=0; other n=0 | endMap: render n=0; other n=0"
                                    " | emit: render n=0; other n=0 | other: render n=0; other n=0","lock instrument: line 2 text, exactly");
        // Worst plausible: a day of samples and a second-long wait on every cell, and the absolute worst, which must truncate cleanly.
        copy::LockWindow big;
        for(unsigned c=0;c<copy::kCallers;++c)for(unsigned k=0;k<2;++k){big.n[c][k]=1000000000000ull;big.wait[c][k]=big.n[c][k]*1000000ull;big.waitMax[c][k]=1000000000ull*1000;
                                                                         big.hold[c][k]=big.n[c][k]*1000000ull;big.holdMax[c][k]=1000000000ull*1000;}
        formatBoth(big,1000.0,1800.0,lines);
        check(std::strlen(lines[0])<1150 && std::strlen(lines[1])<1150,"lock instrument: both lines fit the log line at worst plausible values");
        for(unsigned c=0;c<copy::kCallers;++c)for(unsigned k=0;k<2;++k){big.n[c][k]=~0ull;big.wait[c][k]=~0ull;big.waitMax[c][k]=~0ull;big.hold[c][k]=~0ull;big.holdMax[c][k]=~0ull;}
        formatBoth(big,0.001,1e9,lines);
        check(std::strlen(lines[0])<copy::kLockLineBytes && std::strlen(lines[1])<copy::kLockLineBytes,"lock instrument: absurd values truncate inside the buffer and stay terminated");
    }
    emcpu::g_gate.store(savedGate);emcpu::g_renderThreadId.store(savedRenderTid);drain();
}
inline void run(ID3D11Device* device,Check check) {
    runPlansHoldingNothing(device,check);
    runExitLine(device,check);
    runLock(device,check);
    runCopier(device,check);
    // --- (d) walk equivalence: the production walk and the pre-change walk leave identical state ---
    constexpr Shape shapes[]={
        {"1 node",1,1,nullptr,{0,0,0},{0,0,0},{0,3,0},2,false},
        {"8 nodes (64 addresses, the inline limit)",1,8,nullptr,{0,0,0},{0,3,7},{2,2,2},3,false},
        {"9 nodes (spills at the 65th address)",1,9,nullptr,{0,0,0},{8,0,0},{7,0,0},2,false},
        {"70 nodes",1,70,nullptr,{0,0,0},{0,64,69},{0,3,7},3,false},
        {"70 entries (the entry set spills)",70,1,nullptr,{0,35,69},{0,0,0},{0,4,7},3,false},
        {"empty entry",1,0,nullptr,{0,0,0},{0,0,0},{0,0,0},0,false},
        {"a duplicated node",1,3,cycleThroughEarlierNode,{0,0,0},{0,2,0},{0,1,0},2,true},
        {"a self-linked node",1,2,selfLoop,{0,0,0},{0,0,0},{0,0,0},1,true},
        {"a node count of 9",1,3,countNine,{0,0,0},{0,0,0},{0,0,0},1,true},
        {"an unreadable node",1,2,unreadableNode,{0,0,0},{0,0,0},{0,0,0},1,true},
        {"a bucket count over the bound",1,1,bucketsOverBound,{0,0,0},{0,0,0},{0,0,0},1,true},
        {"a second bucket aliasing the first entry",1,1,aliasedBucket,{0,0,0},{0,0,0},{0,0,0},1,true},
        {"2048 nodes (the node bound)",1,2048,nullptr,{0,0,0},{0,1000,2047},{0,5,7},3,false},
        {"2049 nodes (over the node bound)",1,2049,nullptr,{0,0,0},{0,0,0},{0,0,0},1,true},
        {"16384 entries (the entry bound)",16384,0,nullptr,{0,0,0},{0,0,0},{0,0,0},0,false},
        {"16385 entries (over the entry bound)",16385,0,nullptr,{0,0,0},{0,0,0},{0,0,0},0,true},
    };
    for(const auto& shape:shapes) {
        Tree tree(shape.entries,shape.nodesPerEntry);if(shape.corrupt)shape.corrupt(tree);
        Record other=record(900);
        auto exec=[&](bool reference)->Outcome {
            copy::reset();const auto epoch=copy::g_emissionEpoch;
            for(unsigned k=0;k<shape.claims;++k) {
                Record& r=tree.item(shape.claimEntry[k],shape.claimNode[k],shape.claimSlot[k]);r=record(float(10+k));claim(r,kFrame);
            }
            claim(other,kFrame);
            if(reference)referenceClear(tree.ptr());else copy::invalidateDictionaryImpl<false>(tree.ptr());
            check(published(),"claim flag: the production walk and the reference walk both leave the published flag exact");
            return snapshot(epoch,false);
        };
        const Outcome before=exec(true),after=exec(false);
        const std::string label=std::string("walk equivalence (")+shape.name+"): same claims erased, same counters, same failure, same epoch";
        check(before==after,label.c_str());
        if(shape.expectFail)check(after.keys.empty() && after.clearFailed==1 && after.epochDelta>0,
                                  (std::string("walk equivalence (")+shape.name+"): a refused walk clears every claim and counts one failure").c_str());
        else check(after.keys.size()==1 && after.keys[0]==addressOf(other) && after.clearFailed==0 && after.clearedClaims==shape.claims,
                   (std::string("walk equivalence (")+shape.name+"): exactly the claims in freed nodes erased, the unrelated one kept").c_str());
    }
    {   // A detached plan is marked invalid by a clear that touches it, and only then.
        for(int touching=0;touching<2;++touching) {
            Tree source(1,1),bystander(1,1);Destination dst;
            Outcome results[2];
            for(int reference=1;reference>=0;--reference) {
                copy::reset();const auto epoch=copy::g_emissionEpoch;
                source.item(0,0,0)=record(5);claim(source.item(0,0,0),kFrame);
                void* plan=copy::beginMergeOpaque(dst.ptr(),source.anchor(0),kFrame);
                if(reference)referenceClear((touching?source:bystander).ptr());else copy::invalidateDictionaryImpl<false>((touching?source:bystander).ptr());
                const bool invalid=plan!=copy::mergeNoClaimsToken() && static_cast<copy::MergePlan*>(plan)->invalid;
                results[reference]=snapshot(epoch,invalid);copy::endMergeOpaque(plan,true);
            }
            check(results[0]==results[1] && results[0].planInvalid==(touching==1),
                  touching?"walk equivalence: a clear touching a detached plan marks it invalid, as before":
                           "walk equivalence: a clear elsewhere leaves a detached plan valid, as before");
        }
    }
    {   // The set itself, across the inline limit.
        copy::AddressSet set;bool fresh=true,repeats=true;
        for(uintptr_t v=1;v<=200;++v)fresh=fresh && set.insert(v*16).second && set.size()==v;
        for(uintptr_t v=1;v<=200;++v)repeats=repeats && !set.insert(v*16).second && set.count(v*16)==1;
        uintptr_t sum=0;size_t visits=0;set.forEach([&](uintptr_t v){sum+=v;++visits;});
        check(fresh && repeats && set.size()==200 && set.count(17)==0 && visits==200 && sum==16*200*201/2,
              "address set: insert/size/count/forEach agree across the inline limit and the spill");
        copy::AddressSet inlineOnly;for(uintptr_t v=1;v<=copy::AddressSet::kInline;++v)inlineOnly.insert(v);
        check(inlineOnly.size()==copy::AddressSet::kInline && !inlineOnly.insert(copy::AddressSet::kInline).second && inlineOnly.insert(1000).second &&
              inlineOnly.size()==copy::AddressSet::kInline+1 && inlineOnly.count(1)==1 && inlineOnly.count(1000)==1,
              "address set: the 65th distinct value spills without losing or duplicating an entry");
    }

    // --- (e) no heap allocation where the change promised none ---
    {
        Tree one(1,1),eight(1,8),nine(1,9);Destination dst;Record live=record(7);
        copy::reset();rig_allocations::start();
        for(unsigned i=0;i<64;++i){copy::endMergeOpaque(copy::beginMergeOpaque(dst.ptr(),eight.anchor(0),kFrame),true);copy::invalidateDictionary(eight.ptr());}
        const auto none=rig_allocations::stop();
        check(none==0,"allocation: a no-claim merge and a no-claim clear allocate nothing");
        claim(live,kFrame);
        for(Tree* tree:{&one,&eight}) {
            for(unsigned n=0;n<tree->entries[0]->nodes.size();++n)tree->item(0,n,1)=record(float(50+n));
            rig_allocations::start();for(unsigned i=0;i<64;++i)copy::invalidateDictionary(tree->ptr());const auto walk=rig_allocations::stop();
            check(walk==0,tree==&one?"allocation: a clear walking 1 node with claims present allocates nothing":
                                     "allocation: a clear walking 8 nodes with claims present allocates nothing");
        }
        // The walk that erases claims allocates nothing either (erase frees, it does not request memory).
        claim(eight.item(0,3,1),kFrame);rig_allocations::start();copy::invalidateDictionary(eight.ptr());const auto erasing=rig_allocations::stop();
        check(erasing==0 && !held(addressOf(eight.item(0,3,1))) && held(addressOf(live)),"allocation: an erasing 8-node walk allocates nothing and still revokes the claim");
        rig_allocations::start();copy::invalidateDictionary(nine.ptr());const auto checked=rig_allocations::stop();
        check(checked==0,"allocation: the claim check walking 9 nodes (past the 64-address inline limit of the locked walk) allocates nothing");
        rig_allocations::start();copy::invalidateDictionaryImpl<false>(nine.ptr());const auto spilled=rig_allocations::stop();
        check(spilled>0,"allocation: the counter sees the 9-node full walk spill (the instrument is live)");
        copy::reset();
    }

    // --- (a) the published flag equals the containers after every public entry point ---
    {
        copy::reset();check(published() && !copy::g_claimsLive.load(),"claim flag: false after reset");
        Tree tree(1,2);Destination dst;Record a=record(1),b=record(2);
        tree.item(0,0,0)=record(3);tree.item(0,1,0)=record(4);
        check(claim(a,kFrame) && copy::g_claimsLive.load() && published(),"claim flag: true after recordEmission accepts");
        check(claim(a,kFrame) && published(),"claim flag: exact after a repeat of the same claim");
        const auto p=primary_copy_tests::previous(b);
        check(!copy::recordEmission(addressOf(b),b.data(),p,0xDEADBEEFu,kFrame) && published(),"claim flag: exact after recordEmission declines");
        copy::invalidateEmission(addressOf(a));check(!copy::g_claimsLive.load() && published(),"claim flag: false after invalidateEmission erases the last claim");
        claim(a,kFrame);claim(b,kFrame+1);check(held(addressOf(b)) && !held(addressOf(a)) && published(),"claim flag: exact after a new frame replaces the claims");
        copy::invalidateEmission(0);check(!copy::g_claimsLive.load() && published(),"claim flag: false after invalidateEmission(0) clears");
        copy::reset();claim(a,kFrame);
        { copy::ClaimLock lock;copy::overflow(kFrame);}
        check(!copy::g_claimsLive.load() && published(),"claim flag: false after overflow");
        copy::reset();
        std::vector<Record> many(copy::kMaxEmissions+1,record(300));
        bool all=true;for(auto& r:many)all=claim(r,kFrame+3) && all;
        check(!all && copy::g_emissions.empty() && !copy::g_claimsLive.load() && published(),"claim flag: false after recordEmission overflows");
        copy::reset();
        D3D11_BUFFER_DESC d{};d.ByteWidth=672;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.StructureByteStride=336;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        Microsoft::WRL::ComPtr<ID3D11Buffer> pool;
        if(device && SUCCEEDED(device->CreateBuffer(&d,nullptr,&pool))) {
            Record mapped[2]={a,b};
            claim(a,kFrame);check(copy::beginMap(pool.Get(),mapped,sizeof(mapped),336,D3D11_MAP_WRITE_DISCARD,1,kFrame) && published(),"claim flag: exact after beginMap");
            copy::copier(reinterpret_cast<uintptr_t>(mapped),336,addressOf(a),0,1,kFrame);
            check(!copy::g_claimsLive.load() && published(),"claim flag: false after copier consumes the last claim");
            copy::endMap(pool.Get(),1);check(published(),"claim flag: exact after endMap");
            claim(a,kFrame);copy::invalidateMapped(reinterpret_cast<uintptr_t>(mapped));check(copy::g_claimsLive.load() && published(),"claim flag: invalidateMapped leaves source claims alone");
            copy::forget(pool.Get());check(published(),"claim flag: exact after forget");
            std::vector<Microsoft::WRL::ComPtr<ID3D11Buffer>> pools;bool opened=true;
            for(unsigned i=0;i<=copy::kMaxPools;++i) {
                Microsoft::WRL::ComPtr<ID3D11Buffer> extra;opened=opened && SUCCEEDED(device->CreateBuffer(&d,nullptr,&extra));pools.push_back(extra);
            }
            if(opened) {
                for(auto& extra:pools)copy::beginMap(extra.Get(),mapped,sizeof(mapped),336,D3D11_MAP_WRITE_DISCARD,1,kFrame);
                check(copy::g_emissions.empty() && !copy::g_claimsLive.load() && published(),"claim flag: false after beginMap's pool-cap overflow clears the claims");
                for(auto& extra:pools)copy::forget(extra.Get());
            }
        }
        copy::reset();
        void* token=copy::beginMergeOpaque(dst.ptr(),tree.anchor(0),kFrame);
        check(token==copy::mergeNoClaimsToken() && published() && !copy::g_claimsLive.load(),"claim flag: false across a no-claim merge");
        copy::endMergeOpaque(token,true);check(published(),"claim flag: exact after the token's completed end");
        token=copy::beginMergeOpaque(dst.ptr(),tree.anchor(0),kFrame);claim(a,kFrame);copy::endMergeOpaque(token,false);
        check(!copy::g_claimsLive.load() && published(),"claim flag: false after the token's unwind end revokes the late claim");
        claim(tree.item(0,0,0),kFrame);claim(tree.item(0,1,0),kFrame);
        void* plan=copy::beginMergeOpaque(dst.ptr(),tree.anchor(0),kFrame);
        check(plan!=copy::mergeNoClaimsToken() && copy::g_emissions.empty() && copy::g_mergePlans.size()==1 && copy::g_claimsLive.load() && published(),
              "claim flag: true while only a detached plan holds the claims");
        copy::endMergeOpaque(plan,true);check(published(),"claim flag: exact after a full plan's end");
        copy::reset();claim(tree.item(0,0,0),kFrame);
        copy::invalidateDictionary(tree.ptr());check(!copy::g_claimsLive.load() && published(),"claim flag: false after invalidateDictionary erases the last claim");
        copy::invalidateDictionary(tree.ptr());check(published(),"claim flag: exact after a lock-free skip");
        claim(a,kFrame);copy::reset();check(!copy::g_claimsLive.load() && published(),"claim flag: false after reset");
    }

    // --- (c) the fixed no-claim merge token ---
    {
        Tree tree(1,2);Destination dst;Record late=record(5),other=record(6);tree.item(0,0,0)=record(7);tree.item(0,1,0)=record(8);
        copy::reset();void* token=copy::beginMergeOpaque(dst.ptr(),tree.anchor(0),kFrame);
        check(token==copy::mergeNoClaimsToken() && token!=nullptr && copy::g_mergePlans.empty() && copy::stats().activePlans==0 && copy::stats().mergePlans==0,
              "no-claim merge: a non-null fixed token, no plan registered");
        auto s=copy::stats();check(s.mergeCalls==1 && s.mergeWithoutClaims==1 && s.mergeNodes==0,"no-claim merge: counted as a call without claims, no node walked");
        claim(late,kFrame);auto epoch=copy::g_emissionEpoch;copy::endMergeOpaque(token,true);
        check(held(addressOf(late)) && copy::g_emissionEpoch==epoch && copy::stats().mergeFailed==0,"no-claim merge: a completed end leaves a claim registered inside the window intact");
        copy::reset();token=copy::beginMergeOpaque(dst.ptr(),tree.anchor(0),kFrame);claim(late,kFrame);epoch=copy::g_emissionEpoch;
        copy::endMergeOpaque(token,false);
        check(copy::g_emissions.empty() && copy::g_emissionEpoch!=epoch && copy::stats().mergeFailed==1,"no-claim merge: an unwound end revokes the claim registered inside the window and bumps the epoch");
        check(copy::g_mergePlans.empty() && copy::stats().activePlans==0,"no-claim merge: no plan appears at any point");
        // An unreadable merge with no claims is not validated: the token comes back and nothing is counted as failed.
        copy::reset();epoch=copy::g_emissionEpoch;token=copy::beginMergeOpaque(0,tree.anchor(0),kFrame);
        check(token==copy::mergeNoClaimsToken() && copy::g_emissionEpoch==epoch && copy::stats().mergeFailed==0,"no-claim merge: a malformed list is not walked, so no failure and no epoch bump (the old walk bumped it)");
        copy::endMergeOpaque(token,true);
        // The full plan keeps its precision: a clear that touched it, or a newer epoch, spares unrelated later owners on unwind.
        copy::reset();claim(tree.item(0,0,0),kFrame);
        void* plan=copy::beginMergeOpaque(dst.ptr(),tree.anchor(0),kFrame);
        check(plan!=copy::mergeNoClaimsToken() && copy::g_mergePlans.size()==1 && copy::stats().mergePlans==1,"full plan: claims present, so a registered plan");
        copy::invalidateDictionary(tree.ptr());claim(other,kFrame);epoch=copy::g_emissionEpoch;copy::endMergeOpaque(plan,false);
        check(held(addressOf(other)) && copy::g_emissionEpoch==epoch && copy::g_mergePlans.empty(),"full plan: a clear-invalidated plan's unwind preserves unrelated later owners (unchanged)");
        copy::reset();claim(tree.item(0,0,0),kFrame);plan=copy::beginMergeOpaque(dst.ptr(),tree.anchor(0),kFrame);
        claim(other,kFrame+1);epoch=copy::g_emissionEpoch;copy::endMergeOpaque(plan,false);
        check(held(addressOf(other)) && copy::g_emissionEpoch==epoch,"full plan: a stale-epoch plan's unwind preserves owners of the newer epoch (unchanged)");
        // A plan, once registered, makes a concurrent clear walk; a token does not.
        copy::reset();claim(tree.item(0,0,0),kFrame);plan=copy::beginMergeOpaque(dst.ptr(),tree.anchor(0),kFrame);
        auto walked=copy::stats().clearNodes;copy::invalidateDictionary(tree.ptr());
        check(copy::stats().clearNodes>walked,"full plan: a clear during a detached plan still walks");copy::endMergeOpaque(plan,true);
        copy::reset();token=copy::beginMergeOpaque(dst.ptr(),tree.anchor(0),kFrame);walked=copy::stats().clearNodes;copy::invalidateDictionary(tree.ptr());
        check(copy::stats().clearNodes==walked && copy::stats().clearNoClaims==1,"no-claim merge: a clear during its window skips instead of walking");copy::endMergeOpaque(token,true);
        // Counters fold: stats() reports lock-free exits as calls, and reset zeroes them.
        copy::reset();for(unsigned i=0;i<10;++i){copy::invalidateDictionary(tree.ptr());copy::endMergeOpaque(copy::beginMergeOpaque(dst.ptr(),tree.anchor(0),kFrame),true);}
        s=copy::stats();check(s.clearCalls==10 && s.clearNoClaims==10 && s.mergeCalls==10 && s.mergeWithoutClaims==10 && s.activeClaims==0,"counters: lock-free exits fold into the call and no-claim counters");
        copy::reset();s=copy::stats();check(s.clearCalls==0 && s.clearNoClaims==0 && s.mergeCalls==0 && s.mergeWithoutClaims==0,"counters: reset zeroes the lock-free counters");
    }

    // --- (b) who waits for g_mutex ---
    {
        Tree tree(1,1);Destination dst;Record live=record(11);
        struct Holder {
            std::thread thread;std::atomic<bool> locked{false},release{false};
            void start(){thread=std::thread([this]{std::unique_lock<std::mutex> hold(copy::g_mutex);locked.store(true);while(!release.load())std::this_thread::yield();});
                         while(!locked.load())std::this_thread::yield();}
            void stop(){release.store(true);thread.join();}
        };
        // Runs fn on a worker while another thread holds g_mutex; true if fn returned within waitMs. The lock is released
        // afterwards either way so a blocked worker finishes and the fixture is clean.
        auto finishesWhileHeld=[&](auto&& fn,unsigned waitMs)->bool {
            Holder holder;holder.start();std::atomic<bool> done{false};std::thread worker([&]{fn();done.store(true);});
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(waitMs);
            while(!done.load() && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const bool finished=done.load();holder.stop();worker.join();return finished;
        };
        copy::reset();
        check(finishesWhileHeld([&]{copy::invalidateDictionary(tree.ptr());},5000),"mutex: a no-claim clear returns while another thread holds g_mutex");
        check(finishesWhileHeld([&]{copy::endMergeOpaque(copy::beginMergeOpaque(dst.ptr(),tree.anchor(0),kFrame),true);},5000),"mutex: a no-claim merge begin and completed end return while another thread holds g_mutex");
        check(!finishesWhileHeld([&]{copy::endMergeOpaque(copy::mergeNoClaimsToken(),false);},150),"mutex: a token's unwind end does take g_mutex");
        copy::reset();claim(live,kFrame);
        // A claim is live but none of it is in these lists: the claim check hands both calls back without the mutex.
        check(finishesWhileHeld([&]{copy::invalidateDictionary(tree.ptr());},5000),"mutex: a clear with a live claim it does not touch returns while another thread holds g_mutex");
        check(finishesWhileHeld([&]{void* p=copy::beginMergeOpaque(dst.ptr(),tree.anchor(0),kFrame);copy::endMergeOpaque(p,true);},5000),"mutex: a merge with a live claim it does not touch returns while another thread holds g_mutex");
        check(held(addressOf(live)) && published(),"mutex: those calls left the live claim and the published state exact");
        // The claim is in the list: the call revokes or stages it, which needs the mutex.
        Tree hitTree(1,1);Destination hitDst;hitTree.item(0,0,0)=record(12);claim(hitTree.item(0,0,0),kFrame);
        check(!finishesWhileHeld([&]{copy::invalidateDictionary(hitTree.ptr());},150),"mutex: a clear whose nodes hold a live claim blocks until g_mutex is released");
        check(!held(addressOf(hitTree.item(0,0,0))) && held(addressOf(live)) && published(),"mutex: the blocked clear completed afterwards, revoked that claim only and left the state exact");
        claim(hitTree.item(0,0,0),kFrame);
        check(!finishesWhileHeld([&]{void* p=copy::beginMergeOpaque(hitDst.ptr(),hitTree.anchor(0),kFrame);copy::endMergeOpaque(p,true);},150),"mutex: a merge whose list holds a live claim blocks until g_mutex is released");
        check(held(addressOf(live)) && published(),"mutex: the blocked merge completed afterwards and left the state exact");
        copy::reset();
    }

    // --- four threads of mixed traffic: no deadlock, and the flag is exact once they stop ---
    {
        constexpr unsigned kThreads=4;constexpr unsigned kRounds=4000;
        std::vector<std::unique_ptr<Tree>> trees;std::vector<std::unique_ptr<Destination>> dsts;std::vector<std::unique_ptr<Record>> records;
        for(unsigned t=0;t<kThreads;++t){trees.push_back(std::make_unique<Tree>(1,2));dsts.push_back(std::make_unique<Destination>());records.push_back(std::make_unique<Record>(record(float(t))));
                                         trees[t]->item(0,0,0)=record(float(100+t));trees[t]->item(0,1,0)=record(float(200+t));}
        copy::reset();std::atomic<bool> go{false};std::vector<std::thread> pool;
        for(unsigned t=0;t<kThreads;++t)pool.emplace_back([&,t]{
            while(!go.load())std::this_thread::yield();
            for(unsigned i=0;i<kRounds;++i) {
                switch((i+t)%5) {
                case 0:claim(trees[t]->item(0,i&1,0),kFrame);break;
                case 1:copy::invalidateDictionary(trees[t]->ptr());break;
                case 2:{void* p=copy::beginMergeOpaque(dsts[t]->ptr(),trees[t]->anchor(0),kFrame);copy::endMergeOpaque(p,(i&7)!=0);}break;
                case 3:claim(*records[t],kFrame);break;
                default:copy::invalidateEmission(addressOf(*records[t]));break;
                }
            }
        });
        go.store(true);for(auto& th:pool)th.join();
        check(published() && copy::g_mergePlans.empty(),"mixed traffic: four threads of claims, merges and clears finish, with the flag exact and no plan left behind");
        copy::reset();
    }
}

namespace bench {
using Clock=std::chrono::steady_clock;
inline double nanoseconds(Clock::duration d){return double(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());}
// Best of five repeats of `iterations` calls; ns per call.
template<class F> inline double perCall(unsigned iterations,F&& f) {
    double best=1e300;
    for(int rep=0;rep<5;++rep) {
        const auto t0=Clock::now();for(unsigned i=0;i<iterations;++i)f();
        best=std::min(best,nanoseconds(Clock::now()-t0)/iterations);
    }
    return best;
}
template<class F> inline uint64_t allocationsPerThousand(F&& f) {
    rig_allocations::start();for(unsigned i=0;i<1000;++i)f();return rig_allocations::stop();
}
// `threads` workers each call body(threadIndex) `iterations` times concurrently from a common start; best of three
// repeats of wall time / iterations, i.e. ns per call on each thread while all of them contend.
template<class Body> inline double contended(unsigned threads,unsigned iterations,Body&& body) {
    double best=1e300;
    for(int rep=0;rep<3;++rep) {
        std::atomic<unsigned> ready{0};std::atomic<bool> go{false};std::vector<std::thread> pool;
        for(unsigned t=0;t<threads;++t)pool.emplace_back([&,t]{
            ++ready;while(!go.load(std::memory_order_acquire))std::this_thread::yield();
            for(unsigned i=0;i<iterations;++i)body(t);
        });
        while(ready.load()<threads)std::this_thread::yield();
        const auto t0=Clock::now();go.store(true,std::memory_order_release);
        for(auto& th:pool)th.join();
        best=std::min(best,nanoseconds(Clock::now()-t0)/iterations);
    }
    return best;
}
inline void line(const char* what,double ns,uint64_t allocationsPer1000) {
    std::printf("bench: %-58s %9.1f ns/call   heap allocations per 1000 calls: %llu\n",what,ns,static_cast<unsigned long long>(allocationsPer1000));
}
inline void lineThreads(const char* what,unsigned threads,double ns) {
    std::printf("bench: %-58s %9.1f ns/call/thread (%u threads contending), %.1f Mcalls/s total\n",what,ns,threads,1000.0*threads/ns);
}
inline void run(ID3D11Device* device) {
    std::printf("bench: primary_copy observers, in-process fake of Elite's dictionary/list memory. Best of 5 (single thread) or 3 "
                "(contended) repeats; one pass, one machine: compare before/after on the same run, not the absolute figures.\n");
    constexpr unsigned kIterations=100000;
    Destination destination;
    auto clearBench=[&](Tree& tree,bool claims,const char* what) {
        copy::reset();Record live=record(7);if(claims)claim(live,kFrame);
        auto f=[&]{copy::invalidateDictionary(tree.ptr());};
        line(what,perCall(kIterations,f),allocationsPerThousand(f));copy::reset();
    };
    auto mergeBench=[&](Tree& source,bool claims,const char* what) {
        copy::reset();Record live=record(7);if(claims)claim(live,kFrame);
        auto f=[&]{copy::endMergeOpaque(copy::beginMergeOpaque(destination.ptr(),source.anchor(),kFrame),true);};
        line(what,perCall(kIterations,f),allocationsPerThousand(f));copy::reset();
    };
    Tree one(1,1),eight(1,8);
    mergeBench(one,false,"merge begin+end, no claims, 1 node source");
    mergeBench(eight,false,"merge begin+end, no claims, 8 node source");
    clearBench(one,false,"clear, no claims, 1 node dictionary");
    clearBench(eight,false,"clear, no claims, 8 node dictionary");
    clearBench(one,true,"clear, claims present, walks 1 node");
    clearBench(eight,true,"clear, claims present, walks 8 nodes");
    {   // The mechanism the findings suspect: another thread's merge is between its two native callbacks while this
        // one clears. Before the change that merge's registered plan made every concurrent clear walk.
        copy::reset();Tree other(1,1);Destination otherDestination;
        void* inFlight=copy::beginMergeOpaque(otherDestination.ptr(),other.anchor(),kFrame);
        auto f=[&]{copy::invalidateDictionary(eight.ptr());};
        line("clear, no claims, one no-claim merge in flight, 8 nodes",perCall(kIterations,f),allocationsPerThousand(f));
        copy::endMergeOpaque(inFlight,true);copy::reset();
    }
    {   // The mutex alone: a lock, a failed erase, an unlock. What every emit-hook call costs, and the floor under the lock instrument.
        copy::reset();Record item=record(1);
        auto f=[&]{copy::invalidateEmission(reinterpret_cast<uintptr_t>(&item));};
        line("invalidateEmission, no claims (lock + failed erase)",perCall(kIterations,f),allocationsPerThousand(f));
    }
    mergeBench(one,true,"merge begin+end, claims present, 1 node source");
    mergeBench(eight,true,"merge begin+end, claims present, 8 node source");
    copy::g_claimPrecheck=false;   // the code as it was before the claim check
    clearBench(one,true,"clear, claims present, walks 1 node (check off)");
    mergeBench(one,true,"merge begin+end, claims present, 1 node source (check off)");
    copy::g_claimPrecheck=true;
    // Four job threads contending, each on its own fake memory so only the observers' shared state is shared.
    constexpr unsigned kThreads=4,kContended=50000;
    {
        std::vector<std::unique_ptr<Tree>> trees;std::vector<std::unique_ptr<Destination>> dsts;
        for(unsigned t=0;t<kThreads;++t){trees.push_back(std::make_unique<Tree>(1,1));dsts.push_back(std::make_unique<Destination>());}
        copy::reset();
        auto mergeOnly=[&](unsigned t){copy::endMergeOpaque(copy::beginMergeOpaque(dsts[t]->ptr(),trees[t]->anchor(),kFrame),true);};
        auto clearOnly=[&](unsigned t){copy::invalidateDictionary(trees[t]->ptr());};
        auto both=[&](unsigned t){mergeOnly(t);clearOnly(t);};
        copy::reset();
        lineThreads("invalidateEmission, no claims (lock + failed erase)",kThreads,contended(kThreads,kContended,[&](unsigned t){
            copy::invalidateEmission(reinterpret_cast<uintptr_t>(&trees[t]->item(0,0,0)));}));
        copy::reset();
        lineThreads("merge begin+end, no claims, 1 node source",kThreads,contended(kThreads,kContended,mergeOnly));
        copy::reset();
        lineThreads("clear, no claims, 1 node dictionary",kThreads,contended(kThreads,kContended,clearOnly));
        copy::reset();
        lineThreads("merge begin+end then clear, no claims",kThreads,contended(kThreads,kContended,both));
        copy::reset();
        Record live=record(7);claim(live,kFrame);
        lineThreads("merge begin+end then clear, claims present (the claim check)",kThreads,contended(kThreads,kContended,both));
        copy::reset();
        {   // The same claim one claim live and untouched, with the claim check off: the code as it was before it.
            Record held=record(7);claim(held,kFrame);copy::g_claimPrecheck=false;
            lineThreads("merge begin+end then clear, claims present (check off)",kThreads,contended(kThreads,kContended/4,both));
            copy::g_claimPrecheck=true;copy::reset();
        }
    }
    // The pool copier, for comparison (not changed by this work): one copy-list entry per call against an active pool.
    D3D11_BUFFER_DESC d{};d.ByteWidth=336*8;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.StructureByteStride=336;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    if(device && SUCCEEDED(device->CreateBuffer(&d,nullptr,&buffer))) {
        copy::reset();std::vector<Record> mapped(8,record(1)),sources(8,record(2));
        const bool open=copy::beginMap(buffer.Get(),mapped.data(),uint32_t(mapped.size()*336),336,D3D11_MAP_WRITE_DISCARD,1,kFrame);
        auto base=reinterpret_cast<uintptr_t>(mapped.data()),src=reinterpret_cast<uintptr_t>(sources.data());
        auto f1=[&]{copy::copier(base,336,src,0,1,kFrame);};
        auto f8=[&]{copy::copier(base,336,src,0,8,kFrame);};
        if(open) {
            line("copier, active pool, no claims, 1 slot",perCall(kIterations,f1),allocationsPerThousand(f1));
            line("copier, active pool, no claims, 8 slots",perCall(kIterations,f8),allocationsPerThousand(f8));
            lineThreads("copier, active pool, no claims, 1 slot",kThreads,contended(kThreads,kContended,[&](unsigned){f1();}));
            {   // The render thread's copier against job threads that only call invalidateEmission (the emit hook's traffic: it
                // takes the mutex on every call and, with no claim, changes nothing).
                std::atomic<bool> stop{false};std::vector<std::thread> emitters;Record item=record(3);
                for(unsigned t=0;t<3;++t)emitters.emplace_back([&]{while(!stop.load(std::memory_order_relaxed))copy::invalidateEmission(reinterpret_cast<uintptr_t>(&item));});
                const double ns=perCall(kIterations,f1);stop.store(true);for(auto& th:emitters)th.join();
                std::printf("bench: %-58s %9.1f ns/call   (3 other threads calling invalidateEmission flat out)\n","copier, active pool, no claims, 1 slot",ns);
            }
            {   // The same, with the copier's own pool changing hands: a lease opens and closes between batches of copies.
                std::atomic<bool> stop{false};std::vector<std::thread> emitters;Record item=record(3);
                for(unsigned t=0;t<3;++t)emitters.emplace_back([&]{while(!stop.load(std::memory_order_relaxed))copy::invalidateEmission(reinterpret_cast<uintptr_t>(&item));});
                copy::endMap(buffer.Get(),1);uint64_t sequence=2;
                auto cycle=[&]{
                    copy::beginMap(buffer.Get(),mapped.data(),uint32_t(mapped.size()*336),336,D3D11_MAP_WRITE_DISCARD,sequence,kFrame);
                    for(unsigned i=0;i<16;++i)copy::copier(base,336,src,0,1,kFrame);
                    copy::endMap(buffer.Get(),sequence++);
                };
                const double ns=perCall(kIterations/16,cycle)/16;stop.store(true);for(auto& th:emitters)th.join();
                std::printf("bench: %-58s %9.1f ns/call   (3 other threads calling invalidateEmission flat out; includes 1/16 of a map+unmap)\n","copier, 16 copies per map cycle, no claims",ns);
            }
            {   // A plan in flight that holds nothing (the emission it was begun beside has been consumed): the review's chain.
                copy::reset();   // forgets the earlier lease
                Tree heldTree(1,1);Destination heldDst,otherDst;Tree other(1,1);Record e=record(9);heldTree.item(0,0,0)=record(4);
                copy::beginMap(buffer.Get(),mapped.data(),uint32_t(mapped.size()*336),336,D3D11_MAP_WRITE_DISCARD,1,kFrame);
                claim(e,kFrame);
                void* plan=copy::beginMergeOpaque(heldDst.ptr(),heldTree.anchor(0),kFrame);       // a plan, registered while an emission is live
                copy::copier(base,336,reinterpret_cast<uintptr_t>(&e),0,1,kFrame);                // the emission is consumed: the plan holds nothing
                auto beside=[&]{copy::endMergeOpaque(copy::beginMergeOpaque(otherDst.ptr(),other.anchor(0),kFrame),true);};
                line("merge begin+end beside an open plan that holds nothing",perCall(kIterations,beside),allocationsPerThousand(beside));
                line("copier beside an open plan that holds nothing",perCall(kIterations,f1),allocationsPerThousand(f1));
                copy::endMergeOpaque(plan,true);
            }
        } else std::printf("bench: copier fixture could not open a pool lease\n");
        copy::endMap(buffer.Get(),1);copy::forget(buffer.Get());copy::reset();
    }
}
}  // namespace bench
}  // namespace primary_copy_observer_tests
