#pragma once
#include <d3d11.h>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace edvr {
// Flat-only demand cache of authoritative CPU uploads. It never maps a GPU
// resource or retains a mapped pointer. Publish while the caller's CPU bytes
// are valid; every unobserved or partial mutation invalidates the whole entry.
class FlatAnimatedIdentityLedger {
public:
    static constexpr unsigned maxInstanceBuffers=4, maxPoolBuffers=2, maxMutationResources=128;
    static constexpr unsigned maxInstanceBytes=1024*1024, maxPoolRows=16384;
    static constexpr unsigned poolStride=336;
    struct Identity {
        uint32_t slot=0, skeleton=0, allocation=0;
        uint64_t instanceEpoch=0, poolEpoch=0;
        const char* refusal=nullptr;
    };
    // Diagnostic ordering only. No field in this receipt certifies identity;
    // lookup still requires bytes from a witnessed CPU publication.
    struct PoolReceipt {
        const void* resource=nullptr;
        uint64_t firstDemand=0,lastDemand=0,lastPublication=0,slotDemand=0;
        uint64_t publicationEpoch=0;
        unsigned requestedAtPublication=0,requestedNow=0;
        unsigned publications=0,slotEvictions=0,resourceEvictions=0,receiptEvictions=0;
        uint32_t slot=0,lastEvictedSlot=0;
        bool resident=false,rowObserved=false,slotRedemanded=false;
    };
    // Call for complete writes to a structurally matching pool before it is
    // demanded. This records ordering without reading or retaining any bytes.
    void notePoolPublication(const void* resource,unsigned byteWidth) {
        if(!resource || !byteWidth || byteWidth%poolStride || byteWidth/poolStride>maxPoolRows)return;
        auto& r=receiptFor(resource);r.lastPublication=++eventSequence_;++r.publications;
        const Entry* e=findIn(pools_,resource);
        r.publicationEpoch=e && e->valid?e->epoch:0;
        r.requestedAtPublication=e?unsigned(e->requestedRows.size()):0;
    }
    PoolReceipt poolReceipt(const void* resource,uint32_t slot) const {
        PoolReceipt out{};out.resource=resource;out.slot=slot;
        for(const auto& r:poolReceipts_)if(r.resource==resource){out=r;break;}
        out.slot=slot;out.resourceEvictions=resourceEvictions_;out.receiptEvictions=receiptEvictions_;
        const Entry* e=findIn(pools_,resource);
        if(e){out.resident=true;out.requestedNow=unsigned(e->requestedRows.size());
            out.rowObserved=e->valid && slot<e->rows.size() &&
                (!e->sparsePool || (slot<e->rowValid.size() && e->rowValid[slot]));
            for(size_t i=0;i<e->requestedRows.size();++i)if(e->requestedRows[i]==slot){
                out.slotDemand=e->requestedSeq[i];out.slotRedemanded=e->requestedRedemanded[i]!=0;break;}
        }
        return out;
    }
    bool demandInstances(const void* resource,unsigned byteWidth) {
        return demand(instances_,resource,byteWidth,0,maxInstanceBytes);
    }
    bool demandPool(const void* resource,unsigned byteWidth,unsigned stride=poolStride) {
        if(stride!=poolStride || byteWidth%poolStride || byteWidth/poolStride>maxPoolRows) {
            invalidate(resource);return false;
        }
        return demand(pools_,resource,byteWidth,stride,maxPoolRows*poolStride);
    }
    // Observe actual mapped storage at Unmap, including untouched bytes of a
    // NO_OVERWRITE append. The span certifies readable publication storage;
    // it does not claim that the application initialized every byte itself.
    bool beginMap(const void* resource,D3D11_MAP kind) {
        if(kind==D3D11_MAP_READ)return false;
        noteMutation(resource);Entry* e=find(resource);if(!e)return false;
        makeInvalid(*e);e->pendingMap=true;e->mapKind=kind;return true;
    }
    bool endMap(const void* resource,const void* bytes,size_t spanBytes,bool wholeResource=true) {
        Entry* e=find(resource);if(!e)return false;
        const bool full=e->pendingMap && wholeResource && spanBytes==e->byteWidth && bytes &&
            (e->mapKind==D3D11_MAP_WRITE || e->mapKind==D3D11_MAP_WRITE_DISCARD || e->mapKind==D3D11_MAP_WRITE_NO_OVERWRITE);
        e->pendingMap=false;
        if(!full){makeInvalid(*e);return false;}
        return publish(*e,bytes);
    }
    // Exact full initial data / UpdateSubresource only. Partial updates,
    // copies and GPU writes must call invalidate(), even if bytes look equal.
    bool publishWhole(const void* resource,const void* bytes,size_t spanBytes) {
        noteMutation(resource);
        Entry* e=find(resource);if(!e)return false;
        if(e->pendingMap || !bytes || spanBytes!=e->byteWidth){makeInvalid(*e);return false;}
        return publish(*e,bytes);
    }
    void invalidate(const void* resource) {
        if(!resource){for(auto& e:mutations_)if(e.resource)e.epoch=++epoch_;for(auto& e:instances_)makeInvalid(e);for(auto& e:pools_)makeInvalid(e);return;}
        noteMutation(resource);
        for(auto& e:instances_)if(e.resource==resource)makeInvalid(e);
        for(auto& e:pools_)if(e.resource==resource)makeInvalid(e);
    }
    void erase(const void* resource) {
        for(auto& e:instances_)if(e.resource==resource)e=Entry{};
        for(auto& e:pools_)if(e.resource==resource)e=Entry{};
        for(auto& e:mutations_)if(e.resource==resource)e=Mutation{};
        for(auto& r:poolReceipts_)if(r.resource==resource)r=PoolReceipt{};
    }
    void reset(){for(auto& e:instances_)e=Entry{};for(auto& e:pools_)e=Entry{};for(auto& e:mutations_)e=Mutation{};
        for(auto& r:poolReceipts_)r=PoolReceipt{};eventSequence_=0;resourceEvictions_=receiptEvictions_=nextReceipt_=0;}
    // Demand only actual inputs of a qualified foreground draw. Hooks notify
    // every mutation of watched inputs, including bone/other SRVs which need
    // no CPU shadow. Zero means unknown/evicted and cannot certify equivalence.
    uint64_t demandMutation(const void* resource) {
        if(!resource)return 0;
        Mutation* selected=nullptr;
        for(auto& e:mutations_)if(e.resource==resource){e.touch=++clock_;return e.epoch;}
        for(auto& e:mutations_)if(!e.resource){selected=&e;break;}
        if(!selected){selected=&mutations_[0];for(auto& e:mutations_)if(e.touch<selected->touch)selected=&e;}
        selected->resource=resource;selected->epoch=++epoch_;selected->touch=++clock_;return selected->epoch;
    }
    uint64_t mutationEpoch(const void* resource) const {
        if(resource)for(const auto& e:mutations_)if(e.resource==resource)return e.epoch;return 0;
    }
    void noteMutation(const void* resource) {
        if(resource)for(auto& e:mutations_)if(e.resource==resource){e.epoch=++epoch_;return;}
    }
    bool lookupInstance(const void* resource,uint64_t offset,uint32_t& slot,uint64_t& epoch) const {
        const Entry* index=findIn(instances_,resource);
        if(!index || !index->valid || index->pendingMap || offset%4 || offset>index->byteWidth || index->byteWidth-offset<4)return false;
        std::memcpy(&slot,index->bytes.data()+size_t(offset),4);epoch=index->epoch;return true;
    }
    // Pool identities are demanded only after the actual raw instance index
    // becomes known. New slots stay unknown until a later CPU publication.
    bool demandPoolSlot(const void* resource,unsigned byteWidth,unsigned stride,uint32_t slot) {
        if(!demandPool(resource,byteWidth,stride))return false;Entry* e=find(resource);
        if(slot>=e->rows.size())return false;
        if(!e->sparsePool){e->sparsePool=true;e->rowValid.resize(e->rows.size());makeInvalid(*e);}
        for(unsigned requested:e->requestedRows)if(requested==slot)return true;
        auto& r=receiptFor(resource);
        if(e->requestedRows.size()==64){r.lastEvictedSlot=e->requestedRows.front();++r.slotEvictions;
            e->rowValid[r.lastEvictedSlot]=0;e->requestedRows.erase(e->requestedRows.begin());
            e->requestedSeq.erase(e->requestedSeq.begin());e->requestedRedemanded.erase(e->requestedRedemanded.begin());}
        const uint64_t sequence=++eventSequence_;
        const unsigned char bit=static_cast<unsigned char>(1u<<(slot&7));
        const bool redemanded=(e->seenBits[slot>>3]&bit)!=0;
        e->seenBits[slot>>3]|=bit;e->requestedRows.push_back(slot);e->requestedSeq.push_back(sequence);
        e->requestedRedemanded.push_back(redemanded?1:0);
        r.lastDemand=sequence;e->rowValid[slot]=0;return true;
    }
    bool lookup(const void* instanceResource,uint64_t instanceByteOffset,const void* poolResource,Identity& out) const {
        out=Identity{};
        auto refuse=[&](const char* why){out.refusal=why;return false;};
        const Entry* index=findIn(instances_,instanceResource);
        const Entry* pool=findIn(pools_,poolResource);
        if(!index || !pool)return refuse("identity-resource-not-demanded");
        if(!index->valid || !pool->valid || index->pendingMap || pool->pendingMap)return refuse("identity-upload-unobserved");
        if(instanceByteOffset%4 || instanceByteOffset>index->byteWidth || index->byteWidth-instanceByteOffset<4)
            return refuse("identity-instance-offset");
        std::memcpy(&out.slot,index->bytes.data()+size_t(instanceByteOffset),4);
        if(out.slot>=pool->rows.size())return refuse("identity-pool-index-range");
        if(pool->sparsePool && !pool->rowValid[out.slot])return refuse("identity-pool-slot-unobserved");
        out.skeleton=pool->rows[out.slot].skeleton;out.allocation=pool->rows[out.slot].allocation;
        out.instanceEpoch=index->epoch;out.poolEpoch=pool->epoch;return true;
    }
    size_t retainedBytes() const {
        size_t total=0;for(const auto& e:instances_)total+=e.bytes.size();
        for(const auto& e:pools_)total+=e.rows.size()*sizeof(Row)+e.rowValid.size()+e.requestedRows.size()*sizeof(unsigned);return total;
    }
private:
    struct Row {uint32_t skeleton=0,allocation=0;};
    struct Mutation {const void* resource=nullptr;uint64_t epoch=0,touch=0;};
    struct Entry {
        const void* resource=nullptr;unsigned byteWidth=0,stride=0;
        uint64_t epoch=0,touch=0;bool valid=false,pendingMap=false;
        D3D11_MAP mapKind=D3D11_MAP_READ;
        std::vector<unsigned char> bytes;std::vector<Row> rows;
        bool sparsePool=false;std::vector<unsigned char> rowValid,seenBits,requestedRedemanded;
        std::vector<unsigned> requestedRows;std::vector<uint64_t> requestedSeq;
    };
    template<size_t N> static const Entry* findIn(const std::array<Entry,N>& entries,const void* resource) {
        if(!resource)return nullptr;for(const auto& e:entries)if(e.resource==resource)return &e;return nullptr;
    }
    Entry* find(const void* resource) {
        if(!resource)return nullptr;
        for(auto& e:instances_)if(e.resource==resource)return &e;
        for(auto& e:pools_)if(e.resource==resource)return &e;
        return nullptr;
    }
    template<size_t N> bool demand(std::array<Entry,N>& entries,const void* resource,unsigned byteWidth,unsigned stride,unsigned cap) {
        if(!resource)return false;
        if(!byteWidth || byteWidth>cap){invalidate(resource);return false;}
        if(const Entry* e=find(resource))if(e->stride!=stride){invalidate(resource);return false;}
        Entry* selected=nullptr;
        for(auto& e:entries)if(e.resource==resource){selected=&e;break;}
        if(selected && selected->byteWidth==byteWidth && selected->stride==stride){selected->touch=++clock_;demandMutation(resource);return true;}
        if(!selected)for(auto& e:entries)if(!e.resource){selected=&e;break;}
        if(!selected){selected=&entries[0];for(auto& e:entries)if(e.touch<selected->touch)selected=&e;
            if(stride && selected->resource)++resourceEvictions_;}
        *selected=Entry{};selected->resource=resource;selected->byteWidth=byteWidth;selected->stride=stride;
        selected->touch=++clock_;selected->epoch=++epoch_;demandMutation(resource);
        if(stride){selected->rows.resize(byteWidth/stride);selected->seenBits.resize((byteWidth/stride+7)/8);
            auto& r=receiptFor(resource);r.lastDemand=++eventSequence_;
            if(!r.firstDemand)r.firstDemand=r.lastDemand;}
        else selected->bytes.resize(byteWidth);
        return true;
    }
    void makeInvalid(Entry& e){if(e.resource){e.valid=false;e.pendingMap=false;e.epoch=++epoch_;}}
    bool publish(Entry& e,const void* data) {
        noteMutation(e.resource);
        const auto* bytes=static_cast<const unsigned char*>(data);
        auto readRow=[&](size_t i){std::memcpy(&e.rows[i].skeleton,bytes+i*poolStride,4);
            std::memcpy(&e.rows[i].allocation,bytes+i*poolStride+28,4);};
        if(e.stride){if(e.sparsePool){std::fill(e.rowValid.begin(),e.rowValid.end(),static_cast<unsigned char>(0));
                for(unsigned i:e.requestedRows){readRow(i);e.rowValid[i]=1;}}
            else for(size_t i=0;i<e.rows.size();++i)readRow(i);
        } else std::memcpy(e.bytes.data(),data,e.byteWidth);
        e.epoch=++epoch_;e.touch=++clock_;e.valid=true;
        if(e.stride)notePoolPublication(e.resource,e.byteWidth);
        return true;
    }
    PoolReceipt& receiptFor(const void* resource) {
        for(auto& r:poolReceipts_)if(r.resource==resource)return r;
        for(auto& r:poolReceipts_)if(!r.resource){r.resource=resource;return r;}
        auto& r=poolReceipts_[nextReceipt_++%poolReceipts_.size()];++receiptEvictions_;r=PoolReceipt{};r.resource=resource;return r;
    }
    std::array<Entry,maxInstanceBuffers> instances_;
    std::array<Entry,maxPoolBuffers> pools_;
    std::array<Mutation,maxMutationResources> mutations_;
    uint64_t epoch_=0,clock_=0;
    std::array<PoolReceipt,8> poolReceipts_{};
    uint64_t eventSequence_=0;
    unsigned resourceEvictions_=0,receiptEvictions_=0,nextReceipt_=0;
};
} // namespace edvr
