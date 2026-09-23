// Test rig for the cull gate probe (src/d3d11/cull_gate_probe.*): drives the
// three relay observers with synthetic engine memory laid out exactly as the
// decompiles read it -- a render context with three views, a record, its
// pose context with instance entries, models and sub-items, the traversal's
// gate context, and the draw-item builder's frame around its FUN_1442B3FC0
// call -- and a stand-in for the engine's plane test, then writes the file
// tools/cull_gate_probe.py --verify-fixture checks. No hooks, no game.
#include "../../src/d3d11/cull_gate_probe.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("  FAIL  %s\n", what); }
    else std::printf("  ok    %s\n", what);
}

template <typename T> void put(std::vector<uint8_t>& block, size_t off, const T& v) { std::memcpy(block.data() + off, &v, sizeof(T)); }
uintptr_t addr(const std::vector<uint8_t>& block) { return reinterpret_cast<uintptr_t>(block.data()); }

// The stand-in for FUN_1404F4E10: view 0 inside (1), view 1 outside (-1),
// view 2 straddling (0) -- keyed by the view's own bit index (+0x578).
uint64_t __fastcall fakeFrustum(uintptr_t view, const float* point, const float* interval) {
    uint32_t bit = 0;
    std::memcpy(&bit, reinterpret_cast<const void*>(view + 0x578), 4);
    (void)point; (void)interval;
    if (bit == 0) return 1;
    if (bit == 1) return 0xFFFFFFFFull;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("cull_gate_probe_test: dry-run, 0 checks, 0 failures\n");
        return 0;
    }
    const std::string out = argc > 1 ? argv[1] : "gate_fixture.bin";
    using edvr::CullGateProbe;
    CullGateProbe& probe = edvr::cullGateProbe;

    // Render context: three views at +0x40 (stride 0x6A0), count at +0x1A940.
    std::vector<uint8_t> ctx(0x1A948 + 64, 0);
    put(ctx, 0x30, 0.75f);
    put<uint64_t>(ctx, 0x1A940, 3);
    std::vector<std::vector<float>> planes(3);
    for (uint32_t v = 0; v < 3; ++v) {
        const size_t view = 0x40 + size_t(v) * 0x6A0;
        planes[v] = {1.f, 0.f, 0.f, float(v), 0.f, 1.f, 0.f, 2.f};   // two planes per view
        put<uint64_t>(ctx, view + 0x30, reinterpret_cast<uint64_t>(planes[v].data()));
        put<uint16_t>(ctx, view + 0x44, 2);
        const float cam[4] = {10.f * v, 20.f, 30.f, 1.f};
        std::memcpy(ctx.data() + view + 0x540, cam, 16);
        put(ctx, view + 0x550, 0.5f + v);
        put(ctx, view + 0x560, 0.25f);
        put<uint64_t>(ctx, view + 0x570, 1ull << v);
        put<uint32_t>(ctx, view + 0x578, v);
        put<uint32_t>(ctx, view + 0x688, 0x10u * v);
        put<uint8_t>(ctx, view + 0x68D, v == 2 ? 1 : 0);
        put<uint32_t>(ctx, 0x1A840 + v * 4, v);
    }
    // Pose context: quaternion +0x10, translation +0x20, entries +0x48/+0x50,
    // local centre +0x80, extents +0x90. A +90 degree turn about z.
    std::vector<uint8_t> pose(0x100, 0);   // room for the shifted (mismatch) read below
    const float s = std::sqrt(0.5f);
    const float q[4] = {0.f, 0.f, s, s}, t[4] = {10.f, 20.f, 30.f, 1.f}, c[4] = {1.f, 0.f, 0.f, 0.f}, e[4] = {3.f, 4.f, 0.f, 0.f};
    std::memcpy(pose.data() + 0x10, q, 16);
    std::memcpy(pose.data() + 0x20, t, 16);
    std::memcpy(pose.data() + 0x80, c, 16);
    std::memcpy(pose.data() + 0x90, e, 16);
    std::vector<uint8_t> entries(2 * 0x58, 0);
    std::vector<float> subs(3 * 8);
    for (int k = 0; k < 3; ++k) {
        const float sub[8] = {0.f, 0.f, 0.f, 1.f, float(k), 2.f * k, 3.f * k, 1.f};
        std::memcpy(&subs[k * 8], sub, 32);
    }
    // The entries' models: the local sphere (+0x00 centre, +0x10 radius) and
    // the +0x40 cell the builder copies into its frame (local_448).
    std::vector<uint8_t> model0(0x50, 0), model1(0x50, 0);
    const float mc0[4] = {0.5f, 0.25f, 0.125f, 1.f}, ms0[4] = {4.5f, 0.5f, 0.f, 0.f};
    const float mc1[4] = {0.f, 0.f, 0.f, 1.f}, ms1[4] = {9.f, 0.f, 0.f, 0.f};
    std::memcpy(model0.data(), mc0, 16);
    std::memcpy(model0.data() + 0x10, ms0, 16);
    put<uint64_t>(model0, 0x40, 0x4E54ull);
    std::memcpy(model1.data(), mc1, 16);
    std::memcpy(model1.data() + 0x10, ms1, 16);
    put<uint64_t>(model1, 0x40, 0x4E55ull);
    // The entries' LOD tables (+0x08, version 3): entry 0's asset table A
    // (t0 0.5, t1 0.1, t2 0.3, count 2), which the builder copies into its
    // frame; table B is swapped in before P9 while the copy keeps A's bytes.
    std::vector<uint8_t> assetA(0x80, 0), assetB(0x80, 0);
    const float tA[3] = {0.5f, 0.1f, 0.3f}, tB[4] = {0.9f, 0.2f, 0.4f, 0.6f};
    for (int i = 0; i < 3; ++i) put(assetA, size_t(0x10 * i), tA[i]);
    put<uint32_t>(assetA, 0x70, 2);
    for (int i = 0; i < 4; ++i) put(assetB, size_t(0x10 * i), tB[i]);
    put<uint32_t>(assetB, 0x70, 3);
    put<uint64_t>(entries, 0x08, addr(assetA));
    put<uint64_t>(entries, 0x00, addr(model0));
    put<uint64_t>(entries, 0x20, 3);
    put<uint64_t>(entries, 0x28, reinterpret_cast<uint64_t>(subs.data()));
    put<uint64_t>(entries, 0x58 + 0x00, addr(model1));
    put<uint64_t>(entries, 0x58 + 0x20, 0);
    put<uint64_t>(pose, 0x48, 2);
    put<uint64_t>(pose, 0x50, addr(entries));
    // The record (stride 0x2F0 in its collection).
    std::vector<uint8_t> record(0x2F0, 0);
    put<uint64_t>(record, 0x18, 0x5150ull);
    const float pos[3] = {100.f, 200.f, 300.f};
    std::memcpy(record.data() + 0x170, pos, 12);
    const uint8_t quat[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::memcpy(record.data() + 0x17C, quat, 8);
    put<uint64_t>(record, 0x208, 0b101ull);
    put<uint64_t>(record, 0x210, 0x21ull);
    const float wc[4] = {101.f, 202.f, 303.f, 1.f}, lc[4] = {1.f, 2.f, 3.f, 0.f}, rad[4] = {7.5f, 0.f, 0.f, 0.f};
    std::memcpy(record.data() + 0x240, wc, 16);
    std::memcpy(record.data() + 0x270, lc, 16);
    std::memcpy(record.data() + 0x280, rad, 16);
    put<uint64_t>(record, 0x290, addr(pose));
    // The traversal's gate context: [0] -> a block whose [0] is the render
    // context, [2] the record (decomp_4312040.txt:287-304).
    std::vector<uint8_t> outer(16, 0);
    put<uint64_t>(outer, 0, addr(ctx));
    std::vector<uint8_t> gateCtx(0x60, 0);
    put<uint64_t>(gateCtx, 0x00, addr(outer));
    put<uint64_t>(gateCtx, 0x10, addr(record));
    std::vector<uint8_t> passOut(8, 0), rejectOut(8, 0);
    put<uint32_t>(passOut, 0, 2);
    put<uint8_t>(passOut, 4, 1);
    const uintptr_t view0 = addr(ctx) + 0x40, view1 = view0 + 0x6A0;
    // An address that faults: reserved, never committed.
    void* hole = VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
    // The builder's frame around its FUN_1442B3FC0 call, as its machine code
    // lays it out (cull_gate_probe.h kFrame*): param_1 is the builder's
    // rbp+0x70, six pointers, with the builder's own locals below it.
    std::vector<uint8_t> frameBlock(0x400, 0);
    const size_t itemsOff = 0x200;
    const uintptr_t items = addr(frameBlock) + itemsOff;
    auto builderFrame = [&](uintptr_t entry, const std::vector<uint8_t>& model, uintptr_t sub, uintptr_t view,
                            uintptr_t poseAt) {
        put<uint64_t>(frameBlock, itemsOff + 0x00, items - 0x20);   // [0] the world centre
        put<uint64_t>(frameBlock, itemsOff + 0x08, items - 0x10);   // [1] the model's +0x10 copy
        put<uint64_t>(frameBlock, itemsOff + 0x10, poseAt);         // [2] the pose context
        put<uint64_t>(frameBlock, itemsOff + 0x18, items - 0xC0);   // [3] -> *(model+0x40)
        put<uint64_t>(frameBlock, itemsOff + 0x20, items + 0xD0);   // [4] the LOD table copy
        put<uint64_t>(frameBlock, itemsOff + 0x28, addr(ctx));      // [5] the render context
        put<uint64_t>(frameBlock, itemsOff - 0x140, view);          // local_4c8: param_3
        put<uint64_t>(frameBlock, itemsOff - 0xD8, poseAt);         // rbp-0x68: the builder's param_1
        put<uint64_t>(frameBlock, itemsOff - 0xB8, addr(ctx));      // rbp-0x48: the render context
        put<uint64_t>(frameBlock, itemsOff - 0xC8, entry);          // rbp-0x58: the entry
        put<uint64_t>(frameBlock, itemsOff - 0xB0, addr(model));    // rbp-0x40: the model
        put<uint64_t>(frameBlock, itemsOff - 0xE0, sub);            // rbp-0x70: the sub-item
        uint64_t cell = 0;
        std::memcpy(&cell, model.data() + 0x40, 8);
        put<uint64_t>(frameBlock, itemsOff - 0xC0, cell);           // local_448
        const float centre[4] = {11.f, 22.f, 33.f, 1.f};
        std::memcpy(frameBlock.data() + itemsOff - 0x20, centre, 16);
        std::memcpy(frameBlock.data() + itemsOff - 0x10, model.data() + 0x10, 16);
        std::memcpy(frameBlock.data() + itemsOff + 0xD0, assetA.data(), 0x80);   // the builder's table copy
    };
    const uintptr_t entry0 = addr(entries);
    const uintptr_t sub0 = reinterpret_cast<uintptr_t>(subs.data());

    probe.configure(true);
    check(probe.enabled() && !probe.armed(), "configured on, not armed");
    probe.noteGate(addr(gateCtx), addr(passOut), view0);
    check(probe.counts().gateCalls == 0, "an unarmed probe records nothing");
    check(probe.arm(100, &fakeFrustum, 1) && probe.armed(), "arms for ledger frames 100..102");
    check(probe.firstFrame() == 100 && probe.lastFrame() == 102, "a three-frame window");
    probe.setPartHooked(true);
    probe.setFrame(99);
    probe.noteGate(addr(gateCtx), addr(passOut), view0);
    check(probe.counts().gateCalls == 0, "frames before the window are ignored");
    probe.setFrame(100);
    probe.noteGate(addr(gateCtx), addr(passOut), view0);
    probe.noteGate(addr(gateCtx), addr(rejectOut), view1);
    probe.noteGate(addr(gateCtx), addr(passOut), view0 + 8);   // not a view of this context
    probe.noteGate(reinterpret_cast<uintptr_t>(hole), addr(passOut), view0);   // a wild gate context
    const uint32_t row0 = probe.noteBuilder(addr(pose), addr(ctx), 0b111ull, addr(record) + 0x210);
    const uint32_t row1 = probe.noteBuilder(addr(pose) + 8, addr(ctx), 0b001ull, addr(record) + 0x210);   // pose disagrees with rec+0x290
    check(row0 == 0 && row1 == 1, "the builder observer returns the rows it kept");
    // FUN_1442B3FC0's calls inside builder row 0 (entry 0, sub-items 0..2).
    builderFrame(entry0, model0, sub0 + 32, view0, addr(pose));
    probe.notePart(items, addr(passOut), view0, row0, true);          // P1: verified, passed in view 0
    builderFrame(entry0, model0, sub0 + 64, view1, addr(pose));
    probe.notePart(items, addr(rejectOut), view1, row0, true);        // P2: verified, rejected in view 1
    builderFrame(entry0, model0, sub0, view0, addr(pose));
    put<uint64_t>(frameBlock, itemsOff + 0x18, items - 0xB8);         // param_1[3] off by 8: not the builder's frame
    probe.notePart(items, addr(passOut), view0, row0, true);          // P3: verdict kept, identity unverified
    builderFrame(entry0, model0, sub0, view0, addr(pose));
    probe.notePart(items, addr(passOut), view0, row0, false);         // P4: another caller
    probe.notePart(items, addr(passOut), view0, row1, true);          // P5: row 1's pose is not param_1[2]
    probe.notePart(items, addr(passOut), view0, CullGateProbe::kNoRow, true);   // P6: no builder row
    probe.notePart(reinterpret_cast<uintptr_t>(hole), addr(passOut), view0, row0, true);   // P7: wild: a fault, no row
    builderFrame(entry0, model0, sub0 + 96, view0, addr(pose));
    probe.notePart(items, addr(passOut), view0, row0, true);          // P8: one past the entry's sub-items
    probe.setFrame(101);
    probe.noteGate(addr(gateCtx), addr(passOut), view0);
    probe.setFrame(103);
    probe.noteGate(addr(gateCtx), addr(passOut), view0);
    check(probe.noteBuilder(addr(pose), addr(ctx), 0b111ull, addr(record) + 0x210) == CullGateProbe::kNoRow,
          "a builder call outside the window returns no row");
    builderFrame(entry0, model0, sub0, view0, addr(pose));
    put<uint64_t>(entries, 0x08, addr(assetB));   // a second table pointer; the copy still holds A's bytes
    probe.notePart(items, addr(passOut), view0, row0, true);          // P9: kept with its builder row's frame
    probe.notePart(items, addr(passOut), view0, CullGateProbe::kNoRow, true);   // P10: outside the window, no row
    const CullGateProbe::Counts n = probe.counts();
    check(n.gateCalls == 5 && n.gateKept == 4 && n.gateDropped == 0, "gate calls in the window kept, a fault not reserved");
    check(n.builderCalls == 2 && n.builderKept == 2, "builder calls in the window kept");
    check(n.faults >= 2 && n.recordMismatch == 1, "the wild gate context and part block count faults, the pose mismatch counts");
    check(n.dumps == 2 && n.dumpsDropped == 0, "one view-array dump per (context, frame)");
    check(probe.distinctRecords() == 1, "one distinct engine record");
    check(n.partCalls == 9 && n.partKept == 8 && n.partDropped == 0,
          "part tests in the window or a kept builder row recorded, a fault not reserved, none outside");
    check(n.partUnverified == 3 && n.partForeign == 1 && n.partUnlinked == 1,
          "an unverified frame, a foreign caller and a missing builder row are each counted");
    check(n.tablesKept == 2 && n.tablesDropped == 0,
          "version 3: two LOD table pointers, each recorded once however many parts name it");
    probe.disarm();
    probe.setFrame(101);
    probe.noteGate(addr(gateCtx), addr(passOut), view0);
    probe.notePart(items, addr(passOut), view0, row0, true);
    check(probe.counts().gateCalls == 5 && probe.counts().partCalls == 9, "a disarmed probe records nothing");
    check(probe.write(std::wstring(out.begin(), out.end()).c_str()), "gate file written");
    VirtualFree(hole, 0, MEM_RELEASE);
    std::printf("cull_gate_probe_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
