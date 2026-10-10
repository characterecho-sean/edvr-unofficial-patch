#pragma once

// advanced.cull_pose's engine half (src\common\cull_pose.h says what and why): the 2-byte hot patch at RVA 0x4E36EE of Elite build
// 332841 that keeps a game-thread (arg6 = 0) head-pose caller off the latched, cached WaitGetPoses pose. TEMPORARY with the key.
//
// HOW IT IS SAFE.
//   - The bytes are `0F 84` (the first two of `je rel32`) and become `90 E9` (`nop ; jmp rel32`): the displacement that follows is the
//     same, and the jump ends at the same address, so both forms land on 0x4E384F. Both bytes sit in the aligned eight-byte word
//     0x4E36E8..0x4E36EF, written as ONE interlocked store, so no thread ever executes a half-written branch.
//   - Gated on the PE stamp and image size of build 332841 and on the six original bytes; any mismatch refuses, and a restore refuses
//     unless the bytes are the ones this wrote.
//   - Applied only from the graphics half's frame boundary (native_frame.cpp beginFrame), which the runtime calls from the owner thread
//     inside WaitGetPoses; every pose call hops to that same thread, so the patch never lands inside one.
#include "../common/cull_pose.h"

#include <intrin.h>

#include <cstdint>

namespace edvr::cullpose {

// The PE stamp/size and the 16 bytes from 0x4E36E8 of the image mapped at `base`. Every read is guarded.
LatchImage readLatchImage(uintptr_t base) noexcept;

// The store itself, on memory that is already writable: one interlocked eight-byte exchange, so no other thread ever reads half of it.
inline void storeWordAtomic(void* address, uint64_t word) noexcept {
    _InterlockedExchange64(reinterpret_cast<volatile long long*>(address), static_cast<long long>(word));
}

// Store `word` over the aligned eight bytes at `address` with one interlocked store (VirtualProtect around it, instruction cache
// flushed). False if the address is unaligned or cannot be written; nothing is stored then.
bool writeWordAtomic(void* address, uint64_t word) noexcept;

// One frame boundary for the executable image at `base` (production passes the running executable; a rig a synthetic one): runs the
// driver in cull_pose.h on the process-wide state and returns the mode code the runtime is told. `sink` receives the change line.
uint32_t frameFor(uintptr_t base, Mode requested, void (*sink)(const char*));
uint32_t frame(Mode requested, void (*sink)(const char*));

// What the process-wide driver holds now (the log line, the rig).
const Driver& driver();

// Rig only: forget the process-wide state (the bytes are not touched).
void resetForTest();

}  // namespace edvr::cullpose
