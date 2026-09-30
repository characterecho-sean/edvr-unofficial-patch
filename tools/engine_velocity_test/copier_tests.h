#pragma once
// engine_velocity_test: the engine's pool copier reaches engine_velocity.cpp
// with no engine lock (the 2026-09-29 architecture review, finding C-2).
//
// observePoolCopy runs on game job threads, once per copy-list entry. It used
// to take the engine's recursive g_mutex -- held by the render thread across
// every D3D call of its slow half -- only to bump a plain counter before
// calling primaryCopy::copier, which locks primaryCopy::g_mutex for everything
// it touches. The counter is atomic now and the engine mutex is gone from that
// path. The cases:
//   B1  a job thread's observer calls complete while the render thread holds
//       the engine mutex (the pre-fix code waits for it: this fails there)
//   B2  four job threads against the render thread's Map/Unmap tees and slow-
//       half lock traffic: every call is counted exactly once, by the engine's
//       atomic and by primaryCopy's own counter under its own lock
//   B3  the observer does nothing once the feature is off
// The observer is the one configure registers (lifecycle_tests.h's stub keeps
// it), called the way the hook does.

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "lifecycle_tests.h"
#include "pin_tests.h"
#include "../../src/d3d11/engine_velocity.h"
#include "../../src/d3d11/engine_velocity_primary_copy.h"

// engine_velocity.cpp's own mutex and observer, the two things this rig holds
// and calls directly (external linkage in engine_velocity_detail).
namespace edvr { namespace engine_velocity_detail {
extern std::recursive_mutex g_mutex;
void observePoolCopy(uintptr_t mappedBase, uint32_t stride, uintptr_t source, uint64_t slot, uint32_t count) noexcept;
}}

namespace copier_tests {
using Microsoft::WRL::ComPtr;
using lifecycle_fake::g_log;
namespace copy = edvr::engine_velocity_primary_copy;

// The 30 s line's cumulative copier span count, printed by a frame boundary
// on the fake clock.
inline unsigned long long copierSpans(const lifecycle_tests::Harness& h) {
    const size_t mark = g_log.size();
    lifecycle_fake::g_clock += 31000;
    edvr::engineVelocityFrameBoundary(h.context);
    return lifecycle_tests::number(lifecycle_tests::lastLine("engine motion: primary private copy cumulative", mark),
                                   "copier spans ");
}

inline void run(const lifecycle_tests::Harness& h) {
    using edvr::engine_velocity_detail::watchesPrimaryResource;
    edvr::g_clockForTest = &lifecycle_fake::fakeClock;
    edvr::engineVelocityConfigure(true);
    h.check(lifecycle_fake::g_poolCopyObserver == &edvr::engine_velocity_detail::observePoolCopy,
            "copier: configure registers observePoolCopy as the pool copier's observer");
    const edvr::EnginePoolCopyObserverFn observe = lifecycle_fake::g_poolCopyObserver;

    // B1: the render thread inside its slow half holds the engine mutex across
    // its D3D calls; a job thread's observer must not wait for it.
    {
        std::atomic<bool> finished{false};
        bool finishedWhileHeld = false;
        std::thread job;
        {
            std::lock_guard<std::recursive_mutex> slowHalf(edvr::engine_velocity_detail::g_mutex);
            job = std::thread([&] {
                observe(0x20000, 336, 0x10000, 0, 8);            // a copy range
                observe(0x20000, 336, 0x10000, 0, UINT32_MAX);   // a refused copy
                finished.store(true);
            });
            for (int i = 0; i < 200 && !finished.load(); ++i) Sleep(10);   // up to two seconds
            finishedWhileHeld = finished.load();
        }
        job.join();
        h.check(finishedWhileHeld, "copier B1: a job thread's observer completes while the engine mutex is held");
    }

    // B2: job threads against the render thread's tees, on a registered pool
    // whose leases the copiers meet.
    ComPtr<ID3D11Buffer> pool;
    const D3D11_BUFFER_DESC desc = pin_tests::poolDesc();
    h.check(SUCCEEDED(h.device->CreateBuffer(&desc, nullptr, &pool)), "copier B2: pool-shaped buffer fixture");
    edvr::engineVelocityBufferCreated(pool.Get(), &desc);
    h.check(watchesPrimaryResource(pool.Get()), "copier B2: the live feature registers the pool");
    std::vector<unsigned char> mem(desc.ByteWidth);
    const uintptr_t mapped = reinterpret_cast<uintptr_t>(mem.data());
    constexpr unsigned kThreads = 4, kIterations = 4000;
    const unsigned long long spans0 = copierSpans(h);
    const uint64_t copies0 = copy::stats().copies;
    std::atomic<bool> go{false};
    std::vector<std::thread> jobs;
    for (unsigned t = 0; t < kThreads; ++t)
        jobs.emplace_back([&, t] {
            while (!go.load()) {}
            for (unsigned i = 0; i < kIterations; ++i) {
                if ((i & 7u) == 7u) observe(mapped, 336, 0x10000, 0, UINT32_MAX);              // a refused copy
                else observe(mapped, 336, 0x10000 + i * 336u, ((i + t) & 1u) * 4u, 8);         // a copy range, no claim behind it
            }
        });
    go.store(true);
    for (unsigned n = 0; n < 500; ++n) {
        edvr::engineVelocityResourceMapped(pool.Get(), mem.data(), D3D11_MAP_WRITE_NO_OVERWRITE);
        { std::lock_guard<std::recursive_mutex> slowHalf(edvr::engine_velocity_detail::g_mutex); }
        edvr::engineVelocityResourceWritten(pool.Get());
    }
    for (auto& j : jobs) j.join();
    const unsigned long long spans = copierSpans(h) - spans0;
    const uint64_t copies = copy::stats().copies - copies0;
    h.check(spans == uint64_t(kThreads) * kIterations,
            "copier B2: every observer call is counted exactly once across concurrent job threads");
    h.check(copies == uint64_t(kThreads) * (kIterations - kIterations / 8),
            "copier B2: primaryCopy counted every copy range exactly once under its own lock");

    // B3: off, the observer does nothing.
    edvr::engineVelocityConfigure(false);
    observe(mapped, 336, 0x10000, 0, 8);
    observe(mapped, 336, 0x10000, 0, UINT32_MAX);
    h.check(copy::stats().copies == 0, "copier B3: the observer ignores a copy once the feature is off");
    edvr::g_clockForTest = nullptr;
    std::printf("  copier: the pool copier's observer runs on job threads with no engine lock -- %u threads x %u calls "
                "counted exactly, and it completes while the engine mutex is held\n", kThreads, kIterations);
}
}  // namespace copier_tests
