#pragma once
// engine_velocity_test: the primary-pool cache holds a game buffer only while
// the feature is live (the 2026-09-29 architecture review, finding C-6).
//
// engine_velocity.cpp registers a pool-shaped buffer (dynamic, structured,
// stride 336, CPU write) at creation and holds it by reference for the private
// copy path. Before the fix the creation tee ran with the feature off -- the
// default -- and pinned up to 16 game buffers for the whole session; only a
// stand-down that never happened would have let them go, and a full cache
// dropped the seventeenth silently. The cases:
//   A1  off: no created buffer is registered or held, however many; the locked
//       entry refuses too (the race with the stand-down); nothing is logged
//   A2  live: a pool-shaped buffer is registered (one reference, once), other
//       shapes are not; live -> off lets every one go; off again holds nothing
//   A3  the cache full: exactly one line for the overflow, the 30 s figure
//       keeps counting, and the next run has the line again
//   A4  the feature switched on mid-session: a pool made while it was off is
//       nominated from the t33 binding at the first snapshot (logged) and the
//       Map/Unmap tees then track it -- the recognition needs no buffer seen at
//       creation
// Driven through the same linked engine_velocity.cpp lifecycle_tests.h stubs.

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

#include "lifecycle_tests.h"
#include "../../src/d3d11/engine_velocity.h"
#include "../../src/d3d11/engine_velocity_primary_copy.h"

namespace pin_tests {
using Microsoft::WRL::ComPtr;
using lifecycle_fake::g_log;
namespace copy = edvr::engine_velocity_primary_copy;

// The pool's shape as notePrimaryBufferCreated filters it.
inline D3D11_BUFFER_DESC poolDesc() {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = 336 * 16;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    d.StructureByteStride = 336;
    return d;
}
// The count a caller sees, with this probe's own reference taken out again.
inline ULONG refs(IUnknown* object) {
    object->AddRef();
    return object->Release();
}
inline size_t lines(const char* fragment, size_t from) {
    size_t n = 0;
    for (size_t i = from; i < g_log.size(); ++i)
        if (g_log[i].find(fragment) != std::string::npos) ++n;
    return n;
}
// The 30 s block's positive map cache overflow figure (cumulative), read from
// the summary a frame boundary prints on the fake clock.
inline unsigned long long overflowFigure(const lifecycle_tests::Harness& h) {
    const size_t mark = g_log.size();
    lifecycle_fake::g_clock += 31000;
    edvr::engineVelocityFrameBoundary(h.context);
    return lifecycle_tests::number(lifecycle_tests::lastLine("engine motion: primary private copy cumulative", mark),
                                   "positive map cache overflow ");
}

inline void run(const lifecycle_tests::Harness& h) {
    using edvr::engine_velocity_detail::watchesPrimaryResource;
    edvr::g_clockForTest = &lifecycle_fake::fakeClock;
    h.check(!edvr::engineVelocityActive(), "pins: the feature starts off");
    constexpr unsigned kCapacity = edvr::engine_velocity_detail::kPrimaryPoolResources;
    const D3D11_BUFFER_DESC desc = poolDesc();
    std::vector<ComPtr<ID3D11Buffer>> pools(kCapacity + 4);
    std::vector<ULONG> base;
    for (auto& p : pools) {
        h.check(SUCCEEDED(h.device->CreateBuffer(&desc, nullptr, &p)), "pins: pool-shaped buffer fixture");
        base.push_back(refs(p.Get()));
    }
    auto held = [&](size_t i) { return refs(pools[i].Get()) != base[i]; };
    auto watched = [&](size_t i) { return watchesPrimaryResource(pools[i].Get()); };

    // A1: off, the creation tee registers and holds nothing.
    size_t mark = g_log.size();
    for (auto& p : pools) edvr::engineVelocityBufferCreated(p.Get(), &desc);
    edvr::engine_velocity_detail::notePrimaryBufferCreated(pools[0].Get(), desc);   // the locked entry, past the inline test
    bool any = false;
    for (size_t i = 0; i < pools.size(); ++i) any = any || held(i) || watched(i);
    h.check(!any, "pins A1: with the feature off no created buffer is registered or held");
    h.check(g_log.size() == mark, "pins A1: off says nothing (no overflow line for buffers never held)");

    // A2: live registers a pool-shaped buffer once; other shapes never.
    edvr::engineVelocityConfigure(true);
    h.check(edvr::engineVelocityActive(), "pins A2: configure turns the feature on");
    for (size_t i = 0; i < 3; ++i) edvr::engineVelocityBufferCreated(pools[i].Get(), &desc);
    edvr::engineVelocityBufferCreated(pools[0].Get(), &desc);   // the same buffer again
    h.check(refs(pools[0].Get()) == base[0] + 1 && refs(pools[1].Get()) == base[1] + 1 &&
            refs(pools[2].Get()) == base[2] + 1, "pins A2: live holds each registered buffer once");
    h.check(watched(0) && watched(1) && watched(2) && !watched(3) && !held(3),
            "pins A2: the watch slots name the registered buffers only");
    {
        ComPtr<ID3D11Buffer> other;
        D3D11_BUFFER_DESC od = desc;
        od.Usage = D3D11_USAGE_DEFAULT;
        od.CPUAccessFlags = 0;
        h.check(SUCCEEDED(h.device->CreateBuffer(&od, nullptr, &other)), "pins A2: other-shape fixture");
        const ULONG otherBase = refs(other.Get());
        edvr::engineVelocityBufferCreated(other.Get(), &od);
        h.check(refs(other.Get()) == otherBase && !watchesPrimaryResource(other.Get()),
                "pins A2: a buffer that is not the pool's shape is not held");
    }
    edvr::engineVelocityConfigure(false);
    h.check(!edvr::engineVelocityActive() && !held(0) && !held(1) && !held(2) && !watched(0) && !watched(1) && !watched(2),
            "pins A2: live -> off releases every registered buffer and clears the watch slots");
    edvr::engineVelocityBufferCreated(pools[5].Get(), &desc);
    h.check(!held(5) && !watched(5), "pins A2: off again holds nothing");

    // A3: the cache full says so once per run; the figure keeps counting.
    edvr::engineVelocityConfigure(true);
    const unsigned long long figure0 = overflowFigure(h);
    mark = g_log.size();
    for (auto& p : pools) edvr::engineVelocityBufferCreated(p.Get(), &desc);   // capacity + 4
    unsigned pinned = 0;
    for (size_t i = 0; i < pools.size(); ++i) pinned += held(i) ? 1u : 0u;
    h.check(pinned == kCapacity && held(0) && held(kCapacity - 1) && !held(kCapacity),
            "pins A3: the cache holds its capacity, the first arrivals");
    h.check(lines("primary pool cache full", mark) == 1, "pins A3: the overflow is logged exactly once");
    for (size_t i = kCapacity; i < pools.size(); ++i) edvr::engineVelocityBufferCreated(pools[i].Get(), &desc);
    h.check(lines("primary pool cache full", mark) == 1, "pins A3: later overflows stay silent");
    h.check(overflowFigure(h) == figure0 + 8, "pins A3: the 30 s line keeps counting every overflow (4, then 4 again)");
    edvr::engineVelocityConfigure(false);
    pinned = 0;
    for (size_t i = 0; i < pools.size(); ++i) pinned += (held(i) || watched(i)) ? 1u : 0u;
    h.check(pinned == 0, "pins A3: the stand-down releases the whole full cache");
    edvr::engineVelocityConfigure(true);
    mark = g_log.size();
    for (auto& p : pools) edvr::engineVelocityBufferCreated(p.Get(), &desc);
    h.check(lines("primary pool cache full", mark) == 1, "pins A3: the next run logs its own overflow once");
    edvr::engineVelocityConfigure(false);

    // A4: switched on mid-session -- a pool made while off is nominated at the
    // first snapshot, and the map tees then follow it.
    {
        lifecycle_tests::Game g(h);
        g.setup();
        ComPtr<ID3D11Buffer> dyn;
        ComPtr<ID3D11ShaderResourceView> dynView;
        h.check(SUCCEEDED(h.device->CreateBuffer(&desc, nullptr, &dyn)) &&
                SUCCEEDED(h.device->CreateShaderResourceView(dyn.Get(), nullptr, &dynView)), "pins A4: dynamic pool + view");
        const ULONG dynBase = refs(dyn.Get());
        edvr::engineVelocityBufferCreated(dyn.Get(), &desc);   // created while off
        h.check(!watchesPrimaryResource(dyn.Get()) && refs(dyn.Get()) == dynBase, "pins A4: created while off, not held");
        edvr::engineVelocityConfigure(true);
        h.check(!watchesPrimaryResource(dyn.Get()), "pins A4: turning the feature on registers nothing by itself");
        mark = g_log.size();
        g.beginFrame();
        g.setPool(dynView.Get());
        g.writeScene(g.sceneA.Get(), g.rows[0]);
        g.pass(0);
        h.check(watchesPrimaryResource(dyn.Get()), "pins A4: the first snapshot nominates the pool from the t33 binding");
        h.check(lifecycle_tests::logged("nominated at draw frame", mark), "pins A4: the nomination is logged");
        g.endFrame();
        std::vector<unsigned char> mapped(desc.ByteWidth);
        edvr::engineVelocityResourceMapped(dyn.Get(), mapped.data(), D3D11_MAP_WRITE_NO_OVERWRITE);
        auto lease = copy::g_pools.find(dyn.Get());
        h.check(lease != copy::g_pools.end() && lease->second.active, "pins A4: the next Map opens a lease on the nominated pool");
        edvr::engineVelocityResourceWritten(dyn.Get());
        lease = copy::g_pools.find(dyn.Get());
        h.check(lease != copy::g_pools.end() && !lease->second.active, "pins A4: the Unmap tee closes it");
        edvr::engineVelocityConfigure(false);
        h.check(!watchesPrimaryResource(dyn.Get()) && refs(dyn.Get()) == dynBase && copy::g_pools.empty(),
                "pins A4: stand-down releases the nominated pool, its lease and the eye-frame's hold");
    }
    edvr::g_clockForTest = nullptr;
    std::printf("  pins: the pool cache holds nothing while the feature is off, releases on stand-down, logs a full "
                "cache once, and a mid-session activation nominates the pool from its binding\n");
}
}  // namespace pin_tests
