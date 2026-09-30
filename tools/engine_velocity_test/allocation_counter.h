#pragma once
// Count real heap requests only inside an explicitly armed, single-threaded
// fixture. Replacements retain normal allocation/free behavior for the rig.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
namespace rig_allocations {
inline std::atomic<bool> armed{false};
inline std::atomic<uint64_t> calls{0};
inline void start(){calls.store(0,std::memory_order_relaxed);armed.store(true,std::memory_order_relaxed);}
inline uint64_t stop(){armed.store(false,std::memory_order_relaxed);return calls.load(std::memory_order_relaxed);}
}
void* operator new(size_t bytes) {
    if(rig_allocations::armed.load(std::memory_order_relaxed))rig_allocations::calls.fetch_add(1,std::memory_order_relaxed);
    if(void* p=std::malloc(bytes?bytes:1))return p;
    throw std::bad_alloc();
}
void* operator new[](size_t bytes){return ::operator new(bytes);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
void operator delete[](void* p,size_t) noexcept {std::free(p);}
