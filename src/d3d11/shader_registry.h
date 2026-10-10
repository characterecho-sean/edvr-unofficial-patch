#pragma once

#include <atomic>
#include <cstdint>

namespace edvr {

// Core-owned map from shader object identity to the hash of its bytecode.
// The exposure hook host starts this service when its state is created and
// ends it on install failure or shutdown. Calls outside that lifetime are
// ignored, matching the former exposure-owned registry.
void shaderRegistryBegin();
void shaderRegistryEnd();
void registerShaderHash(void* shader, uint64_t hash);
uint64_t lookupShaderHash(void* shader);

namespace detail {
extern std::atomic<uint32_t> g_shaderRegistryGen;
}

inline uint32_t shaderRegistryGeneration() {
    return detail::g_shaderRegistryGen.load(std::memory_order_acquire);
}

}  // namespace edvr
