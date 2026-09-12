#pragma once

// Safe, opt-in call census primitives.  This file deliberately does not
// install hooks or manufacture ABI-compatible thunks.  The typed forwarding
// interfaces call observe() from exact method bodies; when disabled the
// branch is empty and the method continues with its original implementation.
#include "openvr_v0_9_20.h"
#include "openvr_abi_manifest.h"
#include <atomic>
#include <cstdint>

namespace edvr::openvr_abi {
enum class Origin : uint8_t { Unknown, Game, Edvr, Injected };
struct Event { const char* interface_name; uint16_t slot; Origin origin; };
using Sink = void (*)(const Event&) noexcept;
using OriginResolver = Origin (*)(const void* caller_address) noexcept;

class Census {
public:
    void enable(Sink sink) noexcept { sink_.store(sink, std::memory_order_release); }
    void set_origin_resolver(OriginResolver resolver) noexcept {
        resolver_.store(resolver, std::memory_order_release);
    }
    void disable() noexcept { sink_.store(nullptr, std::memory_order_release); }
    bool enabled() const noexcept { return sink_.load(std::memory_order_acquire) != nullptr; }

    // The caller supplies a slot from kMethods and an evidence-based origin.
    // No arguments or return values are touched, so the original method result
    // and side effects remain authoritative.
    void observe(const Method& method, Origin origin) const noexcept {
        Sink sink = sink_.load(std::memory_order_acquire);
        if (!sink) return;
        sink(Event{method.interface_name, static_cast<uint16_t>(method.slot), origin});
    }

    Origin classify(const void* caller_address) const noexcept {
        OriginResolver resolver = resolver_.load(std::memory_order_acquire);
        return resolver ? resolver(caller_address) : Origin::Unknown;
    }

private:
    std::atomic<Sink> sink_{nullptr};
    std::atomic<OriginResolver> resolver_{nullptr};
};

}
