#include "shader_registry.h"

#include <windows.h>

#include <unordered_map>

namespace edvr {
namespace {

SRWLOCK g_shaderRegistryLock = SRWLOCK_INIT;
std::unordered_map<void*, uint64_t> g_shaderHashes;
bool g_shaderRegistryActive = false;

}  // namespace

namespace detail {
std::atomic<uint32_t> g_shaderRegistryGen{0};
}

void shaderRegistryBegin() {
    AcquireSRWLockExclusive(&g_shaderRegistryLock);
    g_shaderHashes.clear();
    g_shaderRegistryActive = true;
    ReleaseSRWLockExclusive(&g_shaderRegistryLock);
}

void shaderRegistryEnd() {
    AcquireSRWLockExclusive(&g_shaderRegistryLock);
    g_shaderRegistryActive = false;
    g_shaderHashes.clear();
    ReleaseSRWLockExclusive(&g_shaderRegistryLock);
}

void registerShaderHash(void* shader, uint64_t hash) {
    if (!shader) return;
    AcquireSRWLockExclusive(&g_shaderRegistryLock);
    if (g_shaderRegistryActive) {
        g_shaderHashes[shader] = hash;
        detail::g_shaderRegistryGen.fetch_add(1, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_shaderRegistryLock);
}

uint64_t lookupShaderHash(void* shader) {
    if (!shader) return 0;
    uint64_t out = 0;
    AcquireSRWLockShared(&g_shaderRegistryLock);
    if (g_shaderRegistryActive) {
        const auto it = g_shaderHashes.find(shader);
        if (it != g_shaderHashes.end()) out = it->second;
    }
    ReleaseSRWLockShared(&g_shaderRegistryLock);
    return out;
}

}  // namespace edvr
