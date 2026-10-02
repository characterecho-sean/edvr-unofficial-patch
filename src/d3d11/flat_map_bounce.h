#pragma once
// Cached WRITE_DISCARD mappings. No D3D/Windows dependency; records outlive the
// projection runtime. The adapter must call preMap before every real Map.
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

namespace edvr { namespace flatmap {
enum class Mode { Auto, On, Off };
enum class State { Pending, On, Off, Tripped };
enum class Trip { None, Remap, Present, Context, Verify };
struct MapRequest {
    uintptr_t resource = 0, context = 0;
    void* real = nullptr;
    uint32_t width = 0;
    uint64_t frame = 0;
    bool discard = false, eligible = false, success = false;
    const void* seed = nullptr;
    bool seedValid = false;
};
struct Counters {
    uint64_t bounced = 0, flushes = 0, flushBytes = 0, flushTicks = 0;
    uint64_t abandoned = 0, openAtPresent = 0, trips = 0;
    uint64_t verifySamples = 0, verifyMismatches = 0, full = 0, width = 0;
    uint64_t unchangedRows = 0, rowsChecked = 0;
};
struct NoFaults {
    static constexpr bool skipFlush=false, shortFlush=false, longFlush=false;
    static constexpr bool acceptNonDiscard=false, acceptFailed=false, noDrop=false;
    static constexpr bool unaligned=false, reuseOpen=false, inverted=false, verifyOff=false;
    static constexpr bool flushAfterUnmap=false, skipOffOwner=false;
};

template<class Driver, class Faults = NoFaults> class Runtime {
    static constexpr uintptr_t busy = std::numeric_limits<uintptr_t>::max();
    struct alignas(64) Slot {
        alignas(64) unsigned char bytes[65536 + 64]{};
        // Independent cached seed: shadow storage can disappear during reset.
        // Width-only copies buy an exact unchanged-row count without driver reads.
        unsigned char baseline[65536]{};
        std::atomic<uintptr_t> key{0};
        uintptr_t context = 0;
        void* real = nullptr;
        uint32_t width = 0;
        uint64_t frame = 0;
        bool warned = false;
    } slots_[8];
    Driver& driver_;
    std::atomic<Mode> mode_{Mode::Auto};
    std::atomic<State> state_{State::Pending};
    std::atomic<Trip> trip_{Trip::None};
    std::atomic<uint32_t> open_{0};
    std::atomic<uint64_t> bounced_{0}, flushes_{0}, flushBytes_{0}, flushTicks_{0};
    std::atomic<uint64_t> abandoned_{0}, openAtPresent_{0}, trips_{0};
    std::atomic<uint64_t> verifySamples_{0}, verifyMismatches_{0}, full_{0}, width_{0};
    std::atomic<uint64_t> unchangedRows_{0}, rowsChecked_{0};
    // Sampling and Map run only on the owning render thread.
    uint32_t samples_ = 0;
    uint64_t batchBytes_[4]{}, batchTicks_[4]{};
    double rates_[4]{};
    bool frozen_ = false, autoSlow_ = false;
    unsigned char* data(Slot& s) { return s.bytes + (Faults::unaligned ? 8 : 0); }
    void release(Slot& s, uintptr_t resource) {
        driver_.release(resource,s.context);
        s.key.store(0, std::memory_order_release);
        --open_;
    }
public:
    class Lease {
        Runtime* runtime_ = nullptr;
        Slot* slot_ = nullptr;
        uintptr_t resource_ = 0;
        friend class Runtime;
        Lease(Runtime* r, Slot* s, uintptr_t resource) : runtime_(r),slot_(s),resource_(resource) {}
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept : runtime_(other.runtime_),slot_(other.slot_),resource_(other.resource_) { other.slot_=nullptr; }
        ~Lease() { finish(); }
        void finish() noexcept { if (slot_) { runtime_->release(*slot_,resource_); slot_=nullptr; } }
        bool bounced() const { return slot_ != nullptr; }
        const void* data() const { return slot_ ? runtime_->data(*slot_) : nullptr; }
        uint32_t width() const { return slot_ ? slot_->width : 0; }
    };
    explicit Runtime(Driver& driver) : driver_(driver) {}
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    void setMode(Mode mode) {
        mode_.store(mode, std::memory_order_relaxed);
        if (trip_.load()!=Trip::None) return;
        state_.store(mode==Mode::On ? State::On : mode==Mode::Off ? State::Off :
            !frozen_ ? State::Pending : autoSlow_ ? State::On : State::Off);
        // A foreign-thread Unmap can trip concurrently with a config change.
        if (trip_.load()!=Trip::None) state_.store(State::Tripped);
    }
    Mode mode() const { return mode_.load(); }
    State decision() const { return trip_.load()!=Trip::None ? State::Tripped : state_.load(); }
    Trip tripReason() const { return trip_.load(); }
    uint32_t samples() const { return samples_; }
    bool samplingPending() const { return !frozen_ && mode()==Mode::Auto && tripReason()==Trip::None; }
    const double* rates() const { return rates_; }
    void trip(Trip reason) {
        Trip expected=Trip::None;
        if (trip_.compare_exchange_strong(expected,reason)) { ++trips_; state_.store(State::Tripped); }
    }
    void observeCopy(uint32_t width, uint64_t ticks) {
        if (!samplingPending() || width<256) return;
        const unsigned batch=samples_/8;
        batchBytes_[batch]+=width; batchTicks_[batch]+=ticks;
        if (++samples_!=32) return;
        double ordered[4];
        const double frequency=double(driver_.ticksPerSecond());
        for (unsigned i=0;i<4;++i) {
            rates_[i]=batchTicks_[i]==0 ? std::numeric_limits<double>::infinity() :
                double(batchBytes_[i])*frequency/(double(batchTicks_[i])*1e9);
            ordered[i]=rates_[i];
        }
        std::sort(ordered,ordered+4);
        const double median=(ordered[1]+ordered[2])*0.5;
        autoSlow_ = Faults::inverted ? median>=1.0 : median<1.0;
        frozen_=true;
        setMode(mode());
    }
    void preMap(uintptr_t resource) {
        if (Faults::noDrop || !resource || resource==busy || open_.load(std::memory_order_acquire)==0) return;
        for (auto& s:slots_) {
            uintptr_t expected=resource;
            if (!s.key.compare_exchange_strong(expected,busy,std::memory_order_acq_rel)) continue;
            const auto drops=++abandoned_;
            // A new Map invalidates the old driver pointer. Never flush it.
            release(s,resource);
            if (drops>=3) trip(Trip::Remap);
        }
    }
    void* install(const MapRequest& r) {
        if (decision()!=State::On || tripReason()!=Trip::None || !r.eligible || !r.resource || r.resource==busy ||
            !r.real || (!r.success && !Faults::acceptFailed) || (!r.discard && !Faults::acceptNonDiscard)) return r.real;
        if (r.width<16 || r.width>65536 || r.width%16!=0) { ++width_; return r.real; }
        for (auto& s:slots_) {
            uintptr_t expected=0;
            if (!s.key.compare_exchange_strong(expected,busy,std::memory_order_acq_rel)) {
                if (!Faults::reuseOpen || expected==busy) continue;
                s.key.store(busy);
                driver_.release(expected,s.context);
                --open_;
            }
            s.context=r.context; s.real=r.real; s.width=r.width; s.frame=r.frame; s.warned=false;
            if (r.seedValid && r.seed) std::memcpy(data(s),r.seed,r.width);
            else std::memset(data(s),0,r.width);
            std::memcpy(s.baseline,data(s),r.width);
            std::memset(s.bytes+65536,0,64);
            driver_.retain(r.resource,r.context);
            ++bounced_;
            ++open_;
            s.key.store(r.resource,std::memory_order_release);
            return data(s);
        }
        ++full_; return r.real;
    }
    Lease beginUnmap(uintptr_t resource, uintptr_t context) {
        if (!resource || resource==busy || open_.load(std::memory_order_acquire)==0) return {};
        if constexpr (Faults::skipOffOwner) { if (!driver_.ownerThread) return {}; }
        for (auto& s:slots_) {
            uintptr_t expected=resource;
            if (!s.key.compare_exchange_strong(expected,busy,std::memory_order_acq_rel)) continue;
            if (context!=s.context) {
                // Wrong context cannot authorize writing the driver mapping. Keep
                // its record for the matching Unmap, even after standing down.
                s.key.store(resource,std::memory_order_release); trip(Trip::Context); return {};
            }
            const auto started=driver_.clockTicks();
            uint64_t unchanged=0;
            for (uint32_t offset=0;offset<s.width;offset+=16)
                if (std::memcmp(data(s)+offset,s.baseline+offset,16)==0) ++unchanged;
            unchangedRows_+=unchanged; rowsChecked_+=s.width/16;
            const size_t bytes=s.width+(Faults::longFlush ? 1 : 0)-(Faults::shortFlush ? 1 : 0);
            if constexpr (Faults::flushAfterUnmap) driver_.unmapBeforeFlush(s.real,s.width);
            if (!Faults::skipFlush) std::memcpy(s.real,data(s),bytes);
            const auto finished=driver_.clockTicks();
            const auto count=++flushes_;
            flushBytes_+=bytes; flushTicks_+=finished-started;
            if (!Faults::verifyOff && mode()==Mode::On && count%16==0) {
                ++verifySamples_;
                if (!driver_.verify(s.real,data(s),s.width)) { ++verifyMismatches_; trip(Trip::Verify); }
            }
            return Lease(this,&s,resource);
        }
        return {};
    }
    void present(uint64_t frame) {
        if (open_.load(std::memory_order_acquire)==0) return;
        for (auto& s:slots_) {
            auto resource=s.key.load(std::memory_order_acquire);
            if (!resource || resource==busy || !s.key.compare_exchange_strong(resource,busy,std::memory_order_acq_rel)) continue;
            const bool overdue=s.frame<frame && !s.warned;
            if (overdue) { s.warned=true; ++openAtPresent_; }
            s.key.store(resource,std::memory_order_release);
            if (overdue) trip(Trip::Present);
        }
    }
    Counters counters() const {
        return {bounced_.load(),flushes_.load(),flushBytes_.load(),flushTicks_.load(),
            abandoned_.load(),openAtPresent_.load(),trips_.load(),verifySamples_.load(),
            verifyMismatches_.load(),full_.load(),width_.load(),unchangedRows_.load(),rowsChecked_.load()};
    }
};
} }
