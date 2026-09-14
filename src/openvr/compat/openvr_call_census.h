#pragma once
// Typed, bounded observations of the historical OpenVR interface. No caller
// storage is touched until an enabled census has reserved an evidence record.
#include "openvr_v0_9_20.h"
#include "openvr_abi_manifest.h"
#include <atomic>
#include <cstdint>

namespace edvr::openvr_abi {
enum class Origin : uint8_t { Unknown, Game, Edvr, Injected };
enum EvidenceFlags : uint32_t {
    EvidenceNone = 0, EvidenceArgs = 1u << 0, EvidenceResult = 1u << 1,
    EvidenceOutputs = 1u << 2, EvidenceMatrix44 = 1u << 3,
    EvidenceMatrix34 = 1u << 4, EvidenceTexture = 1u << 5,
    EvidenceEvent = 1u << 6, EvidenceColor = 1u << 7,
    EvidenceReadFault = 1u << 8
};
// schema is a compile-time field description, never runtime/user text.
// Masks distinguish a measured zero from a field which was not read.
struct Evidence {
    const char* schema = "none";
    uint32_t flags = EvidenceNone;
    uint32_t u32[8]{}; int32_t i32[4]{}; float f32[16]{};
    uint64_t u64 = 0;
    uint32_t uMask = 0, iMask = 0, fMask = 0;
    bool haveU64 = false;
    uint32_t bytes = 0;
    float matrix44[16]{}; float matrix34[12]{};
    void u(unsigned n, uint32_t v) noexcept { u32[n]=v; uMask|=1u<<n; }
    void i(unsigned n, int32_t v) noexcept { i32[n]=v; iMask|=1u<<n; }
    void f(unsigned n, float v) noexcept { f32[n]=v; fMask|=1u<<n; }
};
struct Event { const char* interface_name; uint16_t slot; Origin origin; uint32_t record; Evidence evidence; const char* signature; };
using Sink = void (*)(const Event&) noexcept;
using OriginResolver = Origin (*)(const void*) noexcept;

class Census {
public:
    static constexpr unsigned kKeys = 16, kSamples = 4;
    // An occupied cell packs its exact 32-bit discriminator above an 8-bit
    // sample count. Cells never move/reset, so concurrent first insertions and
    // claims cannot alias distinct keys or reopen a saturated budget.
    bool claim(const Method& method, Origin origin, uint32_t discriminator,
               uint32_t& record) const noexcept {
        record=0;
        if (!enabled()) return false;
        size_t index=kMethodCount;
        for(size_t i=0;i<kMethodCount;++i) if(&kMethods[i]==&method){index=i;break;}
        const unsigned o=static_cast<unsigned>(origin);
        if(index==kMethodCount || o>=4) return false;
        for(auto& cell:claims_[index][o]) {
            uint64_t value=cell.load(std::memory_order_relaxed);
            for(;;) {
                if(value && (value>>8)!=discriminator) break;
                const unsigned count=static_cast<unsigned>(value&255u);
                if(count>=kSamples) return false;
                const uint64_t next=(uint64_t(discriminator)<<8)|(count+1);
                if(cell.compare_exchange_weak(value,next,std::memory_order_relaxed)) {
                    record=nextRecord_.fetch_add(1,std::memory_order_relaxed)+1;
                    return true;
                }
            }
        }
        return false; // Distinct-key capacity exhausted; no absence claim.
    }
    void enable(Sink sink) noexcept { sink_.store(sink,std::memory_order_release); }
    void disable() noexcept { sink_.store(nullptr,std::memory_order_release); }
    bool enabled() const noexcept { return sink_.load(std::memory_order_acquire)!=nullptr; }
    void set_origin_resolver(OriginResolver resolver) noexcept { resolver_.store(resolver,std::memory_order_release); }
    Origin classify(const void* address) const noexcept {
        auto resolver=resolver_.load(std::memory_order_acquire);
        return resolver ? resolver(address):Origin::Unknown;
    }
    void observe(const Method& method, Origin origin) const noexcept {
        uint32_t id=0;
        if(claim(method,origin,0,id)) observe(method,origin,Evidence{},id);
    }
    void observe(const Method& method, Origin origin, const Evidence& evidence,
                 uint32_t record=0) const noexcept {
        if(!record && !claim(method,origin,0,record)) return;
        auto sink=sink_.load(std::memory_order_acquire);
        if(sink) sink(Event{method.interface_name,static_cast<uint16_t>(method.slot),origin,record,evidence,method.signature});
    }
private:
    std::atomic<Sink> sink_{nullptr};
    std::atomic<OriginResolver> resolver_{nullptr};
    mutable std::atomic<uint64_t> claims_[kMethodCount][4][kKeys]{};
    mutable std::atomic<uint32_t> nextRecord_{0};
};
}
